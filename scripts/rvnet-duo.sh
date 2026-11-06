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
#             autopilot   the host's autopilot: its flyby shown on the guest at once, then the guest taken along
#             combat      after the host's autopilot to the enemies, the guest flies at them and fires: its hits
#                         reach the host's game, and come back to it
#             ram         after the host's autopilot to the enemies, the guest flies into them: its ship's share of
#                         each collision goes to the host's game
#             kill        the host's autopilot to the enemies, which it destroys (!killfoes): the guest sees them
#                         explode, then go
#             chat        each pilot says a line (the 0 key's prompt, !say=): it is on the other's comm VDU, and its
#                         own
#             field       (HOST_MISSION=1 GUEST_MISSION=1: ENIGMA2) nav 2 picked first, the host's autopilot stops at
#                         the asteroid field on that way, and both fly into it: each RVLOG's rocks
#   PLAYERS   2 (default) or 3: a third pilot, seated by the host
#   ROOM      1: the instances meet in a room of a local lobby server (LOBBY_PORT, default 13961), as the pages do,
#             not over TCP: there the host's first mission never waits for the guest, and after a mission flown
#             together it waits at the briefing for the guest's (the lockstep: docs/multiplayer.md)
#   GUEST_DELAY  seconds between the host's start and the guest's (default 1): with ROOM=1 and a long one, the
#             guest joins the host's mission in progress
#   ROOM_LAG  ROOM=1 with no GUEST_DELAY: the host's scripted steps come this much later (default 6 s), the guest's
#             way into the host's first mission being some seconds long
#   OUT       where the instances run and leave their logs and dumps (default /tmp/rvnet-SCENARIO)
#   PORT      the TCP port (default 13951); DOSBOX the emulator (default this tree's build-native);
#   GAME      the game's directory (default retrovega/build/dos); WCFPS passes on; RV_ARGS more of the game's switches
#             (with GAME a copy whose RV.EXE is RVT.EXE: /record N, the test build's screenshots)
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
ROOM=${ROOM:-}
LOBBY_PORT=${LOBBY_PORT:-13961}
GUEST_DELAY=${GUEST_DELAY:-1}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
GAME=${GAME:-$ROOT/retrovega/build/dos}
[ -x "$DOSBOX" ] || { echo "rvnet-duo: no emulator at $DOSBOX" >&2; exit 2; }
[ -f "$GAME/RV.EXE" ] || { echo "rvnet-duo: no RV.EXE in $GAME" >&2; exit 2; }

# the scripts (WCNET_KEYSCRIPT: T:item, T seconds from the first frame in flight; `T:wait` waits for the next flight
# and starts the clock again there, `T:waitkey=r` tapping R meanwhile); every instance stops its own ship first, so
# where it is holds still for the dumps
STOP="0.5:s,1:s"
HOST_MISSION=${HOST_MISSION:-0}
GUEST_MISSION=${GUEST_MISSION:-3}
# (a late guest: the host's look comes as late, so that both are of the same moment: its clock starts at its own
# first frame in flight, the guest's at the guest's)
HOST_LOOK=$((GUEST_DELAY > 1 ? 10 + GUEST_DELAY : 8))
HOST="$STOP,3:!pos=1000000:0:0,$HOST_LOOK:!ds=moved"
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
    HOST="$HOST,10:a,11.5:!shot=flyby,14:!ds=after"
    GUEST="$GUEST,11.5:!shot=flyby,14:!ds=after" ;;
ram)
    HOST="$HOST,10:a"
    GUEST="$GUEST,13:f,15:!ram=npc,17:!ram=npc,19:!ram=npc" ;;
combat)
    HOST="$HOST,10:a"
    GUEST="$GUEST,13:!face=npc,13.3:f,15:!face=npc,15.5:+space,19:-space,19.2:!face=npc,19.5:+space,23:-space,23.5:enter,24:!face=npc,24.5:+space,28:-space" ;;
kill)
    HOST="$HOST,10:a,15:!killfoes,15.7:!ds=dying,22:!ds=gone"
    GUEST="$GUEST,15.8:!ds=dying,22:!ds=gone" ;;
chat)
    HOST="$HOST,9:!say=hello_from_the_host,12:!shot=chat"
    GUEST="$GUEST,10:!say=hi_from_the_guest,12:!shot=chat" ;;
field)                                  # (HOST_MISSION=1 GUEST_MISSION=1: ENIGMA2, its field on the way to nav 2)
    HOST="$HOST,10:n,11:a,16:f,46:!ds=field"
    GUEST="$GUEST,16:f,46:!ds=field" ;;
