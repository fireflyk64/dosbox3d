#!/bin/bash
# desktop/fetch-data.sh DIR: the game's data files from the published site (fireflyk64.github.io/rv), each download
# checked against the SHA-256 below before anything is unpacked: a different file is refused.  The site's files are
# made reproducibly (host/web/site.sh, tools/artweb.py): these hashes hold until the data itself changes.  For a
# machine without the DOS build (../build.sh); then
#   make data DATA=<the game's directory> FROM=DIR
# which adds the ships' sprite sets, made here from art/ships.  This is the only step that uses the network, and
# it only downloads: nothing fetched is run.  SITE=url fetches from another copy of the site (same hashes).
set -euo pipefail
[ $# = 1 ] || { echo "usage: desktop/fetch-data.sh DIR"; exit 2; }
dir=$1
site=${SITE:-https://fireflyk64.github.io/rv}
GAME_SHA256=9b5ba934bc9f343288eee0968106303c84d513d6bd7c23db15515dd530a67cbd    # game.tar.gz
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --proto =https -o "$tmp/game.tar.gz" "$site/game.tar.gz"
echo "$GAME_SHA256  $tmp/game.tar.gz" | sha256sum -c --quiet \
    || { echo "fetch-data.sh: game.tar.gz is not the checked-in one (its SHA-256 differs): not used"; exit 1; }
# (only plain file names in the tarball: nothing that could land outside DIR)
if tar -tzf "$tmp/game.tar.gz" | grep -qv '^[A-Za-z0-9_.-]*$'; then
    echo "fetch-data.sh: game.tar.gz names something other than plain files: not used"
    exit 1
fi
mkdir -p "$dir"
tar -xzf "$tmp/game.tar.gz" -C "$dir" --no-same-owner --no-same-permissions
echo "fetch-data.sh: the game's data files in $dir (checked); now: make data DATA=... FROM=$dir"
