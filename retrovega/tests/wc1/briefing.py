#!/usr/bin/env python3
"""The old game's words, read from the user's copy: GAMEDAT/BRIEFING.000 (the funeral and promotion scenes, then a
mission at a time: its briefing, debrief by result, and the bar and barracks talk).  For the shape of our scenes, which
are new writing: docs/engine.md "Lines".

    tests/wc1/briefing.py FILE [ENTRY ...]        print the spoken lines of those entries (all without)

The file: a size, a table of 3-byte offsets each with a flag byte (0xFF: empty), then the entries, each a 4-byte size
and LZW (9 to 12 bits, low bits first, 256 clear, 257 end).  An entry holds each line with its mouth shapes and timing,
which are left out here."""
import re
import struct
import sys


def lzw(b):
    out, tab, nxt, bits, pos, prev = bytearray(), {}, 258, 9, 0, None
    while (pos >> 3) < len(b):
        c = (int.from_bytes(b[pos >> 3:(pos >> 3) + 4], 'little') >> (pos & 7)) & ((1 << bits) - 1)
        pos += bits
        if c == 256:
            tab, nxt, bits, prev = {}, 258, 9, None
            continue
        if c == 257:
            break
        e = bytes([c]) if c < 256 else tab[c] if c in tab else prev + prev[:1]
        out += e
        if prev is not None and nxt < 4096:
            tab[nxt] = prev + e[:1]
            nxt += 1
            if nxt >= (1 << bits) and bits < 12:
                bits += 1
        prev = e
    return bytes(out)


def entries(path):
    d = open(path, 'rb').read()
    size = struct.unpack('<I', d[:4])[0]
    first = struct.unpack('<I', d[4:8])[0] & 0xFFFFFF
    t = [struct.unpack('<I', d[i:i + 4])[0] for i in range(4, first, 4)]
    t = [(v & 0xFFFFFF, v >> 24) for v in t]
    for k, (o, f) in enumerate(t):
        if f == 0xFF:
            continue
        end = next((e[0] for e in t[k + 1:] if e[1] != 0xFF), size)
        yield k, lzw(d[o + 4:end])


def spoken(data):
    for m in re.finditer(rb'[\x20-\x7e]{3,}', data):
        s = m.group().decode()
        if not re.fullmatch(r'[a-z0-9$]+', s) and not re.fullmatch(r'[A-Z0-9,]+', s) and not s.startswith('RA'):
            yield s


if __name__ == '__main__':
    want = {int(a) for a in sys.argv[2:]}
    for k, data in entries(sys.argv[1]):
        if not want or k in want:
            print('===== entry', k)
            for s in spoken(data):
                print(s)
