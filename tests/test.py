"""Correctness tests for out\\bf.exe.

Every case is run four ways: a naive Python reference interpreter, the JIT,
the IR interpreter, and the JIT with optimisation disabled.  Cases that touch
the device window skip the Python reference (it has no devices).

    python tests\test.py [path\to\bf.exe]
"""
import subprocess
import sys
import os

BF = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "out", "bf.exe")


def reference(src, stdin=b"", eof="nochange", limit=50_000_000):
    """Plain 8-bit brainfuck, pointer starts at 0 on an unbounded-right tape."""
    code = [c for c in src if c in "+-<>.,[]"]
    jump = {}
    stack = []
    for i, c in enumerate(code):
        if c == "[":
            stack.append(i)
        elif c == "]":
            j = stack.pop()
            jump[i] = j
            jump[j] = i
    tape = bytearray(65536)
    p = 0
    pc = 0
    out = bytearray()
    inp = 0
    steps = 0
    while pc < len(code):
        steps += 1
        if steps > limit:
            raise RuntimeError("reference interpreter step limit")
        c = code[pc]
        if c == "+":
            tape[p] = (tape[p] + 1) & 255
        elif c == "-":
            tape[p] = (tape[p] - 1) & 255
        elif c == ">":
            p += 1
        elif c == "<":
            p -= 1
            assert p >= 0, "reference: pointer went negative"
        elif c == ".":
            out.append(tape[p])
        elif c == ",":
            if inp < len(stdin):
                tape[p] = stdin[inp]
                inp += 1
            elif eof == "0":
                tape[p] = 0
            elif eof == "255":
                tape[p] = 255
        elif c == "[" and tape[p] == 0:
            pc = jump[pc]
        elif c == "]" and tape[p] != 0:
            pc = jump[pc]
        pc += 1
    return bytes(out)


def run_bf(src, stdin, eof, flags):
    r = subprocess.run([BF, "-e", src, "--eof", eof] + flags, input=stdin, capture_output=True, timeout=60)
    return r.returncode, r.stdout, r.stderr.decode("latin-1")


CASES = [
    # name, source, stdin, eof, expected stdout (None = use reference), uses_devices
    ("hello", open(os.path.join(os.path.dirname(__file__), "..", "programs", "hello.b")).read(), b"", "nochange", None, False),
    ("echo-eof0", ",[.,]", b"abc\n", "0", None, False),
    ("echo-eof255", ",+[-.,+]", b"xyz", "255", None, False),
    ("wrap", "-.", b"", "nochange", None, False),
    ("mul-simple", "++++[>++++<-]>[>++++<-]>.", b"", "nochange", None, False),
    ("mul-odd-dec", "++++++[>+++<---]>.", b"", "nochange", None, False),
    ("mul-odd-inc", "++++[>+<+]>.", b"", "nochange", None, False),
    ("mul-neg", "+++++[>-<-]>.", b"", "nochange", None, False),
    ("mul-chain", "++[>+++[>++++<-]<-]>>.", b"", "nochange", None, False),
    ("set-in-loop", "+++[>[-]+++<-]>.", b"", "nochange", None, False),
    ("set-in-loop-zero", "[>[-]+++<-]>.", b"", "nochange", None, False),
    ("if-once", ">+++<+[>[-]++<[-]]>.", b"", "nochange", None, False),
    ("scan-left", "+>+>+>+>>+[<]>.", b"", "nochange", None, False),
    ("scan-right", "+>+>+>>+<<<<[>]>.", b"", "nochange", None, False),
    ("scan-stride2", "+>>+>>+>>>>+<<<<<<<<[>>]>.", b"", "nochange", None, False),
    ("io-in-loop", ">+++++[<+++++>-]<[>.+<-]", b"", "nochange", None, False),
    ("dead-loop", "[+++.]++.", b"", "nochange", None, False),
    ("dead-after-loop", "+++[-][+.]+.", b"", "nochange", None, False),
    ("nested-io", "++++[>++++[>+.<-]<-]", b"", "nochange", None, False),
    ("offsets-far", ">>>>>>>>>>>>>>>>>>>>+++++++++[<<<<<<<<<<<<<<<<<<<<+>>>>>>>>>>>>>>>>>>>>-]<<<<<<<<<<<<<<<<<<<<.", b"", "nochange", None, False),
    ("big-run", "+" * 300 + ".", b"", "nochange", None, False),
    ("square", "++++++++[>++++++++<-]>[<+>-]<[>+>+<<-]>>[<<+>>-]<<[>[>+>+<<-]>>[<<+>>-]<<<-]>>.", b"", "nochange", None, False),
    # devices: write 65 into LEDS via a multiply-loop copy, read back and print
    ("mmio-leds", "++++++++[>++++++++<-]>+[<<+>>-]<<[>>+<<-]>>.", b"", "nochange", b"A", True),
    # CONOUT does not read back, so the copy loop literally writes 1 sixty-five times in every mode
    ("mmio-conout", "++++++++[>++++++++<-]>+[<<<<<+>>>>>-]", b"", "nochange", b"\x01" * 65, True),
    # a block that starts with a pointer move and then a transformed loop reaching the device
    # window must resume the literal program from the move, not from the loop
    ("mmio-block-leading-move", ">+++[<<<<+>>>>-]<<<<[>>>>+<<<<-]>>>>.", b"", "nochange", b"\x03", True),
    # a trigger inside a loop must fire once per iteration even though the loop optimises to a multiply
    ("fb-trigger-in-loop",
     "+++[" + "<" * 22 + "+<+" + ">" * 23 + "-]" + "<" * 20 + "[-]++" + "<<<<<+" + ">>>.",
     b"", "nochange", b"\x03", True),
    ("mmio-halt", "+++.<<<<<<<<<<<+>>>>>>>>>>>.", b"", "nochange", b"\x03", True),
    ("mmio-number", "+++++[<<<<<<<<<+>>>>>>>>>-]<<<<<<<<<<+>[>>>>>>>>>+<<<<<<<<<-]>>>>>>>>>.", b"", "nochange", b"\x05", True),
    ("mmio-scan-into", "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<+>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>[<]>+.", b"", "nochange", None, True),
    ("mmio-key-pop", "<<+<[>>>+<<<-]>>>.", b"q", "nochange", b"q", True),
    # serial loopback: send 'H' and 'i' through SER_DATA/SER_TX, then pop and print what came back
    ("serial-loopback",
     "<<<<<<<<<<<<<<<<" + "+" * 72 + "<+>[-]" + "+" * 105 + "<+>>[+<.>]",
     b"", "nochange", b"Hi", True, ["--serial", "loopback"]),
    # framebuffer: plot two pixels, read the first back and print it (FB_COLOR latch = 0x41)
    ("framebuffer-readback",
     "<" * 22 + "+" * 65 + "<+" + ">[-]+<+" + ">>>[-]" + "<<<<<+" + ">>>.",
     b"", "nochange", b"A", True),
]


