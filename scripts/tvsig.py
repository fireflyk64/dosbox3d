#!/usr/bin/env python3
"""Terminal Velocity: the hooks' signatures, made from the shareware GAME.EXE and, where the registered game's code
is another shape, from the registered GAME.EXE too (docs/terminalvelocity.md).

    scripts/tvsig.py SHAREWARE/GAME.EXE REGISTERED/GAME.EXE > src/cpu/wcnet_tv_sig.h

The emulator finds the program in memory by its code, not by its file, and reads every data address it uses out
of the code itself: each ANCHOR below is a window of the code object's bytes around one place (the bytes the
loader relocates and the offsets of near calls masked out, since another build lays things out elsewhere), and
each DATA item is the 32-bit operand at a fixed distance into one anchor's window, which the loader has made the
linear address the game uses.  A build whose windows all match once (the registered game, if its code is the
same but for where things are) is flown as this one; any window not found leaves the game alone.

The registered game (the CD-ROM edition's GAME.EXE, as GOG sells it) is another compile of the same source: 30 of
the 31 windows and every natively drawn routine are in it byte for byte (but where things are); its campaign loop
keeps the level's index in memory, not in ebx, so that anchor has a second window (VARIANTS), made from the
registered program, with its own data offsets.  The emulator tries an anchor's first window, then its variants.

Each window must match exactly once in its own GAME.EXE's code object, and every anchor (by its first window or a
variant) and every routine once in each program given: checked here, the header not written otherwise.
scripts/tvsigcheck.py tries another build's GAME.EXE the same way.
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
    ('weapons', 0x1BAB7, 0, 0x60, "the weapons' frame: the afterburner (slot 22) taken while its key is held and it has fuel, the fire key"),
    ('flight', 0x1228C, 0, 0x98, "the flight's attitude step: the spin rates made a rotation, the attitude turned by it"),
    ('hit_end', 0x2937F, -16, 27, "the shot-against-objects test's end: the hits counted returned"),
    ('level_fn', 0x2CDF0, 0, 0x37, "the level flown: (its file's name), the campaign's call ebx its index; 1 back for a level done"),
    ('settings', 0x329AF, 0, 0x30, "the settings' writer: the difficulty and the briefing, cinema and demo flags"),
    ('level_end', 0x2D8B8, 0, 0x2D, "the level loop's end of a frame: the level done (the exit flown, or TRINEXT), else a death"),
    ('message_fn', 0x2CC00, 0, 40, "a line of the game's own on the screen for a while: (text, 16.16 seconds); the net game's chat shows so"),
    ('view_keys', 0x1147C, 0, 0x40, "the input handler's B key: the view (0 the cockpit, 2 the chase camera) cycled"),
    ('tunnel_enter_fn', 0x42380, 0, 0x40, "a tunnel entered: (its number); the outdoor objects saved to enemy.sav/enemy2.sav, the tunnel's loaded"),
    ('tunnel_loop', 0x42540, 0, 0x70, "the tunnels' mouths tried each frame: their count, their records (0x173C each, +0x30 the mouth's x y z)"),
    ('tunnel_frame', 0x42A90, 0, 0x30, "each frame of the level: the mouths tried outdoors, the way out in a tunnel (its z past the end)"),
    ('launch_spawn', 0x274FC, -0x6C, 0x80, "a launcher's update (0x27490): its 4 s up and its last craft off the pad, a new object about to be made (the launcher ebp, the table edi)"),
    ('launch_end', 0x2770D, -0x1D, 0x2B, "the launcher's update's end (the table put back from edi): a new object made by then"),
    ('generate_spawn', 0x2838D, -0x3D, 0x48, "a generator's update (0x28350): its counter past its time, a new object of its type about to be made (the generator edx, the table ebp)"),
    ('generate_end', 0x28530, -0x24, 0x32, "the generator's update's end (the table put back from ebp): a new object made by then"),
    ('reinforce', 0x23EF1, -0xB1, 0xB8, "the level's reinforcements (0x23E40, after the objects' loop): its 4 s clock taken, a craft about to be put near the player if few hostiles are awake near it (1/2/4 by difficulty)"),
    ('reinforce_end', 0x241CA, -0x19, 0x27, "the reinforcements' end: a craft made by then"),
    ('load_model_fn', 0x402E0, 0, 48, "a model loaded from the game's files, by name: its handle (a name it cannot find ends the program)"),
    ('campaign', 0x2EC79, 0x2EC58 - 0x2EC79, 0x2EC8F - 0x2EC58, "the campaign loop's call of the level function (the level's index in memory, its file's name pushed): the index, the count, the names"),
    ('span_call', 0x3E85D, -9, 0x30, "the polygon filler's call of a scanline's span routine (through the pointer the polygon's kind set): esi, edi its two edges, ebx the row"),
]

# An anchor's other windows, from another program: anchor, the program (1: the registered GAME.EXE, the second
# argument), place, window start, length, and its data items' offsets from the place.
VARIANTS = [
    ('campaign', 1, 0x2ED8E, 0x2ED74 - 0x2ED8E, 0x2ED9E - 0x2ED74,
     {'level_index': 0x2ED74 + 1 - 0x2ED8E, 'level_count': 0x2ED79 + 2 - 0x2ED8E, 'level_names': 0x2ED88 + 1 - 0x2ED8E}),
]

# The span routines drawn natively (src/cpu/wcnet_tv_span.cpp): name, image start, image end, what it is.  Each is
# compared whole (every byte but the relocated operands) before its twin is used: a build whose routine differs at
# all keeps the game's own.  'dispatch' is the jump through the routine pointer (its operand: the pointer's address).
SPANS = [
    ('dispatch', 0x49707, 0x49720, "the span call's jump through the routine pointer (and the padding after the empty function 0x49710: the frame cap's idle)"),
    ('persp32_t64_fogA', 0x4732E, 0x480D4, "perspective, a divide every 32 pixels, a 64x64 texture, the depth fog (w*4)"),
    ('persp32_t256_fogB', 0x44C97, 0x458D4, "perspective, a divide every 32 pixels, a 256x256 texture, the depth fog (w/8)"),
    ('persp16_t64_fogA', 0x46552, 0x46D98, "perspective, a divide every 16 pixels, a 64x64 texture, the depth fog (w*4)"),
    ('persp16_t256_fogB', 0x444FA, 0x44C97, "perspective, a divide every 16 pixels, a 256x256 texture, the depth fog (w/8)"),
    ('affine_lit', 0x42F18, 0x431BE, "u/w and v/w divided at the span's ends only, stepped between, one shade (the light table) for the span"),
    ('affine_clear', 0x480D4, 0x48379, "as affine_lit unlit, colour 0 not drawn (see-through), u and v stepped in a packed pair (add, adc)"),
    ('zbuf_lit', 0x48693, 0x48A90, "u and v stepped (no divide) into a strip, then each pixel nearer than the depth buffer's drawn lit and its depth kept"),
    ('fill', 0x49503, 0x4954A, "one colour (rep stos)"),
    ('none', 0x42F13, 0x42F18, "no pixels (push, pop, ret)"),
    ('height', 0x3C7E0, 0x3C96B, "the terrain's height under a point (x, z): the height map's cell interpolated on the half its diagonal cuts (an entry)"),
    ('poly', 0x3E510, 0x3E8E3, "the polygon filler 0x3E720 (vertices, count) with its edge setup 0x3E510 and edge finder 0x3E6E0: projected, edges stepped row by row, a span routine per row (an entry)"),
    ('cockpit_copy', 0x2FAB0, 0x2FB17, "the cockpit's 320-wide picture into the 640-wide frame, each byte doubled, by its rows' runs (an entry: its caller's call made natively)"),
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
    ('key_table', 'weapons', 0x1BABC - 0x1BAB7 + 2, "the keys down, a byte by scan code"),
    ('burn_key', 'weapons', 0x1BAB7 - 0x1BAB7 + 1, "the afterburner's key (a scan code; VOX.INI's weaponKey, 56 Left Alt)"),
    ('fire_key', 'weapons', 0x1BB09 - 0x1BAB7 + 1, "the fire key (57 Space)"),
    ('burn_fuel', 'weapons', 0x1BAD3 - 0x1BAB7 + 2, "the afterburner's fuel (0 at the start; a pickup or TRIBURN gives some)"),
    ('local_spin', 'flight', 1, "the ship's turn rates about its own axes (the stick's and keys' doing)"),
    ('attitude', 'flight', 0x122FF - 0x1228C + 1, "the ship's attitude: a word, then a 3x3 float matrix (columns its right, up, forward)"),
    ('difficulty', 'settings', 0x329C3 - 0x329AF + 2, "the difficulty (0..3, Easy..Terminal)"),
    ('level_done', 'level_end', 2, "the level done: set, the level loop ends at the frame's end as completed (alive)"),
    ('pod_table', 'load_model_fn', 0x402EC - 0x402E0 + 1, "the archives' table the model loader looks a name up in"),
    ('view_mode', 'view_keys', 0x1148F - 0x1147C + 2, "the view: 0 the cockpit's, 1 and 2 cameras outside the ship"),
    ('tunnel_index', 'tunnel_enter_fn', 0x423A6 - 0x42380 + 2, "the tunnel flown (its number), while in one"),
    ('in_tunnel', 'tunnel_enter_fn', 0x423AC - 0x42380 + 2, "1 in a tunnel (the object table then the tunnel's own)"),
    ('tunnel_count', 'tunnel_loop', 0x4257B - 0x42540 + 2, "the level's tunnels"),
    ('tunnel_mouths', 'tunnel_loop', 0x425A9 - 0x42540 + 2, "tunnel 0's mouth, x y z (the next tunnel's 0x173C further)"),
    ('nav_index', 'tunnel_loop', 0x42547 - 0x42540 + 2, "the nav point flown to (the level's NAV list, 0..)"),
    ('nav_types', 'tunnel_loop', 0x4255F - 0x42540 + 3, "the nav points' types, 0xA4 bytes apart (0 targets, 1 tunnel, 2 checkpoint, 3 departure, 4 tunnel's exit, 5 the mission goal: a boss; 6 the start): its place x y z 0xC before, a boss's object +0x54, its hit zones' count +0x68 and objects +0x6C"),
    ('tunnel_length', 'tunnel_frame', 0x42AB1 - 0x42A90, "the tunnel's length in 16-unit steps of z (the way out past it)"),
    ('awake_hostiles', 'reinforce', 0x23E87 + 2 - 0x23EF1, "the hostiles awake near the player this frame (the objects' loop counts them): the reinforcements' measure"),
    ('reinforce_clock', 'reinforce', 0x23EB8 + 2 - 0x23EF1, "the reinforcements' clock (16.16: a quarter of the frame time added, one at 1.0)"),
    ('level_index', 'campaign', 0x2EC58 + 2 - 0x2EC79, "the campaign's level, 0.."),
    ('level_count', 'campaign', 0x2EC5E + 2 - 0x2EC79, "the campaign's level count"),
    ('level_names', 'campaign', 0x2EC6D + 1 - 0x2EC79, "the levels' file names, 0x28 bytes each"),
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
    # (the window read on a little past its end: an instruction its end cuts is still told, a near call's offset
    # bytes inside the window masked like any other's)
    for ins in md.disasm(bytes(img[a:a + length + 16]), a):
        if ins.address >= a + length:
            break
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
    bstart, best = key_of(mask)
    key = data[bstart:bstart + best]
    out = []
    j = img.find(key, lo, hi)
    while j >= 0:
        s = j - bstart
        if all(mask[k] == 0 or img[s + k] == data[k] for k in range(len(data))):
            out.append(s)
        j = img.find(key, j + 1, hi)
    return out, bstart, best


def key_of(mask):
    """The longest run of kept bytes: (start, length)."""
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
    return bstart, best


def span_window(le, img, a, b):
    return bytes(img[a:b]), bytes(0 if a + i in le.fixed else 0xff for i in range(b - a))


def build(les, imgs, err):
    """The anchors' windows: rows of (name, place, start, data, mask, key start, key length, what, program, data
    offsets or None): each anchor's first window (program 0), then the variants.  False too if a check failed."""
    rows, ok = [], True
    fire = [None] * len(les)
    todo = [(n, 0, p, s, l, w, None) for n, p, s, l, w in ANCHORS]
    for n, prog, p, s, l, offs in VARIANTS:
        todo.append((n, prog, p, s, l, [a[4] for a in ANCHORS if a[0] == n][0], offs))
    for name, prog, place, start, length, what, offs in todo:
        if prog >= len(les):
            err('anchor %s: its variant needs program %d (the registered GAME.EXE)' % (name, prog))
            return rows, False
        le, img = les[prog], imgs[prog]
        o = le.objs[0]
        data, mask = window(le, img, place, start, length)
        found, kstart, klen = matches(img, o['base'], o['base'] + o['vsize'], data, mask)
        if found != [place + start]:
            err('anchor %s: found at %s, not once at %x' % (name, [hex(f) for f in found], place + start))
            ok = False
        if klen < 4:
            err('anchor %s: its longest kept run is %d bytes' % (name, klen))
            ok = False
        rows.append((name, place, start, data, mask, kstart, klen, what, prog, offs))
    for name, anchor, off, what in DATA:
        for r in rows:
            if r[0] != anchor:
                continue
            if r[9] is not None and name not in r[9]:
                err('data %s: the variant of %s has no offset for it' % (name, anchor))
                ok = False
                continue
            k = (off if r[9] is None else r[9][name]) - r[2]
            if not (0 <= k and k + 4 <= len(r[3])) or any(r[4][k:k + 4]):
                err('data %s: offset %d of %s (program %d) is not a relocated operand' % (name, k + r[2], anchor, r[8]))
                ok = False
    return rows, ok


