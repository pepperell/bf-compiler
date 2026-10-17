# Brainfuck Machine

A fast brainfuck executor for Windows with memory-mapped hardware and a Win32 front end.
C++20, MSVC, no dependencies.

```
build.bat                      release build -> out\bf.exe, out\bfui.exe
build.bat debug                unoptimised build with symbols
python tests\test.py           correctness suite (JIT, interpreter, unoptimised, Python reference)

out\bf.exe [options] prog.b    console runner   (bf --help for options)
out\bfui.exe [prog.b]          Windows UI
```

The UI has a device panel (lamps, number display), a source editor, a VT100 terminal and a
memory viewer. F5 compiles and runs whatever is in the editor; Shift+F5 stops; Ctrl+O opens,
Ctrl+S saves, Ctrl+R reloads from disk, Ctrl+L clears the terminal. Dropping a `.b` file on
the window loads and runs it. Drag the bar between editor and terminal to resize them.

The terminal (`src/terminal.cpp`) is a real VT100/ANSI emulator: cursor addressing, erase
display and line, scroll regions, insert and delete, SGR colours and attributes, cursor
show/hide, and a scrollback for plain output. It starts 25 rows tall so classic 80x24
programs such as `hanoi.b` draw correctly. Keys typed into it feed `,` and the keyboard
registers; arrows and function keys arrive as VT sequences. Run > Terminal speed paces
output like a serial line (115200 down to 2400 baud) so animations that finish in
milliseconds at full speed become visible.

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
character (add one from the viewer's address box or at the pointer). Ticking a row makes
it a data breakpoint: the program pauses as soon as that cell is written, which works for
device registers too. "Break on entering device window" pauses when the pointer first
moves into the device cells.

Debug sessions run on the interpreter with one IR instruction per source command, so
they are much slower than a normal run; use them for logic, not for timing.
The API behind this is `bf::Debugger` in `src/bf.h`.

## Speed

Source is compiled to an optimised IR and then to x64 machine code at run time.
`mandelbrot.b` (Erik Bosman) runs in about 0.4 s with the JIT and 3.2 s on the IR
interpreter on this machine. A naive interpreter takes well over a minute.

Optimisations: run folding, offset fusion (one pointer update per block), clear loops,
multiply loops with any odd step, conditional once-only loops, scan loops, dead-loop removal.

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
copy, and whenever a block of optimised code could reach the device window the executor
runs the literal copy from the same source position until the pointer is back in RAM at a
point where the optimised code can resume. So a trigger written inside a loop fires once
per iteration exactly as the source says, in the JIT, the interpreter and the debugger.

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
`rel` is the offset within the mapped range, and call `Machine::map(addr, size, &dev)`. Device
code runs on the executor thread; use the thread-safe patterns in `devices.h` for UI work.
