#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <deque>
#include <string>
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
#define kTmpVectorOff (ds::aLoadingWingCom)

// Soft rocks (RocksMode): what a rock does to a player's own ship.
enum { kSoftRockDivisor = 16, kSoftRockMost = 80 };

static std::deque<PendingFire> g_pendingFires;
static Bit32u g_nextFireSeq = 1;

// True while the server runs do_damage for a hit a client reported (one of
// that client's own rocks or mines against a ship the server owns).  The call
// comes from the trampoline, but it is the server's own decision and is
// broadcast like any other.
static bool g_applyingReport = false;

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

// Which ships and missiles are on their way out: a ship the game has blown up
// keeps its slot, in AI state 9, until the explosion is over, and a missile's
// slot becomes its explosion.
struct DoomedSnapshot {
    bool doomed[kMaxShipSlot + 1];
    bool alive[kMaxShipSlot + 1];
    static bool is_doomed(int slot) {
        Bit16u type = entity_type(slot);
        return type < ET_MISSILE || (type >= ET_SHIP && rd16((Bit16u)(ds::shipAiState + 2 * slot)) == 9);
    }
    void take() {
        for (int i = 0; i <= kMaxShipSlot; i++) {
            alive[i] = entity_type(i) >= ET_MISSILE;
            doomed[i] = is_doomed(i);
        }
    }
    // Destroyed since take(): it was a live ship or missile then.
    bool destroyed_since(int slot) const {
        return alive[slot] && !doomed[slot] && slot_in_use(slot) && is_doomed(slot);
    }
};

static GameCall call_of(const Loc &fn) {
    return GameCall(fn);
}

// The shooter's locked target (dseg:C284[shooter], 0xff = none) decides
// whether a missile homes; it is read when the shot is fired.
static void capture_lock(WeaponFire *fire, int shooter) {
    Bit8u target = rd8((Bit16u)(ds::missileTarget + shooter));
    if (target != 0xff) {
        NetworkShipId t = NetworkShipId::from_local(target);
        if (!t.is_invalid()) {
            fire->set_target(t.to_net());
        }
    }
}

static void apply_lock(const WeaponFire &fire, int shooter) {
    Bit8u target = 0xff;
    if (fire.has_target()) {
        if (!g_entityMap || g_entityMap->is_mapped(fire.target())) {
            int local = NetworkShipId::from_net(fire.target()).to_local();
            if (local != kInvalidSlot && slot_in_use(local)) {
                target = (Bit8u)local;
            }
        }
    }
    wr8((Bit16u)(ds::missileTarget + shooter), target);
}

// ---------------------------------------------------------------------------
// Jobs

// Runs fireGunFromShip(ship, gun); afterwards records which permanent slot
// (missile) it used and either broadcasts the event or resolves a predicted
// local fire.
class FireJob : public VmJob {
public:
    enum Mode { BROADCAST, PREDICT, REPLAY };
    FireJob(Mode mode, const WeaponFire &fire, int localShip, int gun, bool replayingClientFire = false)
        : mode_(mode), fire_(fire), localShip_(localShip), gun_(gun), replayingClientFire_(replayingClientFire) {}

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
        if (mode_ == REPLAY || replayingClientFire_) {
            apply_lock(fire_, localShip_);
        }
        wclog(2, "%s fire: ship %d (net %d) gun %d target %s",
              mode_ == PREDICT ? "predict" : (mode_ == REPLAY ? "replay" : "local"),
              localShip_, fire_.shooter(), gun_,
              fire_.has_target() ? std::to_string(fire_.target()).c_str() : "none");
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
    bool replayingClientFire_;  // server replaying a client's shot: use the client's lock
    SlotSnapshot before_;
    bool started_ = false;
};

