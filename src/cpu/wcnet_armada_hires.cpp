/*
 *  Wing Commander Armada's flight at 640x400 (wcnet_armada_hires.h, docs/armada.md "Twice the resolution").
 *
 *  The flight draws into a 320x200 back buffer (a bitmap record: segment, row table, clip x0 y0 x1 y1), which JEMM's
 *  32-bit helper then copies to mode 13h.  Each frame the frame function (seg2B4B in ARMADA.EXE) runs a section:
 *
 *      top+00  mov al, [di + clear colour] ... lcall fill      the buffer cleared
 *      top+11  mov word [list + 2], 0                          the display list emptied
 *      top+25  lcall setup (push si: the view)                 every point projected through the view, the list built
 *      top+3B  lcall sort                                      by depth
 *      top+51  lcall draw                                      each entry's own draw
 *      top+57  ...                                             the optional overlay, then the cockpit
 *      top+81  (after the cockpit)                             then JEMM copies the buffer to the screen
 *
 *  With the picture on, the section runs four times a frame, each time with the view's centre and scale made a
 *  quarter's of a picture twice as large (centre 2c - the quarter's corner, scale 2s, the clip rectangle likewise),
 *  and each time the buffer is kept as that quarter of a 640x400 scene.  Then the section runs a fifth time exactly as
 *  the game runs it (its view, its state: everything the frame leaves behind is the game's own), its scene is kept, and
 *  the buffer is filled with a colour the cockpit never uses (the marker) for the cockpit to be drawn over as usual:
 *  where the buffer still holds the marker after it, the 640x400 scene shows; anywhere else the game's pixel, doubled.
 *  The buffer then gets the game's scene back under the cockpit, so the screen gets exactly the game's own frame.
 *
 *  What the section changes besides the picture is put back before each further pass: the star field's stars (a
 *  locked resource of 12-byte positions, moved by the camera's motion each time they are drawn) and its last camera
 *  position.  The far-off sprites (the sun, the planets) are drawn by the game at a fixed size; in the quarters they are
 *  drawn here instead, each pixel 2x2.  The draw range (DS far clip, ~9.9 km) is made longer for the quarters, so ships
 *  too small to show at 320 come in at 640 (WCARMHIRESFAR, default 16 times).
 *
 *  The strategy map is drawn one level out the same way: its panel four times a frame, the scroll shifted to each
 *  quarter of a view twice the size, the game's own icons at their own pixel size ("the strategy map" below).
 *
 *  WCARMHIRES=1 at the launch, wc_web_arm_hires while flying, the ` key flips it.  Until it is first wanted none of
 *  this is watched.  The four programs (Armada and its Proving Grounds, floppy and CD builds) are each checked by
 *  their bytes at every place; one that differs keeps the game's own picture.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <math.h>
#include <set>
#include <algorithm>
#include "wcnet_armada_hires.h"
#include "wcnet_armada_art.h"
#include "wcnet_game.h"
#include "wcnet_log.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "pic.h"
#include "vga.h"
#include "render.h"
#include "cpu.h"
#include "keyboard.h"

namespace wc {

namespace {

struct Build {
    const char *exe;
    Bit16u frameSeg, top;       // the scene section's first instruction (mov al, [di + clear colour])
    Bit16u rec;                 // DS: the near pointer to the back buffer's bitmap record
    Bit16u view;                // DS: the view every point is projected with (centre x, y words; scale x, y dwords)
    Bit16u farClip;             // DS: the draw range (dword)
    Bit16u starSeg, starEntry, starLockA, starLockB;    // the star field's draw: its entry, its two locks' returns
    Bit16u bgSeg, bgCall;       // the far-off sprites' draw: the shape routine's call (lcall, 5 bytes)
    Bit16u waitSeg[2], waitCall[2];     // the frame waits' loops: the timer's call at their heads (0: none)
    Bit16u turnSeg, turnEntry;  // the library's shape turned and scaled (x, y, shapes far, index, degrees, scale, flags)
    Bit16u boomSeg, boomEntry;  // an explosion piece's draw (its frame counted on at +32 each time it is drawn)
    Bit16u listSeg, listCall;   // the display list's call of each entry's draw (a test aid's)
    Bit16u lineSeg, lineEntry;  // the library's line (bitmap record, x0, y0, x1, y1, colour)
    Bit16u monoSeg, monoEntry;  // its shape in one colour (bitmap record, x, y, shapes far, index, colour): the reticle
};
const Build kBuilds[] = {
    { "ARMADA.EXE",   0x2B4B, 0x061A, 0x621A, 0x83BA, 0x8646, 0x29CA, 0x054A, 0x07C5, 0x0D70, 0x2943, 0x0864,
      { 0x3FD8, 0 }, { 0x0443, 0 }, 0x6493, 0x0ECC, 0x2230, 0x008A, 0x36D4, 0x003F, 0x5F32, 0x0006, 0x5F69, 0x0008 },
    { "ARMADACD.EXE", 0x2B4E, 0x061A, 0x62A6, 0x843A, 0x86C6, 0x29CD, 0x054A, 0x07C5, 0x0D70, 0x2946, 0x0864,
      { 0x3FDB, 0 }, { 0x0443, 0 }, 0x6496, 0x0ECC, 0x2233, 0x008A, 0x36D7, 0x003F, 0x5F35, 0x0006, 0x5F6C, 0x0008 },
    { "PG.EXE",       0x205C, 0x0632, 0x4E78, 0x714E, 0x73DA, 0x1DDF, 0x0540, 0x07BB, 0x0D66, 0x1D57, 0x086A,
      { 0x2C03, 0x3DA0 }, { 0x0CBE, 0x029D }, 0x6100, 0x0ECE, 0x17BE, 0x008F, 0x2FC7, 0x0049, 0x5B9F, 0x0008, 0x5BD6, 0x000A },
    { "PGCD.EXE",     0x2060, 0x0632, 0x4F88, 0x725E, 0x74EA, 0x1DE3, 0x0540, 0x07BB, 0x0D66, 0x1D5B, 0x086A,
      { 0x2C23, 0x3DBF }, { 0x0CBC, 0x02AB }, 0x6120, 0x0ECC, 0x17C2, 0x008F, 0x2FE7, 0x0047, 0x5BBF, 0x0006, 0x5BF6, 0x0008 },
};
enum { kCleared = 0x11, kSetup = 0x25, kDrawn = 0x57, kCockpit = 0x81 };   // from the section's top

const int W = 320, H = 200, FW = 640, FH = 400;
Bit8u kMarker = 0xFF;           // the fifth pass's clear: a colour the cockpit does not use (WCARMHIRESMARKER, a test aid)

const Build *g_b;
Bit16u g_load;
bool g_codeOk;

int g_level = -1;               // 0 off, 1 the space alone, 2 all (the cockpit's lines and reticle too); -1 unread
bool g_never, g_used;
int g_farTimes = 16;

int g_pass = -1;                // 0..3 the quarters, 4 the marker's clear, 5 the cockpit over it; -1 none
bool g_jumped;                  // (the browser's own call for a place a hook jumped to: not a new arrival)
bool g_set;                     // the view is a quarter's now
PhysPt g_master;                // DS:SI+8, the view the frame was given
Bit8u g_viewKeep[12], g_clipKeep[8];
Bit32u g_farKeep;
bool g_empty[4];

std::vector<Bit8u> g_fine(FW * FH), g_out(FW * FH), g_low(W * H);   // g_low: the last frame put on the screen
std::vector<Bit8u> g_own(W * H);       // the game's own pass's scene, kept for under the cockpit
std::vector<Bit8u> g_over(FW * FH), g_overSet(FW * FH);     // the cockpit's stage's fine drawing (level 1: the space's lines)
bool g_overAny;
double g_lastCompose = -1e9;
bool g_override;
unsigned g_frames;

struct StarKeep {
    PhysPt obj;
    Bit8u head[18];             // the field's count of stars (+08, its density: the frame rate's), last camera place (+0E)
    std::vector<Bit8u> stars;   // its stars, 12 bytes each, as they were before the frame
};
std::vector<StarKeep> g_starKeep;

// While the picture is 640x400 the emulated CPU is faster (the four quarters' work; the game holds its own frame time
// with its own wait), and that wait is skipped ahead a little at a time instead of spun: never past the game's own
// timer unit (1/6026 s), never past the next event
int g_cyclesWant = -1;          // WCARMHIRESCYCLES; default three times the launch's
Bit32s g_cyclesBase, g_cyclesBoost;
const double kIdleStepMs = 0.05;

// !armside: what passes 1 and 2 changed differently from pass 0, the whole memory compared (a test aid)
int g_side;
std::vector<Bit8u> g_sideP, g_sideA[3];

inline PhysPt ds_at(Bit16u off) { return SegPhys(ds) + off; }
inline Bit16u rd16l(PhysPt a) { return mem_readw(a); }

int level() {
    if (g_level < 0) {
        // WCARMHIRES: 1, 2 or all: all; space: the space alone; off: never (the key inert); else off
        const char *env = getenv("WCARMHIRES");
        g_never = env && !strcmp(env, "off");
        g_level = g_never || !env || !env[0] || env[0] == '0' ? 0 : !strcmp(env, "space") ? 1 : 2;
        const char *marker = getenv("WCARMHIRESMARKER");
        if (marker && marker[0]) kMarker = (Bit8u)strtol(marker, NULL, 16);
        const char *far = getenv("WCARMHIRESFAR");
        if (far && far[0]) g_farTimes = atoi(far) < 1 ? 1 : atoi(far) > 64 ? 64 : atoi(far);
    }
    return g_never ? 0 : g_level;
}

bool wanted() { return level() > 0; }

void now_wanted() {
    if (!wanted() || g_used) return;
    g_used = true;
    hooks_game_changed();       // (our places watched from the next instruction)
}

// The game's linear memory, a page at a time through the TLB (the back buffer and the stars are JEMM's pages)
void lin_read(PhysPt lin, Bit8u *dst, Bitu n) {
    while (n) {
        const Bitu chunk = n < 4096 - (lin & 0xFFF) ? n : 4096 - (lin & 0xFFF);
        const HostPt t = get_tlb_read(lin);
        if (t) {
            memcpy(dst, t + lin, chunk);
        } else {
            for (Bitu k = 0; k < chunk; k++) if (mem_readb_checked((PhysPt)(lin + k), &dst[k])) dst[k] = 0;
        }
        lin += chunk; dst += chunk; n -= chunk;
    }
}

void lin_write(PhysPt lin, const Bit8u *src, Bitu n) {
    while (n) {
        const Bitu chunk = n < 4096 - (lin & 0xFFF) ? n : 4096 - (lin & 0xFFF);
        const HostPt t = get_tlb_write(lin);
        if (t) {
            memcpy(t + lin, src, chunk);
        } else {
            for (Bitu k = 0; k < chunk; k++) mem_writeb_checked((PhysPt)(lin + k), src[k]);
        }
        lin += chunk; src += chunk; n -= chunk;
    }
}

// The back buffer's bitmap record: its rows' linear addresses and its clip rectangle
struct Bitmap {
    PhysPt rec;
    PhysPt row[H];
    int x0, y0, x1, y1;
    bool ok;
};

Bitmap bitmap() {
    Bitmap b;
    b.ok = false;
    const Bit16u recOff = rd16l(ds_at(g_b->rec));
    b.rec = ds_at(recOff);
    const Bit16u seg = rd16l(b.rec), rows = rd16l(b.rec + 2);
    b.x0 = (Bit16s)rd16l(b.rec + 4); b.y0 = (Bit16s)rd16l(b.rec + 6);
    b.x1 = (Bit16s)rd16l(b.rec + 8); b.y1 = (Bit16s)rd16l(b.rec + 10);
    if (!seg) return b;
    for (int y = 0; y < H; y++) b.row[y] = ((PhysPt)seg << 4) + rd16l(ds_at((Bit16u)(rows + 2 * y)));
    b.ok = b.x0 >= 0 && b.y0 >= 0 && b.x1 < W && b.y1 < H && b.x0 <= b.x1 && b.y0 <= b.y1;
    return b;
}

bool code_here() {
    const PhysPt f = ((PhysPt)(Bit16u)(g_load + g_b->frameSeg) << 4) + g_b->top;
    if (mem_readb(f) != 0x8A || mem_readb(f + 1) != 0x85 || mem_readb(f + 4) != 0x50 || mem_readw(f + 7) != g_b->rec ||
        mem_readb(f + 9) != 0x9A || mem_readw(f + kCleared) != 0x06C7 || mem_readb(f + kSetup) != 0x9A ||
        mem_readb(f + kDrawn) != 0x9A || mem_readb(f + kCockpit) != 0x9A || mem_readb(f + kCockpit - 1) != 0x59) {
        return false;
    }
    const PhysPt s = (PhysPt)(Bit16u)(g_load + g_b->starSeg) << 4;
    if (mem_readd(s + g_b->starEntry) != 0x81EC8B55 || mem_readd(s + g_b->starLockA) != 0x89EA5689 ||
        mem_readd(s + g_b->starLockB) != 0x89EA5689) {
        return false;
    }
    const PhysPt bg = ((PhysPt)(Bit16u)(g_load + g_b->bgSeg) << 4) + g_b->bgCall;
    if (mem_readb(bg) != 0x9A || mem_readw(bg + 5) != 0xC483 || mem_readb(bg + 7) != 0x0C) return false;
    // the turned-and-scaled shape: 55 8B EC 56 57 1E 81 7E 14 (the scale compared with 100h first)
    const PhysPt turn = ((PhysPt)(Bit16u)(g_load + g_b->turnSeg) << 4) + g_b->turnEntry;
    if (mem_readd(turn) != 0x56EC8B55 || mem_readw(turn + 6) != 0x7E81 || mem_readb(turn + 8) != 0x14) return false;
    // an explosion piece's draw: 55 8B EC C4 5E 06 26 FF 47 32 (inc word es:[bx+32])
    const PhysPt boom = ((PhysPt)(Bit16u)(g_load + g_b->boomSeg) << 4) + g_b->boomEntry;
    if (mem_readd(boom + 3) != 0x26065EC4 || mem_readw(boom + 7) != 0x47FF || mem_readb(boom + 9) != 0x32) return false;
    // the display list's call of a draw: FF 1F (call far [bx])
    const PhysPt list = ((PhysPt)(Bit16u)(g_load + g_b->listSeg) << 4) + g_b->listCall;
    return mem_readw(list) == 0x1FFF;
}

// ----- the view, per quarter ---------------------------------------------------------------------------------------

void view_write(PhysPt at, int cx, int cy, Bit32s sx, Bit32s sy) {
    mem_writew(at, (Bit16u)cx);
    mem_writew(at + 2, (Bit16u)cy);
    mem_writed(at + 4, (Bit32u)sx);
    mem_writed(at + 8, (Bit32u)sy);
}

void view_back() {
    if (!g_set) return;
    g_set = false;
    for (int k = 0; k < 12; k++) {
        mem_writeb(g_master + k, g_viewKeep[k]);
        mem_writeb(ds_at(g_b->view) + k, g_viewKeep[k]);
    }
    const Bit16u recOff = rd16l(ds_at(g_b->rec));
    for (int k = 0; k < 8; k++) mem_writeb(ds_at(recOff) + 4 + k, g_clipKeep[k]);
    mem_writed(ds_at(g_b->farClip), g_farKeep);
}

// The quarter's view at the setup call (SI: the view; its centre and scale at +8): false if the quarter is empty
bool view_quarter(int q) {
    g_master = ds_at((Bit16u)(reg_si + 8));
    for (int k = 0; k < 12; k++) g_viewKeep[k] = mem_readb(g_master + k);
    const Bitmap b = bitmap();
    const Bit16u recOff = rd16l(ds_at(g_b->rec));
    for (int k = 0; k < 8; k++) g_clipKeep[k] = mem_readb(ds_at(recOff) + 4 + k);
    g_farKeep = mem_readd(ds_at(g_b->farClip));
    const int cx = (Bit16s)(g_viewKeep[0] | g_viewKeep[1] << 8), cy = (Bit16s)(g_viewKeep[2] | g_viewKeep[3] << 8);
    Bit32s sx, sy;
    memcpy(&sx, g_viewKeep + 4, 4);
    memcpy(&sy, g_viewKeep + 8, 4);
    const int qx = (q & 1) * W, qy = (q >> 1) * H;
    int x0 = 2 * b.x0 - qx, x1 = 2 * b.x1 + 1 - qx, y0 = 2 * b.y0 - qy, y1 = 2 * b.y1 + 1 - qy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (y1 > H - 1) y1 = H - 1;
    if (x0 > x1 || y0 > y1) return false;
    g_set = true;
    view_write(g_master, 2 * cx - qx, 2 * cy - qy, 2 * sx, 2 * sy);
    view_write(ds_at(g_b->view), 2 * cx - qx, 2 * cy - qy, 2 * sx, 2 * sy);
    mem_writew(ds_at(recOff) + 4, (Bit16u)x0);
    mem_writew(ds_at(recOff) + 6, (Bit16u)y0);
    mem_writew(ds_at(recOff) + 8, (Bit16u)x1);
    mem_writew(ds_at(recOff) + 10, (Bit16u)y1);
    const Bit64s far = (Bit64s)(Bit32s)g_farKeep * g_farTimes;
    mem_writed(ds_at(g_b->farClip), (Bit32u)(far > 0x3FFFFF ? 0x3FFFFF : far < 1 ? g_farKeep : far));   // (the game shifts it left by 9)
    return true;
}

// ----- the pictures ------------------------------------------------------------------------------------------------

void keep_quarter(int q) {
    const Bitmap b = bitmap();
    if (!b.ok && !b.row[0]) return;
    Bit8u *o = &g_fine[(q >> 1) * H * FW + (q & 1) * W];
    for (int y = 0; y < H; y++) lin_read(b.row[y], o + y * FW, W);
}

void fill_quarter(int q, Bit8u c) {
    Bit8u *o = &g_fine[(q >> 1) * H * FW + (q & 1) * W];
    for (int y = 0; y < H; y++) memset(o + y * FW, c, W);
}

void marker_clear() {
    const Bitmap b = bitmap();
    std::vector<Bit8u> line(W, kMarker);
    for (int y = 0; y < H; y++) lin_write(b.row[y], &line[0], W);
}

// The window's edge smoothed (level 2; Privateer's way, smooth_edge): the window (where the marker was left) blurred
// 3x3 and sampled at each fine pixel; where that side is not the game pixel's own, the fine pixel is the scene's there,
// or a neighbouring cockpit pixel's.  The edge's staircase becomes one-pixel steps of the 640x400 at any slope.
void smooth_edge(const std::vector<Bit8u> &win) {
    static int blur[W * H];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int sum = 0;
            for (int j = -1; j <= 1; j++) {
                const int yy = y + j < 0 ? 0 : y + j >= H ? H - 1 : y + j;
                for (int i = -1; i <= 1; i++) {
                    const int xx = x + i < 0 ? 0 : x + i >= W ? W - 1 : x + i;
                    sum += win[yy * W + xx] * (i ? 1 : 2) * (j ? 1 : 2);
                }
            }
            blur[y * W + x] = sum;
        }
    }
    for (int Y = 0; Y < FH; Y++) {
        int vy = 10 * Y - 5;
        if (vy < 0) vy = 0;
        int y0 = vy / 20, fy = vy % 20;
        if (y0 >= H - 1) { y0 = H - 1; fy = 0; }
        const int y1 = y0 + 1 < H ? y0 + 1 : y0;
        for (int X = 0; X < FW; X++) {
            int vx = 10 * X - 5;
            if (vx < 0) vx = 0;
            int x0 = vx / 20, fx = vx % 20;
            if (x0 >= W - 1) { x0 = W - 1; fx = 0; }
            const int x1 = x0 + 1 < W ? x0 + 1 : x0;
            const int a = blur[y0 * W + x0], b2 = blur[y0 * W + x1], c = blur[y1 * W + x0], d = blur[y1 * W + x1];
            if ((a | b2 | c | d) == 0 || (a & b2 & c & d) == 16) continue;      // (not near the edge)
            const int v = (a * (20 - fx) + b2 * fx) * (20 - fy) + (c * (20 - fx) + d * fx) * fy;
            const Bit8u side = v > 8 * 20 * 20 ? 1 : 0;
            const int x = X >> 1, y = Y >> 1;
            if (win[y * W + x] == side) continue;
            if (side) {
                g_out[Y * FW + X] = g_fine[Y * FW + X];
                continue;
            }
            static const int kDx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 }, kDy[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
            for (int k = 0; k < 8; k++) {
                const int nx = x + kDx[k], ny = y + kDy[k];
                if (nx >= 0 && ny >= 0 && nx < W && ny < H && !win[ny * W + nx]) {
                    g_out[Y * FW + X] = g_low[ny * W + nx];
                    break;
                }
            }
        }
    }
}

// After the cockpit: the 640x400 shown, and the game's own buffer as the game made it (its scene under the cockpit)
void compose() {
    const Bitmap b = bitmap();
    std::vector<Bit8u> line(W);
    static std::vector<Bit8u> win(W * H);
    for (int y = 0; y < H; y++) {
        lin_read(b.row[y], &line[0], W);
        Bit8u *own = &g_low[y * W];
        memcpy(own, &g_own[y * W], W);
        Bit8u *o0 = &g_out[2 * y * FW], *o1 = o0 + FW;
        const Bit8u *f0 = &g_fine[2 * y * FW], *f1 = f0 + FW;
        bool back = false;
        for (int x = 0; x < W; x++) {
            const Bit8u l = line[x];
            win[y * W + x] = l == kMarker;
            if (l == kMarker) {
                o0[2 * x] = f0[2 * x]; o0[2 * x + 1] = f0[2 * x + 1];
                o1[2 * x] = f1[2 * x]; o1[2 * x + 1] = f1[2 * x + 1];
                back = true;            // (own[x] is the game's scene there)
            } else {
                o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = l;
                own[x] = l;
            }
        }
        if (back) lin_write(b.row[y], own, W);
    }
    if (level() == 2) smooth_edge(win);
    if (g_overAny) {
        int n = 0;
        for (int i = 0; i < FW * FH; i++) if (g_overSet[i]) { g_out[i] = g_over[i]; n++; }
        static int told = 0;
        if (getenv("WCARMHIRESDEBUG") && told++ < 5) wclog(1, "Armada overlay: %d fine pixels", n);
    }
    g_lastCompose = PIC_FullIndex();
    if (getenv("WCARMHIRESCOLOURS") && (g_frames % 50) == 0) {
        // (a test aid: the colours the cockpit's stage drew this frame, and which it never used so far)
        static bool used[256];
        unsigned marker = 0;
        for (int i = 0; i < W * H; i++) {
            if (g_low[i] == kMarker) marker++;
        }
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                // (g_low now holds the fine scene where the marker was: count the cockpit's own pixels only)
                const Bit8u c = g_out[2 * y * FW + 2 * x];
                if (c == g_out[2 * y * FW + 2 * x + 1] && c == g_out[(2 * y + 1) * FW + 2 * x] &&
                    c == g_out[(2 * y + 1) * FW + 2 * x + 1]) used[c] = true;
            }
        }
        std::string unused;
        for (int c = 0; c < 256; c++) {
            if (!used[c]) { char b[8]; snprintf(b, sizeof(b), " %02X", c); unused += b; }
        }
        wclog(1, "Armada colours: never in a 2x2 block so far:%s", unused.c_str());
    }
    if (++g_frames == 1) wclog(1, "Armada: the first frame at 640x400 (far clip %u, x%d for the quarters)", (unsigned)g_farKeep, g_farTimes);
}

// ----- the star field: the same stars for every quarter ------------------------------------------------------------

StarKeep *star_keep(PhysPt obj, bool make) {
    for (size_t i = 0; i < g_starKeep.size(); i++) if (g_starKeep[i].obj == obj) return &g_starKeep[i];
    if (!make || g_starKeep.size() >= 8) return NULL;
    g_starKeep.push_back(StarKeep());
    g_starKeep.back().obj = obj;
    return &g_starKeep.back();
}

void star_entry() {
    // (a far call: [sp] the return, [sp+4] the field's offset, [sp+6] its segment)
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const PhysPt obj = ((PhysPt)mem_readw(sp + 6) << 4) + mem_readw(sp + 4);
    if (g_pass == 0) {
        StarKeep *k = star_keep(obj, true);
        if (k) for (int i = 0; i < 18; i++) k->head[i] = mem_readb(obj + 8 + i);
    } else {
        const StarKeep *k = star_keep(obj, false);
        if (k) for (int i = 0; i < 18; i++) mem_writeb(obj + 8 + i, k->head[i]);
    }
}

void star_locked() {
    // the stars' resource locked: DX:AX; the field at [bp+6], its count of stars at +8
    const PhysPt fp = SegPhys(ss) + (Bit16u)(reg_bp + 6);
    const PhysPt obj = ((PhysPt)mem_readw(fp + 2) << 4) + mem_readw(fp);
    const PhysPt stars = ((PhysPt)reg_dx << 4) + reg_ax;
    const Bitu n = (Bitu)(mem_readw(obj + 8) & 0x0FFF) * 12;
    if (g_pass == 0) {
        StarKeep *k = star_keep(obj, true);
        if (k) {
            k->stars.resize(n);
            if (n) lin_read(stars, &k->stars[0], n);
        }
    } else {
        const StarKeep *k = star_keep(obj, false);
        if (k && k->stars.size() == n && n) lin_write(stars, &k->stars[0], n);
    }
}

// ----- the far-off sprites at twice the size ------------------------------------------------------------------------

// The shape routine's call (bitmap record, x, y, shape file far, index): drawn here, each pixel 2x2 about the same
// point, and the call stepped over (its arguments are the caller's to pop)
void bg_sprite() {
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const PhysPt rec = ds_at(mem_readw(sp));
    const int X = (Bit16s)mem_readw(sp + 2), Y = (Bit16s)mem_readw(sp + 4);
    const PhysPt file = ((PhysPt)mem_readw(sp + 8) << 4) + mem_readw(sp + 6);
    const int index = (Bit16s)mem_readw(sp + 10);
    reg_eip = g_b->bgCall + 5;
    if (index < 0 || index > 4095) return;
    const Bit32u table = mem_readd(file + 4) & 0xFFFFFF;
    if ((Bit32u)(4 * (index + 1)) >= table) return;
    PhysPt p = file + (mem_readd(file + 4 + 4 * index) & 0xFFFFF) + 8;
    const Bit16u seg = mem_readw(rec), rows = mem_readw(rec + 2);
    const int x0 = (Bit16s)mem_readw(rec + 4), y0 = (Bit16s)mem_readw(rec + 6);
    const int x1 = (Bit16s)mem_readw(rec + 8), y1 = (Bit16s)mem_readw(rec + 10);
    PhysPt rowAt[H];
    for (int y = 0; y < H; y++) rowAt[y] = ((PhysPt)seg << 4) + mem_readw(ds_at((Bit16u)(rows + 2 * y)));
    Bit8u px[2];
    for (int runs = 0; runs < 4096; runs++) {
        const Bit16u head = mem_readw(p);
        if (!head) break;
        int rx = (Bit16s)mem_readw(p + 2);
        const int ry = (Bit16s)mem_readw(p + 4);
        p += 6;
        int n = head >> 1;
        const int fy = Y + 2 * ry;
        auto put = [&](int x, Bit8u c) {
            const int fx = X + 2 * x;
            px[0] = px[1] = c;
            for (int dy = 0; dy < 2; dy++) {
                const int yy = fy + dy;
                if (yy < y0 || yy > y1) continue;
                const int a = fx < x0 ? x0 : fx, e = fx + 1 > x1 ? x1 : fx + 1;
                if (a <= e) lin_write(rowAt[yy] + a, px, (Bitu)(e - a + 1));
            }
        };
        if (!(head & 1)) {
            for (; n > 0; n--, rx++, p++) put(rx, mem_readb(p));
            continue;
        }
        while (n > 0) {
            const Bit8u sub = mem_readb(p++);
            int run = sub >> 1;
            if (!run) return;
            n -= run;
            if (sub & 1) {
                const Bit8u c = mem_readb(p++);
                for (; run; run--, rx++) put(rx, c);
            } else {
                for (; run; run--, rx++, p++) put(rx, mem_readb(p));
            }
        }
    }
}

// ----- what a draw changes in its own object: as before the frame for every pass ------------------------------------

struct Keep { PhysPt at; int n; Bit8u bytes[8]; };
std::vector<Keep> g_keep;       // this frame's (cleared at its first pass)

void keep_or_restore(PhysPt at, int n) {
    for (size_t i = 0; i < g_keep.size(); i++) {
        if (g_keep[i].at == at && g_keep[i].n == n) {
            for (int k = 0; k < n; k++) mem_writeb(at + k, g_keep[i].bytes[k]);
            return;
        }
    }
    if (g_keep.size() >= 1024) return;
    Keep k;
    k.at = at;
    k.n = n;
    for (int i = 0; i < n; i++) k.bytes[i] = mem_readb(at + i);
    g_keep.push_back(k);
}

// An explosion piece's draw counts its frame on (+32) each time it is drawn: the same frame for every pass
void boom_entry() {
    const PhysPt sp = SegPhys(ss) + reg_sp;
    keep_or_restore(((PhysPt)mem_readw(sp + 6) << 4) + mem_readw(sp + 4) + 0x32, 2);
}

// ----- the library's shape turned and scaled, in the quarters ------------------------------------------------------

struct Shape { int w, h, left, top; std::vector<Bit16s> px; };

bool shape_at(PhysPt file, int index, Shape &s) {
    if (index < 0 || index > 4095) return false;
    const Bit32u table = mem_readd(file + 4) & 0xFFFFFF;
    if ((Bit32u)(4 * (index + 1)) >= table) return false;
    const PhysPt at = file + (mem_readd(file + 4 + 4 * index) & 0xFFFFF);
    const int x2 = (Bit16s)mem_readw(at), x1 = (Bit16s)mem_readw(at + 2);
    const int y1 = (Bit16s)mem_readw(at + 4), y2 = (Bit16s)mem_readw(at + 6);
    s.w = x1 + x2 + 1;
    s.h = y1 + y2 + 1;
    s.left = x1;
    s.top = y1;
    if (s.w <= 0 || s.h <= 0 || s.w > 1024 || s.h > 1024) return false;
    s.px.assign(s.w * s.h, (Bit16s)-1);
    PhysPt p = at + 8;
    for (int runs = 0; runs < 8192; runs++) {
        const Bit16u head = mem_readw(p);
        if (!head) break;
        int x = (Bit16s)mem_readw(p + 2) + x1;
        const int y = (Bit16s)mem_readw(p + 4) + y1;
        p += 6;
        int n = head >> 1;
        Bit16s *row = y >= 0 && y < s.h ? &s.px[y * s.w] : NULL;
        if (!(head & 1)) {
            for (; n > 0; n--, x++, p++) if (row && x >= 0 && x < s.w) row[x] = mem_readb(p);
            continue;
        }
        while (n > 0) {
            const Bit8u sub = mem_readb(p++);
            int run = sub >> 1;
            if (!run) return true;
            n -= run;
            if (sub & 1) {
                const Bit8u c = mem_readb(p++);
                for (; run; run--, x++) if (row && x >= 0 && x < s.w) row[x] = c;
            } else {
                for (; run; run--, x++, p++) if (row && x >= 0 && x < s.w) row[x] = mem_readb(p);
            }
        }
    }
    return true;
}

// (bitmap record, x, y, shapes far, index, degrees, scale 8.8, flags: 10h mirrored left-right, 20h upside down):
// mirrored first, then turned clockwise, in the game's pixels, sampled at each pixel's middle; clipped to the record
void turn_draw(PhysPt rec, int x, int y, PhysPt file, int index, int degrees, int scale, int flags) {
    Shape s;
    if (scale <= 0 || scale > 64 * 256 || !shape_at(file, index, s)) return;
    const double sc = scale / 256.0, w = degrees * 3.141592653589793 / 180.0, c = cos(w), n = sin(w);
    const double mx = (flags & 0x10) ? -1 : 1, my = (flags & 0x20) ? -1 : 1;
    const Bit16u seg = mem_readw(rec), rows = mem_readw(rec + 2);
    const int cx0 = (Bit16s)mem_readw(rec + 4), cy0 = (Bit16s)mem_readw(rec + 6);
    const int cx1 = (Bit16s)mem_readw(rec + 8), cy1 = (Bit16s)mem_readw(rec + 10);
    double lox = 1e9, hix = -1e9, loy = 1e9, hiy = -1e9;
    for (int k = 0; k < 4; k++) {
        const double u = ((k & 1 ? s.w : 0) - s.left) * sc * mx, v = ((k & 2 ? s.h : 0) - s.top) * sc * my;
        const double px = x + u * c - v * n, py = y + u * n + v * c;
        lox = px < lox ? px : lox; hix = px > hix ? px : hix;
        loy = py < loy ? py : loy; hiy = py > hiy ? py : hiy;
    }
    int X0 = (int)floor(lox) - 1, X1 = (int)ceil(hix) + 1, Y0 = (int)floor(loy) - 1, Y1 = (int)ceil(hiy) + 1;
    X0 = X0 < cx0 ? cx0 : X0; X1 = X1 > cx1 ? cx1 : X1;
    Y0 = Y0 < cy0 ? cy0 : Y0; Y1 = Y1 > cy1 ? cy1 : Y1;
    if (X0 > X1 || Y0 > Y1) return;
    Bit8u line[W];
    for (int Y = Y0; Y <= Y1; Y++) {
        const PhysPt row = ((PhysPt)seg << 4) + mem_readw(ds_at((Bit16u)(rows + 2 * Y))) + X0;
        const int n1 = X1 - X0 + 1;
        lin_read(row, line, n1);
        bool any = false;
        for (int X = X0; X <= X1; X++) {
            const double gx = X + 0.5 - x, gy = Y + 0.5 - y;
            const double u = (gx * c + gy * n) * mx / sc + s.left, v = (-gx * n + gy * c) * my / sc + s.top;
            const int iu = (int)floor(u), iv = (int)floor(v);
            if (iu < 0 || iv < 0 || iu >= s.w || iv >= s.h) continue;
            const Bit16s colour = s.px[iv * s.w + iu];
            if (colour < 0) continue;
            line[X - X0] = (Bit8u)colour;
            any = true;
        }
        if (any) lin_write(row, line, n1);
    }
}

// At the routine's entry in a quarter: drawn here, and returned from as its retf (the caller pops the arguments)
void turn_entry() {
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const Bit16u retIp = mem_readw(sp), retCs = mem_readw(sp + 2);
    const PhysPt a = sp + 4;
    const PhysPt rec = ds_at(mem_readw(a));
    const int x = (Bit16s)mem_readw(a + 2), y = (Bit16s)mem_readw(a + 4);
    const PhysPt file = ((PhysPt)mem_readw(a + 8) << 4) + mem_readw(a + 6);
    turn_draw(rec, x, y, file, (Bit16s)mem_readw(a + 10), (Bit16s)mem_readw(a + 12), (Bit16s)mem_readw(a + 14),
              mem_readw(a + 16));
    reg_sp = (Bit16u)(reg_sp + 4);
    SegSet16(cs, retCs);
    reg_eip = retIp;
}

// WCARMHIRESDRAWS=1 (a test aid): each kind of draw the display list calls, logged once (what each changes in its own
// object is to be looked at: tools objwrites.py)
void list_call() {
    static int on = -1;
    static std::set<Bit32u> seen;
    if (on < 0) on = getenv("WCARMHIRESDRAWS") != NULL;
    if (!on) return;
    // (at the call far [bx]: BX the object's table in DS, its first entry the draw's far address)
    const PhysPt target = ds_at(reg_bx);
    const Bit16u off = mem_readw(target), seg = mem_readw(target + 2);
    const Bit32u key = ((Bit32u)(Bit16u)(seg - g_load) << 16) | off;
    if (seen.insert(key).second) wclog(1, "Armada draws: a new kind, %04X:%04X (from the load segment)", key >> 16, key & 0xFFFF);
}

// ----- the cockpit's stage (level 1: the lines on the space; level 2: all its lines, the reticle, turned shapes) -----

armart::Target over_target(PhysPt rec) {
    armart::Target t;
    t.px = &g_over[0];
    t.set = &g_overSet[0];
    t.w = FW;
    t.h = FH;
    t.x0 = (Bit16s)mem_readw(rec + 4); t.y0 = (Bit16s)mem_readw(rec + 6);
    t.x1 = (Bit16s)mem_readw(rec + 8); t.y1 = (Bit16s)mem_readw(rec + 10);
    return t;
}

void return_far() {
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const Bit16u retIp = mem_readw(sp), retCs = mem_readw(sp + 2);
    reg_sp = (Bit16u)(reg_sp + 4);
    SegSet16(cs, retCs);
    reg_eip = retIp;
}

Bit8u pixel_of(PhysPt rec, int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;
    const Bit16u seg = mem_readw(rec), rows = mem_readw(rec + 2);
    Bit8u c = 0;
    mem_readb_checked(((PhysPt)seg << 4) + mem_readw(ds_at((Bit16u)(rows + 2 * y))) + x, &c);
    return c;
}

// The line (bitmap record, x0, y0, x1, y1, colour): one fine pixel wide, if it is drawn on the space (the target's
// brackets: level 1 too) or at level 2 any; else the game's own
void cockpit_line() {
    const PhysPt a = SegPhys(ss) + reg_sp + 4;
    const PhysPt rec = ds_at(mem_readw(a));
    const int x0 = (Bit16s)mem_readw(a + 2), y0 = (Bit16s)mem_readw(a + 4);
    const int x1 = (Bit16s)mem_readw(a + 6), y1 = (Bit16s)mem_readw(a + 8);
    const Bit8u colour = mem_readb(a + 10);
    // on the space: a third of its pixels on the marker or more (the reticle, drawn first, may cross a bracket's piece)
    int n = 0, on = 0;
    {
        int X = x0, Y = y0;
        const int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int k = 0; k < 1024; k++) {
            n++;
            if (pixel_of(rec, X, Y) == kMarker) on++;
            if (X == x1 && Y == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; X += sx; }
            if (e2 <= dx) { err += dx; Y += sy; }
        }
    }
    const bool space = 3 * on >= n;
    static int told = 0;
    if (getenv("WCARMHIRESDEBUG") && told < 40) {
        told++;
        wclog(1, "Armada line: %d,%d - %d,%d colour %02X, under its middle %02X (%s), clip %d,%d-%d,%d", x0, y0, x1, y1, colour,
              pixel_of(rec, (x0 + x1) / 2, (y0 + y1) / 2), space ? "space" : "cockpit", (Bit16s)mem_readw(rec + 4),
              (Bit16s)mem_readw(rec + 6), (Bit16s)mem_readw(rec + 8), (Bit16s)mem_readw(rec + 10));
    }
    if (!space && level() < 2) return;
    armart::line(over_target(rec), x0, y0, x1, y1, colour);
    g_overAny = true;
    return_far();
}

// The shape in one colour (bitmap record, x, y, shapes far, index, colour) at the view's middle: the reticle, as line
// art (level 2); anything else (the text) the game's own
void cockpit_mono() {
    const PhysPt a = SegPhys(ss) + reg_sp + 4;
    const int x = (Bit16s)mem_readw(a + 2), y = (Bit16s)mem_readw(a + 4);
    const PhysPt view = ds_at(g_b->view);
    if (level() < 2 || x != (Bit16s)mem_readw(view) || y != (Bit16s)mem_readw(view + 2)) return;
    const PhysPt rec = ds_at(mem_readw(a));
    const PhysPt file = ((PhysPt)mem_readw(a + 8) << 4) + mem_readw(a + 6);
    Shape s;
    if (!shape_at(file, (Bit16s)mem_readw(a + 10), s)) return;
    const Bit8u colour = mem_readb(a + 12);
    armart::Shape art;
    art.w = s.w; art.h = s.h; art.left = s.left; art.top = s.top;
    art.px = s.px;
    for (size_t i = 0; i < art.px.size(); i++) if (art.px[i] >= 0) art.px[i] = colour;
    armart::shape_lines(over_target(rec), art, x, y);
    g_overAny = true;
    return_far();
}

// A turned-and-scaled shape in the cockpit's stage (the target display's ship): drawn at the fine pixels (level 2)
void cockpit_turn() {
    if (level() < 2) return;
    const PhysPt a = SegPhys(ss) + reg_sp + 4;
    const PhysPt rec = ds_at(mem_readw(a));
    const int x = (Bit16s)mem_readw(a + 2), y = (Bit16s)mem_readw(a + 4);
    const PhysPt file = ((PhysPt)mem_readw(a + 8) << 4) + mem_readw(a + 6);
    const int index = (Bit16s)mem_readw(a + 10), degrees = (Bit16s)mem_readw(a + 12), scale = (Bit16s)mem_readw(a + 14);
    const int flags = mem_readw(a + 16);
    Shape s;
    if (scale <= 0 || scale > 64 * 256 || !shape_at(file, index, s)) return;
    const armart::Target t = over_target(rec);
    const double sc = scale / 256.0, w = degrees * 3.141592653589793 / 180.0, c = cos(w), n = sin(w);
    const double mx = (flags & 0x10) ? -1 : 1, my = (flags & 0x20) ? -1 : 1;
    for (int Y = 2 * t.y0; Y < 2 * (t.y1 + 1); Y++) {
        for (int X = 2 * t.x0; X < 2 * (t.x1 + 1); X++) {
            const double gx = (X + 0.5) / 2 - x, gy = (Y + 0.5) / 2 - y;
            if (fabs(gx) > 2 * (s.w + s.h) * sc + 4 || fabs(gy) > 2 * (s.w + s.h) * sc + 4) continue;
            const int iu = (int)floor((gx * c + gy * n) * mx / sc + s.left), iv = (int)floor((-gx * n + gy * c) * my / sc + s.top);
            if (iu < 0 || iv < 0 || iu >= s.w || iv >= s.h) continue;
            const Bit16s colour = s.px[iv * s.w + iu];
            if (colour < 0) continue;
            armart::plot(t, X, Y, (Bit8u)colour);
        }
    }
    g_overAny = true;
    return_far();
}

// WCARMHIRESCALLS=1 (a test aid, ARMADA.EXE): during the cockpit's stage, each library routine's call logged once a
// caller, with its arguments (which draws the reticle, the brackets, the radar, the displays); =2: at any time (the
// strategy map's, the menus')
const Bit16u kLibSeg[] = { 0x5F32, 0x5F91, 0x5F69, 0x5F55, 0x5F51, 0x5FB6, 0x5FB7, 0x5FCB, 0x6493, 0x5E2C, 0x5EE6,
                           0x5D8D, 0x5DE6, 0x5D70 };
const Bit16u kLibOff[] = { 0x0006, 0x0008, 0x0008, 0x0004, 0x000E, 0x0006, 0x000C, 0x0004, 0x0ECC, 0x000E, 0x000E,
                           0x0006, 0x0242, 0x0000 };
int g_calls = -1;
bool calls_on() {
    if (g_calls < 0) g_calls = getenv("WCARMHIRESCALLS") && g_b && !strcmp(g_b->exe, "ARMADA.EXE") ? atoi(getenv("WCARMHIRESCALLS")) : 0;
    return g_calls >= 1;
}
bool lib_call(Bit16u para, Bit16u ip) {
    for (size_t i = 0; i < sizeof(kLibSeg) / sizeof(kLibSeg[0]); i++) {
        if (para != kLibSeg[i] || ip != kLibOff[i]) continue;
        if (g_calls == 1 && g_pass != 5) return true;
        static std::set<Bit64u> seen;
        const PhysPt sp = SegPhys(ss) + reg_sp;
        const Bit16u retIp = mem_readw(sp), retCs = (Bit16u)(mem_readw(sp + 2) - g_load);
        const Bit64u key = ((Bit64u)i << 32) | ((Bit32u)retCs << 16) | retIp;
        if (!seen.insert(key).second) return true;
        char a[96] = "";
        for (int k = 0; k < 9; k++) snprintf(a + strlen(a), sizeof(a) - strlen(a), " %d", (Bit16s)mem_readw(sp + 4 + 2 * k));
        wclog(1, "Armada calls: %04X:%04X from %04X:%04X, args%s", para, ip, retCs, retIp, a);
        return true;
    }
    return false;
}

// ----- !armside ----------------------------------------------------------------------------------------------------

void side_snap(std::vector<Bit8u> &v) {
    const Bitu n = MEM_TotalPages() * 4096;
    v.resize(n);
    memcpy(&v[0], MemBase, n);
}

void side_report() {
    const char *dir = getenv("WCNET_DUMP_DIR");
    FILE *f = fopen((std::string(dir ? dir : ".") + "/side.txt").c_str(), "w");
    unsigned differ = 0;
    const size_t n = g_sideP.size();
    for (size_t a = 0; a < n; a++) {
        if (g_sideA[0][a] == g_sideA[1][a] && g_sideA[1][a] == g_sideA[2][a]) continue;
        differ++;
        if (f) fprintf(f, "%07X P %02x A0 %02x A1 %02x A2 %02x\n", (unsigned)a, g_sideP[a], g_sideA[0][a], g_sideA[1][a], g_sideA[2][a]);
    }
    if (f) fclose(f);
    wclog(1, "Armada side: %u bytes differ between passes 0, 1 and 2 (side.txt)", differ);
    g_sideP.clear();
    for (int k = 0; k < 3; k++) g_sideA[k].clear();
}

// ----- the section's places ----------------------------------------------------------------------------------------

// WCARMHIRESTIME=1: a test aid, every 5 s: a frame's emulated milliseconds in the passes, in the rest, and its period
double g_tTop = -1, g_tLast = -1, g_sumPass, g_sumFrame, g_maxPass, g_tNext;
unsigned g_nTime;
int g_timeOn = -1;

void time_frame_top() {
    if (g_timeOn < 0) g_timeOn = getenv("WCARMHIRESTIME") != NULL;
    if (!g_timeOn) return;
    const double now = PIC_FullIndex();
    if (g_tLast > 0) { g_sumFrame += now - g_tLast; g_nTime++; }
    g_tLast = now;
    g_tTop = now;
    if (now >= g_tNext && g_nTime) {
        wclog(1, "Armada time: %u frames, %.1f ms a frame, passes %.1f ms (most %.1f)", g_nTime, g_sumFrame / g_nTime,
              g_sumPass / g_nTime, g_maxPass);
        g_sumPass = g_sumFrame = g_maxPass = 0;
        g_nTime = 0;
        g_tNext = now + 5000;
    }
}

void time_passes_done() {
    if (g_timeOn <= 0 || g_tTop < 0) return;
    const double d = PIC_FullIndex() - g_tTop;
    g_sumPass += d;
    if (d > g_maxPass) g_maxPass = d;
}

void at_top() {
    if (g_jumped) {             // (the browser's call for the place jumped to: the pass was set up by the jump)
        g_jumped = false;
        if (getenv("WCARMHIRESDEBUG")) wclog(1, "Armada top: a jump's arrival (pass %d)", g_pass);
        return;
    }
    if (getenv("WCARMHIRESDEBUG") && g_pass >= 0) wclog(1, "Armada top: a frame began with pass %d unfinished", g_pass);
    view_back();                // (a frame left in the middle: the game's view again)
    g_pass = -1;
    time_frame_top();
    if (!g_codeOk || !wanted() || vga.mode != M_VGA) return;
    const Bitmap b = bitmap();
    if (!b.ok) {
        if (getenv("WCARMHIRESDEBUG")) wclog(1, "Armada top: the bitmap's clip %d,%d-%d,%d: the game's own", b.x0, b.y0, b.x1, b.y1);
        return;
    }
    g_pass = 0;
    g_starKeep.clear();
    g_keep.clear();
    if (g_side == 1) {
        g_side = 2;
        side_snap(g_sideP);
    }
}

void next_pass() {
    view_back();
    if (g_side == 2 && g_pass <= 2) {
        side_snap(g_sideA[g_pass]);
        if (g_pass == 2) {
            side_report();
            g_side = 0;
        }
    }
    g_pass++;
    reg_eip = g_b->top;         // (the section again: the next quarter, or the marker's clear)
    g_jumped = true;
}

bool frame_hook(Bit16u ip) {
    const Bit16u o = (Bit16u)(ip - g_b->top);
    switch (o) {
    case 0:
        at_top();
        return true;
    case kCleared:
        g_jumped = false;
        return true;
    case kSetup:
        if (g_pass >= 0 && g_pass <= 3) {
            g_empty[g_pass] = !view_quarter(g_pass);
            if (g_empty[g_pass]) {
                fill_quarter(g_pass, 0);
                reg_sp += 2;            // (the view's push, which the setup call's pop would have taken)
                next_pass();
            }
        }
        return true;
    case kDrawn:
        if (g_jumped) {
            g_jumped = false;
            return true;
        }
        if (g_pass >= 0 && g_pass <= 3) {
            keep_quarter(g_pass);
            next_pass();
        } else if (g_pass == 4) {
            // the game's own pass, as it always runs: its scene kept, and the cockpit drawn over the marker
            const Bitmap b = bitmap();
            for (int y = 0; y < H; y++) lin_read(b.row[y], &g_own[y * W], W);
            marker_clear();
            if (g_overAny) {
                std::fill(g_overSet.begin(), g_overSet.end(), 0);
                g_overAny = false;
            }
            g_pass = 5;
        }
        return true;
    case kCockpit:
        g_jumped = false;
        if (g_pass == 5) {
            compose();
            time_passes_done();
        }
        g_pass = -1;
        return true;
    }
    return false;
}

void boost(bool on) {
    if (on) {
        if (g_cyclesBoost || CPU_CycleAutoAdjust) return;
        if (g_cyclesWant < 0) {
            const char *env = getenv("WCARMHIRESCYCLES");
            g_cyclesWant = env && env[0] ? atoi(env) : 3 * (int)CPU_CycleMax;
        }
        if (g_cyclesWant <= CPU_CycleMax) return;
        g_cyclesBase = CPU_CycleMax;
        g_cyclesBoost = g_cyclesWant;
        CPU_CycleMax = g_cyclesBoost;
        wclog(1, "Armada: %d cycles while the picture is 640x400 (%d otherwise)", (int)g_cyclesBoost, (int)g_cyclesBase);
    } else if (g_cyclesBoost) {
        if (CPU_CycleMax == g_cyclesBoost) CPU_CycleMax = g_cyclesBase;     // (changed by hand meanwhile: the player's)
        g_cyclesBoost = 0;
    }
}

// The wait's loop head (the timer's call): this round's spin skipped, as far as the next event allows
void wait_idle() {
    if (!g_override || CPU_Cycles <= 0) return;
    const Bit32s step = (Bit32s)(kIdleStepMs * CPU_CycleMax);
    const Bit32s skip = CPU_Cycles < step ? CPU_Cycles : step;
    CPU_IODelayRemoved += skip;
    CPU_Cycles -= skip;
}

bool g_mapModeWas;              // (the map's mode as the override was last set: its log line)

void set_override(bool on, bool map) {
    if (on == g_override && map == g_mapModeWas) return;
    const bool was = g_override;
    g_override = on;
    g_mapModeWas = map;
    if (on) {
        if (!was) {
            const double fps = vga.draw.delay.vtotal > 0 ? 1000.0 / vga.draw.delay.vtotal : 70.0;
            VGA_SetOverride(true);
            RENDER_SetSize(FW, FH, 8, (float)fps, vga.draw.aspect_ratio, false, false);
        }
        wclog(1, "Armada: the %s picture is 640x400", map ? "strategy map's" : "flight's");
    } else {
        VGA_SetOverride(false);
        wclog(1, "Armada: the picture is the game's own");
    }
}


// ----- the strategy map ----------------------------------------------------------------------------------------------
//
// The strategy screen's map panel (x 105..301, y 15..151: 197 x 137, its middle 203,83) is drawn at one world unit a
// pixel: an object's screen place is its world place less the view's scroll (the view: DS:[76CA]; its scroll +42h,
// +44h; the limits of its middle +46h..+4Ch, the sector less half the panel).  So a view twice as wide and tall at the
// same pixel size is four of the game's own panels side by side: each frame the map's panel routine (seg14C6:066D,
// called from the screen's frame function at 3084) runs four times with the scroll shifted to each quarter of a view
// twice the size (the objects' screen places and hit boxes, which the game computes after the drawing for the next
// frame, set for each quarter as its own conversion would set them), the panel kept each time and its background put
// back, then once more as the game runs it.  Nothing is scaled: every icon, line and circle is the game's own, at its
// own pixel size, a 640x400 pixel each (the user, 2026-11-23: "everything is just zoomed out one level so that we
// don't need to size anything up").  Only this panel: the menus, dialogs and other screens are the game's own picture.
//  - The doubled view is kept within the sector as the game keeps its own: its middle's limits less another half panel
//    (or the sector's middle, where the sector is narrower than the view); the game's own scroll is put there, so the
//    game's view and the one shown have the same middle ("are we allowed to peer below it?": no).
//  - The grid: every other line drawn in the quarters (the user: "skip half the draws so the effective grid spacing on
//    a screen is the same"): a line every 20 units and a darker one every 80, 20 and 80 pixels apart as before.
//  - Clicks: the game's own conversion (seg11DE:07B8, an object's screen place and its +-5 hit box, from the map's
//    update loop only: 14C6:0AA4 -> 11DE:0927 -> 07B8) puts each object at half its distance from the middle while the
//    picture is on, so the game's own pointer (half the 640x400 one) meets its hit box where the 640x400 picture shows
//    the object, the box twice the object's size there as before.
//  - The overview's rectangle (seg14C6:44CD) is the doubled view's, kept inside the overview.
//  - The arrow keys, which the map does not use, move the view (250 units a second; 125 on the game's own map), within
//    those limits, the picture on or not; not under a dialog.
//  - The wheel: up, the game's own map; down, one level out again; about the pointer (map_zoom).
//  - + and - in Move Resources: a click on a row's arrows (mr_update; the game's own keys do the rest: F1..F8).
// The picture: each screen refresh the game's screen doubled, and in the panel wherever the screen still holds the
// game's own pass's pixel (of the latest frame or the one before) the 640x400 panel instead: the pointer, the dialogs
// and anything else drawn over the panel stay the game's.
struct MapBuild {
    const char *exe;
    Bit16u seg, objSeg;         // the map screen's code (frame, panel, overview), its objects' code
    Bit16u frame;               // the screen's frame function's entry (55 8B EC)
    Bit16u loop, after;         // the panel's call: its first instruction (cmp [si+174h],0), after its pop cx
    Bit16u updateRet;           // the update loop's return from an object's method (call far [bx+14h]; then add sp,4)
    Bit16u conv, convRet;       // the conversion's place after +0A/+0C are computed; 11DE:0927's return from it
    Bit16u grid[4];             // the grid's line calls: minor across, minor down, major across, major down
    Bit16u ovSize, ovLines;     // the overview's rectangle: before its half-sizes are read, before its lines
    Bit16u regCall;             // the registered objects' loop (seg14C6:449A): its call of each one's draw (call far [bx+4])
    Bit16u present;             // the frame's end, before the pointer and the copy to the screen (push [rec]; call far)
    Bit16u planetConv, fleetConv;   // the objects' kinds by their update method: a planet's (world - scroll), a fleet's
    Bit16u planetBox;           // the planet's conversion after its hit box is computed (cmp [si+2],69h)
    Bit16u fleetDone, fleetBox, fleetRet;   // the fleet's: its place computed; its box computed; its caller's return
    Bit16u viewPtr, held;       // DS: the view's near pointer; the held-key table (by scan code)
    Bit16u mouseX, mouseY;      // DS: the pointer (x 0..639, y 0..199)
    // a marker beside a planet (a squadron, a fleet's other ships, the other side's presence: seg11DE:23A0, its planet
    // at +10h): its update method; its offset from the planet (2361); after 2361's call; its box computed
    Bit16u markConv, markOffset, markPlaced, markBox;
    // the Move Resources dialog (seg0C92 in ARMADA.EXE): its update method's entry; its mouse handler (its arrows' boxes)
    Bit16u dlgSeg, mrUpdate, mrEvent;
    Bit16u qStart, qEnd, qHead, qTail, qSize;   // DS: the mouse's event queue (seg6A6C:0162 puts, 0180 takes)
};
const MapBuild kMapBuilds[] = {
    { "ARMADA.EXE", 0x14C6, 0x11DE, 0x2DD2, 0x306D, 0x3094, 0x0AA7, 0x07DD, 0x0939,
      { 0x074F, 0x0783, 0x07B7, 0x07EB }, 0x4554, 0x458A, 0x44C0, 0x311F, 0x07B8, 0x2892, 0x080A, 0x28B1, 0x28D5, 0x0B08, 0x76CA, 0x8E56, 0x8EFA, 0x8EFC,
      0x2390, 0x2361, 0x23EE, 0x2412, 0x0C92, 0x0F05, 0x11F5, 0x6C2C, 0x6C2E, 0x6C30, 0x6C32, 0x8ED8 },
    { "ARMADACD.EXE", 0x14C9, 0x11E1, 0x2DD2, 0x306D, 0x3094, 0x0AA7, 0x07DD, 0x0939,
      { 0x074F, 0x0783, 0x07B7, 0x07EB }, 0x4554, 0x458A, 0x44C0, 0x311F, 0x07B8, 0x2892, 0x080A, 0x28B1, 0x28D5, 0x0B08, 0x774A, 0x8ED6, 0x8F7A, 0x8F7C,
      0x2390, 0x2361, 0x23EE, 0x2412, 0x0C95, 0x0F05, 0x11F5, 0x6CB8, 0x6CBA, 0x6CBC, 0x6CBE, 0x8F58 },
};   // (Proving Grounds has no strategy map)
enum { kPX0 = 105, kPY0 = 15, kPW = 197, kPH = 137, kPCX = 203, kPCY = 83, kMW = 2 * kPW, kMH = 2 * kPH };
enum { kMapX0 = 2 * kPX0, kMapY0 = 2 * kPY0 };      // the doubled panel on the 640x400 screen
const double kPanPerMs = 0.25;  // the arrows' speed: world units a millisecond

const MapBuild *g_m;
bool g_mapCodeOk;
int g_mapPass = -1;             // 0..3 the quarters, 4 the game's own pass; -1 none
bool g_mapJumped;
bool g_mapMode;                 // the 640x400 picture is the map's now
std::vector<Bit8u> g_mapBg(kPW * kPH), g_mapComp[2], g_mapSnap[2], g_mapMask[2], g_mapPre(kPW * kPH), g_mapPost(kPW * kPH);
bool g_mapDiff;                 // a draw that is not the map's is running in the game's own pass (its pixels: the mask)
bool g_mapTail;                 // the game's own pass is done and the rest of the frame draws over the marker
int g_mapGen;                   // the newest of the two
bool g_mapHave[2];
double g_mapLastOwn = -1e9, g_mapPanAt = -1;
double g_mapPanX, g_mapPanY;    // the arrows' part units not yet moved
struct MapObj { PhysPt at; int kind; Bit8u f[12]; };   // an object, its kind (0 planet, 1 fleet, 2 other), its +02..+0D
std::set<Bit32u> g_mapKindsTold;
std::vector<MapObj> g_mapObjs;
Bit16u g_mapScroll[2], g_mapMouse[2];
Bit16u g_mapMouseSet[2];        // the pointer as the latest quarter put it (else the mouse's own interrupt moved it)
Bit16u g_mapOvHalf[2];
bool g_mapOvSet;
unsigned g_mapFrames;
Bit16u g_mapMarker;             // the sector record's +15Ch: the selection's marker (drawn in the panel routine), or 0
bool g_mapZoomIn;               // the wheel's: the game's own map (up), or one level out (down; the default)
int g_mapWheel;                 // the wheel's turn not yet taken (the frame's entry takes it: the quarters' scroll and
                                // pointer are the game's own again there)
double g_mapFrameAt = -1e9;     // the map screen's frame function's latest entry
bool g_mrOk;                    // the Move Resources dialog and the event queue are as known (+ and -)
double g_mrAt = -1e9;           // its update's latest call
std::vector<int> g_mrPending;   // + (1) and - (-1) not yet clicked

PhysPt map_view() { return ds_at(rd16l(ds_at(g_m->viewPtr))); }
PhysPt g_mapDS;                 // the game's DS, from the map's frame and the dialog's update (the keys and the wheel
                                // come between any two instructions: an interrupt's DS, maybe)
PhysPt map_view_in(PhysPt dsb) { return dsb + mem_readw(dsb + g_m->viewPtr); }

bool map_code_here() {
    const PhysPt c = (PhysPt)(Bit16u)(g_load + g_m->seg) << 4, o = (PhysPt)(Bit16u)(g_load + g_m->objSeg) << 4;
    if (mem_readb(c + g_m->frame) != 0x55 || mem_readw(c + g_m->frame + 1) != 0xEC8B) return false;
    if (mem_readd(c + g_m->loop) != 0x0174BC83 || mem_readb(c + g_m->loop + 4) != 0x00) return false;   // cmp [si+174h],0
    if (mem_readw(c + g_m->loop + 0x16) != 0xE80E || mem_readb(c + g_m->after - 1) != 0x59 ||
        mem_readw(c + g_m->after - 5) != 0xE80E) return false;     // push cs; call 066D ... push cs; call 449A; pop cx
    if (mem_readw(c + g_m->regCall) != 0x5FFF || mem_readb(c + g_m->regCall + 2) != 0x04) return false;   // call far [bx+4]
    if (mem_readw(c + g_m->present) != 0x36FF || mem_readb(c + g_m->present + 4) != 0x9A) return false;
    if (mem_readd(o + g_m->planetConv) != 0x56EC8B55 || mem_readd(o + g_m->fleetConv) != 0x56EC8B55) return false;
    // the boxes' first check (cmp word [si+2],69h) after each; the fleet's place: add ax,-8; mov [si+0Ch],ax; its caller
    if (mem_readd(o + g_m->planetBox) != 0x69027C83 || mem_readd(o + g_m->fleetBox) != 0x69027C83) return false;
    if (mem_readd(o + g_m->fleetDone - 6) != 0x89FFF805 || mem_readw(o + g_m->fleetDone - 2) != 0x0C44 ||
        mem_readw(o + g_m->fleetRet - 2) != 0x1FFF) return false;
    // the conversion: sub ax,[bx+44h]; mov [si+0Ch],ax; cmp word [si+1Eh],0
    if (mem_readd(o + g_m->conv - 6) != 0x8944472B || mem_readb(o + g_m->conv - 1) != 0x0C ||
        mem_readd(o + g_m->conv) != 0x001E7C83) return false;
    if (mem_readw(o + g_m->convRet - 2) != 0x1FFF) return false;
    // the marker's update; its offset (idx > 0: x - 12, y + (row - count/2) * 8; else x + 12, y + 8); after the offset's
    // call (add sp,0Ah; mov ax,[si+0Ah]; add ax,-3); its box (cmp word [si+2],69h)
    if (mem_readd(o + g_m->markConv) != 0x56EC8B55 || mem_readd(o + g_m->markOffset) != 0x56EC8B55 ||
        mem_readd(o + g_m->markOffset + 0x0B) != 0x000C7E83 || mem_readd(o + g_m->markOffset + 0x11) != 0x8B0C2C83 ||
        mem_readd(o + g_m->markOffset + 0x1E) != 0x0103E2C1 || mem_readd(o + g_m->markOffset + 0x25) != 0x830C0483 ||
        mem_readw(o + g_m->markOffset + 0x29) != 0x0805) return false;
    if (mem_readw(o + g_m->markPlaced - 3) != 0xC483 || mem_readd(o + g_m->markPlaced) != 0x050A448B ||
        mem_readw(o + g_m->markPlaced + 4) != 0xFFFD || mem_readd(o + g_m->markBox) != 0x69027C83) return false;
    for (int i = 0; i < 4; i++) if (mem_readb(c + g_m->grid[i]) != 0x9A || mem_readw(c + g_m->grid[i] + 1) != 0x0006) return false;
    if (mem_readd(c + g_m->ovSize) != 0x45BF0F66 || mem_readb(c + g_m->ovSize + 4) != 0x18) return false;
    return mem_readw(c + g_m->ovLines) == 0x7D6A;
}

// The Move Resources dialog's update (55 8B EC 56 57 8B 76 06, then its key: mov al,[...]) and its mouse handler: a
// row's arrows at x C7..CD (to the ship) and CF..D5 (to the planet), its rows 8 high from y 4Ch
bool mr_code_here() {
    const PhysPt d = (PhysPt)(Bit16u)(g_load + g_m->dlgSeg) << 4;
    if (mem_readd(d + g_m->mrUpdate) != 0x56EC8B55 || mem_readd(d + g_m->mrUpdate + 4) != 0x06768B57 ||
        mem_readb(d + g_m->mrUpdate + 8) != 0xA0) return false;
    const PhysPt e = d + g_m->mrEvent;
    if (mem_readd(e) != 0x83EC8B55 || mem_readb(e + 0x10F) != 0xC7 || mem_readd(e + 0x110) != 0x004CF846) return false;
    if (mem_readd(e + 0x12F) != 0x00C7FF81 || mem_readd(e + 0x135) != 0x00CDFF81) return false;
    if (mem_readd(e + 0x172) != 0x00CFFF81 || mem_readd(e + 0x178) != 0x00D5FF81) return false;
    return mem_readd(e + 0x143) == 0x05F8468B && mem_readw(e + 0x147) == 0x0008;   // (mov ax,[bp-8]; add ax,8)
}

bool map_wanted() { return g_m && g_mapCodeOk && wanted() && !g_mapZoomIn && vga.mode == M_VGA; }

// A dialog over the map (view+2A4h: Move Resources' and the like; view+2ACh: the catalog's, the menu's): the arrows
// and the wheel leave the map alone then (the catalog's list takes Up and Down)
bool map_dialog() {
    const PhysPt view = map_view();
    return mem_readw(view + 0x2A4) || mem_readw(view + 0x2AC);
}

// The doubled view's middle for the view's own middle (cx, cy): within the sector
void map_clamp(PhysPt view, int &cx, int &cy, bool doubled) {
    const int x0 = (Bit16s)mem_readw(view + 0x46), x1 = (Bit16s)mem_readw(view + 0x48);
    const int y0 = (Bit16s)mem_readw(view + 0x4A), y1 = (Bit16s)mem_readw(view + 0x4C);
    if (!doubled) {
        cx = cx < x0 ? x0 : cx > x1 ? x1 : cx;
        cy = cy < y0 ? y0 : cy > y1 ? y1 : cy;
        return;
    }
    const int left = x0 - kPCX + kPX0, right = x1 + (kPX0 + kPW - kPCX), top = y0 - kPCY + kPY0, bottom = y1 + (kPY0 + kPH - kPCY);
    cx = left + kPW > right - kPW ? (left + right) / 2 : std::max(left + kPW, std::min(right - kPW, cx));
    cy = top + kPH > bottom - kPH ? (top + bottom) / 2 : std::max(top + kPH, std::min(bottom - kPH, cy));
}

// The wheel's turn, at the frame's entry
void map_zoom(bool in) {
    g_mapWheel = 0;
    const PhysPt view = map_view();
    if (in == g_mapZoomIn || !wanted() || map_dialog()) return;
    const int px = (Bit16s)mem_readw(ds_at(g_m->mouseX)), py = 2 * (Bit16s)mem_readw(ds_at(g_m->mouseY));
    int ox = 0, oy = 0;
    if (px >= kMapX0 && px < kMapX0 + kMW && py >= kMapY0 && py < kMapY0 + kMH) {
        ox = px - (kMapX0 + kPW);
        oy = py - (kMapY0 + kPH);
    }
    int cx = (Bit16s)mem_readw(view + 0x42) + kPCX, cy = (Bit16s)mem_readw(view + 0x44) + kPCY;
    cx += in ? ox / 2 : -(ox / 2);
    cy += in ? oy / 2 : -(oy / 2);
    map_clamp(view, cx, cy, !in);
    mem_writew(view + 0x42, (Bit16u)(cx - kPCX));
    mem_writew(view + 0x44, (Bit16u)(cy - kPCY));
    mem_writew(view + 0x52, 0);         // (the game's own glide to a place, if one was going)
    g_mapZoomIn = in;
    g_mapHave[0] = g_mapHave[1] = false;    // (one level out again: from its next frame)
    wclog(1, "Armada map: %s (the middle %d,%d; the pointer %d,%d)", in ? "the game's own" : "one level out", cx, cy, px, py);
}

// At the frame function's entry (the picture on or not): the wheel's turn, the arrows' move, and the game's scroll put
// where the doubled view is
void map_frame() {
    const double now = PIC_FullIndex();
    g_mapFrameAt = now;
    g_mapDS = ds_at(0);
    if (g_mapWheel) map_zoom(g_mapWheel > 0);
    const PhysPt view = map_view();
    int cx = (Bit16s)mem_readw(view + 0x42) + kPCX, cy = (Bit16s)mem_readw(view + 0x44) + kPCY;
    const int wasX = cx, wasY = cy;
    const PhysPt held = ds_at(g_m->held);
    const bool free = !map_dialog();
    const int ax = free ? (mem_readb(held + 0x4D) == 1) - (mem_readb(held + 0x4B) == 1) : 0;
    const int ay = free ? (mem_readb(held + 0x50) == 1) - (mem_readb(held + 0x48) == 1) : 0;
    if ((ax || ay) && g_mapPanAt >= 0) {
        const double dt = std::min(now - g_mapPanAt, 100.0);
        const double speed = map_wanted() ? kPanPerMs : kPanPerMs / 2;   // (the same on the screen at either level)
        g_mapPanX += ax * speed * dt;
        g_mapPanY += ay * speed * dt;
        const int mx = (int)g_mapPanX, my = (int)g_mapPanY;
        g_mapPanX -= mx;
        g_mapPanY -= my;
        cx += mx;
        cy += my;
        mem_writew(view + 0x52, 0);     // (the game's own glide to a place, if one was going: the arrows win)
    } else if (!ax && !ay) {
        g_mapPanX = g_mapPanY = 0;
    }
    g_mapPanAt = now;
    if (map_wanted()) map_clamp(view, cx, cy, true);                    // (one level out: the doubled view's limits)
    else if (cx != wasX || cy != wasY) map_clamp(view, cx, cy, false);  // (the game's own map: its own, when moved)
    if (cx != wasX || cy != wasY) {
        mem_writew(view + 0x42, (Bit16u)(cx - kPCX));
        mem_writew(view + 0x44, (Bit16u)(cy - kPCY));
    }
}

// The objects' screen places for the quarter whose scroll is (sx, sy), each where the game itself puts it: a planet as
// seg11DE:07B8 sets it (its hit box too, its calls aside), a fleet as 2892 does (its planet's place + 12,-8: beside it,
// as the game draws it), a marker beside a planet (seg11DE:23A0: its planet at +10h) at its own offset from that
// planet; anything else from the game's own update's place (at half the distance from the middle while the picture is
// on) mapped into the quarter
// The object's update method (load-relative segment, offset)
void map_update_of(PhysPt o, Bit16u &seg, Bit16u &off) {
    const PhysPt vt = ds_at(mem_readw(o));
    seg = (Bit16u)(mem_readw(vt + 2) - g_load);
    off = mem_readw(vt);
}

// A planet the map keeps (its kind 0 entry), or NULL
const MapObj *map_planet(PhysPt at) {
    for (size_t k = 0; k < g_mapObjs.size(); k++) if (g_mapObjs[k].at == at && g_mapObjs[k].kind == 0) return &g_mapObjs[k];
    return NULL;
}

// An object drawn beside a planet: the planet and the object's offset from it in game pixels, as the game itself sets
// it: a fleet (2892) at 12,-8; a marker (23A0, its planet at +10h: a squadron, the other side's presence) by its index
// +14h, its row +16h and the planet's count +40h (2361); anything else of the map's with a planet at +10h at the offset
// the game's own update gave it
bool map_beside(const MapObj &m, PhysPt &planet, int &dx, int &dy) {
    const PhysPt o = m.at;
    if (m.kind == 1) {
        planet = ds_at(mem_readw(o + 0x12));
        dx = 12; dy = -8;
        return true;
    }
    if (m.kind != 2) return false;
    planet = ds_at(mem_readw(o + 0x10));
    const MapObj *p = map_planet(planet);
    if (!p) return false;
    Bit16u useg, uoff;
    map_update_of(o, useg, uoff);
    if (useg == g_m->objSeg && uoff == g_m->markConv) {
        const int idx = (Bit16s)mem_readw(o + 0x14), row = (Bit16s)mem_readw(o + 0x16), n = (Bit16s)mem_readw(planet + 0x40);
        dx = idx > 0 ? -12 : 12;
        dy = idx > 0 ? (Bit16s)(Bit16u)((row - (n >> 1)) << 3) : 8;
        return true;
    }
    dx = (Bit16s)(m.f[8] | m.f[9] << 8) - (Bit16s)(p->f[8] | p->f[9] << 8);
    dy = (Bit16s)(m.f[10] | m.f[11] << 8) - (Bit16s)(p->f[10] | p->f[11] << 8);
    return true;
}

void map_objects_at(int q, int sx, int sy) {
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < g_mapObjs.size(); i++) {
            if (g_mapObjs[i].kind != pass) continue;
            const PhysPt o = g_mapObjs[i].at;
            int a, b;
            if (pass >= 1) {
                // beside its planet (put in this quarter already: the planets go first), at the game's own offset
                PhysPt planet;
                int dx, dy;
                if (map_beside(g_mapObjs[i], planet, dx, dy)) {
                    mem_writew(o + 0x0A, (Bit16u)((Bit16s)mem_readw(planet + 0x0A) + dx));
                    mem_writew(o + 0x0C, (Bit16u)((Bit16s)mem_readw(planet + 0x0C) + dy));
                    continue;
                }
                const int a0 = (Bit16s)(g_mapObjs[i].f[8] | g_mapObjs[i].f[9] << 8), b0 = (Bit16s)(g_mapObjs[i].f[10] | g_mapObjs[i].f[11] << 8);
                const int X = g_mapMode ? 2 * a0 : kMapX0 + a0 + kPW - kPCX, Y = g_mapMode ? 2 * b0 : kMapY0 + b0 + kPH - kPCY;
                mem_writew(o + 0x0A, (Bit16u)(kPX0 + X - kMapX0 - (q & 1) * kPW));
                mem_writew(o + 0x0C, (Bit16u)(kPY0 + Y - kMapY0 - (q >> 1) * kPH));
                continue;
            }
            a = (Bit16s)mem_readw(o + 0x10) - sx;
            b = (Bit16s)mem_readw(o + 0x12) - sy;
            mem_writew(o + 0x0A, (Bit16u)a);
            mem_writew(o + 0x0C, (Bit16u)b);
            if (!mem_readw(o + 0x1E)) {
                mem_writew(o + 0x02, 0);
                mem_writew(o + 0x04, 0);
                continue;
            }
            int l = a - 5, r = a + 5, t = b - 5, d = b + 5;
            if (l < kPX0 && r > kPX0) l = kPX0;
            if (r > kPX0 + kPW - 1 && l < kPX0 + kPW - 1) r = kPX0 + kPW - 1;
            if (t < kPY0 && d > kPY0) t = kPY0;
            if (d > kPY0 + kPH - 1 && t < kPY0 + kPH - 1) d = kPY0 + kPH - 1;
            if (!(l >= kPX0 && r <= kPX0 + kPW - 1 && t >= kPY0 && d <= kPY0 + kPH - 1) &&
                l >= 0x50 && r <= 0x146 && t >= -10 && d <= 0xB0) {
                l = t = r = d = -1;     // (near the panel, not in it: no box)
            }
            mem_writew(o + 0x02, (Bit16u)l);
            mem_writew(o + 0x04, (Bit16u)t);
            mem_writew(o + 0x06, (Bit16u)r);
            mem_writew(o + 0x08, (Bit16u)d);
        }
    }
}

// An object to put in each quarter (once), its kind by its update method
void map_keep_object(Bit16u near) {
    if (!near) return;
    const PhysPt at = ds_at(near);
    for (size_t i = 0; i < g_mapObjs.size(); i++) if (g_mapObjs[i].at == at) return;
    const PhysPt vt = ds_at(mem_readw(at));
    const Bit16u useg = (Bit16u)(mem_readw(vt + 2) - g_load), uoff = mem_readw(vt);
    MapObj m;
    m.at = at;
    m.kind = useg != g_m->objSeg ? 2 : uoff == g_m->planetConv ? 0 : uoff == g_m->fleetConv ? 1 : 2;
    if (m.kind == 2 && g_mapKindsTold.insert((Bit32u)useg << 16 | uoff).second) {
        wclog(1, "Armada map: an object of another kind (update %04X:%04X) placed from its own place", useg, uoff);
    }
    for (int k = 0; k < 12; k++) m.f[k] = mem_readb(at + 2 + k);
    g_mapObjs.push_back(m);
}

// The mouse's interrupt moved the pointer while a quarter had it (it writes the game's +8EFA/8EFC itself): that place
// is the real one now (put back after the quarters, not the one from before them: a move then was lost till the next)
void map_mouse_moved() {
    const Bit16u x = mem_readw(ds_at(g_m->mouseX)), y = mem_readw(ds_at(g_m->mouseY));
    if (x != g_mapMouseSet[0] || y != g_mapMouseSet[1]) { g_mapMouse[0] = x; g_mapMouse[1] = y; }
}

void map_quarter(int q) {
    const PhysPt view = map_view();
    const int cx = (Bit16s)g_mapScroll[0] + kPCX, cy = (Bit16s)g_mapScroll[1] + kPCY;
    const int sx = cx - kPW + (q & 1) * kPW - kPX0, sy = cy - kPH + (q >> 1) * kPH - kPY0;
    mem_writew(view + 0x42, (Bit16u)sx);
    mem_writew(view + 0x44, (Bit16u)sy);
    map_objects_at(q, sx, sy);
    // the pointer in the quarter's terms (it is the 640x400 one: x in 640ths, y in 200ths, as the game keeps it)
    map_mouse_moved();
    const int px = (Bit16s)g_mapMouse[0], py = 2 * (Bit16s)g_mapMouse[1];
    g_mapMouseSet[0] = (Bit16u)(2 * (kPX0 + px - kMapX0 - (q & 1) * kPW));
    g_mapMouseSet[1] = (Bit16u)(kPY0 + py - kMapY0 - (q >> 1) * kPH);
    mem_writew(ds_at(g_m->mouseX), g_mapMouseSet[0]);
    mem_writew(ds_at(g_m->mouseY), g_mapMouseSet[1]);
}

void map_panel_read(std::vector<Bit8u> &to, int stride, int x0, int y0) {
    const Bitmap b = bitmap();
    for (int y = 0; y < kPH; y++) lin_read(b.row[kPY0 + y] + kPX0, &to[(y0 + y) * stride + x0], kPW);
}

void map_panel_write(const std::vector<Bit8u> &from) {
    const Bitmap b = bitmap();
    for (int y = 0; y < kPH; y++) lin_write(b.row[kPY0 + y] + kPX0, &from[y * kPW], kPW);
}

void map_present();

// At the panel's call (the frame's first arrival): the quarters begin
void map_loop() {
    if (g_mapJumped) { g_mapJumped = false; return; }
    if (g_mapTail) map_present();       // (a frame that ended another way: its panel put back all the same)
    g_mapPass = -1;
    if (!map_wanted()) return;
    const Bitmap b = bitmap();
    if (!b.row[0]) return;
    for (int g = 0; g < 2; g++) {
        if (g_mapComp[g].empty()) { g_mapComp[g].assign(kMW * kMH, 0); g_mapSnap[g].assign(kPW * kPH, 0); g_mapMask[g].assign(kPW * kPH, 0); }
    }
    map_panel_read(g_mapBg, kPW, 0, 0);
    const PhysPt view = map_view();
    g_mapScroll[0] = mem_readw(view + 0x42);
    g_mapScroll[1] = mem_readw(view + 0x44);
    g_mapObjs.clear();
    g_mapMarker = 0;
    if ((Bit16s)mem_readw(ds_at((Bit16u)(reg_si + 0x174))) >= 0) {
        const Bit16u rec = (Bit16u)(mem_readw(ds_at((Bit16u)(reg_si + 0x174))) * 0x185 + mem_readw(ds_at((Bit16u)(reg_si + 0x176))));
        g_mapMarker = mem_readw(ds_at((Bit16u)(rec + 0x15C)));
    }
    const int n = (Bit16s)mem_readw(view + 0x6C);
    for (int i = 0; i < n && i < 256; i++) map_keep_object(mem_readw(view + 0x70 + 2 * i));
    const Bit16u cnt = mem_readw(view + 0x288);
    const PhysPt list = ((PhysPt)mem_readw(view + 0x28C) << 4) + mem_readw(view + 0x28A);
    for (int i = 0; i < cnt && i < 256; i++) {
        const Bit16u obj = mem_readw(list + 2 * i);
        const PhysPt vt = ds_at(mem_readw(ds_at(obj)));
        if ((Bit16u)(mem_readw(vt + 6) - g_load) == g_m->objSeg) map_keep_object(obj);   // (the map's own draws)
    }
    map_keep_object(g_mapMarker);
    g_mapMouse[0] = g_mapMouseSet[0] = mem_readw(ds_at(g_m->mouseX));
    g_mapMouse[1] = g_mapMouseSet[1] = mem_readw(ds_at(g_m->mouseY));
    g_mapPass = 0;
    map_quarter(0);
}

// After the panel's call
void map_after() {
    g_mapJumped = false;
    if (g_mapPass < 0) return;
    const int next = 1 - g_mapGen;
    if (g_mapPass <= 3) {
        map_panel_read(g_mapComp[next], kMW, (g_mapPass & 1) * kPW, (g_mapPass >> 1) * kPH);
        map_panel_write(g_mapBg);
        if (g_mapPass < 3) {
            map_quarter(++g_mapPass);
        } else {
            const PhysPt view = map_view();
            mem_writew(view + 0x42, g_mapScroll[0]);
            mem_writew(view + 0x44, g_mapScroll[1]);
            for (size_t i = 0; i < g_mapObjs.size(); i++) {
                for (int k = 0; k < 12; k++) mem_writeb(g_mapObjs[i].at + 2 + k, g_mapObjs[i].f[k]);
            }
            map_mouse_moved();
            mem_writew(ds_at(g_m->mouseX), g_mapMouse[0]);
            mem_writew(ds_at(g_m->mouseY), g_mapMouse[1]);
            std::fill(g_mapMask[next].begin(), g_mapMask[next].end(), 0);
            g_mapDiff = false;
            g_mapPass = 4;
        }
        reg_eip = g_m->loop;            // (the panel again: the next quarter, or the game's own)
        g_mapJumped = true;
        return;
    }
    // the game's own panel kept; the rest of the frame (dialogs, the widgets) then draws over the marker colour, so that
    // every pixel it writes is marked, a dialog's black over the map's black too (map_present puts the panel back)
    map_panel_read(g_mapSnap[next], kPW, 0, 0);
    g_mapPost.assign(kPW * kPH, kMarker);
    map_panel_write(g_mapPost);
    g_mapTail = true;
    g_mapPass = -1;
    if (++g_mapFrames == 1) wclog(1, "Armada: the strategy map's first frame at 640x400 (%d objects)", (int)g_mapObjs.size());
    // WCARMHIRESDEBUG: every 3 s the objects in the doubled view, where the 640x400 picture has them (the click rigs')
    static double told = -1e9;
    if (getenv("WCARMHIRESDEBUG") && PIC_FullIndex() - told > 3000.0) {
        told = PIC_FullIndex();
        const PhysPt view = map_view();
        const int cx = (Bit16s)mem_readw(view + 0x42) + kPCX, cy = (Bit16s)mem_readw(view + 0x44) + kPCY;
        std::string line, more;
        const auto planetXY = [&](PhysPt o, int &X, int &Y) {
            X = kMapX0 + (Bit16s)mem_readw(o + 0x10) - (cx - kPW);
            Y = kMapY0 + (Bit16s)mem_readw(o + 0x12) - (cy - kPH);
        };
        for (size_t i = 0; i < g_mapObjs.size(); i++) {
            const PhysPt o = g_mapObjs[i].at;
            int X, Y;
            if (g_mapObjs[i].kind == 0) {
                if (!mem_readw(o + 0x1E)) continue;
                planetXY(o, X, Y);
            } else {
                PhysPt planet;
                int dx, dy;
                if (!map_beside(g_mapObjs[i], planet, dx, dy) || !map_planet(planet)) continue;
                planetXY(planet, X, Y);
                X += dx;
                Y += dy;
            }
            if (X < kMapX0 || X >= kMapX0 + kMW || Y < kMapY0 || Y >= kMapY0 + kMH) continue;
            char b[48];
            snprintf(b, sizeof(b), " %X@%d,%d", (unsigned)(o - SegPhys(ds)), X, Y);
            (g_mapObjs[i].kind == 0 ? line : more) += b;
            if (g_mapObjs[i].kind) more += g_mapObjs[i].kind == 1 ? "F" : "M";
        }
        wclog(1, "Armada map: middle %d,%d; objects (640x400):%s; beside them:%s", cx, cy, line.c_str(), more.c_str());
        // the registered objects (what seg14C6:449A draws: view+288h, a count and a far list of near pointers), each
        // with its methods +0 (the update's) and +4 (the draw), as load-relative seg:off
        const Bit16u cnt = mem_readw(view + 0x288);
        const PhysPt list = ((PhysPt)mem_readw(view + 0x28C) << 4) + mem_readw(view + 0x28A);
        std::string reg;
        for (int i = 0; i < cnt && i < 64; i++) {
            const Bit16u obj = mem_readw(list + 2 * i);
            const PhysPt vt = ds_at(mem_readw(ds_at(obj)));
            char b[96];
            snprintf(b, sizeof(b), " %X[%04X:%04X %04X:%04X @%d,%d]", obj, (Bit16u)(mem_readw(vt + 2) - g_load), mem_readw(vt),
                     (Bit16u)(mem_readw(vt + 6) - g_load), mem_readw(vt + 4), (Bit16s)mem_readw(ds_at(obj) + 0xA),
                     (Bit16s)mem_readw(ds_at(obj) + 0xC));
            reg += b;
        }
        wclog(1, "Armada map: registered %u:%s", (unsigned)cnt, reg.c_str());
        if (g_mapMarker) {
            const PhysPt mk = ds_at(g_mapMarker), vt = ds_at(mem_readw(mk));
            wclog(1, "Armada map: marker %X [%04X:%04X %04X:%04X %04X:%04X] @%d,%d +10=%X +12=%X", g_mapMarker,
                  (Bit16u)(mem_readw(vt + 2) - g_load), mem_readw(vt), (Bit16u)(mem_readw(vt + 6) - g_load), mem_readw(vt + 4),
                  (Bit16u)(mem_readw(vt + 10) - g_load), mem_readw(vt + 8), (Bit16s)mem_readw(mk + 0xA), (Bit16s)mem_readw(mk + 0xC),
                  mem_readw(mk + 0x10), mem_readw(mk + 0x12));
        }
    }
}

// The frame's end, before the pointer and the copy to the screen: what the rest of the frame wrote over the panel is
// marked, the panel is the frame's own again, and the frame's picture is the newest
void map_present() {
    if (!g_mapTail) return;
    g_mapTail = false;
    const int next = 1 - g_mapGen;
    map_panel_read(g_mapPost, kPW, 0, 0);
    std::vector<Bit8u> &m = g_mapMask[next];
    for (int i = 0; i < kPW * kPH; i++) {
        if (g_mapPost[i] != kMarker) m[i] = 1;
        else g_mapPost[i] = g_mapSnap[next][i];
    }
    map_panel_write(g_mapPost);
    g_mapHave[next] = true;
    g_mapGen = next;
    g_mapLastOwn = PIC_FullIndex();
}

// The grid's line call in a quarter: every other line kept (minor every 20 units, major every 80, in the world)
void map_grid(int which) {
    if (g_mapPass < 0 || g_mapPass > 3) return;
    const PhysPt sp = SegPhys(ss) + reg_sp;     // (rec, x0, y0, x1, y1, colour)
    const PhysPt view = map_view();
    const bool across = which == 0 || which == 2;
    const int world = across ? (Bit16s)mem_readw(sp + 4) + (Bit16s)mem_readw(view + 0x44)
                             : (Bit16s)mem_readw(sp + 2) + (Bit16s)mem_readw(view + 0x42);
    const int phase = across ? 15 : 25;         // (the major lines' place: the panel's corner, 105,15, mod 40)
    const int every = which >= 2 ? 80 : 20;
    if ((((world - phase) % every) + every) % every != 0) reg_eip += 5;     // (past the call; the caller pops)
}

// While the picture is on, the game's own update puts each planet at half its distance from the middle (map_conv); its
// fleet then goes at half its offset (12,-8 game pixels: 6,-4), where the 640x400 picture draws it beside its planet,
// and the hit boxes are what the picture shows: a planet's +-3 (its circle, 12 pixels across on the 640x400 screen),
// a fleet's +-2 (its icon).  The game's own +-5 and +-3 at half the offsets overlapped: a click on a fleet took its
// planet.  (The user, 2026-11-23: "I always remebmer seeing ships and carriers right next to a planet")  A marker
// beside a planet (seg11DE:23A0: a squadron of fighters left of it, a column 8 apart; the other side's presence below
// right) the same: half its offset (2361's -12 or +12 across, its rows 8 apart: 4) and a box of +-2.  Its offset was
// left whole at first: the user's Phantom drawn 12 pixels left of its planet answered a click 24 to the left ("the
// player must click most of the way towards the destination nav point to select the phantom at the source").
PhysPt g_mapConvObj;            // the planet map_conv has just put (its box: map_planet_box)
PhysPt g_mapMarkObj;            // the marker map_mark has just put (its box)
PhysPt g_mapFleetObj;           // the fleet map_fleet has just put (its box)
std::set<PhysPt> g_mapHalved;   // the planets whose place now is the halved one (map_conv's), not the game's own

bool map_from(Bit16u ret) {
    const PhysPt bp = SegPhys(ss) + reg_bp;
    return mem_readw(bp + 2) == ret && (Bit16u)(mem_readw(bp + 4) - g_load) == g_m->objSeg;
}

void map_set_box(PhysPt o, int r) {
    const int a = (Bit16s)mem_readw(o + 0x0A), b = (Bit16s)mem_readw(o + 0x0C);
    mem_writew(o + 0x02, (Bit16u)(a - r));
    mem_writew(o + 0x06, (Bit16u)(a + r));
    mem_writew(o + 0x04, (Bit16u)(b - r));
    mem_writew(o + 0x08, (Bit16u)(b + r));
}

void map_planet_box() {
    const PhysPt o = ds_at((Bit16u)reg_si);
    if (o != g_mapConvObj) return;
    g_mapConvObj = 0;
    map_set_box(o, 3);
}

// The fleet's place (2892: its planet's + 12,-8) and its box, while its planet is at its halved place
void map_fleet(bool box) {
    if (g_mapPass >= 0) return;
    const PhysPt o = ds_at((Bit16u)reg_si);
    if (box) {
        if (o == g_mapFleetObj) map_set_box(o, 2);
        g_mapFleetObj = 0;
        return;
    }
    g_mapFleetObj = 0;
    if (!map_from(g_m->fleetRet) || !g_mapHalved.count(ds_at(mem_readw(o + 0x12)))) return;
    mem_writew(o + 0x0A, (Bit16u)((Bit16s)mem_readw(o + 0x0A) - 6));
    mem_writew(o + 0x0C, (Bit16u)((Bit16s)mem_readw(o + 0x0C) + 4));
    g_mapFleetObj = o;
}

// A marker's place (23A0: its planet's + 2361's offset, just computed) and its box, while its planet is at its halved
// place: half the offset (12 and multiples of 8: exact)
void map_mark(bool box) {
    if (g_mapPass >= 0) return;
    const PhysPt o = ds_at((Bit16u)reg_si);
    if (box) {
        if (o == g_mapMarkObj) map_set_box(o, 2);
        g_mapMarkObj = 0;
        return;
    }
    g_mapMarkObj = 0;
    const PhysPt planet = ds_at(mem_readw(o + 0x10));
    if (!g_mapHalved.count(planet)) return;
    const int px = (Bit16s)mem_readw(planet + 0x0A), py = (Bit16s)mem_readw(planet + 0x0C);
    const int dx = (Bit16s)mem_readw(o + 0x0A) - px, dy = (Bit16s)mem_readw(o + 0x0C) - py;
    mem_writew(o + 0x0A, (Bit16u)(px + dx / 2));
    mem_writew(o + 0x0C, (Bit16u)(py + dy / 2));
    g_mapMarkObj = o;
}

// The registered objects' draws: in a quarter only the map's own (planets, fleets), not the overview and the buttons;
// in the game's own pass each other draw's pixels in the panel are marked (the BEGIN and DONE buttons, the map's
// pointer): the picture shows the game's own there
void map_reg_call() {
    if (g_mapPass < 0 || g_mapPass > 4) return;
    const PhysPt vt = ds_at((Bit16u)reg_bx);
    if ((Bit16u)(mem_readw(vt + 6) - g_load) == g_m->objSeg) return;
    if (g_mapPass <= 3) {
        reg_eip += 3;                   // (past the call; the caller pops)
        return;
    }
    // (every pixel it writes, a black fill over black too: the panel is the marker colour while it draws, then what it
    // left alone is put back)
    map_panel_read(g_mapPre, kPW, 0, 0);
    g_mapPost.assign(kPW * kPH, kMarker);
    map_panel_write(g_mapPost);
    g_mapDiff = true;
}

void map_reg_ret() {
    if (!g_mapDiff) return;
    g_mapDiff = false;
    map_panel_read(g_mapPost, kPW, 0, 0);
    std::vector<Bit8u> &m = g_mapMask[1 - g_mapGen];
    bool any = false;
    for (int i = 0; i < kPW * kPH; i++) {
        if (g_mapPost[i] != kMarker) { m[i] = 1; any = true; }
        else g_mapPost[i] = g_mapPre[i];
    }
    if (any) map_panel_write(g_mapPost);
    else map_panel_write(g_mapPre);
}

// The overview's rectangle: the doubled view's half-sizes for its computation, the result kept inside the overview
void map_overview(bool lines) {
    // (drawn in the game's own pass, 4: the quarters skip it; it was gated off there too at first, so the rectangle
    // stayed the game's own size until 2026-11-23)
    if (!g_mapMode || (g_mapPass >= 0 && g_mapPass <= 3)) {
        if (lines && g_mapOvSet) { g_mapOvSet = false; }
        return;
    }
    const PhysPt di = ds_at((Bit16u)reg_di);
    if (!lines) {
        g_mapOvHalf[0] = mem_readw(di + 0x18);
        g_mapOvHalf[1] = mem_readw(di + 0x1A);
        mem_writew(di + 0x18, (Bit16u)(2 * (Bit16s)g_mapOvHalf[0]));
        mem_writew(di + 0x1A, (Bit16u)(2 * (Bit16s)g_mapOvHalf[1]));
        g_mapOvSet = true;
        return;
    }
    if (!g_mapOvSet) return;
    g_mapOvSet = false;
    mem_writew(di + 0x18, g_mapOvHalf[0]);
    mem_writew(di + 0x1A, g_mapOvHalf[1]);
    const struct { int off, lo, hi; } f[4] = { { 0x20, 0x13, 0x56 }, { 0x28, 0x13, 0x56 }, { 0x24, 0x64, 0x98 }, { 0x2C, 0x64, 0x98 } };
    for (int i = 0; i < 4; i++) {
        const Bit32s v = (Bit32s)mem_readd(di + f[i].off);
        mem_writed(di + f[i].off, (Bit32u)(v < f[i].lo ? f[i].lo : v > f[i].hi ? f[i].hi : v));
    }
}

// A screen refresh's 640x400 picture while the map's is on: the game's screen doubled; in the panel, where the screen
// has the game's own pass's pixel (the newest frame's, else the one before: the screen may show either), the doubled
// view's 2x2 there
void map_compose() {
    const Bit8u *scr = vga.fastmem;
    const int g0 = g_mapGen, g1 = 1 - g_mapGen;
    for (int y = 0; y < H; y++) {
        Bit8u *o0 = &g_out[2 * y * FW], *o1 = o0 + FW;
        const bool rowIn = y >= kPY0 && y < kPY0 + kPH;
        for (int x = 0; x < W; x++) {
            const Bit8u v = scr[y * W + x];
            if (rowIn && x >= kPX0 && x < kPX0 + kPW) {
                const int p = (y - kPY0) * kPW + (x - kPX0);
                const std::vector<Bit8u> *c = NULL;
                if (g_mapHave[g0] && g_mapSnap[g0][p] == v) c = g_mapMask[g0][p] ? NULL : &g_mapComp[g0];
                else if (g_mapHave[g1] && g_mapSnap[g1][p] == v) c = g_mapMask[g1][p] ? NULL : &g_mapComp[g1];
                if (c) {
                    const int u = 2 * (x - kPX0), w = 2 * (y - kPY0);
                    const Bit8u *f0 = &(*c)[w * kMW + u], *f1 = f0 + kMW;
                    o0[2 * x] = f0[0]; o0[2 * x + 1] = f0[1];
                    o1[2 * x] = f1[0]; o1[2 * x + 1] = f1[1];
                    continue;
                }
            }
            o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = v;
        }
    }
}

// The conversion's place (+0A/+0C just computed): from the map's update loop, while the picture is on, at half the
// distance from the middle, where the 640x400 picture has the object (its pixel there, halved: the game's pointer)
void map_conv() {
    if (g_mapPass >= 0) return;
    const PhysPt o = ds_at((Bit16u)reg_si);
    const PhysPt bp = SegPhys(ss) + reg_bp;
    const PhysPt bp0 = SegPhys(ss) + mem_readw(bp);
    if (!g_mapMode || mem_readw(bp + 2) != g_m->convRet || (Bit16u)(mem_readw(bp + 4) - g_load) != g_m->objSeg ||
        mem_readw(bp0 + 2) != g_m->updateRet || (Bit16u)(mem_readw(bp0 + 4) - g_load) != g_m->seg) {
        if (!g_mapHalved.empty()) g_mapHalved.erase(o);     // (the game's own place: its fleet and markers' too)
        return;
    }
    // the object's 640x400 place: kMapX0 + (its place in the doubled view) = kMapX0 + a - 6, kMapY0 + b + 54
    // (a place a on the game's panel is the doubled view's column a - (kPCX - kPW) - kPX0 ... : with the game's middle
    // and the doubled view's the same, column u = a + kPW - kPCX + kPX0 - kPX0 = a - 6; on the screen kMapX0 + u, and
    // the game's pointer half that: (kMapX0 + a - 6) >> 1; likewise down: row v = b + kPH - kPCY = b + 54)
    const int a = (Bit16s)mem_readw(o + 0x0A), b = (Bit16s)mem_readw(o + 0x0C);
    const int X = kMapX0 + a + kPW - kPCX, Y = kMapY0 + b + kPH - kPCY - kPY0 + kPY0;
    mem_writew(o + 0x0A, (Bit16u)(X >> 1));
    mem_writew(o + 0x0C, (Bit16u)(Y >> 1));
    g_mapConvObj = o;
    g_mapHalved.insert(o);
}

// The Move Resources dialog's + and - (the user, 2026-11-23: "increasing resource count for transport or carrier";
// "if there are gaps we can add those": the game's own keys are F1..F8 for the buttons, C and D for a dialog's Cancel
// and Done, none for these arrows).  + is a click on a row's left arrow (a ton from the planet onto the ship), - on
// its right (back to the planet): the row level with the pointer, else the first.  The click is the game's own: a
// press and a release put in the mouse's event queue (seg6A6C: 7 bytes each: kind 1 press / 2 release, the button,
// x in 640ths, y, the buttons held), from the dialog's update (the game's own flow, never inside the mouse's
// interrupt), so the dialog's handler takes it as a click at that arrow, its limits and all, the pointer where it was.
// One a frame; a key held repeats as the keyboard repeats it.
const int kMrRowTop = 0x4C, kMrRows = 6;

bool mr_queue_put(PhysPt ds, Bit8u kind, int x, int y, Bit8u held) {
    const Bit16u start = mem_readw(ds + g_m->qStart), end = mem_readw(ds + g_m->qEnd), size = mem_readw(ds + g_m->qSize);
    const Bit16u head = mem_readw(ds + g_m->qHead), tail = mem_readw(ds + g_m->qTail);
    if (size != 7 || start >= end || (end - start) / size < 4 || head < start || head >= end || tail < start || tail >= end) return false;
    Bit16u next = (Bit16u)(tail + size);
    if (next >= end) next = start;
    if (next == head) return false;     // (full: the next frame)
    const Bit8u ev[7] = { kind, 1, (Bit8u)(x & 0xFF), (Bit8u)(x >> 8), (Bit8u)(y & 0xFF), (Bit8u)(y >> 8), held };
    for (int k = 0; k < 7; k++) mem_writeb(ds + tail + k, ev[k]);
    mem_writew(ds + g_m->qTail, next);
    return true;
}

void mr_update() {
    g_mrAt = PIC_FullIndex();
    g_mapDS = ds_at(0);
    if (g_mrPending.empty()) return;
    const PhysPt ds = g_mapDS;
    const Bit16u head = mem_readw(ds + g_m->qHead), tail = mem_readw(ds + g_m->qTail);
    if (head != tail) return;           // (the game's own events first)
    const int py = (Bit16s)mem_readw(ds_at(g_m->mouseY));
    const int row = py >= kMrRowTop && py < kMrRowTop + 8 * kMrRows ? (py - kMrRowTop) / 8 : 0;
    const int x = 2 * (g_mrPending.front() > 0 ? 0xCA : 0xD2), y = kMrRowTop + 8 * row + 4;
    if (mr_queue_put(ds, 1, x, y, 1) && mr_queue_put(ds, 2, x, y, 0)) {
        g_mrPending.erase(g_mrPending.begin());
    }
}

bool mr_open() { return g_m && g_mrOk && PIC_FullIndex() - g_mrAt < 300.0 && mem_readw(map_view_in(g_mapDS) + 0x2A4); }

// The keys: the main row's = (+, shifted or not) and -, as the keyboard gives them (its repeat while held: the
// emulated keyboard's, 30 a second after half a second); they go on to the game too, which has no use for them here.
// (Not the keypad's: on the strategy screen the game's own update takes a held keypad - (and keypad +, with its
// flag DS:16AC set) to set the side's record's +18h)
bool mr_key(int kbdKey, bool pressed) {
    if (!pressed || (kbdKey != KBD_equals && kbdKey != KBD_minus) || !mr_open()) return false;
    if (g_mrPending.size() < 2) g_mrPending.push_back(kbdKey == KBD_equals ? 1 : -1);
    return false;
}

// The wheel on the strategy map (the user, 2026-11-23: "seamlessly zoom in (2x) and zoom out (normal) using wheel"):
// up, the game's own map (its 320x200 picture doubled); down, one level out (the default).  About the pointer: the
// place under it stays under it (one level out a unit is a 640x400 pixel, the game's own map two), as far as the
// view's limits let it; the pointer off the map: about the middle.  Not while a dialog is over the map.
bool map_wheel(int dy) {
    if (!dy || !g_m || !g_mapCodeOk || !wanted() || PIC_FullIndex() - g_mapFrameAt > 300.0) return false;
    g_mapWheel = dy > 0 ? 1 : -1;
    return true;
}

}  // namespace

void arm_hires_loaded(const char *exe, Bit16u loadSeg) {
    if (g_override) set_override(false, false);
    boost(false);
    g_b = NULL;
    g_pass = -1;
    g_set = false;
    if (!exe) return;
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(kBuilds[0]); i++) {
        if (strcmp(exe, kBuilds[i].exe) == 0) g_b = &kBuilds[i];
    }
    g_load = loadSeg;
    g_codeOk = false;
    g_m = NULL;
    g_mapCodeOk = false;
    g_mapPass = -1;
    g_mapJumped = false;
    g_mapMode = false;
    g_mapHave[0] = g_mapHave[1] = false;
    g_mapTail = g_mapDiff = false;
    g_mapLastOwn = -1e9;
    g_mapZoomIn = false;
    g_mapWheel = 0;
    g_mapFrameAt = g_mrAt = -1e9;
    g_mrOk = false;
    g_mrPending.clear();
    g_mapHalved.clear();
    g_mapConvObj = g_mapMarkObj = g_mapFleetObj = 0;
    for (size_t i = 0; i < sizeof(kMapBuilds) / sizeof(kMapBuilds[0]); i++) {
        if (strcmp(exe, kMapBuilds[i].exe) == 0) g_m = &kMapBuilds[i];
    }
    if (g_m) {
        g_mapCodeOk = map_code_here();
        g_mrOk = g_mapCodeOk && mr_code_here();
        wclog(1, "Armada: %s's strategy map %s; its Move Resources %s", exe, g_mapCodeOk ? "can be drawn at 640x400" :
              "is not as known: the game's own", g_mrOk ? "takes + and -" : "is the game's own");
    }
    if (g_b) wanted();
}

void arm_hires_watch(void (*watch)(Bit16u ip)) {
    now_wanted();
    // (the arrows on the map, the wheel's knowing the map is up, and + and - in Move Resources: high resolution or not)
    if (g_m && g_mapCodeOk) watch(g_m->frame);
    if (g_m && g_mrOk) watch(g_m->mrUpdate);
    if (!g_b || !g_used) return;
    for (int o : { 0, (int)kCleared, (int)kSetup, (int)kDrawn, (int)kCockpit }) watch((Bit16u)(g_b->top + o));
    watch(g_b->starEntry);
    watch(g_b->starLockA);
    watch(g_b->starLockB);
    watch(g_b->bgCall);
    for (int i = 0; i < 2; i++) if (g_b->waitCall[i]) watch(g_b->waitCall[i]);
    watch(g_b->turnEntry);
    watch(g_b->boomEntry);
    watch(g_b->listCall);
    watch(g_b->lineEntry);
    watch(g_b->monoEntry);
    if (calls_on()) for (size_t i = 0; i < sizeof(kLibSeg) / sizeof(kLibSeg[0]); i++) watch(kLibOff[i]);
    if (g_m && g_mapCodeOk) {
        for (Bit16u a : { g_m->loop, g_m->after, g_m->conv, g_m->planetBox, g_m->fleetDone, g_m->fleetBox, g_m->markPlaced, g_m->markBox,
                          g_m->ovSize, g_m->ovLines, g_m->regCall, (Bit16u)(g_m->regCall + 3), g_m->present }) watch(a);
        for (int i = 0; i < 4; i++) watch(g_m->grid[i]);
    }
}

bool arm_hires_hook(Bit32u ip) {
    if (ip > 0xFFFF) return false;
    const Bit16u para = (Bit16u)(SegValue(cs) - g_load);
    if (g_m && g_mapCodeOk && para == g_m->seg && ip == g_m->frame) { map_frame(); return true; }
    if (g_m && g_mrOk && para == g_m->dlgSeg && ip == g_m->mrUpdate) { mr_update(); return true; }
    if (!g_b || !g_used) return false;
    if (g_m && g_mapCodeOk) {
        if (para == g_m->seg) {
            if (ip == g_m->loop) { map_loop(); return true; }
            if (ip == g_m->after) { map_after(); return true; }
            for (int i = 0; i < 4; i++) if (ip == g_m->grid[i]) { map_grid(i); return true; }
            if (ip == g_m->ovSize) { map_overview(false); return true; }
            if (ip == g_m->ovLines) { map_overview(true); return true; }
            if (ip == g_m->regCall) { map_reg_call(); return true; }
            if (ip == g_m->regCall + 3) { map_reg_ret(); return true; }
            if (ip == g_m->present) { map_present(); return true; }
        } else if (para == g_m->objSeg) {
            if (ip == g_m->conv) { map_conv(); return true; }
            if (ip == g_m->planetBox) { map_planet_box(); return true; }
            if (ip == g_m->fleetDone) { map_fleet(false); return true; }
            if (ip == g_m->fleetBox) { map_fleet(true); return true; }
            if (ip == g_m->markPlaced) { map_mark(false); return true; }
            if (ip == g_m->markBox) { map_mark(true); return true; }
        }
    }
    if (para == g_b->frameSeg) {
        if ((Bit16u)ip == g_b->top && g_pass < 0 && !g_jumped) {
            const bool was = g_codeOk;
            g_codeOk = code_here();
            if (g_codeOk != was) {
                wclog(1, "Armada: %s's flight picture %s", g_b->exe,
                      g_codeOk ? "can be drawn at 640x400" : "is not as known: the game's own");
            }
        }
        return frame_hook((Bit16u)ip);
    }
    for (int i = 0; i < 2; i++) {
        if (g_b->waitCall[i] && para == g_b->waitSeg[i] && ip == g_b->waitCall[i]) {
            const PhysPt at = SegPhys(cs) + ip;
            if (mem_readb(at) == 0x9A && mem_readd(at + 0xF) == 0x00017700) wait_idle();
            return true;
        }
    }
    if (g_pass == 5 && level() >= 1) {
        if (para == g_b->lineSeg && ip == g_b->lineEntry) { cockpit_line(); return true; }
        if (para == g_b->monoSeg && ip == g_b->monoEntry) { cockpit_mono(); return true; }
        if (para == g_b->turnSeg && ip == g_b->turnEntry) { cockpit_turn(); return true; }
    }
    if (calls_on() && lib_call(para, (Bit16u)ip)) {
        if (para == g_b->turnSeg && ip == g_b->turnEntry && g_pass >= 0 && g_pass <= 3) turn_entry();
        return true;
    }
    if (g_pass < 0 || g_pass > 4) return false;
    if (para == g_b->turnSeg && ip == g_b->turnEntry) {
        if (g_pass <= 3) turn_entry();
        return true;
    }
    if (para == g_b->boomSeg && ip == g_b->boomEntry) {
        boom_entry();
        return true;
    }
    if (para == g_b->listSeg && ip == g_b->listCall) {
        list_call();
        return true;
    }
    if (para == g_b->starSeg) {
        if (ip == g_b->starEntry) { star_entry(); return true; }
        if (ip == g_b->starLockA || ip == g_b->starLockB) { star_locked(); return true; }
    } else if (para == g_b->bgSeg && ip == g_b->bgCall && g_pass <= 3) {
        bg_sprite();
        return true;
    }
    return false;
}

// On from the first frame composed; off as soon as the screen shows anything but the last one (the game's copy of
// its buffer leaves exactly that between frames: a menu, a message, the end of a battle drawn otherwise), or after a
// second without one
void arm_hires_tick() {
    if (!g_b) {
        if (g_override) set_override(false, false);
        boost(false);
        return;
    }
    now_wanted();
    const double now = PIC_FullIndex(), since = now - g_lastCompose;
    bool want = wanted() && g_codeOk && vga.mode == M_VGA && since < 1000.0;
    static double checked = 0;
    if (want && g_override && since > 40.0 && now - checked > 20.0) {
        checked = now;
        if (memcmp(vga.fastmem, &g_low[0], W * H) != 0) {
            if (getenv("WCARMHIRESDEBUG")) {
                int n = 0, first = -1;
                for (int i = 0; i < W * H; i++) if (vga.fastmem[i] != g_low[i]) { n++; if (first < 0) first = i; }
                wclog(1, "Armada screen: %d pixels not the frame's, the first at %d,%d (%02X, ours %02X), %.0f ms since",
                      n, first % W, first / W, vga.fastmem[first], g_low[first], since);
            }
            want = false;
            g_lastCompose = -1e9;   // (until the next frame composed)
        }
    }
    if (!g_override && since > 250.0) want = false;
    // the strategy map's: on while its frames come (each composes its panel), off a moment after they stop
    const bool map = !want && map_wanted() && g_mapHave[g_mapGen] && now - g_mapLastOwn < 300.0;
    g_mapMode = map;
    set_override(want || map, map);
    boost(want);
}

bool arm_hires_script_item(const std::string &item, double t) {
    // !armmapdump=<name>: the doubled panel and the game's own pass's (the newest) to $WCNET_DUMP_DIR/<name>.ppm and
    // <name>-own.ppm, in the screen's palette (a test aid)
    if (item.compare(0, 12, "!armmapdump=") == 0) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        const std::string base = std::string(dir ? dir : ".") + "/" + item.substr(12);
        const int g = g_mapGen;
        for (int k = 0; k < 2 && g_mapHave[g]; k++) {
            const std::vector<Bit8u> &v = k ? g_mapSnap[g] : g_mapComp[g];
            const int w = k ? kPW : kMW, h = k ? kPH : kMH;
            FILE *f = fopen((base + (k ? "-own.ppm" : ".ppm")).c_str(), "wb");
            if (!f) continue;
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) {
                const Bit8u c = v[i];
                const Bit8u rgb[3] = { (Bit8u)(vga.dac.rgb[c].red << 2), (Bit8u)(vga.dac.rgb[c].green << 2), (Bit8u)(vga.dac.rgb[c].blue << 2) };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        wclog(1, "key script %.1fs: the map's panels to %s.ppm", t, base.c_str());
        return true;
    }
    // !armwheel=<n>: the mouse's wheel (n > 0 up: the game's own map; n < 0 down: one level out)
    if (item.compare(0, 10, "!armwheel=") == 0) {
        const bool took = map_wheel(atoi(item.c_str() + 10));
        wclog(1, "key script %.1fs: the wheel %s%s", t, item.c_str() + 10, took ? "" : " (not the map's)");
        return true;
    }
    if (item == "!armside") {
        g_side = 1;
        return true;
    }
    if (item.compare(0, 10, "!armhires=") == 0) {
        arm_hires_set(item.compare(10, std::string::npos, "space") == 0 ? 1 : atoi(item.c_str() + 10) ? 2 : 0);
        (void)t;
        return true;
    }
    return false;
}

void arm_hires_set(int lvl) {
    level();
    g_level = lvl < 0 ? 0 : lvl > 2 ? 2 : lvl;
    g_mapZoomIn = false;
    now_wanted();
    wclog(1, "Armada: the high-resolution picture %s", g_level == 2 ? "all" : g_level == 1 ? "the space alone" : "off");
}

// The ` key: all -> the space alone -> off -> all (Privateer's order)
void arm_hires_flip() {
    if (g_never) return;
    const int l = level();
    g_level = l == 2 ? 1 : l == 1 ? 0 : 2;
    g_mapZoomIn = false;
    now_wanted();
    wclog(1, "Armada: the high-resolution picture %s", g_level == 2 ? "all" : g_level == 1 ? "the space alone" : "off");
}

int arm_hires_level() { return g_override ? level() : 0; }

bool arm_hires_on() { return g_override; }

bool arm_hires_key(int kbdKey, bool pressed) { return mr_key(kbdKey, pressed); }

bool arm_hires_wheel(int dy) { return map_wheel(dy); }

}  // namespace wc

void wc_arm_hires_vsync() {
    if (!wc::g_override) return;
    if (wc::g_mapMode) wc::map_compose();
    if (!RENDER_StartUpdate()) return;
    for (int y = 0; y < wc::FH; y++) RENDER_DrawLine(&wc::g_out[y * wc::FW]);
    RENDER_EndUpdate(false);
}
