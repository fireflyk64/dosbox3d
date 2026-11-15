#!/usr/bin/env python3
"""Terminal Velocity: where the game's time goes, by function (docs/terminalvelocity.md, profile.md).

    WCNET_TV_PROF=1 in a rig's environment (with WCNET_DUMP_DIR, as the rigs set it) writes tvprof-<seconds>.txt every
    10 emulated seconds: the program's place sampled at each tick of the layer (every ~1000 instructions), counted in
    16-byte steps of its code (the shareware's image addresses).

    scripts/tvprof.py GAME.EXE DIR [FROM_S TO_S]     the counts of DIR's dumps in that window, by function

Each step is given to the function it lies in (tvexe's func: back to the nearest entry the code's calls name, or the
prologue before it); the busiest functions printed with their share of all samples and their own busiest steps.
"""
import glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvexe


def main():
    le = tvexe.LE(sys.argv[1])
    img = le.load()
    lo_s = float(sys.argv[3]) if len(sys.argv) > 3 else 0
    hi_s = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
    total = other = 0
    steps = {}
    for path in sorted(glob.glob(os.path.join(sys.argv[2], 'tvprof-*.txt'))):
        t = float(os.path.basename(path)[7:-4])
        if not (lo_s < t <= hi_s):
            continue
        for line in open(path):
            k, v = line.split()
            if k == 'total':
                total += int(v)
            elif k == 'other':
                other += int(v)
            else:
                steps[int(k, 16)] = steps.get(int(k, 16), 0) + int(v)
    if not total:
        print('no samples')
        return
    funcs = {}
    cache = {}
    for a, n in steps.items():
        f = cache.get(a)
        if f is None:
            f = cache[a] = tvexe.func_of(le, img, a)
        e = funcs.setdefault(f, [0, {}])
        e[0] += n
        e[1][a] = n
    print('%d samples (%.0f s at ~%d a second), %.1f%% outside the game\'s code' % (total, total / 58000.0, 58000, 100.0 * other / total))
    acc = 0.0
    for f, (n, st) in sorted(funcs.items(), key=lambda kv: -kv[1][0])[:25]:
        share = 100.0 * n / total
        acc += share
        span = '%05x-%05x' % (min(st), max(st) + 16)
        print('  %05x  %5.1f%%  (%5.1f%% so far)  steps %s' % (f, share, acc, span))


if __name__ == '__main__':
    main()
