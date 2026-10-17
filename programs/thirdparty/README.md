# Third-party brainfuck programs

These programs were written by other people and are kept here, unmodified, as
examples and as real-world tests for the executor. The test suite
(`tests/test.py`) runs every terminating one through the JIT, the interpreter
and the unoptimised path and compares the output with a reference interpreter.

## Top level

| File | Author | Notes |
|------|--------|-------|
| `mandelbrot.b` | Erik Bosman | ASCII Mandelbrot set; the main speed benchmark |
| `hanoi.b` | Clifford Wolf, <http://www.clifford.at/bfcpu/> (credited in its output) | Towers of Hanoi animated with VT100 cursor control; needs a terminal about 120 columns wide for the third peg |

## `cristofani/`

All by Daniel B. Cristofani, from <https://brainfuck.org/> (downloaded 2026-10-17).
Licensed under the [Creative Commons Attribution-ShareAlike 4.0 International
License](https://creativecommons.org/licenses/by-sa/4.0/); the author states on
that page that the licence covers all of the programs, including files that do
not carry the notice. The `short.b` collection of fragments from the same page is
not included because it is not a single runnable program.

Run them with `out\bf.exe programs\thirdparty\cristofani\NAME.b` or open them in
the UI. Most read with `,`; at a console end the input with Ctrl+Z, or pipe a
file in. Programs marked "endless" stream output until stopped (Ctrl+C, or
Shift+F5 in the UI).

| Program | What it does | Try it with |
|---------|--------------|-------------|
| `bounce.b` | particle automaton: `^ v` particles, `/ \ X` walls | a small grid, then Enter to step (interactive) |
| `bsort.b` | bubble sort of the input bytes | `echo hello world \| bf bsort.b` |
| `chessboard.b` | ASCII-art chessboard from a FEN position | `rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1` |
| `collatz.b` | Collatz step counts for decimal numbers, one per line | `27` gives 111 |
| `dbf2c.b` | translates brainfuck (on stdin) into C | any program |
| `dbfi.b` | brainfuck self-interpreter: program, `!`, then its input | `++++++++[>++++++++<-]>+.!` |
| `dquine.b`, `392quine.b`, `400quine.b` | quines (the last two after Erik Bosman's 410-byte quine) | no input; the output reproduces itself |
| `dvorak.b` | types Dvorak on a QWERTY keyboard | any text |
| `e.b` | digits of e (endless) | no input |
| `factorial2.b` | factorials (endless) | no input |
| `fib.b` | Fibonacci numbers (endless) | no input |
| `ghost.b` | a small chase game: `w a s d` and Enter | interactive |
| `golden.b` | digits of the golden ratio (endless) | no input |
| `head.b` | first ten lines of the input | any text |
| `impeccable.b` | OEIS A014221: 0 1 2 4 16 65536, then 2^65536 (very slow) | no input |
| `isort.b` | insertion sort of the input bytes | any text |
| `jabh.b` | "Just another brainfuck hacker," | no input |
| `life.b` | Conway's Life; `be` toggles a cell, Enter steps, `q` quits | interactive |
| `numwarp.b` | draws a number in slashes | `3.14159` |
| `qsort.b` | quicksort of the input bytes | any text |
| `random.b` | random bytes from the Rule 30 automaton (endless, binary) | no input |
| `rot13.b` | ROT13 | any text |
| `sierpinski.b` | Sierpinski triangle | no input |
| `squares.b` | squares from 0 to 10000 | no input |
| `squares2.b` | squares (endless) | no input |
| `thuemorse.b` | Thue-Morse sequence (endless) | no input |
| `tictactoe.b` | plays noughts and crosses; enter a square number 1-9 | interactive |
| `utm.b` | universal Turing machine after Rogozhin | `b1b1bbb1c1c11111d` gives `1c11111` |
| `wc.b` | line, word and character counts | any text |
| `xmastree.b` | ASCII Christmas tree of the given height | `5` |
