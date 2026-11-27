#!/usr/bin/env python3
"""The Stiletto's cockpit, made from the game's own cockpits (docs/privateer.md section 42).

The user (2026-11-27): "It would look like a very cramped version of the centurion cockpit with 2 VDUs instead of
one, very much like the epee ... manufature at runtime the actual cockpit from the centurion and orion pieces ... the
location of vdus etc can be takend from the stiletto_cp folder it's just the art looks too cartooney so should come
from this game being cut and pasted instead."

This script is the recipe's source: a list of cuts (RECIPE) from the Centurion's and the Orion's forward views and
where the instruments sit (LAYOUT).  It draws the result from a PRIV.TRE for looking at (`stilcock.py <PRIV.TRE>
<out.png>`), and writes the same recipe as a C++ table (`--header src/cpu/wcnet_priv_stiletto.h`) that the emulator
follows at run time on the player's own game files (src/cpu/wcnet_priv_cockpit.cpp): none of the game's art is in
this repository or in the emulator, only how to cut it.
"""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from privart import *

W, H = 320, 200
C, O, T = 0, 1, 2                       # the sources: the Centurion's, the Orion's, the Tarsus's forward view
SOURCES = ['FIGHTCK', 'TUGCK', 'CLUNKCK']

# ---- the recipe: (op, source, sx, sy, w, h, dx, dy) -------------------------------------------------------------------
# COPY: the source's pixels (where it has none the picture stays); PUT: the rectangle as it is (none where the source
# has none); FILL: the rectangle (dx, dy, w, h) in the colour of the source's pixel (sx, sy); CLEAR: none.
COPY, PUT, FILL, CLEAR = 0, 1, 2, 3
RECIPE = []
def copy(src, sx, sy, w, h, dx, dy): RECIPE.append((COPY, src, sx, sy, w, h, dx, dy))
def put(src, sx, sy, w, h, dx, dy): RECIPE.append((PUT, src, sx, sy, w, h, dx, dy))
def fill(src, sx, sy, dx, dy, w, h): RECIPE.append((FILL, src, sx, sy, w, h, dx, dy))
def clear(dx, dy, w, h): RECIPE.append((CLEAR, 0, 0, 0, w, h, dx, dy))

def strips(op, src, sx, sy, w, h, dx, dy, drop_cols=(), drop_rows=()):
    """A rectangle with some of its columns and rows left out (a frame made narrower between its buttons)."""
    xs = [x for x in range(sx, sx + w) if x not in drop_cols]
    ys = [y for y in range(sy, sy + h) if y not in drop_rows]
    def runs(v):
        out = []
        for a in v:
            if out and out[-1][0] + out[-1][1] == a: out[-1][1] += 1
            else: out.append([a, 1])
        return out
    oy = dy
    for y0, hh in runs(ys):
        ox = dx
        for x0, ww in runs(xs):
            op(src, x0, y0, ww, hh, ox, oy); ox += ww
        oy += hh

def frame(src, box, rim, dx, dy, w, h):
    """A small frame (the source's `box` = x, y, w, h with a rim of `rim` pixels) redrawn at another size: its corners
    as they are, its edges repeated."""
    bx, by, bw, bh = box
    for (sy0, sh, oy0, oh) in ((by, rim, dy, rim), (by + bh - rim, rim, dy + h - rim, rim)):
        put(src, bx, sy0, rim, sh, dx, oy0); put(src, bx + bw - rim, sy0, rim, sh, dx + w - rim, oy0)
        x = dx + rim
        while x < dx + w - rim:
            n = min(bw - 2 * rim, dx + w - rim - x); put(src, bx + rim, sy0, n, sh, x, oy0); x += n
    y = dy + rim
    while y < dy + h - rim:
        n = min(bh - 2 * rim, dy + h - rim - y)
        put(src, bx, by + rim, rim, n, dx, y); put(src, bx + bw - rim, by + rim, rim, n, dx + w - rim, y); y += n

