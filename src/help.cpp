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
  Ctrl+P      profile: run on the interpreter, then show where the
              instructions went (see "Debugger"); typed in the terminal
              while a program runs, Ctrl+P goes to the program
  F11         step one command            F10   step over a [ ] loop
  Ctrl+F10    run to the caret            F6    pause a running debug session
  F9          toggle a breakpoint on the command at the caret
  Ctrl+O      open    Ctrl+S save    Ctrl+R reload from disk
  Ctrl+L      clear the terminal
  Ctrl+G      jump to the memory viewer's address box
  Ctrl+M      show or hide the memory viewer and watch list
  Ctrl+F      show or hide the framebuffer window
  Ctrl+B      show the brainfuck generated from a BFL program
  F4          next BFL error or warning (Shift+F4: previous)
  F1          this help

Dropping a .b or .bfl file onto the window loads and runs it.  A .bfl file
is written in BFL, a small language that compiles to brainfuck (see the
"BFL language" topic).  Drag the bar between
the editor and the terminal to resize them.

VIEW AND SETTINGS
View > Memory viewer (Ctrl+M) hides the memory column so the editor and the
terminal get the full width.  View > Terminal width fixes the terminal at 80,
120 or 132 columns instead of fitting the window.  View > Error list shows or
hides the BFL error list under the editor.  Settings (newline mode, terminal
speed and width, interpreter, check options, window placement, terminal
height, panes and the last file) are kept in %APPDATA%\bfmachine\settings.ini.
Started without a file, the program reopens the last one without running it.

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
  Loops that run a cell's value times with tests inside (a carry chain, a
  compare, halving, byte division: what BFL's multi-byte arithmetic is made
  of) are recognised as idioms and run in one step.  Known cell values are
  carried from block to block, so stores of a value a cell already holds
  disappear.
  None of this changes what a device sees: whenever the pointer can reach the
  device window the executor runs the literal program, one command at a time
  (only runs of < and > are merged, since moving touches no cell), so a
  trigger inside a loop fires once per iteration exactly as written.  The
  only shortcut there: a move or clear loop over RAM and plain registers
  (ones that behave like memory, such as FB_COLOR) runs in one step.)";

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
  For a BFL program the list is filled with the global variables by name.
  Each row shows the whole value: words and dwords in one row, signed types
  as negative numbers, pointers with the heap byte they point at (*=), and
  array elements of any width.  Ticking a wide row watches all its bytes.

IMPORTED FILES (BFL)
  When the debugger stops in a file brought in by import, that file is shown
  read-only in the editor's place, with the current statement selected.  The
  File box on the toolbar switches between the program and its imports; F9
  in an imported file sets a breakpoint there, and Ctrl+F10 runs to its
  caret line.  The editor comes back when the session ends.

Debug sessions use the interpreter with one instruction per source command,
so they are far slower than a normal run.  Use them for logic, not timing.

PROFILER
  Run > Profile (Ctrl+P) runs the editor contents on the interpreter and
  counts the IR instructions each source line executes, the same counts as
  bf --profile.  When the program ends (or Shift+F5 stops it) a gutter at the
  left of the editor shows each line's share of all instructions on a heat
  scale from pale yellow to red, and the pane under the editor lists the
  lines that executed the most, with their counts; double-click a row to go
  to its line.  For a BFL program a line is the innermost statement starting
  there; the row (outside statements) is code such as the dispatcher of
  rec procedures.
  For brainfuck the list gives line:column of each instruction.
  With imports, the File box shows each file's share, and an imported file
  picked there has the gutter too.  The next edit or run clears the profile.
  The interpreter is slower than the JIT, so the times are not comparable.)";

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
Programs that finish in milliseconds at full speed become watchable at 9600.

View > Terminal width keeps the terminal at a fixed 80, 120 or 132 columns
(hanoi.b wants about 120) instead of as many as fit; the window widens to
fit when it can, and the area past the margin is shaded.

