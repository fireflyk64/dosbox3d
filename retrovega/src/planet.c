/* planet.c: a planet in a flight (planet.h).
 *
 * The ground a game flies over is the bake's (bake.h: the mission's ID.PLN, made at build time), on every machine: the
 * file is read into XMS as the flight starts and every question the logic has (how high a ship is over the ground, an
 * arch's rock over it, where a ground nav point goes) is answered from it with bake.c's integer sums.  A machine with
 * the sprite card and one without, DOS and the native builds, all fly the same ground.
 *
 * The picture is the card's where there is one: each call a request copied into the card's block (gx_planet_call), its
 * layout the card's (docs/gx.md).  The native game (RV_HOST_GAME) answers the same requests with terrain.c itself,
 * here; rvsim and the host's play tests draw nothing and make no picture. */
#include <string.h>
#if defined(RV_HOST) && !defined(RV_HOST_GAME)
#include <stdio.h>
#else
#include <io.h>
#include <dos.h>
#include <fcntl.h>
#endif
#include "rv.h"
#include "gx.h"
#include "world.h"
#include "planet.h"
#include "bake.h"
#include "cockpit.h"
#include "pview.h"

#define PL_LOAD 0xE740                  /* the card's planet calls (rvgx.cpp) */
#define PL_VIEW 0xE741
#define PL_COLUMN 0xE743
#define PL_SEEN 0xE744
#define PL_MAKE 0xE745
#define PL_DROP 0xE746
#define PL_PALETTE 0xE747
/* (0xE748 PL_PLACE and 0xE749 PL_RAISE, a nav point and a ship put over the card's ground: the card still answers
 * them, the game asks no more.  Those sums are the bake's now, planet_navs and planet_raise) */

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
    int set;                            /* a planet mission's planet is set */
    int ground;                         /* its ground is in: the bake, in XMS (blk) */
    int pic;                            /* its picture is the card's (the native game's): being made, or made */
    int made;
    int pv;                             /* its picture is RV.EXE's own, without the card (pview.c: DOS) */
    XmsBlock blk;
    uint32_t key;                       /* the set planet's records as a number (bk_key): the file must be theirs */
    char file[13];                      /* its ground's file, ID.PLN ("": none, the test planet) */
    LfVec centre;                       /* positions */
    int32_t km[3];                      /* (the same, km) */
    long eye[3];                        /* the frame's eye, planet-relative */
} __far g_pl;

#if defined(RV_HOST) && !defined(RV_HOST_GAME)
#define PL_PALETTE_NOW() 0              /* (rvsim: no palette, no picture; no clock, no watch for a freeze) */
#define PL_CLOCK() 0UL
#define PL_HOLD() (void)0
#else
#include "freeze.h"
#include "music.h"                      /* (clock240) */
#define PL_PALETTE_NOW() shot_palette()
#define PL_CLOCK() clock240()
#define PL_HOLD() freeze_hold()
#endif

#if defined(RV_HOST) && !defined(RV_HOST_GAME)
/* --- rvsim and the host's play tests: the ground alone, no picture --- */
int gx_planet_on(void) { return 0; }
int gx_planet_call(unsigned ax, const void *in, int n_in, void *out, int n_out)
{
    (void)ax, (void)in, (void)n_in, (void)out, (void)n_out;
    return -1;
}
void gx_ground(int y0, int y1, const short *ext, int ext_y0, int ext_rows)
{
    (void)y0, (void)y1, (void)ext, (void)ext_y0, (void)ext_rows;
}
#elif defined(RV_HOST)
/* --- the native game: the card's picture made and drawn here, with terrain.c --- */
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
        if (p && tr_same(p, &d, st) && !memcmp(g_host_centre, m.centre, sizeof g_host_centre))
            return 0;                   /* (the same planet again: kept, as the card keeps it) */
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
    if (ax == PL_MAKE || ax == PL_COLUMN)   /* (all at once natively) */
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
    if (g_host_planet && g_host_viewed && y0 < y1)
        tr_blit(g_host_planet, vid_row(y0), y0, SCR_W, y0, y1, 0, SCR_W, ext, ext_y0, ext_rows);
}
#endif

