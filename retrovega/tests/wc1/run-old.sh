#!/bin/bash
# The old game flown alone by the multiplayer layer (WCNET=0: no network, the mission picked by SERIES / MIS), its
# flight traced (WCNET_SHIPTRACE) and a key script run in space: for comparing its AI with ours (README.md).
#   tests/wc1/run-old.sh OUTDIR SERIES MIS "KEYSCRIPT" [SECONDS]
# e.g. the first mission, the player flown to nav 1 by the autopilot and left there, invulnerable, for two minutes:
#   tests/wc1/run-old.sh /tmp/enyo1 1 0 "1:!rocks=0,3:a,10:@nav1,130:@end" 260
# OUTDIR gets trace.txt, the key script's dumps (@tag -> tag.bin, read by dsdump.py) and screenshots (!shot=name).
# Needs dosbox3d's DOSBox with the multiplayer layer (BIN=; tests/dosbox.sh finds the tree's build-native) and
# Wing Commander 1 (WC=, default the tree's wc/).
set -u
OUT=$1; SERIES=$2; MIS=$3; KEYS=$4; SECS=${5:-260}
[ -n "${BIN:-}" ] || { . "$(dirname "$0")/../dosbox.sh"; BIN=$DOSBOX; }
WC=${WC:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)/wc}
[ -d "$WC" ] || { echo "no Wing Commander 1 at $WC: set WC=its folder" >&2; exit 2; }
mkdir -p "$OUT"
cd "$OUT" && env WCNET=0 WCNET_AUTOKEYS=1 SERIES=$SERIES MIS=$MIS WCNET_LOG=2 WCCALLSIGN=CHEATMAV \
    WCNET_SHIPTRACE="$OUT/trace.txt" WCNET_KEYSCRIPT="$KEYS" WCNET_DUMP_DIR="$OUT" \
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL "$SECS" "$BIN" \
    -c "mount c $WC" -c "c:" -c "wc" -c "exit" > out.txt 2> err.txt
echo "trace: $(grep -c '^S' "$OUT/trace.txt" 2>/dev/null || echo 0) ship lines in $OUT/trace.txt"
