"""Differential fuzzer for the BFL compiler, the IR optimiser and the JIT
(docs/ROADMAP.md, phase 9).

Two generators:

  bfl  random BFL programs over every integer type: arithmetic, shifts, mulshr,
       compares, casts, constants, arrays in cells and in the heap, pointers,
       loops with break and continue, inlined and recursive procedures.  Each
       program is also run by a model of BFL written here in Python, the
       oracle.  It is compiled twice (the brainfuck must be identical) and once
       without the automatic layout of globals, and run by every executor.  Its
       brainfuck is also run with one cell poked before the start, compared with
       the unoptimised run, so that scratch cells hold unexpected values and the
       loop idioms' guards must send loops through their bodies.
  bf   random brainfuck aimed at the loop shapes the optimiser rewrites,
       checked against a plain interpreter in Python.

The executors: the JIT and the IR interpreter, each with optimisation, without
it, and without the loop idioms or the block rebuilding (BF_NO_IDIOMS,
BF_NO_REBUILD).

A failing program is shrunk (statements, expressions and brainfuck removed while
it still fails the same way) and written out next to the original.

    python tests/fuzz.py [--bfl N] [--bf N] [--seed S] [--jobs J] [--out DIR] [--no-shrink] [bf.exe]
    python tests/fuzz.py --replay bfl|bf SEED       one program: its text and every executor's output

tests/test.py and tests/test_lang.py run a few seeds of each.
"""
import argparse
import concurrent.futures
import os
import random
import re
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BF = os.path.join(HERE, "..", "out", "bf.exe")

# ---- BFL semantics ---------------------------------------------------------------------------------
TYPES = {"byte": (8, False), "sbyte": (8, True), "word": (16, False), "int": (16, True), "dword": (32, False), "long": (32, True)}
TNAMES = list(TYPES)
CMP = {"==", "!=", "<", "<=", ">", ">="}


def wrap(v, t):
    bits, signed = TYPES[t]
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


def fits(v, t):
    bits, signed = TYPES[t]
    return -(1 << (bits - 1)) <= v < (1 << (bits - 1)) if signed else 0 <= v < (1 << bits)


def fits_either(v, t):
    bits = TYPES[t][0]
    return -(1 << (bits - 1)) <= v <= (1 << bits) - 1


def assign_fits(v, t):
    """A constant stored in a variable of type t must fit it (an unsigned type also takes negatives)."""
    return fits(v, t) if TYPES[t][1] else fits_either(v, t)


def const_type(v):
    """The type of a constant: the smallest that holds its value (sema.cpp, natural)."""
    if v >= 0:
        return "byte" if v <= 255 else "word" if v <= 65535 else "dword"
    return "sbyte" if v >= -128 else "int" if v >= -32768 else "long"


def common(a, b):
    """The type two operands are converted to (sema.cpp, common).  A constant takes the other
    operand's type when its value fits it, signed or unsigned."""
    if a.const and not b.const and fits_either(a.v, b.t):
        return b.t
    if b.const and not a.const and fits_either(b.v, a.t):
        return a.t
    (sa, ga), (sb, gb) = TYPES[a.t], TYPES[b.t]
    if sa != sb:
        return a.t if sa > sb else b.t
    if ga and gb:
        return a.t
    return b.t if ga else a.t


