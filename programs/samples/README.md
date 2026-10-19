# BFL samples

Small programs that use the newer parts of BFL: integer types, structs,
pointers and the heap, strings, imports, and recursion. Run one with
`bf programs\samples\NAME.bfl`, or open it in `bfui` and press F5 (Ctrl+F5 to
debug it statement by statement). `tests\test_lang.py` runs every sample and
compares its output with `tests\samples\NAME.out`; `cube.bfl`,
`starfield.bfl`, `raytracer.bfl`, `raytracer_text.bfl`, `flag.bfl`,
`galaxies.bfl` and `traffic.bfl` run until stopped, so they are only
compiled there.

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
| `cube.bfl` | A revolving 3D wireframe cube, every frame computed live | Integer trigonometry (Bhaskara's sine), rotation with `mulshr` in 16-bit ints, perspective, Bresenham lines, a heap frame buffer, colour escapes |
| `starfield.bfl` | The Enterprise-D's main viewscreen: stars at impulse, then streaking at warp, with LCARS panels, Ops and Conn, and the captain giving the order | A 3D starfield with perspective, double-buffered output that sends only changed characters, 256-colour backgrounds, a pixel-map picture |
| `raytracer.bfl` | Four reflective spheres on a checkerboard, two coloured lamps with soft shadows, the camera circling the scene on the framebuffer (about 31 ms a frame; [one orbit](../../docs/raytracer.gif), made by `tools/rt_gif.py`) | Fixed-point ray tracing with `mulshr` and incremental primary rays, screen-box culling, a precomputed floor light map in the heap, one reflection bounce, Blinn highlights, ordered dithering to RRRGGGBB, a `shared proc` |
| `raytracer_text.bfl` | The same scene drawn as coloured text in any terminal (`bf` in Windows Terminal, xterm, the UI): 79 x 22 cells, about 8 ms a frame | Brightness to a character ramp, colour to the nearest of 256 ANSI colours, a camera with tall cells |
| `flag.bfl` | A chequered flag flying from a pole in gusty wind, the camera drifting round it on the framebuffer: 21 x 11 particles of simulated cloth (about 28 ms a frame, paced to 30 frames a second) | Verlet integration, springs relaxed with Jakobsen's square-root-free correction, normals from neighbouring particles for the wind and the light, light through the cloth, depth buckets, a triangle filler with Gouraud shading and gap-free edges, a cast shadow, a `shared proc` with three callers, double buffering with `FB_BUFFER` so a frame is never shown half drawn |
| `galaxies.bfl` | Two disc galaxies swing past each other, throw out tidal tails and a bridge, and merge; four encounters with the discs tilted different ways, 500 stars on the framebuffer (about 20 ms a frame) | Toomre and Toomre's restricted three-body method, Q20 positions and Q24 velocities, softened gravity with an integer square root, discs started in exact circular orbits, dynamical friction, additive blending with `FB_READ`, a zoom that follows the stars, structs of structs (both galaxies in one array) |
| `traffic.bfl` | A traffic jam out of nothing: 22 cars following the Intelligent Driver Model round a 230 m ring (Sugiyama's experiment) break into stop-and-go waves, shown on the ring and in a time-space diagram; keys add and remove cars and make a driver brake | Car following in fixed point, a ring of cars kept in order as they pass the start line, keyboard input during an animation, drawing by erasing and redrawing only what moved, a sweeping time-space plot |
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
- **Jams.** Run `traffic.bfl` and wait for the first red band in the
  time-space diagram (about two minutes of simulated time, fifteen seconds
  on the screen), then click in the terminal and press `-` until 12 cars are
  left: the waves die away and the flow settles at over 30 km/h. `+` brings
  them back.
