#!/usr/bin/env python3
"""Skins: a ship re-textured with another texture, projected onto it.

A mesh's texture coordinates only fit its own maps.  A skin replaces them:
each triangle takes the texture by straight projection along its main
axis (a top or bottom face from above, a side face from the side, a front
or back face from ahead), `tile` mesh units to a repeat of the texture.
The skin's diffuse and specular maps are made once and cached.

    aera    textures/AeraHull2.bmp, the hull map the Aera capital ships
            tile: grey panels ruled in thin green lines.  Tinted to the warm
            grey-tan of the Aera fighters' own hulls (Ariston, Nicander,
            Areus), its lines turned the blue of their accent marks (green
            is kept for the glows and light strips), and bars of their
            bright accent blue laid along some of the lines, as the Ariston
            and the Nicander carry them; specular from its own shading,
            strong enough for their metal sheen, less on the paint.
    anaxander  made, not read: the Anaxander's paint (Vega Strike's hammer-head
            cruiser, its own map's colours measured: khaki 134 129 99, its
            darker panels 103 98 75, purple 106 82 143, cyan 4 207 205) for
            the Aera capital ships whose maps are missing (the user,
            2026-11-14: "let's add similarly artful textures to the untextured
            craft ... To match the anaxander style"): panels of khaki ruled in
            thin dark lines, a few darker; purple brush strokes laid
            diagonally in twos, threes and fours (its service markings); a few
            thin cyan lines along panel edges.  Specular from the khaki,
            less on the paint.
"""
import hashlib
import os

import numpy as np
from PIL import Image
from scipy import ndimage

SKINS = {
    'anaxander': {'make': 'anaxander', 'tile': 6.0, 'size': 1024, 'seed': 11,
                  'base': (134, 129, 99), 'dark': (103, 98, 75), 'line': (40, 38, 30), 'purple': (106, 82, 143),
                  'cyan': (4, 207, 205), 'panels': 24, 'groups': 15, 'cyan_lines': 14, 'spec': 0.6},
    'aera': {'diffuse': 'textures/AeraHull2.bmp', 'tile': 4.0,
             # the fighters' hull median, (74, 65, 57) in Ariston's map, as a cast
             'tint': (1.14, 1.0, 0.88), 'gain': 1.25, 'spec': 0.75,
             # Ariston's accent marks, median (17, 105, 189)
             'lines': (0.07, 0.41, 0.74), 'line_mix': 0.6, 'calm': 1.5,
             # the fighters' blue marks: bars laid along some of the panel lines
             'accents': {'count': 70, 'length': (36, 110), 'width': 7, 'colour': (0.09, 0.45, 0.86), 'seed': 3}},
}


