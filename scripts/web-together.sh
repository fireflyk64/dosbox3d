#!/bin/bash
# The "Free to play now" buttons finding company (scripts/web-together.mjs), pages in rooms of a local lobby server.
#
#   scripts/web-together.sh [button ...]      (default: demoTfd demoRv)
#   env: PORT, LOBBY_PORT, OUT, ARCHIVE_ZIP (default ~/dev/tf-demo-dl/TIE_Fighter_Preview__Dodge_Neon.zip)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-together}; mkdir -p $OUT
PORT=${PORT:-8877}; LOBBY_PORT=${LOBBY_PORT:-8878}
PW="${PLAYWRIGHT_DIR:-$HOME/dev/beauty}"
LOBBY_BIN=${LOBBY_BIN:-$ROOT/build-web/p2p-lobby-server}
[ -x "$LOBBY_BIN" ] || LOBBY_BIN=$HOME/dev/priv/build-web/p2p-lobby-server
printf '[rooms]\nmax_players_hard = 8\n' > $OUT/lobby.toml
"$LOBBY_BIN" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LOBBY_PORT --allowed-origin "http://localhost:$PORT,http://127.0.0.1:$PORT" \
    --public-url http://127.0.0.1:$LOBBY_PORT --allow-no-origin > $OUT/lobby.log 2>&1 &
LPID=$!
export WEB_ROOT="$OUT/dist"
rm -rf "$WEB_ROOT" && mkdir -p "$WEB_ROOT"
for f in "$ROOT"/web/dist/*; do [ "$(basename "$f")" = wc.tar.gz ] || ln -s "$f" "$WEB_ROOT/"; done
python3 "$ROOT/web/serve.py" $PORT > $OUT/http.log 2>&1 &
HPID=$!
trap 'kill $LPID $HPID 2>/dev/null' EXIT
sleep 1
ARCHIVE_ZIP=${ARCHIVE_ZIP:-$HOME/dev/tf-demo-dl/TIE_Fighter_Preview__Dodge_Neon.zip} PLAYWRIGHT_DIR="$PW" \
    timeout -s KILL 600 node "$ROOT/scripts/web-together.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" "$@" 2>&1 | tee $OUT/run.log
