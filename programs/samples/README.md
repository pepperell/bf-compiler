# BFL samples

Small programs that use the newer parts of BFL: integer types, structs,
pointers and the heap, strings, imports, and recursion. Run one with
`bf programs\samples\NAME.bfl`, or open it in `bfui` and press F5 (Ctrl+F5 to
debug it statement by statement). `tests\test_lang.py` runs every sample and
compares its output with `tests\samples\NAME.out`; `cube.bfl` runs until
stopped, so it is only compiled there.

| File | What it does | Features it shows |
|------|--------------|-------------------|
| `linked_list.bfl` | Builds a sorted list, reverses it, removes the negative values | `struct`, `Node*`, `->`, `new`, `free`, `null`, freed blocks reused |
| `bst.bfl` | A binary search tree of random keys: sorted walk, height, lookups | `rec proc` over pointers, structs in the heap, 32-bit random generator |
| `calc.bfl` | An interactive calculator: `12 * (3 + 4) - -5` | Mutually recursive `rec proc`s, `long` arithmetic, keyboard input |
| `queens.bfl` | The 6 queens puzzle, drawing the first solution | A `rec proc` with an array parameter, inline procedures sharing it |
| `mergesort.bfl` | Merge sort of 16 signed numbers | A local array in a `rec proc` (a fresh heap copy per call) |
| `text.bfl` | Sorts words, builds a sentence, converts numbers to text and back | `import "strings.bfl"`, string pointers, `strdup` and `free` |
| `numbers.bfl` | Factorials, Fibonacci, wrap-around, signed division, Collatz | `sbyte` `int` `dword` `long`, casts, shifts, `import "math.bfl"` |
| `maze.bfl` + `queue.bfl` | Shortest path through a maze by breadth-first search | A program in two files, a struct passed by `ref`, heap arrays |
| `cube.bfl` | A revolving 3D wireframe cube, every frame computed live | Integer trigonometry (Bhaskara's sine), perspective, Bresenham lines, a heap frame buffer, colour escapes |
| `life.bfl` | Conway's Game of Life, animated in the terminal | Swapping heap grids through pointers, ANSI escapes, the `TICKS` clock |

Things to try in the UI:

- **Multi-file debugging.** Open `maze.bfl`, pick `queue.bfl` in the toolbar's
  File box, put the caret on a line of `pop` and press F9, then Ctrl+F5 and F5.
  The debugger stops inside `queue.bfl`, and the watch list shows `q.data` with
  the heap value it points at. Debug sessions use the interpreter, so the
  maze setup takes several seconds before the breakpoint is reached.
- **Watching wide values.** Debug `numbers.bfl` and step with F11: the watch list
  shows `dword` and `long` variables whole, negative values as negative.
- **Recursion.** Debug `bst.bfl`, set a breakpoint in `height`, and use F10 to
  step over a recursive call: it runs until the stack is back at the same depth.
- **Interactive input.** `calc.bfl` reads the keyboard; click in the terminal
  before typing.
