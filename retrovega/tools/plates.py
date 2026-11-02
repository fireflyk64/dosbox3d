#!/usr/bin/env python3
"""The armour plate Kit stands in front of when a fighter comes home (src/game.c, docs/damage.md): a clean plate, and damage cut out as
stamps the game lays over it, so the plate shows what happened on this flight (the user): a big stamp (a torn hole, its lips peeled back,
wires hanging out) for each system hurt, a small one (a puncture, a slash, a hit's splash, gouges) for each ten percent of the hull gone.
Drawn from a height field and lit, so everything shares one light, then bent: it is the flank of a fighter on the deck, curving away from
the camera and under, into the dark of its belly.  No rust and no soot: this happened in a vacuum, and it
was sliced (the user).  Each stamp is drawn in its own place on the plate, then whatever differs from the clean plate is cut out, so its
light and its bend are right where it lies.  Writes art/places/plate/: base.png, big_NN.png, small_NN.png, stamps.json.  python3 -P
tools/plates.py [DIR], by hand; the results are checked in.  With --sheet FILE too: every stamp at once, and a few flights' worth."""
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

EXTRA = 100                             # the plate is this much wider than the screen, at the left: it rolls into place behind Kit (game.c)
W, H, SS = 640 + EXTRA, 400, 2
FW, FH = W * SS, H * SS
FOOT_Y = 318                            # Kit's bust ends at row 62 + 256: the hull has gone under by then
CURVE_Y = 118                           # where the flank starts to turn away


def blur(a, r):
    """about a Gaussian: three box blurs each way"""
    w = max(1, int(round(r * 1.7)))
    out = a.astype(np.float32)
    for _ in range(3):
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (w + 1, w)
            c = np.cumsum(np.pad(out, pad, mode='edge'), axis=axis)
            out = (np.take(c, range(2 * w + 1, c.shape[axis]), axis=axis) - np.take(c, range(0, c.shape[axis] - 2 * w - 1), axis=axis)) / (2 * w + 1)
    return out


