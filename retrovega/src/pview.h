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
    uint32_t noblk_at;                  /* PV_BW x PV_BW blocks' highest of PV_NOGROUND (a window's blocks' start) */
    uint32_t spare_[2];
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
#define PV_SLAB_CELLS 2048              /* coarser where the mission's grids would have more cells than this; pview.c
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

/* --- the frame's numbers (pview.c sets them, pview.asm and the C twins read them, by these layouts) --- */

#define PV_STEPS 128                    /* steps of the march a frame at most (85 the most seen: eyes 10 km to
                                           30,000 km up, the longer steps grazing the ground) */
#define PV_LINES 288                    /* a quarter's lines at most (the four are marched one after another): 272 the
                                           most for any V on and round the picture; pv_lines keeps to the cap */
#define PV_SLAB_MEM 2048                /* slab cells pview holds (the bake's PV_SLAB_CELLS) */

typedef struct {                        /* a step of the march: every line's samples at one angle from straight down */
    int32_t c, s;                       /* 0: the angle's cosine and sine, 2^30 */
    int32_t a, b;                       /* 8: the datum's point there from the eye, along up and along the line's way:
                                           R c - D, R s, 2^vsh plane units */
    int32_t uc;                         /* 16: uE c, 2^30 (uE: up along the plane's middle) */
    int32_t dist;                       /* 20: the datum's point's distance, plane units */
    int16_t ds;                         /* 24: the step on from it, plane units (32767 at most) */
    uint8_t lev, pad_;                  /* 26: the least level its step is not too long for */
} PvStep;                               /* 28 bytes */

#define PV_L_VERT 1                     /* PvLine.flags: a vertical quarter's line (its pixels a row each) */
#define PV_L_DONE 2                     /* its march is over */
#define PV_L_HID 4                      /* its last sample was hidden */
#define PV_L_POK 8                      /* there is a last sample seen (prho, plight, pfog, ph, pds, mat) */
#define PV_L_PSLAB 16                   /* the last sample had a slab (plo, phi) */
#define PV_L_PRECT 32                   /* the last step was in a slab's grid */
typedef struct {                        /* a line: what is fixed for the frame, then where its march is */
    int32_t te, kx, ky;                 /* 0: its way t along the plane's middle (2^30); its plane point at a step is
                                           P + K m, m the step's s over its den (65536ths) */
    int32_t hu, ht;                     /* 12: the horizon's ray, along up and t (any length) */
    int32_t col, dcol;                  /* 20: its pixel k's place across, 65536ths of a pixel: col + k dcol */
    int16_t ra_, rb_;                   /* 28: a point (a, b)'s pixel: ra_ + rb_ (a us + b ts) / (a uf + b tf),
                                           256ths of a pixel from its first */
    int16_t us, ts, uf, tf;             /* 32: up and t along the screen line's way and the camera's front, 2^10 */
    int16_t horizon, prho;              /* 40: the filled part's edge; the last sample's pixel (256ths) */
    int16_t plo, phi;                   /* 44: the last sample's slab's pixels */
    int16_t plight, pfog, ph, pds;      /* 48: its light and haze (256ths), height (BK_HU), step (plane units) */
    int16_t step;                       /* 56: the next step of its march */
    uint8_t row, k0, k1;                /* 58: its pixel k's along: row + k drow; the pixels wanted k0 .. k1 */
    int8_t drow;                        /* 61 */
    uint8_t flags, mat;                 /* 62: PV_L_*; the last sample's material */
} PvLine;                               /* 64 bytes */

typedef struct {                        /* a quarter of the picture round V */
    int16_t on, vert, s, inner, edge, len, first, lines;   /* (inner: the row (column) nearest V) */
    int32_t va, vc;                     /* V along and across its lines, 256ths of a pixel */
} PvQuarter;

/* the frame: set by pv_frame; its arrays where the frame says (DOS: the band buffer and the 32 KB scratch buffer;
 * the twins test: its own) */
