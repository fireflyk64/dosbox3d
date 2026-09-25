#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <map>
#include <vector>
#include "wc_net.h"
#include "wcnet_session.h"
#include "wcnet_events.h"
#include "wcnet_vm.h"
#include "wcnet_memory.h"
#include "wcnet_code.h"
#include "wcnet_transport.h"
#include "wcnet_log.h"
#include "cpu.h"
#include "regs.h"

// Text shown by the GUI overlay while not in space (src/gui/sdlmain.cpp).
extern std::string incoming_text;

bool within_briefed_mission = false;
NetConfig net_config;

namespace wc {

Session *g_session = NULL;
int in_simulation = 0;
bool has_started_up = false;

// ---------------------------------------------------------------------------
// NetConfig

}  // namespace wc

NetConfig::NetConfig() {
    reset_from_env();
}

void NetConfig::reset_from_env() {
    reset(getenv("WCHOST"), getenv("WCPORT"));
}

void NetConfig::reset(const char *h, const char *p) {
    host = h;
    portstr = p ? p : "13255";
    port = (uint16_t)atoi(portstr);
    if (port < 1024) {
        fprintf(stderr, "You must set the WCPORT and (optionally) WCHOST env variables! Not %s\n", portstr);
        abort();
    }
}

namespace wc {

// ---------------------------------------------------------------------------
// Pilot identity and small game helpers

std::string get_last_name() {
    std::string saved = read_cstring(ds::pilotLastName, 14);
    const char *env = getenv("WCLASTNAME");
    if (env && env[0] && saved != env) {
        saved = env;
        write_cstring(ds::pilotLastName, 14, saved);
    }
    return saved;
}

std::string get_callsign() {
    std::string saved = read_cstring(ds::pilotCallsign, 14);
    const char *env = getenv("WCCALLSIGN");
    if (env && env[0] && saved != env) {
        saved = env;
        write_cstring(ds::pilotCallsign, 14, saved);
    }
    return saved;
}

// Clear the KIA flags so the debriefing never tells a sob story about a
// wingman who is in fact alive on another machine.
static void liven_everyone() {
    for (int i = 0; i < 8; i++) {
        wr32((Bit16u)(ds::statusPilots + i), 0);
    }
}

enum { kMissionTreeSize = 8 * 19 };

static void load_mission_tree_progress(const std::string &data) {
    if (data.length() != kMissionTreeSize) {
        wclog(0, "unexpected mission tree length %d", (int)data.length());
        return;
    }
    for (int i = 0; i < kMissionTreeSize; i++) {
        wr8((Bit16u)(ds::navPointState + i), (Bit8u)data[i]);
    }
}

static void populate_mission_end(MissionEnd *end) {
    end->set_game_update((GameState)rd16(ds::missionStatus));
    std::string *tree = end->mutable_mission_tree_progress();
    tree->clear();
    for (int i = 0; i < kMissionTreeSize; i++) {
        tree->push_back((char)rd8((Bit16u)(ds::navPointState + i)));
    }
}

static void populate_mission_addendum(MissionEnd *end) {
    end->set_victory_points_plus_one(1 + (Bit32u)rd16(ds::victoryPoints));
}

void run_campaign(int missionId, int seriesId) {
    wr8(ds::currentMission, (Bit8u)missionId);
    wr8(ds::currentSeries, (Bit8u)seriesId);
    wr8(ds::savedGameLoaded, 1);
    CPU_Push16((Bit16u)SegValue(cs));
    CPU_Push16((Bit16u)reg_eip + 5);
    SegSet16(cs, code::runHangarMission.stubSeg);
    reg_eip = code::runHangarMission.stubOff;
}

template <class V>
static void fill_vector(V *out, Bit16u off) {
    Vec3 v = read_vec(off);
    out->set_x(v.x);
    out->set_y(v.y);
    out->set_z(v.z);
}

template <class V>
static Vec3 to_vec(const V &in) {
    Vec3 v = { in.x(), in.y(), in.z() };
    return v;
}

static void populate_location(Location *loc, int slot) {
    fill_vector(loc->mutable_pos(), vec_slot(ds::gPositionVector, slot));
    fill_vector(loc->mutable_vel(), vec_slot(ds::gVelocityVector, slot));
    fill_vector(loc->mutable_right(), vec_slot(ds::gOrientationRightVector, slot));
    fill_vector(loc->mutable_up(), vec_slot(ds::gOrientationUpVector, slot));
    fill_vector(loc->mutable_fore(), vec_slot(ds::gOrientationFrontVector, slot));
    loc->set_set_speed(rd32((Bit16u)(ds::setSpeed + 4 * slot)));
}

static void apply_location(const Location &loc, int slot) {
    if (loc.has_pos()) {
        write_vec(vec_slot(ds::gPositionVector, slot), to_vec(loc.pos()));
    }
    if (loc.has_vel()) {
        write_vec(vec_slot(ds::gVelocityVector, slot), to_vec(loc.vel()));
    }
    if (loc.has_right() && loc.has_up() && loc.has_fore()) {
        write_vec(vec_slot(ds::gOrientationRightVector, slot), to_vec(loc.right()));
        write_vec(vec_slot(ds::gOrientationUpVector, slot), to_vec(loc.up()));
        write_vec(vec_slot(ds::gOrientationFrontVector, slot), to_vec(loc.fore()));
    }
    if (loc.has_set_speed()) {
        wr32((Bit16u)(ds::setSpeed + 4 * slot), loc.set_speed());
    }
}

static void fill_health(ShipHealth *out, const ShipHealthState &h) {
    out->Clear();
    for (int i = 0; i < 2; i++) {
        out->add_shield(h.shield[i]);
        out->add_shield_max(h.shieldMax[i]);
    }
    for (int i = 0; i < 4; i++) {
        out->add_armor(h.armor[i]);
    }
    out->set_damage_points(h.damagePoints);
    out->set_core_hp(h.coreHp);
    out->set_hull_counter(h.hullCounter);
    out->set_state_byte(h.stateByte);
    out->set_gun_damage(h.gunDamage);
    out->set_engine_flag(h.engineFlag);
    out->set_gun_energy(h.gunEnergy);
}

static bool parse_health(const ShipHealth &in, ShipHealthState *h) {
    if (in.shield_size() != 2 || in.shield_max_size() != 2 || in.armor_size() != 4) {
        return false;
    }
    memset(h, 0, sizeof(*h));
    for (int i = 0; i < 2; i++) {
        h->shield[i] = (Bit16s)in.shield(i);
        h->shieldMax[i] = (Bit16s)in.shield_max(i);
    }
    for (int i = 0; i < 4; i++) {
        h->armor[i] = (Bit16s)in.armor(i);
    }
    h->damagePoints = (Bit16s)in.damage_points();
    h->coreHp = (Bit8u)in.core_hp();
    h->hullCounter = (Bit8u)in.hull_counter();
    h->stateByte = (Bit8u)in.state_byte();
    h->gunDamage = (Bit8u)in.gun_damage();
    h->engineFlag = (Bit8u)in.engine_flag();
    h->gunEnergy = (Bit16s)in.gun_energy();
    return true;
}

// Sends a ship's health when it changed, and periodically anyway so a lost
// update cannot leave a peer stale forever.
class HealthPublisher {
public:
    HealthPublisher() : framesSince_(0) {}
    void reset() { last_.clear(); framesSince_ = 0; }
    void next_frame() { framesSince_++; }
    bool should_send(int slot, ShipHealthState *out) {
        *out = read_health(slot);
        std::map<int, ShipHealthState>::iterator it = last_.find(slot);
        bool changed = it == last_.end() || it->second != *out;
        if (!changed && framesSince_ < kPeriod) {
            return false;
        }
        last_[slot] = *out;
        return true;
    }
    void end_frame() {
        if (framesSince_ >= kPeriod) {
            framesSince_ = 0;
        }
    }

private:
    enum { kPeriod = 30 };
    std::map<int, ShipHealthState> last_;
    int framesSince_;
};

static ShipUpdate *update_for(Frame *frame, int net) {
    for (int i = 0; i < frame->update_size(); i++) {
        if (frame->update(i).has_ship_id() && (int)frame->update(i).ship_id() == net) {
            return frame->mutable_update(i);
        }
    }
    ShipUpdate *su = frame->add_update();
    su->set_ship_id(net);
    return su;
}

static const char *status_name(int status) {
    switch (status) {
    case Proceed: return "proceed";
    case EndLand: return "landed";
    case EndEject: return "ejected";
    case EndCarrier: return "carrier";
    case EndDeath: return "died";
    case EndExit: return "exited";
    }
    return "?";
}

static bool ends_only_this_player(int status) {
    return status == EndDeath || status == EndEject || status == EndExit;
}

static void handle_incoming_chat(const Chat &chat) {
    std::string formatted = chat.message();
    if (chat.callsign().length() > 1 && chat.callsign() != "BLUEHAIR") {
        formatted = chat.callsign() + ": " + chat.message();
    }
    incoming_text = formatted;
    enqueue_chat_display(chat.ship_id(), chat.callsign(), chat.message());
}

// ---------------------------------------------------------------------------
// Server

struct RemoteClient {
    int net;                     // network id == server slot this client flies
    Connection conn;
    std::string callsign;
    std::string missionTreeProgress;  // reported with a shared mission end
    bool requestedBriefingStart;
    bool needsMissionStartState;
    bool inMission;              // takes part in the frame exchange
    bool leftThisMission;        // died/ejected/exited this mission

