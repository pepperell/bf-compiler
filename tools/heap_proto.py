"""Phase 0 of docs/ROADMAP.md: prototype and measure heap access layouts.

Generates brainfuck programs that read and write bytes of a heap at 16-bit
addresses held in cells, for several coarse hop sizes, runs them through
out\\bf.exe and reports code size per access and run time.

Heap layout (K = 5 cells per byte):  C H L V D
  C  counter while walking, then a trail marker (1) once passed
  H  high byte of the address, carried along during the coarse stage
  L  low byte of the address, carried along during the coarse stage
  V  value carried forward (write) or back (read)
  D  the data byte
A guard of HOP elements with C = 0 precedes element 0.

Access to address a = 256*hi + lo:
  coarse stage: hi times, make 256/HOP hops of HOP elements (C counts hops,
                leaves a marker at each hop start)
  fine stage:   lo single steps (C counts, leaves markers)
  then read or write D, and return: single steps back over the fine trail,
  then HOP steps back over the coarse trail, ending in the guard.

    python tools\\heap_proto.py            measure all hop sizes
"""
import os
import random
import subprocess
import sys
import time

BF = os.path.join(os.path.dirname(__file__), "..", "out", "bf.exe")
K = 5
C, H, L, V, D = range(5)


class Gen:
    def __init__(self, hop, heap_bytes):
        self.hop = hop
        self.out = []
        self.cur = 0                         # absolute cell (relative to the start cell)
        # static cells: 0..7 temporaries; heap guard after them
        self.tmp = 0
        self.base = 8                        # first cell of the guard
        self.elem0 = self.base + hop * K     # element 0
        self.heap_bytes = heap_bytes

    def cell(self, a, f):                    # field f of element a
        return self.elem0 + a * K + f

    def emit(self, s):
        self.out.append(s)

    def go(self, c):
        d = c - self.cur
        self.emit(">" * d if d > 0 else "<" * -d)
        self.cur = c

    def clear(self, c):
        self.go(c); self.emit("[-]")

    def set(self, c, v):
        self.clear(c); self.emit("+" * v if v <= 128 else "-" * (256 - v))

    def move(self, src, dsts):
        self.go(src); self.emit("[-")
        for d in dsts:
            self.go(d); self.emit("+")
        self.go(src); self.emit("]")

    # carry field f of the current element `steps` elements forward (relative code)
    @staticmethod
    def carry(f, steps, at):
        """relative code: from field `at`, move value of field f forward `steps` elements, return to `at`."""
        dist = steps * K
        to_f = f - at
        s = (">" * to_f if to_f > 0 else "<" * -to_f)
        s += "[-" + ">" * dist + "+" + "<" * dist + "]"
        s += ("<" * to_f if to_f > 0 else ">" * -to_f)
        return s

    @staticmethod
    def rel(n):
        return ">" * n if n > 0 else "<" * -n

    def access(self, hi_cell, lo_cell, val_cell, write):
        """hi/lo/val are static temporaries; hi and lo are consumed.  Read: val += heap[a].
        Write: heap[a] = val (val consumed)."""
        hop = self.hop
        e0 = self.elem0
        self.move(hi_cell, [e0 + H])
        self.move(lo_cell, [e0 + L])
        if write:
            self.move(val_cell, [e0 + V])
        # coarse stage, at element 0 field H
        self.go(e0 + H)
        n_inner = 256 // hop                 # hops per unit of hi
        body = "-"                            # hi -= 1
        # inner counter in C: n_inner hops (n_inner may be 256 -> use 255 + 1)
        cnt = n_inner if n_inner < 256 else 255
        body += self.rel(C - H) + "+" * cnt   # at C
        hop_step = ""
        hop_step += "-"                                            # counter - 1 (at C)
        hop_step += "[-" + ">" * (hop * K) + "+" + "<" * (hop * K) + "]"   # rest of counter forward
        hop_step += self.carry(H, hop, C) + self.carry(L, hop, C)
        if write:
            hop_step += self.carry(V, hop, C)
        hop_step += "+" + ">" * (hop * K)                          # marker, then to next C
        body += "[" + hop_step + "]"
        if n_inner == 256:                    # one extra hop
            body += self.carry(H, hop, C) + self.carry(L, hop, C)
            if write:
                body += self.carry(V, hop, C)
            body += "+" + ">" * (hop * K)
        body += self.rel(H - C)               # back to H of the current element
        self.emit("[" + body + "]")
        # fine stage: at H (0).  move L into C, loop single steps
        fine = self.rel(L - H) + "[-" + self.rel(C - L) + "+" + self.rel(L - C) + "]" + self.rel(C - L)
        step = "-[-" + ">" * K + "+" + "<" * K + "]"
        if write:
            step += self.carry(V, 1, C)
        step += "+" + ">" * K
        fine += "[" + step + "]"
        self.emit(fine)                       # now at C of the destination (0)
        if write:
            back = self.rel(D - C) + "[-]" + self.rel(V - D) + "[-" + self.rel(D - V) + "+" + self.rel(V - D) + "]" + self.rel(C - V)
            back += "<" * K + "[-" + "<" * K + "]" + ">" * K          # fine trail back; land on the first unmarked + 1
            back += "<" * (hop * K) + "[-" + "<" * (hop * K) + "]"   # coarse trail back; ends in the guard
        else:
            # copy D into V using H as a temporary
            back = self.rel(D - C) + "[-" + self.rel(V - D) + "+" + self.rel(H - V) + "+" + self.rel(D - H) + "]"
            back += self.rel(H - D) + "[-" + self.rel(D - H) + "+" + self.rel(H - D) + "]" + self.rel(C - H)
            # carry V back over the fine trail
            back += self.carry_back(1, True)
            # now at the C of the first fine-trail element (or the destination when lo = 0)
            back += self.carry_back(hop, False)
        self.emit(back)
        # we are at C of a guard element: which one depends on nothing (always element -hop)
        self.cur = e0 - hop * K + C
        if not write:
            src = e0 - hop * K + V
            self.move(src, [val_cell])

    def carry_back(self, stride, fine):
        """At C of element e (trail end).  Repeatedly: move V to the element `stride` back,
        step there; continue while that element is a marker (clear it).  For the fine trail
        stop on the first unmarked element and return one step forward."""
        d = stride * K
        mv = self.rel(V - C) + "[-" + "<" * d + "+" + ">" * d + "]" + self.rel(C - V)
        s = mv + "<" * d + "[-" + mv + "<" * d + "]"
        if fine:
            # we stopped on an unmarked element (the one before the fine trail): V went there;
            # bring V one step forward again and stand on that element
            s += self.rel(V - C) + "[-" + ">" * d + "+" + "<" * d + "]" + self.rel(C - V) + ">" * d
        return s

    def text(self):
        return "".join(self.out)


