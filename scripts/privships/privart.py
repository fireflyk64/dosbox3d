"""Privateer's art files, read and written (docs/privateer.md section 42): the TRE, IFF forms, shapes, the cockpits'
window templates.  Used by the Stiletto cockpit's prototype (stilcock.py), whose steps the emulator repeats in C++
(src/cpu/wcnet_priv_ships.cpp)."""
import struct

def tre_load(path):
    d = open(path, 'rb').read()
    n, = struct.unpack_from('<I', d, 0)
    out = {}
    for i in range(n):
        e = d[8 + 74 * i: 8 + 74 * (i + 1)]
        name = e[1:66].split(b'\0')[0].decode('latin1')
        off, size = struct.unpack_from('<II', e, 66)
        out[name.split('DATA\\')[-1].upper()] = d[off:off + size]
    return out

class Chunk:
    def __init__(self, tag, data=b'', kids=None, form=None):
        self.tag, self.data, self.kids, self.form = tag, data, kids, form   # form: the FORM's type (kids then)
    def find(self, *path):
        c = self
        for p in path:
            n = 0
            if isinstance(p, tuple): p, n = p
            m = [k for k in c.kids if (k.form or k.tag) == p]
            c = m[n]
        return c
    def all(self, name): return [k for k in self.kids if (k.form or k.tag) == name]
    def dump(self, dep=0, maxb=24):
        if self.form:
            print(' ' * dep + 'FORM ' + self.form)
            for k in self.kids: k.dump(dep + 2, maxb)
        else: print(' ' * dep + self.tag, len(self.data), self.data[:maxb].hex(' '))

def iff_parse(d, o=0, end=None):
    end = len(d) if end is None else end
    out = []
    while o + 8 <= end:
        tag = d[o:o + 4].decode('latin1'); n, = struct.unpack_from('>I', d, o + 4)
        if tag == 'FORM': out.append(Chunk('FORM', kids=iff_parse(d, o + 12, o + 8 + n), form=d[o + 8:o + 12].decode('latin1')))
        else: out.append(Chunk(tag, d[o + 8:o + 8 + n]))
        o += 8 + n + (n & 1)
    return out

def iff_build(c):
    if c.form:
        body = c.form.encode('latin1') + b''.join(iff_build(k) for k in c.kids)
        return b'FORM' + struct.pack('>I', len(body)) + body
    return c.tag.encode('latin1') + struct.pack('>I', len(c.data)) + c.data + (b'\0' if len(c.data) & 1 else b'')

def pal_load(d):
    return [tuple(min(255, v * 4 + (v >> 4)) for v in d[4 + 3 * i: 7 + 3 * i]) for i in range(256)]

# A shape: a header of four words (the extents right, left, up and down of its origin) and rows of pixels, each a
# word (pixels * 2, +1 packed), x and y from the origin; a zero word ends it.
def shape_decode(v, o=0):
    x2, x1, y1, y2 = struct.unpack_from('<4h', v, o); o += 8; pts = {}
    while True:
        h, = struct.unpack_from('<H', v, o)
        if not h: break
        x, y = struct.unpack_from('<hh', v, o + 2); o += 6; n = h >> 1
        if not h & 1:
            for i in range(n): pts[x + i, y] = v[o + i]
            o += n
        else:
            while n > 0:
                c = v[o]; o += 1; r = c >> 1
                for i in range(r): pts[x + i, y] = v[o] if c & 1 else v[o + i]
                o += 1 if c & 1 else r; x += r; n -= r
    return (x2, x1, y1, y2), pts

def shape_encode(ext, pts):
    """Rows of pixels, each stretch of a row packed: repeats of 3 or more as (count * 2 + 1, colour), the rest as
    (count * 2, the colours)."""
    out = bytearray(struct.pack('<4h', *ext))
    rows = {}
    for (x, y), c in pts.items(): rows.setdefault(y, {})[x] = c
    for y in sorted(rows):
        xs = sorted(rows[y]); i = 0
        while i < len(xs):
            j = i
            while j + 1 < len(xs) and xs[j + 1] == xs[j] + 1: j += 1
            run = bytes(rows[y][x] for x in xs[i:j + 1])
            out += struct.pack('<Hhh', len(run) * 2 + 1, xs[i], y) + pack_run(run)
            i = j + 1
    out += b'\0\0'
    return bytes(out)

def pack_run(run):
    out = bytearray(); lit = bytearray(); k = 0
    def flush():
        nonlocal lit
        while lit:
            part = lit[:127]; out.append(len(part) * 2); out.extend(part); lit = lit[127:]
    while k < len(run):
        n = 1
        while k + n < len(run) and run[k + n] == run[k] and n < 127: n += 1
        if n >= 3:
            flush(); out.append(n * 2 + 1); out.append(run[k])
        else: lit.extend(run[k:k + n])
        k += n
    flush()
    return bytes(out)

def pack_shapes(v):
    """A shape pack: its length, a table of offsets (24 bits; the top byte a flag), the shapes."""
    first = struct.unpack_from('<I', v, 4)[0] & 0xffffff
    return [struct.unpack_from('<I', v, 4 + 4 * j)[0] & 0xffffff for j in range((first - 4) // 4)]

def pack_build(shapes, flag=0):
    n = len(shapes); o = 4 + 4 * n; tab = b''; body = b''
    for s in shapes:
        tab += struct.pack('<I', (o + len(body)) | (flag << 24))
        body += s
    return struct.pack('<I', o + len(body)) + tab + body

# A cockpit view's template: where space shows.  Words: the count of words, 2, the width, the rows, two zeros; then
# spans (left, row, width); 0xFFFF ends it.
def tplt_decode(t):
    n, two, w, h, a, b = struct.unpack_from('<6H', t, 0)
    spans = []
    o = 12
    while struct.unpack_from('<H', t, o)[0] != 0xFFFF:
        spans.append(struct.unpack_from('<3H', t, o)); o += 6
    return (two, w, h, a, b), spans

def tplt_encode(head, spans):
    two, w, h, a, b = head
    body = b''.join(struct.pack('<3H', *s) for s in spans) + b'\xff\xff'
    return struct.pack('<6H', (12 + len(body)) // 2, two, w, h, a, b) + body
