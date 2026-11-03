#!/usr/bin/env python3
"""A wreck's debris pictures (src/debris.c): chunks of armour cut from the plate's damage stamps (tools/plates.py,
art/places/plate/big_NN.png), the torn metal round the holes, each given a torn outline of its own.  The user
(2026-11-03): "we need some kind of photo source. Like those armor chunks in the interstitial ... those could be almost
reused directly. I guess they would need to tumble".  debris.c makes the tumble itself, as a flat piece of plating
turns: each chunk is kept once, its light only (the stamps' own, the plate's light), and an axis in its plane.
Writes src/debris_art.h (checked in); by hand: python3 -P tools/debris.py [--sheet FILE]."""
import argparse
import math
import os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
ART = os.path.join(ROOT, 'art', 'places', 'plate')
SIDE = 48                               # a chunk's picture, pixels a side
STAMPS = [0, 2, 3, 4, 7, 10, 12, 13]    # the stamps with the most bare metal (the rest are mostly wires and hole)
LEVELS = 15                             # a pixel's light, 1 .. 15 (0 clear)
SEED = 7


def metal_of(i):
    """a stamp's light and where it is bare metal (opaque, grey: not a wire, not a burn)"""
    im = np.asarray(Image.open(os.path.join(ART, 'big_%02d.png' % i)).convert('RGBA')).astype(float) / 255
    a, rgb = im[..., 3], im[..., :3]
    mx, mn = rgb.max(-1), rgb.min(-1)
    sat = (mx - mn) / (mx + 1e-3)
    return 0.3 * rgb[..., 0] + 0.59 * rgb[..., 1] + 0.11 * rgb[..., 2], (a > 0.6) & (sat < 0.22)


def torn(rng, aspect):
    """a torn outline: an uneven polygon, every edge notched, stretched along a way of its own (aspect: its width over
    its length; a strip of plating at the narrow end), as coverage 0 .. 1"""
    n = SIDE * 4
    img = Image.new('L', (n, n), 0)
    k, base, pts = int(rng.integers(7, 12)), rng.uniform(0.36, 0.46) * n, []
    way = rng.uniform(0, math.pi)
    for i in range(k * 3):
        a = 2 * math.pi * i / (k * 3) + rng.uniform(-0.08, 0.08)
        r = base * (rng.uniform(0.75, 1.1) if i % 3 == 0 else rng.uniform(0.55, 1.05))
        u, v = r * math.cos(a), r * math.sin(a) * aspect
        pts.append((n / 2 + u * math.cos(way) - v * math.sin(way), n / 2 + u * math.sin(way) + v * math.cos(way)))
    ImageDraw.Draw(img).polygon(pts, fill=255)
    return np.asarray(img.resize((SIDE, SIDE), Image.BOX)).astype(float) / 255


ASPECTS = [1.0, 0.45, 0.8, 0.35, 0.65, 0.9, 0.5, 0.75]   # (each chunk's: a few strips, a few broad pieces)


def chunk(stamp, rng, aspect):
    """the window of the stamp with the most metal (about three quarters of it), cut to a torn outline; its light
    stretched to the full range"""
    lum, metal = metal_of(stamp)
    h, w = lum.shape
    best, at = -1.0, (0, 0)
    for y in range(0, h - SIDE, 3):
        for x in range(0, w - SIDE, 3):
            c = metal[y:y + SIDE, x:x + SIDE].mean()
            score = c - abs(c - 0.75) * 0.5 + rng.uniform(0, 0.05)
            if score > best:
                best, at = score, (y, x)
    y, x = at
    light = lum[y:y + SIDE, x:x + SIDE]
    on = metal[y:y + SIDE, x:x + SIDE] * torn(rng, aspect) > 0.5
    lo, hi = np.percentile(light[on], 3), np.percentile(light[on], 97)
    level = 1 + np.clip((light - lo) / max(hi - lo, 1e-3), 0, 0.999) * LEVELS
    return np.where(on, np.clip(level.astype(int), 1, LEVELS), 0).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'src', 'debris_art.h'))
    ap.add_argument('--sheet', default=None, help='the chunks, 4x, as a picture')
    args = ap.parse_args()
    rng = np.random.default_rng(SEED)
    chunks, axes = [], []
    for i, s in enumerate(STAMPS):
        chunks.append(chunk(s, rng, ASPECTS[i]))
        axes.append(int(rng.integers(0, 256)))   # (its tumble's axis, 256 a half turn)
    lines = ['/* debris_art.h: a wreck\'s debris pictures, made by tools/debris.py from the plate\'s damage stamps',
             ' * (art/places/plate): do not edit.  Each chunk SIDE x SIDE, two pixels a byte (the left one low), each its',
             ' * light 1 .. 15, 0 clear; and the axis it tumbles about, in its plane, 256 a half turn */',
             '#define DB_ART_KINDS %d' % len(chunks), '#define DB_ART_SIDE %d' % SIDE,
             'static const uint8_t db_art_axis[DB_ART_KINDS] = { %s };' % ', '.join(map(str, axes)),
             'static const uint8_t db_art_light[DB_ART_KINDS][DB_ART_SIDE * DB_ART_SIDE / 2] = {']
    for c in chunks:
        flat = c.reshape(-1)
        packed = (flat[0::2] | (flat[1::2] << 4)).astype(np.uint8)
        lines.append('    {')
        for i in range(0, len(packed), 24):
            lines.append('        ' + ','.join('0x%02x' % b for b in packed[i:i + 24]) + ',')
        lines.append('    },')
    lines.append('};')
    with open(args.out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    if args.sheet:
        sheet = np.zeros((SIDE, SIDE * len(chunks)), np.uint8)
        for i, c in enumerate(chunks):
            sheet[:, i * SIDE:(i + 1) * SIDE] = c.astype(int) * 255 // LEVELS
        Image.fromarray(sheet).resize((sheet.shape[1] * 4, SIDE * 4), Image.NEAREST).save(args.sheet)
    print('debris.py: %d chunks from stamps %s -> %s' % (len(chunks), STAMPS, args.out))


if __name__ == '__main__':
    main()
