#!/usr/bin/env python3
"""Where each wave starts, from the old game's own records (tests/wc1/dump-all.sh's dumps), onto data/missions.txt's ship
lines, which keep their written options: tests/wc1/places.py DUMPDIR [--write].

Two things the first port (port.py) got wrong, and 2026-11-01's report of wingmen dead seconds after launch found:
 - a ship in no nav point's list was put at nav 0, the launch.  In the old game a ship is brought in only from a nav point's
   list, so those never fly: 11 of the 15 missions with "enemies at the launch" had none (Shmrn 3's four Convolutions, the
   wave that killed Torch in 1.7 s, are in no list).  They are dropped.
 - the offset from the nav point (+10, +14, +18, s24) was left out, so a wave spawned on top of its nav point, and at nav 0 on
   top of the player.  A wave's offset is its leader's (at the map's 0.45 until 2026-11-04, its own since): each leader (behaviour not 4) gets its own line.
Escort targets keep their place (their routes start from the nav point), and so do lines with no record to match."""
import os
import re
import struct
import sys

SCALE = 1.0                             # (0.45 until 2026-11-04: the map is the old game's own since, tests/wc1/fullscale.py)
NODES = ['ENIGMA', 'CEPHID', 'SHELTON', 'CRUCIBL', 'OLDZIEY', 'DEFIANC', 'LIXIS', 'ATARAXI', 'RIGEL', 'BARNARD', 'WOLF359',
         'SHMRN', 'LUYTEN']
TYPES = {0: 'SCHROED', 1: 'LANCELT', 2: 'FRANKLN', 3: 'GAWAIN', 8: 'VIGILNC', 9: 'AGIS', 10: 'NICANDR', 11: 'ARISTON',
         12: 'CONVOLN', 13: 'CLEOMEN', 6: 'OX', 4: 'KAHAN', 5: 'AGASICL', 7: 'KAHAN', 14: 'CHARILS', 15: 'AGASICL',
         16: 'AGASICL', 17: 'AGASICL', 18: 'ANAXIDM', 19: 'CHARILS', 21: 'LEONIDS'}


def s24(b, o):
    v = b[o] | b[o + 1] << 8 | b[o + 2] << 16
    return v - (1 << 24) if v & 0x800000 else v


def km(v):
    return round(v * SCALE)


def read(path):
    d = open(path, 'rb').read()
    navs = {}
    for n in range(16):
        r = 0x687C + 0x4D * n
        rec = d[r:r + 0x4D]
        name = rec[:16].split(b'\0')[0].decode('latin-1')
        if any(rec):                    # (a record with no name is a hidden point too: tests/wc1/waves.py)
            navs[n] = dict(at=(km(s24(d, r + 32)), km(s24(d, r + 36)), km(s24(d, r + 40))),
                           hidden=not name or name.startswith('.') or rec[30] == 0,
                           ships=[s for s in struct.unpack_from('<10H', d, r + 0x39) if s != 0xFFFF])
    ships = {}
    for m in range(32):
        r = 0x6E80 + 0x2A * m
        t = struct.unpack_from('<h', d, r)[0]
        if not any(d[r:r + 0x2A]) or t < 0:
            continue
        side, leader, beh = struct.unpack_from('<hhh', d, r + 2)
        ships[m] = dict(t=t, side=side, leader=leader, beh=beh, off=(km(s24(d, r + 10)), km(s24(d, r + 14)), km(s24(d, r + 18))))
    return navs, ships, struct.unpack_from('<H', d, 0x9560)[0]


