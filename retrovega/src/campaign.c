/* campaign.c: RVGAME.DAT, the campaign's moves, loading a mission's ships
 * into the slot tables the layer reads, and the bunks' saves.
 *
 * The campaign (docs/engine.md): every node has two ways out, good (towards
 * victory) and bad (towards loss).  After its four missions a node trends
 * good with 3-4 wins, bad with 0-1, and with 2 as the campaign's running
 * total says. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "rv.h"
#include "rvhooks.h"
#include "campaign.h"
#include "world.h"

RvSave g_camp;
uint8_t g_rule_hit, g_rule_flags;
uint8_t g_easy;                         /* the bunk menu's difficulty (RVSETUP.CFG): 1 easy, the held fire on in every mission */
RvPilot g_pilots[RV_PILOTS];
RvNode g_nodes[RV_NODES];
uint16_t g_npilots, g_nnodes, g_nmissions;
RvShipType __far g_shiptypes[RV_SHIPRECORDS];   /* (RV_SHIPTYPES is how high a type's number goes: g_type_of's size) */
uint16_t g_nshiptypes;
RvWeapon __far g_weapons[RV_WEAPONS];
uint16_t g_nweapons;
static const RvShipType __far * __far g_type_of[RV_SHIPTYPES];   /* each type number's record (ship_type), 0 none */
static const RvWeapon __far * __far g_weapon_of[RV_WEAPON_TYPES];   /* each weapon number's record (weapon), 0 none */
static RvGameHeader g_head;
static XmsBlock g_missions, g_lines;

int16_t camp_load_data(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = rv_filebuf;
    uint16_t i;
    if (!f)
        return 0;
    fread(&g_head, sizeof g_head, 1, f);
    if (memcmp(g_head.magic, RVGAME_MAGIC, 6) != 0 || g_head.version != RVGAME_VERSION
        || g_head.pilot_size != sizeof(RvPilot) || g_head.node_size != sizeof(RvNode)
        || g_head.mission_size != sizeof(RvMission)) {
        RV_LOG("game data: %s is not this build's (sizes %u %u %u)", path, g_head.pilot_size, g_head.node_size,
               g_head.mission_size);
        fclose(f);
        return 0;
    }
    RV_ASSERT(g_head.npilots <= RV_PILOTS && g_head.nnodes <= RV_NODES && g_head.nmissions <= RV_MISSIONS);
    g_npilots = g_head.npilots;
    g_nnodes = g_head.nnodes;
    g_nmissions = g_head.nmissions;
    fseek(f, g_head.pilots_at, SEEK_SET);
    fread(g_pilots, sizeof(RvPilot), g_npilots, f);
    fseek(f, g_head.nodes_at, SEEK_SET);
    fread(g_nodes, sizeof(RvNode), g_nnodes, f);
    g_missions = xms_alloc((uint32_t)sizeof(RvMission) * g_nmissions, XMS_RESIDENT);
    RV_ASSERT(g_missions != 0);
    fseek(f, g_head.missions_at, SEEK_SET);
    for (i = 0; i < g_nmissions; i++) {
        uint16_t k;
        for (k = 0; k < sizeof(RvMission); k += sizeof rv_filebuf) {
            uint16_t part = sizeof(RvMission) - k < sizeof rv_filebuf ? sizeof(RvMission) - k : sizeof rv_filebuf;
            fread(buf, 1, part, f);     /* (straight to XMS, a file buffer at a time: no second mission in memory) */
            xms_put(g_missions, (uint32_t)i * sizeof(RvMission) + k, buf, part);
        }
    }
    RV_ASSERT(g_head.line_size == sizeof(RvTalk));
    g_lines = xms_alloc((uint32_t)sizeof(RvTalk) * g_head.nlines, XMS_RESIDENT);
    RV_ASSERT(g_lines != 0);
    fseek(f, g_head.lines_at, SEEK_SET);
    {
        uint32_t at = 0, all = (uint32_t)sizeof(RvTalk) * g_head.nlines;
        while (at < all) {
            uint16_t part = all - at < sizeof rv_filebuf ? (uint16_t)(all - at) : (uint16_t)sizeof rv_filebuf;
            fread(buf, 1, part, f);
            xms_put(g_lines, at, buf, part);
            at += part;
        }
    }
    RV_ASSERT(g_head.nshiptypes <= RV_SHIPRECORDS && g_head.shiptype_size == sizeof(RvShipType));
    g_nshiptypes = g_head.nshiptypes;
    fseek(f, g_head.shiptypes_at, SEEK_SET);
    for (i = 0; i < RV_SHIPTYPES; i++)
        g_type_of[i] = 0;
    for (i = 0; i < g_nshiptypes; i++) {
        const RvShipType __far *t = &g_shiptypes[i];
        fread(buf, 1, sizeof(RvShipType), f);
        _fmemcpy(&g_shiptypes[i], buf, sizeof(RvShipType));
        RV_ASSERT(t->type < RV_SHIPTYPES && !g_type_of[t->type]);
        g_type_of[t->type] = t;
    }
    RV_ASSERT(g_head.nweapons <= RV_WEAPONS && g_head.weapon_size == sizeof(RvWeapon));
    g_nweapons = g_head.nweapons;
    fseek(f, g_head.weapons_at, SEEK_SET);
    for (i = 0; i < RV_WEAPON_TYPES; i++)
        g_weapon_of[i] = 0;
    for (i = 0; i < g_nweapons; i++) {
        const RvWeapon __far *w = &g_weapons[i];
        fread(buf, 1, sizeof(RvWeapon), f);
        _fmemcpy(&g_weapons[i], buf, sizeof(RvWeapon));
        RV_ASSERT(w->type < RV_WEAPON_TYPES && !g_weapon_of[w->type]);
        g_weapon_of[w->type] = w;
    }
    fclose(f);
    RV_LOG("game data: %u pilots, %u nodes, %u missions, %u ship types, %u weapons", g_npilots, g_nnodes, g_nmissions,
           g_nshiptypes, g_nweapons);
    return 1;
}

