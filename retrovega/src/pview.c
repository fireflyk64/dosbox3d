/* pview.c: a planet's ground drawn without the card, 128 x 80 (pview.h).  terrain.c's march (tr_view, tr_quarter,
 * tr_line, tr_fill_ground) in integers, from the bake's pyramid instead of terrain.c's grids. */
#include <string.h>
#ifdef PV_DEBUG
#include <stdio.h>
#include <stdlib.h>
#endif
#include "pview.h"
#include "bake.h"

#ifdef __WATCOMC__
#define PV_ASM __far __cdecl
#else
#define PV_ASM
#endif

int xms_get_at(unsigned handle, unsigned long off, void PV_FAR *dst, unsigned long n);

typedef long long i64;
#define M64(a, b) ((i64)(a) * (i64)(b))

/* --- the file --- */

static struct {
    int on;
    unsigned handle;
    uint32_t base;                      /* the section's start in the handle's memory */
    PvHead h;
    PvLevel lv[PV_LEVELS];
    PvFog fog;
    PvGlow glow;
    PvSlabs slabs;
    int16_t slab[PV_SLAB_CELLS * 2];    /* the slab grids' cells, one after another (slab_at[k]: grid k's first) */
    int slab_first[PV_SLABS];
    PvDither dither[PV_FOGS * 256];
    int32_t wox[PV_LEVELS], woy[PV_LEVELS];
    int wok[PV_LEVELS];
    uint8_t win[PV_LEVELS][PV_W * PV_W * PV_CELL + 2];
    int16_t bwin[PV_LEVELS][PV_BW * PV_BW];
} g_pv;

static void pv_get(uint32_t at, void *dst, uint32_t n)
{
    (void)xms_get_at(g_pv.handle, g_pv.base + at, dst, (n + 1) & ~1UL);
}

int pv_begin(unsigned handle, uint32_t base, uint32_t at)
{
    PvHead *h = &g_pv.h;
    g_pv.on = 0;
    g_pv.handle = handle;
    g_pv.base = base + at;
    pv_get(0, h, sizeof *h);
    if (memcmp(h->magic, "RVPV", 4) || h->nlev < 1 || h->nlev > PV_LEVELS || h->w != PV_W || h->snap != PV_SNAP
        || h->radius <= 0 || h->shift0 < 8 || h->shift0 + h->nlev > 30)
        return -1;
    pv_get(h->lev_at, g_pv.lv, sizeof(PvLevel) * h->nlev);
    pv_get(h->fog_at, &g_pv.fog, sizeof g_pv.fog);
    pv_get(h->dither_at, g_pv.dither, sizeof g_pv.dither);
    pv_get(h->glow_at, &g_pv.glow, sizeof g_pv.glow);
    pv_get(h->slabs_at, &g_pv.slabs, sizeof g_pv.slabs);
    {
        int k, n = 0;
        if (g_pv.slabs.n > PV_SLABS || (g_pv.slabs.n && (g_pv.slabs.shift < 8 || g_pv.slabs.shift > 20)))
            return -1;
        for (k = 0; k < g_pv.slabs.n; k++) {
            int32_t c = (int32_t)g_pv.slabs.s[k].w * g_pv.slabs.s[k].h;
            if (n + c > PV_SLAB_CELLS)
                return -1;
            g_pv.slab_first[k] = n;
            pv_get(g_pv.slabs.s[k].at, g_pv.slab + 2 * n, (uint32_t)c * 4);
            n += c;
        }
    }
    memset(g_pv.wok, 0, sizeof g_pv.wok);
    g_pv.on = 1;
    return 0;
}

void pv_end(void) { g_pv.on = 0; }
int pv_on(void) { return g_pv.on; }

/* level l's window with its corner at cell (ox, oy): the level's cells there, no ground elsewhere */
static const uint8_t *pv_window(int l, int32_t ox, int32_t oy)
{
    const PvLevel *lv = &g_pv.lv[l];
    uint8_t *w = g_pv.win[l];
    int j;
    if (g_pv.wok[l] && g_pv.wox[l] == ox && g_pv.woy[l] == oy)
        return w;
    pv_get(g_pv.h.absent_at, w, PV_W * PV_W * PV_CELL + 2);
    for (j = 0; j < PV_W; j++) {
        int32_t r = oy + j - lv->y0, a0, a1, ax0;
        PvRow row;
        if (r < 0 || r >= lv->h)
            continue;
        pv_get(lv->rows_at + (uint32_t)r * sizeof row, &row, sizeof row);
        if (!row.n)
            continue;
        ax0 = lv->x0 + row.x0;
        a0 = ox > ax0 ? ox : ax0;
        a1 = ox + PV_W < ax0 + row.n ? ox + PV_W : ax0 + row.n;
        if (a0 >= a1)
            continue;
        pv_get(row.at + (uint32_t)(a0 - ax0) * PV_CELL, w + ((uint32_t)j * PV_W + (a0 - ox)) * PV_CELL,
               (uint32_t)(a1 - a0) * PV_CELL);
    }
    /* its blocks' highest ground */
    {
        int16_t *b = g_pv.bwin[l];
        int32_t bx = ox / PV_BLK - lv->bx0, by = oy / PV_BLK - lv->by0, i;
        for (j = 0; j < PV_BW; j++)
            for (i = 0; i < PV_BW; i++) {
                int16_t v = PV_NOGROUND;
                if (by + j >= 0 && by + j < lv->bh && bx + i >= 0 && bx + i < lv->bw)
                    pv_get(lv->bmax_at + ((uint32_t)(by + j) * lv->bw + (bx + i)) * 2, &v, 2);
                b[j * PV_BW + i] = v;
            }
    }
    g_pv.wox[l] = ox;
    g_pv.woy[l] = oy;
    g_pv.wok[l] = 1;
    return w;
}

/* --- integers --- */

