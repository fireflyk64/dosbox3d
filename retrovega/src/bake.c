/* bake.c: a baked planet's ground asked (bake.h): where a point lies on the cube's faces or in one of the mission's
 * patches, and the ground's height there, the four cells round it weighed.  The sums are terrain.c's own (tr_locate,
 * tr_height_at, tr_patch_find, tr_column, tr_place) on the bake's grid, in integers whose every step has one answer:
 * RV.EXE on DOS, the native builds and the tools all get the same numbers.
 *
 * c_bk_column and c_bk_muldiv are the sums in C: the native builds', the baker's, and on DOS the twins bake.asm's
 * rv_bk_column and rv_bk_muldiv are checked against (RVP.EXE /twins; RV.EXE links neither: nothing in it calls them). */
#include "bake.h"

#ifdef __WATCOMC__
typedef long long bk_wide;
typedef unsigned long long bk_uwide;
#else
typedef int64_t bk_wide;
typedef uint64_t bk_uwide;
#endif

/* the file's bytes, n (even) from `at`: rv.h's xms_get_at (xms.c's; the hosts'; a tool's own, the file in its memory) */
int xms_get_at(unsigned handle, unsigned long off, void BK_FAR *dst, unsigned long n);
#define BK_FETCH(at, dst, n) ((void)xms_get_at(bk_ask.handle, bk_ask.base + (at), (dst), (n)))

BkAsk BK_FAR bk_ask;

/* --- the sums' pieces: each a few instructions of a 386's (the product of two 32-bit numbers whole in 64) --- */

static uint32_t c_isqrt3(int32_t x, int32_t y, int32_t z)
{
    bk_uwide n = (bk_uwide)((bk_wide)x * x) + (bk_uwide)((bk_wide)y * y) + (bk_uwide)((bk_wide)z * z);
    uint32_t r, q;
    int s = 0;
    if (!n)
        return 0;
    /* Newton's steps down from a power of two over the root (the root is under 2^32: n is under 3 * 2^62) */
    while (s < 32 && (n >> (2 * s)))
        s++;
    r = s >= 32 ? 0xFFFFFFFFUL : (uint32_t)1 << s;
    for (;;) {
        q = (uint32_t)(((bk_uwide)r + n / r) >> 1);
        if (q >= r)
            break;
        r = q;
    }
    return r > 0x7FFFFFFFUL ? 0x7FFFFFFFUL : r;
}

int32_t c_bk_muldiv(int32_t a, int32_t b, int32_t c)
{
    return (int32_t)(((bk_wide)a * b) / c);
}

static int32_t c_scale(int32_t a, int32_t t)
{
    return (int32_t)(((bk_wide)a * t) >> 16);
}

#define BK_LERP(p, q, t) ((p) + c_scale((q) - (p), (t)))

/* where a planet-relative point falls in a patch: 1 and its place on the patch's grid in 65536ths of a cell
 * (terrain.c's tr_patch_find; a point more than twice the radius out along the patch's middle is in none) */
static int c_patch_find(const BkPatch *pt, int32_t x, int32_t y, int32_t z, int32_t *gx, int32_t *gy)
{
    bk_wide sc = (bk_wide)x * pt->c[0] + (bk_wide)y * pt->c[1] + (bk_wide)z * pt->c[2], k, u, v;
    if (sc <= 0)
        return 0;
    k = sc / bk_ask.radius;             /* (the point's along c over the radius, 2^30 = 1) */
    if (k <= 0 || k > 0x7FFFFFFFL)
        return 0;
    u = ((bk_wide)x * pt->a[0] + (bk_wide)y * pt->a[1] + (bk_wide)z * pt->a[2]) / k;   /* (positions on the tangent plane) */
    v = ((bk_wide)x * pt->b[0] + (bk_wide)y * pt->b[1] + (bk_wide)z * pt->b[2]) / k;
    if (u <= -pt->half_a || u >= pt->half_a || v <= -pt->half_b || v >= pt->half_b)
        return 0;
    *gx = c_bk_muldiv((int32_t)u + pt->half_a, 65536L, pt->cell);
    *gy = c_bk_muldiv((int32_t)v + pt->half_b, 65536L, pt->cell);
    return 1;
}

/* --- */

