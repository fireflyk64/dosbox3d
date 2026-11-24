/*
 *  The TIE Fighter demo's stars, natively (wcnet_tie640.h, docs/tie640.md).
 *
 *  TIEMISS.EXE's rtsvga2_drawstars (image 1B66:0F0E..15E9, far, no arguments) plots the star field straight into the
 *  VGA.  What it does, read from its code:
 *
 *    The stars sit on a cube around the eye, 16384 units a side.  A face holds 16 x 16 of them on a grid, each moved
 *    off its grid point by one of 64 small steps: a star is two bytes of the table at DS:63D2 (256 stars: the step's
 *    number 0..63, a brightness 0..7; random numbers put there when the flight starts), and the SAME 256 are used on
 *    every face.  Three faces meet at the cube's corner the routine starts from; a star whose depth comes out
 *    negative is turned through the eye (all three coordinates negated), which gives the three faces opposite: 768
 *    stars seen from inside, 1536 places.
 *
 *    The cube turned into the eye's axes is ready in tables another routine (1B66:1640) fills from the camera's
 *    matrix DS:288C (nine words, 32767 = 1): the 64 steps (x at DS:1B89, y at 1C09, z at 1C89: words), and for each
 *    of the cube's three edges its first 16 multiples (edge U: x at DS:1D75, y 1D95, z 1DB5; edge V: 1DD5, 1DF5,
 *    1E15; edge W: 1E35, 1E55, 1E75).  The routine itself makes the corner: minus a quarter of the sum of the
 *    matrix's three columns' words (x from 288C, 2892, 2898; y from 288E, 2894, 289A; z from 2890, 2896, 289C).
 *
 *    Loop 1 walks face U x V (U the inner edge), loop 2 U x W, loop 3 V x W; the step along both edges is the detail
 *    DS:1B64 (1: every star).  A row's first point is the corner for row 0 and corner + outer[(row - 1) * step] from
 *    then on; a star is row point + inner[column * step] + its step (the tables' entry k is k + 1 edges' sixteenths:
 *    the columns are 1..16 sixteenths along the inner edge, the rows 0..15 along the outer).
 *
 *    Projection, 16 bits: a star is refused when |x| >> 8 >= z >> 8 or |y| >> 8 >= z >> 8 (more than 255 pixels from
 *    the centre: the divide would overflow); else its place from the centre is (|v| * 256) / z, unsigned, the
 *    remainder dropped, the sign put back: toward zero.  The centre's column DS:48C9 and row DS:48D5 + DS:289E are
 *    added, the window DS:48C5 x DS:48D1 tested, the offset in the VGA is CS:071E[row] + column + DS:48C3.
 *
 *    A star is plotted only where the screen's pixel is at or above the background's colour DS:48BC (F7: the
 *    background or a star); its colour is F8 + min(7, its brightness + DS:E7C5).  The offsets plotted go into a
 *    list in the routine's own code segment (CS:0000 or CS:0200, 0x200 bytes each, FFFF-ended: DS:1B85 this frame's,
 *    DS:1B87 the last frame's, DS:1B83 where the next goes) and into a hash of 512 words at DS:3090 (cleared at
 *    entry; slot = offset * 2 + 2 and on in twos, mod 0x400).  At the end every offset of the last frame's list still
 *    above the background's colour and not in the hash is put back to the background's colour.  DS:BF98 (the whole
 *    window is drawn again) empties the last frame's list first.
 *
 *  Here: the same arithmetic (walk), a check against the game's own plots in its 320x200 (WCNET_TIESTARS_CHECK=1),
 *  and for the 640x400 picture the stars' places at twice the scale, (|v| * 512) / z, one pixel each.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcnet_tie640.h"
#include "wcnet_log.h"
#include "regs.h"
#include "mem.h"
#include "cpu.h"
#include "vga.h"

namespace wc {

namespace {

enum {
    kStarPara = 0x1B66, kDataPara = 0x421F,
    kEntry = 0x0F0E, kPlot1 = 0x116B, kPlot2 = 0x1351, kPlot3 = 0x1537, kErase = 0x15DF, kExit = 0x15E4,
    kRowTable = 0x071E,         // CS: the rows' offsets (words)
    kStars = 0x63D2,            // DS: 256 stars, two bytes each (the step's number, the brightness)
    kStepX = 0x1B89, kStepY = 0x1C09, kStepZ = 0x1C89,      // DS: the 64 steps in the eye's axes
    kEdgeU = 0x1D75, kEdgeV = 0x1DD5, kEdgeW = 0x1E35,      // DS: an edge's 16 multiples, x then y (+0x20) then z (+0x40)
    kMatrix = 0x288C,           // DS: the camera's matrix, nine words
    kDetail = 0x1B64,           // DS: the step along the edges
    kGlobalBright = 0xE7C5,     // DS: added to every star's brightness (a byte)
    kBackColour = 0x48BC, kCorner = 0x48C3, kWide = 0x48C5, kHalfWide = 0x48C9, kDeep = 0x48D1, kHalfDeep = 0x48D5,
    kYOffset = 0x289E, kFullUpdate = 0xBF98,
    kScreenSeg = 0x1B66,        // DS: the screen's segment (a word; by chance the number of the routine's paragraph)
    kListNow = 0x1B85, kListLast = 0x1B87, kListCursor = 0x1B83, kHash = 0x3090,
    kMaxStars = 768,            // three faces of 256: no walk gives more
};

// The 640 picture's stars are one pixel where the doubled 320 picture's were four, and the game's eight star colours
// are dim (F8 = 9 11 20 of 63 up to FF = 37 37 44): a star's colour is made this much brighter, in percent of the
// DAC's levels (each of red, green, blue; 63 the most), and the palette's nearest colour is what is plotted.  100 is
// the game's own colour; 188 gives the one pixel the light of the four (4 ^ (1 / 2.2), the screen's gamma), where
// the brightest clip to white.  (WCNET_TIE640_STARGAIN= another.)
const int kStarGainPercent = 150;

struct Star { Bit16s x, y; Bit8u bright; };     // from the centre; the brightness as the table has it

Bit16u g_load;
inline PhysPt dsa(Bit16u off) { return ((PhysPt)(Bit16u)(g_load + kDataPara) << 4) + off; }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline Bit8u r8(Bit16u off) { return mem_readb(dsa(off)); }
inline PhysPt csa(Bit16u off) { return ((PhysPt)(Bit16u)(g_load + kStarPara) << 4) + off; }

// One coordinate's place from the centre: false when the game refuses the star.
inline bool project(Bit16u v, Bit16u z, int shift, Bit16s *out) {
    const bool neg = (v & 0x8000) != 0;
    if (neg) v = (Bit16u)(0 - v);
    if ((v >> 8) >= (z >> 8)) return false;         // (z >> 8 is 1 at least from here on)
    const Bit32u q = ((Bit32u)v << shift) / z;
    *out = (Bit16s)(neg ? 0 - q : q);
    return true;
}

// The corner's coordinate: minus a quarter of three of the matrix's words, the two bits shifted out carried between
// them as the game does (the sum of the three quarters, rounded down).
inline Bit16u corner(Bit16u a, Bit16u b, Bit16u c) {
    Bit32s sum = 0;
    const Bit16u m[3] = { a, b, c };
    for (int i = 0; i < 3; i++) sum += (Bit16s)(Bit16u)(0 - m[i]);
    return (Bit16u)(sum >> 2);
}

// Every star the game would project inside its guard, in the game's order: the three loops.  shift 8 is the game's
// scale, 9 twice it.  Returns the count (kMaxStars at most); 0 when the detail step is none the loops end on.
int walk(int shift, Star *out) {
    const int step = (Bit16s)r16(kDetail);
    if (step != 1 && step != 2 && step != 4 && step != 8 && step != 16) return 0;
    Bit16u stepv[3][64], edge[3][3][16], base[3];
    Bit8u stars[512];
    static const Bit16u kSteps[3] = { kStepX, kStepY, kStepZ }, kEdges[3] = { kEdgeU, kEdgeV, kEdgeW };
    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < 64; i++) stepv[a][i] = r16((Bit16u)(kSteps[a] + 2 * i));
        for (int e = 0; e < 3; e++) for (int i = 0; i < 16; i++) edge[e][a][i] = r16((Bit16u)(kEdges[e] + 0x20 * a + 2 * i));
        base[a] = corner(r16((Bit16u)(kMatrix + 2 * a)), r16((Bit16u)(kMatrix + 6 + 2 * a)), r16((Bit16u)(kMatrix + 12 + 2 * a)));
    }
    for (int i = 0; i < 512; i++) stars[i] = r8((Bit16u)(kStars + i));
    static const int kInner[3] = { 0, 0, 1 }, kOuter[3] = { 1, 2, 2 };
    int n = 0;
    for (int loop = 0; loop < 3; loop++) {
        const Bit16u (*in)[16] = edge[kInner[loop]], (*outer)[16] = edge[kOuter[loop]];
        Bit16u row[3] = { base[0], base[1], base[2] };
        int di = 0;
        for (int r = 0; r < 16; r += step) {
            for (int c = 0; c < 16; c += step, di += 2) {
                const int s = stars[di];            // (0..63 as made; the game would read past the 64 for more: so here)
                Bit16u v[3];
                for (int a = 0; a < 3; a++) v[a] = (Bit16u)(row[a] + in[a][c] + (s < 64 ? stepv[a][s] : r16((Bit16u)(kSteps[a] + 2 * s))));
                if (v[2] & 0x8000) for (int a = 0; a < 3; a++) v[a] = (Bit16u)(0 - v[a]);
                Bit16s x, y;
                // (the game tests x's guard, then y's, then divides y, then x: the order makes no difference)
                if (!project(v[0], v[2], shift, &x) || !project(v[1], v[2], shift, &y)) continue;
                if (n < kMaxStars) {
                    out[n].x = x;
                    out[n].y = y;
                    out[n].bright = stars[di + 1];
                    n++;
                }
            }
            for (int a = 0; a < 3; a++) row[a] = (Bit16u)(outer[a][r] + base[a]);
        }
    }
    return n;
}

inline Bit8u colour_of(Bit8u bright) {
    Bit8u b = (Bit8u)(bright + r8(kGlobalBright));
    if (b > 7) b = 7;
    return (Bit8u)(0xF8 + b);
}

// The eight star colours made brighter: for each the palette's nearest to it (the DAC as it is now: a fade's too).
void brighter(int percent, Bit8u map[8]) {
    for (int b = 0; b < 8; b++) {
        const RGBEntry &own = vga.dac.rgb[0xF8 + b];
        map[b] = (Bit8u)(0xF8 + b);
        if (percent == 100) continue;
        int want[3] = { own.red * percent / 100, own.green * percent / 100, own.blue * percent / 100 };
        for (int k = 0; k < 3; k++) if (want[k] > 63) want[k] = 63;
        int best = 1 << 30;
        for (int c = 0; c < 256; c++) {
            const RGBEntry &e = vga.dac.rgb[c];
            const int dr = e.red - want[0], dg = e.green - want[1], db = e.blue - want[2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < best) { best = d; map[b] = (Bit8u)c; }
        }
    }
}

// --- the check: the game's own plots against walk()'s, in its 320x200 --------------------------------------------------
int g_check = -1;
struct Plot { Bit16u off; Bit8u colour; };
Plot g_want[kMaxStars], g_got[kMaxStars + 1];
Bit16u g_wantErase[1024], g_gotErase[1024];
int g_wantN, g_gotN, g_wantEraseN, g_gotEraseN;
bool g_armed;
unsigned g_framesChecked, g_framesWrong, g_plotsChecked, g_erasesChecked, g_mostStars, g_brightHist[8];

void check_entry() {
    g_armed = true;
    g_gotN = g_gotEraseN = g_wantN = g_wantEraseN = 0;
    static Star stars[kMaxStars];
    static Bit8u screen[0x10000];
    static Bit16u hash[512];
    const PhysPt vga = (PhysPt)r16(kScreenSeg) << 4;
    const Bit8u key = r8(kBackColour);
    const Bit16u halfW = r16(kHalfWide), halfD = r16(kHalfDeep), yoff = r16(kYOffset), wide = r16(kWide), deep = r16(kDeep),
                 corner = r16(kCorner);
    const int n = walk(8, stars);
    memset(hash, 0, sizeof(hash));
    // (the screen as it is now, where the routine may look: the window)
    static bool seen[0x10000];
    memset(seen, 0, sizeof(seen));
    #define SCREEN(o) (seen[o] ? screen[o] : (seen[o] = true, screen[o] = mem_readb(vga + (o))))
    for (int i = 0; i < n; i++) {
        Bit16u bx = (Bit16u)(stars[i].y + halfD);
        bx = (Bit16u)(bx + yoff);
        if (bx & 0x8000) continue;
        const Bit16u ax = (Bit16u)(stars[i].x + halfW);
        if ((ax & 0x8000) || ax >= wide || bx >= deep) continue;
        const Bit16u off = (Bit16u)(mem_readw(csa((Bit16u)(kRowTable + 2 * bx))) + ax + corner);
        if (SCREEN(off) < key) continue;
        const Bit8u c = colour_of(stars[i].bright);
        screen[off] = c;
        g_want[g_wantN].off = off;
        g_want[g_wantN++].colour = c;
        Bit16u h = (Bit16u)(off << 1);
        do h = (Bit16u)((h + 2) & 0x3FF); while (hash[h >> 1]);
        hash[h >> 1] = off;
    }
    // the last frame's list (this frame's pointer at entry: the routine swaps the two), unless all is drawn again
    if (!r16(kFullUpdate)) {
        Bit16u at = r16(kListNow);
        for (int i = 0; i < 1023; i++, at = (Bit16u)(at + 2)) {
            const Bit16u off = mem_readw(csa(at));
            if (off == 0xFFFF) break;
            if (SCREEN(off) <= key) continue;
            Bit16u h = (Bit16u)(off << 1);
            bool there = false;
            for (;;) {
                h = (Bit16u)((h + 2) & 0x3FF);
                if (!hash[h >> 1]) break;
                if (hash[h >> 1] == off) { there = true; break; }
            }
            if (there) continue;
            screen[off] = key;
            g_wantErase[g_wantEraseN++] = off;
        }
    }
    #undef SCREEN
}

void check_exit() {
    if (!g_armed) return;
    g_armed = false;
    bool same = g_gotN == g_wantN && g_gotEraseN == g_wantEraseN;
    for (int i = 0; same && i < g_wantN; i++) same = g_got[i].off == g_want[i].off && g_got[i].colour == g_want[i].colour;
    for (int i = 0; same && i < g_wantEraseN; i++) same = g_gotErase[i] == g_wantErase[i];
    g_framesChecked++;
    g_plotsChecked += (unsigned)g_gotN;
    g_erasesChecked += (unsigned)g_gotEraseN;
    if ((unsigned)g_gotN > g_mostStars) g_mostStars = (unsigned)g_gotN;
    for (int i = 0; i < g_gotN; i++) g_brightHist[g_got[i].colour & 7]++;
    if (!same) {
        g_framesWrong++;
        if (g_framesWrong <= 5) {
            int i = 0;
            while (i < g_gotN && i < g_wantN && g_got[i].off == g_want[i].off && g_got[i].colour == g_want[i].colour) i++;
            wclog(1, "TIE demo stars check: frame %u DIFFERS: the game plotted %d, the twin %d (first difference at %d: %04X/%02X against %04X/%02X); erased %d, the twin %d",
                  g_framesChecked, g_gotN, g_wantN, i, i < g_gotN ? g_got[i].off : 0xFFFF, i < g_gotN ? g_got[i].colour : 0,
                  i < g_wantN ? g_want[i].off : 0xFFFF, i < g_wantN ? g_want[i].colour : 0, g_gotEraseN, g_wantEraseN);
        }
    }
    if (g_framesChecked % 50 == 0)
        wclog(1, "TIE demo stars check: %u frames compared, %u differ; %u plots, %u erasures; this frame %d stars, the most %u; detail %d, brightness added %d; matrix %d %d %d; colours F8..FF %u %u %u %u %u %u %u %u",
              g_framesChecked, g_framesWrong, g_plotsChecked, g_erasesChecked, g_gotN, g_mostStars, (int)(Bit16s)r16(kDetail),
              (int)r8(kGlobalBright), (int)(Bit16s)r16(kMatrix), (int)(Bit16s)r16(kMatrix + 2), (int)(Bit16s)r16(kMatrix + 4),
              g_brightHist[0], g_brightHist[1], g_brightHist[2], g_brightHist[3], g_brightHist[4], g_brightHist[5], g_brightHist[6], g_brightHist[7]);
}

// --- the 640 picture's stars ---------------------------------------------------------------------------------------------
struct Fine { Bit16s x, y; Bit8u colour, level; };     // (level: the game's own colour, less F8)
Fine g_fine[kMaxStars];
int g_fineN;
Bit8u g_fineKey = 0xF7;
int g_native = -1;
unsigned g_lastPlotted, g_lastHist[8], g_gathers, g_lastCandidates;
// research (WCNET_TIE640_STARDUMP=1, !t640stars=<name>): the frame shown, its stars as plotted and as the game's own
// scale would have put them (for pictures of the two side by side)
int g_dump = -1;
Fine g_shown[kMaxStars], g_coarse[kMaxStars], g_coarseNow[kMaxStars];
bool g_shownPlotted[kMaxStars];
int g_shownN, g_coarseN, g_coarseNowN;

}  // namespace

bool tie640_stars_check_on() {
    if (g_check < 0) {
        const char *env = getenv("WCNET_TIESTARS_CHECK");
        g_check = env && env[0] == '1' ? 1 : 0;
    }
    return g_check != 0;
}

void tie640_stars_watch(void (*watch)(Bit16u ip)) {
    if (!tie640_stars_check_on()) return;
    static const Bit16u ips[] = { kEntry, kPlot1, kPlot2, kPlot3, kErase, kExit };
    for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) watch(ips[i]);
}

// The check's places (the game's own picture only: tie640_hook has the routine's entry when the 640 picture is on).
bool tie640_stars_hook(Bit32u ip, Bit16u loadSeg) {
    if (!tie640_stars_check_on() || tie640_on()) return false;
    if (SegValue(cs) != (Bit16u)(loadSeg + kStarPara)) return false;
    g_load = loadSeg;
    switch (ip) {
    case kEntry:
        check_entry();
        return true;
    case kPlot1: case kPlot2: case kPlot3:
        if (g_armed && g_gotN < kMaxStars) {
            g_got[g_gotN].off = reg_bx;
            g_got[g_gotN++].colour = reg_al;
        }
        return true;
    case kErase:
        if (g_armed && g_gotEraseN < 1024) g_gotErase[g_gotEraseN++] = reg_di;
        return true;
    case kExit:
        check_exit();
        return true;
    }
    return false;
}

// WCNET_TIE640_STARS=0: the game's own routine in each quarter (a star four pixels of the 640 picture), as before.
bool tie640_stars_native() {
    if (g_native < 0) {
        const char *env = getenv("WCNET_TIE640_STARS");
        g_native = env && env[0] == '0' ? 0 : 1;
    }
    return g_native != 0;
}

// At the routine's entry (the tables are the frame's): every star's place in a picture of twice the scale whose
// centre is (centreX, centreY), kept for tie640_stars_plot.
void tie640_stars_gather(Bit16u loadSeg, int centreX, int centreY, int wide, int deep) {
    g_load = loadSeg;
    static Star stars[kMaxStars];
    const int n = walk(9, stars);
    static int gain = -1;
    if (gain < 0) {
        gain = getenv("WCNET_TIE640_STARGAIN") ? atoi(getenv("WCNET_TIE640_STARGAIN")) : kStarGainPercent;
        if (gain < 0 || gain > 1000) gain = kStarGainPercent;
    }
    Bit8u map[8];
    brighter(gain, map);
    g_fineKey = r8(kBackColour);
    g_fineN = 0;
    g_lastCandidates = (unsigned)n;
    for (int i = 0; i < n && g_fineN < kMaxStars; i++) {
        const int x = stars[i].x + centreX, y = stars[i].y + centreY;
        if (x < 0 || y < 0 || x >= wide || y >= deep) continue;
        g_fine[g_fineN].x = (Bit16s)x;
        g_fine[g_fineN].y = (Bit16s)y;
        g_fine[g_fineN].level = (Bit8u)(colour_of(stars[i].bright) & 7);
        g_fine[g_fineN++].colour = map[colour_of(stars[i].bright) & 7];
    }
    g_gathers++;
    if (g_dump < 0) g_dump = getenv("WCNET_TIE640_STARDUMP") ? 1 : 0;
    if (g_dump) {
        const int m = walk(8, stars);
        g_coarseNowN = 0;
        for (int i = 0; i < m; i++) {
            const int x = stars[i].x + centreX / 2, y = stars[i].y + centreY / 2;
            if (x < 0 || y < 0 || x >= wide / 2 || y >= deep / 2) continue;
            g_coarseNow[g_coarseNowN].x = (Bit16s)x;
            g_coarseNow[g_coarseNowN].y = (Bit16s)y;
            g_coarseNow[g_coarseNowN++].colour = colour_of(stars[i].bright);
        }
    }
}

// The routine left at its entry: the far return, and its lists left empty (nothing of its own is on the screen).
void tie640_stars_skip(Bit16u loadSeg) {
    g_load = loadSeg;
    const Bit16u now = r16(kListNow), last = r16(kListLast);
    if (now <= 0x3FE) mem_writew(csa(now), 0xFFFF);
    if (last <= 0x3FE) mem_writew(csa(last), 0xFFFF);
    mem_writew(dsa(kListCursor), now);
    const Bit16u ip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    SegSet16(cs, seg);
    reg_eip = ip;
}

// The stars gathered, into the finished scene: one pixel each, where the scene is still the background's (or a star).
void tie640_stars_plot(Bit8u *scene, int pitch, int wide, int deep) {
    g_lastPlotted = 0;
    memset(g_lastHist, 0, sizeof(g_lastHist));
    if (g_dump > 0) {
        memcpy(g_shown, g_fine, sizeof(g_shown));
        g_shownN = g_fineN;
        memcpy(g_coarse, g_coarseNow, sizeof(g_coarse));
        g_coarseN = g_coarseNowN;
    }
    for (int i = 0; i < g_fineN; i++) {
        const Fine &s = g_fine[i];
        if (s.x >= wide || s.y >= deep) continue;
        Bit8u *p = scene + s.y * pitch + s.x;
        if (g_dump > 0) g_shownPlotted[i] = *p >= g_fineKey;
        if (*p < g_fineKey) continue;
        *p = s.colour;
        g_lastPlotted++;
        g_lastHist[s.level]++;
    }
    g_fineN = 0;
}

void tie640_stars_say(double t, const std::string &name) {
    if (!name.empty() && g_dump > 0) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        const std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + name + ".txt";
        FILE *f = fopen(path.c_str(), "w");
        if (f) {
            for (int c = 0; c <= 0xFF; c++) fprintf(f, "P %02X %d %d %d\n", c, vga.dac.rgb[c].red, vga.dac.rgb[c].green, vga.dac.rgb[c].blue);
            for (int i = 0; i < g_shownN; i++) fprintf(f, "F %d %d %02X %d %02X\n", g_shown[i].x, g_shown[i].y, 0xF8 + g_shown[i].level, g_shownPlotted[i] ? 1 : 0, g_shown[i].colour);
            for (int i = 0; i < g_coarseN; i++) fprintf(f, "C %d %d %02X\n", g_coarse[i].x, g_coarse[i].y, g_coarse[i].colour);
            fclose(f);
        }
    }
    wclog(1, "key script %.1fs: TIE demo 640 stars: native %d, %u gathered frames; last frame %u inside the guard, %u plotted; the game's colours F8..FF %u %u %u %u %u %u %u %u; detail %d, brightness added %d",
          t, tie640_stars_native() ? 1 : 0, g_gathers, g_lastCandidates, g_lastPlotted, g_lastHist[0], g_lastHist[1], g_lastHist[2],
          g_lastHist[3], g_lastHist[4], g_lastHist[5], g_lastHist[6], g_lastHist[7], g_load ? (int)(Bit16s)r16(kDetail) : 0,
          g_load ? (int)r8(kGlobalBright) : 0);
}

}  // namespace wc
