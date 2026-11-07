/* logic.c: the game's logic in flight (logic.h).  THIS FILE IS THE GAME's, to be written: docs/logic.md is its
 * guide (the order of a step, the helpers, the rules the multiplayer layer needs kept).
 *
 * The flight loop (rvflight.inc, flight.c) runs logic_frame once a frame, between reading the controls (g_pilot)
 * and drawing: everything that happens in the world happens here, in the slot arrays (world.h) and the shots
 * (shots.h); the drawing, the cockpit, the music and the asteroid fields then show the world as it stands.  The
 * thunk bodies below (the _impl functions) are reached through RVHOOK's thunks: call do_damage, fireGunFromShip,
 * aiFighterThink and the rest by their thunk names, never the _impl, so the layer sees every call.
 *
 * Everything below is empty but the mission's ships put in their slots at the start and the thunks whose whole job
 * the ABI fixes (marked PLUMBING).  The first flight's stand-ins (the steering, the throttle, the stand-in AI, the
 * damage model's first cut) are in git: `git show 65bc2a7b:retrovega/src/flight.c` (ship_physics, do_damage_impl). */
#include <string.h>
#include <stdlib.h>
#include "rv.h"
#include "rvhooks.h"
#include "campaign.h"
#include "panel.h"
#include "collide.h"
#include "flight.h"
#include "world.h"
#include "shots.h"
#include "hud.h"
#include "logic.h"

typedef char wc_frame_check[LOGIC_HZ == WC_FPS * WC_FRAME_STEPS ? 1 : -1];   /* (world.h) */

PilotInput __far g_pilot;
static int16_t __far g_turn_frac[NUM_SHIP_SLOTS][3];

/* --- the minds' decisions: a turn a slot (step_minds fills it, step_motion flies it), and each mind's memory --- */

typedef struct { int8_t x, y, roll, pad; } Turn;   /* steer_x, steer_y, roll: -9 .. 9, as g_pilot gives them */
static Turn __far g_turn[NUM_SHIP_SLOTS];

/* an AI's memory (the AI, below): what it is doing, and to whom */
typedef struct {
    uint8_t tactic, phase;              /* the maneuver running (T_*), its phase */
    int16_t timer;                      /* steps left in it */
    uint8_t target;                     /* the slot it fights, 0xFF none (rvTarget says the same) */
    uint8_t leader;                     /* the slot it keeps station on, 0xFF none */
    int8_t dir;                         /* which way a break or a loop goes */
    uint8_t burn;                       /* the afterburner this tactic */
    uint8_t fire;                       /* FIRE_*: what step_fire does for it this step */
    uint8_t launcher;                   /* the launcher FIRE_MISSILE fires */
    uint8_t profile;                    /* its AiProfile (P_*) */
    uint8_t fear;                       /* the old game's fear (0-255: the fear model, pick_tactic) */
    uint16_t lock;                      /* steps its target has sat in its missiles' lock cone */
    uint16_t turrets;                   /* a capital ship's turrets to fire this step, a bit each */
    uint16_t since;                     /* the step it last fired a missile */
    uint8_t passing;                    /* a pass on its target is coming this step (the avoidance leaves it out) */
    uint8_t gap;                        /* steps since its last volley (saturating): the profile's fire_gap */
    uint8_t leaving;                    /* 1: gone for good (the leave: away and removed far off); 0 not */
    uint8_t fear_up, fear_down;         /* its fear's rates (the profile's, or the mission ship's own: fear=) */
    int16_t taunt;                      /* steps left of its anger at the player (logic_taunt) */
    uint8_t catching;                   /* 1: closing on its target at full speed (from CATCH_FAR_KM in), 0: at its
                                           attack speed */
    uint8_t burst_left;                 /* the leave's afterburner: steps left of this burst */
    uint8_t leg;                        /* an escort target out with jump=N: the waypoint it is making for (0: the first) */
    uint8_t raiding;                    /* the player's escort target it was sent after, its slot + 1 (raid_target); 0 none */
} Mind;
enum { FIRE_GUNS = 1, FIRE_MISSILE = 2 };
static Mind __far g_mind[NUM_SHIP_SLOTS];

/* a missile's own (its slot has no ship type) */
typedef struct { uint16_t weapon, life; } MissileState;
static MissileState __far g_missile[NUM_SHIP_SLOTS];
static uint8_t __far g_order[NUM_SHIP_SLOTS];   /* the wingmen's orders (ORDER_*, logic.h) */
static uint16_t __far g_lock_steps;     /* the player's missile lock building (lock_tick) */

#define RAM_STEPS 10                    /* a ship hurt by a ram is not hurt by one again for this many steps */
#define AI_SCATTER 18                   /* an AI's shot's scatter about the nose, 256ths each way (18: 4 degrees; gun_fire), up
                                           close: less with the range (scatter_of) */
static int32_t scatter_of(int16_t ship);
static uint16_t __far g_rammed[NUM_SHIP_SLOTS];   /* the step a ship was last hurt by a ram */
static uint8_t __far g_under_fire[NUM_SHIP_SLOTS];   /* the old game's hit counter: a hit adds 4, a step takes 1 (the
                                           fear model's "hit" situation while it stands: no calming) */
static uint16_t __far g_died[NUM_SHIP_SLOTS];     /* the step a ship was seen dying (0: not), its explosion's start */
static uint16_t __far g_killer[NUM_SHIP_SLOTS];   /* what brought its end about (do_damage's src), for delayedDespawn */
/* a capital ship's turrets as the old game's fired theirs (ovr143:284F, 26F6, 1DCF: read in its code 2026-11-05, the
 * user: "I remember the fire rate of the destroyers were very low.... Just enough to take you down a bit but not enough
 * to do you in, if you were careful"; tests/wc1/README.md "Capital ships"): a turret's shot is a flak burst about its
 * target, now and then a bolt: one when the ship has none on its way 40 times in 100, else 8, and a bolt makes it wait 7
 * to 16 of the old game's frames to be ready again.  The step its last bolt went and that wait, on every machine alike
 * (capital_shot runs in the call the layer replays) */
static uint16_t __far g_capbolt_at[NUM_SHIP_SLOTS];
static uint8_t __far g_capbolt_wait[NUM_SHIP_SLOTS];
#define CAP_TURRET_KM 5000              /* its turrets take aim at an enemy within this (the old game's) */
#define CAP_TURRET_FRAMES 10            /* its turrets' turn comes once in so many of the old game's frames: measured, a
                                           still pilot alone 2,500 km behind the old game's Ralari took 259 hits of flak in
                                           4,176 frames, where the code's chances every frame would make some ten times
                                           that; the test (M8.44) has ours at 89 in 1,800 on the smaller Franklin, the old
                                           112 (tests/wc1/README.md "Capital ships") */
#define CAP_SPREAD_FRAMES 6             /* a burst's place a way off the target by so many frames of its travel more
                                           (measured: the old game's flak on a pilot at 20 km a frame, 62 hits in 4,181
                                           frames, fits 6) */
#define CAP_HULL_POINTS 14              /* a capital hull's count in points, a laser's hit (hull_take; ships.csv: count x 14) */
#define WEAPON_FLAK 0x1F                /* the burst's own weapon (data/weapons.csv): not drawn, no sound (step_shots) */
#define FLAK_KM 500                     /* a burst hurts a ship whose hull is within this */

/* the old game's dice for a capital ship's turret: a hash of the step, the ship, the gun and which roll, the same on
 * every machine the layer replays a call on (as an AI's scatter is: gun_fire) */
static uint16_t cap_dice(uint16_t ship, uint16_t gun, uint16_t roll)
{
    uint32_t h = (uint32_t)frameCounter * UINT32_C(2654435761) ^ (uint32_t)ship * UINT32_C(40503)
                 ^ (uint32_t)gun * UINT32_C(9973) ^ (uint32_t)roll * UINT32_C(0x9E3779B9);
    h ^= h >> 15;
    h *= UINT32_C(2246822519);
    h ^= h >> 13;
    return (uint16_t)(h >> 8);
}
uint16_t __far logic_splashes;          /* this flight's kills by the local pilot's own guns, missiles and rams (logic.h) */
uint8_t __far logic_pilot_kills[RV_PILOTS];   /* and by each named pilot the AI flew (logic.h) */
/* the humans in the flight but the local pilot: the ships people fly on other machines (RV_FLAG_HUMAN), noted at each
 * step's start (humans_note) */
#define HUMANS_MAX 4
static int8_t __far g_humans[HUMANS_MAX];
static int16_t g_nhumans;
#define DEATH_STEPS (LOGIC_HZ * 3 / 2)  /* a fighter explodes this long before its slot ends; a capital ship longer */
#define DEATH_STEPS_CAPITAL (LOGIC_HZ * 4)

static uint8_t profile_of(uint16_t type, uint16_t side);
static void mind_rates(int16_t s);      /* (its fear's rates from its profile: the AI, below) */
static void target_tick(void);          /* (the player's target each step: the targeting, below) */
static void kit_boost(void);            /* (what Kit did to the player's fighter: below, with the launchers) */
static void lock_tick(void);            /* (the player's missile lock each step: with the missiles, below) */

void logic_prepare_spawn(int16_t ship_index)
{
    if (ship_index < 0 || ship_index >= NUM_SHIP_SLOTS)   /* (only ship slots keep anything: nothing to forget) */
        return;
    if (ship_index == 0)
        damage_reset();                 /* (a new ship in the player's place: nothing of the last one's damage) */
    _fmemset(&g_turn_frac[ship_index][0], 0, sizeof(g_turn_frac[0]));
    _fmemset(&g_turn[ship_index], 0, sizeof(Turn));
    _fmemset(&g_mind[ship_index], 0, sizeof(Mind));
    g_mind[ship_index].target = g_mind[ship_index].leader = 0xFF;
    g_mind[ship_index].profile = profile_of(gInstanceShipTypes[ship_index], commAnimInfo[ship_index]);
    mind_rates(ship_index);
    g_missile[ship_index].weapon = 0xFFFF;
    g_missile[ship_index].life = 0;
    g_rammed[ship_index] = (uint16_t)(frameCounter - RAM_STEPS);
    g_under_fire[ship_index] = 0;
    g_died[ship_index] = 0;
    g_killer[ship_index] = 0xFFFF;
}

/* the HUD's marks, worked out once a step (hud_marks, the step's end) and drawn band by band (logic_hud, some four
 * times a frame: it only draws).  arm 0: none yet */
static struct {
    int16_t cx, cy, arm;                /* the gunsight: the view's middle (where the nose points), its arms (px) */
    int16_t target;                     /* the target's brackets: 1 when it is on the view */
    int16_t x0, y0, x1, y1, colour;
    int16_t nav;                        /* the chosen nav point's cross: 1 when it is on the view (the user's white cross) */
    int16_t nav_x, nav_y, nav_dx, nav_dy;
    int16_t lock;                       /* the missile lock's reticle: 0 none, 1 building (closing on the target), 2 locked */
    int16_t sx, sy, half, lock_r;
} __far g_hud;

/* --- the mission: its nav points and ships, as camp_load_units put them in the tables the layer reads --- */

#define NAVS 16                         /* navPointTable's records: nav n is record n (nav 0 the launch, the origin) */
#define NAV_REC(n) (navPointTable + navPointTable_SIZE * (n))
#define NAV_AT(n, k) NAV_REC_KM(NAV_REC(n), k)                     /* its place, k 0..2: x, y, z in km (32 bits) */
#define NAV_SHIPS(n) ((const uint16_t *)(NAV_REC(n) + 0x39))         /* its ten mission ships, 0xFFFF ends them */
#define MSHIP(r) (missionShipTable + missionShipTable_SIZE * ((r) & 63))
#define MSHIP_SIDE(r) (*(const uint16_t *)(MSHIP(r) + 2))
#define MSHIP_NAV(r) (*(const uint16_t *)(MSHIP(r) + 8))
#define MSHIP_NO_TURRETS(r) (MSHIP(r)[20])   /* the mission's turrets=off: a capital ship with its turrets silent */
#define MSHIP_ROLE(r) (MSHIP(r)[18])    /* an escort target's role (RV_ESCORT_OUT, RV_ESCORT_IN; campaign.c), 0 none */
#define MSHIP_JUMP(r) (MSHIP(r)[21])    /* role=out's last nav point (missions.txt jump=N), 0 the next after its own */
#define MSHIP_PREY(r) (MSHIP(r)[41])    /* the type of the ship it is ordered to attack (missions.txt prey=KEY) + 1, 0 none: the
                                           old game's own byte (+0x29) for its orders' target (prey_of) */
#define ROUTE_JUMP_KM 3000              /* an escort target in this near nav 0 is home (the carrier is there) */
#define ROUTE_OUT_KM 1000               /* an escort target out this near a nav point is there: it holds at a waypoint
                                           on its way, jumps at its last (the user, 2026-11-03: "lets allow it to be
                                           within 200 km of the nav point ... or even 500 or 1000"; it was 3,000 km
                                           short; at 200 the Ox, 80 kps turning 4 degrees a second, circled 1,300 km
                                           out) */
#define ROUTE_TURN_COS 230              /* its way more than some 25 degrees off its nose: a fifth of its speed, so it
                                           turns in rather than round */
#define ROUTE_WAKE_KM 8000              /* an escort target that waits sets off home when the player is this near, or has
                                           reached its nav point (the player arrives at the point's reach: 10,000 km out
                                           and more) */
#define ROUTE_DOCK_KM 150               /* an escort target in, this near the home carrier's hull, is home: it stops there,
                                           its nose turned the carrier's way (the user, 2026-11-04: "when the ships arrive
                                           within 150km of the vigilance they should stop and ideally turn in the same
                                           direction as the vigilance") */
#define MSHIP_BEHAVIOUR(r) (*(const uint16_t *)(MSHIP(r) + 6))     /* 6 the player, 4 a wingman */
/* a nav point's reach: the player within it has reached the point (enterNavPoint), and the autopilot stops at it, as
 * the old game's did: its nav records carry a radius (+43), 10,000 to 32,500 km, and its autopilot stopped 10,389,
 * 15,189 and 15,335 km short of points of 10,000, 15,000 and 15,000 (traced 2026-11-04, tests/wc1/fullscale.py).  The
 * launch's is HOME_REACH_KM, its Tiger's Claw's in 36 of its 40 missions */
#define HOME_REACH_KM 20000
#define HOME_ARRIVE_KM 4000             /* but the autopilot home, with nothing on the way, stops this far off the home
                                           carrier's hull, facing it (the user, 2026-11-05: "why do we arrive home so far
                                           away from the carrier... that doesn't seem right": at the reach it was some
                                           20,000 km, a minute's flying at cruise; 4,000 is my choice: just outside the
                                           2,000 of a cleared landing, LANDING_NEAR_KM) */
#define LAUNCH_NEAR_KM 5000             /* a hidden point this near the launch is at the launch (wave_may_come) */

static uint16_t g_wave_held;            /* a bit per trigger point whose wave has waited for the enemies out */
static uint8_t g_enemy_ran;             /* the last enemy gone left, not killed */
static uint16_t g_navs_seen;            /* a bit per nav point the player has reached (nav 0 from the start) */
static uint16_t __far g_navs_been;      /* a bit per nav point whose reach this machine's pilot has been in (a guest's
                                           g_navs_seen waits on the host's waves: the landing by the trigger asks this) */
static RvField __far g_fields[RV_FIELDS];   /* the mission's asteroid and mine fields: the autopilot looks out for them */
static int16_t __far g_nfields;
static uint8_t __far g_cleared;         /* the carrier cleared the player to land (logic_request_landing) */
static uint8_t __far g_stopped;         /* the carrier's hull stopped the player, not cleared: 2; cleared but coming
                                           in on the afterburner: 3 (the comm's to say) */
/* the autopilot's flyby formation (logic_form_up): */
static uint32_t __far g_formed;         /* the ships held in the formation, a bit a slot */
static int16_t __far g_form_steps;      /* the steps it holds yet */
static int16_t __far g_form_off[NUM_SHIP_SLOTS][3];   /* each one's place off the player: km right, up, ahead */
static void form_hold(void);
static uint8_t __far g_killed[64];      /* a mission ship (its record) seen dying: shipAiState 9 */
static uint8_t __far g_away[64];        /* a mission ship gone its own way (an escort target jumped out, or home): not killed */

static int16_t nav_defined(int16_t n)
{
    return n == 0 || NAV_REC(n)[0] == n;
}

/* a nav point on the player's lists: home and the mission's waypoints, not its hidden trigger points */
#define NAV_HIDDEN(n) (NAV_REC(n)[1] & 1)
static int16_t nav_listed(int16_t n)
{
    return nav_defined(n) && (n == 0 || !NAV_HIDDEN(n));
}

/* the way the player is headed: the first waypoint not reached yet, home when all are */
static int16_t nav_next_unseen(void)
{
    int16_t n;
    for (n = 1; n < NAVS; n++)
        if (nav_listed(n) && !(g_navs_seen >> n & 1))
            return n;
    return 0;
}

/* N: the next on the list (the waypoints, then home).  g_sys.nav follows the way ahead until the player picks one, and
 * again once that one is reached */
static uint8_t __far g_nav_chosen;

void logic_nav_next(void)
{
    int16_t k = g_sys.nav, i;
    for (i = 0; i < NAVS; i++) {
        k = (int16_t)((k + 1) % NAVS);
        if (nav_listed(k))
            break;
    }
    g_sys.nav = (uint8_t)k;
    g_nav_chosen = 1;
}

static int32_t len_of(int32_t x, int32_t y, int32_t z);

/* nav point n's reach, km (the record's: missions.txt reach=) */
static int32_t nav_reach(int16_t n)
{
    return n == 0 ? HOME_REACH_KM : (int32_t)NAV_REC_REACH(NAV_REC(n));
}

/* the field a waypoint lies in, -1 none (six of them in the 40 missions: Enigma 1's nav 2, as the old game's Enyo 1's).
 * Such a point is the field (the user, 2026-11-06: "in the very first mission, asteroids were supposed to be entirely
 * surrounding nav 2 ... I saw no asteroids anywhere", "the nav point gets ticked off even though I'm nowhere close to
 * it": its reach, 20,000 km, is nearly three times the field's 7,500, so the autopilot stopped, and the point was
 * reached, far outside it): reached only inside the field, the autopilot taking the player in (nav_stop) */
static int16_t nav_field(int16_t n)
{
    int16_t i;
    if (n <= 0 || !nav_listed(n))
        return -1;
    for (i = 0; i < g_nfields; i++)
        if (len_of(NAV_AT(n, 0) - g_fields[i].x, NAV_AT(n, 1) - g_fields[i].y, NAV_AT(n, 2) - g_fields[i].z)
            < g_fields[i].radius)
            return i;
    return -1;
}

/* how far short of nav point n the autopilot stops, km: its reach; for a point in a field, halfway between the point and
 * the field's edge (so inside the field, whichever way it comes) */
static int32_t nav_stop(int16_t n)
{
    int16_t f = nav_field(n);
    int32_t in;
    if (f < 0)
        return nav_reach(n);
    in = ((int32_t)g_fields[f].radius
          - len_of(NAV_AT(n, 0) - g_fields[f].x, NAV_AT(n, 1) - g_fields[f].y, NAV_AT(n, 2) - g_fields[f].z)) / 2;
    return in < nav_reach(n) ? in : nav_reach(n);
}

/* the player within nav point n's reach (measured straight, as the autopilot puts it there); a point in a field, within
 * the field too */
static int16_t nav_in_reach(int16_t n)
{
    int32_t px = TO_KM(gPositionVector[0].x), py = TO_KM(gPositionVector[0].y), pz = TO_KM(gPositionVector[0].z);
    int16_t f = nav_field(n);
    if (f >= 0 && len_of(g_fields[f].x - px, g_fields[f].y - py, g_fields[f].z - pz) >= g_fields[f].radius)
        return 0;
    return len_of(NAV_AT(n, 0) - px, NAV_AT(n, 1) - py, NAV_AT(n, 2) - pz) < nav_reach(n);
}

/* how far the player is from nav point n, km, roughly (the largest axis and half the rest, as player_km) */
static int32_t nav_km(int16_t n)
{
    int32_t dx = labs(TO_KM(gPositionVector[0].x) - NAV_AT(n, 0)), dy = labs(TO_KM(gPositionVector[0].y) - NAV_AT(n, 1)),
            dz = labs(TO_KM(gPositionVector[0].z) - NAV_AT(n, 2)), m = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
    return m + ((dx + dy + dz - m) >> 1);
}

int16_t logic_nav_km(int16_t n, int32_t *km)
{
    if (n < 0 || n >= NAVS || !nav_listed(n))
        return 0;
    *km = nav_km(n);
    return 1;
}

int16_t logic_nav_seen(int16_t n)
{
    return n > 0 && n < NAVS && nav_listed(n) && (g_navs_seen >> n & 1);
}

/* --- the wingmen's orders (logic.h) --- */

static int16_t role_of(int16_t s);

static int16_t is_wingman(int16_t s)
{
    return s > 0 && s < NUM_SHIP_SLOTS && slot_is_ship(s) && maybeEntityType[s] == ET_SHIP && rvController[s] == CTL_AI
           && shipAiState[s] != 9 && slot_is_ship(0) && commAnimInfo[s] == commAnimInfo[0];
}

/* the player's wing, as the comm menu and the radio have it: a fighter of ours this machine's AI flies, not the
 * carrier's patrol (role=stay: it is no wingman of the player's, and ordering it about made no sense); the mission's
 * own wingman first */
int16_t logic_is_wingman(int16_t s)
{
    return is_wingman(s) && role_of(s) != RV_ESCORT_STAY;
}

int16_t logic_wingmen(int16_t *out, int16_t max)
{
    int16_t s, n = 0, pass;
    for (pass = 0; pass < 2; pass++)
        for (s = 1; s < NUM_SHIP_SLOTS && n < max; s++)
            if (logic_is_wingman(s)
                && (slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4) == (pass == 0))
                out[n++] = s;
    return n;
}

static int16_t nearest_capital(int16_t s);

/* an order slot s would take now: ATTACK needs an enemy chosen, RTB a capital ship of its side (the comm menu lists
 * only these) */
int16_t logic_order_ok(int16_t s, int16_t order)
{
    if (!is_wingman(s) || order < 0 || order >= ORDER_COUNT)
        return 0;
    if (order == ORDER_ATTACK && (g_sys.target < 0 || !slot_is_ship(g_sys.target) || shipAiState[g_sys.target] == 9
                                  || commAnimInfo[g_sys.target] == commAnimInfo[s]))
        return 0;
    if (order == ORDER_RTB && nearest_capital(s) < 0)
        return 0;
    return 1;
}

int16_t logic_order(int16_t s, int16_t order)
{
    if (!logic_order_ok(s, order))
        return 0;
    g_order[s] = (uint8_t)order;
    g_mind[s].timer = 0;                /* (its tactic over: the next step picks afresh for the order) */
    return 1;
}

int16_t logic_wingman_lost(void)
{
    int16_t j;
    for (j = 0; j < 64; j++)            /* (g_killed 2: a person flew it, on another machine: no named pilot died) */
        if (g_killed[j] == 1 && MSHIP_BEHAVIOUR(j) == 4 && MSHIP(j)[16] < RV_PILOTS)
            return MSHIP(j)[16];
    return -1;
}

int16_t logic_escort_lost(void)
{
    int16_t j;
    for (j = 0; j < 64; j++)
        if (g_killed[j] && !g_away[j] && MSHIP_ROLE(j) && MSHIP_ROLE(j) != RV_ESCORT_HOME && MSHIP_SIDE(j) == commAnimInfo[0])
            return 1;
    return 0;
}

int16_t logic_order_of(int16_t s)
{
    return s > 0 && s < NUM_SHIP_SLOTS ? g_order[s] : ORDER_FREE;
}

/* an enemy fighter told what the player thinks of it (the comm menu's taunt): it turns on the player and nobody else
 * for TAUNT_STEPS, wherever the player is, and holds no fire for being the player's target (holds_fire).  A ship that
 * has already broken off stays gone (fighter_think); a capital ship does not rise to it: 0 */
#define TAUNT_STEPS (20 * LOGIC_HZ)
int16_t logic_taunt(int16_t s)
{
    if (s <= 0 || s >= NUM_SHIP_SLOTS || !slot_is_ship(s) || maybeEntityType[s] != ET_SHIP || shipAiState[s] == 9
        || commAnimInfo[s] == commAnimInfo[0])
        return 0;
    g_mind[s].taunt = TAUNT_STEPS;
    g_mind[s].timer = 0;                /* (its tactic over: the next step picks afresh, with the player as its target) */
    return 1;
}

/* the mission's rule, a patrol's: every nav point reached and every enemy of the mission's killed */
static int16_t mission_done(void)
{
    const uint16_t *at;
    int16_t n, j;
    for (j = 0; j < 64; j++)            /* (an escort target of the player's side killed: lost, whatever else was done) */
        if (g_killed[j] && !g_away[j] && MSHIP_ROLE(j) && MSHIP_SIDE(j) == commAnimInfo[0])
            return 0;
    for (n = 0; n < NAVS; n++) {
        if (!nav_defined(n))
            continue;
        if (!(g_navs_seen >> n & 1)) {
            if (NAV_HIDDEN(n))
                continue;               /* (a trigger point not come to: it asks nothing of the player) */
            return 0;
        }
        at = NAV_SHIPS(n);
        for (j = 0; j < 10 && at[j] != 0xFFFF; j++)   /* (an enemy that left, afraid, is not in the way) */
            if (MSHIP_SIDE(at[j]) != commAnimInfo[0] && !g_killed[at[j] & 63] && !g_away[at[j] & 63])
                return 0;
    }
    return 1;
}

/* --- the home carrier and landing (logic.h) --- */

static int16_t role_of(int16_t s);

int16_t logic_is_home(int16_t s)
{
    return s > 0 && s < NUM_SHIP_SLOTS && slot_is_ship(s) && shipAiState[s] != 9 && role_of(s) == RV_ESCORT_HOME;
}

/* the user's rule (2026-10-31): the carrier takes the player back with the mission finished or at least one nav point
 * flown (nav 0, the launch, does not count), the player within LANDING_RANGE_KM when asking; a clearance stands */
#define LANDING_RANGE_KM 25000          /* (the autopilot home stops at the launch's reach, 20,000 km, and the carrier is
                                           2,000 behind the launch: it may be asked on the way in) */
static int32_t gap_between(int16_t a, int16_t b);

int16_t logic_landing_allowed(void)
{
    return ((g_navs_seen | g_navs_been) & ~1u) != 0 || mission_done();   /* (been: a guest's own, its waves the host's) */
}

int16_t logic_request_landing(void)
{
    int16_t s;
    if (g_cleared)
        return LANDING_CLEARED;
    if (!logic_landing_allowed())
        return LANDING_NOT_YET;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (logic_is_home(s) && gap_between(0, s) <= LANDING_RANGE_KM) {
            g_cleared = 1;
            return LANDING_CLEARED;
        }
    return LANDING_TOO_FAR;
}

#ifdef RV_TESTS
static void route_jump(int16_t s);
static int16_t escorted(int16_t s);

/* a script's way to see a jump (flight.c's test key): the first escorted ship 2,500 km ahead of the player, gone */
void logic_test_jump_out(void)
{
    int16_t s;
    const RvVec3 *F = &gOrientationFrontVector[0];
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) && shipAiState[s] != 9 && escorted(s)) {
            gPositionVector[s] = vec3(gPositionVector[0].x + F->x * 2500, gPositionVector[0].y + F->y * 2500,
                                      gPositionVector[0].z + F->z * 2500);
            route_jump(s);
            return;
        }
}

/* a script's way to lose the wingman (flight.c's test key) */
void logic_test_kill_wingman(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) && slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4)
            shipAiState[s] = 9;
}

/* a script's way to clear a nav point (flight.c's test key): every enemy ship there begins to die */
void logic_test_kill_enemies(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) && shipAiState[s] != 9 && commAnimInfo[s] != commAnimInfo[0])
            shipAiState[s] = 9;
}

/* a script's look at who is where (flight.c's test key %): each ship's type, side, km from the player and whether the
 * scene holds it (ET_OFFSCENE), and the player takes no damage from then on */
void logic_test_census(void)
{
    static const char __far s_me[] = "census: the player at %ld %ld %ld km, %u kps, nav %d chosen";
    static const char __far s_line[] = "census: slot %d type %02x side %d %ld km off (%ld ahead, %ld right, %ld up), %u kps%s";
    int16_t s;
    playerCanBeDamaged = 0;
    rv_logf(s_me, (long)TO_KM(gPositionVector[0].x), (long)TO_KM(gPositionVector[0].y), (long)TO_KM(gPositionVector[0].z),
            (uint16_t)speedLo[0], g_sys.nav);
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) || maybeEntityType[s] == ET_OFFSCENE) {
            int32_t dx = TO_KM(gPositionVector[s].x - gPositionVector[0].x), dy = TO_KM(gPositionVector[s].y - gPositionVector[0].y),
                    dz = TO_KM(gPositionVector[s].z - gPositionVector[0].z);
            rv_logf(s_line, s, gInstanceShipTypes[s], commAnimInfo[s], (long)player_km(s),
                    (long)(FIX_MUL(gOrientationFrontVector[0].x, dx) + FIX_MUL(gOrientationFrontVector[0].y, dy)
                           + FIX_MUL(gOrientationFrontVector[0].z, dz)),
                    (long)(FIX_MUL(gOrientationRightVector[0].x, dx) + FIX_MUL(gOrientationRightVector[0].y, dy)
                           + FIX_MUL(gOrientationRightVector[0].z, dz)),
                    (long)(FIX_MUL(gOrientationUpVector[0].x, dx) + FIX_MUL(gOrientationUpVector[0].y, dy)
                           + FIX_MUL(gOrientationUpVector[0].z, dz)),
                    (uint16_t)speedLo[s],
                    maybeEntityType[s] == ET_OFFSCENE ? ", held off the scene" : shipAiState[s] == 9 ? ", dying" : "");
        }
}

/* a script's way to see the carrier die (flight.c's test key) */
void logic_test_kill_carrier(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (logic_is_home(s))
            shipAiState[s] = 9;
}

/* a script's way to the carrier (flight.c's test keys): the player 3,000 km off its bow (0) or its beam (1), facing it */
void logic_test_cleared(void)
{
    g_cleared = 1;
}

void logic_test_to_carrier(int16_t where)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (logic_is_home(s)) {
            const RvVec3 *F = &gOrientationFrontVector[s], *R = &gOrientationRightVector[s];
            const RvVec3 *W = where ? R : F;
            RvVec3 *P = &gOrientationFrontVector[0];
            const RvVec3 *U = &gOrientationUpVector[s];
            int32_t up = where ? 75 : 0;   /* (the beam at the deck's height: under it is the gap) */
            gPositionVector[0] = vec3(gPositionVector[s].x + W->x * 3000 + U->x * up,
                                      gPositionVector[s].y + W->y * 3000 + U->y * up,
                                      gPositionVector[s].z + W->z * 3000 + U->z * up);   /* (a unit is 256: x km = positions) */
            *P = vec3(-W->x, -W->y, -W->z);
            gOrientationUpVector[0] = gOrientationUpVector[s];
            gOrientationRightVector[0] = where ? vec3(F->x, F->y, F->z) : vec3(-R->x, -R->y, -R->z);
            gVelocityVector[0] = vec3(0, 0, 0);
            setSpeed[0] = speedLo[0] = 0;
            return;
        }
}
#endif

int16_t logic_carrier_stopped(void)
{
    int16_t was = g_stopped;
    g_stopped = 0;
    return was;
}

/* --- the flight begins --- */

/* the player into slot 0 (the camera and the cockpit are slot 0's) at the launch point, facing ahead, and nav 0's
 * other mission ships (the wing) at it through outerSpawnShipEntity; the other nav points' come when the player
 * gets there (enterNavPoint).  The mission (m) is in the tables already: camp_load_units put it there */
/* Enemies at the launch (the user, 2026-11-01: "if enemies arrive the wingman starts quite far away behind the enemies to
 * sneak up on them"): put down beside the player, a wave had a named wingman dead in under two seconds and for good,
 * head on or under fire, before the player could turn.  The wingman starts past them instead, on the far side from the
 * player and facing them, and its formation place is the usual one off the wing */
#define WING_BEHIND_KM 3000
static int32_t len_of(int32_t x, int32_t y, int32_t z);
static void wingman_behind(void)
{
    int32_t x = 0, y = 0, z = 0, len, horiz;
    int16_t s, wing = -1, n = 0, *off;
    RvVec3 to;
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {
        if (!slot_is_ship(s))
            continue;
        if (commAnimInfo[s] != commAnimInfo[0]) {
            x += TO_KM(gPositionVector[s].x);
            y += TO_KM(gPositionVector[s].y);
            z += TO_KM(gPositionVector[s].z);
            n++;
        } else if (wing < 0 && slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4) {
            wing = s;
        }
    }
    if (!n || wing < 0)
        return;
    x /= n;
    y /= n;
    z /= n;
    len = len_of(x, y, z);              /* (the player is at the launch point, the middle of things) */
    if (len < 100) {
        to = vec3(gOrientationFrontVector[0].x * WING_BEHIND_KM / 256, gOrientationFrontVector[0].y * WING_BEHIND_KM / 256,
                  gOrientationFrontVector[0].z * WING_BEHIND_KM / 256);
    } else {
        to = vec3(x * WING_BEHIND_KM / len, y * WING_BEHIND_KM / len, z * WING_BEHIND_KM / len);
    }
    gPositionVector[wing] = vec3((x + to.x) * KM, (y + to.y) * KM, (z + to.z) * KM);
    horiz = (int32_t)lf_sqrt((uint32_t)((to.x >> 4) * (to.x >> 4) + (to.z >> 4) * (to.z >> 4))) * 16;
    ship_face(wing, lf_atan2(-to.x, -to.z), lf_atan2(-to.y, horiz));
    off = (int16_t *)(formationOffset + 6 * wing);
    off[0] = 800;
    off[1] = off[2] = 0;
}