def anaxander(s):
    """The Anaxander's style as a tiling map, (rgb 0..1, specular 0..1): see SKINS."""
    from PIL import ImageDraw
    n, rng = s['size'], np.random.default_rng(s['seed'])
    col = lambda c: np.array(c, np.float32) / 255
    out = np.ones((n, n, 3), np.float32) * col(s['base'])
    # panels: the square cut again and again (wrapping), each a little lighter or darker, a few the darker khaki
    lines = np.zeros((n, n), np.float32)
    cuts = [(0, 0, n, n)]
    while len(cuts) < s['panels']:
        k = int(rng.integers(len(cuts)))
        x, y, w, h = cuts.pop(k)
        if w >= h and w > 128:
            c = int(rng.integers(w // 4, 3 * w // 4)) // 16 * 16
            cuts += [(x, y, c, h), (x + c, y, w - c, h)]
        elif h > 128:
            c = int(rng.integers(h // 4, 3 * h // 4)) // 16 * 16
            cuts += [(x, y, w, c), (x, y + c, w, h - c)]
        else:
            cuts.append((x, y, w, h))
    for x, y, w, h in cuts:
        tone = col(s['dark']) if rng.random() < 0.18 else col(s['base']) * float(rng.uniform(0.95, 1.04))
        out[y:y + h, x:x + w] = tone
        lines[y, x:x + w] = lines[y:y + h, x] = 1
    grain = ndimage.gaussian_filter(rng.standard_normal((n, n)).astype(np.float32), 3, mode='wrap')
    out *= (1 + 0.05 * grain / max(1e-6, float(grain.std())))[..., None]
    lines = np.clip(ndimage.uniform_filter(lines, 2, mode='wrap') * 2, 0, 1)
    out = out * (1 - 0.7 * lines[..., None]) + col(s['line']) * 0.7 * lines[..., None]
    # the purple strokes: groups of 2-4 parallel diagonal brush strokes, tapered, drawn wrapping round
    paint = Image.new('L', (n, n), 0)
    d = ImageDraw.Draw(paint)
    for _g in range(s['groups']):
        cx, cy = rng.uniform(0, n, 2)
        ang = np.radians(rng.uniform(52, 72)) * (1 if rng.random() < 0.5 else -1)
        length, width, gap = rng.uniform(110, 230), rng.uniform(11, 17), rng.uniform(24, 34)
        ax, ay = np.cos(ang), np.sin(ang)
        for k in range(int(rng.integers(2, 5))):
            ox, oy = cx + k * gap * ay, cy - k * gap * ax
            half = length * rng.uniform(0.75, 1.0) / 2
            for t in np.linspace(-1, 1, 24):          # (a stroke: discs along it, thinner at its ends)
                r = width / 2 * (1 - 0.6 * abs(t) ** 3)
                px, py = ox + t * half * ax, oy + t * half * ay
                for wx in (-n, 0, n):
                    for wy in (-n, 0, n):
                        d.ellipse((px + wx - r, py + wy - r, px + wx + r, py + wy + r), fill=255)
    purple = ndimage.gaussian_filter(np.asarray(paint, np.float32) / 255, 1.2, mode='wrap')
    out = out * (1 - purple[..., None]) + col(s['purple']) * purple[..., None]
    # the cyan lines: thin, along part of a few panel edges
    cyan = np.zeros((n, n), np.float32)
    for _c in range(s['cyan_lines']):
        x, y, w, h = cuts[int(rng.integers(len(cuts)))]
        if rng.random() < 0.5:
            a = int(rng.integers(x, x + max(1, w // 2)))
            cyan[(y + 4) % n, a:a + int(rng.integers(w // 4 + 1, w // 2 + 2))] = 1
        else:
            a = int(rng.integers(y, y + max(1, h // 2)))
            cyan[a:a + int(rng.integers(h // 4 + 1, h // 2 + 2)), (x + 4) % n] = 1
    cyan = np.clip(ndimage.uniform_filter(cyan, 3, mode='wrap') * 3, 0, 1)
    out = out * (1 - cyan[..., None]) + col(s['cyan']) * cyan[..., None]
    spec = np.clip(out.mean(axis=2) * s['spec'] * (1 - 0.5 * purple) + 0.3 * cyan, 0, 1)
    return np.clip(out, 0, 1), spec


def maps(name, assets, cache):
    """(diffuse path, specular path) of a skin, made if missing."""
    s = SKINS[name]
    os.makedirs(cache, exist_ok=True)
    tag = hashlib.md5(repr(sorted(s.items())).encode()).hexdigest()[:8]    # remade when the recipe changes
    dpath = os.path.join(cache, 'skin-%s-%s.png' % (name, tag))
    spath = os.path.join(cache, 'skin-%s-%s-spec.png' % (name, tag))
    if s.get('make') == 'anaxander' and not (os.path.exists(dpath) and os.path.exists(spath)):
        out, spec = anaxander(s)
        Image.fromarray((out * 255 + 0.5).astype(np.uint8)).save(dpath)
        Image.fromarray((spec * 255 + 0.5).astype(np.uint8)).convert('RGB').save(spath)
    if not (os.path.exists(dpath) and os.path.exists(spath)):
        a = np.asarray(Image.open(os.path.join(assets, s['diffuse'])).convert('RGB')).astype(np.float32) / 255
        lum = a.mean(axis=2, keepdims=True)
        green = np.clip((a[..., 1:2] - np.maximum(a[..., 0:1], a[..., 2:3])) * 8, 0, 1)
        if s.get('calm'):
            # its speckle softened (the lines are put back over it), wrapping round as it tiles
            lum = ndimage.gaussian_filter(lum, (s['calm'], s['calm'], 0), mode='wrap')
        green *= s.get('line_mix', 1.0)
        hull = lum * np.array(s['tint'], np.float32) * s['gain']
        lines = np.array(s['lines'], np.float32) if 'lines' in s else a * s['gain']
        out = hull * (1 - green) + lines * green
        paint = accents(green[..., 0] > 0.3 * s.get('line_mix', 1.0), s['accents']) if 'accents' in s else None
        spec = np.clip(lum[..., 0] * s['spec'] + green[..., 0] * 0.2, 0, 1)
        if paint is not None:
            out = out * (1 - paint[..., None]) + np.array(s['accents']['colour'], np.float32) * paint[..., None]
            spec *= 1 - 0.5 * paint
        Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)).save(dpath)
        Image.fromarray((spec * 255 + 0.5).astype(np.uint8)).convert('RGB').save(spath)
    return dpath, spath


def accents(line, cfg):
    """Accent bars over a tiling texture: the straight runs of its panel
    lines (line, a mask) long enough to carry one, a random few of them
    painted along part of their length, `width` pixels thick.  Wraps round
    as the texture tiles.  (0..1 coverage)"""
    rng = np.random.default_rng(cfg['seed'])
    h, w = line.shape
    lo, hi = cfg['length']
    runs = []
    for axis in (0, 1):                                      # along rows, then along columns
        m = line if axis == 0 else line.T
        for r in range(m.shape[0]):
            row = np.concatenate([[False], m[r], [False]])
            d = np.diff(row.astype(np.int8))
            for a, b in zip(np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]):
                if b - a >= lo:
                    runs.append((axis, r, a, b))
    out = np.zeros((h, w), np.float32)
    if not runs:
        return out
    half = cfg['width'] // 2
    for k in rng.choice(len(runs), size=min(cfg['count'], len(runs)), replace=False):
        axis, r, a, b = runs[k]
        n = int(rng.integers(lo, min(hi, b - a) + 1))
        start = int(rng.integers(a, b - n + 1))
        along = np.arange(start, start + n) % (w if axis == 0 else h)
        across = np.arange(r - half, r + half + 1) % (h if axis == 0 else w)
        if axis == 0:
            out[np.ix_(across, along)] = 1
        else:
            out[np.ix_(along, across)] = 1
    return ndimage.uniform_filter(out, 2, mode='wrap')


def apply(mesh, name, assets, cache, tile=None):
    """Re-texture mesh in place with skin name: projected coordinates, its maps.
    tile (mesh units a repeat) overrides the skin's."""
    tile = tile or SKINS[name]['tile']
    t = np.asarray(mesh.tri_index).reshape(-1, 3)
    p = mesh.positions[t]                                   # (F, 3, 3)
    fn = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    axis = np.argmax(np.abs(fn), axis=1)                    # 0 side, 1 top/bottom, 2 front/back
    plane = {0: (2, 1), 1: (0, 2), 2: (0, 1)}
    uv = np.empty((len(t), 3, 2))
    for k, (i, j) in plane.items():
        sel = axis == k
        uv[sel, :, 0] = p[sel, :, i] / tile
        uv[sel, :, 1] = p[sel, :, j] / tile
    # each triangle moved by whole repeats so it lies near 0..1 (the maps repeat)
    uv -= np.floor(uv.min(axis=1, keepdims=True))
    mesh.tri_uv = uv
    dpath, spath = maps(name, assets, cache)
    mesh.textures = {0: [(2, dpath)], 1: [(2, spath)]}
