/*
 *  Terminal Velocity: the textured spans drawn natively (wcnet_tv.h, docs/terminalvelocity.md "The pixels drawn
 *  natively").
 *
 *  Most of a frame's emulated instructions were the game's span routines (53-67%, profile.md): for each scanline of
 *  a polygon, the polygon filler (0x3E720 in the shareware) calls the routine the polygon's kind set (through the
 *  pointer at 0x7CF14, jumped through at 0x49707) with its two edges (esi, edi: x 16.16 at +8, w at +0x10, u and v
 *  -- divided by w or not, by the routine -- at +0x18 and +0x20) and the row (ebx).  At the filler's call, the twin
 *  of the routine the pointer names draws the scanline: a C++ transliteration of the routine (the same registers,
 *  the same branches, the same 32-bit wraps and carries, the same quirks: three pixels drawn where four are left at
 *  the end of an unrolled loop), writing what the routine writes -- the frame buffer, the depth buffer, the
 *  routine's own variables -- and the call is stepped over: the filler reloads every register after it but ebp,
 *  which the routines keep.  The cockpit's picture copied into the 640-wide frame each frame (0x2FAB0: every byte
 *  doubled) is made natively at its entry the same way, its registers left as it leaves them.
 *
 *  A routine is twinned only when its code in memory is the shareware's, byte for byte but for the operands the
 *  loader relocated (wcnet_tv_sig.h's kSpans), and they all point where the shareware's layout puts its data, moved
 *  by one delta.  Anything a twin cannot do exactly gives the call back to the game's own code before anything is
 *  written: a divide the CPU would fault on, memory outside the game's RAM (conventional memory, where the frame
 *  buffer is, and extended memory from 1 MB), a span longer than any screen, a segment not flat, the direction
 *  flag set.
 *
 *  WCNET_TV_SPANS: unset or 1 the twins draw; 0 the game's own code draws everything; "check" each twin runs first
 *  (as it runs in play), the memory it declared (the span's row, the depth buffer's row, the routines' variables;
 *  for the cockpit the whole picture), widened by 64 bytes either side, is kept as the twin left it and put back as
 *  it was, the game's own code runs, and at its return every byte of that memory and the registers are compared.
 */
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcnet_tv.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "mem.h"
#include "regs.h"
#include "cpu.h"
#include "pic.h"
#include "wcnet_tv_sig.h"

