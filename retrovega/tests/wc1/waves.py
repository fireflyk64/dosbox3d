#!/usr/bin/env python3
"""Every enemy wave of the old game against data/missions.txt (the user, 2026-11-07, after LIXIS1's ambush of 5 Agis
was found missing: "I'm worried this is a systemic problem ... The hidden nav points are key").
    tests/wc1/waves.py DUMPDIR [MISSION ...]       (DUMPDIR: tests/wc1/dump-all.sh's, sS-mM/t1.bin)
For each mission: every nav point of the old game with enemies in its list, by place, against the enemies our nav
points at that place bring; where ours has fewer of a type, the lines that would bring the old ones (a hidden nav point
when we have none there).  Exit 1 when any mission differs.

The old tools lost waves two ways: port.py and places.py skipped a nav record with an EMPTY name (`if not name`), though
the old game lists ships there and brings them in (Dakota 1's ambush of 5 Salthi on the way to Nav 1, flown: they
stopped the autopilot 8 s out and killed the outbound Drayman), so their ships fell to "the launch" and were dropped as
"in no list"; and places.py only rewrote the ship lines it found, so a group with no line stayed out.

The lines: one per group, a group being a ship that is not a follower (behaviour 4) and the followers whose leader it
is; a ship with the attack order (behaviour 2) heads its own line, prey= the type of its target (+0x29), since only a
line's first ship carries the order against a taunt (logic.c order_hold); at= the group head's offset (+10/+14/+18).
"""
import collections
import os
import struct
import sys

NODES = ['ENIGMA', 'CEPHID', 'SHELTON', 'CRUCIBL', 'OLDZIEY', 'DEFIANC', 'LIXIS', 'ATARAXI', 'RIGEL', 'BARNARD', 'WOLF359',
         'SHMRN', 'LUYTEN']
TYPES = {0: 'SCHROED', 1: 'LANCELT', 2: 'FRANKLN', 3: 'GAWAIN', 8: 'VIGILNC', 9: 'AGIS', 10: 'NICANDR', 11: 'ARISTON',
         12: 'CONVOLN', 13: 'CLEOMEN', 6: 'OX', 4: 'KAHAN', 5: 'AGASICL', 7: 'KAHAN', 14: 'CHARILS', 15: 'AGASICL',
         16: 'AGASICL', 17: 'AGASICL', 18: 'ANAXIDM', 19: 'CHARILS', 21: 'LEONIDS'}
HERE = os.path.dirname(os.path.abspath(__file__))
MISSIONS = os.path.join(HERE, '..', '..', 'data', 'missions.txt')
NEAR = 2000                             # (km: a nav point of ours this near the old one's place is that place)


def s24(b, o):
    v = b[o] | b[o + 1] << 8 | b[o + 2] << 16
    return v - (1 << 24) if v & 0x800000 else v


def read_dump(path):
    d = open(path, 'rb').read()
    navs = []
    for n in range(16):
        r = 0x687C + 0x4D * n
        rec = d[r:r + 0x4D]
        if not any(rec):
            continue
        name = rec[:16].split(b'\0')[0].decode('latin-1')
        navs.append(dict(n=n, name=name, hidden=n > 0 and (not name or name.startswith('.') or rec[30] == 0),
                         at=(0, 0, 0) if n == 0 else (s24(d, r + 32), s24(d, r + 36), s24(d, r + 40)),
                         reach=struct.unpack_from('<H', d, r + 43)[0],
                         ships=list(dict.fromkeys(s for s in struct.unpack_from('<10H', d, r + 0x39) if s != 0xFFFF))))
    ships = {}
    for m in range(32):
        r = 0x6E80 + 0x2A * m
        t = struct.unpack_from('<h', d, r)[0]
        if not any(d[r:r + 0x2A]) or t < 0:
            continue
        side, leader, beh = struct.unpack_from('<hhh', d, r + 2)
        ships[m] = dict(m=m, t=t, side=side, leader=leader, beh=beh, target=d[r + 0x29],
                        off=(s24(d, r + 10), s24(d, r + 14), s24(d, r + 18)))
    return navs, ships


def read_ours(path):
    out, cur = {}, None
    for line in open(path):
        w = line.split()
        if not w:
            continue
        if w[0] == 'mission':
            cur = out.setdefault(w[1], dict(navs={0: ((0, 0, 0), False)}, ships=[]))
        elif cur is None:
            continue
        elif w[0] == 'nav':
            cur['navs'][int(w[1])] = (tuple(int(v) for v in w[2:5]), 'hidden' in w)
        elif w[0] == 'ship' and w[1] not in ('player', 'wingman'):
            opts = dict(o.split('=', 1) for o in w[4:] if '=' in o)
            navs = [int(w[3][3:])] + ([int(opts['also'])] if 'also' in opts else [])
            cur['ships'].append(dict(kind=w[1], side=int(w[2]), navs=navs, count=int(opts.get('count', 1))))
    return out


