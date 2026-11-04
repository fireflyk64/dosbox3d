#!/usr/bin/env python3
"""The checks of a RetroVega shared flight run by scripts/rvnet-duo.sh: its dumps (!ds=, the data segment of each
instance at a moment of its key script) and its logs.  Offsets come from src/cpu/wcnet_rv.h, the interface the game
and the layer are both built from: no address is written here.

    rvnet-check.py OUT PLAYERS SCENARIO

OUT/pN is instance N (p0 the host); its dumps in OUT/pN/dump/NAME.ds, the layer's log in OUT/pN/dosbox.log, the
game's own in OUT/pN/game/RVLOG.TXT.  Prints every failure and FAILED, or OK with what was seen; status 1 on failure.
"""
import math
import os
import re
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
HEADER = os.path.join(ROOT, 'src', 'cpu', 'wcnet_rv.h')
KM = 256
NEAR_KM = 100                   # a ship's copy within this of where its own machine has it (the ships are stopped)
ET_OFFSCENE, ET_SHIP, ET_CAPSHIP = 1, 12, 13
CTL_PUPPET, FLAG_HUMAN = 2, 32


def layout():
    defs = {}
    for line in open(HEADER):
        m = re.match(r'#define (RV_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b', line)
        if m:
            defs[m.group(1)] = int(m.group(2), 0)
    return defs


D = layout()
MAXSHIP = D['RV_MAX_SHIP_SLOT']


class Dump:
    def __init__(self, path):
        self.path = path
        self.b = open(path, 'rb').read()

    def u8(self, name, i=0, size=1):
        return self.b[D['RV_DS_' + name] + size * i]

    def u16(self, name, i=0):
        return struct.unpack_from('<H', self.b, D['RV_DS_' + name] + 2 * i)[0]

    def pos(self, slot):
        return struct.unpack_from('<iii', self.b, D['RV_DS_gPositionVector'] + 12 * slot)

    def etype(self, slot):
        return self.u16('maybeEntityType', slot)

    def ships(self):
        """The ship slots in use by ships: in the scene, or held off it (ET_OFFSCENE: far from every human)"""
        return [s for s in range(MAXSHIP + 1) if self.etype(s) in (ET_OFFSCENE, ET_SHIP, ET_CAPSHIP)]

    def table(self, name, size):
        at = D['RV_DS_' + name]
        return self.b[at:at + size]


def km(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b))) / KM


