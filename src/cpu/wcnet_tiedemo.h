/*
 *  The TIE Fighter demo (LucasArts, early 1994): TIEMISS.EXE, the demo's flight program, a 16-bit real-mode program
 *  (Microsoft C, overlays in the EXE, EMS).  docs/tiedemo.md.
 *
 *  A module of its own, as Privateer's is (wcnet_priv.cpp): DOS loads TIEMISS.EXE (TIEDEMO.BAT: "tiemiss demo"), the
 *  layer knows its load segment from the start, and the build is checked by its version string in the image.  It is
 *  an earlier build of the TIE Fighter the CD version's port knows (wcnet_tie*.cpp, a 32-bit DOS/4GW program): the same
 *  functions (OpenTIE's names), the same object records (FlightObject, 88 bytes, 116 of them), in 16 bits.  The
 *  session (wcnet_tiedemo_session.cpp) is the CD port's, ported: ownership, rates, puppets, shots, hits, deaths, the
 *  room's end.
 */
#ifndef WCNET_TIEDEMO_H_
#define WCNET_TIEDEMO_H_

#include <string>
#include "dosbox.h"
#include "mem.h"

namespace wc {

// The dispatcher's (wcnet_game.cpp, wcnet_hooks.cpp).
void tdm_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp);  // DOS loaded a program
void tdm_tick();                                    // every ~1000 instructions
bool tdm_found();                                   // TIEMISS.EXE loaded and known
bool tdm_watching();                                // ... or the first release's Neon showroom (NEON.EXE) running
bool tdm_check_hooks(Bit32u ip);                    // a watched IP came up: true when it was one of ours
void tdm_watch_list(void (*watch)(Bit16u ip));      // our IPs, for the dispatcher's table
bool tdm_script_item(const std::string &item, double t);   // key-script aids (!tdm...)
bool tdm_key(int key, bool pressed);                // a key on its way to the game: true to keep it (Esc in TIEMISS)
bool tdm_controls_key(int key, bool pressed);       // ... the flight's keys (wcnet_tiedemo_controls.cpp): arrows, Ins/Del, Space
void tdm_controls_frame();                          // the top of each flight frame: the keys' mickeys
bool tdm_controls_no_bank();                        // at the bank into a turn: true to step over it (point to aim)
void tdm_controls_steer(const std::string &spec); // test aid (the video rig, !tdmsteer=): fly at a craft, fire
void tdm_program_ended(Bit16u psp);                 // a program ended (its exit code still to be read: the showroom's)

// The flight's tables: offsets in the data segment (DGROUP), and fields (docs/tiedemo.md section 4).
namespace tdm {
enum {
    kObjects = 0x67CA,          // FlightObject objects[116], 88 bytes each (the CD's layout: +4 ship_idx, +6 x y z ...)
    kObjectSize = 0x58,
    kObjectCount = 0x74,
    kCraftSlots = 0x1C,         // NUM_CRAFTS: objects 0..27 are craft (each its CraftData, +0x54), from 28 projectiles
    kPlayer = 0xCD78,           // pstate: +0 the player's object (near), +2 its craft, +4 its object number,
    kPlayerObj = 0xCD7C,
    kTarget = 0xCD84,           //   +0x0C its target (0xFFFF none)
    kFrameticks = 0xCE44,       // frameticks (236 a second), framerate at 0xCE48
    kMissionStatus = 0x614C,    // the mission's state: the player's status (the CD's +0x248), the end flag the flight's
    kMissionEnd = 0x614D,       //   frame runs until (the CD's +0x249: set, the flight ends at once)
    kSeed = 0x5372,             // math2_randomseed (0x2357 at first), and the second generator's state
    kSeed2 = 0x1A70,
    kWarheads = 0xBF9C,         // a projectile's record (+0x54): kWarheads + 4 * (its slot - 28)
    // The collision test's bolt (collide_collisions, 05B3:0745: the CD's laserx.. and laserxold..), int32 each: where
    // the bolt is and where it was a frame before; and the impact along that way, from its start (int16 each: the hit
    // test's, 05B3:15BF; the CD's collidexoff..).  collide_laserhitcraft reads them, not the bolt: the side struck of
    // the player's craft from the bolt's way, the flash put at the start plus the impact (05B3:1C02).
    kLaserX = 0x65D8,
    kLaserY = 0x65DE,
    kLaserZ = 0x677A,
    kLaserXOld = 0x620C,
    kLaserYOld = 0x6218,
    kLaserZOld = 0x6224,
    kCollideXOff = 0xBF92,
    kCollideYOff = 0xBF9A,
    kCollideZOff = 0xC06A,
    kFgSeg = 0x5890,            // the mission's segment (a word): the flight groups from its 0xD0, 0x124 each
    kFgBase = 0xD0,
    kFgSize = 0x124,
    kFgCraft = 0x33,            //   +0x33 its craft, +0x42 the player's craft in it (1-based; 0 none)
    kFgPlayerCraft = 0x42,
    kFgCount = 48,
    // a craft's data (CraftData, 0x18C bytes: the CD's, shorter)
    kCraftAiCountdown = 0x2C,   // the planner runs it when this is <= 0 (pai_updateplaneai, 091E:01CE)
    kCraftAttacker = 0x40,
    kCraftFlightFlag = 0x5C,    // (3 and 4: docking, destroyed)
    kCraftHull = 0x9E,          // its damage (collide_damagecraft adds), dead at kCraftHullMax
    kCraftHullMax = 0xA2,
    kCraftSystems = 0xA8,
    kCraftShieldFront = 0xC2,
    kCraftShieldRear = 0xC4,
};
}  // namespace tdm

