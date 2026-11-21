#!/bin/bash
# Privateer's bolt soak (the user, 2026-11-21: "Could our gc idea be leaving trash. Should we run it 20x as long and see"):
# bolts.sh's room (4 Centurions, guns 6 times as fast, the copies' caps lifted), the guns held in bursts -- 40 s on, 20 s
# off -- for DUR seconds (default 1300: 20 bursts), !privbrains at each rest's end: the log's "kept pointers N of 200" and
# the per-second "near heap free" there must come back to the same numbers every rest (no trash), and no machine may
# report an error.   scripts/privtests/boltsoak.sh [seconds]   PLAYERS=4 GUNS=6 GC= OUT=/tmp/privsoak DOSBOX=
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-1300}; PLAYERS=${PLAYERS:-4}; OUT=${OUT:-/tmp/privsoak}
export SAVE=$ROOT/privsaves/OXFORD01.SAV NOROUTE=1
rm -rf "$OUT"; mkdir -p "$OUT"
E="WCPRIVCOPYSTRESS=1 WCPRIVGUNSTRESS=${GUNS:-6}${GC:+ WCPRIVBOLTGC=$GC}"
export HOSTENV="$E" GUESTENV="$E"
X="4:esc,7:!mouse=0.38:0.94,7.5:!button=0:1,7.8:!button=0:0,10:!mouse=0.31:0.405,10.5:!button=0:1,10.8:!button=0:0,12:!mouse=0.31:0.345,12.5:!button=0:1,12.8:!button=0:0,15:!mouse=0.85:0.345,15.5:!button=0:1,15.8:!button=0:0"
X="$X,50:!mouse=0.60:0.72,50.5:!button=0:1,50.8:!button=0:0,58:!privbrains"
for t in $(seq 60 60 $((DUR - 70))); do X="$X,$t:+space,$((t + 40)):-space,$((t + 58)):!privbrains"; done
P=""; for t in $(seq 56 15 $DUR); do P="$P,$t:!privpeace"; done
for g in $(seq 1 $((PLAYERS - 1))); do
  K=""; for t in $(seq 58 4 $((DUR - 2))); do K="$K,$t:!privgopilot=0:$((g * 60))"; done
  export GUEST${g}X="$X$P$K"
done
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 "$ROOT/scripts/privnet.sh" "$DUR" "$X$P" "$X$P" > /dev/null 2>&1
python3 "$ROOT/scripts/privtests/loadsum.py" "$OUT"
