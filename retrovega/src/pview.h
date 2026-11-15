/* pview.h: a planet's ground drawn by RV.EXE itself, without the sprite card, at 128 x 80 (pview.c).  The user,
 * 2026-11-14: "Lets use the bake and do 128x80 size", and "hopefully we can make the bake look just as nice as the
 * other version".
 *
 * The picture is terrain.c's own way of drawing, at a fifth of the screen's size each way: the screen cut into four
 * quarters round V (the point straight down, or straight up, on the screen), each quarter's lines running out from V,
 * each line's pixels filled front to back by marching out along the ground's great circle under it.  What is new is
 * where the ground comes from: not terrain.c's 40 MB of floats, but a pyramid of grids baked into the mission's .PLN
 * (tools/planetbake.c), laid on the plane that touches the planet at the mission's middle.  Seen from the planet's
 * centre (gnomonic), every great circle is a straight line on that plane, so a line's samples walk a straight line
 * there: each a division and two products from its step's own numbers.
 *
 *   - Level l's cells are 1 << (shift0 + l) plane units a side (a plane unit 1/16 km; level 0 16 km).  Finer than the
 *     planet's own grid (terrain.c's 65 km) a level has cells only round the mission's detail patches; coarser, it
 *     covers the flight's region and the ground seen from it, out to four times the radius on the plane (76 degrees
 *     from the middle).  Past every level the ground is the plain sphere, coloured far_c.
 *   - The march goes a level at a time, every line's: a window of PV_W x PV_W cells of the level round the eye's
 *     point is all a march at that level reads (in a 32 KB buffer on DOS); a line leaves the level at its first sample
 *     the window or the level has no ground for, or whose step is too long for its cells, and goes on at the next.
 *     The finer levels' ground reaches half a window past each patch, so a line leaving one has left its patch.
 *   - The picture is 128 x 80 pixels of (colour id, haze) (terrain.c's px), each drawn as a 5 x 5 block of the
 *     palette's two nearest colours dithered (the table baked from the flight palette's shared colours, 16 .. 207).
 *
 * Every number is an integer, so the asm (pview.asm, DOS) and its C twins here draw the same pixels to the bit. */
#ifndef PVIEW_H
#define PVIEW_H

#include "rvtypes.h"

#ifdef __WATCOMC__
#define PV_FAR __far
#else
#define PV_FAR
#endif

#define PV_PU 16                        /* positions a plane unit */
#define PV_LEVELS 12
#define PV_W 96                         /* a window's cells a side */
#define PV_SNAP 4                       /* a window's corner moves this many cells at a time */
#define PV_CELL 3                       /* a cell's bytes: its height (int16, BK_HU units over the datum), colour id */
#define PV_ABSENT 0x80                  /* a cell's height's high byte where its level has no ground */
#define PV_FOGS 32
#define PV_LIGHTS 32                    /* (terrain.c's TR_LIGHTS: a colour id is its material * 32 + its light) */
#define PV_FOG_GS 256                   /* the haze's table: G at heights h0 + k hstep */
#define PV_FOG_HB 64                    /* the frame's haze by height: this many heights over bottom .. top */
#define PV_FOG_TAUS 2048
#define PV_TMAX 4                       /* the levels reach this times the radius on the plane */

#define PV_LW 128                       /* the picture */
#define PV_LH 80
#define PV_BLOCK 5                      /* a picture pixel's screen pixels each way */
#define PV_EMPTY 0xFFFF                 /* a picture pixel the ground has not filled */
#define PV_GLOW 0xE000                  /* a picture pixel of sky glowing: PV_GLOW | its haze level */
#define PV_DITHER_LEVELS 11             /* a dither's t: 0 .. 10 of the block's 25 pixels' thresholds (pv_blit) */