    explicit RemoteClient(int n)
        : net(n), requestedBriefingStart(false), needsMissionStartState(false),
          inMission(false), leftThisMission(false) {}
    bool connected() const { return conn.is_open(); }
    void disconnect() {
        conn.close();
        inMission = false;
        needsMissionStartState = false;
        requestedBriefingStart = false;
    }
};

class ServerSession : public Session {
public:
    ServerSession()
        : listenFd_(-1), epoch_(1), frameNumber_(0), sendFrameAtIdle_(false), ignoreNextFrameTop_(false) {
        allowedIds_.push_back(1);
        allowedIds_.push_back(3);
        reset_pending_frame();
    }
    ~ServerSession() {
        if (listenFd_ != -1) {
            close_socket(listenFd_);
        }
        for (size_t i = 0; i < clients_.size(); i++) {
            delete clients_[i];
        }
    }

    bool listen() {
        listenFd_ = listen_on(net_config.portstr);
        return listenFd_ != -1;
    }

    virtual Role role() const { return ROLE_SERVER; }

    virtual bool is_remote_player_slot(int slot) const {
        RemoteClient *c = client_for_slot(slot);
        return c != NULL && c->inMission;
    }

    virtual void queue_outgoing_event(const Event &ev) {
        *pendingFrame_.mutable_frame()->add_event() = ev;
    }
    virtual void on_spawned(const Spawn &spawn) { spawns_.add(spawn); }
    virtual void on_despawned(int net) { spawns_.remove(net); }

