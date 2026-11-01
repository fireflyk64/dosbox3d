/* simulator.c: the bar's simulator (the user, 2026-11-01).  The player in a Schroedinger, nothing else out there: "the
 * player starts fighting one of [the] rlaan types.  It cycles through all the rlaan fighter types, then goes to two of
 * the first and so forth.  Until the player is destroyed ... No wingman, no carrier, just fighting.  Shields get healed
 * every time the count increases"; then "a solid 47 kills would be enough to demonstrate prowess" (it comes in the
 * fourth round) and "missiles fuel etc should be replenished when the count increases".
 *
 * The waves' bookkeeping only: the flight, its AI, its guns and its deaths are the game's own, as they are in a mission.
 * It is strictly one player's (flight_simulator: none of the flight loop's hook sites, simulatorStart and simulatorEnd
 * for the layer), and nothing of it goes into the campaign. */
#include <stdio.h>
#include <string.h>
#include "rv.h"
#include "rvhooks.h"
#include "campaign.h"
#include "world.h"
#include "logic.h"
#include "panel.h"
#include "simulator.h"

#define SIM_BREATH 60                   /* steps between a wave's end and the next (2 s) */
#define SIM_AHEAD_KM 5000L              /* a wave comes in this far ahead of the player */
#define SIM_SPACING_KM 1200L            /* its ships this far apart, six to a row */
#define SIM_ROW 6
#define SIM_FUEL_FULL 1000              /* (flight.c systems_start) */

static const uint8_t __far k_types[SIM_TYPES] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15 };   /* ships.csv's order: Shizu,
                                           Zhuangzong, Taizong, Shizong, Xuanzong, Gaozong */
static RvMission __far g_mission;
static struct {
    uint8_t count, type_i, won;
    uint16_t wait, kills, end;
    uint8_t counted[NUM_SHIP_SLOTS];    /* this slot's death is in the kills */
} __far g_s;

static uint8_t __far g_first;           /* a test's /simat: how many the first wave is */

void simulator_test_first(int16_t count)
{
    g_first = (uint8_t)(count > SIM_MAX_SHIPS ? SIM_MAX_SHIPS : count);
}

const RvMission __far *simulator_mission(const RvMission __far *rules)
{
    static const char __far s_id[] = "SIMULATOR", __far s_title[] = "The simulator";
    int16_t i;
    _fmemset(&g_mission, 0, sizeof g_mission);
    _fstrcpy(g_mission.id, s_id);
    _fstrcpy(g_mission.title, s_title);
    g_mission.fighter = SIM_FIGHTER;
    g_mission.hit = rules->hit;
    g_mission.rules = rules->rules;
    g_mission.wipe = 0xFF;
    g_mission.nnav = 1;                 /* (home, hidden: no cross on the view, nowhere to go) */
    g_mission.nav[0].flags = 1;
    g_mission.nship = SIM_TYPES + 1;
    g_mission.ship[0].type = RV_SHIP_PLAYER;
    g_mission.ship[0].count = 1;
    g_mission.ship[0].pilot = 0xFF;
    for (i = 0; i < SIM_TYPES; i++) {   /* (none of them: only so that the flight loads their sets) */
        g_mission.ship[i + 1].type = k_types[i];
        g_mission.ship[i + 1].side = SIDE_RLAAN;
        g_mission.ship[i + 1].pilot = 0xFF;
    }
    return &g_mission;
}

void simulator_start(void)
{
    _fmemset(&g_s, 0, sizeof g_s);
    g_s.count = g_first ? g_first : 1;
    g_s.wait = SIM_BREATH;
}