/* a type number's record: the records are in ships.csv's order and the numbers have gaps (0x07, 0x18 .. 0x1F, ...),
 * so a table by number, made as RVGAME.DAT loads */
const RvShipType __far *ship_type(uint16_t type)
{
    return type < RV_SHIPTYPES ? g_type_of[type] : 0;
}

/* a weapon number's record, the same way (weapons.csv's numbers have gaps too: every gun asks, every step) */
const RvWeapon __far *weapon(uint16_t type)
{
    return type < RV_WEAPON_TYPES ? g_weapon_of[type] : 0;
}

void camp_new(void)
{
    uint16_t i;
    memset(&g_camp, 0, sizeof g_camp);
    memcpy(g_camp.magic, SAVE_MAGIC, 6);
    g_camp.version = SAVE_VERSION;
    g_camp.size = sizeof g_camp;
    g_camp.node = (uint8_t)g_head.start_node;
    g_camp.player_ship = 0x03;           /* the Redeemer, until Kit has a word */
    g_camp.seed = ticks();
    for (i = 0; i < RV_PILOTS; i++)
        g_camp.pilot[i].alive = 1;
    for (i = 0; i < g_npilots; i++)     /* (the board's marks each brings: their splashes before the campaign, people.csv) */
        g_camp.pilot[i].kills = g_pilots[i].kills;
    campaignNode(g_camp.node, g_camp.mission);
}

uint16_t camp_mission_index(void)
{
    RV_CHECK(g_camp.node, g_nnodes);
    RV_CHECK(g_camp.mission, RV_MISSIONS_PER_NODE);
    return g_nodes[g_camp.node].missions[g_camp.mission];
}

/* a line of the mission's: every scene, briefing and debrief lives in XMS and comes down a line at a time, so the
 * flight does not carry them.  One copy here: good until the next call for another line */