def locate(rows, les, imgs, prog, say=None):
    """Program prog tried as the emulator tries memory: each anchor by its first window that is there once, each
    data operand relocated, each routine once (or the nearest to where the dispatch's move puts it).  The count of
    what was not found."""
    le, img = les[prog], imgs[prog]
    o = le.objs[0]
    lo, hi = o['base'], o['base'] + o['vsize']
    bad = 0
    for name, place, start, length, what in ANCHORS:
        got = None
        for v, r in enumerate([r for r in rows if r[0] == name]):
            found, _, _ = matches(img, lo, hi, r[3], r[4])
            if len(found) == 1:
                got = (v, r, found[0] - r[2])
                break
        if say:
            say('%-18s %s' % (name, 'MISSING <<<' if not got else '%05X%s' % (got[2], ' (variant %d)' % got[0] if got[0] else '')))
        if not got:
            bad += 1
            continue
        for dname, anchor, off, dwhat in DATA:
            if anchor != name:
                continue
            at = got[2] + (off if got[1][9] is None else got[1][9][dname])
            fixed = all(at + k in le.fixed for k in range(4))
            bad += not fixed
            if say:
                say('  data %-16s %05X %s' % (dname, struct.unpack_from('<I', img, at)[0], '' if fixed else 'NOT RELOCATED <<<'))
    shift = None
    for name, s, e, what in SPANS:
        data, mask = span_window(les[0], imgs[0], s, e)
        found, _, _ = matches(img, lo, hi, data, mask)
        if shift is not None:
            found = sorted(found, key=lambda f: abs(f - s - shift))[:1]
        elif len(found) == 1:
            shift = found[0] - s
        bad += len(found) != 1
        if say:
            say('span %-18s %05X -> %s' % (name, s, ' '.join('%05X' % f for f in found) if len(found) == 1 else 'MISSING or not once <<<'))
    return bad


