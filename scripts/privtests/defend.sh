#!/bin/bash
# Privateer's defend-base mission with three helpers (the user, 2026-11-09: "now test with some of those defend nav
# point missions and a few other missions with multiple helpers").  Four machines load the user's save (Oxford,
# Masterson's series open) with the clock fixed (WCNET_BOOTCLOCK: the Mission Computer deals the same missions every
# run): the host, the mission holder, takes Oxford's Mission Computer's fourth, "DEFEND BASE MISSION ... Oxford in
# Oxford system seeks reinforcements ... strong Kilrathi blockade" (3,890 credits; objectives Nav 4, Oxford); the three
# helpers launch just after it.  Everybody stops by the base (a fight chased by it enters its landing zone, which lands
# the ship: measured).  The rig's piloting cannot win this fight (a Dralthi outflies it; a helper put behind one lands;
# the T key's nearest ship was a fellow pilot), so at 100 s the attack is ended by !privkillhostile in the holder's game
# (a won fight's stand-in); then everybody docks (the autopilot will not fly 2 km from Oxford: the rig's teleport into
# the base's sphere, which lands a ship).  Printed: the holder's missions (each objective's state) and credits, each
# machine's wing calls at Oxford, the Kilrathi as each shows them, landings, deaths.
#
#   scripts/privtests/defend.sh [seconds]          (default 160)   PLAYERS=4 OUT=/tmp/privdefend DOSBOX= SAVE=
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-160}; PLAYERS=${PLAYERS:-4}; OUT=${OUT:-/tmp/privdefend}
export SAVE=${SAVE:-$ROOT/privsaves/OXFORD01.SAV}
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
LOADR="4:esc,7:!mouse=0.38:0.94,7.5:!button=0:1,7.8:!button=0:0,10:!mouse=0.31:0.405,10.5:!button=0:1,10.8:!button=0:0,12:!mouse=0.31:0.345,12.5:!button=0:1,12.8:!button=0:0,15:!mouse=0.85:0.345,15.5:!button=0:1,15.8:!button=0:0"
# the holder: the campus (0.95, 0.45), the Mission Computer (0.20, 0.30), the palm (0.72, 0.82), the right arrow
# (0.34, 0.82) three times: the fourth mission; Accept (0.47, 0.81), out (0.5, 0.96), the train (0.12, 0.72), launch
H="$LOADR,20:!mouse=0.95:0.45,20.5:!button=0:1,20.8:!button=0:0,25:!mouse=0.20:0.30,25.5:!button=0:1,25.8:!button=0:0,29:!mouse=0.72:0.82,29.5:!button=0:1,29.8:!button=0:0,33.5:!mouse=0.34:0.82"
for t in 34 35 36; do H="$H,$t:!button=0:1,$t.3:!button=0:0"; done
H="$H,37:!frame=mission,38:!mouse=0.47:0.81,38.5:!button=0:1,38.8:!button=0:0,41:!mouse=0.5:0.96,41.5:!button=0:1,41.8:!button=0:0,45:!mouse=0.12:0.72,45.5:!button=0:1,45.8:!button=0:0,50:!mouse=0.60:0.72,50.5:!button=0:1,50.8:!button=0:0,48:!privcredits"
fly() {   # <shift>: by the base, then home, on the host's clock
  local d=$1 x="" t
  # stopped by the base (Backspace): a fight chased by the base enters its landing zone, which lands the ship (measured:
  # the holder hunting a Dralthi landed at 81 s); the T key's nearest ship was a fellow pilot (no missiles, then)
  x="$((60 - d)):backspace"
  # home: the autopilot will not fly 2 km from Oxford ("Destination: Oxford"): the rig's teleport into the base's
  # sphere, which lands a ship, one pilot a second
  x="$x,$((110 + d / 2)):!privgonav=3:0"
  for t in 65 80 95 105; do x="$x,$((t - d)).5:!frame=f$t"; done
  echo "$x"
}
# the attack ended at 100 s by the rig's !privkillhostile in the holder's game (a won fight's stand-in), then home
H="$H,$(fly 0),100:!privkillhostile,140:!privcredits,62:!privscene,99:!privscene"
for g in $(seq 1 $((PLAYERS - 1))); do
  d=$((2 * g))
  export GUEST${g}X="$LOADR,$((58 - d + g)):!mouse=0.60:0.72,$((58 - d + g)).5:!button=0:1,$((58 - d + g)).8:!button=0:0,$(fly $d)"
done
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 HOSTENV="WCNET_BOOTCLOCK=12:00:00 PRIVCOMMLOG=1 ${HOSTENV:-}" GUESTENV="WCNET_BOOTCLOCK=12:00:00 ${GUESTENV:-}" GUEST_LAG=2 \
  "$ROOT/scripts/privnet.sh" "$DUR" "$H" "" > /dev/null 2>&1
echo "holder: $(grep -a 'our missions' "$OUT/host/run.err" | sed 's/.*Privateer: //' | tr '\n' ';')"
echo "holder: $(grep -a 'our credits' "$OUT/host/run.err" | sed 's/.*: our credits/credits/' | tr '\n' ' ')"
for d in host $(seq -f guest%g 1 $((PLAYERS - 1))); do
  f=$OUT/$d/run.err
  echo "== $d: errors $(grep -ac 'reports error' "$f"), deaths $(grep -ac 'our ship was destroyed' "$f"), program loads $(grep -ac 'PRCD.EXE loaded' "$f"), landings $(grep -ac 'presence sent (a landing)' "$f")"
  grep -a "wing spawn at nav 3\|nav 3 is\|Oxford nav 3" "$f" | sed 's/.*Privateer: /   /' | sort | uniq -c | head -6
  echo "   hits sent on pilot 1's ships: $(grep -ac "our hit on pilot 1's encounter ship" "$f"); pilot 1's ships destroyed here: $(grep -a "pilot 1's [a-z]* (ds:[0-9A-F]*) destroyed" "$f" | sed "s/.*pilot 1's \([a-z]*\) .*/\1/" | sort | uniq -c | tr '\n' ' ')"
  echo "   hostile ships in its scene: $(grep -a 'keyboard frames' "$f" | sed 's/.*t=\([0-9]*\)\.[0-9],.*pirates \([0-9]*\),.*/\1:\2/' | awk -F: '{if ($2!=last) printf "%s ", $0; last=$2}')"
done
