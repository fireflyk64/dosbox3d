#!/usr/bin/env python3
"""Two player fighters trade places: each takes the other's part in the campaign and its numbers, and keeps its own
looks (the user, 2026-11-07: the Lancelot to be the old game's Raptor, the heavy one, and the Gawain its Rapier, the
quick one; "this should not change the game since it's basically just an art change").

    tools/swapships.py [KEY_A KEY_B] [--write]        (default GAWAIN LANCELT; without --write it only reports)

What moves from one to the other (a ship's part):
 - data/ships.csv: every column the simulation reads (speeds, turns, shields, armour, hull, guns, missiles, fuel,
   radius_km: the old game's hit size, and the rest in PART); the name, the art (mesh, vdu, lod0_px), the class and
   the notes stay with the ship;
 - data/missions.txt: the two keys trade places on every line (the missions the player flies them in, the ships of
   the player's side at the nav points, prey=) and so do the two names in the words (briefings, Kit's and the bar's
   lines: "Tell me about the Lancelot" becomes "Tell me about the Gawain" in the same mission);
 - tests/play/playtest.c: the tests that want a loadout (PT_HOMER, PT_MIXED, the shield test's type) take the type
   that now has it, and their words the name;
 - tests/wc1/port.py and places.py: their old-game type map (TYPES: the Rapier and the Raptor).
What stays with each ship: its sprites (SHIPnn.RV by its type number), its cockpit, panels, legs and stick, its
engines' glow points, its hit box and silhouettes (data/hitboxes.csv, SHAPES.DAT), the deck's picture of it.

The words are swapped as names only: a line that says what kind of ship one is ("A Gawain. A proper interceptor.")
then says it of the other, so the lines naming a kind are listed to be read again by hand, as are the docs.
"""
import argparse
import csv
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# the ships.csv columns the game's logic reads (tools/rvdata.py SHIPTYPE and compile_loadout): a ship's part
PART = ['cruise_kps', 'max_kps', 'ab_kps', 'accel', 'yaw_dps', 'pitch_dps', 'roll_dps', 'slide_kps', 'slide_dmg_kps',
        'shield_f', 'shield_r', 'armor_f', 'armor_r', 'armor_l', 'armor_rt', 'hull', 'hull_hits', 'guns', 'turrets',
        'missiles', 'bays', 'radius_km', 'gun_energy_steps', 'shield_full_shift', 'shield_steps', 'fuel', 'hit']
# words in a line that say what kind of ship it is: such a line is listed to be read again
KIND_WORDS = re.compile(r'interceptor|heavy|superiority|prototype|quick|fast|agile|nimble|sluggish|armou?r|tank|'
                        r'light fighter|bomber', re.I)
# the lines of those that would say the wrong thing after the swap, as they read then and as they should
# (both ways: a second run puts everything back as it was)
REWORD = {('GAWAIN', 'LANCELT'): [('"A Lancelot. A proper interceptor. Don\'t get used to it."',
                                   '"A Lancelot. A proper heavy fighter. Don\'t get used to it."'),
                                  ('"A Gawain. A proper heavy fighter. Don\'t get used to it."',
                                   '"A Gawain. A proper interceptor. Don\'t get used to it."')]}


def swap_tokens(text, pairs):
    """every whole word of pairs (a dict both ways) replaced by its partner, at once (no partner turned back)"""
    rx = re.compile(r'\b(%s)(?=(?:s|\'s)?\b)' % '|'.join(map(re.escape, pairs)))
    return rx.sub(lambda m: pairs[m.group(1)], text)


def both_ways(a, b):
    return {a: b, b: a}


class Edit:
    def __init__(self, write):
        self.write, self.changed = write, []

    def file(self, rel, new_text, old_text):
        if new_text == old_text:
            return
        self.changed.append(rel)
        if self.write:
            open(os.path.join(ROOT, rel), 'w', newline='').write(new_text)


def ships_csv(ed, a, b):
    rel = 'data/ships.csv'
    text = open(os.path.join(ROOT, rel), newline='').read()
    lines = text.split('\n')
    head = next(csv.reader([lines[0]]))
    at = {}
    for i, line in enumerate(lines[1:], 1):
        if line:
            row = next(csv.reader([line]))
            if row[head.index('key')] in (a, b):
                at[row[head.index('key')]] = (i, row)
    if set(at) != {a, b}:
        sys.exit('swapships: ships.csv has no row %s' % ' or '.join(k for k in (a, b) if k not in at))
    for k, (i, row) in at.items():                 # (the two rows written back as the csv module writes them: check
        out = io.StringIO()                         # that this changes nothing else in them)
        csv.writer(out, lineterminator='').writerow(row)
        if out.getvalue() != lines[i]:
            sys.exit('swapships: ships.csv %s: the row would not be written back as it was; edit it by hand' % k)
    (ia, ra), (ib, rb) = at[a], at[b]
    print('ships.csv: %s and %s trade their parts:' % (ra[head.index('name')], rb[head.index('name')]))
    for col in PART:
        j = head.index(col)
        if ra[j] != rb[j]:
            print('  %-14s %s %-22s <-> %s %s' % (col, a, ra[j] or '(blank)', b, rb[j] or '(blank)'))
        ra[j], rb[j] = rb[j], ra[j]
    for i, row in ((ia, ra), (ib, rb)):
        out = io.StringIO()
        csv.writer(out, lineterminator='').writerow(row)
        lines[i] = out.getvalue()
    ed.file(rel, '\n'.join(lines), text)
    return ra[head.index('name')], rb[head.index('name')]


