#!/usr/bin/env python3
# wchfont.py: writes src/cpu/wcnet_wch_font.h, the glyphs of Wing Commander 1's cockpit font (its VDUs and the speed
# readouts, height 6: a blank row, four rows of capitals, a row for descenders; a blank column first) for each
# character the user's own font has (scripts/privfont/, wcnet_priv_font.h).  At 640 x 400 a letter of the cockpit is
# drawn in the user's font only when the game draws exactly the glyph kept here (src/cpu/wcnet_wch.cpp, letters):
# any other font, or another glyph, stays the game's.  WC1's font is the 3 x 4 font Privateer's grew from: most
# glyphs are the ones the user's letters were drawn from, some differ by a pixel or two (listed in the header).
#
# The glyphs are read from a font dump of the running game (WCHIRESFONTS=1 writes wch-font-<n>.txt into
# WCNET_DUMP_DIR; the cockpit's font is the one of height 6 used with the contexts 9F02, 9F24 and 9EC4):
#   python3 scripts/wchfont.py wch-font-3.txt > src/cpu/wcnet_wch_font.h
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def read_dump(path):
    lines = open(path).read().split('\n')
    m = re.match(r'font (\w+):(\w+) height (\d+) ink (\w+) ground (\w+)', lines[0])
    assert m, 'not a font dump'
    h = int(m.group(3))
    glyphs = {}
    i = 1
    while i < len(lines) and lines[i].startswith('char'):
        c = re.match(r"char (\d+) '.' width (\d+)", lines[i])
        rows = lines[i + 1:i + 1 + h]
        glyphs[int(c.group(1))] = (int(c.group(2)), rows)
        i += 1 + h
    return h, glyphs


def user_glyphs():
    src = open(os.path.join(HERE, '..', 'src', 'cpu', 'wcnet_priv_font.h')).read()
    out = {}
    for m in re.finditer(r"\{ (\d+), (\d+), (\d+), (\d+), \{ ([^}]*) \}, (\d+), \{", src):
        ch, w, h, row = map(int, m.group(1, 2, 3, 4))
        out[ch] = (w, h, row, [int(x, 16) for x in m.group(5).split(',')])
    return out


def main():
    h, glyphs = read_dump(sys.argv[1])
    assert h == 6, 'the cockpit font is 6 rows high'
    users = user_glyphs()
    entries, differ = [], []
    for ch in sorted(users):
        if ch not in glyphs:
            continue
        w, rows = glyphs[ch]
        assert w <= 16 and all(len(r) == w and set(r) <= set('#.') for r in rows), chr(ch)
        bits = [sum(1 << x for x in range(w) if r[x] == '#') for r in rows]
        entries.append((ch, w, bits))
        uw, uh, urow, ubits = users[ch]
        want = [0] * 6
        for k, b in enumerate(ubits):
            want[1 + urow + k] = b << 1
        if w - 1 != uw or bits != want:
            differ.append(chr(ch))
    out = sys.stdout
    out.write("// Written by scripts/wchfont.py from a font dump of Wing Commander 1 (WCHIRESFONTS=1): the glyphs of its cockpit\n"
              "// font (height 6: a blank row, four of capitals, one for descenders; a blank column first) for each character\n"
              "// the user's own font has (wcnet_priv_font.h).  A cockpit letter is drawn in the user's font only when the game\n"
              "// draws exactly the glyph kept here.  Per character: its width and six rows (bit 0 the left).\n"
              "// Glyphs not quite the Privateer ones the user's letters were drawn from (the same letters): %s\n"
              "#ifndef WCNET_WCH_FONT_H_\n#define WCNET_WCH_FONT_H_\n\n"
              "struct WchGlyph {\n    unsigned char ch, w;\n    unsigned short rows[6];\n};\n"
              "static const WchGlyph kWchGlyphs[] = {\n" % ' '.join(differ))
    for ch, w, bits in entries:
        out.write("    { %d, %d, { %s } },   // %r\n" % (ch, w, ', '.join('0x%04X' % b for b in bits), chr(ch)))
    out.write("};\nenum { kWchGlyphCount = sizeof(kWchGlyphs) / sizeof(kWchGlyphs[0]) };\n\n#endif\n")


main()
