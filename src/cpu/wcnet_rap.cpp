/*
 *  Raptor: Call of the Shadows: finding the program in memory and watching it (wcnet_rap.h, docs/raptor.md).
 *
 *  RAP.EXE is bound with DOS/4GW Professional: DOS loads it, the extender puts its LE image (a code object, image
 *  0x10000, a data object, image 0x60000) in extended memory, flat (every selector's base 0, paging off), so EIP and
 *  every pointer the program holds are linear addresses.  The code is looked for by scripts/rapsig.py's windows
 *  (wcnet_rap_sig.h, made from the shareware's RAP.EXE 1.2), every data address read out of the found code's own
 *  operands.  The names below are the game's own (its 1.2 source, published under the GPL).
 *
 *  The places watched: Do_Game's entry (a wave begun) and its loop's top (each frame: our damage to the wave's
 *  enemies measured and told, the others' taken off), its draw point (the others' ships and shots drawn into the
 *  frame), its frame wait's end (a machine ahead of the others held there), its loop's end (the wave over); in
 *  ENEMY_Think the call adding the wave's next enemy (its index in the wave's script noted for its slot) and the
 *  kill (the money left out for a kill the others' damage made); in WIN_MainLoop the sector chosen (the launch,
 *  together: wcnet_rap_session.cpp).
 */
#include <algorithm>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcnet_rap.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "cpu.h"
#include "pic.h"
#include "vga.h"
#include "wcnet_rap_sig.h"

