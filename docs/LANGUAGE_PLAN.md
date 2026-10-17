# Plan: a low-level language that compiles to brainfuck

**Status: implemented.** All eight phases are in; the language reference is
`docs/LANG.md`. Differences from the plan below: `print` (decimal output) and
`input` (decimal input) are statements/expressions backed by a prelude written in
BFL; registers carry a kind (latch / status / write-only) in `registers.h`, which
replaced the plan's "assignment to write-only data registers"; the statement map
is an in-memory structure plus a `.map` text file rather than JSON.

Working name **BFL** (file extension `.bfl`). The name and the surface syntax are the two
things most worth changing before phase 2 starts; everything else in the plan is
independent of them.

## 1. Goal and constraints

Write hardware programs (LED chaser, serial echo, framebuffer drawing, later a game or a
monitor) without counting `<` and `>` by hand. The language is a thin layer over the
machine, not a general-purpose language:

- **Target is this machine's brainfuck**: 8-bit wrapping cells, pointer starts at 0x100,
  device registers at absolute cells 0x00..0xFF, latch-plus-trigger convention.
- **The compiler always knows where the pointer is.** Every variable lives in a fixed cell
  chosen at compile time. Control flow keeps the invariant that the pointer is in the same
  cell at a loop's `[` and its `]`. This is what makes register access possible: `LEDS = x`
  is a known number of `<` from a known cell.
- **No call stack.** Procedures are inlined at every call site. No recursion, no function
  pointers. This is the only approach that stays fast and keeps the pointer statically known.
- **Unsigned only.** `byte` (one cell) first, `word` (two cells, little-endian) later.
- **Generated code must suit the existing optimiser**: use `[-]` for clear, `[-x+y]` for
  move, `[-]+++` for set, so the IR passes turn most of it into SET, MUL and ADD. Anything
  touching the device window executes literally anyway, so hardware access should emit the
  minimum number of writes.
- **Register names and access rules come from `src/registers.h`**, the existing single
  source of truth. Writing a read-only register or reading a write-only one is a compile
  error. Adding a register to the table makes it a keyword with no other change.

## 2. The language

### Types and declarations

```
byte x;              // one cell, zero-initialised (RAM is zero at start)
byte y = 7;
word n = 1000;       // phase 6: two cells
const byte MAX = 10; // compile-time only, no cell
byte buf[16];        // phase 6: fixed-size array
```

Globals only in phase 3; procedure locals (which are just more globals with scoped names)
in phase 5. Every variable gets a fixed cell from 0x100 upward.

### Statements

```
x = expr;            x += expr;  x -= expr;  x++;  x--;
if (cond) { ... } else { ... }
while (cond) { ... }
for (i = 0; i < 10; i++) { ... }        // sugar for while
break;                                   // phase 5, via a flag cell
out expr;            out "text\n";       // '.'  (string compiles to set+out pairs)
x = in;                                  // ','
LEDS = x;            x = KEY_DATA;       // register access by name
pulse TICK_LATCH;                        // exactly one write of 1 to a trigger register
halt;                                    // write HALT
asm { <<+>> }                            // raw brainfuck; must be pointer-neutral
proc blink(byte times) { ... }           // inlined at each call
blink(3);
```

### Expressions

Arithmetic `+ - * / %`, shifts `<< >>` (as multiply/divide by powers of two), comparisons
`== != < <= > >=` (result 0 or 1), logical `&& || !` with short-circuit, parentheses,
constants (decimal, hex, char `'A'`), variables, registers, `in`. Bitwise `& | ^` are
expensive in brainfuck (loop over 8 bits) and come last.

A condition is any expression; non-zero is true. `while (1)` is the idiom for a main loop.

### Hardware model in the language

- A register name reads or writes the absolute cell. Readable registers are latches, so
  `x = KEY_DATA` is an ordinary copy.
- `pulse REG;` is the only way to touch a trigger register; it emits a single `+`. Plain
  assignment to a write-only register is allowed for data registers like CONOUT and FB_FILL
  (value computed in RAM, then moved in with one `[-]`-free transfer loop so the register
  sees `v` increments, which is the brainfuck minimum).
- The compiler never emits `[-]` on a write-only register (it would be a pointless read)
  and never emits `[` or `]` while the pointer is on a register.

## 3. Compiler architecture

New directory `src/lang/`, compiled into CORE so both `bf.exe` and `bfui.exe` can compile
`.bfl` directly. A thin `bfc.exe` is optional; `bf prog.bfl` and `bf --emit prog.bfl` cover
the command line.

```
src/lang/lexer.cpp      tokens with line/column
src/lang/parser.cpp     recursive descent -> AST
src/lang/sema.cpp       names, types, register access checks, constant folding
src/lang/codegen.cpp    AST -> brainfuck text + source map
src/lang/lang.h         public API: compile_bfl(src) -> {bf_text, source_map, symbols, errors}
```

Core code generation pieces:

- **Emitter** with `cur` (absolute cell), `goto(cell)`, `emit(text)`, `loop_begin()` /
  `loop_end()` that assert pointer equality, and a byte-offset counter that drives the
  source map.
- **Cell allocator**: fixed cells for named variables; a LIFO temp pool just above them.
  Temps are always returned zeroed (the allocator tracks whether a cell is known-zero so
  `[-]` is skipped when it is).