void logic_start(const RvMission __far *m)
{
    /* Clear the accumulation of fractional turn progress */
    const uint16_t *at = NAV_SHIPS(0);
    int16_t j;
    g_nfields = 0;
    if (m)
        for (j = 0; j < (int16_t)m->nfield && j < RV_FIELDS; j++)
            g_fields[g_nfields++] = m->field[j];
    _fmemset(&g_pilot, 0, sizeof g_pilot);
    _fmemset(g_turn_frac, 0, sizeof(g_turn_frac));
    _fmemset(g_turn, 0, sizeof g_turn);
    _fmemset(g_mind, 0, sizeof g_mind);
    _fmemset(g_missile, 0, sizeof g_missile);
    _fmemset(g_order, 0, sizeof g_order);
    g_formed = 0;
    g_form_steps = 0;
    g_lock_steps = 0;
    _fmemset(g_rammed, 0, sizeof g_rammed);
    _fmemset(g_died, 0, sizeof g_died);
    _fmemset(g_capbolt_at, 0, sizeof g_capbolt_at);
    _fmemset(g_capbolt_wait, 0, sizeof g_capbolt_wait);
    damage_reset();                     /* the player's systems whole, the reserve full (flight.c clears only the cockpit's) */
    _fmemset(g_killed, 0, sizeof g_killed);
    _fmemset(g_away, 0, sizeof g_away);
    _fmemset(&g_hud, 0, sizeof g_hud);   /* (no marks until the first step works them out) */
    g_navs_seen = 1;
    g_navs_been = 1;
    g_wave_held = 0;
    g_enemy_ran = 0;
    g_nav_chosen = 0;
    g_cleared = 0;
    landedOn = 0xFFFF;
    g_stopped = 0;
    currentNavPoint = 0;
    victoryPoints = 0;                  /* (over the launch's roll: won when the mission is done, step_mission) */
    logic_splashes = 0;
    _fmemset(logic_pilot_kills, 0, sizeof logic_pilot_kills);
    g_nhumans = 0;
    /* the player's fighter is the one its record names (camp_load_tables put the save's there; in a shared flight a
     * guest's is the host's choice: docs/multiplayer.md); a test's scene with no player record, the save's */
    ship_spawn(0, MSHIP_BEHAVIOUR(playerMissionShip) == 6 ? *(const uint16_t *)MSHIP(playerMissionShip) : g_camp.player_ship,
               SIDE_TERRAN, CTL_PLAYER, 0, 0, 0, 0);
    shipStateByte[0] = 8;               /* (a human) */
    slotMissionShip[0] = playerMissionShip;
    kit_boost();
    for (j = 0; j < 10 && at[j] != 0xFFFF; j++)
        if (MSHIP_BEHAVIOUR(at[j]) != 6)
            (void)outerSpawnShipEntity(at[j], 0);
    wingman_behind();
}

/* --- a step: the main loop's body --- */

/* the points a store of `most` gains this step to fill in 1 << shift steps, spread evenly over them:
 * phase (the step's place among them) takes its share, and the shares add up to `most`.  The share is
 * (phase + 1) * most >> shift less phase * most >> shift: all that decides it is what adding `most` carries out of
 * the low `shift` bits of phase * most, so 16 bits do (tools/rvdata.py keeps most to 32768 and shift to 15).  The
 * type's shift (ships.csv; blank 9: 512 steps, 17 s, as the old game's Dralthi took 15 s over its 50 points,
 * tests/wc1/README.md) */
static uint16_t refill(uint16_t most, uint16_t phase, uint8_t shift)
{
    return (uint16_t)((((uint16_t)(phase * most) & ((1U << shift) - 1)) + most) >> shift);
}

/* 1. what runs down or builds up by itself: the guns' cooldowns (GUN_STATE), gun energy (gunEnergy), the shields
 * coming back (SHIELD toward SHIELD_MAX), the afterburner's fuel.  (The guns' are M2's.)  The rates are the type's
 * (gun_energy_steps; shield_steps, the old game's stat: a point back on each side every so many of its frames, each
 * WC_FRAME_STEPS of ours, the Hornet 5 and the Dralthi 6, a 40-point shield in 13 s; a type without it fills in
 * 1 << shield_full_shift steps).  The armour
 * doesn't come back in flight; a dying ship's shields stay down; a puppet's are the layer's */
static void step_timers(void)
{
    int16_t ship, k;
    for (ship = 0; ship < NUM_SHIP_SLOTS; ship++) {
        const RvShipType __far *type_of_ship;
        uint8_t shift, energy_steps;
        uint16_t phase;
        if (!slot_is_ship(ship) || shipAiState[ship] == 9 || rvController[ship] == CTL_PUPPET)
            continue;
        type_of_ship = slot_type(ship);
        if (g_under_fire[ship])
            g_under_fire[ship]--;
        shift = type_of_ship ? type_of_ship->shield_full_shift : RV_SHIELD_FULL_SHIFT;
        energy_steps = type_of_ship ? type_of_ship->gun_energy_steps : RV_GUN_ENERGY_STEPS;
        phase = frameCounter & ((1U << shift) - 1);
        /* (the player's shield generator and power plant hurt: the shields and the energy come back on one in
         * 1 + the damage of the steps they would have, counted in their own steps (frameCounter / shield_steps: a
         * type's 3 and a moderate generator's 3 went back at the full rate counted in frames).  The shields wrecked
         * come back not at all; the energy wrecked at a fifth, to 40%: the guns with none left for good took the
         * player out of the fight (the user's)) */
        for (k = 0; k < 2; k+=1) {
            uint16_t most = SHIELD_MAX(ship, k);
            if (ship == 0 && rvController[0] == CTL_PLAYER)
                most = damage_shield_cap(most);   /* (the generator hurt holds less: what is over it goes at once) */
            if (SHIELD(ship, k) != most) {
                uint16_t steps = type_of_ship && type_of_ship->shield_steps ? type_of_ship->shield_steps * WC_FRAME_STEPS : 1;
                uint16_t up = type_of_ship && type_of_ship->shield_steps ? (frameCounter % steps == 0)
                              : refill(most, phase, shift);
                if (ship == 0 && rvController[0] == CTL_PLAYER && systemDamage[SYS_SHIELDS]
                    && (systemDamage[SYS_SHIELDS] >= SYS_WRECKED
                        || (uint16_t)(frameCounter / steps) % (1 + systemDamage[SYS_SHIELDS]) != 0))
                    up = 0;
                SHIELD(ship, k) = SHIELD(ship, k) + up < most ? SHIELD(ship, k) + up : most;
            }
        }
        if (gunEnergy[ship] < (ship == 0 && rvController[0] == CTL_PLAYER ? damage_energy_cap() : 100U)
            && (uint16_t)(frameCounter + ship) % energy_steps == 0
            && !(ship == 0 && rvController[0] == CTL_PLAYER && systemDamage[SYS_POWER]
                 && (uint16_t)(frameCounter / energy_steps) % (1 + systemDamage[SYS_POWER]) != 0)) {
            gunEnergy[ship] += 1;
        }
        {
            int16_t gun_count = GUN_COUNT(ship);
            for (k = 0; k < gun_count; k+=1) {
                const RvWeapon __far *w = weapon(GUN_TYPE(ship, k));
                if (w && (w->kind == RV_WK_GUN || w->kind == RV_WK_TURRET) && (GUN_STATE(ship, k) & 0x7F)) {
                    GUN_STATE(ship, k) -= 1;
                }
            }
        }
    }
}

/* 2. each ship's mind, by its rvController: CTL_PLAYER flies by g_pilot; CTL_AI thinks through the aiFighterThink
 * or aiShipThink thunk (ET_CAPSHIP); CTL_PUPPET is the layer's (it writes the slot: nothing here); CTL_SCRIPT the
 * mission's; CTL_FROZEN nothing.  A mind decides: where to turn, the throttle (aiSetSpeed), whom to target */
static void step_minds(void)
{
    int16_t s;
    for (s = 0; s < NUM_SHIP_SLOTS; s++) {
        if (!slot_is_ship(s))
            continue;
        g_turn[s].x = g_turn[s].y = g_turn[s].roll = 0;
        g_mind[s].fire = 0;
        g_mind[s].turrets = 0;
        if (shipAiState[s] == 9)        /* dying: it neither thinks nor steers (it coasts to its end) */
            continue;
        switch (rvController[s]) {
        case CTL_PLAYER:
            g_turn[s].x = g_pilot.steer_x;
            g_turn[s].y = g_pilot.steer_y;
            g_turn[s].roll = g_pilot.roll;
            if (s == 0) {
                target_tick();
                lock_tick();
            }
            rvTarget[s] = (uint8_t)(g_sys.target >= 0 ? g_sys.target : 0xFF);   /* (the layer reads them) */
            playerTarget = (uint16_t)(g_sys.target >= 0 ? g_sys.target : 0xFFFF);
            break;
        case CTL_AI:                    /* (always the thunk: the layer skips it where the ship is a puppet) */
            if (maybeEntityType[s] == ET_CAPSHIP)
                aiShipThink((uint16_t)s);
            else
                aiFighterThink((uint16_t)s);
            break;
        default:                        /* a puppet (the layer flies it), a script's, a frozen one: nothing of its own */
            break;
        }
    }
}

/* --- the player's targeting (logic.h): T, Y, L and AUTO, with the lock --- */

static int16_t cos_nose_to(int16_t s, int16_t h);
static int32_t km_between(int16_t a, int16_t b);

int16_t logic_target_ahead(int16_t s)
{
    RvVec3 d;
    if (s <= 0 || s >= NUM_SHIP_SLOTS || !slot_is_ship(s) || maybeEntityType[s] == ET_MISSILE || shipAiState[s] == 9)
        return 0;
    d = vec3(TO_KM(gPositionVector[s].x - gPositionVector[0].x), TO_KM(gPositionVector[s].y - gPositionVector[0].y),
             TO_KM(gPositionVector[s].z - gPositionVector[0].z));
    return vec_dot(&d, &gOrientationFrontVector[0]) >= 0;   /* (on the wing line too: a wingman in formation beside
                                                                the player is there to be picked) */
}

/* the ships in front of the pilot, the nearest to the nose first (an enemy only when `enemies`): n of them */
static int16_t targets_ahead(int16_t *out, int16_t enemies)
{
    int16_t n = 0, k, i, j;
    int16_t c[NUM_SHIP_SLOTS];
    for (k = 1; k < NUM_SHIP_SLOTS; k++) {
        int16_t ck;
        if (!logic_target_ahead(k) || (enemies && commAnimInfo[k] == commAnimInfo[0]))
            continue;
        ck = cos_nose_to(0, k);
        for (i = 0; i < n && c[i] >= ck; i++)
            ;
        for (j = n; j > i; j--) {
            out[j] = out[j - 1];
            c[j] = c[j - 1];
        }
        out[i] = k;
        c[i] = ck;
        n++;
    }
    return n;
}

int16_t logic_target_next(void)
{
    int16_t list[NUM_SHIP_SLOTS], n, i;
    if (g_sys.hold)
        return g_sys.target;
    n = targets_ahead(list, 0);
    if (!n)
        return g_sys.target;
    for (i = 0; i < n; i++)
        if (list[i] == g_sys.target) {
            g_sys.target = list[(i + 1) % n];
            return g_sys.target;
        }
    g_sys.target = list[0];
    return g_sys.target;
}

int16_t logic_target_nearest(void)
{
    int16_t list[NUM_SHIP_SLOTS], n, i, best = -1;
    int32_t bd = INT32_C(0x7FFFFFFF), d;
    if (g_sys.hold)
        return g_sys.target;
    n = targets_ahead(list, 1);
    for (i = 0; i < n; i++)
        if ((d = km_between(0, list[i])) < bd) {
            bd = d;
            best = list[i];
        }
    if (best >= 0)
        g_sys.target = best;
    return g_sys.target;
}

int16_t logic_target_lock(void)
{
    if (g_sys.hold) {
        g_sys.hold = 0;
        return 0;
    }
    if (g_sys.target < 0 || !slot_is_ship(g_sys.target) || shipAiState[g_sys.target] == 9 || systemDamage[SYS_TRACK] >= SYS_WRECKED)
        return 0;
    g_sys.hold = 1;
    return 1;
}

/* each step: a target that died is dropped and its lock with it; the lock goes with the tracking system */
static void target_tick(void)
{
    if (g_sys.target >= 0 && (!slot_is_ship(g_sys.target) || shipAiState[g_sys.target] == 9)) {
        g_sys.target = -1;
        g_sys.hold = 0;
    }
    if (systemDamage[SYS_TRACK] >= SYS_WRECKED)
        g_sys.hold = 0;
}

/* 3. the fire loop: who pulls a trigger this step (the player's g_pilot.guns with g_sys.armed, g_pilot.missile;
 * the AI's own decisions) and which guns are ready (cooled, the energy there): each one fired through the
 * fireGunFromShip thunk (or maybe_fire_all_guns), which makes the shot */
static void guns_fire_ready(uint16_t ship);

static int16_t is_launcher(int16_t s, int16_t g)
{
    const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
    return w && (w->kind == RV_WK_MISSILE || w->kind == RV_WK_TORPEDO || w->kind == RV_WK_ROCKET);
}

/* the player talked to Kit before the launch (game.c, a line of hers with boost=KIND): half as much again on all four
 * armour faces (the displays read full until the extra is gone), or one more round in the first launcher.  The fuel's
 * is the flight's (g_sys.fuel_extra) */
uint8_t __far logic_boost;

static void kit_boost(void)
{
    int16_t k;
    if (logic_boost == RV_BOOST_ARMOR) {
        for (k = 0; k < 4; k++)
            ARMOR(0, k) += ARMOR(0, k) / 2;
    } else if (logic_boost == RV_BOOST_MISSILE) {
        for (k = 0; k < GUN_COUNT(0); k++)
            if (is_launcher(0, k)) {
                GUN_STATE(0, k) += 1;
                break;
            }
    }
    if (logic_boost)
        RV_LOG("logic: Kit's boost %d", logic_boost);
}

/* a ship's launcher with rounds left: the first, or, the player's, the one chosen (g_sys.launcher_sel: the n-th
 * launcher) when it has rounds; -1 none */
static int16_t launcher_of(int16_t s)
{
    int16_t g, n = 0, first = -1;
    for (g = 0; g < GUN_COUNT(s); g++) {
        if (!is_launcher(s, g))
            continue;
        if (!(GUN_STATE(s, g) & GUN_DESTROYED) && (GUN_STATE(s, g) & 0x7F)) {
            if (s == 0 && rvController[0] == CTL_PLAYER && n == g_sys.launcher_sel)
                return g;
            if (first < 0)
                first = g;
        }
        n++;
    }
    return first;
}

/* --- the player's weapon choice (logic.h) --- */

int16_t logic_gun_types(uint16_t *out, int16_t max)
{
    int16_t g, n = 0, i;
    for (g = 0; g < GUN_COUNT(0) && n < max; g++) {
        const RvWeapon __far *w = weapon(GUN_TYPE(0, g));
        if (!w || (w->kind != RV_WK_GUN && w->kind != RV_WK_TURRET))
            continue;
        for (i = 0; i < n && out[i] != GUN_TYPE(0, g); i++)
            ;
        if (i == n)
            out[n++] = GUN_TYPE(0, g);
    }
    return n;
}

int16_t logic_guns_next(void)
{
    uint16_t types[8];
    int16_t n = logic_gun_types(types, 8);
    g_sys.gun_sel = (uint8_t)(n > 1 ? (g_sys.gun_sel + 1) % (n + 1) : 0);   /* (one type: the choice is all of them) */
    return g_sys.gun_sel;
}

int16_t logic_gun_selected(int16_t g)
{
    uint16_t types[8];
    int16_t n;
    if (!g_sys.gun_sel)
        return 1;
    n = logic_gun_types(types, 8);
    return g_sys.gun_sel <= n && GUN_TYPE(0, g) == types[g_sys.gun_sel - 1];
}

int16_t logic_missile_next(void)
{
    int16_t g, n = 0;
    for (g = 0; g < GUN_COUNT(0); g++)
        if (is_launcher(0, g))
            n++;
    g_sys.launcher_sel = (uint8_t)(n ? (g_sys.launcher_sel + 1) % n : 0);
    return g_sys.launcher_sel;
}

int16_t logic_launcher(void)
{
    return launcher_of(0);
}

static uint8_t launch_target(int16_t ship, const RvWeapon __far *w);

/* a launcher fires: whom its missile is for published first in missileTarget[ship], where the layer reads it with the
 * shot (capture_lock) and puts it on the other machines before replaying it there (missile_launch reads it for a
 * puppet), then the shot through the thunk */
static void fire_launcher(int16_t ship, int16_t g)
{
    const RvWeapon __far *w = weapon(GUN_TYPE(ship, g));
    if (w)
        missileTarget[ship] = launch_target(ship, w);
    (void)fireGunFromShip((uint16_t)ship, (uint16_t)g);
}

static void step_fire(void)
{
    int16_t s, g;
    /* the player: the missile and gun key */
    if (slot_is_ship(0) && rvController[0] == CTL_PLAYER && shipAiState[0] != 9 &&  g_sys.armed)
    {
        if (g_pilot.missile) {
            int16_t g = launcher_of(0);
            if (g >= 0) {
                fire_launcher(0, g);
            }
        }
        if (g_pilot.guns) {             /* (the trigger: all the guns, or the type chosen: guns_fire_ready) */
            if (g_sys.gun_sel)
                guns_fire_ready(0);
            else
                maybe_fire_all_guns(0);
        }
    }
    /* the minds' choices (aiFighterThink, aiShipThink left them in g_mind): through the thunks */
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {
        if (!slot_is_ship(s) || rvController[s] != CTL_AI || shipAiState[s] == 9)
            continue;
        if (g_mind[s].fire & FIRE_GUNS)
            maybe_fire_all_guns((uint16_t)s);
        if (g_mind[s].fire & FIRE_MISSILE)
            fire_launcher(s, (int16_t)g_mind[s].launcher);
        if (g_mind[s].turrets)          /* a capital ship's turrets that bear */
            for (g = 0; g < GUN_COUNT(s) && g < 16; g++)
                if (g_mind[s].turrets & (1u << g))
                    (void)fireGunFromShip((uint16_t)s, (uint16_t)g);
    }
}


int32_t vec_len(const RvVec3 *a) {
    return lf_sqrt((uint32_t)(a->x * a->x + a->y * a->y + a->z * a->z));
}

static int32_t reduce_slide(int16_t ship_index)
{
    const RvShipType __far *t = slot_type(ship_index);
    /* the speed above which a full-rate turn slides (ships.csv) */
    int32_t slide = ship_index == 0 && systemDamage[SYS_ABSORBERS] ? damage_slide(t->slide_dmg_kps) : t->slide_kps;
    int32_t target_speed = speedLo[ship_index];
    const RvVec3 *front = &gOrientationFrontVector[ship_index];
    int32_t step = KPS_STEP(target_speed);
    RvVec3 target = vec3(
        FIX_MUL(front->x, step),
        FIX_MUL(front->y, step),
        FIX_MUL(front->z, step));
    RvVec3 *velocity = &gVelocityVector[ship_index];
    RvVec3 delta = vec3(
        target.x - velocity->x,
        target.y - velocity->y,
        target.z - velocity->z);
    int32_t delta_len = vec_len(&delta);
    /* at full rate the nose turns yaw_dps / KPS_HZ degrees a step: at the slide speed that moves the velocity it asks
     * for by `a` a step, the most it can follow.  (The float part is all constants: it folds to 1146) */
    int32_t a = KPS_STEP(slide) * t->yaw_dps / (int32_t)(KPS_HZ * 180.0 / RV_PI + 0.5);
    if (delta_len <= a) {
        *velocity = target;
    } else {
        velocity->x += delta.x * a / delta_len;
        velocity->y += delta.y * a / delta_len;
        velocity->z += delta.z * a / delta_len;
    }
    return target_speed;
}


static void step_speed(int16_t ship_index, uint16_t phase)
{
    uint16_t v = (uint16_t)speedLo[ship_index];
    const RvShipType __far *t = slot_type(ship_index);
    const int16_t player = ship_index == 0 && rvController[0] == CTL_PLAYER;
    uint16_t top = player ? damage_kps(t->max_kps) : t->max_kps;   /* (the player's engine hurt gives less of all of it) */
    uint16_t accel = player ? damage_accel(t->accel) : t->accel;
    uint16_t desired_speed = top;
    if (rvAfterburner[ship_index] && t->ab_kps) {
        desired_speed = player ? damage_kps(t->ab_kps) : t->ab_kps;
    } else if (setSpeed[ship_index] < top) {
        desired_speed = setSpeed[ship_index];
    }
    {
        uint16_t dv = accel / KPS_HZ + (phase < accel % KPS_HZ);

        if (v < desired_speed) {
            v = desired_speed - v > dv ? v + dv : desired_speed;
        } else {
            v = v - desired_speed > dv ? v - dv : desired_speed;
        }
    }
    speedLo[ship_index] = v;
}

static int16_t turn_units(int16_t ship_index, int16_t axis, int32_t amount)
{
    int32_t acc;
    int16_t whole_part;
    RV_CHECK(ship_index, NUM_SHIP_SLOTS);   /* (a ship slot always: step_motion's loop; rv_fail if not) */
    RV_CHECK(axis, 3);
    acc = g_turn_frac[ship_index][axis] + amount;
    /* Note that the implementation-defined behavior of >> on negatives is
       divide due to rv_sar_check. */
    whole_part = (int16_t)(acc >> TURN_FRAC_SHIFT);
    g_turn_frac[ship_index][axis] = (int16_t)(acc - (int32_t) whole_part * (1 << TURN_FRAC_SHIFT));
    return whole_part;
}


static void step_turn(int16_t ship_index,
                      int8_t steer_x,
                      int8_t steer_y,
                      int8_t roll)
{
    const int16_t player = ship_index == 0 && rvController[0] == CTL_PLAYER;
    if (player) {                       /* (the thrusters badly hurt pull the nose to the side that lost them) */
        steer_x = (int8_t)(steer_x + damage_pull());
    }
    if (steer_x || steer_y || roll) {
        const RvShipType __far *t = slot_type(ship_index);
        if (steer_x) {
            vec_turn(&gOrientationFrontVector[ship_index],
                     &gOrientationRightVector[ship_index],
                     turn_units(ship_index, 0, TURN_STEP(player ? damage_turn_rate(t->yaw_dps) : t->yaw_dps, steer_x)));
        }
        if (steer_y) {
            vec_turn(&gOrientationFrontVector[ship_index],
                     &gOrientationUpVector[ship_index],
                     turn_units(ship_index, 1, TURN_STEP(player ? damage_turn_rate(t->pitch_dps) : t->pitch_dps, steer_y)));
        }
        if (roll) {
            vec_turn(&gOrientationRightVector[ship_index],
                     &gOrientationUpVector[ship_index],
                     turn_units(ship_index, 2, TURN_STEP(player ? damage_turn_rate(t->roll_dps) : t->roll_dps, roll)));
        }
        ship_orthonormalise(ship_index);
    }
}
  

/* ====================================================================================================================
 * Helpers of the minds, the missiles and the collisions: angles and lengths in km (positions overflow when squared)
 * ==================================================================================================================== */

/* the angle of (x, y) in the plane of x (0) and y (a quarter turn), -512 .. 511 (TURN a whole turn): the arguments
 * halved until lf_atan2 takes them (under 2^16) */
static int16_t angle_of(int32_t y, int32_t x)
{
    int16_t t;
    while (x >= 32768L || x <= -32768L || y >= 32768L || y <= -32768L) {
        x /= 2;
        y /= 2;
    }
    t = lf_atan2(y, x);
    return (int16_t)(t >= 512 ? t - 1024 : t);
}

/* |(x, y, z)|, the parts halved until their squares fit */
static int32_t len_of(int32_t x, int32_t y, int32_t z)
{
    int32_t k = 1;
    while (labs(x) > 30000L || labs(y) > 30000L || labs(z) > 30000L) {
        x /= 2;
        y /= 2;
        z /= 2;
        k *= 2;
    }
    return (int32_t)lf_sqrt((uint32_t)(x * x) + (uint32_t)(y * y) + (uint32_t)(z * z)) * k;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* km between two slots */
static int32_t km_between(int16_t a, int16_t b)
{
    return len_of(TO_KM(gPositionVector[b].x - gPositionVector[a].x), TO_KM(gPositionVector[b].y - gPositionVector[a].y),
                  TO_KM(gPositionVector[b].z - gPositionVector[a].z));
}

/* how far apart two ships are, hull to hull (the user, 2026-10-31: "exclude the sum of the radii, since it is effectively
 * that much closer"), at least 1.  What a distance to a target means: in an attack, a turret's and a lock's range, the
 * landing's limit, the cockpit's readout.  Fighters: the middles' distance less the sum of their half boxes.  A capital
 * ship, under any setting, by its actual shape (the user again): along the line to its middle as far as its silhouettes
 * begin, less the other's half box; with no hull on that line (it runs under the Vigilance's deck) to the box its
 * silhouettes fill, so that a fighter beside a hollow does not take itself to be far from everything */
#define GAP_MAP_KM 30000                /* (collide_segment's reach; further off the radius will do) */
static int32_t gap_of(int32_t km, int16_t a, int16_t b)
{
    int32_t room;
    int16_t cap = maybeEntityType[b] == ET_CAPSHIP ? b : a, other = cap == b ? a : b, t;
    LfVec hit;
    RvVec3 r;
    const RvShipType __far *ty;
    if (maybeEntityType[a] == ET_CAPSHIP && maybeEntityType[b] == ET_CAPSHIP && collide_half(a) > collide_half(b)) {
        cap = a;
        other = b;
    }
    if (maybeEntityType[cap] != ET_CAPSHIP || km > GAP_MAP_KM)
        room = TO_KM(collide_half(a) + collide_half(b));
    else {
        room = TO_KM(collide_half(other));
        if (collide_segment(cap, (const LfVec *)&gPositionVector[other], (const LfVec *)&gPositionVector[cap], &hit, &t))
            km = km * t / 1024;
        else if ((ty = slot_type(cap)) != 0) {
            int32_t half = TO_KM(collide_half(cap)), m;
            ship_rel(cap, other, &r);
            r.x = labs(r.x) - half * ty->hit[0] / 127;
            r.y = labs(r.y) - half * ty->hit[1] / 127;
            r.z = labs(r.z) - half * ty->hit[2] / 127;
            r.x = r.x < 0 ? 0 : r.x;
            r.y = r.y < 0 ? 0 : r.y;
            r.z = r.z < 0 ? 0 : r.z;
            m = r.x > r.y ? (r.x > r.z ? r.x : r.z) : (r.y > r.z ? r.y : r.z);   /* (the largest and half the rest) */
            km = m + ((r.x + r.y + r.z - m) >> 1);
        }
    }
    return km > room + 1 ? km - room : 1;
}

static int32_t gap_between(int16_t a, int16_t b)
{
    return gap_of(km_between(a, b), a, b);
}

int32_t logic_target_km(int16_t s)
{
    return gap_between(0, s);
}

/* a world vector (km, or anything under 2^23) as s's frame sees it: right, up, ahead */
static void to_body(int16_t s, const RvVec3 *d, RvVec3 *out)
{
    out->x = vec_dot(d, &gOrientationRightVector[s]) / 256;
    out->y = vec_dot(d, &gOrientationUpVector[s]) / 256;
    out->z = vec_dot(d, &gOrientationFrontVector[s]) / 256;
}

/* what a shot of s's carries: s's velocity along its nose, positions a step (the old game's bolts, read in its code
 * 2026-10-31: the gun's speed plus the dot of the ship's velocity and the gun's line, nothing of its slide: a
 * sliding ship's shots go where its nose points) */
static RvVec3 carried_of(int16_t s)
{
    const RvVec3 *F = &gOrientationFrontVector[s];
    int32_t along = vec_dot(&gVelocityVector[s], F) / 256;
    return vec3(FIX_MUL(F->x, along), FIX_MUL(F->y, along), FIX_MUL(F->z, along));
}

/* s flying along its nose at its speed (after a jump: no momentum the other way to slide off) */
static void fly_ahead(int16_t s)
{
    const RvVec3 *F = &gOrientationFrontVector[s];
    int32_t step = KPS_STEP(speedLo[s]);
    gVelocityVector[s] = vec3(FIX_MUL(F->x, step), FIX_MUL(F->y, step), FIX_MUL(F->z, step));
}

/* where a gun record's mount is (positions): across and up in 32nds of the half box, and the nose ahead; a capital
 * ship's turrets along its hull instead, the i-th of n from the stern to the bow (every one had sat at the bow, so a
 * destroyer's bolts all came from its front: the user, 2026-11-05, "it seems to only shoot to the front") */
static const RvShipType __far* gun_mount(int16_t s, int16_t g, RvVec3 *out)
{
    const RvShipType __far *t = slot_type(s);
    int32_t h = collide_half(s), ax = (int32_t)GUN_MOUNT_X(s, g) * (h / 32), ay = (int32_t)GUN_MOUNT_Y(s, g) * (h / 32);
    int32_t az = t ? (int32_t)t->hit[2] * (h / 127) : h / 2;
    const RvVec3 *R = &gOrientationRightVector[s], *U = &gOrientationUpVector[s], *F = &gOrientationFrontVector[s];
    if (t && maybeEntityType[s] == ET_CAPSHIP) {
        const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
        if (w && w->kind == RV_WK_TURRET) {
            int16_t i = 0, n = 0, k;
            for (k = 0; k < GUN_COUNT(s) && k < 16; k++) {
                const RvWeapon __far *wk = weapon(GUN_TYPE(s, k));
                if (wk && wk->kind == RV_WK_TURRET) {
                    n++;
                    if (k < g)
                        i++;
                }
            }
            az = az * (2 * i + 1 - n) / (n ? n : 1);
        }
    }
    out->x = gPositionVector[s].x + FIX_MUL(R->x, ax) + FIX_MUL(U->x, ay) + FIX_MUL(F->x, az);
    out->y = gPositionVector[s].y + FIX_MUL(R->y, ax) + FIX_MUL(U->y, ay) + FIX_MUL(F->y, az);
    out->z = gPositionVector[s].z + FIX_MUL(R->z, ax) + FIX_MUL(U->z, ay) + FIX_MUL(F->z, az);
    return t;
}

/* the first of s's guns (a gun or a turret, not destroyed), -1 none: its weapon says how far and fast they shoot */
static int16_t gun_of(int16_t s)
{
    int16_t g;
    for (g = 0; g < GUN_COUNT(s); g++) {
        const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
        if (w && (w->kind == RV_WK_GUN || w->kind == RV_WK_TURRET) && !(GUN_STATE(s, g) & GUN_DESTROYED))
            return g;
    }
    return -1;
}

/* ====================================================================================================================
 * The guns (M2: fireGunFromShip's guns and turrets, maybe_fire_all_guns): to be written (docs/gameplay.md)
 * ==================================================================================================================== */

/* (ship, gun, its weapon): a gun or a turret fires: a shot from its mount (gun_mount), along the nose at the
 * weapon's speed plus the ship's own along that line (carried_of; a turret's toward rvTarget[ship]'s lead point:
 * the ship's mind aims it); the cooldown into GUN_STATE's low bits, the energy off gunEnergy.  Returns 0xFFFF */
/* (ship, gun, its weapon, its target): a capital ship's turret fires as the old game's (ovr143:26F6): at a place
 * about the target, a way off on each axis, up to 400 km (a sixth of the range beyond 2,400) and CAP_SPREAD_FRAMES
 * frames' travel of the target's more (a target that moves is the harder to hit), 1,500 km at the most, either way; a bolt there from the mount (40 in 100 when the ship
 * is ready for one, else 8: g_capbolt_at) or, the rest, a flak burst there (WEAPON_FLAK, for step_shots).  Its dice
 * are the step's, the ship's and the gun's: every machine that runs the call makes the same shot */
static uint16_t capital_shot(uint16_t ship, uint16_t gun, const RvWeapon __far *w, int16_t target)
{
    RvVec3 muzzle, at, zero;
    int32_t off[3], b, km;
    int16_t i, r;
    (void)gun_mount((int16_t)ship, (int16_t)gun, &muzzle);
    /* (the target's speed in km an old frame, times CAP_SPREAD_FRAMES; positions a step: KM a km) */
    b = len_of(TO_KM(gVelocityVector[target].x), TO_KM(gVelocityVector[target].y), TO_KM(gVelocityVector[target].z))
        * WC_FRAME_STEPS * CAP_SPREAD_FRAMES;
    km = km_between((int16_t)ship, target) / 6;
    if (km < 400)
        km = 400;
    for (i = 0; i < 3; i++) {
        int32_t a = (int32_t)(cap_dice(ship, gun, (uint16_t)(3 + i)) % (uint16_t)km) + b;
        if (a > 1500)
            a = 1500;
        off[i] = cap_dice(ship, gun, (uint16_t)(6 + i)) & 1 ? -a : a;
    }
    at = vec3(gPositionVector[target].x + off[0] * KM, gPositionVector[target].y + off[1] * KM,
              gPositionVector[target].z + off[2] * KM);
    r = (int16_t)(cap_dice(ship, gun, 9) % 100);
    if (r < 8 || (r < 40 && (uint16_t)(frameCounter - g_capbolt_at[ship]) >= g_capbolt_wait[ship]) || !weapon(WEAPON_FLAK)) {
        RvVec3 d = vec3(TO_KM(at.x - muzzle.x), TO_KM(at.y - muzzle.y), TO_KM(at.z - muzzle.z)), vel;
        int32_t step = KPS_STEP(w->speed_kps);
        km = len_of(d.x, d.y, d.z);
        while (labs(d.x) > 30000L || labs(d.y) > 30000L || labs(d.z) > 30000L) {   /* (a direction: 256 long) */
            d.x /= 2;
            d.y /= 2;
            d.z /= 2;
            km /= 2;
        }
        if (km) {
            d.x = d.x * 256 / km;
            d.y = d.y * 256 / km;
            d.z = d.z * 256 / km;
        } else
            d = gOrientationFrontVector[ship];
        vel = vec3(FIX_MUL(d.x, step), FIX_MUL(d.y, step), FIX_MUL(d.z, step));
        /* its life the way there less 5 to 12 of the old game's frames (spawnShip1B: the old bolts die short of the
         * place, and seldom reach a fighter: one hit in 4,176 frames on a pilot 2,500 km off, measured) */
        km = len_of(TO_KM(at.x - muzzle.x), TO_KM(at.y - muzzle.y), TO_KM(at.z - muzzle.z)) * KM / (step ? step : 1)
             - (int32_t)(5 + cap_dice(ship, gun, 11) % 8) * WC_FRAME_STEPS;
        i = shot_add((int16_t)ship, GUN_TYPE(ship, gun), &muzzle, &vel);
        if (i >= 0)
            shot(i)->life = (uint16_t)(km > 1 ? (km < 0xFFFFL ? km : 0xFFFF) : 1);
        g_capbolt_at[ship] = frameCounter;
        g_capbolt_wait[ship] = (uint8_t)((7 + cap_dice(ship, gun, 10) % 10) * WC_FRAME_STEPS);
    } else {
        zero = vec3(0, 0, 0);
        i = shot_add((int16_t)ship, WEAPON_FLAK, &at, &zero);
        if (i >= 0)
            shot(i)->life = 2;          /* (it bursts after one step: step_shots) */
    }
    GUN_STATE(ship, gun) = (uint8_t)((GUN_STATE(ship, gun) & GUN_DESTROYED) | (w->refire & 0x7F));
    gunEnergy[ship] = (uint16_t)(gunEnergy[ship] > w->energy ? gunEnergy[ship] - w->energy : 0);
    return 0xFFFF;
}

static uint16_t gun_fire(uint16_t ship, uint16_t gun, const RvWeapon __far *w)
{
    RvVec3 physical_offset, vel;
    int32_t step = KPS_STEP(w->speed_kps);
    int16_t target = rvTarget[ship];
    RvVec3 front = gOrientationFrontVector[ship];
    if (w->kind == RV_WK_TURRET && maybeEntityType[ship] == ET_CAPSHIP && target != 0xff && slot_is_ship(target))
        return capital_shot(ship, gun, w, target);
    (void)gun_mount(ship, gun, &physical_offset);
    if (w->kind == RV_WK_TURRET && target != 0xff && slot_is_ship(target)) {
        /* a turret's shot toward the target's lead point (km from the mount; the shot's time there, twice refined),
         * the target's velocity less what the shot carries of the ship's own (below) */
        RvVec3 d, c = carried_of((int16_t)ship), vt = vec3(TO_KM(gVelocityVector[target].x - c.x),
                                                            TO_KM(gVelocityVector[target].y - c.y),
                                                            TO_KM(gVelocityVector[target].z - c.z));
        int32_t km, tof, kps_step = w->speed_kps / KPS_HZ + 1;
        d = vec3(TO_KM(gPositionVector[target].x - physical_offset.x),
                 TO_KM(gPositionVector[target].y - physical_offset.y),
                 TO_KM(gPositionVector[target].z - physical_offset.z));
        tof = len_of(d.x, d.y, d.z) / kps_step;
        tof = len_of(d.x + vt.x * tof, d.y + vt.y * tof, d.z + vt.z * tof) / kps_step;
        d = vec3(d.x + vt.x * tof, d.y + vt.y * tof, d.z + vt.z * tof);
        km = len_of(d.x, d.y, d.z);
        while (labs(d.x) > 30000L || labs(d.y) > 30000L || labs(d.z) > 30000L) {   /* (a direction: 256 long) */
            d.x /= 2;
            d.y /= 2;
            d.z /= 2;
            km /= 2;
        }
        if (km) {
            d.x = d.x * 256 / km;
            d.y = d.y * 256 / km;
            d.z = d.z * 256 / km;
            front = d;
        }
    }
    if (rvController[ship] == CTL_AI && w->kind == RV_WK_GUN) {
        /* an AI's shot scatters up to AI_SCATTER about the nose (the old game's fighters hit a hunting player with 13%
         * of their shots and a straight one with 28%; with the fine aim alone ours hit 30% and 44% under the lead
         * rule, and a tail under the cone rule, its nose held exactly on a straight player, hit with two shots in
         * three where the old tail's nose wandered within 25 degrees and hit with one in four), by a hash of the
         * step, the ship and the gun: the same on every machine the layer replays the call on */
        uint16_t h = (uint16_t)(frameCounter * 2654435U + ship * 40503U + gun * 9973U);
        int32_t sc = scatter_of(ship);
        int32_t jx = ((int32_t)(h & 31) - 16) * sc / 16, jy = ((int32_t)((h >> 5) & 31) - 16) * sc / 16;
        const RvVec3 *R = &gOrientationRightVector[ship], *U = &gOrientationUpVector[ship];
        front.x += FIX_MUL(R->x, jx) + FIX_MUL(U->x, jy);
        front.y += FIX_MUL(R->y, jx) + FIX_MUL(U->y, jy);
        front.z += FIX_MUL(R->z, jx) + FIX_MUL(U->z, jy);
    }
    /* along the gun's line at the weapon's speed plus the ship's own speed along that line (the old game's rule,
     * read in its code 2026-10-31: the dot of its velocity and the line, nothing of its slide; the fork's shots
     * carried the whole velocity and drifted sideways where the old ones did not) */
    step += vec_dot(&gVelocityVector[ship], &front) / 256;
    vel = vec3(FIX_MUL(front.x, step),
               FIX_MUL(front.y, step),
               FIX_MUL(front.z, step));
    (void)shot_add((int16_t)ship, GUN_TYPE(ship, gun), &physical_offset, &vel);
    GUN_STATE(ship, gun) = (uint8_t)((GUN_STATE(ship, gun) & GUN_DESTROYED) | (w->refire & 0x7F));
     gunEnergy[ship] = (uint16_t)(gunEnergy[ship] > w->energy ? gunEnergy[ship] - w->energy : 0);
    return 0xFFFF;
}

/* (ship): each gun or turret that is ready (not destroyed, cooled, the energy there), through the fireGunFromShip
 * thunk; never a launcher */
static void guns_fire_ready(uint16_t ship)
{
    int16_t g;
    int16_t gun_count = GUN_COUNT(ship);
    uint16_t already_tried_bits = 0;
    for (g = 0; g < gun_count; g += 1) {
        int linked_count = 1;
        int16_t linked;
        uint16_t this_type = GUN_TYPE(ship, g);
        const RvWeapon __far *w = weapon(this_type);
        if (!w || (w->kind != RV_WK_GUN && w->kind != RV_WK_TURRET)) {
            continue;
        }
        if (ship == 0 && rvController[0] == CTL_PLAYER && !logic_gun_selected(g)) {   /* (the player's choice, G) */
            continue;
        }
        if ((GUN_STATE(ship, g) & (0x7f | GUN_DESTROYED)) || gunEnergy[ship] < w->energy) {
            continue;
        }
        if ((1 << g) & already_tried_bits) {
            continue; /* we have tried to link-fire this weapon type */
        }


        for (linked = g + 1 ; linked < gun_count; linked += 1) {
            if (GUN_TYPE(ship, linked) == this_type && (GUN_STATE(ship, linked) & (0x7f | GUN_DESTROYED)) == 0) {
                already_tried_bits |= (1 << linked);
                linked_count += 1;
            }
        }
        if (gunEnergy[ship] < w->energy * linked_count) {
            continue;
        }
        for (linked = g ; linked < gun_count; linked += 1) {
            if (GUN_TYPE(ship, linked) == this_type && (GUN_STATE(ship, linked) & (0x7f | GUN_DESTROYED)) == 0) {
                (void)fireGunFromShip(ship, (uint16_t)linked);
            }
        }
    }
}

/* ====================================================================================================================
 * Collisions (M6): two ships inside one another are parted and both hurt
 * ==================================================================================================================== */

#define RAM_BASE 2                      /* the soft rule (RV_RULE_RAM_SOFT): a touch's damage */
#define RAM_PER 1600                    /* and positions a step of closing speed a point more (125 kps: two fighters
                                           head on at their top speeds bump for a dozen points) */