def near(a, b):
    return all(abs(x - y) < NEAR for x, y in zip(a, b))


def place_key(places, at):
    """the index of the place at `at` in `places` (a list of positions), added if new"""
    for i, p in enumerate(places):
        if near(p, at):
            return i
    places.append(at)
    return len(places) - 1


def groups_of(listed, ships):
    """the lines a list of enemies makes: [(kind, count, offset, prey kind or None)]"""
    heads, members = [], {}
    for m in listed:
        s = ships[m]
        if s['beh'] == 4 and s['leader'] in listed and ships.get(s['leader'], {}).get('beh') != 4:
            continue                    # (a follower: counted with its leader below)
        heads.append(m)
        members[m] = [m]
    for m in listed:
        if m not in members and ships[m]['leader'] in members:
            members[ships[m]['leader']].append(m)
    out = []
    for h in heads:
        s = ships[h]
        prey = TYPES.get(ships[s['target']]['t']) if s['beh'] == 2 and s['target'] in ships else None
        out.append((TYPES[s['t']], len(members[h]), s['off'], prey))
    return out


def main():
    dumpdir = sys.argv[1]
    only = set(sys.argv[2:])
    ours = read_ours(MISSIONS)
    differ = 0
    for si, node in enumerate(NODES):
        for mi in range(4):
            mid = '%s%d' % (node[:7], mi + 1)
            path = os.path.join(dumpdir, 's%d-m%d' % (si + 1, mi), 't1.bin')
            if mid not in ours or not os.path.exists(path) or (only and mid not in only):
                continue
            navs, ships = read_dump(path)
            mine = ours[mid]
            places = []
            # the old game's waves: its enemies grouped by the set of places whose lists bring them
            where = collections.defaultdict(list)
            for nv in navs:
                for m in nv['ships']:
                    s = ships.get(m)
                    if s and s['side'] == 1 and s['t'] in TYPES:
                        where[m].append(nv)
            old = collections.defaultdict(list)
            for m, nvs in where.items():
                old[frozenset(place_key(places, nv['at']) for nv in nvs)].append(m)
            have = collections.defaultdict(collections.Counter)
            for s in mine['ships']:
                if s['side'] == 1:
                    have[frozenset(place_key(places, mine['navs'][n][0]) for n in s['navs'])][s['kind']] += s['count']
            report = []
            next_id = max(mine['navs']) + 1
            for key, ms in sorted(old.items(), key=lambda kv: sorted(kv[0])):
                want = collections.Counter(TYPES[ships[m]['t']] for m in ms)
                short = want - have.get(key, collections.Counter())
                if not short:
                    continue
                nvs = [nv for nv in navs if any(m in nv['ships'] for m in ms)]
                report.append('  the old game brings %s at %s; ours %s there' % (
                    dict(want), ' or '.join('%d %r %s' % (nv['n'], nv['name'], nv['at']) for nv in nvs),
                    dict(have.get(key, {})) or 'nothing'))
                ids = []
                for nv in nvs:          # (our nav point at each place, the hidden one for a hidden place; else a new one)
                    k = [n for n, (p, h) in mine['navs'].items() if near(p, nv['at']) and h == nv['hidden']]
                    if k:
                        ids.append(k[0])
                    else:
                        ids.append(next_id)
                        report.append('    nav      %d %d %d %d%s reach=%d' % (next_id, nv['at'][0], nv['at'][1], nv['at'][2],
                                      ' hidden' if nv['hidden'] else '', nv['reach'] or 5000))
                        mine['navs'][next_id] = (nv['at'], nv['hidden'])
                        next_id += 1
                for kind, count, off, prey in groups_of([m for m in nvs[0]['ships'] if m in ms], ships):
                    if short.get(kind):
                        report.append('    ship     %s 1 nav%d%s%s%s%s' % (kind, ids[0], ' count=%d' % count if count > 1 else '',
                                      ' at=%d,%d,%d' % off if any(off) else '', ' prey=%s' % prey if prey else '',
                                      ' also=%d' % ids[1] if len(ids) > 1 else ''))
            if report:
                differ += 1
                print('%s (the old game\'s series %d mission %d)' % (mid, si + 1, mi))
                print('\n'.join(report))
    print('%d missions bring fewer enemies than the old game, or bring them elsewhere' % differ)
    return 1 if differ else 0


if __name__ == '__main__':
    sys.exit(main())
