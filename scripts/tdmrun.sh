#!/bin/bash
# The TIE Fighter demo headless, one machine (docs/tiedemo.md): a copy of the game unpacked from its floppies
# (scripts/tiedemo-unpack.mjs; LANDRU.CFG and OPTIONS.CFG as its installer writes them), TIEDEMO.BAT run, the key
# script from boot (Esc at 8 s skips the opening: in flight by ~12 s); the log and the pictures land in $OUT.
#
#   scripts/tdmrun.sh [seconds] [the key script]
#
#   TDMDISKS=   the floppies unpacked (tiedem1.zip and tiedem2.zip, side by side in d1/ and d2/; default
#               ~/dev/tf-demo-dl/z)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   CYCLES=     the emulated CPU (default 30000)
#   ENV=        more of the environment ("WCPORT=7400", "WCHOST=127.0.0.1")
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/tdmrun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-40}; SCRIPT=${2:-8:esc}
DISKS=${TDMDISKS:-$HOME/dev/tf-demo-dl/z}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
if [ ! -f $OUT/game/TIEMISS.EXE ]; then
  mkdir -p $OUT/game
  node "$ROOT/scripts/tiedemo-unpack.mjs" "$DISKS/d1" "$DISKS/d2" $OUT/game > /dev/null || exit 2
fi
printf 'adlib_w\r\nFAST\r\nems\r\ndiff\r\n' > $OUT/game/LANDRU.CFG
cp -f $OUT/game/FASTYES.CFG $OUT/game/OPTIONS.CFG
cat > $OUT/tdm.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=16
[cpu]
core=normal
cycles=fixed ${CYCLES:-30000}
[dos]
ems=true
xms=true
umb=true
[sblaster]
sbtype=sb16
irq=7
dma=1
hdma=5
[autoexec]
@echo off
mount c $OUT/game
c:
tiedemo
CONF
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$SCRIPT" timeout -s KILL $DUR $DOSBOX -conf $OUT/tdm.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "TIE demo" $OUT/run.err | grep -v ": [0-9.]*s: " | cut -c1-200 | tail -12