class DamageJob : public VmJob {
public:
    // REPLAY: damage the server decided, replayed on a client.
    // REPORTED: on the server, a hit a client saw one of its own rocks or
    // mines do to a ship the server owns; the server applies it and its
    // do_damage hook broadcasts it (see g_applyingReport).
    enum Mode { REPLAY, REPORTED };
    DamageJob(Mode mode, const Damage &dam) : mode_(mode), dam_(dam) {}
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
        if (mode_ == REPORTED && (!is_ship_slot(dstLocal) || entity_type(dstLocal) < ET_SHIP)) {
            return false;  // only ships take collision or blast damage
        }
        int srcLocal = kInvalidSlot;
        if (mode_ == REPLAY && dam_.has_shooter()) {
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
        if (mode_ == REPLAY && dam_.has_seed()) {
            seed_rng(dam_.seed());
        }
        if (mode_ == REPORTED) {
            wclog(2, "reported damage to %d qty %d", dstLocal, dam_.quantity());
            g_applyingReport = true;
            before_.take();
        } else {
            wclog(2, "replay damage %d -> %d (local %d -> %d) qty %d", dam_.has_shooter() ? (int)dam_.shooter() : -1,
                  dam_.ship_id(), srcLocal, dstLocal, dam_.quantity());
        }
        call_of(code::do_damage)
            .arg((Bit16u)srcLocal).arg((Bit16u)dstLocal)
            .arg((Bit16u)dam_.quantity()).arg((Bit16u)kTmpVectorOff)
            .invoke();
        return true;
    }
    virtual void finish() {
        if (mode_ != REPORTED || !g_applyingReport) {
            return;
        }
        g_applyingReport = false;
        // A kill inside that damage (the ship itself, or one caught in its
        // blast) ran natively, nested in our call: the clients hear of it
        // here, after the damage.
        for (int slot = kMinShipSlot; slot <= kMaxShipSlot; slot++) {
            if (!before_.destroyed_since(slot) || g_session->is_player_slot(slot)) {
                continue;
            }
            wclog(2, "slot %d destroyed by reported damage", slot);
            Event ev;
            Despawn *d = ev.mutable_despawn();
            d->set_ship_id(NetworkShipId::from_local(slot).to_net());
            d->set_explode(1);
            g_session->queue_outgoing_event(ev);
        }
    }
    virtual const char *describe() const { return "damage"; }

private:
    Mode mode_;
    Damage dam_;
    DoomedSnapshot before_;
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
            // The mission setup gives a ship its pilot (dseg:D1A2) after the
            // spawn returns; on a client that setup never runs, this replayed
            // spawn only builds the ship, so the server sends the byte along.
            // The comms code names and draws the speaker from it and the
            // talk-to-wingman menu indexes by it, so it must be one of the
            // eight named pilots (0-7) for the body that stands in for the
            // host; 8, the player's own value, shows "(null)".
            int pilot = spawn_.has_pilot() ? (int)spawn_.pilot() : -1;
            if ((int)spawn_.ship_id() == g_entityMap->own_ship()) {
                wclog(2, "net %d is us (slot 0); slot %d becomes the server player's ship", spawn_.ship_id(), slot);
                const char *env = getenv("WCHOSTPILOT");  // 0-7: another named pilot for the host
                if (env && env[0] && atoi(env) >= 0 && atoi(env) <= 7) {
                    pilot = atoi(env);
                }
                if (pilot < 0 || pilot > 7) {
                    pilot = 0;  // an old server sent nothing usable: the first named pilot
                }
                wr8((Bit16u)(ds::shipStateByte + slot), (Bit8u)pilot);
                wclog(2, "slot %d wears pilot %d for the server player (spawn said %d)", slot, pilot,
                      spawn_.has_pilot() ? (int)spawn_.pilot() : -1);
            } else {
                wclog(2, "net %d -> slot %d", spawn_.ship_id(), slot);
                if (pilot >= 0) {
                    wr8((Bit16u)(ds::shipStateByte + slot), (Bit8u)pilot);
                }
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
        // The server sends one event when its autopilot camera starts and one
        // when it finishes.  Both are replayed by calling the game's
        // autoAnimation: the finish event carries no camera parameters, and
        // the resulting (0, 0, 0) call is what takes the client out of the
        // cinematic and back to the cockpit (as the original wc_net.cpp did);
        // skipping it leaves the client in the letterboxed camera view.
        bool finish = ape_.has_finish_camera() && ape_.finish_camera();
        wclog(2, "replay autopilot %s (%d, %d, %d)", finish ? "finish" : "camera",
              ape_.cam_ship_type(), ape_.cam_mode(), ape_.duration());
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
        // (ship, messageId).  The call sets up the comm display (face,
        // timer) and copies message 0 -- an empty string, shown as
        // "(null)" -- into commGlobalTxt; the VDU renders that buffer on
        // the following frames, so our text has to go in after the call
        // returns (finish), exactly as the original wc_net.cpp did.  With
        // the arguments the other way round the game shows message #slot
        // from ship 0 instead.
        call_of(code::showCommMessage).arg((Bit16u)local).arg(0).invoke();
        return true;
    }
    virtual void finish() {
        write_cstring(ds::commGlobalTxt, 80, text_);
    }
    virtual const char *describe() const { return "chat"; }

private:
    int netShip_;
    std::string text_;
};