typedef struct {
    int32_t R, D, rt, rb;               /* 0: plane units: the datum's radius, the eye's, the highest and lowest ground's */
    int32_t up[3];                      /* 16: the eye's way, 2^30 */
    int32_t ue, ua, ub;                 /* 28: up along the plane's middle and ways */
    int32_t px, py;                     /* 40: the eye's point on the plane */
    int32_t lcx, lcy, lf;               /* 48: the picture's middle and focal length, 256ths of a pixel */
    int32_t vx, vy;                     /* 60: V, 256ths of a pixel */
    int32_t cr[3], cu[3], cf[3];        /* 68: the camera's right, up, front, 2^14 */
    int32_t e[3], a[3], b[3];           /* 104: the plane's middle and ways, 2^30 */
    int32_t fog_h0, fog_rcp;            /* 140: the haze's heights' first (BK_HU), and 65536 over their step */
    int32_t alt;                        /* 148: the eye over the datum, BK_HU */
    int32_t under_div;                  /* 152: (PvSlabs) */
    int32_t skf, skr, sku;              /* 156: up along the camera's front, right, up, 65536ths (the glow) */
    int16_t nadir, vsh;                 /* 168: V is straight down; the vertical numbers' scale */
    int16_t nsteps, nlines;             /* 172 */
    int16_t bx0, by0, bx1, by1;         /* 176: the picture's part wanted, pixels */
    int16_t shift0, nlev, far_c, nslab; /* 184 */
    int16_t slab_shift, glow_any;       /* 192 */
    int16_t lev, sh;                    /* 196: the level marched and its cells' shift */
    int32_t ox, oy;                     /* 200: its window's corner, cells */
    int16_t max_steps, max_lines;       /* 208: the arrays' room (PV_STEPS, PV_LINES; the twins test's fewer) */
    PvSlab slab[PV_SLABS];              /* 212: the slab grids (at: their first cell in the frame's slab cells) */
    uint32_t samples, seen, skips, subs;   /* 276: the work done */
    uint16_t o_pic, o_st, o_ln, o_slab;    /* 292: the arrays' places in buf: the picture, steps, lines, slab cells */
    uint16_t o_win, o_bwin, o_foglev, o_fogm, o_glow, o_under;  /* 300: and in scr: the window, its blocks'
                                           highest, the haze's levels and rates, the glow, the slabs' undersides */
    uint8_t PV_FAR *buf;                /* 312 (DOS: a far pointer, the asm's segment) */
    uint8_t PV_FAR *scr;                /* 316 */
    int16_t step_px, wide;              /* 320: the detail (pv_detail): the steps about this many pixels apart; wide:
                                           every other line marched, each filling its neighbour's pixels too */
    int32_t fog_k, fog_hs;              /* 324: the haze's density and height scale as one number, and the heights'
                                           step of its table (pv_fogt) */
} PvFrame;
#ifdef __WATCOMC__
typedef char pv_frame_is_332[sizeof(PvFrame) == 332 ? 1 : -1];
#endif
typedef char pv_step_is_28[sizeof(PvStep) == 28 ? 1 : -1];
typedef char pv_line_is_64[sizeof(PvLine) == 64 ? 1 : -1];

typedef struct {                        /* a band's blit (pv_blit): the picture's pixels as 5 x 5 blocks */
    uint8_t PV_FAR *band;               /* 0: screen row y0's first byte (the rows 640 bytes apart) */
    const int16_t PV_FAR *ext;          /* 4: the opening's (first, end) a row from ext_y0, or 0 */
    const uint16_t PV_FAR *pic;         /* 8: the picture's rows from row ly0 */
    const uint8_t PV_FAR *dither;       /* 12: PV_FOGS x 256 PvDither, then the sky's PV_FOGS */
    uint8_t PV_FAR *work;               /* 16: PV_BLIT_WORK bytes: a picture row's two patterns, its runs */
    int16_t y0, y1, ext_y0, ext_rows, ly0, pad_;   /* 20 */
} PvBlit;                               /* 32 bytes (DOS) */
#define PV_BLIT_WORK (2 * PV_LW * PV_BLOCK + 4 * 64)   /* (64 runs a row at most: 128 pixels' changes) */

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

/* the frame's picture made (128 x 80 of colour id | haze << 8, PV_GLOW | level, PV_EMPTY where no ground) */
void pv_frame(const PvCam *cam);
const uint16_t PV_FAR *pv_picture(void);   /* (the host's: the picture made) */
const PvFrame PV_FAR *pv_frame_now(void);
void pv_blit_host(uint8_t *fb, int stride, int y0, int y1);