uint32_t bk_key(const void BK_FAR *planet_rec, const void BK_FAR *navs, int nnav)
{
    const uint8_t BK_FAR *p = (const uint8_t BK_FAR *)planet_rec;
    uint32_t h = 0x811C9DC5UL;
    int i, n = 65;                      /* (terrain.h's TrPlanetRec, then its TrNavRecs, 20 bytes each) */
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x01000193UL;
    p = (const uint8_t BK_FAR *)navs;
    n = nnav * 20;
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x01000193UL;
    h = (h ^ BK_VERSION) * 0x01000193UL;
    return h;
}

int bk_begin(unsigned handle, uint32_t base)
{
    BkHead h;
    BkPatch p;
    int k, j;
    bk_ask.n = 0;
    bk_ask.handle = (uint16_t)handle;
    bk_ask.base = base;
    BK_FETCH(0, &h, sizeof h);
    if (h.magic[0] != 'R' || h.magic[1] != 'V' || h.magic[2] != 'P' || h.magic[3] != 'L' || h.version != BK_VERSION
        || h.npatch > BK_PATCHES || h.radius <= 0 || h.n < 16 || h.n > 1024)
        return -1;
    bk_ask.npatch = h.npatch;
    bk_ask.radius = h.radius;
    bk_ask.grid_at = h.grid_at;
    bk_ask.patch_at = h.patch_at;
    for (k = 0; k < (int)h.npatch; k++) {   /* (each patch's quick test: the rest of it stays in the file) */
        BK_FETCH(h.patch_at + (uint32_t)k * sizeof p, &p, sizeof p);
        for (j = 0; j < 3; j++)
            bk_ask.q[k].c[j] = (int16_t)(p.c[j] >> 16);
        bk_ask.q[k].cosmin = p.cosmin;
    }
    bk_ask.n = h.n;
    return 0;
}

static int32_t c_h16(const uint8_t *c) { return (int16_t)((uint16_t)c[0] | ((uint16_t)c[1] << 8)); }

/* the ground at a place on face f's grid (65536ths of a cell, cell i's middle at i * 65536): positions over the datum,
 * the four cells round it weighed */
static int32_t c_height_at(int f, int32_t gi, int32_t gj)
{
    int32_t i = gi >> 16, j = gj >> 16, fi = gi & 0xFFFF, fj = gj & 0xFFFF, a, b;
    int32_t n = bk_ask.n, side = n + 2;
    uint8_t c[2][2 * BK_CELL];
    uint32_t at;
    /* (gi and gj are -32768 .. (2 n - 1) 32768, a point's two lesser parts over its largest: i and j -1 .. n - 1,
     * always in the face's rows with their border; the asm has no test for more) */
    at = bk_ask.grid_at + (((uint32_t)f * (uint32_t)side + (uint32_t)(j + 1)) * (uint32_t)side + (uint32_t)(i + 1)) * BK_CELL;
    BK_FETCH(at, c[0], 2 * BK_CELL);
    BK_FETCH(at + (uint32_t)side * BK_CELL, c[1], 2 * BK_CELL);
    a = BK_LERP(c_h16(c[0]) * BK_HU, c_h16(c[0] + BK_CELL) * BK_HU, fi);
    b = BK_LERP(c_h16(c[1]) * BK_HU, c_h16(c[1] + BK_CELL) * BK_HU, fi);
    return BK_LERP(a, b, fj);
}

static uint16_t c_isqrt32(uint32_t n)
{
    uint32_t r = 0, bit = 0x40000000UL;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else
            r >>= 1;
        bit >>= 2;
    }
    return (uint16_t)r;
}

