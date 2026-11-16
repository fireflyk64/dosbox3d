#!/bin/bash
# DOOM, two or more machines over TCP on this one (each its own copy of the game), run by the layer's launcher as one
# co-operative network game: the host and its guests, each with its own key script.  The logs in $OUT/host and
# $OUT/guest (guest2, guest3: run.err), pictures there too.  docs/doom.md.
#
#   scripts/doomnet-duo.sh [seconds] [the host's key script] [the guest's] [the second guest's] [the third's]
#
#   PLAYERS=     2 (default), 3 or 4
#   ARGS=        the host's arguments to the game (default "-skill 3 -warp 1 1")
#   PORT=        the TCP port (default 7491)
#   GUEST_LATE=  seconds the guests start after the host (default 2)
#   CYCLES=      (default 50000)
#   DOOMDIR=     the game all play; HOST_DOOMDIR= / GUEST_DOOMDIR= each one's
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/doomduo}; PLAYERS=${PLAYERS:-2}
DUR=${1:-90}; HX=${2:-}
GX=("${3:-}" "${4:-}" "${5:-}")
PORT=${PORT:-7491}
mkdir -p $OUT/host
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
ARGS=${ARGS--skill 3 -warp 1 1}
DOOMDIR=${HOST_DOOMDIR:-${DOOMDIR:-}} OUT=$OUT/host DOSBOX=$BIN CYCLES=${CYCLES:-50000} PROG="doomnet $ARGS" SHOW=0 \
    ENV="WCPORT=$PORT WCPLAYERS=$PLAYERS WCCALLSIGN=HOST ${HOSTENV:-}" $ROOT/scripts/doomrun.sh $DUR "$HX" &
PIDS=$!
sleep ${GUEST_LATE:-2}
NAMES=(guest guest2 guest3)
for ((g = 0; g < PLAYERS - 1; g++)); do
  w=${NAMES[$g]}; mkdir -p $OUT/$w
  DOOMDIR=${GUEST_DOOMDIR:-${DOOMDIR:-}} OUT=$OUT/$w DOSBOX=$BIN CYCLES=${CYCLES:-50000} PROG="doomnet ${GUEST_ARGS:-}" SHOW=0 \
      ENV="WCHOST=127.0.0.1 WCPORT=$PORT WCCALLSIGN=${w^^} ${GUESTENV:-}" $ROOT/scripts/doomrun.sh $DUR "${GX[$g]}" &
  PIDS="$PIDS $!"
done
wait $PIDS
for w in host ${NAMES[@]:0:$((PLAYERS - 1))}; do
  echo "== $w"
  grep -a "DOOM" $OUT/$w/run.err | grep -v "DOOM: [0-9.]*s: " | cut -c1-200 | tail -14
  grep -a "DOOM: [0-9.]*s: " $OUT/$w/run.err | tail -2 | cut -c1-240
done
