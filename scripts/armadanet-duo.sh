#!/bin/bash
# Wing Commander Armada, two (or more) machines on this one over TCP, the rig's stand-in for a room: each its own copy
# of the game, NETBIOS.EXE on the emulator's IPX carried by the links (src/cpu/wcnet_ipx.cpp), then the program; each
# with its own key script.  Logs and pictures in $OUT/host, $OUT/guest (guest2 ...).  docs/armada.md.
#
#   scripts/armadanet-duo.sh [seconds] [the host's key script] [the guest's]
#
#   PORT=        the TCP port (default 7491)
#   PLAYERS=     machines (default 2; Proving Grounds takes up to 6: G2X= G3X= ... their key scripts)
#   PROG=        armada (default) or pg (Proving Grounds: NETBIOS=0 is its way, it talks IPX itself)
#   GUEST_LATE=  seconds the guest starts after the host (default 2)
#   NET=ipxnet   DOSBox's own IPX over UDP instead (IPXNET STARTSERVER on the host), for comparison
#   CYCLES=      (default 25000)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/armadaduo}; mkdir -p $OUT/host $OUT/guest
DUR=${1:-90}; HX=${2:-}; GX=${3:-}
PORT=${PORT:-7491}
PLAYERS=${PLAYERS:-2}
BIN=$OUT/dosbox
cp -f ${DOSBOX:-$ROOT/build-native/src/dosbox} $BIN
if [ "${NET:-room}" = ipxnet ]; then
  HNET="NET=ipxnet=server"; GNET="NET=ipxnet=127.0.0.1"; HENV=""; GENV=""
else
  HNET="NET=room"; GNET="NET=room"; HENV="WCPORT=$PORT WCPLAYERS=$PLAYERS"; GENV="WCHOST=127.0.0.1 WCPORT=$PORT"
fi
env $HNET OUT=$OUT/host DOSBOX=$BIN ENV="$HENV WCCALLSIGN=HOST ${HOSTENV:-}" SHOW=0 IPXPORT=$((PORT+1)) \
    $ROOT/scripts/armadarun.sh $DUR "$HX" &
PIDS=$!
sleep ${GUEST_LATE:-2}
env $GNET OUT=$OUT/guest DOSBOX=$BIN ENV="$GENV WCCALLSIGN=GUEST ${GUESTENV:-}" SHOW=0 IPXPORT=$((PORT+1)) \
    $ROOT/scripts/armadarun.sh $DUR "$GX" &
PIDS="$PIDS $!"
who="host guest"
for n in 2 3 4 5; do
  [ $PLAYERS -gt $n ] || continue
  mkdir -p $OUT/guest$n; x=G${n}X
  env $GNET OUT=$OUT/guest$n DOSBOX=$BIN ENV="$GENV WCCALLSIGN=GUEST$n ${GUESTENV:-}" SHOW=0 IPXPORT=$((PORT+1)) \
      $ROOT/scripts/armadarun.sh $DUR "${!x:-}" &
  PIDS="$PIDS $!"; who="$who guest$n"
done
wait $PIDS
for w in $who; do
  echo "== $w"
  grep -a "IPX" $OUT/$w/run.err $OUT/$w/run.out 2>/dev/null | sed 's/^[^:]*://' | cut -c1-200 | tail -${SHOW:-8}
done
