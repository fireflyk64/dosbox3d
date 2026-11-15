#!/usr/bin/env python3
"""The TIE Fighter demo's campaign: the missions flown after the demo's own (docs/tiedemo.md section 8).

The demo reads its mission file (DEMO.TIE) whole into memory at create_createmission: a 456-byte header (three counts,
then OpenTIE's EMissionStruct), the flight groups (OpenTIE's EFGStruct, 292 bytes each, at most 48) and the radio
messages (90 bytes each, at most 10).  The layer writes one of these missions over that copy just after the read
(wcnet_tiedemo_campaign.cpp); the game's files are never changed.

    scripts/tdmmissions.py build [out-dir]     the C header (src/cpu/wcnet_tiedemo_missions.h) and, in out-dir, each
                                              mission as a .TIE file the decoder reads
    scripts/tdmmissions.py decode FILE.TIE     a mission file's flight groups, orders, goals, places and messages

Places are in the mission file's units: 160 to the kilometre.  The craft types are the demo's (X-Wing's numbering).
"""
import os
import struct
import sys

KM = 160
# craft types (the demo's: TIEMISS's species table, SPECIES.LFD's models)
XW, YW, AW, TF, TI, TB, GUN, TRN, SHU, TUG, CON, FRT, CRS, FRG, CRV, STD = range(1, 17)
TYPE_NAMES = {1: 'X-W', 2: 'Y-W', 3: 'A-W', 4: 'T/F', 5: 'T/I', 6: 'T/B', 7: 'GUN', 8: 'TRN', 9: 'SHU', 10: 'TUG',
              11: 'CON', 12: 'FRT', 13: 'CRS', 14: 'FRG', 15: 'CRV', 16: 'STD', 17: 'T/A', 18: 'B-W'}
REBEL, IMPERIAL = 0, 1
MISSILES, TORPEDOES = 3, 4
# orders (the mission file's: OpenTIE ordersldr)
HOLD, GO_HOME, CIRCLE, EVADE, RENDEZVOUS, DISABLED, AWAIT_BOARD, ATTACK, ATTACK_ESCORTS, PROTECT, ESCORT = range(11)
SS_PATROL, SS_WAIT_PROTECT = 21, 25
# a flight group's status (+0x34, TIE95's Status1, measured in this build): 1 twice the warheads, 4 half the shields,
# 7 shields at nothing (recharging)
DOUBLE_WARHEADS, HALF_SHIELDS = 1, 4
# conditions (score_checkcondition)
ALWAYS, ARRIVED, DESTROYED, ATTACKED = 0, 1, 2, 3
SURVIVED, NEVER = 9, 10
# who a condition or an order's target names
G_NONE, G_FG, G_TYPE, G_GENUS, G_FAMILY, G_SIDE, G_ORDER, G_ATTR, G_SET = range(9)
# how many: a trigger's amount, and a goal's (this build's score_checkobjective takes a goal's as it is, unlike the CD's
# percentcon): 0 all, 1 three quarters, 2 half, 3 a quarter, 4 at least one, 5 all but one
ALL, P75, P50, P25, ONE, ALL_BUT_ONE = 0, 1, 2, 3, 4, 5

FG_SIZE, MSG_SIZE, HEADER_SIZE, MAX_FG, MAX_MSG = 0x124, 90, 456, 48, 10


def text(s, n):
    b = s.encode('latin1')
    assert len(b) < n, (s, n)
    return b + b'\0' * (n - len(b))


class Cond:
    """A trigger: what happened (cond), to whom (kind, ident), how many (amount)."""
    def __init__(self, cond=ALWAYS, kind=G_FG, ident=0, amount=ALL):
        self.cond, self.kind, self.ident, self.amount = cond, kind, ident, amount

    def pack(self):
        return bytes([self.cond, self.kind, self.ident & 0xFF, self.amount])


def arrived(fg, amount=ALL):
    return Cond(ARRIVED, G_FG, fg.index, amount)


def destroyed(fg, amount=ALL):
    return Cond(DESTROYED, G_FG, fg.index, amount)


def set_destroyed(group, amount=ALL):
    return Cond(DESTROYED, G_SET, group, amount)


