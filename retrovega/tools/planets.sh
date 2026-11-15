#!/bin/sh
# The planet missions' grounds baked (tools/planetbake.c, src/bake.h): build/planets/ID.PLN, one a planet mission of
# RVGAME.DAT's, each made again only when its mission's records or the code that makes it change (some 3 s a planet),
# and linked beside the game's data.
#   tools/planets.sh DIR [planetbake's options]     DIR has RVGAME.DAT; the files are linked into it
set -e
cd "$(dirname "$0")/.."
DIR=$1
shift
mkdir -p build/host build/planets
B=build/host/planetbake
# the baker, built again when its sources change (rvdata.h is written by every build: its sum, not its date)
SUM=$(cat src/terrain.c src/terrain.h src/bake.h src/bake.c tools/planetbake.c src/rvdata.h src/rvtypes.h | cksum | cut -d' ' -f1)
if [ ! -x "$B" ] || [ "$(cat "$B.sum" 2> /dev/null)" != "$SUM" ]; then
    gcc -std=gnu99 -O2 -Wall -I src -o "$B" tools/planetbake.c src/bake.c -lm
    echo "$SUM" > "$B.sum"
fi
# a file's stamp: the sum of what makes a ground (not rvdata.h: a mission's own records are the file's key)
STAMP=$(cat src/terrain.c src/terrain.h src/bake.h src/bake.c tools/planetbake.c | cksum | cut -d' ' -f1)
"$B" "$DIR/RVGAME.DAT" build/planets --stamp "$STAMP" --quiet "$@"
for f in build/planets/*.PLN; do
    [ -e "$f" ] || continue
    [ "$f" -ef "$DIR/$(basename "$f")" ] || ln -f "$f" "$DIR/"
done
