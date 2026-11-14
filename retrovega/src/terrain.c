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

#if defined(__GNUC__) || defined(__clang__)
#define TR_HOT static inline __attribute__((always_inline))   /* (the march's helpers: in the loop) */
#else
#define TR_HOT static
#endif

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
#define TR_BLIT_W 1024                  /* the widest screen drawn */
#define TR_THIN_MAX 12                  /* lines thinned to every 4096th at most */
#define TR_STEP_PX 2.0                  /* the march's step in the far ground: pixels' worth of the distance */
#define TR_SKIP_FIRST 2                 /* the finest level the march skips hidden ground by */
#define TR_FOG_HS_ 1024                 /* (tr_fog's tables' sizes) */
#define TR_FOG_TAUS_ 2048               /* a quarter's lines at most (more: spaced wider than a pixel) */

enum { TR_SAND, TR_ROCK, TR_GREEN, TR_UNDER };

typedef struct {
    int16_t lo[TR_TILE * TR_TILE], hi[TR_TILE * TR_TILE];   /* over the datum, height units; lo >= hi: none */
} TrSlabTile;

typedef struct {                        /* a cell of a level's grid */
    int16_t h;                          /* over the datum, height units */
    uint8_t c;                          /* colour id: material * TR_LIGHTS + light */
    uint8_t slab;                       /* 1: a slab lies over it (level 0), or over a cell under it (the next levels) */
} TrCell;

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
    uint8_t *thin;                      /* len: at each pixel from `near`, the lines kept are those that are multiples
                                           of 1 << thin (near V the lines bunch up: only enough to be a pixel apart are
                                           marched there) */
    int thin_cap;
} TrQuarter;

#define TR_PX_EMPTY 0xFFFF              /* not reached: the sky as it is */
#define TR_PX_SKY 0x8000                /* | haze level: no ground, the sky's pixel hazed */
                                        /* else colour id | haze level << 8 */

typedef struct { double x, y, z; } TrV;

typedef struct {                        /* the frame's eye, worked out once for all the lines (tr_view) */
    TrV eye, up, cr, cu, cf;            /* the eye; straight up there; the camera's right, up and front */
    double D, alt, rt, theta_max;       /* the eye from the centre; its height; the highest ground's radius; how far
                                           round the planet any ground may show */
    double near_ds, far_k;              /* the march's step: the least, and per position of distance */
    double level_d2[TR_LEVELS];         /* the squared distance past which the march takes the next level */
    double skip_dth[TR_LEVELS];         /* half a cell of a level, as an angle (the skips) */
} TrFrameK;

struct TrPlanet {
    TrPlanetDesc desc;
    int32_t radius;                     /* the datum, positions */
    int32_t top, bottom;                /* the highest (a slab's top too) and lowest ground, height units */
    TrCell *g[TR_LEVELS][6];            /* the grids, bordered (tr_side) */
    int16_t *hmax[TR_LEVELS][6];        /* (levels TR_SKIP_FIRST on) the highest any ground or slab gets within a cell
                                           of each cell (tr_maxes): what the march may skip */
    int32_t *tiles[6];                  /* TR_TILES x TR_TILES: an index into slabs + 1, or 0 */
    TrSlabTile *slabs;
    int nslabs, slabs_cap;
    float rgb[256][3];                  /* the colour ids' colours, 0 .. 255 */
    float fog_rgb[3];                   /* the haze's colour */
    double fog_density, fog_height;     /* the haze per position at the datum; its scale height, positions */
    double atmo_top;                    /* where the air is taken to end, over the datum, positions */
    double fog_h0, fog_hstep, fog_e[TR_FOG_HS_], fog_ea;   /* (tr_fog's tables; fog_ea: the frame's eye's) */
    float fog_lev[TR_FOG_TAUS_];
    double cell;                        /* a level-0 cell's side, about, positions */
    uint8_t pal[256][3];                /* the game's palette, 8 bits */
    float pal_lab[256][3];              /* the same in L*a*b* */
    TrDither ground[TR_FOGS][256];      /* a colour id at a haze level, as the palette's two */
    TrDither sky[TR_FOGS][256];         /* a palette entry at a haze level */
    uint16_t skytab[1024];              /* the frame's: the sky's glow by a ray's angle from straight up, 256ths of a
                                           haze level (tr_sky_table) */
    int sky_any;                        /* the frame's: some of the sky glows */
    uint16_t glow[TR_BLIT_W];           /* (tr_blit's: a row's sky glow) */
    double sky_f, sky_r, sky_u;         /* the frame's: straight up dotted with the eye's front, right and up */
    double lv_half[TR_LEVELS], lv_off[TR_LEVELS];   /* a level's n / 2 and (n - 1) / 2 (tr_sample) */
    /* the frame's picture */
    TrCam cam;
    double vx, vy;                      /* V on the screen */
    int nadir;                          /* 1: V is the point below (the ground lies out from it), 0: above */
    TrQuarter q[4];                     /* top, bottom, left, right */
    TrFrameK fk;
    uint32_t samples, skips;            /* (the last frame's march, for timing) */
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
    return (int32_t)(((int64_t)acc * 16384) / total);
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

static TrCell *tr_cell(const TrPlanet *p, int level, int f, int i, int j)   /* (i, j: -1 .. n, the border included) */
{
    return p->g[level][f] + (j + 1) * tr_side(level) + (i + 1);
}

static int16_t *tr_hp(const TrPlanet *p, int level, int f, int i, int j) { return &tr_cell(p, level, f, i, j)->h; }
static uint8_t *tr_cp(const TrPlanet *p, int level, int f, int i, int j) { return &tr_cell(p, level, f, i, j)->c; }

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
    const TrCell *r;
    if (i < -1) { i = -1; fi = 0; }
    if (j < -1) { j = -1; fj = 0; }
    if (i > TR_N - 1) { i = TR_N - 1; fi = 65535; }
    if (j > TR_N - 1) { j = TR_N - 1; fj = 65535; }
    r = tr_cell(p, 0, f, i, j);
    a = TR_LERP(r[0].h * TR_HU, r[1].h * TR_HU, fi);
    b = TR_LERP(r[TR_S].h * TR_HU, r[TR_S + 1].h * TR_HU, fi);
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
                    tr_cell(p, l, f, i, j)->slab = (uint8_t)(tr_cell(p, l - 1, f, 2 * i, 2 * j)->slab | tr_cell(p, l - 1, f, 2 * i + 1, 2 * j)->slab
                                                             | tr_cell(p, l - 1, f, 2 * i, 2 * j + 1)->slab | tr_cell(p, l - 1, f, 2 * i + 1, 2 * j + 1)->slab);
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
                    *tr_cell(p, l, f, i, j) = *tr_cell(p, l, f2, ci, cj);
                }
    }
}

