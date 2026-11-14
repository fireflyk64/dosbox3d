#!/bin/bash
# Terminal Velocity headless, one machine: booted from TV.EXE, through the menus into the first level of episode one
# (New Game, Tactical Strike, Normal, the pilot "ace"/"ace"), then the extra key script; the log and the pictures land
# in $OUT (default /tmp/tvrun).  docs/terminalvelocity.md section 4.
#
#   scripts/tvrun.sh [seconds] [the extra key script]
#
#   TVDIR=      the game (the shareware's files unpacked, set up: SOUNDRV.COM, SETUP.CFG; default tvgame/ in this tree)
#   DOSBOX=     the emulator (default build-native/src/dosbox)
#   NOROUTE=1   no route: the extra script alone, from boot
#   CYCLES=     the emulated CPU (default 30000)
#   ENV=        more of the environment ("WCPORT=7400", "WCLAB=/tmp/lab": picture and sound recorded)
#   COPY=1      the game's files copied into $OUT/game first (two machines side by side: each its own VOX.INI and saves)
#   BRIEF=1     the level briefings and cinematics as the game has them (the copy's VOX.INI: off by default, the route's
#               timing assumes them off: in flight at ~62 s)
#   PROG=       what runs (default "tv")
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/tvrun}; mkdir -p $OUT; rm -f $OUT/*.ppm $OUT/*.txt
DUR=${1:-90}; EXTRA=${2:-}
TVDIR=${TVDIR:-$ROOT/tvgame}
DOSBOX=${DOSBOX:-$ROOT/build-native/src/dosbox}
[ -f "$TVDIR/GAME.EXE" ] || { echo "no Terminal Velocity at $TVDIR (unpack the shareware there, see docs)"; exit 2; }
if [ -n "${COPY:-}" ]; then
  mkdir -p $OUT/game; cp -f "$TVDIR"/* $OUT/game/ 2>/dev/null
  TVDIR=$OUT/game
  if [ -z "${BRIEF:-}" ] && [ -f $TVDIR/VOX.INI ]; then
    python3 - "$TVDIR/VOX.INI" <<'PY'
import sys
p = sys.argv[1]; lines = open(p, newline='').read().split('\r\n')
for i, l in enumerate(lines):
    if l.startswith('difficulty,briefingFlag') and i + 1 < len(lines):
        v = lines[i + 1].split(','); v[1] = '0'; v[2] = '0'; v[3] = '0'; lines[i + 1] = ','.join(v)
open(p, 'w', newline='').write('\r\n'.join(lines))
PY
  fi
fi
cat > $OUT/tv.conf <<CONF
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
[autoexec]
@echo off
mount c $TVDIR
c:
${PROG:-tv}
CONF
# The route (from boot, briefings off): Play Game at 38 s, New Game 42, episode one 46, Normal 50, the name and call
# sign 52-56, the story's page 61: in flight at ~62 s.
route="38:enter,42:enter,46:enter,50:enter,52:a,52.3:c,52.6:e,53:enter,55:a,55.3:c,55.6:e,56:enter,61:enter"
[ -n "${NOROUTE:-}" ] && route=""
by_time() { tr ',' '\n' | grep -v '^$' | sort -t: -k1,1 -g -s | paste -sd, -; }
script="$(echo "$route${EXTRA:+,$EXTRA}" | by_time)"
( cd $OUT && env ${ENV:-} SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy WCNET_LOG=${WCNET_LOG:-2} WCNET_KEYSCRIPT_FROM=boot \
    WCNET_DUMP_DIR=$OUT WCNET_KEYSCRIPT="$script" timeout -s KILL $DUR $DOSBOX -conf $OUT/tv.conf > $OUT/run.out 2> $OUT/run.err )
grep -a "Terminal Velocity" $OUT/run.err | grep -v ' 0 frames' | cut -c1-220 | tail -${SHOW:-8}
