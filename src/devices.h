// devices.h - standard memory-mapped devices shared by the console runner and the UI.
//
// The device window is the 256 cells immediately left of the starting cell, so
// a program reaches a register with a handful of '<'.  Addresses below are cell
// indices; "<n" is the number of '<' from the start cell.
//
// Readable registers are latches: they read back what was last written, so the
// usual destructive-copy idiom ([>+<-]) works on them and optimised code sees
// memory-like behaviour.  Writing to a trigger register loads a latch.
//
//   0xFF  <1   LEDS        r/w   8 lamps, bit 7 = leftmost
//   0xFE  <2   KEY_STATUS  r/w   reads 1 when a key is waiting; writing pops the
//                                next key into KEY_DATA (0 when none)
//   0xFD  <3   KEY_DATA    r/w   latch holding the last popped key
//   0xFC  <4   CONOUT      w     writes a byte to the console, same as '.'
//   0xFB  <5   TICKS0      r/w   latch: milliseconds since reset, low byte
//   0xFA  <6   TICKS1      r/w
//   0xF9  <7   TICKS2      r/w
//   0xF8  <8   TICKS3      r/w   high byte
//   0xF7  <9   NUMBER0     r/w   16-bit display, low byte
//   0xF6  <10  NUMBER1     r/w   high byte
//   0xF5  <11  HALT        w     any write ends the program with status "halted"
//   0xF4  <12  RANDOM      r/w   latch holding the last random byte
//   0xF3  <13  TICK_LATCH  w     writing loads TICKS0..3 with the current time
//   0xF2  <14  ROLL        w     writing loads RANDOM with a new random byte
#pragma once
#include "bf.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace bf {

constexpr uint32_t A_LEDS       = 0xFF;
constexpr uint32_t A_KEY_STATUS = 0xFE;
constexpr uint32_t A_KEY_DATA   = 0xFD;
constexpr uint32_t A_CONOUT     = 0xFC;
constexpr uint32_t A_TICKS      = 0xF8;   // 4 bytes, 0xFB is the low byte
constexpr uint32_t A_NUMBER     = 0xF6;   // 2 bytes, 0xF7 is the low byte
constexpr uint32_t A_HALT       = 0xF5;
constexpr uint32_t A_RANDOM     = 0xF4;
constexpr uint32_t A_TICK_LATCH = 0xF3;
constexpr uint32_t A_ROLL       = 0xF2;

struct KeyQueue {
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<uint8_t>     q;
    bool                    cancelled = false;

    void push(uint8_t k) { { std::lock_guard<std::mutex> g(mu); q.push_back(k); } cv.notify_all(); }
    bool available()     { std::lock_guard<std::mutex> g(mu); return !q.empty(); }
    int  try_pop()       { std::lock_guard<std::mutex> g(mu); if (q.empty()) return -1; int k = q.front(); q.pop_front(); return k; }
    int  wait_pop() {                       // blocks; -1 once cancelled
        std::unique_lock<std::mutex> g(mu);
        cv.wait(g, [&] { return !q.empty() || cancelled; });
        if (q.empty()) return -1;
        int k = q.front(); q.pop_front(); return k;
    }
    void cancel()  { { std::lock_guard<std::mutex> g(mu); cancelled = true; } cv.notify_all(); }
    void reset()   { std::lock_guard<std::mutex> g(mu); cancelled = false; q.clear(); }
};

struct LedDevice : Device {
    std::atomic<uint8_t> value{0};
    std::function<void(uint8_t)> on_change;
    uint8_t read(uint32_t) override { return value; }
    void write(uint32_t, uint8_t v) override { value = v; if (on_change) on_change(v); }
};