/* the highest the ground (or a slab's top) gets near each cell of the levels TR_SKIP_FIRST on: over the level-0
 * cells under it and one more all round (a sample's four cells reach that far), then over its eight neighbours too
 * (the next faces' through the border), so that anywhere in the cell, a cell's width either way is under it.  The
 * march skips ground whose bound stays under the ray it would have to rise above */
static int tr_maxes(TrPlanet *p)
{
    int16_t *raw[TR_LEVELS][6];
    int l, f, i, j, ok = 1;
    memset(raw, 0, sizeof raw);
    for (l = TR_SKIP_FIRST; l < TR_LEVELS; l++) {
        int n = TR_N >> l, side = n + 2;
        for (f = 0; f < 6; f++) {
            raw[l][f] = (int16_t *)malloc((size_t)side * side * sizeof(int16_t));
            p->hmax[l][f] = (int16_t *)malloc((size_t)n * n * sizeof(int16_t));
            if (!raw[l][f] || !p->hmax[l][f])
                ok = 0;
        }
    }
    if (!ok)
        goto out;
    for (l = TR_SKIP_FIRST; l < TR_LEVELS; l++) {
        int n = TR_N >> l, side = n + 2;
        for (f = 0; f < 6; f++)
            for (j = 0; j < n; j++)
                for (i = 0; i < n; i++) {
                    int16_t m = -32768;
                    if (l == TR_SKIP_FIRST) {
                        int a, b, w = 1 << l, lo, hi;
                        for (b = j * w - 1; b <= (j + 1) * w; b++)
                            for (a = i * w - 1; a <= (i + 1) * w; a++) {
                                const TrCell *c = tr_cell(p, 0, f, a, b);
                                if (c->h > m)
                                    m = c->h;
                                if (c->slab && tr_slab(p, f, a, b, &lo, &hi) && hi > m)
                                    m = (int16_t)hi;
                            }
                    } else {
                        int ps = 2 * n + 2;    /* (the level below's side) */
                        const int16_t *r = raw[l - 1][f] + (2 * j + 1) * ps + (2 * i + 1);
                        m = r[0];
                        if (r[1] > m) m = r[1];
                        if (r[ps] > m) m = r[ps];
                        if (r[ps + 1] > m) m = r[ps + 1];
                    }
                    raw[l][f][(j + 1) * side + (i + 1)] = m;
                }
        /* the border, from the faces the cells really lie on */
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
                    raw[l][f][(j + 1) * side + (i + 1)] = raw[l][f2][(cj + 1) * side + (ci + 1)];
                }
        for (f = 0; f < 6; f++)
            for (j = 0; j < n; j++)
                for (i = 0; i < n; i++) {
                    const int16_t *r = raw[l][f] + j * side + i;   /* (the 3 x 3 round (i, j), bordered) */
                    int16_t m = r[0];
                    int a, b;
                    for (b = 0; b < 3; b++)
                        for (a = 0; a < 3; a++)
                            if (r[b * side + a] > m)
                                m = r[b * side + a];
                    p->hmax[l][f][j * n + i] = m;
                }
    }
