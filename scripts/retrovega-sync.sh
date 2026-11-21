#!/bin/sh
# RetroVega's own repository brought up to date: the commits made here since the last export (the ones that changed
# retrovega/), added on top of it, then pushed to every remote it has (github.com/fireflyk64/retrovega).  Run it any
# time, from anywhere; with nothing new it only pushes.  Development goes on here, in dosbox3d: this repository is the
# source of truth, the other a copy that only ever grows.
#   scripts/retrovega-sync.sh               into ~/dev/retrovega (RV_EXPORT=another)
#   scripts/retrovega-sync.sh --ref BRANCH  up to another branch's tip than this checkout's HEAD
# (scripts/retrovega-export.py has the details; the RetroVega deploy, scripts/rv-release/deploy.sh, runs it too)
exec python3 -I "$(dirname "$0")/retrovega-export.py" "${RV_EXPORT:-$HOME/dev/retrovega}" --push "$@"
