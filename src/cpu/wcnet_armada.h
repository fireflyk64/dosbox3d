/*
 *  Wing Commander Armada and its Proving Grounds (Origin, 1994): ARMADA.EXE and PG.EXE (and the CD-ROM builds,
 *  ARMADACD.EXE and PGCD.EXE), 16-bit real-mode programs (Borland C++, VROOMM overlays).  docs/armada.md.
 *
 *  Their network play is their own (NetBIOS or IPX, carried by the room's links: wcnet_ipx.cpp); nothing of a flight
 *  is hooked.  This module is the flight's controls only: the arrow keys ramped as Wing Commander's are.
 */
#ifndef WCNET_ARMADA_H_
#define WCNET_ARMADA_H_

#include "dosbox.h"

namespace wc {

// The dispatcher's (wcnet_game.cpp, wcnet_hooks.cpp).
void arm_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp);   // DOS loaded a program
void arm_program_ended(Bit16u psp);
bool arm_found();                                   // one of the four builds the hooks know is running
void arm_watch_list(void (*watch)(Bit16u ip));      // our IPs, for the dispatcher's table
bool arm_check_hooks(Bit32u ip);                    // a watched IP came up: true when it was one of ours

}  // namespace wc

#endif
