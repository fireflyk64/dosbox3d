#!/bin/bash
# X-Wing Collector's CD headless, one machine (docs/xwing.md): a copy of the installed game's hard-disk files as C:,
# the CD's files as D: (the game looks for \bwing.exe on a drive), BWING.EXE run, a key script from boot; the log and
# the pictures land in $OUT.
#
#   scripts/xwrun.sh [seconds] [the key script]
#
#   XWDIR=    the installed game's hard-disk files (default ~/dev/dosbox3d/xwingcd/XWING)
#   XWCD=     the CD's files (default XWDIR/CD, else ~/dev/dosbox3d/xwingcd/XWNOCP/CD)
#   OPTS=     an OPTIONS.CFG to fly with instead of the folder's
#   XWISO=    the CD as an image instead of XWDIR/CD
#   DOSBOX=   the emulator (default build-native/src/dosbox)
#   CYCLES=   the emulated CPU (default 30000)
#   ENV=      more of the environment
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/xwrun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-40}; SCRIPT=${2:-}
XW=${XWDIR:-$HOME/dev/dosbox3d/xwingcd/XWING}
XWCD=${XWCD:-$XW/CD}; [ -d "$XWCD" ] || XWCD=$HOME/dev/dosbox3d/xwingcd/XWNOCP/CD
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
if [ ! -f $OUT/game/BWING.EXE ]; then
  mkdir -p $OUT/game
  find "$XW" -maxdepth 1 -type f -exec cp {} $OUT/game/ \;
fi
[ -n "$OPTS" ] && cp -f "$OPTS" $OUT/game/OPTIONS.CFG
if [ -n "$XWISO" ]; then CD="imgmount d \"$XWISO\" -t iso"; else CD="mount d \"$XWCD\" -t cdrom"; fi
cat > $OUT/xw.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=${MACHINE:-svga_s3}
memsize=${MEMSIZE:-16}
[cpu]
core=normal
cycles=fixed ${CYCLES:-30000}
[dos]
ems=${EMS:-true}
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
$CD
c:
bwing
CONF
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$SCRIPT" timeout -s KILL $DUR $DOSBOX -conf $OUT/xw.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "X-Wing\|xwing" $OUT/run.err | cut -c1-200 | tail -12