static void bump_away(int16_t s, int8_t dir);   /* (an AI ship's mind after a bump: the AI, below) */

/* a ship gives way in a collision: a frozen one or a puppet (the layer's) never */
static int16_t ram_moves(int16_t s)
{
    return rvController[s] != CTL_FROZEN && rvController[s] != CTL_PUPPET;
}

/* moved by part / 256 of push (positions); its velocity's part toward the other ship (into: the sign of the push that
 * points at it) taken away, and its speed quartered: a collision all but stops a ship */
static void ram_shove(int16_t s, const LfVec *push, int16_t part, int16_t into)
{
    RvVec3 n, *v = &gVelocityVector[s];
    int32_t d;
    gPositionVector[s].x += push->x / 256 * part + (push->x % 256) * part / 256;
    gPositionVector[s].y += push->y / 256 * part + (push->y % 256) * part / 256;
    gPositionVector[s].z += push->z / 256 * part + (push->z % 256) * part / 256;
    n = vec3(TO_KM(push->x), TO_KM(push->y), TO_KM(push->z));   /* (its way: km, so the squares fit) */
    vec_norm(&n);
    d = vec_dot(v, &n) / 256 * into;    /* (the velocity toward the other ship) */
    if (d > 0) {
        v->x -= FIX_MUL(n.x, d) * into;
        v->y -= FIX_MUL(n.y, d) * into;
        v->z -= FIX_MUL(n.z, d) * into;
    }
    speedLo[s] = speedLo[s] / 4;        /* (a bump takes the way off a ship) */
}

/* the player against the home carrier, on its actual shape (the user: in any setting a capital ship uses the actual
 * map; so the gap under the Vigilance's deck amidships can be flown through): cleared, the player has landed (it would
 * have at LANDING_NEAR_KM: land_near), and the flight ends at the loop's next check; not cleared, the fighter flying
 * into it stops where it was, unhurt, and can fly off again.  (Until 2026-11-04 only its bow took a cleared fighter,
 * the user's rule of 2026-10-31; the user then: "lets get rid of all the bow logic and you can land just by distance
 * to carrier after requesting...hope that saves some code and stops some user frustration") */
static void ram(int16_t a, int16_t b);

/* (the user, 2026-11-05: "check that you don't have afterburner on when landing... if you have afterburner on when
 * attempting landing it should cause normal collision damage": cleared, but burning, the hull is any capital ship's,
 * ram's, and the carrier says why: g_stopped 3) */
static void carrier_contact(int16_t home)
{
    RvVec3 *v = &gVelocityVector[0];
    LfVec push;
    int32_t depth;
    if (!collide_ships(0, home, &push, &depth))
        return;
    if (g_cleared && rvAfterburner[0]) {
        g_stopped = 3;
        ram(0, home);
        return;
    }
    if (g_cleared) {
        landedOn = (uint16_t)home;      /* (the engine's cue: the landing seen from outside, Kit at the plate) */
        missionStatus = 1;
        return;
    }
    /* (flying into it, not off it: toward its middle; km and km per step, so the products fit) */
    if (TO_KM(gPositionVector[home].x - gPositionVector[0].x) * (v->x / 16)
        + TO_KM(gPositionVector[home].y - gPositionVector[0].y) * (v->y / 16)
        + TO_KM(gPositionVector[home].z - gPositionVector[0].z) * (v->z / 16) > 0) {
        gPositionVector[0].x -= v->x;   /* (the step that took it there undone) */
        gPositionVector[0].y -= v->y;
        gPositionVector[0].z -= v->z;
        *v = vec3(0, 0, 0);
        setSpeed[0] = speedLo[0] = 0;
        g_stopped = 2;
    }
}

/* cleared, the player lands as it comes within LANDING_NEAR_KM of the carrier's hull, from any side (carrier_contact);
 * not with its afterburner lit (the carrier says so: g_stopped 3), which comes in as a ram */
#define LANDING_NEAR_KM 2000
static void land_near(void)
{
    int16_t s;
    if (!g_cleared || missionStatus || !slot_is_ship(0) || shipAiState[0] == 9 || rvController[0] != CTL_PLAYER)
        return;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (logic_is_home(s) && gap_between(0, s) <= LANDING_NEAR_KM) {
            if (rvAfterburner[0]) {
                g_stopped = 3;
                return;
            }
            landedOn = (uint16_t)s;
            missionStatus = 1;
            return;
        }
}

/* a collision's damage to each ship: the old game's rule (read in its code, 2026-10-31; the user: sensible), the
 * square of the two ships' closing speed in km a step, halved (two fighters head on at 400 kps close at 53 km a
 * step: 1,400 points, death; a 100-kps brush, 18; a touch at rest, nothing), or the soft rule the mission may ask
 * for (ram=soft: a touch's RAM_BASE and a point per RAM_PER of closing) */
static uint16_t ram_damage(int16_t a, int16_t b)
{
    int32_t closing = len_of(gVelocityVector[a].x - gVelocityVector[b].x, gVelocityVector[a].y - gVelocityVector[b].y,
                             gVelocityVector[a].z - gVelocityVector[b].z);
    if (g_rule_flags & RV_RULE_RAM_SOFT)
        return (uint16_t)(RAM_BASE + closing / RAM_PER);
    closing = TO_KM(closing);
    if (closing > 360)                  /* (the square fits a word: 64,800) */
        closing = 360;
    return (uint16_t)(closing * closing / 2);
}

static void hit_sides(int16_t dst, const RvVec3 *w, int16_t *sh, int16_t *ar);

/* what a collision may cost (the user, 2026-11-02: "collisions are too punishing ... This will prevent massive
 * frustration and still make collisions punishing and memorable"), never more than the old game's (ram_damage).  The
 * player, on the side the other ship is: with that shield at a tenth or less, the whole of it; with more, at most that
 * shield and that armour, or, that armour gone, that shield and the hull down to 40% of what it is (and when that cut
 * the old game's, a system point besides: ram_hurt) */
static uint16_t ram_cap_player_at(uint16_t qty, const RvVec3 *where)
{
    const RvShipType __far *t = slot_type(0);
    int16_t sh, ar;
    uint32_t cap;
    if (!t)
        return qty;
    hit_sides(0, where, &sh, &ar);
    if ((uint32_t)SHIELD(0, sh) * 10 <= SHIELD_MAX(0, sh))
        return qty;
    cap = (uint32_t)SHIELD(0, sh) + ARMOR(0, ar);
    if (!ARMOR(0, ar)) {
        uint16_t room = damagePoints[0] < t->hull ? (uint16_t)(t->hull - damagePoints[0]) : 0;
        cap += room - (uint32_t)room * 2 / 5;
    }
    return cap < qty ? (uint16_t)cap : qty;
}

static uint16_t ram_cap_player(uint16_t qty, int16_t other)
{
    return ram_cap_player_at(qty, &gPositionVector[other]);
}

uint16_t logic_rock_harm(int32_t closing, const RvVec3 *where)
{
    uint16_t qty;
    if (g_rule_flags & RV_RULE_RAM_SOFT)
        qty = (uint16_t)(RAM_BASE + closing / RAM_PER);
    else {
        int32_t km = TO_KM(closing);
        if (km > 360)
            km = 360;
        qty = (uint16_t)(km * km / 2 + 1);
    }
    return ram_cap_player_at(qty, where);
}

/* an NPC, any (a wingman, an escort, an enemy, a capital ship), on the side the other ship is: at most that shield and
 * half of what is left of that armour (the user: "only ... enough to remove the shield and half the remaining armor
 * on that face, if any", "that can be all NPCs to avoid tragedy"): a bump never kills one */
static uint16_t ram_cap_npc(int16_t s, uint16_t qty, int16_t other)
{
    int16_t sh, ar;
    uint32_t cap;
    hit_sides(s, &gPositionVector[other], &sh, &ar);
    cap = (uint32_t)SHIELD(s, sh) + ARMOR(s, ar) / 2;
    return cap < qty ? (uint16_t)cap : qty;
}

#define RAM_STOP_KPS 100                /* a ship flying into a capital ship faster than this stops dead */

/* s ran into a capital ship: faster than RAM_STOP_KPS it stops dead, its throttle at 0 too (the user, 2026-11-02:
 * "you can creep away but if you're fighting big time and smack into a capship you get stopped which can make you a
 * sitting duck"); slower, the bump's quartered speed (ram_shove) and away it creeps.  kps: its speed before the bump */
static void ram_stop(int16_t s, uint32_t kps)
{
    if (kps <= RAM_STOP_KPS || !ram_moves(s) || maybeEntityType[s] == ET_CAPSHIP)
        return;
    gVelocityVector[s] = vec3(0, 0, 0);
    setSpeed[s] = speedLo[s] = 0;
}

/* dst hurt by its collision with src (src's place the hit's); the player, when its cap cut the old game's damage (qty
 * under raw), besides loses a system a point on that side unless the hit broke one (damage_ram_point; the user: the
 * point is "only if the original formula would do more": a bump the old rule made light stays as it was) */
static void ram_hurt(int16_t src, int16_t dst, uint16_t qty, uint16_t raw)
{
    uint8_t before = damage_points_so_far();
    int16_t sh, ar;
    (void)do_damage((uint16_t)src, (uint16_t)dst, qty, RV_WHERE(&gPositionVector[src]));
    if (qty < raw && rvController[dst] == CTL_PLAYER && playerCanBeDamaged && slot_is_ship(0)
        && shipAiState[0] != 9) {       /* (the local player: slot 0; see ram's note on asking a == 0) */
        hit_sides(0, &gPositionVector[src], &sh, &ar);
        damage_ram_point(before, ar);
    }
}

/* a and b touch coarsely: the fine test, then the push that parts them (the lighter, by its box, gives way the more;
 * one that cannot move leaves it all to the other) and the hurt, each with the other as the source and the other's
 * place as the hit's (the side facing it); a pair touching on hurts every RAM_STEPS.  What each takes: the player its
 * cap (ram_cap_player), and an enemy that hit it as much as the player took, a friend its own cap; two NPCs each its
 * own cap (ram_cap_npc) */
static void ram(int16_t a, int16_t b)
{
    LfVec push;
    int32_t depth, ha, hb;
    int16_t ma, mb, pa, pb;
    uint16_t qty, qa, qb;
    uint32_t ka, kb;
    int16_t player;
    /* a pair is this machine's when its own pilot is in it, or when no ship in it is flown on another machine
     * (CTL_PUPPET, a shared flight's): every machine works out its own pilot's collisions, both shares (the user,
     * 2026-11-02: "the damage is worked out by the player that collided"; the layer takes what it does to another
     * machine's ship there, and two humans' bump to nothing), and the host its NPCs' */
    if (rvController[a] != CTL_PLAYER && rvController[b] != CTL_PLAYER
        && (rvController[a] == CTL_PUPPET || rvController[b] == CTL_PUPPET))
        return;
    if (!collide_ships(a, b, &push, &depth))
        return;
    push.x += push.x / 2;               /* (half as far again as touching, so the next step is not inside again) */
    push.y += push.y / 2;
    push.z += push.z / 2;
    ma = ram_moves(a);
    mb = ram_moves(b);
    ha = collide_half(a);
    hb = collide_half(b);
    if (ma && mb) {                     /* push is b's move: shared in the other's weight */
        pb = (int16_t)(ha * 256 / (ha + hb));
        pa = (int16_t)(256 - pb);
    } else {
        pa = ma ? 256 : 0;
        pb = mb ? 256 : 0;
    }
    qty = ram_damage(a, b);
    ka = speedLo[a];
    kb = speedLo[b];
    if (pb)
        ram_shove(b, &push, pb, -1);
    if (pa)
        ram_shove(a, &push, (int16_t)-pa, 1);
    /* (after the push: the sides do_damage will find.  b is never slot 0: a < b.  The local player is the slot that
     * is CTL_PLAYER, slot 0: asked of rvController[a], not of a == 0, which Watcom -ox took as never true here (it
     * put a constant 0 for `a == 0 && ...` and dropped the player's arm, as it once dropped an `if (s == 0)` arm in
     * the physics): on DOS the player took an NPC's cap.  docs/logic.md) */
    player = (int16_t)(rvController[a] == CTL_PLAYER);
    qa = player ? ram_cap_player(qty, b) : ram_cap_npc(a, qty, b);
    qb = player && commAnimInfo[b] != commAnimInfo[0] ? qa : ram_cap_npc(b, qty, a);
    if (maybeEntityType[b] == ET_CAPSHIP)
        ram_stop(a, ka);
    if (maybeEntityType[a] == ET_CAPSHIP)
        ram_stop(b, kb);
    if (rvController[a] == CTL_AI)      /* (a bump: the minds break away, each its own way) */
        bump_away(a, 1);
    if (rvController[b] == CTL_AI)
        bump_away(b, -1);
    if ((uint16_t)(frameCounter - g_rammed[a]) >= RAM_STEPS) {
        g_rammed[a] = frameCounter;
        ram_hurt(b, a, qa, qty);
    }
    if ((uint16_t)(frameCounter - g_rammed[b]) >= RAM_STEPS) {
        g_rammed[b] = frameCounter;
        ram_hurt(a, b, qb, qty);
    }
}

/* ====================================================================================================================
 * Missiles (M7): a launcher's round is a ship slot of ET_MISSILE that flies, homes and bursts on what it meets
 * ==================================================================================================================== */

#define MISSILE_HALF_KM 1               /* its half box (gMaybeShipRadius), km */
/* a seeker's turn rate by its lock (RV_LOCK_*: none, heat, image, auto, proximity), degrees a second; a torpedo's
 * own.  (weapons.csv has no column for it yet) */
static const uint8_t __far k_seeker_dps[5] = { 0, 70, 60, 90, 0 };
#define TORPEDO_DPS 25

static int16_t missile_dps(const RvWeapon __far *w)
{
    return w->kind == RV_WK_TORPEDO ? TORPEDO_DPS : w->lock < 5 ? k_seeker_dps[w->lock] : 0;
}

static int16_t nearest_enemy(int16_t s, int16_t capital_only);

/* whom a missile launched by `ship` homes on: a dumbfire nobody; the player's the cockpit's lock (a friend-or-foe
 * the target, else the nearest enemy); an AI's its own target; a puppet's what its own machine published
 * (missileTarget[ship]: fire_launcher) */
static uint8_t missile_target_of(int16_t ship, const RvWeapon __far *w)
{
    int16_t t;
    if (w->lock == RV_LOCK_NONE || w->lock == RV_LOCK_PROXIMITY)
        return 0xFF;
    if (rvController[ship] == CTL_PUPPET)
        return missileTarget[ship];
    if (rvController[ship] != CTL_PLAYER)
        return rvTarget[ship];
    if (g_sys.target >= 0 && slot_is_ship(g_sys.target) && (g_sys.lock || w->lock == RV_LOCK_AUTO))
        return (uint8_t)g_sys.target;
    if (w->lock == RV_LOCK_AUTO && (t = nearest_enemy(ship, 0)) >= 0)
        return (uint8_t)t;
    return 0xFF;
}

/* whom a launch by `ship` is for (fire_launcher publishes it): a seeker's homing target; a dumbfire's, the target it
 * may be pointed at (dumbfire_aim): the player's chosen one, an AI's own, a puppet's as its machine published it */
static uint8_t launch_target(int16_t ship, const RvWeapon __far *w)
{
    if (w->kind != RV_WK_MISSILE || w->lock != RV_LOCK_NONE)
        return missile_target_of(ship, w);
    if (rvController[ship] == CTL_PUPPET)
        return missileTarget[ship];
    if (rvController[ship] == CTL_PLAYER)
        return (uint8_t)(g_sys.target >= 0 ? g_sys.target : 0xFF);
    return rvTarget[ship];
}

#define PLAYER_LOCK_CONE 230            /* the player's seekers lock with the target within this: some 26 degrees */

/* the player's missile lock (the user's ask, 2026-10-31: a Top Gun lock display; a heat seeker from behind only,
 * friend-or-foe and dumbfire pointed at the target, one seeking the nearest, the other straight): the chosen
 * launcher's seeker against the target: a heat seeker only from the target's rear arc (its engines toward the
 * player, or its afterburner lit) with the EMCON not off; image recognition with the radar on; friend-or-foe with
 * the radar on and the IFF; each needs the target within PLAYER_LOCK_CONE and the weapon's range for its lock time
 * (weapons.csv lock_s), and is lost out of them; a dumbfire never locks; none with the tracking wrecked.
 * g_sys.lock, lock_pct and lock_kind say where it stands (the HUD's reticle, the lock lamp, the launch) */
static void lock_tick(void)
{
    int16_t l = launcher_of(0), t = g_sys.target, ok = 0;
    const RvWeapon __far *w = l >= 0 ? weapon(GUN_TYPE(0, l)) : 0;
    int16_t factor = damage_lock_factor();   /* (the tracking and the computer hurt: a lock takes longer; wrecked, never) */
    uint16_t need = (w ? (uint16_t)(w->lock_tenths * 3 + 1) : 1) * (uint16_t)(factor ? factor : 1);
    g_sys.lock_kind = (uint8_t)(w ? w->lock : RV_LOCK_NONE);
    if (w && w->lock != RV_LOCK_NONE && w->lock != RV_LOCK_PROXIMITY && t > 0 && slot_is_ship(t) && shipAiState[t] != 9
        && factor && cos_nose_to(0, t) >= PLAYER_LOCK_CONE && gap_between(0, t) <= w->range_km) {
        if (w->lock == RV_LOCK_HEAT) {
            RvVec3 d = vec3(TO_KM(gPositionVector[t].x - gPositionVector[0].x), TO_KM(gPositionVector[t].y - gPositionVector[0].y),
                            TO_KM(gPositionVector[t].z - gPositionVector[0].z));
            ok = g_sys.emcon != EMCON_OFF && (vec_dot(&d, &gOrientationFrontVector[t]) > 0 || rvAfterburner[t]);
        } else if (w->lock == RV_LOCK_IMAGE)
            ok = g_sys.emcon == EMCON_ACTIVE;
        else
            ok = g_sys.emcon == EMCON_ACTIVE && g_sys.iff && sys_level(SYS_IFF) < SYS_WRECKED;
    }
    if (ok) {
        if (g_lock_steps < need)
            g_lock_steps++;
    } else
        g_lock_steps = 0;
    g_sys.lock = (uint8_t)(ok && g_lock_steps >= need);
    g_sys.lock_pct = (uint8_t)(ok ? (int32_t)g_lock_steps * 100 / need : 0);
}

/* (ship, gun, its weapon): a launcher fires: a round fewer, and the missile into a free ship slot at the mount,
 * facing the ship's way at its weapon's speed, its parent the ship, its target by the lock.  The slot (the layer
 * maps it), 0xFFFF when every slot is taken (the round is spent and nothing flies) */
#define DUMBFIRE_CONE 247               /* a dumbfire sets off toward its shooter's target when the target's lead point
                                           lies within this of the nose (cos x 256: some 15 degrees) */

/* a dumbfire's help (the user, 2026-11-02: "the dumbfires tilt towards the target to help aim since you only get one
 * shot"): its shooter's target (the player's chosen one, an AI's rvTarget) alive and its lead point (where it will be
 * when the missile gets there, at the missile's speed: twice refined) within DUMBFIRE_CONE of the nose, the missile
 * sets off toward that point; then it flies straight, as a dumbfire does */
static void dumbfire_aim(int16_t s, int16_t ship, const RvWeapon __far *w)
{
    uint8_t aim = launch_target(ship, w);
    int16_t t = aim == 0xFF ? -1 : aim;
    int32_t tof, len, km_step = w->speed_kps / KPS_HZ + 1;
    int16_t i, yaw, pitch;
    RvVec3 p, vt, d, b;
    if (t < 0 || t == ship || !slot_is_ship(t) || shipAiState[t] == 9)
        return;
    p = vec3(TO_KM(gPositionVector[t].x - gPositionVector[s].x), TO_KM(gPositionVector[t].y - gPositionVector[s].y),
             TO_KM(gPositionVector[t].z - gPositionVector[s].z));
    vt = vec3(TO_KM(gVelocityVector[t].x), TO_KM(gVelocityVector[t].y), TO_KM(gVelocityVector[t].z));
    d = p;
    for (i = 0; i < 2; i++) {
        tof = len_of(d.x, d.y, d.z) / km_step;
        d = vec3(p.x + vt.x * tof, p.y + vt.y * tof, p.z + vt.z * tof);
    }
    to_body(s, &d, &b);
    len = len_of(b.x, b.y, b.z);
    if (b.z <= 0 || b.z * 256 < (int32_t)DUMBFIRE_CONE * len)
        return;
    yaw = angle_of(b.x, b.z);
    pitch = angle_of(b.y, len_of(b.x, 0, b.z));
    if (yaw)
        vec_turn(&gOrientationFrontVector[s], &gOrientationRightVector[s], yaw);
    if (pitch)
        vec_turn(&gOrientationFrontVector[s], &gOrientationUpVector[s], pitch);
    if (yaw || pitch)
        ship_orthonormalise(s);
}

static uint16_t missile_launch(uint16_t ship, uint16_t gun, const RvWeapon __far *w)
{
    int16_t s = ship_free_slot(), k;
    uint8_t rounds = (uint8_t)(GUN_STATE(ship, gun) & 0x7F);
    RvVec3 at;
    if (rounds)
        GUN_STATE(ship, gun) = (uint8_t)((GUN_STATE(ship, gun) & GUN_DESTROYED) | (rounds - 1));
    if (s < 0)
        return 0xFFFF;
    gun_mount((int16_t)ship, (int16_t)gun, &at);
    maybeEntityType[s] = ET_MISSILE;
    gInstanceShipTypes[s] = GUN_TYPE(ship, gun);   /* (its weapon: the drawing and the effects read it; no ship type) */
    commAnimInfo[s] = commAnimInfo[ship];
    rvController[s] = CTL_AI;
    parentShipForShip[s] = (uint8_t)ship;
    shipStateByte[s] = 0xFF;
    slotMissionShip[s] = 0xFFFF;
    gPositionVector[s] = at;
    gOrientationFrontVector[s] = gOrientationFrontVector[ship];
    gOrientationRightVector[s] = gOrientationRightVector[ship];
    gOrientationUpVector[s] = gOrientationUpVector[ship];
    if (w->kind == RV_WK_MISSILE && w->lock == RV_LOCK_NONE)
        dumbfire_aim(s, (int16_t)ship, w);
    setSpeed[s] = speedLo[s] = w->speed_kps;
    fly_ahead(s);
    for (k = 0; k < 2; k++)
        SHIELD(s, k) = SHIELD_MAX(s, k) = 0;
    for (k = 0; k < 4; k++)
        ARMOR(s, k) = 0;
    damagePoints[s] = 0;
    coreHp[s] = 0;
    gunEnergy[s] = 100;                 /* (full: the old game's ships fire from the first second) */
    gunDamage[s] = 0;
    engineFlag[s] = 0;
    GUN_COUNT(s) = 0;
    shipAiState[s] = 0;
    gMaybeShipRadius[s] = MISSILE_HALF_KM;
    damageProperty[s] = w->damage;
    rvFlags[s] = 0;
    rvAfterburner[s] = 0;
    logic_prepare_spawn(s);
    missileTarget[s] = rvTarget[s] = missile_target_of((int16_t)ship, w);
    g_missile[s].weapon = GUN_TYPE(ship, gun);
    g_missile[s].life = (uint16_t)((int32_t)w->range_km * KPS_HZ / (w->speed_kps ? w->speed_kps : 1));
    return (uint16_t)s;
}

/* a seeker's nose toward its target's lead point (where the target will be when it gets there), by at most its rate */
static void missile_seek(int16_t s, int16_t t, int16_t dps)
{
    RvVec3 d, b;
    int32_t tof, horiz;
    int16_t yaw, pitch, most;
    d = vec3(TO_KM(gPositionVector[t].x - gPositionVector[s].x), TO_KM(gPositionVector[t].y - gPositionVector[s].y),
             TO_KM(gPositionVector[t].z - gPositionVector[s].z));
    tof = len_of(d.x, d.y, d.z) / (speedLo[s] / KPS_HZ + 1);   /* steps to it (km a step: kps / KPS_HZ) */
    d.x += TO_KM(gVelocityVector[t].x) * tof;
    d.y += TO_KM(gVelocityVector[t].y) * tof;
    d.z += TO_KM(gVelocityVector[t].z) * tof;
    to_body(s, &d, &b);
    horiz = len_of(b.x, 0, b.z);
    yaw = angle_of(b.x, b.z);
    pitch = angle_of(b.y, horiz);
    most = turn_units(s, 0, TURN_STEP(dps, 9));   /* (its rate's units this step, the fraction carried) */
    yaw = (int16_t)clamp32(yaw, -most, most);
    most = turn_units(s, 1, TURN_STEP(dps, 9));
    pitch = (int16_t)clamp32(pitch, -most, most);
    if (yaw)
        vec_turn(&gOrientationFrontVector[s], &gOrientationRightVector[s], yaw);
    if (pitch)
        vec_turn(&gOrientationFrontVector[s], &gOrientationUpVector[s], pitch);
    if (yaw || pitch)
        ship_orthonormalise(s);
}