class Order:
    def __init__(self, order, speed=0, targets=(), var=(0, 0, 0, 0)):
        self.order, self.speed, self.targets, self.var = order, speed, list(targets), list(var)

    def pack(self):
        b = bytearray(18)
        b[0], b[1] = self.order, self.speed
        b[2:6] = bytes(self.var)
        t = [(G_NONE, 0)] * 4
        for k, x in enumerate(self.targets[:4]):
            t[k] = (G_FG, x.index) if isinstance(x, FG) else x
        b[6], b[7], b[8], b[9] = t[0][0], t[1][0], t[0][1], t[1][1]
        b[10] = 1                                   # targets 1 or 2
        b[12], b[13], b[14], b[15] = t[2][0], t[2][1], t[3][0], t[3][1]
        b[16] = 1
        return bytes(b)


class FG:
    def __init__(self, mission, name, craft, n=1, side=REBEL, skill=2, warhead=0, cargo='', special=None, group=0,
                 waves=1, wave_delay=0, formation=0, player=0, arrive=None, arrive2=None, either=False, delay=0,
                 mother=None, orders=(), start=(0, 0, 0), way=(), home=None, goal=None, bonus=None, bonus_points=0,
                 markings=0, version=0, beam=0):
        self.index = len(mission.fgs)
        mission.fgs.append(self)
        self.__dict__.update(dict(name=name, craft=craft, n=n, side=side, skill=skill, warhead=warhead, cargo=cargo,
                                  special=special, group=group, waves=waves, wave_delay=wave_delay,
                                  formation=formation, player=player, arrive=arrive, arrive2=arrive2, either=either,
                                  delay=delay, mother=mother, orders=list(orders), start=start, way=list(way),
                                  home=home, goal=goal, bonus=bonus, bonus_points=bonus_points, markings=markings,
                                  version=version, beam=beam))

    def pack(self):
        f = bytearray(FG_SIZE)
        f[0:12] = text(self.name, 12)
        f[0x18:0x24] = text(self.cargo, 12)
        f[0x30] = self.special if self.special is not None else self.n   # (no special craft: past the last)
        f[0x32], f[0x33] = self.craft, self.n
        f[0x34], f[0x35], f[0x36], f[0x37] = self.version, self.warhead, self.beam, self.side
        f[0x38], f[0x39] = self.skill, self.markings
        f[0x3C], f[0x3D] = self.formation, 3 if self.formation == 0 else 0
        f[0x3E] = self.group
        f[0x40], f[0x41] = self.waves - 1, self.wave_delay
        f[0x42] = self.player
        f[0x44] = 0x40                              # (as every flight group of the demo's)
        f[0x4A:0x4E] = (self.arrive or Cond()).pack()
        f[0x4E:0x52] = (self.arrive2 or Cond()).pack()
        f[0x52] = 1 if self.either else 0
        f[0x54], f[0x55] = self.delay // 60, self.delay % 60
        f[0x56:0x5A] = Cond().pack()
        if self.mother is not None:
            f[0x60], f[0x61] = self.mother.index, 1
            f[0x62], f[0x63] = self.mother.index, 1
        orders = self.orders + [Order(HOLD, 8)] * (3 - len(self.orders))
        for k in range(3):
            f[0x68 + 18 * k: 0x68 + 18 * k + 18] = orders[k].pack()
        if self.goal:
            f[0x9E], f[0x9F] = self.goal
        if self.bonus:
            f[0xA4], f[0xA5] = self.bonus
            f[0xA6] = self.bonus_points & 0xFF
        points = [None] * 15
        points[0] = self.start
        for k, p in enumerate(self.way[:8]):
            points[4 + k] = p
        points[13] = self.home if self.home is not None else self.start
        points[14] = self.start
        for k, p in enumerate(points):
            if p is None:
                continue
            x, y, z = (int(round(v)) for v in p)
            struct.pack_into('<h', f, 0xA8 + 2 * k, x)
            struct.pack_into('<h', f, 0xC6 + 2 * k, y)
            struct.pack_into('<h', f, 0xE4 + 2 * k, z)
            struct.pack_into('<h', f, 0x102 + 2 * k, 1)
        f[0x123] = 1
        return bytes(f)