class Check:
    def __init__(self, out, players, scenario):
        self.out, self.players, self.scenario = out, players, scenario
        self.fails, self.seen = [], []

    def need(self, ok, what):
        (self.seen if ok else self.fails).append(what)
        return ok

    def dump(self, n, name):
        path = os.path.join(self.out, 'p%d' % n, 'dump', name + '.ds')
        if not os.path.exists(path):
            self.need(False, 'p%d has no dump %s' % (n, name))
            return None
        return Dump(path)

    def log(self, n):
        path = os.path.join(self.out, 'p%d' % n, 'dosbox.log')
        return open(path, errors='replace').read() if os.path.exists(path) else ''

    def game_log(self, n):
        path = os.path.join(self.out, 'p%d' % n, 'game', 'RVLOG.TXT')
        return open(path, errors='replace').read() if os.path.exists(path) else ''

    def meeting(self, tag):
        """The flight shared at the dumps called `tag`: one mission, by the host's records, every pilot where its
        own machine has it, every ship of the host's a puppet on a guest."""
        host = self.dump(0, tag)
        if not host:
            return
        self.need(host.u8('rvNetRole') == 1, '%s: the host says it is the host (rvNetRole %d)' % (tag, host.u8('rvNetRole')))
        self.need(host.u8('rvPlayerCount') == self.players,
                  '%s: the host counts %d pilots (rvPlayerCount %d)' % (tag, self.players, host.u8('rvPlayerCount')))
        for n in range(1, self.players):
            g = self.dump(n, tag)
            if not g:
                continue
            who = '%s: guest p%d' % (tag, n)
            self.need(g.u8('rvNetRole') == 2, '%s says it is a guest (rvNetRole %d)' % (who, g.u8('rvNetRole')))
            self.need((g.u8('currentSeries'), g.u8('currentMission')) == (host.u8('currentSeries'), host.u8('currentMission')),
                      "%s flies the host's mission (node %d mission %d; the host's %d %d)" % (
                          who, g.u8('currentSeries'), g.u8('currentMission'), host.u8('currentSeries'), host.u8('currentMission')))
            size = D['RV_MISSION_SHIPS'] * D['RV_MISSION_SHIP_SIZE']
            self.need(g.table('missionShipTable', size) == host.table('missionShipTable', size)
                      and g.u16('playerMissionShip') == host.u16('playerMissionShip'),
                      "%s flies by the host's mission records" % who)
            net = g.u8('rvNetId', 0)
            if not self.need(1 <= net <= MAXSHIP, '%s knows its network id (%d)' % (who, net)):
                continue
            self.need(host.u8('rvController', net) == CTL_PUPPET and host.u16('rvFlags', net) & FLAG_HUMAN,
                      "%s: the host's slot %d is its puppet, a human's (controller %d, flags %d)" % (
                          who, net, host.u8('rvController', net), host.u16('rvFlags', net)))
            d = km(host.pos(net), g.pos(0))
            self.need(d <= NEAR_KM, "%s: the host has it where it is (%.0f km off)" % (who, d))
            body = [s for s in g.ships() if s and g.u8('rvNetId', s) == 0]
            if self.need(len(body) == 1, "%s has the host's ship (%d slots with net id 0)" % (who, len(body))):
                s = body[0]
                self.need(g.u8('rvController', s) == CTL_PUPPET and g.u16('rvFlags', s) & FLAG_HUMAN,
                          "%s: the host's ship, slot %d, is a human's puppet" % (who, s))
                d = km(g.pos(s), host.pos(0))
                self.need(d <= NEAR_KM, "%s has the host where the host is (%.0f km off)" % (who, d))
            ai = [s for s in g.ships() if s and g.u8('rvController', s) != CTL_PUPPET]
            self.need(not ai, '%s: no copy of the host\'s ships is flown here (not puppets: %s)' % (who, ai))
            self.need(len(g.ships()) == len(host.ships()),
                      '%s has as many ships as the host (%d, the host %d)' % (who, len(g.ships()), len(host.ships())))

    def logs_say(self, n, pattern, what):
        return self.need(re.search(pattern, self.log(n)) is not None, 'p%d: %s' % (n, what))

    def logs_say_game(self, n, pattern, what):
        return self.need(re.search(pattern, self.game_log(n)) is not None, 'p%d: %s' % (n, what))

    def run(self):
        for n in range(self.players):
            self.need('RV ABI mismatch' not in self.log(n), 'p%d: the interface matched' % n)
        self.meeting('moved')
        # the pace: a step a frame at the host's rate (WCFPS, default 30), every pilot alike: the 'moved' dumps are
        # 8 s into each one's flight (rvnet-duo.sh)
        fps = int(os.environ.get('WCFPS') or 30)
        for n in range(self.players):
            d = self.dump(n, 'moved')
            if d:
                frames = d.u16('frameCounter')
                at = int(os.environ.get('RVNET_HOST_LOOK', '8')) if n == 0 else 8   # (the host's look comes later with a late guest)
                self.need(abs(frames - at * fps) <= fps, 'p%d flies at %d frames a second (%d frames in %d s)' % (n, fps, frames, at))
        sc = self.scenario
        if sc in ('retry', 'carrier', 'guestdeath'):
            self.logs_say(0, r'our choice on the death screen: fly the mission again', 'the host chose R')
            for n in range(1, self.players):
                self.logs_say(n, r"the host's choice on the death screen: fly the mission again", "the host's R reached it")
            self.meeting('again')
            host = self.dump(0, 'again')
            if host:
                self.need(host.u16('missionStatus') == 0, 'again: the host flies (status %d)' % host.u16('missionStatus'))
        if sc == 'carrier':
            for n in range(self.players):
                self.need(re.search(r'mission ended: lost the carrier|server ended the mission: lost the carrier',
                                    self.log(n)) is not None, 'p%d: the carrier lost, for everyone' % n)
        if sc == 'guestdeath':
            self.logs_say(0, r'player \d+ died; ending the mission for everyone', "the guest's death ended the host's flight")
        if sc == 'land':
            self.logs_say(0, r'player \d+ landed on slot \d+: everyone lands there', "the guest's landing is the host's")
            won = [re.findall(r'mission \S+ (won|lost)', self.game_log(n)) for n in range(self.players)]
            self.need(all(w for w in won) and len(set(w[-1] for w in won)) == 1,
                      'everyone scored the mission alike (%s)' % [w[-1] if w else None for w in won])
        if sc == 'autopilot':
            self.logs_say(1, r'replay autopilot', 'the guest replayed the autopilot')
            # the host's flyby on every guest while the host's runs (flight.c, autoAnimation_c): begun by the camera's
            # start, ended by the jump's end, most of its 90 frames long, and no jump of the guest's own
            self.need(re.search(r'flight: autopilot: on to nav', self.game_log(0)) is not None, 'the host jumped')
            for n in range(1, self.players):
                g = self.game_log(n)
                start = re.search(r'flight: autoAnimation\(\d+, 1, (\d+)\) at frame (\d+)', g)
                end = start and re.search(r'flight: autoAnimation\(0, 0, 0\) at frame (\d+)', g[start.end():])
                shown = start and re.search(r"flight: the view: the autopilot's flyby", g[start.end():])
                if self.need(start and end and shown, "p%d showed the host's flyby (its start, the view, its end)" % n):
                    frames = int(end.group(1)) - int(start.group(2))
                    self.need(int(start.group(1)) * 2 // 3 <= frames <= int(start.group(1)) + 60,
                              "p%d's flyby lasted as the host's (%d frames of %s)" % (n, frames, start.group(1)))
                self.need('flight: autopilot: on to nav' not in g, 'p%d did not jump by itself' % n)
            host, g = self.dump(0, 'moved'), self.dump(0, 'after')
            if host and g:
                self.need(km(host.pos(0), g.pos(0)) > 5000, 'the host went by autopilot (%.0f km)' % km(host.pos(0), g.pos(0)))
            # the guest, 7,800 km from the host when the host pressed A, put in the host's formation as the flyby began
            # (logic_form_up: 350 km off the right wing, 250 back) and come out of the jump beside it
            for n in range(1, self.players):
                ga = self.dump(n, 'after')
                if g and ga:
                    d = km(g.pos(0), ga.pos(0))
                    self.need(d < 1500, "p%d came out of the autopilot in the host's formation (%.0f km from the host; "
                              "its place 430)" % (n, d))
            self.meeting('after')
        if sc == 'ram':
            self.logs_say(1, r'reporting a hit by our own ship', "the guest's collisions went to the host")
            self.logs_say(0, r'reported damage to \d+', "the host took the guest's share")
        if sc == 'combat':
            self.logs_say(1, r'reporting a hit by our own guns', "the guest's hits went to the host")
            self.logs_say(0, r'reported damage to \d+', "the host took the guest's hits")
            self.logs_say(1, r'replay damage 1 -> ', "the host's word on the guest's hits came back")
        if sc == 'chat':
            self.logs_say_game(1, r'a chat line from slot \d+ on the comm: CHEATHOST: hello from the host',
                               "the host's line on the guest's comm")
            self.logs_say_game(0, r'a chat line from slot \d+ on the comm: GUEST: hi from the guest',
                               "the guest's line on the host's comm")
            self.logs_say_game(0, r'a chat line from slot 0 on the comm: CHEATHOST: hello from the host',
                               "the host's own line on its own comm")
        if sc == 'kill':
            self.meeting('gone')
            for n in range(1, self.players):
                self.logs_say(n, r'despawn slot \d+ explode=1', 'the host\'s kill reached the guest')
                dying = self.dump(n, 'dying')
                if dying:
                    states = [dying.u16('shipAiState', s) for s in dying.ships()]
                    self.need(9 in states, 'p%d: a ship dying (shipAiState 9) after the kill' % n)
        if self.fails:
            for f in self.fails:
                print('rvnet-check: FAIL ' + f)
            print('rvnet-check: FAILED (%s, %d pilots): %d of %d' % (self.scenario, self.players, len(self.fails),
                                                                       len(self.fails) + len(self.seen)))
            return 1
        print('rvnet-check: OK (%s, %d pilots): %d checks' % (self.scenario, self.players, len(self.seen)))
        return 0


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(Check(sys.argv[1], int(sys.argv[2]), sys.argv[3]).run())
