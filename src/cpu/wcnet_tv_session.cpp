/*
 *  Terminal Velocity's session (wcnet_tv.h, docs/terminalvelocity.md section 5).
 *
 *  Everybody in the room flies the same campaign level together.  The host is seat 0 (the room's, or the TCP
 *  listener); a guest opens a stream to it and says hello, the host answers with the guest's seat.  Each pilot sends
 *  its ship (TvState: place, angles, their rates, shield) some 15 times a second while it flies; the others draw it,
 *  carried on between reports by its rates, with the game's own call that puts a model in the frame's draw list
 *  (where the game's net mode draws its other players), as the model of the pilot's choice (its `look`: the TV-202,
 *  the Ymir IceRider or SnowScout).  A pilot's shots are the game's fire call's arguments (TvShot), made again on the
 *  others' machines as shots of their own player's: they fly and hit there as they do here.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "wcnet_tv.h"
#include "wcnet_tv_sig.h"
#include "wcnet_transport.h"
#include "wcnet_lobby.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "net_config.h"
#include "pic.h"

extern std::string incoming_text;   // the line the emulator draws over the picture (sdlmain.cpp)

namespace wc {

namespace {

enum {
    kMaxSeats = 4,
    kRetryMs = 10000,
    kHelloWaitMs = 20000,
    kPollMs = 15,                   // the sockets looked at this often (emulated ms)
    kStateEveryMs = 66,             // our ship, some 15 times a second
    kStateStaleMs = 3000,           // a ship whose reports stopped this long is not drawn
    kCarryMs = 1000,                // a ship carried on by its rates at most this long past its report
    kNoticeMs = 8000,
};

// The models a pilot's ship may be drawn as (the page's Ship choice): the game's own files.  Only these are ever
// loaded (a model the game cannot find ends the program).
const char *const kLooks[] = { "ship.bin", "icebrd.bin", "snowftr.bin" };
const char *const kLookNames[] = { "TV-202", "Ymir IceRider", "Ymir SnowScout" };
const int kLookCount = 3;

int look_index(const std::string &look) {
    for (int i = 0; i < kLookCount; i++) {
        if (look == kLooks[i]) return i;
    }
    return 0;
}

struct Peer {
    Connection conn;
    int player;                 // the seat it flies (the host's guests: 1..; a guest's one peer: the host, 0)
    std::string callsign;
    bool hello;
    Peer() : player(-1), hello(false) {}
};

// A pilot's ship as its reports say.
struct Remote {
    bool known;
    std::string callsign, look;
    double at;                  // the last report (emulated ms)
    bool flying;
    int level;
    Bit32s pos[6], rate[6];
    Bit32s shield;
    Remote() : known(false), look("ship.bin"), at(-1e9), flying(false), level(-1), shield(0) {
        memset(pos, 0, sizeof(pos));
        memset(rate, 0, sizeof(rate));
    }
};

struct TvNet {
    bool host;
    int self;
    int seats;
    std::string callsign, look;
    Listener *listener;
    std::vector<Peer *> peers;
    Remote remotes[kMaxSeats];
    double stateAt;
    // the models loaded for this level (by look; 0: not yet), the level they belong to
    Bit32u models[kLookCount];
    bool loading[kLookCount];
    Bit32u modelsFrame;         // the frame count at the level's start (a new level: loaded again)
    TvNet() : host(false), self(-1), seats(2), listener(NULL), stateAt(-1e9), modelsFrame(0) {
        memset(models, 0, sizeof(models));
        memset(loading, 0, sizeof(loading));
    }
    ~TvNet() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

TvNet *g_net;
double g_nextTryMs;
double g_noticeAt = -1;
std::vector<std::pair<int, TvShot> > g_shots;   // others' shots, made at the next frame (seat, shot)
unsigned g_shotsMade = 0, g_shotsSent = 0, g_drawn = 0;

void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "Terminal Velocity: %s", text.c_str());
}

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    return env && env[0] ? env : "Pilot";
}

std::string own_look() {
    const char *env = getenv("WCTVLOOK");
    std::string l = env && env[0] ? env : "ship.bin";
    for (size_t i = 0; i < l.size(); i++) l[i] = (char)tolower((unsigned char)l[i]);
    if (l == "icerider") l = "icebrd.bin";
    if (l == "snowscout") l = "snowftr.bin";
    if (l == "tv-202" || l == "tv202") l = "ship.bin";
    return kLooks[look_index(l)];
}

bool configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') return false;
    if (getenv("WCSOLO") && getenv("WCSOLO")[0] == '1') return false;
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

bool start() {
    net_config.reset_from_env();
    TvNet *n = new TvNet();
    n->callsign = own_callsign();
    n->look = own_look();
    n->seats = net_config.lobby_players > 0 ? std::min((int)kMaxSeats, net_config.lobby_players) : 2;
    std::string where;
    bool client;
    if (net_config.use_lobby()) {
#ifdef C_LOBBYLINK
        if (!g_lobby) g_lobby = LobbyHub::join(net_config);
        if (!g_lobby) {
            delete n;
            return false;
        }
        client = !g_lobby->is_host();
        if (g_lobby->max_players() > 0) n->seats = std::min((int)kMaxSeats, g_lobby->max_players());
        where = "room " + g_lobby->code();
#else
        wclog(0, "WCROOM is set but this build has no lobbylink support");
        delete n;
        return false;
#endif
    } else {
#ifdef __EMSCRIPTEN__
        wclog(0, "the browser build has no TCP transport: set WCROOM (a room code) to play");
        delete n;
        return false;
#endif
        client = net_config.host && net_config.host[0];
        where = client ? std::string(net_config.host) + ":" + net_config.portstr : std::string("port ") + net_config.portstr;
    }
    if (client) {
        Stream *s = NULL;
#ifdef C_LOBBYLINK
        if (g_lobby) {
            s = g_lobby->open_peer(0, kHelloWaitMs);
        } else
#endif
        {
            s = tcp_connect(net_config.host, net_config.portstr);
        }
        if (!s) {
            wclog(0, "Terminal Velocity: could not reach the host at %s", where.c_str());
            delete n;
            return false;
        }
        Peer *p = new Peer();
        p->conn.adopt(s);
        p->player = 0;
        n->peers.push_back(p);
        NetworkMessage msg;
        msg.mutable_tv_hello()->set_callsign(n->callsign);
        msg.mutable_tv_hello()->set_look(n->look);
        p->conn.send(msg);
    } else {
        Listener *l = NULL;
#ifdef C_LOBBYLINK
        if (g_lobby) {
            l = g_lobby->make_listener();
        } else
#endif
        {
            l = tcp_listen(net_config.portstr);
        }
        if (!l) {
            wclog(0, "Terminal Velocity: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
    }
    g_net = n;
    wclog(1, "Terminal Velocity: %s at %s, %d seats, callsign %s, flying as a %s", n->host ? "HOST" : "GUEST", where.c_str(),
          n->seats, n->callsign.c_str(), kLookNames[look_index(n->look)]);
    if (n->host) notice("Room open: fly a level and the others fly it with you");
    return true;
}

int free_player() {
    for (int p = 1; p < g_net->seats; p++) {
        bool taken = false;
        for (size_t i = 0; i < g_net->peers.size() && !taken; i++) taken = g_net->peers[i]->player == p;
        if (!taken) return p;
    }
    return -1;
}

// (the host: a message of a guest's to the other guests)
template <class Send> void relay(Peer *from, Send send) {
    if (!g_net->host) return;
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *to = g_net->peers[i];
        if (to != from && to->hello && to->conn.is_open()) send(to);
    }
}

void take_state(const TvState &st, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) return;
    Remote &r = g_net->remotes[seat];
    const bool wasFlying = r.flying && PIC_FullIndex() - r.at < kStateStaleMs;
    r.known = true;
    r.at = PIC_FullIndex();
    r.flying = st.flying();
    r.level = st.level();
    if (st.has_look()) r.look = kLooks[look_index(st.look())];
    for (int i = 0; i < 6; i++) {
        r.pos[i] = i < st.pos_size() ? st.pos(i) : 0;
        r.rate[i] = i < st.rate_size() ? st.rate(i) : 0;
    }
    r.shield = st.shield();
    if (r.flying && !wasFlying) {
        wclog(1, "Terminal Velocity: %s flies level %d (%s)", r.callsign.c_str(), r.level, st.level_name().c_str());
    }
}

void handle(Peer *p, NetworkMessage &msg) {
    if (msg.has_tv_hello()) {
        const TvHello &h = msg.tv_hello();
        if (g_net->host) {
            if (p->hello) return;
            const int player = free_player();
            if (player < 0) {
                wclog(1, "Terminal Velocity: %s came in with no seat left: turned away", h.callsign().c_str());
                p->conn.close();
                return;
            }
            p->player = player;
            p->callsign = h.callsign();
            p->hello = true;
            g_net->remotes[player].callsign = h.callsign();
            g_net->remotes[player].look = kLooks[look_index(h.look())];
            NetworkMessage reply;
            reply.mutable_tv_hello()->set_callsign(g_net->callsign);
            reply.mutable_tv_hello()->set_player((Bit32u)player);
            reply.mutable_tv_hello()->set_seats((Bit32u)g_net->seats);
            reply.mutable_tv_hello()->set_look(g_net->look);
            p->conn.send(reply);
            char line[160];
            snprintf(line, sizeof(line), "%s joined as pilot %d of %d (in a %s)", p->callsign.c_str(), player + 1, g_net->seats,
                     kLookNames[look_index(h.look())]);
            notice(line);
        } else {
            p->callsign = h.callsign();
            p->hello = true;
            g_net->self = h.has_player() ? (int)h.player() : 1;
            if (h.has_seats() && h.seats() >= 2) g_net->seats = std::min((int)kMaxSeats, (int)h.seats());
            g_net->remotes[0].callsign = h.callsign();
            g_net->remotes[0].look = kLooks[look_index(h.look())];
            char line[160];
            snprintf(line, sizeof(line), "In %s's room as pilot %d of %d", p->callsign.c_str(), g_net->self + 1, g_net->seats);
            notice(line);
        }
    } else if (msg.has_tv_state()) {
        if (!p->hello) return;
        const int seat = g_net->host ? p->player : (msg.tv_state().has_player() ? (int)msg.tv_state().player() : 0);
        if (g_net->host && seat >= 0 && seat < kMaxSeats) g_net->remotes[seat].callsign = p->callsign;
        take_state(msg.tv_state(), seat);
        NetworkMessage out;
        *out.mutable_tv_state() = msg.tv_state();
        out.mutable_tv_state()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send_best_effort(out); });
    } else if (msg.has_tv_shot()) {
        if (!p->hello) return;
        const int seat = g_net->host ? p->player : (msg.tv_shot().has_player() ? (int)msg.tv_shot().player() : 0);
        if (g_shots.size() < 64) g_shots.push_back(std::make_pair(seat, msg.tv_shot()));   // (a list has a cap)
        NetworkMessage out;
        *out.mutable_tv_shot() = msg.tv_shot();
        out.mutable_tv_shot()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send(out); });
    }
}

void poll_peers() {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        NetworkMessage msg;
        for (int n = 0; n < 64; n++) {
            RecvStatus st = p->conn.poll(CAT_GAME, msg);
            if (!st.ok()) {
                if (st.failed()) p->conn.close();
                break;
            }
            handle(p, msg);
        }
        for (int n = 0; n < 64 && p->conn.is_open(); n++) {
            RecvStatus st = p->conn.poll_best_effort(msg);
            if (!st.ok()) break;
            handle(p, msg);
        }
    }
    for (size_t i = 0; i < g_net->peers.size();) {
        Peer *p = g_net->peers[i];
        if (p->conn.is_open()) {
            i++;
            continue;
        }
        if (p->hello) {
            notice(p->callsign + (g_net->host ? " left the room" : " (the host) is gone: the room is closed"));
            if (p->player >= 0 && p->player < kMaxSeats) g_net->remotes[p->player] = Remote();
        }
        delete p;
        g_net->peers.erase(g_net->peers.begin() + i);
    }
}

void send_all(NetworkMessage &msg, bool bestEffort) {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *to = g_net->peers[i];
        if (!to->hello || !to->conn.is_open()) continue;
        if (bestEffort) to->conn.send_best_effort(msg); else to->conn.send(msg);
    }
}

void send_state(double now) {
    TvShip s;
    const bool flying = tv_player(&s);
    NetworkMessage msg;
    TvState *st = msg.mutable_tv_state();
    st->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    st->set_level(tv_level());
    st->set_flying(flying);
    for (int i = 0; i < 3; i++) st->add_pos(s.pos[i]);
    for (int i = 0; i < 3; i++) st->add_pos(s.ang[i]);
    for (int i = 0; i < 3; i++) st->add_rate(s.vel[i]);
    for (int i = 0; i < 3; i++) st->add_rate(s.spin[i]);
    st->set_shield(s.shield);
    st->set_look(g_net->look);
    static int lastLevel = -2;
    if (tv_level() != lastLevel || (int)(now / 1000) % 5 == 0) {
        st->set_level_name(tv_level_name());
        lastLevel = tv_level();
    }
    send_all(msg, true);
}

void shutdown() {
    if (!g_net) return;
    wclog(1, "Terminal Velocity: session closed");
    delete g_net;
    g_net = NULL;
    g_shots.clear();
#ifdef C_LOBBYLINK
    if (g_lobby) {
        LobbyHub *l = g_lobby;
        g_lobby = NULL;
        delete l;
    }
#endif
}

void model_loaded(int look, Bit32u eax) {
    if (!g_net || look < 0 || look >= kLookCount) return;
    g_net->models[look] = eax;
    g_net->loading[look] = false;
    wclog(1, "Terminal Velocity: the %s's model (%s) loaded: %08x", kLookNames[look], kLooks[look], (unsigned)eax);
}

// The model a look is drawn with in this level: the player's own ship's is the game's (ship.bin, loaded with the
// level); another is loaded once a level, by the game's loader, at a frame's top.
Bit32u model_for(int look) {
    if (look == 0) return tv_r32(tv_data(tvsig::D_SHIP_MODEL));
    if (g_net->models[look]) return g_net->models[look];
    if (!g_net->loading[look]) {
        const std::string name = kLooks[look];
        std::vector<Bit32u> scratch((name.size() + 4) / 4, 0);
        memcpy(&scratch[0], name.c_str(), name.size());
        std::vector<Bit32u> args(1, TV_SCRATCH);
        g_net->loading[look] = true;
        tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_LOAD_MODEL), args, scratch, model_loaded, look);
    }
    return 0;
}

void shot_made(int seat, Bit32u eax) {
    (void)eax;
    (void)seat;
    g_shotsMade++;
}

}  // namespace

void tv_session_tick() {
    const double now = PIC_FullIndex();
    if (g_noticeAt >= 0 && now - g_noticeAt > kNoticeMs) {
        g_noticeAt = -1;
        incoming_text = "";
    }
    if (!tv_found()) {
        shutdown();
        return;
    }
    if (!g_net) {
        if (!configured() || now < g_nextTryMs) return;
        g_nextTryMs = now + kRetryMs;
        start();
        return;
    }
    static double polledAt = -1e9;
    if (now - polledAt < kPollMs && now >= polledAt) return;
    polledAt = now;
    if (g_net->host && g_net->listener) {
        for (int n = 0; n < 4; n++) {
            Stream *s = g_net->listener->accept(false);
            if (!s) break;
            Peer *p = new Peer();
            p->conn.adopt(s);
            g_net->peers.push_back(p);
            wclog(1, "Terminal Velocity: a guest at %s", s->describe().c_str());
        }
    }
    poll_peers();
    if (!g_net->host && g_net->peers.empty()) {
        shutdown();     // (the host gone: tried again later, as at the start)
        return;
    }
    if (now - g_net->stateAt >= kStateEveryMs) {
        g_net->stateAt = now;
        send_state(now);
    }
    static double reportAt = 0;
    if (now - reportAt >= 5000) {
        reportAt = now;
        for (int s = 0; s < kMaxSeats; s++) {
            const Remote &r = g_net->remotes[s];
            if (s == g_net->self || !r.known) continue;
            wclog(2, "Terminal Velocity: seat %d %s: %s level %d at %d %d %d, %.0f ms old; shots made %u sent %u, drawn %u",
                  s, r.callsign.c_str(), r.flying ? "flying" : "not flying", r.level, r.pos[0], r.pos[1], r.pos[2], now - r.at,
                  g_shotsMade, g_shotsSent, g_drawn);
        }
    }
}

void tv_session_frame() {
    if (!g_net) return;
    // a new level: its models are the game's to free; ours loaded again
    const int level = tv_level();
    static int modelsLevel = -2;
    static Bit32u lastFrames = 0;
    if (level != modelsLevel || tv_frames() < lastFrames) {
        modelsLevel = level;
        memset(g_net->models, 0, sizeof(g_net->models));
        memset(g_net->loading, 0, sizeof(g_net->loading));
    }
    lastFrames = tv_frames();
    // the others' shots, made as our own player's (they fly and hit here as there)
    for (size_t i = 0; i < g_shots.size(); i++) {
        const TvShot &sh = g_shots[i].second;
        if (sh.posmat_size() != 16) continue;
        std::vector<Bit32u> scratch(16);
        for (int k = 0; k < 16; k++) scratch[k] = (Bit32u)sh.posmat(k);
        std::vector<Bit32u> args;
        args.push_back(TV_SCRATCH);
        args.push_back(sh.weapon());
        args.push_back((Bit32u)sh.damage());
        args.push_back(0);
        args.push_back(sh.kind());
        tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_FIRE), args, scratch, shot_made, g_shots[i].first);
    }
    g_shots.clear();
}

void tv_session_draw() {
    if (!g_net || !tv_in_flight()) return;
    const double now = PIC_FullIndex();
    const int level = tv_level();
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == g_net->self || !r.known || !r.flying || r.level != level || now - r.at > kStateStaleMs) continue;
        const int look = look_index(r.look);
        const Bit32u model = model_for(look);
        if (!model) continue;
        const double dt = std::min(now - r.at, (double)kCarryMs) / 1000.0;
        std::vector<Bit32u> scratch(6);
        for (int k = 0; k < 6; k++) scratch[k] = (Bit32u)(r.pos[k] + (Bit32s)(r.rate[k] * dt));
        std::vector<Bit32u> args;
        args.push_back(TV_SCRATCH);
        args.push_back(model);
        args.push_back(0x10000);
        tv_queue_call(TV_AT_DRAW, tv_fn(TV_FN_DRAW_MODEL), args, scratch, NULL, s);
        g_drawn++;
    }
}

bool tv_session_puppet(int obj) {
    (void)obj;
    return false;
}

void tv_session_fired(const Bit32u posmat[16], Bit32u weapon, Bit32u damage, Bit32u kind) {
    if (!g_net) return;
    NetworkMessage msg;
    TvShot *sh = msg.mutable_tv_shot();
    sh->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    for (int i = 0; i < 16; i++) sh->add_posmat((Bit32s)posmat[i]);
    sh->set_weapon(weapon);
    sh->set_damage((Bit32s)damage);
    sh->set_kind(kind);
    send_all(msg, false);
    g_shotsSent++;
}

void tv_session_hit(int obj, Bit32s damage) {
    (void)obj;
    (void)damage;
}

void tv_session_level(int index, const std::string &name) {
    (void)index;
    (void)name;
}

void tv_session_shutdown() { shutdown(); }

bool tv_session_active() { return g_net != NULL; }

}  // namespace wc
