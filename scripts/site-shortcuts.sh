#!/bin/bash
# The site's short addresses for a game: fireflyk64.github.io/tv and the like, each a page that sends the browser on to
# the multiplayer page with that game asked for (dosbox/?game=..., web/wc.js askedGame), whatever else the address
# carried (?room= and so on) kept.  (The user, 2026-11-17: "deployed shortcuts like fireflyk64.github.io/wc or /tfd or
# /tf or /tv or /raptor that redirect to dosbox but with that game selected".)  /rv is the native RetroVega page and
# /retrovega its blog: RetroVega's short address is /vega.
#
#   scripts/site-shortcuts.sh PAGES_REPO_DIR
set -eu
SITE="$1"
while read -r name game title; do
    [ -e "$SITE/$name" ] && [ ! -f "$SITE/$name/.shortcut" ] && { echo "$SITE/$name exists and is no shortcut: left alone" >&2; continue; }
    mkdir -p "$SITE/$name"
    : > "$SITE/$name/.shortcut"
    cat > "$SITE/$name/index.html" <<HTML
<!doctype html>
<meta charset="utf-8">
<title>$title, multiplayer in the browser</title>
<meta http-equiv="refresh" content="0; url=../dosbox/?game=$game">
<script>
  var q = new URLSearchParams(location.search); q.set("game", "$game");
  location.replace("../dosbox/?" + q.toString() + location.hash);
</script>
<p><a href="../dosbox/?game=$game">$title, multiplayer in the browser</a></p>
HTML
    echo "$name -> dosbox/?game=$game"
done <<LIST
wc wc1 Wing Commander
wc2 wc2 Wing Commander II
tf tf TIE Fighter
tfd tiedemo TIE Fighter (the 1994 demo)
tv tv Terminal Velocity
raptor rap Raptor
privateer priv Privateer
vega rv RetroVega
LIST