static uint32_t pv_isqrt(unsigned long long v)
{
    unsigned long long r = 0, b = 1ULL << 62;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

static int32_t pv_dot30(const int32_t *a, const int32_t *b)   /* (2^30 vectors) */
{
    return (int32_t)((M64(a[0], b[0]) + M64(a[1], b[1]) + M64(a[2], b[2])) >> 30);
}

static int32_t pv_div(i64 a, i64 b) { return (int32_t)(a / b); }   /* (toward zero, as idiv) */
static i64 pv_floordiv(i64 a, i64 b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }   /* (b > 0) */
static i64 pv_ceildiv(i64 a, i64 b) { return -pv_floordiv(-a, b); }

/* --- the frame --- */

#define PV_DEN_MIN (1L << 26)           /* a direction with less than this along the plane's middle is past its levels */
#define PV_NEAR_DS 192                  /* the least step, plane units (three quarters of level 0's cell) */
#define PV_STEP_PX 2                    /* the step: about this many pixels' worth of the distance */
#define PV_SUBS 16                      /* a step's samples at most where it crosses a slab's grid */

typedef struct {
    int on, vert, s, near, edge, len, first, lines;
    int32_t va, vc;                     /* V along and across, 256ths of a pixel */
    uint8_t thin[PV_LW];
} PvQuarter;

static struct {
    int32_t R, D, vsh, rt, rb;
    int32_t up[3];                      /* 2^30 */
    int32_t ue, ua, ub;
    int32_t px, py;                     /* the eye's point on the plane, plane units */
    int32_t cr[3], cu[3], cf[3];        /* 2^14 */
    int nadir;
    int32_t vx, vy, lcx, lcy, lf;       /* 256ths of a picture pixel */
    int levels;                         /* the levels are drawn from (the eye is on the plane's side) */
    int32_t bx0, by0, bx1, by1;         /* the picture's part wanted (the view's), pixels */
    int nsteps;
    PvStep st[PV_STEPS];
    PvQuarter q[4];
    int nlines;
    PvLine ln[PV_LINES];
    int32_t w0[PV_LINES][3], w1[PV_LINES][3], t[PV_LINES][3];   /* (rays 2^24, t 2^30: the end test) */
    int32_t fogm[PV_FOG_HB];
    uint8_t glow[PV_GLOW_CZ];           /* the frame's glow by a ray's cosine from straight up, 8ths of a level */
    int glow_any;
    int32_t skf, skr, sku;              /* up along the camera's front, right and up, 65536ths */
    int32_t fog_rcp;
    uint16_t pic[PV_LW * PV_LH];
    uint32_t samples, seen, npx, far, setups, levpass, skips, slabs, slabpx, slabq, subs;
} g_f;

const uint16_t *pv_picture(void) { return g_f.pic; }
uint32_t pv_samples(void) { return g_f.samples; }

/* a picture point's ray (256ths of a pixel), the world's way, 2^24 */
static void pv_ray(int32_t x, int32_t y, int32_t *w)
{
    int32_t rx = pv_div(M64(x - g_f.lcx, 65536), g_f.lf), ry = pv_div(M64(y - g_f.lcy, 65536), g_f.lf);
    int k;
    for (k = 0; k < 3; k++)
        w[k] = (int32_t)((M64(g_f.cf[k], 65536) + M64(g_f.cr[k], rx) - M64(g_f.cu[k], ry)) >> 6);
}

static void pv_quarter(int which)
{
    PvQuarter *q = &g_f.q[which];
    int32_t amax, cmax, ext, xs, lo = 0x7FFFFFFF, hi = -0x7FFFFFFF, L, rn;
    int k, kk;
    q->on = 0;
    q->vert = which < 2;
    q->s = (which & 1) ? 1 : -1;
    q->va = q->vert ? g_f.vy : g_f.vx;
    q->vc = q->vert ? g_f.vx : g_f.vy;
    amax = (q->vert ? PV_LH : PV_LW) - 1;
    cmax = (q->vert ? PV_LW : PV_LH) - 1;
    q->edge = q->s < 0 ? 0 : amax;
    if (q->s > 0) {
        q->near = (q->va >> 8) + 1;
        if (q->near < 0)
            q->near = 0;
        if (q->near > amax)
            return;
    } else {
        q->near = -((-q->va) >> 8) - 1;
        if (q->near > amax)
            q->near = amax;
        if (q->near < 0)
            return;
    }
    q->len = q->s > 0 ? amax - q->near + 1 : q->near + 1;
    ext = q->edge * 256 - q->va;
    if (ext < 0)
        ext = -ext;
    for (k = 0; k < 4; k++) {
        int32_t a = (k < 2 ? q->near : q->edge) * 256, half = a - q->va, cc;
        if (half < 0)
            half = -half;
        cc = (k & 1) ? q->vc + half : q->vc - half;
        if (cc < 0) cc = 0;
        if (cc > cmax * 256) cc = cmax * 256;
        xs = q->vc + pv_div(M64(cc - q->vc, q->edge * 256 - q->va), a - q->va);
        if (xs < lo) lo = xs;
        if (xs > hi) hi = xs;
    }
    if (lo < q->vc - ext) lo = q->vc - ext;
    if (hi > q->vc + ext) hi = q->vc + ext;
    if (hi < lo)
        return;
    q->first = (lo >> 8) - 1;
    q->lines = -((-(hi - q->first * 256)) >> 8) + 2;
    /* the lines' spacing at pixel k is (k + rn) / L: a line whose index has m trailing zero bits is wanted from where
     * twice 2^m of it reaches a pixel */
    L = ext;
    rn = q->s * (q->near * 256 - q->va);
    kk = 12;
    for (k = 0; k < q->len; k++) {
        int32_t sp = k * 256 + rn;      /* (the spacing times L) */
        while (kk > 0 && ((i64)sp << kk) >= L)
            kk--;
        while (kk < 12 && ((i64)sp << (kk + 1)) < L)
            kk++;
        q->thin[k] = (uint8_t)kk;
    }
    q->on = 1;
}

/* the frame's steps: every line's samples at the same angles from straight down */
static void pv_steps(void)
{
    int32_t c = 1L << 30, s = 0, R = g_f.R, D = g_f.D, cosmax;
    int n = 0;
    {
        /* past acos(rb / D) + acos(rb / rt) no ground can be seen */
        i64 cd = pv_div(M64(g_f.rb, 1L << 30), D), ct = pv_div(M64(g_f.rb, 1L << 30), g_f.rt);
        if (D <= g_f.rb)
            cosmax = -(1L << 30);
        else {
            i64 sd = pv_isqrt((unsigned long long)((1LL << 60) - cd * cd)), stt = pv_isqrt((unsigned long long)((1LL << 60) - ct * ct));
            cosmax = (int32_t)((cd * ct - sd * stt) >> 30);
        }
    }
    while (n < PV_STEPS) {               /* (to the first step at or past the last ground that can show) */
        PvStep *st = &g_f.st[n++];
        int32_t a = (int32_t)(M64(R, c) >> 30) - D, b = (int32_t)(M64(R, s) >> 30), dist, ds, g, dt, cd, sd, x;
        int l;
        dist = (int32_t)pv_isqrt((unsigned long long)(M64(a, a) + M64(b, b)));
        ds = pv_div(M64(dist, PV_STEP_PX * 256), g_f.lf);
        if (ds < PV_NEAR_DS)
            ds = PV_NEAR_DS;
        g = -(int32_t)((M64(a, c) + M64(b, s)) >> 30);   /* (the distance times the sine of the ray's angle to the ground) */
        if (g < dist)
            ds = pv_div(M64(ds, dist), g > dist / 4 ? g : dist / 4 > 0 ? dist / 4 : 1);
        st->c = c;
        st->s = s;
        st->a = a >> g_f.vsh;
        st->b = b >> g_f.vsh;
        st->uc = (int32_t)(M64(g_f.ue, c) >> 30);
        st->dist = dist;
        st->ds = ds;
        for (l = 0; l < g_pv.h.nlev - 1 && (1L << (g_pv.h.shift0 + l)) * 2 < ds; l++)
            ;
        st->lev = (uint8_t)l;
        if (c <= cosmax)
            break;
        /* on round by dt (its sine and cosine to the third power) */
        dt = pv_div(M64(ds, 1L << 30), R);
        cd = (1L << 30) - (int32_t)(M64(dt, dt) >> 31);
        sd = dt - (int32_t)((M64((int32_t)(M64(dt, dt) >> 30), dt) >> 30) / 6);
        x = (int32_t)((M64(c, cd) - M64(s, sd)) >> 30);
        s = (int32_t)((M64(s, cd) + M64(c, sd)) >> 30);
        c = x;
    }
    g_f.nsteps = n;
}

/* the step a ray along unit (wu, wt) (in the plane of up and t) first reaches the top sphere at: -1 when never */
static int pv_enter(int32_t wu, int32_t wt)
{
    i64 n = pv_isqrt((unsigned long long)(M64(wu, wu) + M64(wt, wt))), b, disc, s, qu, qt;
    int lo = 0, hi = g_f.nsteps;
    if (!n)
        return -1;
    b = M64(g_f.D, wu) / n;
    disc = b * b - (M64(g_f.D, g_f.D) - M64(g_f.rt, g_f.rt));
    if (disc <= 0)
        return -1;
    s = -b - (i64)pv_isqrt((unsigned long long)disc);
    if (s < 0)
        return -1;
    qu = g_f.D + s * wu / n;
    qt = s * wt / n;
    while (lo < hi) {                   /* (the first step at or past it) */
        int mid = (lo + hi) / 2;
        if (M64(g_f.st[mid].s, 1) * qu - M64(g_f.st[mid].c, 1) * qt >= 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* a quarter's line i set up */
static void pv_line(PvQuarter *q, int i)
{
    PvLine *ln;
    int32_t X = (q->first + i) * 256, a0 = q->near * 256, a1 = (q->near + q->s * (q->len - 1)) * 256, e8, c0, c1;
    int32_t w[3], wk[3], wl[3], T[3], wup, *t, *w0, *w1, tl, cam[3];
    int k, m, kmin, n = g_f.nlines;
    if (n >= PV_LINES)
        return;
    ln = &g_f.ln[n];
    memset(ln, 0, sizeof *ln);
    e8 = q->edge * 256 - q->va;
    for (m = 0; m < 12 && !((i >> m) & 1); m++)
        ;
    for (kmin = 0; kmin < q->len && q->thin[kmin] > m; kmin++)
        ;
    if (kmin >= q->len)
        return;
    c0 = q->vc + pv_div(M64(X - q->vc, a0 - q->va), e8);
    c1 = q->vc + pv_div(M64(X - q->vc, a1 - q->va), e8);
    w0 = g_f.w0[n];
    w1 = g_f.w1[n];
    t = g_f.t[n];
    if (q->vert) {
        pv_ray(c0, a0, w0);
        pv_ray(c1, a1, w1);
        pv_ray(X, q->edge * 256, w);
    } else {
        pv_ray(a0, c0, w0);
        pv_ray(a1, c1, w1);
        pv_ray(q->edge * 256, X, w);
    }
    wup = pv_dot30(w, g_f.up);
    for (k = 0; k < 3; k++)
        T[k] = w[k] - (int32_t)(M64(g_f.up[k], wup) >> 30);
    tl = (int32_t)pv_isqrt((unsigned long long)(M64(T[0], T[0]) + M64(T[1], T[1]) + M64(T[2], T[2])));
    if (tl < 16)
        return;
    for (k = 0; k < 3; k++)
        t[k] = pv_div(M64(T[k], 1L << 30), tl);
    /* the line's pixels on the picture */
    ln->vert = (uint8_t)q->vert;
    ln->len = (int16_t)q->len;
    ln->kmin = (int16_t)kmin;
    ln->row = (int16_t)q->near;
    ln->drow = (int16_t)q->s;
    ln->col = c0 * 256 + 32768;
    ln->dcol = pv_div(M64(X - q->vc, (i64)q->s * 65536), e8);
    /* its march's numbers */
    {
        int32_t ta, tb, qx, qy, *cv = q->vert ? g_f.cu : g_f.cr;
        ln->te = pv_dot30(t, (const int32_t *)g_pv.h.e);
        ta = pv_dot30(t, (const int32_t *)g_pv.h.a);
        tb = pv_dot30(t, (const int32_t *)g_pv.h.b);
        qx = (int32_t)((M64(ta, g_f.ue) - M64(g_f.ua, ln->te)) >> 30);
        qy = (int32_t)((M64(tb, g_f.ue) - M64(g_f.ub, ln->te)) >> 30);
        ln->kx = pv_div(M64(g_f.R, qx), g_f.ue);
        ln->ky = pv_div(M64(g_f.R, qy), g_f.ue);
        for (k = 0; k < 3; k++)
            cam[k] = cv[k];
        ln->us = (int32_t)((M64(g_f.up[0], cam[0]) + M64(g_f.up[1], cam[1]) + M64(g_f.up[2], cam[2])) >> 34);
        ln->ts = (int32_t)((M64(t[0], cam[0]) + M64(t[1], cam[1]) + M64(t[2], cam[2])) >> 34);
        ln->uf = (int32_t)((M64(g_f.up[0], g_f.cf[0]) + M64(g_f.up[1], g_f.cf[1]) + M64(g_f.up[2], g_f.cf[2])) >> 34);
        ln->tf = (int32_t)((M64(t[0], g_f.cf[0]) + M64(t[1], g_f.cf[1]) + M64(t[2], g_f.cf[2])) >> 34);
        if (q->vert) {
            ln->ra_ = q->s * (g_f.lcy - q->near * 256);
            ln->rb_ = -q->s * g_f.lf;
        } else {
            ln->ra_ = q->s * (g_f.lcx - q->near * 256);
            ln->rb_ = q->s * g_f.lf;
        }
    }
    /* the pixels wanted: within the view's part of the picture */
    {
        int32_t k0 = kmin, k1 = q->len - 1, a0v = q->vert ? g_f.by0 : g_f.bx0, a1v = q->vert ? g_f.by1 : g_f.bx1;
        int32_t c0v = (q->vert ? g_f.bx0 : g_f.by0) * 65536, c1v = (q->vert ? g_f.bx1 : g_f.by1) * 65536 - 1, lo, hi;
        if (q->s > 0)
            lo = a0v - q->near, hi = a1v - 1 - q->near;
        else
            lo = q->near - (a1v - 1), hi = q->near - a0v;
        if (lo > k0) k0 = lo;
        if (hi < k1) k1 = hi;
        if (ln->dcol > 0) {
            lo = (int32_t)pv_ceildiv((i64)c0v - ln->col, ln->dcol);
            hi = (int32_t)pv_floordiv((i64)c1v - ln->col, ln->dcol);
        } else if (ln->dcol < 0) {
            lo = (int32_t)pv_ceildiv((i64)ln->col - c1v, -ln->dcol);
            hi = (int32_t)pv_floordiv((i64)ln->col - c0v, -ln->dcol);
        } else if (ln->col >= c0v && ln->col <= c1v)
            lo = k0, hi = k1;
        else
            lo = 1, hi = 0;
        if (lo > k0) k0 = lo;
        if (hi < k1) k1 = hi;
        if (k0 > k1)
            return;
        ln->k0 = (int16_t)k0;
        ln->k1 = (int16_t)k1;
    }
    /* the horizon: the ray just short of the first pixel wanted (the last, looking up at the ground) */
    {
        int32_t kh = g_f.nadir ? ln->k0 - 1 : ln->k1 + 1;
        for (k = 0; k < 3; k++)
            wk[k] = w0[k] + (int32_t)(q->len > 1 ? M64(w1[k] - w0[k], kh) / (q->len - 1) : 0);
        ln->hu = pv_dot30(wk, g_f.up);
        ln->ht = pv_dot30(wk, t);
        ln->horizon = kh * 256;
        ln->hid = 1;
    }
    ln->step = 0;
    if (g_f.D > g_f.rt) {
        /* above every hill: nothing before the lowest of the line's rays reaches the highest ground can show */
        int st, kl = g_f.nadir ? ln->k0 : ln->k1;
        for (k = 0; k < 3; k++)
            wl[k] = w0[k] + (int32_t)(q->len > 1 ? M64(w1[k] - w0[k], kl) / (q->len - 1) : 0);
        st = pv_enter(pv_dot30(wl, g_f.up), pv_dot30(wl, t));
        if (st < 0)
            return;
        ln->step = (int16_t)st;
    }
    g_f.nlines++;
}

/* the haze for the frame: by a sample's height, the sum's rate along the distance (2^24) */
static void pv_fog_frame(void)
{
    const PvFog *f = &g_pv.fog;
    int32_t ha = (int32_t)((g_f.D - g_f.R) / 4), k = (ha - f->h0) / f->hstep, fr, ga, b;
    if (ha < f->h0)
        k = 0, fr = 0;
    else if (k >= PV_FOG_GS - 1)
        k = PV_FOG_GS - 2, fr = f->hstep;
    else
        fr = ha - f->h0 - k * f->hstep;
    ga = f->g[k] + pv_div(M64(f->g[k + 1] - f->g[k], fr), f->hstep);
    for (b = 0; b < PV_FOG_HB; b++) {
        int32_t hb = f->h0 + b * f->hstep, dh = hb - ha, dg = f->g[b] - ga;
        if (dh < f->hstep / 2 && dh > -f->hstep / 2) {
            dh = f->hstep;              /* (level with the eye: the rate there) */
            dg = f->g[k + 1] - f->g[k];
        }
        g_f.fogm[b] = pv_div(M64(dg, f->k), M64(dh, 256));
    }
    g_f.fog_rcp = pv_div(65536, f->hstep);
}

static int pv_fog_at(int32_t dist, int32_t h)
{
    int32_t b = (int32_t)(M64(h - g_pv.fog.h0, g_f.fog_rcp) >> 16), k;
    if (b < 0) b = 0;
    if (b > PV_FOG_HB - 1) b = PV_FOG_HB - 1;
    k = (int32_t)(M64(dist, g_f.fogm[b]) >> 24);
    if (k < 0) k = 0;
    if (k > PV_FOG_TAUS - 1) k = PV_FOG_TAUS - 1;
    return g_pv.fog.lev[k];
}

/* the sky's glow for the frame: the table's two rows round the eye's height, weighed */
static void pv_glow_frame(void)
{
    const PvGlow *gl = &g_pv.glow;
    int32_t a = g_f.D - g_f.R, j, fr, k, any = 0;
    if (a < 0)
        a = 0;
    /* (rows at amax (j / 63)^2: j = 63 sqrt(a / amax), in 256ths) */
    j = (int32_t)pv_isqrt((unsigned long long)pv_div(M64(a, 63 * 63 * 65536LL), gl->amax));
    if (j > 63 * 256)
        j = 63 * 256;
    fr = j & 255;
    j >>= 8;
    for (k = 0; k < PV_GLOW_CZ; k++) {
        int32_t g0 = gl->g[j][k], g1 = j < PV_GLOW_AS - 1 ? gl->g[j + 1][k] : g0;
        g_f.glow[k] = (uint8_t)(g0 + (((g1 - g0) * fr + 128) >> 8));
        any |= g_f.glow[k] >= 4;
    }
    g_f.glow_any = any;
    g_f.skf = (int32_t)((M64(g_f.up[0], g_f.cf[0]) + M64(g_f.up[1], g_f.cf[1]) + M64(g_f.up[2], g_f.cf[2])) >> 28);
    g_f.skr = (int32_t)((M64(g_f.up[0], g_f.cr[0]) + M64(g_f.up[1], g_f.cr[1]) + M64(g_f.up[2], g_f.cr[2])) >> 28);
    g_f.sku = (int32_t)((M64(g_f.up[0], g_f.cu[0]) + M64(g_f.up[1], g_f.cu[1]) + M64(g_f.up[2], g_f.cu[2])) >> 28);
}

/* the glow's table index for a picture pixel's ray (its cosine from straight up) */
static int pv_glow_index(int x, int y)
{
    int32_t dx = pv_div(M64(x * 256 - g_f.lcx, 65536), g_f.lf), dy = pv_div(M64(y * 256 - g_f.lcy, 65536), g_f.lf);
    int32_t num = g_f.skf + (int32_t)(M64(dx, g_f.skr) >> 16) - (int32_t)(M64(dy, g_f.sku) >> 16);
    int32_t den = (int32_t)pv_isqrt((unsigned long long)((1LL << 32) + M64(dx, dx) + M64(dy, dy)));
    int32_t cz = pv_div(M64(num, 65536), den), i = (int32_t)(M64(cz + 65536, PV_GLOW_CZ) >> 17);
    return i < 0 ? 0 : i > PV_GLOW_CZ - 1 ? PV_GLOW_CZ - 1 : i;
}

/* the picture's empty pixels glowing where the air between the eye and space is thick enough: every 4th pixel's ray
 * worked out, those between weighed */
static void pv_glow_pixels(void)
{
    int x, y;
    if (!g_f.glow_any)
        return;
    for (y = 0; y < PV_LH; y++) {
        uint16_t *row = g_f.pic + y * PV_LW;
        int i0 = pv_glow_index(0, y), i1;
        for (x = 0; x < PV_LW; x += 4) {
            int k;
            i1 = pv_glow_index(x + 4, y);
            for (k = 0; k < 4; k++)
                if (row[x + k] == PV_EMPTY) {
                    int g = g_f.glow[i0 + (((i1 - i0) * k) >> 2)];
                    if (g >= 4)
                        row[x + k] = (uint16_t)(PV_GLOW | ((g + 4) >> 3));
                }
            i0 = i1;
        }
    }
}

/* a line's pixels i0 .. i1 still empty filled with the ground between two samples: the light and the haze weighed
 * from a (pixel ra, 256ths) to b (tr_fill_ground) */
static void pv_fill(const PvLine *ln, int i0, int i1, int32_t ra, int32_t rb, int32_t la, int32_t lb, int32_t fa,
                    int32_t fb, int mat)
{
    int32_t span = rb - ra, t0, dt, l, dl, f, df, col;
    int i, row, base = mat * PV_LIGHTS;
    if (span == 0) {
        t0 = 65536;
        dt = 0;
    } else {
        t0 = pv_div(M64(i0 * 256 - ra, 65536), span);
        dt = pv_div(256LL * 65536, span);
        if (t0 < 0) t0 = 0;
        if (t0 > 65536) t0 = 65536;
        if (M64(dt, i1 - i0) + t0 > 65536 + 7 || M64(dt, i1 - i0) + t0 < -7)
            dt = i1 > i0 ? (65536 - t0) / (i1 - i0) : 0;
    }
    l = la * 256 + (int32_t)(M64(lb - la, t0) >> 8) + 32768;
    dl = (int32_t)(M64(lb - la, dt) >> 8);
    f = fa * 256 + (int32_t)(M64(fb - fa, t0) >> 8) + 32768;
    df = (int32_t)(M64(fb - fa, dt) >> 8);
    row = ln->row + i0 * ln->drow;
    col = ln->col + i0 * ln->dcol;
    for (i = i0; i <= i1; i++, row += ln->drow, col += ln->dcol, l += dl, f += df) {
        int x = ln->vert ? col >> 16 : row, y = ln->vert ? row : col >> 16;
        uint16_t *p;
        if ((unsigned)x >= PV_LW || (unsigned)y >= PV_LH)
            continue;
        p = &g_f.pic[y * PV_LW + x];
        if (*p == PV_EMPTY) {
            int lv = l >> 16, fv = f >> 16;
            if (lv < 0) lv = 0;
            if (lv > PV_LIGHTS - 1) lv = PV_LIGHTS - 1;
            if (fv < 0) fv = 0;
            if (fv > PV_FOGS - 1) fv = PV_FOGS - 1;
            *p = (uint16_t)((base + lv) | fv << 8);
        }
    }
}

/* the pixels between the horizon and a sample's (both 256ths), as tr_span, within the line's wanted ones: 0 when none */
static int pv_span(const PvLine *ln, int32_t rho, int *i0, int *i1)
{
    if (g_f.nadir) {
        *i0 = (ln->horizon >> 8) + 1;
        *i1 = rho >> 8;
    } else {
        *i0 = -((-rho) >> 8);
        *i1 = -((-ln->horizon) >> 8) - 1;
    }
    if (*i0 < ln->k0) *i0 = ln->k0;
    if (*i1 > ln->k1) *i1 = ln->k1;
    return *i0 <= *i1;
}

/* a point (a, b) from the eye, along up and t, is above the horizon's ray: seen */
static int pv_above(const PvLine *ln, int32_t a, int32_t b)
{
    return M64(a, ln->ht) - M64(b, ln->hu) > 0;
}

/* the point at a step at height h (BK_HU) from the eye */
static void pv_place(const PvStep *st, int32_t h, int32_t *a, int32_t *b)
{
    *a = st->a + (int32_t)(M64(h, st->c) >> (28 + g_f.vsh));
    *b = st->b + (int32_t)(M64(h, st->s) >> (28 + g_f.vsh));
}

/* is the line over: the horizon's ray has left the top sphere behind it (or misses it) */
static int pv_line_over(const PvLine *ln, const PvStep *st)
{
    i64 nn = pv_isqrt((unsigned long long)(M64(ln->hu, ln->hu) + M64(ln->ht, ln->ht))), b, disc, s1, x, y;
    if (!nn)
        return 1;
    b = M64(g_f.D, ln->hu) / nn;
    disc = b * b - (M64(g_f.D, g_f.D) - M64(g_f.rt, g_f.rt));
    if (disc <= 0)
        return 1;
    s1 = -b + (i64)pv_isqrt((unsigned long long)disc);
    if (s1 <= 0)
        return 1;
    x = g_f.D + s1 * ln->hu / nn;
    y = s1 * ln->ht / nn;
    return (i64)st->s * x - (i64)st->c * y > 0;
}

/* a point's pixel on the line, 256ths: 0 when it is behind the eye */
static int pv_rho(const PvLine *ln, int32_t ra, int32_t rb, int32_t *rho)
{
    int32_t z = ra * ln->uf + rb * ln->tf, num;
    if (z <= 0)
        return 0;
    num = ra * ln->us + rb * ln->ts;
    if ((num < 0 ? -num : num) >> 16 >= z)
        *rho = (num < 0) == (ln->rb_ < 0) ? 1L << 30 : -(1L << 30);
    else
        *rho = pv_div(M64(num, ln->rb_), z);
    *rho += ln->ra_;
    return 1;
}

/* a sample at the line's march: seen (filling the pixels it is the nearest ground for) or not; and the slab over it
 * (slab: lo .. hi) drawn where nothing nearer is.  h its height, light and haze 256ths */
static void pv_sample(PvLine *ln, const PvStep *st, int32_t h, int mat, int32_t light, int slab, int32_t lo, int32_t hi)
{
    int32_t ra, rb, rho;
    int i0, i1;
    g_f.samples++;
    pv_place(st, h, &ra, &rb);
    if (!pv_above(ln, ra, rb) || !pv_rho(ln, ra, rb, &rho)) {
        ln->hid = 1;
        ln->pok = 0;
    } else {
        int32_t fog = (int32_t)pv_fog_at(st->dist, h) << 8;
        g_f.seen++;
        ln->hid = 0;
        if (pv_span(ln, rho, &i0, &i1)) {
            g_f.npx += i1 - i0 + 1;
            if (ln->pok && ln->mat == mat)
                pv_fill(ln, i0, i1, ln->prho, rho, ln->plight, light, ln->pfog, fog, mat);
            else if (ln->pok && (h - ln->ph) * 4 < ln->pds / 2 && (ln->ph - h) * 4 < ln->pds / 2) {
                /* another material on gentle ground: the pixels nearer the last sample its, the rest this one's */
                int32_t mid = (ln->prho + rho) / 2;
                int m0, m1;
                if (pv_span(ln, mid, &m0, &m1))
                    pv_fill(ln, m0, m1, ln->prho, ln->prho, ln->plight, ln->plight, ln->pfog, ln->pfog, ln->mat);
                pv_fill(ln, i0, i1, rho, rho, light, light, fog, fog, mat);
            } else
                pv_fill(ln, i0, i1, rho, rho, light, light, fog, fog, mat);
        }
        ln->pfog = fog;
        ln->horizon = rho;
        ln->hu = ra;
        ln->ht = rb;
        if (g_f.nadir ? ln->horizon >= ln->k1 * 256 : ln->horizon <= ln->k0 * 256)
            ln->done = 1;
        ln->pok = 1;
        ln->prho = rho;
        ln->plight = light;
        ln->ph = h;
        ln->mat = (uint8_t)mat;
        ln->pds = st->ds;
    }
    /* the slab: the rock between its underside and top, in front of whatever is still to come */
    if (slab) {
        int32_t la, lb, ha, hb, r0, r1, s0, s1;
        pv_place(st, lo, &la, &lb);
        pv_place(st, hi, &ha, &hb);
        if (pv_rho(ln, la, lb, &r0) && pv_rho(ln, ha, hb, &r1)) {
            int32_t alt = (g_f.D - g_f.R) / 4, sm, sl, sf;
            s0 = r0 < r1 ? r0 : r1;
            s1 = r0 < r1 ? r1 : r0;
            if (ln->pslab) {
                if (ln->plo < s0) s0 = ln->plo;
                if (ln->phi > s1) s1 = ln->phi;
            }
            sm = alt < lo ? 3 : 1;      /* (terrain.c's TR_UNDER, TR_ROCK) */
            if (alt > hi)
                sl = light >> 8;
            else {
                int32_t gap = lo > h ? (lo - h) * 16 / g_pv.slabs.under_div : 0;
                sl = g_pv.slabs.under[gap > 63 ? 63 : gap];
            }
            sf = pv_fog_at(st->dist, hi);
            i0 = -((-s0) >> 8);
            i1 = s1 >> 8;
            if (i0 < ln->k0) i0 = ln->k0;
            if (i1 > ln->k1) i1 = ln->k1;
            g_f.slabs++;
#ifdef PV_DEBUG
            if (getenv("SLABDBG") && g_f.slabs % 500 == 1)
                printf("   slab: alt %d lo %d hi %d h %d -> mat %d light %d fog %d dist %d km\n", alt, lo, hi, h, sm, sl, sf, st->dist / 16);
#endif
            if (i0 <= i1) {
                g_f.slabpx += i1 - i0 + 1;
                pv_fill(ln, i0, i1, 0, 0, sl << 8, sl << 8, sf << 8, sf << 8, sm);
            }
            ln->plo = r0 < r1 ? r0 : r1;
            ln->phi = r0 < r1 ? r1 : r0;
            ln->pslab = 1;
            if (g_f.nadir ? s1 > ln->horizon : s0 < ln->horizon)
                ln->hid = 0;            /* (some of it is past the filled part: it may hide what follows) */
        } else
            ln->pslab = 0;
    } else
        ln->pslab = 0;
}

#ifdef PV_DEBUG
void (*pv_dbg_ground)(const double *d, int32_t *h, int *c, int32_t *light);   /* (the host's checks: terrain.c's own ground) */
#endif

/* the slab over a plane point, its underside and top weighed between the four cells round it (a cell without one
 * counting as the slab pinched to nothing at the others' middle: terrain.c's tr_sample_patch): 1 when there is one */
static int pv_slab_at(int32_t X, int32_t Y, int32_t *lo, int32_t *hi)
{
    int k;
    for (k = 0; k < g_pv.slabs.n; k++) {
        const PvSlab *sl = &g_pv.slabs.s[k];
        int ssh = g_pv.slabs.shift;
        int32_t sx = (X >> ssh) - sl->x0, sy = (Y >> ssh) - sl->y0, fx, fy, w[4], mid = 0, a = 0, b = 0;
        const int16_t *c;
        int q, n = 0, off[4];
        if ((uint32_t)sx >= (uint32_t)sl->w - 1 || (uint32_t)sy >= (uint32_t)sl->h - 1)
            continue;
        c = g_pv.slab + 2 * (g_pv.slab_first[k] + sy * sl->w + sx);
        off[0] = 0;
        off[1] = 2;
        off[2] = 2 * sl->w;
        off[3] = 2 * sl->w + 2;
        for (q = 0; q < 4; q++)
            if (c[off[q]] < c[off[q] + 1]) {
                mid += c[off[q]] + c[off[q] + 1];
                n++;
            }
        if (!n)
            return 0;
        mid = mid * 128 / n;            /* (the middle, 256ths) */
        fx = (X >> (ssh - 8)) & 255;
        fy = (Y >> (ssh - 8)) & 255;
        w[0] = (256 - fx) * (256 - fy);
        w[1] = fx * (256 - fy);
        w[2] = (256 - fx) * fy;
        w[3] = fx * fy;
        for (q = 0; q < 4; q++) {
            int has = c[off[q]] < c[off[q] + 1];
            a += (int32_t)((M64(w[q], has ? c[off[q]] * 256 : mid)) >> 8);
            b += (int32_t)((M64(w[q], has ? c[off[q] + 1] * 256 : mid)) >> 8);
        }
        /* (a and b: 65536ths of a height unit) */
        if (b - a < 32768)
            return 0;
        *lo = a >> 16;
        *hi = b >> 16;
        return 1;
    }
    return 0;
}

/* a line's plane point at a step: m (65536ths), 0 when the step is past the plane's reach */
static int pv_m(const PvLine *ln, const PvStep *st, int32_t *m)
{
    int32_t den = st->uc + (int32_t)(M64(ln->te, st->s) >> 30);
    if (den < PV_DEN_MIN)
        return 0;
    *m = pv_div((i64)st->s << 16, den);
    return 1;
}

/* hidden ground skipped: the sample at step i is in block (bx, by) of the window (4 x 4 cells), whose ground is
 * nowhere over hb; while the line's samples stay in the block, and the block's highest is hidden at both ends of them
 * and the horizon's ray does not dip under it between (its nearest approach to the centre), none of them can be seen.
 * The step after them, or i when they cannot be skipped */
static int pv_skip(const PvLine *ln, int i, int l, int32_t ox, int32_t oy, int bx, int by, int32_t hb)
{
    int sh = g_pv.h.shift0 + l, j;
    int32_t a, b, m, mx = 0x7FFFFFFF, my = 0x7FFFFFFF;
    i64 e;
    pv_place(&g_f.st[i], hb, &a, &b);
    if (pv_above(ln, a, b))
        return i;
    /* where the line leaves the block (m) */
    if (ln->kx > 0)
        e = ((i64)(ox + 4 * bx + 4) << sh) - g_f.px, mx = (int32_t)(e * 65536 / ln->kx);
    else if (ln->kx < 0)
        e = ((i64)(ox + 4 * bx) << sh) - g_f.px - 1, mx = (int32_t)(e * 65536 / ln->kx);
    if (ln->ky > 0)
        e = ((i64)(oy + 4 * by + 4) << sh) - g_f.py, my = (int32_t)(e * 65536 / ln->ky);
    else if (ln->ky < 0)
        e = ((i64)(oy + 4 * by) << sh) - g_f.py - 1, my = (int32_t)(e * 65536 / ln->ky);
    if (my < mx)
        mx = my;
    for (j = i + 1; j < g_f.nsteps && g_f.st[j].lev <= l && pv_m(ln, &g_f.st[j], &m) && m < mx; j++)
        ;
    if (--j <= i)
        return i;
    pv_place(&g_f.st[j], hb, &a, &b);
    if (pv_above(ln, a, b))
        return i;
    /* the ray's nearest approach to the centre, if it lies between and the block's highest reaches over it */
    if (ln->ht > 0 && M64(g_f.st[i].c, ln->hu) + M64(g_f.st[i].s, ln->ht) <= 0
        && M64(g_f.st[j].c, ln->hu) + M64(g_f.st[j].s, ln->ht) >= 0) {
        i64 hl = pv_isqrt((unsigned long long)(M64(ln->hu, ln->hu) + M64(ln->ht, ln->ht)));
        if (hl && M64(g_f.D, ln->ht) / hl <= g_f.R + hb * 4)
            return i;
    }
    g_f.skips += j + 1 - i;
    return j + 1;
}

/* the ground at a plane point from a level's window: its height (BK_HU), colour id and light (256ths): 0 when the window
 * has no ground there */
static int pv_ground(const uint8_t *w, int sh, int32_t ox, int32_t oy, int32_t X, int32_t Y, int32_t *h, int *c0,
                     int32_t *light)
{
    int32_t ix = (X >> sh) - ox, iy = (Y >> sh) - oy, fx, fy, h0, h1;
    const uint8_t *c00, *c10, *c01, *c11, *cq;
    int diff;
    if ((uint32_t)ix >= PV_W - 1 || (uint32_t)iy >= PV_W - 1)
        return 0;
    c00 = w + (iy * PV_W + ix) * PV_CELL;
    if (c00[1] == PV_ABSENT)
        return 0;
    c10 = c00 + PV_CELL;
    c01 = c00 + PV_W * PV_CELL;
    c11 = c01 + PV_CELL;
    fx = (X >> (sh - 8)) & 255;
    fy = (Y >> (sh - 8)) & 255;
    {
        int32_t a = (int16_t)(c00[0] | c00[1] << 8), b = (int16_t)(c10[0] | c10[1] << 8);
        int32_t c = (int16_t)(c01[0] | c01[1] << 8), d = (int16_t)(c11[0] | c11[1] << 8);
        h0 = a + (((b - a) * fx) >> 8);
        h1 = c + (((d - c) * fx) >> 8);
        *h = h0 + (((h1 - h0) * fy) >> 8);
    }
    cq = (fy >= 128 ? c01 : c00) + (fx >= 128 ? PV_CELL : 0);
    *c0 = cq[2];
    diff = (c00[2] ^ *c0) | (c10[2] ^ *c0) | (c01[2] ^ *c0) | (c11[2] ^ *c0);
    if (diff < PV_LIGHTS) {
        int32_t l0 = (c00[2] & 31) * (256 - fx) + (c10[2] & 31) * fx;
        int32_t l1 = (c01[2] & 31) * (256 - fx) + (c11[2] & 31) * fx;
        *light = (l0 * (256 - fy) + l1 * fy) >> 8;
    } else
        *light = (*c0 & 31) << 8;
    return 1;
}

/* a plane point within one of the slab grids */
static int pv_in_slabs(int32_t X, int32_t Y)
{
    int k;
    for (k = 0; k < g_pv.slabs.n; k++) {
        const PvSlab *sl = &g_pv.slabs.s[k];
        if ((uint32_t)((X >> g_pv.slabs.shift) - sl->x0) < sl->w && (uint32_t)((Y >> g_pv.slabs.shift) - sl->y0) < sl->h)
            return 1;
    }
    return 0;
}

/* every line marched through level l (the window round the eye's point), each until it leaves the level */
static void pv_march(int l)
{
    int sh = g_pv.h.shift0 + l, n;
    int32_t ox = ((g_f.px >> sh) - PV_W / 2 + PV_SNAP / 2) & ~(int32_t)(PV_SNAP - 1);
    int32_t oy = ((g_f.py >> sh) - PV_W / 2 + PV_SNAP / 2) & ~(int32_t)(PV_SNAP - 1);
    const uint8_t *w = pv_window(l, ox, oy);
    const int16_t *bw = g_pv.bwin[l];
    for (n = 0; n < g_f.nlines; n++) {
        PvLine *ln = &g_f.ln[n];
        if (!ln->done && ln->step < g_f.nsteps)
            g_f.levpass++;
        while (!ln->done && ln->step < g_f.nsteps) {
            const PvStep *st = &g_f.st[ln->step];
            int32_t m, X, Y, ix, iy, h, light, lo, hi;
            int c0, inr, sb;
            if (st->lev > l || !pv_m(ln, st, &m))
                break;
            X = g_f.px + (int32_t)(M64(ln->kx, m) >> 16);
            Y = g_f.py + (int32_t)(M64(ln->ky, m) >> 16);
            ix = (X >> sh) - ox;
            iy = (Y >> sh) - oy;
            if ((uint32_t)ix >= PV_W - 1 || (uint32_t)iy >= PV_W - 1 || w[(iy * PV_W + ix) * PV_CELL + 1] == PV_ABSENT)
                break;
            if (ln->hid) {
                int32_t hb = bw[(iy >> 2) * PV_BW + (ix >> 2)];
                int nx = pv_skip(ln, ln->step, l, ox, oy, ix >> 2, iy >> 2, hb);
                if (nx != ln->step) {
                    ln->step = (int16_t)nx;
                    ln->pok = 0;
                    ln->prect = 0;
                    if (!ln->done && pv_line_over(ln, &g_f.st[ln->step - 1]))
                        ln->done = 1;
                    continue;
                }
            }
            /* crossing a slab's grid: samples between the last step and this one, half a slab cell apart (the low
             * picture's steps far off are longer than an arch is thick) */
            inr = pv_in_slabs(X, Y);
            if ((inr || ln->prect) && ln->step > 0) {
                const PvStep *a0 = &g_f.st[ln->step - 1];
                int32_t dx = X - ln->pX, dy = Y - ln->pY, ns;
                int j;
                ns = ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) >> (g_pv.slabs.shift - 1);
                if (ns > PV_SUBS)
                    ns = PV_SUBS;
                for (j = 1; j < ns && !ln->done; j++) {
                    PvStep t;
                    int32_t mj, Xj, Yj;
                    t.c = a0->c + (int32_t)(M64(st->c - a0->c, j) / ns);
                    t.s = a0->s + (int32_t)(M64(st->s - a0->s, j) / ns);
                    t.a = a0->a + (int32_t)(M64(st->a - a0->a, j) / ns);
                    t.b = a0->b + (int32_t)(M64(st->b - a0->b, j) / ns);
                    t.uc = a0->uc + (int32_t)(M64(st->uc - a0->uc, j) / ns);
                    t.dist = a0->dist + (int32_t)(M64(st->dist - a0->dist, j) / ns);
                    t.ds = a0->ds / ns;
                    t.lev = st->lev;
                    if (!pv_m(ln, &t, &mj))
                        break;
                    Xj = g_f.px + (int32_t)(M64(ln->kx, mj) >> 16);
                    Yj = g_f.py + (int32_t)(M64(ln->ky, mj) >> 16);
                    if (!pv_ground(w, sh, ox, oy, Xj, Yj, &h, &c0, &light))
                        break;
                    sb = pv_slab_at(Xj, Yj, &lo, &hi);
                    g_f.subs++;
                    pv_sample(ln, &t, h, c0 >> 5, light, sb, lo, hi);
                }
                if (ln->done)
                    break;
            }
            ln->prect = (uint8_t)inr;
            ln->pX = X;
            ln->pY = Y;
            (void)pv_ground(w, sh, ox, oy, X, Y, &h, &c0, &light);
#ifdef PV_DEBUG
            if (pv_dbg_ground) {
                double d[3];
                int k2, cc;
                for (k2 = 0; k2 < 3; k2++)
                    d[k2] = (g_f.up[k2] * (double)st->c + g_f.t[n][k2] * (double)st->s) / 1073741824.0 / 1073741824.0;
                pv_dbg_ground(d, &h, &cc, &light);
                c0 = cc;
            }
#endif
            lo = hi = 0;
            sb = inr && pv_slab_at(X, Y, &lo, &hi);
            g_f.slabq += sb;
            pv_sample(ln, st, h, c0 >> 5, light, sb, lo, hi);
            ln->step++;
            if ((ln->step & 15) == 0 && !ln->done && pv_line_over(ln, st))
                ln->done = 1;
        }
    }
}

/* the lines past every level: the plain sphere */
static void pv_march_far(void)
{
    int n, c = g_pv.h.far_c;
    for (n = 0; n < g_f.nlines; n++) {
        PvLine *ln = &g_f.ln[n];
        while (!ln->done && ln->step < g_f.nsteps) {
            const PvStep *st = &g_f.st[ln->step];
            g_f.far++;
            pv_sample(ln, st, 0, c >> 5, (int32_t)(c & 31) << 8, 0, 0, 0);
            ln->step++;
            if ((ln->step & 15) == 0 && !ln->done && pv_line_over(ln, st))
                ln->done = 1;
        }
    }
}

void pv_frame(const PvCam *cam)
{
    int32_t e[3], nx, ny, nz, maxd;
    int k, i;
    memset(g_f.pic, 0xFF, sizeof g_f.pic);
    g_f.samples = g_f.seen = g_f.npx = g_f.far = g_f.levpass = g_f.skips = g_f.slabs = g_f.slabpx = g_f.slabq = g_f.subs = 0;
    g_f.nlines = 0;
    g_f.nsteps = 0;
    if (!g_pv.on)
        return;
    g_f.R = g_pv.h.radius;
    for (k = 0; k < 3; k++) {
        e[k] = cam->eye[k] / PV_PU;
        g_f.cr[k] = cam->m[k];
        g_f.cu[k] = cam->m[3 + k];
        g_f.cf[k] = cam->m[6 + k];
    }
    g_f.D = (int32_t)pv_isqrt((unsigned long long)(M64(e[0], e[0]) + M64(e[1], e[1]) + M64(e[2], e[2])));
    if (g_f.D <= 0)
        return;
    for (k = 0; k < 3; k++)
        g_f.up[k] = pv_div(M64(e[k], 1L << 30), g_f.D);
    g_f.rt = g_f.R + g_pv.h.top * 4 + 16;
    g_f.rb = g_f.R + g_pv.h.bottom * 4 - 16;
    g_f.ue = pv_dot30(g_f.up, (const int32_t *)g_pv.h.e);
    g_f.ua = pv_dot30(g_f.up, (const int32_t *)g_pv.h.a);
    g_f.ub = pv_dot30(g_f.up, (const int32_t *)g_pv.h.b);
    g_f.levels = g_f.ue > (1L << 30) / 8;
    if (g_f.levels) {
        g_f.px = pv_div(M64(g_f.R, g_f.ua), g_f.ue);
        g_f.py = pv_div(M64(g_f.R, g_f.ub), g_f.ue);
    }
    /* V: straight down's point on the picture (straight up's when it is behind) */
    nx = -(int32_t)((M64(g_f.up[0], g_f.cr[0]) + M64(g_f.up[1], g_f.cr[1]) + M64(g_f.up[2], g_f.cr[2])) >> 30);
    ny = -(int32_t)((M64(g_f.up[0], g_f.cu[0]) + M64(g_f.up[1], g_f.cu[1]) + M64(g_f.up[2], g_f.cu[2])) >> 30);
    nz = -(int32_t)((M64(g_f.up[0], g_f.cf[0]) + M64(g_f.up[1], g_f.cf[1]) + M64(g_f.up[2], g_f.cf[2])) >> 30);
    if (nz < 1 && nz > -1)
        nz = nz < 0 ? -1 : 1;
    g_f.nadir = nz > 0;
    g_f.lcx = pv_div(M64(cam->cx - 2, 256), PV_BLOCK);
    g_f.lcy = pv_div(M64(cam->cy - 2, 256), PV_BLOCK);
    g_f.lf = pv_div(M64(cam->focal, 256), PV_BLOCK);
    g_f.bx0 = cam->x0 / PV_BLOCK;
    g_f.by0 = cam->y0 / PV_BLOCK;
    g_f.bx1 = (cam->x1 + PV_BLOCK - 1) / PV_BLOCK;
    g_f.by1 = (cam->y1 + PV_BLOCK - 1) / PV_BLOCK;
    if (g_f.bx0 < 0) g_f.bx0 = 0;
    if (g_f.by0 < 0) g_f.by0 = 0;
    if (g_f.bx1 > PV_LW) g_f.bx1 = PV_LW;
    if (g_f.by1 > PV_LH) g_f.by1 = PV_LH;
    {
        i64 vx = g_f.lcx + M64(g_f.lf, nx) / nz, vy = g_f.lcy - M64(g_f.lf, ny) / nz;
        if (vx > (1L << 28)) vx = 1L << 28;
        if (vx < -(1L << 28)) vx = -(1L << 28);
        if (vy > (1L << 28)) vy = 1L << 28;
        if (vy < -(1L << 28)) vy = -(1L << 28);
        g_f.vx = (int32_t)vx;
        g_f.vy = (int32_t)vy;
    }
    /* the vertical numbers' scale: the farthest ground seen within 2^20 */
    maxd = (g_f.D > g_f.rb ? (int32_t)pv_isqrt((unsigned long long)(M64(g_f.D, g_f.D) - M64(g_f.rb, g_f.rb))) : 0)
        + (int32_t)pv_isqrt((unsigned long long)(M64(g_f.rt, g_f.rt) - M64(g_f.rb, g_f.rb))) + 65536;
    for (g_f.vsh = 0; (maxd >> g_f.vsh) > (1L << 20); g_f.vsh++)
        ;
    pv_steps();
    pv_fog_frame();
    pv_glow_frame();
    for (k = 0; k < 4; k++) {
        pv_quarter(k);
        if (g_f.q[k].on)
            for (i = 0; i < g_f.q[k].lines; i++)
                pv_line(&g_f.q[k], i);
    }
    if (g_f.levels)
        for (k = 0; k < g_pv.h.nlev; k++)
            pv_march(k);
    pv_march_far();
    pv_glow_pixels();
}

/* the host's picture drawn on a screen: each pixel a 5 x 5 block of its two colours (blocks' rows 0, 2, 4 by the first
 * row of thresholds, 1 and 3 the second) */
static const uint8_t pv_th[2][PV_BLOCK] = { { 0, 6, 2, 8, 4 }, { 5, 1, 7, 3, 9 } };

void pv_blit_host(uint8_t *fb, int stride, int y0, int y1)
{
    int y, x;
    for (y = y0; y < y1; y++) {
        const uint16_t *src = g_f.pic + (y / PV_BLOCK) * PV_LW;
        const uint8_t *th = pv_th[(y % PV_BLOCK) & 1];
        uint8_t *row = fb + (size_t)y * stride;
        if (y / PV_BLOCK >= PV_LH)
            break;
        for (x = 0; x < PV_LW * PV_BLOCK; x++) {
            uint16_t v = src[x / PV_BLOCK];
            const PvDither *d;
            uint8_t c;
            if (v == PV_EMPTY)
                continue;
            if (v >= PV_GLOW) {         /* (black: the sky as it was shows through) */
                d = &g_pv.glow.sky[v & 31];
                c = th[x % PV_BLOCK] < d->t ? d->b : d->a;
                if (c)
                    row[x] = c;
                continue;
            }
            d = &g_pv.dither[v];
            row[x] = th[x % PV_BLOCK] < d->t ? d->b : d->a;
        }
    }
}
#ifdef PV_DEBUG
/* --- the host's checks (tools/pvshot.c) --- */
int pv_fog_dbg(int32_t dist, int32_t h) { return pv_fog_at(dist, h); }
/* (debug: line n's sample at step i: its plane point by the march's sums, and its direction) */
int pv_dbg_line(int n, int i, int32_t *X, int32_t *Y, double *dir)
{
    const PvLine *ln = &g_f.ln[n];
    const PvStep *st = &g_f.st[i];
    int32_t den = st->uc + (int32_t)(M64(ln->te, st->s) >> 30), m;
    int k;
    if (n >= g_f.nlines || i >= g_f.nsteps || den < PV_DEN_MIN)
        return 0;
    m = pv_div((i64)st->s << 16, den);
    *X = g_f.px + (int32_t)(M64(ln->kx, m) >> 16);
    *Y = g_f.py + (int32_t)(M64(ln->ky, m) >> 16);
    for (k = 0; k < 3; k++)
        dir[k] = (g_f.up[k] * (double)st->c + g_f.t[n][k] * (double)st->s) / 1073741824.0 / 1073741824.0;
    return 1;
}
void pv_dbg_lines(void)
{
    int n;
    printf("   frame: D %d R %d rt %d vsh %d nsteps %d nadir %d V (%d, %d)/256, last step c %d\n", g_f.D, g_f.R, g_f.rt, g_f.vsh,
           g_f.nsteps, g_f.nadir, g_f.vx, g_f.vy, g_f.st[g_f.nsteps - 1].c);
    for (n = 0; n < g_f.nlines; n += 23) {
        const PvLine *ln = &g_f.ln[n];
        printf("   line %3d: vert %d row %d drow %d col %d dcol %d len %d kmin %d: step %d done %d horizon %d\n", n, ln->vert, ln->row,
               ln->drow, ln->col >> 16, ln->dcol, ln->len, ln->kmin, ln->step, ln->done, ln->horizon);
    }
}

void pv_dbg_counts(void)
{
    printf("   %u samples (%u seen, %u far), %u skipped, %u pixels filled, %d lines, %d steps, %u line-level passes\n", g_f.samples,
           g_f.seen, g_f.far, g_f.skips, g_f.npx, g_f.nlines, g_f.nsteps, g_f.levpass);
    printf("   slabs: %u samples over one, %u drawn, %u pixels; %u sub-samples\n", g_f.slabq, g_f.slabs, g_f.slabpx, g_f.subs);
}
#endif