def main():
    err = lambda t: sys.stderr.write(t + '\n')
    les = [tvexe.LE(p) for p in sys.argv[1:3]]
    imgs = [le.load() for le in les]
    le, img = les[0], imgs[0]
    rows, ok = build(les, imgs, err)
    for prog in range(len(les)):
        if ok and locate(rows, les, imgs, prog):
            locate(rows, les, imgs, prog, err)
            err('program %d: not all found' % prog)
            ok = False
    if not ok:
        sys.exit(1)
    fire = [[r[1] for r in rows if r[0] == 'fire_fn'][0]]
    for prog in range(1, len(les)):
        r = [r for r in rows if r[0] == 'fire_fn'][0]
        o = les[prog].objs[0]
        fire.append(matches(imgs[prog], o['base'], o['base'] + o['vsize'], r[3], r[4])[0][0] - r[2])
    first = [r for r in rows if r[9] is None]
    variants = [r for r in rows if r[9] is not None]
    out = []
    w = out.append
    w('// Made by scripts/tvsig.py from the shareware GAME.EXE and the registered one (Terminal Velocity 1.2): do not edit.')
    w('// Each anchor: a window of the code object (mask 0: a byte the loader relocates, or a near call\'s offset),')
    w('// the place it stands for (from the window\'s start), its longest run of kept bytes (the search key), and the')
    w('// fire function\'s image address in the program the window is from.  A variant: an anchor\'s other window, from')
    w('// the registered program, tried when the first is not found, with its own data offsets.')
    w('#ifndef WCNET_TV_SIG_H')
    w('#define WCNET_TV_SIG_H')
    w('namespace wc { namespace tvsig {')
    w('struct Anchor { const char *name; Bit32u image; int place; int len; const Bit8u *bytes; const Bit8u *mask; int key; int keyLen; Bit32u fire; };')
    w('struct Data { const char *name; int anchor; int at; Bit32u image; };')
    w('struct Variant { int anchor; Anchor window; };')
    w('struct VariantData { int variant; int data; int at; };')
    w('enum AnchorId {')
    for i, r in enumerate(first):
        w('    A_%s = %d,     // %s' % (r[0].upper(), i, r[7]))
    w('    A_COUNT = %d' % len(first))
    w('};')
    tag = lambda r: r[0] if r[9] is None else '%s_v%d' % (r[0], variants.index(r))
    for r in rows:
        w('static const Bit8u k_%s_bytes[%d] = { %s };' % (tag(r), len(r[3]), ', '.join('0x%02x' % b for b in r[3])))
        w('static const Bit8u k_%s_mask[%d] = { %s };' % (tag(r), len(r[4]), ', '.join('%d' % (1 if b else 0) for b in r[4])))
    row = lambda r: '{ "%s", 0x%X, %d, %d, k_%s_bytes, k_%s_mask, %d, %d, 0x%X }' % (r[0], r[1], -r[2], len(r[3]), tag(r), tag(r), r[5], r[6], fire[r[8]])
    w('static const Anchor kAnchors[A_COUNT] = {')
    for r in first:
        w('    %s,' % row(r))
    w('};')
    w('enum { V_COUNT = %d };' % len(variants))
    w('static const Variant kVariants[V_COUNT + 1] = {')
    for r in variants:
        w('    { A_%s, %s },' % (r[0].upper(), row(r)))
    w('    { -1, { 0, 0, 0, 0, 0, 0, 0, 0, 0 } }')
    w('};')
    w('enum DataId {')
    for i, d in enumerate(DATA):
        w('    D_%s = %d,     // %s' % (d[0].upper(), i, d[3]))
    w('    D_COUNT = %d' % len(DATA))
    w('};')
    w('static const Data kData[D_COUNT] = {')
    for name, anchor, off, what in DATA:
        a = [r for r in first if r[0] == anchor][0]
        val = struct.unpack_from('<I', img, a[1] + off)[0]
        w('    { "%s", A_%s, %d, 0x%X },' % (name, anchor.upper(), off - a[2], val))
    w('};')
    vd = []
    for v, r in enumerate(variants):
        for i, d in enumerate(DATA):
            if d[1] == r[0]:
                vd.append('    { %d, D_%s, %d },' % (v, d[0].upper(), r[9][d[0]] - r[2]))
    w('enum { VD_COUNT = %d };' % len(vd))
    w('static const VariantData kVariantData[VD_COUNT + 1] = {')
    out.extend(vd)
    w('    { -1, -1, 0 }')
    w('};')
    # the span routines, whole: bytes, mask (0 a relocated byte), and each relocated operand's offset and image value
    w('struct Span { const char *name; Bit32u image; int len; const Bit8u *bytes; const Bit8u *mask; int nrel; const Bit32u *rel; int key; int keyLen; };')
    w('enum SpanId {')
    for i, (name, a, b, what) in enumerate(SPANS):
        w('    S_%s = %d,     // %s' % (name.upper(), i, what))
    w('    S_COUNT = %d' % len(SPANS))
    w('};')
    nrels, keys = {}, {}
    for name, a, b, what in SPANS:
        data = bytes(img[a:b])
        mask = bytes(0 if a + i in le.fixed else 1 for i in range(b - a))
        rel = []
        i = 0
        while i < b - a:
            if not mask[i]:
                if i + 4 > b - a or any(mask[i:i + 4]):
                    sys.stderr.write('span %s: a relocated field at +%d is not 4 bytes\n' % (name, i))
                    sys.exit(1)
                rel += [i, struct.unpack_from('<I', img, a + i)[0]]
                i += 4
            else:
                i += 1
        if name != 'dispatch' and data[-1] != 0xC3:
            sys.stderr.write('span %s: does not end with its ret\n' % name)
            sys.exit(1)
        nrels[name] = len(rel) // 2
        keys[name] = key_of(mask)
        w('static const Bit8u k_span_%s_bytes[%d] = { %s };' % (name, len(data), ', '.join('0x%02x' % x for x in data)))
        w('static const Bit8u k_span_%s_mask[%d] = { %s };' % (name, len(mask), ', '.join('%d' % x for x in mask)))
        w('static const Bit32u k_span_%s_rel[%d] = { %s };' % (name, max(1, len(rel)), ', '.join('0x%X' % x for x in rel) or '0'))
    w('static const Span kSpans[S_COUNT] = {')
    for name, a, b, what in SPANS:
        w('    { "%s", 0x%X, %d, k_span_%s_bytes, k_span_%s_mask, %d, k_span_%s_rel, %d, %d },' % (name, a, b - a, name, name, nrels[name], name, keys[name][0], keys[name][1]))
    w('};')
    w('} }  // namespace wc::tvsig')
    w('#endif')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
