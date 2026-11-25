#!/bin/bash
# Privateer's crew test (docs/privateer.md section 30; the user, 2026-11-24: "launch privateer in a different mode where
# player two (for Orion) or three (for galaxy) flies in the turret of the associated ship").  A crew's room
# (WCPRIVCREW=1, the host's): the host loads the user's save (SAVE=, default privsaves/OXFORD01.SAV: a Centurion with a
# tail turret of two neutron guns, docked at Oxford) and launches at 50 s; the guests register new pilots (a Tarsus
# each, at Achilles in Troy -- another system: the launch takes them to the pilot) and click their pads at GUNNER_AT
# (default 62), after the pilot's launch.  HOSTX / GUESTX: more of the key script (the host's clock; a guest's runs
# GUEST_LAG, 2 s, behind).
#
#   scripts/privtests/crew.sh [seconds]          (default 130)   PLAYERS=2 OUT=/tmp/privcrew DOSBOX= HOSTX= GUESTX= RF=1
# Printed: each machine's crew lines, errors and deaths.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-130}; PLAYERS=${PLAYERS:-2}; OUT=${OUT:-/tmp/privcrew}
export SAVE=${SAVE:-$ROOT/privsaves/OXFORD01.SAV}
[ -f "$SAVE" ] || { echo "no save at $SAVE"; exit 2; }
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
export HOSTENV="WCPRIVCREW=1 ${HOSTENV:-}"
# the host: the title's LOAD, the save's name into the box, LOAD (escort.sh); the pad's ship (0.60, 0.72) at 50 s
H="4:esc,7:!mouse=0.38:0.94,7.5:!button=0:1,7.8:!button=0:0,10:!mouse=0.31:0.405,10.5:!button=0:1,10.8:!button=0:0,12:!mouse=0.31:0.345,12.5:!button=0:1,12.8:!button=0:0,15:!mouse=0.85:0.345,15.5:!button=0:1,15.8:!button=0:0"
[ -z "${HOST_STAYS:-}" ] && H="$H,${PILOT_AT:-50}:!mouse=0.60:0.72,${PILOT_AT:-50}.5:!button=0:1,${PILOT_AT:-50}.8:!button=0:0"
# a guest: a new pilot (privrun.sh's route), the pad's ship (0.45, 0.6) at GUNNER_AT
GA=${GUNNER_AT:-62}
G="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,$((GA - 1)):!mouse=0.45:0.6,$GA:!button=0:1,$GA.3:!button=0:0"
if [ -n "${RF:-}" ]; then
  # Righteous Fire (RF=1): the host's NEW answers "Load an old Privateer ship?" with y and takes the saved game's ship
  # and credits into the new campaign (the pad's ship at 0.5, 0.6); a guest answers n and registers a new pilot
  export RF
  PA=${PILOT_AT:-50}
  H="4:esc,6:esc,10:!mouse=0.14:0.94,10.5:!button=0:1,10.8:!button=0:0,14:y,17:!mouse=0.31:0.405,17.5:!button=0:1,17.8:!button=0:0,19:!mouse=0.31:0.345,19.5:!button=0:1,19.8:!button=0:0,22:!mouse=0.85:0.345,22.5:!button=0:1,22.8:!button=0:0"
  [ -z "${HOST_STAYS:-}" ] && H="$H,$((PA - 1)):!mouse=0.5:0.6,$PA:!button=0:1,$PA.3:!button=0:0"
  G="4:esc,6:esc,10:!mouse=0.14:0.94,10.5:!button=0:1,10.8:!button=0:0,14:n,16:j,16.3:o,16.6:e,17:enter,17.5:a,17.8:c,18.1:e,18.5:enter,$((GA - 1)):!mouse=0.5:0.6,$GA:!button=0:1,$GA.3:!button=0:0"
fi
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 "$ROOT/scripts/privnet.sh" "$DUR" "$H${HOSTX:+,$HOSTX}" "$G${GUESTX:+,$GUESTX}" > /dev/null 2>&1
for d in host $(seq -f guest%g 1 $((PLAYERS - 1))); do
  f=$OUT/$d/run.err
  echo "== $d: errors $(grep -ac 'reports error' "$f"), deaths $(grep -ac 'our ship was destroyed' "$f"), program loads $(grep -ac 'PRCD.EXE loaded' "$f")"
  grep -a 'crew: \|reports error\|joined\|Aboard' "$f" | sed 's/^wcnet: Privateer: //; s/^wcnet: //' | cut -c1-170 | uniq -c | head -${LINES:-40}
done
