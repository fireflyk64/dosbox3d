#!/usr/bin/env python3
"""The game's logic data: campaign, missions and cast, checked and compiled.

    rvdata.py [--out build/dos/RVGAME.DAT] [--header src/rvdata.h] [--manifest build/dos/RVNET.JSON]

Reads data/campaign.csv, data/missions.txt, data/people.csv,
data/relations.csv, data/ships.csv and the seat art (art/places/*/place.json),
checks every reference and rule (docs/engine.md), and writes:

    RVGAME.DAT   fixed-size little-endian records the engine reads straight
                 into its tables: a header, the pilots, the campaign nodes,
                 the missions
    rvdata.h     the same records as packed C structs, the limits and the
                 special values, so the two can never disagree (the engine
                 also checks each record size against the header)

missions.txt is one block per mission, a keyword and its value per line,
'#' comments.  Keywords: mission, node, title, type, music, briefing, win,
lose, fighter (the player's and the wingman's TYPE), nav (id x y z [hidden] [reach=KM], km, the old game's map: the waypoints numbered from 1, then the hidden trigger points; within reach= of it the point is reached and the autopilot stops there, the old game's 10,000 to 32,500), ship (TYPE|player|wingman side navN [count=N] [role=out|in|home|stay] [jump=N] [turrets=off]
[pilot=KEY] [ai=PROFILE] [fear=c/d]), rules (hit= aim= hold= ram=: RULES), field (asteroid|mine x y z radius [density=N] [level=L], km; N rocks per (10,000 km)^3, 1..255,
0 or none: the engine's own; L the old game's rock level, 0..3: how many fly at the player, and when), bar and lounge
(seat=PILOT|- ...), and the lines, docs/engine.md: briefing, win, lose [WHO] [conditions] "line" (or plain text: the XO's,
split at its sentences), talk, say, radio.
"""
import argparse
import re
import csv
import json
import math
import os
import shlex
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, 'data')

VERSION = 21                            # 2: the ship types' engines; 3: their hit boxes and vitals; 4: the weapons
                                        # and each type's loadout; 5: each weapon's sound (data/sounds.csv); 6: the
                                        # ship types' slide speeds; 7: their sizes (radius_km); 8: their gun energy's
                                        # and shields' rates back; 9: the hit volumes (a type's box of the sphere's
                                        # volume, a weapon's radius, a mission's rules); 10: a weapon's damage at
                                        # the muzzle; 11: a type's shield back a point a side every so many steps;
                                        # 12: a weapon's cone, a mission ship's AI profile and fear;
                                        # 13: a mission ship's turrets=off; 14: scenes: 24 lines a
                                        # mission, 120 characters, conditions (say, radio, after=, if=, unless=, dead=);
                                        # 17: a ship type's fuel rates (fuel_cruise, fuel_burn); 18: a pilot's board seed and twin;
                                        # 19: a mission ship's jump=N (an escort target's last nav point)
                                        # 20: a field's level=L (the old game's rock level, dseg:BFF4)
                                        # 21: the map at the old game's own size: nav points, ship offsets and
                                        # fields in 32 bits of km (its farthest point 105,000 km out)
LIMITS = {'PILOTS': 16, 'NODES': 32, 'MISSIONS': 128, 'MISSIONS_PER_NODE': 4, 'NAVS': 8, 'SHIPS': 16,
          'SEATS': 3, 'TALKS': 200, 'FIELDS': 7, 'SHIPTYPES': 64, 'SHIPRECORDS': 44, 'WEAPONS': 40}
TEXT = {'ID': 16, 'TITLE': 48, 'TALK': 108, 'KEY': 10, 'CALLSIGN': 12,
        'NAME': 24, 'NODE_ID': 8, 'LINE': 72, 'CODE': 6, 'WNAME': 20}
FIELDS = {'asteroid': 9, 'mine': 10}                 # their EntityType, as fieldTable keeps them
MAP_KM = 1000000                                     # a place's km on an axis (positions are km x 256 in 32 bits: 8.3
                                                     # million km; the old game's farthest nav point is 105,000 out)
NONE, GOOD, BAD = 0xFF, 0xFE, 0xFD                   # node edges: a node index, or an ending
SHIP_PLAYER, SHIP_WINGMAN = 0xFF, 0xFE               # mission ship types chosen at run time
ROOMS = ['bar', 'lounge']
SEAT_NAMES = ['left', 'middle', 'right']
HOME_TIERS = ['intact', 'scarred', 'holed', 'wrecked']   # (damage_home_tier's order)
FIXED_PLACES = {'TECH': 'the deck', 'TALLY': 'the bar', 'XO': 'the briefing room'}
TYPES = {'patrol': 0}
# the music's cues, in their files' order: the rooms' songs (tools/songs.py, tools/midi.py), then the flight's
# (tools/flightsongs.py; src/score.c picks them), the stings last (played once: music_sting)
MUSIC_FILES = ['BRIEF', 'BAR', 'LAUNCH', 'WIN', 'LOSE', 'QUARTERS', 'LOUNGE',
               'CRUISE1', 'CRUISE2', 'CRUISE3', 'CRUISE4', 'CRUISE5', 'CRUISE6', 'CRUISE7', 'CRUISE8',
               'HOMEWARD', 'HOMELOSS', 'BATTLE1', 'BATTLE2', 'BATTLE3', 'ROCKS',
               'PANIC1', 'PANIC2', 'PANIC3', 'LOST', 'VICTORY1', 'VICTORY2', 'VICTORY3', 'TRAGEDY']
MUSIC = {'briefing': 0, 'bar': 1, 'launch': 2, 'win': 3, 'lose': 4, 'quarters': 5, 'lounge': 6}
MUSIC.update({f.lower(): i for i, f in enumerate(MUSIC_FILES) if i >= len(MUSIC)})
ROLES = {'pilot': 0, 'hero': 1, 'barman': 2, 'xo': 3, 'tech': 4}

# record layouts: (C type, field, struct format)
HEADER = [('char', 'magic[8]', '8s'), ('unsigned short', 'version', 'H'), ('unsigned short', 'header_size', 'H'),
          ('unsigned short', 'npilots', 'H'), ('unsigned short', 'pilot_size', 'H'), ('unsigned long', 'pilots_at', 'L'),
          ('unsigned short', 'nnodes', 'H'), ('unsigned short', 'node_size', 'H'), ('unsigned long', 'nodes_at', 'L'),
          ('unsigned short', 'nmissions', 'H'), ('unsigned short', 'mission_size', 'H'),
          ('unsigned long', 'missions_at', 'L'), ('unsigned short', 'start_node', 'H'),
          ('unsigned short', 'nshiptypes', 'H'), ('unsigned short', 'shiptype_size', 'H'),
          ('unsigned long', 'shiptypes_at', 'L'), ('unsigned short', 'nweapons', 'H'),
          ('unsigned short', 'weapon_size', 'H'), ('unsigned long', 'weapons_at', 'L'),
          ('unsigned short', 'nlines', 'H'), ('unsigned short', 'line_size', 'H'), ('unsigned long', 'lines_at', 'L'),
          ('unsigned short', 'home_line', 'H'), ('unsigned char', 'home_n[4]', '4s')]   # (Kit's words at the plate: the first, how many by tier)
# a ship type: data/ships.csv's row, its numbers as the simulation wants them
SHIPTYPE = [('char', 'key[RV_KEY]', '%ds' % TEXT['KEY']), ('char', 'name[RV_NAME]', '%ds' % TEXT['NAME']),
            ('unsigned char', 'type', 'B'), ('unsigned char', 'side', 'B'), ('unsigned char', 'flags', 'B'),
            ('unsigned char', 'cockpit', 'B'),
            ('unsigned short', 'cruise_kps', 'H'), ('unsigned short', 'max_kps', 'H'), ('unsigned short', 'ab_kps', 'H'),
            ('unsigned short', 'accel', 'H'), ('unsigned short', 'slide_kps', 'H'),
            ('unsigned short', 'slide_dmg_kps', 'H'), ('unsigned char', 'yaw_dps', 'B'),
            ('unsigned char', 'pitch_dps', 'B'),
            ('unsigned char', 'roll_dps', 'B'), ('unsigned char', 'guns', 'B'),
            ('unsigned short', 'shield_f', 'H'), ('unsigned short', 'shield_r', 'H'),
            ('unsigned short', 'armor_f', 'H'), ('unsigned short', 'armor_r', 'H'), ('unsigned short', 'armor_l', 'H'),
            ('unsigned short', 'armor_rt', 'H'), ('unsigned short', 'hull', 'H'), ('unsigned short', 'lod0_px', 'H'),
            ('unsigned short', 'radius_km', 'H'),
            ('unsigned short', 'missiles', 'H'), ('unsigned char', 'nengines', 'B'),
            ('unsigned char', 'gun_energy_steps', 'B'), ('unsigned char', 'shield_full_shift', 'B'),
            ('unsigned char', 'shield_steps', 'B'),
            ('unsigned short', 'fuel_cruise', 'H'), ('unsigned short', 'fuel_burn', 'H'),
            ('signed char', 'engine[RV_ENGINES][4]', '%db' % (4 * 4)),
            ('unsigned char', 'hit[3]', '3B'), ('unsigned char', 'box[3]', '3B'), ('unsigned char', 'core', 'B'),
            ('unsigned char', 'nvitals', 'B'),
            ('signed char', 'vital[RV_VITALS][3]', '%db' % (3 * 8)),
            ('unsigned char', 'loadout[RV_GUNTABLE]', '%ds' % 0x33)]
