#!/bin/bash
# Wing Commander II in the browser build, end to end, in two headless Chrome
# pages against a local lobby server (see scripts/web-wc2.mjs).  Needs
# web/dist from scripts/build-web.sh and the game as a .zip of its folder
# (or the GOG installer .exe):
#
#   GAME_FILE=/path/to/wc2.zip scripts/web-wc2.sh [series/mission] [drone|gunner|wingman] [door x,y]
#
# Logs and screenshots land in $OUT (default /tmp/web-wc2).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-/tmp/web-wc2}"
PORT="${PORT:-8773}"
LOBBY_PORT="${LOBBY_PORT:-8793}"
[ -n "${GAME_FILE:-}" ] || { echo "GAME_FILE (a .zip of the Wing Commander II folder) is required"; exit 2; }
mkdir -p "$OUT"
export PATH="$PATH:/usr/local/go/bin"

LOBBY="$ROOT/build-web/p2p-lobby-server"
if [ ! -x "$LOBBY" ]; then
    echo "building the lobby server"
    (cd "$ROOT/lobbylink" && CGO_ENABLED=0 go build -trimpath -o "$LOBBY" ./cmd/p2p-lobby-server) || exit 1
fi
PW="${PLAYWRIGHT_DIR:-$ROOT/build-web/playwright}"
if [ ! -d "$PW/node_modules/playwright" ]; then
    echo "installing playwright into $PW"
    mkdir -p "$PW" && (cd "$PW" && npm init -y >/dev/null && npm i --silent playwright) || exit 1
fi

"$LOBBY" --listen-http "127.0.0.1:$LOBBY_PORT" --allowed-origin "http://localhost:$PORT,http://127.0.0.1:$PORT" \
    --public-url "http://127.0.0.1:$LOBBY_PORT" --allow-no-origin > "$OUT/lobby.log" 2>&1 &
LOBBY_PID=$!
python3 "$ROOT/web/serve.py" "$PORT" > "$OUT/http.log" 2>&1 &
HTTP_PID=$!
trap 'kill $LOBBY_PID $HTTP_PID 2>/dev/null' EXIT
sleep 1
PLAYWRIGHT_DIR="$PW" SHOT_DIR="$OUT" GAME_FILE="$GAME_FILE" node "$ROOT/scripts/web-wc2.mjs" \
    "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" "$@" 2>&1 | tee "$OUT/run.log"
exit "${PIPESTATUS[0]}"
