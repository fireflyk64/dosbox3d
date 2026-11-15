/* planetbake.c: the planet missions' ground baked (src/bake.h): each of RVGAME.DAT's planet missions made with
 * terrain.c, as the card makes it, and written as ID.PLN: the grid BK_LEVEL halvings down, its halvings, the mission's
 * patches.  Then the file is read back through bake.c and asked what the game will ask: inside a patch its answers
 * must be terrain.c's own to the position; outside, how far they are from terrain.c's finest ground is printed.
 *
 *   planetbake RVGAME.DAT OUTDIR --palette FILE [--stamp N] [--force] [--only ID] [--quiet]
 *
 * A file whose key (its mission's records) and stamp (build.sh's: the sources' sum) are these is left as it is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "terrain.c"
#include "rvdata.h"
#include "bake.h"
#include "pview.h"

static uint8_t *g_file;
static uint32_t g_size;
static uint8_t g_pal[768];              /* the flight's palette, 6 bits (--palette; the picture uses its 16 .. 207) */

/* (bake.c reads the file through this: here it is in memory, handle and base 0) */
int xms_get_at(unsigned handle, unsigned long at, void *dst, unsigned long n)
{
    (void)handle;
    if (at + n > g_size) {
        fprintf(stderr, "planetbake: a read past the file's end (%lu + %lu of %u)\n", at, n, g_size);
        exit(1);
    }
    memcpy(dst, g_file + at, n);
    return 1;
}

static void put(FILE *f, const void *p, size_t n)
{
    if (fwrite(p, 1, n, f) != n) {
        perror("planetbake: write");
        exit(1);
    }
}

