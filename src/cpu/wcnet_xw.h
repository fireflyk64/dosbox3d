/*
 *  X-Wing Collector's CD-ROM (LucasArts, 1994): BWING.EXE, a 16-bit real-mode program (Microsoft C, .RTLink/Plus
 *  overlays: BFRONT.OVL the front end, BFLIGHT.OVL the flight), EMS.  docs/xwing.md.
 *
 *  A module of its own, as the TIE Fighter demo's is (wcnet_tiedemo.cpp), and made from it: the CD's flight engine is
 *  the TIE demo's ("X-Wing CD uses a new graphics system, similar to the one used in TIE Fighter", its READ.ME), an
 *  earlier build: the same functions in the same order, the object record the demo's less its first word (84 bytes,
 *  116 of them), the craft data shorter.  BFLIGHT.OVL is loaded at a fixed place (its first code segment the load
 *  segment + 0920), so its places are paragraphs from the load segment like the root's; it is there only while a
 *  mission is flown (the front end's overlay has the same place), so its code is checked before any hook is believed.
 */
#ifndef WCNET_XW_H_
#define WCNET_XW_H_

#include <string>
#include "dosbox.h"
#include "mem.h"

namespace wc {

// The dispatcher's (wcnet_game.cpp, wcnet_hooks.cpp).
void xw_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp);  // DOS loaded a program
void xw_tick();                                    // every ~1000 instructions
bool xw_found();                                   // BWING.EXE loaded
bool xw_check_hooks(Bit32u ip);                    // a watched IP came up: true when it was one of ours
void watch_at(Bit16u seg, Bit16u ip);              // (wcnet_hooks.cpp: a place watched in its own code segment only)
void xw_watch_list(void (*watch)(Bit16u ip));      // our IPs, for the dispatcher's table
bool xw_script_item(const std::string &item, double t);   // key-script aids (!xw...)
bool xw_key(int key, bool pressed);                // a key on its way to the game: true to keep it

// The flight's tables: offsets in the data segment (DGROUP), and fields (docs/xwing.md section 3).
namespace xw {
enum {
    kObjects = 0x9886,          // the objects, 116 of 84 bytes: the TIE demo's FlightObject less its first word
    kObjectSize = 0x54,
    kObjectCount = 0x74,
    kCraftSlots = 0x1C,         // objects 0..27 are craft (each its craft data, +0x50 near, +0x52 its segment)
    // an object's fields
    kOCat = 0x00,               // 0 a craft, 1 a projectile, 5 an explosion
    kOGenus = 0x01,
    kOType = 0x02,              // its species (0 a free slot): 1 X-W, 2 Y-W, 3 A-W, 4 T/F ... (the mission files')
    kOPos = 0x04,               // x, y, z (int32)
    kOPrev = 0x10,              // ... the frame before's
    kOHeading = 0x1C,
    kOPitch = 0x1E,
    kORoll = 0x20,
    kOSpeed = 0x24,
    kORadius = 0x28,            // (a bolt's: its damage)
    kODeath = 0x2A,             // its death's timer
    kOAge = 0x2C,
    kOSelf = 0x2E,              // a bolt's shooter
    kOSide = 0x31,
    kOMove = 0x36,              // the move vector (3 words)
    kOAxes = 0x3E,              // forward, side, up (9 words)
    kOCraft = 0x50,             // the craft data: near, then DS
    // the player
    kPlayer = 0xBFAC,           // +0 the player's object (near), +2 its craft, +4 its object number
    kPlayerObj = 0xBFB0,
    kPlayerFg = 0xBFB2,         // (a byte) the player's flight group
    kTarget = 0xBFB8,           //   +0x0C its target (0xFFFF none)
    kFrameticks = 0xC802,       // a frame's ticks (236 a second), the rate at 0xC948
    kMissionStatus = 0x6F5A,    // how the flight ended
    kMissionEnd = 0x6F5B,       // the flight's frames run until this is set
    kSeed = 0x5ECA,             // the random generator's state (two bytes), its last number before it
    kSeedOut = 0x5EC8,
    kWarheads = 0xC64E,         // a projectile's record (+0x50): kWarheads + 4 * (its slot - 28)
    kCurCraft = 0xC63A,         // the craft data the AI, the damage and the rest are looking at
    // the collision test's bolt (collide_collisions): where it is and was (int32 each), the impact from its start
    kLaserX = 0xD770,
    kLaserY = 0xD776,
    kLaserZ = 0xD914,
    kLaserXOld = 0xD39C,
    kLaserYOld = 0xD3AA,
    kLaserZOld = 0xD3B8,
    kCollideXOff = 0xC076,
    kCollideYOff = 0xC07C,
    kCollideZOff = 0xC080,
    // the mission as read (create_createmission): its header at DS:C72C (the file's, 0xCE bytes), its flight groups in
    // the mission segment (the load segment + kMissionPara) from 0xE8, the file's records of 0x94 bytes
    kHeader = 0xC72C,
    kNumFgs = 0xC7F6,
    kMissionPara = 0x3E3E,
    kFgBase = 0xE8,
    kFgSize = 0x94,
    kFgType = 0x32,
    kFgCraft = 0x38,            // its craft in a wave (a word)
    kFgPlayerCraft = 0x82,      // the player's craft in it (a word, 1-based; 0 none)
    kFgOrder = 0x86,
    kFgMax = 32,
    // a craft's data (0x170 bytes)
    kCraftFg = 0x05,            // its flight group (a byte)
    kCraftLeader = 0x06,
    kCraftAiReload = 0x24,      // the planner's countdown is set from this when it has run
    kCraftAiCountdown = 0x26,   // the planner (135A:011E) runs a craft when this is 0
    kCraftAttacker = 0x3C,
    kCraftFlightFlag = 0x41,    // (3 and 4: docking, destroyed)
    kCraftMode = 0x42,          // its maneuver (0x12 boarding)
    kCraftHull = 0x94,          // its damage (collide_damagecraft adds), dead at kCraftHullMax
    kCraftHullMax = 0x98,
    kCraftSystems = 0x9B,
    kCraftShieldFront = 0xB6,
    kCraftShieldRear = 0xB8,
};
}  // namespace xw

