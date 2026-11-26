/*
 *  The TIE Fighter demo's flight at 640x400 (wcnet_tie640.h, docs/tie640.md).
 *
 *  What the picture is made of:
 *    L   the game's own 320x200, the VGA's memory: the cockpit, the instruments, the brackets, the text, drawn by the
 *        game as before.  Its 3D window (320x189, the game's mask says where the cockpit leaves it open) holds the
 *        background colour wherever nothing is drawn over the scene: the scene is no longer drawn there.
 *    W   the window: the mask's open pixels.
 *    H   the scene at 640x378, drawn by the game's own renderer: tie_updatescreen's scene section (0090:0B1A to
 *        0090:1069) is run four times a frame, each time a quarter of the picture at twice the scale (the
 *        projection's centre moved, its scale doubled at the places it is computed), through an open mask, its
 *        writes to the VGA landing in H instead (tie640_hook).
 *  A frame: every pixel of L becomes four.  The window's edge is cut at the finer grid (smooth_edge); inside the
 *  window a pixel still at the background colour shows H's four, anything drawn over the scene is doubled; outside it
 *  the cockpit is doubled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "wcnet_t640_game.h"
#include "wcnet_tie640.h"
#include "wcnet_log.h"
#include "wcnet_perf.h"
#include "regs.h"
#include "cpu.h"
#include "mem.h"
#include "pic.h"
#include "vga.h"
#include "render.h"

#ifdef T640_XWING
extern bool wc_hires_watch;     // (the one flag: the VGA's memory asks whichever game's picture is on)
#else
bool wc_hires_watch = false;
namespace wc { bool xw640_on(); }
bool xw640_hires_write(PhysPt offset, Bitu val, int bytes);
bool xw640_hires_read(PhysPt offset, int bytes, Bitu *val);
#endif

namespace wc {

namespace {

enum { LW = 320, LH = 200, HW = 640, HH = 400 };

bool g_on = false;
int g_wanted = -1;              // WCNET_TIE640: 0 keeps the game's own picture
Bit16u g_load = 0;
Bit8u g_window[LW * LH];        // W
Bit8u g_hi[HW * HH];            // H, as shown
Bit8u g_draw[HW * HH];          // H, the frame being drawn
bool g_haveHi = false;          // H holds a frame
int g_pass = -1;                // the quarter being drawn (-1: none)
int g_passX = -1, g_passY = 0;  // its corner in H
Bit8u g_key = 0xF7;             // the background colour (DS:48BC)
Bit8u g_out[HW * HH];
std::map<Bit32u, unsigned> g_writers;   // research: (image paragraph << 16 | IP & FFF0) -> bytes written
bool g_countWriters = false;
extern bool g_edgeDirty;
unsigned g_hookCalls = 0;       // (research: the hook's calls, !t640low logs them)
unsigned g_frames = 0;

inline const Bit8u *low() { return vga.fastmem; }

void reset_state() {
    memset(g_window, 0, sizeof(g_window));
    memset(g_hi, 0, sizeof(g_hi));
    g_haveHi = false;
    g_pass = g_passX = -1;
    g_edgeDirty = true;
}

// The window's edge at the finer grid.  W's edge is a staircase: runs of pixels along a row (or a column), each a step
// of one from the next.  The line it stands for passes through the middle of each run, so near a step the edge is
// half a pixel (one of the finer grid's) nearer the next run's level: over the run's last quarter towards a step.  For
// each of the 640x400's pixels, g_from says which of L's pixels it shows: its own (0), or the one above (1), below
// (2), left (3) or right (4), whose side of the edge it has been given.
Bit8u g_from[HW * HH];
bool g_edgeDirty = true;

inline int win(int x, int y) { return x >= 0 && y >= 0 && x < LW && y < LH ? g_window[y * LW + x] : -1; }

void find_islands();
void smooth_edge() {
    memset(g_from, 0, sizeof(g_from));
    // edges along a row: between rows y and y + 1, a run of columns [x0, x1] where W differs
    for (int y = 0; y < LH - 1; y++) {
        for (int x0 = 0; x0 < LW;) {
            const int p = win(x0, y);
            if (p == win(x0, y + 1)) { x0++; continue; }
            int x1 = x0;
            while (x1 + 1 < LW && win(x1 + 1, y) == p && win(x1 + 1, y + 1) != p) x1++;
            const int n = x1 - x0 + 1;
            for (int end = 0; end < 2; end++) {
                const int xn = end ? x1 + 1 : x0 - 1;     // the column past this end
                if (xn < 0 || xn >= LW) continue;
                int dir = 0;
                if (win(xn, y - 1) == p && win(xn, y) == !p) dir = -1;                  // the edge one row up there
                else if (win(xn, y) == p && win(xn, y + 1) == p && win(xn, y + 2) == !p) dir = 1;     // one row down
                if (!dir) continue;
                const int k = dir < 0 ? (n + 1) / 2 : n / 2;        // fine columns given over (the tie goes up)
                const int row = dir < 0 ? 2 * y + 1 : 2 * y + 2;    // the lower half of row y, or the upper of y + 1
                for (int i = 0; i < k; i++) {
                    const int col = end ? 2 * x1 + 1 - i : 2 * x0 + i;
                    g_from[row * HW + col] = dir < 0 ? 2 : 1;
                }
            }
            x0 = x1 + 1;
        }
    }
    // edges along a column, runs of two rows and more (a diagonal's single steps are the rows' pass's)
    for (int x = 0; x < LW - 1; x++) {
        for (int y0 = 0; y0 < LH;) {
            const int p = win(x, y0);
            if (p == win(x + 1, y0)) { y0++; continue; }
            int y1 = y0;
            while (y1 + 1 < LH && win(x, y1 + 1) == p && win(x + 1, y1 + 1) != p) y1++;
            const int n = y1 - y0 + 1;
            if (n >= 2) {
                for (int end = 0; end < 2; end++) {
                    const int yn = end ? y1 + 1 : y0 - 1;
                    if (yn < 0 || yn >= LH) continue;
                    int dir = 0;
                    if (win(x - 1, yn) == p && win(x, yn) == !p) dir = -1;
                    else if (win(x, yn) == p && win(x + 1, yn) == p && win(x + 2, yn) == !p) dir = 1;
                    if (!dir) continue;
                    const int k = dir < 0 ? (n + 1) / 2 : n / 2;
                    const int col = dir < 0 ? 2 * x + 1 : 2 * x + 2;
                    for (int i = 0; i < k; i++) {
                        const int row = end ? 2 * y1 + 1 - i : 2 * y0 + i;
                        if (!g_from[row * HW + col]) g_from[row * HW + col] = dir < 0 ? 4 : 3;
                    }
                }
            }
            y0 = y1 + 1;
        }
    }
    find_islands();
    g_edgeDirty = false;
}

// What the game draws over the scene inside the window (the gunsight, a target's brackets) is 320 art: doubled, its
// one-pixel strokes are two wide over a scene drawn at the finer grid (the user: "fix ... the reticle specifically to
// be thinner").  So it is drawn as its own picture at the finer grid instead: each of its pixels one fine pixel at its
// cell's corner, joined to its neighbours (right, below, and across a diagonal when nothing else joins them) by the
// fine pixel between.  A stroke one pixel wide is one fine pixel wide, the same length; a filled patch stays filled.
// The scene shows through the rest.  WCNET_T640_THINSIGHT=0: doubled as before.
bool thin_sight() {
    static int on = -1;
    if (on < 0) { const char *e = getenv("WCNET_T640_THINSIGHT"); on = e && e[0] == '0' ? 0 : 1; }
    return on == 1;
}
Bit8u g_island[LW * LH];     // 1: a closed pixel of the mask that is no part of the cockpit (the gunsight)
bool g_anyIsland = false;
// The mask's closed pixels in patches that do not reach the window's sides, none wider than a quarter of it: the
// gunsight (the game closes it in the mask and draws it itself).  Found when the mask changes.
void find_islands() {
    memset(g_island, 0, sizeof(g_island));
    g_anyIsland = false;
    int x0 = LW, y0 = LH, x1 = -1, y1 = -1;
    for (int y = 0; y < LH; y++) for (int x = 0; x < LW; x++) if (g_window[y * LW + x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) return;
    static Bit8u seen[LW * LH];
    static int stack[LW * LH];
    memset(seen, 0, sizeof(seen));
    for (int y = y0 + 1; y < y1; y++) {
        for (int x = x0 + 1; x < x1; x++) {
            if (g_window[y * LW + x] || seen[y * LW + x]) continue;
            int n = 0, top = 0, bx0 = x, bx1 = x, by0 = y, by1 = y;
            bool cockpit = false;
            stack[top++] = y * LW + x; seen[y * LW + x] = 1;
            const int first = n;
            static int members[LW * LH];
            while (top) {
                const int e = stack[--top], ex = e % LW, ey = e / LW;
                members[n++] = e;
                if (ex <= x0 || ex >= x1 || ey <= y0 || ey >= y1) { cockpit = true; continue; }
                if (ex < bx0) bx0 = ex; if (ex > bx1) bx1 = ex; if (ey < by0) by0 = ey; if (ey > by1) by1 = ey;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    const int q = (ey + dy) * LW + ex + dx;
                    if (!g_window[q] && !seen[q]) { seen[q] = 1; stack[top++] = q; }
                }
            }
            if (cockpit || bx1 - bx0 > LW / 4 || by1 - by0 > LH / 4) continue;
            for (int k = first; k < n; k++) g_island[members[k]] = 1;
            g_anyIsland = true;
        }
    }
}
void thin_overlay(const Bit8u *L) {
    if (!g_anyIsland) return;
    auto isle = [&](int x, int y) { return x >= 0 && y >= 0 && x < LW && y < LH && g_island[y * LW + x]; };
    for (int y = 0; y < LH; y++) {
        for (int x = 0; x < LW; x++) {
            const int e = y * LW + x;
            Bit8u *o = g_out + (2 * y) * HW + 2 * x;
            const Bit8u *h = g_hi + (2 * y) * HW + 2 * x;
            const bool right = isle(x + 1, y), below = isle(x, y + 1), across = isle(x + 1, y + 1);
            if (g_island[e]) {
                o[0] = L[e];
                o[1] = right ? L[e] : h[1];
                o[HW] = below ? L[e] : h[HW];
                o[HW + 1] = across && right == below ? L[e] : h[HW + 1];
            } else if (g_window[e] && (right || below || across || isle(x - 1, y) || isle(x, y - 1) || isle(x - 1, y - 1) || isle(x + 1, y - 1) || isle(x - 1, y + 1))) {
                // beside the sight: the scene, or what the game drew over it, whole (no edge rule here); and the
                // other diagonal's joint
                const bool scene = L[e] == g_key;
                o[0] = scene ? h[0] : L[e]; o[1] = scene ? h[1] : L[e]; o[HW] = scene ? h[HW] : L[e]; o[HW + 1] = scene ? h[HW + 1] : L[e];
                if (right && below && !across) o[HW + 1] = L[e + 1];
            }
        }
    }
}

// The frame, composed into g_out.
void compose() {
    static const int kDx[5] = { 0, 0, 0, -1, 1 }, kDy[5] = { 0, -1, 1, 0, 0 };
    if (g_edgeDirty) smooth_edge();
    const Bit8u *L = low();
    for (int Y = 0; Y < HH; Y++) {
        const int y = Y >> 1;
        const Bit8u *from = g_from + Y * HW, *hi = g_hi + Y * HW;
        Bit8u *out = g_out + Y * HW;
        for (int X = 0; X < HW; X++) {
            const int x = X >> 1, e = y * LW + x;
            const int f = from[X];
            const int s = f ? (y + kDy[f]) * LW + x + kDx[f] : e;
            // the scene shows where the window's pixel is still the background's (a cell cut out of the cockpit's
            // pixel shows the scene too)
            if (g_haveHi && g_window[s] && (f || L[e] == g_key)) out[X] = hi[X];
            else out[X] = L[s];
        }
    }
    if (g_haveHi && thin_sight()) thin_overlay(L);
}

// --- the scene in quarters -----------------------------------------------------------------------------------------
// (Found by running: docs/tie640.md.  Places are image paragraphs; DS is the program's data segment.)
enum {
    kFlightPara = XA(0x0090, 0x0920), kRendPara = XA(0x1CCA, 0x222D), kStarPara = XA(0x1B66, 0x20CE), kBackdropPara = XA(0x168C, 0x1D5D), kDataPara = XA(0x421F, 0x3F1C),
    kSceneTop = XA(0x0B1A, 0x0AD8),         // tie_updatescreen: the scene section's start (the camera is set)
    kSceneCentre = XA(0x0B36, 0x0AF4),      // ... xtrans2_initxtrans has come back (it sets the centre again every frame)
    kSceneEnd = XA(0x1069, 0x104B),         // ... the section's end: the panel's work follows
    kDrawStars = XA(0x0F0E, 0x0E88),        // 1B66: rtsvga2_drawstars
    kFullUpdate = XA(0xBF98, 0xC07A),       // DS: the whole window drawn again this frame (the run tables forgotten)
    kBackColour = XA(0x48BC, 0x541A),       // DS: the background's colour (a byte)
    kCorner = XA(0x48C3, 0x5421),           // DS: the window's corner in the screen
    kWide = XA(0x48C5, 0x5423), kDeep = XA(0x48D1, 0x5433),         // DS: the window, 320 x 189
    kHalfWide = XA(0x48C9, 0x542B), kHalfDeep = XA(0x48D5, 0x543B), // DS: the projection's centre (160, 94)
    kYOffset = XA(0x289E, 0x33DE),          // DS: ... and the 32 bits added to the centre's row (-19: the sight's height)
    kTablesSeg = XA(0xD8A6, 0xD34A), kMaskAt = XA(0x28B6, 0x33FB),  // DS: the run tables' segment, the mask's offset in it
    kStatics = XA(0x8FC0, 0xC2BE),          // DS: the static objects (0x12 a record, 64): three angle bytes advanced in the section
    kMaskBytes = 0x900,
};
Bit8u g_maskKept[kMaskBytes], g_statics[64 * 3];
Bit16u g_keepHalfWide = 0;
Bit32u g_keepYOffset = 0;
bool g_centreMoved = false, g_filled = false, g_jumped = false;

inline PhysPt dsa(Bit16u off) { return ((PhysPt)(Bit16u)(g_load + kDataPara) << 4) + off; }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline bool cs_is(Bit16u para) { return SegValue(cs) == (Bit16u)(g_load + para); }

// The game's mask (the cockpit's window; in the full view the scopes and the sight closed): for each row a first byte
// (its top bit set: the row begins closed, else open) and then run lengths, closed and open in turn, a 0 then a byte for 256 and
// more.  Read into W, kept, and opened for the quarters (every row one open run).
void mask_open() {
    const PhysPt a = ((PhysPt)r16(kTablesSeg) << 4) + r16(kMaskAt);
    const int w = r16(kWide), h = r16(kDeep), corner = r16(kCorner);
    for (int i = 0; i < kMaskBytes; i++) g_maskKept[i] = mem_readb(a + (PhysPt)i);
    static Bit8u win[LW * LH];
    memset(win, 0, sizeof(win));
    int at = 0;
    bool good = w <= LW && h <= LH && corner % LW + w <= LW && corner / LW + h <= LH;
    for (int y = 0; y < h && good; y++) {
        bool open = !(g_maskKept[at++] & 0x80);
        for (int x = 0; x < w;) {
            if (at + 2 >= kMaskBytes) { good = false; break; }
            int n = g_maskKept[at++];
            if (!n) n = 256 + g_maskKept[at++];
            for (int i = 0; i < n && x < w; i++, x++) {
                const int o = corner + y * LW + x;
                if (open && o < LW * LH) win[o] = 1;
            }
            open = !open;
        }
    }
    if (!good) memset(win, 0, sizeof(win));
    if (memcmp(win, g_window, sizeof(win))) {
        memcpy(g_window, win, sizeof(win));
        g_edgeDirty = true;
        g_filled = false;
    }
    PhysPt to = a;
    for (int y = 0; y < h; y++) {
        mem_writeb(to++, 1);
        if (w >= 256) mem_writeb(to++, 0);
        mem_writeb(to++, (Bit8u)w);
    }
}

void mask_back() {
    const PhysPt a = ((PhysPt)r16(kTablesSeg) << 4) + r16(kMaskAt);
    for (int i = 0; i < kMaskBytes; i++) mem_writeb(a + (PhysPt)i, g_maskKept[i]);
}

// The window of the game's own picture set to the background colour: the scene is not drawn there any more, and what
// the game draws over the scene from now on stands out against it.
void fill_window() {
    for (int o = 0; o < LW * LH; o++) {
        if (!g_window[o]) continue;
        vga.fastmem[o] = g_key;
        if (o < LW) vga.fastmem[o + 64 * 1024] = g_key;
        vga.mem.linear[((o & ~3) << 2) + (o & 3)] = g_key;
    }
    g_filled = true;
}

// The projection's scale is a shift by 8 in the renderer's code.  For the quarters the code itself is changed (and
// put back at the scene's end: the panel's own small 3D picture and its brackets use the same routines at the game's
// scale): a hook at each would be reached thousands of times a frame, and in the browser every hook's call leaves
// the CPU's loop.  transfm2_getscreenx, getscreeny: shld edx,eax,8 / shl eax,8 -> 9.  The near plane's clip, four
// places: DX:AX shifted by 8 with byte moves (mov dh,dl / mov dl,ah / mov ah,al / xor al,al) -> shld dx,ax,9 /
// shl ax,9 / nop.
struct Patch { Bit16u off; int n; Bit8u was[8], now[8]; };
const Patch kScalePatches[] = {
    { XA(0x7820, 0x7DBE), 1, { 0x08 }, { 0x09 } }, { XA(0x7824, 0x7DC2), 1, { 0x08 }, { 0x09 } },
    { XA(0x78BE, 0x7E5C), 1, { 0x08 }, { 0x09 } }, { XA(0x78C2, 0x7E60), 1, { 0x08 }, { 0x09 } },
    { XA(0x7A27, 0x7FCF), 8, { 0x8A, 0xF2, 0x8A, 0xD4, 0x8A, 0xE0, 0x32, 0xC0 }, { 0x0F, 0xA4, 0xC2, 0x09, 0xC1, 0xE0, 0x09, 0x90 } },
    { XA(0x7AF7, 0x809F), 8, { 0x8A, 0xF2, 0x8A, 0xD4, 0x8A, 0xE0, 0x32, 0xC0 }, { 0x0F, 0xA4, 0xC2, 0x09, 0xC1, 0xE0, 0x09, 0x90 } },
#ifndef T640_XWING     // (the demo's renderer has four such places, X-Wing's two)
    { 0x7C11, 8, { 0x8A, 0xF2, 0x8A, 0xD4, 0x8A, 0xE0, 0x32, 0xC0 }, { 0x0F, 0xA4, 0xC2, 0x09, 0xC1, 0xE0, 0x09, 0x90 } },
    { 0x7CA1, 8, { 0x8A, 0xF2, 0x8A, 0xD4, 0x8A, 0xE0, 0x32, 0xC0 }, { 0x0F, 0xA4, 0xC2, 0x09, 0xC1, 0xE0, 0x09, 0x90 } },
#endif
};
// NATIVE (WCNET_T640_NATIVE): the three "smaller than a pixel: not drawn" tests (0090:0C4A craft, 121C bolts and
// debris, 1392 static objects) take the depth / 256 into DX:AX with byte moves (mov al,ah / mov ah,dl / mov dl,dh /
// shl dh,1 / sbb dh,dh); for a pixel half the size the depth / 512: shrd ax,dx,9 / sar dx,9.  The same value the
// hooks at 0C5B, 1226, 139C make of it, without a hook's call for every object of every quarter.
const Bit16u kPixelPatches[] = { XA(0x0C51, 0x0C0E), XA(0x121C, 0x11FA), XA(0x1392, 0x1370) };
const Bit8u kPixelWas[10] = { 0x8A, 0xC4, 0x8A, 0xE2, 0x8A, 0xD6, 0xD0, 0xE6, 0x1A, 0xF6 };
const Bit8u kPixelNow[10] = { 0x0F, 0xAC, 0xD0, 0x09, 0xC1, 0xFA, 0x09, 0x90, 0x90, 0x90 };
// ... and draw_getcompdetailptr (1CCA:09FC) picks a part's detail level by its depth: the depth halved as it is
// loaded (sar dx,1 / rcr ax,1; room made by the loop's three long jumps written short).
const Bit8u kLodWas[30] = { 0x8B, 0x46, 0x0A, 0x8B, 0x56, 0x0C, 0x26, 0x3B, 0x57, 0x02, 0x0F, 0x8C, 0x10, 0x00, 0x0F, 0x8F, 0x07, 0x00,
                            0x26, 0x3B, 0x07, 0x0F, 0x86, 0x05, 0x00, 0x83, 0xC3, 0x06, 0xEB, 0xE8 };
const Bit8u kLodNow[30] = { 0x8B, 0x46, 0x0A, 0x8B, 0x56, 0x0C, 0xD1, 0xFA, 0xD1, 0xD8, 0x26, 0x3B, 0x57, 0x02, 0x7C, 0x0E, 0x7F, 0x05,
                            0x26, 0x3B, 0x07, 0x76, 0x07, 0x83, 0xC3, 0x06, 0xEB, 0xEE, 0x90, 0x90 };
enum { kLodAt = XA(0x0A03, 0x0AF3) };
int g_pixelPatched = -1;        // -1 not looked at; 0 the hooks; 1 the code patched while the quarters are drawn
bool pixel_patches() {
    if (g_pixelPatched < 0) {
        g_pixelPatched = tie640_native_on() ? 1 : 0;
        const PhysPt base = (PhysPt)(Bit16u)(g_load + kFlightPara) << 4;
        for (int i = 0; i < 3 && g_pixelPatched; i++)
            for (int k = 0; k < 10; k++) if (mem_readb(base + kPixelPatches[i] + (PhysPt)k) != kPixelWas[k]) g_pixelPatched = 0;
        const PhysPt rend = (PhysPt)(Bit16u)(g_load + kRendPara) << 4;
        for (int k = 0; k < 30 && g_pixelPatched; k++) if (mem_readb(rend + kLodAt + (PhysPt)k) != kLodWas[k]) g_pixelPatched = 0;
    }
    return g_pixelPatched == 1;
}
bool g_patchedNow = false;      // the bytes in memory are the patched ones
bool g_checking = false;        // WCNET_T640_CHECK with the native pass: the hooks are kept for the game's own drawing
void native_patches(bool on) {
    if (!pixel_patches() || on == g_patchedNow) return;
    const PhysPt flight = (PhysPt)(Bit16u)(g_load + kFlightPara) << 4, rend = (PhysPt)(Bit16u)(g_load + kRendPara) << 4;
    for (int i = 0; i < 3; i++) for (int k = 0; k < 10; k++) mem_writeb(flight + kPixelPatches[i] + (PhysPt)k, on ? kPixelNow[k] : kPixelWas[k]);
    for (int k = 0; k < 30; k++) mem_writeb(rend + kLodAt + (PhysPt)k, on ? kLodNow[k] : kLodWas[k]);
    g_patchedNow = on;
}
bool g_scaled = false;
int g_codeKnown = -1;       // the renderer's bytes at those places are the ones known (checked once)

void scale_code(bool doubled) {
    if (doubled == g_scaled) return;
    const PhysPt base = (PhysPt)(Bit16u)(g_load + kRendPara) << 4;
    for (size_t i = 0; i < sizeof(kScalePatches) / sizeof(kScalePatches[0]); i++) {
        const Patch &q = kScalePatches[i];
        for (int k = 0; k < q.n; k++) mem_writeb(base + q.off + (PhysPt)k, doubled ? q.now[k] : q.was[k]);
    }
    if (!doubled) native_patches(false);
    g_scaled = doubled;
}

bool code_known() {
    if (g_codeKnown < 0) {
        g_codeKnown = 1;
        const PhysPt base = (PhysPt)(Bit16u)(g_load + kRendPara) << 4;
        for (size_t i = 0; i < sizeof(kScalePatches) / sizeof(kScalePatches[0]); i++) {
            const Patch &q = kScalePatches[i];
            for (int k = 0; k < q.n; k++) if (mem_readb(base + q.off + (PhysPt)k) != q.was[k]) g_codeKnown = 0;
        }
        if (!g_codeKnown) wclog(0, T640_GAME ": the renderer's projection is not the code known: the picture stays the game's own");
    }
    return g_codeKnown == 1;
}

void pass_begin(bool again = false) {      // FAST: again = the same quarter once more (the check)
    native_patches(!tie640_fast_reference());   // NATIVE (the check's own drawing: the game's code as it was, the hooks)
    if (g_pass == 0 && !again) {
        scale_code(true);
        g_key = mem_readb(dsa(kBackColour));
        const bool full = r16(kFullUpdate) != 0;
        mask_open();
        if (full || !g_filled) fill_window();
        for (int i = 0; i < 64; i++) for (int k = 0; k < 3; k++) g_statics[i * 3 + k] = mem_readb(dsa((Bit16u)(kStatics + i * XA(0x12, 0x0E) + k)));
    } else {
        for (int i = 0; i < 64; i++) for (int k = 0; k < 3; k++) mem_writeb(dsa((Bit16u)(kStatics + i * XA(0x12, 0x0E) + k)), g_statics[i * 3 + k]);
    }
    mem_writew(dsa(kFullUpdate), 1);        // (every quarter drawn whole: the run tables are another quarter's)
    // the window (w x h at its corner in the game's picture; the side views' is smaller) is twice that in H, and
    // this quarter's corner there is the window's doubled plus the quarter's: a write at (x, y) of the game's
    // picture lands at (x + g_passX, y + g_passY)
    const int corner = r16(kCorner);
    g_passX = corner % LW + (g_pass & 1) * (int)r16(kWide);
    g_passY = corner / LW + (g_pass >> 1) * (int)r16(kDeep);
    {   // FAST: the quarter's own mask (the open one again first: a quarter's mask may not fit)
        const PhysPt a = ((PhysPt)r16(kTablesSeg) << 4) + r16(kMaskAt);
        const int w = r16(kWide), h = r16(kDeep);
        PhysPt to = a;
        for (int y = 0; y < h; y++) {
            mem_writeb(to++, 1);
            if (w >= 256) mem_writeb(to++, 0);
            mem_writeb(to++, (Bit8u)w);
        }
        if (g_edgeDirty) smooth_edge();
        tie640_fast_mask(g_pass, a, g_maskKept, kMaskBytes, g_window, g_from, w, h, corner);
    }
}

}  // namespace

void tie640_native_ref(Bit16u loadSeg, int what);
void tie640_native_quarter(int quarter);
// ONE PASS (wcnet_tie640_onepass.cpp, WCNET_T640_NATIVE=2)
bool tie640_onepass_on(); bool tie640_onepass_sprite_running(); bool tie640_onepass_check(); bool tie640_onepass_no_sprites(); void tie640_onepass_places(void (*place)(Bit16u, Bit16u));
void tie640_onepass_begin(Bit16u loadSeg); bool tie640_onepass_hook(Bit16u loadSeg, Bit32u ip, Bit8u *draw, int pitch, int limitX, int limitY, int corner);
void tie640_onepass_say(); void tie640_onepass_check_begin(); void tie640_onepass_check_middle(const Bit8u *draw);
void tie640_onepass_check_end(Bit16u loadSeg, const Bit8u *draw, int wide, int deep, int corner);
bool tie640_native_fill(Bit16u loadSeg, Bit32u ip);
bool tie640_trace_check_on(); void tie640_trace_check_entry(Bit16u loadSeg); void tie640_trace_check_return(Bit16u loadSeg); void tie640_trace_check_say(); void tie640_trace_shapes_places(void (*place)(Bit16u, Bit16u)); bool tie640_trace_shapes_hook(Bit16u loadSeg, Bit32u ip);    // TRACE    // NATIVE (wcnet_tie640_native.cpp, the check)
namespace { void place_at(Bit16u para, Bit16u ip) { watch_at((Bit16u)(g_load + para), ip); } }
namespace {
#include "wcnet_tie640_dump.inc"      // RESEARCH AID (WCNET_T640_DUMP=1): the one-pass primitives of a frame, written out
}

// (each place in its own code segment, wc::watch_at: other segments' code passes these offsets some 1,500 times a frame)
void tie640_watch(Bit16u loadSeg) {
    g_load = loadSeg;
    static const Bit16u ips[] = { kSceneTop, kSceneCentre, kSceneEnd, kDrawStars,
                                  XA(0x0775, 0x0775), XA(0xDF1D, 0xE1AD), XA(0xDF26, 0xE1B6), XA(0x09FC, 0x0AEC), XA(0x0C5B, 0x0C18), XA(0x1226, 0x1204), XA(0x139C, 0x137A), XA(0x0782, 0x07E4) };
    static const Bit16u paras[] = { XA(0x0090, 0x0920), XA(0x0090, 0x0920), XA(0x0090, 0x0920), XA(0x1B66, 0x20CE),
                                    XA(0x1CCA, 0x222D), XA(0x1CCA, 0x222D), XA(0x1CCA, 0x222D), XA(0x1CCA, 0x222D), XA(0x0090, 0x0920), XA(0x0090, 0x0920), XA(0x0090, 0x0920), XA(0x168C, 0x1D5D) };
    g_pixelPatched = -1;
    g_checking = tie640_native_on() && getenv("WCNET_T640_CHECK");
    for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) {
        if ((ips[i] == XA(0x0C5B, 0x0C18) || ips[i] == XA(0x1226, 0x1204) || ips[i] == XA(0x139C, 0x137A) || ips[i] == XA(0x09FC, 0x0AEC)) && pixel_patches() && !g_checking) continue;     // NATIVE: the code patched instead
        if ((ips[i] == XA(0xDF1D, 0xE1AD) || ips[i] == XA(0xDF26, 0xE1B6)) && tie640_native_on() && !g_checking) continue;    // NATIVE: the routine whole (DF00)
        place_at(paras[i], ips[i]);
    }
    // [stars] the three per-star places only when the game's own routine is run in the quarters (WCNET_TIE640_STARS=0,
    // a research aid): natively the routine's entry is the stars' one hook a call
    if (!tie640_stars_native()) { place_at(XA(0x1B66, 0x20CE), XA(0x112A, 0x10A4)); place_at(XA(0x1B66, 0x20CE), XA(0x1310, 0x128A)); place_at(XA(0x1B66, 0x20CE), XA(0x14F6, 0x1470)); }
    tie640_fast_places(place_at);   // FAST
    dump_places();      // RESEARCH AID
    if (tie640_trace_check_on()) { place_at(kRendPara, XA(0x53F3, 0x559D)); place_at(kRendPara, XA(0x13DF, 0x14FE)); place_at(kRendPara, XA(0x193D, 0x1A7D)); place_at(kRendPara, XA(0x1A1A, 0x1B5A)); tie640_trace_shapes_places(place_at); }    // TRACE
    if (tie640_native_on()) { place_at(kRendPara, XA(0x85E9, 0x89A7)); place_at(kRendPara, XA(0xDF00, 0xE190)); place_at(kRendPara, XA(0xD772, 0xDB94)); if (!XA(0, 1)) place_at(kRendPara, XA(0x8510, 0x88B0)); if (getenv("WCNET_T640_CHECK")) { place_at(kRendPara, XA(0x98BF, 0x9CD8)); place_at(kRendPara, XA(0x9724, 0x9B30)); } }    // NATIVE
    if (tie640_onepass_on()) { tie640_onepass_places(place_at); place_at(kFlightPara, XA(0x0B5C, 0x0B1A)); }      // ONE PASS
    tie640_fast_hooked(ips, (int)(sizeof(ips) / sizeof(ips[0])));   // FAST
}

// ONE PASS: the scene section about to run once for the whole 640x378 (the CPU at its start, or about to be put there)
namespace {
enum { kOnePass = 5 };
Bit16u g_onePassKeep[4];
bool g_onePassWindow = false, g_onePassRef = false;
void onepass_start() {
    g_pass = kOnePass;
    native_patches(true);
    scale_code(true);
    g_key = mem_readb(dsa(kBackColour));
    const bool full = r16(kFullUpdate) != 0;
    mask_open();
    if (full || !g_filled) fill_window();
    mem_writew(dsa(kFullUpdate), 1);
    g_passX = -1;
    tie640_fast_pass(4, false);
}
// ... the game's window made 640x378 (0090:0B36), and put back (0090:1069)
void onepass_window(bool wide) {
    if (wide == g_onePassWindow) return;
    g_onePassWindow = wide;
    if (wide) {
        const int w = r16(kWide), h = r16(kDeep);
        g_onePassKeep[0] = (Bit16u)w; g_onePassKeep[1] = r16(XA(0x48C7, 0x5427)); g_onePassKeep[2] = (Bit16u)h; g_onePassKeep[3] = r16(XA(0x48D3, 0x5437));
        mem_writew(dsa(kWide), (Bit16u)(2 * w));
        mem_writew(dsa(XA(0x48C7, 0x5427)), (Bit16u)(2 * w - 1));
        mem_writew(dsa(kDeep), (Bit16u)(2 * h));
        mem_writew(dsa(XA(0x48D3, 0x5437)), (Bit16u)(2 * h - 1));
    } else {
        mem_writew(dsa(kWide), g_onePassKeep[0]);
        mem_writew(dsa(XA(0x48C7, 0x5427)), g_onePassKeep[1]);
        mem_writew(dsa(kDeep), g_onePassKeep[2]);
        mem_writew(dsa(XA(0x48D3, 0x5437)), g_onePassKeep[3]);
    }
}
}

void tie640_onepass_ref_sprite(Bit16u loadSeg, int quarter);
bool tie640_hook(Bit32u ip) {
    if ((ip == kSceneTop || ip == kSceneEnd) && SegValue(cs) == (Bit16u)(g_load + kFlightPara)) tie640_fast_scene(g_on, (int)ip);   // FAST
    if (!g_on) return false;
    g_hookCalls++;
    if (g_pass == 4 && cs_is(kRendPara) && dump_hook(ip)) return true;      // RESEARCH AID (before FAST: A02A is its place too)
    // ONE PASS: the window is 640x378 only while drawpol draws an object (its edges' and faces' tests against the
    // window); the sprites' and the backdrop's code, sized for the game's own 320x189 buffer, never sees it
    // ONE PASS: the backdrop's call is over (drawn or not): the window is the 640x378 again for the objects (a
    // sprite's row is worked out from the window's depth when it is queued)
    if (g_pass == kOnePass && ip == XA(0x0B5C, 0x0B1A) && cs_is(kFlightPara)) { onepass_window(true); return true; }
    if (g_pass >= 0 && g_pass < 4 && g_onePassRef && ip == XA(0xA132, 0xA555) && cs_is(kRendPara)) tie640_onepass_ref_sprite(g_load, g_pass);
    if (g_pass == kOnePass && cs_is(kRendPara)) {
        if (ip == XA(0xDF00, 0xE190) || ip == XA(0x0775, 0x0775) || ip == XA(0xA02A, 0xA44D)) onepass_window(false); else if (ip == XA(0x0AAB, 0x0B9B)) onepass_window(true);
    }
    if (g_pass == kOnePass && cs_is(kRendPara) && tie640_onepass_hook(g_load, ip, g_draw, HW, HW, HH, r16(kCorner))) {     // ONE PASS
        if (ip == XA(0xA02A, 0xA44D) && !tie640_onepass_sprite_running()) onepass_window(true);     // (the scaler has drawn all four quarters)
        return true;
    }
    if (ip == XA(0xA02A, 0xA44D) && g_pass >= 0 && g_pass < 4 && cs_is(kRendPara) && tie640_onepass_no_sprites()) {     // ONE PASS (its check without sprites: none in the quarters either)
        far_return();
        return true;
    }
    if (g_pass >= 0 && g_pass < 4 && cs_is(kRendPara) && tie640_native_on() && tie640_fast_reference() && (ip == XA(0x85E9, 0x89A7) || ip == XA(0x98BF, 0x9CD8) || ip == XA(0x9724, 0x9B30))) {
        tie640_native_ref(g_load, ip == XA(0x85E9, 0x89A7) ? 0 : ip == XA(0x98BF, 0x9CD8) ? 1 : 2);       // NATIVE (the check: the game's own runs noted)
        return true;
    }
    if (g_pass >= 0 && g_pass < 4 && cs_is(kRendPara) && tie640_trace_check_on()) {      // TRACE (the check of wcnet_tie640_trace.cpp)
        if (ip == XA(0x53F3, 0x559D)) { tie640_trace_check_entry(g_load); return true; }
        if (ip == XA(0x13DF, 0x14FE) || ip == XA(0x193D, 0x1A7D) || ip == XA(0x1A1A, 0x1B5A)) { tie640_trace_check_return(g_load); return true; }
        if (tie640_trace_shapes_hook(g_load, ip)) return true;      // TRACE
    }
    if ((ip == XA(0xD772, 0xDB94) || ip == XA(0x8510, 0x88B0)) && g_pass >= 0 && (g_pass < 4 || g_pass == kOnePass) && cs_is(kRendPara) && tie640_native_on() && !tie640_fast_reference()
        && tie640_native_fill(g_load, ip)) return true;     // NATIVE: the full update's two fills
    // NATIVE: the quarter's scanline pass (xtrans2_drawxtrans) done by wcnet_tie640_native.cpp
    if (ip == XA(0x85E9, 0x89A7) && g_pass >= 0 && g_pass < 4 && g_passX >= 0 && cs_is(kRendPara) && tie640_native_on() && !tie640_fast_reference()) {
        tie640_native_quarter(g_pass);
        tie640_native_scanlines(g_load, g_draw, HW, HW, HH, g_passX, g_passY);
        return true;
    }
    if (tie640_fast_hook(ip)) return true;      // FAST
    if (cs_is(kFlightPara)) {
        switch (ip) {
        case kSceneTop:
            // (natively no hook's call comes for a place jumped to; in the browser, where the hooks run from the
            // main loop, one does: the next quarter's arrival here is not the frame's)
            if (g_jumped) {
                g_jumped = false;
                return true;
            }
            if (tie640_onepass_on()) {      // ONE PASS (its check: the quarters first)
                if (!tie640_onepass_check()) { onepass_start(); return true; }
                tie640_onepass_check_begin();
                g_onePassRef = true;
            }
            g_pass = 0;
            tie640_fast_frame_begin();      // FAST (the check of whole frames)
            pass_begin();
            tie640_fast_pass(g_pass, false);    // FAST
            tie640_fast_run_scene();            // FAST (the last thing here: the section on as translated code)
            return true;
        case kSceneCentre:
            g_jumped = false;
            if (g_pass < 0) return true;
            // the projection's centre for this quarter: twice the picture's centre, less the quarter's corner
            g_keepHalfWide = r16(kHalfWide);
            g_keepYOffset = mem_readd(dsa(kYOffset));
            g_centreMoved = true;
            {
                const int cx = (Bit16s)g_keepHalfWide, half = (Bit16s)r16(kHalfDeep), cy = half + (Bit32s)g_keepYOffset;
                const int qx = g_pass < 4 ? (g_pass & 1) : 0, qy = g_pass < 4 ? (g_pass >> 1) : 0;    // (the one pass: the whole picture's centre)
                mem_writew(dsa(kHalfWide), (Bit16u)(2 * cx - (int)r16(kWide) * qx));
                mem_writed(dsa(kYOffset), (Bit32u)(2 * cy - (int)r16(kDeep) * qy - half));
            }
            if (g_pass == 4) dump_centre();     // RESEARCH AID
            if (g_pass == kOnePass) {           // ONE PASS: the quarters' images start from here; then the window 640x378
                tie640_onepass_begin(g_load);
                onepass_window(true);
            }
            return true;
        case kSceneEnd:
            if (g_pass < 0) return true;
            if (g_centreMoved) {
                g_centreMoved = false;
                mem_writew(dsa(kHalfWide), g_keepHalfWide);
                mem_writed(dsa(kYOffset), g_keepYOffset);
            }
            mem_writew(dsa(kFullUpdate), 0);
            g_passX = -1;
            if (g_pass == kOnePass) {           // ONE PASS: the frame is done
                onepass_window(false);
                g_pass = -1;
                scale_code(false);
                mask_back();
                if (tie640_onepass_check()) tie640_onepass_check_end(g_load, g_draw, (int)r16(kWide), (int)r16(kDeep), r16(kCorner));
                if (tie640_stars_native()) tie640_stars_plot(g_draw, HW, 2 * (int)r16(kWide), 2 * (int)r16(kDeep));
                memcpy(g_hi, g_draw, sizeof(g_hi));
                g_haveHi = true;
                g_frames++;
                tie640_fast_pass(-1, true);
                return true;
            }
            if (tie640_fast_again(g_pass, g_draw, r16(kWide), r16(kDeep), r16(kCorner))) {    // FAST (the check: the same quarter once more)
                pass_begin(true);
                reg_eip = kSceneTop;
                g_jumped = true;
                tie640_fast_run_scene();        // FAST (the last thing here)
                return true;
            }
            if (g_pass == 4) dump_end();        // RESEARCH AID: the fifth pass (one pass, 640x378, nothing drawn) is over
            else if (g_pass == 3 && g_dumpAsked && dump_enabled()) {
                g_pass = 4;
                dump_begin();
                reg_eip = kSceneTop;
                g_jumped = true;
                return true;
            }
            if (g_pass < 4 && ++g_pass < 4) {
                pass_begin();
                tie640_fast_pass(g_pass, false);    // FAST
                reg_eip = kSceneTop;        // (the section again, the next quarter)
                g_jumped = true;
                tie640_fast_run_scene();            // FAST (the last thing here)
                return true;
            }
            if (g_onePassRef) {         // ONE PASS (its check): the quarters are done; the memory put back, the frame in one pass
                g_onePassRef = false;
                tie640_onepass_check_middle(g_draw);
                g_scaled = false;       // (the renderer's code and the mask are the game's own again with the memory)
                g_patchedNow = false;
                onepass_start();
                reg_eip = kSceneTop;
                g_jumped = true;
                return true;
            }
            if (tie640_fast_frame_again(g_draw)) {      // FAST (the check: the memory is the scene's start again)
                g_scaled = false;       // (... the renderer's code and the mask the game's own with it)
                g_pass = 0;
                pass_begin();
                tie640_fast_pass(g_pass, false);
                reg_eip = kSceneTop;
                g_jumped = true;
                tie640_fast_run_scene();        // (the last thing here)
                return true;
            }
            g_pass = -1;
            scale_code(false);
            mask_back();
            // [stars] the stars gathered at the first quarter's rtsvga2_drawstars, into the finished scene
            if (tie640_stars_native()) tie640_stars_plot(g_draw, HW, 2 * (int)r16(kWide), 2 * (int)r16(kDeep));
            memcpy(g_hi, g_draw, sizeof(g_hi));
            g_haveHi = true;
            g_frames++;
            tie640_fast_pass(-1, true);     // FAST
            return true;
        case XA(0x0C5B, 0x0C18): case XA(0x1226, 0x1204): case XA(0x139C, 0x137A):
            // the craft's, the bolts' and the static objects' test, depth / 256 in DX:AX against the object's size
            // (smaller than a pixel: not drawn): a pixel is half the size now
            if (g_pass >= 0 && !g_patchedNow) {
                const Bit32s v = (Bit32s)(((Bit32u)reg_dx << 16) | reg_ax) >> 1;
                reg_ax = (Bit16u)v;
                reg_dx = (Bit16u)((Bit32u)v >> 16);
            }
            return true;
        }
        return false;
    }
    if (g_pass < 0) return false;
    if (cs_is(kRendPara)) {
        switch (ip) {
        case XA(0xDF00, 0xE190): {      // NATIVE: rotscale_calcscale(depth lo, hi, size, factor) whole: (2 size / (depth >> 8)) * factor >> 8, 800h at most
            if (!tie640_native_on() || tie640_fast_reference()) return false;
            const PhysPt a = ((PhysPt)SegValue(ss) << 4) + (Bit16u)((reg_esp & 0xFFFF) + 4);
            const Bit16u lo = mem_readw(a), hi = mem_readw(a + 2), size = mem_readw(a + 4), factor = mem_readw(a + 6);
            Bit16u by = (Bit16u)((lo >> 8) | ((hi & 0xFF) << 8));
            if (hi >> 8) by = (Bit16u)((by & 0xFF) | 0xFF00);
            if (!by) by = 1;
            const Bit32u v = (Bit32u)size << 1;
            Bit16u q;
            if (v / by > 0xFFFF) { q = 0xFFFF; by = 1; } else q = (Bit16u)(v / by);
            const Bit32u m = (Bit32u)q * factor;
            Bit16u r = (Bit16u)(m >> 8);
            if (r > 0x800) r = 0x800;
            reg_ax = r; reg_dx = (Bit16u)(m >> 16); reg_bx = by; reg_cx = hi;
            far_return();
            return true;
        }
        case XA(0x09FC, 0x0AEC): {      // draw_getcompdetailptr(component, z): the depth its detail level is picked by, halved
            if (g_patchedNow) return true;
            const PhysPt a = ((PhysPt)SegValue(ss) << 4) + (Bit16u)((reg_esp & 0xFFFF) + 8);
            mem_writed(a, (Bit32u)((Bit32s)mem_readd(a) >> 1));
            return true;
        }
        case XA(0xDF1D, 0xE1AD): {      // the sprites' scale: the size in AX about to be divided by the depth in BX
            const Bit32u v = (Bit32u)reg_ax << 1;
            if (reg_bx && v / reg_bx > 0xFFFF) {
                reg_ax = 0xFFFF;
                reg_dx = 0;
                reg_bx = 1;
            } else {
                reg_ax = (Bit16u)v;
                reg_dx = (Bit16u)(v >> 16);
            }
            return true;
        }
        case XA(0xDF26, 0xE1B6):        // ... its result's limit (0x400, four times: eight now)
            if (reg_ax > 0x800) reg_ax = 0x800;
            reg_eip = XA(0xDF2E, 0xE1BE);
            return true;
        case XA(0x0775, 0x0775):        // the backdrop's place from the centre, AX and BX
            reg_ax = (Bit16u)(reg_ax << 1);
            reg_bx = (Bit16u)(reg_bx << 1);
            return true;
        }
        return false;
    }
    if (cs_is(kStarPara)) {
        if (ip == kDrawStars && tie640_stars_native()) {
            // [stars] the routine is not run: the stars' places at the 640 picture's scale are worked out once a
            // frame (the picture's centre: the frame's own, doubled) and plotted when the scene is complete
            if (g_pass == 0 || g_pass == kOnePass) {
                const int cx = (Bit16s)g_keepHalfWide, cy = (Bit16s)r16(kHalfDeep) + (Bit32s)g_keepYOffset;
                const int twice = g_pass == kOnePass ? 1 : 2;       // (ONE PASS: the window is the 640x378 already)
                tie640_stars_gather(g_load, 2 * cx, 2 * cy, twice * (int)r16(kWide), twice * (int)r16(kDeep));
            }
            tie640_stars_skip(g_load);
            // (the return lands on the section's end, and no hook's call comes for a place jumped to)
            if (cs_is(kFlightPara) && reg_eip == kSceneEnd) return tie640_hook(kSceneEnd);
            return true;
        }
        if (ip == kDrawStars) {
            mem_writew(dsa(kFullUpdate), 1);    // (the frame has cleared it: the stars' lists are another quarter's too)
            return true;
        }
        if (ip == XA(0x112A, 0x10A4) || ip == XA(0x1310, 0x128A) || ip == XA(0x14F6, 0x1470)) {     // the stars' three loops: AX, BX the star from the centre
            reg_ax = (Bit16u)(reg_ax << 1);
            reg_bx = (Bit16u)(reg_bx << 1);
            return true;
        }
        return false;
    }
    if (ip == XA(0x0782, 0x07E4) && cs_is(kBackdropPara)) {     // the backdrop picture's scale (0x100 = 1), about to be pushed
        reg_ax = (Bit16u)(reg_ax << 1);
        return true;
    }
    return false;
}

bool tie640_on() { return g_on; }

// The browser's CPU core at a watched place (wc_net_hook_exit): the one pass's places in the renderer are dealt with
// there and then, as natively -- they wait on nothing, and a trip to the main loop for each is the frame's cost.
bool tie640_fast_trip(Bit32u ip) {
    if (!g_on || g_pass != kOnePass || !cs_is(kRendPara)) return false;
    return tie640_hook(ip);
}

static float g_fps = 70; static double g_ratio = 1;       // the picture's rate and shape as given to the emulator's renderer
static void wanted_init() {     // (static: this file is compiled twice, for X-Wing too)
    if (g_wanted < 0) {
        const char *env = getenv("WCNET_TIE640");
        g_wanted = env && env[0] == '0' ? 0 : 1;
    }
}

void tie640_tick(bool flight, Bit16u loadSeg) {
    wanted_init();
    g_load = loadSeg;
    const bool want = g_wanted && flight && vga.mode == M_VGA && code_known();
    if (flight) tie640_fast_tick(loadSeg);      // FAST
    if (want == g_on) return;
    g_on = want;
    g_load = loadSeg;
    if (g_on) {
        reset_state();
        const double fps = vga.draw.delay.vtotal > 0 ? 1000.0 / vga.draw.delay.vtotal : 70.0;
        const double ratio = vga.draw.aspect_ratio;
        VGA_SetOverride(true);
        g_fps = (float)fps; g_ratio = ratio;
        RENDER_SetSize(HW, HH, 8, (float)fps, ratio, false, false);
        wc_hires_watch = true;
        wclog(1, T640_GAME ": the flight's picture is 640x400 (the game's 320x200 behind it)");
    } else {
        wclog(1, T640_GAME ": the picture is the game's own again (%s)", !flight ? "the flight is over" : vga.mode != M_VGA ? "the game has left mode 13h" : "switched off");
        if (g_pass >= 0) mask_back();
        onepass_window(false);      // ONE PASS
        g_onePassRef = false;
        scale_code(false);
        g_pass = g_passX = -1;
        wc_hires_watch = false;
        VGA_SetOverride(false);
    }
}

// The backquote key (wcnet_hooks.cpp; with the page's voice on push to talk, Shift+` -- either works at any time):
// the 640x400 picture off, the game's own 320x200 in its place, or on again, as Privateer's and Armada's flip theirs
// (the user, 2026-11-26: "is it possible to make ` toggle resolution in tie demo and xwing cd?").  The picture follows
// at the next tick, which also turns it on at a flight's start and off at its end, wherever the game is.
void tie640_flip() {
    wanted_init();
    g_wanted = !g_wanted;
    wclog(1, T640_GAME ": the 640x400 picture %s (the ` key)", g_wanted ? "on" : "off: the game's own 320x200");
}

// The flight's frames against the HOST's clock (the user, 2026-11-23: "the interesting number is the minimum framerate
// over 30 seconds of gameplay"): a game frame counted in the wall second it came in; every 30 s, and when asked
// (!wallfps), the fewest frames of any one second, the mean, and how much emulated time passed in that wall time (work
// done outside the emulated clock, the sprites' scaler run inside a hook, shows here and nowhere in the game's own
// frame rate).
void tie640_onepass_cost(char *to, size_t n);
void tie640_wall_frame(bool say) {
    static double since = 0, secAt = 0, emuAt = 0;
    static int inSec = 0, least = 1 << 30, most = 0, total = 0, secs = 0;
    static std::string each;
    const double now = perf_now_ms();
    if (since == 0) { since = secAt = now; emuAt = PIC_FullIndex(); }
    if (!say) { inSec++; total++; }
    while (now - secAt >= 1000) {
        if (inSec < least) least = inSec;
        if (inSec > most) most = inSec;
        secs++;
        { char b[8]; snprintf(b, sizeof(b), " %d", inSec); each += b; }
        inSec = 0;
        secAt += 1000;
    }
    if ((say && secs > 0) || now - since >= 30000) {
        const double emu = PIC_FullIndex() - emuAt, wall = now - since;
        char cost[240];
        tie640_onepass_cost(cost, sizeof(cost));
        wclog(1, T640_GAME ": wall clock: %d s of flight: the fewest frames in a second %d, the most %d, the mean %.1f; the emulator at %.0f%% of real time (%s); each second:%s; %s",
              secs, least, most, secs ? (double)total / secs : 0.0, wall > 0 ? 100.0 * emu / wall : 0.0, g_on ? "640x400" : "the game's own picture", each.c_str(), cost);
        each.clear();
        since = secAt = now;
        emuAt = PIC_FullIndex();
        least = 1 << 30; most = total = secs = inSec = 0;
    }
}

bool tie640_shot(const std::string &path) {
    if (!g_on) return false;
    compose();
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", (unsigned)HW, (unsigned)HH);
    for (int i = 0; i < HW * HH; i++) {
        const RGBEntry &c = vga.dac.rgb[g_out[i]];
        fputc((c.red << 2) | (c.red >> 4), f);
        fputc((c.green << 2) | (c.green >> 4), f);
        fputc((c.blue << 2) | (c.blue >> 4), f);
    }
    fclose(f);
    return true;
}

bool tie640_script_item(const std::string &item, double t) {
    if (tie640_fast_script_item(item, t)) return true;      // FAST
    if (item == "!t640trace") { tie640_trace_check_say(); return true; }     // TRACE
    if (item == "!t640native") { tie640_native_say(); if (tie640_onepass_on()) tie640_onepass_say(); return true; }    // NATIVE: its counts, and the next frame's decisions logged
    if (item.compare(0, 10, "!t640dump=") == 0) {       // RESEARCH AID (wcnet_tie640_dump.inc)
        g_dumpName = item.substr(10);
        g_dumpAsked = true;
        wclog(1, "key script %.1fs: TIE demo 640: one-pass dump %s asked (%s)", t, g_dumpName.c_str(), dump_enabled() ? "on" : "needs WCNET_T640_DUMP=1 WCNET_T640_FAST=0");
        return true;
    }
    if (item.compare(0, 9, "!t640low=") == 0) {
        // research: the game's own picture (L) and the window (W) as PGMs of palette indices
        const char *dir = getenv("WCNET_DUMP_DIR");
        const std::string base = std::string(dir && dir[0] ? dir : ".") + "/" + item.substr(9);
        FILE *f = fopen((base + "_low.pgm").c_str(), "wb");
        if (f) { fprintf(f, "P5\n320 200\n255\n"); fwrite(low(), 1, LW * LH, f); fclose(f); }
        f = fopen((base + "_win.pgm").c_str(), "wb");
        if (f) { fprintf(f, "P5\n320 200\n255\n"); for (int i = 0; i < LW * LH; i++) fputc(g_window[i] ? 255 : 0, f); fclose(f); }
        wclog(1, "key script %.1fs: TIE demo 640: %s_low.pgm, _win.pgm (frames %u, pass %d, key %02X, hook calls %u)", t, base.c_str(), g_frames, g_pass, g_key, g_hookCalls);
        return true;
    }
    if (item.compare(0, 10, "!t640stars") == 0) {     // [stars]
        tie640_stars_say(t, item.size() > 11 ? item.substr(11) : std::string());
        return true;
    }
    if (item == "!t640writers") {
        // research: which code wrote the picture since the last call (16-byte buckets, bytes written)
        std::vector<std::pair<unsigned, Bit32u> > top;
        for (std::map<Bit32u, unsigned>::const_iterator i = g_writers.begin(); i != g_writers.end(); ++i) top.push_back(std::make_pair(i->second, i->first));
        std::sort(top.rbegin(), top.rend());
        std::string text;
        for (size_t i = 0; i < top.size() && i < 80; i++) {
            char b[40];
            snprintf(b, sizeof(b), " %04X:%04X=%u", top[i].second >> 16, top[i].second & 0xFFFF, top[i].first);
            text += b;
        }
        wclog(1, "key script %.1fs: TIE demo 640: the picture's writers (image paragraph:IP=bytes):%s", t, text.c_str());
        g_writers.clear();
        g_countWriters = true;
        return true;
    }
    return false;
}

}  // namespace wc

using namespace wc;

bool wc_hires_write(PhysPt offset, Bitu val, int bytes) {
#ifndef T640_XWING
    if (xw640_on()) return xw640_hires_write(offset, val, bytes);
#endif
    if (!g_on) return false;
    if (g_countWriters) g_writers[((Bit32u)(Bit16u)(SegValue(cs) - g_load) << 16) | (reg_eip & 0xFFF0)] += bytes;
    if (g_passX < 0) return false;
    // a quarter of the scene being drawn: its pixels land in H, the VGA's memory is left as it is
    for (int i = 0; i < bytes; i++, offset++, val >>= 8) {
        if (offset >= (PhysPt)(LW * LH)) continue;
        const int x = g_passX + (int)(offset % LW), y = g_passY + (int)(offset / LW);
        if (y < HH && x < HW) g_draw[y * HW + x] = (Bit8u)val;
    }
    return true;
}

bool wc_hires_read(PhysPt offset, int bytes, Bitu *val) {
#ifndef T640_XWING
    if (xw640_on()) return xw640_hires_read(offset, bytes, val);
#endif
    if (!g_on || g_passX < 0) return false;
    // ... and what it reads of the screen (the stars' routine: is the pixel still the background's?) is H's
    Bitu v = 0;
    for (int i = 0; i < bytes; i++, offset++) {
        Bit8u c = g_key;
        if (offset < (PhysPt)(LW * LH)) {
            const int x = g_passX + (int)(offset % LW), y = g_passY + (int)(offset / LW);
            if (y < HH && x < HW) c = g_draw[y * HW + x];
        }
        v |= (Bitu)c << (8 * i);
    }
    *val = v;
    return true;
}

void wc_hires_vsync() {
    if (!g_on) return;
    compose();      // (whatever the renderer says: the key script's pictures are this)
    static unsigned refused = 0, said = 0;
    if (!RENDER_StartUpdate()) {
        // The emulator's renderer is not taking frames.  A frame skipped is nothing; when it goes on (seen once:
        // the picture stood still after a death and the next launch) the picture's size is given to it again.
        if (++refused == 30) {
            if (said++ < 5) wclog(0, T640_GAME " 640: the renderer took no frame 30 times running: its size set again");
            RENDER_SetSize(HW, HH, 8, wc::g_fps, wc::g_ratio, false, false);
            refused = 0;
        }
        return;
    }
    refused = 0;
    for (int y = 0; y < HH; y++) RENDER_DrawLine(g_out + y * HW);
    RENDER_EndUpdate(false);
}