out:
    for (l = 0; l < TR_LEVELS; l++)
        for (f = 0; f < 6; f++)
            free(raw[l][f]);
    return ok;
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
static int tr_maxes(TrPlanet *p);

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
            p->g[l][f] = (TrCell *)calloc((size_t)tr_side(l) * tr_side(l), sizeof(TrCell));
            if (!p->g[l][f]) {
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
    for (f = 0; f < 6; f++)
        for (j = 0; j < TR_N; j++)
            for (i = 0; i < TR_N; i++) {
                int lo, hi;
                tr_cell(p, 0, f, i, j)->slab = (uint8_t)tr_slab(p, f, i, j, &lo, &hi);
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
    for (l = 0; l < TR_LEVELS; l++) {
        p->lv_half[l] = (TR_N >> l) * 0.5;
        p->lv_off[l] = ((TR_N >> l) - 1) * 0.5;
    }
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
    if (!tr_maxes(p)) {
        tr_free(p);
        return 0;
    }
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
            free(p->g[l][f]);
            free(p->hmax[l][f]);
        }
    for (f = 0; f < 6; f++)
        free(p->tiles[f]);
    free(p->slabs);
    for (l = 0; l < 4; l++) {
        free(p->q[l].px);
        free(p->q[l].thin);
    }
    free(p);
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the picture */

static TrV tr_v(double x, double y, double z) { TrV v; v.x = x; v.y = y; v.z = z; return v; }
static double tr_dot(TrV a, TrV b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static TrV tr_add(TrV a, TrV b, double k) { return tr_v(a.x + b.x * k, a.y + b.y * k, a.z + b.z * k); }
static TrV tr_scale(TrV a, double k) { return tr_v(a.x * k, a.y * k, a.z * k); }
static TrV tr_norm(TrV a)
{
    double l = sqrt(tr_dot(a, a));
    return l > 0 ? tr_v(a.x / l, a.y / l, a.z / l) : a;
}

typedef struct {                        /* what a sample of the ground found */
    double h;                           /* the height over the datum, positions (the four cells round it weighed) */
    float light;                        /* 0 .. TR_LIGHTS-1, weighed too where the four are of one material */
    int mat;
    int f, ci, cj;                      /* the nearest cell, in its level's numbering */
    int slab;                           /* that cell's slab mark */
} TrHit;

/* a sample of the ground at a unit direction, from the grid of a level */
TR_HOT void tr_sample(const TrPlanet *p, double dx, double dy, double dz, int level, TrHit *o)
{
    double ax = fabs(dx), ay = fabs(dy), az = fabs(dz), m, gi, gj, fi, fj, off = p->lv_off[level];
    int n = TR_N >> level, i, j, side = n + 2, k, c0, diff;
    const TrCell *g;
    if (ax >= ay && ax >= az) {
        m = p->lv_half[level] / ax;
        gi = dy * m + off;
        gj = dz * m + off;
        o->f = dx < 0;
    } else if (ay >= az) {
        m = p->lv_half[level] / ay;
        gi = dz * m + off;
        gj = dx * m + off;
        o->f = 2 + (dy < 0);
    } else {
        m = p->lv_half[level] / az;
        gi = dx * m + off;
        gj = dy * m + off;
        o->f = 4 + (dz < 0);
    }
    i = (int)(gi + 2) - 2;              /* (floor: gi is over -2) */
    j = (int)(gj + 2) - 2;
    fi = gi - i;
    fj = gj - j;
    if (i < -1) { i = -1; fi = 0; }
    if (j < -1) { j = -1; fj = 0; }
    if (i > n - 1) { i = n - 1; fi = 1; }
    if (j > n - 1) { j = n - 1; fj = 1; }
    g = p->g[level][o->f] + (j + 1) * side + (i + 1);
    k = (fj >= 0.5 ? side : 0) + (fi >= 0.5);
    o->ci = i + (fi >= 0.5);
    o->cj = j + (fj >= 0.5);
    c0 = g[k].c;
    o->slab = g[k].slab;
    o->mat = c0 >> 6;
    diff = (g[0].c ^ c0) | (g[1].c ^ c0) | (g[side].c ^ c0) | (g[side + 1].c ^ c0);
    if (diff < TR_LIGHTS)               /* the four of one material: their light weighed */
        o->light = (float)(((g[0].c & 63) * (1 - fi) + (g[1].c & 63) * fi) * (1 - fj)
                           + ((g[side].c & 63) * (1 - fi) + (g[side + 1].c & 63) * fi) * fj);
    else
        o->light = (float)(c0 & 63);
    o->h = ((g[0].h * (1 - fi) + g[1].h * fi) * (1 - fj) + (g[side].h * (1 - fi) + g[side + 1].h * fi) * fj) * TR_HU;
}

/* the haze between the eye and a point: a level 0 .. TR_FOGS-1 (a fraction too).  The air thins with height as
 * e^(-h/H); along a straight line from height a to height b its sum is L * (e^(-a/H) - e^(-b/H)) / ((b - a) / H)
 * (the planet taken as flat for it).  From tables: e^(-h/H) over the ground's heights, and the level by that sum (a
 * frame's eye's own e^(-a/H) is worked out once, in tr_view) */
#define TR_FOG_HS TR_FOG_HS_            /* e^(-h/H) at heights bottom .. top */
#define TR_FOG_TAUS TR_FOG_TAUS_        /* the level by the sum, 256 steps to each 1 */

static void tr_fog_tables(TrPlanet *p)
{
    int k;
    p->fog_h0 = p->bottom * TR_HU;
    p->fog_hstep = (double)(p->top - p->bottom + 1) * TR_HU / (TR_FOG_HS - 1);
    for (k = 0; k < TR_FOG_HS; k++)
        p->fog_e[k] = exp(-(p->fog_h0 + k * p->fog_hstep) / p->fog_height);
    for (k = 0; k < TR_FOG_TAUS; k++)
        p->fog_lev[k] = (float)((1 - exp(-(k + 0.5) / 256.0)) * (TR_FOGS - 1));
}

TR_HOT float tr_fog(const TrPlanet *p, double dist, double ha, double hb)
{
    double x = (hb - ha) / p->fog_height, eb, tau;
    int k = (int)((hb - p->fog_h0) / p->fog_hstep + 0.5);
    eb = p->fog_e[k < 0 ? 0 : k > TR_FOG_HS - 1 ? TR_FOG_HS - 1 : k];
    if (fabs(x) < 1e-3)
        tau = p->fog_density * dist * p->fog_ea;
    else
        tau = p->fog_density * dist * (p->fog_ea - eb) / x;
    k = (int)(tau * 256);
    return k < 0 ? 0 : k >= TR_FOG_TAUS ? (float)(TR_FOGS - 1) : p->fog_lev[k];
}

/* the sky's glow along a ray that meets no ground, by the cosine of its angle from straight up (TR_SKY_STEPS over
 * -1 .. 1), in 256ths of a haze level: the air it passes through to the air's edge, made to glow brighter than it dims
 * the ground (the light it scatters) */
#define TR_SKY_STEPS 1024
#define TR_SKY_GLOW 8.0

static void tr_sky_table(TrPlanet *p, double D)
{
    double ra = p->radius + p->atmo_top;
    int k, s;
    p->sky_any = 0;
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
        p->skytab[k] = (uint16_t)(w * (TR_FOGS - 1) * 256);
        if (p->skytab[k] >= 128)
            p->sky_any = 1;
    }
}

/* a pixel's ray, not unit: its screen place (x right, y down) through the eye */
static TrV tr_ray(const TrCam *c, double x, double y)
{
    TrV r = tr_v(c->fwd[0], c->fwd[1], c->fwd[2]);
    r = tr_add(r, tr_v(c->right[0], c->right[1], c->right[2]), (x - c->cx) / c->focal);
    return tr_add(r, tr_v(c->up[0], c->up[1], c->up[2]), -(y - c->cy) / c->focal);
}

/* a quarter set up for the frame: which of the screen's pixels are in it, its lines.  Its pixels are kept a row at a
 * time (all its lines' pixels at one distance from V together): the march fills them a line after another, nearby,
 * and the blit reads a screen row's straight along */
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
    memset(q->px, 0xFF, (size_t)q->lines * q->len * sizeof(uint16_t));   /* TR_PX_EMPTY */
    if (q->len > q->thin_cap) {
        uint8_t *t = (uint8_t *)realloc(q->thin, (size_t)q->len);
        if (!t)
            return;
        q->thin = t;
        q->thin_cap = q->len;
    }
    {
        /* the lines' spacing at pixel k is step (k + |near - V|) / |edge - V|; a line whose index has m trailing
         * zero bits is marched from where twice 2^m of that spacing reaches a pixel (tr_line) */
        double L = fabs(q->edge - q->v_along), rn = q->s * (q->near - q->v_along);
        int kk = TR_THIN_MAX;
        for (k = 0; k < q->len; k++) {
            double sp = q->step * (k + rn) / L;
            while (kk > 0 && sp * (1 << kk) >= 1)
                kk--;
            while (kk < TR_THIN_MAX && sp * (2 << kk) < 1)
                kk++;
            q->thin[k] = (uint8_t)kk;
        }
    }
    q->on = 1;
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

/* the pixels a .. b of a line, a and b in pixels from `near` (clamped to the line: they may be far off it) */
TR_HOT int tr_span(double a, double b, int lo, int len, int *i0, int *i1)
{
    if (a < -1) a = -1;
    if (b > len) b = len;
    if (a > b)
        return 0;
    *i0 = (int)(a + 2) - 1;             /* ceil(a) .. floor(b), a and b over -2 */
    if (*i0 - 1 >= a)
        (*i0)--;
    *i1 = (int)(b + 2) - 2;
    if (*i0 < lo) *i0 = lo;
    if (*i1 > len - 1) *i1 = len - 1;
    return *i0 <= *i1;
}

/* the bound on the ground near a unit direction at level l (TR_SKIP_FIRST on): tr_maxes', in positions over the
 * datum */
TR_HOT double tr_bound(const TrPlanet *p, double dx, double dy, double dz, int l)
{
    double ax = fabs(dx), ay = fabs(dy), az = fabs(dz), m, gi, gj;
    int n = TR_N >> l, f, i, j;
    if (ax >= ay && ax >= az) {
        m = p->lv_half[l] / ax;
        gi = dy * m + p->lv_off[l];
        gj = dz * m + p->lv_off[l];
        f = dx < 0;
    } else if (ay >= az) {
        m = p->lv_half[l] / ay;
        gi = dz * m + p->lv_off[l];
        gj = dx * m + p->lv_off[l];
        f = 2 + (dy < 0);
    } else {
        m = p->lv_half[l] / az;
        gi = dx * m + p->lv_off[l];
        gj = dy * m + p->lv_off[l];
        f = 4 + (dz < 0);
    }
    i = (int)(gi + 2.5) - 2;
    j = (int)(gj + 2.5) - 2;
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > n - 1) i = n - 1;
    if (j > n - 1) j = n - 1;
    return (double)p->hmax[l][f][j * n + i] * TR_HU;
}

