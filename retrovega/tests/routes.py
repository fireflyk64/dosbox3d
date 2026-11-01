#!/usr/bin/env python3
"""Every way through the campaign's map (data/campaign.csv), one per line: a W or an L for each mission flown, for
RV.EXE's /route.  A system flown all W comes out good, all L bad (campaign.c, way_good).  tests/monkey.sh walks them."""
import csv
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
nodes = {r['node']: r for r in csv.DictReader(open(os.path.join(ROOT, 'data', 'campaign.csv')))}


def walk(node, route):
    if node not in nodes:
        yield route
        return
    n = len(nodes[node]['missions'].split())
    good, bad = nodes[node]['good'], nodes[node]['bad']
    yield from walk(good, route + 'W' * n)
    if bad != good:
        yield from walk(bad, route + 'L' * n)


for r in walk('ENIGMA', ''):
    print(r)
