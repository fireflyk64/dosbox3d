#!/bin/bash
# DOOMNET.COM assembled (nasm) and written as the bytes the emulator puts on its Z: drive: src/cpu/wcnet_doomnet.h
# from src/cpu/wcnet_doomnet.asm.  The header is checked in: run this after a change to the .asm.  docs/doom.md.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP=$(mktemp -d)
nasm -f bin -o $TMP/doomnet.com $ROOT/src/cpu/wcnet_doomnet.asm
python3 - $TMP/doomnet.com $ROOT/src/cpu/wcnet_doomnet.h <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
marks = [i for i in range(len(data) - 3) if data[i:i + 4] == bytes([0xFE, 0x38, 0x5A, 0xA5])]
assert len(marks) == 3, marks
out = ["// DOOMNET.COM (wcnet_doomnet.asm, by scripts/doomnet-asm.sh: do not edit).  %d bytes; the emulator's callback" % len(data),
       "// instruction at %s (its number written over the 5A A5)." % ", ".join("0x%X" % m for m in marks),
       "static const unsigned char kDoomnetCom[] = {"]
for i in range(0, len(data), 24):
    out.append("    " + ",".join("0x%02X" % b for b in data[i:i + 24]) + ",")
out.append("};")
out.append("static const unsigned kDoomnetCalls[] = { %s };" % ", ".join("0x%X" % m for m in marks))
open(sys.argv[2], 'w').write("\n".join(out) + "\n")
print("%s: %d bytes, %d calls" % (sys.argv[2], len(data), len(marks)))
EOF
rm -rf $TMP
