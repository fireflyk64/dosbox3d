#!/usr/bin/env python3
"""tdmnet-syncmp.py HOST_LOG GUEST_LOG
The TIE Fighter demo's two machines compared (scripts/tdmnet-sync.sh): each log's !tdmcraft tables, the host's paired
with the guest's nearest by the wall clock, craft matched by identity (flight group . arrival).  A line a pair: what
differs; then the differences that lasted 4 s or more."""
import re, sys, math
rx_line = re.compile(rb'key script ([0-9.]+)s: TIE demo craft \(primary (\d+)(?:, wall ([0-9.]+))?\):(.*)')
rx_c = re.compile(r'\[(\d+) id(\d+)\.(\d+) fg(\d+) t(\d+) ([-0-9.]+)km \(([-0-9.]+) ([-0-9.]+) ([-0-9.]+)\) v(-?\d+) hull (\d+)/(\d+) sh (-?\d+) wh (\d+)( dying)?([^\]]*?) g(\d+)\]')
def load(path, shift):
    out = {}
    for line in open(path, 'rb'):
        m = rx_line.search(line)
        if not m: continue
        t = round(float(m.group(1)) + shift, 1)
        wall = float(m.group(3)) if m.group(3) else None
        crafts = {}
        for c in rx_c.finditer(m.group(4).decode('latin1')):
            slot, fg_id, arr, fg, typ = int(c.group(1)), int(c.group(2)), int(c.group(3)), int(c.group(4)), int(c.group(5))
            key = (fg_id, arr) if (fg_id or arr or slot == 0 or fg == 0) else ('slot', slot)
            crafts.setdefault(key, []).append(dict(slot=slot, fg=fg, type=typ, pos=(float(c.group(7)), float(c.group(8)), float(c.group(9))),
                               dkm=float(c.group(6)), v=int(c.group(10)), hull=int(c.group(11)), hmax=int(c.group(12)), sh=int(c.group(13)),
                               wh=int(c.group(14)), dying=bool(c.group(15)), extra=c.group(16).strip(), g=int(c.group(17))))
        out[t] = (int(m.group(2)), crafts, wall)
    return out
H, G = load(sys.argv[1], 0.0), load(sys.argv[2], 0.0)
# pairs: by the wall clock when both have it (each host table with the guest's nearest), else by the script's time
pairs = []
if all(v[2] is not None for v in H.values()) and all(v[2] is not None for v in G.values()) and H and G:
    gl = sorted(G.items(), key=lambda kv: kv[1][2])
    for t, hv in sorted(H.items()):
        best = min(gl, key=lambda kv: abs(kv[1][2] - hv[2]))
        if abs(best[1][2] - hv[2]) < 1.0: pairs.append((t, hv, best[1], best[1][2] - hv[2]))
else:
    pairs = [(t, H[t], G[t], 0.0) for t in sorted(set(H) & set(G))]
issues = 0
worst_near = worst_far = 0.0
last = {}       # a figure's difference seen at the pair before: (what) -> since
persist = {}
for t, hv, gv, skew in pairs:
    ph, ch = hv[0], hv[1]; pg, cg = gv[0], gv[1]
    notes = []; figs = {}
    if ph != pg: notes.append('primary host %d guest %d' % (ph, pg))
    for k in sorted(set(ch) | set(cg), key=str):
        a, b = ch.get(k), cg.get(k)
        name = 'id%s.%s' % k
        if a and len(a) > 1 or b and len(b) > 1:
            notes.append('%s twice on %s' % (name, 'host' if a and len(a) > 1 else 'guest'))
        a = a[0] if a else None; b = b[0] if b else None
        if not a or not b:
            one = a or b
            if one['dying'] or one['type'] >= 0x40: continue
            notes.append('%s (fg%d t%d, %.1f km off, hull %d/%d) only on %s' % (name, one['fg'], one['type'], one['dkm'], one['hull'], one['hmax'], 'host' if a else 'guest'))
            figs[(name, 'only')] = 'only on %s' % ('host' if a else 'guest')
            continue
        if a['type'] != b['type'] and not (a['dying'] or b['dying'] or a['type'] >= 0x40 or b['type'] >= 0x40):
            notes.append('%s type host %d guest %d' % (name, a['type'], b['type']))
        if a['dying'] != b['dying'] or (a['type'] >= 0x40) != (b['type'] >= 0x40):
            notes.append('%s dying on %s only' % (name, 'host' if (a['dying'] or a['type'] >= 0x40) else 'guest'))
            figs[(name, 'dying')] = 'dying on %s only' % ('host' if (a['dying'] or a['type'] >= 0x40) else 'guest')
            continue
        if a['dying'] or a['type'] >= 0x40: continue
        d = math.dist(a['pos'], b['pos'])
        near = min(a['dkm'], b['dkm']) < 5.0
        if near: worst_near = max(worst_near, d)
        else: worst_far = max(worst_far, d)
        if d > (0.35 if near else 2.5):
            notes.append('%s (t%d) %.2f km apart (%.1f / %.1f km off the pilots)' % (name, a['type'], d, a['dkm'], b['dkm']))
            figs[(name, 'place')] = '%.2f km apart' % d
        if a['hull'] != b['hull']:
            notes.append('%s (t%d) hull host %d guest %d of %d' % (name, a['type'], a['hull'], b['hull'], a['hmax']))
            figs[(name, 'hull')] = 'hull host %d guest %d of %d' % (a['hull'], b['hull'], a['hmax'])
        if abs(a['sh'] - b['sh']) > 1500:
            notes.append('%s (t%d) shields host %d guest %d' % (name, a['type'], a['sh'], b['sh']))
            figs[(name, 'shields')] = 'shields host %d guest %d' % (a['sh'], b['sh'])
        if a['wh'] != b['wh']:
            notes.append('%s (t%d) warheads host %d guest %d' % (name, a['type'], a['wh'], b['wh']))
            figs[(name, 'warheads')] = 'warheads host %d guest %d' % (a['wh'], b['wh'])
        if a['g'] != b['g']: notes.append('%s goal state host %d guest %d' % (name, a['g'], b['g']))
        if a['extra'] != b['extra']: notes.append('%s boarding host "%s" guest "%s"' % (name, a['extra'], b['extra']))
    print('%6.1fs (%+.2f s): %2d / %2d craft%s' % (t, skew, sum(len(v) for v in ch.values()), sum(len(v) for v in cg.values()), ': ' + '; '.join(notes) if notes else ' the same'))
    issues += len(notes)
    for k in list(last):
        if k not in figs:
            since, what = last.pop(k)
            if t - since >= 3.9: persist[(k, since)] = (t - since, what)
    for k, what in figs.items():
        last[k] = (last[k][0] if k in last else t, what)
for k, (since, what) in last.items():
    if pairs and pairs[-1][0] - since >= 3.9: persist[(k, since)] = (pairs[-1][0] - since, what + ' (to the end)')
print('PERSISTING (4 s or more):')
for (k, since), (dur, what) in sorted(persist.items(), key=lambda kv: kv[0][1]):
    print('  %6.1fs for %5.1f s: %s %s' % (since, dur, k[0], what))
print('pairs %d, notes %d, persisting %d, worst place difference near %.2f km, far %.2f km' % (len(pairs), issues, len(persist), worst_near, worst_far))
