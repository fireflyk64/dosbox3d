#!/bin/bash
# Builds libwclobby.a, a local lobbylink server and the C test, then runs the
# test against the local server (no Origin header, --allow-no-origin).
#
#   ./run.sh                    local server on 127.0.0.1:8789
#   ./run.sh https://host/path  an existing server (Origin derived from it)
set -e
here="$(cd "$(dirname "$0")" && pwd)"
crate="$here/.."
lobbylink="$crate/../lobbylink"

(cd "$crate" && cargo build --release)
gcc -g -O1 -Wall -o "$here/wclobby_test" "$here/wclobby_test.c" -I"$crate/include" \
    "$crate/target/release/libwclobby.a" -lgcc_s -lutil -lrt -lpthread -lm -ldl

if [ -n "$1" ]; then
    exec "$here/wclobby_test" "$1" "WCTEST$$" "$(echo "$1" | sed -E 's#^(https?://[^/]+).*#\1#')"
fi

(cd "$lobbylink" && CGO_ENABLED=0 go build -o dist/p2p-lobby-server ./cmd/p2p-lobby-server)
"$lobbylink/dist/p2p-lobby-server" --listen-http 127.0.0.1:8789 --allow-no-origin \
    --public-url http://127.0.0.1:8789 >"$here/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null' EXIT
sleep 1
"$here/wclobby_test" http://127.0.0.1:8789 "WCTEST$$" ""
