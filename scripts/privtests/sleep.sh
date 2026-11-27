#!/bin/bash
# The lowest seat of an open Privateer room put to sleep and woken (docs/privateer.md section 40; the user, 2026-11-27:
# "they can sleep the laptop and then wake it and continue playing"; "What happens during the low player handoff during
# that two minute disconnect dance if it's lowest player ... it must be robust"; "The 15s where I am has to also change
# if a player moves a bunch").
#
#   scripts/privtests/sleep.sh        OUT=/tmp/privsleep DOSBOX= (a build with lobbylink: configure's default)
#
# Wall seconds (each machine scripts/privrun.sh's route: launched from Achilles at ~22 s of its own):
#   0      HOST (seat 0, the decider) comes; 3 GUEST1 (seat 1); 6 GUEST2 (seat 2)
#   ~56    GUEST2 put 30 km off nav 0 (!privgonav=0:30000, in no sphere: told as nav 1 left), then 15 km on
#          (!privgonav=0:45000): no sphere left or entered, its presence goes out at once all the same ("a move"): its presence goes out at once ("a move")
#   SLEEP  (75) HOST's emulator stopped (SIGSTOP: the computer asleep -- nothing it says, nothing it answers, its
#          connections left open)
#   WAKE   (SLEEP + ASLEEP, 60 s) HOST's emulator goes on (SIGCONT)
#   END    (230) everybody stops
# A native machine has no page to join the room again (web/wc.js resumeRoom does that in a browser:
# scripts/web-priv-sleep.sh): here the links come back only if lobbylink's own rebuild makes them.  Printed: each
# machine's room lines, and the checks.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/privsleep}; SEATS=${SEATS:-32}
SLEEP=${SLEEP:-75}; ASLEEP=${ASLEEP:-60}; END=${END:-230}
rm -rf "$OUT"; mkdir -p "$OUT"
LP=${LOBBY_PORT:-8864}; ROOM=PRV-$RANDOM
LOBBYSERVER="$ROOT/build-web/p2p-lobby-server"
[ -x "$LOBBYSERVER" ] || { echo "no lobby server at $LOBBYSERVER"; exit 2; }
export DOSBOX=${DOSBOX:-$ROOT/build-open/src/dosbox}
printf '[rooms]\nmax_players_hard = 32\n' > $OUT/lobby.toml
"$LOBBYSERVER" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LP --public-url http://127.0.0.1:$LP --allow-no-origin > $OUT/lobby.log 2>&1 &
LPID=$!; sleep 1
NET="WCROOM=$ROOM WCLOBBY=http://127.0.0.1:$LP WCLOBBY_ORIGIN= WCPLAYERS=$SEATS"
export COPY=1
fly() {  # name wall-duration key-script
  OUT=$OUT/$1 ENV="$NET WCCALLSIGN=$1" "$ROOT/scripts/privrun.sh" $2 "$3" > /dev/null 2>&1
}
T0=$(date +%s)
fly HOST $END "" & P="$!"
sleep 3; fly GUEST1 $((END - 3)) "" & P="$P $!"
sleep 3; fly GUEST2 $((END - 6)) "50:!privgonav=0:30000,54:!privgonav=0:45000" & P="$P $!"
emu() { ps -eo pid=,comm=,args= | awk -v c="$OUT/$1/priv.conf" '$2 == "dosbox" && index($0, c) { print $1 }'; }
sleep $((SLEEP - 6))
H=$(emu HOST); echo "$(( $(date +%s) - T0 )) s: HOST's emulator ($H) asleep"
kill -STOP $H
sleep $ASLEEP
kill -CONT $H; echo "$(( $(date +%s) - T0 )) s: HOST's emulator awake"
wait $P
kill $LPID 2>/dev/null
for d in HOST GUEST1 GUEST2; do
  echo "===== $d ($OUT/$d/run.err)"
  grep -a "Privateer: \(OPEN\|linked\|.* is in the room\|.* left the room\|we decide\|we are the lowest\|pilot [0-9]* decides\|pilot [0-9]* has not said\|pilot [0-9]* says who\|back after\|our presence sent (a move)\|.*'s .* is in your space\|pilot [0-9]*'s .* given back\|.*: its owner by the table\|reports error\)\|link lost\|link up\|lost its lobby" $OUT/$d/run.err \
    | sed 's/^wcnet: Privateer: //; s/^wcnet: //' | cut -c1-150 | uniq -c | head -${LINES_EACH:-40}
done
echo "===== checks"
ok=1
chk() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; ok=0; fi; }
# (in the order things happen)
chk "GUEST2's move told at once" "grep -aq 'our presence sent (a move)' $OUT/GUEST2/run.err"
# (GUEST1 has GUEST2 45 km off nav 0 -- x over 66 km, 17,000,000 units -- from its presence)
chk "GUEST1 hears where GUEST2 went" "grep -a 'GUEST2 (pilot 3) flying in' $OUT/GUEST2/../GUEST1/run.err | awk '{ x = \$0; sub(/.* at /, \"\", x); if (x + 0 > 17000000) f = 1 } END { exit !f }'"
chk "GUEST1 passes the sleeping decider over" "grep -aq 'pilot 1 has not said who runs the nav points' $OUT/GUEST1/run.err"
chk "GUEST2 passes the sleeping decider over" "grep -aq 'pilot 1 has not said who runs the nav points' $OUT/GUEST2/run.err"
chk "GUEST1 decides in its place" "grep -aq 'we decide the nav points' $OUT/GUEST1/run.err"
chk "GUEST2 takes GUEST1 for the decider" "grep -aq 'pilot 2 decides the nav points' $OUT/GUEST2/run.err"
chk "GUEST1 and GUEST2 see each other through it" "grep -aq 'GUEST2.s .* is in your space' $OUT/GUEST1/run.err && grep -aq 'GUEST1.s .* is in your space' $OUT/GUEST2/run.err"
chk "HOST knows it was away" "grep -aq 'back after' $OUT/HOST/run.err"
chk "HOST decides again once it has the room's table" "grep -a 'back after' -A 400 $OUT/HOST/run.err | grep -aq \"we decide the nav points' owners now (the lowest seat here, the room's table heard)\""
chk "no game errors" "! grep -aq 'reports error' $OUT/*/run.err"
[ $ok = 1 ] && echo "SLEEP OK" || echo "SLEEP FAILED"
