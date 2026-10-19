// registers.h - the single source of truth for the device window layout.
//
// Everything that documents the memory map is generated from this table: the
// help window, `bf --regs` (Markdown), and the README table, which the test
// suite checks against `bf --regs`.  Add a register here when you map one.
#pragma once
#include "devices.h"
#include "hw.h"
#include <string>
#include <vector>

namespace bf {

// How a register behaves under brainfuck's read-modify-write cells.  The BFL
// compiler uses this to decide what code may touch a register:
//   LATCH    reads and writes are free of side effects: copy loops work
//   STATUS   readable, but every write has a side effect (pop, open/close):
//            read only by testing, write only with pulse / += 1 / -= 1
//   TRIGGER  write-only: reads give 0, any write fires; use pulse
//   SWITCH   holds a value, but changing it has an effect: read only by
//            testing, write with a constant (= 0, = 1, += 1, -= 1, pulse)
enum RegKind : uint8_t { REG_LATCH, REG_STATUS, REG_TRIGGER, REG_SWITCH };

struct RegisterInfo {
    uint32_t    addr;        // cell index inside the device window
    const char* name;
    const char* access;      // "r/w", "r", "w"
    const char* desc;
    RegKind     kind;
};

inline const std::vector<RegisterInfo>& register_map() {
    static const std::vector<RegisterInfo> regs = {
        {A_LEDS,       "LEDS",       "r/w", "8 lamps, bit 7 is leftmost", REG_LATCH},
        {A_KEY_STATUS, "KEY_STATUS", "r/w", "reads 1 when a key waits; a write pops the next key into KEY_DATA", REG_STATUS},
        {A_KEY_DATA,   "KEY_DATA",   "r/w", "latch holding the last popped key", REG_LATCH},
        {A_CONOUT,     "CONOUT",     "w",   "writes a byte to the console, same as '.'", REG_TRIGGER},
        {A_TICKS + 3,  "TICKS0",     "r/w", "milliseconds since run start, low byte (loaded by TICK_LATCH)", REG_LATCH},
        {A_TICKS + 2,  "TICKS1",     "r/w", "milliseconds, second byte (256 ms units)", REG_LATCH},
        {A_TICKS + 1,  "TICKS2",     "r/w", "milliseconds, third byte", REG_LATCH},
        {A_TICKS + 0,  "TICKS3",     "r/w", "milliseconds, high byte", REG_LATCH},
        {A_NUMBER + 1, "NUMBER0",    "r/w", "16-bit numeric display, low byte", REG_LATCH},
        {A_NUMBER + 0, "NUMBER1",    "r/w", "16-bit numeric display, high byte", REG_LATCH},
        {A_HALT,       "HALT",       "w",   "any write ends the program with status \"halted\"", REG_TRIGGER},
        {A_RANDOM,     "RANDOM",     "r/w", "latch holding the last random byte (loaded by ROLL)", REG_LATCH},
        {A_TICK_LATCH, "TICK_LATCH", "w",   "writing loads TICKS0..3 with the current time", REG_TRIGGER},
        {A_ROLL,       "ROLL",       "w",   "writing loads RANDOM with a new random byte", REG_TRIGGER},
        {A_SER_STATUS, "SER_STATUS", "r/w", "reads 1 while a received byte waits; a write pops it into SER_DATA", REG_STATUS},
        {A_SER_DATA,   "SER_DATA",   "r/w", "latch: the byte to send, or the byte last popped", REG_LATCH},
        {A_SER_TX,     "SER_TX",     "w",   "transmit SER_DATA", REG_TRIGGER},
        {A_SER_CTRL,   "SER_CTRL",   "r/w", "write 0 close, 1 open the port; reads bit0 open, bit7 error", REG_STATUS},
        {A_SER_BAUD,   "SER_BAUD",   "r/w", "0:300 1:1200 2:2400 3:9600 4:19200 5:38400 6:57600 7:115200", REG_LATCH},
        {A_FB_X,       "FB_X",       "r/w", "framebuffer cursor column 0..127", REG_LATCH},
        {A_FB_Y,       "FB_Y",       "r/w", "framebuffer cursor row 0..63", REG_LATCH},
        {A_FB_COLOR,   "FB_COLOR",   "r/w", "colour latch, RRRGGGBB", REG_LATCH},
        {A_FB_PLOT,    "FB_PLOT",    "w",   "plot FB_COLOR at the cursor, then advance (wraps to the next row)", REG_TRIGGER},
        {A_FB_FILL,    "FB_FILL",    "w",   "fill the whole screen with the written value", REG_TRIGGER},
        {A_FB_READ,    "FB_READ",    "w",   "load FB_COLOR with the pixel under the cursor", REG_TRIGGER},
        {A_FB_BUFFER,  "FB_BUFFER",  "r/w", "1: draw into a back buffer; 1 to 0 shows it; 0: draw straight to the screen", REG_SWITCH},
    };
    return regs;
}

// Markdown table of the map, as printed by `bf --regs` and embedded in README.md.
inline std::string register_map_markdown(uint32_t start_cell = 256) {
    std::string s = "| Cell | From start | Register | Access | Meaning |\n|------|-----------|----------|--------|---------|\n";
    char line[512];
    for (const RegisterInfo& r : register_map()) {
        snprintf(line, sizeof line, "| 0x%02X | <%u | %s | %s | %s |\n", r.addr, start_cell - r.addr, r.name, r.access, r.desc);
        s += line;
    }
    return s;
}

// Plain-text table for the help window and the console.
inline std::string register_map_text(uint32_t start_cell = 256) {
    std::string s = "Cell  From   Register    Access  Meaning\n";
    char line[512];
    for (const RegisterInfo& r : register_map()) {
        snprintf(line, sizeof line, "0x%02X  <%-4u  %-11s %-6s  %s\n", r.addr, start_cell - r.addr, r.name, r.access, r.desc);
        s += line;
    }
    return s;
}

} // namespace bf
