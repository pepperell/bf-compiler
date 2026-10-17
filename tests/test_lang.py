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


def run(src, stdin=b"", flags=(), expect_rc=0):
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


CASES = []


def case(name, src, expected, stdin=b"", extra=()):
    CASES.append((name, src, expected, stdin, extra))


case("hello", 'out "Hello\\n";', b"Hello\n")
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
    for name, src, expected, stdin, extra in CASES:
        if not check(name, src, expected, stdin, extra):
            fails += 1
    pairs8 = [(0, 0), (1, 0), (0, 1), (7, 3), (3, 7), (255, 1), (1, 255), (200, 100), (100, 200), (128, 128), (255, 255), (16, 4), (85, 170), (250, 7)]
    if not grid("byte-grid", pairs8, 8, BYTE_OPS):
        fails += 1
    pairs16 = [(0, 0), (1, 0), (300, 7), (7, 300), (65535, 1), (1000, 1000), (12345, 678), (65535, 65535), (256, 255), (4660, 3)]
    if not grid("word-grid", pairs16, 16, BYTE_OPS):
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
        ("err-string-in-expr", 'byte x = "a";', "string"),
        ("err-word-array", "word w[3];", "word arrays"),
        ("err-pulse-var", "byte x; pulse x;", "not a register"),
    ]
    for name, src, frag in errors:
        if not check_error(name, src, frag):
            fails += 1
    total = len(CASES) + 3 + len(errors)
    print(f"{total - fails}/{total} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