/* --- the ground: the bake (bake.h), its file in XMS --- */

/* no ground in: its block given back now when it was the last taken (else with its level: the play tests fly one
 * planet after another without ending a mission) */
static void ground_drop(void)
{
#ifdef __WATCOMC__
    if (g_pl.pv)
        pv_end();
#endif
    g_pl.pv = 0;
    if (g_pl.ground) {
        bk_end();
        xms_shrink(g_pl.blk, 0);
    }
    g_pl.ground = 0;
    g_pl.blk = 0;
}

void planet_xms_freed(unsigned nblocks)
{
#ifdef __WATCOMC__
    pv_xms_freed(nblocks);
    if (g_pl.pv && !pv_on())
        g_pl.pv = 0;
#endif
    if (g_pl.blk > nblocks) {
        bk_end();
        g_pl.ground = 0;
        g_pl.blk = 0;
    }
}

/* the set planet's file read into XMS and taken as the ground: 1 when the ground is in (it may be already) */
static int ground_load(int say)        /* (say: the file missing logged: the last try, after the page has had the
                                           launch to fetch it) */
{
    BkHead h;
    char name[13];                      /* (near: DOS wants it so) */
    unsigned long at = 0, t0 = PL_CLOCK(), off;
    unsigned got, handle;
    XmsBlock blk = 0;
    int i;
    if (g_pl.ground)
        return 1;
    if (!g_pl.set || !g_pl.file[0])
        return 0;
    for (i = 0; i < 13; i++)
        name[i] = g_pl.file[i];
    h.key = h.size = 0;
    PL_HOLD();                          /* (megabytes: seconds from a slow disk) */
    {
#if defined(RV_HOST) && !defined(RV_HOST_GAME)
        static unsigned char part[32768];
        FILE *f = fopen(name, "rb");
        if (!f) {
            if (say)
                RV_LOG("planet: no %s here: no ground", name);
            return 0;
        }
        got = (unsigned)fread(&h, 1, sizeof h, f);
#define PL_READ(n) (got = (unsigned)fread(part, 1, (n), f))
#define PL_PART sizeof part
#define PL_CLOSE() fclose(f)
#define PL_REWIND() rewind(f)
#else
        /* (a kilobyte at a time, through the one near buffer file reads land in: the play tests' build has no 32 KB
         * scratch buffer, and under an emulator the reads cost next to nothing) */
        unsigned char *part = rv_filebuf;
        int fd;
        if (_dos_open(name, O_RDONLY, &fd) != 0) {
            if (say)
                RV_LOG("planet: no %s here: no ground", name);
            return 0;
        }
        if (_dos_read(fd, part, sizeof h, &got) != 0)
            got = 0;
        if (got == sizeof h)
            _fmemcpy(&h, part, sizeof h);
#define PL_READ(n) (_dos_read(fd, part, (n), &got) != 0 ? (got = 0) : got)
#define PL_PART sizeof rv_filebuf
#define PL_CLOSE() _dos_close(fd)
#define PL_REWIND() lseek(fd, 0, SEEK_SET)
#endif
        /* (the key: the mission's records and the bake's version as a number.  Another's file, or another build's,
         * would be another planet's ground) */
        if (got == sizeof h && h.key == g_pl.key && h.size <= 64UL * 1024 * 1024
            && (blk = xms_alloc(h.size + 2, XMS_MISSION)) != 0) {
            PL_REWIND();
            while (at < h.size) {
                unsigned want = h.size - at < PL_PART ? (unsigned)(h.size - at) : (unsigned)PL_PART;
                if (PL_READ(want) != want)
                    break;
                (void)xms_put(blk, at, part, (want + 1) & ~1U);
                at += want;
            }
        }
        PL_CLOSE();
#undef PL_READ
#undef PL_PART
#undef PL_CLOSE
#undef PL_REWIND
    }
    if (blk) {
        g_pl.blk = blk;
        g_pl.ground = 1;
        xms_where(blk, &handle, &off);
        if (at == h.size && bk_begin(handle, off) == 0) {
            RV_LOG("planet: %s, the ground: %lu KB in XMS in %lu ms (%d patches; %lu KB of XMS free)", name,
                   (unsigned long)(h.size / 1024), (PL_CLOCK() - t0) * 1000 / 240, (int)bk_ask.npatch, xms_free_kb());
#ifdef __WATCOMC__
            /* no card to draw it: RV.EXE draws it itself, from the file's picture section (pview.h) */
            if (!g_pl.pic && h.view && pv_begin(handle, off, h.view_at) == 0)
                g_pl.pv = 1;
#endif
            return 1;
        }
        ground_drop();                  /* (its block back) */
    }
    /* (which: a key that is not the mission's; less read than the file says it has; fewer KB free than it needs) */
    RV_LOG("planet: %s is no ground for this mission: key %08lX for %08lX, %lu of %lu bytes read, %lu KB of XMS free",
           name, (unsigned long)h.key, (unsigned long)g_pl.key, at, (unsigned long)h.size, xms_free_kb());
    return 0;
}

