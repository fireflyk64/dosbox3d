#!/usr/bin/env python3
"""Raptor: Call of the Shadows: the hooks' signatures, made from the shareware RAP.EXE 1.2 (docs/raptor.md).

    scripts/rapsig.py RAP.EXE > src/cpu/wcnet_rap_sig.h

As scripts/tvsig.py does for Terminal Velocity (its window() and matches() are used here): each ANCHOR is a window
of the code object's bytes about one place (the bytes the loader relocates and the offsets of calls out of it
masked), checked to match exactly once in this RAP.EXE's code; each DATA item is the 32-bit operand at a fixed
distance into an anchor's window, which the loader has made the linear address the game uses.  The names are the
game's own (its 1.2 source, published under the GPL: skynettx/dosraptor), the addresses the shareware image's.
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tvexe
from tvsig import window, matches

# name, image address of the place, window start (relative), window length, what it is
ANCHORS = [
    ('begtext', 0x10000, 0, 0x10, "the code's first bytes (int 3; jmp $-1; 13 nops, never run): the holds' idle is written there"),
    ('game_entry', 0x11128, 0, 0x50, "Do_Game (): a wave flown, its loop to its end; srand (1024 * game_wave [cur_game]) at its start"),
    ('game_init', 0x111BF, 0, 0x30, "Do_Game: playerx, playery to their start (144, 160)"),
    ('buffers', 0x11226, 0, 0x6A, "Do_Game: displaybuffer cleared, local_cnt = FRAME_COUNT, start_score = plr.score"),
    ('frame_top', 0x112A4, -0x1C, 0x40, "Do_Game's loop: its top (SHADOW_Clear, then IPT_MovePlayer)"),
    ('think', 0x11606, 0, 0x50, "Do_Game's loop: startendwave counted down (end_wave at 0), gl_cnt++, the Think () calls"),
    ('draw_player', 0x11651, 0, 0x30, "Do_Game's loop: draw_player, the ship's shadow from curship [playerpic + g_flash]"),
    ('draw_flames', 0x116B9, 0, 0x40, "Do_Game's loop: the ship's engine flames at player_cx, player_cy"),
    ('draw_point', 0x11775, -0x34, 0x40, "Do_Game's loop: ANIMS_DisplayHigh () called, the ship drawn by then: the others' ships drawn here"),
    ('wait', 0x11839, -0x12, 0x2A, "Do_Game's loop: the frame's wait (FRAME_COUNT - local_cnt < 3: 140 Hz / 3) done"),
    ('game_end', 0x11ABB, -0x10, 0x20, "Do_Game's loop left: the wave over (its end, an abort, a death)"),
    ('enemy_spawn', 0x1845C, -0x52, 0x66, "ENEMY_Think: the wave's next sprites at the scroll's row: ENEMY_Add (cur_enemy) called"),
    ('enemy_list', 0x1849A, 0, 0x30, "ENEMY_Think: the live enemies (first_enemy.next .. &last_enemy)"),
    ('kill', 0x18F81, 0, 0x30, "ENEMY_Think: an enemy's hits gone (<= 0): plr.score += curlib->money, its explosion, its bonus"),
    ('enemy_clear', 0x179C8, 0, 0x7E, "ENEMY_Clear: free_enemy = ships, the wave's sprites (csprite, ml->numsprites)"),
    ('diff', 0x1760C, -0x10, 0x20, "ENEMY_LoadSprites: a sprite's level & cur_diff (the difficulty's sprites)"),
    ('curplr', 0x17D8E, -0x8, 0x20, "ENEMY_Add: a boss halved at curplr_diff <= DIFF_1"),
    ('launch', 0x257F4, -0x1C, 0x40, "WIN_MainLoop: the ship computer's sector chosen (cur_game, game_wave): the wave's song, its load, Do_Game next"),
    ('objs_amt', 0x1B1EE, 0, 0x30, "OBJS_GetAmt (type): p_objs [type]->num"),
    ('glb_fetch', 0x27AC2, 0, 0x40, "GLB_FetchItem: an item's handle (archive << 16 | item) to its entry (0x28 bytes, +0x10 its memory)"),
    ('shots_display', 0x15E9D, 0, 0x40, "SHOTS_Display: the player's shots (first_shots.next .. &last_shots)"),
    ('alt_x', 0x1192F, 0, 0x20, "Do_Game's loop: KBD_Key (SC_X) && KBD_Key (SC_ALT) (the keys down: a BOOL a scan code)"),
    ('tile_kill', 0x12D7C, -0x4F, 0x5A, "TILE_Think: a tile's hits gone (< 0) and not dead: plr.score += money [mapspot] (the map spot in [ebp - 0x14])"),
    ('shadow_add', 0x172BF, 0, 0x30, "SHADOW_Add (item, x, y): an air ship's shadow put in shads [num_shadows] (at most MAX_SHADOWS, the cmp's byte), drawn by SHADOW_DisplaySky"),
    ('flame_shades', 0x19760, 0, 0x44, "FLAME_InitShades: the flames' light tables made from the game's palette"),
]

# name, anchor, offset of the operand from the anchor's place, what the address is
DATA = [
    ('cur_game', 'game_entry', 0x11163 - 0x11128 + 1, "the sector flown (0 Bravo, 1 Tango, 2 Outer Regions)"),
    ('game_wave', 'game_entry', 0x1116B - 0x11128 + 2, "the wave of each sector (4 dwords)"),
    ('draw_player', 'game_entry', 0x11159 - 0x11128 + 2, "the ship drawn (0 once it has blown up)"),
    ('playerx', 'game_init', 2, "the ship's left (5..314)"),
    ('playery', 'game_init', 0x111C9 - 0x111BF + 2, "its top (0..160)"),
    ('displaybuffer', 'buffers', 0x1122D - 0x11226 + 1, "the pointer to the 320 x 200 frame drawn"),
    ('frame_count', 'buffers', 0x11248 - 0x11226 + 1, "FRAME_COUNT (140 a second)"),
    ('plr_score', 'buffers', 0x11286 - 0x11226 + 1, "plr.score: the pilot's money"),
    ('startendwave', 'think', 2, "the wave's end counting down (-1: not ending)"),
    ('end_wave', 'think', 0x11618 - 0x11606 + 2, "the wave over (the loop ends)"),
    ('gl_cnt', 'think', 0x11628 - 0x11606 + 2, "the frames of the game"),
    ('playerpic', 'draw_player', 0x11666 - 0x11651 + 1, "the ship's frame (its bank)"),
    ('g_flash', 'draw_player', 0x1166B - 0x11651 + 2, "the ship's flash frame (hit)"),
    ('curship', 'draw_player', 0x11674 - 0x11651 + 2, "the ship's frames (GLB items, the wave's set)"),
    ('player_cy', 'draw_flames', 0x116D3 - 0x116B9 + 2, "the ship's centre y"),
    ('player_cx', 'draw_flames', 0x116E5 - 0x116B9 + 1, "the ship's centre x"),
    ('tilepos', 'enemy_spawn', 0x1840F - 0x1845C + 1, "the map's scroll (its tile, 9 a row)"),
    ('tiley', 'enemy_spawn', 0x18422 - 0x1845C + 1, "the scroll's row less 3: the wave's sprites at it are added"),
    ('end_waveflag', 'enemy_spawn', 0x18427 - 0x1845C + 2, "the wave's last sprite added"),
    ('cur_enemy', 'enemy_spawn', 0x18434 - 0x1845C + 1, "the wave's next sprite (CSPRITE, 0x18 bytes)"),
    ('end_enemy', 'enemy_spawn', 0x18466 - 0x1845C + 2, "the wave's last sprite"),
    ('first_enemy_next', 'enemy_list', 0x184A4 - 0x1849A + 1, "first_enemy.next: the live enemies' list (SPRITE_SHIP, 0xBC bytes: +0x18 x, +0x1C y, +0x50 hits)"),
    ('last_enemy', 'enemy_list', 0x184AC - 0x1849A + 1, "&last_enemy: the list's end"),
    ('plr_score_kill', 'kill', 0x18F87 - 0x18F81 + 2, "plr.score (as the kill adds to it)"),
    ('ships', 'enemy_clear', 6, "ships [30]: the enemies' slots"),
    ('ml', 'enemy_clear', 0x17A17 - 0x179C8 + 1, "the wave's map header"),
    ('csprite', 'enemy_clear', 0x17A2E - 0x179C8 + 2, "the pointer to the wave's sprites (CSPRITE, 0x18 bytes each)"),
    ('cur_diff', 'diff', 2, "the difficulty's sprite levels (EB_ bits)"),
    ('curplr_diff', 'curplr', 2, "the pilot's difficulty in this sector (0 training .. 3 elite)"),
    ('p_objs', 'objs_amt', 0x1B205 - 0x1B1EE + 2, "p_objs [type]: the pilot's objects (OBJ: +8 num; 15 the super shield, 16 the energy)"),
    ('glb_items', 'glb_fetch', 0x27AE7 - 0x27AC2 + 2, "filedesc [0].item: each archive's entries (0xA0 bytes an archive)"),
    ('first_shots_next', 'shots_display', 0x15EAB - 0x15E9D + 1, "first_shots.next: the player's shots (SHOTS, 0x60 bytes: +8 pic, +0xC x, +0x10 y, +0x14 move, +0x58 lib)"),
    ('last_shots', 'shots_display', 0x15EB3 - 0x15E9D + 1, "&last_shots: their list's end"),
    ('keyboard_x', 'alt_x', 2, "keyboard [SC_X]: the keys down (a BOOL a scan code, from keyboard [0] 0xB4 before it)"),
    ('tile_hits', 'tile_kill', 0x12D2D - 0x12D7C + 3, "the map's tiles' hits (a SHORT a map spot: 9 a row, 150 rows)"),
    ('tile_dead', 'tile_kill', 0x12D3C - 0x12D7C + 3, "the map's tiles destroyed (a SHORT a map spot)"),
    ('num_shadows', 'shadow_add', 0x172D4 - 0x172BF + 2, "num_shadows: the air shadows of this frame"),
    ('shads', 'shadow_add', 0x172DB - 0x172BF + 1, "shads [MAX_SHADOWS]: the air shadows (item, x, y: 12 bytes each)"),
    ('o_engine', 'draw_flames', 0x116EA - 0x116B9 + 2, "o_engine [8]: how far each engine's flame is from the ship's middle at each bank"),
    ('palette', 'flame_shades', 0x19798 - 0x19760 + 1, "palette: the pointer to the game's palette (768 bytes, 0..63 each)"),
]


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
    w('// Made by scripts/rapsig.py from the shareware RAP.EXE (Raptor: Call of the Shadows 1.2): do not edit.')
    w('// Each anchor: a window of the code object (mask 0: a byte the loader relocates, or a call\'s offset), the place')
    w('// it stands for (from the window\'s start), and its longest run of kept bytes (the search key).')
    w('#ifndef WCNET_RAP_SIG_H')
    w('#define WCNET_RAP_SIG_H')
    w('namespace wc { namespace rapsig {')
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
    w('} }  // namespace wc::rapsig')
    w('#endif')
    print('\n'.join(out))


if __name__ == '__main__':
    main()