static void put_grid(FILE *f, const TrPlanet *p, int level)
{
    int face, side = tr_side(level);
    size_t k, n = (size_t)side * side;
    uint8_t *row = (uint8_t *)malloc(n * BK_CELL);
    for (face = 0; face < 6; face++) {
        const TrCell *g = p->g[level][face];
        for (k = 0; k < n; k++) {
            row[k * BK_CELL] = (uint8_t)(g[k].h & 0xFF);
            row[k * BK_CELL + 1] = (uint8_t)((uint16_t)g[k].h >> 8);
            row[k * BK_CELL + 2] = g[k].c;
        }
        put(f, row, n * BK_CELL);
    }
    free(row);
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* the file asked as the game asks it, against terrain.c: 0 when every answer in a patch is terrain.c's */
static int check(const TrPlanet *p, const char *id, int quiet)
{
    enum { NS = 40000 };
    static int32_t diff[NS];
    int k, bad = 0, nd = 0, in_patch = 0, slabs_out = 0, f, i, j;
    long asked = 0;
    if (bk_begin(0, 0) < 0) {
        fprintf(stderr, "planetbake: %s: the file is no bake\n", id);
        return 1;
    }
    /* each patch's points and places between them, at the ground, 300 km over it and half a radius out */
    for (k = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        if (!pt->h)
            continue;
        for (j = 0; j <= pt->nb; j += 1 + (int)(rnd() % 5))
            for (i = 0; i <= pt->na; i += 1 + (int)(rnd() % 5)) {
                int64_t v[3];
                int alt;
                tr_patch_point(p, pt, i, j, v);
                for (alt = 0; alt < 3; alt++) {
                    static const double scale[3] = { 1.0, 1.005, 1.5 };
                    int32_t q[3];
                    TrColumn a;
                    BkColumn b;
                    q[0] = (int32_t)(v[0] * scale[alt]) + (int32_t)(rnd() % 4096) - 2048;
                    q[1] = (int32_t)(v[1] * scale[alt]) + (int32_t)(rnd() % 4096) - 2048;
                    q[2] = (int32_t)(v[2] * scale[alt]) + (int32_t)(rnd() % 4096) - 2048;
                    tr_column(p, q[0], q[1], q[2], &a);
                    bk_column(q[0], q[1], q[2], &b);
                    asked++;
                    /* (terrain.c's answer is a patch's when a patch holds the point: then the two must be one) */
                    {
                        int held = 0, n;
                        int64_t gx, gy;
                        for (n = 0; n < p->npatch && !held; n++)
                            held = p->patch[n].h && tr_patch_find(p, &p->patch[n], q[0], q[1], q[2], &gx, &gy);
                        if (!held)
                            continue;
                    }
                    if (a.ground != b.ground || a.slab_lo != b.slab_lo || a.slab_hi != b.slab_hi || a.r != b.r) {
                        if (bad++ < 5)
                            fprintf(stderr, "planetbake: %s: patch %d point (%d, %d) x %.3f: terrain.c ground %d slab %d..%d r %d, "
                                    "the bake %d, %d..%d, %d\n", id, k, i, j, scale[alt], a.ground, a.slab_lo, a.slab_hi, a.r,
                                    b.ground, b.slab_lo, b.slab_hi, b.r);
                    }
                }
            }
    }
    /* places all over the planet: outside the patches, how far the bake's ground is from terrain.c's */
    for (k = 0; k < NS; k++) {
        double d[3], len;
        int32_t q[3];
        int held = 0, n;
        int64_t gx, gy;
        TrColumn a;
        BkColumn b;
        do {
            d[0] = (double)(rnd() % 20001) - 10000;
            d[1] = (double)(rnd() % 20001) - 10000;
            d[2] = (double)(rnd() % 20001) - 10000;
            len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        } while (len < 100 || len > 10000);
        for (n = 0; n < 3; n++)
            q[n] = (int32_t)(d[n] / len * p->radius);
        for (n = 0; n < p->npatch && !held; n++)
            held = p->patch[n].h && tr_patch_find(p, &p->patch[n], q[0], q[1], q[2], &gx, &gy);
        tr_column(p, q[0], q[1], q[2], &a);
        bk_column(q[0], q[1], q[2], &b);
        if (held) {
            in_patch++;
            if (a.ground != b.ground || a.slab_lo != b.slab_lo || a.slab_hi != b.slab_hi)
                bad++;
            continue;
        }
        if (b.slab_lo || b.slab_hi)
            bad++;
        diff[nd++] = abs(a.ground - b.ground);
    }
    /* a slab of the planet's own grid outside every patch would be lost: there must be none */
    for (f = 0; f < 6; f++)
        for (j = 0; j < TR_N; j++)
            for (i = 0; i < TR_N; i++) {
                int lo, hi, n, held = 0;
                int64_t v[3], gx, gy;
                if (!p->tiles[f][(j / TR_TILE) * TR_TILES + i / TR_TILE]) {
                    i |= TR_TILE - 1;
                    continue;
                }
                if (!tr_slab(p, f, i, j, &lo, &hi))
                    continue;
                tr_cell_vec(f, i, j, TR_N, v);
                for (n = 0; n < 3; n++)
                    v[n] *= 4096;
                for (n = 0; n < p->npatch && !held; n++)
                    held = p->patch[n].h && tr_patch_find(p, &p->patch[n], v[0], v[1], v[2], &gx, &gy);
                slabs_out += !held;
            }
    qsort(diff, (size_t)nd, sizeof diff[0], cmp_i32);
    if (!quiet || bad || slabs_out)
        printf("  %s: %ld answers in the patches asked, %d not terrain.c's; outside them (%d places) the bake's ground from "
               "terrain.c's finest: half within %.1f km, 90%% %.1f, 99%% %.1f, the most %.1f; %d slab cells outside every patch\n",
               id, asked, bad, nd, nd ? diff[nd / 2] / 256.0 : 0.0, nd ? diff[nd * 9 / 10] / 256.0 : 0.0,
               nd ? diff[(int)(nd * 0.99)] / 256.0 : 0.0, nd ? diff[nd - 1] / 256.0 : 0.0, slabs_out);
    (void)in_patch;
    bk_end();
    return bad || slabs_out;
}

/* --- the picture's ground (pview.h): the pyramid of grids on the plane touching the planet at the mission's middle --- */

#define PV_FINE 1.15                    /* a level finer than the planet's grid only round patches at least about as fine */
#define PV_FLIGHT_MARGIN_KM 6000.0      /* the flight's region: its nav points, launch and patches, and this round them */

static uint8_t *g_v;                    /* the section being made */
static size_t g_vn, g_vcap;

static uint32_t v_put(const void *p, size_t n)
{
    uint32_t at = (uint32_t)g_vn;
    if (g_vn + n + 1 > g_vcap) {
        g_vcap = (g_vn + n + 1) * 2;
        g_v = (uint8_t *)realloc(g_v, g_vcap);
    }
    if (p)
        memcpy(g_v + g_vn, p, n);
    else
        memset(g_v + g_vn, 0, n);
    g_vn += n;
    return at;
}

typedef struct { double e[3], a[3], b[3], r; } VPlane;   /* r: the radius, plane units */

static double v_dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* a direction's place on the plane, plane units: 0 when it is not on the plane's side */
static int v_on(const VPlane *pl, const double *d, double *x, double *y)
{
    double de = v_dot(d, pl->e);
    if (de < 1e-6)
        return 0;
    *x = pl->r * v_dot(d, pl->a) / de;
    *y = pl->r * v_dot(d, pl->b) / de;
    return 1;
}

static void v_dir(const VPlane *pl, double x, double y, double *d)
{
    int k;
    double l;
    for (k = 0; k < 3; k++)
        d[k] = pl->e[k] * pl->r + pl->a[k] * x + pl->b[k] * y;
    l = sqrt(v_dot(d, d));
    for (k = 0; k < 3; k++)
        d[k] /= l;
}

/* a convex polygon's x extent over rows y0 .. y1 (clipped to the band), 0 when it misses it */
static int v_band(const double (*q)[2], int n, double y0, double y1, double *xmin, double *xmax)
{
    double a[16][2], b[16][2];
    int na = n, nb, i, k;
    memcpy(a, q, sizeof(double) * 2 * n);
    for (k = 0; k < 2; k++) {
        double lim = k ? y1 : y0, sg = k ? -1 : 1;
        nb = 0;
        for (i = 0; i < na; i++) {
            const double *p0 = a[i], *p1 = a[(i + 1) % na];
            double d0 = sg * (p0[1] - lim), d1 = sg * (p1[1] - lim);
            if (d0 >= 0) {
                b[nb][0] = p0[0];
                b[nb++][1] = p0[1];
            }
            if ((d0 >= 0) != (d1 >= 0)) {
                double t = d0 / (d0 - d1);
                b[nb][0] = p0[0] + (p1[0] - p0[0]) * t;
                b[nb++][1] = p0[1] + (p1[1] - p0[1]) * t;
            }
        }
        memcpy(a, b, sizeof(double) * 2 * nb);
        na = nb;
        if (!na)
            return 0;
    }
    *xmin = 1e300;
    *xmax = -1e300;
    for (i = 0; i < na; i++) {
        if (a[i][0] < *xmin) *xmin = a[i][0];
        if (a[i][0] > *xmax) *xmax = a[i][0];
    }
    return 1;
}

/* the ground at a plane point for a level of the given cell (plane units): its height (BK_HU) and colour id, from
 * terrain.c's grid at the level whose cells are about as big there (its patches where that is fine enough) */
static void v_cell(TrPlanet *p, const VPlane *pl, double x, double y, double cell, uint8_t *out)
{
    double d[3], cosf = pl->r / sqrt(pl->r * pl->r + x * x + y * y), foot = cell * pow(cosf, 1.5) * PV_PU;
    int k = 0, h, c, light;
    TrHit s;
    while (k < TR_LEVELS - 1 && foot >= p->cell * (2 << k))
        k++;
    v_dir(pl, x, y, d);
    p->line_patches = (1 << p->npatch) - 1;
    tr_sample(p, d[0], d[1], d[2], k, &s);
    h = (int)floor(s.h / TR_HU + 0.5);
    if (h < -32000) h = -32000;
    if (h > 32000) h = 32000;
    light = (int)floor(s.light + 0.5);
    if (light > TR_LIGHTS - 1) light = TR_LIGHTS - 1;
    if (light < 0) light = 0;
    c = s.mat * TR_LIGHTS + light;
    out[0] = (uint8_t)(h & 255);
    out[1] = (uint8_t)((unsigned)h >> 8 & 255);
    out[2] = (uint8_t)c;
}

/* the section made (g_v, g_vn): the plane at the nav points' middle; the levels; the dither and haze tables */
static int view_bake(TrPlanet *p, const RvMission *m, const TrMissionKm *mk, const char *id, int quiet)
{
    static const double fr[PV_DITHER_LEVELS] = { 0, 3 / 25.0, 5 / 25.0, 8 / 25.0, 10 / 25.0, 13 / 25.0, 15 / 25.0,
                                                 18 / 25.0, 20 / 25.0, 23 / 25.0, 1 };
    PvHead h;
    PvLevel lv[PV_LEVELS];
    VPlane pl;
    double fx0 = 1e300, fx1 = -1e300, fy0 = 1e300, fy1 = -1e300, quad[TR_PATCHES][4][2], qcell[TR_PATCHES], base_cell;
    int npq = 0, k, j, l, i, nlev;
    long counts[256];
    uint32_t total = 0;
    g_vn = 0;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "RVPV", 4);
    /* the plane: its middle the nav points' (as directions from the centre) */
    {
        double e[3] = { 0, 0, 0 }, ln;
        for (j = 0; j < m->nnav; j++) {
            double q[3];
            for (k = 0; k < 3; k++)
                q[k] = (double)(&m->nav[j].x)[k] - mk->centre[k];
            ln = sqrt(v_dot(q, q));
            for (k = 0; k < 3; k++)
                e[k] += q[k] / ln;
        }
        if (!m->nnav)
            for (k = 0; k < 3; k++)
                e[k] = -(double)mk->centre[k];
        ln = sqrt(v_dot(e, e));
        for (k = 0; k < 3; k++)
            pl.e[k] = e[k] / ln;
        k = fabs(pl.e[0]) < 0.6 ? 0 : 1;
        pl.a[0] = pl.a[1] = pl.a[2] = 0;
        pl.a[k] = 1;
        ln = v_dot(pl.a, pl.e);
        for (k = 0; k < 3; k++)
            pl.a[k] -= ln * pl.e[k];
        ln = sqrt(v_dot(pl.a, pl.a));
        for (k = 0; k < 3; k++)
            pl.a[k] /= ln;
        pl.b[0] = pl.e[1] * pl.a[2] - pl.e[2] * pl.a[1];
        pl.b[1] = pl.e[2] * pl.a[0] - pl.e[0] * pl.a[2];
        pl.b[2] = pl.e[0] * pl.a[1] - pl.e[1] * pl.a[0];
        pl.r = (double)p->radius / PV_PU;
        for (k = 0; k < 3; k++) {
            h.e[k] = (int32_t)floor(pl.e[k] * 1073741824.0 + 0.5);
            h.a[k] = (int32_t)floor(pl.a[k] * 1073741824.0 + 0.5);
            h.b[k] = (int32_t)floor(pl.b[k] * 1073741824.0 + 0.5);
        }
        h.radius = p->radius / PV_PU;
    }
    /* the flight's region: the nav points, the launch (the carrier, at the world's middle), the patches */
    {
        double x, y, q[3];
        for (j = 0; j <= m->nnav; j++) {
            for (k = 0; k < 3; k++)
                q[k] = (j < m->nnav ? (double)(&m->nav[j].x)[k] : 0) - mk->centre[k];
            if (!v_on(&pl, q, &x, &y))
                continue;
            if (x < fx0) fx0 = x;
            if (x > fx1) fx1 = x;
            if (y < fy0) fy0 = y;
            if (y > fy1) fy1 = y;
        }
        for (k = 0; k < p->npatch; k++) {
            const TrPatch *pt = &p->patch[k];
            if (!pt->h)
                continue;
            for (j = 0; j < 4; j++) {
                double su = (j == 1 || j == 2) ? 1 : -1, sv = j >= 2 ? 1 : -1;
                for (i = 0; i < 3; i++)
                    q[i] = pt->cf[i] + pt->af[i] * su * pt->half_a / p->radius + pt->bf[i] * sv * pt->half_b / p->radius;
                if (!v_on(&pl, q, &quad[npq][j][0], &quad[npq][j][1])) {
                    fprintf(stderr, "planetbake: %s: patch %d is not on the plane's side\n", id, k);
                    return 1;
                }
                x = quad[npq][j][0];
                y = quad[npq][j][1];
                qcell[npq] = (double)pt->cell / PV_PU;
                if (x < fx0) fx0 = x;
                if (x > fx1) fx1 = x;
                if (y < fy0) fy0 = y;
                if (y > fy1) fy1 = y;
            }
            npq++;
        }
        x = PV_FLIGHT_MARGIN_KM * 256 / PV_PU;
        fx0 -= x, fx1 += x, fy0 -= x, fy1 += x;
    }
    /* the levels: level 0 16 km, until a window covers the plane out to PV_TMAX radii */
    h.shift0 = 8;
    h.w = PV_W;
    h.snap = PV_SNAP;
    h.top = (int16_t)p->top;
    h.bottom = (int16_t)p->bottom;
    base_cell = p->cell / PV_PU;
    for (nlev = 0; nlev < PV_LEVELS; nlev++)
        if ((PV_W / 2) * (double)(1L << (h.shift0 + nlev)) >= PV_TMAX * pl.r)
            break;
    if (nlev == PV_LEVELS) {
        fprintf(stderr, "planetbake: %s: %d levels do not reach the plane's edge\n", id, PV_LEVELS);
        return 1;
    }
    nlev++;
    h.nlev = (uint8_t)nlev;
    v_put(&h, sizeof h);
    h.lev_at = v_put(0, sizeof(PvLevel) * nlev);
    /* the slabs: each patch's that has any, on its own grid (as fine as the cells' limit lets) */
    {
        PvSlabs sl;
        uint32_t ncells = 0;
        int shift;
        size_t mark = g_vn;
        double c;
        for (shift = PV_SLAB_SHIFT;; shift++) {
            g_vn = mark;
            ncells = 0;
            c = (double)(1 << shift);
            memset(&sl, 0, sizeof sl);
            sl.shift = (uint8_t)shift;
            sl.under_div = (int32_t)floor(p->relief * 0.08 + 0.5);
            for (k = 0; k < 64; k++)
                sl.under[k] = (uint8_t)(TR_LIGHTS * (0.24 + 0.3 * exp(-(k + 0.5) / 16.0)));
            for (k = 0; k < p->npatch; k++) {
                const TrPatch *pt = &p->patch[k];
                double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
                int32_t i0, j0, iw, jh, a2, b2;
                long q2, ns = 0;
                if (!pt->h)
                    continue;
                for (q2 = 0; q2 < (long)(pt->na + 1) * (pt->nb + 1); q2++)
                    if (pt->lo[q2] < pt->hi[q2]) {
                        double u = -pt->half_a + (q2 % (pt->na + 1)) * pt->cell, w2 = -pt->half_b + (q2 / (pt->na + 1)) * pt->cell, d[3], x, y;
                        for (i = 0; i < 3; i++)
                            d[i] = pt->cf[i] + pt->af[i] * u / p->radius + pt->bf[i] * w2 / p->radius;
                        if (!v_on(&pl, d, &x, &y))
                            continue;
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                        ns++;
                    }
                if (!ns)
                    continue;
                if (sl.n == PV_SLABS) {
                    fprintf(stderr, "planetbake: %s: more than %d patches with slabs\n", id, PV_SLABS);
                    return 1;
                }
                i0 = (int32_t)floor(x0 / c) - 1;
                j0 = (int32_t)floor(y0 / c) - 1;
                iw = (int32_t)ceil(x1 / c) + 2 - i0;
                jh = (int32_t)ceil(y1 / c) + 2 - j0;
                sl.s[sl.n].x0 = i0;
                sl.s[sl.n].y0 = j0;
                sl.s[sl.n].w = (uint16_t)iw;
                sl.s[sl.n].h = (uint16_t)jh;
                sl.s[sl.n].at = v_put(0, (size_t)iw * jh * 4);
                ncells += (uint32_t)(iw * jh);
                for (b2 = 0; b2 < jh; b2++)
                    for (a2 = 0; a2 < iw; a2++) {
                        double d[3];
                        TrHit hs;
                        int16_t *o = (int16_t *)(g_v + sl.s[sl.n].at) + 2 * (b2 * iw + a2);
                        v_dir(&pl, (i0 + a2) * c, (j0 + b2) * c, d);
                        p->line_patches = (1 << p->npatch) - 1;
                        hs.pslab = 0;
                        tr_sample(p, d[0], d[1], d[2], 0, &hs);
                        o[0] = o[1] = 0;
                        if (hs.patch && hs.pslab) {
                            o[0] = (int16_t)floor(hs.slo + 0.5);
                            o[1] = (int16_t)floor(hs.shi + 0.5);
                        }
                    }
                sl.n++;
            }
            if (ncells <= PV_SLAB_CELLS)
                break;
        }
        if (!quiet && sl.n)
            printf("  view slabs: %d grids of %.0f km cells, %u cells\n", sl.n, c * PV_PU / 256, ncells);
        h.slabs_at = v_put(&sl, sizeof sl);
    }
    memset(counts, 0, sizeof counts);
    for (l = 0; l < nlev; l++) {
        double c = (double)(1L << (h.shift0 + l)), mg = (PV_W / 2 + PV_SNAP) * c, lim = PV_TMAX * pl.r;
        double x0, x1, y0, y1;
        int fine = c < base_cell * 0.9, rows, empty = 0;
        PvRow *rw;
        uint32_t bytes = 0;
        if (fine) {
            x0 = y0 = 1e300;
            x1 = y1 = -1e300;
            for (k = 0; k < npq; k++)
                for (j = 0; j < 4 && qcell[k] <= c * PV_FINE; j++) {
                    if (quad[k][j][0] < x0) x0 = quad[k][j][0];
                    if (quad[k][j][0] > x1) x1 = quad[k][j][0];
                    if (quad[k][j][1] < y0) y0 = quad[k][j][1];
                    if (quad[k][j][1] > y1) y1 = quad[k][j][1];
                }
            if (x0 > x1) {
                x0 = y0 = x1 = y1 = 0;
                empty = 1;
            }
        } else {
            x0 = fx0, x1 = fx1, y0 = fy0, y1 = fy1;
        }
        x0 -= mg, x1 += mg, y0 -= mg, y1 += mg;
        if (x0 < -lim) x0 = -lim;
        if (y0 < -lim) y0 = -lim;
        if (x1 > lim) x1 = lim;
        if (y1 > lim) y1 = lim;
        lv[l].x0 = (int32_t)floor(x0 / c);
        lv[l].y0 = (int32_t)floor(y0 / c);
        if (empty) {
            lv[l].w = lv[l].h = 0;
        } else {
            lv[l].w = (uint16_t)((int32_t)ceil(x1 / c) - lv[l].x0 + 1);
            lv[l].h = (uint16_t)((int32_t)ceil(y1 / c) - lv[l].y0 + 1);
        }
        if (ceil(x1 / c) - lv[l].x0 + 1 > 60000 || ceil(y1 / c) - lv[l].y0 + 1 > 60000) {
            fprintf(stderr, "planetbake: %s: level %d is too big\n", id, l);
            return 1;
        }
        rows = lv[l].h;
        lv[l].rows_at = v_put(0, sizeof(PvRow) * rows);
        for (j = 0; j < rows; j++) {
            double y = (double)(lv[l].y0 + j) * c, a = 1e300, b = -1e300, xa, xb;
            int32_t i0, i1;
            PvRow r;
            if (fine) {
                for (k = 0; k < npq; k++)
                    if (qcell[k] <= c * PV_FINE && v_band(quad[k], 4, y - mg, y + mg, &xa, &xb)) {
                        if (xa - mg < a) a = xa - mg;
                        if (xb + mg > b) b = xb + mg;
                    }
            } else {
                a = x0;
                b = x1;
            }
            r.x0 = 0;
            r.n = 0;
            r.at = 0;
            if (a <= b) {
                i0 = (int32_t)floor(a / c) - lv[l].x0;
                i1 = (int32_t)ceil(b / c) - lv[l].x0;
                if (i0 < 0) i0 = 0;
                if (i1 > lv[l].w - 1) i1 = lv[l].w - 1;
                if (i0 <= i1) {
                    r.x0 = (uint16_t)i0;
                    r.n = (uint16_t)(i1 - i0 + 1);
                    r.at = v_put(0, (size_t)r.n * PV_CELL);
                    for (i = 0; i < r.n; i++) {
                        uint8_t *o = g_v + r.at + (size_t)i * PV_CELL;
                        v_cell(p, &pl, (double)(lv[l].x0 + r.x0 + i) * c, y, c, o);
                        if (l == nlev - 1)
                            counts[o[2]]++;
                    }
                    bytes += (uint32_t)r.n * PV_CELL;
                }
            }
            rw = (PvRow *)(g_v + lv[l].rows_at) + j;
            *rw = r;
        }
        /* its blocks' highest ground (PV_BLK cells a side, a cell more all round) */
        {
            int32_t W = lv[l].w, H = lv[l].h, bi, bj, x, y;
            int16_t *hh = (int16_t *)malloc(sizeof(int16_t) * (W ? W : 1) * (H ? H : 1)), *bm;
            for (j = 0; j < W * H; j++)
                hh[j] = PV_NOGROUND;
            for (j = 0; j < H; j++) {
                const PvRow *r = (const PvRow *)(g_v + lv[l].rows_at) + j;
                for (i = 0; i < r->n; i++) {
                    const uint8_t *o = g_v + r->at + (size_t)i * PV_CELL;
                    hh[j * W + r->x0 + i] = (int16_t)(o[0] | o[1] << 8);
                }
            }
            {   /* (the slabs' tops count) */
                const PvSlabs *sl = (const PvSlabs *)(g_v + h.slabs_at);
                int si, a2, b2, sh = h.shift0 + l, ssh = sl->shift;
                for (si = 0; si < sl->n; si++)
                    for (b2 = 0; b2 < sl->s[si].h; b2++)
                        for (a2 = 0; a2 < sl->s[si].w; a2++) {
                            const int16_t *o = (const int16_t *)(g_v + sl->s[si].at) + 2 * (b2 * sl->s[si].w + a2);
                            int64_t px0 = (int64_t)(sl->s[si].x0 + a2) << ssh, py0 = (int64_t)(sl->s[si].y0 + b2) << ssh;
                            int32_t cx0, cy0, cx1, cy1;
                            if (o[0] >= o[1])
                                continue;
                            cx0 = (int32_t)((px0 - (1 << ssh)) >> sh) - lv[l].x0;
                            cy0 = (int32_t)((py0 - (1 << ssh)) >> sh) - lv[l].y0;
                            cx1 = (int32_t)((px0 + (1 << ssh)) >> sh) - lv[l].x0;
                            cy1 = (int32_t)((py0 + (1 << ssh)) >> sh) - lv[l].y0;
                            for (y = cy0; y <= cy1; y++)
                                for (x = cx0; x <= cx1; x++)
                                    if (x >= 0 && x < W && y >= 0 && y < H && hh[y * W + x] != PV_NOGROUND && hh[y * W + x] < o[1])
                                        hh[y * W + x] = o[1];
                        }
            }
            lv[l].bx0 = (int32_t)floor(lv[l].x0 / (double)PV_BLK);
            lv[l].by0 = (int32_t)floor(lv[l].y0 / (double)PV_BLK);
            lv[l].bw = (uint16_t)((int32_t)floor((lv[l].x0 + W - 1) / (double)PV_BLK) - lv[l].bx0 + 1);
            lv[l].bh = (uint16_t)((int32_t)floor((lv[l].y0 + H - 1) / (double)PV_BLK) - lv[l].by0 + 1);
            if (!W || !H)
                lv[l].bw = lv[l].bh = 0;
            lv[l].bmax_at = v_put(0, (size_t)lv[l].bw * lv[l].bh * 2 + 2);
            bm = (int16_t *)(g_v + lv[l].bmax_at);
            for (bj = 0; bj < lv[l].bh; bj++)
                for (bi = 0; bi < lv[l].bw; bi++) {
                    int32_t best = PV_NOGROUND;
                    for (y = (lv[l].by0 + bj) * PV_BLK - 1; y < (lv[l].by0 + bj) * PV_BLK + PV_BLK + 1; y++)
                        for (x = (lv[l].bx0 + bi) * PV_BLK - 1; x < (lv[l].bx0 + bi) * PV_BLK + PV_BLK + 1; x++) {
                            int32_t cx = x - lv[l].x0, cy = y - lv[l].y0;
                            if (cx >= 0 && cx < W && cy >= 0 && cy < H && hh[cy * W + cx] > best)
                                best = hh[cy * W + cx];
                        }
                    bm[bj * lv[l].bw + bi] = (int16_t)best;
                }
            free(hh);
        }
        total += bytes;
        if (!quiet)
            printf("  view level %d: %.0f km cells, %s %d x %d, %u KB\n", l, c * PV_PU / 256, fine ? "round the patches," : "",
                   lv[l].w, lv[l].h, bytes / 1024);
    }
    memcpy(g_v + h.lev_at, lv, sizeof(PvLevel) * nlev);
    {
        long best = -1;
        for (k = 0; k < 256; k++)
            if (counts[k] > best) {
                best = counts[k];
                h.far_c = (uint8_t)k;
            }
    }
    /* the dither: terrain.c's pair for each (haze, colour id), its share of b as the nearest of the block's */
    h.dither_at = v_put(0, sizeof(PvDither) * PV_FOGS * 256);
    for (k = 0; k < PV_FOGS; k++)
        for (j = 0; j < 256; j++) {
            const TrDither *d = &p->ground[k][j];
            PvDither *o = (PvDither *)(g_v + h.dither_at) + k * 256 + j;
            double want = d->t / 16.0, bd = 2;
            int t;
            o->a = d->a;
            o->b = d->b;
            o->t = 0;
            for (t = 0; t < PV_DITHER_LEVELS; t++)
                if (fabs(fr[t] - want) < bd) {
                    bd = fabs(fr[t] - want);
                    o->t = (uint8_t)t;
                }
            if (o->t == PV_DITHER_LEVELS - 1) {   /* (all b: b alone) */
                o->a = o->b;
                o->t = 0;
            }
        }
    /* the haze: G over the heights, the eye's own up to far over the ground; the level by the sum */
    {
        PvFog fg;
        double dk = p->fog_density * 256 * PV_PU, hhu = p->fog_height / TR_HU;
        memset(&fg, 0, sizeof fg);
        fg.h0 = p->bottom;
        fg.hstep = (p->top - p->bottom + PV_FOG_HB - 2) / (PV_FOG_HB - 1);
        if (fg.hstep < 1)
            fg.hstep = 1;
        fg.k = (int32_t)floor(dk * hhu * 256 + 0.5);
        for (k = 0; k < PV_FOG_GS; k++)
            fg.g[k] = (int32_t)floor(tr_fog_g(p, (double)(fg.h0 + k * fg.hstep) * TR_HU) * 16777216.0 + 0.5);
        for (k = 0; k < PV_FOG_TAUS; k++)
            fg.lev[k] = (uint8_t)floor(p->fog_lev[k] + 0.5);
        h.fog_at = v_put(&fg, sizeof fg);
        if (!quiet)
            printf("  view haze: k %d, step %d HU, G %d .. %d\n", fg.k, fg.hstep, fg.g[0], fg.g[PV_FOG_GS - 1]);
    }
    /* the sky's glow: terrain.c's by the eye's height */
    {
        static PvGlow gl;
        int ja, jc;
        memset(&gl, 0, sizeof gl);
        gl.amax = (int32_t)(40000.0 * 256 / PV_PU);
        for (k = 0; k < PV_FOGS; k++) {
            gl.sky[k].a = p->sky[k][0].a;
            gl.sky[k].b = p->sky[k][0].b;
            gl.sky[k].t = (uint8_t)floor(p->sky[k][0].t * 10 / 16.0 + 0.5);
        }
        for (ja = 0; ja < PV_GLOW_AS; ja++) {
            double a = (double)gl.amax * PV_PU * ja * ja / ((PV_GLOW_AS - 1.0) * (PV_GLOW_AS - 1.0));
            tr_sky_table(p, p->radius + a);
            for (jc = 0; jc < PV_GLOW_CZ; jc++) {
                double cz = -1 + 2.0 * (jc + 0.5) / PV_GLOW_CZ;
                int si = (int)((cz + 1) * 0.5 * TR_SKY_STEPS);
                si = si < 0 ? 0 : si > TR_SKY_STEPS - 1 ? TR_SKY_STEPS - 1 : si;
                gl.g[ja][jc] = (uint8_t)(p->skytab[si] >> 5);
            }
        }
        h.glow_at = v_put(&gl, sizeof gl);
    }
    /* a window's worth of no ground */
    h.absent_at = v_put(0, (size_t)PV_W * PV_W * PV_CELL + 2);
    for (k = 0; k < PV_W * PV_W; k++)
        g_v[h.absent_at + k * PV_CELL + 1] = PV_ABSENT;
    h.noblk_at = v_put(0, (size_t)PV_BW * PV_BW * 2);
    for (k = 0; k < PV_BW * PV_BW; k++)
        g_v[h.noblk_at + k * 2 + 1] = 0x80;
    v_put(0, 8);
    if (g_vn & 1)
        v_put(0, 1);
    h.size = (uint32_t)g_vn;
    memcpy(g_v, &h, sizeof h);
    if (!quiet)
        printf("  view: %d levels, %u KB of cells, %u KB in all\n", nlev, total / 1024, h.size / 1024);
    return 0;
}

