#!/bin/bash
# Wolfenstein 3-D played by two (or more) browser pages in a room of a local lobby server (scripts/web-wolf.mjs): the
# page as it is built (web/dist, without wc.tar.gz: the site as deployed), the game dropped into each page (or the
# page's own button, DEMO=1), every page's game in the host's floor over the pages' WebRTC data channels.  Ends
# "WOLF PAGE OK".  docs/wolf3d.md.
#
#   scripts/web-wolf.sh [seconds]
#   ZIP= the game (default the shareware, ~/dev/wolf-dl/1wolf14.zip; GOG's installer of the full game works too);
#   PORT, LOBBY_PORT; PLAYERS= 2 (default) to 4; HOST_KEYS= / GUEST_KEYS= each page's key script (from boot);
#   MISSION= the host's pick ("2/3": I am Death incarnate, floor 4); LATE=, DEMO=1, FALLEN=: scripts/web-wolf.mjs
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-wolf}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-120}
PORT=${PORT:-8863}; LOBBY_PORT=${LOBBY_PORT:-8864}
ZIP=${ZIP:-$HOME/dev/wolf-dl/1wolf14.zip}
PW="${PLAYWRIGHT_DIR:-$HOME/dev/beauty}"
LOBBY_BIN=${LOBBY_BIN:-$ROOT/build-web/p2p-lobby-server}
[ -x "$LOBBY_BIN" ] || LOBBY_BIN=$HOME/dev/priv/build-web/p2p-lobby-server
[ -f "$ZIP" ] || { echo "no shareware zip (ZIP)"; exit 2; }
for p in $PORT $LOBBY_PORT; do (echo > /dev/tcp/127.0.0.1/$p) 2>/dev/null && { echo "port $p is taken (PORT, LOBBY_PORT)"; exit 2; }; done
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
ROOM=WOLF-$((1000 + RANDOM % 9000))
# the host is put three tiles ahead of the start facing back at it: the players see each other
hs="${HOST_KEYS-40:!wolfwarp=32.5:57.5:180}"
gs="${GUEST_KEYS-}"
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 300)) node "$ROOT/scripts/web-wolf.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$ZIP" "$hs" "$gs" $DUR 2>&1 | tee $OUT/run.log