void c_bk_column(int32_t x, int32_t y, int32_t z, BkColumn *out)
{
    int32_t c[3], m, gi, gj;
    int a, f, k;
    out->slab_lo = out->slab_hi = 0;
    out->r = (int32_t)c_isqrt3(x, y, z);
    out->ground = 0;
    if (!bk_ask.n)
        return;
    c[0] = x;
    c[1] = y;
    c[2] = z;
    a = 0;
    if ((c[1] < 0 ? -c[1] : c[1]) > (c[a] < 0 ? -c[a] : c[a]))
        a = 1;
    if ((c[2] < 0 ? -c[2] : c[2]) > (c[a] < 0 ? -c[a] : c[a]))
        a = 2;
    m = c[a] < 0 ? -c[a] : c[a];
    if (bk_ask.npatch && m > 0) {
        /* the mission's patches first.  The point's direction in 16 bits and its length, once: a patch whose middle
         * it is far from is passed over for three 16-bit products (cosmin: the baker's, with room for the rounding);
         * a patch it is near is read from the file */
        int16_t d[3];
        int32_t len;
        int s = 0;
        while ((m >> s) >= 16384)
            s++;
        d[0] = (int16_t)(x >> s);
        d[1] = (int16_t)(y >> s);
        d[2] = (int16_t)(z >> s);
        len = c_isqrt32((uint32_t)((int32_t)d[0] * d[0]) + (uint32_t)((int32_t)d[1] * d[1]) + (uint32_t)((int32_t)d[2] * d[2]));
        for (k = 0; k < (int)bk_ask.npatch; k++) {
            const BkQuick BK_FAR *qk = &bk_ask.q[k];
            int32_t dot = (int32_t)d[0] * qk->c[0] + (int32_t)d[1] * qk->c[1] + (int32_t)d[2] * qk->c[2];
            BkPatch pt;
            int32_t gx, gy;
            if (dot < len * qk->cosmin)
                continue;
            BK_FETCH(bk_ask.patch_at + (uint32_t)k * sizeof pt, &pt, sizeof pt);
            if (c_patch_find(&pt, x, y, z, &gx, &gy)) {
                int32_t i = gx >> 16, j = gy >> 16, fx = gx & 0xFFFF, fy = gy & 0xFFFF, p, q;
                uint8_t e[2][2 * BK_CELL];
                uint32_t at;
                if (i > (int32_t)pt.na - 1) { i = (int32_t)pt.na - 1; fx = 65535L; }
                if (j > (int32_t)pt.nb - 1) { j = (int32_t)pt.nb - 1; fy = 65535L; }
                at = (uint32_t)j * ((uint32_t)pt.na + 1) + (uint32_t)i;
                BK_FETCH(pt.at + at * BK_CELL, e[0], 2 * BK_CELL);
                BK_FETCH(pt.at + (at + pt.na + 1) * BK_CELL, e[1], 2 * BK_CELL);
                p = BK_LERP(c_h16(e[0]) * BK_HU, c_h16(e[0] + BK_CELL) * BK_HU, fx);
                q = BK_LERP(c_h16(e[1]) * BK_HU, c_h16(e[1] + BK_CELL) * BK_HU, fx);
                out->ground = bk_ask.radius + BK_LERP(p, q, fy);
                if (pt.slabs) {         /* the slab of the point nearest */
                    uint8_t sl[BK_SLAB];
                    int32_t lo, hi;
                    at += (fy >= 32768L ? (uint32_t)pt.na + 1 : 0) + (fx >= 32768L);
                    BK_FETCH(pt.slab_at + at * BK_SLAB, sl, BK_SLAB);
                    lo = c_h16(sl);
                    hi = c_h16(sl + 2);
                    if (lo < hi) {
                        out->slab_lo = bk_ask.radius + lo * BK_HU;
                        out->slab_hi = bk_ask.radius + hi * BK_HU;
                    }
                }
                return;
            }
        }
    }
    /* the planet's own ground: the face the point is over (its middle on axis f >> 1, + for even f; u along the next
     * axis, v the one after), and its place on the face's grid */
    f = 2 * a + (c[a] < 0);
    if (m == 0)
        gi = gj = 0;
    else {
        int32_t n = bk_ask.n;
        gi = c_bk_muldiv(c[(a + 1) % 3], n * 32768L, m) + (n - 1) * 32768L;
        gj = c_bk_muldiv(c[(a + 2) % 3], n * 32768L, m) + (n - 1) * 32768L;
    }
    out->ground = bk_ask.radius + c_height_at(f, gi, gj);
}

void bk_place(int32_t v[3], const BkColumn *c, int32_t over)
{
    int32_t want = c->ground + over;
    int k;
    if (c->r <= 0)
        return;
    for (k = 0; k < 3; k++)
        v[k] = bk_muldiv(v[k], want, c->r);
}
