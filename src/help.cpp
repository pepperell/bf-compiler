// help.cpp - help window contents and implementation.
#include "help.h"
#include "registers.h"
#include <commctrl.h>

namespace help {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Edit controls want CRLF.
std::wstring crlf(std::wstring s) {
    std::wstring o; o.reserve(s.size() + 64);
    for (wchar_t c : s) { if (c == L'\n') o += L"\r\n"; else o += c; }
    return o;
}

const wchar_t* START_TEXT = LR"(BRAINFUCK MACHINE

A fast brainfuck executor with memory-mapped hardware.  The editor holds the
program, the terminal shows its output and takes keyboard input, the device
panel shows the lamps and the number display, and the memory viewer shows the
tape.  Press F5 to run whatever is in the editor.

KEYS
  F5          run the editor contents (or continue when paused in the debugger)
  Shift+F5    stop
  Ctrl+F5     start debugging, paused on the first command
  F11         step one command            F10   step over a [ ] loop
  Ctrl+F10    run to the caret            F6    pause a running debug session
  F9          toggle a breakpoint on the command at the caret
  Ctrl+O      open    Ctrl+S save    Ctrl+R reload from disk
  Ctrl+L      clear the terminal
  Ctrl+G      jump to the memory viewer's address box
  Ctrl+F      show or hide the framebuffer window
  F1          this help

Dropping a .b file onto the window loads and runs it.  Drag the bar between
the editor and the terminal to resize them.

HOW A RUN WORKS
The source is compiled to an optimised intermediate form and then to x64
machine code, which runs on a worker thread.  Cells are 8-bit and wrap.  The
pointer starts at cell 0x100 (256); cells 0..0xFF are the device window (see
Memory map).  A pointer that leaves the tape ends the run with status "fault".
Run > Terminal speed paces output like a serial line so animations are
visible.  Run > Use interpreter runs the slower reference interpreter instead
of the JIT; the debugger always uses the interpreter.)";

const wchar_t* SYMBOLS_TEXT = LR"(BRAINFUCK SYMBOLS

The language has eight commands.  Every other character is a comment, so a
comment must not contain any of  + - < > . , [ ]  (write "minus", "to",
"ptr" and so on instead).

  >   move the pointer one cell to the right
  <   move the pointer one cell to the left
  +   add one to the cell under the pointer (255 wraps to 0)
  -   subtract one from the cell under the pointer (0 wraps to 255)
  .   output the cell under the pointer as a byte (to the terminal)
  ,   read one byte from the keyboard into the cell under the pointer;
      at end of input the cell is left unchanged (bf --eof changes this)
  [   if the cell under the pointer is 0, jump forward past the matching ]
  ]   if the cell under the pointer is not 0, jump back past the matching [

Brackets must balance; the program ends after the last command.

THIS MACHINE
  cells        8-bit, wrapping
  tape         256 device cells, then 1 MiB of RAM (bf --ram changes the size)
  start        the pointer begins at cell 0x100, the first RAM cell
  left of it   the device window: see Memory map
  , and .      go to the terminal; typed keys queue until the program reads them

IDIOMS
  [-]              set the cell to 0
  [>+<-]           move the cell into the cell to the right (destroys it)
  [>+>+<<-]>>[<<+>>-]<<
                   copy the cell to the right, keeping the original (uses a temp)
  [<]  [>]         scan left / right to the next zero cell
  ++++++++[>++++++++<-]>   build 64 in the next cell with a loop
  >+<[>-<[-]]>[<+>-]<      if the cell is zero, set it to 1 (uses the next cell)

TALKING TO HARDWARE
  Registers live just left of the start cell, so "<" a few times reaches them
  (the Memory map lists how many).  Readable registers are latches: they read
  back what was last written, so [>+<-] style copies work on them.  Actions
  with side effects (send a byte, plot a pixel, pop a key) are separate
  trigger registers: load the latch with + and - in a loop, then write the
  trigger once.  Example, send 72 ('H') out of the serial port:

    <<<<<<<<<<<<<<<<     SER_DATA is 16 left of the start
    ++++...(72 times)    load the latch
    <+                   SER_TX is one further left: any write transmits

  To read a value out of a register, move it with a loop: the register counts
  down as a RAM cell would.  To wait for something, test a status register
  with [ ]: it reads 1 while a byte or key is waiting and 0 otherwise.

OPTIMISATION AND DEVICES
  Loops such as [>+<-] are compiled into single multiply-add instructions.
  That never changes what a device sees: whenever the pointer can reach the
  device window the executor runs the literal program, one command at a time,
  so a trigger inside a loop fires once per iteration exactly as written.)";

