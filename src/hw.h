// hw.h - serial port and framebuffer devices.
//
// Serial port (a real COM port, or loopback for testing).  Latches and triggers
// are separate so the usual '+' loops never cause a side effect by accident:
//   0xF1  <15  SER_STATUS  r/w   reads 1 while a received byte is waiting, else 0;
//                                writing pops the next received byte into SER_DATA
//   0xF0  <16  SER_DATA    r/w   latch: the byte to send or the byte last popped
//   0xEF  <17  SER_TX      w     transmit SER_DATA
//   0xEE  <18  SER_CTRL    r/w   write 0 close, 1 open; reads bit0 open, bit7 error
//   0xED  <19  SER_BAUD    r/w   0:300 1:1200 2:2400 3:9600 4:19200 5:38400 6:57600 7:115200
//
// Framebuffer, 128 x 64 pixels, one byte per pixel, colour = RRRGGGBB:
//   0xEC  <20  FB_X        r/w   cursor column 0..127
//   0xEB  <21  FB_Y        r/w   cursor row 0..63
//   0xEA  <22  FB_COLOR    r/w   latch
//   0xE9  <23  FB_PLOT     w     plot FB_COLOR at (FB_X, FB_Y), then advance X (wrapping to the next row)
//   0xE8  <24  FB_FILL     w     fill the whole screen with the written value
//   0xE7  <25  FB_READ     w     load FB_COLOR with the pixel at (FB_X, FB_Y)
#pragma once
#include "bf.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bf {

constexpr uint32_t A_SER_STATUS = 0xF1;
constexpr uint32_t A_SER_DATA   = 0xF0;
constexpr uint32_t A_SER_TX     = 0xEF;
constexpr uint32_t A_SER_CTRL   = 0xEE;
constexpr uint32_t A_SER_BAUD   = 0xED;
constexpr uint32_t A_FB_X       = 0xEC;
constexpr uint32_t A_FB_Y       = 0xEB;
constexpr uint32_t A_FB_COLOR   = 0xEA;
constexpr uint32_t A_FB_PLOT    = 0xE9;
constexpr uint32_t A_FB_FILL    = 0xE8;
constexpr uint32_t A_FB_READ    = 0xE7;

constexpr int SER_BAUDS[8] = {300, 1200, 2400, 9600, 19200, 38400, 57600, 115200};

// Mapped at A_SER_BAUD, size 5: rel 0 baud, 1 ctrl, 2 tx, 3 data, 4 status.
struct SerialDevice : Device {
    std::string port;                    // "COM3", "loopback" or "" (none)
    uint8_t baud_idx = 3;
    uint8_t data = 0;
    std::atomic<bool> error{false};
    std::function<void()> on_change;     // status changed (UI refresh)

    HANDLE h = INVALID_HANDLE_VALUE;
    bool loopback = false;
    std::thread reader;
    std::atomic<bool> quit{false};
    std::mutex mu;
    std::deque<uint8_t> rx;

    ~SerialDevice() override { close(); }

    bool is_open() const { return loopback || h != INVALID_HANDLE_VALUE; }

    void configure(const std::string& p) { close(); port = p; if (!port.empty()) open(); }

    bool open() {
        close();
        error = false;
        if (port.empty()) return false;
        if (port == "loopback") { loopback = true; notify(); return true; }
        std::wstring name = L"\\\\.\\" + std::wstring(port.begin(), port.end());
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { error = true; notify(); return false; }
        DCB dcb{}; dcb.DCBlength = sizeof dcb;
        GetCommState(h, &dcb);
        dcb.BaudRate = SER_BAUDS[baud_idx & 7];
        dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = TRUE; dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE; dcb.fOutX = FALSE; dcb.fInX = FALSE;
        if (!SetCommState(h, &dcb)) { error = true; CloseHandle(h); h = INVALID_HANDLE_VALUE; notify(); return false; }
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD; to.ReadTotalTimeoutMultiplier = MAXDWORD; to.ReadTotalTimeoutConstant = 50;  // 50 ms poll
        to.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(h, &to);
        PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
        quit = false;
        reader = std::thread([this] { read_loop(); });
        notify();
        return true;
    }

