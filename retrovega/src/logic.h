/* logic.h: the game's logic in flight (logic.c): the step the flight loop runs once a frame, and the bodies of the
 * thunks the layer hooks (spec 5.1) that are play rather than drawing.  docs/logic.md is the guide. */
#ifndef LOGIC_H
#define LOGIC_H

#include "rvdata.h"
#include "rvhooks.h"                    /* (RvVec3) */
#include "damage.h"                     /* (the player's systems: SYS_*, what each does when hurt) */

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
void logic_hud(void);
#ifdef RV_HOST
void logic_damage_snap(int16_t save);   /* (the host's src check: do_damage's state outside the fixed block) */
#endif
extern uint8_t __far logic_pilot_kills[RV_PILOTS];   /* and by each named pilot the AI flew, by pilot index (the board's
                                           tallies: camp_resolve) */                   /* a band of the view is drawn: draw over it with hud.h's calls */
void logic_end(void);                   /* the flight is over (missionStatus says how) */
int16_t logic_autopilot(void);          /* the A key, between steps: 1 when it flew the player on (to the next nav
                                           point), 0 when it may not (enemies near, nowhere to go) */
int16_t logic_autopilot_ready(void);    /* the nav point it would go to, -1 not now (the engine asks before its flyby) */
void logic_form_up(int16_t steps);      /* the autopilot's flyby begins (flight.c): the player faces the nav point, the wing
                                           and the escort targets close up on it and fly with it for `steps` steps */

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

int32_t logic_target_km(int16_t s);     /* how far slot s is from the player, hull to hull (the middles' distance less
                                           the sum of the half boxes: the VDUs' range, a lock's, the landing's) */

/* nav point n of the mission (0 home): 1 and how far the player is from it (km), 0 when there is no such point */
int16_t logic_nav_km(int16_t n, int32_t *km);   /* a nav point on the player's lists: how far; 0 not one */
void logic_nav_goal(int16_t n, int32_t *x, int32_t *y, int32_t *z);   /* where nav point n is to be reached, km: its place,
                                           or for a point in a field the field's middle (the host's test pilot) */
int16_t logic_nav_seen(int16_t n);      /* a waypoint the player has reached already (the lists mark it: *NAV 1) */
extern uint8_t __far logic_scene_off;   /* 1: nobody leaves the scene (a test's) */
void logic_nav_next(void);              /* N: the next on the list, chosen */
int16_t logic_nav_mark(int16_t *x, int16_t *y);   /* the chosen one on the view: 1 in view, 2 pinned at the rim, 0 no mark */

/* the wingmen's orders (the comm menu, flight.c and vdu.c; the user's asks, 2026-10-31): a wingman (a fighter of the
 * player's side flown by the AI) flies in formation and fights what comes within its reach (FREE, the default), or
 * keeps station and does not fight (FORMATION: fly in formation), or leaves the formation, goes for the enemy nearest
 * it however far and with none about circles the player (BREAK: break formation), or the player's target wherever it
 * goes (ATTACK: attack my target), or the enemy nearest the player, the one on the player first (HELP: help me out),
 * or the nearest capital ship of its side, circled (RTB: return to base) */
enum { ORDER_FREE, ORDER_ATTACK, ORDER_FORMATION, ORDER_HELP, ORDER_RTB, ORDER_BREAK, ORDER_COUNT };
int16_t logic_wingmen(int16_t *out, int16_t max);   /* the wingmen's slots, in order: n of them */
int16_t logic_is_wingman(int16_t s);    /* slot s is one of them */
int16_t logic_order_ok(int16_t s, int16_t order);   /* 1 when slot s would take that order now (the comm menu) */
int16_t logic_order(int16_t s, int16_t order);      /* 1 when slot s takes it (0: no wingman there, or ATTACK with
                                                       no enemy targeted, or RTB with no capital ship of its side) */
int16_t logic_order_of(int16_t s);
int16_t logic_taunt(int16_t s);         /* an enemy fighter turns on the player for a while: 1 when it does */
extern uint8_t __far logic_boost;       /* RV_BOOST_*: what Kit did to the fighter for this mission's flights; 0 none */
int16_t logic_escort_lost(void);        /* a ship the player was escorting was killed this flight */
int16_t logic_wingman_lost(void);       /* the mission's wingman was killed this flight: their pilot (people.csv row), -1 no */
extern uint16_t __far logic_splashes;   /* this flight's kills by the local pilot (its guns, missiles, hull): a guest's save takes them */

/* the home carrier (the mission's ship with role=home, holding station at nav 0) and landing (the user's rules,
 * 2026-10-31): asked on the comm with the carrier as the target, it clears the player with the mission finished or at
 * least one nav point flown, the player near enough; the player then lands on coming within 2,000 km of its hull, from
 * any side (since 2026-11-04: it was into its bow only).  Not cleared, the hull just stops the fighter, unhurt.  Landing
 * with the mission undone is a loss */
enum { LANDING_CLEARED, LANDING_NOT_YET, LANDING_TOO_FAR };
#define MISSION_CARRIER_LOST 6          /* missionStatus: the home carrier was destroyed: the flight ends with it, as the
                                           player's own death does (game.c's screen; the score is a loss) */
int16_t logic_is_home(int16_t s);       /* slot s is the home carrier, alive */
int16_t logic_landing_allowed(void);
int16_t logic_autoland_ready(void);     /* the landing by the trigger may be had: every waypoint flown, no enemy near, the
                                           home carrier's hull close (the home slot, -1 not now) */
int16_t logic_autoland(void);           /* the trigger pulled: landed then (1), as on coming near; 0 not now */
int16_t logic_request_landing(void);    /* LANDING_*: a clearance stands for the rest of the flight */
int16_t logic_carrier_stopped(void);    /* the hull stopped the player (not cleared) since last asked: 2, 0 no */
/* an escorted ship jumps away, just before its slot ends (flight.c: fx_jump's star and flash); friendly: the player's side */
extern void (*__far logic_on_jump)(int16_t slot, int16_t friendly);
extern void (*__far logic_on_mayday)(int16_t slot);   /* a capital ship's mayday (its type's line): the old game's
                                                          35 in 1,000 a hit through the armour (hull_take) */
void logic_test_leave(int16_t s);        /* the play tests' way to have an enemy leave, as fear or a jump does */
/* a rock's hit on the player (rocks.c): its closing speed (positions a step) and where it struck, the harm.  The old
 * game's for a collision, half the square of the closing speed in km a step (its asteroids hit for a thousand and
 * more), capped as a collision is for the player: at most the shield and the armour on that side (the user,
 * 2026-11-04: "the asteroids were not hurtful at all"; they had hit for some 7 points head on at 250 kps) */
uint16_t logic_rock_harm(int32_t closing, const RvVec3 *where);
#ifdef RV_TESTS
void logic_test_cleared(void);           /* the carrier has cleared the player to land, whatever they have flown */
void logic_test_to_carrier(int16_t where);   /* a script's way there: 0 off its bow, 1 off its beam */
void logic_test_kill_carrier(void);
void logic_test_kill_wingman(void);
void logic_test_kill_enemies(void);     /* a script's way to clear a nav point: every enemy ship there dies */
void logic_test_census(void);           /* a script's look at who is where (each ship's km, held or not); no damage after */
void logic_test_jump_out(void);         /* a script's way to see a jump: the first escorted ship, ahead of the player */
#endif

#endif
