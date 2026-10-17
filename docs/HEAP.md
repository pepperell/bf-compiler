# Heap layout: measurements and decision

Phase 0 of docs/ROADMAP.md. The prototype is `tools/heap_proto.py`; it generates
brainfuck that writes and reads bytes at 16-bit addresses held in cells, runs it
through `out\bf.exe`, and checks the result.

## The access routine

Brainfuck has no indirect addressing, so reading `heap[a]` walks the tape.
Each heap byte occupies K = 5 cells:

```
C  counter while walking; a trail marker (1) once the walk has passed
H  high byte of the address, carried during the coarse stage
L  low byte of the address, carried during the coarse stage
V  the value: carried forward by a write, back by a read
D  the data byte
```

A guard of HOP elements with C = 0 sits before element 0. For address
a = 256·hi + lo:

1. **Coarse stage.** hi times, make 256/HOP hops of HOP elements. C counts the
   hops and becomes a marker in each element a hop starts from.
2. **Fine stage.** lo single steps, again leaving markers.
3. Read D into V, or write V into D.
4. **Return.** Step back over the fine trail one element at a time, then over
   the coarse trail HOP elements at a time, clearing markers and (for a read)
   carrying V. The walk always ends in the same guard element, so the compiler
   knows where the pointer is afterwards.

The routine never loops on the address itself, so it needs no 16-bit counter.

## Measurements

3 000 accesses (1 500 writes, then 1 500 reads) at random addresses, release
build on this machine. "Code" is brainfuck per access site, including about 250
commands that set up the address and value.

| Hop | Code | JIT, 8 KB heap | Interpreter, 8 KB | JIT, 32 KB heap | Interpreter, 32 KB |
|---:|---:|---:|---:|---:|---:|
| 1 | 765 | 15.7 µs | 201 µs | 61.7 µs | 804 µs |
| 4 | 794 | 4.7 µs | 51 µs | 16.3 µs | 201 µs |
| 8 | 1 122 | 2.7 µs | 30 µs | 8.3 µs | 103 µs |
| 16 | 1 826 | 1.7 µs | 16 µs | 4.7 µs | 55 µs |
| 32 | 3 258 | 1.0 µs | 10 µs | 2.7 µs | 33 µs |

The IR instruction count is the same for every hop size, because the optimiser
folds each run of `>` into a single move; only the brainfuck text grows.

## Decision

**Hop 16, K = 5.** It is nine times faster than single steps for about 2.4
times the code, and past 16 each halving of the time doubles the code. With the
default 8 KB heap a random access costs under 2 µs on the JIT, and the worst
case (top of a 64 KB heap) about 9 µs.

Two consequences shaped the compiler:

- **Constant addresses need no walk.** An element's cell position is fixed, so a
  heap access whose address is known at compile time (a `heap` array with a
  constant index, a string literal, a field of a static struct) compiles to a
  plain pointer move. Only runtime addresses walk.
- **Code size per runtime access is about 1.6 KB.** A program with hundreds of
  pointer accesses compiles to megabytes of brainfuck. The JIT copes, because
  the IR is small, but the generated-code window gets large.