Bit16u tdm_load_segment();              // TIEMISS's load segment (its image's paragraph 0)
PhysPt tdm_ds(Bit16u off);              // DS:off as a linear address
PhysPt tdm_obj(int o);                  // object o's record
PhysPt tdm_craft(int o);                // its craft data (0: none: not a craft slot, or none)
bool tdm_in_flight();                   // the flight's frames are coming and its end flag is clear
int tdm_player_object();                // our object (-1: none)
double tdm_frame_ms();                  // a frame's length, measured

// Calls into the program (Microsoft C's far cdecl: the arguments pushed last first, the caller pops them), made at
// move_moveobjects's entry as the CD port's tie_queue_call makes its: the CPU's state kept, the arguments and the
// place's own address pushed, CS:IP at the function; the place's hook, reached again by its return, gives the state
// back and makes the next call or lets the frame go on.  CPU state only: no code is written.
enum TdmFn { TDM_CREATE_PROJECTILE, TDM_LASER_HIT, TDM_DAMAGE, TDM_UPDATE_KILLS, TDM_BOARD };
// `before` runs just before the call is made (to write what it needs at that moment; it may change an argument);
// false: the call is not made.  `done` with its result (AX).
typedef bool (*TdmCallBefore)(int tag, Bit16u *args);
typedef void (*TdmCallDone)(int tag, Bit16u ax);
void tdm_queue_call(TdmFn fn, int nargs, Bit16u a0, Bit16u a1, Bit16u a2, Bit16u a3, TdmCallBefore before, TdmCallDone done, int tag);
bool tdm_in_call();
// At a hooked function's entry: its k-th argument; return to its caller at once with AX.
Bit16u tdm_arg(int k);
void tdm_skip_call(Bit16u ax);

// The session's (wcnet_tiedemo_session.cpp), for the game's hooks.
void tdm_session_tick();                // every ~1000 instructions
void tdm_session_shutdown();
bool tdm_session_launch_hold();         // at create_createmission's entry (polled while held): true to hold there
void tdm_session_before_create();       // ... its mission file read (0FC2:0136): a guest's seed and seat written
void tdm_session_frame();               // the top of each flight frame (before the AI's planner)
void tdm_session_before_move();         // move_moveobjects's entry: the others' craft written in
void tdm_session_shot();                // laser_createprojectile's entry
void tdm_session_laser_hit();           // collide_laserhitcraft's entry
void tdm_session_damage();              // collide_damagecraft's entry
void tdm_session_board();               // paiman_boardmaneuver's entry (a boarding craft's dock and transfer)
void tdm_session_flight_ended();
// ... for the campaign: who decides (a guest follows the host's go and verdicts; solo and host decide)
enum TdmRole { TDM_SOLO, TDM_HOST, TDM_GUEST };
TdmRole tdm_session_role();
// A craft slot's identity in the room (its flight group << 8 | its arrival in the group; 0 none): the aids'.
unsigned tdm_session_ident(int obj);
int tdm_session_launch_mission();       // the mission the host's latest go launched
bool tdm_session_room_ended();          // this flight ended by the room (a pilot down, the host's flight over)
void tdm_session_verdict(int next, bool over);          // the host's verdict on its flight, sent
bool tdm_session_verdict_wait(int *next, bool *over);   // a guest: true while the host's verdict on our flight is to come
void tdm_notice(const std::string &text);               // a line for the log (the room's: never over the picture)
bool tdm_session_fly_alone();             // a guest's launch held for a host already flying: fly alone (Esc)
bool tdm_session_active();                // in a room (the comms prompt, the 0 key in flight, has somebody to tell)
void tdm_session_chat(const std::string &text);   // a line typed at the prompt: to the room, and over our picture

// The campaign (wcnet_tiedemo_campaign.cpp, docs/tiedemo.md section 8): after the demo's own mission, the missions of
// scripts/tdmmissions.py, written over the mission the game has read; the flight ended by the layer once won or
// failed, and the game's own loop (a death's: fly again) flies the next.
void tdm_campaign_loaded();             // TIEMISS.EXE loaded (the interceptor's and bomber's cockpit named)
int tdm_campaign_next();                // the mission this machine launches next (0: the demo's own)
void tdm_campaign_mission_read();       // 0FC2:0136, before the session's seat and seeds: the mission written
void tdm_campaign_frame();              // the top of each flight frame
void tdm_campaign_flight_ending();      // the room has decided this flight's end (the session's room_end)
void tdm_campaign_host_verdict(int next, bool over);    // a guest: the host's verdict on the flight under way
bool tdm_campaign_flight_over();        // 0090:04FB, the flight's frames over: true to be held there (a guest
                                        //   waiting for the host's verdict); else the status written
bool tdm_campaign_script_item(const std::string &item, double t);

}  // namespace wc

#endif
