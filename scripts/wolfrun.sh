#!/bin/bash
# Wolfenstein 3-D headless, one machine: a copy of the game, the program run, the key script from boot; the log and
# the pictures land in $OUT (default /tmp/wolfrun).  docs/wolf3d.md.
#
#   scripts/wolfrun.sh [seconds] [the key script]
#
#   WOLFDIR=    the game (the shareware's files unpacked: WOLF3D.EXE, *.WL1; default ~/dev/doom/wolfgame)
#   PROG=       the command run (default "wolf3d tedlevel 0 normal": straight into the first level, as the page's
#               rooms run it; "wolf3d": the game by itself, its title screens and menus)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   CYCLES=     the emulated CPU (default 20000)
#   ENV=        more of the environment ("WCPORT=7500", "WCHOST=127.0.0.1")
#   KEEP=1      keep $OUT/game from an earlier run (its CONFIG.WL1, its saved games)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/wolfrun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-60}; SCRIPT=${2:-}
WOLFDIR=${WOLFDIR:-$HOME/dev/doom/wolfgame}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
ls "$WOLFDIR"/VSWAP.* > /dev/null 2>&1 || { echo "no Wolfenstein 3-D at $WOLFDIR (the shareware's W3D1_BBS._1 is a zip: unpack it there)"; exit 2; }
if [ -z "${KEEP:-}" ] || [ ! -d $OUT/game ]; then
  rm -rf $OUT/game; mkdir -p $OUT/game; cp -f "$WOLFDIR"/* $OUT/game/ 2>/dev/null
fi
cat > $OUT/wolf.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=16
[cpu]
core=normal
cycles=fixed ${CYCLES:-20000}
[sblaster]
sbtype=sb16
irq=7
dma=1
hdma=5
[autoexec]
@echo off
mount c $OUT/game
c:
${PROG:-wolf3d tedlevel 0 normal}
CONF
by_time() { tr ',' '\n' | grep -v '^$' | sort -t: -k1,1 -g -s | paste -sd, -; }
script="$(echo "$SCRIPT" | by_time)"
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$script" timeout -s KILL $DUR $DOSBOX -conf $OUT/wolf.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "Wolf" $OUT/run.err | cut -c1-220 | tail -${SHOW:-8}