namespace wc {

namespace {

using namespace tvsig;

enum { kMaxSpan = 4096, kCheckMargin = 64 };

// The shareware's image addresses of the data the routines use (their relocated operands checked to point at these,
// moved by one delta).
const Bit32u I_ROWS = 0x81CE8;     // the frame buffer's row pointers
const Bit32u I_ZROWS = 0x928E8;    // the depth buffer's row pointers (a dword a pixel)
const Bit32u I_TEX = 0x7CF18;      // the polygon's texture (its high 16 bits; the texel's v the next 8, u the low 8)
const Bit32u I_SHADE = 0x7CF38;    // the polygon's shade (its high 24 bits, 0x100 a shade, the texel the low 8)
const Bit32u I_COLOUR = 0x7CF2C;   // the polygon's colour (a flat fill)
const Bit32u I_LIT = 0x934E8;      // the light table: shades of 256 colours
const Bit32u I_FOG = 0x944E8;      // the depth fog's table: 17 shades of 256 colours, 1..16 used (0x100 a shade)
const Bit32u I_VARS = 0x68520;     // the routines' variables and the strip (0x68520..0x689A0)
const Bit32u I_VARS_END = 0x689A0;
const Bit32u I_STRIP = 0x68590;    // zbuf_lit's strip of texels
const Bit32u I_CP_COUNT = 0x6828C; // the cockpit: its runs' count ...
const Bit32u I_CP_RUNS = 0xE2DF8;  // ... the runs (start, end in the 320-wide picture: 8 bytes each) ...
const Bit32u I_CP_PIC = 0xDDDF8;   // ... the picture ...
const Bit32u I_CP_DEST = 0x7CF44;  // ... and the 640-wide frame's start

int g_mode = -1;                   // 0 the game's own, 1 the twins, 2 checked
bool g_ready = false;
Bit32u g_delta = 0;                // linear - image, for the data
Bit32u g_ptr = 0;                  // the routine pointer (linear)
Bit32u g_entry[S_COUNT];           // each routine's entry (linear; 0: not twinned)
Bit32u g_top = 0;                  // the end of memory (linear)
inline Bit32u D(Bit32u image) { return image + g_delta; }

// --- memory: the game's RAM is conventional memory (below 0xA0000: the frame buffer the extender gave it there)
// and extended memory (from 1 MB: its code and data) ---------------------------------------------------------------

inline bool ram(Bit32u a, Bit32u n) {
    return (a < 0xA0000u && n <= 0xA0000u - a) || (a >= 0x100000u && a < g_top && n <= g_top - a);
}

struct Region { Bit32u lo, len; };

// A twin's run: its reads checked (a read outside the RAM marks it bad: given back, if nothing is written yet),
// its writes to the memory it declared (and checked) first.  In the check (snap) each region declared is widened
// by kCheckMargin either side and kept as it was, to be put back after the twin (a write of the twin's just
// outside what it declared then shows as a byte the game's code did not write).
struct Run {
    bool bad;
    bool snap;
    Region reg[4];
    std::vector<Bit8u> saved[4];
    int nreg;
    Bit32u first;                   // the first region's length as declared (the span's pixels)
    Run() : bad(false), snap(false), nreg(0), first(0) {}
    inline Bit8u r8(Bit32u a) {
        if (a < 0xA0000u || a - 0x100000u < g_top - 0x100000u) return MemBase[a];
        bad = true;
        return 0;
    }
    inline Bit32u r32(Bit32u a) {
        if (ram(a, 4)) return host_readd(MemBase + a);
        bad = true;
        return 0;
    }
    // the memory it will write: false (given back) if any is outside the RAM
    bool will_write(Bit32u lo, Bit32u len) {
        if (!ram(lo, len) || nreg == 4) return false;
        if (!nreg) first = len;
        if (snap) {
            const bool low = lo < 0xA0000u;
            const Bit32u floor = low ? 0 : 0x100000u, ceil = low ? 0xA0000u : g_top;
            const Bit32u a = std::max<Bit32u>(floor + kCheckMargin, lo) - kCheckMargin;
            const Bit32u b = std::min<Bit32u>(ceil, lo + len + kCheckMargin);
            lo = a;
            len = b - a;
            saved[nreg].assign(MemBase + lo, MemBase + lo + len);
        }
        reg[nreg].lo = lo;
        reg[nreg].len = len;
        nreg++;
        return true;
    }
    inline void w8(Bit32u a, Bit8u v) { MemBase[a] = v; }
    inline void w32(Bit32u a, Bit32u v) { host_writed(MemBase + a, v); }
    void wn(Bit32u a, const Bit8u *p, Bit32u n) { memcpy(MemBase + a, p, n); }
};

// edx:eax = eax << 24 sign-extended, idiv by w: false where the CPU would fault (w 0, the quotient past 32 bits)
inline bool div24(Bit32u a, Bit32u w, Bit32u &q) {
    if (w == 0) return false;
    const Bit64s num = (Bit64s)(Bit32s)a * 16777216;
    const Bit64s r = num / (Bit64s)(Bit32s)w;
    if (r < -(Bit64s)0x80000000LL || r > 0x7FFFFFFFLL) return false;
    q = (Bit32u)(Bit32s)r;
    return true;
}

// cdq, idiv by a count (1..kMaxSpan: never faults)
inline Bit32u div_n(Bit32u a, Bit32u n) { return (Bit32u)((Bit32s)a / (Bit32s)n); }

inline Bit32u sar(Bit32u a, int s) { return (Bit32u)((Bit32s)a >> s); }

// The span's ends (every routine's start): the edges ordered by x (unsigned), the first pixel's address, the count.
struct Ends {
    Bit32u A, B, x0, dst, n;
};
inline bool ends(Run &m, Bit32u esi, Bit32u edi, Bit32u ebx, Ends &e) {
    Bit32u A = esi, B = edi;
    Bit32u xa = m.r32(A + 8), xb = m.r32(B + 8);
    if (xa > xb) {                                  // (cmp eax, ecx; jbe: unsigned)
        std::swap(xa, xb);
        std::swap(A, B);
    }
    e.A = A;
    e.B = B;
    e.x0 = xa >> 16;
    e.n = (xb >> 16) - e.x0 + 1;
    e.dst = m.r32(D(I_ROWS) + ebx * 4) + e.x0;
    return !m.bad && e.n <= kMaxSpan;
}

// --- the perspective spans ----------------------------------------------------------------------------------------

// The depth fog's shade from w (0x100..0x10FF: the shade in the high byte).  A: (w * 4, at most 0xFFFFF) inverted,
// / 256.  B: (w, at most 0xFFFF) inverted, / 8, at most 0xFFF.
template <int FOG> inline Bit32u fog(Bit32u w);
template <> inline Bit32u fog<0>(Bit32u w) {
    Bit32u t = w << 2;
    if (t > 0xFFFFFu) t = 0xFFFFFu;
    t ^= 0xFFFFFu;
    return sar(t, 8) + 0x100;
}
template <> inline Bit32u fog<1>(Bit32u w) {
    Bit32u t = w;
    if (t > 0xFFFFu) t = 0xFFFFu;
    t ^= 0xFFFFu;
    t = sar(t, 3);
    if (t > 0xFFFu) t = 0xFFFu;
    return t + 0x100;
}

// One pixel: ebx's low 16 bits the texel's v (dh) and u (ch), its high 16 the texture's (masked to 64 x 64: and
// ebx, 0xffff3f3f); eax the shade (ebp) with the texel in al, read through the fog's table.
template <bool T64> inline Bit8u fog_pixel(Run &m, Bit32u tex, Bit32u u, Bit32u v, Bit32u l) {
    Bit32u ta = (tex & 0xFFFF0000u) | (v & 0xFF00u) | ((u >> 8) & 0xFFu);
    if (T64) ta &= 0xFFFF3F3Fu;
    const Bit32u ix = (l & 0xFFFFFF00u) | m.r8(ta);
    return m.r8(D(I_FOG) + ix);
}

// The same, its reads unchecked: tb the texture's 64 KB, fb the fog's table (both checked in the RAM first).
template <bool T64> inline Bit8u fog_fast(const Bit8u *tb, const Bit8u *fb, Bit32u u, Bit32u v, Bit32u l) {
    Bit32u ta = (v & 0xFF00u) | ((u >> 8) & 0xFFu);
    if (T64) ta &= 0x3F3Fu;
    return fb[(l & 0xFFFFFF00u) | tb[ta]];
}

// The fog table's rows a run of `count` pixels reads, the shade from l by steps of st (the CPU's 32-bit sums): true
// when none of the sums wraps and every row read is in the RAM (the run's reads then unchecked).
inline bool fog_rows_ok(Bit32u l, Bit32u st, Bit32s count) {
    const Bit64s L0 = (Bit32s)l, L1 = L0 + (Bit64s)(Bit32s)st * (count - 1);
    const Bit64s lo = std::min(L0, L1), hi = std::max(L0, L1);
    if (lo < 0 || hi > 0x7FFFFFFFLL) return false;
    const Bit32u a = D(I_FOG) + (Bit32u)(lo & ~0xFFLL);
    return a >= D(I_FOG) && ram(a, (Bit32u)((hi | 0xFF) - (lo & ~0xFFLL) + 1));
}

// 0x4732E, 0x44C97, 0x46552, 0x444FA: perspective, a divide every 2^LOG pixels (u/w and v/w at each step's ends,
// stepped linearly between, the shade too), the tail's divide at the span's end; T64 the 64 x 64 texture's mask,
// FOG the shade's formula.  The variables (dwords from 0x68528): 0 u*w, 1 v*w, 2 w at the step's start; 3..5 at its
// end; 6..8 at the span's end; 9..11 their steps over 2^LOG pixels; 12, 13 u and v (16.16 of a texel) at the step's
// start; 14, 15 at its end; 18, 19 their per-pixel steps; 21 the shade at the start, 22 at the end, 23 its step.
template <int LOG, bool T64, int FOG>
bool persp(Run &m, Bit32u esi, Bit32u edi, Bit32u ebx) {
    const Bit32s S = 1 << LOG;
    Ends e;
    if (!ends(m, esi, edi, ebx, e)) return false;
    const Bit32u n = e.n;
    Bit32u v28 = m.r32(e.A + 0x18), v40 = m.r32(e.B + 0x18);
    Bit32u v2c = m.r32(e.A + 0x20), v44 = m.r32(e.B + 0x20);
    Bit32u v30 = m.r32(e.A + 0x10), v48 = m.r32(e.B + 0x10);
    const Bit32u tex = m.r32(D(I_TEX));
    if (m.bad) return false;
    const bool texOk = ram(tex & 0xFFFF0000u, 0x10000);
    const Bit8u *tb = MemBase + (tex & 0xFFFF0000u), *fb = MemBase + D(I_FOG);
    const Bit32u v4c = div_n(v40 - v28, n) << LOG;
    const Bit32u v50 = div_n(v44 - v2c, n) << LOG;
    const Bit32u v54 = div_n(v48 - v30, n) << LOG;
    Bit32u v58, v5c;
    if (!div24(v28, v30, v58) || !div24(v2c, v30, v5c)) return false;
    Bit32u v7c = fog<FOG>(v30);
    Bit32u v34 = 0, v38 = 0, v3c = 0, v60 = 0, v64 = 0, v70 = 0, v74 = 0, v80 = 0, v84 = 0;
    bool stepped = false, tailed = false;
    Bit8u px[kMaxSpan];
    Bit32s cnt = (Bit32s)n;
    Bit32u at = 0;
    for (;;) {
        cnt -= S;
        if (cnt < 0) break;
        v34 = v28 + v4c;
        v38 = v2c + v50;
        v3c = v30 + v54;
        if (!div24(v34, v3c, v60) || !div24(v38, v3c, v64)) return false;
        v80 = fog<FOG>(v3c);
        v70 = sar(v60 - v58, 8 + LOG);
        v74 = sar(v64 - v5c, 8 + LOG);
        v84 = sar(v80 - v7c, LOG);
        stepped = true;
        Bit32u u = sar(v58, 8), v = sar(v5c, 8), l = v7c;
        if (texOk && fog_rows_ok(l, v84, S)) {
            for (Bit32s k = 0; k < S; k++) {
                px[at + k] = fog_fast<T64>(tb, fb, u, v, l);
                u += v70;
                v += v74;
                l += v84;
            }
        } else {
            for (Bit32s k = 0; k < S; k++) {
                px[at + k] = fog_pixel<T64>(m, tex, u, v, l);
                u += v70;
                v += v74;
                l += v84;
            }
        }
        at += S;
        v28 = v34; v2c = v38; v30 = v3c;
        v7c = v80; v58 = v60; v5c = v64;
    }
    cnt += S;
    if (cnt != 0) {
        if (!div24(v40, v48, v60) || !div24(v44, v48, v64)) return false;
        v80 = fog<FOG>(v48);
        v70 = sar(div_n(v60 - v58, (Bit32u)cnt), 8);
        v74 = sar(div_n(v64 - v5c, (Bit32u)cnt), 8);
        v84 = div_n(v80 - v7c, (Bit32u)cnt);
        tailed = true;
        // esi: the count less one in its high word, u's step in its low (added to u whole: only u's low 16 bits
        // reach the texel)
        Bit32u st = (v70 & 0xFFFFu) | ((Bit32u)(cnt - 1) << 16);
        Bit32u u = sar(v58, 8), v = sar(v5c, 8), l = v7c;
        if (texOk && fog_rows_ok(l, v84, cnt)) {
            for (;;) {
                px[at++] = fog_fast<T64>(tb, fb, u, v, l);
                u += st;
                v += v74;
                l += v84;
                st -= 0x10000u;
                if ((Bit32s)st < 0) break;
            }
        } else for (;;) {
            px[at++] = fog_pixel<T64>(m, tex, u, v, l);
            u += st;
            v += v74;
            l += v84;
            st -= 0x10000u;
            if ((Bit32s)st < 0) break;
        }
    }
    if (m.bad) return false;
    if (!m.will_write(e.dst, at) || !m.will_write(D(I_VARS), I_VARS_END - I_VARS)) return false;
    m.wn(e.dst, px, at);
    const Bit32u V = D(0x68528);
    m.w32(V + 0, v28); m.w32(V + 4, v2c); m.w32(V + 8, v30);
    m.w32(V + 24, v40); m.w32(V + 28, v44); m.w32(V + 32, v48);
    m.w32(V + 36, v4c); m.w32(V + 40, v50); m.w32(V + 44, v54);
    m.w32(V + 48, v58); m.w32(V + 52, v5c); m.w32(V + 84, v7c);
    if (stepped) {
        m.w32(V + 12, v34); m.w32(V + 16, v38); m.w32(V + 20, v3c);
    }
    if (stepped || tailed) {
        m.w32(V + 56, v60); m.w32(V + 60, v64); m.w32(V + 72, v70); m.w32(V + 76, v74);
        m.w32(V + 88, v80); m.w32(V + 92, v84);
    }
    return true;
}

// --- the affine spans ---------------------------------------------------------------------------------------------

// 0x42F18: u/w and v/w divided at the span's two ends (locals on the stack), stepped between (8.8 in ecx and edx:
// the texel ch, dh), one shade for the span ([0x7CF38]: the texel in its low byte, through the light table).  An odd
// first pixel alone, then four at a time while more than four are left, then at most three (four left: three drawn).
bool affine_lit(Run &m, Bit32u esi_, Bit32u edi_, Bit32u ebx_) {
    Ends e;
    if (!ends(m, esi_, edi_, ebx_, e)) return false;
    const Bit32u Aw = m.r32(e.A + 0x10), Bw = m.r32(e.B + 0x10);
    const Bit32u Au = m.r32(e.A + 0x18), Av = m.r32(e.A + 0x20), Bu = m.r32(e.B + 0x18), Bv = m.r32(e.B + 0x20);
    const Bit32u tex = m.r32(D(I_TEX)), shade = m.r32(D(I_SHADE));
    if (m.bad) return false;
    Bit32u l1c, l20, l24, l28;
    if (!div24(Au, Aw, l1c) || !div24(Av, Aw, l20) || !div24(Bu, Bw, l24) || !div24(Bv, Bw, l28)) return false;
    const Bit32u l8 = sar(l1c, 8), l14 = div_n(sar(l24, 8) - sar(l1c, 8), e.n);
    const Bit32u l4 = sar(l20, 8), l10 = div_n(sar(l28, 8) - sar(l20, 8), e.n);
    const Bit32u texBase = tex & 0xFFFF0000u, lit = D(I_LIT) + (shade & 0xFFFFFF00u);
    if (!ram(texBase, 0x10000) || !ram(lit, 0x100) || !m.will_write(e.dst, e.n)) return false;
    Bit32u esi = (l14 & 0xFFFFu) | ((e.n - 1) << 16);
    Bit32u ecx = l8, edx = l4, edi = e.dst;
    const Bit32u ebp = l10;
#define PX(k) do { \
        const Bit32u ta = texBase | (edx & 0xFF00u) | ((ecx >> 8) & 0xFFu); \
        ecx += esi; \
        edx += ebp; \
        m.w8(edi + (k), MemBase[lit + MemBase[ta]]); \
    } while (0)
    if (edi & 1) {
        PX(0);
        edi++;
        esi -= 0x10000u;
        if ((Bit32s)esi < 0) return true;
    }
    for (;;) {
        esi -= 0x40000u;
        if ((Bit32s)esi < 0) break;
        PX(0); PX(1); PX(2); PX(3);
        edi += 4;
    }
    esi += 0x40000u;
    if ((Bit32s)esi < 0) return true;
    PX(0);
    esi -= 0x10000u;
    if ((Bit32s)esi < 0) return true;
    PX(1);
    esi -= 0x10000u;
    if ((Bit32s)esi < 0) return true;
    {
        const Bit32u ta = texBase | (edx & 0xFF00u) | ((ecx >> 8) & 0xFFu);
        m.w8(edi + 2, MemBase[lit + MemBase[ta]]);
    }
#undef PX
    return true;
}

// 0x480D4: as affine_lit but unlit, the texel 0 not drawn (see-through), and u and v stepped as one packed pair: the
// locals laid out on the stack and read back at odd offsets give ecx = v's top byte : u's low 24 bits, edx = u's top
// byte : v's low 24 bits (u and v 16.16 of a texel shifted up 8), their steps alike in esi and ebp; each pixel adds
// edx + ebp, then ecx + esi with its carry, then edx's carry from that: the texel dl (u), cl (v).  The count in
// eax's high word; after the four-at-a-time loop the tail counts in ah (add ah, 5 ... dec ah: jle).
bool affine_clear(Run &m, Bit32u esi_, Bit32u edi_, Bit32u ebx_) {
    Ends e;
    if (!ends(m, esi_, edi_, ebx_, e)) return false;
    const Bit32u Aw = m.r32(e.A + 0x10), Bw = m.r32(e.B + 0x10);
    const Bit32u Au = m.r32(e.A + 0x18), Av = m.r32(e.A + 0x20), Bu = m.r32(e.B + 0x18), Bv = m.r32(e.B + 0x20);
    const Bit32u tex = m.r32(D(I_TEX));
    if (m.bad) return false;
    Bit32u l1c, l20, l24, l28;
    if (!div24(Au, Aw, l1c) || !div24(Av, Aw, l20) || !div24(Bu, Bw, l24) || !div24(Bv, Bw, l28)) return false;
    // the locals: [ebp-4] l4, [ebp-8] l8, [ebp-0xc] lc, [ebp-0x10] l10, [ebp-0x14] l14, [ebp-0x18] l18
    const Bit32u l8 = l1c << 8, l14 = div_n(l24 - l1c, e.n) << 8;
    const Bit32u l4 = l20 << 8, lc = l4, l10 = div_n(l28 - l20, e.n) << 8, l18 = l10;
    Bit32u ecx = (lc >> 24) | (l8 << 8);           // [ebp-9]
    Bit32u edx = (l8 >> 24) | (l4 << 8);           // [ebp-5]
    const Bit32u esi = (l18 >> 24) | (l14 << 8);   // [ebp-0x15]
    const Bit32u ebp = (l14 >> 24) | (l10 << 8);   // [ebp-0x11]
    const Bit32u texBase = tex & 0xFFFF0000u;
    if (!ram(texBase, 0x10000) || !m.will_write(e.dst, e.n)) return false;
    Bit32u eax = (e.n - 1) << 16;
    Bit32u edi = e.dst;
#define PX(k) do { \
        const Bit32u ta = texBase | ((ecx & 0xFFu) << 8) | (edx & 0xFFu); \
        Bit64u t = (Bit64u)edx + ebp; \
        edx = (Bit32u)t; \
        t = (Bit64u)ecx + esi + (t >> 32); \
        ecx = (Bit32u)t; \
        edx += (Bit32u)(t >> 32); \
        const Bit8u c = MemBase[ta]; \
        eax = (eax & 0xFFFFFF00u) | c; \
        if (c) m.w8(edi + (k), c); \
    } while (0)
    if (edi & 1) {
        PX(0);
        edi++;
        eax -= 0x10000u;
        if ((Bit32s)eax < 0) return true;
    }
    for (;;) {
        eax -= 0x40000u;
        if ((Bit32s)eax < 0) break;
        PX(0); PX(1); PX(2); PX(3);
        edi += 4;
    }
    eax >>= 8;
    int ah = (Bit8s)(Bit8u)(eax >> 8) + 5;         // add ah, 5; jle
    if (ah <= 0) return true;
    PX(0);
    ah = (Bit8s)(Bit8u)ah - 1;                     // dec ah; jle
    if (ah <= 0) return true;
    PX(1);
    ah = (Bit8s)(Bit8u)ah - 1;
    if (ah <= 0) return true;
    {
        const Bit32u ta = texBase | ((ecx & 0xFFu) << 8) | (edx & 0xFFu);
        const Bit8u c = MemBase[ta];
        if (c) m.w8(edi + 2, c);
    }
#undef PX
    return true;
}

