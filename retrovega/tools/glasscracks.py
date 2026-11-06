#!/usr/bin/env python3
"""A cockpit's canopy damage layer as cracks in its glass (the user, 2026-11-05: the Schroedinger's "diagonal canopy
cracks ... don't really cover any kind of real glass they seem to be covering the metal struts which doesn't make
sense...we can remove that particular damage area, or add some subtle cracks somewhere on the windshield").

    glasscracks.py KEY ...

cockpit.json's canopy damage entry (id canopy) says where the glass was hit: "glass": [[x, y, arms, reach, [ring, ...]],
...] (screen pixels: an impact's place, how many cracks run from it, how far the longest goes, the radii where chords
of the web join neighbouring cracks) and "glass_seed".  Each impact is a star of straight cracks with two or three sharp
kinks and a branch now and then, chords between neighbours at the rings, a pixel wide, grey-blue, brighter at the
impact; kept to the glass (where base.png is open, 3 pixels in from the frame) with a pixel left out here and there.
Out: the entry's file (damage/23-canopy.png), cropped, and its x, y, w, h in cockpit.json.
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def star(cx, cy, rng, arms, reach, rings):
    """an impact's cracks: (points, brightness) a line"""
    lines = []
    ends = []
    for a in np.sort(rng.uniform(0, 2 * math.pi, arms)):
        length = rng.uniform(0.5, 1.0) * reach
        x, y = float(cx), float(cy)
        path = [(x, y)]
        kinks = int(rng.integers(2, 4))
        for _k in range(kinks):
            seg = length / kinks * rng.uniform(0.7, 1.3)
            x += math.cos(a) * seg
            y += math.sin(a) * seg
            path.append((x, y))
            if rng.random() < 0.4:      # a branch off this kink
                b = a + rng.choice([-1, 1]) * rng.uniform(0.35, 0.7)
                bl = rng.uniform(0.15, 0.3) * reach
                lines.append(([(x, y), (x + math.cos(b) * bl, y + math.sin(b) * bl)], 0.55))
            a += rng.normal(0, 0.16)
        lines.append((path, 1.0))
        ends.append(path)

    def at(path, r):                    # where a crack is r from the impact
        for (x0, y0), (x1, y1) in zip(path, path[1:]):
            d0, d1 = math.hypot(x0 - cx, y0 - cy), math.hypot(x1 - cx, y1 - cy)
            if d0 <= r <= d1 and d1 > d0:
                t = (r - d0) / (d1 - d0)
                return (x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
        return None
    for r in rings:                     # the web: a straight chord between neighbouring cracks, at about r
        for i in range(len(ends)):
            if rng.random() < 0.6:
                rr = r * rng.uniform(0.85, 1.15)
                u, v = at(ends[i], rr), at(ends[(i + 1) % len(ends)], rr)
                if u and v and math.hypot(u[0] - v[0], u[1] - v[1]) < 2.2 * rr:
                    lines.append(([u, v], 0.5))
    return lines


def cracks(base, impacts, seed):
    glass = ndimage.binary_erosion(np.asarray(base)[..., 3] < 128, iterations=3)
    rng = np.random.default_rng(seed)
    layer = Image.new('RGBA', base.size, (0, 0, 0, 0))
    dr = ImageDraw.Draw(layer)
    for cx, cy, arms, reach, rings in impacts:
        for path, bright in star(cx, cy, rng, arms, reach, rings):
            for (x0, y0), (x1, y1) in zip(path, path[1:]):
                k = bright * (1.0 - 0.55 * min(1.0, math.hypot((x0 + x1) / 2 - cx, (y0 + y1) / 2 - cy) / reach))
                dr.line([(x0, y0), (x1, y1)], fill=(int(70 + 120 * k), int(80 + 125 * k), int(92 + 130 * k), 255))
        dr.point([(cx, cy), (cx + 1, cy), (cx, cy + 1)], fill=(215, 225, 235, 255))
    a = np.asarray(layer).copy()
    a[~glass] = 0
    a[rng.random(a.shape[:2]) < 0.08] = 0   # (a gap here and there)
    return Image.fromarray(a)


def build(key):
    d = os.path.join(ROOT, 'art', 'cockpits', key)
    path = os.path.join(d, 'cockpit.json')
    cj = json.load(open(path))
    e = next(x for x in cj['damage'] if x['id'] == 'canopy')
    base = Image.open(os.path.join(d, 'base.png')).convert('RGBA')
    img = cracks(base, e['glass'], e.get('glass_seed', 7))
    bb = img.getbbox()
    img.crop(bb).save(os.path.join(d, e['file']))
    e.update(x=bb[0], y=bb[1], w=bb[2] - bb[0], h=bb[3] - bb[1])
    json.dump(cj, open(path, 'w'), indent=1)
    return bb


if __name__ == '__main__':
    for key in sys.argv[1:]:
        print('glasscracks: %s at %s' % (key, build(key)))
