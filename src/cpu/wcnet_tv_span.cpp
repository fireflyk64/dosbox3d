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
#include "lazyflags.h"
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

// 0x42F13: no pixels (push esi; push edi; pop edi; pop esi; ret)
bool none(Run &, Bit32u, Bit32u, Bit32u) { return true; }

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
    case S_NONE: return &none;
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

// --- the terrain's height (an entry) ------------------------------------------------------------------------------

const Bit32u I_HMAP = 0x104DA0;    // the level's height map (256 x 256 bytes: a row by x, a byte by z)

// imul then shrd eax, edx, 16: the product's bits 16..47; hi its high dword (edx after the imul)
inline Bit32u imul16(Bit32u a, Bit32u b, Bit32u &hi) {
    const Bit64s p = (Bit64s)(Bit32s)a * (Bit64s)(Bit32s)b;
    hi = (Bit32u)((Bit64u)p >> 32);
    return (Bit32u)(Bit64u)(p >> 16);
}

// 0x3C7E0 (x, z): the ground's height under a point (20 bits of a cell's fraction, 8 of the cell): the cell's four
// heights (<< 15) interpolated over the half of the cell the point is in, the cell cut along the diagonal its
// parities choose.  Pure: it reads the map, writes only its own stack.  eax the height; ecx and edx as it leaves them.
bool height(Run &m, Bit32u a, Bit32u b, Regs &o) {
    const Bit32u map = m.r32(D(I_HMAP));
    if (m.bad || !ram(map, 0x10000)) return false;
    const Bit8u *H = MemBase + map;
    const Bit32u fb = (b & 0xFFFFFu) >> 4, fa = (a & 0xFFFFFu) >> 4;
    const Bit32u ib = b >> 20, ia = a >> 20;
    const Bit32u ib8 = ib & 0xFFu, ia8 = ia & 0xFFu, ib1 = (ib8 + 1) & 0xFFu, ia1 = (ia8 + 1) & 0xFFu;
    const Bit32u i00 = (ia8 << 8) + ib8, i01 = (ia8 << 8) + ib1, i11 = (ia1 << 8) + ib1, i10 = ib8 + (ia1 << 8);
    Bit32u hi, ecx, edx, base, eax = fb;
    if ((ib & 1) == (ia & 1)) {
        if ((Bit32s)fb > (Bit32s)fa) {
            const Bit32u h00 = (Bit32u)H[i00] << 15, h01 = (Bit32u)H[i01] << 15, h11 = (Bit32u)H[i11] << 15;
            base = h00 + imul16(eax, h01 - h00, hi);
            ecx = h11 - h01;
        } else {
            const Bit32u h00 = (Bit32u)H[i00] << 15, h11 = (Bit32u)H[i11] << 15, h10 = (Bit32u)H[i10] << 15;
            base = h00 + imul16(eax, h11 - h10, hi);
            ecx = h10 - h00;
        }
    } else {
        eax = 0x10000u - fb;
        if ((Bit32s)eax > (Bit32s)fa) {
            const Bit32u h00 = (Bit32u)H[i00] << 15, h01 = (Bit32u)H[i01] << 15, h10 = (Bit32u)H[i10] << 15;
            base = h01 + imul16(eax, h00 - h01, hi);
            ecx = h10 - h00;
        } else {
            const Bit32u h01 = (Bit32u)H[i01] << 15, h11 = (Bit32u)H[i11] << 15, h10 = (Bit32u)H[i10] << 15;
            base = h01 + imul16(eax, h10 - h11, hi);
            ecx = h11 - h01;
        }
    }
    o.eax = imul16(fa, ecx, edx) + base;
    o.ecx = ecx;
    o.edx = edx;
    return true;
}

// --- the polygon filler (an entry) --------------------------------------------------------------------------------

const Bit32u I_EDGES = 0x104DA8;   // the edges: 16 of 0x28 bytes (+0 top row, +4 bottom row, +8 x, +0x10 w, +0x18 u,
                                   // +0x20 v, each value's step a row after it), then the filler's own:
