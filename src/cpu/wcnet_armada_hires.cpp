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
// caller, with its arguments (which draws the reticle, the brackets, the radar, the displays)
const Bit16u kLibSeg[] = { 0x5F32, 0x5F91, 0x5F69, 0x5F55, 0x5F51, 0x5FB6, 0x5FB7, 0x5FCB, 0x6493, 0x5E2C, 0x5EE6,
                           0x5D8D, 0x5DE6, 0x5D70 };
const Bit16u kLibOff[] = { 0x0006, 0x0008, 0x0008, 0x0004, 0x000E, 0x0006, 0x000C, 0x0004, 0x0ECC, 0x000E, 0x000E,
                           0x0006, 0x0242, 0x0000 };
int g_calls = -1;
bool calls_on() {
    if (g_calls < 0) g_calls = getenv("WCARMHIRESCALLS") && g_b && !strcmp(g_b->exe, "ARMADA.EXE") ? 1 : 0;
    return g_calls == 1;
}
bool lib_call(Bit16u para, Bit16u ip) {
    for (size_t i = 0; i < sizeof(kLibSeg) / sizeof(kLibSeg[0]); i++) {
        if (para != kLibSeg[i] || ip != kLibOff[i]) continue;
        if (g_pass != 5) return true;
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

void set_override(bool on) {
    if (on == g_override) return;
    g_override = on;
    boost(on);
    if (on) {
        const double fps = vga.draw.delay.vtotal > 0 ? 1000.0 / vga.draw.delay.vtotal : 70.0;
        VGA_SetOverride(true);
        RENDER_SetSize(FW, FH, 8, (float)fps, vga.draw.aspect_ratio, false, false);
        wclog(1, "Armada: the flight's picture is 640x400");
    } else {
        VGA_SetOverride(false);
        wclog(1, "Armada: the picture is the game's own");
    }
}

}  // namespace

void arm_hires_loaded(const char *exe, Bit16u loadSeg) {
    if (g_override) set_override(false);
    g_b = NULL;
    g_pass = -1;
    g_set = false;
    if (!exe) return;
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(kBuilds[0]); i++) {
        if (strcmp(exe, kBuilds[i].exe) == 0) g_b = &kBuilds[i];
    }
    g_load = loadSeg;
    g_codeOk = false;
    if (g_b) wanted();
}

void arm_hires_watch(void (*watch)(Bit16u ip)) {
    now_wanted();
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
}

bool arm_hires_hook(Bit32u ip) {
    if (!g_b || !g_used || ip > 0xFFFF) return false;
    const Bit16u para = (Bit16u)(SegValue(cs) - g_load);
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
        if (g_override) set_override(false);
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
    set_override(want);
}

bool arm_hires_script_item(const std::string &item, double t) {
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
    now_wanted();
    wclog(1, "Armada: the high-resolution picture %s", g_level == 2 ? "all" : g_level == 1 ? "the space alone" : "off");
}

// The ` key: all -> the space alone -> off -> all (Privateer's order)
void arm_hires_flip() {
    if (g_never) return;
    const int l = level();
    g_level = l == 2 ? 1 : l == 1 ? 0 : 2;
    now_wanted();
    wclog(1, "Armada: the high-resolution picture %s", g_level == 2 ? "all" : g_level == 1 ? "the space alone" : "off");
}

int arm_hires_level() { return g_override ? level() : 0; }

bool arm_hires_on() { return g_override; }

}  // namespace wc

void wc_arm_hires_vsync() {
    if (!wc::g_override || !RENDER_StartUpdate()) return;
    for (int y = 0; y < wc::FH; y++) RENDER_DrawLine(&wc::g_out[y * wc::FW]);
    RENDER_EndUpdate(false);
}