// 0x48693: the craft's polygons: u and v (no divide: the edges' own) stepped into a strip of texels (0x68590), four
// at a time while more than four are left, then at most three; then each pixel whose w is more than the depth
// buffer's (signed) drawn through the light table at the polygon's shade and its w kept, the same counts.
bool zbuf_lit(Run &m, Bit32u esi_, Bit32u edi_, Bit32u ebx_) {
    Ends e;
    if (!ends(m, esi_, edi_, ebx_, e)) return false;
    const Bit32u n = e.n;
    const Bit32u zp = m.r32(D(I_ZROWS) + ebx_ * 4) + (e.x0 << 2);
    const Bit32u Aw = m.r32(e.A + 0x10), Bw = m.r32(e.B + 0x10);
    const Bit32u Au = m.r32(e.A + 0x18), Bu = m.r32(e.B + 0x18), Av = m.r32(e.A + 0x20), Bv = m.r32(e.B + 0x20);
    const Bit32u tex = m.r32(D(I_TEX)), shade = m.r32(D(I_SHADE));
    if (m.bad) return false;
    const Bit32u v998 = div_n(Bw - Aw, n);
    const Bit32u v560 = sar(Au, 8), v990 = div_n(sar(Bu - Au, 8), n);
    const Bit32u v564 = sar(Av, 8), v994 = div_n(sar(Bv - Av, 8), n);
    const Bit32u texBase = tex & 0xFFFF0000u, lit = D(I_LIT) + (shade & 0xFFFFFF00u);
    const Bit32u strip = D(I_STRIP);
    if (!ram(texBase, 0x10000) || !ram(lit, 0x100) || !m.will_write(e.dst, n) || !m.will_write(zp, 4 * n) ||
        !m.will_write(D(I_VARS), I_VARS_END - I_VARS) || strip + n > D(I_VARS_END)) {
        return false;
    }
    m.w32(D(0x68998), v998);
    m.w32(D(0x68560), v560);
    m.w32(D(0x68990), v990);
    m.w32(D(0x68564), v564);
    m.w32(D(0x68994), v994);
    m.w32(D(0x68588), e.dst);
    m.w32(D(0x68524), n);
    m.w32(D(0x6856C), Aw);
    m.w32(D(0x6858C), zp);
    // the strip
    Bit32u eax = (n - 1) << 16, ecx = v560, edx = v564, s = strip;
#define TX() (MemBase[texBase | (edx & 0xFF00u) | ((ecx >> 8) & 0xFFu)])
    for (;;) {
        eax -= 0x40000u;
        if ((Bit32s)eax < 0) break;
        for (int k = 0; k < 4; k++) {
            m.w8(s + k, TX());
            ecx += v990;
            edx += v994;
        }
        s += 4;
    }
    eax += 0x40000u;
    if ((Bit32s)eax >= 0) {
        m.w8(s, TX()); ecx += v990; edx += v994;
        eax -= 0x10000u;
        if ((Bit32s)eax >= 0) {
            m.w8(s + 1, TX()); ecx += v990; edx += v994;
            eax -= 0x10000u;
            if ((Bit32s)eax >= 0) m.w8(s + 2, TX());
        }
    }
#undef TX
    // the depth test and the pixels
    Bit32s c = (Bit32s)n;
    Bit32u w = Aw, src = strip, dst = e.dst, z = zp;
#define ZP(k) do { \
        if ((Bit32s)w > (Bit32s)host_readd(MemBase + z + 4 * (k))) { \
            const Bit8u t = MemBase[src + (k)]; \
            m.w32(z + 4 * (k), w); \
            m.w8(dst + (k), MemBase[lit + t]); \
        } \
    } while (0)
    for (;;) {
        c -= 4;
        if (c <= 0) break;
        ZP(0); w += v998;
        ZP(1); w += v998;
        ZP(2); w += v998;
        ZP(3); w += v998;
        src += 4;
        dst += 4;
        z += 16;
    }
    c += 4;
    if (c <= 0) return true;
    ZP(0); w += v998;
    if (--c <= 0) return true;
    ZP(1); w += v998;
    if (--c <= 0) return true;
    ZP(2);
