"""Tests for the BFL compiler (src/lang).

Each program is compiled by bf.exe and run through the JIT, the interpreter and
the unoptimised interpreter; the output must match the expectation, which for
the arithmetic grids is computed in Python with the language's semantics
(unsigned, bytes wrap mod 256, words mod 65536).

    python tests\test_lang.py [path\to\bf.exe]
"""
import os
import subprocess
import sys
import tempfile

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
BF = ARGS[0] if ARGS else os.path.join(os.path.dirname(__file__), "..", "out", "bf.exe")
TMP = tempfile.mkdtemp(prefix="bfl")


EXTRA_FILES = {}      # name -> text, written next to t.bfl for the current case


def run(src, stdin=b"", flags=(), expect_rc=0):
    for name, text in EXTRA_FILES.items():
        os.makedirs(os.path.dirname(os.path.join(TMP, name)) or TMP, exist_ok=True)
        with open(os.path.join(TMP, name), "w", newline="\n") as f:
            f.write(text)
    path = os.path.join(TMP, "t.bfl")
    with open(path, "w", newline="\n") as f:
        f.write(src)
    r = subprocess.run([BF, path, "--quiet-devices"] + list(flags), input=stdin, capture_output=True, timeout=120)
    return r.returncode, r.stdout, r.stderr.decode("latin-1")


def check(name, src, expected, stdin=b"", extra=(), modes=(("jit", []), ("interp", ["--interp"]), ("noopt", ["--no-opt"]))):
    ok = True
    detail = []
    for label, flags in modes:
        rc, out, err = run(src, stdin, list(flags) + list(extra))
        if rc != 0 or out != expected:
            ok = False
            detail.append(f"   {label}: rc={rc} out={out!r} err={err.strip()!r}")
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        print("   expected:", repr(expected))
        for d in detail:
            print(d)
    return ok


def check_diags(name, src, fragments, flags=(), expect_rc=None):
    """Every fragment must appear on its own line of stderr, in order; no other diagnostic lines."""
    rc, out, err = run(src, b"", list(flags) + ["--mmio", "256"])
    lines = [l for l in err.splitlines() if ": " in l and l.count(":") >= 3]
    ok = len(lines) == len(fragments) and all(f in l for f, l in zip(fragments, lines))
    if expect_rc is not None:
        ok = ok and rc == expect_rc
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        print(f"   expected {fragments!r} rc={expect_rc}, got rc={rc}:")
        for l in err.splitlines():
            print("   | " + l)
    return ok


DIAGS = [
    # several errors in one compile, each reported once, in source order
    ("diag-several-parse-errors", "byte a = ;\nbyte b = 1 +;\nout 1\nbyte c;\n",
     ["1:10: unexpected ';' in expression", "2:13: unexpected ';' in expression", "4:1: expected ';' before 'byte'"], (), 1),
    ("diag-several-semantic-errors", "byte b = 300;\nc = 1;\nbreak;\nout HALT;\n",
     ["1:10: value 300 does not fit a byte", "2:1: 'c' is not declared", "3:1: 'break' outside a loop", "4:5: HALT is write-only"], (), 1),
    ("diag-error-recovery-in-block", "while (1) { byte x = ; out 1 2; }",
     ["1:22: unexpected ';' in expression", "1:30: expected ';' before '2'"], (), 1),
    ("warn-unused", "byte used = 1; byte unused = 2; proc f() { byte local; } f(); out used;",
     ["1:16: warning: variable 'unused' is never read", "1:44: warning: variable 'local' is never read"], ["--warn"], 0),
    ("warn-truncation", "word w = 300; byte b = w; out b;",
     ["1:24: warning: value of type 'word' is truncated to 'byte'"], ["--warn"], 0),
    ("warn-unreachable", "proc f() { return; out 1; } f(); byte i; for (i = 0; i < 2; i++) { break; out 2; }",
     ["1:20: warning: unreachable code", "1:75: warning: unreachable code"], ["--warn"], 0),
    ("warn-off-by-default", "byte unused = 2;", [], (), 0),
    ("warn-mixed-sign", "int a = 1; word b = 2; print a < b;",
     ["1:30: warning: comparing or combining signed 'int' with unsigned 'word' treats both as unsigned"], ["--warn"], 0),
    ("err-addr-of-cell", "byte x; byte* p = &x;", ["1:19: '&' needs something in the heap; variables held in cells have no address"], (), 1),
    ("err-deref-int", "byte x; out *x;", ["1:13: '*' needs a pointer, not 'byte'"], (), 1),
    ("err-heap-too-small", "heap 100; heap byte b[200];", ["1:1: heap arrays and string literals need 207 bytes of heap but the heap has 100; declare a larger one with 'heap N;'"], (), 1),
    ("err-pointer-int-assign", "heap byte b[2]; byte* p = 5;", ["1:27: cannot convert 'byte' to 'byte*' without a cast"], (), 1),
    ("err-struct-unknown-field", "struct Point { int x; int y; } Point p; p.z = 1;", ["1:42: 'Point' has no field 'z'"], (), 1),
    ("err-struct-compare", "struct Point { int x; int y; } Point a; Point b; out a == b;", ["1:56: invalid operands to '==' ('Point' and 'Point')"], (), 1),
    ("err-struct-self", "struct S { S inner; }", ["1:12: a struct cannot contain itself; use a pointer"], (), 1),
    ("err-dot-on-pointer", "struct Point { int x; int y; } heap Point h[1]; Point* p = h; p.x = 1;", ["1:64: 'Point*' is a pointer: use '->'"], (), 1),
    ("err-cell-struct-runtime-index", "struct B { byte d[3]; } B b; byte i = 1; b.d[i] = 2;", ["1:46: an array field of a struct held in cells needs a constant index; put the struct in the heap"], (), 1),
    ("err-import-missing", 'import "nope.bfl";', ["1:1: cannot find 'nope.bfl' next to this file or in the standard library"], (), 1),
    ("warn-library-quiet", 'import "strings.bfl"; import "math.bfl"; byte x = 1; out x;', [], ["--warn"], 0),
    ("err-int-range", "int a = 40000; sbyte s = 300;",
     ["1:9: value 40000 does not fit an int", "1:26: value 300 does not fit a sbyte"], (), 1),
    ("diag-global-after-call", "f(); byte g = 0; proc f() { g++; }",
     ["1:1: 'f' uses 'g', which is declared after this call"], (), 1),
    ("diag-lexical-scope", "proc f() { out x; } byte x = 1; f();",
     ["1:16: 'x' is not declared"], (), 1),
]


