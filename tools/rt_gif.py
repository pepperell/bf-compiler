"""Render one camera orbit of programs/samples/raytracer.bfl headless into an animated GIF.

    python tools/rt_gif.py OUT.gif [--png FRAME.png] [--frame N] [bf.exe]

A scratch copy of the sample replaces its endless loop with one orbit at the sample's own
step (its STEP constant: 10 degrees, 36 frames); after each frame it reads the framebuffer
back with FB_READ and writes the 8192 RRRGGGBB bytes to stdout. The frames are scaled up 4
times (nearest neighbour) and shown 30 ms each, about the time the JIT takes to draw one.
--png also saves frame N (default 0) as a PNG, to look at one.
Runs out\\bf.exe of the tree it is in unless a bf.exe is given. Needs Pillow.
"""
import argparse, os, re, subprocess, sys, tempfile, time
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'programs', 'samples', 'raytracer.bfl')
W, H, SCALE, MS = 128, 64, 4, 30

# after the scene is set up: the light map, then N frames, each dumped to stdout
DUMP = r'''
// read the framebuffer back and write it to stdout, one RRRGGGBB byte per pixel
proc dump() {
    byte y;
    for (y = 0; y < H; y++) {
        byte x;
        for (x = 0; x < W; x++) {
            FB_X = x;
            FB_Y = y;
            pulse FB_READ;
            byte c = FB_COLOR;
            out c;
        }
    }
}

prepare_lights();
build_light_map();
word frame;
for (frame = 0; frame < 360 / STEP; frame++) {
    place_camera(30 + frame * STEP);            // one full orbit, as the sample turns
    render();
    dump();
}
'''

def capture_program():
    """the sample up to its banner, followed by DUMP; and the number of frames in an orbit"""
    s = open(SRC, encoding='utf-8').read()
    run = s.index('// ---- run ---')
    loop = s.index('print "BFL raytracer', run)
    step = int(re.search(r'const STEP = (\d+);', s).group(1))
    return s[:loop] + DUMP, 360 // step

def rgb(v):                       # as FramebufferDevice::colour in src/hw.h
    return ((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3)

def main():
    ap = argparse.ArgumentParser(description='Render the raytracer sample into an animated GIF.')
    ap.add_argument('out', help='the GIF to write')
    ap.add_argument('bf', nargs='?', default=os.path.join(ROOT, 'out', 'bf.exe'))
    ap.add_argument('--png', help='also save one frame as this PNG')
    ap.add_argument('--frame', type=int, default=0, help='the frame --png saves')
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'rt_capture.bfl')
        text, N = capture_program()
        open(path, 'w', encoding='utf-8', newline='\n').write(text)
        t = time.time()
        r = subprocess.run([a.bf, '--quiet-devices', path], capture_output=True, timeout=1200)
        took = time.time() - t
    data = r.stdout
    if r.returncode != 0 or len(data) != N * W * H:
        sys.exit(f'bf exited with {r.returncode} after {len(data)} bytes of output: '
                 + r.stderr.decode('latin-1').strip())
    print(f'rendered {N} frames in {took:.1f} s (light map and framebuffer read-back included)')

    palette = []
    for v in range(256):
        palette.extend(rgb(v))
    frames = []
    for f in range(N):
        im = Image.frombytes('P', (W, H), data[f * W * H:(f + 1) * W * H])
        im.putpalette(palette)
        frames.append(im.resize((W * SCALE, H * SCALE), Image.NEAREST))
    frames[0].save(a.out, save_all=True, append_images=frames[1:], duration=MS, loop=0,
                   optimize=False)
    print('wrote', a.out)
    if a.png:
        frames[a.frame].convert('RGB').save(a.png)
        print('wrote', a.png)

if __name__ == '__main__':
    main()
