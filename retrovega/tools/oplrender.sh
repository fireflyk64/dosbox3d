#!/bin/sh
# Builds build/oplrender (tools/oplrender.cpp): src/opl.c on the host, through an OPL3 emulator: ymfm's YMF262
# (host/ymfm, as the native build), or DOSBox's own dbopl when this tree sits inside a DOSBox tree (../src/hardware/
# dbopl.cpp, configured in ../build-native: the chip the game has in DOSBox).  OPL=ymfm or OPL=dbopl chooses.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
DB=..
if [ -z "$OPL" ]; then
    if [ -f $DB/src/hardware/dbopl.cpp ] && [ -f $DB/build-native/config.h ]; then OPL=dbopl; else OPL=ymfm; fi
fi
gcc -O2 -Wall -c src/opl.c -o build/host/opl.o
if [ "$OPL" = dbopl ]; then
    g++ -O2 -w -I$DB/include -I$DB/build-native -c $DB/src/hardware/dbopl.cpp -o build/host/dbopl.o
    g++ -O2 -Wall -DOPL_DBOPL -I$DB/include -I$DB/build-native -c tools/oplrender.cpp -o build/host/oplrender.o
    g++ -o build/oplrender build/host/oplrender.o build/host/dbopl.o build/host/opl.o
else
    for f in ymfm_opl ymfm_adpcm ymfm_pcm; do
        g++ -O2 -std=c++14 -w -c host/ymfm/$f.cpp -o build/host/$f.o
    done
    g++ -O2 -std=c++14 -Wall -c tools/oplrender.cpp -o build/host/oplrender.o
    g++ -o build/oplrender build/host/oplrender.o build/host/ymfm_opl.o build/host/ymfm_adpcm.o build/host/ymfm_pcm.o build/host/opl.o
fi
echo "built build/oplrender ($OPL)"