#ifdef __WATCOMC__
#pragma pack(1)
#else
#pragma pack(push, 1)
#endif
typedef struct {
    char magic[4];                      /* "RVPV" */
    int32_t e[3], a[3], b[3];           /* the plane: the way from the centre it touches the planet, its two ways, 2^30 */
    int32_t radius;                     /* the datum's, plane units */
    uint8_t nlev, shift0, w, snap;      /* levels; level 0's cell 1 << shift0 plane units; PV_W, PV_SNAP when made */
    int16_t top, bottom;                /* the highest and lowest ground, BK_HU */
    uint8_t far_c, pad_[3];             /* the plain sphere's colour id, past every level */
    uint32_t lev_at;                    /* nlev PvLevel (offsets from the file's start, all of them) */
    uint32_t dither_at;                 /* PV_FOGS x 256 PvDither: a (haze, colour id)'s two colours and its t */
    uint32_t fog_at;                    /* PvFog */
    uint32_t absent_at;                 /* PV_W x PV_W cells of no ground and 2 bytes (a window's start) */
    uint32_t size;                      /* the section's */
    uint32_t glow_at;                   /* PvGlow */
    uint32_t slabs_at;                  /* PvSlabs */
    uint32_t spare_[3];
} PvHead;                               /* 96 bytes */

#define PV_BLK 4                        /* the highest ground's blocks: cells a side */
#define PV_BW (PV_W / PV_BLK)           /* a window's blocks a side */
#define PV_NOGROUND (-32768)            /* a block's highest where it has no cells */
typedef struct {
    int32_t x0, y0;                     /* its rectangle's first cell (cell i's corner at i << shift plane units) */
    uint16_t w, h;
    uint32_t rows_at;                   /* h PvRow */
    int32_t bx0, by0;                   /* its blocks' rectangle: block (i, j) is cells 4i .. 4i + 3, 4j .. 4j + 3, */
    uint16_t bw, bh;                    /* its highest ground the highest of cells 4i - 1 .. 4i + 5 (int16, BK_HU; */
    uint32_t bmax_at;                   /* PV_NOGROUND: none), bw x bh of them */
} PvLevel;                              /* 32 bytes */

typedef struct {
    uint16_t x0, n;                     /* the row's cells: x0 .. x0 + n - 1 of the rectangle's, at `at` (n 0: none) */
    uint32_t at;
} PvRow;                                /* 8 bytes */

typedef struct {
    uint8_t a, b, t;                    /* the block's pixels whose threshold is under t are b, the rest a */
} PvDither;

/* the rock over the ground (an arch's), its own grid round each patch that has some: 32 km cells */
#define PV_SLABS 4
#define PV_SLAB_SHIFT 9                 /* a slab cell's side, 1 << this plane units at the finest (PvSlabs.shift: */
#define PV_SLAB_CELLS 3072              /* coarser where the mission's grids would have more cells than this; pview.c
                                           holds them all) */
typedef struct {
    int32_t x0, y0;                     /* its first cell (cell i's corner at i << PV_SLAB_SHIFT plane units) */
    uint16_t w, h;
    uint32_t at;                        /* w x h of its underside and top (int16 each, BK_HU; underside >= top: none) */
} PvSlab;                               /* 16 bytes */
typedef struct {
    int32_t under_div;                  /* the underside's glow by its height over the ground (BK_HU) times 16 over */
    uint8_t under[64];                  /* this: the light (terrain.c: 32 (0.24 + 0.3 e^-gap)) */
    uint8_t n, shift, pad_[2];          /* grids; a cell's side, 1 << shift plane units */
    PvSlab s[PV_SLABS];
} PvSlabs;

#define PV_GLOW_AS 64                   /* the sky's glow: by the eye's height, amax (j / 63)^2 over the datum, */
#define PV_GLOW_CZ 256                  /* and by a ray's cosine from straight up, -1 .. 1 (terrain.c tr_sky_table) */
typedef struct {
    int32_t amax;                       /* plane units */
    PvDither sky[PV_FOGS];              /* black at a haze level as two colours (black: the sky shows through) */
    uint8_t g[PV_GLOW_AS][PV_GLOW_CZ];  /* the glow, 8ths of a haze level */
} PvGlow;

typedef struct {
    int32_t h0, hstep;                  /* the table's heights, BK_HU */
    int32_t k;                          /* the haze's density and height scale as one number (pv_fog_frame) */
    int32_t g[PV_FOG_GS];               /* G(h) (terrain.c tr_fog_g), 2^24 */
    uint8_t lev[PV_FOG_TAUS];           /* the haze level by its sum (terrain.c fog_lev, rounded) */
} PvFog;
#ifdef __WATCOMC__
#pragma pack()
#else
#pragma pack(pop)
#endif
typedef char pv_head_is_96[sizeof(PvHead) == 96 ? 1 : -1];
typedef char pv_level_is_32[sizeof(PvLevel) == 32 ? 1 : -1];
typedef char pv_row_is_8[sizeof(PvRow) == 8 ? 1 : -1];

