/* terrain.h: a planet's ground, made from a seed and drawn into an 8-bit frame buffer (terrain.c).  The user's design
 * (2026-11-14): "planetary missions ... a special interrupt to draw a terrain sphere ... only available if there is
 * hardware acceleration.  The accelerator needs to take a command to load a textured terrain and spherical height map.
 * It should be procedural so only the seed is needed.  The host should be able to do height queries and occlusion
 * queries to know whether to draw 2d sprites ... only full occlusion or zero occlusion ... two layers of terrain to
 * support caves and tunnels (arches) and probably an overlay over the procedural height map so missions can define
 * something.  The planets should be small so you can fly over them in about 5 minutes ... some kind of atmospheric
 * fog effect that depends on height to camera."
 *
 * Runs only on the host: the sprite accelerator (src/hardware/rvgx.cpp, E740h on) and the native builds call it; RV.EXE
 * has only the structs, to fill the card's block from.  Everything a game's logic may ask (tr_column: the ground and
 * the arch layer under a point) is integer arithmetic on the grid made from the seed, so every machine in a room and
 * the play tests get the same answer; the picture uses floats.
 *
 * The planet is a cube's six faces blown up to a sphere, each a TR_FACE x TR_FACE grid of cells: a height (the ground's
 * over the planet's radius, in eighths of a km), a colour, and in a few places a second layer: a slab of rock lying
 * over the ground with air under it, which is how an arch, a tunnel or a cave's roof is made.  Missions add their own
 * shapes over what the seed makes (TrStamp: a flat pad, an arch). */
#ifndef TERRAIN_H
#define TERRAIN_H

#include <stdint.h>

#define TR_FACE 1024                    /* cells along a face's side */
#define TR_STAMPS_MAX 16                /* a mission's own shapes at most */

enum {                                  /* TrStamp.kind */
    TR_STAMP_NONE = 0,
    TR_STAMP_PAD = 1,                   /* the ground made flat: a round pad (at, size[0] its radius, size[1] the slope
                                           round it), at its middle's height plus size[2] */
    TR_STAMP_ARCH = 2                   /* a stone arch over the ground: at its middle, `to` along its length, size[0]
                                           half its span, size[1] its width, size[2] the air under its crown, size[3]
                                           its thickness; its feet stand on the ground */
};

#ifdef __WATCOMC__
#pragma pack(1)                         /* (RV.EXE writes them straight into the card's block) */
#endif
typedef struct {
    uint8_t kind, flags;
    int16_t pad_;
    int32_t at[3];                      /* a point over the ground (planet-relative positions): its direction only */
    int32_t to[3];                      /* a second one: which way the shape lies */
    int32_t size[4];                    /* positions (256 a km), as the kind says */
} TrStamp;                              /* 36 bytes */

typedef struct {
    uint32_t seed;
    int32_t radius;                     /* the planet's, to its datum, in positions (256 a km) */
    uint8_t kind;                       /* TR_KIND_* */
    uint8_t nstamps;
    int16_t pad_;
} TrPlanetDesc;
#ifdef __WATCOMC__
#pragma pack()
#endif

enum { TR_KIND_DESERT = 0 };

typedef struct {                        /* what is under a point: radii from the planet's centre, in positions */
    int32_t ground;                     /* the ground's surface */
    int32_t slab_lo, slab_hi;           /* the second layer's underside and top; both 0 where there is none */
} TrColumn;

typedef struct TrPlanet TrPlanet;       /* (terrain.c's: the grids, the frame's picture) */

typedef struct {                        /* the eye, for drawing (floats: only the picture uses it) */
    double pos[3];                      /* planet-relative, in positions (256 a km) */
    float right[3], up[3], fwd[3];      /* in the planet's frame, unit length */
    float focal, cx, cy;                /* px per unit at distance 1; the view's middle on the screen */
    int w, h;                           /* the screen */
} TrCam;

#ifdef __cplusplus
extern "C" {
#endif

/* a planet made from its description and its stamps; pal: the game's 256 colours, 6 bits each (VGA's); 0 if no memory */
TrPlanet *tr_make(const TrPlanetDesc *d, const TrStamp *stamps, const uint8_t *pal);
void tr_free(TrPlanet *p);
int32_t tr_radius(const TrPlanet *p);
int32_t tr_top(const TrPlanet *p);      /* the highest the ground or a slab gets, as a radius */

/* the ground and the slab under (or over) a point, planet-relative in positions (the world's units, 256 a km):
 * integers only, the same answer on every machine */
void tr_column(const TrPlanet *p, int32_t x, int32_t y, int32_t z, TrColumn *out);

/* the frame's picture made for an eye (tr_view), then put into the frame buffer a band of rows at a time (tr_blit:
 * the rows y0 .. y1-1 and columns xa .. xb-1 of the screen, fb the first byte of screen row fb_y0; ext, when not 0,
 * each row's first column and one past its last from ext_y0 on, the cockpit's opening).  Where the ground is not, the
 * sky's pixels already there are hazed by the air the ray passes through.  Returns the pixels written. */
void tr_view(TrPlanet *p, const TrCam *cam);
uint32_t tr_blit(TrPlanet *p, uint8_t *fb, int fb_y0, int stride, int y0, int y1, int xa, int xb, const int16_t *ext,
                 int ext_y0, int ext_rows);

/* 1 when the ground hides nothing of the line from the eye to the point (planet-relative), 0 when it is behind the
 * ground: a sprite is drawn whole or not at all (the user: "only full occlusion or zero occlusion") */
int tr_seen(const TrPlanet *p, const double eye[3], const double pt[3]);

uint32_t tr_samples(const TrPlanet *p);  /* the last tr_view's samples of the ground (for timing) */
uint32_t tr_skips(const TrPlanet *p);    /* and its skips of hidden ground */

#ifdef __cplusplus
}
#endif

#endif
