#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "wcnet_retrovega.h"
#include "wcnet_memory.h"
#include "wcnet_log.h"
#include "wcnet_rv.h"

namespace wc {

static_assert(RV_MAX_SHIP_SLOT <= kShipSlotCap, "RV.EXE's ship slots must fit the layer's per-ship arrays");
static_assert(RV_MAX_SHIP_SLOT < 32, "a bit per ship slot (g_rvMarked)");

static bool g_rvCompatible = false;
static std::string g_rvMismatch;
static Bit32u g_rvMarked = 0;   // ship slots the layer made puppets or marked human

bool rv_compatible() { return g_game == GAME_RV && g_rvCompatible; }

void rv_set_compatible(bool ok, const std::string &why) {
    g_rvCompatible = ok;
    g_rvMismatch = ok ? std::string() : why;
    g_rvMarked = 0;
}

const std::string &rv_mismatch() { return g_rvMismatch; }

// Shared flights for a compatible RV.EXE in a room (the page offers them:
// web/gamefiles.js); WCRVNET=0 keeps them shut, the game then only watched
// as before them.
static const bool kRvSharedByDefault = true;

bool rv_shared() {
    if (!rv_compatible()) {
        return false;
    }
    const char *env = getenv("WCRVNET");
    return env && env[0] ? env[0] != '0' : kRvSharedByDefault;
}

bool rv_interface_check(Bit16u loadSeg, Bit16u dgroupPara, std::string *why) {
    char buf[200];
    const PhysPt hook = (PhysPt)loadSeg * 0x10;
    const PhysPt ds = ((PhysPt)loadSeg + dgroupPara) * 0x10;
    const unsigned nthunks = sizeof(kRvThunks) / sizeof(kRvThunks[0]), nsites = sizeof(kRvSites) / sizeof(kRvSites[0]);
    const unsigned version = mem_readw(hook + RV_HOOK_VERSION);
    if (version != RV_ABI_VERSION) {
        snprintf(buf, sizeof(buf), "RV.EXE is built for ABI %u, the emulator for ABI %u", version, (unsigned)RV_ABI_VERSION);
        *why = buf;
        return false;
    }
    if (mem_readw(hook + RV_HOOK_NTHUNK) != nthunks || mem_readw(hook + RV_HOOK_NSITE) != nsites) {
        snprintf(buf, sizeof(buf), "RV.EXE has %u thunks and %u sites, the emulator knows %u and %u",
                 (unsigned)mem_readw(hook + RV_HOOK_NTHUNK), (unsigned)mem_readw(hook + RV_HOOK_NSITE), nthunks, nsites);
        *why = buf;
        return false;
    }
    const PhysPt h = ds + RV_PUPPET_HEADER;
    static const char kMagic[] = RV_PUPPET_MAGIC;
    for (size_t k = 0; k < 8; k++) {
        if (mem_readb(h + (PhysPt)k) != (Bit8u)kMagic[k]) {
            *why = "RV.EXE has no puppet header at DS:0010";
            return false;
        }
    }
    static const struct { int at; unsigned want; const char *what; } kWords[] = {
        { 8, RV_ABI_VERSION, "its ABI version" },          { 10, RV_SLOTS, "the slots" },
        { 12, RV_MAX_SHIP_SLOT, "the last ship slot" },     { 14, RV_FIRST_TEMP_SLOT, "the first temporary slot" },
        { 16, RV_CAMERA_SLOT, "the camera's slot" },        { 18, RV_MISSION_SHIP_SIZE, "a mission ship's size" },
        { 20, RV_NAV_POINT_SIZE, "a nav point's size" },    { 22, RV_GUNS_SIZE, "a gun table's size" },
        { 24, RV_GUN_SIZE, "a gun's size" },
    };
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++) {
        unsigned have = mem_readw(h + kWords[i].at);
        if (have != kWords[i].want) {
            snprintf(buf, sizeof(buf), "RV.EXE's puppet header has %u for %s, the emulator %u", have, kWords[i].what,
                     kWords[i].want);
            *why = buf;
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(kRvDirectory) / sizeof(kRvDirectory[0]); i++) {
        unsigned have = mem_readw(ds + RV_DIRECTORY + 2 * (PhysPt)i);
        if (have != kRvDirectory[i]) {
            snprintf(buf, sizeof(buf), "RV.EXE's fixed block differs from the emulator's at item %u (DS:%04X, the "
                     "emulator's DS:%04X)", (unsigned)i, have, (unsigned)kRvDirectory[i]);
            *why = buf;
            return false;
        }
    }
    return true;
}

GameParams rv_params() {
    // (Field order: wcnet_game.h.)  RV keeps its asteroid and mine fields in
    // fieldTable, never among its mission ships: no ship type stands for one
    // (WC1's 0x16 and 0x17 are two of RV's fighters).  The nav point's ten
    // mission-ship words at +0x39 and a record's behaviour word at +6 are WC1's
    // record layout, which RV keeps (rvabi.def, campaign.c camp_load_tables).
    GameParams p = { RV_SLOTS, RV_MISSION_SHIP_SIZE, 0, 2, -1, -1, RV_NAV_POINT_SIZE, 0x39, 6, RV_GUNS_SIZE, RV_GUN_SIZE,
                     -1, 0, RV_MAX_SHIP_SLOT };
    return p;
}

void rv_set_role(int role, int players) {
    if (!rv_compatible()) {
        return;
    }
    if (rd8(RV_DS_rvNetRole) != (Bit8u)role || rd8(RV_DS_rvPlayerCount) != (Bit8u)players) {
        wclog(1, "RV.EXE: %s, %d pilots", role == RV_ROLE_HOST ? "the host" : role == RV_ROLE_GUEST ? "a guest" : "alone",
              players);
    }
    wr8(RV_DS_rvNetRole, (Bit8u)role);
    wr8(RV_DS_rvPlayerCount, (Bit8u)players);
}

int rv_controller(int slot) {
    return rv_compatible() && slot >= 0 && slot <= RV_MAX_SHIP_SLOT ? rd8((Bit16u)(RV_DS_rvController + slot)) : -1;
}

bool rv_human(int slot) {
    return rv_compatible() && slot >= 0 && slot <= RV_MAX_SHIP_SLOT && (rd16((Bit16u)(RV_DS_rvFlags + 2 * slot)) & RV_FLAG_HUMAN);
}

bool rv_marked(int slot) {
    return slot >= 0 && slot <= RV_MAX_SHIP_SLOT && (g_rvMarked >> slot & 1);
}

void rv_mark(int slot, int controller, bool human, int net) {
    if (!rv_compatible() || slot < 0 || slot > RV_MAX_SHIP_SLOT) {
        return;
    }
    static const char *const kName[] = { "the AI's", "the local pilot's", "a puppet", "the script's", "frozen" };
    const Bit16u flagsAt = (Bit16u)(RV_DS_rvFlags + 2 * slot);
    const Bit8u was = rd8((Bit16u)(RV_DS_rvController + slot));
    const Bit16u flags = rd16(flagsAt);
    const Bit16u want = human ? (Bit16u)(flags | RV_FLAG_HUMAN) : (Bit16u)(flags & ~RV_FLAG_HUMAN);
    if (was != controller || flags != want) {
        wclog(controller == RV_CTL_PUPPET || was == RV_CTL_PUPPET ? 1 : 2, "RV.EXE: slot %d is %s%s (was %s), net %d", slot,
              controller >= 0 && controller <= 4 ? kName[controller] : "?", human ? ", a human's" : "",
              was <= 4 ? kName[was] : "?", net);
    }
    wr8((Bit16u)(RV_DS_rvController + slot), (Bit8u)controller);
    wr16(flagsAt, want);
    wr8((Bit16u)(RV_DS_rvNetId + slot), net < 0 || net > 0xfe ? 0xff : (Bit8u)net);
    if (controller == RV_CTL_PUPPET || human) {
        g_rvMarked |= 1u << slot;
    } else {
        g_rvMarked &= ~(1u << slot);
    }
}

void rv_unmark_all() {
    if (!rv_compatible()) {
        return;
    }
    for (int slot = 0; slot <= RV_MAX_SHIP_SLOT; slot++) {
        if (!(g_rvMarked >> slot & 1)) {
            continue;
        }
        int ctl = rd8((Bit16u)(RV_DS_rvController + slot));
        rv_mark(slot, ctl == RV_CTL_PUPPET ? RV_CTL_AI : ctl, false, -1);
    }
    g_rvMarked = 0;
    rv_set_role(RV_ROLE_ALONE, 1);
}

void rv_set_net_id(int slot, int net) {
    if (rv_compatible() && slot >= 0 && slot <= RV_MAX_SHIP_SLOT) {
        wr8((Bit16u)(RV_DS_rvNetId + slot), net < 0 || net > 0xfe ? 0xff : (Bit8u)net);
    }
}

void rv_stamp_motion(int slot) {
    if (rv_compatible() && slot >= 0 && slot <= RV_MAX_SHIP_SLOT) {
        wr16((Bit16u)(RV_DS_rvLastWrite + 2 * slot), rd16(RV_DS_frameCounter));
    }
}

static std::string read_bytes(Bit16u off, unsigned n) {
    std::string out;
    out.reserve(n);
    for (unsigned i = 0; i < n; i++) {
        out.push_back((char)rd8((Bit16u)(off + i)));
    }
    return out;
}

RvMissionTables rv_read_mission_tables() {
    RvMissionTables t;
    if (!rv_compatible()) {
        return t;
    }
    t.missionShips = read_bytes(RV_DS_missionShipTable, RV_MISSION_SHIPS * RV_MISSION_SHIP_SIZE);
    t.navPoints = read_bytes(RV_DS_navPointTable, RV_NAV_POINTS * RV_NAV_POINT_SIZE);
    t.playerMissionShip = rd16(RV_DS_playerMissionShip);
    return t;
}

bool rv_write_mission_tables(const RvMissionTables &t) {
    if (!rv_compatible()) {
        return false;
    }
    if (t.missionShips.size() != RV_MISSION_SHIPS * RV_MISSION_SHIP_SIZE || t.navPoints.size() != RV_NAV_POINTS * RV_NAV_POINT_SIZE ||
        t.playerMissionShip < 0 || t.playerMissionShip >= RV_MISSION_SHIPS) {
        wclog(0, "RV.EXE: the host's mission records are %u and %u bytes (player record %d), ours %u and %u: not taken",
              (unsigned)t.missionShips.size(), (unsigned)t.navPoints.size(), t.playerMissionShip,
              (unsigned)(RV_MISSION_SHIPS * RV_MISSION_SHIP_SIZE), (unsigned)(RV_NAV_POINTS * RV_NAV_POINT_SIZE));
        return false;
    }
    for (size_t i = 0; i < t.missionShips.size(); i++) {
        wr8((Bit16u)(RV_DS_missionShipTable + i), (Bit8u)t.missionShips[i]);
    }
    for (size_t i = 0; i < t.navPoints.size(); i++) {
        wr8((Bit16u)(RV_DS_navPointTable + i), (Bit8u)t.navPoints[i]);
    }
    wr16(RV_DS_playerMissionShip, (Bit16u)t.playerMissionShip);
    wclog(1, "RV.EXE: flying by the host's mission records (the player's is %d)", t.playerMissionShip);
    return true;
}

}  // namespace wc
