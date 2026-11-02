#!/usr/bin/env python3
"""The shapes the damage and weapons pages draw on the ship's wire (src/vdu.c), as eleven-by-eleven pixel art: '#' the bright
colour, '+' the dark one, '.' nothing.  Writes src/vduparts.h, one bright plane and one dark, 11 rows of 16 bits each: 44 bytes a
shape.  Run it by hand after changing a shape, the header is checked in.  With --sheet FILE, a picture of them all to look at."""
import os
import sys

PARTS = [
('ABSORBERS', """
###########
...........
..#######..
.........#.
..#######..
.#.........
..#######..
.........#.
..#######..
...........
###########"""),
('POWER', """
...#####...
..#.....#..
.#...##..#.
#...##....#
#..##.....#
#..#####..#
#.....##..#
#....##...#
.#..##...#.
..#.....#..
...#####..."""),
('SHIELDS', """
###########
#+++++++++#
#++#####++#
#++#+++#++#
#++#+++#++#
#++#+++#++#
.#+#+++#+#.
.#++###++#.
..#+++++#..
...#+++#...
....###...."""),
('COMPUTER', """
.#########.
.#+++++++#.
.#+.....+#.
.#+.##..+#.
.#+.....+#.
.#+++++++#.
.#########.
....###....
..#######..
...........
#+#+#+#+#+#"""),
('INTERCOM', """
..#.....#..
.#..+.+..#.
#..+...+..#
#..+.#.+..#
#..+###+..#
.#..###..#.
....###....
.....#.....
.....#.....
...+++++...
..#######.."""),
('TRACK', """
###.....###
#.........#
#....#....#
.....#.....
....+++....
..##+#+##..
....+++....
.....#.....
#....#....#
#.........#
###.....###"""),
('EJECTOR', """
.#......##.
###.....##.
.#......##.
.#......##.
.#.....##..
.#.....##..
.......##..
.......####
.......####
.........+.
........+++"""),
('REPAIR', """
.......##..
......##...
......##..#
......###.#
.....#####.
....#####..
...###.....
..###......
.###.......
###........
.#........."""),
('GUNS', """
.....#.....
.....#.....
....+#+....
....+#+....
....+#+....
....###....
....###....
...#####...
...#+#+#...
...#####...
....+++...."""),
('ENGINE', """
...#####...
..#+++++#..
.#+#+++#+#.
.#++#+#++#.
.#+++#+++#.
.#++#+#++#.
.#+#+++#+#.
..#+++++#..
..#######..
.#########.
###########"""),
('FUEL', """
...#####...
..#+++++#..
.#+++++++#.
.#########.
.#...#...#.
.#..###..#.
.#.#####.#.
.#..###..#.
.#########.
.#+++++++#.
..#######.."""),
('RADAR', """
#.......+..
##....+..+.
#.#..+.+..+
#..#..+..+.
#...#......
.#...#.....
..#...#....
...#####...
.....#.....
....###....
..#######.."""),
('VDU_L', """
###########
#+++++++++#
#+.#.....+#
#+.#.....+#
#+.#.....+#
#+.#.....+#
#+.###...+#
#+++++++++#
###########
....###....
..#######.."""),
('VDU_R', """
###########
#+++++++++#
#+.##....+#
#+.#.#...+#
#+.##....+#
#+.#.#...+#
#+.#.#...+#
#+++++++++#
###########
....###....
..#######.."""),
('NAV', """
.....#.....
.....#.....
....###....
....#+#....
.+.#+++#.+.
####+++####
.+.#+++#.+.
....#+#....
....###....
.....#.....
.....#....."""),
('THRUSTERS', """
...........
...........
....#......
...##.+....
..###..+...
.####.+.+..
..###..+...
...##.+....
....#......
...........
..........."""),
('RACKS', """
.....#.....
....###....
....###....
....+#+....
....###....
....###....
....###....
...+###+...
..++###++..
..+.###.+..
.....+....."""),
('IFF', """
###########
#+++++++++#
...........
###.###.###
.#..#...#..
.#..##..##.
.#..#...#..
###.#...#..
...........
#+++++++++#
###########"""),
('DECOYS', """
#....#....#
.+...+...+.
..#..#..#..
...+.+.+...
.....#.....
....###....
....###....
....+#+....
....+#+....
...#####...
...#####..."""),
]


def planes(art):
    rows = art.strip('\n').split('\n')
    assert len(rows) == 11 and all(len(r) == 11 for r in rows), art
    assert all(set(r) <= set('#+.') for r in rows), art
    bright = [sum(1 << (10 - i) for i, c in enumerate(r) if c == '#') for r in rows]
    dark = [sum(1 << (10 - i) for i, c in enumerate(r) if c == '+') for r in rows]
    return bright, dark


def header():
    out = ['/* vduparts.h: made by tools/vduparts.py, which has them as pictures: the shapes on the damage and weapons pages (vdu.c),',
           ' * 11 rows of the bright colour then 11 of the dark, the top row first, the leftmost pixel in bit 10 */',
           '#define PART_N 11', 'static const unsigned short __far g_part[%d][22] = {' % len(PARTS)]
    for name, art in PARTS:
        b, d = planes(art)
        out.append('    { ' + ', '.join('0x%03X' % v for v in b) + ',')
        out.append('      ' + ', '.join('0x%03X' % v for v in d) + ' },   /* %s */' % name)
    out.append('};')
    return '\n'.join(out) + '\n'


def sheet(path):
    from PIL import Image, ImageDraw
    n, zoom, cell = len(PARTS), 7, 100
    im = Image.new('RGB', (cell * 7, (cell + 40) * 3), (6, 22, 16))
    dr = ImageDraw.Draw(im)
    for i, (name, art) in enumerate(PARTS):
        x, y = cell * (i % 7) + 10, (cell + 40) * (i // 7) + 6
        b, d = planes(art)
        for r in range(11):
            for c in range(11):
                col = (235, 40, 30) if b[r] >> (10 - c) & 1 else (130, 24, 20) if d[r] >> (10 - c) & 1 else None
                if col:
                    dr.rectangle([x + c * zoom, y + r * zoom, x + (c + 1) * zoom - 1, y + (r + 1) * zoom - 1], fill=col)
                    im.putpixel((x + c, y + 86 + r), col)
        dr.text((x + 18, y + 88), name, fill=(200, 200, 120))
    im.save(path)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--sheet':
        sheet(sys.argv[2])
    else:
        here = os.path.dirname(os.path.abspath(__file__))
        with open(os.path.join(here, '..', 'src', 'vduparts.h'), 'w') as f:
            f.write(header())