    virtual void on_mission_starting(int mission, int series) {
        liven_everyone();
        within_briefed_mission = true;
        MissionEnd end;
        populate_mission_end(&end);
        lastBriefing_.set_mission_tree_progress(end.mission_tree_progress());
        lastBriefing_.set_mission_id(mission);
        lastBriefing_.set_series_id(series);
        accept_clients(true);
        wclog(1, "mission %d/%d starting with %d client slot(s)", mission, series, (int)clients_.size());
        for (size_t i = 0; i < clients_.size(); i++) {
            RemoteClient *c = clients_[i];
            if (c->connected() && c->requestedBriefingStart) {
                send_briefing_state(c);
            }
        }
    }

    virtual void on_mission_reset() {
        despawn_all_ships();
        spawns_.clear();
        health_.reset();
        epoch_ += 1;
        frameNumber_ = 0;
        for (size_t i = 0; i < clients_.size(); i++) {
            clients_[i]->leftThisMission = false;
            clients_[i]->inMission = false;
            clients_[i]->missionTreeProgress.clear();
        }
    }

    virtual void on_mission_victory_calc() {
        MissionEnd *end = pendingFrame_.mutable_frame()->mutable_mission_end();
        populate_mission_addendum(end);
        wclog(1, "victory points %d", (int)end->victory_points_plus_one() - 1);
        sendFrameAtIdle_ = true;
        exchange(true);
    }

    virtual void on_mission_ended() {
        liven_everyone();
        within_briefed_mission = false;
        int status = rd16(ds::missionStatus);
        wclog(1, "mission ended: %s", status_name(status));
        if (status > EndExit) {
            return;
        }
        for (size_t i = 0; i < clients_.size(); i++) {
            if (!clients_[i]->missionTreeProgress.empty()) {
                load_mission_tree_progress(clients_[i]->missionTreeProgress);
            }
        }
        MissionEnd *end = pendingFrame_.mutable_frame()->mutable_mission_end();
        populate_mission_end(end);
        if (status == EndDeath || status == EndCarrier) {
            // These endings skip the victory computation, so flush now.
            sendFrameAtIdle_ = true;
            exchange(true);
        }
    }

    virtual void on_frame_top() {
        if (ignoreNextFrameTop_) {
            ignoreNextFrameTop_ = false;
            return;
        }
        exchange(false);
    }

    virtual void on_async_tick() {
        accept_clients(false);
        NetworkMessage msg;
        for (size_t i = 0; i < clients_.size(); i++) {
            RemoteClient *c = clients_[i];
            if (!c->connected()) {
                continue;
            }
            if (c->conn.poll(CAT_CHAT, msg).ok()) {
                handle_incoming_chat(msg.chat());
                for (size_t j = 0; j < clients_.size(); j++) {
                    if (j != i && clients_[j]->connected()) {
                        clients_[j]->conn.send(msg);
                    }
                }
            }
            // Requests and stale frames that arrived while we were not in
            // the frame loop.
            const NetworkMessage *next = c->conn.peek(CAT_GAME);
            if (next && next->has_frame() && next->epoch() != epoch_ && !in_simulation) {
                c->conn.recv(CAT_GAME, msg);
                wclog(2, "discarding frame from epoch %u (now %u)", msg.epoch(), epoch_);
            } else if (next && next->has_start_briefing_req()) {
                c->conn.recv(CAT_GAME, msg);
                handle_briefing_request(c);
            }
        }
    }

