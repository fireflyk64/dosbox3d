#!/usr/bin/env python3
"""A cockpit's canopy damage as cracks in its glass, a set of them the engine puts at random places (the user,
2026-11-05: the Schroedinger's "diagonal canopy cracks ... don't really cover any kind of real glass they seem to be
covering the metal struts which doesn't make sense...we can remove that particular damage area, or add some subtle
cracks somewhere on the windshield"; 2026-11-06: "if they are in the DAT file can we have 6 more or so. Then load in
random ones at random places when those damages hit. Hopefully that makes it feel less wrote").

    glasscracks.py KEY ...          (art/cockpits/KEY; --dir DIR: a cockpit's directory elsewhere)

cockpit.json's canopy damage entry (id canopy) lists the cracks, "glass": [{"kind", "arms", "reach", "rings"}, ...],
and "glass_seed":
 - star: an impact, `arms` straight cracks from it with two or three sharp kinks and a branch now and then, chords of
   the web between neighbouring cracks at the radii `rings`;
 - fan: the same from a hit at a slant, its cracks over half a turn;
 - pit: a small chip, short cracks close together, a brighter middle;
 - run: a crack running across the glass from an edge's stress, no impact, `arms` branches off it.
Each a pixel wide, grey-blue, brighter near where it began, a pixel left out here and there.  Out: damage/23-crackN.png
(each cropped) and, in the entry, "cracks": [{"file", "x", "y", "places": [[x, y], ...]}, ...]: where the crack may be
put, its top left, every one with all of it on the glass (where base.png is open, 3 pixels in from the frame) and clear
of the gunsight (the opening's middle).  tools/rvpack.py packs them as the canopy layer's sprites in a row and after
them orders(): ORDERS random orders of cockpits.CRACKS cracks, each a crack not in it yet at the farthest of a few of
its places from those before it; a flight takes one at random and a crack more from it as the damage comes (cockpit.c
cracks_update: the choosing done here, none of it in the game's memory).
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PLACES = 12                             # places a crack
ORDERS = 64                             # orders of the cracks packed (rvpack.py: 64 x 6 x 6 bytes of RV.DAT)
TRIES = 6                               # places of a crack tried for an order's next: the farthest from those there
SIGHT_CLEAR = 40                        # pixels between a crack's reach and the gunsight


def path_lines(cx, cy, rng, angles, reach, kinks=(2, 4), branch=0.4, wobble=0.16):
    """cracks from (cx, cy) along the angles: (lines, ends)"""
    lines, ends = [], []
    for a in angles:
        length = rng.uniform(0.5, 1.0) * reach
        x, y = float(cx), float(cy)
        path = [(x, y)]
        n = int(rng.integers(*kinks))
        for _k in range(n):
            seg = length / n * rng.uniform(0.7, 1.3)
            x += math.cos(a) * seg
            y += math.sin(a) * seg
            path.append((x, y))
            if rng.random() < branch:   # a branch off this kink
                b = a + rng.choice([-1, 1]) * rng.uniform(0.35, 0.7)
                bl = rng.uniform(0.15, 0.3) * reach
                lines.append(([(x, y), (x + math.cos(b) * bl, y + math.sin(b) * bl)], 0.55))
            a += rng.normal(0, wobble)
        lines.append((path, 1.0))
        ends.append(path)
    return lines, ends


def web(cx, cy, rng, ends, rings, closed=True):
    """the chords between neighbouring cracks at about each radius"""
    def at(path, r):                    # where a crack is r from the impact
        for (x0, y0), (x1, y1) in zip(path, path[1:]):
            d0, d1 = math.hypot(x0 - cx, y0 - cy), math.hypot(x1 - cx, y1 - cy)
            if d0 <= r <= d1 and d1 > d0:
                t = (r - d0) / (d1 - d0)
                return (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
        return None
    lines = []
    for r in rings:
        for i in range(len(ends) if closed else len(ends) - 1):
            if rng.random() < 0.6:
                rr = r * rng.uniform(0.85, 1.15)
                u, v = at(ends[i], rr), at(ends[(i + 1) % len(ends)], rr)
                if u and v and math.hypot(u[0] - v[0], u[1] - v[1]) < 2.2 * rr:
                    lines.append(([u, v], 0.5))
    return lines


def crack(c, rng):
    """one crack about (0, 0): (lines, where it began)"""
    kind, arms, reach, rings = c['kind'], c.get('arms', 6), c['reach'], c.get('rings', [])
    if kind == 'star':
        lines, ends = path_lines(0, 0, rng, np.sort(rng.uniform(0, 2 * math.pi, arms)), reach)
        return lines + web(0, 0, rng, ends, rings), (0, 0)
    if kind == 'fan':                   # (a hit at a slant: half a turn of cracks, the web open on its back)
        way = rng.uniform(0, 2 * math.pi)
        lines, ends = path_lines(0, 0, rng, np.sort(way + rng.uniform(-1.6, 1.6, arms)), reach)
        return lines + web(0, 0, rng, ends, rings, closed=False), (0, 0)
    if kind == 'pit':                   # (a chip: short cracks, crowded)
        angles = np.sort(rng.uniform(0, 2 * math.pi, arms))
        lines, ends = path_lines(0, 0, rng, angles, reach, kinks=(1, 3), branch=0.25, wobble=0.3)
        return lines + web(0, 0, rng, ends, rings), (0, 0)
    if kind == 'run':                   # (a crack across the glass: one long kinked line, branches off it)
        way = rng.uniform(0, 2 * math.pi)
        x, y = -math.cos(way) * reach / 2, -math.sin(way) * reach / 2
        path, lines = [(x, y)], []
        n = int(rng.integers(6, 9))
        for _k in range(n):
            seg = reach / n * rng.uniform(0.7, 1.3)
            x += math.cos(way) * seg
            y += math.sin(way) * seg
            path.append((x, y))
            way += rng.normal(0, 0.22)
        for _b in range(arms):
            k = int(rng.integers(1, len(path) - 1))
            b = way + rng.choice([-1, 1]) * rng.uniform(0.5, 1.1)
            bl = rng.uniform(0.08, 0.2) * reach
            bx, by = path[k]
            lines.append(([(bx, by), (bx + math.cos(b) * bl, by + math.sin(b) * bl)], 0.6))
        lines.append((path, 1.0))
        return lines, path[0]
    sys.exit('glasscracks: no kind %r' % kind)


def draw(c, rng):
    """a crack's picture, cropped: (image, its pixels)"""
    reach = c['reach']
    m = reach + 8
    lines, (sx, sy) = crack(c, rng)
    layer = Image.new('RGBA', (2 * m, 2 * m), (0, 0, 0, 0))
    dr = ImageDraw.Draw(layer)
    for path, bright in lines:
        for (x0, y0), (x1, y1) in zip(path, path[1:]):
            far = math.hypot((x0 + x1) / 2 - sx, (y0 + y1) / 2 - sy) / (reach * (1.4 if c['kind'] == 'run' else 1.0))
            k = bright * (1.0 - 0.55 * min(1.0, far))
            dr.line([(x0 + m, y0 + m), (x1 + m, y1 + m)], fill=(int(70 + 120 * k), int(80 + 125 * k), int(92 + 130 * k), 255))
    if c['kind'] != 'run':
        dr.point([(m, m), (m + 1, m), (m, m + 1)], fill=(215, 225, 235, 255))
        if c['kind'] == 'pit':
            dr.point([(m - 1, m), (m, m - 1), (m + 1, m + 1)], fill=(200, 210, 222, 255))
    a = np.asarray(layer).copy()
    a[rng.random(a.shape[:2]) < 0.08] = 0   # (a gap here and there)
    img = Image.fromarray(a)
    bb = img.getbbox()
    img = img.crop(bb)
    return img, np.asarray(img)[..., 3] >= 128


