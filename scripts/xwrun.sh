#!/bin/bash
# X-Wing Collector's CD headless, one machine (docs/xwing.md): a copy of the installed game's hard-disk files as C:,
# the CD's files as D: (the game looks for \bwing.exe on a drive), BWING.EXE run, a key script from boot; the log and
# the pictures land in $OUT.
#
#   scripts/xwrun.sh [seconds] [the key script]
#
#   XWDIR=    the installed game's hard-disk files (default ~/dev/dosbox3d/xwingcd/XWING)
#   XWCD=     the CD's files as a folder, instead of the image
#   OPTS=     an OPTIONS.CFG to fly with instead of the folder's
#   FRESH=1   the game's copy made anew
#   XWISO=    the CD as an image (default ~/dev/dosbox3d/xwingcd/xwing.iso)
#   DOSBOX=   the emulator (default build-native/src/dosbox)
#   CYCLES=   the emulated CPU (default 30000)
#   ENV=      more of the environment
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/xwrun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
[ -n "$FRESH" ] && rm -rf $OUT/game           # (a new copy: no pilot of the run before)
DUR=${1:-40}; SCRIPT=${2:-}
# "tod1" first in the key script: the keys and clicks from boot to Tour of Duty I's first mission with a new pilot
# (scripts/xwroute-tod1.txt: in flight some 112 s in; wants FRESH=1)
# "trench" first: Historical Combat, the B-Wing's list (its first mission: the later ones are locked for a pilot who has
# not flown them; ENV="WCNET_XW_MISSION=\\mission\\t5h6mb.xwi" makes it the sixth, the Death Star's trench), in flight
# some 130 s in.  "hist" first: the same to Historical Combat's mission as the roster's second pilot last flew it (in flight some 95 s in;
# with ENV="WCNET_XW_MISSION=\\mission\\t5h6mb.xwi" any mission: that one the B-wing's trench run)
case "$SCRIPT" in tod1*) SCRIPT="$(cat "$ROOT/scripts/xwroute-tod1.txt")${SCRIPT#tod1}";; hist*) SCRIPT="$(cat "$ROOT/scripts/xwroute-hist.txt")${SCRIPT#hist}";;
  trench*) SCRIPT="$(cat "$ROOT/scripts/xwroute-trench.txt")${SCRIPT#trench}";; esac
XW=${XWDIR:-$HOME/dev/dosbox3d/xwingcd/XWING}
# (the CD: an image by default.  ~/dev/dosbox3d/xwingcd/XWNOCP/CD is a copy whose forward cockpits are blank)
[ -z "$XWCD" ] && [ -z "$XWISO" ] && XWISO=$HOME/dev/dosbox3d/xwingcd/xwing.iso
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