const wchar_t* MEMORY_TEXT = LR"(MEMORY MAP

  cell 0x000 .. 0x0FF   device window (256 cells)
  cell 0x100            start of RAM; the pointer begins here
  cell 0x100 + RAM size end of the tape (1 MiB of RAM by default)

"From start" is the number of '<' commands from the start cell to the
register.  Readable registers are latches (they read back the last value
written); "w" registers are triggers whose side effect happens on any write.
Unmapped device cells read 0 and ignore writes.

)";

const wchar_t* DEVICES_TEXT = LR"(DEVICES

LAMPS AND NUMBER DISPLAY
  LEDS (<1) drives the eight lamps in the panel, bit 7 on the left.  NUMBER0
  and NUMBER1 (<9, <10) form a 16-bit value shown on the red display.

KEYBOARD
  Keys typed into the terminal queue up.  ',' takes the next one; or test
  KEY_STATUS (<2) which reads 1 while a key waits, write anything to it to pop
  the key into KEY_DATA (<3), then move KEY_DATA into RAM with a loop.

TIME AND RANDOM
  Write TICK_LATCH (<13) to load TICKS0..3 (<5..<8) with the milliseconds
  since the run started, low byte first.  programs\leds.b waits for TICKS1
  (256 ms units) to change.  Write ROLL (<14) to load RANDOM (<12).

HALT
  Any write to HALT (<11) ends the program with status "halted".

SERIAL PORT
  Choose the port under Devices > Serial port: a real COM port, Loopback
  (TX wired to RX, for testing without hardware) or None.  Registers:
    SER_STATUS (<15)  reads 1 while a received byte waits; a write pops it
    SER_DATA   (<16)  latch: byte to send, or the byte last popped
    SER_TX     (<17)  any write transmits SER_DATA
    SER_CTRL   (<18)  write 0 close / 1 open; reads bit0 open, bit7 error
    SER_BAUD   (<19)  0:300 1:1200 2:2400 3:9600 4:19200 5:38400 6:57600 7:115200
  The panel shows the port, speed and state.  programs\serial_echo.b sends
  "Hi" and prints what comes back.  Console: bf --serial COM3 (or loopback).

FRAMEBUFFER
  128 x 64 pixels, one byte per pixel, colour bits RRRGGGBB (0xE0 red,
  0x1C green, 0x03 blue, 0xFF white).  Shown in its own window (Ctrl+F, or
  automatically on the first plot).  Registers:
    FB_X (<20), FB_Y (<21)   cursor
    FB_COLOR (<22)           colour latch
    FB_PLOT (<23)            plot FB_COLOR at the cursor, then X advances and
                             wraps to the next row, so a stream of plots fills
                             the screen row by row
    FB_FILL (<24)            fill the screen with the written value
    FB_READ (<25)            load FB_COLOR with the pixel under the cursor
  programs\fb.b paints a gradient over the whole screen.

ADDING A DEVICE
  Derive from bf::Device (src\bf.h), override read(rel) and write(rel, v)
  where rel is the offset inside the mapped range, and map it with
  Machine::map(addr, size, &dev).  Add its registers to src\registers.h so
  this help, bf --regs and the README stay in step (the tests check that).)";

const wchar_t* DEBUGGER_TEXT = LR"(DEBUGGER

Ctrl+F5 (or the Debug button) starts the editor contents paused on the first
command.  The current command is highlighted in the editor, the status bar
shows why the program paused, its line and column and the value of the cell
under the pointer, and the memory viewer follows the pointer.

  F11        step one command
  F10        step over: a [ runs its whole loop and stops after the matching ]
  Ctrl+F10   run to the command at the caret
  F5         continue until a breakpoint, a data breakpoint or the end
  F6         pause a running session
  Shift+F5   stop the session
  F9         toggle a breakpoint on the command at the caret (or the next
             command on that line); breakpoints are shown in red and move
             with the text when you edit

WATCH LIST (under the memory viewer)
  "+ Address" watches the cell typed in the viewer's address box; "+ Pointer"
  watches the cell under the pointer.  Rows show the value live in hex,
  decimal and as a character.  Tick a row to make it a data breakpoint: the
  program pauses as soon as that cell is written, which works for device
  registers too.  "Break on entering device window" pauses when the pointer
  first moves left of the start cell.

Debug sessions use the interpreter with one instruction per source command,
so they are far slower than a normal run.  Use them for logic, not timing.)";

const wchar_t* TERMINAL_TEXT = LR"(TERMINAL

The terminal pane is a VT100 / ANSI emulator.  Output from '.' and CONOUT
goes there; keys typed into it feed ',' and the keyboard registers (arrows
and function keys arrive as VT sequences).  Click it to give it the focus.