const Bit32u I_F_SPANS = 0x105028; // spans drawn (a count)
const Bit32u I_F_POLYS = 0x10502C; // polygons filled (a count)
const Bit32u I_F_YMAX = 0x105030;  // the polygon's bottom row
const Bit32u I_F_NEDGE = 0x105034; // its edges
const Bit32u I_F_YMIN = 0x105038;  // its top row
const Bit32u I_F_END = 0x10503C;
const Bit32u I_KX = 0x7CF30, I_KY = 0x7CF34, I_CX = 0x7CF40, I_CY = 0x7CF48;     // the projection
const Bit32u I_VIEW_W = 0x64D34, I_VIEW_H = 0x64D38;                              // the view's size
enum { kMaxEdges = 16 };

// 0x3E510's 16.16 divide: |a| (one's complement: a ^ its sign) / d unsigned, the whole part at most 0x7FFF, the
// fraction from the remainder, the sign put back the same way.
inline Bit32u sdiv16(Bit32u a, Bit32u d) {
    const Bit32u s = (Bit32s)a < 0 ? 0xFFFFFFFFu : 0;
    const Bit32u x = a ^ s;
    Bit32u q = x / d;
    const Bit32u r = x % d;
    if (q >= 0x7FFFu) q = 0x7FFFu;
    const Bit32u q2 = (Bit32u)(((Bit64u)r << 32) / d);
    return ((q << 16) | (q2 >> 16)) ^ s;
}

inline Bit32u rd(Bit32u a) { return host_readd(MemBase + a); }
inline void wr(Bit32u a, Bit32u v) { host_writed(MemBase + a, v); }

// 0x3E510 (A, B): the edge from A to B (its rows' ends, the polygon's top and bottom kept), its values at A and their
// steps a row (B's less A's over the rows' 16.16 distance), stepped on to the first row's centre; none if A and B are
// on one row.  (Never faults: the distance is not 0 between two rows.)
void poly_edge(Bit32u A, Bit32u B) {
    Bit32s top = (Bit32s)rd(A + 0x1C) >> 16, bot = (Bit32s)rd(B + 0x1C) >> 16;
    if (top == bot) return;
    if ((Bit32s)rd(A + 0x1C) > (Bit32s)rd(B + 0x1C)) {
        std::swap(A, B);
        std::swap(top, bot);
    }
    const Bit32u idx = rd(D(I_F_NEDGE)), e = D(I_EDGES) + idx * 0x28;
    wr(e, (Bit32u)top);
    wr(e + 4, (Bit32u)bot);
    if (top < (Bit32s)rd(D(I_F_YMIN))) wr(D(I_F_YMIN), (Bit32u)top);
    if (bot > (Bit32s)rd(D(I_F_YMAX))) wr(D(I_F_YMAX), (Bit32u)bot);
    wr(e + 8, rd(A + 0x18));
    wr(e + 0x18, rd(A + 0xC));
    wr(e + 0x20, rd(A + 0x10));
    wr(e + 0x10, rd(A + 0x14));
    const Bit32u dy = rd(B + 0x1C) - rd(A + 0x1C);
    wr(e + 0xC, sdiv16(rd(B + 0x18) - rd(A + 0x18), dy));
    wr(e + 0x1C, sdiv16(rd(B + 0xC) - rd(A + 0xC), dy));
    wr(e + 0x24, sdiv16(rd(B + 0x10) - rd(A + 0x10), dy));
    wr(e + 0x14, sdiv16(rd(B + 0x14) - rd(A + 0x14), dy));
    const Bit32u f = (rd(A + 0x1C) & 0xFFFFu) ^ 0xFFFFu;
    Bit32u hi;
    wr(e + 8, rd(e + 8) + imul16(f, rd(e + 0xC), hi));
    wr(e + 0x18, rd(e + 0x18) + imul16(f, rd(e + 0x1C), hi));
    wr(e + 0x20, rd(e + 0x20) + imul16(f, rd(e + 0x24), hi));
    wr(e + 0x10, rd(e + 0x10) + imul16(f, rd(e + 0x14), hi));
    wr(D(I_F_NEDGE), idx + 1);
}