    void close() {
        quit = true;
        if (reader.joinable()) reader.join();
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
        loopback = false;
        { std::lock_guard<std::mutex> g(mu); rx.clear(); }
        notify();
    }

    void read_loop() {
        uint8_t buf[256];
        while (!quit) {
            DWORD n = 0;
            if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) { error = true; notify(); Sleep(50); continue; }
            if (n) { std::lock_guard<std::mutex> g(mu); for (DWORD i = 0; i < n; i++) rx.push_back(buf[i]); }
        }
    }

    void transmit(uint8_t v) {
        if (loopback) { std::lock_guard<std::mutex> g(mu); rx.push_back(v); return; }
        if (h == INVALID_HANDLE_VALUE) { error = true; return; }
        DWORD n = 0;
        if (!WriteFile(h, &v, 1, &n, nullptr) || n != 1) error = true;
    }

    bool waiting() { std::lock_guard<std::mutex> g(mu); return !rx.empty(); }
    int pop() { std::lock_guard<std::mutex> g(mu); if (rx.empty()) return -1; int v = rx.front(); rx.pop_front(); return v; }
    void notify() { if (on_change) on_change(); }

    uint8_t read(uint32_t rel) override {
        switch (rel) {
        case 0: return baud_idx;
        case 1: return (uint8_t)((is_open() ? 1 : 0) | (error ? 0x80 : 0));
        case 3: return data;
        case 4: return waiting() ? 1 : 0;
        default: return 0;
        }
    }
    void write(uint32_t rel, uint8_t v) override {
        switch (rel) {
        case 0: baud_idx = v & 7; if (h != INVALID_HANDLE_VALUE) open(); break;
        case 1: if (v) open(); else close(); break;
        case 2: transmit(data); break;
        case 3: data = v; break;
        case 4: { int k = pop(); data = k < 0 ? 0 : (uint8_t)k; break; }
        }
    }
};

// Mapped at A_FB_READ, size 6: rel 0 read-trigger, 1 fill, 2 plot, 3 colour, 4 y, 5 x.
struct FramebufferDevice : Device {
    static constexpr int W = 128, H = 64;
    uint8_t pixels[W * H] = {};
    uint8_t x = 0, y = 0, color = 0;
    std::atomic<bool> dirty{false};
    std::function<void()> on_first_plot;   // UI: show the window
    bool plotted = false;

    void clear() { memset(pixels, 0, sizeof pixels); x = y = color = 0; dirty = true; plotted = false; }
    void touch() { dirty = true; if (!plotted) { plotted = true; if (on_first_plot) on_first_plot(); } }

    static COLORREF colour(uint8_t v) {            // RRRGGGBB
        int r = (v >> 5) * 255 / 7, g = ((v >> 2) & 7) * 255 / 7, b = (v & 3) * 255 / 3;
        return RGB(r, g, b);
    }

    uint8_t read(uint32_t rel) override {
        switch (rel) { case 3: return color; case 4: return y; case 5: return x; default: return 0; }
    }
    void write(uint32_t rel, uint8_t v) override {
        switch (rel) {
        case 0: color = pixels[(y % H) * W + (x % W)]; break;
        case 1: memset(pixels, v, sizeof pixels); touch(); break;
        case 2:
            pixels[(y % H) * W + (x % W)] = color;
            if (++x >= W) { x = 0; if (++y >= H) y = 0; }
            touch();
            break;
        case 3: color = v; break;
        case 4: y = v % H; break;
        case 5: x = v % W; break;
        }
    }
};

inline void install_hw(Machine& m, SerialDevice& ser, FramebufferDevice& fb) {
    m.map(A_SER_BAUD, 5, &ser);
    m.map(A_FB_READ, 6, &fb);
}

// COM ports present on this machine ("COM1", ...).
inline std::vector<std::string> list_com_ports() {
    std::vector<std::string> out;
    wchar_t target[512];
    for (int i = 1; i <= 255; i++) {
        std::wstring n = L"COM" + std::to_wstring(i);
        if (QueryDosDeviceW(n.c_str(), target, 512)) out.push_back("COM" + std::to_string(i));
    }
    return out;
}

} // namespace bf
