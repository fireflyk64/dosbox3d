/*
 *  Which game is running, and where.
 *
 *  DOS_Execute reports every program it loads (wc_net_program_loaded); when
 *  it is one of the executables the hooks know, the data-segment map
 *  (wcnet_ds.def -> ds::...) and the code addresses (code::...) are loaded
 *  for it, relative to the segment DOS put it at.  While no such program
 *  runs, DS is 0, every code::Loc is unknown and the per-instruction hook
 *  does nothing game specific.
 *
 *  There are two games and five executables: The Secret Missions 2 is
 *  Wing Commander's program built again (SM2.EXE), the two Special
 *  Operations are Wing Commander II's (SO1.EXE, SO2.EXE).  What the code
 *  asks is which game it is (is_wc1(), is_wc2()); the build only picks the
 *  addresses.
 */
#ifndef WCNET_GAME_H_
#define WCNET_GAME_H_

#include "dosbox.h"

namespace wc {

enum GameId {
    GAME_NONE = 0,
    GAME_WC1,   // Wing Commander: WC.EXE (with The Secret Missions) and SM2.EXE
    GAME_WC2,   // Wing Commander II: WC2.EXE, SO1.EXE and SO2.EXE
    GAME_RV,    // RetroVega (retrovega/): RV.EXE, the new game, hooked through its RVHOOK thunks
};

// The executable: its column of wcnet_ds.def, its table of code places.
enum BuildId {
    BUILD_NONE = 0,
    BUILD_WC1,  // WC.EXE
    BUILD_SM2,  // SM2.EXE: The Secret Missions 2
    BUILD_WC2,  // WC2.EXE
    BUILD_SO1,  // SO1.EXE: Special Operations 1
    BUILD_SO2,  // SO2.EXE: Special Operations 2
    BUILD_RV,   // RV.EXE: its DS from RVHOOK, its data map and places generated from its interface (wcnet_rv.h)
};

extern GameId g_game;
extern BuildId g_build;
extern Bit16u g_loadSeg;  // first paragraph of the program image

// Layout facts that differ between the games.
struct GameParams {
    int slots;               // entity slots (every per-entity array has this many elements)
    int missionShipSize;     // bytes per mission ship record (ds::missionShipTable)
    int missionShipClassOff; // where in it the type/class that marks a field is
    int missionShipClassSize;
    int asteroidField, mineField;  // those two values
    int navPointSize;        // bytes per nav point record (ds::navPointTable)
    int navPointShipsOff;    // ten mission-ship words in it (0: not located)
    int missionShipAiOff;    // WC2: the word in a mission ship record that picks its AI (6: the player; 0: not located)
    int gunsSize, gunSize;   // bytes per slot in ds::gunTable, and per gun in it
    int campaign;            // WC1: the campaign whose table the program loads when it starts (-1: none)
    int shipStatsSize;       // bytes per ship type in the game's table of ship figures (ds::shipStatsHull)
    int maxShipSlot;         // the last ship slot: ships, capships and missiles in 1..this (9; RV.EXE: its header's,
                             // at most kShipSlotCap), temporaries from the next (wcnet_memory.h)
};
extern GameParams g_params;

// What a game has, for the code that is the same for every game but for these
// (the session, the seats, the pacing).  Asked instead of which game it is, so
// a game's own ways are written down in one place (wcnet_game.cpp, traits_of),
// and Wing Commander's raw-address work stays behind is_wc1().
struct GameTraits {
    bool wingSeatsByRecords;    // the wing's seats are the mission's wingman records (wing_slots_wc1): WC1, RV
    bool seatsInRoomOfTwo;      // a room of two seats its guest as a room of three does (a real wingman, else a ship
                                // made for it), not by plan_seat_of_two's slot 1: RV
    bool seatFromPlayerRecord;  // with no wingman record to copy, a made seat copies the player's own: WC2, RV
    bool turrets;               // gunner and drone seats: WC2
    bool puppets;               // ships flown elsewhere are marked in the game (rvController, rvFlags): RV
    bool missionTables;         // the host's compiled mission records go with the briefing: RV
    bool carrierLostEnd;        // missionStatus 6, the carrier lost, is a shared ending: RV
    bool victoryCalcAlways;     // missionVictoryCalc follows every ending, a death too (no early reset): RV
    bool restartChoice;         // the death screen's R/C is the host's (restartChoice thunk): RV
    bool netNeedsConfig;        // networking only with a room or a peer configured (WCROOM, WCHOST, WCPORT): RV
    int defaultFps;             // frames a second the hooks pace a flight at (WCFPS overrides; 0: the game's own)
};
extern GameTraits g_traits;

inline bool is_wc1() { return g_game == GAME_WC1; }
inline bool is_wc2() { return g_game == GAME_WC2; }
inline bool is_rv() { return g_game == GAME_RV; }
const char *game_name();

// From DOS: a program was loaded at loadSeg for the process `psp`, and the
// process `psp` ended.
void game_program_loaded(const char *path, Bit16u loadSeg, Bit16u psp);
void game_program_ended(Bit16u psp);

// The hook dispatcher's part (wcnet_hooks.cpp): the tables changed.
void hooks_game_changed();

}  // namespace wc

#endif
