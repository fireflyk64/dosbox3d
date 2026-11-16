#!/bin/bash
# DOOM headless, one machine: a copy of the game (its DEFAULT.CFG written as SETUP.EXE writes it when there is none:
# a Sound Blaster for the music and the effects, the keyboard and the mouse), the program run, the key script from
# boot; the log and the pictures land in $OUT (default /tmp/doomrun).  docs/doom.md.
#
#   scripts/doomrun.sh [seconds] [the key script]
#
#   DOOMDIR=    the game (the shareware's files unpacked: DOOM.EXE, DOOM1.WAD; default ~/dev/doom/doomgame)
#   PROG=       the command run (default "doomnet": the layer's launcher, which runs DOOM.EXE as a network game when
#               there is a room and alone when there is none; "doom -warp 1 1": the game by itself)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   CYCLES=     the emulated CPU (default 50000)
#   ENV=        more of the environment ("WCPORT=7500", "WCHOST=127.0.0.1")
#   KEEP=1      keep $OUT/game from an earlier run (its saved games)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/doomrun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-60}; SCRIPT=${2:-}
DOOMDIR=${DOOMDIR:-$HOME/dev/doom/doomgame}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
ls "$DOOMDIR"/*.WAD > /dev/null 2>&1 || { echo "no DOOM at $DOOMDIR (unpack the shareware's DOOMS_19.1 + DOOMS_19.2, a zip, there)"; exit 2; }
if [ -z "${KEEP:-}" ] || [ ! -d $OUT/game ]; then
  rm -rf $OUT/game; mkdir -p $OUT/game; cp -f "$DOOMDIR"/* $OUT/game/ 2>/dev/null
fi
[ -f $OUT/game/DEFAULT.CFG ] || printf 'mouse_sensitivity\t\t5\r\nsfx_volume\t\t8\r\nmusic_volume\t\t8\r\nshow_messages\t\t1\r\nuse_mouse\t\t1\r\nuse_joystick\t\t0\r\nscreenblocks\t\t10\r\ndetaillevel\t\t0\r\nsnd_channels\t\t8\r\nsnd_musicdevice\t\t3\r\nsnd_sfxdevice\t\t3\r\nsnd_sbport\t\t544\r\nsnd_sbirq\t\t7\r\nsnd_sbdma\t\t1\r\nsnd_mport\t\t816\r\nusegamma\t\t0\r\n' > $OUT/game/DEFAULT.CFG
cat > $OUT/doom.conf <<CONF
[sdl]
output=surface
[dosbox]
machine=svga_s3
memsize=16
[cpu]
core=normal
cycles=fixed ${CYCLES:-50000}
[sblaster]
sbtype=sb16
irq=7
dma=1
hdma=5
[autoexec]
@echo off
mount c $OUT/game
c:
${PROG:-doomnet}
CONF
by_time() { tr ',' '\n' | grep -v '^$' | sort -t: -k1,1 -g -s | paste -sd, -; }
script="$(echo "$SCRIPT" | by_time)"
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$script" timeout -s KILL $DUR $DOSBOX -conf $OUT/doom.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "DOOM" $OUT/run.err | cut -c1-220 | tail -${SHOW:-8}
