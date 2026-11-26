/*
 *  Privateer's hot leaf routines made natively, and its frame wait idled (docs/privateer.md section 21).
 *
 *  A fight at Oxford (a Centurion, five hostiles more) took 1.2 million cycles a frame of the 1.05 million there are
 *  at the page's 30,000 a millisecond: a sixth drawing, over half the collision tests' and the AI's vector maths
 *  (seg039 over seg115, seg095, seg128).  The twins below are the leaves of that and the two biggest plain copies: at
 *  a routine's entry the twin works out what it would write and the registers it would leave (all of them, the high
 *  halves too), writes that, and returns to the caller as its retf does.  Flags are not made: no caller reads them.
 *  What a twin cannot do exactly goes to the game's own code before anything is written: code that is not the
 *  game's, memory not mapped, operands overlapping, the game's own error path, a loop that would not end.
 *
 *  A planet filling the window was half the emulated CPU again (section 29): the turned shape's runs (eight near
 *  routines of seg197) and the loop that unpacks a shape's row (22 copies, seg197 to seg204) are twins too.
 *
 *  The flight's loop waits for its next frame by spinning on the 600 Hz clock (seg075:062B), two thirds of the
 *  emulated CPU in quiet space: there the rest of the emulated slice is skipped, as a hlt's is.
 *
 *  WCPRIVTWINS: unset or 1 the twins; 0 the game's own code; "check" the game's own code, and at each return what
 *  it left compared with what the twin said (logged).  WCPRIVIDLE=0: the wait spins as the game's does.
 *
 *  JEMM's map of a handle's pages (its protected-mode routine, entered from the game's INT 67h AH=50h) is made
 *  natively too: WCPRIVJEMM unset or 1 the twin, 0 JEMM's own code, "check" JEMM's own code compared (section 35).
 */
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <map>
#include "wcnet_priv_native.h"
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "cpu.h"
#include "lazyflags.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "pic.h"

