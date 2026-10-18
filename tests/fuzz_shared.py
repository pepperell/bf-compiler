"""Differential test of shared procedures: random BFL programs run with their
procedures plain (inlined at every call) and with some marked 'shared' (one copy,
called through the trampoline).  The outputs must be the same.

Two things legitimately differ between inlined and trampolined procedures and
are avoided: the order of side effects when a call sits inside an expression
(so only side-effect-free procedures are called in expressions), and ref
parameters, which are copy-in / copy-out through the trampoline (so a procedure
with ref parameters touches no globals, and no variable is passed to two of them).

    python testsuzz_shared.py [count] [first_seed]     (tests	est_lang.py runs a few)
"""
import os
import random
import subprocess
import sys
import tempfile

TYPES = ['byte', 'word', 'int', 'long']


class Gen:
    def __init__(self, rnd):
        self.r = rnd
        self.vtype = {}

    def expr(self, reads, depth, pure):
        r = self.r
        if depth > 2 or r.random() < 0.3:
            if reads and r.random() < 0.7:
                return r.choice(reads)
            return str(r.randint(0, 300))
        if r.random() < 0.15 and pure:
            p = r.choice(pure)
            args = ', '.join(f"{pt}({self.expr(reads, depth + 1, pure)})" for pt, _ in p['params'])
            return f"{p['ret']}({p['name']}({args}))"
        op = r.choice(['+', '-', '*', '/', '%', '&', '|', '^', '<', '==', '>>', '<<'])
        a, b = self.expr(reads, depth + 1, pure), self.expr(reads, depth + 1, pure)
        if op in ('/', '%'):
            return f"({a} {op} ({b} | 1))"
        if op in ('>>', '<<'):
            return f"({a} {op} {r.randint(0, 9)})"
        return f"({a} {op} {b})"

    # targets: assignable scalars; reads: readable scalars; arrs: (name, size) writable arrays;
    # out: may print; calls: procedures callable as statements; pure: callable in expressions
    def stmts(self, targets, reads, arrs, depth, calls, pure, in_loop, n, out_ok):
        r = self.r
        res = []
        for _ in range(n):
            k = r.random()
            if k < 0.35 and targets:
                v = r.choice(targets)
                res.append(f"{v} = {self.vtype[v]}({self.expr(reads, 0, pure)});")
            elif k < 0.45 and arrs:
                a, size = r.choice(arrs)
                res.append(f"{a}[byte({self.expr(reads, 1, pure)}) % {size}] = {self.expr(reads, 1, pure)};")
            elif k < 0.55 and arrs and targets:
                a, size = r.choice(arrs)
                v = r.choice(targets)
                res.append(f"{v} = {self.vtype[v]}({a}[byte({self.expr(reads, 1, pure)}) % {size}]);")
            elif k < 0.65 and depth < 2:
                cond = self.expr(reads, 1, pure)
                body = self.stmts(targets, reads, arrs, depth + 1, calls, pure, in_loop, r.randint(1, 3), out_ok)
                els = self.stmts(targets, reads, arrs, depth + 1, calls, pure, in_loop, r.randint(1, 2), out_ok) if r.random() < 0.5 else []
                s = f"if ({cond}) {{ {' '.join(body)} }}"
                if els: s += f" else {{ {' '.join(els)} }}"
                res.append(s)
            elif k < 0.75 and depth < 2:
                cv = f"c{depth}_{r.randint(0, 99999)}"
                self.vtype[cv] = 'byte'
                body = self.stmts(targets, reads + [cv], arrs, depth + 1, calls, pure, True, r.randint(1, 3), out_ok)
                res.append(f"byte {cv}; for ({cv} = 0; {cv} < {r.randint(1, 4)}; {cv}++) {{ {' '.join(body)} }}")
            elif k < 0.8 and in_loop:
                res.append(r.choice(['break;', 'continue;']) if r.random() < 0.5 else f"if ({self.expr(reads, 1, pure)} & 1) {{ {r.choice(['break;', 'continue;'])} }}")
            elif k < 0.9 and calls:
                p = r.choice(calls)
                args = []
                ok = True
                for (pt, ref) in p['params']:
                    if ref:
                        cands = [v for v in targets if self.vtype.get(v) == pt and v not in args]
                        if not cands: ok = False; break
                        args.append(r.choice(cands))
                    else:
                        args.append(f"{pt}({self.expr(reads, 1, pure)})")
                if ok:
                    res.append(f"{p['name']}({', '.join(args)});")
            elif out_ok and reads:
                res.append(f"out byte({r.choice(reads)});")
        return res

    def program(self):
        r = self.r
        g = []
        decls = []
        for i in range(r.randint(2, 4)):
            t = r.choice(TYPES); name = f"g{i}"
            self.vtype[name] = t; g.append(name)
            decls.append(f"{t} {name} = {r.randint(0, 200)};")
        garr = []
        if r.random() < 0.6:
            t = r.choice(TYPES); size = r.randint(2, 6)
            decls.append(f"{t} ga[{size}];"); garr.append(('ga', size))
        pure, calls, text = [], [], []
        for pi in range(r.randint(2, 5)):
            name = f"p{pi}"
            kind = r.choice(['pure', 'impure', 'ref'])
            ret = r.choice(TYPES) if kind == 'pure' else r.choice([None] + TYPES)
            params, plocal = [], []
            for k in range(r.randint(0 if kind != 'ref' else 1, 3)):
                pt = r.choice(TYPES)
                ref = kind == 'ref' and (k == 0 or r.random() < 0.4)
                params.append((pt, ref))
                pn = f"{name}a{k}"
                self.vtype[pn] = pt
                plocal.append(pn)
            body_decl, larr = [], []
            if r.random() < 0.3:
                lt = r.choice(TYPES); ls = r.randint(2, 4)
                body_decl.append(f"{lt} {name}arr[{ls}];"); larr.append((f"{name}arr", ls))
            globals_visible = [] if kind == 'ref' else g
            for k in range(r.randint(0, 2)):
                lt = r.choice(TYPES); ln = f"{name}l{k}"
                self.vtype[ln] = lt
                body_decl.append(f"{lt} {ln} = {lt}({self.expr(plocal + globals_visible, 0, pure)});")
                plocal.append(ln)
            if kind == 'pure':
                body = self.stmts(plocal, plocal + g, larr, 0, [], pure, False, r.randint(2, 5), False)
            elif kind == 'impure':
                body = self.stmts(plocal + g, plocal + g, larr + garr, 0, calls, pure, False, r.randint(2, 5), True)
            else:
                body = self.stmts(plocal, plocal, larr, 0, [c for c in calls if c['kind'] == 'ref'], [], False, r.randint(2, 5), True)
            if ret:
                body.append(f"return {ret}({self.expr(plocal + globals_visible, 0, pure if kind != 'ref' else [])});")
            plist = ', '.join(('ref ' if ref else '') + f"{pt} {name}a{k}" for k, (pt, ref) in enumerate(params))
            text.append(f"proc {ret + ' ' if ret else ''}{name}({plist}) {{ {' '.join(body_decl)} {' '.join(body)} }}")
            p = {'name': name, 'ret': ret, 'params': params, 'kind': kind}
            calls.append(p)
            if kind == 'pure': pure.append(p)
        main = self.stmts(g, g, garr, 0, calls, pure, False, r.randint(4, 8), True)
        main += [f"out byte({v}), byte({v} >> 8);" for v in g]
        return '\n'.join(decls + text + main) + '\n', [p['name'] for p in calls]


