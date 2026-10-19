"""Differential fuzzer for the BFL compiler, the IR optimiser and the JIT
(docs/ROADMAP.md, phases 9 and 19).

Two generators:

  bfl  random BFL programs over every integer type: arithmetic, shifts, mulshr,
       compares, casts, constants, sizeof, arrays in cells and in the heap,
       pointers, loops with break and continue, procedures (inlined, rec and
       shared), and runs of statements with several wide multiplies or divisions
       of one kind (which the compiler shares), some reading variables given
       constants just before or inside the run.  Structs: up to three types with
       integer fields, array fields and earlier structs (and arrays of them) as
       fields; struct variables, arrays of structs in cells and in the heap,
       pointers to them (p->f, (*p).f, p[i].f), fields read and written, whole
       structs copied by assignment, passed by value and by ref and returned
       (also mk(1).f).  Procedures take ref array parameters (ref T a[], T an
       integer type or a struct), in cells when inlined and heap arrays when
       trampolined, and some procs are marked shared.  A procedure the program
       does not call gets a call added in the main program (declaring what its
       ref and array arguments need, printed after it) or in a later procedure,
       so that what is declared runs; calls such as f(i, a[i]), where f writes
       i, are frequent.  Each program is also run
       by a model of BFL written here in Python, the oracle.  It is compiled twice
       (the brainfuck must be identical), once without the automatic layout of
       globals, once without shared arithmetic (BFL_SHARE=0) and, when it has
       shared procs, once with them inlined instead, and run by every executor
       (the last three by the JIT; the summary counts the programs whose code the
       arithmetic sharing changed and those with shared procs).  Its brainfuck is
       also run with one cell poked before the start, compared with the
       unoptimised run, so that scratch cells hold unexpected values and the loop
       idioms' guards must send loops through their bodies.  A program whose
       recursion overflows the default stack (unchecked at run time) is found with
       --check-stack and given a bigger one.
  bf   random brainfuck aimed at the loop shapes the optimiser rewrites,
       checked against a plain interpreter in Python.

Inlined and trampolined (rec, shared) procedures legitimately differ in two
ways, which the generator keeps out of its programs (it took them over from the
former tests/fuzz_shared.py):
  - the order of side effects when a call sits inside an expression: calls in
    an expression run before the rest of it when trampolined, and the right
    operand of && or || is always evaluated.  So only pure procedures (which
    write and print nothing outside themselves) are called in expressions.
  - ref parameters: an inlined procedure works on the argument's cells where it
    can, a trampolined one (and an inlined one for a place outside cells) copies
    the argument in and back out.  So a procedure with ref parameters touches no
    globals and calls nothing that does, and no two ref or array arguments of
    one call share storage (a variable, an array, a struct, the heap array a
    pointer points into; an array parameter may be any global array of its
    element type).  Where the argument is copied back to does not change: its
    index or address is computed once, at the call.
Array parameters are always bound to the array itself, inlined or not.  A
trampolined procedure takes a pointer, so its array arguments are heap arrays,
array parameters, or local arrays of a re-entrant procedure (which live in the
heap).  An array field of a struct held in cells is indexed by a constant; in
the heap (a heap array, a pointer, a re-entrant procedure's local array, an
array parameter given only heap arrays, as a trampolined one always is) by
anything.

The executors: the JIT and the IR interpreter, each with optimisation, without
it, and without the loop idioms or the block rebuilding (BF_NO_IDIOMS,
BF_NO_REBUILD).

A failing program is shrunk (statements, expressions and brainfuck removed while
it still fails the same way) and written out next to the original.

    python tests/fuzz.py [--bfl N] [--bf N] [--seed S] [--focus shared|struct] [--jobs J] [--out DIR] [--no-shrink] [bf.exe]
    python tests/fuzz.py --replay bfl|bf SEED [--focus F]     one program: its text and every executor's output

--focus makes shared procs or structs more frequent (other seeds).  tests/test.py
and tests/test_lang.py run a few seeds of each (test_lang.py also of --focus shared).
"""
import argparse
import concurrent.futures
import itertools
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import traceback

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


class Alias:
    """A ref parameter bound to a place that is not a whole variable: a field, an element, a heap location."""
    __slots__ = ("t", "get", "put")

    def __init__(self, t, get, put):
        self.t, self.get, self.put = t, get, put

    @property
    def v(self):
        return self.get()

    @v.setter
    def v(self, x):
        self.put(x)


SCALARS = (Cell, Alias)       # what a variable name can stand for, besides arrays and pointers


class Arr:
    __slots__ = ("t", "a")

    def __init__(self, t, n, st=None):
        self.t, self.a = t, [zero(t, st) for _ in range(n)]


class Ptr:
    __slots__ = ("arr", "j")

    def __init__(self, arr, j):
        self.arr, self.j = arr, j


# Types other than the integer ones: a struct is its name, an array field ("[]", element type, length),
# a pointer the element type's name with a "*" (only for sizeof).  st maps struct names to StructDecls.
def is_struct(t):
    return isinstance(t, str) and t not in TYPES and not t.endswith("*")


def zero(t, st):
    if isinstance(t, tuple):
        return [zero(t[1], st) for _ in range(t[2])]
    if t in TYPES:
        return 0
    if st is None or t not in st:
        raise ModelError("unknown type " + str(t))
    return {f: zero(ft, st) for f, ft in st[t].fields}


def tsize(t, st):
    if isinstance(t, tuple):
        return t[2] * tsize(t[1], st)
    if t in TYPES:
        return TYPES[t][0] // 8
    if t.endswith("*"):
        return 2
    return sum(tsize(ft, st) for _, ft in st[t].fields)


def clone(v):
    """A struct value is copied whole: fields are dicts, array fields lists."""
    if isinstance(v, dict):
        return {k: clone(x) for k, x in v.items()}
    if isinstance(v, list):
        return [clone(x) for x in v]
    return v


def same_type(t, v):
    """A value of struct type t, for a struct copy (structs convert to nothing else)."""
    if not is_struct(t) or v.t != t:
        raise ModelError("struct type")
    return clone(v.v)


class Model:
    def __init__(self, prog, stdin):
        self.out = bytearray()
        self.inp = list(stdin)
        self.globals = {}
        self.frame = self.globals
        self.consts = {}
        self.procs = {p.name: p for p in prog.procs}
        self.st = {s.name: s for s in prog.structs}
        self.steps = 0
        self.depth = 0
        self.ran = set()                 # the procedures called

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
        self.ran.add(p.name)
        frame = {}
        for (t, ref, pn), a in zip(p.params, args):
            if ref == "[]":
                # an array parameter stands for the array itself (in cells, inlined) or its address
                if not isinstance(a, Var):
                    raise ModelError("array argument")
                arr = self.lookup(a.name)
                if not isinstance(arr, Arr) or arr.t != t or len(arr.a) < p.asize.get(pn, 1):
                    raise ModelError("array argument type")
                frame[pn] = arr
            elif ref:
                # bound to the place for the whole call (the compiler copies in and back out where it
                # cannot bind the cells themselves; the generator keeps that unobservable)
                if isinstance(a, Var) and isinstance(self.lookup(a.name), Cell):
                    c = self.lookup(a.name)
                else:
                    c = Alias(*a.ref(self))
                if c.t != t:
                    raise ModelError("ref type")
                frame[pn] = c
            elif is_struct(t):
                frame[pn] = Cell(t, same_type(t, a.ev(self)))
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
        if is_struct(p.ret):
            return V(p.ret, False, same_type(p.ret, r))
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
    if not isinstance(v.v, int):
        raise ModelError("not a number")
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
        if not isinstance(c, SCALARS):
            raise ModelError("not a variable")
        return V(c.t, False, c.v)

    def ref(self, m):
        return VarL(self.name).ref(m)


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

    def ref(self, m):
        return IdxL(self.arr, self.i).ref(m)


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


class Sizeof(Expr):
    """sizeof(T): a constant (its size computed by the generator, which knows the structs)."""

    def __init__(self, t, size):
        self.t, self.size = t, size

    def emit(self):
        return f"sizeof({self.t})"

    def ev(self, m):
        return V(const_type(self.size), True, self.size)