# The Centurion's VDU frame: 98 x 86 at (114, 102), its screen at (124, 112)-(202, 176), its border 10 wide.
VDU = (114, 102, 98, 86)
VDU_DROP = (139, 158, 167, 185)         # four columns between its buttons left out: 94 wide, the screen 74
VDU_Y = 96                              # (low: the user, "shove the cockpit down to obscure the keyboards to give us the better
                                        # visibility", then "shift it up just a little bit ... only 1/2 as much as you moved it down.
                                        # Otherwise it looks a bit \" close\"")
VDU_LX, VDU_RX = -5, 231                # (the outer rims run off the screen's edges: cramped)
BOX = (74, 105, 34, 10)                 # the Centurion's small readout frame (its fuel gauge's), rim 2
BLOCK = (89, 122, 142, 66)              # the middle housing between the VDUs: the VDU's frame again, made wider
DARK = (120, 150)                       # a pixel of the frame's dark face

def bezel(dx, dy, w, h):
    """The VDU's frame at another size: its corners as they are, its sides repeated or cut short."""
    x0, y0, bw, bh = VDU; b = 10
    for sy, oy in ((y0, dy), (y0 + bh - b, dy + h - b)):
        put(C, x0, sy, b, b, dx, oy); put(C, x0 + bw - b, sy, b, b, dx + w - b, oy)
        mid = w - 2 * b; first = mid % (bw - 2 * b) if mid > bw - 2 * b else 0
        x = dx + b
        if first: put(C, x0 + b, sy, first, b, x, oy); x += first
        while x < dx + w - b:
            n = min(bw - 2 * b, dx + w - b - x); put(C, x0 + b, sy, n, b, x, oy); x += n
    y = dy + b
    while y < dy + h - b:
        n = min(bh - 2 * b, dy + h - b - y)
        put(C, x0, y0 + b, b, n, dx, y); put(C, x0 + bw - b, y0 + b, b, n, dx + w - b, y); y += n

