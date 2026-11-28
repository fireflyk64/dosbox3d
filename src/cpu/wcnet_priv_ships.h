/*
 *  Privateer's added ships (wcnet_priv_ships.cpp, docs/privateer.md section 42).
 */
#ifndef WCNET_PRIV_SHIPS_H
#define WCNET_PRIV_SHIPS_H

#include <string>
#include <vector>
#include <utility>
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
    struct Mark { int mount, x, y; } marks[5];
    // Its launchers' mounts as the appearance has them (SKELETON.IFF's, and its type's file) and the numbers they are
    // given here: ones the hull's upgrade bay can mount a launcher on (the Centurion's are 2 and 3; the game's
    // Stiletto has its one at 0, which no bay offers).  A `was` of -1 ends them; a launcher not listed stays where it
    // is and no bay reaches it.
    struct Renumber { int was, now; } launchers[3];
    // What the dealers do not sell it beyond what they do not sell its hull (LIMITS.IFF's list of item numbers, the
    // equipment list's: 0x2C the tail turret, 0x2F-0x31 shield generators 3-5, 0x3E-0x40 engine upgrades 3-5); 0 ends.
    Bit8u notSold[8];
    // Its sale (wcnet_priv_convs.cpp): the price, and the conversation of OPTIONS\GAMEFLOW.IFF's list after which it
    // is offered ("credits": Admiral Terrell's, once the campaign is won; 0: not sold anywhere yet)
    long price;
    const char *soldAfter;
    // ... or the base (BASES.IFF's number) and the sprite of one of its rooms (GAMEFLOW.IFF's SPRT INFO) whose click
    // ends with the offer (-1: none): the Demon's is the bartender of Tuck's, the pirate base of the Sherwood system
    int soldBase, soldSprite, soldSpriteLast;        // (a bar has three sprites for its bartender, 0xD7-0xD9: one is there at a time)
    // Its picture in the upgrade bay: which of the appearance's views (VSHP's, 0-36), turned left to right or not,
    // enlarged to fill the bay (the user: "want size to fill the screen mostly")
    int bayView, bayMirror, bayRight;       // (bayRight: pixels to the right of the room's middle)
    // Its picture in the takeoff and landing scenes: the view seen flying past (nose to the right; turned over for the
    // landing, where the ships fly to the left)
    int sceneView;
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
    kShipsHoldMoreEnd = 0x0BC7,
    kShipsSaleEntry = 0x0CC1,       // ovr286:0CC1(room, hull): the dealer's sale of a ship
    kShipsSaleSame = 0x0CE2,        // ovr286:0CE2: its "is that the hull you own?" (AL the hull byte, against the argument)
    kShipsSalePrice = 0x0CAC,       // ovr286:0CAC: the end of ovr286:0C4F(hull), a hull's price ([BP-4])
    kShipsSaleAccept = 0x1018,      // ovr286:1018: the offer accepted (the hull byte written next)
    kShipsSaleEnd = 0x1194,         // ovr286:1194: the sale's end
    kShipsBayPlace = 0x14BA,        // ovr287:14BA: the upgrade bay's "may what is in hand go on this place?"
    kShipsBayCount = 0x1F8D,        // ovr287:1F8D: the places counted (ds:09A4), before one is asked for
    kShipsBayDone = 0x31E7          // ovr287:31E7: back in the purchase (ovr287:3186) from the mounting
};
bool priv_ships_hook(Bit16u ip, Bit16u codeSeg, Bit16u loadSeg);

// A file of the game's TRE by its full name there.
typedef bool (*PrivTreFile)(const std::string &name, std::vector<Bit8u> *out);
// The added ship's cockpit made from the game's own (wcnet_priv_cockpit.cpp): its IFF and its PAK.
bool priv_ship_cockpit(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff, std::vector<Bit8u> *pak);
// Its appearance given what a player's ship needs (the weapons display's picture); false: good as it is.
bool priv_ship_appearance(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff);
// OPTIONS\OPTIONS.IFF (given) with the ship on every base's landing pad in the place of its hull's; false: left.
bool priv_ship_pads(const PrivAddedShip &ship, PrivTreFile file, const std::vector<Bit8u> &options, std::vector<Bit8u> *out);
// MIDGAMES\TAKEOFFS.PAK or LANDINGS.PAK (given) with the ship flying in the place of its scene ship; false: left.
bool priv_ship_scene(const PrivAddedShip &ship, PrivTreFile file, const std::vector<Bit8u> &pak, bool landing, std::vector<Bit8u> *out);
// OPTIONS\SHIPART.IFF (given) with the ship's picture in its hull's upgrade bay; false: left as it is.
bool priv_ship_bay(const PrivAddedShip &ship, PrivTreFile file, const std::vector<Bit8u> &shipart, std::vector<Bit8u> *out);
// The conversations of its sale (wcnet_priv_convs.cpp): each file's name in DATA\CONV and its bytes.
bool priv_ship_convs(const PrivAddedShip &ship, PrivTreFile file, std::vector<std::pair<std::string, std::vector<Bit8u> > > *files);
// Their names, in the place of the dealer's own for a Centurion: "buyfight", "cargo", "toopoor", "samefght" (and no
// longer than those: they are written over them while the sale runs).
void priv_ship_sale_names(const PrivAddedShip &ship, char names[4][9]);

}

#endif