class Message:
    """A radio line: shown when `when` (and `when2`, or either) holds.  This build ignores a message's own delay, so a
    line `after` seconds past the flight's start (`when` ALWAYS) or past a flight group's arrival (`when` arrived(fg))
    is the layer's: its conditions are written never-true and the layer makes them true when its time comes
    (wcnet_tiedemo_campaign.cpp), the game then showing it as its own."""
    def __init__(self, mission, words, when, when2=None, either=False, after=None, tag=''):
        assert len(words) <= 60, (len(words), words)        # (the message bar's width)
        assert words == words.upper(), words
        self.index = len(mission.msgs)
        mission.msgs.append(self)
        self.words, self.when, self.when2, self.either, self.after, self.tag = words, when, when2, either, after, tag
        if after is not None:
            assert when2 is None and (when.cond == ALWAYS or (when.cond == ARRIVED and when.kind == G_FG)), words

    def timed(self):
        """(message, flight group or 255 for the flight's start, seconds), or None."""
        if self.after is None:
            return None
        return (self.index, 255 if self.when.cond == ALWAYS else self.when.ident, int(self.after))

    def pack(self):
        m = bytearray(MSG_SIZE)
        # (the game ends a line with a full stop unless it ends with "!" or "?": a line written ending with "." has
        # that one left to the game)
        m[0:64] = text(self.words[:-1] if self.words.endswith('.') else self.words, 64)
        never = Cond(NEVER, G_NONE, 0, 0)
        m[64:68] = (never if self.after is not None else self.when).pack()
        m[68:72] = (never if self.after is not None else (self.when2 or Cond(ALWAYS))).pack()
        m[72:88] = text(self.tag, 16)
        m[89] = 1 if self.either else 0
        return bytes(m)


class Mission:
    """A mission and the layer's rule for its end: the flight is over min_delay s after the primary goals are complete,
    once the flight groups in `tail` (whatever may still come after them) have been quiet (none of their craft about)
    for `quiet` s, or `cap` s after them at the latest; a failed primary goal, or the death of a craft of a flight
    group in `protect`, ends it a few seconds later (the mission flown again)."""
    def __init__(self, key, title, brief, minutes=30, win=('', ''), loss=('', ''), min_delay=10, quiet=0, cap=0):
        self.key, self.title, self.brief = key, title, brief
        self.minutes, self.win, self.loss = minutes, win, loss
        self.min_delay, self.quiet, self.cap = min_delay, quiet, cap
        self.fgs, self.msgs, self.tail, self.protect = [], [], [], []

    def pack(self):
        assert len(self.fgs) <= MAX_FG and len(self.msgs) <= MAX_MSG
        h = bytearray(HEADER_SIZE)
        struct.pack_into('<hhh', h, 0, len(self.fgs), len(self.msgs), 0)
        e = 6
        h[e + 0], h[e + 1], h[e + 2] = self.minutes, 0, 1
        def line(w):                                    # (the game's full stop, as a message's)
            assert len(w) <= 60 and w == w.upper(), w
            return text(w[:-1] if w.endswith('.') else w, 64)
        for k in range(2):
            h[e + 0x10 + 64 * k: e + 0x10 + 64 * k + 64] = line(self.win[k])
            h[e + 0x110 + 64 * k: e + 0x110 + 64 * k + 64] = line(self.loss[k])
        return bytes(h) + b''.join(fg.pack() for fg in self.fgs) + b''.join(m.pack() for m in self.msgs)


def ring(cx, cy, cz, r, k, n):
    """The k-th of n places on a ring of radius r about (cx, cy, cz), in the x-y plane."""
    import math
    a = 2 * math.pi * k / n
    return (cx + r * math.cos(a), cy + r * math.sin(a), cz)


# --- the missions --------------------------------------------------------------------------------------------------
# Speeds measured in flight (the world moves 0.44 m/s for each unit of a craft's speed): the T/I 110 m/s, the T/F and
# X-wing 99, the T/B and Y-wing 79, the shuttle 66.  In the world the file's y is reversed (the game's own).

def patrol(m, name, craft, centre, radius, side=IMPERIAL, speed=2, n=1, **kw):
    """A craft that stays: a slow loop of four waypoints about its start (100 rounds)."""
    cx, cy, cz = centre
    way = [(cx + radius, cy, cz), (cx, cy + radius, cz), (cx - radius, cy, cz), (cx, cy - radius, cz)]
    return FG(m, name, craft, n, side, start=centre, way=way, orders=[Order(SS_PATROL, speed, var=(100, 0, 0, 0))],
              **kw)


