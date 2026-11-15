#!/bin/bash
# Raptor, two machines over TCP on this one (each its own copy of the game): the host and a guest both routed through
# New Mission into the Bravo sector's first wave (the launch held for the one first there), each with its own extra key
# script.  The logs in $OUT/host and $OUT/guest (run.err), pictures there too.  docs/raptor.md.
#
#   scripts/rapnet-duo.sh [seconds] [the host's extra key script] [the guest's]
#
#   PORT=        the TCP port (default 7481)
#   GUEST_LATE=  seconds the guest starts after the host (default 2)
#   CYCLES=      (default 60000)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/rapduo}; mkdir -p $OUT/host $OUT/guest
DUR=${1:-110}; HX=${2:-}; GX=${3:-}
PORT=${PORT:-7481}
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
# New Mission, the name and call sign, the difficulty (as offered), the hangar's Fly Mission, the Bravo sector
route="41:enter,45:a,45.3:c,45.6:e,46:enter,49:a,49.3:c,49.6:e,50:enter,53:enter,57:down,59:enter,63:enter"
OUT=$OUT/host DOSBOX=$BIN CYCLES=${CYCLES:-60000} ENV="WCPORT=$PORT WCCALLSIGN=HOST ${HOSTENV:-}" SHOW=0 \
    $ROOT/scripts/raprun.sh $DUR "$route${HX:+,$HX}" &
HPID=$!
sleep ${GUEST_LATE:-2}
OUT=$OUT/guest DOSBOX=$BIN CYCLES=${CYCLES:-60000} ENV="WCHOST=127.0.0.1 WCPORT=$PORT WCCALLSIGN=GUEST ${GUESTENV:-}" SHOW=0 \
    $ROOT/scripts/raprun.sh $DUR "$route${GX:+,$GX}" &
GPID=$!
wait $HPID $GPID
for w in host guest; do
  echo "== $w"
  grep -a "Raptor" $OUT/$w/run.err | grep -v ": [0-9.]*s: " | cut -c1-200 | tail -12
  grep -a "Raptor: [0-9.]*s: " $OUT/$w/run.err | tail -3 | cut -c1-240
done
