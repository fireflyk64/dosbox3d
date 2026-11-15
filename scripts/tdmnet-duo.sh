#!/bin/bash
# The TIE Fighter demo, two machines over TCP on this one (docs/tiedemo.md), each with its own copy of the game
# (scripts/tdmrun.sh): the host and a guest, Esc at 8 s past the opening, each with its own extra key script.  The
# logs in $OUT/host and $OUT/guest (run.err), pictures there too.
#
#   scripts/tdmnet-duo.sh [seconds] [the host's extra key script] [the guest's]
#
#   PORT=        the TCP port (default 7491)
#   OPENING=     the keys to the mission (default 8:esc; the Internet Archive's first release, its Neon showroom
#                first: 3:g,10:esc, with TDMDISKS its floppies)
#   GUEST_LATE=  seconds the guest starts after the host (default 1)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/tdmduo}; mkdir -p $OUT/host $OUT/guest
DUR=${1:-40}; HX=${2:-}; GX=${3:-}
PORT=${PORT:-7491}
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
OUT=$OUT/host DOSBOX=$BIN ENV="WCPORT=$PORT WCCALLSIGN=HOST ${HOSTENV:-}" $ROOT/scripts/tdmrun.sh $DUR "${OPENING:-8:esc}${HX:+,$HX}" > /dev/null &
HPID=$!
sleep ${GUEST_LATE:-1}
OUT=$OUT/guest DOSBOX=$BIN ENV="WCHOST=127.0.0.1 WCPORT=$PORT WCCALLSIGN=GUEST ${GUESTENV:-}" $ROOT/scripts/tdmrun.sh $DUR "${OPENING:-8:esc}${GX:+,$GX}" > /dev/null &
GPID=$!
wait $HPID $GPID
for w in host guest; do
  echo "== $w"
  grep -a "TIE demo" $OUT/$w/run.err | grep -v ": [0-9.]*s: " | cut -c1-200 | tail -10
  grep -a "TIE demo: [0-9.]*s: " $OUT/$w/run.err | tail -2 | cut -c1-240
done
