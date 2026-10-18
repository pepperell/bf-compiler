# BFL language reference

BFL is a small, unsigned, low-level language that compiles to brainfuck for this
machine. Files end in `.bfl`. The compiler lives in `src/lang/` and is built into
both `bf.exe` and `bfui.exe`:

```
bf prog.bfl                  compile and run
bf --emit prog.b prog.bfl    write the brainfuck (and prog.b.map: statement map, symbols)
bf --lang -e "out 'A';"      treat -e code or any file as BFL
bfui prog.bfl                open in the UI: F5 runs, Ctrl+F5 debugs by statement,
                             Ctrl+B shows the generated brainfuck
```

The design rule behind everything: **the compiler always knows where the pointer
is.** Every variable owns fixed cells, procedures are inlined at each call site,
and device registers are addressed by name from the register table in
`src/registers.h`. Recursion is available through `rec proc`, which compiles a
procedure as a trampoline with an explicit stack array (see "Procedures and
recursion"). There is no other indirect addressing except byte arrays, which use
a walking idiom described at the end.

## Lexical elements

- Comments: `// to end of line` and `/* block */`.
- Numbers: decimal `200`, hex `0x1F`, binary `0b1010`, character `'A'`.
  Character and string escapes: `\n \r \t \0 \e \a \b \\ \' \" \xHH`.
- Strings `"text"` appear only in `out` and `print`.
- Identifiers: letters, digits and `_`, case-sensitive. A name may not be a
  register name or a keyword: `byte word const proc rec ref return stack if else
  while for break continue out print pulse halt asm in input mulshr`.

## Types and declarations

```
byte x;              // one cell, 0..255; zero unless initialised
sbyte s = -5;        // one cell, -128..127
word w = 1000;       // two cells (low byte first), 0..65535
int i = -300;        // two cells, -32768..32767
dword d = 4000000000;   // four cells, 0..4294967295
long l = -100000;    // four cells, -2147483648..2147483647
byte buf[16];        // byte array; the index may be any expression
const MAX = 10;      // compile-time constant, occupies no cell
```

All integer types wrap on overflow (two's complement). Signed values are stored
in two's complement, so a `sbyte` holding -1 is the byte 255.

Variables are allocated upward from the first RAM cell (0x100). A variable
declared inside `{ }` exists until the closing brace and is cleared there, so a
loop body's locals start at zero on every iteration. Top-level variables keep
their cells for the whole program. Arrays cannot be initialised in the
declaration. Arrays in cells may hold any type (`int a[10]`, `Point pts[8]`);
a runtime index into one is a byte, so only the first 256 elements are reachable
that way. Larger arrays belong in the heap.

## Structs

```
struct Point { int x; int y; }
struct Node  { int val; Node* next; }        // a pointer to its own type is fine
struct Buf   { byte data[16]; byte len; }    // fixed-size array fields

Point p;                     // a struct in cells: fields are consecutive cells
p.x = 3;
Point q = p;                 // whole-struct copy
Point pts[8];                // an array of structs in cells
pts[i].y = -1;
heap Node pool[100];         // structs in the heap
Node* n = &pool[0];
n->next = &pool[1];          // -> through a pointer
print sizeof(Node);          // 4
```

Structs are declared at the top level. Fields are laid out in order with no
padding. A struct can be assigned, passed and returned by value, passed by `ref`,
stored in arrays and in the heap, and contain other structs and arrays; it
cannot contain itself (use a pointer). Structs cannot be compared or printed.
An array field of a struct held in cells needs a constant index; in the heap any
index works (`pool[i].data[j]`), also through an array parameter given a heap
array (`a[i].data[j]`; a trampolined procedure's always are) and in the local
arrays of a re-entrant procedure. A part of a returned struct can be read directly,
through fields and constant indices (`mk().x`, `mk().a.data[2]`).

## Pointers and the heap

Variables live in cells and have no address. The **heap** is a separate memory
of up to 64 KB, addressed by 16-bit pointers, where data can be reached through
an address computed at run time:

```
heap 16384;                  // heap size in bytes (top level; default 8192)
heap byte buf[100];          // a static array in the heap
heap int scores[50];         // heap arrays may hold any type
byte* p = &buf[10];          // the address of a heap element
*p = 'x';                    // write through a pointer
p[3] = p[2] + 1;             // index from a pointer
p++;  p += 4;                // pointer arithmetic is scaled by the element size
int* q = scores;             // a heap array's name is a constant pointer
word n = q - scores;         // a pointer difference counts elements
word* w = (word*)p;          // casts between pointer types, and to and from integers
if (p != null) { ... }       // null is address 0
```

Rules:

- A pointer is two bytes. `T*` points to a `T`; `byte*` (also written `string`)
  converts to and from any other pointer type silently, other pointer
  conversions give a warning, and integers need a cast (except the constant 0).
- `&` takes the address of anything in the heap: an element of a heap array, a
  location reached through a pointer, a field of a struct in the heap. Variables
  held in cells have no address.
- Address 0 is null and addresses 1 to 6 hold the allocator's bookkeeping;
  static data starts at 7. Accessing a null or out-of-range address is not
  checked unless the program is compiled with `--check-heap` (bf.exe) or Run >
  Check heap accesses (UI), which print `heap access out of range` and halt.
- Cost: an access through a runtime address walks the heap (docs/HEAP.md). With
  the default heap a byte access takes about 2 µs on the JIT and compiles to
  about 1.6 KB of brainfuck at each place it appears. An access whose address is
  known at compile time (a heap array with a constant index, `*p` after `p` was
  set to a constant) is a direct cell access with no walk.
- Each byte of a multi-byte value is a separate access, so reading a `long`
  through a pointer costs four walks.

## Allocation and strings

```
Node* n = new Node;          // a zeroed Node in the heap, or null when the heap is full
int* a = new int[100];       // an array
byte* raw = alloc(64);       // 64 zeroed bytes
free(n);                     // give a block back (free(null) does nothing)

string s = "hello";          // a string literal is a byte* to NUL-terminated bytes
print s, " ", s[1];          // print writes a string's text
```

`alloc`, `free` and `new` are part of the built-in prelude (`lib/prelude.bfl`),
written in BFL. The allocator is first fit over a free list with a two-byte size
header per block; big free blocks are split, freed blocks are not merged, and
there is no garbage collector. Blocks come back zeroed. The allocator's state
lives in heap bytes 1 to 6, so `heap N;` bounds everything. Identical string
literals share one copy; they are written into the heap at start-up, and changing
one through a pointer changes it for every use.

## Imports and the standard library

```
import "strings.bfl";        // the standard library
import "shapes.bfl";         // a file next to this one (or "lib/shapes.bfl" below it)
```

An import includes a file's procedures, structs, constants and variables at that
point, once: importing the same file again (including through a cycle) does
nothing. The path is looked up next to the importing file first, then in the
standard library, which is compiled into `bf.exe` and `bfui.exe` from `lib/`.
Names are global: a clash between two files is an error that names both places.
Warnings are not reported for the standard library.

| File | Contents |
|------|----------|
| `strings.bfl` | `strlen`, `strcpy`, `strcat`, `strcmp`, `strchr`, `strdup`, `utoa`, `ltoa`, `atol` |
| `math.bfl` | `min`, `max`, `abs`, `clamp` (int); `gcd`, `isqrt`, `pow` (dword) |
| `io.bfl` | `read_byte`, `read_line(buf, size)`, `read_long` |
| `memory.bfl` | `memset`, `memcpy`, `memcmp`, `heap_unused` |
| `prelude.bfl` | built in: decimal print and input, the string printer, `alloc`, `free` |

## Statements

```
x = e;      x += e;  x -= e;  x *= e;  x /= e;  x %= e;
x <<= e;    x >>= e; x &= e;  x |= e;  x ^= e;
x++;        x--;
if (c) { ... } else if (c) { ... } else { ... }
while (c) { ... }
for (init; c; step) { ... }     // init and step are assignments, ++/-- or calls
break;  continue;               // innermost loop
out e, "text", ...;             // write bytes to the terminal ('.')
print e, "text", ...;           // write numbers in decimal; strings as text
x = in;                         // read one byte (','); x unchanged at end of input
w = input;                      // read a decimal number up to the first non-digit
LEDS = x;   x = KEY_DATA;       // registers by name
pulse TICK_LATCH;               // write 1 to a register once
halt;                           // write HALT: ends the run with status "halted"
asm { <<+>> }                   // raw brainfuck; must leave the pointer where it was
proc name(byte a, ref word w, ref byte buf[]) { ... }
proc byte sq(byte x) { return x * x; }       // a procedure with a result
name(a + 1, w, buf);            // call: inlined here
y = sq(3) + sq(4);              // calls in expressions
return;  return e;              // leave a procedure (or end the main program)
rec proc byte fib(byte n) { ... }            // may call itself: see below
shared proc glow(long x, long y) { ... }     // one copy, called through the trampoline
stack 64;                       // cells of recursion stack (default 128, top level)
{ ... }                         // a block with its own scope
```

Braces are required around every body. Top-level statements run in order; the
program ends after the last one or at a top-level `return;`.

## Procedures and recursion

Procedures are declared at the top level and may appear after their first use.
Scoping is lexical: a procedure body sees its parameters, its own locals, and the
top-level variables and constants declared before the procedure. A call from the
main program must also come after the declarations of every top-level variable
the procedure uses, directly or through procedures it calls.
`proc byte f(...)` and `proc word f(...)` return a value with `return e;`; a
procedure without a result type uses plain `return;`, and a procedure that
reaches its closing brace returns 0. A call with a result can be used as an
expression; a call statement discards the result. Parameters are passed by value
unless marked `ref`; a `ref` argument must be a variable (or any other
assignable location, such as `p->x`) of the same type, and an array parameter
(`ref byte name[]`) must be an array of that element type. A `ref` argument held
in cells is the variable itself; one elsewhere (an element reached by a runtime
index, a location in the heap) is copied in at the call and back out when it
returns, to the place it was read from: its index or address is computed once, at
the call.

A plain `proc` is **inlined** at every call: its body is copied into the caller
with its own locals, so it cannot call itself. A value parameter of an integer
type that the body never assigns (nor passes to a `ref` parameter) and that
receives a constant argument becomes a constant in that copy, so everything
computed from it folds, inside loops and branches too: `plot(x, 3)` costs no
more than writing the 3 into the body by hand. A `rec proc` may call itself and
other `rec proc`s freely, and a `shared proc` exists once in the output however
often it is called; both are compiled as a **trampoline** (docs/TRAMPOLINE.md):

- Its body is cut into numbered blocks at every call to a `rec` or `shared`
  proc. A state cell names the next block and one dispatcher loop runs them.
  Every procedure that calls one, and the main program when it does, is
  compiled the same way (the compiler promotes them automatically). When the
  rest of a statement list calls none, it stays ordinary code in one block.
- The current activation's parameters and locals live in fixed cells, so the
  watch list shows them by name (`fib.n`). Procedures that can never be active
  at the same time share those cells, laid out like a call stack, and a return
  leaves its cells zero.
- A procedure that can come back to itself through calls (it is in a cycle) is
  *re-entrant*: a call within its cycle saves the caller's parameters and
  locals to the save stack with the state to resume, and a return restores
  them. Any other procedure keeps its resume state in a cell of its own, and a
  call to it saves nothing (the stack pointer moves by one so the debugger's
  step over still works). `stack N;` at the top
  level sets its size in cells (default 128, at most 32768); each call uses one
  cell per byte of parameters and locals, plus one (two above 254 blocks). Up
  to 256 cells the stack is a fast byte-indexed array; above that it uses the
  heap's walking layout with a 16-bit stack pointer, which costs more per call.
  Overflow is not checked unless the program is compiled with `--check-stack`
  (bf.exe) or Run > Check recursion stack (UI), which print
  `recursion stack overflow` and halt.
- Local arrays of a re-entrant procedure live in the heap: each call takes
  their space from the top of the heap (the allocator's end-of-heap word moves
  down), zeroes it, and gives it back on return; those of any other trampolined
  procedure live in its cells and are cleared at the declaration. Array
  parameters (`ref byte a[]`) are passed
  as pointers, so the argument must be a heap array or another such parameter
  or local; an array held in cells has no address.
- `ref` parameters are copied in at the call and copied back when it returns,
  which works for any assignable argument, including heap locations; the place
  is fixed at the call, as for an inlined procedure.
- Variables declared in code that calls a `rec` or `shared` proc must have
  names unique within that procedure; the right operand of `&&` / `||`
  containing such a call is always evaluated, and calls in an expression run
  before the rest of it.
- Cost: a call to a small procedure runs about 450 IR instructions (60 ns on
  the JIT) with nothing to save; within a cycle add the frame saved and
  restored (a one-byte recursive call is about twice that). See
  docs/TRAMPOLINE.md.

A plain `proc` that calls itself without `rec` is a compile error, and so is a
`shared proc` that calls itself.

**When `shared` pays.** Sharing removes the copies of the body at every call
site, but trampolines the callers, all the way up to the main program, which
then pay the dispatcher, lose the compiler's knowledge of cell values at every
call, and keep the variables that live across a call in fixed cells. It suits
a sizeable procedure called from many places that are not themselves deep in
hot loops. In `programs/samples/raytracer.bfl`, sharing `glow` (13 call sites)
makes the program 2% smaller at the same speed; sharing `isqrt`, called inside
the per-pixel loops, makes it 15% larger and 45% slower.

`print` of a `word` costs several 16-bit comparisons; `print` of a byte is a
division by 100 and by 10.

## Expressions

Operators, loosest binding first:

| Precedence | Operators |
|-----------:|-----------|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `\|` |
| 4 | `^` |
| 5 | `&` |
| 6 | `==` `!=` |
| 7 | `<` `<=` `>` `>=` |
| 8 | `<<` `>>` |
| 9 | `+` `-` |
| 10 | `*` `/` `%` |
| unary | `-` `!` `~` and the casts `byte(e)` `sbyte(e)` `word(e)` `int(e)` `dword(e)` `long(e)` or `(T) e` |

Rules:

- **Operand types.** The two operands of an arithmetic or comparison operator
  are converted to a common type: the wider of the two; at equal width, signed
  only if both are signed (mixing signed and unsigned of the same width is
  unsigned and gives a warning). A constant takes the type of the other
  operand when its value fits, so `s < -1` compares signed bytes and `w + 1`
  stays a `word`. `byte + byte` wraps at 256: write `word(a) + b` for a 16-bit sum.
- **Shifts** have the type of their left operand. `>>` is arithmetic (copies the
  sign bit) on signed types and logical on unsigned ones. Shifting by the width
  or more gives 0, or -1 for a negative signed value shifted right; so does a
  negative count. The count keeps its own type, so `b << w` with `w` a `word`
  holding 256 is 0.
- **Division** truncates towards zero, and `%` takes the sign of the dividend,
  as in C: `-7 / 2` is -3 and `-7 % 2` is -1. Division by zero gives 0 and the
  remainder is the dividend.
- **Conversions.** Widening a signed value copies its sign (`sbyte` -3 becomes
  `int` -3, or `word` 65533); widening an unsigned value fills with zeros.
  Narrowing keeps the low bytes and gives a warning unless written as a cast.
  Casts convert between any integer types.
- **Constants.** Numbers are typed by value: the smallest of `byte`, `word`,
  `dword` that holds them, or `sbyte`, `int`, `long` when negative; an `L`
  suffix (`5L`) makes a `long`. Constant expressions are folded at compile time.
  A constant must fit its target: `int i = 40000;` is an error, while an unsigned
  target also accepts a negative constant (`byte x = -1;` is 255).
- Comparisons, `!`, `&&` and `||` give 0 or 1 (a `byte`). Any non-zero value is
  true. `&&` and `||` evaluate their right operand only when needed.
- `in` reads one byte; `input` reads a decimal number as a `word`. Both have
  side effects and are evaluated where they appear.
- **`mulshr(a, b, k)`** multiplies and shifts in one step, for fixed-point
  arithmetic (see below).
- `print` writes any integer type in decimal, with a `-` for negative values.

## Fixed point: mulshr

```
long px = ox + mulshr(dx, t, 12);       // ox + d * t, with d in units of 1/4096
int  x1 = mulshr(x, cosine, 10) + mulshr(z, sine, 10);   // the product needs 32 bits, the result 16
```

`mulshr(a, b, k)` is `(a * b) >> k` computed exactly:

- `a` and `b` are converted to their common type, as for `*` (a constant takes the
  other operand's type when it fits); the result has that type.
- The product is exact: twice the type's width, signed when the type is signed,
  so nothing wraps before the shift. The low bytes of the shifted product are the
  result, wrapped to the type like any other value.
- The shift rounds down (towards minus infinity, as `>>` does on a signed value):
  `mulshr(-3, 5, 1)` is -8.
- `k` must be a constant, 0 or more. `mulshr(a, b, 0)` is `a * b`. A count of the
  width or more takes the high part of the product (`mulshr(x, y, 32)` on `long`s is
  its top 32 bits); from twice the width up every bit is shifted out and the result
  is 0, or -1 for a negative product.
- So whenever `a * b` does not overflow, `mulshr(a, b, k)` equals `(a * b) >> k`;
  when it does, `mulshr` still gives the right answer. Two constants fold exactly.

Cost: a shift-and-add loop that runs once per bit of `|b|`. Unlike `*`, which
compares its operands and loops over the smaller, `mulshr` takes them as written, so
put the operand with the smaller magnitude second. Each step adds `|a|`, shifted,
into an accumulator that keeps only the bytes that survive the shift (and one below
them for the bits): after
each byte of `b` whose bits are all below the shift, the accumulator's low byte is
final and is dropped; the last `k % 8` bits are one pass of byte divisions. A Q12
product of two `long`s runs about 1 100 IR instructions and compiles to about 12 000
commands, against 1 900 and 18 000 for `(a * b) >> 12` (`tools/bench.py`); for `int`
it is 490 and 4 900 against 690 and 10 000, since `>>` on an `int` is unrolled bit by
bit. Each further bit of `|b|` adds about 100 IR instructions: with `b` near 2^31
and `a` around 5 000, `mulshr(a, b, 12)` runs about 2 900 against 1 800 for
`(a * b) >> 12`, and 1 100 with the operands the other way round. A constant factor
runs the same loop, which is slower than the unrolled constant multiply of `*`:
`mulshr(a, 218, 8)` on `long`s runs about 690 IR instructions against 560 for
`(a * 218) >> 8`, though it is the smaller code (about 8 000 commands against
12 000). So `mulshr` pays for two variables in the right order, and wherever `a * b`
could overflow; a constant factor whose product fits is faster with `*` and `>>`.

## Registers

Register names come from `src/registers.h`; `bf --regs` and Help > Memory map
list them. Each has a kind that decides what code may touch it, because a
brainfuck program can only ever read a cell, add one, or subtract one:

- **Latch** registers (LEDS, KEY_DATA, TICKS0..3, NUMBER0/1, RANDOM, SER_DATA,
  SER_BAUD, FB_X, FB_Y, FB_COLOR) read back what was written and have no write
  side effects. They work like variables: `LEDS = x`, `x = LEDS + 1`, `LEDS++`.
  A write of value *v* is emitted as *v* increments, so a display register
  passes through the intermediate values.
- **Status** registers (KEY_STATUS, SER_STATUS, SER_CTRL) are readable but every
  write has a side effect. In an expression they are *tested*, never written:
  `if (KEY_STATUS)`, `while (SER_STATUS)`, `x = !SER_STATUS`, `b = KEY_STATUS`
  (which gives 0 or 1, not the raw value). Write them only with `pulse REG`,
  `REG += n` or `REG -= n` where *n* is a constant: `SER_CTRL += 1` opens the
  port, `SER_CTRL -= 1` closes it, `pulse KEY_STATUS` pops a key.
- **Write-only** registers (CONOUT, HALT, TICK_LATCH, ROLL, SER_TX, FB_PLOT,
  FB_FILL, FB_READ) accept only `pulse`, which writes 1, or `REG += n`. Reading
  one is a compile error.

The usual patterns:

```
while (KEY_STATUS) { pulse KEY_STATUS; c = KEY_DATA; ... }   // drain the keyboard
SER_DATA = c; pulse SER_TX;                                   // send a byte
while (SER_STATUS) { pulse SER_STATUS; out SER_DATA; }        // print received bytes
pulse TICK_LATCH; t = TICKS0;                                 // sample the clock
```

`while (REG)` and `if (REG)` test the register without writing it: the compiler
loops on the register cell directly, or uses a pointer-resynchronising idiom (see
below) that only reads it.

## Errors and warnings

The compiler checks the whole program before generating any code and reports
every error it finds, up to 20, as `file:line:col: message` in source order.
After a syntax error it skips to the next `;` or `}` and carries on, so one
missing semicolon does not hide the rest. Syntax errors are reported first;
the meaning of the program is checked once it parses.

Warnings are off by default in `bf.exe` (`--warn` turns them on) and always on in
the UI:

- a variable that is never read;
- a value truncated by an assignment, argument or return (`byte b = w;`);
- unreachable code after `return`, `break` or `continue`;
- combining signed and unsigned values of the same width, which is unsigned.

In the UI the errors and warnings are listed in a pane under the editor and the
status bar shows the first one; double-click a row, or press F4 and Shift+F4, to
step through them and select the line, also in imported files.

## The debugger

In the UI a `.bfl` program is debugged by statement. F11 steps to the next
statement, entering procedure calls; F10 steps over the whole statement (a loop
runs to completion, a call runs its body, and a recursive call runs until the
stack pointer is back at the current depth); Ctrl+F10 runs to the statement at
the caret; F9 toggles a breakpoint on the caret's line. The current statement is
highlighted, the status bar shows its line, and the watch list is filled with
the program's global variables by name (arrays up to their first 16 elements),
each shown whole: wide and signed values in one row, pointers with the heap byte
they point at. When the current statement is in an imported file, that file is
shown read-only in the editor's place; the toolbar's File box switches between
the program and its imports, and breakpoints can be set in imported files.
A breakpoint stops where a statement starts, not where one split by a recursive
call or by shared arithmetic ("Idioms used by the compiler") goes on.
`print` and `input` are implemented as library procedures written in BFL; the
debugger does not step into them.

## What the output looks like

The generated brainfuck is readable: each statement is preceded by a comment
line `# L<line>: <source text>` (with the eight command characters replaced by
`_`), indented by nesting depth. `bf --emit` also writes a `.map` file listing,
per statement, the source line and the byte range of generated code, plus the
symbol table.

Cost model, roughly, per operation on bytes:

| Operation | Generated commands | Executed (worst case) |
|-----------|-------------------:|----------------------:|
| `x = k`, `x += k` | 1..130, usually few | same |
| `x = y`, `x += y` | ~20 | ~4·y |
| `x = y * z` | ~30 | ~3·y·z |
| `==`, `!=` | ~30 | ~4·(y+z) |
| `<`, `<=`, `>`, `>=` | ~60 | ~40·min(y,z) |
| `/`, `%` | ~80 | ~40·y |
| `<<`, `>>` by 1 | ~20 | ~5·y |
| `&`, `\|`, `^` | ~500 | ~5000 |
| `a[i]` read / write | ~70 | ~15·i |

Word operations are 5 to 20 times a byte's; word `/` and `%` loop once per bit
of the dividend (16 passes for a word, 32 for a long). The executed column is
what a plain brainfuck interpreter does. On this machine the IR optimiser turns
the common idioms (clear, move, copy, constant multiply) into single
instructions, and it also recognises the loops whose trip count is a value: the
byte compare, the carry and borrow chains of multi-byte `+` and `-`, halving
(shifts, bitwise operators) and byte division each run as one step (`src/ir.cpp`,
"loop idioms"), and so does the whole multi-byte division loop ("the division
loop"). Measured in IR instructions executed, a 32-bit `+` is about 40, `<` about
120, `*` 1 700 to 2 400 (more for larger operands) and `/` about 200, against
10 000, 2 300, 129 000 and 290 000 without those idioms. Several of the same
16- or 32-bit `*`, `mulshr`, `/` or `%` in one sum or run of statements share
one copy of their code
("Shared arithmetic" below): three `long` products in a sum are about 15 000
commands instead of 34 500.
`bf --profile` (Run > Profile in the UI) shows where a program's instructions go, by statement. The compiler also tracks cells whose value it knows in straight-line
code: constants, string output and copies of known variables are emitted as the
shortest delta, and expressions over known variables fold entirely.

## Idioms used by the compiler

These are the building blocks; knowing them helps read the output and write
`asm` blocks that cooperate with it.

- **Temporaries** come from a stack just above the variables and are always
  returned to zero, so a fresh temporary is known to be 0.
- **Layout of globals.** The pointer reaches a cell one step at a time, so a
  variable costs roughly its references times its distance from the
  temporaries. Each run of top-level declarations with no other statement
  between them is laid out together when the first is reached: by references
  per cell (a procedure's body counts once per inlined call), the densest
  nearest the temporaries. The order of declarations within such a run does not
  matter; a declaration after other statements still starts a new run above
  the variables that exist then.
- **Clear** `[-]`, **move** `a[-b+a]`, **copy** `a[-b+t+a]t[-a+t]`.
- **If** `c[ body c[-] ]` on a temporary copy of the condition;
  **if/else** adds a flag cell set to 1 and cleared by the taken branch.
- **While** `c[ body ; recompute c ]`; a body containing `break` or
  `continue` gets an `alive` flag that guards the statements after the exit
  point, and `break` also sets a flag that clears the loop cell.
- **Register test without a write**: `reg[ flag+ P ]` leaves the pointer on
  `reg` (0) or on `P` (0); moving `M - reg` cells right then lands on `M`,
  prepared with 1, or on `P + M - reg`, which is free and 0; `[- >·D ]` with
  `D = P - reg` brings the first case to the same cell. Costs about 70 commands.
- **Division** of words and longs (`n` bytes) is one loop on a block of
  `12n + 2` temporaries (`bf::division_loop` and `bf::DivisionLayout` in
  `src/bf.h`): the pass counter, set to `8n`, then six cells for each byte of
  `A` (the dividend, which becomes the quotient), `R` (the remainder) and a
  borrow byte `X`: the byte, two cells for its zero test, a counter, and in R's
  groups a byte of the divisor `B` and a temporary. Each pass shifts `A:R` left
  by one bit, high byte first (a byte counts its old value up twice, and when it
  wraps the byte above gets the carry), subtracts `B` from `R` one unit at a time
  with a borrow chain into `X`, sets `A`'s low bit, and on a borrow adds `B`
  back and clears the bit again. A byte's zero test is `t+ c[>-]>[- … >]` with
  `t` and the cell after it free: it ends on that cell either way, without
  looping over `c`, so the loop runs in time linear in the values (as written,
  from about 0.4 to 2.8 million commands for a 32-bit `/`, more for larger
  operands). The optimiser recognises the loop by its exact text and runs all
  the passes in one step; signs, division by zero and the moves in and out are
  done around it.
- **Shared arithmetic.** A 16- or 32-bit `*`, `mulshr`, `/` or `%` is thousands
  of commands (a 32-bit `*` about 11 000), so when a block would emit the same
  routine (operator, width, signedness, and for `mulshr` the count) at least
  twice, it emits it once, inside a loop that runs once per operation: a counter
  cell runs the loop, and a flag per operation chooses the code after the
  routine that takes its result and evaluates the next operation's factors into
  the routine's operand cells. Two places do this. In one sum, such as
  `a * b + c * d - e * f`, the products go straight into the sum. In a run of
  assignments and declarations without calls, each operation in them (a term of
  a sum, or inside a shift, a cast or another operation's operands) is computed
  into cells of its own as the loop reaches it, and once a statement's
  operations are done the statement itself runs, reading those cells; up to two
  statements without such an operation may sit between two that have one. The
  first operation's factors are evaluated before the loop and the last
  statement after it, with the cell values the compiler knows; the other
  statements run inside the loop without them. Values known before the loop
  that no statement in it writes are known again after it. A constant factor
  is not shared (it becomes shifts and adds), nor is an operation that reads a
  variable whose value the compiler knows: a run ends before its statement,
  which can then fold it or use a constant factor, and its result is known. One
  loop runs at most 16 operations, because the code between the loop's flags
  and the variables grows with their number: a sum with more is cut into
  several loops, and a run ends before the statement that would pass 16.
  `BFL_SHARE=N` in the environment of `bf` sets the fewest operations shared
  (1 counts as 2), and 0 or less turns sharing off, for measuring. In the
  statement map each statement of a run keeps a range where it starts, where
  the debugger stops; the loop's own code, the routine included, is a
  continuation of the run's first statement, so `bf --profile` counts the
  routine's work on that statement's line.
- **Byte arrays** occupy `3·(n+1)` cells: a zero header triple, then per element
  `(C, V, D)`: counter, value carry, data. A read moves the index into `C₀`,
  walks `[-[->>>+<<<]+>>>]` leaving markers, copies `D` into `V` using `C` as the
  temporary, and walks back `<<<[->>>>[-<<<+>>>]<<<<<<<]` carrying the value,
  clearing the markers. A write carries index and value down together and
  returns over the markers. Element *k*'s data cell is `base + 3 + 3k + 2`.
- **Arrays of wider elements** use the same walk with a group of `K = 1 + 2s`
  cells per element of `s` bytes: `(C, V₀…Vₛ₋₁, D₀…Dₛ₋₁)`, after a zero header
  group. One walk carries every byte that is read or written (a field of a
  struct element carries only its own bytes), so a `long` element costs one walk,
  not four. Byte *b* of element *k* is at `base + K·(k+1) + 1 + s + b`.

## Limits and non-features

- No function values, no array bounds checks for runtime indices into cell
  arrays (a constant index is checked), no garbage collection.
- `asm` blocks are not analysed: they must leave the pointer where it was, and
  the compiler forgets every known cell value after one.
- The compiler stops at the first error and reports `line:col: message`.