static const RvTalk __far *line_at(uint16_t at)
{
    static RvTalk __far line;
    static uint16_t have = 0xFFFF;
    if (at != have) {
        xms_get(g_lines, (uint32_t)at * sizeof(RvTalk), &line, sizeof(RvTalk));
        have = at;
    }
    return &line;
}

const RvTalk __far *camp_line(const RvMission __far *m, int16_t k)
{
    RV_CHECK(k, m->ntalk);
    return line_at(m->talk0 + k);
}

int16_t camp_home_count(int16_t tier)
{
    RV_CHECK(tier, 4);
    return g_head.home_n[tier];
}

const char __far *camp_home_line(int16_t tier, int16_t k)
{
    uint16_t at = g_head.home_line;
    int16_t i;
    RV_CHECK(tier, 4);
    RV_CHECK(k, g_head.home_n[tier]);
    for (i = 0; i < tier; i++)
        at += g_head.home_n[i];
    return line_at(at + k)->text;
}

/* the way out of a node: good with more missions won than lost, bad with fewer, and an even split by the campaign's
 * running total (a node of four: three or four won good, none or one bad, two by the total) */
static int16_t way_good(int16_t wins, uint16_t count, int16_t trend)
{
    return wins * 2 > (int16_t)count || (wins * 2 == (int16_t)count && trend >= 0);
}

uint8_t g_camp_way;

int16_t camp_line_ok(const RvTalk __far *t)
{
    uint8_t f = t->flags, was;
    was = t->room == RV_ROOM_WIN || t->room == RV_ROOM_LOSE ? g_camp_way : g_camp.last;
    if ((f & RV_TALK_WON) && was != 1)
        return 0;
    if ((f & RV_TALK_LOST) && was != 2)
        return 0;
    if ((f & RV_TALK_IF) && !(g_camp.systems >> t->arg & 1))
        return 0;
    if ((f & RV_TALK_UNLESS) && (g_camp.systems >> t->arg & 1))
        return 0;
    if ((f & RV_TALK_DEAD) && g_camp.pilot[t->arg].alive)
        return 0;
    if ((f & RV_TALK_ALIVE) && !g_camp.pilot[t->arg].alive)
        return 0;
    if ((f & RV_TALK_ESCORT) && !g_camp.escort_lost)
        return 0;
    if (t->speaker < RV_PILOTS && !g_camp.pilot[t->speaker].alive)
        return 0;
    if (t->listener < RV_PILOTS && !g_camp.pilot[t->listener].alive)
        return 0;
    return 1;
}

void camp_mission(uint16_t i, RvMission __far *out)
{
    RV_CHECK(i, g_nmissions);
    xms_get(g_missions, (uint32_t)i * sizeof(RvMission), out, sizeof(RvMission));
}

/* the mission's ships into the tables the layer already reads (spec 4.4):
 * missionShipTable records of 0x2a bytes (+0 type, +2 side, +4 leader,
 * +6 behaviour, +8 nav point, +16 pilot, +17 AI profile, +18 role, +19 fear rates, +20 turrets off, +21 jump, +22
 * position x y z in km off the nav point, 32 bits each: MSHIP_REC_KM, +41 the prey type), and the nav points' records of 0x4d bytes,
 * record n for nav n (outerSpawnShipEntity takes a nav's number as its record), nav 0 the launch point at the origin:
 * +0 id, +1 flags, +2 position x y z in km, 32 bits each (NAV_REC_KM: the old game's map, up to 105,000 km out), +14
 * its reach (NAV_REC_REACH), and at +0x39 the ten mission ships there (0xFFFF none).  A group's ships side by side, 1,500 km apart (a fighter is
 * some 900 km across). */