/* a missile's step: its life down (gone at its range), its seeking, its move, and its hit: its step through a ship's
 * hull (any ship but its launcher), where it bursts (do_damage with the missile as the source) and its slot ends */
static void missile_step(int16_t s)
{
    const RvWeapon __far *w;
    RvVec3 last, hit;
    LfVec lh;
    int32_t reach;
    int16_t t, dps, k;
    if (rvController[s] == CTL_PUPPET) {   /* (the layer flies it) */
        gPositionVector[s].x += gVelocityVector[s].x;
        gPositionVector[s].y += gVelocityVector[s].y;
        gPositionVector[s].z += gVelocityVector[s].z;
        return;
    }
    w = weapon(g_missile[s].weapon);
    if (!w || !g_missile[s].life) {
        ship_clear(s);
        return;
    }
    g_missile[s].life--;
    t = missileTarget[s];
    dps = missile_dps(w);
    if (dps && t != 0xFF && slot_is_ship(t) && shipAiState[t] != 9)
        missile_seek(s, t, dps);
    last = gPositionVector[s];
    fly_ahead(s);
    gPositionVector[s].x += gVelocityVector[s].x;
    gPositionVector[s].y += gVelocityVector[s].y;
    gPositionVector[s].z += gVelocityVector[s].z;
    reach = KPS_STEP(speedLo[s]);
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        int32_t h;
        if (!slot_is_ship(k) || k == parentShipForShip[s] || shipAiState[k] == 9)
            continue;
        h = collide_half(k) + reach;    /* (its step's box against the ship's: nothing near, nothing more) */
        if (labs(gPositionVector[s].x - gPositionVector[k].x) > h || labs(gPositionVector[s].y - gPositionVector[k].y) > h
            || labs(gPositionVector[s].z - gPositionVector[k].z) > h)
            continue;
        if (collide_segment(k, (const LfVec *)&last, (const LfVec *)&gPositionVector[s], &lh, 0)) {
            hit = vec3(lh.x, lh.y, lh.z);
            (void)do_damage((uint16_t)s, (uint16_t)k, w->damage, RV_WHERE(&hit));
            ship_clear(s);
            return;
        }
    }
}

/* ====================================================================================================================
 * The AI (M5): minds for the ships of CTL_AI, through aiFighterThink and aiShipThink (docs/ai.md)
 *
 * A mind runs a TACTIC (T_*: a maneuver with phases) picked from its PROFILE's table by the SITUATION it is in
 * (SIT_*: the target far, near ahead, on its tail, just passed, itself hurt, nobody about), with weighted dice from
 * game_rand.  The tactic decides where the nose should point (a direction in the ship's own frame), the throttle
 * (aiSetSpeed), the afterburner and whether to fire; the STEERING turns that direction into the steer and roll the
 * player's controls give (g_turn), within the profile's PULLS (which ways it may pull the nose: a ship that cannot
 * turn left rolls until the target is above or below it and pulls there) and its style (bank then pull, as the
 * fighters of the early nineties turned, or yaw and pitch at once).  The flight model (step_motion) flies it like
 * any ship, within its type's rates and speeds.  Every number is a table entry: the profiles are measured against
 * the old games' flights (docs/ai.md), and a new behaviour is a new row.
 * ==================================================================================================================== */

enum { T_IDLE, T_CRUISE, T_FORMATION, T_ESCORT, T_WAIT,         /* nobody to fight (T_WAIT: still, until a chance) */
       T_PURSUE, T_APPROACH, T_CLOSE,                           /* attacks (T_APPROACH: the old game's silent approach to a
                                                                   point behind the target; T_CLOSE: its choice, the
                                                                   pursuit unless the target faces it, then the approach) */
       T_HEADON, T_STRAFE, T_SIT, T_RAM, T_TORPEDO,
       T_BREAK, T_EVADE, T_LOOP, T_FLEE, T_HOME,                /* getting away (T_HOME: to its side's capital ship) */
       T_COUNT };
enum { SIT_IDLE, SIT_FAR, SIT_NEAR, SIT_TAILED, SIT_PASSED, SIT_HURT, SIT_COUNT };
enum { PULL_RIGHT = 1, PULL_LEFT = 2, PULL_UP = 4, PULL_DOWN = 8, PULL_ALL = 15 };
enum { P_INTERCEPTOR, P_FIGHTER, P_HEAVY, P_ACE, P_BOMBER, P_WINGMAN, P_CAPITAL, P_WAITER, P_NOLEFT, P_COUNT };

#define PICKS 4
typedef struct { uint8_t tactic, weight; } Pick;
typedef struct {
    uint16_t engage_km;                 /* an enemy this near is fought */
    uint16_t fire_km;                   /* the guns fire within this (and their own range) */
    uint16_t break_km;                  /* a closing attacker breaks off this near (0: never) */
    uint16_t out_km;                    /* a flat break: after its first yaw, straight away until the target is this far
                                           (or BREAK_OUT_STEPS), then the yaw back (0: the yaw back at once) */
    uint16_t pounce_km;                 /* T_WAIT: a target this near is a chance */
    uint8_t aim_cos;                    /* the fire cone: the lead point within it (cos x 256) */
    uint8_t pulls;                      /* PULL_*: which ways the steering may pull the nose */
    uint8_t bank;                       /* 1: roll toward the target, then pull (near the nose, a yaw and a pitch at
                                           once: the fine aim); 0: yaw and pitch at once (with every pull), or the one
                                           pull always (without) */
    uint8_t ab_pct;                     /* percent of its closing and fleeing on the afterburner */
    uint8_t scared_pct;                 /* its fear climbs freely with its health (armour and hull) under this percent
                                           (the old game's 40); 0: it is never scared (the fear model, below) */
    uint8_t missile_pct;                /* percent of its locks that end in a launch */
    uint8_t wait_s;                     /* T_WAIT: the most it waits for a chance */
    uint8_t idle;                       /* its tactic with nobody to fight */
    uint16_t pass_pct;                  /* a pass goes by the target this percent of the two hulls' half boxes apart */
    uint16_t avoid_pct;                 /* another ship ahead within this percent of the two half boxes is steered round
                                           (when coming together at a quarter of the top speed or more) */
    uint8_t turn_pct;                   /* the AI uses this percent of its type's turn rate at most */
    uint8_t burst;                      /* steps of afterburner at the start of every break (the old games' dash, 6 of its
                                           frames; 0 none) */
    uint8_t lead_pct;                   /* how much of the lead it aims off (100: the shots meet the target; 0: it aims at
                                           the target itself and its passes come of the target's own motion, as the old
                                           game's fighters shot at 0 degrees off) */
    uint8_t tail_pct;                   /* behind its target within the guns' range it flies this percent of its top
                                           speed (the old game's fighters tailed at some 60%) */
    uint8_t fire_gap;                   /* steps between its volleys with the target in the cone (the old game's
                                           fighters: a volley every 15 frames, 1 s at its 15 a second, not every frame
                                           their guns were cool) */
    uint8_t fear_up, fear_down;         /* the fear model's c and d (the old game's class bytes: c 2-5, d 6-10) */
    uint8_t flags;                      /* PF_* */
    Pick pick[SIT_COUNT][PICKS];        /* the dice by situation (SIT_IDLE's row is unused: idle says) */
} AiProfile;
enum { PF_NO_MERCY = 1 };               /* it fires at a player that has it selected (the old game's class 11) */

/* The fear model (the old game's, read in its code 2026-10-31; tests/wc1/README.md): a ship's fear moves at every
 * tactic pick by its situation (a missile homing on it +2c, tailed +c, a tail chase -c, its target dying halves
 * it, the plain case -d) and then by its health, the armour and hull left in a hundred: under the profile's
 * scared_pct (40) +2c and no cap, under HEALTH_BOLD (75) +c and capped at FEAR_CAP_WARY (28), else capped at
 * FEAR_CAP_BOLD (7); a ship hit in the last steps (g_under_fire) is in the old game's "hit" case: nothing calms it.  Fear of FEAR_WARY (15) picks from the cautious row (SIT_HURT's), FEAR_LEAVE (30) is the
 * leave: with a living capital ship of its side about, a run to it (T_HOME) and its fear reset, as the old game
 * zeroed the fear of a ship with its carrier about; else away for good (leave_step: straight up or down, the
 * afterburner while an enemy is within LEAVE_KM, removed beyond LEAVE_KM of every human's ship, not killed).  A
 * mission ship's own rates (missions.txt fear=c/d) stand in for the profile's */
#define FEAR_WARY 15
#define FEAR_LEAVE 30
#define FEAR_CAP_WARY 28
#define FEAR_CAP_BOLD 7
#define HEALTH_BOLD 75
#define LEAVE_KM 16000                  /* (the old game's removal distance) */
#define HOLD_COS 205                    /* the held fire: the player within 37 degrees ahead of its nose (cos x 256) */
#define AI_REFIRE_STEPS (13 * WC_FRAME_STEPS)   /* the cone rule: an AI's guns no oftener than this (the old game's 13 frames) */

#define BREAK_OUT_STEPS 72              /* a flat break's straight leg at most (2.4 s: a target that chases keeps near; the
                                           old game's 1.8 s as measured at 20 frames a second, at its 15) */
#define AIM_CONE 250                    /* cos x 256 of the usual fire cone: some 7 degrees */
#define LOCK_CONE 230                   /* a missile locks with the target within this: some 26 degrees */
#define TAIL_KM 3500                    /* an attacker behind, this near, is on my tail */
/* An attack's speed, as the old game's fighters flew it (its first mission traced at 15 frames a second, 2026-11-02:
 * tests/wc1/README.md): at its attack speed (the profile's tail_pct of its top: the old ship table's +16, the Dralthi's
 * 230) once near, head on or behind, firing while the guns reach; behind a target faster than that it falls back, out
 * of reach, and only past CATCH_FAR_KM does it close again at full speed, to CATCH_NEAR_KM (a target facing it: into
 * its guns' reach, the profile's fire_km; the old fighters slowed at some 5,500 km, their reach).  The old tail fired a third of the time so, where one held at the guns' reach fired all the time */
#define CATCH_FAR_KM 8000
#define CATCH_NEAR_KM 2500
#define RELAUNCH_STEPS (4 * LOGIC_HZ)   /* missiles no closer together than this */

static const AiProfile __far k_profiles[P_COUNT] = {
    /* P_INTERCEPTOR: light and fast: head-on passes, flat turns round, runs when hurt */
    { 25000, 3000, 900, 2500, 0, AIM_CONE, PULL_RIGHT | PULL_LEFT, 1, 40, 40, 50, 0, T_CRUISE, 170, 300, 100, 13, 0, 60, 29, 3, 6, 0,
      { { { 0, 0 } }, { { T_HEADON, 60 }, { T_PURSUE, 40 } }, { { T_PURSUE, 60 }, { T_STRAFE, 40 } },
        { { T_PURSUE, 50 }, { T_BREAK, 30 }, { T_EVADE, 20 } }, { { T_PURSUE, 60 }, { T_BREAK, 40 } },
        { { T_FLEE, 60 }, { T_PURSUE, 40 } } } },
    /* P_FIGHTER: the line fighter, as the old game's Dralthi measured (tests/wc1/README.md): straight at the
     * target at full speed, a hard flat yaw away at 900 km (its passes 250-900 km), round again in a second or two
     * with a dash of afterburner at every pass and none on the way in (ab_pct 0: the old one closed at its top speed,
     * 2026-11-02), a tail at 55% of its speed (437 km/s: under a player's 300 setting, as
     * the old Dralthi's 466 was under the old player's 600, so it falls back rather than creeps up and bumps) firing
     * from up to 4,000 km at the lead point (its nose sat 4.8 degrees off a crossing player's position: the lead
     * angle; with no lead its tail never hit a player flying straight, the old's did 15% of the time), a volley every
     * 0.75 s, keeps attacking
     * when hunted; it yaws (rolling to bring the target level) and does not pitch.  Its rows are the old game's
     * (read in its code, 2026-10-31; the user: keep them as they were, it is fun): far off and in the plain case the
     * silent approach (its maneuver 40) 60% of the time, else its choice (45: the nose on the target with the guns
     * unless the target faces it, then the approach); near, the choice mostly, sometimes the guns at once (29) */
    { 25000, 4000, 900, 2500, 0, AIM_CONE, PULL_RIGHT | PULL_LEFT, 1, 0, 40, 50, 0, T_CRUISE, 170, 350, 100, 13, 100, 55, 29, 3, 7, 0,
      { { { 0, 0 } }, { { T_APPROACH, 60 }, { T_CLOSE, 40 } }, { { T_CLOSE, 50 }, { T_PURSUE, 30 }, { T_APPROACH, 20 } },
        { { T_PURSUE, 50 }, { T_BREAK, 50 } }, { { T_CLOSE, 60 }, { T_BREAK, 40 } },
        { { T_PURSUE, 70 }, { T_FLEE, 30 } } } },
    /* P_HEAVY: slow and armoured: head on mostly, pulls rather than yaws, rarely runs */
    { 25000, 3000, 0, 0, 0, AIM_CONE, PULL_ALL, 1, 20, 40, 60, 0, T_CRUISE, 170, 200, 100, 13, 0, 70, 29, 3, 8, 0,
      { { { 0, 0 } }, { { T_HEADON, 80 }, { T_PURSUE, 20 } }, { { T_HEADON, 60 }, { T_PURSUE, 40 } },
        { { T_BREAK, 60 }, { T_EVADE, 40 } }, { { T_BREAK, 70 }, { T_LOOP, 30 } },
        { { T_PURSUE, 80 }, { T_FLEE, 20 } } } },
    /* P_ACE: strafing runs, hard evasion, never scared (scared_pct 0), uses the afterburner half the time, and fires at
     * a player that has it selected (PF_NO_MERCY: the old game's class 11) */
    { 30000, 3000, 0, 0, 0, AIM_CONE, PULL_ALL, 1, 50, 0, 70, 0, T_CRUISE, 150, 180, 100, 20, 50, 80, 20, 2, 9, PF_NO_MERCY,
      { { { 0, 0 } }, { { T_STRAFE, 50 }, { T_PURSUE, 50 } }, { { T_STRAFE, 50 }, { T_PURSUE, 50 } },
        { { T_EVADE, 60 }, { T_BREAK, 20 }, { T_LOOP, 20 } }, { { T_LOOP, 50 }, { T_BREAK, 50 } },
        { { T_PURSUE, 100 } } } },
    /* P_BOMBER: torpedo runs on capital ships (off at 1,500 km once the torpedo is away), else a heavy's fight */
    { 25000, 2800, 1500, 2500, 0, AIM_CONE, PULL_ALL, 1, 20, 40, 80, 0, T_CRUISE, 170, 200, 100, 13, 100, 70, 29, 2, 8, 0,
      { { { 0, 0 } }, { { T_TORPEDO, 70 }, { T_PURSUE, 30 } }, { { T_TORPEDO, 60 }, { T_PURSUE, 40 } },
        { { T_BREAK, 70 }, { T_EVADE, 30 } }, { { T_BREAK, 70 }, { T_LOOP, 30 } },
        { { T_FLEE, 60 }, { T_PURSUE, 40 } } } },
    /* P_WINGMAN: the player's side: keeps station, and fights what comes within 12,000 km as the line fighter does
     * (the old game's wingman flew its fighters' own AI: in the straight run it killed two of the three Dralthi and
     * came home; with its own pitching, fleeing row ours fired 6 shots in 69 s and died), but tailed it breaks 90
     * degrees with a dash or loops, as the old game's first named pilot's table has it (a break, else two half
     * loops; tests/wc1/README.md), and its fear falls faster (d 8) */
    { 12000, 4000, 900, 2500, 0, AIM_CONE, PULL_RIGHT | PULL_LEFT, 1, 30, 40, 50, 0, T_FORMATION, 170, 350, 100, 13, 100, 55, 29, 3, 8, 0,
      { { { 0, 0 } }, { { T_APPROACH, 60 }, { T_CLOSE, 40 } }, { { T_CLOSE, 50 }, { T_PURSUE, 30 }, { T_APPROACH, 20 } },
        { { T_BREAK, 50 }, { T_LOOP, 50 } }, { { T_CLOSE, 60 }, { T_BREAK, 40 } },
        { { T_PURSUE, 70 }, { T_FLEE, 30 } } } },
    /* P_CAPITAL: holds its course; its turrets and launchers are aiShipThink's */
    { 8000, 6000, 0, 0, 0, AIM_CONE, PULL_ALL, 0, 0, 0, 100, 0, T_CRUISE, 100, 100, 100, 0, 100, 100, 0, 0, 10, 0,
      { { { 0, 0 } }, { { T_CRUISE, 100 } }, { { T_CRUISE, 100 } }, { { T_CRUISE, 100 } }, { { T_CRUISE, 100 } },
        { { T_CRUISE, 100 } } } },
    /* P_WAITER: lies still (cloaked) until a target comes near unawares, then a fighter's fight on the afterburner */
    { 25000, 3000, 900, 2500, 5000, AIM_CONE, PULL_ALL, 0, 60, 40, 50, 30, T_WAIT, 170, 300, 100, 13, 0, 60, 29, 3, 6, 0,
      { { { 0, 0 } }, { { T_PURSUE, 70 }, { T_HEADON, 30 } }, { { T_PURSUE, 60 }, { T_STRAFE, 40 } },
        { { T_PURSUE, 50 }, { T_BREAK, 30 }, { T_EVADE, 20 } }, { { T_PURSUE, 60 }, { T_BREAK, 40 } },
        { { T_FLEE, 60 }, { T_PURSUE, 40 } } } },
    /* P_NOLEFT: a fighter whose controls cannot turn left: it rolls and pulls instead */
    { 25000, 3000, 900, 2500, 0, AIM_CONE, PULL_RIGHT | PULL_UP | PULL_DOWN, 0, 30, 40, 50, 0, T_CRUISE, 170, 300, 100, 13, 0, 60, 29, 3, 6, 0,
      { { { 0, 0 } }, { { T_HEADON, 60 }, { T_PURSUE, 40 } }, { { T_PURSUE, 70 }, { T_HEADON, 30 } },
        { { T_EVADE, 40 }, { T_LOOP, 40 }, { T_BREAK, 20 } }, { { T_LOOP, 50 }, { T_BREAK, 50 } },
        { { T_PURSUE, 60 }, { T_FLEE, 40 } } } },
};

static void mind_rates(int16_t s)
{
    const AiProfile __far *p = &k_profiles[g_mind[s].profile < P_COUNT ? g_mind[s].profile : P_FIGHTER];
    g_mind[s].fear_up = p->fear_up;
    g_mind[s].fear_down = p->fear_down;
}

/* a type's profile (data/ships.csv's roles; a mission ship's record may name another) */
static uint8_t profile_of(uint16_t type, uint16_t side)
{
    const RvShipType __far *t = ship_type(type);
    if (!t)
        return P_FIGHTER;
    if (t->type >= 0x20)
        return P_CAPITAL;
    if (side == SIDE_TERRAN)
        return P_WINGMAN;
    switch (type) {
    case 0x0B: case 0x10: case 0x15: return P_INTERCEPTOR;   /* Agis, Shizu, Gaozong */
    case 0x0A: case 0x12: return P_BOMBER;   /* Areus, Taizong */
    case 0x0C: case 0x11: return P_HEAVY;    /* Cleomenes, Zhuangzong */
    case 0x0D: return P_ACE;                 /* Convolution */
    default: return P_FIGHTER;
    }
}

/* the target as a mind sees it */
typedef struct {
    RvVec3 rel;                         /* right, up, ahead, km */
    RvVec3 aim;                         /* its lead point the same way: where the guns' shots meet it */
    int32_t km;
    int16_t cos_nose;                   /* how straight my nose points at it (x 256) */
    int16_t cos_aim;                    /* at its lead point */
    RvVec3 aim_fire;                    /* the lead point as the ship's own shots fly (its velocity carried): the nose
                                           goes there within the guns' reach (aim_at) */
    int16_t cos_fire;                   /* at that point */
    int32_t miss_km;                    /* how far the nose's line passes from that point */
    int16_t cos_its;                    /* how straight ITS nose points at me */
    int32_t closing;                    /* how fast the two come together, km a step (negative: apart) */
} View;

static int16_t cos_of(const RvVec3 *v, int32_t len)
{
    return (int16_t)(len ? clamp32(v->z * 256 / len, -256, 256) : 256);
}

/* s's view of t: where it is, where its lead point is for s's guns (the first gun's speed), who points at whom */
static void view_of(int16_t s, int16_t t, int16_t lead, View *v)
{
    RvVec3 d, w, vt;
    int16_t g = gun_of(s);
    const RvWeapon __far *gw = g >= 0 ? weapon(GUN_TYPE(s, g)) : 0;
    int32_t tof, kps = gw && gw->speed_kps ? gw->speed_kps : 2000, alen;
    ship_rel(s, t, &v->rel);
    v->km = len_of(v->rel.x, v->rel.y, v->rel.z);
    v->cos_nose = cos_of(&v->rel, v->km);
    /* the lead: the target's move over the shot's flight, twice refined.  The way to fly at (aim) takes the target's
     * own velocity, as the old game's fighters flew; the guns' test (cos_fire, miss_km) takes it less what a shot
     * carries of the ship's (its velocity along the nose: carried_of, gun_fire), since the shot must meet the target
     * as seen from the ship */
    vt = vec3(TO_KM(gVelocityVector[t].x), TO_KM(gVelocityVector[t].y), TO_KM(gVelocityVector[t].z));
    to_body(s, &vt, &w);
    tof = v->km / (kps / KPS_HZ);
    v->aim = vec3(v->rel.x + w.x * tof, v->rel.y + w.y * tof, v->rel.z + w.z * tof);
    tof = len_of(v->aim.x, v->aim.y, v->aim.z) / (kps / KPS_HZ);
    v->aim = vec3(v->rel.x + w.x * tof, v->rel.y + w.y * tof, v->rel.z + w.z * tof);
    v->aim = vec3(v->rel.x + (v->aim.x - v->rel.x) * lead / 100, v->rel.y + (v->aim.y - v->rel.y) * lead / 100,
                  v->rel.z + (v->aim.z - v->rel.z) * lead / 100);
    alen = len_of(v->aim.x, v->aim.y, v->aim.z);
    v->cos_aim = cos_of(&v->aim, alen);
    d = carried_of(s);
    vt = vec3(TO_KM(gVelocityVector[t].x - d.x), TO_KM(gVelocityVector[t].y - d.y), TO_KM(gVelocityVector[t].z - d.z));
    to_body(s, &vt, &w);
    tof = v->km / (kps / KPS_HZ);
    d = vec3(v->rel.x + w.x * tof, v->rel.y + w.y * tof, v->rel.z + w.z * tof);
    tof = len_of(d.x, d.y, d.z) / (kps / KPS_HZ);
    d = vec3(v->rel.x + (w.x * tof) * lead / 100, v->rel.y + (w.y * tof) * lead / 100, v->rel.z + (w.z * tof) * lead / 100);
    v->aim_fire = d;
    alen = len_of(d.x, d.y, d.z);
    v->cos_fire = cos_of(&d, alen);
    v->miss_km = alen * (int32_t)lf_sqrt((uint32_t)(INT32_C(65536) - (v->cos_fire < 0 ? 0 : (int32_t)v->cos_fire * v->cos_fire))) / 256;
    /* its nose on me: me from it, along its front */
    d = vec3(TO_KM(gPositionVector[s].x - gPositionVector[t].x), TO_KM(gPositionVector[s].y - gPositionVector[t].y),
             TO_KM(gPositionVector[s].z - gPositionVector[t].z));
    v->cos_its = (int16_t)(v->km ? clamp32(vec_dot(&d, &gOrientationFrontVector[t]) / v->km, -256, 256) : 0);
    /* closing: the relative velocity (positions a step) along the line from me to it, in km a step */
    w = vec3(TO_KM(gVelocityVector[s].x - gVelocityVector[t].x), TO_KM(gVelocityVector[s].y - gVelocityVector[t].y),
             TO_KM(gVelocityVector[s].z - gVelocityVector[t].z));
    v->closing = v->km ? -vec_dot(&w, &d) / v->km : 0;
    /* the distances an attack goes by (when to break off, when to fire) are hull to hull: at 900 km from its middle a
     * fighter attacking the carrier was within a few steps of its hull, and every one that came near flew into it (the
     * user's suspicion, measured 2026-10-31).  (Last: the cosines above want the middles' distance) */
    v->km = gap_of(v->km, s, t);
}

/* the nearest living ship of another side (not cloaked), or of the capital ships only; -1 none */
static int16_t nearest_enemy(int16_t s, int16_t capital_only)
{
    int16_t k, best = -1;
    int32_t bd = INT32_C(0x7FFFFFFF), d;
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        if (k == s || !slot_is_ship(k) || commAnimInfo[k] == commAnimInfo[s] || shipAiState[k] == 9 || (rvFlags[k] & 16))
            continue;
        if (capital_only && maybeEntityType[k] != ET_CAPSHIP)
            continue;
        d = km_between(s, k);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* the slot s keeps station on: its mission record's leader, else (on the player's side) the player; 0xFF none */
static uint8_t leader_of(int16_t s)
{
    int16_t k;
    if (slotMissionShip[s] < 64) {
        uint16_t lead = *(const uint16_t *)(MSHIP(slotMissionShip[s]) + 4);
        if (lead < 64)
            for (k = 0; k < NUM_SHIP_SLOTS; k++)
                if (k != s && slot_is_ship(k) && slotMissionShip[k] == lead)
                    return (uint8_t)k;
    }
    if (s != 0 && commAnimInfo[s] == commAnimInfo[0] && slot_is_ship(0))
        return 0;
    return 0xFF;
}

/* --- the steering: the nose toward a direction in the ship's own frame, as the profile allows --- */

#define FULL_AT 28                      /* a full steer this far off the way (1024 a turn: 10 degrees), less within */

static int8_t steer_of(int16_t angle)
{
    int16_t r = (int16_t)clamp32((int32_t)angle * 9 / FULL_AT, -9, 9);
    if (!r && angle)                    /* (the last degree too: with none under 3 units, 1.1 degrees, a tail's shots
                                           passed 45 km over a target 3,000 km ahead, every one) */
        r = (int16_t)(angle > 0 ? 1 : -1);
    return (int8_t)r;
}

/* (dx right, dy up, dz ahead): g_turn[s].  The straight way is a yaw and a pitch at once; a profile that banks, or
 * one whose pulls do not allow the straight way, rolls until the target lies along an allowed pull (right, left,
 * up or down of the nose) and pulls there.  A roll of +t (the right wing up) turns a bearing in the right-up plane
 * by -t: to bring a bearing b to the pull c, roll by b - c */
/* the two hulls' half boxes together, km: the unit the passes and the avoidance are measured in */
static int32_t room_of(int16_t a, int16_t b)
{
    return TO_KM(collide_half(a) + collide_half(b));
}

/* how far a ship flies sideways turning away at its speed: its turning circle's radius, its speed over its slower turn
 * (kps x 180 / (pi x dps) km: the speed and the turn come out of the same steps).  A ship turning at the old game's rates
 * (the Gratha 60 a second, the Jalthi 50) needs this much more room to keep off another than one turning twice as fast */
static int32_t turn_room(int16_t s)
{
    const RvShipType __far *t = slot_type(s);
    int32_t dps = t ? (t->yaw_dps < t->pitch_dps ? t->yaw_dps : t->pitch_dps) : 0;
    return dps > 0 ? (int32_t)speedLo[s] * 57 / dps : 0;
}

/* collision avoidance: another ship (the mind's own target too, unless `target` says so) within the profile's avoid
 * distance and coming together with this one pushes the wanted direction sideways away from it, the harder the
 * nearer */
static void avoid(int16_t s, const AiProfile __far *p, uint8_t target, int32_t *dx, int32_t *dy, int32_t dz)
{
    int16_t k;
    int32_t limit, want = len_of(*dx, *dy, dz);
    RvVec3 r;
    if (!p->avoid_pct)
        return;
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        if (k == s || k == target || !slot_is_ship(k))
            continue;
        limit = room_of(s, k) * p->avoid_pct / 100 + turn_room(s);
        if (labs(TO_KM(gPositionVector[k].x - gPositionVector[s].x)) > limit
            || labs(TO_KM(gPositionVector[k].y - gPositionVector[s].y)) > limit
            || labs(TO_KM(gPositionVector[k].z - gPositionVector[s].z)) > limit)
            continue;
        ship_rel(s, k, &r);             /* (any way round: one coming from behind is dodged too) */
        {
            int32_t d = len_of(r.x, r.y, r.z), away = limit - d, closing;
            RvVec3 dv;
            if (away <= 0)
                continue;
            /* only when coming together at a quarter of the top speed or more: a tail or an orbit at the same speed
             * is no collision coming, and the nose stays on the target */
            dv = vec3(TO_KM(gVelocityVector[s].x - gVelocityVector[k].x), TO_KM(gVelocityVector[s].y - gVelocityVector[k].y),
                      TO_KM(gVelocityVector[s].z - gVelocityVector[k].z));
            {
                RvVec3 w = vec3(TO_KM(gPositionVector[k].x - gPositionVector[s].x), TO_KM(gPositionVector[k].y - gPositionVector[s].y),
                                TO_KM(gPositionVector[k].z - gPositionVector[s].z));
                closing = d ? vec_dot(&dv, &w) / d : 0;
            }
            /* (inside a hull sum and a half, any closing at all: a wingman that drifted across its leader's path at
             * the same speed bumped it, 123 points under the old collision rule) */
            if (closing * KPS_HZ * 4 < (slot_type(s) ? slot_type(s)->max_kps : 300) && (closing <= 0 || d > room_of(s, k) * 3 / 2))
                continue;
            /* (the push in proportion to the wanted direction's length: a wingman wanting a target 12,000 km off
             * was pushed 500 km sideways, a 2-degree nudge, and drifted into its leader; now a push of away/limit
             * of the way itself: a quarter turn at the limit's half) */
            if (want > limit)
                away = away * (want / limit) + away * (want % limit) / limit;
            if (labs(r.x) + labs(r.y) < 10) {   /* (dead ahead: to the right) */
                *dx += away;
                continue;
            }
            *dx -= r.x * away / d;
            *dy -= r.y * away / d;
        }
    }
}

static void steer_to(int16_t s, const AiProfile __far *p, int32_t dx, int32_t dy, int32_t dz)
{
    static const int16_t __far k_pull_at[4] = { 0, 512, 256, 768 };   /* PULL_RIGHT, LEFT, UP, DOWN: their bearings */
    int32_t horiz, across;
    int16_t yaw, pitch, off, cap = (int16_t)((9 * p->turn_pct + 50) / 100);
    int16_t bearing, best = -1, bd = 1024, k, c, d, roll, pull;
    Turn __far *t = &g_turn[s];
    avoid(s, p, 0xFF, &dx, &dy, dz);    /* (the target too: a pass goes round it, as the old game's did) */
    horiz = len_of(dx, 0, dz);
    across = len_of(dx, dy, 0);
    yaw = angle_of(dx, dz);
    pitch = angle_of(dy, horiz);
    off = angle_of(across, dz);
    bearing = angle_of(dy, dx);
    if (cap < 1)
        cap = 1;
    if (off < 0)
        off = (int16_t)-off;
    /* the straight way (a yaw and a pitch at once): a profile with every pull, unless it banks and the way is well
     * off the nose; a profile without them all only when it banks and the way is near the nose (the fine aim: the
     * old game's fighters shot at 0 degrees off); else the roll and the one pull */
    if ((p->pulls & PULL_ALL) == PULL_ALL ? (!p->bank || off <= 2 * FULL_AT) : (p->bank && off <= 2 * FULL_AT)) {
        t->x = (int8_t)clamp32(steer_of(yaw), -cap, cap);
        t->y = (int8_t)clamp32(steer_of(pitch), -cap, cap);
        t->roll = 0;
        return;
    }
    /* the allowed pull nearest the bearing */
    for (k = 0; k < 4; k++) {
        if (!(p->pulls & (1 << k)))
            continue;
        d = (int16_t)(bearing - k_pull_at[k]);
        d = (int16_t)(((d + 512) & 1023) - 512);   /* (the shorter way round) */
        if (d < 0)
            d = (int16_t)-d;
        if (d < bd || (d == bd && k == 2)) {
            bd = d;
            best = k;
        }
    }
    if (best < 0) {                     /* (a profile that may pull nowhere: it flies straight) */
        t->x = t->y = t->roll = 0;
        return;
    }
    c = (int16_t)(bearing - k_pull_at[best]);
    c = (int16_t)(((c + 512) & 1023) - 512);
    roll = steer_of(c);                 /* (toward the pull) */
    /* the pull: full when off the nose, eased in near it; only once the target is within a quarter turn of the pull */
    pull = (int16_t)((int32_t)steer_of(off) * lf_cos(c) / 16384);
    if (pull < 0)
        pull = 0;
    if (pull > cap)
        pull = cap;
    t->x = (int8_t)(best == 0 ? pull : best == 1 ? -pull : 0);
    t->y = (int8_t)(best == 2 ? pull : best == 3 ? -pull : 0);
    t->roll = (int8_t)roll;
}

/* --- the situation, the dice, and the tactics --- */

static int16_t situation(int16_t s, const Mind __far *m, const AiProfile __far *p, const View *v)
{
    (void)p;
    if (m->fear >= FEAR_WARY)           /* (the fear model: the cautious row) */
        return SIT_HURT;
    if (v->rel.z < 0 && v->km < TAIL_KM && v->cos_its > 180)
        return SIT_TAILED;
    if (m->target != 0xFF && v->km < room_of(s, m->target) * 4 && (v->rel.z < 0 || v->cos_nose < 100))
        return SIT_PASSED;              /* (just passed, or about to: within four hulls and off the nose) */
    if (v->km < (int32_t)p->fire_km * 4 / 3 && v->cos_nose > 0)
        return SIT_NEAR;
    return SIT_FAR;
}

