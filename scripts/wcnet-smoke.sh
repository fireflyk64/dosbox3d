#!/bin/bash
# Headless smoke test for the Wing Commander multiplayer layer.
#
# Starts a server and a client DOSBox (dummy SDL video/audio), both jumping
# straight into campaign mission MIS/SERIES, lets them run for DURATION
# seconds, then prints their wcnet logs.  Useful to check the connection
# handshake, briefing sync and mission start without a display.
#
#   scripts/wcnet-smoke.sh [duration_seconds] [mission] [series]
#
# Logs land in $OUT (default /tmp/wcnet-smoke).  Set WCNET_LOG=3 for a full
# protocol trace.  Set WCNET_AUTOKEYS=1 to have both instances press Enter
# periodically so the briefing advances without a human.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DURATION="${1:-60}"
MIS="${2:-1}"
SERIES="${3:-1}"
OUT="${OUT:-/tmp/wcnet-smoke}"
PORT="${WCPORT:-13399}"
DOSPATH="${DOSPATH:-$ROOT/wc}"
mkdir -p "$OUT"
pkill -9 -x dosbox 2>/dev/null   # never let a stray instance join the test
rm -f "$OUT"/server.* "$OUT"/client.*

launch() {  # name, extra env...
    local name="$1"; shift
    ( cd "$OUT" && exec env "$@" MIS="$MIS" SERIES="$SERIES" WCPORT="$PORT" WCNET_LOG="${WCNET_LOG:-2}" \
        WCNET_AUTOKEYS="${WCNET_AUTOKEYS:-}" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        timeout -s KILL "$DURATION" "$ROOT/src/dosbox" -c "mount c $DOSPATH" -c "c:" -c "wc" \
        > "$OUT/$name.out" 2> "$OUT/$name.err" ) &
}

launch server WCHOST=
sleep 3
launch client WCHOST=127.0.0.1 WCCALLSIGN=WINGMAN
echo "running server and client for ${DURATION}s (logs in $OUT)"
sleep $((DURATION + 2))
pkill -9 -x dosbox 2>/dev/null
echo "===== server ($OUT/server.err) ====="
grep -v "^$" "$OUT/server.err" | tail -60
echo "===== client ($OUT/client.err) ====="
grep -v "^$" "$OUT/client.err" | tail -60