// 0x3E6E0 (row, but): the first edge starting at the row that is not `but` (0: none)
Bit32u poly_find(Bit32u y, Bit32u but) {
    const Bit32s n = (Bit32s)rd(D(I_F_NEDGE));
    for (Bit32s i = 0; i < n; i++) {
        const Bit32u e = D(I_EDGES) + 0x28 * (Bit32u)i;
        if (rd(e) == y && e != but) return e;
    }
    return 0;
}

// an edge a row on: x, u, v, w by their steps
inline void poly_step(Bit32u e) {
    wr(e + 8, rd(e + 8) + rd(e + 0xC));
    wr(e + 0x18, rd(e + 0x18) + rd(e + 0x1C));
    wr(e + 0x20, rd(e + 0x20) + rd(e + 0x24));
    wr(e + 0x10, rd(e + 0x10) + rd(e + 0x14));
}

// The memory a check keeps: regions and what they held.
struct Snap {
    std::vector<Region> reg;
    std::vector<std::vector<Bit8u> > saved;
    void add(Bit32u lo, Bit32u len) {
        if (!len || !ram(lo, len)) return;
        Region r = { lo, len };
        reg.push_back(r);
        saved.push_back(std::vector<Bit8u>(MemBase + lo, MemBase + lo + len));
    }
    void put_back() {
        for (size_t i = reg.size(); i-- > 0;) memcpy(MemBase + reg[i].lo, &saved[i][0], reg[i].len);
    }
};

// --- the calls -----------------------------------------------------------------------------------------------------

struct Stats {
    unsigned native[S_COUNT], given[S_COUNT];       // calls made by the twin; given back to the game's code
    Bit64u pixels[S_COUNT];
    unsigned resumed;                               // polygons handed to the game's code at a row (a span given back)
    unsigned polySpans;                             // spans drawn inside native polygons
    std::map<Bit32u, unsigned> other;               // spans of routines not twinned, by image address
};
Stats g_st;
unsigned g_checked = 0, g_differed = 0, g_unchecked = 0;
double g_nextLogMs = 0;

// the check: the memory the twin may change as it left it, the registers it left, where the game's code comes back
struct Check {
    bool pending;
    int s;
    bool entry;                     // an entry: its ret (esp + 4), eax ecx edx as the twin left them
    Bit32u ret;                     // the game's code back here: compared
    Bit32u in[7];                   // the registers at the call (eax ecx edx ebx ebp esi edi)
    Bit32u esp;
    Regs out;
    bool flagsKnown;                // the flags the twin left (FMASK_TEST), compared
    Bit32u flags;
    std::vector<Region> reg;
    std::vector<std::vector<Bit8u> > after;
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
        char b[200];
        if (s == S_POLY) snprintf(b, sizeof(b), " %s %u (%u spans in them, %u handed over at a row)", kSpans[s].name, g_st.native[s], g_st.polySpans, g_st.resumed);
        else if (s == S_COCKPIT_COPY || s == S_HEIGHT) snprintf(b, sizeof(b), " %s %u", kSpans[s].name, g_st.native[s]);
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
    char chk[128] = "";
    if (g_mode == 2) snprintf(chk, sizeof(chk), "; checked %u, %u differed, %u not checked", g_checked, g_differed, g_unchecked);
    wclog(2, "Terminal Velocity native drawing (10 s): %s:%s; the game's own spans (no twin):%s%s",
          g_mode == 2 ? "checked" : "native", line.empty() ? " none" : line.c_str(), other.empty() ? " none" : other.c_str(), chk);
    g_st = Stats();
}

bool flat() {
    return SegPhys(ds) == 0 && SegPhys(es) == 0 && SegPhys(ss) == 0 && SegPhys(cs) == 0 && cpu.stack.big;
}

