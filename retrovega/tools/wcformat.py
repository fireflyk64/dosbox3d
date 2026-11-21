"""wcformat.py: the old Wing Commander games' block container and run-length sprites, read and written.

RetroVega's ship views (art/ships/SHIPnn.RV, tools/pack.py) are in these formats, as Origin's own ships were, so the
game's drawing code and the multiplayer layer read them alike.  (dosbox3d's scripts/wc2data.py, the rest of Wing
Commander II's files, takes these from here.)

* The block container: a dword of the file's size, a table of dword entries up to the first block (the low 24 bits
  an offset, the high byte a flag: 0xE0 plain data, 0xC1 a table of shapes, 0xFF an empty block, 0x20 LZW-compressed
  with a dword of plain size in front), then the blocks.  `read_container`, `compose_container`, `blocks_of`.
* The LZW of the compressed blocks: codes of 9 to 12 bits, LSB first, 256 resets the dictionary, 257 ends.
* A sprite: four words (x2, x1, y1, y2), then records of (key, x, y) and their pixels: an even key 2n is n plain
  pixels, an odd key 2n+1 n pixels in sub-runs (an even byte 2k: k plain pixels; odd 2k+1: k of the next colour);
  key 0 ends.  `decode_sprite`, `encode_sprite`.
"""
import struct


def lzw_decode(src, expect=None):
    out = bytearray()
    dict_ = []
    nbits = 9
    prev = None

    def reset():
        nonlocal dict_, nbits, prev
        dict_ = [bytes([i]) for i in range(256)] + [b"", b""]
        nbits = 9
        prev = None

    reset()
    bitpos = 0
    total_bits = len(src) * 8
    while bitpos + nbits <= total_bits:
        code = 0
        for i in range(nbits):
            b = bitpos + i
            code |= ((src[b >> 3] >> (b & 7)) & 1) << i
        bitpos += nbits
        if code == 256:
            reset()
            continue
        if code == 257:
            break
        if code < len(dict_):
            entry = dict_[code]
        elif code == len(dict_) and prev is not None:
            entry = prev + prev[:1]
        else:
            raise ValueError("bad LZW code %d at bit %d" % (code, bitpos))
        out += entry
        if prev is not None:
            dict_.append(prev + entry[:1])
        prev = entry
        if len(dict_) == (1 << nbits) and nbits < 12:
            nbits += 1
        if expect is not None and len(out) >= expect:
            break
    return bytes(out)


def read_container(path):
    """[(flag, bytes)] with compressed blocks decompressed (their flag kept)."""
    d = open(path, "rb").read() if isinstance(path, str) else path
    first = struct.unpack("<I", d[4:8])[0] & 0xFFFFFF
    n = (first - 4) // 4
    entries = [struct.unpack("<I", d[4 + 4 * i:8 + 4 * i])[0] for i in range(n)]
    out = []
    for i in range(n):
        off, flag = entries[i] & 0xFFFFFF, entries[i] >> 24
        nxt = (entries[i + 1] & 0xFFFFFF) if i + 1 < n else len(d)
        block = d[off:nxt]
        if flag == 0x20:
            expect = struct.unpack("<I", block[:4])[0]
            block = lzw_decode(block[4:], expect)
        out.append((flag, block))
    return out


def compose_container(parts, flags=None):
    """The file for blocks `parts` (bytes) with `flags` (0xE0 when None)."""
    n = len(parts)
    off = 4 + 4 * n
    table = []
    for i, p in enumerate(parts):
        fl = 0xE0 if flags is None else flags[i]
        if fl == 0x20:
            fl = 0xE0   # we never write compressed blocks
        table.append(off | (fl << 24))
        off += len(p)
    return struct.pack("<I", off) + b"".join(struct.pack("<I", e) for e in table) + b"".join(parts)


def write_container(path, parts, flags=None):
    with open(path, "wb") as f:
        f.write(compose_container(parts, flags))


def blocks_of(path):
    """The blocks alone."""
    return [b for _, b in read_container(path)]


def decode_sprite(data):
    """(x2, x1, y1, y2, rows) with rows a dict y -> {x: colour} in origin coordinates."""
    x2, x1, y1, y2 = struct.unpack_from("<hhhh", data, 0)
    rows = {}
    off = 8
    while off + 6 <= len(data):
        key, vx, vy = struct.unpack_from("<hhh", data, off)
        off += 6
        if key == 0:
            break
        row = rows.setdefault(vy, {})
        x = vx
        if key % 2 == 0:
            for _ in range(key // 2):
                row[x] = data[off]; off += 1; x += 1
        else:
            n = 0
            while n < key // 2 and off < len(data):
                k = data[off]; off += 1
                if k % 2 == 0:
                    for _ in range(k // 2):
                        row[x] = data[off]; off += 1; x += 1; n += 1
                else:
                    c = data[off]; off += 1
                    for _ in range(k // 2):
                        row[x] = c; x += 1; n += 1
    return x2, x1, y1, y2, rows


def _encode_span(span):
    """The cheapest packing of one row span (colour indices): (bytes, raw)
    with raw=True for an even-key record of plain pixels.  Sub-runs: a run
    of one colour costs 2 bytes, n plain pixels 1 + n; at most 127 each."""
    n = len(span)
    best = [0] + [None] * n
    how = [None] * (n + 1)
    for i in range(n):
        if best[i] is None:
            continue
        length = 1
        while length <= 127 and i + length <= n and span[i + length - 1] == span[i]:
            if best[i + length] is None or best[i] + 2 < best[i + length]:
                best[i + length], how[i + length] = best[i] + 2, (True, i)
            length += 1
        for length in range(1, min(127, n - i) + 1):
            if best[i + length] is None or best[i] + 1 + length < best[i + length]:
                best[i + length], how[i + length] = best[i] + 1 + length, (False, i)
    units, j = [], n
    while j > 0:
        run, i = how[j]
        units.append((run, span[i:j]))
        j = i
    body = bytearray()
    for run, u in reversed(units):
        body += bytes((2 * len(u) + 1, u[0])) if run else bytes((2 * len(u),)) + bytes(u)
    if len(body) >= n:
        return bytes(span), True
    return bytes(body), False


def encode_sprite(x2, x1, y1, y2, rows):
    """The inverse of decode_sprite: one record per span of drawn pixels
    (every pixel decode_sprite listed, colour 0 too: it is drawn black),
    packed as cheaply as the format allows.  Origin's own views come out a
    few per cent smaller than their files."""
    out = bytearray(struct.pack("<hhhh", x2, x1, y1, y2))
    for y in sorted(rows):
        row = rows[y]
        xs = sorted(row)
        i = 0
        while i < len(xs):
            j = i
            while j + 1 < len(xs) and xs[j + 1] == xs[j] + 1:
                j += 1
            span = [row[x] for x in xs[i:j + 1]]
            body, raw = _encode_span(span)
            out += struct.pack("<hhh", 2 * len(span) + (0 if raw else 1), xs[i], y) + body
            i = j + 1
    out += struct.pack("<h", 0)   # the end mark, as Origin's views have it
    return bytes(out)
