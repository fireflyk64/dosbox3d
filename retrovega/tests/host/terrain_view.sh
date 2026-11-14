#!/bin/bash
# tests/host/terrain_view.sh [OUTDIR] [SEED]: the desert planet made and drawn from eight places without the game
# (terrain_view.c), the times printed, the pictures as PNGs and a contact sheet.  Needs python3 with PIL for the PNGs.
cd "$(dirname "$0")/../.."
OUT=${1:-/tmp/rv-terrain}; SEED=${2:-1}
mkdir -p "$OUT"
python3 -c "
import json;d=json.load(open('data/palette.json'))['rgb'];open('$OUT/pal.bin','wb').write(bytes(v>>2 for c in d for v in c))" || exit 1
gcc -O2 -std=gnu99 -Wall -Wextra -o "$OUT/terrain_view" tests/host/terrain_view.c src/terrain.c -lm || exit 1
"$OUT/terrain_view" "$OUT/pal.bin" "$OUT" "$SEED" || exit 1
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
