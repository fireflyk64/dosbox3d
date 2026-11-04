#!/usr/bin/env python3
"""The campaign's map at the old game's own size (the user, 2026-11-04: "Yes lets do the full scale"): every nav point,
ship offset and field of data/missions.txt back to the old game's numbers, read from its mission records
(tests/wc1/dump-all.sh's dumps): tests/wc1/fullscale.py DUMPDIR [--write].

Until then the map was 0.45 of the old game's (port.py, places.py: a nav point was kept in 16 bits of km), while the
ships flew its speeds, so a leg flown by hand took under half its time.  The fields were at its own radius and offset
about our nav points already (2026-11-03), some made smaller to keep a point of ours out: at the old game's map they
are its own again, radius and all.

 - a nav line: the old nav point it was scaled from (places.py's match: the scaled place and the hidden flag), its place
   and its reach (the record's +43, 10,000 to 20,000 km: within it the point is reached, and the old autopilot stops at
   it: measured 10,389, 15,189 and 15,335 km short against 10,000, 15,000 and 15,000);
 - a ship line's at=x,y,z: the old offset it was scaled from (a leader's in that nav point's list), else x / 0.45;
 - a field line: the old field (types 22, 23) whose place, scaled about its nav point, it is: that one's place about the
   old nav point and its radius; level= and density= kept.
Anything it cannot match it scales by 1 / 0.45 and says so."""
import os
import re
import struct
import sys

SCALE = 0.45
NODES = ['ENIGMA', 'CEPHID', 'SHELTON', 'CRUCIBL', 'OLDZIEY', 'DEFIANC', 'LIXIS', 'ATARAXI', 'RIGEL', 'BARNARD', 'WOLF359',
         'SHMRN', 'LUYTEN']


def s24(b, o):
    v = b[o] | b[o + 1] << 8 | b[o + 2] << 16
    return v - (1 << 24) if v & 0x800000 else v


def small(v):
    """the 0.45 map's number for the old one (port.py's)"""
    return max(-32000, min(32000, round(v * SCALE)))


def read(path):
    """the old nav points, the ship offsets, and the fields: one for each nav point whose list has it (a record in two
    lists comes about whichever is entered), about nav 0 when none has it"""
    d = open(path, 'rb').read()
    navs, lists = {}, {}
    for n in range(16):
        r = 0x687C + 0x4D * n
        rec = d[r:r + 0x4D]
        name = rec[:16].split(b'\0')[0].decode('latin-1')
        if name:
            navs[n] = dict(at=(s24(d, r + 32), s24(d, r + 36), s24(d, r + 40)), hidden=name.startswith('.') or rec[30] == 0,
                           reach=struct.unpack_from('<H', d, r + 43)[0])
            lists[n] = [s for s in struct.unpack_from('<10H', d, r + 0x39) if s != 0xFFFF]
    offs, fields = set(), []
    for m in range(32):
        r = 0x6E80 + 0x2A * m
        t = struct.unpack_from('<h', d, r)[0]
        if not any(d[r:r + 0x2A]) or t < 0:
            continue
        off = (s24(d, r + 10), s24(d, r + 14), s24(d, r + 18))
        if t in (22, 23):
            radius = struct.unpack_from('<h', d, r + 28)[0]
            for n in [n for n, l in lists.items() if m in l] or [0]:
                fields.append(dict(nav=n, off=off, radius=radius, kind='mine' if t == 23 else 'asteroid'))
        else:
            offs.add(off)
    return navs, offs, fields


def scaled(v):
    return round(v / SCALE)