SHIPTYPE_FLAG_PLAYER = 1               # the player may fly it (a cockpit kit exists)
# what a bolt must do to hit a type (ships.csv `hit`; a mission's `rules hit=` overrides every type's: the user's
# ruling of 2026-10-31, a game or a mission picks its look and feel): blank, pass within its radius (the old game's
# volume, shots.c segment_in_sphere) or through its silhouettes; silhouette, the silhouettes alone; sphere, the
# radius alone; box, a box in the ship's frame of the sphere's volume (below).  Every test adds the bolt's own
# radius (weapons.csv radius_km) and takes the bolt's whole step (the segment from where it was to where it is)
SHIPTYPE_FLAG_HIT_SILHOUETTE = 2
SHIPTYPE_FLAG_HIT_SPHERE = 4
SHIPTYPE_FLAG_HIT_BOX = 8
HIT_MODES = {'': 0, 'silhouette': SHIPTYPE_FLAG_HIT_SILHOUETTE, 'sphere': SHIPTYPE_FLAG_HIT_SPHERE,
             'box': SHIPTYPE_FLAG_HIT_BOX}
# a mission's rules (missions.txt `rules`, at the top for every mission and in a mission for itself): hit=ships (each
# type's own) | silhouette | sphere | box; the flags: aim=lead (the AI leads its shots, scattered a little) | cone
# (the old game's: no lead, every ready gun whenever the nose is within the weapon's cone); hold=off | on (the old
# game's: the ship a player has selected holds its guns while that player is within 37 degrees ahead of its nose);
# ram=old (a collision takes the square of the closing speed in km a step, halved: the old game's, deadly) | soft
# (a touch's 2 points and a point per 125 kps closing)
RULE_AIM_CONE, RULE_HOLD_FIRE, RULE_RAM_SOFT = 1, 2, 4
RULES = {'hit': {'ships': 0, 'silhouette': SHIPTYPE_FLAG_HIT_SILHOUETTE, 'sphere': SHIPTYPE_FLAG_HIT_SPHERE,
                 'box': SHIPTYPE_FLAG_HIT_BOX},
         'aim': {'lead': 0, 'cone': RULE_AIM_CONE}, 'hold': {'off': 0, 'on': RULE_HOLD_FIRE},
         'ram': {'old': 0, 'soft': RULE_RAM_SOFT}}
BOLT_RADIUS_KM = 10                    # blank in weapons.csv for a gun or a turret: the old game's bolt (a missile 0: it
                                       # goes through the hull itself)
GUN_ENERGY_STEPS = 1                   # blank in ships.csv: the gun energy comes back a point every so many steps (a
                                       # point a step: the old game's Hornet held two lasers at a volley every third
                                       # of a second for a minute, which two guns at 5 a shot and a refire of 10 are
                                       # exactly, tests/wc1/README.md)
SHIELD_FULL_SHIFT = 9                  # blank in ships.csv: a shield comes back from nothing in 1 << this steps (512:
                                       # 17 s at LOGIC_HZ, the old game's 15 s), whatever its size (2 points to 900)
# the afterburner's fuel, as the old game counts it (its code, read 2026-11-02: a type's tank at kShipStats +0x0C, 5 a
# frame while the throttle is open, ovr143:33E3, and 200 more a frame on the afterburner, ovr141:1B38; a frame is a
# fifteenth of a second, as it ran on a machine of its day): ships.csv `fuel` is the tank in its units, blank the old
# game's 200,000 (the Hornet's and every non-Confed type's; the Rapier 250,000, the Scimitar 280,000, the Raptor
# 300,000).  The game's tank is 1,000 points (the gauge's tenths of a percent): a type's rates are thousandths of a
# point a step (LOGIC_HZ), so a Hornet's tank is 65 s of afterburner and 44 minutes of throttle, as the old game's
FUEL_TANK = 200000
FUEL_CRUISE, FUEL_BURN, FUEL_HZ, LOGIC_HZ = 5, 200, 15, 30   # (its frames: 15 a second on a machine of its day, world.h WC_FPS)


def fuel_rates(r):
    """a ships.csv row's (fuel_cruise, fuel_burn): thousandths of the game's 1,000-point tank a step with the throttle
    open, and more on the afterburner"""
    tank = int(r.get('fuel') or FUEL_TANK)
    if not 20000 <= tank <= 10000000:
        raise Fail('ships.csv %s: fuel %d is not 20,000 to 10,000,000 (the old game\'s units: a Hornet 200,000)'
                   % (r['key'], tank))
    rate = lambda per_frame: int(round(1000.0 * 1000 * per_frame * FUEL_HZ / (LOGIC_HZ * tank)))
    return rate(FUEL_CRUISE), rate(FUEL_BURN)
# a ship type's engines (data/engines.csv, tools/engines.py): x, y, z in its sprites' box (127 = the half box; +x
# the ship's left, +z the nose) and the glow's radius, min(0.3 size, 0.09) of the half box, in 1/1024ths
ENGINES = 4
# its hit box and vital points (data/hitboxes.csv, tools/hitboxes.py): the box's half extents across, up and along
# (127 = the sprites' half box), its core's radius (the biggest sphere about its middle inside its sprite at every
# view), and points (mounts, nose, tail, wingtips) in the engines' frame
VITALS = 8
# its loadout (ships.csv's guns, turrets and missiles columns: "2xPRT 2xION", "rear:2xLAS", "2xTOR 2xHS"): the gun
# table a ship of the type starts with, as the slot's gunTable keeps it (spec 4.5): a count, then GUNS records of
# 5 bytes: the weapon's type (a word), the mount across and up (int8, 1/32 of the ship's half box: collide_half),
# the state (bit 7 destroyed; a gun's cooldown frames, a launcher's rounds).  A gun and a turret are a record each,
# a missile kind one launcher with its rounds.  The mounts are placeholders: a type's guns spread evenly over the
# middle 60% of its hit box's width, a little below its middle; turrets above, launchers under the belly
GUNTABLE, GUNS = 0x33, 10
# a weapon (data/weapons.csv): its kind, look (how its shot is drawn), lock, and numbers; range in km (the csv's
# thousands of km)
WEAPON = [('char', 'code[RV_CODE]', '%ds' % TEXT['CODE']), ('char', 'name[RV_WNAME]', '%ds' % TEXT['WNAME']),
          ('unsigned char', 'type', 'B'), ('unsigned char', 'kind', 'B'), ('unsigned char', 'look', 'B'),
          ('unsigned char', 'lock', 'B'), ('unsigned short', 'damage', 'H'), ('unsigned short', 'speed_kps', 'H'),
          ('unsigned short', 'range_km', 'H'), ('unsigned char', 'refire', 'B'), ('unsigned char', 'energy', 'B'),
          ('unsigned char', 'lock_tenths', 'B'), ('unsigned char', 'sfx', 'B'), ('unsigned char', 'radius_km', 'B'),
          ('unsigned short', 'damage_near', 'H'), ('unsigned char', 'cone_cos', 'B')]
GUN_CONE_DEG = 46                      # blank in weapons.csv for a gun: the old game's laser cone, within which an AI
                                       # fires it under the cone rule (its neutron 37, mass driver 32, flak 84)
# the flight's sounds (data/sounds.csv: tools/sfx.py makes them, SFX.DAT): a weapon's (its sfx column) and the
# effects' (src/fx.c) by name; RV_SFX_NONE none
SOUNDS = [r['name'] for r in csv.DictReader(open(os.path.join(DATA, 'sounds.csv')))]
SFX_NONE = 0xFF
WEAPON_KINDS = ['gun', 'turret', 'missile', 'torpedo', 'rocket', 'mine', 'special']
WEAPON_LOOKS = ['bolt', 'slug', 'plasma', 'fire', 'star', 'beam', 'missile', 'none']   # slug .. star: BALL_* + 1
WEAPON_LOCKS = ['none', 'heat', 'image', 'auto', 'proximity']   # (an empty cell: none)
PILOT = [('char', 'key[RV_KEY]', '%ds' % TEXT['KEY']), ('char', 'callsign[RV_CALLSIGN]', '%ds' % TEXT['CALLSIGN']),
         ('char', 'name[RV_NAME]', '%ds' % TEXT['NAME']), ('unsigned char', 'role', 'B'),
         ('unsigned char', 'kills', 'B'), ('unsigned char', 'twin', 'B'), ('char', 'line[RV_LINE]', '%ds' % TEXT['LINE'])]
                                        # (kills: people.csv's, the splashes a pilot brings to the board at a new campaign, by
                                        # seniority and story; twin: the pilot index of its twin, relations.csv `twins`, 255
                                        # none: the board puts the two on one row, the user's)