// Removes this machine's rocks or mines and forgets the nav point's fields.
class ClearFieldsJob : public VmJob {
public:
    virtual bool start() {
        call_of(code::clearFields).invoke();
        return true;
    }
    virtual void finish() {
        wr16(ds::fieldCount, 0);
        wr16(ds::fieldRockTally, 0);
    }
    virtual const char *describe() const { return "clear fields"; }
};

// Registers one field of the current nav point again (the game's own call;
// the spawn hook lets it through because it comes from the trampoline).
class RegisterFieldJob : public VmJob {
public:
    RegisterFieldJob(int missionShip, int navPoint) : missionShip_(missionShip), navPoint_(navPoint) {}
    virtual bool start() {
        wclog(2, "registering field %d of nav point %d again", missionShip_, navPoint_);
        call_of(code::outerSpawnShipEntity).arg((Bit16u)missionShip_).arg((Bit16u)navPoint_).invoke();
        return true;
    }
    virtual const char *describe() const { return "register field"; }

private:
    int missionShip_, navPoint_;
};

// ---------------------------------------------------------------------------
// Rocks on or off

static int g_rocksMode = -1;  // -1: not yet read from WCROCKS

int rocks_mode() {
    if (g_rocksMode < 0) {
        const char *env = getenv("WCROCKS");
        g_rocksMode = !env || !env[0] ? ROCKS_ON
            : (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || !strcasecmp(env, "off")) ? ROCKS_OFF
            : (env[0] == '2' || !strcasecmp(env, "soft")) ? ROCKS_SOFT : ROCKS_ON;
    }
    return g_rocksMode;
}

void set_rocks_mode(int mode) {
    mode = mode == ROCKS_OFF ? ROCKS_OFF : mode == ROCKS_SOFT ? ROCKS_SOFT : ROCKS_ON;
    if (rocks_mode() != mode) {
        wclog(1, "asteroid and mine fields are %s", mode == ROCKS_OFF ? "off" : mode == ROCKS_SOFT ? "on, rocks soft" : "on");
    }
    g_rocksMode = mode;
}

void enqueue_rocks_change(int mode) {
    set_rocks_mode(mode);
    if (!rocks_enabled()) {
        g_trampoline.enqueue(new ClearFieldsJob());
        return;
    }
    if (rd16(ds::fieldCount) != 0) {
        return;  // still registered
    }
    int nav = rd16(ds::currentNavPoint);
    for (int i = 0; i < 10; i++) {
        int ship = rd16((Bit16u)(ds::navPointTable + 0x4d * nav + 0x39 + 2 * i));
        if (is_field_mission_ship(ship)) {
            g_trampoline.enqueue(new RegisterFieldJob(ship, nav));
        }
    }
}

// ---------------------------------------------------------------------------
// Interception

static bool replaying() {
    return g_trampoline.is_running();
}

// The network id of the ship behind a hit or a kill, when a ship is behind
// it: a rock or a mine belongs to nobody and exists on one machine only, so
// its slot number means nothing anywhere else.
static bool ship_behind(int src, int *net) {
    NetworkShipId shooter = NetworkShipId::from_top_level_local(src);
    if (shooter.is_invalid() || !is_ship_slot(top_level_parent(src))) {
        return false;
    }
    *net = shooter.to_net();
    return true;
}

