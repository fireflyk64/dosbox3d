#!/usr/bin/env python3
"""The old game's nav points switched on and off (traced 2026-11-07; the user: in the Franklin escort "the agis spawned
on my way TO the transport instead of on my way home"): tests/wc1/navswitch.py DUMPDIR [--write]

Its getNearbyNavPoint (seg001:1FF3) takes nav k as near only when byte +30 of its record is 1 and the player is within
its radius (+43): a point with 0 there is switched off and never comes (2: never by distance, the second waves, which
ours hold for the enemies out).  Entering a point applies up to four (state, nav) pairs at +45..+52 (255: none): state
1 switches that nav on, 0 off.  McAuliffe 3's Nav 1 has (1, 2): its ".Encounter" (state 0 at the start) is switched on
by reaching the Drayman, so its Salthi are the way home's; ours came on the way out.

For every mission of data/missions.txt (its dump DUMPDIR/sS-mM/t1.bin, tests/wc1/dump-all.sh) this matches our nav
lines to the old records by place (the full map's: fullscale.py) and prints what each should carry: `off` (starts
switched off) and `switch=+N,-M` (entering it switches our nav N on, M off).  --write puts them on the nav lines.
A switch of nav 0 (s11-m2's Nav 1 moves the Tiger's Claw) is left out: ours keeps its carrier where it is; so are a
second wave's (state 2: ours brings it by its own wave rule, OLDZIEY1's .Ambush, whose switch would take nav 1 off before
it might be flown).  A point the old game starts off whose name has no dot (RIGEL1's Nav 1b) stays hidden, as the port
made it: it comes at its time, not on the lists.
"""
import os
import re
import struct
import sys

NODES = ['ENIGMA', 'CEPHID', 'SHELTON', 'CRUCIBL', 'OLDZIEY', 'DEFIANC', 'LIXIS', 'ATARAXI', 'RIGEL', 'BARNARD', 'WOLF359',
         'SHMRN', 'LUYTEN']             # (fullscale.py's: series 1 .. 13; it runs on import, so not imported)


def s24(b, o):
    v = b[o] | b[o + 1] << 8 | b[o + 2] << 16
    return v - (1 << 24) if v & 0x800000 else v


def old_navs(path):
    d = open(path, 'rb').read()
    out = {}
    for n in range(16):
        r = 0x687C + 0x4D * n
        rec = d[r:r + 0x4D]
        name = rec[:16].split(b'\0')[0].decode('latin-1')
        if name:
            pairs = [(rec[45 + 2 * k], rec[46 + 2 * k]) for k in range(4)]
            out[n] = dict(name=name, at=(s24(d, r + 32), s24(d, r + 36), s24(d, r + 40)), state=rec[30],
                          pairs=[(s, k) for s, k in pairs if s in (0, 1) and k < 16])
    return out


def main():
    dumps = sys.argv[1]
    write = '--write' in sys.argv
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'data', 'missions.txt')
    lines = open(path).read().split('\n')
    # each mission's nav lines: (line index, our id, place, hidden)
    blocks, mid = {}, None
    for i, l in enumerate(lines):
        m = re.match(r'mission\s+(\S+)', l)
        if m:
            mid = m[1]
            blocks[mid] = []
        w = l.split()
        if mid and w and w[0] == 'nav':
            blocks[mid].append((i, int(w[1]), tuple(int(v) for v in w[2:5]), 'hidden' in w))
    changed = 0
    for mid, navs in blocks.items():
        node = next(n for n in NODES if mid.startswith(n[:7]))
        dump = os.path.join(dumps, 's%d-m%d' % (NODES.index(node) + 1, int(mid[len(node[:7]):]) - 1), 't1.bin')
        old = old_navs(dump)
        ours = {0: 0}                   # old index -> our id
        for _i, k, at, hidden in navs:
            cands = [n for n, v in old.items() if n and v['at'] == at]
            if len(cands) > 1:          # (a second wave at the same place: by the hidden flag, the name's dot)
                cands = [n for n in cands if (old[n]['name'].startswith('.') or old[n]['state'] != 1) == hidden] or cands
            if cands:
                ours[cands[0]] = k
        notes = []
        for i, k, at, hidden in navs:
            o = next((n for n, v in ours.items() if v == k and n), None)
            if o is None:
                notes.append('nav %d: no old record at its place' % k)
                continue
            v = old[o]
            sw = []
            if v['state'] == 2 and v['pairs']:   # (a second wave: never entered by distance in the old game; ours comes as its
                notes.append('nav %d (%s): a second wave\'s switches %s left out' % (k, v['name'], v['pairs']))
                v = dict(v, pairs=[])           # wave logic says, which could switch a waypoint off before it is flown)
            for s, n in v['pairs']:
                if n == 0:
                    notes.append('nav %d (%s): switches nav 0 %s, left out' % (k, v['name'], 'on' if s else 'off'))
                elif n not in ours:
                    notes.append('nav %d (%s): switches old nav %d (%s), not ours' % (k, v['name'], n,
                                                                                    old.get(n, {}).get('name', '?')))
                else:
                    sw.append(('+' if s else '-') + str(ours[n]))
            words = [w for w in lines[i].split() if w != 'off' and not w.startswith('switch=')]
            if v['state'] == 0:
                words.append('off')
            if sw:
                words.append('switch=' + ','.join(sw))
            new = 'nav      ' + ' '.join(words[1:])
            if new != lines[i]:
                changed += 1
                print('%-9s %-40s -> %s   (old %d %r state %d pairs %s)' % (mid, lines[i], new, o, v['name'], v['state'],
                                                                         v['pairs']))
                lines[i] = new
        for n in notes:
            print('%-9s %s' % (mid, n))
    print('navswitch: %d nav lines %s' % (changed, 'written' if write else 'to change (--write)'))
    if write:
        open(path, 'w').write('\n'.join(lines))


if __name__ == '__main__':
    main()