field1)                                 # (HOST_MISSION=0 GUEST_MISSION=0: ENIGMA1, whose field covers nav 2)
    HOST="$HOST,10:a,15:!killfoes,22:a,27:f,80:!ds=field"
    GUEST="$GUEST,27:f,80:!ds=field" ;;
*) echo "rvnet-duo: no scenario $SCENARIO" >&2; exit 2 ;;
esac

# (ROOM=1: the host's first mission never waits, so the guest comes into it some seconds late; the host's own steps
# before its first wait move ROOM_LAG seconds later, its stop at the start apart, so that the two meet as over TCP)
if [ -n "$ROOM" ] && [ "$GUEST_DELAY" -le 1 ]; then
    HOST=$(python3 -c '
import sys
lag, items, out, shifting = float(sys.argv[2]), sys.argv[1].split(","), [], True
for it in items:
    t, _, what = it.partition(":")
    if shifting and float(t) > 1:
        t = "%g" % (float(t) + lag)
    if what.startswith("wait"):
        shifting = False
    out.append(t + ":" + what)
print(",".join(out))' "$HOST" "${ROOM_LAG:-6}")
    HOST_LOOK=$((8 + ${ROOM_LAG:-6}))
fi
rm -rf "$OUT"
mkdir -p "$OUT"
PIDS=()
LOBBY_PID=
cleanup() { for p in "${PIDS[@]:-}" $LOBBY_PID; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
# (ROOM=1: the local lobby server, and every instance's way into its room instead of the TCP port)
NET=(WCPORT="$PORT")
GUEST_NET=(WCHOST=127.0.0.1)
if [ -n "$ROOM" ]; then
    LOBBY="$ROOT/build-web/p2p-lobby-server"
    [ -x "$LOBBY" ] || (cd "$ROOT/lobbylink" && PATH="$PATH:/usr/local/go/bin" CGO_ENABLED=0 go build -trimpath -o "$LOBBY" ./cmd/p2p-lobby-server)
    printf '[rooms]\nmax_players_hard = 256\n' > "$OUT/lobby.toml"
    "$LOBBY" --config "$OUT/lobby.toml" --listen-http "127.0.0.1:$LOBBY_PORT" --public-url "http://127.0.0.1:$LOBBY_PORT" \
        --allow-no-origin > "$OUT/lobby.log" 2>&1 &
    LOBBY_PID=$!
    sleep 1
    CODE="RV-$((RANDOM % 9000 + 1000))"   # (not tr | head: pipefail takes its SIGPIPE for a failure)
    NET=(WCROOM="$CODE" WCLOBBY="http://127.0.0.1:$LOBBY_PORT" WCLOBBY_ORIGIN=)
    GUEST_NET=()
    echo "rvnet-duo: room $CODE at 127.0.0.1:$LOBBY_PORT"
fi

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
    printf '[autoexec]\nmount c "%s"\nc:\nRV.EXE /fly %d /noquit /verbose %s\nexit\n' "$dir/game" "$mission" "${RV_ARGS:-}" >> "$dir/run.conf"
    (cd "$dir" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCRVNET=1 "${NET[@]}" WCPLAYERS="$PLAYERS" \
        WCNET_LOG=2 WCNET_PERF=5 WCNET_DUMP_DIR="$dir/dump" WCNET_KEYSCRIPT="$script" "$@" \
        timeout -k 5 "$SECONDS_RUN" "$DOSBOX" -conf "$dir/run.conf" > "$dir/dosbox.log" 2>&1) &
    PIDS+=($!)
}

start 0 "$HOST_MISSION" "$HOST" WCCALLSIGN=CHEATHOST
sleep "$GUEST_DELAY"
start 1 "$GUEST_MISSION" "$GUEST" "${GUEST_NET[@]}" WCCALLSIGN=GUEST
if [ "$PLAYERS" = 3 ]; then
    sleep 1
    start 2 "$GUEST_MISSION" "$(echo "$GUEST" | sed 's/!pos=-1000000:0:0/!pos=0:1000000:0/')" "${GUEST_NET[@]}" WCCALLSIGN=THIRD
fi
for p in "${PIDS[@]}"; do wait "$p" || true; done
PIDS=()
RVNET_HOST_LOOK=$HOST_LOOK python3 "$ROOT/scripts/rvnet-check.py" "$OUT" "$PLAYERS" "$SCENARIO"
