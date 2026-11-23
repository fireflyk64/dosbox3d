#!/usr/bin/env python3
# privtalon.py: writes src/cpu/wcnet_priv_talon.h, hand-made 2x art for parts of the target display's ships
# (docs/privateer.md section 28).  The Talon's front, the user, 2026-11-23: "Everyone sees the talon the [most]. Can
# you have custom drawing code for the front ... keyed off the actual sprite map edges?" -> a hand-drawn front, then
# "Thinner outline", "Make sure it is symmetrical".  The front part (94 pixels: the bulb's outline, the canopy, the
# engines and their exhausts) is drawn as: the bulb's outline a smooth curve one fine pixel wide through the middles of
# the game's outline pixels (its flat top, out to the game's widest row, back into the engines; below them a U to the
# tip), the canopy the same with its fill, the engines and exhausts the game's blocks.  Each curve is laid half a fine
# pixel outward of the game pixels' middles (the shape keeps its size), drawn on the left half and mirrored pixel for
# pixel (u + u' = 51 in the part's frame: fine X and 103 - X).  The colours are the sprite's own at three pixels (its
# damage shows); the art is used only where the part's lit pixels are exactly GAME below.
#   python3 scripts/privtalon.py > src/cpu/wcnet_priv_talon.h
import math
import sys

PART = 0xC7EFE729                   # the Talon's front part's mask hash (both Talons), as kWireBlockParts
U0, V0 = 19, 29                     # the game pixels GAME stands for, in the part's 52 x 52 frame
GAME = [                            # 'x' drawn here by the art, 'b' kept as the game's block, '.' none of the part
    "...xxxxxxxx...",
    "..x........x..",
    ".x..........x.",
    ".x...xxxx...x.",
    ".x..xxxxxx..x.",
    "x...xxxxxx...x",
    ".x..xxxxxx..x.",
    ".bbb.xxxx.bbb.",
    ".b.b..xx..b.b.",
    ".bbb......bbb.",
    ".b.b......b.b.",
    ".b.bx....xb.b.",
    ".bbbx....xbbb.",
    "..b.x....x.b..",
    "..b..x..x..b..",
    ".....x..x.....",
    "......xx......",
]
SAMPLES = {'o': (22, 29), 'k': (24, 32), 'f': (25, 34)}    # the bulb's outline, the canopy's, its fill: these pixels'
MIRROR = 103                        # fine X pairs with MIRROR - X (game u with 51 - u)
AX = MIRROR / 2.0 + 0.5             # the middle, fine x


def G(u, v):                        # a game pixel's middle, fine
    return (2 * u + 1.0, 2 * v + 1.0)


def catmull(P, n=12):
    out = []
    for i in range(len(P) - 1):
        p0, p1, p2, p3 = P[max(i - 1, 0)], P[i], P[i + 1], P[min(i + 2, len(P) - 1)]
        for k in range(n):
            s = k / n
            out.append(tuple(0.5 * (2 * p1[j] + (-p0[j] + p2[j]) * s + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * s * s
                                    + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * s ** 3) for j in (0, 1)))
    out.append(P[-1])
    return out