def mission_shuttles():
    m = Mission('shuttles', 'The Shuttles Run',
                "Mission 2 of 4, T/I Alpha, from the Chimaera: the repair yard's survivors are running in shuttles, "
                "four flights of three. Grand Admiral Thrawn wants no one warning their fleet: destroy at least two of "
                "each flight before it jumps. The X-wings are there to draw you off.",
                minutes=25, win=('THE FUGITIVES ARE DESTROYED. NONE WILL WARN THE REBELS.',
                                 'THE GRAND ADMIRAL WILL BE PLEASED.'),
                loss=('TOO MANY SHUTTLES ESCAPED.', 'THRAWN FORGIVES AN HONEST MISTAKE. ONCE.'))
    alpha = FG(m, 'ALPHA', TI, 3, IMPERIAL, skill=4, player=2, formation=1, start=(0, 0, 0), way=[(0, 3 * KM, 0)],
               orders=[Order(ATTACK, 0, [(G_TYPE, SHU - 1), (G_TYPE, XW - 1), (G_TYPE, YW - 1)])])
    prowler1 = patrol(m, 'PROWLER 1', CRV, (-1.2 * KM, -1.0 * KM, -0.3 * KM), 0.4 * KM, skill=3, markings=2)
    patrol(m, 'PROWLER 2', CRV, (1.2 * KM, -1.2 * KM, -0.3 * KM), 0.4 * KM, skill=3, markings=2)
    alpha.mother = prowler1
    # four flights of three, a minute apart, each crossing ahead of Alpha to a jump some 19 km on (about 290 s)
    plan = [('SIGMA', (-3, 3.5, 0.5), (14, 12, 1)), ('TAU', (4, 4, -0.5), (-14, 13, -1)),
            ('UPSILON', (-2, 6, 1.5), (12, 18, 2)), ('PHI', (3, 6.5, 1), (-12, 19, 0))]
    flights = []
    for name, a, b in plan:
        prev = flights[-1] if flights else None
        a = tuple(v * KM for v in a)
        b = tuple(v * KM for v in b)
        mid = tuple((a[k] + b[k]) / 2 for k in range(3))
        flights.append(FG(m, name, SHU, 3, REBEL, skill=1, cargo='SURVIVORS', group=1,
                          arrive=arrived(prev) if prev else None, delay=60 if prev else 0, start=a, way=[mid], home=b,
                          orders=[Order(GO_HOME, 10)], goal=(DESTROYED, P50)))
    FG(m, 'RED', XW, 2, REBEL, skill=2, formation=2, start=(0, 6 * KM, 0.8 * KM), orders=[Order(ATTACK, 0, [alpha])])
    FG(m, 'GOLD', YW, 2, REBEL, skill=1, formation=2, markings=1, start=(-1.5 * KM, 5 * KM, -0.5 * KM),
       orders=[Order(ATTACK, 0, [alpha])])
    red3 = FG(m, 'RED 3', XW, 2, REBEL, skill=3, formation=2, arrive=arrived(flights[2]), delay=10,
              start=(3 * KM, 8 * KM, 1 * KM), orders=[Order(ATTACK, 0, [alpha])])
    FG(m, 'GOLD 3', YW, 2, REBEL, skill=2, formation=2, markings=1, arrive=arrived(flights[3]), delay=5,
       start=(-3 * KM, 8 * KM, 0), orders=[Order(ATTACK, 0, [alpha])])
    # the radio: the Chimaera's, Alpha's, and the rebels' own calls, overheard (at most 10, 60 characters each)
    Message(m, 'CHIMAERA: SURVIVORS ARE FLEEING THE YARD. STOP THEM, ALPHA!', Cond(ALWAYS), tag='RUN')
    Message(m, 'PELLAEON: THE GRAND ADMIRAL WANTS NO ONE WARNING THE FLEET.', Cond(ALWAYS), after=12, tag='NOWARN')
    Message(m, 'SIGMA LEAD: INTERCEPTORS! ALL POWER TO THE ENGINES!', Cond(ALWAYS), after=30, tag='SIGMA')
    Message(m, 'GOLD LEAD: KEEP THOSE TIES BUSY! BUY THE SHUTTLES TIME!', Cond(ALWAYS), after=55, tag='GOLD')
    Message(m, 'MORE SHUTTLES ON SCOPE, ALPHA. KEEP AFTER THEM!', arrived(flights[1]), tag='MORE')
    Message(m, "TAU LEAD: SPLIT UP! THEY CAN'T CATCH US ALL!", arrived(flights[1]), after=12, tag='TAU')
    Message(m, 'SIGMA IS BROKEN. THE ADMIRAL WILL HEAR OF IT, ALPHA.', destroyed(flights[0], P50), tag='BROKEN')
    Message(m, 'PELLAEON: X-WINGS. A DECOY, AS THRAWN SAID. IGNORE THEM!', arrived(red3), tag='DECOY')
    Message(m, 'LAST FLIGHT OF SHUTTLES, ALPHA!', arrived(flights[3]), tag='LAST')
    Message(m, 'PHI LEAD: JUMP COORDINATES LOCKED. JUST HOLD TOGETHER!', arrived(flights[3]), after=15, tag='PHI')
    return m


