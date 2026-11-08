#!/usr/bin/env python3
"""The old game's campaign tree, read from the user's copy, against data/campaign.csv (the user, 2026-11-08, on the
winning path at Shelton in Schroedingers: "I was expecting more franklin missions or some gawain or lancelot missions",
"I wonder if winning and losing paths got mixed up": they had; port.py wrote the branching "as recalled").
    tests/wc1/camptree.py GAMEDAT/CAMP.000
CAMP.000 is briefing.py's container (a size, 3-byte offsets with a flag byte, entries of a 4-byte size and LZW); its
entry 1 is the series table, a 90-byte record a series in the campaign's order: +0 the wingman, +2 how many missions,
+6 the series a win goes to and +7 the fighter flown there, +8 the series a loss goes to and +9 its fighter (series
1-based, 0xFF the end), +3 the points a win needs (campaign.csv's pass; the last two none).  The fighters agree with
every series' own player record (port.py's TYPES), which is how the fields were read.  Prints the tree; exit 1 when
campaign.csv's good, bad or pass differs."""
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import briefing                         # noqa: E402  (the container and its LZW)

NODES = ['ENIGMA', 'CEPHID', 'SHELTON', 'CRUCIBL', 'OLDZIEY', 'DEFIANC', 'LIXIS', 'ATARAXI', 'RIGEL', 'BARNARD', 'WOLF359',
         'SHMRN', 'LUYTEN']
FIGHTERS = {0: 'SCHROED', 1: 'LANCELT', 2: 'FRANKLN', 3: 'GAWAIN'}
END = {'SHMRN': 'GOOD', 'LUYTEN': 'BAD'}  # (the two last series end the campaign: won, lost)
RECORD = 90


def main():
    table = dict(briefing.entries(sys.argv[1]))[1]
    ours = {r['node']: r for r in csv.DictReader(open(os.path.join(HERE, '..', '..', 'data', 'campaign.csv')))}
    differ = 0
    for i, node in enumerate(NODES):
        r = table[RECORD * i:RECORD * (i + 1)]
        if r[6] == 0xFF:
            want = (END[node], END[node] if node == 'LUYTEN' else 'BAD', 0)
            how = 'the end'
        else:
            want = (NODES[r[6] - 1], NODES[r[8] - 1], r[3] | r[4] << 8)
            how = 'won with %d points: %s (in %s), lost: %s (in %s)' % (want[2], want[0], FIGHTERS.get(r[7], r[7]), want[1],
                                                                        FIGHTERS.get(r[9], r[9]))
        have = (ours[node]['good'], ours[node]['bad'], int(ours[node].get('pass') or 0)) if node in ours else None
        bad = have != want
        differ += bad
        print('%-8s %d missions, %s%s' % (node, r[2], how, '   <-- ours: good %s, bad %s, pass %d' % have if bad and have else
                                            '   <-- not in campaign.csv' if bad else ''))
    print('%d systems branch otherwise than the old game' % differ)
    return 1 if differ else 0


if __name__ == '__main__':
    sys.exit(main())
