# wcshapes.py: Origin containers of shape tables (WC1/WC2 GAMEDAT *.Vnn, *.VGA) -> shapes (scripts/wchsheet/sheet.py)
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'retrovega', 'tools'))
from wcformat import lzw_decode

def container(d):
    """[(flag, bytes)] of a container (dword size, dword offset|flag<<24 ...)."""
    first = struct.unpack_from('<I', d, 4)[0] & 0xFFFFFF
    n = (first - 4) // 4
    ents = [struct.unpack_from('<I', d, 4 + 4 * i)[0] for i in range(n)]
    out = []
    for i, e in enumerate(ents):
        off, fl = e & 0xFFFFFF, e >> 24
        nxt = min([x & 0xFFFFFF for x in ents if (x & 0xFFFFFF) > off] + [len(d)])
        blk = d[off:nxt]
        if fl == 0x01 or fl == 0x20 or fl == 0x03:
            try:
                blk = lzw_decode(blk[4:], struct.unpack_from('<I', blk, 0)[0])
            except Exception:
                pass
        out.append((fl, blk))
    return out

def shapes(table):
    """The shapes of a shape table: (w, h, left, top, {(x,y): colour})."""
    size = struct.unpack_from('<I', table, 0)[0]
    first = struct.unpack_from('<I', table, 4)[0]
    n = (first - 4) // 4
    out = []
    for i in range(n):
        off = struct.unpack_from('<I', table, 4 + 4 * i)[0] & 0xFFFFF
        if off + 8 > len(table):
            out.append(None); continue
        x2, x1, y1, y2 = struct.unpack_from('<hhhh', table, off)
        w, h = x1 + x2 + 1, y1 + y2 + 1
        if w <= 0 or h <= 0 or w > 640 or h > 400:
            out.append(None); continue
        px = {}
        p = off + 8
        while p + 6 <= len(table):
            key, vx, vy = struct.unpack_from('<hhh', table, p); p += 6
            if key == 0: break
            x = vx + x1; y = vy + y1; n2 = key >> 1
            if not (key & 1):
                for _ in range(n2):
                    px[(x, y)] = table[p]; p += 1; x += 1
                continue
            while n2 > 0 and p < len(table):
                sub = table[p]; p += 1; run = sub >> 1
                if not run: break
                n2 -= run
                if sub & 1:
                    c = table[p]; p += 1
                    for _ in range(run): px[(x, y)] = c; x += 1
                else:
                    for _ in range(run): px[(x, y)] = table[p]; p += 1; x += 1
        out.append((w, h, x1, y1, px))
    return out

def palette(path='/home/sandbox/dev/dosbox3d/wc/GAMEDAT/GAME.PAL'):
    d = open(path, 'rb').read()
    d = d[-768:] if len(d) >= 768 else d
    return [(d[3*i] << 2 & 255, d[3*i+1] << 2 & 255, d[3*i+2] << 2 & 255) for i in range(256)]
