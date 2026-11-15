#!/bin/bash
# The TIE Fighter demo's campaign (docs/tiedemo.md section 8), two machines over TCP (scripts/tdmnet-duo.sh): the host
# starts at mission FIRST (default 1, the shuttles: TDM_MISSION) and marks each mission's primary goals complete
# (!tdmwin) a while into each flight; every flight's end is the host's verdict, and the guest follows its go.  Checks,
# each printed with OK or FAIL: both machines fly each mission from FIRST to the last with their own craft of Alpha
# (the guest's seat written into the mission the layer wrote), each other's craft seen, each other's shots made, the
# host's verdicts taken by the guest, and both programs ending once the last is won.  Ends "CAMPAIGN DUO OK".
#
#   scripts/tdmcampaign-duo.sh [seconds]
#
#   FIRST=   the first mission (1..3)      WIN=  seconds into each flight the host's !tdmwin comes (default 22)
#   OUT=     the logs (default /tmp/tdmcampaign)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export OUT=${OUT:-/tmp/tdmcampaign}
DUR=${1:-200}
FIRST=${FIRST:-1}
WIN=${WIN:-22}
LAST=3
# the host: fire now and then; a win WIN s into each flight ("wait" restarts the clock at the next flight's first frame)
hx=""
for ((m = FIRST; m <= LAST; m++)); do
  hx="$hx${hx:+,}4:!button=0:1,9:!button=0:0,10:!tdmcraft,$WIN:!tdmwin,$((WIN + 1)):!tdmcraft,$((WIN + 2)):!shot=h$m"
  [ $m -lt $LAST ] && hx="$hx,$((WIN + 3)):wait"
done
gx=""
for ((m = FIRST; m <= LAST; m++)); do
  gx="$gx${gx:+,}5:!button=0:1,10:!button=0:0,11:!tdmcraft,$((WIN + 2)):!shot=g$m"
  [ $m -lt $LAST ] && gx="$gx,$((WIN + 3)):wait"
done
HOSTENV="TDM_MISSION=$FIRST" "$ROOT/scripts/tdmnet-duo.sh" "$DUR" "$hx" "$gx" > /dev/null
H=$OUT/host/run.err G=$OUT/guest/run.err
failed=0
check() { if eval "$1"; then echo "OK   $2"; else echo "FAIL $2"; failed=$((failed + 1)); fi; }
for ((m = FIRST; m <= LAST; m++)); do
  n=$((m + 1))
  check "grep -aq 'mission $n of 4, .*written over' $H" "host: mission $n of 4 written and flown"
  check "grep -aq 'mission $n of 4, .*written over' $G" "guest: mission $n of 4 written and flown (the host's go)"
done
check "grep -aq 'we fly craft 1 of flight group 0' $G" "guest: its own craft of Alpha in the written missions"
for ((m = FIRST; m < LAST; m++)); do
  check "grep -aq \"mission $m over (status [0-9]*): mission $((m + 1)) next\" $G" "guest: the host's verdict after mission $((m + 1)): mission $((m + 2)) next"
done
check "grep -aq 'the campaign over: the program ends' $H" "host: the campaign won, its program ends"
check "grep -aq 'the campaign over: the program ends' $G" "guest: the campaign over on the host's verdict, its program ends"
check "grep -aq ' of pilot 2: here' $H" "host: the guest's craft seen"
check "grep -aq ' of pilot 1: here' $G" "guest: the host's craft seen"
hm=$(grep -a "TIE demo: [0-9.]*s: frame" $H | grep -o "[0-9]* made" | awk '{s+=$1} END {print s+0}')
gm=$(grep -a "TIE demo: [0-9.]*s: frame" $G | grep -o "[0-9]* made" | awk '{s+=$1} END {print s+0}')
echo "     the other's shots made: host $hm, guest $gm"
check "[ ${hm:-0} -gt 0 ] && [ ${gm:-0} -gt 0 ]" "each machine made the other's shots"
echo "== host"; grep -a "campaign\|verdict\|launch [0-9]" $H | cut -c1-170 | tail -14
echo "== guest"; grep -a "campaign\|verdict\|held" $G | cut -c1-170 | tail -14
[ $failed -eq 0 ] && echo "CAMPAIGN DUO OK" || echo "CAMPAIGN DUO FAILED ($failed)"