#undef ZP
    return true;
}

// 0x49503: one colour ([0x7CF2C]): rep stosb of (the start's address & 3) bytes, rep stosd of the rest / 4 (the
// dword whole), rep stosb of the rest & 3; fewer than three pixels all by bytes.
bool fill(Run &m, Bit32u esi_, Bit32u edi_, Bit32u ebx_) {
    Ends e;
    if (!ends(m, esi_, edi_, ebx_, e)) return false;
    const Bit32u colour = m.r32(D(I_COLOUR));
    if (m.bad || (reg_flags & FLAG_DF) || !m.will_write(e.dst, e.n)) return false;
    Bit32u edi = e.dst, rest = e.n, lead = e.n;
    if (e.n >= 3) {
        lead = edi & 3;
        rest = e.n - lead;
    } else {
        rest = 0;
    }
    for (Bit32u i = 0; i < lead; i++) m.w8(edi++, (Bit8u)colour);
    if (e.n >= 3) {
        for (Bit32u i = 0; i < (rest >> 2); i++, edi += 4) m.w32(edi, colour);
        for (Bit32u i = 0; i < (rest & 3); i++) m.w8(edi++, (Bit8u)colour);
    }
    return true;
}

typedef bool (*Twin)(Run &m, Bit32u esi, Bit32u edi, Bit32u ebx);

