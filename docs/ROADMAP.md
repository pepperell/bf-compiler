# Roadmap: BFL towards modern programming

Status: implemented (phases 0 to 9); phases 10 to 14 are planned (section 11).
Hardware and devices are out of scope here. The sections below are the plan as
written. Where the result differs, docs/HEAP.md (phase 0), docs/TRAMPOLINE.md
(phase 7) and the note under phase 8 say what was built, and docs/LANG.md
describes the language as it is.

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
| 13 | Planned: the profiler in the UI | nothing |
| 14 | Planned: housekeeping (the raytracer animation) | nothing |

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

## 11. Phases 9 to 14 (9 to 12 built, the rest planned)

After phase 8 the optimiser learnt loop idioms (multi-byte carries, compares,
halving and byte division each run in one step), known cell values carried
across blocks, automatic layout of globals, one-walk array elements, folding of
constant arguments and `shared proc` (README.md "Speed", docs/LANG.md). Where
that leaves things, measured on `programs/samples/raytracer.bfl`: a frame takes
70 ms (2.3 s before), the program is 5.67M brainfuck commands (7.92M before), a
32-bit `*` executes about 2 400 IR instructions and a `/` about 7 000, and long
arithmetic is most of both the code and the run time.

### Phase 9: differential fuzzer for the optimiser and JIT

The loop idioms, the block rebuilding and the inline idiom code in the JIT are
the most intricate parts of the executor, and each is checked by a handful of
test programs. `tests/fuzz_shared.py` (shared against inlined procedures) found
a miscompile on its first run that no hand-written test had: a dangling pointer
that made the same source compile differently now and then.

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
counts 0 to 70 (including 7, 8, 12, 15, 16, 31 and the width and twice it), targeted
cases (constant folding, operand order, known values, a `rec` call inside, errors),
and `MulShr` in the fuzzer's generator and model; `--bfl 1500 --bf 300` ran clean.

Measured with `tools/bench.py` against the commit before:

| | before | after |
|---|---:|---:|
| `(long * long) >> 12` → `mulshr`, IR instructions | 1 893 | 1 088 |
| the same, commands | 18 077 | 11 864 |
| `(int * int) >> 7` → `mulshr`, IR instructions | 690 | 487 |
| the same, commands | 9 963 | 4 932 |
| `raytracer.bfl`: ms a frame / light map / commands | 75.0 / 175 / 5.67 M | 66.3 / 160 / 5.12 M |
| `raytracer_text.bfl` | 36.5 / 174 / 5.18 M | 30.1 / 153 / 4.64 M |
| `cube.bfl`: ms a frame / commands | 1.52 / 537 K | 1.40 / 388 K |
| `starfield.bfl` | 1.25 / 1.81 M | 1.19 / 1.80 M |

The raytracers use it for every single product and for the dot products (a sum of
`mulshr` terms rounds each term down: rendered headless against the commit before,
0.8 to 2.2% of a frame's pixels change, 1.5% over 16 frames; two thirds of those by
one level of a colour channel (a dither step), the rest by up to 7 levels, mostly in
the spheres' reflections, where a ray now meets a slightly different point; a lamp
moves by a pixel in three of the 16 frames (4, 8, 12). The pictures look the same).
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
minimum, 0 turns it off). Variants measured on the samples (only these four change):

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
loop with `continue` and `break`, in a `rec proc`), and `share-map`: a run is smaller than
with `BFL_SHARE=0`, prints the same, has a step mark for each statement and `bf --profile`
counts each line. The fuzzer generates runs of statements with several operations of one
kind (`shared_run`) and runs every program compiled with `BFL_SHARE=0` as well; `--bfl 2000
--bf 300` (seed 20000) found nothing, and 1 151 of the 2 000 programs shared arithmetic.

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
code went from 5.67 M to 4.06 M commands and its frame from 75 to 54 ms.

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
| `raytracer.bfl`: ms a frame / light map / commands | 65.3 / 158 / 5.12 M | 57.8 / 89 / 4.80 M |
| `raytracer_text.bfl` | 29.5 / 152 / 4.64 M | 24.9 / 85 / 4.31 M |
| `cube.bfl`, `starfield.bfl`: ms a frame | 1.38, 1.16 | 1.35, 1.08 |

The unoptimised count is for the benchmark's operands (`892200 / 1006`); the loop's
passes cost more the larger the values, so the loop alone runs from about 0.4 M commands
(`123456789 / 10`) to 2.8 M (`0xFFFFFFFF / 0xFFFFFFFF`).

A raytracer frame executes 181 M IR instructions instead of 225 M: its 8 000 divisions
took about 45 M and now take 1.6 M. (Against the commit before phase 10 a frame went
from 75 to 58 ms.) `text.bfl` grows by 3% although its `dword / 10` shrinks from 10.7 K
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

### Phase 14: housekeeping

`docs/raytracer.gif` (16 frames of the raytracer) is not committed and was
recorded when a frame took 2.3 s. Re-record it now that a frame takes 70 ms, and
commit it with a mention in README.md, or drop it.

## 12. Decisions for you

1. **Pointer syntax.** C-style `byte* p`, `*p`, `p->x` is assumed. An alternative
   is Pascal-style `^byte`, or keeping `ref` and adding `ptr`.
2. **Integer names.** `sbyte`, `int`, `dword`, `long` is assumed. An alternative
   is explicit widths: `u8`, `i8`, `u16`, `i16`, `u32`, `i32`.
3. **Heap size default.** 8 KB is assumed, because every byte costs three cells
   and walking time.
4. **Memory management.** Manual `free` is assumed; no garbage collector.
5. **Checks.** Null, bounds and stack checks are opt-in flags, since each one
   costs on every access.

## 13. Risks

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