/* a ray of the line's plane, from the eye at (D, 0) along unit (wu, wt) (up, t): how far from the centre it is where
 * it crosses the radial line at the angle whose cosine and sine are c, s (very far where it never does) */
TR_HOT double tr_ray_r2(double D, double wu, double wt, double c, double s)
{
    double den = wt * c - wu * s, k, x, y;
    if (den <= 1e-12)
        return 1e300;
    k = D * s / den;
    if (k < 0)
        return 1e300;
    x = D + k * wu;
    y = k * wt;
    return x * x + y * y;
}

/* fill a line's pixels i0 .. i1 that are still empty with the ground between two samples: the light and the haze
 * weighed from one to the other along the rows (b's material throughout) */
TR_HOT void tr_fill_ground(uint16_t *px, int stride, int i0, int i1, double ra, double rb, float la, float lb,
                           float fa, float fb, int mat)
{
    double span = rb - ra, t0, dt;
    int32_t l, dl, f, df;
    uint16_t *e = px + (size_t)i0 * stride;
    int k, base = mat * TR_LIGHTS;
    if (span < 1e-6 && span > -1e-6) {
        t0 = 1;
        dt = 0;
    } else {
        t0 = (i0 - ra) / span;
        dt = 1 / span;
        if (t0 < 0) t0 = 0;
        if (t0 > 1) t0 = 1;
        if (t0 + dt * (i1 - i0) > 1.0001 || t0 + dt * (i1 - i0) < -0.0001)
            dt = i1 > i0 ? (1 - t0) / (i1 - i0) : 0;   /* (rounding: never past b) */
    }
    l = (int32_t)((la + (lb - la) * t0) * 65536 + 32768);
    dl = (int32_t)((lb - la) * dt * 65536);
    f = (int32_t)((fa + (fb - fa) * t0) * 65536 + 32768);
    df = (int32_t)((fb - fa) * dt * 65536);
    for (k = i0; k <= i1; k++, e += stride, l += dl, f += df)
        if (*e == TR_PX_EMPTY)
            *e = (uint16_t)((base + (l >> 16)) | (f >> 16) << 8);
}

