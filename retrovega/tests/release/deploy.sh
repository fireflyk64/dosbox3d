#!/bin/bash
# tests/release/deploy.sh TAG "MESSAGE" (gate.sh calls it): the worktree's HEAD, fully: the web build, a combat flight on the page, the smokes, and only if they pass the deploy
S=${RV_RELEASE_DIR:-/tmp/rv-release}; mkdir -p $S
R=$(cd "$(dirname "$0")" && pwd)   # (these scripts)
WT=/home/sandbox/dev/rv-gx; MAIN=/home/sandbox/dev/dosbox3d; PAGES=/home/sandbox/dev/fireflyk64.github.io
TAG=$1; MSG=$2
export PLAYWRIGHT_DIR=/home/sandbox/dev/beauty
cd $WT
. ~/emsdk/emsdk_env.sh > /dev/null 2>&1
JOBS=2 scripts/build-web.sh > $S/webbuild.log 2>&1 || { echo "WEB BUILD FAILED"; tail -5 $S/webbuild.log; exit 1; }
H=$(git rev-parse --short HEAD)
echo "HEAD $H; wasm: $(strings -n 8 web/dist/dosbox.wasm | grep -a 'built with' | grep -av SDL | head -1 | cut -c1-60)"
for f in wc.js rvsets.js gamefiles.js index.html; do diff -q <(git show HEAD:web/$f) web/dist/$f > /dev/null || { echo "$f DIFFERS FROM HEAD"; exit 1; }; done
python3 $R/reap.py 280 $R/webrig.sh $TAG-fight "env.WCNET_PERF=4"
tail -1 $S/web-$TAG-fight/out.txt
grep -a "gx: this\|flight: over\|killed by\|FAIL\|LEAK" $S/web-$TAG-fight/rv-flight-RVLOG.TXT | cut -c1-120
grep -a "perf:" $S/web-$TAG-fight/out.txt | sed -E 's/.*emulator at ([0-9]+)%.*/\1/' | tail -8 | tr '\n' ' '; echo
PORT=8793 LOBBY_PORT=8794 OUT=$S/smoke-$TAG-rv python3 $R/reap.py 500 scripts/web-smoke.sh rv > $S/smoke-$TAG-rv.log 2>&1
grep -a -q 'RV OK' $S/smoke-$TAG-rv.log || { echo "RV SMOKE FAILED"; tail -6 $S/smoke-$TAG-rv.log | cut -c1-140; exit 1; }
echo "rv smoke: $(grep -a 'RV OK' $S/smoke-$TAG-rv.log | cut -c1-40)"
PORT=8793 LOBBY_PORT=8794 GAME_SOURCE=gog GAME_FILE="$MAIN/setup_wing_commander_1.01_gog_v1_(76764).exe" OUT=$S/smoke-$TAG-wc python3 $R/reap.py 600 scripts/web-smoke.sh 90 > $S/smoke-$TAG-wc.log 2>&1
grep -a -q 'SMOKE OK' $S/smoke-$TAG-wc.log || { echo "WC SMOKE FAILED"; tail -6 $S/smoke-$TAG-wc.log | cut -c1-140; exit 1; }
echo "wc smoke: SMOKE OK"
ps -eo args | grep -E 'deploy-full|host/web/deploy' | grep -v grep | grep -v fulldeploy && { echo "ANOTHER DEPLOY IS RUNNING"; exit 1; }
git push -q origin retrovega-gx:retrovega || { echo "PUSH OF THE BRANCH FAILED"; exit 1; }
# (the main tree is other sessions' too: fast-forward only, never over their uncommitted work; if their edits overlap
# ours it stays where it was and says so, and the deploy goes on: the branch is pushed, the Pages come from this tree)
if git -C $MAIN merge -q --ff-only retrovega-gx 2> $S/ff-$TAG.log; then
  echo "branch: retrovega at $(git -C $MAIN rev-parse --short HEAD)"
else
  echo "branch: retrovega at $(git rev-parse --short HEAD) (pushed); the main tree left at $(git -C $MAIN rev-parse --short HEAD): $(grep -a -c '^	' $S/ff-$TAG.log) of its uncommitted files are ours too"
fi
git -C $PAGES pull -q --ff-only || { echo "PAGES PULL FAILED"; exit 1; }
for f in web/dist/*; do
  n=$(basename "$f")
  [ "$n" = wc.tar.gz ] && continue
  cp -r "$f" "$PAGES/dosbox/"
done
cd $PAGES
git add -A dosbox
git commit -q -m "dosbox: $MSG (from dosbox3d retrovega $H)"
git push -q origin main || { echo "PAGES PUSH FAILED"; exit 1; }
echo "pages: $(git log --oneline -1 | cut -c1-90)"
cd $WT
for i in $(seq 1 60); do
  if curl -sf "https://fireflyk64.github.io/dosbox/dosbox.wasm?t=$(date +%s)" | cmp -s - web/dist/dosbox.wasm && curl -sf "https://fireflyk64.github.io/dosbox/retrovega.tar.gz?t=$(date +%s)" | cmp -s - web/dist/retrovega.tar.gz; then
    echo "live /dosbox matches the build"; break
  fi
  python3 -c "import time;time.sleep(10)"
  [ $i = 60 ] && echo "LIVE /dosbox DOES NOT MATCH AFTER 10 MINUTES"
done
python3 $R/reap.py 300 node scripts/web-rv.mjs https://fireflyk64.github.io/dosbox/ https://pqrstuvw.xyz/lobbylink 20 > $S/live-$TAG-rv.log 2>&1
echo "live web-rv: $(grep -a 'RV OK' $S/live-$TAG-rv.log | cut -c1-40)"
for i in $(seq 1 40); do
  [ "$(gh run list -R fireflyk64/fireflyk64.github.io -L 1 --json status -q '.[0].status' 2>/dev/null)" = completed ] && break
  python3 -c "import time;time.sleep(10)"
done
cd $WT/retrovega
RUN="python3 $R/reap.py 500" host/web/deploy.sh "$MSG" > $S/deploy-$TAG-rv.log 2>&1
echo "rv deploy rc $?"
tail -4 $S/deploy-$TAG-rv.log | cut -c1-120
