#!/bin/bash
# Privateer's jump test (docs/privateer.md section 15; the user, 2026-11-08: "they all start in Troy and then one with
# the new jump drive leaves and arrives and leaves and arrives 6 times with all the fuel. And ensure memory stays flat
# and also what happens in the foreign system if two jump").  Three pilots in Troy:
#   the host     waits 2 km from Troy's jump point to Pyrenees (JMPPYRI), owning that nav, its encounter cleared
#   guest 1      is given 60,000 credits (!privcredits, a rig's aid only), buys a jump drive keeping its laser, is put
#                400 m from the jump point and presses J, then J again 15 s after every arrival: Troy -> Pyrenees ->
#                Troy ... until its fuel is gone (6 jumps)
#   guest 2      buys a drive too, waits by the jump point, jumps with guest 1's third trip out, and stays in Pyrenees
# Printed: each machine's errors, deaths and program loads, its system and heap at every change of system, and the
# host's heap every 10 s (flat: no puppet made again as the jumper comes and goes).
#
#   scripts/privtests/jump.sh [seconds]          (default 300)   OUT=/tmp/privjump DOSBOX=build-native/src/dosbox
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-300}; OUT=${OUT:-/tmp/privjump}
rm -rf "$OUT"; mkdir -p "$OUT"
export NOROUTE=1
BUYJ="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,15:!mouse=0.93:0.50,16:!button=0:1,16.3:!button=0:0,17:!privcredits=60000,19:!mouse=0.50:0.45,20:!button=0:1,20.3:!button=0:0,23:!mouse=0.60:0.22,24:!button=0:1,24.3:!button=0:0,26:!mouse=0.43:0.80,26.5:!button=0:1,26.8:!button=0:0,27.5:!button=0:1,27.8:!button=0:0,28.5:!button=0:1,28.8:!button=0:0,29.5:!button=0:1,29.8:!button=0:0,30.5:!button=0:1,30.8:!button=0:0,31.5:!button=0:1,31.8:!button=0:0,35.5:!mouse=0.18:0.72,36:!button=0:1,36.3:!button=0:0,37:!frame=bought,38:!mouse=0.04:0.29,39:!button=0:1,39.3:!button=0:0,41:!mouse=0.39:0.08,42:!button=0:1,42.3:!button=0:0,44:!mouse=0.15:0.45,45:!button=0:1,45.3:!button=0:0,48:!mouse=0.45:0.60,49:!button=0:1,49.3:!button=0:0"
H="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter,16:!mouse=0.45:0.60,17:!button=0:1,17.3:!button=0:0,28:backspace,30:!privpos=7680000:10240000:5632000,31:!privpeace"
for t in $(seq 40 8 "$DUR"); do H="$H,$t:!privpeace"; done
G1="$BUYJ,62:backspace,63:!privpos=7680000:10240000:5222400"
for k in $(seq 0 11); do t=$((66 + 15 * k)); G1="$G1,$t:j,$((t + 5)):!privpeace,$((t + 9)):!privpeace"; done
G2="$BUYJ,62:backspace,63:!privpos=7731200:10240000:5222400,70:!privpeace,80:!privpeace,90:!privpeace,100:!privpeace,110:!privpeace,120:!privpeace,128:j"
for t in $(seq 133 6 "$DUR"); do G2="$G2,$t:!privpeace"; done
OUT=$OUT PLAYERS=3 LINES_EACH=0 GUEST2X="$G2" "$ROOT/scripts/privnet.sh" "$DUR" "$H" "$G1" > /dev/null 2>&1
for d in host guest1 guest2; do
  f=$OUT/$d/run.err
  echo "== $d: errors $(grep -ac 'reports error' "$f"), deaths $(grep -ac 'our ship was destroyed' "$f"), program loads $(grep -ac 'PRCD.EXE loaded' "$f"), jumps $(grep -ac 'puppets forgotten (a jump)' "$f")"
  grep -a 'keyboard frames' "$f" | sed 's/.*t=\([0-9.]*\),.*near heap free \(-\?[0-9]*\) (lowest this second \(-\?[0-9]*\).*ours here: \([0-9]*\) pilots, \([0-9]*\) encounter.*; in \(.*\)/\1 \6 low=\3 pilots=\4 ships=\5/' |
    awk -v every="$([ "$d" = host ] && echo 10 || echo 0)" '{ split($1, t, "."); if ($2 != last || (every > 0 && t[1] % every == 0)) { print "   " $0; last = $2 } }'
done
grep -a 'owns it\|ours to run' "$OUT"/*/run.err | grep -a Pyrenees | sed 's/.*Privateer: //' | sort | uniq -c | head
