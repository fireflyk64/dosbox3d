/*
 *  Which game is running, and where.
 *
 *  DOS_Execute reports every program it loads (wc_net_program_loaded); when
 *  it is one of the executables the hooks know, the data-segment map
 *  (wcnet_ds.def -> ds::...) and the code addresses (code::...) are loaded
 *  for it, relative to the segment DOS put it at.  While no such program
 *  runs, DS is 0, every code::Loc is unknown and the per-instruction hook
 *  does nothing game specific.
 */
#ifndef WCNET_GAME_H_
#define WCNET_GAME_H_

#include "dosbox.h"

namespace wc {

enum GameId {
    GAME_NONE = 0,
    GAME_WC1,   // WC.EXE: Wing Commander and The Secret Missions
    GAME_WC2,   // WC2.EXE: Wing Commander II
};

extern GameId g_game;
extern Bit16u g_loadSeg;  // first paragraph of the program image

inline bool is_wc1() { return g_game == GAME_WC1; }
inline bool is_wc2() { return g_game == GAME_WC2; }
const char *game_name();

// From DOS: a program was loaded at loadSeg for the process `psp`, and the
// process `psp` ended.
void game_program_loaded(const char *path, Bit16u loadSeg, Bit16u psp);
void game_program_ended(Bit16u psp);

// The hook dispatcher's part (wcnet_hooks.cpp): the tables changed.
void hooks_game_changed();

}  // namespace wc

#endif
