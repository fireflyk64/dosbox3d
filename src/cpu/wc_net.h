/*
 *  Public interface of the Wing Commander multiplayer layer.
 *
 *  The implementation lives in src/cpu/wcnet_*.cpp:
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
#include "net_config.h"

// Called before every instruction by the normal CPU core.
void wc_net_check_cpu_hooks();

// Start a server (WCHOST unset) or connect to one (WCHOST set); see NetConfig.
bool init_network();
void uninit_network();

// GUI queries (src/gui/sdlmain.cpp).
bool is_wc_connected();
bool in_space();
extern bool within_briefed_mission;
void wcnetSendChatMessage(const std::string &msg);

#endif
