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
#define TR_HU 64                        /* a height unit: 64 positions, a quarter of a km */

#define TR_MATS 8                       /* (below) */
#define TR_LIGHTS 32                    /* a material's levels of light: colour id = material * 32 + light */
#define TR_LSHIFT 5
#define TR_FOGS 32                      /* levels of haze */
#define TR_LINES_MAX 4096
#define TR_LINE_PX 2                    /* the lines' spacing where they are farthest apart, pixels (the user, 2026-11-15:
                                           "maybe we should draw wider lines or something?"): half the march's work of
                                           1, each pixel dithered between its two nearest lines (tr_blit) */
#define TR_BLIT_W 1024                  /* the widest screen drawn */
#define TR_THIN_MAX 12                  /* lines thinned to every 4096th at most */
#define TR_SCHED_MAX 16384              /* the march's steps a frame at most (tr_view): more, and each line goes its own way */
#define TR_STEP_PX 2.0                  /* the march's step in the far ground: pixels' worth of the distance */
#define TR_SKIP_FIRST 2                 /* the finest level the march skips hidden ground by */
#define TR_FOG_HS_ 1024                 /* (tr_fog's tables' sizes) */
#define TR_FOG_TAUS_ 2048               /* a quarter's lines at most (more: spaced wider than a pixel) */

enum { TR_SAND, TR_ROCK, TR_GREEN, TR_UNDER, TR_PALE, TR_SCRUB, TR_WATER, TR_DARK };   /* the materials */

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

typedef struct {                        /* a stamp's frame on the ground: its middle and two ways along it, unit */
    double c[3], a[3], b[3];
} TrFrame;

typedef struct {                        /* a mission's shape as made: its frame and its base (height units) */
    const TrStamp *s;
    TrFrame fr;
    double base;
    /* a canyon (made at its chain's first stamp; the later ones' npts is 0): its points, unit, and the floor's height
     * at each (height units); each stretch's plane (cn: unit, square to it), the way along it from its first point
     * (ct: unit, square to that point), the cosine and sine of its angle; how far from its line it changes anything */
    int npts;
    double pt[TR_CANYON_PTS][3], floor[TR_CANYON_PTS], depth[TR_CANYON_PTS];   /* (depth: under the land there) */
    double cn[TR_CANYON_PTS][3], ct[TR_CANYON_PTS][3], ccos[TR_CANYON_PTS], csin[TR_CANYON_PTS];
    double reach;
} TrShape;

#define TR_PATCHES 12                   /* detail patches at most (a mission's places with shapes, a canyon's stretches) */
#define TR_PATCH_N 384                  /* a pad's or an arch's patch: its cells a side */
#define TR_PATCH_HALF_MAX (2400.0 * 256)   /* half that patch's side at most, positions */
#define TR_PATCH_POINTS 330000          /* a canyon stretch's patch: its points at most */
#define TR_PATCH_CELL_MIN (8 * 256)     /* and its cells' side at least, positions */

typedef struct {
    int32_t c[3], a[3], b[3];           /* its middle and its two ways, unit, 2^30 = 1 (the queries' integers) */
    double cf[3], af[3], bf[3];         /* the same (the picture's) */
    double cos_reach;                   /* (a quick test: a direction nearer c than this may be in it) */
    int32_t half_a, half_b, cell;       /* half its length along a and along b, a cell's side: positions */
    int na, nb;                         /* its cells along a and along b (its points: one more each way) */
    int16_t top;                        /* the highest of it, height units */
    int16_t over;                       /* the most it stands over the planet's grid there (the march's bounds) */
    int16_t *h;                         /* (na + 1) x (nb + 1): heights over the datum, height units */
    uint8_t *col;                       /* colour ids (materials until lit) */
    int16_t *lo, *hi;                   /* a slab over each point (lo >= hi: none) */
} TrPatch;