def mission_corvettes():
    m = Mission('corvettes', 'Corvette Hunt',
                'Mission 3 of 4, T/B Alpha: the Grand Admiral has found where the rebel corvettes regroup. Sink all '
                'four: eight proton torpedoes each (W arms them; target a corvette, wait for the lock), about five to '
                'a corvette, lasers for the rest. Y-wings will dog you; X-wings will come.',
                minutes=30, win=('THE REBEL CORVETTES ARE DESTROYED.', 'THE CHIMAERA SENDS ITS COMPLIMENTS.'),
                loss=('', ''))
    # (measured: a corvette has 5,000 hull behind 10,000 of shields, a torpedo does some 1,300 to 2,500, the T/B carries
    # four: twice the warheads for Alpha, half the shields for the corvettes, some ten torpedoes' worth each)
    alpha = FG(m, 'ALPHA', TB, 3, IMPERIAL, skill=4, warhead=TORPEDOES, version=DOUBLE_WARHEADS, player=2, formation=1, start=(0, 0, 0),
               way=[(0, 4 * KM, 0)], orders=[Order(ATTACK, 0, [(G_SET, 1)])])
    patrol(m, 'PROWLER 1', CRV, (0, -1.5 * KM, -0.3 * KM), 0.4 * KM, skill=3, markings=2)
    corvettes = []
    for k, (name, c) in enumerate([('HORNET', (-2, 7, 0)), ('TYPHOON', (2, 7.5, 0.5)), ('WARRIOR', (-1, 9.5, -0.5)),
                                   ('TEMPEST', (2.5, 10, 0.3))]):
        corvettes.append(patrol(m, name, CRV, tuple(v * KM for v in c), 0.8 * KM, side=REBEL, speed=3, skill=2,
                                group=1, cargo='TROOPS' if k % 2 else 'SUPPLIES', goal=(DESTROYED, ALL),
                                version=HALF_SHIELDS))
    FG(m, 'GOLD', YW, 3, REBEL, skill=1, formation=2, markings=1, start=(0, 6 * KM, 0.5 * KM),
       orders=[Order(ATTACK, 0, [alpha])])
    red = FG(m, 'RED', XW, 2, REBEL, skill=2, formation=2, arrive=set_destroyed(1, ONE), delay=20, start=(4 * KM, 12 * KM, 1 * KM), orders=[Order(ATTACK, 0, [alpha])])
    red3 = FG(m, 'RED 3', XW, 2, REBEL, skill=2, formation=2, arrive=set_destroyed(1, P50), delay=15,
              start=(-4 * KM, 12 * KM, 0), orders=[Order(ATTACK, 0, [alpha])])
    Message(m, 'PELLAEON: FOUR CORVETTES, ALPHA. THE ADMIRAL WANTS THEM ALL.', Cond(ALWAYS), tag='SINK')
    Message(m, 'ALPHA LEAD: TORPEDOES ARMED. WAIT FOR THE LOCK, THEN FIRE.', Cond(ALWAYS), after=10, tag='ARMED')
    Message(m, 'HORNET: IMPERIAL BOMBERS! ALL TURRETS, TRACK THOSE TIES!', Cond(ALWAYS), after=22, tag='HORNET')
    Message(m, 'Y-WINGS ON YOU, ALPHA. STAY ON THE CORVETTES!', Cond(ALWAYS), after=35, tag='YWINGS')
    Message(m, 'CHIMAERA: THRAWN SAYS THEIR GUNNERS WATCH THE BOW. COME AFT.', Cond(ALWAYS), after=50, tag='AFT')
    Message(m, 'GOLD LEAD: KEEP THOSE BOMBERS OFF THE CORVETTES!', Cond(ALWAYS), after=80, tag='GOLD')
    Message(m, 'ONE DOWN. THE ADMIRAL EXPECTS THE REST TO BUNCH UP.', set_destroyed(1, ONE), tag='ONE')
    Message(m, "X-WINGS HAVE COME TO THE CORVETTES' AID!", arrived(red), tag='XWINGS')
    Message(m, "REBEL CAPTAIN: WE CAN'T HOLD THEM! WHERE IS OUR ESCORT?", set_destroyed(1, P50), tag='HOLD')
    Message(m, 'MORE X-WINGS, ALPHA. FINISH THE CORVETTES!', arrived(red3), tag='MORE')
    return m


