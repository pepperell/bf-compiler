# Roadmap: BFL towards modern programming

Status: implemented (phases 0 to 19).
Hardware and devices are out of scope here. The sections below are the plan as
written. Where the result differs, docs/HEAP.md (phase 0), docs/TRAMPOLINE.md
(phase 7), the note under phase 8 and the **Built** notes in sections 11 and 12 say
what was built, and docs/LANG.md describes the language as it is.

BFL today has bytes and 16-bit words, byte arrays, inlined procedures with return
values, and recursion through `rec proc` (docs/LANG.md, docs/TRAMPOLINE.md). What
it cannot express is data that lives somewhere other than a fixed cell: no
pointers, no structs, no strings as values, no word arrays, no dynamic
allocation, and recursive procedures cannot own arrays. This plan adds those,
plus the type system, library and tooling they need.

## 1. Order and why

| Phase | Deliverable | Depends on |
|------:|-------------|------------|
| 0 | Heap access prototype and measurements; pick the layout | nothing |
| 1 | Typed front end: semantic pass, error recovery, several errors per compile | nothing |
| 2 | Integer types: signed 16-bit `int`, 32-bit `dword` and `long` | 1 |
| 3 | Heap and pointers | 0, 1 |
| 4 | Structs, word arrays, arrays of structs | 3 |
| 5 | Allocator and strings | 3, 4 |
| 6 | Imports and the standard library | 1, 5 |
| 7 | Recursion on the heap: frames in memory, restrictions lifted | 3, 4 |
| 8 | Tooling: settings, terminal width, syntax colouring, multi-file debugging | 6 |
| 9 | A differential fuzzer for the compiler, the optimiser and the JIT | nothing |
| 10 | Fused multiply-and-shift for fixed-point arithmetic | 9 |
| 11 | Arithmetic shared within a block, without the trampoline | 9 |
| 12 | Faster division | 9 |
| 13 | The profiler in the UI | nothing |
| 14 | Housekeeping (the raytracer animation) | nothing |
| 15 | Multiplication as one IR step | 9 |
| 16 | Shorter trips to operands: globals laid out by the trips to them, some above the temporaries | 9 |
| 17 | Cheaper `mulshr`: no shift after the loop, signs as a start value, constant factors | 10, 15 |
| 18 | Loose ends of phases 11 to 13: device-window profile, dark theme, known values in shared runs | 11, 13 |
| 19 | Fuzzer coverage of structs, `ref` arrays and `shared proc` | 9 |

One change from the earlier suggestion: better compiler errors move from last to
phase 1. Pointers, structs and signed types need a real type checker, and today
types are worked out ad hoc during code generation (`width()` in
`src/lang/codegen.cpp`). Building a semantic pass first gives the type checker,
and reporting several errors comes almost free with it.

Phases 0 and 1 can run in parallel. Each phase ends with both test suites green,
the golden files either unchanged or deliberately regenerated, the docs updated,
and a commit.

## 2. Phase 0: heap prototype (decision gate)

The whole plan rests on one routine: read or write the byte at a 16-bit address
held in a variable. Brainfuck has no indirect addressing, so the access has to
walk the tape. Phase 0 builds that routine both ways, measures it, and picks one.

**Layout A, flat.** The heap is one array of up to 64 KB using today's array
idiom: three cells per byte (counter, carry, data), walked one element per step.
Code per access site is small, about 70 commands. Run time is proportional to
the address, so a byte near the top of a 64 KB heap is tens of thousands of
steps away.

**Layout B, paged.** 256 pages of 256 bytes. The walk first hops whole pages,
then elements within the page, so no access passes more than 510 steps. The cost
moves into code size: hopping one page moves the pointer about 770 cells, and the
brainfuck text for each carry loop repeats that move. Each access site could be
several kilobytes of brainfuck. The IR optimiser folds the moves, so the JIT code
stays small, but the generated `.b` file grows.

**Measure** with three workloads written in hand-made BFL using `asm` blocks:

- walking a 1 000-node linked list;
- copying a 1 000-byte string;
- reading a field from a 1 000-element array of 8-byte records at random indexes.

For each layout record generated commands, IR instructions, x64 bytes and run
time on the JIT and the interpreter. Pick the layout, or a hybrid such as flat
for heaps up to 4 KB and paged above. Write the result into docs/HEAP.md.

**Done when** the numbers are in docs/HEAP.md and the choice is recorded. If both
layouts are too slow for the list walk, stop and rethink before phase 3.

## 3. Phase 1: typed front end

**Goal:** a semantic pass between parsing and code generation that resolves
names, assigns a type to every expression and reports errors, so the code
generator only sees valid, typed programs.

- A `Type` representation in `src/lang/types.h`: integer kinds with width and
  signedness, array of T, pointer to T, struct, procedure. Phase 1 only uses
  `byte` and `word`; later phases add the others without restructuring.
- A `sema.cpp` pass that annotates the syntax tree: resolved symbol per name,
  type per expression, constant value where known, and which procedures are
  trampolined (moved out of `plan_trampolines`).
- Parser error recovery: after an error, skip to the next `;` or `}` and carry
  on. Report up to 20 errors, each `file:line:col: message`.
- Warnings, off by default in the CLI (`--warn`) and shown in the UI status bar:
  unused variable, value truncated to a byte, unreachable code after `return`.
- The UI lists all errors and lets F4 jump to the next one.

**Tests:** every existing case passes, and every golden file is byte-for-byte
unchanged. That proves the pass changed no code. New cases cover multiple errors
in one file and each warning.

## 4. Phase 2: integer types

**Goal:** signed and 32-bit arithmetic with the same operator set as today.

- Types: `byte` and `word` unchanged; `sbyte` (signed 8), `int` (signed 16),
  `dword` (unsigned 32), `long` (signed 32). All wrap on overflow.
- Code generation becomes width-generic. The 16-bit routines (add with carry,
  compare, multiply, divide) become N-byte routines, and words become the N = 2
  case.
- Signed comparison flips the sign bit of both operands and compares unsigned.
  Signed division truncates towards zero, and `%` takes the sign of the dividend,
  as in C. `>>` is arithmetic on signed types.
- Conversion rules as in C: the smaller operand widens; mixing signed and
  unsigned of the same width is unsigned and gives a warning; narrowing needs an
  explicit cast and otherwise warns.
- Literals: negative constants, a `L` suffix for 32-bit, and range checks.
- `print` handles negatives and 32-bit values; `input` gets signed and 32-bit
  forms.

**Tests:** the existing arithmetic grids extended to every type, with expected
values from Python (`ctypes` integer semantics), in all three execution modes.

**Cost to watch:** a 32-bit multiply or divide is large. Record generated size
and speed for each type in docs/LANG.md, as the current cost table does.

## 5. Phase 3: heap and pointers

**Goal:** memory reached through addresses.

- The heap sits above every statically allocated cell, using the layout chosen in
  phase 0. Size is set by `heap 16384;` at the top level, with a default of 8 KB.
  Addresses are 16-bit, and 0 is null.
- Syntax, C-like:
  - `byte* p;` declares a pointer; pointers are 16-bit values that can be copied,
    compared, passed and returned.
  - `*p`, `p[i]` and `p + n` read, index and offset, scaled by the size of T.
  - `heap byte buf[100];` declares a static array in the heap, so `buf` has an
    address. Plain variables stay in cells and have no address, so `&x` is an
    error that names this rule.
- Every heap access compiles to the phase 0 routine. A `--check-heap` option
  adds a null and bounds check that halts with a message, for debugging.
- The memory viewer gets a "Heap" button and shows addresses in heap terms.

**Tests:** pointer arithmetic, null comparisons, a hand-built linked list, and
aliasing (two pointers to one byte).

## 6. Phase 4: structs and richer arrays

- `struct Point { int x; int y; }` defines a layout with fixed field offsets.
- A value struct in cells (`Point p;`) is a group of consecutive cells. It can
  be assigned whole, passed by value, returned, and its fields used as `p.x`.
- A heap struct is reached through a pointer: `Point* q;` then `q->x`.
- `word`, `int`, `dword` and struct arrays: `heap int scores[50];` and
  `heap Point pts[20];`. Arrays of wider elements in cells use one byte array per
  byte of the element, with no new walking idiom.
- `sizeof(T)` is a compile-time constant.

**Tests:** struct copy and compare, nested structs, an array of structs sorted by
a field, and structs passed to and returned from procedures.

## 7. Phase 5: allocator and strings

- `alloc(n)` returns a `byte*` or null, and `free(p)` releases it. They are written
  in BFL as a first-fit free list with a 2-byte size header, kept in the heap
  itself. `new T` and `new T[n]` are typed forms of `alloc`.
- `string` is shorthand for `byte*` to NUL-terminated bytes. String literals
  are placed in the heap by start-up code, so `string s = "hello";` works.
- A strings library: length, copy, compare, concatenate into a buffer, find a
  character, number to text and text to number for each integer type.
- `print` of a `string` prints the text.

**Tests:** allocate and free in patterns that fragment the heap, string
round-trips, and a word-frequency count over the input using a hash table, as a
realistic program.

## 8. Phase 6: imports and the standard library

- `import "path.bfl";` includes another file once, resolving the path relative
  to the importing file, then a `lib/` directory next to `bf.exe`.
- Names stay global. A clash between files is an error that names both files.
  Namespaces wait until a program needs them.
- The standard library moves into `lib/`: the current prelude (decimal print and
  input), plus `strings.bfl`, `memory.bfl` (allocator), `math.bfl` (min, max,
  abs, gcd, integer square root) and `io.bfl` (read a line, read a number).
- The statement map gains a file index per statement, so the debugger knows
  which file a statement came from.

**Tests:** import chains, a cycle (imported once, no error), clash errors, and
every library routine.

## 9. Phase 7: recursion on the heap

The trampoline currently saves frames to a dedicated stack array and keeps
locals in fixed cells. With a heap the frame can live in memory:

- the call stack is a region at the top of the heap, growing down;
- a `rec proc` can own arrays and structs, which live in its frame;
- array parameters work, passed as pointers;
- `ref` parameters become pointers to heap data, so copy-in and copy-out go away
  for heap values (cell variables still use copy-in and copy-out).

Also in this phase:

- **The 254-block limit.** The state cell is a byte, so a program with many
  recursive procedures will hit 254 blocks. The state becomes 16-bit, with the
  dispatcher testing the high byte first.
- **Stack overflow.** An optional `--check-stack` check halts with a message.

**Tests:** quicksort on a heap array passed as a pointer, a recursive tree built
with `new`, and a recursive procedure with array locals.

## 10. Phase 8: tooling

- **Remember settings** between runs in `%APPDATA%\bfmachine\settings.ini`:
  newline mode, terminal speed, interpreter choice, window size and splitter
  position, last file.
- **Terminal width.** The memory viewer becomes collapsible and the terminal
  gets a column count option (80, 120, fit). `hanoi.b` needs about 120 columns.
- **Syntax colouring** for `.bfl` in the editor: keywords, types, numbers,
  strings, comments and register names. RichEdit formatting is already used for
  breakpoints, so the colouring has to leave the breakpoint background alone.
- **Multi-file debugging.** When the debugger stops in an imported file, the
  editor shows that file read-only with the current statement highlighted, and a
  file list lets you set breakpoints in imported files.
- **Error list.** A pane under the editor listing compiler errors and warnings;
  double-click jumps to the line.

As built: the settings file also keeps the check options, terminal width, pane
visibility and the generated-code view mode, and `BFUI_SETTINGS` can point at
another file. The terminal width option offers 80, 120 and 132 columns; with a
fixed width the window grows to fit when the screen allows. Colouring goes
through the RichEdit Text Object Model with undo suspended, so it neither
touches breakpoint backgrounds nor fills the undo history. Watch rows show
multi-byte, signed and pointer values whole. Run > Check heap accesses and Run >
Check recursion stack match the `bf` flags.

## 11. Phases 9 to 14 (built)

After phase 8 the optimiser learnt loop idioms (multi-byte carries, compares,
halving and byte division each run in one step), known cell values carried
across blocks, automatic layout of globals, one-walk array elements, folding of
constant arguments and `shared proc` (README.md "Speed", docs/LANG.md). Where
that left things, measured on `programs/samples/raytracer.bfl` when this section
was planned: a frame took 70 ms (2.3 s before the idioms), the program was 5.67 M
brainfuck commands (7.92 M before), a 32-bit `*` executed about 2 400 IR
instructions and a `/` about 7 000, and long arithmetic was most of both the code
and the run time. The phases below were planned from there; each has a **Built**
or **Done** note saying what was made.

