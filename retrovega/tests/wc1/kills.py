#!/usr/bin/env python3
"""A kill rig's flight (the old game's under run-old.sh, ours under rvsim; README.md "The guns against the old game's")
read for its kills.

    kills.py TRACE FPS [--quiet]

FPS: what a trace frame is in seconds of the logic, 15 for the old game's (its frames at the 15 a second our logic
copies, whatever the emulator ran it at), 30 for ours.  For each enemy ship's life in its slot (from its first frame
to the frame it shows dying, aistate 9, or changes type; a ship that is off the trace for a while, off the scene, lives
on), the player's hits on it (D lines, the killing one included: their sizes say how far the bolts flew), the time
from its first hit to its death, and the player's shots (F lines of slot 0, a gun each) fired while it was the first
living foe (the rigs' target).  Then a line a type (hits to kill), "all:" (the kills' times from the player's first
shot, the shots and the hits on enemies) and "out:" (out of the fight: killed, or last seen 30 s or more before the
trace's end, as the old game's crippled fighters flee and are taken off 16,000 km out).
"""
import sys
from collections import defaultdict

path, fps = sys.argv[1], float(sys.argv[2])
quiet = '--quiet' in sys.argv
S = defaultdict(dict)                   # frame -> slot -> (type, side, aistate)
D, F = [], []
last = -1
for line in open(path):
    p = line.split()
    if not p:
        continue
    if p[0] == 'S' and len(p) >= 32:
        fr = int(p[1])
        if fr < last - 5:
            break                       # (a second flight: the first only)
        last = fr
        S[fr][int(p[2])] = (int(p[4]), int(p[5]), int(p[21]))
    elif p[0] == 'D':
        D.append((int(p[1]), int(p[2]), int(p[3]), int(p[4])))
    elif p[0] == 'F':
        F.append((int(p[1]), int(p[2]), int(p[3])))
frames = sorted(S)
ours = next(S[f][0][1] for f in frames if 0 in S[f])
lives, cur, dead = [], {}, set()
target = {}
for fr in frames:
    seen = S[fr]
    for s in list(dead):
        if s not in seen:
            dead.discard(s)             # (a dead ship's slot is free again once it is gone)
    for s, (ty, side, ai) in sorted(seen.items()):
        if s == 0 or side == ours or s in dead:
            continue
        if s in cur and cur[s]['type'] != ty:
            cur[s].update(end=fr, how='type'); lives.append(cur.pop(s)); dead.add(s); continue
        if s not in cur:
            cur[s] = {'slot': s, 'type': ty, 'start': fr, 'end': None, 'how': None}
        if ai == 9:
            cur[s].update(end=fr, how='dying'); lives.append(cur.pop(s)); dead.add(s)
    alive = sorted(cur)
    target[fr] = (alive[0], cur[alive[0]]['start']) if alive else None
for s in cur:
    cur[s].update(end=frames[-1], how='alive'); lives.append(cur[s])
tot = defaultdict(list)
for L in sorted(lives, key=lambda l: l['start']):
    hits = [d for d in D if d[2] == L['slot'] and L['start'] <= d[0] <= L['end']]
    shots = [f for f in F if f[1] == 0 and target.get(f[0]) == (L['slot'], L['start']) and f[0] <= L['end']]
    if not hits and not shots:
        continue
    sizes = [h[3] for h in hits]
    t = (L['end'] - hits[0][0]) / fps if hits else 0
    if not quiet:
        print('  slot %d type %d: %s at %.1f s: %d hits (%s) = %d, %.1f s from the first hit, %d shots (%.0f%% hit)' % (
            L['slot'], L['type'], L['how'], L['end'] / fps, len(hits), ' '.join(map(str, sizes)), sum(sizes), t,
            len(shots), 100.0 * len(hits) / max(1, len(shots))))
    if L['how'] != 'alive':
        tot[L['type']].append((len(hits), t, len(shots), sum(sizes) / max(1, len(sizes))))
for ty, ks in sorted(tot.items()):
    n = len(ks)
    print('type %d: %d kills: hits to kill %.1f (%s), damage a hit %.1f, %.1f s from the first hit, %.0f shots, %.0f%% hit' % (
        ty, n, sum(k[0] for k in ks) / n, ' '.join(str(k[0]) for k in ks), sum(k[3] for k in ks) / n,
        sum(k[1] for k in ks) / n, sum(k[2] for k in ks) / n, 100.0 * sum(k[0] for k in ks) / max(1, sum(k[2] for k in ks))))
t0 = min([f[0] for f in F if f[1] == 0] or [0])     # (from the player's first shot: the old game's frames count from the launch)
deaths = sorted((L['end'] - t0) / fps for L in lives if L['how'] != 'alive')
pshots = len([f for f in F if f[1] == 0])
enemy_slots = {L['slot'] for L in lives}
phits = len([d for d in D if d[2] in enemy_slots and d[2] != 0])
print('all: %d kills at %s s; the player\'s shots %d, hits on enemies %d (%.1f%%)' % (
    len(deaths), ' '.join('%.0f' % t for t in deaths), pshots, phits, 100.0 * phits / max(1, pshots)))
lastseen = {}
for fr in frames:
    for s, (ty, side, ai) in S[fr].items():
        if s != 0 and side != ours and ty < 24:     # (ships, not the old game's weapons: types 0x18 on)
            lastseen[s] = fr
outs = []
for L in lives:
    if L['type'] >= 24:
        continue
    if L['how'] != 'alive':
        outs.append((L['end'] - t0) / fps)
    elif lastseen.get(L['slot'], L['end']) < frames[-1] - 30 * fps:
        outs.append((lastseen[L['slot']] - t0) / fps)
print('out: %d at %s s' % (len(outs), ' '.join('%.0f' % t for t in sorted(outs))))