#define HOME_BEHIND_KM 2000
void camp_load_tables(const RvMission __far *m)
{
    uint8_t *ships = missionShipTable, *navs = navPointTable;
    uint16_t i, n = 0, k;
    if (m->fighter != 0xFF)             /* the mission says what the player (and the wingman) fly, as the old campaign did */
        g_camp.player_ship = m->fighter;
    camp_load_rules(m);
    memset(ships, 0, missionShipTable_SIZE * 64);
    memset(navs, 0, navPointTable_SIZE * 16);
    RV_ASSERT(m->nnav <= RV_NAVS && m->nship <= RV_SHIPS);
    for (i = 0; i < 16; i++)
        memset(navs + navPointTable_SIZE * i + 0x39, 0xFF, 20);
    for (i = 0; i < m->nnav; i++) {
        uint8_t *rec;
        RV_CHECK(m->nav[i].id, 16);
        rec = navs + navPointTable_SIZE * m->nav[i].id;
        rec[0] = m->nav[i].id;
        rec[1] = m->nav[i].flags;       /* (1: hidden, logic.c NAV_HIDDEN; 2: off at the start, NAV_OFF_AT_START) */
        for (k = 0; k < 4; k++)
            NAV_REC_SW(rec, k) = m->nav[i].sw[k];
        NAV_REC_KM(rec, 0) = m->nav[i].x;
        NAV_REC_KM(rec, 1) = m->nav[i].y;
        NAV_REC_KM(rec, 2) = m->nav[i].z;
        NAV_REC_REACH(rec) = m->nav[i].reach;
    }
    for (i = 0; i < m->nship; i++) {
        const RvShip __far *s = &m->ship[i];
        /* (a wingman the player lost is gone: the user, 2026-10-31: the rest of the system is solo) */
        if (s->type == RV_SHIP_WINGMAN && s->pilot < RV_PILOTS && !g_camp.pilot[s->pilot].alive)
            continue;
        for (k = 0; k < s->count && n < 64; k++, n++) {
            uint8_t *rec = ships + missionShipTable_SIZE * n;
            uint16_t type = s->type == RV_SHIP_PLAYER || s->type == RV_SHIP_WINGMAN ? g_camp.player_ship : s->type;
            uint16_t *at = (uint16_t *)(navs + navPointTable_SIZE * (s->nav & 15) + 0x39);
            uint16_t j;
            *(uint16_t *)(rec + 0) = type;
            *(uint16_t *)(rec + 2) = s->side;
            *(uint16_t *)(rec + 4) = s->type == RV_SHIP_PLAYER ? 0xFFFF : 0;
            *(uint16_t *)(rec + 6) = s->type == RV_SHIP_PLAYER ? 6 : s->type == RV_SHIP_WINGMAN ? 4 : 0;
            *(uint16_t *)(rec + 8) = s->nav;
            MSHIP_REC_KM(rec, 0) = s->x + (int32_t)k * 1500;
            MSHIP_REC_KM(rec, 1) = s->y;    /* (where the wave starts, off its nav point: at=x,y,z, the old game's) */
            MSHIP_REC_KM(rec, 2) = s->z;
            if (s->role == RV_ESCORT_HOME)  /* (the carrier: behind the launch, which faces +z, its hull clear of the player) */
                MSHIP_REC_KM(rec, 2) = -HOME_BEHIND_KM;
            rec[16] = s->pilot;
            rec[17] = s->ai;                /* (its AI profile, P_* + 1, 0 the type's own; logic.c outerSpawnShipEntity) */
            rec[18] = s->role;              /* (an escort target's, the home carrier's: RV_ESCORT_*; logic.c MSHIP_ROLE) */
            rec[19] = s->fear;              /* (its own fear rates, c | d << 4, 0 the profile's) */
            rec[20] = s->noturrets;         /* (its turrets silent: a mission's turrets=off; logic.c ship_think) */
            rec[21] = s->jump;              /* (role=out's last nav point, jump=N: 0 the next after its own; logic.c route_step) */
            rec[41] = s->prey;              /* (the type it is ordered to attack + 1, prey=KEY, 0 none: the old game's own byte for
                                               its orders' target; logic.c prey_of) */
            for (j = 0; j < 10 && at[j] != 0xFFFF; j++)
                ;
            if (j < 10)
                at[j] = n;
        }
    }
    playerMissionShip = 0;
    missionStatus = 3;                   /* on the carrier */
    currentSeries = g_camp.node;
    currentMission = g_camp.mission;
    RV_LOG("mission %.16Fs: %u nav points, %u mission ships", m->id, (uint16_t)m->nnav, n);
}