    virtual void on_barracks() {
        accept_clients(true);
        wclog(1, "server has %d client slot(s)", (int)clients_.size());
    }

    virtual void check_mission_status() {}

    virtual void on_autopilot_begin(Bit16u camShipType, Bit16u camMode, Bit16u duration) {
        AutoPilotEvent *ape = pendingFrame_.mutable_frame()->add_event()->mutable_autopiloting();
        ape->set_cam_ship_type(camShipType);
        ape->set_cam_mode(camMode);
        ape->set_duration(duration);
        exchange(true);
    }

    virtual void on_autopilot_finished() {
        AutoPilotEvent *ape = pendingFrame_.mutable_frame()->add_event()->mutable_autopiloting();
        ape->set_finish_camera(true);
        exchange(true);
    }

    virtual void on_trampoline_idle() {
        flush_outgoing_frame();
    }

    virtual bool in_space() const {
        if (!within_briefed_mission) {
            return false;
        }
        for (size_t i = 0; i < clients_.size(); i++) {
            if (clients_[i]->connected() && clients_[i]->needsMissionStartState) {
                return false;
            }
        }
        return true;
    }

    virtual void send_chat(const std::string &text) {
        NetworkMessage msg;
        Chat *chat = msg.mutable_chat();
        chat->set_ship_id(0);
        chat->set_message(text);
        chat->set_callsign(get_callsign());
        for (size_t i = 0; i < clients_.size(); i++) {
            if (clients_[i]->connected() && !clients_[i]->conn.send(msg)) {
                wclog(0, "failed to send chat to client %d", clients_[i]->net);
            }
        }
    }

private:
    RemoteClient *client_for_slot(int slot) const {
        if (slot < 0 || slot >= (int)clients_.size()) {
            return NULL;
        }
        RemoteClient *c = clients_[slot];
        return c->connected() ? c : NULL;
    }

    RemoteClient *allocate_client() {
        for (size_t i = 0; i < allowedIds_.size(); i++) {
            int net = allowedIds_[i];
            while (net >= (int)clients_.size()) {
                clients_.push_back(new RemoteClient((int)clients_.size()));
            }
            if (!clients_[net]->connected()) {
                return clients_[net];
            }
        }
        return NULL;
    }

    bool any_client_connected() const {
        for (size_t i = 0; i < clients_.size(); i++) {
            if (clients_[i]->connected()) {
                return true;
            }
        }
        return false;
    }

    // Accept pending connections.  With blockIfNone the call waits until at
    // least one client is connected: a server alone cannot fly the mission.
    void accept_clients(bool blockIfNone) {
        while (true) {
            bool block = blockIfNone && !any_client_connected();
            if (block) {
                wclog(1, "waiting for a client to connect on port %s", net_config.portstr);
            }
            int s = accept_connection(listenFd_, block);
            if (s < 0) {
                return;
            }
            RemoteClient *c = allocate_client();
            if (!c) {
                wclog(1, "no free player slot; refusing connection");
                close_socket(s);
                return;
            }
            c->conn.adopt(s);
            NetworkMessage msg;
            if (!c->conn.recv(CAT_GAME, msg).ok() || !msg.has_connect()) {
                wclog(0, "client did not introduce itself");
                c->disconnect();
                continue;
            }
            c->callsign = msg.connect().callsign();
            c->leftThisMission = false;
            c->missionTreeProgress.clear();
            wclog(1, "%s connected as player %d", c->callsign.c_str(), c->net);
            if (within_briefed_mission) {
                // Joining a mission in progress: bring them in next frame.
                c->needsMissionStartState = true;
            }
        }
    }

    void handle_briefing_request(RemoteClient *c) {
        if (within_briefed_mission && !c->leftThisMission) {
            wclog(1, "player %d joins the mission in progress", c->net);
            send_briefing_state(c);
        } else {
            wclog(1, "player %d will get the next briefing", c->net);
            c->requestedBriefingStart = true;
        }
    }

    bool send_briefing_state(RemoteClient *c) {
        NetworkMessage msg;
        *msg.mutable_briefing_start() = lastBriefing_;
        msg.set_epoch(epoch_);
        msg.set_frame_number(frameNumber_);
        if (!c->conn.send(msg)) {
            c->disconnect();
            return false;
        }
        c->requestedBriefingStart = false;
        c->needsMissionStartState = true;
        return true;
    }