/* --- the frame's numbers (pview.c; the asm reads them by these layouts) --- */

#define PV_STEPS 400                    /* steps of the march a frame at most */
#define PV_LINES 640                    /* lines a frame at most, the four quarters' */

typedef struct {                        /* a step of the march: every line's samples at one angle from straight down */
    int32_t c, s;                       /* the angle's cosine and sine, 2^30 */
    int32_t a, b;                       /* the datum's point there from the eye, along up and along the line's way,
                                           plane units: R c - D, R s */
    int32_t uc;                         /* uE c, 2^30 (uE: up along the plane's middle) */
    int32_t dist;                       /* the datum's point's distance, plane units */
    int32_t ds;                         /* the step on from it, plane units */
    uint8_t lev;                        /* the least level its step is not too long for */
    uint8_t pad_[3];
} PvStep;                               /* 32 bytes */

typedef struct {                        /* a line: what is fixed for the frame, then where its march is */
    int32_t te;                         /* its way t along the plane's middle, 2^30 */
    int32_t kx, ky;                     /* its plane point at m (a step's s over its den, 65536ths) is P + K m */
    int32_t us, ts, uf, tf;             /* up and t along the screen line's way and along the camera's front, 2^14 */
    int32_t ra_, rb_;                   /* its pixel by a point's place (a, b): ra_ + rb_ (a us + b ts) / (a uf + b tf),
                                           256ths of a pixel from its first */
    int32_t col, dcol;                  /* its pixel k's place across, 65536ths of a picture pixel: col + k dcol */
    int16_t row, drow;                  /* its pixel k's along: row + k drow (vertical quarters: rows; horizontal: the
                                           roles swapped) */
    int16_t len, kmin;                  /* its pixels; the first wanted (the lines bunch up near V) */
    int16_t k0, k1;                     /* the pixels wanted: past kmin and within the view (the cockpit's opening) */
    int32_t hu, ht;                     /* the horizon's ray: its way along up and t (any length) */
    uint8_t vert, done, mat, hid;       /* a vertical quarter's line; its march over; the last sample's material; the
                                           last sample was hidden */
    int16_t step;                       /* where its march is: the next step */
    int32_t horizon;                    /* the filled part's edge, 256ths of a pixel */
    int32_t prho, plight, pfog, ph;     /* the last sample seen: its pixel (256ths), light and haze (256ths), height */
    int32_t pds;                        /* (its step's length, plane units, for the gentle-ground test) */
    int32_t plo, phi;                   /* the last sample's slab's pixels (256ths) */
    int32_t pX, pY;                     /* the last step's plane point */
    uint8_t pok, pslab, prect, pad3_;   /* there is a last sample; it had a slab; it was in a slab's grid */
} PvLine;

typedef struct {                        /* the frame's camera, planet-relative */
    int32_t eye[3];                     /* positions */
    int16_t m[9];                       /* right, up, front, 16384 = 1 */
    int16_t focal, cx, cy;              /* the screen's (640 x 400) */
    int16_t x0, y0, x1, y1;             /* the screen's part the ground can be seen in (the cockpit's opening) */
} PvCam;

#ifdef __cplusplus
extern "C" {
#endif

/* the view's file section at `at` of a handle's memory (the host: of a file in memory) taken: 0, or -1 when there is
 * none fit to draw from; pv_end: none */
int pv_begin(unsigned handle, uint32_t base, uint32_t at);
void pv_end(void);
int pv_on(void);

/* the frame's picture made (128 x 80 of colour id | haze << 8, PV_EMPTY where no ground): the eye and the camera */
void pv_frame(const PvCam *cam);
/* the host's: the picture made, and how much work it took */
const uint16_t PV_FAR *pv_picture(void);
uint32_t pv_samples(void);

#ifdef __cplusplus
}
#endif

#endif