// add's flags (CF PF AF ZF SF OF) for a + b
Bit32u add_flags(Bit32u a, Bit32u b) {
    const Bit32u r = a + b;
    Bit32u f = 0;
    if (r < a) f |= FLAG_CF;
    if (r == 0) f |= FLAG_ZF;
    if (r & 0x80000000u) f |= FLAG_SF;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) f |= FLAG_OF;
    if ((a ^ b ^ r) & 0x10u) f |= FLAG_AF;
    Bit32u p = r & 0xFFu;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    if (!(p & 1)) f |= FLAG_PF;
    return f;
}

void set_flags(Bit32u f) {
    reg_flags = (reg_flags & ~FMASK_TEST) | f;
    lflags.type = t_UNKNOWN;
}

// The check's first half: the twin has run; what it left in the regions kept, the regions put back as they were
// (snap), the game's code to run and be compared at `ret`.
void check_begin(int s, Snap &snap, Bit32u ret, bool entry, const Regs *out, bool flagsKnown, Bit32u flags) {
    g_check.pending = true;
    g_check.s = s;
    g_check.entry = entry;
    g_check.ret = ret;
    g_check.in[0] = reg_eax; g_check.in[1] = reg_ecx; g_check.in[2] = reg_edx; g_check.in[3] = reg_ebx;
    g_check.in[4] = reg_ebp; g_check.in[5] = reg_esi; g_check.in[6] = reg_edi;
    g_check.esp = reg_esp;
    if (out) g_check.out = *out;
    g_check.flagsKnown = flagsKnown;
    g_check.flags = flags;
    g_check.reg = snap.reg;
    g_check.after.resize(snap.reg.size());
    for (size_t i = 0; i < snap.reg.size(); i++) {
        g_check.after[i].assign(MemBase + snap.reg[i].lo, MemBase + snap.reg[i].lo + snap.reg[i].len);
    }
    snap.put_back();
    wc_net_watch32[ret & 0xFFFF] = true;    // (the game's own code back at ret: watched)
}

// a span routine's Run as a Snap (its regions, as kept before its first write)
void snap_of(Run &m, Snap &snap) {
    for (int i = 0; i < m.nreg; i++) {
        snap.reg.push_back(m.reg[i]);
        snap.saved.push_back(m.saved[i]);
    }
}