TR_HOT void tr_fill(uint16_t *px, int stride, int i0, int i1, uint16_t v)
{
    for (; i0 <= i1; i0++)
        if (px[(size_t)i0 * stride] == TR_PX_EMPTY)
            px[(size_t)i0 * stride] = v;
}

/* one line of a quarter: march out along its great circle, filling its pixels front to back */
static void tr_line(TrPlanet *p, TrQuarter *q, int line)
{
    const TrCam *c = &p->cam;
    const TrFrameK *fk = &p->fk;
    uint16_t *px = q->px + line;
    const int stride = q->lines, len = q->len, nadir = p->nadir;
    const double D = fk->D, R = p->radius;
    TrV up = fk->up, t, w, w0, w1, wk;
    double X = q->first + line * q->step, theta, horizon, rho_near = q->s * (q->near - q->v_along);
    double prev_lo = 0, prev_hi = 0, prev_rho = 0, prev_dist = 0, prev_h = 0;
    float prev_light = 0, prev_fog = 0;
    int prev_slab = 0, prev_ok = 0, prev_mat = 0, n = 0, skips = 0, kmin;
    /* the rays of the line's first and last pixels (the line's pixel at `along` a is at across
     * v_across + (X - v_across) (a - v_along) / (edge - v_along)) */
    {
        double a0 = q->near, a1 = q->near + q->s * (len - 1);
        double c0 = q->v_across + (X - q->v_across) * (a0 - q->v_along) / (q->edge - q->v_along);
        double c1 = q->v_across + (X - q->v_across) * (a1 - q->v_along) / (q->edge - q->v_along);
        w0 = q->vert ? tr_ray(c, c0, a0) : tr_ray(c, a0, c0);
        w1 = q->vert ? tr_ray(c, c1, a1) : tr_ray(c, a1, c1);
    }
    {
        /* the first pixel this line is wanted for (the lines bunch up near V: q->thin) */
        int m = 0;
        while (m < TR_THIN_MAX && !((line >> m) & 1))
            m++;
        kmin = 0;
        while (kmin < len && q->thin[kmin] > m)
            kmin++;
        if (kmin >= len)
            return;
        wk = tr_add(w0, tr_add(w1, w0, -1), len > 1 ? (double)kmin / (len - 1) : 0);
    }
    w = q->vert ? tr_ray(c, X, q->edge) : tr_ray(c, q->edge, X);
    t = tr_add(w, up, -tr_dot(w, up));
    if (tr_dot(t, t) < 1e-18)
        return;
    t = tr_norm(t);
    theta = 0;
    if (D > fk->rt) {
        /* above every hill: nothing before the lowest of the line's rays reaches the highest ground can show */
        if (!tr_enter(fk->eye, up, t, tr_norm(nadir ? wk : w1), fk->rt, &theta))
            return;
    }
    horizon = nadir ? kmin - 1 : len;
    /* everything on the line lies in the plane of up and t: a point's place there is (along up, along t) from the
     * planet's centre, the eye at (D, 0); its pixel on the line, rho = ra_ + rb_ (a us + b ts) / (a uf + b tf) */
    {
        const double uf = tr_dot(up, fk->cf), tf = tr_dot(t, fk->cf), alt = fk->alt, theta_max = fk->theta_max;
        const double near_ds = fk->near_ds, far_k = fk->far_k;
        double us, ts, ra_, rb_, cth = cos(theta), sth = sin(theta);
        double hr_u = 0, hr_t = 0, hr_phi = 0, hr_rmin2 = 0;   /* the ray through the filled part's edge, in the plane */
        int hr_dirty = 1, hidden = 0, level = 0;
        if (q->vert) {
            us = tr_dot(up, fk->cu);
            ts = tr_dot(t, fk->cu);
            ra_ = q->s * (c->cy - q->v_along) - rho_near;
            rb_ = -q->s * c->focal;
        } else {
            us = tr_dot(up, fk->cr);
            ts = tr_dot(t, fk->cr);
            ra_ = q->s * (c->cx - q->v_along) - rho_near;
            rb_ = q->s * c->focal;
        }
#define TR_RHO(a, b) (ra_ + rb_ * ((a) * us + (b) * ts) / ((a) * uf + (b) * tf))
        while (theta < theta_max) {
            TrHit s;
            double ra, rb, z, rho, dist2, dist, ds, dt, cd, sd, x, g, dx, dy, dz;
            int i0, i1, lo, hi;
            float fog = -1;
            /* the step: three quarters of a cell near, then about two pixels' worth of the distance, longer as the
             * ray grazes the ground (edge on it is squeezed into few rows; up to 4 times) */
            ra = R * cth - D;
            rb = R * sth;
            dist2 = ra * ra + rb * rb;
            while (level < TR_LEVELS - 1 && dist2 > fk->level_d2[level])
                level++;                        /* (the grid's detail by the distance alone) */
            while (level > 0 && dist2 <= fk->level_d2[level - 1])
                level--;
            dist = sqrt(dist2);
            ds = dist * far_k;
            if (ds < near_ds)
                ds = near_ds;
            g = -(ra * cth + rb * sth);         /* the distance times the sine of the ray's angle to the ground */
            if (g < dist)
                ds = ds * dist / (g > 0.25 * dist ? g : 0.25 * dist);
            dx = up.x * cth + t.x * sth;
            dy = up.y * cth + t.y * sth;
            dz = up.z * cth + t.z * sth;
            /* in a stretch of hidden ground: skip it while a coarse bound on the ground stays under the ray it would
             * have to rise above (half a coarse cell at a time) */
            if (hidden) {
                int L = level + 1 < TR_SKIP_FIRST ? TR_SKIP_FIRST : level + 1;
                double dth, lim, c1, s1;
                while (L < TR_LEVELS - 1 && fk->skip_dth[L] * R < 4 * ds)
                    L++;                        /* (a skip worth about four steps) */
                dth = fk->skip_dth[L];
                if (hr_dirty) {
                    double hk = horizon < kmin ? kmin : horizon > len - 1 ? len - 1 : horizon;
                    TrV wr = tr_add(w0, tr_add(w1, w0, -1), len > 1 ? hk / (len - 1) : 0);
                    double l2 = sqrt(tr_dot(wr, wr));
                    hr_u = tr_dot(wr, up) / l2;
                    hr_t = tr_dot(wr, t) / l2;
                    hr_phi = -D * hr_u > 0 ? atan2(-D * hr_u * hr_t, D - D * hr_u * hr_u) : -1;
                    hr_rmin2 = D * D * hr_t * hr_t;
                    hr_dirty = 0;
                }
                lim = R + tr_bound(p, dx, dy, dz, L);
                lim *= lim;
                c1 = cth * (1 - dth * dth * 0.5) - sth * dth;
                s1 = sth * (1 - dth * dth * 0.5) + cth * dth;
                if (tr_ray_r2(D, hr_u, hr_t, cth, sth) > lim && tr_ray_r2(D, hr_u, hr_t, c1, s1) > lim
                    && !(hr_phi >= theta && hr_phi <= theta + dth && hr_rmin2 <= lim)) {
                    cth = c1;
                    sth = s1;
                    theta += dth;
                    prev_ok = prev_slab = 0;
                    skips++;
                    continue;
                }
            }
            tr_sample(p, dx, dy, dz, level, &s);
            ra = (R + s.h) * cth - D;
            rb = (R + s.h) * sth;
            z = ra * uf + rb * tf;
            n++;
            if (z > 1) {
                rho = TR_RHO(ra, rb);
                hidden = 1;
                if (nadir ? rho > horizon : rho < horizon) {
                    hidden = 0;
                    hr_dirty = 1;
                    if (nadir ? tr_span(horizon + 1e-9, rho, kmin, len, &i0, &i1) : tr_span(rho, horizon - 1e-9, kmin, len, &i0, &i1)) {
                        fog = tr_fog(p, dist, alt, s.h);
                        if (prev_ok && prev_mat == s.mat) {
                            if (prev_fog < 0)
                                prev_fog = tr_fog(p, prev_dist, alt, prev_h);
                            tr_fill_ground(px, stride, i0, i1, prev_rho, rho, prev_light, s.light, prev_fog, fog, s.mat);
                        } else
                            tr_fill_ground(px, stride, i0, i1, rho, rho, s.light, s.light, fog, fog, s.mat);
                    }
                    horizon = rho;
                }
                /* the second layer, near enough to be drawn as itself */
                if (s.slab && level < TR_SLAB_LEVELS && tr_slab(p, s.f, s.ci << level, s.cj << level, &lo, &hi)) {
                    double la = (R + lo * TR_HU) * cth - D, lb = (R + lo * TR_HU) * sth;
                    double ha = (R + hi * TR_HU) * cth - D, hb = (R + hi * TR_HU) * sth;
                    if (la * uf + lb * tf > 1 && ha * uf + hb * tf > 1) {
                        double r0 = TR_RHO(la, lb), r1 = TR_RHO(ha, hb);
                        double s0 = r0 < r1 ? r0 : r1, s1 = r0 < r1 ? r1 : r0;
                        int mat = alt < lo * TR_HU ? TR_UNDER : TR_ROCK;
                        int light = alt > hi * TR_HU ? (int)s.light : TR_LIGHTS / 3;
                        hidden = 0;
                        if (prev_slab) {
                            if (prev_lo < s0) s0 = prev_lo;
                            if (prev_hi > s1) s1 = prev_hi;
                        }
                        if (tr_span(s0, s1, kmin, len, &i0, &i1)) {
                            if (fog < 0)
                                fog = tr_fog(p, dist, alt, hi * TR_HU);
                            tr_fill(px, stride, i0, i1, (uint16_t)((mat * TR_LIGHTS + light) | (int)(fog + 0.5f) << 8));
                        }
                        prev_lo = r0 < r1 ? r0 : r1;
                        prev_hi = r0 < r1 ? r1 : r0;
                        prev_slab = 1;
                    } else
                        prev_slab = 0;
                } else
                    prev_slab = 0;
                prev_ok = 1;
                prev_rho = rho;
                prev_light = s.light;
                prev_fog = fog;   /* (-1: worked out only if the next sample needs it) */
                prev_dist = dist;
                prev_h = s.h;
                prev_mat = s.mat;
                if (nadir ? horizon >= len - 1 : horizon <= kmin)
                    break;
            } else
                prev_slab = prev_ok = 0;
            /* nothing farther can show once the ray through the filled part's edge has left the highest ground's
             * sphere behind (asked now and then) */
            if ((n & 15) == 0) {
                double hk = horizon < kmin ? kmin : horizon > len - 1 ? len - 1 : horizon;
                TrV wr = tr_norm(tr_add(w0, tr_add(w1, w0, -1), len > 1 ? hk / (len - 1) : 0));
                double wu = tr_dot(wr, up), wt = tr_dot(wr, t), b = D * wu, cc = D * D - fk->rt * fk->rt, disc = b * b - cc, s1;
                if (disc <= 0)
                    break;
                s1 = -b + sqrt(disc);
                if (s1 <= 0 || theta > atan2(s1 * wt, D + s1 * wu))
                    break;
            }
            /* on round the great circle by dt (small: its sine and cosine to the third power) */
            dt = ds / R;
            cd = 1 - dt * dt * 0.5;
            sd = dt - dt * dt * dt / 6;
            x = cth * cd - sth * sd;
            sth = sth * cd + cth * sd;
            cth = x;
            theta += dt;
        }
#undef TR_RHO
    }
    p->samples += n;
    p->skips += skips;
}