NEWLINE MODE
Programs written for Unix end lines with LF only and rely on the terminal
driver to add the carriage return.  With newline mode on (the default; Run >
Terminal newline mode, or the "LF = new line" box on the toolbar) LF, VT and
FF also return the cursor to column 1.  Turn it off for strict VT100
behaviour, where LF only moves down and a program must send CR LF.  A program
can switch the mode itself with ESC [ 20 h (on) and ESC [ 20 l (off); clearing
the terminal or ESC c restores the setting chosen in the UI.)";

const wchar_t* CLI_TEXT = LR"(COMMAND LINE: bf.exe

  bf [options] program.b
  bf [options] -e 'code'

  --interp         run the interpreter instead of the x64 JIT
  --no-opt         disable optimisation (one instruction per command)
  --dump           print the intermediate code and exit
  --regs           print the memory map as a Markdown table and exit
  --time           print run time and status to stderr
  --profile        run in the interpreter, then list where the executed
                   instructions went (by BFL statement for a .bfl file);
                   Run > Profile in this window shows the same counts
  --eof MODE       ',' at end of input: nochange (default), 0, 255
  --ram N          RAM cells (default 1048576)
  --mmio N         device window cells (default 256, 0 disables devices)
  --serial PORT    attach the serial device to COMn, or 'loopback'
  --emit FILE      compile a .bfl file to brainfuck in FILE (and FILE.map)
  --lang           treat the input as BFL whatever its name
  --warn           report BFL warnings (unused variables, truncation, ...)
  --check-heap     BFL: halt with a message on a null or out-of-range heap access
  --check-stack    BFL: halt with a message when the recursion stack overflows
  --quiet-devices  do not report LED / number display changes on stderr

Exit code 0 when the program ends or halts, 3 on stop, fault or error.
Ctrl+C stops a running program.  LED and number display changes are printed
to stderr unless --quiet-devices is given.  In a Windows console, VT100 /
ANSI escape sequences in the program's output (colours, cursor movement) are
interpreted, so terminal demos such as programs\samples\raytracer_text.bfl
animate in place.

TESTS AND BUILD
  build.bat            release build to out\
  python tests\test.py runs every case through the JIT, the interpreter and
                       the unoptimised JIT and compares with a reference
                       interpreter; also checks README.md against bf --regs)";


const wchar_t* LANGUAGE_TEXT = LR"(BFL - A LOW-LEVEL LANGUAGE THAT COMPILES TO BRAINFUCK

Files ending in .bfl are compiled to brainfuck before they run (Run > Source
is BFL turns this on for other names).  Ctrl+B shows the generated brainfuck,
either annotated (a "# L12: source" comment before each statement, indented
by nesting) or compacted to the commands alone as one continuous string;
Copy puts the shown form on the clipboard and File > Export writes a .b file.  The debugger steps by statement: F11
steps into a procedure call, F10 steps over it, F9 sets a breakpoint on the
statement at the caret, and the watch list is filled with the program's
global variables.  The editor colours keywords, types, numbers, strings,
comments and register names.  Errors and warnings are listed under the
editor: double-click one or press F4 to go to it, also in imported files.
Run > Check heap accesses and Run > Check recursion stack add the checks of
bf --check-heap and --check-stack.  The full specification is docs/LANG.md
in the repository.

The compiler always knows where the pointer is: every variable has a fixed
cell (from 0x100 upward), procedures are inlined at each call, and registers
are addressed by name.  Recursion uses rec proc, described below.

TYPES AND DECLARATIONS
  byte x;  byte y = 7;        one cell, 0..255, wraps.  Zero unless initialised
  sbyte s = -5;               one cell, -128..127
  word w = 1000;              two cells (low, high), 0..65535, wraps
  int i = -300;               two cells, -32768..32767
  dword d;  long l = -1;      four cells, unsigned / signed 32-bit
  byte buf[16];               array of bytes; index may be a variable
  const MAX = 10;             compile-time constant, no cell
Variables declared inside { } exist until the closing brace and are cleared
there.  Names are case-sensitive and may not be register names.

