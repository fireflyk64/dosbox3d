#!/bin/bash
# Privateer's Mission Computer missions flown by two holders and two helpers in Troy (the user, 2026-11-09: "now test
# with some of those defend nav point missions and a few other missions with multiple helpers").  Four new pilots at
# Achilles with the clock fixed (WCNET_BOOTCLOCK: the computer deals the same six every run: 1 DEFEND BASE "Retro
# aggression at Helen in the Troy system" 6,539; 2 DEFEND BASE Hector 6,351; 3, 4 CARGO to Newcastle; 5 ATTACK "Retro
# presence eliminated at Nav 5 in the Troy system" 5,765; 6 SCOUT "disappearance of military craft at Nav 7" 2,976):
#   the host    takes the scout (6) and the Helen defence (1): objectives D0 Nav 7 (sphere 6); D6 Nav 6 (sphere 5,
#               Helen) and E2/01 (back to Achilles)
#   guest 1     takes the attack (5): Nav 5 (sphere 4)
#   guests 2, 3 helpers, no mission
# All launch, then fly together (the rig's !privgonav: gathered outside a sphere, in together, then close) to Nav 5,
# Nav 7 and Helen; at Nav 5 and Helen the fight is everybody's for 25 s (the rig's !privturn=hunt), then ended by
# !privkillhostile in the holder's game (a won fight's stand-in: the rig's piloting does not win fights); then all dock
# at Achilles (the rig's teleport into its sphere, which lands a ship), where the missions pay.  Printed: both holders'
# missions (each objective's state) and credits, every machine's wing calls at each mission's nav point, the mission
# ships as each shows them, landings, deaths, errors.
#
#   scripts/privtests/missions.sh [seconds]          (default 320)   PLAYERS=4 OUT=/tmp/privmissions DOSBOX=
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-320}; PLAYERS=${PLAYERS:-4}; OUT=${OUT:-/tmp/privmissions}
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
REG="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,15:!mouse=0.93:0.50,16:!button=0:1,16.3:!button=0:0"
MC="19:!mouse=0.80:0.55,20:!button=0:1,20.3:!button=0:0,23:!mouse=0.72:0.82,23.5:!button=0:1,23.8:!button=0:0,27:!mouse=0.34:0.82"
rights() { local x="" t k; for k in $(seq 1 $2); do t=$(( $1 + k )); x="$x,$t:!button=0:1,$t.3:!button=0:0"; done; echo "${x#,}"; }
# the host: the scout (five rights: the sixth), Accept; the left arrow (0.25, 0.82) six times back to the first, Accept
H="$REG,$MC,$(rights 27 5),33:!frame=scout,34:!mouse=0.47:0.81,34.5:!button=0:1,34.8:!button=0:0,46:!mouse=0.25:0.82"
for t in 47 48 49 50 51 52; do H="$H,$t:!button=0:1,$t.3:!button=0:0"; done
H="$H,53:!frame=defend,54:!mouse=0.47:0.81,54.5:!button=0:1,54.8:!button=0:0"
# guest 1: the attack (four rights: the fifth), Accept
G1="$REG,$MC,$(rights 27 4),32:!frame=attack,34:!mouse=0.47:0.81,34.5:!button=0:1,34.8:!button=0:0"
# out of the computer (0.5, 0.96), the Hangar (0.15, 0.45), the ship (0.45, 0.60): in flight ~70 s
OUTL="57:!mouse=0.5:0.96,57.5:!button=0:1,57.8:!button=0:0,60:!mouse=0.15:0.45,60.5:!button=0:1,60.8:!button=0:0,63:!mouse=0.45:0.60,63.5:!button=0:1,63.8:!button=0:0"
HELPER="${REG%%,15:*},58:!mouse=0.45:0.60,58.5:!button=0:1,58.8:!button=0:0"   # (a helper launches from the pad)
go() {    # <shift> <seat> <sphere> <time> <gather m> <enter m> <close m>: a place reached together
  local d=$1 s=$2 sp=$3 t=$4
  echo "$((t - d)):!privgonav=$sp:$(( $5 + 150 * s )),$((t + 6 - d)):!privgonav=$sp:$(( $6 + 150 * s )),$((t + 8 - d)):!privgonav=$sp:$(( $7 + 300 * s ))"
}
fight() { echo "$(( $2 - $1 )):!privturn=hunt:120:25"; }
route() { # <shift> <seat>
  local d=$1 s=$2 x
  x="$((68 - d)):w"
  for k in 0 1 2 3 4 5 6 7 8 9; do x="$x,$((68 - d)).$k:equals"; done
  x="$x,$(go $d $s 4 72 13500 11000 600),$(fight $d 82)"              # Nav 5: guest 1's attack
  x="$x,$(go $d $s 6 115 13500 11000 600)"                              # Nav 7: the host's scout
  x="$x,$(go $d $s 5 140 18500 16000 6000),$(fight $d 150)"            # Helen (a base: kept 6 km out, where its attacker comes)
  x="$x,$((245 + s)):!privgonav=1:0"                                    # home: docked at Achilles, one a second
  for t in 85 100 120 145 160 180; do x="$x,$((t - d)).5:!frame=f$t"; done
  echo "$x"
}
H="$H,$OUTL,$(route 0 0),108:!privkillhostile,190:!privkillhostile,205:!privkillhostile,220:!privkillhostile,235:!privkillhostile,66:!privcredits,280:!privcredits,160:!privscene,189:!privscene,200:!privscene,215:!privscene,230:!privscene"
export GUEST1X="$G1,$OUTL,$(route 2 1),106:!privkillhostile,64:!privcredits,278:!privcredits"
for g in $(seq 2 $((PLAYERS - 1))); do export GUEST${g}X="$HELPER,$(route $((2 * g)) $g)"; done
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 HOSTENV="WCNET_BOOTCLOCK=12:00:00 PRIVCOMMLOG=1 ${HOSTENV:-}" GUESTENV="WCNET_BOOTCLOCK=12:00:00 ${GUESTENV:-}" GUEST_LAG=2 \
  "$ROOT/scripts/privnet.sh" "$DUR" "$H" "" > /dev/null 2>&1
