#!/bin/bash
# tools/anaxander.sh [ASSETS]: Vega Strike's Anaxander (an Aeran hammer-head cruiser, GPL with Vega Strike's assets)
# into the local assets (default ~/dev/vegastrike-assets) as units/vessels/Anaxander/anaxander.obj and its maps, for
# tools/render_all.py (the user, 2026-11-14: "Look for the hammer head capital ship ... this is the one.  Should be the
# healthy equivalent": data/ships.csv ANAXIDM's mesh).  Its source: github.com/vegastrike/Assets-Masters,
# units/vessels/Anaxander (a Maya model exported to COLLADA in 2010, its 1024-texel colour and specular maps), never in
# the game's data.  The build itself needs none of this: the views it makes are packed into art/ships.
set -euo pipefail
ASSETS=${1:-$HOME/dev/vegastrike-assets}
URL=https://raw.githubusercontent.com/vegastrike/Assets-Masters/master/units/vessels/Anaxander
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for f in anaxander.dae Anaxander_COLOR.tga Anaxander_SPM2.tga; do
    curl -fsSL -o "$TMP/$f" "$URL/$f"
done
OUT="$ASSETS/units/vessels/Anaxander"
mkdir -p "$OUT"
python3 -I "$(dirname "$0")/dae2obj.py" "$TMP/anaxander.dae" "$OUT/anaxander.obj" anaxander anaxander.png
printf 'map_Ks anaxander_spec.png\n' >> "$OUT/anaxander.mtl"
"${VENV:-$HOME/.venvs/retrovega-art}/bin/python" -P -c "
import sys
from PIL import Image
Image.open(sys.argv[1]).convert('RGB').save(sys.argv[3])
Image.open(sys.argv[2]).convert('RGB').save(sys.argv[4])" "$TMP/Anaxander_COLOR.tga" "$TMP/Anaxander_SPM2.tga" \
    "$OUT/anaxander.png" "$OUT/anaxander_spec.png"
echo "the Anaxander in $OUT"
