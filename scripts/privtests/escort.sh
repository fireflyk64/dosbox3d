#!/bin/bash
# Privateer's Oxford escort with a helper (the user, 2026-11-08: "ensure that escort missions like Oxford function with
# several helpers"; 2026-11-09, a save at Oxford with Masterson's series open: "Maybe have two players load it").  Both
# machines load the save (SAVE=, default privsaves/OXFORD01.SAV, the user's: pilot Catnap in a Centurion, docked at Oxford):
#   the host, the mission holder, walks to the campus (the pad's "Train to Oxford University", 0.95 0.45), into the
#             Library (0.88, 0.20) where Masterson offers S3MA ("Meet with Hunter Toth's ship at Oxford system, Nav 3
#             and escort him safely to Oxford"): his lines skipped with Esc, the hand's Accept (0.5, 0.9); back by the
#             train (0.12, 0.72) and launches (the ship, 0.60 0.72)
#   guest 1,  the helper, launches with it and waits by Oxford
# The holder clears Oxford's own hostiles (the rig's !privpeace: the autopilot will not fly with them about), flies the
# autopilot to Nav 3 (the map, N: its big arrow 0.88 0.82 twice; Esc; A), stops, and talks Toth through (the user's
# notes: "talk to the Drayman there ... When you are done talking, you are attacked by 2 Retros.  When these are
# destroyed, a further 3 attack ... then autopilot back to Oxford.  Hunter Toth will follow"); the helper flies out
# as the talk ends; both turn on the nearest hostile ship firing guns and missiles, then fly the autopilot home (the
# arrow once more: Nav 4, Oxford), whose landing zone lands them.  Printed: the holder's missions and credits before
# and after, both machines' wing calls at Nav 3, the mission's ships as the helper shows them, hostile ships over time,
# deaths, errors.
#
#   scripts/privtests/escort.sh [seconds]          (default 450)   SAVE=<file> OUT=/tmp/privescort DOSBOX=
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-450}; OUT=${OUT:-/tmp/privescort}
export SAVE=${SAVE:-$ROOT/privsaves/OXFORD01.SAV}     # (the user's save, 2026-11-09; not in git)
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
# the title's LOAD (0.38, 0.94), the save's name in the list (0.31, 0.405), into the box (0.31, 0.345), LOAD (0.85, 0.345)
LOADR="4:esc,7:!mouse=0.38:0.94,7.5:!button=0:1,7.8:!button=0:0,10:!mouse=0.31:0.405,10.5:!button=0:1,10.8:!button=0:0,12:!mouse=0.31:0.345,12.5:!button=0:1,12.8:!button=0:0,15:!mouse=0.85:0.345,15.5:!button=0:1,15.8:!button=0:0"
H="$LOADR,20:!mouse=0.95:0.45,20.5:!button=0:1,20.8:!button=0:0,25:!mouse=0.88:0.20,25.5:!button=0:1,25.8:!button=0:0,28:!mouse=0.5:0.9"
for t in $(seq 29 35); do H="$H,$t:esc"; done
H="$H,36.5:!button=0:1,36.8:!button=0:0"
for t in $(seq 38 44); do H="$H,$t:esc"; done
H="$H,48:!mouse=0.12:0.72,48.5:!button=0:1,48.8:!button=0:0,53:!mouse=0.60:0.72,53.5:!button=0:1,53.8:!button=0:0,62:!privcredits"
G="$LOADR,50:!mouse=0.60:0.72,50.5:!button=0:1,50.8:!button=0:0"     # (launched with the holder: Oxford has its own hostiles)
leg() {   # <shift> <time> <clicks>: the autopilot to a nav point: the map (N), its big arrow (0.88, 0.82) stepping the
          # selected nav point (Nav 1 at the launch: twice for Nav 3; once more for Nav 4), Esc, A
  local d=$1 t=$2 k x="$(( $2 - $1 )):n,$(( $2 - $1 )).5:!mouse=0.88:0.82"
  for k in $(seq 1 $3); do x="$x,$(( t + k - d )):!button=0:1,$(( t + k - d )).3:!button=0:0"; done
  echo "$x,$(( t + $3 + 1 - d )):!frame=map$t,$(( t + $3 + 2 - d )):esc,$(( t + $3 + 3 - d )):a"
}
fight() {  # <shift> <from> <to>: the nearest hostile ship turned on, guns held, a missile (Enter) every 6 s
  local d=$1 t=$2 e=$3 x="" k
  for k in $(seq $2 30 $(( $3 - 10 ))); do x="$x,$(( k - d )):!privturn=pirate:90:30"; done
  x="$x,$(( t + 1 - d )):+space,$(( e - d )):-space"
  for k in $(seq $(( t + 4 )) 6 $(( e - 2 ))); do x="$x,$(( k - d )):enter"; done
  echo "${x#,}"
}
# the holder: the Oxford hostiles cleared (the rig's !privpeace: the autopilot will not fly with them about), the
# autopilot to Nav 3, a stop (Backspace), and Toth's talk -- the comm (C), Toth (2), the answer (1), three times: "Hey,
# I'm no Retro." / "Sir, I'm here to escort you to Oxford." / "Don't worry.  You're in good hands." -- then two Retros,
# then three more; home by the autopilot with Toth following, the landing zone's automatic landing
H="$H,62:!privpeace,$(leg 0 64 2),80:backspace,90:c,91.5:!frame=k1,92:2,93.5:!frame=k2,95:1,102:c,103.5:!frame=k3,104:2,107:1,116:c,117.5:!frame=k4,118:2,121:1"
# (to the fight at speed -- "equals" ten times -- a missile selected: W, fired with Enter)
EQ() { local x="" k; for k in 0 1 2 3 4 5 6 7 8 9; do x="$x,$(( $1 - $2 )).$k:equals"; done; echo "${x#,},$(( $1 + 1 - $2 )):w"; }
# the waves: the holder holds its fire by Toth while the helper fights the first for real (its hits go to the holder's
# game); each wave is then ended by the rig's !privkillhostile in the holder's game -- a won fight's stand-in (the rigs'
# gunnery is a turn and a held trigger: left to it, the second wave killed Toth in 12 s and then the holder: measured)
# -- the second coming as the first is gone; then the autopilot home, Toth following
# home: the autopilot drops the holder 14.6 km from Oxford, stopped; Toth follows at his own pace (some 165 m/s: ~3
# minutes behind), and when he is near the holder flies on into the landing zone, which lands it
H="$H,138:!privkillhostile,146:!privkillhostile,$(leg 0 152 1),200:!privcredits,$(EQ 330 0),420:!privcredits"
# the helper: stopped by Oxford (Backspace: drifting on it enters the base's landing zone, where the autopilot will not
# fly), its hostiles cleared too; it stays away while the holder talks (the comm's list offers the nearest ship: with
# the helper close by, "2" was the helper, not Toth: measured) and joins it as the Retros come (the rig's
# !privgopilot=0:2500, beside the holder's reported place), fights at speed, home by the autopilot (Nav 1 still selected:
# the arrow thrice for Nav 4)
G="$G,54:backspace,60:!privpeace,$(( 124 - 2 )):!privgopilot=0:2500,$(EQ 125 2),$(fight 2 126 146),$(leg 2 152 3)"
for t in 75 100 124 135 142 150 160 200 250 300 330 345 360 380 400; do H="$H,$t.5:!frame=f$t"; G="$G,$(( t - 2 )).5:!frame=f$t"; done
H="$H,130:!privscene,150:!privscene,180:!privscene,240:!privscene,300:!privscene,329:!privscene,360:!privscene"
OUT=$OUT PLAYERS=2 LINES_EACH=0 HOSTENV="PRIVCOMMLOG=1 ${HOSTENV:-}" "$ROOT/scripts/privnet.sh" "$DUR" "$H" "$G" > /dev/null 2>&1
echo "holder: $(grep -a 'our missions' "$OUT/host/run.err" | sed 's/.*Privateer: //' | tr '\n' ';')"
echo "holder: $(grep -a 'our credits' "$OUT/host/run.err" | sed 's/.*: our credits/credits/' | tr '\n' ' ')"
for d in host guest1; do
  f=$OUT/$d/run.err
  echo "== $d: errors $(grep -ac 'reports error' "$f"), deaths $(grep -ac 'our ship was destroyed' "$f"), program loads $(grep -ac 'PRCD.EXE loaded' "$f")"
  grep -a "wing spawn at nav 2\|nav 2 is\|Oxford nav 2\|co-op: the wing at nav 2\|wing of .* at nav 2\|at nav [0-9] (was\|left nav" "$f" | sed 's/.*Privateer: /   /' | head -12
  echo "   shown from pilot 1: $(grep -a "Privateer: pilot 1's [A-Z]* (" "$f" | sed "s/.*pilot 1's \([A-Z]*\) (\([A-Z]*\), \([A-Z_]*\)).*/\1 \2 \3/" | sort | uniq -c | tr '\n' ';')"
  echo "   hostile ships in its scene: $(grep -a 'keyboard frames' "$f" | sed 's/.*t=\([0-9]*\)\.[0-9],.*pirates \([0-9]*\),.*/\1:\2/' | awk -F: '{if ($2!=last) printf "%s ", $0; last=$2}')"
done
