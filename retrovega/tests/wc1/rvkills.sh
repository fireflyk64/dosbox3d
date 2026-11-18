#!/bin/bash
# Our side of the kill rigs (README.md "The guns against the old game's"): rvsim flies a scene over seeds, the player
# invulnerable, and kills.py reads each flight; the kills pooled.
#   tests/wc1/rvkills.sh MODE SHIP SCENE SEEDS [rvsim args]
# MODE: tail:KM (KM behind the first enemy, firing; their guns taken), shoot:Q or shoot:Q:back (hits of Q, the damage
# model alone), hunt (the nose onto the first enemy every 3 s, firing, the enemies armed: 240 s).  SHIP: 0 the
# Schroedinger, 1 the Franklin...  e.g. tests/wc1/rvkills.sh tail:1000 1 tests/wc1/three-aristons.scene 6
# Extra args go to rvsim: --mounts 38:3,-38:3 (the guns moved), --data DIR (another RVGAME.DAT).  Builds nothing: run
# SDL=0 host/build.sh first.  The traces stay in build/kills/.
set -u
cd "$(dirname "$0")/../.."
mode=$1; ship=$2; scene=$3; seeds=$4; shift 4
case $mode in
    tail:*) args="--disarm --tail ${mode#tail:}"; frames=4500 ;;
    shoot:*) args="--shoot ${mode#shoot:}"; frames=4500 ;;
    hunt) args="--hunt 3 --hunt-first"; frames=7200 ;;
    *) echo "rvkills.sh: MODE is tail:KM, shoot:Q[:back] or hunt"; exit 2 ;;
esac
mkdir -p build/kills
name=$(basename "$scene" .scene)-$ship-${mode//:/_}
for seed in $(seq 1 "$seeds"); do
    build/host32-nosdl/rvsim --headless --frames $frames --ship "$ship" --nodamage $args --scene "$scene" --seed "$seed" \
        --shiptrace "build/kills/$name-$seed.txt" --log "build/kills/$name-$seed.log" "$@" > /dev/null 2>&1
    python3 -I tests/wc1/kills.py "build/kills/$name-$seed.txt" 30 --quiet
done | python3 -I -c '
import re, sys
hits, outs, n = {}, [], 0
for l in sys.stdin:
    m = re.match(r"type (\d+): \d+ kills: hits to kill [\d.]+ \(([\d ]+)\), damage a hit ([\d.]+)", l)
    if m:
        h = hits.setdefault(m.group(1), [[], []])
        h[0] += [int(x) for x in m.group(2).split()]
        h[1].append(float(m.group(3)))
    m = re.match(r"out: \d+ at ([\d. ]*) s", l)
    if m:
        n += 1
        outs.append(sorted(float(x) for x in m.group(1).split()))
for ty, (hs, dm) in sorted(hits.items()):
    hs.sort()
    print("type %s: %d kills, hits to kill %.1f (median %d, %d-%d), %.1f a hit" % (ty, len(hs), sum(hs) / len(hs),
          hs[len(hs) // 2], hs[0], hs[-1], sum(dm) / len(dm)))
k = [sorted(f[i] for f in outs if len(f) > i) for i in range(3)]
print("%d flights; out of the fight, the medians: first %s, second %s, third %s (of %d/%d/%d flights)" % (n,
      *["%.0f s" % x[len(x) // 2] if x else "-" for x in k], len(k[0]), len(k[1]), len(k[2])))
'
