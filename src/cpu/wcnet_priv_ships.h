/*
 *  Privateer's added ships (wcnet_priv_ships.cpp, docs/privateer.md section 42).
 */
#ifndef WCNET_PRIV_SHIPS_H
#define WCNET_PRIV_SHIPS_H

#include <string>
#include <vector>
#include "dosbox.h"

namespace wc {

// A ship the player can fly that is none of the game's four.
struct PrivAddedShip {
    int id;                     // told in the saved game and to the room (never renumbered)
    const char *name;           // "Stiletto"
    const char *appearance;     // DATA\APPEARNC\<this>.IFF: the ship in space
    const char *cockpit;        // DATA\COCKPITS\<this>.IFF and .PAK: made by the layer
    int hull;                   // the game's hull it is flown on (ds:655E: 0 Tarsus, 1 Orion, 2 Centurion, 3 Galaxy)
    int sceneShip;              // its ship in the landing and takeoff scenes (SHIP2CHR.IFF's number)
};

// The ship the player owns, if an added one (0: one of the game's four).
const PrivAddedShip *priv_added_ship();
// At the IFF reader's table seek (seg214:03DF).
void priv_ships_table_seek();

// A file of the game's TRE by its full name there.
typedef bool (*PrivTreFile)(const std::string &name, std::vector<Bit8u> *out);
// The added ship's cockpit made from the game's own (wcnet_priv_cockpit.cpp): its IFF and its PAK.
bool priv_ship_cockpit(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff, std::vector<Bit8u> *pak);
// Its appearance given what a player's ship needs (the weapons display's picture); false: good as it is.
bool priv_ship_appearance(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff);

}

#endif
