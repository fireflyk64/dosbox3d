#include <stdio.h>
#include <string.h>
#include <string>
#include "wc_net.h"
#include "wcnet_game.h"
#include "wcnet_memory.h"
#include "wcnet_code.h"
#include "wcnet_log.h"

namespace wc {

GameId g_game = GAME_NONE;
Bit16u g_loadSeg = 0;
GameParams g_params = { 0x40, 0x2a, 0, 2, 0x16, 0x17, 0x4d, 0x39 };

static const GameParams kWc1Params = { 0x40, 0x2a, 0, 2, 0x16, 0x17, 0x4d, 0x39 };
// WC2: 70 slots; a mission ship is 0x3c bytes with its class word at +0x15
// (5 and 6 are fields: ovr116:1D72); nav points are 0x65 bytes
static const GameParams kWc2Params = { 0x46, 0x3c, 0x15, 2, 5, 6, 0x65, 0 };
Bit16u DS = 0;
PhysPt DS_OFF = 0;
static Bit16u g_gamePsp = 0;

namespace ds {
#define WC_DS(name, wc1, wc2) Bit16u name = 0;
#include "wcnet_ds.def"
#undef WC_DS
}  // namespace ds

namespace code {
#define X(name) Loc name = { 0, 0, 0, 0, 0, 0, #name };
WC_CODE_LIST(X)
#undef X
}  // namespace code

// ---------------------------------------------------------------------------
// Tables

static void load_ds(GameId game) {
#define WC_DS(name, wc1, wc2) ds::name = game == GAME_WC1 ? (wc1) : game == GAME_WC2 ? (wc2) : 0;
#include "wcnet_ds.def"
#undef WC_DS
}

static void clear_code() {
#define X(name) code::name.seg = 0;
    WC_CODE_LIST(X)
#undef X
}

// An overlay function or place: the stub's paragraph in the image, a thunk
// in the stub, the offset in the overlay.
static void ovr(Loc &l, Bit16u stubPara, Bit16u stubOff, Bit16u off, int nargs = 0, bool pascal = false) {
    l.seg = (Bit16u)(g_loadSeg + stubPara);
    l.stubOff = stubOff;
    l.off = off;
    l.overlay = 1;
    l.nargs = (Bit8u)nargs;
    l.pascal = pascal ? 1 : 0;
}

static void root(Loc &l, Bit16u para, Bit16u off, int nargs = 0, bool pascal = false) {
    l.seg = (Bit16u)(g_loadSeg + para);
    l.stubOff = 0;
    l.off = off;
    l.overlay = 0;
    l.nargs = (Bit8u)nargs;
    l.pascal = pascal ? 1 : 0;
}

// WC.EXE.  Paragraphs from the executable's segment table (scripts/wcexe.py
// segs); the comments give the names in the IDA database.
static void load_wc1_code() {
    using namespace code;
    enum { SEG001 = 0x03BE, STUB133 = 0x10C4, STUB134 = 0x10C8, STUB140 = 0x110B, STUB143 = 0x1135, STUB145 = 0x1150,
           STUB148 = 0x1176, STUB150 = 0x1185, STUB161 = 0x11BF, STUB162 = 0x11C4, STUB163 = 0x11CE, STUB168 = 0x121B };
    // overlay 143: ships, damage, weapons
    ovr(do_damage, STUB143, 0x0084, 0x0A99, 4);            // do_damage(src, dst, quantity, vec*)
    ovr(delayedDespawn, STUB143, 0x008E, 0x1F15, 2);       // delayedDespawn(src, ship)
    ovr(fireGunFromShip, STUB143, 0x012E, 0x2978, 2);      // fireGunFromShip(ship, gun)
    ovr(maybe_fire_all_guns, STUB143, 0x0156, 0x2E76, 1);  // maybe_fire_all_guns(ship)
    ovr(aiSetSpeed, STUB143, 0x010B, 0x0874);              // ship_stats_every_frame+26E (AI set speed)
    ovr(aiSetSpeedReturn, STUB143, 0x010B, 0x0918);        // ship_stats_every_frame+312 (early return)
    // overlay 145: mission spawning
    ovr(outerSpawnShipEntity, STUB145, 0x00B6, 0x115D, 2); // outerSpawnShipEntity(missionShip, situation)
    // overlay 140: entities
    ovr(despawn, STUB140, 0x01DD, 0x1C16, 1);              // despawn(ship)
    // overlay 163: AI
    ovr(aiShipThink, STUB163, 0x00CA, 0x160E, 1);          // per-frame ship AI (from ovr141:28D0)
    // overlay 134: VDU / comms
    ovr(showCommMessage, STUB134, 0x025F, 0x33ED, 2);      // outerSomeCommThing(ship, msg): msg 0 shows dseg:8DF8 on the VDU
    // overlay 133: autopilot
    ovr(autoAnimation, STUB133, 0x0025, 0x0000, 3);        // autoAnimation(camShipType, camMode, duration)
    ovr(autoAnimationBody, STUB133, 0x002A, 0x0003);       // autoAnimation+3 (after push bp/mov bp,sp)
    ovr(autopilotFinished, STUB133, 0x002A, 0x05C9);       // autopilot camera finished
    // overlay 148: briefing
    ovr(briefingStarted, STUB148, 0x0020, 0x07C1);         // outerLoadBriefingAnimation entry
    // overlay 150: barracks
    ovr(enterBarracks, STUB150, 0x00AC, 0x105D);           // enterBarracks
    ovr(enterBarracksReturn, STUB150, 0x00AC, 0x1391);     // its epilogue (AX = 7: fly)
    // overlay 161: mission lifecycle
    ovr(missionStarting, STUB161, 0x0020, 0x0470);         // runHangarMission+7C: mission about to start
    ovr(missionVictoryCalc, STUB161, 0x0020, 0x0251);      // compute_victory+70: mission over, computing score
    ovr(missionEnded, STUB161, 0x0020, 0x04DD);            // runHangarMission+E9: mission ended
    ovr(runHangarMission, STUB161, 0x0039, 0x03F4);        // runHangarMission
    // overlay 162: simulator
    ovr(simulatorStart, STUB162, 0x002A, 0x112E);          // simulator
    ovr(simulatorEnd, STUB162, 0x002A, 0x132C);            // simulator+1FE
    // overlay 168: asteroid and mine fields
    ovr(clearFields, STUB168, 0x003E, 0x01AE);             // remove the current field's rocks or mines
    // root image
    root(mainLoopTop, SEG001, 0x20E3);                     // main_loop+9A: top of the in-flight frame loop
    root(statusCheckAfterKeys, SEG001, 0x20F4);            // main_loop+AB: cmp missionStatus after handle_key
    root(statusCheckAfterFrame, SEG001, 0x2108);           // main_loop+BF: cmp missionStatus after the frame
    root(statusSetByExitKey, SEG001, 0x20F2);              // main_loop+A9: after missionStatus = EndExit
    root(skipOrchestra, SEG001, 0x04F2);                   // possible_main+358: far call we skip (orchestra)
    root(afterStartup, SEG001, 0x0512);                    // possible_main+378: program started
    root(afterHangarMission, SEG001, 0x0536);              // possible_main+39C: runHangarMission returned
    root(autopilotKey, SEG001, 0x1695);                    // handle_key+12E: autopilot key far call
}

// WC2.EXE: see docs/wc2-port.md for how each was found (scripts/wcmap.py
// pairs the functions with WC.EXE's).  Many of them are pascal.
static void load_wc2_code() {
    using namespace code;
    enum { SEG001 = 0x03CA, SEG005 = 0x073C, SEG006 = 0x0BD7, STUB107 = 0x1743, STUB114 = 0x1764, STUB116 = 0x1783,
           STUB128 = 0x17D6, STUB129 = 0x17DB, STUB134 = 0x1826, STUB141 = 0x1850 };
    // overlay 114: ships, damage, weapons
    ovr(do_damage, STUB114, 0x00AC, 0x1128, 4, true);        // do_damage(src, dst, quantity, vec*)
    ovr(delayedDespawn, STUB114, 0x00B6, 0x2B10, 2, true);   // destroy(src, ship): wrapper of ovr114:2B69
    ovr(fireGunFromShip, STUB114, 0x0183, 0x3847, 2, true);  // fireGunFromShip(ship, gun)
    ovr(aiSetSpeed, STUB114, 0x013D, 0x0E5A, 3, true);       // AI set speed (ship, ...)
    // overlay 116: mission spawning
    ovr(outerSpawnShipEntity, STUB116, 0x002A, 0x1CED, 2);   // outerSpawnShipEntity(missionShip, navPoint)
    // root: entities
    root(despawn, SEG006, 0x1AB7, 1);                        // despawn(ship)
    // overlay 141: the per-entity AI dispatcher (fighters go on to ovr129:150E,
    // capital ships to ovr129:1F13, ...)
    ovr(aiShipThink, STUB141, 0x005C, 0x2CD3, 1);            // entity_ai(slot)
    // overlay 107: autopilot
    ovr(autoAnimation, STUB107, 0x0025, 0x0000, 3);          // autoAnimation(camShipType, camMode, duration)
    // overlay 128: the campaign loop around a flight (ovr128:02CE)
    ovr(missionStarting, STUB128, 0x0039, 0x0486);           // about to load the mission and fly
    ovr(missionEnded, STUB128, 0x0039, 0x04C3);              // the flight loop returned
    // overlay 134: asteroid and mine fields
    ovr(clearFields, STUB134, 0x004D, 0x01B8);               // remove the current field's rocks or mines
    // root image: the flight loop (seg001:1BED)
    root(mainLoopTop, SEG001, 0x1CA3);                       // top of the in-flight frame loop
    root(statusCheckAfterFrame, SEG001, 0x1CF2);             // cmp missionStatus after the frame
    root(statusCheckAfterKeys, SEG001, 0x1D07);              // the loop's own test of missionStatus
}

// ---------------------------------------------------------------------------
// Recognising the program

struct KnownGame {
    GameId id;
    const char *exe;         // file name, upper case
    Bit16u dgroupPara;       // DGROUP's paragraph in the image
    Bit16u signatureOff;     // a string in DGROUP that tells this build from another
    const char *signature;
    const char *title;
};

static const KnownGame kGames[] = {
    { GAME_WC1, "WC.EXE", 0x1231, 0x0187, "Loading WING COMMANDER", "Wing Commander" },
    { GAME_WC2, "WC2.EXE", 0x1976, 0x8D65, "Origin", "Wing Commander II" },
};

const char *game_name() {
    for (size_t i = 0; i < sizeof(kGames) / sizeof(kGames[0]); i++) {
        if (kGames[i].id == g_game) {
            return kGames[i].title;
        }
    }
    return "";
}

static void set_game(const KnownGame *game, Bit16u loadSeg, Bit16u psp) {
    g_game = game ? game->id : GAME_NONE;
    g_loadSeg = game ? loadSeg : 0;
    g_gamePsp = game ? psp : 0;
    DS = game ? (Bit16u)(loadSeg + game->dgroupPara) : 0;
    DS_OFF = (PhysPt)DS * 0x10;
    load_ds(g_game);
    g_params = g_game == GAME_WC2 ? kWc2Params : kWc1Params;
    clear_code();
    if (g_game == GAME_WC1) {
        load_wc1_code();
    } else if (g_game == GAME_WC2) {
        load_wc2_code();
    }
    hooks_game_changed();
}

void game_program_loaded(const char *path, Bit16u loadSeg, Bit16u psp) {
    std::string name = path ? path : "";
    size_t cut = name.find_last_of("\\/:");
    if (cut != std::string::npos) {
        name = name.substr(cut + 1);
    }
    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] >= 'a' && name[i] <= 'z') {
            name[i] = (char)(name[i] - 'a' + 'A');
        }
    }
    for (size_t i = 0; i < sizeof(kGames) / sizeof(kGames[0]); i++) {
        const KnownGame &g = kGames[i];
        if (name != g.exe) {
            continue;
        }
        PhysPt at = ((PhysPt)(loadSeg + g.dgroupPara)) * 0x10 + g.signatureOff;
        size_t len = strlen(g.signature);
        bool same = true;
        for (size_t k = 0; k < len && same; k++) {
            same = mem_readb(at + (PhysPt)k) == (Bit8u)g.signature[k];
        }
        if (!same) {
            wclog(0, "%s is not the %s build the hooks know: no multiplayer", name.c_str(), g.title);
            continue;
        }
        set_game(&g, loadSeg, psp);
        wclog(1, "%s loaded at %04X (data segment %04X)", g.title, loadSeg, DS);
        return;
    }
    // Anything else: a child of the game (it stays), or another program.
}

void game_program_ended(Bit16u psp) {
    if (g_game != GAME_NONE && psp == g_gamePsp) {
        wclog(1, "%s ended", game_name());
        set_game(NULL, 0, 0);
    }
}

}  // namespace wc

void wc_net_program_loaded(const char *path, Bit16u loadSeg, Bit16u psp) {
    wc::game_program_loaded(path, loadSeg, psp);
}

void wc_net_program_ended(Bit16u psp) {
    wc::game_program_ended(psp);
}