    bool send_start_state(RemoteClient *c) {
        NetworkMessage msg;
        msg.set_epoch(epoch_);
        msg.set_frame_number(frameNumber_);
        Game *game = msg.mutable_game();
        game->set_assigned_player_id(c->net);
        Frame *frame = game->mutable_starting_state();
        for (int slot = kPlayerSlot; slot <= kMaxShipSlot; slot++) {
            if (!slot_in_use(slot)) {
                continue;
            }
            ShipUpdate *su = update_for(frame, slot);
            populate_location(su->mutable_loc(), slot);
            if (!client_for_slot(slot)) {
                fill_health(su->mutable_health(), read_health(slot));
            }
        }
        const std::map<int, Spawn> &spawns = spawns_.all();
        for (std::map<int, Spawn>::const_iterator it = spawns.begin(); it != spawns.end(); ++it) {
            *frame->add_event()->mutable_spawn() = it->second;
        }
        wclog(1, "sending mission start state to player %d (%d ships)", c->net, (int)spawns.size());
        if (!c->conn.send(msg)) {
            c->disconnect();
            return false;
        }
        c->inMission = true;
        return true;
    }

    void reset_pending_frame() {
        pendingFrame_.Clear();
        pendingFrame_.set_epoch(epoch_);
        pendingFrame_.set_frame_number(frameNumber_);
    }

    void populate_server_frame() {
        Frame *frame = pendingFrame_.mutable_frame();
        health_.next_frame();
        for (int slot = kPlayerSlot; slot <= kMaxShipSlot; slot++) {
            if (!slot_in_use(slot)) {
                continue;
            }
            ShipUpdate *su = update_for(frame, slot);
            populate_location(su->mutable_loc(), slot);
            if (!is_remote_player_slot(slot)) {
                ShipHealthState h;
                if (health_.should_send(slot, &h)) {
                    fill_health(su->mutable_health(), h);
                }
            }
        }
        health_.end_frame();
    }

    void merge_client_frame(RemoteClient *c, const Frame &frame) {
        if (frame.has_player_end()) {
            int status = frame.player_end().state();
            wclog(1, "player %d %s; the mission continues without them", c->net, status_name(status));
            c->inMission = false;
            c->leftThisMission = true;
            c->needsMissionStartState = false;
            if (slot_in_use(c->net)) {
                enqueue_wingman_lost(c->net);
                spawns_.remove(c->net);
            }
            return;
        }
        if (frame.has_mission_end()) {
            // A shared ending (landing): end our mission too, with their
            // nav-point progress folded in when the debriefing runs.
            wr16(ds::missionStatus, (Bit16u)frame.mission_end().game_update());
            c->missionTreeProgress = frame.mission_end().mission_tree_progress();
            wclog(1, "player %d %s; ending the mission", c->net, status_name(frame.mission_end().game_update()));
        }
        for (int i = 0; i < frame.update_size(); i++) {
            const ShipUpdate &su = frame.update(i);
            if (!su.has_ship_id() || (int)su.ship_id() != c->net) {
                continue;  // clients only speak for their own ship
            }
            if (!slot_in_use(c->net)) {
                continue;
            }
            if (su.has_loc()) {
                apply_location(su.loc(), c->net);
                *update_for(pendingFrame_.mutable_frame(), c->net)->mutable_loc() = su.loc();
            }
            if (su.has_health()) {
                ShipHealthState h;
                if (parse_health(su.health(), &h)) {
                    write_health(c->net, h);
                    *update_for(pendingFrame_.mutable_frame(), c->net)->mutable_health() = su.health();
                }
            }
        }
        for (int i = 0; i < frame.event_size(); i++) {
            const Event &ev = frame.event(i);
            if (ev.has_fire()) {
                Event copy;
                *copy.mutable_fire() = ev.fire();
                copy.mutable_fire()->set_shooter(c->net);
                enqueue_remote_event(copy);
            } else {
                wclog(2, "ignoring non-fire event from player %d", c->net);
            }
        }
    }

    // Receive one message from a client in the mission, handling anything
    // that is not a frame in place.  Returns false when the client is gone.
    bool receive_client_frame(RemoteClient *c, bool mergeUpdates) {
        NetworkMessage msg;
        for (int attempts = 0; attempts < 16; attempts++) {
            if (!c->conn.recv(CAT_GAME, msg).ok()) {
                return false;
            }
            if (msg.has_start_briefing_req()) {
                handle_briefing_request(c);
                continue;
            }
            if (msg.has_start_mission_req()) {
                return send_start_state(c);
            }
            if (msg.has_frame()) {
                if (msg.epoch() != epoch_) {
                    wclog(2, "player %d sent a frame from epoch %u (now %u)", c->net, msg.epoch(), epoch_);
                } else if (mergeUpdates || msg.frame().has_player_end()) {
                    merge_client_frame(c, msg.frame());
                }
                return true;
            }
            wclog(0, "unexpected %s message from player %d", message_type_name(msg), c->net);
            return false;
        }
        return false;
    }

