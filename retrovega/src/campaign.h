/* campaign.h: the campaign, its missions and the bunks' saves (campaign.c) */
#ifndef CAMPAIGN_H
#define CAMPAIGN_H

#include "rvdata.h"

#define BUNKS 8
#define SAVE_MAGIC "RVSAVE"
#define SAVE_VERSION 2                   /* (a version 1 save is read too: the fields at the end start empty) */
enum { ENDING_NONE, ENDING_GOOD, ENDING_BAD };

#pragma pack(1)
typedef struct {
    uint16_t missions, kills;           /* kills: the splash count */
    uint32_t medals;                    /* a bit each */
    uint8_t alive, pad;
} RvPilotStats;

typedef struct {
    char magic[6];
    uint16_t version, size;
    uint8_t bunk;
    char name[24];
    uint8_t base;                       /* the ship the player lives on: 0 the carrier */
    uint8_t node, mission;              /* the campaign node, and the mission within it (0-3) */
    uint8_t wins;                       /* in this node */
    int16_t trend;                      /* the campaign's running total: wins minus losses */
    uint8_t ending;                     /* ENDING_* */
    uint8_t player_ship, livery;        /* ships.csv type, liveries.csv row */
    uint8_t pad;
    uint32_t seed;
    uint16_t flown;                     /* missions flown in all */
    RvPilotStats pilot[RV_PILOTS];
    uint16_t systems;                   /* a bit per campaign node flown (the scenes' if= and unless=) */
    uint8_t last;                       /* the last mission: 0 none yet, 1 won, 2 lost (the scenes' after=) */
    uint8_t escort_lost;                /* the last flight lost a ship it was escorting (the scenes' escort=lost) */
} RvSave;
#pragma pack()

extern RvSave g_camp;
const RvTalk __far *camp_line(const RvMission __far *m, int16_t k);   /* line k of the mission, out of XMS */
int16_t camp_home_count(int16_t tier);   /* what Kit has to say to a fighter come home in this state (damage_home_tier): how many lines */
const char __far *camp_home_line(int16_t tier, int16_t k);   /* line k of them, out of XMS */
int16_t camp_line_ok(const RvTalk __far *t);   /* a scene's line can be said now: its conditions, and nobody dead in it */
/* the mission's rules (data/missions.txt `rules`, camp_load_units): what a bolt must do to hit a ship (0: each
 * type's own ships.csv choice; else an RV_SHIPTYPE_FLAG_HIT_* for every ship: shots.c) and the RV_RULE_* flags
 * (logic.c: how the AI aims, whether a player's target holds its guns, what a collision costs) */
extern uint8_t g_rule_hit, g_rule_flags;
extern RvPilot g_pilots[RV_PILOTS];
extern RvNode g_nodes[RV_NODES];
extern uint16_t g_npilots, g_nnodes, g_nmissions;
extern RvShipType __far g_shiptypes[RV_SHIPRECORDS];
extern uint16_t g_nshiptypes;
const RvShipType __far *ship_type(uint16_t type);   /* data/ships.csv's row for a type, or NULL */
extern RvWeapon __far g_weapons[RV_WEAPONS];
extern uint16_t g_nweapons;
const RvWeapon __far *weapon(uint16_t type);    /* data/weapons.csv's row for a weapon type (a gun record's), or NULL */

int16_t camp_load_data(const char *path);
void camp_new(void);
uint16_t camp_mission_index(void);              /* the mission to fly next */
void camp_mission(uint16_t i, RvMission __far *out);
void camp_load_tables(const RvMission __far *m);   /* camp_load_units without telling the layer (missionLoaded): the simulator's */
void camp_load_units(const RvMission __far *m);   /* missionShipTable and navPointTable, then missionLoaded */
extern uint8_t g_camp_way;                               /* a debrief's system=: 1 won, 2 lost, 0 the system goes on */
void camp_way_out(int16_t won);                          /* works that out for the flight just flown, before camp_resolve */
int16_t camp_resolve(int16_t won);                      /* scores the mission; returns 1 when the node is done */
uint16_t camp_node_missions(uint16_t node);             /* how many missions a node holds (1 .. RV_MISSIONS_PER_NODE) */
const char *camp_ending_text(void);

int16_t save_write(int16_t bunk, const char *name);
int16_t save_read(int16_t bunk, RvSave __far *out);     /* 0: empty or not a save */
int16_t save_load(int16_t bunk);

#endif
