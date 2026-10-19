"""Render a sample headless into an animated GIF, and give a BFL program's compilation figures.

    python tools/demo_gif.py flag OUT.gif [bf.exe]     programs/samples/flag.bfl, 2.5 s of flying
    python tools/demo_gif.py life OUT.gif [bf.exe]     programs/samples/life.bfl, every generation
    python tools/demo_gif.py stats FILE.bfl [bf.exe]   lines and statements of BFL, then what they
                                                       compile to (the captions in README.md)

flag: a scratch copy of the sample runs a fixed number of frames instead of its endless loop and,
after every other one, reads the framebuffer back with FB_READ and writes its 8192 RRRGGGBB bytes
to stdout (as tools/rt_gif.py does for the raytracer); frames are scaled up 4 times.
life: the sample's terminal output is split at each cursor-home escape and drawn as text in the
UI terminal's colours.  Runs out\\bf.exe of the tree it is in unless a bf.exe is given.  Needs
Pillow (and Consolas, which Windows has, for life).
"""
import os, re, subprocess, sys, tempfile, time
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAMPLES = os.path.join(ROOT, 'programs', 'samples')
W, H, SCALE = 128, 64, 4


def run_bfl(bf, text, timeout=1200):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'capture.bfl')
        open(path, 'w', encoding='utf-8', newline='\n').write(text)
        t = time.time()
        r = subprocess.run([bf, '--quiet-devices', path], capture_output=True, timeout=timeout)
        took = time.time() - t
    if r.returncode != 0:
        sys.exit(f'bf exited with {r.returncode}: ' + r.stderr.decode('latin-1').strip())
    return r.stdout, took


# ---- flag ------------------------------------------------------------------------------------
FLAG_SKIP, FLAG_SHOTS = 30, 38       # skip the first second (the flag starts hanging still)
FLAG_RUN = r'''
proc gif_dump() {
    byte gy;
    for (gy = 0; gy < 64; gy++) {
        byte gx;
        for (gx = 0; gx < 128; gx++) {
            FB_X = gx;
            FB_Y = gy;
            pulse FB_READ;
            byte gc = FB_COLOR;
            out gc;
        }
    }
}

word frame = 0;
word shots = 0;
while (shots < %d) {
    weather();
    place_camera(word(350 + mulshr(long(sin1024(frame / 2)), 30, 10)));
    FB_BUFFER = 1;
    background();
    shadow();
    pole();
    cloth();
    FB_BUFFER = 0;
    if (frame >= %d && (frame & 1) == 0) { gif_dump(); shots++; }
    byte s;
    for (s = 0; s < SUB; s++) { step(s == 0); }
    frame++;
}
''' % (FLAG_SHOTS, FLAG_SKIP)


def rgb(v):                       # as FramebufferDevice::colour in src/hw.h
    return ((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3)


def flag(out, bf):
    s = open(os.path.join(SAMPLES, 'flag.bfl'), encoding='utf-8').read()
    data, took = run_bfl(bf, s[:s.index('print "BFL flag')] + FLAG_RUN)
    if len(data) != FLAG_SHOTS * W * H:
        sys.exit(f'expected {FLAG_SHOTS * W * H} bytes of frames, got {len(data)}')
    palette = [c for v in range(256) for c in rgb(v)]
    frames = []
    for f in range(FLAG_SHOTS):
        im = Image.frombytes('P', (W, H), data[f * W * H:(f + 1) * W * H])
        im.putpalette(palette)
        frames.append(im.resize((W * SCALE, H * SCALE), Image.NEAREST))
    # every other frame of a 30 frames a second animation: 66 ms each
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=66, loop=0, optimize=False)
    print(f'rendered {FLAG_SKIP + 2 * FLAG_SHOTS} frames in {took:.1f} s; wrote {FLAG_SHOTS} to {out}')


# ---- life ------------------------------------------------------------------------------------
BG, FG, BRIGHT = (0, 0, 0), (204, 204, 204), (255, 255, 255)
ANSI = {30: (0, 0, 0), 31: (205, 49, 49), 32: (13, 188, 121), 33: (229, 229, 16), 34: (36, 114, 200),
        35: (188, 63, 188), 36: (17, 168, 205), 37: (204, 204, 204)}   # src/terminal.cpp's palette


