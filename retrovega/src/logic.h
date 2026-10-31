/* logic.h: the game's logic in flight (logic.c): the step the flight loop runs once a frame, and the bodies of the
 * thunks the layer hooks (spec 5.1) that are play rather than drawing.  docs/logic.md is the guide. */
#ifndef LOGIC_H
#define LOGIC_H

#include "rvdata.h"

#define LOGIC_HZ 30                     /* steps a second: the flight draws a frame a step and waits out the rest of
                                           1 / LOGIC_HZ s (with the layer, its frame pacing sets the rate instead) */

/* the pilot's controls this step (flight.c reads them before the step) */
typedef struct {
    int8_t steer_x;                     /* -9 .. 9: + turns the nose right */
    int8_t steer_y;                     /* -9 .. 9: + pulls the nose up (the down arrow, a stick pulled back) */
    int8_t roll;                        /* -9 .. 9: + lifts the right wing (E), - the left (Q) */
    uint8_t guns;                       /* the trigger held: Space, the left button in the view, a script's hold fire */
    uint8_t missile;                    /* Enter pressed this step */
    uint8_t afterburner;                /* Tab held while there is fuel (flight.c's systems: g_sys.burning) */
} PilotInput;
extern PilotInput __far g_pilot;

/* a ship is being spawned into a slot (ship_spawn calls it, whoever spawns): forget what is kept for the slot (its
 * turn's fractions).  Any slot number may come; only ship slots keep anything */
void logic_prepare_spawn(int16_t ship_index);
void logic_start(const RvMission __far *m);   /* the flight begins: its ships into the slots (before their art loads) */
void logic_frame(void);                 /* one step (frameCounter is already one more) */
void logic_hud(void);                   /* a band of the view is drawn: draw over it with hud.h's calls */
void logic_end(void);                   /* the flight is over (missionStatus says how) */
int16_t logic_autopilot(void);          /* the A key, between steps: 1 when it flew the player on (to the next nav
                                           point), 0 when it may not (enemies near, nowhere to go) */
int16_t logic_autopilot_ready(void);    /* the nav point it would go to, -1 not now (the engine asks before its flyby) */

/* the player's systems (systemDamage's bytes, one each: 0 whole .. SYS_WRECKED; the old game's nine, read in its
 * damage readout): a hit through the armour to the hull may hurt one (do_damage).  What each does when hurt:
 * ABSORBERS the slide (M1); POWER the gun energy back slower, none wrecked (and the running lights out, flight.c);
 * SHIELDS the shields back slower, none wrecked; TRACK no target lock and no missile lock wrecked; INTERCOM no comm
 * menu wrecked; EJECTOR no ejection wrecked; GUNS a gun destroyed a point; COMPUTER and REPAIR the readout only */
enum { SYS_ABSORBERS, SYS_POWER, SYS_SHIELDS, SYS_COMPUTER, SYS_INTERCOM, SYS_TRACK, SYS_EJECTOR, SYS_REPAIR, SYS_GUNS,
       SYS_COUNT };
#define SYS_WRECKED 4                   /* systemDamage's most */
const char __far *logic_system_name(int16_t k);   /* its name for the readout (D) */

/* the player's targeting (flight.c's T, Y and L keys, its AUTO; the user's ruling, 2026-10-31): the target key
 * selects only among the ships in front of the pilot (ahead of the wing line), the one nearest the nose first and
 * each press the next round; a lock (L, g_sys.hold) keeps the chosen target through T, Y and AUTO until it is
 * unlocked or the target dies; no lock with the target-tracking system wrecked.  Each returns the target, -1 none */
int16_t logic_target_next(void);        /* T */
int16_t logic_target_nearest(void);     /* Y, AUTO: the nearest enemy in front */
int16_t logic_target_lock(void);        /* L: the lock on, or off again; 1 when it is on after this */
int16_t logic_target_ahead(int16_t s);  /* slot s is in front of the pilot (ahead of the wing line) */

#endif
