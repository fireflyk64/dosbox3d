#!/bin/bash
# The TIE Fighter demo's sync test (docs/tiedemo.md section 5): a flight on two machines (scripts/tdmnet-duo.sh), both
# logging every craft every few seconds (!tdmcraft, with the wall clock), then the two tables compared craft by craft,
# by identity (scripts/tdmnet-syncmp.py): a craft on one machine only, its place, hull, shields, warheads, dying, the
# goals; the differences that last 4 s or more listed apart ("PERSISTING": a difference in one table alone is a hit or
# a death on its way).
#
#   scripts/tdmnet-sync.sh NAME SECONDS MISSION "the host's keys" "the guest's keys" [seconds between tables: 2]
#
#   MISSION  the campaign's (TDM_MISSION): 0 the demo's own, 1 the Shuttles Run, 2 Corvette Hunt, 3 Escort the Prowlers
#   OUT=     where (default /tmp/tdmsync): $OUT/NAME/host, guest (run.err), $OUT/NAME.cmp
#   keys such as "16:backspace,16.5:!tdmsteer=god,18:!tdmsteer=hostile:1.2" (both pilots at the rebels, never shot down)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S=${OUT:-/tmp/tdmsync}; mkdir -p $S
NAME=$1; DUR=$2; MIS=$3; HK=$4; GK=$5; EVERY=${6:-2}
hs=""; gs=""
# (the guest starts a second after the host: its tables 0.8 s earlier in its script, to fall with the host's)
for ((t=20; t<DUR-2; t+=EVERY)); do hs="$hs,$t:!tdmcraft"; gs="$gs,$((t-1)).2:!tdmcraft"; done
OUT=$S/$NAME PORT=${PORT:-7531} OPENING="${OPENING:-12:esc}" HOSTENV="WCNET_LOG_WALL=1 TDM_MISSION=$MIS ${HOSTENV:-}" \
  GUESTENV="WCNET_LOG_WALL=1 ${GUESTENV:-}" "$ROOT/scripts/tdmnet-duo.sh" $DUR "$HK$hs" "$GK$gs" > $S/$NAME.out 2>&1
python3 "$ROOT/scripts/tdmnet-syncmp.py" $S/$NAME/host/run.err $S/$NAME/guest/run.err > $S/$NAME.cmp
sed -n '/^PERSISTING/,$p' $S/$NAME.cmp