def noise(rng, scale, amp=1.0):
    """smooth noise, about -1 .. 1"""
    n = rng.standard_normal((FH // scale + 2, FW // scale + 2)).astype(np.float32)
    im = Image.fromarray(n, 'F').resize((FW, FH), Image.BICUBIC)
    a = np.asarray(im)
    return a / (a.std() + 1e-6) * amp


def fractal(rng, base, octaves=4):
    out, amp, sc = 0, 1.0, base
    for _ in range(octaves):
        out = out + noise(rng, max(1, sc), amp)
        amp *= 0.55
        sc = max(1, sc // 2)
    return out


YY, XX = np.mgrid[0:FH, 0:FW].astype(np.float32)
SEAMS_X = [(x + EXTRA) * SS for x in (-88, 112, 368, 600)]
SEAMS_Y = [y * SS for y in (58, 196)]


def base_height(rng):
    """the plate as it left the yard: panels, seams and rivets"""
    h = fractal(rng, 96, 3) * 0.35
    for sx in SEAMS_X:
        d = np.abs(XX - sx)
        h -= np.exp(-(d / 2.6) ** 2) * 7
        h += np.exp(-((d - 9) / 5.0) ** 2) * 0.9
    for sy in SEAMS_Y:
        d = np.abs(YY - sy)
        h -= np.exp(-(d / 2.6) ** 2) * 7
        h += np.exp(-((d - 9) / 5.0) ** 2) * 0.9
    r = np.zeros((FH, FW), np.float32)
    im = Image.new('L', (FW, FH), 0)
    dr = ImageDraw.Draw(im)
    rivets = []
    for sx in SEAMS_X:
        for k, y in enumerate(range(10, FH, 44)):
            rivets += [(sx - 22, y), (sx + 22, y)]
    for sy in SEAMS_Y:
        for x in range(10, FW, 44):
            rivets += [(x, sy - 22), (x, sy + 22)]
    for x, y in rivets:
        dr.ellipse([x - 7, y - 7, x + 7, y + 7], fill=255)
    r = blur(np.asarray(im, np.float32) / 255, 3.0) * 7
    return h + r


def stencils(rng):
    """the albedo's paint: gunmetal, a hazard band, stencilled numbers"""
    a = np.zeros((FH, FW, 3), np.float32)
    a[...] = (92, 100, 104)
    tone = fractal(rng, 64, 4)
    a *= (1 + tone[..., None] * 0.035)
    for k, sx in enumerate(SEAMS_X[:-1]):                   # panels a touch different from each other
        m = (XX > sx) & (XX < SEAMS_X[k + 1])
        a[m] *= (0.97, 1.0, 1.03)[k % 3]
    im = Image.new('L', (FW, FH), 0)
    dr = ImageDraw.Draw(im)
    for x in range(-200, FW + 200, 72):                     # the band under the top seam
        dr.polygon([(x, 124), (x + 36, 124), (x + 72, 168), (x + 36, 168)], fill=255)
    band = np.asarray(im, np.float32) / 255
    band_on = ((YY > 124) & (YY < 168)).astype(np.float32)
    colour = band[..., None] * np.array((178, 138, 44), np.float32) + (1 - band[..., None]) * np.array((26, 26, 28), np.float32)
    a = a * (1 - band_on[..., None]) + colour * band_on[..., None]
    return a


# the big stamps' places: x, y, radius in the screen's pixels: behind and beside Kit, above and among the words.  A cap: the game shows at most
# this many, whatever is hurt (src/game.c)
BIG = [(-52, 150, 30), (-70, 44, 24), (286, 96, 58), (452, 66, 44), (72, 62, 38), (556, 170, 32), (330, 244, 36), (182, 34, 26), (606, 58, 24), (492, 268, 26),
       (20, 210, 32), (392, 170, 22), (360, 28, 28), (540, 36, 28), (604, 252, 24), (426, 226, 22)]
# the small ones: (kind, x, y, size): a puncture, a slash (x1, y1 its other end), a splash, gouges
SMALL = [('hole', -40, 60, 10), ('slash', -84, 124, -24, 72, 3.0), ('splash', -60, 228, 18), ('hole', 306, 128, 13), ('hole', 486, 60, 11), ('hole', 92, 40, 10), ('hole', 574, 108, 9), ('hole', 356, 196, 11), ('hole', 16, 128, 11),
         ('slash', 300, 22, 372, 80, 3.5), ('slash', 470, 158, 540, 108, 3.0), ('slash', 340, 150, 400, 118, 2.6), ('slash', 584, 196, 632, 150, 2.8),
         ('slash', 18, 268, 58, 212, 3.0), ('slash', 408, 30, 450, 6, 2.5),
         ('splash', 216, 86, 26), ('splash', 540, 120, 26), ('splash', 428, 190, 20), ('splash', 130, 24, 18), ('splash', 610, 214, 18), ('splash', 372, 76, 18),
         ('gouge', 280, 44, 36), ('gouge', 500, 216, 34), ('gouge', 620, 30, 26), ('gouge', 330, 180, 30), ('gouge', 460, 120, 28), ('gouge', 30, 170, 26)]


def slash_field(rng, slashes):
    """a slash's distance field, as hole_mask's: negative inside, its edges torn"""
    j = fractal(rng, 10, 3)
    f = np.full((FH, FW), 1e4, np.float32)
    for x0, y0, x1, y1, w in slashes:
        ax, ay, bx, by = x0 * SS, y0 * SS, x1 * SS, y1 * SS
        dx, dy = bx - ax, by - ay
        ll = dx * dx + dy * dy
        t = np.clip(((XX - ax) * dx + (YY - ay) * dy) / ll, 0, 1)
        d = np.hypot(XX - (ax + t * dx), YY - (ay + t * dy))
        width = w * SS * np.maximum(np.sin(t * math.pi), 0.03) ** 0.7
        f = np.minimum(f, d / width - 1 + j * 0.24)
    return f


class Peel:
    """the metal torn back: petals of it curling outward from a rim, each its own height (so the light finds it), a bright torn edge,
    rust on what used to be the inner face, and a shadow on the plate under it"""

    def __init__(self):
        self.height = np.zeros((FH, FW), np.float32)
        self.mask = np.zeros((FH, FW), np.float32)
        self.edge = np.zeros((FH, FW), np.float32)
        self.shadow = np.zeros((FH, FW), np.float32)
        self.turn = np.zeros((FH, FW), np.float32)          # how far along its petal: the plate's own paint at the root, rust at the tear

    def petal(self, rng, cx, cy, a, width, r0, length, lift):
        """from a rim r0 out from (cx, cy), about way a, `width` wide where it leaves the rim"""
        ca, sa = math.cos(a), math.sin(a)
        ux, uy = -sa, ca
        pts = []
        steps = 8
        bend = rng.uniform(-0.28, 0.28) * length           # the tip goes off to one side, as torn metal does
        for k in range(steps + 1):                         # up one side
            t = k / steps
            half = width / 2 * (1 - t * t) ** 0.6 * rng.uniform(0.70, 1.20)
            along = r0 + t * length
            pts.append((cx + ca * along - ux * half + ux * bend * t * t, cy + sa * along - uy * half + uy * bend * t * t))
        for k in range(steps - 1, -1, -1):                 # and back down the other
            t = k / steps
            half = width / 2 * (1 - t * t) ** 0.6 * rng.uniform(0.70, 1.20)
            along = r0 + t * length
            pts.append((cx + ca * along + ux * half + ux * bend * t * t, cy + sa * along + uy * half + uy * bend * t * t))
        pad = 4
        x0 = int(max(0, min(p[0] for p in pts) - pad))
        x1 = int(min(FW, max(p[0] for p in pts) + pad + 1))
        y0 = int(max(0, min(p[1] for p in pts) - pad))
        y1 = int(min(FH, max(p[1] for p in pts) + pad + 1))
        if x1 <= x0 or y1 <= y0:
            return
        im = Image.new('L', (x1 - x0, y1 - y0), 0)
        ImageDraw.Draw(im).polygon([(p[0] - x0, p[1] - y0) for p in pts], fill=255)
        m = np.asarray(im, np.float32) / 255
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        s = np.clip(((xx - cx) * ca + (yy - cy) * sa - r0) / length, 0, 1)
        curl = lift * (s ** 1.6) + np.sin(s * math.pi) * lift * 0.28   # it rises as it goes, a rolled edge
        self.height[y0:y1, x0:x1] = np.where(m > 0, np.maximum(self.height[y0:y1, x0:x1], curl), self.height[y0:y1, x0:x1])
        self.mask[y0:y1, x0:x1] = np.maximum(self.mask[y0:y1, x0:x1], m)
        self.turn[y0:y1, x0:x1] = np.maximum(self.turn[y0:y1, x0:x1], m * s)
        er = np.asarray(im.filter(ImageFilter.MinFilter(3)), np.float32) / 255
        self.edge[y0:y1, x0:x1] = np.maximum(self.edge[y0:y1, x0:x1], m - er)
        # its shadow: thrown down and right, further the higher it stands
        off = int(4 + lift * 0.34)
        sx0, sy0 = x0 + off, y0 + off
        sx1, sy1 = min(FW, x1 + off), min(FH, y1 + off)
        if sx1 > sx0 and sy1 > sy0:
            sub = (m * (0.35 + 0.65 * s))[:sy1 - sy0, :sx1 - sx0]
            self.shadow[sy0:sy1, sx0:sx1] = np.maximum(self.shadow[sy0:sy1, sx0:sx1], sub)

    def round_hole(self, rng, cx, cy, r, n):
        a0 = rng.uniform(0, math.tau)
        for k in range(n):                                  # broad uneven lips, not spikes round a sun
            a = a0 + k / n * math.tau + rng.uniform(-0.5, 0.5)
            self.petal(rng, cx * SS, cy * SS, a, r * SS * rng.uniform(0.9, 1.5), r * SS * rng.uniform(0.74, 0.92),
                       r * SS * rng.uniform(0.28, 0.72), rng.uniform(16, 36))

    def along_slash(self, rng, x0, y0, x1, y1, w, n):
        dx, dy = x1 - x0, y1 - y0
        ll = math.hypot(dx, dy)
        up = math.atan2(dy, dx) + math.pi / 2
        for k in range(n):
            t = (k + 0.5) / n + rng.uniform(-0.03, 0.03)
            side = up if k % 2 else up + math.pi
            at = max(0.3, math.sin(t * math.pi) ** 0.7)
            cx, cy = x0 + dx * t, y0 + dy * t
            self.petal(rng, cx * SS, cy * SS, side + rng.uniform(-0.3, 0.3), ll / n * SS * rng.uniform(1.1, 1.8), w * SS * at * 0.72,
                       w * SS * rng.uniform(0.7, 1.7) * at + 6, rng.uniform(10, 24))


def hole_mask(rng, spots):
    """a torn hole's distance field: negative inside, its edge jagged"""
    j = fractal(rng, 24, 4)
    fine = fractal(rng, 6, 2)
    f = np.full((FH, FW), 1e4, np.float32)
    for cx, cy, r in spots:
        d = np.hypot(XX - cx * SS, (YY - cy * SS) * 1.08)
        ang = np.arctan2(YY - cy * SS, XX - cx * SS)
        petals = np.sin(ang * rng.integers(5, 9) + rng.uniform(0, 6)) * 0.14
        f = np.minimum(f, d / (r * SS) * (1 + petals) - 1 + j * 0.13 + fine * 0.05)
    return f


def wires(rng, spots, n_each):
    """wires and hoses out of the holes: an RGBA layer at the working size"""
    im = Image.new('RGBA', (FW, FH), (0, 0, 0, 0))
    shadow = Image.new('L', (FW, FH), 0)
    dr, ds = ImageDraw.Draw(im), ImageDraw.Draw(shadow)
    cols = [(196, 44, 34), (214, 170, 48), (56, 110, 190), (66, 150, 84), (200, 200, 200), (216, 110, 36), (40, 40, 44)]
    for cx, cy, r in spots:
        for _ in range(n_each):
            a = rng.uniform(0, math.tau)
            p0 = np.array((cx + math.cos(a) * r * 0.5, cy + math.sin(a) * r * 0.5)) * SS
            # hanging: out over the rim and down, slack, some ends loose
            out = rng.uniform(0.7, 1.5) * r * SS
            p3 = p0 + np.array((math.cos(a) * out + rng.uniform(-30, 30), rng.uniform(0.7, 2.3) * r * SS))
            p1 = p0 + np.array((math.cos(a) * out * 0.8, rng.uniform(-0.5, 0.2) * r * SS))
            p2 = p3 + np.array((rng.uniform(-40, 40), -rng.uniform(20, 70)))
            pts = []
            for t in np.linspace(0, 1, 40):
                q = (1 - t) ** 3 * p0 + 3 * (1 - t) ** 2 * t * p1 + 3 * (1 - t) * t * t * p2 + t ** 3 * p3
                pts.append(tuple(q))
            th = int(rng.choice((2, 3, 3, 4, 6, 9)))
            col = cols[int(rng.integers(0, len(cols)))]
            ds.line([(x + 9, y + 11) for x, y in pts], fill=170, width=th + 2)
            dr.line(pts, fill=(col[0] // 3, col[1] // 3, col[2] // 3, 255), width=th + 2)
            dr.line(pts, fill=col + (255,), width=th)
            bright = tuple(min(255, c + 90) for c in col)
            dr.line([(x - th * 0.25, y - th * 0.25) for x, y in pts], fill=bright + (255,), width=max(1, th // 3))
            ex, ey = pts[-1]                                # the bared copper end
            dr.ellipse([ex - th, ey - th, ex + th, ey + th], fill=(232, 158, 84, 255))
    return im, blur(np.asarray(shadow, np.float32) / 255, 4)


def interior(rng):
    """what is behind the plate: frames and pipes in the dark"""
    a = np.full((FH, FW, 3), 13, np.float32)
    a *= np.array((0.9, 1.0, 1.2), np.float32)
    for x in range(40, FW, 150):
        w = 26
        m = np.abs(XX - x) < w
        shade = np.clip(1 - np.abs(XX - x + 8) / w, 0, 1)[..., None]
        a[m] = (np.array((20, 23, 28), np.float32) * (0.4 + shade * 1.2))[m]
    for y in range(70, FH, 170):
        m = np.abs(YY - y) < 16
        shade = np.clip(1 - np.abs(YY - y + 5) / 16, 0, 1)[..., None]
        a[m] = (np.array((26, 25, 24), np.float32) * (0.4 + shade * 1.3))[m]
    return a


def lit(h, albedo, gloss):
    gy, gx = np.gradient(h)
    n = np.dstack((-gx, -gy, np.full_like(h, 3.0)))
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    l = np.array((-0.45, -0.62, 0.64), np.float32)
    l /= np.linalg.norm(l)
    diff = np.clip((n * l).sum(2), 0, 1)
    v = np.array((0, 0, 1), np.float32)
    hv = (l + v) / np.linalg.norm(l + v)
    spec = np.clip((n * hv).sum(2), 0, 1) ** 30 * gloss
    light = 0.38 + 0.92 * diff
    # the hangar's pool of light, from above
    pool = 0.82 + 0.34 * np.exp(-(((XX - FW * 0.42) / (FW * 0.55)) ** 2 + ((YY - FH * 0.16) / (FH * 0.62)) ** 2))
    return albedo * (light * pool)[..., None] + spec[..., None] * np.array((150, 148, 140), np.float32)


def bend(out):
    """the flat plate wrapped on the hull: below CURVE_Y it turns away like the side of a cylinder, so its rows crowd together and it
    falls into the dark"""
    y0, ye = CURVE_Y * SS, (FOOT_Y + 2) * SS
    r = ye - y0
    th = np.arcsin(np.clip((YY - y0) / r, 0, 0.9995))
    u = (XX - FW * 0.44) / (FW * 0.56)
    ys = np.where(YY < y0, YY, y0 + r * th) + (u * u) * 4 * SS     # (a seam barely bends: the user, "not individual U's like balloons")
    bent = out[np.clip(ys, 0, FH - 1).astype(np.int32), XX.astype(np.int32)]
    shade = np.cos(th) ** 1.35 * (1 - 0.30 * np.abs(u) ** 2.4)
    sheen = np.exp(-((th - 0.62) / 0.20) ** 2) * 0.22                         # the deck lights along the bend
    bent = bent * (shade + sheen * np.cos(th))[..., None] + (sheen * 16)[..., None]
    bent[YY >= ye] = 0
    return bent


SEED = 27


def clean():
    rng = np.random.default_rng(SEED)
    h = base_height(rng)
    albedo = stencils(rng)
    wear = fractal(np.random.default_rng(SEED + 1), 16, 4)
    albedo = albedo * (1 - np.clip(wear - 1.2, 0, 1)[..., None] * 0.14)   # even new plate has seen a deck
    return h, albedo, wear


def damaged(h, albedo, drng, holes=(), slashes=(), splashes=(), gouges=(), wired=False):
    """the clean plate with this damage: holes (x, y, r), slashes (x0, y0, x1, y1, w), splashes and gouge clusters (x, y, r)"""
    gloss = 0.55
    # no soot in a vacuum (the user: "singes probably don't make sense in space"): a hit blasts the paint off down to bright bare steel,
    # chipped at its edge, and throws droplets of melted metal, which set where they land
    blast_at = list(holes) + list(splashes)
    if blast_at:
        blast = np.zeros((FH, FW), np.float32)
        chip = fractal(drng, 9, 3)
        for cx, cy, r in blast_at:
            d = np.hypot(XX - cx * SS, YY - cy * SS) / (r * SS * 1.7)
            blast = np.maximum(blast, 1.05 - d)
        bare = (np.clip((blast + chip * 0.14 - 0.14) * 6, 0, 1) * (blast > 0.01))[..., None]
        albedo = albedo * (1 - bare) + (np.array((122, 124, 128), np.float32) + chip[..., None] * 7) * bare
        gloss = gloss + bare[..., 0] * 0.25
        im = Image.new('L', (FW, FH), 0)
        dr = ImageDraw.Draw(im)
        for cx, cy, r in blast_at:
            for _ in range(int(r * 1.3)):
                a, b = drng.uniform(0, math.tau), r * SS * (1 + drng.exponential(0.6))
                x, y, sz = cx * SS + math.cos(a) * b, cy * SS + math.sin(a) * b, drng.uniform(1.5, 4.5)
                dr.ellipse([x - sz, y - sz, x + sz, y + sz], fill=255)
        spatter = np.asarray(im, np.float32) / 255
        h = h + blur(spatter, 1.0) * 5
        albedo = albedo * (1 - spatter[..., None]) + np.array((150, 152, 156), np.float32) * spatter[..., None]
    # scoring out from a hit, and gouges
    im = Image.new('L', (FW, FH), 0)
    dr = ImageDraw.Draw(im)
    for cx, cy, r in blast_at:
        for _ in range(int(r * 0.5)):
            a = drng.uniform(0, math.tau)
            r0, r1 = r * SS * drng.uniform(0.7, 1.1), r * SS * drng.uniform(1.3, 2.6)
            dr.line([(cx * SS + math.cos(a) * r0, cy * SS + math.sin(a) * r0), (cx * SS + math.cos(a) * r1, cy * SS + math.sin(a) * r1)],
                    fill=255, width=int(drng.integers(2, 4)))
    for cx, cy, r in gouges:
        for _ in range(int(drng.integers(3, 6))):
            x, y = cx * SS + drng.uniform(-r, r) * SS, cy * SS + drng.uniform(-r, r) * SS
            a, n = drng.uniform(0.2, 0.9) if drng.random() < 0.7 else drng.uniform(2.2, 2.9), drng.uniform(r * 1.4, r * 3.4) * SS
            pts = [(x, y)]
            for _k in range(6):
                a += drng.uniform(-0.09, 0.09)
                x += math.cos(a) * n / 6
                y += math.sin(a) * n / 6
                pts.append((x, y))
            dr.line(pts, fill=255, width=int(drng.integers(3, 8)))
    gouge = np.asarray(im, np.float32) / 255
    if gouge.any():
        h = h - blur(gouge, 1.6) * 7 + blur(gouge, 5) * 1.5
        g = blur(gouge, 1)[..., None]
        albedo = albedo * (1 - g * 0.2) + g * 34
    # dents round a hole
    for cx, cy, r in holes:
        for _ in range(3):
            a, b, q = drng.uniform(0, math.tau), r * SS * drng.uniform(1.0, 1.7), r * SS * drng.uniform(0.25, 0.6)
            x, y = cx * SS + math.cos(a) * b, cy * SS + math.sin(a) * b
            h = h - np.exp(-(((XX - x) ** 2 + (YY - y) ** 2) / (q * q))) * q * 0.5
    if not holes and not slashes:
        return lit(h, albedo, gloss)
    f = hole_mask(drng, holes) if holes else np.full((FH, FW), 1e4, np.float32)
    peel = Peel()
    for cx, cy, r in holes:
        peel.round_hole(drng, cx, cy, r, int(drng.integers(3, 6)) if r > 16 else 2)
    for sl in slashes:
        peel.along_slash(drng, *sl, 7 if sl[4] > 5 else 3)
    if slashes:
        f = np.minimum(f, slash_field(drng, slashes))
    inside = (f < 0) & (peel.mask < 0.5)
    # the rim: the metal buckled up and away from the hole, in lobes
    rim = np.exp(-(np.maximum(f, 0) / 0.26) ** 2)
    h = h + rim * 46 * (1 + fractal(drng, 22, 2) * 0.7)
    heat = np.clip(1 - np.maximum(f, 0) / 0.5, 0, 1) ** 2
    albedo = albedo * np.dstack((1 + heat * 0.06, 1 - heat * 0.06, 1 + heat * 0.05))     # (heat-blued, faintly)
    # what is peeled back shows its inner face: bare steel, freshly cut (no rust: the user, "it wasn't left to sea", "just sliced")
    grain = fractal(drng, 14, 3)
    inner = np.array((112, 114, 118), np.float32) + grain[..., None] * np.array((11, 11, 12), np.float32)
    m = peel.mask[..., None]
    mix = np.clip(peel.turn * 1.5, 0, 1)[..., None] * m     # (the root still the plate's own)
    albedo = albedo * (1 - mix) + inner * mix
    gloss = gloss * (1 - peel.mask) + peel.mask * 0.35
    h = h + blur(peel.height, 0.8) * 1.15
    out = lit(h, albedo, gloss)
    out = out * (1 - blur(peel.shadow, 2.2)[..., None] * 0.72 * (1 - m))
    e = peel.edge[..., None] * np.clip(peel.turn * 2, 0, 1)[..., None] * 0.5     # (a torn edge catches a little light, toward its tip)
    out = out * (1 - e) + np.array((196, 198, 202), np.float32) * e
    back = interior(drng)
    rim_shadow = np.clip(1 - np.maximum(-f, 0) / 0.3, 0, 1)[..., None]            # the plate's edge shades what is under it
    back = back * (1 - rim_shadow * 0.7)
    out[inside] = back[inside]
    lip = (f >= 0) & (f < 0.05)
    out[lip] = np.clip(out[lip] * 1.45 + 26, 0, 255)
    if wired:
        layer, shadow = wires(drng, holes, 5)
        out = out * (1 - shadow[..., None] * 0.6 * (~inside)[..., None])
        la = np.asarray(layer, np.float32)
        al = la[..., 3:] / 255
        out = out * (1 - al) + la[..., :3] * al
        # sparks, something still shorting
        spark = Image.new('L', (FW, FH), 0)
        ds = ImageDraw.Draw(spark)
        for cx, cy, r in holes:
            for _ in range(5):
                a, b = drng.uniform(0, math.tau), drng.uniform(0.2, 1.2) * r * SS
                x, y = cx * SS + math.cos(a) * b, cy * SS + math.sin(a) * b
                ds.ellipse([x - 2, y - 2, x + 2, y + 2], fill=255)
        sp = blur(np.asarray(spark, np.float32) / 255, 1.2)[..., None]
        out = out * (1 - sp) + np.array((255, 224, 150), np.float32) * sp
    return out


def cut(out, base):
    """what differs from the clean plate, cropped: (RGBA picture, x, y)"""
    a = np.asarray(out).astype(np.int32)
    b = np.asarray(base).astype(np.int32)
    diff = np.abs(a - b).sum(2) > 9
    m = Image.fromarray((diff * 255).astype(np.uint8), 'L').filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3))
    box = m.getbbox()
    if not box:
        return None
    rgba = Image.fromarray(a.astype(np.uint8)).convert('RGBA')
    rgba.putalpha(m)
    return rgba.crop(box), box[0], box[1]


def finish(out):
    # the words go at x 400 on, rows 130 on: darker there, softly, so they read whatever is behind them
    k = np.clip((XX - (300 + EXTRA) * SS) / (130 * SS), 0, 1) * np.clip((YY - 60 * SS) / (110 * SS), 0, 1)
    k = k * k * (3 - 2 * k)
    out = out * (1 - k[..., None] * 0.62)
    out = out * (0.86 - 0.16 * ((XX / FW - 0.5) ** 2 + (YY / FH - 0.45) ** 2) * 2)[..., None]
    im = Image.fromarray(np.clip(bend(out), 0, 255).astype(np.uint8))
    return im.resize((W, H), Image.LANCZOS).convert('RGB')


def main():
    args = [a for a in sys.argv[1:]]
    sheet = None
    if '--sheet' in args:
        k = args.index('--sheet')
        sheet = args[k + 1]
        del args[k:k + 2]
    here = os.path.dirname(os.path.abspath(__file__))
    out_dir = args[0] if args else os.path.join(here, '..', 'art', 'places', 'plate')
    os.makedirs(out_dir, exist_ok=True)
    h, albedo, _w = clean()
    base = finish(lit(h, albedo, 0.55))
    base.save(os.path.join(out_dir, 'base.png'))
    table = {'big': [], 'small': []}
    all_stamps = base.copy()
    for k, spot in enumerate(BIG):
        drng = np.random.default_rng(1000 + k)
        spot = (spot[0] + EXTRA, spot[1], spot[2])
        slash = []
        if k % 2 == 0:                                       # every other one with a slash running off it
            a = drng.uniform(0, math.tau)
            r = spot[2]
            slash = [(spot[0] + math.cos(a) * r * 0.7, spot[1] + math.sin(a) * r * 0.7, spot[0] + math.cos(a) * r * 2.4,
                      spot[1] + math.sin(a) * r * 2.4, max(4, r * 0.15))]
        st = cut(finish(damaged(h, albedo, drng, holes=[spot], slashes=slash, wired=True)), base)
        name = 'big_%02d.png' % k
        st[0].save(os.path.join(out_dir, name))
        table['big'].append({'file': name, 'x': st[1] - EXTRA, 'y': st[2]})
        all_stamps.paste(st[0], (st[1], st[2]), st[0])
    for k, spec in enumerate(SMALL):
        drng = np.random.default_rng(2000 + k)
        kind, rest = spec[0], (spec[1] + EXTRA,) + spec[2:]
        if kind == 'slash':
            rest = rest[:2] + (rest[2] + EXTRA,) + rest[3:]
        kw = {'hole': {'holes': [rest]}, 'slash': {'slashes': [rest]}, 'splash': {'splashes': [rest]}, 'gouge': {'gouges': [rest]}}[kind]
        st = cut(finish(damaged(h, albedo, drng, **kw)), base)
        name = 'small_%02d.png' % k
        st[0].save(os.path.join(out_dir, name))
        table['small'].append({'file': name, 'x': st[1] - EXTRA, 'y': st[2]})
        all_stamps.paste(st[0], (st[1], st[2]), st[0])
    with open(os.path.join(out_dir, 'stamps.json'), 'w') as f:
        json.dump(table, f, indent=1)
    if sheet:
        all_stamps.save(sheet)


if __name__ == '__main__':
    main()