Twin twin_of(int s) {
    switch (s) {
    case S_PERSP32_T64_FOGA: return &persp<5, true, 0>;
    case S_PERSP32_T256_FOGB: return &persp<5, false, 1>;
    case S_PERSP16_T64_FOGA: return &persp<4, true, 0>;
    case S_PERSP16_T256_FOGB: return &persp<4, false, 1>;
    case S_AFFINE_LIT: return &affine_lit;
    case S_AFFINE_CLEAR: return &affine_clear;
    case S_ZBUF_LIT: return &zbuf_lit;
    case S_FILL: return &fill;
    }
    return NULL;
}

// --- the cockpit's copy (an entry) --------------------------------------------------------------------------------

// 0x2FAB0: for each of the runs ([0x6828C] of them, signed), the bytes of the 320-wide picture from its start to its
// end each written twice (a word) into the 640-wide frame from 2 * start.  Its registers: ebx, esi, edi, ebp kept;
// eax, ecx, edx as it leaves them.
struct Regs { Bit32u eax, ecx, edx; };
bool cockpit_copy(Run &m, Regs &r) {
    const Bit32u count = m.r32(D(I_CP_COUNT)), runs = D(I_CP_RUNS), pic = D(I_CP_PIC), dest = m.r32(D(I_CP_DEST));
    if (m.bad) return false;
    Bit32u eax = r.eax, ecx = r.ecx, edx = count;
    if ((Bit32s)count > 0) {
        if (count > 4096 || !ram(runs, count * 8)) return false;
        // what it will write: from the first run's start to the last's end, doubled
        Bit32u lo = 0xFFFFFFFFu, hi = 0;
        for (Bit32u i = 0; i < count; i++) {
            const Bit32u a = host_readd(MemBase + runs + 8 * i), b = host_readd(MemBase + runs + 8 * i + 4);
            if ((Bit32s)(b - a) <= 0) continue;
            if (b - a > 0x100000u || !ram(pic + a, b - a)) return false;
            lo = std::min(lo, dest + 2 * a);
            hi = std::max(hi, dest + 2 * b);
        }
        if (hi > lo && !m.will_write(lo, hi - lo)) return false;
        for (Bit32u i = 0; i < count; i++) {
            edx = host_readd(MemBase + runs + 8 * i);
            eax = host_readd(MemBase + runs + 8 * i + 4) - edx;
            if (eax == 0) continue;
            Bit32u src = pic + edx;
            ecx = edx * 2;
            edx = dest + ecx;
            if ((Bit32s)eax <= 0) continue;
            // (the loop: a byte, the word of it written, edx on by 2, eax down to 0)
            const Bit8u *sp = MemBase + src;
            Bit8u *dp = MemBase + edx;
            const Bit32u k = eax;
            for (Bit32u i = 0; i < k; i++) {
                ecx = (ecx & 0xFFFF0000u) | sp[i];
                ecx += ecx << 8;
                dp[2 * i] = (Bit8u)ecx;
                dp[2 * i + 1] = (Bit8u)(ecx >> 8);
            }
            edx += 2 * k;
            eax = 0;
        }
    }
    r.eax = eax;
    r.ecx = ecx;
    r.edx = edx;
    return true;
}

