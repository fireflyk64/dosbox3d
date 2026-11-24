/*
 *  The TIE Fighter demo's flight at 640x400, made fast (wcnet_tie640.h, wcnet_tie640.cpp; the facts in
 *  docs/tie640.md and the research notes perf.md).
 *
 *  The 640x400's scene is the game's own renderer run four times a frame, a quarter each time.  What is here cuts
 *  what a quarter costs, the picture kept as it is to the pixel:
 *    - the profile (WCNET_T640_PROF=1): the samples by quarter and by routine, the frames a second;
 *    - the check (WCNET_T640_CHECK=1): every quarter drawn twice in the same frame, once with what is here and once
 *      as the game does it alone, the two pictures (and the data segment) compared.
 *
 *  WCNET_T640_FAST: a mask of what is on (default: all).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "wcnet_tie640.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "regs.h"
#include "mem.h"
#include "pic.h"
#include "cpu.h"
#include "paging.h"
#include "callback.h"
#include "lazyflags.h"
#include "instructions.h"

namespace wc {
bool tie640_onepass_on();

namespace {

enum { kFlightPara = 0x0090, kRendPara = 0x1CCA, kDataPara = 0x421F };

Bit16u g_load = 0;
int g_pass = -1;            // the quarter being drawn (-1: none; 4: the plain game's scene)
bool g_reference = false;   // the check's second drawing of a quarter: the game alone
unsigned g_fast = 0xFFFFFFFFu;   // (read_env: kFastDefault unless WCNET_T640_FAST says)
int g_check = -1;
int g_prof = -1;
unsigned g_frames = 0;
std::map<Bit32u, unsigned> g_leftAt;    // ... the places the translation was left at (top bit: by a divide's fault)
std::map<Bit32u, unsigned> g_hitAt;     // the profile's: trips by place (image paragraph << 16 | IP)
void crowd_step();          // (a test aid, below)
unsigned g_culled[4], g_drawn[4];   // left out and drawn, by kind (craft, bolt, static, sprite): this profile period's
unsigned g_ours = 0;        // ... those that were this file's (the culls' places, the translated routines' entries)
unsigned g_rcRuns = 0;      // translated runs entered (this profile period)
unsigned g_hits = 0;        // our places' hook calls (the profile says how many a frame)

inline PhysPt dsa(Bit16u off) { return ((PhysPt)(Bit16u)(g_load + kDataPara) << 4) + off; }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline Bit32u r32(Bit16u off) { return mem_readd(dsa(off)); }
inline bool cs_is(Bit16u para) { return SegValue(cs) == (Bit16u)(g_load + para); }

void read_env() {
    if (g_check >= 0) return;
    const char *e = getenv("WCNET_T640_CHECK");
    g_check = e && e[0] && e[0] != '0' ? atoi(e) : 0;
    e = getenv("WCNET_T640_FAST");
    g_fast = e && e[0] ? (unsigned)strtoul(e, NULL, 0) : 9u | 1u | 2u | 32u;   // (kFastDefault, declared below)
    if (tie640_native_on()) g_fast &= ~32u;     // (the hand-written scanline pass: the translated code is not used)
    e = getenv("WCNET_T640_PROF");
    g_prof = e && e[0] && e[0] != '0' ? 1 : 0;
}

// --- the profile ---------------------------------------------------------------------------------------------------
// Sampled every ~1000 instructions (tie640_fast_tick): where the CPU is, by the quarter being drawn (0..3; 4 the
// plain game's scene; 5 the rest of the frame) and by routine (the renderer's entries known; 256 bytes elsewhere).
const Bit16u kRendFuncs[] = {
    0x0000, 0x044E, 0x06F3, 0x0702, 0x0776, 0x07A3, 0x07B6, 0x09BB, 0x09FC, 0x0A12, 0x0A3D, 0x0A74, 0x0A85, 0x0AAB, 0x0DBE,
    0x0E54, 0x0F38, 0x0F64, 0x1090, 0x1103, 0x1142, 0x11BE, 0x1625, 0x162D, 0x1639, 0x1656, 0x1680, 0x169E, 0x16BA, 0x179A,
    0x19BF, 0x19D0, 0x1AA5, 0x1ABA, 0x1B6E, 0x1B98, 0x1C64, 0x1CE0, 0x1CFC, 0x1D6C, 0x1DBD, 0x2071, 0x20D0, 0x225E, 0x2378,
    0x2412, 0x2672, 0x26BE, 0x2770, 0x277C, 0x28B0, 0x28B2, 0x291C, 0x2B79, 0x2BB0, 0x2C8D, 0x2CBC, 0x2CCC, 0x2D04, 0x2D3B,
    0x2D81, 0x2DF6, 0x2E49, 0x3062, 0x3475, 0x347C, 0x3489, 0x3540, 0x360F, 0x363A, 0x373E, 0x3773, 0x37B1, 0x381A, 0x382E,
    0x386B, 0x3A3E, 0x3CE2, 0x4049, 0x4328, 0x4466, 0x45AA, 0x4704, 0x4875, 0x4C1A, 0x4FF3, 0x528F, 0x5293, 0x53F3, 0x5839,
    0x584D, 0x5861, 0x58B1, 0x58C5, 0x58ED, 0x5915, 0x593D, 0x5965, 0x596D, 0x5AF5, 0x6034, 0x6053, 0x6055, 0x605B, 0x63DD,
    0x66C3, 0x69D0, 0x6AB7, 0x6B9E, 0x6C85, 0x6DDB, 0x6F49, 0x70B6, 0x7253, 0x73D8, 0x7545, 0x75FE, 0x76AB, 0x7776, 0x778C,
    0x779C, 0x77A2, 0x77FC, 0x7859, 0x789A, 0x7907, 0x794A, 0x7981, 0x7B6B, 0x7CD0, 0x7E04, 0x7E7B, 0x8206, 0x8510, 0x8524,
    0x8571, 0x85E9, 0x8A0A, 0x9724, 0x9B3B, 0x9C41, 0x9CA0, 0x9FA2, 0x9FE3, 0x9FF3, 0xA002, 0xA02A, 0xA0FC, 0xA132, 0xA36E,
    0xA62F, 0xA6EA, 0xA708, 0xA865, 0xAA59, 0xAC6D, 0xAE93, 0xB0A8, 0xB24C, 0xB41B, 0xB47D, 0xB5E1, 0xB697, 0xBBEC, 0xC98D,
    0xD1C3, 0xD1FA, 0xD5DA, 0xD5EB, 0xD5FD, 0xD772, 0xD790, 0xD7A2, 0xD8C8, 0xD930, 0xDBDB, 0xDF00, 0xDF30, 0xDFC8, 0xE0AF,
    0xE0CE, 0xE11D, 0xE12E, 0xE19C, 0xE227, 0xE297, 0xE2B8, 0xE2F8, 0xE35A, 0xE51C, 0xE56E, 0xE6B1, 0xE6E5, 0xE776, 0xF2D0,
    0xF30B, 0xF33C, 0xF375, 0xF3B6, 0xF401, 0xF41B, 0xF467, 0xF487, 0xF49A, 0xF4E7, 0xF59D, 0xF6F0, 0xF9CC, 0xFAEE, 0xFBDA,
};
enum { kRendFuncCount = sizeof(kRendFuncs) / sizeof(kRendFuncs[0]) };
std::map<Bit32u, unsigned> g_profAt[6];
unsigned g_profN[6];
double g_profNextMs = 0, g_profFromMs = 0;
unsigned g_profFrames0 = 0;

void prof_sample() {
    const Bit16u para = (Bit16u)(SegValue(cs) - g_load);
    Bit16u ip = (Bit16u)reg_eip;
    if (para == kRendPara) {
        const Bit16u *f = std::upper_bound(kRendFuncs, kRendFuncs + kRendFuncCount, ip);
        ip = f == kRendFuncs ? 0 : f[-1];
    } else {
        ip &= 0xFF00;
    }
    const int where = g_pass >= 0 && g_pass <= 4 ? g_pass : 5;
    g_profAt[where][((Bit32u)para << 16) | ip]++;
    g_profN[where]++;
    const double now = PIC_FullIndex();
    if (!g_profNextMs) { g_profNextMs = now + 5000; g_profFromMs = now; g_profFrames0 = g_frames; }
    if (now < g_profNextMs) return;
    unsigned total = 0;
    for (int w = 0; w < 6; w++) total += g_profN[w];
    const unsigned frames = g_frames - g_profFrames0;
    const double perFrame = frames ? 1.0 / frames : 0;
    wclog(1, "T640 profile: %u frames in %.1f s (%.1f fps), %u samples, %.0f a frame: quarters %.0f %.0f %.0f %.0f, plain scene %.0f, rest %.0f",
          frames, (now - g_profFromMs) / 1000, frames * 1000.0 / (now - g_profFromMs), total, total * perFrame, g_profN[0] * perFrame,
          g_profN[1] * perFrame, g_profN[2] * perFrame, g_profN[3] * perFrame, g_profN[4] * perFrame, g_profN[5] * perFrame);
    wclog(1, "T640 profile: hooks reached %.0f a frame (%.0f this file's, %.0f of them translated runs); left out a frame: craft %.1f of %.1f, bolts %.1f of %.1f, statics %.1f of %.1f, sprites %.1f of %.1f",
          g_hits * perFrame, g_ours * perFrame, g_rcRuns * perFrame, g_culled[0] * perFrame, (g_culled[0] + g_drawn[0]) * perFrame, g_culled[1] * perFrame, (g_culled[1] + g_drawn[1]) * perFrame,
          g_culled[2] * perFrame, (g_culled[2] + g_drawn[2]) * perFrame, g_culled[3] * perFrame, (g_culled[3] + g_drawn[3]) * perFrame);
    {
        std::vector<std::pair<unsigned, Bit32u> > top;
        for (std::map<Bit32u, unsigned>::const_iterator i = g_hitAt.begin(); i != g_hitAt.end(); ++i) top.push_back(std::make_pair(i->second, i->first));
        std::sort(top.rbegin(), top.rend());
        std::string text;
        for (size_t i = 0; i < top.size() && i < 24; i++) {
            char b[48];
            snprintf(b, sizeof(b), " %04X:%04X=%.1f", top[i].second >> 16, top[i].second & 0xFFFF, top[i].first * perFrame);
            text += b;
        }
        wclog(1, "T640 profile: hooks reached by place (a frame):%s", text.c_str());
        g_hitAt.clear();
        top.clear();
        text.clear();
        for (std::map<Bit32u, unsigned>::const_iterator i = g_leftAt.begin(); i != g_leftAt.end(); ++i) top.push_back(std::make_pair(i->second, i->first));
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 16; i++) {
            char b[48];
            snprintf(b, sizeof(b), " %04X:%04X%s=%.1f", (top[i].second >> 16) & 0x7FFF, top[i].second & 0xFFFF, top[i].second >> 31 ? "(divide)" : "", top[i].first * perFrame);
            text += b;
        }
        wclog(1, "T640 profile: the translation left at (a frame):%s", text.c_str());
        g_leftAt.clear();
    }
    g_hits = g_ours = g_rcRuns = 0;
    memset(g_culled, 0, sizeof(g_culled));
    memset(g_drawn, 0, sizeof(g_drawn));
    for (int w = 0; w < 6; w++) {
        if (!g_profN[w]) continue;
        std::vector<std::pair<unsigned, Bit32u> > top;
        for (std::map<Bit32u, unsigned>::const_iterator i = g_profAt[w].begin(); i != g_profAt[w].end(); ++i) top.push_back(std::make_pair(i->second, i->first));
        std::sort(top.rbegin(), top.rend());
        std::string text;
        for (size_t i = 0; i < top.size() && i < 40; i++) {
            char b[48];
            snprintf(b, sizeof(b), " %04X:%04X=%.1f", top[i].second >> 16, top[i].second & 0xFFFF, top[i].first * perFrame);
            text += b;
        }
        wclog(1, "T640 profile %d (samples a frame):%s", w, text.c_str());
        g_profAt[w].clear();
        g_profN[w] = 0;
    }
    g_profNextMs = now + 5000;
    g_profFromMs = now;
    g_profFrames0 = g_frames;
}


// --- what is on ----------------------------------------------------------------------------------------------------
enum {
    F_CULL_CRAFT = 1,       // a craft that cannot touch the quarter is not drawn in it
    F_CULL_BOLT = 2,        // ... a bolt
    F_CULL_STATIC = 4,      // ... a static object
    F_CULL_SPRITE = 8,      // ... a sprite or a backdrop (the scaler's call stepped over)
    F_MASK = 16,            // the quarter's own mask: the cockpit's, at the finer grid (nothing drawn behind the cockpit)
    F_NATIVE = 32,          // the scanline pass and the per-quarter clears run as translated code (wcnet_tie640_rc.inc)
};
// The mask is off unless asked for: a shaded span's dither runs on from its first pixel drawn, so a span that begins
// at the cockpit's edge instead of behind it comes out a shade's step different here and there (measured: a few
// pixels a quarter, one palette step).
const unsigned kFastDefault = F_CULL_CRAFT | F_CULL_BOLT | F_CULL_SPRITE | F_NATIVE;
inline bool fast(unsigned bit) { return (g_fast & bit) && !g_reference && g_pass >= 0 && g_pass < 4; }

// --- a quarter's bounds --------------------------------------------------------------------------------------------
// The projection (transfm2_getscreenx, getscreeny, the scale doubled by tie640's hooks): from the picture's centre
// (320, 150 of the 640x378 in the cockpit's view), X' = 512 x / z and Y' = 512 * (0xE8BA / 65536) * y / z, each cut
// towards the centre.  The quarters meet there at X' = 0 (columns 0..319 | 320..639) and between Y' = 38 and 39.
// Everything of an object is within its bound s of its eye position on each axis, so all of it is left of the
// column X' = B (by kMargin pixels) when  x - b z + s (1 + |b|) <= 0  with b = (B - kMargin) / 512, a test that
// holds for the points behind the eye too (the near plane's cut keeps a polygon on its side of a plane through
// the eye).  The same on each side and for the rows.
const double kMargin = 4;
const double kScaleX = 512.0, kScaleY = 512.0 * 0xE8BA / 65536.0;

// The object in tie_updatescreen's loop: its eye position (DS:C460, CD68, CD72) and its bound (DS:CE46), against
// the line this quarter shares with its neighbour -- in the quarter's own terms: its window is columns 0..wide-1 and
// rows 0..deep-1 (DS:48C5, 48D1; the look-around views' window is smaller) and its centre is where tie640's hook
// has put it (DS:48C9; DS:48D5 + DS:289E).  Only the shared lines are tested, never the picture's outer sides: an
// object the game itself would draw is then drawn in at least one quarter (its direction vectors are worked out
// on the way, as in the game alone).
bool object_off_quarter() {
    const double x = (Bit32s)r32(0xC460), y = (Bit32s)r32(0xCD68), z = (Bit32s)r32(0xCD72), s = r16(0xCE46);
    const double wide = r16(0x48C5), deep = r16(0x48D1);
    const double cx = (Bit16s)r16(0x48C9), cy = (double)(Bit16s)r16(0x48D5) + (Bit32s)r32(0x289E);
    double b;
    if (!(g_pass & 1)) {        // the left quarter: all of it right of the last column?
        b = (wide - cx + kMargin) / kScaleX;
        if (x - b * z - s * (1 + (b < 0 ? -b : b)) >= 0) return true;
    } else {                    // the right quarter: all of it left of the first column?
        b = (0 - cx - kMargin) / kScaleX;
        if (x - b * z + s * (1 + (b < 0 ? -b : b)) <= 0) return true;
    }
    if (!(g_pass & 2)) {        // the upper quarter: all below the last row?
        b = (deep - cy + kMargin) / kScaleY;
        if (y - b * z - s * (1 + (b < 0 ? -b : b)) >= 0) return true;
    } else {                    // the lower quarter: all above the first row?
        b = (0 - cy - kMargin) / kScaleY;
        if (y - b * z + s * (1 + (b < 0 ? -b : b)) <= 0) return true;
    }
    return false;
}

// A craft's parts (draw_drawcraft, 168C:0260): each of the species' N parts (the species' record +0x474; the parts'
// records 12 bytes each at DS:[+0x471]) has a state byte in the craft's data (+0x15C + part) picking a word of the
// part's table (the record's first word): below 0x8000 a mesh (drawn inside the craft's bound), and then a chain
// of more records (the record's +2: next, count) whose word, 0x8000..0xFEFF, is a sprite queued (a part burning,
// lightning) -- of its own size, not the craft's.  A craft with any such sprite to queue is never left out.
bool craft_has_sprites(unsigned object) {
    const Bit16u craft = r16(0xBF8C);
    const unsigned species = mem_readb(dsa((Bit16u)(0x67CA + 0x58 * object + 4)));
    const PhysPt sp = ((PhysPt)r16(0x58D6) << 4) + 0x14 * species;
    const Bit16u parts = mem_readw(sp + 0x471);
    const unsigned n = mem_readb(sp + 0x474);
    if (!craft || n > 48) return true;
    for (unsigned c = 0; c < n; c++) {
        const Bit16u rec = (Bit16u)(parts + 12 * c);
        const Bit16u op = r16((Bit16u)(r16(rec) + 2 * mem_readb(dsa((Bit16u)(craft + 0x15C + c)))));
        if (op >= 0x8000) continue;
        unsigned next = mem_readb(dsa((Bit16u)(rec + 2))), count = mem_readb(dsa((Bit16u)(rec + 3)));
        while (count--) {
            const Bit16u r2 = (Bit16u)(parts + 12 * next);
            const Bit16u op2 = r16((Bit16u)(r16(r2) + 2 * mem_readb(dsa((Bit16u)(craft + 0x15C + next)))));
            if (op2 >= 0x8000 && op2 < 0xFF00) return true;
            next = mem_readb(dsa((Bit16u)(r2 + 2)));
            if (!next) break;
        }
    }
    return false;
}

// rotscale_rotatescaleimage(x, y, scale, far image), 1CCA:A02A, at its entry: the sprite scaler, for a backdrop
// (168C:0718) or a sprite of the frame's list (1768:03A4).  The picture's four corners are its two anchors' columns
// (the image's bytes 0 and 2, signed, plus DS:435C) and rows (bytes 1 and 3, plus DS:435E), scaled (0x100 = 1; the
// rows by DS:50BC / 256 too) and turned, from (x, y) -- y counted from the window's last row up.  When no corner can
// be inside the window (two pixels wider each side: the scan's own margin), the scaler draws nothing and adds no
// object: its call is stepped over.  What it would have left behind is its own scratch (DS:434A.., 5112.., the box
// DS:48B2..48B9 that only it reads).
bool sprite_off_quarter() {
    const PhysPt sp = SegPhys(ss) + (Bit16u)reg_sp;
    const int x = (Bit16s)mem_readw(sp + 4), y = (Bit16s)mem_readw(sp + 6);
    const unsigned scale = mem_readw(sp + 8);
    const PhysPt image = ((PhysPt)mem_readw(sp + 12) << 4) + mem_readw(sp + 10);
    int ax = 0, ay = 0;
    for (int k = 0; k < 4; k++) {
        const int v = (Bit8s)mem_readb(image + k), a = v < 0 ? -v : v;
        if (k & 1) ay = std::max(ay, a); else ax = std::max(ax, a);
    }
    const int ox = (Bit16s)r16(0x435C), oy = (Bit16s)r16(0x435E);
    ax += ox < 0 ? -ox : ox;
    ay += oy < 0 ? -oy : oy;
    const unsigned rows = std::max<unsigned>(256, r16(0x50BC));
    // (each scaled length rounded up, a pixel more for each of the scaler's roundings; turned by any angle the corner
    // is within the two lengths' sum of the centre on each axis)
    const double r = (ax * (double)scale / 256 + 2) + (ay * (double)rows / 256 * scale / 256 + 3) + 4;
    const int wide = r16(0x48C5), deep = r16(0x48D1);
    const double yt = (double)r16(0x48D3) - y;
    return x + r < 0 || x - r >= wide || yt + r < 0 || yt - r >= deep;
}

// --- the quarter's own mask ----------------------------------------------------------------------------------------
// The game's mask is the cockpit's window in the 320-wide picture; tie640 opens it whole for the quarters and shows
// the scene only where the window (its edge cut at the finer grid) is.  Here each quarter gets the window's part at
// the finer grid as its mask, so the scanline pass leaves out what the cockpit hides.  (A row: its first byte's top
// bit set when it begins closed, then the runs' lengths, closed and open in turn, 0 then a byte for 256 and more.)
Bit8u g_fine[640 * 400];    // the fine picture's pixels the window shows (1)
bool g_fineOn = false;      // this quarter's mask is the fine one (the check compares the open pixels only)

}  // namespace

// (the one pass: the scaler's call for a quarter the sprite cannot touch is not made)
bool tie640_fast_sprite_off() { return sprite_off_quarter(); }

void tie640_fast_mask(int pass, PhysPt maskAt, const Bit8u *kept, int keptBytes, const Bit8u *window, const Bit8u *from, int wide, int deep, int corner) {
    static const int kDx[5] = { 0, 0, 0, -1, 1 }, kDy[5] = { 0, -1, 1, 0, 0 };
    if (pass < 0 || pass > 3) return;
    g_pass = pass;
    if (g_reference) return;    // (the check's second drawing: the open mask; g_fineOn stays the first's, for the comparison)
    g_fineOn = false;
    if (!fast(F_MASK) || wide > 320 || deep > 200) return;
    // the game's own mask's length: ours is to fit in it
    int at = 0;
    for (int y = 0; y < deep; y++) {
        at++;
        for (int x = 0; x < wide;) {
            if (at + 2 >= keptBytes) return;
            int n = kept[at++];
            if (!n) n = 256 + kept[at++];
            x += n;
        }
    }
    const int room = at;
    static Bit8u rows[4096];
    int len = 0;
    // (the window's corner in the game's picture is twice that in the fine picture; the quarter's follows)
    const int X0 = 2 * (corner % 320) + (pass & 1) * wide, Y0 = 2 * (corner / 320) + (pass >> 1) * deep;
    if (X0 + wide > 640 || Y0 + deep > 400) return;
    for (int r = 0; r < deep; r++) {
        const int Y = Y0 + r, y = Y >> 1;
        Bit8u *fine = g_fine + Y * 640 + X0;
        for (int c = 0; c < wide; c++) {
            const int X = X0 + c, x = X >> 1, f = from[Y * 640 + X];
            const int s = f ? (y + kDy[f]) * 320 + x + kDx[f] : y * 320 + x;
            fine[c] = Y < 400 && s >= 0 && s < 320 * 200 && window[s] ? 1 : 0;
        }
        if (len + 2 * wide + 4 > (int)sizeof(rows)) return;
        rows[len++] = fine[0] ? 0x01 : 0xFF;
        for (int c = 0; c < wide;) {
            int n = 1;
            while (c + n < wide && fine[c + n] == fine[c]) n++;
            if (n >= 256) { rows[len++] = 0; rows[len++] = (Bit8u)(n - 256); }
            else rows[len++] = (Bit8u)n;
            c += n;
        }
    }
    if (len > room) return;     // (no room: the open mask stays)
    for (int i = 0; i < len; i++) mem_writeb(maskAt + (PhysPt)i, rows[i]);
    g_fineOn = true;
}

// --- the renderer's routines as translated code --------------------------------------------------------------------
// scripts/tie640-recompile.py makes wcnet_tie640_rc.inc from the game's code: the scanline pass (xtrans2_drawxtrans,
// 1CCA:85E9, with the run output and the nearest-object search it calls), the background buffer's clear, the run
// table's and the edge lists' resets: each instruction the statements DOSBox's own core runs for it, on the same
// registers, flags and memory, so the routine leaves everything as the interpreter would -- without the
// interpreter's fetch and decode, and at no cost in the emulated CPU's cycles.  One hook's call a routine's call
// (and one more for each far call that leaves the translated set and comes back: the polygons' depth sort).
namespace {
#define RC_LR(x) (x)
#define RC_SR(x, v) (x) = (v)
#undef EXCEPTION
#define EXCEPTION(n) goto rc_fault
// A software interrupt has just been taken (CPU_Interrupt: the CPU is at its handler).  When the handler is one of the
// emulator's own callbacks with nothing around it (the stub CALLBACK_Setup writes for CB_IRET: FE 38 <number>, then
// iret -- the EMS driver's int 67h is one), the callback is called and the iret done here, as the core would in its
// next two instructions; true then.
bool rc_callback() {
    const PhysPt at = SegPhys(cs) + reg_eip;
    if (cpu.pmode || mem_readb(at) != 0xFE || mem_readb(at + 1) != 0x38 || mem_readb(at + 4) != 0xCF) return false;
    const Bit16u cb = mem_readw(at + 2);
    if (cb >= CB_MAX) return false;
    FillFlags();
    reg_eip += 4;
    if (CallBack_Handlers[cb]()) return false;      // (a callback that wants the machine stopped: none here)
    CPU_IRET(false, reg_eip + 1);
    return true;
}

bool g_rcInside = false;    // a translated run is going on (the hooks it calls itself start no other)
Bit32u g_rcSkipAt = 0;      // the place (image paragraph << 16 | IP) a hook has just sent the CPU to: no hook's call there
// A hooked place passed by the translation: the hook called from here (no trip out of the CPU's loop).  A hook that
// moves the CPU ends the function (-2): the run goes on from where it points.
#define RC_HOOK(a) { \
    const Bit32u key_ = ((Bit32u)(Bit16u)(SegValue(cs) - load) << 16) | (a); \
    if (g_rcSkipAt == key_) g_rcSkipAt = 0; \
    else { \
        const Bit16u cs_ = SegValue(cs); \
        reg_eip = (a); \
        tie640_hook(a); \
        if (reg_eip != (Bit32u)(a) || SegValue(cs) != cs_) { ip_ = (Bit16u)reg_eip; return -2; } \
    } }
#ifdef WCNET_T640_RC
#define RC_FN(k) rc_fn_##k
#define RC_TABLE rc_table
#define RC_T(a)
#include "wcnet_tie640_rc.inc"
#undef RC_FN
#undef RC_TABLE
#undef RC_T
#else
// (The translation is not part of the build: its file, some 3 MB made by scripts/tie640-recompile.py from the game's
// own code, is not kept.  It was the first way to make the quarters fast and the proof the hand-written renderer was
// measured against; to have it again: make the file and build with -DWCNET_T640_RC.  Without it: nothing translated.)
static const Bit32u kRcHash = 0;
static const Bit16u kRcRuns[1][3] = { { 0xFFFF, 0, 0 } };
static const Bit32u kRcSkip[1] = { 0 };
static const Bit32u kRcEntries[1] = { 0xFFFFFFFFu }, kRcNear[1] = { 0xFFFFFFFFu }, kRcReturns[1] = { 0xFFFFFFFFu };
static const Bit16u kRcHookIps[1] = { 0xFFFF }, kRcCode[1] = { 0xFFFF };
static int rc_find(Bit16u, Bit16u) { return -1; }
typedef int (*RcFn)(Bit16u &, const Bit16u);
static int rc_none(Bit16u &, const Bit16u) { return -1; }
static const RcFn rc_table[] = { rc_none };
#endif

// The translation's own test (WCNET_T640_TRACE=<n>): the n-th call of the scanline pass is run by the interpreter
// first, the registers and flags before each of its instructions kept; then everything is put back as it was and the
// translation runs the same call, compared before each instruction with what the interpreter had there; at the end
// the conventional memory is compared too.  The first difference names the instruction before it.
struct Step { Bit16u ip, flags, es, ds, fs, ss; Bit32u r[8]; };
std::vector<Step> g_trace;
size_t g_traceAt = 0;
int g_tracePhase = 0;       // 1 the interpreter's run recorded, 2 the translation's compared
Bit16u g_tracePara = 0x1CCA;
int g_traceWanted = -1;
int g_traceEvery = 0;        // WCNET_T640_TRACE_EVERY=<k>: and every k-th call after it
unsigned g_traceDone = 0, g_traceFailed = 0, g_traceSteps = 0;
Bit16u g_traceEntry = 0x85E9;   // WCNET_T640_TRACE_AT=<entry, hex>: another of the translated entries
unsigned g_traceCalls = 0, g_traceBad = 0;
Bit16u g_traceRetIp, g_traceRetCs, g_traceSp, g_traceLastIp = 0;
std::vector<Bit8u> g_traceMem, g_traceMemAfter;
CPU_Regs g_traceRegs;
Segments g_traceSegs;
Bitu g_traceFlags;

inline void step_now(Step &s, Bit16u ip) {
    s.ip = ip;
    s.flags = (Bit16u)(FillFlags() & 0x8D5);    // (carry, parity, aux, zero, sign, overflow)
    s.es = SegValue(es); s.ds = SegValue(ds); s.fs = SegValue(fs); s.ss = SegValue(ss);
    s.r[0] = reg_eax; s.r[1] = reg_ecx; s.r[2] = reg_edx; s.r[3] = reg_ebx; s.r[4] = reg_esp; s.r[5] = reg_ebp; s.r[6] = reg_esi; s.r[7] = reg_edi;
}

// Before every instruction of the translation's traced run: true at the call's return (the run ends there, as the
// interpreter's record does).
bool rc_trace(Bit16u ip) {
    if (SegValue(cs) == g_traceRetCs && ip == g_traceRetIp && reg_sp == g_traceSp) return true;
    if (g_traceBad > 8) return false;
    Step now;
    step_now(now, ip);
    if (g_traceAt >= g_trace.size()) {
        if (!g_traceBad++) wclog(1, "T640 trace: the translation runs on past the interpreter's %u instructions, at %04X after %04X", (unsigned)g_trace.size(), ip, g_traceLastIp);
        return false;
    }
    const Step &was = g_trace[g_traceAt++];
    // (the undefined flags of a shift or a multiply are DOSBox's own in both, the same macros: compared too)
    if (memcmp(&now, &was, sizeof(Step))) {
        g_traceBad++;
        wclog(1, "T640 trace: step %u differs after %04X: at %04X (the interpreter %04X) flags %04X/%04X es %04X/%04X ds %04X/%04X fs %04X/%04X eax %08X/%08X ecx %08X/%08X edx %08X/%08X ebx %08X/%08X esp %08X/%08X ebp %08X/%08X esi %08X/%08X edi %08X/%08X",
              (unsigned)g_traceAt - 1, g_traceLastIp, ip, was.ip, now.flags, was.flags, now.es, was.es, now.ds, was.ds, now.fs, was.fs, now.r[0], was.r[0], now.r[1], was.r[1],
              now.r[2], was.r[2], now.r[3], was.r[3], now.r[4], was.r[4], now.r[5], was.r[5], now.r[6], was.r[6], now.r[7], was.r[7]);
    }
    g_traceLastIp = ip;
    return false;
}

// (the translation a second time, with the comparison before every instruction: not in the browser's build, where
// the test is not run and the code would only be weight)
#if defined(__EMSCRIPTEN__) || !defined(WCNET_T640_RC)
#define rc_table_traced rc_table
#else
#define RC_BODY_ONLY
#define RC_FN(k) rc_fnt_##k
#define RC_TABLE rc_table_traced
#define RC_T(a) if (rc_trace(a)) { ip_ = (a); return -4; }
#include "wcnet_tie640_rc.inc"
#undef RC_FN
#undef RC_TABLE
#undef RC_T
#endif
#undef EXCEPTION

int g_rcKnown = -1;         // the code in memory is the code translated (checked once a flight)
Bit16u g_rcLoad = 0;
std::vector<Bit16u> g_hooked;   // tie640's own watched places (tie640_fast_hooked)

template <class T, size_t N> inline bool in_list(const T (&list)[N], T v) { return std::find(list, list + N, v) != list + N; }
inline Bit32u place_now(Bit16u ip) { return ((Bit32u)(Bit16u)(SegValue(cs) - g_load) << 16) | ip; }

bool rc_known() {
    if (g_rcKnown >= 0 && g_rcLoad == g_load) return g_rcKnown == 1;
    g_rcLoad = g_load;
    Bit32u h = 0x811C9DC5u;
    for (size_t r = 0; r < sizeof(kRcRuns) / sizeof(kRcRuns[0]); r++) {
        const PhysPt base = (PhysPt)(Bit16u)(g_load + kRcRuns[r][0]) << 4;
        for (unsigned k = 0; k < kRcRuns[r][2]; k++) {
            const Bit16u at = (Bit16u)(kRcRuns[r][1] + k);
            Bit8u b = mem_readb(base + at);
            if (std::binary_search(kRcSkip, kRcSkip + sizeof(kRcSkip) / sizeof(kRcSkip[0]), ((Bit32u)kRcRuns[r][0] << 16) | at)) b = 0;    // (relocated, or tie640's patch)
            h = (h ^ b) * 0x01000193u;
        }
    }
    g_rcKnown = h == kRcHash ? 1 : 0;
    // a place watched that the translation passes without calling its hook would never be seen
    for (size_t i = 0; i < g_hooked.size() && g_rcKnown == 1; i++) {
        if (std::binary_search(kRcCode, kRcCode + sizeof(kRcCode) / sizeof(kRcCode[0]), g_hooked[i]) && !in_list(kRcHookIps, g_hooked[i])) {
            wclog(0, "T640: the place %04X is hooked but scripts/tie640-recompile.py's HOOK_IPS does not have it", g_hooked[i]);
            g_rcKnown = 0;
        }
    }
    wclog(1, "T640: the renderer's routines %s", g_rcKnown ? "are the ones translated: run as native code" : "are NOT the ones translated: left to the interpreter");
    return g_rcKnown == 1;
}

inline bool rc_allowed() {
#ifndef WCNET_T640_RC
    return false;
#endif
    // (16-bit code and stack, real or virtual-86 addressing: what the translation assumes)
    return (g_fast & F_NATIVE) && !g_reference && !cpu.code.big && cpu.stack.mask == 0xFFFF && (!cpu.pmode || (reg_flags & FLAG_VM));
}

// The translation run from CS:ip until control leaves it for good.
void rc_run(Bit16u ip, bool traced) {
    g_rcInside = true;
    g_rcRuns++;
    int k = rc_find((Bit16u)(SegValue(cs) - g_load), ip);
    for (;;) {
        while (k >= 0) k = (traced ? rc_table_traced : rc_table)[k](ip, g_load);
        reg_eip = ip;
        if (k == -4) break;     // (the traced run's end: the call has returned)
        if (k == -3) {      // (a divide that faults: the interpreter's, and the game's handler's)
            if (g_prof) g_leftAt[place_now(ip) | 0x80000000u]++;
            break;
        }
        if (k == -1 && g_prof) g_leftAt[place_now(ip)]++;
        bool moved = k == -2;       // (a hook the translation called has moved the CPU)
        if (!moved) {
            // out of the translation at CS:ip.  No hook's call comes for a place a hook's call leaves the CPU at (and
            // this run is inside one): if the place is hooked, its hook is called here.
            const Bit32u key = place_now(ip);
            if (g_rcSkipAt == key) { g_rcSkipAt = 0; break; }
            if (!in_list(kRcHookIps, ip)) break;
            const Bit16u cs0 = SegValue(cs);
            tie640_hook(ip);
            if (reg_eip == ip && SegValue(cs) == cs0) break;
            moved = true;
        }
        // where a hook has sent the CPU: no hook's call there; on in the translation if the place is its own
        ip = (Bit16u)reg_eip;
        g_rcSkipAt = place_now(ip);
        k = rc_allowed() ? rc_find((Bit16u)(SegValue(cs) - g_load), ip) : -1;
        if (k < 0) { g_rcSkipAt = 0; break; }
    }
    g_rcInside = false;
}

// One of the translated set's entries (or a place control comes back to it) is where the CPU is: run from there.
bool trace_begin(Bit32u place);
bool rc_enter(Bit16u ip) {
    const Bit32u place = place_now(ip);
    if (place == 0x00900B1Au) return false;     // (the scene section: entered from tie640's hook there, tie640_fast_run_scene)
    if (!in_list(kRcEntries, place) && !in_list(kRcReturns, place)) return false;
    if (g_tracePhase || trace_begin(place)) return false;
    if (!rc_allowed() || !rc_known()) return false;
    rc_run(ip, false);
    return true;
}

bool rc_is_code(Bit16u para, Bit16u ip) {
    for (size_t r = 0; r < sizeof(kRcRuns) / sizeof(kRcRuns[0]); r++)
        if (kRcRuns[r][0] == para && ip >= kRcRuns[r][1] && ip < kRcRuns[r][1] + kRcRuns[r][2]) return true;
    return false;
}

// At the scanline pass's entry: the n-th call starts the test (the interpreter runs it: false).
bool trace_begin(Bit32u place) {
    const Bit16u ip = (Bit16u)place;
    if (g_traceWanted < 0) {
        const char *e = getenv("WCNET_T640_TRACE");
        g_traceWanted = e && e[0] ? atoi(e) : 0;
        e = getenv("WCNET_T640_TRACE_AT");      // (<entry> or <paragraph>:<entry>, hex)
        if (e && e[0]) {
            const char *c = strchr(e, ':');
            if (c) g_tracePara = (Bit16u)strtoul(e, NULL, 16);
            g_traceEntry = (Bit16u)strtoul(c ? c + 1 : e, NULL, 16);
        }
        e = getenv("WCNET_T640_TRACE_EVERY");
        if (e && e[0]) g_traceEvery = atoi(e);
    }
    if (!g_traceWanted || g_tracePhase || place != (((Bit32u)g_tracePara << 16) | g_traceEntry) || ++g_traceCalls != (unsigned)g_traceWanted) return false;
    FillFlags();
    g_traceRegs = cpu_regs;
    g_traceSegs = Segs;
    g_traceMem.assign(MemBase, MemBase + 0xA0000);
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const bool nearCall = in_list(kRcNear, place);
    g_traceRetIp = mem_readw(sp);
    g_traceRetCs = nearCall ? SegValue(cs) : mem_readw(sp + 2);
    g_traceSp = (Bit16u)(reg_sp + (nearCall ? 2 : 4));
    g_trace.clear();
    g_tracePhase = 1;
    wc_net_countdown = 0;       // (the tick before every instruction from here)
    {
        Step s;
        step_now(s, ip);
        g_trace.push_back(s);   // (this instruction: the tick has passed for it)
    }
    if (!g_traceEvery) wclog(1, "T640 trace: call %u of %04X: the interpreter's run recorded", g_traceCalls, ip);
    return true;
}

// Every instruction while the test runs (tie640_fast_tick).
void trace_tick() {
    wc_net_countdown = 0;       // (the tick before every instruction)
    const Bit16u ip = (Bit16u)reg_eip;
    if (SegValue(cs) == g_traceRetCs && ip == g_traceRetIp && reg_sp == g_traceSp) {
        // the interpreter is back from the call: its memory kept, everything put back, the translation's turn
        g_traceMemAfter.assign(MemBase, MemBase + 0xA0000);
        const CPU_Regs after = cpu_regs;
        memcpy(MemBase, &g_traceMem[0], 0xA0000);
        cpu_regs = g_traceRegs;
        Segs = g_traceSegs;
        lflags.type = t_UNKNOWN;
        g_tracePhase = 2;
        g_traceAt = 0;
        g_traceBad = 0;
        g_traceLastIp = 0;
        const size_t steps = g_trace.size();
        rc_run(g_traceEntry, true);
        unsigned differ = 0, first = 0;
        // (the BIOS's data and the stack below the call's frame hold the interrupts taken meanwhile: left out)
        const Bit32u stackLo = (Bit32u)SegPhys(ss) + (Bit16u)(g_traceSp - 0x1000), stackHi = (Bit32u)SegPhys(ss) + g_traceSp;
        // (the renderer's own memory -- the data segment, the run tables' segment, the background buffer's, the
        // edge pool's FS -- must be the same to the byte; elsewhere the sound driver's interrupts write meanwhile)
        const Bit32u own[4] = { (Bit32u)dsa(0), (Bit32u)r16(0xD8A6) << 4, (Bit32u)r16(0x48BD) << 4, (Bit32u)(Bit16u)(0x3598 + g_load) << 4 };
        unsigned ownDiffer = 0;
        for (unsigned i = 0x600; i < 0xA0000; i++) {
            if (MemBase[i] == g_traceMemAfter[i] || (i >= stackLo && i < stackHi)) continue;
            if (!differ++) first = i;
            // (DS:C464, the timer interrupt's tick count, moves meanwhile too)
            if ((i >= own[0] + 0xC464 && i < own[0] + 0xC468) || (i >= own[0] + 0xCD6C && i < own[0] + 0xCD6E)) continue;   // (and DS:CD6C: seen moving with nothing of ours on)
            for (int k = 0; k < 4; k++) if (i >= own[k] && i < own[k] + 0x10000) {
                if (!ownDiffer++) wclog(1, "T640 trace: the renderer's memory differs at segment %04X offset %04X", own[k] >> 4, i - own[k]);
                break;
            }
        }
        std::string where;
        for (unsigned i = 0x600, n = 0; i < 0xA0000 && n < 12; i++) {
            if (MemBase[i] == g_traceMemAfter[i] || (i >= stackLo && i < stackHi)) continue;
            unsigned j = i;
            while (j + 1 < 0xA0000 && MemBase[j + 1] != g_traceMemAfter[j + 1]) j++;
            char b[32];
            snprintf(b, sizeof(b), " %05X..%05X", i, j);
            where += b;
            i = j;
            n++;
        }
        if (differ && !g_traceEvery) wclog(1, "T640 trace: memory differs at%s (the data segment is at %05X)", where.c_str(), (unsigned)dsa(0));
        unsigned stackDiffer = 0;
        for (unsigned i = stackLo; i < stackHi; i++) if (MemBase[i] != g_traceMemAfter[i]) stackDiffer++;
        if (stackDiffer && !g_traceEvery) wclog(1, "T640 trace: %u bytes of the stack below the call differ (interrupts, or the routine's own frame)", stackDiffer);
        const bool back = SegValue(cs) == g_traceRetCs && (Bit16u)reg_eip == g_traceRetIp && reg_sp == g_traceSp;
        const bool regsDiffer = memcmp(&after, &cpu_regs, sizeof(after)) != 0;
        const bool failed = !back || g_traceBad || regsDiffer || steps != g_traceAt || ownDiffer;
        g_traceDone++;
        g_traceSteps += (unsigned)steps;
        if (failed) g_traceFailed++;
        if (!g_traceEvery || failed)
            wclog(1, "T640 trace: call %u of %04X: the interpreter ran %u instructions of the set; the translation %u, %s; %u steps differed; %u bytes of memory differ (%u in the renderer's own segments; the first at %05X); registers %s",
                  g_traceCalls, g_traceEntry, (unsigned)steps, (unsigned)g_traceAt, back ? "back at the caller" : "LEFT the set elsewhere", g_traceBad, differ, ownDiffer, first,
                  regsDiffer ? "DIFFER" : "the same");
        if (g_traceEvery) {
            g_traceWanted += g_traceEvery;
            if (g_traceDone % 50 == 0) wclog(1, "T640 trace: %u calls of %04X run both ways (%u instructions), %u differed", g_traceDone, g_traceEntry, g_traceSteps, g_traceFailed);
        }
        g_tracePhase = 0;
        g_trace.clear();
        return;
    }
    if (rc_is_code((Bit16u)(SegValue(cs) - g_load), ip)) {
        // (a rep the interpreter comes back to, its cycles run out part way: one instruction)
        if (!g_trace.empty() && g_trace.back().ip == ip && mem_readb(SegPhys(cs) + ip) == 0xF3) return;
        Step s;
        step_now(s, ip);
        g_trace.push_back(s);
    }
}

}  // namespace

// --- the check -----------------------------------------------------------------------------------------------------
// WCNET_T640_CHECK=1: a quarter is drawn twice, first with what is on here, then (g_reference) as the game alone
// draws it; the quarter's pixels the window shows and the data segment after each are compared.
namespace {
Bit8u g_checkPic[320 * 200];
Bit8u g_checkDs[0x10000];
std::set<Bit16u> g_dsDiff;
unsigned g_checkQuarters = 0, g_checkBad = 0, g_checkPixels = 0, g_checkDumps = 0;
double g_checkNextMs = 0;
}

bool tie640_fast_again(int pass, const Bit8u *draw, int wide, int deep, int corner) {
    read_env();
    if (g_check != 1 || pass < 0 || pass > 3 || deep > 200 || wide > 320) return false;
    tie640_native_keep(true);
    const int X0 = 2 * (corner % 320) + (pass & 1) * wide, Y0 = 2 * (corner / 320) + (pass >> 1) * deep;
    if (X0 + wide > 640 || Y0 + deep > 400) return false;
    const PhysPt ds = dsa(0);
    if (!g_reference) {
        for (int r = 0; r < deep; r++) memcpy(g_checkPic + r * 320, draw + (Y0 + r) * 640 + X0, wide);
        for (unsigned i = 0; i < 0x10000; i++) g_checkDs[i] = mem_readb(ds + i);
        g_reference = true;
        return true;
    }
    g_reference = false;
    unsigned bad = 0;
    int fx = -1, fy = -1;
    for (int r = 0; r < deep; r++) {
        const Bit8u *a = g_checkPic + r * 320, *b = draw + (Y0 + r) * 640 + X0, *fine = g_fine + (Y0 + r) * 640 + X0;
        for (int c = 0; c < wide; c++) {
            if (a[c] == b[c] || (g_fineOn && !fine[c])) continue;
            if (!bad) { fx = c; fy = r; }
            bad++;
        }
    }
    g_checkQuarters++;
    if (bad) {
        g_checkBad++;
        g_checkPixels += bad;
        if (g_checkDumps < 6) {
            const char *dir = getenv("WCNET_DUMP_DIR");
            char name[512];
            for (int which = 0; which < 2; which++) {
                snprintf(name, sizeof(name), "%s/check%u_%s.pgm", dir && dir[0] ? dir : ".", g_checkDumps, which ? "game" : "fast");
                FILE *f = fopen(name, "wb");
                if (!f) continue;
                fprintf(f, "P5\n%d %d\n255\n", wide, deep);
                for (int r = 0; r < deep; r++) {
                    // (what the mask closed is written as the game's: the files differ where the pictures do)
                    Bit8u row[320];
                    memcpy(row, draw + (Y0 + r) * 640 + X0, wide);
                    if (!which) for (int c = 0; c < wide; c++) if (!g_fineOn || g_fine[(Y0 + r) * 640 + X0 + c]) row[c] = g_checkPic[r * 320 + c];
                    fwrite(row, 1, wide, f);
                }
                fclose(f);
            }
            wclog(1, "T640 check: frame %u quarter %d: %u pixels differ, the first at %d,%d (fast %02X, the game's %02X; mask %d, open there %d): check%u_fast.pgm, _game.pgm",
                  g_frames, pass, bad, fx, fy, g_checkPic[fy * 320 + fx], draw[(Y0 + fy) * 640 + X0 + fx], (int)g_fineOn, (int)g_fine[(Y0 + fy) * 640 + X0 + fx], g_checkDumps);
            if (tie640_native_on()) tie640_native_row(fy);      // (the hand-written pass: its runs of that row)
            g_checkDumps++;
        }
    }
    // (the stack below the frame's own, when the stack is in the data segment: interrupts' frames, dead)
    const unsigned spAt = SegValue(ss) == (Bit16u)(g_load + kDataPara) ? reg_sp : 0x10000;
    bool news = false;
    for (unsigned i = 0; i < 0x10000; i++) {
        if (g_checkDs[i] == mem_readb(ds + i) || (i < spAt && i + 0x1000 >= spAt) || g_dsDiff.count((Bit16u)i)) continue;
        g_dsDiff.insert((Bit16u)i);
        news = true;
    }
    static bool g_dsNews = false;
    g_dsNews |= news;
    const double now = PIC_FullIndex();
    if (now >= g_checkNextMs) {
        g_checkNextMs = now + 5000;
        if (g_dsNews) {
            g_dsNews = false;
            std::string text;
            unsigned ranges = 0;
            for (std::set<Bit16u>::const_iterator i = g_dsDiff.begin(); i != g_dsDiff.end() && ranges < 600; ranges++) {
                unsigned lo = *i, hi = lo;
                for (++i; i != g_dsDiff.end() && *i <= hi + 2; ++i) hi = *i;
                char b[24];
                snprintf(b, sizeof(b), lo == hi ? " %04X" : " %04X-%04X", lo, hi);
                text += b;
            }
            wclog(1, "T640 check: the data segment's bytes that differed after a quarter (all so far):%s", text.c_str());
        }
        wclog(1, "T640 check: %u quarters compared, %u differ (%u pixels); left out: craft %u of %u, bolts %u of %u, statics %u of %u, sprites %u of %u; %u data segment bytes ever differed",
              g_checkQuarters, g_checkBad, g_checkPixels, g_culled[0], g_culled[0] + g_drawn[0], g_culled[1], g_culled[1] + g_drawn[1],
              g_culled[2], g_culled[2] + g_drawn[2], g_culled[3], g_culled[3] + g_drawn[3], (unsigned)g_dsDiff.size());
    }
    return false;
}

// WCNET_T640_CHECK=2: a whole frame's scene twice from the same start.  At the scene's start the conventional memory
// is kept; after the four quarters drawn with what is on here, the picture and the memory are kept and the start put
// back; the four quarters are drawn again as the game alone draws them, and the two results compared: the picture's
// pixels, the data segment, the rest of the memory.
namespace {
std::vector<Bit8u> g_frameStart, g_frameFast;
Bit8u g_framePic[640 * 400];
std::set<Bit32u> g_memDiff;
unsigned g_frameChecks = 0, g_frameBad = 0, g_framePixels = 0, g_frameDumps = 0;
bool g_frameKept = false, g_memNews = false;
}

void tie640_fast_frame_begin() {
    read_env();
    if (g_check != 2 || g_reference) return;
    g_frameStart.assign(MemBase, MemBase + 0xA0000);
    g_frameKept = true;
}

// The frame's last quarter is done (tie640's own state still the quarters'): true to have the frame's scene again.
bool tie640_fast_frame_again(const Bit8u *draw) {
    if (g_check != 2 || !g_frameKept) return false;
    if (!g_reference) {
        g_frameFast.assign(MemBase, MemBase + 0xA0000);
        memcpy(g_framePic, draw, sizeof(g_framePic));
        memcpy(MemBase, &g_frameStart[0], 0xA0000);
        g_reference = true;
        return true;
    }
    g_reference = false;
    g_frameKept = false;
    g_frameChecks++;
    unsigned bad = 0;
    int first = -1;
    for (int i = 0; i < 640 * 400; i++) if (g_framePic[i] != draw[i] && (!g_fineOn || g_fine[i])) { if (!bad++) first = i; }
    if (bad) {
        g_frameBad++;
        g_framePixels += bad;
        if (g_frameDumps < 4) {
            const char *dir = getenv("WCNET_DUMP_DIR");
            char name[512];
            for (int which = 0; which < 2; which++) {
                snprintf(name, sizeof(name), "%s/frame%u_%s.pgm", dir && dir[0] ? dir : ".", g_frameDumps, which ? "game" : "fast");
                FILE *f = fopen(name, "wb");
                if (!f) continue;
                fprintf(f, "P5\n640 400\n255\n");
                fwrite(which ? draw : g_framePic, 1, 640 * 400, f);
                fclose(f);
            }
            wclog(1, "T640 check: frame %u: %u pixels differ, the first at %d,%d: frame%u_fast.pgm, _game.pgm", g_frames, bad, first % 640, first / 640, g_frameDumps);
            g_frameDumps++;
        }
    }
    // the memory: the stack below the frame's own (interrupts' frames) and the BIOS's data left out
    const Bit32u stackHi = (Bit32u)SegPhys(ss) + reg_sp, stackLo = stackHi > 0x1000 ? stackHi - 0x1000 : 0;
    for (Bit32u i = 0x600; i < 0xA0000; i++) {
        if (MemBase[i] == g_frameFast[i] || (i >= stackLo && i < stackHi) || g_memDiff.count(i)) continue;
        g_memDiff.insert(i);
        g_memNews = true;
    }
    const double now = PIC_FullIndex();
    if (now >= g_checkNextMs) {
        g_checkNextMs = now + 5000;
        wclog(1, "T640 check: %u frames drawn twice from the same start, %u differ (%u pixels); %u bytes of memory ever differed",
              g_frameChecks, g_frameBad, g_framePixels, (unsigned)g_memDiff.size());
        if (g_memNews) {
            g_memNews = false;
            // by segment: the data segment's offsets, the others' linear addresses
            const Bit32u dsLo = (Bit32u)dsa(0), dsHi = dsLo + 0x10000;
            std::string text;
            unsigned ranges = 0;
            for (std::set<Bit32u>::const_iterator i = g_memDiff.begin(); i != g_memDiff.end() && ranges < 400; ranges++) {
                Bit32u lo = *i, hi = lo;
                for (++i; i != g_memDiff.end() && *i <= hi + 2; ++i) hi = *i;
                char b[40];
                if (lo >= dsLo && lo < dsHi) snprintf(b, sizeof(b), lo == hi ? " DS:%04X" : " DS:%04X-%04X", lo - dsLo, hi - dsLo);
                else snprintf(b, sizeof(b), lo == hi ? " %05X" : " %05X-%05X", lo, hi);
                text += b;
            }
            wclog(1, "T640 check: the memory that differed after a frame (all so far; DS: the data segment's offsets, else linear):%s", text.c_str());
        }
    }
    return false;
}

// The same places with their segments (image paragraph, offset), for a dispatcher that watches by both
// (wc::watch_at): the culls' four, the translated routines' entries, the places control comes back to.
void tie640_fast_places(void (*place)(Bit16u para, Bit16u ip)) {
    if (tie640_onepass_on()) return;    // (the one pass: no quarter to leave anything out of)
    place(kFlightPara, 0x0D0A);
    place(kRendPara, 0xA02A);
    // (the hand-written scanline pass: no translated code, so none of its places; and of the culls the craft's and
    // the sprites' only -- a bolt's saves less than its hook's call costs in the browser)
    if (tie640_native_on()) return;
    place(kFlightPara, 0x0D4A);
    place(kFlightPara, 0x0FA1);
    for (size_t i = 0; i < sizeof(kRcEntries) / sizeof(kRcEntries[0]); i++) place((Bit16u)(kRcEntries[i] >> 16), (Bit16u)kRcEntries[i]);
    for (size_t i = 0; i < sizeof(kRcReturns) / sizeof(kRcReturns[0]); i++) place((Bit16u)(kRcReturns[i] >> 16), (Bit16u)kRcReturns[i]);
}

void tie640_fast_watch(void (*watch)(Bit16u ip)) {
    static const Bit16u ips[] = { 0x0D0A, 0x0D4A, 0x0FA1, 0xA02A };
    for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) watch(ips[i]);
    for (size_t i = 0; i < sizeof(kRcEntries) / sizeof(kRcEntries[0]); i++) watch((Bit16u)kRcEntries[i]);
    for (size_t i = 0; i < sizeof(kRcReturns) / sizeof(kRcReturns[0]); i++) watch((Bit16u)kRcReturns[i]);
}

// A place of ours came up while a quarter is drawn: true when it was ours.
bool tie640_fast_hook(Bit32u ip) {
    const bool trip = !g_rcInside;      // (called by the CPU's loop, not by the translation: a trip out of the loop in the browser)
    if (trip) {
        g_hits++;
        if (g_prof) g_hitAt[place_now((Bit16u)ip)]++;
    }
    if (!g_rcInside && rc_enter((Bit16u)ip)) { g_ours++; return true; }
    if (g_pass < 0 || g_pass > 3) return false;
    if (cs_is(kFlightPara)) {
        const PhysPt bp = SegPhys(ss) + (Bit16u)reg_bp;
        switch (ip) {
        case 0x0D0A: if (trip) g_ours++;      // a craft has passed the game's own tests (in the view's cone, a pixel or more): about to be drawn
            if (fast(F_CULL_CRAFT) && object_off_quarter() && !craft_has_sprites(mem_readw(bp - 0x1A))) {
                g_culled[0]++;
                reg_eip = 0x0B7D;       // (the loop's next object)
            } else if (!g_reference) g_drawn[0]++;
            return true;
        case 0x0D4A: if (trip) g_ours++;      // a bolt: tie_checkobjecteyexyz has returned (AX: to be drawn)
            if (reg_ax && fast(F_CULL_BOLT) && object_off_quarter()) {
                g_culled[1]++;
                reg_eip = 0x0B7D;
            } else if (reg_ax && !g_reference) g_drawn[1]++;
            return true;
        case 0x0FA1: if (trip) g_ours++;      // a static object to be drawn (its turning done)
            if (fast(F_CULL_STATIC) && object_off_quarter()) {
                g_culled[2]++;
                reg_eip = 0x0E08;       // (the statics' loop's next)
            } else if (!g_reference) g_drawn[2]++;
            return true;
        }
        return false;
    }
    if (ip == 0xA02A && cs_is(kRendPara)) {
        if (trip) g_ours++;
        if (fast(F_CULL_SPRITE) && sprite_off_quarter()) {
            g_culled[3]++;
            const Bit16u retIp = CPU_Pop16(), retCs = CPU_Pop16();     // (a far return: the caller takes its arguments off)
            SegSet16(cs, retCs);
            reg_eip = retIp;
        } else if (!g_reference) g_drawn[3]++;
        return true;
    }
    return false;
}

bool tie640_fast_reference() { return g_reference; }

// tie640's own watched places (for the check that the translation calls every hook it passes).
void tie640_fast_hooked(const Bit16u *ips, int n) {
    g_hooked.assign(ips, ips + n);
    g_rcKnown = -1;
}

// tie640's hook has set the CPU at the scene section's start (0090:0B1A) and done its own work for the quarter: the
// section runs as translated code from here -- to its end, where the translation calls tie640's hook itself and
// goes on with the next quarter, the frame's four in one run unless something leaves it (the stars' routine, a
// sprite drawn, the overlay's static objects).  The LAST thing tie640's hook does before it returns.
void tie640_fast_run_scene() {
    if (g_rcInside || g_tracePhase || !rc_allowed() || !rc_known()) return;
    if (!cs_is(kFlightPara) || (Bit16u)reg_eip != 0x0B1A) return;
    g_rcSkipAt = 0x00900B1Au;
    rc_run(0x0B1A, false);
}

void tie640_fast_tick(Bit16u loadSeg) {
    g_load = loadSeg;
    read_env();
    if (g_tracePhase == 1) trace_tick();
    if (g_prof) prof_sample();
}

// The scene section's start and end, in the plain game too (the profile's frames).
void tie640_fast_scene(bool on, int ip) {
    if (ip == 0x0B1A && (!on || g_pass < 0) && !g_reference) crowd_step();     // (a frame's scene about to be drawn)
    if (on) return;
    if (ip == 0x0B1A) g_pass = 4;
    else { g_pass = -1; g_frames++; }
}

// (test aid) !t640crowd=<forward>:<spacing>: every live craft of our side but ours held in a grid three wide so far ahead of us,
// each frame: many craft in the view at once.  !t640crowd=0 lets them go.
namespace {
int g_crowdAhead = 0, g_crowdSpacing = 0;
void crowd_step() {
    const Bit16u me = r16(0xCD78);      // (pstate: our object's record)
    if (!g_crowdAhead || me < 0x67CA) return;
    int n = 0;
    for (int i = 0; i < 28; i++) {
        const Bit16u o = (Bit16u)(0x67CA + 0x58 * i);
        if (o == me || !mem_readb(dsa((Bit16u)(o + 4)))) continue;
        if (mem_readb(dsa((Bit16u)(o + 0x33))) != mem_readb(dsa((Bit16u)(me + 0x33)))) continue;     // (our side's only: the others would shoot us down)
        const int col = n % 3 - 1, row = n / 3 % 3 - 1, deep = n / 9;
        n++;
        for (int a = 0; a < 3; a++) {
            // (our forward, side and up vectors, +0x42, +0x48, +0x4E, 32767 = 1)
            const Bit64s d = (Bit64s)(g_crowdAhead + deep * g_crowdSpacing) * (Bit16s)r16((Bit16u)(me + 0x42 + 2 * a)) +
                             (Bit64s)(col * g_crowdSpacing) * (Bit16s)r16((Bit16u)(me + 0x48 + 2 * a)) +
                             (Bit64s)(row * g_crowdSpacing) * (Bit16s)r16((Bit16u)(me + 0x4E + 2 * a));
            mem_writed(dsa((Bit16u)(o + 6 + 4 * a)), (Bit32u)((Bit32s)r32((Bit16u)(me + 6 + 4 * a)) + (Bit32s)(d / 32767)));
        }
    }
}
}  // namespace

bool tie640_fast_script_item(const std::string &item, double t) {
    if (item.compare(0, 11, "!t640crowd=") == 0) {
        g_crowdAhead = g_crowdSpacing = 0;
        sscanf(item.c_str() + 11, "%d:%d", &g_crowdAhead, &g_crowdSpacing);
        wclog(1, "key script %.1fs: T640: the craft %s", t, g_crowdAhead ? "held in a grid ahead" : "let go");
        return true;
    }
    return false;
}

void tie640_fast_pass(int pass, bool frameDone) {
    g_pass = pass;
    if (frameDone) g_frames++;
}

}  // namespace wc
