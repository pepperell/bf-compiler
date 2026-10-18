"""Benchmark for the code generator: compiled sizes, frame times and the cost of single operations.

    python tools/bench.py [--json FILE] [--compare FILE] [--only sizes|frames|ops] [bf.exe]

Measures with out\\bf.exe of the tree it runs in (or the bf.exe given):

  sizes   brainfuck commands (bf --emit) of every programs/samples/*.bfl and programs/*.bfl
  frames  ms per frame of the animated samples, run headless: a scratch copy of each
          runs a fixed number of frames without its delays and reads the TICKS clock
          around them; the minimum of 3 runs, since other work may share the CPU.
          Also the raytracers' light-map precompute
  ops     IR instructions executed (bf --profile) by one statement doing a single
          operation on values read with 'in', so nothing folds; also the brainfuck
          commands the statement adds to the program

--json FILE writes the results; --compare FILE prints them next to an earlier --json
file with the change. Takes under a minute.
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = 3

# ---- sizes ------------------------------------------------------------------------------

def emit_size(bf, src, tmp):
    """brainfuck commands bf --emit reports for src, or None if it does not compile"""
    r = subprocess.run([bf, '--emit', os.path.join(tmp, 'emit.b'), src],
                       capture_output=True, text=True, timeout=300)
    m = re.search(r'(\d+) commands', r.stdout + r.stderr)
    if r.returncode or not m:
        return None
    return int(m.group(1))

def sizes(bf, tmp):
    files = sorted(glob.glob(os.path.join(ROOT, 'programs', 'samples', '*.bfl'))) + \
            sorted(glob.glob(os.path.join(ROOT, 'programs', '*.bfl')))
    res = {}
    for f in files:
        name = os.path.relpath(f, ROOT).replace('\\', '/')
        res[name] = emit_size(bf, f, tmp)
        print(f'  {name:40} {fmt(res[name])}', flush=True)
    return res

# ---- frames -----------------------------------------------------------------------------

# sample, frames a run, whether it has a light map
FRAMES = [('raytracer', 12), ('raytracer_text', 20), ('cube', 300), ('starfield', 300)]

CLOCK = '''
proc dword bench_ms() {
    pulse TICK_LATCH;
    return ((dword(TICKS3) * 256 + TICKS2) * 256 + TICKS1) * 256 + TICKS0;
}
'''

def frame_program(name, n):
    """the sample with its endless loop cut to n frames and timed, delays off"""
    s = open(os.path.join(ROOT, 'programs', 'samples', name + '.bfl'), encoding='utf-8').read()
    s = re.sub(r'^const DELAY_MS = \d+;', 'const DELAY_MS = 0;', s, flags=re.M)
    loops = list(re.finditer(r'^while \(.*\) \{\n', s, flags=re.M))
    if not loops:
        raise RuntimeError(f'{name}.bfl: no main loop (a "while (...) {{" line at column 0)')
    lp = loops[-1]
    loop = ('word bench_n = 0;\n'
            'dword bench_t0 = bench_ms();\n'
            'while (1) {\n'
            f'    if (bench_n == {n}) {{ print "\\n#BENCH frames ", bench_ms() - bench_t0, "\\n"; break; }}\n'
            '    bench_n++;\n')
    s = s[:lp.start()] + loop + s[lp.end():]
    first = lp.start()
    lm = re.search(r'^prepare_lights\(\);\nbuild_light_map\(\);\n', s, flags=re.M)
    if lm:
        timed = ('dword bench_l0 = bench_ms();\n' + lm.group(0) +
                 'print "\\n#BENCH lightmap ", bench_ms() - bench_l0, "\\n";\n')
        s = s[:lm.start()] + timed + s[lm.end():]
        first = lm.start()
    return s[:first] + CLOCK + s[first:]

def frames(bf, tmp):
    res = {}
    for name, n in FRAMES:
        path = os.path.join(tmp, name + '.bfl')
        open(path, 'w', encoding='utf-8', newline='\n').write(frame_program(name, n))
        exe = os.path.join(tmp, name + '.b')       # compile once, time the brainfuck
        r = subprocess.run([bf, '--emit', exe, path], capture_output=True, text=True, timeout=300)
        if r.returncode:
            raise RuntimeError(f'{name}: does not compile: {r.stderr.strip()}')
        per, lmap = [], []
        for _ in range(RUNS):
            r = subprocess.run([bf, '--quiet-devices', exe], capture_output=True, timeout=600,
                               stdin=subprocess.DEVNULL)
            out = r.stdout.decode('latin-1')
            got = dict(re.findall(r'#BENCH (\w+) (\d+)', out))
            if r.returncode or 'frames' not in got:
                raise RuntimeError(f'{name}: run failed (rc {r.returncode}): {r.stderr.decode("latin-1").strip()}')
            per.append(int(got['frames']) / n)
            if 'lightmap' in got:
                lmap.append(int(got['lightmap']))
        res[name] = round(min(per), 2)
        line = f'  {name:16} {min(per):8.2f} ms a frame  ({n} frames; runs ' + \
               ', '.join(f'{v:.2f}' for v in per) + ')'
        if lmap:
            res[name + ' light map'] = min(lmap)
            line += f'   light map {min(lmap)} ms'
        print(line, flush=True)
    return res

# ---- ops --------------------------------------------------------------------------------

# Values come from the two input bytes (200, 77), so nothing is known at compile time.
SETUP = '''byte x = in; byte y = in;
long la = long(x) * 4111 + 70000;
long lb = long(y) * 3313 - 400000;
long ld = long(y) * 13 + 5;
long fa = long(x) * 41 - 3000;
long fb = long(y) * 53 + 100;
int ia = int(x) * 97 - 30000;
int ib = int(y) - 30;
word wa = word(x) * 251 + 7;
word wb = word(y) + 3;
byte ba = x;
byte bb = y - 60;
long lc; int ic; word wc; byte bc;
'''
TAIL = 'out byte(lc) + byte(lc >> 8) + byte(lc >> 16) + byte(lc >> 24) + byte(ic) + byte(wc) + bc;\n'

# The cost of * depends on the operands' values: 'long * long' has the operands of the
# shifted product, so the difference is the shift.
OPS = [('long * long', 'lc = fa * fb;'),           # 5200 * 4181, Q12-sized as in the raytracer
       ('long * long, large', 'lc = la * lb;'),    # 892200 * -144899, wraps
       ('long / long', 'lc = la / ld;'),           # 892200 / 1006
       ('long % long', 'lc = la % ld;'),
       ('int / int', 'ic = ia / ib;'),             # -10600 / 47
       ('word / word', 'wc = wa / wb;'),           # 50207 / 80
       ('(long * long) >> 12', 'lc = (fa * fb) >> 12;'),   # 5200 * 4181 >> 12, fixed point
       ('byte / byte', 'bc = ba / bb;')]           # 200 / 17

def profile_line(bf, path, line):
    """IR instructions bf --profile charges to the statement on line"""
    r = subprocess.run([bf, '--profile', path], input=bytes([200, 77]), capture_output=True, timeout=300)
    text = (r.stdout + r.stderr).decode('latin-1')        # the report is on stderr
    if r.returncode or 'IR instructions executed' not in text:
        raise RuntimeError(f'--profile failed: {r.stderr.decode("latin-1").strip()}')
    for m in re.finditer(r'^\s*(\d+)\s+[\d.]+%\s+(\d+)\s', text, flags=re.M):
        if int(m.group(2)) == line:
            return int(m.group(1))
    return 0

def ops(bf, tmp):
    res = {}
    base = os.path.join(tmp, 'op_base.bfl')
    open(base, 'w', newline='\n').write(SETUP + TAIL)
    base_size = emit_size(bf, base, tmp)
    line = SETUP.count('\n') + 1
    for name, stmt in OPS:
        path = os.path.join(tmp, 'op.bfl')
        open(path, 'w', newline='\n').write(SETUP + stmt + '\n' + TAIL)
        n = profile_line(bf, path, line)
        size = emit_size(bf, path, tmp)
        res[name] = n
        res[name + ' size'] = size - base_size if size is not None and base_size is not None else None
        print(f'  {name:22} {n:>9,} IR instructions   {fmt(res[name + " size"]):>9} commands', flush=True)
    return res

# ---- report -----------------------------------------------------------------------------

def fmt(v):
    if v is None:
        return '-'
    if isinstance(v, float):
        return f'{v:,.2f}'
    return f'{v:,}'

UNITS = {'sizes': 'brainfuck commands', 'frames': 'ms', 'ops': 'IR instructions executed (size: commands)'}

def compare(old, new):
    for group in ('sizes', 'frames', 'ops'):
        if group not in new:                    # not measured this time (--only)
            continue
        a, b = old.get(group, {}), new[group]
        print(f'\n{group} ({UNITS[group]})')
        print(f'  {"":40} {"old":>12} {"new":>12} {"change":>9}')
        for k in list(a) + [k for k in b if k not in a]:
            x, y = a.get(k), b.get(k)
            ch = ''
            if x and y is not None:
                ch = f'{(y - x) / x * 100:+.1f}%'
            print(f'  {k:40} {fmt(x):>12} {fmt(y):>12} {ch:>9}')

def git_rev():
    try:
        r = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True)
        d = subprocess.run(['git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=no'],
                           capture_output=True, text=True)
        return r.stdout.strip() + ('+' if d.stdout.strip() else '')
    except OSError:
        return ''

def main():
    ap = argparse.ArgumentParser(description='Compiled sizes, frame times and operation costs.')
    ap.add_argument('bf', nargs='?', default=os.path.join(ROOT, 'out', 'bf.exe'))
    ap.add_argument('--json', help='write the results to this file')
    ap.add_argument('--compare', help='print the results next to this earlier --json file')
    ap.add_argument('--only', choices=['sizes', 'frames', 'ops'])
    a = ap.parse_args()
    if not os.path.exists(a.bf):
        sys.exit(f'no {a.bf}: build first, or name a bf.exe')
    old = json.load(open(a.compare)) if a.compare else None

    res = {'bf': os.path.abspath(a.bf), 'commit': git_rev(), 'date': time.strftime('%Y-%m-%d %H:%M')}
    tmp = tempfile.mkdtemp(prefix='bfbench_')
    t = time.time()
    try:
        for group, fn in (('sizes', sizes), ('frames', frames), ('ops', ops)):
            if a.only and a.only != group:
                continue
            print(f'{group} ({UNITS[group]})', flush=True)
            res[group] = fn(a.bf, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'took {time.time() - t:.0f} s')
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(res, f, indent=1)
    if old:
        print(f'\nold: {old.get("commit", "?")} {old.get("date", "")}   new: {res["commit"]} {res["date"]}')
        compare(old, res)

if __name__ == '__main__':
    main()
