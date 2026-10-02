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
 *  * Shared fate: any player's ending (landed, died, ejected, quit) ends the
 *    mission for everyone with that same status.  A client reports its own
 *    ending with PlayerEnd; the server adopts it, and its MissionEnd frame
 *    carries it to the other clients.
 *
 *  Frame exchange (lockstep, unchanged from the original design)
 *  ---------------------------------------------------------------
 *  At the top of every in-flight frame each client sends its ShipUpdate plus
 *  any events, then blocks for the server's frame.  The server blocks for one
 *  message from every client in the mission, merges them, and sends one frame
 *  to each.  Events are replayed through the trampoline before the game
 *  simulates the frame.  The server sends exactly one frame per client
 *  message, so that the two sides never drift apart in message count.
 */
#ifndef WCNET_SESSION_H_
#define WCNET_SESSION_H_

#include <string>
#include "dosbox.h"
#include "wcnet_entities.h"
#include "wc.pb.h"

namespace wc {

enum Role { ROLE_SERVER, ROLE_CLIENT };

// What a client is in a mission (Game.seat).
enum Seat {
    SEAT_WINGMAN = 0,  // flies the ship the mission has in the wingman's slot
    SEAT_DRONE = 1,    // a mission the story flies alone: a ship of its own that exists on its
                       // machine only; nothing sees or hits it and it cannot shoot
};

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
    // This machine's own seat (a server is the leader, never a drone).
    virtual Seat seat() const { return SEAT_WINGMAN; }
    bool is_drone() const { return seat() == SEAT_DRONE; }
    // A drone can ride behind the leader instead of flying itself.
    virtual void toggle_chase() {}
    // Network ids flown by humans: the host and the two client slots.
    static bool is_player_net(int net) { return net == 0 || net == 1 || net == 3; }

    // --- events -------------------------------------------------------------
    // Add an event to the frame that will be sent at the end of this frame's
    // exchange (server: to all clients; client: to the server).
    virtual void queue_outgoing_event(const Event &ev) = 0;
    // Spawn bookkeeping for late joiners (server only).
    virtual void on_spawned(const Spawn &spawn) { (void)spawn; }
    virtual void on_despawned(int net) { (void)net; }

    // --- lifecycle hooks (see wcnet_hooks.cpp for where each fires) ---------
    virtual void on_mission_starting(int mission, int series) = 0;
    virtual void on_mission_reset() = 0;         // entities cleared (mission over)
    virtual void on_mission_victory_calc() = 0;  // score being computed
    virtual void on_mission_ended() = 0;         // back to the hangar flow
    virtual void on_frame_top() = 0;             // top of the in-flight loop
    virtual void on_async_tick() = 0;            // every ~1000 instructions
    virtual void on_barracks() = 0;              // entering the barracks
    virtual void check_mission_status() = 0;     // before the game tests dseg:00AE
    virtual void on_autopilot_begin(Bit16u camShipType, Bit16u camMode, Bit16u duration) {
        (void)camShipType; (void)camMode; (void)duration;
    }
    virtual void on_autopilot_finished() {}
    virtual void on_trampoline_idle() {}
    virtual bool in_space() const = 0;

    virtual void send_chat(const std::string &text) = 0;

    // Switch the asteroid and mine fields on, off or soft (RocksMode) for
    // everybody.  Only the server decides; a client's request just says so.
    // In flight it takes effect at the top of the next frame.
    virtual void request_rocks(int mode) = 0;
};

// The active session, or NULL when not connected.
extern Session *g_session;

extern int in_simulation;
extern bool has_started_up;

// Jump into the campaign at a mission (MIS/SERIES environment variables).
void run_campaign(int missionId, int seriesId);
// Pilot identity (env WCCALLSIGN / WCLASTNAME override the save game).
std::string get_callsign();
std::string get_last_name();

}  // namespace wc

#endif
