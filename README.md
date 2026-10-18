# Brainfuck Machine

A brainfuck executor for Windows that behaves like a small computer rather than a toy
interpreter. C++20, MSVC, no dependencies.

- **Memory-mapped hardware.** LEDs, a numeric display, a millisecond clock, a random
  source, a real serial port and a 128x64 framebuffer live in tape cells just left of the
  start cell, driven with the same `+`, `-`, `<` and `>` as everything else.
- **x64 JIT.** Source is optimised to an IR and compiled to machine code at run time;
  `mandelbrot.b` runs in 0.4 s. Loop idioms recognise multi-byte arithmetic and run whole
  loops in one step.
- **Exact device semantics.** Optimised code never touches a register, so a trigger in a
  loop fires once per iteration exactly as written, in the JIT, interpreter and debugger.
- **BFL.** A small C-like language with 8/16/32-bit integers, pointers and a heap, structs,
  strings, recursion and a standard library, compiled to brainfuck with registers by name.
- **Win32 UI.** Editor, device panel, VT100 terminal with serial-line pacing, live memory
  viewer and watch list, framebuffer window.
- **Debugger and profiler.** Step, step over, breakpoints and data breakpoints on cells,
  variables or registers, working on BFL source or raw brainfuck; per-line instruction
  counts shown in the editor gutter.
- **Zero-overhead stop and fault detection.** No poll in generated code: stopping revokes
  page access, and guard regions catch a pointer leaving the tape with the cell it touched.

```
build.bat                      release build -> out\bf.exe, out\bfui.exe
build.bat debug                unoptimised build with symbols

out\bf.exe [options] prog.b    console runner   (bf --help for options)
out\bf.exe prog.bfl            compile a BFL program to brainfuck and run it
out\bfui.exe [prog.b|prog.bfl] Windows UI

python tests\test.py           correctness suite (JIT, interpreter, unoptimised, Python reference)
python tests\test_lang.py      BFL compiler suite (arithmetic grids, devices, golden files)
python tests\fuzz.py           random BFL and brainfuck through every executor
```

## Memory map

Cells are 8-bit. The tape is 256 device cells followed by 1 MiB of RAM. The pointer starts
at the first RAM cell, so `<n` is how many `<` reach a register. This table is generated
from `src/registers.h` by `bf --regs` (the test suite fails if it differs) and is also in
the UI's help (F1).

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

Readable registers are latches that read back what was last written, so the usual copy
idiom `[>+<-]` works on them. Triggers (sends, pops, plots) are separate cells from their
data latches, so `+` loops never fire one by accident. The serial device talks to a real
COM port (Devices menu, or `bf --serial COM3`; `loopback` for testing). The framebuffer
opens in its own window on the first plot (Ctrl+F).

Optimised code never touches a register. Every program keeps a literal
one-instruction-per-command copy, and whenever a block could reach the device window the
executor runs that copy until the pointer is back in RAM. A trigger written inside a loop
therefore fires once per iteration exactly as the source says, in the JIT, the interpreter
and the debugger. `programs/leds.b`, `serial_echo.b` and `fb.b` are small demos.
Adding a device is a `bf::Device` subclass and one `Machine::map()` call.

## BFL

Counting `<` to reach a register is tedious, so BFL (`docs/LANG.md`, Help > BFL language)
compiles to brainfuck with every variable in a fixed cell and registers addressed by name:

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

It has signed and unsigned 8, 16 and 32-bit integers, pointers into a 64 KB heap with
`new` and `free`, structs, strings, arrays, the usual control flow and operators,
`asm { }` for raw brainfuck, `import` with a standard library in `lib/`, and recursion
(`rec proc` compiles to a trampoline with an explicit stack, `docs/TRAMPOLINE.md`).
`bf --emit` writes the generated code with a source comment before each statement.
`programs/samples/` has a linked list, a tree, a calculator, N queens, merge sort, a maze
solver, Life, a revolving cube, a starfield and a raytracer; `programs/thirdparty/` has
classic programs by Erik Bosman and Daniel B. Cristofani.

![raytracer.bfl: 16 frames of one camera orbit](docs/raytracer.gif)

`programs/samples/raytracer.bfl`: four reflective spheres, two lamps with soft shadows and
a checkerboard floor, in fixed-point BFL on the framebuffer. A 128x64 frame takes about
42 ms with the JIT.

## UI

Device panel, source editor, VT100 terminal, memory viewer and watch list. F5 compiles and
runs the editor, Shift+F5 stops, Ctrl+B shows the generated brainfuck. The terminal is a
real VT100/ANSI emulator (cursor addressing, scroll regions, colours), and Run > Terminal
speed paces output like a serial line so fast animations are visible.

**Debugger.** Ctrl+F5 starts paused; F11 steps, F10 steps over a loop or call, F9 sets a
breakpoint, Ctrl+F10 runs to the caret. The watch list shows cells or BFL variables by
name, whole and typed, and ticking one makes it a data breakpoint. For BFL the debugger
works on the source, including imported files.

**Profiler.** Run > Profile (Ctrl+P) counts IR instructions per source line and colours
the editor gutter by share; `bf --profile` does the same on the console.

Settings persist in `%APPDATA%\bfmachine\settings.ini`.

## Speed

Source is compiled to an optimised IR and then to x64 at run time. `mandelbrot.b` runs in
about 0.4 s with the JIT and 3.2 s on the interpreter; a naive interpreter takes over a
minute. Besides the usual run folding, offset fusion, clear, multiply and scan loops, the
optimiser executes candidate loop bodies symbolically and recognises BFL's multi-byte
arithmetic idioms (carry chains, saturating subtract, halving, division, multiplication),
running the whole loop in one step; with them a raytracer frame went from 2.3 s to about
42 ms. `bf --dump` shows the IR.

## How it works

```
src/bf.h        public API: compile(), Machine, Device, ConsoleIo
src/ir.cpp      parser and optimiser
src/jit.cpp     x64 code generator
src/interp.cpp  IR interpreter: reference executor and the JIT's slow path
src/machine.cpp tape, device bus, run loop, stop and fault handling
src/devices.h   the standard devices
src/registers.h the register table (single source of truth)
src/lang/       the BFL compiler
lib/            the BFL standard library, embedded at build time
src/cli.cpp     bf.exe
src/ui.cpp      bfui.exe
```

The JIT keeps the tape pointer in `r12` and the start of RAM in `r13`; each block checks
its lowest reachable address once and hands off to the interpreter if it could reach the
device window. There is no stop poll in generated code: `request_stop()` revokes page
access on RAM, the next access faults, and a vectored exception handler unwinds to the exit
stub. The tape sits between two 1 GiB guard regions, so a pointer that leaves it is
reported as a fault with the cell it touched.

## Licence

MIT; see `LICENSE`. The third-party brainfuck programs in `programs/thirdparty/` keep their
authors' own terms, listed in `programs/thirdparty/README.md`.