void camp_load_rules(const RvMission __far *m)
{
    g_rule_hit = m->hit;                /* (its rules: the hit volume, the AI's aim, the held fire, the collisions) */
    g_rule_flags = m->rules;
    if (g_easy)                         /* (the easy mode: the old game's held fire, a player's selected target holding its
                                           guns while the player is ahead of its nose; off for the usual play, the user's
                                           ruling 2026-11-03: a selected Shizong circled a still player for 15 minutes
                                           and never fired) */
        g_rule_flags |= RV_RULE_HOLD_FIRE;
}

uint16_t camp_flight_fighter(void)
{
    const uint8_t *rec = missionShipTable + missionShipTable_SIZE * (playerMissionShip & 63);
    return *(const uint16_t *)(rec + 6) == 6 ? *(const uint16_t *)rec : g_camp.player_ship;
}

void camp_load_units(const RvMission __far *m)
{
    camp_load_tables(m);
    missionLoaded(camp_mission_index());
}

/* a node's missions: up to RV_MISSIONS_PER_NODE, the unused entries 0xFFFF (a node of the old campaign's shape has two
 * or three) */
uint16_t camp_node_missions(uint16_t node)
{
    uint16_t k;
    for (k = 0; k < RV_MISSIONS_PER_NODE; k++)
        if (g_nodes[node].missions[k] == 0xFFFF)
            break;
    return k ? k : 1;
}

void camp_way_out(int16_t won)
{
    uint16_t count = camp_node_missions(g_camp.node);
    if (g_camp.mission + 1u < count)
        g_camp_way = 0;
    else
        g_camp_way = way_good(g_camp.wins + (won ? 1 : 0), count, g_camp.trend + (won ? 1 : -1)) ? 1 : 2;
}

int16_t camp_resolve(int16_t won, uint16_t hero_kills, const uint8_t __far *pilot_kills)
{
    RvShip ships[RV_SHIPS];             /* (only these: a whole mission would not fit on the stack) */
    uint32_t at = (uint32_t)camp_mission_index() * sizeof(RvMission);
    uint8_t nship;
    uint16_t i, next;
    xms_get(g_missions, at + offsetof(RvMission, nship), &nship, 1);
    xms_get(g_missions, at + offsetof(RvMission, ship), ships, sizeof ships);
    g_camp.last = (uint8_t)(won ? 1 : 2);
    g_camp.systems |= (uint16_t)(1u << g_camp.node);
    g_camp.flown++;
    g_camp.wins += won ? 1 : 0;
    g_camp.trend += won ? 1 : -1;
    /* the board's tallies are the flight's own splashes (the user, 2026-11-02: two splashes of the host's went up as
     * Frost's): the hero's, and each named pilot's the AI flew, whoever it was; a sortie each (they had a random 1 to 3
     * on a win, and the hero none) */
    for (i = 0; i < nship; i++)
        if (ships[i].pilot != RV_PILOT_NONE && ships[i].pilot < RV_PILOTS)
            g_camp.pilot[ships[i].pilot].missions++;
    for (i = 0; i < RV_PILOTS; i++)
        if (pilot_kills && pilot_kills[i])
            g_camp.pilot[i].kills = (uint16_t)(g_camp.pilot[i].kills + pilot_kills[i]);
    for (i = 0; i < g_npilots; i++)
        if (g_pilots[i].role == RV_ROLE_HERO) {   /* (the first of them: the game's own `you`) */
            g_camp.pilot[i].missions++;
            g_camp.pilot[i].kills = (uint16_t)(g_camp.pilot[i].kills + hero_kills);
            break;
        }
    missionResolved(won ? 1 : 0);
    if (++g_camp.mission < camp_node_missions(g_camp.node)) {
        campaignNode(g_camp.node, g_camp.mission);
        return 0;
    }
    {
        const RvNode *n = &g_nodes[g_camp.node];
        uint16_t count = camp_node_missions(g_camp.node);
        int16_t good = way_good(g_camp.wins, count, g_camp.trend);
        next = good ? n->good : n->bad;
        RV_LOG("node %.8s: %u of %u won, trend %d -> %s", n->id, g_camp.wins, count, g_camp.trend, good ? "good" : "bad");
    }
    g_camp.mission = 0;
    g_camp.wins = 0;
    if (next == RV_NODE_GOOD || next == RV_NODE_BAD) {
        g_camp.ending = next == RV_NODE_GOOD ? ENDING_GOOD : ENDING_BAD;
    } else {
        RV_CHECK(next, g_nnodes);
        g_camp.node = (uint8_t)next;
        campaignNode(g_camp.node, g_camp.mission);
    }
    return 1;
}

