/* collide.h: collisions with ships (collide.c).
 *
 * A ship's hull is its three silhouettes (the user's pick, 2026-10-21): from
 * above, the side and the front, 64 x 64 bit masks over its hit box (the
 * mesh's extents, axis-aligned in its own frame; tools/hitboxes.py's
 * SHAPES.DAT); a point is in the hull when it is in the box and solid in all
 * three (a type without silhouettes: its box).  Ship to ship: coarse (the
 * two bounding spheres), then fine (the user's): the smaller ship's hull
 * against the bigger one's vitals (its core sphere: the biggest about its
 * middle inside its sprite at every view, to 95%; its engines, weapon
 * mounts, nose, tail and wingtips), each taken into the hull's frame by one
 * 3 x 3 product; only when that finds nothing, the bigger one's hull against
 * the smaller's vitals (a fighter in a capital ship, between its vitals).  A
 * point (a blaster shot), a segment (a bolt from where it was to where it
 * is) against one ship's hull.  A hit gives the least move that frees the
 * deepest point: the vector that rectifies the intersection and shoves the
 * ships apart. */
#ifndef COLLIDE_H
#define COLLIDE_H

#include "lightfield.h"

int16_t collide_reserve(void);          /* when the game starts (resident XMS): SHAPES.DAT in */
void collide_flight_end(void);          /* the silhouettes kept for the flight's types given back */

/* a slot's ship: its half box in positions (256 a km): its type's radius_km (data/ships.csv: the old game's sizes, a
 * light fighter 100 km, the carrier 700; the flight draws the sprite full size where its width is that), 0 when there
 * is no ship there */
int32_t collide_half(int16_t slot);

int16_t collide_coarse(int16_t a, int16_t b);   /* their bounding spheres touch */

/* coarse, then fine: 0 when no vital is in the other's box; else *push (positions) is the least move of b that
 * frees the deepest (move a by -push instead, or each by half), *depth how deep it was */
int16_t collide_ships(int16_t a, int16_t b, LfVec *push, int32_t *depth);

/* a world point in the ship in a slot: 1 when it is, and *push (if asked) the least move of the point out */
int16_t collide_point(int16_t slot, const LfVec *p, LfVec *push);

/* a segment (a bolt from where it was, a, to where it is, b) against the ship in a slot: 1 when it goes in, *hit
 * (if asked) the first point of it in the hull and *t1024 (if asked) how far along it that is, 1024ths of a to b.
 * Its ends within 32,768 km of the ship */
int16_t collide_segment(int16_t slot, const LfVec *a, const LfVec *b, LfVec *hit, int16_t *t1024);
/* the same for a bolt of radius `fat` (positions): the hull's box and slabs grown by it, and a cell counts when
 * the bolt's middle or any of six points its radius off along the ship's axes is solid (the user's rule of
 * 2026-10-31: the bolt's radius and its whole step count in every hit test) */
int16_t collide_segment_fat(int16_t slot, const LfVec *a, const LfVec *b, int32_t fat, LfVec *hit, int16_t *t1024);
/* the bolt's step against the type's volume box (ships.csv: the silhouettes' width and length and the height that
 * gives it the bounding sphere's volume: tools/rvdata.py volume_box), in the ship's frame: the segment's middle
 * against the box grown by the bolt's radius and by the half step along each axis, three compares.  1 and *hit
 * (if asked) when it is in */
int16_t collide_box(int16_t slot, const LfVec *a, const LfVec *b, int32_t fat, LfVec *hit);

/* a ship's hit box and vitals in the world, for drawing them: its box's eight corners, its vitals (the core
 * first, then the engines, then the rest): how many vitals (at most 1 + 4 + 8) */
int16_t collide_shape(int16_t slot, LfVec corner[8], LfVec *vital, int16_t maxv);

/* a type's three silhouettes for drawing them (the VDUs' top-down wireframe): 3 x 512 bytes, each a 64 x 64 bit
 * mask, rows of 8 bytes, a cell's bit 1 << (col & 7) of byte col >> 3: mask 0 from above (row: the cell along
 * the ship's front, 0 the tail, 63 the nose; column: along its right), 1 from the side (row along its up, column
 * along its front), 2 from the front (row along its up, column along its right); 0 when the type has none */
const uint8_t __far *collide_mask(uint16_t type);

#endif