namespace wc {

namespace {

using namespace rapsig;

enum {
    kShipSize = 0xBC,           // SPRITE_SHIP
    kShips = 30,                // MAX_ONSCREEN
    kCspriteSize = 0x18,        // CSPRITE
    kHitsAt = 0x50,             // SPRITE_SHIP.hits
    kShotLibAt = 0x58,          // SHOTS.lib
    kEnergy = 16,               // S_ENERGY
    kMapLeft = 16, kMapRight = 320 - 16,
    kMapSize = 9 * 150,         // MAP_COLS * MAP_ROWS: the tiles
    kTileFlag = 0x40000000,     // (RapHits: a tile's map spot, not an enemy's index)
};

bool g_armed = false, g_found = false;
double g_armedUntilMs = 0, g_nextScanMs = 0, g_nextReportMs = 0;
Bit32u g_anchor[A_COUNT];
Bit32u g_data[D_COUNT];
Bit32u g_top = 0;

enum Place { P_GAME, P_FRAME, P_DRAW, P_WAIT, P_GAME_END, P_ADD, P_ADD_AFTER, P_KILL, P_LAUNCH, P_IDLE, P_SHOTS, P_SHOTS_AFTER,
             P_TILE_MONEY, P_COUNT };
Bit32u g_places[P_COUNT];
Bit32u g_waitLoop = 0, g_killSkip = 0;
Bit32s g_maxShadows = 0;        // MAX_SHADOWS, as SHADOW_Add's cmp has it

// the wave
bool g_inWave = false;
Bit32u g_frame = 0;
bool g_launchReleased = false;  // the launch's hold over: the launch place next reached goes on
// the enemies' slots: the wave script's index of the enemy in each, its hits as we last knew them
int g_slotId[kShips];
Bit32s g_lastHits[kShips];
bool g_slotKnown[kShips];
bool g_slotRemoteKill[kShips];  // its hits gone by the others' damage (no money for its kill here)
int g_pendingAdd = -1;
// the tiles: their hits before our shots' frame (what our weapons did to them measured across SHOTS_Think: the
// explosions' damage to their neighbours is every machine's own), those the others' damage destroyed
Bit16s g_tileBefore[kMapSize];
bool g_tileRemoteKill[kMapSize];
// the others' damage not yet taken (the enemy not here yet: a frame or two behind), by index
struct Pending { int index; Bit32s damage; double at; };
std::vector<Pending> g_pending;
unsigned g_frames = 0, g_damageSent = 0, g_damageTaken = 0, g_remoteKills = 0;
bool g_god = false;             // (the aid !rapgod=1: our energy kept full)
// (the aid !rapshot=<frame>:<name>: the game's last frame at that frame of the wave, WCNET_DUMP_DIR/<name>.ppm)
std::vector<std::pair<Bit32u, std::string> > g_shotsAt;

// --- memory -------------------------------------------------------------------------------------------------------

inline bool ram(Bit32u a, Bit32u n) {
    return (a < 0xA0000u && n <= 0xA0000u - a) || (a >= 0x100000u && a < g_top && n <= g_top - a);
}
inline Bit32u r32(Bit32u a) { return ram(a, 4) ? host_readd(MemBase + a) : 0; }
inline void w32(Bit32u a, Bit32u v) { if (ram(a, 4)) host_writed(MemBase + a, v); }
inline Bit32u D(int i) { return g_data[i]; }

Bit32u place_of(int anchor) { return g_anchor[anchor] + (Bit32u)kAnchors[anchor].place; }

bool window_at(const Anchor &a, Bit32u lin) {
    if (!ram(lin, (Bit32u)a.len)) return false;
    for (int i = 0; i < a.len; i++) {
        if (a.mask[i] && MemBase[lin + (Bit32u)i] != a.bytes[i]) return false;
    }
    return true;
}

int find_anchor(const Anchor &a, Bit32u lo, Bit32u hi, Bit32u *out, int cap) {
    if (hi > g_top) hi = g_top;
    const Bit8u *key = a.bytes + a.key;
    int found = 0;
    for (Bit32u p = lo + (Bit32u)a.key; p + (Bit32u)a.keyLen <= hi;) {
        const Bit8u *hit = (const Bit8u *)memchr(MemBase + p, key[0], hi - (Bit32u)a.keyLen - p + 1);
        if (!hit) break;
        p = (Bit32u)(hit - MemBase);
        if (memcmp(hit, key, (size_t)a.keyLen) == 0 && p >= (Bit32u)a.key && window_at(a, p - (Bit32u)a.key)) {
            if (found < cap) out[found] = p - (Bit32u)a.key;
            found++;
        }
        p++;
    }
    return found;
}

// Every anchor near the game's entry's window (memory can hold a second copy of the code, the extender's read
// buffer, whose windows match too: of each anchor's matches the one nearest where the shareware's layout puts it,
// and a copy is the program only when every data operand is relocated, past the first megabyte).
bool resolve(Bit32u entry) {
    const Bit32u lo = entry > 0x100000 + 0x80000 ? entry - 0x80000 : 0x100000;
    const Bit32u hi = std::min(g_top, entry + 0x80000);
    for (int i = 0; i < A_COUNT; i++) {
        Bit32u at[8];
        const int k = find_anchor(kAnchors[i], lo, hi, at, 8);
        if (k < 1) {
            wclog(1, "Raptor: code near %08x, but '%s' was not found: not a build the hooks know", entry, kAnchors[i].name);
            return false;
        }
        const Bit32s want = (Bit32s)(kAnchors[i].image - kAnchors[i].place) - (Bit32s)kAnchors[A_GAME_ENTRY].image;
        int best = 0;
        for (int j = 1; j < std::min(k, 8); j++) {
            if (labs((Bit32s)(at[j] - entry) - want) < labs((Bit32s)(at[best] - entry) - want)) best = j;
        }
        g_anchor[i] = at[best];
    }
    for (int i = 0; i < D_COUNT; i++) {
        const Bit32u v = r32(g_anchor[kData[i].anchor] + (Bit32u)kData[i].at);
        if (v < 0x100000 || v >= g_top) return false;
        g_data[i] = v;
    }
    return true;
}

bool scan() {
    static unsigned attempts = 0;
    attempts++;
    if (PAGING_Enabled()) return false;
    g_top = (Bit32u)MEM_TotalPages() << 12;
    Bit32u entry[4];
    const int n = find_anchor(kAnchors[A_GAME_ENTRY], 0x100000, g_top, entry, 4);
    int ok = -1;
    for (int c = 0; c < std::min(n, 4) && ok < 0; c++) {
        if (resolve(entry[c])) ok = c;
    }
    if (ok < 0) {
        if (attempts % 8 == 1) wclog(2, "Raptor: scan %u: Do_Game found %d times, none the program's", attempts, n);
        return false;
    }
    g_places[P_GAME] = place_of(A_GAME_ENTRY);
    g_places[P_FRAME] = place_of(A_FRAME_TOP);
    g_places[P_DRAW] = place_of(A_DRAW_POINT);
    g_places[P_WAIT] = place_of(A_WAIT);
    g_waitLoop = g_places[P_WAIT] - (0x11839 - 0x1182C);       // (the wait's loop: mov eax, [FRAME_COUNT] ...)
    g_places[P_GAME_END] = place_of(A_GAME_END);
    g_places[P_ADD] = place_of(A_ENEMY_SPAWN);                  // (call ENEMY_Add, cur_enemy in eax)
    g_places[P_ADD_AFTER] = g_places[P_ADD] + 5;
    g_places[P_KILL] = place_of(A_KILL);
    g_killSkip = g_places[P_KILL] + (0x18F8D - 0x18F81);        // (past plr.score += curlib->money)
    g_places[P_LAUNCH] = place_of(A_LAUNCH);
    g_places[P_SHOTS] = place_of(A_THINK) + (0x11642 - 0x11606);            // (call SHOTS_Think)
    g_places[P_SHOTS_AFTER] = g_places[P_SHOTS] + 5;
    g_places[P_TILE_MONEY] = place_of(A_TILE_KILL);                         // (add [plr.score], eax: 6 bytes)
    // the holds' idle: in the code's first bytes (int 3; jmp $-1; then nops never run): jmp $; jmp <the launch>
    const Bit32u idle = place_of(A_BEGTEXT) + 3;
    MemBase[idle] = 0xEB;
    MemBase[idle + 1] = 0xFE;
    MemBase[idle + 2] = 0xE9;
    host_writed(MemBase + idle + 3, g_places[P_LAUNCH] - (idle + 7));
    g_places[P_IDLE] = idle;
    g_maxShadows = MemBase[place_of(A_SHADOW_ADD) + (0x172EB - 0x172BF)];     // (cmp [num_shadows], MAX_SHADOWS: its byte)
    return true;
}

void set_watch(bool on) {
    for (int i = 0; i < P_COUNT; i++) {
        if (g_places[i]) wc_net_watch32[g_places[i] & 0xFFFF] = on;
    }
}

void own_forget();

void forget(const char *why) {
    wclog(1, "Raptor: %s", why);
    if (g_places[P_IDLE] && reg_eip == g_places[P_IDLE]) reg_eip = g_places[P_IDLE] + 2;
    rap_session_shutdown();
    own_forget();
    set_watch(false);
    memset(g_places, 0, sizeof(g_places));
    g_armed = g_found = false;
    g_inWave = false;
}

// --- the enemies --------------------------------------------------------------------------------------------------

int slot_of(Bit32u ship) {
    const Bit32u base = D(D_SHIPS);
    if (ship < base || (ship - base) % kShipSize) return -1;
    const Bit32u s = (ship - base) / kShipSize;
    return s < kShips ? (int)s : -1;
}

void clear_enemies() {
    for (int i = 0; i < kShips; i++) {
        g_slotId[i] = -1;
        g_slotKnown[i] = false;
        g_slotRemoteKill[i] = false;
    }
    g_pendingAdd = -1;
    g_pending.clear();
    memset(g_tileRemoteKill, 0, sizeof(g_tileRemoteKill));
}

// Each frame (its top, before ENEMY_Think): what our weapons did to each live enemy since the last frame (its hits
// less than we left them) told; the others' damage taken off; the hits noted as we leave them.
void enemies_frame() {
    const double now = PIC_FullIndex();
    const Bit32u end = D(D_LAST_ENEMY);
    Bit32u ship = r32(D(D_FIRST_ENEMY_NEXT));
    for (int guard = 0; ship && ship != end && guard < kShips + 2; guard++) {
        const int s = slot_of(ship);
        if (s >= 0 && g_slotKnown[s]) {
            Bit32s hits = (Bit32s)r32(ship + kHitsAt);
            const Bit32s ours = g_lastHits[s] - hits;
            if (ours > 0 && g_slotId[s] >= 0) {
                rap_session_damage(g_slotId[s], ours);
                g_damageSent++;
            }
            // the others' damage to it
            Bit32s theirs = 0;
            for (size_t i = 0; i < g_pending.size();) {
                if (g_pending[i].index == g_slotId[s]) {
                    theirs += g_pending[i].damage;
                    g_pending.erase(g_pending.begin() + i);
                } else {
                    i++;
                }
            }
            if (theirs > 0 && hits > 0) {
                hits -= theirs;
                if (hits <= 0) {
                    g_slotRemoteKill[s] = true;
                    g_remoteKills++;
                }
                w32(ship + kHitsAt, (Bit32u)hits);
                g_damageTaken++;
            }
            g_lastHits[s] = hits;
        }
        ship = r32(ship + 4);
    }
    // the others' damage to the map's tiles
    const Bit32u th = D(D_TILE_HITS), td = D(D_TILE_DEAD);
    for (size_t i = 0; i < g_pending.size() && ram(th, 2 * kMapSize);) {
        if (!(g_pending[i].index & kTileFlag)) {
            i++;
            continue;
        }
        const int spot = g_pending[i].index & ~kTileFlag;
        if (spot >= 0 && spot < kMapSize && !host_readw(MemBase + td + 2 * spot)) {
            const Bit16s h = (Bit16s)host_readw(MemBase + th + 2 * spot);
            const Bit16s after = (Bit16s)(h - g_pending[i].damage);
            host_writew(MemBase + th + 2 * spot, (Bit16u)after);
            if (h >= 0 && after < 0) {
                g_tileRemoteKill[spot] = true;
                g_remoteKills++;
            }
            g_damageTaken++;
        }
        g_pending.erase(g_pending.begin() + i);
    }
    // (the others' damage to an enemy that has not come, or has gone: kept 4 s)
    for (size_t i = 0; i < g_pending.size();) {
        if (now - g_pending[i].at > 4000) g_pending.erase(g_pending.begin() + i);
        else i++;
    }
}

// --- drawing (natively, into the game's frame: its sprites' format, GFX_PutSprite's clipped way) -----------------

// An item of the game's archives in memory: its address and size (0: not there)
Bit32u item_mem(Bit32u item, Bit32u *size) {
    const Bit32u file = item >> 16, n = item & 0xFFFF;
    if (file >= 15) return 0;
    const Bit32u items = r32(D(D_GLB_ITEMS) + 0xA0 * file);
    const Bit32u count = r32(D(D_GLB_ITEMS) + 0xA0 * file + 4);
    if (!items || n >= count) return 0;
    const Bit32u e = items + 0x28 * n;
    const Bit32u mem = r32(e + 0x10);
    if (size) *size = r32(e + 0x18);
    return mem;
}

// Each pilot's ship by its seat (the user, 2026-11-15: "draw the second player as green stripes instead of red and the
// next as blue stripes instead of red and the last as pruple stripes if possible").  The ship's stripes are the
// palette's red ramp (32..47, light to dark; the hull its greys): seat 1's are the green ramp (96..), seat 2's the blue
// (128..), each two shades lighter; the palette has no purple, so seat 3's are a checker of the red and that blue,
// which the eye mixes at the game's 320 x 200, by the sprite's own columns and rows (it does not crawl as the ship moves).  Seat 0 keeps the game's red.
enum { kTints = 4 };
inline Bit32u hd(const Bit8u *p) { return host_readd((HostPt)p); }
inline Bit8u tint_px(Bit8u c, int tint, int x, int y) {
    if (c < 32 || c >= 48 || tint <= 0) return c;
    const int i = c - 32;
    if (tint == 1) return (Bit8u)(96 + std::max(0, i - 2));
    if (tint == 2) return (Bit8u)(128 + std::max(0, i - 2));
    return ((x + y) & 1) ? c : (Bit8u)(128 + std::max(0, i - 2));
}

// GFX_PutSprite's sprite: a GFX_PIC (type 0, ..., width, height), then segments {x, y, offset, length} each with its
// length's pixels, the last offset -1; clipped to the map (16..304) and the screen's rows.  `pic` is the sprite's bytes
// (the game's memory, or a copy of ours).
void put_sprite(const Bit8u *pic, Bit32u size, int x, int y, int tint = 0) {
    const Bit32u buf = r32(D(D_DISPLAYBUFFER));
    if (!pic || size < 0x14 || !ram(buf, 64000) || hd(pic) != 0) return;
    Bit32u p = 0x14;
    for (int guard = 0; p + 16 <= size && guard < 512; guard++) {
        const Bit32s sx = (Bit32s)hd(pic + p), sy = (Bit32s)hd(pic + p + 4), off = (Bit32s)hd(pic + p + 8),
                     len = (Bit32s)hd(pic + p + 12);
        if (off == -1) break;
        p += 16;
        if (len <= 0 || (Bit32u)len > size - p) break;
        const int ox = x + sx, oy = y + sy;
        if (oy >= 0 && oy < 200) {
            const int a = std::max(ox, (int)kMapLeft), b = std::min(ox + len, (int)kMapRight);
            Bit8u *to = MemBase + buf + (Bit32u)(oy * 320);
            if (b > a && tint <= 0) memcpy(to + a, pic + p + (Bit32u)(a - ox), (size_t)(b - a));
            else for (int i = a; i < b; i++) to[i] = tint_px(pic[p + (Bit32u)(i - ox)], tint, i - x, sy);
        }
        p += (Bit32u)len;
    }
}

// a sprite's bytes in the game's memory (NULL: not there, or not all in memory)
const Bit8u *item_bytes(Bit32u item, Bit32u *size) {
    const Bit32u mem = item_mem(item, size);
    return mem && *size >= 0x14 && *size < 0x100000 && ram(mem, *size) ? MemBase + mem : NULL;
}

// Our own ship in our seat's colour: the game draws it itself (GFX_PutSprite of curship [playerpic + g_flash]), so its
// 14 frames are tinted where the game keeps them, in its archive's memory, while a wave is flown together, and put
// back as they were at the wave's end.  Each frame of the wave they are looked at again (an item the game let go and
// loaded again is as it was: tinted again).  The others' ships are drawn from the copies kept of them as they were.
struct OwnFrame {
    Bit32u item;
    std::vector<Bit8u> was, tinted;
    OwnFrame() : item(0) {}
};
enum { kOwnFrames = 14 };
OwnFrame g_own[kOwnFrames];

void tint_sprite(Bit8u *pic, Bit32u size, int tint) {
    if (size < 0x14 || hd(pic) != 0) return;
    Bit32u p = 0x14;
    for (int guard = 0; p + 16 <= size && guard < 512; guard++) {
        const Bit32s sx = (Bit32s)hd(pic + p), sy = (Bit32s)hd(pic + p + 4), off = (Bit32s)hd(pic + p + 8),
                     len = (Bit32s)hd(pic + p + 12);
        if (off == -1) break;
        p += 16;
        if (len <= 0 || (Bit32u)len > size - p) break;
        for (Bit32s i = 0; i < len; i++) pic[p + (Bit32u)i] = tint_px(pic[p + (Bit32u)i], tint, sx + i, sy);
        p += (Bit32u)len;
    }
}

void own_forget() {
    for (int i = 0; i < kOwnFrames; i++) g_own[i] = OwnFrame();
}

void own_restore() {
    for (int i = 0; i < kOwnFrames; i++) {
        OwnFrame &o = g_own[i];
        Bit32u size = 0;
        Bit8u *mem = o.item ? (Bit8u *)item_bytes(o.item, &size) : NULL;
        if (mem && size == o.tinted.size() && memcmp(mem, &o.tinted[0], size) == 0) memcpy(mem, &o.was[0], size);
        o = OwnFrame();
    }
}

void own_tint(int tint) {
    if (tint <= 0 || tint >= kTints) return;
    for (int i = 0; i < kOwnFrames; i++) {
        const Bit32u item = r32(D(D_CURSHIP) + 4 * (Bit32u)i);
        bool twice = false;         // (the flash frames can be the plain ones again)
        for (int j = 0; j < i && !twice; j++) twice = g_own[j].item == item;
        Bit32u size = 0;
        Bit8u *mem = item && !twice ? (Bit8u *)item_bytes(item, &size) : NULL;
        OwnFrame &o = g_own[i];
        if (!mem || size > 16384) {
            if (o.item != item) o = OwnFrame();
            continue;
        }
        if (o.item == item && o.tinted.size() == size && memcmp(mem, &o.tinted[0], size) == 0) continue;
        o.item = item;
        o.was.assign(mem, mem + size);
        tint_sprite(mem, size, tint);
        o.tinted.assign(mem, mem + size);
    }
}

// a ship's frame as the game has it (not as tinted for our own seat)
const Bit8u *ship_bytes(Bit32u item, Bit32u *size) {
    const Bit8u *mem = item_bytes(item, size);
    for (int i = 0; mem && i < kOwnFrames; i++) {
        const OwnFrame &o = g_own[i];
        if (o.item == item && o.tinted.size() == *size && memcmp(mem, &o.tinted[0], *size) == 0) return &o.was[0];
    }
    return mem;
}

void put_line(int x0, int y0, int x1, int y1, Bit8u colour) {
    const Bit32u buf = r32(D(D_DISPLAYBUFFER));
    if (!ram(buf, 64000)) return;
    const int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 1024; guard++) {
        if (x0 >= kMapLeft && x0 < kMapRight && y0 >= 0 && y0 < 200) MemBase[buf + (Bit32u)(y0 * 320 + x0)] = colour;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// The other pilots' engine flames: the game's own (FLAME_Down: two flames 4 pixels wide under the ship, 8 or 12 rows by
// the frame's parity, each row's pixels lightened through one of 8 light tables, the brightest at the ship) in another
// colour and shape, so that the two ships tell apart (the user: "Ideally other players flames are slightly different
// shape or color"): each row's pixels taken towards a pale blue (kTint) instead of lightened, the flame narrowing to 2
// pixels for its last third.  The tables are made from the game's palette the way GFX_MakeLightTable makes the game's
// (GFX_Remap: the nearest colour by the sum of the differences, the last of equals) over colours 0..239 (from 240 the
// palette's rotating colours), again whenever the palette is not the one they were made from.
const int kTint[3] = { 30, 46, 63 };
Bit8u g_flameTab[8][256];
Bit8u g_flamePal[768];
bool g_flameTabOk = false;

bool flame_tables() {
    const Bit32u pal = r32(D(D_PALETTE));
    if (!ram(pal, 768)) return false;
    if (g_flameTabOk && memcmp(g_flamePal, MemBase + pal, 768) == 0) return true;
    memcpy(g_flamePal, MemBase + pal, 768);
    for (int k = 0; k < 8; k++) {
        const int a = (8 - k) * 13;                 // (of 128: the row at the ship 81% of the way to the tint, the last 10%)
        for (int c = 0; c < 256; c++) {
            int want[3];
            for (int i = 0; i < 3; i++) {
                const int v = g_flamePal[3 * c + i] & 63;
                want[i] = v + (kTint[i] - v) * a / 128;
            }
            int best = 0, low = 1 << 30;
            for (int n = 0; n < 240; n++) {
                const Bit8u *q = g_flamePal + 3 * n;
                const int d = abs((q[0] & 63) - want[0]) + abs((q[1] & 63) - want[1]) + abs((q[2] & 63) - want[2]);
                if (d <= low) {
                    low = d;
                    best = n;
                }
            }
            g_flameTab[k][c] = (Bit8u)best;
        }
    }
    g_flameTabOk = true;
    return true;
}

void shade_row(Bit32u buf, int x, int y, int w, const Bit8u *tab) {
    if (y < 0 || y >= 200) return;
    const int a = std::max(x, (int)kMapLeft), b = std::min(x + w, (int)kMapRight);
    for (int i = a; i < b; i++) {
        Bit8u &p = MemBase[buf + (Bit32u)(y * 320 + i)];
        p = tab[p];
    }
}

// The game's frame (its display buffer, the VGA's palette) as a PPM
void write_frame(const std::string &name) {
    const Bit32u buf = r32(D(D_DISPLAYBUFFER));
    if (!ram(buf, 64000)) return;
    const char *dir = getenv("WCNET_DUMP_DIR");
    const std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + name + ".ppm";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (int i = 0; i < 64000; i++) {
        const RGBEntry &c = vga.dac.rgb[MemBase[buf + (Bit32u)i]];
        const Bit8u px[3] = { (Bit8u)(c.red << 2 | c.red >> 4), (Bit8u)(c.green << 2 | c.green >> 4), (Bit8u)(c.blue << 2 | c.blue >> 4) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    wclog(1, "Raptor: frame %u of the wave written to %s", g_frame, path.c_str());
}

int tiles_dead() {
    const Bit32u td = D(D_TILE_DEAD);
    if (!ram(td, 2 * kMapSize)) return 0;
    int n = 0;
    for (int i = 0; i < kMapSize; i++) n += host_readw(MemBase + td + 2 * i) != 0;
    return n;
}

void report(double now) {
    RapLocal l;
    rap_local(&l);
    wclog(2, "Raptor: %.1fs: %u frames, %s, sector %d wave %d, ship at %d %d, energy %d, money %u; damage told %u, taken %u, "
             "kills by the others %u; run %u; tiles destroyed %d; the others' ships drawn %u",
          now / 1000.0, g_frames, l.inWave ? "in a wave" : "not in a wave", l.game, l.wave, l.x, l.y, l.shield,
          r32(D(D_PLR_SCORE)), g_damageSent, g_damageTaken, g_remoteKills, rap_session_run(), l.inWave ? tiles_dead() : 0, rap_session_drawn());
    g_frames = g_damageSent = g_damageTaken = g_remoteKills = 0;
}

}  // namespace

void rap_program_loaded(const char *name, Bit16u psp) {
    if (strcmp(name, "RAP.EXE") != 0) return;
    if (g_found) forget("RAP.EXE started again");
    g_armed = true;
    g_found = false;
    g_nextScanMs = PIC_FullIndex() + 500;
    g_armedUntilMs = PIC_FullIndex() + 60000;
    wclog(1, "RAP.EXE started (PSP %04X): looking for Raptor's program", psp);
}

bool rap_found() { return g_found; }

void rap_tick() {
    if (g_found || g_armed) rap_session_tick();
    if (!g_armed) return;
    const double now = PIC_FullIndex();
    if (g_found) {
        if (now >= g_nextReportMs) {
            g_nextReportMs = now + 1000;
            if (!window_at(kAnchors[A_GAME_ENTRY], g_anchor[A_GAME_ENTRY])) {
                forget("the program is gone from memory: ended");
                return;
            }
            report(now);
        }
        return;
    }
    if (now < g_nextScanMs) return;
    if (now > g_armedUntilMs) {
        g_armed = false;
        wclog(1, "Raptor: no program of a build the hooks know appeared within a minute of RAP.EXE: not watched");
        return;
    }
    g_nextScanMs = now + 500;
    if (!scan()) return;
    g_found = true;
    g_nextReportMs = now + 1000;
    clear_enemies();
    set_watch(true);
    wclog(1, "Raptor: the program's code at %08x (Do_Game; image 0x11128 in the shareware), the ship at %08x, the enemies "
             "at %08x: watching %d places", g_places[P_GAME], g_data[D_PLAYERX], g_data[D_SHIPS], (int)P_COUNT);
}

bool rap_local(RapLocal *out) {
    memset(out, 0, sizeof(*out));
    if (!g_found) return false;
    out->inWave = g_inWave;
    out->game = (int)r32(D(D_CUR_GAME));
    out->wave = out->game >= 0 && out->game < 4 ? (int)r32(D(D_GAME_WAVE) + 4 * (Bit32u)out->game) : -1;
    out->diff = r32(D(D_CUR_DIFF));
    out->plrdiff = (int)r32(D(D_CURPLR_DIFF));
    out->frame = g_frame;
    out->x = (int)r32(D(D_PLAYERX));
    out->y = (int)r32(D(D_PLAYERY));
    const Bit32u pic = r32(D(D_PLAYERPIC)) + r32(D(D_G_FLASH));
    out->pic = pic < 16 ? r32(D(D_CURSHIP) + 4 * pic) : 0;
    out->drawn = r32(D(D_DRAW_PLAYER)) != 0;
    const Bit32u energy = r32(D(D_P_OBJS) + 4 * kEnergy);
    out->shield = energy ? (int)r32(energy + 8) : 0;
    out->bank = (int)r32(D(D_PLAYERPIC));
    return true;
}

bool rap_in_wave() { return g_found && g_inWave; }

// Our shots as RapState carries them: (kind << 28 | item), x | y << 16 [, a beam's top]
void rap_shots(std::vector<Bit32u> &out, int max) {
    out.clear();
    if (!g_found) return;
    const Bit32u end = D(D_LAST_SHOTS);
    Bit32u shot = r32(D(D_FIRST_SHOTS_NEXT));
    const int cx = (int)r32(D(D_PLAYER_CX)), cy = (int)r32(D(D_PLAYER_CY));
    (void)cx; (void)cy;
    for (int guard = 0; shot && shot != end && guard < 80 && (int)out.size() < 3 * max; guard++) {
        const Bit32u lib = r32(shot + kShotLibAt), pic = r32(shot + 8);
        const Bit32u beam = r32(lib + 0x70);
        const int x = (int)r32(shot + 0xC), y = (int)r32(shot + 0x10);
        if (beam == 1) {
            // a line: from the ship to its target (the move's x, y)
            out.push_back(1u << 28);
            out.push_back(((Bit32u)r32(shot + 0x14) & 0xFFFF) | ((Bit32u)r32(shot + 0x18) << 16));
        } else {
            Bit32u item = 0xFFFFFFFFu;
            for (int k = 0; k < 10; k++) {
                if (r32(lib + 4 + 4 * (Bit32u)k) == pic) {
                    item = r32(lib) + (Bit32u)k;
                    break;
                }
            }
            if (item != 0xFFFFFFFFu && pic) {
                out.push_back(((beam == 2 ? 2u : 0u) << 28) | (item & 0x0FFFFFFF));
                out.push_back(((Bit32u)x & 0xFFFF) | ((Bit32u)y << 16));
                if (beam == 2) out.push_back(r32(shot + 0x20));         // (the beam's top: the move's y2)
            }
        }
        shot = r32(shot + 4);
    }
}

void rap_set_wave(int game, int wave, Bit32u diff, int plrdiff) {
    if (!g_found || game < 0 || game > 3) return;
    w32(D(D_CUR_GAME), (Bit32u)game);
    w32(D(D_GAME_WAVE) + 4 * (Bit32u)game, (Bit32u)wave);
    w32(D(D_CUR_DIFF), diff);
    w32(D(D_CURPLR_DIFF), (Bit32u)plrdiff);
}

void rap_take_hits(const std::vector<std::pair<int, int> > &hits) {
    const double now = PIC_FullIndex();
    for (size_t i = 0; i < hits.size() && g_pending.size() < 512; i++) {
        Pending p = { hits[i].first, (Bit32s)hits[i].second, now };
        g_pending.push_back(p);
    }
}

void rap_blow_up() {
    if (!g_found || !g_inWave) return;
    const Bit32u energy = r32(D(D_P_OBJS) + 4 * kEnergy);
    if (energy && (Bit32s)r32(energy + 8) > 0) {
        w32(energy + 8, 0);
        wclog(1, "Raptor: our ship's energy to none (another pilot blown up: the wave is over for everybody)");
    }
}

bool rap_esc_down() {
    return g_found && r32(D(D_KEYBOARD_X) - 4 * 0x2D + 4 * 1) != 0;     // (keyboard [SC_ESC])
}

// (FLAME_Down at player_cx -/+ o_engine [playerpic] - 3/2, player_cy + 15: the ship's middle, PLAYERWIDTH and
// PLAYERHEIGHT 32)
void rap_draw_flames(int bank, int x, int y) {
    if (!g_found || !flame_tables()) return;
    const Bit32u buf = r32(D(D_DISPLAYBUFFER));
    if (!ram(buf, 64000)) return;
    const Bit32s spread = (Bit32s)r32(D(D_O_ENGINE) + 4 * (Bit32u)(bank < 0 ? 0 : bank > 7 ? 7 : bank));
    if (spread < -16 || spread > 16) return;
    const int cx = x + 16, cy = y + 16;
    const int h = (r32(D(D_GL_CNT)) & 1) ? 12 : 8;     // (FLAME_Down's height [gl_cnt % 2])
    const int left[2] = { cx - spread - 3, cx + spread - 2 };
    for (int e = 0; e < 2; e++) {
        for (int r = 0; r < h; r++) {
            const bool tip = r * 3 >= h * 2;
            shade_row(buf, left[e] + (tip ? 1 : 0), cy + 15 + r, tip ? 2 : 4, g_flameTab[r * 8 / h]);
        }
    }
}

// Put in the game's air shadows (SHADOW_Add's way, within its MAX_SHADOWS): SHADOW_DisplaySky draws it with the
// game's own, in the game's 3D perspective, under the enemies in the air.  A frame not in memory here is our own
// ship's current one (the game would load it from its archive in the middle of the frame).
void rap_add_shadow(Bit32u item, int x, int y) {
    if (!g_found) return;
    if (!item_mem(item, NULL)) item = r32(D(D_CURSHIP) + 4 * r32(D(D_PLAYERPIC)));
    const Bit32s n = (Bit32s)r32(D(D_NUM_SHADOWS));
    if (n < 0 || n >= g_maxShadows) return;
    const Bit32u at = D(D_SHADS) + 12 * (Bit32u)n;
    w32(at, item);
    w32(at + 4, (Bit32u)x);
    w32(at + 8, (Bit32u)y);
    w32(D(D_NUM_SHADOWS), (Bit32u)(n + 1));
}

void rap_draw_ship(Bit32u item, int x, int y, int seat) {
    if (!g_found) return;
    Bit32u size = 0;
    const Bit8u *pic = ship_bytes(item, &size);
    if (!pic) {
        // (a frame not in memory here: our own ship's current one, the same set)
        const Bit32u own = r32(D(D_CURSHIP) + 4 * r32(D(D_PLAYERPIC)));
        pic = ship_bytes(own, &size);
    }
    put_sprite(pic, size, x, y, seat > 0 && seat < kTints ? seat : 0);
}

void rap_draw_shots(const std::vector<Bit32u> &s, int shipX, int shipY) {
    for (size_t i = 0; i + 1 < s.size();) {
        const Bit32u kind = s[i] >> 28;
        const int x = (Bit16s)(s[i + 1] & 0xFFFF), y = (Bit16s)(s[i + 1] >> 16);
        if (kind == 1) {
            const int cx = shipX + 16, cy = shipY + 16;
            put_line(cx + 1, cy, x, y, 69);
            put_line(cx - 1, cy, x, y, 69);
            put_line(cx, cy, x, y, 64);
            i += 2;
            continue;
        }
        Bit32u size = 0;
        const Bit8u *pic = item_bytes(s[i] & 0x0FFFFFFF, &size);
        if (kind == 2) {
            if (i + 2 >= s.size()) break;
            const int top = (int)(Bit32s)s[i + 2];
            for (int yy = std::max(top, -64), n = 0; yy < y && n < 80; yy += 3, n++) put_sprite(pic, size, x, yy);
            i += 3;
        } else {
            put_sprite(pic, size, x, y);
            i += 2;
        }
    }
}

int rap_wave_hold_frames() { return 2; }

bool rap_script_item(const std::string &item, double t) {
    if (item.compare(0, 11, "!rapenergy=") == 0) {
        const Bit32u energy = g_found ? r32(D(D_P_OBJS) + 4 * kEnergy) : 0;
        if (energy) w32(energy + 8, (Bit32u)atoi(item.c_str() + 11));
        wclog(1, "key script %.1fs: Raptor: our energy to %d", t, energy ? atoi(item.c_str() + 11) : -1);
        return true;
    }
    if (item.compare(0, 9, "!rapshot=") == 0) {
        const size_t c = item.find(':');
        if (c != std::string::npos) g_shotsAt.push_back(std::make_pair((Bit32u)atoi(item.c_str() + 9), item.substr(c + 1)));
        return true;
    }
    if (item.compare(0, 8, "!rapgod=") == 0) {
        g_god = atoi(item.c_str() + 8) != 0;
        wclog(1, "key script %.1fs: Raptor: our energy kept full: %s", t, g_god ? "on" : "off");
        return true;
    }
    return false;
}

bool rap_fast_hook(Bit32u eip) {
    if (!g_found) return false;
    if (eip == g_places[P_WAIT]) {
        // (the frame's wait over: held while ahead of a pilot flying the same wave)
        if (g_inWave && rap_session_hold_frame(g_frame)) {
            reg_eip = g_waitLoop;
            CPU_IODelayRemoved += CPU_Cycles;
            CPU_Cycles = 0;
        }
        return true;
    }
    if (eip == g_places[P_IDLE]) {
        // (the launch held: the slice's cycles skipped until the others are there)
        if (rap_session_launch_go()) {
            g_launchReleased = true;
            reg_eip = eip + 2;
        } else {
            CPU_IODelayRemoved += CPU_Cycles;
            CPU_Cycles = 0;
        }
        return true;
    }
    return false;
}

void rap_check_hooks(Bit32u eip) {
    if (!g_found) return;
    if (rap_fast_hook(eip)) return;
    if (eip == g_places[P_FRAME]) {
        g_frame++;
        g_frames++;
        for (size_t i = 0; i < g_shotsAt.size();) {
            if (g_shotsAt[i].first == g_frame && g_inWave) {
                write_frame(g_shotsAt[i].second);
                g_shotsAt.erase(g_shotsAt.begin() + i);
            } else {
                i++;
            }
        }
        if (g_god) {
            const Bit32u energy = r32(D(D_P_OBJS) + 4 * kEnergy);
            if (energy && (Bit32s)r32(energy + 8) > 0) w32(energy + 8, 100);
        }
        if (rap_session_run()) {
            own_tint(rap_session_seat());
            enemies_frame();
            RapLocal l;
            rap_local(&l);
            static Bit32u deadSentRun = 0;
            if (l.shield <= 0 && deadSentRun != rap_session_run()) {
                deadSentRun = rap_session_run();
                rap_session_dead();
            }
        }
    } else if (eip == g_places[P_SHOTS]) {
        const Bit32u th = D(D_TILE_HITS);
        if (rap_session_run() && ram(th, 2 * kMapSize)) memcpy(g_tileBefore, MemBase + th, sizeof(g_tileBefore));
    } else if (eip == g_places[P_SHOTS_AFTER]) {
        if (rap_session_run()) rap_session_shadows();       // (before the ship's own: SHADOW_DisplaySky draws them all)
        const Bit32u th = D(D_TILE_HITS);
        if (rap_session_run() && ram(th, 2 * kMapSize)) {
            for (int spot = 0; spot < kMapSize; spot++) {
                const Bit16s now = (Bit16s)host_readw(MemBase + th + 2 * spot);
                if (now < g_tileBefore[spot]) {
                    rap_session_damage(spot | kTileFlag, g_tileBefore[spot] - now);
                    g_damageSent++;
                }
            }
        }
    } else if (eip == g_places[P_TILE_MONEY]) {
        const int spot = (int)r32(reg_ebp - 0x14);
        if (spot >= 0 && spot < kMapSize && g_tileRemoteKill[spot]) {
            g_tileRemoteKill[spot] = false;
            if (rap_session_run()) reg_eip = eip + 6;       // (the others' kill: no money here)
        }
    } else if (eip == g_places[P_DRAW]) {
        if (rap_session_run()) rap_session_draw();
    } else if (eip == g_places[P_ADD]) {
        const Bit32u cur = reg_eax, base = r32(D(D_CSPRITE));
        g_pendingAdd = cur >= base && (cur - base) % kCspriteSize == 0 ? (int)((cur - base) / kCspriteSize) : -1;
    } else if (eip == g_places[P_ADD_AFTER]) {
        if (g_pendingAdd >= 0) {
            // (ENEMY_Add put it last in the list: last_enemy.prev)
            const Bit32u ship = r32(D(D_LAST_ENEMY));
            const int s = slot_of(ship);
            if (s >= 0) {
                g_slotId[s] = g_pendingAdd;
                g_lastHits[s] = (Bit32s)r32(ship + kHitsAt);
                g_slotKnown[s] = true;
                g_slotRemoteKill[s] = false;
            }
            g_pendingAdd = -1;
        }
    } else if (eip == g_places[P_KILL]) {
        // (ENEMY_Think's sprite in its frame's [ebp - 4])
        const Bit32u ship = r32(reg_ebp - 4);
        const int s = slot_of(ship);
        if (s >= 0 && g_slotKnown[s]) {
            const Bit32s hits = (Bit32s)r32(ship + kHitsAt);
            const Bit32s crash = g_lastHits[s] - hits;      // (our ship crashing into it, in ENEMY_Think)
            if (crash > 0 && g_slotId[s] >= 0 && rap_session_run()) {
                rap_session_damage(g_slotId[s], crash);
                g_damageSent++;
            }
            if (g_slotRemoteKill[s] && crash <= 0 && rap_session_run()) reg_eip = g_killSkip;   // (the others' kill)
            g_slotKnown[s] = false;
        }
    } else if (eip == g_places[P_GAME]) {
        g_inWave = true;
        g_frame = 0;
        clear_enemies();
        rap_session_wave_start();
    } else if (eip == g_places[P_GAME_END]) {
        g_inWave = false;
        own_restore();
        rap_session_wave_end();
    } else if (eip == g_places[P_LAUNCH]) {
        if (g_launchReleased) {
            g_launchReleased = false;       // (back from the hold: on)
            return;
        }
        if (rap_session_launch_arrive()) reg_eip = g_places[P_IDLE];
    }
}

}  // namespace wc
