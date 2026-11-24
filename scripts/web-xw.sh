#!/bin/bash
# X-Wing Collector's CD flown by two browser pages in a room of a local lobby server (scripts/web-xw.mjs): the page as
# it is built (web/dist, without wc.tar.gz: the site as deployed), the CD's image dropped into each page as a player
# drops it, the session over the pages' WebRTC data channels.  Ends "XW PAGE OK".  docs/xwing.md.
#
#   scripts/web-xw.sh [seconds]
#   GAME= the game (default ~/dev/dosbox3d/xwingcd/xwing.iso); PORT, LOBBY_PORT; HOST_SCRIPT, GUEST_SCRIPT: each
#   page's keys after the route to the flight (default: both fire from 130 s)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-xw}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-200}
PORT=${PORT:-8873}; LOBBY_PORT=${LOBBY_PORT:-8874}
ZIP=${GAME:-$HOME/dev/dosbox3d/xwingcd/xwing.iso}
PW="${PLAYWRIGHT_DIR:-$HOME/dev/beauty}"
LOBBY_BIN=${LOBBY_BIN:-$ROOT/build-web/p2p-lobby-server}
[ -x "$LOBBY_BIN" ] || LOBBY_BIN=$HOME/dev/priv/build-web/p2p-lobby-server
[ -f "$ZIP" ] || { echo "no game (GAME: the CD's image, or a zip)"; exit 2; }
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
ROOM=XW-$((1000 + RANDOM % 9000))
hs="${HOST_SCRIPT:-130:!button=0:1,150:!button=0:0}"
gs="${GUEST_SCRIPT:-132:!button=0:1,152:!button=0:0}"
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 300)) ${REAP:-} node "$ROOT/scripts/web-xw.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$ZIP" "$hs" "$gs" $DUR 2>&1 | tee $OUT/run.log
