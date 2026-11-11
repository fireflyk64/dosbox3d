/*
 *  TIE Fighter's session (docs/tiefighter.md, section 5): the room's pilots
 *  fly the same mission, each in a craft of the player's flight group, and
 *  every craft is flown by one machine, its owner, whose game reports it;
 *  the others write the reports into their own game (puppets).
 *
 *  Nothing of the Wing Commander session (wcnet_session.h) runs for TIE
 *  Fighter: g_session stays NULL.  The same transports carry it: a lobby
 *  room (WCROOM, player 0 the host) or TCP (WCHOST/WCPORT), as
 *  init_network() has them; without one named the game plays alone
 *  (RetroVega's netNeedsConfig).
 */
#ifndef WCNET_TIE_SESSION_H_
#define WCNET_TIE_SESSION_H_

#include "dosbox.h"
#include <string>

namespace wc {

// The periodic tick (wcnet_tie.cpp's, every 999 instructions): the room
// joined, hellos answered, frames taken in, notices shown.
void tie_session_tick();
// The other pilot nearest ours flying this mission now: its craft's object slot (-1: none) and what it has targeted
// (0xFFFF: nothing); the DEMO pilot's wingman follows and helps it (wcnet_tie_controls.cpp).
int tie_session_partner(int *target);
// At tie_updatetime, once a flight frame: our craft sent, the others'
// written into the game.
void tie_session_frame();
// At move_moveobjects's entry, once a flight frame: the others' craft
// written into the game, after its AI and dynamics, before its move.
void tie_session_before_move();
// At shipext_Get_Mission_Path's entry, just before the mission file's name
// is made from the front end's mission name: a guest launching from the
// Combat Chamber flies the host's mission.
void tie_session_mission_path();
// The room's mission (step 8): a mission file's name ("hg2w", "b5m1iw") flown whenever a pilot launches a Combat
// Chamber mission, or "" for none (the Combat Chamber's own).  WCNET_TIE_MISSION at the launch; the page's
// wc_web_tie_mission while the game runs.
void tie_session_set_mission(const char *name);
// Step 13 (the user: "it might be simpler if the host did it and guest had to wait"): at shipext_Get_Mission_Path's
// entry, true while a guest's launch is to wait for the host (the call is then made again); a key while it waits (Esc:
// fly alone), true when taken.
bool tie_session_mission_wait();
bool tie_session_key(int kbdKey, bool pressed);
// A mouse button for the game (the driver's press or release): true when taken (a guest's launch while the host does
// not fly).
bool tie_session_click(int button, bool pressed);
// The guns (step 6), at the entries of laser_createprojectile, collide_laserhitcraft and collide_damagecraft (the
// arguments in eax, edx, ebx, ecx).
void tie_session_shot();
void tie_session_laser_hit();
void tie_session_damage();
// At create_createmission's entry: the player's flight group gets a craft a
// seat, and a guest flies its own one of them.
void tie_session_mission_create();
// At fediskio_loadspecies's entry, before the cockpit's: a mission flown alone gets the user's pairing (step 9).
void tie_session_before_species();
// The flight is over (maingameflag off) or the program ended.
void tie_session_flight_ended();
void tie_session_shutdown();
bool tie_session_active();
// A line typed at the comms prompt (sdlmain.cpp's 0): to every pilot in the room, and on our screen.
void tie_session_chat(const std::string &text);
// An object flown by another pilot's machine.
bool tie_is_puppet(int obj);

}  // namespace wc

#endif