for d in host guest1; do
  echo "$d: $(grep -a 'our missions' "$OUT/$d/run.err" | sed 's/.*Privateer: our missions: //' | tr '\n' ';')"
  echo "$d: $(grep -a 'our credits' "$OUT/$d/run.err" | sed 's/.*: our credits/credits/' | tr '\n' ' ')"
done
for d in host $(seq -f guest%g 1 $((PLAYERS - 1))); do
  f=$OUT/$d/run.err
  echo "== $d: errors $(grep -ac 'reports error' "$f"), deaths $(grep -ac 'our ship was destroyed' "$f"), program loads $(grep -ac 'PRCD.EXE loaded' "$f"), landings $(grep -ac 'presence sent (a landing)' "$f")"
  grep -a "wing spawn at nav [456] \|nav [456] is a mission\|Troy nav [456]: pilot\|our mission's" "$f" | sed 's/.*Privateer: /   /; s/(call [0-9]*, /(/' | sort | uniq -c | head -10
  echo "   others' ships shown: $(grep -a "Privateer: pilot [12]'s [a-zA-Z]* ([a-z]*, [A-Z_0-9]*)" "$f" | sed "s/.*pilot \([12]\)'s [a-zA-Z]* (\([a-z]*\), \([A-Z_0-9]*\)).*/\1:\2:\3/" | sort | uniq -c | tr '\n' ' ')"
  echo "   their deaths shown: $(grep -a "Privateer: pilot [12]'s [a-zA-Z]* (ds:[0-9A-F]*) destroyed" "$f" | sed "s/.*pilot \([12]\)'s \([a-zA-Z]*\) .*/\1:\2/" | sort | uniq -c | tr '\n' ' ')"
  echo "   hostile ships: $(grep -a 'keyboard frames' "$f" | sed 's/.*t=\([0-9]*\)\.[0-9],.*pirates \([0-9]*\),.*/\1:\2/' | awk -F: '{if ($2!=last) printf "%s ", $0; last=$2}')"
done