class Path(Expr):
    """A place reached through fields and indices: from a variable or an array (how "v": s.f, a[i].f.d[2],
    p[i].f through a pointer), a pointer (how "a": p->f..., how "d": (*p)...) or a returned struct (how
    "c": mk(1).f...).  steps: field names and index expressions, in order.  As an expression its value is a
    number or a struct (a dict, copied by whatever stores it); as an lvalue it is (type, get, put)."""
    EF = ("steps", "call")

    def __init__(self, how, name, steps, call=None):
        self.how, self.name, self.steps, self.call = how, name, steps, call

    def emit(self):
        steps = self.steps
        if self.how == "a":
            s, steps = f"{self.name}->{steps[0]}", steps[1:]
        elif self.how == "d":
            s = f"(*{self.name})"
        elif self.how == "c":
            s = self.call.emit()
        else:
            s = self.name
        for x in steps:
            s += "." + x if isinstance(x, str) else f"[{x.emit()}]"
        return s

    @staticmethod
    def elem(arr, i):
        if not 0 <= i < len(arr.a):
            raise ModelError("index out of range")
        return arr.t, (lambda: arr.a[i]), (lambda v: arr.a.__setitem__(i, v))

    def locate(self, m):
        steps = self.steps
        if self.how == "c":
            if not isinstance(self.call, Call):
                raise ModelError("not a call")
            r = self.call.ev(m)
            loc = (r.t, lambda: r.v, None)
        else:
            obj = m.lookup(self.name)
            if self.how in ("a", "d"):
                if not isinstance(obj, Ptr) or (self.how == "a" and (not steps or not isinstance(steps[0], str))):
                    raise ModelError("not a pointer")
                loc = self.elem(m.lookup(obj.arr), obj.j)
            elif isinstance(obj, Ptr):
                if not steps or isinstance(steps[0], str):
                    raise ModelError("pointer without an index")
                loc = self.elem(m.lookup(obj.arr), obj.j + steps[0].ev(m).v)
                steps = steps[1:]
            elif isinstance(obj, Arr):
                loc = (("[]", obj.t, len(obj.a)), lambda: obj.a, None)
            elif isinstance(obj, SCALARS):
                loc = (obj.t, lambda: obj.v, lambda v: setattr(obj, "v", v))
            else:
                raise ModelError("not a place")
        for x in steps:
            t, get, _ = loc
            if isinstance(x, str):
                if not is_struct(t):
                    raise ModelError("not a struct")
                d = get()
                if x not in d:
                    raise ModelError("no such field")
                loc = (m.st[t].ftype[x], (lambda d=d, x=x: d[x]), (lambda v, d=d, x=x: d.__setitem__(x, v)))
            else:
                if not isinstance(t, tuple):
                    raise ModelError("not an array")
                lst, i = get(), x.ev(m).v
                if not 0 <= i < len(lst):
                    raise ModelError("index out of range")
                loc = (t[1], (lambda lst=lst, i=i: lst[i]), (lambda v, lst=lst, i=i: lst.__setitem__(i, v)))
        return loc

    def ev(self, m):
        t, get, _ = self.locate(m)
        if isinstance(t, tuple):
            raise ModelError("an array is not a value")
        return V(t, False, get())

    def ref(self, m):
        t, get, put = self.locate(m)
        if put is None or isinstance(t, tuple):
            raise ModelError("not assignable")
        return t, get, put


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
        if not isinstance(c, SCALARS):
            raise ModelError("not a variable")
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
        if is_struct(self.t):
            v = same_type(self.t, self.init.ev(m)) if self.init is not None else zero(self.t, m.st)
        else:
            v = wrap(self.init.ev(m).v, self.t) if self.init is not None else 0
        m.frame[self.name] = Cell(self.t, v)


class ArrDecl(Stmt):
    def __init__(self, t, name, n, heap):
        self.t, self.name, self.n, self.heap = t, name, n, heap

    def emit(self):
        return ("heap " if self.heap else "") + f"{self.t} {self.name}[{self.n}];"

    def ex(self, m):
        m.frame[self.name] = Arr(self.t, self.n, m.st)


class PtrDecl(Stmt):
    """T* name = &arr[j];  j a number or an expression"""
    EF = ("j",)

    def __init__(self, t, name, arr, j):
        self.t, self.name, self.arr, self.j = t, name, arr, j

    def emit(self):
        j = self.j.emit() if isinstance(self.j, Expr) else self.j
        return f"{self.t}* {self.name} = &{self.arr}[{j}];"

    def ex(self, m):
        a = m.lookup(self.arr)
        j = self.j.ev(m).v if isinstance(self.j, Expr) else self.j
        if not isinstance(a, Arr) or a.t != self.t or not 0 <= j < len(a.a):
            raise ModelError("bad pointer")
        m.frame[self.name] = Ptr(self.arr, j)


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
        if is_struct(t):
            if self.op != "=":
                raise ModelError("struct arithmetic")
            put(same_type(t, E))
            return
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
    """params: (type, ref, name) with ref False, True or "[]" (a ref array parameter, whose arguments have
    at least asize[name] elements).  A shared proc is compiled once and called through the trampoline."""
    SF = ("body",)

    def __init__(self, name, ret, params, body, rec, shared=False, asize=None):
        self.name, self.ret, self.params, self.body, self.rec = name, ret, params, body, rec
        self.shared, self.asize = shared, asize or {}
        self.hparams = set()             # struct array parameters that take only heap arrays (Gen.proc)

    def lines(self, ind, unshare=False):
        plist = ", ".join(("ref " if ref else "") + f"{t} {n}" + ("[]" if ref == "[]" else "") for t, ref, n in self.params)
        head = ("rec " if self.rec else "shared " if self.shared and not unshare else "") + "proc " + \
            (self.ret + " " if self.ret else "") + f"{self.name}({plist}) {{"
        return [ind + head] + lines(self.body, ind + "    ") + [ind + "}"]


class StructDecl(Node):
    """struct name { fields }: fields (name, type), a type being an integer type, an earlier struct or an
    array ("[]", element type, length) of either."""

    def __init__(self, name, fields):
        self.name, self.fields = name, fields
        self.ftype = dict(fields)

    def lines(self, ind):
        fs = " ".join(f"{t[1]} {f}[{t[2]}];" if isinstance(t, tuple) else f"{t} {f};" for f, t in self.fields)
        return [ind + f"struct {self.name} {{ {fs} }}"]


class Program(Node):
    SF = ("decls", "procs", "main", "dump")

    def __init__(self, decls, procs, main, dump, structs=()):
        self.decls, self.procs, self.main, self.dump = decls, procs, main, dump
        self.structs = list(structs)
        self.stack = None                # `stack N;` when the default recursion stack is too small (fit_stack)

    def text(self, unshare=False):
        """The source; unshare: with every shared proc an ordinary (inlined) one."""
        head = [f"stack {self.stack};"] if self.stack else []
        procs = [x for p in self.procs for x in p.lines("", unshare)]
        return "\n".join(head + lines(self.structs, "") + lines(self.decls, "") + procs + lines(self.main, "") +
                         lines(self.dump, "")) + "\n"


def lines(lst, ind):
    out = []
    for s in lst:
        out += s.lines(ind)
    return out


def model_run(prog, stdin, ran=None):
    """The program's output; ran, a set, gets the names of the procedures it called."""
    m = Model(prog, stdin)
    if ran is not None:
        m.ran = ran
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
    """What generated code may use here.  Scalars: targets (assignable) and reads; arrays of integers
    (name, type, length, heap) with heap False, True, "ptr" (a pointer: name, type, elements from it on) or
    "param" (a ref array parameter): warrs (writable) and arrs; struct roots (SR): swroots and sroots;
    pure and spure: procedures callable in expressions (spure return a struct); calls: callable as
    statements; tpass: arrays that may be passed to a trampolined procedure's array parameter; hptrs:
    heap arrays a local pointer may point into; ro: nothing outside the procedure may be written."""

    def __init__(self, **kw):
        self.__dict__.update(kw)

    def but(self, **kw):
        c = Cx(**self.__dict__)
        c.__dict__.update(kw)
        return c


class SR:
    """A struct root, where a struct place starts: kind "var" (a struct variable or parameter), "cells" (an
    array of structs in cells), "heap" (a heap array of structs), "ptr" (a pointer to a struct in a heap
    array, with n elements from it on), "param" (a ref array parameter).  root: the storage it stands for
    (for a pointer, the heap array), so that the ref arguments of one call never share one.  inheap: the
    structs are known to be in the heap, so an array field takes a runtime index (a heap array or a
    pointer into one, an array parameter that only ever gets heap arrays, a re-entrant procedure's local
    array)."""
    __slots__ = ("name", "t", "n", "kind", "root", "inheap")

    def __init__(self, name, t, n, kind, root=None, inheap=None):
        self.name, self.t, self.n, self.kind, self.root = name, t, n, kind, root or name
        self.inheap = kind in ("heap", "ptr") if inheap is None else inheap


def walk(node):
    """node and every node below it"""
    yield node
    for f in node.EF + node.SF:
        v = getattr(node, f)
        for x in (v if isinstance(v, list) else [v]):
            if isinstance(x, Node):
                yield from walk(x)