STATEMENTS
  x = e;   x += e;  x -= e;  x *= e;  x /= e;  x %= e;
  x <<= e;  x >>= e;  x &= e;  x |= e;  x ^= e;   x++;  x--;
  if (c) { ... } else if (c) { ... } else { ... }
  while (c) { ... }          for (i = 0; i < 10; i++) { ... }
  break;  continue;
  out e;  out "text\n", x;   write bytes to the terminal ('.')
  print e, " ", w;           write numbers in decimal (strings as text)
  x = in;                    read one byte (','), unchanged at end of input
  w = input;                 read a decimal number up to the first non-digit
  LEDS = x;   x = LEDS;      registers by name (see Memory map)
  pulse TICK_LATCH;          write 1 to a register once (triggers)
  halt;                      write HALT
  asm { <<+>> }              raw brainfuck; must leave the pointer where it was
  proc name(byte a, ref word w, ref byte buf[]) { ... }    name(1, w, buf);
  proc byte sq(byte x) { return x * x; }     y = sq(3) + 1;   return e; / return;
  rec proc byte fib(byte n) { ... }          a procedure that may call itself
  shared proc glow(long x) { ... }           one copy, called through the trampoline
  stack 64;                  cells of recursion stack (top level; default 128)

PROCEDURES AND RECURSION
  A plain proc is inlined at every call and cannot call itself.  A rec proc
  is compiled as a trampoline: its body is cut into blocks at each call, a
  state cell picks the next block, and a stack array saves the parameters
  and locals of the current activation around each call.  Everything that
  calls a rec proc is compiled the same way.  Locals of a rec proc live in
  fixed cells and appear in the watch list as fib.n; __sp is the stack
  pointer.  F10 on a recursive call runs until the stack is back at the
  current depth.  Local arrays of a rec proc live in the heap, a fresh
  zeroed copy per call; array parameters are passed as pointers (the
  argument must be in the heap).  ref parameters are copied in and back
  out, and variables of code that calls a rec proc need unique names.
  stack N; sets the save stack (default 128 cells, up to 32768; above 256
  it is slower).  bf --check-stack halts on overflow with a message.  A
  call to a small procedure is about 60 ns on the JIT, more for each cell
  saved.
  A shared proc is compiled the same way but must not call itself: its
  body exists once however many places call it.  Only calls within a
  cycle of procedures save anything on the stack.  Sharing trampolines the
  callers too, so it pays for a sizeable procedure called from many places
  outside hot loops.

EXPRESSIONS
  Operators, loosest first:  ||  &&  |  ^  &  == !=  < <= > >=  << >>  + -  * / %
  Unary: - ! ~ and the casts byte(e), word(e).  Numbers: 10, 0x1F, 0b101, 'A'.
  Operands are converted to the wider type (signed only if both are signed);
  a constant takes the other operand's type when it fits.  byte + byte wraps
  at 256, so use word(a) + b for a 16-bit sum.  Casts: int(e) or (int) e.
  Signed / and % truncate towards zero as in C; >> is arithmetic on signed
  types.  A shift has its left operand's type.  Comparisons and ! && || give
  0 or 1.  Division by zero gives 0, the remainder gives the dividend.
  Numbers with an L suffix (5L) are long.
  && and || only evaluate the right side when needed.

ALLOCATION AND STRINGS
  Node* n = new Node;  int* a = new int[100];  byte* b = alloc(64);  free(n);
  Blocks are zeroed; null when the heap is full.  First fit, no merging.
  string s = "hello";         a string is a byte* to bytes ending in 0
  print s;                    prints the text

IMPORTS AND THE STANDARD LIBRARY
  import "strings.bfl";       next to this file first, then the standard library
  strings.bfl   strlen strcpy strcat strcmp strchr strdup utoa ltoa atol
  math.bfl      min max abs clamp (int); gcd isqrt pow (dword)
  io.bfl        read_byte read_line(buf, size) read_long
  memory.bfl    memset memcpy memcmp heap_unused
  Each file is included once; a name defined in two files is an error.