def readme_matches_regs():
    """README.md's register table must equal `bf --regs` (generated from src/registers.h)."""
    regs = subprocess.run([BF, "--regs"], capture_output=True, timeout=30).stdout.decode().replace("\r\n", "\n")
    readme = open(os.path.join(os.path.dirname(__file__), "..", "README.md"), encoding="utf-8").read().replace("\r\n", "\n")
    if regs in readme:
        return True, ""
    return False, "README.md register table differs from `bf --regs`; paste its output into the README"


TP = os.path.join(os.path.dirname(__file__), "..", "programs", "thirdparty", "cristofani")
FEN = b"rnbqkbnr/pp1ppppp/8/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2\n"
# Daniel B. Cristofani's programs (programs/thirdparty/README.md): file, stdin.  EOF reads as 0.
THIRDPARTY = [
    ("392quine", b""), ("400quine", b""), ("dquine", b""),
    ("bsort", b"the quick brown fox"), ("isort", b"the quick brown fox"), ("qsort", b"the quick brown fox"),
    ("chessboard", FEN), ("collatz", b"27\n97\n871\n"), ("dbf2c", b"++++++++[>++++++++<-]>+."),
    ("dbfi", b"++++++++[>++++++++<-]>+.+.+.!"), ("dvorak", b"hello world\n"),
    ("head", b"".join(b"line %d\n" % i for i in range(1, 16))), ("jabh", b""),
    ("life", b"bc\ncd\nbd\nad\n\n\n\nq\n"), ("numwarp", b"3.14159\n"), ("rot13", b"Hello, World!\n"),
    ("sierpinski", b""), ("squares", b""), ("tictactoe", b"1\n2\n3\n4\n5\n6\n7\n8\n9\n"),
    ("utm", b"b1b1bbb1c1c11111d\n"), ("wc", b"one two three\nfour five\n"), ("xmastree", b"5\n"),
    ("bounce", b"  ^  \n /  \\\n\n\n\n\n"),
]


def run_path(path, stdin, flags):
    r = subprocess.run([BF, path, "--eof", "0", "--quiet-devices"] + flags, input=stdin, capture_output=True, timeout=60)
    return r.returncode, r.stdout, r.stderr.decode("latin-1")


