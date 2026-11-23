#!/usr/bin/env python3
# privfont.py: writes src/cpu/wcnet_priv_font.h, the letters of Privateer's high-resolution cockpit (docs/privateer.md
# section 28): the user's own font, drawn by hand at twice the game's (scripts/privfont/: the metrics, the atlas, its
# README and REVISION), one-pixel strokes, each letter in the game's own letter's box.  For each character the table
# keeps the game's glyph it was drawn from (source_bitmap), so that the emulator replaces a letter only when the game
# draws exactly that glyph.
# The metrics' shift_up (2x rows; the user, 2026-11-23, of their letters: "It was an accident I made them lower", "Can you
# shift the font I provided up by the equivalent pixels then. That way it's a data change") puts the cells that many
# rows above the game's letter's top: kPrivGlyphShiftUp.
#   python3 scripts/privfont.py > src/cpu/wcnet_priv_font.h
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def bits(row):
    return sum(1 << i for i, c in enumerate(row) if c == '1')


def main():
    m = json.load(open(os.path.join(HERE, 'privfont', 'font-replacement-metrics.json')))
    assert m['scale_from_source'] == 2 and m['stroke_width'] == 1
    out = sys.stdout
    out.write("// Written by scripts/privfont.py from scripts/privfont/font-replacement-metrics.json: the letters of Privateer's\n"
              "// high-resolution cockpit, the user's own font, twice the game's.  Per character: the character, the game's glyph\n"
              "// it replaces (width, rows, its first row's place below the capitals' top, its rows: bit 0 the left), then the\n"
              "// letter's own width and ten rows from the capitals' top (bit 0 the left).\n"
              "#ifndef WCNET_PRIV_FONT_H_\n#define WCNET_PRIV_FONT_H_\n\n"
              "struct PrivGlyph {\n    unsigned char ch, srcW, srcH, srcRow;\n    unsigned char src[5];\n"
              "    unsigned char w;\n    unsigned short rows[10];\n};\n")
    glyphs = sorted(m['glyphs'].items(), key=lambda kv: ord(kv[0]))
    out.write("static const PrivGlyph kPrivGlyphs[] = {\n")
    for ch, g in glyphs:
        src = g['source_bitmap']
        rows = g['bitmap']
        assert len(rows) == 10 and all(len(r) == g['cell_width'] for r in rows) and g['cell_width'] <= 16, ch
        # the game's glyph: its inked rows (the source cell's rows from the first to the last with ink)
        inked = [i for i, r in enumerate(src) if '1' in r]
        first, last = inked[0], inked[-1]
        srcW = len(src[0])
        assert last - first + 1 <= 5 and srcW * 2 == g['cell_width'], ch
        out.write("    { %d, %d, %d, %d, { %s }, %d, { %s } },   // %r\n" % (
            ord(ch), srcW, last - first + 1, first, ", ".join("0x%02X" % bits(src[i]) for i in range(first, last + 1)),
            g['cell_width'], ", ".join("0x%04X" % bits(r) for r in rows), ch))
    out.write("};\nenum { kPrivGlyphCount = sizeof(kPrivGlyphs) / sizeof(kPrivGlyphs[0]) };\n")
    shift = int(m.get('shift_up', 0))
    assert 0 <= shift <= 2
    out.write("enum { kPrivGlyphShiftUp = %d };      // (the cells' top, in 2x rows, above the game's letter's top)\n\n#endif\n" % shift)


if __name__ == '__main__':
    main()