STRUCTS
  struct Point { int x; int y; }      fields in order, no padding
  Point p;  p.x = 3;  Point q = p;    in cells; copied, passed and returned by value
  Point pts[8];  pts[i].y = -1;       arrays of structs
  heap Point hp[50];  Point* r = &hp[2];  r->x = 1;   in the heap, through pointers
  sizeof(Point)                        4

POINTERS AND THE HEAP
  heap 16384;                 heap size in bytes (default 8192, at most 65535)
  heap byte buf[100];         a static array in the heap
  byte* p = &buf[10];         an address; *p, p[3], p + 1, p++ (scaled by size)
  word* w = (word*)p;         pointer casts; null is address 0
  Variables live in cells and have no address; only heap data does.  An
  access through a runtime address walks the heap: about 2 us and 1.6 KB of
  brainfuck per access site.  bf --check-heap halts on a null or
  out-of-range access with a message.

REGISTERS
  Latch registers (LEDS, KEY_DATA, TICKS0..3, NUMBER0/1, RANDOM, SER_DATA,
  SER_BAUD, FB_X, FB_Y, FB_COLOR) read and write like variables.
  Status registers (KEY_STATUS, SER_STATUS, SER_CTRL) are read only by
  testing - if (KEY_STATUS), while (SER_STATUS), x = !SER_STATUS - which
  never writes them; they count as 1 when non-zero.  Write them with pulse
  or with += 1 / -= 1 (SER_CTRL += 1 opens the port, -= 1 closes it).
  Write-only registers (CONOUT, HALT, TICK_LATCH, ROLL, SER_TX, FB_PLOT,
  FB_FILL, FB_READ) accept only pulse; every write a brainfuck program can
  make is an increment, so they always receive 1.

  Typical keyboard and serial loops:
      while (KEY_STATUS) { pulse KEY_STATUS; c = KEY_DATA; ... }
      SER_DATA = c; pulse SER_TX;
      while (SER_STATUS) { pulse SER_STATUS; out SER_DATA; }

COSTS
  Generated code is much longer than hand-written brainfuck, but the
  optimiser recovers most of the speed: + - and constant * become single IR
  instructions.  Comparisons, / %, shifts and bit operations are loops of a
  few hundred commands; word arithmetic is 5-20 times a byte's.  Array
  access with a variable index walks the array (3 cells per element).
  Compile-time constants cost nothing: the compiler tracks cell values in
  straight-line code and emits only the difference.

REGISTER KINDS (from registers.h)
)";

std::string register_kinds() {
    std::string s;
    char line[128];
    for (const bf::RegisterInfo& r : bf::register_map()) {
        const char* k = r.kind == bf::REG_LATCH ? "latch" : r.kind == bf::REG_STATUS ? "status (test only; pulse / += 1 / -= 1)" : "write-only (pulse)";
        snprintf(line, sizeof line, "  %-11s %s\n", r.name, k);
        s += line;
    }
    return s;
}

std::vector<Entry> build() {
    std::vector<Entry> t(TOPIC_COUNT);
    t[START]        = {L"Getting started", crlf(START_TEXT)};
    t[SYMBOLS]      = {L"Brainfuck symbols", crlf(SYMBOLS_TEXT)};
    t[MEMORY_MAP]   = {L"Memory map", crlf(std::wstring(MEMORY_TEXT) + widen(bf::register_map_text()))};
    t[DEVICES]      = {L"Devices", crlf(DEVICES_TEXT)};
    t[DEBUGGER]     = {L"Debugger", crlf(DEBUGGER_TEXT)};
    t[TERMINAL]     = {L"Terminal", crlf(TERMINAL_TEXT)};
    t[COMMAND_LINE] = {L"Command line", crlf(CLI_TEXT)};
    t[LANGUAGE]     = {L"BFL language", crlf(std::wstring(LANGUAGE_TEXT) + widen(register_kinds()))};
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