/* --- */

int g_planet_off;
#if (defined(RV_HOST) && !defined(RV_HOST_GAME)) || defined(__WATCOMC__)
int planet_can(void) { return !g_planet_off; }   /* (DOS draws a planet itself without the card: pview.c; the host's
                                           tools and tests fly the campaign's planet missions as DOS does) */
#else
int planet_can(void) { return !g_planet_off && gx_planet_on(); }   /* (the native game: the card's picture or none) */
#endif

/* the planet set from its records; id: the mission's (its ground's file), or 0 */
static int planet_set(const TrPlanetRec __far *pr, const TrNavRec __far *navs, int nnav, const char __far *id)
{
    uint32_t key;
    int i;
    if (!pr->radius) {
        planet_clear();
        return -1;
    }
    key = bk_key(pr, navs, nnav);
    if (g_pl.ground && (key != g_pl.key || !id))   /* (the same planet's again is kept) */
        ground_drop();
    g_pl.set = 1;
    g_pl.key = key;
    for (i = 0; id && i < 8 && id[i] && id[i] != ' '; i++)
        g_pl.file[i] = id[i];
    if (i) {
        g_pl.file[i] = '.';
        g_pl.file[i + 1] = 'P';
        g_pl.file[i + 2] = 'L';
        g_pl.file[i + 3] = 'N';
        i += 4;
    }
    g_pl.file[i] = 0;
    g_pl.km[0] = pr->x;
    g_pl.km[1] = pr->y;
    g_pl.km[2] = pr->z;
    g_pl.centre.x = pr->x * 256L;
    g_pl.centre.y = pr->y * 256L;
    g_pl.centre.z = pr->z * 256L;
    /* its picture, where there is a card: begun now, made a piece at a time (planet_make) */
    g_pl.pic = g_pl.made = 0;
    if (gx_planet_on()) {
        PlLoad l;
        int r;
        l.planet = pr;
        l.navs = navs;
        l.pal = PL_PALETTE_NOW();
        l.nnav = (unsigned short)nnav;
        r = gx_planet_call(PL_LOAD, &l, sizeof l, 0, 0);
        if (r < 0)
            RV_LOG("planet: the card took none (%d)", r);
        else
            g_pl.pic = 1;
    }
    return 0;
}

int planet_load(const TrPlanetRec __far *pr, const TrNavRec __far *navs, int nnav)
{
    return planet_set(pr, navs, nnav, 0);
}

/* more of the picture made: 100 when it is, or when there is none to make */
static int pic_make(unsigned ms)
{
    int r;
    if (!g_pl.pic || g_pl.made)
        return 100;
    r = gx_planet_call(PL_MAKE, &ms, sizeof ms, 0, 0);
    if (r == 100)
        g_pl.made = 1;
    else if (r < 0) {
        RV_LOG("planet: the card made none (%d): no picture of it", r);
        g_pl.pic = 0;
        r = 100;
    }
    return r;
}

