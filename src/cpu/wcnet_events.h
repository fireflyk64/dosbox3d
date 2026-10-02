/*
 *  Game events: interception of the game's own calls and replay of remote
 *  events through the trampoline.
 *
 *  Interception entry points are invoked by the hook dispatcher when CS:IP is
 *  at the entry of the corresponding game function, at any nesting depth
 *  (including from inside a replayed call).  Each decides between letting the
 *  call run, skipping it (return_from_call), or running it through the
 *  trampoline so that the result can be inspected and the event broadcast.
 */
#ifndef WCNET_EVENTS_H_
#define WCNET_EVENTS_H_

#include "dosbox.h"
#include "wcnet_vm.h"
#include "wc.pb.h"

namespace wc {

// --- interception ---------------------------------------------------------
void on_do_damage_entry();        // do_damage(src, dst, quantity, vec*)
void on_fire_entry();             // fireGunFromShip(ship, gun)
void on_spawn_entry();            // outerSpawnShipEntity(missionShip, situation)
void on_despawn_entry();          // despawn(ship)
void on_delayed_despawn_entry();  // delayedDespawn(src, ship)
void on_ai_think_entry();         // per-frame ship AI (ovr163:160E)
void on_ai_set_speed_entry();     // ovr143:0874

// --- replay ---------------------------------------------------------------
// Queue trampoline jobs for every event in a received frame.
void enqueue_remote_event(const Event &ev);
void enqueue_chat_display(int netShipId, const std::string &callsign, const std::string &text);
// Server: a client saw one of its own rocks or mines hit a ship of ours.
void enqueue_reported_damage(const Damage &reported);
// Server: a client left the mission; remove its copy here (with an explosion
// when it died, quietly when it landed).
void enqueue_wingman_lost(int slot, bool explode);
void enqueue_test_kill(int slot);

// Client-side prediction of the local player's own shots.
struct PendingFire {
    Bit32u seq;
    int gun;
    int spawnedSlot;  // permanent slot the local fire used (missiles), or -1
};
void reset_pending_fires();

// Bookkeeping helpers shared with the session.
void despawn_all_ships();  // C++ translation of ovr140:1C16 for slots 0..9, no animations

}  // namespace wc

#endif
