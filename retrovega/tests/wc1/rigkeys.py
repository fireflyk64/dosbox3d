#!/usr/bin/env python3
"""The old game's kill rigs as a key script for run-old.sh (README.md "The guns against the old game's"): the fields
off at 1 s and the autopilot at 3 s always, then the pieces given, at seconds of the script's clock:

    rigkeys.py ITEM ...
      tail=T0-T1:KM        our ship put KM behind the first foe every 0.05 s, the trigger held (!tail=foe:KM)
      face=T0-T1:EVERY     the nose onto the first foe every EVERY s, the trigger held (the hunting rig, !face=foe)
      shoot=T0-T1:Q[:back] the first foe hit for Q from dead ahead (or behind) every 0.1 s (!shoot=foe:Q): the damage
                           model alone
      press=K@T,T..        a key at those times (a for the autopilot on to the next nav point)
      kill=T:SLOT          that slot killed (the wingman, 1, out of the fight)
      disarm=T:S,S..       those slots' gun lists emptied (dseg:C472, 0x33 a slot: a count, then 5 bytes an entry)
      poke=T:OFF:BYTE      a byte of the data segment
      dump=T:TAG           the data segment to TAG.bin (dsdump.py)

e.g. the Hornet given a third laser at the middle mount (16) at Enyo 1's nav 1 and hunting the Dralthi:
    tests/wc1/run-old.sh OUT 1 0 "$(tests/wc1/rigkeys.py kill=10:1 poke=10.1:C472:06 poke=10.1:C48C:18 \\
        poke=10.1:C48D:00 poke=10.1:C48E:10 poke=10.1:C48F:00 poke=10.1:C490:00 face=10.5-250.5:3)" 330
(the player's list then has six entries, its two lasers, two DF, a HS and the new laser; poke=10.1:C475:11 and
poke=10.1:C47A:0F move the first two to mounts 17 and 15, 30 km either side of the middle as the Schroedinger's)
"""
import sys

items = [(1.0, '!rocks=0'), (3.0, 'a')]
for arg in sys.argv[1:]:
    k, _, v = arg.partition('=')
    if k in ('tail', 'shoot', 'face'):
        span, _, rest = v.partition(':')
        t0, t1 = (float(x) for x in span.split('-'))
        if k != 'shoot':
            items.append((t0, '+space'))
        step = 0.05 if k == 'tail' else 0.1 if k == 'shoot' else float(rest or 3)
        what = '!face=foe' if k == 'face' else '!%s=foe:%s' % (k, rest)
        t = t0
        while t < t1:
            items.append((t, what))
            t += step
        if k != 'shoot':
            items.append((t1, '-space'))
    elif k == 'press':
        key, _, ts = v.partition('@')
        items += [(float(t), key) for t in ts.split(',')]
    elif k == 'kill':
        t, _, s = v.partition(':')
        items.append((float(t), '!kill=%s' % s))
    elif k == 'disarm':
        t, _, ss = v.partition(':')
        items += [(float(t), '!poke=%X:0' % (0xC472 + 0x33 * int(s))) for s in ss.split(',')]
    elif k == 'poke':
        t, _, rest = v.partition(':')
        items.append((float(t), '!poke=%s' % rest))
    elif k == 'dump':
        t, _, tag = v.partition(':')
        items.append((float(t), '@' + tag))
    else:
        sys.exit('rigkeys.py: what is %r? (tail, face, shoot, press, kill, disarm, poke, dump)' % arg)
items.sort(key=lambda i: i[0])
print(','.join('%.2f:%s' % i for i in items))
