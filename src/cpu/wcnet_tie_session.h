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

namespace wc {

// The periodic tick (wcnet_tie.cpp's, every 999 instructions): the room
// joined, hellos answered, frames taken in, notices shown.
void tie_session_tick();
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
// At create_createmission's entry: the player's flight group gets a craft a
// seat, and a guest flies its own one of them.
void tie_session_mission_create();
// The flight is over (maingameflag off) or the program ended.
void tie_session_flight_ended();
void tie_session_shutdown();
bool tie_session_active();
// An object flown by another pilot's machine.
bool tie_is_puppet(int obj);

}  // namespace wc

#endif