// The polygon filler, natively.  -1: given back at its entry (nothing written); 0: handed to the game's code at a
// row's span call (a span twin gave back: the filler's frame and registers made as its own code has them there);
// 1: filled, returned from.  snap (the check): the memory it may change kept first; a hand-over there is -2 (all put
// back, the game's code fills the polygon from its entry).  handAt >= 0 (the check's test of the hand-over): handed
// over before that many rows are drawn, as if the span twin had given back there.
int poly(Snap *snap, Regs &out, int handAt = -1) {
    const Bit32u E = reg_esp;
    if (!ram(E, 12)) return -1;
    const Bit32u verts = rd(E + 4), n = rd(E + 8);
    if ((Bit32s)n > kMaxEdges || ((Bit32s)n > 0 && !ram(verts, n * 0x20))) return -1;
    if (!ram(D(I_EDGES), I_F_END - I_EDGES) || !ram(D(I_VARS), I_VARS_END - I_VARS)) return -1;
    if (!ram(g_ptr, 4)) return -1;
    const int s = routine_at(rd(g_ptr));
    Twin t = s > 0 ? twin_of(s) : NULL;
    if (!t) return -1;
    // the projection of the vertices not yet projected (x 0x18, y 0x1C = k * x / z + the centre): its divides first
    const Bit32u kx = rd(D(I_KX)), ky = rd(D(I_KY)), cx = rd(D(I_CX)), cy = rd(D(I_CY));
    Bit32u px[kMaxEdges], py[kMaxEdges];
    for (Bit32s i = 0; i < (Bit32s)n; i++) {
        const Bit32u v = verts + 0x20 * (Bit32u)i;
        if (rd(v + 0x18) != 0xFFFFFFFFu) continue;
        const Bit32s z = (Bit32s)rd(v + 8);
        if (z == 0) return -1;
        const Bit64s qx = (Bit64s)(Bit32s)rd(v) * (Bit32s)kx / z, qy = (Bit64s)(Bit32s)rd(v + 4) * (Bit32s)ky / z;
        if (qx < -(Bit64s)0x80000000LL || qx > 0x7FFFFFFFLL || qy < -(Bit64s)0x80000000LL || qy > 0x7FFFFFFFLL) return -1;
        px[i] = (Bit32u)(Bit32s)qx + cx;
        py[i] = (Bit32u)(Bit32s)qy + cy;
    }
    if (snap) {
        if ((Bit32s)n > 0) snap->add(verts, n * 0x20);
        snap->add(D(I_EDGES), I_F_END - I_EDGES);
        snap->add(D(I_VARS), I_VARS_END - I_VARS);
    }
    wr(D(I_F_POLYS), rd(D(I_F_POLYS)) + 1);
    for (Bit32s i = 0; i < (Bit32s)n; i++) {
        const Bit32u v = verts + 0x20 * (Bit32u)i;
        if (rd(v + 0x18) != 0xFFFFFFFFu) continue;
        wr(v + 0x18, px[i]);
        wr(v + 0x1C, py[i]);
    }
    wr(D(I_F_YMIN), rd(D(I_VIEW_H)));
    wr(D(I_F_YMAX), 0);
    wr(D(I_F_NEDGE), 0);
    for (Bit32s i = 0; i < (Bit32s)n; i++) {
        const Bit32s j = i + 1 < (Bit32s)n ? i + 1 : 0;
        poly_edge(verts + 0x20 * (Bit32u)i, verts + 0x20 * (Bit32u)j);
    }
    if (snap) {
        // the rows it may draw (the frame buffer's, and the depth buffer's for the craft's spans), 64 bytes either side
        const Bit32s y0 = (Bit32s)rd(D(I_F_YMIN)), y1 = (Bit32s)rd(D(I_F_YMAX)), h = (Bit32s)rd(D(I_VIEW_H));
        const Bit32u w = rd(D(I_VIEW_W));
        if (y0 < 0 || y1 >= h || w > 4096) {
            snap->put_back();
            return -2;
        }
        for (int z = 0; z < (s == S_ZBUF_LIT ? 2 : 1); z++) {
            Bit32u lo = 0xFFFFFFFFu, hi = 0;
            const Bit32u tab = D(z ? I_ZROWS : I_ROWS), rowLen = z ? 4 * w : w;
            for (Bit32s y = y0; y <= y1; y++) {
                if (!ram(tab + 4 * (Bit32u)y, 4)) continue;
                lo = std::min(lo, rd(tab + 4 * (Bit32u)y));
                hi = std::max(hi, rd(tab + 4 * (Bit32u)y) + rowLen);
            }
            if (hi > lo) {
                const Bit32u a = lo >= kCheckMargin ? lo - kCheckMargin : 0;
                snap->add(a, hi + kCheckMargin - a);
            }
        }
    }
    // the rows: each the span between the two edges at it, the edges stepped; an edge past its bottom row replaced by
    // the next starting there
    Bit32u y = rd(D(I_F_YMIN));
    Bit32u e1 = poly_find(y, 0), e2 = e1 ? poly_find(y, e1) : 0;
    int rows = 0;
    while (e1 && e2) {
        if ((Bit32s)y >= (Bit32s)rd(e1 + 4)) {
            wr(e1, 0xFFFFFFFFu);
            e1 = poly_find(y, e2);
            if (!e1) break;
        }
        if ((Bit32s)y >= (Bit32s)rd(e2 + 4)) {
            wr(e2, 0xFFFFFFFFu);
            e2 = poly_find(y, e1);
            if (!e2) break;
        }
        Run sm;
        if (rows++ == handAt || !t(sm, e1, e2, y)) {
            if (rows - 1 != handAt) g_st.given[s]++;
            if (snap) {
                snap->put_back();
                return -2;
            }
            // the filler's frame at its span call (0x3E85D): ebx esi edi ebp pushed, [esp] the row, [esp+4] the first
            // edge; esi the first edge, edi and ebp the second, ebx the row
            wr(E - 4, reg_ebx);
            wr(E - 8, reg_esi);
            wr(E - 12, reg_edi);
            wr(E - 16, reg_ebp);
            wr(E - 20, e1);
            wr(E - 24, y);
            reg_esp = E - 24;
            reg_esi = e1;
            reg_edi = reg_ebp = e2;
            reg_ebx = y;
            reg_eax = y;
            g_st.resumed++;
            return 0;
        }
        g_st.native[s]++;
        g_st.pixels[s] += sm.first;
        g_st.polySpans++;
        wr(D(I_F_SPANS), rd(D(I_F_SPANS)) + 1);
        poly_step(e1);
        poly_step(e2);
        y++;
    }
    // (returned: eax 0 from the last edge sought, ecx and edx the edges' count as its loop left them)
    const Bit32u ne = rd(D(I_F_NEDGE));
    out.eax = 0;
    out.ecx = ne;
    out.edx = (Bit32s)ne > 0 ? ne : 0;
    return 1;
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
    wclog(1, "Terminal Velocity: made natively%s: %s (the data moved by %08x)", g_mode == 2 ? " and checked against the game's own code" : "",
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

int tv_native_entry_places(Bit32u out[3]) {
    if (!g_ready) return 0;
    out[0] = g_entry[S_COCKPIT_COPY];
    out[1] = g_entry[S_HEIGHT];
    out[2] = g_entry[S_POLY] ? g_entry[S_POLY] + (0x3E720 - kSpans[S_POLY].image) : 0;
    return 3;
}

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
        Snap snap;
        snap_of(m, snap);
        check_begin(s, snap, reg_eip + 5, false, NULL, false, 0);
        return false;
    }
    return true;
}