void tr_view(TrPlanet *p, const TrCam *cam)
{
    TrV eye = tr_v(cam->pos[0], cam->pos[1], cam->pos[2]), n;
    double D = sqrt(tr_dot(eye, eye)), nx, ny, nz;
    int k, i;
    p->cam = *cam;
    p->samples = p->skips = 0;
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
    p->sky_f = tr_dot(tr_scale(n, -1), tr_v(cam->fwd[0], cam->fwd[1], cam->fwd[2]));
    p->sky_r = tr_dot(tr_scale(n, -1), tr_v(cam->right[0], cam->right[1], cam->right[2]));
    p->sky_u = tr_dot(tr_scale(n, -1), tr_v(cam->up[0], cam->up[1], cam->up[2]));
    p->fog_ea = exp(-((D - p->radius) < p->bottom * TR_HU ? p->bottom * TR_HU : D - p->radius) / p->fog_height);
    {
        TrFrameK *fk = &p->fk;
        double rb = p->radius + p->bottom * TR_HU;
        fk->eye = eye;
        fk->D = D;
        fk->up = tr_scale(eye, 1 / D);
        fk->cr = tr_v(cam->right[0], cam->right[1], cam->right[2]);
        fk->cu = tr_v(cam->up[0], cam->up[1], cam->up[2]);
        fk->cf = tr_v(cam->fwd[0], cam->fwd[1], cam->fwd[2]);
        fk->alt = D - p->radius;
        fk->rt = p->radius + p->top * TR_HU;
        fk->theta_max = D > rb ? acos(rb / D) + acos(rb / fk->rt) : 3.14159265;
        if (fk->theta_max > 3.14159265)
            fk->theta_max = 3.14159265;
        fk->near_ds = p->cell * 0.75;
        fk->far_k = TR_STEP_PX / cam->focal;
        for (k = 0; k < TR_LEVELS; k++) {
            /* level k + 1 once the step's pixels' worth of the distance passes two of level k's cells */
            double d = p->cell * (2 << k) / fk->far_k;
            fk->level_d2[k] = d * d;
            fk->skip_dth[k] = p->cell * (1 << k) * 0.5 / p->radius;
        }
    }
    for (k = 0; k < 4; k++) {
        tr_quarter(p, k);
        if (p->q[k].on)
            for (i = 0; i < p->q[k].lines; i++)
                tr_line(p, &p->q[k], i);
    }
}

