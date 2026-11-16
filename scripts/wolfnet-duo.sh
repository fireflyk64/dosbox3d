#!/bin/bash
# Wolfenstein 3-D, two or more machines over TCP on this one (each its own copy of the game), played together: the
# host and its guests, each with its own key script.  The logs in $OUT/host and $OUT/guest (guest2, guest3: run.err),
# pictures there too.  docs/wolf3d.md.
#
#   scripts/wolfnet-duo.sh [seconds] [the host's key script] [the guest's] [the second guest's] [the third's]
#
#   PLAYERS=     2 (default), 3 or 4
#   LEVEL=       the host's TEDLEVEL number, 10 * episode + floor, from 0 (default 0); SKILL= baby, easy, normal, hard
#   GUEST_LEVEL= the guests' own (default 0: the room's is taken anyway)
#   PORT=        the TCP port (default 7493)
#   GUEST_LATE=  seconds the guests start after the host (default 2)
#   CYCLES=      (default 20000)
#   WOLFDIR=     the game all play; HOST_WOLFDIR= / GUEST_WOLFDIR= each one's
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/wolfduo}; PLAYERS=${PLAYERS:-2}
DUR=${1:-60}; HX=${2:-}
GX=("${3:-}" "${4:-}" "${5:-}")
PORT=${PORT:-7493}
mkdir -p $OUT/host
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
WOLFDIR=${HOST_WOLFDIR:-${WOLFDIR:-}} OUT=$OUT/host DOSBOX=$BIN CYCLES=${CYCLES:-20000} PROG="wolf3d tedlevel ${LEVEL:-0} ${SKILL:-normal} nowait" SHOW=0 \
    ENV="WCPORT=$PORT WCPLAYERS=$PLAYERS WCCALLSIGN=HOST ${HOSTENV:-}" $ROOT/scripts/wolfrun.sh $DUR "$HX" &
PIDS=$!
sleep ${GUEST_LATE:-2}
NAMES=(guest guest2 guest3)
for ((g = 0; g < PLAYERS - 1; g++)); do
  w=${NAMES[$g]}; mkdir -p $OUT/$w
  WOLFDIR=${GUEST_WOLFDIR:-${WOLFDIR:-}} OUT=$OUT/$w DOSBOX=$BIN CYCLES=${CYCLES:-20000} PROG="wolf3d tedlevel ${GUEST_LEVEL:-0} ${GUEST_SKILL:-normal} nowait" SHOW=0 \
      ENV="WCHOST=127.0.0.1 WCPORT=$PORT WCCALLSIGN=${w^^} ${GUESTENV:-}" $ROOT/scripts/wolfrun.sh $DUR "${GX[$g]}" &
  PIDS="$PIDS $!"
done
wait $PIDS
for w in host ${NAMES[@]:0:$((PLAYERS - 1))}; do
  echo "== $w"
  grep -a "Wolfenstein" $OUT/$w/run.err | grep -v "Wolfenstein 3-D: [0-9.]*s: " | cut -c1-${WIDE:-260} | tail -${SHOW:-16}
  grep -a "Wolfenstein 3-D: [0-9.]*s: " $OUT/$w/run.err | tail -1 | cut -c1-260
done
