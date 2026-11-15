#!/bin/bash
# Raptor flown by two browser pages in a room of a local lobby server (scripts/web-rap.mjs): the page as it
# is built (web/dist, without wc.tar.gz: the site as deployed), the shareware's zip dropped into each page, the page's
# settings, the session over the pages' WebRTC data channels.  Ends "RAP PAGE OK".  docs/raptor.md.
#
#   scripts/web-rap.sh [seconds]
#   ZIP= the shareware (default ~/dev/rap-dl/1rap12.zip); PORT, LOBBY_PORT; ROUTE= the key script into the wave
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-rap}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-150}
PORT=${PORT:-8853}; LOBBY_PORT=${LOBBY_PORT:-8854}
ZIP=${ZIP:-$HOME/dev/rap-dl/1rap12.zip}
PW="${PLAYWRIGHT_DIR:-$HOME/dev/beauty}"
LOBBY_BIN=${LOBBY_BIN:-$ROOT/build-web/p2p-lobby-server}
[ -x "$LOBBY_BIN" ] || LOBBY_BIN=$HOME/dev/priv/build-web/p2p-lobby-server
[ -f "$ZIP" ] || { echo "no shareware zip (ZIP)"; exit 2; }
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
ROOM=RAP-$((1000 + RANDOM % 9000))
# the route of scripts/rapnet-duo.sh: New Mission, the name and call sign, the difficulty as offered, the hangar's Fly
# Mission, the Bravo sector; then each pilot's flying (the energy kept full, both firing, the guest to the right)
route="${ROUTE:-41:enter,45:a,45.3:c,45.6:e,46:enter,49:a,49.3:c,49.6:e,50:enter,53:enter,57:down,59:enter,63:enter}"
hs="$route,70:!rapgod=1,72:+ctrl"
gs="$route,70:!rapgod=1,73:right*0.6,74:+ctrl"
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 300)) node "$ROOT/scripts/web-rap.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$ZIP" "$hs" "$gs" $DUR 2>&1 | tee $OUT/run.log
