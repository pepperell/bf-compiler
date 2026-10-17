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

struct RegisterInfo {
    uint32_t    addr;        // cell index inside the device window
    const char* name;
    const char* access;      // "r/w", "r", "w"
    const char* desc;
};

inline const std::vector<RegisterInfo>& register_map() {
    static const std::vector<RegisterInfo> regs = {
        {A_LEDS,       "LEDS",       "r/w", "8 lamps, bit 7 is leftmost"},
        {A_KEY_STATUS, "KEY_STATUS", "r/w", "reads 1 when a key waits; a write pops the next key into KEY_DATA"},
        {A_KEY_DATA,   "KEY_DATA",   "r/w", "latch holding the last popped key"},
        {A_CONOUT,     "CONOUT",     "w",   "writes a byte to the console, same as '.'"},
        {A_TICKS + 3,  "TICKS0",     "r/w", "milliseconds since run start, low byte (loaded by TICK_LATCH)"},
        {A_TICKS + 2,  "TICKS1",     "r/w", "milliseconds, second byte (256 ms units)"},
        {A_TICKS + 1,  "TICKS2",     "r/w", "milliseconds, third byte"},
        {A_TICKS + 0,  "TICKS3",     "r/w", "milliseconds, high byte"},
        {A_NUMBER + 1, "NUMBER0",    "r/w", "16-bit numeric display, low byte"},
        {A_NUMBER + 0, "NUMBER1",    "r/w", "16-bit numeric display, high byte"},
        {A_HALT,       "HALT",       "w",   "any write ends the program with status \"halted\""},
        {A_RANDOM,     "RANDOM",     "r/w", "latch holding the last random byte (loaded by ROLL)"},
        {A_TICK_LATCH, "TICK_LATCH", "w",   "writing loads TICKS0..3 with the current time"},
        {A_ROLL,       "ROLL",       "w",   "writing loads RANDOM with a new random byte"},
        {A_SER_STATUS, "SER_STATUS", "r/w", "reads 1 while a received byte waits; a write pops it into SER_DATA"},
        {A_SER_DATA,   "SER_DATA",   "r/w", "latch: the byte to send, or the byte last popped"},
        {A_SER_TX,     "SER_TX",     "w",   "transmit SER_DATA"},
        {A_SER_CTRL,   "SER_CTRL",   "r/w", "write 0 close, 1 open the port; reads bit0 open, bit7 error"},
        {A_SER_BAUD,   "SER_BAUD",   "r/w", "0:300 1:1200 2:2400 3:9600 4:19200 5:38400 6:57600 7:115200"},
        {A_FB_X,       "FB_X",       "r/w", "framebuffer cursor column 0..127"},
        {A_FB_Y,       "FB_Y",       "r/w", "framebuffer cursor row 0..63"},
        {A_FB_COLOR,   "FB_COLOR",   "r/w", "colour latch, RRRGGGBB"},
        {A_FB_PLOT,    "FB_PLOT",    "w",   "plot FB_COLOR at the cursor, then advance (wraps to the next row)"},
        {A_FB_FILL,    "FB_FILL",    "w",   "fill the whole screen with the written value"},
        {A_FB_READ,    "FB_READ",    "w",   "load FB_COLOR with the pixel under the cursor"},
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