/* a tactic's length, steps */
static int16_t tactic_steps(int16_t s, int16_t tactic)
{
    const RvShipType __far *t = slot_type(s);
    switch (tactic) {
    case T_PURSUE: case T_APPROACH: return 8 * LOGIC_HZ;
    case T_HEADON: case T_STRAFE: case T_TORPEDO: return 20 * LOGIC_HZ;
    case T_SIT: return 3 * LOGIC_HZ;
    case T_BREAK: return 4 * LOGIC_HZ;  /* (at most: over when the nose is back on the target) */
    case T_EVADE: return 4 * LOGIC_HZ;
    case T_LOOP: return (int16_t)(t && t->pitch_dps ? INT32_C(360) * KPS_HZ / t->pitch_dps + 4 : 60);
    case T_FLEE: case T_HOME: return 6 * LOGIC_HZ;
    case T_CRUISE: case T_FORMATION: case T_ESCORT: return 2 * LOGIC_HZ;
    default: return 0x7FFF;             /* (T_RAM, T_WAIT: until something else) */
    }
}

/* a tactic taken up: its timer, its way and its afterburner */
static void take_tactic(int16_t s, Mind __far *m, const AiProfile __far *p, int16_t tactic)
{
#ifdef RV_HOST
    if (getenv("RV_TACTICS")) {
        static const char __far f[] = "tactic %u: slot %d %d -> %d (target %d, km %ld, passing %d)";
        rv_logf(f, (unsigned)frameCounter, s, m->tactic, tactic, m->target, m->target != 0xFF ? km_between(s, m->target) : -1L, m->passing);
    }
#endif
    m->tactic = (uint8_t)tactic;
    m->phase = 0;
    m->timer = tactic_steps(s, tactic);
    m->dir = (int8_t)(game_rand() & 1 ? 1 : -1);
    m->burn = (uint8_t)(p->ab_pct && game_rand() % 100 < p->ab_pct);
    /* (an attack on a target facing it from beyond its guns' reach begins by closing in; a tail's own hysteresis stands
     * across the picks: CATCH_*) */
    if (tactic == T_PURSUE && m->target != 0xFF && km_between(s, m->target) > (int32_t)p->fire_km
        && cos_nose_to(m->target, s) >= 0)
        m->catching = 1;
    if (tactic == T_WAIT) {
        m->timer = (int16_t)(p->wait_s * LOGIC_HZ);
        rvFlags[s] |= 16;               /* (cloaked: unseen by the others' minds) */
    }
}

/* the armour and hull left, in a hundred (the old game's health, which its fear goes by) */
static int16_t health_pct(int16_t s)
{
    const RvShipType __far *t = slot_type(s);
    int32_t most, left;
    if (!t)
        return 100;
    most = (int32_t)t->armor_f + t->armor_r + t->armor_l + t->armor_rt + t->hull;
    left = (int32_t)ARMOR(s, 0) + ARMOR(s, 1) + ARMOR(s, 2) + ARMOR(s, 3) + (t->hull > damagePoints[s] ? t->hull - damagePoints[s] : 0);
    return (int16_t)(most ? left * 100 / most : 100);
}

/* a missile is homing on s */
static int16_t missile_at(int16_t s)
{
    int16_t k;
    for (k = 1; k < NUM_SHIP_SLOTS; k++)
        if (maybeEntityType[k] == ET_MISSILE && missileTarget[k] == s)
            return 1;
    return 0;
}

/* the nearest living capital ship of s's side, -1 none */
static int16_t nearest_capital(int16_t s)
{
    int16_t k, best = -1;
    int32_t bd = INT32_C(0x7FFFFFFF), d;
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        if (k == s || !slot_is_ship(k) || maybeEntityType[k] != ET_CAPSHIP || commAnimInfo[k] != commAnimInfo[s] || shipAiState[k] == 9)
            continue;
        d = km_between(s, k);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* the fear model at a pick (above): the fear moved by the situation and the health; 1 when it is the leave */
static int16_t fear_step(int16_t s, Mind __far *m, const AiProfile __far *p, int16_t sit, const View *v)
{
    int16_t f = m->fear, c = m->fear_up, d = m->fear_down, h;
    if (missile_at(s))
        f += 2 * c;
    else if (m->target != 0xFF && shipAiState[m->target] == 9)
        f /= 2;
    else if (sit == SIT_TAILED)
        f += c;
    else if (v->cos_nose > 139 && v->cos_its < 0)   /* (a tail chase: the target ahead within 57 degrees, facing away) */
        f -= c;
    else if ((sit == SIT_FAR || sit == SIT_NEAR) && !g_under_fire[s])   /* (the plain case: not hit of late) */
        f -= d;
    h = health_pct(s);
    if (p->scared_pct && h < p->scared_pct)
        f += 2 * c;
    else if (h < HEALTH_BOLD) {
        f += c;
        if (f > FEAR_CAP_WARY)
            f = FEAR_CAP_WARY;
    } else if (f > FEAR_CAP_BOLD)
        f = FEAR_CAP_BOLD;
    m->fear = (uint8_t)clamp32(f, 0, 255);
    return m->fear >= FEAR_LEAVE;
}

/* the dice over the profile's picks for a situation (a torpedo run only with a torpedo and a capital ship to run at:
 * else a pursuit); first the fear model: the leave, or the run home, before any dice */
static void pick_tactic(int16_t s, Mind __far *m, const AiProfile __far *p, int16_t sit, const View *v)
{
    const Pick __far *row;
    int16_t k, sum = 0, tactic = T_PURSUE;
    uint16_t r;
    if (m->tactic == T_HOME)            /* (home, or its run's time up: its fear is gone, as the old game had it) */
        m->fear = 0;
    if (v && fear_step(s, m, p, sit, v)) {
        if (nearest_capital(s) >= 0) {
            if (m->tactic != T_HOME) {
                take_tactic(s, m, p, T_HOME);
                return;
            }
        } else {
            m->leaving = 1;
            take_tactic(s, m, p, T_FLEE);
            return;
        }
    }
    if (m->fear >= FEAR_WARY)
        sit = SIT_HURT;
    row = p->pick[sit];
    for (k = 0; k < PICKS; k++)
        sum = (int16_t)(sum + row[k].weight);
    if (sum) {
        r = (uint16_t)(game_rand() % sum);
        for (k = 0; k < PICKS; k++) {
            if (r < row[k].weight) {
                tactic = row[k].tactic;
                break;
            }
            r = (uint16_t)(r - row[k].weight);
        }
    }
    if (tactic == T_TORPEDO && (launcher_of(s) < 0 || m->target == 0xFF || maybeEntityType[m->target] != ET_CAPSHIP))
        tactic = T_PURSUE;
    /* the old game's choice (its maneuver 45): the pursuit, unless the target faces the ship within 37 degrees, then the
     * approach; and the approach itself only under the cone rule (under aim=lead the measured pursuit stands) */
    if (tactic == T_CLOSE)
        tactic = v && v->cos_its > HOLD_COS ? T_APPROACH : T_PURSUE;
    if (tactic == T_APPROACH && !(g_rule_flags & RV_RULE_AIM_CONE))
        tactic = T_PURSUE;
    take_tactic(s, m, p, tactic);
}

static void throttle(int16_t s, int32_t kps, int16_t burn);
static int16_t cos_nose_to(int16_t s, int16_t h);

/* the leave (the old game's order 5, read in its code): straight up, or down for an odd slot, at its top speed with the
 * afterburner in bursts; and beyond LEAVE_KM of every human's ship it is removed, quietly, its mission ship away (not
 * killed: the mission goes on without it) */
static void leave_step(int16_t s, Mind __far *m, const AiProfile __far *p)
{
    static const char __far s_gone[] = "logic: slot %d (mission ship %d) left: beyond %d km of every pilot";
    const RvShipType __far *t = slot_type(s);
    RvVec3 up = vec3(0, (s & 1) ? -256 : 256, 0), b;
    int16_t k, near_ = 0, far_ = 1;
    to_body(s, &up, &b);
    steer_to(s, p, b.x, b.y, b.z);
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        if (k == s || !slot_is_ship(k) || shipAiState[k] == 9)
            continue;
        if (commAnimInfo[k] != commAnimInfo[s] && km_between(s, k) < LEAVE_KM)
            near_ = 1;
        if ((rvController[k] == CTL_PLAYER || rvController[k] == CTL_PUPPET) && km_between(s, k) <= LEAVE_KM)
            far_ = 0;
    }
    /* the burner in the old game's bursts (its leave, read in its code 2026-11-02: ovr163:0228, ovr143:2F5D): 40 of its
     * frames at a time, the next only once it has slowed back to its top speed (coasting at its acceleration), then at
     * once with an enemy within LEAVE_KM, else half the time a frame.  A run that surges and coasts, which a pilot on the
     * burner catches (the Nicander's 1,200 for 2.7 s, then 3.6 s down to 400: some 980 kps on the whole, the
     * Schroedinger's burner 1,260); held on it, ours got away (the user: "leap off into the ether") */
    if (m->burst_left)
        m->burst_left--;
    else if (t && speedLo[s] <= (uint32_t)t->max_kps && (near_ || game_rand() % (2 * WC_FRAME_STEPS) == 0))
        m->burst_left = 40 * WC_FRAME_STEPS;
    throttle(s, t ? t->max_kps : 300, m->burst_left > 0);
#ifdef RV_HOST
    if (getenv("RV_TACTICS") && frameCounter % 30 == 0) {
        static const char __far f[] = "leave %u: slot %d fear %d at %ld %ld %ld km, speed %lu, burner %d, steer %d %d %d";
        rv_logf(f, (unsigned)frameCounter, s, m->fear, (long)TO_KM(gPositionVector[s].x), (long)TO_KM(gPositionVector[s].y),
                (long)TO_KM(gPositionVector[s].z), (unsigned long)speedLo[s], rvAfterburner[s], g_turn[s].x, g_turn[s].y, g_turn[s].roll);
    }
#endif
    if (far_) {
        rv_notef(s_gone, s, slotMissionShip[s] < 64 ? slotMissionShip[s] : -1, LEAVE_KM);
        if (slotMissionShip[s] < 64)
            g_away[slotMissionShip[s]] = 1;
        if (commAnimInfo[s] != commAnimInfo[0])
            g_enemy_ran = 1;
        despawn((uint16_t)s);
    }
}

/* the throttle, through the thunk, and the afterburner */
static void throttle(int16_t s, int32_t kps, int16_t burn)
{
    const RvShipType __far *t = slot_type(s);
    int32_t most = t ? t->max_kps : 300;
    aiSetSpeed((uint16_t)s, (uint16_t)clamp32(kps, 0, most));
    rvAfterburner[s] = (uint8_t)(burn && t && t->ab_kps ? 1 : 0);
    if (rvAfterburner[s] && speedLo[s] < (uint32_t)t->ab_kps)   /* (on it, at its burner speed at once: the old game's
                                                                   fighters dashed to 3x in a frame; the player's ramps) */
        speedLo[s] = (uint32_t)t->ab_kps;
    /* (off it, it slows at its type's acceleration, as the player's does: the old fighters came down from a dash at
     * some 22 km/s a frame of their 15, the Nicander's 150 here; one put at its top speed at once had its burner as
     * good as always on) */
}

#define AIM_MISS_KM 300                 /* a shot is let go when the nose's line passes this near the lead point */

/* how straight s's nose points at slot h (cos x 256) */
static int16_t cos_nose_to(int16_t s, int16_t h)
{
    RvVec3 d = vec3(TO_KM(gPositionVector[h].x - gPositionVector[s].x), TO_KM(gPositionVector[h].y - gPositionVector[s].y),
                    TO_KM(gPositionVector[h].z - gPositionVector[s].z));
    int32_t km = len_of(d.x, d.y, d.z);
    return (int16_t)(km ? clamp32(vec_dot(&d, &gOrientationFrontVector[s]) / km, -256, 256) : 256);
}

#define MISSILE_DODGE_KM 8000           /* the old game's missile dodge (its maneuver 32, read in its code): a missile
                                           homing on the ship from within 37 degrees ahead and this near, and the ship
                                           fires its guns at its target whatever the hold */

/* a missile homing on s from within 37 degrees ahead of its nose and under MISSILE_DODGE_KM */
static int16_t missile_ahead(int16_t s)
{
    int16_t k;
    for (k = 1; k < NUM_SHIP_SLOTS; k++)
        if (maybeEntityType[k] == ET_MISSILE && missileTarget[k] == s && km_between(s, k) < MISSILE_DODGE_KM
            && cos_nose_to(s, k) > HOLD_COS)
            return 1;
    return 0;
}

/* the held fire (the mission's rule hold=on; the old game's, read in its code and seen in its flight: the ship a
 * player has selected as a target holds its guns, its missiles and its mines while that player is within 37 degrees
 * ahead of its nose, in its tailing attacks; its wingmen fire on; under a missile coming at it from ahead it fires,
 * the old missile dodge): 1 when s must hold this step.  A profile of PF_NO_MERCY never holds (the user's ruling,
 * 2026-10-31: the ace keeps the old game's one exempt class) */
static int16_t holds_fire(int16_t s, const AiProfile __far *p)
{
    int16_t h;
    if (!(g_rule_flags & RV_RULE_HOLD_FIRE) || (p->flags & PF_NO_MERCY) || g_mind[s].taunt)
        return 0;
    for (h = 0; h < NUM_SHIP_SLOTS; h++)
        if (h != s && slot_is_ship(h) && shipAiState[h] != 9 && (rvController[h] == CTL_PLAYER || rvController[h] == CTL_PUPPET)
            && rvTarget[h] == s && cos_nose_to(s, h) > HOLD_COS)
            return !missile_ahead(s);
    return 0;
}

/* the guns, when the lead point is in the cone and the target in range (the lead rule), or when the target itself is
 * within the weapon's own cone and in range, every AI_REFIRE_STEPS (the cone rule: the old game's spray, the hits
 * coming as the nose crosses the target); a missile after a lock */
static void aim_and_fire(int16_t s, Mind __far *m, const AiProfile __far *p, const View *v, int16_t guns)
{
    int16_t g = gun_of(s), l, held;
    const RvWeapon __far *w = g >= 0 ? weapon(GUN_TYPE(s, g)) : 0;
    int32_t reach = w ? w->range_km : 0;
    if (reach > p->fire_km)
        reach = p->fire_km;
    if (m->gap < 255)
        m->gap++;
    held = holds_fire(s, p);
    if (guns && w && v->km <= reach && gunEnergy[s] > 0 && !held) {
        if (g_rule_flags & RV_RULE_AIM_CONE) {
            if (v->cos_nose >= (int16_t)w->cone_cos && m->gap >= AI_REFIRE_STEPS) {
                m->fire |= FIRE_GUNS;
                m->gap = 0;
            }
        } else if (v->cos_fire >= p->aim_cos && m->gap >= p->fire_gap && v->miss_km <= AIM_MISS_KM) {
            /* (the lead point within the cone, and, far out, within AIM_MISS_KM of the nose's line at its range: the
             * cone alone let a tail fire 6 degrees off at 2,800 km, 300 km wide of the target) */
            m->fire |= FIRE_GUNS;
            m->gap = 0;
        }
    }
    /* a lock: the target in the cone for the weapon's lock time, then the dice, then the launch (not under the held
     * fire: the old wrapper skipped the missiles with the guns) */
    if ((l = launcher_of(s)) >= 0 && (w = weapon(GUN_TYPE(s, l))) != 0 && v->cos_nose >= LOCK_CONE
        && v->km <= w->range_km && (uint16_t)(frameCounter - m->since) >= RELAUNCH_STEPS) {
        if (++m->lock >= (uint16_t)(w->lock_tenths * 3 + 1)) {
            m->lock = 0;
            m->since = frameCounter;
            if (game_rand() % 100 < p->missile_pct && !held) {
                m->fire |= FIRE_MISSILE;
                m->launcher = (uint8_t)l;
            }
        }
    } else {
        m->lock = 0;
    }
}

/* the place o (km: right, up, ahead) off ship L, in its frame, as positions */
static RvVec3 off_ship(int16_t L, const int16_t __far *o)
{
    const RvVec3 *R = &gOrientationRightVector[L], *U = &gOrientationUpVector[L], *F = &gOrientationFrontVector[L];
    return vec3(gPositionVector[L].x + (FIX_MUL(R->x, o[0]) + FIX_MUL(U->x, o[1]) + FIX_MUL(F->x, o[2])) * KM,
                gPositionVector[L].y + (FIX_MUL(R->y, o[0]) + FIX_MUL(U->y, o[1]) + FIX_MUL(F->y, o[2])) * KM,
                gPositionVector[L].z + (FIX_MUL(R->z, o[0]) + FIX_MUL(U->z, o[1]) + FIX_MUL(F->z, o[2])) * KM);
}

/* T_FORMATION: station on the leader (formationOffset, km in its frame: taken from where the ship is when it has
 * none), the leader's heading near it, and a throttle that closes the gap and then matches */
#define FORM_NEAR_KM 500                /* within this of its place a wingman flies the leader's way, not at its place */
#define FORM_LEAD_KM 2000               /* how far ahead along it it aims: its gap across, at most 500 km, 14 degrees */
static void formation_step(int16_t s, Mind __far *m, const AiProfile __far *p)
{
    int16_t L = m->leader;
    int16_t *off = (int16_t *)(formationOffset + 6 * s);
    RvVec3 st, d, b;
    int32_t km, along, kps;
    const RvVec3 *F;
    if (L == 0xFF || !slot_is_ship(L) || shipAiState[L] == 9) {
        throttle(s, slot_type(s) ? slot_type(s)->cruise_kps : 200, 0);
        return;
    }
    F = &gOrientationFrontVector[L];
    if (!off[0] && !off[1] && !off[2]) {   /* its place: where it is now off the leader, at least 800 km to a side */
        ship_rel(L, s, &b);
        off[0] = (int16_t)clamp32(b.x, -6000, 6000);
        off[1] = (int16_t)clamp32(b.y, -6000, 6000);
        off[2] = (int16_t)clamp32(b.z, -6000, 6000);
        if (labs(off[0]) < 800 && labs(off[1]) < 800)
            off[0] = (int16_t)(off[0] < 0 ? -800 : 800);
    }
    st = off_ship(L, off);
    d = vec3(TO_KM(st.x - gPositionVector[s].x), TO_KM(st.y - gPositionVector[s].y), TO_KM(st.z - gPositionVector[s].z));
    km = len_of(d.x, d.y, d.z);
    along = vec_dot(&d, F) / 256;       /* (the gap along the leader's way: behind it, or ahead of it) */
    if (km > FORM_NEAR_KM) {
        to_body(s, &d, &b);
        steer_to(s, p, b.x, b.y, b.z);
    } else {
        /* near its place: along the leader's way, the nose turned only by the gap across it; the gap along it is the
         * throttle's (the user, 2026-11-03: "when I say form up the wingmate faces me instead of being parallel") */
        RvVec3 a = vec3(d.x - FIX_MUL(F->x, along) + FIX_MUL(F->x, FORM_LEAD_KM),
                        d.y - FIX_MUL(F->y, along) + FIX_MUL(F->y, FORM_LEAD_KM),
                        d.z - FIX_MUL(F->z, along) + FIX_MUL(F->z, FORM_LEAD_KM));
        to_body(s, &a, &b);
        steer_to(s, p, b.x, b.y, b.z);
    }
    kps = speedLo[L];
    if (km > 4000)
        kps = 9999;
    else if (km > FORM_NEAR_KM)         /* (headed for its place: closing on it, whichever way it lies; by the gap along
                                           the leader's way alone, a place behind it with the player still was no speed,
                                           and it sat facing its place, and the player, the user's "faces me") */
        kps += km / 8 + 20;
    else if (along > 0)
        kps += along / 8 + 20;
    else
        kps += along / 8;
    throttle(s, kps, km > 8000 && m->burn);
}

/* T_ESCORT: round the leader at a distance, a turn every so often */
static void escort_step(int16_t s, Mind __far *m, const AiProfile __far *p)
{
    int16_t L = m->leader, a = (int16_t)((frameCounter / 4) & 1023);
    RvVec3 st, d, b;
    const RvVec3 *R, *F;
    if (L == 0xFF || !slot_is_ship(L) || shipAiState[L] == 9) {
        formation_step(s, m, p);
        return;
    }
    R = &gOrientationRightVector[L];
    F = &gOrientationFrontVector[L];
    st = vec3(gPositionVector[L].x + (FIX_MUL(R->x, lf_cos(a) / 64 * 2500 / 256) + FIX_MUL(F->x, lf_sin(a) / 64 * 2500 / 256)) * KM,
              gPositionVector[L].y,
              gPositionVector[L].z + (FIX_MUL(R->z, lf_cos(a) / 64 * 2500 / 256) + FIX_MUL(F->z, lf_sin(a) / 64 * 2500 / 256)) * KM);
    d = vec3(TO_KM(st.x - gPositionVector[s].x), TO_KM(st.y - gPositionVector[s].y), TO_KM(st.z - gPositionVector[s].z));
    to_body(s, &d, &b);
    steer_to(s, p, b.x, b.y, b.z);
    throttle(s, slot_type(s) ? slot_type(s)->cruise_kps : 200, 0);
}

/* a pass is coming: closing at a tenth of the top speed or more (a target tailed at its own speed is aimed at, a
 * slow overtake is a pass) */
static int16_t passing(int16_t s, const View *v)
{
    const RvShipType __far *t = slot_type(s);
    return v->closing * KPS_HZ * 10 >= (t ? t->max_kps : 300);
}

/* an attack aims beside its target rather than through it: by a lane (the attackers of one target, in slot order,
 * spread across it a hull's width apart, so a wing does not fly together into one point and into each other), and
 * near the target by the tactic's own side (a pass goes by the two half boxes and a margin): the old games' fighters
 * pass close by, they seldom collide */
static int32_t pass_side(int16_t s, const Mind __far *m, const AiProfile __far *p, const View *v)
{
    int32_t room, lane = 0, n = 0, rank = 0;
    int16_t k;
    if (m->target == 0xFF)
        return 0;
    room = room_of(s, m->target) * p->pass_pct / 100;
    for (k = 0; k < NUM_SHIP_SLOTS; k++)
        if (k != s && slot_is_ship(k) && rvController[k] == CTL_AI && shipAiState[k] != 9 && g_mind[k].target == m->target
            && commAnimInfo[k] == commAnimInfo[s] && maybeEntityType[k] == ET_SHIP) {
            n++;
            if (k < s)
                rank++;
        }
    if (n)                              /* (lanes -n/2 .. n/2 about the target) */
        lane = (rank * 2 - n) * room / 2;
    if (passing(s, v) && v->km < v->closing * LOGIC_HZ + 2 * turn_room(s))   /* (the pass within a second, and the room
                                           its turning circle wants: by its own side of the target) */
        lane += (int32_t)m->dir * room;
    return lane;
}

/* where an attack points the nose: within the guns' reach, the lead point as the ship's own shots fly (its speed
 * along the nose carried: gun_fire), so what the nose is on is what the shots meet; beyond it, the target's own
 * lead point, the old fighters' approach (the ship's velocity shifts as it turns, and so does the shot's way) */
static const RvVec3 *aim_at(const AiProfile __far *p, const View *v)
{
    return v->km <= p->fire_km ? &v->aim_fire : &v->aim;
}

#define APPROACH_KM 500                 /* the old game's approach (its maneuver 40, read in its code): at a point this
                                           far plus the target's radius behind the target, above it when the target
                                           faces it, until within APPROACH_TAIL_KM plus the radius with the target
                                           facing away by more than 120 degrees: then the tail, nose on it */
#define APPROACH_TAIL_KM 2000

/* the approach's point (above), in s's frame; 1 when it is the way, 0 when the tail (nose on the target) is.  An approach that goes straight down the target's gun line is what a hunting player hits every
 * shot: the old fighters came in beside it */
static int16_t approach_point(int16_t s, int16_t tgt, const View *v, RvVec3 *out)
{
    int32_t off = (int32_t)(slot_type(tgt) ? slot_type(tgt)->radius_km : 100) + APPROACH_KM;
    const RvVec3 *ax;
    RvVec3 d;
    if (v->km < off - APPROACH_KM + APPROACH_TAIL_KM && v->cos_its < -128)
        return 0;
    ax = v->cos_its > HOLD_COS ? &gOrientationUpVector[tgt] : &gOrientationFrontVector[tgt];
    d = vec3(TO_KM(gPositionVector[tgt].x - gPositionVector[s].x) + (v->cos_its > HOLD_COS ? ax->x * off / 256 : -ax->x * off / 256),
             TO_KM(gPositionVector[tgt].y - gPositionVector[s].y) + (v->cos_its > HOLD_COS ? ax->y * off / 256 : -ax->y * off / 256),
             TO_KM(gPositionVector[tgt].z - gPositionVector[s].z) + (v->cos_its > HOLD_COS ? ax->z * off / 256 : -ax->z * off / 256));
    to_body(s, &d, out);
    return 1;
}

/* the tactic's step: the nose, the throttle, the fire; 1 when it is over before its time */
static int16_t run_tactic(int16_t s, Mind __far *m, const AiProfile __far *p, const View *v)
{
    const RvShipType __far *t = slot_type(s);
    int32_t cruise = t ? t->cruise_kps : 200, max = t ? t->max_kps : 300;
    switch (m->tactic) {
    case T_PURSUE:                      /* at its lead point (the cone rule: at the target itself), the guns when they
                                           bear (the old game's 29, and its tail, 26); closing, then matching; off
                                           before a collision */
        steer_to(s, p, aim_at(p, v)->x + pass_side(s, m, p, v), aim_at(p, v)->y, aim_at(p, v)->z);
        /* (closing from far at full speed, then at its attack speed (CATCH_*), head on or behind; past it, full speed
         * round) */
        if (v->km > CATCH_FAR_KM)
            m->catching = 1;
        else if (v->km < (v->cos_its >= 0 ? (int32_t)p->fire_km : CATCH_NEAR_KM))
            m->catching = 0;
        throttle(s, m->catching || v->rel.z < 0 ? max : max * p->tail_pct / 100, m->catching && v->km > 5000 && m->burn);
        aim_and_fire(s, m, p, v, 1);
        if (p->break_km && v->km < p->break_km && v->cos_nose > 0 && m->passing) {
            take_tactic(s, m, p, T_BREAK);   /* (the old game's fighters: straight at it, then a hard flat yaw away) */
            return 0;
        }
        return v->rel.z < 0 && v->km < TAIL_KM;
    case T_APPROACH: {                  /* the old game's approach (its maneuver 40, read in its code; the user: keep
                                           it): to the point 500 km plus the radius behind the target (above it when
                                           it faces the ship), no guns at all, at full speed; the tail (T_PURSUE,
                                           nose on it, the guns) once behind within the radius plus 2,000 km with
                                           the target facing away by more than 120 degrees; a break before a pass */
        RvVec3 ap;
        if (m->target == 0xFF || !approach_point(s, m->target, v, &ap)) {
            take_tactic(s, m, p, T_PURSUE);
            return 0;
        }
        steer_to(s, p, ap.x, ap.y, ap.z);
        throttle(s, max, v->km > 5000 && m->burn);
        if (p->break_km && v->km < p->break_km && v->cos_nose > 0 && m->passing) {
            take_tactic(s, m, p, T_BREAK);
            return 0;
        }
        return v->rel.z < 0 && v->km < TAIL_KM;
    }
    case T_HEADON:                      /* straight at it at full speed, firing; off at the break distance */
    case T_STRAFE:                      /* the same, but through: it keeps going until it has passed */
        steer_to(s, p, aim_at(p, v)->x + pass_side(s, m, p, v), aim_at(p, v)->y, aim_at(p, v)->z);
        throttle(s, max, m->burn && v->km > 4000);
        aim_and_fire(s, m, p, v, 1);
        if (p->break_km && v->km < p->break_km && (m->tactic == T_HEADON || (m->passing && v->cos_its < 0))) {
            take_tactic(s, m, p, T_BREAK);   /* (a strafe goes through a head-on pass, not through a tail) */
            return 0;
        }
        return v->rel.z < 0;            /* (through the pass: the target behind, the dice again) */
    case T_SIT:                         /* still, turning onto it, firing */
        steer_to(s, p, aim_at(p, v)->x, aim_at(p, v)->y, aim_at(p, v)->z);
        throttle(s, 0, 0);
        aim_and_fire(s, m, p, v, 1);
        return 0;
    case T_RAM:                         /* at it, as fast as it goes, never off */
        steer_to(s, p, v->rel.x, v->rel.y, v->rel.z);
        throttle(s, max, 1);
        aim_and_fire(s, m, p, v, 1);
        return 0;
    case T_TORPEDO: {                   /* at a capital ship until a torpedo is away, then off */
        int16_t l = launcher_of(s);
        steer_to(s, p, aim_at(p, v)->x, aim_at(p, v)->y, aim_at(p, v)->z);
        throttle(s, v->km > 4000 ? max : cruise, 0);
        aim_and_fire(s, m, p, v, v->km < 2000);
        if (m->fire & FIRE_MISSILE) {
            take_tactic(s, m, p, T_BREAK);
            return 0;
        }
        return l < 0 || (p->break_km && v->km < p->break_km);
    }
    case T_BREAK: {                     /* a hard turn one way at full throttle: a flat yaw (away from a target still
                                           ahead, after it once it is behind: the old game's fighters) for a profile
                                           that yaws, a pull with a roll toward the target's side for one that banks
                                           and may pull up or down; the afterburner for the profile's burst at its
                                           start */
        int16_t at = (int16_t)(tactic_steps(s, T_BREAK) - m->timer), cap = (int16_t)((9 * p->turn_pct + 50) / 100);
        int16_t flat = !(p->bank && (p->pulls & (PULL_UP | PULL_DOWN)));
        if (!m->phase) {
            m->phase = 1;
            if (m->target != 0xFF)
                m->dir = (int8_t)(flat ? ((v->rel.x < 0) == (v->rel.z > 0) ? 1 : -1) : (v->rel.x < 0 ? -1 : 1));
        }
        if (!flat) {
            throttle(s, max, at < p->burst);   /* (the dash: the old game's fighters lit it at every pass) */
            g_turn[s].y = (int8_t)(cap * m->dir);
            g_turn[s].roll = (int8_t)(5 * m->dir);
            return m->target != 0xFF && v->cos_nose >= 230 && at > 2;
        }
        /* the old game's fighters, measured (tests/wc1/README.md): a yaw away for the burst (20-120 degrees in the
         * pass's second), then straight out to out_km (2,500 km: their nose was back on the player 1.9 s after a
         * pass, 2,500 km out, and the next pass came 5.3 s after; ours came round after a second at 900 km, passed
         * every 2 s, and crowded one another), then the yaw back toward the target's side until the nose is on it */
        if (m->phase == 1 && at >= p->burst)
            m->phase = 2;
        if (m->phase == 2 && (m->target == 0xFF || v->km >= p->out_km || at >= p->burst + BREAK_OUT_STEPS))
            m->phase = 3;
        if (m->phase == 1)
            g_turn[s].x = (int8_t)(cap * m->dir);
        else if (m->phase == 3 && m->target != 0xFF)
            g_turn[s].x = (int8_t)(v->rel.x < 0 ? -cap : cap);
        /* the dash on the straight leg's first steps, away (the old game's at every pass): lit during the yaw, into
         * the pass, it closed at 2,000 km/s from 900 km and grazed a player flying straight in 7 duels of 8 */
        throttle(s, max, m->phase == 2 && at < 2 * p->burst);
        return m->phase == 3 && (m->target == 0xFF || v->cos_nose >= 230);
        /* (over with the nose on the target: the pursuit takes it from there, and breaks away again if too near) */
    }
    case T_EVADE: {                     /* jinks: a pull each way with a roll, a stop by chance, then away */
        int16_t at = (int16_t)(tactic_steps(s, T_EVADE) - m->timer);
        if (at < LOGIC_HZ) {
            g_turn[s].y = (int8_t)(9 * m->dir);
            g_turn[s].roll = 9;
            throttle(s, max, 0);
        } else if (at < 2 * LOGIC_HZ) {
            g_turn[s].y = (int8_t)(-9 * m->dir);
            g_turn[s].roll = -9;
            throttle(s, max, 0);
        } else if (m->burn) {           /* (the stop: the pursuer overshoots) */
            g_turn[s].x = (int8_t)(5 * m->dir);
            throttle(s, at < 3 * LOGIC_HZ ? 0 : max, at >= 3 * LOGIC_HZ);
        } else {
            g_turn[s].y = 9;
            g_turn[s].roll = (int8_t)(4 * m->dir);
            throttle(s, max, 1);
        }
        return v->rel.z > 0 && v->km > TAIL_KM;
    }
    case T_LOOP:                        /* a whole loop, nose up */
        g_turn[s].y = 9;
        throttle(s, cruise, 0);
        return 0;
    case T_FLEE:                        /* away from it, as fast as it goes */
        steer_to(s, p, -v->rel.x, -v->rel.y, -v->rel.z);
        throttle(s, max, m->burn);
        return 0;
    case T_HOME: {                      /* to its side's nearest capital ship on the afterburner, under its guns (the
                                           old game's ship with its carrier about had its fear zeroed and fought on:
                                           ours runs there first); over there, or when the time is up (fear 0 then,
                                           in pick_tactic) */
        int16_t h = nearest_capital(s);
        RvVec3 d, b;
        if (h < 0)
            return 1;
        d = vec3(TO_KM(gPositionVector[h].x - gPositionVector[s].x), TO_KM(gPositionVector[h].y - gPositionVector[s].y),
                 TO_KM(gPositionVector[h].z - gPositionVector[s].z));
        to_body(s, &d, &b);
        steer_to(s, p, b.x, b.y, b.z);
        throttle(s, max, 1);
        return km_between(s, h) < room_of(s, h) * 3;
    }
    default:
        return 1;
    }
}