Where they leave things, measured with `tools/bench.py` against the commit before
phase 10 (frame times are the fastest of three runs, both builds timed one after the
other on an otherwise idle machine):

| | before | now |
|---|---:|---:|
| `raytracer.bfl`: ms a frame / light map / commands | 71.0 / 167 / 5.67 M | 55.0 / 86 / 4.06 M |
| `raytracer_text.bfl` | 32.4 / 162 / 5.18 M | 23.7 / 81 / 3.59 M |
| `cube.bfl`: ms a frame / commands | 1.38 / 537 K | 1.33 / 313 K |
| `starfield.bfl` | 1.17 / 1.81 M | 1.07 / 1.79 M |
| `long / long`, IR instructions / commands | 7 147 / 15 776 | 201 / 7 467 |
| `int / int` | 2 301 / 5 826 | 200 / 3 683 |
| `word / word` | 2 198 / 3 721 | 81 / 1 946 |
| `(a * b) >> 12` on `long`, as `mulshr(a, b, 12)` | 1 893 / 18 077 | 1 088 / 11 864 |
| `lc = fa * fb + la * ld - lb * fa;` (`BFL_SHARE=0` before) | 4 768 / 34 540 | 4 812 / 14 936 |

A 32-bit `*` is unchanged (1 700 to 2 400 IR instructions, more for larger
operands); what changed is the division loop (phase 12), fixed point through
`mulshr` (phase 10) and one copy of the code for repeated arithmetic (phase 11).
Of the other samples only `text.bfl` grew (470 K to 486 K commands; phase 12 says
why). `tests/fuzz.py` (phase 9) checks the compiler, the optimiser and the JIT
against each other and a model of BFL: `--bfl 4000 --bf 2000` finds nothing.

### Phase 9: differential fuzzer for the optimiser and JIT

The loop idioms, the block rebuilding and the inline idiom code in the JIT are
the most intricate parts of the executor, and each is checked by a handful of
test programs. `tests/fuzz_shared.py` (shared against inlined procedures, part of
`tests/fuzz.py` since phase 19) found a miscompile on its first run that no
hand-written test had: a dangling pointer that made the same source compile
differently now and then.

- Generate random BFL programs over every integer type: arithmetic, shifts,
  compares, casts, arrays in cells and in the heap, loops with `break` and
  `continue`, inlined procedures. Generate raw brainfuck too, with poked
  scratch cells, as the idiom tests do.
- Run each through the JIT, the IR interpreter and the unoptimised JIT (the
  reference: every command as written), with idioms and block rebuilding on and
  off (`BF_NO_IDIOMS`, `BF_NO_REBUILD`); for BFL arithmetic also check against
  values computed in Python.
- Check that compiling the same source twice gives the same brainfuck.
- Shrink failing programs automatically, as `reduce_nd.py` did by hand.
- A few seeds run in the test suites; long runs on demand.

Done when: a long run (thousands of programs) finds nothing, and every bug it
found has a minimal regression test.

**Built** as `tests/fuzz.py`. The BFL generator builds a program tree, and a
model of the language in Python runs that tree: sema's typing rules, constant
folding, conversions, division and shifts. The tree is printed as source,
compiled twice (the two outputs must be identical), compiled once more with
`BFL_NO_LAYOUT`, and run by eight executors: the JIT and the interpreter, each
optimised, unoptimised, and without idioms or without block rebuilding. Each
output is compared with the model's. The brainfuck is also run with a cell of
the program poked first and compared with the unoptimised run. Programs include
recursive procedures as well as inlined ones, and pointers into heap arrays. The
brainfuck generator aims at multiplies, ifs, carries and scans, and is checked
against a Python interpreter. Failures are shrunk: statements and expressions
for BFL, delta debugging for brainfuck. `tests/test.py` runs 60 brainfuck seeds
and `tests/test_lang.py` runs 16 BFL seeds.

All the bugs it found were in the compiler, not the optimiser or the JIT. Each
has a case in `tests/test_lang.py`:

- A shift count was taken modulo 256 at run time, converted to the left
  operand's type when known at compile time, and ignored when negative, so
  `b << w` with `w` a `word` holding 256 gave `b`. Sema's folding of a negative
  constant count was undefined behaviour in C++, and turned into wrong constants
  and spurious "does not fit" errors. A count that is negative or at least the
  width now shifts everything out everywhere (`shift-count-out-of-range`).
- A sum folded nested `+` and `-` into one list of terms across types, so in
  `long - (byte - 1)` the byte part never wrapped (`narrow-part-of-wide-sum`).
- `break` and `return` in a `for` loop still ran the step, so a counter read
  after `break` was one too high. In a trampolined procedure the step wrote a
  frame cell that the return had just cleared, and the stale cell later
  corrupted a result (`for-break-skips-step`).

The final run of 5000 BFL and 3000 brainfuck programs (seed 100000) found
nothing. A later run hung on one program (seed 17897) in every executor: its
nested recursion needed more than the default 128 cells of stack, and an
overflow is not checked at run time, so the program wrote over other cells. The
fuzzer now runs a program with `rec` procedures once with `--check-stack` and,
when it overflows, gives it `stack 4096;` (which also exercises the 16-bit stack
pointer), or skips it if even that overflows (`fuzz-stack` in `tests/test.py`).

### Phase 10: fused multiply-and-shift

Fixed-point code is full of `(a * b) >> 12`: a full 32-bit product, then a
shift. Each such statement in the raytracer compiles to 40 000 to 70 000
commands. A built-in (for example `mulshr(a, b, 12)`, or a recognised pattern)
computes only the bytes of the product that survive the shift: fewer
partial products, no shift afterwards.

Done when: the raytracers, `cube.bfl` and `starfield.bfl` use it, with sizes and
frame times measured before and after, and the grids test it against Python for
every type and shift count.

**Built** as the built-in `mulshr(a, b, k)` (a keyword; docs/LANG.md, "Fixed point:
mulshr"). The product is exact, twice the width, rounded down by the shift, so it
equals `(a * b) >> k` whenever the product does not overflow; `k` is a constant and
counts of the width and more take the high half, then 0 or -1. The code
(`mulshr` in `src/lang/codegen.cpp`) is one shift-and-add loop over the bits of `|b|`
with an accumulator of `n + k/8 (+1)` bytes: after each byte of `b` the accumulator's
low byte is final and is dropped with byte moves while whole bytes of the shift
remain, the last `k % 8` bits are one pass of byte divisions (the DIVMOD idiom), and
a signed product is formed from magnitudes with the accumulator starting at `2^k - 1`
when it is negative, so negating the rounded-up magnitude gives the floor. The
pattern `(a * b) >> k` is not rewritten, since its wrapping product is defined
behaviour. Tests: a grid per integer type in `tests/test_lang.py` against Python with
every count from 0 to twice the width plus 6 (0 to 70 for 32-bit types), targeted
cases (constant folding, operand order, known values, a `rec` call inside, errors),
and `MulShr` in the fuzzer's generator and model; `--bfl 1500 --bf 300` ran clean.

Measured with `tools/bench.py` against the commit before:

| | before | after |
|---|---:|---:|
| `(long * long) >> 12` → `mulshr`, IR instructions | 1 893 | 1 088 |
| the same, commands | 18 077 | 11 864 |
| `(int * int) >> 7` → `mulshr`, IR instructions | 690 | 487 |
| the same, commands | 9 963 | 4 932 |
| `raytracer.bfl`: ms a frame / light map / commands | 71.0 / 167 / 5.67 M | 65.0 / 160 / 5.12 M |
| `raytracer_text.bfl` | 32.4 / 162 / 5.18 M | 29.6 / 155 / 4.64 M |
| `cube.bfl`: ms a frame / commands | 1.38 / 537 K | 1.38 / 388 K |
| `starfield.bfl` | 1.17 / 1.81 M | 1.16 / 1.80 M |

The raytracers use it for every single product and for the dot products (a sum of
`mulshr` terms rounds each term down: rendered headless against the commit before,
0.8 to 2.2% of a frame's pixels change, 1.5% over 16 frames; two thirds of those by
one level of a colour channel (a dither step), the rest by up to 7 levels, mostly in
the spheres' reflections, where a ray now meets a slightly different point; a lamp
moves by a pixel in three of the 16 frames (4, 8, 12). One change is visible: in
frame 0 a small pink patch at the upper left of the chrome sphere is gone. Since the
red sphere is in front of it and to the right, that patch was probably an artefact of
a 32-bit product that overflowed before the shift, which the exact product no longer
does; this has not been traced to a statement).
`cube.bfl` rotates in `int`s, because the exact product no longer needs `long`s, with
shifts instead of `/ 1024`: a corner sometimes lands one cell over, since the shift
rounds down where the division truncated. `starfield.bfl` has no fixed-point products
in its frame (its perspective divides by depth); its random numbers now scale the
generator's high half by `mulshr(r, n, 16)` instead of `% n`, a different but equally
random sky.

Found along the way:

- A raytracer statement's 40 000 to 70 000 commands are mostly the trips to its
  operands: a global 900 cells from the temporaries costs about 4 x 900 commands per
  byte copied. `mulshr` removes the multiply's and the shift's own code (about a
  third of the bench statement), not those trips; the samples shrink by 10%.
- The IR's loop idioms match a carry chain of at most 5 cells (24 cells and 40 000
  verification points in `src/ir.cpp`); a 6-byte accumulator's chain ran as written
  and was 7 times slower, so the wide add ripples a carry cell below the top 5.
- `mulshr(a, b, 0)` is `a * b` in about 60% of the IR instructions of `*` (no operand
  compare and swap, doubling by adding instead of `shlN`), and the one-pass byte
  division shift would make every constant `>>` and `<<` of more than one bit much
  shorter (`fa >> 12` is 6 400 commands today). Both are left for the phases that
  rework arithmetic (11, 12).

### Phase 11: arithmetic shared within a block

Most of the remaining size is inlined `*` and `/`: `place_camera` in the
raytracer is 1.6M commands of straight-line arithmetic. `shared proc` cannot fix
this, because sharing through the trampoline makes every caller a trampoline
too (docs/LANG.md, "When shared pays"). Instead, when a block or statement holds
several multiplies (or divides) of the same width, emit one copy of the routine
inside a small local loop that runs once per operation, choosing operands and
destination by a counter. No dispatcher, no frames, and known values survive
outside the loop.

Done when: measured on the samples; kept only where it makes code smaller
without making frames noticeably slower.

**Built** in `src/lang/codegen.cpp` ("arithmetic shared within a block"; docs/LANG.md,
"Shared arithmetic"). An operation's routine is its operator (`*`, `mulshr`, `/`, `%`),
width, signedness and `mulshr`'s count; a constant factor is not shared, since it becomes
shifts and adds. `run_shared` emits the routine once inside a loop on a counter cell that
runs once per operation, with a flag per operation choosing the code after the routine:
it takes the result and evaluates the next operation's factors into the operand cells.
That code is generated in statement order and placed last-first in the loop, so that the
flag it sets for the next operation is not seen until the next pass. Two places use it:

- The terms of one sum (`shared_terms`, from `binary_wide`): the products go straight into
  the sum's cells, through a sign flag when some are subtracted. This works wherever a sum
  is evaluated: a condition, a call's argument, a shift's operand.
