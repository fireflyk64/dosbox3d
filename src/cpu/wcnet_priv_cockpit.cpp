/*
 *  Privateer's added ships' cockpits (wcnet_priv_ships.h, docs/privateer.md section 42).
 */
#include <string.h>
#include "wcnet_priv_ships.h"

namespace wc {

bool priv_ship_cockpit(const PrivAddedShip &ship, PrivTreFile file, std::vector<Bit8u> *iff, std::vector<Bit8u> *pak) {
    (void)ship;
    return file("..\\..\\DATA\\COCKPITS\\FIGHTCK.IFF", iff) && file("..\\..\\DATA\\COCKPITS\\FIGHTCK.PAK", pak);
}

}
