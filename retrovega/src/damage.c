/* damage.c: the player's systems (damage.h, docs/damage.md).  Everything about them is in the tables at the top, a row
 * a system, so a system's effect or how likely it is to break is one number to change.  All data is far (DGROUP). */
#include <string.h>
#include "rv.h"
#include "rvhooks.h"
#include "campaign.h"
#include "panel.h"
#include "world.h"
#include "damage.h"

#define SYS_MORE (SYS_COUNT - SYS_FIXED)
static uint8_t __far g_more[SYS_MORE];  /* (systemDamage has ten bytes: the other systems') */
static uint16_t __far g_reserve_used;
static uint8_t __far g_in_reserve;
static uint8_t __far g_pull;            /* which way the thrusters pull, once heavy: 0 left, 1 right */
static uint16_t __far g_repair_wait;    /* steps until the repair system's next point */
static uint16_t __far g_leak;           /* the tank's leak, carried between steps */
static struct { uint8_t sys, level; } __far g_events[8];
static uint8_t __far g_event_in, __far g_event_out;

/* The reserve, against the type's hull (damage.h: 1.7 times).  A fighter whose hull has gone has this much again to limp
 * home on (the user's: "an extra reservoir of health"); every hit on it breaks something */

/* a hit through to the hull breaks a system with odds left x 3 / 2 in hull, and a heavy one breaks more */
#define POINT_QTY 24                    /* a hit breaks a further system per this much */
#define POINTS_MOST 3

/* the repair system: a point off one hurt system every 20 s, chosen at random, while it is whole itself (the user's) */
#define REPAIR_STEPS (20 * 30)

/* Each system's chance of a hit on each side, in 64ths (each row of the table is a system, each column a side: front,
 * rear, left, right); and what its level does */
static const uint8_t __far k_odds[SYS_COUNT][4] = {
    /*             front rear left right */
    /* ABSORBERS */ { 2,   6,   4,   4 },
    /* POWER     */ { 2,   8,   3,   3 },
    /* SHIELDS   */ { 4,   6,   3,   3 },
    /* COMPUTER  */ { 5,   0,   3,   3 },
    /* INTERCOM  */ { 3,   0,   3,   3 },
    /* TRACK     */ { 6,   0,   2,   2 },
    /* EJECTOR   */ { 1,   1,   1,   1 },
    /* REPAIR    */ { 1,   3,   2,   2 },
    /* GUNS      */ { 11,  0,   9,   9 },
    /* ENGINE    */ { 0,  17,   2,   2 },
    /* FUEL      */ { 0,  13,   4,   4 },
    /* RADAR     */ { 8,   1,   2,   2 },
    /* VDU_L     */ { 5,   0,   9,   0 },
    /* VDU_R     */ { 5,   0,   0,   9 },
    /* NAV       */ { 4,   1,   2,   2 },
    /* THRUSTERS */ { 2,   5,   7,   7 },
    /* RACKS     */ { 1,   2,   5,   5 },
    /* IFF       */ { 3,   0,   2,   2 },
    /* DECOYS    */ { 1,   1,   3,   3 }
};
typedef char k_odds_front_is_64[sizeof k_odds == SYS_COUNT * 4 ? 1 : -1];

static const char __far k_name[SYS_COUNT][10] = {   /* (the readout's lines are short: a small VDU) */
    "ABSORBER", "POWER", "SHIELDS", "COMPUTER", "INTERCOM", "TRACKING", "EJECTOR", "REPAIR", "GUNS",
    "ENGINE", "FUEL TANK", "RADAR", "LEFT VDU", "RIGHT VDU", "NAV COMP", "THRUSTERS", "MISSILES", "IFF", "DECOYS"
};

/* the cockpit overlays each shows hurt: a bit of rvCockpitDamage from a level up, two to a system (0xFF none; the bits are
 * tools/cockpits.py's: not every kit paints every one) */