static int bake(const RvMission *m, const char *dir, uint32_t stamp, int force, int quiet)
{
    char id[RV_ID + 1], path[1024];
    TrMissionKm mk;
    TrPlanetDesc d;
    TrStamp st[TR_STAMPS_MAX];
    TrPlanet *p;
    BkHead h;
    BkPatch bp[BK_PATCHES];
    FILE *f;
    int k, np = 0, r;
    uint32_t at, key;
    memcpy(id, m->id, RV_ID);
    id[RV_ID] = 0;
    if (strlen(id) > 8) {
        fprintf(stderr, "planetbake: %s: a planet mission's id names its file (8 letters at most)\n", id);
        return 1;
    }
    snprintf(path, sizeof path, "%s/%s.PLN", dir, id);
    key = bk_key(&m->px, m->nav, m->nnav);
    f = fopen(path, "rb");
    if (f) {
        size_t got = fread(&h, 1, sizeof h, f);
        fclose(f);
        if (!force && got == sizeof h && !memcmp(h.magic, "RVPL", 4) && h.version == BK_VERSION && h.key == key
            && h.stamp == stamp)
            return 0;
    }
    tr_from_records((const TrPlanetRec *)&m->px, (const TrNavRec *)m->nav, m->nnav, &mk);
    tr_from_km(&mk, &d, st);
    p = tr_begin(&d, st, g_pal);
    while (p && (r = tr_build(p)) > 0)
        ;
    if (!p || r < 0) {
        fprintf(stderr, "planetbake: %s: no memory for the planet\n", id);
        return 1;
    }
    memset(&h, 0, sizeof h);
    memset(bp, 0, sizeof bp);
    memcpy(h.magic, "RVPL", 4);
    h.version = BK_VERSION;
    h.n = (uint16_t)(TR_N >> BK_LEVEL);
    h.key = key;
    h.stamp = stamp;
    h.radius = p->radius;
    h.top = (int16_t)p->top;
    h.bottom = (int16_t)p->bottom;
    h.kind = p->desc.kind;
    at = sizeof h;
    h.grid_at = at;
    at += 6u * tr_side(BK_LEVEL) * tr_side(BK_LEVEL) * BK_CELL;
    for (k = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        BkPatch *b = &bp[np];
        size_t n = (size_t)(pt->na + 1) * (pt->nb + 1), i;
        double cs;
        if (!pt->h)
            continue;
        if (pt->na >= 32767 || pt->nb >= 32767) {
            fprintf(stderr, "planetbake: %s: patch %d is %d x %d cells (32766 at most)\n", id, k, pt->na, pt->nb);
            return 1;
        }
        memcpy(b->c, pt->c, sizeof b->c);
        memcpy(b->a, pt->a, sizeof b->a);
        memcpy(b->b, pt->b, sizeof b->b);
        b->half_a = pt->half_a;
        b->half_b = pt->half_b;
        b->cell = pt->cell;
        b->na = (uint16_t)pt->na;
        b->nb = (uint16_t)pt->nb;
        b->top = pt->top;
        b->over = pt->over;
        /* its far corner's direction from its middle, less the quick test's rounding (bake.c bk_column) */
        cs = p->radius / sqrt((double)p->radius * p->radius + (double)pt->half_a * pt->half_a + (double)pt->half_b * pt->half_b);
        b->cosmin = (int16_t)(floor(cs * 16384) - 12);
        for (i = 0; i < n && !b->slabs; i++)
            b->slabs = pt->lo[i] < pt->hi[i];
        np++;
    }
    h.npatch = (uint8_t)np;
    h.patch_at = at;
    at += (uint32_t)np * sizeof(BkPatch);
    for (k = 0; k < np; k++) {
        size_t n = (size_t)(bp[k].na + 1) * (bp[k].nb + 1);
        bp[k].at = at;
        at += (uint32_t)(n * BK_CELL);
        at += at & 1;
        if (bp[k].slabs) {
            bp[k].slab_at = at;
            at += (uint32_t)(n * BK_SLAB);
        }
    }
    at += 8;                            /* (room past the last point: a read of two cells there stays in the file) */
    if (view_bake(p, m, &mk, id, quiet))
        return 1;
    h.view = 1;
    h.view_at = at;
    h.view_size = (uint32_t)g_vn;
    at += (uint32_t)g_vn;
    h.size = at;
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    put(f, &h, sizeof h);
    put_grid(f, p, BK_LEVEL);
    put(f, bp, (size_t)np * sizeof(BkPatch));
    for (k = 0, np = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        size_t n = (size_t)(pt->na + 1) * (pt->nb + 1), i;
        uint8_t *buf;
        if (!pt->h)
            continue;
        buf = (uint8_t *)malloc(n * BK_SLAB + 1);
        for (i = 0; i < n; i++) {
            buf[i * BK_CELL] = (uint8_t)(pt->h[i] & 0xFF);
            buf[i * BK_CELL + 1] = (uint8_t)((uint16_t)pt->h[i] >> 8);
            buf[i * BK_CELL + 2] = pt->col[i];
        }
        buf[n * BK_CELL] = 0;
        put(f, buf, n * BK_CELL + ((n * BK_CELL) & 1));
        if (bp[np].slabs) {
            for (i = 0; i < n; i++) {
                buf[i * 4] = (uint8_t)(pt->lo[i] & 0xFF);
                buf[i * 4 + 1] = (uint8_t)((uint16_t)pt->lo[i] >> 8);
                buf[i * 4 + 2] = (uint8_t)(pt->hi[i] & 0xFF);
                buf[i * 4 + 3] = (uint8_t)((uint16_t)pt->hi[i] >> 8);
            }
            put(f, buf, n * BK_SLAB);
        }
        free(buf);
        np++;
    }
    {
        static const uint8_t zero[8];
        put(f, zero, 8);
    }
    put(f, g_v, g_vn);
    if ((uint32_t)ftell(f) != h.size) {
        fprintf(stderr, "planetbake: %s: wrote %ld bytes, the head says %u\n", id, ftell(f), h.size);
        return 1;
    }
    fclose(f);
    /* read back and asked */
    f = fopen(path, "rb");
    g_size = h.size;
    g_file = (uint8_t *)malloc(g_size);
    if (!f || fread(g_file, 1, g_size, f) != g_size) {
        perror(path);
        return 1;
    }
    fclose(f);
    printf("%s.PLN: %u bytes (%d patches: %u bytes of them; the picture's ground %u)\n", id, h.size, h.npatch,
           h.view_at - h.patch_at, h.view_size);
    r = check(p, id, quiet);
    free(g_file);
    g_file = 0;
    tr_free(p);
    if (r) {
        remove(path);
        fprintf(stderr, "planetbake: %s: the bake's answers are not right (the file removed)\n", id);
    }
    return r;
}

