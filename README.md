# Brainfuck Machine

A fast brainfuck executor for Windows with memory-mapped hardware and a Win32 front end.
C++20, MSVC, no dependencies.

```
build.bat                      release build -> out\bf.exe, out\bfui.exe
build.bat debug                unoptimised build with symbols
python tests\test.py           correctness suite (JIT, interpreter, unoptimised, Python reference)

out\bf.exe [options] prog.b    console runner   (bf --help for options)
out\bf.exe prog.bfl            compile a BFL program to brainfuck and run it
out\bfui.exe [prog.b|prog.bfl] Windows UI
python tests\test_lang.py      BFL compiler suite (arithmetic grids, devices, golden files)
python tests\fuzz.py          random BFL and brainfuck through every executor (--help for options)
```

The UI has a device panel (lamps, number display), a source editor, a VT100 terminal and a
memory viewer. F5 compiles and runs whatever is in the editor; Shift+F5 stops; Ctrl+O opens,
Ctrl+S saves, Ctrl+R reloads from disk, Ctrl+L clears the terminal. Dropping a `.b` file on
the window loads and runs it. Drag the bar between editor and terminal to resize them.
View > Memory viewer (Ctrl+M) hides the memory column to give the editor and terminal
the full width, and View > Terminal width fixes the terminal at 80, 120 or 132 columns
(`hanoi.b` needs about 120) instead of fitting the window.

The UI remembers its settings in `%APPDATA%\bfmachine\settings.ini`: newline mode,
terminal speed and width, interpreter choice, the check options, window placement,
terminal height, which panes are shown and the last file. Started without a file, it
reopens the last one without running it. (The environment variable `BFUI_SETTINGS`
names a different settings file.)

The terminal (`src/terminal.cpp`) is a real VT100/ANSI emulator: cursor addressing, erase
display and line, scroll regions, insert and delete, SGR colours and attributes, cursor
show/hide, and a scrollback for plain output. It starts 25 rows tall so classic 80x24
programs such as `hanoi.b` draw correctly. Keys typed into it feed `,` and the keyboard
registers; arrows and function keys arrive as VT sequences. Run > Terminal speed paces
output like a serial line (115200 down to 2400 baud) so animations that finish in
milliseconds at full speed become visible. Newline mode (on by default; Run menu or
the "LF = new line" toolbar box) makes LF also return to column 1, as a Unix
terminal driver does, so programs that end lines with LF alone print correctly.
Turn it off for strict VT100 behaviour; programs can also set it with `ESC[20h` /
`ESC[20l`.

The memory viewer shows a hex dump of any tape region (type a hex address and press Enter,
or use the Start / Dev / Ptr buttons), refreshes live while the program runs, marks
device-window rows with `D` and brackets the pointer cell.

### Debugger

Ctrl+F5 (or the Debug button) starts the editor contents paused on the first command.
F11 steps one command, F10 steps over a `[` loop (runs it to the matching `]`), Ctrl+F10
runs to the command at the caret, F5 continues, F6 pauses a running program, Shift+F5 stops.
F9 toggles a breakpoint on the command at the caret (or the next command on that line);
breakpoints show as a red background and survive edits because they live in the text
formatting. The current command is highlighted in the editor, the status bar shows why
the program paused, its line and column and the current cell's value, and the memory
viewer follows the pointer.