def thirdparty_cases():
    """Real programs through all three executors, checked against the reference interpreter."""
    fails = 0
    for name, stdin in THIRDPARTY:
        path = os.path.join(TP, name + ".b")
        src = open(path, encoding="latin-1").read()
        expected = reference(src, stdin, "0")
        results = {label: run_path(path, stdin, flags) for label, flags in (("jit", []), ("interp", ["--interp"]), ("noopt", ["--no-opt"]))}
        ok = all(r[0] == 0 and r[1] == expected for r in results.values())
        if ok and name.endswith("quine"):
            # a quine's output, run again, must reproduce itself
            again = subprocess.run([BF, "-e", expected.decode("latin-1"), "--quiet-devices"], capture_output=True, timeout=60).stdout
            ok = again == expected
        print(("PASS " if ok else "FAIL ") + "thirdparty-" + name)
        if not ok:
            fails += 1
            print("   expected:", expected[:200])
            for k, v in results.items():
                print(f"   {k}: rc={v[0]} out={v[1][:200]!r} err={v[2].strip()!r}")
    return fails


# BFL programs whose arithmetic compiles to the loops the optimiser runs as idioms
# (src/ir.cpp, "loop idioms").  Each is also run with one cell poked before it
# starts, so that scratch cells hold unexpected values and the idioms' guards
# must send the loop through its body.  Everything is checked against the
# reference interpreter.
IDIOM_PROGRAMS = [
    ("compare", "byte x = in; byte y = in; byte f = x < y; out f; f = y < x; out f; f = x >= y; out f;"),
    ("word-carry", "word a = in; word b = in; a = a * 300 + b; word c = a + 50000; out byte(c), byte(c >> 8);"
                   " c = a - 40000; out byte(c), byte(c >> 8);"),
    ("long-mul-div", "long a = in; long b = in; a = a * 100003 + 7; long c = a * b;"
                     " out byte(c), byte(c >> 8), byte(c >> 16), byte(c >> 24); c = a / (b + 1); out byte(c), byte(c >> 8);"),
    ("bits", "byte x = in; byte y = in; out x >> 1, x & y, x | y, x ^ y, x << 3;"),
    ("byte-divmod", "byte a = in; byte b = in; out a / b, a % b;"),
    ("signed", "int a = in; int b = in; a = a - 100; int c = a * b; out byte(c), byte(c >> 8); c = a / 7; out byte(c), byte(c >> 8);"
               " if (a < b) { out 1; } else { out 2; }"),
    # the division loop (ir.cpp, "the division loop"); its cells lie above the operands, so these
    # are also poked further up (with the first input), into the counter, A, R, B and every scratch cell
    ("word-divide", "word a = in; word b = in; a = a * 300 + b; word d = b + 1; word c = a / d; out byte(c), byte(c >> 8);"
                    " c = a % d; out byte(c), byte(c >> 8);", range(16, 48)),
    ("long-divide", "long a = in; long b = in; a = a * 100003 - 7000000; long d = b - 3; long c = a / d;"
                    " out byte(c), byte(c >> 8), byte(c >> 16), byte(c >> 24); dword u = dword(a) % dword(d * 1000);"
                    " out byte(u), byte(u >> 8), byte(u >> 16), byte(u >> 24);", range(16, 96)),
]
IDIOM_INPUTS = [b"\xc8\x4d", b"\x00\x00", b"\xff\x01", b"\x05\x05"]


def idiom_cases():
    import tempfile
    fails = 0
    tmp = tempfile.mkdtemp()
    for name, prog, *more in IDIOM_PROGRAMS:
        bfl = os.path.join(tmp, name + ".bfl")
        out_b = os.path.join(tmp, name + ".b")
        open(bfl, "w").write(prog + "\n")
        r = subprocess.run([BF, "--emit", out_b, bfl], capture_output=True, timeout=60)
        if r.returncode:
            print("FAIL idiom-" + name + " (compile)", r.stderr.decode("latin-1")[:300])
            fails += 1
            continue
        base = "".join(ch for ch in open(out_b, encoding="latin-1").read() if ch in "+-<>.,[]")
        variants = [("", base)] + [(f"poke{k}:{v}", ">" * k + "+" * v + "<" * k + base) for k in range(2, 16) for v in (1,)] \
                   + [(f"poke{k}:255", ">" * k + "-" + "<" * k + base) for k in (3, 5, 7, 9)]
        further = [(f"poke{k}:{v}", ">" * k + ("+" if v == 1 else "-") + "<" * k + base) for k in (more[0] if more else ()) for v in (1, 255)]
        bad = []
        for stdin in IDIOM_INPUTS:
            for vname, src in variants + (further if stdin == IDIOM_INPUTS[0] else []):
                # the unoptimised run executes every command as written (checked against the
                # reference interpreter by the other cases); a poke may make a program spin
                path = os.path.join(tmp, "variant.b")
                open(path, "w", encoding="latin-1").write(src)
                try:
                    r = subprocess.run([BF, path, "--eof", "0", "--no-opt", "--quiet-devices"], input=stdin, capture_output=True, timeout=10)
                except subprocess.TimeoutExpired:
                    continue
                expected = r.stdout
                for label, flags in (("jit", []), ("interp", ["--interp"])):
                    rc, out, err = run_path(path, stdin, flags)
                    if rc != r.returncode or out != expected:
                        bad.append(f"{vname} {stdin!r} {label}: rc={rc} out={out!r} expected={expected!r} {err.strip()[:100]}")
        print(("PASS " if not bad else "FAIL ") + "idiom-" + name)
        if bad:
            fails += 1
            for b in bad[:6]:
                print("   " + b)
    return fails, len(IDIOM_PROGRAMS)


