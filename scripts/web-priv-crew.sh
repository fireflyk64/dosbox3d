#!/bin/bash
# Privateer's crew in two browser pages (scripts/web-priv-crew.mjs): web/dist served on PORT, a local lobby on LOBBY_PORT.
#   scripts/web-priv-crew.sh       SAVE= (default privsaves/OXFORD01.SAV)  PORT= LOBBY_PORT= (8857, 8858)  OUT=/tmp/web-priv-crew
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-priv-crew}; mkdir -p "$OUT"
PORT=${PORT:-8857}; LOBBY_PORT=${LOBBY_PORT:-8858}
export SAVE=${SAVE:-$ROOT/privsaves/OXFORD01.SAV}
GAME_FILE=${GAME_FILE:-$(ls "$ROOT"/setup_wing_commander_privateer*.exe /home/sandbox/dev/dosbox3d/setup_wing_commander_privateer*.exe 2>/dev/null | head -1)}
PW="${PLAYWRIGHT_DIR:-/home/sandbox/dev/beauty}"
LOBBYSERVER=${LOBBYSERVER:-$ROOT/build-web/p2p-lobby-server}
printf '[rooms]\nmax_players_hard = 8\n' > "$OUT/lobby.toml"
"$LOBBYSERVER" --config "$OUT/lobby.toml" --listen-http 127.0.0.1:$LOBBY_PORT --allowed-origin "http://localhost:$PORT,http://127.0.0.1:$PORT" \
    --public-url http://127.0.0.1:$LOBBY_PORT --allow-no-origin > "$OUT/lobby.log" 2>&1 &
LPID=$!
# (the site as deployed: no wc.tar.gz; the links made anew over whatever an earlier run left)
export WEB_ROOT="$OUT/dist"
mkdir -p "$WEB_ROOT"
for f in "$ROOT"/web/dist/*; do [ "$(basename "$f")" = wc.tar.gz ] || ln -sfn "$f" "$WEB_ROOT/"; done
python3 "$ROOT/web/serve.py" $PORT > "$OUT/http.log" 2>&1 &
HPID=$!
trap 'kill $LPID $HPID 2>/dev/null' EXIT
sleep 1
MJS="$(dirname "$0")/web-priv-crew.mjs"
PLAYWRIGHT_DIR="$PW" SHOT_DIR="$OUT" timeout -s KILL 1500 node "$MJS" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" "$GAME_FILE" 2>&1 | tee "$OUT/run.log"
