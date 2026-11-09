#!/bin/bash
# Privateer's four-ships test (the user, 2026-11-08: "grant enough money for a test rig to buy each of the four ships
# and make sure that the multiple players at the same nav works").  A room of PLAYERS (default 8) registers at Achilles
# in Troy; every machine is given 500,000 credits (!privcredits, a rig's aid only) and walks to the ship dealer, where
# seat s buys the ship s mod 4: 0 keeps the Tarsus, 1 an Orion, 2 a Centurion, 3 a Galaxy (the salesman's pitch skipped
# with Esc, the hand's Accept clicked); all launch together and meet at Achilles' nav point, the host adding PIRATES
# pirates (default 3), every pilot turning on the nearest pirate firing guns and missiles, and each saying a line.
# Printed: each machine's own ship, the others' ships as it made them, its heap's low (scripts/privtests/loadsum.py),
# its errors and deaths, and who heard whose line.
#
#   scripts/privtests/ships.sh [seconds]          (default 195)   PLAYERS=8 PIRATES=3 OUT=/tmp/privships
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-195}; PLAYERS=${PLAYERS:-8}; PIRATES=${PIRATES:-3}; OUT=${OUT:-/tmp/privships}
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
# registered, the pad's Main Concourse, the credits, the Ship Dealer (0.50, 0.45)
REG="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,15:!mouse=0.93:0.50,16:!button=0:1,16.3:!button=0:0,17:!privcredits=500000,19:!mouse=0.50:0.45,20:!button=0:1,20.3:!button=0:0"
# the purchase: the ship clicked at 24, its pitch skipped (Esc, 12 lines), Accept (the hand, 0.5 0.9), the thanks skipped;
# then outfitted as a pilot would before flying it (a new ship comes bare: no shields, no armour -- the first hit through
# kills it, the game's own rule, measured): Repair and Upgrade (0.60, 0.22), the list stepped with its down arrow (0.43,
# 0.80) to Tungsten Armor (item $2) and then to the best shield generator (item $3), each bought with a click on its
# picture (0.18, 0.72); out through "Ship Dealer" (0.04, 0.29).  (A gun needs a mount chosen: none bought.)
ts() { printf '%d.%d' $(( $1 / 10 )) $(( $1 % 10 )); }     # tenths of a second, as the key script's time
buy() {
  local x="23:!mouse=$1,24:!button=0:1,24.3:!button=0:0" t k
  for t in $(seq 26 37); do x="$x,$t:esc"; done
  x="$x,38:!mouse=0.5:0.9,38.5:!button=0:1,38.8:!button=0:0"
  for t in 40 41 42 43 44; do x="$x,$t:esc"; done
  x="$x,47:!mouse=0.60:0.22,47.5:!button=0:1,47.8:!button=0:0,49.5:!mouse=0.43:0.80"
  t=500
  for k in $(seq 1 $3); do
    x="$x,$(ts $t):!button=0:1,$(ts $((t + 3))):!button=0:0"; t=$((t + 8))
    if [ $k -eq $2 ]; then
      x="$x,$(ts $t):!mouse=0.18:0.72,$(ts $((t + 3))):!button=0:1,$(ts $((t + 6))):!button=0:0,$(ts $((t + 10))):!mouse=0.43:0.80"; t=$((t + 16))
    fi
  done
  x="$x,$(ts $t):!mouse=0.18:0.72,$(ts $((t + 3))):!button=0:1,$(ts $((t + 6))):!button=0:0,$(ts $((t + 15))):!frame=outfitted"
  x="$x,72:!mouse=0.04:0.29,72.5:!button=0:1,72.8:!button=0:0"
  echo "$x"
}
# out of the dealer to the Main Concourse (0.39, 0.08), the Hangar (0.15, 0.45), the ship on the pad: in flight ~88 s
LAUNCH="75:!mouse=0.39:0.08,75.5:!button=0:1,75.8:!button=0:0,78:!mouse=0.15:0.45,78.5:!button=0:1,78.8:!button=0:0,82:!mouse=0.45:0.60,82.5:!button=0:1,82.8:!button=0:0"
FIGHT="94:!privturn=pirate:60:150,95:+space,110:-space,112:enter,114:+space,130:-space,132:enter,134:+space,150:-space,152:enter,154:+space,170:-space"
script_for() {     # seat
  local s="$REG"
  case $(( $1 % 4 )) in
    1) s="$s,$(buy 0.32:0.71 8 17)";;      # Purchase Orion: Tungsten Armor item 8, Shield Generator 5 item 17
    2) s="$s,$(buy 0.10:0.35 7 14)";;      # Purchase Centurion: 7, Shield Generator 3 14
    3) s="$s,$(buy 0.67:0.64 8 16)";;      # Purchase Galaxy: 8, Shield Generator 3 16
  esac
  s="$s,$LAUNCH,$FIGHT,$((98 + 3 * $1)):!say=SEAT_$(( $1 + 1 ))_HERE,92:!privarmor"
  echo "$s"
}
H="$(script_for 0)"
for j in $(seq 0 $((PIRATES - 1))); do
  H="$H,$((90 + j)):!privspawn=TALNWIMP:TALPIR:PIR_DA:$(( (j % 5 - 2) * 200000 )):$(( 700000 + (j / 5) * 100000 )):$(( (j % 5) * 80000 ))"
done
for g in $(seq 1 $((PLAYERS - 1))); do export GUEST${g}X="$(script_for $g)"; done
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 GUEST_LAG=${GUEST_LAG:-1} "$ROOT/scripts/privnet.sh" "$DUR" "$H" "" > /dev/null 2>&1
for d in host $(seq -f guest%g 1 $((PLAYERS - 1))); do
  f=$OUT/$d/run.err
  own=$(grep -a 'ours a' "$f" | grep -av 'ours a -;' | tail -1 | sed 's/.*ours a \([^;]*\);.*/\1/')
  made=$(grep -a "Privateer: [A-Z0-9]*'s [A-Za-z]* is in your space" "$f" | sed "s/.*Privateer: \([A-Z0-9]*\)'s \([A-Za-z]*\) is.*/\1:\2/" | sort -u | tr '\n' ' ')
  heard=$(grep -a 'Privateer: say: ds:.*face frames' "$f" | wc -l)
  echo "== $d flies a ${own:-?}; the others' ships it showed: $made; lines said here by others' ships: $heard"
done
python3 "$ROOT/scripts/privtests/loadsum.py" "$OUT"
