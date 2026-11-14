#!/bin/bash
# Terminal Velocity, two machines over TCP on this one (each its own copy of the game): the host and a guest both
# routed into the first level (scripts/tvrun.sh), each with its own extra key script.  The logs in $OUT/host and
# $OUT/guest (run.err), pictures there too.  docs/terminalvelocity.md section 4.
#
#   scripts/tvnet-duo.sh [seconds] [the host's extra key script] [the guest's]
#
#   PORT=        the TCP port (default 7471)
#   HOSTENV= / GUESTENV=   more of each one's environment (WCTVLOOK=icebrd.bin ...)
#   GUEST_LATE=  seconds the guest starts after the host (default 2)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/tvduo}; mkdir -p $OUT/host $OUT/guest
DUR=${1:-110}; HX=${2:-}; GX=${3:-}
PORT=${PORT:-7471}
# (one binary for both, copied: a rebuild meanwhile does not hit a running test)
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
TOP=$OUT
OUT=$TOP/host COPY=1 DOSBOX=$BIN ENV="WCPORT=$PORT WCCALLSIGN=HOST ${HOSTENV:-}" SHOW=0 $ROOT/scripts/tvrun.sh $DUR "$HX" &
HPID=$!
sleep ${GUEST_LATE:-2}
OUT=$TOP/guest COPY=1 DOSBOX=$BIN ENV="WCHOST=127.0.0.1 WCPORT=$PORT WCCALLSIGN=GUEST ${GUESTENV:-}" SHOW=0 $ROOT/scripts/tvrun.sh $DUR "$GX" &
GPID=$!
wait $HPID $GPID
for w in host guest; do
  echo "== $w"
  grep -a "Terminal Velocity" $OUT/$w/run.err | grep -vE ' 0 frames|: [0-9.]+s: ' | cut -c1-200 | tail -12
  grep -a "Terminal Velocity: [0-9.]*s: " $OUT/$w/run.err | tail -2 | cut -c1-260
done