/* the wave: facing the player, ahead of them, as a wall six ships wide */
static void spawn_wave(void)
{
    static const char __far s_wave[] = "simulator: %d of type %02X; %u kills so far";
    const RvVec3 *F = &gOrientationFrontVector[0], *R = &gOrientationRightVector[0], *U = &gOrientationUpVector[0];
    int16_t i, type = k_types[g_s.type_i], cols = g_s.count < SIM_ROW ? g_s.count : SIM_ROW,
            rows = (int16_t)((g_s.count + SIM_ROW - 1) / SIM_ROW);
    for (i = 0; i < g_s.count; i++) {
        int16_t s = ship_free_slot();
        int32_t across, up, along;
        if (s < 0)
            break;
        across = ((int32_t)(2 * (i % SIM_ROW) - (cols - 1)) * SIM_SPACING_KM) / 2 + (int32_t)(game_rand() % 401) - 200;
        up = ((int32_t)(2 * (i / SIM_ROW) - (rows - 1)) * SIM_SPACING_KM) / 2 + (int32_t)(game_rand() % 401) - 200;
        along = SIM_AHEAD_KM + (int32_t)(game_rand() % 801);
        ship_spawn(s, type, SIDE_RLAAN, CTL_AI, gPositionVector[0].x + F->x * along + R->x * across + U->x * up,
                   gPositionVector[0].y + F->y * along + R->y * across + U->y * up,
                   gPositionVector[0].z + F->z * along + R->z * across + U->z * up, 0);
        gOrientationFrontVector[s] = vec3(-F->x, -F->y, -F->z);
        gOrientationUpVector[s] = *U;
        gOrientationRightVector[s] = vec3(-R->x, -R->y, -R->z);
        g_s.counted[s] = 0;
    }
    rv_logf(s_wave, g_s.count, type, g_s.kills);
}

/* the count went up: what a flight uses up made whole: the shields, the fuel, the guns' energy, the launchers' rounds.
 * (Not the armour, the hull or a system hurt: that is damage, and what a run has to answer for.  A gun destroyed stays so) */
static void replenish(void)
{
    const RvShipType __far *t = slot_type(0);
    int16_t g;
    SHIELD(0, 0) = SHIELD_MAX(0, 0);
    SHIELD(0, 1) = SHIELD_MAX(0, 1);
    gunEnergy[0] = 100;
    g_sys.fuel = SIM_FUEL_FULL;
    for (g = 0; t && g < GUN_COUNT(0); g++)
        GUN_STATE(0, g) = (uint8_t)((GUN_STATE(0, g) & GUN_DESTROYED) | t->loadout[1 + g * 5 + 4]);
}

void simulator_frame(void)
{
    int16_t s, alive = 0;
    for (s = 1; s < NUM_SHIP_SLOTS; s++) {
        if (maybeEntityType[s] != ET_SHIP || commAnimInfo[s] != SIDE_RLAAN)
            continue;
        alive++;                        /* (a dying one too: the next wave waits for its explosion) */
        if (shipAiState[s] == 9 && !g_s.counted[s]) {
            g_s.counted[s] = 1;
            g_s.kills++;
        }
    }
    if (g_s.kills >= SIM_WIN_KILLS && !g_s.won) {
        static const char __far s_won[] = "simulator: won, %u kills";
        g_s.won = 1;
        g_s.end = SIM_BREATH;           /* (the last one's explosion seen) */
        rv_logf(s_won, g_s.kills);
    }
    if (missionStatus)
        return;
    if (g_s.won) {                      /* (won, whatever the wave's other ships do now) */
        if (--g_s.end == 0)
            missionStatus = 1;
        return;
    }
    if (alive || shipAiState[0] == 9)
        return;
    if (!g_s.wait) {                    /* the wave is over: on to the next type, or one more of the first */
        if (++g_s.type_i == SIM_TYPES) {
            g_s.type_i = 0;
            if (g_s.count < SIM_MAX_SHIPS)
                g_s.count++;
            replenish();
        }
        g_s.wait = SIM_BREATH;
    }
    if (--g_s.wait == 0)
        spawn_wave();
}

void simulator_line(char *out)
{
    const RvShipType __far *t = ship_type(k_types[g_s.type_i]);
    char name[RV_NAME];
    name[0] = 0;
    if (t)
        _fstrncpy(name, t->name, sizeof name - 1);
    name[sizeof name - 1] = 0;
    sprintf(out, "%d x %s  KILLS %u OF %d  ESC LEAVES", g_s.count, name, g_s.kills, SIM_WIN_KILLS);
}

int16_t simulator_count(void) { return g_s.count; }
int16_t simulator_type(void) { return k_types[g_s.type_i]; }
uint16_t simulator_kills(void) { return g_s.kills; }
int16_t simulator_won(void) { return g_s.won; }

void simulator_test_at(int16_t count, int16_t type_i)
{
    g_s.count = (uint8_t)count;
    g_s.type_i = (uint8_t)type_i;
}

void simulator_test_kill(void)
{
    int16_t s;
    for (s = 1; s < NUM_SHIP_SLOTS; s++)
        if (maybeEntityType[s] == ET_SHIP && commAnimInfo[s] == SIDE_RLAAN)
            shipAiState[s] = 9;
}
