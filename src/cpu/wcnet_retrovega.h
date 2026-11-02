/*
 *  RetroVega (RV.EXE) for the multiplayer layer: the interface check made
 *  when the program loads, and the fields of its fixed block the layer
 *  writes (retrovega/docs/multiplayer.md).  Everything RV-specific the layer
 *  does is here or behind a GameTraits flag; the offsets come from the
 *  generated wcnet_rv.h only.
 */
#ifndef WCNET_RETROVEGA_H_
#define WCNET_RETROVEGA_H_

#include <string>
#include "dosbox.h"
#include "wcnet_game.h"

namespace wc {

// RV.EXE's interface, checked against the one this layer was built from
// (wcnet_rv.h): RVHOOK's magic, ABI version, thunk and site counts at the
// image's first paragraph; at DS (dgroupPara past loadSeg) the puppet header
// and every word of the directory.  False with `why` said when anything
// differs: the layer then only watches the game.
bool rv_interface_check(Bit16u loadSeg, Bit16u dgroupPara, std::string *why);

// The running RV.EXE passed the check (set by wcnet_game.cpp at load).
bool rv_compatible();
void rv_set_compatible(bool ok, const std::string &why);
// Why it did not ("" when it did, or no RV.EXE runs).
const std::string &rv_mismatch();

// Shared flights for the running RV.EXE: the interface matches, the room is
// not a room of one (WCSOLO), and WCRVNET is not 0.
bool rv_shared();

// RV.EXE's GameParams: its record sizes and slots (wcnet_rv.h), no fields
// among its mission ships (RV keeps them in fieldTable).
GameParams rv_params();

// --- the fields RV publishes for the layer (abi/rvabi.def "RV's own") ---
enum RvController { RV_CTL_AI = 0, RV_CTL_PLAYER = 1, RV_CTL_PUPPET = 2, RV_CTL_SCRIPT = 3, RV_CTL_FROZEN = 4 };
enum { RV_FLAG_HUMAN = 32 };    // rvFlags: a human flies this ship, on another machine
enum RvRole { RV_ROLE_ALONE = 0, RV_ROLE_HOST = 1, RV_ROLE_GUEST = 2 };

void rv_set_role(int role, int players);
int rv_controller(int slot);
bool rv_human(int slot);        // rvFlags' human bit
bool rv_marked(int slot);       // the layer made it a puppet or a human's (rv_mark)
// A ship slot flown on another machine (`human`: by a person), or given back
// to this one (CTL_AI).  Writes rvController, rvFlags' human bit and rvNetId
// (`net`, -1 none); logs the change once.
void rv_mark(int slot, int controller, bool human, int net);
// Every mark the layer made, undone (the session ends): puppets back to the
// AI, the human bits cleared, the role alone.
void rv_unmark_all();
// rvNetId alone (a guest's own ship: its network id).
void rv_set_net_id(int slot, int net);
// rvLastWrite: the frame the layer last wrote the slot's motion.
void rv_stamp_motion(int slot);

// --- the mission's compiled records: the host's, which a guest flies by ---
// missionShipTable and navPointTable as bytes, and which record the player
// flies.  The write refuses records of another size (false, logged).
struct RvMissionTables {
    std::string missionShips, navPoints;
    int playerMissionShip = 0;
};
RvMissionTables rv_read_mission_tables();
bool rv_write_mission_tables(const RvMissionTables &t);

}  // namespace wc

#endif
