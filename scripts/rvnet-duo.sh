#!/bin/bash
# RetroVega flown by two (or three) native DOSBoxes over TCP on this machine: one scenario, then its checks
# (scripts/rvnet-check.py).  retrovega/docs/multiplayer.md is what is checked.
#
#   scripts/rvnet-duo.sh [SECONDS]          the scenario in SCENARIO (default meet), for SECONDS (default 45)
#
#   SCENARIO  meet        the host flies mission 0, the guest asked for mission 3: they meet in the host's, each sees
#                         the other where it is, every ship the host owns is a puppet on the guest
#             retry       the host dies (forced), presses R on its death screen: both fly the mission again
#             carrier     the host's carrier is lost (forced status 6): the carrier's screen on both, R
#             guestdeath  the guest dies (forced): both on their death screens, the host's R decides
#             land        the guest lands on the carrier (forced, !land): the host's game lands on the same ship
#             autopilot   the host's autopilot takes the guest along
#             combat      after the host's autopilot to the enemies, the guest flies at them and fires: its hits
#                         reach the host's game, and come back to it
#             kill        the host's autopilot to the enemies, which it destroys (!killfoes): the guest sees them
#                         explode, then go
#   PLAYERS   2 (default) or 3: a third pilot, seated by the host
#   OUT       where the instances run and leave their logs and dumps (default /tmp/rvnet-SCENARIO)
#   PORT      the TCP port (default 13951); DOSBOX the emulator (default this tree's build-native);
#   GAME      the game's directory (default retrovega/build/dos); WCFPS passes on
#
# Every instance gets a directory of its own: symlinks to the game's files but those it writes (RV.EXE is copied:
# a running EXE is not shared).  One pair at a time on this machine (the sandbox's pid limit).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECONDS_RUN=${1:-45}
SCENARIO=${SCENARIO:-meet}
PLAYERS=${PLAYERS:-2}
OUT=${OUT:-/tmp/rvnet-$SCENARIO}
PORT=${PORT:-13951}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
GAME=${GAME:-$ROOT/retrovega/build/dos}
[ -x "$DOSBOX" ] || { echo "rvnet-duo: no emulator at $DOSBOX" >&2; exit 2; }
[ -f "$GAME/RV.EXE" ] || { echo "rvnet-duo: no RV.EXE in $GAME" >&2; exit 2; }

# the scripts (WCNET_KEYSCRIPT: T:item, T seconds from the first frame in flight; `T:wait` waits for the next flight
# and starts the clock again there, `T:waitkey=r` tapping R meanwhile); every instance stops its own ship first, so
# where it is holds still for the dumps
STOP="0.5:s,1:s"
HOST_MISSION=0
GUEST_MISSION=3
HOST="$STOP,3:!pos=1000000:0:0,8:!ds=moved"
GUEST="$STOP,3:!pos=-1000000:0:0,8:!ds=moved"
case "$SCENARIO" in
meet) ;;
retry)
    HOST="$HOST,10:!status=4,10.5:waitkey=r,3:!ds=again"
    GUEST="$GUEST,9:wait,3:!ds=again" ;;
carrier)
    HOST="$HOST,10:!status=6,10.5:waitkey=r,3:!ds=again"
    GUEST="$GUEST,9:wait,3:!ds=again" ;;
guestdeath)
    HOST="$HOST,9:waitkey=r,3:!ds=again"
    GUEST="$GUEST,10:!status=4,10.5:wait,3:!ds=again" ;;
land)
    GUEST="$GUEST,10:!land" ;;
autopilot)
    HOST="$HOST,10:a,14:!ds=after"
    GUEST="$GUEST,14:!ds=after" ;;
combat)
    HOST="$HOST,10:a"
    GUEST="$GUEST,13:!face=npc,13.3:f,15:!face=npc,15.5:+space,19:-space,19.2:!face=npc,19.5:+space,23:-space,23.5:enter,24:!face=npc,24.5:+space,28:-space" ;;
kill)
    HOST="$HOST,10:a,15:!killfoes,15.7:!ds=dying,22:!ds=gone"
    GUEST="$GUEST,15.8:!ds=dying,22:!ds=gone" ;;
*) echo "rvnet-duo: no scenario $SCENARIO" >&2; exit 2 ;;
esac

rm -rf "$OUT"
mkdir -p "$OUT"
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT

# instance N: the directory, its conf, and the emulator started in it with its environment
start() {
    local n=$1 mission=$2 script=$3; shift 3
    local dir="$OUT/p$n"
    mkdir -p "$dir/game" "$dir/dump"
    for f in "$GAME"/*; do
        case "$(basename "$f")" in
        RV.EXE) cp "$f" "$dir/game/" ;;
        RVLOG.TXT|*.BMP|BUNK*.SAV|RVSETUP.CFG|WALK.TXT|*.PPM) ;;
        *) ln -s "$f" "$dir/game/" ;;
        esac
    done
    sed -n '/^\[autoexec\]/q;p' "$ROOT/retrovega/tests/rv.conf" > "$dir/run.conf"
    printf '[autoexec]\nmount c "%s"\nc:\nRV.EXE /fly %d /noquit /verbose\nexit\n' "$dir/game" "$mission" >> "$dir/run.conf"
    (cd "$dir" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCRVNET=1 WCPORT="$PORT" WCPLAYERS="$PLAYERS" \
        WCNET_LOG=2 WCNET_PERF=5 WCNET_DUMP_DIR="$dir/dump" WCNET_KEYSCRIPT="$script" "$@" \
        timeout "$SECONDS_RUN" "$DOSBOX" -conf "$dir/run.conf" > "$dir/dosbox.log" 2>&1) &
    PIDS+=($!)
}

start 0 "$HOST_MISSION" "$HOST" WCCALLSIGN=CHEATHOST
sleep 1
start 1 "$GUEST_MISSION" "$GUEST" WCHOST=127.0.0.1 WCCALLSIGN=GUEST
if [ "$PLAYERS" = 3 ]; then
    sleep 1
    start 2 "$GUEST_MISSION" "$(echo "$GUEST" | sed 's/!pos=-1000000:0:0/!pos=0:1000000:0/')" WCHOST=127.0.0.1 WCCALLSIGN=THIRD
fi
for p in "${PIDS[@]}"; do wait "$p" || true; done
PIDS=()
python3 "$ROOT/scripts/rvnet-check.py" "$OUT" "$PLAYERS" "$SCENARIO"
