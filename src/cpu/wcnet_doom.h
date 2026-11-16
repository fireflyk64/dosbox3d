/*
 *  DOOM (id Software, 1993): DOOM.EXE's own network game, the emulator its network driver.  docs/doom.md.
 *
 *  Nothing of the game is hooked.  DOOM plays a network game through a driver: a program resident under it that
 *  shares a block of memory with it (doomcom_t: a command, a node number, one packet) and answers an interrupt -- "send
 *  this packet to that node", "is there a packet".  id shipped two (IPXSETUP.EXE, SERSETUP.EXE) and published the
 *  interface for others.  Ours is DOOMNET.COM, a file the emulator puts on its Z: drive (wcnet_doomnet.asm: a hundred
 *  bytes of DOS program that keep the block and run the game) with everything else done here: the players are the
 *  room's (wcnet_doom_session.cpp), the packets go over the room's links, and the game is DOOM's own co-operative
 *  game, its own lockstep, its own consistency check.
 */
#ifndef WCNET_DOOM_H_
#define WCNET_DOOM_H_

#include <string>
#include "dosbox.h"

namespace wc {

// The dispatcher's (wcnet_hooks.cpp) and DOS's (dos_programs.cpp, through wc_net_dos_programs).
void doom_setup();                  // DOOMNET.COM put on the Z: drive
void doom_tick();                   // every ~1000 instructions
bool doom_launched();               // the launcher was run (DOOM's machine: none of the other games' aids apply)
bool doom_running();                // DOOM run by the launcher, in a network game or alone
bool doom_script_item(const std::string &item, double t);   // a rig's key-script aid: !doomtext (the text screen logged)

enum { kDoomMaxPlayers = 4, kDoomPacketMax = 104 };    // (MAXPLAYERS; sizeof(doomdata_t): 8 + BACKUPTICS * 8)

// The session's (wcnet_doom_session.cpp), for the launcher and the driver.
struct DoomGame {
    Bit32u serial;                  // the host's count of games (0: none)
    int players;                    // DOOM's players, 2..4
    int self;                       // ours (0 the host's: DOOM's "key player")
    std::string args;               // the command line, the host's
    std::string names[kDoomMaxPlayers];
    int ticdup, extratics;
    DoomGame() : serial(0), players(0), self(0), ticdup(1), extratics(1) {}
};
bool doom_session_configured();     // a room (or a rig's TCP port) to play in
void doom_session_tick();
void doom_session_shutdown();
// our launcher waits for the game (ready), with the program and WADs it would run and its own command line
void doom_session_waiting(bool ready, const std::string &game, const std::string &args);
// the host's: start the game once everybody in the room is ready (now: with whoever is, Enter); false: not yet.
// *alone: nobody to play with (the game run by itself)
bool doom_session_start(bool now, bool *alone);
bool doom_session_game(DoomGame *out);      // the game started (ours to run), until doom_session_game_over
void doom_session_game_over();              // our DOOM ended
std::string doom_session_line();            // where things stand, for the launcher's screen
bool doom_session_is_host();
void doom_session_send(int player, const Bit8u *data, int len);
int doom_session_recv(int *player, Bit8u *data, int max);      // a packet's length, 0: none
bool doom_session_gone(int player);         // that player is out of the game under way (its exit told to ours)

}  // namespace wc

#endif
