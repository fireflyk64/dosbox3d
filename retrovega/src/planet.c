/* planet.c: a planet in a flight (planet.h).  On DOS the card does the work: each call a request copied into the
 * card's block (gx_planet_call), its layout the card's (docs/gx.md); its sums too (where nav points and ships go over
 * the ground: integers, every machine's the same), so RV.EXE carries little.  The native builds (RV_HOST) answer the
 * same requests with terrain.c themselves, here. */
#include <string.h>
#include "rv.h"
#include "gx.h"
#include "world.h"
#include "planet.h"

#define PL_LOAD 0xE740                  /* the card's planet calls (rvgx.cpp) */
#define PL_VIEW 0xE741
#define PL_COLUMN 0xE743
#define PL_SEEN 0xE744
#define PL_MAKE 0xE745
#define PL_DROP 0xE746
#define PL_PALETTE 0xE747
#define PL_PLACE 0xE748                 /* a nav point (world km) put as high over the ground as over the radius */
#define PL_RAISE 0xE749                 /* a ship (world positions) raised to km over the ground if it is under that */

#ifdef __WATCOMC__
#pragma pack(1)
#endif
typedef struct {                        /* PL_LOAD: the mission record's planet tail and nav points, the palette */
    const TrPlanetRec __far *planet;
    const TrNavRec __far *navs;
    const unsigned char __far *pal;
    unsigned short nnav;
} PlLoad;
typedef struct {                        /* PL_VIEW */
    long eye[3];                        /* planet-relative positions */
    short m[9];                         /* the camera's right, up and front, 16384 = 1 */
    short focal, cx, cy, w, h;
} PlView;
#ifdef __WATCOMC__
#pragma pack()
#endif

static struct {
    int on;                             /* a planet set (made or being made) */
    int made;
    LfVec centre;                       /* positions */
    long eye[3];                        /* the frame's eye, planet-relative */
} __far g_pl;

#if defined(RV_HOST) && !defined(RV_HOST_GAME)
#define PL_PALETTE_NOW() 0              /* (rvsim: no palette, no picture) */
#else
#define PL_PALETTE_NOW() shot_palette()
#endif

#ifdef RV_HOST
/* --- the native builds: the card's work done here --- */
static TrPlanet *g_host_planet;
static int g_host_viewed;
static int32_t g_host_centre[3];        /* km */

int gx_planet_on(void) { return 1; }