static const struct { uint8_t bit, at, bit2, at2; } __far k_overlay[SYS_COUNT] = {
    { 16, 1, 0xFF, 0 },                 /* ABSORBERS: the aux flight display */
    { 9, 1, 0xFF, 0 },                  /* POWER: power distribution (flight.c's running lights read the bit) */
    { 3, 1, 0xFF, 0 },                  /* SHIELDS: the shields' readout */
    { 27, 2, 28, 3 },                   /* COMPUTER: the engineering VDU, the sensor's */
    { 13, 1, 0xFF, 0 },                 /* INTERCOM: communications */
    { 18, 1, 22, 3 },                   /* TRACK: the target controls, the lock lamp */
    { 20, 1, 0xFF, 0 },                 /* EJECTOR: the handle */
    { 10, 2, 0xFF, 0 },                 /* REPAIR: the E/M panel */
    { 17, 1, 0xFF, 0 },                 /* GUNS: the weapon controls */
    { 8, 1, 11, 2 },                    /* ENGINE: the propulsion panel, the engine status */
    { 7, 1, 0xFF, 0 },                  /* FUEL: the fuel gauge */
    { 2, 3, 19, 3 },                    /* RADAR: the scope's glass, its controls */
    { 0, 3, 0xFF, 0 },                  /* VDU_L: its glass (heavy: below that static and lost rows, a page still to read) */
    { 1, 3, 0xFF, 0 },                  /* VDU_R: its glass */
    { 12, 1, 0xFF, 0 },                 /* NAV: the nav lamp */
    { 15, 1, 0xFF, 0 },                 /* THRUSTERS: the flight controls */
    { 17, 1, 0xFF, 0 },                 /* RACKS: the weapon controls */
    { 14, 1, 0xFF, 0 },                 /* IFF: its lamp */
    { 0xFF, 0, 0xFF, 0 }                /* DECOYS: nothing painted */
};
/* (the hull's own, damage_step: the armour and hull readouts, the canopy, the warning panel, the survival VDU) */
#define OVERLAY_ARMOR 4
#define OVERLAY_HULL 5
#define OVERLAY_SPEED 6
#define OVERLAY_WARNINGS 21
#define OVERLAY_CANOPY 23
#define OVERLAY_CANOPY_RIGHT 24
#define OVERLAY_CANOPY_TOP 25
#define OVERLAY_SURVIVAL 26

/* what each level leaves of a thing, in a hundred */
static const uint8_t __far k_engine_pct[5] = { 100, 85, 65, 45, 25 };
static const uint8_t __far k_turn_pct[5] = { 100, 90, 75, 60, 40 };
static const uint8_t __far k_shield_pct[5] = { 100, 75, 50, 25, 0 };
static const uint8_t __far k_energy_pct[5] = { 100, 85, 65, 40, 40 };   /* (wrecked it comes back at a fifth: step_timers) */
static const uint8_t __far k_fuel_pct[5] = { 100, 80, 55, 30, 0 };
static const uint8_t __far k_slide_pct[5] = { 100, 100, 80, 60, 40 };
/* the tank's leak, thousandths of its fuel a second */
static const uint8_t __far k_leak[5] = { 0, 2, 6, 15, 0 };

/* --- a system's level --- */

int16_t sys_level(int16_t k)
{
    if (k < 0 || k >= SYS_COUNT)
        return 0;
    return k < SYS_FIXED ? systemDamage[k] : g_more[k - SYS_FIXED];
}

void sys_set(int16_t k, int16_t level)
{
    if (k < 0 || k >= SYS_COUNT)
        return;
    if (k < SYS_FIXED)
        systemDamage[k] = (uint8_t)level;
    else
        g_more[k - SYS_FIXED] = (uint8_t)level;
}

const char __far *logic_system_name(int16_t k)
{
    return k >= 0 && k < SYS_COUNT ? k_name[k] : k_name[0];
}

void damage_reset(void)
{
    memset(systemDamage, 0, systemDamage_SIZE);   /* (near: a far pointer to this fixed address has segment 0) */
    _fmemset(g_more, 0, sizeof g_more);
    g_reserve_used = 0;
    g_in_reserve = 0;
    g_pull = 0;
    g_repair_wait = REPAIR_STEPS;
    g_leak = 0;
    g_event_in = g_event_out = 0;
}

