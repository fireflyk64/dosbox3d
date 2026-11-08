#!/bin/bash
# Privateer flown together headless (docs/privateer.md): a host and guests, each scripts/privrun.sh's route into
# flight from Achilles plus its own extra key script; their logs in $OUT/<name>/ (default /tmp/privnet).
#
#   scripts/privnet.sh [seconds] [the host's extra script] [every guest's extra script]
#
#   PLAYERS=2..8    the room's seats, all of them flown (guest1.. guest7)
#   LOBBY=1         a local lobby room (WebRTC data channels, as the browser's; LOBBY_PORT, 8851), else TCP (PORT, 13471)
#   GUEST_LAG=2     seconds between the starts
#   GUEST2X=..      a script of its own for guest 2 (GUEST3X ... too)
# Example, the guest engaging the autopilot (A) after the host's presence:
#   scripts/privnet.sh 60 "" "40:a"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/privnet}; mkdir -p $OUT
DUR=${1:-50}; HOSTX=${2:-}; GUESTX=${3:-}; PLAYERS=${PLAYERS:-2}
PORT=${PORT:-13471}
if [ -n "${LOBBY:-}" ]; then
  LP=${LOBBY_PORT:-8851}; ROOM=PRV-$RANDOM
  LOBBYSERVER="$ROOT/build-web/p2p-lobby-server"
  [ -x "$LOBBYSERVER" ] || (cd "$ROOT/lobbylink" && CGO_ENABLED=0 go build -trimpath -o "$LOBBYSERVER" ./cmd/p2p-lobby-server) || exit 1
  printf '[rooms]\nmax_players_hard = 8\n' > $OUT/lobby.toml
  "$LOBBYSERVER" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LP --public-url http://127.0.0.1:$LP --allow-no-origin > $OUT/lobby.log 2>&1 &
  LPID=$!; sleep 1
  NET_HOST="WCROOM=$ROOM WCLOBBY=http://127.0.0.1:$LP WCLOBBY_ORIGIN= WCPLAYERS=$PLAYERS"; NET_GUEST="$NET_HOST"
else
  NET_HOST="WCPORT=$PORT WCPLAYERS=$PLAYERS"; NET_GUEST="WCHOST=127.0.0.1 WCPORT=$PORT"
fi
PIDS=""
OUT=$OUT/host ENV="$NET_HOST WCCALLSIGN=HOST" "$ROOT/scripts/privrun.sh" $DUR "$HOSTX" > /dev/null &
PIDS="$PIDS $!"
for g in $(seq 1 $((PLAYERS - 1))); do
  sleep ${GUEST_LAG:-2}
  var="GUEST${g}X"; X="${!var:-$GUESTX}"
  OUT=$OUT/guest$g ENV="$NET_GUEST WCCALLSIGN=GUEST$g" "$ROOT/scripts/privrun.sh" $DUR "$X" > /dev/null &
  PIDS="$PIDS $!"
done
wait $PIDS
[ -n "${LPID:-}" ] && kill $LPID 2>/dev/null
for d in host $(seq -f guest%g 1 $((PLAYERS - 1))); do
  echo "===== $d ($OUT/$d/run.err)"
  grep -a "Privateer: \|lobby:" $OUT/$d/run.err | grep -av "keyboard frames\|arrows [+-]" | cut -c1-170 | head -${LINES_EACH:-14}
done
