#!/bin/bash
# The GUI's soak test: no flying, no player.  The whole game natively, under AddressSanitizer, on a virtual clock (a
# campaign in seconds), clicking and pressing at random (game.c's /monkey) along every way through the campaign's map
# and over many seeds, with wingmen lost, escorts lost, the player dead and the carrier lost at random (/kill).
#   tests/monkey.sh [-j N] [-s SEEDS] [-b ACTIONS] [-k KILL%] [-t SECONDS] [-n ROUTES] [--dos] [--flying]
# Every run must end in "bye", with no sanitizer report, no "LEAK:" on the ledger (src/mem.c) and no "FAIL".  A run still
# going after -t seconds is a freeze: gdb takes its stack (the emulator's, and the game's CS:IP, with --dos) before it is
# killed.  A failed run's files are kept in build/monkey/fail-*, with the line to run it again.
#   --dos     RV.EXE itself, in the native DOSBox with DOSBOX_TURBO (a real-flight campaign in under a minute);
#             RV_EXE=RVT.EXE the far-heap test build
#   --flying  real flights of a second, not /noflight
set -u
cd "$(dirname "$0")/.."
JOBS=4 SEEDS=3 BUDGET=120 KILL=25 LIMIT=180 DOS= FLYING= MAX=0
RV_EXE=${RV_EXE:-RV.EXE}
while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS=$2; shift 2;;
        -s) SEEDS=$2; shift 2;;
        -b) BUDGET=$2; shift 2;;
        -k) KILL=$2; shift 2;;
        -t) LIMIT=$2; shift 2;;
        -n) MAX=$2; shift 2;;
        --dos) DOS=1; shift;;
        --flying) FLYING=1; shift;;
        *) echo "usage: $0 [-j N] [-s SEEDS] [-b ACTIONS] [-k KILL%] [-t SECONDS] [-n ROUTES] [--dos] [--flying]"; exit 2;;
    esac
done
OUT=build/monkey
mkdir -p "$OUT"
rm -rf "$OUT"/run-* "$OUT"/fail-*
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
if [ -z "$DOS" ]; then
    SAN=asan host/game.sh > "$OUT/build.log" 2>&1 || { tail -5 "$OUT/build.log"; echo "the native build failed"; exit 1; }
    export RV_VCLOCK=1 ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
fi
FLAGS="/noflight"
[ -n "$FLYING" ] && FLAGS="/flight 1"
python3 tests/routes.py > "$OUT/routes.txt"
: > "$OUT/jobs.txt"
n=0
while read -r route; do
    for s in $(seq 1 "$SEEDS"); do
        n=$((n + 1))
        echo "$n $((n * 7919 + s)) $route" >> "$OUT/jobs.txt"
    done
done < <(if [ "$MAX" -gt 0 ]; then shuf --random-source=<(yes) -n "$MAX" "$OUT/routes.txt"; else cat "$OUT/routes.txt"; fi)
one() {
    id=$1; seed=$2; route=$3
    d=$OUT/run-$id
    args="$FLAGS /monkey $seed $BUDGET /route $route /kill $KILL"
    mkdir -p "$d"
    if [ -n "$DOS" ]; then
        for f in build/dos/*; do
            case "$f" in */RVLOG.TXT|*/BUNK*|*/RVSETUP.CFG|*/WALK.TXT|*.BMP|*.obj|*.MAP|*/t|*/p|*/stubs) ;; *) ln -sf "$PWD/$f" "$d/";; esac
        done
        sed -e "s#mount c ../dos#mount c $PWD/$d#" -e "s#RV.EXE /script WALK.TXT#${RV_EXE:-RV.EXE} $args#" \
            -e "s#^cycles=.*#cycles=${CYCLES:-20000}#" tests/rv.conf > "$d/run.conf"
        DOSBOX_TURBO=1 $HOME/dev/dosbox3d/build-native/src/dosbox -conf "$d/run.conf" > "$d/out.txt" 2>&1 &
        rc_p=$!
        log=$d/RVLOG.TXT
    else
        for f in build/game32-asan/run/*; do
            case "$f" in */RVLOG.TXT|*/BUNK*|*/RVSETUP.CFG) ;; *) ln -sf "$PWD/$f" "$d/";; esac
        done
        build/game32-asan/rv --data "$d" --mute /noquit $args > "$d/out.txt" 2>&1 &
        rc_p=$!
        log=$d/RVLOG.TXT
    fi
    t=0
    while kill -0 "$rc_p" 2>/dev/null && [ "$t" -lt "$((LIMIT * 2))" ]; do sleep 0.5; t=$((t + 1)); done
    why=
    if kill -0 "$rc_p" 2>/dev/null; then
        why=FREEZE
        tests/freezetrace.sh "$rc_p" "$log" > "$d/freeze.txt" 2>&1
        kill -9 "$rc_p" 2>/dev/null
    fi
    wait "$rc_p" 2>/dev/null; rc=$?
    if [ -z "$why" ]; then
        if grep -q "AddressSanitizer\|runtime error" "$d/out.txt"; then why=SANITIZER
        elif grep -q "^LEAK:" "$log" 2>/dev/null; then why=LEAK
        elif grep -q "^FREEZE\|^FAIL" "$log" 2>/dev/null; then why=FAIL
        elif ! grep -q "^bye" "$log" 2>/dev/null; then why=DIED
        fi
    fi
    if [ -n "$why" ]; then
        echo "$args" > "$d/again.txt"
        find "$d" -type l -delete
        mv "$d" "$OUT/fail-$id"
        echo "$why $id seed $seed route $route (rc $rc): $(tail -1 "$OUT/fail-$id/RVLOG.TXT" 2>/dev/null | cut -c1-70)"
    else
        echo "ok $id $(grep -c '^mission' "$log") missions, $(grep '^ending' "$log" | cut -c9-30), $(grep '^monkey' "$log" | cut -c9-24)"
        rm -rf "$d"
    fi
}
export -f one
export OUT FLAGS BUDGET KILL LIMIT DOS RV_EXE
total=$(wc -l < "$OUT/jobs.txt")
t0=$(date +%s)
xargs -P "$JOBS" -L 1 bash -c 'one "$0" "$1" "$2"' < "$OUT/jobs.txt" > "$OUT/results.txt"
ok=$(grep -c '^ok' "$OUT/results.txt")
echo "monkey: $ok of $total runs clean in $(( $(date +%s) - t0 )) s, $(grep -c '^ok.*victory' "$OUT/results.txt") to victory, $(grep -c '^ok.*loss' "$OUT/results.txt") to loss"
grep -v '^ok' "$OUT/results.txt" | head -40
[ "$ok" = "$total" ]