def mission_escort():
    m = Mission('escort', 'Escort the Prowlers',
                "Mission 4 of 4, T/F Alpha: the Prowlers carry a Noghri team for the Grand Admiral. Y-wings will come "
                "for them, a flight at a time with X-wing cover: stop each flight before it can fire. Thrawn expects "
                "something more after; watch your scope.",
                minutes=30, win=('THE PROWLERS ARE SAFE.', 'PELLAEON: WELL FLOWN, ALPHA. THE ADMIRAL NOTICED.'),
                loss=('A PROWLER IS LOST.', ''), min_delay=10, quiet=25, cap=600)
    alpha = FG(m, 'ALPHA', TF, 3, IMPERIAL, skill=4, player=2, formation=1, start=(0, 0.6 * KM, 0),
               way=[(0, 4 * KM, 0)], orders=[Order(ATTACK, 0, [(G_SET, 2)])])
    # the Prowlers' slow route: a long loop
    route = [(-1.5 * KM, 3 * KM, 0), (1.5 * KM, 6 * KM, 0.2 * KM), (3 * KM, 3 * KM, 0), (1.5 * KM, 0, -0.2 * KM)]
    prowlers = []
    for k, off in enumerate([(-0.6, 0, 0), (0.6, -0.3, 0.1), (0, -0.9, -0.1)]):
        o = tuple(v * KM for v in off)
        way = [tuple(p[a] + o[a] for a in range(3)) for p in route]
        prowlers.append(FG(m, f'PROWLER {k + 1}', CRV, 1, IMPERIAL, skill=3, markings=2, group=1, start=o, way=way,
                           orders=[Order(SS_PATROL, 2, var=(100, 0, 0, 0))]))
    m.protect = prowlers                # (the layer's rule: this build's goals never fail a "survived" once met)
    alpha.mother = prowlers[0]
    # the Y-wing flights, each with cover; each comes when the one before is down
    waves = [('GOLD 1', 2, 'RED 1', 1, (-6, 9, 0.5)), ('GOLD 2', 3, 'RED 2', 2, (7, 9, -0.5)),
             ('GOLD 3', 3, 'RED 3', 1, (0, 11, 1.0))]
    golds = []
    for gname, gn, rname, rn, where in waves:
        prev = golds[-1] if golds else None
        w = tuple(v * KM for v in where)
        arrive = destroyed(prev) if prev else None
        gold = FG(m, gname, YW, gn, REBEL, skill=2, warhead=TORPEDOES, formation=2, markings=1, group=2, arrive=arrive,
                  delay=15 if prev else 0, start=w, orders=[Order(ATTACK, 0, [(G_SET, 1)])], goal=(DESTROYED, ALL))
        golds.append(gold)
        FG(m, rname, XW, rn, REBEL, skill=2, formation=2, arrive=arrive, delay=15 if prev else 0,
           start=(w[0] + 0.8 * KM, w[1] + 0.5 * KM, w[2] + 0.3 * KM), orders=[Order(ATTACK, 0, [alpha])])
    # afterwards: a shuttle running far off; destroyed, a convoy comes for it
    shuttle = FG(m, 'KESTREL', SHU, 1, REBEL, skill=2, cargo='OFFICERS', arrive=set_destroyed(2), delay=5,
                 start=(-9 * KM, 8 * KM, 1 * KM), way=[(-12 * KM, 14 * KM, 1 * KM)], home=(-16 * KM, 22 * KM, 1 * KM),
                 orders=[Order(GO_HOME, 10)], bonus=(DESTROYED, ALL), bonus_points=50)
    convoy = []
    for k, (x, y) in enumerate([(-14, 18), (-12, 19.5)]):
        convoy.append(FG(m, f'BRIGHT {k + 1}', FRT, 2, REBEL, skill=1, cargo='SUPPLIES', formation=2,
                         arrive=destroyed(shuttle), delay=15, start=(x * KM, y * KM, 0),
                         way=[(x * KM + 2 * KM, y * KM - 6 * KM, 0)], home=(x * KM + 8 * KM, y * KM - 14 * KM, 0),
                         orders=[Order(GO_HOME, 8)], bonus=(DESTROYED, ALL), bonus_points=25))
    guards = []
    for k, (x, y) in enumerate([(-15, 17), (-11, 20)]):
        guards.append(FG(m, f'GUARDIAN {k + 1}', CRV, 1, REBEL, skill=2, arrive=destroyed(shuttle), delay=15,
                         start=(x * KM, y * KM, 0.3 * KM), way=[(x * KM + 2 * KM, y * KM - 6 * KM, 0)],
                         home=(x * KM + 8 * KM, y * KM - 14 * KM, 0), orders=[Order(ATTACK, 0, [alpha])]))
    m.tail = [shuttle] + convoy + guards
    Message(m, 'PROWLER 1: Y-WINGS INBOUND. ALPHA, KEEP THEM OFF US!', Cond(ALWAYS), tag='YWINGS')
    Message(m, 'PELLAEON: THRAWN EXPECTS THREE WAVES. THEN SOMETHING MORE.', Cond(ALWAYS), after=10, tag='WAVES')
    Message(m, 'GOLD 1 LEAD: CORVETTES DEAD AHEAD. ARM TORPEDOES.', Cond(ALWAYS), after=25, tag='GOLD1')
    Message(m, 'PROWLER 3: OUR NOGHRI PASSENGERS GROW RESTLESS, ALPHA.', Cond(ALWAYS), after=45, tag='NOGHRI')
    Message(m, 'SECOND Y-WING FLIGHT ON SCOPE, ALPHA!', arrived(golds[1]), tag='SECOND')
    Message(m, 'LAST Y-WING FLIGHT, ALPHA. DO NOT LET THEM FIRE!', arrived(golds[2]), tag='LAST')
    Message(m, 'PROWLER 2: SOMETHING SMALL AT THE EDGE OF OUR SCOPE...', arrived(shuttle), after=3, tag='KESTREL')
    Message(m, 'PELLAEON: THEIR OFFICERS, RUNNING. JUST AS THE ADMIRAL SAID.', arrived(shuttle), after=15, tag='OFFICERS')
    Message(m, 'A REBEL CONVOY HAS COME FOR THE SHUTTLE. HIT THE FREIGHTERS!', arrived(convoy[0]), tag='CONVOY')
    Message(m, 'PELLAEON: THEY NEVER LEAVE THEIR OWN. THRAWN COUNTED ON IT.', arrived(convoy[0]), after=10, tag='COUNTED')
    return m


