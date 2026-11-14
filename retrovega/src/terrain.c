/* terrain.c: a planet's ground (terrain.h).  Plain C with C89's declarations, compiled into the sprite accelerator
 * (rvgx_kernel.h, inside its C++) and the native builds, never into RV.EXE.  Its names begin tr_.
 *
 * The ground is made from the seed with integer arithmetic (value noise on a 3D lattice, integer square roots and
 * divisions), so the heights, and what tr_column answers from them, are the same on every machine.  A mission's
 * stamps are shaped with doubles from integers only, each rounded to an integer before it is kept (IEEE doubles add,
 * multiply, divide and take square roots alike on x86-64 and wasm; the 32-bit host build compiles this file with
 * SSE2).  The colours and the picture use floats.
 *
 * The picture is drawn as the old voxel-landscape games drew theirs, a line of the screen at a time, front to back,
 * each line marching out over the ground and filling the pixels its hills rise into beyond what is already filled;
 * here on a sphere, for any way the eye may turn.  A plane through the eye and the planet's centre cuts the ground in
 * one curve and the screen in one line, through the point straight below (or above) the eye as the screen shows it
 * (V, the nadir's vanishing point); so the screen is cut into lines through V, each drawn by marching along the great
 * circle in its plane.  As Ken Silverman's Voxlap does, the screen is split in four by the diagonals through V: in the
 * top and bottom quarters the lines are a pixel apart where they meet the screen's row farthest from V and each keeps
 * a pixel for every screen row; in the left and right ones the same with columns.  A row of the screen then finds
 * its pixels' lines with one addition a pixel (tr_blit).  The second layer, a slab with air under it, is drawn in the
 * same march: what a line has filled is never filled again, so nearer rock hides what is behind it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "terrain.h"

#define TR_N TR_FACE
#define TR_S (TR_N + 2)                 /* a face's grid with a cell of border all round (the next faces' ground) */
#define TR_LEVELS 8                     /* the grid and its halvings, for the far ground: 1024 .. 8 cells a side */
#define TR_TILE 16                      /* the second layer is kept in tiles of 16 x 16 cells, only where there is one */
#define TR_TILES (TR_N / TR_TILE)
#define TR_HU 32                        /* a height unit: 32 positions, an eighth of a km */

#define TR_MATS 4                       /* sand, rock, green, the underside of rock */
#define TR_LIGHTS 64                    /* a material's levels of light: colour id = material * 64 + light */
#define TR_FOGS 32                      /* levels of haze */
#define TR_LINES_MAX 4096
#define TR_FOG_HS_ 1024                 /* (tr_fog's tables' sizes) */
#define TR_FOG_TAUS_ 2048               /* a quarter's lines at most (more: spaced wider than a pixel) */

enum { TR_SAND, TR_ROCK, TR_GREEN, TR_UNDER };

typedef struct {
    int16_t lo[TR_TILE * TR_TILE], hi[TR_TILE * TR_TILE];   /* over the datum, height units; lo >= hi: none */
} TrSlabTile;

typedef struct { uint8_t a, b, t; } TrDither;   /* a colour as two of the palette's: b where the screen's 4 x 4
                                                   threshold is under t (of 16), else a */

typedef struct {                        /* one of the screen's four quarters round V */
    int on;
    int vert;                           /* 1: top or bottom (a pixel for each row); 0: left or right (each column) */
    int s;                              /* -1: top or left (away from V is towards 0), +1: bottom or right */
    double v_along, v_across;           /* V, along the quarter's lines and across them */
    double edge;                        /* the row (column) farthest from V that the lines are spaced along */
    double first, step;                 /* where line 0 crosses it, and the spacing */
    int near;                           /* the row (column) nearest V that is on the screen */
    int lines, len;                     /* lines, and the pixels each keeps */
    uint16_t *px;                       /* lines x len: what each pixel is (TR_PX_*), from `near` outward */
    size_t cap;
} TrQuarter;

#define TR_PX_EMPTY 0xFFFF              /* not reached: the sky as it is */
#define TR_PX_SKY 0x8000                /* | haze level: no ground, the sky's pixel hazed */
                                        /* else colour id | haze level << 8 */

struct TrPlanet {
    TrPlanetDesc desc;
    int32_t radius;                     /* the datum, positions */
    int32_t top, bottom;                /* the highest (a slab's top too) and lowest ground, height units */
    int16_t *h[TR_LEVELS][6];           /* heights over the datum, height units, bordered (tr_side) */
    uint8_t *c[TR_LEVELS][6];           /* colour ids */
    int32_t *tiles[6];                  /* TR_TILES x TR_TILES: an index into slabs + 1, or 0 */
    TrSlabTile *slabs;
    int nslabs, slabs_cap;
    float rgb[256][3];                  /* the colour ids' colours, 0 .. 255 */
    float fog_rgb[3];                   /* the haze's colour */
    double fog_density, fog_height;     /* the haze per position at the datum; its scale height, positions */
    double atmo_top;                    /* where the air is taken to end, over the datum, positions */
    double fog_h0, fog_hstep, fog_e[TR_FOG_HS_], fog_ea;   /* (tr_fog's tables; fog_ea: the frame's eye's) */
    uint8_t fog_lev[TR_FOG_TAUS_];
    double cell;                        /* a level-0 cell's side, about, positions */
    uint8_t pal[256][3];                /* the game's palette, 8 bits */
    float pal_lab[256][3];              /* the same in L*a*b* */
    TrDither ground[TR_FOGS][256];      /* a colour id at a haze level, as the palette's two */
    TrDither sky[TR_FOGS][256];         /* a palette entry at a haze level */
    uint8_t skytab[1024];               /* the frame's: the sky's glow by a ray's angle from straight up (tr_sky_table) */
    /* the frame's picture */
    TrCam cam;
    double vx, vy;                      /* V on the screen */
    int nadir;                          /* 1: V is the point below (the ground lies out from it), 0: above */
    TrQuarter q[4];                     /* top, bottom, left, right */
    uint32_t samples;                   /* (the last frame's march, for timing) */
};

/* --------------------------------------------------------------------------------------------------------------- */
/* integers: the ground made from the seed */

static uint32_t tr_mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

static int32_t tr_lattice(int32_t x, int32_t y, int32_t z, uint32_t seed)   /* -32768 .. 32767 */
{
    return (int32_t)(tr_mix((uint32_t)x * 0x8DA6B343U ^ (uint32_t)y * 0xD8163841U ^ (uint32_t)z * 0xCB1AB31FU ^ seed) >> 16)
           - 32768;
}

static int32_t tr_fade(int32_t f)       /* 0 .. 65536 -> smoothstep, the same scale */
{
    int64_t t = f;
    return (int32_t)(((t * t) >> 16) * (3 * 65536 - 2 * t) >> 16);
}

#define TR_LERP(p, q, t) ((p) + (int32_t)(((int64_t)((q) - (p)) * (t)) >> 16))

/* value noise at a point in 65536ths of a lattice step: -32768 .. 32767, smooth */
static int32_t tr_noise(int32_t x, int32_t y, int32_t z, uint32_t seed)
{
    int32_t ix = x >> 16, iy = y >> 16, iz = z >> 16;
    int32_t fx = tr_fade(x & 0xFFFF), fy = tr_fade(y & 0xFFFF), fz = tr_fade(z & 0xFFFF);
    int32_t a, b, c, d;
    a = TR_LERP(tr_lattice(ix, iy, iz, seed), tr_lattice(ix + 1, iy, iz, seed), fx);
    b = TR_LERP(tr_lattice(ix, iy + 1, iz, seed), tr_lattice(ix + 1, iy + 1, iz, seed), fx);
    c = TR_LERP(tr_lattice(ix, iy, iz + 1, seed), tr_lattice(ix + 1, iy, iz + 1, seed), fx);
    d = TR_LERP(tr_lattice(ix, iy + 1, iz + 1, seed), tr_lattice(ix + 1, iy + 1, iz + 1, seed), fx);
    a = TR_LERP(a, b, fy);
    c = TR_LERP(c, d, fy);
    return TR_LERP(a, c, fz);
}

/* octaves of it at a direction (65536 = 1), freq lattice steps over the planet's radius: about -32768 .. 32767 */
static int32_t tr_fbm(const int32_t d[3], int32_t freq, int octaves, uint32_t seed)
{
    int32_t acc = 0, amp = 16384, total = 0;
    int o;
    for (o = 0; o < octaves; o++) {
        acc += (int32_t)(((int64_t)tr_noise(d[0] * freq, d[1] * freq, d[2] * freq, seed + (uint32_t)o * 0x9E3779B9U) * amp) >> 14);
        total += amp;
        amp >>= 1;
        freq <<= 1;
    }
    return (int32_t)(((int64_t)acc << 14) / total);
}

