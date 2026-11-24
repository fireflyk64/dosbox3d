/*
 *  The TIE Fighter demo at 640x400: the renderer's edge tracers, hand-written (docs/tie640.md, 4b).
 *
 *  trace2_drawface (1CCA:53F3) takes a mesh face whose edges transfm2_classifyedges has prepared and writes each
 *  edge not yet traced into the rows' lists (the entries and records wcnet_tie640_native.cpp's RowLists reads).
 *  This file is that routine written again as native code, working on an IMAGE of the game's memory (its data
 *  segment and its tables' segment, 64 K each) in the game's own formats.
 *
 *  The other three routines that fill the lists are here too (struct Shapes): trace2_drawscreencoords (1CCA:4FF3,
 *  a flat polygon), drawln2_tracelineedges (1CCA:2BB0, a thick line) and the sprite scaler's row pass (1CCA:A132),
 *  each checked in the same way (the line's two points and the background buffer's segment compared as well).
 *
 *  WCNET_T640_TRACECHECK=1 (with WCNET_T640_FAST=0): in every quarter, at every call of the game's routine, the
 *  memory is copied, the native routine run on the copy, and when the game's routine has returned the two are
 *  compared byte for byte (the tables' segment 0000..9FFF: the object records and the lists' pool; the data segment
 *  2000..67FF: the rows' table, the edges' tables and flags, the pool's cursor).  `!t640trace` logs the counts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "wcnet_t640_game.h"
#include "wcnet_tie640.h"
#include "wcnet_log.h"
#include "regs.h"
#include "cpu.h"
#include "mem.h"

namespace wc {

// the image the tracers work on
struct TraceMem {
    Bit8u *ds, *tb;     // the data segment, the tables' segment (64 K each)
    const Bit8u *cs;    // the renderer's code segment (for the materials' table in it; may be null)
    // (words and longs through memcpy: the emulator's host_readw / host_writed are casts of the pointer, and with
    // them the compiler may take a word read not to see a long written just before -- it did, in the lines' twin)
    static inline Bit16u get16(const Bit8u *p) { Bit16u v; memcpy(&v, p, 2); return v; }
    static inline Bit32u get32(const Bit8u *p) { Bit32u v; memcpy(&v, p, 4); return v; }
    static inline void put16(Bit8u *p, Bit16u v) { memcpy(p, &v, 2); }
    static inline void put32(Bit8u *p, Bit32u v) { memcpy(p, &v, 4); }
    inline Bit16u d16(Bit16u o) const { return get16(ds + o); }
    inline Bit8u d8(Bit16u o) const { return ds[o]; }
    inline Bit32u d32(Bit16u o) const { return get32(ds + o); }
    inline void D16(Bit16u o, Bit16u v) { put16(ds + o, v); }
    inline void D8(Bit16u o, Bit8u v) { ds[o] = v; }
    inline void D32(Bit16u o, Bit32u v) { put32(ds + o, v); }
    inline Bit16u t16(Bit16u o) const { return get16(tb + o); }
    inline Bit8u t8(Bit16u o) const { return tb[o]; }
    inline Bit32u t32(Bit16u o) const { return get32(tb + o); }
    inline void T16(Bit16u o, Bit16u v) { put16(tb + o, v); }
    inline void T8(Bit16u o, Bit8u v) { tb[o] = v; }
    inline void T32(Bit16u o, Bit32u v) { put32(tb + o, v); }
};

namespace {
typedef Bit8u u8; typedef Bit16u u16; typedef Bit32u u32; typedef Bit8s s8; typedef Bit16s s16; typedef Bit32s s32;

// Where things are in the data segment
enum : u16 {
    // the face and its object (inputs)
    kVertexPoint = XA(0x213A, 0x2C72),      // [2v] vertex v's screen point (near pointer to {long x, long y}; 0 = behind the near plane)
    kVertexLight = XA(0x223A, 0x2D72),      // [2v] its light
    kGouraud = XA(0x243A, 0x2F72),          // 1: the face is shaded from its vertices' lights
    kObject = XA(0x2464, 0x2F9C), kFace = XA(0x2474, 0x2FAC), kRecord = XA(0x247E, 0x2FB6), kEdges = XA(0x2470, 0x2FA8),
    kShadeMode = XA(0x26DC, 0x3214),
    // the edges' tables (inputs; the flags and the slope's cache are written)
    kPointA = XA(0x2AF0, 0x3635), kPointB = XA(0x2BF0, 0x3735),                 // [2e] the edge's two points
    kAbsDx = XA(0x3090, 0x3BD5), kAbsDy = XA(0x3978, 0x44BD),                   // [4e] longs
    kSignDx = XA(0x28F0, 0x3435), kSignDy = XA(0x2970, 0x34B5),                 // [e]
    kFlags = XA(0x2FF0, 0x3B35),                                    // [e]
    kSlopeFrac = XA(0x2DF0, 0x3935), kSlopeLo = XA(0x2EF0, 0x3A35), kSlopeHi = XA(0x3B78, 0x46BD),     // [2e] the slope kept (flag 40)
    // the window
    kWide = XA(0x48C5, 0x5423), kWideLess1 = XA(0x48C7, 0x5427), kDeep = XA(0x48D1, 0x5433),
    kWide7 = XA(0x28AE, 0x33F3),            // the width << 7
    kRowLimit = XA(0x28B0, 0x33F5),         // (long) the steep tracers' end: row.fraction and light packed as they keep them
    kDeep2 = XA(0x28B4, 0x33F9),            // the depth * 2
    kOddWord = XA(0x28D2, 0x3417),          // (a word the game's clip leaves in CX in one case; FFFF)
    // the lists
    kRows = XA(0x6232, 0xD3C8), kPool = XA(0x274A, 0x3282),
    // the routine's own variables (the game's, kept in the image where the game keeps them)
    kCountdown = XA(0x245E, 0x2F96), kTraced = XA(0x2466, 0x2F9E),
    kId = XA(0x274C, 0x3284),               // byte: the edge, byte: the object -- the tag of every entry written
    kLightA = XA(0x274E, 0x3286), kLightB = XA(0x2750, 0x3288),     // the lights at the edge's first and second point
    kLightPerPixel = XA(0x2752, 0x328A),    // a steep edge's light step a pixel of x
    kLightPerRow2 = XA(0x2754, 0x328C),     // twice the light's step a row
    kStartX32 = XA(0x2756, 0x328E),         // (long) a shallow edge's x where it starts
    kStartX = XA(0x275A, 0x3292),           // the x it starts at in the window (or how the clip left it)
    kRowFrom = XA(0x275C, 0x3294), kRowTo = XA(0x275E, 0x3296),     // bytes: the rows it runs between
    kSignX = XA(0x2760, 0x3298), kSignY = XA(0x2762, 0x329A),       // bytes: +1 / -1
    kSpareEdge = XA(0x2764, 0x32A4),        // an edge number of this face that needs no tracing: lent to the cut's closing edge
    kCutPoint = XA(0x2766, 0x32A6), kBehind = XA(0x2768, 0x32A8),
};
enum { kPoolEnd = XA(0x9F80, 0xDF80), kShadesMiddle = XA(0x0798, 0x0798) };
enum Side { kLeftSide, kRightSide };

// The check's notes of which parts of the tracer have run (the log lists them, and those that never did)
#define TRACE_PARTS(X) X(face_shaded) X(face_flat) X(div_small) X(div_by_whole) X(div_by_fraction) X(div_big_estimates) X(div_big) X(div_mid_exact) X(div_mid_estimates) X(divfar) X(divfar_narrow) X(slope_under1_longdivisor) X(slope_long_both) X(slope_over_word) X(slope_under1) X(slope_plain) X(pool_full_reuse) X(put8_split) X(put12_split_2_1) X(put12_split_1_2) X(steepdown_starts_at_0) X(steepdown_leaves) X(steepdown) X(steepup) X(steepup_starts_at_0) X(steepup_last_short) X(steepup_leaves) X(shallowdown_left) X(shallowdown_right) X(shallowdown_leaves_left) X(shallowdown_leaves_right) X(shallowup_left) X(shallowup_right) X(shallowup_leaves_left) X(shallowup_leaves_right) X(clip_from_b) X(clip_from_b_a_outside) X(clip_far_above) X(clip_just_above) X(clip_below) X(steep_far_above_long_slope) X(steep_outside_long_slope) X(shallow_comes_in) X(steepbeside_down) X(steepbeside_up) X(STRAY_upright_beside_up) X(steep_upright) X(steep_too_far_out) X(steep_comes_in) X(steep_starts_beside) X(steep_fraction_out) X(shallowbeside_down) X(shallowbeside_up) X(shallowbeside_coarse) X(level_edge) X(level_edge_far) X(level_edge_column) X(shallow_too_far_out) X(shallow_comes_in_walk) X(left_flat) X(beside_shallow_coarse) X(beside_shallow_left) X(beside_shallow_right) X(beside_shallow_from_above) X(beside_steep_left) X(beside_steep_right) X(beside_steep_from_above) X(beside_steep_long_slope) X(cut_no_spare_edge) X(cut_shaded_made_flat) X(cut_column) X(cut_steep) X(cut_shallow) X(cut_first_vertex_in_front) X(cut_first_vertex_behind) X(flat_outside) X(flat_none_left) X(flat_upright_line) X(flat_level_line) X(flat_level_line_one_end) X(flat_outline) X(flat_edge_upright) X(flat_edge_column) X(flat_edge_steep) X(flat_edge_shallow) X(line_outside) X(line_none_left) X(line_wide_beside) X(line_wide) X(line_light_along) X(line_thick_0) X(line_light_across) X(line_end_brought_in) X(line_wide_upper_edge) X(line_wide_level) X(line_wide_below) X(line_cap) X(line_wide_lower_out) X(line_wide_lower_edge_only) X(line_wide_lower_edge) X(line_tall_above_below) X(line_tall) X(line_tall_left_out) X(line_tall_right_out) X(line_tall_left_record) X(line_tall_steep) X(line_tall_shallow) X(line_tall_right_only_steep) X(line_tall_right_only_shallow) X(line_tall_right_steep) X(line_tall_right_shallow) X(sprite_none_left) X(sprite_outside) X(sprite_rows) X(sprite_run) X(own_put4_new_chunk) X(own_put4_into_full) X(own_put4_into_link)
#define X(n) k_##n,
enum { TRACE_PARTS(X) kParts };
#undef X
#define X(n) #n,
const char *const g_partNames[] = { TRACE_PARTS(X) };
#undef X
unsigned g_parts[kParts];
int g_path[48], g_pathLength = 0;       // the parts of the call being checked, in order (for the log of a call that differs)
inline void seen(int part) { g_parts[part]++; if (g_pathLength < 48) g_path[g_pathLength++] = part; }
std::string path_text() { std::string t; for (int i = 0; i < g_pathLength; i++) { t += ' '; t += g_partNames[g_path[i]]; } return t; }

// ---------------------------------------------------------------------------------------------------------------
// The game's divisions.  They are not plain divisions: each has its own rounding and short cuts, and the tracers'
// numbers depend on every one of them.  A divisor is 16.16 fixed point (whole : fraction).
// ---------------------------------------------------------------------------------------------------------------

// n / d, rounded up when the remainder is more than half the divisor
inline u32 div_rounded(u32 n, u16 d) {
    const u32 q = n / d, r = n % d;
    return q + ((u32)(d >> 1) < r ? 1 : 0);
}

// 1CCA:F59D.  n (16.16) / d (16.16) -> a whole number, rounded; by cases of the divisor's size.
u32 fixed_div_round(u32 n, u16 dWhole, u16 dFrac) {
#ifdef T640_XWING
    // X-Wing's (23CF:F875) is the plain division, not rounded: n / d, both taken as longs (a divisor of 0: n as it is).
    // The demo's by-cases routine below is a later build's.  (Where an edge comes in from just beside the window the
    // two differ by a row: the seam at a quarter's side, the user's "cut line".)
    { const u32 d = (u32)dWhole << 16 | dFrac; return d ? n / d : n; }
#endif
    u16 nHi = (u16)(n >> 16), nLo = (u16)n;
    if (nHi == 0) {
        // less than one: only a divisor less than one gives anything
        seen(k_div_small);
        if (dWhole != 0 || nLo == 0) return 0;
        return dFrac > 1 ? div_rounded(nLo, dFrac) : nLo;
    }
    if (dFrac == 0) {
        // a whole divisor: the dividend rounded to a whole number first
        seen(k_div_by_whole);
        if (dWhole == 0) return n;
        const u16 whole = (u16)((n + 0x8000) >> 16);
        return dWhole > 1 ? div_rounded(whole, dWhole) : whole;
    }
    if (dWhole == 0) { seen(k_div_by_fraction); return div_rounded(n, dFrac); }
    if (dWhole >= 0xFF) {
        // a big divisor: the answer is small.  0 or 1 by "twice the dividend reaches the divisor" ...
        seen(k_div_big);
        if (nHi == dWhole) return 1;
        if (nHi < dWhole) return ((n & 0x80000000u) || (u16)((n << 1) >> 16) >= dWhole) ? 1 : 0;
        // ... else by repeated estimates with the whole part + 1, each correcting the last
        seen(k_div_big_estimates);
        const u16 est = (u16)(dWhole + 1), back = (u16)(0 - dFrac);
        if (!est) return 0;
        u16 total = 0;
        for (;;) {
            const u16 q = (u16)(nHi / est), r = (u16)(nHi % est);
            total = (u16)(total + q);
            const u32 p = (u32)q * back + nLo;
            nLo = (u16)p;
            nHi = (u16)((p >> 16) + r);
            if (nHi > est) continue;
            if (nHi == est) return (u16)(total + 1);
            break;
        }
        const u32 half = ((u32)dWhole << 16 | dFrac) >> 1;
        const u16 halfHi = (u16)(half >> 16), halfLo = (u16)half;
        if (nHi < halfHi || (nHi == halfHi && nLo < halfLo)) return total;
        return (u16)(total + 1);
    }
    // a divisor of 1..254.x: its middle 16 bits (8.8) divide the dividend's upper 24
    if (nHi < dWhole) return ((n & 0x80000000u) || (u16)((n << 1) >> 16) >= dWhole) ? 1 : 0;
    if (nHi == dWhole) return 1;
    const u16 mid = (u16)(dWhole << 8 | dFrac >> 8);
    const u8 low = (u8)dFrac;
    if (low == 0) { seen(k_div_mid_exact); return div_rounded(((n + 0x80) >> 8) & 0xFFFFFF, mid); }
    // ... and when the divisor has bits below those: estimates with the middle + 1, corrected by the low byte
    seen(k_div_mid_estimates);
    const u16 est = (u16)(mid + 1), back = (u8)(0 - low);
    if (!est) return 0;
    u16 total = 0;
    u32 rest = n;
    u8 below;
    u16 a;
    for (;;) {
        do {
            below = (u8)rest;
            rest >>= 8;
            const u16 q = (u16)(rest / est), r = (u16)(rest % est);
            total = (u16)(total + q);
            rest = (u32)q * back + ((u32)r << 8 | below);
        } while (rest >> 24);
        bool again = false;
        for (;;) {
            below = (u8)rest;
            a = (u16)(rest >> 8);
            if (a < est) break;
            if (a == est) return (u16)(total + 1);
            const u16 q = (u16)(a / est), r = (u16)(a % est);
            total = (u16)(total + q);
            rest = (u32)q * back + ((u32)r << 8 | below);
            if (rest >> 24) { again = true; break; }
        }
        if (!again) break;
    }
    // the last half: round up when what is left reaches half of (middle : low byte)
    const u16 e = (u16)(est - 1);
    const u8 halfLow = (u8)((u8)(0 - back) >> 1 | (e & 1) << 7);
    const u16 halfMid = (u16)(e >> 1);
    if (a < halfMid || (a == halfMid && below < halfLow)) return total;
    return (u16)(total + 1);
}

// 1CCA:F6F0.  n (whole) / d (16.16) -> a whole number, not rounded; used once, for a steep edge that begins far
// above the window.
u32 fixed_div_far(u32 n, u16 dWhole, u16 dFrac) {
#ifdef T640_XWING
    // X-Wing's (23CF:F897): the 48-bit n : 0000 by the divisor as a long; when that would overflow, the dividend's
    // low word moved up is what is left in the registers
    {
        const u32 d = (u32)dWhole << 16 | dFrac;
        if ((n >> 16) >= d) return (n & 0xFFFF) << 16;
        return (u32)((((Bit64u)(n >> 16) << 32) | ((Bit64u)(n & 0xFFFF) << 16)) / d);
    }
#endif
    if ((n >> 16) == 0) return fixed_div_round(n << 16, dWhole, dFrac);
    seen(k_divfar);
    if (dFrac == 0) return dWhole <= 1 ? n : n / dWhole;
    const bool narrow = dWhole < 0xFF;       // (then the divisor is taken as 8.16 and the answer has 8 more bits)
    const u16 est = narrow ? (u16)((u16)(dWhole << 8 | dFrac >> 8) + 1) : (u16)(dWhole + 1);
    const u16 back = narrow ? (u16)(0 - (u16)((dFrac & 0xFF) << 8 | dWhole >> 8)) : (u16)(0 - dFrac);
    if (!est) return 0;
    u16 totalLo = 0, totalHi = 0;
    u32 rest = n;
    if ((u16)(rest >> 16) >= est) {
        const u16 qHi = (u16)((rest >> 16) / est);
        const u32 t = ((rest >> 16) % est) << 16 | (rest & 0xFFFF);
        const u16 qLo = (u16)(t / est), r = (u16)(t % est);
        totalHi = qHi; totalLo = qLo;
        rest = (u32)qHi * back + (((u32)qLo * back) >> 16) + r;
    }
    for (;;) {
        const u16 q = (u16)(rest / est), r = (u16)(rest % est);
        const u32 sum = (u32)totalLo + q;
        totalLo = (u16)sum;
        totalHi = (u16)(totalHi + (sum >> 16));
        rest = (((u32)q * back) >> 16) + r;
        if (rest >> 16) continue;
        if ((u16)rest > est) continue;
        break;
    }
    if (!narrow) return (u32)totalHi << 16 | totalLo;
    seen(k_divfar_narrow);
    // eight more bits from what is left, and the total moved up to make room (the game's byte shuffle brings the
    // total's top byte round to the bottom: kept)
    const u32 last = (rest & 0xFFFF) << 8;
    const u16 q = (u16)(last / est), r = (u16)(last % est);
    u16 add = (u8)q;
    const unsigned t = ((((u8)q * (unsigned)(back >> 8)) >> 8) & 0xFF) + (r >> 8);
    if (t > 0xFF || (u8)t > (u8)(est >> 8)) add++;
    const u32 moved = (u32)(u16)((totalHi & 0xFF) << 8 | totalLo >> 8) << 16 | (u16)((totalLo & 0xFF) << 8 | totalHi >> 8);
    return moved + add;
}

// 1CCA:F4E7.  A slope: n / d (both whole longs) as whole part (long) and fraction (word).
struct Slope { u16 frac, lo, hi; };
Slope slope_div(u32 n, u32 d) {
    u16 nHi = (u16)(n >> 16), nLo = (u16)n, dHi = (u16)(d >> 16), dLo = (u16)d;
    Slope s = { 0, 0, 0 };
    const Slope most = { 0, 0xFFFF, 0x7FFF };       // (a division by nothing)
    bool small = false, medium = false;
    if (nHi == 0) {
        if (dHi != 0) { seen(k_slope_under1_longdivisor); const u32 q = fixed_div_round((u32)nLo << 16, dHi, dLo); s.frac = (u16)q; s.hi = (u16)(q >> 16); return s; }
        if (nLo == 0) return s;
        if (dLo == 0) return most;
        if (dLo == 1) { s.lo = nLo; return s; }
        small = true;
    } else {
        if (dHi != 0) {
            // both brought down until the divisor fits a word
            seen(k_slope_long_both);
            if (dHi >> 8) { d >>= 8; n >>= 8; }
            do { d >>= 1; n >>= 1; } while (d >> 16);
            nHi = (u16)(n >> 16); nLo = (u16)n; dLo = (u16)d;
        }
        if (dLo == 0) return most;
        if (nHi == 0) small = true;
        else if (nHi < dLo) medium = true;
        else {
            if (dLo <= 1) { s.lo = nLo; s.hi = nHi; return s; }
            // a quotient of more than a word: rounded, and the "fraction" is the remainder as it is
            seen(k_slope_over_word);
            const u32 q = div_rounded(n, dLo);
            s.frac = (u16)(n % dLo); s.lo = (u16)q; s.hi = (u16)(q >> 16);
            return s;
        }
    }
    if (small) {
        if (dLo <= 1) { s.lo = nLo; return s; }
        if (nLo == dLo) { s.lo = 1; return s; }
        if (nLo < dLo) { seen(k_slope_under1); s.frac = (u16)(((u32)nLo << 16) / dLo); return s; }
        medium = true;
    }
    if (medium) {
        if (dLo <= 1) { s.lo = nLo; s.hi = nHi; return s; }
        const u32 whole = ((u32)nHi << 16 | nLo);
        seen(k_slope_plain);
        s.lo = (u16)(whole / dLo);
        s.frac = (u16)(((whole % dLo) << 16) / dLo);
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// The tracer
// ---------------------------------------------------------------------------------------------------------------
struct Tracer {
    TraceMem &m;
    u16 strayHigh;      // the high half of ECX as the code before left it: the game writes it into a record in one
                        // odd case (an upright edge beside the window, traced upwards)

    explicit Tracer(TraceMem &mem) : m(mem), strayHigh(0) {}

    u16 wide() const { return m.d16(kWide); }
    u16 deep() const { return m.d16(kDeep); }
    s8 sign_x() const { return (s8)m.d8(kSignX); }
    s8 sign_y() const { return (s8)m.d8(kSignY); }
    s16 light_per_row2() const { return (s16)m.d16(kLightPerRow2); }
    s16 light_per_pixel() const { return (s16)m.d16(kLightPerPixel); }
    u32 id() const { return m.d16(kId); }
    u8 flags(u16 e) const { return m.d8((u16)(kFlags + e)); }
    void set_flags(u16 e, u8 f) { m.D8((u16)(kFlags + e), (u8)(m.d8((u16)(kFlags + e)) | f)); }
    void swap_lights() { const u16 a = m.d16(kLightA); m.D16(kLightA, m.d16(kLightB)); m.D16(kLightB, a); }
    void turn_x() { m.D8(kSignX, (u8)(0 - m.d8(kSignX))); }
    // the edge's slot in the object's record: byte face1, byte face2, word light step a row
    u16 edge_slot(u16 e) const { const u16 r = m.d16(kRecord); return (u16)(r + 4 * e + 2 * m.t16((u16)(r + 0x10)) + 0x67); }

    // -----------------------------------------------------------------------------------------------------------
    // The rows' lists.  A row's list is a chain of 80h-byte chunks from the pool: word 0 = the bytes used (4..7Ch)
    // or, once full, the next chunk.  Three sizes are written; one that does not fit is split over two chunks.
    // When the pool is full the last chunk (9F80) is handed out again and again: the game does not check.
    // -----------------------------------------------------------------------------------------------------------
    u16 new_chunk() {
        const u16 c = m.d16(kPool);
        if (c != kPoolEnd) m.D16(kPool, (u16)(c + 0x80)); else seen(k_pool_full_reuse);
        return c;
    }
    // (a row past the table's reach cannot be written into the image: the game would write outside its segment)
    bool row_slot(u32 row, u16 &slot) const { if (row > 0x4E00) return false; slot = (u16)(kRows + 2 * row); return true; }

    // one crossing: {edge, object, x << 7 | light}.  Gives what the game's pointer register holds afterwards (the
    // entry's place, or the chunk's when the chunk is new): the upward tracer to the right compares with it.
    u16 put4(u32 row, u32 entry) {
        u16 slot;
        if (!row_slot(row, slot)) return 0;
        u16 at = m.d16(slot);
        if (!at) { at = new_chunk(); m.D16(slot, at); m.T16(at, 4); m.T32((u16)(at + 4), entry); return at; }
        for (;;) {
            const u16 used = m.t16(at);
            if (used < 0x7C) { m.T16(at, (u16)(used + 4)); m.T32((u16)(at + used + 4), entry); return (u16)(at + used + 4); }
            if (used == 0x7C) { const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 4); m.T32((u16)(n + 4), entry); return n; }
            at = used;
        }
    }
    // a crossing that stays for several rows: {FFFF, rows, light's fraction} and the crossing.  Gives what the
    // game's pointer register holds afterwards (the crossing's place, or the chunk's when the chunk is new): the
    // flat polygon's upright line passes it on as a "light".
    u16 put8(u32 row, u32 first, u32 entry) {
        u16 slot;
        if (!row_slot(row, slot)) return 0;
        u16 at = m.d16(slot);
        if (!at) { at = new_chunk(); m.D16(slot, at); m.T16(at, 8); m.T32((u16)(at + 4), first); m.T32((u16)(at + 8), entry); return at; }
        for (;;) {
            const u16 used = m.t16(at);
            if (used < 0x78) { m.T16(at, (u16)(used + 8)); m.T32((u16)(at + used + 4), first); m.T32((u16)(at + used + 8), entry); return (u16)(at + used + 8); }
            if (used == 0x78) { seen(k_put8_split); m.T32((u16)(at + 0x7C), first); const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 4); m.T32((u16)(n + 4), entry); return n; }
            if (used == 0x7C) { const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 8); m.T32((u16)(n + 4), first); m.T32((u16)(n + 8), entry); return n; }
            at = used;
        }
    }
    // an edge that runs beside the window: {kind, slope}, {edge, object, light}, {x or error, rows}  (1CCA:363A)
    void put12(u32 row, u32 first, u32 second, u32 third) {
        u16 slot;
        if (!row_slot(row, slot)) return;
        u16 at = m.d16(slot);
        if (!at) {
            at = new_chunk(); m.D16(slot, at); m.T16(at, 0xC);
            m.T32((u16)(at + 4), first); m.T32((u16)(at + 8), second); m.T32((u16)(at + 0xC), third);
            return;
        }
        for (;;) {
            const u16 used = m.t16(at);
            if (used < 0x74) {
                m.T16(at, (u16)(used + 0xC));
                m.T32((u16)(at + used + 4), first); m.T32((u16)(at + used + 8), second); m.T32((u16)(at + used + 0xC), third);
                return;
            }
            if (used == 0x74) {
                seen(k_put12_split_2_1);
                m.T32((u16)(at + 0x78), first); m.T32((u16)(at + 0x7C), second);
                const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 4); m.T32((u16)(n + 4), third);
                return;
            }
            if (used == 0x78) {
                seen(k_put12_split_1_2);
                m.T32((u16)(at + 0x7C), first);
                const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 8); m.T32((u16)(n + 4), second); m.T32((u16)(n + 8), third);
                return;
            }
            if (used == 0x7C) {
                const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 0xC);
                m.T32((u16)(n + 4), first); m.T32((u16)(n + 8), second); m.T32((u16)(n + 0xC), third);
                return;
            }
            at = used;
        }
    }
    void running(u32 row, u16 kind, u16 slope, u16 light, u32 third) {
        put12(row, (u32)slope << 16 | kind, (u32)light << 16 | id(), third);
        strayHigh = light;
    }

    // An upright stretch: one crossing at x for `rows` rows from `row`, the light as a whole word.  (1CCA:3540)
    // Gives the pointer register as the game's routine leaves it (`light` itself when there are no rows).
    u16 put_tall(u16 row, u16 rows, u16 light, u16 x) {
        if (!rows) return light;
        const u16 l = (u16)(light >> 1);
        const u16 after = put8(row, (u32)(u16)((l & 0xFF) << 8 | (rows & 0xFF)) << 16 | 0xFFFF, (u32)(u16)((u16)(x << 7) | l >> 8) << 16 | id());
        strayHigh = 0;
        return after;
    }

    // -----------------------------------------------------------------------------------------------------------
    // The steep tracers (an edge of more than two rows a pixel): for every column the edge passes, one crossing
    // that stays for the rows it spends there.  The position (row, 8 bits of fraction) and the light (6 bits, 8 of
    // fraction) are kept in the two halves of one long, as the game keeps them, and stepped with one addition.
    // The first step is half a pixel.  `rem` = the rows still to go (8.8); the walk ends when they are used up, or
    // at the window's side: there what is left goes on as a running record.
    // -----------------------------------------------------------------------------------------------------------
    void steep_column(u32 at, u16 rows, u16 x) {
        put8(at >> 24, (u32)(u16)((at & 0xFF) << 8 | (rows & 0xFF)) << 16 | 0xFFFF, (u32)(u16)((u16)(x << 7) | ((at >> 8) & 0x7F)) << 16 | id());
    }
    // the light's step for a column, by the direction of the walk (the light is modulo 40h: a step down is a big step up)
    u16 column_light(bool negated, bool half) const {
        const s16 step = light_per_pixel();
        if (negated) return (u16)((0 - (half ? step >> 2 : step >> 1)) & 0x7FFF);
        return half ? (u16)((u16)(step >> 1) >> 1) : (u16)((u16)step >> 1);
    }

    // downwards (1CCA:373E to the left, 3A3E to the right)
    void steep_down(int dir, u16 x, u16 pos, u16 light, u16 slope, u16 rem) {
        seen(k_steepdown);
        if (dir < 0 && x == 0) { seen(k_steepdown_starts_at_0); running(pos >> 8, 0xFFFD, slope, light, (u32)rem << 16 | (pos & 0xFF) << 8); return; }
        u32 at = (u32)pos << 16 | light >> 1;
        u32 left = (u32)rem << 16;
        u32 step = ((u32)slope << 16) >> 1;
        bool first = true;
        for (;;) {
            if (!first) step = (u32)slope << 16 | column_light(dir < 0, false);
            u32 limit = first ? left : (left | (step & 0xFFFF));
            if (limit < step) step = limit;
            const u16 rows = (u16)(((at + step) >> 24) - (at >> 24));
            if (!rows) return;
            steep_column(at, rows, x);
            if (first) step = (step & 0xFFFF0000u) | column_light(dir < 0, true);
            const u32 next = at + step;
            if (next < at) return;
            at = next & ~0xC000u;
            if (at >= m.d32(kRowLimit)) return;
            left -= step & 0xFFFF0000u;
            if (!left) return;
            x = (u16)(x + dir);
            first = false;
            if (dir < 0 ? x == 0 : x >= wide()) break;
        }
        // at the window's side: the rest runs on beside it
        seen(k_steepdown_leaves);
        const u16 p = (u16)(at >> 16), r = (u16)(left >> 16);
        if ((u8)((((r & 0xFF) + (p & 0xFF)) >> 8) + (r >> 8)) == 0) return;
        const u16 beside = dir < 0 ? x : (u16)(x - wide());
        running(p >> 8, dir < 0 ? 0xFFFD : 0xFFF6, slope, (u16)(at << 1), (u32)r << 16 | (p & 0xFF) << 8 | (beside & 0xFF));
    }

    // the rest of an upward edge beside the window: the record goes into its TOP row, with how far out it is there
    void steep_up_rest(int dir, u16 pos, u16 rem, u16 slope, u16 light, bool checked) {
        const u16 top = (u16)(pos - rem);
        if (checked && (u8)((((rem & 0xFF) + (top & 0xFF)) >> 8) + (rem >> 8)) == 0) return;
        if (!slope) return;
        const u16 columns = (u16)(rem / slope);
        const u16 change = (u16)(columns * (u16)light_per_pixel());
        light = dir < 0 ? (u16)(light - change) : (u16)(light + change);
        running(top >> 8, dir < 0 ? 0xFFFE : 0xFFF5, slope, light, (u32)rem << 16 | (top & 0xFF) << 8 | (columns & 0xFF));
    }

    // upwards (1CCA:3CE2 to the left, 4049 to the right): the same walk backwards, each crossing put into the top
    // row of its stretch
    void steep_up(int dir, u16 x, u16 pos, u16 light, u16 slope, u16 rem) {
        seen(k_steepup);
        if (dir < 0 && x == 0) { seen(k_steepup_starts_at_0); steep_up_rest(dir, pos, rem, slope, light, false); return; }
        u32 at = (u32)pos << 16 | light >> 1;
        u32 left = (u32)rem << 16;
        bool first = true;
        for (;;) {
            const u16 l = column_light(dir > 0, first);
            u32 step = ((first ? ((u32)slope << 16) >> 1 : (u32)slope << 16) & 0xFFFF0000u) | l;
            if ((left | l) < step) {
                // the last stretch is shorter: the light by its whole rows and the row's step
                seen(k_steepup_last_short);
                step = left | (u16)((u16)((left >> 24) * (u16)light_per_row2()) >> 1);
            }
            const u32 next = ((at | 0xC000) - step) & ~0xC000u;
            const u16 rows = (u16)((at >> 24) - (next >> 24));
            at = next;
            if (!rows) return;
            steep_column(at, rows, x);
            left -= step & 0xFFFF0000u;
            if (!left) return;
            x = (u16)(x + dir);
            first = false;
            if (dir < 0 ? x == 0 : x >= wide()) break;
        }
        seen(k_steepup_leaves);
        steep_up_rest(dir, (u16)(at >> 16), (u16)(left >> 16), slope, (u16)(at << 1), true);
    }

    // -----------------------------------------------------------------------------------------------------------
    // The shallow tracers: one crossing a row.  x (9 bits and 7 of fraction) and the light share a long again.
    // `half`: the edge starts at its end point, so the first row is half a step on.  When x leaves the window the
    // rows left go on as a running record.
    // -----------------------------------------------------------------------------------------------------------
    u16 row_light(bool negated, bool half) const {
        const s16 step = light_per_row2();
        if (negated) return (u16)((0 - (half ? step >> 2 : step >> 1)) & 0x7FFF);
        return half ? (u16)((u16)(step >> 1) >> 1) : (u16)((u16)step >> 1);
    }
    u32 crossing(u32 at) const { return (u32)(u16)((at >> 23) << 7 | ((at >> 8) & 0x7F)) << 16 | id(); }

    // downwards (1CCA:4328 to the left, 4466 to the right)
    void shallow_down(int dir, u8 row, u8 rows, u16 x, u16 slope, u16 light, bool half) {
        seen(dir < 0 ? k_shallowdown_left : k_shallowdown_right);
        u16 row2 = (u16)(row * 2);
        const u32 step = (u32)slope << 16 | row_light(dir < 0, false);
        const u32 limit = (u32)m.d16(kWide7) << 16;
        u32 at = (u32)x << 16 | light >> 1;
        bool out = false;
        if (half) {
            const u32 h = ((step >> 1) & 0xFFFF0000u) | row_light(dir < 0, true);
            if (dir < 0) { at |= 0xC000; out = at < h; at -= h; }
            else { const u32 n = at + h; out = n < at || n >= limit; at = n; }
        }
        while (!out) {
            at &= ~0xC000u;
            put4(row2 >> 1, crossing(at));
            row2 = (u16)(row2 + 2);
            if (row2 >= m.d16(kDeep2)) return;
            if (--rows == 0) return;
            if (dir < 0) { at |= 0xC000; out = at < step; at -= step; }
            else { const u32 n = at + step; out = n < at; at = n; if (!out) { at &= ~0xC000u; out = at >= limit; } }
        }
        at &= ~0xC000u;
        const u32 beside = dir < 0 ? (0 - at) >> 16 : at >> 16;
        seen(dir < 0 ? k_shallowdown_leaves_left : k_shallowdown_leaves_right);
        running(row2 >> 1, dir < 0 ? 0xFFFB : 0xFFF4, slope, (u16)(at << 1), (u32)rows << 24 | beside);
    }

    // upwards (1CCA:45AA to the left, 4704 to the right); the record of the rest goes into its top row.
    // Game bug, kept: the tracer to the right keeps the window's right side (x << 7) in the high half of a
    // register whose low half it then uses as its pointer into the lists.  So once a crossing is written, an x of
    // exactly the window's width counts as outside only if the light (the low half of the position) is not below
    // that pointer.
    void shallow_up(int dir, u8 row, u8 rows, u16 x, u16 slope, u16 light, bool half) {
        s16 row2 = (s16)(row * 2 - 2);
        if (row2 < 0) return;
        seen(dir < 0 ? k_shallowup_left : k_shallowup_right);
        const u32 step = (u32)slope << 16 | row_light(dir > 0, false);
        u32 limit = (u32)m.d16(kWide7) << 16;
        u32 at = (u32)x << 16 | light >> 1;
        bool out = false;
        if (half) {
            const u32 h = ((step >> 1) & 0xFFFF0000u) | row_light(dir > 0, true);
            if (dir < 0) { at |= 0xC000; out = at < h; at -= h; }
            else { const u32 n = at + h; out = n < at || n >= limit; at = n; }
        }
        while (!out) {
            at &= ~0xC000u;
            limit = (limit & 0xFFFF0000u) | put4((u16)row2 >> 1, crossing(at));
            row2 = (s16)(row2 - 2);
            if (row2 < 0) return;
            if (--rows == 0) return;
            if (dir < 0) { at |= 0xC000; out = at < step; at -= step; }
            else { const u32 n = at + step; out = n < at || n >= limit; at = n; }
        }
        at &= ~0xC000u;
        const u16 l = (u16)((u16)(at << 1) - (u16)(rows * (u16)light_per_row2()));
        seen(dir < 0 ? k_shallowup_leaves_left : k_shallowup_leaves_right);
        const u16 top = (u16)(((u16)row2 >> 1) + 1 - rows);
        at = (at & 0xFFFF0000u) | l;
        const u32 beside = (u32)rows * slope + (dir < 0 ? (0 - at) >> 16 : at >> 16);
        running(top, dir < 0 ? 0xFFFC : XA(0xFFF3, 0xFFF3), slope, l, (u32)rows << 24 | (beside & 0xFFFFFF));
    }

    // -----------------------------------------------------------------------------------------------------------
    // The clip to the window's rows, for both kinds of edge: which end the walk starts from (the upper one that is
    // in the window, else where the edge enters it: at the top going down, or at the bottom going up), the rows
    // it runs between (kRowFrom, kRowTo), the light there (kLightA; the lights are swapped when it starts from b),
    // and how far the edge has moved sideways by then.  The two kinds differ only in that last sum.
    // -----------------------------------------------------------------------------------------------------------
    enum Start { kAtPoint, kFarAbove, kJustAbove, kBelow };
    struct RowClip { u16 point; Start how; bool fromB; };

    RowClip clip_rows(u16 a, u16 b) {
        const u16 d = deep();
        const u16 aHi = m.d16((u16)(a + 6)), aLo = m.d16((u16)(a + 4)), bHi = m.d16((u16)(b + 6)), bLo = m.d16((u16)(b + 4));
        RowClip c = { a, kAtPoint, false };
        // the row of the far end as the game takes it: 0 when it is above (the depth's high byte), the depth when below
        bool bInside = false;
        if (aHi == 0 && aLo < d) {
            m.D8(kRowFrom, (u8)aLo);
            u8 to;
            if (bHi & 0x8000) to = (u8)(d >> 8);
            else if (bHi != 0 || bLo >= d || (u8)bLo >= (u8)aLo) to = (bHi != 0 || bLo >= d) ? (u8)d : (u8)bLo;
            else {
                // both in the window and b the upper: from b
                seen(k_clip_from_b);
                m.D8(kRowFrom, (u8)bLo); m.D8(kRowTo, (u8)aLo);
                c.point = b; c.fromB = true;
                return c;
            }
            m.D8(kRowTo, to);
            return c;
        }
        if (aHi == 0) bInside = bHi == 0 && bLo < d;           // a below the window
        else if (bHi & 0x8000) { c.point = b; c.fromB = true; c.how = bHi == 0xFFFF ? kJustAbove : kFarAbove; }
        else if (bHi != 0) c.how = aHi == 0xFFFF ? kJustAbove : kFarAbove;
        else if (bLo < d) bInside = true;
        else { c.point = b; c.fromB = true; c.how = kBelow; }
        if (bInside) {
            // from b, which is in the window, towards a outside it
            seen(k_clip_from_b_a_outside);
            m.D8(kRowFrom, (u8)bLo);
            u8 to;
            if (aHi & 0x8000) to = (u8)(d >> 8);
            else if (aHi != 0 || aLo >= d) to = (u8)d;
            else to = (u8)aLo;
            m.D8(kRowTo, to);
            c.point = b; c.fromB = true;
            return c;
        }
        if (aHi == 0) c.how = kBelow;       // a below, b not in the window: up from the bottom (c.point is a)
        return c;
    }

    // the light where the edge enters the window (the start point is `point`, outside)
    void light_at_entry(const RowClip &c) {
        const u16 yLo = m.d16((u16)(c.point + 4));
        const u16 step2 = (u16)light_per_row2();
        seen(c.how == kFarAbove ? k_clip_far_above : c.how == kJustAbove ? k_clip_just_above : k_clip_below);
        if (c.how == kFarAbove) m.D16(kLightA, (u16)(m.d16(kLightA) + (u16)((0 - m.d32((u16)(c.point + 4))) * (u32)(s32)light_per_row2())));
        else if (c.how == kJustAbove) m.D16(kLightA, (u16)(m.d16(kLightA) + (u16)((u16)((u16)((u16)(0 - yLo) >> 1) * step2) << 1)));
        else m.D16(kLightA, (u16)(m.d16(kLightA) - (u16)((u16)((u16)((u16)(yLo - deep()) >> 1) * step2) << 1)));
        if (c.how == kBelow) { m.D8(kRowFrom, (u8)deep()); m.D8(kRowTo, (u8)(deep() >> 8)); }
        else { m.D8(kRowFrom, (u8)(deep() >> 8)); m.D8(kRowTo, (u8)deep()); }
    }
    // x where the walk starts: the point's own, moved by `shift` against or with the edge's direction
    u32 entry_x(const RowClip &c, u32 shift) {
        if (c.fromB) turn_x();
        if (sign_x() < 0) shift = 0 - shift;
        return shift + m.d32(c.point);
    }

    // 1CCA:4875, a steep edge (slope = rows a pixel: slopeHi : whole . frac).  Gives 0 and kStartX when the walk
    // starts in the window's columns (or near enough), a negative number when it is left of them, a positive one
    // when right.  x = where it starts (a long).  `whole` comes back as FFFF for a slope past a word.
    int clip_steep(u16 a, u16 b, u16 slopeHi, u16 &whole, u16 frac, u32 &x) {
        const RowClip c = clip_rows(a, b);
        if (c.how == kAtPoint) {
            if (slopeHi) whole = 0xFFFF;
            if (c.fromB) { swap_lights(); turn_x(); }
            x = m.d32(c.point);
        } else {
            if (c.fromB) swap_lights();
            light_at_entry(c);
            const u16 yLo = m.d16((u16)(c.point + 4));
            if (c.how == kFarAbove) {
                const u32 rows = 0 - m.d32((u16)(c.point + 4));
                u32 columns;
                if (slopeHi) { seen(k_steep_far_above_long_slope); columns = fixed_div_round(rows, slopeHi, whole); whole = m.d16(kOddWord); }
                else columns = fixed_div_far(rows, whole, frac);
                x = entry_x(c, columns);
            } else if (slopeHi) {
                // (the game leaves x at the point's, and the direction unturned)
                seen(k_steep_outside_long_slope);
                whole = 0xFFFF;
                x = m.d32(c.point);
            } else {
                const u16 rows = c.how == kJustAbove ? (u16)(0 - yLo) : (u16)(yLo - deep());
                x = entry_x(c, fixed_div_round((u32)rows << 16, whole, frac));
            }
        }
        const u16 xHi = (u16)(x >> 16), xLo = (u16)x;
        if (xHi & 0x8000) {
            if (sign_x() < 0 || xHi != 0xFFFF) return (s16)xHi;
            const u16 reach = (u16)(0 - deep());
            if (reach >= xLo) return (s16)reach;
        } else if (xHi != 0) return (s16)xHi;
        else if (xLo != 0) {
            const u16 reach = (u16)(deep() + wide());
            if (reach < xLo) return (s16)reach;
            if (sign_x() >= 0 && xLo >= wide()) return (s16)reach;
        } else if (sign_x() < 0) return -1;
        m.D16(kStartX, xLo);
        return 0;
    }

    // 1CCA:4C1A, a shallow edge (slope = pixels a row, whole . frac).  0: kStartX = the column it starts at in the
    // window and `skip1` = 0, or (it starts beside the window but comes in) kStartX = the window's side and
    // `skip1` = the rows until then + 1.  Negative / positive: it stays left / right of the window.
    int clip_shallow(u16 a, u16 b, u16 whole, u16 frac, u16 &skip1) {
        const RowClip c = clip_rows(a, b);
        u32 x;
        if (c.how == kAtPoint) {
            if (c.fromB) { swap_lights(); turn_x(); }
            x = m.d32(c.point);
        } else {
            if (c.fromB) swap_lights();
            light_at_entry(c);
            const u16 yLo = m.d16((u16)(c.point + 4));
            u32 shift;
            if (c.how == kFarAbove) shift = (u32)(((Bit64u)(0 - m.d32((u16)(c.point + 4))) * ((u32)whole << 16 | frac)) >> 16);
            else {
                const u16 rows = c.how == kJustAbove ? (u16)(0 - yLo) : (u16)(yLo - deep());
                shift = (u32)rows * whole + (((u32)rows * frac) >> 16);
            }
            x = entry_x(c, shift);
        }
        m.D32(kStartX32, x);
        skip1 = 0;
        const u16 xHi = (u16)(x >> 16), xLo = (u16)x;
        u32 out;        // how far beside the window
        u16 side;
        if (xHi & 0x8000) {
            if (sign_x() < 0) return -1;
            out = 0 - x;
            // it comes in within 256 rows only if out / 256 is no more than the slope
            if ((out >> 24) != 0 || (u16)(out >> 8) > whole) return -1;
            side = 0;
        } else if (xHi != 0) {
            if (sign_x() >= 0) return 1;
            side = m.d16(kWideLess1);
            out = x - side;
            if ((out >> 24) != 0 || (u16)(out >> 8) >= whole) return 1;
        } else {
            if (xLo == 0 && sign_x() < 0) return -1;
            if (xLo < wide()) { m.D16(kStartX, xLo); return 0; }
            if (sign_x() >= 0) return 1;
            side = m.d16(kWideLess1);
            out = (u16)(xLo - side);
            if ((whole >> 8) == 0 && (u8)(out >> 8) > (u8)whole) return 1;
        }
        // the rows until it reaches the window's side
        seen(k_shallow_comes_in);
        u32 rows;
        if ((whole >> 8) == 0) rows = fixed_div_round(out << 16 | out >> 16, whole, frac);
        else rows = fixed_div_round((out & 0xFFFFFF) << 8, (u16)(whole >> 8), (u16)(whole << 8 | frac >> 8));
        m.D16(kStartX, side);
        skip1 = (u16)(rows + 1);
        return 0;
    }

    // -----------------------------------------------------------------------------------------------------------
    // A steep edge (1CCA:6055)
    // -----------------------------------------------------------------------------------------------------------

    // the part of it that is beside the window between kRowFrom and kRowTo, as one running record; x = where it
    // is at kRowFrom.  `upright`: an edge too steep to move a pixel in the window (its slope is written as FFFF).
    void steep_beside(Side side, u16 slope, u32 x, bool upright) {
        const u8 from = m.d8(kRowFrom), to = m.d8(kRowTo);
        if (from == to) return;
        const bool back = sign_x() < 0;
        if (from < to) {
            const u16 kind = side == kLeftSide ? (back ? XA(0xFFF9, 0xFFF9) : XA(0xFFFA, 0xFFFA)) : (back ? XA(0xFFF1, 0xFFF1) : XA(0xFFF2, 0xFFF2));
            seen(k_steepbeside_down);
            const u32 out = side == kLeftSide ? 0 - x : x;
            running(from, kind, slope, m.d16(kLightA), (u32)(u8)(to - from) << 24 | (out & 0xFFFFFF));
            return;
        }
        // upwards: the record starts at the top, so x and the light are taken there
        seen(k_steepbeside_up);
        const u8 rows = (u8)(from - to);
        u16 kind = side == kLeftSide ? XA(0xFFFA, 0xFFFA) : XA(0xFFF2, 0xFFF2);
        if (!upright) {
            u16 columns = (u16)(((u32)rows << 8) / slope);
            if (!back) { kind--; columns = (u16)(0 - columns); }
            x -= (u32)(s32)(s16)columns;
        }
        const u32 out = side == kLeftSide ? 0 - x : x;
        const u16 light = (u16)(m.d16(kLightA) - (u16)(rows * (u16)light_per_row2()));
        if (upright) {
            // (the game jumps past the lines that make the record's first long: it writes what ECX held)
            seen(k_STRAY_upright_beside_up);
            put12(to, (u32)strayHigh << 16 | 0xFFFF, (u32)light << 16 | id(), (u32)rows << 24 | (out & 0xFFFFFF));
            strayHigh = light;
        } else running(to, kind, slope, light, (u32)rows << 24 | (out & 0xFFFFFF));
    }

    void steep_edge(u16 a, u16 b, const Slope &s) {
        u16 whole = s.lo;
        const u16 frac = s.frac;
        u32 x;
        const int where = clip_steep(a, b, s.hi, whole, frac, x);
        if (where != 0) {
            const u16 slope = (whole >> 8) ? 0xFFFF : (u16)(whole << 8 | frac >> 8);
            steep_beside(where < 0 ? kLeftSide : kRightSide, slope, x, false);
            return;
        }
        if (whole >= deep()) {
            // upright in the window: one crossing for all its rows (or a record when it is just beside the window)
            seen(k_steep_upright);
            if ((s16)(u16)x < 0) { steep_beside(kLeftSide, 0xFFFF, x, true); return; }
            if ((u16)x >= wide()) { steep_beside(kRightSide, 0xFFFF, x, true); return; }
            u16 top = m.d8(kRowFrom), end = m.d8(kRowTo), light = m.d16(kLightA);
            if (top == end) return;
            if (top > end) {
                const u16 t = top; top = end; end = t;
                light = m.d16(kLightB);
                if (top == 0) light = (u16)(m.d16(kLightA) - (u16)(end * (u16)light_per_row2()));
            }
            put_tall(top, (u16)(end - top), light, m.d16(kStartX));
            return;
        }
        const u16 slope = (u16)(whole << 8 | frac >> 8);        // rows a pixel, 8.8
        // where it starts: in the window, or beside it by `out` pixels = `skip` rows (8.8) until it comes in
        const s16 startX = (s16)m.d16(kStartX);
        u16 x0, skip = 0;
        if (startX < 0) { x0 = 0; }
        else if ((u16)startX > m.d16(kWideLess1)) { x0 = m.d16(kWideLess1); }
        else x0 = (u16)startX;
        if (x0 != (u16)startX) {
            const u32 rows = (u32)(startX < 0 ? (u16)(0 - startX) : (u16)(startX - x0)) * slope;
            if (rows >> 16) { seen(k_steep_too_far_out); steep_beside(x0 == 0 ? kLeftSide : kRightSide, slope, (u32)(s32)startX, false); return; }
            skip = (u16)rows;
        }
        const Side side = x0 == 0 ? kLeftSide : kRightSide;
        const u8 from = m.d8(kRowFrom), to = m.d8(kRowTo);
        if (from == to) return;
        const bool down = to > from;
        u16 rem = (u16)((down ? to - from : from - to) << 8);
        if ((skip >> 8) == 0) {
            const u16 pos = (u16)(from << 8 | skip);
            if (skip) seen(k_steep_fraction_out);
            const int dir = sign_x() < 0 ? -1 : 1;
            if (down) steep_down(dir, x0, pos, m.d16(kLightA), slope, rem);
            else steep_up(dir, x0, pos, m.d16(kLightA), slope, rem);
            return;
        }
        // whole rows beside the window first: the walk from where it comes in (inwards), then the part beside as a record
        if (rem >= skip && ((u16)(rem - skip) >> 8) != 0) {
            seen(k_steep_comes_in);
            rem = (u16)(rem - skip);
            const u16 change = (u16)((skip >> 8) * (u16)light_per_row2());
            if (down) {
                const u16 light = (u16)(m.d16(kLightA) + change);
                const u8 row = (u8)((skip >> 8) + from);
                m.D8(kRowTo, row);
                if (side == kRightSide) m.D16(kLightB, light);
                steep_down(side == kLeftSide ? 1 : -1, x0, (u16)(row << 8 | (skip & 0xFF)), light, slope, rem);
            } else {
                const u16 light = (u16)(m.d16(kLightA) - change);
                const u16 pos = (u16)((from << 8) - skip);
                m.D16(kLightB, light);
                m.D8(kRowTo, (u8)(pos >> 8));
                steep_up(side == kLeftSide ? 1 : -1, x0, pos, light, slope, rem);
            }
        }
        seen(k_steep_starts_beside);
        steep_beside(side, slope, (u32)(s32)startX, false);
    }

    // -----------------------------------------------------------------------------------------------------------
    // A shallow edge (1CCA:63DD)
    // -----------------------------------------------------------------------------------------------------------

    // the part beside the window between kRowFrom and kRowTo as a running record; x in 9.7, or in whole pixels
    // when the slope is 512 a row or more
    void shallow_beside(Side side, u16 whole, u16 frac) {
        const bool coarse = whole >= 0x200;
        const u16 slope = coarse ? whole : (u16)(whole << 7 | frac >> 9);
        if (coarse) seen(k_shallowbeside_coarse);
        const u8 from = m.d8(kRowFrom), to = m.d8(kRowTo);
        if (from == to) return;
        const bool back = sign_x() < 0;
        u32 out = side == kLeftSide ? 0 - m.d32(kStartX32) : m.d32(kStartX32);
        if (!coarse) out <<= 7;
        const u16 kind = (u16)((side == kLeftSide ? 0xFFFC : 0xFFF4) - (coarse ? 4 : 0));
        if (from < to) {
            seen(k_shallowbeside_down);
            running(from, back ? (u16)(kind - 1) : kind, slope, m.d16(kLightA), (u32)(u8)(to - from) << 24 | (out & 0xFFFFFF));
            return;
        }
        // upwards: from the top row, where it is further on
        seen(k_shallowbeside_up);
        const u8 rows = (u8)(from - to);
        u32 run = (u32)rows * slope;
        if (!back) run = 0 - run;
        if (side == kLeftSide) { out += run; if (!coarse && (s32)out < 0) out = 0; }
        else out -= run;
        const u16 light = (u16)(m.d16(kLightA) - (u16)(rows * (u16)light_per_row2()));
        running(to, back ? kind : (u16)(kind - 1), slope, light, (u32)rows << 24 | (out & 0xFFFFFF));
    }

    // an edge of more than 65535 pixels a row: only where it cuts the window's left side matters, an upright
    // stretch at x 0 from there (the game's cases, of which several do nothing)
    void level_edge(u16 a, u16 b, const Slope &s) {
        const u16 aXHi = m.d16((u16)(a + 2)), bXHi = m.d16((u16)(b + 2));
        bool column = false;        // the stretch at x 0 over the edge's rows in the window
        seen(k_level_edge);
        if (aXHi == 0) column = sign_x() < 0;
        else if (aXHi == 0xFFFF) { if (bXHi != 0) return; column = sign_x() >= 0; }
        else if (bXHi == 0) column = sign_x() >= 0;
        else if (!(bXHi & 0x8000) || bXHi != 0xFFFF) {
            // one end far left, the other far right (or further left): the rows from the point `p` to x 0
            seen(k_level_edge_far);
            const bool fromA = !(bXHi & 0x8000);
            const u16 p = fromA ? a : b;
            const u32 rows = fixed_div_round(0 - m.d32(p), s.hi, s.lo);
            const bool upFromP = fromA ? sign_y() < 0 : sign_y() >= 0;
            const u16 light = m.d16(upFromP == fromA ? kLightB : kLightA);
            if (upFromP) {
                const u32 top = m.d32((u16)(p + 4)) - rows;
                if ((top >> 16) != 0 || (u16)top >= deep()) return;
                put_tall((u16)top, (u16)rows, light, 0);
            } else put_tall(m.d16((u16)(p + 4)), (u16)rows, light, 0);
            return;
        } else return;
        if (!column) return;
        // from the upper point down to the lower, cut to the window; the light counted on from above the window
        seen(k_level_edge_column);
        const u16 upper = sign_y() < 0 ? b : a, lower = sign_y() < 0 ? a : b;
        const u16 uHi = m.d16((u16)(upper + 6)), uLo = m.d16((u16)(upper + 4)), lHi = m.d16((u16)(lower + 6)), lLo = m.d16((u16)(lower + 4));
        u16 top = 0;
        if (!(uHi & 0x8000)) { if (uHi != 0 || uLo >= deep()) return; top = uLo; }
        if (lHi & 0x8000) return;
        const u16 end = (lHi == 0 && lLo < deep()) ? lLo : deep();
        u16 light = s.lo;       // (not a light: the game has the slope's word in the register here)
        if (top == 0) light = (u16)((u16)((0 - m.d32((u16)(upper + 4))) * (u32)(s32)light_per_row2()) + m.d16(sign_y() < 0 ? kLightB : kLightA));
        put_tall(top, (u16)(end - top), light, 0);
    }

    void shallow_edge(u16 a, u16 b, const Slope &s) {
        if (s.hi) { level_edge(a, b, s); return; }
        const u16 whole = s.lo, frac = s.frac;
        u16 skip1;
        const int where = clip_shallow(a, b, whole, frac, skip1);
        if (where != 0) { shallow_beside(where < 0 ? kLeftSide : kRightSide, whole, frac); return; }
        const u16 slope = whole >= 0x200 ? 0xFFFF : (u16)(whole << 7 | frac >> 9);      // pixels a row, 9.7
        const u16 startX = m.d16(kStartX);
        const u16 x = (u16)(((startX >> 1) & 0xFF) << 8 | (startX & 1) << 7 | startX >> 9);     // (x << 7, as the game makes it)
        const Side side = x == 0 ? kLeftSide : kRightSide;
        if (skip1 >> 8) { seen(k_shallow_too_far_out); shallow_beside(side, whole, frac); return; }
        const u8 from = m.d8(kRowFrom), to = m.d8(kRowTo);
        if (from == to) return;
        const bool down = to > from;
        u8 rows = (u8)(down ? to - from : from - to);
        if ((u8)skip1 == 0) {
            const int dir = sign_x() < 0 ? -1 : 1;
            if (down) shallow_down(dir, from, rows, x, slope, m.d16(kLightA), true);
            else shallow_up(dir, from, rows, x, slope, m.d16(kLightA), true);
            return;
        }
        // rows beside the window first: the walk from where it comes in, then the part beside as a record
        const u8 skip = (u8)(skip1 - 1);
        if (rows > skip) {
            seen(k_shallow_comes_in_walk);
            rows = (u8)(rows - skip);
            const u16 change = (u16)(skip * (u16)light_per_row2());
            if (down) {
                const u8 row = (u8)(skip + from);
                m.D8(kRowTo, row);
                shallow_down(side == kLeftSide ? 1 : -1, row, rows, x, slope, (u16)(m.d16(kLightA) + change), false);
            } else {
                const u8 row = (u8)(from - skip);
                const u16 light = (u16)(m.d16(kLightA) - change);
                m.D16(kLightB, light);
                m.D8(kRowTo, row);
                shallow_up(side == kLeftSide ? 1 : -1, row, rows, x, slope, light, false);
            }
        }
        shallow_beside(side, whole, frac);
    }

    // -----------------------------------------------------------------------------------------------------------
    // An edge's set-up
    // -----------------------------------------------------------------------------------------------------------

    // The light's step a row from the two ends' lights: into the edge's record (the scanline pass steps the light
    // of a crossing that stays with it) and, doubled, into kLightPerRow2.  A step of less than one unit a row
    // counts as none.
    u16 light_step_rows(u16 e) { return light_step_rows_of(m.d32((u16)(kAbsDy + 4 * e)), m.d8((u16)(kSignDy + e))); }
    u16 light_step_rows_of(u32 rows, u8 signDy) {
        const u16 a = m.d16(kLightA), b = m.d16(kLightB);
        const bool down = (s16)(u16)(b - a) < 0;
        u16 diff = (u16)(b - a);
        if (down) diff = (u16)(0 - diff);
        diff >>= 1;
        u16 step = 0;
        if ((u32)diff > rows) {
            step = (u16)rows ? (u16)(diff / (u16)rows) : diff;
            if (step >> 8) step &= 0xFFFE;
            if (((down ? 0xFF : 0) ^ signDy) & 0x80) step = (u16)(0 - step);
        }
        m.D16(kLightPerRow2, (u16)(step << 1));
        return (u16)((u16)(step << 1) >> 1);
    }
    // ... and a pixel, for a steep edge (kLightPerPixel)
    void light_step_pixels(u16 e) { light_step_pixels_of(m.d32((u16)(kAbsDx + 4 * e)), m.d8((u16)(kSignDx + e))); }
    void light_step_pixels_of(u32 pixels, u8 signDx) {
        const u16 a = m.d16(kLightA), b = m.d16(kLightB);
        const bool down = (s16)(u16)(b - a) < 0;
        u16 diff = (u16)(b - a);
        if (down) diff = (u16)(0 - diff);
        u16 step = 0;
        if ((u32)diff > pixels) {
            step = (u16)pixels ? (u16)(diff / (u16)pixels) : diff;
            step &= 0xFFFC;
            if (((down ? 0xFF : 0) ^ signDx) & 0x80) step = (u16)(0 - step);
        }
        m.D16(kLightPerPixel, step);
    }

    // The slope of |dx|, |dy|: steep (rows a pixel) when the edge has more than two rows a pixel, else pixels a row.
    Slope slope_of(u32 dx, u32 dy, bool &steep) {
        const u32 half = dy >> 1;
        steep = half >= dx;
        if (half == dx) { const Slope two = { 0, 2, 0 }; return two; }
        return steep ? slope_div(dy, dx) : slope_div(dx, dy);
    }
    // ... of an edge, kept in its tables for the face on its other side (flag 40; flag 80 = steep)
    Slope edge_slope(u16 e) {
        Slope s;
        if (flags(e) & 0x40) {
            s.frac = m.d16((u16)(kSlopeFrac + 2 * e)); s.lo = m.d16((u16)(kSlopeLo + 2 * e)); s.hi = m.d16((u16)(kSlopeHi + 2 * e));
            return s;
        }
        set_flags(e, 0x40);
        bool steep;
        s = slope_of(m.d32((u16)(kAbsDx + 4 * e)), m.d32((u16)(kAbsDy + 4 * e)), steep);
        m.D16((u16)(kSlopeFrac + 2 * e), s.frac); m.D16((u16)(kSlopeLo + 2 * e), s.lo); m.D16((u16)(kSlopeHi + 2 * e), s.hi);
        if (steep) set_flags(e, 0x80);
        return s;
    }

    // what every traced edge begins with: counted, its points' pointers put right (flag 10: kept complemented)
    void begin_edge(u16 e, u8 f) {
        m.D16(kTraced, (u16)(m.d16(kTraced) + 1));
        if (f & 0x10) {
            m.D16((u16)(kPointA + 2 * e), (u16)~m.d16((u16)(kPointA + 2 * e)));
            m.D16((u16)(kPointB + 2 * e), (u16)~m.d16((u16)(kPointB + 2 * e)));
        }
    }
    // a point's row cut to the window (0 above it, the depth below)
    u16 row_in_window(u16 p) const {
        const u16 hi = m.d16((u16)(p + 6)), lo = m.d16((u16)(p + 4));
        if (hi & 0x8000) return 0;
        return (hi != 0 || lo >= deep()) ? deep() : lo;
    }

    // -----------------------------------------------------------------------------------------------------------
    // An edge wholly left of the window (flag 02), or right of it (flag 01; traced only for a shaded face: the
    // scanline pass needs it for the light).  Left: a face without shading gets a plain upright stretch at x 0.
    // Else one running record over the edge's rows in the window, in the fixed point that fits its slope:
    //   shallow: x in 9.7 and the slope in 9.7 a row, or both in whole pixels when the slope is 512 or more
    //   steep:   x in whole pixels and the slope in rows a pixel (8.8; FFFF when more)
    // -----------------------------------------------------------------------------------------------------------
    void edge_beside(u16 e, u8 f, Side side) {
        begin_edge(e, f);
        const bool shaded = m.d8(kGouraud) != 0;
        const u16 slot = edge_slot(e);
        m.T16(slot, m.d16(kFace));
        m.T16((u16)(slot + 2), (side == kRightSide || shaded) ? light_step_rows(e) : 0);
        m.D8(kSignX, m.d8((u16)(kSignDx + e)));
        m.D8(kSignY, m.d8((u16)(kSignDy + e)));
        // from the upper end
        u16 p = m.d16((u16)(kPointA + 2 * e)), top = row_in_window(p), end = row_in_window(m.d16((u16)(kPointB + 2 * e)));
        u16 light = m.d16(kLightA);
        if (top == end) return;
        if (top > end) {
            const u16 t = top; top = end; end = t;
            turn_x();
            p = m.d16((u16)(kPointB + 2 * e));
            light = m.d16(kLightB);
        }
        if (side == kLeftSide && !shaded) {
            seen(k_left_flat);
            put_tall(top, (u16)(end - top), p, 0);      // (the "light" is the point's address: the game's register; not used)
            return;
        }
        const u32 above = 0 - m.d32((u16)(p + 4));      // the rows from the point down to row 0, when it is above
        if (top == 0) light = (u16)(light + (u16)(above * (u32)(s32)light_per_row2()));
        const u16 rows = (u16)(end - top);
        const Slope s = edge_slope(e);
        const bool back = sign_x() < 0;
        const bool grows = side == kLeftSide ? back : !back;      // (how far out it is grows downwards)
        u32 out = side == kLeftSide ? 0 - m.d32(p) : m.d32(p);
        u16 kind, slopeWord;
        if (!(flags(e) & 0x80)) {
            const u32 slope = ((u32)s.hi << 16 | s.lo) << 7 | s.frac >> 9;      // pixels a row, 25.7
            seen(side == kLeftSide ? k_beside_shallow_left : k_beside_shallow_right);
            if (top == 0) seen(k_beside_shallow_from_above);
            out <<= 7;
            if (top == 0) out = grows ? out + above * slope : out - above * slope;
            kind = (u16)((side == kLeftSide ? 0xFFFC : 0xFFF4) - (back ? 1 : 0));
            if (slope >> 16) { seen(k_beside_shallow_coarse); kind = (u16)(kind - 4); slopeWord = (u16)(slope >> 7); out >>= 7; }
            else slopeWord = (u16)slope;
        } else {
            const u32 slope = ((u32)s.hi << 16 | s.lo) << 8 | s.frac >> 8;      // rows a pixel, 24.8
            seen(side == kLeftSide ? k_beside_steep_left : k_beside_steep_right);
            if (top == 0) seen(k_beside_steep_from_above);
            if (slope >> 16) seen(k_beside_steep_long_slope);
            if (top == 0) {
                const u32 columns = slope ? (u32)(((Bit64u)above << 8) / slope) : 0;
                out = grows ? out + columns : out - columns;
            }
            kind = (u16)((side == kLeftSide ? XA(0xFFFA, 0xFFFA) : XA(0xFFF2, 0xFFF2)) - (back ? 1 : 0));
            slopeWord = (slope >> 16) ? 0xFFFF : (u16)slope;
        }
        running(top, kind, slopeWord, light, (u32)(u8)rows << 24 | (out & 0xFFFFFF));
    }

    // -----------------------------------------------------------------------------------------------------------
    // An edge that crosses the window (flag 08)
    // -----------------------------------------------------------------------------------------------------------
    void edge_crossing(u16 e, u8 f) {
        begin_edge(e, f);
        const bool shaded = m.d8(kGouraud) != 0;
        const u16 step = shaded ? light_step_rows(e) : 0;
        const u16 slot = edge_slot(e);
        m.T16(slot, m.d16(kFace));
        m.T16((u16)(slot + 2), step);
        m.D8(kSignX, m.d8((u16)(kSignDx + e)));
        m.D8(kSignY, m.d8((u16)(kSignDy + e)));
        const Slope s = edge_slope(e);
        const u16 a = m.d16((u16)(kPointA + 2 * e)), b = m.d16((u16)(kPointB + 2 * e));
        if (!(flags(e) & 0x80)) { shallow_edge(a, b, s); return; }
        if (shaded) light_step_pixels(e);
        steep_edge(a, b, s);
    }

    // -----------------------------------------------------------------------------------------------------------
    // A face cut by the near plane (some of its vertices behind, not all): the cut leaves the face open between
    // the two points where its outline meets the plane.  They are joined by an edge of the face's own, under the
    // number of an edge that needed no tracing (off screen or behind).  A shaded face is made flat first (its
    // material's middle shade): there are no lights for the cut.
    // -----------------------------------------------------------------------------------------------------------
    u16 other_end(u16 e, u16 point) const {
        const u16 a = m.d16((u16)(kPointA + 2 * e));
        return a == point ? m.d16((u16)(kPointB + 2 * e)) : a;
    }
    void take_spare_edge() {
        if (m.d16(kSpareEdge) == 0) {
            // none met: the first edge of the object that is neither traced, nor left, nor crossing
            seen(k_cut_no_spare_edge);
            const u16 n = m.d16(kEdges);
            u16 e = 0;
            u16 left = n;
            bool found = false;
            do { if (!(flags(e) & 0x2A)) { found = true; break; } e++; } while (--left);
            m.D16(kSpareEdge, found ? e : (u16)(n - 1));
        }
        m.D8(kId, m.d8(kSpareEdge));
    }
    void give_spare_edge() {
        const u16 e = m.d16(kSpareEdge);
        m.D16(kSpareEdge, 0);
        set_flags(e, 0x20);
        m.T16(edge_slot(e), m.d16(kFace));
    }

    void close_cut_face(const u8 *face) {
#ifndef T640_XWING
        if (m.d8(kGouraud)) {
            const u16 at = (u16)(m.d16(kRecord) + 2 * m.d16(kFace) + 0x65);
            m.T8(at, m.cs ? m.cs[(u16)(kShadesMiddle + 16 * m.t8(at))] : m.t8(at));
            m.D8(kGouraud, 0);
            m.D8(kShadeMode, 7);
            seen(k_cut_shaded_made_flat);
        }
#endif
        // the two cut points: where the outline goes behind the plane and where it comes back
        auto point = [&](int i) { return m.d16((u16)(kVertexPoint + 2 * face[i])); };
        int i = 0;
        u16 from, to;
        if (point(0)) {
            seen(k_cut_first_vertex_in_front);
            do i += 2; while (point(i));
            from = other_end(face[i - 1], point(i - 2));
            do i += 2; while (!point(i));
            to = other_end(face[i - 1], point(i));
        } else {
            do i += 2; while (!point(i));
            seen(k_cut_first_vertex_behind);
            from = other_end(face[i - 1], point(i));
            do i += 2; while (point(i));
            to = other_end(face[i - 1], point(i - 2));
        }
        m.D16(kCutPoint, from);
#ifdef T640_XWING
        close_cut_face_xwing(from, to);
    }

    // X-Wing's closing edge (23CF:5FA1..6740), an earlier build's: a shaded face stays shaded (the lights are the
    // words before the two cut points, stepped along the closing edge), and an edge wholly beside the window is one
    // running record there (right of it: for a shaded face only), as the tracers' own edges beside it are.
    void cut_beside(Side side, u16 top, u16 rows, u16 point, u16 light, u32 dx, u32 dy) {
        const u32 half = dy >> 1;
        const s32 x = (s32)m.d32(point), y = (s32)m.d32((u16)(point + 4));
        const bool back = (s8)m.d8(kSignX) < 0;
        // (left of the window the edge's way in is away from x 0 turned round)
        const bool add = side == kLeftSide ? back : !back;
        u32 out = side == kLeftSide ? (u32)(0 - x) : (u32)x;
        u16 kind, slopeWord;
        if (half < dx) {
            // pixels a row, 7 bits of fraction
            const Slope s = slope_div(dx, dy);
            const u32 slope7 = ((u32)s.hi << 16 | s.lo) << 7 | s.frac >> 9;
            out <<= 7;
            if (top == 0) { const u32 moved = (u32)(0 - y) * slope7; out = add ? out + moved : out - moved; }
            kind = side == kLeftSide ? (back ? 0xFFFB : 0xFFFC) : (back ? 0xFFF3 : 0xFFF4);
            if (slope7 >> 16) { kind = (u16)(kind - 4); slopeWord = (u16)(slope7 >> 7); out >>= 7; }
            else slopeWord = (u16)slope7;
            seen(k_cut_shallow);
        } else {
            // rows a pixel, 8 bits of fraction
            const Slope two = { 0, 2, 0 };
            const Slope s = half == dx ? two : slope_div(dy, dx);
            const u32 slope8 = ((u32)s.hi << 16 | s.lo) << 8 | s.frac >> 8;
            if (top == 0) { const u32 moved = slope8 ? (u32)(((Bit64u)(u32)(0 - y) << 8) / slope8) : 0; out = add ? out + moved : out - moved; }
            slopeWord = (slope8 >> 16) ? 0xFFFF : (u16)slope8;
            kind = side == kLeftSide ? (back ? 0xFFF9 : 0xFFFA) : (back ? 0xFFF1 : 0xFFF2);
            seen(k_cut_steep);
        }
        running(top, kind, slopeWord, light, (u32)(u8)rows << 24 | (out & 0xFFFFFF));
    }
    void close_cut_face_xwing(u16 from, u16 to) {
        enum : u16 { kCutDx = 0x329C, kCutDy = 0x32A0 };
        m.D16(kLightA, m.d16((u16)(from - 2)));
        m.D16(kLightB, m.d16((u16)(to - 2)));
        const bool shaded = m.d8(kGouraud) != 0;
#endif
        // the edge from `from` to `to`: nothing to do when it is level, or not in the window's rows or columns
        const s32 fromX = (s32)m.d32(from), fromY = (s32)m.d32((u16)(from + 4)), toX = (s32)m.d32(to), toY = (s32)m.d32((u16)(to + 4));
        m.D8(kSignY, 1);
        if (toY == fromY) return;
        if (toY < fromY) m.D8(kSignY, 0xFF);
        {
            const s32 upperY = toY < fromY ? toY : fromY, lowerY = toY < fromY ? fromY : toY;
            if (lowerY < 0) return;
            if (upperY >= 0 && ((u32)upperY >> 16 || (u16)upperY >= deep())) return;
        }
        const u32 dy = toY < fromY ? (u32)fromY - (u32)toY : (u32)toY - (u32)fromY;
#ifdef T640_XWING
        m.D8(kSignX, 1);
        int where = 0;              // -1 wholly at or left of x 0, +1 wholly right of the window
        u32 dx = (u32)toX - (u32)fromX;
        if (toX != fromX) {
            if (toX < fromX) { m.D8(kSignX, 0xFF); dx = 0 - dx; }
            const s32 rightX = toX < fromX ? fromX : toX, leftX = toX < fromX ? toX : fromX;
            if (rightX <= 0) where = -1;
            else if (leftX >= 0 && ((u32)leftX >> 16 || (u16)leftX >= wide())) where = 1;
        }
        if (where > 0 && !shaded) return;
        m.D32(kCutDx, dx); m.D32(kCutDy, dy);
        take_spare_edge();
        const u16 slot = edge_slot(m.d16(kSpareEdge));
        const u16 step = shaded ? light_step_rows_of(dy, m.d8(kSignY)) : 0;
        m.T16(slot, m.d16(kFace));
        m.T16((u16)(slot + 2), step);
        if (where != 0) {
            u16 top = row_in_window(from), end = row_in_window(to), point = from, light = m.d16(kLightA);
            if (top != end) {
                if (top > end) { const u16 t = top; top = end; end = t; m.D8(kSignX, (u8)(0 - m.d8(kSignX))); point = to; light = m.d16(kLightB); }
                if (!shaded) {
                    seen(k_cut_column);
                    put_tall(top, (u16)(end - top), point, 0);      // (the point's address is in the register the light is taken from)
                } else {
                    if (top == 0) light = (u16)(light + (u16)((u32)(0 - m.d32((u16)(point + 4))) * (u32)(s32)(s16)m.d16(kLightPerRow2)));
                    cut_beside(where < 0 ? kLeftSide : kRightSide, top, (u16)(end - top), point, light, dx, dy);
                }
            }
        } else {
            bool steep;
            const Slope s = slope_of(dx, dy, steep);
            seen(steep ? k_cut_steep : k_cut_shallow);
            if (steep) { if (shaded) light_step_pixels_of(dx, m.d8(kSignX)); steep_edge(from, to, s); }
            else shallow_edge(from, to, s);
        }
        const u16 e = m.d16(kSpareEdge);
        m.D16(kSpareEdge, 0);
        set_flags(e, 0x20);
    }
#else
        m.D8(kSignX, 1);
        bool column = false;        // the edge is wholly at or left of x 0
        u32 dx = (u32)toX - (u32)fromX;
        if (toX != fromX) {
            if (toX < fromX) { m.D8(kSignX, 0xFF); dx = 0 - dx; }
            const s32 rightX = toX < fromX ? fromX : toX, leftX = toX < fromX ? toX : fromX;
            if (rightX <= 0) column = true;
            else if (leftX >= 0 && ((u32)leftX >> 16 || (u16)leftX >= wide())) return;
        }
        take_spare_edge();
        if (column) {
            u16 top = row_in_window(to), end = row_in_window(from);
            if (top > end) { const u16 t = top; top = end; end = t; }
            seen(k_cut_column);
            put_tall(top, (u16)(end - top), (u16)dy, 0);        // (again a register as the light)
        } else {
            bool steep;
            const Slope s = slope_of(dx, dy, steep);
            seen(steep ? k_cut_steep : k_cut_shallow);
            if (steep) steep_edge(from, to, s); else shallow_edge(from, to, s);
        }
        give_spare_edge();
    }
#endif

    // -----------------------------------------------------------------------------------------------------------
    // trace2_drawface (1CCA:53F3)
    // -----------------------------------------------------------------------------------------------------------
    void face(const u8 *body, u16 vertices) {
        seen(m.d8(kGouraud) ? k_face_shaded : k_face_flat);
        m.D8(kCountdown, (u8)vertices);
        m.D16(kCutPoint, vertices);
        m.D16(kBehind, 0);
        const u8 *at = body;
        do {
            const u16 e = at[1];
            // the lights at the edge's two ends, in the order of the edge's own points
            m.D16(kLightB, m.d16((u16)(kVertexLight + 2 * at[2])));
            m.D16(kLightA, m.d16((u16)(kVertexLight + 2 * at[0])));
            const u16 point = m.d16((u16)(kVertexPoint + 2 * at[0]));
            if (point == m.d16((u16)(kPointB + 2 * e))) swap_lights();
            if (!point) m.D16(kBehind, (u16)(m.d16(kBehind) + 1));
            m.D8((u16)(kId + 1), m.d8(kObject));
            m.D8(kId, (u8)e);
            const u8 f = flags(e);
            bool spare = false;
            if (f == 0x80 || f == 0xA0 || (f & 4)) spare = true;            // behind, or off screen: nothing to trace
            else if (f & 0x20) m.T8((u16)(edge_slot(e) + 1), m.d8(kFace));  // traced by the face on its other side: this is its second face
            else {
                set_flags(e, 0x20);
                if (f & 1) { if (m.d8(kGouraud)) edge_beside(e, f, kRightSide); else spare = true; }
                else if (f & 2) edge_beside(e, f, kLeftSide);
                else if (f & 8) edge_crossing(e, f);
            }
            if (spare && !(f & 0x20)) m.D16(kSpareEdge, e);
            at += 2;
            m.D8(kCountdown, (u8)(m.d8(kCountdown) - 1));
        } while (m.d8(kCountdown));
        const u16 behind = m.d16(kBehind);
        if (behind && behind != m.d16(kCutPoint)) close_cut_face(body);
    }
};

// ---------------------------------------------------------------------------------------------------------------
// The other three routines that put things into the rows' lists: flat polygons, thick lines, the sprites' rows.
// They share the tracers above; what is their own is below.
// ---------------------------------------------------------------------------------------------------------------
enum : u16 {
    // the flat objects' tables ([n] or [2n], n = the flat object's number; its object byte is 80h + n)
    kFlatNext = XA(0x26E3, 0x321B),         // the next flat object's number (77h = none left)
    kFlatColour = XA(0x410C, 0x4C51), kFlatComponent = XA(0x428C, 0x4DD1), kFlatParent = XA(0x418C, 0x4CD1),
    kFlatX = XA(0x3E0C, 0x4951), kFlatY = XA(0x3F0C, 0x4A51), kFlatZ = XA(0x400C, 0x4B51),
    kParent = XA(0x2480, 0x2FB8), kIdLow = XA(0x2484, 0x2FBC),
    // a flat polygon's screen points: a ring of {long x, long y}
    kRingFirst = XA(0x2452, 0x2F8A), kRingEnd = XA(0x2454, 0x2F8C),
    kMinX = XA(0x2456, 0x2F8E), kMaxX = XA(0x2458, 0x2F90), kMinY = XA(0x245A, 0x2F92), kMaxY = XA(0x245C, 0x2F94),     // pointers to the extreme points
    kPoints = XA(0x2472, 0x2FAA), kSameRow = XA(0x2460, 0x2F98), kSameColumn = XA(0x2462, 0x2F9A),          // how many points; how many share a row / a column
    kColour = XA(0x2447, 0x2F7F),
    // a thick line
    kLineSide = XA(0x2726, 0x325E),         // byte: which way the part left of the window leans
    kLineSignY = XA(0x2727, 0x325F), kLineSignX = XA(0x2728, 0x3260),       // bytes: +1 / -1, the line's direction from its first point
    kLineSlope = XA(0x2729, 0x3261),        // three words: the slope kept between the line's two edges
    kLineState = XA(0x272F, 0x3267),        // byte: what the slope kept is (0 none yet, 1 steep, FF shallow, 2 a level line)
    kThick = XA(0x2730, 0x3268), kLineP = XA(0x2732, 0x326A),
    kCapLightP = XA(0x2734, 0x326C), kCapLightQ = XA(0x2736, 0x326E),       // the lights of the two end caps
    kLightP = XA(0x2738, 0x3270), kLightQ = XA(0x273A, 0x3272),             // the two ends' lights (inputs; reworked)
    kAcrossStep = XA(0x273E, 0x3276),       // twice the light's step a row across the line's thickness
    kLineDx = XA(0x2740, 0x3278), kLineDy = XA(0x2744, 0x327C),             // longs
    // a sprite's rows
    kBoxLeft = XA(0x48B2, 0x5410), kBoxTop = XA(0x48B4, 0x5412), kBoxRight = XA(0x48B6, 0x5414), kBoxBottom = XA(0x48B8, 0x5416),   // the dirty box (y upwards); then left, top row, width, rows
    kRowStart = XA(0x48BA, 0x5418), kSpriteMap = XA(0x48A2, 0x5400), kDeepLess1 = XA(0x48D3, 0x5437),
    kSpriteX = XA(0xC448, 0xC282), kSpriteY = XA(0xC44C, 0xC286), kSpriteZ = XA(0xC458, 0xC28A),                         // longs: where the sprite is
    kRowOffsets = XA(0xCFE0, 0xD400),       // in the CODE segment: [2 * row] the row's offset in the background buffer
};
enum { kNoFlatLeft = XA(0x77, 0x6F) };

struct Shapes : Tracer {
    int kind;       // what the call turned out to be (for the check's table)
    u16 strayEaxHigh;   // the high half of EAX at the flat polygon's entry (a level line's crossings get it as their x: a game bug)
    explicit Shapes(TraceMem &mem) : Tracer(mem), kind(0), strayEaxHigh(0) {}

    u16 xlo(u16 p) const { return m.d16(p); }
    u16 xhi(u16 p) const { return m.d16((u16)(p + 2)); }
    u16 ylo(u16 p) const { return m.d16((u16)(p + 4)); }
    u16 yhi(u16 p) const { return m.d16((u16)(p + 6)); }
    s32 x32(u16 p) const { return (s32)m.d32(p); }
    s32 y32(u16 p) const { return (s32)m.d32((u16)(p + 4)); }
    static bool neg(u16 hi) { return (hi & 0x8000) != 0; }

    // The routines' own writer of one crossing (1CCA:50FF, A26E): as put4, but it gives the CHUNK the crossing
    // went into.  `slot` = the row's place in the rows' table.
    u16 put4_chunk(u16 slot, u32 entry) {
        u16 at = m.d16(slot);
        if (!at) { at = new_chunk(); m.D16(slot, at); m.T16(at, 0); }
        for (;;) {
            const u16 used = m.t16(at);
            if (used < 0x7C) break;
            if (used == 0x7C) { seen(k_own_put4_new_chunk); const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 4); m.T32((u16)(n + 4), entry); return n; }
            at = used;
        }
        m.T8(at, (u8)(m.t8(at) + 4));
        m.T32((u16)(at + m.t16(at)), entry);
        return at;
    }
    // ... and a second crossing straight into that chunk (1CCA:5187).  (A link is followed once and the chunk
    // there written without a look at its count: it cannot happen, the chunk is the row's last.)
    void put4_into(u16 at, u32 entry) {
        const u16 used = m.t16(at);
        if (used == 0x7C) { seen(k_own_put4_into_full); const u16 n = new_chunk(); m.T16(at, n); m.T16(n, 4); m.T32((u16)(n + 4), entry); return; }
        if (used > 0x7C) { seen(k_own_put4_into_link); at = used; }
        m.T8(at, (u8)(m.t8(at) + 4));
        m.T32((u16)(at + m.t16(at)), entry);
    }

    // =========================================================================================================
    // trace2_drawscreencoords (1CCA:4FF3): a flat polygon from the ring of screen points.  It becomes a flat
    // object (the next number; its colour, component and parent into the flat tables), and its outline goes into
    // the lists under the id (kIdLow's byte, 80h + number).  Gives the game's AX: 0 = not in the window or no
    // number left, 1 = taken.
    // The lights of what it writes are whatever the edge before left in kLightA.. (a flat object has none).
    // =========================================================================================================

    // the polygon's box (its four extreme points) against the window
    bool flat_in_window() const {
        if (neg(yhi(m.d16(kMaxY))) || neg(xhi(m.d16(kMaxX)))) return false;
        u16 p = m.d16(kMinY);
        if (!neg(yhi(p)) && (yhi(p) != 0 || ylo(p) >= deep())) return false;
        p = m.d16(kMinX);
        if (!neg(xhi(p)) && (xhi(p) != 0 || xlo(p) >= wide())) return false;
        return true;
    }
    // the next flat object's number taken, and the id of what is written from here on
    u16 take_flat(u8 colour) {
        const u16 n = m.d16(kFlatNext);
        m.D16(kFlatNext, (u16)(n + 1));
        m.D8((u16)(kFlatColour + n), colour);
        m.D8((u16)(kFlatComponent + n), m.d8(kObject));
        m.D16((u16)(kFlatParent + 2 * n), m.d16(kParent));
        return n;
    }
    void flat_id(u16 n) {
        m.D8((u16)(kId + 1), (u8)(n + 0x80));
        m.D8(kId, m.d8(kIdLow));
    }

    // all the points in one column: two upright stretches, at x and x + 1.  (The "lights" are the game's
    // registers: the lowest point's address, then the pointer the first write left; and the second count has the
    // first light's low byte above the rows.)
    void flat_upright_line() {
        u16 p = m.d16(kMinY);
        const u16 top = neg(yhi(p)) ? 0 : ylo(p);
        p = m.d16(kMaxY);
        const u16 end = (yhi(p) == 0 && ylo(p) < deep()) ? ylo(p) : deep();
        const u16 rows = (u16)(end - top), x = xlo(p);
        if (!rows) return;
        const u16 after = put_tall(top, rows, p, x);
        put_tall(top, (u16)(((p >> 1) & 0xFF) << 8 | (rows & 0xFF)), after, (u16)((x & 0x1FF) + 1));
    }

    // all the points in one row: meant as a crossing at its left end and, when its right end is in the window, one
    // there.  Game bug, kept: the two shifts that should put x << 7 into the crossing's high word are 16-bit ones
    // (1CCA:50F0, 5181: no operand-size prefix), so the x is lost and both crossings get the high half of EAX as
    // the caller left it.
    void flat_level_line() {
        const u32 entry = (u32)strayEaxHigh << 16 | id();
        const u16 slot = (u16)(kRows + (u16)(ylo(m.d16(kMinY)) << 1));
        const u16 chunk = put4_chunk(slot, entry);
        const u16 p = m.d16(kMaxX);
        if (xhi(p) != 0 || xlo(p) >= wide()) { seen(k_flat_level_line_one_end); return; }
        put4_into(chunk, entry);
    }

    // one edge of the outline, from `a` to the next point `b`
    void flat_edge(u16 a, u16 b) {
        // rows: nothing for a level edge, or one wholly above or below the window
        const s32 ya = y32(a), yb = y32(b);
        m.D8(kSignY, 1);
        if (yb == ya) return;
        const u16 upper = yb < ya ? b : a, lower = yb < ya ? a : b;
        if (yb < ya) m.D8(kSignY, 0xFF);
        if (neg(yhi(lower))) return;
        if (!neg(yhi(upper)) && (yhi(upper) != 0 || ylo(upper) >= deep())) return;
        u32 dy = yb < ya ? (u32)ya - (u32)yb : (u32)yb - (u32)ya;
        // columns
        const s32 xa = x32(a), xb = x32(b);
        m.D8(kSignX, 1);
        if (xb == xa) {
            // upright: one stretch from its upper end, for all its rows (not cut to the window's depth unless
            // they are 256 or more)
            u16 top = ylo(upper);
            if (yhi(upper) != 0) { dy += m.d32((u16)(upper + 4)); top = 0; }
            const u16 rows = ((dy >> 16) != 0 || ((u16)dy >> 8) != 0) ? deep() : (u16)dy;
            if (!rows) return;
            u16 x = 0;
            if (!neg(xhi(a))) { if (xhi(a) != 0 || xlo(a) >= wide()) return; x = xlo(a); }
            seen(k_flat_edge_upright);
            put_tall(top, rows, (u16)dy, x);        // (the "light" is the rows' register)
            return;
        }
        const u16 right = xb < xa ? a : b, left = xb < xa ? b : a;
        if (xb < xa) m.D8(kSignX, 0xFF);
        const u32 dx = xb < xa ? (u32)xa - (u32)xb : (u32)xb - (u32)xa;
        if (neg(xhi(right)) || (xhi(right) == 0 && xlo(right) == 0)) {
            // wholly at or left of x 0: an upright stretch there over its rows in the window
            const u16 from = row_in_window(a), to = row_in_window(b);
            m.D8(kRowFrom, (u8)from); m.D8(kRowTo, (u8)to);
            const u8 f = (u8)from, t = (u8)to;
            if (f == t) return;
            seen(k_flat_edge_column);
            put_tall(f < t ? f : t, (u16)(f < t ? t - f : f - t), (u16)dy, 0);
            return;
        }
        if (!neg(xhi(left)) && (xhi(left) != 0 || xlo(left) >= wide())) return;      // wholly right of the window
        bool steep;
        const Slope s = slope_of(dx, dy, steep);
        seen(steep ? k_flat_edge_steep : k_flat_edge_shallow);
        if (steep) steep_edge(a, b, s); else shallow_edge(a, b, s);
    }

    u16 flat_polygon() {
        if (!flat_in_window()) { seen(k_flat_outside); kind = 0; return 0; }
        if (m.d16(kFlatNext) == kNoFlatLeft) { seen(k_flat_none_left); kind = 0; return 0; }
        const u16 n = take_flat(m.d8(kColour));
        m.D16((u16)(kFlatZ + 2 * n), XA(0x8001, 0x8001));       // (no place of its own: the caller may write one)
        flat_id(n);
        const u8 points = (u8)m.d16(kPoints);
        if (points == m.d8(kSameColumn)) { seen(k_flat_upright_line); kind = 1; flat_upright_line(); return 1; }
        if (points == m.d8(kSameRow)) { seen(k_flat_level_line); kind = 2; flat_level_line(); return 1; }
        seen(k_flat_outline); kind = 3;
        // round the ring from the top point
        const u16 first = m.d16(kMinY);
        u16 b = first;
        do {
            const u16 a = b;
            b = (u16)(b + 8);
            if (b == m.d16(kRingEnd)) b = m.d16(kRingFirst);
            flat_edge(a, b);
        } while (b != first);
        return 1;
    }

    // =========================================================================================================
    // drawln2_tracelineedges (1CCA:2BB0): a thick line from P (kLineP) to Q, kThick pixels thick, as the outline
    // of a flat object of its own: a line that is wider than tall gets its upper and lower edge (moved up and
    // down by half the thickness) and an upright cap at each end; a taller one its left and right edge.  The
    // ends' lights (kLightP, kLightQ) run along the edges; when they are the same the light is taken as running
    // ACROSS the thickness instead.  The two points are moved about as it goes (they are left changed).
    // =========================================================================================================

    // a light's step from kLightA to kLightB over `n` rows or pixels, doubled (the tracers' form); a step of
    // less than one a row counts as none
    u16 line_light_step(u32 n, u8 sign) {
        u16 diff = (u16)(m.d16(kLightB) - m.d16(kLightA));
        const bool down = neg(diff);
        if (down) diff = (u16)(0 - diff);
        diff >>= 1;
        u16 step = 0;
        if ((u32)diff > n) {
            step = (u16)n ? (u16)(diff / (u16)n) : diff;
            if (step >> 8) step &= 0xFFFE;
            if (down != ((sign & 0x80) != 0)) step = (u16)(0 - step);
        }
        return (u16)(step << 1);
    }
    static u16 not_below_0(u16 light) { return neg(light) ? 0 : light; }
    void line_lights_again() {
        m.D16(kLightA, not_below_0(m.d16(kLightP)));
        m.D16(kLightB, not_below_0(m.d16(kLightQ)));
    }
    void line_signs_again() {
        m.D8(kSignY, m.d8(kLineSignY));
        m.D8(kSignX, m.d8(kLineSignX));
    }
    // is the line, grown by its thickness, out of the window along one axis (`field` 0: x, 4: y)
    bool line_misses(u16 lowEnd, u16 highEnd, u16 field, u16 size) const {
        const u16 thick = m.d16(kThick);
        const s16 hh = (s16)m.d16((u16)(highEnd + field + 2)), lh = (s16)m.d16((u16)(lowEnd + field + 2));
        if (hh < -1) return true;
        if (hh == -1 && (s16)m.d16((u16)(highEnd + field)) < (s16)(u16)(0 - thick)) return true;
        if (lh > 0) return true;
        if (lh == 0 && (s16)m.d16((u16)(lowEnd + field)) > (s16)(u16)(thick + size)) return true;
        return false;
    }
    void add_long(u16 at, u32 v) { m.D32(at, m.d32(at) + v); }
    void slope_keep(const Slope &s) { m.D16(kLineSlope, s.frac); m.D16((u16)(kLineSlope + 2), s.lo); m.D16((u16)(kLineSlope + 4), s.hi); }
    Slope slope_kept() const { Slope s = { m.d16(kLineSlope), m.d16((u16)(kLineSlope + 2)), m.d16((u16)(kLineSlope + 4)) }; return s; }

    // --- wider than tall ---

    // an end beside the window is brought to the window's side: its row and its lights moved along the line
    void line_bring_in(u16 p, bool isQ) {
        const u32 x = m.d32(p), side = wide();
        u32 out;
        if (x & 0x80000000u) out = 0 - x;
        else if (x > side) out = x - side;
        else return;
        seen(k_line_end_brought_in);
        const u32 dxl = m.d32(kLineDx);
        const Bit64u product = (Bit64u)((Bit64s)(s32)out * (Bit64s)(s32)m.d32(kLineDy));
        u32 rows = dxl ? (u32)(product / dxl) : 0;
        if (((m.d8(kLineSignY) & 0x80) != 0) != isQ) rows = 0 - rows;
        add_long((u16)(p + 4), rows);
        const u16 change = (u16)((u16)rows * m.d16(kLightPerRow2));
        const u16 a = isQ ? kLightB : kLightA, l = isQ ? kLightQ : kLightP, c = isQ ? kCapLightQ : kCapLightP;
        m.D16(a, (u16)(m.d16(a) + change));
        m.D16(l, (u16)(m.d16(l) + change));
        m.D16(c, (u16)(m.d16(c) + change));
        m.D32(p, (x & 0x80000000u) ? 0 : side);
    }
    // an end's cap: an upright stretch of the line's thickness at the end's x, cut to the window
    void line_cap(u16 p, u16 lightAt) {
        const u16 thick = m.d16(kThick);
        u16 row = 0, rows;
        if (neg(yhi(p))) {
            m.D16(lightAt, (u16)(m.d16(lightAt) - (u16)((u32)(s32)(s16)m.d16(kAcrossStep) * m.d32((u16)(p + 4)))));
            const u32 sum = (u32)thick + ylo(p);
            if ((u16)(yhi(p) + (sum >> 16)) != 0) return;
            rows = (u16)sum;
            if (rows > deep()) rows = deep();
        } else {
            if (yhi(p) != 0) return;
            row = ylo(p);
            if (deep() <= row) return;
            rows = (u16)(deep() - row);
            if (rows > thick) rows = thick;
        }
        seen(k_line_cap);
        put_tall(row, rows, m.d16(lightAt), xlo(p));
    }
    // both points moved by `by` rows; is anything of the edge between them in the window's rows?  `gone` = the
    // line can be left altogether (below the window, or -- when moving down -- wholly outside)
    bool line_move_rows(u16 p, u16 q, u32 by, bool down, bool &gone) {
        gone = false;
        add_long((u16)(p + 4), by);
        add_long((u16)(q + 4), by);
        const bool qIn = yhi(q) == 0 && ylo(q) < deep();
        if (neg(yhi(p))) { if (!neg(yhi(q))) return true; gone = down; return false; }
        if (yhi(p) != 0) { if (neg(yhi(q)) || qIn) return true; gone = down; return false; }
        if (neg(yhi(q)) || qIn || ylo(p) < deep()) return true;
        gone = true;
        return false;
    }

    void line_wide(u16 p, u16 q) {
        // some of it in the window's columns?
        const bool pIn = xhi(p) == 0 && xlo(p) < wide(), qIn = xhi(q) == 0 && xlo(q) < wide();
        if (neg(xhi(p)) ? neg(xhi(q)) : (!pIn && !neg(xhi(q)) && !qIn)) { seen(k_line_wide_beside); return; }
        seen(k_line_wide); kind = 1;
        // the light's steps: along the line a row, or across its thickness
        if (m.d16(kLightP) != m.d16(kLightQ)) {
            seen(k_line_light_along);
            m.D16(kLightPerRow2, line_light_step(m.d32(kLineDy), m.d8(kSignY)));
            m.D16(kAcrossStep, 0);
        } else {
            u16 diff = (u16)(m.d16(kLightP) - m.d16(kLightA));
            const bool down = neg(diff);
            if (down) diff = (u16)(0 - diff);
            diff >>= 1;
            const u16 thick = m.d16(kThick);
            u16 step = 0;
            if (diff > thick && thick == 0) {
                // (the game jumps into the other form's code here: the step goes to the other variable)
                seen(k_line_thick_0);
                step = diff;
                if (step >> 8) step &= 0xFFFE;
                if (down != ((m.d8(kSignY) & 0x80) != 0)) step = (u16)(0 - step);
                m.D16(kLightPerRow2, (u16)(step << 1));
                m.D16(kAcrossStep, 0);
            } else {
                if (diff > thick) {
                    seen(k_line_light_across);
                    step = (u16)(diff / thick);
                    if (step >> 8) step &= 0xFFFE;
                    if (down) step = (u16)(0 - step);
                }
                m.D16(kAcrossStep, (u16)(step << 1));
                m.D16(kLightPerRow2, 0);
            }
        }
        line_bring_in(p, false);
        line_bring_in(q, true);
        // the upper edge: half the thickness up
        bool gone;
        const u16 half = (u16)((u16)(m.d16(kThick) + 1) >> 1);
        if (line_move_rows(p, q, 0 - (u32)half, false, gone)) {
            const u32 dy = m.d32(kLineDy);
            m.D8(kLineState, 2);
            if (dy) {
                seen(k_line_wide_upper_edge);
                const Slope s = slope_div(m.d32(kLineDx), dy);
                slope_keep(s);
                m.D8(kLineState, 0xFF);
                shallow_edge(p, q, s);
            } else seen(k_line_wide_level);
        } else if (gone) { seen(k_line_wide_below); return; }
        line_signs_again();
        line_cap(p, kCapLightP);
        line_cap(q, kCapLightQ);
        // the lower edge: the whole thickness down from the upper
        if (!line_move_rows(p, q, m.d16(kThick), true, gone)) { seen(k_line_wide_lower_out); return; }
        line_lights_again();
        const u8 state = m.d8(kLineState);
        if (state == 0) {
            // the upper edge was not in the window: the slope now
            const u32 dy = m.d32(kLineDy);
            if (!dy) return;
            seen(k_line_wide_lower_edge_only);
            shallow_edge(p, q, slope_div(m.d32(kLineDx), dy));
            return;
        }
        const Slope s = slope_kept();
        m.D8(kLineState, (u8)(state - 1));
        if ((u8)(state - 1) == 0) { steep_edge(p, q, s); return; }       // (cannot be: the state is 2 or FF here)
        m.D8(kLineState, (u8)(state - 2));
        if ((u8)(state - 2) == 0) return;                               // a level line: the caps are all of it
        seen(k_line_wide_lower_edge);
        shallow_edge(p, q, s);
    }

    // --- taller than wide ---

    // both edges' part left of the window, when the left edge is wholly there and the right one is not: one
    // running record over the line's rows in the window, x in whole pixels, the slope in rows a pixel (8.8)
    void line_left_record(u16 p, u16 q) {
        const u16 step2 = m.d16(kLightPerRow2);
        u16 pRow = 0, qRow = 0;
        if (neg(yhi(p))) m.D16(kLightA, (u16)(m.d16(kLightA) - (u16)((u32)(s32)(s16)step2 * m.d32((u16)(p + 4)))));
        else pRow = (yhi(p) != 0 || ylo(p) > deep()) ? deep() : ylo(p);
        if (neg(yhi(q))) m.D16(kLightB, (u16)(m.d16(kLightB) - (u16)((u32)(s32)(s16)step2 * m.d32((u16)(q + 4)))));
        else qRow = (yhi(q) != 0 || ylo(q) > deep()) ? deep() : ylo(q);
        // from the upper end
        m.D8(kLineSide, m.d8(kLineSignX));
        u16 top = qRow, end = pRow, light = m.d16(kLightB);
        u32 x = m.d32(q);
        if (qRow >= pRow) {
            top = pRow; end = qRow; light = m.d16(kLightA); x = m.d32(p);
            m.D8(kLineSide, (u8)(0 - m.d8(kLineSide)));
        }
        const Bit64u dy8 = (Bit64u)(Bit64s)(s32)m.d32(kLineDy) << 8;
        const u32 dxl = m.d32(kLineDx);
        const u32 rowsAPixel = (u32)(dy8 >> 32) >= dxl ? (u32)dy8 : (u32)(dy8 / dxl);
        const u16 slope = (rowsAPixel >> 16) ? 0xFFFF : (u16)rowsAPixel;
        seen(k_line_tall_left_record);
        running(top, (m.d8(kLineSide) & 0x80) ? XA(0xFFFA, 0xFFFA) : XA(0xFFF9, 0xFFF9), slope, light, (u32)(u8)(end - top) << 24 | ((0 - x) & 0xFFFFFF));
    }
    // one of the two edges, its slope worked out
    void line_tall_edge(u16 p, u16 q, bool first) {
        bool steep;
        const Slope s = slope_of(m.d32(kLineDx), m.d32(kLineDy), steep);
        if (first) { slope_keep(s); m.D8(kLineState, (u8)(m.d8(kLineState) + (steep ? 1 : -1))); }
        if (steep) {
            seen(first ? k_line_tall_steep : k_line_tall_right_only_steep);
            if (m.d16(kLightP) != m.d16(kLightQ)) m.D16(kLightPerPixel, line_light_step(m.d32(kLineDx), m.d8(kSignX)));
            steep_edge(p, q, s);
        } else {
            seen(first ? k_line_tall_shallow : k_line_tall_right_only_shallow);
            shallow_edge(p, q, s);
        }
    }

    void line_tall(u16 p, u16 q) {
        m.D16(kAcrossStep, 0); m.D16(kLightPerRow2, 0); m.D16(kLightPerPixel, 0);
        if (m.d16(kLightP) != m.d16(kLightQ)) {
            seen(k_line_light_along);
            const u16 step2 = line_light_step(m.d32(kLineDy), m.d8(kSignY));
            m.D16(kLightPerRow2, step2); m.D16(kAcrossStep, step2);
        }
        // some of it in the window's rows?
        const bool pIn = yhi(p) == 0 && ylo(p) < deep(), qIn = yhi(q) == 0 && ylo(q) < deep();
        if (neg(yhi(p)) ? neg(yhi(q)) : (!pIn && !neg(yhi(q)) && !qIn)) { seen(k_line_tall_above_below); return; }
        seen(k_line_tall); kind = 2;
        // the left edge: half the thickness to the left
        const u16 thick = m.d16(kThick);
        const u32 half = (u16)((u16)(thick + 1) >> 1);
        add_long(p, 0 - half);
        add_long(q, 0 - half);
        const bool qInX = xhi(q) == 0 && xlo(q) < wide();
        if (neg(xhi(p)) && neg(xhi(q))) {
            // wholly left of the window: of interest only if the right edge will not be
            const u16 reach = (u16)(0 - thick);
            if (!((xhi(p) == 0xFFFF && xlo(p) > reach) || (xhi(q) == 0xFFFF && xlo(q) > reach))) { seen(k_line_tall_left_out); return; }
            kind = 3;
            line_left_record(p, q);
        } else {
            if (!neg(xhi(p))) {
                if (xhi(p) != 0) { if (!neg(xhi(q)) && !qInX) { seen(k_line_tall_right_out); return; } }
                else if (!neg(xhi(q)) && !qInX && xlo(p) >= wide()) { seen(k_line_tall_right_out); return; }
            }
            line_tall_edge(p, q, true);
        }
        // the right edge: the whole thickness on from the left
        line_signs_again();
        line_lights_again();
        add_long(p, thick);
        add_long(q, thick);
        const u8 state = m.d8(kLineState);
        if (state == 0) { line_tall_edge(p, q, false); return; }
        const Slope s = slope_kept();
        m.D8(kLineState, (u8)(state - 1));
        if ((u8)(state - 1) == 0) { seen(k_line_tall_right_steep); steep_edge(p, q, s); }
        else { seen(k_line_tall_right_shallow); shallow_edge(p, q, s); }
    }

    void thick_line(u16 q) {
        const u16 p = m.d16(kLineP);
        kind = 0;
        // the direction and the lengths
        const s32 dy = (s32)(m.d32((u16)(q + 4)) - m.d32((u16)(p + 4)));
        const u8 signY = y32(q) < y32(p) ? 0xFF : 1;
        m.D8(kLineSignY, signY); m.D8(kSignY, signY);
        m.D32(kLineDy, signY == 1 ? (u32)dy : 0 - (u32)dy);
        const s32 dx = (s32)(m.d32(q) - m.d32(p));
        const u8 signX = x32(q) < x32(p) ? 0xFF : 1;
        m.D8(kLineSignX, signX); m.D8(kSignX, signX);
        m.D32(kLineDx, signX == 1 ? (u32)dx : 0 - (u32)dx);
        // grown by its thickness, is it in the window at all
        if (signY == 1 ? line_misses(p, q, 4, deep()) : line_misses(q, p, 4, deep())) { seen(k_line_outside); return; }
        if (signX == 1 ? line_misses(p, q, 0, wide()) : line_misses(q, p, 0, wide())) { seen(k_line_outside); return; }
        if (m.d16(kFlatNext) == kNoFlatLeft) { seen(k_line_none_left); return; }
        m.D16(kFlatNext, (u16)(m.d16(kFlatNext) + 1));      // (a number is used up; the id is the caller's)
        // the lights: none below 0; and two equal ones mean a light across the thickness
        const u16 lp = m.d16(kLightP), lq = m.d16(kLightQ);
        m.D16(kLightA, not_below_0(lp)); m.D16(kCapLightP, not_below_0(lp));
        m.D16(kLightB, not_below_0(lq)); m.D16(kCapLightQ, not_below_0(lq));
        if (lp == lq) {
            const u16 v = neg(lp) ? (u16)(0 - lp) : 0;
            m.D16(kLightP, v); m.D16(kLightQ, v);
        }
        m.D8(kLineState, 0);
        if (m.d32(kLineDy) > m.d32(kLineDx)) line_tall(p, q); else line_wide(p, q);
    }

    // =========================================================================================================
    // The sprite scaler's row pass (1CCA:A132).  The scaler has drawn the sprite into the background buffer
    // with its raw colours (below 40h) and kept the box it touched.  Here the sprite becomes a flat object of
    // colour 0 ("copy from the background buffer"), every run of raw pixels in the box is given its real colours
    // (kSpriteMap[]) in place, and the run's first pixel and the pixel after its last go into the row's list as
    // two crossings.  `bg` = the background buffer's segment.
    // =========================================================================================================

    // x as a crossing's word, the game's way (x << 7 for the x that can be)
    static u16 x_word(u16 x) { return (u16)(((x >> 1) & 0xFF) << 8 | (x & 1) << 7 | x >> 9); }

    // the box grown by two pixels each way and cut to the window: kBoxLeft = its left, kBoxRight = its WIDTH,
    // kBoxTop = its top row (the box's y runs upwards: turned over), kBoxBottom = its ROWS.  False: nothing of it.
    bool sprite_box() {
        // (grown by two pixels each way; X-Wing's by four)
        m.D16(kBoxRight, (u16)(m.d16(kBoxRight) + XA(2, 4))); m.D16(kBoxBottom, (u16)(m.d16(kBoxBottom) - XA(2, 4)));
        m.D16(kBoxLeft, (u16)(m.d16(kBoxLeft) - XA(2, 4))); m.D16(kBoxTop, (u16)(m.d16(kBoxTop) + XA(2, 4)));
        const s16 w = (s16)wide(), d = (s16)deep();
        s16 left = (s16)m.d16(kBoxLeft);
        if (left >= w) return false;
        if (left < 0) { left = 0; m.D16(kBoxLeft, 0); }
        s16 right = (s16)m.d16(kBoxRight);
        if (right < 0) return false;
        if (right >= w) right = w;
        if ((u16)right <= (u16)left) return false;
        m.D16(kBoxRight, (u16)(right - left));
        s16 top = (s16)(u16)(m.d16(kDeepLess1) - m.d16(kBoxTop));
        if (top >= d) return false;
        if (top < 0) top = 0;
        m.D16(kBoxTop, (u16)top);
        s16 end = (s16)(u16)(m.d16(kDeepLess1) - m.d16(kBoxBottom));
        if (end < 0) return false;
        if (end >= d) end = d;
        if ((u16)end <= (u16)top) return false;
        m.D16(kBoxBottom, (u16)(end - top));
        return true;
    }

    void sprite_rows(Bit8u *bg) {
        kind = 0;
        if (m.d16(kFlatNext) >= kNoFlatLeft) { seen(k_sprite_none_left); return; }
        if (!sprite_box()) { seen(k_sprite_outside); return; }
        seen(k_sprite_rows); kind = 1;
        // the flat object: colour 0, and where it is in space (for the order against the meshes)
        const u16 n = take_flat(0);
        m.D16((u16)(kFlatX + 2 * n), (u16)((s32)m.d32(kSpriteX) >> 5));
        m.D16((u16)(kFlatY + 2 * n), (u16)((s32)m.d32(kSpriteY) >> 5));
        m.D16((u16)(kFlatZ + 2 * n), (u16)((s32)m.d32(kSpriteZ) >> 5));
        flat_id(n);
        // the rows.  (The first row's place in the buffer is from the table; the next ones a window's width on.)
        u16 row2 = (u16)(m.d16(kBoxTop) << 1);
        u16 start = m.cs ? TraceMem::get16(m.cs + (u16)(kRowOffsets + row2)) : 0;
        for (;;) {
            m.D16(kRowStart, start);
            u16 at = (u16)(start + m.d16(kBoxLeft)), left = m.d16(kBoxRight);
            const u16 slot = (u16)(kRows + row2);
            while (left) {
                if (bg[at] >= 0x40) { at++; left--; continue; }
                seen(k_sprite_run);
                put4_chunk(slot, (u32)x_word((u16)(at - start)) << 16 | id());
                do { bg[at] = m.d8((u16)(kSpriteMap + bg[at])); at++; left--; } while (left && bg[at] < 0x40);
                put4_chunk(slot, (u32)x_word((u16)(at - start)) << 16 | id());
            }
            start = (u16)(m.d16(kRowStart) + wide());
            row2 = (u16)(row2 + 2);
            const u16 rows = (u16)(m.d16(kBoxBottom) - 1);
            m.D16(kBoxBottom, rows);
            if (!rows) break;
        }
    }
};
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// trace2_drawface: `vertices` = CX at the game's entry; `face` = the face's body (the bytes at [2488]:[2486],
// (vertex, edge) pairs with the first vertex again at the end; it lies in the ship's data, not in the two segments).
// ---------------------------------------------------------------------------------------------------------------
void tie640_trace_drawface(TraceMem &m, const Bit8u *face, int vertices) {
    Tracer t(m);
    t.face(face, (Bit16u)vertices);
}

// ---------------------------------------------------------------------------------------------------------------
// The other three (see struct Shapes).  `strayHigh` = the high half of ECX at the game routine's entry (written
// into a record in one odd case only, see steep_beside); `strayEaxHigh` = that of EAX (see flat_level_line); `kind` gets what the call turned out to be.
//   trace2_drawscreencoords 1CCA:4FF3: gives the game's AX (0 = refused, 1 = taken)
//   drawln2_tracelineedges 1CCA:2BB0:  `q` = DI at entry, the line's second point (the first is DS:[2732])
//   the sprite's rows 1CCA:A132:       `bg` = an image of the background buffer's segment (DS:[48BD]); m.cs is needed
// ---------------------------------------------------------------------------------------------------------------
int tie640_trace_flat_polygon(TraceMem &m, Bit16u strayHigh, Bit16u strayEaxHigh, int *kind) {
    Shapes s(m);
    s.strayHigh = strayHigh;
    s.strayEaxHigh = strayEaxHigh;
    const int ax = s.flat_polygon();
    if (kind) *kind = s.kind;
    return ax;
}
void tie640_trace_thick_line(TraceMem &m, Bit16u q, Bit16u strayHigh, int *kind) {
    Shapes s(m);
    s.strayHigh = strayHigh;
    s.thick_line(q);
    if (kind) *kind = s.kind;
}
void tie640_trace_sprite_rows(TraceMem &m, Bit8u *bg, int *kind) {
    Shapes s(m);
    s.sprite_rows(bg);
    if (kind) *kind = s.kind;
}

// ---------------------------------------------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------------------------------------------
namespace {
enum { kDataPara = XA(0x421F, 0x3F1C), kRendPara = XA(0x1CCA, 0x222D), kTablesSeg = XA(0xD8A6, 0xD34A) };
Bit8u g_ds[0x10000], g_tb[0x10000], g_face[160];
bool g_pending = false;
int g_vertices = 0;
unsigned g_calls = 0, g_bad = 0, g_said = 0;
unsigned g_byFlags[256][2];     // [the OR of the face's edges' flags at entry][0 calls, 1 bad]
Bit8u g_flagsOr = 0, g_entryFlags[256];
std::string g_inputs;

std::string describe(Bit8u *ds, const Bit8u *face, int n) {
    char b[200];
    std::string t;
    snprintf(b, sizeof(b), "object %u face %u gouraud %u, %d vertices:", host_readw(ds + XA(0x2464, 0x2F9C)), host_readw(ds + XA(0x2474, 0x2FAC)), ds[XA(0x243A, 0x2F72)], n);
    t = b;
    for (int i = 0; i < n; i++) {
        const int v1 = face[2 * i], e = face[2 * i + 1];
        const Bit16u p1 = host_readw(ds + XA(0x2AF0, 0x3635) + 2 * e), p2 = host_readw(ds + XA(0x2BF0, 0x3735) + 2 * e);
        snprintf(b, sizeof(b), " [e %d v %d flags %02X (%d,%d)-(%d,%d) light %04X]", e, v1, ds[XA(0x2FF0, 0x3B35) + e],
                 (int)(Bit32s)host_readd(ds + p1), (int)(Bit32s)host_readd(ds + (Bit16u)(p1 + 4)), (int)(Bit32s)host_readd(ds + p2), (int)(Bit32s)host_readd(ds + (Bit16u)(p2 + 4)),
                 host_readw(ds + XA(0x223A, 0x2D72) + 2 * v1));
        t += b;
    }
    return t;
}
}  // namespace

bool tie640_trace_check_on() {
    static int on = -1;
    if (on < 0) { const char *e = getenv("WCNET_T640_TRACECHECK"); on = e && e[0] == '1' ? 1 : 0; }
    return on == 1;
}

// the game's trace2_drawface is at its entry (1CCA:53F3)
void tie640_trace_check_entry(Bit16u loadSeg) {
    const PhysPt ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    Bit8u *M = GetMemBase();
    memcpy(g_ds, M + ds, 0x10000);
    memcpy(g_tb, M + ((PhysPt)host_readw(M + ds + kTablesSeg) << 4), 0x10000);
    g_vertices = reg_cx & 0x3F;
    const PhysPt face = ((PhysPt)host_readw(M + ds + XA(0x2488, 0x2FC0)) << 4) + host_readw(M + ds + XA(0x2486, 0x2FBE));
    for (unsigned i = 0; i < sizeof(g_face); i++) g_face[i] = mem_readb(face + i);     // (the ship's data is in the EMS page frame: not in the flat memory)
    g_flagsOr = 0;
    for (int i = 0; i < g_vertices; i++) g_flagsOr |= g_ds[XA(0x2FF0, 0x3B35) + g_face[2 * i + 1]];
    g_inputs = describe(g_ds, g_face, g_vertices);
    memcpy(g_entryFlags, g_ds + XA(0x2FF0, 0x3B35), 256);
    TraceMem m = { g_ds, g_tb, M + ((PhysPt)(Bit16u)(loadSeg + kRendPara) << 4) };
    tie640_trace_drawface(m, g_face, reg_cx);
    g_pending = true;
}

// ... and has returned (1CCA:13DF, 193D, 1A1A)
void tie640_trace_check_return(Bit16u loadSeg) {
    if (!g_pending) return;
    g_pending = false;
    const PhysPt ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    Bit8u *M = GetMemBase();
    Bit8u *live = M + ds, *tables = M + ((PhysPt)host_readw(M + ds + kTablesSeg) << 4);
    g_calls++;
    g_byFlags[g_flagsOr][0]++;
    int firstT = -1, firstD = -1, countT = 0, countD = 0;
    for (int i = 0; i < XA(0xA000, 0xE000); i++) if (g_tb[i] != tables[i]) { if (firstT < 0) firstT = i; countT++; }
    // (DS:6040..6077 are the input module's variables (code at image 19D0:00A7..), written from its interrupt handler
    // while the routine runs: not the tracer's)
    for (int i = 0x2000; i < XA(0x6800, 0x6000); i++) if (g_ds[i] != live[i] && !(i >= XA(0x6040, 0xFFFE) && i < XA(0x6078, 0xFFFF))) { if (firstD < 0) firstD = i; countD++; }
    if (!countT && !countD) return;
    g_bad++;
    g_byFlags[g_flagsOr][1]++;
    if (g_said < 12) {
        g_said++;
        std::string t;
        char b[80];
        int shown = 0;
        for (int i = 0x2000; i < XA(0x6800, 0x6000) && shown < 24; i++) if (g_ds[i] != live[i] && !(i >= XA(0x6040, 0xFFFE) && i < XA(0x6078, 0xFFFF))) { snprintf(b, sizeof(b), " DS:%04X %02X/%02X", i, g_ds[i], live[i]); t += b; shown++; }
        shown = 0;
        for (int i = 0; i < XA(0xA000, 0xE000) && shown < 40; i++) if (g_tb[i] != tables[i]) { snprintf(b, sizeof(b), " T:%04X %02X/%02X", i, g_tb[i], tables[i]); t += b; shown++; }
        wclog(1, "T640 trace check: call %u differs (%d bytes of the tables' segment, %d of the data segment; native/game):%s", g_calls, countT, countD, t.c_str());
        wclog(1, "T640 trace check: ... its face: %s", g_inputs.c_str());
        wclog(1, "T640 trace check: ... the native routine's way:%s", path_text().c_str());
        wclog(1, "T640 trace check: ... window: wide %u less1 %u wide7 %04X deep %u (before), wide %u (after)", host_readw(g_ds + kWide), host_readw(g_ds + kWideLess1), host_readw(g_ds + kWide7), host_readw(g_ds + kDeep), mem_readw(SegPhys(::ds) + kWide));
        if (firstT >= 0x2000) {
            // where the lists first differ: the row whose chain holds the chunk, and the entry there (the game's
            // chunk walked from its start; a record split over two chunks can put the walk out of step)
            const int chunk = firstT & ~0x7F;
            int row = -1;
            for (int r = 0; r < 256 && row < 0; r++)
                for (int at = host_readw(live + XA(0x6232, 0xD3C8) + 2 * r), n = 0; at >= 0x2000 && at < XA(0xA000, 0xE000) && n < 300; n++) {
                    if (at == chunk) { row = r; break; }
                    const int used = host_readw(tables + at);
                    if (used <= 0x7C) break;
                    at = used;
                }
            int used = host_readw(tables + chunk);
            if (used > 0x7C) used = 0x7C;
            const char *kind = "the chunk's count or link";
            int e = -1, o = -1, at = chunk + 4;
            char rec[24] = "";
            while (at < chunk + 4 + used) {
                const Bit16u w = host_readw(tables + at);
                const int size = w == 0xFFFF ? 8 : w >= XA(0xFFEF, 0xFFEF) ? 12 : 4;
                if (firstT < at + size || at + size >= chunk + 4 + used) {
                    const int tag = w == 0xFFFF ? at + 4 : w >= XA(0xFFEF, 0xFFEF) ? at + 4 : at;
                    e = tables[tag]; o = tables[tag + 1];
                    if (w == 0xFFFF) kind = "FFFF prefix + entry";
                    else if (w >= XA(0xFFEF, 0xFFEF)) { snprintf(rec, sizeof(rec), "running record %04X", w); kind = rec; }
                    else kind = "plain entry";
                    break;
                }
                at += size;
            }
            wclog(1, "T640 trace check: ... first at T:%04X (native %02X, game %02X): row %d, %s at %04X, object %d face %u edge %d (its flags at entry %02X)",
                  firstT, g_tb[firstT], tables[firstT], row, kind, at, o, host_readw(g_ds + XA(0x2474, 0x2FAC)), e, e >= 0 ? g_entryFlags[e] : 0);
        }
        {   // the lists' chunks that differ (their first 30h bytes: native, game), and the rows they are of
            std::string u; char c[40];
            for (int i = 0x2000; i < XA(0xA000, 0xE000); i += 0x80) {
                if (!memcmp(g_tb + i, tables + i, 0x80)) continue;
                snprintf(c, sizeof(c), " | chunk %04X N:", i); u += c;
                for (int k = 0; k < 0x30; k++) { snprintf(c, sizeof(c), "%02X", g_tb[i + k]); u += c; if (k % 4 == 3) u += ' '; }
                u += " G:";
                for (int k = 0; k < 0x30; k++) { snprintf(c, sizeof(c), "%02X", tables[i + k]); u += c; if (k % 4 == 3) u += ' '; }
                for (int r = 0; r < 200; r++) if (host_readw(live + XA(0x6232, 0xD3C8) + 2 * r) == i) { snprintf(c, sizeof(c), " row %d", r); u += c; }
            }
            if (!u.empty()) wclog(1, "T640 trace check: ... %s", u.c_str());
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The check of the other three routines, in the same way: at the game routine's entry the memory is copied and
// the native one run on the copy; where the game's routine returns to, the two are compared.
// ---------------------------------------------------------------------------------------------------------------
namespace {
enum Shape { kNoShape = -1, kFlatShape, kLineShape, kSpriteShape, kShapes };
const char *const kShapeNames[kShapes] = { "trace2_drawscreencoords (4FF3)", "drawln2_tracelineedges (2BB0)", "the sprite's rows (A132)" };
const char *const kShapeKinds[kShapes][4] = {
    { "refused", "upright line", "level line", "outline" },
    { "refused", "wide", "tall", "tall, left record" },
    { "nothing", "rows", "", "" } };
struct ShapeCount { unsigned calls, bad, said, kinds[4][2], oddEs, oddSs; } g_shape[kShapes];
Shape g_shapePending = kNoShape;
int g_shapeKind = 0;
Bit16u g_shapeAx = 0;
Bit8u g_bg[0x10000], g_bgLive[0x10000];
PhysPt g_bgAt = 0;
Bit16u g_linePoint[2];

// where the lists first differ: the row whose chain holds the chunk, and the entry there (the game's chunk walked
// from its start; a record split over two chunks can put the walk out of step)
std::string lists_first_difference(Bit8u *live, Bit8u *tables, int firstT) {
    char out[300];
    const int chunk = firstT & ~0x7F;
    int row = -1;
    for (int r = 0; r < 256 && row < 0; r++)
        for (int at = host_readw(live + XA(0x6232, 0xD3C8) + 2 * r), n = 0; at >= 0x2000 && at < XA(0xA000, 0xE000) && n < 300; n++) {
            if (at == chunk) { row = r; break; }
            const int used = host_readw(tables + at);
            if (used <= 0x7C) break;
            at = used;
        }
    int used = host_readw(tables + chunk);
    if (used > 0x7C) used = 0x7C;
    const char *kind = "the chunk's count or link";
    int e = -1, o = -1, at = chunk + 4;
    char rec[24] = "";
    while (at < chunk + 4 + used) {
        const Bit16u w = host_readw(tables + at);
        const int size = w == 0xFFFF ? 8 : w >= XA(0xFFEF, 0xFFEF) ? 12 : 4;
        if (firstT < at + size || at + size >= chunk + 4 + used) {
            const int tag = w >= XA(0xFFEF, 0xFFEF) ? at + 4 : at;
            e = tables[tag]; o = tables[tag + 1];
            if (w == 0xFFFF) kind = "FFFF prefix + entry";
            else if (w >= XA(0xFFEF, 0xFFEF)) { snprintf(rec, sizeof(rec), "running record %04X", w); kind = rec; }
            else kind = "plain entry";
            break;
        }
        at += size;
    }
    snprintf(out, sizeof(out), "first at T:%04X (native %02X, game %02X): row %d, %s at %04X, id %02X%02X", firstT, g_tb[firstT], tables[firstT], row, kind, at, o & 0xFF, e & 0xFF);
    return out;
}
// the lists' chunks that differ (their first 30h bytes: native, game), and the rows they are of
std::string lists_chunks_that_differ(Bit8u *live, Bit8u *tables) {
    std::string u; char c[40];
    int shown = 0;
    for (int i = 0x2000; i < XA(0xA000, 0xE000) && shown < 6; i += 0x80) {
        if (!memcmp(g_tb + i, tables + i, 0x80)) continue;
        shown++;
        snprintf(c, sizeof(c), " | chunk %04X N:", i); u += c;
        for (int k = 0; k < 0x30; k++) { snprintf(c, sizeof(c), "%02X", g_tb[i + k]); u += c; if (k % 4 == 3) u += ' '; }
        u += " G:";
        for (int k = 0; k < 0x30; k++) { snprintf(c, sizeof(c), "%02X", tables[i + k]); u += c; if (k % 4 == 3) u += ' '; }
        for (int r = 0; r < 200; r++) if (host_readw(live + XA(0x6232, 0xD3C8) + 2 * r) == i) { snprintf(c, sizeof(c), " row %d", r); u += c; }
    }
    return u;
}

std::string point_text(Bit8u *ds, Bit16u p) {
    char b[60];
    snprintf(b, sizeof(b), "%04X(%d,%d)", p, (int)(Bit32s)host_readd(ds + p), (int)(Bit32s)host_readd(ds + (Bit16u)(p + 4)));
    return b;
}
std::string describe_shape(Shape s, Bit8u *ds) {
    char b[240];
    std::string t;
    if (s == kFlatShape) {
        const Bit16u first = host_readw(ds + XA(0x2452, 0x2F8A)), end = host_readw(ds + XA(0x2454, 0x2F8C));
        snprintf(b, sizeof(b), "flat number %02X colour %02X id-low %02X object %u, %u points (%u in a row, %u in a column), top point %04X:",
                 host_readw(ds + XA(0x26E3, 0x321B)), ds[XA(0x2447, 0x2F7F)], ds[XA(0x2484, 0x2FBC)], host_readw(ds + XA(0x2464, 0x2F9C)), host_readw(ds + XA(0x2472, 0x2FAA)), ds[XA(0x2460, 0x2F98)], ds[XA(0x2462, 0x2F9A)], host_readw(ds + XA(0x245A, 0x2F92)));
        t = b;
        for (Bit16u p = first; p != end && t.size() < 700; p = (Bit16u)(p + 8)) { t += ' '; t += point_text(ds, p); }
        snprintf(b, sizeof(b), "; stale lights %04X %04X steps %04X %04X", host_readw(ds + XA(0x274E, 0x3286)), host_readw(ds + XA(0x2750, 0x3288)), host_readw(ds + XA(0x2752, 0x328A)), host_readw(ds + XA(0x2754, 0x328C)));
        t += b;
    } else if (s == kLineShape) {
        snprintf(b, sizeof(b), "id %04X flat number %02X, thickness %u, lights %04X %04X, from ", host_readw(ds + XA(0x274C, 0x3284)), host_readw(ds + XA(0x26E3, 0x321B)),
                 host_readw(ds + XA(0x2730, 0x3268)), host_readw(ds + XA(0x2738, 0x3270)), host_readw(ds + XA(0x273A, 0x3272)));
        t = b; t += point_text(ds, host_readw(ds + XA(0x2732, 0x326A))); t += " to "; t += point_text(ds, reg_di);
    } else {
        snprintf(b, sizeof(b), "flat number %02X id-low %02X, box x %d..%d y %d..%d (upwards), buffer %04X", host_readw(ds + XA(0x26E3, 0x321B)), ds[XA(0x2484, 0x2FBC)],
                 (Bit16s)host_readw(ds + XA(0x48B2, 0x5410)), (Bit16s)host_readw(ds + XA(0x48B6, 0x5414)), (Bit16s)host_readw(ds + XA(0x48B8, 0x5416)), (Bit16s)host_readw(ds + XA(0x48B4, 0x5412)), host_readw(ds + XA(0x48BD, 0x541B)));
        t = b;
    }
    return t;
}

void shape_entry(Bit16u loadSeg, Shape s) {
    const PhysPt dsAt = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    Bit8u *M = GetMemBase();
    memcpy(g_ds, M + dsAt, 0x10000);
    memcpy(g_tb, M + ((PhysPt)host_readw(M + dsAt + kTablesSeg) << 4), 0x10000);
    ShapeCount &c = g_shape[s];
    // (the routines write the lists through ES as their callers left it, and the sprite's rows once through SS)
    if (s != kSpriteShape && SegValue(es) != host_readw(M + dsAt + kTablesSeg)) c.oddEs++;
    if (SegValue(ss) != SegValue(ds)) c.oddSs++;
    g_inputs = describe_shape(s, g_ds);
    g_pathLength = 0;
    TraceMem m = { g_ds, g_tb, M + ((PhysPt)(Bit16u)(loadSeg + kRendPara) << 4) };
    g_shapeAx = 0;
    if (s == kFlatShape) g_shapeAx = (Bit16u)tie640_trace_flat_polygon(m, (Bit16u)(reg_ecx >> 16), (Bit16u)(reg_eax >> 16), &g_shapeKind);
    else if (s == kLineShape) {
        const Bit16u p = host_readw(g_ds + XA(0x2732, 0x326A)), q = reg_di;
        g_linePoint[0] = p; g_linePoint[1] = q;
        tie640_trace_thick_line(m, q, (Bit16u)(reg_ecx >> 16), &g_shapeKind);
    } else {
        g_bgAt = (PhysPt)host_readw(g_ds + XA(0x48BD, 0x541B)) << 4;
        MEM_BlockRead(g_bgAt, g_bg, 0x10000);
        tie640_trace_sprite_rows(m, g_bg, &g_shapeKind);
    }
    g_shapePending = s;
}

void shape_return(Bit16u loadSeg, Shape s) {
    if (g_shapePending != s) return;
    g_shapePending = kNoShape;
    const PhysPt ds = (PhysPt)(Bit16u)(loadSeg + kDataPara) << 4;
    Bit8u *M = GetMemBase();
    Bit8u *live = M + ds, *tables = M + ((PhysPt)host_readw(M + ds + kTablesSeg) << 4);
    ShapeCount &c = g_shape[s];
    c.calls++;
    c.kinds[g_shapeKind & 3][0]++;
    int firstT = -1, countT = 0, countD = 0, countB = 0, firstB = -1;
    bool pointDiffers = false;
    for (int i = 0; i < XA(0xA000, 0xE000); i++) if (g_tb[i] != tables[i]) { if (firstT < 0) firstT = i; countT++; }
    for (int i = 0x2000; i < XA(0x6800, 0x6000); i++) if (g_ds[i] != live[i] && !(i >= XA(0x6040, 0xFFFE) && i < XA(0x6078, 0xFFFF))) countD++;
    if (s == kSpriteShape) {
        MEM_BlockRead(g_bgAt, g_bgLive, 0x10000);
        for (int i = 0; i < 0x10000; i++) if (g_bg[i] != g_bgLive[i]) { if (firstB < 0) firstB = i; countB++; }
    }
    if (s == kLineShape) {
        // the line's two points are changed by the routine: they lie outside the part of the data segment compared
        for (int k = 0; k < 2; k++) for (int i = 0; i < 8; i++) { const Bit16u at = (Bit16u)(g_linePoint[k] + i); if ((at < 0x2000 || at >= XA(0x6800, 0x6000)) && g_ds[at] != live[at]) { countD++; pointDiffers = true; } }
    }
    const bool axDiffers = s == kFlatShape && g_shapeAx != reg_ax;
    if (!countT && !countD && !countB && !axDiffers) return;
    c.bad++;
    c.kinds[g_shapeKind & 3][1]++;
    if (c.said >= 8) return;
    c.said++;
    std::string t;
    char b[96];
    int shown = 0;
    for (int i = 0x2000; i < XA(0x6800, 0x6000) && shown < 24; i++) if (g_ds[i] != live[i] && !(i >= XA(0x6040, 0xFFFE) && i < XA(0x6078, 0xFFFF))) { snprintf(b, sizeof(b), " DS:%04X %02X/%02X", i, g_ds[i], live[i]); t += b; shown++; }
    shown = 0;
    for (int i = 0; i < XA(0xA000, 0xE000) && shown < 40; i++) if (g_tb[i] != tables[i]) { snprintf(b, sizeof(b), " T:%04X %02X/%02X", i, g_tb[i], tables[i]); t += b; shown++; }
    if (axDiffers) { snprintf(b, sizeof(b), " AX %04X/%04X", g_shapeAx, reg_ax); t += b; }
    if (pointDiffers) for (int k = 0; k < 2; k++) { t += k ? " Q " : " P "; t += point_text(g_ds, g_linePoint[k]); t += '/'; t += point_text(live, g_linePoint[k]); }
    wclog(1, "T640 trace check: %s call %u (%s) differs (%d bytes of the tables' segment, %d of the data segment, %d of the buffer; native/game):%s",
          kShapeNames[s], c.calls, kShapeKinds[s][g_shapeKind & 3], countT, countD, countB, t.c_str());
    wclog(1, "T640 trace check: ... its inputs: %s", g_inputs.c_str());
    wclog(1, "T640 trace check: ... the native routine's way:%s", path_text().c_str());
    if (firstT >= 0x2000) {
        wclog(1, "T640 trace check: ... %s", lists_first_difference(live, tables, firstT).c_str());
        wclog(1, "T640 trace check: ... %s", lists_chunks_that_differ(live, tables).c_str());
    }
    if (firstB >= 0) {
        // the row: the buffer's rows are a window's width apart from the first row's offset
        const Bit16u first = host_readw(M + ((PhysPt)(Bit16u)(loadSeg + kRendPara) << 4) + XA(0xCFE0, 0xD400)), wide = host_readw(live + XA(0x48C5, 0x5423));
        const int off = (Bit16u)(firstB - first);
        wclog(1, "T640 trace check: ... the buffer first at %04X (native %02X, game %02X, before %02X): row %d x %d", firstB, g_bg[firstB], g_bgLive[firstB],
              0, wide ? off / wide : -1, wide ? off % wide : -1);
    }
}
}  // namespace

void tie640_trace_shapes_places(void (*place)(Bit16u, Bit16u)) {
    static const Bit16u ips[] = { XA(0x4FF3, 0x519D), XA(0x0CA1, 0x0D91), XA(0x1F80, 0x20F4), XA(0x2BA5, 0x2D1D),  XA(0x2BB0, 0x2D30), XA(0x0EC1, 0x0FB1), XA(0x20CD, 0x2245), XA(0x21C0, 0x2338),  XA(0xA132, 0xA555), XA(0xA0F8, 0xA51B) };
    for (size_t i = 0; i < sizeof(ips) / sizeof(ips[0]); i++) place(kRendPara, ips[i]);
}

// a watched place in the renderer's segment: true when it is one of the shapes' and the check has dealt with it
bool tie640_trace_shapes_hook(Bit16u loadSeg, Bit32u ip) {
    switch (ip) {
    case XA(0x4FF3, 0x519D): shape_entry(loadSeg, kFlatShape); return true;
    case XA(0x0CA1, 0x0D91): case XA(0x1F80, 0x20F4): case XA(0x2BA5, 0x2D1D): shape_return(loadSeg, kFlatShape); return true;
    case XA(0x2BB0, 0x2D30): shape_entry(loadSeg, kLineShape); return true;
    case XA(0x0EC1, 0x0FB1): case XA(0x20CD, 0x2245): case XA(0x21C0, 0x2338): shape_return(loadSeg, kLineShape); return true;
    case XA(0xA132, 0xA555): shape_entry(loadSeg, kSpriteShape); return true;
    case XA(0xA0F8, 0xA51B): shape_return(loadSeg, kSpriteShape); return true;
    }
    return false;
}

void tie640_trace_shapes_say() {
    for (int s = 0; s < kShapes; s++) {
        const ShapeCount &c = g_shape[s];
        std::string t; char b[96];
        for (int k = 0; k < 4; k++) if (c.kinds[k][0]) { snprintf(b, sizeof(b), " %s:%u/%u", kShapeKinds[s][k], c.kinds[k][1], c.kinds[k][0]); t += b; }
        wclog(1, "T640 trace check: %u calls of %s compared, %u differ; by kind (differ/calls):%s; ES not the tables' segment %u times, SS not DS %u",
              c.calls, kShapeNames[s], c.bad, t.c_str(), c.oddEs, c.oddSs);
    }
}

void tie640_trace_check_say() {
    std::string t;
    char b[96];
    for (int f = 0; f < 256; f++) if (g_byFlags[f][0]) { snprintf(b, sizeof(b), " %02X:%u/%u", f, g_byFlags[f][1], g_byFlags[f][0]); t += b; }
    wclog(1, "T640 trace check: %u calls of trace2_drawface compared, %u differ; by the edges' flags together (differ/calls):%s", g_calls, g_bad, t.c_str());
    std::string ran, never;
    for (int i = 0; i < kParts; i++) {
        if (g_parts[i]) { snprintf(b, sizeof(b), " %s:%u", g_partNames[i], g_parts[i]); ran += b; }
        else { never += ' '; never += g_partNames[i]; }
    }
    wclog(1, "T640 trace check: parts run:%s", ran.c_str());
    wclog(1, "T640 trace check: parts never run:%s", never.c_str());
    tie640_trace_shapes_say();
}

}  // namespace wc
