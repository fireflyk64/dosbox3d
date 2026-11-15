#!/bin/bash
# Terminal Velocity flown by two browser pages in a room of a local lobby server (scripts/web-tv.mjs): the page as it
# is built (web/dist, without wc.tar.gz: the site as deployed), the shareware's zip dropped into each page, the host's
# craft menus, the page's settings, the session over the pages' WebRTC data channels.  Ends "TV PAGE OK".
#
#   scripts/web-tv.sh [seconds]
#   ZIP= the shareware (default ~/dev/tv-dl/3dtv12.zip), or the registered game's installer as GOG sells it
#   (setup_terminal_velocity_1.2_*.exe: unpacked by the page); GUEST_ZIP= the guest's own (the shareware beside the
#   host's registered game); PORT, LOBBY_PORT; HOST_CRAFT / GUEST_CRAFT
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-tv}; mkdir -p $OUT; rm -f $OUT/*.png
DUR=${1:-200}
PORT=${PORT:-8853}; LOBBY_PORT=${LOBBY_PORT:-8854}
ZIP=${ZIP:-$HOME/dev/tv-dl/3dtv12.zip}
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
ROOM=TV-$((1000 + RANDOM % 9000))
# the route of scripts/tvrun.sh (briefings on in the page's settings: a slide show between the story and the flight,
# Enter through it), then each pilot's flying
route="38:enter,42:enter,46:enter,50:enter,52:a,52.3:c,52.6:e,53:enter,55:a,55.3:c,55.6:e,56:enter,61:enter"
# (the briefing's slides at the page's best detail take longer: Enter through 148 s, flying from 155 s)
for t in $(seq 64 4 148); do route="$route,$t:enter"; done
# (HOST_FLY= / GUEST_FLY=: each pilot's flying, in the default's place: "215:!tvfly=partner:0:1:8" a guest kept behind
# the host for a picture of it)
hs="$route,${HOST_FLY:-155:!tvgod=1,156:!tvfly=obj:408,156.5:!tvfire=1}"
gs="$route,${GUEST_FLY:-155:!tvgod=1,156:!tvfly=partner:3:1:8,170:!tvfly=obj:408,170.5:!tvfire=1}"
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 300)) node "$ROOT/scripts/web-tv.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$ZIP" "$hs" "$gs" $DUR 2>&1 | tee $OUT/run.log
