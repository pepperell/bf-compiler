# Recursion in BFL: the trampoline design

**Status: implemented** as `rec proc` (see docs/LANG.md, "Procedures and
recursion"). The compiler follows section 3 below with these choices: the
dispatcher is a countdown chain over all blocks of all trampolined procedures
plus the main program; `ref` parameters are copy-in / copy-out; the stack array
sits just above the device window so variables and temporaries stay adjacent;
the debugger's step-over waits for the stack pointer to return to its value at
the call. Measured on `programs/hanoi.bfl` and `programs/quicksort.bfl`:
quicksort of 16 bytes compiles to 110 000 commands and runs in 8 ms on the JIT.

Phase 7 of docs/ROADMAP.md extended this:

- **Save stack.** Up to 256 cells it stays the byte-indexed cell array. Above
  that it is a region in cells with the heap's walking layout and a 16-bit stack
  pointer; the address of a frame is computed once per call and stepped. The
  plan's idea of keeping saved frames in the heap itself was measured and
  dropped: every call would cost about a hundred times more. fib(20) runs in
  74 ms on the byte stack and 122 ms on the 16-bit one.
- **Local arrays** of trampolined procedures take their space from the top of the
  heap at entry (the allocator's end-of-heap word is the frame pointer), are
  zeroed, and are released on every return path. **Array parameters** are
  pointers.
- **More than 254 blocks** switches to a 16-bit state: the dispatcher picks the
  group of 256 by the high byte, then the block by the low byte.
- `--check-stack` adds an overflow check before every push (and before taking
  heap space for local arrays).

Later, `shared proc` (one copy of a procedure that may not recurse) brought
these changes, which also apply to `rec proc`:

- **Re-entrant or not.** The compiler works out which trampolined procedures
  can reach themselves through calls. A call saves the caller's frame only when
  caller and callee are in one cycle; the resume state goes on the stack only
  for a re-entrant callee, and otherwise into a cell of the callee's own. The
  stack pointer still moves by one on such a call so that the debugger's step
  over can wait for it. Local arrays of procedures outside a cycle stay in cells.
- **Frames like a call stack.** Each frame sits just above the highest frame of
  anything that can call it, so procedures that are never active together share
  cells. A return clears its frame (the caller's resume code moves the result and
  `ref` parameters out), so the cells above a running procedure's frame are zero
  and its temporaries start right there, instead of above every frame of the
  program. The main program's frame (the globals) is ordered by the trips the
  code makes to them, as in inlined code, and the globals that cost less from
  above leave it for the region above the temporaries (docs/LANG.md, "Layout of
  globals"); the frames above it move down by what left.
- **Flat dispatcher.** The countdown runs as one sequence of tests instead of
  nested ones, so every block's temporaries start at the same height, and the
  dispatcher's own cells sit below the frames.
- **Plain runs.** When the rest of a statement list calls no trampolined
  procedure, it is generated as ordinary code in one block, its variables scoped
  locals rather than frame cells.

With these, `mergesort.bfl` went from 272 000 to 190 000 commands, `calc.bfl`
from 1 048 000 to 963 000 and `bst.bfl` from 639 000 to 605 000.

BFL inlines every procedure because brainfuck has no jump to an address, and
because the compiler addresses every variable as a fixed cell. This note works
out how recursion could be added anyway, measures a hand-made version, and
estimates the cost of building it into the compiler.

## 1. The idea

Brainfuck can do one kind of control flow: loop while a cell is non-zero. A
function that may be entered from many places, and may re-enter itself, is
turned into a **state machine**:

- The function body is cut at every call into numbered *segments*. Segment 0 is
  the entry; the segment after a call is where that call returns to.
- A `state` cell says which segment runs next. One loop, the **dispatcher**,
  runs `while (state != DONE) { if (state == 0) {...} else if (state == 1) {...} ... }`.
- The current activation's parameters and locals live in **fixed cells**, as
  they do today. A call saves them to an explicit **stack array** together with
  the state to resume, loads the arguments, and sets `state` to the callee's
  entry. A return restores them and sets `state` to the saved resume state.
- Return values go through a fixed `retval` cell per function.

Only the save and restore walk the stack array; inside a segment every variable
access is as cheap as it is now. This is the "spill at calls" layout rather than
"locals on the stack", and it is what makes the scheme fast enough to use.

## 2. A hand-written example

`programs/trampoline_fib.bfl` is `fib(n) { if (n < 2) return n; a = fib(n-1); return a + fib(n-2); }`
written exactly as the compiler would have to emit it: four states, a 3-byte
frame (`n`, `a`, `ret`), a 96-cell stack. It is ordinary BFL, so it compiles
and debugs with the existing tools.

Measured (release build, this machine):

| | brainfuck commands | IR instr. | x64 bytes | fib(20), 21 891 calls |
|---|---:|---:|---:|---:|
| trampoline, JIT | 23 823 | 1 043 | 12 441 | 0.018 s |
| trampoline, interpreter | | | | 0.163 s |
| trampoline, unoptimised | | | | 1.015 s |
| iterative loop, JIT | 2 230 | 338 | 3 872 | 0.000 s |

About 0.8 µs per call on the JIT, which is roughly 1 000 brainfuck commands
per call once the optimiser has folded the copies. The breakdown per call:

- three array writes and three array reads for the frame, each a walk of about
  15·sp commands at stack index sp, plus the fixed cost of the idiom;
- one dispatch: the `else if` chain tests `state` against each segment number
  with a byte compare of about 30 commands each;
- the segment body itself, which is normal code.

So a call costs on the order of ten times a plain inlined statement. For
anything that is naturally recursive and not called millions of times, that is
acceptable. The code size is the larger price: ten times the iterative version,
because every segment carries its own save and restore sequence.

## 3. What the compiler would have to do

The scheme can be added without touching the existing code paths, as a second
mode used only for procedures that take part in recursion.

**Marking.** A procedure declared `rec proc f(...)` is compiled as a trampoline.
Plain `proc` stays inlined. The compiler can also detect cycles in the call
graph and demand the keyword, so recursion never happens by accident. All
procedures in one cycle share one dispatcher and one stack.

**Frames.** Each `rec proc` gets fixed cells for its parameters, locals and
`retval`, exactly like today's inlined procedure but allocated once. Its frame on
the stack holds those cells plus the resume state. The stack is a byte array
sized by a declaration (`rec stack 256;`) or a default; words take two slots.
Depth is not checked unless a `--check-stack` option adds a compare on every
call, which costs about 60 commands.

**Splitting the body.** This is the real work. A call to a `rec proc` can appear
anywhere: inside `if`, inside `while`, inside an expression. A segment boundary
inside a brainfuck loop is impossible, because the loop's `[` and `]` are static
code around the boundary. So the body of a `rec proc` is lowered to a control
flow graph first:

- statements without a call to a `rec proc` keep their structured code;
- an `if` or `while` that contains such a call is lowered to blocks and state
  transitions (the loop's back edge becomes "set state to the condition block");
- a call inside an expression is hoisted into a temporary before the statement.

Every block becomes a dispatcher case. Procedures with calls only at statement
level and not inside loops need no lowering beyond the split itself, which is
the common case.

**Dispatch.** An `else if` chain is O(K) compares for K segments. A cheaper form
is a countdown: copy `state` to a temporary and test, decrement, test, ... which
is about 10 commands per level instead of 30 and needs no temporaries for the
compare. With K around 20 to 50 that is 200 to 500 commands per transition.

**Calls from outside.** Code that is not itself a `rec proc` calls one by
pushing a frame with a unique DONE sentinel and running the dispatcher loop in
place: `state = ENTRY; while (state != DONE_k) { dispatch }`. The dispatcher is
emitted once per such call site, so it is better to emit it once as an inlined
procedure and call that. A nested dispatcher is safe: it shares the stack and
only exits when its own sentinel appears.

**Restrictions.** `ref` parameters to a `rec proc` can only alias globals and
arrays, because a caller's local lives in a fixed cell that deeper activations
overwrite. Arrays cannot be locals of a `rec proc` (they would have to be
copied to the stack); they stay global.

**Debugger.** The statement map is unchanged, so breakpoints and step-into work
as they do now, and the watch list shows the *current activation* because
locals are fixed cells. Step-over of a recursive call needs one addition: pause
at the return segment only when `sp` is back to its value at the call. That is
a data breakpoint on `state` with a condition on `sp`, which the debugger core
does not have yet; about 30 lines.

## 4. Effort and verdict

| Piece | Estimate |
|---|---|
| `rec proc` syntax, cycle detection | 80 lines |
| frame layout, stack array, save/restore | 150 lines |
| CFG lowering of bodies that call `rec proc`s inside if/while | 300 lines |
| dispatcher emission, call sites from outside | 150 lines |
| debugger step-over with stack depth | 80 lines |
| tests (recursive fib, factorial, Ackermann, tree walk over an array, mutual recursion) | 150 lines |

Roughly 900 lines, a week of careful work, and no change to the inlined mode
that everything else uses. The result would make textbook recursive algorithms
expressible at about ten times the cost of a loop in time and code size.

The honest counter-argument: on this machine most recursive algorithms are
better written with an explicit array as a stack, which BFL already supports, and
that version is what the compiler would generate anyway. The feature buys
notation, not capability. It is worth building if programs with real recursion
(parsers, tree algorithms, a Lisp interpreter) are on the roadmap; for device
drivers and demos it is not.
