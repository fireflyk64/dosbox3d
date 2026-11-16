#!/usr/bin/env python3
"""Wolfenstein 3-D: the hooks' signatures, made from the shareware WOLF3D.EXE 1.4 (docs/wolf3d.md).

    scripts/wolfsig.py SHAREWARE/WOLF3D.EXE [FULL/WOLF3D.EXE ...] > src/cpu/wcnet_wolf_sig.h

As scripts/rapsig.py does for Raptor: each ANCHOR is a run of the program's instructions about one place, kept as
bytes with the ones that differ from build to build masked out (a segment the loader relocates, a far or near call's
target, an address in the data segment); each must be in the program's code exactly once.  Each ITEM is a 16-bit
operand in an anchor, which in the running program is what the hooks want: an offset in the data segment, a code
segment as loaded, a function's offset.  Any other WOLF3D.EXE given too (the six-episode game: GOG's is the Activision
1.4 build) is tried against every anchor and its items printed beside the shareware's.

The programs are packed with LZEXE (scripts/unlzexe.py); in memory they are plain.  The names are the game's own (its
source as id Software published it); the addresses in this file are the shareware image's (segment:offset from the
load segment).
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unlzexe import unlzexe

# name, first instruction, the address after the last, what it is.  A '!' place is a hook's (its IP watched).
ANCHORS = [
    ('newgame', '044E:036B', '044E:03A1', "NewGame (difficulty, episode): gamestate cleared, 100 health, 8 bullets, 3 lives"),
    ('viewsize', '044E:13AC', '044E:13DC', "SetViewSize (width, height): viewwidth (a multiple of 16), viewheight, centerx, shootdelta"),
    ('tedlevel', '044E:15DB', '044E:15F6', "DemoLoop: the TEDLEVEL start (episode and map from tedlevelnum), GameLoop () called"),
    ('setuplevel', '0B56:068B', '0B56:06AE', "SetupGameLevel (): the level's counts cleared unless a game was loaded"),
    ('setuprnd', '0B56:06B6', '0B56:06E9', "SetupGameLevel: the random table's start (a demo's: the same every time), the map read"),
    ('died', '0B56:1125', '0B56:115D', "Died (): a life less (never under TEDLEVEL), health, weapons and keys as at a start"),
    ('gameloop', '0B56:11CD', '0B56:1205', "GameLoop: PlayLoop () called for the level"),
    ('leveldone', '0B56:1238', '0B56:124A', "GameLoop: PlayLoop left, by playstate; a level completed: the keys gone, DrawKeys ()"),
    ('initactors', '0C8E:09DE', '0C8E:0A04', "InitActorList (): the free list, the last object, the player the first one made"),
    ('frame', '0C8E:1039', '0C8E:106D', "PlayLoop's frame: madenoise = false, MoveDoors (), MovePWalls (), DoActor (obj) for every object"),
    ('frame_end', '0C8E:1112', '0C8E:112E', "PlayLoop's frame: while (!playstate && !startgame), and the loop left"),
    ('damage', '1019:0EF0', '1019:0F38', "DamageActor (ob, damage): madenoise, twice the damage when not attacking, KillActor or FirstSighting"),
    ('knife', '1193:0F0F', '1193:0F2A', "KnifeAttack: DamageActor (closest, US_RndT () / 6)"),
    ('statics', '12B2:000B', '12B2:004A', "InitStaticList (), SpawnStatic (tilex, tiley, type): laststatobj, statinfo, spotvis"),
    ('connect', '12B2:01B3', '12B2:01E9', "ConnectAreas (): areabyplayer cleared, the player's area's connections followed"),
    ('opendoor', '12B2:033C', '12B2:0365', "OpenDoor (door): an open door's time again, any other started opening"),
    ('spawndoor', '12B2:0263', '12B2:02B5', "SpawnDoor (tilex, tiley, vertical, lock): the door's record, its number | 0x80 in actorat and tilemap"),
    ('dooropen', '12B2:062D', '12B2:065B', "DoorOpen (door): its time open counted in tics, closed after 300"),
    ('movedoors', '12B2:08E8', '12B2:0911', "MoveDoors (): every door by its action, unless gamestate.victoryflag"),
    ('pushwall', '12B2:093B', '12B2:096B', "PushWall (checkx, checky, dir): none while one moves (pwallstate), the tile's wall"),
    ('pushwall_go', '12B2:0A7B', '12B2:0ACD', "PushWall: gamestate.secretcount++, pwallx, pwally, pwalldir, pwallstate = 1, pwallpos = 0, the info plane's mark gone"),
]

# name, anchor, the instruction, which operand ('disp' its memory address, 'imm' its 16-bit immediate, 'rel' a near
# call's target, 'faroff' and 'farseg' a far call's), what it is
ITEMS = [
    ('gamestate', 'newgame', '044E:036F', 'imm', "gamestate (66 bytes: difficulty, mapon, the scores, lives, health, ammo, keys, weapons, ... episode, the counts)"),
    ('viewwidth', 'viewsize', '044E:13B8', 'disp', "viewwidth: the 3-D view's width in pixels (centred above the status bar)"),
    ('viewheight', 'viewsize', '044E:13C1', 'disp', "viewheight"),
    ('tedlevelnum', 'tedlevel', '044E:15DB', 'disp', "tedlevelnum (the TEDLEVEL argument: 10 * episode + map)"),
    ('gameloop_off', 'tedlevel', '044E:15F1', 'faroff', "GameLoop ()"),
    ('seg_game', 'tedlevel', '044E:15F1', 'farseg', "WL_GAME's code segment"),
    ('loadedgame', 'setuplevel', '0B56:0693', 'disp', "loadedgame"),
    ('demoplayback', 'setuprnd', '0B56:06B6', 'disp', "demoplayback"),
    ('demorecord', 'setuprnd', '0B56:06BD', 'disp', "demorecord"),
    ('tedlevel', 'died', '0B56:1125', 'disp', "tedlevel: started by TEDLEVEL (no life is ever lost)"),
    ('playloop_off', 'gameloop', '0B56:11F5', 'faroff', "PlayLoop ()"),
    ('seg_play', 'gameloop', '0B56:11F5', 'farseg', "WL_PLAY's code segment"),
    ('drawkeys_off', 'leveldone', '0B56:1245', 'faroff', "DrawKeys ()"),
    ('seg_agent', 'leveldone', '0B56:1245', 'farseg', "WL_AGENT's code segment"),
    ('objfreelist', 'initactors', '0C8E:09E4', 'disp', "objfreelist"),
    ('objlist', 'initactors', '0C8E:09E4', 'imm', "objlist [MAXACTORS] (60 bytes each)"),
    ('lastobj', 'initactors', '0C8E:09EA', 'disp', "lastobj"),
    ('objcount', 'initactors', '0C8E:09F0', 'disp', "objcount"),
    ('getnewactor', 'initactors', '0C8E:09F8', 'rel', "GetNewActor (): the next free object, linked last, in `new`"),
    ('newobj', 'initactors', '0C8E:09FB', 'disp', "new"),
    ('player', 'initactors', '0C8E:09FE', 'disp', "player"),
    ('actorat', 'spawndoor', '12B2:029F', 'disp', "actorat [64][64] (words: an object, or a wall's or door's number)"),
    ('madenoise', 'frame', '0C8E:1039', 'disp', "madenoise"),
    ('movedoors_off', 'frame', '0C8E:103F', 'faroff', "MoveDoors ()"),
    ('seg_act1', 'frame', '0C8E:103F', 'farseg', "WL_ACT1's code segment"),
    ('doactor', 'frame', '0C8E:1056', 'rel', "DoActor (ob)"),
    ('tics', 'dooropen', '12B2:0643', 'disp', "tics: the frame's length, 70ths of a second"),
    ('playstate', 'frame_end', '0C8E:1112', 'disp', "playstate (0 playing, 1 completed, 2 died, 3 warped, 6 victorious, 9 the secret level)"),
    ('startgame', 'frame_end', '0C8E:1119', 'disp', "startgame: a new game asked for (the menu's)"),
    ('damage_off', 'knife', '1193:0F1E', 'faroff', "DamageActor (ob, damage)"),
    ('seg_state', 'knife', '1193:0F1E', 'farseg', "WL_STATE's code segment"),
    ('killactor', 'damage', '1019:0F1C', 'rel', "KillActor (ob)"),
    ('firstsighting', 'damage', '1019:0F32', 'rel', "FirstSighting (ob)"),
    ('laststatobj', 'statics', '12B2:000E', 'disp', "laststatobj"),
    ('statobjlist', 'statics', '12B2:000E', 'imm', "statobjlist [MAXSTATS] (8 bytes each)"),
    ('statinfo', 'statics', '12B2:0028', 'disp', "statinfo [] (a static's picture and kind, 4 bytes each)"),
    ('spotvis', 'statics', '12B2:003F', 'imm', "spotvis [64][64]"),
    ('areabyplayer', 'connect', '12B2:01B7', 'imm', "areabyplayer [NUMAREAS] (words)"),
    ('dooraction', 'opendoor', '12B2:034C', 'disp', "doorobjlist [0].action (a door 10 bytes: tilex, tiley, vertical, lock, action, ticcount)"),
    ('doorposition', 'spawndoor', '12B2:0269', 'disp', "doorposition [MAXDOORS] (words: 0 closed .. 0xFFFF open)"),
    ('victoryflag', 'movedoors', '12B2:08ED', 'disp', "gamestate.victoryflag"),
    ('doornum', 'movedoors', '12B2:08F9', 'disp', "doornum: the level's doors"),
    ('pwallstate', 'pushwall', '12B2:0949', 'disp', "pwallstate (0: no wall moving)"),
    ('tilemap', 'pushwall', '12B2:095B', 'disp', "tilemap [64][64] (bytes)"),
    ('pwallx', 'pushwall_go', '12B2:0A7F', 'disp', "pwallx"),
    ('pwally', 'pushwall_go', '12B2:0A83', 'disp', "pwally"),
    ('pwalldir', 'pushwall_go', '12B2:0A87', 'disp', "pwalldir (0 north, 1 east, 2 south, 3 west)"),
    ('pwallpos', 'pushwall_go', '12B2:0A91', 'disp', "pwallpos"),
    ('farmapylookup', 'pushwall_go', '12B2:0AA9', 'disp', "farmapylookup [64] (a map row's offset)"),
    ('mapsegs1', 'pushwall_go', '12B2:0AAF', 'disp', "mapsegs [1]: the info plane's segment (mapsegs [0], the walls and areas, the word before)"),
]

# name, anchor, the instruction: a place the hooks watch, in which code segment
PLACES = [
    ('setuplevel', 'setuplevel', '0B56:068B', 'seg_game', "SetupGameLevel's entry"),
    ('frame', 'frame', '0C8E:1039', 'seg_play', "the frame, the controls read (tics made), before the doors and the actors"),
    ('loop_exit', 'frame_end', '0C8E:1123', 'seg_play', "PlayLoop left (playstate says why)"),
    ('damage', 'damage', '1019:0EF0', 'seg_state', "DamageActor's entry"),
    ('pushwall', 'pushwall', '12B2:093B', 'seg_act1', "PushWall's entry"),
]

PREFIXES = (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3)
IMM16 = set([0x68, 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0xA9, 0x81, 0xC7, 0x69] + list(range(0xB8, 0xC0)))
NOMODRM = set([0x68, 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0xA9, 0x6A, 0xE8, 0xE9, 0xEB, 0x9A, 0xEA, 0xC3, 0xCB, 0xC2, 0xCA,
               0x90, 0x99, 0x98, 0xC9, 0x0E, 0x1E, 0x06, 0x07, 0x1F, 0xCD, 0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0xA8,
               0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7] + list(range(0x40, 0x60)) + list(range(0x70, 0x80))
              + list(range(0xB0, 0xC0)) + list(range(0xA0, 0xA4)))


def operands(b):
    """The 16-bit operands of one instruction: {kind: position}."""
    p = 0
    while b[p] in PREFIXES:
        p += 1
    op = b[p]
    out = {}
    if op in (0x9A, 0xEA):
        out['faroff'] = p + 1
        out['farseg'] = p + 3
        return out
    if op in (0xE8, 0xE9):
        out['rel'] = p + 1
        return out
    if 0xA0 <= op <= 0xA3:
        out['disp'] = p + 1
        return out
    if op not in NOMODRM and len(b) > p + 1:
        m = p + 2 if op == 0x0F else p + 1
        mod, rm = b[m] >> 6, b[m] & 7
        if ((mod == 0 and rm == 6) or mod == 2) and len(b) >= m + 3:
            out['disp'] = m + 1
    if op in IMM16 and len(b) >= p + 3:
        if op == 0x81 or op == 0xC7 or op == 0x69:
            m = p + 1
            mod, rm = b[m] >> 6, b[m] & 7
            dl = 2 if (mod == 0 and rm == 6) or mod == 2 else 1 if mod == 1 else 0
            if len(b) == m + 1 + dl + 2:
                out['imm'] = len(b) - 2
        else:
            out['imm'] = len(b) - 2
    return out


class Program:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        img, rel, inf = unlzexe(d)
        self.img = bytes(img)
        self.rel = set(s * 16 + o for o, s in rel)
        ent = inf[1] * 16 + inf[0]
        assert self.img[ent] == 0xBA, 'the entry is not Borland C\'s'
        self.dgroup = struct.unpack_from('<H', self.img, ent + 1)[0]
        self.codeEnd = self.dgroup * 16

    def insns(self, lin, end):
        """The instructions from lin up to end: (address, bytes)."""
        with tempfile.NamedTemporaryFile() as f:
            f.write(self.img[lin:end + 16])
            f.flush()
            p = subprocess.run(['ndisasm', '-b16', '-o0', f.name], capture_output=True, text=True)
        out = []
        for line in p.stdout.split('\n'):
            parts = line.split()
            if len(parts) < 3:
                continue
            o = int(parts[0], 16)
            if o >= end - lin:
                break
            out.append((lin + o, bytes.fromhex(parts[1])))
        assert out and out[-1][0] + len(out[-1][1]) == end, 'the window does not end at an instruction'
        return out


def lin_of(text):
    s, o = text.split(':')
    return int(s, 16) * 16 + int(o, 16)


def make_anchor(prog, start, end):
    lin, last = lin_of(start), lin_of(end)
    data = bytearray(prog.img[lin:last])
    mask = bytearray([1] * len(data))
    where = {}
    for at, b in prog.insns(lin, last):
        ops = operands(b)
        where[at] = ops
        for kind, pos in ops.items():
            v = struct.unpack_from('<H', b, pos)[0]
            # a memory address past the small ones (a field of a record, a local) and an immediate that may be one
            keep = (kind == 'disp' and v < 0x100) or (kind == 'imm' and v < 0xC00)
            if not keep:
                mask[at - lin + pos] = mask[at - lin + pos + 1] = 0
    for r in prog.rel:
        if lin <= r < last - 1:
            mask[r - lin] = mask[r - lin + 1] = 0
    return data, mask, where


def find(prog, data, mask):
    # the longest run of kept bytes is looked for, the rest compared
    best, run, at = (0, 0), 0, 0
    for i, m in enumerate(mask):
        if m:
            if not run:
                at = i
            run += 1
            if run > best[1]:
                best = (at, run)
        else:
            run = 0
    key = bytes(data[best[0]:best[0] + best[1]])
    out = []
    i = prog.img.find(key, 0, prog.codeEnd)
    while i >= 0:
        s = i - best[0]
        if s >= 0 and all(not m or prog.img[s + k] == data[k] for k, m in enumerate(mask)):
            out.append(s)
        i = prog.img.find(key, i + 1, prog.codeEnd)
    return out, best


def main():
    progs = [Program(p) for p in sys.argv[1:]]
    sw = progs[0]
    anchors = {}
    bad = 0
    for name, start, end, what in ANCHORS:
        data, mask, where = make_anchor(sw, start, end)
        hits = [find(p, data, mask) for p in progs]
        for p, (h, key) in zip(progs, hits):
            if len(h) != 1:
                sys.stderr.write('%s: %d matches in %s\n' % (name, len(h), p.path))
                bad += 1
        anchors[name] = (lin_of(start), data, mask, where, hits)
    if bad:
        sys.exit(1)
    out = []
    out.append('// Made by scripts/wolfsig.py from the shareware WOLF3D.EXE (Wolfenstein 3-D 1.4, Apogee): do not edit.')
    out.append("// Each anchor: a run of the program's code (mask 0: a byte that differs from build to build -- a segment the")
    out.append("// loader relocates, a call's target, a data address), found once in the running program; each item a 16-bit")
    out.append('// operand in one; each place an instruction the hooks watch.  The comments\' addresses are the shareware image\'s.')
    out.append('#ifndef WCNET_WOLF_SIG_H')
    out.append('#define WCNET_WOLF_SIG_H')
    out.append('namespace wc { namespace wolfsig {')
    out.append('struct Anchor { const char *name; int len; const Bit8u *bytes; const Bit8u *mask; int key; int keyLen; };')
    out.append('enum Kind { K_DATA, K_IMM, K_REL, K_FAROFF, K_FARSEG };')
    out.append('struct Item { const char *name; int anchor; int at; Kind kind; };')
    out.append('struct Place { const char *name; int anchor; int at; int seg; };     // seg: the item that is its code segment (-1: none)')
    out.append('enum AnchorId {')
    for i, (name, start, end, what) in enumerate(ANCHORS):
        out.append('    A_%s = %d,     // %s %s' % (name.upper(), i, start, what))
    out.append('    A_COUNT = %d' % len(ANCHORS))
    out.append('};')
    for name, start, end, what in ANCHORS:
        lin, data, mask, where, hits = anchors[name]
        out.append('static const Bit8u k_%s_bytes[%d] = { %s };' % (name, len(data), ', '.join('0x%02x' % b for b in data)))
        out.append('static const Bit8u k_%s_mask[%d] = { %s };' % (name, len(mask), ', '.join(str(b) for b in mask)))
    out.append('static const Anchor kAnchors[A_COUNT] = {')
    for name, start, end, what in ANCHORS:
        lin, data, mask, where, hits = anchors[name]
        key = hits[0][1]
        out.append('    { "%s", %d, k_%s_bytes, k_%s_mask, %d, %d },' % (name, len(data), name, name, key[0], key[1]))
    out.append('};')
    kinds = {'disp': 'K_DATA', 'imm': 'K_IMM', 'rel': 'K_REL', 'faroff': 'K_FAROFF', 'farseg': 'K_FARSEG'}
    out.append('enum ItemId {')
    values = {}
    for i, (name, anchor, at, kind, what) in enumerate(ITEMS):
        lin, data, mask, where, hits = anchors[anchor]
        a = lin_of(at)
        assert a in where and kind in where[a], '%s: no %s operand at %s' % (name, kind, at)
        pos = a - lin + where[a][kind]
        vals = []
        for p, (h, key) in zip(progs, hits):
            v = struct.unpack_from('<H', p.img, h[0] + pos)[0]
            if kind == 'rel':
                v = (h[0] + pos + 2 + v) & 0xFFFFF if v < 0x8000 else h[0] + pos + 2 + v - 0x10000
                vals.append('%05X' % v)
            else:
                vals.append('%04X' % v)
        values[name] = (pos, vals)
        out.append('    I_%s = %d,     // %s (%s)' % (name.upper(), i, what, ', '.join(vals) if len(vals) > 1 else vals[0]))
    out.append('    I_COUNT = %d' % len(ITEMS))
    out.append('};')
    out.append('static const Item kItems[I_COUNT] = {')
    for name, anchor, at, kind, what in ITEMS:
        out.append('    { "%s", A_%s, %d, %s },' % (name, anchor.upper(), values[name][0], kinds[kind]))
    out.append('};')
    out.append('enum PlaceId {')
    for i, (name, anchor, at, seg, what) in enumerate(PLACES):
        out.append('    P_%s = %d,     // %s %s' % (name.upper(), i, at, what))
    out.append('    P_COUNT = %d' % len(PLACES))
    out.append('};')
    out.append('static const Place kPlaces[P_COUNT] = {')
    for name, anchor, at, seg, what in PLACES:
        lin = anchors[anchor][0]
        out.append('    { "%s", A_%s, %d, %s },' % (name, anchor.upper(), lin_of(at) - lin, 'I_' + seg.upper() if seg else '-1'))
    out.append('};')
    out.append('} }')
    out.append('#endif')
    print('\n'.join(out))
    for name, anchor, at, kind, what in ITEMS:
        sys.stderr.write('%-14s %s\n' % (name, ' '.join(values[name][1])))
    for i, p in enumerate(progs):
        sys.stderr.write('%s: DGROUP %04X; anchors at %s\n' % (p.path, p.dgroup, ' '.join('%05X' % anchors[n[0]][4][i][0][0] for n in ANCHORS)))


if __name__ == '__main__':
    main()