NODE = [('char', 'id[RV_NODE_ID]', '%ds' % TEXT['NODE_ID']), ('unsigned char', 'layer', 'B'),
        ('signed char', 'col', 'b'), ('unsigned short', 'missions[RV_MISSIONS_PER_NODE]', '4H'),
        ('unsigned char', 'good', 'B'), ('unsigned char', 'bad', 'B'), ('unsigned char', 'outside', 'B'),
        ('unsigned char', 'pad', 'B')]
NAV = [('unsigned char', 'id', 'B'), ('unsigned char', 'flags', 'B'), ('long', 'x', 'l'), ('long', 'y', 'l'),
       ('long', 'z', 'l'), ('unsigned short', 'reach', 'H')]   # (km, the old game's map: tests/wc1/fullscale.py; reach:
                                        # within it the point is reached, and the autopilot stops at it: the old record's)
NAV_REACH = 15000                       # a nav line's reach= when it has none (the old game's most usual for a waypoint)
SHIP = [('unsigned char', 'type', 'B'), ('unsigned char', 'side', 'B'), ('unsigned char', 'nav', 'B'),
        ('unsigned char', 'count', 'B'), ('unsigned char', 'pilot', 'B'), ('unsigned char', 'role', 'B'),
        ('unsigned char', 'ai', 'B'), ('unsigned char', 'fear', 'B'), ('unsigned char', 'noturrets', 'B'),
        ('unsigned char', 'jump', 'B'),  # (role=out's jump=N: the nav point it jumps short of, 0 the next after its own)
        ('long', 'x', 'l'), ('long', 'y', 'l'), ('long', 'z', 'l')]   # (where it starts off its nav point, km: at=x,y,z)
# a mission ship's AI profile (missions.txt ai=NAME: logic.c's P_* in this order, 1-based in the record, 0 the type's
# own) and its own fear rates (fear=c/d, 0-15 each: the old game's class bytes; 0: the profile's)
PROFILES = ['interceptor', 'fighter', 'heavy', 'ace', 'bomber', 'wingman', 'capital', 'waiter', 'noleft']
ESCORT_ROLES = {'out': 1, 'in': 2, 'home': 3, 'stay': 4}   # a mission ship's role (RV_ESCORT_*): an escort target
                                        # flies out to the next nav point and jumps there, or waits at its own until the
                                        # player comes and flies home; `home` is the carrier the player lands on: it holds
                                        # station; `stay`, a fighter of the player's side that stays at its nav point (the
                                        # carrier's patrol, the old game's): the autopilot leaves it, and with nobody to
                                        # fight it flies round the capital ship nearest it (the user, 2026-11-05)
FIELD = [('unsigned char', 'type', 'B'), ('unsigned char', 'density', 'B'), ('long', 'x', 'l'), ('long', 'y', 'l'), ('long', 'z', 'l'),
         ('short', 'radius', 'h'), ('unsigned char', 'level', 'B')]
TALK = [('unsigned char', 'room', 'B'), ('unsigned char', 'speaker', 'B'), ('unsigned char', 'listener', 'B'),
        ('unsigned char', 'flags', 'B'), ('unsigned char', 'arg', 'B'), ('unsigned char', 'boost', 'B'),
        ('char', 'text[RV_TALK]', '%ds' % TEXT['TALK'])]
# a line's flags: a conversation's (say) and not table talk; shown only after the last mission was won, or lost; only
# with system `arg` flown, or not flown; only with pilot `arg` dead.  A line with a dead pilot in it is never shown
TALK_FLAGS = {'SAY': 1, 'WON': 2, 'LOST': 4, 'IF': 8, 'UNLESS': 16, 'DEAD': 32, 'ALIVE': 64, 'ESCORT': 128}
SPEAKER_YOU = 0xFE                      # the player, in a conversation
# what Kit does to the fighter on a line of hers that plays (say deck ... boost=KIND): logic_boost
TALK_BOOSTS = {'armor': 1, 'fuel': 2, 'missile': 3}
# the rooms of the lines past the seated ones (RV_ROOM_RADIO = len(ROOMS), the wingman at the launch; then the deck)
BRIEF_ROOMS = {'briefing': len(ROOMS) + 2, 'win': len(ROOMS) + 3, 'lose': len(ROOMS) + 4}
MISSION = [('char', 'id[RV_ID]', '%ds' % TEXT['ID']), ('unsigned char', 'node', 'B'), ('unsigned char', 'type', 'B'),
           ('unsigned char', 'music', 'B'), ('unsigned char', 'nnav', 'B'), ('unsigned char', 'nship', 'B'),
           ('unsigned char', 'ntalk', 'B'), ('unsigned char', 'nfield', 'B'), ('unsigned char', 'fighter', 'B'),
           ('char', 'title[RV_TITLE]', '%ds' % TEXT['TITLE']),
           ('RvNav', 'nav[RV_NAVS]', None), ('RvShip', 'ship[RV_SHIPS]', None),
           ('unsigned char', 'seat[RV_ROOMS][RV_SEATS]', '6B'), ('unsigned char', 'wipe', 'B'), ('unsigned char', 'hit', 'B'),
           ('unsigned char', 'rules', 'B'),
           ('unsigned short', 'talk0', 'H'), ('RvField', 'field[RV_FIELDS]', None)]


def fmt(fields):
    return '<' + ''.join(f for _t, _n, f in fields if f)


# campaign.csv's outside: what a room's window shows in that system (flight.c): one far thing, partly hidden; asteroids; one
# well off to the side, its edge showing; nothing but stars
OUTSIDE = ['', 'rocks', 'edge', 'empty']


class Fail(Exception):
    pass


def text(s, n, where):
    b = s.encode('cp437')
    if len(b) >= n:
        raise Fail('%s: %d characters, at most %d' % (where, len(b), n - 1))
    return b


def load_csv(name):
    with open(os.path.join(DATA, name)) as f:
        return list(csv.DictReader(f))


def parse_rules(where, val, into):
    """a `rules` line's key=value pairs (RULES) into a dict"""
    for kv in val.split():
        k, _, v = kv.partition('=')
        if k not in RULES or v not in RULES[k]:
            raise Fail('%s: rules are %s' % (where, ', '.join('%s=%s' % (k, '|'.join(RULES[k])) for k in RULES)))
        into[k] = v
    return into


def split_sentences(val, most):
    """Plain text as lines of at most `most` characters, each ending a sentence where it can."""
    out, cur = [], ''
    for word in val.split():
        if cur and len(cur) + 1 + len(word) > most:
            out.append(cur)
            cur = word
        else:
            cur = cur + ' ' + word if cur else word
        if cur.endswith(('.', '!', '?')) and len(cur) > most // 2:
            out.append(cur)
            cur = ''
    return out + [cur] if cur else out


