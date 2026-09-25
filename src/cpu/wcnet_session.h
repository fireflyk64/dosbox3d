/*
 *  Multiplayer session: who we are, who we talk to, and the per-frame
 *  lockstep exchange.
 *
 *  Authority model
 *  ---------------
 *  * The server owns the mission: it spawns and despawns entities, runs the
 *    AI, and computes damage to NPC ships.  Clients replay those events.
 *  * Every human's own ship is authoritative on that human's machine.
 *    Wing Commander applies damage to slot 0 with a different (player) model
 *    than to NPC slots, so damage to a player's ship is only ever computed on
 *    the machine where that ship is slot 0.  The owner replicates the
 *    resulting health snapshot (ShipHealth) to everyone else, and no machine
 *    ever runs do_damage/despawn against a remote player's ship.
 *  * Positions of a player's ship come from its owner every frame.
 *
 *  Frame exchange (lockstep, unchanged from the original design)
 *  ---------------------------------------------------------------
 *  At the top of every in-flight frame each client sends its ShipUpdate plus
 *  any events, then blocks for the server's frame.  The server blocks for one
 *  message from every client in the mission, merges them, and sends one frame
 *  to each.  Events are replayed through the trampoline before the game
 *  simulates the frame.
 */
#ifndef WCNET_SESSION_H_
#define WCNET_SESSION_H_

#include <deque>
#include <string>
#include <vector>
#include "dosbox.h"
#include "net_config.h"
#include "wcnet_entities.h"
#include "wcnet_transport.h"
#include "wcnet_memory.h"
#include "../wc.pb.h"

namespace wc {

enum Role { ROLE_SERVER, ROLE_CLIENT };

class Session {
public:
    virtual ~Session() {}
    virtual Role role() const = 0;
    bool is_server() const { return role() == ROLE_SERVER; }
    bool is_client() const { return role() == ROLE_CLIENT; }

    // --- ownership --------------------------------------------------------
    // Slot flown by another human on this machine.
    virtual bool is_remote_player_slot(int slot) const = 0;
    bool is_player_slot(int slot) const { return slot == kPlayerSlot || is_remote_player_slot(slot); }

    // --- events -------------------------------------------------------------
    // Add an event to the frame that will be sent at the end of this frame's
    // exchange (server: to all clients; client: to the server).
    virtual void queue_outgoing_event(const Event &ev) = 0;
    // Spawn bookkeeping for late joiners (server only; no-op on clients).
    virtual void on_spawned(const Spawn &spawn) {}
    virtual void on_despawned(int net) {}

    // --- lifecycle hooks ----------------------------------------------------
    virtual void on_mission_starting(int mission, int series) = 0;
    virtual void on_mission_reset() = 0;         // entities cleared (mission over, ejected, ...)
    virtual void on_mission_victory_calc() = 0;  // score being computed
    virtual void on_mission_ended() = 0;         // back to the hangar flow
    virtual void on_frame_top() = 0;             // top of the in-flight loop
    virtual void on_async_tick() = 0;            // every ~1000 instructions
    virtual void on_barracks() = 0;              // entering the barracks
    virtual void on_mission_status_write(int status) = 0;  // dseg:00AE changed locally
    virtual void on_autopilot_begin(Bit16u camShipType, Bit16u camMode, Bit16u duration) {}
    virtual void on_autopilot_finished() {}
    virtual void on_trampoline_idle() {}

    virtual void send_chat(const std::string &text) = 0;
    virtual const std::string &callsign() const = 0;
};

// The active session, or NULL when not connected.
extern Session *g_session;

// Program-wide flags shared with the GUI (sdlmain.cpp).
extern bool within_briefed_mission;
extern int in_simulation;
extern bool has_started_up;

bool init_network();
void uninit_network();
bool is_wc_connected();
bool in_space();
void wcnetSendChatMessage(const std::string &msg);
void run_campaign(int missionId, int seriesId);

// Pilot identity (env WCCALLSIGN / WCLASTNAME override the save game).
std::string get_callsign();
std::string get_last_name();

}  // namespace wc

#endif