static uint32_t tr_isqrt(uint64_t n)
{
    uint64_t r = (uint64_t)sqrt((double)n);
    while (r * r > n)
        r--;
    while ((r + 1) * (r + 1) <= n)
        r++;
    return (uint32_t)r;
}

static int32_t tr_smooth(int32_t a, int32_t b, int32_t x)   /* 0 at a .. 65536 at b, eased (a < b) */
{
    if (x <= a)
        return 0;
    if (x >= b)
        return 65536;
    return tr_fade((int32_t)(((int64_t)(x - a) << 16) / (b - a)));
}

static int32_t tr_mul(int32_t a, int32_t f) { return (int32_t)(((int64_t)a * f) >> 16); }   /* a times f / 65536 */

/* the faces: face f's middle is on axis f >> 1, + for even f; a cell's u runs along axis (a + 1) % 3, v along
 * (a + 2) % 3.  A level's grid is n = TR_N >> level cells a side, kept with a border: tr_side */
static int tr_side(int level) { return (TR_N >> level) + 2; }

/* a cell's direction, not unit: integers (face f, cell i, j of n a side, -1 .. n with the border) */
static void tr_cell_vec(int f, int i, int j, int n, int64_t v[3])
{
    int a = f >> 1;
    v[a] = (f & 1) ? -n : n;
    v[(a + 1) % 3] = 2 * i + 1 - n;
    v[(a + 2) % 3] = 2 * j + 1 - n;
}

/* where a direction lands: its face, and its place on the face's grid of n cells, in 65536ths of a cell (cell i's
 * middle at i * 65536) */
static int tr_locate(int64_t x, int64_t y, int64_t z, int n, int32_t *gi, int32_t *gj)
{
    int64_t c[3], m;
    int a, f;
    c[0] = x;
    c[1] = y;
    c[2] = z;
    a = 0;
    if ((c[1] < 0 ? -c[1] : c[1]) > (c[a] < 0 ? -c[a] : c[a]))
        a = 1;
    if ((c[2] < 0 ? -c[2] : c[2]) > (c[a] < 0 ? -c[a] : c[a]))
        a = 2;
    m = c[a] < 0 ? -c[a] : c[a];
    f = 2 * a + (c[a] < 0);
    if (m == 0) {
        *gi = *gj = 0;
        return 0;
    }
    /* u = c[b] / m, the cell (u * n + n - 1) / 2 (positions under 2^31, n at most 2^10: the product under 2^58) */
    *gi = (int32_t)(((c[(a + 1) % 3] * n + m * (n - 1)) * 32768) / m);
    *gj = (int32_t)(((c[(a + 2) % 3] * n + m * (n - 1)) * 32768) / m);
    return f;
}

/* --- the desert (the user: "a simple desert planet with some greenery sections") --- */

typedef struct {
    int32_t h;                          /* over the datum, height units */
    uint8_t mat;
} TrGround;

static TrGround tr_desert(const int32_t d[3], int32_t relief, uint32_t seed)
{
    TrGround g;
    int32_t basin = tr_fbm(d, 2, 4, seed ^ 0xA5170001U);             /* broad highs and lows */
    int32_t ridge = tr_fbm(d, 9, 5, seed ^ 0xA5170002U);             /* mountains */
    int32_t mesa = tr_fbm(d, 5, 3, seed ^ 0xA5170003U);              /* table lands */
    int32_t wet = tr_fbm(d, 4, 3, seed ^ 0xA5170004U);               /* where the green grows */
    int32_t fine = tr_fbm(d, 96, 3, seed ^ 0xA5170005U);             /* rubble */
    int32_t mount = tr_smooth(4000, 18000, basin);                     /* mountains where the land is high */
    int32_t table = tr_smooth(6000, 9000, mesa);                       /* the mesas' steep sides */
    int32_t sand = 65536 - (mount > table ? mount : table);
    int32_t ridged = 32767 - 2 * (ridge < 0 ? -ridge : ridge);       /* sharp crests where the noise crosses 0 */
    int32_t dune, h;
    int64_t phase;
    h = tr_mul(relief / 4, basin * 2);                                 /* +- a quarter */
    h += tr_mul(tr_mul(relief, (ridged + 32768) * 2 / 3), mount);
    h += tr_mul(relief / 6, table);                                    /* a mesa stands a sixth high, flat on top */
    /* dunes: long ridges across a wind, bent by the noise, only on the sand */
    phase = ((int64_t)d[0] * 70 + (int64_t)d[1] * 43 + (int64_t)basin * 3 + (int64_t)wet * 2) & 0xFFFF;
    dune = (int32_t)(phase < 0x8000 ? phase : 0xFFFF - phase) * 2;    /* a triangle wave 0 .. 65535 */
    dune = tr_mul(dune, dune);
    h += tr_mul(tr_mul(relief / 60, dune), sand);
    h += tr_mul(relief / 200, fine * 2);
    g.mat = TR_SAND;
    if (mount > 30000 || table > 30000)
        g.mat = TR_ROCK;
    else if (basin < -2000 && wet > 6000)
        g.mat = TR_GREEN;                                              /* the green, low and wet */
    if (g.mat == TR_GREEN)
        h -= tr_mul(relief / 40, tr_smooth(6000, 14000, wet));         /* a little lower: the water table */
    g.h = h;
    return g;
}

/* --------------------------------------------------------------------------------------------------------------- */

int32_t tr_radius(const TrPlanet *p) { return p->radius; }
int32_t tr_top(const TrPlanet *p) { return p->radius + p->top * TR_HU; }

static int16_t *tr_hp(const TrPlanet *p, int level, int f, int i, int j)   /* (i, j: -1 .. n, the border included) */
{
    return p->h[level][f] + (j + 1) * tr_side(level) + (i + 1);
}

static uint8_t *tr_cp(const TrPlanet *p, int level, int f, int i, int j)
{
    return p->c[level][f] + (j + 1) * tr_side(level) + (i + 1);
}

static TrSlabTile *tr_tile(const TrPlanet *p, int f, int i, int j)   /* (interior cells only) */
{
    int32_t k = p->tiles[f][(j / TR_TILE) * TR_TILES + i / TR_TILE];
    return k ? &p->slabs[k - 1] : 0;
}

static TrSlabTile *tr_tile_make(TrPlanet *p, int f, int i, int j)
{
    int32_t *k = &p->tiles[f][(j / TR_TILE) * TR_TILES + i / TR_TILE];
    if (!*k) {
        if (p->nslabs == p->slabs_cap) {
            int cap = p->slabs_cap ? p->slabs_cap * 2 : 64;
            TrSlabTile *more = (TrSlabTile *)realloc(p->slabs, (size_t)cap * sizeof *more);
            if (!more)
                return 0;
            p->slabs = more;
            p->slabs_cap = cap;
        }
        memset(&p->slabs[p->nslabs], 0, sizeof p->slabs[0]);
        *k = ++p->nslabs;
    }
    return &p->slabs[*k - 1];
}

/* the slab over cell (i, j) of face f, level 0: 1 and its underside and top when there is one */
static int tr_slab(const TrPlanet *p, int f, int i, int j, int *lo, int *hi)
{
    const TrSlabTile *t;
    int k;
    if (i < 0 || j < 0 || i >= TR_N || j >= TR_N)
        return 0;
    t = tr_tile(p, f, i, j);
    if (!t)
        return 0;
    k = (j % TR_TILE) * TR_TILE + i % TR_TILE;
    if (t->lo[k] >= t->hi[k])
        return 0;
    *lo = t->lo[k];
    *hi = t->hi[k];
    return 1;
}

/* the ground's height at a place on face f's level-0 grid (65536ths of a cell), in positions over the datum, the four
 * cells round it weighed */
static int32_t tr_height_at(const TrPlanet *p, int f, int32_t gi, int32_t gj)
{
    int32_t i = gi >> 16, j = gj >> 16, fi = gi & 0xFFFF, fj = gj & 0xFFFF, a, b;
    const int16_t *r;
    if (i < -1) { i = -1; fi = 0; }
    if (j < -1) { j = -1; fj = 0; }
    if (i > TR_N - 1) { i = TR_N - 1; fi = 65535; }
    if (j > TR_N - 1) { j = TR_N - 1; fj = 65535; }
    r = tr_hp(p, 0, f, i, j);
    a = TR_LERP(r[0] * TR_HU, r[1] * TR_HU, fi);
    b = TR_LERP(r[TR_S] * TR_HU, r[TR_S + 1] * TR_HU, fi);
    return TR_LERP(a, b, fj);
}

