#!/bin/bash
# Raptor: Call of the Shadows headless, one machine: a copy of the game (its SETUP.INI written as SETUP.EXE writes it:
# a Sound Blaster for the music and the effects), RAP.EXE run, the key script from boot; the log and the pictures land
# in $OUT (default /tmp/raprun).  docs/raptor.md.
#
#   scripts/raprun.sh [seconds] [the key script]
#
#   RAPDIR=     the game (the shareware's files unpacked: RAP.EXE, FILE0000.GLB, FILE0001.GLB; default ~/dev/rap)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   CYCLES=     the emulated CPU (default 30000)
#   ENV=        more of the environment ("WCPORT=7400", "WCHOST=127.0.0.1")
#   KEEP=1      keep $OUT/game from an earlier run (its saved pilots)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/raprun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-60}; SCRIPT=${2:-}
RAPDIR=${RAPDIR:-$HOME/dev/rap}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
[ -f "$RAPDIR/RAP.EXE" ] || { echo "no Raptor at $RAPDIR (unpack the shareware's RAPSW12.SHR there)"; exit 2; }
if [ -z "${KEEP:-}" ] || [ ! -d $OUT/game ]; then
  rm -rf $OUT/game; mkdir -p $OUT/game; cp -f "$RAPDIR"/* $OUT/game/ 2>/dev/null
fi
[ -f $OUT/game/SETUP.INI ] || printf '[Setup]\r\nDetail=1\r\nControl=0\r\n[Music]\r\nCardType=5\r\nBasePort=220\r\nIrq=7\r\nDma=1\r\nVolume=127\r\n[SoundFX]\r\nCardType=5\r\nBasePort=220\r\nIrq=7\r\nDma=1\r\nChannels=4\r\nVolume=127\r\n' > $OUT/game/SETUP.INI
cat > $OUT/rap.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=16
[cpu]
core=normal
cycles=fixed ${CYCLES:-30000}
[sblaster]
sbtype=sb16
irq=7
dma=1
hdma=5
[autoexec]
@echo off
mount c $OUT/game
c:
${PROG:-rap}
CONF
by_time() { tr ',' '\n' | grep -v '^$' | sort -t: -k1,1 -g -s | paste -sd, -; }
script="$(echo "$SCRIPT" | by_time)"
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$script" timeout -s KILL $DUR $DOSBOX -conf $OUT/rap.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "Raptor" $OUT/run.err | cut -c1-220 | tail -${SHOW:-8}