/* a bump (step_collisions): a fighter breaks away from it, a capital ship holds its course */
static void bump_away(int16_t s, int8_t dir)
{
    Mind __far *m = &g_mind[s];
    if (maybeEntityType[s] != ET_SHIP || shipAiState[s] == 9 || m->tactic == T_BREAK)
        return;
    take_tactic(s, m, &k_profiles[m->profile < P_COUNT ? m->profile : P_FIGHTER], T_BREAK);
    m->phase = 1;                       /* (its way is the bump's, not the target's side) */
    m->dir = dir;
    m->burn = 1;
}

/* the old game's steer-clear (its maneuver 2, read in its code: turn away, break when within three radii, burn) for
 * any ship, not the target alone: one ahead within the avoid limit's half and closing at a quarter of the top speed
 * or more is broken away from (a flat break, away from its side, with the dash); 1 when taken.  Two fighters that
 * rammed each other at 1,000 points apiece under the old collision rule had only the sideways nudge before */
static int16_t avoid_break(int16_t s, Mind __far *m, const AiProfile __far *p)
{
    const RvShipType __far *t = slot_type(s);
    int16_t k;
    if (!p->avoid_pct || !p->break_km || m->tactic == T_BREAK || m->tactic == T_HOME)
        return 0;
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        int32_t limit, d, closing;
        RvVec3 r, dv, w;
        if (k == s || !slot_is_ship(k) || maybeEntityType[k] == ET_MISSILE)
            continue;
        limit = room_of(s, k) * p->avoid_pct / 200 + turn_room(s);
        if (labs(TO_KM(gPositionVector[k].x - gPositionVector[s].x)) > limit
            || labs(TO_KM(gPositionVector[k].y - gPositionVector[s].y)) > limit
            || labs(TO_KM(gPositionVector[k].z - gPositionVector[s].z)) > limit)
            continue;
        ship_rel(s, k, &r);
        d = len_of(r.x, r.y, r.z);
        if (d > limit || r.z <= 0 || r.z * 2 < d)   /* (ahead, within 60 degrees of the nose) */
            continue;
        dv = vec3(TO_KM(gVelocityVector[s].x - gVelocityVector[k].x), TO_KM(gVelocityVector[s].y - gVelocityVector[k].y),
                  TO_KM(gVelocityVector[s].z - gVelocityVector[k].z));
        w = vec3(TO_KM(gPositionVector[k].x - gPositionVector[s].x), TO_KM(gPositionVector[k].y - gPositionVector[s].y),
                 TO_KM(gPositionVector[k].z - gPositionVector[s].z));
        closing = d ? vec_dot(&dv, &w) / d : 0;
        if (closing * KPS_HZ * 4 < (t ? t->max_kps : 300))
            continue;
        take_tactic(s, m, p, T_BREAK);
        m->phase = 1;
        m->dir = (int8_t)(r.x < 0 ? 1 : -1);
        m->burn = 1;
        return 1;
    }
    return 0;
}

/* An escort target of the player's side about (a transport the player guards): half its enemies' fighters near it,
 * at least one, go for it, the ones nearest it, and keep at it until it is gone or they are; then the next nearest
 * (the user, 2026-11-03: "the enemies don't seem particularly interested in it even though at least one should go for
 * it"; 2026-11-04, of one raider at an Ox as hard as a carrier: "The escort wasn't even a bit challenging").  The
 * player's taunt still turns a raider (logic_taunt).  Only the other side's fighters raid: a wingman's targets stay
 * its orders'.  For s: the escort target to go for, -1 none */
#define RAID_KM 20000                   /* an escort target this near draws its raider */
static int16_t escorted(int16_t s);

static int16_t raid_target(int16_t s)
{
    int16_t e, k, best = -1, near_n = 1, ahead = 0;
    int32_t d, bd = INT32_C(0x7FFFFFFF);
    if (commAnimInfo[s] == commAnimInfo[0] || maybeEntityType[s] != ET_SHIP || g_mind[s].leaving)
        return -1;
    for (e = 1; e < NUM_SHIP_SLOTS; e++)    /* (the escort target of the player's side nearest it) */
        if (slot_is_ship(e) && shipAiState[e] != 9 && escorted(e) && commAnimInfo[e] == commAnimInfo[0]
            && (d = km_between(s, e)) < RAID_KM && d < bd) {
            bd = d;
            best = e;
        }
    if (best < 0)
        return -1;
    if (g_mind[s].raiding == best + 1)
        return best;                    /* (already at it) */
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {  /* (half of them near it raid, rounded up: the others at it already, or nearer) */
        if (k == s || !slot_is_ship(k) || shipAiState[k] == 9 || commAnimInfo[k] != commAnimInfo[s]
            || maybeEntityType[k] != ET_SHIP || rvController[k] != CTL_AI || g_mind[k].leaving)
            continue;
        d = km_between(k, best);
        if (d < RAID_KM)
            near_n++;
        if (g_mind[k].raiding == best + 1 || d < bd)
            ahead++;
    }
    return ahead < (near_n + 1) / 2 ? best : -1;
}

/* the ship s is ordered to attack (its mission record's prey type, missions.txt prey=KEY: the old game's behaviour-2
 * orders, read from its mission records 2026-11-05: at Enyo 2's nav 1 its Salthi went for the Drayman 8,000 km off
 * with the player at 2,000, and so in every escort of the campaign; tests/wc1/README.md "Orders"), while its health
 * is at least the profile's share of a hundred (the old game's 70 - 15 x its class, 0 to 4: a fighter's class 1, 55;
 * an ace's 10) and the prey lives: the nearest living ship of that type of another side; -1 none */
static const uint8_t __far k_prey_pct[P_COUNT] = { 55, 55, 40, 10, 40, 0, 0, 55, 55 };
static int16_t prey_of(int16_t s, uint8_t profile)
{
    uint16_t rec = slotMissionShip[s];
    uint8_t type;
    int16_t k, best = -1;
    int32_t bd = INT32_C(0x7FFFFFFF), d;
    if (rec >= 64 || !MSHIP_PREY(rec) || health_pct(s) < k_prey_pct[profile < P_COUNT ? profile : P_FIGHTER])
        return -1;
    type = (uint8_t)(MSHIP_PREY(rec) - 1);
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        if (k == s || !slot_is_ship(k) || commAnimInfo[k] == commAnimInfo[s] || shipAiState[k] == 9 || (rvFlags[k] & 16)
            || gInstanceShipTypes[k] != type)
            continue;
        d = km_between(s, k);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* a fighter's mind for a step: its target, its situation, its tactic, its decisions (g_turn, the throttle, fire) */
static void fighter_think(int16_t s)
{
    Mind __far *m = &g_mind[s];
    const AiProfile __far *p = &k_profiles[m->profile < P_COUNT ? m->profile : P_FIGHTER];
    View v;
    int16_t sit, k, near_ = -1;
    int32_t d, bd = INT32_C(0x7FFFFFFF);
    m->leader = leader_of(s);
    if (m->leaving) {                   /* (gone for good: the leave, nothing else) */
        leave_step(s, m, p);
        return;
    }
    if (m->tactic == T_IDLE && p->idle == T_WAIT)   /* (its first thought: one that waits lies in wait from the start) */
        take_tactic(s, m, p, T_WAIT);
    /* the target: kept while it lives and stays in reach, else the nearest enemy within the engage distance */
    if (m->target != 0xFF && (!slot_is_ship(m->target) || shipAiState[m->target] == 9 || commAnimInfo[m->target] == commAnimInfo[s]
                              || km_between(s, m->target) > (int32_t)p->engage_km * 3 / 2))
        m->target = 0xFF;
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        if (k == s || !slot_is_ship(k) || commAnimInfo[k] == commAnimInfo[s] || shipAiState[k] == 9 || (rvFlags[k] & 16))
            continue;
        d = km_between(s, k);
        if (d < bd) {
            bd = d;
            near_ = k;
        }
    }
    {
        int16_t prey = prey_of(s, m->profile);
        if (prey >= 0) {                /* (its orders: at the prey as a raider is, whoever is nearer) */
            m->target = (uint8_t)prey;
            m->raiding = (uint8_t)(prey + 1);
        } else {
            int16_t raid = raid_target(s);
            m->raiding = (uint8_t)(raid + 1);
            if (raid >= 0)
                m->target = (uint8_t)raid;
        }
    }
    if (!m->raiding && near_ >= 0 && bd <= p->engage_km && (m->target == 0xFF || bd * 3 < km_between(s, m->target) * 2))
        m->target = (uint8_t)near_;
    if (m->taunt) {                     /* taunted: the player, wherever it is and whoever is nearer (logic_taunt) */
        m->taunt--;
        if (slot_is_ship(0) && shipAiState[0] != 9)
            m->target = 0;
    }
    /* a wingman under orders (logic_order): the player's target wherever it is (ATTACK), the enemy nearest the player,
     * one on the player first (HELP), the enemy nearest it however far (BREAK), or nobody at all (FORMATION, RTB) */
    if (g_order[s] == ORDER_BREAK && is_wingman(s)) {
        if (m->target == 0xFF && near_ >= 0)
            m->target = (uint8_t)near_;
    } else if (g_order[s] != ORDER_FREE && is_wingman(s)) {
        if (g_order[s] == ORDER_ATTACK) {
            if (g_sys.target >= 0 && slot_is_ship(g_sys.target) && shipAiState[g_sys.target] != 9
                && commAnimInfo[g_sys.target] != commAnimInfo[s])
                m->target = (uint8_t)g_sys.target;
            else
                g_order[s] = ORDER_FREE;   /* (the target gone: back to its own fight) */
        } else if (g_order[s] == ORDER_HELP) {
            int16_t best = -1, on = 0;
            bd = INT32_C(0x7FFFFFFF);
            for (k = 1; k < NUM_SHIP_SLOTS; k++) {
                int16_t targets_player;
                if (k == s || !slot_is_ship(k) || commAnimInfo[k] == commAnimInfo[s] || shipAiState[k] == 9 || (rvFlags[k] & 16))
                    continue;
                targets_player = rvTarget[k] == 0;
                d = player_km(k);
                if (targets_player > on || (targets_player == on && d < bd)) {
                    bd = d;
                    best = k;
                    on = targets_player;
                }
            }
            if (best >= 0)
                m->target = (uint8_t)best;
        } else
            m->target = 0xFF;
    }
    rvTarget[s] = m->target;
    if (m->target == 0xFF) {            /* nobody to fight: the idle tactic */
        int16_t cap;
        if (((g_order[s] == ORDER_RTB && is_wingman(s)) || (role_of(s) == RV_ESCORT_STAY && g_order[s] == ORDER_FREE))
            && (cap = nearest_capital(s)) >= 0) {   /* home: round its carrier; a fighter that stays (role=stay, the
                                           carrier's patrol), round the capital ship nearest it */
            m->leader = (uint8_t)cap;
            if (m->tactic != T_ESCORT)
                take_tactic(s, m, p, T_ESCORT);
            escort_step(s, m, p);
            return;
        }
        if (g_order[s] == ORDER_BREAK && is_wingman(s) && slot_is_ship(0) && shipAiState[0] != 9) {
            m->leader = 0;              /* (out of formation, nobody to fight: on its own, round the player) */
            if (m->tactic != T_ESCORT)
                take_tactic(s, m, p, T_ESCORT);
            escort_step(s, m, p);
            return;
        }
        if (g_order[s] == ORDER_FORMATION && is_wingman(s) && slot_is_ship(0) && shipAiState[0] != 9) {
            m->leader = 0;              /* (the player's wing, whatever its idle tactic: a transport's escort too) */
            if (m->tactic != T_FORMATION)
                take_tactic(s, m, p, T_FORMATION);
            formation_step(s, m, p);
            return;
        }
        if (m->tactic != p->idle) {
            take_tactic(s, m, p, p->idle);
            m->timer = tactic_steps(s, p->idle);
            if (p->idle == T_WAIT)
                m->timer = (int16_t)(p->wait_s * LOGIC_HZ);
        }
        if (m->tactic == T_FORMATION)
            formation_step(s, m, p);
        else if (m->tactic == T_ESCORT)
            escort_step(s, m, p);
        else if (m->tactic == T_WAIT) {
            throttle(s, 0, 0);
            if (--m->timer <= 0)
                rvFlags[s] &= ~16;      /* (its patience over: it shows itself and flies on) */
        } else
            throttle(s, slot_type(s) ? slot_type(s)->cruise_kps : 200, 0);
        return;
    }
    view_of(s, m->target, (g_rule_flags & RV_RULE_AIM_CONE) ? 0 : p->lead_pct, &v);   /* (the cone rule: no lead) */
    m->passing = (uint8_t)passing(s, &v);
    if (m->tactic == T_WAIT) {          /* still until a chance: the target near and not pointing at it, or very near */
        throttle(s, 0, 0);
        if (--m->timer > 0 && !(v.km < p->pounce_km && (v.cos_its < 100 || v.km < p->pounce_km / 2)))
            return;
        rvFlags[s] &= ~16;
        take_tactic(s, m, p, T_PURSUE);
        m->burn = 1;
    }
    sit = situation(s, m, p, &v);
    if (m->tactic < T_PURSUE || m->timer <= 0)
        pick_tactic(s, m, p, sit, &v);
    (void)avoid_break(s, m, p);
    m->timer--;
    if (run_tactic(s, m, p, &v))
        pick_tactic(s, m, p, sit, &v);
}

/* a capital ship's mind for a step: its course held at cruise; its turrets at the nearest enemy in their range (each
 * that is cooled: fired by step_fire through the thunk, aimed at rvTarget by the gun's own code); a launcher at an
 * enemy capital ship now and then */
/* a slot's mission record's role (an escort target's, the home carrier's), 0 none */
static int16_t role_of(int16_t s)
{
    return slotMissionShip[s] < 64 ? MSHIP_ROLE(slotMissionShip[s]) : 0;
}

/* an escort target: a ship with a route of its own and the autopilot's company (the home carrier has neither) */
static int16_t escorted(int16_t s)
{
    int16_t r = role_of(s);
    return r == RV_ESCORT_OUT || r == RV_ESCORT_IN;
}

/* an escort target's own way (its mission record's role): out, to the mission's next nav point after its own (nav 1
 * from the launch) at its top speed, and gone (a jump) there: the old game's transport set off
 * from the carrier for Nav 1 at once, 300 km/s, and vanished there (tests/wc1/README.md); in, still at its own nav
 * point until the player reaches it or comes within ROUTE_WAKE_KM, then to the home carrier, stopping ROUTE_DOCK_KM off its hull
 * with its nose the carrier's way, home (with no carrier, to nav 0 and gone there); an enemy's stays where it is.
 * Out, it is there ROUTE_OUT_KM from the nav point.  With the mission's jump=N, it goes with the player waypoint by
 * waypoint and out only at nav N (the user, 2026-11-03: "the ox should jump at nav 2 not nav 1"): at a waypoint before
 * N it holds, ROUTE_OUT_KM off it, until the player has been there, then bears on for the next whatever is about, and
 * jumps only on coming to its last (the user, 2026-11-04: "Ideally it would bear directly for the nav point no matter
 * what and when it arrived (slowly) it would jump"; it had held while enemies were near and jumped from wherever it was
 * once they were gone).  The autopilot, carrying it, moves it on to its own next (Mind.leg) */
void (*__far logic_on_jump)(int16_t slot, int16_t friendly);

/* gone safe: its star and the player's flash (fx_jump), and it counts as away, not killed */
static void route_jump(int16_t s)
{
    if (logic_on_jump)
        logic_on_jump(s, commAnimInfo[s] == commAnimInfo[0]);
    g_away[slotMissionShip[s] & 63] = 1;
    if (commAnimInfo[s] != commAnimInfo[0])
        g_enemy_ran = 1;
    ship_clear(s);
}

static int16_t route_last(int16_t rec, int16_t first)   /* role=out's last nav point: jump=N, else the first */
{
    int16_t j = MSHIP_JUMP(rec);
    return first >= 0 && j > first && j < NAVS && nav_defined(j) ? j : first;
}

/* the autopilot's next nav point is on an escort target's way, so it comes along: an escort out's waypoints from the
 * one it makes for to its last (one in goes home with whoever goes).  Otherwise it goes on its own way (the user,
 * 2026-11-03: Enigma 2's Ox, taken along with the wing after Nav 2 before it had jumped, turned back for Nav 2 at 4
 * degrees a second, "just hung around and started flying in a random direction") */
static int16_t escort_on_way(int16_t s, int16_t next)
{
    int16_t rec = slotMissionShip[s] & 63, first = -1, n;
    if (role_of(s) != RV_ESCORT_OUT)
        return 1;
    for (n = (int16_t)(MSHIP_NAV(rec) & 15) + 1; n < NAVS && first < 0; n++)
        if (nav_defined(n))
            first = n;
    return first >= 0 && next >= first && next >= g_mind[s].leg && next <= route_last(rec, first);
}

/* an escort out's next waypoint, as the autopilot carries it: the first on its way (from its own leg on) the player
 * has not reached, else its last (on to its jump) */
static int16_t escort_next(int16_t s)
{
    int16_t rec = slotMissionShip[s] & 63, n, first = -1, last;
    for (n = (int16_t)(MSHIP_NAV(rec) & 15) + 1; n < NAVS && first < 0; n++)
        if (nav_defined(n))
            first = n;
    last = route_last(rec, first);
    for (n = g_mind[s].leg > first ? g_mind[s].leg : first; n >= 0 && n <= last; n++)
        if (nav_listed(n) && !(g_navs_seen >> n & 1))
            return n;
    return last;
}

/* an escort target of the player's side the autopilot takes along to `next` (its flyby's formation, then the jump):
 * one out to its own next waypoint, however far it is (the user, 2026-11-04, the Ox not at nav 1 on arriving there),
 * and only there: picked past it, the autopilot goes and the escort is left to fly its way alone, the old game's (its
 * Enyo 2, Nav 2 taken on the map: its Drayman left 30,500 km behind; the user's pick, "Pick anywhere, Ox left
 * behind"); one in on its way within AUTOPILOT_ESCORT_KM */
#define AUTOPILOT_ESCORT_KM 30000
static int16_t escort_comes(int16_t s, int16_t next)
{
    if (maybeEntityType[s] != ET_CAPSHIP || !escorted(s) || commAnimInfo[s] != commAnimInfo[0])
        return 0;
    if (role_of(s) == RV_ESCORT_OUT)
        return next != 0 && next == escort_next(s);
    return escort_on_way(s, next) && player_km(s) < AUTOPILOT_ESCORT_KM;
}

static void route_step(int16_t s, Mind __far *m, const AiProfile __far *p)
{
    static const char __far s_gone[] = "logic: slot %d (mission ship %d) %s at nav %d";
    static const char __far s_on[] = "logic: slot %d (mission ship %d) on from nav %d to nav %d";
    static const char __far s_dock[] = "logic: slot %d (mission ship %d) home beside the carrier (slot %d)";
    const RvShipType __far *t = slot_type(s);
    int16_t rec = slotMissionShip[s] & 63, role = MSHIP_ROLE(rec), goal = -1, last = -1, n, home = -1;
    int32_t reach = role == RV_ESCORT_OUT ? ROUTE_OUT_KM : ROUTE_JUMP_KM, top = t ? t->max_kps : 100;
    RvVec3 d, b;
    if (role == RV_ESCORT_OUT) {
        for (n = (int16_t)(MSHIP_NAV(rec) & 15) + 1; n < NAVS && goal < 0; n++)
            if (nav_defined(n))
                goal = n;
        last = route_last(rec, goal);
        if (m->leg > goal)
            goal = m->leg < last ? m->leg : last;
    } else if (commAnimInfo[s] == commAnimInfo[0]
               && (m->phase || player_km(s) < ROUTE_WAKE_KM || (g_navs_seen >> (MSHIP_NAV(rec) & 15) & 1))) {
        if (!m->phase)
            m->phase = 1;               /* (1 under way, 2 home beside the carrier) */
        goal = last = 0;
        for (n = 1; n < NUM_SHIP_SLOTS && home < 0; n++)
            if (logic_is_home(n))
                home = n;
    }
    if (goal < 0) {
        throttle(s, 0, 0);
        return;
    }
    if (home >= 0) {                    /* in, to the carrier: slowing as it comes, then beside it, stopped, its way */
        int32_t gap = gap_between(s, home), km, kps;
        if (m->phase == 2 || gap <= ROUTE_DOCK_KM) {
            const RvVec3 *F = &gOrientationFrontVector[home];
            if (m->phase != 2) {
                m->phase = 2;
                g_away[rec] = 1;        /* (delivered: what befalls it there is no loss of the player's) */
                rv_notef(s_dock, s, rec, home);
            }
            d = vec3(FIX_MUL(F->x, 1000), FIX_MUL(F->y, 1000), FIX_MUL(F->z, 1000));   /* (1,000 km the carrier's way) */
            to_body(s, &d, &b);
            steer_to(s, p, b.x, b.y, b.z);
            throttle(s, 0, 0);
            return;
        }
        d = vec3(TO_KM(gPositionVector[home].x - gPositionVector[s].x), TO_KM(gPositionVector[home].y - gPositionVector[s].y),
                 TO_KM(gPositionVector[home].z - gPositionVector[s].z));
        to_body(s, &d, &b);
        steer_to(s, p, b.x, b.y, b.z);
        km = len_of(b.x, b.y, b.z);
        kps = (gap - ROUTE_DOCK_KM) / 8;   /* (its top speed until some 800 km off its hull at 80 kps; a tenth of it at 230) */
        if (kps > top)
            kps = top;
        if (kps < top / 10 + 1)
            kps = top / 10 + 1;
        throttle(s, km && b.z * 256 / km < ROUTE_TURN_COS && kps > top / 5 ? top / 5 : kps, 0);
        return;
    }
    for (;;) {                          /* (a waypoint before the last: on to the next once the player has been there and
                                           it has come to it, or at once when the player has been on at the next) */
        d = vec3(NAV_AT(goal, 0) - TO_KM(gPositionVector[s].x), NAV_AT(goal, 1) - TO_KM(gPositionVector[s].y),
                 NAV_AT(goal, 2) - TO_KM(gPositionVector[s].z));
        if (goal == last)
            break;
        for (n = goal + 1; n < last && !nav_listed(n); n++)
            ;
        if (!(g_navs_seen >> n & 1)) {
            if (len_of(d.x, d.y, d.z) >= reach)
                break;                  /* (on its way to it) */
            if (!(g_navs_seen >> goal & 1)) {
                throttle(s, 0, 0);      /* (holding there: the player not there yet) */
                return;
            }
        }
        rv_notef(s_on, s, rec, goal, n);
        goal = n;
        m->leg = (uint8_t)n;
    }
    if (len_of(d.x, d.y, d.z) < reach) {
        rv_notef(s_gone, s, rec, role == RV_ESCORT_OUT ? "jumped out" : "home", goal);
        route_jump(s);
        return;
    }
    to_body(s, &d, &b);
    steer_to(s, p, b.x, b.y, b.z);
    {
        int32_t km = len_of(b.x, b.y, b.z);
        throttle(s, km && b.z * 256 / km < ROUTE_TURN_COS ? top / 5 : top, 0);
    }
}

/* the old game's capital ship's target this frame: one of the enemies within CAP_TURRET_KM (nearest_enemy's), by its
 * dice; -1 none.  *near_n: how many there are */
static int16_t cap_pick(int16_t s, int16_t *near_n)
{
    int16_t k, n = 0, want;
    for (want = -1; want < 1; want++) {   /* (counted, then the one picked found) */
        for (k = 0; k < NUM_SHIP_SLOTS; k++) {
            if (k == s || !slot_is_ship(k) || commAnimInfo[k] == commAnimInfo[s] || shipAiState[k] == 9 || (rvFlags[k] & 16)
                || km_between(s, k) > CAP_TURRET_KM)
                continue;
            if (want < 0)
                n++;
            else if (!n--)
                return k;
        }
        *near_n = n;
        if (!n)
            return -1;
        n = (int16_t)(cap_dice((uint16_t)s, 0xFF, 0) % (uint16_t)n);
    }
    return -1;
}

static void ship_think(int16_t s)
{
    Mind __far *m = &g_mind[s];
    const RvShipType __far *t = slot_type(s);
    int16_t near_ = nearest_enemy(s, 0), g, pick, near_n = 0;
    if (escorted(s)) {
        route_step(s, m, &k_profiles[m->profile < P_COUNT ? m->profile : P_CAPITAL]);
        if (!slot_is_ship(s))
            return;
    } else
        throttle(s, role_of(s) == RV_ESCORT_HOME ? 0 : t ? t->cruise_kps : 100, 0);   /* (the carrier holds station) */
    m->target = (uint8_t)(near_ >= 0 ? near_ : 0xFF);
    rvTarget[s] = m->target;
    if (near_ < 0)
        return;
    /* the turrets and the missiles once in CAP_TURRET_FRAMES frames of the old game's (each WC_FRAME_STEPS of ours), as
     * its capital ships' (ovr143:284F): the target one of the enemies within CAP_TURRET_KM picked at random; each turret one chance in two,
     * and bearing on it (its mount's outward line within some 75 degrees of it: here 3 in 8, the mounts being the art's
     * placeholders) or, the old game trying the others in turn, on another of them (1 - (5/8)^n in all; the shot is at
     * the pick, one target a ship a frame, as the layer replays a call); a launcher the same chance, bearing (within 60
     * degrees: 1 in 4) and one in 14.  capital_shot makes a turret's shot */
    if (frameCounter % WC_FRAME_STEPS || (uint16_t)(frameCounter / WC_FRAME_STEPS + s) % CAP_TURRET_FRAMES)
        return;
    pick = cap_pick(s, &near_n);
    if (pick >= 0) {
        m->target = (uint8_t)pick;
        rvTarget[s] = m->target;
    }
    for (g = 0; g < GUN_COUNT(s) && g < 16; g++) {
        const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
        if (!w || (GUN_STATE(s, g) & GUN_DESTROYED))
            continue;
        if (w->kind == RV_WK_TURRET) {
            int16_t k, bears = 0;
            for (k = 0; k < near_n && k < 8 && !bears; k++)
                bears = cap_dice(s, g, (uint16_t)(20 + k)) % 8 < 3;
            if (pick >= 0 && bears && !(GUN_STATE(s, g) & 0x7F) && !(cap_dice(s, g, 1) & 1)
                && !(slotMissionShip[s] < 64 && MSHIP_NO_TURRETS(slotMissionShip[s])))   /* (a mission's turrets=off) */
                m->turrets |= (uint16_t)(1u << g);
        } else if (w->kind == RV_WK_MISSILE) {
            if (pick >= 0 && (GUN_STATE(s, g) & 0x7F) && !(m->fire & FIRE_MISSILE) && !(cap_dice(s, g, 1) & 1)
                && cap_dice(s, g, 2) % 4 == 0 && cap_dice(s, g, 3) % 14 == 0) {
                m->fire |= FIRE_MISSILE;
                m->launcher = (uint8_t)g;
            }
        } else if (w->kind == RV_WK_TORPEDO) {
            int16_t cap = nearest_enemy(s, 1);
            if (cap >= 0 && (GUN_STATE(s, g) & 0x7F) && gap_between(s, cap) <= w->range_km
                && (uint16_t)(frameCounter - m->since) >= (uint16_t)(10 * LOGIC_HZ)) {
                m->since = frameCounter;
                rvTarget[s] = (uint8_t)cap;
                m->fire |= FIRE_MISSILE;
                m->launcher = (uint8_t)g;
            }
        }
    }
}

/* 4. every ship flies its step: its turn applied to its orientation (vec_turn, then ship_orthonormalise), its speed
 * toward setSpeed, gVelocityVector (positions a step: KPS_STEP), the velocity added to gPositionVector.  A puppet
 * only coasts (position += velocity) */
static void step_motion(void)
{
    uint16_t phase = frameCounter % KPS_HZ;
    int16_t ship_index;
    for (ship_index = 0; ship_index < NUM_SHIP_SLOTS; ship_index +=1){
        if (!slot_is_ship(ship_index) || rvController[ship_index] == CTL_FROZEN) {
            continue;
        }
        if (rvController[ship_index] != CTL_PUPPET) {
            step_turn(ship_index, g_turn[ship_index].x, g_turn[ship_index].y, g_turn[ship_index].roll);
            step_speed(ship_index, phase);
            reduce_slide(ship_index);
        }
        {
            const RvVec3 *velocity = &gVelocityVector[ship_index];
            RvVec3 *position = &gPositionVector[ship_index];
            position->x += velocity->x;
            position->y += velocity->y;
            position->z += velocity->z;
        }
    }
}

/* 5. the shots fly (shots_move) and hit (shot_hit: the ship and the place; do_damage through its thunk with
 * shot_source(i) as the source; shot_remove).  Backwards: a removed shot's index goes to the last one */
/* a flak burst where shot i is (the old game's ovr143:1DCF): every ship but its owner's whose hull is within FLAK_KM of
 * it hurt by 1000 / k^2, k 8 at the hull to 25 at FLAK_KM (15 points to 1), the player by three quarters of that, at
 * least 1; what comes to 1 or less does nothing.  The puff is the flight's (shots_on_flak) */
static void flak_burst(int16_t i)
{
    const Shot __far *f = shot(i);
    RvVec3 at;
    int16_t k, src = -1;
    at = vec3(f->x, f->y, f->z);
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        const RvShipType __far *t;
        int32_t d;
        uint16_t kk, q;
        if (k == f->owner || !slot_is_ship(k) || shipAiState[k] == 9 || (t = slot_type(k)) == 0)
            continue;
        d = len_of(TO_KM(gPositionVector[k].x - at.x), TO_KM(gPositionVector[k].y - at.y), TO_KM(gPositionVector[k].z - at.z))
            - (int32_t)t->radius_km;
        if (d > FLAK_KM)
            continue;
        kk = (uint16_t)(8 + (d > 0 ? d : 0) * 17 / FLAK_KM);
        q = (uint16_t)(1000 / kk / kk);
        if (k == 0)
            q = q * 3 / 4 > 1 ? (uint16_t)(q * 3 / 4) : 1;
        if (q <= 1)
            continue;
        if (src < 0)
            src = shot_source(i);
        if (k == 0 && shots_on_hit)     /* (the player's: the shield's or the hull's sound; the puff is the picture) */
            shots_on_hit(0, &at, f->owner, q);
        (void)do_damage((uint16_t)src, (uint16_t)k, q, RV_WHERE(&at));
    }
    if (shots_on_flak)
        shots_on_flak(&at);
}

static void step_shots(void)
{
    int16_t i, slot;
    RvVec3 where;
    shots_move();
    for (i = shots_count() - 1; i >= 0; i--) {
        if (shot(i)->weapon == WEAPON_FLAK) {   /* (a burst's: placed, it bursts a step later: capital_shot) */
            if (shot(i)->life <= 1) {
                flak_burst(i);
                shot_remove(i);
            }
            continue;
        }
        if (shot_hit(i, &slot, &where)) {
            (void)do_damage((uint16_t)shot_source(i), (uint16_t)slot, shot(i)->damage, RV_WHERE(&where));
            shot_remove(i);
        }
    }
}

/* 6. missiles (ship slots of ET_MISSILE: they steer like ships, home on missileTarget, burst by a proximity test) */
static void step_missiles(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (maybeEntityType[s] == ET_MISSILE)
            missile_step(s);
}

/* 7. ships into ships (collide_ships: the push that parts them, the damage both ways through do_damage) */
static void step_collisions(void)
{
    int16_t a, b;
    for (a = 0; a < NUM_SHIP_SLOTS - 1; a++) {
        if (!slot_is_ship(a))
            continue;
        for (b = a + 1; b < NUM_SHIP_SLOTS; b++) {   /* (a dying ship is still a hull: the others bump into it) */
            if (!slot_is_ship(b))
                continue;
            if (a == 0 && shipAiState[0] != 9 && logic_is_home(b))
                carrier_contact(b);     /* (the player and the home carrier: its own rule) */
            else if (collide_coarse(a, b))
                ram(a, b);
        }
    }
}

/* 8. the dying (shipAiState 9: an explosion's time, then despawn) and the player's end (missionStatus 4) */
static void step_deaths(void)
{
    int16_t s;
    for (s = 0; s < NUM_SHIP_SLOTS; s++) {
        if (!slot_is_ship(s) || shipAiState[s] != 9) {
            g_died[s] = 0;
            continue;
        }
        if (!g_died[s]) {               /* seen now: its end, through the thunk (the layer hears of a kill as of the old
                                           game's; on a guest the host's word starts it), whose body starts its explosion
                                           (fx.c shows it); it drifts to a stop */
            delayedDespawn(g_killer[s], (uint16_t)s);
            continue;
        }
        if ((uint16_t)(frameCounter - g_died[s]) < (maybeEntityType[s] == ET_CAPSHIP ? DEATH_STEPS_CAPITAL : DEATH_STEPS))
            continue;
        if (s == 0)                     /* the player's end: the flight ends at the loop's next check */
            missionStatus = 4;
        else {
            if (role_of(s) == RV_ESCORT_HOME && !missionStatus)   /* (the carrier's end is the player's, the user's ask) */
                missionStatus = MISSION_CARRIER_LOST;
            despawn((uint16_t)s);
        }
    }
}

