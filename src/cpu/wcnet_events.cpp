#include <stdio.h>
#include <deque>
#include <vector>
#include "wcnet_events.h"
#include "wcnet_session.h"
#include "wcnet_memory.h"
#include "wcnet_code.h"
#include "wcnet_log.h"
#include "cpu.h"
#include "regs.h"

namespace wc {

// Scratch vector inside the data segment for replayed damage positions.
enum { kTmpVectorOff = ds::aLoadingWingCom };

static std::deque<PendingFire> g_pendingFires;
static Bit32u g_nextFireSeq = 1;

void reset_pending_fires() {
    g_pendingFires.clear();
}

// ---------------------------------------------------------------------------
// Helpers

static void seed_rng(Bit32u seed) {
    wr32(ds::randomSeed, seed);
}

// Snapshot of which permanent slots are in use, to detect what a game call
// spawned (fireGunFromShip returns the bolt slot in AX for bolts but missiles
// take a permanent slot that we must map).
struct SlotSnapshot {
    Bit16u types[kMaxShipSlot + 1];
    void take() {
        for (int i = 0; i <= kMaxShipSlot; i++) {
            types[i] = entity_type(i);
        }
    }
    // First permanent slot that became used since take(); -1 if none.
    int newly_used() const {
        for (int i = kMinShipSlot; i <= kMaxShipSlot; i++) {
            if (entity_type(i) && !types[i]) {
                return i;
            }
        }
        return -1;
    }
};

static GameCall call_of(const code::OverlayFn &fn) {
    return GameCall(fn.stubSeg, fn.stubOff);
}

// ---------------------------------------------------------------------------
// Jobs

// Runs fireGunFromShip(ship, gun); afterwards records which permanent slot
// (missile) it used and either broadcasts the event or resolves a predicted
// local fire.
class FireJob : public VmJob {
public:
    enum Mode { BROADCAST, PREDICT, REPLAY };
    FireJob(Mode mode, const WeaponFire &fire, int localShip, int gun)
        : mode_(mode), fire_(fire), localShip_(localShip), gun_(gun) {}

    virtual bool start() {
        if (localShip_ == kInvalidSlot || !slot_in_use(localShip_)) {
            wclog(2, "fire: shooter slot %d not usable", localShip_);
            return false;
        }
        if (free_temp_slots() < 5) {
            wclog(2, "fire: too few free slots, dropping shot");
            return false;
        }
        before_.take();
        started_ = true;
        wclog(2, "%s fire: ship %d (net %d) gun %d",
              mode_ == PREDICT ? "predict" : (mode_ == REPLAY ? "replay" : "local"),
              localShip_, fire_.shooter(), gun_);
        call_of(code::fireGunFromShip).arg((Bit16u)localShip_).arg((Bit16u)gun_).invoke();
        return true;
    }

