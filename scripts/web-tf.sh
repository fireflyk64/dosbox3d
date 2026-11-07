#!/bin/bash
# TIE Fighter flown by a native host and a browser guest in a room of a local lobby server: the page's WebRTC data
# channels and its glue against the native game's (scripts/web-tf-guest.mjs, scripts/tfnet-duo.sh's route).  Logs,
# reports and screenshots in $OUT (default /tmp/web-tf); then scripts/tfnet-join.py $OUT/host.wall $OUT/guest.wall 1.
#
#   scripts/web-tf.sh [seconds] [the host's extra script] [the guest's extra script]
#   GAME_FILE= GOG's installer (default: the one beside this repository's root); TFDIR= the native host's unpacked game
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/web-tf}; mkdir -p $OUT; rm -f $OUT/*.png $OUT/*.wall
DUR=${1:-150}; HOSTX=${2:-}; GUESTX=${3:-}
PORT=${PORT:-8843}; LOBBY_PORT=${LOBBY_PORT:-8844}
TFDIR=${TFDIR:-$ROOT/tf}
GAME_FILE=${GAME_FILE:-$(ls "$ROOT"/setup_star_wars*tie_fighter*.exe 2>/dev/null | head -1)}
PW="${PLAYWRIGHT_DIR:-$ROOT/build-web/playwright}"
[ -f "$GAME_FILE" ] || { echo "no installer (GAME_FILE)"; exit 2; }
script=""; for t in $(seq 6 2 32); do script="$script${script:+,}$t:space"; done
script="$script,36:!tiecursor=100:105,36.3:!button=0:1,36.6:!button=0:0,38:!tiecursor=280:115,38.3:!button=0:1,38.6:!button=0:0"
script="$script,44:!tiecursor=149:96,44.3:!button=0:1,44.6:!button=0:0,52:!tiecursor=268:148,52.3:!button=0:1,52.6:!button=0:0"
script="$script,62:!tiecursor=242:186,62.3:!button=0:1,62.6:!button=0:0"
# The page's game has no pilot (GOG's TOPACE10.TFR is in __support/save, which the page leaves out as installer junk):
# the guest registers a new one, as a player does, by typing a name at the registration desk and taking the door.
gscript=""; for t in $(seq 6 2 32); do gscript="$gscript${gscript:+,}$t:space"; done
gscript="$gscript,36:w,36.4:e,36.8:b,39:!tiecursor=280:115,39.3:!button=0:1,39.6:!button=0:0"
gscript="$gscript,45:!tiecursor=149:96,45.3:!button=0:1,45.6:!button=0:0,53:!tiecursor=268:148,53.3:!button=0:1,53.6:!button=0:0"
gscript="$gscript,63:!tiecursor=242:186,63.3:!button=0:1,63.6:!button=0:0"
printf '[rooms]\nmax_players_hard = 8\n' > $OUT/lobby.toml
"$ROOT/build-web/p2p-lobby-server" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LOBBY_PORT --allowed-origin "http://localhost:$PORT,http://127.0.0.1:$PORT" \
    --public-url http://127.0.0.1:$LOBBY_PORT --allow-no-origin > $OUT/lobby.log 2>&1 &
LPID=$!
# (the site as deployed: no wc.tar.gz, or a fresh page is ready with Wing Commander before the installer is unpacked)
export WEB_ROOT="$OUT/dist"
rm -rf "$WEB_ROOT" && mkdir -p "$WEB_ROOT"
for f in "$ROOT"/web/dist/*; do [ "$(basename "$f")" = wc.tar.gz ] || ln -s "$f" "$WEB_ROOT/"; done
python3 "$ROOT/web/serve.py" $PORT > $OUT/http.log 2>&1 &
HPID=$!
trap 'kill $LPID $HPID 2>/dev/null' EXIT
sleep 1
ROOM=TF-$((1000 + RANDOM % 9000))
# the native host, its own game's configuration as the rig writes it (tfnet-duo.sh: one copy, the host's)
mkdir -p $OUT/native
sed -n '/^cat > \$OUT\/tf.conf <<CONF$/,/^CONF$/p' "$ROOT/scripts/tfnet-duo.sh" | sed '1d;$d' | sed "s#\$TFDIR#$TFDIR#g" > $OUT/tf.conf
( cd $OUT && env WCROOM=$ROOM WCLOBBY=http://127.0.0.1:$LOBBY_PORT WCLOBBY_ORIGIN= WCPLAYERS=2 WCCALLSIGN=HOST SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    WCNET_LOG=2 WCNET_KEYSCRIPT_FROM=boot WCNET_DUMP_DIR=$OUT WCNET_TIE_REPORT_MS=${WCNET_TIE_REPORT_MS:-250} \
    WCNET_KEYSCRIPT="$script${HOSTX:+,$HOSTX}" timeout -s KILL $DUR ${DOSBOX:-$ROOT/build-native/src/dosbox} -conf $OUT/tf.conf \
    > $OUT/host.out 2> $OUT/host.err ) &
NPID=$!
sleep 3
PLAYWRIGHT_DIR="$PW" SHOT_DIR=$OUT timeout -s KILL $((DUR + 600)) node "$ROOT/scripts/web-tf-guest.mjs" "http://localhost:$PORT/" "http://127.0.0.1:$LOBBY_PORT" \
    $ROOM "$GAME_FILE" "$gscript${GUESTX:+,$GUESTX}" $((DUR - 20)) 2>&1 | tee $OUT/guest-run.log
wait $NPID
grep -a "TIE: wall" $OUT/host.err > $OUT/host.wall
echo "===== host"; grep -a "TIE: \|lobby:" $OUT/host.err | grep -av "TIE: wall\|frames (\|call [0-9]:" | cut -c1-170 | head -16
