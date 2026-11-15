/* planet.h: a planet in a flight (the user, 2026-11-14: "planetary missions ... a special interrupt to draw a terrain
 * sphere ... only available if there is hardware acceleration"; then, of DOS without the card: "Lets use the bake").
 * The ground a ship flies over is the bake's on every machine (bake.h: the mission's ID.PLN, read into XMS as the
 * flight starts): how high a point is over it, where a ground nav point goes.  Its picture is the card's (gx.h's planet
 * calls, terrain.c): made from a seed, drawn into each band before the ships, asked whether it hides a ship.  The
 * native game makes and draws that picture itself (planet.c). */
#ifndef PLANET_H
#define PLANET_H

#include "rvtypes.h"
#include "lightfield.h"
#include "terrain.h"
#include "rvdata.h"

int planet_can(void);                   /* there is a card that draws planets (1.3), or this is a native build */
extern int g_planet_off;                /* (the play tests: as if there were no card, for the campaign's planet missions) */

/* a planet for the coming flight, from a mission record's planet tail and nav points (terrain.h TrPlanetRec,
 * TrNavRec: km, the world's frame); made a piece at a time from now (planet_make) */
int planet_load(const TrPlanetRec __far *pr, const TrNavRec __far *navs, int nnav);
int planet_make(unsigned ms);           /* more of its picture made, for at most ms of the host's; 0: all of it, and the
                                           ground read in (a flight's start: the XMS is the mission's): 100 when made,
                                           -1 with no planet set or no ground to be had */
void planet_clear(void);                /* no planet */
int planet_on(void);                    /* a planet's ground is in: ships fly over it */
int planet_seen(void);                  /* its picture is made: it is drawn */
void planet_xms_freed(unsigned nblocks);   /* (dat.c's, the host's: the XMS blocks past the first nblocks were given
                                           back, the ground's among them at a mission's end) */
const LfVec __far *planet_centre(void);

/* the frame's eye (once a frame, before the bands), then each band's ground (after the sky, before the ships) */
void planet_frame(const LfVec *eye, const LfBasis *b, int cx, int cy, int focal);
void planet_band(int y0, int y1, const short __far *ext, int ext_y0, int ext_rows);

/* the ground hides a world point from the frame's eye: a ship there is not drawn (all or nothing) */
int planet_hides(const LfVec *pos);
int planet_hides_at(long x, long y, long z);   /* (the same, for a far record's point: a shot, a missile, a blast) */

/* how high a world point is over the ground, positions (negative: under it, or inside an arch's rock); *roof: how far
 * under a slab over it (an arch, a tunnel's roof), a large number when none */
long planet_over(const LfVec *pos, long *roof);


/* a mission's planet (missions.txt planet, ground): begun as the mission loads (next_mission), made and coloured at
 * the launch (planet_ready), its ground nav points put over the ground (planet_navs, after the tables are loaded) */
void planet_mission(const RvMission __far *m);
void planet_ready(void);
void planet_navs(void);
void planet_raise(LfVec *pos, long km);   /* (the autopilot's: a ship it put under the ground raised km over it) */

#endif
