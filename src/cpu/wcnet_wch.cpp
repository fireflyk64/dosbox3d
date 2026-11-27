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
#include <string>
#include <vector>
#include "dosbox.h"
#include "wcnet_wch.h"
#include "wcnet_game.h"
#include "wcnet_memory.h"
#include "wcnet_log.h"
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
enum Op { OP_SHAPE, OP_TURN, OP_LINE, OP_RECT, OP_FILL, OP_CLEAR, OP_COPY, OP_PRESENT };

// A place in a driver's code (the VGA entry's offsets, WC.EXE's GAMEDAT): `framed`, past the routine's own
// `push bp / mov bp,sp` and its shape pointer's resolving (EMS: [bp+0C] then holds the real pointer), its arguments
// at [bp+6]; else the routine's first instruction, its arguments at [sp+4].  `code`: the bytes there, checked.
struct Place { Drv drv; Bit16u ip; Op op; bool framed; Bit8u code[3]; };
const Place kPlaces[] = {
    { TM, 0x0EBB, OP_SHAPE,   true,  { 0xFC, 0x0E, 0x07 } },     // SHAPE (rec, x, y, shapes far, index): the dust, the gunsight, the pointer
    { ST, 0x161E, OP_TURN,    true,  { 0x8C, 0xC8, 0x8E } },     // TURNSCALE (... index, degrees, scale, flags): every ship, bolt, missile
    { TM, 0x3A12, OP_LINE,    false, { 0x55, 0x8B, 0xEC } },     // LINE (rec, x0, y0, x1, y1, colour): the target's brackets
    { TM, 0x3896, OP_RECT,    false, { 0x55, 0x8B, 0xEC } },     // RECT (rec, x0, y0, x1, y1, colour): the locked target's box
    { TM, 0x37F2, OP_FILL,    false, { 0x55, 0x8B, 0xEC } },     // FILLRECT (rec, x0, y0, x1, y1, colour)
    { TM, 0x192F, OP_CLEAR,   false, { 0x55, 0x8B, 0xEC } },     // CLEAR (rec, colour)
    { TM, 0x198F, OP_COPY,    false, { 0x55, 0x8B, 0xEC } },     // COPY (src rec, dst rec): the outside views' way to the screen
    { ST, 0x7454, OP_PRESENT, false, { 0x55, 0x8B, 0xEC } },     // SPANCOPY (src rec, dst rec, span list near): the cockpit views'
};
enum { kPlaceCount = sizeof(kPlaces) / sizeof(kPlaces[0]) };

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
int g_level = 0;                    // what shows: 1 the picture, 0 the game's own (the ` key flips it)
bool g_override = false;
Bit16u g_watchedTm = 0, g_watchedSt = 0;   // the drivers' segments the places are watched in (0: none watched)
bool g_onlyOurs[0x10000];           // an IP watched for these places alone

void read_env() {
    if (g_allowed >= 0) return;
    const char *env = getenv("WCHIRES");
    g_never = env && !strcmp(env, "off");
    g_allowed = !g_never && env && env[0] == '1';
    g_level = g_allowed ? 1 : 0;
}