def main():
    dumps = sys.argv[1]
    write = '--write' in sys.argv
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'data', 'missions.txt')
    lines = open(path).read().split('\n')
    out, cur, notes, mid = [], None, [], None
    navs = offs = fields = None
    ours = {}
    i = 0
    while i < len(lines):
        l = lines[i]
        m = re.match(r'mission\s+(\S+)', l)
        if m:
            mid = m[1]
            node = next(n for n in NODES if mid.startswith(n[:7]))
            dump = os.path.join(dumps, 's%d-m%d' % (NODES.index(node) + 1, int(mid[len(node[:7]):]) - 1), 't1.bin')
            navs, offs, fields = read(dump)
            # our nav ids against the old ones (the scaled place and the hidden flag), and our nav places as they are
            block = []
            j = i + 1
            while j < len(lines) and not lines[j].startswith('mission'):
                block.append(lines[j])
                j += 1
            ours = {0: ((0, 0, 0), 0)}
            for w in (b.split() for b in block):
                if w and w[0] == 'nav':
                    k, at, hid = int(w[1]), tuple(int(v) for v in w[2:5]), 'hidden' in w
                    old = next((n for n, v in navs.items() if n and tuple(map(small, v['at'])) == at and v['hidden'] == hid), None)
                    ours[k] = (at, old)
            out.append(l)
            i += 1
            continue
        w = l.split()
        if mid and w and w[0] == 'nav':
            k = int(w[1])
            at, old = ours[k]
            new = navs[old]['at'] if old is not None else tuple(scaled(v) for v in at)
            reach = navs[old]['reach'] if old is not None else 15000
            if not reach:               # (three hidden points at the launch have 0: the way home's encounters, which ours
                reach = 5000            # bring within 5,000 km, as they did; a point of 0 would never be reached)
            if old is None:
                notes.append('%s nav %d: no old nav point at %s: scaled' % (mid, k, at))
            out.append('nav      %d %d %d %d%s reach=%d' % (k, new[0], new[1], new[2], ' hidden' if 'hidden' in w else '', reach))
        elif mid and w and w[0] == 'ship' and any(o.startswith('at=') for o in w):
            res = []
            for o in w[1:]:
                if o.startswith('at='):
                    at = tuple(int(v) for v in o[3:].split(','))
                    new = next((v for v in offs if tuple(map(small, v)) == at), None)
                    if new is None:
                        new = tuple(scaled(v) for v in at)
                        notes.append('%s ship %s: at=%s in no old record: scaled to %s' % (mid, w[1], o[3:], new))
                    o = 'at=%d,%d,%d' % new
                res.append(o)
            out.append('ship     ' + ' '.join(res))
        elif mid and w and w[0] == 'field':
            kind, at, radius, opts = w[1], tuple(int(v) for v in w[2:5]), int(w[5]), w[6:]
            hit = None
            for f in fields:
                for k, (base, o) in ours.items():
                    if o == f['nav'] or (k == 0 and f['nav'] == 0):
                        if tuple(base[c] + f['off'][c] for c in range(3)) == at:
                            hit = (f, f['nav'])
                            break
                if hit:
                    break
            if hit is None:
                # (one the small map's clamp moved: the old field of its kind nearest it there, about its nav point)
                def gap(f):
                    o = navs[f['nav']]['at'] if f['nav'] in navs else (0, 0, 0)
                    return sum((small(o[c] + f['off'][c]) - at[c]) ** 2 for c in range(3))
                f = min((f for f in fields if f['kind'] == kind), key=gap)
                hit = (f, f['nav'])
                notes.append('%s field at %s: no exact match (the small map clamped it): the old %s field about nav %d'
                             % (mid, at, kind, f['nav']))
            if hit:
                f, n = hit
                o = navs[n]['at'] if n in navs else (0, 0, 0)
                new = tuple(o[c] + f['off'][c] for c in range(3))
                r = f['radius']
                if r != radius:
                    notes.append('%s field at %s: radius %d again (was %d, made smaller on the small map)' % (mid, new, r, radius))
            out.append('field    %s %d %d %d %d%s' % (kind, new[0], new[1], new[2], r, ''.join(' ' + o for o in opts)))
        else:
            out.append(l)
        i += 1
    for n in notes:
        print(n)
    if write:
        open(path, 'w').write('\n'.join(out))


main()
