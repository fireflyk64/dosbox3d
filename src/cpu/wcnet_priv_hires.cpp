/*
 *  Privateer at 640 x 480 (docs/privateer.md section 20).
 *
 *  In flight the game draws the scene and the instruments into one buffer in memory, every pixel of it by a dozen
 *  routines of its graphics library (kPlaces), and shows it by copying a list of spans, the cockpit's window and
 *  screens, to the VGA (seg205:0002); the cockpit itself it drew to the VGA once.  So each call of the library on
 *  that buffer is drawn again here, from its own arguments as it is entered, into a 640 x 480 picture (H); the
 *  game's drawing is not touched and neither is its memory: one pilot's display, nothing a room could see.
 *
 *  At the span copy H is frozen with what the game shows (the buffer under the spans).  At the VGA's frame the
 *  picture is made: a pixel of the spans that still holds what was copied shows H; every other, and any screen that
 *  is not a flight's, is the game's own pixel, 2 wide and 2 or 3 tall.
 *
 *  The game runs paged (its memory manager's): linear A0000 is memory of its own, where that buffer is, and the
 *  screen is mapped at FFFF:0010.  So the screen is known by its physical address, and the game's memory is read
 *  only by rd8/rd16/rd32, which never fault (these hooks run inside the CPU's core).
 *
 *  The arguments come from the game's memory: every one is bounded before it is used, every list has its cap.
 *
 *  Until the picture is first wanted (the page's box, WCPRIVHIRES=1, or the ` key) none of this runs: its places are
 *  not watched.  WCPRIVHIRES=off, or WCPRIVCLASSIC=1 (the game as it was: wcnet_priv.h), keeps it so for the run.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "wcnet_priv_hires.h"
#include "wcnet_priv.h"
#include "wcnet_game.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "mem.h"
#include "paging.h"
#include "pic.h"
#include "regs.h"
#include "render.h"
#include "vga.h"

namespace wc {

namespace {

enum { LW = 320, LH = 200, HW = 640, HH = 480 };
enum {
    kMaxShapeSide = 640,            // a shape's width or height
    kMaxShapeBytes = 0x10000,       // what one shape's decoding may read
    kCacheBytes = 8 << 20,          // decoded shapes kept (then all are let go)
    kMaxCached = 4096,
    kMaxVerts = 16,                 // a polygon's corners (the game's have 3)
    kMaxExact = 64,                 // projected corners remembered for their fractions
    kMaxSpans = 4096,               // the span list's
    kScreen = 0xA0000,              // the VGA's memory, physical
    kStaleMs = 300,                 // a frame copied longer ago is not shown (the longest the game steps by is 233 ms)
};

enum Op { OP_FILL, OP_CLEAR, OP_COPY, OP_ELLIPSE, OP_LINE, OP_PIXEL, OP_RECT, OP_SILHOUETTE, OP_SHAPE, OP_SCALE,
          OP_SCALE_HAZE, OP_TURN, OP_POLY, OP_PRESENT, OP_CORNER_X, OP_CORNER_Y };

struct Place { Bit16u para, ip; Op op; Bit8u code[3]; };
const Place kPlaces[] = {
    { 0x54C8, 0x0008, OP_FILL,       { 0x55, 0x8B, 0xEC } },    // seg180
    { 0x54D1, 0x000A, OP_CLEAR,      { 0x55, 0x8B, 0xEC } },    // seg181
    { 0x54D6, 0x0008, OP_COPY,       { 0x55, 0x8B, 0xEC } },    // seg182
    { 0x54DF, 0x0004, OP_ELLIPSE,    { 0x55, 0x8B, 0xEC } },    // seg183
    { 0x5514, 0x0002, OP_LINE,       { 0xC8, 0x0A, 0x00 } },    // seg184
    { 0x5533, 0x000A, OP_PIXEL,      { 0x55, 0x8B, 0xEC } },    // seg185
    { 0x5537, 0x0000, OP_RECT,       { 0x55, 0x8B, 0xEC } },    // seg186
    { 0x554B, 0x0004, OP_SILHOUETTE, { 0x55, 0x8B, 0xEC } },    // seg187
    { 0x5575, 0x0008, OP_SHAPE,      { 0x55, 0x8B, 0xEC } },    // seg188
    { 0x559A, 0x0008, OP_SCALE_HAZE, { 0xC8, 0xDA, 0x02 } },    // seg189
    { 0x5600, 0x0006, OP_SCALE,      { 0xC8, 0xD8, 0x02 } },    // seg193
    { 0x5B3F, 0x0ECE, OP_TURN,       { 0x55, 0x8B, 0xEC } },    // seg202
    { 0x47A2, 0x000E, OP_POLY,       { 0xC8, 0x94, 0x00 } },    // seg116
    { 0x5D17, 0x0002, OP_PRESENT,    { 0xC8, 0x06, 0x00 } },    // seg205
    { 0x3771, 0x05D7, OP_CORNER_X,   { 0x8B, 0x46, 0xE7 } },    // seg091: a laser's corner, before it is rounded
    { 0x3771, 0x0621, OP_CORNER_Y,   { 0x8B, 0x46, 0xD3 } },
};
enum { kPlaceCount = sizeof(kPlaces) / sizeof(kPlaces[0]) };
enum {
    kCrosshairCaller = 0x2897,      // seg060: the crosshair's shape, drawn as one-pixel lines
    kRadarCaller = 0x13AE,          // the radar's: its art (one shape a frame, under the blips), drawn as line art
    kFillerSelf = 0x47A2,           // seg116 calls itself once, to align its stack
};

// A bitmap record of the game's: segment, row table (near, DGROUP), clip.
struct Target {
    Bit16u seg, rowTable;
    int x0, y0, x1, y1;
};

struct Shape {
    int w, h, left, top;            // the anchor is at (left, top)
    Bit32u stamp;                   // of the bytes it was decoded from
    std::vector<Bit16s> px;         // -1 where nothing is drawn
};

struct Exact { int xi, yi; double x, y; };

Bit8u g_hi[HW * HH];                // H, being drawn
Bit8u g_shownHi[HW * HH];           // H as it was at the last span copy
Bit8u g_shownLow[LW * LH];          // ... and what that copy put on the screen
Bit8u g_window[LW * LH];            // the spans
Bit16u g_from[HW * HH];             // the edge at the fine grid: see smooth_edge
Bit8u g_fineWindow[HW * HH];
Bit8u g_out[HW * HH];
bool g_haveFrame = false;
bool g_drewThis = false;            // H has been drawn since the last span copy (not while flipped off)
Bit32u g_frameSerial = 0;           // the frames frozen
Bit32u g_spanStamp = 0;

int g_rowFrom[LH + 1];              // the first of H's rows that shows the game's row y
int g_lowRow[HH];
bool g_tables = false;

Bit16u g_bufSeg = 0, g_bufRow0 = 0; // the flight's buffer, learnt at the span copy
double g_presentMs = -1e9;

std::map<Bit64u, Shape> g_cache;
size_t g_cacheBytes = 0;

Exact g_exact[kMaxExact];
int g_exactNext = 0, g_exactCount = 0;
double g_cornerX = 0;
int g_cornerXi = 0;
bool g_haveCornerX = false;

bool g_onlyOurs[0x10000];           // an IP watched for these places alone

int g_want = -1;                    // the checkbox (WCPRIVHIRES at the launch)
bool g_flipped = false;             // the backquote key's
bool g_never = false;               // WCPRIVHIRES=off, or the game as it was (priv_classic): nothing of this runs
bool g_used = false;                // wanted once: its places are watched from then on
bool g_hooked = false;              // ... and they are
bool g_override = false;
int g_setW = 0;

inline bool hires_wanted() {
    if (g_want < 0) {
        const char *env = getenv("WCPRIVHIRES");
        g_never = priv_classic() || (env && !strcmp(env, "off"));
        g_want = !g_never && env && env[0] && env[0] != '0' ? 1 : 0;
    }
    return !g_never && (g_want != 0) != g_flipped;
}

// Wanted for the first time: its places watched from the next instruction (until then none of this runs at all).
void now_wanted() {
    if (!hires_wanted() || g_used) return;
    g_used = true;
    if (!g_hooked) hooks_game_changed();
}

void make_tables() {
    if (g_tables) return;
    g_tables = true;
    for (int Y = 0; Y < HH; Y++) g_lowRow[Y] = (2 * Y + 1) * LH / (2 * HH);
    int Y = 0;
    for (int y = 0; y <= LH; y++) {
        while (Y < HH && g_lowRow[Y] < y) Y++;
        g_rowFrom[y] = Y;
    }
}

// The game's memory as a real-mode program reaches it (the first MB and 64 KB): 0 for anywhere else or not mapped.
enum { kReach = 0x10FFF0 };

inline Bit8u rd8(PhysPt a) {
    Bit8u v = 0;
    return a < (PhysPt)kReach && !mem_readb_checked(a, &v) ? v : 0;
}

inline Bit16u rd16(PhysPt a) { return (Bit16u)(rd8(a) | rd8(a + 1) << 8); }
inline Bit32u rd32(PhysPt a) { return rd16(a) | (Bit32u)rd16(a + 2) << 16; }

inline Bit16s arg16(int i) {
    return (Bit16s)rd16(SegPhys(ss) + (Bit16u)(reg_sp + 4 + 2 * i));
}

bool read_target(Bit16u at, Target &t) {
    const PhysPt ds = PhysMake(priv_ds(), 0);
    t.seg = rd16(ds + at);
    t.rowTable = rd16(ds + (Bit16u)(at + 2));
    t.x0 = (Bit16s)rd16(ds + (Bit16u)(at + 4));
    t.y0 = (Bit16s)rd16(ds + (Bit16u)(at + 6));
    t.x1 = (Bit16s)rd16(ds + (Bit16u)(at + 8));
    t.y1 = (Bit16s)rd16(ds + (Bit16u)(at + 10));
    if (t.x0 < 0) t.x0 = 0;
    if (t.y0 < 0) t.y0 = 0;
    if (t.x1 > LW - 1) t.x1 = LW - 1;
    if (t.y1 > LH - 1) t.y1 = LH - 1;
    return t.x0 <= t.x1 && t.y0 <= t.y1;
}

inline Bit16u row_of(const Target &t, int y) {
    return rd16(PhysMake(priv_ds(), (Bit16u)(t.rowTable + 2 * y)));
}

inline bool flights_buffer(const Target &t) {
    return g_bufSeg && t.seg == g_bufSeg && row_of(t, 0) == g_bufRow0;
}

// ---- the 640 x 480 picture's drawing: a game pixel is the block g_rowFrom gives -------------------------------------

inline void block_row(int x0, int x1, int y, Bit8u c) {     // (clipped already)
    for (int Y = g_rowFrom[y]; Y < g_rowFrom[y + 1]; Y++) memset(g_hi + Y * HW + 2 * x0, c, 2 * (x1 - x0 + 1));
}

inline void block(int x, int y, Bit8u c) { block_row(x, x, y, c); }

void fill(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 > x1) { const int s = x0; x0 = x1; x1 = s; }
    if (y0 > y1) { const int s = y0; y0 = y1; y1 = s; }
    if (x0 < t.x0) x0 = t.x0;
    if (y0 < t.y0) y0 = t.y0;
    if (x1 > t.x1) x1 = t.x1;
    if (y1 > t.y1) y1 = t.y1;
    if (x0 > x1) return;
    for (int y = y0; y <= y1; y++) block_row(x0, x1, y, c);
}

inline void plot(const Target &t, int X, int Y, Bit8u c) {
    if (X < 2 * t.x0 || X >= 2 * (t.x1 + 1) || Y < g_rowFrom[t.y0] || Y >= g_rowFrom[t.y1 + 1]) return;
    g_hi[Y * HW + X] = c;
}

inline int fine_x(int x) { return 2 * x + 1; }
inline int fine_y(int y) { return (12 * y + 6) / 5; }       // (the middle of the pixel)

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

// One fine pixel wide, between the middles of the game's pixels.
void line(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 < -2000 || x0 > 2000 || x1 < -2000 || x1 > 2000 || y0 < -2000 || y0 > 2000 || y1 < -2000 || y1 > 2000) return;
    line_fine(t, fine_x(x0), fine_y(y0), fine_x(x1), fine_y(y1), c);
}

void ellipse(const Target &t, int x, int y, int rx, int ry, Bit8u c) {
    if (rx < 0 || ry < 0 || rx > 1000 || ry > 1000 || x < -2000 || x > 2000 || y < -2000 || y > 2000) return;
    const double cx = 2 * x + 1, cy = (y + 0.5) * 2.4, a = 2.0 * rx, b = 2.4 * ry;
    const int n = 8 + (int)(8 * (a > b ? a : b));
    for (int i = 0; i < n; i++) {
        const double w = i * 6.283185307179586 / n;
        plot(t, (int)floor(cx + a * cos(w)), (int)floor(cy + b * sin(w)), c);
    }
}

// ---- the game's shapes ----------------------------------------------------------------------------------------------

const Shape *shape_of(Bit16u off, Bit16u seg, int index) {
    if (index < 0 || index > 4095) return NULL;
    const PhysPt file = PhysMake(seg, off);
    const Bit32u table = rd32(file + 4) & 0xFFFFFF;
    if ((Bit32u)(4 * (index + 1)) >= table) return NULL;
    const PhysPt at = file + (rd32(file + 4 + 4 * index) & 0xFFFFF);
    const int x2 = (Bit16s)rd16(at), x1 = (Bit16s)rd16(at + 2);
    const int y1 = (Bit16s)rd16(at + 4), y2 = (Bit16s)rd16(at + 6);
    const int w = x1 + x2 + 1, h = y1 + y2 + 1;
    if (w <= 0 || h <= 0 || w > kMaxShapeSide || h > kMaxShapeSide) return NULL;
    // (EMS moves other shapes under the same address: the first bytes and the size's say which it is)
    Bit32u stamp = 2166136261u;
    for (PhysPt i = 0; i < 64; i += 4) stamp = (stamp ^ rd32(at + i)) * 16777619u;
    for (PhysPt i = 64; i < (PhysPt)(w * h / 4) && i < 4096; i += 124) stamp = (stamp ^ rd32(at + i)) * 16777619u;
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
    s.stamp = stamp;
    s.px.assign(w * h, (Bit16s)-1);
    PhysPt p = at + 8;
    const PhysPt end = at + kMaxShapeBytes;
    while (p + 6 <= end) {
        const Bit16u head = rd16(p);
        if (!head) break;
        int x = (Bit16s)rd16(p + 2) + x1;
        const int y = (Bit16s)rd16(p + 4) + y1;
        p += 6;
        int n = head >> 1;
        const bool inside = y >= 0 && y < h;
        Bit16s *row = inside ? &s.px[y * w] : NULL;
        if (!(head & 1)) {
            for (; n > 0 && p < end; n--, x++, p++) {
                if (inside && x >= 0 && x < w) row[x] = rd8(p);
            }
            continue;
        }
        while (n > 0 && p < end) {
            const Bit8u sub = rd8(p++);
            int run = sub >> 1;
            if (!run) return &s;        // (not a shape: what there is)
            n -= run;
            if (sub & 1) {
                const Bit8u c = rd8(p++);
                for (; run; run--, x++) {
                    if (inside && x >= 0 && x < w) row[x] = c;
                }
            } else {
                for (; run && p < end; run--, x++, p++) {
                    if (inside && x >= 0 && x < w) row[x] = rd8(p);
                }
            }
        }
    }
    return &s;
}

void shape_blocks(const Target &t, const Shape &s, int x, int y, int colour) {
    const int left = x - s.left, top = y - s.top;
    for (int v = 0; v < s.h; v++) {
        const int py = top + v;
        if (py < t.y0 || py > t.y1) continue;
        const Bit16s *row = &s.px[v * s.w];
        for (int u = 0; u < s.w; u++) {
            const int px = left + u;
            if (row[u] >= 0 && px >= t.x0 && px <= t.x1) block(px, py, (Bit8u)(colour >= 0 ? colour : row[u]));
        }
    }
}

// Line art of one pixel's width: each pixel joined to its neighbours' middles.
void shape_lines(const Target &t, const Shape &s, int x, int y) {
    static const int kDx[4] = { 1, 0, 1, -1 }, kDy[4] = { 0, 1, 1, 1 };
    const int left = x - s.left, top = y - s.top;
    for (int v = 0; v < s.h; v++) {
        for (int u = 0; u < s.w; u++) {
            const int c = s.px[v * s.w + u];
            if (c < 0) continue;
            line(t, left + u, top + v, left + u, top + v, (Bit8u)c);
            for (int k = 0; k < 4; k++) {
                const int nu = u + kDx[k], nv = v + kDy[k];
                if (nu < 0 || nu >= s.w || nv >= s.h || s.px[nv * s.w + nu] < 0) continue;
                if (kDx[k] && kDy[k] && (s.px[v * s.w + nu] >= 0 || s.px[nv * s.w + u] >= 0)) continue;
                line(t, left + u, top + v, left + nu, top + nv, (Bit8u)c);
            }
        }
    }
}

// The radar's art, redrawn each frame under its blips: its most common colour (the screen's ground) in blocks, each
// other colour's pixels joined to their same-coloured neighbours by one-pixel lines -- the rings and the cross, each
// ring of its own colour (joining any colours made a mesh of the two-tone rings; thinning the bitmap, combs).  A pixel
// in no chain of its colour (fewer than two such neighbours: a two-tone bar's, a line's end) reaches halfway to each
// neighbour of another ink, so the side gauge's two-tone bar is a line and not dots.
void shape_radar(const Target &t, const Shape &s, int x, int y) {
    static const int kDx[4] = { 1, 0, 1, -1 }, kDy[4] = { 0, 1, 1, 1 };
    static const int kAx[8] = { 1, -1, 0, 0, 1, -1, 1, -1 }, kAy[8] = { 0, 0, 1, -1, 1, 1, -1, -1 };
    const int left = x - s.left, top = y - s.top;
    int count[256] = { 0 };
    for (int i = 0; i < s.w * s.h; i++) {
        if (s.px[i] >= 0) count[s.px[i] & 0xFF]++;
    }
    int ground = 0;
    for (int c = 1; c < 256; c++) {
        if (count[c] > count[ground]) ground = c;
    }
    shape_blocks(t, s, x, y, ground);
    auto same = [&](int u, int v, int c) { return u >= 0 && u < s.w && v >= 0 && v < s.h && s.px[v * s.w + u] == c; };
    auto ink = [&](int u, int v) {
        return u >= 0 && u < s.w && v >= 0 && v < s.h && s.px[v * s.w + u] >= 0 && s.px[v * s.w + u] != ground;
    };
    for (int v = 0; v < s.h; v++) {
        for (int u = 0; u < s.w; u++) {
            const int c = s.px[v * s.w + u];
            if (c < 0 || c == ground) continue;
            const int X = fine_x(left + u), Y = fine_y(top + v);
            line_fine(t, X, Y, X, Y, (Bit8u)c);
            for (int k = 0; k < 4; k++) {
                const int nu = u + kDx[k], nv = v + kDy[k];
                if (!same(nu, nv, c)) continue;
                if (kDx[k] && kDy[k] && (same(nu, v, c) || same(u, nv, c))) continue;
                line_fine(t, X, Y, fine_x(left + nu), fine_y(top + nv), (Bit8u)c);
            }
            int alike = 0;
            for (int k = 0; k < 8; k++) alike += same(u + kAx[k], v + kAy[k], c);
            if (alike >= 2) continue;
            for (int k = 0; k < 8; k++) {
                const int nu = u + kAx[k], nv = v + kAy[k];
                if (!ink(nu, nv) || same(nu, nv, c)) continue;
                if (kAx[k] && kAy[k] && (ink(nu, v) || ink(u, nv))) continue;
                line_fine(t, X, Y, (X + fine_x(left + nu)) / 2, (Y + fine_y(top + nv)) / 2, (Bit8u)c);
            }
        }
    }
}

struct Haze { PhysPt table; int first, count; };

// A shape scaled and turned as the game's blitters do (mirrored first, then turned clockwise, in the game's pixels:
// no correction for their shape), sampled at each fine pixel's middle.
void shape_scaled(const Target &t, const Shape &s, int x, int y, double sx, double sy, int degrees, bool fx, bool fy,
                  const Haze *haze) {
    if (sx <= 0 || sy <= 0 || sx > 64 || sy > 64) return;
    const double w = degrees * 3.141592653589793 / 180.0, c = cos(w), n = sin(w);
    const double mx = fx ? -1 : 1, my = fy ? -1 : 1;
    // the box: the corners of the shape through the same transform
    double lox = 1e9, hix = -1e9, loy = 1e9, hiy = -1e9;
    for (int k = 0; k < 4; k++) {
        const double u = ((k & 1 ? s.w : 0) - 0.5 - s.left) * sx * mx, v = ((k & 2 ? s.h : 0) - 0.5 - s.top) * sy * my;
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
    int X0 = (int)(lox * 2), X1 = (int)ceil(hix * 2), Y0 = (int)(loy * 2.4), Y1 = (int)ceil(hiy * 2.4);
    if (X0 < 2 * t.x0) X0 = 2 * t.x0;
    if (X1 > 2 * (t.x1 + 1)) X1 = 2 * (t.x1 + 1);
    if (Y0 < g_rowFrom[t.y0]) Y0 = g_rowFrom[t.y0];
    if (Y1 > g_rowFrom[t.y1 + 1]) Y1 = g_rowFrom[t.y1 + 1];
    // the source's place, 16.16, stepped along a row
    const Bit32s du = (Bit32s)floor(c * 0.5 * mx / sx * 65536 + 0.5), dv = (Bit32s)floor(-n * 0.5 * my / sy * 65536 + 0.5);
    const Bit16s *src = &s.px[0];
    for (int Y = Y0; Y < Y1; Y++) {
        const double gx = (X0 + 0.5) / 2 - x - 0.5, gy = (Y + 0.5) / 2.4 - y - 0.5;
        const double u0 = (gx * c + gy * n) * mx / sx + 0.5 + s.left, v0 = (-gx * n + gy * c) * my / sy + 0.5 + s.top;
        if (fabs(u0) > 16000 || fabs(v0) > 16000) continue;
        Bit32s u = (Bit32s)floor(u0 * 65536), v = (Bit32s)floor(v0 * 65536);
        Bit8u *out = g_hi + Y * HW;
        for (int X = X0; X < X1; X++, u += du, v += dv) {
            const int iu = u >> 16, iv = v >> 16;
            if ((unsigned)iu >= (unsigned)s.w || (unsigned)iv >= (unsigned)s.h) continue;
            const int colour = src[iv * s.w + iu];
            if (colour < 0) continue;
            if (haze && colour >= haze->first && colour < haze->first + haze->count) {
                out[X] = rd8(haze->table + 256 * (colour - haze->first) + out[X]);
            } else {
                out[X] = (Bit8u)colour;
            }
        }
    }
}

// A convex polygon through a table (the lasers' translucency): each row's span with both its ends, as the game's, so
// that a bolt far away is still a fine pixel wide.
void polygon(const Target &t, const double *px, const double *py, int n, PhysPt table) {
    double lo = py[0], hi = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < lo) lo = py[i];
        if (py[i] > hi) hi = py[i];
    }
    const double top = (lo + 0.5) * 2.4, bottom = (hi + 0.5) * 2.4;
    if (bottom < 0 || top >= HH) return;
    int Y0 = (int)floor(top), Y1 = (int)floor(bottom);
    if (Y0 < g_rowFrom[t.y0]) Y0 = g_rowFrom[t.y0];
    if (Y1 >= g_rowFrom[t.y1 + 1]) Y1 = g_rowFrom[t.y1 + 1] - 1;
    Bit8u map[256];
    for (int i = 0; i < 256; i++) map[i] = rd8(table + i);
    for (int Y = Y0; Y <= Y1; Y++) {
        double mid = Y + 0.5;
        if (mid < top) mid = top;
        if (mid > bottom) mid = bottom;
        double l = 1e9, r = -1e9;
        for (int i = 0; i < n; i++) {
            const int j = i + 1 == n ? 0 : i + 1;
            const double ax = (px[i] + 0.5) * 2, ay = (py[i] + 0.5) * 2.4;
            const double bx = (px[j] + 0.5) * 2, by = (py[j] + 0.5) * 2.4;
            if (ay == by) {
                if (ay != mid) continue;
                if (ax < l) l = ax;
                if (bx < l) l = bx;
                if (ax > r) r = ax;
                if (bx > r) r = bx;
            } else if ((mid >= ay && mid <= by) || (mid >= by && mid <= ay)) {
                const double at = ax + (mid - ay) * (bx - ax) / (by - ay);
                if (at < l) l = at;
                if (at > r) r = at;
            }
        }
        if (l > r) continue;
        if (l < 2 * t.x0) l = 2 * t.x0;
        if (r > 2 * (t.x1 + 1) - 1) r = 2 * (t.x1 + 1) - 1;
        if (l > r) continue;
        Bit8u *out = g_hi + Y * HW;
        for (int X = (int)l; X <= (int)r; X++) out[X] = map[out[X]];
    }
}

// ---- the calls ------------------------------------------------------------------------------------------------------

void corner(bool isY) {
    const PhysPt ds = PhysMake(priv_ds(), 0);
    const Bit16u index = rd16(SegPhys(ss) + (Bit16u)(reg_bp + 0x16));
    const Bit16u field = (Bit16u)(reg_di + (isY ? 0x34 : 0x32) + 4 * index);
    const int base = (Bit16s)rd16(ds + field);
    const Bit32s v = (Bit32s)rd32(SegPhys(ss) + (Bit16u)(reg_bp - (isY ? 0x2E : 0x1A)));   // 24.8
    const int whole = v >> 8;
    if (!isY) {
        g_cornerX = base + v / 256.0;
        g_cornerXi = base + whole;
        g_haveCornerX = true;
        return;
    }
    if (!g_haveCornerX) return;
    g_haveCornerX = false;
    Exact &e = g_exact[g_exactNext];
    e.xi = g_cornerXi;
    e.x = g_cornerX;
    e.yi = base - whole;
    e.y = base - v / 256.0;
    g_exactNext = (g_exactNext + 1) % kMaxExact;
    if (g_exactCount < kMaxExact) g_exactCount++;
}

void exact_corner(int xi, int yi, double &x, double &y) {
    x = xi;
    y = yi;
    for (int i = 1; i <= g_exactCount; i++) {
        const Exact &e = g_exact[(g_exactNext - i + kMaxExact) % kMaxExact];
        if (e.xi == xi && e.yi == yi) {
            x = e.x;
            y = e.y;
            return;
        }
    }
}

void present() {
    Target src, dst;
    if (!read_target((Bit16u)arg16(0), src) || !read_target((Bit16u)arg16(1), dst)) return;
    if (PAGING_GetPhysicalAddress(PhysMake(dst.seg, row_of(dst, 0))) != kScreen) return;
    const Bit16u row0 = row_of(src, 0);
    const bool known = src.seg == g_bufSeg && row0 == g_bufRow0;
    g_bufSeg = src.seg;
    g_bufRow0 = row0;
    g_presentMs = PIC_FullIndex();
    const bool drew = g_drewThis;
    g_drewThis = hires_wanted();        // (drawn from here to the next copy only while it is shown)
    if (!known || !drew || !g_drewThis) {   // (a buffer just learnt or a flip: the picture is ours from the next frame)
        g_haveFrame = false;
        return;
    }
    PhysPt p = PhysMake((Bit16u)arg16(3), (Bit16u)arg16(2)) + 12;
    Bit32u stamp = 2166136261u;
    int spans = 0;
    static Bit16u list[3 * kMaxSpans];
    for (; spans < kMaxSpans; spans++, p += 6) {
        const Bit16u x = rd16(p);
        if (x == 0xFFFF) break;
        const Bit16u y = rd16(p + 2), n = rd16(p + 4);
        list[3 * spans] = x;
        list[3 * spans + 1] = y;
        list[3 * spans + 2] = n;
        stamp = (((stamp ^ x) * 16777619u ^ y) * 16777619u ^ n) * 16777619u;
    }
    if (stamp != g_spanStamp) {
        g_spanStamp = stamp;
        memset(g_window, 0, sizeof(g_window));
        for (int i = 0; i < spans; i++) {
            const int x = list[3 * i], y = list[3 * i + 1];
            int n = list[3 * i + 2];
            if (x >= LW || y >= LH) continue;
            if (n > LW - x) n = LW - x;
            memset(g_window + y * LW + x, 1, n);
        }
    }
    for (int i = 0; i < spans; i++) {
        const int x = list[3 * i], y = list[3 * i + 1];
        int n = list[3 * i + 2];
        if (x >= LW || y >= LH) continue;
        if (n > LW - x) n = LW - x;
        const PhysPt from = PhysMake(src.seg, 0) + (Bit16u)(row_of(src, y) + x);
        for (int k = 0; k < n; k++) g_shownLow[y * LW + x + k] = rd8(from + k);
    }
    memcpy(g_shownHi, g_hi, sizeof(g_hi));
    g_haveFrame = true;
    g_frameSerial++;
}

void call(const Place &p) {
    if (p.op == OP_CORNER_X || p.op == OP_CORNER_Y) {
        corner(p.op == OP_CORNER_Y);
        return;
    }
    if (p.op == OP_PRESENT) {
        present();
        return;
    }
    if (!g_bufSeg || !g_drewThis) return;
    Target t;
    const Bit16u caller = (Bit16u)(rd16(SegPhys(ss) + (Bit16u)(reg_sp + 2)) - priv_load_seg());
    if (p.op == OP_POLY && caller == kFillerSelf) return;
    if (!read_target((Bit16u)arg16(p.op == OP_COPY ? 1 : 0), t) || !flights_buffer(t)) return;
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
    case OP_ELLIPSE:
        ellipse(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
        break;
    case OP_PIXEL: {
        const int x = arg16(1), y = arg16(2);
        if (x >= t.x0 && x <= t.x1 && y >= t.y0 && y <= t.y1) block(x, y, (Bit8u)arg16(3));
        break;
    }
    case OP_COPY: {
        Target from;
        if (!read_target((Bit16u)arg16(0), from)) return;
        int w = from.x1 - from.x0 + 1, h = from.y1 - from.y0 + 1;
        if (w > t.x1 - t.x0 + 1) w = t.x1 - t.x0 + 1;
        if (h > t.y1 - t.y0 + 1) h = t.y1 - t.y0 + 1;
        for (int y = 0; y < h; y++) {
            const Bit16u row = row_of(from, from.y0 + y);
            for (int x = 0; x < w; x++) block(t.x0 + x, t.y0 + y, rd8(PhysMake(from.seg, (Bit16u)(row + from.x0 + x))));
        }
        break;
    }
    case OP_POLY: {
        const int n = arg16(1);
        if (n < 2 || n > kMaxVerts) return;
        const Bit16u off = (Bit16u)arg16(2), seg = (Bit16u)arg16(3);
        double px[kMaxVerts], py[kMaxVerts];
        for (int i = 0; i < n; i++) {
            const PhysPt v = PhysMake(seg, (Bit16u)(off + 22 * i));
            exact_corner((Bit16s)rd16(v), (Bit16s)rd16(v + 2), px[i], py[i]);
        }
        polygon(t, px, py, n, PhysMake((Bit16u)arg16(5), (Bit16u)arg16(4)));
        break;
    }
    case OP_SILHOUETTE:
    case OP_SHAPE:
    case OP_SCALE:
    case OP_SCALE_HAZE:
    case OP_TURN: {
        const int x = arg16(1), y = arg16(2);
        int degrees = 0, sx = 256, sy = 256, flags = 0;
        if (p.op == OP_TURN) {
            degrees = arg16(6);
            sx = sy = arg16(7);
            flags = arg16(8);
            // (as it is: seg202 jumps to the plain shape's entry, which comes up here itself)
            if (sx == 256 && (Bit8s)flags == degrees) return;
        } else if (p.op == OP_SCALE || p.op == OP_SCALE_HAZE) {
            sx = arg16(6);
            sy = arg16(7);
            if (sx < 0) { sx = -sx; flags |= 0x10; }
            if (sy < 0) { sy = -sy; flags |= 0x20; }
        }
        const Shape *s = shape_of((Bit16u)arg16(3), (Bit16u)arg16(4), arg16(5));
        if (!s) return;
        if (p.op == OP_SILHOUETTE) {
            shape_blocks(t, *s, x, y, (Bit8u)arg16(6));
        } else if (p.op == OP_SHAPE) {
            if (caller == kCrosshairCaller) shape_lines(t, *s, x, y);
            else if (caller == kRadarCaller) shape_radar(t, *s, x, y);
            else shape_blocks(t, *s, x, y, -1);
        } else if (p.op == OP_SCALE_HAZE) {
            Haze h;
            h.table = PhysMake((Bit16u)arg16(9), (Bit16u)arg16(8));
            h.first = arg16(10);
            h.count = arg16(11);
            if (h.first < 0 || h.count < 0 || h.count > 256) return;
            shape_scaled(t, *s, x, y, sx / 256.0, sy / 256.0, 0, (flags & 0x10) != 0, (flags & 0x20) != 0, &h);
        } else {
            shape_scaled(t, *s, x, y, sx / 256.0, sy / 256.0, degrees, (flags & 0x10) != 0, (flags & 0x20) != 0, NULL);
        }
        break;
    }
    default:
        break;
    }
}

// ---- the picture ----------------------------------------------------------------------------------------------------

// The cockpit's edge at the fine grid: the spans' mask smoothed over its neighbours (1 2 1 each way) and read at
// each fine pixel's middle between the four nearest.  g_from: the game's pixel a fine pixel takes its colour from,
// on its own side of the edge, so that the cockpit's art stays as it is.
void smooth_edge() {
    static Bit32u done = 0;
    static bool any = false;
    if (any && done == g_spanStamp) return;
    any = true;
    done = g_spanStamp;
    static int blur[LW * LH];
    for (int y = 0; y < LH; y++) {
        for (int x = 0; x < LW; x++) {
            int sum = 0;
            for (int j = -1; j <= 1; j++) {
                const int yy = y + j < 0 ? 0 : y + j >= LH ? LH - 1 : y + j;
                for (int i = -1; i <= 1; i++) {
                    const int xx = x + i < 0 ? 0 : x + i >= LW ? LW - 1 : x + i;
                    sum += g_window[yy * LW + xx] * (i ? 1 : 2) * (j ? 1 : 2);
                }
            }
            blur[y * LW + x] = sum;
        }
    }
    for (int Y = 0; Y < HH; Y++) {
        // (positions in twentieths of a game pixel)
        const int y = g_lowRow[Y];
        int vy = (20 * Y + 10) * 5 / 12 - 10;
        if (vy < 0) vy = 0;
        int y0 = vy / 20, fy = vy % 20;
        if (y0 >= LH - 1) { y0 = LH - 1; fy = 0; }
        const int y1 = y0 + 1 < LH ? y0 + 1 : y0;
        for (int X = 0; X < HW; X++) {
            int vx = 10 * X - 5;
            if (vx < 0) vx = 0;
            int x0 = vx / 20, fx = vx % 20;
            if (x0 >= LW - 1) { x0 = LW - 1; fx = 0; }
            const int x1 = x0 + 1 < LW ? x0 + 1 : x0;
            const int v = (blur[y0 * LW + x0] * (20 - fx) + blur[y0 * LW + x1] * fx) * (20 - fy)
                        + (blur[y1 * LW + x0] * (20 - fx) + blur[y1 * LW + x1] * fx) * fy;
            const int x = X >> 1;
            const Bit8u side = v > 8 * 20 * 20 ? 1 : 0;
            int fromX = x, fromY = y;
            if (g_window[y * LW + x] != side) {
                static const int kDx[8] = { -1, 1, 0, 0, -1, 1, -1, 1 }, kDy[8] = { 0, 0, -1, 1, -1, -1, 1, 1 };
                for (int k = 0; k < 8; k++) {
                    const int nx = x + kDx[k], ny = y + kDy[k];
                    if (nx >= 0 && ny >= 0 && nx < LW && ny < LH && g_window[ny * LW + nx] == side) {
                        fromX = nx;
                        fromY = ny;
                        break;
                    }
                }
            }
            g_fineWindow[Y * HW + X] = g_window[fromY * LW + fromX];
            g_from[Y * HW + X] = (Bit16u)(fromY * LW + fromX);      // (64,000 fits)
        }
    }
}

void compose() {
    const Bit8u *low = vga.fastmem;
    const bool flight = hires_wanted() && g_haveFrame && PIC_FullIndex() - g_presentMs < kStaleMs;
    // (the VGA shows 70 frames a second, the game draws 28.6: most of them are the one before)
    static Bit8u madeFrom[LW * LH];
    static Bit32u madeSerial = 0;
    static int madeFlight = -1;
    const Bit32u serial = flight ? g_frameSerial : 0;
    if (madeFlight == (int)flight && madeSerial == serial && !memcmp(madeFrom, low, LW * LH)) return;
    madeFlight = flight;
    madeSerial = serial;
    memcpy(madeFrom, low, LW * LH);
    if (!flight) {
        for (int Y = 0; Y < HH; Y++) {
            Bit8u *out = g_out + Y * HW;
            if (Y && g_lowRow[Y] == g_lowRow[Y - 1]) {
                memcpy(out, out - HW, HW);
                continue;
            }
            const Bit8u *row = low + g_lowRow[Y] * LW;
            for (int x = 0; x < LW; x++) out[2 * x] = out[2 * x + 1] = row[x];
        }
        return;
    }
    smooth_edge();
    for (int i = 0; i < HW * HH; i++) {
        const int from = g_from[i];
        g_out[i] = g_fineWindow[i] && low[from] == g_shownLow[from] ? g_shownHi[i] : low[from];
    }
}

bool game_picture() {           // the game's own 320 x 200, shown from the start of the VGA's memory
    return priv_found() && vga.mode == M_VGA && vga.config.compatible_chain4 && !vga.config.real_start;
}

}  // namespace

bool priv_hires_on() { return g_override; }

void priv_hires_set(bool on) {
    hires_wanted();
    if (g_never) return;
    g_want = on ? 1 : 0;
    g_flipped = false;
    now_wanted();
}

void priv_hires_flip() {
    hires_wanted();
    if (g_never) {
        wclog(1, "Privateer: the high-resolution picture is off for this run (the game as it was)");
        return;
    }
    g_flipped = !g_flipped;
    now_wanted();
    wclog(1, "Privateer: the high-resolution picture %s", hires_wanted() ? "on" : "off");
}

// The picture is 640 x 480 from the first time hi-res is wanted, so that the key flips without a change of mode.
void priv_hires_tick() {
    now_wanted();
    const bool want = g_used && g_hooked && game_picture();
    if (want == g_override) return;
    make_tables();
    g_override = want;
    if (want) {
        const double fps = vga.draw.delay.vtotal > 0 ? 1000.0 / vga.draw.delay.vtotal : 70.0;
        VGA_SetOverride(true);
        RENDER_SetSize(HW, HH, 8, (float)fps, 1.0, false, false);
        wclog(1, "Privateer: the picture is 640 x 480");
    } else {
        g_haveFrame = false;
        g_bufSeg = 0;
        VGA_SetOverride(false);
        wclog(1, "Privateer: the picture is the game's own");
    }
}

// (Called last of Privateer's: what is not watched yet is watched for these places alone, so that another segment's
// instruction at such an offset -- 0002, 0008, every library's entries -- goes straight on.)
void priv_hires_watch(void (*watch)(Bit16u ip)) {
    memset(g_onlyOurs, 0, sizeof(g_onlyOurs));
    now_wanted();
    g_hooked = g_used;
    if (!g_hooked) return;          // (never wanted yet: nothing of the picture's is looked at)
    for (int i = 0; i < kPlaceCount; i++) g_onlyOurs[kPlaces[i].ip] = !wc_net_watch[kPlaces[i].ip];
    for (int i = 0; i < kPlaceCount; i++) watch(kPlaces[i].ip);
    wclog(1, "Privateer: the high-resolution picture's places watched");
}

bool priv_hires_hook(Bit32u ip) {
    if (ip > 0xFFFF || !priv_found()) return false;
    const Bit16u para = (Bit16u)(SegValue(cs) - priv_load_seg());
    for (int i = 0; i < kPlaceCount; i++) {
        const Place &p = kPlaces[i];
        if (p.ip != ip || p.para != para) continue;
        if (!g_override) return true;
        const PhysPt at = SegPhys(cs) + p.ip;
        for (int k = 0; k < 3; k++) {
            if (rd8(at + k) != p.code[k]) return true;        // (not the game's code as we know it)
        }
        make_tables();
        call(p);
        return true;
    }
    return g_onlyOurs[ip];
}

}  // namespace wc

void wc_priv_hires_vsync() {
    if (!wc::g_override || !RENDER_StartUpdate()) return;
    wc::compose();
    for (int y = 0; y < wc::HH; y++) RENDER_DrawLine(wc::g_out + y * wc::HW);
    RENDER_EndUpdate(false);
}