def wanted(navs, ships, player, ids):
    """(nav id, kind, side) -> [(count, offset), ...] and what was left in no list"""
    carrier = {m for m, s in ships.items() if s['t'] == 8}
    escorts = {m for m, s in ships.items() if s['beh'] == 3 and s['leader'] in carrier}
    escorts |= {m for m, s in ships.items() if s['leader'] in escorts}
    followers = [m for m, s in ships.items() if s['leader'] == player]
    listed = {}
    for n, v in navs.items():
        for m in v['ships']:
            listed[m] = n
    out, orphans, group = {}, 0, None
    for m, s in sorted(ships.items()):
        if s['t'] in (22, 23) or s['t'] == 8 or m == player or m in escorts or (followers and m == followers[0]):
            continue
        if s['t'] not in TYPES or s['beh'] in (6, 7):
            continue
        if m not in listed:
            orphans += s['side'] != 0
            continue
        key = (ids[listed[m]], TYPES[s['t']], 1 if s['side'] else 0)
        if s['beh'] == 4 and group and group[0] == key:
            group[1][0] += 1
        else:
            group = (key, [1, s['off']])
            out.setdefault(key, []).append(group[1])
    return out, orphans


def main():
    dumps = sys.argv[1]
    write = '--write' in sys.argv
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'data', 'missions.txt')
    lines = open(path).read().split('\n')
    res, i, tot_orph = [], 0, 0
    while i < len(lines):
        m = re.match(r'mission\s+(\S+)', lines[i])
        if not m:
            res.append(lines[i])
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not lines[j].startswith('mission'):
            j += 1
        block = lines[i:j]
        i = j
        mid = m[1]
        node = next(n for n in NODES if mid.startswith(n[:7]))
        dump = os.path.join(dumps, 's%d-m%d' % (NODES.index(node) + 1, int(mid[len(node[:7]):]) - 1), 't1.bin')
        navs, ships, player = read(dump)
        have = [(int(w[1]), tuple(int(v) for v in w[2:5]), 'hidden' in w) for w in (l.split() for l in block) if w and w[0] == 'nav']
        ids, used = {0: 0}, set()
        for n, v in sorted(navs.items()):
            if n == 0:
                continue
            for k, at, hid in have:
                if k not in used and at == v['at'] and hid == v['hidden']:
                    ids[n] = k
                    used.add(k)
                    break
            else:
                raise SystemExit('%s: the old nav %d at %s is not in missions.txt' % (mid, n, v['at']))
        want, orphans = wanted(navs, ships, player, ids)
        tot_orph += orphans
        out, n_lines, notes = [], 0, []
        for l in block:
            w = l.split()
            if not w or w[0] != 'ship':
                out.append(l)
                continue
            n_lines += 1
            opts = [o for o in w[4:] if not o.startswith(('count=', 'at='))]
            count = next((int(o[6:]) for o in w[4:] if o.startswith('count=')), 1)
            key = (int(w[3][3:]), w[1], int(w[2]))
            if w[1] in ('player', 'wingman') or any(o.startswith('role=') for o in opts):
                out.append(l)
                continue
            if key in want:
                groups = want.pop(key)
                if sum(g[0] for g in groups) != count:
                    notes.append('%s nav%d: %d in the line, %d in the old lists' % (w[1], key[0], count, sum(g[0] for g in groups)))
                n_lines += len(groups) - 1
                for c, off in groups:
                    out.append('ship     %s %d nav%d%s%s%s' % (w[1], key[2], key[0], ' count=%d' % c if c > 1 else '',
                                                              ' at=%d,%d,%d' % off if any(off) else '',
                                                              ''.join(' ' + o for o in opts)))
            elif key[0] == 0 and key[2] == 1:
                notes.append('%s x%d at the launch: in no old list, dropped' % (w[1], count))
                n_lines -= 1
            else:
                notes.append('kept as it is: %s' % ' '.join(w[1:]))
                out.append(l)
        for key, g in want.items():
            notes.append('in the old lists, no line: %s' % (key,))
        print('%-9s %2d ship lines; %s' % (mid, n_lines, '; '.join(notes)))
        res += out
    print('in no list, enemies:', tot_orph)
    if write:
        open(path, 'w').write('\n'.join(res))


main()
