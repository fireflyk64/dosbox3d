#!/bin/bash
# tests/release/gate.sh TAG "MESSAGE": the regression, and the full deploy (deploy.sh) only if every part of it is
# clean.  Logs in $RV_RELEASE_DIR (/tmp/rv-release).  Never edit the worktree while it runs: the deploy builds from it.
TAG=$1; MSG=$2
[ -n "$TAG" ] && [ -n "$MSG" ] || { echo 'usage: tests/release/gate.sh TAG "MESSAGE"'; exit 2; }
S=${RV_RELEASE_DIR:-/tmp/rv-release}; mkdir -p $S
R=$(cd "$(dirname "$0")" && pwd)   # (these scripts)
cd /home/sandbox/dev/rv-gx/retrovega
export DOSBOX=/home/sandbox/dev/rv-gx/build-native/src/dosbox
TIMEOUT=900 tests/play/playtest.sh --dos > $S/play-dos.log 2>&1
P=$?
echo "play --dos rc $P"; grep '^playtest: [0-9]* of' $S/play-dos.log | cut -c1-150; tail -3 $S/play-dos.log | grep hashes | cut -c1-90
NOBUILD=1 TIMEOUT=300 tests/host/twins_test.sh > $S/twins.log 2>&1; tail -1 $S/twins.log | cut -c1-110
tests/quiet.sh > $S/quiet.log 2>&1
Q=$?
echo "quiet rc $Q"; tail -8 $S/quiet.log | cut -c1-105
GX=1 EXE=RVT.EXE NOBUILD=1 TIMEOUT=150 tests/run.sh tests/gxcapital.txt /capital /capblocks 4 /bench 0 > $S/cap.log 2>&1
grep -a -E "gx: this|^MEMORY" $S/cap.log | cut -c1-120 | head -4
if [ $P -ne 0 ] || [ $Q -ne 0 ] || ! grep -q "TWINS OK" $S/twins.log; then
  echo "NOT DEPLOYED: the regression is not clean"
  exit 1
fi
unset DOSBOX
bash $R/deploy.sh "$TAG" "$MSG"
