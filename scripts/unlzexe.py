#!/usr/bin/env python3
"""An LZEXE 0.91 program unpacked (WOLF3D.EXE is one): the load image and its relocations as an ordinary MZ file.

    scripts/unlzexe.py WOLF3D.EXE WOLF3D.UNP

The algorithm is Fabrice Bellard's, as UNLZEXE (Mitugu Kurizono, 1990) reads it.  The layer never needs the file: the
program unpacks itself in memory; this is for reading its code (scripts/wolfsig.py, a disassembler)."""
import struct
import sys


def unlzexe(d):
    h = struct.unpack_from('<14H', d, 0)
    if d[0x1C:0x20] != b'LZ91':
        raise ValueError('not an LZEXE 0.91 file')
    hdr = h[4] << 4
    cs0 = hdr + (h[0x0B] << 4)                      # CS:0000 of the packed file: the unpacker's facts
    inf = struct.unpack_from('<8H', d, cs0)
    # the relocations, packed
    rel = []
    p = cs0 + 0x158
    off = seg = 0
    while True:
        span = d[p]; p += 1
        if span == 0:
            span = struct.unpack_from('<H', d, p)[0]; p += 2
            if span == 0:
                seg += 0x0FFF
                continue
            if span == 1:
                break
        off += span
        seg += (off & ~0x0F) >> 4
        off &= 0x0F
        rel.append((off, seg))
    # the image
    p = hdr + ((h[0x0B] - inf[4]) << 4)
    out = bytearray()
    buf = struct.unpack_from('<H', d, p)[0]; p += 2
    count = 16

    def bit():
        nonlocal buf, count, p
        b = buf & 1
        count -= 1
        if count == 0:
            buf = struct.unpack_from('<H', d, p)[0]; p += 2
            count = 16
        else:
            buf >>= 1
        return b

    while True:
        if bit():
            out.append(d[p]); p += 1
            continue
        if not bit():
            n = bit() << 1
            n |= bit()
            n += 2
            span = d[p] | 0xFF00; p += 1
        else:
            span = d[p]; n = d[p + 1]; p += 2
            span |= ((n & ~7) << 5) | 0xE000
            n = (n & 7) + 2
            if n == 2:
                n = d[p]; p += 1
                if n == 0:
                    break
                if n == 1:
                    continue
                n += 1
        span -= 0x10000
        for _ in range(n):
            out.append(out[span])
    return out, rel, inf


def main():
    d = open(sys.argv[1], 'rb').read()
    img, rel, inf = unlzexe(d)
    h = list(struct.unpack_from('<14H', d, 0))
    relbytes = b''.join(struct.pack('<HH', o, s) for o, s in rel)
    hdrsize = (0x1C + len(relbytes) + 0x1FF) & ~0x1FF
    total = hdrsize + len(img)
    packed_load = ((h[2] - 1) * 512 + (h[1] or 512) if h[1] else h[2] * 512) - (h[4] << 4)
    extra = h[5] * 16 + packed_load - len(img)      # what DOS gives past the image (the BSS and the stack)
    oh = [0x5A4D, total % 512, (total + 511) // 512, len(rel), hdrsize >> 4, max(0, (extra + 15) >> 4), 0xFFFF, inf[3], inf[2], 0,
          inf[0], inf[1], 0x1C, 0]
    with open(sys.argv[2], 'wb') as f:
        f.write(struct.pack('<14H', *oh) + relbytes)
        f.write(b'\0' * (hdrsize - 0x1C - len(relbytes)))
        f.write(img)
    print('image %d bytes, %d relocations, entry %04X:%04X, stack %04X:%04X, header %d' % (len(img), len(rel), inf[1], inf[0], inf[3], inf[2], hdrsize))


if __name__ == '__main__':
    main()
