/* damage.h: what a hit through to the player's hull breaks, and what that does (damage.c; docs/damage.md).
 *
 * The player's ship has a set of systems, each 0 whole, 1 light, 2 moderate, 3 heavy or 4 wrecked.  Where a hit lands
 * decides which is likely (the guns and sensors are forward, the engine and tank aft, a side's own VDU and guns on that
 * side).  Past the hull there is a reserve of health, so a pilot limps on for a while with everything failing rather
 * than dying at the hull's last point.  Only the player's own ship has any of this. */
#ifndef DAMAGE_H
#define DAMAGE_H

#include "rvtypes.h"

/* the systems: the old game's nine first, in its order, then the others.  The first SYS_FIXED live in systemDamage (the
 * fixed block has room for ten, and the layer reads them); the rest in damage.c */
enum { SYS_ABSORBERS, SYS_POWER, SYS_SHIELDS, SYS_COMPUTER, SYS_INTERCOM, SYS_TRACK, SYS_EJECTOR, SYS_REPAIR, SYS_GUNS,
       SYS_ENGINE, SYS_FUEL, SYS_RADAR, SYS_VDU_L, SYS_VDU_R, SYS_NAV, SYS_THRUSTERS, SYS_RACKS, SYS_IFF, SYS_DECOYS,
       SYS_COUNT };
#define SYS_FIXED 10
#define SYS_WRECKED 4                   /* a system's most */

int16_t sys_level(int16_t k);
void sys_set(int16_t k, int16_t level);
const char __far *logic_system_name(int16_t k);   /* its name for the readout (D) */

/* the player's ship is new: every system whole, the reserve full (logic_start; a spawn into slot 0) */
void damage_reset(void);

/* the reserve: what a hit past the hull's last point draws on.  `over` of it spent: 1 when the reserve holds, 0 when the
 * hit is the end of the ship (hull: the type's) */
int16_t damage_reserve_take(uint16_t over, uint16_t hull);
int16_t damage_reserve_pct(uint16_t hull);    /* what is left of it, in a hundred */
int16_t damage_in_reserve(void);        /* the hull is gone and it is what is left */

/* a hit of `left` through to the hull (of `hull` in all), on armour side `side` (0 front, 1 rear, 2 left, 3 right): which
 * systems it breaks, from game_rand */
void damage_hurt(uint16_t left, uint16_t hull, int16_t side);

/* once a step: the repair system's work, the tank's leak, the overlays the hull's state shows */
void damage_step(void);

/* what the player's ship can do now, each by its system's level (the caller knows it is the player's): */
uint16_t damage_kps(uint16_t kps);      /* a speed cut to what the engine gives */
uint16_t damage_accel(uint16_t accel);  /* the same for its acceleration */
int16_t damage_burner_ok(void);         /* the afterburner lights */
int16_t damage_turn_rate(int16_t dps);  /* a turn rate cut to what the thrusters give */
int16_t damage_pull(void);              /* the yaw the thrusters pull the nose by, heavy or worse: -1, 0 or 1 */
int32_t damage_slide(int32_t slide_dmg);   /* the speed the nose slides above, the absorbers hurt (the type's figure for
                                           them, less at the worse levels) */
uint16_t damage_shield_cap(uint16_t most);   /* the most a shield holds, the generator hurt */
uint16_t damage_energy_cap(void);       /* the most gun energy the power plant keeps up */
int16_t damage_lock_factor(void);       /* a missile's lock takes this many times as long; 0 it never comes */

/* what has just broken, for the effects (the flight's: sparks, a klaxon): 1 and the next one's system and level, 0 none
 * waiting.  Only ever read: a machine that does not ask loses nothing */
int16_t damage_event(int16_t *sys, int16_t *level);
int16_t damage_overlay(int16_t k);      /* the cockpit overlay that shows system k hurt: where its sparks fly from; -1 none */

#ifdef RV_TESTS
void damage_test_worse(void);           /* every system a point worse (RVT.EXE's ( key) */
void damage_test_breach(void);          /* the hull gone and half the reserve spent (its ) key) */
void damage_test_all(void);             /* every system a little hurt (its ^ key: all the shapes) */
void damage_test_some(void);            /* a few systems hurt, the VDUs left whole (its _ key: a look at the damage page) */
#endif

#endif
