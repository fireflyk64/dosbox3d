#!/usr/bin/env python3
# Two TIE Fighter machines' reports joined by the wall clock (scripts/tfnet-duo.sh writes them, WCNET_TIE_REPORT_MS sets
# their period): for each line of A in which object OBJ is a puppet (flown by B), where B's own game had it at the same
# wall-clock moment (interpolated between B's lines), and how far apart the two are, in place and attitude.
#
#   scripts/tfnet-join.py A.wall B.wall OBJ
#   scripts/tfnet-join.py A.wall B.wall all     every craft B flies that A shows, by its distance from A's own craft
#                                               (near within 2.5 km, mid within 10 km, far beyond: the rates' tiers)
import re, sys, math
def parse(fn):
    out = []
    for line in open(fn, errors="replace"):
        m = re.search(r"wall (\d+) frame (\d+)", line)
        if not m: continue
        rec = {"wall": int(m.group(1)), "frame": int(m.group(2)), "own": None, "puppets": {}, "mine": {}}
        mo = re.search(r"own obj (\d+): (-?\d+) (-?\d+) (-?\d+) hpr (\d+) (\d+) (\d+) speed (-?\d+)", line)
        if mo: rec["own"] = (int(mo.group(1)), [int(v) for v in mo.groups()[1:]])
        for mm in re.finditer(r"mine obj (\d+): (-?\d+) (-?\d+) (-?\d+) hpr (\d+) (\d+) (\d+) speed (-?\d+) type (\d+)", line):
            g = [int(v) for v in mm.groups()]
            rec["mine"][g[0]] = g[1:8]
        for mp in re.finditer(r"obj (\d+) of pilot (\d+): here (-?\d+) (-?\d+) (-?\d+) hpr (\d+) (\d+) (\d+) speed (-?\d+) type (\d+), reported (-?\d+) (-?\d+) (-?\d+) (\d+) ms ago", line):
            g = [int(v) for v in mp.groups()]
            rec["puppets"][g[0]] = {"here": g[2:5], "hpr": g[5:8], "speed": g[8], "type": g[9], "rep": g[10:13], "age": g[13]}
        out.append(rec)
    return out
a, b = parse(sys.argv[1]), parse(sys.argv[2])
def series(obj):
    out = []
    for r in b:
        if r["own"] and r["own"][0] == obj: out.append((r["wall"], r["own"][1]))
        elif obj in r["mine"]: out.append((r["wall"], r["mine"][obj]))
    return out
def at(ser, w):
    for i in range(len(ser) - 1):
        (w0, s0), (w1, s1) = ser[i], ser[i + 1]
        if w0 <= w <= w1 and w1 > w0 and w1 - w0 < 2000:
            f = (w - w0) / (w1 - w0)
            ang = [(s0[k] + f * (((s1[k] - s0[k] + 32768) % 65536) - 32768)) % 65536 for k in range(3, 6)]
            return [s0[k] + f * (s1[k] - s0[k]) for k in range(3)], ang
    return None, None
def angle_gap(p, hpr):
    return max(abs(((p["hpr"][k] - hpr[k] + 32768) % 65536) - 32768) for k in range(3))
if sys.argv[3] != "all":
    obj = int(sys.argv[3]); ser = series(obj)
    gaps, hgaps = [], []
    print(f"{'wall':>9} {'here (A)':>26} {'truth (B)':>26} {'gap':>6} {'speed':>5} hdg A/B  age")
    for r in a:
        p = r["puppets"].get(obj)
        if not p or p["type"] == 0: continue
        t, hpr = at(ser, r["wall"])
        if t is None: continue
        gap = math.dist(p["here"], t); gaps.append(gap); hgaps.append(angle_gap(p, hpr))
        print(f"{r['wall']:9d} {str(p['here']):>26} {str([round(v) for v in t]):>26} {gap:6.0f} {p['speed']:5d} {p['hpr'][0]:5d}/{round(hpr[0]):5d} {p['age']:3d}")
    if gaps:
        gs, hs = sorted(gaps), sorted(hgaps)
        print(f"{len(gs)} samples: gap median {gs[len(gs)//2]:.0f}, worst {gs[-1]:.0f} units (1 km = 40,960 units; a T/F is ~1,300 across); "
              f"attitude median {hs[len(hs)//2]:.0f}, worst {hs[-1]:.0f} (65,536 a turn: {hs[len(hs)//2] * 360 / 65536:.2f} and {hs[-1] * 360 / 65536:.2f} degrees)")
    sys.exit(0)
tiers = {"near": [], "mid": [], "far": []}
objs = set()
for r in a:
    if not r["own"]: continue
    me = r["own"][1][:3]
    for obj, p in r["puppets"].items():
        if p["type"] == 0: continue
        t, hpr = at(series(obj), r["wall"])
        if t is None: continue
        objs.add(obj)
        d = math.dist(me, t)
        tier = "near" if d < 102400 else "mid" if d < 409600 else "far"
        tiers[tier].append((math.dist(p["here"], t), angle_gap(p, hpr), p["age"]))
print(f"{len(objs)} craft flown by B and shown by A")
for tier, rows in tiers.items():
    if not rows: print(f"  {tier:4s}: none"); continue
    g = sorted(r[0] for r in rows); h = sorted(r[1] for r in rows); ages = sorted(r[2] for r in rows)
    print(f"  {tier:4s}: {len(rows):5d} samples, place median {g[len(g)//2]:7.0f} worst {g[-1]:7.0f} units, "
          f"attitude median {h[len(h)//2]:5.0f} worst {h[-1]:5.0f}, report age median {ages[len(ages)//2]} ms worst {ages[-1]} ms")