int gx_planet_call(unsigned ax, const void *in, int n_in, void *out, int n_out)
{
    TrPlanet *p = g_host_planet;
    (void)n_in;
    (void)n_out;
    if (ax == PL_LOAD) {
        const PlLoad *l = (const PlLoad *)in;
        TrMissionKm m;
        TrPlanetDesc d;
        TrStamp st[TR_STAMPS_MAX];
        tr_from_records(l->planet, l->navs, l->nnav, &m);
        tr_from_km(&m, &d, st);
        g_host_centre[0] = m.centre[0];
        g_host_centre[1] = m.centre[1];
        g_host_centre[2] = m.centre[2];
        tr_free(p);
        g_host_planet = tr_begin(&d, st, l->pal);
        g_host_viewed = 0;
        return g_host_planet ? 0 : -2;
    }
    if (!p)
        return -1;
    if (ax == PL_MAKE || ax == PL_COLUMN || ax == PL_PLACE || ax == PL_RAISE)   /* (all at once natively) */
        while (!tr_ready(p))
            if (tr_build(p) < 0)
                return -2;
    switch (ax) {
    case PL_MAKE:
        return 100;
    case PL_COLUMN: {
        const long *at = (const long *)in;
        long *o = (long *)out;
        TrColumn col;
        tr_column(p, (int32_t)at[0], (int32_t)at[1], (int32_t)at[2], &col);
        o[0] = col.ground;
        o[1] = col.slab_lo;
        o[2] = col.slab_hi;
        o[3] = col.r;
        return 0;
    }
    case PL_PLACE: {
        const long *a = (const long *)in;
        long *o = (long *)out;
        int32_t v[3];
        TrColumn col;
        int k;
        for (k = 0; k < 3; k++)
            v[k] = (int32_t)((a[k] - g_host_centre[k]) * 256);
        tr_column(p, v[0], v[1], v[2], &col);
        tr_place(p, v, col.r - tr_radius(p));
        for (k = 0; k < 3; k++)
            o[k] = v[k] / 256 + g_host_centre[k];
        return 0;
    }
    case PL_RAISE: {
        const long *a = (const long *)in;
        long *o = (long *)out;
        int32_t v[3];
        TrColumn col;
        int k;
        for (k = 0; k < 3; k++)
            v[k] = (int32_t)(a[k] - g_host_centre[k] * 256);
        tr_column(p, v[0], v[1], v[2], &col);
        if (col.r - col.ground < a[3] * 256)
            tr_place(p, v, (int32_t)(a[3] * 256));
        for (k = 0; k < 3; k++)
            o[k] = v[k] + g_host_centre[k] * 256;
        return 0;
    }
    case PL_VIEW: {
        const PlView *v = (const PlView *)in;
        TrCam c;
        int k;
        if (!tr_ready(p))
            return -3;
        for (k = 0; k < 3; k++) {
            c.pos[k] = v->eye[k];
            c.right[k] = v->m[k] / 16384.0f;
            c.up[k] = v->m[3 + k] / 16384.0f;
            c.fwd[k] = v->m[6 + k] / 16384.0f;
        }
        c.focal = v->focal;
        c.cx = v->cx;
        c.cy = v->cy;
        c.w = v->w;
        c.h = v->h;
        tr_view(p, &c);
        g_host_viewed = 1;
        return 0;
    }
    case PL_SEEN: {
        const long *e = (const long *)in;
        double a[3], b[3];
        int k;
        if (!tr_ready(p))
            return -3;
        for (k = 0; k < 3; k++) {
            a[k] = e[k];
            b[k] = e[3 + k];
        }
        return tr_seen(p, a, b);
    }
    case PL_PALETTE:
        tr_palette(p, *(const unsigned char *const *)in);
        return 0;
    case PL_DROP:
        tr_free(p);
        g_host_planet = 0;
        return 0;
    }
    return -1;
}

void gx_ground(int y0, int y1, const short *ext, int ext_y0, int ext_rows)
{
#ifdef RV_HOST_GAME                     /* (rvsim, the logic alone, draws nothing) */
    if (g_host_planet && g_host_viewed && y0 < y1)
        tr_blit(g_host_planet, vid_row(y0), y0, SCR_W, y0, y1, 0, SCR_W, ext, ext_y0, ext_rows);
#else
    (void)y0, (void)y1, (void)ext, (void)ext_y0, (void)ext_rows;
#endif
}
#endif

int planet_can(void) { return gx_planet_on(); }

int planet_load(const TrPlanetRec __far *pr, const TrNavRec __far *navs, int nnav)
{
    PlLoad l;
    int r;
    planet_clear();
    if (!pr->radius || !gx_planet_on())
        return -1;
    l.planet = pr;
    l.navs = navs;
    l.pal = PL_PALETTE_NOW();
    l.nnav = (unsigned short)nnav;
    r = gx_planet_call(PL_LOAD, &l, sizeof l, 0, 0);
    if (r < 0) {
        RV_LOG("planet: the card took none (%d)", r);
        return r;
    }
    g_pl.on = 1;
    g_pl.made = 0;
    g_pl.centre.x = pr->x << 8;
    g_pl.centre.y = pr->y << 8;
    g_pl.centre.z = pr->z << 8;
    return 0;
}

int planet_make(unsigned ms)
{
    int r;
    if (!g_pl.on)
        return -1;
    if (g_pl.made)
        return 100;
    r = gx_planet_call(PL_MAKE, &ms, sizeof ms, 0, 0);
    if (r == 100)
        g_pl.made = 1;
    else if (r < 0)
        g_pl.on = 0;
    return r;
}

