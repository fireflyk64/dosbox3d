/*
 *  Wing Commander 1 at 640 x 400 in flight, each game pixel a block of 2 x 2 (docs/wc-hires.md): Privateer's picture
 *  (wcnet_priv_hires.cpp) brought back to the game its library grew from.
 *
 *  WC.EXE draws through two drivers it loads at its start (GAMEDAT\TM.DRV, the graphics primitives, and SUPERTM.DRV,
 *  the turned and scaled shapes and the span copy; seg091:000F), each in memory of its own, their code segments kept
 *  in its data segment (tmDriverSeg, superTmDriverSeg).  The flight's scene goes into a buffer of the view's size (the
 *  record flightBuffer), every pixel of it by those routines, and reaches the screen by a list of spans (SPANCOPY, at
 *  seg001:05DB): the cockpit, the radar and the screens beside it are drawn on the screen itself and never pass
 *  through the buffer.  The routines are Privateer's library's, an older build: the same records, arguments and shape
 *  format.  So, as there, each call on the flight's buffer is drawn again here from its own arguments, into a picture
 *  of twice the buffer's size (H), and at the span copy H is frozen with what the game shows; at the VGA's frame a
 *  pixel of the spans that still holds what was copied shows H, every other is the game's own, 2 x 2.
 *
 *  The game erases its buffer only where it drew (each object's box, the target box redrawn in the space's colour,
 *  the comm text), after the span copy: H is made anew instead -- the space's colour at the frame's first drawing,
 *  then that frame's calls.  What the game draws there by anything not hooked here (the comm text's letters, the
 *  simulator's clock) is put into H at the span copy as the game's own pixels, where H drew nothing near.
 *
 *  Nothing of the game's is changed, its memory or its drawing: one pilot's display.  The game's memory is read only by
 *  rd8/rd16, which never fault.  Every list has its cap, every argument is bounded before it is used.
 *
 *  Classic Wing Commander first (the user, 2026-11-27: "we must make sure not to break classic WC1 and so that the `
 *  returns to the current mode. that game was the first and is the most fragile"): unless the page's box is ticked
 *  (WCHIRES=1 at the launch, wc_web_wc_hires while running) none of this runs, its places are not watched, and the `
 *  key goes to the game as it always did; once allowed, ` flips between this picture and the game's own, which gives
 *  the VGA back to DOSBox's drawing of it (the override let go).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "dosbox.h"
#include "wcnet_wch.h"
#include "wcnet_wch_font.h"
#include "wcnet_wire.h"
#include "wcnet_priv_font.h"
#include "wcnet_game.h"
#include "wcnet_memory.h"
#include "wcnet_log.h"
#include "wcnet_perf.h"
#include "wc_net.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "regs.h"
#include "render.h"
#include "vga.h"

void VGA_SetOverride(bool vga_override);

namespace wc {

void hooks_game_changed();
void watch_at(Bit16u seg, Bit16u ip);

namespace {

enum { LW = 320, LH = 200, HW = 640, HH = 400 };
enum {
    kMaxShapeSide = 640,            // a shape's width or height
    kMaxShapeBytes = 0x10000,       // what one shape's decoding may read
    kCacheBytes = 8 << 20,          // decoded shapes kept (then all are let go)
    kMaxCached = 4096,
    kMaxSpans = 4096,               // a span list's
    kScreenSeg = 0xA000,
    kStaleMs = 300,                 // a frame copied longer ago is not shown
};

enum Drv { TM, ST };
enum Op { OP_SHAPE, OP_TURN, OP_LINE, OP_RECT, OP_FILL, OP_CLEAR, OP_COPY, OP_PRESENT, OP_GLYPH };

// A place in a driver's code, found from the driver's own table of entries (a word per routine at its start, the
// game's loader copies them into its slots: WC.EXE seg098:0043, WC2.EXE ovr204:003F): the entry plus `delta`.
// `framed`: past the routine's own `push bp / mov bp,sp` and its shape pointer's resolving (EMS: [bp+0C] then holds
// the real pointer), its arguments at [bp+6]; else the routine's first instruction, its arguments at [sp+4].
// `code`: the bytes there, checked.  WC2's SUPERTM.DRV's VGA entry is WC1's byte for byte; its TM.DRV is WC1's with
// the routines it never calls (the triangles, the dissolve, the silhouette) taken out, the rest moved up (SHAPE
// 0E87, LINE 1CE8, RECT 1B6C, FILLRECT 1AC8, CLEAR 1930, COPY 1990, GLYPH 2BFB against WC1's 0E86, 3A12, 3896,
// 37F2, 192F, 198F, 4925).
struct Place { Drv drv; Bit8u slot, delta; Op op; bool framed; Bit8u code[3]; };
const Place kPlaces[] = {
    { TM, 0x0E, 0x35, OP_SHAPE,   true,  { 0xFC, 0x0E, 0x07 } },     // SHAPE (rec, x, y, shapes far, index): the dust, the gunsight, the pointer
    { ST, 0x02, 0x35, OP_TURN,    true,  { 0x8C, 0xC8, 0x8E } },     // TURNSCALE (... index, degrees, scale, flags): every ship, bolt, missile
    { TM, 0x1E, 0x00, OP_LINE,    false, { 0x55, 0x8B, 0xEC } },     // LINE (rec, x0, y0, x1, y1, colour): the target's brackets
    { TM, 0x26, 0x00, OP_RECT,    false, { 0x55, 0x8B, 0xEC } },     // RECT (rec, x0, y0, x1, y1, colour): the locked target's box
    { TM, 0x24, 0x00, OP_FILL,    false, { 0x55, 0x8B, 0xEC } },     // FILLRECT (rec, x0, y0, x1, y1, colour)
    { TM, 0x18, 0x00, OP_CLEAR,   false, { 0x55, 0x8B, 0xEC } },     // CLEAR (rec, colour)
    { TM, 0x16, 0x00, OP_COPY,    false, { 0x55, 0x8B, 0xEC } },     // COPY (src rec, dst rec): the outside views' way to the screen
    { ST, 0x08, 0x00, OP_PRESENT, false, { 0x55, 0x8B, 0xEC } },     // SPANCOPY (src rec, dst rec, span list near): the cockpit views'
    { TM, 0x02, 0x00, OP_GLYPH,   false, { 0x2E, 0x83, 0x3E } },     // GLYPH (registers): every letter of every text
};
enum { kPlaceCount = sizeof(kPlaces) / sizeof(kPlaces[0]) };
Bit16u g_placeIp[kPlaceCount];      // where each is in the drivers loaded (0: not watched)

struct Target { Bit16u seg, rowTable; int x0, y0, x1, y1; };

struct Shape {
    int w, h, left, top;            // the anchor is at (left, top)
    std::vector<Bit16s> px;         // -1 where nothing is drawn
};

Bit8u g_hi[HW * HH];                // H, being drawn (the buffer's coordinates, twice)
Bit8u g_drawn[LW * LH];             // a game pixel of the buffer H drew in this frame
Bit8u g_shownHi[HW * HH];           // at the last span copy: the screen's spans from H
Bit8u g_shownLow[LW * LH];          // ... what the game put there
Bit8u g_window[LW * LH];            // ... where they are
Bit8u g_out[HW * HH];
bool g_fresh = true;                // a span copy since H was last drawn in: H is made anew at the next drawing
bool g_haveFrame = false;
Bit32u g_frameSerial = 0;
double g_presentMs = -1e9;

std::vector<std::string> g_frameOps;   // (test aid: this frame's calls, for WCHIRESDUMP)
std::map<Bit64u, Shape> g_cache;
size_t g_cacheBytes = 0;

int g_allowed = -1;                 // the page's box (WCHIRES=1 at the launch, wc_web_wc_hires since): -1 not read yet
bool g_never = false;               // WCHIRES=off: nothing of this, ever
int g_level = 0;                    // what shows: 2 all, 1 the space alone, 0 the game's own (the ` key cycles them)
bool g_override = false;
Bit16u g_watchedTm = 0, g_watchedSt = 0;   // the drivers' segments the places are watched in (0: none watched)
bool g_onlyOurs[0x10000];           // an IP watched for these places alone
Bit32u g_markerDraws = 0;           // the gunsight and lock cross drawn as line art (the page's test asks)
int g_lettersShown = 0, g_artsShown = 0, g_labelsShown = 0;   // in the last picture made: the user's letters, the VDUs'
                                                            // wireframes, the KPS / SET labels (the page's test)
double g_costMs = 0;                // wall ms in the drawing (call, compose): the page's meter

void read_env() {
    if (g_allowed >= 0) return;
    const char *env = getenv("WCHIRES");
    g_never = env && !strcmp(env, "off");
    // (WCHIRES=1 or all: everything; space: the ships, the reticle and the target boxes, the cockpit's text the game's)
    const bool space = env && !strcmp(env, "space");
    g_allowed = !g_never && env && (space || !strcmp(env, "1") || !strcmp(env, "all"));
    g_level = !g_allowed ? 0 : space ? 1 : 2;
}

bool usable() {
    return ((is_wc1() && (g_build == BUILD_WC1 || g_build == BUILD_SM2)) ||
            (is_wc2() && (g_build == BUILD_WC2 || g_build == BUILD_SO1 || g_build == BUILD_SO2))) && DS &&
           ds::known(ds::tmDriverSeg) && ds::known(ds::superTmDriverSeg) && ds::known(ds::flightBuffer);
}

// The game's memory as a real-mode program reaches it: 0 for anywhere else.
enum { kReach = 0x10FFF0 };
inline Bit8u rd8p(PhysPt a) {
    Bit8u v = 0;
    return a < (PhysPt)kReach && !mem_readb_checked(a, &v) ? v : 0;
}
inline Bit16u rd16p(PhysPt a) { return (Bit16u)(rd8p(a) | rd8p(a + 1) << 8); }
inline Bit32u rd32p(PhysPt a) { return rd16p(a) | (Bit32u)rd16p(a + 2) << 16; }
inline Bit16u dsw(Bit16u off) { return rd16p(PhysMake(DS, off)); }

bool g_framed = false;
inline Bit16s arg16(int i) {
    return g_framed ? (Bit16s)rd16p(SegPhys(ss) + (Bit16u)(reg_bp + 6 + 2 * i))
                    : (Bit16s)rd16p(SegPhys(ss) + (Bit16u)(reg_sp + 4 + 2 * i));
}

bool read_target(Bit16u at, Target &t) {
    t.seg = dsw(at);
    t.rowTable = dsw((Bit16u)(at + 2));
    t.x0 = (Bit16s)dsw((Bit16u)(at + 4));
    t.y0 = (Bit16s)dsw((Bit16u)(at + 6));
    t.x1 = (Bit16s)dsw((Bit16u)(at + 8));
    t.y1 = (Bit16s)dsw((Bit16u)(at + 10));
    if (t.x0 < 0) t.x0 = 0;
    if (t.y0 < 0) t.y0 = 0;
    if (t.x1 > LW - 1) t.x1 = LW - 1;
    if (t.y1 > LH - 1) t.y1 = LH - 1;
    return t.x0 <= t.x1 && t.y0 <= t.y1;
}

// (the comm text draws through a copy of the record on the stack: the buffer is known by its memory, not the record)
inline bool flights_buffer(const Target &t) {
    return t.seg && t.seg == dsw(ds::flightBuffer) && t.rowTable == dsw((Bit16u)(ds::flightBuffer + 2));
}

inline PhysPt pixel_at(const Target &t, int x, int y) {
    return PhysMake(t.seg, (Bit16u)(dsw((Bit16u)(t.rowTable + 2 * y)) + x));
}

// ---- H's drawing: a game pixel is the block 2x .. 2x+1, 2y .. 2y+1 ------------------------------------------------

void make_fresh() {
    if (!g_fresh) return;
    g_fresh = false;
    memset(g_hi, rd8p(PhysMake(DS, ds::spaceColour)), sizeof(g_hi));
    memset(g_drawn, 0, sizeof(g_drawn));
}

inline void block(int x, int y, Bit8u c) {
    Bit8u *p = g_hi + 2 * y * HW + 2 * x;
    p[0] = p[1] = p[HW] = p[HW + 1] = c;
    g_drawn[y * LW + x] = 1;
}

// A drawing caught in a canvas of its own instead of the picture, to be made a mirror image of itself before it is
// shown (capture_show, Privateer's): the reticle's.
struct Capture { bool on; int X0, Y0, W, H; std::vector<Bit16s> px; };
Capture g_cap = { false, 0, 0, 0, 0, std::vector<Bit16s>() };

inline void plot(const Target &t, int X, int Y, Bit8u c) {
    if (g_cap.on) {                     // (caught: into the canvas, unclipped -- the copy to the picture is clipped)
        const int x = X - g_cap.X0, y = Y - g_cap.Y0;
        if (x >= 0 && y >= 0 && x < g_cap.W && y < g_cap.H) g_cap.px[y * g_cap.W + x] = c;
        return;
    }
    if (X < 2 * t.x0 || X >= 2 * (t.x1 + 1) || Y < 2 * t.y0 || Y >= 2 * (t.y1 + 1)) return;
    g_hi[Y * HW + X] = c;
    g_drawn[(Y >> 1) * LW + (X >> 1)] = 1;
}

const double kSy = 2.0;                 // (H's rows to a game row)

// One fine pixel wide, between two fine pixels (clipped by plot).
void line_fine(const Target &t, int X, int Y, int Xe, int Ye, Bit8u c) {
    const int dx = abs(Xe - X), dy = -abs(Ye - Y), sx = X < Xe ? 1 : -1, sy = Y < Ye ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(t, X, Y, c);
        if (X == Xe && Y == Ye) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; X += sx; }
        if (e2 <= dx) { err += dx; Y += sy; }
    }
}

void fill(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 > x1) { const int s = x0; x0 = x1; x1 = s; }
    if (y0 > y1) { const int s = y0; y0 = y1; y1 = s; }
    if (x0 < t.x0) x0 = t.x0;
    if (y0 < t.y0) y0 = t.y0;
    if (x1 > t.x1) x1 = t.x1;
    if (y1 > t.y1) y1 = t.y1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) block(x, y, c);
    }
}

// One fine pixel wide, between the middles of the game's pixels (Privateer's line, 2x + 1).
void line(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 < -2000 || x0 > 2000 || x1 < -2000 || x1 > 2000 || y0 < -2000 || y0 > 2000 || y1 < -2000 || y1 > 2000) return;
    line_fine(t, 2 * x0 + 1, 2 * y0 + 1, 2 * x1 + 1, 2 * y1 + 1, c);
}

// ---- the game's shapes (Privateer's format and decoding: wcnet_priv_hires.cpp) --------------------------------------

void decode_rows(PhysPt p, const PhysPt end, const int x1, const int y1, Shape &s) {
    const int w = s.w, h = s.h;
    while (p + 6 <= end) {
        const Bit16u head = rd16p(p);
        if (!head) break;
        int x = (Bit16s)rd16p(p + 2) + x1;
        const int y = (Bit16s)rd16p(p + 4) + y1;
        p += 6;
        int n = head >> 1;
        const bool inside = y >= 0 && y < h;
        Bit16s *row = inside ? &s.px[y * w] : NULL;
        if (!(head & 1)) {
            for (; n > 0 && p < end; n--, x++, p++) {
                if (inside && x >= 0 && x < w) row[x] = rd8p(p);
            }
            continue;
        }
        while (n > 0 && p < end) {
            const Bit8u sub = rd8p(p++);
            int run = sub >> 1;
            if (!run) return;        // (not a shape: what there is)
            n -= run;
            if (sub & 1) {
                const Bit8u c = rd8p(p++);
                for (; run; run--, x++) {
                    if (inside && x >= 0 && x < w) row[x] = c;
                }
            } else {
                for (; run && p < end; run--, x++, p++) {
                    if (inside && x >= 0 && x < w) row[x] = rd8p(p);
                }
            }
        }
    }
}

// A table: a dword (its size), then a dword offset per shape; a shape: its extents right, left, top, bottom from the
// anchor, then its runs.  (WC.EXE's TM.DRV 0E86: valid while 4 (index + 1) is below the first offset.)
const Shape *shape_of(Bit16u off, Bit16u seg, int index) {
    if (index < 0 || index > 4095) return NULL;
    const PhysPt file = PhysMake(seg, off);
    const Bit32u table = rd32p(file + 4) & 0xFFFF;
    if ((Bit32u)(4 * (index + 1)) >= table) return NULL;
    const PhysPt at = file + (rd32p(file + 4 + 4 * index) & 0xFFFFF);
    const int x2 = (Bit16s)rd16p(at), x1 = (Bit16s)rd16p(at + 2);
    const int y1 = (Bit16s)rd16p(at + 4), y2 = (Bit16s)rd16p(at + 6);
    const int w = x1 + x2 + 1, h = y1 + y2 + 1;
    if (w <= 0 || h <= 0 || w > kMaxShapeSide || h > kMaxShapeSide) return NULL;
    // (EMS maps other shapes under the same address: the first bytes say which it is)
    Bit32u stamp = 2166136261u;
    for (PhysPt i = 0; i < 64; i += 4) stamp = (stamp ^ rd32p(at + i)) * 16777619u;
    for (PhysPt i = 64; i < (PhysPt)(w * h / 4) && i < 4096; i += 124) stamp = (stamp ^ rd32p(at + i)) * 16777619u;
    const Bit64u key = ((Bit64u)at << 32) | stamp;
    std::map<Bit64u, Shape>::iterator found = g_cache.find(key);
    if (found != g_cache.end()) return &found->second;
    if (g_cache.size() >= kMaxCached || g_cacheBytes + 2u * w * h > kCacheBytes) {
        g_cache.clear();
        g_cacheBytes = 0;
    }
    Shape &s = g_cache[key];
    g_cacheBytes += 2u * w * h;
    s.w = w;
    s.h = h;
    s.left = x1;
    s.top = y1;
    s.px.assign(w * h, (Bit16s)-1);
    decode_rows(at + 8, at + kMaxShapeBytes, x1, y1, s);
    return &s;
}

void shape_blocks(const Target &t, const Shape &s, int x, int y) {
    const int left = x - s.left, top = y - s.top;
    for (int v = 0; v < s.h; v++) {
        const int py = top + v;
        if (py < t.y0 || py > t.y1) continue;
        const Bit16s *row = &s.px[v * s.w];
        for (int u = 0; u < s.w; u++) {
            const int px = left + u;
            if (row[u] >= 0 && px >= t.x0 && px <= t.x1) block(px, py, (Bit8u)row[u]);
        }
    }
}

// ---- the reticle: Privateer's (wcnet_priv_hires.cpp), the gunsight and the lock cross drawn as one-pixel line art ----

Bit32u shape_hash(const Shape &s) {
    Bit32u h = 2166136261u;
    const int head[4] = { s.w, s.h, s.left, s.top };
    for (int i = 0; i < 4; i++) h = (h ^ (Bit32u)head[i]) * 16777619u;
    for (size_t i = 0; i < s.px.size(); i++) h = (h ^ (Bit16u)s.px[i]) * 16777619u;
    return h;
}

struct Ring { double cx, cy, a, b; int colour; };
struct Rings {
    Bit32u hash;
    std::vector<Ring> rings;
    std::vector<Bit8u> on;          // per pixel of the shape: 1 + the ring's index it belongs to, 0 none
};
enum { kRingCache = 16, kMaxRings = 24, kMaxPiecePixels = 4096, kRingSectors = 48 };
Bit32u shape_hash(const Shape &s);

// M x = the last column, four unknowns (Gauss, the largest pivot): false if it has no single answer.
bool solve4(double M[4][5], double x[4]) {
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) {
            if (fabs(M[r][c]) > fabs(M[p][c])) p = r;
        }
        if (fabs(M[p][c]) < 1e-9) return false;
        for (int k = 0; k < 5; k++) std::swap(M[c][k], M[p][k]);
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int k = c; k < 5; k++) M[r][k] -= f * M[c][k];
        }
    }
    for (int i = 0; i < 4; i++) x[i] = M[i][4] / M[i][i];
    return true;
}

void rings_find(const Shape &s, int ground, Rings &out) {
    const int w = s.w, h = s.h;
    out.rings.clear();
    out.on.assign(w * h, 0);
    std::vector<Bit8u> seen(w * h, 0);
    std::vector<int> piece, stack;
    for (int start = 0; start < w * h && (int)out.rings.size() < kMaxRings; start++) {
        const int colour = s.px[start];
        if (colour < 0 || colour == ground || seen[start]) continue;
        // the piece: this colour's pixels 8-connected to it
        piece.clear();
        stack.assign(1, start);
        seen[start] = 1;
        while (!stack.empty() && (int)piece.size() < kMaxPiecePixels) {
            const int i = stack.back();
            stack.pop_back();
            piece.push_back(i);
            const int u = i % w, v = i / w;
            for (int dv = -1; dv <= 1; dv++) {
                for (int du = -1; du <= 1; du++) {
                    const int nu = u + du, nv = v + dv;
                    if (nu < 0 || nv < 0 || nu >= w || nv >= h) continue;
                    const int j = nv * w + nu;
                    if (!seen[j] && s.px[j] == colour) {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
        }
        // rings in it, the biggest first, each found taken out before the next look (at most four a piece)
        for (int round = 0; round < 4 && (int)out.rings.size() < kMaxRings; round++) {
            std::vector<int> left;
            for (size_t k = 0; k < piece.size(); k++) {
                if (!out.on[piece[k]]) left.push_back(piece[k]);
            }
            if (left.size() < 12) break;
            int u0 = w, u1 = -1, v0 = h, v1 = -1;
            for (size_t k = 0; k < left.size(); k++) {
                const int u = left[k] % w, v = left[k] / w;
                u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
            }
            const double cx = (u0 + u1) / 2.0, cy = (v0 + v1) / 2.0;
            double bestQ = 1, bestR = 0;
            int bestN = 0;
            std::vector<double> r(left.size());
            for (int qi = 0; qi <= 50; qi++) {
                const double q = 0.6 + qi * 0.02;           // (height over width)
                for (size_t k = 0; k < left.size(); k++) {
                    const double du = left[k] % w - cx, dv = (left[k] / w - cy) / q;
                    r[k] = sqrt(du * du + dv * dv);
                }
                std::sort(r.begin(), r.end());
                for (size_t a = 0, b = 0; a < r.size(); a++) {
                    while (b < r.size() && r[b] - r[a] <= 1.2) b++;
                    if ((int)(b - a) > bestN) {
                        bestN = (int)(b - a);
                        bestQ = q;
                        bestR = (r[a] + r[b - 1]) / 2;
                    }
                }
            }
            if (bestR < 2.5 || bestR * bestQ < 2.5) break;
            // the ellipse itself, fitted: A du^2 + B dv^2 + C du + D dv = 1 by least squares over the pixels near the last
            // one (within a pixel, then 0.8), four times.  (Its pixels' box was taken first: the crosshair's ticks lie on
            // its axes, and the box made its circle of 9 an ellipse of 9 by 10 that most of its pixels missed: the user,
            // "The targeting reticle is still wobbly back and forth instead of a beautiful circle like the radar".)
            double ex = cx, ey = cy, ea = bestR, eb = bestR * bestQ;
            bool fitted = true;
            for (int it = 0; it < 4 && fitted; it++) {
                const double tol = it ? 0.8 : 1.0;
                double M[4][5] = { { 0 } };
                int n = 0;
                for (size_t k = 0; k < left.size(); k++) {
                    const double du = left[k] % w - ex, dv = left[k] / w - ey;
                    const double rho = sqrt((du / ea) * (du / ea) + (dv / eb) * (dv / eb));
                    if (rho < 1e-6 || sqrt(du * du + dv * dv) * fabs(1 - 1 / rho) > tol) continue;
                    const double f[4] = { du * du, dv * dv, du, dv };
                    for (int i = 0; i < 4; i++) {
                        M[i][4] += f[i];
                        for (int j = 0; j < 4; j++) M[i][j] += f[i] * f[j];
                    }
                    n++;
                }
                double sol[4];
                fitted = n >= 8 && solve4(M, sol) && sol[0] > 0 && sol[1] > 0;
                if (!fitted) break;
                ex -= sol[2] / (2 * sol[0]);
                ey -= sol[3] / (2 * sol[1]);
                const double K = 1 + sol[2] * sol[2] / (4 * sol[0]) + sol[3] * sol[3] / (4 * sol[1]);
                fitted = K > 0;
                if (!fitted) break;
                ea = sqrt(K / sol[0]);
                eb = sqrt(K / sol[1]);
                fitted = ea < w && eb < h && fabs(ex - cx) < w && fabs(ey - cy) < h;
            }
            if (!fitted) break;
            Ring g = { ex, ey, ea, eb, colour };
            if (g.a < 2.5 || g.b < 2.5) break;
            // the pixels near that ellipse (within 0.8 of a pixel along its radius), and how far round they go: of the
            // points along it, those with one of them within a pixel
            std::vector<int> on;
            std::vector<Bit8u> mark(w * h, 0);
            for (size_t k = 0; k < left.size(); k++) {
                const double du = left[k] % w - g.cx, dv = left[k] / w - g.cy;
                const double rho = sqrt((du / g.a) * (du / g.a) + (dv / g.b) * (dv / g.b));
                if (rho < 1e-6 || sqrt(du * du + dv * dv) * fabs(1 - 1 / rho) > 0.8) continue;
                on.push_back(left[k]);
                mark[left[k]] = 1;
            }
            int covered = 0;
            for (int k = 0; k < kRingSectors; k++) {
                const double th = k * 6.283185307179586 / kRingSectors;
                const int cu = (int)floor(g.cx + g.a * cos(th) + 0.5), cv = (int)floor(g.cy + g.b * sin(th) + 0.5);
                bool near = false;
                for (int dv = -1; dv <= 1 && !near; dv++) {
                    for (int du = -1; du <= 1 && !near; du++) {
                        const int u = cu + du, v = cv + dv;
                        near = u >= 0 && v >= 0 && u < w && v < h && mark[v * w + u];
                    }
                }
                covered += near;
            }
            if (covered * 100 < kRingSectors * 80) break;
            out.rings.push_back(g);
            for (size_t k = 0; k < on.size(); k++) out.on[on[k]] = (Bit8u)out.rings.size();
        }
    }
}

const Rings &rings_of(const Shape &s, int ground) {
    static Rings cache[kRingCache];
    static int used = 0, next = 0;
    const Bit32u hash = shape_hash(s) ^ (Bit32u)(ground + 1) * 0x9E3779B9u;
    for (int i = 0; i < used; i++) {
        if (cache[i].hash == hash && cache[i].on.size() == s.px.size()) return cache[i];
    }
    Rings &r = cache[next];
    next = (next + 1) % kRingCache;
    if (used < kRingCache) used++;
    r.hash = hash;
    rings_find(s, ground, r);
    return r;
}

// The rings drawn: each one fine pixel wide along its ellipse, where the game's ring has a pixel within one of it.  The
// ellipse's fine pixels in order round it, the corner of any L of three left out (the curve 8-connected, one wide).
void rings_draw(const Target &t, const Shape &s, const Rings &r, int left, int top, int ground) {
    std::vector<int> px, py;
    for (size_t k = 0; k < r.rings.size(); k++) {
        const Ring &g = r.rings[k];
        const double fx = 2 * (left + g.cx) + 1, fy = kSy * (top + g.cy) + kSy / 2, A = 2 * g.a, B = kSy * g.b;
        const int n = 16 + (int)(8 * (A > B ? A : B));
        px.clear();
        py.clear();
        auto flush = [&]() {
            // (the corner of an L of three dropped -- judged against the pixels kept, not the ellipse's next point:
            //  two corners in a row had left a gap, the ring dotted on its steep arcs; the gaps then stepped across)
            std::vector<int> kx, ky;
            for (size_t i = 0; i < px.size(); i++) {
                const size_t n = kx.size();
                if (n >= 2 && abs(px[i] - kx[n - 2]) == 1 && abs(py[i] - ky[n - 2]) == 1) {
                    kx.pop_back();
                    ky.pop_back();
                }
                kx.push_back(px[i]);
                ky.push_back(py[i]);
            }
            for (size_t i = 0; i < kx.size(); i++) {
                if (i > 0 && (abs(kx[i] - kx[i - 1]) > 1 || abs(ky[i] - ky[i - 1]) > 1)) {
                    line_fine(t, kx[i - 1], ky[i - 1], kx[i], ky[i], (Bit8u)g.colour);
                }
                plot(t, kx[i], ky[i], (Bit8u)g.colour);
            }
            px.clear();
            py.clear();
        };
        for (int i = 0; i <= n; i++) {
            const double th = i * 6.283185307179586 / n;
            const double cu = g.cx + g.a * cos(th), cv = g.cy + g.b * sin(th);
            bool near = false;
            for (int dv = -1; dv <= 1 && !near; dv++) {
                for (int du = -1; du <= 1 && !near; du++) {
                    const int u = (int)floor(cu + 0.5) + du, v = (int)floor(cv + 0.5) + dv;
                    near = u >= 0 && v >= 0 && u < s.w && v < s.h && r.on[v * s.w + u] == k + 1;
                }
            }
            if (!near) {
                // (where two rings touch, a pixel they share is one ring's colour: the other passes over any ink there)
                const int u = (int)floor(cu + 0.5), v = (int)floor(cv + 0.5);
                near = u >= 0 && v >= 0 && u < s.w && v < s.h && s.px[v * s.w + u] >= 0 && s.px[v * s.w + u] != ground;
            }
            if (!near) {
                flush();
                continue;
            }
            const int X = (int)floor(fx + A * cos(th) + 0.5), Y = (int)floor(fy + B * sin(th) + 0.5);
            if (!px.empty() && X == px.back() && Y == py.back()) continue;
            px.push_back(X);
            py.push_back(Y);
        }
        flush();
    }
}

struct Blit { Bit32u key; int w, h; std::vector<Bit32u> px; };
Blit g_blit[8];
int g_blitUsed = 0, g_blitNext = 0;

bool blit_cached(const Target &t, const Shape &s, Bit32u key, int left, int top) {
    for (int i = 0; i < g_blitUsed; i++) {
        const Blit &b = g_blit[i];
        if (b.key != key || b.w != s.w || b.h != s.h) continue;
        const int X0 = 2 * left - 4, Y0 = 2 * top - 4;
        for (size_t k = 0; k < b.px.size(); k++) {
            const Bit32u v = b.px[k];
            plot(t, X0 + (int)((v >> 8) & 0xFF), Y0 + (int)(v >> 16), (Bit8u)(v & 0xFF));
        }
        return true;
    }
    return false;
}

void capture_begin(const Shape &s, int left, int top) {
    const int M = 4;
    g_cap.X0 = 2 * left - M;
    g_cap.Y0 = 2 * top - M;
    g_cap.W = 2 * s.w + 2 * M;
    g_cap.H = 2 * s.h + 2 * M;
    g_cap.px.assign((size_t)g_cap.W * g_cap.H, (Bit16s)-1);
    g_cap.on = true;
}

void capture_show(const Target &t, const Shape &s, int left, int top, Bit32u key) {
    g_cap.on = false;
    const int w = s.w, h = s.h, W = g_cap.W, H = g_cap.H;
    auto col = [&](int u, int v) { return u >= 0 && v >= 0 && u < w && v < h ? (int)s.px[v * w + u] : -1; };
    int lit = 0;
    for (int i = 0; i < w * h; i++) lit += s.px[i] >= 0;
    for (int pass = 0; pass < 2 && lit > 0; pass++) {
        const bool across = pass == 0;
        const int n = across ? w : h;
        int best = -1, bestAgree = -1;
        for (int S = n - 9; S <= n + 7; S++) {
            int agree = 0;
            for (int v = 0; v < h; v++) {
                for (int u = 0; u < w; u++) {
                    const int c = col(u, v);
                    if (c >= 0 && c == (across ? col(S - u, v) : col(u, S - v))) agree++;
                }
            }
            if (agree > bestAgree) { bestAgree = agree; best = S; }
        }
        if (bestAgree * 100 < lit * 60) continue;
        auto same = [&](int u, int v) {             // (the art's 3 x 3 about (u, v) the same as about its mirror)
            for (int dv = -1; dv <= 1; dv++) {
                for (int du = -1; du <= 1; du++) {
                    const int m = across ? col(best - u - du, v + dv) : col(u + du, best - v - dv);
                    if (col(u + du, v + dv) != m) return false;
                }
            }
            return true;
        };
        const int off = 4;                          // (the canvas's margin: fine x from the shape's left is x - off)
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const int fx = x - off, fy = y - off;   // (from the shape's corner, fine)
                const int f = across ? fx : fy, m = 2 * best + 2 - f;
                if (f <= best + 1) continue;        // (the left or top half: the source; best + 1 the axis itself)
                const int mx = across ? m + off : x, my = across ? y : m + off;
                if (mx < 0 || my < 0 || mx >= W || my >= H) continue;
                const int u = fx >= 0 ? fx / 2 : -1 - (-fx - 1) / 2, v = fy >= 0 ? fy / 2 : -1 - (-fy - 1) / 2;
                if (!same(u, v)) continue;
                g_cap.px[y * W + x] = g_cap.px[my * W + mx];
            }
        }
    }
    Blit &b = g_blit[g_blitNext];               // (kept, then shown as every later frame is)
    g_blitNext = (g_blitNext + 1) % 8;
    if (g_blitUsed < 8) g_blitUsed++;
    b.key = key;
    b.w = w;
    b.h = h;
    b.px.clear();
    for (int y = 0; y < H && y < 256; y++) {
        for (int x = 0; x < W && x < 256; x++) {
            const Bit16s c = g_cap.px[y * W + x];
            if (c >= 0) b.px.push_back(((Bit32u)y << 16) | ((Bit32u)x << 8) | (Bit8u)c);
        }
    }
    blit_cached(t, s, key, left, top);
}

void shape_lines(const Target &t, const Shape &s, int x, int y) {
    static const int kDx[4] = { 1, 0, 1, -1 }, kDy[4] = { 0, 1, 1, 1 };
    g_markerDraws++;
    const int left = x - s.left, top = y - s.top;
    const Bit32u key = shape_hash(s);
    if (blit_cached(t, s, key, left, top)) return;
    const Rings &r = rings_of(s, -1);
    capture_begin(s, left, top);
    rings_draw(t, s, r, left, top, -1);
    for (int v = 0; v < s.h; v++) {
        for (int u = 0; u < s.w; u++) {
            const int c = s.px[v * s.w + u];
            if (c < 0) continue;
            const bool ring = r.on[v * s.w + u] != 0;
            if (!ring) line(t, left + u, top + v, left + u, top + v, (Bit8u)c);
            for (int k = 0; k < 4; k++) {
                const int nu = u + kDx[k], nv = v + kDy[k];
                if (nu < 0 || nu >= s.w || nv >= s.h || s.px[nv * s.w + nu] < 0) continue;
                if (ring && r.on[nv * s.w + nu]) continue;
                if (kDx[k] && kDy[k] && (s.px[v * s.w + nu] >= 0 || s.px[nv * s.w + u] >= 0)) continue;
                line(t, left + u, top + v, left + nu, top + nv, (Bit8u)c);
            }
        }
    }
    capture_show(t, s, left, top, key);
}

// Scaled and turned as the game's blitters do (mirrored first, then turned clockwise, in the game's pixels), sampled
// at each fine pixel's middle (Privateer's shape_scaled).
void shape_scaled(const Target &t, const Shape &s, int x, int y, double sc, int degrees, bool fx, bool fy, bool corner) {
    if (sc <= 0 || sc > 64) return;
    const double w = degrees * 3.141592653589793 / 180.0, c = cos(w), n = sin(w);
    const double mx = fx ? -1 : 1, my = fy ? -1 : 1;
    double lox = 1e9, hix = -1e9, loy = 1e9, hiy = -1e9;
    for (int k = 0; k < 4; k++) {
        const double u = ((k & 1 ? s.w : 0) - 0.5 - s.left) * sc * mx, v = ((k & 2 ? s.h : 0) - 0.5 - s.top) * sc * my;
        const double px = x + 0.5 + u * c - v * n, py = y + 0.5 + u * n + v * c;
        if (px < lox) lox = px;
        if (px > hix) hix = px;
        if (py < loy) loy = py;
        if (py > hiy) hiy = py;
    }
    if (lox < t.x0) lox = t.x0;
    if (hix > t.x1 + 1) hix = t.x1 + 1;
    if (loy < t.y0) loy = t.y0;
    if (hiy > t.y1 + 1) hiy = t.y1 + 1;
    if (lox >= hix || loy >= hiy) return;
    int X0 = (int)(lox * 2), X1 = (int)ceil(hix * 2), Y0 = (int)(loy * 2), Y1 = (int)ceil(hiy * 2);
    if (X0 < 2 * t.x0) X0 = 2 * t.x0;
    if (X1 > 2 * (t.x1 + 1)) X1 = 2 * (t.x1 + 1);
    if (Y0 < 2 * t.y0) Y0 = 2 * t.y0;
    if (Y1 > 2 * (t.y1 + 1)) Y1 = 2 * (t.y1 + 1);
    const Bit32s du = (Bit32s)floor(c * 0.5 * mx / sc * 65536 + 0.5), dv = (Bit32s)floor(-n * 0.5 * my / sc * 65536 + 0.5);
    const Bit16s *src = &s.px[0];
    for (int Y = Y0; Y < Y1; Y++) {
        // (corner: a source pixel u covers [x0 + u k, x0 + (u + 1) k), x0 = x - left k, as the game's magnifier places it;
        // else each fine pixel's middle is sampled about the pixel's middle, Privateer's way)
        // (a mirrored axis: the game takes the whole source step from the anchor first, then mirrors it --
        // u = left - floor(e / k) -- so the anchor's own pixel stays where it was: + 1 - 1/65536 before the floor)
        const double h = corner ? 0 : 0.5;
        const double gx = (X0 + 0.5) / 2 - x - h, gy = (Y + 0.5) / 2 - y - h;
        const double fu = corner && fx ? 1 - 1.0 / 65536 : h, fv = corner && fy ? 1 - 1.0 / 65536 : h;
        const double u0 = (gx * c + gy * n) * mx / sc + fu + s.left, v0 = (-gx * n + gy * c) * my / sc + fv + s.top;
        if (fabs(u0) > 16000 || fabs(v0) > 16000) continue;
        Bit32s u = (Bit32s)floor(u0 * 65536), v = (Bit32s)floor(v0 * 65536);
        Bit8u *out = g_hi + Y * HW;
        for (int X = X0; X < X1; X++, u += du, v += dv) {
            const int iu = u >> 16, iv = v >> 16;
            if ((unsigned)iu >= (unsigned)s.w || (unsigned)iv >= (unsigned)s.h) continue;
            const int colour = src[iv * s.w + iu];
            if (colour < 0) continue;
            out[X] = (Bit8u)colour;
            g_drawn[(Y >> 1) * LW + (X >> 1)] = 1;
        }
    }
}

// ---- the span copy: H frozen with what the game shows ------------------------------------------------------------------

// What the game drew in its buffer by anything not hooked here (the comm text's letters, the simulator's clock): its
// own pixels, 2 x 2, where H drew nothing in the 3 x 3 game pixels about them.
void fill_in(const Target &src) {
    const Bit8u space = rd8p(PhysMake(DS, ds::spaceColour));
    for (int y = src.y0; y <= src.y1; y++) {
        const PhysPt row = pixel_at(src, 0, y);
        for (int x = src.x0; x <= src.x1; x++) {
            const Bit8u c = rd8p(row + x);
            if (c == space) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; dy++) {
                const int yy = y + dy;
                if (yy < 0 || yy >= LH) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    const int xx = x + dx;
                    if (xx >= 0 && xx < LW && g_drawn[yy * LW + xx]) { near = true; break; }
                }
            }
            if (!near) {
                Bit8u *p = g_hi + 2 * y * HW + 2 * x;
                p[0] = p[1] = p[HW] = p[HW + 1] = c;
            }
        }
    }
}

// A run of the screen (linear: a span may go on past its row) from the buffer, (x - xoff, y - yoff).
void span(const Target &src, int x, int y, int n, int xoff, int yoff) {
    int s = y * LW + x;
    for (int k = 0; k < n && s < LW * LH; k++, s++) {
        if (s < 0) continue;
        const int sx = s % LW, sy = s / LW, bx = sx - xoff, by = sy - yoff;
        if (bx < src.x0 || bx > src.x1 || by < src.y0 || by > src.y1) continue;
        g_window[s] = 1;
        g_shownLow[s] = rd8p(pixel_at(src, bx, by));
        const Bit8u *h = g_hi + 2 * by * HW + 2 * bx;
        Bit8u *o = g_shownHi + 2 * sy * HW + 2 * sx;
        o[0] = h[0];
        o[1] = h[1];
        o[HW] = h[HW];
        o[HW + 1] = h[HW + 1];
    }
}

void present_begin(const Target &src) {
    if (g_fresh) {                          // (nothing drawn since the last copy: H as it was, or the game's alone)
        if (!g_haveFrame) make_fresh();
    }
    fill_in(src);
    memset(g_window, 0, sizeof(g_window));
}

// Test aid (WCHIRESCHECK=1): at each span copy, how often a buffer pixel H drew in has its colour in H's block
// there; logged each 100 frames.  A turn or a mirror the other way shows as a fall in it.  WCHIRESDUMP=<per cent>: a
// frame below it is written (the game's buffer above H's), four at most.
void check(const Target &src) {
    static int on = -1, below = -2, dumped = 0;
    if (on < 0) { const char *e = getenv("WCHIRESCHECK"); on = e && e[0] == '1'; }
    if (!on) return;
    if (below == -2) { const char *e = getenv("WCHIRESDUMP"); below = e ? atoi(e) : -1; }
    static Bit64u drawn = 0, agree = 0, frames = 0;
    Bit64u d = 0, a = 0;
    for (int y = src.y0; y <= src.y1; y++) {
        const PhysPt row = pixel_at(src, 0, y);
        for (int x = src.x0; x <= src.x1; x++) {
            if (!g_drawn[y * LW + x]) continue;
            const Bit8u c = rd8p(row + x);
            const Bit8u *h = g_hi + 2 * y * HW + 2 * x;
            d++;
            a += h[0] == c || h[1] == c || h[HW] == c || h[HW + 1] == c;
        }
    }
    drawn += d;
    agree += a;
    frames++;
    static int from = -1;
    if (from < 0) { const char *e = getenv("WCHIRESDUMPFROM"); from = e ? atoi(e) : 0; }
    if (below > 0 && dumped < 4 && (int)frames >= from && d > 300 && a * 100 < d * (Bit64u)below) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/wch-%d.ppm", dir && dir[0] ? dir : ".", dumped++);
        FILE *f = fopen(path, "wb");
        if (f) {
            const int w = src.x1 + 1, h = src.y1 + 1;
            fprintf(f, "P6\n%d %d\n255\n", 2 * w, 4 * h);
            for (int Y = 0; Y < 4 * h; Y++) {
                for (int X = 0; X < 2 * w; X++) {
                    const Bit8u c = Y < 2 * h ? rd8p(pixel_at(src, X / 2, Y / 2)) : g_hi[(Y - 2 * h) * HW + X];
                    const Bit8u rgb[3] = { (Bit8u)(vga.dac.rgb[c].red << 2), (Bit8u)(vga.dac.rgb[c].green << 2),
                                           (Bit8u)(vga.dac.rgb[c].blue << 2) };
                    fwrite(rgb, 1, 3, f);
                }
            }
            fclose(f);
            wclog(1, "Wing Commander 640 x 400 check: frame %llu at %.1f%%: %s", (unsigned long long)frames, 100.0 * a / d, path);
            for (size_t i = 0; i < g_frameOps.size(); i++) wclog(1, "  its call %d: %s", (int)i, g_frameOps[i].c_str());
        }
    }
    if (frames % 100 == 0) {
        wclog(1, "Wing Commander 640 x 400 check: %llu frames, %llu pixels drawn in, %.2f%% with the game's colour in their block",
              (unsigned long long)frames, (unsigned long long)drawn, drawn ? 100.0 * agree / drawn : 0.0);
        drawn = agree = 0;
    }
}

void present_end() {
    g_frameOps.clear();
    g_haveFrame = true;
    g_frameSerial++;
    g_presentMs = PIC_FullIndex();
    g_fresh = true;
}

void present_spans(const Target &src, Bit16u list) {
    const int xoff = (Bit16s)dsw((Bit16u)(list + 4)), yoff = (Bit16s)dsw((Bit16u)(list + 6));
    if (xoff < -LW || xoff > LW || yoff < -LH || yoff > LH) return;
    present_begin(src);
    check(src);
    Bit16u p = (Bit16u)(list + 8);
    for (int i = 0; i < kMaxSpans; i++, p += 6) {
        const Bit16u x = dsw(p);
        if (x == 0xFFFF) break;
        const Bit16u y = dsw((Bit16u)(p + 2)), n = dsw((Bit16u)(p + 4));
        if (x >= LW || y >= LH) continue;
        span(src, x, y, n, xoff, yoff);
    }
    present_end();
}

// ---- the letters -------------------------------------------------------------------------------------------------------

// GLYPH (TM.DRV 4925, the game's only way to draw a letter: seg065:0071 calls it for each): AL the character, BX the
// text context (+0 the record, +2 x, +6 the font far, +A ink colour, +B background colour, FF none), CX the font's
// height, DX the letter's width, SI twice the top row.  The font: +0 its height, +2 the value its letters' ink is
// stored as, +3 their background's, +4 a width per character, +104 / +204 the low / high bytes of each letter's offset
// from the font; a letter is width x height bytes, one per pixel.  The driver returns at once while it is busy
// ([cs:0]).
struct Glyph { int ch, x, y, w, h, fg, bg, ink, ground; Bit16u ctx, rec, fseg, foff; Target t; };

bool read_glyph(Glyph &g) {
    if (rd16p(SegPhys(cs))) return false;
    g.ch = reg_al;
    g.ctx = reg_bx;
    g.h = reg_cx;
    g.w = reg_dx;
    g.y = (Bit16s)reg_si >> 1;
    g.rec = dsw(g.ctx);
    g.x = (Bit16s)dsw((Bit16u)(g.ctx + 2));
    g.foff = dsw((Bit16u)(g.ctx + 6));
    g.fseg = dsw((Bit16u)(g.ctx + 8));
    g.fg = rd8p(PhysMake(DS, (Bit16u)(g.ctx + 0xA)));
    g.bg = rd8p(PhysMake(DS, (Bit16u)(g.ctx + 0xB)));
    const PhysPt font = PhysMake(g.fseg, g.foff);
    g.ink = rd8p(font + 2);
    g.ground = rd8p(font + 3);
    return g.w > 0 && g.w <= 32 && g.h > 0 && g.h <= 32 && read_target(g.rec, g.t);
}

inline PhysPt glyph_data(const Glyph &g) {
    const PhysPt font = PhysMake(g.fseg, g.foff);
    const Bit16u off = (Bit16u)(rd8p(font + 0x104 + g.ch) | rd8p(font + 0x204 + g.ch) << 8);
    return PhysMake(g.fseg, (Bit16u)(g.foff + off));
}

// Test aid (WCHIRESFONTS=1): each font the game writes with, once, as text art in WCNET_DUMP_DIR/wch-font-<n>.txt;
// each text context and record it is used with, logged once.
void font_dump(const Glyph &g) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("WCHIRESFONTS"); on = e && e[0] == '1'; }
    if (!on) return;
    static std::map<Bit32u, int> fonts;
    static std::map<Bit64u, int> uses;
    const Bit32u fkey = (Bit32u)g.fseg << 16 | g.foff;
    if (!fonts.count(fkey) && fonts.size() < 32) {
        const int n = (int)fonts.size();
        fonts[fkey] = n;
        const char *dir = getenv("WCNET_DUMP_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/wch-font-%d.txt", dir && dir[0] ? dir : ".", n);
        FILE *f = fopen(path, "w");
        if (f) {
            const PhysPt font = PhysMake(g.fseg, g.foff);
            const int h = rd16p(font);
            fprintf(f, "font %04X:%04X height %d ink %02X ground %02X\n", g.fseg, g.foff, h, g.ink, g.ground);
            for (int c = 0; c < 256 && h > 0 && h <= 32; c++) {
                const int w = rd8p(font + 4 + c);
                if (!w || w > 32) continue;
                Glyph k = g;
                k.ch = c;
                const PhysPt d = glyph_data(k);
                fprintf(f, "char %d '%c' width %d\n", c, c >= 32 && c < 127 ? c : '?', w);
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        const int v = rd8p(d + y * w + x);
                        if (v == g.ink) fputc('#', f);
                        else if (v == g.ground) fputc('.', f);
                        else fprintf(f, "<%02X>", v);
                    }
                    fputc('\n', f);
                }
            }
            fclose(f);
        }
        wclog(1, "Wing Commander 640 x 400 fonts: font %d at %04X:%04X: %s", n, g.fseg, g.foff, path);
    }
    const Bit64u ukey = (Bit64u)fkey << 32 | (Bit32u)g.ctx << 16 | g.rec;
    if (!uses.count(ukey) && uses.size() < 256) {
        uses[ukey] = 1;
        wclog(1, "Wing Commander 640 x 400 fonts: font %d, context %04X, record %04X (segment %04X, %d,%d..%d,%d), "
                 "ink %02X ground %02X, first '%c' at %d,%d (%d x %d)",
              fonts.count(fkey) ? fonts[fkey] : -1, g.ctx, g.rec, g.t.seg, g.t.x0, g.t.y0, g.t.x1, g.t.y1, g.fg, g.bg,
              g.ch >= 32 && g.ch < 127 ? g.ch : '?', g.x, g.y, g.w, g.h);
    }
}

// The cockpit's letters at the top level, in the user's font (wcnet_priv_font.h: their Privateer letters, made for the
// 3 x 4 font WC1's cockpit font is): each letter the game writes on the screen in that font (its VDUs, the speed
// readouts: kWchGlyphs, exactly) is kept as a cell of the screen, with what was under it and what the game made of
// it.  At the VGA's frame a cell whose letter's ink is all still there shows the cell without that ink and the
// user's letter on it (the capitals' top a row below the cell's, less the font's shift up; its left a column in), in
// the letter's colour; its ink gone since (the screen cleared, the text redrawn, the cockpit drawn again) and the cell
// is the game's again and forgotten.  A letter written where another was takes its place (one with no ground of its
// own over others, a heading's underline of _, leaves them); until the game has drawn it, the one it replaced stays
// shown (the VGA's frame may fall between the call and its drawing).  The view's comm text is another font (eight
// rows, capitals six high), not the user's: the game's own, 2 x 2.
struct Look { const PrivGlyph *g; int fg, bg; Bit8u ink[16 * 8], under[16 * 8]; };
struct Letter { int x, y, w, h; Look now, prev; bool hasPrev, seen; double at; };
enum { kMaxLetters = 1024, kLetterYoungMs = 100 };
std::vector<Letter> g_letters;
Bit32u g_lettersSerial = 0;

const WchGlyph *wch_glyph(int ch) {
    for (int i = 0; i < kWchGlyphCount; i++) {
        if (kWchGlyphs[i].ch == ch) return &kWchGlyphs[i];
    }
    return NULL;
}

const PrivGlyph *user_letter(int ch) {
    for (int i = 0; i < kPrivGlyphCount; i++) {
        if (kPrivGlyphs[i].ch == ch) return &kPrivGlyphs[i];
    }
    return NULL;
}

void letter_add(const Glyph &g) {
    if (g.t.seg != kScreenSeg || g.w > 16 || g.h > 8) return;  // (the cockpit's own screens; the view's comm text is another font)
    // the cell on the screen (the record's rows: the screen's, one under the other)
    const int off = dsw((Bit16u)(g.t.rowTable + 2 * g.y)) + g.x;
    const int sx = off % LW, sy = off / LW;
    if (g.x < g.t.x0 || g.y < g.t.y0 || sx + g.w > LW || sy + g.h > LH) return;
    for (int y = 1; y < g.h; y++) {
        if (dsw((Bit16u)(g.t.rowTable + 2 * (g.y + y))) + g.x != off + y * LW) return;
    }
    // (the game's colours: ink and ground copied as they are when they are the font's own, else ink -> fg, ground -> bg,
    //  FF drawn as nothing)
    const bool raw = g.fg == g.ink && g.bg == g.ground;
    const int fg = raw ? g.ink : g.fg, bg = raw ? g.ground : (g.bg == 0xFF ? -1 : g.bg);
    // one of the cockpit font's letters, exactly, that the user's font has
    const WchGlyph *k = g.h == 6 ? wch_glyph(g.ch) : NULL;
    const PrivGlyph *u = user_letter(g.ch);
    bool ours = k && u && k->w == g.w && fg != 0xFF;
    Look look;
    look.g = u;
    look.fg = fg;
    look.bg = bg;
    const PhysPt d = glyph_data(g);
    for (int y = 0; y < g.h && ours; y++) {
        int bits = 0;
        for (int x = 0; x < g.w; x++) {
            const int v = rd8p(d + y * g.w + x);
            if (v == g.ink) bits |= 1 << x;
            else if (v != g.ground) ours = false;
        }
        ours = ours && bits == k->rows[y];
        for (int x = 0; x < g.w && ours; x++) look.ink[y * g.w + x] = (bits >> x) & 1;
    }
    // the letters it is drawn over: gone when its cell takes any of their ink (a cell of its own ground covers it, a
    // letter with none only where it has ink -- the headings' underline, a row of _ under them, leaves them), and the
    // one in the same cell; the rows below a line's letters (the Raptor's speed readout: KPS's six rows begin on the
    // last of SET's) leave them too
    Letter n;
    n.hasPrev = false;
    bool changed = false;
    const PhysPt d0 = glyph_data(g);
    for (size_t i = 0; i < g_letters.size();) {
        const Letter &o = g_letters[i];
        const bool same = o.x == sx && o.y == sy && o.w == g.w && o.h == g.h;
        bool covers = same;
        for (int y = 0; y < o.h && !covers; y++) {
            const int Y = o.y + y - sy;
            if (Y < 0 || Y >= g.h) continue;
            for (int x = 0; x < o.w && !covers; x++) {
                const int X = o.x + x - sx;
                if (X < 0 || X >= g.w || !o.now.ink[y * o.w + x]) continue;
                covers = bg >= 0 || rd8p(d0 + Y * g.w + X) == g.ink;
            }
        }
        if (covers) {
            if (same && o.seen && ours) {
                n.prev = o.now;
                n.hasPrev = true;
            }
            g_letters[i] = g_letters.back();
            g_letters.pop_back();
            changed = true;
        } else {
            i++;
        }
    }
    if (changed) g_lettersSerial++;
    if (!ours) return;
    const Bit8u *low = vga.fastmem;
    for (int y = 0; y < g.h; y++) {
        for (int x = 0; x < g.w; x++) look.under[y * g.w + x] = low[(sy + y) * LW + sx + x];
    }
    n.x = sx;
    n.y = sy;
    n.w = g.w;
    n.h = g.h;
    n.now = look;
    n.seen = false;
    n.at = PIC_FullIndex();
    if (g_letters.size() >= kMaxLetters) g_letters.erase(g_letters.begin());
    g_letters.push_back(n);
    g_lettersSerial++;
}

// The game's letter still there: every pixel of its ink in its colour.  (Its ground may have been drawn on since: the
// headings' underline.  A screen cleared, a letter drawn over it or the cockpit redrawn take its ink.)
bool letter_there(const Letter &l, const Look &k, const Bit8u *low) {
    for (int y = 0; y < l.h; y++) {
        for (int x = 0; x < l.w; x++) {
            if (k.ink[y * l.w + x] && low[(l.y + y) * LW + l.x + x] != k.fg) return false;
        }
    }
    return true;
}

void letter_draw(const Letter &l, const Look &k) {
    for (int y = 0; y < l.h; y++) {                 // (the game's ink gone: what is under it)
        for (int x = 0; x < l.w; x++) {
            const int i = y * l.w + x;
            if (!k.ink[i]) continue;
            const Bit8u c = (Bit8u)(k.bg >= 0 ? k.bg : k.under[i]);
            Bit8u *o = g_out + 2 * (l.y + y) * HW + 2 * (l.x + x);
            o[0] = o[1] = o[HW] = o[HW + 1] = c;
        }
    }
    const int X0 = 2 * (l.x + 1), Y0 = 2 * (l.y + 1) - kPrivGlyphShiftUp;
    for (int r = 0; r < 10; r++) {
        const int Y = Y0 + r;
        if (Y < 2 * l.y || Y >= 2 * (l.y + l.h)) continue;
        for (int u = 0; u < k.g->w; u++) {
            const int X = X0 + u;
            if (X >= 2 * (l.x + l.w) || !(k.g->rows[r] & (1 << u))) continue;
            g_out[Y * HW + X] = (Bit8u)k.fg;
        }
    }
}

// At the VGA's frame, after the picture: each cell checked, shown in the user's letters or forgotten.
void letters_compose(const Bit8u *low, bool show) {
    const double now = PIC_FullIndex();
    g_lettersShown = 0;
    for (size_t i = 0; i < g_letters.size();) {
        Letter &l = g_letters[i];
        if (letter_there(l, l.now, low)) {
            l.seen = true;
            l.hasPrev = false;
            if (show) {
                letter_draw(l, l.now);
                g_lettersShown++;
            }
        } else if (!l.seen && now - l.at < kLetterYoungMs) {
            if (show && l.hasPrev && letter_there(l, l.prev, low)) letter_draw(l, l.prev);
        } else {
            g_letters[i] = g_letters.back();
            g_letters.pop_back();
            continue;
        }
        i++;
    }
}

bool shapes_from(Bit16u where, Bit16u off, Bit16u seg);

// ---- the VDUs' ship pictures at the top level: wireframes -----------------------------------------------------------
//
// The VDUs' pictures (the weapons display's own ship and its missiles, the target display's ship and its shields) are
// SHAPEs on the VDUs' records on the screen (leftVdu, rightVdu, and copies of them with narrower clips): line art of
// one game pixel.  At the top level they are drawn as Privateer's target display's are (the user, 2026-11-27, of the
// sheets of every ship, scripts/wchsheet/: "Shield lines are good", then "All uprez are acceptable") -- the way the
// picture is drawn (wcnet_wire.cpp: lines one fine pixel wide between the pixels' middles, rings
// round, solid parts the game's blocks, drawn exactly symmetric where the art is).  The shapes drawn on one VDU one
// after another are gathered into one picture of the VDU (a ship comes in parts, the missiles over it), made when
// anything else is drawn next, and kept as a cell of the screen as the letters are: shown while every pixel of it
// still has its colour there, its pixels then what was under them and the wireframe on them.  A picture of more than
// kArtColours colours, or filling more than kArtFill per cent of its box, is no line art (the comm display's face):
// left the game's.
struct Art { Bit16u rec; int x, y, w, h; std::vector<Bit16s> lit; std::vector<Bit8u> under; std::shared_ptr<const WireFrame> wf; bool seen; double at; };
// (the wireframes kept: one per picture seen -- a target's sections and shields each change it -- so that a picture
//  is made once and then copied, the user: "Make sure to cache the drawings so it doesn't slow down framerate")
enum { kMaxArts = 32, kArtMax = 96, kArtColours = 12, kArtFill = 45, kWireCache = 128 };
std::vector<Art> g_arts;
Bit32u g_artsSerial = 0;
struct ArtGather { bool pending; Bit16u rec; int sx, sy, w, h; std::vector<Bit16s> px; std::vector<Bit8u> under; };
ArtGather g_gather = { false, 0, 0, 0, 0, 0, std::vector<Bit16s>(), std::vector<Bit8u>() };


struct WireCached { Bit32u hash; int w, h; std::shared_ptr<const WireFrame> wf; };
WireCached g_wires[kWireCache];
int g_wiresUsed = 0, g_wiresNext = 0;

std::shared_ptr<const WireFrame> wire_of(int w, int h, const std::vector<Bit16s> &px) {
    Bit32u hash = 2166136261u;
    for (size_t i = 0; i < px.size(); i++) hash = (hash ^ (Bit16u)px[i]) * 16777619u;
    for (int i = 0; i < g_wiresUsed; i++) {
        if (g_wires[i].hash == hash && g_wires[i].w == w && g_wires[i].h == h) return g_wires[i].wf;
    }
    WireCached &c = g_wires[g_wiresNext];
    g_wiresNext = (g_wiresNext + 1) % kWireCache;
    if (g_wiresUsed < kWireCache) g_wiresUsed++;
    c.hash = hash;
    c.w = w;
    c.h = h;
    WireFrame *made = new WireFrame();
    wire_picture(w, h, px, *made);
    c.wf.reset(made);
    return c.wf;
}

bool art_there(const Art &a, const Bit8u *low) {
    for (int v = 0; v < a.h; v++) {
        for (int u = 0; u < a.w; u++) {
            const Bit16s c = a.lit[v * a.w + u];
            if (c >= 0 && low[(a.y + v) * LW + a.x + u] != (Bit8u)c) return false;
        }
    }
    return true;
}

void art_flush() {
    if (!g_gather.pending) return;
    g_gather.pending = false;
    const ArtGather &g = g_gather;
    // the picture: its pixels' box, its colours
    int u0 = g.w, u1 = -1, v0 = g.h, v1 = -1;
    int colours[kArtColours + 1], ncol = 0;
    for (int v = 0; v < g.h; v++) {
        for (int u = 0; u < g.w; u++) {
            const Bit16s c = g.px[v * g.w + u];
            if (c < 0) continue;
            u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
            bool known = false;
            for (int k = 0; k < ncol && !known; k++) known = colours[k] == c;
            if (!known && ncol <= kArtColours) colours[ncol++] = c;
        }
    }
    // (the VDU's earlier pictures go: this one is the VDU's now)
    for (size_t i = 0; i < g_arts.size();) {
        if (g_arts[i].rec == g.rec) {
            g_arts[i] = g_arts.back();
            g_arts.pop_back();
            g_artsSerial++;
        } else {
            i++;
        }
    }
    if (u1 < 0 || ncol > kArtColours || u1 - u0 + 1 > kArtMax || v1 - v0 + 1 > kArtMax) return;
    // (line art leaves most of its box empty; a picture fills it: the comm display's face and its scan lines)
    int lit = 0;
    for (int v = v0; v <= v1; v++) {
        for (int u = u0; u <= u1; u++) lit += g.px[v * g.w + u] >= 0;
    }
    if (lit * 100 > (u1 - u0 + 1) * (v1 - v0 + 1) * kArtFill) return;
    Art a;
    a.rec = g.rec;
    a.x = g.sx + u0;
    a.y = g.sy + v0;
    a.w = u1 - u0 + 1;
    a.h = v1 - v0 + 1;
    a.lit.resize(a.w * a.h);
    a.under.resize(a.w * a.h);
    for (int v = 0; v < a.h; v++) {
        for (int u = 0; u < a.w; u++) {
            a.lit[v * a.w + u] = g.px[(v0 + v) * g.w + u0 + u];
            a.under[v * a.w + u] = g.under[(v0 + v) * g.w + u0 + u];
        }
    }
    a.wf = wire_of(a.w, a.h, a.lit);
    a.seen = false;
    a.at = PIC_FullIndex();
    if (g_arts.size() >= kMaxArts) g_arts.erase(g_arts.begin());
    g_arts.push_back(a);
    g_artsSerial++;
}

// The VDU a record draws on: one of the VDUs' records, or a copy of one with a narrower clip (the target display draws
// the hull a section at a time so, each section in its state's colours: WC1 ovr137:025D, records on the stack); 0 none.
Bit16u vdu_of(Bit16u rec, const Target &t, Target &vdu) {
    const Bit16u vdus[2] = { ds::known(ds::leftVdu) ? ds::leftVdu : (Bit16u)0, ds::known(ds::rightVdu) ? ds::rightVdu : (Bit16u)0 };
    for (int i = 0; i < 2; i++) {
        if (vdus[i] && rec == vdus[i]) { vdu = t; return vdus[i]; }
    }
    for (int i = 0; i < 2; i++) {
        Target v;
        if (!vdus[i] || !read_target(vdus[i], v)) continue;
        if (v.seg == t.seg && v.rowTable == t.rowTable && t.x0 >= v.x0 && t.x1 <= v.x1 && t.y0 >= v.y0 && t.y1 <= v.y1) {
            vdu = v;
            return vdus[i];
        }
    }
    return 0;
}

// A SHAPE on a VDU (vdu: its record's; t: the drawing's own clip): gathered into the VDU's picture (true), or the VDU
// not one the screen holds row under row (false).  A picture the VDU's size (a comm display's face, its static) is
// no ship: left the game's, the gathering going on.
bool art_shape(const Target &vdu, const Target &t, Bit16u rec, const Shape &s, int x, int y) {
    ArtGather &g = g_gather;
    if (s.w >= vdu.x1 - vdu.x0 - 1 && s.h >= vdu.y1 - vdu.y0 - 1) return true;
    if (g.pending && g.rec != rec) art_flush();
    if (!g.pending) {
        // the VDU's clip on the screen (the record's rows: the screen's, one under the other)
        const int w = vdu.x1 - vdu.x0 + 1, h = vdu.y1 - vdu.y0 + 1;
        const int off = dsw((Bit16u)(vdu.rowTable + 2 * vdu.y0)) + vdu.x0;
        if (w <= 0 || h <= 0 || w > 160 || h > 160 || off < 0 || off % LW + w > LW || off / LW + h > LH) return false;
        for (int v = 1; v < h; v++) {
            if (dsw((Bit16u)(vdu.rowTable + 2 * (vdu.y0 + v))) + vdu.x0 != off + v * LW) return false;
        }
        g.pending = true;
        g.rec = rec;
        g.sx = off % LW;
        g.sy = off / LW;
        g.w = w;
        g.h = h;
        g.px.assign(w * h, (Bit16s)-1);
        g.under.assign(w * h, 0);
        const Bit8u *low = vga.fastmem;
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) g.under[v * w + u] = low[(g.sy + v) * LW + g.sx + u];
        }
        // (a picture of this VDU still shown -- the text drawn between its parts flushed it -- goes on in this one)
        for (size_t i = 0; i < g_arts.size(); i++) {
            const Art &a = g_arts[i];
            if (a.rec != rec || !art_there(a, low)) continue;
            for (int v = 0; v < a.h; v++) {
                for (int u = 0; u < a.w; u++) {
                    const int gu = a.x + u - g.sx, gv = a.y + v - g.sy;
                    if (gu < 0 || gv < 0 || gu >= w || gv >= h) continue;
                    g.under[gv * w + gu] = a.under[v * a.w + u];
                    if (a.lit[v * a.w + u] >= 0) g.px[gv * w + gu] = a.lit[v * a.w + u];
                }
            }
        }
    }
    const int X = x - s.left - vdu.x0, Y = y - s.top - vdu.y0;  // (the shape's corner in the VDU's clip; drawn in its own)
    for (int v = 0; v < s.h; v++) {
        for (int u = 0; u < s.w; u++) {
            const Bit16s c = s.px[v * s.w + u];
            const int sx = x - s.left + u, sy = y - s.top + v;
            if (c >= 0 && X + u >= 0 && X + u < g.w && Y + v >= 0 && Y + v < g.h && sx >= t.x0 && sx <= t.x1 && sy >= t.y0 && sy <= t.y1) {
                g.px[(Y + v) * g.w + X + u] = c;
            }
        }
    }
    return true;
}

void arts_compose(const Bit8u *low, bool show) {
    const double now = PIC_FullIndex();
    g_artsShown = 0;
    for (size_t i = 0; i < g_arts.size();) {
        Art &a = g_arts[i];
        const bool there = art_there(a, low);
        if (there) {
            a.seen = true;
        } else if (a.seen || now - a.at >= kLetterYoungMs) {
            g_arts[i] = g_arts.back();
            g_arts.pop_back();
            continue;
        }
        if (there && show && a.wf && a.wf->w == a.w && a.wf->h == a.h) {
            g_artsShown++;
            for (int v = 0; v < a.h; v++) {
                for (int u = 0; u < a.w; u++) {
                    if (a.lit[v * a.w + u] < 0) continue;
                    const Bit8u c = a.under[v * a.w + u];
                    Bit8u *o = g_out + 2 * (a.y + v) * HW + 2 * (a.x + u);
                    o[0] = o[1] = o[HW] = o[HW + 1] = c;
                }
            }
            const int W = 2 * a.w;
            for (int Y = 0; Y < 2 * a.h; Y++) {
                for (int X = 0; X < W; X++) {
                    const Bit16s c = a.wf->fine[Y * W + X];
                    if (c >= 0) g_out[(2 * a.y + Y) * HW + 2 * a.x + X] = (Bit8u)c;
                }
            }
        }
        i++;
    }
}

// ---- the speed readouts' labels at the top level: the user's letters ----------------------------------------------
//
// The user, 2026-11-27: "ops set font" (KPS, SET).  Those labels are the cockpits' pictures, not text: four rows of
// pixels in one ink colour, the same 3 x 4 letters the user's font was drawn for, in five patterns over the cockpits
// read (WC1's Hornet, Scimitar, Rapier and Raptor, WC2's Ferret, SO2's).  The screen is searched for them when the
// picture comes on, when a cockpit is drawn (a big SHAPE on the screen) and every two seconds; a label found (its
// '#' all one colour, its '.' none of it, nor the pixels round it) is kept while its ink is there, and drawn as the
// user's glyphs in its ink colour, side by side with their two-pixel gaps, centred on it (Privateer's art_word), the
// ink's pixels the panel's colour about them.
struct LabelPattern { const char *text; const char *rows[4]; };
const LabelPattern kLabelPatterns[] = {
    { "KPS", { "#.#.###.###", "#.#.#.#.#..", "##..###...#", "#.#.#...###" } },     // WC1's Hornet, SO2's
    { "KPS", { "#.#.###.##.", "#.#.#.#.#..", "##..###..#.", "#.#.#...##." } },     // WC1's Scimitar, Rapier, Raptor
    { "KPS", { "#.#.###.###", "##..#.#.#..", "#.#.##....#", "#.#.#...###" } },     // WC2's Ferret
    { "SET", { "###.###.###", "#...##...#.", "..#.#....#.", "###.###..#." } },     // WC1's Hornet, WC2's Ferret, SO2's
    { "SET", { "##.###.###", "#..##...#.", ".#.#....#.", "##.###..#." } },         // WC1's Scimitar, Rapier, Raptor
};
enum { kLabelPatternCount = sizeof(kLabelPatterns) / sizeof(kLabelPatterns[0]), kMaxLabels = 16, kLabelSearchMs = 2000 };
struct Label { int x, y, pattern; Bit8u ink; Bit8u under[4 * 12]; };
std::vector<Label> g_labels;
bool g_labelsDirty = true;
double g_labelsSearched = -1e9;

bool label_at(const Bit8u *low, int x, int y, const LabelPattern &p, Bit8u &ink) {
    const int w = (int)strlen(p.rows[0]);
    if (x < 1 || y < 1 || x + w + 1 > LW || y + 5 > LH) return false;
    ink = low[y * LW + x];                          // (every pattern's first pixel is ink)
    for (int v = 0; v < 4; v++) {
        for (int u = 0; u < w; u++) {
            if ((p.rows[v][u] == '#') != (low[(y + v) * LW + x + u] == ink)) return false;
        }
    }
    for (int v = -1; v <= 4; v++) {
        for (int u = -1; u <= w; u++) {
            if (u >= 0 && v >= 0 && u < w && v < 4) continue;
            if (low[(y + v) * LW + x + u] == ink) return false;
        }
    }
    return true;
}

void labels_search(const Bit8u *low) {
    g_labels.clear();
    for (int y = 1; y + 5 <= LH && (int)g_labels.size() < kMaxLabels; y++) {
        for (int x = 1; x + 12 < LW && (int)g_labels.size() < kMaxLabels; x++) {
            for (int i = 0; i < kLabelPatternCount; i++) {
                Bit8u ink;
                if (!label_at(low, x, y, kLabelPatterns[i], ink)) continue;
                Label l;
                l.x = x;
                l.y = y;
                l.pattern = i;
                l.ink = ink;
                // (each ink pixel's panel: the commonest other colour of the eight about it -- the Rapier's is engraved
                //  in a grained panel)
                const int w = (int)strlen(kLabelPatterns[i].rows[0]);
                for (int v = 0; v < 4; v++) {
                    for (int u = 0; u < w; u++) {
                        int best = -1, bestN = 0;
                        int cs[8], n = 0;
                        for (int dv = -1; dv <= 1; dv++) {
                            for (int du = -1; du <= 1; du++) {
                                const Bit8u c = low[(y + v + dv) * LW + x + u + du];
                                if ((du || dv) && c != ink) cs[n++] = c;
                            }
                        }
                        for (int a = 0; a < n; a++) {
                            int k = 0;
                            for (int b = 0; b < n; b++) k += cs[b] == cs[a];
                            if (k > bestN) { bestN = k; best = cs[a]; }
                        }
                        l.under[v * 12 + u] = (Bit8u)(best >= 0 ? best : low[(y + v) * LW + x + u]);
                    }
                }
                g_labels.push_back(l);
                break;
            }
        }
    }
}

void labels_compose(const Bit8u *low, bool show) {
    const double now = PIC_FullIndex();
    if (show && (g_labelsDirty || now - g_labelsSearched > kLabelSearchMs)) {
        g_labelsDirty = false;
        g_labelsSearched = now;
        labels_search(low);
    }
    g_labelsShown = 0;
    if (!show) return;
    for (size_t i = 0; i < g_labels.size(); i++) {
        const Label &l = g_labels[i];
        const LabelPattern &p = kLabelPatterns[l.pattern];
        Bit8u ink;
        if (!label_at(low, l.x, l.y, p, ink) || ink != l.ink) continue;
        const int w = (int)strlen(p.rows[0]), n = (int)strlen(p.text);
        const PrivGlyph *g[4];
        int fineW = 2 * (n - 1);
        bool have = n <= 4;
        for (int k = 0; k < n && have; k++) {
            g[k] = user_letter(p.text[k]);
            have = g[k] != NULL;
            if (have) fineW += g[k]->w;
        }
        if (!have) continue;
        for (int v = 0; v < 4; v++) {
            for (int u = 0; u < w; u++) {
                if (p.rows[v][u] != '#') continue;
                Bit8u *o = g_out + 2 * (l.y + v) * HW + 2 * (l.x + u);
                o[0] = o[1] = o[HW] = o[HW + 1] = l.under[v * 12 + u];
            }
        }
        int X = 2 * l.x + (2 * w - fineW) / 2;
        const int Y0 = 2 * l.y;
        for (int k = 0; k < n; k++) {
            for (int r = 0; r < 8; r++) {
                for (int u = 0; u < g[k]->w; u++) {
                    if (g[k]->rows[r] & (1 << u)) g_out[(Y0 + r) * HW + X + u] = l.ink;
                }
            }
            X += g[k]->w + 2;
        }
        g_labelsShown++;
    }
}

void glyph_call() {
    Glyph g;
    if (!read_glyph(g)) return;
    font_dump(g);
    letter_add(g);
}

// A table of shapes the game keeps a far pointer to (the cockpit's markers, the shield arcs), as the driver has it at the hook: the game keeps a shapes pointer
// in EMS as a handle (segment C000 and up), which the driver's resolver has turned into the page frame's segment
// (emsFrameSeg), the offset kept, by then; the handle it mapped in is kept (emsLastShapes).  (Matching the pointer
// alone missed the markers whenever they were in EMS, as on the page: the gunsight was the game's blocks there.)
bool shapes_from(Bit16u where, Bit16u off, Bit16u seg) {
    const Bit16u moff = dsw(where), mseg = dsw((Bit16u)(where + 2));
    if (off != moff) return false;
    if (seg == mseg) return true;
    return (mseg & 0xC000) == 0xC000 && ds::known(ds::emsFrameSeg) && ds::known(ds::emsLastShapes) && seg == dsw(ds::emsFrameSeg) &&
           dsw(ds::emsLastShapes) == moff && dsw((Bit16u)(ds::emsLastShapes + 2)) == mseg;
}

bool markers(Bit16u off, Bit16u seg) { return shapes_from(ds::cockpitMarkers, off, seg); }

// ---- the calls --------------------------------------------------------------------------------------------------------

void call(const Place &p) {
    g_framed = p.framed;
    if (g_gather.pending && p.op != OP_SHAPE) art_flush();
    if (p.op == OP_GLYPH) {
        glyph_call();
        return;
    }
    Target t;
    if (p.op == OP_PRESENT || p.op == OP_COPY) {
        Target src, dst;
        if (!read_target((Bit16u)arg16(0), src) || !read_target((Bit16u)arg16(1), dst)) return;
        if (!flights_buffer(src) || dst.seg != kScreenSeg) {
            if (p.op == OP_COPY && flights_buffer(dst)) {           // (something copied into the buffer: its pixels, 2 x 2)
                make_fresh();
                const int w = std::min(src.x1 - src.x0, dst.x1 - dst.x0) + 1, h = std::min(src.y1 - src.y0, dst.y1 - dst.y0) + 1;
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) block(dst.x0 + x, dst.y0 + y, rd8p(pixel_at(src, src.x0 + x, src.y0 + y)));
                }
            }
            return;
        }
        if (p.op == OP_PRESENT) {
            present_spans(src, (Bit16u)arg16(2));
        } else {                            // (the outside views: the buffer's clip to the screen's clip's corner)
            present_begin(src);
            const int w = std::min(src.x1 - src.x0, dst.x1 - dst.x0) + 1, h = std::min(src.y1 - src.y0, dst.y1 - dst.y0) + 1;
            for (int y = 0; y < h; y++) span(src, dst.x0, dst.y0 + y, w, dst.x0 - src.x0, dst.y0 - src.y0);
            present_end();
        }
        return;
    }
    static int keep = -1;
    if (keep < 0) { const char *e = getenv("WCHIRESDUMP"); keep = e && atoi(e) > 0; }
    if (keep && g_frameOps.size() < 400) {
        char b[160];
        snprintf(b, sizeof(b), "op%d %d %d %d %d %d %d %d %d %d fresh %d", (int)p.op, arg16(0), arg16(1), arg16(2), arg16(3), arg16(4),
                 arg16(5), arg16(6), arg16(7), arg16(8), (int)g_fresh);
        g_frameOps.push_back(b);
    }
    if (!read_target((Bit16u)arg16(0), t)) return;
    Target vdu;
    const Bit16u onVdu = p.op == OP_SHAPE && t.seg == kScreenSeg ? vdu_of((Bit16u)arg16(0), t, vdu) : 0;
    if (onVdu) {
        const Shape *s = shape_of((Bit16u)arg16(3), (Bit16u)arg16(4), arg16(5));
        {   // (dev: WCHIRESPARTS=1 writes each new part drawn on a VDU, its shapes and index)
            static int on = -1, n = 0;
            static std::map<Bit64u, int> seen;
            if (on < 0) { const char *e = getenv("WCHIRESPARTS"); on = e && e[0] == '1'; }
            const Bit64u key = (Bit64u)(Bit16u)arg16(4) << 32 | (Bit32u)(Bit16u)arg16(3) << 16 | (Bit16u)arg16(5);
            if (on && s && n < 60 && !seen.count(key)) {
                seen[key] = 1;
                const char *dir = getenv("WCNET_DUMP_DIR");
                char path[512];
                snprintf(path, sizeof(path), "%s/part-%02d.ppm", dir && dir[0] ? dir : ".", n);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
                    for (int i = 0; i < s->w * s->h; i++) {
                        const int c = s->px[i];
                        const Bit8u rgb[3] = { (Bit8u)(c < 0 ? 40 : vga.dac.rgb[c].red << 2), (Bit8u)(c < 0 ? 0 : vga.dac.rgb[c].green << 2),
                                               (Bit8u)(c < 0 ? 40 : vga.dac.rgb[c].blue << 2) };
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                }
                if (!n) {                                   // (and the palette, 256 x RGB 0..63, for the sheet's rig)
                    snprintf(path, sizeof(path), "%s/palette.bin", dir && dir[0] ? dir : ".");
                    FILE *pf = fopen(path, "wb");
                    if (pf) {
                        for (int c = 0; c < 256; c++) {
                            const Bit8u rgb[3] = { vga.dac.rgb[c].red, vga.dac.rgb[c].green, vga.dac.rgb[c].blue };
                            fwrite(rgb, 1, 3, pf);
                        }
                        fclose(pf);
                    }
                }
                wclog(1, "VDU part %d: on %04X, shapes %04X:%04X #%d at %d,%d, %dx%d anchor %d,%d", n, (Bit16u)arg16(0), (Bit16u)arg16(4),
                      (Bit16u)arg16(3), arg16(5), arg16(1), arg16(2), s->w, s->h, s->left, s->top);
                n++;
            }
        }
        if (s && art_shape(vdu, t, onVdu, *s, arg16(1), arg16(2))) return;
    }
    if (g_gather.pending) art_flush();
    // (a cockpit drawn -- its picture's corner at the screen's: not the radar's or the gauges' marks, every frame --
    //  its labels looked for)
    if (p.op == OP_SHAPE && t.seg == kScreenSeg && arg16(1) == 0 && arg16(2) == 0) g_labelsDirty = true;
    if (!flights_buffer(t)) {
        // Test aid (WCHIRESOPS=1): the calls on other records (the cockpit's screens), each kind once with its caller.
        static int ops = -1;
        if (ops < 0) { const char *e = getenv("WCHIRESOPS"); ops = e && e[0] == '1'; }
        if (ops) {
            static std::map<Bit64u, int> seen;
            const PhysPt sp = SegPhys(ss) + (Bit16u)(p.framed ? reg_bp + 2 : reg_sp);
            const Bit16u rip = rd16p(sp), rcs = rd16p(sp + 2);
            const Bit64u key = (Bit64u)p.op << 48 | (Bit64u)(Bit16u)arg16(0) << 32 | (Bit32u)rcs << 16 | rip;
            if (!seen.count(key) && seen.size() < 400) {
                seen[key] = 1;
                wclog(1, "Wing Commander 640 x 400 ops: op %d on %04X (segment %04X %d,%d..%d,%d) from %04X:%04X: %d %d %d %d %d %d %d %d",
                      (int)p.op, (Bit16u)arg16(0), t.seg, t.x0, t.y0, t.x1, t.y1, rcs, rip, arg16(1), arg16(2), arg16(3), arg16(4),
                      arg16(5), arg16(6), arg16(7), arg16(8));
            }
        }
        return;
    }
    make_fresh();
    switch (p.op) {
    case OP_CLEAR:
        fill(t, t.x0, t.y0, t.x1, t.y1, (Bit8u)arg16(1));
        break;
    case OP_FILL:
        fill(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
        break;
    case OP_LINE:
        line(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
        break;
    case OP_RECT: {
        const int x0 = arg16(1), y0 = arg16(2), x1 = arg16(3), y1 = arg16(4);
        const Bit8u c = (Bit8u)arg16(5);
        line(t, x0, y0, x1, y0, c);
        line(t, x1, y0, x1, y1, c);
        line(t, x1, y1, x0, y1, c);
        line(t, x0, y1, x0, y0, c);
        break;
    }
    case OP_SHAPE: {
        const Shape *s = shape_of((Bit16u)arg16(3), (Bit16u)arg16(4), arg16(5));
        if (!s) break;
        // (the cockpit's markers, dseg cockpitMarkers: 0 the gunsight at the view's middle, 1 the lock cross -- the
        // reticle, as Privateer's: one-pixel line art, its circles fitted, drawn half and mirrored)
        // (WC2's: 0 the gunsight, 4 the same a pixel right, 1 the lock cross; 5 is the pointer, seg005:331D)
        const int index = arg16(5);
        const bool marker = markers((Bit16u)arg16(3), (Bit16u)arg16(4)) && (index == 0 || index == 1 || (is_wc2() && index == 4));
        if (marker) shape_lines(t, *s, arg16(1), arg16(2));
        else shape_blocks(t, *s, arg16(1), arg16(2));
        break;
    }
    case OP_TURN: {
        const Shape *s = shape_of((Bit16u)arg16(3), (Bit16u)arg16(4), arg16(5));
        if (!s) break;
        int degrees = arg16(6);
        const int scale = (Bit16u)arg16(7), flags = arg16(8);
        if (scale <= 0) break;
        degrees %= 360;
        if (degrees < 0) degrees += 360;
        int turn = flags;
        if (degrees >= 180) {           // (as the driver does: 180 and over are both axes mirrored (0x30) and the rest)
            degrees -= 180;
            turn ^= 0x30;
        }
        if (scale == 0x100 && degrees == 0 && !(turn & 0x30)) {      // (the driver's own copy of SHAPE: as it is)
            shape_blocks(t, *s, arg16(1), arg16(2));
        } else {
            // (magnified: placed as the game's magnifier does, a source pixel from the anchor's corner -- measured against the
            // game's own, 2026-11-27, a close Dralthi at 3x and 5x off by up to 5 game pixels otherwise; shrunk: sampled
            // about the pixels' middles, Privateer's way, which matched the game's better than the corner did)
            shape_scaled(t, *s, arg16(1), arg16(2), scale / 256.0, degrees, (turn & 0x10) != 0, (turn & 0x20) != 0, scale > 0x100);
        }
        break;
    }
    default:
        break;
    }
}

// ---- the picture --------------------------------------------------------------------------------------------------------

void compose() {
    const Bit8u *low = vga.fastmem;
    const bool flight = g_level && g_haveFrame && PIC_FullIndex() - g_presentMs < kStaleMs;
    static Bit8u madeFrom[LW * LH];
    static Bit32u madeSerial = 0;
    static int madeFlight = -1;
    const Bit32u serial = flight ? g_frameSerial : 0;
    static Bit32u madeLetters = 0, madeArts = 0;
    static int madeLevel = -1;
    if (madeFlight == (int)flight && madeSerial == serial && madeLetters == g_lettersSerial && madeArts == g_artsSerial &&
        madeLevel == g_level && !memcmp(madeFrom, low, LW * LH)) return;
    madeFlight = flight;
    madeSerial = serial;
    madeLetters = g_lettersSerial;
    madeArts = g_artsSerial;
    madeLevel = g_level;
    memcpy(madeFrom, low, LW * LH);
    for (int y = 0; y < LH; y++) {
        const Bit8u *now = low + y * LW, *in = g_window + y * LW, *then = g_shownLow + y * LW;
        const Bit8u *h0 = g_shownHi + 2 * y * HW, *h1 = h0 + HW;
        Bit8u *o0 = g_out + 2 * y * HW, *o1 = o0 + HW;
        for (int x = 0; x < LW; x++) {
            const Bit8u c = now[x];
            if (flight && in[x] && c == then[x]) {
                o0[2 * x] = h0[2 * x];
                o0[2 * x + 1] = h0[2 * x + 1];
                o1[2 * x] = h1[2 * x];
                o1[2 * x + 1] = h1[2 * x + 1];
            } else {
                o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = c;
            }
        }
    }
    arts_compose(low, flight && g_level == 2);
    letters_compose(low, flight && g_level == 2);
    labels_compose(low, flight && g_level == 2);
}

bool game_picture() {           // the game's own 320 x 200, shown from the start of the VGA's memory
    return vga.mode == M_VGA && vga.config.compatible_chain4 && !vga.config.real_start;
}

void set_override(bool want) {
    if (want == g_override) return;
    g_override = want;
    if (want) {
        g_labelsDirty = true;
        const double fps = vga.draw.delay.vtotal > 0 ? 1000.0 / vga.draw.delay.vtotal : 70.0;
        VGA_SetOverride(true);
        RENDER_SetSize(HW, HH, 8, (float)fps, 1.0, false, false);
        wclog(1, "Wing Commander: the picture is 640 x 400 (each game pixel 2 x 2)");
    } else {
        g_haveFrame = false;
        g_fresh = true;
        // (the letters and the VDUs' pictures kept: each is checked against the screen before it is shown, and the
        //  game does not draw a VDU again for nothing -- forgotten here, they stayed the game's blocks after ` came
        //  back round to the top level)
        g_gather.pending = false;
        VGA_SetOverride(false);
        wclog(1, "Wing Commander: the picture is the game's own");
    }
}

}  // namespace

bool wch_on() { return g_override; }
int wch_marker_draws() { return (int)(g_markerDraws & 0x7FFFFFFF); }
int wch_letters_shown() { return g_lettersShown; }
int wch_arts_shown() { return g_artsShown; }
int wch_labels_shown() { return g_labelsShown; }
double wch_cost_ms() { return g_costMs; }

int wch_level() {
    read_env();
    return g_allowed > 0 ? g_level : 0;
}

static const char *level_name(int level) {
    return level == 2 ? "on" : level == 1 ? "on in space alone" : "off";
}

// The page's boxes: 2 all, 1 the space alone, 0 off (and the ` key leaves the game's again).
void wch_set(int level) {
    read_env();
    if (g_never) return;
    const int was = g_allowed > 0 ? g_level : 0;
    level = level < 0 ? 0 : level > 2 ? 2 : level;
    g_allowed = level ? 1 : 0;
    g_level = level;
    if (was != level) wclog(1, "Wing Commander: the high-resolution picture %s (the page's)", level_name(level));
}

bool wch_key(bool pressed) {
    read_env();
    if (g_allowed <= 0 || !(is_wc1() || is_wc2())) return false;   // (not allowed: the key is the game's, as it always was)
    if (pressed) {
        g_level = (g_level + 2) % 3;            // (all, then the space alone, then the game's own, then all)
        wclog(1, "Wing Commander: the high-resolution picture %s (the ` key)", level_name(g_level));
    }
    return true;
}

// Every ~1000 instructions.  Not allowed: nothing (and the override, if it was ours, given back).
void wch_tick() {
    read_env();
    if (g_allowed <= 0 || !usable()) {
        if (g_override) set_override(false);
        return;
    }
    const Bit16u tm = dsw(ds::tmDriverSeg), st = dsw(ds::superTmDriverSeg);
    if (tm && st && (tm != g_watchedTm || st != g_watchedSt)) {
        hooks_game_changed();                       // (the drivers are loaded: their places watched from now on)
    }
    set_override(g_level && g_watchedTm && game_picture());
}

void wch_watch(void (*watch)(Bit16u ip)) {
    (void)watch;
    memset(g_onlyOurs, 0, sizeof(g_onlyOurs));
    g_watchedTm = g_watchedSt = 0;
    read_env();
    if (g_allowed <= 0 || !usable()) return;
    const Bit16u tm = dsw(ds::tmDriverSeg), st = dsw(ds::superTmDriverSeg);
    if (!tm || !st) return;                         // (not loaded yet: nothing to watch)
    // (the drivers are loaded at the start of their segments: the loader keeps a far pointer, its offset first)
    if (dsw((Bit16u)(ds::tmDriverSeg - 2)) || dsw((Bit16u)(ds::superTmDriverSeg - 2))) return;
    int watched = 0;
    for (int i = 0; i < kPlaceCount; i++) {
        const Place &p = kPlaces[i];
        const Bit16u seg = p.drv == TM ? tm : st;
        const Bit16u ip = (Bit16u)(rd16p(PhysMake(seg, p.slot)) + p.delta);
        g_placeIp[i] = 0;
        bool known = ip >= 0x10;
        for (int k = 0; k < 3 && known; k++) known = rd8p(PhysMake(seg, (Bit16u)(ip + k))) == p.code[k];
        if (!known) {
            wclog(1, "Wing Commander: the high-resolution picture's place %d (%s +%02X) not as known at %04X: left alone", i,
                  p.drv == TM ? "TM.DRV" : "SUPERTM.DRV", p.slot, ip);
            continue;
        }
        g_placeIp[i] = ip;
        g_onlyOurs[ip] = g_onlyOurs[ip] || !wc_net_watch[ip];
        watch_at(seg, ip);
        watched++;
    }
    g_watchedTm = tm;
    g_watchedSt = st;
    wclog(1, "Wing Commander: the high-resolution picture's places watched (TM.DRV at %04X, SUPERTM.DRV at %04X): %d of %d",
          tm, st, watched, (int)kPlaceCount);
}

bool wch_hook(Bit32u ip) {
    if (ip > 0xFFFF || !g_watchedTm) return false;
    const Bit16u code = SegValue(cs);
    for (int i = 0; i < kPlaceCount; i++) {
        const Place &p = kPlaces[i];
        if (!g_placeIp[i] || g_placeIp[i] != ip || code != (p.drv == TM ? g_watchedTm : g_watchedSt)) continue;
        if (!g_override) return true;
        const PhysPt at = SegPhys(cs) + ip;
        for (int k = 0; k < 3; k++) {
            if (rd8p(at + k) != p.code[k]) return true;      // (not the driver as we know it)
        }
        const double t0 = perf_now_ms();
        call(p);
        g_costMs += perf_now_ms() - t0;
        return true;
    }
    return g_onlyOurs[ip];
}

}  // namespace wc

void wc_wch_vsync() {
    if (!wc::g_override) return;
    if (!RENDER_StartUpdate()) return;
    const double t0 = wc::perf_now_ms();
    wc::compose();
    for (int y = 0; y < wc::HH; y++) RENDER_DrawLine(wc::g_out + y * wc::HW);
    RENDER_EndUpdate(false);
    wc::g_costMs += wc::perf_now_ms() - t0;
}