int planet_make(unsigned ms)
{
    int r;
    if (!g_pl.set)
        return -1;
    r = pic_make(ms);
    if (ms == 0 && !ground_load(0) && g_pl.file[0])
        return -1;                      /* (a mission's planet with no ground to fly over) */
    return r;
}

void planet_clear(void)
{
    if (g_pl.pic)
        (void)gx_planet_call(PL_DROP, &g_pl.set, 0, 0, 0);
    ground_drop();
    g_pl.set = g_pl.pic = g_pl.made = 0;
}

int planet_on(void) { return g_pl.ground; }
int planet_seen(void) { return g_pl.pic && g_pl.made; }
const LfVec __far *planet_centre(void) { return &g_pl.centre; }   /* (far: g_pl is; a near pointer to it reads DS) */

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
#ifdef __WATCOMC__
    if (g_pl.pv) {                      /* RV.EXE's own picture: the eye, the camera, the opening the ground shows in */
        PvCam c;
        int ey0, rows, r;
        const short __far *ext = cockpit_extents(&ey0, &rows);
        long e[3];
        rel(eye, e);
        for (k = 0; k < 3; k++) {
            c.eye[k] = e[k];
            c.m[k] = (int16_t)(f[3 + k] * 64);
            c.m[3 + k] = (int16_t)(f[6 + k] * 64);
            c.m[6 + k] = (int16_t)(f[k] * 64);
            g_pl.eye[k] = e[k];
        }
        c.focal = (int16_t)focal;
        c.cx = (int16_t)cx;
        c.cy = (int16_t)cy;
        c.x0 = 0;
        c.x1 = SCR_W;
        c.y0 = 0;
        c.y1 = SCR_H;
        if (ext && rows > 0) {
            c.x0 = SCR_W;
            c.x1 = 0;
            for (r = 0; r < rows; r++)
                if (ext[2 * r] < ext[2 * r + 1]) {
                    if (ext[2 * r] < c.x0) c.x0 = ext[2 * r];
                    if (ext[2 * r + 1] > c.x1) c.x1 = ext[2 * r + 1];
                }
            c.y0 = (int16_t)ey0;
            c.y1 = (int16_t)(ey0 + rows);
        }
        pv_frame(&c);
        return;
    }
#endif
    if (!g_pl.pic || pic_make(1) != 100 || !g_pl.pic)
        return;
    rel(eye, v.eye);
    for (k = 0; k < 6; k++)
        v.m[k] = (short)(f[3 + k] * 64);
    for (k = 0; k < 3; k++)
        v.m[6 + k] = (short)(f[k] * 64);
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
#ifdef __WATCOMC__
    if (g_pl.pv) {
        pv_band(y0, y1, ext, ext_y0, ext_rows);
        return;
    }
#endif
    if (planet_seen())
        gx_ground(y0, y1, ext, ext_y0, ext_rows);
}

static int hides(long x, long y, long z, int ground)
{
    long q[6];
    q[3] = x - g_pl.centre.x;
    q[4] = y - g_pl.centre.y;
    q[5] = z - g_pl.centre.z;
#ifdef __WATCOMC__
    if (g_pl.pv)                        /* (no card: RV.EXE's own picture's ground) */
        return pv_hides(g_pl.eye, q + 3, ground);
#endif
    (void)ground;
    if (!planet_seen())
        return 0;
    q[0] = g_pl.eye[0];
    q[1] = g_pl.eye[1];
    q[2] = g_pl.eye[2];
    return gx_planet_call(PL_SEEN, q, sizeof q, 0, 0) == 0;
}

/* (no card: a shot's, a missile's, a blast's point is hidden only by the planet's body; a ship's by its ground too) */
int planet_hides_at(long x, long y, long z) { return hides(x, y, z, 0); }
int planet_hides(const LfVec *pos) { return hides(pos->x, pos->y, pos->z, 1); }

