#!/bin/bash
# webrig.sh TAG [PARAMS]: a browser flight of the worktree's dist, on its own ports, COMBAT=1.  Each TAG is a fresh directory.
ROOT=/home/sandbox/dev/rv-gx; S=${RV_RELEASE_DIR:-/tmp/rv-release}; mkdir -p $S
TAG=$1; PARAMS_=$2; OUT=$S/web-$TAG; mkdir -p $OUT
PORT=${PORT:-8791}; LOBBY_PORT=${LOBBY_PORT:-8792}
export PATH="$PATH:/usr/local/go/bin"
LOBBY=$ROOT/build-web/p2p-lobby-server
[ -x "$LOBBY" ] || (cd $ROOT/lobbylink && CGO_ENABLED=0 go build -trimpath -o "$LOBBY" ./cmd/p2p-lobby-server) || exit 1
printf '[rooms]\nmax_players_hard = 256\n' > $OUT/lobby.toml
"$LOBBY" --config $OUT/lobby.toml --listen-http "127.0.0.1:$LOBBY_PORT" --allowed-origin "http://localhost:$PORT,http://127.0.0.1:$PORT" --public-url "http://127.0.0.1:$LOBBY_PORT" --allow-no-origin > $OUT/lobby.log 2>&1 &
LP=$!
export WEB_ROOT=$OUT/dist; mkdir -p $WEB_ROOT
for f in $ROOT/web/dist/*; do [ "$(basename $f)" = wc.tar.gz ] || ln -sf $f $WEB_ROOT/; done
python3 $ROOT/web/serve.py $PORT > $OUT/http.log 2>&1 &
HP=$!
trap 'kill $LP $HP 2>/dev/null' EXIT
sleep 1
cd $OUT
PLAYWRIGHT_DIR=/home/sandbox/dev/beauty SHOT_DIR=$OUT COMBAT=${COMBAT-1} FLIGHT_S=${FLIGHT_S:-20} PARAMS="$PARAMS_" node $ROOT/scripts/web-rv-flight.mjs "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" > $OUT/out.txt 2>&1
echo "rc $?" >> $OUT/out.txt
