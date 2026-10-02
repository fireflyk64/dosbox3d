/*
 *  Public interface of the Wing Commander multiplayer layer.
 *
 *  The implementation lives in src/cpu/wcnet_*.cpp:
 *    wcnet_game       which executable is running, and its address tables
 *    wcnet_memory     data-segment map and typed accessors
 *    wcnet_code       code addresses (stubs, overlays, hook points)
 *    wcnet_vm         running game code (interception, trampoline)
 *    wcnet_transport  TCP framing and per-category message queues
 *    wcnet_entities   network id <-> local slot mapping
 *    wcnet_events     interception and replay of game events
 *    wcnet_session    server/client sessions and the per-frame exchange
 *    wcnet_hooks      the per-instruction dispatcher
 */
#ifndef WC_NET_H_
#define WC_NET_H_

#include <string>
#include "dosbox.h"
#include "net_config.h"

// Called before every instruction by the normal CPU core.
void wc_net_check_cpu_hooks();

// Called by DOS when it has loaded a program (its path, the first paragraph
// of the image, its PSP) and when a process ends: the hooks only apply to
// the executables they know, wherever DOS put them.
void wc_net_program_loaded(const char *path, Bit16u loadSeg, Bit16u psp);
void wc_net_program_ended(Bit16u psp);

// Start a server (WCHOST unset) or connect to one (WCHOST set); with WCROOM
// set, meet the others in a lobbylink room instead.  See NetConfig.
bool init_network();
void uninit_network();

// GUI queries (src/gui/sdlmain.cpp) and the WCNET command.
bool is_wc_connected();
// "server", "client" or "" when not connected.
const char *wc_net_role();
// Something worth telling the player about the connection, or "".
const char *wc_net_status_note();
bool in_space();
// Chat lines and notices go on the emulator's overlay: outside flight, and in
// flight when the game has no comms display the hooks can write to (WC2).
bool wc_net_overlay_chat();
extern bool within_briefed_mission;
void wcnetSendChatMessage(const std::string &msg);
// Asteroid and mine fields: 1 on (the default), 0 off (WCROCKS=0), 2 soft
// (WCROCKS=soft: a rock that hits a player's own ship does a small fraction
// of the game's damage, so one collision is survivable).  The host's choice
// goes to every player; "/rocks on", "/rocks soft" and "/rocks off" in the
// comms prompt do the same.
void wc_net_set_rocks(int mode);
int wc_net_rocks();

#endif