def truncdiv(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def w64(v):
    v &= (1 << 64) - 1
    return v - (1 << 64) if v >> 63 else v


def fold(op, a, b):
    """Folding of two constants: exact values in an int64, whatever their types."""
    if op == "+": r = a + b
    elif op == "-": r = a - b
    elif op == "*": r = a * b
    elif op == "/": r = truncdiv(a, b) if b else 0
    elif op == "%": r = a - truncdiv(a, b) * b if b else a
    elif op == "<<": r = 0 if not 0 <= b < 32 else a << b
    elif op == ">>": r = (-1 if a < 0 else 0) if not 0 <= b < 32 else a >> b
    elif op == "&": r = a & b
    elif op == "|": r = a | b
    elif op == "^": r = a ^ b
    elif op in CMP: r = compare(op, a, b)
    elif op == "&&": r = int(bool(a) and bool(b))
    elif op == "||": r = int(bool(a) or bool(b))
    else: raise ValueError(op)
    return w64(r)


def compare(op, a, b):
    return int({"==": a == b, "!=": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op])


def arith(op, a, b, t):
    """a op b at type t, both already of that type; division truncates, x / 0 is 0 and x % 0 is x."""
    if op == "/":
        return 0 if b == 0 else wrap(truncdiv(a, b), t)
    if op == "%":
        return a if b == 0 else wrap(a - truncdiv(a, b) * b, t)
    r = {"+": a + b, "-": a - b, "*": a * b, "&": a & b, "|": a | b, "^": a ^ b}[op]
    return wrap(r, t)


def shift(op, a, k, t):
    """a (of type t) shifted by k: by the width or more, or by a negative count, gives 0
    (-1 for a negative value shifted right)."""
    bits, signed = TYPES[t]
    if 0 <= k < bits:
        return wrap(a << k, t) if op == "<<" else wrap(a >> k, t)
    return -1 if op == ">>" and signed and a < 0 else 0


class V:
    """A value: its type, whether it is a compile-time constant (exact, not wrapped), the value."""
    __slots__ = ("t", "const", "v")

    def __init__(self, t, const, v):
        self.t, self.const, self.v = t, const, v


class ModelError(Exception):
    """The model cannot run the program (a shrunk program may be invalid)."""


class NotConst(Exception):
    pass


class Brk(Exception):
    pass


class Cont(Exception):
    pass


class Ret(Exception):
    def __init__(self, v):
        self.v = v


class Cell:
    __slots__ = ("t", "v")

    def __init__(self, t, v):
        self.t, self.v = t, v


class Arr:
    __slots__ = ("t", "a")

    def __init__(self, t, n):
        self.t, self.a = t, [0] * n


class Ptr:
    __slots__ = ("arr", "j")

    def __init__(self, arr, j):
        self.arr, self.j = arr, j


class Model:
    def __init__(self, prog, stdin):
        self.out = bytearray()
        self.inp = list(stdin)
        self.globals = {}
        self.frame = self.globals
        self.consts = {}
        self.procs = {p.name: p for p in prog.procs}
        self.steps = 0
        self.depth = 0

    def lookup(self, name):
        if name in self.frame:
            return self.frame[name]
        if name in self.globals:
            return self.globals[name]
        raise ModelError("undeclared " + name)

    def tick(self):
        self.steps += 1
        if self.steps > 400_000:
            raise ModelError("too long")

    def read_in(self):
        if not self.inp:
            raise ModelError("input exhausted")
        return self.inp.pop(0)

    def call(self, p, args):
        if len(args) != len(p.params):
            raise ModelError("argument count")
        frame = {}
        for (t, ref, pn), a in zip(p.params, args):
            if ref:
                if not isinstance(a, Var):
                    raise ModelError("ref needs a variable")
                c = self.lookup(a.name)
                if not isinstance(c, Cell) or c.t != t:
                    raise ModelError("ref type")
                frame[pn] = c
            else:
                frame[pn] = Cell(t, wrap(a.ev(self).v, t))
        self.depth += 1
        if self.depth > 100:
            raise ModelError("too deep")
        saved, self.frame = self.frame, frame
        r = None
        try:
            run_list(p.body, self)
        except Ret as e:
            r = e.v
        except (Brk, Cont):
            raise ModelError("break outside a loop")
        finally:
            self.frame = saved
            self.depth -= 1
        if p.ret is None:
            if r is not None:
                raise ModelError("value from a void proc")
            return None
        if r is None:
            raise ModelError("no return value")
        return V(p.ret, False, wrap(r.v, p.ret))


class ConstModel(Model):
    """Evaluates constant expressions only."""

    def __init__(self, consts):
        self.consts = consts

    def lookup(self, name):
        raise NotConst()

    def read_in(self):
        raise NotConst()

    def call(self, p, args):
        raise NotConst()

    @property
    def procs(self):
        raise NotConst()


def run_list(lst, m):
    for s in lst:
        m.tick()
        s.ex(m)


def truth(v):
    return v.v != 0


# ---- the program tree -----------------------------------------------------------------------------
# Every node lists its expression fields (EF) and statement-list fields (SF) for the shrinker.
class Node:
    EF = ()
    SF = ()


class Expr(Node):
    pass


class Lit(Expr):
    def __init__(self, v, style=0):
        self.v, self.style = v, style

    def emit(self):
        if self.style == 1:
            return hex(self.v)
        if self.style == 2 and 32 <= self.v < 127 and chr(self.v) not in "'\\":
            return "'" + chr(self.v) + "'"
        return str(self.v)

    def ev(self, m):
        return V(const_type(self.v), True, self.v)


class CRef(Expr):
    def __init__(self, name):
        self.name = name

    def emit(self):
        return self.name

    def ev(self, m):
        if self.name not in m.consts:
            raise ModelError("undeclared constant")
        return m.consts[self.name]


class Var(Expr):
    def __init__(self, name):
        self.name = name

    def emit(self):
        return self.name

    def ev(self, m):
        c = m.lookup(self.name)
        if not isinstance(c, Cell):
            raise ModelError("not a scalar")
        return V(c.t, False, c.v)


def element(m, arr, idx_expr, base=0):
    """The array and the element number reached by arr[idx] (or through a pointer)."""
    a = m.lookup(arr)
    if isinstance(a, Ptr):
        base = a.j
        a = m.lookup(a.arr)
    if not isinstance(a, Arr):
        raise ModelError("not an array")
    i = base + (idx_expr.ev(m).v if idx_expr is not None else 0)
    if not 0 <= i < len(a.a):
        raise ModelError("index out of range")
    return a, i


class Idx(Expr):
    """arr[i], or through a pointer p[i]; i None for *p."""
    EF = ("i",)

    def __init__(self, arr, i):
        self.arr, self.i = arr, i

    def emit(self):
        return f"(*{self.arr})" if self.i is None else f"{self.arr}[{self.i.emit()}]"

    def ev(self, m):
        a, i = element(m, self.arr, self.i)
        return V(a.t, False, a.a[i])


class Cast(Expr):
    EF = ("e",)

    def __init__(self, t, e):
        self.t, self.e = t, e

    def emit(self):
        return f"{self.t}({self.e.emit()})"

    def ev(self, m):
        a = self.e.ev(m)
        return V(self.t, a.const, wrap(a.v, self.t))


class Un(Expr):
    EF = ("e",)

    def __init__(self, op, e):
        self.op, self.e = op, e

    def emit(self):
        return f"({self.op}{self.e.emit()})"

    def ev(self, m):
        a = self.e.ev(m)
        if self.op == "!":
            return V("byte", a.const, int(a.v == 0))
        if a.const:
            if self.op == "-":
                return V(const_type(-a.v), True, -a.v)
            return V(a.t, True, wrap(~a.v, a.t))
        return V(a.t, False, wrap(-a.v if self.op == "-" else ~a.v, a.t))


class Bin(Expr):
    EF = ("a", "b")

    def __init__(self, op, a, b):
        self.op, self.a, self.b = op, a, b

    def emit(self):
        return f"({self.a.emit()} {self.op} {self.b.emit()})"

    def ev(self, m):
        op = self.op
        A = self.a.ev(m)
        B = self.b.ev(m)
        if A.const and B.const:
            r = fold(op, A.v, B.v)
            if op in CMP or op in ("&&", "||"):
                return V("byte", True, r)
            if r > 0xFFFFFFFF or r < -0x80000000:
                r = wrap(r, "dword")
            return V(const_type(r), True, r)
        if op in ("&&", "||"):
            r = (truth(A) and truth(B)) if op == "&&" else (truth(A) or truth(B))
            return V("byte", False, int(r))
        if op in ("<<", ">>"):
            return V(A.t, False, shift(op, wrap(A.v, A.t), B.v, A.t))
        c = common(A, B)
        a, b = wrap(A.v, c), wrap(B.v, c)
        if op in CMP:
            return V("byte", False, compare(op, a, b))
        return V(c, False, arith(op, a, b, c))


class MulShr(Expr):
    """mulshr(a, b, k): a and b at their common type, the exact product shifted right by the constant k
    (rounding down), wrapped to that type; two constants fold exactly."""
    EF = ("a", "b")

    def __init__(self, a, b, k):
        self.a, self.b, self.k = a, b, k

    def emit(self):
        return f"mulshr({self.a.emit()}, {self.b.emit()}, {self.k})"

    def ev(self, m):
        A = self.a.ev(m)
        B = self.b.ev(m)
        if A.const and B.const:
            r = (A.v * B.v) >> self.k
            if r > 0xFFFFFFFF or r < -0x80000000:
                r = wrap(r, "dword")
            return V(const_type(r), True, r)
        c = common(A, B)
        return V(c, False, wrap((wrap(A.v, c) * wrap(B.v, c)) >> self.k, c))


class Call(Expr):
    EF = ("args",)

    def __init__(self, name, args):
        self.name, self.args = name, args

    def emit(self):
        return f"{self.name}({', '.join(a.emit() for a in self.args)})"

    def ev(self, m):
        p = m.procs.get(self.name)
        if p is None or p.ret is None:
            raise ModelError("no such function")
        return m.call(p, self.args)


class In(Expr):
    def emit(self):
        return "in"

    def ev(self, m):
        return V("byte", False, m.read_in())


def static_const(e, consts):
    try:
        return e.ev(ConstModel(consts))
    except NotConst:
        return None
    except ModelError:
        return None


# lvalues
class VarL(Node):
    def __init__(self, name):
        self.name = name

    def emit(self):
        return self.name

    def ref(self, m):
        c = m.lookup(self.name)
        if not isinstance(c, Cell):
            raise ModelError("not a scalar")
        return c.t, (lambda: c.v), (lambda v: setattr(c, "v", v))


class IdxL(Node):
    EF = ("i",)

    def __init__(self, arr, i):
        self.arr, self.i = arr, i

    def emit(self):
        return f"*{self.arr}" if self.i is None else f"{self.arr}[{self.i.emit()}]"

    def ref(self, m):
        a, i = element(m, self.arr, self.i)
        return a.t, (lambda: a.a[i]), (lambda v: a.a.__setitem__(i, v))


class Stmt(Node):
    def lines(self, ind):
        return [ind + self.emit()]


class Decl(Stmt):
    """T name = init;  (or T name; which is zero)"""
    EF = ("init",)

    def __init__(self, t, name, init):
        self.t, self.name, self.init = t, name, init

    def emit(self):
        return f"{self.t} {self.name}" + (f" = {self.init.emit()};" if self.init is not None else ";")

    def ex(self, m):
        v = wrap(self.init.ev(m).v, self.t) if self.init is not None else 0
        m.frame[self.name] = Cell(self.t, v)


class ArrDecl(Stmt):
    def __init__(self, t, name, n, heap):
        self.t, self.name, self.n, self.heap = t, name, n, heap

    def emit(self):
        return ("heap " if self.heap else "") + f"{self.t} {self.name}[{self.n}];"

    def ex(self, m):
        m.frame[self.name] = Arr(self.t, self.n)


class PtrDecl(Stmt):
    def __init__(self, t, name, arr, j):
        self.t, self.name, self.arr, self.j = t, name, arr, j

    def emit(self):
        return f"{self.t}* {self.name} = &{self.arr}[{self.j}];"

    def ex(self, m):
        a = m.lookup(self.arr)
        if not isinstance(a, Arr) or not 0 <= self.j < len(a.a):
            raise ModelError("bad pointer")
        m.frame[self.name] = Ptr(self.arr, self.j)


class ConstDecl(Stmt):
    EF = ("e",)

    def __init__(self, name, e):
        self.name, self.e = name, e

    def emit(self):
        return f"const {self.name} = {self.e.emit()};"

    def ex(self, m):
        v = static_const(self.e, m.consts)
        if v is None:
            raise ModelError("not constant")
        m.consts[self.name] = v


class Assign(Stmt):
    EF = ("lv", "e")

    def __init__(self, lv, op, e):
        self.lv, self.op, self.e = lv, op, e

    def emit(self):
        return f"{self.lv.emit()} {self.op} {self.e.emit()};"

    def ex(self, m):
        t, get, put = self.lv.ref(m)
        E = self.e.ev(m)
        if self.op == "=":
            put(wrap(E.v, t))
            return
        op = self.op[:-1]
        if op in ("<<", ">>"):
            put(shift(op, get(), E.v, t))      # the count is the right operand's own value
        else:
            put(arith(op, get(), wrap(E.v, t), t))   # x op= e computes at x's type


class IncDec(Stmt):
    EF = ("lv",)

    def __init__(self, lv, op):
        self.lv, self.op = lv, op

    def emit(self):
        return f"{self.lv.emit()}{self.op};"

    def ex(self, m):
        t, get, put = self.lv.ref(m)
        put(wrap(get() + (1 if self.op == "++" else -1), t))


class If(Stmt):
    EF = ("c",)
    SF = ("a", "b")

    def __init__(self, c, a, b):
        self.c, self.a, self.b = c, a, b

    def lines(self, ind):
        L = [ind + f"if ({self.c.emit()}) {{"] + lines(self.a, ind + "    ")
        if self.b:
            L += [ind + "} else {"] + lines(self.b, ind + "    ")
        return L + [ind + "}"]

    def ex(self, m):
        run_list(self.a if truth(self.c.ev(m)) else self.b, m)


class For(Stmt):
    """byte c; for (c = 0; c < n; c++) { body }"""
    SF = ("body",)

    def __init__(self, cv, n, body):
        self.cv, self.n, self.body = cv, n, body

    def lines(self, ind):
        return [ind + f"byte {self.cv}; for ({self.cv} = 0; {self.cv} < {self.n}; {self.cv}++) {{"] + lines(self.body, ind + "    ") + [ind + "}"]

    def ex(self, m):
        c = Cell("byte", 0)
        m.frame[self.cv] = c
        while c.v < self.n:
            try:
                run_list(self.body, m)
            except Brk:
                break
            except Cont:
                pass
            c.v = wrap(c.v + 1, "byte")


class While(Stmt):
    """byte w = n; while (w != 0 && (c)) { w--; body }"""
    EF = ("c",)
    SF = ("body",)

    def __init__(self, wv, n, c, body):
        self.wv, self.n, self.c, self.body = wv, n, c, body

    def lines(self, ind):
        return [ind + f"byte {self.wv} = {self.n}; while ({self.wv} != 0 && {self.c.emit()}) {{", ind + f"    {self.wv}--;"] + \
            lines(self.body, ind + "    ") + [ind + "}"]

    def ex(self, m):
        w = Cell("byte", self.n)
        m.frame[self.wv] = w
        while w.v != 0 and truth(self.c.ev(m)):
            w.v -= 1
            try:
                run_list(self.body, m)
            except Brk:
                break
            except Cont:
                pass


class Block(Stmt):
    SF = ("body",)

    def __init__(self, body):
        self.body = body

    def lines(self, ind):
        return [ind + "{"] + lines(self.body, ind + "    ") + [ind + "}"]

    def ex(self, m):
        run_list(self.body, m)


class Break(Stmt):
    def emit(self):
        return "break;"

    def ex(self, m):
        raise Brk()


class Continue(Stmt):
    def emit(self):
        return "continue;"

    def ex(self, m):
        raise Cont()


class Print(Stmt):
    """print e; out 32;"""
    EF = ("e",)

    def __init__(self, e):
        self.e = e

    def emit(self):
        return f"print {self.e.emit()}; out 32;"

    def ex(self, m):
        v = self.e.ev(m)
        m.out += str(wrap(v.v, v.t) if v.const else v.v).encode() + b" "


class Out(Stmt):
    EF = ("es",)

    def __init__(self, es):
        self.es = es

    def emit(self):
        return f"out {', '.join(e.emit() for e in self.es)};"

    def ex(self, m):
        if not self.es:
            raise ModelError("empty out")
        for e in self.es:
            m.out.append(wrap(e.ev(m).v, "byte"))


class CallS(Stmt):
    EF = ("args",)

    def __init__(self, name, args):
        self.name, self.args = name, args

    def emit(self):
        return f"{self.name}({', '.join(a.emit() for a in self.args)});"

    def ex(self, m):
        p = m.procs.get(self.name)
        if p is None:
            raise ModelError("no such proc")
        m.call(p, self.args)


class Return(Stmt):
    EF = ("e",)

    def __init__(self, e):
        self.e = e

    def emit(self):
        return "return;" if self.e is None else f"return {self.e.emit()};"

    def ex(self, m):
        raise Ret(self.e.ev(m) if self.e is not None else None)


class Proc(Node):
    SF = ("body",)

    def __init__(self, name, ret, params, body, rec):
        self.name, self.ret, self.params, self.body, self.rec = name, ret, params, body, rec

    def lines(self, ind):
        plist = ", ".join(("ref " if ref else "") + f"{t} {n}" for t, ref, n in self.params)
        head = ("rec " if self.rec else "") + "proc " + (self.ret + " " if self.ret else "") + f"{self.name}({plist}) {{"
        return [ind + head] + lines(self.body, ind + "    ") + [ind + "}"]


class Program(Node):
    SF = ("decls", "procs", "main", "dump")

    def __init__(self, decls, procs, main, dump):
        self.decls, self.procs, self.main, self.dump = decls, procs, main, dump

    def text(self):
        return "\n".join(lines(self.decls, "") + lines(self.procs, "") + lines(self.main, "") + lines(self.dump, "")) + "\n"


def lines(lst, ind):
    out = []
    for s in lst:
        out += s.lines(ind)
    return out


def model_run(prog, stdin):
    m = Model(prog, stdin)
    try:
        run_list(prog.decls, m)
        run_list(prog.main, m)
        run_list(prog.dump, m)
    except (Brk, Cont, Ret):
        raise ModelError("control flow outside its construct")
    except (RecursionError, KeyError, TypeError, AttributeError, ValueError, IndexError, ZeroDivisionError) as e:
        raise ModelError(repr(e))
    return bytes(m.out)


# ---- the BFL generator ------------------------------------------------------------------------------
INTERESTING = [0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 15, 16, 17, 31, 32, 63, 64, 100, 127, 128, 129, 200, 254, 255, 256, 257,
               1000, 4095, 4096, 32767, 32768, 32769, 40000, 65535, 65536, 65537, 100000, 1 << 24, 2147483647,
               2147483648, 4294967295]
OPS = ["+"] * 4 + ["-"] * 4 + ["*"] * 3 + ["/", "%"] * 1 + ["&", "|", "^"] + ["==", "!=", "<", "<=", ">", ">="] + \
      ["&&", "||"] + ["<<", ">>"] * 2
COMPOUND = ["+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="]


class Cx:
    """What generated code may use here."""

    def __init__(self, **kw):
        self.__dict__.update(kw)

    def but(self, **kw):
        c = Cx(**self.__dict__)
        c.__dict__.update(kw)
        return c


class Gen:
    def __init__(self, r):
        self.r = r
        self.n = 0
        self.vt = {}            # scalar name -> type
        self.consts = {}        # constant name -> V

    def fresh(self, p):
        self.n += 1
        return f"{p}{self.n}"

    # expressions
    def lit(self):
        r = self.r
        k = r.random()
        if k < 0.6:
            v = r.choice(INTERESTING)
        elif k < 0.8:
            v = r.randint(0, 255)
        else:
            v = r.randint(0, (1 << r.choice([16, 32])) - 1)
        return Lit(v, 1 if r.random() < 0.08 else 2 if r.random() < 0.05 else 0)

    def leaf(self, cx):
        r = self.r
        k = r.random()
        if k < 0.5 and cx.reads:
            return Var(r.choice(cx.reads))
        if k < 0.6 and cx.arrs:
            return self.index_of(cx, r.choice(cx.arrs), 2)
        if k < 0.64 and cx.ptrs:
            return self.index_of(cx, r.choice(cx.ptrs), 2)
        if k < 0.68 and self.consts:
            return CRef(r.choice(list(self.consts)))
        if k < 0.74:
            return Un("-", self.lit())
        return self.lit()

    def index_expr(self, cx, n, depth, heap):
        """An index known to be in 0..n-1."""
        r = self.r
        if cx.counters and r.random() < 0.3:
            c = r.choice(cx.counters)
            return Bin("%", Var(c), Lit(n))
        t = "word" if heap and r.random() < 0.4 else "byte"
        return Bin("%", Cast(t, self.expr(cx, depth + 1)), Lit(n))

    def index_of(self, cx, a, depth, lvalue=False):
        name, t, n, heap = a
        if heap == "ptr" and self.r.random() < 0.4:
            i = None
        else:
            i = self.index_expr(cx, n, depth, heap)
        return IdxL(name, i) if lvalue else Idx(name, i)

    def count(self, cx, depth):
        """A shift count: usually small, sometimes anything."""
        r = self.r
        k = r.random()
        if k < 0.4:
            return Lit(r.choice([0, 1, 2, 3, 4, 5, 7, 8, 9, 12, 15, 16, 17, 23, 24, 25, 31, 32, 33, 40, 255, 256, 257, 65536]))
        if k < 0.45:
            return Un("-", Lit(r.choice([1, 2, 8, 255, 256])))
        if k < 0.65:
            return Bin("&", Cast("byte", self.expr(cx, depth + 1)), Lit(r.choice([7, 15, 31])))
        if k < 0.75 and cx.reads:
            return Var(r.choice(cx.reads))           # any type and value: wide, negative, 256 and up
        if k < 0.8:
            return Cast(r.choice(TNAMES), self.expr(cx, depth + 1))
        return self.expr(cx, depth + 1)

    def call_args(self, cx, p, depth):
        args = []
        for i, (t, ref, _) in enumerate(p.params):
            if i == 0 and p.rec:
                args.append(Bin("%", Cast("byte", self.expr(cx, depth + 1)), Lit(4)))
            else:
                args.append(self.fit(self.expr(cx, depth + 1), t))
        return args

    def expr(self, cx, depth=0):
        r = self.r
        if depth >= 3 or r.random() < 0.22 + 0.12 * depth:
            return self.leaf(cx)
        k = r.random()
        if k < 0.66:
            op = r.choice(OPS)
            a = self.expr(cx, depth + 1)
            b = self.count(cx, depth) if op in ("<<", ">>") else self.expr(cx, depth + 1)
            return Bin(op, a, b)
        if k < 0.71:
            kk = r.choice([0, 1, 4, 7, 8, 9, 12, 15, 16, 17, 20, 24, 31, 32, 33, 40, 48, 63, 64, 65, 100])
            return MulShr(self.expr(cx, depth + 1), self.expr(cx, depth + 1), kk)
        if k < 0.82:
            return Cast(r.choice(TNAMES), self.expr(cx, depth + 1))
        if k < 0.91:
            return Un(r.choice("-~!"), self.expr(cx, depth + 1))
        if cx.pure and depth < 2:
            p = r.choice(cx.pure)
            return Call(p.name, self.call_args(cx, p, depth))
        return self.leaf(cx)

    def fit(self, e, t):
        """e stored in type t: a constant that does not fit gets a cast."""
        c = static_const(e, self.consts)
        if (c is not None and not assign_fits(c.v, t)) or self.r.random() < 0.15:
            return Cast(t, e)
        return e

    # statements
    def stmts(self, cx, n, depth):
        r = self.r
        res = []
        cx = cx.but(targets=list(cx.targets), reads=list(cx.reads), warrs=list(cx.warrs), arrs=list(cx.arrs))
        for _ in range(n):
            k = r.random()
            if k < 0.22 and cx.targets:
                v = r.choice(cx.targets)
                res.append(Assign(VarL(v), "=", self.fit(self.expr(cx), self.vt[v])))
            elif k < 0.32 and cx.targets:
                v = r.choice(cx.targets)
                op = r.choice(COMPOUND)
                res.append(Assign(VarL(v), op, self.count(cx, 0) if op in ("<<=", ">>=") else self.expr(cx)))
            elif k < 0.35 and cx.targets:
                res.append(IncDec(VarL(r.choice(cx.targets)), r.choice(["++", "--"])))
            elif k < 0.45 and cx.warrs:
                a = r.choice(cx.warrs)
                lv = self.index_of(cx, a, 1, lvalue=True)
                if r.random() < 0.7:
                    res.append(Assign(lv, "=", self.fit(self.expr(cx, 1), a[1])))
                elif r.random() < 0.5:
                    res.append(IncDec(lv, r.choice(["++", "--"])))
                else:
                    op = r.choice(COMPOUND)
                    res.append(Assign(lv, op, self.count(cx, 1) if op in ("<<=", ">>=") else self.expr(cx, 1)))
            elif k < 0.48 and cx.targets and cx.arrs:
                v = r.choice(cx.targets)
                res.append(Assign(VarL(v), "=", self.fit(self.index_of(cx, r.choice(cx.arrs), 1), self.vt[v])))
            elif k < 0.58 and depth < 2:
                c = self.expr(cx, 1)
                a = self.stmts(cx, r.randint(1, 3), depth + 1)
                b = self.stmts(cx, r.randint(1, 2), depth + 1) if r.random() < 0.5 else []
                res.append(If(c, a, b))
            elif k < 0.64 and depth < 2:
                cv = self.fresh("c")
                self.vt[cv] = "byte"
                body = self.stmts(cx.but(reads=cx.reads + [cv], counters=cx.counters + [cv], in_loop=True), r.randint(1, 3), depth + 1)
                res.append(For(cv, r.randint(1, 4), body))
                cx.reads.append(cv)              # where a break left it
            elif k < 0.67 and depth < 2:
                wv = self.fresh("w")
                self.vt[wv] = "byte"
                body = self.stmts(cx.but(reads=cx.reads + [wv], in_loop=True), r.randint(1, 3), depth + 1)
                res.append(While(wv, r.randint(1, 4), self.expr(cx, 1), body))
                cx.reads.append(wv)
            elif k < 0.72:
                # a declaration, usable by the rest of this list
                if r.random() < 0.15:
                    t = r.choice(TNAMES)
                    name = self.fresh("la")
                    size = r.randint(2, 5)
                    res.append(ArrDecl(t, name, size, False))
                    cx.warrs.append((name, t, size, False))
                    cx.arrs.append((name, t, size, False))
                else:
                    t = r.choice(TNAMES)
                    name = self.fresh("l")
                    self.vt[name] = t
                    res.append(Decl(t, name, self.fit(self.expr(cx), t) if r.random() < 0.7 else None))
                    cx.targets.append(name)
                    cx.reads.append(name)
            elif k < 0.75 and depth < 2:
                res.append(Block(self.stmts(cx, r.randint(1, 3), depth + 1)))
            elif k < 0.79 and cx.in_loop:
                s = r.choice([Break(), Continue()])
                res.append(s if r.random() < 0.4 else If(self.expr(cx, 1), [s], []))
            elif k < 0.86 and cx.calls:
                p = r.choice(cx.calls)
                args, ok, used = [], True, set()
                for i, (t, ref, _) in enumerate(p.params):
                    if ref:
                        # no variable is passed twice: an inlined ref is the variable itself
                        cands = [v for v in cx.targets if self.vt[v] == t and v not in used]
                        if not cands:
                            ok = False
                            break
                        v = r.choice(cands)
                        used.add(v)
                        args.append(Var(v))
                    elif i == 0 and p.rec:
                        args.append(Bin("%", Cast("byte", self.expr(cx, 1)), Lit(4)))
                    else:
                        args.append(self.fit(self.expr(cx, 1), t))
                if ok:
                    res.append(CallS(p.name, args))
            elif k < 0.89 and cx.out_ok:
                e = self.expr(cx, 1)
                res.append(Print(e) if r.random() < 0.25 else Out([e]))
            elif k < 0.91 and cx.ret is not None:
                res.append(Return(None if cx.ret == "void" else self.fit(self.expr(cx, 1), cx.ret)))
        return res

    def proc(self, cx, i, procs):
        """One procedure; it may call the ones before it."""
        r = self.r
        name = f"p{i}"
        rec = r.random() < 0.25
        kind = r.choice(["pure", "pure", "impure", "ref"]) if not rec else r.choice(["pure", "impure"])
        ret = r.choice(TNAMES) if kind == "pure" else r.choice([None, None] + TNAMES)
        params = []
        if rec:
            params.append(("byte", False, f"{name}n"))
        for k in range(r.randint(0 if kind != "ref" else 1, 3 - rec)):
            t = r.choice(TNAMES)
            ref = kind == "ref" and (k == 0 or r.random() < 0.4)
            params.append((t, ref, f"{name}a{k}"))
        for t, _, pn in params:
            self.vt[pn] = t
        pnames = [pn for _, _, pn in params]
        mine = [pn for pn in pnames if not (rec and pn == f"{name}n")]
        pure = [p for p in procs if p.kind == "pure"]
        g = cx.globals_ if kind != "ref" else []
        garrs = cx.garrs if kind != "ref" else []
        gptrs = cx.gptrs if kind != "ref" else []
        body = []
        local_reads = list(pnames)
        local_targets = list(mine)
        for k in range(r.randint(0, 2)):
            t = r.choice(TNAMES)
            ln = f"{name}l{k}"
            self.vt[ln] = t
            body.append(Decl(t, ln, self.fit(self.expr(cx.but(reads=local_reads + g, arrs=garrs, ptrs=gptrs, pure=pure, counters=[])), t)))
            local_reads.append(ln)
            local_targets.append(ln)
        larrs = []
        if r.random() < 0.3:
            t, size = r.choice(TNAMES), r.randint(2, 4)
            body.append(ArrDecl(t, f"{name}arr", size, False))
            larrs.append((f"{name}arr", t, size, False))
        ret_t = ret if ret else "void"
        if kind == "pure":
            bcx = cx.but(targets=local_targets, reads=local_reads + g, warrs=larrs, arrs=larrs + garrs, ptrs=gptrs,
                         pure=pure, calls=[], counters=[], in_loop=False, out_ok=False, ret=ret_t)
        elif kind == "impure":
            bcx = cx.but(targets=local_targets + g, reads=local_reads + g, warrs=larrs + cx.gwarrs, arrs=larrs + garrs,
                         ptrs=gptrs, pure=pure, calls=[p for p in procs], counters=[], in_loop=False, out_ok=True, ret=ret_t)
        else:
            bcx = cx.but(targets=local_targets, reads=local_reads, warrs=larrs, arrs=larrs, ptrs=[], pure=[],
                         calls=[p for p in procs if p.kind == "ref"], counters=[], in_loop=False, out_ok=True, ret=ret_t)
        body += self.stmts(bcx, r.randint(2, 5), 0)
        p = Proc(name, ret, params, body, rec)
        p.kind = kind
        if rec:
            n = Var(f"{name}n")
            down = Cast("byte", Bin("-", n, Lit(1)))
            # every call to itself counts n down, so the recursion ends
            if kind == "pure":
                base = Return(self.fit(self.expr(bcx, 1), ret))
                body.append(If(Bin("==", n, Lit(0)), [base], []))
                calls = [Call(name, [down] + [self.fit(self.expr(bcx, 2), t) for t, _, _ in params[1:]]) for _ in range(r.choice([1, 1, 2]))]
                e = calls[0]
                for c in calls[1:]:
                    e = Bin(r.choice(["+", "-", "^", "*"]), e, c)
                e = Bin(r.choice(OPS), e, self.expr(bcx, 2)) if r.random() < 0.6 else e
                body.append(Return(self.fit(e, ret)))
            else:
                then = []
                for _ in range(r.choice([1, 1, 2])):
                    then.append(CallS(name, [down] + [self.fit(self.expr(bcx, 2), t) for t, _, _ in params[1:]]))
                    then += self.stmts(bcx, r.randint(0, 2), 1)
                body.append(If(Bin("!=", n, Lit(0)), then, []))
                body += self.stmts(bcx, r.randint(0, 2), 1)
                if ret:
                    body.append(Return(self.fit(self.expr(bcx, 1), ret)))
        elif ret:
            body.append(Return(self.fit(self.expr(bcx, 1), ret)))
        return p

    def program(self):
        r = self.r
        decls, stdin = [], []
        for i in range(r.randint(0, 2)):
            name = f"K{i}"
            e = self.lit() if r.random() < 0.6 else Cast(r.choice(TNAMES), self.lit())
            decls.append(ConstDecl(name, e))
            self.consts[name] = static_const(e, self.consts)
        g = []
        for i in range(r.randint(2, 5)):
            t = r.choice(TNAMES)
            name = f"g{i}"
            self.vt[name] = t
            g.append(name)
            if r.random() < 0.5:
                nb = TYPES[t][0] // 8
                decls.append(Decl(t, name, In()))
                stdin.append(r.randint(0, 255))
                for _ in range(nb - 1):
                    decls.append(Assign(VarL(name), "=", Bin("+", Bin("*", Var(name), Lit(256)), In())))
                    stdin.append(r.randint(0, 255))
            else:
                decls.append(Decl(t, name, self.fit(self.lit(), t)))
        garrs, gptrs, harrs = [], [], []
        for i in range(r.choice([0, 1, 1, 2])):
            t, size = r.choice(TNAMES), r.randint(2, 6)
            decls.append(ArrDecl(t, f"ga{i}", size, False))
            garrs.append((f"ga{i}", t, size, False))
        for i in range(r.choice([0, 0, 1, 2])):
            t, size = r.choice(TNAMES), r.randint(2, 6)
            decls.append(ArrDecl(t, f"ha{i}", size, True))
            harrs.append((f"ha{i}", t, size, True))
        for i in range(r.choice([0, 1]) if harrs else 0):
            name, t, size, _ = r.choice(harrs)
            j = r.randint(0, size - 1)
            decls.append(PtrDecl(t, f"pt{i}", name, j))
            gptrs.append((f"pt{i}", t, size - j, "ptr"))
        cx = Cx(globals_=g, garrs=garrs + harrs, gwarrs=garrs + harrs + gptrs, gptrs=gptrs)
        procs = []
        for i in range(r.choice([0, 1, 2, 2, 3, 4])):
            procs.append(self.proc(cx, i, procs))
        mcx = cx.but(targets=list(g), reads=list(g), warrs=garrs + harrs + gptrs, arrs=garrs + harrs, ptrs=gptrs,
                     pure=[p for p in procs if p.kind == "pure"], calls=procs, counters=[], in_loop=False, out_ok=True, ret=None)
        main = self.stmts(mcx, r.randint(4, 10), 0)
        dump = []
        for name in g:
            nb = TYPES[self.vt[name]][0] // 8
            dump.append(Out([Cast("byte", Bin(">>", Var(name), Lit(8 * k))) for k in range(nb)]))
        for name, t, size, _ in garrs + harrs:
            nb = TYPES[t][0] // 8
            dump.append(Out([Cast("byte", Bin(">>", Idx(name, Lit(i)), Lit(8 * k))) for i in range(size) for k in range(nb)]))
        stdin += [r.randint(0, 255) for _ in range(4)]
        return Program(decls, procs, main, dump), bytes(stdin)


def gen_bfl(seed):
    """A program and its input for one seed; retries until the model can run it."""
    r = random.Random(seed)
    for _ in range(20):
        prog, stdin = Gen(r).program()
        try:
            expected = model_run(prog, stdin)
        except ModelError:
            continue
        return prog, stdin, expected
    return None


# ---- the brainfuck generator and reference interpreter ----------------------------------------------
def pm(d):
    return "+" * d if d > 0 else "-" * -d


def bf_balanced(r, depth):
    """A loop whose body comes back to the loop cell: a multiply, an if, a carry, an idiom candidate."""
    offs = r.sample([o for o in range(-3, 5) if o], r.randint(1, 4))
    segs = []
    for o in offs:
        for _ in range(r.randint(1, 2)):
            k = r.random()
            if k < 0.6:
                segs.append((o, pm(r.choice([1, 1, 1, 2, 3, -1, -1, -2, 255, -128]))))
            elif k < 0.72:
                segs.append((o, "[-]"))
            elif k < 0.85 and depth < 3:
                segs.append((o, bf_balanced(r, depth + 1)))
            elif k < 0.93:
                segs.append((o, "."))
            else:
                segs.append((o, "[-]+"))
    lc = r.choice(["-", "-", "-", "+", "---", "-", "[-]", "--", "+++"])
    segs.insert(r.randint(0, len(segs)), (0, lc))
    s, pos = "[", 0
    for o, text in segs:
        s += (">" * (o - pos) if o > pos else "<" * (pos - o)) + text
        pos = o
    return s + (">" * -pos if pos < 0 else "<" * pos) + "]"


def bf_seq(r, depth, n):
    parts = []
    for _ in range(n):
        k = r.random()
        if k < 0.22:
            parts.append(r.choice("+-") * r.choice([1, 1, 2, 3, 5, 8, 13, 40, 128, 255]))
        elif k < 0.36:
            parts.append(r.choice("<>") * r.randint(1, 3))
        elif k < 0.42:
            parts.append(".")
        elif k < 0.45:
            parts.append(",")
        elif k < 0.72 and depth < 3:
            parts.append(bf_balanced(r, depth))
        elif k < 0.82:
            parts.append(r.choice(["[-]", "[+]", "[-]+", "[>]", "[<]", "[>>]", "[<<]", "[->+<]", "[-<+>]", "[->>+<<]"]))
        elif depth < 3:
            parts.append("[" + bf_seq(r, depth + 1, r.randint(1, 4)) + "]")
    return "".join(parts)


class BfLimit(Exception):
    pass


def bf_reference(src, stdin, eof, limit=600_000):
    """Plain 8-bit brainfuck on a tape that starts at cell 0 (runs of a command count once)."""
    code = []
    for c in src:
        if c in "+-<>" and code and code[-1][0] == c:
            code[-1][1] += 1
        elif c in "+-<>.,[]":
            code.append([c, 1])
    jump, stack = {}, []
    for i, (c, _) in enumerate(code):
        if c == "[":
            stack.append(i)
        elif c == "]":
            j = stack.pop()
            jump[i], jump[j] = j, i
    tape = bytearray(1 << 16)
    p = pc = inp = steps = 0
    out = bytearray()
    n = len(code)
    while pc < n:
        steps += 1
        if steps > limit:
            raise BfLimit("steps")
        c, k = code[pc]
        if c == "+":
            tape[p] = (tape[p] + k) & 255
        elif c == "-":
            tape[p] = (tape[p] - k) & 255
        elif c == ">":
            p += k
            if p >= len(tape):
                raise BfLimit("right")
        elif c == "<":
            p -= k
            if p < 0:
                raise BfLimit("left")
        elif c == ".":
            out.append(tape[p])
            if len(out) > 100_000:
                raise BfLimit("output")
        elif c == ",":
            if inp < len(stdin):
                tape[p] = stdin[inp]
                inp += 1
            elif eof == "0":
                tape[p] = 0
            elif eof == "255":
                tape[p] = 255
        elif c == "[":
            if tape[p] == 0:
                pc = jump[pc]
        elif tape[p] != 0:
            pc = jump[pc]
        pc += 1
    return bytes(out)


def gen_bf(seed):
    r = random.Random(seed)
    for _ in range(50):
        src = ">" * 8 + bf_seq(r, 0, r.randint(4, 14))
        stdin = bytes(r.randint(0, 255) for _ in range(r.randint(0, 6)))
        eof = r.choice(["0", "255", "nochange"])
        try:
            return src, stdin, eof, bf_reference(src, stdin, eof)
        except BfLimit:
            continue
    return None


# ---- running bf.exe ------------------------------------------------------------------------------------
SWITCHES = ("BF_NO_IDIOMS", "BF_NO_REBUILD", "BFL_NO_LAYOUT", "BF_IDIOM_DEBUG", "BF_TRACE")
BASE_ENV = {k: v for k, v in os.environ.items() if k not in SWITCHES}

MODES = [
    ("jit", [], {}),
    ("interp", ["--interp"], {}),
    ("noopt", ["--no-opt"], {}),
    ("interp-noopt", ["--interp", "--no-opt"], {}),
    ("jit-noidioms", [], {"BF_NO_IDIOMS": "1"}),
    ("jit-norebuild", [], {"BF_NO_REBUILD": "1"}),
    ("interp-noidioms", ["--interp"], {"BF_NO_IDIOMS": "1"}),
    ("interp-norebuild", ["--interp"], {"BF_NO_REBUILD": "1"}),
]


SLOW = {"interp-noopt"}      # may run out of time on a big program: that is not a failure


def run(bf, args, stdin=b"", env=None, timeout=60):
    try:
        r = subprocess.run([bf] + args, input=stdin, capture_output=True, timeout=timeout,
                           env={**BASE_ENV, **env} if env else BASE_ENV)
        return r.returncode, r.stdout, r.stderr.decode("latin-1")
    except subprocess.TimeoutExpired:
        return None, b"", "timeout"


class Failure:
    def __init__(self, kind, mode, detail):
        self.kind, self.mode, self.detail = kind, mode, detail

    def __str__(self):
        return f"{self.kind}{' ' + self.mode if self.mode else ''}: {self.detail}"


def describe(rc, out, expected, err):
    return f"rc={rc} out={out[:48]!r}{'...' if len(out) > 48 else ''} expected={expected[:48]!r}" + \
           (f" stderr={err.strip()[:120]!r}" if err.strip() else "")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def check_bf_file(bf, path, stdin, eof, expected, modes):
    fails = []
    for label, flags, env in modes:
        rc, out, err = run(bf, [path, "--quiet-devices", "--eof", eof] + flags, stdin, env, timeout=30 if label in SLOW else 60)
        if rc is None and label in SLOW:
            continue
        if rc != 0 or out != expected:
            fails.append(Failure("run", label, describe(rc, out, expected, err)))
    return fails


def check_bfl(bf, d, text, stdin, expected, modes=MODES, pokes=None, determinism=True, layout=True):
    """Compile and run one BFL program; returns the failures.  pokes: (cell offset, value) pairs."""
    src = os.path.join(d, "p.bfl")
    with open(src, "w", newline="\n") as f:
        f.write(text)
    a = os.path.join(d, "p.b")
    rc, out, err = run(bf, ["--emit", a, src])
    if rc != 0:
        return [Failure("compile", "", err.strip()[:300])]
    cells = int(m.group(1)) if (m := re.search(r"(\d+) cells", out.decode("latin-1") + err)) else 32
    fails = []
    if determinism:
        b = os.path.join(d, "p2.b")
        run(bf, ["--emit", b, src])
        if read(a) != read(b) or read(a + ".map") != read(b + ".map"):
            fails.append(Failure("determinism", "", "two compilations of the same source differ"))
    fails += check_bf_file(bf, a, stdin, "nochange", expected, modes)
    if layout:
        c = os.path.join(d, "p_nolayout.b")
        rc, _, err = run(bf, ["--emit", c, src], env={"BFL_NO_LAYOUT": "1"})
        if rc != 0:
            fails.append(Failure("layout", "compile", err.strip()[:200]))
        else:
            fails += [Failure("layout", f.mode, f.detail) for f in check_bf_file(bf, c, stdin, "nochange", expected, modes[:1])]
    if pokes is None:
        return fails
    base = "".join(ch for ch in read(a).decode("latin-1") if ch in "+-<>.,[]")
    for cell, val in pokes(cells):
        cell += 256
        v = os.path.join(d, "poke.b")
        with open(v, "w", encoding="latin-1") as f:
            f.write(">" * cell + pm(val) + "<" * cell + base)
        rc0, want, _ = run(bf, [v, "--quiet-devices", "--no-opt"], stdin, timeout=10)
        if rc0 is None:
            continue                     # the poke made it spin
        for label, flags, env in (modes[0], modes[1]):
            rc, out, err = run(bf, [v, "--quiet-devices"] + flags, stdin, env, timeout=30)
            if rc != rc0 or out != want:
                fails.append(Failure("poke", f"{label} cell {cell} {val:+d}", describe(rc, out, want, err)))
    return fails


def poker(seed):
    r = random.Random(seed * 7919 + 1)

    def pokes(cells):
        return [(r.randint(0, cells + 4), r.choice([1, 1, 2, -1, 127, -128])) for _ in range(2)]
    return pokes


# ---- shrinking --------------------------------------------------------------------------------------------
def candidates(prog):
    """Edits that make the program smaller: (apply, undo) pairs."""
    out = []

    def lists(node):
        for f in node.SF:
            lst = getattr(node, f)
            yield lst
            for s in lst:
                yield from lists(s)

    def exprs(node, holder, key):
        """Every expression slot below node."""
        for f in node.EF:
            val = getattr(node, f)
            if isinstance(val, list):
                for i, x in enumerate(val):
                    if isinstance(x, Node):
                        yield val, i, x
                        yield from exprs(x, val, i)
            elif isinstance(val, Node):
                if isinstance(val, Expr):
                    yield node, f, val
                yield from exprs(val, node, f)
        for f in node.SF:
            for s in getattr(node, f):
                yield from exprs(s, None, None)

    def setter(holder, key):
        if isinstance(holder, list):
            return lambda v: holder.__setitem__(key, v)
        return lambda v: setattr(holder, key, v)

    for lst in lists(prog):
        for i in range(len(lst)):
            s = lst[i]
            out.append((lambda lst=lst, i=i, s=s: lst.pop(i), lambda lst=lst, i=i, s=s: lst.insert(i, s)))
            for f in s.SF if not isinstance(s, Proc) else ():
                body = getattr(s, f)
                out.append((lambda lst=lst, i=i, body=body: lst.__setitem__(slice(i, i + 1), body),
                            lambda lst=lst, i=i, body=body, s=s: lst.__setitem__(slice(i, i + len(body)), [s])))
    for holder, key, e in exprs(prog, None, None):
        put = setter(holder, key)
        repl = [getattr(e, f) for f in e.EF if isinstance(getattr(e, f), Expr)]
        if isinstance(e, Call):
            repl += [a for a in e.args if isinstance(a, Expr)]
        if not (isinstance(e, Lit) and e.v in (0, 1)):
            repl += [Lit(0), Lit(1)]
        for x in repl:
            out.append((lambda put=put, x=x: put(x), lambda put=put, e=e: put(e)))
    return out


def shrink_bfl(bf, prog, stdin, fails, d, log):
    kinds = {f.kind for f in fails}
    modes = [m for m in MODES if any(f.mode.split(" ")[0] == m[0] for f in fails)] or MODES[:1]
    compile_msg = next((re.sub(r"^.*?:\d+:\d+: ", "", f.detail.splitlines()[0]) for f in fails if f.kind == "compile"), None)
    pokes = None
    poke_specs = [(int(f.mode.split()[2]) - 256, int(f.mode.split()[3])) for f in fails if f.kind == "poke"]
    if poke_specs:
        pokes = lambda cells: poke_specs[:1]          # noqa: E731
        modes = MODES[:2]

    def still(p):
        try:
            expected = model_run(p, stdin)
        except ModelError:
            return False
        res = check_bfl(bf, d, p.text(), stdin, expected, modes, pokes, "determinism" in kinds, "layout" in kinds)
        if compile_msg is not None:
            return any(f.kind == "compile" and compile_msg in f.detail for f in res)
        return any(f.kind in kinds for f in res)

    tries = 0
    changed = True
    while changed:
        changed = False
        i = 0
        cands = candidates(prog)
        while i < len(cands):
            apply, undo = cands[i]
            apply()
            tries += 1
            if still(prog):
                changed = True
                cands = candidates(prog)
                continue
            undo()
            i += 1
        log(f"      shrink pass: {len(prog.text())} bytes after {tries} tries")
    return prog


def balanced(s):
    depth = 0
    for c in s:
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def shrink_bf(bf, src, stdin, eof, fails, d):
    modes = [m for m in MODES if any(f.mode == m[0] for f in fails)] or MODES
    path = os.path.join(d, "shrink.b")

    def still(s):
        if not balanced(s):
            return False
        try:
            expected = bf_reference(s, stdin, eof)
        except BfLimit:
            return False
        with open(path, "w", encoding="latin-1") as f:
            f.write(s)
        return bool(check_bf_file(bf, path, stdin, eof, expected, modes))

    changed = True
    while changed:
        changed = False
        size = max(len(src) // 2, 1)
        while size >= 1:
            i = 0
            while i < len(src):
                cand = src[:i] + src[i + size:]
                if cand != src and still(cand):
                    src, changed = cand, True
                else:
                    i += size
            size //= 2
        for i in range(len(src)):          # a loop's brackets, keeping its body
            if src[i] == "[":
                depth, j = 0, i
                for j in range(i, len(src)):
                    depth += {"[": 1, "]": -1}.get(src[j], 0)
                    if depth == 0:
                        break
                cand = src[:i] + src[i + 1:j] + src[j + 1:]
                if still(cand):
                    src, changed = cand, True
                    break
    return src


# ---- one case of each kind -----------------------------------------------------------------------------
class Result:
    def __init__(self, kind, seed, status, fails=(), data=None):
        self.kind, self.seed, self.status, self.fails, self.data = kind, seed, status, list(fails), data


def bfl_case(bf, seed, tmp, modes=MODES, pokes=True):
    g = gen_bfl(seed)
    if g is None:
        return Result("bfl", seed, "skip")
    prog, stdin, expected = g
    d = os.path.join(tmp, f"bfl{seed}")
    os.makedirs(d, exist_ok=True)
    fails = check_bfl(bf, d, prog.text(), stdin, expected, modes, poker(seed) if pokes else None)
    return Result("bfl", seed, "fail" if fails else "ok", fails, (prog, stdin, expected))


def bf_case(bf, seed, tmp, modes=MODES):
    g = gen_bf(seed)
    if g is None:
        return Result("bf", seed, "skip")
    src, stdin, eof, expected = g
    d = os.path.join(tmp, f"bf{seed}")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "p.b")
    with open(path, "w", encoding="latin-1") as f:
        f.write(src)
    fails = check_bf_file(bf, path, stdin, eof, expected, modes)
    return Result("bf", seed, "fail" if fails else "ok", fails, (src, stdin, eof, expected))


def run_cases(bf, bfl_n, bf_n, seed0, tmp, jobs=None, log=print, progress=False):
    """Run bfl_n BFL and bf_n brainfuck programs from seed0 on; returns the results."""
    jobs = jobs or max(1, (os.cpu_count() or 2))
    work = [("bfl", seed0 + i) for i in range(bfl_n)] + [("bf", seed0 + i) for i in range(bf_n)]
    results = []
    lock = threading.Lock()
    t0 = last = time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(bfl_case if k == "bfl" else bf_case, bf, s, tmp): (k, s) for k, s in work}
        for f in concurrent.futures.as_completed(futs):
            try:
                res = f.result()
            except Exception as e:      # a bug in the fuzzer itself
                k, s = futs[f]
                res = Result(k, s, "fail", [Failure("fuzzer", "", repr(e))])
            with lock:
                results.append(res)
                if res.status == "fail":
                    log(f"FAIL {res.kind} seed {res.seed}: " + "; ".join(str(x) for x in res.fails[:3]))
                if progress and time.time() - last > 10:
                    last = time.time()
                    nf = sum(r.status == "fail" for r in results)
                    log(f"   {len(results)}/{len(work)} programs, {nf} failing, {time.time() - t0:.0f} s")
    return results