uint32_t tr_samples(const TrPlanet *p) { return p->samples; }
uint32_t tr_skips(const TrPlanet *p) { return p->skips; }

static const uint8_t tr_bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* a pixel the ground did not fill: the sky's pixel there hazed by its glow g (256ths of a level), dithered between
 * the two levels it lies between and then as the palette's two */
static void tr_sky_px(const TrPlanet *p, uint8_t *row, int x, int th, int g)
{
    int lev = g >> 8;
    const TrDither *d;
    if (th < ((g >> 4) & 15) && lev < TR_FOGS - 1)
        lev++;
    d = &p->sky[lev][row[x]];
    row[x] = th < d->t ? d->b : d->a;
}

/* a row's sky glow, 256ths of a level, for its columns a .. b-1: from the ray's angle from straight up, worked out
 * every 8th column and weighed between */
static void tr_sky_row(const TrPlanet *p, int y, int a, int b, uint16_t *out)
{
    const TrCam *c = &p->cam;
    double dy = (y - c->cy) / c->focal, k0 = p->sky_f - dy * p->sky_u, x;
    int x0, g0 = 0, g1;
    for (x0 = a; x0 < b; x0 += 8) {
        int x1 = x0 + 8 < b ? x0 + 8 : b, si, k;
        if (x0 == a) {
            x = (x0 - c->cx) / c->focal;
            si = (int)(((k0 + x * p->sky_r) / sqrt(1 + x * x + dy * dy) + 1) * 0.5 * TR_SKY_STEPS);
            g0 = p->skytab[si < 0 ? 0 : si > TR_SKY_STEPS - 1 ? TR_SKY_STEPS - 1 : si];
        }
        x = (x1 - c->cx) / c->focal;
        si = (int)(((k0 + x * p->sky_r) / sqrt(1 + x * x + dy * dy) + 1) * 0.5 * TR_SKY_STEPS);
        g1 = p->skytab[si < 0 ? 0 : si > TR_SKY_STEPS - 1 ? TR_SKY_STEPS - 1 : si];
        {
            int gg = g0 * 8, dg = g1 - g0;
            for (k = x0; k < x1; k++, gg += dg)
                out[k] = (uint16_t)(gg >> 3);
        }
        g0 = g1;
    }
}