def build(hop, heap_bytes, ops, seed):
    """ops: list of ("w", addr, val) / ("r", addr).  Prints the checksum of reads as two bytes."""
    g = Gen(hop, heap_bytes)
    HI, LO, VAL, ACC0, ACC1 = 0, 1, 2, 3, 4
    sizes = []
    for op in ops:
        start = len(g.text())
        a = op[1]
        g.set(HI, a >> 8); g.set(LO, a & 255)
        if op[0] == "w":
            g.set(VAL, op[2])
            g.access(HI, LO, VAL, True)
        else:
            g.access(HI, LO, VAL, False)
            g.move(VAL, [ACC0])              # checksum mod 256
        sizes.append(len(g.text()) - start)
    g.go(ACC0); g.emit(".")
    return g.text(), sizes


def run(src, flags=()):
    path = os.path.join(os.environ.get("TEMP", "."), "heap_proto.b")
    open(path, "w").write(src)
    r = subprocess.run([BF, path, "--quiet-devices", "--time"] + list(flags), capture_output=True, timeout=600)
    t = r.stderr.decode().strip().split("]")[1].split(" s")[0] if b"]" in r.stderr else "?"
    return r.stdout, float(t.strip()) if t != "?" else None, r.stderr.decode().strip()


def main():
    rnd = random.Random(1)
    heap = 8192
    ops = []
    mem = {}
    for _ in range(300):
        a = rnd.randrange(heap)
        v = rnd.randrange(1, 256)
        ops.append(("w", a, v)); mem[a] = v
    keys = list(mem)
    checksum = 0
    for _ in range(300):
        a = rnd.choice(keys)
        ops.append(("r", a)); checksum = (checksum + mem[a]) & 255
    print(f"workload: 300 writes then 300 reads at random addresses in an {heap}-byte heap; expected checksum {checksum}")
    print(f"{'hop':>5} {'bytes/access':>13} {'IR instr':>9} {'JIT s':>8} {'interp s':>9} {'us/access':>10}  ok")
    for hop in (1, 4, 16, 64, 256):
        src, sizes = build(hop, heap, ops, 1)
        out, tj, err = run(src)
        _, ti, _ = run(src, ["--interp"])
        ir = subprocess.run([BF, os.path.join(os.environ.get("TEMP", "."), "heap_proto.b"), "--dump"], capture_output=True).stderr.decode().split("-> ")[-1].split()[0]
        ok = out == bytes([checksum])
        print(f"{hop:>5} {sum(sizes) // len(sizes):>13} {ir:>9} {tj:>8.3f} {ti:>9.3f} {tj / len(ops) * 1e6:>10.1f}  {ok} {'' if ok else err}")


if __name__ == "__main__":
    main()
