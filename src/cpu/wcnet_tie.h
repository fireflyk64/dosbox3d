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

// Test aid: the front end's pointer put at a point of its 320x200 screen
// (the key script's !tiecursor=x:y; a mouse button then clicks there).
void tie_set_cursor(int x, int y);

}  // namespace wc

#endif