def parse_missions(path):
    missions, cur, defaults = [], None, {}
    for lineno, raw in enumerate(open(path), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        key, _, val = line.partition(' ')
        val = val.strip()
        where = '%s:%d' % (os.path.basename(path), lineno)
        if key == 'mission':
            cur = {'id': val, 'where': where, 'nav': [], 'ship': [], 'talk': [], 'seats': {}, 'field': [],
                   'rules': dict(defaults)}
            missions.append(cur)
        elif key == 'rules':            # (before any mission: every mission's; in one: its own)
            parse_rules(where, val, defaults if cur is None else cur['rules'])
        elif cur is None:
            raise Fail('%s: %s before any mission' % (where, key))
        elif key in ('briefing', 'win', 'lose'):
            # (the XO's words, plain and split at the sentences; or, quoted, a line: [WHO] [conditions] "line")
            for line_ in ([shlex.split(val)] if '"' in val else [[t] for t in split_sentences(val, TEXT['TALK'] - 1)]):
                cur['talk'].append((where, key, line_))
        elif key in ('node', 'title', 'type', 'music', 'fighter', 'wipe'):
            cur[key] = val
        elif key == 'nav':
            cur['nav'].append((where, val.split()))
        elif key == 'ship':
            cur['ship'].append((where, val.split()))
        elif key == 'field':
            cur['field'].append((where, val.split()))
        elif key in ROOMS:
            cur['seats'][key] = (where, dict(kv.split('=', 1) for kv in val.split()))
        elif key in ('talk', 'say', 'radio'):   # (one list: their order is the order they are said in)
            cur['talk'].append((where, key, shlex.split(val)))
        else:
            raise Fail('%s: unknown keyword %r' % (where, key))
    return missions


def weapon_types():
    """the size of the game's table of weapons by number (campaign.c): the power of two above weapons.csv's highest"""
    return 1 << max(int(r['type'], 16) for r in load_csv('weapons.csv')).bit_length()


def compile_weapons():
    """data/weapons.csv: {code: (type, kind)} and the packed records"""
    weapons, out = {}, []
    rows = load_csv('weapons.csv')
    if len(rows) > LIMITS['WEAPONS']:
        raise Fail('weapons.csv: %d weapons, at most %d' % (len(rows), LIMITS['WEAPONS']))
    nums = [int(r['type'], 16) for r in rows]
    if len(set(nums)) != len(nums) or max(nums) > 0xFF:
        raise Fail('weapons.csv: type numbers must be unique and fit a byte (the game looks a weapon up by its number)')
    for r in rows:
        where = 'weapons.csv %s' % r['code']
        if r['kind'] not in WEAPON_KINDS or r['look'] not in WEAPON_LOOKS or (r['lock'] or 'none') not in WEAPON_LOCKS:
            raise Fail('%s: kind %r, look %r or lock %r unknown' % (where, r['kind'], r['look'], r['lock']))
        t = int(r['type'], 16)
        if r['sfx'] and r['sfx'] not in SOUNDS:
            raise Fail('%s: no sound %r in sounds.csv' % (where, r['sfx']))
        # a shot's own radius, km (the old game's bolt 10: it hits within its radius plus the ship's); blank: a gun's or
        # a turret's BOLT_RADIUS_KM, anything else's 0
        radius = int(r.get('radius_km') or (BOLT_RADIUS_KM if r['kind'] in ('gun', 'turret') else 0))
        if not 0 <= radius <= 255:
            raise Fail('%s: radius_km %d is not 0 to 255' % (where, radius))
        # what a hit does at the muzzle (damage_near) and at the end of its reach (damage), a straight line between by
        # the shot's life left (the old game's: a laser 10 at the muzzle, 25 at the end; blank: the same all the way)
        near = int(r.get('damage_near') or r['damage'])
        cone = float(r.get('cone_deg') or (GUN_CONE_DEG if r['kind'] == 'gun' else 0))
        if not 0 <= cone <= 180:
            raise Fail('%s: cone_deg %g is not 0 to 180' % (where, cone))
        cone_cos = max(0, min(255, int(round(math.cos(math.radians(cone)) * 256)))) if cone else 255
        weapons[r['code']] = (t, r['kind'])
        out.append(struct.pack(fmt(WEAPON), text(r['code'], TEXT['CODE'], where), text(r['name'], TEXT['WNAME'], where),
                               t, WEAPON_KINDS.index(r['kind']), WEAPON_LOOKS.index(r['look']),
                               WEAPON_LOCKS.index(r['lock'] or 'none'), int(r['damage']), int(r['speed_kps']),
                               int(round(float(r['range_km']) * 1000)), int(r['refire_frames'] or 0),
                               int(r['energy'] or 0), int(round(float(r['lock_s'] or 0) * 10)),
                               SOUNDS.index(r['sfx']) if r['sfx'] else SFX_NONE, radius, near, cone_cos))
    return weapons, out


def volume_box(hit):
    """the box of the bounding sphere's volume (the user's hit volume of 2026-10-31, for the thin ships): the
    silhouettes' width and length, and the height that gives the box the sphere's volume, never under the
    silhouettes' own and never over the radius.  127ths of the radius, as `hit` is"""
    hx, hy, hz = (v / 127 for v in hit)
    grown = math.pi / (6 * hx * hz)     # (4/3 pi r^3 = 8 hx hy hz r^3)
    return [hit[0], max(hit[1], min(127, int(round(grown * 127)))), hit[2]]


def compile_loadout(r, weapons, hit):
    """a ships.csv row's guns, turrets and missiles: (guns, missile rounds, the gun table's bytes)"""
    recs, guns, rounds = [], 0, 0
    for col in ('guns', 'turrets', 'missiles'):
        for tok in r[col].split():
            place, _, tok = tok.rpartition(':')
            try:
                count, code = tok.split('x', 1)
                count = int(count)
            except ValueError:
                raise Fail('ships.csv %s %s: %r is not NxCODE' % (r['key'], col, tok))
            if code not in weapons:
                raise Fail('ships.csv %s %s: no weapon %s in weapons.csv' % (r['key'], col, code))
            t, kind = weapons[code]
            if col == 'missiles':
                recs.append((t, 0, -8, min(count, 127)))       # a launcher under the belly, its rounds
                rounds += count
                continue
            for i in range(count):                             # a record a gun, spread across the middle; an odd
                x = (2 * i + 1 - count) / max(count, 1) * 0.6 * hit[0] / 127 * 32   # count's middle one on top (the
                up = 12 if col == 'turrets' else 8 if count % 2 and i == count // 2 else -2   # Schroedinger's third laser)
                recs.append((t, int(round(x)), up, 0))
            guns += count if col == 'guns' else 0
    if len(recs) > GUNS:
        print('rvdata: ships.csv %s: %d guns and launchers, the gun table keeps %d' % (r['key'], len(recs), GUNS),
              file=sys.stderr)
        recs = recs[:GUNS]
    table = bytes([len(recs)]) + b''.join(struct.pack('<Hbbb', *rec) for rec in recs)
    return guns, rounds, table + bytes(GUNTABLE - len(table))


def compile_all():
    people = load_csv('people.csv')

    twin_of = {}
    for r in load_csv('relations.csv'):
        if r['relation'] == 'twins':
            keys = [p['key'] for p in people]
            if r['a'] not in keys or r['b'] not in keys:
                raise Fail('relations.csv: twins %s and %s are not both in people.csv' % (r['a'], r['b']))
            twin_of[r['a']], twin_of[r['b']] = keys.index(r['b']), keys.index(r['a'])

    def seed_kills(p):
        k = int(p.get('kills') or 0)
        if not 0 <= k <= 99:
            raise Fail('people.csv %s: kills %d is not 0 to 99 (the board\'s marks a pilot starts with)' % (p['key'], k))
        return k
    pilot_index = {p['key']: i for i, p in enumerate(people)}
    if len(people) > LIMITS['PILOTS']:
        raise Fail('people.csv: %d pilots, at most %d' % (len(people), LIMITS['PILOTS']))
    ships_csv = load_csv('ships.csv')
    ships = {r['key']: int(r['type'], 16) for r in ships_csv}
    cockpit_keys = list(json.load(open(os.path.join(DATA, 'cockpits.json')))['ships'])
    if len(ships_csv) > LIMITS['SHIPRECORDS']:   # (SHIPTYPES is how high their numbers go; each record costs conventional memory)
        raise Fail('ships.csv: %d types, room for %d (LIMITS SHIPRECORDS)' % (len(ships_csv), LIMITS['SHIPRECORDS']))
    nums = [int(r['type'], 16) for r in ships_csv]
    if len(set(nums)) != len(nums) or max(nums) >= LIMITS['SHIPTYPES']:
        raise Fail('ships.csv: type numbers must be unique and below 0x%02X (the game looks a type up by its number)'
                   % LIMITS['SHIPTYPES'])
    weapons, out_weapons = compile_weapons()
    out_types = []
    engines_csv = {r['key']: r for r in load_csv('engines.csv')}
    hits_csv = {r['key']: r for r in load_csv('hitboxes.csv')}
    for r in ships_csv:
        n = lambda k: int(r[k] or 0)
        eng = []
        e = engines_csv.get(r['key'])
        for i in range(int(e['n']) if e else 0):
            x, y, z, size = (float(e['%s%d' % (c, i)]) for c in ('x', 'y', 'z', 'size'))
            clamp = lambda v: max(-127, min(127, int(round(v * 127))))
            eng += [clamp(x), clamp(y), clamp(z), max(1, int(round(min(0.3 * size, 0.09) * 1024)))]
        nengines = len(eng) // 4
        eng += [0] * (4 * ENGINES - len(eng))
        h = hits_csv.get(r['key'])
        clamp = lambda v: max(-127, min(127, int(round(v * 127))))
        hit = [max(1, clamp(float(h[c]))) for c in ('hx', 'hy', 'hz')] if h else [90, 40, 120]
        core = clamp(float(h['core'])) if h else 0
        vit = []
        for i in range(int(h['n']) if h else 0):
            vit += [clamp(float(h['%s%d' % (c, i)])) for c in ('x', 'y', 'z')]
        nvitals = len(vit) // 3
        vit += [0] * (3 * VITALS - len(vit))
        guns, rounds, loadout = compile_loadout(r, weapons, hit)
        for k in ('yaw_dps', 'pitch_dps', 'roll_dps'):
            if n(k) > 255:
                raise Fail('ships.csv %s: %s %d does not fit a byte' % (r['key'], k, n(k)))
        # the ship's size: its half box, km (the old game's measured radii for its counterparts, the user's call of
        # 2026-10-30: 100 for a light fighter, 700 for the carrier); blank: by the sprite's size, a light fighter's
        # 128 px as 100 km, the carrier's 640 as 700
        if (r.get('hit') or '') not in HIT_MODES:
            raise Fail('ships.csv %s: hit is blank (within its radius or through its silhouettes), silhouette, sphere or '
                       'box' % r['key'])
        radius = n('radius_km') or (n('lod0_px') * (700 if int(r['type'], 16) >= 0x20 else 100) // (640 if int(r['type'], 16) >= 0x20 else 128))
        # the speed above which a full-rate turn slides, and that with the acceleration absorbers damaged; blank:
        # its top speed (it slides on the afterburner only), and its cruise
        slide, slide_dmg = n('slide_kps') or n('max_kps'), n('slide_dmg_kps') or n('cruise_kps')
        # how fast its gun energy and shields come back: step_timers divides by the one and shifts a 16-bit phase by
        # the other
        energy_steps = int(r['gun_energy_steps'] or GUN_ENERGY_STEPS)
        full_shift = int(r['shield_full_shift'] or SHIELD_FULL_SHIFT)
        # the old game's shield stat (read in its code, 2026-10-31): a point back on each side every shield_steps
        # steps (the Hornet 5, the Dralthi 6: a 40-point shield in 7 s); blank 0: by shield_full_shift instead
        shield_steps = int(r.get('shield_steps') or 0)
        if not 1 <= energy_steps <= 255:
            raise Fail('ships.csv %s: gun_energy_steps %d is not 1 to 255' % (r['key'], energy_steps))
        if not 0 <= full_shift <= 15:
            raise Fail('ships.csv %s: shield_full_shift %d is not 0 to 15' % (r['key'], full_shift))
        if not 0 <= shield_steps <= 255:
            raise Fail('ships.csv %s: shield_steps %d is not 0 to 255' % (r['key'], shield_steps))
        for k in ('shield_f', 'shield_r'):      # (refill adds one to the low bits of a 16-bit product)
            if n(k) > 32768:
                raise Fail('ships.csv %s: %s %d is over 32768' % (r['key'], k, n(k)))
        fuel_cruise, fuel_burn = fuel_rates(r)
        out_types.append(struct.pack(fmt(SHIPTYPE), text(r['key'], TEXT['KEY'], 'ships.csv'),
                                     text(r['name'], TEXT['NAME'], 'ships.csv'), int(r['type'], 16), n('side'),
                                     (SHIPTYPE_FLAG_PLAYER if r['key'] in cockpit_keys else 0)
                                     | HIT_MODES[r.get('hit') or ''],
                                     cockpit_keys.index(r['key']) if r['key'] in cockpit_keys else 0xFF,
                                     n('cruise_kps'), n('max_kps'), n('ab_kps'), n('accel'), slide, slide_dmg,
                                     n('yaw_dps'), n('pitch_dps'), n('roll_dps'), guns, n('shield_f'), n('shield_r'),
                                     n('armor_f'), n('armor_r'), n('armor_l'), n('armor_rt'), n('hull'),
                                     n('lod0_px'), radius, rounds, nengines, energy_steps, full_shift, shield_steps,
                                     fuel_cruise, fuel_burn, *eng, *hit,
                                     *volume_box(hit), core, nvitals, *vit, loadout))
    talk_ok = {frozenset((r['a'], r['b'])): r['talk'] == 'yes' for r in load_csv('relations.csv')}
    for pair in talk_ok:
        for k in pair:
            if k not in pilot_index:
                raise Fail('relations.csv: unknown pilot %s' % k)
    seat_art = {}
    for room in ROOMS:
        with open(os.path.join(ROOT, 'art', 'places', room, 'place.json')) as f:
            seat_art[room] = {s: set(v) for s, v in json.load(f)['seats'].items()}

    nodes = load_csv('campaign.csv')
    node_index = {n['node']: i for i, n in enumerate(nodes)}
    if len(nodes) > LIMITS['NODES']:
        raise Fail('campaign.csv: %d nodes, at most %d' % (len(nodes), LIMITS['NODES']))
    missions = parse_missions(os.path.join(DATA, 'missions.txt'))
    mission_index = {m['id']: i for i, m in enumerate(missions)}
    if len(missions) > LIMITS['MISSIONS'] or len(mission_index) != len(missions):
        raise Fail('missions.txt: %d missions (at most %d), ids must be unique' % (len(missions), LIMITS['MISSIONS']))

    def edge(v, where):
        if v in ('GOOD', 'BAD'):
            return {'GOOD': GOOD, 'BAD': BAD}[v]
        if v not in node_index:
            raise Fail('%s: edge to unknown node %s' % (where, v))
        return node_index[v]

    out_nodes = []
    for n in nodes:
        where = 'campaign.csv %s' % n['node']
        ms = n['missions'].split()
        if not 1 <= len(ms) <= LIMITS['MISSIONS_PER_NODE']:
            raise Fail('%s: %d missions, 1 to %d' % (where, len(ms), LIMITS['MISSIONS_PER_NODE']))
        for m in ms:
            if m not in mission_index:
                raise Fail('%s: unknown mission %s' % (where, m))
            if missions[mission_index[m]].get('node') != n['node']:
                raise Fail('%s: mission %s says it belongs to %s' % (where, m, missions[mission_index[m]].get('node')))
        out_nodes.append(struct.pack(fmt(NODE), text(n['node'], TEXT['NODE_ID'], where), int(n['layer']),
                                     int(n['col']), *([mission_index[m] for m in ms] + [0xFFFF] * (LIMITS['MISSIONS_PER_NODE'] - len(ms))),
                                     edge(n['good'], where), edge(n['bad'], where), OUTSIDE.index(n.get('outside') or ''), 0))
        if n['good'] == n['bad']:
            print('%s: both ways out go to %s' % (where, n['good']), file=sys.stderr)
    check_dag(nodes)

    out_missions = []
    out_lines = []
    for m in missions:
        w = m['where']
        for k in ('node', 'title', 'type', 'music'):
            if k not in m:
                raise Fail('%s: mission %s has no %s' % (w, m['id'], k))
        if m['node'] not in node_index:
            raise Fail('%s: unknown node %s' % (w, m['node']))
        if m['type'] not in TYPES or m['music'] not in MUSIC:
            raise Fail('%s: type %r / music %r unknown' % (w, m['type'], m['music']))
        navs = []
        for where, f in m['nav']:
            # nav ID x y z [hidden] [reach=KM]: the player within KM of it has reached it, and the autopilot stops there
            words = [o for o in f[4:] if '=' not in o]
            opts = dict(o.split('=', 1) for o in f[4:] if '=' in o)
            if len(f) < 4 or words not in ([], ['hidden']) or set(opts) - {'reach'} \
                    or not 1000 <= int(opts.get('reach', NAV_REACH)) <= 60000:
                raise Fail('%s: nav ID x y z [hidden] [reach=KM, 1,000 to 60,000]' % where)
            navs.append(struct.pack(fmt(NAV), int(f[0]), 1 if words else 0, int(f[1]), int(f[2]), int(f[3]),
                                    int(opts.get('reach', NAV_REACH))))
        if len(navs) > LIMITS['NAVS']:
            raise Fail('%s: %d nav points, at most %d' % (w, len(navs), LIMITS['NAVS']))
        nav_ids = {0} | {int(f[0]) for _w, f in m['nav']}
        hidden_navs = {int(f[0]) for _w, f in m['nav'] if 'hidden' in f[4:]}
        listed = [int(f[0]) for _w, f in m['nav']]
        if any(not 1 <= i <= 15 for i in listed) or len(set(listed)) != len(listed):
            raise Fail('%s: nav points are numbered 1..15, each once (navPointTable has a record a number; 0 is the '
                       'launch point)' % w)
        for where, f in m['nav']:
            if any(not -MAP_KM <= int(v) <= MAP_KM for v in f[1:4]):
                raise Fail('%s: a nav point is at most %d km out on each axis' % (where, MAP_KM))
        shipr, wingman = [], None
        for where, f in m['ship']:
            opts = dict(o.split('=', 1) for o in f[3:])
            kind = f[0]
            tnum = SHIP_PLAYER if kind == 'player' else SHIP_WINGMAN if kind == 'wingman' else ships.get(kind)
            if tnum is None:
                raise Fail('%s: unknown ship %s' % (where, kind))
            nav = int(f[2][3:]) if f[2].startswith('nav') else -1
            if nav not in nav_ids:
                raise Fail('%s: unknown nav point %s' % (where, f[2]))
            pilot = NONE
            if 'pilot' in opts:
                # (the barman and the chief can fly: Tally and Kit, the user's wingmen nine and ten; the arcs doc)
                if opts['pilot'] not in pilot_index or people[pilot_index[opts['pilot']]]['role'] not in ('pilot', 'barman', 'tech'):
                    raise Fail('%s: %s is not a pilot' % (where, opts['pilot']))
                pilot = pilot_index[opts['pilot']]
                if kind == 'wingman':
                    wingman = opts['pilot']
            role = 0
            if 'role' in opts:
                if opts['role'] not in ESCORT_ROLES or kind in ('player', 'wingman'):
                    raise Fail('%s: role=%s: an escort target is role=out or role=in, the home carrier role=home, a '
                               'fighter that stays at its nav point role=stay' % (where, opts['role']))
                role = ESCORT_ROLES[opts['role']]
                if role == ESCORT_ROLES['home'] and (nav != 0 or int(f[1]) != 0 or int(opts.get('count', 1)) != 1):
                    raise Fail('%s: role=home is one ship of side 0 at nav0' % where)
                if role == ESCORT_ROLES['stay'] and int(f[1]) != 0:
                    raise Fail('%s: role=stay is the player\'s side\'s (side 0)' % where)
            ai = 0
            if 'ai' in opts:
                if opts['ai'] not in PROFILES:
                    raise Fail('%s: ai=%s: the profiles are %s' % (where, opts['ai'], ', '.join(PROFILES)))
                ai = PROFILES.index(opts['ai']) + 1
            fear = 0
            if 'fear' in opts:
                try:
                    c, d = (int(v) for v in opts['fear'].split('/'))
                    if not (0 <= c <= 15 and 0 <= d <= 15):
                        raise ValueError
                except ValueError:
                    raise Fail('%s: fear=c/d, each 0 to 15 (the rate it grows afraid and the rate it calms)' % where)
                fear = c | d << 4
            if opts.get('turrets', 'on') not in ('on', 'off'):
                raise Fail('%s: turrets=on or turrets=off' % where)
            jump = 0
            if 'jump' in opts:          # (through the waypoints with the player, out only short of nav N: logic.c route_step)
                try:
                    jump = int(opts['jump'])
                except ValueError:
                    jump = -1
                if role != ESCORT_ROLES['out'] or jump <= nav or jump not in nav_ids or jump in hidden_navs:
                    raise Fail('%s: jump=N goes with role=out: N a waypoint after its own nav point' % where)
            at = opts.get('at', '0,0,0').split(',')
            if len(at) != 3 or any(not -MAP_KM <= int(v) <= MAP_KM for v in at):
                raise Fail('%s: at=x,y,z, km off its nav point, at most %d' % (where, MAP_KM))
            shipr.append(struct.pack(fmt(SHIP), tnum, int(f[1]), nav, int(opts.get('count', 1)), pilot, role, ai, fear,
                                     opts.get('turrets', 'on') == 'off', jump, int(at[0]), int(at[1]), int(at[2])))
        if len(shipr) > LIMITS['SHIPS']:
            raise Fail('%s: %d ships, at most %d' % (w, len(shipr), LIMITS['SHIPS']))
        if wingman is not None:         # (a wingman lost, the XO says this without them: game.c cuts " and NAME")
            you = 'You and %s' % people[pilot_index[wingman]]['callsign']
            if you not in ' '.join(f[-1] for _w, k_, f in m['talk'] if k_ == 'briefing'):
                raise Fail('%s: the briefing must say "%s": it is cut when the wingman is dead' % (w, you))
        # what the XO says is there is there: "3 Nicanders at Nav 1" against the mission's ship lines
        named = {r['name']: r['key'] for r in load_csv('ships.csv')}
        for _w, k_, f in m['talk']:
            for n_, say_, nav_ in re.findall(r'(\d+) ([A-Z][a-z]+) at Nav (\d)', f[-1]) if k_ == 'briefing' else ():
                key_ = named.get(say_) or named.get(say_[:-1]) if say_ not in named else named[say_]
                have = sum(int(next((o[6:] for o in g[3:] if o.startswith('count=')), 1)) for _x, g in m['ship']
                           if g[0] == key_ and g[1] == '1' and g[2] == 'nav' + nav_)
                if int(nav_) in hidden_navs:
                    raise Fail('%s: the briefing names Nav %s, a hidden trigger point the player has no list of' % (_w, nav_))
                if have != int(n_):
                    raise Fail('%s: the briefing says %s %s at Nav %s, the mission has %d' % (_w, n_, say_, nav_, have))
        homes = sum('role=home' in f[3:] for _w, f in m['ship'])
        if homes != 1:                  # (the player lands by calling it up on the comm: without it no flight can end well)
            raise Fail('%s: %d ships with role=home: a mission has its home carrier, one' % (w, homes))
        seats, placed = [], {}
        for room in ROOMS:
            where, seat = m['seats'].get(room, (w, {}))
            people_here = []
            for s in SEAT_NAMES:
                who = seat.get(s, '-')
                if who == '-':
                    seats.append(NONE)
                    continue
                if who not in pilot_index:
                    raise Fail('%s: unknown pilot %s' % (where, who))
                if who in FIXED_PLACES and not (who == 'TECH' and wingman == 'TECH'):    # (Kit, while she flies)
                    raise Fail('%s: %s is always at %s' % (where, who, FIXED_PLACES[who]))
                if who not in seat_art[room].get(s, ()):
                    raise Fail('%s: no art of %s in the %s %s seat (only %s)'
                               % (where, who, room, s, ', '.join(sorted(seat_art[room].get(s, ())))))
                if who in placed:
                    raise Fail('%s: %s is already in the %s' % (where, who, placed[who]))
                placed[who] = room
                people_here.append(who)
                seats.append(pilot_index[who])
            for a in people_here:
                for b in people_here:
                    if a < b and not talk_ok.get(frozenset((a, b)), True):
                        raise Fail('%s: %s and %s will not sit at one table (relations.csv)' % (where, a, b))
        talks = []
        for where, kind, f in m['talk']:
            # talk ROOM A B [conditions] "line"      table talk, A to B, both seated: subtitles in the room
            # say ROOM OWNER WHO [conditions] "line" what clicking OWNER plays, in order, WHO speaking: OWNER, `you`,
            #                                        or another seated there; OWNER seated, or Tally in the bar
            # radio [conditions] "line"              the wingman, on the comm at the launch
            # talk deck TECH you [conditions] "line" Kit, in subtitles as the player walks onto the deck, once
            # say deck TECH TECH|you [conditions] "line"   clicking Kit on the deck, the first time in a mission
            line, opts = f[-1], f[:-1]
            flags, arg, boost, args = 0, 0, 0, 0
            conds = [o for o in opts if '=' in o]       # (anywhere among the words before the line)
            opts = [o for o in opts if '=' not in o]
            for cond in conds:
                k, v = cond.split('=', 1)
                if k == 'boost' and v in TALK_BOOSTS:
                    boost = TALK_BOOSTS[v]
                elif k == 'after' and v in ('won', 'lost') and kind not in ('win', 'lose'):
                    flags |= TALK_FLAGS['WON' if v == 'won' else 'LOST']
                elif k in ('if', 'unless') and v in node_index:
                    flags |= TALK_FLAGS[k.upper()]
                    arg = node_index[v]
                    args += 1
                elif k in ('dead', 'alive') and v in pilot_index:
                    flags |= TALK_FLAGS[k.upper()]
                    arg = pilot_index[v]
                    args += 1
                elif k == 'escort' and v == 'lost':
                    flags |= TALK_FLAGS['ESCORT']
                elif k == 'system' and v in ('won', 'lost') and kind in ('win', 'lose'):
                    flags |= TALK_FLAGS['WON' if v == 'won' else 'LOST']   # (game: how the system comes out of this flight)
                else:
                    raise Fail('%s: %s=%s: the conditions are after=won|lost, if=NODE, unless=NODE, dead=PILOT, alive=PILOT, escort=lost, in a debrief system=won|lost, and on '
                               'Kit\'s lines boost=armor|fuel|missile' % (where, k, v))
            if args > 1:
                raise Fail('%s: only one of if=, unless=, dead= and alive= on a line' % where)
            if boost and (kind != 'say' or opts[:2] != ['deck', 'TECH'] or opts[2:] != ['TECH']):
                raise Fail('%s: a boost is on a line Kit says on the deck: say deck TECH TECH boost=KIND "line"' % where)
            if kind in BRIEF_ROOMS:     # [WHO] [conditions] "line": the XO's, or anyone's in their close-up
                if len(opts) > 1 or (opts and opts[0] != 'you' and opts[0] not in pilot_index):
                    raise Fail('%s: %s [WHO] [conditions] "line": WHO is `you` or someone in people.csv' % (where, kind))
                who = opts[0] if opts else 'XO'
                talks.append(struct.pack(fmt(TALK), BRIEF_ROOMS[kind], SPEAKER_YOU if who == 'you' else pilot_index[who],
                                         NONE, flags, arg, 0, text(line, TEXT['TALK'], where)))
                continue
            if kind == 'radio':
                if opts or wingman is None:
                    raise Fail('%s: radio [conditions] "line", in a mission with a wingman' % where)
                talks.append(struct.pack(fmt(TALK), len(ROOMS), pilot_index[wingman], NONE, flags, arg, 0,
                                         text(line, TEXT['TALK'], where)))
                continue
            if len(opts) != 3 or opts[0] not in ROOMS + ['deck']:
                raise Fail('%s: %s ROOM A B [conditions] "line"' % (where, kind))
            room, a, b = opts
            if room == 'deck':
                if a != 'TECH' or b not in ('TECH', 'you') or (kind == 'talk' and b != 'you'):
                    raise Fail('%s: the deck is Kit\'s: talk deck TECH you, or say deck TECH TECH|you' % where)
                if kind == 'talk':
                    speaker, listener = pilot_index['TECH'], NONE
                else:
                    speaker, listener = SPEAKER_YOU if b == 'you' else pilot_index['TECH'], pilot_index['TECH']
                    flags |= TALK_FLAGS['SAY']
                talks.append(struct.pack(fmt(TALK), len(ROOMS) + 1, speaker, listener, flags, arg, boost,
                                         text(line, TEXT['TALK'], where)))
                continue
            if kind == 'talk':
                if placed.get(a) != room or placed.get(b) != room:
                    raise Fail('%s: %s and %s are not both in the %s' % (where, a, b, room))
                if not talk_ok.get(frozenset((a, b)), True):
                    raise Fail('%s: %s and %s do not talk (relations.csv)' % (where, a, b))
                speaker, listener = pilot_index[a], pilot_index[b]
            else:
                owner, who = a, b
                if placed.get(owner) != room and not (room == 'bar' and owner == 'TALLY'):
                    raise Fail('%s: nobody clicks %s in the %s: not seated there' % (where, owner, room))
                if who != 'you' and who != owner and placed.get(who) != room and not (room == 'bar' and who in ('TALLY', 'XO')):
                    raise Fail('%s: %s is not in the %s to speak' % (where, who, room))
                if who != 'you' and not talk_ok.get(frozenset((owner, who)), True):
                    raise Fail('%s: %s and %s do not talk (relations.csv)' % (where, owner, who))
                speaker, listener = SPEAKER_YOU if who == 'you' else pilot_index[who], pilot_index[owner]
                flags |= TALK_FLAGS['SAY']
            talks.append(struct.pack(fmt(TALK), ROOMS.index(room), speaker, listener, flags, arg, 0,
                                     text(line, TEXT['TALK'], where)))
        if len(talks) > LIMITS['TALKS']:
            raise Fail('%s: %d talk lines, at most %d' % (w, len(talks), LIMITS['TALKS']))
        fields = []
        for where, f in m['field']:
            density = level = 0
            opts = [w for w in f[5:]]
            f = f[:5]
            for o in opts:
                if o.startswith('density='):
                    density = int(o[8:])
                elif o.startswith('level='):
                    level = int(o[6:])
                else:
                    raise Fail('%s: a field is asteroid|mine x y z radius [density=0..255] [level=0..3]' % where)
            if f[0] not in FIELDS or len(f) != 5 or not 0 <= density <= 255 or not 0 <= level <= 3:
                raise Fail('%s: a field is asteroid|mine x y z radius [density=0..255] [level=0..3]' % where)
            if any(not -MAP_KM <= int(v) <= MAP_KM for v in f[1:4]) or not 0 < int(f[4]) <= 32767:
                raise Fail('%s: a field is at most %d km out on each axis, its radius 1..32,767 km' % (where, MAP_KM))
            fields.append(struct.pack(fmt(FIELD), FIELDS[f[0]], density, int(f[1]), int(f[2]), int(f[3]), int(f[4]), level))
        if len(fields) > LIMITS['FIELDS']:
            raise Fail('%s: %d fields, at most %d' % (w, len(fields), LIMITS['FIELDS']))
        # the fighter the mission puts the player (and the wingman) in; none (0xFF): the campaign's choice stays
        cockpits = set(json.load(open(os.path.join(DATA, 'cockpits.json')))['ships'])
        fighter = 0xFF
        if 'fighter' in m:
            if m['fighter'] not in ships or m['fighter'] not in cockpits:
                raise Fail('%s: fighter %s is not a ship the player can fly (cockpits.json)' % (w, m['fighter']))
            fighter = ships[m['fighter']]
        rec = struct.pack('<%ds8B' % TEXT['ID'], text(m['id'], TEXT['ID'], w), node_index[m['node']],
                          TYPES[m['type']], MUSIC[m['music']], len(navs), len(shipr), len(talks), len(fields), fighter)
        rec += text(m['title'], TEXT['TITLE'], w).ljust(TEXT['TITLE'], b'\0')
        rec += b''.join(navs).ljust(LIMITS['NAVS'] * struct.calcsize(fmt(NAV)), b'\0')
        rec += b''.join(shipr).ljust(LIMITS['SHIPS'] * struct.calcsize(fmt(SHIP)), b'\0')
        rules = m['rules']
        # wipe ALL|PILOT: Tally's board, as the player comes to this mission
        wipe = m.get('wipe', '')
        if wipe and wipe != 'ALL' and wipe not in pilot_index:
            raise Fail('%s: wipe ALL or a pilot: %s' % (w, wipe))
        rec += struct.pack('<6BBBB', *seats, 0xFE if wipe == 'ALL' else pilot_index[wipe] if wipe else NONE,
                           RULES['hit'][rules.get('hit', 'ships')],
                           sum(RULES[k][rules[k]] for k in rules if k != 'hit'))
        rec += struct.pack('<H', len(out_lines))        # (its lines are their own table: XMS, read one at a time)
        out_lines += talks
        rec += b''.join(fields).ljust(LIMITS['FIELDS'] * struct.calcsize(fmt(FIELD)), b'\0')
        out_missions.append(rec)

    out_pilots = [struct.pack(fmt(PILOT), text(p['key'], TEXT['KEY'], 'people.csv'),
                              text(p['callsign'] or p['key'].title(), TEXT['CALLSIGN'], 'people.csv'),
                              text(p['name'], TEXT['NAME'], 'people.csv'), ROLES[p['role']], seed_kills(p),
                              twin_of.get(p['key'], 255),
                              text(p['line'], TEXT['LINE'], 'people.csv %s line' % p['key'])) for p in people]
    # what Kit says when a fighter comes home, by how badly (data/homecoming.csv): after every mission's lines, in the tiers' order
    home_line = len(out_lines)
    home_n = []
    rows = load_csv('homecoming.csv')
    for tier in HOME_TIERS:
        said = [r['line'] for r in rows if r['tier'] == tier]
        if len(said) < 4:                               # (the user: at least 4 each, so they don't get old)
            raise Fail('homecoming.csv: %d lines for %s, at least 4' % (len(said), tier))
        home_n.append(len(said))
        out_lines += [struct.pack(fmt(TALK), 0, pilot_index['TECH'], pilot_index['TECH'], 0, 0, 0,
                                  text(line, TEXT['TALK'], 'homecoming.csv')) for line in said]
    if len(rows) != sum(home_n):
        raise Fail('homecoming.csv: a tier that is not one of %s' % ', '.join(HOME_TIERS))
    return out_pilots, out_nodes, out_missions, out_types, out_weapons, node_index[nodes[0]['node']], out_lines, home_line, bytes(home_n)


def check_dag(nodes):
    edges = {n['node']: [n[k] for k in ('good', 'bad') if n[k] not in ('GOOD', 'BAD')] for n in nodes}
    state = {}

    def visit(v):
        if state.get(v) == 1:
            raise Fail('campaign.csv: a cycle through %s' % v)
        if state.get(v) == 2:
            return
        state[v] = 1
        for w in edges[v]:
            visit(w)
        state[v] = 2
    visit(nodes[0]['node'])
    left = [n['node'] for n in nodes if n['node'] not in state]
    if left:
        raise Fail('campaign.csv: unreachable %s' % left)


# the header's types: rvtypes.h's sized ones (the same size on DOS and on the host harness's 32- and 64-bit builds)
SIZED = {'unsigned char': 'uint8_t', 'signed char': 'int8_t', 'unsigned short': 'uint16_t', 'short': 'int16_t',
         'unsigned long': 'uint32_t', 'long': 'int32_t', 'char': 'char'}


def c_struct(name, fields):
    lines = ['typedef struct {']
    for ctype, field, _f in fields:
        lines.append('    %s %s;' % (SIZED.get(ctype, ctype), field))
    lines.append('} %s;' % name)
    return '\n'.join(lines)


def header_text(sizes):
    out = ['/* rvdata.h: RVGAME.DAT\'s records.  Generated by tools/rvdata.py: do not edit. */',
           '#ifndef RVDATA_H', '#define RVDATA_H', '', '#include "rvtypes.h"', '',
           '#define RVGAME_MAGIC "RVGAME"', '#define RVGAME_VERSION %d' % VERSION, '']
    for k, v in LIMITS.items():
        out.append('#define RV_%s %d' % (k, v))
    out.append('#define RV_WEAPON_TYPES %d' % weapon_types())
    out.append('#define RV_ROOMS %d' % len(ROOMS))
    out.append('#define RV_ENGINES %d' % ENGINES)
    out.append('#define RV_VITALS %d' % VITALS)
    out.append('#define RV_GUNTABLE %d' % GUNTABLE)
    out.append('#define RV_GUNS %d' % GUNS)
    for k, v in TEXT.items():
        out.append('#define RV_%s %d' % (k, v))
    out += ['', '/* node edges */', '#define RV_NODE_NONE 0x%02X' % NONE, '#define RV_NODE_GOOD 0x%02X' % GOOD,
            '#define RV_NODE_BAD 0x%02X' % BAD,
            '', '/* mission ship types picked at run time */',
            '#define RV_SHIP_PLAYER 0x%02X' % SHIP_PLAYER, '#define RV_SHIP_WINGMAN 0x%02X' % SHIP_WINGMAN,
            '#define RV_ESCORT_OUT %d' % ESCORT_ROLES['out'], '#define RV_ESCORT_IN %d' % ESCORT_ROLES['in'],
            '#define RV_ESCORT_HOME %d' % ESCORT_ROLES['home'], '#define RV_ESCORT_STAY %d' % ESCORT_ROLES['stay'],
            '#define RV_PILOT_NONE 0xFF', '#define RV_SPEAKER_YOU 0x%02X' % SPEAKER_YOU,
            '#define RV_ROOM_RADIO %d' % len(ROOMS), '#define RV_ROOM_DECK %d' % (len(ROOMS) + 1)]
    for k, v in BRIEF_ROOMS.items():
        out.append('#define RV_ROOM_%s %d' % ({'briefing': 'BRIEF'}.get(k, k.upper()), v))
    for k, v in TALK_BOOSTS.items():
        out.append('#define RV_BOOST_%s %d' % (k.upper(), v))
    out.append('')
    for k, v in TALK_FLAGS.items():
        out.append('#define RV_TALK_%s %d' % (k, v))
    for i, r in enumerate(ROOMS):
        out.append('#define RV_ROOM_%s %d' % (r.upper(), i))
    for k, v in FIELDS.items():
        out.append('#define RV_FIELD_%s %d' % (k.upper(), v))
    for k, v in MUSIC.items():
        out.append('#define RV_MUSIC_%s %d' % (k.upper(), v))
    out.append('#define RV_MUSIC_COUNT %d' % len(MUSIC_FILES))
    out.append('#define RV_MUSIC_STINGS %d     /* from here on the cues are played once */' % MUSIC_FILES.index('VICTORY1'))
    out.append('#define RV_MUSIC_FILES %s' % ', '.join('"%s.MID"' % f for f in MUSIC_FILES))
    for k, v in ROLES.items():
        out.append('#define RV_ROLE_%s %d' % (k.upper(), v))
    out.append('#define RV_SHIPTYPE_FLAG_PLAYER %d' % SHIPTYPE_FLAG_PLAYER)
    out.append('#define RV_SHIPTYPE_FLAG_HIT_SILHOUETTE %d' % SHIPTYPE_FLAG_HIT_SILHOUETTE)
    out.append('#define RV_SHIPTYPE_FLAG_HIT_SPHERE %d' % SHIPTYPE_FLAG_HIT_SPHERE)
    out.append('#define RV_SHIPTYPE_FLAG_HIT_BOX %d' % SHIPTYPE_FLAG_HIT_BOX)
    out.append('#define RV_SHIPTYPE_HIT_MASK %d' % (SHIPTYPE_FLAG_HIT_SILHOUETTE | SHIPTYPE_FLAG_HIT_SPHERE
                                                    | SHIPTYPE_FLAG_HIT_BOX))
    out.append('#define RV_RULE_AIM_CONE %d' % RULE_AIM_CONE)
    out.append('#define RV_RULE_HOLD_FIRE %d' % RULE_HOLD_FIRE)
    out.append('#define RV_RULE_RAM_SOFT %d' % RULE_RAM_SOFT)
    out.append('#define RV_PROFILES %d                 /* missions.txt ai=: %s (logic.c P_*, in this order) */'
               % (len(PROFILES), ', '.join(PROFILES)))
    out.append('#define RV_GUN_ENERGY_STEPS %d' % GUN_ENERGY_STEPS)
    out.append('#define RV_SHIELD_FULL_SHIFT %d' % SHIELD_FULL_SHIFT)
    for i, k in enumerate(SOUNDS):
        out.append('#define RV_SFX_%s %d' % (k.upper(), i))
    out.append('#define RV_SFX_COUNT %d' % len(SOUNDS))
    out.append('#define RV_SFX_NONE 0x%02X' % SFX_NONE)
    for names, prefix in ((WEAPON_KINDS, 'RV_WK_'), (WEAPON_LOOKS, 'RV_LOOK_'), (WEAPON_LOCKS, 'RV_LOCK_')):
        for i, k in enumerate(names):
            out.append('#define %s%s %d' % (prefix, k.upper(), i))
    out += ['', '#pragma pack(1)', c_struct('RvGameHeader', HEADER), c_struct('RvPilot', PILOT),
            c_struct('RvNode', NODE), c_struct('RvNav', NAV), c_struct('RvShip', SHIP), c_struct('RvTalk', TALK),
            c_struct('RvField', FIELD),
            c_struct('RvMission', MISSION), c_struct('RvShipType', SHIPTYPE), c_struct('RvWeapon', WEAPON),
            '#pragma pack()', '']
    for k, v in sizes.items():
        out.append('#define RV_%s_SIZE %d' % (k, v))
    # the quarter sine wave for the simulation's integer trigonometry (flight.c):
    # sin(i / 256 * 90 degrees) * 16384, i = 0..256
    out += ['', '/* sin(i/256 x 90 degrees) x 16384, i = 0..256: the simulation\'s trigonometry (flight.c) */',
            '#ifdef RV_TABLES', '#ifdef __WATCOMC__', '#define RV_FAR __far', '#else', '#define RV_FAR', '#endif',
            'static const int16_t RV_FAR rv_qsin[257] = {']
    vals = [int(round(math.sin(i / 256 * math.pi / 2) * 16384)) for i in range(257)]
    for i in range(0, 257, 12):
        out.append('    ' + ', '.join('%d' % v for v in vals[i:i + 12]) + ',')
    out += ['};', '#endif']
    out += ['', '#endif', '']
    return '\n'.join(out)


def manifest(nodes, missions):
    """RVNET.JSON: the campaign as the page's mission menu lists it (web/gamefiles.js, the build copies it beside the
    page as rv-campaign.json), read back from the packed records RVGAME.DAT holds, so the menu and the game agree
    however the campaign changes: a node a series, in RVGAME.DAT's order, its missions in its order, each with the
    number RV.EXE's /brief N takes (its record's), its id and its title"""
    node = struct.Struct(fmt(NODE))
    head = struct.Struct('<%ds8B%ds' % (TEXT['ID'], TEXT['TITLE']))   # (a mission record's start: rec in compile_all)
    series = []
    for i, rec in enumerate(nodes):
        v = node.unpack(rec)
        ms = []
        for n in v[3:3 + LIMITS['MISSIONS_PER_NODE']]:
            if n == 0xFFFF:
                continue
            h = head.unpack_from(missions[n])
            ms.append({'n': n, 'id': h[0].rstrip(b'\0').decode('ascii'), 'title': h[-1].rstrip(b'\0').decode('ascii')})
        series.append({'node': i, 'id': v[0].rstrip(b'\0').decode('ascii'), 'missions': ms})
    return {'version': 1, 'series': series}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'dos', 'RVGAME.DAT'))
    ap.add_argument('--header', default=os.path.join(ROOT, 'src', 'rvdata.h'))
    ap.add_argument('--manifest', default=os.path.join(ROOT, 'build', 'dos', 'RVNET.JSON'))
    args = ap.parse_args()
    try:
        pilots, nodes, missions, types, weapons, start, lines, home_line, home_n = compile_all()
    except Fail as e:
        sys.exit('rvdata: %s' % e)
    sizes = {'HEADER': struct.calcsize(fmt(HEADER)), 'PILOT': len(pilots[0]), 'NODE': len(nodes[0]),
             'MISSION': len(missions[0]), 'SHIPTYPE': len(types[0]), 'WEAPON': len(weapons[0]),
             'LINE': struct.calcsize(fmt(TALK))}
    hsize = sizes['HEADER']
    pilots_at = hsize
    nodes_at = pilots_at + len(pilots) * sizes['PILOT']
    missions_at = nodes_at + len(nodes) * sizes['NODE']
    types_at = missions_at + len(missions) * sizes['MISSION']
    weapons_at = types_at + len(types) * sizes['SHIPTYPE']
    lines_at = weapons_at + len(weapons) * sizes['WEAPON']
    head = struct.pack(fmt(HEADER), b'RVGAME', VERSION, hsize, len(pilots), sizes['PILOT'], pilots_at,
                       len(nodes), sizes['NODE'], nodes_at, len(missions), sizes['MISSION'], missions_at, start,
                       len(types), sizes['SHIPTYPE'], types_at, len(weapons), sizes['WEAPON'], weapons_at,
                       len(lines), sizes['LINE'], lines_at, home_line, home_n)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(head + b''.join(pilots) + b''.join(nodes) + b''.join(missions) + b''.join(types) + b''.join(weapons)
                + b''.join(lines))
    with open(args.header, 'w') as f:
        f.write(header_text(sizes))
    with open(args.manifest, 'w') as f:
        json.dump(manifest(nodes, missions), f, separators=(',', ':'))
        f.write('\n')
    print('%s: %d pilots, %d nodes, %d missions (%d bytes each), %d ship types, %d weapons, %d bytes'
          % (args.out, len(pilots), len(nodes), len(missions), sizes['MISSION'], len(types), len(weapons),
             os.path.getsize(args.out)))


if __name__ == '__main__':
    main()
