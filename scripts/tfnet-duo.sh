#!/bin/bash
# Two TIE Fighters flown together headless, a host and a guest, both scripted through the front end into the Combat
# Chamber's first mission (docs/tiefighter.md, section 4's route), then each one's extra key script; the logs, the
# wall-clock reports and the screenshots land in $OUT (default /tmp/tfnet-duo).
#
#   scripts/tfnet-duo.sh [seconds] [the host's extra script] [the guest's extra script]
#
#   LOBBY=1      a lobby room (WebRTC data channels, as the browser's; a local lobby server on LOBBY_PORT, 8841), else TCP
#   TFDIR=       the game as innoextract unpacks GOG's installer (default: tf/ beside this repository's root)
#   DOSBOX=      the emulator (default build-native/src/dosbox)
#   WCNET_TIE_REPORT_MS=250   the reports' period (scripts/tfnet-join.py lines them up)
#   GUEST_LAG=2  seconds between the host's start and the guest's
#
# Example, the guest at full throttle turning past the host:
#   t="66:backspace"; for s in 67 68 69 70; do for d in 0 1 2 3 4 5 6 7 8 9; do t="$t,$s.$d:!mickey=40:-12"; done; done
#   WCNET_TIE_REPORT_MS=250 scripts/tfnet-duo.sh 92 "" "$t"
#   scripts/tfnet-join.py /tmp/tfnet-duo/host.wall /tmp/tfnet-duo/guest.wall 1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/tfnet-duo}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.png
DUR=${1:-100}; HOSTX=${2:-}; GUESTX=${3:-}
PORT=${PORT:-13455}
TFDIR=${TFDIR:-$ROOT/tf}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
[ -f "$TFDIR/game.gog" ] || { echo "no TIE Fighter at $TFDIR (innoextract GOG's installer there)"; exit 2; }
# (a pilot for the registration desk, GOG's own each run: both games share the file, and the Combat Chamber's last
# mission is kept in it, so a run's clicks would move the next run's start)
cp "$TFDIR/__support/save/TOPACE10.TFR" "$TFDIR/TOPACE10.TFR"
# GOG's own machine (its __support/app/dosbox_tiecd.conf), the disc as drive D, no exit after the game
cat > $OUT/tf.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=16
[cpu]
core=normal
cycles=fixed 60000
[sblaster]
sbtype=sb16
irq=7
dma=1
hdma=5
[dos]
xms=true
ems=true
umb=true
[autoexec]
@echo off
mount c $TFDIR
imgmount d $TFDIR/game.gog -t iso
c:
tie.exe
CONF
# Space through the opening, TOPACE10 at the registration desk, the door, the Combat Chamber, its briefing, its mission
script=""; for t in $(seq 6 2 32); do script="$script${script:+,}$t:space"; done
script="$script,36:!tiecursor=100:105,36.3:!button=0:1,36.6:!button=0:0,38:!tiecursor=280:115,38.3:!button=0:1,38.6:!button=0:0"
script="$script,44:!tiecursor=149:96,44.3:!button=0:1,44.6:!button=0:0,52:!tiecursor=268:148,52.3:!button=0:1,52.6:!button=0:0"
script="$script,62:!tiecursor=242:186,62.3:!button=0:1,62.6:!button=0:0"
# (the key script runs its steps in their order: a route and its extras are merged by time)
by_time() { tr ',' '\n' | sort -t: -k1,1 -g -s | paste -sd, -; }
PIDS=""
run() {  # name, extra env...
  local name=$1; shift
  ( cd $OUT && env "$@" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot WCNET_DUMP_DIR=$OUT \
      timeout -s KILL $DUR $DOSBOX -conf $OUT/tf.conf > $OUT/$name.out 2> $OUT/$name.err ) &
  PIDS="$PIDS $!"
}
if [ -n "${LOBBY:-}" ]; then
  LP=${LOBBY_PORT:-8841}; ROOM=TF-$RANDOM
  LOBBYSERVER="$ROOT/build-web/p2p-lobby-server"
  [ -x "$LOBBYSERVER" ] || (cd "$ROOT/lobbylink" && CGO_ENABLED=0 go build -trimpath -o "$LOBBYSERVER" ./cmd/p2p-lobby-server) || exit 1
  printf '[rooms]\nmax_players_hard = 8\n' > $OUT/lobby.toml
  "$LOBBYSERVER" --config $OUT/lobby.toml --listen-http 127.0.0.1:$LP --public-url http://127.0.0.1:$LP --allow-no-origin > $OUT/lobby.log 2>&1 &
  LPID=$!; sleep 1
  NET_HOST="WCROOM=$ROOM WCLOBBY=http://127.0.0.1:$LP WCLOBBY_ORIGIN= WCPLAYERS=2"; NET_GUEST="$NET_HOST"
else
  NET_HOST="WCPORT=$PORT"; NET_GUEST="WCHOST=127.0.0.1 WCPORT=$PORT"
fi
run host $NET_HOST WCCALLSIGN=HOST WCNET_KEYSCRIPT="$(echo "$script${HOSTX:+,$HOSTX}" | by_time)"
sleep ${GUEST_LAG:-2}   # (GUEST_LAG=30: the guest still on the ground while the host flies)
run guest $NET_GUEST WCCALLSIGN=GUEST WCNET_KEYSCRIPT="$(echo "$script${GUESTX:+,$GUESTX}" | by_time)"
wait $PIDS   # (the games only: a lobby server never ends)
[ -n "${LPID:-}" ] && kill $LPID 2>/dev/null
for n in host guest; do grep -a "TIE: wall" $OUT/$n.err > $OUT/$n.wall; done
for n in host guest; do
  echo "===== $n ($OUT/$n.err)"
  grep -a "TIE: \|TIE Fighter\|lobby:" $OUT/$n.err | grep -av "TIE: wall\|frames (\|call [0-9]:" | cut -c1-180 | head -20
done
