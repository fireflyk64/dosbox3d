/*
 *  Privateer at 640 x 400, each game pixel a block of 2 x 2 (docs/privateer.md sections 20, 26).
 *
 *  In flight the game draws the scene and the instruments into one buffer in memory, every pixel of it by a dozen
 *  routines of its graphics library (kPlaces), and shows it by copying a list of spans, the cockpit's window and
 *  screens, to the VGA (seg205:0002); the cockpit itself it drew to the VGA once.  So each call of the library on
 *  that buffer is drawn again here, from its own arguments as it is entered, into a 640 x 400 picture (H); the
 *  game's drawing is not touched and neither is its memory: one pilot's display, nothing a room could see.
 *
 *  At the span copy H is frozen with what the game shows (the buffer under the spans).  At the VGA's frame the
 *  picture is made: a pixel of the spans that still holds what was copied shows H; every other, and any screen that
 *  is not a flight's, is the game's own pixel, 2 x 2.
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
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "dosbox.h"
#include "dos_inc.h"
#include "wcnet_priv_hires.h"
#include "wcnet_priv_font.h"
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

enum { LW = 320, LH = 200, HW = 640, HH = 400 };
const double kSy = (double)HH / LH;      // (H's rows to a game row: 2, each game pixel a block of 2 x 2)
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
    kBaseStaleMs = 2000,            // a landing's or takeoff's base drawn longer ago is not looked for
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
    kBracketCaller = 0x2903,        // the target's box (its lines): one pixel wide in space alone too
    kFontCaller = 0x6108,           // the text's: each letter a silhouette of the game's font: the user's letters instead
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

int g_want = -1;                    // the page's level (WCPRIVHIRES at the launch): 0 off, 1 space, 2 all (see hires_level)
int g_level = 0;                    // what shows: the page's level, or the backquote key's since
bool g_never = false;               // WCPRIVHIRES=off, or the game as it was (priv_classic): nothing of this runs
bool g_used = false;                // wanted once: its places are watched from then on
bool g_hooked = false;              // ... and they are
bool g_override = false;
int g_setW = 0;

// The levels (the user, 2026-11-22: "Can we have a third toggle mode where reticle and cockpit are not high rez but
// sprites in space are?"; "Target boxes are but aiming and all other things are not in that third mode"): 2, all of
// it; 1, the space alone -- the ships, bases and planets drawn from their art (scaled and turned), the lasers and the
// target boxes at 640 x 400, the cockpit, its window's edge, the crosshair, the radar, every other line and the text
// the game's own pixels; 0, the game's picture.  WCPRIVHIRES=1 (or all) 2, =space 1; the page's two boxes.
inline int hires_level() {
    if (g_want < 0) {
        const char *env = getenv("WCPRIVHIRES");
        g_never = priv_classic() || (env && !strcmp(env, "off"));
        g_want = g_never || !env || !env[0] || env[0] == '0' ? 0 : !strcmp(env, "space") ? 1 : 2;
        g_level = g_want;
    }
    return g_never ? 0 : g_level;
}

inline bool hires_cockpit() { return hires_level() == 2; }

inline bool hires_wanted() {
    return hires_level() > 0;
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

// ---- the 640 x 400 picture's drawing: a game pixel is the block g_rowFrom gives (2 x 2) -------------------------------------

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
inline int fine_y(int y) { return (2 * HH * y + HH) / (2 * LH); }      // (the middle of the pixel)

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
    const double cx = 2 * x + 1, cy = (y + 0.5) * kSy, a = 2.0 * rx, b = kSy * ry;
    const int n = 8 + (int)(8 * (a > b ? a : b));
    for (int i = 0; i < n; i++) {
        const double w = i * 6.283185307179586 / n;
        plot(t, (int)floor(cx + a * cos(w)), (int)floor(cy + b * sin(w)), c);
    }
}

// ---- the game's shapes ----------------------------------------------------------------------------------------------

// A shape's run-length rows (the game's format: a word of length and kind, x and y; then the pixels, or sub-runs of a
// byte each, the odd ones one colour) read from p up to end of whatever `bytes` reads, into s (its size set).
template <class Bytes> void decode_rows(const Bytes &bytes, Bit32u p, const Bit32u end, const int x1, const int y1, Shape &s) {
    const int w = s.w, h = s.h;
    while (p + 6 <= end) {
        const Bit16u head = bytes.r16(p);
        if (!head) break;
        int x = (Bit16s)bytes.r16(p + 2) + x1;
        const int y = (Bit16s)bytes.r16(p + 4) + y1;
        p += 6;
        int n = head >> 1;
        const bool inside = y >= 0 && y < h;
        Bit16s *row = inside ? &s.px[y * w] : NULL;
        if (!(head & 1)) {
            for (; n > 0 && p < end; n--, x++, p++) {
                if (inside && x >= 0 && x < w) row[x] = bytes.r8(p);
            }
            continue;
        }
        while (n > 0 && p < end) {
            const Bit8u sub = bytes.r8(p++);
            int run = sub >> 1;
            if (!run) return;        // (not a shape: what there is)
            n -= run;
            if (sub & 1) {
                const Bit8u c = bytes.r8(p++);
                for (; run; run--, x++) {
                    if (inside && x >= 0 && x < w) row[x] = c;
                }
            } else {
                for (; run && p < end; run--, x++, p++) {
                    if (inside && x >= 0 && x < w) row[x] = bytes.r8(p);
                }
            }
        }
    }
}

struct EmuBytes {           // the game's memory
    Bit8u r8(Bit32u a) const { return rd8(a); }
    Bit16u r16(Bit32u a) const { return rd16(a); }
};

struct BufBytes {           // a file's bytes (nothing beyond them)
    const std::vector<Bit8u> &d;
    explicit BufBytes(const std::vector<Bit8u> &bytes) : d(bytes) {}
    Bit8u r8(Bit32u a) const { return a < d.size() ? d[a] : 0; }
    Bit16u r16(Bit32u a) const { return (Bit16u)(r8(a) | r8(a + 1) << 8); }
};

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
    decode_rows(EmuBytes(), at + 8, at + kMaxShapeBytes, x1, y1, s);
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

// The game's own pixels for a line (the space alone: the cockpit's lines, the radar's, the nav map's): the line's game
// pixels as blocks, as the game's own line routine steps them (Bresenham, both ends).
void line_blocks(const Target &t, int x0, int y0, int x1, int y1, Bit8u c) {
    if (x0 < -2000 || x0 > 2000 || x1 < -2000 || x1 > 2000 || y0 < -2000 || y0 > 2000 || y1 < -2000 || y1 > 2000) return;
    const int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= t.x0 && x0 <= t.x1 && y0 >= t.y0 && y0 <= t.y1) block(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ellipse_blocks(const Target &t, int x, int y, int rx, int ry, Bit8u c) {
    if (rx < 0 || ry < 0 || rx > 1000 || ry > 1000 || x < -2000 || x > 2000 || y < -2000 || y > 2000) return;
    const int n = 8 + 8 * (rx > ry ? rx : ry);
    for (int i = 0; i < n; i++) {
        const double w = i * 6.283185307179586 / n;
        const int px = (int)floor(x + rx * cos(w) + 0.5), py = (int)floor(y + ry * sin(w) + 0.5);
        if (px >= t.x0 && px <= t.x1 && py >= t.y0 && py <= t.y1) block(px, py, c);
    }
}

// One fine pixel wide: every line with the cockpit in high resolution, the target's box alone without it.
bool fine_lines(Bit16u caller) {
    return hires_cockpit() || caller == kBracketCaller;
}

// ---- rings: the line art's round parts drawn round --------------------------------------------------------------------
//
// The radar's rings and the crosshair's circle are the game's pixels at 320 x 200: joined pixel to pixel they were the
// pixels' staircase, bumpy and not round (the user, 2026-11-22: "the radar is very bumpy and not circular. Same with
// the targeting reticle").  So each colour's pixels are split into pieces (8-connected), and in each piece the ellipse
// (upright, about the piece's middle) that most of its pixels lie on is looked for: its height to width tried from 0.6
// to 1.6, its size the band of 1.2 pixels holding the most of them, then the ellipse fitted to the pixels near it
// (least squares, four rounds).  It is a ring when it has 2.5 pixels' radius at
// least and its pixels go 80 in 100 of the way round; then its pixels are drawn as that ellipse, one fine pixel wide,
// wherever the game's ring has pixels (its gaps kept), and the rest of the piece is looked at again (a ring inside a
// ring, the cross's middle).  What is no ring is joined as before, a piece touching a ring reaching its pixel's middle.
// Worked out once a shape (by its pixels' hash: the radar's art is drawn every frame); kept for 16 shapes.

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
            for (size_t i = 0; i < px.size(); i++) {
                const bool corner = i > 0 && i + 1 < px.size() && abs(px[i + 1] - px[i - 1]) == 1 && abs(py[i + 1] - py[i - 1]) == 1;
                if (corner) continue;
                if (i > 0 && (abs(px[i] - px[i - 1]) > 1 || abs(py[i] - py[i - 1]) > 1)) {
                    line_fine(t, px[i - 1], py[i - 1], px[i], py[i], (Bit8u)g.colour);
                }
                plot(t, px[i], py[i], (Bit8u)g.colour);
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

// ---- the cockpit's letters: the user's font ---------------------------------------------------------------------
//
// The text in flight (the readouts, the screens' names and distances, the comm lists and replies) is one routine's
// (para 0x6108): each letter a silhouette of the game's 3 x 4 font whose index is its character.  The user drew a font
// of their own at twice its size, one-pixel strokes, each letter in the game's letter's box (scripts/privfont/,
// wcnet_priv_font.h: "I hand made a font").  A letter is replaced only when the game draws exactly the glyph it was
// drawn from (its size, its place below the capitals' top, its pixels): any other font stays the game's blocks.
bool letter(const Target &t, const Shape &s, int x, int y, int index, Bit8u c) {
    const PrivGlyph *g = NULL;
    for (int i = 0; i < kPrivGlyphCount && !g; i++) {
        if (kPrivGlyphs[i].ch == index) g = &kPrivGlyphs[i];
    }
    if (!g || s.w != g->srcW || s.h != g->srcH || s.top != 3 - g->srcRow || s.left != 0) return false;
    for (int v = 0; v < s.h; v++) {
        int bits = 0;
        for (int u = 0; u < s.w; u++) {
            if (s.px[v * s.w + u] >= 0) bits |= 1 << u;
        }
        if (bits != g->src[v]) return false;
    }
    const int X0 = 2 * x, Y0 = 2 * (y - 3) - kPrivGlyphShiftUp;  // (the capitals' top: the anchor is the baseline's row)
    for (int r = 0; r < 10; r++) {
        for (int u = 0; u < g->w; u++) {
            if (g->rows[r] & (1 << u)) plot(t, X0 + u, Y0 + r, c);
        }
    }
    return true;
}

// ---- the cockpit's lettering that is art, in the user's font ------------------------------------------------------
//
// The user, 2026-11-23: "A couple more displays in the cockpit need the new font like AUTO and E for fuel".  Those are
// no text: AUTO is a picture of its own (28 x 4, drawn by root code 130E: two red marks each side, the letters in the
// middle), the E the top of the energy gauge's (11 x 23, root code 131A), and their letters are the art's own (a round
// A, U and O; an E five rows tall).  A table of them: the code that draws the picture, the letters' exact pattern ('#'
// the letters, '.' the panel about them, one colour; the pixels round the pattern none of the letters' colours), the
// text, and for each fine row of the letters the user's glyphs' row (their capitals, eight rows; a taller letter's stems
// drawn a row longer).  The pattern is looked for in the picture; where it is, the letters' pixels are the panel's and
// the user's glyphs are drawn there in the colour of the art's letter at (inkU, inkV), side by side with the user's
// two-pixel gaps, centred on the art's letters.  Any other picture, or the pattern not found: the game's blocks.
struct ArtWord {
    Bit16u caller;
    int w, h;
    const char *rows[6];
    const char *text;
    int inkU, inkV;
    signed char glyphRow[12];       // per fine row of the 2h: the glyphs' row (-1 none)
};
// (each found in the four cockpits the player flies, scratch cockpit.sh: AUTO from 130E, the energy gauge's E from
//  131A, the Centurion's fuel gauge's F from 1356.  An eight-row glyph in a five-row letter: centred (the A's slant
//  kept whole), but the E, whose stems are drawn a row longer each to fill it.)
const ArtWord kArtWords[] = {
    { 0x130E, 16, 4, { ".#..#..#.###..#.",               // the Tarsus's AUTO: a round A and O
                       "#.#.#..#..#..#.#",
                       "###.#..#..#..#.#",
                       "#.#..##...#...#." }, "AUTO", 0, 1, { 0, 1, 2, 3, 4, 5, 6, 7 } },
    { 0x130E, 20, 4, { "####.#..#.#####.####",           // the Centurion's: square letters
                       "#..#.#..#...#...#..#",
                       "####.#..#...#...#..#",
                       "#..#.####...#...####" }, "AUTO", 0, 1, { 0, 1, 2, 3, 4, 5, 6, 7 } },
    { 0x130E, 21, 5, { ".##...#..#..###...##.",          // the Orion's: five rows
                       "#..#..#..#...#...#..#",
                       "####..#..#...#...#..#",
                       "#..#..#..#...#...#..#",
                       "#..#...##....#....##." }, "AUTO", 0, 1, { -1, 0, 1, 2, 3, 4, 5, 6, 7, -1 } },
    { 0x130E, 25, 5, { ".##...#...#..#####...###.",      // the Galaxy's: five rows, wide
                       "#..#..#...#....#....#...#",
                       "####..#...#....#....#...#",
                       "#..#..#...#....#....#...#",
                       "#..#...###.....#.....###." }, "AUTO", 0, 1, { -1, 0, 1, 2, 3, 4, 5, 6, 7, -1 } },
    { 0x131A, 3, 5, { "###",                            // the energy gauge's E: the Tarsus's, Orion's, Galaxy's
                      "#..",
                      "##.",
                      "#..",
                      "###" }, "E", 0, 1, { 0, 1, 2, 2, 3, 4, 5, 6, 6, 7 } },
    { 0x131A, 4, 5, { "####",                           // the Centurion's
                      "#...",
                      "###.",
                      "#...",
                      "####" }, "E", 0, 1, { 0, 1, 2, 2, 3, 4, 5, 6, 6, 7 } },
    { 0x1356, 3, 4, { "###",                            // the Centurion's fuel gauge's F
                      "#..",
                      "##.",
                      "#.." }, "F", 0, 1, { 0, 1, 2, 3, 4, 5, 6, 7 } },
};

const PrivGlyph *user_glyph(char ch) {
    for (int i = 0; i < kPrivGlyphCount; i++) {
        if (kPrivGlyphs[i].ch == (unsigned char)ch) return &kPrivGlyphs[i];
    }
    return NULL;
}

void shape_blocks(const Target &t, const Shape &s, int x, int y, int colour);

bool art_word(const Target &t, Bit16u caller, const Shape &s, int x, int y) {
    for (size_t e = 0; e < sizeof(kArtWords) / sizeof(kArtWords[0]); e++) {
        const ArtWord &a = kArtWords[e];
        if (a.caller != caller || s.w < a.w || s.h < a.h) continue;
        const PrivGlyph *g[8];
        const int n = (int)strlen(a.text);
        int inkW = 0;
        bool have = n <= 8;
        for (int i = 0; i < n && have; i++) {
            g[i] = user_glyph(a.text[i]);
            have = g[i] != NULL;
            if (have) inkW += g[i]->w;
        }
        const int fineW = inkW + 2 * (n - 1);
        if (!have || fineW > 2 * a.w + 2) continue;
        auto px = [&](int u, int v) { return u >= 0 && v >= 0 && u < s.w && v < s.h ? (int)s.px[v * s.w + u] : -1; };
        for (int pv = 0; pv + a.h <= s.h; pv++) {
            for (int pu = 0; pu + a.w <= s.w; pu++) {
                int bg = -2, ink[2] = { -2, -2 };
                for (int v = 0; v < a.h && bg == -2; v++) {     // (the panel's colour: the pattern's first '.')
                    for (int u = 0; u < a.w && bg == -2; u++) {
                        if (a.rows[v][u] == '.') bg = px(pu + u, pv + v);
                    }
                }
                bool ok = bg != -2;
                for (int v = 0; v < a.h && ok; v++) {
                    for (int u = 0; u < a.w && ok; u++) {
                        const int c = px(pu + u, pv + v);
                        if (a.rows[v][u] == '.') {
                            ok = c == bg;
                        } else {                                // (a letter: of one or two colours, not the panel's)
                            ok = c >= 0 && c != bg;
                            if (!ok) break;
                            if (ink[0] == -2) ink[0] = c;
                            else if (c != ink[0]) {
                                if (ink[1] == -2) ink[1] = c;
                                ok = c == ink[1];
                            }
                        }
                    }
                }
                for (int v = -1; v <= a.h && ok; v++) {         // (no letter pixel of its colours just outside)
                    for (int u = -1; u <= a.w && ok; u++) {
                        if (u >= 0 && v >= 0 && u < a.w && v < a.h) continue;
                        const int c = px(pu + u, pv + v);
                        ok = c < 0 || (c != ink[0] && c != ink[1]);
                    }
                }
                if (!ok) continue;
                const int colour = px(pu + a.inkU, pv + a.inkV);
                Shape plain = s;
                for (int v = 0; v < a.h; v++) {
                    for (int u = 0; u < a.w; u++) {
                        if (a.rows[v][u] == '#') plain.px[(pv + v) * s.w + pu + u] = (Bit16s)bg;
                    }
                }
                shape_blocks(t, plain, x, y, -1);
                int X = 2 * (x - s.left + pu) + (2 * a.w - fineW) / 2;
                const int Y0 = 2 * (y - s.top + pv);
                for (int i = 0; i < n; i++) {
                    for (int r = 0; r < 2 * a.h && r < 12; r++) {
                        const int gr = a.glyphRow[r];
                        if (gr < 0 || gr > 9) continue;
                        for (int u = 0; u < g[i]->w; u++) {
                            if (g[i]->rows[gr] & (1 << u)) plot(t, X + u, Y0 + r, (Bit8u)colour);
                        }
                    }
                    X += g[i]->w + 2;
                }
                return true;
            }
        }
    }
    return false;
}

// ---- the target display's ship: straight lines, mirrored exactly ---------------------------------------------------
//
// The user, 2026-11-22: "we could try a variant for target craft where they actually had diagonal lines instead of
// stair steps.... like the top part of the sloping downward talon wing should have been a single line not multiple
// stair steps"; 2026-11-23: "try this with the uprezzed wireframes--but we need to ensure the wireframes are symmetric
// exactly".  The wireframe is line art in a few colours.  Each colour's pixels are a graph: a pixel joined to its
// neighbours of the colour, a diagonal one only where no corner pixel of the colour joins the pair.  Chains of pixels
// with two neighbours, between ends and junctions (or round a loop), are simplified to straight segments (Douglas and
// Peucker, within 0.75 of a pixel: a staircase of any slope is one segment, a corner stays) and drawn one fine pixel
// wide between the pixels' middles.  A pixel in a square of four of its colour is a solid part, the game's 2 x 2 block.
// The display draws the ship in parts, each in the same frame (the hull's halves, its middle, each shield): they are
// gathered into one picture as the game lays them and drawn whole (wire_gather, wire_flush).  Chains of a colour that
// meet at a pixel and together run straight are one line (the Talon's wing edges).  The drawing is made on a canvas of
// the picture's own fine pixels; when the picture is a mirror image of itself (its
// pixels' outline 60 in 100 the same about one column or the line between two: the shields' arcs, one side's to its
// state, differ), wherever the art is the same as its mirror about a pixel the canvas's left half is copied onto its
// right, pixel for pixel, each copy taking its colour from the art's right half there (a side's damage shows) -- so
// the hull is drawn exactly symmetric whatever the line's rounding did, and a side's own state stays its own.
// 2026-11-23, the user's notes on every ship ("Paradign should have every pod be rounded", "Stilettos back engines
// should be flat", "Tarsis front cockpit should connect through the 2 weapon pods"): a piece of four pixels or fewer
// touching a solid part is solid; a colour's loop round a small hole that fits an upright ellipse is drawn as one; a
// line's end reaches half-way to a touching pixel of another colour; every part is drawn so (no part judged a picture),
// except the parts on kWireBlockParts ("I rather liked the blocky front of the talon").  Curves through the hulls'
// corners were tried and taken out ("the gothri and dralthi were acceptable before and this edit made it way worse").
// Worked out once a shape (its hash), 8 kept; shapes over 96 x 96 stay the game's blocks.

struct Wireframe {
    Bit32u hash;
    int w, h;
    int axis;                       // u + u' of a mirrored pair of the art's columns, -1: not a mirror image
    std::vector<Bit16s> fine;       // 2w x 2h: a colour, or -1
};
enum { kWireCache = 8, kWireMax = 96, kChainMax = 4096, kMaxChains = 512 };

// The chain's points simplified (Douglas and Peucker, within tol), its ends kept: the indices kept, in order.
void simplify(const std::vector<int> &cu, const std::vector<int> &cv, double tol, std::vector<int> &keep) {
    const int n = (int)cu.size();
    std::vector<char> on(n, 0);
    on[0] = on[n - 1] = 1;
    std::vector<std::pair<int, int> > stack(1, std::make_pair(0, n - 1));
    while (!stack.empty()) {
        const int a = stack.back().first, b = stack.back().second;
        stack.pop_back();
        if (b - a < 2) continue;
        const double dx = cu[b] - cu[a], dy = cv[b] - cv[a], len = sqrt(dx * dx + dy * dy);
        double best = -1;
        int k = a;
        for (int i = a + 1; i < b; i++) {
            const double ex = cu[i] - cu[a], ey = cv[i] - cv[a];
            const double d = len > 0 ? fabs(dx * ey - dy * ex) / len : sqrt(ex * ex + ey * ey);
            if (d > best) { best = d; k = i; }
        }
        if (best <= tol) continue;
        on[k] = 1;
        stack.push_back(std::make_pair(a, k));
        stack.push_back(std::make_pair(k, b));
    }
    keep.clear();
    for (int i = 0; i < n; i++) {
        if (on[i]) keep.push_back(i);
    }
}

void wire_make(const Shape &s, const std::vector<char> &block, Wireframe &out) {
    const int w = s.w, h = s.h, W = 2 * w, H = 2 * h;
    out.w = w;
    out.h = h;
    out.axis = -1;
    out.fine.assign(W * H, (Bit16s)-1);
    auto col = [&](int u, int v) { return u >= 0 && v >= 0 && u < w && v < h ? (int)s.px[v * w + u] : -1; };
    auto put = [&](int X, int Y, int c) { if (X >= 0 && Y >= 0 && X < W && Y < H) out.fine[Y * W + X] = (Bit16s)c; };
    auto drawf = [&](double x0, double y0, double x1, double y1, int c) {
        const int X0 = (int)floor(x0 + 0.5), Y0 = (int)floor(y0 + 0.5), X1 = (int)floor(x1 + 0.5), Y1 = (int)floor(y1 + 0.5);
        int X = X0, Y = Y0;
        const int dx = abs(X1 - X), dy = -abs(Y1 - Y), sx = X < X1 ? 1 : -1, sy = Y < Y1 ? 1 : -1;
        int err = dx + dy;
        for (int n = 0; n < 4 * (W + H); n++) {
            put(X, Y, c);
            if (X == X1 && Y == Y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; X += sx; }
            if (e2 <= dx) { err += dx; Y += sy; }
        }
    };
    static const int kDu8[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, kDv8[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    // solid parts: a pixel in a square of four of its colour, the game's block
    std::vector<char> solid(w * h, 0);
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            const int c = col(u, v);
            if (c < 0) continue;
            if ((size_t)(v * w + u) < block.size() && block[v * w + u]) solid[v * w + u] = 1;
            for (int dv = -1; dv <= 0 && !solid[v * w + u]; dv++) {
                for (int du = -1; du <= 0; du++) {
                    if (col(u + du, v + dv) == c && col(u + du + 1, v + dv) == c && col(u + du, v + dv + 1) == c &&
                        col(u + du + 1, v + dv + 1) == c) {
                        solid[v * w + u] = 1;
                        break;
                    }
                }
            }
        }
    }
    // A piece of four pixels or fewer of a colour touching a solid part is solid too (the Stiletto's engines: a yellow
    // block and the orange row at its end -- the user: "Stilettos back engines should be flat")
    {
        std::vector<char> seen(w * h, 0);
        std::vector<int> piece;
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                const int c = col(u, v);
                if (c < 0 || solid[v * w + u] || seen[v * w + u]) continue;
                piece.assign(1, v * w + u);
                seen[v * w + u] = 1;
                // (the piece grown whole, at most the picture: one cut short at five left its rest unmarked, and
                //  each bit of a long line then counted as a small piece of its own: chance blocks)
                for (size_t i = 0; i < piece.size() && piece.size() <= (size_t)(w * h); i++) {
                    const int pu = piece[i] % w, pv = piece[i] / w;
                    for (int k = 0; k < 8; k++) {
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        if (col(nu, nv) == c && !solid[nv * w + nu] && !seen[nv * w + nu]) {
                            seen[nv * w + nu] = 1;
                            piece.push_back(nv * w + nu);
                        }
                    }
                }
                if (piece.size() > 4) continue;
                bool touches = false;
                for (size_t i = 0; i < piece.size() && !touches; i++) {
                    const int pu = piece[i] % w, pv = piece[i] / w;
                    for (int k = 0; k < 8 && !touches; k += 2) {        // (the four orthogonal neighbours)
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        touches = nu >= 0 && nv >= 0 && nu < w && nv < h && solid[nv * w + nu] == 1;
                    }
                }
                if (touches) {
                    for (size_t i = 0; i < piece.size(); i++) solid[piece[i]] = 2;
                }
            }
        }
    }
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            if (!solid[v * w + u]) continue;
            const int c = col(u, v);
            put(2 * u, 2 * v, c);
            put(2 * u + 1, 2 * v, c);
            put(2 * u, 2 * v + 1, c);
            put(2 * u + 1, 2 * v + 1, c);
        }
    }
    // A colour's loop round a hole that is an ellipse is drawn as one (the Paradigm's pods: the user, "Paradign should
    // have every pod be rounded"): for each colour, a region of other pixels (4-connected) that touches no edge, of 200
    // pixels at most; its loop, the colour's pixels about it; not a box (two corners of its bounding box its own
    // pixels: the Tarsus's cells stay square), an upright ellipse fitted (least squares) with radii of 1.5 to 14 pixels
    // and every pixel of the loop within 0.8 of it.  Its pixels are then the ellipse's: no chain's; a chain reaching one
    // ends at its middle.
    std::vector<int> ring(w * h, 0);
    {
        std::vector<int> region(w * h, 0), stamp(w * h, 0), comp, loop;
        int stampNo = 0, rings = 0;
        std::vector<int> colours;
        for (int i = 0; i < w * h; i++) {
            if (s.px[i] >= 0 && std::find(colours.begin(), colours.end(), (int)s.px[i]) == colours.end() && colours.size() < 32) {
                colours.push_back(s.px[i]);
            }
        }
        for (size_t ci = 0; ci < colours.size() && rings < 64; ci++) {
            const int c = colours[ci];
            std::fill(region.begin(), region.end(), 0);
            for (int start = 0; start < w * h && rings < 64; start++) {
                if (region[start] || col(start % w, start / w) == c) continue;
                comp.assign(1, start);
                region[start] = 1;
                bool edge = false;
                // (the region flooded whole, at most the picture: one cut short left its rest unmarked, each piece of
                //  it then taken for a hole of its own -- chance rings, moved by a change anywhere in the picture)
                for (size_t i = 0; i < comp.size() && comp.size() <= (size_t)(w * h); i++) {
                    const int pu = comp[i] % w, pv = comp[i] / w;
                    if (pu == 0 || pv == 0 || pu == w - 1 || pv == h - 1) edge = true;
                    for (int k = 0; k < 8; k += 2) {
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        if (nu < 0 || nv < 0 || nu >= w || nv >= h || region[nv * w + nu] || col(nu, nv) == c) continue;
                        region[nv * w + nu] = 1;
                        comp.push_back(nv * w + nu);
                    }
                }
                if (edge || comp.size() > 200) continue;
                stampNo++;
                loop.clear();
                bool line = true;                       // (a loop along a part kept as blocks is no ring: the Talon's)
                for (size_t i = 0; i < comp.size() && line; i++) {
                    const int pu = comp[i] % w, pv = comp[i] / w;
                    for (int k = 0; k < 8 && line; k++) {
                        const int nu = pu + kDu8[k], nv = pv + kDv8[k];
                        if (col(nu, nv) == c && (size_t)(nv * w + nu) < block.size() && block[nv * w + nu]) line = false;
                        if (col(nu, nv) != c || solid[nv * w + nu] || stamp[nv * w + nu] == stampNo) continue;
                        stamp[nv * w + nu] = stampNo;
                        loop.push_back(nv * w + nu);
                    }
                }
                if (!line || loop.size() < 6) continue;
                int u0 = w, u1 = -1, v0 = h, v1 = -1;
                for (size_t i = 0; i < loop.size(); i++) {
                    const int pu = loop[i] % w, pv = loop[i] / w;
                    u0 = std::min(u0, pu); u1 = std::max(u1, pu); v0 = std::min(v0, pv); v1 = std::max(v1, pv);
                }
                const int corners = (stamp[v0 * w + u0] == stampNo) + (stamp[v0 * w + u1] == stampNo) +
                                    (stamp[v1 * w + u0] == stampNo) + (stamp[v1 * w + u1] == stampNo);
                if (corners >= 2) continue;
                const double cx = (u0 + u1) / 2.0, cy = (v0 + v1) / 2.0;
                double M[4][5] = { { 0 } };
                for (size_t i = 0; i < loop.size(); i++) {
                    const double du = loop[i] % w - cx, dv = loop[i] / w - cy, f[4] = { du * du, dv * dv, du, dv };
                    for (int a = 0; a < 4; a++) {
                        M[a][4] += f[a];
                        for (int b = 0; b < 4; b++) M[a][b] += f[a] * f[b];
                    }
                }
                double sol[4];
                if (!solve4(M, sol) || sol[0] <= 0 || sol[1] <= 0) continue;
                const double ex = cx - sol[2] / (2 * sol[0]), ey = cy - sol[3] / (2 * sol[1]);
                const double K = 1 + sol[2] * sol[2] / (4 * sol[0]) + sol[3] * sol[3] / (4 * sol[1]);
                if (K <= 0) continue;
                const double ea = sqrt(K / sol[0]), eb = sqrt(K / sol[1]);
                if (ea < 1.5 || eb < 1.5 || ea > 14 || eb > 14) continue;
                bool fits = true;
                for (size_t i = 0; i < loop.size() && fits; i++) {
                    const double du = loop[i] % w - ex, dv = loop[i] / w - ey;
                    const double rho = sqrt((du / ea) * (du / ea) + (dv / eb) * (dv / eb));
                    fits = rho > 1e-9 && sqrt(du * du + dv * dv) * fabs(1 - 1 / rho) <= 0.8;
                }
                if (!fits) continue;
                rings++;
                for (size_t i = 0; i < loop.size(); i++) ring[loop[i]] = rings;
                const int n = 16 + (int)(16 * std::max(ea, eb));
                double px0 = 2 * (ex + ea) + 1, py0 = 2 * ey + 1;
                for (int i = 1; i <= n; i++) {
                    const double th = i * 6.283185307179586 / n;
                    const double px1 = 2 * (ex + ea * cos(th)) + 1, py1 = 2 * (ey + eb * sin(th)) + 1;
                    drawf(px0, py0, px1, py1, c);
                    px0 = px1;
                    py0 = py1;
                }
            }
        }
    }
    // each pixel's links (bit k: neighbour k) to its colour, a diagonal only where no corner of the colour joins them
    static const int kDu[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, kDv[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    std::vector<Bit8u> link(w * h, 0), used(w * h, 0), degree(w * h, 0);
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            const int c = col(u, v);
            if (c < 0 || solid[v * w + u] || ring[v * w + u]) continue;
            for (int k = 0; k < 8; k++) {
                const int nu = u + kDu[k], nv = v + kDv[k];
                if (col(nu, nv) != c) continue;
                if (kDu[k] && kDv[k] && (col(nu, v) == c || col(u, nv) == c)) continue;
                link[v * w + u] |= (Bit8u)(1 << k);
                degree[v * w + u]++;
            }
        }
    }
    // the chains: pixels of two links between ends and junctions (or round a loop), each link walked once
    struct Chain { int col; std::vector<int> u, v; bool alive; };
    std::vector<Chain> chains;
    auto walk = [&](int pu, int pv, int k) {
        Chain c;
        c.col = col(pu, pv);
        c.alive = true;
        c.u.assign(1, pu);
        c.v.assign(1, pv);
        int u = pu, v = pv;
        for (int steps = 0; steps < kChainMax; steps++) {
            used[v * w + u] |= (Bit8u)(1 << k);
            const int nu = u + kDu[k], nv = v + kDv[k];
            used[nv * w + nu] |= (Bit8u)(1 << ((k + 4) & 7));
            c.u.push_back(nu);
            c.v.push_back(nv);
            u = nu;
            v = nv;
            if (solid[v * w + u] || ring[v * w + u] || degree[v * w + u] != 2) break;
            int next = -1;
            for (int j = 0; j < 8 && next < 0; j++) {
                if ((link[v * w + u] & (1 << j)) && !(used[v * w + u] & (1 << j))) next = j;
            }
            if (next < 0) break;
            k = next;
        }
        if (chains.size() < kMaxChains) chains.push_back(c);
    };
    for (int pass = 0; pass < 2; pass++) {          // (from the ends and junctions first, then what is left: loops)
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                const int i = v * w + u;
                if (col(u, v) < 0 || solid[i] || ring[i] || (pass == 0 && degree[i] == 2)) continue;
                for (int k = 0; k < 8; k++) {
                    if ((link[i] & (1 << k)) && !(used[i] & (1 << k))) walk(u, v, k);
                }
            }
        }
    }
    // A straight line through a junction is one line (the user, 2026-11-23: "the talon's leading edge wings are
    // straight, but the system drew it as a subtle curve": the wing's 45-degree edge, met half-way by an inner stroke,
    // was two chains, each simplified to its own slope).  Of the chains of a colour that end at one pixel, the pair
    // that together simplify to no corner there (within the tolerance) is joined -- the straightest pair first, again
    // until no pair is left; a closed loop and a chain's own two ends are not joined.
    std::vector<int> keep, cu, cv;
    auto cat = [&](const Chain &A, bool aEndsHere, const Chain &B, bool bStartsHere) {
        cu.clear();
        cv.clear();
        const int na = (int)A.u.size(), nb = (int)B.u.size();
        for (int i = 0; i < na; i++) {
            const int j = aEndsHere ? i : na - 1 - i;
            cu.push_back(A.u[j]);
            cv.push_back(A.v[j]);
        }
        for (int i = 1; i < nb; i++) {
            const int j = bStartsHere ? i : nb - 1 - i;
            cu.push_back(B.u[j]);
            cv.push_back(B.v[j]);
        }
        return na - 1;                              // (the junction's index in the joined chain)
    };
    for (int round = 0; round < kMaxChains; round++) {
        double best = 1e9;
        int bi = -1, bj = -1;
        bool bEi = false, bEj = false;
        for (size_t i = 0; i < chains.size(); i++) {
            const Chain &A = chains[i];
            if (!A.alive || A.u.size() < 2 || (A.u.front() == A.u.back() && A.v.front() == A.v.back())) continue;
            for (int ei = 0; ei < 2; ei++) {
                const int ju = ei ? A.u.back() : A.u.front(), jv = ei ? A.v.back() : A.v.front();
                for (size_t j = i + 1; j < chains.size(); j++) {
                    const Chain &B = chains[j];
                    if (!B.alive || B.col != A.col || B.u.size() < 2 || (B.u.front() == B.u.back() && B.v.front() == B.v.back())) continue;
                    for (int ej = 0; ej < 2; ej++) {
                        const int ku = ej ? B.u.back() : B.u.front(), kv = ej ? B.v.back() : B.v.front();
                        if (ku != ju || kv != jv) continue;
                        const int at = cat(A, ei == 1, B, ej == 0);
                        simplify(cu, cv, 0.75, keep);
                        int k0 = -1, k1 = -1;
                        bool corner = false;
                        for (size_t k = 0; k < keep.size(); k++) {
                            if (keep[k] == at) corner = true;
                            if (keep[k] < at) k0 = keep[k];
                            if (keep[k] > at && k1 < 0) k1 = keep[k];
                        }
                        if (corner || k0 < 0 || k1 < 0) continue;
                        const double dx = cu[k1] - cu[k0], dy = cv[k1] - cv[k0], len = sqrt(dx * dx + dy * dy);
                        const double d = len > 0 ? fabs(dx * (cv[at] - cv[k0]) - dy * (cu[at] - cu[k0])) / len : 0;
                        if (d < best) { best = d; bi = (int)i; bj = (int)j; bEi = ei == 1; bEj = ej == 0; }
                    }
                }
            }
        }
        if (bi < 0) break;
        cat(chains[bi], bEi, chains[bj], bEj);
        chains[bi].u = cu;
        chains[bi].v = cv;
        chains[bj].alive = false;
    }
    // each chain's straight segments, drawn between the pixels' middles; a chain ending where another's line passes
    // through (a junction) ends on that line, the nearest point of it within 1.5 pixels
    struct Poly { int col; std::vector<double> x, y; const Chain *chain; };
    std::vector<Poly> polys;
    for (size_t i = 0; i < chains.size(); i++) {
        const Chain &c = chains[i];
        if (!c.alive || c.u.size() < 2) continue;
        simplify(c.u, c.v, 0.75, keep);
        Poly p;
        p.col = c.col;
        p.chain = &c;
        for (size_t k = 0; k < keep.size(); k++) {
            p.x.push_back(c.u[keep[k]]);
            p.y.push_back(c.v[keep[k]]);
        }
        polys.push_back(p);
    }
    auto snap = [&](double &px, double &py, size_t own) {
        double best = 1.5;
        double bx = px, by = py;
        for (size_t k = 0; k < polys.size(); k++) {
            if (k == own || polys[k].col != polys[own].col) continue;
            const Chain &c = *polys[k].chain;
            bool through = false;                       // (the point one of its inner pixels)
            for (size_t i = 1; i + 1 < c.u.size() && !through; i++) through = c.u[i] == px && c.v[i] == py;
            if (!through) continue;
            for (size_t i = 0; i + 1 < polys[k].x.size(); i++) {
                const double ax = polys[k].x[i], ay = polys[k].y[i], dx = polys[k].x[i + 1] - ax, dy = polys[k].y[i + 1] - ay;
                const double l2 = dx * dx + dy * dy;
                double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                const double qx = ax + t * dx, qy = ay + t * dy, d = sqrt((qx - px) * (qx - px) + (qy - py) * (qy - py));
                if (d < best) { best = d; bx = qx; by = qy; }
            }
        }
        px = bx;
        py = by;
    };
    std::vector<char> inChain(w * h, 0);
    for (size_t k = 0; k < polys.size(); k++) {
        Poly &p = polys[k];
        snap(p.x.front(), p.y.front(), k);
        snap(p.x.back(), p.y.back(), k);
        for (size_t i = 0; i + 1 < p.x.size(); i++) {
            drawf(2 * p.x[i] + 1, 2 * p.y[i] + 1, 2 * p.x[i + 1] + 1, 2 * p.y[i + 1] + 1, p.col);
        }
        for (size_t i = 0; i < p.chain->u.size(); i++) inChain[p.chain->v[i] * w + p.chain->u[i]] = 1;
    }
    for (int v = 0; v < h; v++) {                   // (a pixel in no chain, nor solid, nor a ring's: its middle)
        for (int u = 0; u < w; u++) {
            if (col(u, v) >= 0 && !solid[v * w + u] && !ring[v * w + u] && !inChain[v * w + u]) put(2 * u + 1, 2 * v + 1, col(u, v));
        }
    }
    // A line's end reaches half-way to a pixel of another colour that touches it (a diagonal one only where no corner
    // pixel of another colour comes between): the colours meet as the game's pixels do (the Tarsus's cockpit bar and its
    // weapon pods: the user, "Tarsis front cockpit should connect through the 2 weapon pods. There shouldn't be gaps")
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            const int i = v * w + u, c = col(u, v);
            if (c < 0 || solid[i] || ring[i] || degree[i] > 1) continue;
            for (int k = 0; k < 8; k++) {
                const int nu = u + kDu[k], nv = v + kDv[k], nc = col(nu, nv);
                if (nc < 0 || nc == c) continue;
                if (kDu[k] && kDv[k]) {
                    const int a = col(nu, v), b = col(u, nv);
                    if ((a >= 0 && a != c) || (b >= 0 && b != c)) continue;
                }
                drawf(2 * u + 1, 2 * v + 1, u + nu + 1, v + nv + 1, c);      // (to the middle between the two)
            }
        }
    }
    // a mirror image?  The axis (u + u' of a mirrored pair) near the middle that most of the outline agrees with
    int lit = 0, best = -1, bestAgree = -1;
    for (int i = 0; i < w * h; i++) lit += s.px[i] >= 0;
    for (int axis = w - 3; axis <= w + 1; axis++) {
        int agree = 0;
        for (int v = 0; v < h; v++) {
            for (int u = 0; u < w; u++) {
                if (s.px[v * w + u] >= 0 && col(axis - u, v) >= 0) agree++;
            }
        }
        if (agree > bestAgree) { bestAgree = agree; best = axis; }
    }
    if (w < 8 || lit == 0 || bestAgree * 100 < lit * 60) return;
    out.axis = best;
    // the right half's pixels where the art is a mirror image there (its outline in the 3 x 3 pixels about it the same
    // as about its mirror) are the left half's copied, pixel for pixel; elsewhere (a side's shields, which differ) each
    // half keeps its own drawing.  A copy's colour is the art's own there (its pixel, or the nearest lit one about it
    // whose mirror is the left's colour, or any lit one): a side's damage shows.
    const int sum = 2 * best + 1;                  // (fine X and sum - X are mirror images)
    if (best % 2 == 0) {
        // A column on the axis itself (best even: u and best - u the same column): its two fine columns are each
        // other's mirror, so a line along it, drawn in its right one (the pixels' middles are 2u + 1), is drawn in
        // both -- two pixels wide, the one way it is a mirror image of itself
        const int XL = best, XR = best + 1;
        for (int Y = 0; Y < H; Y++) {
            if (XR < W && out.fine[Y * W + XR] >= 0 && out.fine[Y * W + XL] < 0) out.fine[Y * W + XL] = out.fine[Y * W + XR];
        }
    }
    std::vector<Bit16s> drawn(out.fine);
    auto mirrored = [&](int u, int v) {
        for (int dv = -1; dv <= 1; dv++) {
            for (int du = -1; du <= 1; du++) {
                if ((col(u + du, v + dv) >= 0) != (col(best - u - du, v + dv) >= 0)) return false;
            }
        }
        return true;
    };
    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            const int M = sum - X;                      // (X here the right half's, M its mirror in the left)
            if (M >= X || M < 0 || !mirrored(X >> 1, Y >> 1)) continue;
            const Bit16s c = drawn[Y * W + M];
            if (c < 0) {
                out.fine[Y * W + X] = (Bit16s)-1;
                continue;
            }
            const int u = X >> 1, v = Y >> 1;
            bool same = true;                           // (the art's colours a mirror image here too: the copy's own)
            for (int dv = -1; dv <= 1 && same; dv++) {
                for (int du = -1; du <= 1 && same; du++) same = col(u + du, v + dv) == col(best - u - du, v + dv);
            }
            if (same) {
                out.fine[Y * W + X] = c;
                continue;
            }
            int pick = -1, any = -1;
            for (int k = -1; k < 8 && pick < 0; k++) {
                const int qu = k < 0 ? u : u + kDu[k], qv = k < 0 ? v : v + kDv[k];
                const int qc = col(qu, qv);
                if (qc < 0) continue;
                if (any < 0) any = qc;
                if (col(best - qu, qv) == c) pick = qc;
            }
            out.fine[Y * W + X] = (Bit16s)(pick >= 0 ? pick : any >= 0 ? any : c);
        }
    }
}

// The target display's ship is drawn by a routine of an overlay (its segment wherever the overlay manager put it: at
// E613 in one run, elsewhere in the next), four calls of the plain shape routine, the hull and the shields' parts, each
// at the routine's same place: so it is known by its call -- its return's offset and the code before it, `push word
// [bp-1Ch]; lea ax,[bp-68h]; push ax; call far 0008:` (the call's segment is relocated: not compared).
bool target_art_call() {
    static const Bit8u kSite[10] = { 0xFF, 0x76, 0xE4, 0x8D, 0x46, 0x98, 0x50, 0x9A, 0x08, 0x00 };
    const Bit16u retIp = rd16(SegPhys(ss) + (Bit16u)reg_sp), retCs = rd16(SegPhys(ss) + (Bit16u)(reg_sp + 2));
    if (retIp != 0x039A && retIp != 0x043B && retIp != 0x04C2 && retIp != 0x054A) return false;
    const PhysPt at = ((PhysPt)retCs << 4) + (Bit16u)(retIp - 12);
    for (int i = 0; i < 10; i++) {
        if (rd8(at + i) != kSite[i]) return false;
    }
    return true;
}

// The parts of one target display (the hull's halves, its middle, the shields' arcs: each a shape in one frame, drawn
// one after another) are gathered into one picture as the game lays them, and that is drawn when anything else is
// drawn next (or the frame is shown): so its lines run on across the parts, and its mirror image is the whole ship's
// (the halves are not each other's mirrors: the drone's left half has the middle column's pixels, the Gothri's right).
//
// A part on this list stays the game's blocks: the Talon's front (its canopy and engines, 94 pixels; the user: "I rather
// liked the blocky front of the talon", "a special exemption for talon is fine").  A part is known by its frame and
// which of its pixels are lit (not their colours: those change with its damage): FNV-1a of w, h, left, top, then lit.
const Bit32u kWireBlockParts[] = { 0xC7EFE729u };

Bit32u part_mask_hash(const Shape &s) {
    Bit32u h = 2166136261u;
    const int head[4] = { s.w, s.h, s.left, s.top };
    for (int i = 0; i < 4; i++) h = (h ^ (Bit32u)head[i]) * 16777619u;
    for (size_t i = 0; i < s.px.size(); i++) h = (h ^ (Bit32u)(s.px[i] >= 0 ? 1 : 0)) * 16777619u;
    return h;
}

Shape g_wireComp;
std::vector<char> g_wireBlock;      // per pixel of it: from a part on the list above, drawn as the game's blocks
bool g_wirePending = false;
Target g_wireT;
int g_wireX = 0, g_wireY = 0;

void shape_wire(const Target &t, const Shape &s, const std::vector<char> &block, int x, int y);

void wire_flush() {
    if (!g_wirePending) return;
    g_wirePending = false;
    shape_wire(g_wireT, g_wireComp, g_wireBlock, g_wireX, g_wireY);
}

void wire_gather(const Target &t, const Shape &s, int x, int y) {
    if (g_wirePending && (s.w != g_wireComp.w || s.h != g_wireComp.h || s.left != g_wireComp.left || s.top != g_wireComp.top ||
                          x != g_wireX || y != g_wireY || t.seg != g_wireT.seg)) {
        wire_flush();
    }
    if (!g_wirePending) {
        g_wireComp.w = s.w;
        g_wireComp.h = s.h;
        g_wireComp.left = s.left;
        g_wireComp.top = s.top;
        g_wireComp.stamp = 0;
        g_wireComp.px.assign(s.w * s.h, (Bit16s)-1);
        g_wireBlock.assign(s.w * s.h, 0);
        g_wireT = t;
        g_wireX = x;
        g_wireY = y;
        g_wirePending = true;
    }
    const Bit32u part = part_mask_hash(s);
    char keep = 0;
    for (size_t i = 0; i < sizeof(kWireBlockParts) / sizeof(kWireBlockParts[0]); i++) keep |= kWireBlockParts[i] == part;
    for (size_t i = 0; i < s.px.size() && i < g_wireComp.px.size(); i++) {
        if (s.px[i] >= 0) {
            g_wireComp.px[i] = s.px[i];
            g_wireBlock[i] = keep;
        }
    }
}

void shape_wire(const Target &t, const Shape &s, const std::vector<char> &block, int x, int y) {
    static Wireframe cache[kWireCache];
    static int used = 0, next = 0;
    Bit32u hash = shape_hash(s);
    for (size_t i = 0; i < block.size(); i++) hash = (hash ^ (Bit32u)block[i]) * 16777619u;
    const Wireframe *wf = NULL;
    for (int i = 0; i < used && !wf; i++) {
        if (cache[i].hash == hash && cache[i].w == s.w && cache[i].h == s.h) wf = &cache[i];
    }
    if (!wf) {
        Wireframe &made = cache[next];
        next = (next + 1) % kWireCache;
        if (used < kWireCache) used++;
        made.hash = hash;
        wire_make(s, block, made);
        wf = &made;
    }
    const int X0 = 2 * (x - s.left), Y0 = 2 * (y - s.top), W = 2 * wf->w, H = 2 * wf->h;
    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            const Bit16s c = wf->fine[Y * W + X];
            if (c >= 0) plot(t, X0 + X, Y0 + Y, (Bit8u)c);
        }
    }
}

// Line art of one pixel's width: each pixel joined to its neighbours' middles, its rings drawn round.
void shape_lines(const Target &t, const Shape &s, int x, int y) {
    static const int kDx[4] = { 1, 0, 1, -1 }, kDy[4] = { 0, 1, 1, 1 };
    const int left = x - s.left, top = y - s.top;
    const Rings &r = rings_of(s, -1);
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
    const Rings &r = rings_of(s, ground);
    rings_draw(t, s, r, left, top, ground);
    auto same = [&](int u, int v, int c) { return u >= 0 && u < s.w && v >= 0 && v < s.h && s.px[v * s.w + u] == c; };
    auto ink = [&](int u, int v) {
        return u >= 0 && u < s.w && v >= 0 && v < s.h && s.px[v * s.w + u] >= 0 && s.px[v * s.w + u] != ground;
    };
    for (int v = 0; v < s.h; v++) {
        for (int u = 0; u < s.w; u++) {
            const int c = s.px[v * s.w + u];
            if (c < 0 || c == ground) continue;
            const bool ring = r.on[v * s.w + u] != 0;
            const int X = fine_x(left + u), Y = fine_y(top + v);
            if (!ring) line_fine(t, X, Y, X, Y, (Bit8u)c);
            for (int k = 0; k < 4; k++) {
                const int nu = u + kDx[k], nv = v + kDy[k];
                if (!same(nu, nv, c)) continue;
                if (ring && r.on[nv * s.w + nu]) continue;
                if (kDx[k] && kDy[k] && (same(nu, v, c) || same(u, nv, c))) continue;
                line_fine(t, X, Y, fine_x(left + nu), fine_y(top + nv), (Bit8u)c);
            }
            int alike = 0;
            for (int k = 0; k < 8; k++) alike += same(u + kAx[k], v + kAy[k], c);
            if (alike >= 2 || ring) continue;
            for (int k = 0; k < 8; k++) {
                const int nu = u + kAx[k], nv = v + kAy[k];
                if (!ink(nu, nv) || same(nu, nv, c)) continue;
                if (kAx[k] && kAy[k] && (ink(nu, v) || ink(u, nv))) continue;
                line_fine(t, X, Y, (X + fine_x(left + nu)) / 2, (Y + fine_y(top + nv)) / 2, (Bit8u)c);
            }
        }
    }
}

// ---- the landing and takeoff scenes' bases (the plan's step 7) --------------------------------------------------------
//
// LANDINGS.IFF and TAKEOFFS.IFF draw a base from LTOBASES.PAK, small copies of the flight's art (APPEARNC\*.IFF, its
// SHAP: four quarters).  The scene draws it unscaled with the plain shape routine into its own buffer and copies that to
// the screen with its own code.  So when the game draws one of the small bases its place is noted, and at the compose,
// where the screen still shows that base's own pixels (90 in 100 of them at least: else nothing), the flight's art is
// shown in the same box at its own resolution (blocky: the user's ruling); whatever covers the base stays the game's.
// The pairs were matched by eye (design/privateer-hires: bases.py); the sizes are checked as the files are read.

struct BaseArt { int index, sw, sh; const char *name; int bw, bh; };
const BaseArt kBaseArt[] = {
    { 1, 132, 79, "REFINE", 219, 132 },
    { 2, 83, 67, "ROIDBASE", 209, 184 },
    { 3, 84, 70, "AGRIC", 182, 170 },
    { 4, 83, 67, "ROIDBASE", 209, 184 },
    { 6, 89, 72, "DETROIT", 184, 150 },
    { 7, 141, 81, "DERLCT", 225, 128 },
};
enum { kBaseArts = sizeof(kBaseArt) / sizeof(kBaseArt[0]), kMaxTreEntries = 20000, kMaxPackFile = 1 << 20 };

struct Substitute { Bit32u hash; Shape small, big; };
std::vector<Substitute> g_subs;
int g_subsState = 0;            // 0 not read yet, 1 read, -1 not to be had
struct BaseShown { int sub; int x0, y0; double ms; Bit32u serial; };
BaseShown g_base = { -1, 0, 0, 0, 0 };

Bit32u shape_hash(const Shape &s) {
    Bit32u h = 2166136261u;
    const int head[4] = { s.w, s.h, s.left, s.top };
    for (int i = 0; i < 4; i++) h = (h ^ (Bit32u)head[i]) * 16777619u;
    for (size_t i = 0; i < s.px.size(); i++) h = (h ^ (Bit16u)s.px[i]) * 16777619u;
    return h;
}

// A file of whichever drive has it (PRIV.TRE: on the CD image, drive D), read in parts (wcnet_tie_voice.cpp's way).
class DriveFile {
public:
    explicit DriveFile(const char *dosName) : f_(NULL) {
        std::vector<char> n(dosName, dosName + strlen(dosName) + 1);
        for (int d = 0; d < DOS_DRIVES && !f_; d++) {
            DOS_File *f = NULL;
            if (Drives[d] && Drives[d]->FileOpen(&f, &n[0], OPEN_READ) && f) {
                f->AddRef();                // (as DOS_OpenFile does: Close closes at one reference)
                f_ = f;
            }
        }
    }
    ~DriveFile() {
        if (f_) {
            f_->Close();
            if (f_->RemoveRef() <= 0) delete f_;
        }
    }
    bool ok() const { return f_ != NULL; }
    bool read(Bit32u at, Bit32u size, std::vector<Bit8u> &out) {
        Bit32u pos = at;
        if (!f_ || !f_->Seek(&pos, DOS_SEEK_SET) || pos != at) return false;
        out.resize(size);
        for (Bit32u got = 0; got < size;) {
            Bit16u n = (Bit16u)std::min<Bit32u>(size - got, 0x8000);
            if (!f_->Read(&out[got], &n) || !n) return false;
            got += n;
        }
        return true;
    }

private:
    DriveFile(const DriveFile &);
    DriveFile &operator=(const DriveFile &);
    DOS_File *f_;
};

inline Bit32u le32(const std::vector<Bit8u> &d, size_t at) {
    return at + 4 <= d.size() ? d[at] | d[at + 1] << 8 | d[at + 2] << 16 | (Bit32u)d[at + 3] << 24 : 0;
}

// Shape `index` of a shape file at `file` in d (as shape_of reads one in the game's memory).
bool shape_in(const std::vector<Bit8u> &d, Bit32u file, int index, Shape &s) {
    const BufBytes bytes(d);
    const Bit32u table = le32(d, file + 4) & 0xFFFFFF;
    if (index < 0 || (Bit32u)(4 * (index + 1)) >= table) return false;
    const Bit32u at = file + (le32(d, file + 4 + 4 * index) & 0xFFFFF);
    if (at + 8 > d.size()) return false;
    const int x2 = (Bit16s)bytes.r16(at), x1 = (Bit16s)bytes.r16(at + 2);
    const int y1 = (Bit16s)bytes.r16(at + 4), y2 = (Bit16s)bytes.r16(at + 6);
    const int w = x1 + x2 + 1, h = y1 + y2 + 1;
    if (w <= 0 || h <= 0 || w > kMaxShapeSide || h > kMaxShapeSide) return false;
    s.w = w;
    s.h = h;
    s.left = x1;
    s.top = y1;
    s.stamp = 0;
    s.px.assign(w * h, (Bit16s)-1);
    decode_rows(bytes, at + 8, at + kMaxShapeBytes, x1, y1, s);
    return true;
}

// The flight's picture of a base: its SHAP's shapes (the quarters) laid over each other at their common anchor.
bool stitched(const std::vector<Bit8u> &iff, Shape &out) {
    size_t i = 0;
    for (; i + 8 <= iff.size(); i++) {
        if (!memcmp(&iff[i], "SHAP", 4)) break;
    }
    if (i + 8 > iff.size()) return false;
    const Bit32u size = (Bit32u)iff[i + 4] << 24 | iff[i + 5] << 16 | iff[i + 6] << 8 | iff[i + 7];
    if (size < 8 || i + 8 + size > iff.size()) return false;
    const std::vector<Bit8u> shap(iff.begin() + i + 8, iff.begin() + i + 8 + size);
    std::vector<Shape> parts;
    for (int k = 0; k < 16; k++) {
        Shape q;
        if (!shape_in(shap, 0, k, q)) break;
        parts.push_back(q);
    }
    if (parts.empty()) return false;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
    for (size_t k = 0; k < parts.size(); k++) {
        x0 = std::min(x0, -parts[k].left);
        y0 = std::min(y0, -parts[k].top);
        x1 = std::max(x1, parts[k].w - 1 - parts[k].left);
        y1 = std::max(y1, parts[k].h - 1 - parts[k].top);
    }
    out.w = x1 - x0 + 1;
    out.h = y1 - y0 + 1;
    if (out.w > kMaxShapeSide || out.h > kMaxShapeSide) return false;
    out.left = -x0;
    out.top = -y0;
    out.stamp = 0;
    out.px.assign(out.w * out.h, (Bit16s)-1);
    for (size_t k = 0; k < parts.size(); k++) {
        const Shape &q = parts[k];
        for (int v = 0; v < q.h; v++) {
            for (int u = 0; u < q.w; u++) {
                const Bit16s c = q.px[v * q.w + u];
                if (c >= 0) out.px[(v - q.top + out.top) * out.w + (u - q.left + out.left)] = c;
            }
        }
    }
    return true;
}

// PRIV.TRE's LTOBASES.PAK and the bases' APPEARNC files, read once (the first time a scene draws a shape of a small
// base's size); a part that is not as expected is left out, with a line saying so.
void bases_load() {
    if (g_subsState) return;
    g_subsState = -1;
    DriveFile tre("PRIV.TRE");
    std::vector<Bit8u> head, dir;
    if (!tre.ok() || !tre.read(0, 8, head)) {
        wclog(1, "Privateer: the landing and takeoff bases: no PRIV.TRE (the game's own art)");
        return;
    }
    const Bit32u n = le32(head, 0);
    if (n == 0 || n > kMaxTreEntries || !tre.read(8, 74 * n, dir)) {
        wclog(1, "Privateer: the landing and takeoff bases: PRIV.TRE not read (the game's own art)");
        return;
    }
    auto find = [&](const std::string &tail, std::vector<Bit8u> &out) {
        for (Bit32u k = 0; k < n; k++) {
            const Bit8u *e = &dir[74 * k];
            std::string path((const char *)e + 1, strnlen((const char *)e + 1, 65));
            for (size_t c = 0; c < path.size(); c++) path[c] = (char)toupper((unsigned char)path[c]);
            if (path.size() < tail.size() || path.compare(path.size() - tail.size(), tail.size(), tail)) continue;
            const Bit32u off = e[66] | e[67] << 8 | e[68] << 16 | (Bit32u)e[69] << 24;
            const Bit32u len = e[70] | e[71] << 8 | e[72] << 16 | (Bit32u)e[73] << 24;
            return len > 0 && len <= kMaxPackFile && tre.read(off, len, out);
        }
        return false;
    };
    std::vector<Bit8u> pak;
    if (!find("\\LTOBASES.PAK", pak)) {
        wclog(1, "Privateer: the landing and takeoff bases: no LTOBASES.PAK (the game's own art)");
        return;
    }
    const Bit32u first = le32(pak, 4) & 0xFFFFFF;
    const int blocks = first >= 8 ? (int)((first - 4) / 4) : 0;
    for (int k = 0; k < kBaseArts; k++) {
        const BaseArt &a = kBaseArt[k];
        Substitute sb;
        std::vector<Bit8u> iff;
        bool ok = a.index < blocks && (le32(pak, 4 + 4 * a.index) >> 24) != 0x20;      // (0x20: packed, not ours)
        ok = ok && shape_in(pak, le32(pak, 4 + 4 * a.index) & 0xFFFFFF, 0, sb.small) && sb.small.w == a.sw &&
             sb.small.h == a.sh;
        ok = ok && find(std::string("\\APPEARNC\\") + a.name + ".IFF", iff) && stitched(iff, sb.big) &&
             sb.big.w == a.bw && sb.big.h == a.bh;
        if (!ok) {
            wclog(1, "Privateer: the landing and takeoff bases: LTOBASES %d / %s not as known (the game's own)", a.index, a.name);
            continue;
        }
        sb.hash = shape_hash(sb.small);
        g_subs.push_back(sb);
    }
    g_subsState = g_subs.empty() ? -1 : 1;
    wclog(1, "Privateer: the landing and takeoff bases: %d of %d from the flight's art", (int)g_subs.size(), kBaseArts);
}

// A shape drawn outside the flight's buffer: one of the small bases?  Its place noted (on the screen, as the scene's
// buffer is copied there whole: the compose checks that).
void note_base(const Shape &s, int x, int y) {
    bool sized = false;
    for (int k = 0; k < kBaseArts && !sized; k++) sized = s.w == kBaseArt[k].sw && s.h == kBaseArt[k].sh;
    if (!sized) return;
    bases_load();
    const Bit32u h = shape_hash(s);
    for (size_t k = 0; k < g_subs.size(); k++) {
        if (g_subs[k].hash != h) continue;
        g_base.sub = (int)k;
        g_base.x0 = x - s.left;
        g_base.y0 = y - s.top;
        g_base.ms = PIC_FullIndex();
        g_base.serial++;
        return;
    }
}

// The flight's art over the small base, where the screen still shows the small one's own pixels.
void overlay_base(const Bit8u *low) {
    if (g_base.sub < 0 || g_base.sub >= (int)g_subs.size() || PIC_FullIndex() - g_base.ms > kBaseStaleMs) return;
    const Substitute &sb = g_subs[g_base.sub];
    const Shape &sm = sb.small, &bg = sb.big;
    int lit = 0, same = 0;
    for (int v = 0; v < sm.h; v++) {
        for (int u = 0; u < sm.w; u++) {
            const int c = sm.px[v * sm.w + u], x = g_base.x0 + u, y = g_base.y0 + v;
            if (c < 0 || x < 0 || x >= LW || y < 0 || y >= LH) continue;
            lit++;
            same += low[y * LW + x] == c;
        }
    }
    if (!lit || same * 10 < lit * 9) return;
    const double fx = (double)bg.w / sm.w, fy = (double)bg.h / sm.h;
    for (int v = 0; v < sm.h; v++) {
        const int y = g_base.y0 + v;
        if (y < 0 || y >= LH) continue;
        for (int u = 0; u < sm.w; u++) {
            const int c = sm.px[v * sm.w + u], x = g_base.x0 + u;
            if (c < 0 || x < 0 || x >= LW || low[y * LW + x] != c) continue;
            for (int Y = g_rowFrom[y]; Y < g_rowFrom[y + 1]; Y++) {
                const int V = (int)(((Y + 0.5) / kSy - g_base.y0) * fy);
                if (V < 0 || V >= bg.h) continue;
                for (int X = 2 * x; X < 2 * x + 2; X++) {
                    const int U = (int)(((X + 0.5) / 2.0 - g_base.x0) * fx);
                    if (U < 0 || U >= bg.w) continue;
                    const Bit16s b = bg.px[V * bg.w + U];
                    if (b >= 0) g_out[Y * HW + X] = (Bit8u)b;
                }
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
    int X0 = (int)(lox * 2), X1 = (int)ceil(hix * 2), Y0 = (int)(loy * kSy), Y1 = (int)ceil(hiy * kSy);
    if (X0 < 2 * t.x0) X0 = 2 * t.x0;
    if (X1 > 2 * (t.x1 + 1)) X1 = 2 * (t.x1 + 1);
    if (Y0 < g_rowFrom[t.y0]) Y0 = g_rowFrom[t.y0];
    if (Y1 > g_rowFrom[t.y1 + 1]) Y1 = g_rowFrom[t.y1 + 1];
    // the source's place, 16.16, stepped along a row
    const Bit32s du = (Bit32s)floor(c * 0.5 * mx / sx * 65536 + 0.5), dv = (Bit32s)floor(-n * 0.5 * my / sy * 65536 + 0.5);
    const Bit16s *src = &s.px[0];
    for (int Y = Y0; Y < Y1; Y++) {
        const double gx = (X0 + 0.5) / 2 - x - 0.5, gy = (Y + 0.5) / kSy - y - 0.5;
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
    const double top = (lo + 0.5) * kSy, bottom = (hi + 0.5) * kSy;
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
            const double ax = (px[i] + 0.5) * 2, ay = (py[i] + 0.5) * kSy;
            const double bx = (px[j] + 0.5) * 2, by = (py[j] + 0.5) * kSy;
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
    if (g_wirePending && !(p.op == OP_SHAPE && target_art_call())) wire_flush();     // (the target display's ship, whole)
    if (p.op == OP_CORNER_X || p.op == OP_CORNER_Y) {
        corner(p.op == OP_CORNER_Y);
        return;
    }
    if (p.op == OP_PRESENT) {
        present();
        return;
    }
    if (p.op == OP_SHAPE && hires_wanted() && g_subsState >= 0) {     // (a landing's or takeoff's base?)
        Target scene;
        if (read_target((Bit16u)arg16(0), scene) && !flights_buffer(scene)) {
            const Shape *s = shape_of((Bit16u)arg16(3), (Bit16u)arg16(4), arg16(5));
            if (s) note_base(*s, arg16(1), arg16(2));
            return;
        }
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
        if (fine_lines(caller)) line(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
        else line_blocks(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
        break;
    case OP_RECT: {
        const int x0 = arg16(1), y0 = arg16(2), x1 = arg16(3), y1 = arg16(4);
        const Bit8u c = (Bit8u)arg16(5);
        void (*draw)(const Target &, int, int, int, int, Bit8u) = fine_lines(caller) ? line : line_blocks;
        draw(t, x0, y0, x1, y0, c);
        draw(t, x1, y0, x1, y1, c);
        draw(t, x1, y1, x0, y1, c);
        draw(t, x0, y1, x0, y0, c);
        break;
    }
    case OP_ELLIPSE:
        if (hires_cockpit()) ellipse(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
        else ellipse_blocks(t, arg16(1), arg16(2), arg16(3), arg16(4), (Bit8u)arg16(5));
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
            if (!(caller == kFontCaller && hires_cockpit() && letter(t, *s, x, y, arg16(5), (Bit8u)arg16(6)))) {
                shape_blocks(t, *s, x, y, (Bit8u)arg16(6));
            }
        } else if (p.op == OP_SHAPE) {
            if (hires_cockpit() && target_art_call() && s->w <= kWireMax && s->h <= kWireMax) {
                wire_gather(t, *s, x, y);
            } else if (caller == kCrosshairCaller && hires_cockpit()) shape_lines(t, *s, x, y);
            else if (hires_cockpit() && art_word(t, caller, *s, x, y)) {
            }
            else if (caller == kRadarCaller && hires_cockpit()) shape_radar(t, *s, x, y);
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
        int vy = (20 * Y + 10) * LH / HH - 10;
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
    // A square corner stays square: where the edge about a pixel is straight both ways -- the pixels on the other side
    // of it among its eight neighbours exactly a whole column, a whole row, or a column and a row of them (a box's side
    // or corner: the readouts' and screens' windows) -- its fine pixels are its own side's.  The smoothing rounded those
    // corners off, and with them the corner of a letter there (the user, 2026-11-23: "why does the K in KPS not go to
    // the bottom"): it is for the canopy's curves and slants, whose steps match none of these.
    static const int kNu[8] = { -1, 0, 1, -1, 1, -1, 0, 1 }, kNv[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    enum { TL = 1, T = 2, TR = 4, L = 8, R = 16, BL = 32, B = 64, BR = 128 };
    static const int kBoxSides[8] = { TL | L | BL, TR | R | BR, TL | T | TR, BL | B | BR, TL | T | TR | L | BL,
                                      TL | L | BL | B | BR, TL | T | TR | R | BR, TR | R | BR | BL | B };
    for (int y = 0; y < LH; y++) {
        for (int x = 0; x < LW; x++) {
            const Bit8u side = g_window[y * LW + x];
            int other = 0;
            for (int k = 0; k < 8; k++) {
                const int nx = x + kNu[k], ny = y + kNv[k];
                const Bit8u n = nx >= 0 && ny >= 0 && nx < LW && ny < LH ? g_window[ny * LW + nx] : side;
                if (n != side) other |= 1 << k;
            }
            bool box = false;
            for (int i = 0; i < 8 && !box; i++) box = other == kBoxSides[i];
            if (!box) continue;
            for (int Y = g_rowFrom[y]; Y < g_rowFrom[y + 1]; Y++) {
                for (int X = 2 * x; X < 2 * x + 2; X++) {
                    g_fineWindow[Y * HW + X] = side;
                    g_from[Y * HW + X] = (Bit16u)(y * LW + x);
                }
            }
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
    const int mode = flight ? (hires_cockpit() ? 1 : 3) : hires_wanted() ? 2 : 0;  // (2: the game's picture, a base over it; 3: space alone)
    const Bit32u serial = flight ? g_frameSerial : mode == 2 ? g_base.serial : 0;
    if (madeFlight == mode && madeSerial == serial && !memcmp(madeFrom, low, LW * LH)) return;
    madeFlight = mode;
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
        if (mode == 2) overlay_base(low);
        return;
    }
    if (mode == 3) {            // (the space alone: the cockpit's window edge the game's own, block for block)
        for (int Y = 0; Y < HH; Y++) {
            const int row = g_lowRow[Y] * LW;
            for (int X = 0; X < HW; X++) {
                const int from = row + (X >> 1), i = Y * HW + X;
                g_out[i] = g_window[from] && low[from] == g_shownLow[from] ? g_shownHi[i] : low[from];
            }
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

static const char *level_name(int level) {
    return level == 2 ? "on" : level == 1 ? "on in space alone" : "off";
}

void priv_hires_set(int level) {
    const int was = hires_level();
    if (g_never) return;
    g_want = g_level = level < 0 ? 0 : level > 2 ? 2 : level;
    if (g_level != was) wclog(1, "Privateer: the high-resolution picture %s (the page's)", level_name(g_level));
    now_wanted();
}

int priv_hires_level() {
    return hires_level();
}

void priv_hires_flip() {
    hires_wanted();
    if (g_never) {
        wclog(1, "Privateer: the high-resolution picture is off for this run (the game as it was)");
        return;
    }
    g_level = (hires_level() + 2) % 3;          // (all, then the space alone, then off, then all)
    now_wanted();
    wclog(1, "Privateer: the high-resolution picture %s", level_name(g_level));
}

// The picture is the override's 640 x 400 from the first time hi-res is wanted, so that the key flips without a change of mode.
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
        wclog(1, "Privateer: the picture is 640 x 400 (each game pixel 2 x 2)");
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