bool tv_native_entry() {
    if (!g_ready || g_check.pending) return false;
    const Bit32u eip = reg_eip, E = reg_esp;
    int s = -1;
    if (eip == g_entry[S_COCKPIT_COPY]) s = S_COCKPIT_COPY;
    else if (eip == g_entry[S_HEIGHT]) s = S_HEIGHT;
    else if (g_entry[S_POLY] && eip == g_entry[S_POLY] + (0x3E720 - kSpans[S_POLY].image)) s = S_POLY;
    if (s < 0) return false;
    if (!flat() || !ram(E, 12)) {
        g_st.given[s]++;
        return false;
    }
    const bool check = g_mode == 2;
    Regs r = { reg_eax, reg_ecx, reg_edx };
    bool flagsKnown = false;
    Bit32u flags = 0;
    Snap snap;
    if (s == S_COCKPIT_COPY) {
        Run m;
        m.snap = check;
        if (!cockpit_copy(m, r)) {
            g_st.given[s]++;
            return false;
        }
        snap_of(m, snap);
        g_st.native[s]++;
    } else if (s == S_HEIGHT) {
        Run m;
        if (!height(m, rd(E + 4), rd(E + 8), r)) {
            g_st.given[s]++;
            return false;
        }
        // (its last flags: add esp, 0xc before its pops)
        flagsKnown = true;
        flags = add_flags(E - 0x1C, 0xC);
        g_st.native[s]++;
    } else {
        log_stats(PIC_FullIndex());
        static const bool handTest = getenv("WCNET_TV_RESUME_TEST") != NULL;
        if (check && handTest) {
            // (the hand-over tested: the twin's whole polygon kept as what the game's code must end with, put back,
            // the twin again handing over after 1..3 rows, the game's code finishing it, compared at its return)
            const int got = poly(&snap, r);
            if (got < 0) {
                if (got == -1) g_st.given[s]++;
                else g_unchecked++;
                return false;
            }
            check_begin(s, snap, rd(E), true, &r, true, add_flags(E - 0x18, 8));
            static unsigned turn = 0;
            Regs r2 = { reg_eax, reg_ecx, reg_edx };
            const int got2 = poly(NULL, r2, 1 + (int)(turn++ % 3));
            if (got2 == 0) {
                reg_eip = g_entry[S_POLY] + (0x3E85D - kSpans[S_POLY].image);
                return true;
            }
            // (too few rows to hand over: filled natively again; returned from as in play)
            g_check.pending = false;
            g_unchecked++;
            reg_eax = r2.eax;
            reg_ecx = r2.ecx;
            reg_edx = r2.edx;
            set_flags(add_flags(E - 0x18, 8));
            reg_eip = CPU_Pop32();
            return true;
        }
        const int got = poly(check ? &snap : NULL, r);
        if (got == -1) {
            g_st.given[s]++;
            return false;
        }
        if (got == -2) {
            g_unchecked++;
            return false;
        }
        if (got == 0) {
            reg_eip = g_entry[S_POLY] + (0x3E85D - kSpans[S_POLY].image);    // (its span call, in its own code)
            return true;
        }
        // (its last flags: add esp, 8 before its pops)
        flagsKnown = true;
        flags = add_flags(E - 0x18, 8);
        g_st.native[s]++;
    }
    if (check) {
        check_begin(s, snap, rd(E), true, &r, flagsKnown, flags);
        return false;
    }
    reg_eax = r.eax;
    reg_ecx = r.ecx;
    reg_edx = r.edx;
    if (flagsKnown) set_flags(flags);
    reg_eip = CPU_Pop32();          // (its ret)
    return true;
}