    // One round of the lockstep exchange.  With serverOnlyFlush the client
    // messages are still consumed (to keep the 1:1 message pattern) but only
    // their player_end is honoured, and the frame is sent immediately instead
    // of after the trampoline.
    void exchange(bool serverOnlyFlush) {
        frameNumber_ += 1;
        populate_server_frame();
        for (size_t i = 0; i < clients_.size(); i++) {
            RemoteClient *c = clients_[i];
            if (!c->connected()) {
                continue;
            }
            if (c->needsMissionStartState) {
                c->needsMissionStartState = false;
                if (!send_start_state(c)) {
                    continue;
                }
            }
            if (!c->inMission) {
                continue;
            }
            if (!receive_client_frame(c, !serverOnlyFlush)) {
                wclog(1, "player %d disconnected", c->net);
                c->disconnect();
            }
        }
        sendFrameAtIdle_ = true;
        accept_clients(true);
        if (serverOnlyFlush) {
            flush_outgoing_frame();
        } else {
            g_trampoline.run_before_current_instruction();
            ignoreNextFrameTop_ = true;
        }
    }

    void flush_outgoing_frame() {
        if (!sendFrameAtIdle_) {
            return;
        }
        sendFrameAtIdle_ = false;
        for (size_t i = 0; i < clients_.size(); i++) {
            RemoteClient *c = clients_[i];
            if (c->connected() && c->inMission && !c->conn.send(pendingFrame_)) {
                wclog(1, "player %d disconnected while sending", c->net);
                c->disconnect();
            }
        }
        reset_pending_frame();
    }

    int listenFd_;
    std::vector<RemoteClient *> clients_;
    std::vector<int> allowedIds_;
    Bit32u epoch_;
    Bit64u frameNumber_;
    ServerSendBriefingStart lastBriefing_;
    NetworkMessage pendingFrame_;
    SpawnRegistry spawns_;
    HealthPublisher health_;
    bool sendFrameAtIdle_;
    bool ignoreNextFrameTop_;
};

// ---------------------------------------------------------------------------
// Client

extern bool g_pendingUninit;

class ClientSession : public Session {
public:
    ClientSession()
        : shipNet_(0), lastWrittenMissionStatus_(Proceed), lastVictoryPlusOne_(0),
          isFresh_(true), hasRestartedMission_(false), hasSentConnect_(false),
          ignoreNextFrameTop_(false), ownMissionOver_(false), dead_(false), epoch_(0), frameNumber_(0) {
        callsign_ = get_callsign();
        g_entityMap = &entities_;
    }
    ~ClientSession() {
        g_entityMap = NULL;
    }

    bool connect() {
        int s = connect_to(net_config.host, net_config.portstr);
        if (s < 0) {
            return false;
        }
        conn_.adopt(s);
        return true;
    }

    virtual Role role() const { return ROLE_CLIENT; }

    virtual bool is_remote_player_slot(int slot) const {
        return entities_.is_mapped(0) && entities_.net_to_local(0) == slot;
    }

    virtual void queue_outgoing_event(const Event &ev) {
        *pendingFrame_.mutable_frame()->add_event() = ev;
    }

    virtual void on_mission_starting(int mission, int series) {
        (void)mission; (void)series;
        liven_everyone();
        within_briefed_mission = true;
        if (!dead_) {
            pre_briefing_handshake();
        }
    }

    virtual void on_mission_reset() {
        despawn_all_ships();
        entities_.reset();
        reset_pending_fires();
        pendingHealth_.clear();
        health_.reset();
        hasRestartedMission_ = true;
        ownMissionOver_ = false;
        wclog(1, "client reset its mission state");
    }

    virtual void on_mission_victory_calc() {
        if (lastVictoryPlusOne_) {
            wr16(ds::victoryPoints, (Bit16u)(lastVictoryPlusOne_ - 1));
            wclog(1, "victory points from server: %d", (int)lastVictoryPlusOne_ - 1);
        }
        lastVictoryPlusOne_ = 0;
    }

    virtual void on_mission_ended() {
        liven_everyone();
        within_briefed_mission = false;
        if (!missionTreeProgress_.empty()) {
            load_mission_tree_progress(missionTreeProgress_);
            missionTreeProgress_.clear();
        }
    }