    virtual void finish() {
        if (!started_) {
            return;
        }
        int spawned = before_.newly_used();
        if (spawned != -1 && entity_type(spawned) != ET_MISSILE) {
            wclog(2, "fire spawned permanent slot %d of type %d", spawned, entity_type(spawned));
        }
        switch (mode_) {
        case BROADCAST: {
            Event ev;
            *ev.mutable_fire() = fire_;
            if (spawned != -1) {
                ev.mutable_fire()->set_ship_id(NetworkShipId::from_local(spawned).to_net());
            }
            g_session->queue_outgoing_event(ev);
            break;
        }
        case PREDICT: {
            PendingFire pf;
            pf.seq = fire_.client_seq();
            pf.gun = gun_;
            pf.spawnedSlot = spawned;
            g_pendingFires.push_back(pf);
            Event ev;
            *ev.mutable_fire() = fire_;
            g_session->queue_outgoing_event(ev);
            break;
        }
        case REPLAY:
            if (fire_.has_ship_id() && spawned != -1 && g_entityMap) {
                g_entityMap->record_spawn(fire_.ship_id(), spawned);
            }
            break;
        }
    }
    virtual const char *describe() const { return "fire"; }

private:
    Mode mode_;
    WeaponFire fire_;
    int localShip_, gun_;
    SlotSnapshot before_;
    bool started_ = false;
};

class DamageJob : public VmJob {
public:
    explicit DamageJob(const Damage &dam) : dam_(dam) {}
    virtual bool start() {
        if (!dam_.has_ship_id()) {
            return false;
        }
        NetworkShipId dst = NetworkShipId::from_net(dam_.ship_id());
        int dstLocal = dst.to_local();
        if (g_entityMap && !g_entityMap->is_mapped(dam_.ship_id())) {
            wclog(2, "damage: target net %d unmapped, ignored", dam_.ship_id());
            return false;
        }
        if (dstLocal == kInvalidSlot || !slot_in_use(dstLocal)) {
            return false;
        }
        if (g_session->is_player_slot(dstLocal)) {
            // Damage to a human's ship is computed on that human's machine.
            return false;
        }
        int srcLocal = kInvalidSlot;
        if (dam_.has_shooter()) {
            int shooterLocal = NetworkShipId::from_net(dam_.shooter()).to_local();
            if (shooterLocal != kInvalidSlot) {
                // The game plays hit sounds/effects only when the source is a
                // bolt, so hand it one of the shooter's bolts when we have one.
                int bolt = find_bolt_of(shooterLocal);
                srcLocal = bolt != -1 ? bolt : shooterLocal;
            }
        }
        Vec3 pos = { 0, 0, 256 };
        if (dam_.has_pos()) {
            pos.x = dam_.pos().x();
            pos.y = dam_.pos().y();
            pos.z = dam_.pos().z();
        }
        write_vec(kTmpVectorOff, pos);
        if (dam_.has_seed()) {
            seed_rng(dam_.seed());
        }
        wclog(2, "replay damage %d -> %d (local %d -> %d) qty %d", dam_.shooter(), dam_.ship_id(), srcLocal, dstLocal, dam_.quantity());
        call_of(code::do_damage)
            .arg((Bit16u)srcLocal).arg((Bit16u)dstLocal)
            .arg((Bit16u)dam_.quantity()).arg((Bit16u)kTmpVectorOff)
            .invoke();
        return true;
    }
    virtual const char *describe() const { return "damage"; }

private:
    Damage dam_;
};

class SpawnJob : public VmJob {
public:
    // mode BROADCAST: the server's own spawn (args from the intercepted call).
    // mode REPLAY: a spawn received from the server.
    enum Mode { BROADCAST, REPLAY };
    SpawnJob(Mode mode, const Spawn &spawn) : mode_(mode), spawn_(spawn) {}
    virtual bool start() {
        if (!spawn_.has_mission_ship_id() || !spawn_.has_situation_id()) {
            wclog(0, "invalid spawn event");
            return false;
        }
        if (mode_ == REPLAY && spawn_.has_seed()) {
            seed_rng(spawn_.seed());
        }
        wclog(2, "spawn mission ship %d situation %d", spawn_.mission_ship_id(), spawn_.situation_id());
        call_of(code::outerSpawnShipEntity)
            .arg((Bit16u)spawn_.mission_ship_id()).arg((Bit16u)spawn_.situation_id())
            .invoke();
        return true;
    }
    virtual void finish() {
        int slot = (int)(reg_eax & 0xffff);
        wclog(2, "spawn produced slot %d", slot);
        if (mode_ == BROADCAST) {
            if (slot != kInvalidSlot) {
                spawn_.set_ship_id(NetworkShipId::from_local(slot).to_net());
            }
            Event ev;
            *ev.mutable_spawn() = spawn_;
            g_session->queue_outgoing_event(ev);
            g_session->on_spawned(spawn_);
        } else if (spawn_.has_ship_id() && g_entityMap && slot != kInvalidSlot) {
            g_entityMap->record_spawn(spawn_.ship_id(), slot);
            if ((int)spawn_.ship_id() == g_entityMap->own_ship()) {
                wclog(2, "net %d is us (slot 0); slot %d becomes the server player's ship", spawn_.ship_id(), slot);
            } else {
                wclog(2, "net %d -> slot %d", spawn_.ship_id(), slot);
            }
        }
    }
    virtual const char *describe() const { return "spawn"; }

private:
    Mode mode_;
    Spawn spawn_;
};

class DespawnJob : public VmJob {
public:
    enum Mode { BROADCAST, REPLAY, WINGMAN_LOST };
    DespawnJob(Mode mode, const Despawn &d, int localShip, int localSrc)
        : mode_(mode), d_(d), localShip_(localShip), localSrc_(localSrc) {}
    virtual bool start() {
        if (localShip_ == kInvalidSlot || !slot_in_use(localShip_)) {
            wclog(2, "despawn: slot %d already free", localShip_);
            return false;
        }
        if (mode_ == REPLAY && g_session->is_player_slot(localShip_)) {
            return false;
        }
        bool explode = d_.has_explode() && d_.explode();
        wclog(2, "despawn slot %d explode=%d", localShip_, (int)explode);
        if (explode) {
            call_of(code::delayedDespawn).arg((Bit16u)localSrc_).arg((Bit16u)localShip_).invoke();
        } else {
            call_of(code::despawn).arg((Bit16u)localShip_).invoke();
        }
        started_ = true;
        return true;
    }
    virtual void finish() {
        if (!started_) {
            return;  // nothing was there; nobody needs to hear about it
        }
        bool explode = d_.has_explode() && d_.explode();
        if (mode_ == BROADCAST || mode_ == WINGMAN_LOST) {
            Event ev;
            *ev.mutable_despawn() = d_;
            g_session->queue_outgoing_event(ev);
            if (!explode) {
                g_session->on_despawned(d_.ship_id());
            }
        } else if (!explode && g_entityMap) {
            g_entityMap->record_despawn(d_.ship_id());
        }
    }
    virtual const char *describe() const { return "despawn"; }

private:
    Mode mode_;
    Despawn d_;
    int localShip_, localSrc_;
    bool started_ = false;
};

class AutopilotJob : public VmJob {
public:
    explicit AutopilotJob(const AutoPilotEvent &ape) : ape_(ape) {}
    virtual bool start() {
        if (ape_.has_finish_camera() && ape_.finish_camera()) {
            return false;
        }
        wclog(2, "replay autopilot camera (%d, %d, %d)", ape_.cam_ship_type(), ape_.cam_mode(), ape_.duration());
        call_of(code::autoAnimation)
            .arg((Bit16u)ape_.cam_ship_type()).arg((Bit16u)ape_.cam_mode()).arg((Bit16u)ape_.duration())
            .invoke();
        return true;
    }
    virtual const char *describe() const { return "autopilot"; }

private:
    AutoPilotEvent ape_;
};

class ChatJob : public VmJob {
public:
    ChatJob(int netShip, const std::string &text) : netShip_(netShip), text_(text) {}
    virtual bool start() {
        int local = NetworkShipId::from_net(netShip_).to_local();
        if (local == kInvalidSlot) {
            local = kPlayerSlot;
        }
        write_cstring(ds::commGlobalTxt, 80, text_);
        call_of(code::showCommMessage).arg(0).arg((Bit16u)local).invoke();
        return true;
    }
    virtual const char *describe() const { return "chat"; }

private:
    int netShip_;
    std::string text_;
};

// ---------------------------------------------------------------------------
// Interception

static bool replaying() {
    return g_trampoline.is_running();
}

void on_do_damage_entry() {
    Session *s = g_session;
    if (!s) {
        return;
    }
    int src = call_arg16(0);
    int dst = call_arg16(1);
    Bit16u quantity = call_arg16(2);
    Bit16u vecOff = call_arg16(3);

    if (s->is_remote_player_slot(dst)) {
        // Only the owner's machine may damage a human's ship (see session.h).
        return_from_call(0);
        return;
    }
    if (replaying()) {
        return;  // a replayed event: run natively
    }
    if (s->is_client()) {
        if (dst == kPlayerSlot) {
            return;  // our own ship: the player model runs here and only here
        }
        return_from_call(0);  // NPC damage is decided by the server
        return;
    }
    if (dst == kPlayerSlot) {
        return;  // server's own ship: native, nobody else needs it
    }
    if (!is_ship_slot(dst)) {
        return;  // missiles/bolts hit: not replicated
    }
    Event ev;
    Damage *dam = ev.mutable_damage();
    dam->set_ship_id(NetworkShipId::from_local(dst).to_net());
    NetworkShipId shooter = NetworkShipId::from_top_level_local(src);
    if (!shooter.is_invalid()) {
        dam->set_shooter(shooter.to_net());
    }
    dam->set_quantity(quantity);
    Vec3 v = read_vec(vecOff);
    dam->mutable_pos()->set_x(v.x);
    dam->mutable_pos()->set_y(v.y);
    dam->mutable_pos()->set_z(v.z);
    dam->set_seed(rd32(ds::randomSeed));
    s->queue_outgoing_event(ev);
    // ...and let the game apply it here with its original arguments.
}

void on_fire_entry() {
    Session *s = g_session;
    if (!s || replaying()) {
        return;
    }
    int ship = call_arg16(0);
    int gun = call_arg16(1);
    if (s->is_client()) {
        if (ship != kPlayerSlot) {
            return_from_call(0xffff);  // other ships fire through server events
            return;
        }
        WeaponFire fire;
        fire.set_shooter(NetworkShipId::from_local(ship).to_net());
        fire.set_gun_id(gun);
        fire.set_client_seq(g_nextFireSeq++);
        g_trampoline.enqueue(new FireJob(FireJob::PREDICT, fire, ship, gun));
        g_trampoline.run_instead_of_current_call();
        return;
    }
    if (s->is_remote_player_slot(ship)) {
        return_from_call(0xffff);  // the AI must not fire a human's ship
        return;
    }
    WeaponFire fire;
    fire.set_shooter(NetworkShipId::from_local(ship).to_net());
    fire.set_gun_id(gun);
    g_trampoline.enqueue(new FireJob(FireJob::BROADCAST, fire, ship, gun));
    g_trampoline.run_instead_of_current_call();
}

void on_spawn_entry() {
    Session *s = g_session;
    if (!s || replaying()) {
        return;
    }
    if (s->is_client()) {
        return_from_call(0xffff);  // the server decides what exists
        return;
    }
    Spawn spawn;
    spawn.set_mission_ship_id(call_arg16(0));
    spawn.set_situation_id(call_arg16(1));
    spawn.set_seed(rd32(ds::randomSeed));
    g_trampoline.enqueue(new SpawnJob(SpawnJob::BROADCAST, spawn));
    g_trampoline.run_instead_of_current_call();
}

static void intercept_despawn(bool explode) {
    Session *s = g_session;
    if (!s) {
        return;
    }
    int ship = explode ? call_arg16(1) : call_arg16(0);
    int src = explode ? call_arg16(0) : kInvalidSlot;
    if (!is_ship_slot(ship)) {
        return;  // temporary entities are local on every machine
    }
    if (s->is_remote_player_slot(ship)) {
        return_from_call(0);  // a human's ship only leaves when its owner says so
        return;
    }
    if (replaying()) {
        return;
    }
    if (ship == kPlayerSlot) {
        return;  // our own death/landing: the game handles it, mission status tells the others
    }
    if (s->is_client()) {
        return_from_call(0);
        return;
    }
    Despawn d;
    d.set_ship_id(NetworkShipId::from_local(ship).to_net());
    if (explode) {
        d.set_explode(1);
        NetworkShipId shooter = NetworkShipId::from_top_level_local(src);
        if (!shooter.is_invalid()) {
            d.set_shooter(shooter.to_net());
        }
    }
    g_trampoline.enqueue(new DespawnJob(DespawnJob::BROADCAST, d, ship, src));
    g_trampoline.run_instead_of_current_call();
}

void on_despawn_entry() { intercept_despawn(false); }
void on_delayed_despawn_entry() { intercept_despawn(true); }

void on_ai_think_entry() {
    Session *s = g_session;
    if (!s || !s->is_server()) {
        return;
    }
    if (s->is_remote_player_slot(call_arg16(0))) {
        return_from_call(0);
    }
}

void on_ai_set_speed_entry() {
    Session *s = g_session;
    if (!s) {
        return;
    }
    int ship = call_arg16(0);
    if (s->is_client() || s->is_remote_player_slot(ship)) {
        // Jump to the function's early-return label (ovr143:0918) so the AI
        // cannot change the set speed of a human-flown ship.
        reg_eip = code::aiSetSpeedReturn.ovrOff;
    }
}

// ---------------------------------------------------------------------------
// Replay

static void enqueue_fire(const WeaponFire &fire) {
    if (!fire.has_shooter()) {
        return;
    }
    if (g_entityMap) {
        if (!g_entityMap->is_mapped(fire.shooter())) {
            wclog(2, "fire from unmapped net %d ignored", fire.shooter());
            return;
        }
        if ((int)fire.shooter() == g_entityMap->own_ship()) {
            // Echo of a shot we already fired locally.
            for (std::deque<PendingFire>::iterator it = g_pendingFires.begin(); it != g_pendingFires.end(); ++it) {
                if (fire.has_client_seq() && it->seq == fire.client_seq()) {
                    if (fire.has_ship_id() && it->spawnedSlot != -1) {
                        g_entityMap->record_spawn(fire.ship_id(), it->spawnedSlot);
                    }
                    g_pendingFires.erase(g_pendingFires.begin(), it + 1);
                    return;
                }
            }
            wclog(2, "echo of our fire seq %u not pending; ignoring", fire.client_seq());
            return;
        }
    }
    int local = NetworkShipId::from_net(fire.shooter()).to_local();
    FireJob::Mode mode = g_session->is_server() ? FireJob::BROADCAST : FireJob::REPLAY;
    g_trampoline.enqueue(new FireJob(mode, fire, local, fire.gun_id()));
}

void enqueue_remote_event(const Event &ev) {
    if (ev.has_fire()) {
        enqueue_fire(ev.fire());
    }
    if (ev.has_damage()) {
        g_trampoline.enqueue(new DamageJob(ev.damage()));
    }
    if (ev.has_spawn()) {
        g_trampoline.enqueue(new SpawnJob(SpawnJob::REPLAY, ev.spawn()));
    }
    if (ev.has_despawn() && ev.despawn().has_ship_id()) {
        const Despawn &d = ev.despawn();
        if (g_entityMap && !g_entityMap->is_mapped(d.ship_id())) {
            wclog(2, "despawn of unmapped net %d ignored", d.ship_id());
        } else {
            int local = NetworkShipId::from_net(d.ship_id()).to_local();
            int src = kInvalidSlot;
            if (d.has_shooter()) {
                src = NetworkShipId::from_net(d.shooter()).to_local();
            }
            g_trampoline.enqueue(new DespawnJob(DespawnJob::REPLAY, d, local, src));
        }
    }
    if (ev.has_autopiloting()) {
        g_trampoline.enqueue(new AutopilotJob(ev.autopiloting()));
    }
}

void enqueue_chat_display(int netShipId, const std::string &callsign, const std::string &text) {
    std::string formatted = text;
    if (callsign.length() > 1 && callsign != "BLUEHAIR") {
        formatted = callsign + ": " + text;
    }
    g_trampoline.enqueue(new ChatJob(netShipId, formatted));
}

void enqueue_wingman_lost(int slot) {
    Despawn d;
    d.set_ship_id(NetworkShipId::from_local(slot).to_net());
    d.set_explode(1);
    g_trampoline.enqueue(new DespawnJob(DespawnJob::WINGMAN_LOST, d, slot, kInvalidSlot));
}

// ---------------------------------------------------------------------------
// despawn_all_ships: C++ translation of ovr140:1C16 (despawn) for the
// permanent slots, skipping the capital-ship death animation, used when a
// mission ends to make sure every machine starts the next one empty.

static void set_word_array(Bit16u base, int slot, Bit16u v) {
    wr16((Bit16u)(base + 2 * slot), v);
}

static void despawn_slot_quietly(int slot) {
    if (slot == kInvalidSlot) {
        return;
    }
    set_word_array(ds::entityCullStatus, slot, 0x8001);
    set_word_array(0xb862, slot, 0);
    if (rd16(ds::unknown07A6) == 0) {
        wr16(ds::unknown07A6, 0xffff);
    }
    if (rd16(ds::playerTarget) == 0) {
        wr16(ds::playerTarget, 0xffff);
    }
    for (int i = 0; i < 0x14; i++) {
        if (rd8((Bit16u)(ds::entitiesToDespawn + i)) == slot) {
            wr8((Bit16u)(ds::entitiesToDespawn + i), 0xff);
        }
    }
    if (slot < kMinTempSlot) {
        set_word_array(ds::commAnimInfo, slot, 2);
        wr8((Bit16u)(0xd1a2 + slot), 0xff);
        set_word_array(ds::vduStatus10WhenPlayerHitsSmth, slot, 0xff);
        set_word_array(0xc69e, slot, 0xffff);
        set_word_array(0xc38c, slot, 0);
        wr8((Bit16u)(0xc854 + slot), 0xff);
        set_word_array(0xbe26, slot, 0);
        set_word_array(0xbfe0, slot, 0xffff);
    }
    set_word_array(ds::maybeEntityType, slot, 0);
    wr32((Bit16u)(0xc754 + 4 * slot), 0);
}

void despawn_all_ships() {
    for (int i = 0; i < kMinTempSlot; i++) {
        despawn_slot_quietly(i);
    }
}

}  // namespace wc
