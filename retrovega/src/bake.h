/* bake.h: a planet's ground as a file (bake.c).  The user, 2026-11-14, of the planet on DOS without the sprite
 * accelerator: "Lets use the bake and do 128x80 size ... And lets download the planets on demand between mission or at
 * mission start if possible".
 *
 * terrain.c makes a planet from a seed with floats and some 40 MB of grids: the card's work, and the native builds'.
 * The bake is what a game's logic asks of the ground (how high a point is over it, an arch's rock over it), made once
 * at build time by tools/planetbake.c from the same terrain.c, a file a planet mission (ID.PLN beside RV.EXE):
 *   - the planet's own ground: terrain.c's grid two halvings down (BK_LEVEL: 384 cells along each of the cube's six
 *     faces, about 262 km a cell on Mirage), a height and a colour a cell, with a cell of border all round;
 *   - the ground as RV.EXE draws it without the card (pview.h: a pyramid of grids over the plane that touches the
 *     planet at the mission's middle, and the tables of its colours and haze);
 *   - the mission's places whole: terrain.c's detail patches (a pad, an arch, each stretch of a canyon), every point's
 *     height and colour, and the slab of rock over it where there is one (an arch).
 * Every machine reads the same file and does the same integer sums on it (bk_column), DOS or native, card or none: the
 * ground a ship flies over and crashes into is the bake's everywhere.  In a patch the answers are terrain.c's own to
 * the position; outside one they are the coarser grid's (planetbake prints how far from terrain.c's: on Mirage the
 * half of them within 6 km, 99 in 100 within 93 km).
 *
 * The file lies in XMS (planet.c reads it in); its bytes are read from there.  On DOS the sums are bake.asm's
 * (rv_bk_column: a 64-bit product is one instruction of a 386's, and a call and a page of code in Watcom's 16-bit C);
 * everywhere else, and in the baker, they are bake.c's (c_bk_column), and RVP.EXE /twins holds the two to the same
 * answers to the bit. */
#ifndef BAKE_H
#define BAKE_H

#include "rvtypes.h"

#ifdef __WATCOMC__
#define BK_FAR __far
#else
#define BK_FAR
#endif

#define BK_VERSION 2
#define BK_LEVEL 2                      /* the grid: terrain.c's, this many halvings down */
#define BK_PATCHES 12                   /* (terrain.c's TR_PATCHES) */
#define BK_HU 64                        /* a height unit, positions (terrain.c's TR_HU) */
#define BK_CELL 3                       /* a cell's (a patch point's) bytes: its height (int16, height units over the
                                           datum), its colour id */
#define BK_SLAB 4                       /* a patch point's slab: its underside and top (int16 each; lo >= hi: none) */

#ifdef __WATCOMC__
#pragma pack(1)
#else
#pragma pack(push, 1)
#endif
typedef struct {
    char magic[4];                      /* "RVPL" */
    uint16_t version;
    uint16_t n;                         /* the grid's cells along a face's side (its rows n + 2 long, the border's) */
    uint32_t key;                       /* bk_key of the mission's records it was made from */
    uint32_t stamp;                     /* (the baker's: its sources' sum, to know a stale file by) */
    int32_t radius;                     /* the datum's, positions */
    int16_t top, bottom;                /* the highest (a slab's top too) and lowest ground, height units */
    uint8_t kind, npatch, view, pad_;    /* view: 1, the file has the picture's ground (view_at) */
    uint32_t grid_at;                   /* six faces, each (n + 2) x (n + 2) cells */
    uint32_t view_at;                   /* the picture's ground (pview.h PvHead), view_size bytes */
    uint32_t view_size;
    uint32_t spare_[2];
    uint32_t patch_at;                  /* npatch BkPatch */
    uint32_t size;                      /* the file's */
} BkHead;                               /* 56 bytes */