// --- the calls -----------------------------------------------------------------------------------------------------

struct Stats {
    unsigned native[S_COUNT], given[S_COUNT];       // calls made by the twin; given back to the game's code
    Bit64u pixels[S_COUNT];
    std::map<Bit32u, unsigned> other;               // spans of routines not twinned, by image address
};
Stats g_st;
unsigned g_checked = 0, g_differed = 0;
double g_nextLogMs = 0;

// the check: the memory the twin may change as it left it, the registers it left, where the game's code comes back
struct Check {
    bool pending;
    int s;
    Bit32u ret;                     // the game's code back here: compared
    Bit32u in[7];                   // the registers at the call (eax ecx edx ebx esp ebp esi edi, less esp)
    Bit32u esp;
    Regs out;                       // an entry twin's eax, ecx, edx
    Region reg[4];
    int nreg;
    std::vector<Bit8u> after[4];
} g_check;
unsigned g_logged = 0;

int routine_at(Bit32u lin) {
    for (int s = 1; s < S_COUNT; s++) {
        if (g_entry[s] == lin) return s;
    }
    return -1;
}

void log_stats(double now) {
    if (now < g_nextLogMs) return;
    const bool first = g_nextLogMs == 0;
    g_nextLogMs = now + 10000;
    if (first) return;
    std::string line;
    for (int s = 1; s < S_COUNT; s++) {
        if (!g_st.native[s] && !g_st.given[s]) continue;
        char b[160];
        if (s == S_COCKPIT_COPY) snprintf(b, sizeof(b), " %s %u", kSpans[s].name, g_st.native[s]);
        else snprintf(b, sizeof(b), " %s %u (%.0f px each)", kSpans[s].name, g_st.native[s], g_st.native[s] ? (double)g_st.pixels[s] / g_st.native[s] : 0.0);
        line += b;
        if (g_st.given[s]) {
            snprintf(b, sizeof(b), " +%u given back", g_st.given[s]);
            line += b;
        }
    }
    std::string other;
    for (std::map<Bit32u, unsigned>::const_iterator it = g_st.other.begin(); it != g_st.other.end(); ++it) {
        char b[48];
        snprintf(b, sizeof(b), " %05x:%u", it->first, it->second);
        other += b;
    }
    char chk[96] = "";
    if (g_mode == 2) snprintf(chk, sizeof(chk), "; checked %u, %u differed", g_checked, g_differed);
    wclog(2, "Terminal Velocity native drawing (10 s): %s:%s; the game's own spans (no twin):%s%s",
          g_mode == 2 ? "checked" : "native", line.empty() ? " none" : line.c_str(), other.empty() ? " none" : other.c_str(), chk);
    g_st = Stats();
}

