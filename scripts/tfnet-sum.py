#!/usr/bin/env python3
# TIE Fighter, step 15 (docs/tiefighter.md): each machine's wall reports of a scripts/tfnet-duo.sh run summed (shots, hits,
# kills, kills credited, owner changes) with its flights' deaths and launches from its log:  scripts/tfnet-sum.py /tmp/tfnet-duo/host.err /tmp/tfnet-duo/guest.err
import re, sys
for fn in sys.argv[1:]:
    tot = {}; peers0 = 0
    for line in open(fn, errors='replace'):
        if 'TIE: wall' not in line: continue
        for k, pat in (('owner changes', r'(\d+) owner changes'), ('shots sent', r'(\d+) shots sent'), ('shots made', r'(\d+) made'),
                       ('hits sent', r'(\d+) hits sent'), ('applied', r'(\d+) applied'), ('damage skipped', r'(\d+) damage skipped'),
                       ('near sent', r'\((\d+) near'), ('mid sent', r'(\d+) mid,'), ('far sent', r'(\d+) far\)'),
                       ('far flown by our AI (max)', None),
                       ('kills', r'(\d+) kills'), ('credited', r'(\d+) credited'), ('their shots here', r'(\d+) of their shots here'), ('hits of ours shown', r'(\d+) hits of ours shown')):
            if pat is None:
                m = re.search(r'far: (\d+) flown by our AI', line)
                if m: tot[k] = max(tot.get(k, 0), int(m.group(1)))
                continue
            m = re.search(pat, line)
            if m: tot[k] = tot.get(k, 0) + int(m.group(1))
        if ' 0 peers' in line: peers0 += 1
    log = open(fn, errors='replace').read()
    ev = {k: len(re.findall(p, log)) for k, p in (('launches', r'launched: the Combat'), ('down', r'is down: the flight ends for everybody\n|You are down: the flight ends for everybody\n'),
                                                 ('killed here', r'killed here after'), ('hits on us applied', r'the hit on obj \d+ applied'))}
    print(fn.rsplit('/', 1)[-1], ', '.join('%s %d' % kv for kv in list(tot.items()) + list(ev.items())), ', reports with 0 peers %d' % peers0)
