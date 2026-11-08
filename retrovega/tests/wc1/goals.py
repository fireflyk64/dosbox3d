#!/usr/bin/env python3
"""The old game's mission objectives and their points, against data/missions.txt (the user, 2026-11-08: a lost Kurasawa
2 should send the player to Rostov, "we want people to experience more unless they are experts at Rigel 2").
    tests/wc1/goals.py DUMPDIR GAMEDAT/CAMP.000 [--write]
The objectives: the mission's table at dseg:D25B (a 0x19-byte record each, how many at dseg:BD18): +0 the type, +2 the
subject, +3 the flags (0x4 reached, 0x1 met, 0x2 scored); a mission's points by objective are CAMP.000's (camptree.py).
ovr139's check every frame scores an objective (0x2) when: 0 (a nav point: subject its record) it has been reached and
its wave is gone; 1 (the carrier) the player lands; 2 (a mission ship) an escort out has jumped, one in or back is home;
3 (a mission ship) it reached its point; 4 (a mission ship) it is destroyed.  Ours (missions.txt `goal` lines):
    goal nav N PTS           nav point N reached and every enemy it brings killed or gone
    goal land PTS            landed on the home carrier
    goal escort TYPE navN PTS   that mission's escort target of the type there through (out: jumped; in/back: home)
    goal meet TYPE navN PTS     it came into play
    goal kill TYPE navN PTS     the first ship of the type there killed
Prints each mission's goal lines (objectives worth nothing are left out); --write puts them into missions.txt after
each mission's last ship line, replacing any it had."""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import briefing                         # noqa: E402
from waves import NODES, TYPES, read_dump, read_ours, near   # noqa: E402

MISSIONS = os.path.join(HERE, '..', '..', 'data', 'missions.txt')
KINDS = {2: 'escort', 3: 'meet', 4: 'kill'}


def our_navs(mine, nv):
    """our nav points at an old record's place, the hidden ones for a hidden record (waves.py's rule); its launch: 0"""
    if nv['n'] == 0:
        return [0]
    return [n for n, (at, hid) in sorted(mine['navs'].items()) if n and near(at, nv['at']) and hid == nv['hidden']]


def objectives(path):
    d = open(path, 'rb').read()
    out = []
    for k in range(d[0xBD18]):
        o = 0xD25B + 0x19 * k
        out.append((struct.unpack_from('<H', d, o)[0], d[o + 2]))
    return out


def main():
    dumpdir, camp = sys.argv[1], sys.argv[2]
    table = dict(briefing.entries(camp))[1]
    ours = read_ours(MISSIONS)
    result, problems = {}, []
    for si, node in enumerate(NODES):
        for mi in range(4):
            mid = '%s%d' % (node[:7], mi + 1)
            path = os.path.join(dumpdir, 's%d-m%d' % (si + 1, mi), 't1.bin')
            if mid not in ours or not os.path.exists(path) or node in ('SHMRN', 'LUYTEN'):
                continue                        # (the last two series end the campaign whatever they score)
            rec = table[90 * si:90 * si + 90]
            pts = rec[10 + 20 * mi + 4:10 + 20 * mi + 20]
            navs, ships = read_dump(path)
            bynum = {nv['n']: nv for nv in navs}
            mine = ours[mid]
            lines = []
            for k, (typ, subj) in enumerate(objectives(path)):
                p = pts[k] if k < len(pts) else 0
                if not p:
                    continue
                if typ == 1:
                    lines.append('goal     land %d' % p)
                elif typ == 0:
                    nv = bynum.get(subj)
                    ids = our_navs(mine, nv) if nv else []
                    if not ids:
                        problems.append('%s: objective %d, nav record %d (%r): no nav of ours there' % (
                            mid, k, subj, nv and nv['name']))
                        continue
                    lines.append('goal     nav %d %d' % (ids[0], p))
                elif typ in KINDS:
                    s = ships.get(subj)
                    kind = TYPES.get(s['t']) if s else None
                    where = [nv for nv in navs if subj in nv['ships']]
                    ids = []
                    for nv in where:
                        ids += our_navs(mine, nv)
                    if not where:
                        ids = [0]                   # (in no list: at the launch, as our lines have it)
                    if not kind or not ids:
                        problems.append('%s: objective %d (%s, ship %d, type %s): %s' % (
                            mid, k, KINDS[typ], subj, s and s['t'], 'no type' if not kind else 'in no nav of ours'))
                        continue
                    ids = [n for n in ids if any(sh['kind'] == kind and n in sh['navs'] for sh in mine['ships'])] or ids
                    have = [sh for sh in mine['ships'] if sh['kind'] == kind and ids[0] in sh['navs']]
                    if not have:
                        problems.append('%s: objective %d (%s %s at nav %d): no such ship line' % (mid, k, KINDS[typ],
                                                                                              kind, ids[0]))
                        continue
                    lines.append('goal     %s %s nav%d %d' % (KINDS[typ], kind, ids[0], p))
                else:
                    problems.append('%s: objective %d of type %d' % (mid, k, typ))
            result[mid] = lines
            total = sum(pts)
            got = sum(int(l.split()[-1]) for l in lines)
            print('%s (%d of the series\' points; %d placed)' % (mid, total, got))
            for l in lines:
                print('  ' + l)
    for p in problems:
        print('PROBLEM ' + p)
    if '--write' in sys.argv:
        write(result)
    return 1 if problems else 0


def write(result):
    out, cur, last_ship = [], None, {}
    src = open(MISSIONS).read().split('\n')
    for i, line in enumerate(src):
        w = line.split()
        if w and w[0] == 'mission':
            cur = w[1]
        if w and w[0] == 'ship' and cur:
            last_ship[cur] = i
    for i, line in enumerate(src):
        w = line.split()
        if w and w[0] == 'goal':
            continue
        out.append(line)
        for mid, at in last_ship.items():
            if at == i and mid in result:
                out.extend(result[mid])
    open(MISSIONS, 'w').write('\n'.join(out))


if __name__ == '__main__':
    sys.exit(main())
