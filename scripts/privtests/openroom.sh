#!/bin/bash
# An open Privateer room (docs/privateer.md section 37; the user, 2026-11-27: "join a room code after it launches and
# leave ... come back ... the room limit is very high ... 32"): pilots come and go in a local lobby room of SEATS seats.
#
#   scripts/privtests/openroom.sh            OUT=/tmp/privopen SEATS=32 DOSBOX= (a build with lobbylink: configure's default)
#
# Wall seconds (each machine scripts/privrun.sh's route: a new pilot launched from Achilles at ~22 s of its own):
#   0     HOST (seat 0) comes; it leaves at HOST_DUR (110: killed, as a tab that crashes)
#   3     GUEST1 comes, stays to the end
#   LATE  GUEST2 comes (60: while the others fly), stays to the end
#   BACK  HOST comes back as a new pilot, BACK2 (150: after it left)
#   END   (260) everybody stops
# Each machine presses T (the target: the nearest ship) at intervals and keeps a frame (!frame=) then.  Printed: each
# machine's room lines, and the checks.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/privopen}; SEATS=${SEATS:-32}
HOST_DUR=${HOST_DUR:-110}; LATE=${LATE:-60}; BACK=${BACK:-150}; END=${END:-260}
rm -rf "$OUT"; mkdir -p "$OUT"
LP=${LOBBY_PORT:-8861}; ROOM=PRV-$RANDOM
LOBBYSERVER="$ROOT/build-web/p2p-lobby-server"
[ -x "$LOBBYSERVER" ] || { echo "no lobby server at $LOBBYSERVER"; exit 2; }
printf '[rooms]\nmax_players_hard = 32\n' > $OUT/lobby.toml
"$LOBBYSERVER" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LP --public-url http://127.0.0.1:$LP --allow-no-origin > $OUT/lobby.log 2>&1 &
LPID=$!; sleep 1
NET="WCROOM=$ROOM WCLOBBY=http://127.0.0.1:$LP WCLOBBY_ORIGIN= WCPLAYERS=$SEATS"
export COPY=1
# T and a frame every 20 s of a machine's own time from 40 s
shots() { local who=$1 out="" t; for t in $(seq 40 20 $2); do out="$out,$t:t,$((t + 1)):!frame=${who}_$t"; done; echo "${out#,}"; }
fly() {  # name wall-duration key-script
  OUT=$OUT/$1 ENV="$NET WCCALLSIGN=$1" "$ROOT/scripts/privrun.sh" $2 "$3" > /dev/null 2>&1
}
fly HOST $HOST_DUR "$(shots host 200)" & P="$!"
sleep 3; fly GUEST1 $((END - 3)) "$(shots guest1 260)" & P="$P $!"
sleep $((LATE - 3)); fly GUEST2 $((END - LATE)) "$(shots guest2 200)" & P="$P $!"
sleep $((BACK - LATE)); fly BACK2 $((END - BACK)) "$(shots back2 120)" & P="$P $!"
wait $P
kill $LPID 2>/dev/null
for d in HOST GUEST1 GUEST2 BACK2; do
  echo "===== $d ($OUT/$d/run.err)"
  grep -a "Privateer: \(OPEN\|In room\|linked\|a pilot at\|.* is in the room\|.* left the room\|we decide\|friendly\|.*'s .* is in your space\|pilot [0-9]*'s .* given back\|reports error\)" $OUT/$d/run.err \
    | sed 's/^wcnet: Privateer: //' | cut -c1-150 | uniq -c | head -${LINES_EACH:-30}
done
echo "===== checks"
ok=1
chk() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; ok=0; fi; }
chk "GUEST2 (late) saw GUEST1's ship" "grep -aq 'GUEST1.s .* is in your space' $OUT/GUEST2/run.err"
chk "GUEST2 (late) saw HOST's ship" "grep -aq 'HOST.s .* is in your space' $OUT/GUEST2/run.err"
chk "GUEST1 saw GUEST2's ship" "grep -aq 'GUEST2.s .* is in your space' $OUT/GUEST1/run.err"
chk "GUEST1 heard HOST leave" "grep -aq 'HOST (pilot 1) left the room' $OUT/GUEST1/run.err"
chk "GUEST1 decides after HOST left" "grep -aq 'we decide' $OUT/GUEST1/run.err"
chk "BACK2 saw GUEST1's ship" "grep -aq 'GUEST1.s .* is in your space' $OUT/BACK2/run.err"
chk "GUEST1 saw BACK2's ship" "grep -aq 'BACK2.s .* is in your space' $OUT/GUEST1/run.err"
chk "no game errors" "! grep -aq 'reports error' $OUT/*/run.err"
[ $ok = 1 ] && echo "OPEN ROOM OK" || echo "OPEN ROOM FAILED"
