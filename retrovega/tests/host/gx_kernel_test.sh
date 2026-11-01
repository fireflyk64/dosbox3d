#!/bin/sh
# The sprite accelerator's pixel code on its own (docs/gx.md): the same pixels as the game's kernel, on the real art,
# and damaged files decoded and drawn under the sanitizers; then how fast.  tests/host/gx_kernel_test.sh
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/host
RV=${1:-art/ships/SHIP02.RV}
g++ -std=c++14 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I src -o build/host/gx_kernel_san tests/host/gx_kernel_test.cpp
build/host/gx_kernel_san views art/ships/SHIP*.RV build/rvdat/SHIP2*.RV
build/host/gx_kernel_san equal "$RV"
build/host/gx_kernel_san fuzz "$RV" "${FUZZ:-300}"
g++ -std=c++14 -O2 -I src -o build/host/gx_kernel tests/host/gx_kernel_test.cpp
build/host/gx_kernel bench "$RV"
