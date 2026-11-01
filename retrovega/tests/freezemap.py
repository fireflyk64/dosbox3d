#!/usr/bin/env python3
"""A freeze report (src/freeze.c) in names: tests/freezemap.py RVLOG.TXT [RV.MAP].  Addresses run from the program's load
segment ("freeze: program at"); the map's symbols are relative to it.  The stack is scanned for far return addresses."""
import bisect
import re
import sys

log = sys.argv[1]
mapf = sys.argv[2] if len(sys.argv) > 2 else 'build/dos/RV.MAP'
syms = []
for m in re.finditer(r'^([0-9a-f]{4}):([0-9a-f]{4})[+* ]*\s+(\S+)\s*$', open(mapf, errors='replace').read(), re.M):
    syms.append((int(m[1], 16) * 16 + int(m[2], 16), m[3]))
syms.sort()
addrs = [a for a, _ in syms]
bases = {int(m[1], 16) * 16 for m in (re.match(r'.*?([0-9a-f]{4}):[0-9a-f]{4}\s+[0-9a-f]{8}', l)
                                       for l in open(mapf, errors='replace') if ' CODE ' in l or ' RVHOOK ' in l) if m}


def name(seg, off, load):
    lin = (seg - load) * 16 + off
    i = bisect.bisect_right(addrs, lin) - 1
    if i < 0:
        return None
    return '%s+%x' % (syms[i][1], lin - syms[i][0])


load = 0
for line in open(log, errors='replace'):
    line = line.rstrip()
    if line.startswith('FREEZE') or line.startswith('freeze:'):
        print(line[:110])
    m = re.match(r'freeze: program at ([0-9a-f]{4})', line)
    if m:
        load = int(m[1], 16)
    m = re.match(r'freeze: cs:ip .*?fl ss:sp ([0-9a-f]{4}):([0-9a-f]{4})', line)
    if m:
        print('   stopped in', name(int(m[1], 16), int(m[2], 16), load))
    if line.startswith('freeze: the last second'):
        seen = {}
        for s, o in re.findall(r'([0-9a-f]{4}):([0-9a-f]{4})', line):
            n = name(int(s, 16), int(o, 16), load)
            seen[n] = seen.get(n, 0) + 1
        print('   ', ', '.join('%s x%d' % kv for kv in seen.items()))
    if line.startswith('freeze: stack'):
        w = [int(x, 16) for x in line.split()[2:]]
        for i in range(len(w) - 1):
            off, seg = w[i], w[i + 1]
            # a far return address: its segment is a code segment's own (the rest of the stack is numbers)
            if seg >= load and (seg - load) * 16 in bases:
                n = name(seg, off, load)
                if n:
                    print('    called from', n)
