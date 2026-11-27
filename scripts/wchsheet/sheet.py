#!/usr/bin/env python3
# sheet.py: the contact sheets of the VDUs' ship pictures (docs/wc-hires.md), the game's pixels (2 x 2, level 1) beside
# the wireframe (level 2), drawn by the game's own code (src/cpu/wcnet_wire.cpp, built into wiretool here):
#   WC1's target display (every SHIPTYPE / SHIP file's three-shape table: the hull 0 and the systems 2 at one anchor),
#   WC1's weapons display (every PCSHIP file's tenth table, the ship 0), WC2's target display (its SHIP files).
# The palettes are the flight's, written by a native run with WCHIRESPARTS=1 (palette.bin in WCNET_DUMP_DIR, 256 x RGB
# 0..63): the game's data, not kept in the repository.
#   scripts/wchsheet/sheet.py OUT_DIR WC1_PALETTE WC2_PALETTE [WC1_DIR WC2_DIR]
# The user approves the sheets before the ships are drawn so in the game (2026-11-27); once approved, later sheets are
# compared with the approved ones pixel for pixel.
import os, sys, glob, struct, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from wcshapes import container, shapes
from PIL import Image, ImageDraw
OUT = sys.argv[1]
TOOL = os.path.join(OUT, 'wiretool')
if not os.path.exists(TOOL):
    subprocess.check_call(['g++', '-O2', '-std=gnu++11', '-I', os.path.join(ROOT, 'include'), '-I', os.path.join(ROOT, 'build-native'),
                           '-I', os.path.join(ROOT, 'src', 'cpu'), '-o', TOOL, os.path.join(HERE, 'wiretool.cpp'),
                           os.path.join(ROOT, 'src', 'cpu', 'wcnet_wire.cpp')])
WC1_NAMES = ['Hornet', 'Rapier', 'Scimitar', 'Raptor', 'Venture', 'Dilligent', 'Drayman', 'Exeter', "Tiger's Claw", 'Salthi',
             'Dralthi', 'Krant', 'Gratha', 'Jalthi', 'Spikeri', 'Dorkir', 'Lumbari', 'Ralari', 'Fralthi', 'Snakeir', 'Sivar', 'Star post']

def pal(path):
    d = open(path, 'rb').read()
    return [(d[3*i] << 2, d[3*i+1] << 2, d[3*i+2] << 2) for i in range(256)]

def compose(parts):
    """Shapes drawn at one anchor, in order: (w, h, px list) of their union."""
    parts = [p for p in parts if p]
    x0 = min(-p[2] for p in parts); y0 = min(-p[3] for p in parts)
    x1 = max(p[0] - p[2] for p in parts); y1 = max(p[1] - p[3] for p in parts)
    w, h = x1 - x0, y1 - y0
    px = [-1] * (w * h)
    for (sw, sh, l, t, d) in parts:
        for (x, y), c in d.items():
            X, Y = x - l - x0, y - t - y0
            if 0 <= X < w and 0 <= Y < h: px[Y * w + X] = c
    return w, h, px

def wire(w, h, px):
    data = struct.pack('<hh', w, h) + struct.pack('<%dh' % (w * h), *px)
    out = subprocess.run([TOOL], input=data, capture_output=True).stdout
    return list(struct.unpack('<%dh' % (4 * w * h), out[:8 * w * h]))

def panel(w, h, px, fine, p, z):
    a = Image.new('RGB', (2 * w, 2 * h), (0, 0, 0))
    b = Image.new('RGB', (2 * w, 2 * h), (0, 0, 0))
    for y in range(h):
        for x in range(w):
            c = px[y * w + x]
            if c >= 0:
                for k in range(4): a.putpixel((2 * x + (k & 1), 2 * y + (k >> 1)), p[c])
    for Y in range(2 * h):
        for X in range(2 * w):
            c = fine[Y * 2 * w + X]
            if c >= 0: b.putpixel((X, Y), p[c])
    return a.resize((2 * w * z, 2 * h * z), Image.NEAREST), b.resize((2 * w * z, 2 * h * z), Image.NEAREST)

