#!/bin/bash
# Wing Commander Armada headless, one machine: a copy of the game, NETBIOS.EXE loaded on the emulator's IPX when the
# machine is on a network, then the program; the key script from boot; the log and the pictures in $OUT (default
# /tmp/armadarun).  docs/armada.md.
#
#   scripts/armadarun.sh [seconds] [the key script]
#
#   ARMADADIR=  the game (GOG's installer's app/ directory; default ~/dev/armada/armadagame)
#   PROG=       the program and its arguments (default armada; pg: Proving Grounds)
#   NET=        room: IPX over the room's links (ENV= gives WCPORT, WCHOST: the rig's TCP stand-in for a room)
#               ipxnet=server|<host>:  DOSBox's own IPX over UDP (IPXNET STARTSERVER / CONNECT), for comparison
#               none: no IPX, no NetBIOS (default: room when ENV names a peer, else none)
#   NETBIOS=0   IPX without NETBIOS.EXE (Proving Grounds talks IPX itself)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   CYCLES=     the emulated CPU (default 25000)
#   ENV=        more of the environment ("WCPORT=7400 WCHOST=127.0.0.1 WCCALLSIGN=GUEST")
#   KEEP=1      keep $OUT/game from an earlier run
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/armadarun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-60}; SCRIPT=${2:-}
ARMADADIR=${ARMADADIR:-$ROOT/armadagame}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
[ -f "$ARMADADIR/ARMADA.EXE" ] || { echo "no Armada at $ARMADADIR (GOG's installer: innoextract -I app)"; exit 2; }
if [ -z "${KEEP:-}" ] || [ ! -d $OUT/game ]; then
  rm -rf $OUT/game; mkdir -p $OUT/game; cp -f "$ARMADADIR"/* $OUT/game/ 2>/dev/null
fi
NET=${NET:-}
if [ -z "$NET" ]; then case " ${ENV:-} " in *" WCPORT="*|*" WCHOST="*|*" WCROOM="*) NET=room ;; *) NET=none ;; esac; fi
ipx=false; pre=""
case "$NET" in
  room) ipx=true ;;
  ipxnet=server) ipx=true; pre="ipxnet startserver ${IPXPORT:-213}" ;;
  ipxnet=*) ipx=true; pre="ipxnet connect ${NET#ipxnet=} ${IPXPORT:-213}" ;;
esac
[ "$ipx" = true ] && [ "${NETBIOS:-1}" = 1 ] && pre="$pre
netbios"
cat > $OUT/armada.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=16
[cpu]
core=normal
cycles=fixed ${CYCLES:-25000}
[dos]
xms=true
ems=true
umb=true
[sblaster]
sbtype=sb16
irq=7
dma=1
hdma=5
[ipx]
ipx=$ipx
[autoexec]
@echo off
mount c $OUT/game
c:
$pre
${PROG:-armada}
CONF
by_time() { tr ',' '\n' | grep -v '^$' | sort -t: -k1,1 -g -s | paste -sd, -; }
script="$(echo "$SCRIPT" | by_time)"
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$script" timeout -s KILL $DUR $DOSBOX -conf $OUT/armada.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "IPX" $OUT/run.err $OUT/run.out | cut -c1-220 | tail -${SHOW:-8}
