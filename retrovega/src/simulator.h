/* simulator.h: the bar's Schroedinger simulator (simulator.c; the flight's side is flight_simulator, flight.c) */
#ifndef SIMULATOR_H
#define SIMULATOR_H

#include "rvdata.h"

#define SIM_FIGHTER 0x00                /* the Schroedinger */
#define SIM_TYPES 6                     /* the Rlaan's fighter types, in the order they come */
#define SIM_WIN_KILLS 47                /* the win (the user's: "a solid 47 kills"): it comes in the round of four */
#define SIM_MAX_SHIPS RV_MAX_SHIP_SLOT  /* how many at once at the very most: every ship slot but the player's */
#define SIM_WINDSHIELD_Y 148            /* the Schroedinger's frame is cut flat here, at the top of the VDUs' housings */

const RvMission __far *simulator_mission(const RvMission __far *rules);   /* the flight's record: the fighter, the
                                           Rlaan's types for the loads, nobody spawned; the rules are the given mission's */
void simulator_start(void);             /* after logic_start: the first wave is on its way */
void simulator_frame(void);             /* a step, after the logic's: the kills, the next wave, what is replenished, the win */
void simulator_line(char *out);         /* what the view shows (64 bytes): how many of what, the kills */
int16_t simulator_count(void);          /* how many at once it has come to */
int16_t simulator_type(void);           /* of which type */
uint16_t simulator_kills(void);
int16_t simulator_won(void);            /* SIM_WIN_KILLS reached */
void simulator_test_kill(void);         /* a test's: the wave dead */
void simulator_test_first(int16_t count);   /* a test's: how many the first wave is */
void simulator_test_at(int16_t count, int16_t type_i);   /* a test's: the next wave's count and type */

#endif
