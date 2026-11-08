#!/bin/bash
# Privateer headless, one machine: booted, the opening skipped, a new pilot registered, launched from Achilles
# (Troy), then the extra key script; the log and the pictures land in $OUT (default /tmp/privrun).  docs/privateer.md.
#
#   scripts/privrun.sh [seconds] [the extra key script]
#
#   PRIVDIR=    the game as GOG's installer unpacks it (PRCD.EXE, GAME.GOG beside it; default privgame/ in this tree)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   RF=1        Righteous Fire ("priv r")
#   NOROUTE=1   no route: the extra script alone, from boot
#   CYCLES=     the emulated CPU (default 30000)
#   ENV=        more of the environment ("WCLAB=/tmp/lab": picture and sound recorded)
#
# The route (from boot): Esc skips the opening at 4 s; NEW on the title (0.14, 0.94) at 8 s; the name "joe" and the
# callsign "ace" typed at 10-12.5 s; the pad's ship (0.45, 0.6) clicked at 16 s: in flight near Achilles at ~22 s.
# Pictures: "!frame=<name>" (what the window showed, the emulator's lines on it) or "!shot=<name>".
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/privrun}; mkdir -p $OUT; rm -f $OUT/*.ppm
DUR=${1:-40}; EXTRA=${2:-}
PRIVDIR=${PRIVDIR:-$ROOT/privgame}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
[ -f "$PRIVDIR/PRCD.EXE" ] || { echo "no Privateer at $PRIVDIR (innoextract GOG's installer there)"; exit 2; }
IMG=$(ls "$PRIVDIR" | grep -i '^game.gog$' | head -1)
RUN="priv.exe"; [ -n "${RF:-}" ] && RUN="priv.exe r"
# GOG's own machine (__support/app/dosboxPRIV.conf): an S3, 16 MB, EMS and XMS, a Sound Blaster 16 on IRQ 5
cat > $OUT/priv.conf <<CONF
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
irq=5
dma=1
hdma=5
[dos]
xms=true
ems=true
umb=true
[autoexec]
@echo off
mount c $PRIVDIR
imgmount d $PRIVDIR/$IMG -t iso
c:
$RUN
CONF
route="4:esc,8:!mouse=0.14:0.94,8.3:!button=0:1,8.6:!button=0:0,10:j,10.3:o,10.6:e,11:enter,11.5:a,11.8:c,12.1:e,12.5:enter"
route="$route,15:!mouse=0.45:0.6,16:!button=0:1,16.3:!button=0:0"
[ -n "${NOROUTE:-}" ] && route=""
by_time() { tr ',' '\n' | grep -v '^$' | sort -t: -k1,1 -g -s | paste -sd, -; }
script="$(echo "$route${EXTRA:+,$EXTRA}" | by_time)"
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$script" timeout -s KILL $DUR $DOSBOX -conf $OUT/priv.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "Privateer" $OUT/run.err | cut -c1-200 | head -40