def missions(ed, keys, names, pair):
    rel = 'data/missions.txt'
    text = open(os.path.join(ROOT, rel)).read()
    new = swap_tokens(swap_tokens(text, keys), names)
    for was, now in REWORD.get(tuple(sorted(pair)), []):
        if new.count(was) == 1:
            new = new.replace(was, now)
            print('  reworded: %s -> %s' % (was, now))
    olds, news = text.split('\n'), new.split('\n')
    n = sum(1 for o, w in zip(olds, news) if o != w)
    print('\nmissions.txt: %d lines change (keys %d, names %d)' % (
        n, len(re.findall(r'\b(%s)\b' % '|'.join(keys), text)),
        len(re.findall(r'\b(%s)' % '|'.join(names), text))))
    review = [(i + 1, w) for i, (o, w) in enumerate(zip(olds, news))
              if o != w and re.search('|'.join(names), w) and KIND_WORDS.search(w)]
    if review:
        print('  lines naming a kind of ship, to read again (after the swap):')
        for i, w in review:
            print('    %d: %s' % (i, w.strip()[:150]))
    ed.file(rel, new, text)


def playtest(ed, a_type, b_type, names):
    rel = 'tests/play/playtest.c'
    text = open(os.path.join(ROOT, rel)).read()
    ta, tb = '0x%02X' % a_type, '0x%02X' % b_type
    pairs = {ta: tb, tb: ta}
    # the type numbers only where a test asks for a ship by its loadout: the defines and pt_alone(0x..)
    new = re.sub(r'(#define PT_\w+\s+)(0x[0-9A-Fa-f]{2})\b', lambda m: m.group(1) + pairs.get(m.group(2), m.group(2)), text)
    new = re.sub(r'(pt_alone\()(0x[0-9A-Fa-f]{2})\)', lambda m: m.group(1) + pairs.get(m.group(2), m.group(2)) + ')', new)
    new = swap_tokens(new, names)
    print('\nplaytest.c: %d lines change' % sum(1 for o, w in zip(text.split('\n'), new.split('\n')) if o != w))
    ed.file(rel, new, text)


def wc1_types(ed, keys):
    for rel in ('tests/wc1/port.py', 'tests/wc1/places.py'):
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        text = open(path).read()
        m = re.search(r'^TYPES = \{.*?\}', text, re.S | re.M)
        if not m:
            continue
        new = text[:m.start()] + swap_tokens(m.group(0), keys) + text[m.end():]
        if new != text:
            print('%s: its old-game type map' % rel)
        ed.file(rel, new, text)


def docs(names):
    """the docs that name either ship: not edited (a doc may speak of the looks or of the part), listed"""
    rx = re.compile(r'\b(%s)' % '|'.join(names))
    paths = ['README.md', 'tests/wc1/README.md'] + ['docs/' + f for f in sorted(os.listdir(os.path.join(ROOT, 'docs')))
                                                    if f.endswith('.md')]
    found = []
    for rel in paths:
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            n = len(rx.findall(open(p).read()))
            if n:
                found.append('%s (%d)' % (rel, n))
    print('\nthe docs that name them, to read by hand: ' + ', '.join(found))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('keys', nargs='*', default=['GAWAIN', 'LANCELT'])
    ap.add_argument('--write', action='store_true', help='make the change (without it: only the report)')
    args = ap.parse_args()
    if len(args.keys) != 2:
        sys.exit('usage: tools/swapships.py [KEY_A KEY_B] [--write]')
    a, b = args.keys
    types = {r['key']: int(r['type'], 16) for r in csv.DictReader(open(os.path.join(ROOT, 'data/ships.csv')))}
    ed = Edit(args.write)
    name_a, name_b = ships_csv(ed, a, b)
    names = both_ways(name_a, name_b)
    missions(ed, both_ways(a, b), names, (a, b))
    playtest(ed, types[a], types[b], names)
    wc1_types(ed, both_ways(a, b))
    docs(names)
    print('\n%s: %s' % ('written' if args.write else 'would change (run with --write)', ', '.join(ed.changed)))


if __name__ == '__main__':
    main()