- **Known-value tracking** (phase 8): the emitter remembers cells whose value is a known
  constant and emits the shortest `+`/`-` delta instead of clear-then-count. Invalidated
  by loops and by any read of a device register.
- **Idiom library**: clear, move, copy-via-temp, add-constant, if-once (`t[ body t[-] ]`),
  if/else (two flags), while (condition recomputed at the end of the body), compare
  routines, multiply, divmod. Each idiom is a function on the emitter with documented
  pre/post pointer positions and which temps it needs.
- **Source map**: for every statement, the byte range of the generated brainfuck, plus
  the symbol table (name to cell). Serialised as JSON-ish text next to the `.b` when
  `--emit` is used; passed in memory to the UI.

Short-circuit `&&`/`||`, `break`, and `if/else` all reduce to flag cells and the if-once
idiom, so the control-flow part of the backend is small once that idiom is right.

## 4. Integration with the existing tools

- **bf.exe**: `.bfl` input compiles then runs; `--emit out.b` writes the brainfuck and
  `.map`; errors print `file:line:col: message`.
- **bfui.exe**: Open accepts `.bfl`. F5 compiles, shows errors in the status bar and
  selects the offending text, then runs the brainfuck. A collapsible "Generated" pane (or a
  View menu toggle) shows the brainfuck output.
- **Debugger**: breakpoints and the current-statement highlight work in the high-level
  source. The UI maps a `.bfl` statement to the first brainfuck byte of its range and uses
  the existing IR-pc breakpoints; "step" means run until the pc leaves the current
  statement's range (one `run_to`-style temp breakpoint per following statement, or a
  small extension to `Debugger` taking a pc range). The watch list auto-populates with the
  program's named variables, so a debug session shows `led = 0x08` rather than cell 0x101.
- **Help**: a new topic "BFL language" generated partly from the register table (the list
  of register keywords is `registers.h` again). README gets a section and an example.
- **Tests**: `tests/test_lang.py` compiles each case with `bf --emit`, runs it through the
  JIT, interpreter and `--no-opt`, and compares with expected output. Arithmetic cases
  are generated: for `a op b` over a grid of values, expected values come from Python
  mod 256. Compile-error cases check the message and position.
- **Examples**: rewrite `leds.b`, `serial_echo.b` and `fb.b` as `.bfl`; keep the `.b`
  originals. The generated code of each becomes a regression check (byte-exact after
  phase 8 stabilises, output-exact before).

## 5. Phases

Each phase ends with a green test run and a commit. Phases 3 and 4 are where the risk is;
the rest is mechanical.

| # | Deliverable | Notes |
|---|-------------|-------|
| 1 | `docs/LANG.md`: syntax, semantics, hardware rules, grammar in EBNF | Review point for syntax before any code |
| 2 | Lexer, parser, AST, `bf --parse` dump, error positions | Parser tests only |
| 3 | Codegen core: byte variables, constants, assignment, `+=`/`-=`, `++`/`--`, copy, `out`, `in`, `if`, `while`, register read/write, `pulse`, `halt`; `bf prog.bfl` runs | First real program: LED chaser |
| 4 | Full expressions: `* / % << >>`, all comparisons, `&& || !`, string literals, constant folding | Generated arithmetic test grid |
| 5 | `proc` with inlining, parameters (by value; `ref byte` by cell alias), `for`, `break`, `asm`, scoped locals | serial_echo rewrite |
| 6 | `byte[]` arrays with constant and runtime index, `word` type with carry, `out`/`in` for words as decimal | Runtime index uses the interleaved-scratch layout |
| 7 | UI integration: open/compile/run, generated-code pane, source-level debugging, auto watch list, help topic, README | fb.b rewrite as the demo |
| 8 | Output quality: known-value tracking, temp reuse, shortest constants, cell layout that keeps hot variables adjacent; measure generated size and run time of the three examples | Byte-exact regression files from here on |

## 6. Decisions to confirm

1. **Syntax family.** The plan assumes C-like braces and semicolons because the editor,
   tests and users already deal with C++. A Pascal-like or assembler-like syntax is equally
   feasible; only phases 1 and 2 change.
2. **Procedures are inline-only.** Recursion and real calls are out of scope; if they are
   wanted later they need a stack-in-tape design, which is a separate project.
3. **Unsigned only, 8-bit default.** Signed arithmetic can be layered on later as a
   library of procedures rather than a type.
4. **Compiler lives in the core**, not a separate executable, so the UI gets source-level
   debugging for free. A standalone `bfc.exe` is a three-line addition if wanted.
5. **Name.** BFL is a placeholder.

## 7. Risks

- **Runtime array indexing** is the one feature with no cheap brainfuck idiom; it is
  isolated in phase 6 so everything else can ship first.
- **Code size**: comparisons and division expand to large loops. Known-value tracking and
  the optimiser's multiply-loop recognition recover most of the run time, but a `.bfl`
  program will always compile to much more brainfuck than a hand-written one. The
  generated-code pane makes this visible instead of mysterious.
- **Pointer invariant bugs** are the classic failure in this kind of compiler. The emitter
  asserts the invariant at every `]`, and the four-way test harness (JIT, interpreter,
  no-opt, Python expectation) catches anything that slips through.
