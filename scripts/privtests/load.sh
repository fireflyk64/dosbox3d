#!/bin/bash
# Privateer's load test (docs/privateer.md section 15): a room of PLAYERS at Achilles' nav point in Troy, every pilot
# turning on the nearest pirate and firing guns and missiles, the host adding PIRATES pirates to the natural
# encounter; every machine's near heap sampled (the per-second log line), the summary printed: each machine's lowest
# free and what it showed then, its errors and deaths, and the co-op table (pirates seen / pilots shown / encounter
# puppets / heap low) every 5 s.
#
#   scripts/privtests/load.sh [seconds]          (default 145)
#   PLAYERS=8 PIRATES=3 OUT=/tmp/privload DOSBOX=build-native/src/dosbox
#
# A pass: no "errors=" but 0, no "loads=" but 1 (the game program never restarted).  Measured (2026-11-08, 8 players):
# PIRATES=3: guests >= 10,250 free, the owner >= 5,302; PIRATES=5: guests >= 6,380, the owner >= 3,834; PIRATES=12:
# the owner's game ran out (its error 1303) -- more fighting AI ships than the game itself ever spawns at one place.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DUR=${1:-145}; PLAYERS=${PLAYERS:-8}; PIRATES=${PIRATES:-3}; OUT=${OUT:-/tmp/privload}
rm -rf "$OUT"; mkdir -p "$OUT"
F="34:!privturn=pirate:60:150,35:+space,50:-space,52:enter,54:+space,70:-space,72:enter,74:+space,90:-space,92:enter,94:+space,110:-space,112:enter,114:+space,130:-space"
W=""
for j in $(seq 0 $((PIRATES - 1))); do
  W="$W,$((30 + j)):!privspawn=TALNWIMP:TALPIR:PIR_DA:$(( (j % 5 - 2) * 200000 )):$(( 700000 + (j / 5) * 100000 )):$(( (j % 5) * 80000 ))"
done
H="${W#,}"; H="${H:+$H,}$F"
OUT=$OUT PLAYERS=$PLAYERS LINES_EACH=0 "$ROOT/scripts/privnet.sh" "$DUR" "$H" "$F" > /dev/null 2>&1
python3 "$ROOT/scripts/privtests/loadsum.py" "$OUT"
python3 "$ROOT/scripts/privtests/coopsum.py" "$OUT"
echo "the owner's give-backs $(grep -ac 'given back' "$OUT/host/run.err"), its wings that waited $(grep -ac 'wing waits' "$OUT/host/run.err"); the guests' give-backs $(cat "$OUT"/guest*/run.err 2>/dev/null | grep -ac 'given back')"