/* a guest's campaign taken to the host's mission (flight_prepare: the node and the mission there that the layer named):
 * the story is the host's from here, and the debrief moves it on as the host's (camp_resolve).  A node the guest had
 * not been in starts its tally with this mission (the host's own wins there are not carried) */
void camp_follow(uint16_t node, uint16_t mission)
{
    if (node >= g_nnodes || mission >= camp_node_missions(node))
        return;
    if (node != g_camp.node)
        g_camp.wins = 0;
    g_camp.node = (uint8_t)node;
    g_camp.mission = (uint8_t)mission;
    RV_LOG("guest: the campaign at the host's mission: node %u, mission %u", node, mission);
}

/* --- the bunks: BUNK01.SAV .. BUNK08.SAV --- */

static void bunk_path(int16_t bunk, char *path)
{
    sprintf(path, "BUNK%02d.SAV", bunk + 1);
}

int16_t save_write(int16_t bunk, const char *name)
{
    char path[16];
    FILE *f;
    RV_CHECK(bunk, BUNKS);
    g_camp.bunk = (uint8_t)bunk;
    if (name != g_camp.name)            /* (a save under the name it has: strncpy onto itself is undefined) */
        strncpy(g_camp.name, name, sizeof g_camp.name - 1);
    g_camp.name[sizeof g_camp.name - 1] = 0;
    bunk_path(bunk, path);
    f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(&g_camp, sizeof g_camp, 1, f);
    fclose(f);
    RV_LOG("saved bunk %d '%s': node %u mission %u", bunk + 1, g_camp.name, g_camp.node, g_camp.mission);
    gameSaved(bunk);
    return 1;
}

#define SAVE_V2_MORE 4                  /* the bytes version 2 added at the end: systems, last, escort_lost */
int16_t save_read(int16_t bunk, RvSave __far *out)
{
    char path[16];
    FILE *f;
    static RvSave s;
    RV_CHECK(bunk, BUNKS);
    bunk_path(bunk, path);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    {
        size_t got = fread(&s, 1, sizeof s, f);
        /* (version 1: the same but for the fields at the end, which start empty) */
        if (got == sizeof s - SAVE_V2_MORE && s.version == 1 && s.size == sizeof s - SAVE_V2_MORE) {
            memset((char *)&s + got, 0, SAVE_V2_MORE);
            s.version = SAVE_VERSION;
            s.size = sizeof s;
        } else if (got != sizeof s)
            s.magic[0] = 0;
    }
    _fmemcpy(out, &s, sizeof s);
    if (memcmp(s.magic, SAVE_MAGIC, 6) != 0
        || s.version != SAVE_VERSION || s.size != sizeof s || s.node >= g_nnodes
        || s.mission >= RV_MISSIONS_PER_NODE) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

int16_t save_load(int16_t bunk)
{
    static RvSave __far s;
    if (!save_read(bunk, &s))
        return 0;
    _fmemcpy(&g_camp, &s, sizeof s);
    RV_LOG("loaded bunk %d '%s': node %u mission %u", bunk + 1, g_camp.name, g_camp.node, g_camp.mission);
    gameLoaded(bunk);
    return 1;
}
