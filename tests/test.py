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


def main():
    fails = 0
    ok, why = readme_matches_regs()
    print(("PASS " if ok else "FAIL ") + "readme-register-table")
    if not ok:
        fails += 1
        print("   " + why)
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
    print(f"{len(CASES) + 1 - fails}/{len(CASES) + 1} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
