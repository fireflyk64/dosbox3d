#!/usr/bin/env python3
"""The old game's shots by their geometry (the user's idea, 2026-10-31: log the angles at every fire): for each F (fire)
event of a trace (run-old.sh), the shooter's nose angle to the player, the player's nose angle to the shooter and the
distance, histogrammed per NPC slot within 6,000 km of the player, with the head-on volleys (both within 30 degrees)
against the frames spent head on, and the volleys split by whether the slot was the player's selected target.
    fireangles.py TRACE [FRAME:SLOT,FRAME:SLOT,...]   the selected target from each frame on (dseg:662C, read from
                                                     the key script's @tag dumps with dsdump.py)
"""
import sys, math, collections
trace = sys.argv[1]
target = {}                             # frame -> the player's selected target, from the dumps (optional)
if len(sys.argv) > 2:
    for item in sys.argv[2].split(','):
        fr, sl = item.split(':')
        target[int(fr)] = int(sl)
rows = collections.defaultdict(dict)
fires = []
for l in open(trace):
    f = l.split()
    if not f:
        continue
    if f[0] == "S" and len(f) >= 13:
        fr, slot = int(f[1]), int(f[2])
        x, y, z = (int(v) / 256 for v in f[7:10])
        fx, fy, fz = (int(v) / 256 for v in f[10:13])
        rows[fr][slot] = (x, y, z, fx, fy, fz, int(f[3]))
    elif f[0] == 'F':
        fires.append((int(f[1]), int(f[2]), int(f[3])))
def ang(a, b):
    """angle (degrees) between a's front and the direction to b"""
    dx, dy, dz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    d = math.sqrt(dx * dx + dy * dy + dz * dz) or 1
    fl = math.sqrt(a[3] ** 2 + a[4] ** 2 + a[5] ** 2) or 1
    c = (dx * a[3] + dy * a[4] + dz * a[5]) / (d * fl)
    return math.degrees(math.acos(max(-1, min(1, c)))), d
def sel(fr):
    best = None
    for k in sorted(target):
        if k <= fr:
            best = target[k]
    return best
bins = [10, 20, 30, 37, 46, 60, 90, 181]
per = collections.defaultdict(lambda: {'n': 0, 'nose': collections.Counter(), 'pnose': collections.Counter(), 'headon': 0, 'sel': 0, 'sel_in37': 0, 'free_in37': 0, 'free': 0, 'd': []})
# the geometry per frame: how many frames each NPC spent with its nose within 37 of the player (and selected or not)
frames = collections.defaultdict(lambda: {'in37': 0, 'out': 0, 'sel_in37': 0, 'sel_out': 0, 'headon': 0})
for fr in rows:
    r = rows[fr]
    if 0 not in r:
        continue
    for slot, s in r.items():
        if slot == 0 or s[6] != 12:
            continue
        a, d = ang(s, r[0])
        pa, _ = ang(r[0], s)
        if d >= 6000:
            continue
        g = frames[slot]
        is_sel = sel(fr) == slot
        if a < 37:
            g['in37'] += 1
            if is_sel: g['sel_in37'] += 1
        else:
            g['out'] += 1
            if is_sel: g['sel_out'] += 1
        if a < 30 and pa < 30 and d < 6000:
            g['headon'] += 1
for fr, slot, gun in fires:
    r = rows.get(fr)
    if not r or 0 not in r or slot not in r or slot == 0 or gun != 0:
        continue                        # (one event a volley: gun 0)
    s = r[slot]
    a, d = ang(s, r[0])
    pa, _ = ang(r[0], s)
    if d >= 6000:
        per[slot]['far'] = per[slot].get('far', 0) + 1
        continue
    p = per[slot]
    p['n'] += 1
    p['d'].append(d)
    p['nose'][next(b for b in bins if a < b)] += 1
    p['pnose'][next(b for b in bins if pa < b)] += 1
    if a < 30 and pa < 30 and d < 6000:
        p['headon'] += 1
    is_sel = sel(fr) == slot
    if is_sel:
        p['sel'] += 1
        if a < 37: p['sel_in37'] += 1
    else:
        p['free'] += 1
        if a < 37: p['free_in37'] += 1
print('%s: %d frames, %d fire events' % (trace.split('/')[-2], len(rows), len(fires)))
for slot in sorted(per):
    p = per[slot]; g = frames[slot]
    print('slot %d: %d volleys within 6,000 km of the player (%d farther off, left out), median range %.0f km; head-on (both within 30): %d volleys in %d such frames'
          % (slot, p['n'], p.get('far', 0), sorted(p['d'])[len(p['d']) // 2] if p['d'] else 0, p['headon'], g['headon']))
    print('   shooter nose to player at fire: ' + '  '.join('<%d:%d' % (b, p['nose'][b]) for b in bins))
    print('   player nose to shooter at fire: ' + '  '.join('<%d:%d' % (b, p['pnose'][b]) for b in bins))
    print('   selected: %d volleys (%d with its nose within 37 of the player, over %d such frames; %d outside over %d frames); not selected: %d volleys (%d within 37, over %d frames)'
          % (p['sel'], p['sel_in37'], g['sel_in37'], p['sel'] - p['sel_in37'], g['sel_out'], p['free'], p['free_in37'], g['in37'] - g['sel_in37']))