long planet_over(const LfVec *pos, long *roof)
{
    BkColumn c;
    long at[3];
    *roof = 0x7FFFFFFFL;
    if (!g_pl.ground)
        return 0x7FFFFFFFL;
    rel(pos, at);
    bk_column((int32_t)at[0], (int32_t)at[1], (int32_t)at[2], &c);
    if (c.slab_lo < c.slab_hi) {        /* a slab: under it, the roof is over the point; in it, the point is in rock */
        if (c.r < c.slab_lo)
            *roof = c.slab_lo - c.r;
        else if (c.r <= c.slab_hi)
            return -1;
    }
    return c.r - c.ground;
}

/* --- a mission's planet (missions.txt planet, ground) --- */

static uint8_t __far g_tunnels;         /* the mission's ground has a tunnel (planet_tunnels) */

void planet_mission(const RvMission __far *m)
{
    int k;
    g_tunnels = 0;
    for (k = 0; k < (m->nground & 15) && k < RV_GROUNDS; k++)
        g_tunnels |= m->ground[k].kind == RV_GROUND_TUNNEL;
    (void)planet_set((const TrPlanetRec __far *)&m->px, (const TrNavRec __far *)m->nav, m->nnav, m->id);
}

int planet_tunnels(void) { return g_tunnels && planet_on(); }

void planet_pace(unsigned ticks)
{
#ifdef __WATCOMC__
    if (g_pl.pv) {
        if (ticks == PL_PACE_REPORT)
            pv_pace_report();
        else
            pv_pace(ticks);
    }
#else
    (void)ticks;
#endif
}

int planet_file(char *name)
{
    int i;
    for (i = 0; i < 12 && g_pl.file[i]; i++)
        name[i] = g_pl.file[i];
    name[i] = 0;
    return g_pl.set && i > 0;
}

void planet_ready(void)
{
    const unsigned char __far *pal = PL_PALETTE_NOW();
    if (!g_pl.set)
        return;
    (void)pic_make(0);
    (void)ground_load(1);
    if (g_pl.pic)
        (void)gx_planet_call(PL_PALETTE, &pal, sizeof pal, 0, 0);
}

/* a planet-relative point put `over` positions over the ground under it; over < 0: as far over the ground as it is
 * over the planet's radius (a ground nav point's record); least: only when it is under that many positions over */
static void ground_put(int32_t v[3], long over, int least)
{
    BkColumn c;
    bk_column(v[0], v[1], v[2], &c);
    if (over < 0)
        over = c.r - bk_ask.radius;
    else if (least && c.r - c.ground >= over)
        return;
    bk_place(v, &c, (int32_t)over);
}

/* a ground nav point is as high over the ground under it as its record has it over the planet's radius */
void planet_navs(void)
{
    int n, k;
    if (!g_pl.ground)
        return;
    for (n = 1; n < 16; n++) {
        uint8_t *rec = navPointTable + navPointTable_SIZE * n;
        if (rec[0] && (rec[1] & RV_NAV_GROUND)) {
            int32_t v[3];
            for (k = 0; k < 3; k++)
                v[k] = (NAV_REC_KM(rec, k) - g_pl.km[k]) * 256;
            ground_put(v, -1, 0);
            for (k = 0; k < 3; k++)
                NAV_REC_KM(rec, k) = v[k] / 256 + g_pl.km[k];
        }
    }
}

void planet_raise(LfVec *pos, long km)
{
    int32_t v[3];
    if (!g_pl.ground)
        return;
    v[0] = (int32_t)(pos->x - g_pl.centre.x);
    v[1] = (int32_t)(pos->y - g_pl.centre.y);
    v[2] = (int32_t)(pos->z - g_pl.centre.z);
    ground_put(v, km * 256, 1);
    pos->x = v[0] + g_pl.centre.x;
    pos->y = v[1] + g_pl.centre.y;
    pos->z = v[2] + g_pl.centre.z;
}
