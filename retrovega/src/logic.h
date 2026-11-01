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
int16_t logic_target_ahead(int16_t s);  /* slot s is in front of the pilot (ahead of the wing line, or on it) */

/* the player's weapon choice (flight.c's G and W keys, the weapons VDU): the guns fired by the trigger are all of
 * them (g_sys.gun_sel 0) or one type of them (k: the k-th type the ship carries, in the gun table's order); the
 * launcher the missile key fires is the n-th of the ship's (g_sys.launcher_sel), or the first with rounds */
int16_t logic_guns_next(void);          /* G: the next choice round; the new gun_sel */
int16_t logic_gun_types(uint16_t *out, int16_t max);   /* the types the ship's guns are, in order: n of them */
int16_t logic_gun_selected(int16_t g);  /* gun g of the player's fires with the trigger as the choice stands */
int16_t logic_missile_next(void);       /* W: the next launcher round; the new launcher_sel */
int16_t logic_launcher(void);           /* the launcher the missile key fires: a gun index, -1 none with rounds */

/* nav point n of the mission (0 home): 1 and how far the player is from it (km), 0 when there is no such point */
int16_t logic_nav_km(int16_t n, int32_t *km);

/* the wingmen's orders (the comm menu, flight.c and vdu.c; the user's asks, 2026-10-31): a wingman (a fighter of the
 * player's side flown by the AI) flies in formation and fights what comes within its reach (FREE, the default), or
 * keeps station and does not fight (FORMATION: fly in formation), or leaves the formation, goes for the enemy nearest
 * it however far and with none about circles the player (BREAK: break formation), or the player's target wherever it
 * goes (ATTACK: attack my target), or the enemy nearest the player, the one on the player first (HELP: help me out),
 * or the nearest capital ship of its side, circled (RTB: return to base) */
enum { ORDER_FREE, ORDER_ATTACK, ORDER_FORMATION, ORDER_HELP, ORDER_RTB, ORDER_BREAK, ORDER_COUNT };
int16_t logic_wingmen(int16_t *out, int16_t max);   /* the wingmen's slots, in order: n of them */
int16_t logic_is_wingman(int16_t s);    /* slot s is one of them */
int16_t logic_order(int16_t s, int16_t order);      /* 1 when slot s takes it (0: no wingman there, or ATTACK with
                                                       no enemy targeted, or RTB with no capital ship of its side) */
int16_t logic_order_of(int16_t s);
int16_t logic_taunt(int16_t s);         /* an enemy fighter turns on the player for a while: 1 when it does */

/* the home carrier (the mission's ship with role=home, holding station at nav 0) and landing (the user's rules,
 * 2026-10-31): asked on the comm with the carrier as the target, it clears the player with the mission finished or at
 * least one nav point flown, the player near enough; the player then lands by flying into its bow.  Its other sides,
 * or the bow without a clearance, just stop the fighter, unhurt.  Landing with the mission undone is a loss */
enum { LANDING_CLEARED, LANDING_NOT_YET, LANDING_TOO_FAR };
#define MISSION_CARRIER_LOST 6          /* missionStatus: the home carrier was destroyed: the flight ends with it, as the
                                           player's own death does (game.c's screen; the score is a loss) */
int16_t logic_is_home(int16_t s);       /* slot s is the home carrier, alive */
int16_t logic_landing_allowed(void);
int16_t logic_request_landing(void);    /* LANDING_*: a clearance stands for the rest of the flight */
int16_t logic_carrier_stopped(void);    /* the hull stopped the player since last asked: 1 cleared, 2 not, 0 no */
#ifdef RV_TESTS
void logic_test_to_carrier(int16_t where);   /* a script's way there: 0 off its bow, 1 off its beam */
void logic_test_kill_carrier(void);
#endif

#endif
