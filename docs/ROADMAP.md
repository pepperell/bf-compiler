# Roadmap: BFL towards modern programming

Status: implemented (phases 0 to 8). Hardware and devices are out of scope here.
The sections below are the plan as written. Where the result differs, docs/HEAP.md
(phase 0), docs/TRAMPOLINE.md (phase 7) and the note under phase 8 say what was
built, and docs/LANG.md describes the language as it is.

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

## 11. Decisions for you

1. **Pointer syntax.** C-style `byte* p`, `*p`, `p->x` is assumed. An alternative
   is Pascal-style `^byte`, or keeping `ref` and adding `ptr`.
2. **Integer names.** `sbyte`, `int`, `dword`, `long` is assumed. An alternative
   is explicit widths: `u8`, `i8`, `u16`, `i16`, `u32`, `i32`.
3. **Heap size default.** 8 KB is assumed, because every byte costs three cells
   and walking time.
4. **Memory management.** Manual `free` is assumed; no garbage collector.
5. **Checks.** Null, bounds and stack checks are opt-in flags, since each one
   costs on every access.

## 12. Risks

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