/* --- the reserve --- */

static uint16_t reserve_of(uint16_t hull)
{
    uint32_t r = (uint32_t)hull * DAMAGE_RESERVE_TENTHS / 10;
    return r < DAMAGE_RESERVE_LEAST ? DAMAGE_RESERVE_LEAST : r > 60000UL ? 60000U : (uint16_t)r;
}

int16_t damage_reserve_take(uint16_t over, uint16_t hull)
{
    uint16_t most = reserve_of(hull);
    g_in_reserve = 1;
    if (over >= most - g_reserve_used) {
        g_reserve_used = most;
        return 0;
    }
    g_reserve_used = (uint16_t)(g_reserve_used + over);
    return 1;
}

int16_t damage_reserve_pct(uint16_t hull)
{
    uint16_t most = reserve_of(hull);
    return (int16_t)((uint32_t)(most - g_reserve_used) * 100 / most);
}

int16_t damage_in_reserve(void)
{
    return g_in_reserve;
}

int16_t damage_part_hurt(int16_t k)
{
    int16_t g;
    if (sys_level(k))
        return 1;
    if (k != SYS_GUNS && k != SYS_RACKS)
        return 0;
    for (g = 0; g < GUN_COUNT(0); g++) {
        const RvWeapon __far *w = weapon(GUN_TYPE(0, g));
        if (w && (GUN_STATE(0, g) & GUN_DESTROYED) && (w->kind == RV_WK_GUN || w->kind == RV_WK_TURRET) == (k == SYS_GUNS))
            return 1;
    }
    return 0;
}

int16_t damage_hull_tens(void)
{
    const RvShipType __far *t = slot_type(0);
    uint32_t gone;
    if (!t || !t->hull)
        return 0;
    if (damagePoints[0] >= t->hull)
        return 10;
    gone = (uint32_t)damagePoints[0] * 100;
    return (int16_t)((gone / t->hull + (gone % t->hull != 0) + 9) / 10);
}

/* what a type starts with on an armour face */
static uint32_t armour_of_type(const RvShipType __far *t, int16_t i)
{
    return i == 0 ? t->armor_f : i == 1 ? t->armor_r : i == 2 ? t->armor_l : t->armor_rt;
}

int16_t damage_armour_quarters(void)
{
    const RvShipType __far *t = slot_type(0);
    uint32_t most = 0, left = 0;
    int16_t i;
    if (!t)
        return 0;
    for (i = 0; i < 4; i++) {
        uint32_t m = armour_of_type(t, i), a = ARMOR(0, i);
        most += m;
        left += a < m ? a : m;          /* (Kit's extra plate is no credit against what has gone) */
    }
    if (!most || left >= most)
        return 0;
    return (int16_t)(((most - left) * 4 + most - 1) / most);
}

#define HOME_HOLED_SYSTEMS 4            /* this many systems hurt is more than scars, whatever the armour says */
int16_t damage_home_tier(void)
{
    const RvShipType __far *t = slot_type(0);
    int16_t i, hurt = 0;
    if (!t)
        return HOME_INTACT;
    if (g_in_reserve || damage_hull_tens() > 7)   /* (HULL LOW, or the reserve) */
        return HOME_WRECKED;
    if (damagePoints[0])
        return HOME_HOLED;
    for (i = 0; i < 4; i++)
        if (armour_of_type(t, i) && !ARMOR(0, i))
            return HOME_HOLED;
    for (i = 0; i < SYS_COUNT; i++)
        hurt += damage_part_hurt(i);
    if (hurt >= HOME_HOLED_SYSTEMS)
        return HOME_HOLED;
    return hurt || damage_armour_quarters() ? HOME_SCARRED : HOME_INTACT;
}

/* --- a hit --- */

static void overlays(int16_t k, int16_t level)
{
    if (k_overlay[k].bit != 0xFF && level >= k_overlay[k].at)
        rvCockpitDamage |= 1UL << k_overlay[k].bit;
    if (k_overlay[k].bit2 != 0xFF && level >= k_overlay[k].at2)
        rvCockpitDamage |= 1UL << k_overlay[k].bit2;
}