def places(glass, lit, sight, reach, rng):
    """up to PLACES top-left corners where all of the crack is on the glass and clear of the sight, spread out"""
    h, w = lit.shape
    # (how much of the crack is on the glass at each top left: all of it, or not there)
    on = signal.fftconvolve(glass.astype(np.float32), lit[::-1, ::-1].astype(np.float32), mode='valid')
    ok = on > lit.sum() - 0.5
    ys, xs = np.nonzero(ok)
    cx, cy = xs + w / 2, ys + h / 2
    clear = np.hypot(cx - sight[0], cy - sight[1]) >= max(w, h) / 2 + SIGHT_CLEAR
    xs, ys = xs[clear], ys[clear]
    if not len(xs):
        return []
    got = [int(rng.integers(len(xs)))]
    d = np.hypot(xs - xs[got[0]], ys - ys[got[0]])
    while len(got) < PLACES:            # (the farthest from those picked: spread over the glass)
        j = int(np.argmax(d))
        if d[j] < 8:
            break
        got.append(j)
        d = np.minimum(d, np.hypot(xs - xs[j], ys - ys[j]))
    return [[int(xs[j]), int(ys[j])] for j in got]


def orders(e, n_cracks, seed):
    """ORDERS lists of n_cracks (crack, x, y): the cracks all different, each at the farthest of TRIES random places
    of its own from the middles before it (city-block)"""
    rng = np.random.default_rng(seed)
    cr = e['cracks']
    out = []
    for _o in range(ORDERS):
        got = []
        for k in rng.permutation(len(cr))[:n_cracks]:
            c = cr[int(k)]
            best = None
            for _t in range(TRIES):
                x, y = c['places'][int(rng.integers(len(c['places'])))]
                mx, my = x + c['w'] // 2, y + c['h'] // 2
                d = min([abs(mx - a) + abs(my - b) for _k, a, b in got] or [1 << 15])
                if best is None or d > best[0]:
                    best = (d, x, y, mx, my)
            got.append((int(k), best[3], best[4]))
            out.append((int(k), best[1], best[2]))
    return out