bool flat() {
    return SegPhys(ds) == 0 && SegPhys(es) == 0 && SegPhys(ss) == 0 && SegPhys(cs) == 0 && cpu.stack.big;
}

// The check's first half: the twin has run; what it left in its regions kept, the regions put back as they were,
// the game's code to run and be compared at `ret`.
void check_begin(int s, Run &m, Bit32u ret, const Regs *out) {
    g_check.pending = true;
    g_check.s = s;
    g_check.ret = ret;
    g_check.in[0] = reg_eax; g_check.in[1] = reg_ecx; g_check.in[2] = reg_edx; g_check.in[3] = reg_ebx;
    g_check.in[4] = reg_ebp; g_check.in[5] = reg_esi; g_check.in[6] = reg_edi;
    g_check.esp = reg_esp;
    if (out) g_check.out = *out;
    g_check.nreg = m.nreg;
    for (int i = 0; i < m.nreg; i++) {
        g_check.reg[i] = m.reg[i];
        g_check.after[i].assign(MemBase + m.reg[i].lo, MemBase + m.reg[i].lo + m.reg[i].len);
    }
    // (put back: every region as it was before the twin, all kept before its first write)
    for (int i = 0; i < m.nreg; i++) memcpy(MemBase + m.reg[i].lo, &m.saved[i][0], m.reg[i].len);
    // (the game's own code back at ret: watched)
    wc_net_watch32[ret & 0xFFFF] = true;
}

}  // namespace

void tv_span_reset() {
    g_ready = false;
    memset(g_entry, 0, sizeof(g_entry));
    g_ptr = 0;
    g_check.pending = false;
}

bool tv_span_setup(Bit32u codeDelta) {
    tv_span_reset();
    if (g_mode < 0) {
        const char *e = getenv("WCNET_TV_SPANS");
        g_mode = !e || !e[0] ? 1 : strcmp(e, "check") == 0 ? 2 : atoi(e) ? 1 : 0;
    }
    if (g_mode == 0) {
        wclog(1, "Terminal Velocity: the spans drawn by the game's own routines (WCNET_TV_SPANS=0)");
        return false;
    }
    g_top = (Bit32u)MEM_TotalPages() << 12;
    bool haveDelta = false;
    std::string names;
    for (int s = 0; s < S_COUNT; s++) {
        const Span &sp = kSpans[s];
        const Bit32u lin = sp.image + codeDelta;
        if (!ram(lin, (Bit32u)sp.len)) continue;
        bool same = true;
        for (int i = 0; i < sp.len && same; i++) {
            if (sp.mask[i] && MemBase[lin + (Bit32u)i] != sp.bytes[i]) same = false;
        }
        for (int r = 0; r < sp.nrel && same; r++) {
            const Bit32u d = host_readd(MemBase + lin + sp.rel[2 * r]) - sp.rel[2 * r + 1];
            if (!haveDelta) {
                g_delta = d;
                haveDelta = true;
            } else if (d != g_delta) {
                same = false;
            }
        }
        if (!same) {
            wclog(1, "Terminal Velocity: the routine '%s' (image %05x) is not the shareware's: the game's own code makes it", sp.name, sp.image);
            continue;
        }
        g_entry[s] = lin;
        if (s == S_DISPATCH) {
            g_ptr = sp.rel[1] + g_delta;
        } else {
            names += names.empty() ? "" : ", ";
            names += sp.name;
        }
    }
    if (!g_entry[S_DISPATCH] || names.empty()) {
        wclog(1, "Terminal Velocity: the routines are not the shareware's: the game's own code draws everything");
        return false;
    }
    g_ready = true;
    g_nextLogMs = 0;
    g_st = Stats();
    wclog(1, "Terminal Velocity: drawn natively%s: %s (the data moved by %08x)", g_mode == 2 ? " and checked against the game's own code" : "",
          names.c_str(), g_delta);
    return true;
}