def terminal_frames(data):
    """Split the output at each cursor home and keep each screen as rows of (char, colour, bold)."""
    text = data.decode('latin-1')
    screens = []
    for chunk in text.split('\x1b[H')[1:]:
        rows, row, colour, bold = [], [], FG, False
        for m in re.finditer(r'\x1b\[([0-9;]*)([A-Za-z])|(\n)|(.)', chunk, re.S):
            if m.group(2):
                if m.group(2) == 'm':
                    for p in (m.group(1) or '0').split(';'):
                        p = int(p or 0)
                        if p == 0: colour, bold = FG, False
                        elif p == 1: bold = True
                        elif p in ANSI: colour = ANSI[p]
                continue
            if m.group(3):
                rows.append(row); row = []
            elif m.group(4) not in ('\r',):
                row.append((m.group(4), colour, bold))
        if row: rows.append(row)
        screens.append(rows)
    return screens


def life(out, bf):
    data, took = run_bfl(bf, open(os.path.join(SAMPLES, 'life.bfl'), encoding='utf-8').read())
    screens = terminal_frames(data)
    fonts = os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts')
    font = ImageFont.truetype(os.path.join(fonts, 'consola.ttf'), 18)
    bold_font = ImageFont.truetype(os.path.join(fonts, 'consolab.ttf'), 18)
    cw = round(font.getlength('M'))
    ch = 22
    cols = max(len(r) for s in screens for r in s)
    nrows = max(len(s) for s in screens)
    pad = 10
    frames = []
    for rows in screens:
        im = Image.new('RGB', (cols * cw + 2 * pad, nrows * ch + 2 * pad), BG)
        d = ImageDraw.Draw(im)
        for y, row in enumerate(rows):
            for x, (c, colour, bold) in enumerate(row):
                if c != ' ':
                    d.text((pad + x * cw, pad + y * ch), c, font=bold_font if bold else font,
                           fill=BRIGHT if bold and colour == FG else colour)
        frames.append(im.convert('P', palette=Image.ADAPTIVE, colors=16))
    durations = [150] * len(frames)
    durations[-1] = 1500                                   # hold the last generation
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=durations, loop=0, optimize=True)
    print(f'ran in {took:.1f} s; wrote {len(frames)} generations to {out}')


# ---- stats -----------------------------------------------------------------------------------
def stats(path, bf):
    """Lines and statements of BFL and what they compile to, as a Markdown caption."""
    with tempfile.TemporaryDirectory() as tmp:
        b = os.path.join(tmp, 'out.b')
        t = time.time()
        r = subprocess.run([bf, '--emit', b, path], capture_output=True, timeout=300)
        took = time.time() - t
        if r.returncode != 0:
            sys.exit(r.stderr.decode('latin-1'))
        line = (r.stdout + r.stderr).decode('latin-1')
        cmds, cells, ir, x64 = (int(m) for m in re.search(
            r'(\d+) commands .*?, (\d+) cells, (\d+) IR instructions, (\d+) bytes of x64', line).groups())
        files, stmts = {}, set()
        for l in open(b + '.map', encoding='utf-8'):
            p = l.split()
            if p[:1] == ['F']:
                files[p[1]] = p[2]
            elif p[:1] == ['S']:
                stmts.add((p[8], p[1], p[2]))          # file, line, column: once however often inlined
    src = open(path, encoding='utf-8').read().splitlines()
    code = sum(1 for l in src if l.strip() and not l.strip().startswith('//'))
    name = os.path.basename(path)
    mb = x64 / 1e6
    size = f'{mb:.1f} MB' if mb >= 1 else f'{x64 / 1e3:.0f} KB'
    return (f'`{name}`: {len(src):,} lines of BFL ({code:,} of code, {len(stmts):,} statements) → '
            f'{cmds:,} brainfuck commands → {ir:,} IR instructions → {size} of x64; '
            f'{cells:,} tape cells; compiled in ' + (f'{took:.1f} s' if took >= 1 else f'{took * 1000:.0f} ms'))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('flag', 'life', 'stats'):
        sys.exit(__doc__)
    bf = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'out', 'bf.exe')
    if sys.argv[1] == 'stats':
        sys.stdout.reconfigure(encoding='utf-8')
        print(stats(sys.argv[2], bf))
    else:
        {'flag': flag, 'life': life}[sys.argv[1]](sys.argv[2], bf)


if __name__ == '__main__':
    main()