def target_table(c):
    """The target display's table: three shapes, the hull and its other state the same size (fighters: the second
    table; capital ships: the last)."""
    for fl, blk in c:
        try: sh = shapes(blk)
        except Exception: continue
        if len(sh) == 3 and sh[0] and sh[1] and sh[2] and sh[0][:4] == sh[1][:4]: return sh
    return None

def sheet(entries, p, title, out, z=2, cols=3):
    cells = []
    for name, (w, h, px) in entries:
        a, b = panel(w, h, px, wire(w, h, px), p, z)
        cells.append((name, a, b))
    cw = max(c[1].width * 2 + 30 for c in cells); ch = max(c[1].height for c in cells) + 30
    rows = (len(cells) + cols - 1) // cols
    img = Image.new('RGB', (cols * cw, rows * ch + 40), (40, 40, 40)); d = ImageDraw.Draw(img)
    d.text((8, 10), title, fill=(255, 255, 255))
    for i, (name, a, b) in enumerate(cells):
        x = (i % cols) * cw + 8; y = (i // cols) * ch + 40
        d.text((x, y), name + '   (left: the game, as level 1;  right: the wireframe, level 2)' if i == 0 else name, fill=(230, 230, 230))
        img.paste(a, (x, y + 16)); img.paste(b, (x + a.width + 12, y + 16))
    img.save(out)
    print(out, img.size, len(cells))

G1 = os.path.join(sys.argv[4] if len(sys.argv) > 4 else os.path.join(ROOT, 'wc'), 'GAMEDAT') + '/'
G2 = os.path.join(sys.argv[5] if len(sys.argv) > 5 else os.path.join(ROOT, 'wc2'), 'GAMEDAT') + '/'
p1 = pal(sys.argv[2]); p2 = pal(sys.argv[3])
# WC1, the target display (horizontal): each ship's second table, its hull (0) and systems (2) at one anchor
h1 = []
for f in sorted(glob.glob(G1 + 'SHIPTYPE.V*')) + sorted(glob.glob(G1 + 'SHIP.V*')):
    n = int(f[-2:]); c = container(open(f, 'rb').read())
    sh = target_table(c)
    if not sh: continue
    h1.append(('%s (%s)' % (WC1_NAMES[n] if n < len(WC1_NAMES) else '?', f.split('/')[-1]), compose([sh[0], sh[2]])))
sheet(h1, p1, 'Wing Commander 1: the target display (horizontal)', os.path.join(OUT, 'wc1-horizontal.png'), cols=4)
# WC1, the weapons display (vertical): each player ship's tenth table, its ship (0)
v1 = []
for f in sorted(glob.glob(G1 + 'PCSHIP.V*')):
    n = int(f[-2:]); c = container(open(f, 'rb').read())
    try: sh = shapes(c[9][1])
    except Exception: continue
    nm = WC1_NAMES[n] if n < 4 else {4: 'Hornet, another copy', 5: 'Dralthi (The Secret Missions\' captured one)'}.get(n, 'player ship %d' % n)
    v1.append(('%s (%s)' % (nm, f.split('/')[-1]), compose([sh[0]])))
sheet(v1, p1, 'Wing Commander 1: the weapons display (vertical)', os.path.join(OUT, 'wc1-vertical.png'), cols=3)
# WC2, the target display (horizontal)
h2 = []
import re
for f in sorted(glob.glob(G2 + 'SHIP.V*')):
    d = open(f, 'rb').read(); c = container(d)
    sh = target_table(c)
    if not sh: continue
    # (the name: the ship's record, the 243-byte table, at its start)
    nm = '?'
    for fl, blk in c:
        if len(blk) == 243 and 0 < blk.find(b'\x00') <= 20:
            nm = blk[:blk.find(b'\x00')].decode('latin-1').strip(); break
    h2.append(('%s (%s)' % (nm, f.split('/')[-1]), compose([sh[0], sh[2]])))
sheet(h2, p2, 'Wing Commander II: the target display (horizontal)', os.path.join(OUT, 'wc2-horizontal.png'), cols=4)