static void event(int16_t k, int16_t level)
{
    g_events[g_event_in & 7].sys = (uint8_t)k;
    g_events[g_event_in & 7].level = (uint8_t)level;
    g_event_in++;
    if ((uint8_t)(g_event_in - g_event_out) > 8)
        g_event_out = (uint8_t)(g_event_in - 8);
}

int16_t damage_overlay(int16_t k)
{
    return k >= 0 && k < SYS_COUNT && k_overlay[k].bit != 0xFF ? k_overlay[k].bit : -1;
}

int16_t damage_event(int16_t *sys, int16_t *level)
{
    if (g_event_out == g_event_in)
        return 0;
    *sys = g_events[g_event_out & 7].sys;
    *level = g_events[g_event_out & 7].level;
    g_event_out++;
    return 1;
}

/* gun record g is a working mount of the kind: a launcher, or a gun or turret */
static int16_t mount_working(int16_t g, int16_t launchers)
{
    const RvWeapon __far *w = weapon(GUN_TYPE(0, g));
    int16_t launcher = w && (w->kind == RV_WK_MISSILE || w->kind == RV_WK_TORPEDO || w->kind == RV_WK_ROCKET);
    if (!w || (GUN_STATE(0, g) & GUN_DESTROYED))
        return 0;
    return launchers ? launcher : w->kind == RV_WK_GUN || w->kind == RV_WK_TURRET;
}

/* a mount destroyed: on a side, the working one furthest to that side; front or rear, one by chance */
static void mount_destroyed(int16_t launchers, int16_t side)
{
    int16_t g, best = -1, n = 0;
    int32_t best_x = 0;
    for (g = 0; g < GUN_COUNT(0); g++) {
        int32_t x;
        if (!mount_working(g, launchers))
            continue;
        n++;
        x = side == 3 ? -(int32_t)GUN_MOUNT_X(0, g) : GUN_MOUNT_X(0, g);   /* (+ to the right: the left's the least) */
        if (best < 0 || x < best_x) {
            best = g;
            best_x = x;
        }
    }
    if (!n)
        return;
    if (side < 2) {
        int16_t pick = (int16_t)(game_rand() % n);
        for (g = 0; g < GUN_COUNT(0); g++)
            if (mount_working(g, launchers) && !pick--) {
                best = g;
                break;
            }
    }
    GUN_STATE(0, best) |= GUN_DESTROYED;
}

/* a system broken a point further, on that side: its own effects at once */
static void one_point(int16_t side)
{
    int16_t k = 0, tries, level;
    int16_t r = (int16_t)(game_rand() % 64);
    int16_t sum = 0;
    for (k = 0; k < SYS_COUNT; k++)
        sum = (int16_t)(sum + k_odds[k][side]);
    r = (int16_t)(r % sum);
    for (k = 0; k < SYS_COUNT - 1 && r >= k_odds[k][side]; k++)
        r = (int16_t)(r - k_odds[k][side]);
    for (tries = 0; tries < SYS_COUNT && sys_level(k) >= SYS_WRECKED; tries++)
        k = (int16_t)((k + 1) % SYS_COUNT);   /* (what it would have broken is gone already: the next along) */
    if (tries == SYS_COUNT)
        return;
    level = (int16_t)(sys_level(k) + 1);
    sys_set(k, level);
    overlays(k, level);
    event(k, level);
    switch (k) {
    case SYS_GUNS:                      /* a point is a gun, the first on the hit's side */
        mount_destroyed(0, side);
        break;
    case SYS_RACKS:                     /* and a launcher, its rounds with it */
        mount_destroyed(1, side);
        break;
    case SYS_THRUSTERS:
        g_pull = side == 3 ? 1 : side == 2 ? 0 : (uint8_t)(game_rand() & 1);   /* (the side that lost them drags) */
        break;
    case SYS_FUEL:                      /* the tank holed: some of it gone at once, Kit's reserve tank too when it goes */
        g_sys.fuel = (int16_t)(g_sys.fuel - g_sys.fuel / 8);
        if (level >= SYS_WRECKED)
            g_sys.fuel = g_sys.fuel_extra = 0;
        break;
    case SYS_DECOYS:
        g_sys.decoys = level >= SYS_WRECKED ? 0 : (uint8_t)(g_sys.decoys / 2);
        break;
    }
}