typedef struct {
    int32_t c[3], a[3], b[3];           /* its middle and its two ways, unit, 2^30 = 1 */
    int32_t half_a, half_b, cell;       /* half its length along a and along b, a cell's side: positions */
    uint16_t na, nb;                    /* its cells along a and b (its points one more each way) */
    int16_t top, over;                  /* the highest of it; the most it stands over the grid there: height units */
    int16_t cosmin;                     /* a direction (16384 long) whose part along c (16384 = 1) is under this
                                           times its length is not in it (bk_column's quick test) */
    uint16_t slabs;                     /* 1: it has slabs (slab_at) */
    uint32_t at;                        /* its points' cells, (na + 1) a row, nb + 1 rows */
    uint32_t slab_at;                   /* their slabs, the same order */
} BkPatch;                              /* 68 bytes */
typedef struct {                        /* a patch for bk_column's quick test: its middle's way in 16 bits (16384 = 1), */
    int16_t c[3];                       /* and BkPatch's cosmin */
    int16_t cosmin;
} BkQuick;

typedef struct {                        /* the planet being asked: what every question needs of the file's head, kept in */
    uint16_t handle;                    /* conventional memory (bk_begin fills it; bake.asm reads it by these offsets). */
    uint16_t npatch;                    /* handle, base: where the file lies, an XMS handle and a place in its memory */
    uint32_t base;
    int32_t radius;
    uint32_t grid_at;
    uint32_t patch_at;
    uint16_t n;                         /* (0: no planet) */
    uint16_t pad_;
    BkQuick q[BK_PATCHES];
} BkAsk;                                /* 120 bytes */
#ifdef __WATCOMC__
#pragma pack()
#else
#pragma pack(pop)
#endif
typedef char bk_head_is_56[sizeof(BkHead) == 56 ? 1 : -1];
typedef char bk_patch_is_68[sizeof(BkPatch) == 68 ? 1 : -1];
typedef char bk_ask_is_120[sizeof(BkAsk) == 120 ? 1 : -1];

typedef struct {                        /* what is under a point: radii from the planet's centre, positions */
    int32_t ground;                     /* the ground's surface */
    int32_t slab_lo, slab_hi;           /* a slab's underside and top over it; both 0 where there is none */
    int32_t r;                          /* the point's own radius */
} BkColumn;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __WATCOMC__
#define BK_ASM __far __cdecl
#else
#define BK_ASM
#endif

extern BkAsk BK_FAR bk_ask;

/* the records a planet is made from, as a number: a file made from others is not this mission's */
uint32_t bk_key(const void BK_FAR *planet_rec, const void BK_FAR *navs, int nnav);

/* the file at `base` of an XMS handle's memory taken as the planet (bk_ask filled from its head and patches): 0, or -1
 * when it is no bake of this version; bk_end: none */
int bk_begin(unsigned handle, uint32_t base);
#define bk_end() (bk_ask.n = 0)

/* the ground and the slab under (or over) a planet-relative point, positions: integers, every machine's the same.
 * rv_ is bake.asm's (the host's: host/asm_twins.c), c_ its twin, bake.c's */
void BK_ASM rv_bk_column(int32_t x, int32_t y, int32_t z, BkColumn *out);
void c_bk_column(int32_t x, int32_t y, int32_t z, BkColumn *out);
/* a * b / c, the product whole (the quotient must fit) */
int32_t BK_ASM rv_bk_muldiv(int32_t a, int32_t b, int32_t c);
int32_t c_bk_muldiv(int32_t a, int32_t b, int32_t c);
/* the line from the eye to p (planet-relative positions) passes under the ground: its point nearest the centre under
 * the lowest ground (the radius and bottom height units), or (ground) the ground at 8 of its points under the highest
 * (top) (what RV.EXE draws without the card: pview.c pv_hides) */
int BK_ASM rv_bk_hides(const int32_t BK_FAR *eye, const int32_t BK_FAR *p, int bottom, int top, int ground);
int c_bk_hides(const int32_t BK_FAR *eye, const int32_t BK_FAR *p, int bottom, int top, int ground);
#ifdef __WATCOMC__
#define bk_column rv_bk_column
#define bk_muldiv rv_bk_muldiv
#define bk_hides rv_bk_hides
#else
#define bk_column c_bk_column
#define bk_muldiv c_bk_muldiv
#define bk_hides c_bk_hides
#endif

/* a planet-relative point moved along its line from the centre to `over` positions over the ground under it (c: its
 * column, asked before) */
void bk_place(int32_t v[3], const BkColumn *c, int32_t over);

#ifdef __cplusplus
}
#endif

#endif