struct TrPlanet {
    TrPlanetDesc desc;
    int32_t radius;                     /* the datum, positions */
    int32_t top, bottom;                /* the highest (a slab's top too) and lowest ground, height units */
    TrCell *g[TR_LEVELS][6];            /* the grids, bordered (tr_side) */
    int16_t *hmax[TR_LEVELS][6];        /* (levels TR_SKIP_FIRST on) the highest any ground or slab gets within a cell
                                           of each cell (tr_maxes): what the march may skip */
    int32_t *tiles[6];                  /* TR_TILES x TR_TILES: an index into slabs + 1, or 0 */
    TrSlabTile *slabs;
    TrStamp stamps[TR_STAMPS_MAX];      /* (a copy, for the making) */
    TrShape shape[TR_STAMPS_MAX];       /* (the stamps as made) */
    TrPatch patch[TR_PATCHES];
    int npatch;
    int line_patches;                   /* (the line being marched: the patches near its plane, a bit each) */
    int phase, k, sub;                  /* the making: where it has got to (tr_build: the phase, its stamp, patch, face
                                           or level, and the piece of that) */
    int16_t *raw[TR_LEVELS][6];         /* (the making's: tr_maxes's bounds before the neighbours') */
    float *own;                         /* (the making's: a patch's own shadows, tr_patch_shade) */
    int32_t relief;                     /* the highest mountain over the lowest basin, about, height units */
    float step_px;                      /* the march's far step, pixels (tr_detail; 0: TR_STEP_PX) */
    int32_t *coarse;                    /* (the making's: a face's broad octaves, tr_coarse) */
    float *shadow[6];                   /* the sun's share at each cell of level TR_SHADOW_LEVEL, 0 .. 1 */
    int nslabs, slabs_cap;
    float rgb[256][3];                  /* the colour ids' colours, 0 .. 255 */
    float fog_rgb[3];                   /* the haze's colour */
    float zenith_rgb[3];                /* the sky's colour where the air above is thin */
    double fog_density, fog_height;     /* the haze per position at the datum; its scale height, positions */
    double atmo_top;                    /* where the air is taken to end, over the datum, positions */
    double fog_floor;                   /* the haze grows no thicker under this height (the datum: a canyon's floor
                                           thousands of km under it would be lost in it), positions */
    double fog_h0, fog_hstep, fog_g[TR_FOG_HS_], fog_ea, fog_ga;   /* (tr_fog's tables, tr_fog_g's; fog_ea, fog_ga: the
                                                                     frame's eye's) */
    float fog_lev[TR_FOG_TAUS_];
    double cell;                        /* a level-0 cell's side, about, positions */
    uint8_t pal[256][3];                /* the game's palette, 8 bits */
    float pal_lab[256][3];              /* the same in L*a*b* */
    int8_t grain[TR_MATS][64][64];      /* the near ground's grain (tr_grain_make), 16ths of a light level */
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
    double (*sch)[4];                   /* the march's steps, the same for every line: the angle from V's point, its
                                           cosine and sine, the step there (tr_view); nsch of them, 0: none */
    int nsch;
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

/* octaves o0 .. o1-1 of tr_fbm's sum, not yet scaled by the whole sum's weight (31744 for five octaves) */
static int32_t tr_fbm_acc(const int32_t d[3], int32_t freq, int o0, int o1, uint32_t seed)
{
    int32_t acc = 0;
    int o;
    for (o = o0; o < o1; o++)
        acc += (int32_t)(((int64_t)tr_noise(d[0] * (freq << o), d[1] * (freq << o), d[2] * (freq << o),
                                            seed + (uint32_t)o * 0x9E3779B9U) * (16384 >> o)) >> 14);
    return acc;
}

/* the noise's broad octaves change slowly across a few cells: they are worked out every 4th cell (TR_CS a side, cell
 * i between samples (i + 1) / 4 and the next) and weighed between, in integers */
enum { TR_CO_BASIN, TR_CO_MESA, TR_CO_WET, TR_CO_RIDGE, TR_CO_HILLS, TR_CO_GORGE, TR_COARSE };
#define TR_CS (TR_N / 4 + 2)

static void tr_cell_dir(int f, int i, int j, int32_t dir[3])   /* a cell's direction, unit: 65536 = 1 */
{
    int64_t v[3], a = 2 * i + 1 - TR_N, b = 2 * j + 1 - TR_N, inv;
    uint32_t len = tr_isqrt((uint64_t)((int64_t)TR_N * TR_N + a * a + b * b));
    int k;
    tr_cell_vec(f, i, j, TR_N, v);
    inv = ((int64_t)1 << 40) / len;     /* (one division: 2^40 / len, then each component times it) */
    for (k = 0; k < 3; k++)
        dir[k] = (int32_t)((v[k] * inv) >> 24);
}

static void tr_coarse(const TrPlanet *p, int f, int32_t *out, int b0, int b1)
{
    uint32_t seed = p->desc.seed;
    int a, b;
    for (b = b0; b < b1 && b < TR_CS; b++)
        for (a = 0; a < TR_CS; a++) {
            int32_t d[3], *o = out + (b * TR_CS + a) * TR_COARSE;
            tr_cell_dir(f, 4 * a - 1, 4 * b - 1, d);
            o[TR_CO_BASIN] = tr_fbm(d, 2, 4, seed ^ 0xA5170001U);          /* broad highs and lows */
            o[TR_CO_MESA] = tr_fbm(d, 7, 3, seed ^ 0xA5170003U);           /* table lands */
            o[TR_CO_WET] = tr_fbm(d, 5, 3, seed ^ 0xA5170004U);            /* where the green grows */
            o[TR_CO_RIDGE] = tr_fbm_acc(d, 12, 0, 3, seed ^ 0xA5170002U);  /* mountains: the broad octaves */
            o[TR_CO_HILLS] = tr_fbm(d, 22, 3, seed ^ 0xA5170007U);          /* rolling hills */
            o[TR_CO_GORGE] = tr_fbm(d, 6, 2, seed ^ 0xA5170008U);          /* dry canyons where it crosses 0 */
        }
}

/* a planet's look (TR_KIND_*): its materials' colours (TR_SAND .. TR_DARK, the slots the ground's shaping fills: on
 * a green world TR_SAND is meadow and TR_DARK the snow on the crests), its haze's and its high sky's, and how its land
 * is wet: `wet` added to the wet noise where the materials are picked, `basin` how much higher the green and scrub
 * reach, `sea` how much higher the pools stand (in 64ths of the relief).  Integers where the ground is shaped, so the
 * heights stay the same on every machine; the colours are only the picture's */
typedef struct {
    float rgb[TR_MATS][3], fog[3], zenith[3];
    int32_t wet, basin, sea;
} TrLook;

static const TrLook tr_looks[TR_KINDS] = {
    {   /* desert: golden sand, red rock, a warm haze (Mirage) */
        { { 228, 188, 128 }, { 176, 100, 62 }, { 84, 132, 50 }, { 92, 60, 44 }, { 214, 190, 148 }, { 166, 150, 88 },
          { 46, 104, 122 }, { 112, 74, 60 } }, { 224, 186, 144 }, { 176, 112, 84 }, 0, 0, 0 },
    {   /* green: meadows, woods, lakes, grey rock and snow on the high crests, a blue sky */
        { { 132, 160, 72 }, { 128, 118, 104 }, { 62, 124, 48 }, { 58, 54, 50 }, { 178, 182, 140 }, { 42, 88, 42 },
          { 40, 92, 142 }, { 236, 240, 244 } }, { 196, 212, 226 }, { 92, 136, 200 }, 7000, 6000, -4 },
    {   /* ice: snow fields, blue ice, slate rock, frozen pools */
        { { 230, 236, 244 }, { 104, 114, 132 }, { 150, 192, 222 }, { 50, 56, 70 }, { 246, 248, 252 }, { 150, 154, 150 },
          { 70, 110, 150 }, { 58, 62, 74 } }, { 212, 224, 238 }, { 118, 148, 200 }, 0, 0, 0 },
    {   /* red: rust dust, dark red rock, pink flats, a salmon haze */
        { { 194, 108, 68 }, { 128, 60, 42 }, { 150, 84, 60 }, { 68, 34, 28 }, { 220, 168, 130 }, { 158, 94, 64 },
          { 80, 46, 40 }, { 88, 46, 38 } }, { 214, 150, 112 }, { 148, 82, 68 }, -6000, 0, -20 },
    {   /* ash: grey ash, black basalt, sulphur, pools of lava */
        { { 146, 142, 136 }, { 76, 72, 70 }, { 188, 176, 78 }, { 40, 38, 38 }, { 196, 192, 184 }, { 148, 138, 98 },
          { 236, 112, 36 }, { 44, 42, 46 } }, { 168, 162, 156 }, { 98, 94, 98 }, -2000, 0, 0 },
    {   /* savanna: golden grass, tan rock, olive bush, a pale blue sky */
        { { 198, 172, 92 }, { 150, 118, 88 }, { 92, 130, 56 }, { 78, 62, 50 }, { 222, 208, 160 }, { 118, 118, 58 },
          { 50, 100, 120 }, { 100, 84, 70 } }, { 222, 208, 172 }, { 128, 150, 182 }, 2500, 3000, -2 },
    {   /* violet: lilac sand, slate rock, teal growth, purple scrub */
        { { 172, 150, 182 }, { 110, 88, 122 }, { 40, 140, 128 }, { 56, 44, 66 }, { 212, 200, 222 }, { 118, 70, 130 },
          { 30, 90, 112 }, { 70, 56, 86 } }, { 192, 180, 212 }, { 110, 90, 150 }, 4000, 4000, 0 },
    {   /* ochre: yellow sulphur sand, ochre rock, a pale yellow haze */
        { { 214, 190, 96 }, { 172, 128, 52 }, { 110, 128, 56 }, { 88, 64, 34 }, { 232, 222, 170 }, { 170, 150, 72 },
          { 60, 110, 96 }, { 120, 92, 50 } }, { 226, 214, 160 }, { 170, 150, 100 }, -3000, 0, -20 },
    {   /* jungle: green all over, deep woods, rivers and lakes, dark rock, a damp haze */
        { { 70, 132, 52 }, { 96, 90, 78 }, { 46, 112, 40 }, { 44, 42, 36 }, { 150, 168, 110 }, { 30, 74, 34 },
          { 36, 86, 118 }, { 70, 66, 60 } }, { 176, 200, 182 }, { 96, 140, 160 }, 12000, 9000, -1 },
    {   /* ocean: islands in a sea, white beaches, green hills */
        { { 222, 214, 176 }, { 120, 112, 100 }, { 70, 140, 64 }, { 52, 50, 46 }, { 236, 230, 200 }, { 58, 104, 50 },
          { 30, 84, 150 }, { 230, 234, 238 } }, { 200, 220, 236 }, { 80, 128, 206 }, 14000, 12000, 10 },
    {   /* tundra: grey-green moss, slate, snow on the crests, cold pools */
        { { 134, 144, 112 }, { 98, 100, 104 }, { 96, 124, 92 }, { 46, 48, 52 }, { 214, 220, 224 }, { 110, 112, 88 },
          { 52, 86, 112 }, { 232, 236, 242 } }, { 196, 206, 214 }, { 110, 130, 164 }, 3000, 2000, 0 },
    {   /* steppe: brown earth, grey-green grass, white rock, a cold blue sky */
        { { 150, 132, 96 }, { 186, 180, 168 }, { 118, 136, 82 }, { 80, 74, 66 }, { 206, 196, 166 }, { 128, 132, 90 },
          { 54, 96, 130 }, { 150, 140, 124 } }, { 206, 206, 200 }, { 100, 128, 176 }, 1500, 2000, -2 },
    {   /* grey: a dead moon's dust, darker rock, pale flats, almost no air's colour */
        { { 164, 162, 158 }, { 110, 108, 106 }, { 128, 126, 120 }, { 46, 46, 48 }, { 208, 206, 202 }, { 140, 138, 132 },
          { 70, 70, 74 }, { 78, 78, 82 } }, { 176, 176, 180 }, { 70, 72, 84 }, -8000, 0, -20 }
};

static TrGround tr_desert(const int32_t d[3], const int32_t co[TR_COARSE], int32_t relief, uint32_t seed,
                          const TrLook *lk)
{
    TrGround g;
    int32_t basin = co[TR_CO_BASIN], mesa = co[TR_CO_MESA], wet = co[TR_CO_WET] + lk->wet;
    /* (the ridges' octaves' sum over its whole weight: five of 16384 halving, 31744) */
    int32_t ridge = (int32_t)(((int64_t)(co[TR_CO_RIDGE] + tr_fbm_acc(d, 12, 3, 5, seed ^ 0xA5170002U)) * 16384) / 31744);
    int32_t hills = co[TR_CO_HILLS];
    int32_t fine = tr_fbm(d, 192, 1, seed ^ 0xA5170005U);            /* rubble */
    int32_t mount = tr_smooth(1500, 15000, basin);                     /* mountains where the land is high */
    int32_t table = tr_smooth(5000, 8000, mesa);                       /* the mesas' steep sides */
    int32_t sand = 65536 - (mount > table ? mount : table);
    int32_t ridged = 32767 - 2 * (ridge < 0 ? -ridge : ridge);       /* sharp crests where the noise crosses 0 */
    int32_t gorge = co[TR_CO_GORGE] < 0 ? -co[TR_CO_GORGE] : co[TR_CO_GORGE];
    int32_t cut = 0, dune, h, water = -relief / 11 + relief / 64 * lk->sea;
    int64_t phase;
    h = tr_mul(relief / 4, basin * 2);                                 /* +- a quarter */
    h += tr_mul(tr_mul(relief, (ridged + 32768) * 2 / 3), mount);
    h += tr_mul(relief / 5, table);                                    /* a mesa stands a fifth high, flat on top */
    /* rolling hills everywhere, lower among the mountains (theirs are the ridges) */
    h += tr_mul(tr_mul(relief / 7, hills * 2), 65536 - mount / 2);
    /* dunes: long ridges across a wind, bent by the noise, only on the sand */
    phase = ((int64_t)d[0] * 140 + (int64_t)d[1] * 86 + (int64_t)basin * 3 + (int64_t)co[TR_CO_WET] * 2) & 0xFFFF;
    dune = (int32_t)(phase < 0x8000 ? phase : 0xFFFF - phase) * 2;    /* a triangle wave 0 .. 65535 */
    dune = tr_mul(dune, dune);
    h += tr_mul(tr_mul(tr_mul(relief / 80, dune), sand), tr_smooth(-5000, 7000, hills));   /* (fields of them, not
                                                                                              everywhere) */
    h += tr_mul(relief / 140, fine * 2);
    /* dry canyons, steep sided, cut into the high dry land where the gorge noise crosses 0 */
    if (gorge < 2000) {
        int32_t land = tr_mul(tr_smooth(-6000, 2000, basin), tr_smooth(-4000, 1000, -co[TR_CO_WET]));
        cut = tr_mul(65536 - tr_smooth(700, 2000, gorge), land);
        h -= tr_mul(tr_mul(relief / 4, cut), 65536 - mount / 3);
    }
    g.mat = TR_SAND;
    if (cut > 40000)
        g.mat = wet > 0 ? TR_SCRUB : TR_SAND;                          /* a canyon's floor: a dry wash */
    else if (mount > 30000)
        g.mat = mount > 52000 && ridged > 18000 ? TR_DARK : TR_ROCK;   /* dark crests on the high mountains */
    else if (table > 30000)
        g.mat = TR_PALE;                                               /* the mesas' tops (their sides: rock by slope) */
    else if (basin < -2000 + lk->basin && wet > 6000)
        g.mat = TR_GREEN;                                              /* the green, low and wet */
    else if (basin < 1000 + lk->basin && wet > 2500)
        g.mat = TR_SCRUB;                                              /* round it, scrub */
    else if (basin < -9000 && wet < -5000)
        g.mat = TR_PALE;                                               /* dry salt flats in the lowest dry basins */
    if (g.mat == TR_GREEN) {
        h -= tr_mul(relief / 25, tr_smooth(6000, 14000, co[TR_CO_WET]));   /* lower: the water table */
        if (h < water) {
            g.mat = TR_WATER;                                          /* and pools where it dips under it, flat */
            h = water;
        }
    }
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

static int tr_patch_find(const TrPlanet *p, const TrPatch *pt, int64_t x, int64_t y, int64_t z, int64_t *gx, int64_t *gy);

void tr_column(const TrPlanet *p, int32_t x, int32_t y, int32_t z, TrColumn *out)
{
    int32_t gi, gj, ci, cj;
    int f = tr_locate(x, y, z, TR_N, &gi, &gj), lo, hi;
    int k;
    out->slab_lo = out->slab_hi = 0;
    out->r = (int32_t)tr_isqrt((uint64_t)((int64_t)x * x + (int64_t)y * y + (int64_t)z * z));
    for (k = 0; k < p->npatch; k++) {
        /* a detail patch's ground where there is one (integers: every machine's the same) */
        const TrPatch *pt = &p->patch[k];
        int64_t gx, gy;
        if (pt->h && tr_patch_find(p, pt, x, y, z, &gx, &gy)) {
            int32_t i = (int32_t)(gx >> 16), j = (int32_t)(gy >> 16), fx = (int32_t)(gx & 0xFFFF), fy = (int32_t)(gy & 0xFFFF), a, b;
            const int16_t *h;
            size_t at;
            if (i > pt->na - 1) { i = pt->na - 1; fx = 65535; }
            if (j > pt->nb - 1) { j = pt->nb - 1; fy = 65535; }
            at = (size_t)j * (pt->na + 1) + i;
            h = pt->h + at;
            a = TR_LERP(h[0] * TR_HU, h[1] * TR_HU, fx);
            b = TR_LERP(h[pt->na + 1] * TR_HU, h[pt->na + 2] * TR_HU, fx);
            out->ground = p->radius + TR_LERP(a, b, fy);
            at += (fy >= 32768 ? pt->na + 1 : 0) + (fx >= 32768);
            if (pt->lo[at] < pt->hi[at]) {
                out->slab_lo = p->radius + pt->lo[at] * TR_HU;
                out->slab_hi = p->radius + pt->hi[at] * TR_HU;
            }
            return;
        }
    }
    out->ground = p->radius + tr_height_at(p, f, gi, gj);
    ci = (gi + 32768) >> 16;
    cj = (gj + 32768) >> 16;
    if (tr_slab(p, f, ci, cj, &lo, &hi)) {
        out->slab_lo = p->radius + lo * TR_HU;
        out->slab_hi = p->radius + hi * TR_HU;
    }
}

/* a planet-relative point moved along its line from the centre to `over` positions over the ground under it (integers:
 * a game's logic places nav points and ships so, the same on every machine) */
void tr_place(const TrPlanet *p, int32_t v[3], int32_t over)
{
    TrColumn col;
    int64_t r, want;
    int k;
    tr_column(p, v[0], v[1], v[2], &col);
    r = col.r;
    if (r <= 0)
        return;
    want = (int64_t)col.ground + over;
    for (k = 0; k < 3; k++)
        v[k] = (int32_t)(((int64_t)v[k] * want) / r);
}

int tr_from_km(const TrMissionKm *m, TrPlanetDesc *d, TrStamp st[TR_STAMPS_MAX])
{
    int k, i, n = m->n > TR_STAMPS_MAX ? TR_STAMPS_MAX : m->n;
    memset(d, 0, sizeof *d);
    d->seed = m->seed;
    d->radius = m->radius * 256;
    d->kind = m->kind < TR_KINDS ? m->kind : TR_KIND_DESERT;
    d->nstamps = (uint8_t)n;
    for (k = 0; k < n; k++) {
        const TrGroundKm *g = &m->ground[k];
        memset(&st[k], 0, sizeof st[k]);
        st[k].kind = g->kind;
        st[k].flags = g->flags;
        st[k].heading = g->heading;
        for (i = 0; i < 3; i++) {
            st[k].at[i] = (g->at[i] - m->centre[i]) * 256;
            st[k].to[i] = g->kind == TR_STAMP_CANYON ? (g->to[i] - m->centre[i]) * 256 : st[k].at[i];
        }
        if (g->kind != TR_STAMP_CANYON)
            st[k].to[0] += 1000 * 256;
        for (i = 0; i < 4; i++)
            st[k].size[i] = (int32_t)g->size[i] * 256;
    }
    return n;
}

/* (a nav point's place, km: 1 when the mission has it; nav 0, the launch, is the world's origin) */
static int tr_nav_at(const TrNavRec *navs, int nnav, int id, int32_t at[3])
{
    int i;
    at[0] = at[1] = at[2] = 0;
    for (i = 0; i < nnav; i++)
        if (navs[i].id == id) {
            at[0] = navs[i].x;
            at[1] = navs[i].y;
            at[2] = navs[i].z;
            return 1;
        }
    return id == 0;
}

void tr_from_records(const TrPlanetRec *pr, const TrNavRec *navs, int nnav, TrMissionKm *out)
{
    int k, i, n = (pr->n & 15) > 4 ? 4 : pr->n & 15, id, last;
    memset(out, 0, sizeof *out);
    out->seed = pr->seed;
    out->kind = (uint8_t)(pr->n >> 4);
    out->radius = pr->radius;
    out->centre[0] = pr->x;
    out->centre[1] = pr->y;
    out->centre[2] = pr->z;
    for (k = 0; k < n; k++) {
        const TrGroundRec *g = &pr->g[k];
        TrGroundKm *o;
        int32_t at[3], to[3];
        if (out->n == TR_STAMPS_MAX)
            break;
        if (g->kind == TR_STAMP_CANYON) {
            /* a stretch from each of its nav points to the next one the mission has, up to the one its heading names */
            int links = 0;
            if (!tr_nav_at(navs, nnav, g->nav, at))
                continue;
            for (id = g->nav + 1, last = g->heading; id <= last && id < 256 && out->n < TR_STAMPS_MAX
                 && links < TR_CANYON_PTS - 1; id++) {
                if (!tr_nav_at(navs, nnav, id, to))
                    continue;
                o = &out->ground[out->n++];
                o->kind = TR_STAMP_CANYON;
                o->flags = (uint8_t)(links++ ? TR_STAMP_JOINED : 0);
                for (i = 0; i < 4; i++)
                    o->size[i] = g->size[i];
                for (i = 0; i < 3; i++) {
                    o->at[i] = at[i];
                    o->to[i] = at[i] = to[i];
                }
            }
            continue;
        }
        o = &out->ground[out->n++];
        o->kind = g->kind;
        o->heading = g->heading;
        for (i = 0; i < 4; i++)
            o->size[i] = g->size[i];
        (void)tr_nav_at(navs, nnav, g->nav, o->at);
    }
}

/* --- the missions' stamps --- */


static void tr_unit(double v[3])
{
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* the sine and cosine of a whole number of degrees from + and * alone (the C library's may round otherwise on another
 * machine, and a shape's heights must be every machine's the same) */
static void tr_sincos_deg(int deg, double *sn, double *cs)
{
    int d = ((deg % 360) + 360) % 360, q = d / 90, k;
    double x = (d % 90) * 3.14159265358979323846 / 180, x2 = x * x, s0 = x, c0 = 1, ts = x, tc = 1;
    for (k = 1; k <= 9; k++) {          /* (Taylor's series to x^19: far under a rounding at 90 degrees) */
        ts *= -x2 / ((2 * k) * (2 * k + 1));
        tc *= -x2 / ((2 * k - 1) * (2 * k));
        s0 += ts;
        c0 += tc;
    }
    switch (q) {
    case 0: *sn = s0; *cs = c0; break;
    case 1: *sn = c0; *cs = -s0; break;
    case 2: *sn = -s0; *cs = -c0; break;
    default: *sn = -c0; *cs = s0;
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
    if (s->heading && s->kind != TR_STAMP_CANYON) {   /* turned about the ground's up (a canyon's: its last nav) */
        double ch, sh, a2[3];
        tr_sincos_deg(s->heading, &sh, &ch);
        for (k = 0; k < 3; k++)
            a2[k] = fr->a[k] * ch + fr->b[k] * sh;
        for (k = 0; k < 3; k++) {
            fr->b[k] = fr->b[k] * ch - fr->a[k] * sh;
            fr->a[k] = a2[k];
        }
    }
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

/* a stamp's shape at a point of its frame (x, y positions over the ground, h the ground there, height units): 1 when it
 * touches it, with the ground's new height, its material (-1 as it was) and a slab (lo < hi, height units) */
static int tr_shape_at(const TrShape *sh, double x, double y, double h, double *nh, int *mat, double *lo, double *hi)
{
    const TrStamp *s = sh->s;
    *nh = h;
    *mat = -1;
    *lo = *hi = 0;
    if (s->kind == TR_STAMP_PAD) {
        double r = (double)s->size[0], slope = s->size[1] > 0 ? (double)s->size[1] : 1.0, d = sqrt(x * x + y * y), w;
        if (d >= r + slope)
            return 0;
        w = d <= r ? 1.0 : 1.0 - (d - r) / slope;
        w = w * w * (3 - 2 * w);
        *nh = h + (sh->base - h) * w;
        if (w > 0.6)
            *mat = TR_PALE;             /* (a pad: packed pale ground, water and all) */
        return 1;
    }
    if (s->kind == TR_STAMP_ARCH) {
        double span = (double)s->size[0], half_w = (double)s->size[1] / 2, air = (double)s->size[2] / TR_HU;
        double thick = (double)s->size[3] / TR_HU, outer = span * (1 + thick / (air > 1 ? air : 1));
        double ax = fabs(x), ay = fabs(y), e, top;
        if (ax >= outer || ay >= half_w)
            return 0;
        /* the rock between two half ellipses: inside, `span` out and `air` up; outside, a thickness more */
        e = 1 - (ax / outer) * (ax / outer);
        top = sh->base + (air + thick) * sqrt(e > 0 ? e : 0);
        top -= thick * 0.4 * (ay / half_w) * (ay / half_w);   /* rounded across */
        if (ax < span) {
            e = 1 - (ax / span) * (ax / span);
            *lo = sh->base + air * sqrt(e > 0 ? e : 0);
            if (*lo < h + 1)
                *lo = h + 1;
            *hi = top;
            if (*hi <= *lo + 2)
                *lo = *hi = 0;
        } else if (top > h) {
            *nh = top;                  /* a foot: solid down to the ground */
            *mat = TR_ROCK;
        }
        return 1;
    }
    return 0;
}

/* --- a canyon: a chain of stamps, its points at their nav points; the floor straight from each point to the next
 * (as a ship flies between them), its depth under the ground round each point; walls in three steps of cliff and
 * ledge, the land beside raised to its rim; a river along the floor, green on its banks --- */

static double tr_smoothd(double x) { return x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x); }

/* a canyon's points, unit: its first stamp's at and to, then each joined one's to; how many */
static int tr_canyon_points(const TrPlanet *p, int k, double pt[TR_CANYON_PTS][3])
{
    int n = 0, m, i;
    for (m = k; m < p->desc.nstamps && n < TR_CANYON_PTS; m++) {
        const TrStamp *s = &p->stamps[m];
        if (s->kind != TR_STAMP_CANYON || (m > k && !(s->flags & TR_STAMP_JOINED)))
            break;
        if (m == k) {
            for (i = 0; i < 3; i++)
                pt[n][i] = (double)s->at[i];
            tr_unit(pt[n++]);
        }
        if (n < TR_CANYON_PTS) {
            for (i = 0; i < 3; i++)
                pt[n][i] = (double)s->to[i];
            tr_unit(pt[n++]);
        }
    }
    return n;
}

/* how far from its line a canyon's stamp changes the ground (positions): the floor widened, the walls, the rise */
static double tr_canyon_reach(const TrStamp *s)
{
    return s->size[0] * 0.5 * 1.6 + s->size[1] * 1.3 + s->size[3];
}

/* the ground's height (positions over the datum) at a place a unit direction and a step (positions, along two unit
 * ways) from it: integers from there on */
static int32_t tr_ground_near(const TrPlanet *p, const double u[3], const double a[3], double da, const double b[3], double db)
{
    int64_t v[3];
    int32_t gi, gj;
    int k, f;
    for (k = 0; k < 3; k++)
        v[k] = (int64_t)floor(u[k] * p->radius + a[k] * da + b[k] * db + 0.5);
    f = tr_locate(v[0], v[1], v[2], TR_N, &gi, &gj);
    return tr_height_at(p, f, gi, gj);
}

static void tr_canyon_make(TrPlanet *p, int k)
{
    TrShape *sh = &p->shape[k];
    const TrStamp *s = &p->stamps[k];
    double depth = (double)s->size[2] / TR_HU, far = (double)s->size[0] + s->size[1];
    int n, i, m;
    sh->npts = n = tr_canyon_points(p, k, sh->pt);
    sh->reach = tr_canyon_reach(s);
    for (m = 0; m + 1 < n; m++) {
        double *A = sh->pt[m], *B = sh->pt[m + 1], *N = sh->cn[m], *T = sh->ct[m], d;
        N[0] = A[1] * B[2] - A[2] * B[1];
        N[1] = A[2] * B[0] - A[0] * B[2];
        N[2] = A[0] * B[1] - A[1] * B[0];
        tr_unit(N);
        d = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
        for (i = 0; i < 3; i++)
            T[i] = B[i] - d * A[i];
        tr_unit(T);
        sh->ccos[m] = d;
        sh->csin[m] = B[0] * T[0] + B[1] * T[1] + B[2] * T[2];
    }
    for (m = 0; m < n; m++) {
        /* the land round the point: the ground there and at four places a floor and a wall away, averaged */
        const double *T = sh->ct[m < n - 1 ? m : m - 1], *N = sh->cn[m < n - 1 ? m : m - 1];
        int64_t sum = tr_ground_near(p, sh->pt[m], T, 0, N, 0) * (int64_t)4;
        sum += tr_ground_near(p, sh->pt[m], T, far, N, 0) + tr_ground_near(p, sh->pt[m], T, -far, N, 0);
        sum += tr_ground_near(p, sh->pt[m], T, 0, N, far) + tr_ground_near(p, sh->pt[m], T, 0, N, -far);
        sh->depth[m] = m ? depth : 0;   /* (its mouth, the first point, opens on the land round it) */
        sh->floor[m] = floor((double)(sum / 8) / TR_HU + 0.5) - sh->depth[m];
    }
}

static double tr_n1(const int32_t di[3], int32_t freq, uint32_t seed)   /* (value noise at a direction, -1 .. 1) */
{
    return tr_noise(di[0] * freq, di[1] * freq, di[2] * freq, seed) / 32768.0;
}

/* the canyon's walls: three steps up, each a cliff and then a ledge */
static double tr_terrace(double s)
{
    double x = s * 3;
    int k = (int)x;
    if (s <= 0)
        return 0;
    if (k >= 3)
        return 1;
    x -= k;
    return (k + (x < 0.72 ? 0.9 * tr_smoothd(x / 0.72) : 0.9 + 0.1 * (x - 0.72) / 0.28)) / 3;
}

/* a canyon's ground at a unit direction (h: the ground there, height units): 1 when it changes it, with the new height
 * and the material (-1 as it was) */
static int tr_canyon_at(const TrPlanet *p, const TrShape *sh, const double u[3], double h, double *nh, int *mat)
{
    const TrStamp *s = sh->s;
    const double R = p->radius;
    double best = 1e30, lat = 0, fl = 0, dep = 0, near_pt = 1e30, depth, half, run, rise, rim, up, d, n;
    int k, inside = 0;
    int32_t di[3];
    uint32_t seed = p->desc.seed ^ 0xCA770000U;
    *nh = h;
    *mat = -1;
    for (k = 0; k + 1 < sh->npts; k++) {
        const double *A = sh->pt[k], *N = sh->cn[k], *T = sh->ct[k];
        double l = u[0] * N[0] + u[1] * N[1] + u[2] * N[2], x = u[0] * A[0] + u[1] * A[1] + u[2] * A[2];
        double y = u[0] * T[0] + u[1] * T[1] + u[2] * T[2], w;
        if (x <= 0 || fabs(l) * R >= sh->reach)
            continue;
        w = x * sh->csin[k] - y * sh->ccos[k];   /* (the sine of the angle still to go to the next point, times the
                                                    length of u in the plane: under 0 past it) */
        if (y >= 0 && w >= 0 && fabs(l) * R < best) {
            /* the floor where the straight line from point to point passes over u (a line in polar terms) */
            double ra = R + sh->floor[k] * TR_HU, rb = R + sh->floor[k + 1] * TR_HU, rho = sqrt(x * x + y * y);
            double t = x > 0 ? y * sh->ccos[k] / (x * sh->csin[k]) : 0;   /* (how far along: tan / tan) */
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            best = fabs(l) * R;
            lat = l * R;
            fl = (ra * rb * sh->csin[k] * rho / (ra * y + rb * w) - R) / TR_HU;
            dep = sh->depth[k] + (sh->depth[k + 1] - sh->depth[k]) * t;
            inside = 1;
        }
    }
    for (k = 0; k < sh->npts; k++) {     /* (round each point: the bends and the ends) */
        double dx = u[0] - sh->pt[k][0], dy = u[1] - sh->pt[k][1], dz = u[2] - sh->pt[k][2];
        d = sqrt(dx * dx + dy * dy + dz * dz) * R;
        if (d < near_pt)
            near_pt = d;
        if (d < best) {
            best = d;
            fl = sh->floor[k];
            dep = sh->depth[k];
            inside = 0;
        }
    }
    if (best >= sh->reach)
        return 0;
    for (k = 0; k < 3; k++)
        di[k] = (int32_t)floor(u[k] * 65536 + 0.5);
    depth = (double)s->size[2] / TR_HU;
    half = s->size[0] * 0.5 * (1 + 0.6 * ((n = tr_n1(di, 44, seed + 1)) > 0 ? n : 0));   /* (bays where it widens) */
    run = s->size[1] * (1 + 0.25 * tr_n1(di, 27, seed + 2));
    rise = (double)s->size[3];
    rim = fl + dep * (1 + 0.18 * tr_n1(di, 17, seed + 3));
    /* the land beside it raised to about the rim where it is lower, a quarter of its own rise and fall kept and rough
     * on top (not a table: flat ground lit all alike reads as a sheet of one colour) */
    up = h > rim ? h : rim + (h - rim) * 0.25 + dep * 0.05 * tr_n1(di, 120, seed + 10);
    if (up < h)
        up = h;
    d = best;
    if (d < half) {
        /* the floor: sand and scrub, a river winding along the middle (straight through the bends), green banks */
        double off = inside ? half * 0.4 * tr_n1(di, 34, seed + 4) * tr_smoothd((near_pt - half) / (half * 3)) : 0;
        double dr = inside ? fabs(lat - off) : d, wr = half * 0.07 * (1 + 0.4 * tr_n1(di, 90, seed + 5));
        if (wr < 25 * 256.0)
            wr = 25 * 256.0;
        *nh = fl + depth * 0.025 * tr_n1(di, 310, seed + 6) * tr_smoothd(dr / (wr * 3));
        if (dr < wr) {
            *mat = TR_WATER;
            *nh = fl - depth * 0.012;
        } else if (dr < wr * 2.8)
            *mat = TR_GREEN;
        else
            *mat = tr_n1(di, 140, seed + 7) > 0.3 ? TR_SCRUB : TR_SAND;
        return 1;
    }
    if (d < half + run) {
        double t = (d - half) / run + 0.1 * tr_n1(di, 170, seed + 8);
        *nh = fl + (up - fl) * tr_terrace(t);
        if (up - fl > depth * 0.25)
            *mat = TR_ROCK;             /* (a wall; at the mouth, where they are low, the land's own) */
        else if (up - h > depth * 0.25)
            *mat = TR_PALE;
        return 1;
    }
    if (d < half + run + rise) {
        /* the land raised to the rim and falling away to its own, gullied */
        double t = (d - half - run) / rise, g = 1 - tr_smoothd(t + 0.12 * tr_n1(di, 96, seed + 9));
        g = g < 0 ? 0 : g > 1 ? 1 : g;
        *nh = h + (up - h) * g;
        if ((up - h) * g > depth * 0.25)
            *mat = TR_PALE;             /* (raised far: a plateau's dry top, whatever grew below) */
        return 1;
    }
    return 0;
}

/* a canyon stamped on the planet's grid, face f's rows r0 .. r1-1 (-1 .. TR_N): each cell near any of its stretches
 * once */
static void tr_canyon(TrPlanet *p, int k, int f, int r0, int r1)
{
    TrShape *sh = &p->shape[k];
    int i, j, m;
    {
        int i0 = TR_N, j0 = TR_N, i1 = -1, j1 = -1, any = 0;
        for (m = 0; m + 1 < sh->npts; m++) {
            TrFrame fr;
            int a0, b0, a1, b1;
            double reach = p->radius * sqrt((1 - sh->ccos[m]) * 0.5) * 1.05 + sh->reach;
            for (i = 0; i < 3; i++)
                fr.c[i] = sh->pt[m][i] + sh->pt[m + 1][i];
            tr_unit(fr.c);
            if (!tr_stamp_box(p, &fr, f, reach, &a0, &b0, &a1, &b1))
                continue;
            any = 1;
            if (a0 < i0) i0 = a0;
            if (b0 < j0) j0 = b0;
            if (a1 > i1) i1 = a1;
            if (b1 > j1) j1 = b1;
        }
        if (!any)
            return;
        if (j0 < r0)
            j0 = r0;
        if (j1 > r1 - 1)
            j1 = r1 - 1;
        for (j = j0; j <= j1; j++)
            for (i = i0; i <= i1; i++) {
                int64_t v[3];
                double u[3], nh;
                int mat, q;
                int16_t *h = tr_hp(p, 0, f, i, j);
                tr_cell_vec(f, i, j, TR_N, v);
                for (q = 0; q < 3; q++)
                    u[q] = (double)v[q];
                tr_unit(u);
                if (!tr_canyon_at(p, sh, u, *h, &nh, &mat))
                    continue;
                *h = (int16_t)floor(nh + 0.5);
                if (mat >= 0)
                    tr_cell(p, 0, f, i, j)->c = (uint8_t)mat;
            }
    }
}

/* a stamp on the planet's grid, its base (a pad's height, an arch's foot) taken there first and kept for the patches */
static void tr_stamp(TrPlanet *p, int k)
{
    TrShape *sh = &p->shape[k];
    int f, i, j, i0, j0, i1, j1;
    int32_t gi, gj;
    double reach;
    sh->s = &p->stamps[k];
    sh->npts = 0;
    if (sh->s->kind == TR_STAMP_CANYON)
        return;                         /* (made and stamped by tr_build, a piece at a time) */
    tr_frame(sh->s, &sh->fr);
    f = tr_locate(sh->s->at[0], sh->s->at[1], sh->s->at[2], TR_N, &gi, &gj);
    sh->base = (double)(tr_height_at(p, f, gi, gj) + (sh->s->kind == TR_STAMP_PAD ? sh->s->size[2] : 0)) / TR_HU;
    reach = sh->s->kind == TR_STAMP_PAD ? (double)sh->s->size[0] + sh->s->size[1]
          : (double)sh->s->size[0] * 2.2 + sh->s->size[1];
    for (f = 0; f < 6; f++) {
        if (!tr_stamp_box(p, &sh->fr, f, reach, &i0, &j0, &i1, &j1))
            continue;
        for (j = j0; j <= j1; j++)
            for (i = i0; i <= i1; i++) {
                int64_t v[3];
                double x, y, nh, lo, hi;
                int mat;
                int16_t *h = tr_hp(p, 0, f, i, j);
                tr_cell_vec(f, i, j, TR_N, v);
                tr_in_frame(&sh->fr, v, p->radius, &x, &y);
                if (!tr_shape_at(sh, x, y, *h, &nh, &mat, &lo, &hi))
                    continue;
                *h = (int16_t)floor(nh + 0.5);
                if (mat >= 0)
                    tr_cell(p, 0, f, i, j)->c = (uint8_t)mat;
                if (lo < hi && i >= 0 && j >= 0 && i < TR_N && j < TR_N) {
                    TrSlabTile *t = tr_tile_make(p, f, i, j);
                    int kk = (j % TR_TILE) * TR_TILE + i % TR_TILE;
                    if (t) {
                        t->lo[kk] = (int16_t)floor(lo + 0.5);
                        t->hi[kk] = (int16_t)floor(hi + 0.5);
                    }
                }
            }
    }
}

/* --- detail patches: round a mission's shapes the ground is kept again on a finer grid of its own, TR_PATCH_N cells a
 * side over a square of the tangent plane there (some 8 km a cell where the planet's are 49), made from the planet's
 * ground and finer noise, the shapes drawn on it again, lit and shadowed on its own.  The queries and the picture read
 * it where they fall in it --- */

#define TR_Q30 1073741824.0             /* (a patch's frame's integers: 2^30 = 1) */

static int32_t tr_q30(double v) { return (int32_t)floor(v * TR_Q30 + 0.5); }

static TrPatch *tr_patch_new(TrPlanet *p, const double c[3], const double a[3], const double b[3])
{
    TrPatch *pt;
    int i;
    if (p->npatch == TR_PATCHES)
        return 0;
    pt = &p->patch[p->npatch++];
    memset(pt, 0, sizeof *pt);
    for (i = 0; i < 3; i++) {
        pt->cf[i] = c[i];
        pt->af[i] = a[i];
        pt->bf[i] = b[i];
        pt->c[i] = tr_q30(c[i]);
        pt->a[i] = tr_q30(a[i]);
        pt->b[i] = tr_q30(b[i]);
    }
    return pt;
}

/* a patch per place the mission's pads and arches stand (shapes within 100 km of one another share it, a square), and
 * one along each stretch of a canyon (as long as the stretch and wide as the canyon, its cells as fine as its points
 * allow) */
static void tr_patch_plan(TrPlanet *p)
{
    int k, i, m, squares = 0;
    for (k = 0; k < p->desc.nstamps; k++) {
        const TrStamp *s = &p->stamps[k];
        TrFrame fr;
        double reach = s->kind == TR_STAMP_PAD ? (double)s->size[0] + s->size[1] : (double)s->size[0] * 2.2 + s->size[1];
        TrPatch *pt = 0;
        if (s->kind == TR_STAMP_CANYON) {
            double pts[TR_CANYON_PTS][3], across = tr_canyon_reach(s) - s->size[3] * 0.5 + 200 * 256.0;
            int n;
            if (s->flags & TR_STAMP_JOINED)
                continue;
            n = tr_canyon_points(p, k, pts);
            for (m = 0; m + 1 < n; m++) {
                double c[3], a[3], b[3], d, cs;
                for (i = 0; i < 3; i++) {
                    c[i] = pts[m][i] + pts[m + 1][i];
                    a[i] = pts[m + 1][i] - pts[m][i];
                }
                tr_unit(c);
                d = a[0] * c[0] + a[1] * c[1] + a[2] * c[2];
                for (i = 0; i < 3; i++)
                    a[i] -= d * c[i];
                tr_unit(a);
                b[0] = c[1] * a[2] - c[2] * a[1];
                b[1] = c[2] * a[0] - c[0] * a[2];
                b[2] = c[0] * a[1] - c[1] * a[0];
                pt = tr_patch_new(p, c, a, b);
                if (!pt)
                    break;
                cs = pts[m][0] * pts[m + 1][0] + pts[m][1] * pts[m + 1][1] + pts[m][2] * pts[m + 1][2];
                /* (each end on the tangent plane: the radius times tan(theta / 2), and the canyon's round end past it) */
                pt->half_a = (int32_t)(p->radius * sqrt((1 - cs) / (1 + cs)) + across);
                pt->half_b = (int32_t)across;
                pt->na = -1;            /* (sized below) */
            }
            continue;
        }
        tr_frame(s, &fr);
        for (i = 0; i < p->npatch; i++) {
            double d = (fr.c[0] - p->patch[i].cf[0]) * (fr.c[0] - p->patch[i].cf[0])
                       + (fr.c[1] - p->patch[i].cf[1]) * (fr.c[1] - p->patch[i].cf[1])
                       + (fr.c[2] - p->patch[i].cf[2]) * (fr.c[2] - p->patch[i].cf[2]);
            if (p->patch[i].na >= 0 && sqrt(d) * p->radius < 100 * 256.0)
                pt = &p->patch[i];
        }
        if (!pt) {
            pt = tr_patch_new(p, fr.c, fr.a, fr.b);
            if (!pt)
                continue;
            squares++;
        }
        reach += 300 * 256.0;           /* (room round it: the ground's own between) */
        if (reach > pt->half_a)
            pt->half_a = pt->half_b = (int32_t)(reach > TR_PATCH_HALF_MAX ? TR_PATCH_HALF_MAX : reach);
    }
    (void)squares;
    for (i = 0; i < p->npatch; i++) {
        TrPatch *pt = &p->patch[i];
        if (pt->na < 0) {
            double cell = sqrt(4.0 * pt->half_a * pt->half_b / TR_PATCH_POINTS);
            pt->cell = (int32_t)(cell < TR_PATCH_CELL_MIN ? TR_PATCH_CELL_MIN : ceil(cell));
            pt->na = 2 * (int)((pt->half_a + pt->cell - 1) / pt->cell);
            pt->nb = 2 * (int)((pt->half_b + pt->cell - 1) / pt->cell);
        } else {
            pt->cell = (pt->half_a * 2 + TR_PATCH_N - 1) / TR_PATCH_N;
            pt->na = pt->nb = TR_PATCH_N;
        }
        pt->half_a = pt->cell * pt->na / 2;
        pt->half_b = pt->cell * pt->nb / 2;
        pt->cos_reach = cos(sqrt((double)pt->half_a * pt->half_a + (double)pt->half_b * pt->half_b) * 1.06 / p->radius);
    }
    for (i = 1; i < p->npatch; i++)      /* (finest first: the queries and the picture read the first a point is in) */
        for (m = i; m > 0 && p->patch[m].cell < p->patch[m - 1].cell; m--) {
            TrPatch t = p->patch[m];
            p->patch[m] = p->patch[m - 1];
            p->patch[m - 1] = t;
        }
    for (i = 0; i < p->npatch; i++) {
        TrPatch *pt = &p->patch[i];
        size_t n = (size_t)(pt->na + 1) * (pt->nb + 1);
        pt->h = (int16_t *)calloc(n, sizeof(int16_t));
        pt->col = (uint8_t *)calloc(n, 1);
        pt->lo = (int16_t *)calloc(n, sizeof(int16_t));
        pt->hi = (int16_t *)calloc(n, sizeof(int16_t));
    }
}

/* a patch's point (i, j: its grid) as a planet-relative position, integers */
static void tr_patch_point(const TrPlanet *p, const TrPatch *pt, int i, int j, int64_t v[3])
{
    int64_t x = (int64_t)-pt->half_a + (int64_t)i * pt->cell, y = (int64_t)-pt->half_b + (int64_t)j * pt->cell;
    int k;
    for (k = 0; k < 3; k++)
        v[k] = ((int64_t)pt->c[k] * p->radius + (int64_t)pt->a[k] * x + (int64_t)pt->b[k] * y) >> 30;
}

/* where a planet-relative point (integers) falls in a patch: 1 and its place on the patch's grid in 65536ths of a cell */
static int tr_patch_find(const TrPlanet *p, const TrPatch *pt, int64_t x, int64_t y, int64_t z, int64_t *gx, int64_t *gy)
{
    int64_t sc = x * pt->c[0] + y * pt->c[1] + z * pt->c[2], k, u, v;
    if (sc <= 0)
        return 0;
    k = sc / p->radius;                 /* (the point's along c over the radius, 2^30 = 1) */
    if (k <= 0)
        return 0;
    u = (x * pt->a[0] + y * pt->a[1] + z * pt->a[2]) / k;   /* (positions on the tangent plane) */
    v = (x * pt->b[0] + y * pt->b[1] + z * pt->b[2]) / k;
    if (u <= -pt->half_a || u >= pt->half_a || v <= -pt->half_b || v >= pt->half_b)
        return 0;
    *gx = ((u + pt->half_a) << 16) / pt->cell;
    *gy = ((v + pt->half_b) << 16) / pt->cell;
    return 1;
}

static void tr_patch_base(TrPlanet *p, int n, int j0, int j1)   /* the ground under a patch (its rows j0 .. j1-1), from
                                                                   the planet's and finer noise */
{
    TrPatch *pt = &p->patch[n];
    int i, j;
    for (j = j0; j < j1 && j <= pt->nb; j++)
        for (i = 0; i <= pt->na; i++) {
            int64_t v[3];
            int32_t gi, gj, dir[3];
            uint32_t len;
            int f, k;
            size_t at = (size_t)j * (pt->na + 1) + i;
            tr_patch_point(p, pt, i, j, v);
            f = tr_locate(v[0], v[1], v[2], TR_N, &gi, &gj);
            len = tr_isqrt((uint64_t)(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            for (k = 0; k < 3; k++)
                dir[k] = (int32_t)((v[k] * 65536) / (int64_t)(len ? len : 1));
            pt->h[at] = (int16_t)((tr_height_at(p, f, gi, gj) + tr_mul(p->relief * TR_HU / 300, tr_fbm(dir, 1536, 2, p->desc.seed ^ 0xA5170006U) * 2)) / TR_HU);
            {
                int ci = (gi + 32768) >> 16, cj = (gj + 32768) >> 16;
                ci = ci < 0 ? 0 : ci > TR_N - 1 ? TR_N - 1 : ci;
                cj = cj < 0 ? 0 : cj > TR_N - 1 ? TR_N - 1 : cj;
                pt->col[at] = tr_cell(p, 0, f, ci, cj)->c;   /* (its material, not lit yet) */
            }
        }
}

static void tr_patch_stamps(TrPlanet *p, int n, int j0, int j1)   /* the mission's shapes on a patch (its rows j0 ..
                                                                     j1-1), at its finer grain */
{
    TrPatch *pt = &p->patch[n];
    int i, j, k;
    for (k = 0; k < p->desc.nstamps; k++) {
        const TrShape *sh = &p->shape[k];
        if (sh->s->kind == TR_STAMP_CANYON && !sh->npts)
            continue;                   /* (a canyon's later stretches: made with its first) */
        for (j = j0; j < j1 && j <= pt->nb; j++)
            for (i = 0; i <= pt->na; i++) {
                int64_t v[3];
                double x, y, nh, lo = 0, hi = 0;
                int mat;
                size_t at = (size_t)j * (pt->na + 1) + i;
                tr_patch_point(p, pt, i, j, v);
                if (sh->npts) {
                    double u[3];
                    int q;
                    for (q = 0; q < 3; q++)
                        u[q] = (double)v[q];
                    tr_unit(u);
                    if (!tr_canyon_at(p, sh, u, pt->h[at], &nh, &mat))
                        continue;
                } else {
                    tr_in_frame(&sh->fr, v, p->radius, &x, &y);
                    if (!tr_shape_at(sh, x, y, pt->h[at], &nh, &mat, &lo, &hi))
                        continue;
                }
                pt->h[at] = (int16_t)floor(nh + 0.5);
                if (mat >= 0)
                    pt->col[at] = (uint8_t)mat;
                if (lo < hi) {
                    pt->lo[at] = (int16_t)floor(lo + 0.5);
                    pt->hi[at] = (int16_t)floor(hi + 0.5);
                }
            }
    }
}

/* a patch's highest, and the most it stands over the planet's own grid there (the planet's has the shapes too,
 * coarser: the march's bounds are the planet's and this much): its rows j0 .. j1-1, from 0 on */
static void tr_patch_top(TrPlanet *p, int n, int j0, int j1)
{
    TrPatch *pt = &p->patch[n];
    int i, j;
    if (j0 == 0) {
        pt->top = -32768;
        pt->over = 0;
    }
    for (j = j0; j < j1 && j <= pt->nb; j++)
        for (i = 0; i <= pt->na; i++) {
            size_t at = (size_t)j * (pt->na + 1) + i;
            int64_t v[3];
            int32_t gi, gj;
            int f, ci, cj, lo, hi, mine = pt->h[at], theirs, e;
            if (pt->lo[at] < pt->hi[at] && pt->hi[at] > mine)
                mine = pt->hi[at];
            if (mine > pt->top)
                pt->top = (int16_t)mine;
            tr_patch_point(p, pt, i, j, v);
            f = tr_locate(v[0], v[1], v[2], TR_N, &gi, &gj);
            ci = (gi + 32768) >> 16;
            cj = (gj + 32768) >> 16;
            ci = ci < 0 ? 0 : ci > TR_N - 1 ? TR_N - 1 : ci;
            cj = cj < 0 ? 0 : cj > TR_N - 1 ? TR_N - 1 : cj;
            theirs = -32768;            /* (the highest of the planet's cells round it, a slab's top too: the march's
                                           bounds cover those) */
            {
                int a, b;
                for (b = cj - 1; b <= cj + 1; b++)
                    for (a = ci - 1; a <= ci + 1; a++) {
                        if (tr_cell(p, 0, f, a, b)->h > theirs)
                            theirs = tr_cell(p, 0, f, a, b)->h;
                        if (tr_slab(p, f, a, b, &lo, &hi) && hi > theirs)
                            theirs = hi;
                    }
            }
            e = mine - theirs + 1;
            if (e > pt->over)
                pt->over = (int16_t)(e > 32767 ? 32767 : e);
        }
    if (pt->top > p->top)
        p->top = pt->top;
}

/* --------------------------------------------------------------------------------------------------------------- */
/* the colours (floats: only the picture uses them) */

/* (each material's: tr_looks, by the planet's kind) */

/* the light: always from above, leaning the way the sun lies (no night side: a mission may be anywhere), at about
 * 35 degrees over the horizon */
#define TR_SUN_LEAN 1.4

static void tr_sun(double sun[3])
{
    static double u[3];
    if (u[0] == 0) {
        u[0] = 0.48;
        u[1] = 0.74;
        u[2] = 0.47;
        tr_unit(u);
    }
    sun[0] = u[0];
    sun[1] = u[1];
    sun[2] = u[2];
}

static void tr_light_dir(const double up[3], double l[3])
{
    double sun[3], d;
    int k;
    tr_sun(sun);
    d = sun[0] * up[0] + sun[1] * up[1] + sun[2] * up[2];
    for (k = 0; k < 3; k++)
        l[k] = up[k] + TR_SUN_LEAN * (sun[k] - d * up[k]);
    tr_unit(l);
}

/* a level's height at a unit direction, the four cells round it weighed, positions over the datum */
static double tr_hat(const TrPlanet *p, int level, double dx, double dy, double dz)
{
    double ax = fabs(dx), ay = fabs(dy), az = fabs(dz), m, gi, gj, fi, fj, half = (TR_N >> level) * 0.5;
    double off = ((TR_N >> level) - 1) * 0.5;
    int n = TR_N >> level, f, i, j, side = n + 2;
    const TrCell *g;
    if (ax >= ay && ax >= az) {
        m = half / ax;
        gi = dy * m + off;
        gj = dz * m + off;
        f = dx < 0;
    } else if (ay >= az) {
        m = half / ay;
        gi = dz * m + off;
        gj = dx * m + off;
        f = 2 + (dy < 0);
    } else {
        m = half / az;
        gi = dx * m + off;
        gj = dy * m + off;
        f = 4 + (dz < 0);
    }
    i = (int)(gi + 2) - 2;
    j = (int)(gj + 2) - 2;
    fi = gi - i;
    fj = gj - j;
    if (i < -1) { i = -1; fi = 0; }
    if (j < -1) { j = -1; fj = 0; }
    if (i > n - 1) { i = n - 1; fi = 1; }
    if (j > n - 1) { j = n - 1; fj = 1; }
    g = p->g[level][f] + (j + 1) * side + (i + 1);
    return ((g[0].h * (1 - fi) + g[1].h * fi) * (1 - fj) + (g[side].h * (1 - fi) + g[side + 1].h * fi) * fj) * TR_HU;
}

/* the sun's share at each cell of level TR_SHADOW_LEVEL (soft shadows): a ray towards the light, a cell of that level
 * a step, the nearest it comes to the ground over how far it has gone */
#define TR_SHADOW_LEVEL 2

static void tr_shadow_face(TrPlanet *p, int f, int j0, int j1)   /* (its rows j0 .. j1-1) */
{
    int n = TR_N >> TR_SHADOW_LEVEL, i, j, k;
    double step = p->cell * (1 << TR_SHADOW_LEVEL), rt = p->radius + p->top * TR_HU;
    for (j = j0; j < j1 && j < n; j++)
        for (i = 0; i < n; i++) {
            int64_t v[3];
            double up[3], l[3], q[3], h0, best = 1;
            tr_cell_vec(f, i, j, n, v);
            for (k = 0; k < 3; k++)
                up[k] = (double)v[k];
            tr_unit(up);
            tr_light_dir(up, l);
            h0 = tr_hat(p, TR_SHADOW_LEVEL, up[0], up[1], up[2]) + step * 0.15;
            for (k = 1; k <= 32; k++) {   /* (a cell a step near, longer farther: 64 cells out by the 32nd) */
                double r, h, clear, t = step * (k + k * k / 32.0);
                q[0] = up[0] * (p->radius + h0) + l[0] * t;
                q[1] = up[1] * (p->radius + h0) + l[1] * t;
                q[2] = up[2] * (p->radius + h0) + l[2] * t;
                r = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
                if (r > rt)
                    break;
                h = tr_hat(p, TR_SHADOW_LEVEL, q[0] / r, q[1] / r, q[2] / r);
                clear = (r - p->radius - h) / (t * 0.18);   /* (a penumbra a little under 10 degrees wide) */
                if (clear < best)
                    best = clear;
                if (best <= 0)
                    break;
            }
            p->shadow[f][j * n + i] = (float)(best < 0 ? 0 : best);
        }
}

/* rock's layers: bands by height, a little wavy, each a little lighter or darker (light levels to add; the picture only:
 * no query reads a colour).  Light, not materials: the fill blends light from one sample to the next, so a band's
 * edge stays smooth up a steep wall where a change of material would follow the samples in steps */
static double tr_tri(double x) { x -= floor(x); return x < 0.5 ? 4 * x - 1 : 3 - 4 * x; }   /* -1 .. 1 .. -1 */

static double tr_strata(double h, const double up[3])
{
    static const signed char band[8] = { 0, 2, -3, 1, 3, 5, -1, -3 };
    double b = h * TR_HU / (210 * 256.0) + 0.3 * tr_tri(up[0] * 6.5 + up[2] * 4.6) + 0.2 * tr_tri(up[1] * 10.7 - up[0] * 2.1), f;
    int k = (int)floor(b);
    f = b - k;
    f = f < 0.75 ? 0 : (f - 0.75) * 4;
    return band[k & 7] + (band[(k + 1) & 7] - band[k & 7]) * f * f * (3 - 2 * f);
}

/* one row of a face's level-0 colours: material, and light from the slope, the sun's share and how sheltered the cell
 * lies (lower than the ground round it: darker) */
static void tr_light_row(TrPlanet *p, int f, int j)
{
    int a = f >> 1, b = (a + 1) % 3, c = (a + 2) % 3, i, ns = TR_N >> TR_SHADOW_LEVEL;
    double v = (2.0 * j + 1 - TR_N) / TR_N, sv = (j - 1.5) / (1 << TR_SHADOW_LEVEL);
    int jl = j > -1 ? j - 1 : j, jr = j < TR_N ? j + 1 : j, sj0, k;
    double fsj, sq_v = sqrt(1 + v * v), cell2 = p->radius * (2.0 / TR_N), sun[3];
    static double s_sq_u[TR_N + 2];     /* (sqrt(1 + u^2) by column: the same on every face and row) */
    if (s_sq_u[0] == 0)
        for (i = -1; i <= TR_N; i++) {
            double u = (2.0 * i + 1 - TR_N) / TR_N;
            s_sq_u[i + 1] = sqrt(1 + u * u);
        }
    tr_sun(sun);
    if (sv < 0) sv = 0;
    if (sv > ns - 1) sv = ns - 1;
    sj0 = (int)sv;
    if (sj0 > ns - 2) sj0 = ns - 2;
    fsj = sv - sj0;
    for (i = -1; i <= TR_N; i++) {
        double up[3], ei[3], ej[3], n[3], l[3], u, q, inv, si, sj, gx, gy, shade, lev, su, sh, fsi, around, ao, d, e;
        int mat, il = i > -1 ? i - 1 : i, ir = i < TR_N ? i + 1 : i, si0;
        TrCell *cell = tr_cell(p, 0, f, i, j);
        /* (the cell's way up: its cube point (+-1, u, v) made unit; one square root, shared with the cells' sizes) */
        u = (2.0 * i + 1 - TR_N) / TR_N;
        q = 1 + u * u + v * v;
        inv = 1 / sqrt(q);
        up[a] = ((f & 1) ? -1 : 1) * inv;
        up[b] = u * inv;
        up[c] = v * inv;
        si = cell2 * sq_v / q;
        sj = cell2 * s_sq_u[i + 1] / q;
        gx = (double)(tr_cell(p, 0, f, ir, j)->h - tr_cell(p, 0, f, il, j)->h) * TR_HU / (si * (ir - il));
        gy = (double)(tr_cell(p, 0, f, i, jr)->h - tr_cell(p, 0, f, i, jl)->h) * TR_HU / (sj * (jr - jl));
        e = 1 / sqrt(1 - up[b] * up[b]);
        d = 1 / sqrt(1 - up[c] * up[c]);
        for (k = 0; k < 3; k++) {
            ei[k] = ((k == b) - up[k] * up[b]) * e;
            ej[k] = ((k == c) - up[k] * up[c]) * d;
            n[k] = up[k] - gx * ei[k] - gy * ej[k];
        }
        d = sun[0] * up[0] + sun[1] * up[1] + sun[2] * up[2];
        for (k = 0; k < 3; k++)
            l[k] = up[k] + TR_SUN_LEAN * (sun[k] - d * up[k]);
        shade = (n[0] * l[0] + n[1] * l[1] + n[2] * l[2])
                / sqrt((n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (l[0] * l[0] + l[1] * l[1] + l[2] * l[2]));
        /* the sun's share, from the shadow grid, weighed */
        su = (i - 1.5) / (1 << TR_SHADOW_LEVEL);
        if (su < 0) su = 0;
        if (su > ns - 1) su = ns - 1;
        si0 = (int)su;
        if (si0 > ns - 2) si0 = ns - 2;
        fsi = su - si0;
        {
            const float *sp = p->shadow[f] + sj0 * ns + si0;
            sh = (sp[0] * (1 - fsi) + sp[1] * fsi) * (1 - fsj) + (sp[ns] * (1 - fsi) + sp[ns + 1] * fsi) * fsj;
        }
        /* sheltered: under the level-2 ground's height there (a broad average: its grid weighed by the cell's place
         * on it, a quarter of the way along per cell) */
        {
            double gi = (i + 0.5) * 0.25 - 0.5, gj = (j + 0.5) * 0.25 - 0.5, fi, fj;
            int a0 = (int)floor(gi), b0 = (int)floor(gj), n2 = TR_N >> 2;
            const TrCell *g;
            if (a0 < -1) a0 = -1;
            if (b0 < -1) b0 = -1;
            if (a0 > n2 - 1) a0 = n2 - 1;
            if (b0 > n2 - 1) b0 = n2 - 1;
            fi = gi - a0;
            fj = gj - b0;
            fi = fi < 0 ? 0 : fi > 1 ? 1 : fi;
            fj = fj < 0 ? 0 : fj > 1 ? 1 : fj;
            g = tr_cell(p, 2, f, a0, b0);
            around = ((g[0].h * (1 - fi) + g[1].h * fi) * (1 - fj) + (g[n2 + 2].h * (1 - fi) + g[n2 + 3].h * fi) * fj) * TR_HU;
        }
        ao = 1 - (around - cell->h * TR_HU) / (p->relief * TR_HU * 0.04) * 0.1;
        ao = ao < 0.86 ? 0.86 : ao > 1.04 ? 1.04 : ao;
        mat = cell->c;
        if ((mat == TR_SAND || mat == TR_PALE || mat == TR_SCRUB) && gx * gx + gy * gy > 0.7)
            mat = TR_ROCK;                  /* too steep for sand */
        if (mat == TR_WATER)
            shade = 0.75;                   /* (flat, and calm) */
        lev = (0.30 + 0.92 * (shade > 0 ? shade : 0) * sh) * ao;
        lev = (lev - 0.18) / 1.05 * (TR_LIGHTS - 1);
        if (mat == TR_ROCK)
            lev = lev * 0.88 + 1 + tr_strata(cell->h, up);   /* (room left for the layers either way) */
        /* a little jitter, cell to cell (less on steep ground: a wall's cells are stretched up it into streaks) */
        lev += ((double)(tr_mix((uint32_t)(f * 1100000 + (j + 1) * 1030 + i + 1)) & 3) * 0.5 - 0.75)
               * (gx * gx + gy * gy > 0.7 ? 0.3 : 1);
        k = (int)(lev + 0.5);
        if (k < 0) k = 0;
        if (k > TR_LIGHTS - 1) k = TR_LIGHTS - 1;
        cell->c = (uint8_t)(mat * TR_LIGHTS + k);
    }
}

/* a patch's ground (bilinear, height units) and slab at a place on its plane (u, v positions) */
static double tr_patch_h(const TrPatch *pt, double u, double v)
{
    double gx = (u + pt->half_a) / pt->cell, gy = (v + pt->half_b) / pt->cell, fx, fy;
    int i, j;
    const int16_t *h;
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx > pt->na - 1e-6) gx = pt->na - 1e-6;
    if (gy > pt->nb - 1e-6) gy = pt->nb - 1e-6;
    i = (int)gx;
    j = (int)gy;
    fx = gx - i;
    fy = gy - j;
    h = pt->h + (size_t)j * (pt->na + 1) + i;
    return (h[0] * (1 - fx) + h[1] * fx) * (1 - fy) + (h[pt->na + 1] * (1 - fx) + h[pt->na + 2] * fx) * fy;
}

/* the sun's share at a patch's point from the patch's own ground (an arch's shadow on its pad, a canyon's walls on its
 * floor): a ray to the light over its plane, a cell a step near and longer farther, the nearest it comes to the ground
 * over how far it has gone (soft) */
static float tr_patch_own(const TrPlanet *p, const TrPatch *pt, int i, int j)
{
    int64_t vi[3];
    double up[3], l[3], la, lb, lu, u, v, h0, t, best = 1, far = (pt->half_a > pt->half_b ? pt->half_a : pt->half_b) * 2.0;
    int k;
    tr_patch_point(p, pt, i, j, vi);
    for (k = 0; k < 3; k++)
        up[k] = (double)vi[k];
    tr_unit(up);
    tr_light_dir(up, l);
    la = l[0] * pt->af[0] + l[1] * pt->af[1] + l[2] * pt->af[2];
    lb = l[0] * pt->bf[0] + l[1] * pt->bf[1] + l[2] * pt->bf[2];
    lu = l[0] * up[0] + l[1] * up[1] + l[2] * up[2];
    if (lu <= 0.05)
        return 1;
    u = -pt->half_a + (double)i * pt->cell;
    v = -pt->half_b + (double)j * pt->cell;
    h0 = pt->h[(size_t)j * (pt->na + 1) + i] + 0.5;
    for (t = pt->cell; t < far; t += pt->cell * (1 + t / (pt->cell * 10.0))) {
        double uu = u + la * t, vv = v + lb * t, hh = h0 + lu * t / TR_HU, g, clear;
        int gi2, gj2;
        size_t a2;
        if (uu <= -pt->half_a || uu >= pt->half_a || vv <= -pt->half_b || vv >= pt->half_b || hh > pt->top + 1)
            break;
        g = tr_patch_h(pt, uu, vv);
        gi2 = (int)((uu + pt->half_a) / pt->cell + 0.5);
        gj2 = (int)((vv + pt->half_b) / pt->cell + 0.5);
        a2 = (size_t)gj2 * (pt->na + 1) + gi2;
        clear = (hh - g) / (t / TR_HU * 0.18);
        if (pt->lo[a2] < pt->hi[a2] && hh > pt->lo[a2] - 1 && hh < pt->hi[a2] + 1)
            clear = 0;                  /* (inside an arch's rock) */
        if (clear < best)
            best = clear;
        if (best <= 0)
            break;
    }
    return (float)(best < 0 ? 0 : best > 1 ? 1 : best);
}

/* a patch's own shadows (tr_patch_own) at every other point each way, its rows j0 .. j1-1 of those (nb / 2 + 1), into
 * p->own: soft shadows need no more, and the light weighs them between */
static void tr_patch_shade(TrPlanet *p, int n, int j0, int j1)
{
    TrPatch *pt = &p->patch[n];
    int i, j, ha = pt->na / 2 + 1, hb = pt->nb / 2 + 1;
    for (j = j0; j < j1 && j < hb; j++)
        for (i = 0; i < ha; i++)
            p->own[j * ha + i] = tr_patch_own(p, pt, 2 * i < pt->na ? 2 * i : pt->na, 2 * j < pt->nb ? 2 * j : pt->nb);
}

/* a patch lit, its rows j0 .. j1-1: each point's slope, the planet's shadows over it, and the patch's own (p->own) */
static void tr_patch_light(TrPlanet *p, int n, int j0, int j1)
{
    TrPatch *pt = &p->patch[n];
    int i, j, k, ns = TR_N >> TR_SHADOW_LEVEL, ha = pt->na / 2 + 1, hb = pt->nb / 2 + 1;
    double cellh = (double)pt->cell / TR_HU;   /* (a cell's side in height units) */
    const float *own = p->own;
    const uint8_t *mat = pt->col;       /* (still its materials: each point's read before it is lit) */
    for (j = j0; j < j1 && j <= pt->nb; j++)
        for (i = 0; i <= pt->na; i++) {
            size_t at = (size_t)j * (pt->na + 1) + i;
            int il = i ? i - 1 : i, ir = i < pt->na ? i + 1 : i, jl = j ? j - 1 : j, jr = j < pt->nb ? j + 1 : j;
            int64_t vi[3];
            double up[3], nrm[3], l[3], gx, gy, shade, sh = 1, lev;
            int m = mat[at];
            tr_patch_point(p, pt, i, j, vi);
            for (k = 0; k < 3; k++)
                up[k] = (double)vi[k];
            tr_unit(up);
            gx = (pt->h[(size_t)j * (pt->na + 1) + ir] - pt->h[(size_t)j * (pt->na + 1) + il]) / (cellh * (ir - il));
            gy = (pt->h[(size_t)jr * (pt->na + 1) + i] - pt->h[(size_t)jl * (pt->na + 1) + i]) / (cellh * (jr - jl));
            for (k = 0; k < 3; k++)
                nrm[k] = up[k] - gx * pt->af[k] - gy * pt->bf[k];
            tr_unit(nrm);
            tr_light_dir(up, l);
            shade = nrm[0] * l[0] + nrm[1] * l[1] + nrm[2] * l[2];
            /* the planet's shadows here (its level-2 grid) */
            {
                int32_t gi, gj;
                int f = tr_locate(vi[0], vi[1], vi[2], ns, &gi, &gj), si = (gi + 32768) >> 16, sj = (gj + 32768) >> 16;
                si = si < 0 ? 0 : si > ns - 1 ? ns - 1 : si;
                sj = sj < 0 ? 0 : sj > ns - 1 ? ns - 1 : sj;
                sh = p->shadow[f][sj * ns + si];
            }
            {
                /* the patch's own (from its marched points, weighed) */
                int i0 = i >> 1, j0 = j >> 1, i1 = i0 + 1 < ha ? i0 + 1 : i0, j1 = j0 + 1 < hb ? j0 + 1 : j0;
                float fi = (i & 1) * 0.5f, fj = (j & 1) * 0.5f;
                sh *= (own[j0 * ha + i0] * (1 - fi) + own[j0 * ha + i1] * fi) * (1 - fj)
                      + (own[j1 * ha + i0] * (1 - fi) + own[j1 * ha + i1] * fi) * fj;
            }
            if ((m == TR_SAND || m == TR_PALE || m == TR_SCRUB) && gx * gx + gy * gy > 0.7)
                m = TR_ROCK;
            if (m == TR_WATER)
                shade = 0.75;
            lev = 0.30 + 0.92 * (shade > 0 ? shade : 0) * sh;
            lev = (lev - 0.18) / 1.05 * (TR_LIGHTS - 1);
            if (m == TR_ROCK)
                lev = lev * 0.88 + 1 + tr_strata(pt->h[at], up);
            lev += ((double)(tr_mix((uint32_t)(n * 0x10000 + j * 400 + i)) & 3) * 0.5 - 0.75) * (gx * gx + gy * gy > 0.7 ? 0.3 : 1);
            k = (int)(lev + 0.5);
            k = k < 0 ? 0 : k > TR_LIGHTS - 1 ? TR_LIGHTS - 1 : k;
            pt->col[at] = (uint8_t)(m * TR_LIGHTS + k);
        }
}

static void tr_colours(TrPlanet *p)
{
    int i, k;
    for (i = 0; i < 256; i++) {
        int mat = i / TR_LIGHTS;
        double b = 0.18 + 1.05 * (double)(i % TR_LIGHTS) / (TR_LIGHTS - 1);
        for (k = 0; k < 3; k++) {
            double x = tr_looks[p->desc.kind].rgb[mat][k] * b;
            /* shadows a little bluer (the sky's light), the sunlit a little warmer */
            x = x * (0.82 + 0.18 * b) + (k == 2 ? 18 : k == 1 ? 8 : 4) * (1 - b > 0 ? 1 - b : 0);
            p->rgb[i][k] = (float)(x > 255 ? 255 : x < 0 ? 0 : x);
        }
    }
}

/* the halvings of the grid for the far ground.  The march draws the second layer as itself down to level
 * TR_SLAB_LEVELS - 1; past that a slab counts as solid rock to its top */
#define TR_SLAB_LEVELS 5

static void tr_border(TrPlanet *p, int l)   /* a level's border cells, from the faces they really lie on */
{
    int n = TR_N >> l, f, i, j;
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

static void tr_mip_heights(TrPlanet *p, int l, int f)   /* (face f's; tr_border after the six) */
{
    int n = TR_N >> l, i, j;
    {
        for (j = 0; j < n; j++)
            for (i = 0; i < n; i++) {
                int sum = 0, k, lo, hi, slab = 0;
                for (k = 0; k < 4; k++) {
                    int ci = 2 * i + (k & 1), cj = 2 * j + (k >> 1), h = tr_cell(p, l - 1, f, ci, cj)->h;
                    if (l == TR_SLAB_LEVELS) {     /* the level-0 cells under this one: their slabs' tops */
                        int a, b, sh = TR_SLAB_LEVELS - 1;
                        for (b = cj << sh; b < (cj + 1) << sh; b++)
                            for (a = ci << sh; a < (ci + 1) << sh; a++)
                                if (tr_slab(p, f, a, b, &lo, &hi) && hi > h)
                                    h = hi;
                    }
                    sum += h;
                    slab |= tr_cell(p, l - 1, f, ci, cj)->slab;
                }
                tr_cell(p, l, f, i, j)->h = (int16_t)(sum >> 2);
                tr_cell(p, l, f, i, j)->slab = (uint8_t)slab;
            }
    }
}

static void tr_mip_colours(TrPlanet *p, int l, int f)   /* (each cell: its first child's material, the four's light;
                                                           face f's, tr_border after the six) */
{
    int n = TR_N >> l, i, j;
    {
        for (j = 0; j < n; j++)
            for (i = 0; i < n; i++) {
                int light = 0, k;
                for (k = 0; k < 4; k++)
                    light += tr_cell(p, l - 1, f, 2 * i + (k & 1), 2 * j + (k >> 1))->c % TR_LIGHTS;
                tr_cell(p, l, f, i, j)->c = (uint8_t)(tr_cell(p, l - 1, f, 2 * i, 2 * j)->c / TR_LIGHTS * TR_LIGHTS + light / 4);
            }
    }
}

/* the highest the ground (or a slab's top) gets near each cell of the levels TR_SKIP_FIRST on: over the level-0
 * cells under it and one more all round (a sample's four cells reach that far), then over its eight neighbours too
 * (the next faces' through the border), so that anywhere in the cell, a cell's width either way is under it.  The
 * march skips ground whose bound stays under the ray it would have to rise above */
static int tr_maxes(TrPlanet *p, int l, int f)   /* (level l's face f; f 6: the border, then the maxima; 0 when
                                                     memory ran out) */
{
    int n = TR_N >> l, side = n + 2, i, j;
    if (f < 6) {
        p->raw[l][f] = (int16_t *)malloc((size_t)side * side * sizeof(int16_t));
        p->hmax[l][f] = (int16_t *)malloc((size_t)n * n * sizeof(int16_t));
        if (!p->raw[l][f] || !p->hmax[l][f])
            return 0;
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
                    const int16_t *r = p->raw[l - 1][f] + (2 * j + 1) * ps + (2 * i + 1);
                    m = r[0];
                    if (r[1] > m) m = r[1];
                    if (r[ps] > m) m = r[ps];
                    if (r[ps + 1] > m) m = r[ps + 1];
                }
                p->raw[l][f][(j + 1) * side + (i + 1)] = m;
            }
        return 1;
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
                p->raw[l][f][(j + 1) * side + (i + 1)] = p->raw[l][f2][(cj + 1) * side + (ci + 1)];
            }
    for (f = 0; f < 6; f++)
        for (j = 0; j < n; j++)
            for (i = 0; i < n; i++) {
                const int16_t *r = p->raw[l][f] + j * side + i;   /* (the 3 x 3 round (i, j), bordered) */
                int16_t m = r[0];
                int a, b;
                for (b = 0; b < 3; b++)
                    for (a = 0; a < 3; a++)
                        if (r[b * side + a] > m)
                            m = r[b * side + a];
                p->hmax[l][f][j * n + i] = m;
            }
    if (l > TR_SKIP_FIRST)              /* (the level below's raw bounds done with) */
        for (f = 0; f < 6; f++) {
            free(p->raw[l - 1][f]);
            p->raw[l - 1][f] = 0;
        }
    return 1;
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

/* (lightness counts less than hue: a shaded tan is better as a darker warm entry dithered than as a cool grey of the
 * right lightness; the palette's greys lean blue) */
static double tr_d2(const float a[3], const float b[3])
{
    double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + 3 * (y * y + z * z);
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
    /* (two far apart, a yellow and a green for an olive: dithered they read as noise; one near enough does better) */
    if (best && tr_d2(p->pal_lab[a], p->pal_lab[b]) > 45 * 45 && tr_d2(t, p->pal_lab[a]) < 22 * 22)
        r.t = 0;
    return r;
}

static void tr_tables(TrPlanet *p, int fog)   /* (one haze level's) */
{
    int i, k;
    {
        double w = (double)fog / (TR_FOGS - 1);
        for (i = 0; i < 256; i++) {
            float t[3];
            for (k = 0; k < 3; k++)
                t[k] = (float)(p->rgb[i][k] + (p->fog_rgb[k] - p->rgb[i][k]) * w);
            p->ground[fog][i] = tr_dither(p, t);
            for (k = 0; k < 3; k++) {
                /* the sky: deeper where the air is thin, the haze's own colour where it is thick */
                float sky = (float)(p->zenith_rgb[k] + (p->fog_rgb[k] - p->zenith_rgb[k]) * w);
                t[k] = (float)(p->pal[i][k] + (sky - p->pal[i][k]) * w);
            }
            p->sky[fog][i] = tr_dither(p, t);
        }
    }
}

static void tr_fog_tables(TrPlanet *p);
static int tr_maxes(TrPlanet *p, int l, int f);
static void tr_grain_make(TrPlanet *p);

/* the making, a piece at a time (tr_build), so the card can make a planet between frames without the game waiting:
 * the ground of each face a row at a time from the seed (its broad octaves first), the stamps, the halvings, the
 * bounds for the march's skips, the sun's shadows, the light a block of rows at a time, the palette's tables */
enum { TR_P_GROUND, TR_P_PATCH_BASE, TR_P_STAMPS, TR_P_FLAGS, TR_P_PATCH_STAMPS, TR_P_PATCH_TOP, TR_P_MIPS, TR_P_MAXES,
       TR_P_SHADOW, TR_P_LIGHT, TR_P_PATCH_LIGHT, TR_P_MIPC, TR_P_TABLES, TR_P_DONE };
/* (rows a piece, each phase's: a few ms natively at most, so the rooms go on while the card makes it) */
#define TR_ROWS_COARSE 6
#define TR_ROWS_GROUND 8
#define TR_ROWS_CANYON 96
#define TR_ROWS_FLAGS 384
#define TR_ROWS_PATCH 12
#define TR_ROWS_PATCH_STAMPS 6
#define TR_ROWS_SHADOW 12
#define TR_ROWS_LIGHT 16
#define TR_ROWS_PATCH_SHADE 3
#define TR_BLOCKS(rows, per) (((rows) + (per) - 1) / (per))

TrPlanet *tr_begin(const TrPlanetDesc *d, const TrStamp *stamps, const uint8_t *pal)
{
    TrPlanet *p = (TrPlanet *)calloc(1, sizeof *p);
    int l, f, i, k, ns = TR_N >> TR_SHADOW_LEVEL;
    if (!p)
        return 0;
    p->desc = *d;
    if (p->desc.kind >= TR_KINDS)
        p->desc.kind = TR_KIND_DESERT;
    p->radius = d->radius;
    if (p->desc.nstamps > TR_STAMPS_MAX)
        p->desc.nstamps = TR_STAMPS_MAX;
    if (stamps && p->desc.nstamps)
        memcpy(p->stamps, stamps, p->desc.nstamps * sizeof *stamps);
    else
        p->desc.nstamps = 0;
    tr_patch_plan(p);
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
        p->shadow[f] = (float *)calloc((size_t)ns * ns, sizeof(float));
        if (!p->tiles[f] || !p->shadow[f]) {
            tr_free(p);
            return 0;
        }
    }
    p->coarse = (int32_t *)malloc((size_t)TR_CS * TR_CS * TR_COARSE * sizeof(int32_t));
    if (!p->coarse) {
        tr_free(p);
        return 0;
    }
    p->relief = d->radius / 16 / TR_HU; /* the highest mountain over the lowest basin, about: a sixteenth of the radius */
    if (p->relief > 30000)
        p->relief = 30000;
    p->cell = 2.0 * 3.14159265 * p->radius / 4 / TR_N;
    for (l = 0; l < TR_LEVELS; l++) {
        p->lv_half[l] = (TR_N >> l) * 0.5;
        p->lv_off[l] = ((TR_N >> l) - 1) * 0.5;
    }
    /* the air: a warm haze (the user: "atmospheric fog effect that depends on height to camera"), and above the
     * haze a deeper sky */
    for (k = 0; k < 3; k++) {
        p->fog_rgb[k] = tr_looks[p->desc.kind].fog[k];
        p->zenith_rgb[k] = tr_looks[p->desc.kind].zenith[k];
    }
    p->fog_height = p->relief * TR_HU / 3.5;
    p->fog_density = 0.22 / p->fog_height;   /* straight up from the datum, a fifth of the way to thick haze */
    p->atmo_top = p->fog_height * 7;
    p->fog_floor = 0;
    for (i = 0; i < 256; i++)
        for (k = 0; k < 3; k++)
            p->pal[i][k] = pal ? (uint8_t)(pal[3 * i + k] * 255 / 63) : (uint8_t)i;
    for (i = 0; i < 256; i++) {
        float c[3];
        for (k = 0; k < 3; k++)
            c[k] = p->pal[i][k];
        tr_lab(c, p->pal_lab[i]);
    }
    p->phase = TR_P_GROUND;
    p->k = p->sub = 0;
    return p;
}

/* a row of a face's ground from the seed (its broad octaves weighed from the coarse grid) */
static void tr_ground_row(TrPlanet *p, int f, int j)
{
    int i;
    for (i = -1; i <= TR_N; i++) {
        int32_t d[3], co[TR_COARSE];
        int a0 = (i + 1) >> 2, wa = (i + 1) & 3, b0 = (j + 1) >> 2, wb = (j + 1) & 3, k;
        const int32_t *c00 = p->coarse + (b0 * TR_CS + a0) * TR_COARSE, *c10 = c00 + TR_COARSE;
        const int32_t *c01 = c00 + TR_CS * TR_COARSE, *c11 = c01 + TR_COARSE;
        TrGround g;
        TrCell *cell = tr_cell(p, 0, f, i, j);
        for (k = 0; k < TR_COARSE; k++)
            co[k] = (int32_t)(((int64_t)c00[k] * (4 - wa) * (4 - wb) + (int64_t)c10[k] * wa * (4 - wb)
                               + (int64_t)c01[k] * (4 - wa) * wb + (int64_t)c11[k] * wa * wb) / 16);
        tr_cell_dir(f, i, j, d);
        g = tr_desert(d, co, p->relief, p->desc.seed, &tr_looks[p->desc.kind]);
        if (g.h > 32000) g.h = 32000;
        if (g.h < -32000) g.h = -32000;
        cell->h = (int16_t)g.h;
        cell->c = g.mat;
    }
}

/* the pieces of a phase over its k (a stamp, a patch, a face or a level) and their blocks: on to the next block, the
 * next k after `blocks`, the next phase after the last k (0: that phase is done) */
static int tr_on(TrPlanet *p, int blocks, int ks)
{
    if (++p->sub < blocks)
        return 1;
    p->sub = 0;
    return ++p->k < ks;
}

int tr_build(TrPlanet *p)
{
    int more = 1, f, i, j, lo, hi;
    switch (p->phase) {
    case TR_P_GROUND: {
        /* each face: its broad octaves a block of rows at a time, then its ground's rows -1 .. TR_N */
        int nc = TR_BLOCKS(TR_CS, TR_ROWS_COARSE), ng = TR_BLOCKS(TR_N + 2, TR_ROWS_GROUND), b = p->sub;
        if (b < nc)
            tr_coarse(p, p->k, p->coarse, b * TR_ROWS_COARSE, (b + 1) * TR_ROWS_COARSE);
        else
            for (j = -1 + (b - nc) * TR_ROWS_GROUND; j < -1 + (b - nc + 1) * TR_ROWS_GROUND && j <= TR_N; j++)
                tr_ground_row(p, p->k, j);
        more = tr_on(p, nc + ng, 6);
        break;
    }
    case TR_P_PATCH_BASE:               /* (before the shapes change the planet's ground) */
        if (p->k < p->npatch) {
            TrPatch *pt = &p->patch[p->k];
            if (!pt->h || !pt->col || !pt->lo || !pt->hi)
                return -1;
            tr_patch_base(p, p->k, p->sub * TR_ROWS_PATCH, (p->sub + 1) * TR_ROWS_PATCH);
            more = tr_on(p, TR_BLOCKS(pt->nb + 1, TR_ROWS_PATCH), p->npatch);
        } else
            more = 0;
        break;
    case TR_P_STAMPS:
        /* each stamp; a canyon's first made, then stamped a block of a face's rows at a time */
        if (p->k < p->desc.nstamps) {
            const TrStamp *st = &p->stamps[p->k];
            int nb = TR_BLOCKS(TR_N + 2, TR_ROWS_CANYON);
            if (st->kind != TR_STAMP_CANYON || (st->flags & TR_STAMP_JOINED)) {
                tr_stamp(p, p->k);
                more = tr_on(p, 1, p->desc.nstamps);
            } else {
                if (p->sub == 0) {
                    tr_stamp(p, p->k);
                    tr_canyon_make(p, p->k);
                } else {
                    int b = (p->sub - 1) % nb;
                    tr_canyon(p, p->k, (p->sub - 1) / nb, -1 + b * TR_ROWS_CANYON, -1 + (b + 1) * TR_ROWS_CANYON);
                }
                more = tr_on(p, 1 + 6 * nb, p->desc.nstamps);
            }
        } else
            more = 0;
        if (!more) {
            p->top = -32768;
            p->bottom = 32767;
        }
        break;
    case TR_P_FLAGS:
        f = p->k;
        for (j = p->sub * TR_ROWS_FLAGS; j < (p->sub + 1) * TR_ROWS_FLAGS && j < TR_N; j++)
            for (i = 0; i < TR_N; i++) {
                TrCell *cell = tr_cell(p, 0, f, i, j);
                int h = cell->h;
                cell->slab = (uint8_t)tr_slab(p, f, i, j, &lo, &hi);
                if (cell->slab && hi > h)
                    h = hi;
                if (h > p->top) p->top = h;
                if (cell->h < p->bottom) p->bottom = cell->h;
            }
        more = tr_on(p, TR_BLOCKS(TR_N, TR_ROWS_FLAGS), 6);
        break;
    case TR_P_PATCH_STAMPS:
        if (p->k < p->npatch) {
            tr_patch_stamps(p, p->k, p->sub * TR_ROWS_PATCH_STAMPS, (p->sub + 1) * TR_ROWS_PATCH_STAMPS);
            more = tr_on(p, TR_BLOCKS(p->patch[p->k].nb + 1, TR_ROWS_PATCH_STAMPS), p->npatch);
        } else
            more = 0;
        break;
    case TR_P_PATCH_TOP:
        if (p->k < p->npatch) {
            tr_patch_top(p, p->k, p->sub * TR_ROWS_PATCH, (p->sub + 1) * TR_ROWS_PATCH);
            if (p->sub == TR_BLOCKS(p->patch[p->k].nb + 1, TR_ROWS_PATCH) - 1 && p->patch[p->k].top > p->top)
                p->top = p->patch[p->k].top;
            more = tr_on(p, TR_BLOCKS(p->patch[p->k].nb + 1, TR_ROWS_PATCH), p->npatch);
        } else
            more = 0;
        break;
    case TR_P_MIPS:                     /* levels 1 on, a face at a time, then the level's border */
        if (p->sub < 6)
            tr_mip_heights(p, p->k + 1, p->sub);
        else
            tr_border(p, p->k + 1);
        more = tr_on(p, 7, TR_LEVELS - 1);
        break;
    case TR_P_MAXES:                    /* levels TR_SKIP_FIRST on, a face at a time, then the neighbours' maxima */
        if (!tr_maxes(p, TR_SKIP_FIRST + p->k, p->sub))
            return -1;
        more = tr_on(p, 7, TR_LEVELS - TR_SKIP_FIRST);
        if (!more) {
            for (f = 0; f < 6; f++) {
                free(p->raw[TR_LEVELS - 1][f]);
                p->raw[TR_LEVELS - 1][f] = 0;
            }
            tr_fog_tables(p);
        }
        break;
    case TR_P_SHADOW:
        tr_shadow_face(p, p->k, p->sub * TR_ROWS_SHADOW, (p->sub + 1) * TR_ROWS_SHADOW);
        more = tr_on(p, TR_BLOCKS(TR_N >> TR_SHADOW_LEVEL, TR_ROWS_SHADOW), 6);
        break;
    case TR_P_LIGHT:
        for (j = -1 + p->sub * TR_ROWS_LIGHT; j < -1 + (p->sub + 1) * TR_ROWS_LIGHT && j <= TR_N; j++)
            tr_light_row(p, p->k, j);
        more = tr_on(p, TR_BLOCKS(TR_N + 2, TR_ROWS_LIGHT), 6);
        break;
    case TR_P_PATCH_LIGHT:
        /* each patch: its own shadows a block of their rows at a time, then its light */
        if (p->k < p->npatch) {
            TrPatch *pt = &p->patch[p->k];
            int hb = pt->nb / 2 + 1, ns = TR_BLOCKS(hb, TR_ROWS_PATCH_SHADE), nl = TR_BLOCKS(pt->nb + 1, TR_ROWS_PATCH);
            if (p->sub == 0) {
                free(p->own);
                p->own = (float *)malloc((size_t)(pt->na / 2 + 1) * hb * sizeof(float));
                if (!p->own)
                    return -1;
            }
            if (p->sub < ns)
                tr_patch_shade(p, p->k, p->sub * TR_ROWS_PATCH_SHADE, (p->sub + 1) * TR_ROWS_PATCH_SHADE);
            else
                tr_patch_light(p, p->k, (p->sub - ns) * TR_ROWS_PATCH, (p->sub - ns + 1) * TR_ROWS_PATCH);
            more = tr_on(p, ns + nl, p->npatch);
        } else
            more = 0;
        if (!more) {
            free(p->own);
            p->own = 0;
        }
        break;
    case TR_P_MIPC:
        if (p->sub < 6)
            tr_mip_colours(p, p->k + 1, p->sub);
        else
            tr_border(p, p->k + 1);
        more = tr_on(p, 7, TR_LEVELS - 1);
        break;
    case TR_P_TABLES:
        if (p->sub == 0) {
            tr_colours(p);
            tr_grain_make(p);
        } else
            tr_tables(p, p->sub - 1);
        more = tr_on(p, 1 + TR_FOGS, 1);
        if (!more) {
            free(p->coarse);
            p->coarse = 0;
        }
        break;
    default:
        return 0;
    }
    if (!more) {
        p->phase++;
        p->k = p->sub = 0;
    }
    return p->phase == TR_P_DONE ? 0 : 1;
}

int tr_ready(const TrPlanet *p) { return p && p->phase == TR_P_DONE; }

int tr_same(const TrPlanet *p, const TrPlanetDesc *d, const TrStamp *stamps)
{
    int n = d->nstamps > TR_STAMPS_MAX ? TR_STAMPS_MAX : d->nstamps;
    if (!p || p->desc.seed != d->seed || p->desc.radius != d->radius || p->desc.kind != d->kind
        || p->desc.nstamps != (stamps ? n : 0))
        return 0;
    return !n || !stamps || !memcmp(p->stamps, stamps, (size_t)n * sizeof *stamps);
}

int tr_progress(const TrPlanet *p)       /* how far the making has got, 0 .. 99 (the ground's rows the most of it) */
{
    if (!p)
        return 0;
    if (p->phase == TR_P_GROUND) {
        int per = TR_BLOCKS(TR_CS, TR_ROWS_COARSE) + TR_BLOCKS(TR_N + 2, TR_ROWS_GROUND);
        return 40 * (p->k * per + p->sub) / (6 * per);
    }
    return p->phase >= TR_P_DONE ? 100 : 40 + 59 * p->phase / TR_P_DONE;
}

void tr_palette(TrPlanet *p, const uint8_t *pal)
{
    int i, k;
    if (!p || !pal)
        return;
    for (i = 0; i < 256; i++) {
        float c[3];
        for (k = 0; k < 3; k++) {
            p->pal[i][k] = (uint8_t)((pal[3 * i + k] & 63) * 255 / 63);
            c[k] = p->pal[i][k];
        }
        tr_lab(c, p->pal_lab[i]);
    }
    if (p->phase == TR_P_DONE)
        for (i = 0; i < TR_FOGS; i++)
            tr_tables(p, i);
}

void tr_detail(TrPlanet *p, float step_px)
{
    p->step_px = step_px < 1 ? 1 : step_px > 8 ? 8 : step_px;
}

TrPlanet *tr_make(const TrPlanetDesc *d, const TrStamp *stamps, const uint8_t *pal)
{
    TrPlanet *p = tr_begin(d, stamps, pal);
    int r;
    if (!p)
        return 0;
    while ((r = tr_build(p)) > 0)
        ;
    if (r < 0) {
        tr_free(p);
        return 0;
    }
    return p;
}

void tr_free(TrPlanet *p)
{
    int l, f;
    if (!p)
        return;
    free(p->sch);
    for (l = 0; l < TR_LEVELS; l++)
        for (f = 0; f < 6; f++) {
            free(p->g[l][f]);
            free(p->hmax[l][f]);
        }
    for (f = 0; f < 6; f++) {
        free(p->tiles[f]);
        free(p->shadow[f]);
    }
    free(p->slabs);
    free(p->coarse);
    free(p->own);
    for (l = 0; l < TR_LEVELS; l++)
        for (f = 0; f < 6; f++)
            free(p->raw[l][f]);
    for (f = 0; f < p->npatch; f++) {
        free(p->patch[f].h);
        free(p->patch[f].col);
        free(p->patch[f].lo);
        free(p->patch[f].hi);
    }
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
    int i, j;                           /* the cell below and left of the point, and the point's place past it */
    float fi, fj;
    float gx, gy;                       /* the point's place on its face's level-0 grid, in cells */
    float slo, shi;                     /* (tr_slab4's: the slab there, weighed) */
    int patch;                          /* 1 + the detail patch it was read from, 0 the planet's grid */
    int pslab;                          /* (a patch's: a slab there, slo .. shi) */
    int slab;                           /* that cell's slab mark */
    int slab4;                          /* any of the four cells round the point has one */
} TrHit;

/* a sample of the ground at a unit direction, from the grid of a level */
/* a sample from a detail patch, if the direction falls in one: 1 */
static int tr_sample_patch(const TrPlanet *p, double dx, double dy, double dz, TrHit *o)
{
    int k;
    for (k = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        double dc, u, v, gx, gy, fx, fy, w[4], mid, a, b;
        const int16_t *h, *lo, *hi;
        const uint8_t *c;
        int i, j, n, q, c0, diff, nb = pt->na + 1, off[4];
        if (!(p->line_patches >> k & 1))
            continue;
        dc = dx * pt->cf[0] + dy * pt->cf[1] + dz * pt->cf[2];
        if (dc < pt->cos_reach || !pt->h)
            continue;
        u = (dx * pt->af[0] + dy * pt->af[1] + dz * pt->af[2]) / dc * p->radius;
        v = (dx * pt->bf[0] + dy * pt->bf[1] + dz * pt->bf[2]) / dc * p->radius;
        if (u <= -pt->half_a || u >= pt->half_a || v <= -pt->half_b || v >= pt->half_b)
            continue;
        gx = (u + pt->half_a) / pt->cell;
        gy = (v + pt->half_b) / pt->cell;
        i = (int)gx;
        j = (int)gy;
        if (i > pt->na - 1) i = pt->na - 1;
        if (j > pt->nb - 1) j = pt->nb - 1;
        fx = gx - i;
        fy = gy - j;
        n = j * nb + i;
        h = pt->h + n;
        c = pt->col + n;
        lo = pt->lo + n;
        hi = pt->hi + n;
        w[0] = (1 - fx) * (1 - fy);
        w[1] = fx * (1 - fy);
        w[2] = (1 - fx) * fy;
        w[3] = fx * fy;
        off[0] = 0;
        off[1] = 1;
        off[2] = nb;
        off[3] = nb + 1;
        o->h = (h[0] * w[0] + h[1] * w[1] + h[nb] * w[2] + h[nb + 1] * w[3]) * TR_HU;
        q = (fy >= 0.5 ? nb : 0) + (fx >= 0.5);
        c0 = c[q];
        o->mat = c0 >> TR_LSHIFT;
        diff = (c[0] ^ c0) | (c[1] ^ c0) | (c[nb] ^ c0) | (c[nb + 1] ^ c0);
        if (diff < TR_LIGHTS)
            o->light = (float)((c[0] & (TR_LIGHTS - 1)) * w[0] + (c[1] & (TR_LIGHTS - 1)) * w[1]
                               + (c[nb] & (TR_LIGHTS - 1)) * w[2] + (c[nb + 1] & (TR_LIGHTS - 1)) * w[3]);
        else
            o->light = (float)(c0 & (TR_LIGHTS - 1));
        /* the slab, weighed, pinched to nothing toward a point without one (as tr_slab4) */
        mid = 0;
        for (q = n = 0; q < 4; q++)
            if (lo[off[q]] < hi[off[q]]) {
                mid += (lo[off[q]] + hi[off[q]]) * 0.5;
                n++;
            }
        o->pslab = 0;
        if (n) {
            mid /= n;
            a = b = 0;
            for (q = 0; q < 4; q++) {
                int has = lo[off[q]] < hi[off[q]];
                a += w[q] * (has ? lo[off[q]] : mid);
                b += w[q] * (has ? hi[off[q]] : mid);
            }
            if (b - a >= 0.5) {
                o->pslab = 1;
                o->slo = (float)a;
                o->shi = (float)b;
            }
        }
        o->patch = k + 1;
        o->f = 16 + k;                  /* (the grain's: its own surface) */
        o->gx = (float)(u / p->cell);
        o->gy = (float)(v / p->cell);
        o->slab = o->slab4 = 0;
        o->ci = o->cj = o->i = o->j = 0;
        o->fi = o->fj = 0;
        return 1;
    }
    return 0;
}

TR_HOT void tr_sample(const TrPlanet *p, double dx, double dy, double dz, int level, TrHit *o)
{
    double ax = fabs(dx), ay = fabs(dy), az = fabs(dz), m, gi, gj, fi, fj, off = p->lv_off[level];
    int n = TR_N >> level, i, j, side = n + 2, k, c0, diff;
    const TrCell *g;
    o->patch = 0;
    if (level <= 2 && p->line_patches && tr_sample_patch(p, dx, dy, dz, o))
        return;
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
    o->i = i;
    o->j = j;
    o->fi = (float)fi;
    o->fj = (float)fj;
    o->gx = (float)((i + fi) * (1 << level));
    o->gy = (float)((j + fj) * (1 << level));
    k = (fj >= 0.5 ? side : 0) + (fi >= 0.5);
    o->ci = i + (fi >= 0.5);
    o->cj = j + (fj >= 0.5);
    c0 = g[k].c;
    o->slab = g[k].slab;
    o->slab4 = g[0].slab | g[1].slab | g[side].slab | g[side + 1].slab;
    o->mat = c0 >> TR_LSHIFT;
    diff = (g[0].c ^ c0) | (g[1].c ^ c0) | (g[side].c ^ c0) | (g[side + 1].c ^ c0);
    if (diff < TR_LIGHTS)               /* the four of one material: their light weighed */
        o->light = (float)(((g[0].c & (TR_LIGHTS - 1)) * (1 - fi) + (g[1].c & (TR_LIGHTS - 1)) * fi) * (1 - fj)
                           + ((g[side].c & (TR_LIGHTS - 1)) * (1 - fi) + (g[side + 1].c & (TR_LIGHTS - 1)) * fi) * fj);
    else
        o->light = (float)(c0 & (TR_LIGHTS - 1));
    o->h = ((g[0].h * (1 - fi) + g[1].h * fi) * (1 - fj) + (g[side].h * (1 - fi) + g[side + 1].h * fi) * fj) * TR_HU;
}

/* the slab at a level-0 sample, its underside and top weighed between the four cells round it; a cell without one
 * counts as the slab pinched to nothing at the middle of the others, so the rock thins out to its edge rather than
 * ending in a cell's step.  0 where it has thinned away */
static int tr_slab4(const TrPlanet *p, TrHit *s, int *lo, int *hi)
{
    int l[4], h[4], has[4], k, n = 0;
    double mid = 0, w[4], a, b;
    for (k = 0; k < 4; k++) {
        has[k] = tr_slab(p, s->f, s->i + (k & 1), s->j + (k >> 1), &l[k], &h[k]);
        if (has[k]) {
            mid += (l[k] + h[k]) * 0.5;
            n++;
        }
    }
    if (!n)
        return 0;
    mid /= n;
    w[0] = (1 - s->fi) * (1 - s->fj);
    w[1] = s->fi * (1 - s->fj);
    w[2] = (1 - s->fi) * s->fj;
    w[3] = s->fi * s->fj;
    a = b = 0;
    for (k = 0; k < 4; k++) {
        a += w[k] * (has[k] ? l[k] : mid);
        b += w[k] * (has[k] ? h[k] : mid);
    }
    if (b - a < 0.5)
        return 0;
    s->slo = (float)a;
    s->shi = (float)b;
    *lo = (int)a;
    *hi = (int)b;
    return 1;
}

/* the haze between the eye and a point: a level 0 .. TR_FOGS-1 (a fraction too).  The air thins with height as
 * e^(-h/H); along a straight line from height a to height b its sum is L * (e^(-a/H) - e^(-b/H)) / ((b - a) / H)
 * (the planet taken as flat for it).  From tables: e^(-h/H) over the ground's heights, and the level by that sum (a
 * frame's eye's own e^(-a/H) is worked out once, in tr_view) */
#define TR_FOG_HS TR_FOG_HS_            /* e^(-h/H) at heights bottom .. top */
#define TR_FOG_TAUS TR_FOG_TAUS_        /* the level by the sum, 256 steps to each 1 */

/* the haze's thickness at a height, the datum's 1 (no thicker under fog_floor), and its integral up to there over the
 * scale height: G(h) = -e^(-h/H) over the floor, and under it straight on down at the floor's rate (so the haze a ray
 * passes through from height a to b, along d, is density * d * (G(b) - G(a)) / ((b - a) / H)) */
static double tr_fog_e(const TrPlanet *p, double h) { return exp(-(h > p->fog_floor ? h : p->fog_floor) / p->fog_height); }
static double tr_fog_g(const TrPlanet *p, double h)
{
    double ef = exp(-p->fog_floor / p->fog_height);
    return h >= p->fog_floor ? -exp(-h / p->fog_height) : -ef * (1 + (p->fog_floor - h) / p->fog_height);
}

static void tr_fog_tables(TrPlanet *p)
{
    int k;
    p->fog_h0 = p->bottom * TR_HU;
    p->fog_hstep = (double)(p->top - p->bottom + 1) * TR_HU / (TR_FOG_HS - 1);
    for (k = 0; k < TR_FOG_HS; k++)
        p->fog_g[k] = tr_fog_g(p, p->fog_h0 + k * p->fog_hstep);
    for (k = 0; k < TR_FOG_TAUS; k++)
        p->fog_lev[k] = (float)((1 - exp(-(k + 0.5) / 256.0)) * (TR_FOGS - 1));
}

TR_HOT float tr_fog(const TrPlanet *p, double dist, double ha, double hb)
{
    double x = (hb - ha) / p->fog_height, gb, tau;
    int k = (int)((hb - p->fog_h0) / p->fog_hstep + 0.5);
    gb = p->fog_g[k < 0 ? 0 : k > TR_FOG_HS - 1 ? TR_FOG_HS - 1 : k];
    if (fabs(x) < 1e-3)
        tau = p->fog_density * dist * p->fog_ea;
    else
        tau = p->fog_density * dist * (gb - p->fog_ga) / x;
    k = (int)(tau * 256);
    return k < 0 ? 0 : k >= TR_FOG_TAUS ? (float)(TR_FOGS - 1) : p->fog_lev[k];
}

/* the sky's glow along a ray that meets no ground, by the cosine of its angle from straight up (TR_SKY_STEPS over
 * -1 .. 1), in 256ths of a haze level: the air it passes through to the air's edge, made to glow brighter than it dims
 * the ground (the light it scatters) */
#define TR_SKY_STEPS 1024
#define TR_SKY_GLOW 20.0

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
            tau += tr_fog_e(p, h);
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
    q->first = floor((lo - 1) / TR_LINE_PX) * TR_LINE_PX;   /* (on whole multiples of the spacing: the lines keep their
                                           places as V moves) */
    q->lines = (int)ceil((hi - q->first) / TR_LINE_PX) + 2;
    q->step = TR_LINE_PX;
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
    {
        double b = (double)p->hmax[l][f][j * n + i], over = 0;
        int k;
        for (k = 0; k < p->npatch; k++)   /* (a detail patch's finer ground may stand higher, by its `over`) */
            if (p->patch[k].over > over
                && dx * p->patch[k].cf[0] + dy * p->patch[k].cf[1] + dz * p->patch[k].cf[2] > p->patch[k].cos_reach - 0.01)
                over = p->patch[k].over;
        return (b + over) * TR_HU;
    }
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
/* the fine grain of the near ground, finer than a cell (the picture only): light added per pixel from a 64 x 64
 * tile of each material's own (sand: ripples across the wind; rock: cracks and grit; green: clumps; water: little
 * waves), at 16 texels a cell and again at 64 */
#define TR_GRAIN 64

static void tr_grain_make(TrPlanet *p)
{
    int m, x, y;
    for (m = 0; m < TR_MATS; m++)
        for (y = 0; y < TR_GRAIN; y++)
            for (x = 0; x < TR_GRAIN; x++) {
                uint32_t h = tr_mix((uint32_t)(m * 7919 + y * TR_GRAIN + x) * 0x9E3779B1U);
                double n = (double)(h & 255) / 255 - 0.5, v;
                double ripple = sin((x + y * 0.45 + 3 * sin(y * 0.4)) * 6.2831853 / 5.3);
                switch (m) {
                case TR_SAND: v = ripple * 1.3 + n * 1.0; break;
                case TR_PALE: v = ripple * 0.5 + n * 0.9; break;
                case TR_GREEN: v = (h >> 8 & 7) < 3 ? -2.4 : n * 1.6; break;   /* (clumps' shadows) */
                case TR_SCRUB: v = (h >> 8 & 15) < 3 ? -2.0 : n * 1.2; break;
                case TR_WATER: v = sin((x * 0.9 + y * 0.3) * 6.2831853 / 7) * 0.8; break;
                default: v = n * 2.2 + ((h >> 9 & 31) == 0 ? -2.5 : 0);   /* rock: grit and cracks */
                }
                p->grain[m][y][x] = (int8_t)floor(v * 16 + 0.5);   /* (16ths of a light level) */
            }
}

/* fill a line's pixels i0 .. i1 that are still empty with the ground between two samples a and b: the light and the
 * haze weighed from one to the other along the rows (b's material throughout); near ground (g: the samples' places on
 * a face's level-0 grid, in cells) gets the grain too */
TR_HOT void tr_fill_ground(const TrPlanet *p, uint16_t *px, int stride, int i0, int i1, double ra, double rb, float la,
                           float lb, float fa, float fb, int mat, const float *g)
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
    if (g) {
        /* the place on the grid, in 64ths of a cell, by adding */
        const int8_t (*gr)[TR_GRAIN] = p->grain[mat];
        int32_t gx = (int32_t)((g[0] + (g[2] - g[0]) * t0) * 1024), gy = (int32_t)((g[1] + (g[3] - g[1]) * t0) * 1024);
        int32_t dgx = (int32_t)((g[2] - g[0]) * dt * 1024), dgy = (int32_t)((g[3] - g[1]) * dt * 1024);
        for (k = i0; k <= i1; k++, e += stride, l += dl, f += df, gx += dgx, gy += dgy)
            if (*e == TR_PX_EMPTY) {
                int lv = (l + (gr[(gy >> 6) & 63][(gx >> 6) & 63] + gr[(gy >> 4) & 63][(gx >> 4) & 63]) * 4096) >> 16;
                lv = lv < 0 ? 0 : lv > TR_LIGHTS - 1 ? TR_LIGHTS - 1 : lv;
                *e = (uint16_t)((base + lv) | (f >> 16) << 8);
            }
        return;
    }
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
    double prev_lo = 0, prev_hi = 0, prev_rho = 0, prev_dist = 0, prev_h = 0, prev_ds = 0;
    float prev_g[2] = { 0, 0 };
    int prev_f = -1, prev_level = 99;
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
    {
        /* the detail patches near this line's plane (its normal, up x t): only they are looked in */
        TrV nrm = tr_v(up.y * t.z - up.z * t.y, up.z * t.x - up.x * t.z, up.x * t.y - up.y * t.x);
        int k;
        p->line_patches = 0;
        for (k = 0; k < p->npatch; k++) {
            const TrPatch *pt = &p->patch[k];
            double d = fabs(nrm.x * pt->cf[0] + nrm.y * pt->cf[1] + nrm.z * pt->cf[2]);
            if (d < sqrt(1 - pt->cos_reach * pt->cos_reach) + 0.01)
                p->line_patches |= 1 << k;
        }
    }
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
        double us, ts, ra_, rb_, cth, sth;
        int ks = -1;                    /* (the march's step in p->sch it is on; -1: its own way) */
        double hr_u = 0, hr_t = 0, hr_phi = 0, hr_rmin2 = 0;   /* the ray through the filled part's edge, in the plane */
        int hr_dirty = 1, hidden = 0, level = 0;
        double in_patch = 0;                /* (the last sample's patch: its cell, the least step there) */
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
        if (p->nsch) {                  /* (the first of the steps at or past where it starts) */
            int lo = 0, hi = p->nsch;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (p->sch[mid][0] < theta) lo = mid + 1; else hi = mid;
            }
            if (lo >= p->nsch)
                return;
            ks = lo;
            theta = p->sch[ks][0];
            cth = p->sch[ks][1];
            sth = p->sch[ks][2];
        } else {
            cth = cos(theta);
            sth = sin(theta);
        }
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
            if (ds < (in_patch ? in_patch : near_ds))
                ds = in_patch ? in_patch : near_ds;   /* (over a detail patch: its finer cells) */
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
                    if (ks >= 0) {      /* (on to the next of the steps) */
                        while (ks < p->nsch && p->sch[ks][0] < theta)
                            ks++;
                        if (ks >= p->nsch)
                            break;
                        theta = p->sch[ks][0];
                        cth = p->sch[ks][1];
                        sth = p->sch[ks][2];
                    }
                    continue;
                }
            }
            tr_sample(p, dx, dy, dz, level, &s);
            in_patch = s.patch ? p->patch[s.patch - 1].cell * 1.25 : 0;
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
                            float g[4];  /* (the grain where both samples are near, on one face) */
                            if (prev_fog < 0)
                                prev_fog = tr_fog(p, prev_dist, alt, prev_h);
                            g[0] = prev_g[0];
                            g[1] = prev_g[1];
                            g[2] = s.gx;
                            g[3] = s.gy;
                            tr_fill_ground(p, px, stride, i0, i1, prev_rho, rho, prev_light, s.light, prev_fog, fog, s.mat,
                                           level == 0 && prev_level == 0 && prev_f == s.f ? g : 0);
                        } else if (prev_ok && fabs(s.h - prev_h) < prev_ds * 0.5) {
                            /* (another material on gentle ground: the rows nearer the last sample its, the rest this
                             * one's; up a steep face it is all this one's) */
                            double midr = (prev_rho + rho) * 0.5;
                            int m0, m1;
                            if (prev_fog < 0)
                                prev_fog = tr_fog(p, prev_dist, alt, prev_h);
                            if (nadir ? tr_span(horizon + 1e-9, midr, kmin, len, &m0, &m1) : tr_span(midr, horizon - 1e-9, kmin, len, &m0, &m1))
                                tr_fill_ground(p, px, stride, m0, m1, prev_rho, prev_rho, prev_light, prev_light, prev_fog, prev_fog, prev_mat, 0);
                            tr_fill_ground(p, px, stride, i0, i1, rho, rho, s.light, s.light, fog, fog, s.mat, 0);
                        } else if (prev_ok) {
                            /* (up a steep face, or onto another material: this one's, shaded from the last sample's
                             * light at its foot to this one's at its top, so that neighbouring lines do not stripe) */
                            if (prev_fog < 0)
                                prev_fog = tr_fog(p, prev_dist, alt, prev_h);
                            tr_fill_ground(p, px, stride, i0, i1, prev_rho, rho, prev_light, s.light, prev_fog, fog, s.mat, 0);
                        } else
                            tr_fill_ground(p, px, stride, i0, i1, rho, rho, s.light, s.light, fog, fog, s.mat, 0);
                    }
                    horizon = rho;
                }
                /* the second layer, near enough to be drawn as itself */
                if (s.patch ? s.pslab
                    : level == 0 ? s.slab4 && tr_slab4(p, &s, &lo, &hi)
                    : s.slab && level < TR_SLAB_LEVELS && tr_slab(p, s.f, s.ci << level, s.cj << level, &lo, &hi)) {
                    double flo = lo, fhi = hi, la, lb, ha, hb;
                    if (s.patch) {
                        lo = (int)s.slo;
                        hi = (int)s.shi;
                    }
                    if (level == 0 || s.patch) {
                        flo = s.slo;
                        fhi = s.shi;
                    }
                    la = (R + flo * TR_HU) * cth - D;
                    lb = (R + flo * TR_HU) * sth;
                    ha = (R + fhi * TR_HU) * cth - D;
                    hb = (R + fhi * TR_HU) * sth;
                    if (la * uf + lb * tf > 1 && ha * uf + hb * tf > 1) {
                        double r0 = TR_RHO(la, lb), r1 = TR_RHO(ha, hb);
                        double s0 = r0 < r1 ? r0 : r1, s1 = r0 < r1 ? r1 : r0;
                        int mat = alt < flo * TR_HU ? TR_UNDER : TR_ROCK;
                        /* the top lit as the ground; the underside by the sand's glow below it, brighter low down */
                        double gap = (flo * TR_HU - s.h) / (p->relief * TR_HU * 0.08);
                        int light = alt > fhi * TR_HU ? (int)s.light : (int)(TR_LIGHTS * (0.24 + 0.3 * exp(-(gap > 0 ? gap : 0))));
                        hidden = 0;
                        if (prev_slab) {
                            if (prev_lo < s0) s0 = prev_lo;
                            if (prev_hi > s1) s1 = prev_hi;
                        }
                        if (tr_span(s0, s1, kmin, len, &i0, &i1)) {
                            if (fog < 0)
                                fog = tr_fog(p, dist, alt, fhi * TR_HU);
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
                prev_ds = ds;
                prev_mat = s.mat;
                prev_g[0] = s.gx;
                prev_g[1] = s.gy;
                prev_f = s.f;
                prev_level = level;
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
            if (ks >= 0 && ds >= p->sch[ks][3] * 0.999) {
                /* on to the next of the steps every line takes */
                while (ks < p->nsch && p->sch[ks][0] <= theta)
                    ks++;
                if (ks >= p->nsch)
                    break;
                theta = p->sch[ks][0];
                cth = p->sch[ks][1];
                sth = p->sch[ks][2];
                continue;
            }
            /* (finer, over a detail patch, or with no steps made: its own way) on round the great circle by dt
             * (small: its sine and cosine to the third power) */
            dt = ds / R;
            cd = 1 - dt * dt * 0.5;
            sd = dt - dt * dt * dt / 6;
            x = cth * cd - sth * sd;
            sth = sth * cd + cth * sd;
            cth = x;
            theta += dt;
            if (ks >= 0)
                while (ks + 1 < p->nsch && p->sch[ks + 1][0] <= theta)
                    ks++;
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
    p->fog_ea = tr_fog_e(p, D - p->radius);
    p->fog_ga = tr_fog_g(p, D - p->radius);
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
        fk->far_k = (p->step_px > 0 ? p->step_px : TR_STEP_PX) / cam->focal;
        for (k = 0; k < TR_LEVELS; k++) {
            /* level k + 1 once TR_STEP_PX pixels' worth of the distance passes one of level k's cells, whatever the
             * detail: the levels the picture had at the card's coarsest (4 pixels a step, the level at two cells),
             * which the user liked ("The sharpness of the mountains is pretty cool ... I'd worry we would take too
             * much of that away"); at finer detail the steps fall a cell apart, so the far ground no longer crawls
             * (the coarser levels are averages of the finer: a step over two cells skipped one; a view over the
             * mountains at 2 pixels: 13% of the ground changing a frame, 5% now) */
            double d = p->cell * (1 << k) * cam->focal / TR_STEP_PX;
            fk->level_d2[k] = d * d;
            fk->skip_dth[k] = p->cell * (1 << k) * 0.5 / p->radius;
        }
        /* the march's steps: every line lies in a plane through the eye and the point under it, so how far a step
         * goes depends on the angle from that point alone; every line takes the same angles (from wherever it
         * starts, and after a skip), so the ground's faces are cut alike on neighbouring lines, not striped */
        p->nsch = 0;
        if (!p->sch)
            p->sch = (double (*)[4])malloc(TR_SCHED_MAX * sizeof *p->sch);
        if (p->sch) {
            double th = 0, R = p->radius;
            int n = 0;
            while (th < fk->theta_max && n < TR_SCHED_MAX) {
                double c = cos(th), s = sin(th), ra = R * c - D, rb = R * s, dist = sqrt(ra * ra + rb * rb),
                       ds = dist * fk->far_k, g = -(ra * c + rb * s);
                if (ds < fk->near_ds)
                    ds = fk->near_ds;
                if (g < dist)
                    ds = ds * dist / (g > 0.25 * dist ? g : 0.25 * dist);
                p->sch[n][0] = th;
                p->sch[n][1] = c;
                p->sch[n][2] = s;
                p->sch[n][3] = ds;
                n++;
                th += ds / R;
            }
            p->nsch = n < TR_SCHED_MAX ? n : 0;
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
                fx -= 32768 - 2048;     /* (each pixel from one of its two nearest lines, by the screen's 4 x 4: the
                                           lines blended, not one line's run of a colour beside the next's) */
                for (x = a0; x <= a1; x++, fx += dfx) {
                    int32_t fl = (fx + (bay[x & 3] << 12)) >> 16;
                    uint16_t v = src[fl < 0 ? 0 : fl >= q->lines ? q->lines - 1 : fl];
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
                    i = (int)floor((p->vy + dy * (q->edge - p->vx) / dx - q->first) / q->step + (bay[x & 3] + 0.5) / 16);
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

int tr_seen(const TrPlanet *p0, const double eye[3], const double pt[3])
{
    TrPlanet *p = (TrPlanet *)p0;       /* (every patch looked in: its line is any) */
    TrV e = tr_v(eye[0], eye[1], eye[2]), d = tr_v(pt[0] - eye[0], pt[1] - eye[1], pt[2] - eye[2]);
    double len = sqrt(tr_dot(d, d)), rt = p->radius + p->top * TR_HU, b, c, disc, s, s1;
    int guard = 0;
    p->line_patches = (1 << p->npatch) - 1;
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