struct KeyboardDevice : Device {          // mapped at A_KEY_DATA, size 2: rel 0 = data latch, rel 1 = status
    KeyQueue* keys = nullptr;
    uint8_t   data = 0;
    uint8_t read(uint32_t rel) override {
        if (rel == 1) return (keys && keys->available()) ? 1 : 0;
        return data;
    }
    void write(uint32_t rel, uint8_t v) override {
        if (rel == 1) { int k = keys ? keys->try_pop() : -1; data = k < 0 ? 0 : (uint8_t)k; }
        else data = v;
    }
};

struct ConsoleOutDevice : Device {
    ConsoleIo* io = nullptr;
    void write(uint32_t, uint8_t v) override { if (io) io->out(v); }
};

struct TicksDevice : Device {             // mapped at A_TICKS, size 4 (rel 3 = low byte) and at A_TICK_LATCH
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    uint8_t latch[4] = {0, 0, 0, 0};
    void reset() { t0 = std::chrono::steady_clock::now(); for (auto& b : latch) b = 0; }
    uint8_t read(uint32_t rel) override { return rel < 4 ? latch[rel] : 0; }
    void write(uint32_t rel, uint8_t v) override {
        if (rel < 4) { latch[rel] = v; return; }
        auto ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < 4; i++) latch[i] = (uint8_t)(ms >> (8 * (3 - i)));
    }
};

struct TriggerDevice : Device {           // forwards a write to another device with a fixed rel
    Device* target = nullptr; uint32_t rel = 0;
    void write(uint32_t, uint8_t v) override { if (target) target->write(rel, v); }
};

struct NumberDevice : Device {            // mapped at A_NUMBER, size 2; rel 1 = low byte
    std::atomic<uint16_t> value{0};
    std::function<void(uint16_t)> on_change;
    uint8_t read(uint32_t rel) override { return (uint8_t)(value >> (rel == 1 ? 0 : 8)); }
    void write(uint32_t rel, uint8_t v) override {
        uint16_t cur = value;
        cur = rel == 1 ? (uint16_t)((cur & 0xFF00) | v) : (uint16_t)((cur & 0x00FF) | (v << 8));
        value = cur;
        if (on_change) on_change(cur);
    }
};

struct HaltDevice : Device {
    Machine* m = nullptr;
    void write(uint32_t, uint8_t) override { if (m) m->halt(); }
};

struct RandomDevice : Device {            // rel 0 = latch (A_RANDOM); rel 1 = roll trigger (A_ROLL)
    uint32_t s = 0x9E3779B9u;
    uint8_t  latch = 0;
    uint8_t read(uint32_t) override { return latch; }
    void write(uint32_t rel, uint8_t v) override {
        if (rel == 0) { latch = v; return; }
        s ^= s << 13; s ^= s >> 17; s ^= s << 5; latch = (uint8_t)s;
    }
};

// Everything above, wired to one machine.
struct StdDevices {
    KeyQueue         keys;
    LedDevice        leds;
    KeyboardDevice   keyboard;
    ConsoleOutDevice conout;
    TicksDevice      ticks;
    NumberDevice     number;
    HaltDevice       halt;
    RandomDevice     random;
    TriggerDevice    tick_latch, roll;

    void install(Machine& m, ConsoleIo* io) {
        keyboard.keys = &keys;
        conout.io = io;
        halt.m = &m;
        tick_latch.target = &ticks;  tick_latch.rel = 4;
        roll.target = &random;       roll.rel = 1;
        m.map(A_LEDS, 1, &leds);
        m.map(A_KEY_DATA, 2, &keyboard);
        m.map(A_CONOUT, 1, &conout);
        m.map(A_TICKS, 4, &ticks);
        m.map(A_NUMBER, 2, &number);
        m.map(A_HALT, 1, &halt);
        m.map(A_RANDOM, 1, &random);
        m.map(A_TICK_LATCH, 1, &tick_latch);
        m.map(A_ROLL, 1, &roll);
    }
    void reset() { keys.reset(); leds.value = 0; number.value = 0; ticks.reset(); keyboard.data = 0; random.latch = 0; }
};

} // namespace bf