MISSIONS = [mission_shuttles, mission_corvettes, mission_escort]


# --- output ---------------------------------------------------------------------------------------------------------

def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def c_header(missions):
    out = ['// The TIE Fighter demo\'s campaign missions (docs/tiedemo.md section 8): made by scripts/tdmmissions.py from',
           '// its descriptions; do not edit.  Each is the demo\'s mission file layout (the header, the flight groups, the',
           '// radio messages), written by wcnet_tiedemo_campaign.cpp over the copy the game has just read.',
           '#ifndef WCNET_TIEDEMO_MISSIONS_H_', '#define WCNET_TIEDEMO_MISSIONS_H_', '',
           'namespace wc {', 'namespace tdm_missions {', '']
    for k, m in enumerate(missions):
        data = m.pack()
        out.append(f'// {m.title}: {len(m.fgs)} flight groups, {len(m.msgs)} messages')
        out.append(f'static const unsigned char kMission{k}[{len(data)}] = {{')
        for i in range(0, len(data), 24):
            out.append('    ' + ','.join(str(b) for b in data[i:i + 24]) + ',')
        out.append('};')
        for what, fgs in (('Tail', m.tail), ('Protect', m.protect)):
            listed = ','.join(str(fg.index) for fg in fgs)
            out.append(f'static const unsigned char kMission{k}{what}[] = {{ {listed + "," if listed else ""} 255 }};')
        timed = [t for t in (msg.timed() for msg in m.msgs) if t]
        listed = ' '.join(f'{a},{b},{c},' for a, b, c in timed)
        out.append(f'static const unsigned char kMission{k}Timed[] = {{ {listed} 255 }};   // (message, flight group '
                   f'or 255 the start, seconds)')
        out.append('')
    out.append('struct Mission {')
    out.append('    const char *key, *title, *brief;')
    out.append('    const unsigned char *data;')
    out.append('    unsigned size;')
    out.append('    int fgs, msgs;')
    out.append('    int minDelay, quiet, cap;          // the end rule (seconds; tdmmissions.py Mission)')
    out.append('    const unsigned char *tail;         // flight groups whose quiet the end waits for (255 ends it)')
    out.append('    const unsigned char *protect;      // flight groups whose craft must not die (255 ends it)')
    out.append('    const unsigned char *timed;        // the layer\'s radio lines: (message, flight group or 255, seconds)')
    out.append('};')
    out.append('static const Mission kMissions[] = {')
    for k, m in enumerate(missions):
        out.append(f'    {{ {c_string(m.key)}, {c_string(m.title)}, {c_string(m.brief)}, kMission{k}, sizeof(kMission{k}), '
                   f'{len(m.fgs)}, {len(m.msgs)}, {m.min_delay}, {m.quiet}, {m.cap}, kMission{k}Tail, kMission{k}Protect, '
                   f'kMission{k}Timed }},')
    out.append('};')
    out.append(f'static const int kMissionCount = {len(missions)};')
    out += ['', '}  // namespace tdm_missions', '}  // namespace wc', '', '#endif', '']
    return '\n'.join(out)