    virtual void on_frame_top() {
        if (ignoreNextFrameTop_) {
            ignoreNextFrameTop_ = false;
            return;
        }
        if (dead_) {
            return;
        }
        NetworkMessage msg;
        if (hasRestartedMission_) {
            hasRestartedMission_ = false;
            wclog(1, "waiting for the mission start state");
            while (true) {
                if (!conn_.recv(CAT_GAME, msg).ok()) {
                    drop("lost the server while waiting for the mission start");
                    return;
                }
                if (msg.has_game()) {
                    break;
                }
            }
            apply_start_state(msg);
        }
        if (isFresh_ && !hasSentConnect_) {
            if (!send_connect()) {
                drop("could not introduce ourselves");
                return;
            }
        }
        pendingFrame_.set_epoch(epoch_);
        pendingFrame_.set_frame_number(++frameNumber_);
        populate_own_update();
        if (!conn_.send(pendingFrame_)) {
            drop("lost the server while sending a frame");
            return;
        }
        pendingFrame_.Clear();
        if (!conn_.recv(CAT_GAME, msg).ok() || !msg.has_frame()) {
            drop("lost the server while waiting for a frame");
            return;
        }
        if (msg.epoch() == epoch_) {
            bool applyOwn = false;
            for (int i = 0; i < msg.frame().event_size(); i++) {
                if (msg.frame().event(i).has_autopiloting()) {
                    applyOwn = true;
                }
            }
            apply_frame(msg.frame(), applyOwn);
        } else {
            wclog(2, "ignoring frame from epoch %u (ours %u)", msg.epoch(), epoch_);
        }
        g_trampoline.run_before_current_instruction();
        ignoreNextFrameTop_ = true;
    }

    virtual void on_async_tick() {
        if (dead_) {
            return;
        }
        NetworkMessage msg;
        RecvStatus st = conn_.poll(CAT_CHAT, msg);
        if (st.ok()) {
            handle_incoming_chat(msg.chat());
        } else if (st.failed()) {
            drop("the server went away");
        }
    }

    virtual void on_barracks() {
        if (dead_) {
            return;
        }
        for (int attempt = 0; attempt < 10; attempt++) {
            if (!conn_.is_open() && !connect()) {
                wclog(1, "connect attempt %d failed", attempt + 1);
                sleep(2);
                continue;
            }
            if (!hasSentConnect_ && !send_connect()) {
                conn_.close();
                sleep(2);
                continue;
            }
            wclog(1, "connected to %s as %s", net_config.host, callsign_.c_str());
            return;
        }
    }

    virtual void check_mission_status() {
        int status = rd16(ds::missionStatus);
        if (status == lastWrittenMissionStatus_) {
            return;
        }
        if (status == Proceed) {
            lastWrittenMissionStatus_ = Proceed;
            return;
        }
        if (ends_only_this_player(status)) {
            if (!ownMissionOver_) {
                ownMissionOver_ = true;
                wclog(1, "we %s; leaving the mission", status_name(status));
                NetworkMessage msg;
                msg.set_epoch(epoch_);
                msg.set_frame_number(++frameNumber_);
                msg.mutable_frame()->mutable_player_end()->set_state((GameState)status);
                conn_.send(msg);
            }
            lastWrittenMissionStatus_ = (GameState)status;
            return;  // let our own game end the mission
        }
        // Landing is shared: hold our game in space until the server ends it.
        wr16(ds::missionStatus, 0);
        MissionEnd end;
        populate_mission_end(&end);
        end.set_game_update((GameState)status);
        *pendingFrame_.mutable_frame()->mutable_mission_end() = end;
        wclog(1, "we %s; asking the server to end the mission", status_name(status));
    }

    virtual void on_trampoline_idle() {
        for (size_t i = 0; i < pendingHealth_.size(); i++) {
            const ShipUpdate &su = pendingHealth_[i];
            if (!entities_.is_mapped(su.ship_id())) {
                continue;
            }
            int slot = entities_.net_to_local(su.ship_id());
            if (slot == kPlayerSlot || !slot_in_use(slot)) {
                continue;
            }
            ShipHealthState h;
            if (parse_health(su.health(), &h)) {
                write_health(slot, h);
            }
        }
        pendingHealth_.clear();
    }

    virtual bool in_space() const {
        return within_briefed_mission && !hasRestartedMission_;
    }

    virtual void send_chat(const std::string &text) {
        NetworkMessage msg;
        Chat *chat = msg.mutable_chat();
        chat->set_ship_id(shipNet_);
        chat->set_message(text);
        chat->set_callsign(callsign_);
        if (!conn_.send(msg)) {
            wclog(0, "failed to send chat");
        }
    }

private:
    // Tear the session down at the next safe point; the dispatcher deletes us.
    void drop(const char *why) {
        wclog(0, "%s", why);
        conn_.close();
        dead_ = true;
        g_pendingUninit = true;
    }

    bool send_connect() {
        NetworkMessage msg;
        msg.mutable_connect()->set_callsign(callsign_);
        if (!conn_.send(msg)) {
            return false;
        }
        hasSentConnect_ = true;
        return true;
    }