def save_failure(res, outdir, bf, tmp, shrink, log):
    os.makedirs(outdir, exist_ok=True)
    base = os.path.join(outdir, f"{res.kind}-{res.seed}")
    notes = [f"seed {res.seed}"] + [str(f) for f in res.fails]
    if res.kind == "bfl":
        prog, stdin, expected = res.data
        with open(base + ".bfl", "w", newline="\n") as f:
            f.write(prog.text())
        notes.append(f"stdin {stdin!r}")
        notes.append(f"expected {expected!r}")
        if shrink:
            d = os.path.join(tmp, f"shrink-bfl{res.seed}")
            os.makedirs(d, exist_ok=True)
            small = shrink_bfl(bf, prog, stdin, res.fails, d, log)
            with open(base + ".min.bfl", "w", newline="\n") as f:
                f.write(small.text())
            try:
                notes.append(f"minimal: expected {model_run(small, stdin)!r}")
            except ModelError:
                pass
    else:
        src, stdin, eof, expected = res.data
        with open(base + ".b", "w", encoding="latin-1") as f:
            f.write(src)
        notes.append(f"stdin {stdin!r} --eof {eof}")
        notes.append(f"expected {expected!r}")
        if shrink:
            d = os.path.join(tmp, f"shrink-bf{res.seed}")
            os.makedirs(d, exist_ok=True)
            small = shrink_bf(bf, src, stdin, eof, res.fails, d)
            with open(base + ".min.b", "w", encoding="latin-1") as f:
                f.write(small)
            notes.append(f"minimal: {small}  expected {bf_reference(small, stdin, eof)!r}")
    with open(base + ".txt", "w") as f:
        f.write("\n".join(notes) + "\n")
    log(f"   written: {base}.*")


