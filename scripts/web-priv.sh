#!/bin/bash
# Privateer in the browser (scripts/web-priv.mjs): the page of web/dist served on PORT, a local lobby server on
# LOBBY_PORT, GOG's installer dropped into a headless Chrome page, flown alone.  Logs and pictures in $OUT
# (default /tmp/web-priv); "PRIV OK" at the end when every check passed.
#
#   scripts/web-priv.sh [seconds]
#   GAME_FILE=  GOG's installer (default: the one beside this repository's root, or the main tree's)
#   PORT= LOBBY_PORT=  (default 8853, 8854)   RF=1  Righteous Fire
#   ROOM=1      a native host (build-lobby's emulator, scripts/privrun.sh's route) makes a room of 8 in the local lobby
#               and the page joins it as a guest: presence both ways (the host's log: $OUT/host/run.err)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-priv}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-80}
PORT=${PORT:-8853}; LOBBY_PORT=${LOBBY_PORT:-8854}
GAME_FILE=${GAME_FILE:-$(ls "$ROOT"/setup_wing_commander_privateer*.exe /home/sandbox/dev/dosbox3d/setup_wing_commander_privateer*.exe 2>/dev/null | head -1)}
PW="${PLAYWRIGHT_DIR:-/home/sandbox/dev/beauty}"
LOBBYSERVER=${LOBBYSERVER:-$ROOT/build-web/p2p-lobby-server}
[ -f "$GAME_FILE" ] || { echo "no installer (GAME_FILE)"; exit 2; }
[ -x "$LOBBYSERVER" ] || (cd "$ROOT/lobbylink" && CGO_ENABLED=0 go build -trimpath -o "$LOBBYSERVER" ./cmd/p2p-lobby-server) || exit 1
printf '[rooms]\nmax_players_hard = 8\n' > $OUT/lobby.toml
"$LOBBYSERVER" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LOBBY_PORT --allowed-origin "http://localhost:$PORT,http://127.0.0.1:$PORT" \
    --public-url http://127.0.0.1:$LOBBY_PORT --allow-no-origin > $OUT/lobby.log 2>&1 &
LPID=$!
# (the site as deployed: no wc.tar.gz)
export WEB_ROOT="$OUT/dist"
rm -rf "$WEB_ROOT" && mkdir -p "$WEB_ROOT"
for f in "$ROOT"/web/dist/*; do [ "$(basename "$f")" = wc.tar.gz ] || ln -s "$f" "$WEB_ROOT/"; done
python3 "$ROOT/web/serve.py" $PORT > $OUT/http.log 2>&1 &
HPID=$!
trap 'kill $LPID $HPID 2>/dev/null' EXIT
sleep 1
if [ -n "${ROOM:-}" ]; then
  ROOM=PRV-$((1000 + RANDOM % 9000)); export ROOM
  OUT=$OUT/host DOSBOX=${DOSBOX:-$ROOT/build-lobby/src/dosbox} ENV="WCROOM=$ROOM WCLOBBY=http://127.0.0.1:$LOBBY_PORT WCLOBBY_ORIGIN= WCPLAYERS=8 WCCALLSIGN=HOST" \
    "$ROOT/scripts/privrun.sh" $((DUR + 60)) > /dev/null &
  NPID=$!
  sleep 4
fi
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 700)) node "$ROOT/scripts/web-priv.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    "$GAME_FILE" $DUR 2>&1 | tee $OUT/run.log
if [ -n "${NPID:-}" ]; then
  kill $NPID 2>/dev/null; sleep 1
  echo "===== the native host"
  grep -a "Privateer: PRIVTEST\|Privateer: [A-Z]* joined\|In Troy with you" $OUT/host/run.err | cut -c1-150 | head -8
  grep -a -q "Privateer: PRIVTEST on autopilot" $OUT/host/run.err && echo "ok   the host told of the page's autopilot" || echo "FAIL the host never heard of the page's autopilot"
fi
