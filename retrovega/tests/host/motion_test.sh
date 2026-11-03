#!/bin/sh
# Visual motion effects on a 32-bit host, with bounds and integer checks. No game assets needed.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/host
gcc -m32 -std=gnu99 -g -O1 -funsigned-char -fno-strict-aliasing -ffunction-sections -fdata-sections -Wno-unused-result \
    -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all \
    -include host/rvhost.h -Isrc -Ihost -Ihost/dos -Wl,--gc-sections \
    -o build/host/motion_test tests/host/motion_test.c src/debris.c src/lightfield.c host/fixed.c -lm
build/host/motion_test