    void pre_briefing_handshake() {
        hasRestartedMission_ = true;
        NetworkMessage msg;
        msg.mutable_start_briefing_req();
        wclog(1, "asking the server which briefing to load");
        if (!conn_.send(msg)) {
            drop("lost the server while requesting the briefing");
            return;
        }
        while (true) {
            if (!conn_.recv(CAT_GAME, msg).ok()) {
                drop("lost the server while waiting for the briefing");
                return;
            }
            if (msg.has_briefing_start()) {
                break;
            }
        }
        epoch_ = msg.epoch();
        frameNumber_ = msg.frame_number();
        const ServerSendBriefingStart &b = msg.briefing_start();
        load_mission_tree_progress(b.mission_tree_progress());
        wr8(ds::currentMission, (Bit8u)b.mission_id());
        wr8(ds::currentSeries, (Bit8u)b.series_id());
        wclog(1, "briefing for mission %d/%d", b.mission_id(), b.series_id());
    }

    void apply_start_state(const NetworkMessage &msg) {
        const Game &game = msg.game();
        shipNet_ = game.assigned_player_id();
        entities_.set_own_ship(shipNet_);
        if (game.has_starting_state()) {
            apply_frame(game.starting_state(), true);
        }
        epoch_ = msg.epoch();
        frameNumber_ = msg.frame_number();
        isFresh_ = false;
        wclog(1, "we are player %d; epoch %u frame %llu", shipNet_, epoch_, (unsigned long long)frameNumber_);
    }

    void populate_own_update() {
        ShipUpdate *su = update_for(pendingFrame_.mutable_frame(), shipNet_);
        populate_location(su->mutable_loc(), kPlayerSlot);
        health_.next_frame();
        ShipHealthState h;
        if (health_.should_send(kPlayerSlot, &h)) {
            fill_health(su->mutable_health(), h);
        }
        health_.end_frame();
    }

    void apply_frame(const Frame &frame, bool applyOwnLocation) {
        if (frame.has_mission_end()) {
            const MissionEnd &end = frame.mission_end();
            wr16(ds::missionStatus, (Bit16u)end.game_update());
            lastWrittenMissionStatus_ = end.game_update();
            missionTreeProgress_ = end.mission_tree_progress();
            lastVictoryPlusOne_ = end.victory_points_plus_one();
            wclog(1, "server ended the mission: %s", status_name(end.game_update()));
        }
        for (int i = 0; i < frame.update_size(); i++) {
            const ShipUpdate &su = frame.update(i);
            if (!su.has_ship_id() || !entities_.is_mapped(su.ship_id())) {
                continue;
            }
            int slot = entities_.net_to_local(su.ship_id());
            if (is_temp_slot(slot)) {
                continue;
            }
            bool own = (int)su.ship_id() == shipNet_;
            if (su.has_loc() && (!own || applyOwnLocation)) {
                apply_location(su.loc(), slot);
            }
            if (su.has_health() && !own) {
                pendingHealth_.push_back(su);
            }
        }
        for (int i = 0; i < frame.event_size(); i++) {
            enqueue_remote_event(frame.event(i));
        }
    }

    Connection conn_;
    int shipNet_;
    EntityMap entities_;
    std::string callsign_;
    GameState lastWrittenMissionStatus_;
    Bit32u lastVictoryPlusOne_;
    std::string missionTreeProgress_;
    bool isFresh_;
    bool hasRestartedMission_;
    bool hasSentConnect_;
    bool ignoreNextFrameTop_;
    bool ownMissionOver_;
    bool dead_;
    Bit32u epoch_;
    Bit64u frameNumber_;
    NetworkMessage pendingFrame_;
    std::vector<ShipUpdate> pendingHealth_;
    HealthPublisher health_;
};

static void trampoline_idle() {
    if (g_session) {
        g_session->on_trampoline_idle();
    }
}

}  // namespace wc

// ---------------------------------------------------------------------------
// Public API

using namespace wc;

bool init_network() {
    net_config.reset_from_env();
    uninit_network();
    g_trampoline.set_idle_callback(trampoline_idle);
    if (net_config.host && net_config.host[0]) {
        ClientSession *c = new ClientSession();
        if (!c->connect()) {
            wclog(0, "could not connect to %s:%s", net_config.host, net_config.portstr);
            delete c;
            return false;
        }
        g_session = c;
        fprintf(stderr,
                "=========================================================\n"
                "======================== CLIENT =========================\n"
                "=========================================================\n\n");
    } else {
        ServerSession *s = new ServerSession();
        if (!s->listen()) {
            wclog(0, "could not listen on port %s", net_config.portstr);
            delete s;
            return false;
        }
        g_session = s;
        fprintf(stderr,
                "=========================================================\n"
                "=========--------------- SERVER ----------------=========\n"
                "=========================================================\n\n");
    }
    return true;
}

void uninit_network() {
    net_config.reset_from_env();
    if (g_session) {
        Session *s = g_session;
        g_session = NULL;
        delete s;
    }
}

bool is_wc_connected() {
    return g_session != NULL && has_started_up;
}

bool in_space() {
    return g_session != NULL && g_session->in_space();
}

void wcnetSendChatMessage(const std::string &msg) {
    if (g_session) {
        g_session->send_chat(msg);
    }
}