int main(int argc, char **argv)
{
    const RvGameHeader *hd;
    uint8_t *dat;
    long n;
    FILE *f;
    int k, force = 0, quiet = 0, bad = 0, made = 0, have_pal = 0;
    uint32_t stamp = 0;
    const char *only = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: planetbake RVGAME.DAT OUTDIR --palette FILE [--stamp N] [--force] [--only ID] [--quiet]\n");
        return 2;
    }
    for (k = 3; k < argc; k++) {
        if (!strcmp(argv[k], "--force"))
            force = 1;
        else if (!strcmp(argv[k], "--quiet"))
            quiet = 1;
        else if (!strcmp(argv[k], "--stamp") && k + 1 < argc)
            stamp = (uint32_t)strtoul(argv[++k], 0, 0);
        else if (!strcmp(argv[k], "--only") && k + 1 < argc)
            only = argv[++k];
        else if (!strcmp(argv[k], "--palette") && k + 1 < argc) {
            FILE *pf = fopen(argv[++k], "rb");
            if (!pf || fread(g_pal, 1, 768, pf) != 768) {
                fprintf(stderr, "planetbake: %s is no palette (768 bytes)\n", argv[k]);
                return 2;
            }
            fclose(pf);
            have_pal = 1;
        }
        else {
            fprintf(stderr, "planetbake: what is %s?\n", argv[k]);
            return 2;
        }
    }
    if (!have_pal) {
        fprintf(stderr, "planetbake: the flight's palette is wanted (--palette FILE: 768 bytes, 6 bits each)\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    dat = (uint8_t *)malloc((size_t)n);
    if (fread(dat, 1, (size_t)n, f) != (size_t)n) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    hd = (const RvGameHeader *)dat;
    if (hd->mission_size != sizeof(RvMission)) {
        fprintf(stderr, "planetbake: %s's missions are %u bytes, rvdata.h's %u\n", argv[1], hd->mission_size,
                (unsigned)sizeof(RvMission));
        return 1;
    }
    for (k = 0; k < hd->nmissions; k++) {
        const RvMission *m = (const RvMission *)(dat + hd->missions_at + (long)k * hd->mission_size);
        if (!m->pradius || (only && strncmp(only, m->id, RV_ID)))
            continue;
        bad += bake(m, argv[2], stamp, force, quiet);
        made++;
    }
    if (!made && only) {
        fprintf(stderr, "planetbake: no planet mission %s\n", only);
        return 1;
    }
    return bad ? 1 : 0;
}
