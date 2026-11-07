#!/bin/bash
# TIE Fighter: step 15's scripted pilot (docs/tiefighter.md), a key script for scripts/tfnet-duo.sh, from $1 to $2 s:
# full throttle (Backspace) and the nearest moving enemy of ship indexes $SHIP (default 5-9, the Empire's fighters)
# targeted every 2 s; every 0.2 s our craft turned to where a laser meets the
# target, the fire button held while it is within $RANGE km (default 1.0); a click every 10 s (the debriefing's Fly
# Mission Again after a death); a screenshot every 20 s ($3: its name's prefix).
from=$1; to=$2; p=${3:-x}
t=""
for s in $(seq $from 2 $to); do t="$t${t:+,}$s:backspace,$s.2:!tietarget=nearest:${SHIP:-5-9}"; done
for s in $(seq $from 0.2 $to); do t="$t,$s:!tieface=target:${RANGE:-1.0}"; done
for s in $(seq $from 10 $to); do t="$t,$((s+5)).1:!button=0:1,$((s+5)).4:!button=0:0"; done
for s in $(seq $from 20 $to); do t="$t,$((s+1)).1:!shot=$p$s"; done
echo "$t"