def decode(path):
    d = open(path, 'rb').read()
    nfg, nmsg, ngoal = struct.unpack_from('<hhh', d, 0)
    h = d[6:6 + 450]
    print(f"FGs {nfg} msgs {nmsg} goals {ngoal} time {h[0]}:{h[1]:02} wintype {h[2]}")
    for k, o in (('win', 0x10), ('win', 0x50), ('loss', 0x110), ('loss', 0x150)):
        s = h[o:o + 64].split(b'\0')[0]
        if s:
            print('  ', k, s.decode('latin1'))

    def cond(c):
        return f"{c[0]}/{c[1]}/{c[2]}/{c[3]}"
    for i in range(nfg):
        o = HEADER_SIZE + i * FG_SIZE
        f = d[o:o + FG_SIZE]
        if not f[0x32]:
            continue
        nm = f[0:12].split(b'\0')[0].decode('latin1')
        print(f"{i:2} {nm:11} {TYPE_NAMES.get(f[0x32], f[0x32]):3}x{f[0x33]} side{f[0x37]} skill{f[0x38]} warhead{f[0x35]} status{f[0x34]} "
              f"set{f[0x3E]} waves{f[0x40] + 1} player{f[0x42]} cargo {f[0x18:0x24].split(b'\0')[0].decode('latin1')!r}")
        print(f"     arrive {cond(f[0x4A:0x4E])} {'OR' if f[0x52] == 1 else 'AND'} {cond(f[0x4E:0x52])} after "
              f"{f[0x54]}:{f[0x55]:02}  mother {f[0x60]}/{f[0x61]}")
        for k in range(3):
            a = f[0x68 + 18 * k:0x68 + 18 * k + 18]
            if a[0] or any(a[1:]):
                print(f"     order{k} {a[0]} speed{a[1]} var{list(a[2:6])} targets {a[6]}:{a[8]} {a[7]}:{a[9]} "
                      f"{a[12]}:{a[13]} {a[14]}:{a[15]}")
        g = f[0x9E:0xA8]
        print(f"     goals primary {g[0]}/{g[1]} secondary {g[2]}/{g[3]} bonus {g[6]}/{g[7]} ({struct.unpack('b', g[8:9])[0]})")
        wx = struct.unpack_from('<15h', f, 0xA8)
        wy = struct.unpack_from('<15h', f, 0xC6)
        wz = struct.unpack_from('<15h', f, 0xE4)
        wu = struct.unpack_from('<15h', f, 0x102)
        print('     places', ' '.join(f"{k}:({wx[k]},{wy[k]},{wz[k]})" for k in range(15) if wu[k]))
    mo = HEADER_SIZE + nfg * FG_SIZE
    for k in range(nmsg):
        m = d[mo + MSG_SIZE * k: mo + MSG_SIZE * k + MSG_SIZE]
        print(f"msg{k} {cond(m[64:68])} {'OR' if m[89] == 1 else 'AND'} {cond(m[68:72])} delay {m[88]}: "
              f"{m[:64].split(b'\0')[0].decode('latin1')}")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == 'decode':
        decode(sys.argv[2])
        return
    if len(sys.argv) >= 2 and sys.argv[1] == 'build':
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        missions = [f() for f in MISSIONS]
        with open(os.path.join(root, 'src', 'cpu', 'wcnet_tiedemo_missions.h'), 'w') as fh:
            fh.write(c_header(missions))
        if len(sys.argv) >= 3:
            os.makedirs(sys.argv[2], exist_ok=True)
            for k, m in enumerate(missions):
                with open(os.path.join(sys.argv[2], f'{m.key.upper()}.TIE'), 'wb') as fh:
                    fh.write(m.pack())
        for m in missions:
            print(f'{m.title}: {len(m.fgs)} flight groups, {len(m.msgs)} messages, {len(m.pack())} bytes')
        return
    print(__doc__)
    sys.exit(2)


if __name__ == '__main__':
    main()
