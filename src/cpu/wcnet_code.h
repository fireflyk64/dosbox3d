/*
 *  Wing Commander 1 code addresses used by the multiplayer hooks.
 *
 *  WC.EXE is a Borland overlay program.  Overlay functions are reached through
 *  a stub segment holding `jmp far` thunks (one every 5 bytes from offset
 *  0x20); the overlay code itself is loaded at a run-time dependent segment,
 *  so a hook on an overlay function compares CS against the segment currently
 *  stored in the thunk and IP against the function's fixed offset.
 *
 *  Address forms (all resolvable with scripts/wcdis.py):
 *     stub143:0084  -> DOS 12D7:0084 (DOS segment = IDA segment + 0x1A2)
 *     ovr143:0A99   -> offset inside the overlay; this is the name in wc.idb
 *     seg001:20E3   -> DOS 0560:20E3, root image (fixed segment)
 */
#ifndef WCNET_CODE_H_
#define WCNET_CODE_H_

#include "dosbox.h"

namespace wc {
namespace code {

// DOS segments of the root image and of the overlay stubs (IDA seg + 0x1A2).
enum Segment {
    SEG000 = 0x01a2,
    SEG001 = 0x0560,
    SEG002 = 0x078c,
    STUB133 = 0x1266,
    STUB134 = 0x126a,
    STUB140 = 0x12ad,
    STUB141 = 0x12cc,
    STUB142 = 0x12d4,
    STUB143 = 0x12d7,
    STUB144 = 0x12ed,
    STUB145 = 0x12f2,
    STUB146 = 0x12fe,
    STUB147 = 0x130e,
    STUB148 = 0x1318,
    STUB150 = 0x1327,
    STUB151 = 0x1333,
    STUB161 = 0x1361,
    STUB162 = 0x1366,
    STUB163 = 0x1370,
    STUB164 = 0x1381,
};

// A function that lives in an overlay, identified by its stub thunk.
struct OverlayFn {
    Bit16u stubSeg;   // DOS segment of the stub
    Bit16u stubOff;   // offset of the `jmp far` thunk inside the stub
    Bit16u ovrOff;    // offset of the function inside the overlay (ovrNNN:ovrOff)
    const char *name; // wc.idb name
};

// A location inside an overlay that is not a thunk target (mid-function hook
// points); the overlay's segment is taken from any thunk of that overlay.
struct OverlayLoc {
    Bit16u stubSeg;
    Bit16u anyStubOff; // a thunk of the same overlay used to learn its segment
    Bit16u ovrOff;
    const char *name;
};

// A location in the root image (fixed segment).
struct RootLoc {
    Bit16u seg;
    Bit16u off;
    const char *name;
};

// --- overlay 143: ships, damage, weapons -----------------------------------
static const OverlayFn do_damage             = { STUB143, 0x0084, 0x0A99, "do_damage(src, dst, quantity, vec*)" };
static const OverlayFn delayedDespawn        = { STUB143, 0x008E, 0x1F15, "delayedDespawn(src, ship)" };
static const OverlayFn fireGunFromShip       = { STUB143, 0x012E, 0x2978, "fireGunFromShip(ship, gun)" };
static const OverlayFn maybe_fire_all_guns   = { STUB143, 0x0156, 0x2E76, "maybe_fire_all_guns(ship)" };
static const OverlayFn aiSetSpeed            = { STUB143, 0x010B, 0x0874, "ship_stats_every_frame+26E (AI set speed)" };
static const OverlayLoc aiSetSpeedReturn     = { STUB143, 0x010B, 0x0918, "ship_stats_every_frame+312 (early return)" };
// --- overlay 145: mission spawning ------------------------------------------
static const OverlayFn outerSpawnShipEntity  = { STUB145, 0x00B6, 0x115D, "outerSpawnShipEntity(missionShip, situation)" };
// --- overlay 140: entities -----------------------------------------------
static const OverlayFn despawn               = { STUB140, 0x01DD, 0x1C16, "despawn(ship)" };
// --- overlay 163: AI -------------------------------------------------------
static const OverlayFn aiShipThink           = { STUB163, 0x00CA, 0x160E, "ovr163:160E per-frame ship AI (from ovr141:28D0)" };
// --- overlay 134: VDU / comms ---------------------------------------------
static const OverlayFn showCommMessage       = { STUB134, 0x025F, 0x33ED, "outerSomeCommThing(ship, msg): msg 0 shows dseg:8DF8 on the VDU" };
// --- overlay 133: autopilot -----------------------------------------------
static const OverlayFn autoAnimation         = { STUB133, 0x0025, 0x0000, "autoAnimation(camShipType, camMode, duration)" };
static const OverlayLoc autoAnimationBody    = { STUB133, 0x002A, 0x0003, "autoAnimation+3 (after push bp/mov bp,sp)" };
static const OverlayLoc autopilotFinished    = { STUB133, 0x002A, 0x05C9, "ovr133:05C9 autopilot camera finished" };
// --- overlay 148/151: briefing ---------------------------------------------
static const OverlayFn outerLoadBriefingAnimation = { STUB148, 0x005C, 0x07C1, "outerLoadBriefingAnimation(mission, series)" };
static const OverlayLoc briefingStarted      = { STUB148, 0x0020, 0x07C1, "outerLoadBriefingAnimation entry" };
static const OverlayFn loadScrambleAnimation = { STUB151, 0x0025, 0x01CA, "loadScambleAnimation" };
// --- overlay 150: barracks --------------------------------------------------
static const OverlayFn enterBarracks         = { STUB150, 0x00AC, 0x105D, "enterBarracks" };
// --- overlay 161: mission lifecycle ------------------------------------------
static const OverlayLoc missionStarting      = { STUB161, 0x0020, 0x0470, "runHangarMission+7C: mission about to start" };
static const OverlayLoc missionVictoryCalc   = { STUB161, 0x0020, 0x0251, "compute_victory+70: mission over, computing score" };
static const OverlayLoc missionEnded         = { STUB161, 0x0020, 0x04DD, "runHangarMission+E9: mission ended" };
static const OverlayFn runHangarMission      = { STUB161, 0x0039, 0x03F4, "runHangarMission" };
// --- overlay 162: simulator ---------------------------------------------------
static const OverlayLoc simulatorStart       = { STUB162, 0x002A, 0x112E, "simulator" };
static const OverlayLoc simulatorEnd         = { STUB162, 0x002A, 0x132C, "simulator+1FE" };

// --- root image ---------------------------------------------------------------
static const RootLoc mainLoopTop             = { SEG001, 0x20E3, "main_loop+9A: top of the in-flight frame loop" };
static const RootLoc skipOrchestra           = { SEG001, 0x04F2, "possible_main+358: far call we skip (orchestra)" };
static const RootLoc afterStartup            = { SEG001, 0x0512, "possible_main+378: program started" };
static const RootLoc afterHangarMission      = { SEG001, 0x0536, "possible_main+39C: runHangarMission returned (nonzero: next mission, zero: back to the title)" };
static const RootLoc autopilotKey            = { SEG001, 0x1695, "handle_key+12E: autopilot key far call" };

}  // namespace code
}  // namespace wc

#endif