bool tv_span_checking() { return g_ready && g_mode == 2; }

// The padding after the empty function 0x49710 (its ret at the dispatch's +9; 15 zero bytes, never run, checked
// with the dispatch): 8 bytes for the frame cap's idle (wcnet_tv.cpp).  0: none.
Bit32u tv_span_spare() {
    if (!g_ready || !g_entry[S_DISPATCH]) return 0;
    const Bit32u at = g_entry[S_DISPATCH] + 10;
    return MemBase[at - 1] == 0xC3 ? at : 0;
}

Bit32u tv_native_entry_place() { return g_ready ? g_entry[S_COCKPIT_COPY] : 0; }

bool tv_span_call() {
    if (!g_ready || g_check.pending) return false;
    log_stats(PIC_FullIndex());
    if (!ram(g_ptr, 4)) return false;
    const Bit32u r = host_readd(MemBase + g_ptr);
    const int s = routine_at(r);
    Twin t = s > 0 ? twin_of(s) : NULL;
    if (!t) {
        g_st.other[r - g_entry[S_DISPATCH] + kSpans[S_DISPATCH].image]++;
        return false;
    }
    if (!flat()) {
        g_st.given[s]++;
        return false;
    }
    Run m;
    m.snap = g_mode == 2;
    if (!t(m, reg_esi, reg_edi, reg_ebx)) {
        // (nothing written: every twin gives back before its first write)
        g_st.given[s]++;
        return false;
    }
    g_st.native[s]++;
    g_st.pixels[s] += m.first;
    if (g_mode == 2) {
        check_begin(s, m, reg_eip + 5, NULL);
        return false;
    }
    return true;
}

bool tv_native_entry() {
    if (!g_ready || g_check.pending || reg_eip != g_entry[S_COCKPIT_COPY]) return false;
    const int s = S_COCKPIT_COPY;
    if (!flat()) {
        g_st.given[s]++;
        return false;
    }
    Run m;
    m.snap = g_mode == 2;
    Regs r = { reg_eax, reg_ecx, reg_edx };
    if (!cockpit_copy(m, r)) {
        g_st.given[s]++;
        return false;
    }
    g_st.native[s]++;
    if (g_mode == 2) {
        check_begin(s, m, host_readd(MemBase + reg_esp), &r);
        return false;
    }
    reg_eax = r.eax;
    reg_ecx = r.ecx;
    reg_edx = r.edx;
    reg_eip = CPU_Pop32();          // (its ret)
    return true;
}

bool tv_span_returned(Bit32u eip) {
    if (!g_check.pending || eip != g_check.ret) return false;
    g_check.pending = false;
    g_checked++;
    int firstDiff = -1, diffs = 0, where = -1;
    for (int i = 0; i < g_check.nreg; i++) {
        const Region &R = g_check.reg[i];
        for (Bit32u k = 0; k < R.len; k++) {
            if (MemBase[R.lo + k] != g_check.after[i][k]) {
                if (firstDiff < 0) {
                    firstDiff = (int)k;
                    where = i;
                }
                diffs++;
            }
        }
    }
    // the registers: those the routine keeps; an entry's eax, ecx, edx as the twin left them
    const Bit32u nowIn[7] = { reg_eax, reg_ecx, reg_edx, reg_ebx, reg_ebp, reg_esi, reg_edi };
    int regDiff = -1;
    const bool entry = g_check.s == S_COCKPIT_COPY;
    for (int i = entry ? 0 : 4; i < 7 && regDiff < 0; i++) {
        const Bit32u want = entry && i < 3 ? (i == 0 ? g_check.out.eax : i == 1 ? g_check.out.ecx : g_check.out.edx) : g_check.in[i];
        if (!entry && (i == 5 || i == 6)) continue;     // (the filler reloads esi and edi; the routines keep ebp)
        if (nowIn[i] != want) regDiff = i;
    }
    if (reg_esp != g_check.esp + (entry ? 4 : 0)) regDiff = 7;
    if (!diffs && regDiff < 0) return true;
    g_differed++;
    if (g_logged < 20) {
        g_logged++;
        const Region &R = g_check.reg[where < 0 ? 0 : where];
        wclog(1, "Terminal Velocity check: %s (esi %08x edi %08x ebx %08x): %d bytes differ, the first at %08x (game %02x twin %02x); "
                 "register %d %s",
              kSpans[g_check.s].name, g_check.in[5], g_check.in[6], g_check.in[3], diffs,
              firstDiff >= 0 ? R.lo + (Bit32u)firstDiff : 0, firstDiff >= 0 ? MemBase[R.lo + firstDiff] : 0,
              firstDiff >= 0 ? g_check.after[where][firstDiff] : 0, regDiff, regDiff >= 0 ? "differs" : "the same");
    }
    return true;
}

}  // namespace wc
