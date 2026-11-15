#!/usr/bin/env python3
"""Terminal Velocity: another build's GAME.EXE tried against the hooks' signatures, as the emulator's finder tries
the program in memory (docs/terminalvelocity.md, section 3).

    scripts/tvsigcheck.py SHAREWARE/GAME.EXE REGISTERED/GAME.EXE [OTHER/GAME.EXE]

Each anchor (its first window, then its variants) and each natively drawn routine (scripts/tvsig.py's lists) looked
for in the last program's code object: where it is, or that it is missing or there more than once.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvexe, tvsig


def main():
    les = [tvexe.LE(p) for p in sys.argv[1:]]
    imgs = [le.load() for le in les]
    rows, ok = tvsig.build(les[:2], imgs[:2], lambda t: print(t))
    bad = tvsig.locate(rows, les, imgs, len(les) - 1, print)
    print('%d not found' % bad)
    return 1 if bad or not ok else 0


if __name__ == '__main__':
    sys.exit(main())
