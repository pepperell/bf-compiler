// terminal.h - VT100/ANSI terminal emulator as a Win32 child window.
//
// Supports: CUP/HVP, CUU/CUD/CUF/CUB, CHA, VPA, CNL/CPL, ED, EL, SGR (bold,
// underline, reverse, 16 colours, 256-colour indexes), DECSTBM scroll regions,
// IND/RI/NEL, DECSC/DECRC, CSI s/u, ICH/DCH/IL/DL/ECH, DECTCEM cursor show/hide,
// BS/HT/LF/CR/BEL, and a scrollback of lines pushed off the top.
// Bytes are shown as Latin-1.  Keys typed into the window are delivered through
// the on_key callback as bytes (arrows and function keys as VT sequences).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace term {

struct Config {
    HFONT font = nullptr;
    std::function<void(uint8_t)> on_key;
};

HWND create(HWND parent, int id, const Config& cfg);
void feed(HWND h, const char* bytes, size_t n);      // interpret output bytes
void clear(HWND h);                                   // reset screen, scrollback and attributes
void set_cursor_visible(HWND h, bool visible);        // host override (e.g. program stopped)
void get_size(HWND h, int& cols, int& rows);
void get_cell_size(HWND h, int& cw, int& ch);         // pixel size of one character cell

} // namespace term
