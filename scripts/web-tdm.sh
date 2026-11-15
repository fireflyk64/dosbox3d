#!/bin/bash
# The TIE Fighter demo flown by two browser pages in a room of a local lobby server (scripts/web-tdm.mjs): the page as
# it is built (web/dist, without wc.tar.gz: the site as deployed), tiedemo.zip dropped into each page as a player drops
# it, the session over the pages' WebRTC data channels.  Ends "TDM PAGE OK".  docs/tiedemo.md.
#
#   scripts/web-tdm.sh [seconds]
#   ZIP= the demo (default ~/dev/tf-demo-dl/tiedemo.zip); PORT, LOBBY_PORT
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-tdm}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-60}
PORT=${PORT:-8857}; LOBBY_PORT=${LOBBY_PORT:-8858}
ZIP=${ZIP:-$HOME/dev/tf-demo-dl/tiedemo.zip}
PW="${PLAYWRIGHT_DIR:-$HOME/dev/beauty}"
LOBBY_BIN=${LOBBY_BIN:-$ROOT/build-web/p2p-lobby-server}
[ -x "$LOBBY_BIN" ] || LOBBY_BIN=$HOME/dev/priv/build-web/p2p-lobby-server
[ -f "$ZIP" ] || { echo "no tiedemo.zip (ZIP)"; exit 2; }
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
ROOM=TFD-$((1000 + RANDOM % 9000))
# Esc past the opening; both fire from a few seconds into the flight (the trigger: the mouse's button)
hs="${HOST_SCRIPT:-8:esc,24:!button=0:1}"
gs="${GUEST_SCRIPT:-8:esc,26:!button=0:1}"
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 300)) node "$ROOT/scripts/web-tdm.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$ZIP" "$hs" "$gs" $DUR 2>&1 | tee $OUT/run.log
