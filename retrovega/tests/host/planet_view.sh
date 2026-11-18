#!/bin/bash
# tests/host/planet_view.sh MISSION [OUTDIR]: a mission's planet as the card makes it, from the packed game data
# (build/dos/RVGAME.DAT, made first by tools/rvdata.py), drawn from its nav points (planet_view.c): the times, where
# the nav points went, how far the lines between them clear the ground, PNGs and a contact sheet.
#   tests/host/planet_view.sh --map OUT.PNG SEED RADIUS CX CY CZ X0 Z0 [SPAN]: the seed's ground from over the world's +y
cd "$(dirname "$0")/../.."
SCR=${TMPDIR:-/tmp}/rv-planet-view
mkdir -p "$SCR"
python3 -c "
import json;d=json.load(open('data/palette.json'))['rgb'];open('$SCR/pal.bin','wb').write(bytes(v>>2 for c in d for v in c))" || exit 1
gcc -O2 -std=gnu99 -Wall -Wextra -o "$SCR/planet_view" tests/host/planet_view.c src/terrain.c -lm || exit 1
if [ "$1" = --map ]; then
    OUT=$2; shift 2
    "$SCR/planet_view" --map "$SCR/pal.bin" "$SCR/map.ppm" "$@" || exit 1
    python3 -c "from PIL import Image;Image.open('$SCR/map.ppm').save('$OUT')" && echo "map: $OUT"
    exit 0
fi
MISSION=${1:?mission id}; OUT=${2:-/tmp/rv-planet-$MISSION}
mkdir -p "$OUT"
python3 tools/rvdata.py >/dev/null || exit 1
"$SCR/planet_view" build/dos/RVGAME.DAT "$SCR/pal.bin" "$OUT" "$MISSION" || exit 1
python3 - "$OUT" <<'PY' 2>/dev/null || echo "(no PIL: the pictures stay PPMs)"
import sys, glob, os
from PIL import Image
out = sys.argv[1]
names = sorted(glob.glob(os.path.join(out, '[0-9]*.ppm')))
ims = [Image.open(n) for n in names]
for n, im in zip(names, ims):
    im.save(n[:-4] + '.png')
w, h = ims[0].size
sheet = Image.new('RGB', (2 * w + 10, ((len(ims) + 1) // 2) * (h + 10)), (30, 30, 30))
for k, im in enumerate(ims):
    sheet.paste(im, ((k % 2) * (w + 10), (k // 2) * (h + 10)))
sheet.save(os.path.join(out, 'sheet.png'))
print('sheet:', os.path.join(out, 'sheet.png'))
PY