- A run of assignments and declarations without calls (`find_run`, `gen_run`, from
  `gen_stmts`): every such operation the statements evaluate at full width (a term of a
  sum, inside a shift or a cast, another operation's operand) is computed into cells of
  its own when the loop reaches it, and once a statement's operations are done the
  statement itself is generated, reading those cells instead of computing them
  (`precomputed`). Declared variables get their cells before the loop. Up to two
  statements without such an operation may sit between two that have one.

The first operation's factors are evaluated before the loop and the last statement after
it, with the known cell values of straight-line code, and the values known before the loop
that no statement in it writes are known again after it. The statement map gives each
statement of a run a range where it starts (step marks and breakpoints); the loop itself,
routine included, is a continuation of the run's first statement, so `bf --profile`
charges the routine to that line, and breakpoints no longer stop at continuations
(`pcs_for_line` in `src/ui.cpp`).

The heuristic: share whenever a routine would otherwise be emitted at least twice in one
sum or one run (`Options::share`; `BFL_SHARE=N` in the environment of `bf` sets the
minimum, 1 counts as 2, and 0 or less turns it off). Two limits came from review:

- One loop runs at most 16 operations (`SHARE_MAX`). The flags and operand cells lie past
  the variables, so the code between them grows with the count and the loop's size with
  its square; the pass counter is also one cell, which wrapped at 256 operations and gave
  wrong results. A sum with more is cut into even loops of at most 16, and a run ends
  before the statement that would pass 16. Size with sharing minus size without, in
  commands, for a few caps:

  | | cap 8 | cap 16 | cap 32 | cap 64 | no cap |
  |---|---:|---:|---:|---:|---:|
  | sum of 128 `long` `*` | -1 108 K | -1 135 K | -1 074 K | -892 K | -500 K |
  | sum of 128 `word` `/` | -146 K | -133 K | -88 K | +11 K | +215 K |
  | run of 32 `x = x + x * b;` on `word` | -30 K | -21 K | +1.7 K | +1.7 K | +1.7 K |
  | run of 64 `x = x + mulshr(x, b, 7);` on `long` | -441 K | -433 K | -357 K | -177 K | -177 K |

  16 is best or close to it for 32-bit operations (16-bit runs do better at 8), and the
  samples are the same at any cap from 16 up; at 8 the raytracer is 10 K commands larger.
- An operation that reads a variable whose value the compiler knows ends a run before its
  statement (`reads_known`, against the values known before the run less the variables
  its earlier statements assign). Generated outside the loop it folds or uses a constant
  factor, and its result stays known: `long k = 100000; long m = 7; long x = k / m; ...`
  had been 256 556 commands shared against 192 358 without sharing, and is now the same.
  The other statements of a run still lose the values the compiler knows, since they run
  inside the loop. (Phase 18 gives them back.)

Variants measured on the samples (only these four change):

| | raytracer commands | ms a frame | raytracer_text | cube | starfield |
|---|---:|---:|---:|---:|---:|
| off | 4.80 M | 57.3 | 4.31 M / 25.0 | 341 K / 1.35 | 1.80 M / 1.09 |
| sums only | 4.34 M | 55.5 | 3.85 M / 24.1 | 325 K / 1.35 | 1.80 M / 1.08 |
| sums and runs, no statement between | 4.07 M | 54.3 | 3.59 M / 23.5 | 313 K / 1.33 | 1.79 M / 1.09 |
| sums and runs, up to 2 between (kept) | 4.06 M | 54.7 | 3.59 M / 23.3 | 313 K / 1.32 | 1.79 M / 1.07 |
| up to 4 between | 4.06 M | 54.4 | 3.59 M / 23.6 | 313 K / 1.32 | 1.79 M / 1.06 |
| at least 3 operations | 4.14 M | 54.5 | 3.65 M / 23.5 | 317 K / 1.35 | 1.80 M / 1.09 |
| no divisions | 4.15 M | 56.1 | 3.67 M / 23.8 | 317 K / 1.34 | 1.80 M / 1.09 |
| multiplies only | 4.48 M | 55.7 | 4.00 M / 24.5 | 341 K / 1.35 | 1.80 M / 1.08 |
| `long` only | 4.16 M | 55.3 | 3.68 M / 23.8 | 341 K / 1.36 | 1.80 M / 1.09 |

Every kind of operation and both widths pay, and no variant made a frame slower; the
frame times differ by about 1 ms from run to run.

Tests: `share-*` cases in `tests/test_lang.py` (sums of each kind with mixed signs, runs of
each kind, operations apart in one statement, known values kept and dropped, a run in a
loop with `continue` and `break`, in a `rec proc`, a sum of 300 and a run of 130
statements), `share-map`: a run is smaller than with `BFL_SHARE=0`, prints the same, has a
step mark for each statement and `bf --profile` counts each line, and `share-setting`:
`BFL_SHARE` of 1 and -1 compile like 2 and 0, and operations on known values are the same
size with and without sharing. The fuzzer generates runs of statements with several operations of one
kind (`shared_run`) and runs every program compiled with `BFL_SHARE=0` as well; `--bfl 2000
--bf 300` (seed 20000) found nothing, and 1 151 of the 2 000 programs shared arithmetic; after
the two limits, `--bfl 800 --bf 300` (seed 31000) found nothing, with 439 sharing.

Measured with `tools/bench.py` against the commit before (after phase 12):

| | before | after |
|---|---:|---:|
| `raytracer.bfl`: ms a frame / light map / commands | 57.8 / 88 / 4.80 M | 54.1 / 85 / 4.06 M |
| the same, `place_camera` (commands) | 1.15 M | 0.88 M |
| `raytracer_text.bfl` | 25.0 / 84 / 4.31 M | 23.5 / 81 / 3.59 M |
| `cube.bfl`: ms a frame / commands | 1.36 / 341 K | 1.32 / 313 K |
| `starfield.bfl` | 1.09 / 1.80 M | 1.06 / 1.79 M |
| `lc = fa * fb + la * ld - lb * fa;`, IR instructions / commands | 4 768 / 34 540 | 4 812 / 14 936 |
| `lc = mulshr(fa, fb, 12); lc = mulshr(lc, la, 12) - mulshr(lb, ld, 12);` | 3 863 / 37 154 | 4 007 / 19 289 |

(The two operation rows are new in the benchmark; "before" is this build with
`BFL_SHARE=0`.) The raytracer's light map and two frames execute 789 M IR instructions
instead of 777 M (1.5% more: the loop's flags, the moves into the operand cells, and
factors evaluated inside the loop without known values), but its machine code is a third
smaller (3.1 MB of x64 instead of 4.6 MB, 206 K IR instructions instead of 302 K), and the
frames got faster rather than slower. Against the commit before phase 10 the raytracer's
code went from 5.67 M to 4.06 M commands and its frame from 71 to 55 ms.

### Phase 12: faster division

Division is the costliest operation (about 7 000 IR instructions for 32 bits)
and the top of the raytracer's profile (`isqrt`, normalising vectors). Options to
measure: recognise the whole bit-by-bit division loop as one idiom; divide a byte
at a time; skip more leading zero bits of the divisor and dividend.

Done when: `/` and `%` are measurably cheaper in executed instructions and the
raytracer frame time drops, with the grids green.

**Built** as an idiom recognised by its text (`IDIOM_DIVIDE`; "the division loop" in
`src/ir.cpp`, `bf::division_loop` and `bf::DivisionLayout` in `src/bf.h`). The symbolic
execution behind the other idioms cannot take this loop, whose body holds byte loops
with data-dependent trip counts. Instead the compiler emits the loop from a generator in
the executor, and the optimiser compares the commands of each loop with its text for 2
and 4 bytes, the widths BFL divides: a loop with exactly that text computes the rule
wherever it appears. The other cells it touches are guards, so a loop entered with one
of them non-zero runs as written. The rule, in the interpreter and through the same call
in the JIT, is up to 255 shift-and-subtract steps on 64-bit values. The loop was
rewritten for this: `A`, `R`
and a borrow byte `X` get a group of six cells each (the byte, two cells for a zero
test, a counter, and in R's groups B's byte and a temporary). A pass shifts `A:R` high
byte first (a byte counts its old value up twice, and when it wraps the byte above gets
the carry), subtracts `B` with a borrow chain into `X`, and adds `B` back on a borrow.
The zero test `t+ c[>-]>[- … >]` does not copy the cell, so the loop as written is
linear in the values. The skip over the dividend's whole zero bytes is gone, since the
idiom does every pass in one step. Tests: `word-divide` and `long-divide` in
`IDIOM_PROGRAMS` (`tests/test.py`), poked at every cell of the loop as well; the typed
grids; and a check of the rule against the text on random states (counter, `A`, `R`,
`B` and poked scratch cells) for every width from 2 to 8, run while building it. The
fuzzer ran `--bfl 1500 --bf 500` clean, and `--bfl 400 --bf 200` with `BF_NO_IDIOMS=1`.

Measured with `tools/bench.py` against the commit before (after phase 10):

| | before | after |
|---|---:|---:|
| `long / long`, IR instructions | 7 147 | 201 |
| the same, commands | 15 776 | 7 467 |
| the same, unoptimised: commands executed | 27.2 M | 0.85 M |
| the same with `BF_NO_IDIOMS`, IR instructions | 121 200 | 290 500 |
| `int / int`, IR instructions / commands | 2 301 / 5 826 | 200 / 3 683 |
| `word / word` | 2 198 / 3 721 | 81 / 1 946 |
| `raytracer.bfl`: ms a frame / light map / commands | 65.0 / 160 / 5.12 M | 57.7 / 91 / 4.80 M |
| `raytracer_text.bfl` | 29.6 / 155 / 4.64 M | 25.4 / 85 / 4.31 M |
| `cube.bfl`, `starfield.bfl`: ms a frame | 1.38, 1.16 | 1.37, 1.10 |

The unoptimised count is for the benchmark's operands (`892200 / 1006`); the loop's
passes cost more the larger the values, so the loop alone runs from about 0.4 M commands
(`123456789 / 10`) to 2.8 M (`0xFFFFFFFF / 0xFFFFFFFF`).

A raytracer frame executes 181 M IR instructions instead of 225 M: its 8 000 divisions
took about 45 M and now take 1.6 M. (Against the commit before phase 10 a frame went
from 71 to 58 ms.) `text.bfl` grows by 3% although its `dword / 10` shrinks from 10.7 K
to 4.1 K commands: the loop's 50 cells raise the temporaries' high-water mark, so the
heap sits 38 cells further up and every heap access walks there and back (pointer moves
only, which fold into the moves already there: its IR is 3% smaller).

The other options, measured on the operands of every division in the raytracer (logged
by an instrumented build), would have saved much less at about 300 IR instructions a
pass:

- Skipping the dividend's leading zero bits as well as its whole zero bytes removes 2.8
  of the 18.8 passes of a frame's average division (15%).
- Starting where the remainder can first reach the divisor (the difference of their bit
  lengths) removes 10 passes (53%).
- Dividing a byte at a time applies only when the divisor fits in a byte (63% of a
  frame's divisions, 12% of the light map's), and each step divides 16 bits by 8, which
  the byte division idiom does not do.

### Phase 13: the profiler in the UI

`bf --profile` lists the BFL statements that execute the most instructions. In
the UI: Run > Profile runs the program on the interpreter, then colours the
editor's lines by their share of instructions (a heat scale) and lists the top
statements in a pane; double-click jumps to the line. Multi-file programs show
imported files' counts in the file viewer.

Done when: profiling a sample in the UI agrees with `bf --profile`.

**Built** as Run > Profile (Ctrl+P; README.md "Profiler", the UI's Help). It runs the
editor contents on the interpreter with `Machine::profile` set. When the program ends,
or Shift+F5 stops one that runs forever, a gutter at the left of the editor colours each
line by its share of the executed IR instructions (pale yellow to red) and prints the
percentage, and a pane under the editor lists the lines that executed the most;
double-clicking a row goes to the line. For a multi-file program the File box shows each
file's share, and an imported file picked there has the gutter too. For brainfuck the
list gives each instruction's line and column. The next edit or run clears the profile.
The attribution to statements moved from `src/cli.cpp` into `src/profile.h`, which
`bf --profile` and the UI share, so the two give the same counts: `queens.bfl` 1 293 774
instructions, 46.8% outside statements, line 46 242 868; `maze.bfl` 4 289 120, with
`queue.bfl:30` at 9.0% and the File box at 81.6% for `maze.bfl`. Ctrl+P typed in the
terminal while a program runs goes to the program (as key 0x10), not to the profiler.

Testing it showed that a fault on the interpreter reported a garbage cell (the cell index
was stored where `Machine::run` expects an address). The fix for faults past the tape's
end (phase 12's review) corrected the same thing, so profiling a run that leaves the tape
now reports the cell it touched, as the JIT does. Still not counted: instructions run
through the literal program while the pointer can reach the device window
(`src/interp.cpp`), so such programs under-report, in the UI and in `bf --profile` alike.
(Phase 18 counts them.)

### Phase 14: housekeeping

`docs/raytracer.gif` (16 frames of the raytracer) is not committed and was
recorded when a frame took 2.3 s. Re-record it now that a frame takes 70 ms, and
commit it with a mention in README.md, or drop it.

**Done**: `tools/rt_gif.py OUT.gif` renders the 16 frames headless (2 s in all, a
frame about 55 ms); README.md shows the re-recorded `docs/raytracer.gif`.

## 12. Phases 15 to 19 (built)

Where phases 9 to 14 leave things (`tools/bench.py`): a raytracer frame takes 55 ms
and its light map 86 ms, the program is 4.06 M commands, a 32-bit `/` executes about
200 IR instructions, and a 32-bit `*` still 1 700 to 2 400. The phases below were
picked from that profile and from the open points the Built notes of section 11
list.

### Phase 15: multiplication as one IR step

Division became one idiom in phase 12 (7 147 to 201 IR instructions); multiplication
is now the costliest operation and the raytracer is mostly products and `mulshr`s.
Do the same for it: emit the shift-and-add loops of `*` and `mulshr` from one
generator in the executor (as `bf::division_loop` does), recognise a loop whose
commands are exactly that text, and run all its passes in one step in the JIT and
the interpreter, behind the same entry checks of the scratch cells as the other
idioms. Cover it with `IDIOM_PROGRAMS` cases in `tests/test.py` (including pokes)
and the fuzzer.

Done when: a 32-bit `*` and `mulshr` execute a few hundred IR instructions or fewer,
the raytracer frame time drops, and the grids and a long fuzz run are clean.

**Built** as an idiom recognised by its text, as phase 12 did for division
(`IDIOM_MULTIPLY`; "the multiply loop" in `src/ir.cpp`, `bf::multiply_loop` and
`bf::MultiplyLayout` in `src/bf.h`). One generator gives the loop for factors `A` and `B`
of `n` bytes and a product `P` of `w` bytes (`n <= w <= 2n`); the optimiser matches it
for `n` = 1, 2 and 4 and every `w`. Each pass doubles `P` (high byte first, the top bit
dropped), shifts `B` left by one bit into a cell `X`, and when `X` is set adds `A` to `P`,
each byte of `A` counted out with a carry chain up to `P`'s top; after `8n` passes `P` is
`A * B` mod 256^w and `B` is 0. The block is the counter, five cells per byte of `P` below
`n` (the byte, two for its zero test, a counter, a byte of `A`) and four above, four per
byte of `B`, and `X`: `9n + 2` cells for `*`, up to `13n + 2` for `mulshr`. The rule takes
any pass count (the top bits of `B` times `A`, added to `P` shifted); the JIT does the
compiler's `8n` passes inline (the scratch cells or-ed together and tested, then
`P << 8n + A * B` in a 64-bit register) and other counts through `idiom_fn`.

In the compiler, `mulN` is the loop with `w = n`: the wrapped product is the same signed or
not, so the compare and swap of the operands and their signs are gone. `mulshr` multiplies
the magnitudes with `w = min(2n, n + k/8 (+1 when k%8))`, drops the bytes below `k/8`,
shifts the last `k % 8` bits with one pass of byte divisions as before, and for a negative
product gives `~h` when a bit shifted out was set and `-h` otherwise (`h` the shifted
magnitude). A constant factor of 8 or more is put into `B` and runs the loop as well
(`mul_constN`), which is smaller and faster than the unrolled shifts and adds from there
on: `long * 41` 380 to 85 IR instructions and 8 800 to 5 200 commands, `long * 1103515245`
2 213 to 85 and 47 800 to 5 400, `int * 12` 91 to 59 and 1 600 to 1 350 (below 8 the
unrolled code is smaller, at most twice as slow). The byte `*`, `x[- y[- d+ t+] t[- y+]]`,
was a loop of 3 IR instructions a unit of `x` once its inner loops became multiply-adds; a
loop whose body is multiply-adds from cells it does not write and one decrement of the
loop cell is now `IDIOM_PRODUCT` (each `d += s * f * x`, inline in the JIT).

Tests: `word-multiply`, `long-multiply` (`w` = 2, 3, 4, 6 and 8, `n` = 1, 2 and 4) and
`byte-multiply` in `IDIOM_PROGRAMS`, poked at every cell of the loops, and the two
multiply programs cut off by the end of RAM in the fault test (now
`fault-idiom-loops-at-end`); `bf_product` in the fuzzer's brainfuck generator (12% of its
programs get a product idiom); and a check of the rule against the text on 4 600 random
states (counter, `P`, `A`, `B`, and in a quarter of them a poked scratch cell) for every
`n` and `w`, the loop as written against the JIT and the interpreter, run while building
it. The fuzzer ran `--bfl 2000 --bf 500` and `--bfl 1500 --bf 1000` clean (it runs every
program with `BF_NO_IDIOMS` as well). The raytracer's 16 frames rendered headless are
identical to the commit before.

Measured with `tools/bench.py` against the commit before (frame times: the fastest of
nine runs, the two builds run alternately, three rounds each, on a machine shared with
other work):

| | before | after |
|---|---:|---:|
| `long * long`, IR instructions / commands | 1 695 / 10 937 | 85 / 6 025 |
| the same, large operands (wraps) | 2 368 / 8 465 | 85 / 5 665 |
| `int * int` (the statement's own commands) | 470 / 3 725 | 59 / 1 474 |
| `byte * byte` | 610 / 127 | 14 / 127 |
| `mulshr(long, long, 12)` | 1 088 / 11 864 | 191 / 7 232 |
| `mulshr(int, int, 7)` | 487 / 4 932 | 158 / 3 829 |
| `(long * long) >> 12` | 1 893 / 18 077 | 264 / 13 165 |
| `lc = fa * fb + la * ld - lb * fa;` | 4 812 / 14 936 | 299 / 10 042 |
| the same with `BFL_SHARE=0` (own commands) | 4 768 / 35 388 | 261 / 20 652 |
| `long * long` with `BF_NO_IDIOMS`, IR instructions | 35 435 | 103 522 |
| `raytracer.bfl`: ms a frame / light map / commands | 58.3 / 90 / 4.06 M | 42.5 / 43 / 3.66 M |
| `raytracer_text.bfl` | 25.5 / 85 / 3.59 M | 16.0 / 37 / 3.18 M |
| `cube.bfl`: ms a frame / commands | 1.41 / 313 K | 1.39 / 252 K |
| `starfield.bfl` | 1.15 / 1.79 M | 1.09 / 1.23 M |

A raytracer frame executes 124 M IR instructions instead of 188 M (and 128 000 multiply
loops), and the start (the light map) 175 M instead of 412 M. Of the other samples
`bst.bfl` is 4.6% smaller, `numbers.bfl` 1%, `life.bfl` 0.1% larger; the rest change by
less.

Found along the way:

- The JIT's inline idioms (carry chains, saturating subtract, halving) read every scratch
  cell and touch every cell of a carry chain, where the loop as written may stop short: a
  loop near the end of the tape faulted at once, losing output the interpreter and the
  unoptimised run had printed (the new fault case, `long-multiply` with `--ram 70`). The
  inline code now compares the idiom's highest cell with `Ctx::tape_end` (offset 80) and
  runs the loop as written past it. (Phase 17 replaced the compare: the fault handler
  now resumes such a loop as written.)
- Without the idioms (`BF_NO_IDIOMS`) the loop always makes `8n` passes, so a 32-bit `*`
  runs 104 000 to 416 000 IR instructions instead of 35 000 to 106 000, as division did in
  phase 12; as written the loop alone is 0.2 to 2 million commands.
- `tools/bench.py` shows the sizes of `long / long` and `long % long` 28% larger, but their
  statements are unchanged (9 603 commands before and after): an op's size there is the
  program's minus the setup's, and the setup's constant products are now smaller and lay
  the globals out differently.
- Phase 17's premise has moved: `mulshr` no longer depends on the order or size of its
  operands and runs a constant factor in one step, `mulshr(a, 218, 8)` in 125 IR
  instructions and 5 100 commands (681 and 8 500 before) against 82 and 5 700 for
  `(a * 218) >> 8`, which now runs the loop too. What remains is the cost of the signs.

### Phase 16: shorter trips to operands

A raytracer statement's 40 000 to 70 000 commands were mostly pointer moves: a
global 900 cells from the temporaries costs about 4 x 900 commands per byte copied
(phase 10, "Found along the way"). Options to measure: place a statement's
temporaries next to the variables it uses rather than above all of them; lay out
globals so that variables used together sit together (the layout of phase 8 orders
by use count, not by co-use); keep hot locals of a procedure close to its temporaries.

Done when: the samples are measurably smaller (the raytracers by at least 10%) with
frame times no worse, and the tests and fuzzer are clean, also with `BFL_NO_LAYOUT`.

**Built** in `src/lang/codegen.cpp` (and a few lines of `src/lang/sema.cpp`), from a
measurement first. Of the raytracer's 3.57 M commands 87.6% were pointer moves, and 35% of
those came in runs of 512 cells or more (20% in runs of 1 024 to 2 047): the trips from the
temporaries down to the scene's arrays (`scx[s]`, `socd0[s]`, ...), which the layout of
phase 8 put lowest, 900 to 1 300 cells away, because an array is many cells for its
references. A runtime-indexed `long` element costs about 11 such distances (the index there,
four bytes back, the walk's own trip), and `brow[s] = socd0[s] + long(y) * socdv[s];` was
51 000 commands, 98% of them moves.

**Trips, measured.** The compiler now first generates the whole program dry (the dry pass
of the heap placement, for every program), and `go()` counts, for every global, the trips
whose other end is outside the globals and the cell they start from (`Gen::Cand`,
`note_trip`), and the trips into the heap. From those `plan_globals` makes a `Gen::Plan`:

- **Globals above the temporaries.** A global whose trips start high up (its uses are in
  deeply nested code, whose temporaries sit far above the globals) can be cheaper in a region
  placed just above the temporaries' high-water mark, between them and the heap. The estimate
  of a trip to a global below is its distance down (from the trips' mean start to the top of
  its group, the globals of the group nearer the top, the part of itself below the cell a use
  reaches) less the sizes of the globals of higher groups that moved up; to one above it is the
  distance up to the high-water mark plus what was placed before it there; every heap trip
  gets longer by the region's size. Starting from everything below, single moves either way
  are made while they lower the estimate. A second dry pass with the plan finds where the
  temporaries now end (the frame below shrank), and the last pass places the region there and
  the heap above it. In the raytracer the region holds 19 globals, 576 cells, mostly the
  per-sphere arrays; in `raytracer_text.bfl` 17 (522 cells); in `quicksort.bfl` the array
  of its trampolined main frame, in `trampoline_fib.bfl` its hand-made stack (291 cells) and
  the two cells used with it.
- **The rest by trips per cell**, the busiest nearest the temporaries, in the same groups
  (top-level declarations with no code between them, or the trampolined main frame) as the
  static count of phase 8, which still lays out the measuring pass.
- The plan is used when the second dry pass's estimated commands (the commands it wrote and
  the moves it left out, with the heap and the region taken where they will be) are fewer than
  the first's; otherwise the program is laid out as before.

The trampolined main frame (the raytracer calls a `shared proc`) is planned like the
top-level declarations: the globals that go up leave the frame, and the frames above it move
down by what left, so the frames stay a call stack. Symbols, the .map and the watch list
show the cells wherever they are.

**Copies through a spare cell.** `copy(a)` came back through a temporary at the top:
`a[-b+t+a]t[-a+t]`, two trips there and back per byte. The header and the `C` and `V` cells
of every array that lives for the whole program (top-level, the main frame, the save stack)
are zero between walks, so the copy now uses the one nearest `a` when it is much nearer than
the temporary (`spare_near`): one round trip a byte. The temporary is still taken, so the
cells a program uses do not depend on where things are.

**Known values kept through clears and moves**, which loop on their own cells only (the
experiment of phase 18, now kept): what is known of every other cell holds after them. `set`
on a cell with a known value adds the difference unless `[-]` and the new value is shorter,
which was why `hanoi.bfl` and `quicksort.bfl` grew in the experiment; both are now smaller.
A declaration whose cells were reserved earlier (a layout group, or before an early exit)
knows they are still zero, which they are, so `long k = 5;` after other code is known again.

**String literals first in the static heap.** `raytracer_text.bfl`'s `string ramp` came after
the 12 288-byte light map, so the start-up code that writes it walked 62 000 cells up and its
first statement 62 000 back (4% of the program), and every `ramp[i]` walked there and back at
run time. When the front end finds a literal after a heap array, it runs again with the
literals placed first (`StringPlaces` for `analyse`), unless that moves an array more than a
sixteenth further up: `starfield.bfl`'s 600 bytes of strings in front of its two 682-byte
frame buffers made its frames 24% slower (a heap walk takes as long as its address is high),
so there they stay where they are.

**The phase 18 review's points** (the coordinator's notes):

- *A shared run is larger than unshared in some shapes* (7 of the reviewer's 30 cases, 0.2 to
  2.8%). The dry passes now generate each run both ways and keep the smaller (`run_pays`:
  everything the generation touches is saved and put back); the last pass takes the choices of
  the dry pass before it in the same order, so its code is the same but for the placement. None
  of the reviewer's 37 programs is now larger than with `BFL_SHARE=0`.
- *A variable declared with a constant in a run* (`long x = a*b+1; long k = 5; long y = k*a + x;`)
  was shared as a general product, 255 389 commands against 254 161 unshared: `reads_known` now
  counts the variables declared or assigned with a constant earlier in the run (250 214 now,
  against 252 564 for the program with `5 * a` written in before).
- The phase 18 note's sentence about operations inside the loop is corrected (below).

**Found along the way: a register test wrote into the heap.** `if (KEY_STATUS)` (any readable
register that is not a latch, `test_reg`) steps the pointer as far above its temporaries as
they are above the register and expects the cell it lands on to be zero; in a program with a
heap it could land on a heap byte. `heap 256; byte arr[64]; string s = "abc..."; ... if
(KEY_STATUS) { ... } print s;` printed an empty string in cffb80c (8 of 53 array sizes from 1
to 160). The measuring pass now records the highest landing cell, and the region above the
temporaries and the heap start above it (`register-status-heap` in `tests/test_lang.py`).
(That raised the heap by about the temporaries' height for one test in deep code; the review
below made the landing cell one just above the temporaries.)

Measured with `tools/bench.py --only sizes` against cffb80c:

| | commands before | after | |
|---|---:|---:|---:|
| `raytracer.bfl` | 3 566 063 | 2 770 508 | -22.3% |
| `raytracer_text.bfl` | 3 075 162 | 2 465 851 | -19.8% |
| `quicksort.bfl` | 106 632 | 86 341 | -19.0% |
| `trampoline_fib.bfl` | 23 413 | 7 138 | -69.5% |
| `serial_echo.bfl` | 2 879 | 2 506 | -13.0% |
| `numbers.bfl` | 534 786 | 499 019 | -6.7% |
| `primes.bfl` | 4 971 | 4 662 | -6.2% |
| `cube.bfl` | 248 021 | 235 128 | -5.2% |
| `calc.bfl` | 942 782 | 906 162 | -3.9% |
| `mergesort.bfl` | 187 267 | 181 168 | -3.3% |
| `fb.bfl` | 2 800 | 2 710 | -3.2% |
| `text.bfl` | 486 008 | 478 615 | -1.5% |
| `bst.bfl` | 575 325 | 569 142 | -1.1% |
| `queens.bfl` | 49 802 | 49 325 | -1.0% |
| `starfield.bfl` | 1 224 128 | 1 212 625 | -0.9% |
| `maze.bfl` | 237 473 | 235 456 | -0.8% |
| `life.bfl` | 76 292 | 75 759 | -0.7% |
| `linked_list.bfl` | 1 612 754 | 1 601 728 | -0.7% |
| `hanoi.bfl` | 60 229 | 59 838 | -0.6% |
| `queue.bfl`, `leds.bfl` | 169, 1 352 | the same | |

In the order they were built, on the raytracer and `raytracer_text.bfl`: the region above
and the trips counted -13.7% and -9.4%, the order below by trips -14.4% and -10.1%, the
strings first -14.4% and -14.1%, the spare cells -16.7% and -17.0%, the reserved
declarations known zero -17.0% and -17.3%, the run choices -17.9% and -18.5%, the known
values through clears and moves -22.3% and -19.8%. The raytracer's moves went from 3.12 M
to 2.33 M, and those in runs of 1 024 cells or more from 631 K to 108 K.

Frame times, `tools/bench.py --only frames` on cffb80c built in a temporary worktree and on
this build, alternately, three rounds each, the fastest (the machine was otherwise idle but
for the odd other agent):

| ms | before | after |
|---|---:|---:|
| `raytracer.bfl` frame / light map | 30.58 / 37 | 30.08 / 37 |
| `raytracer_text.bfl` | 11.75 / 33 | 7.55 / 33 |
| `cube.bfl` | 1.31 | 1.27 |
| `starfield.bfl` | 1.03 | 1.02 |

`raytracer_text.bfl` is faster because `ramp[level]`, read for every character, now walks to
byte 7 of the heap instead of byte 12 295. The frame programs execute fewer IR instructions
(`bf --profile`): the raytracer's (light map and 3 frames) 505.9 M against 499.1 M,
`raytracer_text.bfl`'s 310.3 M against 248.2 M, the cube's (40 frames) 238.1 M against
237.6 M, the starfield's 168.7 M against 168.4 M. The raytracer's 16 frames rendered
headless (`tools/rt_gif.py`) are identical to cffb80c's, and so is the output of the other
three frame programs.

`tools/bench.py --only ops` now reports a line's own commands from the .map: the program
less the program without the line had come to include the other globals moving, since the
layout follows the trips (`byte / byte` showed +585%, its line +60%, its variables now further
down since the plan finds them rarely used). The setup's result variables start from an input
byte, so the tail no longer folds when the line is missing. By that measure the lines are 1 to
22% smaller (`mulshr(long, 218, 8)` 3 173 to 2 481, `(long * long) >> 12` 13 906 to 12 183),
but for `byte / byte` (271 to 443) and `mulshr(int, int, 7)` (+1.5%). The IR rows went up by
2 to 9 instructions (`long * long` 52 to 58): every op program executes about 126 fewer IR
instructions than before, the program without the line 132 fewer (the setup's known values).

Tests: `share-pays` (the reviewer's losing shapes and the two known-constant cases: no larger
than with `BFL_SHARE=0`, same output), `layout-plan` (an array used from inlined code lies above
that code's locals; smaller than `BFL_NO_LAYOUT=1`; the same output in every executor and both
layouts), `strings-first` (a literal after a 3 000-byte heap array costs what one before it
does) and `register-status-heap` in `tests/test_lang.py`; `share-setting` compares its sizes with
`BFL_NO_LAYOUT=1` (the layout moves with sharing too) and its run has three products now, since
with two the run was not the smaller and is no longer shared. The golden files were
regenerated (all smaller but `leds.b`, unchanged). Both suites pass (`test.py` 83/83, `test_lang.py` 247 tests with the
grids, all but the golden files also with `BFL_NO_LAYOUT=1`, whose code they do not describe, as
before); and `tests/fuzz.py --bfl 1500 --bf 300` found nothing, run as it is (seed 1 600 000; 929 programs sharing arithmetic, 406 with shared procs) and with `BFL_NO_LAYOUT=1` (seed 1 700 000), nor did 800 programs each of `--focus shared` and `--focus struct`.

**The phase 16 review's points** (the coordinator's notes):

- *One status-register test in deep code raised the heap by the temporaries' height*: the
  landing cell was `P + M - reg`, with `M` a temporary as high as `P`, and the heap went above
  it, so `raytracer.bfl` with `if (KEY_STATUS) { pulse KEY_STATUS; }` after `pulse FB_PLOT`
  grew from 2 770 508 to 3 322 955 commands. Now `M` is the first RAM cell, kept for the test
  in a program that has one (`Gen::test_cell`; the measuring pass finds out, and runs again
  with the cell kept), so `M - reg` is 1 to 22 and the landing cell is a free one a few cells
  above `P`, under the high-water mark or just past it. The test is `M+ reg[ M- dst+ P ]`
  then the same resynchronisation: three long trips (down to `M`, up to `dst`, and the
  resynchronisation's `D`) where the old one made five between the temporaries and the
  register, and what is known of the other cells holds after it. Measured
  with `bf --emit` (cffb80c, before the heap fix / 43dd94a / now): that raytracer 3 591 643 /
  3 322 955 / 2 778 131 (the sample itself is 2 770 508), the reviewer's `p3.bfl` (a heap,
  strings, status tests in an inlined procedure) 403 067 / 529 360 / 389 947, `p5.bfl` (a
  test in a `rec proc` with heap-frame arrays) 578 471 / 850 534 / 521 012. No sample changes
  but `serial_echo.bfl` (the same 2 506 commands; its variables one cell up). Tests:
  `register-test-cost` (a test in code three inlined procedures deep, below 80 longs of
  globals: 37 076 commands and 720 cells more than without it in 43dd94a, now 5 079 and 1;
  the same output in every executor and both layouts) and `register-status-rec` (tests in a
  re-entrant `rec proc` with a heap frame and in a `shared proc`, a 16-bit save stack,
  strings after heap arrays). The reviewer's generator of 1 000 random programs mixing heap
  arrays, strings, arrays in cells, deep inlined temporaries and status tests (extended with
  tests in `rec` and `shared` procedures calling inlined ones) agrees across the JIT, the
  interpreter, `--no-opt` and `BFL_NO_LAYOUT=1`, and with 43dd94a (7 of the 1 000 overflowed
  the default recursion stack the same way in all of them, and agree with `stack 900;`).
- *An array declared in a loop body of trampolined code was not cleared on each pass*
  (before phase 16 too): `for (...) { byte a[4]; a[1] = a[1] + f(2); print a[1]; }` with `f`
  a `rec proc` printed `2 4 6`. A main program's array was taken as zero from the start, and
  a re-entrant procedure's heap-frame array was zeroed only on entry. Both now get a `Clear`
  action where the declaration is met inside a loop (the heap-frame one zeroes its bytes
  through the pointer, `heap_zero`, the loop `frame_enter` uses). Scalars were already
  cleared at every declaration, and a `shared` or non-re-entrant `rec` procedure's arrays at
  every declaration; inlined procedures' locals are on the temporaries' stack, cleared at
  the end of their scope. Test: `trampolined-loop-locals` (all of these). The fuzzer made
  such arrays but seldom read an element before writing it, and its loops often run once:
  an array declared in a loop body now gets `a[k] += expr` right after it in six of ten
  (with a call of a trampolined procedure in it or after it, when one can be called there,
  so that the array is a frame's), and in three of ten programs whose main program can call
  a trampolined pure procedure a loop of two or three passes in it declares an array, adds
  such a call to an element and the element to a global. 43dd94a fails 6 of 300 programs
  (seed 1 800 000) of the fuzzer as it is now, none of 1 000 before the second change.
- *Compile time*: the dry passes had roughly doubled it (`bf --emit` of the raytracer 0.60 s
  against 0.37 s in cffb80c, on this machine, the fastest of five). Most of the code
  generation time went in `clear` and `move` copying the whole map of known values to keep
  it through their loops; they now forget only their own cells. The raytracer's three
  passes took 115, 108 and 70 ms, and now 25, 18 and 17 ms, with the same output for every
  program under `programs/`. `bf --emit` of the raytracer is 0.35 s, of
  `raytracer_text.bfl` 0.35 s (0.61 in 43dd94a, 0.34 in cffb80c), most of it now the IR
  and the JIT, as before phase 16.
- `run_pays`'s count of commands skips a NUL as `dry_commands` does.
- `docs/LANG.md` no longer says the variables are allocated upward from the first cell or that
  a later declaration starts a run above the variables that exist then, without the region
  above the temporaries.

Both suites pass (`test.py` 83/83, `test_lang.py` 249, all but the golden files also with
`BFL_NO_LAYOUT=1`); only `golden/serial_echo.b` changed. `tests/fuzz.py --bfl 1500 --bf 300`
found nothing, as it is (seed 2 000 000; 940 programs sharing arithmetic, 395 with shared
procs) and with `BFL_NO_LAYOUT=1` (seed 2 100 000). The raytracer's 16 frames
(`tools/rt_gif.py`) are identical to 43dd94a's, whose brainfuck it is, as for every program
under `programs/` but `serial_echo.bfl`.

The options the phase listed, and what became of them:

- *A statement's temporaries next to the variables it uses*: the region above the
  temporaries does this for the globals that deep code uses, without temporaries of
  their own per region (a statement's temporaries are a stack its inlined calls grow, so
  they cannot be moved per statement).
- *Globals that are used together, together*: the plan orders by measured trips and the
  height they start from, which is where the code using them sits, rather than by
  co-occurrence in statements. Not tried as such.
- *Hot locals of a procedure near its temporaries*: inlined procedures' locals are already
  allocated on the temporaries' stack, and a trampolined procedure's frame sits right below
  its blocks' temporaries; nothing to change.
- *Known values through every move*: done, with the `set` case fixed.

### Phase 17: cheaper constant factors in `mulshr`

As first planned, `mulshr` ran a constant factor through its general loop and took its
operands as written where `*` looped over the smaller one. Phase 15 moved the ground:
`*` and `mulshr` became one IR idiom (`bf::multiply_loop`), so neither the order nor
the size of the operands matters, and a constant factor from 8 up runs the loop too.
What was left was the work around the loop: the magnitudes and signs of the operands
and of the result, and the last `k % 8` bits shifted by byte divisions after it
(`mulshr(a, 218, 8)` ran 125 IR instructions against 82 for `(a * 218) >> 8`). So the
plan became: measure `mulshr` against `*` then `>>` at every width and signedness, for
two variables and for constant factors (small, 8 to 255, large, negative, powers of
two), and make that work cheaper: the signs, a constant-factor path that knows the
factor's sign and magnitude at compile time, powers of two as shifts, small factors
unrolled. Also two points from phase 15's review: the help text on `mulshr`, and the
cost of the JIT's `tape_end` compare before every inline idiom (2.5% on `cube.bfl`).

Done when: `mulshr` is never larger or slower than `*` then `>>` for the same
operands, measured over constant and variable factors at every width, with the
grids and the fuzzer clean.

**Built** in `mulshr` (`src/lang/codegen.cpp`), with the multiply loop as it was:

- No shift after the loop. The loop's rule takes any pass count and every pass past
  `8n` doubles `P`, so for `k % 8` not 0 `mulshr` runs `8 - k%8` passes more: `P` (still
  `n + ceil(k/8)` bytes, at most `2n`) is then the product shifted left so far that the
  result is the `n` bytes above the `ceil(k/8)` dropped. The byte divisions are gone. The
  JIT runs 8n to 8n + 63 passes inline, `P = ((P << 8n) + A * B) << (passes - 8n)`
  (before, only 8n; any other count goes through `idiom_fn` as before).
- Signs as a start value. Signed factors are multiplied as they are stored: the product
  of the bit patterns is the signed product plus 2^8n (`b` when `a < 0`, plus `a` when
  `b < 0`), modulo 2^16n, and after the passes `P` is `P0 * 2^8n` plus the product, so
  `P` starts at minus those (its low `w - n` bytes, one when `k <= 8`). The bytes kept
  are then the rounded-down result as they stand: no negating the operands, no record
  of the bits shifted out, no fix-up of a negative result. A sign is the byte compare
  of the top byte with 127, its result the if-cell itself (`sign_test`).
- Constant factors, either operand, with the sign known at compile time: a factor
  `c >= 0` needs only `a`'s sign, and then adds a constant to `P0`; a negative one
  starts `P0` at `-a` as well. A power of two whose shift is a whole number of bytes is
  that shift (`mulshr(a, 4096, 12)` is `a`, `mulshr(a, 256, 16)` a byte move), 0 gives
  0 and `k = 0` is `*` (since the review: the multiply loop for 16 and 32 bits, the byte
  product `*` uses for bytes).
- `multiply_block` frees its cells without clearing them: after the loop, as written or
  in one step, everything but `P` and `A` is zero, and `take` empties `P`. That saved a
  `[-]` (an IR instruction) per cell of the block and makes `*` cheaper too.
- `sign_flag` (signed `>>`, `<<`, division, comparisons) lost a temporary and a test: the
  compare's result goes straight into the flag.

Tried and dropped: unrolling small factors as `mul_constN` does. The exact product
needs the byte above the operand's, and copies and adds on `n + 1` bytes were slower
than the loop (`mulshr(w, 3, 8)` on `word`s: 46 IR instructions and 852 commands,
against 18 and 769 for the loop); and putting a byte's factors straight into the
block, which made the pointer travel further (364 to 468 commands).

Measured by a script (not in the tree) over 230 cases: `mulshr(a, f, k)` against
`(a * f) >> k` at every type, for `f` a second variable and 2, 3, 5, 7, 16, 41, 128,
218, 256, 577, 4096, 65536, 1103515245, -3, -218 and -577 where they fit, and two
shift counts per width (`k` = 4 and 7 for bytes, 7 and 8 for 16 bits, 12 and 16 for 32;
for signed types also with `a` negative). Each is the IR instructions the whole program
executes (`bf --profile`) and its commands, less those of the same program with
`c = a;` (or `c = a + b;`) in place of the statement, all printing `c` in a way that
costs the same whatever its value. Before, `mulshr` was slower in 115 cases and larger
in 57; after, slower in 6 and larger in 23. Some of them (IR instructions / commands):

| | `mulshr` before | after | `*` then `>>` before | after |
|---|---:|---:|---:|---:|
| `byte`, k = 4, two variables | 40 / 840 | 4 / 364 | 10 / 348 | 10 / 348 |
| `byte`, k = 4, 218 | 43 / 860 | 6 / 387 | 16 / 341 | 16 / 341 |
| `sbyte`, k = 4, two variables | 94 / 1 642 | 28 / 1 085 | 87 / 1 992 | 67 / 1 884 |
| `sbyte`, k = 4, -3 | 106 / 1 621 | 31 / 746 | 95 / 1 950 | 75 / 1 842 |
| `word`, k = 7, two variables | 58 / 1 960 | 10 / 549 | 123 / 3 698 | 108 / 3 620 |
| `word`, k = 8, two variables | 33 / 721 | 10 / 548 | 32 / 893 | 17 / 815 |
| `word`, k = 8, 218 | 42 / 969 | 18 / 804 | 41 / 1 141 | 26 / 1 063 |
| `word`, k = 8, 3 | 42 / 934 | 18 / 769 | 18 / 550 | 18 / 550 |
| `word`, k = 8, 256 | 42 / 932 | 0 / 53 | 40 / 1 106 | 25 / 1 028 |
| `int`, k = 7, two variables | 146 / 3 135 | 32 / 1 400 | 254 / 6 764 | 204 / 6 497 |
| `int`, k = 8, two variables | 116 / 1 894 | 32 / 1 399 | 46 / 1 184 | 28 / 1 079 |
| `int`, k = 7, 218 | 146 / 3 371 | 30 / 1 237 | 244 / 7 012 | 215 / 6 745 |
| `int`, k = 8, 218 | 116 / 2 130 | 30 / 1 236 | 51 / 1 432 | 36 / 1 327 |
| `int`, k = 8, -218 | 132 / 2 133 | 34 / 1 330 | 55 / 1 435 | 37 / 1 330 |
| `int`, k = 8, 2 | 116 / 2 094 | 30 / 1 164 | 31 / 656 | 29 / 629 |
| `dword`, k = 12, two variables | 96 / 2 298 | 11 / 868 | 147 / 8 238 | 118 / 8 090 |
| `dword`, k = 16, two variables | 55 / 1 245 | 11 / 864 | 53 / 3 392 | 24 / 3 244 |
| `dword`, k = 12, 1103515245 | 123 / 3 924 | 32 / 2 506 | 174 / 9 864 | 145 / 9 716 |
| `long`, k = 12, two variables | 157 / 4 468 | 30 / 3 589 | 247 / 10 401 | 195 / 10 118 |
| `long`, k = 16, two variables | 111 / 3 423 | 30 / 3 585 | 70 / 3 692 | 38 / 3 517 |
| the same, `a` negative | 159 / 3 423 | 41 / 3 585 | 67 / 3 692 | 38 / 3 517 |
| `long`, k = 12, 218 | 178 / 5 792 | 39 / 2 827 | 257 / 11 749 | 205 / 11 466 |
| `long`, k = 16, 218 | 133 / 4 747 | 39 / 2 823 | 90 / 5 040 | 58 / 4 865 |
| `long`, k = 12, -577 | 230 / 5 830 | 48 / 4 298 | 282 / 11 787 | 245 / 11 504 |
| `long`, k = 12, 4096 | 172 / 5 770 | 1 / 175 | 262 / 11 729 | 210 / 11 446 |
| `long`, k = 16, 3 | 133 / 4 712 | 39 / 2 753 | 89 / 3 228 | 84 / 3 201 |

`*` then `>>` got cheaper as well, from the multiply block and `sign_flag`. So the "Done
when" is met except where `>>` moves whole bytes, and only there:

- Two signed variables (`int` with `k = 8`: 32 against 28 IR instructions, 1 399 against
  1 079 commands; `long` with `k = 16` and `a` negative: 41 against 38). `mulshr` tests
  both signs and corrects `P0` for each, while the wrapped `*` needs neither and the
  byte shift one sign test.
- A factor of 2 (`int`, `k = 8`: 30 against 29; `word`: 18 against 12) and, in commands
  only, the other factors below 8 (`word`, `k = 8`, 3: 769 against 550): `*` unrolls
  them into a copy or two on `n` bytes, the loop is about 430 commands.
- Bytes, in commands only: 4 to 15% larger (the loop against the byte product and
  four halvings), at a third of the IR instructions or less.

`tools/bench.py --only ops` now counts an operation's IR instructions as the whole
program's less the program's without it, and its output line no longer depends on the
values (see "Found along the way"); it also has `(int * int) >> 8`, `mulshr(int, int, 8)`,
`(long * 218) >> 8` and `mulshr(long, 218, 8)`. Before and after, both counted that way:

| | before | after |
|---|---:|---:|
| `long * long`, IR instructions / commands | 83 / 5 945 | 54 / 5 797 |
| `(long * long) >> 12` | 280 / 13 085 | 228 / 12 802 |
| `mulshr(long, long, 12)` | 194 / 7 152 | 69 / 6 273 |
| `(int * int) >> 7` | 266 / 7 322 | 219 / 7 055 |
| `mulshr(int, int, 7)` | 160 / 3 693 | 50 / 1 958 |
| `(int * int) >> 8` | 58 / 1 742 | 42 / 1 637 |
| `mulshr(int, int, 8)` | 130 / 2 452 | 50 / 1 957 |
| `(long * 218) >> 8` | 100 / 5 443 | 67 / 5 268 |
| `mulshr(long, 218, 8)` | 125 / 4 865 | 54 / 2 949 |
| `3 long * in a sum` | 321 / 10 210 | 233 / 10 062 |
| `3 mulshr in 2 statements` | 788 / 14 931 | 368 / 14 076 |
| `raytracer.bfl`: ms a frame / light map / commands | 40.3 / 40 / 3.66 M | 31.0 / 38 / 3.62 M |
| `raytracer_text.bfl` | 15.3 / 36 / 3.18 M | 12.1 / 34 / 3.14 M |
| `cube.bfl`: ms a frame / commands | 1.35 / 252 K | 1.29 / 250 K |
| `starfield.bfl` | 1.06 / 1.23 M | 1.07 / 1.23 M |

Frame times: tools/bench.py --only frames on the commit before (built in a temporary
worktree) and on this one, run alternately, five rounds each, the fastest; the machine
was shared with other work. No sample got larger; the raytracers are 1.2% and 1.3%
smaller. The raytracer's 16 frames rendered headless are identical to the commit
before.

**The JIT's end-of-tape check.** Phase 15's compare (`lea rax, [r12+hi]`, `cmp rax,
[rbx+80]`, `jae`) before every inline idiom is gone. The inline code of each idiom opens
with instructions that only read cells: the scratch-cell guards, the loop cell, the
loads of the multiply loop's operands, the first source of a product. When those do
not read its highest cell, one load of that cell (`mov cl, [r12+hi]`) goes first (in
`cube.bfl` 33 of 335 inline idioms need it, in `raytracer_text.bfl` 718 of 2 439). A
fault in those instructions has changed nothing, so the fault handler (`veh` in
`src/machine.cpp`), finding the faulting address past the tape's end and the
instruction in one of them (`JitCode::probes`), resumes at the loop as written, which
faults or not where it should. Keeping `tape_end` in a register (r14 less the largest
idiom's reach, one `cmp r12, r14`) was tried first and measured no faster on
`cube.bfl`. Measured as above, six rounds each, with the old compare / with none (unsafe,
for reference) / with the probes: `cube.bfl` 1.34 / 1.29 / 1.29 ms, the raytracer 31.6 /
30.7 / 30.9 ms, `starfield.bfl` 1.04 / 1.05 / 1.06 ms, `raytracer_text.bfl` 11.9 / 11.8
/ 12.0 ms: the check cost about 4% on the cube and 3% on the raytracer, the probes 0 and 1%;
the other two move within the noise of code alignment (a probe
written as `movzx ecx` instead measured 3% slower on the cube). `fault-idiom-loops-at-end`
in `tests/test.py` covers it (and now a byte product too); without any check it fails
(`long-mul` at `--ram 70`).

Tests: `mulshr-const-*` in `tests/test_lang.py`, a grid per type of `mulshr(a, C, k)`
and `mulshr(C, a, k)` for 7 to 12 constants (0, 1, powers of two, small, negative, the
type's extremes) and up to 22 counts from 0 to past the double width. The fuzzer gives one
`mulshr` in three a constant factor, either side, negated in three of ten. `python
tests/fuzz.py --bfl 1500 --bf 500` ran clean from seeds 700 000 (before the JIT change),
710 000 (with the probe load written as `movzx`) and 720 000 (the final build), and both
test suites pass.

Found along the way:

- `tools/bench.py --only ops` charged an operation the IR instructions `bf --profile`
  gives its line, but the profiler charges a straight-line block to the statement it
  starts in, so a line's count held some of the next line's: up to 20 IR instructions
  that depended on what the operation left behind (`mulshr(int, int, 8)` counted 58
  against 31 for `(int * int) >> 8`, which by the whole program's count are 50 and 42).
  And `lc >> 8` in its output line is signed, so its cost depended on the values.
- The IR optimiser does not know that a fresh temporary is zero, so a copy through one
  is 5 IR instructions after a loop and 1 after a known clear: the same operation costs
  some 5 to 15 instructions more or less depending on its neighbours (the `int`, `k = 8`
  case above is 32 against 28 in one program and 50 against 42 in another).
- An optimised multiply-add writes its destination even when its source is 0, so
  `x[- y[- d+ t+] t[- y+]]` with `y` = 0 and `d` past the end of the tape faults in the
  JIT and the interpreter where the program as written does not touch `d`. This was so
  before; phase 18 fixed it.

**Review fixes.** A sweep of `mulshr(a, b, k)` at every `k` from 0 to `2 * bits + 6`, every
type, for two variables and 14 constants (-577, -256, -100, -3, -2, -1, 2, 3, 5, 7, 77,
128, 256, 4096), each the IR instructions and commands of the program less the same
program with `r = a;`, found the phase larger than ee5c656 at many counts and slower at
some. Fixed:

- From `k = 8n` up, the bits left after the whole bytes were shifted out by one `shrN`
  per bit (about 49 IR instructions each at 32 bits). The high half now goes straight
  from `P` to the result above the whole bytes dropped, then one division of each byte
  by `2^r` (the remainders moved into the byte below) and a sign fill, as before phase 17
  (one bit is still a `shrN`, fewer instructions); `k = 16n` is the top byte's sign.
- Signed with 3 or 4 bytes dropped (32 bits, `k` 17 to 32 and up): the corrections of
  `P0` were borrow chains whose zero tests went through temporaries past the multiply
  block, 50 cells from the bytes of `P`, written twice. They now use the loop's scratch
  cells next to each byte of `P` (zero before the loop; the counter of a byte's
  subtraction is the third). A complement and increment for the first correction, as the
  review suggested, measured 5 to 7 IR instructions more than the subtraction with the
  near cells and saved under 3% of the size at 3 or 4 bytes, so it was not kept.
- Two variables at `k = 0` on bytes are the byte product of `*` (101 commands against
  277 for `sbyte`, in the sweep's units), and a constant at `k = 0` runs the multiply
  loop into the result, as before phase 17 (phase 17 went through `mul_constN`, whose
  carries into a result not known to be zero cost up to twice the commands: `long`
  times -577, 4 499 commands against 1 804 now).

Some of the sweep (IR instructions / commands; ee5c656, 10f5975, now):

| | ee5c656 | 10f5975 | now |
|---|---:|---:|---:|
| `long`, two variables, k = 12 | 205 / 6 482 | 85 / 5 603 | 85 / 4 215 |
| `long`, two variables, k = 24 | 168 / 5 717 | 58 / 8 264 | 60 / 4 910 |
| `long`, two variables, k = 32 | 173 / 6 005 | 62 / 11 993 | 66 / 5 729 |
| `long`, two variables, k = 39 | 216 / 8 128 | 394 / 23 522 | 108 / 7 723 |
| `long`, -577, k = 32 | 172 / 5 353 | 45 / 9 210 | 49 / 3 993 |
| `long`, -577, k = 39 | 215 / 7 476 | 377 / 20 739 | 91 / 5 987 |
| `dword`, two variables, k = 39 | 111 / 5 665 | 211 / 11 598 | 54 / 4 866 |
| `int`, two variables, k = 23 | 172 / 3 705 | 226 / 8 689 | 85 / 3 234 |
| `sbyte`, two variables, k = 15 | 101 / 2 111 | 163 / 4 286 | 59 / 1 698 |
| `sbyte`, two variables, k = 16 | 81 / 1 290 | 52 / 1 363 | 48 / 1 274 |
| `byte`, two variables, k = 0 | 30 / 360 | 16 / 283 | 10 / 107 |

Now no count of any type, for the variables or any of the constants, is larger or slower
than ee5c656. Against 10f5975 a few signed cases are 2 to 4 IR instructions slower (the
copies through the scratch cells, which the optimiser does not know to be zero, are a
loop where a fresh temporary's is a multiply-add; copying through temporaries instead
saved up to 9% of the IR instructions but made some counts larger than ee5c656). The
instruction count of a signed `mulshr` depends on the signs: `mulshr(a, b, 12)` on
`long`s is 47 with both positive and 79 with both negative (59 and 71 in 10f5975).
`tools/bench.py`: `mulshr(long, long, 12)` 6 273 -> 4 885 commands, `mulshr(int, int, 7)`
1 958 -> 1 888, `3 mulshr in 2 statements` 14 076 -> 12 688, the same IR instructions;
`raytracer.bfl` 3 616 121 -> 3 598 515 commands, `raytracer_text.bfl` 3 141 253 ->
3 123 647, `cube.bfl` 250 353 -> 249 537, the rest unchanged.

- Repeated faults at the end of the tape: a loop whose idiom reaches past the end but
  which as written stops short faulted on every entry (1.4 to 2 us each; a program that
  enters a 4-byte carry 62 500 times at `--ram 14` took 120 ms against 0 on ee5c656).
  Each probe now opens with a 5-byte nop; the fault handler turns it into a jump to a stub
  after the code (`lea rax, [r12+hi]`, `cmp rax, [rbx+80]`, `jae` the loop as written,
  else back after the nop), with `VirtualProtect` around the write, so that idiom checks
  the end of the tape, as before phase 17, on every later entry and the others keep no
  check. That program now takes under a millisecond (`bf --time`, best of 5; the same at
  `--ram` 10 to 21, outputs identical to ee5c656). Frame times (as above, three rounds
  each, alternately, fastest): `raytracer.bfl` 31.2 -> 30.4 ms, `raytracer_text.bfl` 12.1
  -> 12.0, `cube.bfl` 1.30 -> 1.29, `starfield.bfl` 1.07 -> 1.04: the nop costs nothing
  measurable. `fault-probe-then-runs` in `tests/test.py` is that program at `--ram 14` on
  every executor, `fault-probe-once` its time on the JIT (under 30 ms; about 100 ms
  without the patch).

The raytracer's 16 frames rendered headless are identical to 10f5975's. The grids, both
test suites, `python tests/fuzz.py --bfl 1500 --bf 1000 --seed 730000`, and the review's
scripts (`mulshr` against Python at every type, count and constant on the JIT, the
interpreter, `--no-opt`, `BF_NO_IDIOMS` and `BFL_SHARE=0`; signed shifts, division and
comparisons; every idiom program at `--ram` 3 to 139 against ee5c656) all pass. Not
done: the optional factor 2 at a whole-byte `k` as a byte move and a doubling.

### Phase 18: loose ends of phases 11 to 13

- The profiler (UI and `bf --profile`) does not count instructions run through the
  literal program while the pointer can reach the device window: add a count array
  for the literal program, mapped back to the source.
- Statements inside a shared-arithmetic run lose the cell values the compiler knew
  before it (phase 11): keep the values that no statement in the run writes, and
  restore the H cells' known values after the loop.
- The UI has no dark theme, so the profiler's heat colours were tuned for the light
  editor only: add a dark theme (editor, file viewer, terminal, panels) and heat
  colours readable in both.

Done when: each point is fixed with a test or a screenshot check, and the docs no
longer list it as open.

**Built** in four parts.

**The profiler counts the device window** (`src/machine.cpp`, `src/interp.cpp`,
`src/profile.h`). `Machine::profile` holds the optimised code's counts followed by the
literal program's (`Ctx::lit_prof`, which the interpreter uses while it runs the literal
program), and `profile.h` maps both back to statements through the instructions'
brainfuck offsets, so `bf --profile` and Run > Profile count them per statement and in
the totals. A loop writing `LEDS` and the framebuffer registers went from 520 to 22 841
counted IR instructions, and its `LEDS` line from uncounted to 32%. Test:
`profile-devices` in `tests/test_lang.py` (the register lines have counts, the rows add
up to the total).

**A dark theme in the UI** (`src/ui.cpp`, `src/help.cpp`, `src/terminal.cpp`). View >
Theme: Follow Windows (the default, from `AppsUseLightTheme`, updated on
`WM_SETTINGCHANGE`), Light or Dark, saved as `theme` in `settings.ini`. Dark covers the
editor, the imported-file viewer, the generated brainfuck and memory viewers, lists and
their headers, buttons, combo boxes, the status bar, the menu bar and popup menus
(uxtheme's ordinals 135 and 136 for the popups; the bar painted on `WM_UAHDRAWMENU` and
`WM_UAHDRAWMENUITEM`), title bars (`DWMWA_USE_IMMERSIVE_DARK_MODE`), the help window and
the terminal's default colours. Syntax colours, breakpoint marks and the profiler's heat
colours have a set per theme; switching keeps the breakpoints, the scroll position and an
open profile, and the light theme looks as before. Its review found and fixed: breakpoint
marks were in the undo history, so undo after a theme switch lost a breakpoint that still
showed (marks are now outside undo, and a debug session reads either theme's mark
colour); View > Generated brainfuck during a run recompiled the program the machine
thread was running, and the interpreter then read freed memory (`idiom_thunk` crashed
within seconds when the framebuffer window was resized): nothing recompiles while a run
or debug session is on, and the window shows the program that runs; disabled buttons in
the dark theme are drawn dim.

**Known values in a shared-arithmetic run** (`gen_run` and `run_shared` in
`src/lang/codegen.cpp`). Each statement of a run runs once and in order, inside the loop
or after it, so when statement j starts, the cells no earlier statement of the run writes
still hold what they held before the run (`known_at`: a statement writes its target
variable, the whole of an array for an element, or a declared variable's cells; writes
through pointers go to the heap, which has no known values). That knowledge goes back in
before each statement and each move of a result into H, since the loops before them make
the compiler forget what it knows; the moves of results into H and out of it
(`moveN_keep`) keep what is known of the other cells. The loop's own code and a sum's
shared loop (`shared_terms`) start each pass with what is known before the loop of the
cells nothing in the loop writes. Reading a known variable in a run now costs what its
constant does: `long x = a * b + k; long y = x * a - j; n = n + 1; long z = y * b + k - j;
x = x * z;` with `k`, `j` and `n` known compiles to 256 754 commands, the same as with
1000, 7, 4 and 993 written in (269 560 before, against 261 989).

The restore after the loop was wrong, but harmlessly: it marked all of H as known zero,
also the cells holding the earlier results of the last statement, and only the loop of
the next move (of the last result into H) made the compiler forget it again before
anything read them. Now H is known only where it is zero, and since that move keeps what
is known, the restored values reach the statements after the loop. Unchanged: an
operation that reads a known variable still ends the run before its statement
(`reads_known`). (Its review found that this missed variables declared with a constant
earlier in the run, which had no cells yet: `long x = a*b+1; long k = 5; long y = k*a + x;`
shared `k*a` with `k` a known 5 inside the loop, and so did one assigned a constant there.
Phase 16 counts both as known, so no operation inside the loop sees a known value.)

Measured on the samples against the commit before (only these four change):

| | commands before | after | IR instructions before | after |
|---|---:|---:|---:|---:|
| `raytracer.bfl` (light map and 2 frames) | 3 616 121 | 3 583 669 (-0.9%) | 390 951 700 | 390 587 316 |
| `raytracer_text.bfl` (light map and 2 frames) | 3 141 253 | 3 092 768 (-1.5%) | 258 495 792 | 258 261 627 |
| `cube.bfl` (20 frames) | 250 353 | 248 837 (-0.6%) | 119 108 015 | 119 107 365 |
| `starfield.bfl` (20 frames) | 1 225 020 | 1 224 128 (-0.1%) | 85 663 743 | 85 658 897 |

The `tools/bench.py --only ops` rows are unchanged (they read no known values). Frame
times, each sample's frame program compiled by both builds and both run by this build,
alternately, five rounds, the fastest (the machine was shared with other work; `tools/bench.py
--only frames` on both builds alternately, three rounds each, moved by more than the
differences): the raytracer 30.9 ms against 31.1 before, `raytracer_text.bfl` 12.1 against
12.2, `starfield.bfl` 1.05 against 1.08, `cube.bfl` 1.36 against 1.32. The cube executes
the same IR instructions as before to within 0.001%, 93% of them in two array walks whose
code did not change; its new code is 2 KB of x64 shorter, which moves those loops. With
1 to 7 multiply-adds of a zero cell put in front of the program (moving all of its code by
a few bytes at a time), the build before measured 1.30 to 1.35 ms and this one 1.33 to
1.37: the cube's frame depends on where its loops land.

Tests: `share-known-in-run` (two inputs: a known variable and an array element read after
statements of the run that write the array through a runtime index or write the variable)
and `share-known` (the program above with variables and with constants: the same size and
output; the build before fails it) in `tests/test_lang.py`. The fuzzer's runs
(`shared_run`) now give one or two variables constants just before a run in four of ten,
and some of the plain statements between its statements constants; the run's statements
read them outside the operations in half of them and in an operation's factor (which
ends the run) now and then. Measured with a counting build over 300 seeds, the programs
with a run whose loop keeps nonzero known values went from 16 to 32, those with a run
ended by a known value from 24 to 64.

**A multiply-add of 0 no longer touches its destination** (`src/interp.cpp`,
`src/jit.cpp`, `src/machine.cpp`). The optimiser turns a balanced loop into multiply-adds
of the loop cell, which write their destinations whatever the loop cell holds, where the
loop as written does not run at all when it is 0: `,[-` then 1000 `>`, `+` and 1000 `<`,
`]` with input 0 and `--ram 1000` faulted in the JIT and the interpreter and not without
optimisation (found in phase 17). The interpreter now reads the source first and skips
the destination when it is 0 (one branch, which also saves the read and write). The JIT
emits the same code as before, and records where each multiply-add's write (`add` or
`sub [r12+off], cl` or `al`) starts (`JitCode::mul_writes`); the fault handler, finding a
fault past the end of the tape at one of them with `cl`, the source, 0, skips the
instruction (it would add 0) and continues. `cl` and not `al` is tested because the
product of a non-zero source can be 0 (`d++` with the source 128): the loop as written
runs then, and faults. Normal runs execute exactly what they did. Tests in
`tests/test.py`: `fault-mul-of-zero` (factor 1, a 32-bit offset), `-of-zero-factors`
(255 and 3, 8-bit offsets), `-in-loop` (the loop phase 17 found, inside another),
`-of-one` and `-wraps-to-zero` (both fault, at the same cell in every executor); the
build before fails the first three. A remaining difference the other way, not changed:
the optimiser drops a cell's `+-`, so `>`×1000 `+-` faults only without optimisation.

Found along the way:

- Keeping what is known through every move, not only those of a run (in `move`, whose
  loop touches only its source and destinations), makes most samples 0.1 to 1.6% smaller
  (`primes.bfl` 3%), but `hanoi.bfl` and `quicksort.bfl` a few commands larger: `set` on a
  cell with a known value adds the difference, which is longer than `[-]` and the new value
  when the difference is large. Not done here; it touches all generated code.

Both test suites pass, `python tests/fuzz.py --bfl 1500 --bf 500 --seed 830000` found
nothing (981 of the 1 500 programs sharing arithmetic), and the raytracer's 16 frames
rendered headless (`tools/rt_gif.py`) are identical to the commit before.

### Phase 19: fuzzer coverage

`tests/fuzz.py` covers the integer types, arrays, pointers, loops and inlined and
`rec` procedures. Structs (fields, copies, arrays of structs, structs in the heap and
through pointers), `ref` array parameters and `shared proc` are covered only by
hand-written tests and the separate `tests/fuzz_shared.py`. Add them to the
generator and the Python model, and fold `fuzz_shared.py` into `fuzz.py`.

Done when: a long run (thousands of programs) with the new constructs finds nothing,
and every bug it found has a minimal regression test.

**Built** in `tests/fuzz.py`. The generator declares up to three struct types
(integer fields, array fields, earlier structs and arrays of them as fields, at
most 20 bytes), a variable of each, arrays of structs in cells and in the heap, and
pointers into the heap ones, globally and as locals. Places inside them are reached
through fields and indices (an array field of a struct held in cells by a constant,
in the heap by anything) from a variable, an array element, `p->f`, `(*p).f`,
`p[i].f` and `mk(1).f`. Fields are read and written; whole structs are copied by
assignment and declaration, passed by value and by `ref` and returned, also from
`rec` procedures; `sizeof` of every type. Procedures take `ref T a[]` (T an integer
type or a struct): bound to arrays in cells when inlined, and when trampolined to
heap arrays, the caller's own array parameters and the local arrays of a
re-entrant procedure. A fifth of the procedures that are not `rec` are marked
`shared`, and such a program is also compiled with them inlined, which must agree
with it. In the model a struct is a dict, an array field a list, and a `ref`
parameter bound to something other than a whole variable is an alias of that
place. `--focus shared` and `--focus struct` make those constructs more frequent.
`tests/fuzz_shared.py` is gone: its two constraints are the generator's (only pure
procedures are called in expressions; a procedure with `ref` parameters touches no
globals, and the `ref` arguments of one call share no storage, now also counting
arrays, structs and the heap array a pointer points into), and
`tests/test_lang.py` runs 12 programs of `--focus shared` in its place, and 8 of
`--focus struct` besides the 16 it ran.

All three bugs were in the compiler, each with a case in `tests/test_lang.py`.
They turned up while writing the generator and the model (the first two by trying
the constructs, the third by reading how `ref` arguments are passed); against the
compiler as it was, the generator finds the first two in 25 of 600 programs.

- A part of a returned struct deeper than one field (`mk().p.x`, `mk().d[2]`)
  passed the semantic pass and then failed to compile ("this cannot be assigned
  to"), and so did `out mk().x` (`returned-struct-part`).
- A heap location with two runtime indices (`pool[i].d[j]`, `p[i].e[j].d[k]`) was
  rejected ("use a pointer for this second runtime index") although any index
  works in the heap; its address now adds up any number of scaled runtime
  indices (`heap-two-runtime-indices`).
- A `ref` argument outside cells (an element reached by a runtime index, a heap
  location) is copied in and back out, and the copy back computed its index
  again after the call: `f(i, a[i])` with `f` changing `i` wrote another element.
  Trampolined, the result depended on the order of the arguments, since the copies
  back run last to first. The index or address is now computed once, at the call:
  into temporaries for an inlined call, into hidden variables of the caller for a
  trampolined one (`ref-arg-place-fixed`). docs/LANG.md says so.

The final run of 3 000 BFL and 3 000 brainfuck programs (seed 200 000) found
nothing, nor did 500 programs each of `--focus shared` and `--focus struct`.
Still open: an assignment to a part of a returned struct (`mk().x = 1;`) is
rejected by code generation, not by the semantic pass, with the same message.

A review found that the generator's coverage figures counted declarations, not what
runs: two thirds of the programs declared a procedure that was never called (and an
uncalled inline procedure is not even compiled). Since then:

- Every procedure runs. `gen_bfl` runs the model, and a procedure that did not run
  gets a call statement, at the top level of the main program or of a later
  procedure that ran and may call it; in the main program the variables and arrays
  its `ref` and array arguments need are declared first when there are none, and
  printed after it. Up to three rounds, kept only while the model can still run
  the program.
- `f(i, a[i])` where `f` writes `i` is frequent, in both orders and trampolined
  too: a call chooses variables for some integer `ref` parameters first, and the
  places it passes to the others prefer indices that read them; a `ref` procedure
  often has two integer `ref` parameters, and writes each with probability 0.6.
  Against the compiler from before the fix (commit fb1ae6f, built in a temporary
  worktree), the generator now finds that bug in 6 of 600 programs and 5 of 400 of
  `--focus shared` (as wrong output; the other failures there are compile errors
  from the first two bugs and the rejected indices below); all 6 and 2 of the 5,
  shrunk, are calls such as `p0(mv4, ha0[mv4])` and `p0(ma17[mv16], mv16, ha0[0])`. As first built, the generator ran that shape
  in 0.2% of programs, and against the old compiler it found only the first two
  bugs.
- A false alarm in waiting: in an impure procedure an array parameter may be the
  global array of its element type that another argument of the same call names
  (`p0(ga0[g0 % 3], p1a0)` with `p1a0` bound to `ga0`: the model printed 6, the
  compiler 1). The no-shared-storage rule now counts an array parameter as any
  global array of its element type.
- The semantic pass rejected a runtime index into an array field of a struct
  that is in the heap behind an array parameter (`shared proc g(ref C a[], byte i,
  byte j) { a[i].d[j] = 1; }`, and an inlined procedure given a heap array) or in a
  re-entrant procedure's local array, with "an array field of a struct held in
  cells needs a constant index". It now leaves those to code generation, which
  knows where the array is and already took any number of runtime heap indices;
  an inlined procedure given an array in cells still fails, with "the array passed
  for 'a' is in cells: pass a heap array", and `mk().d[i]` with "an array field of
  a returned struct needs a constant index" (`heap-field-index-param` and three
  diagnostics in `tests/test_lang.py`). The generator indexes such fields at run
  time: a struct array parameter of an inlined procedure is sometimes given only
  heap arrays, a trampolined one always is.
- A failure of the fuzzer itself is written out (it crashed `save_failure`), the
  `bf.exe` path is made absolute, and the temporary folder is removed after a run
  without failures.

Executed coverage, measured by hooking the model (3 000 programs from seed
200 000; percent of programs, inlined / trampolined callee), before and after:

| what the program ran | before | after |
|---|---|---|
| a call with an array argument | 3.5 / 1.0 | 21.0 / 15.1 |
| ... a struct array | 1.2 / 0.4 | 7.4 / 4.9 |
| a call to a shared procedure | 9.7 | 24.9 |
| a `ref` argument that is a place, not a whole variable | 4.5 / 0.9 | 13.4 / 3.3 |
| `f(i, a[i])` with `f` writing `i` | 0.2 / 0.0 | 2.9 / 0.8 |
| a runtime index into an array field of a struct | 4.4 | 8.0 |
| ... through an array parameter | 0 | 1.4 |
| a whole struct copied | 29.1 | 38.5 |
| a place in a struct | 59.9 | 59.9 |
| a declared procedure that never ran | 67.4 | 6.8 |

With `--focus shared` (500 programs) shared calls ran in 43% before and 89% after,
array arguments to trampolined procedures in 2.8% and 43%, and 94% then 6.4% had a
procedure that never ran; with `--focus struct` whole struct copies ran in 47% and 65%.
After these changes a run of 2 000 BFL and 300 brainfuck programs (seed 600 000)
found nothing, nor did 500 programs each of `--focus shared` (seed 610 000) and
`--focus struct` (seed 620 000).

## 13. Decisions for you

1. **Pointer syntax.** C-style `byte* p`, `*p`, `p->x` is assumed. An alternative
   is Pascal-style `^byte`, or keeping `ref` and adding `ptr`.
2. **Integer names.** `sbyte`, `int`, `dword`, `long` is assumed. An alternative
   is explicit widths: `u8`, `i8`, `u16`, `i16`, `u32`, `i32`.
3. **Heap size default.** 8 KB is assumed, because every byte costs three cells
   and walking time.
4. **Memory management.** Manual `free` is assumed; no garbage collector.
5. **Checks.** Null, bounds and stack checks are opt-in flags, since each one
   costs on every access.

## 14. Risks

- **Heap access speed** decides how usable phases 3 to 7 are. Phase 0 exists to
  find out before anything depends on it.
- **Code size.** Each heap access is inlined at its call site. A program with a
  few hundred accesses could compile to megabytes of brainfuck. The JIT copes,
  since the IR folds pointer moves, but the generated-code window and `.b` export
  get unwieldy. If it bites, a later phase can route heap accesses through the
  trampoline as shared blocks, at the cost of a dispatch per access.
- **Width-generic arithmetic** in phase 2 touches every expression routine. The
  existing grids and golden files are the safety net, which is why phase 1 must
  leave the golden files byte-identical.
- **Scope.** This is several weeks of work. Each phase is useful on its own, so
  stopping after any of them leaves a working, documented language.