def build(d):
    path = os.path.join(d, 'cockpit.json')
    cj = json.load(open(path))
    e = next(x for x in cj['damage'] if x['id'] == 'canopy')
    base = Image.open(os.path.join(d, 'base.png')).convert('RGBA')
    opening = np.asarray(base)[..., 3] < 128
    glass = ndimage.binary_erosion(opening, iterations=3)
    rows = np.nonzero(opening.any(axis=1))[0]
    sight = (base.width / 2, (int(rows[0]) + int(rows[-1]) + 1) / 2)   # (flight.c: the camera's middle)
    rng = np.random.default_rng(e.get('glass_seed', 7))
    out = []
    for i, c in enumerate(e['glass']):
        img, lit = draw(c, rng)
        at = places(glass, lit, sight, c['reach'], rng)
        if not at:
            sys.exit('glasscracks: crack %d (%s, reach %d) fits nowhere on the glass' % (i, c['kind'], c['reach']))
        f = 'damage/23-crack%d.png' % i
        img.save(os.path.join(d, f))
        out.append({'file': f, 'x': at[0][0], 'y': at[0][1], 'w': img.width, 'h': img.height, 'places': at})
    e['cracks'] = out
    e.update(file=out[0]['file'], x=out[0]['x'], y=out[0]['y'], w=out[0]['w'], h=out[0]['h'])
    json.dump(cj, open(path, 'w'), indent=1)
    return [(o['w'], o['h'], len(o['places'])) for o in out]


if __name__ == '__main__':
    args = sys.argv[1:]
    if args[:1] == ['--dir']:
        print('glasscracks: %s: %s' % (args[1], build(args[1])))
    else:
        for key in args:
            print('glasscracks: %s: %s' % (key, build(os.path.join(ROOT, 'art', 'cockpits', key))))
