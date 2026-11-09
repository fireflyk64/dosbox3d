/*
 *  TIE Fighter (LucasArts, the 1995 Collector's CD-ROM): Z_TIE__.EXE, a
 *  32-bit flat program under DOS/4GW.  docs/tiefighter.md.
 *
 *  The Wing Commanders are real-mode programs DOS loads, so DOS tells the
 *  layer where they are (game_program_loaded).  TIE Fighter's launcher
 *  starts DOS4GW.EXE, and the extender loads the LE image itself, into
 *  extended memory, at a place DOS never hears of: once DOS4GW.EXE has
 *  started, the layer looks for the program now and then (tie_tick), by the
 *  bytes of its data object.  Found, it is watched at a few of its functions
 *  through wc_net_watch32 (the per-instruction hook's table for 32-bit
 *  instruction pointers); nothing of the Wing Commander layer applies to it
 *  (DS stays 0 and g_game GAME_NONE) until it has a session of its own.
 */
#ifndef WCNET_TIE_H_
#define WCNET_TIE_H_

#include <string>
#include <vector>
#include "dosbox.h"

namespace wc {

// DOS loaded a program (its name without the path, upper case) for the
// process psp; that process ended (true: it was TIE Fighter's).
void tie_program_loaded(const char *name, Bit16u psp);
bool tie_program_ended(Bit16u psp);

// The periodic tick (every 999 instructions): the search for the program
// while it is awaited, nothing once it is found or when nothing is awaited.
void tie_tick();

// The program is in memory.  Its image addresses (the LE's: code from
// 0x10000, data from 0xC0000, the names OpenTIE gives them) as the linear
// addresses they are loaded at.
bool tie_found();
Bit32u tie_linear(Bit32u image);
// Its memory, by image address (nothing while it is not found: reads 0).
Bit8u tie_r8(Bit32u image);
Bit16u tie_r16(Bit32u image);
Bit32u tie_r32(Bit32u image);
void tie_w8(Bit32u image, Bit8u v);
void tie_w16(Bit32u image, Bit16u v);
void tie_w32(Bit32u image, Bit32u v);
// A flight runs (the game's maingameflag), and the player's object slot
// (-1: none).
bool tie_in_flight();
int tie_player_object();
// The front end's scene (OpenTIE's SCENE_*: 100 the registration desk, 110
// the concourse, 131/132 the Combat Chamber, 160 the tour desk, 179..183 the
// briefing, 190.. the debriefing, 2..4 a flight), or -1: none known.
int tie_scene();
// The mission the front end launches (shipext's mission_name: the mission
// file's name, without its extension), or "" when none is known.
std::string tie_mission_name();

// The flight's tables (image addresses; docs/tiefighter.md section 3).
namespace tie {
enum {
    kObjects = 0xE38BC,         // FlightObject objects[120], 88 bytes each
    kObjectSize = 88,
    kObjectCount = 120,
    kStatics = 0xEA6D4,         // StaticObject staticobjects[64], 18 bytes each: +2 class (8 a mine), +3 species (0: none), +4 x y z (int16, << 8)
    kStaticSize = 18,
    kStaticCount = 64,
    kStaticRef = 0x3800,        // (a static object's number as a target: 0x3800 + its index)
    kCrafts = 0xE6394,          // CraftData crafts[32], 0x21A bytes each
    kCraftSize = 0x21A,
    kPstate = 0xEB158,          // +8 the player's object slot, +0x12 the target's
    kFramerate = 0xEB70C,
    kFrameticks = 0xEB712,
    kHyperspaceFlag = 0xEB75D,
    kMainGameFlag = 0xEB763,
    kMissionHeader = 0xE3534,   // int16 the flight groups' number first
    kFlightGroups = 0xDF6CC,    // EFGStruct fg_array[48], 0x124 each: +0x33 craft, +0x42 the player's craft (1-based)
    kFlightGroupSize = 0x124,
    // The shell's scene head (shellext_Get_Cur_Scene at 0x6621B reads it: `mov eax,[sHead]; mov ax,[eax+0x14]`, the
    // address a fixup into the data object, 0x356F8 of it): its +0x14 the scene.
    kSceneHead = 0xF56F8,
    kSceneHeadScene = 0x14,
    // shipext's mission_name, 14 bytes (shipext_Set_Mission_Name 0x822B4): the Combat Chamber fills it from the craft and
    // mission chosen (shipext_Get_Combat_Mission_Name 0x8149C), the tour desk from the battle's
    kMissionName = 0xFB3E8,
};
}  // namespace tie

// The per-instruction dispatcher's call when EIP's low word is watched.
void tie_check_hooks(Bit32u eip);

// Calls into the program (docs/tiefighter.md, step 6), made at the frame's
// hook place (move_moveobjects's entry): the CPU's state kept, the place
// pushed as the return address, the arguments in eax, edx, ebx, ecx
// (Watcom's register convention), EIP at the function; the place's hook,
// reached again by the return, gives the state back and makes the next call
// or lets the frame go on.  CPU state only: no code is written.
typedef void (*TieCallDone)(int tag, Bit32u eax);
// `before` runs just before the call is made, the arguments already in the
// registers (to write what it needs into the program's memory at that very
// moment, and to change an argument); false: the call is not made.  `done`
// with its result.
typedef bool (*TieCallBefore)(int tag);
void tie_queue_call(Bit32u fnImage, Bit32u eax, Bit32u edx, Bit32u ebx, Bit32u ecx, TieCallBefore before, TieCallDone done, int tag);
// Inside a call of ours (and whatever it calls).
bool tie_in_call();
// At a function's entry (its arguments in registers): return to its caller
// at once with eax.
void tie_skip_call(Bit32u eax);
// At a function's entry: back to the near call that made it, so the program makes it again (false: not a near call).
bool tie_repeat_call();

// The flight's controls (wcnet_tie_controls.cpp, step 7): the arrows steer
// as RetroVega's, the keypad's Ins and Del roll, F12 switches the mouse
// between the game's own drag-to-steer and a pointer that aims.
bool tie_controls_key(int kbdKey, bool pressed);     // true: taken from the game
bool tie_controls_mouse(float xrel, float yrel);     // true: moved our pointer instead of the game's stick
void tie_controls_frame();                           // at the frame's top: this frame's mickeys
int tie_controls_axis(bool across, int delta, int scale, int game);   // the stick's axis scaled, signed (the game's: `game`)
bool tie_controls_marker(double *x, double *y);      // the pointer to draw, -1..1 from the middle
void tie_controls_set_mouse(int mode);              // 0 drag, 1 pointer, 2 keyboard, 3 joystick
void tie_controls_stick(double x, double y);         // a pad's stick, -1..1, right and climbing positive

// The Imperial shuttle's cockpit, made from the gunboat's (wcnet_tie_cockpit.cpp, step 9): at fediskio_loadspecies's
// entry, the mission read and its craft not yet loaded.  WCNET_TIE_FLY_AS=<craft type> flies the player's flight group
// in that craft (17 the shuttle).
void tie_cockpit_before_species();
void tie_cockpit_forget();
// A flight group flown in another craft (the mission files' type number), before the flight loads its craft.
void tie_fly_group_as(int fg, int type);
// A file of the game's own drive (its DOS name, e.g. MISSION\\HG1W.TIE), read from the host; false if not there.
bool tie_game_file(const std::string &dosName, std::vector<Bit8u> &out);
// The game's drive, its index in Drives (-1 if not found); a file written to it (its DOS name), false if not.
int tie_game_drive();
bool tie_game_write(const std::string &dosName, const std::vector<Bit8u> &bytes);

// The Z-95 challenge's radio voices (wcnet_tie_voice.cpp): a line of a mission (its file's name) and its message.
struct TieVoiceLine {
    std::string mission;
    int index;
};
// The challenge's bank made from the CD's, for `lines` (its messages that have text, in order) and `own`'s win and loss
// clips; false if not made (no CD: no voices).  tie_voice_made: made.
bool tie_voice_make(const std::string &own, const std::vector<TieVoiceLine> &lines);
bool tie_voice_made();
// DOS_OpenFile's: the game's open of the bank the challenge's voice ids name made the bank made (its name and drive
// rewritten); false for every other.
bool tie_voice_open(char *fullname, Bit8u *drive);

// Test aid: the front end's pointer put at a point of its 320x200 screen
// (the key script's !tiecursor=x:y; a mouse button then clicks there).
void tie_set_cursor(int x, int y);
// Test aid: our craft turned to face an object (the key script's !tieface=<obj>); `lead`, where a laser fired now
// meets it (!tieface=target).
double tie_face(int obj, bool lead = false);    // (its distance in km, -1: nothing faced)
// The front end's pointer, in its 320x200 screen (false: no program).
bool tie_cursor(int *x, int *y);
// The ship's address system's call to the briefing room, played by the emulator (wcnet_tie_pa.cpp, step 13).
void tie_pa_play();
// Test aid: our target set (the key script's !tietarget=<obj>).
void tie_target(int obj);
// Test aid: the nearest moving craft of a ship index from `ship` to `last` (5 the T/F) on another side, -1 if none.
int tie_nearest(int ship, int last, bool any = false, int nth = 0);    // (any: still craft and our side's too: mines; nth: 1 the second nearest...)
// Test aid: where a laser fired now meets `obj` (lead), in our craft's own frame: the turn right and the climb to it
// (radians) and its distance (km); false: nothing there.
bool tie_aim(int obj, double *right, double *climb, double *km, bool lead = true);
bool tie_friend_in_line(double right, double climb, double km);   // (a craft of our side nearer, within 4 degrees of there)
bool tie_alive(int obj);
// The pilot signed in at the registration desk (shipext's pilot_name, image 0xD14D6: the .TFR file's name).
std::string tie_pilot_name();
// A craft's state as the DEMO pilot's wingman reads it (wcnet_tie_controls.cpp): false when the slot holds no ship.
struct TieObjInfo {
    double x, y, z;             // world units (40,960 a km)
    double fx, fy, fz, sx, sy, sz;  // its forward and side axes (the move vector's way for forward)
    int type, side, speed, radius, aiTarget;
    bool alive;                 // a ship (category 0), not dying
};
bool tie_obj_info(int obj, TieObjInfo *out);
// Where a world direction lies from our nose: the turn right and the climb to it (radians); false: no craft of ours.
bool tie_aim_dir(double dx, double dy, double dz, double *right, double *climb);
int tie_nearest_mine();      // (the nearest mine still there, as a target number: 0x3800 + its index; -1 none)
int tie_mines();             // (the mines still there)     // (a craft there, not dying: its +0x2C death timer 0)
int tie_speed(int obj);      // (its +0x26)
void tie_controls_steer(const std::string &spec);
void tie_controls_god(bool on);     // the key script's !tiegod= (a rig's measurement: no damage to our craft)
bool tie_controls_godly();   // the key script's !tiesteer= (wcnet_tie_controls.cpp)
// Our target, -1 if none (the key script's !tieface=target).
int tie_player_target();

}  // namespace wc

#endif