/* 0. who is in the scene.  The old game took a ship out when it was 16,000 km from the player, and its nav points were
 * far enough apart that the last one's ships were gone before the next one's came (the user: "the previous nav point
 * would leave the scene"; our map is its own since 2026-11-04).  A capital ship or a friendly fighter that far off is
 * held: its slot keeps it just as it is, but it is no ship to anything (no target, no radar, nothing to run into), and
 * it is back when the player is.  An enemy fighter stays, since it can fly after the player; so does the wing, and a
 * ship the player is escorting, which has its own way to go; and so do the ships of the nav point the player came to
 * last, and the launch's on the way home: the player arrives at a point's reach, 10,000 to 30,000 km out, and what
 * it came for is in the scene, however far (the user, 2026-11-04: "I went to nav 2 exactly ... and did not see the
 * destroyer": the small map's scene of 7,200 km had held it 8,100 km off the autopilot's stop) */
#define SCENE_KM 16000
#define SCENE_BACK_KM 15500             /* (a little nearer, so a ship on the line does not flicker) */
static uint8_t __far g_off_type[NUM_SHIP_SLOTS];
uint8_t __far logic_scene_off;          /* (the play tests': most of them set a scene with the player well away from it) */

/* the ships people fly on other machines (RV_FLAG_HUMAN: the layer marks them in a shared flight), noted for the step */
static void humans_note(void)
{
    int16_t s;
    g_nhumans = 0;
    for (s = 1; s < NUM_SHIP_SLOTS && g_nhumans < HUMANS_MAX; s++)
        if ((rvFlags[s] & RV_FLAG_HUMAN) && slot_is_ship(s))
            g_humans[g_nhumans++] = (int8_t)s;
}

/* km from the nearest human, roughly (player_km's measure): the local pilot, or one of the noted (humans_note) */
static int32_t human_km(int16_t s)
{
    int32_t best = player_km(s), km;
    int16_t k;
    for (k = 0; k < g_nhumans; k++)
        if (g_humans[k] != s && (km = km_between(s, g_humans[k])) < best)
            best = km;
    return best;
}

/* a ship of the nav point the player came to last (not the launch: the player leaves it), or of the launch with
 * every waypoint reached, the way home: in the scene however far it is */
static int16_t nav_point_here(int16_t s)
{
    int16_t n;
    if (slotMissionShip[s] >= 64)
        return 0;
    n = (int16_t)(MSHIP_NAV(slotMissionShip[s]) & 15);
    return n == 0 ? nav_next_unseen() == 0 : n == (int16_t)currentNavPoint;
}

/* (by the nearest human: alone, the local pilot; in a shared flight a ship near any pilot stays in the scene on every
 * machine, and a human's own ship is never held) */
static void step_scene(void)
{
    int16_t s, friendly;
    if (!slot_is_ship(0) || logic_scene_off)
        return;
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {
        if (maybeEntityType[s] == ET_OFFSCENE) {
            if (human_km(s) < SCENE_BACK_KM || nav_point_here(s))
                maybeEntityType[s] = g_off_type[s];
            continue;
        }
        if (!slot_is_ship(s) || shipAiState[s] == 9 || (rvFlags[s] & RV_FLAG_HUMAN) || human_km(s) <= SCENE_KM
            || nav_point_here(s))
            continue;
        friendly = commAnimInfo[s] == commAnimInfo[0];
        if (maybeEntityType[s] == ET_CAPSHIP ? (friendly && escorted(s))
            : (!friendly || (slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4)))
            continue;
        g_off_type[s] = maybeEntityType[s];
        maybeEntityType[s] = ET_OFFSCENE;
        if (g_sys.target == s)
            g_sys.target = -1;
    }
}

/* A second wave waits for the first (the user, 2026-11-01: "Second waves should definitely wait until the first wave is
 * dead.  If the last enemy runs, then they can avoid spawning").  A hidden trigger point with enemies on it, the player
 * in reach, holds them while any enemy fighter is still out (a capital ship does not hold a wave: it may never die, and
 * fighters coming to its aid are what it is there for).  When the last one is gone they come; if that last one ran, they
 * are called off, unless the wave has a capital ship or an escort target in it, which the mission may turn on. */
static int16_t enemy_fighters_out(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) && shipAiState[s] != 9 && commAnimInfo[s] != commAnimInfo[0]
            && gInstanceShipTypes[s] < 0x20)
            return 1;
    return 0;
}

/* a test's way to have an enemy leave, as the fear's leave or a jump out does */
void logic_test_leave(int16_t s)
{
    if (slotMissionShip[s] < 64)
        g_away[slotMissionShip[s]] = 1;
    if (commAnimInfo[s] != commAnimInfo[0])
        g_enemy_ran = 1;
    ship_clear(s);
}

static int16_t wave_may_come(int16_t n)
{
    static const char __far s_off[] = "logic: the last enemy ran: the wave at nav %d called off";
    static const char __far s_held[] = "logic: the wave at nav %d waits for the enemies out to be gone";
    const uint16_t *at = NAV_SHIPS(n);
    int16_t j, enemies = 0, stays = 0;
    if (n == 0 || !NAV_HIDDEN(n))
        return 1;
    for (j = 0; j < 10 && at[j] != 0xFFFF; j++)
        if (MSHIP_SIDE(at[j]) != commAnimInfo[0]) {
            enemies++;
            if (MSHIP_ROLE(at[j]) || *(const uint16_t *)MSHIP(at[j]) >= 0x20)
                stays = 1;
        } else {
            stays = 1;                  /* (a friend on it comes, whoever is out) */
        }
    if (!enemies)
        return 1;
    if (enemy_fighters_out()) {
        if (!(g_wave_held >> n & 1))
            rv_notef(s_held, n);
        g_wave_held |= 1u << n;
        return 0;
    }
    /* a hidden point at the launch itself, with nobody out, is the way home's encounter: the old game's `.Encounter` at
     * the carrier.  Not at the launch: a wave has to have waited there, or the player to have flown a nav point first */
    if (!(g_wave_held >> n & 1) && g_navs_seen == 1
        && len_of(NAV_AT(n, 0), NAV_AT(n, 1), NAV_AT(n, 2)) < LAUNCH_NEAR_KM)
        return 0;
    if ((g_wave_held >> n & 1) && g_enemy_ran && !stays) {
        rv_notef(s_off, n);
        for (j = 0; j < 10 && at[j] != 0xFFFF; j++)
            g_away[at[j] & 63] = 1;     /* (as ships that left: nothing asked of the player) */
        g_navs_seen |= 1u << n;
        return 0;
    }
    return 1;
}

/* 9. the mission: nav points reached (enterNavPoint), waves, objectives, the score (victoryPoints, nonzero won).
 * A kill is the mission ship seen dying; the score is the mission done, as it stands (logic_end has the last word) */
static void step_mission(void)
{
    int16_t s, n;
    land_near();
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) && shipAiState[s] == 9 && slotMissionShip[s] < 64) {
            if (!g_killed[slotMissionShip[s]] && commAnimInfo[s] != commAnimInfo[0])
                g_enemy_ran = 0;
            if (!g_killed[slotMissionShip[s]])
                g_killed[slotMissionShip[s]] = (uint8_t)(rvFlags[s] & RV_FLAG_HUMAN ? 2 : 1);
        }
    if (slot_is_ship(0) && shipAiState[0] != 9)
        for (n = 0; n < NAVS; n++) {
            if (!nav_defined(n) || (g_navs_seen >> n & 1 && (g_navs_been >> n & 1 || !nav_listed(n))) || !nav_in_reach(n))
                continue;
            if (nav_listed(n))          /* (a waypoint's: a hidden trigger point is no place been) */
                g_navs_been |= 1u << n;
            if (!(g_navs_seen >> n & 1) && wave_may_come(n))
                enterNavPoint((uint16_t)n);
        }
    victoryPoints = (uint16_t)mission_done();
}

/* --- over the view: a band of rows at a time (hud.h's calls are clipped to it and to the opening) --- */

#define HUD_BRACKET_MIN 4               /* brackets no tighter than this about the target's middle (px): far off, its
                                           box is a dot */

/* the chosen nav point on the view: a white cross where it lies, and when it is off the view a smaller one at the rim
 * in its direction, with a tick pointing out (the user: "I still don't see the cross"): 1 and 2 in g_hud.nav */
#define NAV_PIN_RX 150                  /* the rim: well inside every cockpit's opening, less room below the sight */
#define NAV_PIN_UP 100
#define NAV_PIN_DOWN 50
#define NAV_PIN_TICK 12

static void nav_mark(const RvCamera *cam)
{
    int32_t rx, ry, rz, X, Y, Z, len;
    rx = ((int32_t)NAV_AT(g_sys.nav, 0) * KM - cam->pos.x) >> 4;
    ry = ((int32_t)NAV_AT(g_sys.nav, 1) * KM - cam->pos.y) >> 4;
    rz = ((int32_t)NAV_AT(g_sys.nav, 2) * KM - cam->pos.z) >> 4;
    while (labs(rx) > 0x7FFFFF || labs(ry) > 0x7FFFFF || labs(rz) > 0x7FFFFF) {
        rx /= 2;
        ry /= 2;
        rz /= 2;
    }
    X = (rx * cam->b.right.x + ry * cam->b.right.y + rz * cam->b.right.z) / 256;
    Y = (rx * cam->b.up.x + ry * cam->b.up.y + rz * cam->b.up.z) / 256;
    Z = (rx * cam->b.front.x + ry * cam->b.front.y + rz * cam->b.front.z) / 256;
    if (Z >= 16) {
        int32_t sx = cam->cx + X * cam->focal / Z, sy = cam->cy - Y * cam->focal / Z;
        if (sx >= cam->vx0 && sx < cam->vx1 && sy >= cam->vy0 && sy < cam->vy1
            && labs(sx - cam->cx) <= NAV_PIN_RX && sy >= cam->cy - NAV_PIN_UP && sy <= cam->cy + NAV_PIN_DOWN) {
            g_hud.nav = 1;
            g_hud.nav_x = (int16_t)sx;
            g_hud.nav_y = (int16_t)sy;
            return;
        }
    }
    while (labs(X) > 30000L || labs(Y) > 30000L) {
        X /= 2;
        Y /= 2;
    }
    if (!X && !Y)
        Y = 1;                          /* (dead astern: up is as good a way round as any) */
    len = (int32_t)lf_sqrt((uint32_t)(X * X + Y * Y));
    if (!len)
        len = 1;
    g_hud.nav = 2;
    g_hud.nav_x = (int16_t)(cam->cx + X * NAV_PIN_RX / len);
    g_hud.nav_y = (int16_t)(cam->cy - Y * (Y > 0 ? NAV_PIN_UP : NAV_PIN_DOWN) / len);
    g_hud.nav_dx = (int16_t)(X * NAV_PIN_TICK / len);
    g_hud.nav_dy = (int16_t)(-Y * NAV_PIN_TICK / len);
}

int16_t logic_nav_mark(int16_t *x, int16_t *y)
{
    *x = g_hud.nav_x;
    *y = g_hud.nav_y;
    return g_hud.nav;
}

static void hud_marks(void)
{
    RvCamera cam;
    EntBox b;
    int16_t half;
    flight_camera(&cam);
    g_hud.cx = cam.cx;
    g_hud.cy = cam.cy;
    g_hud.arm = (int16_t)((cam.vx1 - cam.vx0) >> 6);   /* (10 px at 640 across, 5 at 320) */
    if (!g_nav_chosen)
        g_sys.nav = (uint8_t)nav_next_unseen();
    if (NAV_HIDDEN(g_sys.nav))          /* (the simulator: nowhere to go, no cross) */
        g_hud.nav = 0;
    else
        nav_mark(&cam);
    g_hud.target = g_sys.target >= 0 && flight_ent_box(g_sys.target, &cam, &b) && b.visible;
    if (!g_hud.target)
        return;
    {   /* the tracking hurt: the brackets wander, and heavy they come and go (no dice here: the marks are not play) */
        int16_t lv = sys_level(SYS_TRACK);
        if (lv >= 3 && (frameCounter & 12) == 12) {
            g_hud.target = 0;
            return;
        }
        if (lv) {
            int16_t a = (int16_t)(1 << (lv - 1)), wx = (int16_t)((frameCounter * 7) & (2 * a - 1)), wy = (int16_t)((frameCounter * 13) & (2 * a - 1));
            b.sx = (int16_t)(b.sx + wx - a / 2);
            b.sy = (int16_t)(b.sy + wy - a / 2);
        }
    }
    half = b.half > HUD_BRACKET_MIN ? b.half : HUD_BRACKET_MIN;   /* (b.half: steady as it turns) */
    g_hud.x0 = (int16_t)(b.sx - half);
    g_hud.y0 = (int16_t)(b.sy - half);
    g_hud.x1 = (int16_t)(b.sx + half + 1);
    g_hud.y1 = (int16_t)(b.sy + half + 1);
    g_hud.colour = sys_level(SYS_IFF) >= SYS_WRECKED ? UI_GREY5 : commAnimInfo[g_sys.target] == commAnimInfo[0] ? UI_HILITE : UI_RED;
    /* the lock's reticle (the user's Top Gun display): a diamond closing on the target as the lock builds, from four
     * halves out to the brackets; locked, a second one tight about it */
    g_hud.sx = b.sx;
    g_hud.sy = b.sy;
    g_hud.half = half;
    g_hud.lock = 0;
    if (g_sys.lock_kind != RV_LOCK_NONE && g_sys.lock_kind != RV_LOCK_PROXIMITY && (g_sys.lock || g_sys.lock_pct)) {
        g_hud.lock = (int16_t)(g_sys.lock ? 2 : 1);
        g_hud.lock_r = (int16_t)(half + 3 + (int32_t)(100 - g_sys.lock_pct) * (3 * half + 12) / 100);
    }
}

void logic_frame(void)
{
    humans_note();
    step_scene();
    step_timers();
    damage_step();
    step_minds();
    step_fire();
    step_motion();
    form_hold();
    step_shots();
    step_missiles();
    step_collisions();
    step_deaths();
    step_mission();
    hud_marks();
}

/* the gunsight at the view's middle, and brackets round the target (red an enemy, cyan a friend) */
void logic_hud(void)
{
    if (!g_hud.arm)
        return;
    hud_cross(g_hud.cx, g_hud.cy, g_hud.arm, UI_AMBER2);
    if (g_hud.nav == 1)
        hud_cross(g_hud.nav_x, g_hud.nav_y, g_hud.arm, UI_WHITE);
    else if (g_hud.nav == 2) {
        hud_cross(g_hud.nav_x, g_hud.nav_y, (int16_t)(g_hud.arm / 2), UI_WHITE);
        hud_line(g_hud.nav_x, g_hud.nav_y, (int16_t)(g_hud.nav_x + g_hud.nav_dx), (int16_t)(g_hud.nav_y + g_hud.nav_dy), UI_WHITE);
    }
    if (g_hud.target)
        hud_brackets(g_hud.x0, g_hud.y0, g_hud.x1, g_hud.y1, g_hud.colour);
    if (g_hud.target && g_hud.lock == 1)
        hud_diamond(g_hud.sx, g_hud.sy, g_hud.lock_r, UI_AMBER2);
    if (g_hud.target && g_hud.lock == 2) {
        hud_diamond(g_hud.sx, g_hud.sy, (int16_t)(g_hud.half + 3), UI_WHITE);
        hud_diamond(g_hud.sx, g_hud.sy, (int16_t)(g_hud.half + 4), UI_WHITE);
    }
}

/* the final score: won when the player landed (missionStatus 1) home (at nav 0) with the mission done; dead,
 * ejected, landed elsewhere or with it undone, lost */
void logic_end(void)
{
    /* (home: at the launch point, or on the home carrier: in a shared flight the pilot who landed may be another,
     * whose landing the layer gives this game: landedOn) */
    int16_t home = landedOn != 0xFFFF && logic_is_home((int16_t)landedOn);
    victoryPoints = (uint16_t)(missionStatus == 1 && mission_done() && (home || nav_in_reach(0)));
}

#define AUTOPILOT_CLEAR_KM 16000        /* no enemy this near (the engine's autopilot lamp says the same: flight.c): a wave
                                           coming in from WAVE_KM holds it, as the old game's scene did (16,000) */
/* (AUTOPILOT_ESCORT_KM: an escort target of the player's side this near comes along, AUTOPILOT_BEHIND_KM behind the
 * player, one out however far: escort_comes.  The old game's transport came along, 1,800 km behind) */
#define AUTOPILOT_BEHIND_KM 800
#define AUTOPILOT_WING_KM 10000         /* the friendly fighters this near come along, keeping their places */
#define AUTOPILOT_WING_SIDE_KM 1500     /* the mission's wingman, farther than that, comes along this far to the right */

/* the autopilot key (A), between two steps: with no enemy near, the player (and the wing) taken on to the next nav
 * point, as the old games cut to the arrival; 1 when it went, 0 when it may not.  (On a multiplayer client the layer
 * steps over the key's call: the host's autopilot moves everyone).  The next nav point is the first not reached
 * yet, home (nav 0) when all are; the player arrives at its reach (nav_reach), facing it */
#define AUTOPILOT_FIELD_KM 500          /* it stops this far short of a field's edge */
#define AUTOPILOT_REGION_IN_KM 500      /* and this far inside the reach of a trigger point on its way, so it goes off */
#define AUTOPILOT_MIN_KM 1000           /* a field nearer than this along the way: it will not go (the old game's: not in
                                           a field, and it stops at the edge; the player flies through) */

/* a line from the origin along d, len long (16 km units), against a sphere about c of radius r: how far along it
 * first comes inside, -1 never (0: it starts inside) */
static int32_t line_enters(const int32_t d[3], int32_t len, int32_t cx, int32_t cy, int32_t cz, int32_t r)
{
    int32_t u = (cx * d[0] + cy * d[1] + cz * d[2]) / len, perp = cx * cx + cy * cy + cz * cz - u * u, back;
    if (perp >= r * r)
        return -1;
    back = (int32_t)lf_sqrt((uint32_t)(r * r - perp));
    if (u + back < 0 || u - back > len)
        return -1;
    return u - back < 0 ? 0 : u - back;
}

/* a nav point with someone still to come at it */
static int16_t nav_has_ships(int16_t n)
{
    const uint16_t *at = NAV_SHIPS(n);
    int16_t j;
    for (j = 0; j < 10 && at[j] != 0xFFFF; j++)
        if (!g_killed[at[j] & 63] && !g_away[at[j] & 63])
            return 1;
    return 0;
}

/* the way to nav point `next`, traced as the old game's autopilot does (the user: "raytrace the path along the line to
 * see if enemy spawn regions or asteroid fields are in the way"): how far the player can be taken, km; *region the
 * trigger point or waypoint not yet reached that it stops at (-1 none), *field 1 when a field stops it.  Neither: all
 * the way, to the nav point's reach (nav_stop: for a point in a field, into the field) */