uint32_t tr_blit(TrPlanet *p, uint8_t *fb, int fb_y0, int stride, int y0, int y1, int xa, int xb, const int16_t *ext,
                 int ext_y0, int ext_rows)
{
    const TrDither *gt = &p->ground[0][0];
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
    if (xb > TR_BLIT_W)
        xb = TR_BLIT_W;
    for (y = y0; y < y1; y++) {
        uint8_t *row = fb + (size_t)(y - fb_y0) * stride;
        const uint8_t *bay = tr_bayer[y & 3];
        uint16_t *glow = p->glow;
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
        if (p->sky_any)
            tr_sky_row(p, y, ca, cb, glow);
        for (k = 0; k < 4; k++) {
            TrQuarter *q = &p->q[k];
            int a0, a1;
            if (!q->on)
                continue;
            if (q->vert) {
                /* the row's part within the wedge above (below) V: its pixels' lines one after another, in 65536ths
                 * by adding */
                const uint16_t *src;
                double fi, dfi;
                int32_t fx, dfx;
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
                src = q->px + (size_t)idx * q->lines;
                dfi = (q->edge - p->vy) / dy / q->step;
                fi = (p->vx + (a0 - p->vx) * (q->edge - p->vy) / dy - q->first) / q->step + 0.5;
                while (a0 <= a1 && (fi < 0 || fi >= q->lines)) {   /* (the ends may fall just off the lines) */
                    a0++;
                    fi += dfi;
                }
                while (a1 >= a0 && (fi + dfi * (a1 - a0) < 0 || fi + dfi * (a1 - a0) >= q->lines))
                    a1--;
                fx = (int32_t)(fi * 65536);
                dfx = (int32_t)(dfi * 65536);
                if (q->thin[idx]) {
                    /* (lines thinned here: the nearest kept one, a multiple of 1 << thin) */
                    int th = q->thin[idx];
                    fx += (32768 << th) - 32768;
                    for (x = a0; x <= a1; x++, fx += dfx) {
                        int i = (fx >> (16 + th)) << th;
                        uint16_t v = src[i < q->lines ? i : i - (1 << th)];
                        if (v != TR_PX_EMPTY) {
                            const TrDither *d = gt + v;
                            row[x] = bay[x & 3] < d->t ? d->b : d->a;
                            n++;
                        } else if (p->sky_any && glow[x] >= 128) {
                            tr_sky_px(p, row, x, bay[x & 3], glow[x]);
                            n++;
                        }
                    }
                    continue;
                }
                for (x = a0; x <= a1; x++, fx += dfx) {
                    uint16_t v = src[fx >> 16];
                    if (v != TR_PX_EMPTY) {
                        const TrDither *d = gt + v;
                        row[x] = bay[x & 3] < d->t ? d->b : d->a;
                        n++;
                    } else if (p->sky_any && glow[x] >= 128) {
                        tr_sky_px(p, row, x, bay[x & 3], glow[x]);
                        n++;
                    }
                }
            } else {
                /* the parts left (right) of the wedges: a column a pixel, its line by a division */
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
                    uint16_t v;
                    if (dx == 0 || idx < 0 || idx >= q->len)
                        continue;
                    i = (int)((p->vy + dy * (q->edge - p->vx) / dx - q->first) / q->step + 0.5);
                    if (q->thin[idx]) {
                        int th = q->thin[idx];
                        i = ((i + (1 << th >> 1)) >> th) << th;
                        if (i >= q->lines)
                            i -= 1 << th;
                    }
                    if (i < 0 || i >= q->lines)
                        continue;
                    v = q->px[(size_t)idx * q->lines + i];
                    if (v != TR_PX_EMPTY) {
                        const TrDither *d = gt + v;
                        row[x] = bay[x & 3] < d->t ? d->b : d->a;
                        n++;
                    } else if (p->sky_any && glow[x] >= 128) {
                        tr_sky_px(p, row, x, bay[x & 3], glow[x]);
                        n++;
                    }
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
        double r = sqrt(tr_dot(q, q)), step;
        int lo, hi;
        TrHit hit;
        tr_sample(p, q.x / r, q.y / r, q.z / r, 0, &hit);
        if (r < p->radius + hit.h)
            return 0;
        if (hit.slab && tr_slab(p, hit.f, hit.ci, hit.cj, &lo, &hi) && r >= p->radius + lo * TR_HU && r <= p->radius + hi * TR_HU)
            return 0;
        step = (r - p->radius - hit.h) * 0.3;
        if (step < p->cell * 0.5)
            step = p->cell * 0.5;
        s += step;
    }
    return 1;
}