bool usable() {
    return is_wc1() && g_build == BUILD_WC1 && DS && ds::known(ds::tmDriverSeg) && ds::known(ds::superTmDriverSeg) &&
           ds::known(ds::flightBuffer);
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

inline void plot(const Target &t, int X, int Y, Bit8u c) {
    if (X < 2 * t.x0 || X >= 2 * (t.x1 + 1) || Y < 2 * t.y0 || Y >= 2 * (t.y1 + 1)) return;
    g_hi[Y * HW + X] = c;
    g_drawn[(Y >> 1) * LW + (X >> 1)] = 1;
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
    int X = 2 * x0 + 1, Y = 2 * y0 + 1;
    const int Xe = 2 * x1 + 1, Ye = 2 * y1 + 1;
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

// ---- the calls --------------------------------------------------------------------------------------------------------

void call(const Place &p) {
    g_framed = p.framed;
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
    if (!read_target((Bit16u)arg16(0), t) || !flights_buffer(t)) return;
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
        if (s) shape_blocks(t, *s, arg16(1), arg16(2));
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
        if (scale == 0x100 && degrees == 0 && !(flags & 0x30)) {     // (the driver's own copy of SHAPE: as it is)
            shape_blocks(t, *s, arg16(1), arg16(2));
        } else {
            // (magnified: placed as the game's magnifier does, a source pixel from the anchor's corner -- measured against the
            // game's own, 2026-11-27, a close Dralthi at 3x and 5x off by up to 5 game pixels otherwise; shrunk: sampled
            // about the pixels' middles, Privateer's way, which matched the game's better than the corner did)
            shape_scaled(t, *s, arg16(1), arg16(2), scale / 256.0, degrees, (flags & 0x10) != 0, (flags & 0x20) != 0, scale > 0x100);
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
    if (madeFlight == (int)flight && madeSerial == serial && !memcmp(madeFrom, low, LW * LH)) return;
    madeFlight = flight;
    madeSerial = serial;
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
}

bool game_picture() {           // the game's own 320 x 200, shown from the start of the VGA's memory
    return vga.mode == M_VGA && vga.config.compatible_chain4 && !vga.config.real_start;
}

void set_override(bool want) {
    if (want == g_override) return;
    g_override = want;
    if (want) {
        const double fps = vga.draw.delay.vtotal > 0 ? 1000.0 / vga.draw.delay.vtotal : 70.0;
        VGA_SetOverride(true);
        RENDER_SetSize(HW, HH, 8, (float)fps, 1.0, false, false);
        wclog(1, "Wing Commander: the picture is 640 x 400 (each game pixel 2 x 2)");
    } else {
        g_haveFrame = false;
        g_fresh = true;
        VGA_SetOverride(false);
        wclog(1, "Wing Commander: the picture is the game's own");
    }
}

}  // namespace

bool wch_on() { return g_override; }

int wch_level() {
    read_env();
    return g_allowed > 0 ? g_level : 0;
}

void wch_set(int on) {
    read_env();
    if (g_never) return;
    const bool was = g_allowed > 0 && g_level;
    g_allowed = on ? 1 : 0;
    g_level = on ? 1 : 0;
    if (was != (on != 0)) wclog(1, "Wing Commander: the high-resolution picture %s (the page's)", on ? "on" : "off");
}

bool wch_key(bool pressed) {
    read_env();
    if (g_allowed <= 0 || !is_wc1()) return false;      // (not allowed: the key is the game's, as it always was)
    if (pressed) {
        g_level = !g_level;
        wclog(1, "Wing Commander: the high-resolution picture %s (the ` key)", g_level ? "on" : "off");
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
    for (int i = 0; i < kPlaceCount; i++) {
        g_onlyOurs[kPlaces[i].ip] = !wc_net_watch[kPlaces[i].ip];
        watch_at(kPlaces[i].drv == TM ? tm : st, kPlaces[i].ip);
    }
    g_watchedTm = tm;
    g_watchedSt = st;
    wclog(1, "Wing Commander: the high-resolution picture's places watched (TM.DRV at %04X, SUPERTM.DRV at %04X)", tm, st);
}

bool wch_hook(Bit32u ip) {
    if (ip > 0xFFFF || !g_watchedTm) return false;
    const Bit16u code = SegValue(cs);
    for (int i = 0; i < kPlaceCount; i++) {
        const Place &p = kPlaces[i];
        if (p.ip != ip || code != (p.drv == TM ? g_watchedTm : g_watchedSt)) continue;
        if (!g_override) return true;
        const PhysPt at = SegPhys(cs) + p.ip;
        for (int k = 0; k < 3; k++) {
            if (rd8p(at + k) != p.code[k]) return true;      // (not the driver as we know it)
        }
        call(p);
        return true;
    }
    return g_onlyOurs[ip];
}

}  // namespace wc

void wc_wch_vsync() {
    if (!wc::g_override) return;
    if (!RENDER_StartUpdate()) return;
    wc::compose();
    for (int y = 0; y < wc::HH; y++) RENDER_DrawLine(wc::g_out + y * wc::HW);
    RENDER_EndUpdate(false);
}