void planet_clear(void)
{
    if (g_pl.on)
        (void)gx_planet_call(PL_DROP, &g_pl.on, 0, 0, 0);
    g_pl.on = g_pl.made = 0;
}

int planet_on(void) { return g_pl.on && g_pl.made; }
const LfVec *planet_centre(void) { return (const LfVec *)&g_pl.centre; }

/* a world point planet-relative */
static void rel(const LfVec *p, long out[3])
{
    out[0] = p->x - g_pl.centre.x;
    out[1] = p->y - g_pl.centre.y;
    out[2] = p->z - g_pl.centre.z;
}

void planet_frame(const LfVec *eye, const LfBasis *b, int cx, int cy, int focal)
{
    const long *f = (const long *)b;    /* (LfBasis: front, right, up, 256 = 1; the card's right, up, front, 16384) */
    PlView v;
    int k;
    if (!g_pl.on || (!g_pl.made && planet_make(1) != 100))
        return;
    rel(eye, v.eye);
    for (k = 0; k < 6; k++)
        v.m[k] = (short)(f[3 + k] << 6);
    for (k = 0; k < 3; k++)
        v.m[6 + k] = (short)(f[k] << 6);
    v.focal = (short)focal;
    v.cx = (short)cx;
    v.cy = (short)cy;
    v.w = SCR_W;
    v.h = SCR_H;
    g_pl.eye[0] = v.eye[0];
    g_pl.eye[1] = v.eye[1];
    g_pl.eye[2] = v.eye[2];
    (void)gx_planet_call(PL_VIEW, &v, sizeof v, 0, 0);
}

void planet_band(int y0, int y1, const short __far *ext, int ext_y0, int ext_rows)
{
    if (planet_on())
        gx_ground(y0, y1, ext, ext_y0, ext_rows);
}

int planet_hides(const LfVec *pos)
{
    long q[6];
    if (!planet_on())
        return 0;
    q[0] = g_pl.eye[0];
    q[1] = g_pl.eye[1];
    q[2] = g_pl.eye[2];
    rel(pos, q + 3);
    return gx_planet_call(PL_SEEN, q, sizeof q, 0, 0) == 0;
}

long planet_over(const LfVec *pos, long *roof)
{
    long at[3], o[4];                   /* (the answer: the ground's radius, a slab's underside and top, the point's) */
    *roof = 0x7FFFFFFFL;
    if (!planet_on())
        return 0x7FFFFFFFL;
    rel(pos, at);
    if (gx_planet_call(PL_COLUMN, at, sizeof at, o, sizeof o) < 0)
        return 0x7FFFFFFFL;
    if (o[1] < o[2]) {                  /* a slab: under it, the roof is over the point; in it, the point is in rock */
        if (o[3] < o[1])
            *roof = o[1] - o[3];
        else if (o[3] <= o[2])
            return -1;
    }
    return o[3] - o[0];
}

/* --- a mission's planet (missions.txt planet, ground) --- */

void planet_mission(const RvMission __far *m)
{
    planet_clear();
    (void)planet_load((const TrPlanetRec __far *)&m->px, (const TrNavRec __far *)m->nav, m->nnav);
}

void planet_ready(void)
{
    const unsigned char __far *pal = PL_PALETTE_NOW();
    if (!g_pl.on)
        return;
    (void)planet_make(0);
    (void)gx_planet_call(PL_PALETTE, &pal, sizeof pal, 0, 0);
}

void planet_navs(void)
{
    int n;
    if (!planet_on())
        return;
    for (n = 1; n < 16; n++) {
        uint8_t *rec = navPointTable + navPointTable_SIZE * n;
        if (rec[0] && (rec[1] & RV_NAV_GROUND))
            (void)gx_planet_call(PL_PLACE, &NAV_REC_KM(rec, 0), 12, &NAV_REC_KM(rec, 0), 12);
    }
}

void planet_raise(LfVec *pos, long km)
{
    long q[4];
    if (!planet_on())
        return;
    _fmemcpy(q, pos, 12);
    q[3] = km;
    (void)gx_planet_call(PL_RAISE, q, sizeof q, pos, 12);
}
