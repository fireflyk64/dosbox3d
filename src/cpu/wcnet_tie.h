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

// The per-instruction dispatcher's call when EIP's low word is watched.
void tie_check_hooks(Bit32u eip);

// Test aid: the front end's pointer put at a point of its 320x200 screen
// (the key script's !tiecursor=x:y; a mouse button then clicks there).
void tie_set_cursor(int x, int y);

}  // namespace wc

#endif