def replay(bf, kind, seed, tmp):
    if kind == "bfl":
        g = gen_bfl(seed)
        if g is None:
            print("no program for this seed")
            return 0
        prog, stdin, expected = g
        print(prog.text())
        print(f"stdin {stdin!r}\nexpected {expected!r}")
        d = os.path.join(tmp, "replay")
        os.makedirs(d, exist_ok=True)
        fails = check_bfl(bf, d, prog.text(), stdin, expected, MODES, poker(seed))
        print(f"compiled to {os.path.join(d, 'p.b')}")
    else:
        g = gen_bf(seed)
        if g is None:
            print("no program for this seed")
            return 0
        src, stdin, eof, expected = g
        print(src)
        print(f"stdin {stdin!r} --eof {eof}\nexpected {expected!r}")
        d = os.path.join(tmp, "replay")
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, "p.b")
        with open(path, "w", encoding="latin-1") as f:
            f.write(src)
        fails = check_bf_file(bf, path, stdin, eof, expected, MODES)
    for f in fails:
        print("FAIL " + str(f))
    print("ok" if not fails else f"{len(fails)} failures")
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description="Differential fuzzer for the BFL compiler, the optimiser and the JIT")
    ap.add_argument("bf", nargs="?", default=DEFAULT_BF)
    ap.add_argument("--bfl", type=int, default=200, help="BFL programs (default 200)")
    ap.add_argument("--bf", dest="bf_n", type=int, default=200, help="brainfuck programs (default 200)")
    ap.add_argument("--seed", type=int, default=1, help="first seed (default 1)")
    ap.add_argument("--jobs", type=int, default=None, help="programs run at once (default: one per CPU)")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "bf-fuzz"), help="where failing programs go")
    ap.add_argument("--no-shrink", action="store_true", help="do not shrink failing programs")
    ap.add_argument("--replay", nargs=2, metavar=("KIND", "SEED"), help="run one program (bfl or bf) and show it")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="bffuzz")
    if a.replay:
        sys.exit(replay(a.bf, a.replay[0], int(a.replay[1]), tmp))
    t0 = time.time()
    results = run_cases(a.bf, a.bfl, a.bf_n, a.seed, tmp, a.jobs, progress=True)
    failed = sorted((r for r in results if r.status == "fail"), key=lambda r: (r.kind, r.seed))
    for res in failed:
        save_failure(res, a.out, a.bf, tmp, not a.no_shrink, print)
    for kind in ("bfl", "bf"):
        rs = [r for r in results if r.kind == kind]
        if rs:
            print(f"{kind}: {sum(r.status != 'skip' for r in rs)} programs checked, "
                  f"{sum(r.status == 'skip' for r in rs)} skipped, {sum(r.status == 'fail' for r in rs)} failing")
    print(f"{time.time() - t0:.0f} s")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
