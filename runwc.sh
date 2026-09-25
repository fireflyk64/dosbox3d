#!/bin/bash
# Wing Commander multiplayer launcher.
#
#   ./runwc.sh DOSPATH             host a game over TCP on port $WCPORT (13255)
#   ./runwc.sh DOSPATH HOST        join the TCP game hosted at HOST
#   ./runwc.sh DOSPATH room CODE   meet by room code through lobbylink: works
#                                  through firewalls and NAT; the first player
#                                  into the room hosts the game, the rest join
#
# Room codes are 4-64 letters, digits, - or _.  Inside the game the same
# things are available as DOS commands: WCNET ROOM code, WCNET CONNECT host,
# WCNET STARTSERVER, WCNET STATUS, WCNET DISCONNECT.
#
# Environment: WCPORT (TCP port), WCLOBBY (lobby server URL, default
# https://pqrstuvw.xyz/lobbylink), WCPLAYERS (room size, default 3),
# WCLOBBY_RELAY=1 (force the TURN relay), WCCALLSIGN / WCLASTNAME (pilot
# identity), WCNET_LOG=0..3 (log verbosity).
DOSPATH="$1"

case "$2" in
    room)
        if [ -z "$3" ]; then
            echo "usage: $0 DOSPATH room CODE" >&2
            exit 2
        fi
        unset WCHOST
        export WCROOM="$3"
        ;;
    "")
        ;;
    *)
        export WCHOST="$2"
        ;;
esac

exec "$(dirname "$0")/src/dosbox" -c "mount c $DOSPATH" -c "c:" -c "wc"