void damage_hurt(uint16_t left, uint16_t hull, int16_t side)
{
    int16_t points;
    if (!hull || side < 0 || side > 3)
        return;
    /* a hit on the reserve always breaks something; one on the hull by how big it is */
    if (!g_in_reserve && (uint32_t)(game_rand() % hull) * 2 >= (uint32_t)left * 3)
        return;
    points = (int16_t)(1 + left / POINT_QTY);
    if (points > POINTS_MOST)
        points = POINTS_MOST;
    while (points-- > 0)
        one_point(side);
}

uint8_t damage_points_so_far(void)
{
    return g_event_in;
}

void damage_ram_point(uint8_t before, int16_t side)
{
    if (side >= 0 && side <= 3 && g_event_in == before)
        one_point(side);
}

/* --- once a step --- */

/* hurt, and something the repair system can mend: not the absorbers or the tracking, and no weapon that is missing: a gun, a
 * rack or the decoys gone stay gone (the user's) */
static int16_t mendable(int16_t k)
{
    return sys_level(k) && k != SYS_ABSORBERS && k != SYS_TRACK && k != SYS_GUNS && k != SYS_RACKS && k != SYS_DECOYS;
}

void damage_step(void)
{
    const RvShipType __far *t;
    int16_t k, n;
    if (!slot_is_ship(0) || rvController[0] != CTL_PLAYER || (t = slot_type(0)) == 0)
        return;
    /* the repair system, whole: a point off a hurt system at random.  The glass stays cracked: it mends the system, not the overlay */
    if (!sys_level(SYS_REPAIR) && g_repair_wait && --g_repair_wait == 0) {
        n = 0;
        for (k = 0; k < SYS_COUNT; k++)
            if (mendable(k))
                n++;
        if (n) {
            int16_t pick = (int16_t)(game_rand() % n);
            for (k = 0; k < SYS_COUNT; k++)
                if (mendable(k) && !pick--) {
                    sys_set(k, (int16_t)(sys_level(k) - 1));
                    break;
                }
        }
        g_repair_wait = REPAIR_STEPS;
    }
    /* the tank: what it still holds (a hole bigger the worse it is) and its leak */
    {
        int16_t lv = sys_level(SYS_FUEL), cap = (int16_t)(k_fuel_pct[lv] * 10);
        if (g_sys.fuel > cap)
            g_sys.fuel = cap;
        if (lv && lv < SYS_WRECKED) {   /* (thousandths of the tank a second: 30 steps) */
            g_leak = (uint16_t)(g_leak + k_leak[lv]);
            while (g_leak >= 30 && g_sys.fuel > 0) {
                g_leak = (uint16_t)(g_leak - 30);
                g_sys.fuel--;
                if (g_sys.fuel_extra > 0)
                    g_sys.fuel_extra--;
            }
            if (!g_sys.fuel)
                g_leak = 0;
        }
    }
    /* what the hull's state shows in the cockpit */
    for (k = 0; k < 4; k++)
        if (!ARMOR(0, k))
            rvCockpitDamage |= 1UL << OVERLAY_ARMOR;
    if (damagePoints[0]) {
        if (t->hull && (uint32_t)damagePoints[0] * 4 >= (uint32_t)t->hull)
            rvCockpitDamage |= 1UL << OVERLAY_HULL;
        if (t->hull && (uint32_t)damagePoints[0] * 2 >= (uint32_t)t->hull)
            rvCockpitDamage |= 1UL << OVERLAY_CANOPY;
    }
    if (g_in_reserve) {
        int16_t left = damage_reserve_pct(t->hull);
        rvCockpitDamage |= 1UL << OVERLAY_WARNINGS | 1UL << OVERLAY_SURVIVAL;
        if (left < 75)
            rvCockpitDamage |= 1UL << OVERLAY_CANOPY_RIGHT;
        if (left < 40)
            rvCockpitDamage |= 1UL << OVERLAY_CANOPY_TOP;
    }
    if (sys_level(SYS_ENGINE) >= 3)
        rvCockpitDamage |= 1UL << OVERLAY_SPEED;
}