class Gen:
    def __init__(self, r, focus=None):
        self.r = r
        self.n = 0
        self.vt = {}            # variable name -> type
        self.consts = {}        # constant name -> V
        self.st = {}            # struct name -> StructDecl
        self.snames = []
        self.tmpl = {}          # struct name -> its places: (template, type)
        self.ptr_root = {}      # pointer name -> the heap array it points into
        self.procs = {}
        self.focus = focus
        self.p_struct = 0.95 if focus == "struct" else 0.6       # programs with structs
        self.p_shared = 0.7 if focus == "shared" else 0.2        # procs marked shared (not rec)
        self.p_arrparam = 0.2                                    # parameters that are ref arrays

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

    def leaf(self, cx, depth=2):
        r = self.r
        d0, depth = depth, max(depth, 2)
        k = r.random()
        if k < 0.5 and cx.reads:
            return Var(r.choice(cx.reads))
        if k < 0.6 and cx.arrs:
            return self.index_of(cx, r.choice(cx.arrs), depth)
        if k < 0.64 and cx.ptrs:
            return self.index_of(cx, r.choice(cx.ptrs), depth)
        if k < 0.68 and self.consts:
            return CRef(r.choice(list(self.consts)))
        if k < 0.77 and (cx.sroots or cx.spure):
            got = self.sleaf(cx, d0)
            if got is not None:
                return got[0]
        if k < 0.79:
            t = r.choice(TNAMES + self.snames)
            t = t + "*" if r.random() < 0.2 else t
            return Sizeof(t, tsize(t, self.st))
        if k < 0.84:
            return Un("-", self.lit())
        return self.lit()

    def index_expr(self, cx, n, depth, heap):
        """An index known to be in 0..n-1."""
        r = self.r
        if cx.counters and r.random() < 0.3:
            c = r.choice(cx.counters)
            return Bin("%", Var(c), Lit(n))
        if cx.idxvars and r.random() < 0.5:
            return Bin("%", Cast("byte", Var(r.choice(cx.idxvars))), Lit(n))
        if depth >= 4 or (n == 1 and r.random() < 0.5):
            return Lit(r.randrange(n))
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
        """The arguments of a call in an expression (to a pure procedure), or None."""
        args = []
        for i, (t, ref, pn) in enumerate(p.params):
            if i == 0 and p.rec:
                a = Bin("%", Cast("byte", self.expr(cx, depth + 1)), Lit(4))
            elif ref == "[]":
                a = self.array_arg(cx, p, t, p.asize[pn], False, set(), pn)
            elif is_struct(t):
                a = self.struct_expr(cx, t, depth + 1)
            else:
                a = self.fit(self.expr(cx, depth + 1), t)
            if a is None:
                return None
            args.append(a)
        return args

    def expr(self, cx, depth=0):
        r = self.r
        if depth >= 3 or r.random() < 0.22 + 0.12 * depth:
            return self.leaf(cx, depth)
        k = r.random()
        if k < 0.66:
            op = r.choice(OPS)
            a = self.expr(cx, depth + 1)
            b = self.count(cx, depth) if op in ("<<", ">>") else self.expr(cx, depth + 1)
            return Bin(op, a, b)
        if k < 0.71:
            kk = r.choice([0, 1, 4, 7, 8, 9, 12, 15, 16, 17, 20, 24, 31, 32, 33, 40, 48, 63, 64, 65, 100])
            a, b = self.expr(cx, depth + 1), self.expr(cx, depth + 1)
            if r.random() < 0.35:              # a constant factor (the compiler's own paths), either side
                c = self.lit()
                b = Un("-", c) if r.random() < 0.3 else c
                if r.random() < 0.5:
                    a, b = b, a
            return MulShr(a, b, kk)
        if k < 0.82:
            return Cast(r.choice(TNAMES), self.expr(cx, depth + 1))
        if k < 0.91:
            return Un(r.choice("-~!"), self.expr(cx, depth + 1))
        if cx.pure and depth < 2:
            p = r.choice(cx.pure)
            args = self.call_args(cx, p, depth)
            if args is not None:
                return Call(p.name, args)
        return self.leaf(cx, depth)

    def fit(self, e, t):
        """e stored in type t: a constant that does not fit gets a cast."""
        c = static_const(e, self.consts)
        if (c is not None and not assign_fits(c.v, t)) or self.r.random() < 0.15:
            return Cast(t, e)
        return e

    # structs
    def gen_structs(self):
        """One to three struct types; a later one may hold earlier ones and arrays of them."""
        r = self.r
        out = []
        for i in range(r.choice([1, 1, 2, 2, 3])):
            name = f"S{i}"
            earlier = [s.name for s in out]
            while True:
                fields = []
                for k in range(r.randint(1, 4)):
                    q = r.random()
                    if q < 0.45 or (q >= 0.65 and not earlier):
                        ft = r.choice(TNAMES)
                    elif q < 0.65:
                        ft = ("[]", r.choice(TNAMES), r.randint(1, 3))
                    elif q < 0.87:
                        ft = r.choice(earlier)
                    else:
                        ft = ("[]", r.choice(earlier), r.randint(1, 2))
                    fields.append((f"f{k}", ft))
                sd = StructDecl(name, fields)
                self.st[name] = sd
                if tsize(name, self.st) <= 20:
                    break
            out.append(sd)
            self.snames.append(name)
        return out

    def templates(self, t):
        """Every place inside a struct of type t: (steps, type), steps being ("f", field) and ("i", length)
        for an index into an array field; the struct itself has no steps."""
        if t not in self.tmpl:
            res = [([], t)]
            for f, ft in self.st[t].fields:
                pre, et = [("f", f)], ft
                if isinstance(ft, tuple):
                    pre, et = pre + [("i", ft[2])], ft[1]
                if is_struct(et):
                    res += [(pre + s, lt) for s, lt in self.templates(et)]
                else:
                    res.append((pre, et))
            self.tmpl[t] = res
        return self.tmpl[t]

    def inst(self, cx, tmpl, heap, depth):
        """The steps of a template: an index into an array field is a constant unless the struct is known
        to be in the heap (a struct held in cells needs one)."""
        out = []
        for k, v in tmpl:
            if k == "f":
                out.append(v)
            elif heap and self.r.random() < 0.6:
                out.append(self.index_expr(cx, v, depth + 1, True))
            else:
                out.append(Lit(self.r.randrange(v)))
        return out

    def root_path(self, cx, sr, tmpl, depth):
        r = self.r
        if sr.kind == "var":
            return Path("v", sr.name, self.inst(cx, tmpl, False, depth))
        heap = sr.inheap
        if sr.kind == "ptr":
            q = r.random()
            if tmpl and q < 0.5:
                return Path("a", sr.name, self.inst(cx, tmpl, True, depth))
            if q < 0.75:
                return Path("d", sr.name, self.inst(cx, tmpl, True, depth))
        first = self.index_expr(cx, sr.n, depth, sr.kind != "cells")
        return Path("v", sr.name, [first] + self.inst(cx, tmpl, heap, depth))

    def pick(self, roots, ok):
        """A root and one of its places whose type passes ok: (root, template, type), or None."""
        r = self.r
        roots = [sr for sr in roots if any(ok(lt) for _, lt in self.templates(sr.t))]
        if not roots:
            return None
        sr = r.choice(roots)
        s, lt = r.choice([(s, lt) for s, lt in self.templates(sr.t) if ok(lt)])
        return sr, s, lt

    def sleaf(self, cx, depth, write=False, t=None):
        """A number inside a struct (of type t if given): (place, type), or None."""
        r = self.r

        def ok(lt):
            return lt in TYPES and (t is None or lt == t)
        if not write and cx.spure and depth < 3 and r.random() < 0.25:
            p = r.choice(cx.spure)                   # a field of a returned struct: mk(1).f
            tm = [(s, lt) for s, lt in self.templates(p.ret) if ok(lt)]
            args = self.call_args(cx, p, depth) if tm else None
            if args is not None:
                s, lt = r.choice(tm)
                return Path("c", None, self.inst(cx, s, False, depth), Call(p.name, args)), lt
        got = self.pick(cx.swroots if write else cx.sroots, ok)
        if got is None:
            return None
        sr, s, lt = got
        return self.root_path(cx, sr, s, depth), lt

    def splace(self, cx, S, write, depth):
        """A place holding a struct of type S, or None."""
        got = self.pick(cx.swroots if write else cx.sroots, lambda lt: lt == S)
        return None if got is None else self.root_path(cx, got[0], got[1], depth)

    def struct_expr(self, cx, S, depth):
        """A value of struct type S: a place, a call, a struct inside a returned one; or None."""
        r = self.r
        q = r.random()
        if depth < 2 and cx.spure and q < 0.35:
            fns = [p for p in cx.spure if p.ret == S]
            if fns and q < 0.25:
                p = r.choice(fns)
                args = self.call_args(cx, p, depth)
                if args is not None:
                    return Call(p.name, args)
            else:
                p = r.choice(cx.spure)
                tm = [s for s, lt in self.templates(p.ret) if lt == S and s]
                args = self.call_args(cx, p, depth) if tm else None
                if args is not None:
                    return Path("c", None, self.inst(cx, r.choice(tm), False, depth), Call(p.name, args))
        return self.splace(cx, S, False, depth)

    def struct_value(self, cx, S, body):
        """struct_expr, or a fresh zero struct declared at the end of body when there is none."""
        e = self.struct_expr(cx, S, 1)
        if e is None:
            name = self.fresh("lz")
            self.vt[name] = S
            body.append(Decl(S, name, None))
            e = Var(name)
        return e

    def const_leaves(self, sr):
        """Every number in a struct root, by constant indices: (make place, type)."""
        prefixes = [[]] if sr.kind == "var" else [[i] for i in range(sr.n)]
        for pre in prefixes:
            for s, lt in self.templates(sr.t):
                if lt not in TYPES:
                    continue
                for idx in itertools.product(*[range(v) for k, v in s if k == "i"]):
                    it = iter(idx)
                    spec = pre + [v if k == "f" else next(it) for k, v in s]
                    yield (lambda spec=spec: Path("v", sr.name, [x if isinstance(x, str) else Lit(x) for x in spec])), lt

    # arguments of statement calls
    def clash(self, cx, root, used):
        """Can the storage root stand for shares one of the roots in used?  An array parameter may be bound
        to any global array of its element type, which an impure procedure also sees by name."""
        keys = {root}
        if root in cx.aparams:
            keys |= cx.garrays.get(cx.aparams[root], set())
        else:
            keys |= {pn for pn, t in cx.aparams.items() if root in cx.garrays.get(t, ())}
        return bool(keys & used)

    def array_arg(self, cx, callee, t, n, write, used, pn):
        """An array of element type t and at least n elements for the ref array parameter pn, or None.  A
        trampolined callee takes a pointer: a heap array, an array parameter, a local of a re-entrant
        procedure (tpass); a parameter that indexes array fields of its structs at run time (hparams) takes
        only arrays known to be in the heap (hpass)."""
        cands = [a[0] for a in (cx.warrs if write else cx.arrs) if a[3] != "ptr" and a[1] == t and a[2] >= n]
        cands += [sr.name for sr in (cx.swroots if write else cx.sroots) if sr.kind in ("cells", "heap", "param") and sr.t == t and sr.n >= n]
        cands = [c for c in cands if not self.clash(cx, c, used) and (c in cx.tpass if callee.tramp else
                                                                       c in cx.hpass if pn in callee.hparams else True)]
        if not cands:
            return None
        c = self.r.choice(cands)
        used.add(c)
        return Var(c)

    def ref_arg(self, cx, t, used):
        """A place of type t for a ref parameter: a variable, an array element, a place in a struct; or
        None.  No two ref arguments of a call share storage (used: the roots taken), since the compiler
        copies some in and back out and binds others to the cells themselves.  With idxvars (variables
        passed by ref in the same call) it prefers places, whose indices may read them."""
        r = self.r
        cands = []
        if not is_struct(t):
            cands += [(v, lambda v=v: Var(v)) for v in cx.targets if self.vt[v] == t and v not in used] * 2
            for a in cx.warrs:
                root = self.ptr_root.get(a[0], a[0])
                if a[1] == t and not self.clash(cx, root, used):
                    cands.append((root, lambda a=a: self.index_of(cx, a, 1)))
        for sr in cx.swroots:
            if not self.clash(cx, sr.root, used):
                tm = [s for s, lt in self.templates(sr.t) if lt == t]
                if tm:
                    cands.append((sr.root, lambda sr=sr, tm=tm: self.root_path(cx, sr, r.choice(tm), 1)))
        if not cands:
            return None
        root, mk = r.choice(cands if not cx.idxvars else [c for c in cands if c[0] not in cx.targets] * 4 + cands)
        used.add(root)
        return mk()

    def stmt_args(self, cx, p):
        """The arguments of a call statement, or None.  Variables for some integer ref parameters are
        chosen first, so that the index of a place passed to another ref parameter, before or after
        them, may read one of them: f(i, a[i]) where f writes i, whose place must stay where it was."""
        used, args = set(), [None] * len(p.params)
        byvar = []
        for i, (t, ref, pn) in enumerate(p.params):
            if ref is True and not is_struct(t) and self.r.random() < 0.5:
                vs = [v for v in cx.targets if self.vt[v] == t and v not in used]
                if vs:
                    args[i] = Var(self.r.choice(vs))
                    used.add(args[i].name)
                    byvar.append(args[i].name)
        for i, (t, ref, pn) in enumerate(p.params):
            if args[i] is not None:
                continue
            if ref == "[]":
                a = self.array_arg(cx, p, t, p.asize[pn], p.kind != "pure", used, pn)
            elif ref:
                a = self.ref_arg(cx.but(idxvars=byvar + [v for v in used if v in self.vt and v in cx.targets and v not in byvar]), t, used)
            elif i == 0 and p.rec:
                a = Bin("%", Cast("byte", self.expr(cx, 1)), Lit(4))
            elif is_struct(t):
                a = self.struct_expr(cx, t, 1)
            else:
                a = self.fit(self.expr(cx, 1), t)
            if a is None:
                return None
            args[i] = a
        return args

    def wide_op(self, cx, t, op, k):
        """a multiply, mulshr, division or remainder at type t, not of a constant"""
        a, b = Cast(t, self.expr(cx, 2)), Cast(t, self.expr(cx, 2))
        return MulShr(a, b, k) if op == "m" else Bin(op, a, b)

    def wide_expr(self, cx, t, op, k):
        """an expression at type t holding a few operations of one kind: a sum of them, or nested"""
        r = self.r
        e = self.wide_op(cx, t, op, k)
        for _ in range(r.choice([0, 1, 1, 2])):
            x = self.wide_op(cx, t, op, k)
            if r.random() < 0.25:
                x = r.choice([Un("-", x), Bin(">>", x, Lit(r.choice([1, 3, 8]))), Bin("*", x, Lit(3)), Cast(t, x)])
            e = Bin(r.choice("+-"), e, x) if r.random() < 0.8 else Bin(r.choice(["^", "&", "*", "/"]), e, x)
        if r.random() < 0.3:
            e = Bin(r.choice("+-"), e, Cast(t, self.expr(cx, 2)))
        if r.random() < 0.15:
            e = Un("-", e)
        return e

    def shared_run(self, cx):
        """A few assignments and declarations whose expressions use operations of one kind and width
        (the compiler runs them through one copy of the routine), sometimes with a plain statement
        between them; later ones read what earlier ones wrote.  Some variables get constants just before
        the run or between its statements, so the compiler knows their values: the run's statements read
        them outside the operations (which fold inside the loop) and now and then in an operation's factor
        (which ends the run before it)."""
        r = self.r
        t = r.choice(["word", "int", "dword", "long", "long"])
        op = r.choice(["*", "*", "*", "m", "m", "/", "%"])
        k = r.choice([0, 3, 8, 12, 16, 20, 31])
        res, known = [], []
        if cx.targets and r.random() < 0.4:
            for v in r.sample(cx.targets, min(len(cx.targets), r.choice([1, 1, 2]))):
                res.append(Assign(VarL(v), "=", self.fit(self.lit(), self.vt[v])))
                known.append(v)
        for _ in range(r.randint(1, 4)):
            if r.random() < 0.15 and cx.targets:
                v = r.choice(cx.targets)
                lit = r.random() < 0.4
                res.append(Assign(VarL(v), "=", self.fit(self.lit() if lit else self.expr(cx, 1), self.vt[v])))
                if lit:
                    known.append(v)
            mine = [v for v in cx.targets if self.vt[v] == t]
            arrs = [a for a in cx.warrs if a[1] == t]
            e = self.wide_expr(cx, t, op, k)
            if known and r.random() < 0.5:
                e = Bin(r.choice("+-^"), e, Cast(t, Var(r.choice(known))))
            if known and r.random() < 0.15:
                f = Cast(t, Var(r.choice(known)))
                x = Cast(t, self.expr(cx, 2))
                e = Bin(r.choice("+-"), e, MulShr(f, x, k) if op == "m" else Bin(op, f, x) if r.random() < 0.5 else Bin(op, x, f))
            q = r.random()
            if q < 0.35 or not mine:
                name = self.fresh("l")
                self.vt[name] = t
                res.append(Decl(t, name, self.fit(e, t)))
                cx.targets.append(name)
                cx.reads.append(name)
            elif q < 0.75:
                res.append(Assign(VarL(r.choice(mine)), "=", self.fit(e, t)))
            elif q < 0.9:
                res.append(Assign(VarL(r.choice(mine)), r.choice(["+=", "-="]), e))
            elif arrs and r.random() < 0.6:
                res.append(Assign(self.index_of(cx, r.choice(arrs), 1, lvalue=True), "=", self.fit(e, t)))
            elif (got := self.sleaf(cx, 1, write=True, t=t)) is not None:
                res.append(Assign(got[0], "=", self.fit(e, t)))
        return res

    def struct_stmt(self, cx):
        """A statement on structs: a field written, a whole struct copied, a struct declared."""
        r = self.r
        q = r.random()
        if q < 0.55:
            got = self.sleaf(cx, 1, write=True)
            if got is None:
                return []
            lv, t = got
            if q < 0.38:
                return [Assign(lv, "=", self.fit(self.expr(cx, 1), t))]
            if q < 0.45:
                return [IncDec(lv, r.choice(["++", "--"]))]
            op = r.choice(COMPOUND)
            return [Assign(lv, op, self.count(cx, 1) if op in ("<<=", ">>=") else self.expr(cx, 1))]
        if q < 0.85:
            types = sorted({lt for sr in cx.swroots for _, lt in self.templates(sr.t) if is_struct(lt)})
            if not types:
                return []
            S = r.choice(types)
            src = self.struct_expr(cx, S, 1)
            return [] if src is None else [Assign(self.splace(cx, S, True, 1), "=", src)]
        S = r.choice(self.snames)
        name = self.fresh("ls")
        self.vt[name] = S
        init = self.struct_expr(cx, S, 1) if r.random() < 0.6 else None
        sr = SR(name, S, 1, "var")
        cx.sroots.append(sr)
        cx.swroots.append(sr)
        return [Decl(S, name, init)]

    # statements
    def stmts(self, cx, n, depth):
        r = self.r
        res = []
        cx = cx.but(targets=list(cx.targets), reads=list(cx.reads), warrs=list(cx.warrs), arrs=list(cx.arrs), ptrs=list(cx.ptrs),
                    sroots=list(cx.sroots), swroots=list(cx.swroots))
        for _ in range(n):
            k = r.random()
            if k < 0.07:
                res += self.shared_run(cx)
                continue
            if k < 0.17 and self.snames and (cx.swroots or r.random() < 0.3):
                res += self.struct_stmt(cx)
                continue
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
                q = r.random()
                tramp = [c for c in cx.calls if c.tramp] if cx.in_loop else []
                tpure = [c for c in cx.pure if c.tramp] if cx.in_loop else []
                if q < (0.4 if tramp or tpure else 0.15):
                    t = r.choice(self.snames) if self.snames and r.random() < 0.3 else r.choice(TNAMES)
                    name = self.fresh("la")
                    size = r.randint(1, 3) if is_struct(t) else r.randint(2, 5)
                    res.append(ArrDecl(t, name, size, False))
                    if is_struct(t):
                        sr = SR(name, t, size, "cells")
                        cx.sroots.append(sr)
                        cx.swroots.append(sr)
                    else:
                        cx.warrs.append((name, t, size, False))
                        cx.arrs.append((name, t, size, False))
                        if cx.in_loop and r.random() < 0.6:
                            # read before written: a loop body's array starts at zero on every pass
                            # with a trampolined call in it or after it, so that in trampolined code the array
                            # is a frame's (the rest of a list that calls none is generated as plain code)
                            e = None
                            if tpure and r.random() < 0.6:
                                p = r.choice(tpure)
                                args = self.call_args(cx, p, 1)
                                e = Call(p.name, args) if args is not None else None
                            res.append(Assign(IdxL(name, Lit(r.randrange(size))), "+=", e or self.expr(cx, 1)))
                            for _ in range(3 if not e and tramp and r.random() < 0.8 else 0):
                                p = r.choice(tramp)
                                args = self.stmt_args(cx, p)
                                if args is not None:
                                    res.append(CallS(p.name, args))
                                    break
                elif q < (0.48 if tramp or tpure else 0.23) and cx.hptrs:
                    # a pointer into a heap array, at a constant or a computed element
                    arr, t, size = r.choice(cx.hptrs)
                    name = self.fresh("lp")
                    j = r.randrange(size) if r.random() < 0.5 else None
                    res.append(PtrDecl(t, name, arr, Lit(j) if j is not None else self.index_expr(cx, size, 1, True)))
                    self.ptr_root[name] = arr
                    avail = size - j if j is not None else 1
                    if is_struct(t):
                        sr = SR(name, t, avail, "ptr", arr)
                        cx.sroots.append(sr)
                        if not cx.ro:
                            cx.swroots.append(sr)
                    else:
                        cx.ptrs.append((name, t, avail, "ptr"))
                        if not cx.ro:
                            cx.warrs.append((name, t, avail, "ptr"))
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
                for _ in range(3):               # one whose arguments can be found here
                    p = r.choice(cx.calls + [q for q in cx.calls if q.kind == "ref"] * 2)
                    args = self.stmt_args(cx, p)
                    if args is not None:
                        res.append(CallS(p.name, args))
                        break
            elif k < 0.89 and cx.out_ok:
                e = self.expr(cx, 1)
                res.append(Print(e) if r.random() < 0.25 else Out([e]))
            elif k < 0.91 and cx.ret is not None:
                if cx.ret == "void":
                    res.append(Return(None))
                elif is_struct(cx.ret):
                    e = self.struct_expr(cx, cx.ret, 1)
                    if e is not None:
                        res.append(Return(e))
                else:
                    res.append(Return(self.fit(self.expr(cx, 1), cx.ret)))
        return res

    def proc(self, cx, i, procs):
        """One procedure; it may call the ones before it.  Kinds: pure (no effect outside itself, callable
        in expressions), impure (statements, may touch globals), ref (ref parameters and no globals, so that
        copying them in and out is the same as binding them)."""
        r = self.r
        name = f"p{i}"
        S = self.snames
        rec = r.random() < 0.25
        kind = r.choice(["pure", "pure", "impure", "ref"]) if not rec else r.choice(["pure", "impure"])

        def vtype():
            return r.choice(S) if S and r.random() < (0.35 if kind == "ref" else 0.2) else r.choice(TNAMES)
        if kind == "pure":
            ret = r.choice(S) if S and r.random() < 0.3 else r.choice(TNAMES)
        else:
            ret = r.choice(S) if S and r.random() < 0.1 else r.choice([None, None] + TNAMES)
        shared = not rec and r.random() < self.p_shared
        params, asize = [], {}
        if rec:
            params.append(("byte", False, f"{name}n"))
        for k in range(r.randint(0 if kind != "ref" else 1, 3 - rec)):
            pn = f"{name}a{k}"
            if kind == "ref" and k < 2 and r.random() < 0.6:
                # integer ref parameters, often two: f(i, a[i])
                params.append((params[0][0] if k and r.random() < 0.5 else r.choice(TNAMES), True, pn))
            elif r.random() < self.p_arrparam:
                params.append((r.choice(S) if S and r.random() < 0.5 else r.choice(TNAMES), "[]", pn))
                asize[pn] = r.randint(1, 3)
            else:
                params.append((vtype(), kind == "ref" and (k == 0 or r.random() < 0.4), pn))
        ro = kind == "pure"
        # a struct array parameter that may take only heap arrays, so that its array fields take runtime
        # indices: always when trampolined, sometimes when inlined
        hparams = {pn for t, ref, pn in params if ref == "[]" and is_struct(t) and (rec or shared or r.random() < 0.5)}
        reads, targets, arrs, warrs, sroots, swroots = [], [], [], [], [], []
        for t, ref, pn in params:
            self.vt[pn] = t
            if ref == "[]":
                if is_struct(t):
                    sr = SR(pn, t, asize[pn], "param", inheap=pn in hparams)
                    sroots.append(sr)
                    if not ro:
                        swroots.append(sr)
                else:
                    arrs.append((pn, t, asize[pn], "param"))
                    if not ro:
                        warrs.append((pn, t, asize[pn], "param"))
            elif is_struct(t):
                sr = SR(pn, t, 1, "var")          # a copy, or (ref) the caller's struct
                sroots.append(sr)
                swroots.append(sr)
            else:
                reads.append(pn)
                if not (rec and pn == f"{name}n"):
                    targets.append(pn)
        glob, imp = kind != "ref", kind == "impure"
        g = cx.globals_ if glob else []
        bcx = cx.but(targets=targets + (list(g) if imp else []), reads=reads + list(g),
                     warrs=warrs + (list(cx.gwarrs) if imp else []), arrs=arrs + (list(cx.garrs) if glob else []),
                     ptrs=list(cx.gptrs) if glob else [],
                     sroots=sroots + (list(cx.gsroots) if glob else []), swroots=swroots + (list(cx.gsroots) if imp else []),
                     tpass={pn for _, ref, pn in params if ref == "[]"} | (set(cx.gtpass) if glob else set()),
                     hpass={pn for _, ref, pn in params if ref == "[]" and (rec or shared or pn in hparams)} | (set(cx.gtpass) if glob else set()),
                     aparams={pn: t for t, ref, pn in params if ref == "[]"},
                     hptrs=list(cx.ghptrs) if glob else [],
                     pure=[p for p in procs if p.kind == "pure" and not is_struct(p.ret)] if glob else [],
                     spure=[p for p in procs if p.kind == "pure" and is_struct(p.ret)] if glob else [],
                     calls=list(procs) if imp else [p for p in procs if p.kind == "ref"] if kind == "ref" else [],
                     counters=[], in_loop=False, out_ok=not ro, ret=ret or "void", ro=ro)
        body = []
        for k in range(r.randint(0, 2)):
            ln = f"{name}l{k}"
            if S and r.random() < 0.2:
                t = r.choice(S)
                body.append(Decl(t, ln, self.struct_expr(bcx, t, 1) if r.random() < 0.5 else None))
                sr = SR(ln, t, 1, "var")
                bcx.sroots.append(sr)
                bcx.swroots.append(sr)
            else:
                t = r.choice(TNAMES)
                body.append(Decl(t, ln, self.fit(self.expr(bcx), t)))
                bcx.reads.append(ln)
                bcx.targets.append(ln)
            self.vt[ln] = t
        if r.random() < 0.3:
            t, an = vtype(), f"{name}arr"
            size = r.randint(1, 3) if is_struct(t) else r.randint(2, 4)
            body.append(ArrDecl(t, an, size, False))
            if is_struct(t):
                sr = SR(an, t, size, "cells", inheap=rec)
                bcx.sroots.append(sr)
                bcx.swroots.append(sr)
            else:
                bcx.arrs.append((an, t, size, False))
                bcx.warrs.append((an, t, size, False))
            if rec:
                bcx.tpass.add(an)               # a re-entrant procedure's local arrays live in the heap
                bcx.hpass.add(an)
        p = Proc(name, ret, params, body, rec, shared, asize)
        p.kind = kind
        p.tramp = rec or shared
        p.hparams = hparams
        p.bcx, p.top = bcx, len(body)           # where a call may be added later (call_unrun)
        body += self.stmts(bcx, r.randint(2, 5), 0)
        for sr in [sr for sr in bcx.swroots if sr.inheap and sr.kind in ("param", "cells") and sr.name.startswith(name)]:
            # an array field of a struct in the heap reached through a parameter or a re-entrant local, at
            # runtime indices: a[i].d[j] op= e
            tm = [(s, lt) for s, lt in self.templates(sr.t) if lt in TYPES and any(k == "i" for k, _ in s)]
            if tm and r.random() < 0.7:
                s, lt = r.choice(tm)
                steps = [self.index_expr(bcx, sr.n, 1, True)] + \
                        [v if k == "f" else self.index_expr(bcx, v, 2, True) for k, v in s]
                op = r.choice(["=", "=", "+=", "^="])
                body.insert(r.randint(p.top, len(body)), Assign(Path("v", sr.name, steps), op, self.fit(self.expr(bcx, 1), lt)))
        for v in [pn for t, ref, pn in params if ref is True and not is_struct(t)]:
            if r.random() < 0.6:
                # write a ref parameter, which the caller may also have used in the index of another argument
                w = IncDec(VarL(v), r.choice(["++", "--"])) if r.random() < 0.4 else Assign(VarL(v), "=", self.fit(self.expr(bcx, 1), self.vt[v]))
                body.insert(r.randint(p.top, len(body)), w)
        if rec:
            n = Var(f"{name}n")

            def again():
                """the arguments of a call to itself, which counts n down so that the recursion ends"""
                args, used = [Cast("byte", Bin("-", Var(n.name), Lit(1)))], set()
                for t, ref, pn in params[1:]:
                    if ref == "[]":
                        args.append(self.array_arg(bcx, p, t, asize[pn], False, used, pn))
                    elif is_struct(t):
                        args.append(self.struct_value(bcx, t, body))
                    else:
                        args.append(self.fit(self.expr(bcx, 2), t))
                if None in args:                 # every array parameter passed on, which the caller's arguments made safe
                    args = [Var(pn) if ref == "[]" else a for (t, ref, pn), a in zip(params, args)]
                return args
            if kind == "pure" and is_struct(ret):
                base = []
                base.append(Return(self.struct_value(bcx, ret, base)))
                body.append(If(Bin("==", n, Lit(0)), base, []))
                tv = self.fresh("rv")
                self.vt[tv] = ret
                body.append(Decl(ret, tv, Call(name, again())))
                sr = SR(tv, ret, 1, "var")
                got = self.sleaf(bcx.but(swroots=[sr]), 1, write=True)
                if got is not None and r.random() < 0.7:
                    body.append(Assign(got[0], "=", self.fit(self.expr(bcx, 1), got[1])))
                body.append(Return(Var(tv)))
            elif kind == "pure":
                base = Return(self.fit(self.expr(bcx, 1), ret))
                body.append(If(Bin("==", n, Lit(0)), [base], []))
                calls = [Call(name, again()) for _ in range(r.choice([1, 1, 2]))]
                e = calls[0]
                for c in calls[1:]:
                    e = Bin(r.choice(["+", "-", "^", "*"]), e, c)
                e = Bin(r.choice(OPS), e, self.expr(bcx, 2)) if r.random() < 0.6 else e
                body.append(Return(self.fit(e, ret)))
            else:
                then = []
                for _ in range(r.choice([1, 1, 2])):
                    then.append(CallS(name, again()))
                    then += self.stmts(bcx, r.randint(0, 2), 1)
                body.append(If(Bin("!=", n, Lit(0)), then, []))
                body += self.stmts(bcx, r.randint(0, 2), 1)
                if ret:
                    body.append(Return(self.struct_value(bcx, ret, body) if is_struct(ret) else self.fit(self.expr(bcx, 1), ret)))
        elif ret:
            body.append(Return(self.struct_value(bcx, ret, body) if is_struct(ret) else self.fit(self.expr(bcx, 1), ret)))
        p.tramp = p.tramp or any(isinstance(x, (Call, CallS)) and x.name in self.procs and self.procs[x.name].tramp for x in walk(p))
        self.procs[name] = p
        return p

    def call_unrun(self, prog, unrun, ran):
        """Add a call statement to each procedure in unrun, at the top level of the main program or of a
        later procedure that ran and may call it (an impure one, or a ref one calling a ref one, and not a
        trampolined callee from an inlined caller, which would trampoline the caller after its own
        callers chose their arguments).  In the main program, variables and arrays the arguments need are
        declared first when there are none (provide), and printed after the call.  Returns the (list,
        statement) pairs added."""
        r = self.r
        added = []
        for p in unrun:
            sites = [(prog.main, 0, None)]
            for q in prog.procs[prog.procs.index(p) + 1:]:
                if q.name in ran and (q.kind == "impure" or (q.kind == "ref" and p.kind == "ref")) and (q.tramp or not p.tramp):
                    sites.append((q.body, q.top, q.bcx))
            r.shuffle(sites)
            for body, top, cx in sites:
                pre, post = [], []
                if cx is None:
                    cx = self.mcx.but(targets=list(self.mcx.targets), reads=list(self.mcx.reads), warrs=list(self.mcx.warrs),
                                      arrs=list(self.mcx.arrs), sroots=list(self.mcx.sroots), swroots=list(self.mcx.swroots),
                                      tpass=set(self.mcx.tpass), hpass=set(self.mcx.hpass))
                    if r.random() < 0.8:
                        self.provide(cx, p, pre, post)
                args = self.stmt_args(cx, p)
                if args is None:
                    continue
                end = next((k for k in range(top, len(body)) if isinstance(body[k], Return)), len(body))
                at = r.randint(top, end)
                sts = pre + [CallS(p.name, args)] + post
                body[at:at] = sts
                added += [(body, st) for st in sts]
                break
        return added

    def provide(self, cx, p, pre, post):
        """Declarations in the main program (pre) that give each ref and array parameter of p an argument
        when cx has none, and statements printing them after the call (post)."""
        r = self.r
        for t, ref, pn in p.params:
            if ref == "[]":
                n = p.asize[pn]
                have = [a for a in cx.warrs if a[3] != "ptr" and a[1] == t and a[2] >= n] + \
                       [sr for sr in cx.swroots if sr.kind in ("cells", "heap") and sr.t == t and sr.n >= n]
                heap = p.tramp or pn in p.hparams or r.random() < 0.4
                if have and r.random() < 0.5:
                    continue
                name, size = self.fresh("ma"), n + r.randint(0, 1)
                pre.append(ArrDecl(t, name, size, heap))
                if heap:
                    cx.tpass.add(name)
                    cx.hpass.add(name)
                if is_struct(t):
                    sr = SR(name, t, size, "heap" if heap else "cells")
                    cx.sroots.append(sr)
                    cx.swroots.append(sr)
                    es = [Cast("byte", Bin(">>", mk(), Lit(8 * k))) for mk, lt in self.const_leaves(sr) for k in range(TYPES[lt][0] // 8)]
                else:
                    cx.arrs.append((name, t, size, heap))
                    cx.warrs.append((name, t, size, heap))
                    es = [Cast("byte", Bin(">>", Idx(name, Lit(i)), Lit(8 * k))) for i in range(size) for k in range(TYPES[t][0] // 8)]
                post += [Out(es[i:i + 16]) for i in range(0, len(es), 16)]
            elif ref and not is_struct(t):
                if r.random() < 0.5:             # an array of t, for a place: f(i, a[i])
                    heap, name, size = r.random() < 0.5, self.fresh("ma"), r.randint(2, 4)
                    pre.append(ArrDecl(t, name, size, heap))
                    cx.arrs.append((name, t, size, heap))
                    cx.warrs.append((name, t, size, heap))
                    es = [Cast("byte", Bin(">>", Idx(name, Lit(i)), Lit(8 * k))) for i in range(size) for k in range(TYPES[t][0] // 8)]
                    post += [Out(es[i:i + 16]) for i in range(0, len(es), 16)]
                if [v for v in cx.targets if self.vt[v] == t] and r.random() < 0.6:
                    continue
                name = self.fresh("mv")
                self.vt[name] = t
                pre.append(Decl(t, name, self.fit(self.expr(cx, 1), t)))
                cx.targets.append(name)
                cx.reads.append(name)
                post.append(Print(Var(name)))
            elif ref:
                if any(lt == t for sr in cx.swroots for _, lt in self.templates(sr.t)):
                    continue
                name = self.fresh("mv")
                self.vt[name] = t
                pre.append(Decl(t, name, None))
                sr = SR(name, t, 1, "var")
                cx.sroots.append(sr)
                cx.swroots.append(sr)
                es = [Cast("byte", Bin(">>", mk(), Lit(8 * k))) for mk, lt in self.const_leaves(sr) for k in range(TYPES[lt][0] // 8)]
                post += [Out(es[i:i + 16]) for i in range(0, len(es), 16)]

    def program(self):
        r = self.r
        structs = self.gen_structs() if r.random() < self.p_struct else []
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
            self.ptr_root[f"pt{i}"] = name
        # structs: a variable of each type (so a struct value is always at hand), arrays of them in cells
        # and in the heap, pointers into those; some fields start from the input
        gsr = []
        for S in self.snames:
            for _ in range(r.choice([1, 1, 2])):
                name = self.fresh("gs")
                self.vt[name] = S
                decls.append(Decl(S, name, None))
                gsr.append(SR(name, S, 1, "var"))
        if self.snames:
            for _ in range(r.choice([0, 1, 1])):
                S, size, name = r.choice(self.snames), r.randint(1, 3), self.fresh("gsa")
                decls.append(ArrDecl(S, name, size, False))
                gsr.append(SR(name, S, size, "cells"))
            hs = []
            for _ in range(r.choice([0, 1, 1, 2])):
                S, size, name = r.choice(self.snames), r.randint(1, 3), self.fresh("hs")
                decls.append(ArrDecl(S, name, size, True))
                hs.append(SR(name, S, size, "heap"))
            gsr += hs
            for _ in range(r.choice([0, 1, 1]) if hs else 0):
                h, name = r.choice(hs), self.fresh("sp")
                j = r.randrange(h.n)
                decls.append(PtrDecl(h.t, name, h.name, j))
                self.ptr_root[name] = h.name
                gsr.append(SR(name, h.t, h.n - j, "ptr", h.name))
            for sr in [s for s in gsr if s.kind != "ptr"]:
                leaves = list(self.const_leaves(sr))
                for mk, lt in r.sample(leaves, min(len(leaves), r.randint(0, 3))):
                    if r.random() < 0.5:
                        decls.append(Assign(mk(), "=", In()))
                        stdin.append(r.randint(0, 255))
                        if TYPES[lt][0] > 8 and r.random() < 0.6:
                            decls.append(Assign(mk(), r.choice(["*=", "-="]), self.lit()))
                    else:
                        decls.append(Assign(mk(), "=", self.fit(self.lit(), lt)))
        tpass = {a[0] for a in harrs} | {sr.name for sr in gsr if sr.kind == "heap"}
        hptrs = [a[:3] for a in harrs] + [(sr.name, sr.t, sr.n) for sr in gsr if sr.kind == "heap"]
        garrays = {}                     # element type -> the global arrays of it (an array parameter may be any)
        for name, t in [a[:2] for a in garrs + harrs] + [(sr.name, sr.t) for sr in gsr if sr.kind in ("cells", "heap")]:
            garrays.setdefault(t, set()).add(name)
        cx = Cx(globals_=g, garrs=garrs + harrs, gwarrs=garrs + harrs + gptrs, gptrs=gptrs, gsroots=gsr, gtpass=tpass,
                ghptrs=hptrs, sroots=[], swroots=[], spure=[], tpass=set(), hpass=set(), hptrs=[], ro=False, idxvars=[],
                aparams={}, garrays=garrays)
        procs = []
        for i in range(r.choice([2, 3, 3, 4]) if self.focus == "shared" else r.choice([0, 1, 2, 2, 3, 4])):
            procs.append(self.proc(cx, i, procs))
        mcx = cx.but(targets=list(g), reads=list(g), warrs=garrs + harrs + gptrs, arrs=garrs + harrs, ptrs=gptrs,
                     sroots=list(gsr), swroots=list(gsr), tpass=set(tpass), hpass=set(tpass), hptrs=hptrs,
                     pure=[p for p in procs if p.kind == "pure" and not is_struct(p.ret)],
                     spure=[p for p in procs if p.kind == "pure" and is_struct(p.ret)],
                     calls=procs, counters=[], in_loop=False, out_ok=True, ret=None)
        main = self.stmts(mcx, r.randint(4, 10), 0)
        tpure = [p for p in mcx.pure if p.tramp]
        if tpure and mcx.targets and r.random() < 0.3:
            # an array of the trampolined main program's frame declared in a loop body, read before it is
            # written: zero on every pass
            p = r.choice(tpure)
            args = self.call_args(mcx, p, 1)
            if args is not None:
                cv, la = self.fresh("c"), self.fresh("la")
                self.vt[cv] = "byte"
                t, size = r.choice(TNAMES), r.randint(2, 4)
                j = r.randrange(size)
                body = [ArrDecl(t, la, size, False), Assign(IdxL(la, Lit(j)), "+=", Call(p.name, args)),
                        Assign(VarL(r.choice(mcx.targets)), "+=", Idx(la, Lit(j)))]
                main.insert(r.randint(0, len(main)), For(cv, r.randint(2, 3), body))
        self.mcx = mcx
        dump = []
        for name in g:
            nb = TYPES[self.vt[name]][0] // 8
            dump.append(Out([Cast("byte", Bin(">>", Var(name), Lit(8 * k))) for k in range(nb)]))
        for name, t, size, _ in garrs + harrs:
            nb = TYPES[t][0] // 8
            dump.append(Out([Cast("byte", Bin(">>", Idx(name, Lit(i)), Lit(8 * k))) for i in range(size) for k in range(nb)]))
        for sr in gsr:
            if sr.kind != "ptr":
                es = [Cast("byte", Bin(">>", mk(), Lit(8 * k))) for mk, lt in self.const_leaves(sr) for k in range(TYPES[lt][0] // 8)]
                dump += [Out(es[i:i + 16]) for i in range(0, len(es), 16)]
        stdin += [r.randint(0, 255) for _ in range(4)]
        return Program(decls, procs, main, dump, structs), bytes(stdin)


def gen_bfl(seed, focus=None):
    """A program and its input for one seed; retries until the model can run it.  focus: "shared" or
    "struct" makes shared procedures or structs more frequent.  A procedure the program does not call
    gets a call added (Gen.call_unrun), in a few rounds, while the model can still run the program."""
    r = random.Random(seed if focus is None else f"{focus}{seed}")
    for _ in range(20):
        gen = Gen(r, focus)
        prog, stdin = gen.program()
        ran = set()
        try:
            expected = model_run(prog, stdin, ran)
        except ModelError:
            continue
        for _ in range(3):
            unrun = [p for p in prog.procs if p.name not in ran]
            added = gen.call_unrun(prog, unrun, ran) if unrun else []
            if not added:
                break
            more = stdin + bytes(r.randint(0, 255) for _ in range(4))
            ran2 = set()
            try:
                expected, stdin, ran = model_run(prog, more, ran2), more, ran2
            except ModelError:
                for lst, st in added:
                    lst.remove(st)
                break
        return prog, stdin, expected
    return None


# ---- the brainfuck generator and reference interpreter ----------------------------------------------
def pm(d):
    return "+" * d if d > 0 else "-" * -d


def bf_product(r):
    """A counted loop of copies (BFL's byte `*`: x[- y[- d+ t+] t[- y+]]) with their temporaries
    cleared first, which the optimiser runs as one product when no copy writes another's source."""
    cells = r.sample([o for o in range(-3, 9) if o], 6)
    copies = []
    for i in range(r.randint(1, 2)):
        s, d, t = cells[3 * i:3 * i + 3]
        if r.random() < 0.15:
            d = r.choice([0] + cells)                  # not a product
        copies.append((s, d, t, r.choice([1, 1, 2, -1, 3])))
    pos = 0
    out = ""

    def go(c):
        nonlocal pos, out
        out += ">" * (c - pos) if c > pos else "<" * (pos - c)
        pos = c
    for s, d, t, k in copies:
        go(t); out += "[-]"
    go(0)
    out += "["
    dec = r.randint(0, len(copies))
    for i in range(len(copies) + 1):
        if i == dec:
            go(0); out += r.choice(["-", "-", "-", "+", "--"])
        if i < len(copies):
            s, d, t, k = copies[i]
            go(s); out += "[-"; go(d); out += pm(k); go(t); out += "+"; go(s); out += "]"
            go(t); out += "[-"; go(s); out += "+"; go(t); out += "]"
    go(0)
    return out + "]"


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
        elif k < 0.66 and depth < 3:
            parts.append(bf_balanced(r, depth))
        elif k < 0.72:
            parts.append(bf_product(r))
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


def fit_stack(bf, d, prog, stdin):
    """Overflowing the recursion stack is not checked at run time: the program then writes over other cells
    and does anything, often spinning forever, which the model cannot predict.  For a program with rec procs,
    find out with --check-stack whether it fits, and give it a bigger stack when it does not.  False when
    even that overflows."""
    if not any(p.rec for p in prog.procs):
        return True
    src = os.path.join(d, "stack.bfl")
    while True:
        with open(src, "w", newline="\n") as f:
            f.write(prog.text())
        _, out, _ = run(bf, [src, "--check-stack", "--quiet-devices", "--eof", "nochange"], stdin)
        if b"recursion stack overflow" not in out:
            return True                  # fits, or fails some other way that check_bfl reports
        if prog.stack == STACK_BIG:
            return False
        prog.stack = STACK_BIG


STACK_BIG = 4096                         # also exercises the 16-bit stack pointer of stacks above 256 cells


def commands(path):
    return "".join(ch for ch in read(path).decode("latin-1") if ch in "+-<>.,[]")


def check_bfl(bf, d, text, stdin, expected, modes=MODES, pokes=None, determinism=True, layout=True, info=None, unshared=None):
    """Compile and run one BFL program; returns the failures.  pokes: (cell offset, value) pairs.
    info["shared"] is set when the program shares arithmetic: compiled with BFL_SHARE=0 it differs.
    unshared: the program with its shared procs inlined instead, which must behave the same."""
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
    if layout:                           # and without arithmetic shared within a block
        c = os.path.join(d, "p_noshare.b")
        rc, _, err = run(bf, ["--emit", c, src], env={"BFL_SHARE": "0"})
        if rc != 0:
            fails.append(Failure("noshare", "compile", err.strip()[:200]))
        elif commands(c) != commands(a):
            if info is not None:
                info["shared"] = True
            fails += [Failure("noshare", f.mode, f.detail) for f in check_bf_file(bf, c, stdin, "nochange", expected, modes[:1])]
    if unshared is not None:
        u = os.path.join(d, "p_unshared.bfl")
        with open(u, "w", newline="\n") as f:
            f.write(unshared)
        c = os.path.join(d, "p_unshared.b")
        rc, _, err = run(bf, ["--emit", c, u])
        if rc != 0:
            fails.append(Failure("unshare", "compile", err.strip()[:200]))
        else:
            fails += [Failure("unshare", f.mode, f.detail) for f in check_bf_file(bf, c, stdin, "nochange", expected, modes[:1])]
    if pokes is None:
        return fails
    base = commands(a)
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
        if not fit_stack(bf, d, p, stdin):
            return False
        res = check_bfl(bf, d, p.text(), stdin, expected, modes, pokes, "determinism" in kinds, "layout" in kinds,
                        unshared=p.text(unshare=True) if "unshare" in kinds else None)
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
    def __init__(self, kind, seed, status, fails=(), data=None, focus=None):
        self.kind, self.seed, self.status, self.fails, self.data = kind, seed, status, list(fails), data
        self.focus = focus
        self.shared = False              # a BFL program whose code shares arithmetic (check_bfl)
        self.procs_shared = False        # a BFL program with shared procs

    def replay(self):
        return f"python tests/fuzz.py --replay {self.kind} {self.seed}" + (f" --focus {self.focus}" if self.focus else "")


def unshared_text(prog):
    return prog.text(unshare=True) if any(p.shared for p in prog.procs) else None


def bfl_case(bf, seed, tmp, modes=MODES, pokes=True, focus=None):
    g = gen_bfl(seed, focus)
    if g is None:
        return Result("bfl", seed, "skip", focus=focus)
    prog, stdin, expected = g
    d = os.path.join(tmp, f"bfl{focus or ''}{seed}")
    os.makedirs(d, exist_ok=True)
    if not fit_stack(bf, d, prog, stdin):
        return Result("bfl", seed, "skip", focus=focus)
    info = {}
    fails = check_bfl(bf, d, prog.text(), stdin, expected, modes, poker(seed) if pokes else None, info=info,
                      unshared=unshared_text(prog))
    res = Result("bfl", seed, "fail" if fails else "ok", fails, (prog, stdin, expected), focus)
    res.shared = info.get("shared", False)
    res.procs_shared = unshared_text(prog) is not None
    return res


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


def run_cases(bf, bfl_n, bf_n, seed0, tmp, jobs=None, log=print, progress=False, focus=None):
    """Run bfl_n BFL and bf_n brainfuck programs from seed0 on; returns the results.  focus: for the BFL
    programs (gen_bfl)."""
    jobs = jobs or max(1, (os.cpu_count() or 2))
    work = [("bfl", seed0 + i) for i in range(bfl_n)] + [("bf", seed0 + i) for i in range(bf_n)]
    results = []
    lock = threading.Lock()
    t0 = last = time.time()
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {(ex.submit(bfl_case, bf, s, tmp, focus=focus) if k == "bfl" else ex.submit(bf_case, bf, s, tmp)): (k, s)
                for k, s in work}
        for f in concurrent.futures.as_completed(futs):
            try:
                res = f.result()
            except Exception as e:      # a bug in the fuzzer itself
                k, s = futs[f]
                tb = "".join(traceback.format_exception(type(e), e, e.__traceback__)[-3:]).strip()
                res = Result(k, s, "fail", [Failure("fuzzer", "", tb)], focus=focus if k == "bfl" else None)
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
    base = os.path.join(outdir, f"{res.kind}-{res.focus + '-' if res.focus else ''}{res.seed}")
    notes = [f"seed {res.seed}", res.replay()] + [str(f) for f in res.fails]
    if res.data is None:                 # the fuzzer itself failed: there is no program to write
        pass
    elif res.kind == "bfl":
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


def replay(bf, kind, seed, tmp, focus=None):
    if kind == "bfl":
        g = gen_bfl(seed, focus)
        if g is None:
            print("no program for this seed")
            return 0
        prog, stdin, expected = g
        d = os.path.join(tmp, "replay")
        os.makedirs(d, exist_ok=True)
        if not fit_stack(bf, d, prog, stdin):
            print(f"overflows even a stack of {STACK_BIG} cells")
        print(prog.text())
        print(f"stdin {stdin!r}\nexpected {expected!r}")
        fails = check_bfl(bf, d, prog.text(), stdin, expected, MODES, poker(seed), unshared=unshared_text(prog))
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
    ap.add_argument("--focus", choices=["shared", "struct"], help="BFL programs with more shared procs or structs")
    a = ap.parse_args()
    a.bf = os.path.abspath(a.bf)
    tmp = tempfile.mkdtemp(prefix="bffuzz")
    if a.replay:
        sys.exit(replay(a.bf, a.replay[0], int(a.replay[1]), tmp, a.focus))
    t0 = time.time()
    results = run_cases(a.bf, a.bfl, a.bf_n, a.seed, tmp, a.jobs, progress=True, focus=a.focus)
    failed = sorted((r for r in results if r.status == "fail"), key=lambda r: (r.kind, r.seed))
    for res in failed:
        save_failure(res, a.out, a.bf, tmp, not a.no_shrink, print)
    for kind in ("bfl", "bf"):
        rs = [r for r in results if r.kind == kind]
        if rs:
            print(f"{kind}: {sum(r.status != 'skip' for r in rs)} programs checked, "
                  f"{sum(r.status == 'skip' for r in rs)} skipped, {sum(r.status == 'fail' for r in rs)} failing"
                  + (f", {sum(r.shared for r in rs)} sharing arithmetic, {sum(r.procs_shared for r in rs)} with shared procs"
                     if kind == "bfl" else ""))
    print(f"{time.time() - t0:.0f} s")
    if not failed:
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