def check_error(name, src, fragment):
    rc, out, err = run(src)
    ok = rc == 1 and fragment in err
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        print(f"   expected a compile error containing {fragment!r}, got rc={rc} err={err.strip()!r}")
    return ok


# ---- arithmetic grid -------------------------------------------------------------------------
BYTE_OPS = ["+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^", "==", "!=", "<", "<=", ">", ">=", "&&", "||"]


def py_op(op, a, b, bits):
    m = (1 << bits) - 1
    if op == "+": return (a + b) & m
    if op == "-": return (a - b) & m
    if op == "*": return (a * b) & m
    if op == "/": return (a // b) & m if b else None
    if op == "%": return (a % b) & m if b else None
    if op == "<<": return (a << b) & m if b < 16 else 0
    if op == ">>": return (a >> b) & m if b < 16 else 0
    if op == "&": return a & b
    if op == "|": return a | b
    if op == "^": return a ^ b
    if op == "==": return int(a == b)
    if op == "!=": return int(a != b)
    if op == "<": return int(a < b)
    if op == "<=": return int(a <= b)
    if op == ">": return int(a > b)
    if op == ">=": return int(a >= b)
    if op == "&&": return int(bool(a) and bool(b))
    if op == "||": return int(bool(a) or bool(b))


def grid_program(pairs, bits, ops):
    """Variables are loaded through `in` so the compiler cannot fold anything."""
    typ = "byte" if bits == 8 else "word"
    lines = [f"{typ} a; {typ} b; {typ} r;"]
    expected = []
    for a, b in pairs:
        if bits == 8:
            lines.append(f"a = in; b = in;")
            stdin_part = bytes([a, b])
        else:
            lines.append(f"a = in; a = a * 256 + in; b = in; b = b * 256 + in;")
            stdin_part = bytes([a >> 8, a & 255, b >> 8, b & 255])
        for op in ops:
            if op in ("/", "%") and b == 0:
                continue
            if op in ("<<", ">>") and b > 16:
                continue
            lines.append(f'r = a {op} b; print r, " ";')
            expected.append(str(py_op(op, a, b, bits)) + " ")
        lines.append('out "\\n";')
        expected.append("\n")
        yield "\n".join(lines), stdin_part, "".join(expected)
        lines = [f"{typ} a; {typ} b; {typ} r;"]
        expected = []


def grid(name, pairs, bits, ops):
    src_all, stdin_all, exp_all = [], b"", ""
    first = True
    for src, stdin, exp in grid_program(pairs, bits, ops):
        if first:
            src_all.append(src)
            first = False
        else:
            src_all.append("\n".join(src.split("\n")[1:]))     # drop the declarations
        stdin_all += stdin
        exp_all += exp
    return check(name, "\n".join(src_all), exp_all.encode(), stdin_all)


# ---- typed grids (phase 2): every operator at every integer type ---------------------------
TYPES = {"byte": (8, False), "sbyte": (8, True), "word": (16, False), "int": (16, True), "dword": (32, False), "long": (32, True)}


def wrap(v, bits, signed):
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


def typed_op(op, a, b, bits, signed):
    """C semantics at one type: operands already wrapped; division truncates towards zero."""
    if op in ("==", "!=", "<", "<=", ">", ">="):
        return int({"==": a == b, "!=": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op])
    if op == "&&": return int(bool(a) and bool(b))
    if op == "||": return int(bool(a) or bool(b))
    if op == "/":
        if b == 0: return 0
        q = abs(a) // abs(b)
        return wrap(-q if (a < 0) != (b < 0) else q, bits, signed)
    if op == "%":
        if b == 0: return a
        q = abs(a) // abs(b)
        q = -q if (a < 0) != (b < 0) else q
        return wrap(a - q * b, bits, signed)
    if op == "<<": return wrap(a << b, bits, signed) if 0 <= b < bits else 0
    if op == ">>":
        if 0 <= b < bits: return wrap(a >> b, bits, signed)
        return -1 if (signed and a < 0) else 0
    r = {"+": a + b, "-": a - b, "*": a * b, "&": a & b, "|": a | b, "^": a ^ b}[op]
    return wrap(r, bits, signed)


def typed_grid(name, typ, values, ops, shift_counts=(0, 1, 3, 7, 15, 31)):
    """r = a op b at one type, operands read with `in` (big-endian bytes) so nothing folds."""
    bits, signed = TYPES[typ]
    nb = bits // 8
    lines = [f"{typ} a; {typ} b; {typ} r; byte k;"]
    stdin = b""
    exp = ""
    def load(var):
        if nb == 1:
            return f"{var} = in;"
        return f"{var} = in;" + "".join(f" {var} = {var} * 256 + in;" for _ in range(nb - 1))
    def raw(v):
        u = v & ((1 << bits) - 1)
        return bytes((u >> (8 * (nb - 1 - i))) & 255 for i in range(nb))
    for a in values:
        for b in values:
            lines.append(load("a") + " " + load("b"))
            stdin += raw(a) + raw(b)
            for op in ops:
                if op in ("<<", ">>"):
                    continue
                lines.append(f'r = a {op} b; print r, " ";')
                exp += str(typed_op(op, a, b, bits, signed)) + " "
            lines.append('out "\\n";')
            exp += "\n"
        for op in ("<<", ">>"):
            if op not in ops:
                continue
            for k in shift_counts:
                if k >= 32:
                    continue
                lines.append(f"k = in; r = a {op} k; print r, \" \";")
                stdin += bytes([k])
                exp += str(typed_op(op, a, k, bits, signed)) + " "
        lines.append('out "\\n";')
        exp += "\n"
    return check(name, "\n".join(lines), exp.encode(), stdin)


ALL_OPS = ["+", "-", "*", "/", "%", "&", "|", "^", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "<<", ">>"]
GRIDS = [
    ("grid-sbyte", "sbyte", [0, 1, -1, 7, -128, 100, -3]),
    ("grid-int", "int", [0, -1, 300, -300, 32767, -32768, -7]),
    ("grid-dword", "dword", [0, 1, 300, 65536, 4294967295, 123456789]),
    ("grid-long", "long", [0, -1, 300, -70000, 2147483647, -2147483648]),
]


CASES = []


def case(name, src, expected, stdin=b"", extra=(), files=None):
    CASES.append((name, src, expected, stdin, extra, files or {}))


case("hello", 'out "Hello\\n";', b"Hello\n")
# ---- allocator and strings (phase 5) ----
case("string-literal", 'string s = "hello"; print s, " ", s[1]; out s[4], 10; string t = "hello"; print t == s;', b"hello 101o\n1")
case("alloc-free", "byte* a = alloc(10); byte* b = alloc(20); a[0] = 'a'; b[19] = 'b'; out a[0], b[19]; print \" \", b - a; free(a); byte* c = alloc(8); print \" \", c == a, \" \", c[0];",
     b"ab 12 1 0")
case("new-struct", "struct Node { int val; Node* next; } Node* head = null; byte i; for (i = 1; i <= 5; i++) { Node* n = new Node; n->val = int(i) * 10; n->next = head; head = n; } int s = 0; while (head) { s += head->val; Node* d = head; head = head->next; free(d); } print s;",
     b"150")
case("new-array", "int* a = new int[20]; byte i; for (i = 0; i < 20; i++) { a[i] = int(i) * int(i); } print a[19], \" \", a[7]; long* b = new long; *b = -5; print \" \", *b;", b"361 49 -5")
case("alloc-exhaust", "heap 64; byte* p = alloc(40); byte* q = alloc(40); print p != null, q == null;", b"11")
case("alloc-reuse-split", "byte* a = alloc(30); free(a); byte* b = alloc(4); byte* c = alloc(4); print b == a, \" \", c - b;", b"1 6")
case("strings-lib", 'import "strings.bfl"; heap byte buf[40]; strcpy(buf, "brain"); strcat(buf, "fuck"); print buf, " ", strlen(buf), " ", strcmp(buf, "brainfuck"), " ", strcmp("abc", "abd") < 0, " ", strchr(buf, 102); string d = strdup(buf); d[0] = 66; print " ", d, " ", buf;',
     b"brainfuck 9 0 1 5 Brainfuck brainfuck")
case("strings-numbers", 'import "strings.bfl"; heap byte b[16]; print ltoa(-2147483648, b), " ", utoa(4000000000, b), " ", ltoa(0, b), " "; print atol("-12345") + 5, " ", atol("77x");',
     b"-2147483648 4000000000 0 -12340 77")

# ---- imports and the standard library (phase 6) ----
case("import-math", 'import "math.bfl"; print min(-5, 3), " ", max(-5, 3), " ", abs(-300), " ", clamp(500, 0, 255), " ", gcd(1071, 462), " ", isqrt(1000000), " ", isqrt(99), " ", pow(3, 13);',
     b"-5 3 300 255 21 1000 9 1594323")
case("import-io", 'import "io.bfl"; heap byte line[20]; word n = read_line(line, 20); print n, ":", line; long a = read_long(); long b = read_long(); print " ", a + b, " ", read_byte();',
     b"5:hello -42 0", b"hello\r\n 58 -100x")
case("import-memory", 'import "memory.bfl"; heap byte a[8]; heap byte b[8]; memset(a, 7, 8); memcpy(b, a, 6); print b[5], b[6], " ", memcmp(a, b, 8) > 0, " ", heap_unused() > 7000;',
     b"70 1 1")
case("import-relative", 'import "helper.bfl"; print twice(21), " ", LIMIT;', b"42 99",
     files={"helper.bfl": "const LIMIT = 99;\nproc byte twice(byte x) { return x * 2; }\n"})
case("import-once-cycle", 'import "a.bfl"; import "b.bfl"; print fa(), fb();', b"12",
     files={"a.bfl": 'import "b.bfl";\nproc byte fa() { return 1; }\n', "b.bfl": 'import "a.bfl";\nproc byte fb() { return 2; }\n'})
case("import-subdir", 'import "sub/x.bfl"; print fx();', b"7",
     files={"sub/x.bfl": 'import "y.bfl";\nproc byte fx() { return fy() + 1; }\n', "sub/y.bfl": "proc byte fy() { return 6; }\n"})

# ---- structs (phase 4) ----
case("struct-basics", "struct Point { int x; int y; } Point p; p.x = -3; p.y = 7; print p.x + p.y; Point q = p; q.x = 10; print \" \", p.x, \" \", q.x, \" \", q.y, \" \", sizeof(Point);",
     b"4 -3 10 7 4")
case("struct-nested", "struct Point { int x; int y; } struct Rect { Point a; Point b; byte color; } Rect r; r.a.x = 1; r.a.y = 2; r.b.x = 30; r.b.y = 40; r.color = 9; print (r.b.x - r.a.x) * (r.b.y - r.a.y), \" \", r.color, \" \", sizeof(Rect);",
     b"1102 9 9")
case("struct-array-cells", "struct Point { int x; int y; } Point pts[5]; byte i; for (i = 0; i < 5; i++) { pts[i].x = int(i) * 10; pts[i].y = -int(i); } int sx = 0; int sy = 0; for (i = 0; i < 5; i++) { sx += pts[i].x; sy += pts[i].y; } print sx, \" \", sy, \" \", pts[3].x;",
     b"100 -10 30")
case("struct-params-return", "struct Point { int x; int y; } proc Point mk(int x, int y) { Point p; p.x = x; p.y = y; return p; } proc int dot(Point a, Point b) { return a.x * b.x + a.y * b.y; } Point u = mk(3, -4); print dot(u, mk(2, 5)), \" \", mk(7, 8).y, \" \", u.y;",
     b"-14 8 -4")
case("struct-ref-param", "struct Point { int x; int y; } proc shift(ref Point p, int dx) { p.x += dx; p.y -= dx; } Point a; a.x = 1; a.y = 1; shift(a, 5); shift(a, 2); print a.x, \" \", a.y;", b"8 -6")
case("struct-swap-sort", "struct Point { int x; int y; } Point v[4]; v[0].x = 3; v[1].x = 1; v[2].x = 4; v[3].x = 2; byte i; byte j; for (i = 0; i < 4; i++) { v[i].y = int(i) * 100; } for (i = 0; i < 3; i++) { for (j = 0; j < 3 - i; j++) { if (v[j].x > v[j + 1].x) { Point t = v[j]; v[j] = v[j + 1]; v[j + 1] = t; } } } for (i = 0; i < 4; i++) { print v[i].x, \":\", v[i].y, \" \"; }",
     b"1:100 2:300 3:0 4:200 ")
case("struct-heap-list", "struct Node { int val; Node* next; } heap Node pool[6]; Node* head = null; byte i; for (i = 0; i < 6; i++) { Node* n = &pool[i]; n->val = int(i) * int(i) - 10; n->next = head; head = n; } int sum = 0; Node* p = head; while (p) { sum += p->val; p = p->next; } print sum, \" \", head->val, \" \", head->next->next->val, \" \", sizeof(Node);",
     b"-5 15 -1 4")
case("struct-array-field", "struct Buf { byte data[4]; byte len; } Buf b; b.data[0] = 'h'; b.data[3] = 'k'; b.len = 2; heap Buf hb[3]; byte i; for (i = 0; i < 4; i++) { hb[1].data[i] = 'a' + i; } out b.data[0], b.data[3], hb[1].data[2], '0' + b.len; print \" \", sizeof(Buf);",
     b"hkc2 5")
case("struct-heap-copy", "struct Point { int x; int y; } heap Point hp[2]; Point a; a.x = 11; a.y = -22; hp[1] = a; Point b = hp[1]; Point* pp = &hp[1]; pp->x += 1; print b.x, \" \", b.y, \" \", hp[1].x, \" \", (*pp).y;", b"11 -22 12 -22")
case("rec-struct-list", "struct Node { int val; Node* next; } heap Node pool[5]; rec proc int total(Node* p) { if (!p) { return 0; } return p->val + total(p->next); } byte i; for (i = 0; i < 5; i++) { pool[i].val = int(i) + 1; if (i < 4) { pool[i].next = &pool[i + 1]; } } print total(pool);",
     b"15")
case("rec-struct-local", "struct Point { int x; int y; } rec proc int f(byte n) { Point p; p.x = int(n); p.y = 1; if (n == 0) { return p.y; } return p.x + f(n - 1); } print f(5);", b"16")

case("wide-arrays-cells", "word w[5]; long l[3]; byte i; for (i = 0; i < 5; i++) { w[i] = word(i) * 1000; } for (i = 0; i < 3; i++) { l[i] = -long(i) * 100000; } i = 4; print w[i], \" \", w[2] + w[3], \" \", l[2], \" \", l[i - 3];",
     b"4000 5000 -200000 -100000")

# ---- heap and pointers (phase 3) ----
case("heap-array", "heap byte b[20]; byte i; for (i = 0; i < 20; i++) { b[i] = 'a' + i; } out b[0], b[5], b[19]; i = 7; out b[i], b[i + 3];", b"afthk")
case("heap-wide-array", "heap long big[8]; heap int n[5]; byte i; for (i = 0; i < 8; i++) { big[i] = long(i) * -100000; } for (i = 0; i < 5; i++) { n[i] = int(i) - 2; } print big[7], \" \", big[3] + n[0], \" \", n[4];",
     b"-700000 -300002 2")
case("pointer-basics", "heap byte b[10]; byte* p = &b[3]; *p = 'X'; p[1] = 'Y'; *(p + 2) = 'Z'; out b[3], b[4], b[5], *p, p[2]; p++; out *p; p -= 1; out *p;", b"XYZXZYX")
case("pointer-scaling", "heap int n[6]; int* q = n; byte i; for (i = 0; i < 6; i++) { *q = int(i) * 11; q++; } q = n + 2; print *q, \" \", q[3], \" \", q - n, \" \", *(n + 5);",
     b"22 55 2 55")
case("pointer-cast-word", "heap byte b[4]; b[0] = 0x34; b[1] = 0x12; word* w = (word*)b; print *w; *w = 0xABCD; print \" \", b[0], \" \", b[1];", b"4660 205 171")
case("pointer-null", "heap byte b[2]; byte* p = null; if (!p) { out 'n'; } p = b; if (p) { out 'p'; } if (p != null) { out 'q'; } if (p == b) { out 'r'; }", b"npqr")
case("pointer-params", "heap byte b[8]; proc fill(byte* d, byte n, byte v) { byte i; for (i = 0; i < n; i++) { d[i] = v + i; } } proc byte* at(byte* d, byte i) { return d + i; } fill(b, 8, 'a'); out *at(b, 6), b[0], b[7];",
     b"gah")
case("linked-list", "heap byte pool[30]; byte* head = null; byte i; for (i = 0; i < 5; i++) { byte* n = &pool[i * 3]; *n = 'a' + i; *(word*)(n + 1) = word(head); head = n; } byte* p = head; while (p) { out *p; p = (byte*)*(word*)(p + 1); }",
     b"edcba")
case("pointer-alias", "heap byte b[3]; byte* p = &b[1]; byte* q = b + 1; *p = 5; *q += 3; print b[1], \" \", *p;", b"8 8")
case("heap-top", "heap 40000; heap byte big[30000]; big[29999] = 77; big[0] = 1; word i = 29999; print big[i], \" \", big[0];", b"77 1")
case("heap-runtime-index-wide", "heap word w[300]; word i; for (i = 0; i < 300; i += 37) { w[i] = i * 3; } print w[296], \" \", w[37];", b"888 111")
case("heap-check-null", "byte* p = null; out 'a'; out *p; out 'b';", b"a\nheap access out of range\n", b"", ["--check-heap"])
# ---- integer types (phase 2) ----
case("int-print", "int a = -32768; int b = 32767; sbyte c = -128; long d = -2147483648; dword e = 4294967295; print a, \" \", b, \" \", c, \" \", d, \" \", e;",
     b"-32768 32767 -128 -2147483648 4294967295")
case("sign-extension", "sbyte s = -3; int i = s; long l = s; word w = s; dword d = i; print i, \" \", l, \" \", w, \" \", d;",
     b"-3 -3 65533 4294967293")
case("zero-extension", "byte b = 200; int i = b; long l = b; print i, \" \", l, \" \", int(b) - 300;", b"200 200 -100")
case("truncation", "long l = 70000; int i = l; byte b = l; sbyte s = l; print i, \" \", b, \" \", s;", b"4464 112 112")
case("mixed-widths", "int i = -5; byte b = 200; sbyte s = -3; word w = 10; print i + b, \" \", s + w, \" \", i * b, \" \", long(i) * 100000;",
     b"195 7 -1000 -500000")
case("signed-compare", "int a = -1; int b = 1; sbyte c = -100; print a < b, a > b, c < 0, c >= -100, long(a) < 0;", b"10111")
case("signed-division", "int a = -7; int b = 2; print a / b, \" \", a % b, \" \", 7 / -2, \" \", int(7) % int(-2), \" \", a / 0, \" \", a % 0;",
     b"-3 -1 -3 1 0 -7")
case("arith-shift", "int a = -64; sbyte s = -8; long l = -1; print a >> 2, \" \", s >> 1, \" \", l >> 31, \" \", word(a) >> 2;", b"-16 -4 -1 16368")
case("long-literal", "long a = 5L; dword b = 0xFFFFFFFF; long c = -2147483648; print a * 3, \" \", b, \" \", c + 1;", b"15 4294967295 -2147483647")
case("casts", "int i = -1; print word(i), \" \", byte(i), \" \", sbyte(255), \" \", dword(i), \" \", (long)(word)i;",
     b"65535 255 -1 4294967295 65535")
case("int-loop", "int i; long sum = 0; for (i = -3; i <= 3; i++) { sum += i * i * i; } print sum; int j = 10; while (j > -10) { j -= 7; } print \" \", j;", b"0 -11")
case("long-factorial", "long f = 1; byte n; for (n = 1; n <= 12; n++) { f = f * n; } print f;", b"479001600")
case("dword-divmod", "dword x = 4000000000; dword y = 7; print x / y, \" \", x % y, \" \", x / 65536, \" \", x >> 16;", b"571428571 3 61035 61035")
case("int-negate-not", "int a = 300; print -a, \" \", ~a, \" \", !a, \" \", -(-a);", b"-300 -301 0 300")
case("int-compound", "int a = 100; a -= 250; print a; a *= -3; print \" \", a; a /= 7; print \" \", a; a <<= 2; print \" \", a; a >>= 3; print \" \", a; a %= 10; print \" \", a;",
     b"-150 450 64 256 32 2")
case("wide-incdec", "int a = 0; a--; print a; long l = 65535; l++; print \" \", l; dword d = 0; d--; print \" \", d;", b"-1 65536 4294967295")
case("wide-params-return", "proc long sq(long x) { return x * x; } proc int neg(int v) { return -v; } print sq(-3000), \" \", neg(-200), \" \", sq(neg(5));",
     b"9000000 200 25")
case("rec-long", "stack 200; rec proc long fib(byte n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } print fib(12);", b"144")
case("const-fold", "const K = 3 * 7 + 1; byte x = K; out x + 43;", b"A")
case("byte-wrap", "byte x = 250; x += 10; out x + 61; x = x - 5; out x;", b"A\xff")
case("copy-keeps-source", "byte a = 65; byte b = a; byte c = a; out a, b, c;", b"AAA")
case("if-else", "byte x = in; if (x == 'y') { out \"yes\"; } else if (x == 'n') { out \"no\"; } else { out \"?\"; }", b"no", b"n")
case("while-countdown", "byte i = 5; while (i) { out '0' + i; i--; } out '\\n';", b"54321\n")
case("for-break-continue", "byte i; for (i = 0; i < 10; i++) { if (i == 2) { continue; } if (i == 7) { break; } out '0' + i; }", b"013456")
case("nested-break", "byte i; byte j; for (i = 0; i < 3; i++) { for (j = 0; j < 5; j++) { if (j == 2) { break; } out '0' + j; } out ','; }", b"01,01,01,")
case("break-in-block", "byte i = 0; while (1) { i++; { if (i == 4) { break; } } out '0' + i; } out '!';", b"123!")
case("while-1-break", "byte n = in; while (1) { if (n == 0) { break; } out n; n--; }", b"\x03\x02\x01", b"\x03")
case("logic-short-circuit", "byte a = 0; byte b = 1; if (a && in == 'z') { out 'X'; } if (b || in == 'z') { out 'Y'; } out in;", b"Yq", b"q")
case("not", "byte a = in; byte b = !a; byte c = !!a; print b, c; out !a + 'A';", b"01A", b"\x07")
case("neg-and-complement", "byte a = in; byte b = -a; byte c = ~a; print b, \" \", c;", b"253 252", b"\x03")
case("shift-var", "byte a = in; byte n = in; print a << n, \" \", a >> n;", b"200 3", b"\x19\x03")
case("mul-const", "byte a = in; print a * 10, \" \", 7 * a;", b"230 161", b"\x17")
case("div-zero", "byte a = in; print a / 0, \" \", a % 0;", b"0 23", b"\x17")   # defined as 0 and a
case("array-const-index", "byte a[4]; a[0] = 'a'; a[1] = a[0] + 1; a[3] = a[1] + 2; a[2] = a[3] - 1; out a[0], a[1], a[2], a[3];", b"abcd")
case("array-runtime", """
byte a[6]; byte i;
for (i = 0; i < 6; i++) { a[i] = 'A' + i * 2; }
for (i = 0; i < 6; i++) { out a[i]; }
i = in; out a[i]; out a[i - 1];
a[i] = 'z'; a[0] = a[i]; out a[0], a[1], a[2], a[3], a[4], a[5];
a[i]++; a[i] += 2; out a[i]; i = 1; a[i]--; out a[i];
""", b"ACEGIKGEzCEzIK}B", b"\x03")
case("array-ref-param", """
proc fill(ref byte buf[], byte n, byte v) { byte i; for (i = 0; i < n; i++) { buf[i] = v + i; } }
byte a[5]; fill(a, 5, 'a'); byte i; for (i = 0; i < 5; i++) { out a[i]; }
""", b"abcde")
case("proc-value-ref", """
proc swap(ref byte x, ref byte y) { byte t = x; x = y; y = t; }
proc show(byte x, byte y) { out x, y; x = 0; }
byte a = 'p'; byte b = 'q'; swap(a, b); show(a, b); out a;
""", b"qpq")
case("proc-nested-calls", """
proc inc(ref byte x) { x++; }
proc inc2(ref byte x) { inc(x); inc(x); }
byte v = 'a'; inc2(v); inc(v); out v;
""", b"d")
case("proc-locals-reset", """
proc f() { byte local; local++; out '0' + local; }
byte i; for (i = 0; i < 3; i++) { f(); }
""", b"111")
case("word-basics", "word w = 65535; w++; print w; w--; print \" \", w; w = 300; w = w * 200; print \" \", w;", b"0 65535 60000")
case("word-carry", "word w = 255; w += 1; print w; w += 255; print \" \", w; w -= 1; print \" \", w; w -= 255; print \" \", w;", b"256 511 510 255")
case("word-byte-mix", "byte b = in; word w = b * 300; print w; byte c = w; print \" \", c; w = w / b; print \" \", w;", b"1500 220 300", b"\x05")
case("word-cast", "byte a = 200; byte b = 100; word w = a + b; print w; w = word(a) + b; print \" \", w; print \" \", byte(w);", b"44 300 44")
case("word-compare", "word a = in; a = a * 256 + in; word b = 1000; print a < b, a > b, a == b, a != b, a <= b, a >= b;", b"001011", b"\x03\xe8")
case("word-shift-bitwise", "word a = 0x1234; print a << 1, \" \", a >> 4, \" \", a & 0x0FF0, \" \", a | 1, \" \", a ^ 0xFFFF;", b"9320 291 560 4661 60875")
case("input-decimal", "word w = input; byte b = input; print w * 2, \" \", b + 1;", b"24690 43", b"12345\n42\n")
case("print-byte-widths", "byte a = in; print a; out ' '; print a + 1; out ' '; print 0, \" \", 9, \" \", 10, \" \", 99, \" \", 100, \" \", 255;", b"7 8 0 9 10 99 100 255", b"\x07")
case("string-escapes", 'out "a\\tb\\x41\\\\\\"\\e[0m\\n";', b"a\tbA\\\"\x1b[0m\n")
case("in-eof-nochange", "byte c = 'x'; c = in; out c;", b"x")
case("in-loop-echo", "byte c = in; while (c != '.') { out c + 1; c = in; }", b"bcd", b"abc.")
case("asm-block", "byte x = 'A'; asm { ++ } out x;", b"C")
case("halt", "out 'a'; halt; out 'b';", b"a")
case("register-latch", "LEDS = 0x41; byte x = LEDS; out x; LEDS += 1; out LEDS; LEDS = LEDS + 1; out LEDS; LEDS++; out LEDS;", b"ABCD")
case("register-number", "NUMBER0 = 7; NUMBER1 = 3; byte x = NUMBER1 * 10 + NUMBER0; print x;", b"37")
case("register-status-test", """
byte c;
if (KEY_STATUS) { out 'K'; }                 // test without popping
while (KEY_STATUS) { pulse KEY_STATUS; c = KEY_DATA; out c + 1; }
if (KEY_STATUS) { out 'K'; } else { out 'E'; }
if (!KEY_STATUS) { out 'N'; }
""", b"KbcdEN", b"abc")
case("register-serial-loopback", """
SER_CTRL += 1;                      // open
SER_DATA = 'H'; pulse SER_TX;
SER_DATA = 'i'; pulse SER_TX;
while (SER_STATUS) { pulse SER_STATUS; out SER_DATA; }
SER_CTRL -= 1;
""", b"Hi", b"", ["--serial", "loopback"])
case("random-latch", "pulse ROLL; byte r = RANDOM; byte s = RANDOM; print r == s;", b"1")
case("ticks-latch", "pulse TICK_LATCH; byte t = TICKS3; print t;", b"0")
case("scoped-locals", "byte i; for (i = 0; i < 3; i++) { byte t; t += 5; print t; }", b"555")
case("compound-ops", "byte x = in; x *= 3; print x; x /= 4; print \" \", x; x %= 5; print \" \", x; x <<= 3; print \" \", x; x |= 3; print \" \", x; x &= 0x1E; print \" \", x; x ^= 0xFF; print \" \", x; x >>= 2; print \" \", x;", b"33 8 3 24 27 26 229 57", b"\x0b")
case("word-compound", "word w = 1000; w *= 3; print w; w /= 7; print \" \", w; w %= 100; print \" \", w; w <<= 4; print \" \", w; w -= 1; print \" \", w;", b"3000 428 28 448 447")
case("self-assign", "byte x = in; x = x; out x; x = x + x; out x; word w = 300; w = w + w; print w; w = w * 2 - w; print \" \", w;", b"\x05\x0a600 600", b"\x05")
case("deep-expr", "byte a = in; byte b = in; byte r = (a + b) * (a - b) / (a % b + 1) - !(a == b) + (a > b) * 7; print r;", b"26", b"\x07\x03")
case("else-if-chain", "byte x = in; if (x < 3) { out 'a'; } else if (x < 6) { out 'b'; } else if (x < 9) { out 'c'; } else { out 'd'; }", b"c", b"\x07")
case("while-register-loop", "byte n = 0; while (KEY_STATUS) { pulse KEY_STATUS; n++; } print n;", b"4", b"abcd")

# ---- procedures with return values (inlined) ----
case("proc-return-value", "proc byte sq(byte x) { return x * x; } proc word big(byte a) { return word(a) * 300; } print sq(7), \" \", big(3), \" \", sq(2) + sq(3);", b"49 900 13")
case("proc-return-early", "proc byte firstdiv(byte n) { byte d; for (d = 2; d < n; d++) { if (n % d == 0) { return d; } } return n; } print firstdiv(91), \" \", firstdiv(97), \" \", firstdiv(8);", b"7 97 2")
case("proc-return-nested-loops", "proc byte find(byte t) { byte i; byte j; for (i = 0; i < 5; i++) { for (j = 0; j < 5; j++) { if (i * j == t) { return i * 10 + j; } } } return 255; } print find(12), \" \", find(6), \" \", find(11);", b"34 23 255")
case("main-return", "out 'a'; byte i; for (i = 0; i < 9; i++) { if (i == 3) { return; } out '0' + i; } out 'z';", b"a012")
case("proc-call-stmt-discard", "byte n = 0; proc byte bump() { n++; return n; } bump(); bump(); print bump();", b"3")
# ---- recursion (trampoline) ----
case("rec-fib", "rec proc byte fib(byte n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } byte i; for (i = 0; i < 13; i++) { print fib(i), \" \"; }", b"0 1 1 2 3 5 8 13 21 34 55 89 144 ")
case("rec-fact-word", "rec proc word fact(byte n) { if (n == 0) { return 1; } return fact(n - 1) * n; } print fact(8), \" \", fact(0), \" \", fact(5);", b"40320 1 120")
case("rec-mutual", "rec proc byte odd(byte n) { if (n == 0) { return 0; } return even(n - 1); } rec proc byte even(byte n) { if (n == 0) { return 1; } return odd(n - 1); } print even(10), odd(10), even(7), odd(7);", b"1001")
case("rec-call-in-while", "rec proc byte sum(byte n) { byte s = 0; while (n) { s += sum(n - 1) + 1; n--; } return s; } print sum(0), \" \", sum(1), \" \", sum(4);", b"0 1 15")
case("rec-call-in-for-and-if", "rec proc byte count(byte d) { byte c = 1; byte i; if (d == 0) { return 1; } for (i = 0; i < 2; i++) { if (i == 1) { c += count(d - 1); } else { c += count(d - 1) * 2; } } return c; } print count(0), \" \", count(1), \" \", count(3);", b"1 4 40")
case("rec-two-calls-expr", "rec proc byte f(byte n) { if (n == 0) { return 1; } return f(n - 1) * 2 + f(n - 1); } print f(0), \" \", f(1), \" \", f(3);", b"1 3 27")
case("rec-ref-copy-out", "rec proc bump(ref byte x, byte n) { if (n == 0) { return; } x++; bump(x, n - 1); } byte v = 5; bump(v, 4); print v;", b"9")
case("rec-void-proc-output", "rec proc hanoi(byte n, byte a, byte b, byte c) { if (n == 0) { return; } hanoi(n - 1, a, c, b); out a, b, ' '; hanoi(n - 1, c, b, a); } hanoi(3, 'A', 'C', 'B');", b"AC AB CB AC BA BC AC ")
case("rec-ackermann", "rec proc byte ack(byte m, byte n) { if (m == 0) { return n + 1; } if (n == 0) { return ack(m - 1, 1); } return ack(m - 1, ack(m, n - 1)); } print ack(1, 2), \" \", ack(2, 2), \" \", ack(2, 3);", b"4 7 9")
case("rec-tree-array", "byte tree[15]; byte i; for (i = 0; i < 15; i++) { tree[i] = i + 1; } rec proc word sumtree(byte k) { if (k >= 15) { return 0; } return word(tree[k]) + sumtree(k * 2 + 1) + sumtree(k * 2 + 2); } print sumtree(0);", b"120")
case("rec-promoted-caller", "rec proc byte fib(byte n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } proc show(byte k) { print \"fib \", k, \" = \", fib(k), \"\\n\"; } show(5); show(10);", b"fib 5 = 5\nfib 10 = 55\n")
case("rec-break-continue-in-lowered-loop", "rec proc byte f(byte n) { return n; } byte i; byte t = 0; for (i = 0; i < 10; i++) { if (i == 2) { continue; } if (i == 6) { break; } t += f(i); } print t;", b"13")
case("rec-globals-and-locals", "byte g = 0; rec proc word depth(byte n) { byte local = n * 2; g++; if (n == 0) { return local; } return depth(n - 1) + local; } print depth(5), \" \", g;", b"30 6")
case("rec-word-param", "rec proc word pw(word b, byte e) { if (e == 0) { return 1; } return pw(b, e - 1) * b; } print pw(3, 7), \" \", pw(2, 10);", b"2187 1024")
case("rec-stack-size", "stack 32; rec proc byte d(byte n) { if (n == 0) { return 0; } return d(n - 1) + 1; } print d(6);", b"6")
case("rec-print-inside", "rec proc walk(byte n) { if (n == 0) { return; } print n, \" \"; walk(n - 1); print n, \" \"; } walk(3);", b"3 2 1 1 2 3 ")


def golden(update):
    """programs/*.bfl must compile byte-for-byte to tests/golden/<name>.b
    (python tests	est_lang.py --update-golden regenerates them after a deliberate codegen change)."""
    root = os.path.join(os.path.dirname(__file__), "..")
    ok = True
    for name in sorted(os.listdir(os.path.join(root, "programs"))):
        if not name.endswith(".bfl"):
            continue
        out = os.path.join(TMP, name[:-4] + ".b")
        r = subprocess.run([BF, "--emit", out, os.path.join(root, "programs", name)], capture_output=True, timeout=60)
        gold = os.path.join(root, "tests", "golden", name[:-4] + ".b")
        if r.returncode != 0:
            print(f"FAIL golden-{name}: {r.stderr.decode('latin-1').strip()}")
            ok = False
            continue
        got = open(out, "rb").read().replace(b"\r\n", b"\n")
        if update:
            open(gold, "wb").write(got)
            print(f"UPDATED {gold}")
            continue
        want = open(gold, "rb").read().replace(b"\r\n", b"\n") if os.path.exists(gold) else None
        same = got == want
        print(("PASS " if same else "FAIL ") + f"golden-{name}")
        if not same:
            print("   generated code differs from the golden file; run with --update-golden if the change is intended")
            ok = False
    return ok


def main():
    update = "--update-golden" in sys.argv
    fails = 0
    if not golden(update):
        fails += 1
    for name, src, expected, stdin, extra, files in CASES:
        EXTRA_FILES.clear(); EXTRA_FILES.update(files)
        if not check(name, src, expected, stdin, extra):
            fails += 1
    pairs8 = [(0, 0), (1, 0), (0, 1), (7, 3), (3, 7), (255, 1), (1, 255), (200, 100), (100, 200), (128, 128), (255, 255), (16, 4), (85, 170), (250, 7)]
    if not grid("byte-grid", pairs8, 8, BYTE_OPS):
        fails += 1
    pairs16 = [(0, 0), (1, 0), (300, 7), (7, 300), (65535, 1), (1000, 1000), (12345, 678), (65535, 65535), (256, 255), (4660, 3)]
    if not grid("word-grid", pairs16, 16, BYTE_OPS):
        fails += 1
    for gname, typ, vals in GRIDS:
        if not typed_grid(gname, typ, vals, ALL_OPS):
            fails += 1
    errors = [
        ("err-undeclared", "x = 1;", "not declared"),
        ("err-redeclared", "byte x; byte x;", "already declared"),
        ("err-byte-range", "byte x = 300;", "does not fit a byte"),
        ("err-write-only-read", "byte x = HALT;", "write-only"),
        ("err-status-assign", "KEY_STATUS = 1;", "pulse KEY_STATUS"),
        ("err-recursion", "proc f() { f(); } f();", "declare it 'rec proc'"),
        ("err-rec-array-local", "rec proc f(byte n) { byte a[3]; } f(1);", "cannot be local to a rec proc"),
        ("err-rec-array-param", "rec proc f(ref byte a[]) { } byte b[2]; f(b);", "array parameters"),
        ("err-return-value-void", "proc f() { return 1; } f();", "returns nothing"),
        ("err-return-missing-value", "proc byte f() { return; } byte x = f();", "returns a value"),
        ("err-void-in-expr", "proc f() { } byte x = f();", "cannot be used in an expression"),
        ("err-rec-duplicate-local", "rec proc byte f(byte n) { return n; } byte x = f(1); if (x) { byte x = f(2); }", "declared twice"),
        ("err-break-outside", "break;", "outside a loop"),
        ("err-index-range", "byte a[3]; a[3] = 1;", "outside"),
        ("err-arg-count", "proc f(byte a) { } f(1, 2);", "takes 1 argument"),
        ("err-ref-needs-var", "proc f(ref byte a) { } f(1);", "must be a variable"),
        ("err-syntax", "byte x = ;", "expression"),
        ("err-unterminated-string", 'out "abc;', "unterminated string"),
        ("err-string-in-expr", 'byte x = "a";', "cannot convert 'byte*' to 'byte'"),
        ("err-pulse-var", "byte x; pulse x;", "not a register"),
    ]
    for name, src, frag in errors:
        if not check_error(name, src, frag):
            fails += 1
    EXTRA_FILES.clear()
    for name, src, frags, flags, rc in DIAGS:
        if not check_diags(name, src.replace("\\n", "\n"), frags, flags, rc):
            fails += 1
    total = len(CASES) + 3 + len(errors) + len(DIAGS) + len(GRIDS)
    print(f"{total - fails}/{total} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