The watch list under the memory viewer shows chosen cells live in hex, decimal and as a
character (add one from the viewer's address box or at the pointer). For a BFL program it
is filled with the global variables by name, each shown whole: 16 and 32-bit values in
one row, signed types as negative numbers, pointers with the heap byte they point at,
and array elements of any width. Ticking a row makes
it a data breakpoint: the program pauses as soon as that cell is written, which works for
device registers too. "Break on entering device window" pauses when the pointer first
moves into the device cells.

Debug sessions run on the interpreter with one IR instruction per source command, so
they are much slower than a normal run; use them for logic, not for timing.
The API behind this is `bf::Debugger` in `src/bf.h`.

### Profiler

Run > Profile (Ctrl+P) runs the editor contents on the interpreter and counts the IR
instructions executed for each source line: for BFL, the innermost statement that starts
on the line, the same attribution as `bf --profile` (`src/profile.h` is shared by both).
When the program ends, or Shift+F5 stops one that runs forever, a gutter at the left of
the editor colours each line by its share of all instructions (pale yellow to red) and
prints the percentage, and the pane under the editor lists the lines that executed the
most with their counts; double-click a row to go to the line. For a multi-file program the
File box shows each file's share and an imported file picked there has the gutter too.
For plain brainfuck the list gives each instruction's line and column. The next edit or
run clears the profile.

## BFL: a language that compiles to brainfuck

Writing device drivers by counting `<` is tedious, so there is a small unsigned
low-level language, BFL (`docs/LANG.md`, Help > BFL language). A `.bfl` file is
compiled to brainfuck by `src/lang/` inside `bf.exe` and `bfui.exe`, then run like
any other program. The compiler always knows where the pointer is: every variable
has a fixed cell, procedures are inlined, and registers are addressed by name from
`src/registers.h`, which also says how each one may be touched (latch, status,
write-only).

```
proc wait_ms(byte ms) {
    pulse TICK_LATCH;
    byte start = TICKS0;
    byte elapsed = 0;
    while (elapsed < ms) { pulse TICK_LATCH; elapsed = TICKS0 - start; }
}
byte led = 1;
while (led) { LEDS = led; wait_ms(60); led = led << 1; }
halt;
```

Signed and unsigned 8, 16 and 32-bit integers, pointers into a heap of up to
64 KB with `new` and `free`, structs, strings, arrays of any type,
`if`/`while`/`for`/`break`, the usual operators, `out`/`print`/`in`/`input`,
`pulse` for trigger registers, `asm { }` for raw brainfuck, `import` with a
standard library in `lib/` (strings, math, io, memory), procedures with return
values, and recursion:
`rec proc` compiles a procedure as a trampoline with an explicit stack array
(`docs/TRAMPOLINE.md`), so `programs/hanoi.bfl` and `programs/quicksort.bfl`
are written the way they would be in C. `shared proc` uses the same machinery
to keep one copy of a procedure that is called from many places instead of
inlining it at each. `bf --emit prog.b prog.bfl` writes the generated
code (with a `# L12: source` comment before each statement) and a statement map.
In the UI, Ctrl+B shows the generated brainfuck, annotated or compacted to one
continuous string of commands, and the debugger works on the source: F11 steps a statement (into calls), F10 steps over, F9 breakpoints by
line, and the watch list is filled with the program's variables by name.
The editor colours BFL keywords, types, numbers, strings, comments and register
names. Compiler errors and warnings are listed in a pane under the editor;
double-click one, or press F4, to go to it. When the debugger stops in an imported
file, that file is shown read-only in the editor's place; the File box in the
toolbar switches between the program and its imports, and F9 sets breakpoints in
imported files too. Run > Check heap accesses and Run > Check recursion stack
compile in the same checks as `bf --check-heap` and `--check-stack`.
`programs/leds.bfl`, `serial_echo.bfl`, `fb.bfl` and `primes.bfl` are examples;
`tests/golden/` holds their generated code, and `tests\test_lang.py` fails if it
changes (run it with `--update-golden` after a deliberate codegen change).

## Example programs

`programs/` holds small demos for the devices (`leds`, `serial_echo`, `fb`) in
brainfuck and in BFL. `programs/samples/` holds BFL programs that use the
language's newer features: a linked list, a binary search tree, a calculator
with a recursive-descent parser, N queens, merge sort, string handling, the
integer types, a two-file maze solver and an animated Game of Life; its README
says what each one shows. `programs/thirdparty/` holds programs by other authors:
Erik Bosman's `mandelbrot.b`, and 32 programs by Daniel B. Cristofani from
brainfuck.org (CC BY-SA 4.0) including a self-interpreter, a universal Turing
machine, Life, tic-tac-toe, quines, sorts and streaming digits of e. Its README
lists what each one does and what input to give it, and the test suite runs the
terminating ones through every executor.

## Speed

Source is compiled to an optimised IR and then to x64 machine code at run time.
`mandelbrot.b` (Erik Bosman) runs in about 0.4 s with the JIT and 3.2 s on the IR
interpreter on this machine. A naive interpreter takes well over a minute.

Optimisations: run folding, offset fusion (one pointer update per block), clear loops,
multiply loops with any odd step, conditional once-only loops (also with if-blocks inside),
scan loops, dead-loop removal, known cell values carried across blocks (each block is
rebuilt from its net effect, so a copy through a known-zero scratch cell is one
multiply-add), and loop idioms.

**Loop idioms.** BFL's multi-byte arithmetic is built from byte loops that run a cell's
value times: a carry or borrow chain adding a byte into a number one unit at a time, the
saturating subtract behind every compare, halving with a remainder bit, byte division.
The optimiser executes each candidate loop body symbolically (one pass as a function of
the cells' entry values, as a set of paths with linear expressions), compares that
function with each idiom's one-pass rule on a set of points that covers every case, and
on a match puts an `IDIOM` instruction at the top of the body that runs the whole loop at
once. Scratch cells the loop always leaves holding a constant are checked on entry; if
one holds something else the body runs as written. `bf --dump` shows them. On
`programs/samples/raytracer.bfl` this made a frame 16 times faster (2.3 s to 85 ms).

## Memory map

Cells are 8-bit. The tape is `mmio_size` device cells followed by `ram_size` RAM cells.
The pointer starts at the first RAM cell, so hardware lives just to the left of it.
`<n` is how many `<` reach the register from the start cell. Defaults: 256 device cells, 1 MiB RAM.

The table below is generated from `src/registers.h` by `bf --regs`, and the test suite
fails if it differs, so it is always current. The same table appears in the UI's help
(F1, or Help > Memory map).

| Cell | From start | Register | Access | Meaning |
|------|-----------|----------|--------|---------|
| 0xFF | <1 | LEDS | r/w | 8 lamps, bit 7 is leftmost |
| 0xFE | <2 | KEY_STATUS | r/w | reads 1 when a key waits; a write pops the next key into KEY_DATA |
| 0xFD | <3 | KEY_DATA | r/w | latch holding the last popped key |
| 0xFC | <4 | CONOUT | w | writes a byte to the console, same as '.' |
| 0xFB | <5 | TICKS0 | r/w | milliseconds since run start, low byte (loaded by TICK_LATCH) |
| 0xFA | <6 | TICKS1 | r/w | milliseconds, second byte (256 ms units) |
| 0xF9 | <7 | TICKS2 | r/w | milliseconds, third byte |
| 0xF8 | <8 | TICKS3 | r/w | milliseconds, high byte |
| 0xF7 | <9 | NUMBER0 | r/w | 16-bit numeric display, low byte |
| 0xF6 | <10 | NUMBER1 | r/w | 16-bit numeric display, high byte |
| 0xF5 | <11 | HALT | w | any write ends the program with status "halted" |
| 0xF4 | <12 | RANDOM | r/w | latch holding the last random byte (loaded by ROLL) |
| 0xF3 | <13 | TICK_LATCH | w | writing loads TICKS0..3 with the current time |
| 0xF2 | <14 | ROLL | w | writing loads RANDOM with a new random byte |
| 0xF1 | <15 | SER_STATUS | r/w | reads 1 while a received byte waits; a write pops it into SER_DATA |
| 0xF0 | <16 | SER_DATA | r/w | latch: the byte to send, or the byte last popped |
| 0xEF | <17 | SER_TX | w | transmit SER_DATA |
| 0xEE | <18 | SER_CTRL | r/w | write 0 close, 1 open the port; reads bit0 open, bit7 error |
| 0xED | <19 | SER_BAUD | r/w | 0:300 1:1200 2:2400 3:9600 4:19200 5:38400 6:57600 7:115200 |
| 0xEC | <20 | FB_X | r/w | framebuffer cursor column 0..127 |
| 0xEB | <21 | FB_Y | r/w | framebuffer cursor row 0..63 |
| 0xEA | <22 | FB_COLOR | r/w | colour latch, RRRGGGBB |
| 0xE9 | <23 | FB_PLOT | w | plot FB_COLOR at the cursor, then advance (wraps to the next row) |
| 0xE8 | <24 | FB_FILL | w | fill the whole screen with the written value |
| 0xE7 | <25 | FB_READ | w | load FB_COLOR with the pixel under the cursor |

The serial device (`src/hw.h`) talks to a real COM port chosen from the UI's Devices menu
or with `bf --serial COM3`; `loopback` wires TX to RX for testing without hardware.
Sends and pops are triggers separate from the data latch, so the usual `+` loops never
transmit by accident. The framebuffer is a 128x64 byte-per-pixel display shown in its own
window (Ctrl+F, or automatically on the first plot). `programs/serial_echo.b` and
`programs/fb.b` demonstrate both.

Readable registers are latches: they read back what was last written, which is what makes
the normal brainfuck copy idiom `[>+<-]` work on a register. Optimised code never executes
against device registers: every compiled program keeps a literal one-instruction-per-command
copy (only runs of `<` and `>` are merged into one move, since moving touches no cell, so
a loop that copies a value into a register unit by unit does not walk the pointer cell by
cell on every turn), and whenever a block of optimised code could reach the device window the executor
runs the literal copy from the same source position until the pointer is back in RAM at a
point where the optimised code can resume. So a trigger written inside a loop fires once
per iteration exactly as the source says, in the JIT, the interpreter and the debugger.
The one shortcut: a loop the optimiser would turn into multiply-adds runs in one step in
the literal copy too when every cell it touches is RAM or a *plain* register, one that
`Device::plain()` says behaves like memory (it reads back exactly what was written, with
no side effects), such as the framebuffer's colour latch. Then `FB_COLOR = c` costs a few
bus accesses instead of one per unit of the old and new values.

`programs/leds.b` is a chaser that drives LEDS, paces itself with TICK_LATCH / TICKS1 and
finishes through HALT. Comments in `.b` files must avoid the eight command characters.

## How the executor works

```
src/bf.h        public API: compile(), Machine, Device, ConsoleIo
src/ir.cpp      parser and optimiser
src/jit.cpp     x64 code generator
src/interp.cpp  IR interpreter: reference executor and the JIT's slow path
src/machine.cpp tape, device bus, run loop, stop and fault handling
src/devices.h   the standard devices above
src/registers.h the register table: memory map, access, kind (single source of truth)
src/profile.h   profile counts per BFL line or brainfuck offset (bf --profile, Run > Profile)
src/lang/       the BFL compiler: parse.cpp (lexer, parser), sema.cpp (types and checks),
                codegen.cpp (brainfuck emitter)
lib/            the BFL standard library, embedded into the executables at build time
tools/          embed.cpp (the build step that embeds lib/), heap_proto.py (docs/HEAP.md)
src/cli.cpp     bf.exe
src/ui.cpp      bfui.exe
```

**JIT register use.** `r12` holds the tape pointer, `r13` the start of RAM, `rbx` the run
context. Every straight-line block computes its lowest reachable address once and compares
it against `r13`. Blocks that only touch cells at or above the pointer need no check at all.
`[` and `]` are a single `cmp byte [r12], 0` plus a branch.

**Device access.** If a block could reach below RAM, the generated code hands the IR index
to the interpreter, which executes with full bus semantics (every cell access is bounds
checked and routed to devices) until it reaches a labelled instruction with the pointer back
in RAM, then returns the native address to resume at. The pointer may wander through the
device window freely; the fast code never sees it.

**Stop with zero overhead.** There is no poll in the generated loops. `request_stop()` sets
a flag and revokes page access on RAM. The next tape access faults, a vectored exception
handler recognises the run context, and it redirects the thread to the JIT exit stub using
the stack pointer saved in the prologue. The same handler reports a pointer that leaves the
tape (the tape sits between two 1 GiB reserved guard regions) as status "fault".

**Adding a device.** Derive from `bf::Device`, override `read(rel)` / `write(rel, v)` where
`rel` is the offset within the mapped range (and `plain(rel)` for registers that behave
like memory), and call `Machine::map(addr, size, &dev)`. Device
code runs on the executor thread; use the thread-safe patterns in `devices.h` for UI work.
