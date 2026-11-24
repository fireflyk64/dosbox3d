#!/bin/bash
# X-Wing Collector's CD, two or more machines over TCP on this one (docs/xwing.md), each with its own copy of the game
# (scripts/xwrun.sh): the host and the guests, each flown from boot to Tour of Duty I's first mission by the route's
# keys and clicks, each with its own extra key script.  The logs in $OUT/host, $OUT/guest, $OUT/guest2 ... (run.err),
# pictures there too.
#
#   scripts/xwnet-duo.sh [seconds] [the host's extra key script] [the guests']
#
#   PORT=        the TCP port (default 7493)
#   PLAYERS=     machines (default 2; up to 4)
#   GUEST_LATE=  seconds each guest starts after the one before (default 1)
#   ROUTE=       the route to the flight (default tod1; trench: Historical Combat, the B-Wing, see scripts/xwrun.sh)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/xwduo}; mkdir -p $OUT
DUR=${1:-150}; HX=${2:-}; GX=${3:-}
PORT=${PORT:-7493}; N=${PLAYERS:-2}
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
PIDS=""
for i in $(seq 0 $((N-1))); do
  if [ $i = 0 ]; then W=host; E="WCPORT=$PORT WCCALLSIGN=HOST WCPLAYERS=$N ${HOSTENV:-}"; X=$HX
  else W=guest$([ $i -gt 1 ] && echo $i); E="WCHOST=127.0.0.1 WCPORT=$PORT WCCALLSIGN=GUEST$i ${GUESTENV:-}"; X=$GX; fi
  mkdir -p $OUT/$W
  FRESH=1 OUT=$OUT/$W DOSBOX=$BIN ENV="$E" $ROOT/scripts/xwrun.sh $DUR "${ROUTE:-tod1}${X:+,$X}" > /dev/null &
  PIDS="$PIDS $!"
  sleep ${GUEST_LATE:-1}
done
wait $PIDS
for w in $(cd $OUT && ls -d host guest* 2>/dev/null); do
  echo "== $w"
  grep -a "X-Wing" $OUT/$w/run.err | grep -v ": [0-9.]*s: \|key script" | cut -c1-220 | tail -${TAIL:-14}
  grep -a "X-Wing: [0-9.]*s: " $OUT/$w/run.err | tail -1 | cut -c1-400
done
