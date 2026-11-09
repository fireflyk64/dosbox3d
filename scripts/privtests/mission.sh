#!/bin/bash
# Privateer's mission-nav test (the user, 2026-11-08: "ensure that escort missions like Oxford function with several
# helpers").  Three pilots at Achilles in Troy.  Guest 1, the mission holder, takes the Mission Computer's first mission
# (random: the log's "our missions" line says which nav points), launches, has a ship of its own game beside it (a
# merchant made by !privspawn, standing in for an escort's ship: a ship that is its game's alone) and flies to Nav 3
# (sphere 2, a plain nav point) at 58 s -- made one of its mission's nav points by the rig (PRIVMISSIONS=2: the mission
# list is random, the rule is what is tested).  The helpers:
#   MODE=first      the host reaches sphere 2 at 40 s, before the holder has launched (its missions unknown to the
#                   room): the host's game spawns the nav's encounter; the holder's game, arriving, spawns its own
#                   wings there too (a mission's ships are made by its holder's game alone); guest 2 comes at 62 s
#   MODE=reserved   guest 2 reaches sphere 2 at 52 s, the holder flying but at Achilles: its wings are left to the
#                   holder's game (the nav point reserved), which spawns them as it comes, shared with guest 2
# Everybody then turns on the nearest pirate firing.  Printed: the holder's missions, every machine's wing calls and
# what it decided, the holder's ships as the others show them, deaths and errors.
#
#   scripts/privtests/mission.sh [seconds]          (default 130)   MODE=first|reserved OUT=/tmp/privmission
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-130}; MODE=${MODE:-reserved}; OUT=${OUT:-/tmp/privmission}
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
REG="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,15:!mouse=0.93:0.50,16:!button=0:1,16.3:!button=0:0"
PAD="${REG%%,15:*},15:!mouse=0.45:0.60,16:!button=0:1,16.3:!button=0:0"     # (the pad's ship: in flight ~22 s)
FIGHT="70:!privturn=pirate:60:40,71:+space,85:-space,87:enter,89:+space,105:-space"
# the Mission Computer (0.80, 0.55): the palm (0.72, 0.82), the right arrow (0.34, 0.82) four times, Accept (0.47, 0.81),
# out at the screen's bottom (0.5, 0.96), the Hangar (0.15, 0.45), the ship (0.45, 0.60): in flight ~46 s
HOLD="$REG,19:!mouse=0.80:0.55,20:!button=0:1,20.3:!button=0:0,23:!mouse=0.72:0.82,23.5:!button=0:1,23.8:!button=0:0,25.5:!mouse=0.34:0.82"
for t in 26 27 28 29; do HOLD="$HOLD,$t:!button=0:1,$t.3:!button=0:0"; done
HOLD="$HOLD,30:!frame=offer,31:!mouse=0.47:0.81,31.5:!button=0:1,31.8:!button=0:0,33:!frame=accepted,34:!mouse=0.5:0.96,34.5:!button=0:1,34.8:!button=0:0"
HOLD="$HOLD,37:!mouse=0.15:0.45,37.5:!button=0:1,37.8:!button=0:0,40:!mouse=0.45:0.60,40.5:!button=0:1,40.8:!button=0:0"
HOLD="$HOLD,50:!privspawn=TARSTYPE:CLUNKER:MER_DP:0:76800:0,58:!privgonav=2:3200,$FIGHT"
if [ "$MODE" = first ]; then
  H="$PAD,40:!privgonav=2:3000,$FIGHT"; G2="$PAD,62:!privgonav=2:3400,$FIGHT"
else
  H="$PAD,$FIGHT"; G2="$PAD,52:!privgonav=2:3400,$FIGHT"
fi
OUT=$OUT PLAYERS=3 LINES_EACH=0 GUEST2X="$G2" GUEST1ENV="PRIVMISSIONS=2" "$ROOT/scripts/privnet.sh" "$DUR" "$H" "$HOLD" > /dev/null 2>&1
grep -a 'our missions' "$OUT/guest1/run.err" | tail -1 | sed 's/.*Privateer: /holder: /'
for d in host guest1 guest2; do
  f=$OUT/$d/run.err
  echo "== $d: errors $(grep -ac 'reports error' "$f"), deaths $(grep -ac 'our ship was destroyed' "$f"), program loads $(grep -ac 'PRCD.EXE loaded' "$f")"
  grep -a "wing spawn at nav 2\|nav 2 is\|nav 2: pilot\|Troy nav 2" "$f" | sed 's/.*Privateer: /   /' | sort | uniq -c | head -8
  echo "   shown here from pilot 2 (the holder): $(grep -a "Privateer: pilot 2's [A-Z]* (" "$f" | sed "s/.*pilot 2's \([A-Z]*\) (\([A-Z]*\), \([A-Z_]*\)).*/\1 \2 \3/" | sort | uniq -c | tr '\n' ';')"
  echo "   pirates in its scene, most: $(grep -a 'pirates [0-9]*,' "$f" | sed 's/.*pirates \([0-9]*\),.*/\1/' | sort -n | tail -1)"
done