static int32_t way_stop(int16_t next, int16_t *region, int16_t *field)
{
    int32_t px = TO_KM(gPositionVector[0].x), py = TO_KM(gPositionVector[0].y), pz = TO_KM(gPositionVector[0].z), d[3], len, best, e;
    int16_t i, own = nav_field(next);
    d[0] = (NAV_AT(next, 0) - px) >> 4;
    d[1] = (NAV_AT(next, 1) - py) >> 4;
    d[2] = (NAV_AT(next, 2) - pz) >> 4;
    len = (int32_t)lf_sqrt((uint32_t)(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    if (!len)
        len = 1;
    best = len * 16 - nav_stop(next);
    *region = -1;
    *field = 0;
    for (i = 0; i < g_nfields; i++) {
        if (i == own)                   /* (the point's own field: it goes in, nav_stop) */
            continue;
        e = line_enters(d, len, (g_fields[i].x - px) >> 4, (g_fields[i].y - py) >> 4, (g_fields[i].z - pz) >> 4,
                        ((int32_t)g_fields[i].radius + AUTOPILOT_FIELD_KM) >> 4);
        if (e >= 0 && e * 16 < best) {
            best = e * 16;
            *field = 1;
        }
    }
    for (i = 1; i < NAVS; i++) {         /* (not one the player is in already: its wave waits for the one out, or for a
                                           nav point flown; SHELTON2's second wave is a point at the launch 32,500 km
                                           across, and the autopilot would go 500 km a press) */
        if (i == next || !nav_defined(i) || (g_navs_seen >> i & 1) || !nav_has_ships(i) || nav_in_reach(i))
            continue;
        e = line_enters(d, len, (NAV_AT(i, 0) - px) >> 4, (NAV_AT(i, 1) - py) >> 4, (NAV_AT(i, 2) - pz) >> 4,
                        nav_reach(i) >> 4);
        if (e >= 0 && e * 16 + AUTOPILOT_REGION_IN_KM < best) {
            best = e * 16 + AUTOPILOT_REGION_IN_KM;
            *region = i;
            *field = 0;
        }
    }
    return best;
}

/* the autopilot may go: the player alive, no enemy within AUTOPILOT_CLEAR_KM, a nav point to go to (the one chosen, else
 * the next not reached, else home) not yet reached, and no field at hand: that nav point, or -1 (the engine asks before
 * its flyby, flight.c) */
int16_t logic_autopilot_ready(void)
{
    int16_t s, next;
    if (!slot_is_ship(0) || shipAiState[0] == 9 || sys_level(SYS_NAV) >= SYS_WRECKED)
        return -1;                      /* (the nav computer wrecked: the pilot flies home by hand, the cross still shows) */
    humans_note();
    for (s = 1; s < NUM_SHIP_SLOTS; s++)   /* (near any human: a wingman in a fight on another machine holds it too) */
        if (slot_is_ship(s) && commAnimInfo[s] != commAnimInfo[0] && shipAiState[s] != 9
            && human_km(s) < AUTOPILOT_CLEAR_KM)
            return -1;
    /* (the one N picked, else the way ahead: a patrol in any order, as the old game's map lets one (its Enter takes the
     * pick: traced 2026-11-04, Enyo 1's autopilot to Nav 2 first), an escort mission too: the escort comes along only
     * to its own next waypoint (escort_comes); the user: "ok can you make the auto system like WC1 in that respect") */
    next = g_nav_chosen ? g_sys.nav : nav_next_unseen();
    if (nav_in_reach(next))
        return -1;                      /* (there already) */
    {
        int16_t region, field;
        int32_t stop = way_stop(next, &region, &field);
        if (field && stop < AUTOPILOT_MIN_KM)
            return -1;
    }
    return next;
}

static void spawn_apart(int16_t s);

/* --- the autopilot's formation (the user, 2026-11-03: "when I hit auto can you make the wingman and any escort ships
 * automatically form up with the leader and align the velocities so that they look like they're flying in a
 * cluster. Right now it looks like the leader wanders off") ---
 *
 * At the flyby's start (flight.c, the machine whose autopilot it is) the player is turned to the nav point it goes to
 * (the jump turned it there anyway; the cut to the outside camera hides the turn), and the ships the jump takes along
 * that this machine's AI flies close up on it: the mission's wingman 350 km off the right wing and 250 back, the next
 * fighter as far off the left, the rest stepped back to the right (an echelon); an escort target below them and out to
 * the right, its hull clear of theirs.  Each step of the flyby they are held there, the player's heading and velocity theirs, so the
 * cluster flies as one; the jump moves them as it moves the player and they arrive in their places.  A human
 * wingman's ship is its own machine's to fly: it is put in its place once, as the flyby begins, flying the player's
 * way at the player's speed, and the frame that starts the flyby on its machine takes it there (the layer applies a
 * pilot's own place from a frame with the autopilot in it); its stick is held through the flyby (flight.c), so it
 * flies on beside them (the user, 2026-11-03: "player 2 doesn't get the same forcing function for auto somehow and
 * can't see the leader") */
#define FORM_ACROSS_KM 350              /* a fighter's place off the one ahead of it: across */
#define FORM_BACK_KM 250                /* and back */
#define FORM_BELOW_KM 200               /* an escort target's hull this far under the fighters', */
#define FORM_OUT_KM 700                 /* and its near side this far to the right of the player (out from the flyby's camera,
                                           on the left: nearer, a transport went off the bottom of the picture) */

static int16_t form_comes(int16_t s, int16_t next)   /* a ship the jump to `next` takes along: 1 a fighter this
                                                         machine's AI flies, 2 an escort target, 3 a human's fighter */
{
    if (!slot_is_ship(s) || commAnimInfo[s] != commAnimInfo[0] || shipAiState[s] == 9)
        return 0;
    if (rvFlags[s] & RV_FLAG_HUMAN)
        return maybeEntityType[s] == ET_SHIP ? 3 : 0;
    if (rvController[s] != CTL_AI || role_of(s) == RV_ESCORT_STAY)   /* (one that stays: left where it is) */
        return 0;
    if (maybeEntityType[s] == ET_SHIP && (player_km(s) < AUTOPILOT_WING_KM
                                          || (slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4)))
        return 1;
    if (escort_comes(s, next))
        return 2;
    return 0;
}

static void form_place(int16_t s)       /* slot s at its place off the player, flying the player's way and speed */
{
    gPositionVector[s] = off_ship(0, g_form_off[s]);
    gOrientationFrontVector[s] = gOrientationFrontVector[0];
    gOrientationRightVector[s] = gOrientationRightVector[0];
    gOrientationUpVector[s] = gOrientationUpVector[0];
    gVelocityVector[s] = gVelocityVector[0];
    speedLo[s] = speedLo[0];
}

static void form_hold(void)
{
    int16_t s;
    if (!g_form_steps)
        return;
    g_form_steps--;
    if (!slot_is_ship(0) || shipAiState[0] == 9) {
        g_formed = 0;
        g_form_steps = 0;
        return;
    }
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {
        if (!(g_formed >> s & 1))
            continue;
        if (!slot_is_ship(s) || shipAiState[s] == 9 || rvController[s] != CTL_AI) {
            g_formed &= ~((uint32_t)1 << s);   /* (gone, dying or jumped out: out of it) */
            continue;
        }
        form_place(s);
    }
}

/* the player turned to nav point `next` (its yaw and pitch out) */
/* the player turned to face a place (km) */
static void face_km(int32_t x, int32_t y, int32_t z, int16_t *yaw, int16_t *pitch)
{
    RvVec3 d = vec3(x - TO_KM(gPositionVector[0].x), y - TO_KM(gPositionVector[0].y), z - TO_KM(gPositionVector[0].z));
    int32_t horiz = (int32_t)lf_sqrt((uint32_t)((d.x >> 4) * (d.x >> 4) + (d.z >> 4) * (d.z >> 4))) << 4;
    *yaw = lf_atan2(d.x, d.z);
    *pitch = lf_atan2(d.y, horiz);
    ship_face(0, *yaw, *pitch);
}

/* the home carrier's slot, in the scene or held off it (far off, the scene holds it: step_scene), -1 none */
static int16_t home_slot(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if ((slot_is_ship(s) || maybeEntityType[s] == ET_OFFSCENE) && shipAiState[s] != 9 && role_of(s) == RV_ESCORT_HOME)
            return s;
    return -1;
}

static void face_nav(int16_t next, int16_t *yaw, int16_t *pitch)
{
    RvVec3 d = vec3(NAV_AT(next, 0) - TO_KM(gPositionVector[0].x), NAV_AT(next, 1) - TO_KM(gPositionVector[0].y),
                    NAV_AT(next, 2) - TO_KM(gPositionVector[0].z));
    int32_t horiz = (int32_t)lf_sqrt((uint32_t)((d.x >> 4) * (d.x >> 4) + (d.z >> 4) * (d.z >> 4))) << 4;
    *yaw = lf_atan2(d.x, d.z);
    *pitch = lf_atan2(d.y, horiz);
    ship_face(0, *yaw, *pitch);
}

void logic_form_up(int16_t steps)
{
    int32_t top = TO_KM(collide_half(0));
    int16_t s, next, pass, k = 0, e = 0, h = 0, at, yaw, pitch;
    g_formed = 0;
    g_form_steps = 0;
    if ((next = logic_autopilot_ready()) < 0)
        return;
    face_nav(next, &yaw, &pitch);
    fly_ahead(0);
    /* the fighters: the mission's wingman first, then the others by slot */
    for (pass = 0; pass < 2; pass++)
        for (s = 1; s < NUM_SHIP_SLOTS; s++) {
            int16_t wing = slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4, __far *o = g_form_off[s];
            int16_t kind = form_comes(s, next);
            if ((kind != 1 && kind != 3) || (g_formed >> s & 1) || (pass == 0) != (wing != 0))
                continue;
            o[0] = (int16_t)(k == 1 ? -FORM_ACROSS_KM : FORM_ACROSS_KM * (k ? k : 1));
            o[1] = (int16_t)(k < 2 ? 0 : (k & 1) ? -60 : 60);
            o[2] = (int16_t)(-FORM_BACK_KM * (k ? k : 1));
            if (kind == 3) {            /* (a human's: there once, its throttle at the player's speed; not held) */
                form_place(s);
                setSpeed[s] = speedLo[0];
                h++;
            } else
                g_formed |= (uint32_t)1 << s;
            k++;
        }
    /* the escort targets: under them and out to the right, side by side (each one's half box clear of the fighters'
     * by FORM_BELOW_KM, and of the one before it) */
    at = FORM_OUT_KM;
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {
        int16_t __far *o = g_form_off[s];
        int32_t half;
        if (form_comes(s, next) != 2)
            continue;
        half = TO_KM(collide_half(s));
        o[0] = (int16_t)clamp32(at + half, -6000, 6000);
        o[1] = (int16_t)clamp32(-(top + FORM_BELOW_KM + half), -6000, 6000);
        o[2] = (int16_t)clamp32(-FORM_BACK_KM - half / 2, -6000, 6000);
        at = (int16_t)clamp32(at + 2 * half + FORM_ACROSS_KM, 0, 6000);
        g_formed |= (uint32_t)1 << s;
        e++;
    }
    g_form_steps = steps;
    form_hold();                        /* (in their places for the flyby's first picture) */
    RV_NOTE("logic: the autopilot's formation: %d fighters (%d of them humans'), %d escort targets", k, h, e);
}

int16_t logic_autopilot(void)
{
    RvVec3 to;
    int32_t stop;
    uint32_t moved = 0;
    int16_t s, next, yaw, pitch, region, field, home;
    if ((next = logic_autopilot_ready()) < 0)
        return 0;
    if (g_nav_chosen && g_sys.nav != next)
        g_nav_chosen = 0;               /* (a later waypoint picked: the autopilot goes in order, the cross with it) */
    stop = way_stop(next, &region, &field);
    /* (the camera's start was the engine's, at its flyby: flight.c; the jump's end goes through autopilotFinished below:
     * in a shared flight the layer replays it, and every pilot's ship goes where the host's game puts it) */
    face_nav(next, &yaw, &pitch);
    home = next == 0 && region < 0 && !field ? home_slot() : -1;
    if (region >= 0 || field)
        to = vec3((TO_KM(gPositionVector[0].x) + FIX_MUL(gOrientationFrontVector[0].x, stop)) * KM,
                  (TO_KM(gPositionVector[0].y) + FIX_MUL(gOrientationFrontVector[0].y, stop)) * KM,
                  (TO_KM(gPositionVector[0].z) + FIX_MUL(gOrientationFrontVector[0].z, stop)) * KM);
    else if (home >= 0) {               /* home: off the carrier's hull, facing it (HOME_ARRIVE_KM) */
        const RvShipType __far *ht = slot_type(home);   /* (its radius: collide_half has none for a ship held off) */
        int32_t back = HOME_ARRIVE_KM + (ht ? (int32_t)ht->radius_km : 0);
        face_km(TO_KM(gPositionVector[home].x), TO_KM(gPositionVector[home].y), TO_KM(gPositionVector[home].z), &yaw, &pitch);
        to = vec3(gPositionVector[home].x - FIX_MUL(gOrientationFrontVector[0].x, back) * KM,
                  gPositionVector[home].y - FIX_MUL(gOrientationFrontVector[0].y, back) * KM,
                  gPositionVector[home].z - FIX_MUL(gOrientationFrontVector[0].z, back) * KM);
    } else
        to = vec3((NAV_AT(next, 0) - FIX_MUL(gOrientationFrontVector[0].x, nav_stop(next))) * KM,
                  (NAV_AT(next, 1) - FIX_MUL(gOrientationFrontVector[0].y, nav_stop(next))) * KM,
                  (NAV_AT(next, 2) - FIX_MUL(gOrientationFrontVector[0].z, nav_stop(next))) * KM);
    /* the wing: moved as the player is, facing the same way (player_km while the player is still where it was); an
     * escort target of the player's side put behind it */
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {   /* (a human wingman, on another machine, always comes: the wing is the wing) */
        if (!slot_is_ship(s) || commAnimInfo[s] != commAnimInfo[0] || shipAiState[s] == 9
            || (rvController[s] != CTL_AI && !(rvFlags[s] & RV_FLAG_HUMAN))
            || (role_of(s) == RV_ESCORT_STAY && !(rvFlags[s] & RV_FLAG_HUMAN)))   /* (the carrier's patrol stays: the
                                           user, 2026-11-05: "missions have to be able to differentiate between
                                           autopilotees and stay-homers"; a human flying one comes, as any human) */
            continue;
        if (maybeEntityType[s] == ET_SHIP && (player_km(s) < AUTOPILOT_WING_KM || (rvFlags[s] & RV_FLAG_HUMAN))) {
            gPositionVector[s].x += to.x - gPositionVector[0].x;
            gPositionVector[s].y += to.y - gPositionVector[0].y;
            gPositionVector[s].z += to.z - gPositionVector[0].z;
        } else if (maybeEntityType[s] == ET_SHIP && slotMissionShip[s] < 64 && MSHIP_BEHAVIOUR(slotMissionShip[s]) == 4) {
            /* the mission's wingman, however far it had gone, comes along beside the player (the user, 2026-11-03:
             * "sometimes my wingmate doesn't auto with me and I just go it alone"; it was left beyond 10,000 km) */
            const RvVec3 *R = &gOrientationRightVector[0];
            gPositionVector[s] = vec3(to.x + FIX_MUL(R->x, AUTOPILOT_WING_SIDE_KM) * KM,
                                      to.y + FIX_MUL(R->y, AUTOPILOT_WING_SIDE_KM) * KM,
                                      to.z + FIX_MUL(R->z, AUTOPILOT_WING_SIDE_KM) * KM);
        } else if (escort_comes(s, next)) {
            if (g_formed >> s & 1) {    /* (in the flyby's formation: it keeps its place there) */
                gPositionVector[s].x += to.x - gPositionVector[0].x;
                gPositionVector[s].y += to.y - gPositionVector[0].y;
                gPositionVector[s].z += to.z - gPositionVector[0].z;
            } else
                gPositionVector[s] = vec3(to.x - FIX_MUL(gOrientationFrontVector[0].x, AUTOPILOT_BEHIND_KM) * KM,
                                          to.y - FIX_MUL(gOrientationFrontVector[0].y, AUTOPILOT_BEHIND_KM) * KM,
                                          to.z - FIX_MUL(gOrientationFrontVector[0].z, AUTOPILOT_BEHIND_KM) * KM);
            if (role_of(s) == RV_ESCORT_OUT && next > g_mind[s].leg)
                g_mind[s].leg = (uint8_t)next;   /* (carried to the next waypoint: that is its way now; route_step) */
            {
                const RvShipType __far *et = slot_type(s);
                if (et && speedLo[s] > et->max_kps)   /* (the flyby lent it the player's speed: the Ox left the jump at
                                                         some 250 kps where its top is 80, overshot its nav point and
                                                         came back round at 4 degrees a second, "wandered away") */
                    speedLo[s] = et->max_kps;
            }
        } else
            continue;
        ship_face(s, yaw, pitch);
        fly_ahead(s);
        moved |= (uint32_t)1 << s;
    }
    gPositionVector[0] = to;
    fly_ahead(0);
    g_formed = 0;                       /* (the flyby's formation over: each its own way from here) */
    g_form_steps = 0;
    /* nobody lands on anybody: the wing keeps its place off the player, the escort target goes to one fixed spot behind
     * it, and with the nose turned for the next leg a wingman 800 km off the beam came down on the transport (one in
     * eleven jumps within 500 km, at whatever speed it had: a collision's damage goes by its square) */
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (moved >> s & 1)
            spawn_apart(s);
    step_scene();                       /* (what was here gone, what is there back, before the next picture) */
    if (region >= 0) {
        if (wave_may_come(region))
            enterNavPoint((uint16_t)region);
    } else if (next && !field) {
        enterNavPoint((uint16_t)next);
    }
    autopilotFinished();
    return 1;
}

/* the landing by the trigger (the user, 2026-11-06: "could we have autolanding kicked in when there are no enemies
 * present and are very near the carrier and it can say press fire to land. If all landing conditions are met and all
 * nav points have been reached"): every waypoint flown, the carrier's own rule met (logic_landing_allowed), no enemy
 * within AUTOPILOT_CLEAR_KM of a human, and the home carrier's hull within AUTOLAND_KM: the home slot, -1 not now.
 * The autopilot home stops HOME_ARRIVE_KM off the hull, inside it */
#define AUTOLAND_KM 6000
int16_t logic_autoland_ready(void)
{
    int16_t s, home;
    if (missionStatus || !slot_is_ship(0) || shipAiState[0] == 9 || rvController[0] != CTL_PLAYER
        || !logic_landing_allowed())
        return -1;
    for (s = 1; s < NAVS; s++)          /* (every waypoint's reach been in, by this machine's pilot: g_navs_been) */
        if (nav_listed(s) && !(g_navs_been >> s & 1))
            return -1;
    humans_note();
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (slot_is_ship(s) && commAnimInfo[s] != commAnimInfo[0] && shipAiState[s] != 9 && human_km(s) < AUTOPILOT_CLEAR_KM)
            return -1;
    home = home_slot();
    return home >= 0 && logic_is_home(home) && gap_between(0, home) <= AUTOLAND_KM ? home : -1;
}

/* the trigger pulled with logic_autoland_ready: cleared and landed, as on coming near (land_near) */
int16_t logic_autoland(void)
{
    int16_t home = logic_autoland_ready();
    if (home < 0)
        return 0;
    g_cleared = 1;
    landedOn = (uint16_t)home;
    missionStatus = 1;
    return 1;
}

/* --- the thunks' bodies (abi/rvabi.def, spec 5; docs/logic.md has each one's contract) --- */

/* (src, dst, qty, where): a hit of qty on the ship in slot dst at the place `where` (a near RvVec3, positions):
 * returns what it did, 0 nothing.  The player takes none while playerCanBeDamaged is 0.  Everything that hurts goes
 * through here (shots, rocks: rocks.c calls it, rams), and the layer replays a hit on the other machines with src a
 * bolt of the shooter's, the shooter's ship or 0xFFFF: what it does must not depend on src */
/* a store (a shield, an armour) takes what it can of a hit, down to 0: what's left over */
static uint16_t spend(uint16_t *store, uint16_t qty)
{
    uint16_t took = *store < qty ? *store : qty;
    *store = (uint16_t)(*store - took);
    return (uint16_t)(qty - took);
}

/* which shield (0 front, 1 rear) and which armour (0 front, 1 rear, 2 left, 3 right) a hit at w takes on dst: the hit's
 * place from the ship's middle, in km (in positions the products overflow), along its front and its right.  Front or
 * rear picks the shield; the larger of along and across, the armour */
static void hit_sides(int16_t dst, const RvVec3 *w, int16_t *sh, int16_t *ar)
{
    RvVec3 d;
    int32_t f, r;
    d = vec3(TO_KM(w->x - gPositionVector[dst].x), TO_KM(w->y - gPositionVector[dst].y),
             TO_KM(w->z - gPositionVector[dst].z));
    f = vec_dot(&d, &gOrientationFrontVector[dst]);
    r = vec_dot(&d, &gOrientationRightVector[dst]);
    *sh = f >= 0 ? 0 : 1;
    *ar = labs(f) >= labs(r) ? *sh : r >= 0 ? 3 : 2;
}

/* a ship a named pilot flies (its mission record names one of the cast) and no person: the old game's wingmen.  Its hull
 * takes a hit past the armour as the old game's did (read in its code, ovr143:0FE0: a named pilot's ship a fortieth of
 * the hit, at least one, of its hull's count; an unnamed one's a sixth), so a laser's hit is one share of the hull,
 * whatever its size: NAMED_HULL_HITS of them (measured 2026-11-02: Spirit's Hornet died 7 to 10 hull hits after its
 * armour, 14 hits in all at the common rates, as the Schroedinger wingman now does).  Any other ship's hull takes the
 * hit itself.  What the hull is to take of `left` */
#define NAMED_HULL_HITS 8

#ifdef RV_HOST
/* the host's src check (host.c): what do_damage changes outside the fixed block (and the damage module's own) kept, 1,
 * and put back, 0, round its trial with src 0xFFFF */
void logic_damage_snap(int16_t save)
{
    static uint8_t under[NUM_SHIP_SLOTS], pk[RV_PILOTS];
    static uint16_t killer[NUM_SHIP_SLOTS], splashes;
    if (save) {
        _fmemcpy(under, g_under_fire, sizeof under);
        _fmemcpy(killer, g_killer, sizeof killer);
        _fmemcpy(pk, logic_pilot_kills, sizeof pk);
        splashes = logic_splashes;
    } else {
        _fmemcpy(g_under_fire, under, sizeof under);
        _fmemcpy(g_killer, killer, sizeof killer);
        _fmemcpy(logic_pilot_kills, pk, sizeof pk);
        logic_splashes = splashes;
    }
    damage_snap(save);
}
#endif

/* an unnamed fighter's hull as the old game's (ovr143:0EDB, read in its code 2026-11-05: tests/wc1/README.md "The
 * fighters' hulls"): a hit past the armour is max(1, left / 6) points, and each point falls on one of nine systems by
 * the dice: the hull (4), a count, dead past the type's hull_hits of them (a count (hull + hull_hits) / (hull_hits + 1)
 * of damagePoints: the old table's Dralthi 7, Krant 6, Salthi 5); the core (0), a point; from behind (the old game's
 * test is the bolt's way against the nose: a bolt flying the ship's own way struck its rear, where ours take the hit's
 * place behind its middle) one that kills outright (2), the engine (7) and another (1); from the front a gun lost (5),
 * gunDamage up to 5 (6) and the engine flag (8), each while there is one to lose; the shields' generator (3) leaves
 * both shields down for good and spends nothing.  A point that falls where nothing is to spend is rolled again (never
 * for ever: the hull and the core always spend, two of the nine at the fewest, so a point takes 4.5 rolls on average at
 * the worst; ROULETTE_ROLLS of them a point, and some, cap it).  So a Dralthi takes some twenty laser hits
 * past its armour head on, and some three from behind, past the rear armour: the user remembers having "to whale on
 * those things for precious tens of seconds" (the Krant's rear is its thickest side, 180 points of shield and armour)
 * where ours had died at the hull's points from any side, 13 hits at most (182) and a Krant's 6.  Returns what
 * damagePoints takes, 0xFFFF a kill outright */
#define ROULETTE_ROLLS 8
static uint16_t fighter_roulette(int16_t s, uint16_t left, const RvShipType __far *t, int16_t behind)
{
    uint16_t points = left / 6 ? left / 6 : 1, took = 0;
    uint16_t hit = (uint16_t)(((uint32_t)t->hull + t->hull_hits) / (t->hull_hits + 1));
    int32_t rolls, most = (int32_t)points * ROULETTE_ROLLS + 32;
    for (rolls = 0; points && rolls < most; rolls++) {
        int16_t r = (int16_t)(game_rand() % 9), g, n;
        switch (r) {
        case 0:                         /* (the core) */
            if (coreHp[s])
                coreHp[s]--;
            points--;
            break;
        case 1:                         /* (from behind: one more system, and the engine) */
        case 7:
            if (behind)
                points--;
            break;
        case 2:                         /* (from behind: the kill) */
            if (behind)
                return 0xFFFF;
            break;
        case 3:                         /* (the generator: the shields down for good; nothing spent) */
            SHIELD_MAX(s, 0) = SHIELD_MAX(s, 1) = 0;
            SHIELD(s, 0) = SHIELD(s, 1) = 0;
            break;
        case 4:                         /* (the hull: a count) */
            points--;
            took = (uint16_t)(took > 0xFFFFU - hit ? 0xFFFF : took + hit);
            break;
        case 5:                         /* (from the front: a gun lost, one of those left by the dice) */
            if (behind)
                break;
            for (g = 0, n = 0; g < GUN_COUNT(s) && g < 16; g++) {
                const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
                if (w && w->kind == RV_WK_GUN && !(GUN_STATE(s, g) & GUN_DESTROYED))
                    n++;
            }
            if (!n)
                break;
            for (g = 0, n = (int16_t)(game_rand() % n); g < GUN_COUNT(s) && g < 16; g++) {
                const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
                if (w && w->kind == RV_WK_GUN && !(GUN_STATE(s, g) & GUN_DESTROYED) && !n--) {
                    GUN_STATE(s, g) |= GUN_DESTROYED;
                    break;
                }
            }
            points--;
            break;
        case 6:                         /* (from the front: the gun damage, to 5) */
            if (!behind && gunDamage[s] < 5) {
                gunDamage[s]++;
                points--;
            }
            break;
        case 8:                         /* (from the front: the engine flag, once) */
            if (!behind && engineFlag[s] != 0xFF) {
                engineFlag[s] = 0xFF;
                points--;
            }
            break;
        }
    }
    return took;
}

/* a capital ship's mayday (the user, 2026-11-06: "the tiger's claw could say mayday mayday 6 times.... and the exeter 2
 * or 3 times ... and the drayman 1 time"): the old game's capital hull routine rolls, at every hit through the armour of
 * a Confed capital ship, MAYDAY_PER_MILLE in 1,000 for the "Mayday, all Confederate craft please assist" line (ovr143:0F95:
 * rand(1000) < 35), so the maydays go with the hull's count, 560 : 200 : 60.  Ours: the type's ships.csv mayday line,
 * through the flight's hook (the carrier's comm relays it).  The dice roll whether or not a hook listens: the gameplay
 * stream must run the same on every machine, and in the harness */
#define MAYDAY_PER_MILLE 35
void (*__far logic_on_mayday)(int16_t slot);

static uint16_t hull_take(int16_t s, uint16_t left, const RvShipType __far *t, int16_t behind)
{
    uint16_t rec = slotMissionShip[s];
    uint16_t hull = t->hull;
    uint32_t q;
    if (maybeEntityType[s] == ET_CAPSHIP) {
        /* a capital ship's hull as the old game's (ovr143:0F11): a count, a hit past the armour an eighth of itself on the
         * Aera's side and a tenth on the others', at least one; a count CAP_HULL_POINTS of ours (ships.csv: the old
         * game's count x 14), so a laser's hit is one */
        uint16_t div = commAnimInfo[s] == SIDE_AERA ? 8 : 10;
        q = (uint32_t)(left / div ? left / div : 1) * CAP_HULL_POINTS;
        if (game_rand() % 1000 < MAYDAY_PER_MILLE && t->mayday[0] && logic_on_mayday)
            logic_on_mayday(s);
        return q > 0xFFFFUL ? 0xFFFF : (uint16_t)q;
    }
    if ((rvFlags[s] & RV_FLAG_HUMAN) || !hull)
        return left;
    if (rec >= 64 || MSHIP(rec)[16] >= RV_PILOTS)   /* (unnamed: the old game's roulette, for a type with its count) */
        return t->hull_hits ? fighter_roulette(s, left, t, behind) : left;
    q = ((uint32_t)hull * (left / 40 ? left / 40 : 1) + NAMED_HULL_HITS - 1) / NAMED_HULL_HITS;
    return q > 0xFFFFUL ? 0xFFFF : (uint16_t)q;
}

/* the ship behind a do_damage source: a bolt's or a missile's parent, a ship itself; -1 none (the layer's replay may
 * name 0xFFFF) */
static int16_t shooter_of(uint16_t src)
{
    if (!RV_SLOT_OK(src))
        return -1;
    if (src > RV_MAX_SHIP_SLOT || maybeEntityType[src] == ET_MISSILE)
        return parentShipForShip[src] != 0xFF && slot_is_ship(parentShipForShip[src]) ? (int16_t)parentShipForShip[src] : -1;
    return slot_is_ship((int16_t)src) ? (int16_t)src : -1;
}

/* a capital ship's call for help, the old game's Kilrathi side of its capital hull routine (ovr143:0F2A, 0E73, read
 * 2026-11-06; the user: "lets do that aera capital ship hurting sending fighters to help"): hurt through its armour by
 * a ship that no ship of another side has within HELP_KM of it (checkIfAnyShipsInRange: the attacker is unengaged),
 * every ship of its own side turns on the attacker on a coin's toss.  (In the old code the source had to be a ship
 * slot, which its bolts are not, so its guns hardly ever rang the bell; ours rings it for any shooter.)  Ours: the AI
 * fighters of the hurt ship's side, their minds' target set; the orders (prey_of), a raid and a nearer enemy by two
 * thirds may take it back as they do any target.  The coins are tossed whoever the shooter is: the gameplay stream
 * must run the same on every machine */
#define HELP_KM 10000
static void capital_help(int16_t dst, uint16_t src)
{
    static const char __far s_help[] = "logic: frame %u: slot %d (a capital ship, side %d) hurt by slot %d: %d of its fighters turn on it";
    int16_t by = shooter_of(src), k, n = 0, near_ = 0;
    uint8_t side = commAnimInfo[dst];
    if (by >= 0) {
        if (by == dst || commAnimInfo[by] == side)
            by = -1;
        else
            for (k = 0; k < NUM_SHIP_SLOTS && !near_; k++)
                if (k != by && slot_is_ship(k) && shipAiState[k] != 9 && commAnimInfo[k] != commAnimInfo[by]
                    && km_between(by, k) < HELP_KM)
                    near_ = 1;
    }
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        int16_t coin = (int16_t)(game_rand() & 1);
        if (coin && by >= 0 && !near_ && k != dst && slot_is_ship(k) && shipAiState[k] != 9 && commAnimInfo[k] == side
            && maybeEntityType[k] == ET_SHIP && rvController[k] == CTL_AI && !(rvFlags[k] & RV_FLAG_HUMAN)
            && !g_mind[k].leaving) {
            g_mind[k].target = (uint8_t)by;
            rvTarget[k] = (uint8_t)by;
            n++;
        }
    }
    if (n)
        rv_notef(s_help, frameCounter, dst, side, by, n);
}

uint16_t __far __cdecl do_damage_impl(uint16_t src, uint16_t dst, uint16_t qty, uint16_t where)
{
    const RvShipType __far *t;
    int16_t sh, ar;
    uint16_t left;
    (void)src;                          /* (blame and effects only: the amount must not depend on it) */
    if (!slot_is_ship((int16_t)dst) || !qty || (dst == 0 && !playerCanBeDamaged) || (rvFlags[dst] & 1)
        || shipAiState[dst] == 9 || (t = slot_type((int16_t)dst)) == 0)
        return 0;
    hit_sides((int16_t)dst, RV_WHERE_VEC(where), &sh, &ar);   /* (which side) */
    /* that shield, then that armour, then the hull: its end is the ship's (shipAiState 9, the layer's sign of a
     * death; step_deaths takes it from there) */
    g_under_fire[dst] = (uint8_t)(g_under_fire[dst] > 251 ? 255 : g_under_fire[dst] + 4);
    left = spend(&ARMOR(dst, ar), spend(&SHIELD(dst, sh), qty));
    if (left) {
        const int16_t player = dst == 0 && rvController[0] == CTL_PLAYER;
        const uint16_t room = damagePoints[dst] < t->hull ? (uint16_t)(t->hull - damagePoints[dst]) : 0;
        if (!player)
            left = hull_take((int16_t)dst, left, t, sh == 1);   /* (a named pilot's: a share of the hull a hit; an
                                                       unnamed fighter's: the old game's roulette; a capital's: a count) */
        if (!player && maybeEntityType[dst] == ET_CAPSHIP)
            capital_help((int16_t)dst, src);   /* (its side's fighters on the attacker, by the coins) */
        /* the hull's last point is not the player's end: the reserve takes what is left over (damage.h) */
        if (player && left >= room && damage_reserve_take((uint16_t)(left - room), t->hull)) {
            damagePoints[dst] = t->hull;
        } else if (left >= room) {
            static const char __far s_killed[] = "logic: frame %u: slot %u (type %02X, side %d) killed by %u (type %02X): %u points";
            rv_notef(s_killed, frameCounter, dst, gInstanceShipTypes[dst], commAnimInfo[dst], src,
                    RV_SLOT_OK(src) ? gInstanceShipTypes[src] : 0xFF, qty);
            damagePoints[dst] = t->hull;
            shipAiState[dst] = 9;
            g_killer[dst] = src;
            {                           /* (the local pilot's splash: its bolt, its missile, its own hull, at an enemy) */
                uint16_t by = RV_SLOT_OK(src) && (src > RV_MAX_SHIP_SLOT || maybeEntityType[src] == ET_MISSILE)
                              && parentShipForShip[src] != 0xFF ? parentShipForShip[src] : src;
                if (RV_SLOT_OK(by) && by <= RV_MAX_SHIP_SLOT && commAnimInfo[dst] != commAnimInfo[by]) {
                    if (rvController[by] == CTL_PLAYER)
                        logic_splashes++;
                    else if (rvController[by] == CTL_AI && !(rvFlags[by] & RV_FLAG_HUMAN) && slotMissionShip[by] < 64
                             && MSHIP(slotMissionShip[by])[16] < RV_PILOTS
                             && logic_pilot_kills[MSHIP(slotMissionShip[by])[16]] < 255)
                        logic_pilot_kills[MSHIP(slotMissionShip[by])[16]]++;   /* (an AI wingman's own: a human in its
                                                       seat keeps its splashes in its own save: debrief_settle) */
                }
            }
        } else {
            damagePoints[dst] = (uint16_t)(damagePoints[dst] + left);
        }
        if (player && t->hull)
            damage_hurt(left, t->hull, ar);
    }
    return qty;
}

/* An AI's scatter by the range (the user, 2026-11-02: "maybe the 4% number should depend on range"): 4 degrees up close,
 * 3 past 1,000 km, 2 past 1,500.  The old game's tail hit a straight player 71% of the time from 2,000-3,000 km, 45% from
 * 3,000-4,000, 25% from 4,000-5,000; ours, scattering 4 degrees at any range, 25%, 13% and 3% (a straight player took
 * 256 points in 4 minutes where the old one took 872).  Measured in the first mission's scene: 2 degrees from 1,500 km
 * out, 782 points (36 hits; the old 38); going on down to 1 past 2,000 and none past 2,500 (the user's first idea),
 * 1,449: the tail falling back fires from 2,500-4,500 km, all of it on target.  The range is the nearest
 * ship of another side within 25 degrees of the nose, from the positions every machine has (each flies its own copy
 * of a shot); none: the near scatter */
static int32_t scatter_of(int16_t ship)
{
    int32_t nearest = -1;
    int16_t k;
    for (k = 0; k < NUM_SHIP_SLOTS; k++) {
        RvVec3 d;
        int32_t km;
        if (k == ship || !slot_is_ship(k) || commAnimInfo[k] == commAnimInfo[ship] || shipAiState[k] == 9)
            continue;
        d = vec3(TO_KM(gPositionVector[k].x - gPositionVector[ship].x), TO_KM(gPositionVector[k].y - gPositionVector[ship].y),
                 TO_KM(gPositionVector[k].z - gPositionVector[ship].z));
        km = len_of(d.x, d.y, d.z);
        if (km > 0 && vec_dot(&d, &gOrientationFrontVector[ship]) / km < 232 /* (cos 25 degrees x 256) */)
            continue;
        if (nearest < 0 || km < nearest)
            nearest = km;
    }
    return nearest < 1000 ? AI_SCATTER : nearest < 1500 ? AI_SCATTER * 3 / 4 : AI_SCATTER / 2;
}

/* (ship, gun): gun record `gun` of the ship fires: a gun or a turret puts a shot in the world (shot_add, from its
 * mount: GUN_MOUNT_X / Y), a launcher a missile into a free ship slot.  It fires whatever its cooldown and the
 * energy: the deciding is the caller's (the layer replays this call on the other machines with the gun record as
 * the shooter's machine had it, so it must not refuse there); it sets the cooldown and spends the energy itself.
 * Returns a missile's slot (the layer maps it), else 0xFFFF */
uint16_t __far __cdecl fireGunFromShip_impl(uint16_t ship, uint16_t gun)
{
    const RvWeapon __far *w;
    if (!slot_is_ship((int16_t)ship) || gun >= GUN_COUNT(ship) || (w = weapon(GUN_TYPE(ship, gun))) == 0)
        return 0xFFFF;
    if (w->kind == RV_WK_MISSILE || w->kind == RV_WK_TORPEDO || w->kind == RV_WK_ROCKET)
        return missile_launch(ship, gun, w);
    return gun_fire(ship, gun, w);
}

/* (ship): every one of its guns that is ready, fired through the fireGunFromShip thunk */
void __far __cdecl maybe_fire_all_guns_impl(uint16_t ship)
{
    if (slot_is_ship((int16_t)ship))
        guns_fire_ready(ship);
}

/* (slot): a fighter's mind for a step (an AI ship's: the layer skips it on the machines where the ship is a
 * puppet) */
void __far __cdecl aiFighterThink_impl(uint16_t s)
{
    if (RV_SLOT_OK(s) && slot_is_ship((int16_t)s))
        fighter_think((int16_t)s);
}

/* (slot): a capital ship's mind for a step: its turrets, its course */
void __far __cdecl aiShipThink_impl(uint16_t s)
{
    if (RV_SLOT_OK(s) && slot_is_ship((int16_t)s))
        ship_think((int16_t)s);
}

/* (ship): the game's own ending of a ship (its explosion, its debris, a comm line), then the slot empty
 * (ship_clear).  The layer calls it when the host says a ship died */
void __far __cdecl despawn_impl(uint16_t ship)
{
    if (RV_SLOT_OK(ship) && ship <= RV_MAX_SHIP_SLOT)
        ship_clear((int16_t)ship);
}

/* (src, ship): a ship's end that src brought about (a kill) begins: dying (shipAiState 9), its explosion's time from
 * now (step_deaths counts it, then despawn ends it), its throttle at 0.  step_deaths calls it when it first sees a
 * ship dying; the layer, on a machine whose copy the host's kill reaches */
void __far __cdecl delayedDespawn_impl(uint16_t src, uint16_t ship)
{
    (void)src;                          /* (the blame: do_damage's, the layer's) */
    if (!RV_SLOT_OK(ship) || ship > RV_MAX_SHIP_SLOT || !slot_is_ship((int16_t)ship))
        return;
    shipAiState[ship] = 9;
    if (!g_died[ship]) {
        g_died[ship] = (uint16_t)(frameCounter ? frameCounter : 1);
        setSpeed[ship] = 0;
    }
}

/* (nav): the player has reached nav point `nav`: its ships and hazards come (each of its mission ships through
 * outerSpawnShipEntity, at its place off the nav point), once */
/* An enemy wave comes in from the old game's distance (traced 2026-11-02: its autopilot stopped 15,200 km short of
 * Enyo's nav 1, the point's reach, and its three Dralthi were 15,173 km off; their first shots came 18 to 25 s on).
 * On the small map (0.45 of its size, until 2026-11-04) a wave came 4,600 km from an arriving player and fired within
 * seconds.  On its own map most come from that far by themselves; a wave nearer than WAVE_KM (a point of 10,000 km's
 * reach, a wave on the near side of its point) is moved out together, along the way from the player to its middle,
 * until it is WAVE_KM off; its formation and the nav's other ships stay */
#define WAVE_KM 15000

static void wave_out(const uint8_t __far *came)
{
    static const char __far s_out[] = "logic: the wave of %d moved out from %ld km to %ld km of the player";
    int32_t cx = 0, cy = 0, cz = 0, d, k;
    int16_t s, n = 0;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (came[s]) {
            cx += TO_KM(gPositionVector[s].x - gPositionVector[0].x);
            cy += TO_KM(gPositionVector[s].y - gPositionVector[0].y);
            cz += TO_KM(gPositionVector[s].z - gPositionVector[0].z);
            n++;
        }
    if (!n)
        return;
    cx /= n;
    cy /= n;
    cz /= n;
    d = len_of(cx, cy, cz);
    if (d >= WAVE_KM)
        return;
    if (d < 100) {                      /* (on the player: out ahead of it) */
        cx = FIX_MUL(gOrientationFrontVector[0].x, 1000);
        cy = FIX_MUL(gOrientationFrontVector[0].y, 1000);
        cz = FIX_MUL(gOrientationFrontVector[0].z, 1000);
        d = len_of(cx, cy, cz);
        if (d < 1)
            return;
    }
    k = WAVE_KM - d;                    /* (km to go, along the way out) */
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (came[s]) {
            gPositionVector[s].x += cx * k / d * KM;
            gPositionVector[s].y += cy * k / d * KM;
            gPositionVector[s].z += cz * k / d * KM;
        }
    rv_notef(s_out, n, (long)d, (long)WAVE_KM);
}

void __far __cdecl enterNavPoint_impl(uint16_t nav)
{
    static const char __far s_nav[] = "logic: nav %u reached: %d ships came";
    static uint8_t __far came[NUM_SHIP_SLOTS];
    const uint16_t *at;
    int16_t j, n = 0;
    if (nav >= NAVS)
        return;
    currentNavPoint = nav;
    g_navs_seen |= 1u << nav;
    if (nav_listed((int16_t)nav))
        g_navs_been |= 1u << nav;
    if (g_nav_chosen && g_sys.nav == nav)
        g_nav_chosen = 0;               /* (the one the player picked, reached: on along the way) */
    at = NAV_SHIPS(nav);
    _fmemset(came, 0, sizeof came);
    for (j = 0; j < 10 && at[j] != 0xFFFF; j++) {
        uint16_t s;
        if (MSHIP_BEHAVIOUR(at[j]) != 6 && (s = outerSpawnShipEntity(at[j], nav)) != 0xFFFF) {
            n++;
            if (s > 0 && s < NUM_SHIP_SLOTS && maybeEntityType[s] == ET_SHIP && commAnimInfo[s] != commAnimInfo[0]
                && !MSHIP_ROLE(at[j]))
                came[s] = 1;            /* (an enemy fighter: out to WAVE_KM with its wave) */
        }
    }
    wave_out(came);
    rv_notef(s_nav, nav, n);
}

/* PLUMBING (ship, speed): the AI's throttle (the layer skips the call for a puppet) */
void __far __cdecl aiSetSpeed_impl(uint16_t ship, uint16_t speed)
{
    if (RV_SLOT_OK(ship))
        setSpeed[ship] = speed;
}

/* PLUMBING (slot, controller): who flies a slot (CTL_*): the layer's way to make a ship a puppet or give it back */
void __far __cdecl setController_impl(uint16_t slot, uint16_t controller)
{
    if (RV_SLOT_OK(slot))
        rvController[slot] = (uint8_t)controller;
}

/* PLUMBING (slot): gone with nothing to show (the layer's: a ship too far to keep, a duplicate) */
void __far __cdecl despawnQuietly_impl(uint16_t slot)
{
    if (RV_SLOT_OK(slot))
        ship_clear(slot);
}

/* PLUMBING (slot, km): 1 when a ship is too far from the player to keep (spec 5.3: the layer asks before it drops
 * one) */
uint16_t __far __cdecl farShipTest_impl(uint16_t slot, uint16_t km)
{
    return slot_is_ship(slot) && player_km(slot) > (int32_t)km;
}

/* PLUMBING (missionShip, navPoint): a mission ship (missionShipTable's record) into a free slot at its nav point
 * (navPointTable's, km), at its formation place off it; behaviour 6 (the player) takes slot 0.  The layer calls it
 * to put another player's ship into this machine's flight (it switches the record's behaviour from 6 to 4 around
 * the call): returns the slot, 0xFFFF none */
/* nobody spawns inside another ship: a mission ship put down on one already there (a wingman's record has no place of
 * its own, so it landed on the player, and the two ground against each other all flight: the user's flickering lights,
 * 2026-10-30) is moved to that ship's right, 800 km or the two hulls and a margin, whichever is more: where a wingman's
 * formation station is taken from (formation_step) */
static void spawn_apart(int16_t s)
{
    int16_t k, hit = -1, tries;
    int32_t off, step;
    const RvVec3 *R;
    for (k = 0; k < NUM_SHIP_SLOTS; k++)
        if (k != s && slot_is_ship(k) && km_between(s, k) < room_of(s, k) + 100) {
            hit = k;
            break;
        }
    if (hit < 0)
        return;
    off = room_of(s, hit) + 300;
    if (off < 800)
        off = 800;
    R = &gOrientationRightVector[hit];
    /* further out to the right each time the place found is inside another ship (a wingman and a transport both put
     * at the leader's place landed on each other, and the bumps took the transport's way off it every step) */
    for (tries = 1, step = off; tries <= 8; tries++, off += step) {
        gPositionVector[s] = vec3(gPositionVector[hit].x + R->x * off, gPositionVector[hit].y + R->y * off,
                                  gPositionVector[hit].z + R->z * off);   /* (R is 256 a unit: x 256 km = KM x km) */
        for (k = 0; k < NUM_SHIP_SLOTS; k++)
            if (k != s && slot_is_ship(k) && km_between(s, k) < room_of(s, k) + 100)
                break;
        if (k == NUM_SHIP_SLOTS)
            return;
    }
}

uint16_t __far __cdecl outerSpawnShipEntity_impl(uint16_t missionShip, uint16_t navPoint)
{
    const uint8_t *rec = missionShipTable + missionShipTable_SIZE * (missionShip & 63);
    const uint8_t *nav = navPointTable + navPointTable_SIZE * (navPoint & 15);
    uint16_t behaviour = *(const uint16_t *)(rec + 6);
    int16_t s = behaviour == 6 ? 0 : ship_free_slot();
    if (missionShip >= 64 || s < 0)
        return 0xFFFF;
    /* (the nav point's place and the ship's off it, 32 bits each: in DOS's 16-bit int CEPHID2's destroyer, 31,500 +
     * 3,375 km on the small map, had wrapped round to -30,661 and it was 60,000 km away; on the old game's map the nav
     * points themselves are past 32,767 km) */
    ship_spawn(s, *(const uint16_t *)rec, *(const uint16_t *)(rec + 2), s == 0 ? CTL_PLAYER : CTL_AI,
               KM * (NAV_REC_KM(nav, 0) + MSHIP_REC_KM(rec, 0)), KM * (NAV_REC_KM(nav, 1) + MSHIP_REC_KM(rec, 1)),
               KM * (NAV_REC_KM(nav, 2) + MSHIP_REC_KM(rec, 2)), 0);
    slotMissionShip[s] = missionShip;
    shipStateByte[s] = s == 0 ? 8 : rec[16];
    if (rec[17] && rec[17] <= P_COUNT) {   /* (its AI's profile, when the mission names one: docs/ai.md) */
        g_mind[s].profile = (uint8_t)(rec[17] - 1);
        mind_rates(s);
    }
    if (rec[19]) {                      /* (its own fear rates: missions.txt fear=c/d, c the low nibble) */
        g_mind[s].fear_up = (uint8_t)(rec[19] & 15);
        g_mind[s].fear_down = (uint8_t)(rec[19] >> 4);
    }
    if (rec[18] == RV_ESCORT_HOME)      /* (the carrier holds station from the first: no way on to lose) */
        setSpeed[s] = speedLo[s] = 0;
    spawn_apart(s);
    return (uint16_t)s;
}
