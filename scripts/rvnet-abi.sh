#!/bin/bash
# An RV.EXE of another interface is never hooked: copies of the game with its RVHOOK version, then one word of its
# fixed block's directory, changed, each run as the host of a room (WCRVNET=1, WCPORT): the layer must say
# "RV ABI mismatch" and open no session.  retrovega/docs/multiplayer.md, section 2.
#   scripts/rvnet-abi.sh          (OUT, DOSBOX, GAME, PORT as scripts/rvnet-duo.sh)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/rvnet-abi}
PORT=${PORT:-13953}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
GAME=${GAME:-$ROOT/retrovega/build/dos}
rm -rf "$OUT"
fail=0
for what in version directory; do
    dir="$OUT/$what"
    mkdir -p "$dir/game"
    for f in "$GAME"/*; do
        case "$(basename "$f")" in
        RV.EXE|RVLOG.TXT|*.BMP|BUNK*.SAV|RVSETUP.CFG|WALK.TXT|*.PPM) ;;
        *) ln -s "$f" "$dir/game/" ;;
        esac
    done
    # the copy, changed where the layer looks (offsets from the generated header, as the layer has them)
    python3 - "$GAME/RV.EXE" "$dir/game/RV.EXE" "$what" "$ROOT/src/cpu/wcnet_rv.h" <<'EOF'
import re, struct, sys
src, dst, what, header = sys.argv[1:]
d = {m.group(1): int(m.group(2), 0) for m in re.finditer(r'#define (RV_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b', open(header).read())}
b = bytearray(open(src, 'rb').read())
image = struct.unpack_from('<H', b, 8)[0] * 16                  # the MZ header's paragraphs
assert b[image:image + 6] == b'RVHOOK'
if what == 'version':
    struct.pack_into('<H', b, image + d['RV_HOOK_VERSION'], 0xFFFF)
else:
    # DGROUP's paragraph from RVHOOK's word, as DOS would relocate it (the image at segment 0)
    ds = image + struct.unpack_from('<H', b, image + d['RV_HOOK_DGROUP'])[0] * 16
    assert b[ds + d['RV_PUPPET_HEADER']:ds + d['RV_PUPPET_HEADER'] + 8] == b'RVPUPPET'
    at = ds + d['RV_DIRECTORY'] + 2 * 5                             # the sixth item's offset, one word off
    struct.pack_into('<H', b, at, struct.unpack_from('<H', b, at)[0] + 2)
open(dst, 'wb').write(b)
EOF
    sed -n '/^\[autoexec\]/q;p' "$ROOT/retrovega/tests/rv.conf" > "$dir/run.conf"
    printf '[autoexec]\nmount c "%s"\nc:\nRV.EXE /fly 0 /noquit\nexit\n' "$dir/game" >> "$dir/run.conf"
    (cd "$dir" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCRVNET=1 WCPORT="$PORT" WCNET_LOG=2 \
        timeout 12 "$DOSBOX" -conf "$dir/run.conf" > "$dir/dosbox.log" 2>&1) || true
    if grep -q 'RV ABI mismatch' "$dir/dosbox.log" && ! grep -q 'SERVER\|waiting for a client\|CLIENT' "$dir/dosbox.log"; then
        echo "rvnet-abi: OK ($what): $(grep -o 'RV ABI mismatch.*' "$dir/dosbox.log" | head -1)"
    else
        echo "rvnet-abi: FAIL ($what): no mismatch said, or a session opened (see $dir/dosbox.log)"
        fail=1
    fi
done
exit $fail