// Client: one of this machine's own rocks or mines hit a ship the server
// owns.  The server cannot see that rock, so tell it; the damage comes back
// with the server's next frame like any other.
static void report_local_hit(Session *s, int dst, Bit16u quantity, Bit16u vecOff) {
    if (g_entityMap && !g_entityMap->is_local_mapped(dst)) {
        return;
    }
    Event ev;
    Damage *dam = ev.mutable_damage();
    dam->set_ship_id(NetworkShipId::from_local(dst).to_net());
    dam->set_quantity(quantity);
    Vec3 v = read_vec(vecOff);
    dam->mutable_pos()->set_x(v.x);
    dam->mutable_pos()->set_y(v.y);
    dam->mutable_pos()->set_z(v.z);
    wclog(2, "reporting a hit by our own rock or mine on slot %d (net %d) qty %d", dst, dam->ship_id(), quantity);
    s->queue_outgoing_event(ev);
}

void on_do_damage_entry() {
    Session *s = g_session;
    int src = call_arg16(0);
    int dst = call_arg16(1);
    Bit16u quantity = call_arg16(2);
    Bit16u vecOff = call_arg16(3);
    if (dst == kPlayerSlot && src >= 0 && src < kNumSlots && entity_type(src) == ET_ASTEROID && rocks_mode() == ROCKS_SOFT) {
        // Soft rocks: the game's (closing speed)^2 / 2 runs to 1000-3000 for
        // a head-on rock, against a Hornet's 85 of front shield and armor
        // (70 on a side).  A sixteenth of it, and never more than 80, takes
        // the shield and most of the armor but leaves any fresh ship alive
        // after one rock, wherever it lands.
        Bit16u soft = (Bit16u)(quantity / kSoftRockDivisor);
        soft = soft < 1 ? 1 : soft > kSoftRockMost ? (Bit16u)kSoftRockMost : soft;
        wclog(2, "soft rock: %d becomes %d", quantity, soft);
        quantity = soft;
        set_call_arg16(2, quantity);
    }
    if (dst == kPlayerSlot && src >= 0 && src < kNumSlots && entity_type(src) != ET_BOLT) {
        // What hit our own ship, other than gunfire (also when flying alone).
        wclog(2, "our ship is hit by slot %d (type %d, ship type %d) qty %d", src, entity_type(src),
              rd16((Bit16u)(ds::gInstanceShipTypes + 2 * src)), quantity);
    }
    if (!s) {
        return;
    }

    if (s->is_remote_player_slot(dst)) {
        // Only the owner's machine may damage a human's ship (see session.h).
        return_from_call(0);
        return;
    }
    if (replaying() && !g_applyingReport) {
        return;  // a replayed event: run natively
    }
    if (s->is_client()) {
        if (dst == kPlayerSlot) {
            return;  // our own ship: the player model runs here and only here
        }
        if (!is_ship_slot(dst)) {
            return;  // rocks, mines, bolts: every machine has its own
        }
        if (quantity != 0 && is_local_hazard(src) && slot_in_use(dst) && entity_type(dst) >= ET_SHIP) {
            report_local_hit(s, dst, quantity, vecOff);
        }
        return_from_call(0);  // NPC damage is decided by the server
        return;
    }
    if (dst == kPlayerSlot) {
        return;  // server's own ship: native, nobody else needs it
    }
    if (!is_ship_slot(dst)) {
        return;  // rocks, mines, bolts: not replicated
    }
    Event ev;
    Damage *dam = ev.mutable_damage();
    dam->set_ship_id(NetworkShipId::from_local(dst).to_net());
    int shooter;
    if (ship_behind(src, &shooter)) {
        dam->set_shooter(shooter);
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
        capture_lock(&fire, ship);
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
    capture_lock(&fire, ship);
    g_trampoline.enqueue(new FireJob(FireJob::BROADCAST, fire, ship, gun));
    g_trampoline.run_instead_of_current_call();
}

void on_spawn_entry() {
    Session *s = g_session;
    if (is_field_mission_ship(call_arg16(0)) && !rocks_enabled()) {
        return_from_call(0xffff);  // rocks are switched off: the nav point has no field
        return;
    }
    if (!s || replaying()) {
        return;
    }
    if (is_field_mission_ship(call_arg16(0))) {
        // Not a ship: the nav point's asteroid or mine field (ovr145:1183
        // only adds it to the field table).  Every machine sets up a nav
        // point for itself when its own player gets there, clearing that
        // table first, so each registers the field here and then keeps its
        // own rocks around its own ship.
        wclog(2, "mission ship %d is a field: registered locally", call_arg16(0));
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
        if (explode && (entity_type(ship) == ET_ASTEROID || entity_type(ship) == ET_MINE)) {
            wclog(2, "%s in slot %d destroyed", entity_type(ship) == ET_ASTEROID ? "rock" : "mine", ship);
        }
        return;  // temporary entities are local on every machine
    }
    if (!slot_in_use(ship)) {
        // Mission setup despawns every slot to reset its bookkeeping (pilot
        // 0xff, comm flags, cull status, ...).  Nothing to tell anyone, and
        // skipping it leaves the unused slots as whatever the last mission
        // left there -- pilot 0 made them look like extra copies of the
        // wingman to the comms code.
        return;
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
        int shooter;
        if (ship_behind(src, &shooter)) {
            d.set_shooter(shooter);
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
        reg_eip = code::aiSetSpeedReturn.off;
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
    g_trampoline.enqueue(new FireJob(mode, fire, local, fire.gun_id(), g_session->is_server()));
}

void enqueue_remote_event(const Event &ev) {
    if (ev.has_fire()) {
        enqueue_fire(ev.fire());
    }
    if (ev.has_damage()) {
        g_trampoline.enqueue(new DamageJob(DamageJob::REPLAY, ev.damage()));
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

void enqueue_reported_damage(const Damage &reported) {
    // Only what a collision report needs: no source, no seed of the client's.
    Damage dam;
    dam.set_ship_id(reported.ship_id());
    dam.set_quantity(reported.quantity() & 0xffff);
    if (reported.has_pos()) {
        *dam.mutable_pos() = reported.pos();
    }
    g_trampoline.enqueue(new DamageJob(DamageJob::REPORTED, dam));
}

void enqueue_chat_display(int netShipId, const std::string &callsign, const std::string &text) {
    std::string formatted = text;
    if (callsign.length() > 1 && callsign != "BLUEHAIR") {
        formatted = callsign + ": " + text;
    }
    g_trampoline.enqueue(new ChatJob(netShipId, formatted));
}

void enqueue_wingman_lost(int slot, bool explode) {
    Despawn d;
    d.set_ship_id(NetworkShipId::from_local(slot).to_net());
    if (explode) {
        d.set_explode(1);
    }
    g_trampoline.enqueue(new DespawnJob(DespawnJob::WINGMAN_LOST, d, slot, kInvalidSlot));
}

// Test aid (the key script's "!kill=<slot>"): the server destroys a ship as
// if the player had shot it, through the same broadcast path as a real kill.
void enqueue_test_kill(int slot) {
    Despawn d;
    d.set_ship_id(NetworkShipId::from_local(slot).to_net());
    d.set_explode(1);
    d.set_shooter(NetworkShipId::from_local(kPlayerSlot).to_net());
    g_trampoline.enqueue(new DespawnJob(DespawnJob::BROADCAST, d, slot, kPlayerSlot));
}

// Test aid: damage to our own ship as the game would apply it (shield, armor,
// then the player's own damage model with its cockpit damage).
class OwnHitJob : public VmJob {
public:
    OwnHitJob(int quantity, bool fromBehind) : quantity_(quantity), fromBehind_(fromBehind) {}
    virtual bool start() {
        // The vector is the way the blow travels: against our nose for a
        // hit from ahead (front shield and armor), along it from behind.
        Vec3 v = read_vec(vec_slot(ds::gOrientationFrontVector, kPlayerSlot));
        if (!fromBehind_) {
            v.x = -v.x; v.y = -v.y; v.z = -v.z;
        }
        write_vec(kTmpVectorOff, v);
        call_of(code::do_damage)
            .arg((Bit16u)kInvalidSlot).arg((Bit16u)kPlayerSlot)
            .arg((Bit16u)quantity_).arg((Bit16u)kTmpVectorOff)
            .invoke();
        return true;
    }
    virtual const char *describe() const { return "own hit"; }

private:
    int quantity_;
    bool fromBehind_;
};

void enqueue_test_hit(int quantity, bool fromBehind) {
    g_trampoline.enqueue(new OwnHitJob(quantity, fromBehind));
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