void tr_column(const TrPlanet *p, int32_t x, int32_t y, int32_t z, TrColumn *out)
{
    int32_t gi, gj, ci, cj;
    int f = tr_locate(x, y, z, TR_N, &gi, &gj), lo, hi;
    out->ground = p->radius + tr_height_at(p, f, gi, gj);
    out->slab_lo = out->slab_hi = 0;
    ci = (gi + 32768) >> 16;
    cj = (gj + 32768) >> 16;
    if (tr_slab(p, f, ci, cj, &lo, &hi)) {
        out->slab_lo = p->radius + lo * TR_HU;
        out->slab_hi = p->radius + hi * TR_HU;
    }
}

/* --- the missions' stamps --- */

typedef struct {                        /* a stamp's frame on the ground: its middle and two ways along it, unit */
    double c[3], a[3], b[3];
} TrFrame;

static void tr_unit(double v[3])
{
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static void tr_frame(const TrStamp *s, TrFrame *fr)
{
    double d;
    int k;
    for (k = 0; k < 3; k++) {
        fr->c[k] = (double)s->at[k];
        fr->a[k] = (double)s->to[k] - (double)s->at[k];
    }
    tr_unit(fr->c);
    d = fr->a[0] * fr->c[0] + fr->a[1] * fr->c[1] + fr->a[2] * fr->c[2];
    for (k = 0; k < 3; k++)
        fr->a[k] -= d * fr->c[k];
    tr_unit(fr->a);
    fr->b[0] = fr->c[1] * fr->a[2] - fr->c[2] * fr->a[1];
    fr->b[1] = fr->c[2] * fr->a[0] - fr->c[0] * fr->a[2];
    fr->b[2] = fr->c[0] * fr->a[1] - fr->c[1] * fr->a[0];
}

/* a cell's place in a stamp's frame, positions along a and b over the ground (on the tangent plane: stamps are small) */
static void tr_in_frame(const TrFrame *fr, const int64_t v[3], int32_t radius, double *x, double *y)
{
    double u[3], s;
    int k;
    for (k = 0; k < 3; k++)
        u[k] = (double)v[k];
    tr_unit(u);
    s = u[0] * fr->c[0] + u[1] * fr->c[1] + u[2] * fr->c[2];
    if (s <= 0.5) {
        *x = *y = 1e30;
        return;
    }
    *x = (u[0] * fr->a[0] + u[1] * fr->a[1] + u[2] * fr->a[2]) / s * radius;
    *y = (u[0] * fr->b[0] + u[1] * fr->b[1] + u[2] * fr->b[2]) / s * radius;
}

/* the cells a stamp may touch: those within `reach` positions of its middle (a box on each face; a face it does not
 * reach is skipped) */
static int tr_stamp_box(const TrPlanet *p, const TrFrame *fr, int f, double reach, int *i0, int *j0, int *i1, int *j1)
{
    int a = f >> 1, b = (a + 1) % 3, c = (a + 2) % 3;
    double sa = (f & 1) ? -fr->c[a] : fr->c[a], ang = reach / p->radius + 2.0 * 2.0 / TR_N, u, v, du;
    if (sa <= 0.2)
        return 0;
    u = fr->c[b] / sa;
    v = fr->c[c] / sa;
    du = ang * (1 + u * u + v * v) * 1.5 + 4.0 / TR_N;
    *i0 = (int)floor(((u - du) * TR_N + TR_N - 1) / 2) - 1;
    *i1 = (int)ceil(((u + du) * TR_N + TR_N - 1) / 2) + 1;
    *j0 = (int)floor(((v - du) * TR_N + TR_N - 1) / 2) - 1;
    *j1 = (int)ceil(((v + du) * TR_N + TR_N - 1) / 2) + 1;
    if (*i0 < -1) *i0 = -1;
    if (*j0 < -1) *j0 = -1;
    if (*i1 > TR_N) *i1 = TR_N;
    if (*j1 > TR_N) *j1 = TR_N;
    return *i0 <= *i1 && *j0 <= *j1;
}

static void tr_stamp_pad(TrPlanet *p, const TrStamp *s)
{
    TrFrame fr;
    int f, i, j, i0, j0, i1, j1;
    int32_t gi, gj;
    double r = (double)s->size[0], slope = s->size[1] > 0 ? (double)s->size[1] : 1.0, mid;
    tr_frame(s, &fr);
    f = tr_locate(s->at[0], s->at[1], s->at[2], TR_N, &gi, &gj);
    mid = (double)(tr_height_at(p, f, gi, gj) + s->size[2]) / TR_HU;
    for (f = 0; f < 6; f++) {
        if (!tr_stamp_box(p, &fr, f, r + slope, &i0, &j0, &i1, &j1))
            continue;
        for (j = j0; j <= j1; j++)
            for (i = i0; i <= i1; i++) {
                int64_t v[3];
                double x, y, d, w;
                int16_t *h;
                tr_cell_vec(f, i, j, TR_N, v);
                tr_in_frame(&fr, v, p->radius, &x, &y);
                d = sqrt(x * x + y * y);
                if (d >= r + slope)
                    continue;
                w = d <= r ? 1.0 : 1.0 - (d - r) / slope;
                w = w * w * (3 - 2 * w);
                h = tr_hp(p, 0, f, i, j);
                *h = (int16_t)floor(*h + (mid - *h) * w + 0.5);
            }
    }
}

static void tr_stamp_arch(TrPlanet *p, const TrStamp *s)
{
    TrFrame fr;
    int f, i, j, i0, j0, i1, j1;
    int32_t gi, gj;
    double span = (double)s->size[0], half_w = (double)s->size[1] / 2, air = (double)s->size[2] / TR_HU;
    double thick = (double)s->size[3] / TR_HU, outer = span * (1 + thick / (air > 1 ? air : 1)), base;
    tr_frame(s, &fr);
    f = tr_locate(s->at[0], s->at[1], s->at[2], TR_N, &gi, &gj);
    base = (double)tr_height_at(p, f, gi, gj) / TR_HU;
    for (f = 0; f < 6; f++) {
        if (!tr_stamp_box(p, &fr, f, outer + half_w, &i0, &j0, &i1, &j1))
            continue;
        for (j = j0; j <= j1; j++)
            for (i = i0; i <= i1; i++) {
                int64_t v[3];
                double x, y, ax, ay, lo, hi, e;
                int16_t *h;
                tr_cell_vec(f, i, j, TR_N, v);
                tr_in_frame(&fr, v, p->radius, &x, &y);
                ax = fabs(x);
                ay = fabs(y);
                if (ax >= outer || ay >= half_w)
                    continue;
                /* the rock between two half ellipses: inside, `span` out and `air` up; outside, a thickness more */
                e = 1 - (ax / outer) * (ax / outer);
                hi = base + (air + thick) * sqrt(e > 0 ? e : 0);
                hi -= thick * 0.4 * (ay / half_w) * (ay / half_w);   /* rounded across */
                h = tr_hp(p, 0, f, i, j);
                if (ax < span) {
                    e = 1 - (ax / span) * (ax / span);
                    lo = base + air * sqrt(e > 0 ? e : 0);
                    if (lo < *h + 1)
                        lo = *h + 1;
                    if (i >= 0 && j >= 0 && i < TR_N && j < TR_N && hi > lo + 2) {
                        TrSlabTile *t = tr_tile_make(p, f, i, j);
                        int k = (j % TR_TILE) * TR_TILE + i % TR_TILE;
                        if (t) {
                            t->lo[k] = (int16_t)floor(lo + 0.5);
                            t->hi[k] = (int16_t)floor(hi + 0.5);
                        }
                    }
                } else if (hi > *h)
                    *h = (int16_t)floor(hi + 0.5);   /* a foot: solid down to the ground */
            }
    }
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the colours (floats: only the picture uses them) */

static const float tr_mat_rgb[TR_MATS][3] = {
    { 228, 190, 134 },                  /* sand */
    { 178, 104, 66 },                   /* rock, red */
    { 112, 138, 60 },                   /* green */
    { 92, 62, 46 }                      /* the underside of rock */
};

static void tr_light(TrPlanet *p)
{
    static const double sun0[3] = { 0.48, 0.74, 0.47 };
    double sun[3];
    int f, i, j;
    sun[0] = sun0[0];
    sun[1] = sun0[1];
    sun[2] = sun0[2];
    tr_unit(sun);
    for (f = 0; f < 6; f++) {
        int a = f >> 1, b = (a + 1) % 3, c = (a + 2) % 3;
        for (j = -1; j <= TR_N; j++)
            for (i = -1; i <= TR_N; i++) {
                int64_t vi[3];
                double up[3], ei[3], ej[3], n[3], l[3], d, u, v, si, sj, gx, gy, shade, lev;
                int k, mat, il = i > -1 ? i - 1 : i, ir = i < TR_N ? i + 1 : i, jl = j > -1 ? j - 1 : j, jr = j < TR_N ? j + 1 : j;
                uint8_t *cp = tr_cp(p, 0, f, i, j);
                tr_cell_vec(f, i, j, TR_N, vi);
                for (k = 0; k < 3; k++)
                    up[k] = (double)vi[k];
                tr_unit(up);
                u = (2.0 * i + 1 - TR_N) / TR_N;
                v = (2.0 * j + 1 - TR_N) / TR_N;
                si = p->radius * (2.0 / TR_N) * sqrt(1 + v * v) / (1 + u * u + v * v);
                sj = p->radius * (2.0 / TR_N) * sqrt(1 + u * u) / (1 + u * u + v * v);
                gx = (double)(*tr_hp(p, 0, f, ir, j) - *tr_hp(p, 0, f, il, j)) * TR_HU / (si * (ir - il));
                gy = (double)(*tr_hp(p, 0, f, i, jr) - *tr_hp(p, 0, f, i, jl)) * TR_HU / (sj * (jr - jl));
                for (k = 0; k < 3; k++) {
                    ei[k] = (k == b) - up[k] * up[b];
                    ej[k] = (k == c) - up[k] * up[c];
                }
                tr_unit(ei);
                tr_unit(ej);
                /* the light: always from above, leaning the way the sun lies (no night side) */
                d = sun[0] * up[0] + sun[1] * up[1] + sun[2] * up[2];
                for (k = 0; k < 3; k++) {
                    n[k] = up[k] - gx * ei[k] - gy * ej[k];
                    l[k] = up[k] + 0.9 * (sun[k] - d * up[k]);
                }
                tr_unit(n);
                tr_unit(l);
                shade = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
                mat = *cp;
                if (mat == TR_SAND && gx * gx + gy * gy > 0.7)
                    mat = TR_ROCK;              /* too steep for sand */
                lev = 0.28 + 0.9 * (shade > 0 ? shade : 0);
                lev = (lev - 0.2) / 1.0 * (TR_LIGHTS - 1);
                lev += (double)((tr_mix((uint32_t)(f * 1100000 + (j + 1) * 1030 + i + 1)) & 7)) - 3.5;
                k = (int)lev;
                if (k < 0) k = 0;
                if (k > TR_LIGHTS - 1) k = TR_LIGHTS - 1;
                *cp = (uint8_t)(mat * TR_LIGHTS + k);
            }
    }
    for (i = 0; i < 256; i++) {
        int mat = i / TR_LIGHTS, k;
        double b = 0.2 + (double)(i % TR_LIGHTS) / (TR_LIGHTS - 1);
        for (k = 0; k < 3; k++) {
            double x = tr_mat_rgb[mat][k] * b;
            /* shadows a little bluer, the light a little warmer */
            x = x * (0.85 + 0.15 * b) + (k == 2 ? 14 : 4) * (1 - b);
            p->rgb[i][k] = (float)(x > 255 ? 255 : x);
        }
    }
}

/* the halvings of the grid for the far ground.  The march draws the second layer as itself down to level
 * TR_SLAB_LEVELS - 1; past that a slab counts as solid rock to its top */
#define TR_SLAB_LEVELS 3

static void tr_mips(TrPlanet *p)
{
    int l, f, i, j;
    for (l = 1; l < TR_LEVELS; l++) {
        int n = TR_N >> l;
        for (f = 0; f < 6; f++)
            for (j = 0; j < n; j++)
                for (i = 0; i < n; i++) {
                    int sum = 0, light = 0, k, lo, hi;
                    for (k = 0; k < 4; k++) {
                        int ci = 2 * i + (k & 1), cj = 2 * j + (k >> 1), h = *tr_hp(p, l - 1, f, ci, cj);
                        if (l == TR_SLAB_LEVELS) {     /* the level-0 cells under this one: their slabs' tops */
                            int a, b, sh = TR_SLAB_LEVELS - 1;
                            for (b = cj << sh; b < (cj + 1) << sh; b++)
                                for (a = ci << sh; a < (ci + 1) << sh; a++)
                                    if (tr_slab(p, f, a, b, &lo, &hi) && hi > h)
                                        h = hi;
                        }
                        sum += h;
                        light += *tr_cp(p, l - 1, f, ci, cj) % TR_LIGHTS;
                    }
                    *tr_hp(p, l, f, i, j) = (int16_t)(sum >> 2);
                    *tr_cp(p, l, f, i, j) = (uint8_t)(*tr_cp(p, l - 1, f, 2 * i, 2 * j) / TR_LIGHTS * TR_LIGHTS + light / 4);
                }
        /* the border: each cell from the face it really lies on */
        for (f = 0; f < 6; f++)
            for (j = -1; j <= n; j++)
                for (i = -1; i <= n; i++) {
                    int64_t v[3];
                    int32_t gi, gj;
                    int f2, ci, cj;
                    if (i >= 0 && j >= 0 && i < n && j < n)
                        continue;
                    tr_cell_vec(f, i, j, n, v);
                    f2 = tr_locate(v[0], v[1], v[2], n, &gi, &gj);
                    ci = (gi + 32768) >> 16;
                    cj = (gj + 32768) >> 16;
                    if (ci < 0) ci = 0;
                    if (cj < 0) cj = 0;
                    if (ci > n - 1) ci = n - 1;
                    if (cj > n - 1) cj = n - 1;
                    *tr_hp(p, l, f, i, j) = *tr_hp(p, l, f2, ci, cj);
                    *tr_cp(p, l, f, i, j) = *tr_cp(p, l, f2, ci, cj);
                }
    }
}

/* colours compared as the eye does: CIE L*a*b* (from sRGB), so a darkened tan finds a darker tan, not a grey */
static void tr_lab(const float rgb[3], float lab[3])
{
    double l[3], x, y, z;
    int k;
    for (k = 0; k < 3; k++) {
        double c = rgb[k] / 255.0;
        l[k] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    }
    x = (0.4124 * l[0] + 0.3576 * l[1] + 0.1805 * l[2]) / 0.95047;
    y = 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
    z = (0.0193 * l[0] + 0.1192 * l[1] + 0.9505 * l[2]) / 1.08883;
    x = x > 0.008856 ? pow(x, 1 / 3.0) : 7.787 * x + 16 / 116.0;
    y = y > 0.008856 ? pow(y, 1 / 3.0) : 7.787 * y + 16 / 116.0;
    z = z > 0.008856 ? pow(z, 1 / 3.0) : 7.787 * z + 16 / 116.0;
    lab[0] = (float)(116 * y - 16);
    lab[1] = (float)(500 * (x - y));
    lab[2] = (float)(200 * (y - z));
}

static double tr_d2(const float a[3], const float b[3])
{
    double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

/* the palette's entries a picture may use: the ones every mission has the same (the cockpit's own are 208 on, 255
 * flashes in mine fields, 1 .. 15 are the displays') */
#define TR_PAL_FIRST 16
#define TR_PAL_END 208

static int tr_nearest(const TrPlanet *p, const float lab[3])
{
    int i, best = TR_PAL_FIRST;
    double bd = 1e30;
    for (i = TR_PAL_FIRST; i < TR_PAL_END; i++) {
        double d = tr_d2(lab, p->pal_lab[i]);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* the best pair for a colour: the nearest entry, and the nearest to the colour's mirror image past it, mixed by the
 * share that comes closest (in L*a*b*) */
static TrDither tr_dither(const TrPlanet *p, const float rgb[3])
{
    TrDither r;
    float t[3], m[3];
    int k, a, b, best = 0;
    double bd;
    tr_lab(rgb, t);
    a = tr_nearest(p, t);
    for (k = 0; k < 3; k++)
        m[k] = 2 * t[k] - p->pal_lab[a][k];
    b = tr_nearest(p, m);
    r.a = (uint8_t)a;
    r.b = (uint8_t)b;
    r.t = 0;
    if (b == a)
        return r;
    bd = tr_d2(t, p->pal_lab[a]);
    for (k = 1; k <= 16; k++) {
        float mix[3], ml[3];
        int i;
        double d;
        for (i = 0; i < 3; i++)
            mix[i] = (float)(p->pal[a][i] + (p->pal[b][i] - p->pal[a][i]) * k / 16.0);
        tr_lab(mix, ml);
        d = tr_d2(t, ml);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    r.t = (uint8_t)best;
    return r;
}

static void tr_tables(TrPlanet *p)
{
    int fog, i, k;
    for (fog = 0; fog < TR_FOGS; fog++) {
        double w = (double)fog / (TR_FOGS - 1);
        for (i = 0; i < 256; i++) {
            float t[3];
            for (k = 0; k < 3; k++)
                t[k] = (float)(p->rgb[i][k] + (p->fog_rgb[k] - p->rgb[i][k]) * w);
            p->ground[fog][i] = tr_dither(p, t);
            for (k = 0; k < 3; k++)
                t[k] = (float)(p->pal[i][k] + (p->fog_rgb[k] - p->pal[i][k]) * w);
            p->sky[fog][i] = tr_dither(p, t);
        }
    }
}

static void tr_fog_tables(TrPlanet *p);

TrPlanet *tr_make(const TrPlanetDesc *d, const TrStamp *stamps, const uint8_t *pal)
{
    TrPlanet *p = (TrPlanet *)calloc(1, sizeof *p);
    int l, f, i, j, k;
    int32_t relief;
    if (!p)
        return 0;
    p->desc = *d;
    p->radius = d->radius;
    for (l = 0; l < TR_LEVELS; l++)
        for (f = 0; f < 6; f++) {
            p->h[l][f] = (int16_t *)malloc((size_t)tr_side(l) * tr_side(l) * sizeof(int16_t));
            p->c[l][f] = (uint8_t *)malloc((size_t)tr_side(l) * tr_side(l));
            if (!p->h[l][f] || !p->c[l][f]) {
                tr_free(p);
                return 0;
            }
        }
    for (f = 0; f < 6; f++) {
        p->tiles[f] = (int32_t *)calloc((size_t)TR_TILES * TR_TILES, sizeof(int32_t));
        if (!p->tiles[f]) {
            tr_free(p);
            return 0;
        }
    }
    relief = d->radius / 16 / TR_HU;    /* the highest mountain over the lowest basin, about: a sixteenth of the radius */
    if (relief > 30000)
        relief = 30000;
    for (j = -1; j <= TR_N; j++)
        for (i = -1; i <= TR_N; i++) {
            int64_t a = 2 * i + 1 - TR_N, b = 2 * j + 1 - TR_N;
            uint32_t len = tr_isqrt((uint64_t)((int64_t)TR_N * TR_N + a * a + b * b));
            for (f = 0; f < 6; f++) {
                int64_t v[3];
                int32_t dir[3];
                TrGround g;
                tr_cell_vec(f, i, j, TR_N, v);
                for (k = 0; k < 3; k++)
                    dir[k] = (int32_t)((v[k] * 65536) / (int64_t)len);
                g = tr_desert(dir, relief, d->seed);
                if (g.h > 32000) g.h = 32000;
                if (g.h < -32000) g.h = -32000;
                *tr_hp(p, 0, f, i, j) = (int16_t)g.h;
                *tr_cp(p, 0, f, i, j) = g.mat;
            }
        }
    for (k = 0; k < d->nstamps && k < TR_STAMPS_MAX; k++)
        switch (stamps[k].kind) {
        case TR_STAMP_PAD:
            tr_stamp_pad(p, &stamps[k]);
            break;
        case TR_STAMP_ARCH:
            tr_stamp_arch(p, &stamps[k]);
            break;
        }
    p->top = -32768;
    p->bottom = 32767;
    for (f = 0; f < 6; f++)
        for (j = 0; j < TR_N; j++)
            for (i = 0; i < TR_N; i++) {
                int h = *tr_hp(p, 0, f, i, j), lo, hi;
                if (tr_slab(p, f, i, j, &lo, &hi) && hi > h)
                    h = hi;
                if (h > p->top) p->top = h;
                if (*tr_hp(p, 0, f, i, j) < p->bottom) p->bottom = *tr_hp(p, 0, f, i, j);
            }
    p->cell = 2.0 * 3.14159265 * p->radius / 4 / TR_N;
    /* the air: a warm haze (the user: "atmospheric fog effect that depends on height to camera") */
    p->fog_rgb[0] = 222;
    p->fog_rgb[1] = 184;
    p->fog_rgb[2] = 142;
    p->fog_height = relief * TR_HU / 3.5;
    p->fog_density = 0.35 / p->fog_height;   /* straight up from the datum, a third of the way to thick haze */
    p->atmo_top = p->fog_height * 7;
    for (i = 0; i < 256; i++)
        for (k = 0; k < 3; k++)
            p->pal[i][k] = pal ? (uint8_t)(pal[3 * i + k] * 255 / 63) : (uint8_t)i;
    for (i = 0; i < 256; i++) {
        float c[3];
        for (k = 0; k < 3; k++)
            c[k] = p->pal[i][k];
        tr_lab(c, p->pal_lab[i]);
    }
    tr_fog_tables(p);
    tr_light(p);
    tr_mips(p);
    tr_tables(p);
    return p;
}

void tr_free(TrPlanet *p)
{
    int l, f;
    if (!p)
        return;
    for (l = 0; l < TR_LEVELS; l++)
        for (f = 0; f < 6; f++) {
            free(p->h[l][f]);
            free(p->c[l][f]);
        }
    for (f = 0; f < 6; f++)
        free(p->tiles[f]);
    free(p->slabs);
    for (l = 0; l < 4; l++)
        free(p->q[l].px);
    free(p);
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the picture */

typedef struct { double x, y, z; } TrV;

static TrV tr_v(double x, double y, double z) { TrV v; v.x = x; v.y = y; v.z = z; return v; }
static double tr_dot(TrV a, TrV b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static TrV tr_add(TrV a, TrV b, double k) { return tr_v(a.x + b.x * k, a.y + b.y * k, a.z + b.z * k); }
static TrV tr_scale(TrV a, double k) { return tr_v(a.x * k, a.y * k, a.z * k); }
static TrV tr_norm(TrV a)
{
    double l = sqrt(tr_dot(a, a));
    return l > 0 ? tr_v(a.x / l, a.y / l, a.z / l) : a;
}

/* a sample of the ground at a unit direction, from the grid of a level: its height over the datum (positions) and
 * its colour id, the light weighed between the four cells round it where they are of one material, and at level 0 a
 * finer grain of light inside a cell (the picture only); *f, *ci, *cj the nearest cell, in that level's numbering */
static double tr_sample(const TrPlanet *p, TrV d, int level, uint8_t *id, int *f, int *ci, int *cj)
{
    double ax = fabs(d.x), ay = fabs(d.y), az = fabs(d.z), m, u, v, gi, gj, fi, fj;
    int a, n = TR_N >> level, i, j, side = tr_side(level);
    const int16_t *h;
    const uint8_t *c;
    if (ax >= ay && ax >= az) {
        a = 0;
        m = ax;
    } else if (ay >= az) {
        a = 1;
        m = ay;
    } else {
        a = 2;
        m = az;
    }
    *f = 2 * a + ((a == 0 ? d.x : a == 1 ? d.y : d.z) < 0);
    u = (a == 0 ? d.y : a == 1 ? d.z : d.x) / m;
    v = (a == 0 ? d.z : a == 1 ? d.x : d.y) / m;
    gi = (u * n + n - 1) * 0.5;
    gj = (v * n + n - 1) * 0.5;
    i = (int)(gi + 2) - 2;              /* (floor: gi is over -2) */
    j = (int)(gj + 2) - 2;
    fi = gi - i;
    fj = gj - j;
    if (i < -1) { i = -1; fi = 0; }
    if (j < -1) { j = -1; fj = 0; }
    if (i > n - 1) { i = n - 1; fi = 1; }
    if (j > n - 1) { j = n - 1; fj = 1; }
    h = p->h[level][*f] + (j + 1) * side + (i + 1);
    c = p->c[level][*f] + (j + 1) * side + (i + 1);
    *ci = fi < 0.5 ? i : i + 1;
    *cj = fj < 0.5 ? j : j + 1;
    if (c[0] / TR_LIGHTS == c[1] / TR_LIGHTS && c[0] / TR_LIGHTS == c[side] / TR_LIGHTS && c[0] / TR_LIGHTS == c[side + 1] / TR_LIGHTS) {
        double l = ((c[0] % TR_LIGHTS) * (1 - fi) + (c[1] % TR_LIGHTS) * fi) * (1 - fj)
                   + ((c[side] % TR_LIGHTS) * (1 - fi) + (c[side + 1] % TR_LIGHTS) * fi) * fj;
        int k;
        if (level == 0) {
            /* grain: a quarter of a cell's light flecks, fixed to the ground */
            uint32_t g = tr_mix((uint32_t)(*f * 0x10000 + (int)(gj * 4 + 8)) * 0x9E3779B1U + (uint32_t)(int)(gi * 4 + 8));
            l += (double)(g & 7) * 0.6 - 2.1;
        }
        k = (int)(l + 0.5);
        *id = (uint8_t)(c[0] / TR_LIGHTS * TR_LIGHTS + (k < 0 ? 0 : k > TR_LIGHTS - 1 ? TR_LIGHTS - 1 : k));
    } else
        *id = c[(*cj - j) * side + (*ci - i)];
    return ((h[0] * (1 - fi) + h[1] * fi) * (1 - fj) + (h[side] * (1 - fi) + h[side + 1] * fi) * fj) * TR_HU;
}

/* the haze between the eye and a point: a level 0 .. TR_FOGS-1.  The air thins with height as e^(-h/H); along a
 * straight line from height a to height b its sum is L * (e^(-a/H) - e^(-b/H)) / ((b - a) / H) (the planet taken as
 * flat for it).  From tables: e^(-h/H) over the ground's heights, and the level by that sum (a frame's eye's own
 * e^(-a/H) is worked out once, in tr_view) */
#define TR_FOG_HS TR_FOG_HS_             /* e^(-h/H) at heights bottom .. top */
#define TR_FOG_TAUS TR_FOG_TAUS_         /* the level by the sum, 256 steps to each 1 */

static void tr_fog_tables(TrPlanet *p)
{
    int k;
    p->fog_h0 = p->bottom * TR_HU;
    p->fog_hstep = (double)(p->top - p->bottom + 1) * TR_HU / (TR_FOG_HS - 1);
    for (k = 0; k < TR_FOG_HS; k++)
        p->fog_e[k] = exp(-(p->fog_h0 + k * p->fog_hstep) / p->fog_height);
    for (k = 0; k < TR_FOG_TAUS; k++) {
        int lev = (int)((1 - exp(-(k + 0.5) / 256.0)) * (TR_FOGS - 1) + 0.5);
        p->fog_lev[k] = (uint8_t)(lev > TR_FOGS - 1 ? TR_FOGS - 1 : lev);
    }
}

static int tr_fog(const TrPlanet *p, double dist, double ha, double hb)
{
    double x = (hb - ha) / p->fog_height, eb, tau;
    int k = (int)((hb - p->fog_h0) / p->fog_hstep + 0.5);
    eb = p->fog_e[k < 0 ? 0 : k > TR_FOG_HS - 1 ? TR_FOG_HS - 1 : k];
    if (fabs(x) < 1e-3)
        tau = p->fog_density * dist * p->fog_ea;
    else
        tau = p->fog_density * dist * (p->fog_ea - eb) / x;
    k = (int)(tau * 256);
    return k < 0 ? 0 : k >= TR_FOG_TAUS ? TR_FOGS - 1 : p->fog_lev[k];
}

/* the sky's glow along a ray that meets no ground, by the cosine of its angle from straight up (TR_SKY_STEPS over
 * -1 .. 1): the air it passes through to the air's edge, made to glow brighter than it dims the ground (the light it
 * scatters) */
#define TR_SKY_STEPS 1024
#define TR_SKY_GLOW 8.0

static void tr_sky_table(TrPlanet *p, double D)
{
    double ra = p->radius + p->atmo_top;
    int k, s;
    for (k = 0; k < TR_SKY_STEPS; k++) {
        double cz = -1 + 2.0 * (k + 0.5) / TR_SKY_STEPS, sz = sqrt(1 - cz * cz), b = D * cz, c = D * D - ra * ra;
        double disc = b * b - c, s0, s1, ds, tau = 0, w;
        p->skytab[k] = 0;
        if (disc <= 0)
            continue;
        s0 = -b - sqrt(disc);
        s1 = -b + sqrt(disc);
        if (s1 <= 0)
            continue;
        if (s0 < 0)
            s0 = 0;
        ds = (s1 - s0) / 16;
        for (s = 0; s < 16; s++) {
            double t = s0 + (s + 0.5) * ds, x = t * sz, y = D + t * cz, h = sqrt(x * x + y * y) - p->radius;
            if (h < p->bottom * TR_HU)
                h = p->bottom * TR_HU;
            tau += exp(-h / p->fog_height);
        }
        tau *= p->fog_density * ds * TR_SKY_GLOW;
        w = 1 - exp(-tau);
        p->skytab[k] = (uint8_t)(w * (TR_FOGS - 1) + 0.5);
    }
}

/* a pixel's ray, not unit: its screen place (x right, y down) through the eye */
static TrV tr_ray(const TrCam *c, double x, double y)
{
    TrV r = tr_v(c->fwd[0], c->fwd[1], c->fwd[2]);
    r = tr_add(r, tr_v(c->right[0], c->right[1], c->right[2]), (x - c->cx) / c->focal);
    return tr_add(r, tr_v(c->up[0], c->up[1], c->up[2]), -(y - c->cy) / c->focal);
}

/* a quarter set up for the frame: which of the screen's pixels are in it, its lines */
static void tr_quarter(TrPlanet *p, int which)
{
    TrQuarter *q = &p->q[which];
    const TrCam *c = &p->cam;
    int amax, cmax, k;
    double ext, xs[4], lo = 1e30, hi = -1e30;
    q->on = 0;
    q->vert = which < 2;
    q->s = (which & 1) ? 1 : -1;
    q->v_along = q->vert ? p->vy : p->vx;
    q->v_across = q->vert ? p->vx : p->vy;
    amax = (q->vert ? c->h : c->w) - 1;
    cmax = (q->vert ? c->w : c->h) - 1;
    q->edge = q->s < 0 ? 0 : amax;
    /* the nearest row (column) to V on the far side of it, on the screen */
    if (q->s > 0) {
        q->near = (int)floor(q->v_along) + 1;
        if (q->near < 0)
            q->near = 0;
        if (q->near > amax)
            return;
    } else {
        q->near = (int)ceil(q->v_along) - 1;
        if (q->near > amax)
            q->near = amax;
        if (q->near < 0)
            return;
    }
    q->len = q->s > 0 ? amax - q->near + 1 : q->near + 1;
    /* where its lines cross the edge row: from the corners of its part of the screen (within the wedge) */
    ext = fabs(q->edge - q->v_along);
    for (k = 0; k < 4; k++) {
        double a = k < 2 ? q->near : q->edge, half = fabs(a - q->v_along), cc = (k & 1) ? q->v_across + half : q->v_across - half;
        if (cc < 0) cc = 0;
        if (cc > cmax) cc = cmax;
        xs[k] = q->v_across + (cc - q->v_across) * (q->edge - q->v_along) / (a - q->v_along);
        if (xs[k] < lo) lo = xs[k];
        if (xs[k] > hi) hi = xs[k];
    }
    if (lo < q->v_across - ext) lo = q->v_across - ext;
    if (hi > q->v_across + ext) hi = q->v_across + ext;
    if (hi < lo)
        return;
    q->first = floor(lo) - 1;
    q->lines = (int)ceil(hi - q->first) + 2;
    q->step = 1;
    if (q->lines > TR_LINES_MAX) {
        q->step = (hi - q->first + 2) / (TR_LINES_MAX - 1);
        q->lines = TR_LINES_MAX;
    }
    if ((size_t)q->lines * q->len > q->cap) {
        uint16_t *px = (uint16_t *)realloc(q->px, (size_t)q->lines * q->len * sizeof(uint16_t));
        if (!px)
            return;
        q->px = px;
        q->cap = (size_t)q->lines * q->len;
    }
    q->on = 1;
}

/* a line's pixels a .. b (indices from `near`, clamped) that nothing has filled yet: 1 when there are any */
static int tr_gap(const uint16_t *px, int len, double a, double b, int *i0, int *i1)
{
    int k;
    *i0 = (int)ceil(a);
    *i1 = (int)floor(b);
    if (*i0 < 0) *i0 = 0;
    if (*i1 > len - 1) *i1 = len - 1;
    for (k = *i0; k <= *i1; k++)
        if (px[k] == TR_PX_EMPTY)
            return 1;
    return 0;
}

static void tr_fill(uint16_t *px, int i0, int i1, uint16_t v)
{
    for (; i0 <= i1; i0++)
        if (px[i0] == TR_PX_EMPTY)
            px[i0] = v;
}

/* where a ray from the eye along unit w first comes within radius r of the centre: 1 and the angle there from the
 * eye's straight down, towards t; 0 when it never does */
static int tr_enter(TrV eye, TrV up, TrV t, TrV w, double r, double *theta)
{
    double b = tr_dot(eye, w), c = tr_dot(eye, eye) - r * r, disc = b * b - c, s;
    TrV q;
    if (disc <= 0)
        return 0;
    s = -b - sqrt(disc);
    if (s < 0)
        return 0;
    q = tr_add(eye, w, s);
    *theta = atan2(tr_dot(q, t), tr_dot(q, up));
    if (*theta < 0)
        *theta = 0;
    return 1;
}

/* one line of a quarter: march out along its great circle, filling its pixels front to back */
static void tr_line(TrPlanet *p, TrQuarter *q, int line)
{
    const TrCam *c = &p->cam;
    uint16_t *px = q->px + (size_t)line * q->len;
    TrV eye = tr_v(c->pos[0], c->pos[1], c->pos[2]), up, t, w, cr, cu, cf, dir, w0, w1;
    double D = sqrt(tr_dot(eye, eye)), X = q->first + line * q->step, theta, theta_max, rb, rt, horizon, alt;
    double rho_near = q->s * (q->near - q->v_along), prev_lo = 0, prev_hi = 0;
    int prev_slab = 0, k, n = 0;
    for (k = 0; k < q->len; k++)
        px[k] = TR_PX_EMPTY;
    up = tr_scale(eye, 1 / D);
    cr = tr_v(c->right[0], c->right[1], c->right[2]);
    cu = tr_v(c->up[0], c->up[1], c->up[2]);
    cf = tr_v(c->fwd[0], c->fwd[1], c->fwd[2]);
    /* the rays of the line's first and last pixels (the line's pixel at `along` a is at across
     * v_across + (X - v_across) (a - v_along) / (edge - v_along)) */
    {
        double a0 = q->near, a1 = q->near + q->s * (q->len - 1);
        double c0 = q->v_across + (X - q->v_across) * (a0 - q->v_along) / (q->edge - q->v_along);
        double c1 = q->v_across + (X - q->v_across) * (a1 - q->v_along) / (q->edge - q->v_along);
        w0 = q->vert ? tr_ray(c, c0, a0) : tr_ray(c, a0, c0);
        w1 = q->vert ? tr_ray(c, c1, a1) : tr_ray(c, a1, c1);
    }
    w = q->vert ? tr_ray(c, X, q->edge) : tr_ray(c, q->edge, X);
    t = tr_add(w, up, -tr_dot(w, up));
    if (tr_dot(t, t) < 1e-18)
        goto sky;
    t = tr_norm(t);
    alt = D - p->radius;
    rb = p->radius + p->bottom * TR_HU;
    rt = p->radius + p->top * TR_HU;
    theta_max = D > rb ? acos(rb / D) + acos(rb / rt) : 3.14159265;
    if (theta_max > 3.14159265)
        theta_max = 3.14159265;
    theta = 0;
    if (D > rt) {
        /* above every hill: nothing before the lowest of the line's rays reaches the highest ground can show */
        if (!tr_enter(eye, up, t, tr_norm(p->nadir ? w0 : w1), rt, &theta))
            goto sky;
    }
    horizon = p->nadir ? -1 : q->len;
    /* everything on the line lies in the plane of up and t: a point's place there is (along up, along t) from the
     * planet's centre, the eye at (D, 0); a screen place along the line from such a pair with four numbers */
    {
        double uf = tr_dot(up, cf), tf = tr_dot(t, cf), us, ts, a_base, fsign, cth = cos(theta), sth = sin(theta);
        if (q->vert) {
            us = tr_dot(up, cu);
            ts = tr_dot(t, cu);
            a_base = c->cy;
            fsign = -c->focal;
        } else {
            us = tr_dot(up, cr);
            ts = tr_dot(t, cr);
            a_base = c->cx;
            fsign = c->focal;
        }
#define TR_RHO(a, b) (q->s * (a_base + fsign * ((a) * us + (b) * ts) / ((a) * uf + (b) * tf) - q->v_along) - rho_near)
        while (theta < theta_max) {
            double h, ra, rb2, z, rho, dist, ds, dt, cd, sd, x;
            int level, f, ci, cj, lo, hi, i0, i1;
            uint8_t id;
            /* the step: three quarters of a cell near, then about two pixels' worth of the distance */
            ra = p->radius * cth - D;
            rb2 = p->radius * sth;
            dist = sqrt(ra * ra + rb2 * rb2);
            /* ground seen edge on is squeezed into few rows: steps grow as the ray grazes it (up to 4 times) */
            ds = dist * 2.0 / c->focal;
            if (ds < p->cell * 0.75)
                ds = p->cell * 0.75;
            for (level = 0; level < TR_LEVELS - 1 && ds > p->cell * (2 << level); level++)
                ;                               /* (the grid's detail by the distance alone) */
            x = -(ra * cth + rb2 * sth) / dist;    /* the sine of the ray's angle to the ground there */
            if (x < 1)
                ds /= x > 0.25 ? x : 0.25;
            dir = tr_add(tr_scale(up, cth), t, sth);
            h = tr_sample(p, dir, level, &id, &f, &ci, &cj);
            ra = (p->radius + h) * cth - D;
            rb2 = (p->radius + h) * sth;
            z = ra * uf + rb2 * tf;
            n++;
            if (z > 1) {
                int fog = -1;
                rho = TR_RHO(ra, rb2);
                if (p->nadir ? rho > horizon : rho < horizon) {
                    if (p->nadir ? tr_gap(px, q->len, horizon + 1e-9, rho, &i0, &i1) : tr_gap(px, q->len, rho, horizon - 1e-9, &i0, &i1)) {
                        fog = tr_fog(p, sqrt(ra * ra + rb2 * rb2), alt, h);
                        tr_fill(px, i0, i1, (uint16_t)(id | fog << 8));
                    }
                    horizon = rho;
                }
                /* the second layer, near enough to be drawn as itself */
                if (level < TR_SLAB_LEVELS && tr_slab(p, f, ci << level, cj << level, &lo, &hi)) {
                    double la = (p->radius + lo * TR_HU) * cth - D, lb = (p->radius + lo * TR_HU) * sth;
                    double ha = (p->radius + hi * TR_HU) * cth - D, hb = (p->radius + hi * TR_HU) * sth;
                    if (la * uf + lb * tf > 1 && ha * uf + hb * tf > 1) {
                        double r0 = TR_RHO(la, lb), r1 = TR_RHO(ha, hb);
                        double s0 = r0 < r1 ? r0 : r1, s1 = r0 < r1 ? r1 : r0;
                        int mat = alt < lo * TR_HU ? TR_UNDER : TR_ROCK;
                        int light = alt > hi * TR_HU ? id % TR_LIGHTS : TR_LIGHTS / 3;
                        if (prev_slab) {
                            if (prev_lo < s0) s0 = prev_lo;
                            if (prev_hi > s1) s1 = prev_hi;
                        }
                        if (tr_gap(px, q->len, s0, s1, &i0, &i1)) {
                            if (fog < 0)
                                fog = tr_fog(p, sqrt(ha * ha + hb * hb), alt, hi * TR_HU);
                            tr_fill(px, i0, i1, (uint16_t)((mat * TR_LIGHTS + light) | fog << 8));
                        }
                        prev_lo = r0 < r1 ? r0 : r1;
                        prev_hi = r0 < r1 ? r1 : r0;
                        prev_slab = 1;
                    } else
                        prev_slab = 0;
                } else
                    prev_slab = 0;
                if (p->nadir ? horizon >= q->len - 1 : horizon <= 0)
                    break;
            } else
                prev_slab = 0;
            /* nothing farther can show once the ray through the filled part's edge has left the highest ground's
             * sphere behind (asked now and then) */
            if ((n & 15) == 0) {
                double hk = p->nadir ? (horizon < 0 ? 0 : horizon) : (horizon > q->len - 1 ? q->len - 1 : horizon);
                TrV wr = tr_norm(tr_add(w0, tr_add(w1, w0, -1), q->len > 1 ? hk / (q->len - 1) : 0));
                double wu = tr_dot(wr, up), wt = tr_dot(wr, t), b = D * wu, cc = D * D - rt * rt, disc = b * b - cc, s1;
                if (disc <= 0)
                    break;
                s1 = -b + sqrt(disc);
                if (s1 <= 0 || theta > atan2(s1 * wt, D + s1 * wu))
                    break;
            }
            /* on round the great circle by dt (small: its sine and cosine to the third power) */
            dt = ds / p->radius;
            cd = 1 - dt * dt * 0.5;
            sd = dt - dt * dt * dt / 6;
            x = cth * cd - sth * sd;
            sth = sth * cd + cth * sd;
            cth = x;
            theta += dt;
        }
#undef TR_RHO
    }
sky:
    p->samples += n;
    /* what no ground filled: the sky, its glow by the ray's angle from straight up (the line's rays all lie in a
     * plane with straight up in it) */
    {
        TrV dw = tr_scale(tr_add(w1, w0, -1), q->len > 1 ? 1.0 / (q->len - 1) : 0);
        double a0 = tr_dot(w0, up), da = tr_dot(dw, up), b0 = tr_dot(w0, w0), b1 = 2 * tr_dot(w0, dw), b2 = tr_dot(dw, dw);
        for (k = 0; k < q->len; k++)
            if (px[k] == TR_PX_EMPTY) {
                double cz = (a0 + da * k) / sqrt(b0 + (b1 + b2 * k) * k);
                int s = (int)((cz + 1) * 0.5 * TR_SKY_STEPS), fog;
                fog = p->skytab[s < 0 ? 0 : s > TR_SKY_STEPS - 1 ? TR_SKY_STEPS - 1 : s];
                if (fog)
                    px[k] = (uint16_t)(TR_PX_SKY | fog);
            }
    }
}

void tr_view(TrPlanet *p, const TrCam *cam)
{
    TrV eye = tr_v(cam->pos[0], cam->pos[1], cam->pos[2]), n;
    double D = sqrt(tr_dot(eye, eye)), nx, ny, nz;
    int k, i;
    p->cam = *cam;
    p->samples = 0;
    n = tr_v(-eye.x / D, -eye.y / D, -eye.z / D);
    nx = tr_dot(n, tr_v(cam->right[0], cam->right[1], cam->right[2]));
    ny = tr_dot(n, tr_v(cam->up[0], cam->up[1], cam->up[2]));
    nz = tr_dot(n, tr_v(cam->fwd[0], cam->fwd[1], cam->fwd[2]));
    if (fabs(nz) < 1e-6)
        nz = nz < 0 ? -1e-6 : 1e-6;
    p->nadir = nz > 0;
    p->vx = cam->cx + cam->focal * nx / nz;
    p->vy = cam->cy - cam->focal * ny / nz;
    tr_sky_table(p, D);
    p->fog_ea = exp(-((D - p->radius) < p->bottom * TR_HU ? p->bottom * TR_HU : D - p->radius) / p->fog_height);
    for (k = 0; k < 4; k++) {
        tr_quarter(p, k);
        if (p->q[k].on)
            for (i = 0; i < p->q[k].lines; i++)
                tr_line(p, &p->q[k], i);
    }
}

uint32_t tr_samples(const TrPlanet *p) { return p->samples; }

static const uint8_t tr_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static uint8_t tr_pick(const TrDither *d, int x, int y) { return tr_bayer[y & 3][x & 3] < d->t ? d->b : d->a; }

static uint32_t tr_put(TrPlanet *p, uint8_t *row, int x, int y, uint16_t v)
{
    if (v == TR_PX_EMPTY)
        return 0;
    if (v & TR_PX_SKY)
        row[x] = tr_pick(&p->sky[v & 0x1F][row[x]], x, y);
    else
        row[x] = tr_pick(&p->ground[(v >> 8) & 0x1F][v & 0xFF], x, y);
    return 1;
}

uint32_t tr_blit(TrPlanet *p, uint8_t *fb, int fb_y0, int stride, int y0, int y1, int xa, int xb, const int16_t *ext,
                 int ext_y0, int ext_rows)
{
    uint32_t n = 0;
    int y;
    if (y0 < fb_y0)
        y0 = fb_y0;
    if (y1 > p->cam.h)
        y1 = p->cam.h;
    if (xa < 0)
        xa = 0;
    if (xb > p->cam.w)
        xb = p->cam.w;
    for (y = y0; y < y1; y++) {
        uint8_t *row = fb + (size_t)(y - fb_y0) * stride;
        int ca = xa, cb = xb, x, k;
        double dy = y - p->vy, half = fabs(dy);
        if (ext) {
            if (y < ext_y0 || y >= ext_y0 + ext_rows)
                continue;
            if (ext[2 * (y - ext_y0)] > ca) ca = ext[2 * (y - ext_y0)];
            if (ext[2 * (y - ext_y0) + 1] < cb) cb = ext[2 * (y - ext_y0) + 1];
        }
        if (ca >= cb)
            continue;
        for (k = 0; k < 4; k++) {
            TrQuarter *q = &p->q[k];
            int a0, a1;
            if (!q->on)
                continue;
            if (q->vert) {
                /* the row's part within the wedge above (below) V: a line a pixel, found by adding */
                double fi, dfi;
                int idx;
                if (q->s * dy <= 0)
                    continue;
                idx = q->s * (y - q->near);
                if (idx < 0 || idx >= q->len)
                    continue;
                a0 = (int)ceil(p->vx - half);
                a1 = (int)floor(p->vx + half);
                if (a0 < ca) a0 = ca;
                if (a1 > cb - 1) a1 = cb - 1;
                if (a0 > a1)
                    continue;
                dfi = (q->edge - p->vy) / dy / q->step;
                fi = (p->vx + (a0 - p->vx) * (q->edge - p->vy) / dy - q->first) / q->step + 0.5;
                for (x = a0; x <= a1; x++, fi += dfi) {
                    int i = (int)fi;
                    if (i >= 0 && i < q->lines)
                        n += tr_put(p, row, x, y, q->px[(size_t)i * q->len + idx]);
                }
            } else {
                /* the parts left (right) of the wedges: a division a pixel */
                if (q->s < 0) {
                    a0 = ca;
                    a1 = (int)ceil(p->vx - half) - 1;
                    if (a1 > cb - 1) a1 = cb - 1;
                } else {
                    a0 = (int)floor(p->vx + half) + 1;
                    a1 = cb - 1;
                    if (a0 < ca) a0 = ca;
                }
                for (x = a0; x <= a1; x++) {
                    double dx = x - p->vx;
                    int idx = q->s * (x - q->near), i;
                    if (dx == 0 || idx < 0 || idx >= q->len)
                        continue;
                    i = (int)((p->vy + dy * (q->edge - p->vx) / dx - q->first) / q->step + 0.5);
                    if (i >= 0 && i < q->lines)
                        n += tr_put(p, row, x, y, q->px[(size_t)i * q->len + idx]);
                }
            }
        }
    }
    return n;
}

int tr_seen(const TrPlanet *p, const double eye[3], const double pt[3])
{
    TrV e = tr_v(eye[0], eye[1], eye[2]), d = tr_v(pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2]);
    double len = sqrt(tr_dot(d, d)), rt = p->radius + p->top * TR_HU, b, c, disc, s, s1;
    int guard = 0;
    if (len < 1)
        return 1;
    d = tr_v(d.x / len, d.y / len, d.z / len);
    /* only the part of the line inside the highest ground's sphere can be hidden */
    b = tr_dot(e, d);
    c = tr_dot(e, e) - rt * rt;
    disc = b * b - c;
    if (disc <= 0)
        return 1;
    s = -b - sqrt(disc);
    s1 = -b + sqrt(disc);
    if (s < 0)
        s = 0;
    if (s1 > len - p->cell)
        s1 = len - p->cell;   /* (not the point's own ground) */
    while (s < s1 && guard++ < 4000) {
        TrV q = tr_add(e, d, s);
        double r = sqrt(tr_dot(q, q)), h, step;
        int f, ci, cj, lo, hi;
        uint8_t id;
        h = tr_sample(p, tr_v(q.x / r, q.y / r, q.z / r), 0, &id, &f, &ci, &cj);
        if (r < p->radius + h)
            return 0;
        if (tr_slab(p, f, ci, cj, &lo, &hi) && r >= p->radius + lo * TR_HU && r <= p->radius + hi * TR_HU)
            return 0;
        step = (r - p->radius - h) * 0.3;
        if (step < p->cell * 0.5)
            step = p->cell * 0.5;
        s += step;
    }
    return 1;
}