namespace wc {

namespace {

enum {
    kReach = 0x10FFF0,      // what a real-mode program reaches
    kMaxLoop = 64,          // a normalising loop's turns: more would not end
    kMaxWrites = 96,        // the words and dwords a small twin writes (its frame below the stack pointer too)
    kMaxBytes = 80000,      // the bytes a copy or a fill writes
    kMaxSpans = 4096,
    kMaxPending = 16,       // the check's calls open at once
};

enum { kSeg095 = 0x3B4B };     // (what 02DD's call of seg095:0247 pushes, less the load segment)

typedef Bit32u u32;
typedef Bit32s s32;
typedef Bit64u u64;
typedef Bit64s s64;

// The registers at a routine's entry, changed as the routine would; and what it would write.
struct Run {
    u32 a, b, c, d, si, di;
    Bit16u bpEntry;
    Bit16u sp, ds, es, ss;
    bool ok;
    bool clearDirection;
    int writes;
    PhysPt at[kMaxWrites];
    u32 val[kMaxWrites];
    Bit8u size[kMaxWrites];     // 1, 2 or 4
    Bit16u cs;                  // the routine's own code segment (what its calls push)
    int bytes;              // (a copy's or a fill's, in g_byteAt / g_byteVal)
};

PhysPt g_byteAt[kMaxBytes];
Bit8u g_byteVal[kMaxBytes];

inline PhysPt lin(Bit16u seg, Bit16u off) { return (PhysPt)seg * 16 + off; }

inline Bit8u get8(Run &r, PhysPt a) {
    Bit8u v = 0;
    if (a >= (PhysPt)kReach || mem_readb_checked(a, &v)) r.ok = false;
    return v;
}

inline Bit16u get16(Run &r, Bit16u seg, Bit16u off) {
    if (off == 0xFFFF) r.ok = false;
    const PhysPt a = lin(seg, off);
    return (Bit16u)(get8(r, a) | get8(r, a + 1) << 8);
}

inline u32 get32(Run &r, Bit16u seg, Bit16u off) {
    if (off > 0xFFFC) r.ok = false;
    return get16(r, seg, off) | (u32)get16(r, seg, (Bit16u)(off + 2)) << 16;
}

inline void put(Run &r, Bit16u seg, Bit16u off, u32 v, int size) {
    const PhysPt a = lin(seg, off);
    Bit8u probe;
    if ((u32)off + size > 0x10000 || a + size > (PhysPt)kReach || r.writes == kMaxWrites || mem_readb_checked(a, &probe)
        || mem_readb_checked(a + size - 1, &probe)) {
        r.ok = false;
        return;
    }
    r.at[r.writes] = a;
    r.val[r.writes] = v;
    r.size[r.writes++] = (Bit8u)size;
}

inline void put32(Run &r, Bit16u seg, Bit16u off, u32 v) { put(r, seg, off, v, 4); }

// The routine's own stack, below the stack pointer it was entered with (S: where its far return is): what its pushes,
// its locals and its callees' frames leave there, as the game's code would (later code reads some of it as it finds it).
inline void st16(Run &r, int below, u32 v) { put(r, r.ss, (Bit16u)(r.sp - below), v & 0xFFFF, 2); }
inline void st32(Run &r, int below, u32 v) { put(r, r.ss, (Bit16u)(r.sp - below), v, 4); }
inline void st8(Run &r, int below, u32 v) { put(r, r.ss, (Bit16u)(r.sp - below), v & 0xFF, 1); }

inline void put8(Run &r, PhysPt a, Bit8u v) {
    if (a >= (PhysPt)kReach || r.bytes == kMaxBytes) {
        r.ok = false;
        return;
    }
    g_byteAt[r.bytes] = a;
    g_byteVal[r.bytes++] = v;
}

inline void low16(u32 &reg, u32 v) { reg = (reg & 0xFFFF0000u) | (v & 0xFFFF); }
inline void low8(u32 &reg, u32 v) { reg = (reg & 0xFFFFFF00u) | (v & 0xFF); }
inline void high16(u32 &reg, u32 from) { reg = (from & 0xFFFF0000u) | (reg & 0xFFFF); }
inline u32 abs32(u32 v) { return (s32)v < 0 ? 0u - v : v; }
inline u32 sar(u32 v, u32 n) { return (u32)((s32)v >> (n & 31)); }
inline u32 shl(u32 v, u32 n) { return v << (n & 31); }

// An argument (i words past the far return).
inline Bit16u arg16(Run &r, int i) { return get16(r, r.ss, (Bit16u)(r.sp + 4 + 2 * i)); }
inline u32 arg32(Run &r, int i) { return get32(r, r.ss, (Bit16u)(r.sp + 4 + 2 * i)); }

inline bool overlap(Bit16u a, int an, Bit16u b, int bn) {
    return (Bit16u)(b - a) < an || (Bit16u)(a - b) < bn;
}

// ---- seg128: the maths ----------------------------------------------------------------------------------------------
// (Each twin's frame: "S-n" is n bytes below where its far return is; the game's own push order and locals.)

// seg128:02F6's body: the root of a 24.8, bit by bit (the argument in hand), entered k bytes below S with those bp,
// si and di (it pushes them).
void root_body(Run &r, u32 v, int k, Bit16u bp, Bit16u si16, Bit16u di16) {
    st16(r, k + 2, bp);
    st16(r, k + 4, si16);
    st16(r, k + 6, di16);
    u32 a = 0, di = r.di, si = v;
    if ((s32)v < 0) {
        r.a = 0;
        r.d <<= 16;
        high16(r.si, si);
        return;
    }
    u32 b = r.b << 8 | si >> 24;
    si <<= 8;
    u32 d = 0, cx = 0x14;
    for (;;) {
        for (int i = 0; i < 2; i++) {
            const u32 in = si >> 31, out = b >> 7 & 1;
            si <<= 1;
            low8(b, b << 1 | in);
            d = d << 1 | out;
        }
        a <<= 1;
        di = a << 1 | 1;
        if (di <= d) {
            d -= di;
            a++;
        }
        if (!--cx) break;
    }
    r.a = a;
    r.b = b;
    low16(r.c, 0);
    r.d = d << 16 | a >> 16;
    high16(r.si, si);
    high16(r.di, di);
}

void root(Run &r) {     // seg128:02F6 (24.8) -> dx:ax
    root_body(r, arg32(r, 0), 0, r.bpEntry, (Bit16u)r.si, (Bit16u)r.di);
}

void length(Run &r) {   // seg128:0296 (x, y, z) -> dx:ax, about the vector's length
    st16(r, 2, r.bpEntry);
    u32 a = abs32(arg32(r, 0)), b = abs32(arg32(r, 2)), c = abs32(arg32(r, 4)), t;
    if (a < b) { t = a; a = b; b = t; }
    if (a < c) { t = a; a = c; c = t; }
    if (b < c) { t = b; b = c; c = t; }
    c >>= 2;
    a += c;
    b >>= 2;
    a += b;
    b >>= 1;
    a += b;
    b >>= 2;
    a -= b;
    r.a = a;
    r.b = b;
    r.c = c;
    r.d = r.d << 16 | a >> 16;
}

// seg128:0390 (vector, matrix) and 0450 (the matrix's columns): the vector replaced.
void transform(Run &r, bool columns) {
    const Bit16u v = arg16(r, 0), m = arg16(r, 1);
    if (overlap(v, 12, m, 36)) { r.ok = false; return; }
    const u32 x = get32(r, r.ds, v), y = get32(r, r.ds, (Bit16u)(v + 4)), z = get32(r, r.ds, (Bit16u)(v + 8));
    st16(r, 2, r.bpEntry);
    st16(r, 16, r.si);
    st16(r, 18, r.di);
    st32(r, 6, x);
    st32(r, 10, y);
    st32(r, 14, z);
    u32 res = 0;
    for (int i = 0; i < 3; i++) {
        const int first = columns ? 4 * i : 12 * i, step = columns ? 12 : 4;
        const u64 two = (u64)((s64)(s32)x * (s32)get32(r, r.ds, (Bit16u)(m + first)))
                      + (u64)((s64)(s32)y * (s32)get32(r, r.ds, (Bit16u)(m + first + step)));
        const u64 all = two + (u64)((s64)(s32)z * (s32)get32(r, r.ds, (Bit16u)(m + first + 2 * step)));
        res = (u32)(all >> 8);
        put32(r, r.ds, (Bit16u)(v + 4 * i), res);
        r.b = (u32)two;
        r.c = (u32)(two >> 32);
        r.d = (u32)(all >> 32);
    }
    r.a = res;
    low16(r.a, v);
}

void transform_rows(Run &r) { transform(r, false); }
void transform_columns(Run &r) { transform(r, true); }

// ---- seg095: a 24.8's absolute value and its root ------------------------------------------------------------------

// seg095:02DC (result far, value near), entered k bytes below S with those bp, si and di; the result written through
// DS.
void absolute_at(Run &r, int k, Bit16u bp, Bit16u si16, Bit16u di16, Bit16u to, Bit16u toSeg, Bit16u from) {
    st16(r, k + 2, bp);
    st16(r, k + 12, si16);
    st16(r, k + 14, di16);
    const u32 v = get32(r, r.ds, from);
    const u32 out = abs32(v);
    if ((s32)v < 0) {
        r.es = toSeg;
        st32(r, k + 6, to | (u32)toSeg << 16);
        st32(r, k + 10, out);
    }
    put32(r, r.ds, to, out);
    r.a = out;
    low16(r.a, to);
    low16(r.b, to);
    low16(r.d, toSeg);
}

void absolute(Run &r) {
    absolute_at(r, 0, r.bpEntry, (Bit16u)r.si, (Bit16u)r.di, arg16(r, 0), arg16(r, 1), arg16(r, 2));
}

// seg095:0247 (result far, value near) at k below S, the value in hand (02DD's is in its own frame); the root's call
// pushes the code segment `cs95`.  Returns the root.
u32 root_of_at(Run &r, int k, Bit16u bp, Bit16u si16, Bit16u di16, Bit16u cs95, Bit16u to, Bit16u toSeg, u32 v,
               Bit16u from) {
    if ((s32)v < 0) { r.ok = false; return 0; }     // (the game's error 0x62F3)
    st16(r, k + 2, bp);
    st16(r, k + 8, si16);
    st32(r, k + 6, 0);
    st32(r, k + 12, v);
    st16(r, k + 14, cs95);
    st16(r, k + 16, 0x0274);
    root_body(r, v, k + 16, (Bit16u)(r.sp - k - 2), from, di16);
    const u32 out = (r.a & 0xFFFF) | r.d << 16;
    st32(r, k + 6, out);
    put32(r, r.ds, to, out);
    r.a = out;
    low16(r.a, to);
    low16(r.b, to);
    low16(r.d, toSeg);
    return out;
}

void root_of(Run &r) {
    const Bit16u from = arg16(r, 2);
    root_of_at(r, 0, r.bpEntry, (Bit16u)r.si, (Bit16u)r.di, r.cs, arg16(r, 0), arg16(r, 1), get32(r, r.ds, from), from);
}

// ---- seg115: vectors brought into range, and their length ----------------------------------------------------------

// seg115:0133 at k below S (bp, si pushed): the vector v (at `at` in DS) shifted left till one of its components
// reaches the limit; the shift in al.  v is the vector as it is in memory (the twin's own frame, for 02DD's call),
// and is left as the routine leaves it.
Bit8u scale_up_at(Run &r, int k, Bit16u bp, Bit16u si16, Bit16u at, u32 v[3], u32 limit, Bit8u step) {
    st16(r, k + 2, bp);
    st16(r, k + 18, si16);
    low8(r.d, step);
    u32 m[3];
    for (int i = 0; i < 3; i++) m[i] = abs32(v[i]);
    st32(r, k + 16, m[0]);
    st32(r, k + 12, m[1]);
    st32(r, k + 8, m[2]);
    if (!(m[0] | m[1] | m[2])) {
        r.a = 0;
        return 0;
    }
    st8(r, k + 3, 0);
    const u32 lim = limit << 8;
    Bit8u n = 0;
    int turns = 0;
    while ((s32)lim > (s32)m[0] && (s32)lim > (s32)m[1] && (s32)lim > (s32)m[2]) {
        if (++turns > kMaxLoop) { r.ok = false; return 0; }
        low8(r.c, step);
        for (int i = 0; i < 3; i++) m[i] = shl(m[i], step);
        n = (Bit8u)(n + step);
    }
    st32(r, k + 16, m[0]);
    st32(r, k + 12, m[1]);
    st32(r, k + 8, m[2]);
    st8(r, k + 3, n);
    r.a = lim;
    if (n) {
        low8(r.c, n);
        for (int i = 0; i < 3; i++) {
            v[i] = r.a = shl(v[i], n);
            put32(r, r.ds, (Bit16u)(at + 4 * i), r.a);
        }
    }
    low8(r.a, n);
    return n;
}

// seg115:0217 at k below S: shifted right till all are within it.
Bit8u scale_down_at(Run &r, int k, Bit16u bp, Bit16u si16, Bit16u at, u32 v[3], u32 limit, Bit8u step) {
    st16(r, k + 2, bp);
    st16(r, k + 18, si16);
    low8(r.d, step);
    u32 m[3];
    for (int i = 0; i < 3; i++) m[i] = abs32(v[i]);
    st32(r, k + 6, m[0]);
    st32(r, k + 10, m[1]);
    st32(r, k + 14, m[2]);
    st8(r, k + 15, 0);
    const u32 lim = limit << 8;
    Bit8u n = 0;
    int turns = 0;
    while ((s32)lim < (s32)m[0] || (s32)lim < (s32)m[1] || (s32)lim < (s32)m[2]) {
        if (++turns > kMaxLoop) { r.ok = false; return 0; }
        low8(r.c, step);
        for (int i = 0; i < 3; i++) m[i] = sar(m[i], step);
        n = (Bit8u)(n + step);
    }
    st32(r, k + 6, m[0]);
    st32(r, k + 10, m[1]);
    st32(r, k + 14, m[2]);
    st8(r, k + 15, n);
    r.a = lim;
    if (n) {
        low8(r.c, n);
        for (int i = 0; i < 3; i++) {
            v[i] = r.a = sar(v[i], n);
            put32(r, r.ds, (Bit16u)(at + 4 * i), r.a);
        }
    }
    low8(r.a, n);
    return n;
}

void read_vector(Run &r, Bit16u at, u32 v[3]) {
    for (int i = 0; i < 3; i++) v[i] = get32(r, r.ds, (Bit16u)(at + 4 * i));
}

void normalise_up(Run &r) {     // seg115:0133 (vector near, limit, step) -> al, the shift
    u32 v[3];
    read_vector(r, arg16(r, 0), v);
    scale_up_at(r, 0, r.bpEntry, (Bit16u)r.si, arg16(r, 0), v, arg32(r, 1), (Bit8u)arg16(r, 3));
}

void normalise_down(Run &r) {   // seg115:0217
    u32 v[3];
    read_vector(r, arg16(r, 0), v);
    scale_down_at(r, 0, r.bpEntry, (Bit16u)r.si, arg16(r, 0), v, arg32(r, 1), (Bit8u)arg16(r, 3));
}

void shift_within(Run &r) {     // seg115:0962 (vector near, limit): shifted right by threes
    if (r.ds != r.ss) { r.ok = false; return; }     // (its locals are reached through DS)
    const Bit16u at = arg16(r, 0);
    const u32 limit = arg32(r, 1);
    const Bit16u bp = (Bit16u)(r.sp - 2), si16 = (Bit16u)r.si, di16 = (Bit16u)r.di;
    // (its vector must not be its own frame)
    if (overlap(at, 12, (Bit16u)(r.sp - 0x48), 0x48)) { r.ok = false; return; }
    u32 v[3], m[3];
    read_vector(r, at, v);
    st16(r, 2, r.bpEntry);
    st16(r, 40, si16);
    st16(r, 42, di16);
    static const Bit16u kRet[3] = { 0x0978, 0x0997, 0x09B6 };
    for (int i = 0; i < 3; i++) {
        const Bit16u local = (Bit16u)(r.sp - 10 - 8 * i);       // bp-8, bp-0x10, bp-0x18
        st16(r, 44, at + 4 * i);
        st16(r, 46, r.ss);
        st16(r, 48, local);
        st16(r, 50, r.cs);
        st16(r, 52, kRet[i]);
        absolute_at(r, 52, bp, at, di16, local, r.ss, (Bit16u)(at + 4 * i));
        m[i] = sar(abs32(v[i]), 8);
        st32(r, 6 + 8 * i, m[i]);
    }
    Bit16u n = 0;
    int turns = 0;
    while ((s32)m[0] > (s32)limit || (s32)m[1] > (s32)limit || (s32)m[2] > (s32)limit) {
        if (++turns > kMaxLoop) { r.ok = false; return; }
        for (int i = 0; i < 3; i++) m[i] = sar(m[i], 3);
        n = (Bit16u)(n + 3);
    }
    for (int i = 0; i < 3; i++) st32(r, 6 + 8 * i, m[i]);
    r.a = m[2];
    if (n) {
        low8(r.c, n);
        for (int i = 0; i < 3; i++) {
            st32(r, 30 + 4 * i, (u32)(s32)(Bit16s)n);
            r.a = sar(v[i], n);
            put32(r, r.ds, (Bit16u)(at + 4 * i), r.a);
        }
    }
    low16(r.a, at);
}

void distance(Run &r) {         // seg115:02DD (result far, vector near): the length by the root
    if (r.ds != r.ss) { r.ok = false; return; }
    const Bit16u to = arg16(r, 0), toSeg = arg16(r, 1), from = arg16(r, 2);
    if (overlap(from, 12, (Bit16u)(r.sp - 0x60), 0x60)) { r.ok = false; return; }
    const Bit16u bp = (Bit16u)(r.sp - 2), vec = (Bit16u)(r.sp - 20), result = (Bit16u)(r.sp - 8);
    u32 v[3];
    read_vector(r, from, v);
    for (int i = 0; i < 3; i++) v[i] = abs32(v[i]);
    st16(r, 2, r.bpEntry);
    st16(r, 22, r.si);
    st8(r, 4, 0);
    st8(r, 3, 0);
    for (int i = 0; i < 3; i++) st32(r, 20 - 4 * i, v[i]);
    st16(r, 24, 1);
    st32(r, 28, 0x10);
    st16(r, 30, vec);
    st16(r, 32, r.cs);
    st16(r, 34, 0x032E);
    const Bit8u up = scale_up_at(r, 34, bp, from, vec, v, 0x10, 1);
    st8(r, 3, up);
    Bit8u down = 0;
    if (!up) {
        st16(r, 24, 1);
        st32(r, 28, 0x400);
        st16(r, 30, vec);
        st16(r, 32, r.cs);
        st16(r, 34, 0x034A);
        down = scale_down_at(r, 34, bp, from, vec, v, 0x400, 1);
        st8(r, 4, down);
    }
    u32 sq[3];
    for (int i = 0; i < 3; i++) {
        const s64 x = (s64)(s32)v[i] * (s32)v[i];
        sq[i] = (u32)((u64)x >> 8);
        st32(r, 20 - 4 * i, sq[i]);
        r.d = (u32)((u64)x >> 32);
    }
    u32 sum = sq[0] + sq[1];
    st32(r, 20, sum);
    sum += sq[2];
    st32(r, 20, sum);
    r.a = sq[2];
    if (!r.ok) return;
    st16(r, 24, vec);
    st16(r, 26, r.ss);
    st16(r, 28, result);
    st16(r, 30, r.cs);
    st16(r, 32, 0x03AA);
    const u32 root = root_of_at(r, 32, bp, from, (Bit16u)r.di, (Bit16u)(priv_load_seg() + kSeg095), result, r.ss, sum, vec);
    if (!r.ok) return;
    u32 out = root;
    if (up) {
        low8(r.c, up);
        out = sar(out, up);
        st32(r, 8, out);
    } else if (down) {
        low8(r.c, down);
        out = shl(out, down);
        st32(r, 8, out);
    }
    if ((s32)out < 0) {
        out = get32(r, r.ds, 0x33E6);
        st32(r, 8, out);
    }
    put32(r, r.ds, to, out);
    r.a = out;
    low16(r.a, to);
    low16(r.b, to);
    low16(r.d, toSeg);
}

// ---- the two plain copies -------------------------------------------------------------------------------------------

void clear(Run &r) {            // seg181:000A (bitmap, colour): the clip filled
    const Bit16u rec = arg16(r, 0);
    Bit16u colour = arg16(r, 1);
    colour = (Bit16u)((colour & 0xFF) * 0x101);
    const Bit16u seg = get16(r, r.ds, rec), x0 = get16(r, r.ds, (Bit16u)(rec + 4));
    const Bit16u w = (Bit16u)(get16(r, r.ds, (Bit16u)(rec + 8)) - x0 + 1);
    const Bit16u y0 = get16(r, r.ds, (Bit16u)(rec + 6));
    const Bit16u h = (Bit16u)(get16(r, r.ds, (Bit16u)(rec + 10)) - y0 + 1);
    if (!w || !h || w > 1024 || h > 1024 || (u32)w * h > kMaxBytes) { r.ok = false; return; }
    st16(r, 2, r.bpEntry);
    st16(r, 6, r.si);
    st16(r, 8, r.di);
    st16(r, 4, x0);
    Bit16u row = (Bit16u)(get16(r, r.ds, (Bit16u)(rec + 2)) + 2 * y0);
    for (Bit16u y = 0; y < h && r.ok; y++, row = (Bit16u)(row + 2)) {
        const Bit16u start = (Bit16u)(get16(r, r.ss, row) + x0);
        if ((u32)start + w > 0x10000) { r.ok = false; return; }
        const PhysPt to = lin(seg, start);
        for (Bit16u x = 0; x < w; x++) put8(r, to + x, (Bit8u)colour);
    }
    r.es = seg;
    r.clearDirection = true;
    low16(r.a, colour);
    low16(r.b, row);
    low16(r.c, 0);
    low16(r.d, w);
}

void copy_spans(Run &r) {       // seg205:0002 (from, to, spans far): the cockpit's window shown
    if (GETFLAG(DF)) { r.ok = false; return; }
    const Bit16u from = arg16(r, 0), to = arg16(r, 1), listOff = arg16(r, 2), listSeg = arg16(r, 3);
    const Bit16u fromSeg = get16(r, r.ds, from), fromRows = get16(r, r.ds, (Bit16u)(from + 2));
    const Bit16u toSeg = get16(r, r.ds, to), toRows = get16(r, r.ds, (Bit16u)(to + 2));
    if (toSeg == fromSeg) { r.ok = false; return; }     // (a bitmap onto itself: each byte as the game's movsw has it)
    Bit16u p = (Bit16u)(listOff + 12);
    st16(r, 2, r.bpEntry);
    st16(r, 10, r.ds);
    st16(r, 12, r.es);
    st16(r, 14, SegValue(fs));
    st16(r, 16, r.si);
    st16(r, 18, r.di);
    st16(r, 8, r.ds);
    st16(r, 6, toRows);
    st16(r, 4, fromRows);
    st16(r, 20, p);
    low16(r.b, from);
    for (int spans = 0; r.ok; spans++) {
        if (spans == kMaxSpans) { r.ok = false; return; }
        const Bit16u x = get16(r, listSeg, p);
        if (x == 0xFFFF) break;
        const Bit16u bx = (Bit16u)(get16(r, listSeg, (Bit16u)(p + 2)) << 1), n = get16(r, listSeg, (Bit16u)(p + 4));
        p = (Bit16u)(p + 6);
        const Bit16u d = (Bit16u)(get16(r, r.ds, (Bit16u)(bx + toRows)) + x);
        const Bit16u s = (Bit16u)(get16(r, r.ds, (Bit16u)(bx + fromRows)) + x);
        if (n > 1024 || (u32)d + n > 0x10000 || (u32)s + n > 0x10000) { r.ok = false; return; }
        const PhysPt dst = lin(toSeg, d), src = lin(fromSeg, s);
        for (Bit16u i = 0; i < n; i++) put8(r, dst + i, get8(r, src + i));
        low16(r.b, bx);
        low16(r.c, 0);
        low16(r.d, fromSeg);
    }
    st16(r, 20, p);                 // (its push of the list's place, as the last span left it)
    low16(r.a, 0xFFFF);
}

// ---- seg197: a turned shape's runs ------------------------------------------------------------------------------------
// A planet in the cockpit's window is half the emulated CPU of its frame (measured at Oxford, 2026-11-23: 53 in 100 of
// the instructions in seg197, 35 of them in these loops).  seg202's turned and scaled shape is drawn a source row's
// run at a time by one of sixteen near routines (DGROUP:4222, by the turn's octant): ah pixels from ds:si go to es:bx
// along a line stepped by two 8-bit fractions (dl by cl: a pixel along the row, dh by ch: a row, DGROUP:47F5 bytes).
// The eight below are the ones without a clip's count (the other eight jump to these when the run cannot leave the
// clip, DGROUP:47DA).  The first kind skips a source pixel that steps nowhere; the second kind reads every one and
// fills the corner of a diagonal step with the pixel before.  Near routines: nothing is left on the stack.
void shape_run(Run &r, int xStep, bool rowsDown, bool fills, bool rowFirst) {
    if (GETFLAG(DF)) { r.ok = false; return; }
    const Bit16u stride = get16(r, r.ss, 0x47F5);
    const Bit16u yStep = rowsDown ? stride : (Bit16u)(0 - stride);
    const Bit8u cl = (Bit8u)r.c, ch = (Bit8u)(r.c >> 8);
    Bit8u al = (Bit8u)r.a, ah = (Bit8u)(r.a >> 8), dl = (Bit8u)r.d, dh = (Bit8u)(r.d >> 8);
    Bit16u bx = (Bit16u)r.b, si = (Bit16u)r.si;
    const PhysPt src = lin(r.ds, 0), dst = lin(r.es, 0);
    PhysPt readLo = 0xFFFFFFFFu, readHi = 0, wroteLo = 0xFFFFFFFFu, wroteHi = 0;
#define RUN_READ() { const PhysPt at_ = src + si; if (at_ < readLo) readLo = at_; if (at_ > readHi) readHi = at_; al = get8(r, at_); si++; }
#define RUN_WRITE() { const PhysPt at_ = dst + bx; if (at_ < wroteLo) wroteLo = at_; if (at_ > wroteHi) wroteHi = at_; put8(r, at_, al); }
    bool draw = true;
    for (;;) {
        if (draw) {
            RUN_READ();
            RUN_WRITE();
            if (!--ah) break;
        }
        draw = true;
        unsigned t = (unsigned)dl + cl;
        dl = (Bit8u)t;
        if (t > 0xFF) {
            t = (unsigned)dh + ch;
            dh = (Bit8u)t;
            if (!fills) {
                bx = (Bit16u)(bx + xStep);
                if (t > 0xFF) bx = (Bit16u)(bx + yStep);
            } else if (rowFirst) {
                if (t > 0xFF) {
                    bx = (Bit16u)(bx + yStep);
                    RUN_WRITE();
                }
                bx = (Bit16u)(bx + xStep);
            } else {
                bx = (Bit16u)(bx + xStep);
                if (t > 0xFF) {
                    RUN_WRITE();
                    bx = (Bit16u)(bx + yStep);
                }
            }
            continue;
        }
        t = (unsigned)dh + ch;
        dh = (Bit8u)t;
        if (t > 0xFF) {
            bx = (Bit16u)(bx + yStep);
            continue;
        }
        if (fills) RUN_READ() else si++;
        if (!--ah) break;
        draw = false;
    }
#undef RUN_READ
#undef RUN_WRITE
    if (wroteLo <= readHi && readLo <= wroteHi) r.ok = false;   // (a run drawn over its own source: as the game's code has it)
    low16(r.a, al);             // (ah counted down to 0)
    low16(r.b, bx);
    low16(r.d, (u32)dh << 8 | dl);
    low16(r.si, si);
    low16(r.di, stride);
}

void run_a_pp(Run &r) { shape_run(r, 1, true, false, false); }      // seg197:0C15
void run_b_pp(Run &r) { shape_run(r, 1, true, true, false); }       // seg197:0C75
void run_a_pm(Run &r) { shape_run(r, 1, false, false, false); }     // seg197:0D0D
void run_b_pm(Run &r) { shape_run(r, 1, false, true, true); }       // seg197:0D6D
void run_a_mp(Run &r) { shape_run(r, -1, true, false, false); }     // seg197:0E1B
void run_b_mp(Run &r) { shape_run(r, -1, true, true, false); }      // seg197:0E7B
void run_a_mm(Run &r) { shape_run(r, -1, false, false, false); }    // seg197:0F0E
void run_b_mm(Run &r) { shape_run(r, -1, false, true, true); }      // seg197:0F6E

// ---- a shape's row unpacked -------------------------------------------------------------------------------------------
// Every scaled or turned shape's packed row is first unpacked into DGROUP:47F8 by the same 39 bytes of loop, 22 copies
// of it in seg197 to seg204: bx pixels from ds:si to es:di, a byte of count each time -- odd: (count >> 1) of the next
// byte; even: as many bytes as they are.  The twin is entered at the loop's first instruction and leaves at the one
// after it (no stack, no return), with the registers as the loop leaves them.
void row_decode(Run &r) {
    if (GETFLAG(DF)) { r.ok = false; return; }
    Bit16u bx = (Bit16u)r.b, si = (Bit16u)r.si, di = (Bit16u)r.di, ax = (Bit16u)r.a;
    const PhysPt src = lin(r.ds, 0), dst = lin(r.es, 0);
    const Bit16u si0 = si, di0 = di;
    for (int turns = 0;; turns++) {
        if (turns == 2048 || si > 0xFF00 || di > 0xFF00) { r.ok = false; return; }
        const Bit8u head = get8(r, src + si++);
        const Bit16u n = head >> 1;
        if (head & 1) {
            const Bit8u c = get8(r, src + si++);
            for (Bit16u i = 0; i < n; i++) put8(r, dst + di++, c);
            ax = (Bit16u)(c * 0x101);
            bx = (Bit16u)(bx - n);
        } else {
            for (Bit16u i = 0; i < n; i++) put8(r, dst + di++, get8(r, src + si++));
            ax = n;
            bx = (Bit16u)(bx - n);
        }
        if (!r.ok) return;
        if (!bx) break;
    }
    const PhysPt readLo = src + si0, readHi = src + si, wroteLo = dst + di0, wroteHi = dst + di;
    if (wroteLo < readHi && readLo < wroteHi) { r.ok = false; return; }    // (unpacked over itself: the game's own)
    low16(r.a, ax);
    low16(r.b, 0);
    low16(r.c, 0);
    low16(r.si, si);
    low16(r.di, di);
}

// ---- the hooks ------------------------------------------------------------------------------------------------------

struct Twin {
    Bit16u para, ip, len;       // the routine's code from its entry
    u32 sum;                    // ... its FNV-1a, as it is in the file
    Bit16u moved[4];            // the words DOS relocates in it (0 none): the load segment taken off to compare
    void (*make)(Run &);
    const char *name;
    int pop;                    // what its return takes off the stack: 4 a far routine, 2 a near one; 0: a loop inside
                                // a routine, left at the instruction after its len bytes
    int state;                  // 0 not seen yet, 1 the game's code as we know it, -1 not
    unsigned made, left, checked, differ;
};

Twin g_twins[] = {
    { 0x4C5B, 0x0296, 0x060, 0xCA8867CF, { 0x0000, 0x0000, 0x0000, 0x0000 }, length,            "seg128:0296", 4, 0, 0, 0, 0, 0 },
    { 0x4C5B, 0x02F6, 0x056, 0xE5DFD2C5, { 0x0000, 0x0000, 0x0000, 0x0000 }, root,              "seg128:02F6", 4, 0, 0, 0, 0, 0 },
    { 0x4C5B, 0x0390, 0x0C0, 0xEE187329, { 0x0000, 0x0000, 0x0000, 0x0000 }, transform_rows,    "seg128:0390", 4, 0, 0, 0, 0, 0 },
    { 0x4C5B, 0x0450, 0x0C0, 0xFD1772F9, { 0x0000, 0x0000, 0x0000, 0x0000 }, transform_columns, "seg128:0450", 4, 0, 0, 0, 0, 0 },
    { 0x3B4B, 0x02DC, 0x04F, 0x00FF0769, { 0x0000, 0x0000, 0x0000, 0x0000 }, absolute,          "seg095:02DC", 4, 0, 0, 0, 0, 0 },
    { 0x3B4B, 0x0247, 0x053, 0x04763F20, { 0x0265, 0x0272, 0x0000, 0x0000 }, root_of,           "seg095:0247", 4, 0, 0, 0, 0, 0 },
    { 0x4563, 0x0133, 0x0E1, 0x7F20D540, { 0x0000, 0x0000, 0x0000, 0x0000 }, normalise_up,      "seg115:0133", 4, 0, 0, 0, 0, 0 },
    { 0x4563, 0x0217, 0x0C3, 0x974F18A6, { 0x0000, 0x0000, 0x0000, 0x0000 }, normalise_down,    "seg115:0217", 4, 0, 0, 0, 0, 0 },
    { 0x4563, 0x0962, 0x0F6, 0xE94D348F, { 0x0976, 0x0995, 0x09B4, 0x0000 }, shift_within,      "seg115:0962", 4, 0, 0, 0, 0, 0 },
    { 0x4563, 0x02DD, 0x11C, 0xEEA1D99E, { 0x03A8, 0x0000, 0x0000, 0x0000 }, distance,          "seg115:02DD", 4, 0, 0, 0, 0, 0 },
    { 0x54D1, 0x000A, 0x04D, 0x28E4BB71, { 0x0000, 0x0000, 0x0000, 0x0000 }, clear,             "seg181:000A", 4, 0, 0, 0, 0, 0 },
    { 0x5D17, 0x0002, 0x06B, 0x0B790087, { 0x0000, 0x0000, 0x0000, 0x0000 }, copy_spans,        "seg205:0002", 4, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0C15, 0x025, 0x73994C45, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_a_pp,          "seg197:0C15", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0C75, 0x028, 0xC6F9AEF2, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_b_pp,          "seg197:0C75", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0D0D, 0x025, 0x2D746B8D, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_a_pm,          "seg197:0D0D", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0D6D, 0x033, 0x7BA84D77, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_b_pm,          "seg197:0D6D", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0E1B, 0x025, 0xA922C09D, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_a_mp,          "seg197:0E1B", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0E7B, 0x028, 0x773E863A, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_b_mp,          "seg197:0E7B", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0F0E, 0x025, 0x4CFBCC55, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_a_mm,          "seg197:0F0E", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0F6E, 0x033, 0x79648DC6, { 0x0000, 0x0000, 0x0000, 0x0000 }, run_b_mm,          "seg197:0F6E", 2, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0226, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:0226", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0291, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:0291", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x04B7, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:04B7", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0522, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:0522", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0744, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:0744", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x07AF, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:07AF", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x09D4, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:09D4", 0, 0, 0, 0, 0, 0 },
    { 0x5680, 0x0A3F, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg197:0A3F", 0, 0, 0, 0, 0, 0 },
    { 0x579E, 0x011B, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg198:011B", 0, 0, 0, 0, 0, 0 },
    { 0x579E, 0x027B, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg198:027B", 0, 0, 0, 0, 0, 0 },
    { 0x579E, 0x1A42, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg198:1A42", 0, 0, 0, 0, 0, 0 },
    { 0x579E, 0x1B58, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg198:1B58", 0, 0, 0, 0, 0, 0 },
    { 0x5A57, 0x03F3, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg199:03F3", 0, 0, 0, 0, 0, 0 },
    { 0x5A57, 0x0505, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg199:0505", 0, 0, 0, 0, 0, 0 },
    { 0x5AB6, 0x04C7, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg200:04C7", 0, 0, 0, 0, 0, 0 },
    { 0x5AB6, 0x066F, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg200:066F", 0, 0, 0, 0, 0, 0 },
    { 0x5C3D, 0x0238, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg203:0238", 0, 0, 0, 0, 0, 0 },
    { 0x5C3D, 0x03D8, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg203:03D8", 0, 0, 0, 0, 0, 0 },
    { 0x5C9D, 0x0306, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg204:0306", 0, 0, 0, 0, 0, 0 },
    { 0x5C9D, 0x03BC, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg204:03BC", 0, 0, 0, 0, 0, 0 },
    { 0x5C9D, 0x0474, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg204:0474", 0, 0, 0, 0, 0, 0 },
    { 0x5C9D, 0x0615, 0x027, 0xEC304514, { 0x0000, 0x0000, 0x0000, 0x0000 }, row_decode,        "seg204:0615", 0, 0, 0, 0, 0, 0 },
};
enum { kTwinCount = sizeof(g_twins) / sizeof(g_twins[0]) };

enum { kSeg075 = 0x3127, kFrameWait = 0x062B };
const Bit8u kWaitCode[] = { 0x66, 0x8B, 0x46, 0xFC, 0x66, 0x83, 0xC0, 0x14, 0x66, 0x3B, 0x06, 0x36, 0x36, 0x73, 0xF1 };

enum Mode { MODE_GAME, MODE_TWINS, MODE_CHECK };

Mode mode() {
    static int m = -1;
    if (m < 0) {
        const char *env = getenv("WCPRIVTWINS");
        m = !env || !env[0] ? MODE_TWINS : !strcmp(env, "check") ? MODE_CHECK : env[0] == '0' ? MODE_GAME : MODE_TWINS;
        if (priv_classic()) m = MODE_GAME;
    }
    return (Mode)m;
}

bool idle_wanted() {
    static int on = -1;
    if (on < 0) {
        const char *env = getenv("WCPRIVIDLE");
        on = (!env || env[0] != '0') && !priv_classic() ? 1 : 0;
    }
    return on != 0;
}

bool g_only[0x10000];           // an IP nothing else watched
bool g_return[0x10000];         // (the check: an IP a call returns to)

struct Pending {
    Twin *twin;
    Run want;
    Bit16u retIp, retCs, sp;
    std::vector<PhysPt> at;
    std::vector<Bit8u> val;
};
Pending g_pending[kMaxPending];
int g_open = 0;

void run_of(Run &r) {
    r.a = reg_eax;
    r.b = reg_ebx;
    r.c = reg_ecx;
    r.d = reg_edx;
    r.si = reg_esi;
    r.di = reg_edi;
    r.sp = reg_sp;
    r.ds = (Bit16u)SegValue(ds);
    r.es = (Bit16u)SegValue(es);
    r.ss = (Bit16u)SegValue(ss);
    r.cs = (Bit16u)SegValue(cs);
    r.bpEntry = reg_bp;
    r.ok = true;
    r.clearDirection = false;
    r.writes = r.bytes = 0;
}

bool code_known(Twin &t) {
    if (t.state) return t.state > 0;
    u32 h = 2166136261u;
    const PhysPt at = SegPhys(cs) + t.ip;
    std::vector<Bit8u> code(t.len);
    for (int i = 0; i < t.len; i++) {
        if (mem_readb_checked(at + i, &code[i])) return false;
    }
    for (int k = 0; k < 4; k++) {
        if (!t.moved[k]) continue;
        const int i = t.moved[k] - t.ip;
        if (i < 0 || i + 1 >= t.len) return false;
        const Bit16u word = (Bit16u)((code[i] | code[i + 1] << 8) - priv_load_seg());
        code[i] = (Bit8u)word;
        code[i + 1] = (Bit8u)(word >> 8);
    }
    for (int i = 0; i < t.len; i++) h = (h ^ code[i]) * 16777619u;
    t.state = h == t.sum ? 1 : -1;
    if (t.state < 0) wclog(1, "Privateer: %s is not the code we know (%08x): the game's own", t.name, h);
    return t.state > 0;
}

void apply(const Run &r, Bit16u retIp, Bit16u retCs, int pop) {
    for (int i = 0; i < r.writes; i++) {
        if (r.size[i] == 4) mem_writed_checked(r.at[i], r.val[i]);
        else if (r.size[i] == 2) mem_writew_checked(r.at[i], (Bit16u)r.val[i]);
        else mem_writeb_checked(r.at[i], (Bit8u)r.val[i]);
    }
    // A copy's or a fill's bytes (some 117 KB a frame in a fight: the view cleared, the cockpit's window shown), a
    // byte at a time, were a tenth of the browser's busy time.  A run of consecutive places within one 4 KB page has
    // its first byte written as before (the TLB filled as that write fills it); where the page is then memory the TLB
    // writes directly, the rest of the run is copied there at once -- what each byte's write would have done; a page
    // with a handler (the VGA's) still gets every byte through it.  WCPRIVTWINS_VERIFY=1 (a test): each run read back.
    static int verify = -1;
    if (verify < 0) {
        const char *env = getenv("WCPRIVTWINS_VERIFY");
        verify = env && env[0] == '1';
    }
    for (int i = 0; i < r.bytes;) {
        const PhysPt a = g_byteAt[i];
        int n = 1;
        while (i + n < r.bytes && g_byteAt[i + n] == a + (PhysPt)n && ((a + n) & 0xFFF)) n++;
        mem_writeb_checked(a, g_byteVal[i]);
        const HostPt w = n > 1 ? get_tlb_write(a + 1) : 0;
        if (w) memcpy(w + a + 1, &g_byteVal[i + 1], n - 1);
        else for (int k = 1; k < n; k++) mem_writeb_checked(a + k, g_byteVal[i + k]);
        if (verify) {
            static unsigned long long readBack, differ;
            for (int k = 0; k < n; k++) {
                Bit8u b = 0;
                if ((mem_readb_checked(a + k, &b) || b != g_byteVal[i + k]) && ++differ <= 5)
                    wclog(0, "Privateer twins: a run's byte at %08X reads %02X, written %02X", a + k, b, g_byteVal[i + k]);
            }
            if ((readBack += n) % 20000000ull < (unsigned long long)n)
                wclog(1, "Privateer twins: %llu bytes of runs read back, %llu differ", readBack, differ);
        }
        i += n;
    }
    if (r.clearDirection) {
        SETFLAGBIT(DF, false);
        cpu.direction = 1;
    }
    reg_eax = r.a;
    reg_ebx = r.b;
    reg_ecx = r.c;
    reg_edx = r.d;
    reg_esi = r.si;
    reg_edi = r.di;
    if (r.es != (Bit16u)SegValue(es)) SegSet16(es, r.es);
    reg_sp = (Bit16u)(r.sp + pop);
    if (pop == 4) SegSet16(cs, retCs);
    reg_eip = retIp;
}

void report() {
    for (int i = 0; i < kTwinCount; i++) {
        const Twin &t = g_twins[i];
        if (mode() == MODE_CHECK) {
            wclog(1, "Privateer twins check: %s %u calls compared, %u differ, %u left to the game", t.name, t.checked, t.differ, t.left);
        } else {
            wclog(2, "Privateer twins: %s %u made, %u left to the game", t.name, t.made, t.left);
        }
    }
}

void compare_returned() {
    while (g_open) {
        Pending &p = g_pending[g_open - 1];
        const int pop = p.twin->pop;
        if (reg_sp < (Bit16u)(p.sp + pop)) return;      // (still inside it)
        g_open--;
        if (reg_ip != p.retIp || SegValue(cs) != p.retCs || reg_sp != (Bit16u)(p.sp + pop)) continue;
        Twin &t = *p.twin;
        const Run &w = p.want;
        bool same = reg_eax == w.a && reg_ebx == w.b && reg_ecx == w.c && reg_edx == w.d && reg_esi == w.si
                 && reg_edi == w.di && SegValue(es) == w.es;
        const PhysPt stackLo = lin(w.ss, (Bit16u)(p.sp - 0x100)), stackHi = lin(w.ss, p.sp);
        for (int i = 0; same && i < w.writes; i++) {
            if (w.at[i] >= stackLo && w.at[i] < stackHi) continue;
            u32 v = 0;
            for (int k = 0; k < w.size[i]; k++) {
                Bit8u b = 0;
                mem_readb_checked(w.at[i] + k, &b);
                v |= (u32)b << (8 * k);
            }
            same = v == w.val[i];
        }
        for (size_t i = 0; same && i < p.at.size(); i++) {
            Bit8u b = 0;
            mem_readb_checked(p.at[i], &b);
            same = b == p.val[i];
            for (size_t k = i + 1; !same && k < p.at.size(); k++) same = p.at[k] == p.at[i];   // (written again after)
        }
        t.checked++;
        if (!same && ++t.differ <= 5) {
            wclog(0, "Privateer twins check: %s differs: the game eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x, the twin %08x %08x %08x %08x %08x %08x",
                  t.name, reg_eax, reg_ebx, reg_ecx, reg_edx, reg_esi, reg_edi, w.a, w.b, w.c, w.d, w.si, w.di);
        }
        static unsigned n;
        if (++n % 200000 == 0) report();
    }
}

void entered(Twin &t) {
    if (!code_known(t)) return;
    Run r;
    run_of(r);
    const Bit16u retIp = t.pop ? get16(r, r.ss, r.sp) : (Bit16u)(t.ip + t.len);
    const Bit16u retCs = t.pop == 4 ? get16(r, r.ss, (Bit16u)(r.sp + 2)) : r.cs;
    if (r.ok) t.make(r);
    if (!r.ok) {
        t.left++;
        return;
    }
    if (mode() == MODE_CHECK) {
        if (g_open == kMaxPending) return;
        Pending &p = g_pending[g_open++];
        p.twin = &t;
        p.want = r;
        p.retIp = retIp;
        p.retCs = retCs;
        p.sp = r.sp;
        p.at.assign(g_byteAt, g_byteAt + r.bytes);
        p.val.assign(g_byteVal, g_byteVal + r.bytes);
        g_return[retIp] = true;
        wc_net_watch[retIp] = true;
        return;
    }
    apply(r, retIp, retCs, t.pop);
    t.made++;
    static unsigned n;
    if (++n % 1000000 == 0) report();
}

// The wait: while its clock has not passed 20 ticks, the slice's remaining cycles are skipped, as CPU_HLT does.
void frame_wait() {
    Run r;
    run_of(r);
    const PhysPt code = SegPhys(cs) + kFrameWait;
    for (size_t i = 0; i < sizeof(kWaitCode); i++) {
        if (get8(r, code + i) != kWaitCode[i]) return;
    }
    const u32 since = get32(r, r.ss, (Bit16u)(reg_bp - 4)), now = get32(r, r.ds, 0x3636);
    if (r.ok && since + 0x14 >= now && CPU_Cycles > 0) {
        CPU_IODelayRemoved += CPU_Cycles;
        CPU_Cycles = 0;
    }
}

// JEMM's map of a handle's pages (docs/privateer.md section 35).  PRIV.EXE always loads JEMM.OVL, Origin's EMS manager,
// and the game maps EMS through it: seg145:05B3 sends all 24 pages of its large frame (INT 67h AX=5000h CX=18h) each
// time one of its six window slots changes, 70 calls a frame in a fight and thousands as a new type is read in.  JEMM's
// routine (protected mode, selector 18h, 16-bit code; live 0018:5C4C = JEMM.OVL 394Dh) takes the pairs one by one: a
// pair's logical and physical page from the caller's array (FS flat), the handle's four page-table entries for that
// logical page (its record at EBX: +2 the pages, +0Ch the entries) copied to the physical page's place in the page
// table (DS:0DAh, the frame's offset DS:0EEh or 280h for the large frame; an unmapped page gets 000A0007h four
// times), then CR3 reloaded once: some 45 instructions a pair, 98% of JEMM's time in a fight.
//
// The twin is entered at the loop's top (5C83: dec word [bp+0Ah]) and does the pairs in JEMM's order, every read
// and write as JEMM's own (the page table's writes change what later reads find, as JEMM's do), leaving at 5D44 (mov
// eax,cr3: JEMM's own code reloads CR3 and returns) with JEMM's registers and the flags of its last dec.  Before a
// pair is begun every place it reads or writes is tried: a place that would fault, or a pair for JEMM's error path (a
// physical page out of range, a logical page past the handle's), goes back to JEMM's own code at the loop's top with
// nothing of that pair done.  WCPRIVJEMM: unset or 1 the twin, 0 JEMM's own code, "check" JEMM's own code with the
// twin's result worked out at the top (its writes kept aside) and compared at 5D44: every byte written, EAX ECX ESI
// EDI and the six condition flags (logged).
enum { kJemmSel = 0x18, kJemmFrom = 0x5C4C, kJemmLen = 0x112, kJemmTop = 0x5C83, kJemmExit = 0x5D44 };
const u32 kJemmSum = 0x138440BF;        // FNV-1a of 0018:5C4C..5D5D, as in JEMM.OVL at 394Dh
enum JemmMode { JEMM_OWN, JEMM_TWIN, JEMM_CHECK };

JemmMode jemm_mode() {
    static int m = -1;
    if (m < 0) {
        const char *env = getenv("WCPRIVJEMM");
        m = !env || !env[0] ? JEMM_TWIN : !strcmp(env, "check") ? JEMM_CHECK : env[0] == '0' ? JEMM_OWN : JEMM_TWIN;
        if (priv_classic()) m = JEMM_OWN;
    }
    return (JemmMode)m;
}

struct Jemm {
    int code;                   // 0 not looked at, 1 JEMM's code as we know it, -1 not
    unsigned calls, pairs, left, checked, differ;
    unsigned mapped, unmapped, large, small, how2, errors, faults, emptyCalls;     // (the paths taken, pairs)
    // the check: what the twin said at the top, compared at the exit
    bool open;
    Bit16u bp, sp;
    u32 eax, ecx, esi, edi, flags;
    std::map<PhysPt, Bit8u> wrote;
} g_jemm;

// One pass over the pairs.  `real`: memory written as JEMM writes it; else kept in `wrote` (read back from there).
struct JemmRun {
    bool real;
    std::map<PhysPt, Bit8u> *wrote;
    bool fault;
    bool rd(PhysPt a, int size, u32 &v) {
        v = 0;
        for (int k = 0; k < size; k++) {
            Bit8u b = 0;
            if (!real) {
                auto it = wrote->find(a + k);
                if (it != wrote->end()) { v |= (u32)it->second << (8 * k); continue; }
            }
            if (mem_readb_checked(a + k, &b)) { fault = true; return false; }
            v |= (u32)b << (8 * k);
        }
        return true;
    }
    bool probe(PhysPt a, int size) { u32 v; return rd(a, size, v); }
    void wr(PhysPt a, int size, u32 v) {
        if (real) {
            if (size == 4 ? mem_writed_checked(a, v) : mem_writew_checked(a, (Bit16u)v)) fault = true;
            return;
        }
        for (int k = 0; k < size; k++) (*wrote)[a + k] = (Bit8u)(v >> (8 * k));
    }
};

bool jemm_code_known() {
    if (g_jemm.code) return g_jemm.code > 0;
    const PhysPt at = SegPhys(cs) + kJemmFrom;
    u32 h = 2166136261u;
    for (int i = 0; i < kJemmLen; i++) {
        Bit8u b;
        if (mem_readb_checked(at + i, &b)) return false;         // (not now: tried again next time)
        h = (h ^ b) * 16777619u;
    }
    g_jemm.code = h == kJemmSum ? 1 : -1;
    wclog(1, "Privateer: JEMM's map of handle pages %s (%08x at %04X:%04X)", g_jemm.code > 0 ? "found, made natively" :
          "is not the code we know: JEMM's own", h, kJemmSel, kJemmFrom);
    return g_jemm.code > 0;
}

inline bool parity_even(Bit8u v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }

// The pairs from the loop's top.  Returns the pairs done; *exited: the loop's end reached (the registers, flags and
// [bp+0Ah] then as at 5D44), else the state is the loop's top before the pair that is JEMM's to do.
int jemm_pairs(JemmRun &run, u32 &eax, u32 &ecx, u32 &esi, u32 &edi, bool &carry, bool &haveCarry, u32 &exitFlags,
               bool &exited) {
    const PhysPt ssb = SegPhys(ss), dsb = SegPhys(ds), fsb = SegPhys(fs);
    const Bit16u bp = reg_bp;
    const u32 ebx = reg_ebx;
    auto st = [&](int off) { return ssb + (Bit16u)(bp + off); };
    exited = false;
    int done = 0;
    for (;;) {
        u32 count, pairAt, logical, physical, large, pages, list = 0, base, frame = 0, how;
        if (!run.rd(st(0x0A), 2, count)) return done;
        const Bit16u left = (Bit16u)(count - 1);
        if ((Bit16s)count <= 0) {                   // dec word [bp+0Ah]; jl 5D44
            if (!haveCarry) return done;            // (nothing done yet: the CF the dec keeps is JEMM's to keep)
            run.wr(st(0x0A), 2, left);
            const Bit8u lo = (Bit8u)left;
            exitFlags = (carry ? FLAG_CF : 0) | (parity_even(lo) ? FLAG_PF : 0) | ((left & 0x0F) == 0x0F ? FLAG_AF : 0) |
                        (left == 0 ? FLAG_ZF : 0) | ((left & 0x8000) ? FLAG_SF : 0) | ((Bit16u)count == 0x8000 ? FLAG_OF : 0);
            exited = !run.fault;
            return done;
        }
        if (!run.rd(st(0x06), 4, pairAt) || !run.rd(fsb + pairAt, 2, logical) || !run.rd(fsb + pairAt + 2, 2, physical) ||
            !run.rd(st(-2), 1, large) || !run.rd(st(0x0C), 2, how) || !run.rd(dsb + ebx + 2, 2, pages) ||
            !run.rd(dsb + 0xDA, 4, base)) return done;
        if (physical > (large ? 0x17u : 3u)) { g_jemm.errors++; return done; }   // (JEMM's 8Bh)
        const Bit16u last = (Bit16u)(pages - 1);
        if (logical != 0xFFFF) {
            if (logical > last) { g_jemm.errors++; return done; }                 // (JEMM's 8Ah)
            if (!run.rd(dsb + ebx + 0x0C, 4, list)) return done;
        }
        u32 frameAt = 4;
        if (!(how & 2)) {
            if (!run.rd(dsb + 0xEE, 4, frame)) return done;
            frameAt = large ? 0x280 : frame;
        }
        const u32 from = logical != 0xFFFF ? list + (logical << 4) : 0;
        const u32 to = base + frameAt + (physical << 4);
        for (int k = 0; k < 4; k++) {
            if ((from && !run.probe(dsb + from + 4 * k, 4)) || !run.probe(fsb + to + 4 * k, 4)) return done;
        }
        if (!run.probe(st(-6), 2) || !run.probe(st(-4), 2)) return done;
        // the pair, as JEMM does it
        run.wr(st(0x0A), 2, left);
        run.wr(st(0x06), 4, pairAt + 4);
        run.wr(st(-6), 2, logical);
        run.wr(st(-4), 2, physical);
        esi = from;
        edi = to;
        ecx = frameAt & 0xFFFF0000u;                // (mov ecx,..; add edi,ecx; mov cx,4; loop x4: CX 0)
        u32 v = 0x000A0007;
        for (int k = 0; k < 4; k++) {
            if (from && !run.rd(dsb + esi, 4, v)) return -1;
            run.wr(fsb + edi, 4, v);
            if (run.fault) return -1;
            carry = (u64)(from ? esi : edi) + 4 > 0xFFFFFFFFu;     // (the last add's: add esi,4 or add edi,4)
            edi += 4;
            if (from) esi += 4;
        }
        eax = v;
        haveCarry = true;
        done++;
        (from ? g_jemm.mapped : g_jemm.unmapped)++;
        (large ? g_jemm.large : g_jemm.small)++;
        if (how & 2) g_jemm.how2++;
    }
}

void jemm_enter() {
    if (!jemm_code_known()) return;
    g_jemm.calls++;
    u32 eax = reg_eax, ecx = reg_ecx, esi = reg_esi, edi = reg_edi, exitFlags = 0;
    bool carry = false, haveCarry = false, exited = false;
    if (jemm_mode() == JEMM_CHECK) {
        if (g_jemm.open) return;
        g_jemm.wrote.clear();
        JemmRun run = { false, &g_jemm.wrote, false };
        const int n = jemm_pairs(run, eax, ecx, esi, edi, carry, haveCarry, exitFlags, exited);
        if (n <= 0 || !exited || run.fault) {
            g_jemm.left++;
            return;
        }
        g_jemm.open = true;
        g_jemm.bp = reg_bp;
        g_jemm.sp = reg_sp;
        g_jemm.eax = eax;
        g_jemm.ecx = ecx;
        g_jemm.esi = esi;
        g_jemm.edi = edi;
        g_jemm.flags = exitFlags;
        g_jemm.pairs += n;
        return;
    }
    JemmRun run = { true, nullptr, false };
    const int n = jemm_pairs(run, eax, ecx, esi, edi, carry, haveCarry, exitFlags, exited);
    if (n < 0) {
        // (a page-table write or a read that had been tried faulted after all: not expected; JEMM's code is not
        // handed a half-done pair -- the emulation stops being exact here, so it says so loudly)
        wclog(0, "Privateer: JEMM twin: a fault inside a pair it had tried (%04X:%04X): the map may be half done", kJemmSel, kJemmTop);
        return;
    }
    if (n == 0 && !exited) {
        g_jemm.left++;
        return;
    }
    g_jemm.pairs += n;
    reg_eax = eax;
    reg_ecx = ecx;
    reg_esi = esi;
    reg_edi = edi;
    FillFlags();
    const u32 cond = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    if (exited) {
        reg_flags = (reg_flags & ~cond) | exitFlags;
        reg_eip = kJemmExit;
    } else {
        SETFLAGBIT(CF, carry);                      // (back at the top: its dec keeps CF, sets the rest)
        reg_eip = kJemmTop;
    }
    lflags.type = t_UNKNOWN;
    static unsigned n2;
    if (++n2 % 50000 == 0) {
        wclog(1, "Privateer: JEMM twin: %u maps made (%u pairs: %u mapped, %u unmapped, %u large frame, %u 64 KB frame), %u left "
              "to JEMM (%u at its error paths)", g_jemm.calls, g_jemm.pairs, g_jemm.mapped, g_jemm.unmapped, g_jemm.large,
              g_jemm.small, g_jemm.left, g_jemm.errors);
    }
}

void jemm_exit() {
    if (!g_jemm.open) return;
    g_jemm.open = false;
    if (reg_bp != g_jemm.bp || reg_sp != g_jemm.sp) return;        // (not the call that was looked at)
    FillFlags();
    const u32 cond = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    bool same = reg_eax == g_jemm.eax && reg_ecx == g_jemm.ecx && reg_esi == g_jemm.esi && reg_edi == g_jemm.edi &&
                (reg_flags & cond) == g_jemm.flags;
    PhysPt badAt = 0;
    for (const auto &w : g_jemm.wrote) {
        Bit8u b = 0;
        if (mem_readb_checked(w.first, &b) || b != w.second) { same = false; badAt = w.first; break; }
    }
    g_jemm.checked++;
    if (!same && ++g_jemm.differ <= 8) {
        wclog(0, "Privateer: JEMM check differs: JEMM eax %08x ecx %08x esi %08x edi %08x flags %03x, the twin %08x %08x %08x %08x %03x; "
              "memory %s%08X", reg_eax, reg_ecx, reg_esi, reg_edi, (unsigned)(reg_flags & cond), g_jemm.eax, g_jemm.ecx,
              g_jemm.esi, g_jemm.edi, (unsigned)g_jemm.flags, badAt ? "first differing at " : "the same", (unsigned)badAt);
    }
    if (g_jemm.checked % 5000 == 0) {
        wclog(1, "Privateer: JEMM check: %u maps compared (%u pairs: %u mapped, %u unmapped, %u in the large frame, %u in the "
              "64 KB frame, %u by the 'how' flag 2), %u differ, %u left to JEMM (%u at its error paths)", g_jemm.checked,
              g_jemm.pairs, g_jemm.mapped, g_jemm.unmapped, g_jemm.large, g_jemm.small, g_jemm.how2, g_jemm.differ, g_jemm.left,
              g_jemm.errors);
    }
}

bool jemm_hook(Bit32u ip) {
    if ((ip != kJemmTop && ip != kJemmExit) || SegValue(cs) != kJemmSel || !cpu.pmode || (reg_flags & FLAG_VM) ||
        jemm_mode() == JEMM_OWN) return false;
    if (ip == kJemmTop) jemm_enter();
    else jemm_exit();
    return true;
}

}  // namespace

// A base's screens wait for the player in a loop around DOS's "flush the keyboard's buffer" (INT 21h AH=0Ch): 62,000
// calls a second at 30,000 cycles (counted at Achilles' concourse), each one of the game's protected mode's trips to
// DOS through its memory manager (a VCPI switch there and back, INT 67h DE0Ch, as many): the whole of the emulated CPU,
// and the browser's page was 96 to 100 in 100 busy there (the user, 2026-11-22: "the sound was kind scratchy in the
// base"; then "less emulated CPU at bases only").  The screen there changes on the game's clock (10 times a second in
// the concourse, 5 to 6 in the bar, the same at 30,000, 15,000, 10,000 and 6,000 cycles: the user, "lets evaluate if
// animations can keep up"), so the loop's spinning is waiting.  Now, outside a flight, a call within 0.1 ms of the last
// one (the loop going round, not a key read in the middle of work) ends the millisecond's slice, as a hlt does: the
// loop goes round twice a millisecond instead of 62 times, and everything else -- the drawing between its calls, a
// load, the timer's music -- keeps the whole of its cycles.  WCPRIVIDLE=0 (and the game as it was) leave the loop as
// it is.
void priv_dos_keyboard_poll() {
    if (!priv_found() || !idle_wanted() || priv_in_flight()) return;
    static double last = -1e9;
    static unsigned idled = 0;
    const double now = PIC_FullIndex();
    const bool round = now - last < 0.1;
    last = now;
    if (!round || CPU_Cycles <= 0) return;
    CPU_IODelayRemoved += CPU_Cycles;
    CPU_Cycles = 0;
    if (idled++ == 0) wclog(1, "Privateer: a base's wait for a key idled (DOS's keyboard flush called round a loop)");
}

void priv_native_watch(void (*watch)(Bit16u ip)) {
    memset(g_only, 0, sizeof(g_only));
    memset(g_return, 0, sizeof(g_return));
    g_open = 0;
    for (int i = 0; i < kTwinCount; i++) g_twins[i].state = 0;
    if (mode() != MODE_GAME) {
        for (int i = 0; i < kTwinCount; i++) g_only[g_twins[i].ip] = !wc_net_watch[g_twins[i].ip];
        for (int i = 0; i < kTwinCount; i++) watch(g_twins[i].ip);
    }
    if (idle_wanted()) {
        g_only[kFrameWait] = !wc_net_watch[kFrameWait];
        watch(kFrameWait);
    }    if (jemm_mode() != JEMM_OWN) {
        g_jemm.code = 0;
        g_jemm.open = false;
        g_only[kJemmTop] = !wc_net_watch[kJemmTop];
        watch(kJemmTop);
        if (jemm_mode() == JEMM_CHECK) {
            g_only[kJemmExit] = !wc_net_watch[kJemmExit];
            watch(kJemmExit);
        }
    }
}

bool priv_native_hook(Bit32u ip) {
    if (ip > 0xFFFF || !priv_found()) return false;
    if ((ip == kJemmTop || ip == kJemmExit) && jemm_hook(ip)) return true;
    if (g_open && g_return[ip]) compare_returned();
    const Bit16u para = (Bit16u)(SegValue(cs) - priv_load_seg());
    if (ip == kFrameWait && para == kSeg075 && idle_wanted()) {
        frame_wait();
        return true;
    }
    if (mode() != MODE_GAME) {
        for (int i = 0; i < kTwinCount; i++) {
            Twin &t = g_twins[i];
            if (t.ip == ip && t.para == para) {
                entered(t);
                return true;
            }
        }
    }
    return g_only[ip];
}

}  // namespace wc
