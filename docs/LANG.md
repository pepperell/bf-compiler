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
  while for break continue out print pulse halt asm in input`.

## Types and declarations

```
byte x;              // one cell, 0..255, wraps; zero unless initialised
byte y = 7;
word w = 1000;       // two cells (low byte first), 0..65535, wraps
byte buf[16];        // byte array; the index may be any expression
const MAX = 10;      // compile-time constant, occupies no cell
```

Variables are allocated upward from the first RAM cell (0x100). A variable
declared inside `{ }` exists until the closing brace and is cleared there, so a
loop body's locals start at zero on every iteration. Top-level variables keep
their cells for the whole program. Arrays cannot be initialised in the
declaration and `word` arrays are not supported (use two byte arrays).

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
unless marked `ref`; a `ref` argument must be a variable of the same type, and
an array parameter (`ref byte name[]`) must be a byte array.

A plain `proc` is **inlined** at every call: its body is copied into the caller
with its own locals, so it cannot call itself. A `rec proc` may call itself and
other `rec proc`s freely; it is compiled as a **trampoline** (docs/TRAMPOLINE.md):

- Its body is cut into numbered blocks at every call to a `rec proc`. A state
  cell names the next block and one dispatcher loop runs them. Every procedure
  that calls a `rec proc`, and the main program when it does, is compiled the
  same way (the compiler promotes them automatically).
- The current activation's parameters and locals live in fixed cells, so the
  watch list shows them by name (`fib.n`). A call saves them to a stack array
  with the state to resume and a return restores them. `stack N;` at the top
  level sets the array size in cells (default 128, at most 256); each call uses
  one cell per parameter and local plus one. Overflow is not checked.
- Restrictions: a `rec proc` cannot own arrays (declare them at the top level)
  and cannot take array parameters; its `ref` parameters are copied in at the
  call and copied back when it returns; variables declared in code that calls a
  `rec proc` must have names unique within that procedure; the right operand of
  `&&` / `||` containing such a call is always evaluated.
- Cost: a call is about 1 000 commands plus 70 per cell saved and restored;
  see docs/TRAMPOLINE.md for measurements.

A plain `proc` that calls itself without `rec` is a compile error.

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
| unary | `-` `!` `~` `byte(e)` `word(e)` |

Rules:

- Everything is unsigned. An expression is a **word** if any operand is a word
  variable, `input`, a `word(...)` cast or a constant above 255; otherwise it is
  a **byte**. `byte + byte` wraps at 256: write `word(a) + b` for a 16-bit sum.
  Assigning a word to a byte keeps the low byte; assigning a byte to a word
  zero-extends.
- Comparisons, `!`, `&&` and `||` give 0 or 1. Any non-zero value is true.
  `&&` and `||` evaluate their right operand only when needed.
- `/` and `%` by zero give 0 and the dividend respectively. Shifts by 8 or
  more (16 or more for words) give 0.
- `in` reads one byte; `input` reads a decimal number. Both have side effects
  and are evaluated where they appear.
- Constant expressions are folded at compile time. A constant that does not fit
  the target (`byte x = 300;`) is an error.

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

In the UI the status bar shows the first error and how many there are; F4 and
Shift+F4 step through errors and warnings and select the line.

## The debugger

In the UI a `.bfl` program is debugged by statement. F11 steps to the next
statement, entering procedure calls; F10 steps over the whole statement (a loop
runs to completion, a call runs its body, and a recursive call runs until the
stack pointer is back at the current depth); Ctrl+F10 runs to the statement at
the caret; F9 toggles a breakpoint on the caret's line. The current statement is
highlighted, the status bar shows its line, and the watch list is filled with
the program's variables by name (arrays up to their first 16 elements).
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

Word operations are 5 to 20 times a byte's; word `/` and `%` loop once per unit
of the dividend. The IR optimiser turns the common idioms (clear, move, copy,
constant multiply) into single instructions, so straight-line byte code runs at
JIT speed. The compiler also tracks cells whose value it knows in straight-line
code: constants, string output and copies of known variables are emitted as the
shortest delta, and expressions over known variables fold entirely.

## Idioms used by the compiler

These are the building blocks; knowing them helps read the output and write
`asm` blocks that cooperate with it.

- **Temporaries** come from a stack just above the variables and are always
  returned to zero, so a fresh temporary is known to be 0.
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
- **Byte arrays** occupy `3·(n+1)` cells: a zero header triple, then per element
  `(C, V, D)`: counter, value carry, data. A read moves the index into `C₀`,
  walks `[-[->>>+<<<]+>>>]` leaving markers, copies `D` into `V` using `C` as the
  temporary, and walks back `<<<[->>>>[-<<<+>>>]<<<<<<<]` carrying the value,
  clearing the markers. A write carries index and value down together and
  returns over the markers. Element *k*'s data cell is `base + 3 + 3k + 2`.

## Limits and non-features

- No function values, no signed numbers, no `word` arrays, no array bounds
  checks for runtime indices (a constant index is checked), no stack overflow
  check for recursion.
- `asm` blocks are not analysed: they must leave the pointer where it was, and
  the compiler forgets every known cell value after one.
- The compiler stops at the first error and reports `line:col: message`.