def bf_fuzz():
    """A few programs of tests/fuzz.py: random brainfuck aimed at the loops the
    optimiser rewrites, through every executor, against the reference interpreter."""
    import tempfile
    sys.path.insert(0, os.path.dirname(__file__))
    sys.dont_write_bytecode = True
    import fuzz
    results = fuzz.run_cases(BF, 0, 60, 9000, tempfile.mkdtemp(), log=lambda m: None)
    bad = [r for r in results if r.status == "fail"]
    ran = sum(r.status != "skip" for r in results)
    ok = not bad and ran >= 50
    print(("PASS " if ok else "FAIL ") + f"fuzz-bf ({ran} random programs)")
    for r in bad:
        print(f"   seed {r.seed}: " + "; ".join(str(f) for f in r.fails[:3]))
        print(f"   (python tests/fuzz.py --replay bf {r.seed})")
    return 0 if ok else 1


def fuzz_stack():
    """tests/fuzz.py gives a program whose recursion overflows the default stack a
    bigger one (an overflow is unchecked, and such a program once spun forever)."""
    import tempfile
    sys.path.insert(0, os.path.dirname(__file__))
    sys.dont_write_bytecode = True
    import fuzz as F
    n = F.Var("p0n")
    body = [F.If(F.Bin("!=", n, F.Lit(0)), [F.CallS("p0", [F.Cast("byte", F.Bin("-", n, F.Lit(1))), F.Var("p0a")])], [])]
    prog = F.Program([], [F.Proc("p0", None, [("byte", False, "p0n"), ("long", False, "p0a")], body, True)],
                     [F.CallS("p0", [F.Lit(50), F.Lit(7)])], [F.Out([F.Lit(75)])])
    d = tempfile.mkdtemp()
    fitted = F.fit_stack(BF, d, prog, b"")
    fails = F.check_bfl(BF, d, prog.text(), b"", F.model_run(prog, b""), F.MODES[:2], determinism=False, layout=False)
    ok = fitted and prog.stack == F.STACK_BIG and not fails
    print(("PASS " if ok else "FAIL ") + "fuzz-stack")
    for f in fails[:3]:
        print("   " + str(f))
    return 0 if ok else 1


def main():
    fails = 0
    idiom_fails, idiom_n = idiom_cases()
    fails += idiom_fails
    fails += bf_fuzz()
    fails += fuzz_stack()
    ok, why = readme_matches_regs()
    print(("PASS " if ok else "FAIL ") + "readme-register-table")
    if not ok:
        fails += 1
        print("   " + why)
    fails += thirdparty_cases()
    for case in CASES:
        name, src, stdin, eof, expected, devices = case[:6]
        extra = list(case[6]) if len(case) > 6 else []
        try:
            if expected is None and not devices:
                expected = reference(src, stdin, eof)
            results = {}
            for label, flags in (("jit", []), ("interp", ["--interp"]), ("noopt", ["--no-opt"])):
                rc, out, err = run_bf(src, stdin, eof, flags + extra + ["--quiet-devices"])
                results[label] = (rc, out, err.strip())
            ok = all(r[1] == (expected if expected is not None else results["jit"][1]) for r in results.values())
            ok = ok and all(r[0] == 0 for r in results.values())
            if name == "mmio-scan-into":
                # pointer scanned deep into the device window and came back; just require agreement
                ok = results["jit"][1] == results["interp"][1] == results["noopt"][1] and all(r[0] == 0 for r in results.values())
        except Exception as e:  # noqa
            ok = False
            results = {"exception": (None, str(e).encode(), "")}
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            fails += 1
            print("   expected:", expected)
            for k, v in results.items():
                print(f"   {k}: rc={v[0]} out={v[1]!r} err={v[2]!r}")
    total = len(CASES) + 3 + len(THIRDPARTY) + idiom_n
    print(f"{total - fails}/{total} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