bool tv_span_returned(Bit32u eip) {
    if (!g_check.pending || eip != g_check.ret) return false;
    if (reg_esp != g_check.esp + (g_check.entry ? 4 : 0)) return false;    // (a deeper call's return to the same place)
    g_check.pending = false;
    g_checked++;
    int firstDiff = -1, diffs = 0, where = -1;
    for (size_t i = 0; i < g_check.reg.size(); i++) {
        const Region &R = g_check.reg[i];
        for (Bit32u k = 0; k < R.len; k++) {
            if (MemBase[R.lo + k] != g_check.after[i][k]) {
                if (firstDiff < 0) {
                    firstDiff = (int)k;
                    where = (int)i;
                }
                diffs++;
            }
        }
    }
    // the registers: those the routine keeps; an entry's eax, ecx, edx as the twin left them, and its flags
    const Bit32u nowIn[7] = { reg_eax, reg_ecx, reg_edx, reg_ebx, reg_ebp, reg_esi, reg_edi };
    int regDiff = -1;
    const bool entry = g_check.entry;
    for (int i = entry ? 0 : 4; i < 7 && regDiff < 0; i++) {
        const Bit32u want = entry && i < 3 ? (i == 0 ? g_check.out.eax : i == 1 ? g_check.out.ecx : g_check.out.edx) : g_check.in[i];
        if (!entry && (i == 5 || i == 6)) continue;     // (the filler reloads esi and edi; the routines keep ebp)
        if (nowIn[i] != want) regDiff = i;
    }
    if (g_check.flagsKnown && regDiff < 0) {
        FillFlags();
        if ((reg_flags & FMASK_TEST) != g_check.flags) regDiff = 8;
    }
    if (!diffs && regDiff < 0) return true;
    g_differed++;
    if (g_logged < 20) {
        g_logged++;
        const Region &R = g_check.reg.empty() ? Region() : g_check.reg[where < 0 ? 0 : where];
        wclog(1, "Terminal Velocity check: %s (eax %08x esi %08x edi %08x ebx %08x): %d bytes differ, the first at %08x (game %02x twin %02x); "
                 "register %d %s (game eax %08x ecx %08x edx %08x flags %03x; twin %08x %08x %08x %03x)",
              kSpans[g_check.s].name, g_check.in[0], g_check.in[5], g_check.in[6], g_check.in[3], diffs,
              firstDiff >= 0 ? R.lo + (Bit32u)firstDiff : 0, firstDiff >= 0 ? MemBase[R.lo + firstDiff] : 0,
              firstDiff >= 0 ? g_check.after[where][firstDiff] : 0, regDiff, regDiff >= 0 ? "differs" : "the same",
              reg_eax, reg_ecx, reg_edx, (unsigned)(reg_flags & FMASK_TEST), g_check.out.eax, g_check.out.ecx, g_check.out.edx, g_check.flags);
    }
    return true;
}

}  // namespace wc