Bit16u xw_load_segment();               // BWING's load segment (its image's paragraph 0)
PhysPt xw_ds(Bit16u off);               // DS:off as a linear address
PhysPt xw_obj(int o);                   // object o's record
PhysPt xw_craft(int o);                 // its craft data (0: none: not a craft slot, or none)
bool xw_flight_loaded();                // BFLIGHT.OVL is in memory (its code checked)
bool xw_in_flight();                    // the flight's frames are coming and its end flag is clear
int xw_player_object();                 // our object (-1: none)
double xw_frame_ms();                   // a frame's length, measured
std::string xw_mission_path();          // the mission being made, less its drive ("\mission\defect.xwi"; at the launch)
void xw_set_mission_path(const std::string &path);     // ... another to make instead (a guest's: the host's)

// Calls into the program (Microsoft C's far cdecl: the arguments pushed last first, the caller pops them), made at
// move_moveobjects's entry as the TIE demo's are: the CPU's state kept, the arguments and the place's own address
// pushed, CS:IP at the function; the place's hook, reached again by its return, gives the state back and makes the
// next call or lets the frame go on.  CPU state only: no code is written.
enum XwFn { XW_CREATE_PROJECTILE, XW_LASER_HIT, XW_DAMAGE, XW_UPDATE_KILLS };
typedef bool (*XwCallBefore)(int tag, Bit16u *args);
typedef void (*XwCallDone)(int tag, Bit16u ax);
void xw_queue_call(XwFn fn, int nargs, Bit16u a0, Bit16u a1, Bit16u a2, Bit16u a3, XwCallBefore before, XwCallDone done, int tag);
bool xw_in_call();
Bit16u xw_arg(int k);                   // at a hooked function's entry: its k-th argument
void xw_skip_call(Bit16u ax);           // ... return to its caller at once with AX

// The session's (wcnet_xw_session.cpp), for the game's hooks.
void xw_session_tick();                 // every ~1000 instructions
void xw_session_shutdown();
bool xw_session_launch_hold();          // at create_createmission's entry (polled while held): true to hold there
void xw_session_before_create();        // ... its mission file read: the seeds, the seats written
void xw_session_frame();                // the top of each flight frame (before the AI's planner)
void xw_session_before_move();          // move_moveobjects's entry: the others' craft written in
void xw_session_shot();                 // laser_createprojectile's entry
void xw_session_laser_hit();            // collide_laserhitcraft's entry
void xw_session_damage();               // collide_damagecraft's entry
void xw_session_flight_ended();
unsigned xw_session_ident(int obj);     // a craft slot's identity in the room (the aids')
bool xw_session_fly_alone();            // a guest's launch held for a host already flying: fly alone (Esc)
bool xw_session_active();               // in a room
void xw_session_chat(const std::string &text);
bool xw_session_holding();              // our launch is held for the room (Esc there: fly alone)

}  // namespace wc

#endif