# 1. the canopy: the Centurion's hoop and struts, lower, and taller (the user: "The hoop might need to be stretched a
#    bit taller to match the example file"): each column slid down, the middle by HOOP_DOWN rows and the sides by up
#    to HOOP_MORE rows more, so the hoop's band keeps its thickness and its ends come down onto the VDUs' outer
#    corners as the example's do.  The box under its top (the Centurion's energy gauge) blank.
HOOP_DOWN, HOOP_MORE = 24, 26
def hoop_drop(x):
    d = abs(2 * x - 319)                # twice the distance from the middle, 1..319
    return HOOP_DOWN + (HOOP_MORE * d * d * d * d + 319 ** 4 // 2) // 319 ** 4
x = 0
while x < 320:
    n = 1
    while x + n < 320 and hoop_drop(x + n) == hoop_drop(x): n += 1
    copy(C, x, 0, n, 102, x, hoop_drop(x)); x += n
fill(C, *DARK, 135, 25 + HOOP_DOWN, 56, 7)
#STUBS      # (the struts' stubs over the VDUs' tops)
# 2. what shows of the dashboard beside and under the VDUs: the Centurion's own
copy(C, 0, 104, 89, 96, 0, 104)
copy(C, 231, 104, 89, 96, 231, 104)
# 3. the two VDUs
strips(put, C, *VDU, VDU_LX, VDU_Y, drop_cols=VDU_DROP)
strips(put, C, *VDU, VDU_RX, VDU_Y, drop_cols=VDU_DROP)
# 4. the middle housing on a dark plate
bx, by, bw, bh = BLOCK
fill(C, *DARK, bx, by, bw, 200 - by)
bezel(bx, by, bw, bh)
clear(bx + 10, by + 10, bw - 20, bh - 20)
# 5. its three bays: the bars, the scope, the shields; a rib of the frame's rim between them
INX, INY, INW, INH = bx + 10, by + 10, bw - 20, bh - 20
BARS_W, RADAR_W, SHLD_W = 28, 48, 41
RIB = (INW - BARS_W - RADAR_W - SHLD_W) // 2
RADAR = (INX + BARS_W + RIB, INY + (INH - 36) // 2, RADAR_W, 36)
SHLD = (RADAR[0] + RADAR_W + RIB, INY + (INH - 38) // 2, SHLD_W, 38)
for rx in (RADAR[0] - RIB, SHLD[0] - RIB):
    put(C, 115, 116, RIB, INH, rx, INY)
FUEL = (INX + 1, INY + 11, 13, 24)      # upright bars: the Orion's fuel gauge, the Tarsus's energy gauge
ENER = (INX + 15, INY + 11, 13, 24)
# 6. over the housing, against space: the autopilot's light, and the speeds as the Centurion shows them, on the
#    glass (the user: "The gray box around the speed readouts looks a bit weird. Maybe let those readouts be
#    floating directly against the background")
AUTO = (147, by - 7, 26, 5)
SET = (96, by - 9, 0, 0)                # (their text's left end and top)
KPS = (184, by - 9, 0, 0)

LAYOUT = dict(
    vdu_left=(VDU_LX + 10, VDU_Y + 10, VDU_LX + 84, VDU_Y + 74),
    vdu_right=(VDU_RX + 10, VDU_Y + 10, VDU_RX + 84, VDU_Y + 74),
    radar=RADAR, shield=SHLD, fuel=FUEL, energy=ENER, set=SET, kps=KPS, auto=AUTO)
SIGHT = (-19, -7)                       # the cockpit's OFFS: the gunsight's rows above the screen's middle (the Centurion's -25)

# ---- drawing it -------------------------------------------------------------------------------------------------------
def view_pixels(tre, ck, view='FRNT'):
    root = iff_parse(tre['COCKPITS\\%s.IFF' % ck])[0]
    v = root.find(view, 'SHAP').data
    return shape_decode(v, pack_shapes(v)[0])[1]

def compose(tre):
    src = [view_pixels(tre, n) for n in SOURCES]
    out = {}
    for op, s, sx, sy, w, h, dx, dy in RECIPE:
        if op == FILL: c = src[s][sx, sy]
        for j in range(h):
            for i in range(w):
                x, y = dx + i, dy + j
                if not (0 <= x < W and 0 <= y < H): continue
                if op == FILL: out[x, y] = c
                elif op == CLEAR: out.pop((x, y), None)
                else:
                    p = src[s].get((sx + i, sy + j))
                    if p is not None: out[x, y] = p
                    elif op == PUT: out.pop((x, y), None)
    return out

def window(pix):
    """The view's template: every place the picture has no pixel (space and the instruments both show there)."""
    return set((x, y) for y in range(H) for x in range(W) if (x, y) not in pix)

def rect(x, y, w, h): return struct.pack('<4h', x, y, x + w, y + h)
def moved(info, dx, dy): return struct.pack('<4h', *[v + (dx, dy)[i & 1] for i, v in enumerate(struct.unpack('<4h', info))])

def cockpit_iff(tre):
    """The Stiletto's cockpit file: the forward view made by the recipe, the other views the Centurion's, the
    instruments from the game's own cockpits at LAYOUT's places."""
    cent = iff_parse(tre['COCKPITS\\FIGHTCK.IFF'])[0]
    tug = iff_parse(tre['COCKPITS\\TUGCK.IFF'])[0]
    clunk = iff_parse(tre['COCKPITS\\CLUNKCK.IFF'])[0]
    pix = compose(tre); win = window(pix)
    spans = []
    for y in range(H):
        x = 0
        while x < W:
            if (x, y) in win:
                x0 = x
                while x < W and (x, y) in win: x += 1
                spans.append((x0, y, x - x0))
            else: x += 1
    rows = max(s[1] for s in spans) + 1
    frnt = Chunk('FORM', form='FRNT', kids=[
        Chunk('SHAP', pack_build([shape_encode((W - 1, 0, 0, H - 1), pix)])),
        Chunk('TPLT', tplt_encode((2, W, rows, 0, 0), spans))])
    L = LAYOUT
    def amfd(r, a, b): return Chunk('FORM', form='AMFD', kids=[Chunk('INFO', struct.pack('<4h', *r) + bytes([0, a, b]))])
    cmfd = Chunk('FORM', form='CMFD', kids=[amfd(L['vdu_left'], 0x1a, 0x6c), amfd(L['vdu_right'], 0x1b, 0x72), cent.find('CMFD', 'SOFT')])
    chud_c = cent.find('CHUD')
    hinf = chud_c.find('HINF').data[:9] + bytes(8)              # the colour, the forward view's box; no turret's
    hx, hy = struct.unpack_from('<2h', hinf, 1)                 # (the speeds' places are from the box's corner)
    hsft = Chunk('FORM', form='HSFT', kids=[])
    for k in chud_c.find('HSFT').kids:
        if k.form in ('SSPD', 'ASPD'):
            r = L['set'] if k.form == 'SSPD' else L['kps']
            k = Chunk('FORM', form=k.form, kids=[Chunk('INFO', struct.pack('<2h', r[0] - hx, r[1] - hy) + k.find('INFO').data[4:])])
        hsft.kids.append(k)
    chud = Chunk('FORM', form='CHUD', kids=[Chunk('HINF', hinf), hsft, chud_c.find('DAMG')])
    def centre(info): x1, y1, x2, y2 = struct.unpack('<4h', info); return (x1 + x2) // 2, (y1 + y2) // 2
    def gauge(view, r):
        """A gauge's VIEW moved so that its box's middle is the bay's."""
        info = view.find('INFO').data; cx, cy = centre(info)
        dx, dy = r[0] + r[2] // 2 - cx, r[1] + r[3] // 2 - cy
        d = view.find('DATA').data
        return Chunk('FORM', form='VIEW', kids=[Chunk('INFO', moved(info, dx, dy)), Chunk('DATA', d[:1] + moved(d[1:9], dx, dy)), view.find('SHAP')])
    dc = cent.find('DIAL')
    sh = dc.find('SHLD'); scx, scy = L['shield'][0] + L['shield'][2] // 2, L['shield'][1] + L['shield'][3] // 2
    auto = dc.find('AUTO'); acx, acy = centre(auto.find('INFO').data)
    ar = L['auto']
    dial = Chunk('FORM', form='DIAL', kids=[
        Chunk('FORM', form='RADR', kids=[Chunk('INFO', rect(*L['radar']))]),
        Chunk('FORM', form='SHLD', kids=[Chunk('INFO', struct.pack('<4h', scx - 20, scy - 18, scx + 21, scy + 19)), sh.find('DATA'), sh.find('SHAP')]),
        Chunk('FORM', form='ENER', kids=[gauge(clunk.find('DIAL', 'ENER', 'VIEW'), L['energy'])]),
        Chunk('FORM', form='FUEL', kids=[gauge(tug.find('DIAL', 'FUEL', 'VIEW'), L['fuel'])]),
        Chunk('FORM', form='AUTO', kids=[Chunk('INFO', moved(auto.find('INFO').data, ar[0] + ar[2] // 2 - acx, ar[1] + ar[3] // 2 - acy)), auto.find('SHAP')])])
    root = Chunk('FORM', form='COCK', kids=[frnt, cent.find('RITE'), cent.find('BACK'), cent.find('LEFT'),
                                           Chunk('OFFS', struct.pack('<2h', *SIGHT)), cent.find('FONT'),
                                           cmfd, chud, dial, cent.find(('DAMG', 0)), cent.find('CDMG')])
    return iff_build(root)

def appearance_iff(tre, name='STILETTO'):
    """The ship's appearance with a picture for the weapons display (FORM MFDS's WEAP: a ship no player flew has
    none, and the display reads it -- the game's error 150E): its own target-display drawing, the first four parts
    (the two halves, the body, the engines) as one shape, nose up, in the colour the Centurion's picture uses most."""
    root = iff_parse(tre['APPEARNC\\%s.IFF' % name])[0]
    mfds = root.find('BM3D', 'MFDS')
    if any(k.tag == 'WEAP' for k in mfds.kids): return iff_build(root)
    cw = iff_parse(tre['APPEARNC\\FIGHTER.IFF'])[0].find('BM3D', 'MFDS', 'WEAP').data
    used = list(shape_decode(cw, pack_shapes(cw)[0])[1].values())
    colour = max(sorted(set(used)), key=used.count)
    targ = mfds.find('TARG').data
    offs = pack_shapes(targ)
    ext = None; pix = {}
    for o in offs[:4]:
        e, pts = shape_decode(targ, o)
        ext = e if ext is None else tuple(max(a, b) for a, b in zip(ext, e))
        for (x, y) in pts: pix[x, -y] = colour
    mfds.kids.append(Chunk('WEAP', pack_build([shape_encode((ext[0], ext[1], ext[3], ext[2]), pix)])))
    return iff_build(root)

def header():
    """The recipe as the emulator's table."""
    L = LAYOUT
    out = ['// Made by scripts/privships/stilcock.py --header: the Stiletto cockpit\'s recipe (docs/privateer.md section 42).',
           '// Do not edit: change the script and run it again.',
           '',
           '// A cut: op (0 the source\'s pixels copied, 1 the rectangle put as it is -- none where the source has none,',
           '// 2 the rectangle filled with the colour of the source\'s pixel (sx, sy), 3 cleared), the source view (0 the',
           '// Centurion\'s forward view, 1 the Orion\'s, 2 the Tarsus\'s), its place there, the size, the place here.',
           'static const PrivCut kStilettoCuts[] = {']
    for c in RECIPE: out.append('    { %d, %d, %d, %d, %d, %d, %d, %d },' % c)
    out.append('};')
    out.append('static const PrivCockpitLayout kStilettoLayout = {')
    out.append('    { %d, %d, %d, %d }, { %d, %d, %d, %d },     // the left and the right VDU: x1, y1, x2, y2' % (L['vdu_left'] + L['vdu_right']))
    for k in ('radar', 'shield', 'fuel', 'energy', 'set', 'kps', 'auto'):
        out.append('    { %d, %d, %d, %d },     // %s: x, y, w, h' % (L[k] + (k,)))
    out.append('    { %d, %d },     // the gunsight (OFFS)' % SIGHT)
    out.append('};')
    return '\n'.join(out) + '\n'

if __name__ == '__main__':



    a = sys.argv[1:]
    if a[0] == '--header':
        open(a[1], 'w').write(header()); sys.exit(0)
    if a[0] == '--files':               # --files <PRIV.TRE> <dir>: STILCK.IFF and .PAK written there (a rig's: WCPRIVCKDIR)
        tre = tre_load(a[1])
        open(a[2] + '/STILCK.IFF', 'wb').write(cockpit_iff(tre))
        open(a[2] + '/STILCK.PAK', 'wb').write(tre['COCKPITS\\FIGHTCK.PAK'])
        open(a[2] + '/STILETTO.IFF', 'wb').write(appearance_iff(tre))
        sys.exit(0)
    from PIL import Image, ImageDraw
    tre = tre_load(a[0]); pal = pal_load(tre['PALETTE\\SPACE.PAL'])
    pix = compose(tre); win = window(pix)
    im = Image.new('RGB', (W, H), (0, 0, 0))
    for (x, y) in win: im.putpixel((x, y), (0, 0, 70))
    for k, r in LAYOUT.items():
        x1, y1, x2, y2 = r if k.startswith('vdu') else (r[0], r[1], r[0] + r[2], r[1] + r[3])
        for yy in range(y1, y2):
            for xx in range(x1, x2):
                if (xx, yy) in win: im.putpixel((xx, yy), (0, 0, 0))
    for (x, y), c in pix.items(): im.putpixel((x, y), pal[c])
    z = 4; big = im.resize((W * z, H * z), Image.NEAREST)
    if len(a) > 2 and a[2] == 'rects':
        d = ImageDraw.Draw(big)
        for k, r in LAYOUT.items():
            x1, y1, x2, y2 = r if k.startswith('vdu') else (r[0], r[1], r[0] + r[2], r[1] + r[3])
            d.rectangle((x1 * z, y1 * z, x2 * z - 1, y2 * z - 1), outline=(255, 255, 0))
    big.save(a[1]); print(len(RECIPE), 'cuts')
