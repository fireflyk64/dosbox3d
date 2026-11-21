#!/bin/bash
# Privateer's bolt stress (the user, 2026-11-21: "Make a stress tests to routinely exceed the 200 limit (just by having
# like 4 players all with centurions all with 4 laser cannons all firing at max speed"; "maybe we'd need 8 centurions to
# really get there").  A room of PLAYERS (default 8) loads the user's save (SAVE=, default privsaves/OXFORD01.SAV: a
# Centurion docked at Oxford), launches together and holds the guns from 60 s to the end, with the copies' own caps
# lifted (WCPRIVCOPYSTRESS=1: every other pilot's volley copied here), so that the game's 200 kept pointers (a bolt
# keeps its shooter there; the 201st is its error 6281) and its near heap (error 1303) are reached -- and kept from by
# the bolt collector (docs/privateer.md section 18).  GC=off runs without the collector (what the game does then);
# GC=<n> sets its mark.  Printed: each machine's loads / errors / deaths and heap (loadsum.py), its most kept
# pointers, the collector's last line.
#
#   scripts/privtests/bolts.sh [seconds]          (default 150)   PLAYERS=8 GC=off|<n> OUT=/tmp/privbolts DOSBOX=
# A pass: errors=0 and loads=1 on every machine, the most kept under 200.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-150}; PLAYERS=${PLAYERS:-8}; OUT=${OUT:-/tmp/privbolts}
export SAVE=${SAVE:-$ROOT/privsaves/OXFORD01.SAV}
[ -f "$SAVE" ] || { echo "no save at $SAVE"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
E="WCPRIVCOPYSTRESS=1${GC:+ WCPRIVBOLTGC=$GC}"
export HOSTENV="$E ${HOSTENV:-}" GUESTENV="$E ${GUESTENV:-}"
# the title's LOAD, the save's name, into the box, LOAD (escort.sh); the pad's ship (0.60, 0.72) at 50 s; the guns held
X="4:esc,7:!mouse=0.38:0.94,7.5:!button=0:1,7.8:!button=0:0,10:!mouse=0.31:0.405,10.5:!button=0:1,10.8:!button=0:0,12:!mouse=0.31:0.345,12.5:!button=0:1,12.8:!button=0:0,15:!mouse=0.85:0.345,15.5:!button=0:1,15.8:!button=0:0"
X="$X,50:!mouse=0.60:0.72,50.5:!button=0:1,50.8:!button=0:0,60:+space,$((DUR - 5)):-space"
# kept together: Oxford's own hostiles put away (!privpeace: they scatter the room and land it), and every guest put
# 60 m a seat beside the host's ship every 4 s (the ships launch at speed and part beyond the copies' 3 km otherwise)
P=""; for t in $(seq 56 15 $DUR); do P="$P,$t:!privpeace"; done
for g in $(seq 1 $((PLAYERS - 1))); do
  K=""; for t in $(seq 58 4 $((DUR - 2))); do K="$K,$t:!privgopilot=0:$((g * 60))"; done
  export GUEST${g}X="$X$P$K"
done
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 "$ROOT/scripts/privnet.sh" "$DUR" "$X$P" "$X$P" > /dev/null 2>&1
python3 "$ROOT/scripts/privtests/loadsum.py" "$OUT"
for d in "$OUT"/host "$OUT"/guest*; do
  [ -f "$d/run.err" ] || continue
  echo "$(basename "$d"): most kept $(grep -a 'copied shots\|bolt collector:' "$d/run.err" | grep -o 'the most kept [0-9]*\|the most [0-9]*' | grep -o '[0-9]*' | sort -n | tail -1); game errors: $(grep -a 'error [0-9][0-9][0-9][0-9]' "$d/run.err" | grep -o 'error [0-9]*' | sort | uniq -c | tr '\n' ' ')"
  grep -a 'bolt collector:' "$d/run.err" | tail -1 | sed 's/.*bolt collector: /   /' | cut -c1-240
done
