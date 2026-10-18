// terminal.h - VT100/ANSI terminal emulator as a Win32 child window.
//
// Supports: CUP/HVP, CUU/CUD/CUF/CUB, CHA, VPA, CNL/CPL, ED, EL, SGR (bold,
// underline, reverse, 16 colours, 256-colour indexes), DECSTBM scroll regions,
// IND/RI/NEL, DECSC/DECRC, CSI s/u, ICH/DCH/IL/DL/ECH, DECTCEM cursor show/hide,
// BS/HT/LF/CR/BEL, LNM (CSI 20 h/l: newline mode), and a scrollback of lines
// pushed off the top.  In newline mode LF, VT and FF also return to column 1,
// which is what programs written for Unix terminals (LF-only output) expect.
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
    bool newline_mode = true;          // LF implies CR (see above)
};

HWND create(HWND parent, int id, const Config& cfg);
void feed(HWND h, const char* bytes, size_t n);      // interpret output bytes
void clear(HWND h);                                   // reset screen, scrollback and attributes
void set_cursor_visible(HWND h, bool visible);        // host override (e.g. program stopped)
// Host setting for newline mode.  A program may change the mode with CSI 20 h / CSI 20 l;
// clear() and ESC c return it to the host setting.
void set_newline_mode(HWND h, bool on);
bool newline_mode(HWND h);
// Fixed column count (0: as many as fit the window).  With a fixed count wider
// than the window the right part is cut off; narrower leaves a shaded margin.
void set_columns(HWND h, int cols);
int  columns_setting(HWND h);
void get_size(HWND h, int& cols, int& rows);
void get_cell_size(HWND h, int& cw, int& ch);         // pixel size of one character cell
// The default background and text colours (palette entries 0 and 7, black and grey
// unless set), so the terminal can follow the window's theme.
void set_default_colours(HWND h, COLORREF bg, COLORREF fg);

} // namespace term
