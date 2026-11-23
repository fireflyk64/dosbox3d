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
 *  The flight's loop waits for its next frame by spinning on the 600 Hz clock (seg075:062B), two thirds of the
 *  emulated CPU in quiet space: there the rest of the emulated slice is skipped, as a hlt's is.
 *
 *  WCPRIVTWINS: unset or 1 the twins; 0 the game's own code; "check" the game's own code, and at each return what
 *  it left compared with what the twin said (logged).  WCPRIVIDLE=0: the wait spins as the game's does.
 */
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "wcnet_priv_native.h"
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "cpu.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"

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

// ---- the hooks ------------------------------------------------------------------------------------------------------

struct Twin {
    Bit16u para, ip, len;       // the routine's code from its entry
    u32 sum;                    // ... its FNV-1a, as it is in the file
    Bit16u moved[4];            // the words DOS relocates in it (0 none): the load segment taken off to compare
    void (*make)(Run &);
    const char *name;
    int state;                  // 0 not seen yet, 1 the game's code as we know it, -1 not
    unsigned made, left, checked, differ;
};

Twin g_twins[] = {
    { 0x4C5B, 0x0296, 0x060, 0xCA8867CF, { 0x0000, 0x0000, 0x0000, 0x0000 }, length,            "seg128:0296", 0, 0, 0, 0, 0 },
    { 0x4C5B, 0x02F6, 0x056, 0xE5DFD2C5, { 0x0000, 0x0000, 0x0000, 0x0000 }, root,              "seg128:02F6", 0, 0, 0, 0, 0 },
    { 0x4C5B, 0x0390, 0x0C0, 0xEE187329, { 0x0000, 0x0000, 0x0000, 0x0000 }, transform_rows,    "seg128:0390", 0, 0, 0, 0, 0 },
    { 0x4C5B, 0x0450, 0x0C0, 0xFD1772F9, { 0x0000, 0x0000, 0x0000, 0x0000 }, transform_columns, "seg128:0450", 0, 0, 0, 0, 0 },
    { 0x3B4B, 0x02DC, 0x04F, 0x00FF0769, { 0x0000, 0x0000, 0x0000, 0x0000 }, absolute,          "seg095:02DC", 0, 0, 0, 0, 0 },
    { 0x3B4B, 0x0247, 0x053, 0x04763F20, { 0x0265, 0x0272, 0x0000, 0x0000 }, root_of,           "seg095:0247", 0, 0, 0, 0, 0 },
    { 0x4563, 0x0133, 0x0E1, 0x7F20D540, { 0x0000, 0x0000, 0x0000, 0x0000 }, normalise_up,      "seg115:0133", 0, 0, 0, 0, 0 },
    { 0x4563, 0x0217, 0x0C3, 0x974F18A6, { 0x0000, 0x0000, 0x0000, 0x0000 }, normalise_down,    "seg115:0217", 0, 0, 0, 0, 0 },
    { 0x4563, 0x0962, 0x0F6, 0xE94D348F, { 0x0976, 0x0995, 0x09B4, 0x0000 }, shift_within,      "seg115:0962", 0, 0, 0, 0, 0 },
    { 0x4563, 0x02DD, 0x11C, 0xEEA1D99E, { 0x03A8, 0x0000, 0x0000, 0x0000 }, distance,          "seg115:02DD", 0, 0, 0, 0, 0 },
    { 0x54D1, 0x000A, 0x04D, 0x28E4BB71, { 0x0000, 0x0000, 0x0000, 0x0000 }, clear,             "seg181:000A", 0, 0, 0, 0, 0 },
    { 0x5D17, 0x0002, 0x06B, 0x0B790087, { 0x0000, 0x0000, 0x0000, 0x0000 }, copy_spans,        "seg205:0002", 0, 0, 0, 0, 0 },
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

void apply(const Run &r, Bit16u retIp, Bit16u retCs) {
    for (int i = 0; i < r.writes; i++) {
        if (r.size[i] == 4) mem_writed_checked(r.at[i], r.val[i]);
        else if (r.size[i] == 2) mem_writew_checked(r.at[i], (Bit16u)r.val[i]);
        else mem_writeb_checked(r.at[i], (Bit8u)r.val[i]);
    }
    for (int i = 0; i < r.bytes; i++) mem_writeb_checked(g_byteAt[i], g_byteVal[i]);
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
    reg_sp = (Bit16u)(r.sp + 4);
    SegSet16(cs, retCs);
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
        if (reg_sp < (Bit16u)(p.sp + 4)) return;        // (still inside it)
        g_open--;
        if (reg_ip != p.retIp || SegValue(cs) != p.retCs || reg_sp != (Bit16u)(p.sp + 4)) continue;
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
    const Bit16u retIp = get16(r, r.ss, r.sp), retCs = get16(r, r.ss, (Bit16u)(r.sp + 2));
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
    apply(r, retIp, retCs);
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

}  // namespace

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
    }
}

bool priv_native_hook(Bit32u ip) {
    if (ip > 0xFFFF || !priv_found()) return false;
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