It understands cursor positioning (ESC [ row ; col H), the relative moves,
erase display and line, insert and delete, scroll regions, SGR colours and
attributes, cursor show/hide, and keeps a scrollback for plain output (mouse
wheel, Shift+PgUp / PgDn).  It starts 25 rows tall so classic 80 x 24
programs such as programs\thirdparty\hanoi.b draw correctly.

Run > Terminal speed (also the toolbar box) paces output like a serial line.
Programs that finish in milliseconds at full speed become watchable at 9600.)";

const wchar_t* CLI_TEXT = LR"(COMMAND LINE: bf.exe

  bf [options] program.b
  bf [options] -e 'code'

  --interp         run the interpreter instead of the x64 JIT
  --no-opt         disable optimisation (one instruction per command)
  --dump           print the intermediate code and exit
  --regs           print the memory map as a Markdown table and exit
  --time           print run time and status to stderr
  --eof MODE       ',' at end of input: nochange (default), 0, 255
  --ram N          RAM cells (default 1048576)
  --mmio N         device window cells (default 256, 0 disables devices)
  --serial PORT    attach the serial device to COMn, or 'loopback'
  --quiet-devices  do not report LED / number display changes on stderr

Exit code 0 when the program ends or halts, 3 on stop, fault or error.
Ctrl+C stops a running program.  LED and number display changes are printed
to stderr unless --quiet-devices is given.

TESTS AND BUILD
  build.bat            release build to out\
  python tests\test.py runs every case through the JIT, the interpreter and
                       the unoptimised JIT and compares with a reference
                       interpreter; also checks README.md against bf --regs)";

std::vector<Entry> build() {
    std::vector<Entry> t(TOPIC_COUNT);
    t[START]        = {L"Getting started", crlf(START_TEXT)};
    t[SYMBOLS]      = {L"Brainfuck symbols", crlf(SYMBOLS_TEXT)};
    t[MEMORY_MAP]   = {L"Memory map", crlf(std::wstring(MEMORY_TEXT) + widen(bf::register_map_text()))};
    t[DEVICES]      = {L"Devices", crlf(DEVICES_TEXT)};
    t[DEBUGGER]     = {L"Debugger", crlf(DEBUGGER_TEXT)};
    t[TERMINAL]     = {L"Terminal", crlf(TERMINAL_TEXT)};
    t[COMMAND_LINE] = {L"Command line", crlf(CLI_TEXT)};
    return t;
}

HWND g_wnd = nullptr, g_list = nullptr, g_text = nullptr;
HFONT g_mono = nullptr, g_ui = nullptr;
UINT g_dpi = 96;
int px(int v) { return MulDiv(v, (int)g_dpi, 96); }

void select_topic(int i) {
    const auto& t = topics();
    if (i < 0 || i >= (int)t.size()) return;
    SendMessageW(g_list, LB_SETCURSEL, i, 0);
    SetWindowTextW(g_text, t[i].text.c_str());
    SetWindowTextW(g_wnd, (std::wstring(L"Help - ") + t[i].title).c_str());
}

void layout() {
    RECT rc; GetClientRect(g_wnd, &rc);
    int lw = px(170);
    MoveWindow(g_list, 0, 0, lw, rc.bottom, TRUE);
    MoveWindow(g_text, lw + px(4), 0, rc.right - lw - px(4), rc.bottom, TRUE);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_wnd = h;
        g_dpi = GetDpiForWindow(h);
        g_list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, 10, 10, h, (HMENU)1, nullptr, nullptr);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_ui, TRUE);
        SendMessageW(g_list, LB_SETITEMHEIGHT, 0, px(22));
        for (const Entry& e : topics()) SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)e.title);
        g_text = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 10, 10, h, (HMENU)2, nullptr, nullptr);
        SendMessageW(g_text, WM_SETFONT, (WPARAM)g_mono, TRUE);
        SendMessageW(g_text, EM_SETLIMITTEXT, 0, 0);
        return 0;
    case WM_SIZE: layout(); return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_text) { SetBkColor((HDC)w, RGB(255, 255, 255)); return (LRESULT)GetStockObject(WHITE_BRUSH); }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == 1 && HIWORD(w) == LBN_SELCHANGE) select_topic((int)SendMessageW(g_list, LB_GETCURSEL, 0, 0));
        return 0;
    case WM_KEYDOWN: if (w == VK_ESCAPE) ShowWindow(h, SW_HIDE); return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

const std::vector<Entry>& topics() {
    static const std::vector<Entry> t = build();
    return t;
}

void show(HWND owner, HINSTANCE inst, HFONT mono, HFONT ui, Topic topic) {
    g_mono = mono; g_ui = ui;
    if (!g_wnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"BfHelp";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        UINT dpi = GetDpiForWindow(owner);
        CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Help", WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(900, dpi, 96), MulDiv(640, dpi, 96), owner, nullptr, inst, nullptr);
    }
    select_topic(topic);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
}

} // namespace help
