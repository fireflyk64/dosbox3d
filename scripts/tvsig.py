#!/usr/bin/env python3
"""Terminal Velocity: the hooks' signatures, made from the shareware GAME.EXE (docs/terminalvelocity.md).

    scripts/tvsig.py GAME.EXE > src/cpu/wcnet_tv_sig.h

The emulator finds the program in memory by its code, not by its file, and reads every data address it uses out
of the code itself: each ANCHOR below is a window of the code object's bytes around one place (the bytes the
loader relocates and the offsets of near calls masked out, since another build lays things out elsewhere), and
each DATA item is the 32-bit operand at a fixed distance into one anchor's window, which the loader has made the
linear address the game uses.  A build whose windows all match once (the registered game, if its code is the
same but for where things are) is flown as this one; any window not found leaves the game alone.

Each window must match exactly once in this GAME.EXE's code object: checked here, the header not written otherwise.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvexe

# name, image address of the place, window start (relative), window length, what it is
ANCHORS = [
    ('frame_top', 0x2D0D7, -21, 48, "the level loop's frame: the call of the input handler, at the top of each frame"),
    ('remote_draw', 0x2D0FF, -10, 40, "the level loop, after the world's objects are put in the draw list: where a net game draws the other players"),
    ('obj_update', 0x2427C, -33, 48, "the objects' loop: the call of an object's update (its AI and mover), the object pushed (ebx)"),
    ('obj_loop', 0x241E0, 0, 72, "the objects' loop: its start (the count, the table, the types)"),
    ('hit_apply', 0x29327, -59, 64, "a shot's hit on an object confirmed (ebx the object, [esp+0x1c] the damage), before it is applied"),
    ('hit_fn', 0x292B0, 0, 40, "the shot-against-objects test (the near objects' list)"),
    ('destroy_fn', 0x28E70, 0, 32, "an object destroyed: (object, its type)"),
    ('fire_fn', 0x19080, 0, 80, "a projectile made: (pos+angles+matrix, weapon, damage, owner, kind)"),
    ('draw_model_fn', 0x1E640, 0, 64, "a model put in the frame's draw list: (pos+angles, model, scale)"),
    ('send_state', 0x4CC30, 0, 0xB0, "the net game's ship report: the player's place, attitude, speeds, shield"),
    ('ship_model', 0x11820, 0, 0x4B, "the player's own ship's model loaded (ship.bin)"),
    ('load_model_fn', 0x402E0, 0, 48, "a model loaded from the game's files, by name: its handle (a name it cannot find ends the program)"),
    ('campaign', 0x2EC58, 0, 40, "the campaign loop: the level index, the count, the level's file name, the level flown"),
]

# name, anchor, offset of the operand from the anchor's place, what the address is
DATA = [
    ('netgame', 'remote_draw', 2, "nonzero in a net game (the level loop's test)"),
    ('obj_count', 'obj_loop', 0x241E7 - 0x241E0 + 2, "the level's object count"),
    ('obj_table', 'obj_loop', 0x24207 - 0x241E0 + 2, "the pointer to the objects (0xB4 bytes each)"),
    ('obj_types', 'obj_loop', 0x24220 - 0x241E0 + 2, "the pointer to the level's object types (0x244 bytes each)"),
    ('near_count', 'hit_fn', 0x292B7 - 0x292B0 + 2, "the count of the objects near enough to be updated this frame"),
    ('near_list', 'hit_fn', 0x292CE - 0x292B0 + 2, "their pointers"),
    ('projectiles', 'fire_fn', 0x190C5 - 0x19080 + 2, "the projectiles (0x5C bytes each, 256: the player's first 128)"),
    ('draw_count', 'draw_model_fn', 0x1E669 - 0x1E640 + 2, "the frame's draw list's length"),
    ('frame_time', 'send_state', 0x4CC43 - 0x4CC30 + 1, "the last frame's time (16.16 seconds)"),
    ('player_pos', 'send_state', 0x4CC62 - 0x4CC30 + 1, "the player's ship: x, y, z, then three angles"),
    ('player_vel', 'send_state', 0x4CC98 - 0x4CC30 + 1, "its velocity, x y z"),
    ('player_spin', 'send_state', 0x4CCB3 - 0x4CC30 + 1, "its angles' rates"),
    ('player_shield', 'send_state', 0x4CCCE - 0x4CC30 + 1, "its shield (0x10000 full)"),
    ('player_x4', 'send_state', 0x4CCD7 - 0x4CC30 + 1, "a fourth word the report carries (0x10000 at a net level's start)"),
    ('ship_model', 'ship_model', 0x11858 - 0x11820 + 1, "the player's ship's model (ship.bin)"),
    ('level_index', 'campaign', 0x2EC58 - 0x2EC58 + 2, "the campaign's level, 0.."),
    ('level_count', 'campaign', 0x2EC5E - 0x2EC58 + 2, "the campaign's level count"),
    ('level_names', 'campaign', 0x2EC6D - 0x2EC58 + 1, "the levels' file names, 0x28 bytes each"),
]


def window(le, img, place, start, length):
    a = place + start
    data = bytes(img[a:a + length])
    mask = bytearray(b'\xff' * length)
    for i in range(length):
        if a + i in le.fixed:
            mask[i] = 0
    # near calls and jumps out of the window: their offsets differ with the layout
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for ins in md.disasm(data, a):
        b = ins.bytes
        rel = None
        if b[0] in (0xE8, 0xE9) and ins.size == 5:
            rel = 1
        elif b[0] == 0x0F and len(b) == 6 and 0x80 <= b[1] <= 0x8F:
            rel = 2
        if rel is not None:
            target = int(ins.op_str, 16) if ins.op_str.startswith('0x') else None
            if target is None or not (a <= target < a + length) or b[0] == 0xE8:
                for k in range(ins.address + rel, ins.address + ins.size):
                    if a <= k < a + length:
                        mask[k - a] = 0
    return data, bytes(mask)


def matches(img, lo, hi, data, mask):
    # the longest run of kept bytes as the key, then the rest compared
    best, run, bstart, rstart = 0, 0, 0, 0
    for i, m in enumerate(mask):
        if m:
            if run == 0:
                rstart = i
            run += 1
            if run > best:
                best, bstart = run, rstart
        else:
            run = 0
    key = data[bstart:bstart + best]
    out = []
    j = img.find(key, lo, hi)
    while j >= 0:
        s = j - bstart
        if all(mask[k] == 0 or img[s + k] == data[k] for k in range(len(data))):
            out.append(s)
        j = img.find(key, j + 1, hi)
    return out, bstart, best


def main():
    le = tvexe.LE(sys.argv[1])
    img = le.load()
    o = le.objs[0]
    lo, hi = o['base'], o['base'] + o['vsize']
    rows, ok = [], True
    for name, place, start, length, what in ANCHORS:
        data, mask = window(le, img, place, start, length)
        found, kstart, klen = matches(img, lo, hi, data, mask)
        if found != [place + start]:
            sys.stderr.write('anchor %s: found at %s, not once at %x\n' % (name, [hex(f) for f in found], place + start))
            ok = False
        if klen < 4:
            sys.stderr.write('anchor %s: its longest kept run is %d bytes\n' % (name, klen))
            ok = False
        rows.append((name, place, start, data, mask, kstart, klen, what))
    for name, anchor, off, what in DATA:
        a = [r for r in rows if r[0] == anchor][0]
        k = off - a[2]
        if not (0 <= k and k + 4 <= len(a[3])) or any(a[4][k:k + 4]):
            sys.stderr.write('data %s: offset %d of %s is not a relocated operand\n' % (name, off, anchor))
            ok = False
    if not ok:
        sys.exit(1)
    out = []
    w = out.append
    w('// Made by scripts/tvsig.py from the shareware GAME.EXE (Terminal Velocity 1.2): do not edit.')
    w('// Each anchor: a window of the code object (mask 0: a byte the loader relocates, or a near call\'s offset),')
    w('// the place it stands for (from the window\'s start), and its longest run of kept bytes (the search key).')
    w('#ifndef WCNET_TV_SIG_H')
    w('#define WCNET_TV_SIG_H')
    w('namespace wc { namespace tvsig {')
    w('struct Anchor { const char *name; Bit32u image; int place; int len; const Bit8u *bytes; const Bit8u *mask; int key; int keyLen; };')
    w('struct Data { const char *name; int anchor; int at; Bit32u image; };')
    w('enum AnchorId {')
    for i, r in enumerate(rows):
        w('    A_%s = %d,     // %s' % (r[0].upper(), i, r[7]))
    w('    A_COUNT = %d' % len(rows))
    w('};')
    for r in rows:
        w('static const Bit8u k_%s_bytes[%d] = { %s };' % (r[0], len(r[3]), ', '.join('0x%02x' % b for b in r[3])))
        w('static const Bit8u k_%s_mask[%d] = { %s };' % (r[0], len(r[4]), ', '.join('%d' % (1 if b else 0) for b in r[4])))
    w('static const Anchor kAnchors[A_COUNT] = {')
    for r in rows:
        w('    { "%s", 0x%X, %d, %d, k_%s_bytes, k_%s_mask, %d, %d },' % (r[0], r[1], -r[2], len(r[3]), r[0], r[0], r[5], r[6]))
    w('};')
    w('enum DataId {')
    for i, d in enumerate(DATA):
        w('    D_%s = %d,     // %s' % (d[0].upper(), i, d[3]))
    w('    D_COUNT = %d' % len(DATA))
    w('};')
    w('static const Data kData[D_COUNT] = {')
    for name, anchor, off, what in DATA:
        a = [r for r in rows if r[0] == anchor][0]
        val = struct.unpack_from('<I', img, a[1] + off)[0]
        w('    { "%s", A_%s, %d, 0x%X },' % (name, anchor.upper(), off - a[2], val))
    w('};')
    w('} }  // namespace wc::tvsig')
    w('#endif')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
