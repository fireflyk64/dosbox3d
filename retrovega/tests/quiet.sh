#!/bin/bash
# What RV.EXE promises about its log and its memory (the user, 2026-11-01), in real flights in DOSBox, the page's way
# (no /verbose): tests/fight.txt flies each mission, firing and on the autopilot.
#   tests/quiet.sh [MISSION ...]    default a spread: launch fights, fields, waves, capital ships; `seq 0 39` for all
#   1. nothing is logged from a flight's first frame to its end, but for what is wrong (FLIGHT_S=40 a flight, s);
#   2. conventional memory holds: no "LEAK:", the flight's end says 0 falls, DOS and the near heap where they started;
#   3. the music loaded whole, no track refused for want of room, and the exact draw's slab was there: none refused;
#   4. the watch can speak: with /leaktest its LEAK lines come.
# A flight at the page's speed is not needed: DOSBOX_TURBO=1 at 20,000 cycles, CYCLES= another.
set -u
cd "$(dirname "$0")/.."
[ $# -gt 0 ] || set -- 0 2 9 17 30
OUT=build/quiet
rm -rf "$OUT"
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy DOSBOX_TURBO=1
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# run NAME MISSION [switches]: until the flight is over (a flight of FLIGHT_S at most), the log in $OUT/NAME
run() {
    local name=$1 m=$2 d=$OUT/$1
    shift 2
    mkdir -p "$d"
    for f in build/dos/*; do
        case "$f" in */RVLOG.TXT|*/BUNK*|*/RVSETUP.CFG|*/WALK.TXT|*.BMP|*.obj|*.MAP|*/t|*/p|*/stubs) ;; *) ln -sf "$PWD/$f" "$d/";; esac
    done
    cp tests/fight.txt "$d/WALK.TXT"
    sed -e "s#mount c ../dos#mount c $PWD/$d#" -e "s#RV.EXE /script WALK.TXT#RV.EXE /script WALK.TXT /fly $m /flight ${FLIGHT_S:-40} $*#" \
        -e "s#^cycles=.*#cycles=${CYCLES:-20000}#" tests/rv.conf > "$d/run.conf"
    ${DOSBOX:-$HOME/dev/dosbox3d/build-native/src/dosbox} -conf "$d/run.conf" > "$d/out.txt" 2>&1 &
    local p=$! t=0
    while kill -0 "$p" 2> /dev/null && [ "$t" -lt 600 ] && ! grep -q "^flight: over" "$d/RVLOG.TXT" 2> /dev/null; do
        sleep 0.5
        t=$((t + 1))
    done
    sleep 1                             # (the flight's end lines)
    kill -9 "$p" 2> /dev/null
    wait "$p" 2> /dev/null
    find "$d" -type l -delete
    tr -d '\r' < "$d/RVLOG.TXT" > "$d/log.txt"
}

for m in "$@"; do
    run "m$m" "$m"
    L=$OUT/m$m/log.txt
    grep -q "^flight: over" "$L" || { bad "mission $m: the flight did not end"; continue; }
    # the flight's first frame on: the watch's start line and the card's come before it, on the way in
    talk=$(sed -n '/^flight: [^ ]*, [0-9]* ships/,/^flight: over/p' "$L" \
        | grep -Ev '^flight: |^sfx: a Sound Blaster|^conventional memory, the watch starts')
    [ -z "$talk" ] || bad "mission $m: lines in the flight:" $'\n'"$(echo "$talk" | cut -c1-120 | head -10)"
    grep -q "^LEAK" "$L" && bad "mission $m:" $'\n'"$(grep '^LEAK' "$L" | cut -c1-130 | head -5)"
    grep -q "does not fit" "$L" && bad "mission $m: $(grep -m1 'does not fit' "$L")"
    grep -q "no room for the dense slab" "$L" && bad "mission $m: $(grep -m1 'no room for the dense slab' "$L")"
    grep -q "dense copies:.*, 0 refused" "$L" || bad "mission $m: $(grep -m1 'dense copies:' "$L")"
    end=$(grep -m1 "^conventional memory at the flight's end" "$L")
    case "$end" in *": 0 falls;"*) ;; *) bad "mission $m: $end";; esac
    echo "ok   mission $m: $(grep -m1 '^flight: over' "$L" | cut -c9-), ${end#*: }"
done

run leaktest 0 /leaktest 30
n=$(grep -c "^LEAK: conventional memory fell" "$OUT/leaktest/log.txt")
if [ "$n" -gt 0 ]; then echo "ok   /leaktest: $n falls reported"; else bad "/leaktest: the watch said nothing"; fi

[ "$fails" -eq 0 ] && echo "QUIET OK" || { echo "$fails failures"; exit 1; }
