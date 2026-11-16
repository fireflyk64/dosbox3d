#!/bin/bash
# DOOM played by two (or more) browser pages in a room of a local lobby server (scripts/web-doom.mjs): the page as it
# is built (web/dist, without wc.tar.gz: the site as deployed), the shareware's zip dropped into each page (or the
# page's own button, DEMO=1), the page's settings, the game's packets over the pages' WebRTC data channels.  Ends
# "DOOM PAGE OK".  docs/doom.md.
#
#   scripts/web-doom.sh [seconds]
#   ZIP= the shareware (default ~/dev/doom-dl/doom19s.zip); PORT, LOBBY_PORT; PLAYERS= 2 (default) to 4;
#   HOST_KEYS= / GUEST_KEYS= each page's key script (from boot); MISSION= the host's pick ("4/2": Ultra-Violence, E1M3)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-doom}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-120}
PORT=${PORT:-8863}; LOBBY_PORT=${LOBBY_PORT:-8864}
ZIP=${ZIP:-$HOME/dev/doom-dl/doom19s.zip}
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
ROOM=DOOM-$((1000 + RANDOM % 9000))
# the host steps forward and turns about to face the others (a held left turns half round in 1.6 s); nobody fires:
# in DOOM's co-operative game a marine's bullets hurt the others
hs="${HOST_KEYS-45:+up,45.5:-up,45.7:+left,47.3:-left}"
gs="${GUEST_KEYS-}"
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 300)) node "$ROOT/scripts/web-doom.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$ZIP" "$hs" "$gs" $DUR 2>&1 | tee $OUT/run.log