/* the picture's detail, 0 the finest: its steps about 2, 3, 4 pixels apart, then every other line marched (wide) with
 * steps 4, 6, 8 apart; from the next frame on.  pv_pace: a flight frame's work (clock240 ticks, the wait for the frame
 * rate left out), weighed over the last frames: the detail coarser while frames run past 1 / 30 s, finer again when
 * they have room to spare */
#define PV_DETAILS 6
void pv_detail(int level);              /* (the host's tools) */
int pv_detail_now(void);
void pv_pace(unsigned ticks);
void pv_pace_report(void);              /* (the details a flight's frames had: logged at its end) */

/* the parts that run a line, a step or a pixel at a time: the C twins (c_: the host's), and on DOS pview.asm's run
 * from PVIEW.BIN (rv_: pv_bind); RVP.EXE /twins logs the asm's sums, the host's must be the same */
int c_pv_setup(PvFrame PV_FAR *f, const PvCam PV_FAR *cam, const PvHead PV_FAR *h);
void c_pv_quarter(const PvFrame PV_FAR *f, int which, PvQuarter PV_FAR *q, uint8_t PV_FAR *thin);
void c_pv_steps(PvFrame PV_FAR *f);
void c_pv_lines(PvFrame PV_FAR *f, const PvQuarter PV_FAR *q, const uint8_t PV_FAR *thin);
void c_pv_march(PvFrame PV_FAR *f);
void c_pv_far(PvFrame PV_FAR *f);
void c_pv_glow(PvFrame PV_FAR *f);
void c_pv_fogt(PvFrame PV_FAR *f);
void c_pv_blit(const PvBlit PV_FAR *b);
int rv_pv_setup(PvFrame PV_FAR *f, const PvCam PV_FAR *cam, const PvHead PV_FAR *h);
void rv_pv_quarter(const PvFrame PV_FAR *f, int which, PvQuarter PV_FAR *q, uint8_t PV_FAR *thin);
void rv_pv_steps(PvFrame PV_FAR *f);
void rv_pv_lines(PvFrame PV_FAR *f, const PvQuarter PV_FAR *q, const uint8_t PV_FAR *thin);
void rv_pv_march(PvFrame PV_FAR *f);
void rv_pv_far(PvFrame PV_FAR *f);
void rv_pv_glow(PvFrame PV_FAR *f);
void rv_pv_fogt(PvFrame PV_FAR *f);     /* (the haze's rates by height for the frame: its G table at o_win, fogm out) */
void rv_pv_blit(const PvBlit PV_FAR *b);
#ifdef __WATCOMC__
int pv_bind(const uint8_t PV_FAR *code);   /* (PVIEW.BIN's code, wherever it lies: rv_ call into it) */
void pv_bind_blit(const uint8_t PV_FAR *blit);   /* (and its blit's part, copied apart) */
unsigned pv_code_read(uint8_t PV_FAR *dst, unsigned room);
void pv_xms_freed(unsigned nblocks);
int pv_hides(const long PV_FAR *eye, const long *p, int ground);   /* (no card: the line from the eye under the ground) */
#define pv_setup rv_pv_setup
#define pv_quarter rv_pv_quarter
#define pv_steps rv_pv_steps
#define pv_lines rv_pv_lines
#define pv_march rv_pv_march
#define pv_far rv_pv_far
#define pv_glow rv_pv_glow
#define pv_fogt rv_pv_fogt
#define pv_blit rv_pv_blit
#else
#define pv_setup c_pv_setup
#define pv_quarter c_pv_quarter
#define pv_steps c_pv_steps
#define pv_lines c_pv_lines
#define pv_march c_pv_march
#define pv_far c_pv_far
#define pv_glow c_pv_glow
#define pv_fogt c_pv_fogt
#define pv_blit c_pv_blit
#endif
/* a band of the screen (rows y0 .. y1-1, in the band buffer) given the frame's picture of the ground, within the
 * cockpit's opening (ext: its rows from ext_y0) */
void pv_band(int y0, int y1, const short PV_FAR *ext, int ext_y0, int ext_rows);

#ifdef __cplusplus
}
#endif

#endif
