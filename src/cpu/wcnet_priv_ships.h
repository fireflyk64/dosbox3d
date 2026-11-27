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
    const char *type;           // DATA\TYPES\<this>.IFF: the game's own ship of that appearance (another pilot's is made as it)
    const char *cockpit;        // DATA\COCKPITS\<this>.IFF and .PAK: made by the layer
    int hull;                   // the game's hull it is flown on (ds:655E: 0 Tarsus, 1 Orion, 2 Centurion, 3 Galaxy)
    int sceneShip;              // its ship in the landing and takeoff scenes (SHIP2CHR.IFF's number)
    int hold, holdMore;         // the hold's size, and what the cargo expansion adds
    // Its guns' marks on the weapons display: a mount, and the mark's place from the picture's middle (the
    // appearance's own, SKELETON.IFF's, are a display no player saw: the user, "The two bolt mounts need to be
    // pulled together too so they touch the stiletto wings").  A mount of -1 ends them.
    struct Mark { int mount, x, y; } marks[4];
    // What the dealers do not sell it beyond what they do not sell its hull (LIMITS.IFF's list of item numbers, the
    // equipment list's: 0x2C the tail turret, 0x2F-0x31 shield generators 3-5, 0x3E-0x40 engine upgrades 3-5); 0 ends.
    Bit8u notSold[8];
};

// The ship the player owns, if an added one (0: one of the game's four).
const PrivAddedShip *priv_added_ship();
// An added ship by its id (0: none such here).
const PrivAddedShip *priv_added_ship_by_id(int id);
// The appearance the game itself gives a hull ("FIGHTER" the Centurion's).
const char *priv_hull_appearance(int hull);
// At the IFF reader's table seek (seg214:03DF).
void priv_ships_table_seek();
// The other places (their offsets are watched in every segment; the code there tells which is meant).
enum {
    kShipsRecordLoad = 0x01CE,      // stub262:01CE -> ovr262:0040: a saved game's (or a new game's) first record taken
    kShipsRecordStored = 0x003D,    // ovr262:003D: the end of ovr262:0000, which makes that record (SI) from the game's bytes
    kShipsHoldSet = 0x0AA5,         // ovr269:0AA5(hold, hull): the hold's size by the hull
    kShipsHoldPlain = 0x0B40,       // ovr269:0B40(hold): the same, no expansion
    kShipsHoldMore = 0x0B89,        // ovr269:0B89(hold): the same, expanded
    kShipsHoldRead = 0x0BD2,        // ovr269:0BD2(hold, size, hull): the expansion told from a ship file's size
    kShipsHoldSize = 0x178F,        // ovr269:178F: the end of ovr269:173B(base ship, hull), the size for the ship's file in DX
    kShipsHoldPlainEnd = 0x0B7E,    // their ends when the hull is none of the four
    kShipsHoldMoreEnd = 0x0BC7
};
bool priv_ships_hook(Bit16u ip, Bit16u codeSeg, Bit16u loadSeg);

// A file of the game's TRE by its full name there.
typedef bool (*PrivTreFile)(const std::string &name, std::vector<Bit8u> *out);
// The added ship's cockpit made from the game's own (wcnet_priv_cockpit.cpp): its IFF and its PAK.
bool priv_ship_cockpit(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff, std::vector<Bit8u> *pak);
// Its appearance given what a player's ship needs (the weapons display's picture); false: good as it is.
bool priv_ship_appearance(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff);

}

#endif