def run(count, seed0, bf, tmp, log=print):
    """Compare count programs from seed0 on; returns (programs compared, mismatches)."""
    fails = ran = 0
    for n in range(count):
        seed = seed0 + n
        rnd = random.Random(seed)
        src, names = Gen(rnd).program()
        shared_names = [p for p in names if rnd.random() < 0.6] or names[:1]
        sh = src
        for p in shared_names:
            sh = sh.replace(f"proc {p}(", f"shared proc {p}(")
            for t in TYPES:
                sh = sh.replace(f"proc {t} {p}(", f"shared proc {t} {p}(")
        a, b = os.path.join(tmp, f'f{seed}_plain.bfl'), os.path.join(tmp, f'f{seed}_shared.bfl')
        open(a, 'w').write(src)
        open(b, 'w').write(sh)
        try:
            ra = subprocess.run([bf, '--quiet-devices', a], capture_output=True, timeout=20)
        except subprocess.TimeoutExpired:
            continue
        if ra.returncode != 0:
            continue                    # the random program itself misbehaves
        try:
            rb = subprocess.run([bf, '--quiet-devices', b], capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            log(f"   seed {seed}: the shared version timed out ({b})")
            fails += 1
            continue
        ran += 1
        if rb.returncode != 0 or rb.stdout != ra.stdout:
            fails += 1
            log(f"   seed {seed}: plain {ra.stdout[:40]!r}, shared rc={rb.returncode} {rb.stdout[:40]!r} ({b})")
    return ran, fails


if __name__ == "__main__":
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    seed0 = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    bf = os.path.join(os.path.dirname(__file__), "..", "out", "bf.exe")
    ran, fails = run(count, seed0, bf, tempfile.mkdtemp(prefix="bflfuzz"))
    print(f"{ran} programs compared, {fails} mismatches")
    sys.exit(1 if fails else 0)
