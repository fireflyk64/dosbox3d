/* tests/play/playtest.c: the play tests.  What src/logic.c has to do for RetroVega to fly and fight like the space
 * combat games of the early nineties, one check at a time, in the order docs/gameplay.md builds it (M1 flight, M2
 * guns, M3 damage, M4 deaths, M5 the AI, M6 collisions, M7 missiles, M8 the mission, M9 the HUD).  They fail until
 * the logic is written and pass as each piece is; they are yours to change where your design differs (the
 * tolerances are at the top, and each test says what it expects).
 *
 * The same file is built twice:
 *   - on the host into build/host32/playtest (host/build.sh, with the rvsim harness and PLAYTEST_MAIN), where gdb,
 *     rr, valgrind and the sanitizers work and the multiplayer layer's rules are checked at every thunk (host.c);
 *   - into RVP.EXE (build.sh: RV.EXE with the tests), run in DOSBox as RVP.EXE /playtest [IDS], its lines in RVLOG.TXT.
 * Each test ends with a hash of the world: the slot arrays, the mission tables and the shots.  The DOS build must
 * reach the host's world test by test (tests/play/playtest.sh --dos compares them): a difference is a computation
 * that comes out otherwise with DOS's 16-bit int (docs/logic.md, "Sized types").
 *
 * A test is a scene: the flight begun as flight.c begins it (camp_load_units, the scalars, logic_start), ships put
 * where the test wants them (as the layer puts ships in, by ship_spawn), the controls set, then steps of
 * logic_frame, and the world looked at.  Only the logic's public face is used: the thunks, logic_start,
 * logic_frame, logic_end, logic_hud, g_pilot, g_sys and the fixed block.
 *
 * Written for both compilers: C89 (declarations first), the sized types, and nothing in DGROUP (RVP.EXE, RV.EXE and the tests, has
 * 3 KB of it free): every string a `static const char __far`, every table and variable __far.  The messages'
 * numbers are int32_t, printed by ln_f's %d. */
#include <string.h>
#include <stdio.h>
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
#include "simulator.h"
#ifdef RV_HOST
#include "host.h"
#endif

int16_t playtest_run(const char *only);

/* the tolerances (yours to change) */
#define TURN_SLACK 30                   /* percent: a turn may be this far off its rate, dps / KPS_HZ degrees a step */
#define SPEED_SLACK 3                   /* percent: a step's move this far off KPS_STEP(the speed) */
#define AIM_COS 222                     /* the AI fires with its target within 30 degrees of its nose (cos x 256) */
#define AIM_SHARE 70                    /* percent of its shots */

/* the ships the tests fly (data/ships.csv) */
#define PT_PLAYER 0x00                  /* the Schroedinger: three lasers, two dumbfires (its first launcher) and a heat
                                           seeker */
#define PT_HOMER 0x02                   /* the Gawain: two particle cannons, two friend-or-foe missiles */
#define PT_ENEMY 0x08                   /* the Nicander */
#define PT_WEAK 0x0B                    /* the Agis */
#define PT_OX 0x24                      /* the Ox: the transport the old campaign's missions escort */
#define PT_CAPSHIP 0x20                 /* the Vigilance */
#define PT_LINKED 0x03                  /* the Redeemer: four mass drivers, linked */
#define SEC LOGIC_HZ                    /* steps a second */

typedef void (*PtFn)(void);
typedef struct {
    const char __far *name;             /* "M1.1 name (the functions): what it expects" */
    PtFn fn;
    uint8_t host_only;                  /* asks the harness what the DOS build cannot tell (the thunks' calls, the
                                           screen) */
} PtTest;

typedef struct { uint16_t sh[2], ar[4], hull, state; } PtHurt;

/* --- the run's state --- */

static RvMission __far g_tm;            /* the test's mission */
static int16_t __far g_index;           /* the test running */
static int16_t __far g_failed;          /* its failures */
#ifdef RV_HOST
static uint32_t __far g_rules0;         /* host_rules_broken as it began */
#endif
static char __far g_line[200];
static int16_t __far g_len;
static const char __far *__far g_name;  /* its name */
static int16_t __far g_steps;           /* "steps" in the list: a line a step, the world's parts hashed */

static const char __far s_pre[] = "playtest: ";

/* --- the output: a line at a time, the numbers by hand (the same on both compilers) --- */

static void ln_c(char c)
{
    if (g_len < (int16_t)sizeof g_line - 1)
        g_line[g_len++] = c;
}

static void ln_s(const char __far *s)
{
    while (*s)
        ln_c(*s++);
}

static void ln_d(int32_t v)
{
    char d[12];
    int16_t n = 0;
    uint32_t u = v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    if (v < 0)
        ln_c('-');
    do {
        d[n++] = (char)('0' + (int16_t)(u % 10));
        u /= 10;
    } while (u);
    while (n)
        ln_c(d[--n]);
}

static void ln_x(uint32_t v)
{
    int16_t i, k;
    for (i = 28; i >= 0; i -= 4) {
        k = (int16_t)((v >> i) & 15);
        ln_c((char)(k < 10 ? '0' + k : 'A' + k - 10));
    }
}

/* a message: %d takes the next of a, b, c (in RVP.EXE a token, tools/logfmt.py: the messages' 48 KB are in its
 * table, past the load image, not in conventional memory) */
static void ln_f(const char __far *f, int32_t a, int32_t b, int32_t c)
{
    int32_t v[3];
    int16_t k = 0;
#ifndef RV_HOST
    char t[256];
    if (f[0] == '\001') {
        logfmt_text(f, t, sizeof t);
        f = t;
    }
#endif
    v[0] = a;
    v[1] = b;
    v[2] = c;
    while (*f)
        if (f[0] == '%' && f[1] == 'd' && k < 3) {
            ln_d(v[k++]);
            f += 2;
        } else {
            ln_c(*f++);
        }
}

/* the current test's id: its name up to the space */
static void ln_id(void)
{
    const char __far *n = g_name;
    while (*n && *n != ' ')
        ln_c(*n++);
}

static void ln_end(void)
{
    char buf[200];
#ifndef RV_HOST
    static const char __far s_fmt[] = "%s";
#endif
    g_line[g_len] = 0;
    _fstrcpy(buf, g_line);
    g_len = 0;
#ifdef RV_HOST
    puts(buf);
    fflush(stdout);
#else
    rv_logf(s_fmt, buf);
#endif
}

static const char __far s_indent[] = "     ", __far s_colon[] = ": ";

/* a check failed: the first three a test are told */
static void pt_fail(const char __far *f, int32_t a, int32_t b, int32_t c)
{
    if (++g_failed > 3)
        return;
    ln_s(s_pre);
    ln_s(s_indent);
    ln_id();
    ln_s(s_colon);
    ln_f(f, a, b, c);
    ln_end();
}

#define EXPECT(c, f, a, b, d) do { if (!(c)) pt_fail(f, (int32_t)(a), (int32_t)(b), (int32_t)(d)); } while (0)
#define REQUIRE(c, f, a, b, d) do { if (!(c)) { pt_fail(f, (int32_t)(a), (int32_t)(b), (int32_t)(d)); return; } } while (0)

/* --- numbers --- */

static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

/* |v| of any vector (halved until its squares fit) */
static int32_t pt_len(int32_t x, int32_t y, int32_t z)
{
    int32_t k = 1;
    while (abs32(x) > 30000 || abs32(y) > 30000 || abs32(z) > 30000) {
        x /= 2;
        y /= 2;
        z /= 2;
        k *= 2;
    }
    return (int32_t)lf_sqrt((uint32_t)(x * x) + (uint32_t)(y * y) + (uint32_t)(z * z)) * k;
}

/* km between two slots */
static int32_t pt_km(int16_t a, int16_t b)
{
    return pt_len((gPositionVector[b].x - gPositionVector[a].x) / 256, (gPositionVector[b].y - gPositionVector[a].y) / 256,
                  (gPositionVector[b].z - gPositionVector[a].z) / 256);
}

/* km from a slot to a place (km) */
static int32_t pt_km_to(int16_t a, int32_t x, int32_t y, int32_t z)
{
    return pt_len(x - gPositionVector[a].x / 256, y - gPositionVector[a].y / 256, z - gPositionVector[a].z / 256);
}

/* how straight s's nose points at b: the cosine x 256 */
static int32_t pt_aim(int16_t s, int16_t b)
{
    RvVec3 d;
    int32_t len;
    d.x = (gPositionVector[b].x - gPositionVector[s].x) / 256;
    d.y = (gPositionVector[b].y - gPositionVector[s].y) / 256;
    d.z = (gPositionVector[b].z - gPositionVector[s].z) / 256;
    while (abs32(d.x) > 30000 || abs32(d.y) > 30000 || abs32(d.z) > 30000) {
        d.x /= 2;
        d.y /= 2;
        d.z /= 2;
    }
    len = pt_len(d.x, d.y, d.z);
    return len ? vec_dot(&d, &gOrientationFrontVector[s]) / len : 256;
}

/* v's angle in the plane of ax (0) and ay (a quarter turn): TURN a whole turn, -512 .. 511 */
static int16_t pt_angle(const RvVec3 *v, const RvVec3 *ax, const RvVec3 *ay)
{
    int16_t t = lf_atan2(vec_dot(v, ay) / 256, vec_dot(v, ax) / 256);
    return t >= 512 ? t - 1024 : t;
}

/* a turn's expected size in n steps at dps and a steer of 9ths: TURN a whole turn */
static int32_t pt_turn_of(int16_t n, int16_t dps, int16_t steer)
{
    return (int32_t)n * dps * TURN * steer / (INT32_C(360) * KPS_HZ * 9);
}

static int16_t pt_square(int16_t s)
{
    const RvVec3 *f = &gOrientationFrontVector[s], *r = &gOrientationRightVector[s], *u = &gOrientationUpVector[s];
    int32_t lf = vec_dot(f, f) / 256, lr = vec_dot(r, r) / 256, lu = vec_dot(u, u) / 256;
    if (lf < 207 || lf > 310 || lr < 207 || lr > 310 || lu < 207 || lu > 310)
        return 0;
    return abs32(vec_dot(f, r)) <= 26 * 256 && abs32(vec_dot(f, u)) <= 26 * 256 && abs32(vec_dot(r, u)) <= 26 * 256;
}

/* shields and armour left, less the hull's damage: falls with every hurt */
static int32_t pt_health(int16_t s)
{
    return (int32_t)SHIELD(s, 0) + SHIELD(s, 1) + ARMOR(s, 0) + ARMOR(s, 1) + ARMOR(s, 2) + ARMOR(s, 3)
           - damagePoints[s];
}

static void pt_hurt(int16_t s, PtHurt *h)
{
    int16_t k;
    for (k = 0; k < 2; k++)
        h->sh[k] = SHIELD(s, k);
    for (k = 0; k < 4; k++)
        h->ar[k] = ARMOR(s, k);
    h->hull = damagePoints[s];
    h->state = shipAiState[s];
}

/* the shots of a slot's in the list */
static int16_t pt_shots_of(int16_t owner)
{
    int16_t i, n = 0;
    for (i = 0; i < shots_count(); i++)
        if (shot(i)->owner == owner)
            n++;
    return n;
}

/* a gun record's weapon kind (RV_WK_*), 0xFF none */
static int16_t pt_kind(int16_t s, int16_t g)
{
    const RvWeapon __far *w = weapon(GUN_TYPE(s, g));
    return w ? w->kind : 0xFF;
}

static int16_t pt_is_gun(int16_t s, int16_t g)
{
    int16_t k = pt_kind(s, g);
    return k == RV_WK_GUN || k == RV_WK_TURRET;
}

static int16_t pt_guns(int16_t s)       /* its guns and turrets */
{
    int16_t g, n = 0;
    for (g = 0; g < GUN_COUNT(s); g++)
        if (pt_is_gun(s, g) && !(GUN_STATE(s, g) & GUN_DESTROYED))
            n++;
    return n;
}

static int16_t pt_launcher(int16_t s)   /* its first missile launcher, -1 none */
{
    int16_t g, k;
    for (g = 0; g < GUN_COUNT(s); g++)
        if ((k = pt_kind(s, g)) == RV_WK_MISSILE || k == RV_WK_TORPEDO || k == RV_WK_ROCKET)
            return g;
    return -1;
}

static int16_t pt_missile_of(int16_t parent)   /* a missile slot launched by parent, -1 none */
{
    int16_t s;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (maybeEntityType[s] == ET_MISSILE && parentShipForShip[s] == parent)
            return s;
    return -1;
}

static int16_t pt_count(int16_t side, int16_t type)   /* ships of a side (and a type: -1 any) in the slots */
{
    int16_t s, n = 0;
    for (s = 0; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == side && (type < 0 || gInstanceShipTypes[s] == type))
            n++;
    return n;
}

/* the calls of a thunk the harness counted (RV_FN_*, its argument arg was value; arg -1 all): -1 on DOS, which
 * cannot tell */
static int32_t pt_calls(uint16_t fn, int16_t arg, int16_t value)
{
#ifdef RV_HOST
    return (int32_t)host_calls(fn, arg, value);
#else
    (void)fn;
    (void)arg;
    (void)value;
    return -1;
#endif
}

/* --- the world --- */

/* the test's mission: the player alone at nav 0 (pt_nav and pt_mship add to it) */
static void pt_mission(void)
{
    static const char __far s_id[] = "PLAYTEST";
    logic_scene_off = 1;                /* (M8.25's to test: the others set scenes with the player well away) */
    _fmemset(&g_tm, 0, sizeof g_tm);
    _fstrcpy(g_tm.id, s_id);
    g_tm.nship = 1;
    g_tm.fighter = 0xFF;                /* (no fighter of its own: the player's type stays the test's) */
    g_tm.ship[0].type = RV_SHIP_PLAYER;
    g_tm.ship[0].count = 1;
    g_tm.ship[0].pilot = RV_PILOT_NONE;
}

/* (a test's nav point: its reach PT_REACH_KM, the small map's, which the scenes here were written for (the player put
 * 4,500 km short of a point is there); the missions' own are the old game's, 10,000 km and more) */
#define PT_REACH_KM 5000
static void pt_nav(uint8_t id, int32_t x, int32_t y, int32_t z)
{
    RvNav __far *n = &g_tm.nav[g_tm.nnav++];
    n->id = id;
    n->x = x;
    n->y = y;
    n->z = z;
    n->reach = PT_REACH_KM;
}

static void pt_mship(uint8_t type, uint8_t side, uint8_t nav, uint8_t count, uint8_t pilot)
{
    RvShip __far *s = &g_tm.ship[g_tm.nship++];
    s->type = type;
    s->side = side;
    s->nav = nav;
    s->count = count;
    s->pilot = pilot;
}

/* the cockpit's systems as a launch leaves them (flight.c's systems_start) */
static void pt_systems(void)
{
    _fmemset(&g_sys, 0, sizeof g_sys);
    g_sys.emcon = EMCON_ACTIVE;
    g_sys.iff = 1;
    g_sys.armed = 1;
    g_sys.decoys = 6;
    g_sys.radar_range = 1;
    g_sys.target = -1;
    g_sys.fuel = 1000;
    g_sys.hull = 100;
    g_sys.lights = 1;
    g_sys.target_km = g_sys.nav_km = -1;
}

/* the flight begins as flight.c begins it, with the test's mission (g_tm) and the player in a ship of `type`: the
 * world emptied, the mission's tables (camp_load_units), the scalars (victoryPoints 1: the stand-in's roll, which
 * the logic scores over), the systems, the shots' list, then logic_start */
static void pt_launch(int16_t type)
{
    int16_t s;
    _fmemset(&g_pilot, 0, sizeof g_pilot);
    memset(maybeEntityType, 0, (size_t)((uint8_t *)navPointTable - (uint8_t *)maybeEntityType));
    for (s = 0; s < RV_SLOTS; s++) {
        ship_clear(s);
        missileTarget[s] = 0xFF;
        rvTarget[s] = 0xFF;
    }
    shots_flight_end();
    collide_flight_end();
    shots_flight_start(0);
    g_camp.player_ship = (uint8_t)type;
    camp_load_units(&g_tm);
    missionStatus = 0;
    frameCounter = 0;
    playerCanBeDamaged = 1;
    rvCockpitDamage = 0;
    randomSeed = UINT32_C(0x52560000) + (uint32_t)g_index * 977;
    currentNavPoint = 0;
    victoryPoints = 1;
    playerTarget = 0xFFFF;
    landedOn = 0xFFFF;
    rvPlayerCount = 1;
    rvNetRole = 0;
    pt_systems();
#ifdef RV_HOST
    host_counts_reset();
    g_rules0 = host_rules_broken;
#endif
    logic_start(&g_tm);
}

/* a slot's place (km) and heading (TURN a whole turn of yaw), level, still */
static void pt_put(int16_t s, int32_t x, int32_t y, int32_t z, int16_t yaw)
{
    gPositionVector[s].x = x * KM;
    gPositionVector[s].y = y * KM;
    gPositionVector[s].z = z * KM;
    ship_face(s, yaw, 0);
    gVelocityVector[s].x = gVelocityVector[s].y = gVelocityVector[s].z = 0;
}

/* its throttle and speed (kps), and its velocity along its nose */
static void pt_speed(int16_t s, int32_t kps)
{
    setSpeed[s] = (uint32_t)kps;
    speedLo[s] = (uint32_t)kps;
    gVelocityVector[s].x = gOrientationFrontVector[s].x * KPS_STEP(kps) / 256;
    gVelocityVector[s].y = gOrientationFrontVector[s].y * KPS_STEP(kps) / 256;
    gVelocityVector[s].z = gOrientationFrontVector[s].z * KPS_STEP(kps) / 256;
}

/* a ship into a slot, as the layer puts one in (ship_spawn), still */
static void pt_ship(int16_t s, int16_t type, int16_t side, int16_t ctl, int32_t x, int32_t y, int32_t z, int16_t yaw)
{
    ship_spawn(s, type, side, ctl, x * KM, y * KM, z * KM, yaw);
    pt_speed(s, 0);
}

/* the player alone (`type`), at the origin facing +z, still; the slots the mission's start filled emptied (the
 * player put in slot 0 if logic_start did not) */
static void pt_alone(int16_t type)
{
    int16_t s;
    pt_mission();
    pt_launch(type);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        ship_clear(s);
    if (!slot_is_ship(0) || rvController[0] != CTL_PLAYER || gInstanceShipTypes[0] != type) {
        ship_spawn(0, type, SIDE_TERRAN, CTL_PLAYER, 0, 0, 0, 0);
        shipStateByte[0] = 8;
    }
    pt_put(0, 0, 0, 0, 0);
    pt_speed(0, 0);
}

/* the world after a step: every ship's frame square, the layer's rules kept */
static int16_t pt_world_ok(void)
{
    static const char __far e_frame[] = "slot %d's orientation is no longer 256 long and square: ship_orthonormalise "
        "every ship that turns, every step (step %d)";
#ifdef RV_HOST
    static const char __far e_rule[] = "step %d broke a rule the multiplayer layer needs: ";
#endif
    int16_t s;
    for (s = 0; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && !pt_square(s)) {
            pt_fail(e_frame, s, frameCounter, 0);
            return 0;
        }
#ifdef RV_HOST
    if (host_rules_broken != g_rules0) {
        g_rules0 = host_rules_broken;
        if (++g_failed <= 3) {
            ln_s(s_pre);
            ln_s(s_indent);
            ln_id();
            ln_s(s_colon);
            ln_f(e_rule, frameCounter, 0, 0);
            ln_s(host_last_rule);
            ln_end();
        }
        return 0;
    }
#endif
    return 1;
}

static void pt_parts(void);

/* a step of play, as the flight loop runs it */
static int16_t pt_step(void)
{
    frameCounter = (uint16_t)(frameCounter + 1);
#ifdef RV_HOST
    host_ticks = (uint32_t)frameCounter * 91 / 150;
#endif
    logic_frame();
    if (g_steps)
        pt_parts();
    return pt_world_ok();
}

static int16_t pt_steps(int16_t n)
{
    while (n-- > 0)
        if (!pt_step())
            return 0;
    return 1;
}

/* a place on a ship's skin: along its front (f), right (r) and up (u), halves of its half box */
static void pt_on(int16_t s, int16_t f, int16_t r, int16_t u, RvVec3 *w)
{
    int32_t h = collide_half(s) / 2;
    const RvVec3 *F = &gOrientationFrontVector[s], *R = &gOrientationRightVector[s], *U = &gOrientationUpVector[s];
    w->x = gPositionVector[s].x + (F->x * f + R->x * r + U->x * u) * (h / 256);
    w->y = gPositionVector[s].y + (F->y * f + R->y * r + U->y * u) * (h / 256);
    w->z = gPositionVector[s].z + (F->z * f + R->z * r + U->z * u) * (h / 256);
}

/* a hit of qty on slot s at that place */
static uint16_t pt_hit(uint16_t src, int16_t s, uint16_t qty, int16_t f, int16_t r, int16_t u)
{
    RvVec3 w;
    pt_on(s, f, r, u, &w);
    return do_damage(src, (uint16_t)s, qty, RV_WHERE(&w));
}

/* the repair system damaged: a test that sets a system's level and needs it to hold is not racing its 20 s */
static void pt_no_repairs(void)
{
    sys_set(SYS_REPAIR, SYS_WRECKED);
}

/* ======================================================================================================== */
/* M1: flight (step_minds for the player, step_motion, step_timers) */

/* M1.1 */
static void t_coast(void)
{
    static const char __far e_vel[] = "a step's move was %d,%d,%d positions off its gVelocityVector: position += "
        "velocity, every step (the layer reads velocity as the move a step)";
    static const char __far e_len[] = "it moves %d positions a step at %d kps: KPS_STEP(speed) is %d";
    static const char __far e_dir[] = "it moves %d/256 along its nose: it should fly where it points";
    RvVec3 p0, v;
    int32_t want, got;
    int16_t i;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    if (!pt_step())
        return;
    for (i = 0; i < 30; i++) {
        p0 = gPositionVector[0];
        if (!pt_step())
            return;
        v = gVelocityVector[0];
        REQUIRE(gPositionVector[0].x - p0.x == v.x && gPositionVector[0].y - p0.y == v.y
                && gPositionVector[0].z - p0.z == v.z, e_vel, gPositionVector[0].x - p0.x - v.x,
                gPositionVector[0].y - p0.y - v.y, gPositionVector[0].z - p0.z - v.z);
    }
    want = KPS_STEP(speedLo[0]);
    got = pt_len(v.x, v.y, v.z);
    REQUIRE(got * 100 >= want * (100 - SPEED_SLACK) && got * 100 <= want * (100 + SPEED_SLACK) && want > 0, e_len, got,
            (int32_t)speedLo[0], want);
    EXPECT(vec_dot(&v, &gOrientationFrontVector[0]) / got >= 253, e_dir, vec_dot(&v, &gOrientationFrontVector[0]) / got,
           0, 0);
}

/* M1.2 */
static void t_throttle(void)
{
    static const char __far e_back[] = "the speed fell (%d to %d kps) while the throttle asked for %d";
    static const char __far e_over[] = "the speed went past the throttle: %d kps for %d";
    static const char __far e_jump[] = "the speed jumped %d kps in a step: more than a second of its accel (%d kps/s)";
    static const char __far e_once[] = "full speed in a single step (%d kps): the speed should build at the type's accel";
    static const char __far e_slow[] = "after 10 s the speed is %d kps, the throttle %d: speedLo should reach setSpeed";
    static const char __far e_stop[] = "after 10 s at a throttle of 0 the speed is still %d kps";
    const RvShipType __far *t;
    int32_t prev = 0, sp;
    int16_t i;
    pt_alone(PT_PLAYER);
    t = slot_type(0);
    setSpeed[0] = t->max_kps;
    for (i = 0; i < 10 * SEC; i++) {
        if (!pt_step())
            return;
        sp = (int32_t)speedLo[0];
        REQUIRE(sp >= prev, e_back, prev, sp, (int32_t)setSpeed[0]);
        REQUIRE(sp <= (int32_t)setSpeed[0], e_over, sp, (int32_t)setSpeed[0], 0);
        REQUIRE(sp - prev <= t->accel, e_jump, sp - prev, t->accel, 0);
        REQUIRE(i > 0 || sp < (int32_t)t->max_kps, e_once, sp, 0, 0);
        prev = sp;
    }
    REQUIRE(prev == (int32_t)t->max_kps, e_slow, prev, (int32_t)t->max_kps, 0);
    setSpeed[0] = 0;
    for (i = 0; i < 10 * SEC; i++) {
        if (!pt_step())
            return;
        sp = (int32_t)speedLo[0];
        REQUIRE(sp <= prev, e_back, prev, sp, 0);
        prev = sp;
    }
    REQUIRE(prev == 0, e_stop, prev, 0, 0);
}

/* M1.3 */
static void t_top_speed(void)
{
    static const char __far e_fast[] = "%d kps without the afterburner: its type's top speed (max_kps) is %d";
    static const char __far e_slow[] = "only %d kps after 10 s of full throttle (its top speed %d)";
    const RvShipType __far *t;
    int16_t i;
    pt_alone(PT_PLAYER);
    t = slot_type(0);
    pt_speed(0, t->cruise_kps);
    setSpeed[0] = 5000;
    for (i = 0; i < 10 * SEC; i++) {
        if (!pt_step())
            return;
        REQUIRE((int32_t)speedLo[0] <= (int32_t)t->max_kps, e_fast, (int32_t)speedLo[0], t->max_kps, 0);
    }
    REQUIRE((int32_t)speedLo[0] >= (int32_t)t->max_kps - 1, e_slow, (int32_t)speedLo[0], t->max_kps, 0);
}

/* a turn of the player's, still, by the controls (x, y, roll) for n steps: the frame before it in f0, r0, u0 */
static int16_t pt_turn(int8_t x, int8_t y, int8_t roll, int16_t n, RvVec3 *f0, RvVec3 *r0, RvVec3 *u0)
{
    pt_alone(PT_PLAYER);
    *f0 = gOrientationFrontVector[0];
    *r0 = gOrientationRightVector[0];
    *u0 = gOrientationUpVector[0];
    g_pilot.steer_x = x;
    g_pilot.steer_y = y;
    g_pilot.roll = roll;
    return pt_steps(n);
}

static const char __far e_rate[] = "it turned %d (1024 a turn) in 10 steps: %d expected (its type's %d degrees a "
    "second, dps / KPS_HZ a step, x steer / 9)";

/* M1.4 */
static void t_yaw(void)
{
    static const char __far e_roll[] = "a yaw tipped its wings: up is %d/256 of what it was";
    RvVec3 f0, r0, u0;
    int16_t got;
    int32_t want;
    if (!pt_turn(9, 0, 0, 10, &f0, &r0, &u0))
        return;
    got = pt_angle(&gOrientationFrontVector[0], &f0, &r0);
    want = pt_turn_of(10, slot_type(0)->yaw_dps, 9);
    REQUIRE(got * INT32_C(100) >= want * (100 - TURN_SLACK) && got * INT32_C(100) <= want * (100 + TURN_SLACK), e_rate,
            got, want, slot_type(0)->yaw_dps);
    EXPECT(vec_dot(&gOrientationUpVector[0], &u0) / 256 >= 248, e_roll, vec_dot(&gOrientationUpVector[0], &u0) / 256, 0,
           0);
}

/* M1.5 */
static void t_pitch(void)
{
    static const char __far e_side[] = "a pitch swung its wings round: right is %d/256 of what it was";
    RvVec3 f0, r0, u0;
    int16_t got;
    int32_t want;
    if (!pt_turn(0, 9, 0, 10, &f0, &r0, &u0))
        return;
    got = pt_angle(&gOrientationFrontVector[0], &f0, &u0);
    want = pt_turn_of(10, slot_type(0)->pitch_dps, 9);
    REQUIRE(got * INT32_C(100) >= want * (100 - TURN_SLACK) && got * INT32_C(100) <= want * (100 + TURN_SLACK), e_rate,
            got, want, slot_type(0)->pitch_dps);
    EXPECT(vec_dot(&gOrientationRightVector[0], &r0) / 256 >= 248, e_side,
           vec_dot(&gOrientationRightVector[0], &r0) / 256, 0, 0);
}

/* M1.6 */
static void t_roll(void)
{
    static const char __far e_nose[] = "a roll moved its nose: front is %d/256 of what it was";
    RvVec3 f0, r0, u0;
    int16_t got;
    int32_t want;
    if (!pt_turn(0, 0, 9, 10, &f0, &r0, &u0))
        return;
    got = pt_angle(&gOrientationRightVector[0], &r0, &u0);
    want = pt_turn_of(10, slot_type(0)->roll_dps, 9);
    REQUIRE(got * INT32_C(100) >= want * (100 - TURN_SLACK) && got * INT32_C(100) <= want * (100 + TURN_SLACK), e_rate,
            got, want, slot_type(0)->roll_dps);
    EXPECT(vec_dot(&gOrientationFrontVector[0], &f0) / 256 >= 253, e_nose,
           vec_dot(&gOrientationFrontVector[0], &f0) / 256, 0, 0);
}

/* M1.7 */
static void t_steer_steps(void)
{
    static const char __far e_none[] = "a full steer turned it %d: nothing to compare";
    static const char __far e_ratio[] = "a steer of 3 turned it %d, a steer of 9 %d: a third as far expected (the "
        "pointer's steps, steer / 9)";
    RvVec3 f0, r0, u0;
    int16_t full, third;
    if (!pt_turn(9, 0, 0, 10, &f0, &r0, &u0))
        return;
    full = pt_angle(&gOrientationFrontVector[0], &f0, &r0);
    REQUIRE(full > 4, e_none, full, 0, 0);
    if (!pt_turn(3, 0, 0, 10, &f0, &r0, &u0))
        return;
    third = pt_angle(&gOrientationFrontVector[0], &f0, &r0);
    REQUIRE(third * INT32_C(100) >= full * INT32_C(20) && third * INT32_C(100) <= full * INT32_C(50), e_ratio, third,
            full, 0);
}

/* M1.8 */
static void t_aerobatics(void)
{
    static const char __far e_still[] = "20 s of flying at cruise moved it %d km";
    static const int8_t __far k_moves[24] = { 9, 0, 0, 0, 9, 0, 0, 0, 9, -9, 5, -9, 4, -9, 9, 9, 9, 9, -3, -3, 0, 0,
                                              -9, 5 };
    int16_t i, k;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    for (i = 0; i < 20 * SEC; i++) {
        k = (int16_t)(i / 25 % 8) * 3;
        g_pilot.steer_x = k_moves[k];
        g_pilot.steer_y = k_moves[k + 1];
        g_pilot.roll = k_moves[k + 2];
        if (!pt_step())
            return;
    }
    REQUIRE(pt_km_to(0, 0, 0, 0) >= 300, e_still, pt_km_to(0, 0, 0, 0), 0, 0);
}

/* M1.9 */
static void t_afterburner(void)
{
    static const char __far e_ab[] = "%d kps with the afterburner lit: more than its ab_kps, %d";
    static const char __far e_slow[] = "%d kps after 5 s of afterburner: no faster than its top speed (%d)";
    static const char __far e_back[] = "%d kps 20 s after the afterburner went out: back to the throttle (%d) expected";
    const RvShipType __far *t;
    int16_t i;
    pt_alone(PT_PLAYER);
    t = slot_type(0);
    pt_speed(0, t->max_kps);
    g_pilot.afterburner = g_sys.burning = 1;   /* (Tab held: flight.c lights both) */
    rvAfterburner[0] = 1;
    for (i = 0; i < 5 * SEC; i++) {
        if (!pt_step())
            return;
        REQUIRE((int32_t)speedLo[0] <= (int32_t)t->ab_kps, e_ab, (int32_t)speedLo[0], t->ab_kps, 0);
    }
    REQUIRE((int32_t)speedLo[0] > (int32_t)t->max_kps, e_slow, (int32_t)speedLo[0], t->max_kps, 0);
    g_pilot.afterburner = g_sys.burning = 0;
    rvAfterburner[0] = 0;
    if (!pt_steps(20 * SEC))
        return;
    REQUIRE((int32_t)speedLo[0] <= (int32_t)setSpeed[0], e_back, (int32_t)speedLo[0], (int32_t)setSpeed[0], 0);
}

/* M1.10 */
static void t_puppet(void)
{
    static const char __far e_move[] = "a puppet (CTL_PUPPET) is %d,%d,%d positions off where its velocity takes it: "
        "it only coasts, position += velocity (the layer flies it)";
    static const char __far e_turn[] = "a puppet's orientation changed (front.x %d to %d): the layer turns it, not the "
        "logic";
    static const char __far e_fire[] = "a puppet fired %d shots: it does nothing of its own";
    static const char __far e_think[] = "the AI thought for a puppet %d times (aiFighterThink): the layer flies it";
    RvVec3 p0, f0, v;
    int32_t calls;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_PUPPET, 1000, 0, 5000, TURN / 8);
    v.x = 100;
    v.y = -50;
    v.z = 2000;
    gVelocityVector[1] = v;
    p0 = gPositionVector[1];
    f0 = gOrientationFrontVector[1];
    if (!pt_steps(30))
        return;
    REQUIRE(gPositionVector[1].x == p0.x + 30 * v.x && gPositionVector[1].y == p0.y + 30 * v.y
            && gPositionVector[1].z == p0.z + 30 * v.z, e_move, gPositionVector[1].x - p0.x - 30 * v.x,
            gPositionVector[1].y - p0.y - 30 * v.y, gPositionVector[1].z - p0.z - 30 * v.z);
    EXPECT(abs32(gOrientationFrontVector[1].x - f0.x) <= 2 && abs32(gOrientationFrontVector[1].z - f0.z) <= 2, e_turn,
           f0.x, gOrientationFrontVector[1].x, 0);
    EXPECT(pt_shots_of(1) == 0, e_fire, pt_shots_of(1), 0, 0);
    calls = pt_calls(RV_FN_aiFighterThink, 0, 1);
    EXPECT(calls <= 0, e_think, calls, 0, 0);
}

/* M1.11 */
static void t_frozen(void)
{
    static const char __far e_ctl[] = "the player did not move either (%d km): nothing flies yet";
    static const char __far e_move[] = "a frozen ship (CTL_FROZEN) moved %d km: it does nothing";
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 2000, 0, 8000, 0);
    pt_speed(1, 300);
    rvController[1] = CTL_FROZEN;
    if (!pt_steps(30))
        return;
    REQUIRE(pt_km_to(0, 0, 0, 0) > 10, e_ctl, pt_km_to(0, 0, 0, 0), 0, 0);
    REQUIRE(pt_km_to(1, 2000, 0, 8000) == 0, e_move, pt_km_to(1, 2000, 0, 8000), 0, 0);
}

/* --- the turns, more closely: step by step (a turn measured only over many steps can wrap round a whole turn and
 * look right), both ways, there and back, slow, about the ship's own axes, two at once, held still, while flying, and
 * after a slot's ship is replaced --- */

/* the player's type's rate for axis k (0 yaw, 1 pitch, 2 roll), degrees a second */
static int16_t pt_dps(int16_t k)
{
    const RvShipType __far *t = slot_type(0);
    return k == 0 ? t->yaw_dps : k == 1 ? t->pitch_dps : t->roll_dps;
}

/* the player's controls: a steer of v on axis k alone */
static void pt_steer(int16_t k, int16_t v)
{
    g_pilot.steer_x = (int8_t)(k == 0 ? v : 0);
    g_pilot.steer_y = (int8_t)(k == 1 ? v : 0);
    g_pilot.roll = (int8_t)(k == 2 ? v : 0);
}

/* how far axis k's turn has taken the player's frame from (f, r, u), 1024 a turn: a yaw moves the nose toward the
 * right, a pitch the nose toward up, a roll the right wing toward up */
static int16_t pt_axis_angle(int16_t k, const RvVec3 *f, const RvVec3 *r, const RvVec3 *u)
{
    return k == 0 ? pt_angle(&gOrientationFrontVector[0], f, r)
                  : k == 1 ? pt_angle(&gOrientationFrontVector[0], f, u) : pt_angle(&gOrientationRightVector[0], r, u);
}

/* got within TURN_SLACK percent of want, either way round */
static int16_t pt_rate_ok(int32_t got, int32_t want)
{
    if (want < 0) {
        got = -got;
        want = -want;
    }
    return want > 0 && got * 100 >= want * (100 - TURN_SLACK) && got * 100 <= want * (100 + TURN_SLACK);
}

/* the largest change of any component of the player's frame from (f, r, u), in 256ths */
static int32_t pt_frame_moved(const RvVec3 *f, const RvVec3 *r, const RvVec3 *u)
{
    const RvVec3 *v[3], *w[3];
    int32_t d, most = 0;
    int16_t i;
    v[0] = &gOrientationFrontVector[0];
    v[1] = &gOrientationRightVector[0];
    v[2] = &gOrientationUpVector[0];
    w[0] = f;
    w[1] = r;
    w[2] = u;
    for (i = 0; i < 3; i++) {
        d = abs32(v[i]->x - w[i]->x);
        most = d > most ? d : most;
        d = abs32(v[i]->y - w[i]->y);
        most = d > most ? d : most;
        d = abs32(v[i]->z - w[i]->z);
        most = d > most ? d : most;
    }
    return most;
}

static const char __far e_step[] = "a step of a full turn (axis %d: 0 yaw, 1 pitch, 2 roll) turned %d (1024 a turn) "
    "where a step's share is %d: none backwards, none twice its share (TURN_STEP gives 256ths of a unit)";

/* a full steer on axis k (dir 1 or -1) for n steps from wherever the player's frame is, each step's turn checked: the
 * right way and at most twice a step's share (so a turn wrapped round, or 256 times too big, shows); the steps' sum
 * (1024 a turn) in *sum.  0 when a step failed */
static int16_t pt_turn_steps(int16_t k, int16_t dir, int16_t n, int32_t *sum)
{
    RvVec3 f, r, u;
    int32_t share = pt_turn_of(1, pt_dps(k), 9);
    int16_t i, a;
    pt_steer(k, (int16_t)(9 * dir));
    *sum = 0;
    for (i = 0; i < n; i++) {
        f = gOrientationFrontVector[0];
        r = gOrientationRightVector[0];
        u = gOrientationUpVector[0];
        if (!pt_step())
            return 0;
        a = pt_axis_angle(k, &f, &r, &u);
        if (a * dir < 0 || a * dir > 2 * share + 2) {
            pt_fail(e_step, k, a, share * dir);
            return 0;
        }
        *sum += a;
    }
    return 1;
}

/* M1.12 */
static void t_turn_steps(void)
{
    static const char __far e_sum[] = "a second of a full turn (axis %d: 0 yaw, 1 pitch, 2 roll) turned %d (1024 a turn) "
        "step by step: %d expected";
    int16_t k;
    int32_t sum, want;
    for (k = 0; k < 3; k++) {
        pt_alone(PT_PLAYER);
        if (!pt_turn_steps(k, 1, SEC, &sum))
            return;
        want = pt_turn_of(SEC, pt_dps(k), 9);
        REQUIRE(pt_rate_ok(sum, want), e_sum, k, sum, want);
    }
}

/* M1.13 */
static void t_turn_back(void)
{
    static const char __far e_back[] = "a second of a steer of -9 (axis %d: 0 yaw, 1 pitch, 2 roll) turned %d (1024 a "
        "turn): %d expected, the other way as fast";
    int16_t k;
    int32_t sum, want;
    for (k = 0; k < 3; k++) {
        pt_alone(PT_PLAYER);
        if (!pt_turn_steps(k, -1, SEC, &sum))
            return;
        want = -pt_turn_of(SEC, pt_dps(k), 9);
        REQUIRE(pt_rate_ok(sum, want), e_back, k, sum, want);
    }
}

/* M1.14 */
static void t_there_and_back(void)
{
    static const char __far e_off[] = "a second of a full turn (axis %d: 0 yaw, 1 pitch, 2 roll) and a second back left "
        "it %d off where it began (1024 a turn): within %d expected";
    RvVec3 f, r, u;
    int16_t k, off;
    int32_t sum;
    for (k = 0; k < 3; k++) {
        pt_alone(PT_PLAYER);
        f = gOrientationFrontVector[0];
        r = gOrientationRightVector[0];
        u = gOrientationUpVector[0];
        if (!pt_turn_steps(k, 1, SEC, &sum) || !pt_turn_steps(k, -1, SEC, &sum))
            return;
        off = pt_axis_angle(k, &f, &r, &u);
        REQUIRE(abs32(off) <= 3, e_off, k, off, 3);
    }
}

/* M1.15 */
static void t_slow_turns(void)
{
    static const char __far e_cap[] = "a capital ship (yaw %d degrees a second: under a unit a step) turned %d in 2 s "
        "(1024 a turn): %d expected (carry the fraction from step to step)";
    static const char __far e_one[] = "a steer of 1 turned it %d in 2 s (1024 a turn): %d expected, a ninth of a full "
        "steer (the fraction carried)";
    RvVec3 f, r, u;
    int16_t got;
    int32_t want;
    pt_alone(PT_CAPSHIP);
    f = gOrientationFrontVector[0];
    r = gOrientationRightVector[0];
    u = gOrientationUpVector[0];
    pt_steer(0, 9);
    if (!pt_steps(2 * SEC))
        return;
    got = pt_axis_angle(0, &f, &r, &u);
    want = pt_turn_of(2 * SEC, slot_type(0)->yaw_dps, 9);
    REQUIRE(pt_rate_ok(got, want), e_cap, slot_type(0)->yaw_dps, got, want);
    pt_alone(PT_PLAYER);
    f = gOrientationFrontVector[0];
    r = gOrientationRightVector[0];
    u = gOrientationUpVector[0];
    pt_steer(0, 1);
    if (!pt_steps(2 * SEC))
        return;
    got = pt_axis_angle(0, &f, &r, &u);
    want = pt_turn_of(2 * SEC, slot_type(0)->yaw_dps, 1);
    REQUIRE(pt_rate_ok(got, want), e_one, got, want, 0);
}

/* M1.16 */
static void t_body_axes(void)
{
    static const char __far e_axes[] = "rolled a quarter turn (right wing up), a full %d-axis turn (0 yaw, 1 pitch) took "
        "the nose %d toward the ship's own way and %d across it (1024 a turn): turns go about the ship's own axes";
    RvVec3 f, r, u;
    int16_t k, along, across;
    for (k = 0; k < 2; k++) {
        pt_alone(PT_PLAYER);
        /* (a quarter turn of roll: the right wing to the sky, up to the left; the nose as it was) */
        gOrientationRightVector[0].x = 0;
        gOrientationRightVector[0].y = ONE;
        gOrientationRightVector[0].z = 0;
        gOrientationUpVector[0].x = -ONE;
        gOrientationUpVector[0].y = 0;
        gOrientationUpVector[0].z = 0;
        f = gOrientationFrontVector[0];
        r = gOrientationRightVector[0];
        u = gOrientationUpVector[0];
        pt_steer(k, 9);
        if (!pt_steps(10))
            return;
        along = k == 0 ? pt_angle(&gOrientationFrontVector[0], &f, &r) : pt_angle(&gOrientationFrontVector[0], &f, &u);
        across = k == 0 ? pt_angle(&gOrientationFrontVector[0], &f, &u) : pt_angle(&gOrientationFrontVector[0], &f, &r);
        REQUIRE(pt_rate_ok(along, pt_turn_of(10, pt_dps(k), 9)) && abs32(across) <= 8, e_axes, k, along, across);
    }
}

/* M1.17 */
static void t_two_axes(void)
{
    static const char __far e_two[] = "yaw and pitch together for 5 steps took the nose %d toward its right and %d toward "
        "its up (1024 a turn): about %d each expected (each axis at its own rate)";
    RvVec3 f, r, u;
    int16_t yaw, pitch;
    int32_t want_yaw, want_pitch;
    pt_alone(PT_PLAYER);
    f = gOrientationFrontVector[0];
    r = gOrientationRightVector[0];
    u = gOrientationUpVector[0];
    g_pilot.steer_x = 9;
    g_pilot.steer_y = 9;
    if (!pt_steps(5))
        return;
    yaw = pt_angle(&gOrientationFrontVector[0], &f, &r);
    pitch = pt_angle(&gOrientationFrontVector[0], &f, &u);
    want_yaw = pt_turn_of(5, pt_dps(0), 9);
    want_pitch = pt_turn_of(5, pt_dps(1), 9);
    REQUIRE(pt_rate_ok(yaw, want_yaw) && pt_rate_ok(pitch, want_pitch), e_two, yaw, pitch, want_yaw);
}

/* M1.18 */
static void t_steady(void)
{
    static const char __far e_drift[] = "with the controls centred after a turn, %d steps on its frame had moved %d/256 "
        "(its largest change): it holds still, within %d";
    RvVec3 f, r, u;
    int16_t i;
    int32_t moved;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    g_pilot.steer_x = 9;
    g_pilot.steer_y = -5;
    g_pilot.roll = 7;
    if (!pt_steps(10))
        return;
    pt_steer(0, 0);
    if (!pt_step())                     /* (a step for a turn decided before the controls came back) */
        return;
    f = gOrientationFrontVector[0];
    r = gOrientationRightVector[0];
    u = gOrientationUpVector[0];
    for (i = 0; i < 20 * SEC; i++) {
        if (!pt_step())
            return;
        moved = pt_frame_moved(&f, &r, &u);
        REQUIRE(moved <= 2, e_drift, i + 1, moved, 2);
    }
}

/* M1.19 */
static void t_turn_flight(void)
{
    static const char __far e_way[] = "turning hard at cruise, step %d's move was %d/256 along the nose (the cosine): at "
        "least %d expected below the top speed, within 14 degrees (a slide, if any, is past it)";
    RvVec3 p0, d;
    int16_t i;
    int32_t len, c;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    g_pilot.steer_x = 9;
    g_pilot.steer_y = 9;
    for (i = 0; i < 2 * SEC; i++) {
        p0 = gPositionVector[0];
        if (!pt_step())
            return;
        d.x = gPositionVector[0].x - p0.x;
        d.y = gPositionVector[0].y - p0.y;
        d.z = gPositionVector[0].z - p0.z;
        len = pt_len(d.x, d.y, d.z);
        c = len ? vec_dot(&d, &gOrientationFrontVector[0]) / len : 0;
        REQUIRE(c >= 248, e_way, i + 1, c, 248);
    }
}

/* M1.20 */
static void t_respawn(void)
{
    static const char __far e_same[] = "a ship spawned into slot 0 turned otherwise (its frame %d/256 off, step %d) than "
        "one spawned into a fresh slot: reset what you keep for a slot (its turn's fraction) when a ship is spawned in";
    RvVec3 f[10], r[10], u[10];
    int16_t pass, i;
    int32_t moved;
    for (pass = 0; pass < 2; pass++) {
        pt_alone(PT_PLAYER);
        if (pass == 1) {
            /* (the slot's last ship turned a step on every axis: its fractions left behind) */
            g_pilot.steer_x = 9;
            g_pilot.steer_y = 7;
            g_pilot.roll = -5;
            if (!pt_step())
                return;
        }
        ship_spawn(0, PT_PLAYER, SIDE_TERRAN, CTL_PLAYER, 0, 0, 0, 0);   /* (as the layer puts one in: spawn 6) */
        shipStateByte[0] = 8;
        g_pilot.steer_x = 9;
        g_pilot.steer_y = 9;
        g_pilot.roll = 9;
        for (i = 0; i < 10; i++) {
            if (!pt_step())
                return;
            if (pass == 0) {
                f[i] = gOrientationFrontVector[0];
                r[i] = gOrientationRightVector[0];
                u[i] = gOrientationUpVector[0];
            } else {
                moved = pt_frame_moved(&f[i], &r[i], &u[i]);
                REQUIRE(moved == 0, e_same, moved, i + 1, 0);
            }
        }
    }
}

/* M1.21 */
static void t_full_house(void)
{
    static const char __far e_spawn[] = "ship_spawn into slot %d returned %d: every slot 1 .. %d (RV_MAX_SHIP_SLOT) takes a "
        "ship, and its number comes back";
    static const char __far e_free[] = "ship_free_slot() is %d with every ship slot full: -1 expected";
    static const char __far e_move[] = "slot %d of a full house moved %d km in 2 s at %d kps: every ship slot flies";
    int16_t s, got;
    int32_t km;
    pt_alone(PT_PLAYER);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
        got = ship_spawn(s, PT_ENEMY, SIDE_AERA, CTL_AI, (int32_t)s * 2000 * KM, 0, INT32_C(30000) * KM, 0);
        REQUIRE(got == s && slot_is_ship(s), e_spawn, s, got, RV_MAX_SHIP_SLOT);
    }
    REQUIRE(ship_free_slot() == -1, e_free, ship_free_slot(), 0, 0);
    if (!pt_steps(2 * SEC))
        return;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
        km = pt_km_to(s, (int32_t)s * 2000, 0, 30000);
        REQUIRE(km > 100, e_move, s, km, (int32_t)speedLo[s]);
    }
}

/* M1.22 */
static void t_slot_limit(void)
{
    static const int16_t __far k_past[4] = { RV_MAX_SHIP_SLOT + 1, RV_SLOTS - 1, RV_SLOTS, -1 };
    static const char __far e_past[] = "ship_spawn into slot %d returned %d: -1 expected, the ship slots are 0 .. %d";
    static const char __far e_touched[] = "a refused ship_spawn into slot %d made it entity type %d (it was %d): it leaves "
        "the slot as it was";
    static const char __far e_full[] = "with every ship slot full, outerSpawnShipEntity(1, 0) returned %d: 0xFFFF (no "
        "room) expected";
    static const char __far e_kept[] = "a spawn refused for want of room changed ship slot %d (now type %d, side %d)";
    int16_t i, s, got;
    uint16_t et, spawned;
    pt_mission();
    pt_mship(PT_ENEMY, SIDE_AERA, 0, 1, RV_PILOT_NONE);   /* (a mission ship to ask for: record 1, at nav 0) */
    pt_launch(PT_PLAYER);
    /* no ship past the ship slots: refused, the slot as it was */
    for (i = 0; i < 4; i++) {
        s = k_past[i];
        et = s >= 0 && s < RV_SLOTS ? maybeEntityType[s] : 0;
        got = ship_spawn(s, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 0, 0);
        REQUIRE(got == -1, e_past, s, got, RV_MAX_SHIP_SLOT);
        if (s >= 0 && s < RV_SLOTS)
            REQUIRE(maybeEntityType[s] == et, e_touched, s, maybeEntityType[s], et);
    }
    /* the logic's spawn hook takes any slot number (only ship slots keep anything: the host's sanitizers catch a write
     * past its arrays) */
    logic_prepare_spawn(RV_MAX_SHIP_SLOT + 1);
    logic_prepare_spawn(RV_SLOTS + 50);
    logic_prepare_spawn(-1);
    /* every ship slot full: no room for another */
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        (void)ship_spawn(s, PT_WEAK, SIDE_TERRAN, CTL_FROZEN, (int32_t)s * 2000 * KM, 0, 0, 0);
    spawned = outerSpawnShipEntity(1, 0);
    REQUIRE(spawned == 0xFFFF, e_full, spawned, 0, 0);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        REQUIRE(gInstanceShipTypes[s] == PT_WEAK && commAnimInfo[s] == SIDE_TERRAN, e_kept, s, gInstanceShipTypes[s],
                commAnimInfo[s]);
    if (!pt_step())
        return;
}

/* ======================================================================================================== */
/* M2: guns (step_fire, fireGunFromShip, maybe_fire_all_guns, step_timers, step_shots) */

/* M2.1 */
static void t_fire_call(void)
{
    static const char __far e_none[] = "fireGunFromShip(0, 0) put %d shots in the world: one expected (shot_add)";
    static const char __far e_ret[] = "fireGunFromShip returned %d for a gun: 0xFFFF expected (a slot is a missile's)";
    static const char __far e_own[] = "the shot's owner is %d, its weapon %d: the ship (0) and its gun's (%d)";
    static const char __far e_far[] = "the shot starts %d km from the ship: within its size (%d km) expected";
    static const char __far e_speed[] = "the shot moves %d positions a step: the weapon's speed is %d, the ship's own "
        "%d";
    static const char __far e_way[] = "the shot flies %d/256 along the ship's nose";
    Shot __far *s;
    int16_t n0;
    uint16_t r;
    int32_t v, lo, ship;
    RvVec3 sv;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    if (!pt_step())
        return;
    n0 = shots_count();
    r = fireGunFromShip(0, 0);
    REQUIRE(shots_count() == n0 + 1, e_none, shots_count() - n0, 0, 0);
    EXPECT(r == 0xFFFF, e_ret, r, 0, 0);
    s = shot(n0);
    REQUIRE(s->owner == 0 && s->weapon == GUN_TYPE(0, 0), e_own, s->owner, s->weapon, GUN_TYPE(0, 0));
    REQUIRE(pt_len((s->x - gPositionVector[0].x) / 256, (s->y - gPositionVector[0].y) / 256,
                   (s->z - gPositionVector[0].z) / 256) <= 2 * collide_half(0) / 256, e_far,
            pt_len((s->x - gPositionVector[0].x) / 256, (s->y - gPositionVector[0].y) / 256,
                   (s->z - gPositionVector[0].z) / 256), 2 * collide_half(0) / 256, 0);
    sv.x = s->vx;
    sv.y = s->vy;
    sv.z = s->vz;
    v = pt_len(sv.x, sv.y, sv.z);
    lo = KPS_STEP(weapon(GUN_TYPE(0, 0))->speed_kps);
    ship = pt_len(gVelocityVector[0].x, gVelocityVector[0].y, gVelocityVector[0].z);
    REQUIRE(v * 100 >= lo * 97 && v * 100 <= (lo + ship) * 103 + 100, e_speed, v, lo, ship);
    REQUIRE(v > 0 && vec_dot(&sv, &gOrientationFrontVector[0]) / v >= 243, e_way,
            v ? vec_dot(&sv, &gOrientationFrontVector[0]) / v : 0, 0, 0);
}

/* M2.2 */
static void t_never_refuses(void)
{
    static const char __far e_refused[] = "fireGunFromShip refused a gun with no energy that was cooling: it fires "
        "whatever, the caller decides (the layer replays the call)";
    static const char __far e_broke[] = "firing marked the gun destroyed (GUN_STATE bit 7)";
    static const char __far e_wrap[] = "gun energy went below 0 to %d";
    int16_t n0;
    pt_alone(PT_PLAYER);
    gunEnergy[0] = 0;
    GUN_STATE(0, 0) = 50;
    n0 = shots_count();
    (void)fireGunFromShip(0, 0);
    REQUIRE(shots_count() == n0 + 1, e_refused, 0, 0, 0);
    EXPECT(!(GUN_STATE(0, 0) & GUN_DESTROYED), e_broke, 0, 0, 0);
    EXPECT(gunEnergy[0] <= 100, e_wrap, gunEnergy[0], 0, 0);
}

/* M2.3 */
static void t_cooldown(void)
{
    static const char __far e_none[] = "firing set no cooldown in GUN_STATE's bits 0-6 (the layer copies gun records "
        "whole: the cooldown lives there)";
    static const char __far e_long[] = "still cooling after %d steps: its refire is %d steps";
    static const char __far e_short[] = "cooled in %d steps: its refire is %d";
    static const char __far e_bit[] = "the timers cleared a destroyed gun's bit 7 (GUN_STATE %d)";
    int16_t n, refire;
    pt_alone(PT_PLAYER);
    refire = weapon(GUN_TYPE(0, 0))->refire;
    (void)fireGunFromShip(0, 0);
    REQUIRE(GUN_STATE(0, 0) & 0x7F, e_none, 0, 0, 0);
    GUN_STATE(0, 1) = GUN_DESTROYED | 5;
    for (n = 0; n < 4 * refire + 10 && (GUN_STATE(0, 0) & 0x7F); n++)
        if (!pt_step())
            return;
    REQUIRE(!(GUN_STATE(0, 0) & 0x7F), e_long, n, refire, 0);
    EXPECT(n <= 2 * refire + 1, e_long, n, refire, 0);
    EXPECT(n >= refire / 2, e_short, n, refire, 0);
    EXPECT(GUN_STATE(0, 1) & GUN_DESTROYED, e_bit, GUN_STATE(0, 1), 0, 0);
}

/* M2.4 */
static void t_energy(void)
{
    static const char __far e_free[] = "firing spent no gun energy (gunEnergy %d)";
    static const char __far e_fell[] = "gun energy fell (%d to %d) with no gun firing";
    static const char __far e_over[] = "gun energy %d: 100 at most";
    static const char __far e_slow[] = "gun energy back to only %d of 100 after 20 s";
    int16_t i;
    int32_t prev;
    pt_alone(PT_PLAYER);
    (void)fireGunFromShip(0, 0);
    REQUIRE(gunEnergy[0] < 100, e_free, gunEnergy[0], 0, 0);
    prev = gunEnergy[0];
    for (i = 0; i < 20 * SEC; i++) {
        if (!pt_step())
            return;
        REQUIRE((int32_t)gunEnergy[0] >= prev, e_fell, prev, gunEnergy[0], 0);
        REQUIRE(gunEnergy[0] <= 100, e_over, gunEnergy[0], 0, 0);
        prev = gunEnergy[0];
    }
    REQUIRE(gunEnergy[0] == 100, e_slow, gunEnergy[0], 0, 0);
}

/* M2.5 */
static void t_trigger(void)
{
    static const char __far e_first[] = "the trigger's first step fired %d shots: each of its %d guns should fire";
    static const char __far e_fast[] = "%d shots in 2 s: more than its guns' refire allows (%d)";
    static const char __far e_few[] = "only %d shots in 2 s of trigger";
    static const char __far e_after[] = "%d shots after the trigger was let go";
    static const char __far e_missile[] = "the trigger launched a missile: missiles are the missile key's";
    uint16_t a0;
    int16_t guns, refire;
    int32_t most;
    pt_alone(PT_PLAYER);
    guns = pt_guns(0);
    refire = weapon(GUN_TYPE(0, 0))->refire;
    g_sys.armed = 1;
    g_pilot.guns = 1;
    a0 = shots_added();
    if (!pt_step())
        return;
    REQUIRE((int16_t)(shots_added() - a0) >= guns, e_first, (uint16_t)(shots_added() - a0), guns, 0);
    if (!pt_steps(2 * SEC - 1))
        return;
    most = (int32_t)guns * (2 * SEC / refire + 1);
    REQUIRE((int32_t)(uint16_t)(shots_added() - a0) <= most, e_fast, (uint16_t)(shots_added() - a0), most, 0);
    REQUIRE((int16_t)(shots_added() - a0) >= 2 * guns, e_few, (uint16_t)(shots_added() - a0), 0, 0);
    EXPECT(pt_missile_of(0) < 0, e_missile, 0, 0, 0);
    g_pilot.guns = 0;
    a0 = shots_added();
    if (!pt_steps(10))
        return;
    EXPECT(shots_added() == a0, e_after, (uint16_t)(shots_added() - a0), 0, 0);
}

/* M2.6 */
static void t_safety(void)
{
    static const char __far e_none[] = "no shots with the master arm on either: the trigger fires nothing yet";
    static const char __far e_safe[] = "%d shots with the master arm off (g_sys.armed 0)";
    uint16_t a0, off;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    g_pilot.guns = 1;
    a0 = shots_added();
    if (!pt_steps(30))
        return;
    off = (uint16_t)(shots_added() - a0);
    g_sys.armed = 1;
    a0 = shots_added();
    if (!pt_steps(30))
        return;
    REQUIRE(shots_added() != a0, e_none, 0, 0, 0);
    REQUIRE(off == 0, e_safe, off, 0, 0);
}

/* M2.7 */
static void t_broken_gun(void)
{
    static const char __far e_two[] = "the test wants guns 0 and 1 on opposite sides (mounts %d and %d)";
    static const char __far e_none[] = "no shots from the guns left";
    static const char __far e_from[] = "a shot from the destroyed gun 0's side (%d km across; gun 1's mount is %d)";
    int16_t i, k, side;
    uint16_t a0;
    pt_alone(PT_PLAYER);
    REQUIRE(GUN_COUNT(0) >= 2 && pt_is_gun(0, 0) && pt_is_gun(0, 1)
            && (GUN_MOUNT_X(0, 0) < 0) != (GUN_MOUNT_X(0, 1) < 0), e_two, GUN_MOUNT_X(0, 0), GUN_MOUNT_X(0, 1), 0);
    side = GUN_MOUNT_X(0, 1) < 0 ? -1 : 1;
    GUN_STATE(0, 0) |= GUN_DESTROYED;
    g_sys.armed = 1;
    g_pilot.guns = 1;
    a0 = shots_added();
    for (i = 0; i < 30; i++) {
        if (!pt_step())
            return;
        for (k = 0; k < shots_count(); k++)
            REQUIRE(shot(k)->owner != 0 || (shot(k)->x - gPositionVector[0].x) * side >= 0, e_from,
                    (shot(k)->x - gPositionVector[0].x) / 256, GUN_MOUNT_X(0, 1), 0);
    }
    REQUIRE(shots_added() != a0, e_none, 0, 0, 0);
}

/* M2.8 */
static void t_all_guns(void)
{
    static const char __far e_n[] = "maybe_fire_all_guns fired %d shots: the ship has %d guns ready";
    static const char __far e_missile[] = "maybe_fire_all_guns launched a missile (launcher rounds %d to %d): guns only";
    static const char __far e_again[] = "fired %d more at once: the guns were still cooling";
    uint16_t a0;
    int16_t l, r0;
    pt_alone(PT_PLAYER);
    l = pt_launcher(0);
    r0 = l >= 0 ? GUN_STATE(0, l) : 0;
    a0 = shots_added();
    maybe_fire_all_guns(0);
    REQUIRE((int16_t)(shots_added() - a0) == pt_guns(0), e_n, (uint16_t)(shots_added() - a0), pt_guns(0), 0);
    REQUIRE(pt_missile_of(0) < 0 && (l < 0 || GUN_STATE(0, l) == r0), e_missile, r0, l >= 0 ? GUN_STATE(0, l) : 0, 0);
    a0 = shots_added();
    maybe_fire_all_guns(0);
    REQUIRE(shots_added() == a0, e_again, (uint16_t)(shots_added() - a0), 0, 0);
}

/* M2.9 */
static void t_range(void)
{
    static const char __far e_gone[] = "the shot was gone after one step";
    static const char __far e_left[] = "%d shots still flying %d steps after a shot of %d steps' life: step_shots moves "
        "them (shots_move), which ends them";
    static const char __far e_none[] = "fireGunFromShip(0, 0) put no shot in the world";
    const RvWeapon __far *w;
    int16_t life;
    pt_alone(PT_PLAYER);
    w = weapon(GUN_TYPE(0, 0));
    life = (int16_t)((int32_t)w->range_km * KPS_HZ / w->speed_kps);
    (void)fireGunFromShip(0, 0);
    REQUIRE(shots_count() == 1, e_none, 0, 0, 0);
    if (!pt_step())
        return;
    REQUIRE(shots_count() >= 1, e_gone, 0, 0, 0);
    if (!pt_steps(life + 4))
        return;
    REQUIRE(shots_count() == 0, e_left, shots_count(), life + 5, life);
}

/* --- the guns' edges: a launcher's low bits are rounds, bit 7 is kept, energy gates each gun, the mounts are the
 * ship's own, turrets are guns, and a fire changes the gun the same on every machine --- */

/* M2.10 */
static void t_launcher_rounds(void)
{
    static const char __far e_none[] = "the Schroedinger has no launcher to look at";
    static const char __far e_idle[] = "a gun's cooldown stayed %d for 3 s: nothing counts down yet, nothing to "
        "compare (M2.3 first)";
    static const char __far e_rounds[] = "a launcher's GUN_STATE went from %d to %d in 3 s of nobody firing: its low "
        "bits are its rounds (only guns' and turrets' count down)";
    int16_t l;
    uint8_t r0;
    pt_alone(PT_PLAYER);
    l = pt_launcher(0);
    REQUIRE(l >= 0, e_none, 0, 0, 0);
    r0 = GUN_STATE(0, l);
    GUN_STATE(0, 0) = (uint8_t)((GUN_STATE(0, 0) & GUN_DESTROYED) | 5);   /* (a gun cooling: the timers must run) */
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE(!(GUN_STATE(0, 0) & 0x7F), e_idle, GUN_STATE(0, 0) & 0x7F, 0, 0);
    REQUIRE(GUN_STATE(0, l) == r0, e_rounds, r0, GUN_STATE(0, l), 0);
}

/* M2.11 */
static void t_destroyed_bit(void)
{
    static const char __far e_bit[] = "firing a gun marked destroyed (GUN_STATE %d before) left GUN_STATE %d: bit 7 "
        "stays when the cooldown goes into bits 0-6";
    static const char __far e_cool[] = "firing set no cooldown (GUN_STATE %d)";
    pt_alone(PT_PLAYER);
    GUN_STATE(0, 0) = GUN_DESTROYED;    /* (as the layer may hand a record over: the caller decides, not the fire) */
    (void)fireGunFromShip(0, 0);
    REQUIRE(GUN_STATE(0, 0) & GUN_DESTROYED, e_bit, GUN_DESTROYED, GUN_STATE(0, 0), 0);
    REQUIRE(GUN_STATE(0, 0) & 0x7F, e_cool, GUN_STATE(0, 0), 0, 0);
}

/* the guns linked with gun record g (its weapon type's, guns and turrets, not destroyed): how many */
static int16_t pt_group(int16_t s, int16_t g)
{
    int16_t h, n = 0;
    for (h = 0; h < GUN_COUNT(s); h++)
        if (pt_is_gun(s, h) && GUN_TYPE(s, h) == GUN_TYPE(s, g) && !(GUN_STATE(s, h) & GUN_DESTROYED))
            n++;
    return n;
}

/* the player's first gun record that is a gun and not destroyed, -1 none */
static int16_t pt_first_gun(int16_t s)
{
    int16_t g;
    for (g = 0; g < GUN_COUNT(s); g++)
        if (pt_is_gun(s, g) && !(GUN_STATE(s, g) & GUN_DESTROYED))
            return g;
    return -1;
}

static const char __far e_lone[] = "the ship (type %d) has %d gun(s) of its first gun's type: nothing to link (a group "
    "is two or more of one weapon type)";

/* M2.12 */
static void t_energy_gate(void)
{
    static const char __far e_short[] = "with gun energy %d, under a shot's %d, maybe_fire_all_guns fired %d: a gun "
        "needs the energy for its shot";
    static const char __far e_part[] = "with energy for all but one of the group's shots (%d of %d), "
        "maybe_fire_all_guns fired %d: linked guns fire together or not at all (part of them fires lopsided)";
    static const char __far e_all[] = "with the group's energy (%d, %d guns) maybe_fire_all_guns fired %d: all of "
        "them, together";
    static const char __far e_back[] = "after recharging (gun energy %d) the trigger fired %d: it fires again";
    const RvWeapon __far *w;
    uint16_t a0;
    int16_t i, g, n;
    pt_alone(PT_PLAYER);
    g = pt_first_gun(0);
    n = g >= 0 ? pt_group(0, g) : 0;
    REQUIRE(n >= 2, e_lone, PT_PLAYER, n, 0);
    w = weapon(GUN_TYPE(0, g));
    if (!w || w->energy < 1)
        return;                         /* (guns that cost nothing: nothing to gate) */
    gunEnergy[0] = (uint16_t)(w->energy - 1);
    a0 = shots_added();
    maybe_fire_all_guns(0);
    REQUIRE(shots_added() == a0, e_short, gunEnergy[0], w->energy, (uint16_t)(shots_added() - a0));
    gunEnergy[0] = (uint16_t)(n * w->energy - 1);
    a0 = shots_added();
    maybe_fire_all_guns(0);
    REQUIRE(shots_added() == a0, e_part, n * w->energy - 1, n * w->energy, (uint16_t)(shots_added() - a0));
    gunEnergy[0] = (uint16_t)(n * w->energy);
    a0 = shots_added();
    maybe_fire_all_guns(0);
    REQUIRE((int16_t)(shots_added() - a0) == n, e_all, n * w->energy, n, (uint16_t)(shots_added() - a0));
    for (i = 0; i < 20 * SEC && (gunEnergy[0] < 100 || (GUN_STATE(0, g) & 0x7F)); i++)
        if (!pt_step())
            return;
    a0 = shots_added();
    maybe_fire_all_guns(0);
    REQUIRE(shots_added() != a0, e_back, gunEnergy[0], 0, 0);
}

/* M2.13 */
static void t_muzzles(void)
{
    static const char __far e_side[] = "gun %d (mount %d across) fired from %d km to the ship's right: its mount's "
        "side, on the ship's own axes";
    static const char __far e_nose[] = "gun %d fired from %d km ahead of the ship's middle (its half box %d km): from "
        "the nose, inside its size";
    static const char __far e_way[] = "gun %d's shot flies %d/256 along the turned ship's nose";
    static const char __far e_none[] = "no gun of the turned ship put a shot in the world: nothing to look at (M2.1 "
        "first)";
    const RvVec3 *F, *R;
    RvVec3 d, v;
    Shot __far *s;
    int16_t g, n0, fired = 0;
    int32_t across, ahead, len, half;
    pt_alone(PT_PLAYER);
    ship_face(0, TURN / 4 + 37, 40);    /* (a ship turned off the world's axes) */
    F = &gOrientationFrontVector[0];
    R = &gOrientationRightVector[0];
    half = collide_half(0) / 256;
    for (g = 0; g < GUN_COUNT(0); g++) {
        if (!pt_is_gun(0, g))
            continue;
        GUN_STATE(0, g) = 0;
        n0 = shots_count();
        (void)fireGunFromShip(0, (uint16_t)g);
        if (shots_count() != n0 + 1)
            continue;                   /* (M2.1 says) */
        fired++;
        s = shot(n0);
        d.x = (s->x - gPositionVector[0].x) / 256;   /* (km) */
        d.y = (s->y - gPositionVector[0].y) / 256;
        d.z = (s->z - gPositionVector[0].z) / 256;
        across = vec_dot(&d, R) / 256;
        ahead = vec_dot(&d, F) / 256;
        if (GUN_MOUNT_X(0, g) != 0)
            REQUIRE((across < 0) == (GUN_MOUNT_X(0, g) < 0) && across != 0, e_side, g, GUN_MOUNT_X(0, g), across);
        REQUIRE(ahead > 0 && pt_len(d.x, d.y, d.z) <= 2 * half, e_nose, g, ahead, half);
        v.x = s->vx;
        v.y = s->vy;
        v.z = s->vz;
        len = pt_len(v.x, v.y, v.z);
        REQUIRE(len > 0 && vec_dot(&v, F) / len >= 243, e_way, g, len ? vec_dot(&v, F) / len : 0, 0);
    }
    REQUIRE(fired > 0, e_none, 0, 0, 0);
}

/* M2.14 */
static void t_turrets(void)
{
    static const char __far e_none[] = "the Vigilance has %d turrets ready: nothing to test";
    static const char __far e_fired[] = "maybe_fire_all_guns fired %d shots from a capital ship with %d turrets: each "
        "turret fires as a gun does";
    static const char __far e_far[] = "a turret's shot started %d km from the ship: within its size (%d km) expected";
    static const char __far e_cool[] = "turret %d still cooling after %d steps (its refire %d): turrets count down as "
        "guns do";
    const RvWeapon __far *w;
    uint16_t a0;
    int16_t n, g, i, n0, worst = 0;
    int32_t half;
    pt_alone(PT_CAPSHIP);
    n = pt_guns(0);
    REQUIRE(n > 0, e_none, n, 0, 0);
    half = collide_half(0) / 256;
    a0 = shots_added();
    n0 = shots_count();
    maybe_fire_all_guns(0);
    REQUIRE((int16_t)(shots_added() - a0) == n, e_fired, (uint16_t)(shots_added() - a0), n, 0);
    for (i = n0; i < shots_count(); i++)
        REQUIRE(pt_len((shot(i)->x - gPositionVector[0].x) / 256, (shot(i)->y - gPositionVector[0].y) / 256,
                       (shot(i)->z - gPositionVector[0].z) / 256) <= 2 * half, e_far,
                pt_len((shot(i)->x - gPositionVector[0].x) / 256, (shot(i)->y - gPositionVector[0].y) / 256,
                       (shot(i)->z - gPositionVector[0].z) / 256), 2 * half, 0);
    for (g = 0; g < GUN_COUNT(0); g++)
        if (pt_is_gun(0, g) && (w = weapon(GUN_TYPE(0, g))) != 0 && w->refire > worst)
            worst = w->refire;
    for (i = 0; i <= 2 * worst + 1; i++)
        if (!pt_step())
            return;
    for (g = 0; g < GUN_COUNT(0); g++)
        if (pt_is_gun(0, g) && (w = weapon(GUN_TYPE(0, g))) != 0)
            REQUIRE(!(GUN_STATE(0, g) & 0x7F), e_cool, g, 2 * worst + 2, w->refire);
}

/* M2.15 */
static void t_shots_full(void)
{
    static const char __far e_fill[] = "only %d shots went in before the list was full: SHOTS_MAX expected";
    static const char __far e_state[] = "with the shot list full a fire left the gun's cooldown %d and its energy %d "
        "(it was %d): set them anyway, as every machine replaying the call does (a full list is this machine's alone)";
    const RvWeapon __far *w;
    RvVec3 far_pos, still;
    int16_t n;
    uint16_t e0;
    pt_alone(PT_PLAYER);
    w = weapon(GUN_TYPE(0, 0));
    far_pos.x = far_pos.y = 0;
    far_pos.z = INT32_C(100000) * KM;
    still.x = still.y = still.z = 0;
    for (n = 0; n < SHOTS_MAX + 4 && shot_add(1, GUN_TYPE(0, 0), &far_pos, &still) >= 0; n++)
        ;
    REQUIRE(n == SHOTS_MAX, e_fill, n, 0, 0);
    GUN_STATE(0, 0) = 0;
    e0 = gunEnergy[0];
    (void)fireGunFromShip(0, 0);
    REQUIRE((GUN_STATE(0, 0) & 0x7F) && (!w || !w->energy || gunEnergy[0] < e0), e_state, GUN_STATE(0, 0) & 0x7F,
            gunEnergy[0], e0);
}

/* a held trigger on a ship of `type` (one of its guns destroyed first if `destroy`), from one shot short of its first
 * group's volley, so the energy keeps crossing the gate: every step fires 0 shots or the whole group, the group's
 * cooldowns stay equal, and at least three volleys come as the energy recovers.  0 when a check failed */
static int16_t pt_link_fire(int16_t type, int16_t destroy)
{
    static const char __far e_odd[] = "step %d of a held trigger fired %d shots from a group of %d linked guns: all of "
        "them or none, never part of the group";
    static const char __far e_step[] = "after step %d linked gun %d is cooling %d where the group's first is: they "
        "fire together, so they cool together";
    static const char __far e_few[] = "10 s of trigger from low energy fired only %d volleys of %d guns: the group "
        "fires again as the energy comes back";
    const RvWeapon __far *w;
    uint16_t a0, d;
    int16_t i, g, h, n, volleys = 0;
    pt_alone(type);
    if (destroy) {
        g = pt_first_gun(0);
        if (g >= 0)
            GUN_STATE(0, g) |= GUN_DESTROYED;
    }
    g = pt_first_gun(0);
    n = g >= 0 ? pt_group(0, g) : 0;
    if (n < 2) {
        pt_fail(e_lone, type, n, 0);
        return 0;
    }
    w = weapon(GUN_TYPE(0, g));
    gunEnergy[0] = (uint16_t)(w && w->energy ? n * w->energy - 1 : 100);
    g_sys.armed = 1;
    g_pilot.guns = 1;
    for (i = 0; i < 10 * SEC; i++) {
        a0 = shots_added();
        if (!pt_step())
            return 0;
        d = (uint16_t)(shots_added() - a0);
        if (d != 0 && d != (uint16_t)n) {
            pt_fail(e_odd, i + 1, d, n);
            return 0;
        }
        volleys += d != 0;
        for (h = g + 1; h < GUN_COUNT(0); h++)
            if (GUN_TYPE(0, h) == GUN_TYPE(0, g) && !(GUN_STATE(0, h) & GUN_DESTROYED)
                && (GUN_STATE(0, h) & 0x7F) != (GUN_STATE(0, g) & 0x7F)) {
                pt_fail(e_step, i + 1, h, GUN_STATE(0, h) & 0x7F);
                return 0;
            }
    }
    if (volleys < 3) {
        pt_fail(e_few, volleys, n, 0);
        return 0;
    }
    return 1;
}

/* M2.16 */
static void t_volleys(void)
{
    (void)pt_link_fire(PT_PLAYER, 0);
}

/* M2.17 */
static void t_link_fire(void)
{
    if (pt_link_fire(PT_LINKED, 0))     /* (four mass drivers) */
        (void)pt_link_fire(PT_LINKED, 1);   /* (three: one of the four destroyed) */
}

/* M2.18 */
#define PT_GLITCH 2                     /* steps a linked gun is left behind by (shorter than any tested gun's refire) */

/* a step of the held trigger, and the shots it fired */
static int16_t pt_fired(uint16_t *d)
{
    uint16_t a0 = shots_added();
    int16_t ok = pt_step();
    *d = (uint16_t)(shots_added() - a0);
    return ok;
}

/* the first (or last) gun of a linked group with some of its cooldown bits set when the rest have cooled, as a glitch
 * of a hit might one day leave it: the rest fire without it, it fires alone as it cools, and the group comes back
 * together by itself: at the first pull after a rest, or (hold) as the held trigger runs the energy out */
static int16_t pt_syncopated(int16_t type, int16_t last, int16_t hold)
{
    static const char __far e_rest[] = "gun %d, of a group of %d, still cooling: the trigger fired %d: the rest of the "
        "group, without it";
    static const char __far e_alone[] = "gun %d fired %d shots the step it cooled: itself alone";
    static const char __far e_rested[] = "the trigger fired %d after a rest: the whole group of %d, back together";
    static const char __far e_held[] = "30 s of held trigger and the group of %d never fired together again: as the "
        "energy runs out they all wait for it, and go together";
    static const char __far e_apart[] = "%d shots from the group of %d once it was back together: all of them or none";
    const RvWeapon __far *w;
    uint16_t d;
    int16_t i, g, h, n, late;
    g = pt_first_gun(0);                /* (the ship: pt_syncopated_rated's) */
    n = g >= 0 ? pt_group(0, g) : 0;
    if (n < 2) {
        pt_fail(e_lone, type, n, 0);
        return 0;
    }
    w = weapon(GUN_TYPE(0, g));
    late = g;
    if (last)
        for (h = g + 1; h < GUN_COUNT(0); h++)
            if (GUN_TYPE(0, h) == GUN_TYPE(0, g))
                late = h;
    g_sys.armed = 1;
    g_pilot.guns = 1;
    GUN_STATE(0, late) |= PT_GLITCH;
    if (!pt_fired(&d))
        return 0;
    if ((int16_t)d != n - 1) {
        pt_fail(e_rest, late, n, d);
        return 0;
    }
    if (!pt_fired(&d))
        return 0;
    if (d != 1) {
        pt_fail(e_alone, late, d, 0);
        return 0;
    }
    if (!hold) {
        g_pilot.guns = 0;
        if (!pt_steps(w->refire))
            return 0;
        g_pilot.guns = 1;
        if (!pt_fired(&d))
            return 0;
        if ((int16_t)d != n) {
            pt_fail(e_rested, d, n, 0);
            return 0;
        }
        return 1;
    }
    for (i = 0; i < 30 * SEC; i++) {
        if (!pt_fired(&d))
            return 0;
        if ((int16_t)d == n)
            break;
    }
    if (i == 30 * SEC) {
        pt_fail(e_held, n, 0, 0);
        return 0;
    }
    for (i = 0; i < 3 * SEC; i++) {
        if (!pt_fired(&d))
            return 0;
        if (d != 0 && (int16_t)d != n) {
            pt_fail(e_apart, d, n, 0);
            return 0;
        }
    }
    return 1;
}

/* pt_syncopated with, for the held trigger, the type's energy back a point every third step (as M3.13 sets a rate:
 * the slot's type record, put back after): the held trigger must run the energy out for the group to come back
 * together, where the blank rate, a point a step, sustains two lasers for ever, as the old game's did */
static int16_t pt_syncopated_rated(int16_t type, int16_t last, int16_t hold)
{
    RvShipType __far *t;
    uint8_t keep;
    int16_t ok;
    pt_alone(type);
    t = (RvShipType __far *)slot_type(0);
    keep = t->gun_energy_steps;
    if (hold)
        t->gun_energy_steps = 3;
    ok = pt_syncopated(type, last, hold);
    t->gun_energy_steps = keep;
    return ok;
}

static void t_syncopated(void)
{
    int16_t last, hold;
    for (hold = 0; hold < 2; hold++)
        for (last = 0; last < 2; last++)
            if (!pt_syncopated_rated(PT_PLAYER, last, hold) || !pt_syncopated_rated(PT_LINKED, last, hold))
                return;
}

/* ======================================================================================================== */
/* M3: damage (do_damage, step_shots, step_timers) */

/* the player still at the origin, and a frozen Nicander 3,000 km ahead facing away (its rear to the player) */
static void pt_target(void)
{
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);
}

static const char __far e_did[] = "do_damage returned 0 (nothing done) for a hit of %d on a ship";
static const char __far e_same[] = "%d of its shields and armour changed (front shield %d to %d): only the side hit "
    "takes it";

/* the sides that changed: a bit each, 1 2 the shields (front, rear), 4 .. 32 the armour (front, rear, left, right),
 * 64 the hull */
static int16_t pt_changed(const PtHurt *a, const PtHurt *b)
{
    int16_t k, m = 0;
    for (k = 0; k < 2; k++)
        if (a->sh[k] != b->sh[k])
            m |= 1 << k;
    for (k = 0; k < 4; k++)
        if (a->ar[k] != b->ar[k])
            m |= 4 << k;
    if (a->hull != b->hull)
        m |= 64;
    return m;
}

/* a hit at (f, r) on the target with its shields as given, and what changed */
static int16_t pt_side_hit(int16_t f, int16_t r, uint16_t qty, int16_t shields, PtHurt *a, PtHurt *b, uint16_t *ret)
{
    pt_target();
    if (!shields)
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
    pt_hurt(1, a);
    *ret = pt_hit(0xFFFF, 1, qty, f, r, 0);
    pt_hurt(1, b);
    return pt_changed(a, b);
}

/* M3.1 */
static void t_shield_front(void)
{
    static const char __far e_more[] = "the front shield lost %d for a hit of %d";
    PtHurt a, b;
    uint16_t r;
    int16_t m = pt_side_hit(2, 0, 3, 1, &a, &b, &r);
    REQUIRE(r != 0, e_did, 3, 0, 0);
    REQUIRE(m == 1 && b.sh[0] < a.sh[0], e_same, m, a.sh[0], b.sh[0]);
    EXPECT(a.sh[0] - b.sh[0] <= 3, e_more, a.sh[0] - b.sh[0], 3, 0);
}

/* M3.2 */
static void t_shield_rear(void)
{
    static const char __far e_rear[] = "a hit from behind changed %d (front shield %d to %d): the rear shield (bit 2) "
        "takes it";
    PtHurt a, b;
    uint16_t r;
    int16_t m = pt_side_hit(-2, 0, 3, 1, &a, &b, &r);
    REQUIRE(r != 0, e_did, 3, 0, 0);
    REQUIRE(m == 2 && b.sh[1] < a.sh[1], e_rear, m, a.sh[0], b.sh[0]);
}

/* M3.3 */
static void t_armour(void)
{
    static const char __far e_arm[] = "with the shields down a front hit changed %d (front armour %d to %d): the front "
        "armour (bit 4) takes it";
    PtHurt a, b;
    uint16_t r;
    int16_t m = pt_side_hit(2, 0, 3, 0, &a, &b, &r);
    REQUIRE(r != 0, e_did, 3, 0, 0);
    REQUIRE(m == 4 && b.ar[0] < a.ar[0], e_arm, m, a.ar[0], b.ar[0]);
}

/* M3.4 */
static void t_armour_sides(void)
{
    static const char __far e_left[] = "a hit on its left changed %d (left armour %d to %d): the left armour (bit 16) "
        "takes it";
    static const char __far e_right[] = "a hit on its right changed %d (right armour %d to %d): the right armour (bit "
        "32) takes it";
    PtHurt a, b;
    uint16_t r;
    int16_t m = pt_side_hit(0, -2, 2, 0, &a, &b, &r);
    REQUIRE(r != 0, e_did, 2, 0, 0);
    REQUIRE(m == 16, e_left, m, a.ar[2], b.ar[2]);
    m = pt_side_hit(0, 2, 2, 0, &a, &b, &r);
    REQUIRE(m == 32, e_right, m, a.ar[3], b.ar[3]);
}

/* M3.5 */
static void t_hull(void)
{
    static const char __far e_hull[] = "with its front shield and armour gone %d front hits changed %d (hull damage %d to the "
        "same): the hull (damagePoints, bit 64) takes them (an unnamed fighter's through the old game's roulette: one "
        "point in five or so, fighter_roulette)";
    PtHurt a, b;
    uint16_t r;
    int16_t i;
    pt_target();
    SHIELD(1, 0) = SHIELD(1, 1) = 0;
    ARMOR(1, 0) = 0;
    pt_hurt(1, &a);
    for (i = 1; i <= 40; i++) {
        r = pt_hit(0xFFFF, 1, 3, 2, 0, 0);
        REQUIRE(r != 0, e_did, 3, 0, 0);
        pt_hurt(1, &b);
        if (b.hull > a.hull)
            break;
    }
    REQUIRE(pt_changed(&a, &b) == 64 && b.hull > a.hull, e_hull, i, pt_changed(&a, &b), a.hull);
}

/* M3.6 */
static void t_big_hit(void)
{
    static const char __far e_dead[] = "a hit past the whole of a ship of %d left it alive (shipAiState %d, hull "
        "damage %d): a ship whose hull is gone dies, shipAiState 9";
    static const char __far e_wrap[] = "a shield or armour wrapped past 0 (%d, %d): they stop at 0";
    pt_target();
    (void)pt_hit(0xFFFF, 1, 30000, 2, 0, 0);   /* (thirty thousand: past the whole ship many times over, through the
                                                  roulette of an unnamed fighter's hull too: M8.49) */
    if (!pt_step())
        return;
    REQUIRE(shipAiState[1] == 9, e_dead, 30000, shipAiState[1], damagePoints[1]);
    REQUIRE(SHIELD(1, 0) <= SHIELD_MAX(1, 0) && ARMOR(1, 0) <= slot_type(1)->armor_f, e_wrap, SHIELD(1, 0), ARMOR(1, 0),
            0);
}

/* M3.7 */
static void t_no_wrap(void)
{
    static const char __far e_wrap[] = "after hit %d a shield or armour wrapped (front shield %d, front armour %d)";
    static const char __far e_back[] = "a hit on a dying ship brought it back (shipAiState %d)";
    static const char __far e_dead[] = "100 hits of %d all round a ship of %d all told: still alive";
    int16_t i, was_dead = 0, qty;
    int32_t all;
    pt_target();
    all = pt_health(1) + slot_type(1)->hull;
    qty = (int16_t)(all / 80 + 1);      /* (a hundred of them are more than the ship, whatever its units) */
    for (i = 0; i < 100; i++) {
        (void)pt_hit(0xFFFF, 1, (uint16_t)qty, (int16_t)(i % 3 - 1) * 2, (int16_t)(i % 5 - 2), 0);
        REQUIRE(SHIELD(1, 0) <= SHIELD_MAX(1, 0) && SHIELD(1, 1) <= SHIELD_MAX(1, 1)
                && ARMOR(1, 0) <= slot_type(1)->armor_f && ARMOR(1, 1) <= slot_type(1)->armor_r
                && ARMOR(1, 2) <= slot_type(1)->armor_l && ARMOR(1, 3) <= slot_type(1)->armor_rt, e_wrap, i,
                SHIELD(1, 0), ARMOR(1, 0));
        if (shipAiState[1] == 9)
            was_dead = 1;
        REQUIRE(!was_dead || shipAiState[1] == 9, e_back, shipAiState[1], 0, 0);
    }
    if (!pt_step())
        return;
    REQUIRE(shipAiState[1] == 9 || !slot_is_ship(1), e_dead, qty, all, 0);
}

/* M3.8 */
static void t_player_safe(void)
{
    static const char __far e_none[] = "a hit on the player did nothing with playerCanBeDamaged 1: do_damage does "
        "nothing yet";
    static const char __far e_hurt[] = "the player was hurt (%d changed) with playerCanBeDamaged 0";
    static const char __far e_ret[] = "do_damage returned %d for a hit it ignored: 0 expected";
    PtHurt a, b;
    uint16_t r;
    pt_target();
    pt_hurt(0, &a);
    (void)pt_hit(0xFFFF, 0, 3, 2, 0, 0);
    pt_hurt(0, &b);
    REQUIRE(pt_changed(&a, &b), e_none, 0, 0, 0);
    pt_target();
    playerCanBeDamaged = 0;
    pt_hurt(0, &a);
    r = pt_hit(1, 0, 3, 2, 0, 0);
    pt_hurt(0, &b);
    REQUIRE(!pt_changed(&a, &b), e_hurt, pt_changed(&a, &b), 0, 0);
    EXPECT(r == 0, e_ret, r, 0, 0);
}

/* M3.9 */
static void t_invulnerable(void)
{
    static const char __far e_none[] = "a hit did nothing: do_damage does nothing yet";
    static const char __far e_hurt[] = "a ship with rvFlags 1 (invulnerable) was hurt (%d changed)";
    PtHurt a, b;
    pt_target();
    pt_hurt(1, &a);
    (void)pt_hit(0xFFFF, 1, 3, 2, 0, 0);
    pt_hurt(1, &b);
    REQUIRE(pt_changed(&a, &b), e_none, 0, 0, 0);
    pt_target();
    rvFlags[1] |= 1;
    pt_hurt(1, &a);
    (void)pt_hit(0xFFFF, 1, 3, 2, 0, 0);
    pt_hurt(1, &b);
    REQUIRE(!pt_changed(&a, &b), e_hurt, pt_changed(&a, &b), 0, 0);
}

/* what a hit may change on a slot, to compare two tries of it */
typedef struct {
    uint8_t shield[4], armour[8], hull[2], core, guns, engine, table[51], state[2], energy[2];
    uint32_t seed, cockpit;
} PtDamage;

static void pt_damage_get(int16_t s, PtDamage *d)
{
    _fmemcpy(d->shield, (uint8_t *)curShield + s * 4, 4);
    _fmemcpy(d->armour, armorState + s * 8, 8);
    _fmemcpy(d->hull, (uint8_t *)damagePoints + s * 2, 2);
    d->core = coreHp[s];
    d->guns = gunDamage[s];
    d->engine = engineFlag[s];
    _fmemcpy(d->table, gunTable + s * gunTable_SIZE, gunTable_SIZE);
    _fmemcpy(d->state, (uint8_t *)shipAiState + s * 2, 2);
    _fmemcpy(d->energy, (uint8_t *)gunEnergy + s * 2, 2);
    d->seed = randomSeed;
    d->cockpit = rvCockpitDamage;
}

static void pt_damage_set(int16_t s, const PtDamage *d)
{
    _fmemcpy((uint8_t *)curShield + s * 4, d->shield, 4);
    _fmemcpy(armorState + s * 8, d->armour, 8);
    _fmemcpy((uint8_t *)damagePoints + s * 2, d->hull, 2);
    coreHp[s] = d->core;
    gunDamage[s] = d->guns;
    engineFlag[s] = d->engine;
    _fmemcpy(gunTable + s * gunTable_SIZE, d->table, gunTable_SIZE);
    _fmemcpy((uint8_t *)shipAiState + s * 2, d->state, 2);
    _fmemcpy((uint8_t *)gunEnergy + s * 2, d->energy, 2);
    randomSeed = d->seed;
    rvCockpitDamage = d->cockpit;
}

/* M3.10 */
static void t_any_source(void)
{
    static const char __far e_none[] = "a hit of %d did nothing: do_damage does nothing yet";
    static const char __far e_src[] = "a hit of %d came out otherwise with src %d than with src 0xFFFF: what a hit "
        "does must not depend on src (the layer replays hits with another)";
    static const char __far e_ret[] = "do_damage returned %d with src %d and %d with 0xFFFF";
    static uint16_t __far qty[2] = { 3, 30 };
    PtDamage before, base, other;
    uint16_t src[2], r0, r;
    int16_t q, k, i;
    pt_target();
    pt_speed(0, 0);
    (void)fireGunFromShip(0, 0);
    i = shots_count() - 1;
    src[0] = 0;
    src[1] = i >= 0 ? (uint16_t)shot_source(i) : 0;
    for (q = 0; q < 2; q++) {
        pt_damage_get(1, &before);
        r0 = pt_hit(0xFFFF, 1, qty[q], 2, 1, 0);
        pt_damage_get(1, &base);
        REQUIRE(_fmemcmp(&before, &base, sizeof base), e_none, qty[q], 0, 0);
        for (k = 0; k < 2; k++) {
            pt_damage_set(1, &before);
            r = pt_hit(src[k], 1, qty[q], 2, 1, 0);
            pt_damage_get(1, &other);
            REQUIRE(!_fmemcmp(&base, &other, sizeof base), e_src, qty[q], src[k], 0);
            REQUIRE((r != 0) == (r0 != 0), e_ret, r, src[k], r0);
        }
        pt_damage_set(1, &before);
    }
}

/* M3.11 */
static void t_shots_hit(void)
{
    static const char __far e_none[] = "3 s of fire at a ship %d km dead ahead, side on, took nothing off it (%d shots "
        "fired): step_shots finds hits with shot_hit and calls do_damage";
#ifdef RV_HOST
    static const char __far e_src[] = "the hits came with no bolt of the player's as do_damage's src: pass "
        "shot_source(i) (a bolt whose parent is the shooter: how the layer knows who fired)";
#endif
    int32_t h0;
    uint16_t a0;
    pt_alone(PT_PLAYER);
    /* (side on, at the guns' height: fighters' hulls are flat, some 100 km thick to 900 across) */
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, (int32_t)GUN_MOUNT_Y(0, 0) * collide_half(0) / 32 / 256, 1500,
            TURN / 4);
    h0 = pt_health(1);
    g_sys.armed = 1;
    g_pilot.guns = 1;
    a0 = shots_added();
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE(pt_health(1) < h0 || shipAiState[1] == 9 || !slot_is_ship(1), e_none, 1500,
            (uint16_t)(shots_added() - a0), 0);
#ifdef RV_HOST
    EXPECT(host_bolt_hits[0] > 0, e_src, 0, 0, 0);
#endif
}

/* M3.12 */
static void t_recharge(void)
{
    static const char __far e_none[] = "the shields did not come back in 20 s (front %d, rear %d)";
    static const char __far e_over[] = "a shield came back past its most (%d of %d)";
    static const char __far e_armour[] = "the armour came back (%d to %d): only the shields recharge in flight";
    int16_t i;
    pt_alone(PT_PLAYER);
    SHIELD(0, 0) = 1;
    SHIELD(0, 1) = 0;
    ARMOR(0, 0) = 2;
    for (i = 0; i < 20 * SEC; i++) {
        if (!pt_step())
            return;
        REQUIRE(SHIELD(0, 0) <= SHIELD_MAX(0, 0) && SHIELD(0, 1) <= SHIELD_MAX(0, 1), e_over, SHIELD(0, 0),
                SHIELD_MAX(0, 0), 0);
    }
    REQUIRE(SHIELD(0, 0) > 1 && SHIELD(0, 1) > 0, e_none, SHIELD(0, 0), SHIELD(0, 1), 0);
    REQUIRE(ARMOR(0, 0) == 2, e_armour, 2, ARMOR(0, 0), 0);
}

/* M2.20: a shot carries the ship's speed along its nose and nothing of its slide (the old game's bolts, read in its
 * code: the gun's speed plus the dot of the ship's velocity and the gun's line): the player sliding sideways at 50
 * km a step with the nose ahead fires a shot that flies along the nose at the laser's speed; moving ahead at 40 km a
 * step, one that flies at the laser's speed plus 40 */
static void t_forward_only(void)
{
    static const char __far e_side[] = "the shot of a ship sliding 50 km a step sideways drifts %d km a step sideways: "
        "a shot carries the ship's speed along the nose only (gun_fire)";
    static const char __far e_ahead[] = "its speed along the nose is %d km a step: the laser's %d, the ship's along "
        "the nose being 0";
    static const char __far e_plus[] = "a ship moving 40 km a step ahead fires a shot at %d km a step: the laser's %d "
        "plus the 40";
    const RvWeapon __far *w = weapon(GUN_TYPE(0, 0));
    int16_t step;
    pt_alone(PT_PLAYER);
    if (!w)
        return;
    step = (int16_t)TO_KM(KPS_STEP(w->speed_kps));
    gVelocityVector[0] = vec3(50 * KM, 0, 0);
    (void)fireGunFromShip(0, 0);
    REQUIRE(shots_count() == 1 && labs(shot(0)->vx) < KM, e_side, (int16_t)TO_KM(shot(0)->vx), 0, 0);
    REQUIRE(labs(TO_KM(shot(0)->vz) - step) <= 1, e_ahead, (int16_t)TO_KM(shot(0)->vz), step, 0);
    shot_remove(0);
    GUN_STATE(0, 0) = 0;
    gVelocityVector[0] = vec3(0, 0, 40 * KM);
    (void)fireGunFromShip(0, 0);
    REQUIRE(shots_count() == 1 && labs(TO_KM(shot(0)->vz) - (step + 40)) <= 1, e_plus, (int16_t)TO_KM(shot(0)->vz),
            step, 0);
    gVelocityVector[0] = vec3(0, 0, 0);
}

/* M2.19 */
static void t_sustain(void)
{
    static const char __far e_few[] = "%d shots in 10 s of trigger from %d guns: the old game's Hornet fired a volley every "
        "0.44 s at its 15 frames a second (22-23 a gun) without running down";
    static const char __far e_many[] = "%d shots in 10 s of trigger from %d guns: more than a volley every 0.44 s";
    static const char __far e_drained[] = "gun energy down to %d after 10 s of trigger: the lasers are sustained";
    uint16_t a0;
    int16_t guns;
    pt_alone(PT_PLAYER);
    guns = pt_guns(0);
    g_sys.armed = 1;
    g_pilot.guns = 1;
    a0 = shots_added();
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE((uint16_t)(shots_added() - a0) >= guns * 20, e_few, (uint16_t)(shots_added() - a0), guns, 0);
    REQUIRE((uint16_t)(shots_added() - a0) <= guns * 25, e_many, (uint16_t)(shots_added() - a0), guns, 0);
    REQUIRE(gunEnergy[0] >= 50, e_drained, gunEnergy[0], 0, 0);
    g_pilot.guns = 0;
}

/* M3.14: the three hit volumes (shots.c shot_hit, the mission's rule g_rule_hit): a Nicander (radius 160 km; its
 * silhouettes 89 km across and 28 up; its volume box 89 across, 156 up, 154 along) dead ahead with its nose the
 * player's way, and a laser bolt (radius 10) stepping 110 km past its middle along the nose: beside it by 120 km the
 * sphere alone takes it; above it by 100 km the sphere and the box; through the middle all three */
static int16_t pt_bolt_hits(int32_t dx_km, int32_t dy_km, uint8_t mode)
{
    RvVec3 pos = vec3(gPositionVector[1].x + dx_km * KM, gPositionVector[1].y + dy_km * KM, gPositionVector[1].z + 55 * KM);
    RvVec3 vel = vec3(0, 0, 110 * KM), where;
    int16_t i, slot = -1, hit;
    uint8_t keep = g_rule_hit;
    if ((i = shot_add(0, GUN_TYPE(0, 0), &pos, &vel)) < 0)
        return -1;
    g_rule_hit = mode;
    hit = shot_hit(i, &slot, &where);
    g_rule_hit = keep;
    shot_remove(i);
    return hit && slot == 1;
}

static void t_hit_volumes(void)
{
    static const char __far e_side[] = "a bolt 120 km beside a Nicander's middle: sphere %d, box %d, silhouette %d "
        "(wanted 1, 0, 0: the sphere's radius 160 plus the bolt's 10; the box and the hull 89 across)";
    static const char __far e_above[] = "a bolt 100 km above it: sphere %d, box %d, silhouette %d (wanted 1, 1, 0: the "
        "volume box is 156 km up, the silhouettes 28)";
    static const char __far e_mid[] = "a bolt through its middle: sphere %d, box %d, silhouette %d (wanted 1, 1, 1)";
    int16_t a, b, c;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);
    a = pt_bolt_hits(120, 0, RV_SHIPTYPE_FLAG_HIT_SPHERE);
    b = pt_bolt_hits(120, 0, RV_SHIPTYPE_FLAG_HIT_BOX);
    c = pt_bolt_hits(120, 0, RV_SHIPTYPE_FLAG_HIT_SILHOUETTE);
    REQUIRE(a == 1 && b == 0 && c == 0, e_side, a, b, c);
    a = pt_bolt_hits(0, 100, RV_SHIPTYPE_FLAG_HIT_SPHERE);
    b = pt_bolt_hits(0, 100, RV_SHIPTYPE_FLAG_HIT_BOX);
    c = pt_bolt_hits(0, 100, RV_SHIPTYPE_FLAG_HIT_SILHOUETTE);
    REQUIRE(a == 1 && b == 1 && c == 0, e_above, a, b, c);
    a = pt_bolt_hits(0, 0, RV_SHIPTYPE_FLAG_HIT_SPHERE);
    b = pt_bolt_hits(0, 0, RV_SHIPTYPE_FLAG_HIT_BOX);
    c = pt_bolt_hits(0, 0, RV_SHIPTYPE_FLAG_HIT_SILHOUETTE);
    REQUIRE(a == 1 && b == 1 && c == 1, e_mid, a, b, c);
}

/* M3.15: a hit does the weapon's damage_near at the muzzle and its damage at the end of its reach, a line between
 * by the life left (the old game's bolt, read in its code: a laser 10 at the muzzle, 25 at the end): a laser bolt
 * into a Nicander's middle with all its life does 10, with one step left 25 */
static void t_damage_grows(void)
{
    static const char __far e_hit[] = "a laser bolt through a Nicander's middle did not hit (shot_hit)";
    static const char __far e_near[] = "a laser bolt fresh from the muzzle hit for %d: the old game's 10 (weapons.csv "
        "damage_near)";
    static const char __far e_far[] = "a laser bolt at the end of its reach hit for %d: the old game's 25 (weapons.csv "
        "damage)";
    RvVec3 pos, vel = vec3(0, 0, 110 * KM), where;
    int16_t i, slot = -1, hit, d_near, d_far;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);
    pos = vec3(gPositionVector[1].x, gPositionVector[1].y, gPositionVector[1].z + 55 * KM);
    if ((i = shot_add(0, GUN_TYPE(0, 0), &pos, &vel)) < 0)
        return;
    hit = shot_hit(i, &slot, &where);
    d_near = (int16_t)shot(i)->damage;
    shot(i)->life = 1;
    hit = hit && shot_hit(i, &slot, &where);
    d_far = (int16_t)shot(i)->damage;
    shot_remove(i);
    REQUIRE(hit && slot == 1, e_hit, 0, 0, 0);
    REQUIRE(d_near == 10, e_near, d_near, 0, 0);
    REQUIRE(d_far == 25, e_far, d_far, 0, 0);
}

/* M3.13 */
static void t_type_rates(void)
{
    static const char __far e_energy[] = "gun energy back %d points in 60 steps, a point every 6 is 10";
    static const char __far e_shield[] = "a shield back to %d of %d in 16 steps, shield_full_shift 4 fills it in those";
    static const char __far e_steps[] = "a shield back %d points in 16 steps with shield_steps 4 (the old game's stat: "
        "a point a side every 4 of its frames, 8 of our steps): 2 expected";
    RvShipType __far *t;
    uint8_t keep_steps, keep_shift, keep_sh;
    uint16_t shield = 0, by_steps = 0;
    int16_t i, ok = 1;
    pt_alone(PT_PLAYER);
    t = (RvShipType __far *)slot_type(0);       /* (the table outlives the test: put back before any REQUIRE) */
    keep_steps = t->gun_energy_steps;
    keep_shift = t->shield_full_shift;
    keep_sh = t->shield_steps;
    t->gun_energy_steps = 6;
    t->shield_full_shift = 4;
    t->shield_steps = 0;
    gunEnergy[0] = 0;
    SHIELD(0, 0) = 0;
    for (i = 0; i < 60 && ok; i++) {
        ok = pt_step();
        if (i == 15) {                  /* (the shift's 16 steps done: then the old game's stat over the next 16) */
            shield = SHIELD(0, 0);
            t->shield_steps = 4;
            SHIELD(0, 0) = 0;
        }
        if (i == 31)
            by_steps = SHIELD(0, 0);
    }
    t->gun_energy_steps = keep_steps;
    t->shield_full_shift = keep_shift;
    t->shield_steps = keep_sh;
    if (!ok)
        return;
    REQUIRE(shield == SHIELD_MAX(0, 0), e_shield, shield, SHIELD_MAX(0, 0), 0);
    REQUIRE(by_steps == 16 / (4 * WC_FRAME_STEPS), e_steps, by_steps, 0, 0);
    REQUIRE(gunEnergy[0] == 10, e_energy, gunEnergy[0], 0, 0);
}

/* ======================================================================================================== */
/* M4: deaths (step_deaths, despawn, delayedDespawn) */

/* M4.1 */
static void t_kill(void)
{
    static const char __far e_dying[] = "shipAiState is %d a step after a killing hit: 9 (dying)";
    static const char __far e_kept[] = "the slot was emptied at once: keep it while it explodes (the layer learns of "
        "a death from shipAiState 9 in the slot)";
    static const char __far e_short[] = "its slot was emptied %d steps after the hit: an explosion takes a while";
    static const char __far e_long[] = "still in its slot 10 s after it died";
    static const char __far e_thunk[] = "it was never ended through despawn or delayedDespawn (%d calls): the layer "
        "sees a death end through them";
    int16_t n;
    int32_t ends;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 6000, 0);
    (void)pt_hit(0xFFFF, 1, (uint16_t)(pt_health(1) + slot_type(1)->hull + 10), -2, 0, 0);   /* (past the whole ship) */
    if (!pt_step())
        return;
    REQUIRE(slot_is_ship(1), e_kept, 0, 0, 0);
    REQUIRE(shipAiState[1] == 9, e_dying, shipAiState[1], 0, 0);
    for (n = 1; n < 10 * SEC && slot_is_ship(1); n++)
        if (!pt_step())
            return;
    REQUIRE(!slot_is_ship(1), e_long, 0, 0, 0);
    EXPECT(n >= 3, e_short, n, 0, 0);
    ends = pt_calls(RV_FN_despawn, 0, 1);
    if (ends >= 0) {
        ends += pt_calls(RV_FN_delayedDespawn, 1, 1);
        EXPECT(ends >= 1, e_thunk, ends, 0, 0);
    }
}

/* M4.2 */
static void t_dying_quiet(void)
{
    static const char __far e_fired[] = "a dying ship fired (%d shots, %d missiles, %d calls of fireGunFromShip)";
    static const char __far e_quiet[] = "the enemy never fired at the player in 30 s: nothing to test (M5.4 first)";
    int16_t i, k, missiles = 0, more;
    uint16_t a0;
    int32_t c0, c;
    pt_alone(PT_PLAYER);
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 2000, TURN / 2);
    a0 = shots_added();
    for (i = 0; i < 30 * SEC && shots_added() == a0; i++)
        if (!pt_step())
            return;
    REQUIRE(shots_added() != a0, e_quiet, 0, 0, 0);
    (void)pt_hit(0xFFFF, 1, (uint16_t)(pt_health(1) + slot_type(1)->hull + 10), -2, 0, 0);   /* (past the whole ship) */
    if (!pt_step())                     /* (its last step: it may have fired before step_deaths saw its hull gone) */
        return;
    a0 = shots_added();                 /* (the player holds its fire: every shot is the enemy's) */
    c0 = pt_calls(RV_FN_fireGunFromShip, 0, 1);
    for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
        if (maybeEntityType[k] == ET_MISSILE && parentShipForShip[k] == 1)
            missiles++;
    for (i = 0; i < 3 * SEC && slot_is_ship(1); i++) {
        if (!pt_step())
            return;
        for (k = 1, more = 0; k <= RV_MAX_SHIP_SLOT; k++)
            if (maybeEntityType[k] == ET_MISSILE && parentShipForShip[k] == 1)
                more++;
        REQUIRE(shots_added() == a0 && more <= missiles, e_fired, (uint16_t)(shots_added() - a0), more - missiles, 0);
        missiles = more;
    }
    c = pt_calls(RV_FN_fireGunFromShip, 0, 1);
    EXPECT(c == c0, e_fired, 0, 0, c - c0);
}

/* M4.3 */
static void t_player_dies(void)
{
    static const char __far e_status[] = "10 s after the player's ship was destroyed missionStatus is %d: 4 (dead)";
    static const char __far e_dying[] = "the player's ship never went to shipAiState 9";
    static const char __far e_score[] = "victoryPoints is %d after the player died: a dead pilot's mission is lost (0)";
    int16_t i, dying = 0;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, 0);
    (void)pt_hit(1, 0, 500, 2, 0, 0);
    for (i = 0; i < 10 * SEC && !missionStatus; i++) {
        if (!pt_step())
            return;
        if (shipAiState[0] == 9)
            dying = 1;
    }
    REQUIRE(missionStatus == 4, e_status, missionStatus, 0, 0);
    EXPECT(dying, e_dying, 0, 0, 0);
    logic_end();
    REQUIRE(victoryPoints == 0, e_score, victoryPoints, 0, 0);
}

/* (from here on the tests' code is a second segment of its own: in RVP.EXE's medium model a file's code is one
 * segment, at most 64 KB, and the tests outgrew it on 2026-11-04 by 8 bytes: E2021) */
#ifdef __WATCOMC__
#pragma code_seg("playtest2_TEXT", "CODE")
#endif

/* ======================================================================================================== */
/* M5: the AI (aiFighterThink, aiShipThink, aiSetSpeed, step_minds) */

/* M5.1 */
static void t_ai_thunks(void)
{
    static const char __far e_think[] = "aiFighterThink was called %d times in 60 steps for an AI fighter (slot %d): "
        "every few steps at least, through the thunk (the layer skips it where the ship is a puppet)";
    static const char __far e_not[] = "aiFighterThink was called %d times for slot %d, which no AI flies (the player, "
        "a puppet, a frozen ship, a capital ship)";
    static const char __far e_ship[] = "aiShipThink was called %d times in 60 steps for an AI capital ship (slot %d)";
    int16_t s;
    int32_t n;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, -3000, 0, 9000, TURN / 2);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_AI, 3000, 0, 9000, TURN / 2);
    pt_ship(3, PT_ENEMY, SIDE_AERA, CTL_PUPPET, 0, 3000, 12000, TURN / 2);
    pt_ship(4, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, -3000, 12000, TURN / 2);
    pt_ship(5, PT_CAPSHIP, SIDE_TERRAN, CTL_AI, 0, 0, -20000, 0);
    if (!pt_steps(60))
        return;
    for (s = 1; s <= 2; s++) {
        n = pt_calls(RV_FN_aiFighterThink, 0, s);
        REQUIRE(n >= 15, e_think, n, s, 0);
    }
    for (s = 0; s <= 5; s++)
        if (s == 0 || s >= 3) {
            n = pt_calls(RV_FN_aiFighterThink, 0, s);
            REQUIRE(n == 0, e_not, n, s, 0);
        }
    n = pt_calls(RV_FN_aiShipThink, 0, 5);
    REQUIRE(n >= 15, e_ship, n, 5, 0);
}

/* the player still at the origin, its guns safe, and an AI Nicander 6,000 km ahead facing away */
static void pt_duel(void)
{
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 6000, 0);
}

/* M5.2 */
static void t_ai_turns(void)
{
    static const char __far e_turn[] = "in 10 s the enemy never pointed its nose at the player (best %d/256)";
    int16_t i;
    int32_t best = -256, a;
    pt_duel();
    for (i = 0; i < 10 * SEC && best < 230; i++) {
        if (!pt_step())
            return;
        if (slot_is_ship(1) && (a = pt_aim(1, 0)) > best)
            best = a;
    }
    REQUIRE(best >= 230, e_turn, best, 0, 0);
}

/* M5.3 */
static void t_ai_closes(void)
{
    static const char __far e_far[] = "in 30 s the enemy came no nearer than %d km (it began 6,000 km away)";
    int16_t i;
    int32_t least = 999999, d;
    pt_duel();
    for (i = 0; i < 30 * SEC && least >= 2500; i++) {
        if (!pt_step())
            return;
        if (slot_is_ship(1) && (d = pt_km(0, 1)) < least)
            least = d;
    }
    REQUIRE(least < 2500, e_far, least, 0, 0);
}

/* M5.4 */
static void t_ai_fires(void)
{
    static const char __far e_quiet[] = "in 45 s the enemy fired nothing at a still player %d km away";
    static const char __far e_miss[] = "in 45 s the enemy fired %d shots and none hurt the player";
    int16_t i;
    int32_t h0;
    uint16_t a0;
    pt_duel();
    h0 = pt_health(0);
    a0 = shots_added();                 /* (the enemy is the only one firing) */
    for (i = 0; i < 45 * SEC && missionStatus == 0 && (shots_added() == a0 || pt_health(0) == h0); i++)
        if (!pt_step())
            return;
    REQUIRE(shots_added() != a0, e_quiet, slot_is_ship(1) ? pt_km(0, 1) : 0, 0, 0);
    REQUIRE(pt_health(0) < h0, e_miss, (uint16_t)(shots_added() - a0), 0, 0);
}

/* M5.5: an enemy beside the player, in its guns' range but the player 90 degrees off its nose: it turns before it
 * fires (its first 8 volleys, through the harness's count of fireGunFromShip) */
static void t_ai_aims(void)
{
#ifdef RV_HOST
    static const char __far e_quiet[] = "in 20 s the enemy fired %d volleys at a still player beside it";
    static const char __far e_aim[] = "of %d volleys only %d were fired with the player within 30 degrees of the "
        "enemy's nose: an AI fires when its guns bear";
#endif
    int16_t i;
    int32_t calls = 0, aimed = 0, c, was = 0;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 1200, 0, 0, 0);
    for (i = 0; i < 20 * SEC && calls < 8; i++) {
        if (!pt_step())
            return;
        c = pt_calls(RV_FN_fireGunFromShip, 0, 1);
        if (c > was) {
            calls++;
            if (slot_is_ship(1) && pt_aim(1, 0) >= AIM_COS)
                aimed++;
            was = c;
        }
    }
#ifdef RV_HOST
    REQUIRE(calls > 0, e_quiet, calls, 0, 0);
    REQUIRE(aimed * 100 >= calls * AIM_SHARE, e_aim, calls, aimed, 0);
#else
    (void)calls;
    (void)aimed;
#endif
}

/* M5.6 */
static void t_ai_limits(void)
{
    static const char __far e_turn[] = "the enemy's nose turned %d (1024 a turn) in a step: its type allows %d";
    static const char __far e_fast[] = "the enemy flew %d kps (afterburner %d): its type's top speed is %d, and over it "
        "only on the burner or coming down from it";
    static const char __far e_still[] = "the enemy turned %d (1024 a turn) and flew %d km in 30 s: nothing to test";
    const RvShipType __far *t;
    RvVec3 f0, x;
    int16_t i, most;
    int32_t c, turned = 0, was = 0;
    pt_duel();
    t = slot_type(1);
    most = (int16_t)(pt_turn_of(1, t->yaw_dps > t->pitch_dps ? t->yaw_dps : t->pitch_dps, 9) * 3 / 2 + 3);
    for (i = 0; i < 30 * SEC && missionStatus == 0 && slot_is_ship(1); i++) {
        f0 = gOrientationFrontVector[1];
        if (!pt_step())
            return;
        if (!slot_is_ship(1) || shipAiState[1] == 9)
            break;
        vec_cross(&x, &gOrientationFrontVector[1], &f0);
        c = lf_atan2(pt_len(x.x, x.y, x.z), vec_dot(&gOrientationFrontVector[1], &f0) / 256);
        REQUIRE(c <= most, e_turn, c, most, 0);
        turned += c;
        /* (off the burner, over its top speed only coming down: the old fighters slowed from a dash at their rate) */
        REQUIRE((int32_t)speedLo[1] <= (int32_t)(rvAfterburner[1] ? t->ab_kps : t->max_kps)
                || (!rvAfterburner[1] && (int32_t)speedLo[1] < was), e_fast, (int32_t)speedLo[1],
                rvAfterburner[1], rvAfterburner[1] ? t->ab_kps : t->max_kps);
        was = (int32_t)speedLo[1];
    }
    REQUIRE(turned >= TURN / 8 && (!slot_is_ship(1) || pt_km_to(1, 0, 0, 6000) > 500), e_still, turned,
            slot_is_ship(1) ? pt_km_to(1, 0, 0, 6000) : 0, 0);
}

/* M5.7 */
static void t_wingman_fights(void)
{
    static const char __far e_none[] = "in 60 s the wingman did the enemy no harm (the player held its fire)";
#ifdef RV_HOST
    static const char __far e_src[] = "the enemy was hurt, but not by the wingman's gunfire";
#endif
    int16_t i;
    int32_t h0;
    pt_alone(PT_PLAYER);
    playerCanBeDamaged = 0;
    g_sys.armed = 0;
    pt_ship(1, PT_PLAYER, SIDE_TERRAN, CTL_AI, 1500, 0, 0, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 7000, TURN / 2);
    h0 = pt_health(2);
    for (i = 0; i < 60 * SEC && slot_is_ship(2) && pt_health(2) == h0; i++)
        if (!pt_step())
            return;
    REQUIRE(!slot_is_ship(2) || pt_health(2) < h0, e_none, 0, 0, 0);
#ifdef RV_HOST
    EXPECT(host_bolt_hits[1] > 0, e_src, 0, 0, 0);
#endif
}

/* M5.8 */
static void t_formation(void)
{
    static const char __far e_far[] = "the wingman was %d km from the player %d s into a straight flight at cruise: "
        "it should keep with its leader (within 5,000 km)";
    static const char __far e_still[] = "the player flew only %d km in 60 s at cruise: nothing to keep up with (M1.1 "
        "first)";
    int16_t i;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->cruise_kps);
    pt_ship(1, PT_PLAYER, SIDE_TERRAN, CTL_AI, 1500, 0, 0, 0);
    for (i = 0; i < 60 * SEC; i++) {
        if (!pt_step())
            return;
        if (i >= 50 * SEC)
            REQUIRE(slot_is_ship(1) && pt_km(0, 1) < 5000, e_far, slot_is_ship(1) ? pt_km(0, 1) : -1, i / SEC, 0);
    }
    REQUIRE(pt_km_to(0, 0, 0, 0) > 10000, e_still, pt_km_to(0, 0, 0, 0), 0, 0);
}

/* the AI profiles a mission ship's record may name in its byte 17 (P_* + 1 of src/logic.c's; docs/ai.md) */
#define PT_AI_WAITER 8
#define PT_AI_NOLEFT 9

/* an AI enemy spawned through the mission's own way (outerSpawnShipEntity of a record at nav 0), with a profile: the
 * slot it took, -1 none */
static int16_t pt_ai_ship(int16_t type, int16_t profile, int16_t x, int16_t y, int16_t z, int16_t yaw)
{
    uint8_t *rec = missionShipTable + missionShipTable_SIZE * 1;
    uint16_t s;
    _fmemset(rec, 0, missionShipTable_SIZE);
    *(uint16_t *)rec = (uint16_t)type;
    *(uint16_t *)(rec + 2) = SIDE_AERA;
    *(uint16_t *)(rec + 4) = 0xFFFF;
    MSHIP_REC_KM(rec, 0) = x;
    MSHIP_REC_KM(rec, 1) = y;
    MSHIP_REC_KM(rec, 2) = z;
    rec[16] = RV_PILOT_NONE;
    rec[17] = (uint8_t)profile;
    s = outerSpawnShipEntity(1, 0);
    if (s > RV_MAX_SHIP_SLOT)
        return -1;
    ship_face((int16_t)s, yaw, 0);
    pt_speed((int16_t)s, 0);
    return (int16_t)s;
}

/* M5.9: an enemy that cannot turn left (profile P_NOLEFT), the player on its left: its nose never goes left, and
 * yet comes round onto the player (it rolls, and pulls up or down) */
static void t_no_left(void)
{
    static const char __far e_spawn[] = "no enemy spawned through outerSpawnShipEntity (slot %d)";
    static const char __far e_left[] = "the enemy's nose went left by %d (1024 a turn) at step %d: its profile may not";
    static const char __far e_turn[] = "in 20 s the enemy never pointed its nose at the player on its left (best %d/256)";
    RvVec3 f, r;
    int16_t s, i, a;
    int32_t best = -256, c;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    s = pt_ai_ship(PT_ENEMY, PT_AI_NOLEFT, -4000, 0, 3000, 0);   /* (facing +z: the player behind its left shoulder) */
    REQUIRE(s >= 1, e_spawn, s, 0, 0);
    for (i = 0; i < 20 * SEC && best < 230; i++) {
        f = gOrientationFrontVector[s];
        r = gOrientationRightVector[s];
        if (!pt_step())
            return;
        a = pt_angle(&gOrientationFrontVector[s], &f, &r);   /* (the nose's move toward its own right: a yaw) */
        REQUIRE(a >= -2, e_left, a, i + 1, 0);
        if ((c = pt_aim(s, 0)) > best)
            best = c;
    }
    REQUIRE(best >= 230, e_turn, best, 0, 0);
}

/* M5.10: an enemy that waits (profile P_WAITER), 4,000 km ahead of a player pointing at it: it stays still and cloaked
 * while the player watches it; once the player turns away it comes, uncloaked, and gets its nose on the player */
static void t_waiter(void)
{
    static const char __far e_spawn[] = "no enemy spawned through outerSpawnShipEntity (slot %d)";
    static const char __far e_moved[] = "the waiting enemy moved %d km in 5 s with the player pointing at it";
    static const char __far e_cloak[] = "the waiting enemy is not cloaked (rvFlags %d): bit 16 while it waits";
    static const char __far e_still[] = "10 s after the player turned away, the enemy has moved only %d km (speed %d)";
    static const char __far e_shown[] = "the enemy came but stayed cloaked (rvFlags %d)";
    static const char __far e_turn[] = "in 15 s after coming the enemy never pointed its nose at the player (best %d/256)";
    int16_t s, i;
    int32_t best = -256, c;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    s = pt_ai_ship(PT_ENEMY, PT_AI_WAITER, 0, 0, 4000, TURN / 2);   /* (facing the player, who faces it) */
    REQUIRE(s >= 1, e_spawn, s, 0, 0);
    if (!pt_steps(5 * SEC))
        return;
    REQUIRE(pt_km_to(s, 0, 0, 4000) < 50, e_moved, pt_km_to(s, 0, 0, 4000), 0, 0);
    REQUIRE(rvFlags[s] & 16, e_cloak, rvFlags[s], 0, 0);
    ship_face(0, TURN / 4, 0);          /* (the player turns away: its chance) */
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE(pt_km_to(s, 0, 0, 4000) > 500, e_still, pt_km_to(s, 0, 0, 4000), (int32_t)speedLo[s], 0);
    REQUIRE(!(rvFlags[s] & 16), e_shown, rvFlags[s], 0, 0);
    for (i = 0; i < 15 * SEC && best < 230; i++) {
        if (!pt_step())
            return;
        if (slot_is_ship(s) && (c = pt_aim(s, 0)) > best)
            best = c;
    }
    REQUIRE(best >= 230, e_turn, best, 0, 0);
}

/* ======================================================================================================== */
/* M6: collisions (step_collisions) */

/* --- M5.11, M5.12: the old game's first mission, measured (tests/wc1/README.md), as the line fighter's envelope --- */

#define PT_AI_FIGHTER 2                 /* P_FIGHTER + 1 */
#define OLD_PASS_FAR 2500               /* its passes: closest points under this */
#define OLD_SHOT_NEAR 1500              /* its first shot 2,455-5,129 km out */
#define OLD_SHOT_FAR 6000
#define OLD_BACK_STEPS (15 * SEC)       /* its nose back on the player after a pass in 1-3 s mostly, 12 at most */
#define OLD_GAP_STEPS 12                /* its volleys 0.75 s apart: never nearer than this (0.4 s) */

typedef struct {
    int16_t passes, bumps, late, gap;   /* passes; collision damage; passes with the nose not back in time; the
                                           shortest gap between volleys (steps; 0x7FFF none) */
    int32_t nearest, first_shot;        /* the closest it came (km); the first shot's range (km; 0 none) */
} Duel;

/* the player's nose onto a slot (the old game's rig: its "face" key every 3 s) */
static void pt_face(int16_t s)
{
    int32_t x = (gPositionVector[s].x - gPositionVector[0].x) / 256, y = (gPositionVector[s].y - gPositionVector[0].y) / 256,
            z = (gPositionVector[s].z - gPositionVector[0].z) / 256, horiz = pt_len(x, 0, z);
    ship_face(0, lf_atan2(x, z), lf_atan2(y, horiz));
}

/* a flight against one AI fighter (the old game's first mission's Dralthi: a Nicander with the line fighter's profile),
 * 15,000 km ahead and to the left, crossing at the 200 setting; the player a Schroedinger at the 300 setting, its guns
 * silent, invulnerable, flying straight (hunt 0) or facing the fighter every `hunt` seconds; for `secs`.  The fighter's
 * passes (closest points under OLD_PASS_FAR), bumps (its hull down, a second apart: the player never fires), the first shot's range,
 * the shortest gap between its volleys, and the passes after which its nose was not back on the player within
 * OLD_BACK_STEPS.  0 when the world broke (the test has failed) */
static int16_t pt_old_duel(int16_t hunt, int16_t secs, Duel *d)
{
    int16_t s, i, falling = 0, since_pass = 0x7FFF, since_fire = 0x7FFF, since_bump = 0x7FFF, open_ = 0;
    int32_t prev = INT32_C(0x7FFFFFFF), low = INT32_C(0x7FFFFFFF), hull;
    uint16_t a0;
    memset(d, 0, sizeof(*d));
    d->nearest = INT32_C(0x7FFFFFFF);
    d->gap = 0x7FFF;
    pt_alone(PT_PLAYER);
    randomSeed = UINT32_C(0x52560000) + 5000 + (uint32_t)hunt;   /* (its own dice, whatever its place in the table) */
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_speed(0, 300);
    s = pt_ai_ship(PT_ENEMY, PT_AI_FIGHTER, -7000, 700, 13500, TURN / 4);
    if (s < 1)
        return 0;
    pt_speed(s, 200);
    hull = pt_health(s);
    a0 = shots_added();
    for (i = 0; i < secs * SEC && slot_is_ship(s); i++) {
        int32_t km;
        if (hunt && i % (hunt * SEC) == 0)
            pt_face(s);
        if (!pt_step())
            return 0;
        km = pt_km(0, s);
        if (km < d->nearest)
            d->nearest = km;
        /* a pass: the distance fell under OLD_PASS_FAR and rises again (a second after the last) */
        if (km < prev) {
            falling = 1;
            low = km;
        } else if (falling && low < OLD_PASS_FAR && since_pass > SEC) {
            falling = 0;
            d->passes++;
            since_pass = 0;
            open_ = 1;
        } else
            falling = 0;
        prev = km;
        if (since_pass < 0x7FFF)
            since_pass++;
        if (open_ && pt_aim(s, 0) >= 230)
            open_ = 0;
        if (open_ && since_pass > OLD_BACK_STEPS) {
            open_ = 0;
            d->late++;
        }
        if (pt_health(s) < hull) {      /* (a collision hurts every few steps while it lasts: one bump a second) */
            if (since_bump > SEC)
                d->bumps++;
            since_bump = 0;
            hull = pt_health(s);
        }
        if (since_bump < 0x7FFF)
            since_bump++;
        if (shots_added() != a0) {
            a0 = shots_added();
            if (!d->first_shot)
                d->first_shot = km;
            if (since_fire > 2 && since_fire < d->gap)   /* (a volley over two steps is one) */
                d->gap = since_fire;
            since_fire = 0;
        }
        if (since_fire < 0x7FFF)
            since_fire++;
    }
    return 1;
}

/* M5.11: the player flying straight: the fighter comes in and shoots from the old game's range, passes without a bump,
 * is back on the player in time, and fires a volley at a time */
static void t_old_straight(void)
{
    static const char __far e_shot[] = "in 60 s the fighter never fired at a player flying straight (nearest %d km)";
    static const char __far e_range[] = "its first shot came at %d km: the old game's from 2,455-5,129";
    static const char __far e_pass[] = "in 60 s it never came within 2,500 km of the player (nearest %d km)";
    static const char __far e_bump[] = "it hit the player %d times (nearest %d km): the old game's passed clear";
    static const char __far e_back[] = "after %d of %d passes its nose was not back on the player within 15 s";
    static const char __far e_gap[] = "two volleys %d steps apart: the old game's fighters fired one every 0.75 s";
    Duel d;
    if (!pt_old_duel(0, 60, &d))
        return;
    REQUIRE(d.first_shot, e_shot, d.nearest, 0, 0);
    REQUIRE(d.first_shot >= OLD_SHOT_NEAR && d.first_shot <= OLD_SHOT_FAR, e_range, d.first_shot, 0, 0);
    REQUIRE(d.passes >= 1, e_pass, d.nearest, 0, 0);
    REQUIRE(d.bumps == 0, e_bump, d.bumps, d.nearest, 0);
    REQUIRE(d.late == 0, e_back, d.late, d.passes, 0);
    REQUIRE(d.gap >= OLD_GAP_STEPS, e_gap, d.gap, 0, 0);
}

/* M5.12: the player hunting (its nose onto the fighter every 3 s, as the old game's rig did): many passes, at most one
 * bump, the nose back in time after all but one, the volleys apart */
static void t_old_hunt(void)
{
    static const char __far e_pass[] = "in 90 s of hunting the fighter passed the player %d times (nearest %d km): the old "
        "game's some 15";
    static const char __far e_bump[] = "it hit the hunting player %d times (nearest %d km): the old game's once in 52 passes";
    static const char __far e_back[] = "after %d of %d passes its nose was not back on the player within 15 s";
    static const char __far e_gap[] = "two volleys %d steps apart: the old game's fighters fired one every 0.75 s";
    Duel d;
    if (!pt_old_duel(3, 90, &d))
        return;
    REQUIRE(d.passes >= 5, e_pass, d.passes, d.nearest, 0);
    REQUIRE(d.bumps <= 1, e_bump, d.bumps, d.nearest, 0);
    REQUIRE(d.late <= 1, e_back, d.late, d.passes, 0);
    REQUIRE(d.gap >= OLD_GAP_STEPS, e_gap, d.gap, 0, 0);
}

/* --- M5.13-M5.16: the old game's rules read in its code (tests/wc1/README.md): the held fire, the cone, the fear --- */

/* the shots an AI enemy 3,000 km ahead, facing a still and invulnerable player, fires in `secs` with the player
 * within `cos` of its nose (shots_added, the nose read after the step), *wide the rest, *gap the shortest gap between
 * its volleys; the player's selected target is the enemy (`selected`); the rules as given.  -1: the world broke */
static int16_t pt_shots_in_cone(uint8_t rules, int16_t selected, int16_t secs, int16_t cos, int16_t *wide, int16_t *gap,
                                int32_t ex, int32_t ey, int32_t ez, int16_t eyaw, int32_t kps)
{
    uint8_t keep = g_rule_flags;
    uint16_t a0;
    int16_t i, in = 0, since = 0x7FFF;
    *wide = 0;
    *gap = 0x7FFF;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, ex, ey, ez, eyaw);
    pt_speed(0, kps);                   /* (both under way, or both still) */
    pt_speed(1, kps);
    g_sys.target = selected ? 1 : -1;
    g_rule_flags = rules;
    a0 = shots_added();
    for (i = 0; i < secs * SEC && slot_is_ship(1); i++) {
        if (!pt_step()) {
            g_rule_flags = keep;
            g_sys.target = -1;
            return -1;
        }
        if (shots_added() != a0) {
            int32_t a = pt_aim(1, 0);
            a0 = shots_added();
            if (a > cos)
                in++;
            else
                (*wide)++;
            if (since > 2 && since < *gap)
                *gap = since;
            since = 0;
        }
        if (since < 0x7FFF)
            since++;
    }
    g_rule_flags = keep;
    g_sys.target = -1;
    return in;
}

/* M5.13: hold=on: the player's selected target fires nothing with the player within 37 degrees of its nose; with the
 * rule off, or the player's target nobody, it does */
static void t_hold_fire(void)
{
    static const char __far e_held[] = "the player's selected target fired %d shots in 15 s with the player within 33 "
        "degrees of its nose: under hold=on it holds its guns within 37 (holds_fire)";
    static const char __far e_free[] = "with hold=off the enemy fired %d shots in 15 s with the player within 33 degrees "
        "of its nose: it should fire";
    static const char __far e_other[] = "the player's target being nobody, the enemy fired %d shots in 15 s with the "
        "player within 33 degrees of its nose: only the selected target holds";
    int16_t held, free_, other, wide, gap;
    held = pt_shots_in_cone(RV_RULE_AIM_CONE | RV_RULE_HOLD_FIRE, 1, 15, 215, &wide, &gap, 0, 0, 3000, TURN / 2, 0);
    if (held < 0)
        return;
    free_ = pt_shots_in_cone(RV_RULE_AIM_CONE, 1, 15, 215, &wide, &gap, 0, 0, 3000, TURN / 2, 0);
    if (free_ < 0)
        return;
    other = pt_shots_in_cone(RV_RULE_AIM_CONE | RV_RULE_HOLD_FIRE, 0, 15, 215, &wide, &gap, 0, 0, 3000, TURN / 2, 0);
    if (other < 0)
        return;
    REQUIRE(held == 0, e_held, held, 0, 0);
    REQUIRE(free_ > 0, e_free, free_, 0, 0);
    REQUIRE(other > 0, e_other, other, 0, 0);
}

/* M5.14: aim=cone: the enemy's shots come only within its laser's 46-degree cone and its volleys 13 of the old game's
 * frames apart (its refire: 26 steps), where under aim=lead they come the profile's fire_gap (29) apart; the enemy crossing the player's
 * way 45 degrees off, both at 300 kps.  (Shots with the nose 20-46 degrees off the target came of the pursuit that
 * fired while flying at the old approach point; with the approach silent, M5.19, the nose is on the target when the
 * guns go, and the cone shows only as the limit) */
static void t_cone_fire(void)
{
    static const char __far e_none[] = "under aim=cone the enemy fired nothing in 15 s";
    static const char __far e_wide[] = "%d of the enemy's shots came with the player over 50 degrees off its nose: the "
        "cone is 46";
    static const char __far e_gap[] = "two of the enemy's volleys %d steps apart: under aim=cone no nearer than 26 "
        "(AI_REFIRE_STEPS)";
    static const char __far e_slow[] = "under aim=cone the enemy's volleys came no nearer than %d steps: its guns go "
        "every 26 (AI_REFIRE_STEPS), not the profile's fire_gap";
    static const char __far e_lead[] = "under aim=lead two of the enemy's volleys came %d steps apart: the profile's "
        "fire_gap is 29";
    int16_t narrow, over, gap;
    narrow = pt_shots_in_cone(RV_RULE_AIM_CONE, 0, 15, 164, &over, &gap, 3500, 0, 3500, TURN * 5 / 8, 300);   /* (164: within 50 degrees) */
    if (narrow < 0)
        return;
    REQUIRE(narrow + over > 0, e_none, 0, 0, 0);
    REQUIRE(over == 0, e_wide, over, 0, 0);
    REQUIRE(gap >= 13 * WC_FRAME_STEPS - 1, e_gap, gap, 0, 0);
    REQUIRE(gap < 13 * WC_FRAME_STEPS + 7, e_slow, gap, 0, 0);
    narrow = pt_shots_in_cone(0, 0, 15, 164, &over, &gap, 3500, 0, 3500, TURN * 5 / 8, 300);
    if (narrow < 0)
        return;
    REQUIRE(gap >= 28, e_lead, gap, 0, 0);
}

/* the enemy's missiles under the hold: the enemy 2,500 km behind an invulnerable player flying straight at 300 kps,
 * on its tail at the same speed (the player on its nose, within its seekers' lock cone), `secs` seconds; how many
 * missiles it launched */
static int16_t pt_missiles_launched(uint8_t rules, int16_t selected, int16_t secs)
{
    uint8_t keep = g_rule_flags;
    int16_t i, n = 0, last = -1, m;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, -2500, 0);
    pt_speed(0, 300);                   /* (both under way: against a still player it breaks off every few seconds and
                                           its lock never completes) */
    pt_speed(1, 300);
    g_sys.target = selected ? 1 : -1;
    g_rule_flags = rules;
    for (i = 0; i < secs * SEC && slot_is_ship(1); i++) {
        if (!pt_step()) {
            g_rule_flags = keep;
            g_sys.target = -1;
            return -1;
        }
        if ((m = pt_missile_of(1)) >= 0 && m != last) {
            n++;
            last = m;
        }
    }
    g_rule_flags = keep;
    g_sys.target = -1;
    return n;
}

/* M5.17: the held fire holds the missiles too (the old wrapper skipped the whole weapons routine): the player's
 * selected target, on its tail, launches nothing in 40 s under hold=on, and does with the rule off */
static void t_hold_missiles(void)
{
    static const char __far e_held[] = "the player's selected target launched %d missiles in 40 s from the player's tail "
        "under hold=on: the hold withholds the missiles too (aim_and_fire)";
    static const char __far e_free[] = "with hold=off the enemy launched no missile in 40 s from the player's tail: it "
        "has two seekers and a lock";
    int16_t held, free_;
    held = pt_missiles_launched(RV_RULE_AIM_CONE | RV_RULE_HOLD_FIRE, 1, 40);
    if (held < 0)
        return;
    free_ = pt_missiles_launched(RV_RULE_AIM_CONE, 1, 40);
    if (free_ < 0)
        return;
    REQUIRE(held == 0, e_held, held, 0, 0);
    REQUIRE(free_ > 0, e_free, free_, 0, 0);
}

/* M5.18: the missile dodge fires whatever the hold (the old maneuver 32): the player's selected target 3,000 km ahead
 * facing the player holds its guns, until a missile of the player's homes on it from ahead: then it shoots */
static void t_dodge_fire(void)
{
    static const char __far e_none[] = "the player's selected target, holding its guns, fired %d shots in the 6 s a "
        "missile homed on it from ahead: a missile within 8,000 km ahead ends the hold (missile_ahead)";
    static const char __far e_nolaunch[] = "the player's ship (type 0x%x) has no missile launcher";
    static const char __far e_nohome[] = "the player's missile (slot %d) homes on %d, not on the enemy (slot 1)";
    uint8_t keep = g_rule_flags;
    uint16_t a0, m;
    int16_t i, l, n = 0;
    pt_alone(PT_HOMER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 3000, TURN / 2);
    g_sys.target = 1;
    g_sys.lock = 1;
    g_rule_flags = RV_RULE_AIM_CONE | RV_RULE_HOLD_FIRE;
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, PT_HOMER, 0, 0);
    m = fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE && missileTarget[m] == 1, e_nohome, m,
            m <= RV_MAX_SHIP_SLOT ? missileTarget[m] : -1, 0);
    a0 = shots_added();
    for (i = 0; i < 6 * SEC && slot_is_ship(1); i++) {
        if (!pt_step())
            break;
        if (shots_added() != a0) {
            a0 = shots_added();
            n++;
        }
    }
    g_rule_flags = keep;
    g_sys.target = -1;
    g_sys.lock = 0;
    REQUIRE(n > 0, e_none, n, 0, 0);
}

/* the enemy 6,000 km ahead of a still, invulnerable player, facing it, `secs` seconds under `rules`: its shots, how
 * many came while it was still ahead of the player's wing line, and the step of its first; -1 when the world broke */
static int16_t pt_approach_shots(uint8_t rules, int16_t secs, int16_t *ahead, int16_t *first)
{
    uint8_t keep = g_rule_flags;
    uint16_t a0;
    int16_t i, n = 0;
    *ahead = 0;
    *first = -1;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 6000, TURN / 2);
    g_rule_flags = rules;
    a0 = shots_added();
    for (i = 0; i < secs * SEC && slot_is_ship(1); i++) {
        if (!pt_step()) {
            g_rule_flags = keep;
            return -1;
        }
        if (shots_added() != a0) {
            a0 = shots_added();
            n++;
            if (*first < 0)
                *first = i;
            if (gPositionVector[1].z > gPositionVector[0].z)   /* (the player faces +z, still) */
                (*ahead)++;
        }
    }
    g_rule_flags = keep;
    return n;
}

/* M5.19: the approach is silent (the old maneuver 40; the user: keep it): under aim=cone an enemy 6,000 km ahead
 * facing a still player (the player facing it: the choice too takes the approach) fires nothing for 7 s, the time
 * it takes to fly past and come round (it fires by 25 s); under aim=lead (the measured pursuit) it fires head on
 * within 4 s */
static void t_approach_silent(void)
{
    static const char __far e_early[] = "under aim=cone the enemy's first shot came at step %d (%d shots in 25 s, %d of "
        "them ahead of the player's wing line): the old approach has no guns (T_APPROACH), so none before 7 s";
    static const char __far e_none[] = "under aim=cone the enemy fired nothing in 25 s: once round the player it fights "
        "with its guns (T_PURSUE)";
    static const char __far e_lead[] = "under aim=lead the enemy's first shot came at step %d (%d shots in 4 s): the "
        "measured pursuit fires head on when the guns bear";
    int16_t n, ahead, first;
    n = pt_approach_shots(RV_RULE_AIM_CONE, 25, &ahead, &first);
    if (n < 0)
        return;
    REQUIRE(n > 0, e_none, 0, 0, 0);
    REQUIRE(first >= 7 * SEC, e_early, first, n, ahead);
    n = pt_approach_shots(0, 4, &ahead, &first);
    if (n < 0)
        return;
    REQUIRE(n > 0 && first < 4 * SEC, e_lead, first, n, 0);
}

/* the volleys a Shizong (the Rlaan's disabler, four Cripplers) fires in `secs` from 3,000 km ahead of a still and
 * invulnerable player that has it selected, under `rules` (set after the scene: pt_alone loads the test mission's) */
static int16_t pt_shizong_volleys(uint8_t rules, int16_t secs)
{
    uint16_t a0;
    int16_t i, n = 0;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, 0x13, SIDE_RLAAN, CTL_AI, 0, 0, 3000, TURN / 2);
    g_sys.target = 1;
    g_rule_flags = rules;
    a0 = shots_added();
    for (i = 0; i < secs * SEC && slot_is_ship(1); i++) {
        if (!pt_step()) {
            g_sys.target = -1;
            return -1;
        }
        pt_speed(0, 0);
        if (shots_added() != a0) {
            a0 = shots_added();
            n++;
        }
    }
    g_sys.target = -1;
    return n;
}

/* M5.20: the held fire is the easy mode's (the user, 2026-11-03: a selected Shizong circled a still player for 15
 * minutes and never fired; "have a global easy mode that can turn it on...but keep it off for most play"): the first
 * mission's own rules leave it off and its selected Shizong fires; the bunk menu's easy (g_easy) turns it on in
 * camp_load_rules, and the Shizong holds (measured: 38 volleys in 120 s, and none) */
static void t_easy_hold(void)
{
    static const char __far e_data[] = "the first mission's rules have the held fire on (flags %d): it is the easy mode's";
    static const char __far e_easy[] = "under the easy mode the rules are %d: camp_load_rules should add the held fire (%d)";
    static const char __far e_fires[] = "the selected Shizong fired %d volleys in 30 s on a still player under the first "
        "mission's rules: it should fire";
    static const char __far e_holds[] = "under the easy mode the selected Shizong fired %d volleys in 30 s, the player "
        "ahead of its nose: it should hold";
    uint8_t keep = g_rule_flags, keep_easy = g_easy, normal, easy;
    int16_t fired, held;
    camp_mission(0, &g_tm);
    g_easy = 0;
    camp_load_rules(&g_tm);
    normal = g_rule_flags;
    g_easy = 1;
    camp_load_rules(&g_tm);
    easy = g_rule_flags;
    g_easy = keep_easy;
    fired = pt_shizong_volleys(normal, 30);
    held = pt_shizong_volleys(easy, 30);
    g_rule_flags = keep;
    if (fired < 0 || held < 0)
        return;
    REQUIRE(!(normal & RV_RULE_HOLD_FIRE), e_data, normal, 0, 0);
    REQUIRE(easy == (normal | RV_RULE_HOLD_FIRE), e_easy, easy, RV_RULE_HOLD_FIRE, 0);
    REQUIRE(fired > 0, e_fires, fired, 0, 0);
    REQUIRE(held == 0, e_holds, held, 0, 0);
}

/* a scared fighter: its armour gone and its hull nearly, 2,000 km ahead of the player and flying the same way (the
 * player on its tail, pointing at it), the player still and invulnerable */
static int16_t pt_scared(void)
{
    const RvShipType __far *t;
    int16_t k;
    pt_alone(PT_PLAYER);
    randomSeed = UINT32_C(0x52560000) + 5015;   /* (its own dice: the picks, and so the fear's climb, are the same every run) */
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 2000, 0);
    if ((t = slot_type(1)) == 0)
        return 0;
    for (k = 0; k < 4; k++)
        ARMOR(1, k) = 0;
    damagePoints[1] = (uint16_t)(t->hull > 10 ? t->hull - 10 : 0);
    return 1;
}

/* M5.15: the leave: a scared fighter with the player on its tail grows afraid (its fear climbs with its health at every
 * pick, and with the player behind it: some 30 s of its passes at a still player), leaves (down, for an odd slot) and
 * is removed beyond 16,000 km of the player, not killed */
static void t_leave(void)
{
    static const char __far e_stay[] = "in 90 s a scared fighter tailed by the player neither left nor was removed "
        "(still %d00 km off, %d00 km down): fear 30 is the leave (fear_step, leave_step)";
    static const char __far e_dead[] = "the scared fighter died (shipAiState 9) rather than leaving";
    static const char __far e_near[] = "the fighter was removed %d00 km from the player: the leave removes it beyond "
        "16,000";
    int32_t far_ = 0, down = 0;
    int16_t i, died = 0;
    if (!pt_scared())
        return;
    for (i = 0; i < 90 * SEC && slot_is_ship(1); i++) {
        if (!pt_step())
            return;
        if (shipAiState[1] == 9)
            died = 1;
        far_ = pt_km(0, 1);
        if (-gPositionVector[1].y / KM > down)
            down = -gPositionVector[1].y / KM;
    }
    REQUIRE(!died, e_dead, 0, 0, 0);
    REQUIRE(!slot_is_ship(1), e_stay, (int16_t)(far_ / 100), (int16_t)(down / 100), 0);
    REQUIRE(far_ > 15000, e_near, (int16_t)(far_ / 100), 0, 0);
}

/* M5.16: with a capital ship of its side about, the scared fighter runs to it instead (T_HOME: within 5,000 km of it
 * at some point in 30 s), is not removed, and fights on with its fear gone */
static void t_home(void)
{
    static const char __far e_gone[] = "with its side's capital ship 15,000 km off, the scared fighter was removed: it "
        "should run to the capital ship (T_HOME), not leave";
    static const char __far e_far[] = "in 90 s the scared fighter never came within 5,000 km of its side's capital ship "
        "(nearest %d00 km): T_HOME";
    int32_t nearest = INT32_C(0x7FFFFFFF), d;
    int16_t i;
    if (!pt_scared())
        return;
    pt_ship(2, 0x28, SIDE_AERA, CTL_FROZEN, 0, 0, 15000, TURN / 2);   /* (an Agasicles, a destroyer: 13,000 km from the
                                           fighter, a 6 s run home at the Dralthi's afterburner, 1,800 km/s, gets there) */
    for (i = 0; i < 90 * SEC && slot_is_ship(1); i++) {
        if (!pt_step())
            return;
        if (slot_is_ship(2) && (d = pt_km(1, 2)) < nearest)
            nearest = d;
    }
    REQUIRE(slot_is_ship(1), e_gone, 0, 0, 0);
    REQUIRE(nearest < 5000, e_far, (int16_t)(nearest / 100), 0, 0);
}

/* M6.1 */
static void t_ram(void)
{
    static const char __far e_test[] = "the test's two ships do not overlap to begin with";
    static const char __far e_stuck[] = "still inside one another after 10 steps (collide_ships, %d positions deep: "
        "%d is touching)";
    static const char __far e_hurt[] = "a collision hurt the player by %d and the enemy by %d: both take damage";
    LfVec push;
    int32_t depth, h0, h1, low0, low1;
    int16_t i;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 150, TURN / 2);   /* (inside one another: the radii are 100 and 160 km) */
    REQUIRE(collide_ships(0, 1, &push, &depth), e_test, 0, 0, 0);
    gVelocityVector[0] = vec3(0, 0, 10 * KM);   /* (closing at 10 km a step: the hurt is the closing speed's) */
    h0 = low0 = pt_health(0);
    h1 = low1 = pt_health(1);
    for (i = 0; i < 10; i++) {          /* (the lowest the health went: the shields come back as the steps pass) */
        if (!pt_step())
            return;
        if (pt_health(0) < low0)
            low0 = pt_health(0);
        if (slot_is_ship(1) && pt_health(1) < low1)
            low1 = pt_health(1);
    }
    REQUIRE(!slot_is_ship(1) || !collide_ships(0, 1, &push, &depth) || depth <= collide_half(0) / 4, e_stuck, depth,
            collide_half(0) / 4, 0);
    REQUIRE(low0 < h0 && (low1 < h1 || shipAiState[1] == 9 || !slot_is_ship(1)), e_hurt, h0 - low0,
            slot_is_ship(1) ? h1 - low1 : 0, 0);
}

/* M6.3: what a collision costs is the mission's rule: the old game's (ram=old), the square of the closing speed in
 * km a step, halved (10 km a step: 50 points), or the soft one (ram=soft: 2 and a point per 125 kps closing: 3) */
static int16_t pt_ram_cost(uint8_t soft)
{
    int32_t h0;
    uint8_t keep = g_rule_flags;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 150, TURN / 2);
    pt_speed(0, 200);                   /* (10 km a step: a set velocity alone decays at the slide rate before the test) */
    g_rule_flags = (uint8_t)(soft ? keep | RV_RULE_RAM_SOFT : keep & ~RV_RULE_RAM_SOFT);
    h0 = pt_health(0);
    if (!pt_step()) {
        g_rule_flags = keep;
        return -1;
    }
    g_rule_flags = keep;
    return (int16_t)(h0 - pt_health(0));
}

static void t_ram_cost(void)
{
    static const char __far e_old[] = "closing at 10 km a step under the old rule cost the player %d: the square of the "
        "closing speed, halved, is 50 (less a point of shield back)";
    static const char __far e_soft[] = "closing at 10 km a step under the soft rule cost the player %d: 2 and a point per "
        "125 kps is 3";
    int16_t old_cost = pt_ram_cost(0), soft_cost;
    if (old_cost < 0)
        return;
    soft_cost = pt_ram_cost(1);
    if (soft_cost < 0)
        return;
    REQUIRE(old_cost >= 49 && old_cost <= 50, e_old, old_cost, 0, 0);
    REQUIRE(soft_cost >= 2 && soft_cost <= 4, e_soft, soft_cost, 0, 0);
}

/* M6.6: what a collision may cost (the user, 2026-11-02): the player with its shield up loses that shield and that
 * armour at most, a system besides, and the enemy as much; that armour gone, the hull down to 40% of what it was;
 * two NPCs head on at 50 km a step each lose that shield and half that armour at most, and live; a fighter running
 * into a capital ship over 100 kps stops dead, throttle and all, and under it does not */
static void t_ram_caps(void)
{
    static const char __far e_test[] = "the test's two ships do not overlap to begin with (case %d)";
    static const char __far e_capped[] = "flown head on into a Nicander at 450 kps with the front armour at %d: the "
        "armour is %d after and the hull lost %d: shield and armour only";
    static const char __far e_system[] = "the player's collision broke no system: one at least";
    static const char __far e_light[] = "a bump at 200 kps (50 points, under the front shield and armour) broke %d system "
        "points and cost %d: the old rule's damage and no system point";
    static const char __far e_same[] = "the player lost %d and the Nicander %d: the enemy takes what the player took";
    static const char __far e_hull[] = "the front armour gone, the player's hull went from %d left to %d: to 40 percent";
    static const char __far e_npc[] = "two Nicanders head on at 50 km a step: slot %d lost %d (at most %d), or died";
    static const char __far e_stop[] = "into a capital ship at 450 kps the player's speed is %d (throttle %d): stopped";
    static const char __far e_creep[] = "into a capital ship at 80 kps the player's throttle is %d: unchanged (it creeps "
        "on)";
    LfVec push;
    int32_t depth, h0, h1, d0, d1;
    int16_t k, sys, a0, room0, room1, cap[3];
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 150, TURN / 2);
    REQUIRE(collide_ships(0, 1, &push, &depth), e_test, 1, 0, 0);
    pt_speed(0, 450);
    a0 = (int16_t)ARMOR(0, 0);
    room0 = (int16_t)(slot_type(0)->hull - damagePoints[0]);
    h0 = pt_health(0);
    h1 = pt_health(1);
    if (!pt_step())
        return;
    room1 = (int16_t)(slot_type(0)->hull - damagePoints[0]);
    REQUIRE(ARMOR(0, 0) == 0 && room1 == room0, e_capped, a0, ARMOR(0, 0), room0 - room1);
    for (k = 0, sys = 0; k < SYS_COUNT; k++)
        sys = (int16_t)(sys + sys_level(k));
    REQUIRE(sys > 0, e_system, 0, 0, 0);
    d0 = h0 - pt_health(0);
    d1 = h1 - pt_health(1);
    REQUIRE(labs(d0 - d1) <= 3, e_same, d0, d1, 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 150, TURN / 2);
    pt_speed(0, 200);
    h0 = pt_health(0);
    if (!pt_step())
        return;
    for (k = 0, sys = 0; k < SYS_COUNT; k++)
        sys = (int16_t)(sys + sys_level(k));
    d0 = h0 - pt_health(0);
    REQUIRE(sys == 0 && d0 >= 48 && d0 <= 50, e_light, sys, d0, 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 150, TURN / 2);
    pt_speed(0, 450);
    ARMOR(0, 0) = 0;
    room0 = (int16_t)(slot_type(0)->hull - damagePoints[0]);
    if (!pt_step())
        return;
    room1 = (int16_t)(slot_type(0)->hull - damagePoints[0]);
    REQUIRE(room1 >= room0 * 2 / 5 - 1 && room1 <= room0 * 2 / 5 + 1, e_hull, room0, room1, 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 5000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 5060, TURN / 2);
    REQUIRE(collide_ships(1, 2, &push, &depth), e_test, 2, 0, 0);
    gVelocityVector[1] = vec3(0, 0, 25 * KM);
    gVelocityVector[2] = vec3(0, 0, -25 * KM);
    for (k = 1; k <= 2; k++)
        cap[k] = (int16_t)(SHIELD(k, 0) + ARMOR(k, 0) / 2 + 2);   /* (+2: a point of each shield back in the step) */
    h0 = pt_health(1);
    h1 = pt_health(2);
    if (!pt_step())
        return;
    d0 = h0 - pt_health(1);
    d1 = h1 - pt_health(2);
    REQUIRE(d0 > 0 && d0 <= cap[1] && shipAiState[1] != 9, e_npc, 1, d0, cap[1]);
    REQUIRE(d1 > 0 && d1 <= cap[2] && shipAiState[2] != 9, e_npc, 2, d1, cap[2]);
    for (k = 0; k < 2; k++) {
        int16_t kps = k ? 80 : 450;
        pt_alone(PT_PLAYER);
        playerCanBeDamaged = 0;
        pt_ship(1, 0x28, SIDE_AERA, CTL_FROZEN, 0, 0, 0, 0);   /* (an Agasicles about the player) */
        REQUIRE(collide_ships(0, 1, &push, &depth), e_test, 3 + k, 0, 0);
        pt_speed(0, kps);
        if (!pt_step())
            return;
        if (k)
            REQUIRE(setSpeed[0] == 80, e_creep, setSpeed[0], 0, 0);
        else
            REQUIRE(setSpeed[0] == 0 && speedLo[0] == 0, e_stop, speedLo[0], setSpeed[0], 0);
    }
}

/* M6.2 */
static void t_no_ghosts(void)
{
    static const char __far e_in[] = "flown at another ship, the player stayed inside it %d steps in a row (step %d): "
        "the push should part them";
    static const char __far e_never[] = "the player, flown at a ship %d km ahead, never hurt it: no collision (it "
        "flew through, or a collision does no harm)";
    LfVec push;
    int32_t depth, h1;
    int16_t i, run = 0;
    pt_alone(PT_PLAYER);
    pt_speed(0, slot_type(0)->max_kps);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 1500, TURN / 2);
    playerCanBeDamaged = 0;
    h1 = pt_health(1);
    for (i = 0; i < 5 * SEC; i++) {
        if (!pt_step())
            return;
        if (slot_is_ship(1) && collide_ships(0, 1, &push, &depth) && depth > collide_half(0) / 4)   /* (touching: a step's worth) */
            REQUIRE(++run <= 3, e_in, run, i, 0);
        else
            run = 0;
    }
    REQUIRE(!slot_is_ship(1) || pt_health(1) < h1 || shipAiState[1] == 9, e_never, 1500, 0, 0);
}

/* ======================================================================================================== */
/* M7: missiles (fireGunFromShip's launchers, step_missiles) */

static const char __far e_nolaunch[] = "the test's ship (type %d) has no missile launcher";
static const char __far e_nomissile[] = "fireGunFromShip on its launcher (gun %d) returned %d: a missile's slot "
    "(ET_MISSILE) expected";

/* M7.1 */
static void t_launch(void)
{
    static const char __far e_slot[] = "fireGunFromShip on a launcher (gun %d) returned %d: a missile's slot "
        "(maybeEntityType ET_MISSILE) expected";
    static const char __far e_parent[] = "the missile's parentShipForShip is %d: its launcher (0)";
    static const char __far e_rounds[] = "the launcher has %d rounds after a launch from %d";
    static const char __far e_far[] = "the missile starts %d km from its ship";
    int16_t l, r0;
    uint16_t m;
    pt_alone(PT_PLAYER);
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, PT_PLAYER, 0, 0);
    r0 = GUN_STATE(0, l) & 0x7F;
    m = fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_slot, l, m, 0);
    REQUIRE(parentShipForShip[m] == 0, e_parent, parentShipForShip[m], 0, 0);
    REQUIRE((GUN_STATE(0, l) & 0x7F) == r0 - 1, e_rounds, GUN_STATE(0, l) & 0x7F, r0, 0);
    REQUIRE(pt_km(0, (int16_t)m) <= 2 * collide_half(0) / 256, e_far, pt_km(0, (int16_t)m), 0, 0);
}

/* M7.2 */
static void t_missile_key(void)
{
    static const char __far e_none[] = "Enter (g_pilot.missile) launched no missile";
    static const char __far e_count[] = "%d missiles from %d presses with %d rounds";
    int16_t l, r0, i, k, n = 0, seen[RV_MAX_SHIP_SLOT + 1];
    pt_alone(PT_PLAYER);
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, PT_PLAYER, 0, 0);
    r0 = 0;                             /* (every launcher's rounds: the chosen one empty, Enter fires the next's) */
    for (k = 0; k < GUN_COUNT(0); k++)
        if (weapon(GUN_TYPE(0, k)) && weapon(GUN_TYPE(0, k))->kind != RV_WK_GUN
            && weapon(GUN_TYPE(0, k))->kind != RV_WK_TURRET)
            r0 = (int16_t)(r0 + (GUN_STATE(0, k) & 0x7F));
    for (i = 0; i <= RV_MAX_SHIP_SLOT; i++)
        seen[i] = 0;
    for (i = 0; i < r0 + 2; i++) {
        g_pilot.missile = 1;
        if (!pt_step())
            return;
        g_pilot.missile = 0;
        for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
            if (maybeEntityType[k] == ET_MISSILE && parentShipForShip[k] == 0 && !seen[k]) {
                seen[k] = 1;
                n++;
            }
        if (i == 0)
            REQUIRE(n == 1, e_none, 0, 0, 0);
        if (!pt_steps(2 * SEC))
            return;
        for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
            if (maybeEntityType[k] != ET_MISSILE)
                seen[k] = 0;
    }
    REQUIRE(n == r0, e_count, n, r0 + 2, r0);
}

/* M7.3 */
static void t_missile_flies(void)
{
    static const char __far e_gone[] = "the missile was gone after %d steps in empty space";
    static const char __far e_speed[] = "the missile moves %d positions a step: its weapon's speed is %d";
    static const char __far e_way[] = "the dumbfire flies %d/256 along its launcher's nose: straight ahead expected";
    int16_t l, m;
    RvVec3 p0, d;
    int32_t want, got;
    pt_alone(PT_PLAYER);
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, PT_PLAYER, 0, 0);
    want = KPS_STEP(weapon(GUN_TYPE(0, l))->speed_kps);
    m = (int16_t)fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m >= 1 && m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_nomissile, l, (uint16_t)m, 0);
    if (!pt_steps(SEC))
        return;
    REQUIRE(maybeEntityType[m] == ET_MISSILE, e_gone, SEC, 0, 0);
    p0 = gPositionVector[m];
    if (!pt_steps(10))
        return;
    REQUIRE(maybeEntityType[m] == ET_MISSILE, e_gone, SEC + 10, 0, 0);
    d.x = (gPositionVector[m].x - p0.x) / 10;
    d.y = (gPositionVector[m].y - p0.y) / 10;
    d.z = (gPositionVector[m].z - p0.z) / 10;
    got = pt_len(d.x, d.y, d.z);
    REQUIRE(got * 100 >= want * 80 && got * 100 <= want * 120, e_speed, got, want, 0);
    EXPECT(got && vec_dot(&d, &gOrientationFrontVector[0]) / got >= 250, e_way,
           got ? vec_dot(&d, &gOrientationFrontVector[0]) / got : 0, 0, 0);
}

/* a missile at a frozen Nicander, side on: ahead on its launcher's line (dumbfire) or off to the right (homing) */
static void t_missile_at(int16_t type, int32_t x, int32_t z, int16_t homing)
{
    static const char __far e_miss[] = "the missile did the target no harm in 10 s (%d km away)";
    static const char __far e_target[] = "the homing missile's missileTarget is %d: the target, slot %d";
    static const char __far e_left[] = "the missile's slot still flies after it hit";
    int16_t l, i;
    uint16_t m;
    int32_t h0;
    pt_alone(type);
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, type, 0, 0);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, x + (homing ? 0 : (int32_t)GUN_MOUNT_X(0, l) * collide_half(0) / 32 / 256),
            homing ? 0 : (int32_t)GUN_MOUNT_Y(0, l) * collide_half(0) / 32 / 256, z, TURN / 4);
    playerCanBeDamaged = 0;
    g_sys.target = 1;
    rvTarget[0] = 1;
    playerTarget = 1;
    g_sys.lock = 1;
    h0 = pt_health(1);
    m = fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_nomissile, l, m, 0);
    if (homing)
        REQUIRE(missileTarget[m] == 1, e_target, missileTarget[m], 1, 0);
    for (i = 0; i < 10 * SEC && pt_health(1) == h0 && slot_is_ship(1) && shipAiState[1] != 9; i++)
        if (!pt_step())
            return;
    REQUIRE(pt_health(1) < h0 || shipAiState[1] == 9 || !slot_is_ship(1), e_miss, pt_len(x, 0, z), 0, 0);
    if (!pt_steps(2))
        return;
    EXPECT(maybeEntityType[m] != ET_MISSILE || parentShipForShip[m] != 0, e_left, 0, 0, 0);
}

/* M7.4 */
static void t_missile_hits(void) { t_missile_at(PT_PLAYER, 0, 2500, 0); }

/* M7.5 */
static void t_missile_homes(void) { t_missile_at(PT_HOMER, 1500, 2500, 1); }

/* M7.7: a dumbfire sets off toward the chosen target's lead point within 15 degrees of the nose (the user's: "tilt
 * towards the target to help aim"), and hits it; with the target 30 degrees off it flies straight along the nose */
static void t_dumbfire_aims(void)
{
    static const char __far e_miss[] = "a dumbfire at a target 8 degrees off the nose did it no harm in 10 s";
    static const char __far e_straight[] = "a dumbfire with its target 30 degrees off sets off %d/256 along the nose: "
        "straight expected";
    int16_t l, i;
    uint16_t m;
    int32_t h0, along;
    pt_alone(PT_PLAYER);
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, PT_PLAYER, 0, 0);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 351, 0, 2500, TURN / 4);   /* (8 degrees right, side on) */
    playerCanBeDamaged = 0;
    g_sys.target = 1;
    rvTarget[0] = 1;
    playerTarget = 1;
    h0 = pt_health(1);
    m = fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_nomissile, l, m, 0);
    for (i = 0; i < 10 * SEC && pt_health(1) == h0 && slot_is_ship(1) && shipAiState[1] != 9; i++)
        if (!pt_step())
            return;
    REQUIRE(pt_health(1) < h0 || shipAiState[1] == 9 || !slot_is_ship(1), e_miss, 0, 0, 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 1443, 0, 2500, TURN / 4);   /* (30 degrees right) */
    g_sys.target = 1;
    rvTarget[0] = 1;
    playerTarget = 1;
    m = fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_nomissile, l, m, 0);
    along = vec_dot(&gOrientationFrontVector[m], &gOrientationFrontVector[0]) / 256;
    REQUIRE(along >= 255, e_straight, along, 0, 0);
}

/* M7.6 */
static void t_missile_ends(void)
{
    static const char __far e_long[] = "a missile fired into empty space still flies after %d steps (its range %d km, "
        "%d steps' worth): it ends at its range";
    int16_t l, i, life;
    uint16_t m;
    const RvWeapon __far *w;
    pt_alone(PT_PLAYER);
    REQUIRE((l = pt_launcher(0)) >= 0, e_nolaunch, PT_PLAYER, 0, 0);
    w = weapon(GUN_TYPE(0, l));
    life = (int16_t)((int32_t)w->range_km * KPS_HZ / w->speed_kps);
    m = fireGunFromShip(0, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_nomissile, l, m, 0);
    for (i = 0; i < 2 * life + SEC && maybeEntityType[m] == ET_MISSILE; i++)
        if (!pt_step())
            return;
    REQUIRE(maybeEntityType[m] != ET_MISSILE, e_long, i, w->range_km, life);
}

/* ======================================================================================================== */
/* M8: the mission (logic_start, step_mission, enterNavPoint, logic_end) */

#define NAV1_Z 20000                    /* km */
#define NAV2_X 12000
#define NAV2_Z 26000

/* a patrol: the player and a wingman at nav 0, two Nicanders at nav 1, an Agis at nav 2 */
static int16_t pt_patrol(void)
{
    int16_t i, pilot = RV_PILOT_NONE;
    for (i = 0; i < (int16_t)g_npilots && pilot == RV_PILOT_NONE; i++)
        if (g_pilots[i].role == RV_ROLE_PILOT)
            pilot = i;
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_nav(2, NAV2_X, 0, NAV2_Z);
    pt_mship(RV_SHIP_WINGMAN, SIDE_TERRAN, 0, 1, (uint8_t)pilot);
    pt_mship(PT_ENEMY, SIDE_AERA, 1, 2, RV_PILOT_NONE);
    pt_mship(PT_WEAK, SIDE_AERA, 2, 1, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    return pilot;
}

/* the player taken to a place (km), as the autopilot would (and the wingman with it), then a few steps */
static int16_t pt_go(int32_t x, int32_t z)
{
    int16_t s;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_TERRAN && rvController[s] == CTL_AI && !logic_is_home(s)) {
            gPositionVector[s].x = (x + 1500) * KM;
            gPositionVector[s].y = 0;
            gPositionVector[s].z = z * KM;
        }
    gPositionVector[0].x = x * KM;
    gPositionVector[0].y = 0;
    gPositionVector[0].z = z * KM;
    return pt_steps(2 * SEC);
}

/* every ship of a side killed, then their explosions waited out */
static int16_t pt_kill_side(int16_t side)
{
    int16_t s, i;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == side && shipAiState[s] != 9)
            (void)pt_hit(0xFFFF, s, 2000, 2, 0, 0);
    for (i = 0; i < 12 * SEC && pt_count(side, -1); i++)
        if (!pt_step())
            return 0;
    return 1;
}

/* M8.1 */
static void t_start(void)
{
    static const char __far e_player[] = "slot 0 is not the player's ship (type %d, controller %d): logic_start puts "
        "the player in slot 0, CTL_PLAYER, type g_camp.player_ship";
    static const char __far e_wing[] = "no wingman (side 0, an AI, %d of them) near the player at the start";
    static const char __far e_pilot[] = "the wingman's pilot (shipStateByte) is %d: the mission's %d";
    static const char __far e_record[] = "slot %d's slotMissionShip (%d) names no mission ship of its type and side";
    static const char __far e_early[] = "%d enemies out there at the start: the ships of nav 1 and 2 come when the "
        "player gets there";
    static const char __far e_nav[] = "currentNavPoint is %d at the start: 0";
    static const char __far e_inside[] = "the wingman starts %d km from the player, inside the two hulls (%d km): it "
        "spawns beside its leader, not on it";
    int16_t pilot, s, w = -1, n = 0;
    const uint8_t *rec;
    pilot = pt_patrol();
    REQUIRE(slot_is_ship(0) && rvController[0] == CTL_PLAYER && gInstanceShipTypes[0] == PT_PLAYER, e_player,
            gInstanceShipTypes[0], rvController[0], 0);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_TERRAN && rvController[s] == CTL_AI && pt_km(0, s) < 10000) {
            w = s;
            n++;
        }
    REQUIRE(n == 1, e_wing, n, 0, 0);
    REQUIRE(pt_km(0, w) >= (collide_half(0) + collide_half(w)) / KM, e_inside, pt_km(0, w), (collide_half(0) + collide_half(w)) / KM, 0);
    EXPECT(shipStateByte[w] == pilot, e_pilot, shipStateByte[w], pilot, 0);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s)) {
            rec = missionShipTable + missionShipTable_SIZE * (slotMissionShip[s] & 63);
            REQUIRE(slotMissionShip[s] < 64 && *(const uint16_t *)rec == gInstanceShipTypes[s]
                    && *(const uint16_t *)(rec + 2) == commAnimInfo[s], e_record, s, slotMissionShip[s], 0);
        }
    REQUIRE(pt_count(SIDE_AERA, -1) == 0, e_early, pt_count(SIDE_AERA, -1), 0, 0);
    REQUIRE(currentNavPoint == 0, e_nav, currentNavPoint, 0, 0);
}

/* M8.2, M8.3 */
static void t_nav_arrive(int16_t stay)
{
    static const char __far e_nav[] = "at nav 1 currentNavPoint is %d: enterNavPoint(1) when the player gets there";
    static const char __far e_ships[] = "at nav 1 there are %d Nicanders: its 2 come when the player gets there";
    static const char __far e_where[] = "slot %d came %d km from nav 1: its ships come at their nav point";
    static const char __far e_rec[] = "slot %d's slotMissionShip (%d) is no ship of nav 1's";
    static const char __far e_again[] = "nav 1's ships came again: %d Nicanders after 5 s there";
    int16_t s;
    const uint8_t *rec;
    pt_patrol();
    if (!pt_steps(SEC) || !pt_go(0, NAV1_Z))
        return;
    REQUIRE(currentNavPoint == 1, e_nav, currentNavPoint, 0, 0);
    REQUIRE(pt_count(SIDE_AERA, PT_ENEMY) == 2, e_ships, pt_count(SIDE_AERA, PT_ENEMY), 0, 0);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_AERA) {
            REQUIRE(pt_km_to(s, 0, 0, NAV1_Z) < 15000, e_where, s, pt_km_to(s, 0, 0, NAV1_Z), 0);
            rec = missionShipTable + missionShipTable_SIZE * (slotMissionShip[s] & 63);
            REQUIRE(slotMissionShip[s] < 64 && *(const uint16_t *)(rec + 8) == 1, e_rec, s, slotMissionShip[s], 0);
        }
    if (!stay)
        return;
    playerCanBeDamaged = 0;
    if (!pt_steps(5 * SEC))
        return;
    REQUIRE(pt_count(SIDE_AERA, PT_ENEMY) <= 2, e_again, pt_count(SIDE_AERA, PT_ENEMY), 0, 0);
}

static void t_nav1(void) { t_nav_arrive(0); }
static void t_nav_once(void) { t_nav_arrive(1); }

/* M8.4, M8.6: the patrol flown: each nav visited, its ships killed or not, then home and landed */
static void t_patrol(int16_t kill)
{
    static const char __far e_won[] = "victoryPoints is %d after a patrol that visited both navs and killed every "
        "enemy: nonzero (won)";
    static const char __far e_lost[] = "victoryPoints is %d after a patrol that left the enemies alive: 0 (lost)";
    static const char __far e_status[] = "missionStatus is %d before the landing";
    static const char __far e_early[] = "victoryPoints is %d a second into the patrol: 0 until it is won (logic_start "
        "scores over the launch's roll)";
    pt_patrol();
    playerCanBeDamaged = 0;
    if (!pt_steps(SEC))
        return;
    REQUIRE(victoryPoints == 0, e_early, victoryPoints, 0, 0);
    if (!pt_go(0, NAV1_Z) || (kill && !pt_kill_side(SIDE_AERA)))
        return;
    if (!pt_go(NAV2_X, NAV2_Z) || (kill && !pt_kill_side(SIDE_AERA)))
        return;
    if (!pt_go(0, 0))
        return;
    REQUIRE(missionStatus == 0, e_status, missionStatus, 0, 0);
    missionStatus = 1;                  /* (landed: the engine's C key, flight.c) */
    logic_end();
    if (kill)
        REQUIRE(victoryPoints != 0, e_won, victoryPoints, 0, 0);
    else
        REQUIRE(victoryPoints == 0, e_lost, victoryPoints, 0, 0);
}

static void t_won(void) { t_patrol(1); }
static void t_enemies_left(void) { t_patrol(0); }

/* M8.5 */
static void t_skipped(void)
{
    static const char __far e_lost[] = "victoryPoints is %d after landing without flying the patrol: 0 (lost; the "
        "launch's roll left 1 there for the logic to score over)";
    pt_patrol();
    if (!pt_steps(2 * SEC))
        return;
    missionStatus = 1;
    logic_end();
    REQUIRE(victoryPoints == 0, e_lost, victoryPoints, 0, 0);
}

/* M8.7 */
static void t_layer_spawn(void)
{
    static const char __far e_slot[] = "outerSpawnShipEntity(%d, 0) returned %d: a free slot";
    static const char __far e_where[] = "the spawned ship is %d km from nav 0: its nav point and its place off it";
    static const char __far e_far[] = "a ship at a nav point 70,000 km out and 7,500 off it spawned at x %d km: 77,500 "
        "(the record's place and the sum in 32 bits: CEPHID2's destroyer, the old game's map's, past 32,767 km)";
    uint16_t s;
    int16_t k;
    uint8_t *nav = navPointTable + navPointTable_SIZE * 1, *rec = missionShipTable + missionShipTable_SIZE * 1;
    pt_patrol();
    for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
        ship_clear(k);
    s = outerSpawnShipEntity(1, 0);
    REQUIRE(s >= 1 && s <= RV_MAX_SHIP_SLOT && slot_is_ship((int16_t)s), e_slot, 1, s, 0);
    REQUIRE(pt_km_to((int16_t)s, 0, 0, 0) < 5000, e_where, pt_km_to((int16_t)s, 0, 0, 0), 0, 0);
    ship_clear((int16_t)s);
    NAV_REC_KM(nav, 0) = 70000;
    MSHIP_REC_KM(rec, 0) = 7500;
    s = outerSpawnShipEntity(1, 1);
    REQUIRE(s >= 1 && s <= RV_MAX_SHIP_SLOT && slot_is_ship((int16_t)s), e_slot, 1, s, 0);
    REQUIRE(labs(TO_KM(gPositionVector[s].x) - 77500L) < 3000, e_far, TO_KM(gPositionVector[s].x), 0, 0);
}

/* M8.8 */
static void t_autopilot(void)
{
    static const char __far e_went[] = "the autopilot (logic_autopilot) returned %d and left the player %d km from nav "
        "1: with no enemy near it takes the player on to the next nav point";
    static const char __far e_nav[] = "after the autopilot to nav 1 currentNavPoint is %d";
    static const char __far e_stay[] = "the autopilot returned %d and moved the player %d km with %d enemies 4,000 km "
        "ahead: not with enemies about";
    static const char __far e_next[] = "with nav 1's ships gone the autopilot left the player %d km from nav 2 "
        "(returned %d)";
    RvVec3 p0;
    int16_t r, s, k = 0;
    pt_patrol();
    playerCanBeDamaged = 0;
    if (!pt_steps(SEC))
        return;
    r = logic_autopilot();
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(r && pt_km_to(0, 0, 0, NAV1_Z) <= PT_REACH_KM + 100, e_went, r, pt_km_to(0, 0, 0, NAV1_Z), 0);
    REQUIRE(currentNavPoint == 1, e_nav, currentNavPoint, 0, 0);
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)   /* (nav 1's enemies 4,000 km ahead of the player) */
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_AERA) {
            gPositionVector[s].x = gPositionVector[0].x + (int32_t)(k++ * 1500 - 750) * KM;
            gPositionVector[s].y = gPositionVector[0].y;
            gPositionVector[s].z = gPositionVector[0].z + 4000 * KM;
        }
    p0 = gPositionVector[0];
    r = logic_autopilot();
    REQUIRE(!r && pt_len((gPositionVector[0].x - p0.x) / 256, (gPositionVector[0].y - p0.y) / 256,
                         (gPositionVector[0].z - p0.z) / 256) < 100, e_stay, r,
            pt_len((gPositionVector[0].x - p0.x) / 256, (gPositionVector[0].y - p0.y) / 256,
                   (gPositionVector[0].z - p0.z) / 256), pt_count(SIDE_AERA, -1));
    if (!pt_kill_side(SIDE_AERA))
        return;
    r = logic_autopilot();
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(r && pt_km_to(0, NAV2_X, 0, NAV2_Z) <= PT_REACH_KM + 1000, e_next, pt_km_to(0, NAV2_X, 0, NAV2_Z), r, 0);
}

/* ======================================================================================================== */
/* M9: the HUD (logic_hud; the host harness's screen) */

#ifdef RV_HOST
static int16_t pt_drawn(int16_t x0, int16_t y0, int16_t x1, int16_t y1)   /* pixels drawn in a box of the screen */
{
    int16_t x, y, n = 0;
    for (y = y0 < 0 ? 0 : y0; y < y1 && y < HOST_H; y++)
        for (x = x0 < 0 ? 0 : x0; x < x1 && x < HOST_W; x++)
            if (host_screen[y * HOST_W + x])
                n++;
    return n;
}
#endif

/* M8.9 */
static void t_fighter(void)
{
    static const char __far e_first[] = "the first mission's fighter is type %d: the Schroedinger (0), the old campaign's Hornet";
    static const char __far e_player[] = "after camp_load_units the player's ship is type %d: the mission's fighter (%d)";
    static const char __far e_wing[] = "the wingman's record (behaviour 4) has type %d: the mission's fighter (%d), the "
        "player's own";
    static const char __far e_third[] = "the third mission's fighter is type %d: the Franklin (1), the old campaign's Scimitar";
    int16_t n, wing = -1;
    camp_mission(0, &g_tm);
    REQUIRE(g_tm.fighter == 0x00, e_first, g_tm.fighter, 0, 0);
    g_camp.player_ship = 0x03;          /* (the campaign's default, the Redeemer: the mission overrides it) */
    camp_load_units(&g_tm);
    REQUIRE(g_camp.player_ship == g_tm.fighter, e_player, g_camp.player_ship, g_tm.fighter, 0);
    for (n = 0; n < 64; n++)
        if (*(const uint16_t *)(missionShipTable + missionShipTable_SIZE * n + 6) == 4)
            wing = *(const uint16_t *)(missionShipTable + missionShipTable_SIZE * n);
    REQUIRE(wing == g_tm.fighter, e_wing, wing, g_tm.fighter, 0);
    camp_mission(2, &g_tm);
    REQUIRE(g_tm.fighter == 0x01, e_third, g_tm.fighter, 0, 0);
    g_camp.player_ship = PT_PLAYER;
    pt_mission();
}

/* the Ox's slot, -1 none */
static int16_t pt_ox(void)
{
    int16_t s;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && gInstanceShipTypes[s] == PT_OX)
            return s;
    return -1;
}

/* the patrol with an Ox to escort out (role=out at nav 0): nav 1 and 2 as pt_patrol's; the player invulnerable */
static int16_t pt_escort(void)
{
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_nav(2, NAV2_X, 0, NAV2_Z);
    pt_mship(RV_SHIP_WINGMAN, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    pt_mship(PT_OX, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_OUT;
    pt_mship(PT_ENEMY, SIDE_AERA, 1, 2, RV_PILOT_NONE);
    pt_mship(PT_WEAK, SIDE_AERA, 2, 1, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    return pt_ox();
}

/* M8.10 */
static void t_escort_out(void)
{
    static const char __far e_there[] = "no Ox (type 0x24) in the world after the launch (slot %d)";
    static const char __far e_moved[] = "15 s in, the Ox is %d km from nav 1: it was %d (an escort target out sets off "
        "for the next nav point at once)";
    static const char __far e_alive[] = "the Ox died on its way (slot %d)";
    static const char __far e_gone[] = "60 s in the Ox is still here, %d km from nav 1: it jumps out within 1,000 km";
    static const char __far e_won[] = "victoryPoints is %d after the patrol with the Ox jumped out: nonzero (its jump "
        "is not a loss)";
    int16_t ox = pt_escort();
    int32_t d0;
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    d0 = pt_km_to(ox, 0, 0, NAV1_Z);
    if (!pt_steps(15 * SEC))
        return;
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && shipAiState[ox] != 9, e_alive, ox, 0, 0);
    REQUIRE(pt_km_to(ox, 0, 0, NAV1_Z) < d0 - 300, e_moved, pt_km_to(ox, 0, 0, NAV1_Z), d0, 0);
    /* the player goes to nav 1 (the autopilot's way: the Ox along, 800 km behind), and the Ox flies the rest */
    if (!pt_go(0, NAV1_Z) || !pt_kill_side(SIDE_AERA))
        return;
    if (!pt_steps(45 * SEC))
        return;
    REQUIRE(!slot_is_ship(ox) || gInstanceShipTypes[ox] != PT_OX, e_gone, pt_km_to(ox, 0, 0, NAV1_Z), 0, 0);
    if (!pt_go(NAV2_X, NAV2_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(0, 0))
        return;
    missionStatus = 1;
    logic_end();
    REQUIRE(victoryPoints != 0, e_won, victoryPoints, 0, 0);
}

/* M5.21: one of the enemies goes for the escort target (the user, 2026-11-03: "the enemies don't seem particularly
 * interested in it even though at least one should go for it"): two Nicanders near the patrol's Ox, exactly one at it;
 * that one killed, the other takes it on; taunted, it turns on the player */
static void t_raid(void)
{
    static const char __far e_there[] = "no Ox, or no free slots for the Nicanders (%d, %d, %d)";
    static const char __far e_one[] = "5 s in, %d of the two Nicanders are at the Ox (targets %d, %d): exactly one";
    static const char __far e_next[] = "the raider killed, 5 s on the other Nicander's target is %d: the Ox (%d)";
    static const char __far e_taunt[] = "taunted, the raider's target is %d a step on: the player (0)";
    int16_t ox = pt_escort(), a = -1, b = -1, k, n, raider, other;
    for (k = 1; k <= RV_MAX_SHIP_SLOT; k++)
        if (!slot_is_ship(k)) {
            if (a < 0)
                a = k;
            else if (b < 0)
                b = k;
        }
    REQUIRE(ox > 0 && a > 0 && b > 0, e_there, ox, a, b);
    pt_ship(a, PT_ENEMY, SIDE_AERA, CTL_AI, 3000, 0, 8000, TURN / 2);
    pt_ship(b, PT_ENEMY, SIDE_AERA, CTL_AI, -3000, 0, 9000, TURN / 2);
    if (!pt_steps(5 * SEC))
        return;
    n = (rvTarget[a] == ox) + (rvTarget[b] == ox);
    REQUIRE(n == 1, e_one, n, rvTarget[a], rvTarget[b]);
    raider = rvTarget[a] == ox ? a : b;
    other = raider == a ? b : a;
    (void)pt_hit(0xFFFF, raider, 2000, 2, 0, 0);
    if (!pt_steps(5 * SEC))
        return;
    REQUIRE(rvTarget[other] == ox, e_next, rvTarget[other], ox, 0);
    (void)logic_taunt(other);
    if (!pt_step())
        return;
    REQUIRE(rvTarget[other] == 0, e_taunt, rvTarget[other], 0, 0);
}

/* M8.34: the autopilot brings the mission's wingman however far it is (the user, 2026-11-03: "sometimes my wingmate
 * doesn't auto with me and I just go it alone"): 25,000 km off, after the jump it is within 3,000 km of the player */
static void t_autopilot_far_wingman(void)
{
    static const char __far e_wing[] = "no wingman in the patrol (%d)";
    static const char __far e_went[] = "the autopilot returned %d (the player %d km from nav 1)";
    static const char __far e_along[] = "the wingman, 25,000 km off before the jump, is %d km from the player after it: "
        "it comes along";
    int16_t s, w = -1, r;
    pt_patrol();
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_TERRAN && logic_is_wingman(s))
            w = s;
    REQUIRE(w > 0, e_wing, w, 0, 0);
    pt_put(w, -25000, 0, 0, 0);
    if (!pt_steps(SEC))
        return;
    r = logic_autopilot();
    REQUIRE(r && pt_km_to(0, 0, 0, NAV1_Z) <= PT_REACH_KM + 100, e_went, r, pt_km_to(0, 0, 0, NAV1_Z), 0);
    REQUIRE(pt_km_to(w, gPositionVector[0].x / 256, gPositionVector[0].y / 256, gPositionVector[0].z / 256) < 3000,
            e_along, pt_km_to(w, gPositionVector[0].x / 256, gPositionVector[0].y / 256, gPositionVector[0].z / 256), 0, 0);
}

/* M8.35: the autopilot's flyby closes the wing up on the player (the user, 2026-11-03: "make the wingman and any escort
 * ships automatically form up with the leader and align the velocities so that they look like they're flying in a
 * cluster"): the patrol's wingman 6,000 km off, logic_form_up as the flyby begins; two seconds on it is at its place,
 * 350 km off the right wing and 250 back, nose and velocity the player's, and the jump brings it there */
static void t_autopilot_formup(void)
{
    static const char __far e_wing[] = "no wingman in the patrol (%d)";
    static const char __far e_place[] = "2 s into the flyby the wingman is %d km right, %d up, %d ahead of the player: "
        "its place is 350, 0, -250";
    static const char __far e_with[] = "2 s into the flyby the wingman's nose is %d (cos x 256) to the player's and its "
        "velocity %d km a step off the player's: the same";
    static const char __far e_went[] = "the autopilot returned %d after the flyby";
    static const char __far e_kept[] = "after the jump the wingman is %d km right, %d up, %d ahead of the player: still "
        "its place, 350, 0, -250";
    int16_t s, w = -1, r;
    RvVec3 b;
    pt_patrol();
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_TERRAN && logic_is_wingman(s))
            w = s;
    REQUIRE(w > 0, e_wing, w, 0, 0);
    pt_put(w, 6000, 0, -3000, TURN / 4);
    if (!pt_steps(SEC))
        return;
    logic_form_up(3 * SEC + 1);
    if (!pt_steps(2 * SEC))
        return;
    ship_rel(0, w, &b);
    REQUIRE(labs(b.x - 350) < 20 && labs(b.y) < 20 && labs(b.z + 250) < 20, e_place, (int)b.x, (int)b.y, (int)b.z);
    REQUIRE(vec_dot(&gOrientationFrontVector[w], &gOrientationFrontVector[0]) / 256 > 250
            && labs(gVelocityVector[w].x - gVelocityVector[0].x) + labs(gVelocityVector[w].y - gVelocityVector[0].y)
               + labs(gVelocityVector[w].z - gVelocityVector[0].z) < 256,
            e_with, (int)(vec_dot(&gOrientationFrontVector[w], &gOrientationFrontVector[0]) / 256),
            (int)((labs(gVelocityVector[w].x - gVelocityVector[0].x) + labs(gVelocityVector[w].z - gVelocityVector[0].z)) / 256), 0);
    if (!pt_steps(SEC))
        return;
    r = logic_autopilot();
    REQUIRE(r, e_went, r, 0, 0);
    ship_rel(0, w, &b);
    REQUIRE(labs(b.x - 350) < 60 && labs(b.y) < 60 && labs(b.z + 250) < 60, e_kept, (int)b.x, (int)b.y, (int)b.z);
}

/* M8.37: a human's wingman is put in its formation place as the flyby begins, flying the player's way, its throttle at
 * the player's speed (its own machine flies it: the frame that begins the flyby there takes it to the place; the user,
 * 2026-11-03: "player 2 doesn't get the same forcing function for auto somehow and can't see the leader") */
static void t_autopilot_human(void)
{
    static const char __far e_wing[] = "no wingman in the patrol (%d)";
    static const char __far e_place[] = "the flyby begun, the human's wingman is %d km right, %d up, %d ahead of the player: "
        "its place is 350, 0, -250";
    static const char __far e_with[] = "the flyby begun, the human's wingman's nose is %d (cos x 256) to the player's and its "
        "throttle %d kps, the player %d: the same";
    int16_t s, w = -1;
    RvVec3 b;
    pt_patrol();
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (slot_is_ship(s) && commAnimInfo[s] == SIDE_TERRAN && logic_is_wingman(s))
            w = s;
    REQUIRE(w > 0, e_wing, w, 0, 0);
    if (!pt_steps(SEC))
        return;
    rvController[w] = CTL_PUPPET;
    rvFlags[w] |= RV_FLAG_HUMAN;
    pt_put(w, 6000, 0, -3000, TURN / 4);
    logic_form_up(3 * SEC + 1);
    ship_rel(0, w, &b);
    REQUIRE(labs(b.x - 350) < 20 && labs(b.y) < 20 && labs(b.z + 250) < 20, e_place, (int)b.x, (int)b.y, (int)b.z);
    REQUIRE(vec_dot(&gOrientationFrontVector[w], &gOrientationFrontVector[0]) / 256 > 250 && setSpeed[w] == speedLo[0],
            e_with, (int)(vec_dot(&gOrientationFrontVector[w], &gOrientationFrontVector[0]) / 256), (int)setSpeed[w],
            (int)speedLo[0]);
}

/* M10.10: told to form up, the wingman flies the player's way (the user, 2026-11-03: "when I say form up the wingmate
 * faces me instead of being parallel"): 2,500 km ahead of its place facing back, with the player still and then under
 * way, in 20 s its nose is within 25 degrees of the player's way */
static void t_form_parallel(void)
{
    static const char __far e_still[] = "the player still, the wingman put ahead of its place facing back: 20 s on its "
        "nose is %d (cos x 256) to the player's way: within 25 degrees (over 232)";
    static const char __far e_moving[] = "the player at 250 kps, the wingman put ahead facing back: 20 s on its nose is %d "
        "(cos x 256) to the player's way, %ld km off: within 25 degrees (over 232), within 4,000 km";
    int16_t i;
    int32_t c;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_PLAYER, SIDE_TERRAN, CTL_AI, 800, 0, 0, 0);
    (void)logic_order(1, ORDER_FORMATION);
    if (!pt_steps(2 * SEC))             /* (its place taken: 800 km to the player's right) */
        return;
    pt_put(1, 800, 0, 2500, TURN / 2);
    pt_speed(1, 0);
    if (!pt_steps(20 * SEC))
        return;
    c = vec_dot(&gOrientationFrontVector[1], &gOrientationFrontVector[0]) / 256;
    REQUIRE(c > 232, e_still, c, 0, 0);
    pt_put(1, 1500, 0, 2000, TURN / 2);
    pt_speed(1, 0);
    for (i = 0; i < 20 * SEC; i++) {
        pt_speed(0, 250);
        if (!pt_step())
            return;
    }
    c = vec_dot(&gOrientationFrontVector[1], &gOrientationFrontVector[0]) / 256;
    REQUIRE(c > 232 && pt_km(0, 1) < 4000, e_moving, c, pt_km(0, 1), 0);
    (void)logic_order(1, ORDER_FREE);
}

/* M8.33: an escort target out with jump=2 goes through nav 1 with the player and jumps only short of nav 2 (the user,
 * 2026-11-03: "the ox should jump at nav 2 not nav 1"): it holds at nav 1 while the enemies there are about, goes on
 * when they are gone, and is out of the world, not killed, by nav 2 */
static void t_escort_jump(void)
{
    static const char __far e_there[] = "no Ox after the launch (slot %d)";
    static const char __far e_held[] = "30 s at nav 1 with its enemies about, the Ox is %d km from nav 1 (alive %d): it holds "
        "there, not jumped (jump=2)";
    static const char __far e_on[] = "the enemies at nav 1 gone, 30 s on the Ox is %d km from nav 2: it was %d; it goes on";
    static const char __far e_out[] = "60 s by nav 2 the Ox is still here, %d km from nav 2: it jumps out within 1,000 km";
    static const char __far e_won[] = "victoryPoints is %d with the Ox jumped out at nav 2: nonzero";
    int16_t ox, i;
    int32_t d2;
    ox = pt_escort();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    for (i = 0; i < g_tm.nship; i++)    /* (the Ox's ship line: jump=2) */
        if (g_tm.ship[i].type == PT_OX)
            g_tm.ship[i].jump = 2;
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    ox = pt_ox();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    if (!pt_go(0, NAV1_Z - 4500))       /* (the autopilot's arrival: the Ox along, the wave at nav 1 coming in) */
        return;
    for (i = 0; i < 30 * SEC; i++)
        if (!pt_step())
            return;
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && pt_km_to(ox, 0, 0, NAV1_Z) < 6000, e_held,
            pt_km_to(ox, 0, 0, NAV1_Z), slot_is_ship(ox), 0);
    if (!pt_kill_side(SIDE_AERA))
        return;
    d2 = pt_km_to(ox, NAV2_X, 0, NAV2_Z);
    if (!pt_steps(30 * SEC))            /* (from the hold: turning 4 degrees a second, 5 kps a second faster) */
        return;
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && pt_km_to(ox, NAV2_X, 0, NAV2_Z) < d2 - 500, e_on,
            pt_km_to(ox, NAV2_X, 0, NAV2_Z), d2, 0);
    if (!pt_go(NAV2_X, NAV2_Z - 4500))
        return;
    if (!pt_steps(60 * SEC))
        return;
    REQUIRE(!slot_is_ship(ox) || gInstanceShipTypes[ox] != PT_OX, e_out, pt_km_to(ox, NAV2_X, 0, NAV2_Z), 0, 0);
    if (!pt_kill_side(SIDE_AERA) || !pt_go(0, 0))
        return;
    missionStatus = 1;
    logic_end();
    REQUIRE(victoryPoints != 0, e_won, victoryPoints, 0, 0);
}

/* M8.36: an escort target out bears straight for its nav point and jumps only on coming to it (the user, 2026-11-04:
 * "Ideally it would bear directly for the nav point no matter what and when it arrived (slowly) it would jump"); the
 * autopilot leaves it on its way rather than take it where it is not going (the user, 2026-11-03: the Ox "just hung
 * around and started flying in a random direction") */
static void t_escort_clear(void)
{
    static const char __far e_there[] = "no Ox after the launch (slot %d)";
    static const char __far e_on[] = "nav 1 flown, 5 s on the Ox is gone (alive %d): it goes on to nav 2 (jump=2)";
    static const char __far e_left[] = "the autopilot home (returned %d) with the Ox on its way to nav 2 moved the Ox %d "
        "km in a second: it goes its own way, at 150 kps";
    static const char __far e_early[] = "nav 2 reached and its enemy gone, the Ox %d km from nav 2 jumped (alive %d): it "
        "jumps only there";
    static const char __far e_out[] = "put 3,000 km short of nav 2 facing away, 120 s on the Ox is still here, %d km from "
        "nav 2: it turns for it and jumps there";
    static const char __far e_won[] = "victoryPoints is %d with the Ox jumped out: nonzero";
    int16_t ox, i, r;
    RvVec3 at;
    ox = pt_escort();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    for (i = 0; i < g_tm.nship; i++)    /* (the Ox's ship line: jump=2) */
        if (g_tm.ship[i].type == PT_OX)
            g_tm.ship[i].jump = 2;
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    ox = pt_ox();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    if (!pt_go(0, NAV1_Z - 4500) || !pt_kill_side(SIDE_AERA) || !pt_steps(5 * SEC))
        return;
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX, e_on, slot_is_ship(ox), 0, 0);
    for (i = 0; i < 8 && g_sys.nav != 0; i++)   /* (home picked: the N key) */
        logic_nav_next();
    pt_put(0, 0, 0, NAV1_Z + 6000, TURN / 2);   /* (out past the launch's reach, 20,000 km, for the autopilot to go) */
    at = gPositionVector[ox];
    r = logic_autopilot();
    if (!pt_steps(SEC))
        return;
    REQUIRE(r == 1 && slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX
            && labs(TO_KM(gPositionVector[ox].x - at.x)) + labs(TO_KM(gPositionVector[ox].z - at.z)) < 500, e_left, r,
            labs(TO_KM(gPositionVector[ox].x - at.x)) + labs(TO_KM(gPositionVector[ox].z - at.z)), 0);
    gPositionVector[0].x = NAV2_X * KM;   /* (the player alone at nav 2, the Ox far behind: its wave comes) */
    gPositionVector[0].y = 0;
    gPositionVector[0].z = (NAV2_Z - 4500) * KM;
    setSpeed[0] = speedLo[0] = 0;       /* (and stays there: an Ox with no pilot within 20,000 km waits for one) */
    if (!pt_steps(2 * SEC) || !pt_kill_side(SIDE_AERA) || !pt_steps(5 * SEC))
        return;
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX, e_early, pt_km_to(ox, NAV2_X, 0, NAV2_Z),
            slot_is_ship(ox), 0);
    pt_put(ox, NAV2_X, 0, NAV2_Z - 3000, TURN / 2);   /* (3,000 km short, facing away: it turns and comes in) */
    for (i = 0; i < 120 * SEC && slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX; i++)
        if (!pt_step())
            return;
    REQUIRE(!slot_is_ship(ox) || gInstanceShipTypes[ox] != PT_OX, e_out, pt_km_to(ox, NAV2_X, 0, NAV2_Z), 0, 0);
    if (!pt_go(0, 0))
        return;
    missionStatus = 1;
    logic_end();
    REQUIRE(victoryPoints != 0, e_won, victoryPoints, 0, 0);
}

/* M8.39: the old game's own map (the user, 2026-11-04: "Yes lets do the full scale"; "I went to nav 2 exactly ... and
 * did not see the destroyer"): a nav point 60,000 km out, its reach 15,000: the autopilot stops at the reach, and a
 * destroyer 8,000 km beyond the point, 23,000 off, is in the scene and a target as the player arrives (the small map's
 * scene had held one 8,100 km off the stop); the carrier, 47,000 km behind, is held while a waypoint is still to fly,
 * and is in the scene again, however far, once the way is home */
static void t_full_map(void)
{
    static const char __far e_home[] = "no home carrier at the launch (%d)";
    static const char __far e_stop[] = "the autopilot returned %d and left the player %d km from nav 1: it stops at the "
        "point's reach, 15,000 km";
    static const char __far e_here[] = "on arrival the destroyer (slot %d) %d km off is not in the scene (entity type %d): "
        "the ships of the point the player came to are, however far";
    static const char __far e_held[] = "the carrier %d km behind is in the scene (entity type %d), a waypoint still to fly: "
        "past 16,000 km a capital ship of a point left is held";
    static const char __far e_back[] = "every waypoint flown, the way home: the carrier %d km off is not in the scene "
        "(entity type %d)";
    int16_t home = -1, dd = -1, s, r;
    pt_mission();
    pt_nav(1, 0, 0, 60000);
    g_tm.nav[g_tm.nnav - 1].reach = 15000;
    pt_nav(2, 20000, 0, 60000);
    g_tm.nav[g_tm.nnav - 1].reach = 15000;
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    pt_mship(0x28, SIDE_AERA, 1, 1, RV_PILOT_NONE);   /* (an Agasicles, a destroyer: CEPHID2's) */
    g_tm.ship[g_tm.nship - 1].z = 8000;
    pt_launch(PT_PLAYER);
    logic_scene_off = 0;
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (logic_is_home(s))
            home = s;
    REQUIRE(home > 0, e_home, home, 0, 0);
    r = logic_autopilot();
    REQUIRE(r == 1 && labs(pt_km_to(0, 0, 0, 60000) - 15000) < 200, e_stop, r, pt_km_to(0, 0, 0, 60000), 0);
    if (!pt_step())
        return;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (gInstanceShipTypes[s] == 0x28 && maybeEntityType[s] != ET_NONE)
            dd = s;
    REQUIRE(dd > 0 && slot_is_ship(dd) && maybeEntityType[dd] != ET_OFFSCENE, e_here, dd, dd > 0 ? pt_km(0, dd) : -1,
            dd > 0 ? maybeEntityType[dd] : -1);
    REQUIRE(maybeEntityType[home] == ET_OFFSCENE, e_held, pt_km(0, home), maybeEntityType[home], 0);
    pt_put(0, 20000, 0, 60000, 0);      /* (nav 2 come to: every waypoint flown) */
    if (!pt_steps(2))
        return;
    REQUIRE(slot_is_ship(home), e_back, pt_km(0, home), maybeEntityType[home], 0);
}

/* the field of the test mission a nav point lies in, -1 none (logic.c nav_field) */
static int16_t pt_nav_field(const RvNav __far *n)
{
    int16_t f;
    for (f = 0; f < (int16_t)g_tm.nfield; f++)
        if (pt_len(n->x - g_tm.field[f].x, n->y - g_tm.field[f].y, n->z - g_tm.field[f].z) < g_tm.field[f].radius)
            return f;
    return -1;
}

/* M8.40: the whole campaign on the old game's map: each of the missions launched, its enemies killed and the autopilot
 * pressed (the player put at the next waypoint when it may not go: a field at hand) until every waypoint is reached,
 * the world sound throughout; with the autopilot gone to a waypoint, the player is at its reach, or for a waypoint in a
 * field inside the field (M8.55).  (What a mission's numbers, past 32,767 km, would break on DOS's 16-bit int, the
 * hashes show) */
static void t_campaign_way(void)
{
    static const char __far e_way[] = "mission %d: waypoint %d not reached after %d legs";
    static const char __far e_stop[] = "mission %d: the autopilot to a waypoint left the player %d km from it: its reach "
        "is %d";
    static const char __far e_infield[] = "mission %d: the autopilot to a waypoint in a field left the player %d km from "
        "the field's middle: its radius is %d";
    static const char __far e_went[] = "the autopilot went %d times (the player put at the next waypoint %d) in %d missions: "
        "it should take most legs";
    int16_t i, k, leg, s, next, went = 0, put = 0;
    uint16_t m;
    for (m = 0; m < g_nmissions; m++) {
        camp_mission(m, &g_tm);
        pt_launch(g_tm.fighter != 0xFF ? g_tm.fighter : PT_PLAYER);
        playerCanBeDamaged = 0;
        for (leg = 0; leg < 16; leg++) {
            for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)   /* (the enemies in the world killed: they end, as killed ones do) */
                if (slot_is_ship(s) && commAnimInfo[s] != commAnimInfo[0] && shipAiState[s] != 9)
                    shipAiState[s] = 9;
            if (!pt_steps(5 * SEC))
                return;
            for (next = -1, k = 0; k < g_tm.nnav && next < 0; k++)
                if (!(g_tm.nav[k].flags & 1) && !logic_nav_seen(g_tm.nav[k].id))
                    next = k;
            if (next < 0)
                break;
            for (i = 0; i < 8 && g_sys.nav != g_tm.nav[next].id; i++)
                logic_nav_next();
            if (logic_autopilot()) {
                int32_t d = pt_km_to(0, g_tm.nav[next].x, g_tm.nav[next].y, g_tm.nav[next].z);
                int16_t f = pt_nav_field(&g_tm.nav[next]);
                if (logic_nav_seen(g_tm.nav[next].id) && f >= 0) {   /* (into the waypoint's own field) */
                    d = pt_km_to(0, g_tm.field[f].x, g_tm.field[f].y, g_tm.field[f].z);
                    REQUIRE(d < g_tm.field[f].radius, e_infield, m, d, g_tm.field[f].radius);
                } else if (logic_nav_seen(g_tm.nav[next].id))   /* (gone all the way: no field nor point stopped it) */
                    REQUIRE(d > (int32_t)g_tm.nav[next].reach * 9 / 10 && d < (int32_t)g_tm.nav[next].reach * 6 / 5,
                            e_stop, m, d, g_tm.nav[next].reach);
                went++;
            } else {
                pt_put(0, g_tm.nav[next].x, g_tm.nav[next].y, g_tm.nav[next].z, 0);
                put++;
            }
            if (!pt_steps(SEC))
                return;
        }
        for (k = 0; k < g_tm.nnav; k++)
            if (!(g_tm.nav[k].flags & 1))
                REQUIRE(logic_nav_seen(g_tm.nav[k].id), e_way, m, g_tm.nav[k].id, leg);
    }
    EXPECT(went >= 70, e_went, went, put, g_nmissions);   /* (80 legs flown by it, 14 put, on 2026-11-04) */
    g_camp.player_ship = PT_PLAYER;
    pt_mission();
}

/* a mission of the campaign launched by its id, as the campaign has it (its fighter) */
static void pt_launch_named(const char *id)
{
    uint16_t i;
    for (i = 0; i < g_nmissions; i++) {
        camp_mission(i, &g_tm);
        if (!_fstrncmp(g_tm.id, id, sizeof g_tm.id))
            break;
    }
    pt_launch(g_tm.fighter != 0xFF ? g_tm.fighter : PT_PLAYER);
}

/* M8.41: ENIGMA2's Ox (role=out jump=2, the old game's own map), the player lingering at the launch for two and a half
 * minutes (a pilot waiting for another to join; the user, 2026-11-04: "this time I arrived at nav 1 without the ox
 * there--just the enemies and [my wingman]"): the Ox has gone on alone, as the old game's does, and the autopilot to
 * nav 1, its next waypoint, brings it along however far it got */
static void t_escort_waits(void)
{
    static const char __far e_there[] = "no Ox after ENIGMA2's launch (slot %d)";
    static const char __far e_gone[] = "150 s with the player still at the launch, the Ox is %d km off (alive %d): it goes "
        "on alone, past 20,000 km";
    static const char __far e_went[] = "the autopilot returned %d, the player %d km from nav 1";
    static const char __far e_along[] = "after the autopilot to nav 1 the Ox is %d km from the player (alive %d): it comes "
        "along";
    int16_t ox, r;
    pt_launch_named("ENIGMA2");
    playerCanBeDamaged = 0;
    ox = pt_ox();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    setSpeed[0] = speedLo[0] = 0;       /* (the player holds still by the carrier) */
    if (!pt_steps(150 * SEC))
        return;
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && pt_km(0, ox) > 20000, e_gone, pt_km(0, ox),
            slot_is_ship(ox), 0);
    r = logic_autopilot();
    if (!pt_steps(SEC))
        return;
    REQUIRE(r == 1, e_went, r, pt_km_to(0, g_tm.nav[0].x, g_tm.nav[0].y, g_tm.nav[0].z), 0);
    REQUIRE(slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && pt_km(0, ox) < 5000, e_along, pt_km(0, ox),
            slot_is_ship(ox), 0);
    g_camp.player_ship = PT_PLAYER;
    pt_mission();
}

/* M8.42: ENIGMA2 flown by the autopilot (the user, 2026-11-04: "if the player arrives from autopilot on any nav point
 * on the way to the jump point then the ox should follow.... especially the last one...and should have enough time to
 * fly to the actual outbound jump point and leave"): each wave killed, the Ox comes along to nav 1 and to nav 2, its
 * jump's; then the player goes straight home by the autopilot, and the Ox still flies in to its jump point and goes */
static void t_escort_jump_home(void)
{
    static const char __far e_there[] = "no Ox after ENIGMA2's launch (slot %d)";
    static const char __far e_with[] = "after the autopilot to nav %d the Ox is %d km from the player (alive %d): it comes along";
    static const char __far e_home[] = "the autopilot home returned %d (nav 2 reached %d)";
    static const char __far e_gone[] = "the player gone home, 150 s on the Ox is still here, %d km from nav 2: it flies in and "
        "jumps whoever is with it";
    static const char __far e_won[] = "the Ox jumped and the waves killed: logic_escort_lost says %d (0)";
    int16_t ox, r, i, leg;
    pt_launch_named("ENIGMA2");
    playerCanBeDamaged = 0;
    ox = pt_ox();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    for (leg = 1; leg <= 2; leg++) {
        r = logic_autopilot();
        if (!pt_steps(SEC))
            return;
        REQUIRE(r == 1 && slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && pt_km(0, ox) < 5000, e_with, leg,
                pt_km(0, ox), slot_is_ship(ox));
        if (!pt_kill_side(SIDE_AERA) || !pt_steps(3 * SEC))
            return;
    }
    for (i = 0; i < 8 && g_sys.nav != 0; i++)
        logic_nav_next();
    r = logic_autopilot();
    REQUIRE(r == 1, e_home, r, logic_nav_seen(2), 0);
    for (i = 0; i < 150 * SEC && slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX; i++)
        if (!pt_step())
            return;
    REQUIRE(!slot_is_ship(ox) || gInstanceShipTypes[ox] != PT_OX, e_gone,
            pt_km_to(ox, g_tm.nav[1].x, g_tm.nav[1].y, g_tm.nav[1].z), 0, 0);
    REQUIRE(logic_escort_lost() == 0, e_won, logic_escort_lost(), 0, 0);
    g_camp.player_ship = PT_PLAYER;
    pt_mission();
}

/* M8.43: a pick past an escort's next waypoint, the old game's way (the user, 2026-11-04, of its autopilot: "Pick
 * anywhere, Ox left behind"): ENIGMA2 at the launch, nav 2 picked: the autopilot goes to nav 2, the Ox is left
 * behind, and flies its way on alone (past nav 1, which the player has gone beyond, to nav 2) */
/* M8.44: a capital ship's turrets as the old game's (ship_think, capital_shot, flak_burst): an Agasicles with a Franklin
 * alone held 2,250 km behind it, two minutes still, then two at 200 kps.  The old game's Ralari, measured the same way
 * (tests/wc1/README.md "Capital ships": McAuliffe 2, its escorts and the wingman gone, a Scimitar held 2,500 km behind
 * it): still, 950 points of flak in 4,176 of its frames, the most of the hits 2 to 4 points (409 in the test's 1,800);
 * at 200 kps 132 (57).  The Franklin is the smaller ship (the bursts are counted from the hull): ours still within half
 * the Franklin's 275 and twice the Scimitar's 409, a few bolts; moving, at most a third of that and 150 */
static int32_t pt_capital_flak(int16_t kps, int16_t *bolts, int16_t *flak, int16_t bearing, int16_t secs)
{
    PtDamage whole;
    int32_t total = 0;
    int16_t i;
    *bolts = *flak = 0;
    pt_alone(0x01);                     /* (the Franklin: the old game's Scimitar) */
    pt_ship(1, 0x28, SIDE_AERA, CTL_AI, 0, 0, 2250, 0);
    if (!slot_is_ship(1) || gInstanceShipTypes[1] != 0x28)
        return -1;
    pt_damage_get(0, &whole);
    for (i = 0; i < secs * SEC; i++) {
        const RvVec3 *F = &gOrientationFrontVector[1];
        const RvVec3 *W = bearing == 1 ? &gOrientationRightVector[1] : F;   /* (behind it, abeam, or ahead) */
        int32_t h0, d, sign = bearing ? 1 : -1;
        gPositionVector[0].x = gPositionVector[1].x + sign * W->x * (2250 * KM / 256);
        gPositionVector[0].y = gPositionVector[1].y + sign * W->y * (2250 * KM / 256);
        gPositionVector[0].z = gPositionVector[1].z + sign * W->z * (2250 * KM / 256);
        gOrientationFrontVector[0] = *F;   /* (its way, at kps: a target that moves) */
        gVelocityVector[0].x = F->x * KPS_STEP(kps) / 256;
        gVelocityVector[0].y = F->y * KPS_STEP(kps) / 256;
        gVelocityVector[0].z = F->z * KPS_STEP(kps) / 256;
        h0 = pt_health(0);
        if (!pt_step())
            return -2;
        d = h0 - pt_health(0);
        if (d > 0) {
            total += d;
            if (d > 15)
                (*bolts)++;
            else
                (*flak)++;
        }
        pt_damage_set(0, &whole);       /* (held whole: each hit counted as the old game's D lines count it) */
    }
    return total;
}

static void t_capital_flak(void)
{
    static const char __far e_ship[] = "no Agasicles in slot 1 (%d)";
    static const char __far e_still[] = "the Agasicles did %d points to a still player held 2,250 km behind it in 2 minutes (%d "
        "bolts, %d bursts): the old game's Ralari did some 409 to a Scimitar (140 to 820 expected)";
    static const char __far e_bolts[] = "%d of its hits on a still player were bolts (over 15 points), %d flak: the old game's "
        "were one among 259";
    static const char __far e_move[] = "the Agasicles did %d points to a player at 200 kps in 2 minutes (%d bursts), %d still: "
        "the old game's Ralari 57 against 409 (at most 150, and a third of the still)";
    int16_t bolts, flak, bolts2, flak2;
    int32_t still = pt_capital_flak(0, &bolts, &flak, 0, 120), moving;
    if (still == -2)
        return;
    REQUIRE(still >= 0, e_ship, (int16_t)still, 0, 0);
    REQUIRE(still >= 140 && still <= 820, e_still, still, bolts, flak);
    REQUIRE(bolts <= 6, e_bolts, bolts, flak, 0);
    moving = pt_capital_flak(200, &bolts2, &flak2, 0, 120);
    if (moving < 0)
        return;
    REQUIRE(moving <= 150 && moving * 3 <= still, e_move, moving, flak2, still);
}

/* M8.45: a capital ship's hull as the old game's (hull_take): a count, a hit past the armour an eighth of itself on the
 * Aera's side, at least one: an Agasicles (the Ralari's 90) bare to its hull lives through 89 laser hits of 14 and dies
 * at the 90th; a hit of 40 is five of them */
static void t_capital_hull(void)
{
    static const char __far e_early[] = "the Agasicles died at laser hit %d past its armour: the old game's Ralari takes 90";
    static const char __far e_late[] = "the Agasicles lived through %d laser hits past its armour: the old game's Ralari dies at 90";
    static const char __far e_big[] = "a hit of 40 past its armour took %d of its hull's points: five of the old game's counts (70)";
    int16_t i;
    uint16_t before;
    pt_alone(PT_PLAYER);
    pt_ship(1, 0x28, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, 0);
    for (i = 1; i <= 90; i++) {
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
        ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
        (void)do_damage(0, 1, 14, 0);
        if (i < 90)
            REQUIRE(shipAiState[1] != 9, e_early, i, 0, 0);
    }
    REQUIRE(shipAiState[1] == 9, e_late, 90, 0, 0);
    pt_ship(1, 0x28, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, 0);
    SHIELD(1, 0) = SHIELD(1, 1) = 0;
    ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
    before = damagePoints[1];
    (void)do_damage(0, 1, 40, 0);
    REQUIRE(damagePoints[1] - before == 70, e_big, damagePoints[1] - before, 0, 0);
}

/* an unnamed Nicander in slot 1, bare to its hull, hit with 14 from behind (its rear) or from the front until it dies,
 * at most `most` hits: how many it took; *by_count: the death came of the hull's count (the fatal hit took at most its
 * two points' two counts), not outright */
static int16_t pt_roulette_kill(int16_t behind, int16_t most, int16_t *by_count)
{
    const RvShipType __far *t;
    uint16_t step, before;
    int16_t i;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);
    t = slot_type(1);
    if (!t || !t->hull_hits)
        return -1;
    step = (uint16_t)(((uint32_t)t->hull + t->hull_hits) / (t->hull_hits + 1));
    *by_count = 0;
    for (i = 1; i <= most; i++) {
        const RvVec3 *F = &gOrientationFrontVector[1];
        RvVec3 at;
        int32_t off = (behind ? -200 : 200) * KM / 256;
        at = vec3(gPositionVector[1].x + F->x * off, gPositionVector[1].y + F->y * off, gPositionVector[1].z + F->z * off);
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
        ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
        before = damagePoints[1];
        (void)do_damage(0, 1, 14, RV_WHERE(&at));
        if (shipAiState[1] == 9) {
            *by_count = t->hull - before <= 2 * step;
            return i;
        }
    }
    return most;
}

/* M8.49: an unnamed fighter's hull as the old game's roulette (fighter_roulette, read in its code 2026-11-05): each
 * point past the armour (a hit of 14: two) falls on one of nine systems; from behind one kills outright (the old
 * game's hits from behind reach the engines: a Dralthi dies to a few laser hits past its rear armour), from the front
 * only the hull's count (a Nicander dies past seven, its type's hull_hits) among a gun lost, the gun damage and the
 * engine flag: a head-on kill takes some twenty laser hits past the armour.  Twelve of each: from behind at most 8 a
 * kill on average, from the front at least 10 and over twice the other; a front kill came of the hull's count (the
 * fatal hit took at most two), and a hull count is a step of (hull + 7) / 8 points */
static void t_fighter_roulette(void)
{
    static const char __far e_type[] = "the Nicander has no hull_hits in ships.csv (%d)";
    static const char __far e_behind[] = "from behind twelve Nicanders bare to the hull died after %d laser hits of 14 in all: 8 a "
        "kill at most (the old game's kill outright, one point in five)";
    static const char __far e_front[] = "head on twelve Nicanders bare to the hull died after %d laser hits of 14 in all, %d from "
        "behind: 10 a kill at least, and over twice the kills from behind (the hull's count, one point in five)";
    static const char __far e_count[] = "a front kill came outright, not of the hull's count: %d of 12 kills took at most two counts in the fatal hit";
    static const char __far e_step[] = "a hull count took %d points: (hull %d + 7) / 8 = 14";
    int16_t i, behind = 0, front = 0, full = 0, got, hf;
    pt_alone(PT_PLAYER);
    for (i = 0; i < 12; i++) {
        got = pt_roulette_kill(1, 200, &hf);
        REQUIRE(got >= 0, e_type, got, 0, 0);
        behind = (int16_t)(behind + got);
    }
    for (i = 0; i < 12; i++) {
        got = pt_roulette_kill(0, 200, &hf);
        front = (int16_t)(front + got);
        full = (int16_t)(full + hf);
    }
    REQUIRE(behind <= 8 * 12, e_behind, behind, 0, 0);
    REQUIRE(front >= 10 * 12 && front > 2 * behind, e_front, front, behind, 0);
    REQUIRE(full == 12, e_count, full, 0, 0);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);
    SHIELD(1, 0) = SHIELD(1, 1) = 0;
    ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
    for (i = 0; i < 200 && damagePoints[1] == 0 && shipAiState[1] != 9; i++) {
        RvVec3 at = vec3(gPositionVector[1].x, gPositionVector[1].y, gPositionVector[1].z + 200 * KM);
        (void)do_damage(0, 1, 14, RV_WHERE(&at));
    }
    REQUIRE(shipAiState[1] == 9 || damagePoints[1] == 14, e_step, damagePoints[1], slot_type(1)->hull, 0);
}

/* M8.50: the old game's attack orders (missions.txt prey=KEY, its mission records' byte 0x29: at Enyo 2 its Salthi went
 * for the Drayman with the player nearer; prey_of): a Nicander ordered at the Ox, the Ox 40,000 km from the player and
 * the player 12,000 km from it, goes for the Ox; hurt under its profile's share of health (a fighter's 55), it turns on
 * the nearer player */
static void t_prey(void)
{
    static const char __far e_ships[] = "no Nicander (slot %d) or Ox (slot %d) after the launch";
    static const char __far e_prey[] = "the Nicander ordered at the Ox targets slot %d (the Ox is %d, the player 0, nearer)";
    static const char __far e_hurt[] = "its armour gone (health under 55%%) the Nicander still targets slot %d: its orders "
        "lapse (the player, nearer, is %d km off; the Ox %d)";
    int16_t nic = -1, ox = -1, s;
    pt_mission();
    pt_mship(PT_OX, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].z = 40000;
    pt_mship(PT_ENEMY, SIDE_AERA, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].z = 12000;
    g_tm.ship[g_tm.nship - 1].prey = PT_OX + 1;
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
        if (slot_is_ship(s) && gInstanceShipTypes[s] == PT_ENEMY)
            nic = s;
        if (slot_is_ship(s) && gInstanceShipTypes[s] == PT_OX)
            ox = s;
    }
    REQUIRE(nic > 0 && ox > 0, e_ships, nic, ox, 0);
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(rvTarget[nic] == ox, e_prey, rvTarget[nic], ox, 0);
    ARMOR(nic, 0) = ARMOR(nic, 1) = ARMOR(nic, 2) = ARMOR(nic, 3) = 0;   /* (the armour gone: health under 55%) */
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(rvTarget[nic] == 0, e_hurt, rvTarget[nic], pt_km(0, nic), pt_km(ox, nic));
    g_camp.player_ship = PT_PLAYER;
    pt_mission();
}

/* M8.52: a capital ship's mayday (hull_take, logic_on_mayday): the old game's 35 in 1,000 at every hit through the
 * armour of a Confed capital ship, so the maydays go with the hull's count (the Claw's 6, the Exeter's 2 or 3, the
 * Drayman's 1, the user remembers): an Ox bare to its hull, hit 400 times with 14, calls between 4 and 30 times; an
 * Agasicles (no line) never */
static int16_t g_maydays;
static void pt_mayday(int16_t slot)
{
    (void)slot;
    g_maydays++;
}

static void t_mayday(void)
{
    static const char __far e_ox[] = "an Ox hit 400 times through its armour called mayday on %d of them: 4 to 30 (35 in 1,000 a hit)";
    static const char __far e_aera[] = "an Agasicles, with no mayday line, called %d times";
    int16_t i, hits = 0, before;
    pt_alone(PT_PLAYER);
    logic_on_mayday = pt_mayday;
    pt_ship(1, PT_OX, SIDE_TERRAN, CTL_FROZEN, 0, 0, 20000, 0);
    for (i = 0, g_maydays = 0; i < 400 && shipAiState[1] != 9; i++) {
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
        ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
        damagePoints[1] = 0;
        before = g_maydays;
        (void)do_damage(0, 1, 14, 0);
        if (g_maydays > before)         /* (the hits that called: the host's src check runs each do_damage twice, with
                                           the same dice, so the calls come in twos there) */
            hits++;
    }
    REQUIRE(hits >= 4 && hits <= 30, e_ox, hits, 0, 0);
    pt_ship(1, 0x28, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, 0);
    for (i = 0, g_maydays = 0; i < 100; i++) {
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
        ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
        damagePoints[1] = 0;
        (void)do_damage(0, 1, 14, 0);
    }
    REQUIRE(g_maydays == 0, e_aera, g_maydays, 0, 0);
    logic_on_mayday = 0;
}

/* M8.54: a capital ship's call for help (capital_help, the old game's Kilrathi side of its capital hull routine): an
 * Agasicles 30,000 km from a still player, three Nicanders beside it (the player beyond their engage distance of
 * 25,000 km: none would pick it), hit by the player's ship through its armour twenty times: on the coins, at least one
 * Nicander's target is the player.  With a fourth Nicander 5,000 km from the player (the attacker engaged, HELP_KM),
 * the same hits call nobody */
static void t_capital_help(void)
{
    static const char __far e_ships[] = "no Agasicles or Nicanders in slots 1-4 (%d %d)";
    static const char __far e_none[] = "an Agasicles hit 20 times by an unengaged player 30,000 km off: none of its three Nicanders "
        "turned on the player (targets %d %d %d)";
    static const char __far e_engaged[] = "with a Nicander 5,000 km from the player, the hurt Agasicles still called: %d of the far "
        "three target the player";
    int16_t i, k, on;
    pt_alone(PT_PLAYER);
    playerCanBeDamaged = 0;
    pt_ship(1, 0x28, SIDE_AERA, CTL_AI, 0, 0, 30000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_AI, 3000, 0, 31000, TURN / 2);
    pt_ship(3, PT_ENEMY, SIDE_AERA, CTL_AI, -3000, 0, 31000, TURN / 2);
    pt_ship(4, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 3000, 31000, TURN / 2);
    REQUIRE(slot_is_ship(1) && gInstanceShipTypes[1] == 0x28 && slot_is_ship(4) && gInstanceShipTypes[4] == PT_ENEMY,
            e_ships, gInstanceShipTypes[1], gInstanceShipTypes[4], 0);
    if (!pt_steps(2))
        return;
    for (i = 0; i < 20; i++) {
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
        ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
        (void)do_damage(0, 1, 14, 0);
    }
    for (k = 2, on = 0; k <= 4; k++)
        on = (int16_t)(on + (rvTarget[k] == 0));
    REQUIRE(on >= 1, e_none, rvTarget[2], rvTarget[3], rvTarget[4]);
    /* the attacker engaged: a Nicander near the player */
    pt_alone(PT_PLAYER);
    playerCanBeDamaged = 0;
    pt_ship(1, 0x28, SIDE_AERA, CTL_AI, 0, 0, 30000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_AI, 3000, 0, 31000, TURN / 2);
    pt_ship(3, PT_ENEMY, SIDE_AERA, CTL_AI, -3000, 0, 31000, TURN / 2);
    pt_ship(4, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 3000, 31000, TURN / 2);
    pt_ship(5, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 5000, TURN / 2);
    if (!pt_steps(2))
        return;
    for (i = 0; i < 20; i++) {
        SHIELD(1, 0) = SHIELD(1, 1) = 0;
        ARMOR(1, 0) = ARMOR(1, 1) = ARMOR(1, 2) = ARMOR(1, 3) = 0;
        (void)do_damage(0, 1, 14, 0);
    }
    for (k = 2, on = 0; k <= 4; k++)
        on = (int16_t)(on + (rvTarget[k] == 0));
    REQUIRE(on == 0, e_engaged, on, 0, 0);
}

/* M8.51: a capital ship's turrets bear all round (the user, 2026-11-05: an Agasicles "seems to only shoot to the front";
 * its mounts had all sat at the bow: gun_mount spreads them along the hull): a still Franklin held 2,250 km behind it,
 * abeam and ahead for a minute each takes flak at rates within a factor of three of one another, and some from each */
static void t_capital_bearings(void)
{
    static const char __far e_ship[] = "no Agasicles in slot 1 (%d)";
    static const char __far e_none[] = "a still player held 2,250 km off an Agasicles (bearing %d: 0 behind, 1 abeam, 2 ahead) took %d "
        "points in a minute: at least 40";
    static const char __far e_spread[] = "flak behind %d, abeam %d, ahead %d points in a minute each: not within a factor of three";
    int32_t pts[3], lo, hi;
    int16_t b, bolts, flak;
    for (b = 0; b < 3; b++) {
        pts[b] = pt_capital_flak(0, &bolts, &flak, b, 60);
        if (pts[b] == -2)
            return;
        REQUIRE(pts[b] >= 0, e_ship, (int16_t)pts[b], 0, 0);
        REQUIRE(pts[b] >= 40, e_none, b, (int16_t)pts[b], 0);
    }
    lo = pts[0] < pts[1] ? pts[0] : pts[1];
    lo = lo < pts[2] ? lo : pts[2];
    hi = pts[0] > pts[1] ? pts[0] : pts[1];
    hi = hi > pts[2] ? hi : pts[2];
    REQUIRE(hi <= 3 * lo, e_spread, (int16_t)pts[0], (int16_t)pts[1], (int16_t)pts[2]);
}

static void t_escort_order(void)
{
    static const char __far e_there[] = "no Ox after ENIGMA2's launch (slot %d)";
    static const char __far e_pick[] = "N pressed: nav %d picked, wanted 2";
    static const char __far e_went[] = "nav 2 picked, the autopilot returned %d, nav 2 reached %d after %d presses: the pick (a "
        "hidden point on the line on the way)";
    static const char __far e_left[] = "the Ox is %d km from the player after it: left behind (past 20,000)";
    static const char __far e_on[] = "60 s on the Ox is %d km nearer nav 2 (alive %d): it flies on alone";
    int16_t ox, r, i;
    pt_launch_named("ENIGMA2");
    playerCanBeDamaged = 0;
    ox = pt_ox();
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    if (!pt_step())
        return;
    logic_nav_next();
    REQUIRE(g_sys.nav == 2, e_pick, g_sys.nav, 0, 0);
    r = logic_autopilot();              /* (to the field's hidden point on the line, short of nav 2) */
    if (!pt_steps(SEC))
        return;
    REQUIRE(slot_is_ship(ox) && pt_km(0, ox) > 20000, e_left, pt_km(0, ox), 0, 0);
    pt_put(0, g_tm.nav[1].x, g_tm.nav[1].y, g_tm.nav[1].z - 10000, 0);   /* (through the field by hand, at nav 2) */
    if (!pt_steps(2 * SEC))
        return;
    i = 1;
    REQUIRE(r == 1 && logic_nav_seen(2), e_went, r, logic_nav_seen(2), i);
    {
        int32_t before = pt_km_to(ox, g_tm.nav[1].x, g_tm.nav[1].y, g_tm.nav[1].z);
        if (!pt_kill_side(SIDE_AERA) || !pt_steps(60 * SEC))
            return;
        REQUIRE(slot_is_ship(ox) && before - pt_km_to(ox, g_tm.nav[1].x, g_tm.nav[1].y, g_tm.nav[1].z) > 5000, e_on,
                before - pt_km_to(ox, g_tm.nav[1].x, g_tm.nav[1].y, g_tm.nav[1].z), slot_is_ship(ox), 0);
    }
    g_camp.player_ship = PT_PLAYER;
    pt_mission();
}

/* M8.38: an escort target in, its nav point come to, heads for the home carrier and stops within 150 km of its hull, its
 * nose the carrier's way; it is home, so its loss there afterwards is no loss of the player's (the user, 2026-11-04:
 * "inbound ships head to the vigilance", "when the ships arrive within 150km of the vigilance they should stop and
 * ideally turn in the same direction as the vigilance") */
static void t_escort_home(void)
{
    static const char __far e_none[] = "no carrier (slot %d) or no escort in at nav 1 (slot %d)";
    static const char __far e_home[] = "60 s on from 3,000 km off, the escort is %d km off the carrier's hull at %d kps: "
        "within 150 and stopped";
    static const char __far e_way[] = "home 60 s, its nose is %d (cos x 256) to the carrier's: within 25 degrees (over 232)";
    static const char __far e_kept[] = "killed beside the carrier, logic_escort_lost says %d: 0 (it was home)";
    int16_t home = -1, ox = -1, s, i;
    int32_t gap;
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    pt_mship(PT_OX, SIDE_TERRAN, 1, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_IN;
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    if (!pt_go(0, NAV1_Z - 4500))       /* (nav 1 come to: the escort there, and the player near it) */
        return;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
        if (logic_is_home(s))
            home = s;
        if (slot_is_ship(s) && gInstanceShipTypes[s] == PT_OX)
            ox = s;
    }
    REQUIRE(home > 0 && ox > 0, e_none, home, ox, 0);
    pt_put(ox, TO_KM(gPositionVector[home].x) + 3000, 0, TO_KM(gPositionVector[home].z), TURN / 4);
    for (i = 0; i < 60 * SEC; i++)
        if (!pt_step())
            return;
    gap = pt_km(ox, home) - TO_KM(collide_half(ox)) - TO_KM(collide_half(home));   /* (hull to hull, as the logic's) */
    REQUIRE(gap <= 160 && speedLo[ox] == 0, e_home, gap, (int32_t)speedLo[ox], 0);
    if (!pt_steps(60 * SEC))
        return;
    REQUIRE(vec_dot(&gOrientationFrontVector[ox], &gOrientationFrontVector[home]) / 256 > 232, e_way,
            (int)(vec_dot(&gOrientationFrontVector[ox], &gOrientationFrontVector[home]) / 256), 0, 0);
    for (i = 0; i < 100 && shipAiState[ox] != 9; i++)
        (void)pt_hit(0xFFFF, ox, 2000, 2, 0, 0);
    if (!pt_steps(SEC))
        return;
    REQUIRE(logic_escort_lost() == 0, e_kept, logic_escort_lost(), 0, 0);
}

/* a patrol with the home carrier: the player, a wingman and the Vigilance (role=home) at nav 0, two Nicanders at nav 1,
 * an Agis at nav 2: the carrier's slot */
static int16_t pt_home_patrol(void)
{
    int16_t s;
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_nav(2, NAV2_X, 0, NAV2_Z);
    pt_mship(RV_SHIP_WINGMAN, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    pt_mship(PT_ENEMY, SIDE_AERA, 1, 2, RV_PILOT_NONE);
    pt_mship(PT_WEAK, SIDE_AERA, 2, 1, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (logic_is_home(s))
            return s;
    return -1;
}

/* M8.13: the home carrier is in the flight behind the launch, holds station, stays when the autopilot goes, and the
 * target key finds it once it is in front */
static void t_home_carrier(void)
{
    static const char __far e_there[] = "no home carrier in the slots after the launch (%d): the mission's role=home";
    static const char __far e_behind[] = "the carrier is %d km from the launch, or ahead of it (%d): behind, within 5,000 "
        "and clear of the player";
    static const char __far e_station[] = "10 s in, the carrier is %d km from where it was: it holds station";
    static const char __far e_along[] = "the autopilot took the carrier along: it is %d km from where it was";
    static const char __far e_target[] = "with the carrier ahead T picks slot %d, not the carrier (slot %d)";
    int16_t home = pt_home_patrol();
    int32_t x, y, z;
    REQUIRE(home > 0, e_there, home, 0, 0);
    REQUIRE(pt_km(0, home) < 5000 && pt_km(0, home) > TO_KM(collide_half(home) + collide_half(0)) && !logic_target_ahead(home),
            e_behind, pt_km(0, home), logic_target_ahead(home), 0);
    x = TO_KM(gPositionVector[home].x);
    y = TO_KM(gPositionVector[home].y);
    z = TO_KM(gPositionVector[home].z);
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE(pt_km_to(home, x, y, z) < 10, e_station, pt_km_to(home, x, y, z), 0, 0);
    REQUIRE(logic_autopilot(), e_there, 0, 0, 0);
    REQUIRE(pt_km_to(home, x, y, z) < 10, e_along, pt_km_to(home, x, y, z), 0, 0);
    pt_put(0, 0, 0, z - 8000, 0);       /* (facing +z, the carrier straight ahead) */
    g_sys.target = -1;
    (void)logic_target_next();
    REQUIRE(g_sys.target == home, e_target, g_sys.target, home, 0);
    g_sys.target = -1;
}

/* M8.14: the carrier's rule: no clearance at the launch; a nav point flown or the mission finished, and the player
 * within range; a clearance stands */
static void t_landing_rule(void)
{
    static const char __far e_launch[] = "at the launch the carrier answered %d: LANDING_NOT_YET (%d), a nav point first";
    static const char __far e_far[] = "a nav point flown, far off, the carrier answered %d: LANDING_TOO_FAR (%d), %d km off";
    static const char __far e_near[] = "back near home, a nav point flown, the carrier answered %d: LANDING_CLEARED";
    static const char __far e_stands[] = "cleared, then far off, the carrier answered %d: a clearance stands";
    static const char __far e_done[] = "a mission with nothing to do, beside the carrier, was answered %d: finished is enough";
    int16_t home = pt_home_patrol();
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(logic_request_landing() == LANDING_NOT_YET, e_launch, logic_request_landing(), LANDING_NOT_YET, 0);
    if (!pt_go(0, NAV1_Z) || !pt_go(0, 30000))   /* (nav 1 flown, then past LANDING_RANGE_KM, 25,000, of the carrier) */
        return;
    REQUIRE(logic_request_landing() == LANDING_TOO_FAR, e_far, logic_request_landing(), LANDING_TOO_FAR, pt_km(0, home));
    if (!pt_go(0, 3000))
        return;
    REQUIRE(logic_request_landing() == LANDING_CLEARED, e_near, logic_request_landing(), 0, 0);
    if (!pt_go(0, 30000))
        return;
    REQUIRE(logic_request_landing() == LANDING_CLEARED, e_stands, logic_request_landing(), 0, 0);
    pt_mission();
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    pt_launch(PT_PLAYER);
    if (!pt_steps(SEC))
        return;
    REQUIRE(logic_request_landing() == LANDING_CLEARED, e_done, logic_request_landing(), 0, 0);
}

/* the player flown at the carrier at 300 kps from where it is put, until it lands or stops: 0 when the test failed */
static int16_t pt_stopped;              /* what logic_carrier_stopped said, once it said something */
static int16_t pt_fly_at(int32_t x, int32_t y, int32_t z, int16_t yaw)
{
    int16_t i;
    (void)logic_carrier_stopped();
    pt_stopped = 0;
    pt_put(0, x, y, z, yaw);
    pt_speed(0, 300);
    for (i = 0; i < 20 * SEC && !missionStatus && !pt_stopped; i++) {
        if (!pt_step())
            return 0;
        pt_stopped = logic_carrier_stopped();
    }
    return 1;
}

static int16_t pt_fly_in(int32_t x, int32_t z, int16_t yaw)
{
    return pt_fly_at(x, 0, z, yaw);
}

/* M8.15: not cleared, the hull stops the fighter unhurt, and it can fly off again; cleared, the player lands on coming
 * within 2,000 km of the carrier's hull from any side (the user, 2026-11-04: "lets get rid of all the bow logic and you
 * can land just by distance to carrier after requesting"); a landing with the mission undone is lost, with it done won */
static void t_landing_contact(void)
{
    static const char __far e_bow_shut[] = "not cleared, into the bow: status %d, stopped %d (0, and 2: the hull stops it)";
    static const char __far e_hurt[] = "the hull's stop hurt the fighter: its shield was %d and is %d";
    static const char __far e_moving[] = "stopped by the hull its speed is still %d: 0";
    static const char __far e_stuck[] = "stopped, then turned away at 300 kps, it is %d km from the carrier after 3 s: it "
        "flies off (it was %d)";
    static const char __far e_clear[] = "a nav point flown, the request answered %d: 0, cleared";
    static const char __far e_near[] = "cleared, flown in from its beam: status %d, %d km off its hull: 1, landed, within 2,000";
    static const char __far e_lost[] = "victoryPoints is %d after landing with the mission undone: 0";
    static const char __far e_won[] = "victoryPoints is %d after landing with the mission done: nonzero";
    int16_t home = pt_home_patrol(), sh0, yaw_in;
    int32_t cz, d0;
    REQUIRE(home > 0, e_stuck, (int32_t)home, (int32_t)0, 0);
    cz = TO_KM(gPositionVector[home].z);
    sh0 = (int16_t)SHIELD(0, 0);
    /* a nav point flown, so the carrier would clear it; not asked yet */
    if (!pt_go(0, NAV1_Z))
        return;
    if (!pt_fly_in(0, cz + 3000, TURN / 2))
        return;
    REQUIRE(missionStatus == 0 && pt_stopped == 2, e_bow_shut, missionStatus, pt_stopped, 0);
    REQUIRE((int16_t)SHIELD(0, 0) >= sh0, e_hurt, sh0, (int16_t)SHIELD(0, 0), 0);
    REQUIRE(speedLo[0] == 0, e_moving, (int32_t)speedLo[0], 0, 0);
    /* off again, the other way */
    d0 = pt_km(0, home);
    pt_put(0, 0, 0, TO_KM(gPositionVector[0].z), 0);
    pt_speed(0, 300);
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE(pt_km(0, home) > d0 + 300, e_stuck, pt_km(0, home), d0, 0);
    /* cleared, from the beam: landed on coming near, the patrol undone: lost */
    REQUIRE(logic_request_landing() == LANDING_CLEARED, e_clear, logic_request_landing(), 0, 0);
    pt_put(0, 6000, 0, cz, TURN / 4);
    yaw_in = gOrientationFrontVector[0].x < 0 ? TURN / 4 : 3 * TURN / 4;
    if (!pt_fly_at(6000, 0, cz, yaw_in))
        return;
    REQUIRE(missionStatus == 1 && landedOn == (uint16_t)home && pt_km(0, home) > 1500, e_near, missionStatus,
            pt_km(0, home), 0);
    logic_end();
    REQUIRE(victoryPoints == 0, e_lost, victoryPoints, 0, 0);
    /* flown and killed: landed, won */
    missionStatus = 3;
    home = pt_home_patrol();
    missionStatus = 0;
    if (!pt_go(0, NAV1_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(NAV2_X, NAV2_Z) || !pt_kill_side(SIDE_AERA)
        || !pt_go(0, 6000))
        return;
    REQUIRE(logic_request_landing() == LANDING_CLEARED, e_clear, logic_request_landing(), 0, 0);
    if (!pt_fly_in(0, cz + 6000, TURN / 2))
        return;
    REQUIRE(missionStatus == 1, e_near, missionStatus, pt_km(0, home), 0);
    logic_end();
    REQUIRE(victoryPoints != 0, e_won, victoryPoints, 0, 0);
}

/* M8.46: cleared, but coming in on the afterburner (the user, 2026-11-05: "check that you don't have afterburner on when
 * landing... if you have afterburner on when attempting landing it should cause normal collision damage"): no landing
 * within the 2,000 km while it burns, the carrier says why (logic_carrier_stopped 3), and the hull is a ram's: the
 * fighter hurt and stopped; the burner off, it lands */
static void t_landing_burner(void)
{
    static const char __far e_clear[] = "a nav point flown, the request answered %d: 0, cleared";
    static const char __far e_landed[] = "on the afterburner it landed (status %d, %d km off the hull): it must not";
    static const char __far e_said[] = "on the afterburner near the carrier, logic_carrier_stopped never said 3 (last %d)";
    static const char __far e_hurt[] = "into the hull on the afterburner it took nothing: shields and armour %d, then %d";
    static const char __far e_land[] = "the burner off, %d km off the hull, status %d: 1, landed";
    int16_t home = pt_home_patrol(), said = 0, last = 0, i, k;
    int32_t cz, before = 0, after = 0;
    if (home <= 0 || !pt_go(0, NAV1_Z))
        return;
    cz = TO_KM(gPositionVector[home].z);
    REQUIRE(logic_request_landing() == LANDING_CLEARED, e_clear, logic_request_landing(), 0, 0);
    playerCanBeDamaged = 1;             /* (pt_home_patrol's player takes none) */
    pt_put(0, 0, 0, cz + 6000, TURN / 2);   /* (along its length, at its bow: from the beam at this height it would fly
                                           through the gap under the deck) */
    pt_speed(0, 300);
    for (k = 0; k < 2; k++)
        before += SHIELD(0, k);
    for (k = 0; k < 4; k++)
        before += ARMOR(0, k);
    for (i = 0; i < 20 * SEC && !missionStatus && speedLo[0] != 0; i++) {
        rvAfterburner[0] = 1;
        if (!pt_step())
            return;
        if ((last = logic_carrier_stopped()) == 3)
            said = 1;
    }
    for (k = 0; k < 2; k++)
        after += SHIELD(0, k);
    for (k = 0; k < 4; k++)
        after += ARMOR(0, k);
    REQUIRE(missionStatus == 0, e_landed, missionStatus, pt_km(0, home), 0);
    REQUIRE(said, e_said, last, 0, 0);
    REQUIRE(after < before, e_hurt, before, after, 0);
    rvAfterburner[0] = 0;
    if (!pt_steps(2))
        return;
    REQUIRE(missionStatus == 1 && landedOn == (uint16_t)home, e_land, logic_target_km(home), missionStatus, 0);
}

/* M8.47: the autopilot home, nothing on the way, stops 4,000 km off the carrier's hull, facing it (the user,
 * 2026-11-05: "why do we arrive home so far away from the carrier": it stopped at the launch's reach, 20,000 km); the
 * carrier held off the scene while the player was far is in it again */
static void t_home_arrive(void)
{
    static const char __far e_home[] = "no home carrier at the launch (%d)";
    static const char __far e_went[] = "every waypoint flown, the autopilot home returned %d: 1";
    static const char __far e_gap[] = "the autopilot home left the player %d km off the carrier's hull: 4,000 (HOME_ARRIVE_KM)";
    static const char __far e_face[] = "the autopilot home left the player facing %d/256 of the way to the carrier: it faces it";
    static const char __far e_scene[] = "home, the carrier is not in the scene (entity type %d)";
    int16_t home = -1, s, r;
    RvVec3 d;
    int32_t len, dot;
    pt_mission();
    pt_nav(1, 0, 0, 60000);
    g_tm.nav[g_tm.nnav - 1].reach = 15000;
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    pt_launch(PT_PLAYER);
    logic_scene_off = 0;
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++)
        if (logic_is_home(s))
            home = s;
    REQUIRE(home > 0, e_home, home, 0, 0);
    pt_put(0, 0, 0, 60000, 0);          /* (nav 1 come to: every waypoint flown, the way home) */
    if (!pt_steps(2))
        return;
    r = logic_autopilot();
    REQUIRE(r == 1, e_went, r, 0, 0);
    REQUIRE(slot_is_ship(home), e_scene, maybeEntityType[home], 0, 0);
    REQUIRE(labs(logic_target_km(home) - 4000) < 300, e_gap, logic_target_km(home), 0, 0);
    d = vec3(TO_KM(gPositionVector[home].x - gPositionVector[0].x), TO_KM(gPositionVector[home].y - gPositionVector[0].y),
             TO_KM(gPositionVector[home].z - gPositionVector[0].z));
    len = pt_km(0, home);
    dot = len ? (FIX_MUL(gOrientationFrontVector[0].x, d.x) + FIX_MUL(gOrientationFrontVector[0].y, d.y)
                 + FIX_MUL(gOrientationFrontVector[0].z, d.z)) * 256 / len : 0;
    REQUIRE(dot > 245, e_face, dot, 0, 0);
}

/* M8.48: a fighter of ours with role=stay (the carrier's patrol, the old game's: the user, 2026-11-05, "missions have to
 * be able to differentiate between autopilotees and stay-homers") is left where it is by the autopilot, and with
 * nobody to fight keeps round the carrier; one with no role, as near the player, comes along */
static void t_stay_home(void)
{
    static const char __far e_slots[] = "the fighters at the launch: the one that stays in slot %d, the other in slot %d";
    static const char __far e_went[] = "the autopilot to nav 1 returned %d: 1";
    static const char __far e_came[] = "the fighter with no role is %d km off the player after the autopilot: it comes along";
    static const char __far e_stayed[] = "the fighter with role=stay is %d km from the carrier after the autopilot (it was %d): "
        "it stays";
    static const char __far e_round[] = "back home, after 60 s with nobody to fight, the one that stays is %d km from the "
        "carrier: round it, within 6,000";
    int16_t stay = -1, other = -1, home = -1, s, r;
    int32_t was;
    pt_mission();
    pt_nav(1, 0, 0, 60000);
    g_tm.nav[g_tm.nnav - 1].reach = 15000;
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    pt_mship(0x01, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);   /* (a Franklin: the carrier's patrol) */
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_STAY;
    g_tm.ship[g_tm.nship - 1].z = 2500;
    pt_mship(PT_HOMER, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);   /* (a Gawain: none) */
    g_tm.ship[g_tm.nship - 1].x = 1500;
    pt_launch(PT_PLAYER);
    logic_scene_off = 0;
    playerCanBeDamaged = 0;
    for (s = 1; s <= RV_MAX_SHIP_SLOT; s++) {
        if (logic_is_home(s))
            home = s;
        else if (slot_is_ship(s) && gInstanceShipTypes[s] == 0x01)
            stay = s;
        else if (slot_is_ship(s) && gInstanceShipTypes[s] == PT_HOMER)
            other = s;
    }
    REQUIRE(home > 0 && stay > 0 && other > 0, e_slots, stay, other, 0);
    was = pt_km(stay, home);
    r = logic_autopilot();
    REQUIRE(r == 1, e_went, r, 0, 0);
    REQUIRE(pt_km(0, other) < 10000, e_came, pt_km(0, other), 0, 0);
    REQUIRE(labs(pt_km(stay, home) - was) < 1000, e_stayed, pt_km(stay, home), was, 0);
    pt_put(0, 0, 0, 3000, 0);           /* (home again: the carrier and its patrol in the scene) */
    if (!pt_steps(60 * SEC))
        return;
    REQUIRE(slot_is_ship(stay) && pt_km(stay, home) < 6000, e_round, pt_km(stay, home), 0, 0);
}

/* M8.53: the landing by the trigger (the user, 2026-11-06: "autolanding kicked in when there are no enemies present
 * and are very near the carrier and it can say press fire to land. If all landing conditions are met and all nav
 * points have been reached"): not at the launch, not with a waypoint unflown, nor far from the carrier; with both
 * flown, their enemies dead, 3,000 km off the carrier, it is ready, and the trigger lands */
#define PT_LAND_KM 2000                 /* (logic.c LANDING_NEAR_KM: a cleared landing's distance, the trigger's too) */
static void t_autoland(void)
{
    static const char __far e_launch[] = "at the launch the landing by the trigger was ready (%d): no waypoint flown yet";
    static const char __far e_half[] = "with nav 2 unflown it was ready (%d)";
    static const char __far e_far[] = "every waypoint flown, %d km from the carrier, it was ready (%d): too far";
    static const char __far e_near[] = "the carrier's hull %d km off, outside a cleared landing's 2,000: ready (%d) (the "
        "user: \"it should only happen when I'm nearly colliding it\")";
    static const char __far e_ready[] = "every waypoint flown, nobody about, %d km off the carrier: not ready (%d)";
    static const char __far e_land[] = "the trigger: logic_autoland %d, status %d, landed on %d: 1, 1, the carrier";
    int16_t home = pt_home_patrol(), r;
    int32_t cx, cz, d, g = 0;
    if (home <= 0 || !pt_steps(SEC))
        return;
    REQUIRE(logic_autoland_ready() < 0, e_launch, logic_autoland_ready(), 0, 0);
    if (!pt_go(0, NAV1_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(0, 3000))
        return;
    REQUIRE(logic_autoland_ready() < 0, e_half, logic_autoland_ready(), 0, 0);
    if (!pt_go(NAV2_X, NAV2_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(0, 30000))
        return;
    REQUIRE(logic_autoland_ready() < 0, e_far, logic_target_km(home), logic_autoland_ready(), 0);
    /* (in from in front of the carrier, 500 km at a time: not before its hull is within a cleared landing's distance) */
    cx = TO_KM(gPositionVector[home].x);
    cz = TO_KM(gPositionVector[home].z);
    for (d = 9000; d > 0; d -= 500) {
        if (!pt_go(cx, cz + d))
            return;
        g = logic_target_km(home);
        if (g > PT_LAND_KM + 100)
            REQUIRE(logic_autoland_ready() < 0, e_near, g, logic_autoland_ready(), 0);
        else if (g < PT_LAND_KM - 100)
            break;
    }
    REQUIRE(d > 0 && logic_autoland_ready() == home, e_ready, g, logic_autoland_ready(), 0);
    r = logic_autoland();
    REQUIRE(r == 1 && missionStatus == 1 && landedOn == (uint16_t)home, e_land, r, missionStatus, (int16_t)landedOn);
}

/* M8.16: the carrier destroyed: the flight ends with it (missionStatus 6) when its explosion has run, and it is a loss
 * however well the patrol went */
static void t_carrier_lost(void)
{
    static const char __far e_alive[] = "2,000 points a hit, 400 hits, and the carrier is not dying (state %d)";
    static const char __far e_early[] = "the flight ended (%d) the moment the carrier began to die: its explosion runs first";
    static const char __far e_status[] = "missionStatus is %d 10 s after the carrier began to die: 6";
    static const char __far e_score[] = "victoryPoints is %d with the carrier destroyed: 0";
    int16_t home = pt_home_patrol(), i;
    if (!pt_go(0, NAV1_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(NAV2_X, NAV2_Z) || !pt_kill_side(SIDE_AERA))
        return;
    for (i = 0; i < 400 && shipAiState[home] != 9; i++)
        (void)pt_hit(0xFFFF, home, 2000, 2, 0, 0);
    REQUIRE(shipAiState[home] == 9, e_alive, shipAiState[home], 0, 0);
    if (!pt_step())
        return;
    REQUIRE(missionStatus == 0, e_early, missionStatus, 0, 0);
    for (i = 0; i < 10 * SEC && !missionStatus; i++)
        if (!pt_step())
            return;
    REQUIRE(missionStatus == MISSION_CARRIER_LOST, e_status, missionStatus, 0, 0);
    logic_end();
    REQUIRE(victoryPoints == 0, e_score, victoryPoints, 0, 0);
}

/* M8.17: a mission's turrets=off: an enemy 1,500 km from the carrier, frozen: the carrier fires at it, and with its
 * turrets off it fires nothing */
static int16_t pt_carrier_shots(uint8_t noturrets)
{
    uint16_t a0;
    pt_mission();
    pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
    g_tm.ship[g_tm.nship - 1].noturrets = noturrets;
    pt_launch(PT_PLAYER);
    playerCanBeDamaged = 0;
    g_sys.armed = 0;
    pt_put(0, 0, 0, 30000, 0);
    pt_speed(0, 0);
    pt_ship(20, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, -500, 0);
    a0 = shots_added();
    if (!pt_steps(5 * SEC))
        return -1;
    return (int16_t)(shots_added() - a0);
}

static void t_turrets_off(void)
{
    static const char __far e_on[] = "with its turrets on the carrier fired %d shots in 5 s at an enemy 1,500 km off: it should";
    static const char __far e_off[] = "with turrets=off the carrier fired %d shots in 5 s at an enemy 1,500 km off: none";
    int16_t on = pt_carrier_shots(0), off;
    if (on < 0)
        return;
    REQUIRE(on > 0, e_on, on, 0, 0);
    off = pt_carrier_shots(1);
    if (off < 0)
        return;
    REQUIRE(off == 0, e_off, off, 0, 0);
}

/* M8.18: a fighter attacks a capital ship without flying into it (it breaks off by the hull's distance, not the middle's):
 * an ace from each of 16 sides, alone, on the carrier with its turrets off, 90 s each: few touch its hull (a bump no
 * longer kills an NPC: M6.6), and they fire */
static void t_attack_capital(void)
{
    static const char __far e_crash[] = "%d of 16 lone aces attacking the carrier flew into it inside 90 s: at most 3";
    static const char __far e_none[] = "16 lone aces attacking the carrier fired no shot";
    int16_t a, i, s, crashes = 0, touched;
    uint16_t a0 = 0, a1 = 0;
    LfVec push;
    int32_t depth;
    for (a = 0; a < 16; a++) {
        int32_t x = lf_cos((int16_t)(a * 64)) * 4000 / 16384, z = lf_sin((int16_t)(a * 64)) * 4000 / 16384;
        pt_mission();
        pt_mship(PT_CAPSHIP, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
        g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_HOME;
        g_tm.ship[g_tm.nship - 1].noturrets = 1;
        pt_launch(PT_PLAYER);
        /* (its own dice, whatever its place in the table: its count of 16 is a measure near its limit, and these are
           the ones it was measured with, at index 108 until 2026-11-03; the index a uint32_t, as the runner's: 108 x
           977 passes a 16-bit int, and DOS threw other dice) */
        randomSeed = UINT32_C(0x52560000) + (uint32_t)108 * 977;
        playerCanBeDamaged = 0;
        g_sys.armed = 0;
        pt_put(0, 0, 0, 30000, 0);
        pt_speed(0, 0);
        pt_ship(20, 0x0D, SIDE_AERA, CTL_AI, x, 0, z - 2000, (int16_t)(a * 64 + TURN / 2));
        a0 = shots_added();
        touched = 0;
        for (i = 0; i < 90 * SEC && slot_is_ship(20) && shipAiState[20] != 9; i++) {
            if (!pt_step())
                return;
            for (s = 1; s < 20 && !touched; s++)
                if (slot_is_ship(s) && maybeEntityType[s] == ET_CAPSHIP && slot_is_ship(20)
                    && collide_ships(20, s, &push, &depth))
                    touched = 1;
        }
        a1 = (uint16_t)(a1 + (uint16_t)(shots_added() - a0));
        if (touched || (slot_is_ship(20) && shipAiState[20] == 9))
            crashes++;
        missionStatus = 0;
    }
    REQUIRE(crashes <= 3, e_crash, crashes, 0, 0);
    REQUIRE(a1 != 0, e_none, 0, 0, 0);
}

/* M8.19: the autopilot lands nobody on anybody: the wingman placed so its offset comes down exactly on the spot the Ox is
 * put (800 km behind the player at the arrival), at speed: they arrive apart, and in 3 s the Ox takes no hit */
static void t_autopilot_apart(void)
{
    static const char __far e_ox[] = "no Ox (%d) or wingman (%d) after the launch";
    static const char __far e_apart[] = "after the jump the wingman is %ld km from the Ox: clear of it, over %ld";
    static const char __far e_hit[] = "the Ox has lost %d of its front shield 3 s after the jump: nobody hit it";
    int16_t ox, wing = -1, i, sh0;
    missionStatus = 3;
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_mship(RV_SHIP_WINGMAN, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    pt_mship(PT_OX, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    g_tm.ship[g_tm.nship - 1].role = RV_ESCORT_OUT;
    pt_launch(PT_PLAYER);
    missionStatus = 0;
    playerCanBeDamaged = 0;
    g_sys.armed = 0;
    ox = pt_ox();
    for (i = 1; i <= RV_MAX_SHIP_SLOT; i++)
        if (slot_is_ship(i) && maybeEntityType[i] == ET_SHIP && commAnimInfo[i] == SIDE_TERRAN)
            wing = i;
    REQUIRE(ox > 0 && wing > 0, e_ox, ox, wing, 0);
    pt_put(0, 0, 0, 0, 0);
    pt_put(wing, 0, 0, -800, 0);        /* (the player goes on along +z: the wingman stays 800 km behind it) */
    pt_speed(wing, 450);
    sh0 = (int16_t)SHIELD(ox, 0);
    REQUIRE(logic_autopilot(), e_ox, ox, wing, 0);
    REQUIRE(pt_km(wing, ox) > TO_KM(collide_half(wing) + collide_half(ox)), e_apart, pt_km(wing, ox),
            TO_KM(collide_half(wing) + collide_half(ox)), 0);
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE((int16_t)SHIELD(ox, 0) >= sh0 && damagePoints[ox] == 0, e_hit, sh0 - (int16_t)SHIELD(ox, 0), 0, 0);
}

/* M6.4: a capital ship is hit on its actual shape whatever the mission's rule: under hit=sphere a bolt 400 km off the
 * Vigilance's beam, within its 700 km radius but clear of its hull, flies on; one along its keel hits it */
static int16_t pt_capital_shot(int32_t x, int32_t y)
{
    RvVec3 at, vel;
    int16_t sh0, i, ar0;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    pt_ship(1, PT_CAPSHIP, SIDE_AERA, CTL_FROZEN, 0, 0, 0, 0);
    sh0 = (int16_t)SHIELD(1, 1);
    ar0 = (int16_t)SHIELD(1, 0);
    at = vec3(x * KM, y * KM, -1200L * KM);
    vel = vec3(0, 0, 20L * KM);
    (void)shot_add(0, GUN_TYPE(0, 0), &at, &vel);
    for (i = 0; i < 100 && shots_count(); i++)
        if (!pt_step())
            return -1;
    return (int16_t)(sh0 != (int16_t)SHIELD(1, 1) || ar0 != (int16_t)SHIELD(1, 0));
}

static void t_capital_map(void)
{
    static const char __far e_miss[] = "a shot through the carrier's sphere, 400 km off its axis and clear of its hull, under "
        "hit=sphere: it hit (%d): it flies on";
    static const char __far e_hit[] = "a shot along the carrier's deck under hit=sphere: it hit %d: on its actual shape it hits";
    uint8_t keep = g_rule_hit;
    int16_t r;
    g_rule_hit = RV_SHIPTYPE_FLAG_HIT_SPHERE;
    r = pt_capital_shot(400, 0);
    g_rule_hit = keep;
    if (r < 0)
        return;
    REQUIRE(r == 0, e_miss, r, 0, 0);
    g_rule_hit = RV_SHIPTYPE_FLAG_HIT_SPHERE;
    r = pt_capital_shot(0, 75);
    g_rule_hit = keep;
    if (r < 0)
        return;
    REQUIRE(r == 1, e_hit, r, 0, 0);
}

/* M6.5: a fighter against a capital ship is that ship's silhouettes under every setting: clear of the hull but well
 * inside its sphere nothing touches, on the deck it does, whatever the mission's hit= and ram= say */
static void t_capital_ram(void)
{
    static const char __far e_clear[] = "hit rule %d, ram flags %d: a fighter 400 km off the carrier's axis, clear of its hull: collides";
    static const char __far e_coarse[] = "the fighter 400 km off is not inside the carrier's reach (half %ld km): the test proves nothing";
    static const char __far e_deck[] = "hit rule %d, ram flags %d: a fighter in the carrier's deck: does not collide";
    static const uint8_t rules[4] = { 0, RV_SHIPTYPE_FLAG_HIT_SPHERE, RV_SHIPTYPE_FLAG_HIT_BOX, RV_SHIPTYPE_FLAG_HIT_SILHOUETTE };
    uint8_t keep_hit = g_rule_hit, keep_flags = g_rule_flags;
    int16_t i, j, coarse = 1, clear = -1, deck = -1;
    LfVec push;
    int32_t depth;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 2; j++) {
            pt_alone(PT_PLAYER);
            pt_ship(1, PT_CAPSHIP, SIDE_AERA, CTL_FROZEN, 0, 0, 0, 0);
            g_rule_hit = rules[i];
            g_rule_flags = (uint8_t)(j ? (keep_flags | RV_RULE_RAM_SOFT) : (keep_flags & ~RV_RULE_RAM_SOFT));
            gPositionVector[0] = vec3(400L * KM, 0, 0);
            coarse &= collide_coarse(0, 1) != 0;
            if (collide_ships(0, 1, &push, &depth) && clear < 0)
                clear = i * 2 + j;
            gPositionVector[0] = vec3(0, 75L * KM, 0);
            if (!collide_ships(0, 1, &push, &depth) && deck < 0)
                deck = i * 2 + j;
        }
    g_rule_hit = keep_hit;
    g_rule_flags = keep_flags;
    REQUIRE(coarse, e_coarse, (long)(collide_half(1) / KM), 0, 0);
    REQUIRE(clear < 0, e_clear, clear / 2, clear & 1, 0);
    REQUIRE(deck < 0, e_deck, deck / 2, deck & 1, 0);
}

/* M8.20: an escorted ship's jump is told once, friendly, from where it was, and it is no kill */
static int16_t pt_jumps, pt_jump_slot, pt_jump_friend;
static void pt_on_jump(int16_t slot, int16_t friendly)
{
    pt_jumps++;
    pt_jump_slot = slot;
    pt_jump_friend = friendly;
}

static void t_jump_told(void)
{
    static const char __far e_there[] = "no Ox after the launch (%d)";
    static const char __far e_once[] = "the Ox reached its jump and logic_on_jump was called %d times, for slot %d (the Ox: %d): once";
    static const char __far e_friend[] = "the Ox's jump was told as friendly %d: 1, the player's side";
    static const char __far e_kill[] = "the Ox's jump counted as a kill: it is away, and the mission is not lost by it";
    int16_t ox = pt_escort(), i;
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    pt_jumps = 0;
    logic_on_jump = pt_on_jump;
    pt_put(ox, 0, 0, NAV1_Z - 1500, 0);   /* (500 km before its reach, ROUTE_OUT_KM) */
    pt_speed(ox, 80);
    for (i = 0; i < 20 * SEC && pt_jumps == 0; i++)
        if (!pt_step()) {
            logic_on_jump = 0;
            return;
        }
    logic_on_jump = 0;
    REQUIRE(pt_jumps == 1 && pt_jump_slot == ox, e_once, pt_jumps, pt_jump_slot, ox);
    REQUIRE(pt_jump_friend == 1, e_friend, pt_jump_friend, 0, 0);
    REQUIRE(!slot_is_ship(ox) || gInstanceShipTypes[ox] != PT_OX, e_kill, 0, 0, 0);
}

/* M8.21: the wingman lost: logic_wingman_lost names the pilot; a dead pilot's wingman record is left out of the next
 * mission, so it is flown solo; and a line with a dead pilot in it, or one conditioned on a death, says so */
static void t_wingman_lost(void)
{
    static const char __far e_none[] = "no wingman, or no pilot on it (pilot %d, %d wingmen)";
    static const char __far e_clean[] = "logic_wingman_lost says pilot %d with the wingman alive: -1";
    static const char __far e_named[] = "the wingman killed, logic_wingman_lost says pilot %d: %d";
    static const char __far e_solo[] = "%d wingmen with their pilot dead: the record is left out, the mission solo";
    static const char __far e_line[] = "a line by the dead pilot is ok (%d), or one for their death is not (%d)";
    int16_t pilot = pt_patrol(), wing[2], n, i, s = -1, keep;
    RvTalk t;
    n = logic_wingmen(wing, 2);
    REQUIRE(pilot != RV_PILOT_NONE && n >= 1, e_none, pilot, n, 0);
    playerCanBeDamaged = 0;
    REQUIRE(logic_wingman_lost() == -1, e_clean, logic_wingman_lost(), 0, 0);
    s = wing[0];
    for (i = 0; i < 100 && shipAiState[s] != 9; i++)
        (void)pt_hit(0xFFFF, s, 2000, 2, 0, 0);
    if (!pt_step())
        return;
    REQUIRE(logic_wingman_lost() == pilot, e_named, logic_wingman_lost(), pilot, 0);
    keep = g_camp.pilot[pilot].alive;
    g_camp.pilot[pilot].alive = 0;
    (void)pt_patrol();
    n = logic_wingmen(wing, 2);
    _fmemset(&t, 0, sizeof t);
    t.speaker = (uint8_t)pilot;
    t.listener = RV_PILOT_NONE;
    i = camp_line_ok(&t);
    t.speaker = RV_PILOT_NONE;
    t.flags = RV_TALK_DEAD;
    t.arg = (uint8_t)pilot;
    s = camp_line_ok(&t);
    g_camp.pilot[pilot].alive = (uint8_t)keep;
    REQUIRE(n == 0, e_solo, n, 0, 0);
    REQUIRE(i == 0 && s == 1, e_line, i, s, 0);
}

/* M8.22: Kit's boost: half as much armour again on each face, or a round more in the first launcher, for a flight that
 * starts with it (logic_boost, a line of hers on the deck); nothing without it.  (The fuel is the flight's: tests/scenes.txt) */
static void t_kit_boost(void)
{
    static const char __far e_armor[] = "armour face %d: %d with Kit's plate, %d without: half as much again";
    static const char __far e_round[] = "launcher %d: %d rounds with Kit's missile, %d without: one more";
    static const char __far e_plain[] = "armour %d after the boosted flights, %d before: no boost, none";
    int16_t plain[4], plated[4], k, l, rounds, more, again;
    logic_boost = 0;
    (void)pt_patrol();
    l = logic_launcher();
    for (k = 0; k < 4; k++)
        plain[k] = ARMOR(0, k);
    rounds = l >= 0 ? GUN_STATE(0, l) & 0x7F : 0;
    logic_boost = RV_BOOST_ARMOR;
    (void)pt_patrol();
    for (k = 0; k < 4; k++)
        plated[k] = ARMOR(0, k);
    logic_boost = RV_BOOST_MISSILE;
    (void)pt_patrol();
    more = l >= 0 ? GUN_STATE(0, l) & 0x7F : 0;
    logic_boost = 0;
    (void)pt_patrol();
    again = ARMOR(0, 0);
    for (k = 0; k < 4; k++)
        REQUIRE(plated[k] == plain[k] + plain[k] / 2, e_armor, k, plated[k], plain[k]);
    REQUIRE(l >= 0 && more == rounds + 1, e_round, l, more, rounds);
    REQUIRE(again == plain[0], e_plain, again, plain[0], 0);
}

/* M8.23: how a system comes out of the flight just flown (a debrief's system=): nothing until its last mission; then
 * good with more won than lost, bad with fewer, an even split by the running total, as camp_resolve will have it */
static void t_way_out(void)
{
    static const char __far e_way[] = "wins %d, the flight %d: system= says %d";
    RvSave keep = g_camp;
    int16_t node, count, nb, ng, ne;
    for (node = 0; node < (int16_t)g_nnodes && camp_node_missions(node) != 3; node++)
        ;
    g_camp.node = (uint8_t)node;
    count = camp_node_missions(node);
    g_camp.trend = 0;
    g_camp.mission = 0;
    g_camp.wins = 0;
    camp_way_out(1);
    ne = g_camp_way;
    g_camp.mission = (uint8_t)(count - 1);
    g_camp.wins = 1;
    camp_way_out(1);
    ng = g_camp_way;
    camp_way_out(0);
    nb = g_camp_way;
    g_camp = keep;
    REQUIRE(ne == 0, e_way, 0, 1, ne);
    REQUIRE(ng == 1, e_way, 1, 1, ng);
    REQUIRE(nb == 2, e_way, 1, 0, nb);
}

/* M8.24: the chosen nav point: it follows the way ahead until N picks one; N goes down the list and round through home;
 * the autopilot goes to the one chosen, and the choice goes back to following once it is reached */
static void t_nav_choice(void)
{
    static const char __far e_first[] = "after the launch the chosen nav point is %d: 1, the way ahead";
    static const char __far e_next[] = "N pressed %d times: nav %d chosen, wanted %d";
    static const char __far e_went[] = "nav 2 chosen, the autopilot left currentNavPoint at %d (returned %d): a patrol in "
        "any order";
    static const char __far e_back[] = "nav 2 reached: the chosen nav point is %d, the first not reached (1)";
    static const char __far e_home[] = "home chosen with nav 1 still to fly, the autopilot returned %d and left the player "
        "%d km from the launch: home (giving the mission up)";
    (void)pt_patrol();
    if (!pt_step())
        return;
    REQUIRE(g_sys.nav == 1, e_first, g_sys.nav, 0, 0);
    logic_nav_next();
    REQUIRE(g_sys.nav == 2, e_next, 1, g_sys.nav, 2);
    logic_nav_next();
    REQUIRE(g_sys.nav == 0, e_next, 2, g_sys.nav, 0);
    logic_nav_next();
    REQUIRE(g_sys.nav == 1, e_next, 3, g_sys.nav, 1);
    logic_nav_next();
    REQUIRE(g_sys.nav == 2, e_next, 4, g_sys.nav, 2);
    {
        int16_t i, r = 0;
        for (i = 1; i < NUM_SHIP_SLOTS; i++)
            if (slot_is_ship(i) && commAnimInfo[i] != commAnimInfo[0])
                ship_clear(i);
        r = logic_autopilot();
        REQUIRE(r == 1 && currentNavPoint == 2, e_went, currentNavPoint, r, 0);
    }
    if (!pt_step())
        return;
    REQUIRE(g_sys.nav == 1, e_back, g_sys.nav, 0, 0);
    {
        int16_t i, r;
        for (i = 1; i < NUM_SHIP_SLOTS; i++)
            if (slot_is_ship(i) && commAnimInfo[i] != commAnimInfo[0])
                ship_clear(i);
        pt_put(0, 40000, 0, 0, 0);      /* (out past the launch's reach, 20,000 km, nav 1 off the line home) */
        for (i = 0; i < 4 && g_sys.nav != 0; i++)
            logic_nav_next();
        r = logic_autopilot();
        REQUIRE(r == 1 && pt_km_to(0, 0, 0, 0) < 21000, e_home, r, pt_km_to(0, 0, 0, 0), 0);   /* (nav 1 still to fly) */
    }
}

/* M8.25: the last nav point's ships leave the scene: past 16,000 km (the old game's) the carrier is no ship to
 * anything, so it cannot be targeted, and it is back as it was when the player is; the wing stays with the player */
static void t_scene(void)
{
    static const char __far e_there[] = "no home carrier at the launch (%d)";
    static const char __far e_gone[] = "the player %ld km from the carrier: it is still a ship in the world, or the target";
    static const char __far e_wing[] = "the wingman was left out of the scene with the carrier (slot %d)";
    static const char __far e_back[] = "the player back near the carrier: the slot is not a ship again (%d)";
    static const char __far e_same[] = "the carrier came back with its shield at %d, it left with %d";
    int16_t home = pt_home_patrol(), wing[2], nw, z;
    uint16_t sh;
    logic_scene_off = 0;
    REQUIRE(home > 0, e_there, home, 0, 0);
    SHIELD(home, 0) = 33;
    sh = SHIELD(home, 0);
    z = (int16_t)TO_KM(gPositionVector[home].z);
    g_sys.target = home;
    nw = logic_wingmen(wing, 2);
    pt_put(0, 0, 0, (int16_t)(z + 20000), 0);
    if (nw)
        pt_put(wing[0], 800, 0, (int16_t)(z + 20000), 0);
    if (!pt_step())
        return;
    REQUIRE(!slot_is_ship(home) && g_sys.target != home, e_gone, 20000L, 0, 0);
    if (nw)
        REQUIRE(slot_is_ship(wing[0]), e_wing, wing[0], 0, 0);
    pt_put(0, 0, 0, (int16_t)(z + 3000), 0);
    if (!pt_step())
        return;
    REQUIRE(slot_is_ship(home), e_back, home, 0, 0);
    REQUIRE(SHIELD(home, 0) >= sh, e_same, SHIELD(home, 0), sh, 0);
}

/* M8.27: a second wave waits for the first: a hidden point's enemies do not come while an enemy fighter is out; when the
 * last is killed they come; when the last one ran they are called off */
static int16_t pt_enemies_out(void)
{
    int16_t i, n = 0;
    for (i = 1; i < NUM_SHIP_SLOTS; i++)
        n += slot_is_ship(i) && commAnimInfo[i] != commAnimInfo[0] && shipAiState[i] != 9;
    return n;
}

static void pt_two_waves(void)
{
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_nav(2, 0, 0, NAV1_Z);
    g_tm.nav[g_tm.nnav - 1].flags = 1;
    pt_mship(PT_ENEMY, SIDE_AERA, 1, 1, RV_PILOT_NONE);
    pt_mship(PT_ENEMY, SIDE_AERA, 2, 3, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    pt_put(0, 0, 0, NAV1_Z - 3000, 0);
}

static void t_second_wave(void)
{
    static const char __far e_first[] = "at the point: %d enemies out, wanted the first wave's 1";
    static const char __far e_came[] = "the first wave killed: %d enemies out, wanted the second wave's 3";
    static const char __far e_off[] = "the last enemy ran: %d enemies out, wanted none, the second wave called off";
    static const char __far e_done[] = "the wave called off: victoryPoints %d, it should ask nothing of the player";
    int16_t i;
    pt_two_waves();
    if (!pt_steps(30))
        return;
    REQUIRE(pt_enemies_out() == 1, e_first, pt_enemies_out(), 0, 0);
    for (i = 1; i < NUM_SHIP_SLOTS; i++)
        if (slot_is_ship(i) && commAnimInfo[i] != commAnimInfo[0])
            shipAiState[i] = 9;
    if (!pt_steps(10))
        return;
    REQUIRE(pt_enemies_out() == 3, e_came, pt_enemies_out(), 0, 0);
    pt_two_waves();
    if (!pt_steps(30))
        return;
    for (i = 1; i < NUM_SHIP_SLOTS; i++)
        if (slot_is_ship(i) && commAnimInfo[i] != commAnimInfo[0])
            logic_test_leave(i);
    if (!pt_steps(30))
        return;
    REQUIRE(pt_enemies_out() == 0, e_off, pt_enemies_out(), 0, 0);
    REQUIRE(victoryPoints != 0, e_done, victoryPoints, 0, 0);
}

/* M8.28: enemies at the launch: the wingman starts past them, on the far side from the player; none: beside the player */
static void t_wingman_behind(void)
{
    static const char __far e_near[] = "the wingman %d km from the player, on the player's side of the enemies (%d km)";
    static const char __far e_none[] = "no enemies at the launch: the wingman %d km from the player, wanted it beside";
    int16_t i, w = -1, e = -1;
    int32_t dw, de;
    pt_mission();
    pt_mship(RV_SHIP_WINGMAN, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    pt_mship(PT_ENEMY, SIDE_AERA, 0, 2, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    for (i = 1; i < NUM_SHIP_SLOTS; i++)
        if (slot_is_ship(i)) {
            if (commAnimInfo[i] == commAnimInfo[0])
                w = i;
            else
                e = i;
        }
    if (w < 0 || e < 0)
        return;
    dw = player_km(w);
    de = player_km(e);
    REQUIRE(dw > de + 2000, e_near, (int)dw, (int)de, 0);
    pt_mission();
    pt_mship(RV_SHIP_WINGMAN, SIDE_TERRAN, 0, 1, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    for (i = 1; i < NUM_SHIP_SLOTS; i++)
        if (slot_is_ship(i))
            w = i;
    REQUIRE(player_km(w) < 2000, e_none, (int)player_km(w), 0, 0);
}

/* M8.26: the autopilot traces the line ahead: it stops short of a field across it and will not go from its edge; it
 * stops at a hidden trigger point on the way, whose ships then come; hidden points are on no list and ask nothing */
static void pt_field(int16_t x, int16_t y, int16_t z, int16_t r)
{
    RvField __far *f = &g_tm.field[g_tm.nfield++];
    f->type = RV_FIELD_ASTEROID;
    f->density = 0;
    f->x = x;
    f->y = y;
    f->z = z;
    f->radius = r;
}

/* M8.55: a waypoint inside a field is the field (the user, 2026-11-06: "in the very first mission, asteroids were
 * supposed to be entirely surrounding nav 2", "the nav point gets ticked off even though I'm nowhere close to it"):
 * Enigma 1's nav 2, reach 20,000, in a field 7,500 across 5,830 from its middle.  The autopilot takes the player into the
 * field, halfway from the point to its edge; the point is not reached within its reach outside the field, and is
 * inside it */
static void t_field_nav(void)
{
    static const char __far e_in[] = "the autopilot to a nav point in a field left the player %d km from the field's "
        "middle (returned %d): inside its 7,500, about 2,750 short of the point";
    static const char __far e_seen[] = "brought into the field, nav 1 is not reached (%d)";
    static const char __far e_out[] = "%d km from the point, within its 20,000 reach but outside its field, nav 1 was "
        "reached: only in the field";
    static const char __far e_field[] = "%d km from the field's middle, inside it, nav 1 is not reached (%d)";
    int16_t r;
    int32_t z;
    pt_mission();
    pt_nav(1, 0, 0, 30000);
    g_tm.nav[0].reach = 20000;
    pt_field(0, 0, 28000, 7500);
    pt_launch(PT_PLAYER);
    r = logic_autopilot();
    z = TO_KM(gPositionVector[0].z);
    REQUIRE(r == 1 && labs(z - 27250) < 150, e_in, (int)labs(z - 28000), r, 0);
    if (!pt_step())
        return;
    REQUIRE(logic_nav_seen(1), e_seen, logic_nav_seen(1), 0, 0);
    pt_mission();
    pt_nav(1, 0, 0, 30000);
    g_tm.nav[0].reach = 20000;
    pt_field(0, 0, 28000, 7500);
    pt_launch(PT_PLAYER);
    pt_put(0, 0, 0, 12000, 0);
    if (!pt_steps(SEC))
        return;
    REQUIRE(!logic_nav_seen(1), e_out, 18000, 0, 0);
    pt_put(0, 0, 0, 22000, 0);
    if (!pt_steps(SEC))
        return;
    REQUIRE(logic_nav_seen(1), e_field, (int)labs(TO_KM(gPositionVector[0].z) - 28000), logic_nav_seen(1), 0);
}

static void t_way_ahead(void)
{
    static const char __far e_field[] = "a field at 10,000 km, 1,000 across: the autopilot stopped at %d km (returned %d): wanted some 8,500";
    static const char __far e_edge[] = "at the field's edge the autopilot went on to %d km: it should refuse";
    static const char __far e_past[] = "past the field the autopilot left the player at %d km: wanted some 15,000, at the nav point's reach";
    static const char __far e_hid[] = "a hidden point at 10,000 km: the autopilot stopped at %d km: wanted some 5,500 "
        "(500 inside its reach of 5,000)";
    static const char __far e_came[] = "the hidden point's ships did not come (%d enemies in the world)";
    static const char __far e_list[] = "the hidden point is on the player's lists (logic_nav_km says %d), or was chosen (%d)";
    static const char __far e_done[] = "the patrol flown and the hidden point never come to: victoryPoints %d, it asks nothing: 1";
    int16_t r, i, enemies;
    int32_t km;
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_field(0, 0, 10000, 1000);
    pt_launch(PT_PLAYER);
    r = logic_autopilot();
    REQUIRE(r == 1 && labs(TO_KM(gPositionVector[0].z) - 8500) < 150, e_field, (int)TO_KM(gPositionVector[0].z), r, 0);
    REQUIRE(logic_autopilot() == 0, e_edge, (int)TO_KM(gPositionVector[0].z), 0, 0);
    pt_put(0, 0, 0, 12000, 0);
    r = logic_autopilot();
    REQUIRE(r == 1 && labs(TO_KM(gPositionVector[0].z) - (NAV1_Z - PT_REACH_KM)) < 150, e_past,
            (int)TO_KM(gPositionVector[0].z), 0, 0);
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_nav(2, 0, 0, 10000);
    g_tm.nav[g_tm.nnav - 1].flags = 1;
    pt_mship(PT_ENEMY, SIDE_AERA, 2, 2, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    REQUIRE(logic_nav_km(2, &km) == 0 && g_sys.nav != 2, e_list, logic_nav_km(2, &km), g_sys.nav, 0);
    r = logic_autopilot();
    REQUIRE(r == 1 && labs(TO_KM(gPositionVector[0].z) - (10000 - PT_REACH_KM + 500)) < 150, e_hid,
            (int)TO_KM(gPositionVector[0].z), 0, 0);
    enemies = 0;
    for (i = 1; i < NUM_SHIP_SLOTS; i++)
        enemies += slot_is_ship(i) && commAnimInfo[i] != commAnimInfo[0];
    REQUIRE(enemies == 2, e_came, enemies, 0, 0);
    pt_mission();
    pt_nav(1, 0, 0, NAV1_Z);
    pt_nav(2, 15000, 0, 0);
    g_tm.nav[g_tm.nnav - 1].flags = 1;
    pt_mship(PT_ENEMY, SIDE_AERA, 2, 2, RV_PILOT_NONE);
    pt_launch(PT_PLAYER);
    (void)logic_autopilot();
    if (!pt_step())
        return;
    gPositionVector[0] = vec3(0, 0, 0);
    if (!pt_step())
        return;
    missionStatus = 1;
    logic_end();
    REQUIRE(victoryPoints != 0, e_done, victoryPoints, 0, 0);
}

/* M8.11 */
static void t_escort_lost(void)
{
    static const char __far e_there[] = "no Ox in the world after the launch (slot %d)";
    static const char __far e_dead[] = "the Ox is not dying after a hit of 2000 (shipAiState %d)";
    static const char __far e_lost[] = "victoryPoints is %d after the Ox was killed, with every nav flown and every "
        "enemy dead: 0 (an escort target of the player's side lost is the mission lost)";
    static const char __far e_told[] = "logic_escort_lost says %d with the Ox killed: 1 (the debrief's escort=lost)";
    static const char __far e_early[] = "logic_escort_lost says %d with the Ox alive: 0";
    int16_t ox = pt_escort(), i;
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    REQUIRE(logic_escort_lost() == 0, e_early, logic_escort_lost(), 0, 0);
    (void)pt_hit(0xFFFF, ox, 2000, 2, 0, 0);
    for (i = 0; i < 12 * SEC && slot_is_ship(ox) && gInstanceShipTypes[ox] == PT_OX && shipAiState[ox] != 9; i++)
        if (!pt_step())
            return;
    REQUIRE(!slot_is_ship(ox) || gInstanceShipTypes[ox] != PT_OX || shipAiState[ox] == 9, e_dead, shipAiState[ox], 0, 0);
    if (!pt_go(0, NAV1_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(NAV2_X, NAV2_Z) || !pt_kill_side(SIDE_AERA) || !pt_go(0, 0))
        return;
    missionStatus = 1;
    logic_end();
    REQUIRE(victoryPoints == 0, e_lost, victoryPoints, 0, 0);
    REQUIRE(logic_escort_lost() == 1, e_told, logic_escort_lost(), 0, 0);
}

/* M8.12 */
static void t_escort_along(void)
{
    static const char __far e_there[] = "no Ox in the world after the launch (slot %d)";
    static const char __far e_went[] = "the autopilot returned %d and left the player %d km from nav 1: it stops at its "
        "reach, 5,000 km short";
    static const char __far e_along[] = "after the autopilot the Ox is %d km from the player: it comes along, 800 km "
        "behind";
    static const char __far e_behind[] = "after the autopilot the Ox is %d km along the player's nose: behind it";
    int16_t ox = pt_escort(), r;
    int32_t along;
    REQUIRE(ox > 0, e_there, ox, 0, 0);
    if (!pt_steps(SEC))
        return;
    r = logic_autopilot();
    REQUIRE(r && pt_km_to(0, 0, 0, NAV1_Z) > 3500 && pt_km_to(0, 0, 0, NAV1_Z) < 5500, e_went, r,
            pt_km_to(0, 0, 0, NAV1_Z), 0);
    REQUIRE(pt_km(0, ox) >= 600 && pt_km(0, ox) <= 1200, e_along, pt_km(0, ox), 0, 0);
    along = ((gPositionVector[ox].x - gPositionVector[0].x) / 256 * gOrientationFrontVector[0].x
             + (gPositionVector[ox].y - gPositionVector[0].y) / 256 * gOrientationFrontVector[0].y
             + (gPositionVector[ox].z - gPositionVector[0].z) / 256 * gOrientationFrontVector[0].z) / 256;
    REQUIRE(along < -400, e_behind, along, 0, 0);
}

/* ======================================================================================================== */
/* M10: the cockpit (logic_target_*, the lock, SYS_*; the user's rulings of 2026-10-31: the target key only in front,
 * a lock, no lock with the tracking wrecked) */

/* three frozen enemies about a still player facing +z: slot 1 straight ahead at 5,000 km, slot 2 45 degrees right
 * at 4,243 km, slot 3 behind at 4,000 km */
static void pt_three(void)
{
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 5000, TURN / 2);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 3000, 0, 3000, TURN / 2);
    pt_ship(3, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, -4000, 0);
}

/* M10.1 */
static void t_target_front(void)
{
    static const char __far e_first[] = "T from no target picked slot %d: the ship nearest the nose in front (slot 1)";
    static const char __far e_second[] = "T again picked slot %d: the next in front round by the angle (slot 2)";
    static const char __far e_round[] = "T a third time picked slot %d: round to the first in front (slot 1), never "
        "the one behind";
    static const char __far e_behind[] = "the player turned round, T picked slot %d: the ship that was behind is in "
        "front now (slot 3)";
    static const char __far e_none[] = "with nothing in front T changed the target to %d: it keeps the one it has";
    static const char __far e_nearest[] = "Y picked slot %d: the nearest enemy in front (slot 2), not the one behind";
    pt_three();
    if (!pt_step())
        return;
    REQUIRE(logic_target_next() == 1, e_first, g_sys.target, 0, 0);
    REQUIRE(logic_target_next() == 2, e_second, g_sys.target, 0, 0);
    REQUIRE(logic_target_next() == 1, e_round, g_sys.target, 0, 0);
    REQUIRE(logic_target_nearest() == 2, e_nearest, g_sys.target, 0, 0);
    pt_put(0, 0, 0, 0, TURN / 2);
    if (!pt_step())
        return;
    REQUIRE(logic_target_next() == 3, e_behind, g_sys.target, 0, 0);
    pt_put(0, 0, 0, 0, 0);
    ship_clear(1);
    ship_clear(2);
    if (!pt_step())
        return;
    REQUIRE(logic_target_next() == 3, e_none, g_sys.target, 0, 0);
}

/* M10.2 */
static void t_target_lock(void)
{
    static const char __far e_lock[] = "L on slot 1 did not lock (%d, hold %d)";
    static const char __far e_kept[] = "locked on slot 1, T or Y moved the target to %d: a lock keeps it";
    static const char __far e_off[] = "L again left the lock on (%d)";
    static const char __far e_free[] = "unlocked, Y did not take the nearest enemy in front (slot 2): %d";
    static const char __far e_dead[] = "the locked target died and the target is %d, the lock %d: both go";
    static const char __far e_track[] = "with the target tracking wrecked L locked (%d, hold %d): it must not";
    static const char __far e_drop[] = "the tracking wrecked after a lock, the lock stayed (%d): it goes";
    pt_three();
    if (!pt_step())
        return;
    (void)logic_target_next();
    REQUIRE(logic_target_lock() == 1 && g_sys.hold, e_lock, g_sys.target, g_sys.hold, 0);
    (void)logic_target_next();
    (void)logic_target_nearest();
    REQUIRE(g_sys.target == 1, e_kept, g_sys.target, 0, 0);
    REQUIRE(logic_target_lock() == 0 && !g_sys.hold, e_off, g_sys.hold, 0, 0);
    REQUIRE(logic_target_nearest() == 2, e_free, g_sys.target, 0, 0);
    (void)logic_target_lock();
    shipAiState[2] = 9;
    if (!pt_step())
        return;
    REQUIRE(g_sys.target == -1 && !g_sys.hold, e_dead, g_sys.target, g_sys.hold, 0);
    (void)logic_target_next();
    systemDamage[SYS_TRACK] = SYS_WRECKED;
    REQUIRE(logic_target_lock() == 0 && !g_sys.hold, e_track, g_sys.target, g_sys.hold, 0);
    systemDamage[SYS_TRACK] = 0;
    (void)logic_target_lock();
    systemDamage[SYS_TRACK] = SYS_WRECKED;
    if (!pt_step())
        return;
    REQUIRE(!g_sys.hold, e_drop, g_sys.hold, 0, 0);
    systemDamage[SYS_TRACK] = 0;
}

/* M10.3: the systems: hits through the armour to the hull hurt them (a point each, the overlay bit), the shield
 * generator wrecked stops the shields coming back, the power plant wrecked slows the gun energy to a fifth and holds
 * it to 40% (never none: the user's), a hurt generator slows the shields in the type's own steps, and a GUNS point is
 * a gun destroyed */
static void t_systems(void)
{
    static const char __far e_whole[] = "%d hits through the armour to the hull hurt no system (hurt_system)";
    static const char __far e_overlay[] = "a system hurt and no cockpit overlay bit set (rvCockpitDamage 0)";
    static const char __far e_guns[] = "GUNS at %d and %d guns destroyed: a point is a gun";
    static const char __far e_shield[] = "the shield generator wrecked, the front shield came back to %d in 10 s";
    static const char __far e_shield_ok[] = "the generator whole again, the front shield stayed at %d for 10 s";
    static const char __far e_power[] = "the power plant wrecked, the gun energy went from 10 to %d in 1 s (a fifth of "
        "a point a step: 16)";
    static const char __far e_power_cap[] = "the power plant wrecked, the gun energy at %d after 6 s (held to 40)";
    static const char __far e_slow_shield[] = "the Gawain's generator moderately hurt, its front shield came back %d in 3 s "
        "(a point every 3 of the old game's frames, 6 steps, a third of them: 5)";
    int16_t i, k, hurt = 0, guns = 0;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, TURN / 2);
    SHIELD(0, 0) = 0;
    ARMOR(0, 0) = 0;
    for (i = 0; i < 40; i++) {          /* (hits of 20 through the bare front: the odds of a system 20 in the hull's 56;
                                           the hull made whole after each, so the player lives through the forty) */
        (void)pt_hit(0xFFFF, 0, 20, 1, 0, 0);
        damagePoints[0] = 0;
    }
    for (k = 0; k < SYS_COUNT; k++)
        hurt = (int16_t)(hurt + sys_level(k));
    REQUIRE(hurt > 0, e_whole, i, 0, 0);
    REQUIRE(rvCockpitDamage != 0, e_overlay, 0, 0, 0);
    for (k = 0; k < GUN_COUNT(0); k++)
        if (pt_is_gun(0, k) && (GUN_STATE(0, k) & GUN_DESTROYED))
            guns++;
    /* (pt_guns counts the working ones: the destroyed and the working are all the ship has) */
    REQUIRE(guns == (systemDamage[SYS_GUNS] < guns + pt_guns(0) ? systemDamage[SYS_GUNS] : guns + pt_guns(0)), e_guns,
            systemDamage[SYS_GUNS], guns, 0);
    damage_reset();
    pt_no_repairs();
    systemDamage[SYS_SHIELDS] = SYS_WRECKED;
    SHIELD(0, 0) = 0;
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE(SHIELD(0, 0) == 0, e_shield, SHIELD(0, 0), 0, 0);
    systemDamage[SYS_SHIELDS] = 0;
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE(SHIELD(0, 0) > 0, e_shield_ok, SHIELD(0, 0), 0, 0);
    systemDamage[SYS_POWER] = SYS_WRECKED;
    gunEnergy[0] = 10;
    if (!pt_steps(SEC))
        return;
    REQUIRE(gunEnergy[0] >= 15 && gunEnergy[0] <= 17, e_power, gunEnergy[0], 0, 0);
    if (!pt_steps(5 * SEC))
        return;
    REQUIRE(gunEnergy[0] == 40, e_power_cap, gunEnergy[0], 0, 0);
    memset(systemDamage, 0, systemDamage_SIZE);
    pt_alone(0x02);                     /* (the Gawain: shield_steps 3, the moderate generator's 1 + 2 the same) */
    pt_no_repairs();
    systemDamage[SYS_SHIELDS] = 2;
    SHIELD(0, 0) = 0;
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE(SHIELD(0, 0) >= 10 / WC_FRAME_STEPS - 1 && SHIELD(0, 0) <= 10 / WC_FRAME_STEPS + 1, e_slow_shield, SHIELD(0, 0), 0, 0);
    memset(systemDamage, 0, systemDamage_SIZE);
}

#define PT_MIXED 0x05                   /* the Lancelot: two particle cannons and two ion guns, four heat seekers and
                                           two image-recognition missiles */

/* M10.4: the weapon choice: G cycles the guns over all and each type the ship carries, and the trigger then fires
 * that type alone; W cycles the launchers, and the missile key fires the chosen one */
static void t_weapon_choice(void)
{
    static const char __far e_types[] = "the Lancelot's gun types: %d (two particle cannons and two ion guns: 2)";
    static const char __far e_cycle[] = "G from all went %d, %d, %d: the first type, the second, all again";
    static const char __far e_only[] = "with type %d chosen the trigger fired %d guns of it and %d of the other: that "
        "type alone";
    static const char __far e_launchers[] = "W from the first launcher went %d, then %d: round the launchers";
    static const char __far e_fires[] = "the missile key with launcher choice %d fired from gun %d, not a launcher of "
        "the chosen kind (type %d)";
    uint16_t types[8], m;
    int16_t n, a, b, c, g, mine, other, l0, l1, l2, w1;
    pt_alone(PT_MIXED);
    n = logic_gun_types(types, 8);
    REQUIRE(n == 2, e_types, n, 0, 0);
    a = logic_guns_next();
    b = logic_guns_next();
    c = logic_guns_next();
    REQUIRE(a == 1 && b == 2 && c == 0, e_cycle, a, b, c);
    g_sys.gun_sel = 2;
    g_pilot.guns = 1;
    if (!pt_step())
        return;
    g_pilot.guns = 0;
    for (g = mine = other = 0; g < GUN_COUNT(0); g++)
        if (pt_is_gun(0, g) && (GUN_STATE(0, g) & 0x7F)) {
            if (GUN_TYPE(0, g) == types[1])
                mine++;
            else
                other++;
        }
    REQUIRE(mine == 2 && other == 0, e_only, 2, mine, other);
    g_sys.gun_sel = 0;
    l0 = logic_launcher();
    w1 = logic_missile_next();
    l1 = logic_launcher();
    (void)logic_missile_next();
    l2 = logic_launcher();
    REQUIRE(w1 == 1 && l1 != l0 && l2 != l1, e_launchers, l1, l2, 0);
    g_sys.launcher_sel = 1;
    l1 = logic_launcher();
    m = fireGunFromShip(0, (uint16_t)l1);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_fires, 1, l1, GUN_TYPE(0, l1));
    g_sys.launcher_sel = 0;
}

/* M10.5: the nav cross (host only): the chosen nav point 5,000 km right of a point 20,000 km ahead draws a mark a
 * quarter of the focal length right of the view's middle */
static void t_nav_cross(void)
{
#ifdef RV_HOST
    static const char __far e_none[] = "logic_hud drew nothing within 8 px of the chosen nav point's place on the view "
        "(%d, %d): the white cross";
    static const char __far e_stray[] = "with the nav point behind the player logic_hud still drew %d pixels there";
    RvCamera cam;
    int16_t x, y, n;
    pt_mission();
    pt_nav(1, 5000, 0, 20000);
    pt_launch(PT_PLAYER);               /* (pt_alone would empty the mission again) */
    if (!slot_is_ship(0) || rvController[0] != CTL_PLAYER) {
        ship_spawn(0, PT_PLAYER, SIDE_TERRAN, CTL_PLAYER, 0, 0, 0, 0);
        shipStateByte[0] = 8;
    }
    pt_put(0, 0, 0, 0, 0);
    pt_speed(0, 0);
    g_sys.nav = 1;
    if (!pt_step())
        return;
    flight_camera(&cam);
    x = (int16_t)(cam.cx + cam.focal / 4);
    y = cam.cy;
    memset(host_screen, 0, sizeof host_screen);
    hud_band(0, HOST_H, NULL, 0, 0);
    logic_hud();
    REQUIRE(pt_drawn((int16_t)(x - 8), (int16_t)(y - 8), (int16_t)(x + 9), (int16_t)(y + 9)), e_none, x, y, 0);
    pt_put(0, 0, 0, 0, TURN / 2);
    if (!pt_step())
        return;
    memset(host_screen, 0, sizeof host_screen);
    hud_band(0, HOST_H, NULL, 0, 0);
    logic_hud();
    n = pt_drawn((int16_t)(x - 8), (int16_t)(y - 8), (int16_t)(x + 9), (int16_t)(y + 9));
    REQUIRE(n == 0, e_stray, n, 0, 0);
    g_sys.nav = 0;
#endif
}

/* M10.6: the wingman's orders: a wingman 10,000 km right of the player, an enemy 6,000 km ahead of the player (A) and
 * one 6,000 km ahead of the wingman (B): free, it fights B (the nearest to it); told to attack the player's target
 * (A), A; told to help, A (the nearest to the player); told to form up, nobody, and it stays within 3,000 km of the
 * player; told home with a destroyer of its side 20,000 km behind, it goes to within 5,000 km of it */
static void t_orders(void)
{
    static const char __far e_wing[] = "logic_wingmen found %d wingmen (slot 1 is the player's side, AI flown): 1";
    static const char __far e_free[] = "free, the wingman's target is slot %d: the enemy nearest to it (slot 3)";
    static const char __far e_attack[] = "told to attack the player's target (slot 2) its target is slot %d";
    static const char __far e_help[] = "told to help, its target is slot %d: the enemy nearest the player (slot 2)";
    static const char __far e_form[] = "told to form up, its target is %d and it is %ld km from the player after 10 s: "
        "nobody, within 3,000";
    static const char __far e_notarget[] = "attack my target with no target set was taken (%d): it is refused";
    static const char __far e_home[] = "told home, the wingman is %ld km from the destroyer after 40 s: within 5,000";
    int16_t wing[4], n, i;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_PLAYER, SIDE_TERRAN, CTL_AI, 10000, 0, 0, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 6000, TURN / 2);
    pt_ship(3, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 10000, 0, 6000, TURN / 2);
    n = logic_wingmen(wing, 4);
    REQUIRE(n == 1 && wing[0] == 1, e_wing, n, 0, 0);
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(rvTarget[1] == 3, e_free, rvTarget[1], 0, 0);
    g_sys.target = -1;
    REQUIRE(logic_order(1, ORDER_ATTACK) == 0, e_notarget, 1, 0, 0);
    g_sys.target = 2;
    (void)logic_order(1, ORDER_ATTACK);
    if (!pt_steps(SEC))
        return;
    REQUIRE(rvTarget[1] == 2, e_attack, rvTarget[1], 0, 0);
    (void)logic_order(1, ORDER_FREE);
    if (!pt_steps(2 * SEC))
        return;
    (void)logic_order(1, ORDER_HELP);
    if (!pt_steps(SEC))
        return;
    REQUIRE(rvTarget[1] == 2, e_help, rvTarget[1], 0, 0);
    pt_put(1, 800, 0, 0, 0);
    (void)logic_order(1, ORDER_FORMATION);
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE(rvTarget[1] == 0xFF && pt_km(0, 1) < 3000, e_form, rvTarget[1], pt_km(0, 1), 0);
    pt_ship(4, PT_CAPSHIP, SIDE_TERRAN, CTL_AI, 0, 0, -20000, 0);
    (void)logic_order(1, ORDER_RTB);
    for (i = 0; i < 40 * SEC && pt_km(1, 4) > 4000; i++)
        if (!pt_step())
            return;
    REQUIRE(pt_km(1, 4) < 5000, e_home, pt_km(1, 4), 0, 0);
    (void)logic_order(1, ORDER_FREE);
    g_sys.target = -1;
}

/* M10.8: the wingman told to fly in formation fights nobody and keeps its place; told to break formation it goes for
 * an enemy however far, beyond the reach it fights at by itself, and with no enemy about it leaves its place and
 * circles the player */
static void t_break_formation(void)
{
    static const char __far e_free[] = "left alone, the wingman's target is %d with the only enemy 40,000 km off: nobody";
    static const char __far e_form[] = "told to fly in formation its target is %d, %ld km from the player: nobody, near";
    static const char __far e_far[] = "told to break formation its target is slot %d: the enemy 40,000 km off (slot 2)";
    static const char __far e_left[] = "broken off with no enemy about, its way off its old place stayed under %ld km in "
        "10 s: it leaves the formation";
    static const char __far e_round[] = "broken off with no enemy about, it is %ld km from the player: it stays round "
        "the player, within 6,000";
    int16_t i, moved = 0;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_PLAYER, SIDE_TERRAN, CTL_AI, 800, 0, 0, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 40000, TURN / 2);
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(rvTarget[1] == 0xFF, e_free, rvTarget[1], 0, 0);
    (void)logic_order(1, ORDER_FORMATION);
    if (!pt_steps(5 * SEC))
        return;
    REQUIRE(rvTarget[1] == 0xFF && pt_km(0, 1) < 3000, e_form, rvTarget[1], pt_km(0, 1), 0);
    (void)logic_order(1, ORDER_BREAK);
    if (!pt_steps(SEC))
        return;
    REQUIRE(rvTarget[1] == 2, e_far, rvTarget[1], 0, 0);
    ship_clear(2);
    pt_put(1, 800, 0, 0, 0);
    for (i = 0; i < 10 * SEC; i++) {
        if (!pt_step())
            return;
        if (pt_km(0, 1) > 1500)
            moved = 1;
    }
    REQUIRE(moved, e_left, (int32_t)1500, 0, 0);
    REQUIRE(pt_km(0, 1) < 6000, e_round, pt_km(0, 1), 0, 0);
    (void)logic_order(1, ORDER_FREE);
}

/* M10.9: the taunt: an enemy fighting a nearer ship turns on the player, however far, and fires though it is the
 * player's selected target (the held fire is off); it does not move a friend or a capital ship */
static void t_taunt(void)
{
    static const char __far e_first[] = "before the taunt the enemy's target is slot %d: the Terran ship beside it (slot 2)";
    static const char __far e_turn[] = "taunted, the enemy's target is slot %d: the player (slot 0)";
    static const char __far e_others[] = "logic_taunt took a friend (%d) or a capital ship (%d)";
    static const char __far e_fire[] = "the taunted enemy, the player's selected target, with the player on its nose fired "
        "%d shots in 15 s: a taunted ship holds no fire";
    int16_t i, a0;
    pt_alone(PT_PLAYER);
    g_sys.armed = 0;
    playerCanBeDamaged = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 16000, TURN / 2);
    pt_ship(2, PT_PLAYER, SIDE_TERRAN, CTL_FROZEN, 1000, 0, 16000, 0);
    pt_ship(3, PT_CAPSHIP, SIDE_AERA, CTL_FROZEN, -20000, 0, 0, 0);
    if (!pt_steps(2 * SEC))
        return;
    REQUIRE(rvTarget[1] == 2, e_first, rvTarget[1], 0, 0);
    REQUIRE(!logic_taunt(2) && !logic_taunt(3), e_others, logic_taunt(2), logic_taunt(3), 0);
    REQUIRE(logic_taunt(1), e_turn, rvTarget[1], 0, 0);
    if (!pt_steps(SEC))
        return;
    REQUIRE(rvTarget[1] == 0, e_turn, rvTarget[1], 0, 0);
    /* the held fire: the player selects it, it faces the player from 3,000 km */
    g_rule_flags = RV_RULE_AIM_CONE | RV_RULE_HOLD_FIRE;
    ship_clear(2);
    ship_clear(3);
    pt_put(1, 0, 0, 3000, TURN / 2);
    rvTarget[0] = 1;
    (void)logic_taunt(1);
    a0 = shots_added();
    for (i = 0; i < 15 * SEC; i++) {
        rvTarget[0] = 1;
        if (!pt_step())
            return;
    }
    REQUIRE(shots_added() != a0, e_fire, shots_added() - a0, 0, 0);
    rvTarget[0] = 0xFF;
}

/* M10.7: the missile lock: the Lancelot's heat seeker on a target 3,000 km ahead flying away locks in its lock time
 * (3 s), and the lock goes the step the target turns to face the player; the image-recognition missile (W) locks on
 * it facing or not; the tracking wrecked, or the EMCON off, no lock; the Schroedinger's dumbfire never locks */
static void t_missile_lock(void)
{
    static const char __far e_heat[] = "the heat seeker on a target flying away 3,000 km ahead: after 100 steps the lock "
        "is %d at %d percent (its lock time 3 s)";
    static const char __far e_early[] = "the heat seeker locked after %d steps: its lock time is 3 s (90 steps)";
    static const char __far e_face[] = "the target turned to face the player and the heat seeker's lock stayed (%d, %d "
        "percent): a heat seeker locks from behind only";
    static const char __far e_image[] = "the image-recognition missile on the facing target: after 100 steps the lock is "
        "%d (it needs no rear aspect)";
    static const char __far e_track[] = "the tracking wrecked, the lock is %d at %d percent: none";
    static const char __far e_emcon[] = "the EMCON off, the heat seeker's lock is %d: it needs the sensors on";
    static const char __far e_dumb[] = "a dumbfire chosen, the lock is %d at %d percent: a dumbfire never locks";
    int16_t i, at = -1;
    pt_alone(PT_MIXED);
    g_sys.armed = 0;
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);   /* (facing +z: away from the player) */
    g_sys.target = 1;
    g_sys.launcher_sel = 0;             /* (the first launcher: a heat seeker) */
    for (i = 0; i < 100; i++) {
        if (!pt_step())
            return;
        if (g_sys.lock && at < 0)
            at = i;
    }
    REQUIRE(g_sys.lock == 1 && g_sys.lock_pct == 100, e_heat, g_sys.lock, g_sys.lock_pct, 0);
    REQUIRE(at >= 85, e_early, at, 0, 0);
    pt_put(1, 0, 0, 3000, TURN / 2);
    if (!pt_step())
        return;
    REQUIRE(g_sys.lock == 0 && g_sys.lock_pct == 0, e_face, g_sys.lock, g_sys.lock_pct, 0);
    while (logic_launcher() >= 0 && weapon(GUN_TYPE(0, logic_launcher()))->lock != RV_LOCK_IMAGE)
        (void)logic_missile_next();
    if (!pt_steps(100))
        return;
    REQUIRE(g_sys.lock == 1, e_image, g_sys.lock, 0, 0);
    systemDamage[SYS_TRACK] = SYS_WRECKED;
    if (!pt_step())
        return;
    REQUIRE(g_sys.lock == 0 && g_sys.lock_pct == 0, e_track, g_sys.lock, g_sys.lock_pct, 0);
    systemDamage[SYS_TRACK] = 0;
    g_sys.launcher_sel = 0;
    pt_put(1, 0, 0, 3000, 0);
    g_sys.emcon = EMCON_OFF;
    if (!pt_steps(100))
        return;
    REQUIRE(g_sys.lock == 0, e_emcon, g_sys.lock, 0, 0);
    g_sys.emcon = EMCON_ACTIVE;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, 0);
    g_sys.target = 1;
    if (!pt_steps(100))
        return;
    REQUIRE(g_sys.lock == 0 && g_sys.lock_pct == 0, e_dumb, g_sys.lock, g_sys.lock_pct, 0);
    g_sys.target = -1;
}

/* --- M11: the simulator (simulator.c; the user, 2026-11-01) --- */

static int16_t sim_enemies(int16_t *type)
{
    int16_t s, n = 0;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (maybeEntityType[s] == ET_SHIP && commAnimInfo[s] == SIDE_RLAAN) {
            n++;
            *type = (int16_t)gInstanceShipTypes[s];
        }
    return n;
}

/* steps of the flight with the simulator's, until there are enemies (some 1) or none: 0 when they never come */
static int16_t sim_until(int16_t some)
{
    int16_t i, t;
    for (i = 0; i < 600; i++) {
        if (!pt_step())
            return 0;
        simulator_frame();
        if ((sim_enemies(&t) > 0) == some)
            return 1;
    }
    return 0;
}

/* M11.1 */
static void t_sim_waves(void)
{
    static const char __far e_none[] = "wave %d never came, or the last would not die";
    static const char __far e_wave[] = "wave %d: %d ships of type %02X (see the table of what should come in the test)";
    static const char __far e_kills[] = "%u kills after seven waves of eight ships: simulator_frame counts each death once";
    static const uint8_t __far want[7][2] = { { 1, 0x10 }, { 1, 0x11 }, { 1, 0x12 }, { 1, 0x13 }, { 1, 0x14 }, { 1, 0x15 }, { 2, 0x10 } };
    int16_t w, n, t = -1;
    pt_alone(PT_PLAYER);
    simulator_start();
    for (w = 0; w < 7; w++) {
        REQUIRE(sim_until(1), e_none, w, 0, 0);
        n = sim_enemies(&t);
        REQUIRE(n == want[w][0] && t == want[w][1], e_wave, w, n, t);
        simulator_test_kill();
        REQUIRE(sim_until(0), e_none, w, 0, 0);
    }
    REQUIRE(simulator_kills() == 8, e_kills, simulator_kills(), 0, 0);
}

/* M11.2 */
static void t_sim_replenish(void)
{
    static const char __far e_none[] = "wave %d never came, or the last would not die";
    static const char __far e_early[] = "wave %d, the same count: the front shield is %u, not healed: the count has not gone up";
    static const char __far e_full[] = "the count went up to two and the shields are %u and %u of %u: healed every time the count increases";
    static const char __far e_used[] = "the count went up to two and the fuel is %d, the guns' energy %u: replenished then too";
    static const char __far e_round[] = "the count went up to two and gun %d's state is %u, %u loaded: missiles replenished then too";
    static const char __far e_same[] = "wave %d, the same count: the fuel is %d: not replenished until the count goes up";
    uint8_t rounds[8];
    int16_t w, g, t = -1;
    pt_alone(PT_PLAYER);
    simulator_start();
    g_sys.fuel = 1000;
    for (g = 0; g < GUN_COUNT(0) && g < 8; g++)
        rounds[g] = GUN_STATE(0, g);
    for (w = 0; w < 6; w++) {
        REQUIRE(sim_until(1), e_none, w, 0, 0);
        if (w) {
            REQUIRE(SHIELD(0, 0) < SHIELD_MAX(0, 0), e_early, w, SHIELD(0, 0), 0);
            REQUIRE(g_sys.fuel == 1, e_same, w, g_sys.fuel, 0);
        }
        SHIELD(0, 0) = 1;
        SHIELD(0, 1) = 1;
        g_sys.fuel = 1;
        gunEnergy[0] = 1;
        for (g = 0; g < GUN_COUNT(0) && g < 8; g++)
            GUN_STATE(0, g) = 0;
        simulator_test_kill();
        REQUIRE(sim_until(0), e_none, w, 0, 0);
    }
    REQUIRE(sim_until(1), e_none, 6, 0, 0);
    REQUIRE(sim_enemies(&t) == 2 && SHIELD(0, 0) == SHIELD_MAX(0, 0) && SHIELD(0, 1) == SHIELD_MAX(0, 1), e_full, SHIELD(0, 0),
            SHIELD(0, 1), SHIELD_MAX(0, 0));
    REQUIRE(g_sys.fuel == 1000 && gunEnergy[0] == 100, e_used, g_sys.fuel, gunEnergy[0], 0);
    for (g = 0; g < GUN_COUNT(0) && g < 8; g++) {
        const RvWeapon __far *we = weapon(GUN_TYPE(0, g));
        if (we && we->kind != RV_WK_GUN && we->kind != RV_WK_TURRET)
            REQUIRE(GUN_STATE(0, g) == rounds[g], e_round, g, GUN_STATE(0, g), rounds[g]);
    }
}

/* M11.3 */
static void t_sim_win(void)
{
    static const char __far e_none[] = "wave %d never came, or the last would not die";
    static const char __far e_early[] = "won at %u kills: the win is at 47";
    static const char __far e_late[] = "%u kills in wave %d and not won: the win is at 47";
    static const char __far e_round[] = "won in the round of %d: 6, 12 and 18 kills bring 36, so 47 comes in the round of four";
    static const char __far e_end[] = "won %d, missionStatus %u, %d more enemies spawned: the win ends the flight, with no wave after it";
    int16_t w, t = -1, i;
    pt_alone(PT_PLAYER);
    simulator_start();
    for (w = 0; w < 30 && !simulator_won(); w++) {
        REQUIRE(sim_until(1), e_none, w, 0, 0);
        REQUIRE(!simulator_won(), e_early, simulator_kills(), 0, 0);
        simulator_test_kill();
        REQUIRE(sim_until(0), e_none, w, 0, 0);
    }
    REQUIRE(simulator_won(), e_late, simulator_kills(), w, 0);
    REQUIRE(simulator_kills() >= 47 && simulator_count() == 4, e_round, simulator_count(), 0, 0);
    for (i = 0; i < 90 && !missionStatus; i++) {
        if (!pt_step())
            return;
        simulator_frame();
    }
    REQUIRE(missionStatus == 1 && sim_enemies(&t) == 0, e_end, simulator_won(), missionStatus, sim_enemies(&t));
    missionStatus = 0;
}

/* M11.4 */
static void t_sim_death(void)
{
    static const char __far e_more[] = "%d enemies came after the player died: the simulator goes until the player is destroyed";
    static const char __far e_end[] = "missionStatus %u, won %d: a dead player ends the flight (4), not won";
    int16_t i, t = -1;
    pt_alone(PT_PLAYER);
    simulator_start();
    shipAiState[0] = 9;
    for (i = 0; i < 200 && !missionStatus; i++) {
        if (!pt_step())
            return;
        simulator_frame();
    }
    REQUIRE(sim_enemies(&t) == 0, e_more, sim_enemies(&t), 0, 0);
    REQUIRE(missionStatus == 4 && !simulator_won(), e_end, missionStatus, simulator_won(), 0);
    missionStatus = 0;
}

/* M9.1 */
static void t_gunsight(void)
{
#ifdef RV_HOST
    static const char __far e_none[] = "logic_hud drew nothing within 30 px of the view's middle (%d, %d): a gunsight";
    RvCamera cam;
    pt_alone(PT_PLAYER);
    if (!pt_step())
        return;
    flight_camera(&cam);
    memset(host_screen, 0, sizeof host_screen);
    hud_band(0, HOST_H, NULL, 0, 0);
    logic_hud();
    REQUIRE(pt_drawn((int16_t)(cam.cx - 30), (int16_t)(cam.cy - 30), (int16_t)(cam.cx + 31), (int16_t)(cam.cy + 31)),
            e_none, cam.cx, cam.cy, 0);
#endif
}

/* M9.2 */
static void t_brackets(void)
{
#ifdef RV_HOST
    static const char __far e_none[] = "logic_hud drew nothing round the target (its box %d px about %d, %d): "
        "brackets";
    RvCamera cam;
    EntBox b;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 1200, 900, 4000, TURN / 2);
    g_sys.target = 1;
    if (!pt_step())
        return;
    flight_camera(&cam);
    flight_ent_box(1, &cam, &b);
    memset(host_screen, 0, sizeof host_screen);
    hud_band(0, HOST_H, NULL, 0, 0);
    logic_hud();
    memset(host_screen + (cam.cy - 30) * HOST_W, 0, 61 * HOST_W);   /* (not the gunsight's rows) */
    REQUIRE(pt_drawn((int16_t)(b.sx - b.half - 10), (int16_t)(b.sy - b.half - 10), (int16_t)(b.sx + b.half + 11),
                     (int16_t)(b.sy + b.half + 11)), e_none, b.half, b.sx, b.sy);
#endif
}


/* ======================================================================================================== */
/* M12: the player's damage (damage.c, docs/damage.md) */

/* the player's ship down to its hull: nothing between a hit and it */
static void pt_bare(void)
{
    int16_t k;
    for (k = 0; k < 2; k++)
        SHIELD(0, k) = 0;
    for (k = 0; k < 4; k++)
        ARMOR(0, k) = 0;
}

/* M12.1: past the hull's last point the player lives on the reserve; nobody else has one */
static void t_reserve(void)
{
    static const char __far e_dead[] = "the hull gone after %d points and the player's ship is dying: the reserve takes what is over";
    static const char __far e_flag[] = "the hull is gone and damage_in_reserve says %d";
    static const char __far e_long[] = "%d more points past the hull and still alive: the reserve is %d (1.7 hulls)";
    static const char __far e_short[] = "only %d points past the hull killed the player: the reserve is %d (1.7 hulls)";
    static const char __far e_npc[] = "a Nicander's hull gone and its state is %d: 9, it has no reserve";
    uint16_t hull, reserve;
    int16_t i, took = 0;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, TURN / 2);
    hull = slot_type(0)->hull;
    reserve = (uint16_t)((uint32_t)hull * DAMAGE_RESERVE_TENTHS / 10);
    if (reserve < DAMAGE_RESERVE_LEAST)
        reserve = DAMAGE_RESERVE_LEAST;
    pt_bare();
    (void)pt_hit(0xFFFF, 0, (uint16_t)(hull + 5), 1, 0, 0);
    REQUIRE(shipAiState[0] != 9, e_dead, hull + 5, 0, 0);
    REQUIRE(damage_in_reserve(), e_flag, damage_in_reserve(), 0, 0);
    took = 5;
    for (i = 0; i < 1000 && shipAiState[0] != 9; i++) {
        (void)pt_hit(0xFFFF, 0, 5, 1, 0, 0);
        took = (int16_t)(took + 5);
    }
    REQUIRE(took <= reserve + 5, e_long, took, reserve, 0);
    REQUIRE(took >= reserve - 5, e_short, took, reserve, 0);
    SHIELD(1, 0) = SHIELD(1, 1) = 0;
    for (i = 0; i < 4; i++)
        ARMOR(1, i) = 0;
    (void)pt_hit(0xFFFF, 1, 30000, 1, 0, 0);   /* (past its hull many times over: its roulette, M8.49, spends a fifth on the hull) */
    REQUIRE(shipAiState[1] == 9, e_npc, shipAiState[1], 0, 0);
}

/* M12.2: on the reserve every hit breaks something */
static void t_reserve_breaks(void)
{
    static const char __far e_none[] = "hit %d on the reserve broke nothing: every one does";
    int16_t i, k;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, TURN / 2);
    pt_bare();
    (void)pt_hit(0xFFFF, 0, slot_type(0)->hull, 1, 0, 0);
    for (i = 0; i < 8; i++) {
        int16_t was = 0, now = 0;
        for (k = 0; k < SYS_COUNT; k++)
            was = (int16_t)(was + sys_level(k));
        (void)pt_hit(0xFFFF, 0, 2, 1, 0, 0);
        for (k = 0; k < SYS_COUNT; k++)
            now = (int16_t)(now + sys_level(k));
        REQUIRE(now > was, e_none, i, 0, 0);
    }
}

/* M12.3: where a hit lands decides what it breaks */
static void t_where(void)
{
    static const char __far e_rear[] = "%d hits on the rear broke the guns %d times and the VDUs %d: nothing of those is aft";
    static const char __far e_aft[] = "%d hits on the rear broke the engine %d and the tank %d times: they are what is aft";
    static const char __far e_front[] = "hits on the front broke the engine %d times and the tank %d: they are aft";
    static const char __far e_guns[] = "%d hits on the front broke the guns %d times and the radar %d: they are forward";
    static const char __far e_left[] = "hits on the left broke the right VDU %d times";
    static const char __far e_right[] = "hits on the right broke the left VDU %d times";
    int16_t side, i, tally[4][SYS_COUNT];
    static const int16_t f[4] = { 2, -2, 0, 0 }, r[4] = { 0, 0, -2, 2 };
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, TURN / 2);
    memset(tally, 0, sizeof tally);
    for (side = 0; side < 4; side++)
        for (i = 0; i < 300; i++) {
            int16_t k, lv;
            damage_reset();
            pt_bare();
            damagePoints[0] = 0;
            (void)pt_hit(0xFFFF, 0, 40, f[side], r[side], 0);
            while (damage_event(&k, &lv))
                tally[side][k]++;
        }
    REQUIRE(!tally[1][SYS_GUNS] && !tally[1][SYS_VDU_L] && !tally[1][SYS_VDU_R], e_rear, 300, tally[1][SYS_GUNS],
            tally[1][SYS_VDU_L] + tally[1][SYS_VDU_R]);
    REQUIRE(tally[1][SYS_ENGINE] > 60 && tally[1][SYS_FUEL] > 60, e_aft, 300, tally[1][SYS_ENGINE], tally[1][SYS_FUEL]);
    REQUIRE(!tally[0][SYS_ENGINE] && !tally[0][SYS_FUEL], e_front, tally[0][SYS_ENGINE], tally[0][SYS_FUEL], 0);
    REQUIRE(tally[0][SYS_GUNS] > 60 && tally[0][SYS_RADAR] > 40, e_guns, 300, tally[0][SYS_GUNS], tally[0][SYS_RADAR]);
    REQUIRE(!tally[2][SYS_VDU_R], e_left, tally[2][SYS_VDU_R], 0, 0);
    REQUIRE(!tally[3][SYS_VDU_L], e_right, tally[3][SYS_VDU_L], 0, 0);
}

/* M12.4: the engine: less top speed, less acceleration, no burner wrecked, and it comes back with the level */
static void t_engine(void)
{
    static const char __far e_top[] = "the engine at level %d: %d kps at full throttle, its share of the type's %d is at most %d";
    static const char __far e_whole[] = "the engine whole again: %d kps of %d";
    static const char __far e_burner[] = "the engine wrecked and the afterburner still lights";
    static const char __far e_burn2[] = "the engine whole and the afterburner refused";
    static const uint8_t pct[5] = { 100, 85, 65, 45, 25 };
    int16_t lv, most;
    pt_alone(PT_PLAYER);
    most = (int16_t)slot_type(0)->max_kps;
    setSpeed[0] = most;
    pt_no_repairs();
    for (lv = 1; lv <= SYS_WRECKED; lv++) {
        int16_t cap = (int16_t)((int32_t)most * pct[lv] / 100);
        sys_set(SYS_ENGINE, lv);
        if (!pt_steps(5 * SEC))
            return;
        REQUIRE((int32_t)speedLo[0] <= cap, e_top, lv, speedLo[0], most);
    }
    REQUIRE(!damage_burner_ok(), e_burner, 0, 0, 0);
    sys_set(SYS_ENGINE, 0);
    REQUIRE(damage_burner_ok(), e_burn2, 0, 0, 0);
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE((int32_t)speedLo[0] == most, e_whole, speedLo[0], most, 0);
}

/* M12.5: the tank: holds less, leaks, and wrecked is empty with no burner */
static void t_tank(void)
{
    static const char __far e_cap[] = "the tank at level 2 holds %d of 1000: 55%% at most";
    static const char __far e_leak[] = "the tank at level 2 leaked from %d to %d in 10 s";
    static const char __far e_gone[] = "the tank wrecked and %d of fuel in it";
    static const char __far e_burn[] = "the tank wrecked and the afterburner still lights";
    int16_t was;
    pt_alone(PT_PLAYER);
    pt_no_repairs();
    g_sys.fuel = 1000;
    sys_set(SYS_FUEL, 2);
    if (!pt_steps(1))
        return;
    REQUIRE(g_sys.fuel <= 550, e_cap, g_sys.fuel, 0, 0);
    was = g_sys.fuel;
    if (!pt_steps(10 * SEC))
        return;
    REQUIRE(g_sys.fuel < was, e_leak, was, g_sys.fuel, 0);
    sys_set(SYS_FUEL, SYS_WRECKED);
    if (!pt_steps(1))
        return;
    REQUIRE(g_sys.fuel == 0, e_gone, g_sys.fuel, 0, 0);
    REQUIRE(!damage_burner_ok(), e_burn, 0, 0, 0);
}

/* M12.6: the shield generator hurt holds less: what is over its most goes at once */
static void t_shield_cap(void)
{
    static const char __far e_cap[] = "the generator at level 2: the front shield holds %d of %d: half at most";
    static const char __far e_back[] = "the generator whole again and the shield came back to %d of %d in 60 s";
    uint16_t most;
    pt_alone(PT_PLAYER);
    pt_no_repairs();
    most = SHIELD_MAX(0, 0);
    sys_set(SYS_SHIELDS, 2);
    if (!pt_steps(2))
        return;
    REQUIRE((uint32_t)SHIELD(0, 0) * 2 <= most, e_cap, SHIELD(0, 0), most, 0);
    sys_set(SYS_SHIELDS, 0);
    if (!pt_steps(60 * SEC))
        return;
    REQUIRE(SHIELD(0, 0) == most, e_back, SHIELD(0, 0), most, 0);
}

/* M12.7: a hit on a side takes that side's gun first, and a destroyed launcher fires no more */
static void t_mounts(void)
{
    static const char __far e_side[] = "a hit on the right took the gun mounted at %d when one is at %d: the side's own goes first";
    static const char __far e_none[] = "%d hits on the right and the guns never broke";
    static const char __far e_rack[] = "every launcher destroyed and logic_launcher still gives %d";
    int16_t i, g, most_right = -127, taken = -127;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, TURN / 2);
    for (g = 0; g < GUN_COUNT(0); g++)
        if (pt_is_gun(0, g) && GUN_MOUNT_X(0, g) > most_right)
            most_right = GUN_MOUNT_X(0, g);
    for (i = 0; i < 800 && !systemDamage[SYS_GUNS]; i++) {
        pt_bare();
        damagePoints[0] = 0;
        (void)pt_hit(0xFFFF, 0, 20, 0, 2, 0);
    }
    REQUIRE(systemDamage[SYS_GUNS], e_none, i, 0, 0);
    for (g = 0; g < GUN_COUNT(0); g++)
        if (pt_is_gun(0, g) && (GUN_STATE(0, g) & GUN_DESTROYED))
            taken = GUN_MOUNT_X(0, g);
    REQUIRE(taken == most_right, e_side, taken, most_right, 0);
    for (g = 0; g < GUN_COUNT(0); g++)
        if (!pt_is_gun(0, g))
            GUN_STATE(0, g) |= GUN_DESTROYED;
    REQUIRE(logic_launcher() < 0, e_rack, logic_launcher(), 0, 0);
}

/* M12.8: the thrusters: slower turns, and badly hurt the nose pulls by itself */
static void t_thrusters(void)
{
    static const char __far e_slow[] = "the nose swung %ld with the thrusters wrecked, %ld whole, in the same time: slower";
    static const char __far e_steady[] = "hands off with the thrusters whole and the nose moved by %ld";
    static const char __far e_pull[] = "hands off with the thrusters heavy and the nose stayed put";
    RvVec3 start;
    int32_t whole, hurt;
    pt_alone(PT_PLAYER);
    start = gOrientationFrontVector[0];
    g_pilot.steer_x = 9;
    if (!pt_steps(SEC / 3))
        return;
    whole = labs(gOrientationFrontVector[0].x - start.x);
    pt_alone(PT_PLAYER);
    pt_no_repairs();
    sys_set(SYS_THRUSTERS, SYS_WRECKED);
    start = gOrientationFrontVector[0];
    g_pilot.steer_x = 9;
    if (!pt_steps(SEC / 3))
        return;
    hurt = labs(gOrientationFrontVector[0].x - start.x);
    REQUIRE(hurt < whole, e_slow, hurt, whole, 0);
    pt_alone(PT_PLAYER);
    start = gOrientationFrontVector[0];
    g_pilot.steer_x = 0;
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE(gOrientationFrontVector[0].x == start.x, e_steady, gOrientationFrontVector[0].x - start.x, 0, 0);
    pt_no_repairs();
    sys_set(SYS_THRUSTERS, 3);
    if (!pt_steps(3 * SEC))
        return;
    REQUIRE(gOrientationFrontVector[0].x != start.x, e_pull, 0, 0, 0);
}

/* M12.9: the repair system mends a point every 20 s, even a wrecked system; not the absorbers, the tracking or a missing weapon; and
 * hurt at all, nothing */
static void t_repair(void)
{
    static const char __far e_mend[] = "the radar at level 2 after %d s is at %d";
    static const char __far e_never[] = "system %d came back from level %d to %d: the repair system cannot mend it";
    static const char __far e_wrecked[] = "a wrecked engine after 21 s is at level %d: it mends too";
    static const char __far e_random[] = "after 41 s the two hurt systems have lost %d points: a point every 20 s, whichever";
    static const char __far e_both[] = "over 16 repairs the repair system chose the radar %d times: it chooses at random";
    static const char __far e_none[] = "the repair system at level %d and the radar mended to level %d";
    static const char __far e_hull[] = "the hull's damage went from %d to %d with the repair system at work: it never mends the hull";
    static const char __far e_armour[] = "armour %d went from %d to %d with the repair system at work: it never mends armour";
    static const int16_t never[5] = { SYS_ABSORBERS, SYS_TRACK, SYS_GUNS, SYS_RACKS, SYS_DECOYS };
    uint16_t hull, armour[4];
    int16_t k, lv, chose = 0;
    pt_alone(PT_PLAYER);
    sys_set(SYS_RADAR, 2);
    for (k = 0; k < 5; k++)
        sys_set(never[k], (int16_t)(k % 2 + 2));
    damagePoints[0] = 10;
    hull = damagePoints[0];
    for (k = 0; k < 4; k++) {
        ARMOR(0, k) = (uint16_t)(ARMOR(0, k) / 2);
        armour[k] = ARMOR(0, k);
    }
    if (!pt_steps(21 * SEC))
        return;
    REQUIRE(sys_level(SYS_RADAR) == 1, e_mend, 21, sys_level(SYS_RADAR), 0);
    if (!pt_steps(20 * SEC))
        return;
    REQUIRE(sys_level(SYS_RADAR) == 0, e_mend, 41, sys_level(SYS_RADAR), 0);
    if (!pt_steps(25 * SEC))
        return;
    for (k = 0; k < 5; k++)
        REQUIRE(sys_level(never[k]) == k % 2 + 2, e_never, never[k], k % 2 + 2, sys_level(never[k]));
    REQUIRE(damagePoints[0] == hull, e_hull, hull, damagePoints[0], 0);
    for (k = 0; k < 4; k++)
        REQUIRE(ARMOR(0, k) == armour[k], e_armour, k, armour[k], ARMOR(0, k));
    pt_alone(PT_PLAYER);
    sys_set(SYS_ENGINE, SYS_WRECKED);
    if (!pt_steps(21 * SEC))
        return;
    REQUIRE(sys_level(SYS_ENGINE) == SYS_WRECKED - 1, e_wrecked, sys_level(SYS_ENGINE), 0, 0);
    pt_alone(PT_PLAYER);
    sys_set(SYS_RADAR, 2);
    sys_set(SYS_NAV, 2);
    if (!pt_steps(41 * SEC))
        return;
    REQUIRE(sys_level(SYS_RADAR) + sys_level(SYS_NAV) == 2, e_random, 4 - sys_level(SYS_RADAR) - sys_level(SYS_NAV), 0, 0);
    pt_alone(PT_PLAYER);
    for (k = 0; k < 16; k++) {          /* (which, at random: one flight's dice, a repair after another) */
        sys_set(SYS_RADAR, 1);
        sys_set(SYS_NAV, 1);
        if (!pt_steps(20 * SEC))
            return;
        chose = (int16_t)(chose + !sys_level(SYS_RADAR));
    }
    REQUIRE(chose >= 3 && chose <= 13, e_both, chose, 0, 0);
    for (lv = 1; lv <= SYS_WRECKED; lv += 3) {
        pt_alone(PT_PLAYER);
        sys_set(SYS_RADAR, 2);
        sys_set(SYS_REPAIR, lv);
        if (!pt_steps(45 * SEC))
            return;
        REQUIRE(sys_level(SYS_RADAR) == 2, e_none, lv, sys_level(SYS_RADAR), 0);
    }
}

/* M12.10: a flight starts whole, whatever the last one left */
static void t_damage_reset(void)
{
    static const char __far e_level[] = "a new ship and system %d is at level %d";
    static const char __far e_res[] = "a new ship and the reserve is at %d%%";
    int16_t k;
    pt_alone(PT_PLAYER);
    pt_bare();
    (void)pt_hit(0xFFFF, 0, 80, 1, 0, 0);
    for (k = 0; k < SYS_COUNT; k++)
        sys_set(k, 3);
    pt_alone(PT_PLAYER);
    for (k = 0; k < SYS_COUNT; k++)
        REQUIRE(sys_level(k) == 0, e_level, k, sys_level(k), 0);
    REQUIRE(damage_reserve_pct(slot_type(0)->hull) == 100 && !damage_in_reserve(), e_res,
            damage_reserve_pct(slot_type(0)->hull), 0, 0);
}

/* M12.11: what Kit's plate and her words go by: the armour, the systems, the hull in tens, the reserve */
static void t_home_tier(void)
{
    static const char __far e_tier[] = "%d: the tier is %d, not %d";
    static const char __far e_tens[] = "%d%% of the hull gone: %d tens, not %d";
    static const char __far e_quarters[] = "the armour's quarters are %d, not %d (%d)";
    static const char __far e_lost[] = "gun %d destroyed and the guns are not hurt: %d %d";
    const RvShipType __far *t;
    int16_t g;
    pt_alone(PT_PLAYER);
    t = slot_type(0);
    REQUIRE(damage_home_tier() == HOME_INTACT, e_tier, 1, damage_home_tier(), HOME_INTACT);
    REQUIRE(damage_armour_quarters() == 0 && damage_hull_tens() == 0, e_quarters, damage_armour_quarters(), 0, damage_hull_tens());
    /* Kit's extra plate is no credit: more than the type's on a face, less on another, is still worn */
    ARMOR(0, 0) = (uint16_t)(t->armor_f * 2);
    ARMOR(0, 1) = (uint16_t)(t->armor_r - 1);
    REQUIRE(damage_home_tier() == HOME_SCARRED, e_tier, 2, damage_home_tier(), HOME_SCARRED);
    REQUIRE(damage_armour_quarters() == 1, e_quarters, damage_armour_quarters(), 1, 0);
    pt_alone(PT_PLAYER);
    sys_set(SYS_RADAR, 1);
    REQUIRE(damage_home_tier() == HOME_SCARRED, e_tier, 3, damage_home_tier(), HOME_SCARRED);
    sys_set(SYS_ENGINE, 1);
    sys_set(SYS_FUEL, 1);
    sys_set(SYS_NAV, 1);
    REQUIRE(damage_home_tier() == HOME_HOLED, e_tier, 4, damage_home_tier(), HOME_HOLED);
    pt_alone(PT_PLAYER);
    ARMOR(0, 2) = 0;
    REQUIRE(damage_home_tier() == HOME_HOLED, e_tier, 5, damage_home_tier(), HOME_HOLED);
    /* a part begun counts as a ten; past three quarters gone is wrecked */
    pt_alone(PT_PLAYER);
    damagePoints[0] = 1;
    REQUIRE(damage_hull_tens() == 1, e_tens, 1, damage_hull_tens(), 1);
    REQUIRE(damage_home_tier() == HOME_HOLED, e_tier, 6, damage_home_tier(), HOME_HOLED);
    damagePoints[0] = (uint16_t)(t->hull / 2);
    REQUIRE(damage_hull_tens() == 5 || damage_hull_tens() == 6, e_tens, 50, damage_hull_tens(), 5);
    damagePoints[0] = (uint16_t)(t->hull - 1);
    REQUIRE(damage_hull_tens() == 10, e_tens, 99, damage_hull_tens(), 10);
    REQUIRE(damage_home_tier() == HOME_WRECKED, e_tier, 7, damage_home_tier(), HOME_WRECKED);
    /* a weapon lost is its system hurt, and so never whole again */
    pt_alone(PT_PLAYER);
    REQUIRE(!damage_part_hurt(SYS_GUNS), e_lost, 0, damage_part_hurt(SYS_GUNS), 0);
    for (g = 0; g < GUN_COUNT(0); g++) {
        const RvWeapon __far *w = weapon(GUN_TYPE(0, g));
        if (w && w->kind == RV_WK_GUN) {
            GUN_STATE(0, g) |= GUN_DESTROYED;
            break;
        }
    }
    REQUIRE(damage_part_hurt(SYS_GUNS) && !damage_part_hurt(SYS_RACKS), e_lost, g, damage_part_hurt(SYS_GUNS), damage_part_hurt(SYS_RACKS));
    REQUIRE(damage_home_tier() == HOME_SCARRED, e_tier, 8, damage_home_tier(), HOME_SCARRED);
}

/* ======================================================================================================== */

/* ======================================================================================================== */
/* Shared flights (docs/multiplayer.md): the logic's part, by who flies a ship (CTL_PUPPET, RV_FLAG_HUMAN) */

/* M6.7: a collision is the machine's whose pilot is in it, or where no ship in it is another machine's */
static void t_ram_owner(void)
{
    static const char __far e_ai[] = "two AI Nicanders inside each other lost %d and %d: a pair of this machine's is "
        "hurt (the test's control)";
    static const char __far e_puppet[] = "an AI Nicander inside another machine's (a puppet) lost %d and the puppet %d: "
        "that pair is the other machine's, nothing here";
    static const char __far e_player[] = "the player flown into another machine's ship lost nothing: its own collision is "
        "worked out here (%d)";
    int32_t h1, h2;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 20000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 20150, TURN / 2);
    pt_speed(1, 450);
    h1 = pt_health(1);
    h2 = pt_health(2);
    if (!pt_step())
        return;
    REQUIRE(pt_health(1) < h1 && pt_health(2) < h2, e_ai, h1 - pt_health(1), h2 - pt_health(2), 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 20000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_PUPPET, 0, 0, 20150, TURN / 2);
    pt_speed(1, 450);
    h1 = pt_health(1);
    h2 = pt_health(2);
    if (!pt_step())
        return;
    REQUIRE(pt_health(1) == h1 && pt_health(2) == h2, e_puppet, h1 - pt_health(1), h2 - pt_health(2), 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_PUPPET, 0, 0, 150, TURN / 2);
    pt_speed(0, 450);
    h1 = pt_health(0);
    if (!pt_step())
        return;
    REQUIRE(pt_health(0) < h1, e_player, h1 - pt_health(0), 0, 0);
}

/* M7.8: a launch by another machine's ship homes on what that machine published (missileTarget[ship]) */
static void t_puppet_launch(void)
{
    static const char __far e_target[] = "a puppet's seeker, its machine's target slot 2 published, homes on %d";
    static const char __far e_own[] = "the player's seeker launched at its locked target 2 left %d published for the "
        "other machines (missileTarget[0])";
    int16_t l;
    uint16_t m;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_HOMER, SIDE_TERRAN, CTL_PUPPET, 0, 0, -1000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 18000, TURN / 2);
    pt_ship(3, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 3000, 0, 9000, TURN / 2);
    REQUIRE((l = pt_launcher(1)) >= 0, e_nolaunch, PT_HOMER, 0, 0);
    missileTarget[1] = 2;
    rvTarget[1] = 0xFF;
    g_sys.target = -1;
    m = fireGunFromShip(1, (uint16_t)l);
    REQUIRE(m <= RV_MAX_SHIP_SLOT && maybeEntityType[m] == ET_MISSILE, e_nomissile, l, m, 0);
    REQUIRE(missileTarget[m] == 2, e_target, missileTarget[m], 0, 0);
    /* the local pilot's: published before the shot (step_fire), for the layer to carry */
    pt_alone(PT_HOMER);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 3000, TURN / 2);
    missileTarget[0] = 0xFF;
    g_sys.target = 2;
    g_sys.lock = 1;
    g_sys.armed = 1;
    g_pilot.missile = 1;
    if (!pt_step())
        return;
    g_pilot.missile = 0;
    REQUIRE(missileTarget[0] == 2, e_own, missileTarget[0], 0, 0);
}

/* M8.29: a ship near another machine's pilot (RV_FLAG_HUMAN) stays in the scene, though far from this one's */
static void t_scene_human(void)
{
    static const char __far e_there[] = "no home carrier at the launch (%d)";
    static const char __far e_kept[] = "the carrier 20,000 km from the player but 2,000 from another machine's pilot was "
        "held off the scene: it stays near any human";
    static const char __far e_gone[] = "the other pilot gone far too: the carrier is still in the scene";
    int16_t home = pt_home_patrol(), z, h = 20;
    logic_scene_off = 0;
    REQUIRE(home > 0, e_there, home, 0, 0);
    z = (int16_t)TO_KM(gPositionVector[home].z);
    pt_put(0, 0, 0, (int16_t)(z + 20000), 0);
    pt_ship(h, PT_PLAYER, SIDE_TERRAN, CTL_PUPPET, 0, 0, (int16_t)(z + 2000), 0);
    rvFlags[h] |= RV_FLAG_HUMAN;
    if (!pt_step())
        return;
    REQUIRE(slot_is_ship(home), e_kept, 0, 0, 0);
    pt_put(h, 0, 0, (int16_t)(z + 30000), 0);
    if (!pt_step())
        return;
    REQUIRE(!slot_is_ship(home), e_gone, 0, 0, 0);
}

/* M8.30: a wingman another machine's pilot flew, killed, is no named pilot's death */
static void t_human_seat_lost(void)
{
    static const char __far e_none[] = "no wingman, or no pilot on it (pilot %d, %d wingmen)";
    static const char __far e_lost[] = "a human's wingman seat killed: logic_wingman_lost says pilot %d, -1 expected";
    int16_t pilot = pt_patrol(), wing[2], n, i, s;
    n = logic_wingmen(wing, 2);
    REQUIRE(pilot != RV_PILOT_NONE && n >= 1, e_none, pilot, n, 0);
    playerCanBeDamaged = 0;
    s = wing[0];
    rvController[s] = CTL_PUPPET;
    rvFlags[s] |= RV_FLAG_HUMAN;
    for (i = 0; i < 100 && shipAiState[s] != 9; i++)
        (void)pt_hit(0xFFFF, s, 2000, 2, 0, 0);
    if (!pt_step())
        return;
    REQUIRE(logic_wingman_lost() == -1, e_lost, logic_wingman_lost(), 0, 0);
}

/* M8.32: a named pilot's hull takes a hit past its armour as a share (the old game's wingmen: eight laser hits, whatever
 * their size; a big one several shares); a person's wingman seat takes the hit itself */
static int16_t pt_bare_wingman(void)
{
    int16_t wing[2], i;
    (void)pt_patrol();
    if (logic_wingmen(wing, 2) < 1 || slotMissionShip[wing[0]] >= 64
        || (missionShipTable + missionShipTable_SIZE * slotMissionShip[wing[0]])[16] >= RV_PILOTS)
        return -1;
    playerCanBeDamaged = 0;
    SHIELD(wing[0], 0) = SHIELD(wing[0], 1) = 0;
    for (i = 0; i < 4; i++)
        ARMOR(wing[0], i) = 0;
    return wing[0];
}

static void t_named_hull(void)
{
    static const char __far e_none[] = "no wingman with a named pilot in the patrol (slot %d)";
    static const char __far e_named[] = "a named wingman, bare to its hull (%d), died after %d hits of 14: %d, a share each";
    static const char __far e_big[] = "a hit of 400 on a named wingman's bare hull left it alive (state %d, hull taken %d of "
        "%d): ten shares, more than the hull";
    static const char __far e_human[] = "a person's wingman seat, bare to its hull (%d), died after %d hits of 14: %d, the "
        "hit itself";
    int16_t s, i;
    uint16_t hull, share;
    if ((s = pt_bare_wingman()) < 0) {
        REQUIRE(0, e_none, s, 0, 0);
    }
    hull = slot_type(s)->hull;
    share = (uint16_t)((hull + 7) / 8);
    for (i = 0; i < 100 && shipAiState[s] != 9; i++)
        (void)pt_hit(0xFFFF, s, 14, 2, 0, 0);
    REQUIRE(i == (int16_t)((hull + share - 1) / share), e_named, hull, i, (hull + share - 1) / share);
    s = pt_bare_wingman();
    (void)pt_hit(0xFFFF, s, 400, 2, 0, 0);
    REQUIRE(shipAiState[s] == 9, e_big, shipAiState[s], damagePoints[s], hull);
    s = pt_bare_wingman();
    rvController[s] = CTL_PUPPET;
    rvFlags[s] |= RV_FLAG_HUMAN;
    for (i = 0; i < 100 && shipAiState[s] != 9; i++)
        (void)pt_hit(0xFFFF, s, 14, 2, 0, 0);
    REQUIRE(i == (int16_t)((hull + 13) / 14), e_human, hull, i, (hull + 13) / 14);
}

/* M8.31: delayedDespawn starts a ship's end (the layer's kill on another machine), and the player's own kills are its
 * splashes */
static void t_dying(void)
{
    static const char __far e_start[] = "delayedDespawn on a live Nicander: shipAiState %d, a ship %d: dying (9), still a "
        "ship";
    static const char __far e_end[] = "the Nicander still a ship %d steps after delayedDespawn: gone after its explosion";
    static const char __far e_splash[] = "the player's own hull killed a Nicander and an AI one another: %d splashes, 1";
    int16_t i;
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_AI, 0, 0, 20000, 0);
    delayedDespawn(0xFFFF, 1);
    REQUIRE(shipAiState[1] == 9 && slot_is_ship(1), e_start, shipAiState[1], slot_is_ship(1), 0);
    for (i = 0; i < 2 * SEC && slot_is_ship(1); i++)
        if (!pt_step())
            return;
    REQUIRE(!slot_is_ship(1), e_end, i, 0, 0);
    pt_alone(PT_PLAYER);
    pt_ship(1, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 20000, 0);
    pt_ship(2, PT_ENEMY, SIDE_AERA, CTL_FROZEN, 0, 0, 25000, 0);
    pt_ship(3, PT_HOMER, SIDE_TERRAN, CTL_AI, 0, 0, 30000, 0);
    for (i = 0; i < 100 && shipAiState[1] != 9; i++)
        (void)pt_hit(0, 1, 2000, 2, 0, 0);
    for (i = 0; i < 100 && shipAiState[2] != 9; i++)
        (void)pt_hit(3, 2, 2000, 2, 0, 0);
    REQUIRE(logic_splashes == 1, e_splash, logic_splashes, 0, 0);
}

static const char __far n_m1_1[] = "M1.1 coast (step_motion): a ship flies along its nose at its speed, position += "
    "velocity";
static const char __far n_m1_2[] = "M1.2 throttle (step_motion): the speed builds to the throttle at the type's accel, "
    "and falls to 0";
static const char __far n_m1_3[] = "M1.3 top-speed (step_motion): no faster than max_kps without the afterburner";
static const char __far n_m1_4[] = "M1.4 yaw (step_minds, step_motion): steer_x turns the nose right at yaw_dps";
static const char __far n_m1_5[] = "M1.5 pitch (step_minds, step_motion): steer_y pulls the nose up at pitch_dps";
static const char __far n_m1_6[] = "M1.6 roll (step_minds, step_motion): roll lifts the right wing at roll_dps";
static const char __far n_m1_7[] = "M1.7 steer-steps (step_minds): a steer of 3 turns a third as fast as 9";
static const char __far n_m1_8[] = "M1.8 aerobatics (step_motion): 20 s of every turn keeps the frame square";
static const char __far n_m1_9[] = "M1.9 afterburner (step_motion): past max_kps toward ab_kps while lit, back after";
static const char __far n_m1_10[] = "M1.10 puppet (step_minds, step_motion): CTL_PUPPET only coasts";
static const char __far n_m1_11[] = "M1.11 frozen (step_minds, step_motion): CTL_FROZEN stays put";
static const char __far n_m1_12[] = "M1.12 turn-steps (step_minds, step_motion): every step of a full yaw, pitch and roll "
    "turns its share, none backwards or twice it";
static const char __far n_m1_13[] = "M1.13 turn-back (step_minds, step_motion): a steer of -9 turns each axis the other "
    "way, as fast";
static const char __far n_m1_14[] = "M1.14 there-and-back (step_motion): a second of each turn and a second back leaves "
    "the frame where it began";
static const char __far n_m1_15[] = "M1.15 slow-turns (step_minds, step_motion): under a unit a step (a capital ship, a "
    "steer of 1) still turns at its rate";
static const char __far n_m1_16[] = "M1.16 body-axes (step_motion): rolled a quarter turn, yaw and pitch turn about the "
    "ship's own axes";
static const char __far n_m1_17[] = "M1.17 two-axes (step_motion): yaw and pitch at once each turn at their own rate";
static const char __far n_m1_18[] = "M1.18 steady (step_motion): centred controls hold the frame still for 20 s after a "
    "turn";
static const char __far n_m1_19[] = "M1.19 turn-flight (step_motion): turning hard below the top speed, it flies where it "
    "points";
static const char __far n_m1_20[] = "M1.20 respawn (step_motion): a ship spawned into a used slot turns as one in a fresh "
    "slot";
static const char __far n_m1_21[] = "M1.21 full-house (ship_spawn, step_motion): a ship in every slot 1 .. RV_MAX_SHIP_SLOT, "
    "all flying";
static const char __far n_m1_22[] = "M1.22 slot-limit (ship_spawn, outerSpawnShipEntity): no ship past the ship slots, "
    "none when they are full";
static const char __far n_m2_1[] = "M2.1 fire-call (fireGunFromShip): one shot from the gun's mount, along the nose, "
    "at the weapon's speed";
static const char __far n_m2_2[] = "M2.2 never-refuses (fireGunFromShip): it fires with no energy and cooling";
static const char __far n_m2_3[] = "M2.3 cooldown (fireGunFromShip, step_timers): GUN_STATE counts the refire down";
static const char __far n_m2_4[] = "M2.4 energy (fireGunFromShip, step_timers): a shot costs gun energy, which "
    "recharges";
static const char __far n_m2_5[] = "M2.5 trigger (step_fire): the trigger fires every gun at its refire";
static const char __far n_m2_6[] = "M2.6 safety (step_fire): nothing fires with the master arm off";
static const char __far n_m2_7[] = "M2.7 broken-gun (step_fire, maybe_fire_all_guns): a destroyed gun stays quiet";
static const char __far n_m2_8[] = "M2.8 all-guns (maybe_fire_all_guns): every ready gun once, no missiles";
static const char __far n_m2_9[] = "M2.9 range (step_shots): shots end at their range";
static const char __far n_m2_10[] = "M2.10 launcher-rounds (step_timers): a launcher's rounds don't run down with time";
static const char __far n_m2_11[] = "M2.11 destroyed-bit (fireGunFromShip): firing keeps GUN_STATE's bit 7";
static const char __far n_m2_12[] = "M2.12 energy-gate (maybe_fire_all_guns): linked guns fire when the energy covers "
    "them all, not part of them";
static const char __far n_m2_13[] = "M2.13 muzzles (fireGunFromShip): a turned ship fires from its own mounts and "
    "nose, along its nose";
static const char __far n_m2_14[] = "M2.14 turrets (maybe_fire_all_guns, step_timers): a capital ship's turrets fire "
    "and cool as guns do";
static const char __far n_m2_15[] = "M2.15 shots-full (fireGunFromShip): a fire sets the cooldown and spends the "
    "energy even when the shot list is full";
static const char __far n_m2_16[] = "M2.16 volleys (step_fire, maybe_fire_all_guns): a held trigger fires the "
    "player's linked guns together, never lopsided";
static const char __far n_m2_17[] = "M2.17 link-fire (step_fire, maybe_fire_all_guns): four linked guns fire as four, "
    "and as three with one destroyed";
static const char __far n_m2_18[] = "M2.18 syncopated (maybe_fire_all_guns): a linked gun still cooling sits out, fires "
    "alone as it cools, and the group comes back together";
static const char __far n_m3_1[] = "M3.1 shield-front (do_damage): a front hit takes the front shield";
static const char __far n_m3_2[] = "M3.2 shield-rear (do_damage): a hit from behind takes the rear shield";
static const char __far n_m3_3[] = "M3.3 armour (do_damage): with the shields down, the armour on that side";
static const char __far n_m3_4[] = "M3.4 armour-sides (do_damage): left hits the left armour, right the right";
static const char __far n_m3_5[] = "M3.5 hull (do_damage): through the armour, the hull (damagePoints)";
static const char __far n_m3_6[] = "M3.6 big-hit (do_damage, step_deaths): a hit past the hull kills, nothing wraps";
static const char __far n_m3_7[] = "M3.7 no-wrap (do_damage): many small hits, nothing below 0, the dead stay dead";
static const char __far n_m3_8[] = "M3.8 player-safe (do_damage): playerCanBeDamaged 0 protects slot 0";
static const char __far n_m3_9[] = "M3.9 invulnerable (do_damage): rvFlags 1 protects a ship";
static const char __far n_m3_10[] = "M3.10 any-source (do_damage): the same hit does the same whatever src";
static const char __far n_m3_11[] = "M3.11 shots-hit (step_shots): gunfire hurts what it meets, src a bolt of the "
    "shooter's";
static const char __far n_m3_12[] = "M3.12 recharge (step_timers): shields come back, armour does not";
static const char __far n_m2_19[] = "M2.19 sustain (step_timers): the trigger held 10 s fires the old game's 45 shots (a volley of two "
    "every 0.44 s, at its 15 frames a second) and keeps its energy";
static const char __far n_m3_13[] = "M3.13 type-rates (step_timers): gun energy and shields come back at the type's rates (a point a side every shield_steps, or the shift)";
static const char __far n_m3_14[] = "M3.14 hit-volumes (shot_hit): a bolt within the radius, in the volume box, through the silhouettes, as the rule says";
static const char __far n_m3_15[] = "M3.15 damage-grows (shot_hit): a bolt hurts by its life left: the laser 10 at the muzzle, 25 at the end of its reach";
static const char __far n_m2_20[] = "M2.20 forward-only (gun_fire): a shot carries the ship's speed along the nose, nothing of its slide";
static const char __far n_m12_1[] = "M12.1 reserve (damage_reserve_take): past the hull the player lives on 1.7 hulls more; nobody else does";
static const char __far n_m12_2[] = "M12.2 reserve-breaks (damage_hurt): on the reserve every hit breaks a system";
static const char __far n_m12_3[] = "M12.3 where (damage_hurt): the rear breaks the engine and tank not the guns, the front the reverse, a side not the other's VDU";
static const char __far n_m12_4[] = "M12.4 engine (damage_kps): less top speed by its level, no burner wrecked, back when whole";
static const char __far n_m12_5[] = "M12.5 tank (damage_step): holds less, leaks, wrecked is empty with no burner";
static const char __far n_m12_6[] = "M12.6 shield-cap (damage_shield_cap): the generator hurt holds less, whole it fills again";
static const char __far n_m12_7[] = "M12.7 mounts (damage_hurt): a hit on a side takes that side's gun first; a destroyed launcher fires no more";
static const char __far n_m12_8[] = "M12.8 thrusters (damage_turn_rate, damage_pull): slower turns, and heavy the nose pulls by itself";
static const char __far n_m12_9[] = "M12.9 repair (damage_step): a point every 20 s at random, wrecked too; never absorbers, tracking, missing weapons, hull or armour; none itself hurt";
static const char __far n_m12_11[] = "M12.11 home-tier (damage_home_tier): what Kit's plate and words go by: armour, systems, the hull in tens, a weapon lost";
static const char __far n_m12_10[] = "M12.10 damage-reset (damage_reset): a new ship is whole with its reserve full";
static const char __far n_m4_1[] = "M4.1 kill (step_deaths, despawn): dying (9), exploding a while, then gone";
static const char __far n_m4_2[] = "M4.2 dying-quiet (step_fire, AI): a dying ship fires nothing";
static const char __far n_m4_3[] = "M4.3 player-dies (step_deaths, logic_end): missionStatus 4, the mission lost";
static const char __far n_m5_1[] = "M5.1 ai-thunks (step_minds): AI fighters think through aiFighterThink, capital "
    "ships through aiShipThink";
static const char __far n_m5_2[] = "M5.2 ai-turns (aiFighterThink): an enemy turns to face the player";
static const char __far n_m5_3[] = "M5.3 ai-closes (aiFighterThink): an enemy closes in";
static const char __far n_m5_4[] = "M5.4 ai-fires (aiFighterThink, step_fire): an enemy shoots the player";
static const char __far n_m5_5[] = "M5.5 ai-aims (aiFighterThink): it turns to bring its guns to bear before it fires";
static const char __far n_m5_6[] = "M5.6 ai-limits (aiFighterThink, step_motion): the AI turns and flies within its "
    "type";
static const char __far n_m5_7[] = "M5.7 wingman-fights (aiFighterThink): a wingman attacks the enemy";
static const char __far n_m5_8[] = "M5.8 formation (aiFighterThink): a wingman keeps with its leader";
static const char __far n_m5_9[] = "M5.9 no-left (aiFighterThink): a profile that cannot turn left rolls and pulls instead";
static const char __far n_m5_10[] = "M5.10 waiter (aiFighterThink): a profile that lies in wait comes when the player "
    "looks away";
static const char __far n_m5_11[] = "M5.11 old-straight (aiFighterThink): against a player flying straight, the line "
    "fighter keeps the old game's measure: shots from 1,500-6,000 km, a volley at a time, passes clear, round in 15 s";
static const char __far n_m5_12[] = "M5.12 old-hunt (aiFighterThink): hunted by the player, it passes many times, bumps "
    "once at most, comes round in time, fires a volley at a time";
static const char __far n_m5_13[] = "M5.13 hold-fire (holds_fire): the player's selected target holds its guns with the player within 37 degrees ahead";
static const char __far n_m5_14[] = "M5.14 cone-fire (aim_and_fire): under aim=cone the guns go within the weapon's cone, 13 of the old game's frames apart";
static const char __far n_m5_15[] = "M5.15 leave (fear_step, leave_step): a scared fighter tailed leaves and is removed beyond 16,000 km";
static const char __far n_m5_16[] = "M5.16 home (T_HOME): with its side's capital ship about it runs there instead and fights on";
static const char __far n_m5_17[] = "M5.17 hold-missiles (aim_and_fire): the held fire withholds the missiles too";
static const char __far n_m5_18[] = "M5.18 dodge-fire (missile_ahead): a missile homing on it from ahead, it fires whatever the hold";
static const char __far n_m5_21[] = "M5.21 raid (raid_target): one enemy fighter goes for the player's escort target, the next when it dies; a taunt turns it";
static const char __far n_m5_20[] = "M5.20 easy-hold (g_easy): the held fire only under the easy mode; a selected Shizong fires on a still player";
static const char __far n_m5_19[] = "M5.19 approach-silent (T_APPROACH): under aim=cone the approach has no guns; the tail from behind has";
static const char __far n_m6_1[] = "M6.1 ram (step_collisions): ships inside one another are pushed apart, both hurt";
static const char __far n_m6_2[] = "M6.2 no-ghosts (step_collisions): a ship flown at another does not pass through";
static const char __far n_m6_3[] = "M6.3 ram-cost (ram_damage): the old rule's square of the closing speed, halved; the soft rule's dozen";
static const char __far n_m7_1[] = "M7.1 launch (fireGunFromShip): a launcher puts a missile in a ship slot";
static const char __far n_m7_2[] = "M7.2 missile-key (step_fire): Enter launches one, as many as the rounds";
static const char __far n_m7_3[] = "M7.3 missile-flies (step_missiles): a dumbfire flies straight at its speed";
static const char __far n_m7_4[] = "M7.4 missile-hits (step_missiles): a dumbfire hurts the ship ahead and ends";
static const char __far n_m7_5[] = "M7.5 missile-homes (step_missiles): a friend-or-foe turns onto its target";
static const char __far n_m7_6[] = "M7.6 missile-ends (step_missiles): a missile ends at its range";
static const char __far n_m7_7[] = "M7.7 dumbfire-aims (dumbfire_aim): a dumbfire sets off toward the target within 15 degrees of the nose, else straight";
static const char __far n_m8_1[] = "M8.1 start (logic_start): the player in slot 0, the wingman, no enemies yet";
static const char __far n_m8_2[] = "M8.2 nav1 (step_mission, enterNavPoint): nav 1's ships come when the player does";
static const char __far n_m8_3[] = "M8.3 nav-once (step_mission): and only once";
static const char __far n_m8_4[] = "M8.4 won (step_mission, logic_end): every nav flown and every enemy killed: "
    "victoryPoints nonzero";
static const char __far n_m8_5[] = "M8.5 skipped (logic_start, logic_end): landing without the patrol: lost";
static const char __far n_m8_6[] = "M8.6 enemies-left (logic_end): the navs flown, the enemies alive: lost";
static const char __far n_m8_7[] = "M8.7 layer-spawn (outerSpawnShipEntity, plumbing): a mission ship at its nav point";
static const char __far n_m8_8[] = "M8.8 autopilot (logic_autopilot): on to the next nav point, never with enemies "
    "near";
static const char __far n_m8_9[] = "M8.9 fighter (camp_load_units): the mission's fighter line puts the player and the wingman "
    "in that ship (the old campaign's Hornet, Raptor, ...)";
static const char __far n_m8_10[] = "M8.10 escort-out (route_step): an escort target out sets off for the next nav point, "
    "comes along on the autopilot, and jumps out there: not a loss";
static const char __far n_m8_11[] = "M8.11 escort-lost (mission_done): the Ox killed is the mission lost, whatever else was done";
static const char __far n_m8_12[] = "M8.12 escort-along (logic_autopilot): the autopilot brings the Ox, 800 km behind the "
    "player, and stops at the nav point's reach";
static const char __far n_m8_13[] = "M8.13 home-carrier (camp_load_units, ship_think): the Vigilance behind the launch, holding station, T finds it in front";
static const char __far n_m8_14[] = "M8.14 landing-rule (logic_request_landing): none at the launch or too far; a nav point flown or the mission finished; a clearance stands";
static const char __far n_m8_17[] = "M8.17 turrets-off (ship_think): a mission's turrets=off keeps a capital ship's turrets silent";
static const char __far n_m8_18[] = "M8.18 attack-capital (view_of): fighters attacking a capital ship break off by its hull and few fly into it";
static const char __far n_m8_19[] = "M8.19 autopilot-apart (logic_autopilot): the wingman coming down on the Ox's spot arrives apart, and nobody hits it";
static const char __far n_m6_4[] = "M6.4 capital-map (shot_hit): a capital ship is hit on its actual shape under any rule, not its radius";
static const char __far n_m6_5[] = "M6.5 capital-ram (collide_ships): a fighter against a capital ship is its silhouettes under every hit= and ram= setting";
static const char __far n_m6_7[] = "M6.7 ram-owner (ram): a collision is worked out where its pilot is, or where no ship in it is another machine's";
static const char __far n_m7_8[] = "M7.8 puppet-launch (missile_launch, step_fire): a launch by another machine's ship homes on the target that machine published";
static const char __far n_m8_29[] = "M8.29 scene-human (step_scene): a ship near another machine's pilot stays in the scene";
static const char __far n_m8_30[] = "M8.30 human-seat-lost (logic_wingman_lost): a wingman a person flew, killed, is no named pilot's death";
static const char __far n_m8_33[] = "M8.33 escort-jump (route_step): an escort target out with jump=2 goes with the player to nav 1, on to nav 2, and jumps only there";
static const char __far n_m8_34[] = "M8.34 autopilot-wingman (logic_autopilot): the mission's wingman comes along however far it had gone";
static const char __far n_m8_35[] = "M8.35 autopilot-formup (logic_form_up): the flyby closes the wingman up on the player, its way and velocity the player's, kept through the jump";
static const char __far n_m8_37[] = "M8.37 autopilot-human (logic_form_up): a human's wingman is put in its formation place as the flyby begins, the player's way and speed";
static const char __far n_m8_36[] = "M8.36 escort-clear (route_step, logic_autopilot): an escort target out bears straight for its nav point and jumps only there; the autopilot leaves it on its way";
static const char __far n_m8_44[] = "M8.44 capital-flak (ship_think, capital_shot, flak_burst): an Agasicles' turrets on a lone player held 2,250 km behind it hurt it at the old game's Ralari's rate, flak with a few bolts, and far less at 200 kps";
static const char __far n_m8_45[] = "M8.45 capital-hull (do_damage, hull_take): a capital ship's hull a count as the old game's: an Agasicles dies at the 90th laser hit past its armour";
static const char __far n_m8_46[] = "M8.46 landing-burner (land_near, carrier_contact): cleared but on the afterburner, no landing, the carrier says so, the hull a ram's; the burner off, it lands";
static const char __far n_m8_47[] = "M8.47 home-arrive (logic_autopilot): the autopilot home stops 4,000 km off the carrier's hull, facing it, the carrier in the scene";
static const char __far n_m8_48[] = "M8.48 stay-home (logic_autopilot, form_comes, ship_think): a fighter of ours with role=stay is left by the autopilot and keeps round the carrier; one with no role comes along";
static const char __far n_m8_49[] = "M8.49 fighter-hull (do_damage, fighter_roulette): an unnamed fighter's hull is the old game's roulette: a few laser hits from behind kill a Nicander, head on it takes over twice as many, past seven hull counts";
static const char __far n_m8_50[] = "M8.50 prey (prey_of): a Nicander ordered at the Ox (prey=OX) goes for it with the player nearer, and turns on the player once hurt under 55% health";
static const char __far n_m8_51[] = "M8.51 capital-bearings (gun_mount, ship_think): an Agasicles' flak reaches a still player behind it, abeam and ahead alike";
static const char __far n_m8_55[] = "M8.55 field-nav (nav_field, nav_stop): a waypoint inside a field is the field: the autopilot takes the player in, and the point is reached only inside it";
static const char __far n_m8_54[] = "M8.54 capital-help (capital_help): an Aera capital ship hurt through its armour by an unengaged attacker turns half its side's fighters on it; none when a fighter is already near the attacker";
static const char __far n_m8_52[] = "M8.52 mayday (hull_take, logic_on_mayday): an Ox calls mayday on 35 in 1,000 of the hits through its armour, as the old game's capital ships did; an Agasicles never";
static const char __far n_m8_53[] = "M8.53 autoland (logic_autoland_ready, logic_autoland): the trigger lands with every waypoint flown, nobody about and the carrier close; not before";
static const char __far n_m8_43[] = "M8.43 escort-skip (logic_autopilot, escort_comes): a pick past an escort's next waypoint is the autopilot's, the escort left behind to fly on alone: ENIGMA2's nav 2 picked";
static const char __far n_m8_42[] = "M8.42 escort-jump-home (logic_autopilot, route_step): ENIGMA2 by the autopilot, the Ox along to each nav point; the player straight home, the Ox still jumps";
static const char __far n_m8_41[] = "M8.41 escort-linger (route_step, logic_autopilot): ENIGMA2's Ox goes on alone from a player lingering at the launch, and the autopilot to nav 1 brings it along";
static const char __far n_m8_40[] = "M8.40 campaign-way (logic_autopilot, the missions): every mission's waypoints reached on the old game's map, the autopilot stopping at their reach";
static const char __far n_m8_39[] = "M8.39 full-map (logic_autopilot, step_scene): the autopilot stops at a nav point's reach; its ships are in the scene however far, the carrier again on the way home";
static const char __far n_m8_38[] = "M8.38 escort-home (route_step): an escort target in goes to the home carrier, stops within 150 km of its hull, its nose the carrier's way, and is home";
static const char __far n_m10_10[] = "M10.10 form-parallel (formation_step): told to form up, the wingman flies the player's way, not facing it";
static const char __far n_m8_32[] = "M8.32 named-hull (do_damage): a named pilot's hull takes a hit past its armour as a share, eight laser hits; a person's seat the hit itself";
static const char __far n_m8_31[] = "M8.31 dying (delayedDespawn, do_damage): delayedDespawn starts a ship's end; the player's kills are its splashes";
static const char __far n_m6_6[] = "M6.6 ram-caps (ram): a collision costs the player its shield and armour at most (the hull to 40% with the armour gone) and, cut, a system; a light one the old rule's; an NPC its shield and half its armour; into a capital ship over 100 kps, stopped";
static const char __far n_m8_20[] = "M8.20 jump-told (route_step, logic_on_jump): an escorted ship's jump is told once, friendly, for the star and the flash";
static const char __far n_m8_21[] = "M8.21 wingman-lost (logic_wingman_lost, camp_load_units, camp_line_ok): a lost wingman is named, the next mission solo, their lines gone";
static const char __far n_m8_22[] = "M8.22 kit-boost (kit_boost, logic_boost): talking to Kit before the launch: half as much armour again, or a round more; nothing without it";
static const char __far n_m8_23[] = "M8.23 way-out (camp_way_out): how a system comes out of the flight just flown: nothing before its last mission, then as camp_resolve has it";
static const char __far n_m8_24[] = "M8.24 nav-choice (logic_nav_next, logic_autopilot): the chosen nav point follows the way ahead until N picks one; the autopilot goes to it, home too";
static const char __far n_m8_25[] = "M8.25 scene (step_scene): past 16,000 km the carrier is out of the scene and cannot be targeted; it is back as it was when the player is";
static const char __far n_m8_27[] = "M8.27 second-wave (wave_may_come): a hidden point's enemies wait for the enemy fighters out, come when the last is killed, are called off when it ran";
static const char __far n_m8_28[] = "M8.28 wingman-behind (logic_start): enemies at the launch: the wingman starts past them, none: beside the player";
static const char __far n_m8_26[] = "M8.26 way-ahead (logic_autopilot): the autopilot stops short of a field and at a hidden point on its line; hidden points are on no list and ask nothing";
static const char __far n_m8_16[] = "M8.16 carrier-lost (step_deaths, logic_end): the carrier destroyed ends the flight when its explosion has run: a loss";
static const char __far n_m8_15[] = "M8.15 landing-contact (land_near, carrier_contact, logic_end): not cleared the hull stops it unhurt; cleared it lands within 2,000 km of the hull, from any side; undone lost, done won";
static const char __far n_m9_1[] = "M9.1 gunsight (logic_hud): a mark at the view's middle";
static const char __far n_m9_2[] = "M9.2 brackets (logic_hud): marks round the target";
static const char __far n_m10_1[] = "M10.1 target-front (logic_target_next): T picks only in front of the pilot, nearest the nose first, round";
static const char __far n_m10_2[] = "M10.2 target-lock (logic_target_lock): L keeps the target through T, Y; off when it dies or the tracking is wrecked";
static const char __far n_m10_3[] = "M10.3 systems (damage_hurt, step_timers): hits to the hull hurt the systems; a wrecked generator or power plant";
static const char __far n_m10_4[] = "M10.4 weapon-choice (logic_guns_next, logic_missile_next): G picks the guns the trigger fires, W the launcher";
static const char __far n_m10_5[] = "M10.5 nav-cross (logic_hud): a white cross where the chosen nav point lies on the view";
static const char __far n_m10_6[] = "M10.6 orders (logic_order): attack my target, help me out, form on my wing, return to base";
static const char __far n_m10_7[] = "M10.7 missile-lock (lock_tick): a heat seeker from behind in its time, image recognition anyhow, none wrecked or dark, a dumbfire never";

static const char __far n_m10_8[] = "M10.8 break-formation (logic_order): fly in formation fights nobody; break formation goes for any enemy, else round the player";
static const char __far n_m11_1[] = "M11.1 sim-waves (simulator_frame): one of each Rlaan fighter type in turn, then two of the first";
static const char __far n_m11_2[] = "M11.2 sim-replenish (simulator_frame): shields, fuel, energy and missiles whole when the count goes up, not between";
static const char __far n_m11_3[] = "M11.3 sim-win (simulator_frame): 47 kills, in the round of four, ends the flight, won";
static const char __far n_m11_4[] = "M11.4 sim-death (simulator_frame): no more waves once the player is dying, and the flight ends";
static const char __far n_m10_9[] = "M10.9 taunt (logic_taunt): an enemy fighter turns on the player, however far, and holds no fire; not a friend or a capital ship";

static const PtTest __far k_tests[] = {
    { n_m1_1, t_coast, 0 }, { n_m1_2, t_throttle, 0 }, { n_m1_3, t_top_speed, 0 }, { n_m1_4, t_yaw, 0 },
    { n_m1_5, t_pitch, 0 }, { n_m1_6, t_roll, 0 }, { n_m1_7, t_steer_steps, 0 }, { n_m1_8, t_aerobatics, 0 },
    { n_m1_9, t_afterburner, 0 }, { n_m1_10, t_puppet, 0 }, { n_m1_11, t_frozen, 0 },
    { n_m1_12, t_turn_steps, 0 }, { n_m1_13, t_turn_back, 0 }, { n_m1_14, t_there_and_back, 0 },
    { n_m1_15, t_slow_turns, 0 }, { n_m1_16, t_body_axes, 0 }, { n_m1_17, t_two_axes, 0 }, { n_m1_18, t_steady, 0 },
    { n_m1_19, t_turn_flight, 0 }, { n_m1_20, t_respawn, 0 }, { n_m1_21, t_full_house, 0 },
    { n_m1_22, t_slot_limit, 0 },
    { n_m2_1, t_fire_call, 0 }, { n_m2_2, t_never_refuses, 0 }, { n_m2_3, t_cooldown, 0 }, { n_m2_4, t_energy, 0 },
    { n_m2_5, t_trigger, 0 }, { n_m2_6, t_safety, 0 }, { n_m2_7, t_broken_gun, 0 }, { n_m2_8, t_all_guns, 0 },
    { n_m2_9, t_range, 0 }, { n_m2_10, t_launcher_rounds, 0 }, { n_m2_11, t_destroyed_bit, 0 },
    { n_m2_12, t_energy_gate, 0 }, { n_m2_13, t_muzzles, 0 }, { n_m2_14, t_turrets, 0 }, { n_m2_15, t_shots_full, 0 },
    { n_m2_16, t_volleys, 0 }, { n_m2_17, t_link_fire, 0 }, { n_m2_18, t_syncopated, 0 }, { n_m2_19, t_sustain, 0 },
    { n_m2_20, t_forward_only, 0 },
    { n_m3_1, t_shield_front, 0 }, { n_m3_2, t_shield_rear, 0 }, { n_m3_3, t_armour, 0 },
    { n_m3_4, t_armour_sides, 0 }, { n_m3_5, t_hull, 0 }, { n_m3_6, t_big_hit, 0 }, { n_m3_7, t_no_wrap, 0 },
    { n_m3_8, t_player_safe, 0 }, { n_m3_9, t_invulnerable, 0 }, { n_m3_10, t_any_source, 0 },
    { n_m3_11, t_shots_hit, 0 }, { n_m3_12, t_recharge, 0 }, { n_m3_13, t_type_rates, 0 },
    { n_m3_14, t_hit_volumes, 0 }, { n_m3_15, t_damage_grows, 0 },
    { n_m4_1, t_kill, 0 }, { n_m4_2, t_dying_quiet, 0 }, { n_m4_3, t_player_dies, 0 },
    { n_m5_1, t_ai_thunks, 1 }, { n_m5_2, t_ai_turns, 0 }, { n_m5_3, t_ai_closes, 0 }, { n_m5_4, t_ai_fires, 0 },
    { n_m5_5, t_ai_aims, 1 }, { n_m5_6, t_ai_limits, 0 }, { n_m5_7, t_wingman_fights, 0 },
    { n_m5_8, t_formation, 0 }, { n_m5_9, t_no_left, 0 }, { n_m5_10, t_waiter, 0 }, { n_m5_11, t_old_straight, 0 },
    { n_m5_12, t_old_hunt, 0 },
    { n_m5_13, t_hold_fire, 0 }, { n_m5_14, t_cone_fire, 0 }, { n_m5_15, t_leave, 0 }, { n_m5_16, t_home, 0 },
    { n_m5_17, t_hold_missiles, 0 }, { n_m5_18, t_dodge_fire, 0 }, { n_m5_19, t_approach_silent, 0 }, { n_m5_20, t_easy_hold, 0 }, { n_m5_21, t_raid, 0 },
    { n_m6_1, t_ram, 0 }, { n_m6_2, t_no_ghosts, 0 }, { n_m6_3, t_ram_cost, 0 },
    { n_m7_1, t_launch, 0 }, { n_m7_2, t_missile_key, 0 }, { n_m7_3, t_missile_flies, 0 },
    { n_m7_4, t_missile_hits, 0 }, { n_m7_5, t_missile_homes, 0 }, { n_m7_6, t_missile_ends, 0 },
    { n_m8_1, t_start, 0 }, { n_m8_2, t_nav1, 0 }, { n_m8_3, t_nav_once, 0 }, { n_m8_4, t_won, 0 },
    { n_m8_5, t_skipped, 0 }, { n_m8_6, t_enemies_left, 0 }, { n_m8_7, t_layer_spawn, 0 }, { n_m8_8, t_autopilot, 0 },
    { n_m8_9, t_fighter, 0 }, { n_m8_10, t_escort_out, 0 }, { n_m8_11, t_escort_lost, 0 }, { n_m8_12, t_escort_along, 0 },
    { n_m8_27, t_second_wave, 0 }, { n_m8_28, t_wingman_behind, 0 },
    { n_m8_13, t_home_carrier, 0 }, { n_m8_14, t_landing_rule, 0 }, { n_m8_15, t_landing_contact, 0 }, { n_m8_16, t_carrier_lost, 0 }, { n_m8_17, t_turrets_off, 0 }, { n_m8_18, t_attack_capital, 0 }, { n_m8_20, t_jump_told, 0 }, { n_m8_21, t_wingman_lost, 0 }, { n_m8_22, t_kit_boost, 0 }, { n_m8_23, t_way_out, 0 }, { n_m8_24, t_nav_choice, 0 }, { n_m8_25, t_scene, 0 }, { n_m8_26, t_way_ahead, 0 }, { n_m8_19, t_autopilot_apart, 0 }, { n_m6_4, t_capital_map, 0 }, { n_m6_5, t_capital_ram, 0 },
    { n_m9_1, t_gunsight, 1 }, { n_m9_2, t_brackets, 1 },
    { n_m10_1, t_target_front, 0 }, { n_m10_2, t_target_lock, 0 }, { n_m10_3, t_systems, 0 },
    { n_m10_4, t_weapon_choice, 0 }, { n_m10_5, t_nav_cross, 1 }, { n_m10_6, t_orders, 0 }, { n_m10_7, t_missile_lock, 0 },
    { n_m10_8, t_break_formation, 0 }, { n_m10_9, t_taunt, 0 },
    { n_m12_1, t_reserve, 0 }, { n_m12_2, t_reserve_breaks, 0 }, { n_m12_3, t_where, 0 }, { n_m12_4, t_engine, 0 },
    { n_m12_5, t_tank, 0 }, { n_m12_6, t_shield_cap, 0 }, { n_m12_7, t_mounts, 0 }, { n_m12_8, t_thrusters, 0 },
    { n_m12_9, t_repair, 0 }, { n_m12_10, t_damage_reset, 0 }, { n_m12_11, t_home_tier, 0 },
    { n_m11_1, t_sim_waves, 0 }, { n_m11_2, t_sim_replenish, 0 }, { n_m11_3, t_sim_win, 0 }, { n_m11_4, t_sim_death, 0 },
    { n_m7_7, t_dumbfire_aims, 0 }, { n_m6_6, t_ram_caps, 0 },   /* (new tests at the end: a test's dice are its place in the table, and M8.6 and
                                           M8.18 pass on theirs only) */
    { n_m6_7, t_ram_owner, 0 }, { n_m7_8, t_puppet_launch, 0 }, { n_m8_29, t_scene_human, 0 },
    { n_m8_30, t_human_seat_lost, 0 }, { n_m8_31, t_dying, 0 }, { n_m8_32, t_named_hull, 0 }, { n_m8_33, t_escort_jump, 0 }, { n_m8_34, t_autopilot_far_wingman, 0 }, { n_m8_35, t_autopilot_formup, 0 }, { n_m8_36, t_escort_clear, 0 }, { n_m8_37, t_autopilot_human, 0 }, { n_m8_38, t_escort_home, 0 }, { n_m10_10, t_form_parallel, 0 }, { n_m8_39, t_full_map, 0 }, { n_m8_40, t_campaign_way, 0 }, { n_m8_41, t_escort_waits, 0 }, { n_m8_42, t_escort_jump_home, 0 }, { n_m8_43, t_escort_order, 0 }, { n_m8_44, t_capital_flak, 0 }, { n_m8_45, t_capital_hull, 0 }, { n_m8_46, t_landing_burner, 0 }, { n_m8_47, t_home_arrive, 0 }, { n_m8_48, t_stay_home, 0 }, { n_m8_49, t_fighter_roulette, 0 }, { n_m8_50, t_prey, 0 }, { n_m8_51, t_capital_bearings, 0 }, { n_m8_52, t_mayday, 0 }, { n_m8_53, t_autoland, 0 }, { n_m8_54, t_capital_help, 0 }, { n_m8_55, t_field_nav, 0 },
};

/* --- the run --- */

/* the world's hash: the scalars of play, the slot arrays, the mission tables, the shots.  (Its pointer is near: a
 * fixed-block address is a constant, and Watcom makes a constant near pointer passed as a far one segment 0, not
 * DS: the hash of the interrupt table.  docs/gameplay.md, "Watcom traps") */
static uint32_t fnv(uint32_t h, const uint8_t *p, uint16_t n)
{
    for (; n; n--)
        h = (h ^ *p++) * UINT32_C(16777619);
    return h;
}

static uint32_t pt_hash(void)
{
    uint32_t h = UINT32_C(2166136261);
    int16_t i;
    h = fnv(h, (const uint8_t *)&missionStatus, (uint16_t)((uint8_t *)&victoryPoints + 2 - (uint8_t *)&missionStatus));
    h = fnv(h, (const uint8_t *)maybeEntityType, (uint16_t)(RV_FIXED_BYTES - ((uint8_t *)maybeEntityType - RV_AT(uint8_t *, 0))));
    for (i = 0; i < shots_count(); i++) {
        const Shot __far *s = shot(i);
        int32_t w[12];
        w[0] = s->x; w[1] = s->y; w[2] = s->z; w[3] = s->vx; w[4] = s->vy; w[5] = s->vz;
        w[6] = s->hx; w[7] = s->hy; w[8] = s->hz;
        w[9] = s->weapon; w[10] = s->damage; w[11] = (int32_t)((uint32_t)s->life << 16 | (uint32_t)s->owner << 8 | s->look);
        h = fnv(h, (const uint8_t *)w, sizeof w);
    }
    return h;
}

/* the world's parts, a step's line ("steps"): where the DOS build first parts from the host's */
static void pt_parts(void)
{
    static const char __far s_step[] = " step %d ";
    static const uint16_t __far k_at[] = { 0x0600, 0x0B00, 0x1100, 0x1700, 0x2900, 0x2D00, 0x3800, 0x5280, 0x6400,
                                           0x8000 };
    static const char __far k_names[] = "kinds positions velocities frames speeds hurts guns minds mission ";
    const char __far *n = k_names;
    int16_t k;
    uint32_t h;
    ln_s(s_pre);
    ln_id();
    ln_f(s_step, frameCounter, 0, 0);
    ln_x(pt_hash());
    for (k = 0; k + 1 < (int16_t)(sizeof k_at / sizeof k_at[0]); k++) {
        h = fnv(UINT32_C(2166136261), RV_AT(const uint8_t *, 0) + k_at[k], (uint16_t)(k_at[k + 1] - k_at[k]));
        ln_c(' ');
        while (*n != ' ')
            ln_c(*n++);
        n++;
        ln_c('=');
        ln_x(h);
    }
    ln_end();
}

/* the test's id (its name to the space) matches one of `only`'s (commas or spaces between): "M2" takes M2.*,
 * "M2.3" that one */
static int16_t pt_wanted(const char __far *name, const char *only)
{
    const char *o = only;
    int16_t k;
    if (!only || !*only)
        return 1;
    while (*o) {
        while (*o == ',' || *o == ' ')
            o++;
        for (k = 0; o[k] && o[k] != ',' && o[k] != ' '; k++)
            ;
        if (k && !_fmemcmp(name, o, k) && (name[k] == ' ' || name[k] == '.' || o[k - 1] == '.'))
            return 1;
        o += k;
    }
    return 0;
}

int16_t playtest_run(const char *only)
{
    static const char __far s_pass[] = "PASS ", __far s_fail[] = "FAIL ", __far s_sp[] = " ";
#ifndef RV_HOST
    static const char __far s_skip[] = "SKIP -------- ", __far s_host[] = " (the host harness only)";
#endif
    static const char __far s_sum[] = "%d of %d pass";
    static const char __far s_ms[] = "  M%d %d/%d";
    static const char __far s_start[] = "the play tests: %d of them; each line PASS or FAIL, the world's hash, the test";
    static const char __far s_steps[] = "steps";
    int16_t i, n = (int16_t)(sizeof k_tests / sizeof k_tests[0]), ran = 0, passed = 0, m, mr[13], mp[13];
    uint32_t h;
    for (m = 0; m < 10; m++)
        mr[m] = mp[m] = 0;
    mr[10] = mp[10] = mr[11] = mp[11] = mr[12] = mp[12] = 0;
    g_steps = 0;
    if (only)
        for (i = 0; only[i]; i++)
            if ((i == 0 || only[i - 1] == ',' || only[i - 1] == ' ') && !_fmemcmp(only + i, s_steps, 5))
                g_steps = 1;
    camp_new();
    ln_s(s_pre);
    ln_f(s_start, n, 0, 0);
    ln_end();
    for (i = 0; i < n; i++) {
        if (!pt_wanted(k_tests[i].name, only))
            continue;
#ifndef RV_HOST
        if (k_tests[i].host_only) {
            ln_s(s_pre);
            ln_s(s_skip);
            ln_s(k_tests[i].name);
            ln_s(s_host);
            ln_end();
            continue;
        }
#endif
        g_index = i;
        g_name = k_tests[i].name;
        g_failed = 0;
        k_tests[i].fn();
        h = pt_hash();
        m = (int16_t)(k_tests[i].name[1] - '0');   /* (M1 .. M9, M10 .. M12: the digits before the dot) */
        if (k_tests[i].name[2] >= '0' && k_tests[i].name[2] <= '9')
            m = (int16_t)(m * 10 + k_tests[i].name[2] - '0');
        if (m > 12)
            m = 12;
        ran++;
        mr[m]++;
        if (!g_failed) {
            passed++;
            mp[m]++;
        }
        ln_s(s_pre);
        ln_s(g_failed ? s_fail : s_pass);
        ln_x(h);
        ln_s(s_sp);
        ln_s(k_tests[i].name);
        ln_end();
    }
    shots_flight_end();
    collide_flight_end();
    ln_s(s_pre);
    ln_f(s_sum, passed, ran, 0);
    for (m = 1; m < 13; m++)
        if (mr[m])
            ln_f(s_ms, m, mp[m], mr[m]);
    ln_end();
    return (int16_t)(ran - passed);
}

#ifdef PLAYTEST_MAIN
/* the host's: build/hostNN/playtest [--data DIR] [--log FILE] [IDS...] (host/build.sh builds it beside rvsim) */
#include <unistd.h>

int main(int argc, char **argv)
{
    static char only[512], dir[1024];
    const char *data = 0, *log = "playtest.log";
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)
            data = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            log = argv[++i];
        else if (!strcmp(argv[i], "--list")) {
            for (i = 0; i < (int)(sizeof k_tests / sizeof k_tests[0]); i++)
                printf("%s%s\n", k_tests[i].name, k_tests[i].host_only ? " (the host harness only)" : "");
            return 0;
        } else if (argv[i][0] == '-') {
            fputs("usage: playtest [--data DIR] [--log FILE] [--list] [ID ...]   (M2 runs M2.*, M2.3 that one;\n"
                  "       the game's log and the RULE lines go to the log, playtest.log in the data directory)\n",
                  stderr);
            return 2;
        } else if (strlen(only) + strlen(argv[i]) + 2 < sizeof only) {
            strcat(only, argv[i]);
            strcat(only, ",");
        }
    }
    if (!data) {                        /* beside the program (host/build.sh puts the data there) */
        const char *slash = strrchr(argv[0], '/');
        if (slash && (size_t)(slash - argv[0]) < sizeof dir) {
            memcpy(dir, argv[0], (size_t)(slash - argv[0]));
            data = dir;
        }
    }
    if (data && chdir(data)) {
        fprintf(stderr, "playtest: no such data directory: %s\n", data);
        return 2;
    }
    if ((host_log = fopen(log, "w")) == 0) {
        fprintf(stderr, "playtest: cannot write %s\n", log);
        return 2;
    }
    host_check_src = 1;                 /* (every do_damage tried again with src 0xFFFF: the layer's replay) */
    if (!camp_load_data("RVGAME.DAT") || !collide_reserve()) {
        fprintf(stderr, "playtest: no RVGAME.DAT or SHAPES.DAT here (--data DIR: build/host32 has them)\n");
        return 2;
    }
    i = playtest_run(only);
    xms_free_level(XMS_RESIDENT);
    fclose(host_log);
    return i ? 1 : 0;
}
#endif