def thin(pts, centre):
    """one fine pixel wide: half a pixel outward from the shape's middle, each point's pixel, gaps stepped, a pixel
    between two that touch diagonally dropped; the left half, then its mirror"""
    dense = []
    for i in range(len(pts) - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        n = max(2, int(4 * math.hypot(x1 - x0, y1 - y0)))
        dense += [(x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n) for k in range(n)]
    dense.append(pts[-1])
    path = []
    for a, b in dense:
        if a > AX:
            continue
        dx, dy = a - centre[0], b - centre[1]
        L = math.hypot(dx, dy) or 1.0
        q = (int(math.floor(a + 0.5 * dx / L)), int(math.floor(b + 0.5 * dy / L)))
        if q[0] >= AX or (path and path[-1] == q):
            continue
        while path and (abs(path[-1][0] - q[0]) > 1 or abs(path[-1][1] - q[1]) > 1):
            p = path[-1]
            path.append((p[0] + (q[0] > p[0]) - (q[0] < p[0]), p[1] + (q[1] > p[1]) - (q[1] < p[1])))
        if q not in path:
            path.append(q)
    i = 1
    while i < len(path) - 1:
        a, c = path[i - 1], path[i + 1]
        if abs(a[0] - c[0]) == 1 and abs(a[1] - c[1]) == 1:
            path.pop(i)
        else:
            i += 1
    return set(path) | {(MIRROR - X, Y) for X, Y in path}


def inside(poly, X, Y):
    cx, cy, c = X + 0.5, Y + 0.5, False
    for i in range(len(poly)):
        (x0, y0), (x1, y1) = poly[i], poly[i - 1]
        if (y0 > cy) != (y1 > cy) and cx < x0 + (cy - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


def design():
    mirror = lambda P: [(MIRROR + 1 - a, b) for a, b in P]
    upper = catmull([G(25.5, 29), G(22, 29), (42.6, 60.6), (40.7, 63.2), (39.6, 66.2), (39.2, 69.0), (39.8, 71.6), G(20, 36)])
    lower = catmull([G(22.6, 39.2), G(23, 40), G(23.2, 42), (48.6, 87.6), (50.0, 89.8), G(25.5, 45.2)])
    can = catmull([G(25.5, 32), G(24, 32), (46.6, 66.6), (46.6, 70.0), (48.0, 73.0), (50.2, 74.9), G(25.5, 37.2)])
    bulb, canopy = (52.0, 72.0), (52.0, 66.0)
    art = {}
    poly = can + mirror(can)[::-1]
    for Y in range(2 * V0, 2 * (V0 + len(GAME))):
        for X in range(2 * U0, 2 * (U0 + len(GAME[0]))):
            if inside(poly, X, Y):
                art[(X, Y)] = 'f'
    for p in thin(can, canopy):
        art[p] = 'k'
    for p in thin(upper, bulb) | thin(lower, bulb):
        art[p] = 'o'
    # (where the game's blocks stay, nothing of the art)
    for (X, Y) in list(art):
        gu, gv = X // 2 - U0, Y // 2 - V0
        if not (0 <= gv < len(GAME) and 0 <= gu < len(GAME[0])) or GAME[gv][gu] == 'b':
            del art[(X, Y)]
    rows = []
    for Y in range(2 * V0, 2 * (V0 + len(GAME))):
        rows.append(''.join(art.get((X, Y), '.') for X in range(2 * U0, 2 * (U0 + len(GAME[0])))))
    return rows


def main():
    assert all(len(r) == len(GAME[0]) for r in GAME) and all(r == r[::-1] for r in GAME), 'GAME: a mirror image'
    rows = design()
    assert all(r == r[::-1] for r in rows), 'the art: a mirror image'
    for k, (u, v) in SAMPLES.items():
        assert GAME[v - V0][u - U0] == 'x', k
    out = sys.stdout
    out.write("// Written by scripts/privtalon.py: hand-made 2x art for parts of the target display's ships.  Per part: its\n"
              "// mask hash, the game pixels it stands for (u0, v0, w, h: 'x' drawn by the art, 'b' the game's block, '.' none\n"
              "// of the part -- the part's lit pixels must be exactly these), the game pixels whose colours the art's 'o', 'k'\n"
              "// and 'f' take, and the art (2w x 2h fine pixels).\n"
              "#ifndef WCNET_PRIV_TALON_H_\n#define WCNET_PRIV_TALON_H_\n\n"
              "struct PrivPartArt {\n    unsigned int part;\n    int u0, v0, w, h;\n    const char *game[24];\n"
              "    int sample[3][2];\n    const char *fine[48];\n};\n"
              "static const PrivPartArt kPrivPartArt[] = {\n")
    out.write("    { 0x%08Xu, %d, %d, %d, %d,\n      {\n" % (PART, U0, V0, len(GAME[0]), len(GAME)))
    for r in GAME:
        out.write('        "%s",\n' % r)
    out.write("      },\n      { { %d, %d }, { %d, %d }, { %d, %d } },\n      {\n" % (SAMPLES['o'] + SAMPLES['k'] + SAMPLES['f']))
    for r in rows:
        out.write('        "%s",\n' % r)
    out.write("      } },\n};\nenum { kPrivPartArtCount = sizeof(kPrivPartArt) / sizeof(kPrivPartArt[0]) };\n\n#endif\n")


if __name__ == '__main__':
    main()