/* --- what the ship can do --- */

uint16_t damage_kps(uint16_t kps)
{
    return (uint16_t)((uint32_t)kps * k_engine_pct[sys_level(SYS_ENGINE)] / 100);
}

uint16_t damage_accel(uint16_t accel)
{
    return (uint16_t)((uint32_t)accel * k_engine_pct[sys_level(SYS_ENGINE)] / 100);
}

int16_t damage_burner_ok(void)
{
    return sys_level(SYS_ENGINE) < SYS_WRECKED && sys_level(SYS_FUEL) < SYS_WRECKED;
}

int16_t damage_turn_rate(int16_t dps)
{
    return (int16_t)((int32_t)dps * k_turn_pct[sys_level(SYS_THRUSTERS)] / 100);
}

int16_t damage_pull(void)
{
    if (sys_level(SYS_THRUSTERS) < 3)
        return 0;
    return g_pull ? 1 : -1;
}

int32_t damage_slide(int32_t slide_dmg)
{
    return slide_dmg * k_slide_pct[sys_level(SYS_ABSORBERS)] / 100;
}

uint16_t damage_shield_cap(uint16_t most)
{
    return (uint16_t)((uint32_t)most * k_shield_pct[sys_level(SYS_SHIELDS)] / 100);
}

uint16_t damage_energy_cap(void)
{
    return k_energy_pct[sys_level(SYS_POWER)];
}

int16_t damage_lock_factor(void)
{
    int16_t track = sys_level(SYS_TRACK);
    if (track >= SYS_WRECKED)
        return 0;
    return (int16_t)(1 + track + (sys_level(SYS_COMPUTER) >= 3 ? 1 : 0));
}

#ifdef RV_TESTS
/* every system a point worse, with what breaking it does (a script's way to see each level: RVT.EXE's ( key) */
void damage_test_worse(void)
{
    int16_t k, level;
    for (k = 0; k < SYS_COUNT; k++) {
        level = sys_level(k);
        if (level >= SYS_WRECKED)
            continue;
        sys_set(k, (int16_t)(level + 1));
        overlays(k, (int16_t)(level + 1));
        event(k, (int16_t)(level + 1));
        if (k == SYS_GUNS)
            mount_destroyed(0, 3);
        else if (k == SYS_RACKS)
            mount_destroyed(1, 3);
    }
}

/* a few systems hurt, the VDUs and the computer left whole, and a third of the hull more each time, short of the reserve: a look at
 * the damage page and the hull's words (the _ key) */
void damage_test_some(void)
{
    static const uint8_t __far some[4][2] = { { SYS_ENGINE, 2 }, { SYS_GUNS, 1 }, { SYS_FUEL, 4 }, { SYS_ABSORBERS, 3 } };
    int16_t i;
    for (i = 0; i < 4; i++) {
        sys_set(some[i][0], some[i][1]);
        overlays(some[i][0], some[i][1]);
    }
    mount_destroyed(0, 3);
    ARMOR(0, 1) = (uint16_t)(ARMOR(0, 1) / 2);
    ARMOR(0, 3) = 0;
    SHIELD(0, 1) = 0;
    damagePoints[0] = (uint16_t)(damagePoints[0] + slot_type(0)->hull / 3);
    if (damagePoints[0] >= slot_type(0)->hull)
        damagePoints[0] = (uint16_t)(slot_type(0)->hull - 1);
}

/* every system a little hurt: a look at all the shapes on the damage page (the ^ key) */
void damage_test_all(void)
{
    int16_t k;
    for (k = 0; k < SYS_COUNT; k++)
        sys_set(k, 1);
}

/* the hull gone and half the reserve spent (the ) key) */
void damage_test_breach(void)
{
    const RvShipType __far *t = slot_type(0);
    if (!t)
        return;
    damagePoints[0] = t->hull;
    (void)damage_reserve_take(reserve_of(t->hull) / 2, t->hull);
}
#endif
