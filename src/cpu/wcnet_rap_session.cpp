/*
 *  Raptor's session (wcnet_rap.h, docs/raptor.md).
 *
 *  The host is seat 0 (the room's, or the TCP listener); a guest opens a stream to it and says hello, the host
 *  answers with the guest's seat.  Each pilot sends its state (RapState) every frame of a wave, else ten times a
 *  second: where it is (the hangar, the launch, a wave and its frame), its ship and its shots.
 *
 *  The launch: a pilot whose sector is chosen at the ship computer waits there (its game idle) until every pilot of
 *  the room is there too; then all fly the host's wave (its sector, its wave, its difficulty: the guests' set to the
 *  host's before the map is loaded) as one run (the host's serial).  In the wave each machine's frame waits while it
 *  is ahead of another's (a frame or two: the wave's enemies come by the scroll, the same on each machine), the others'
 *  ships and shots are drawn, each pilot's damage to the enemies is told (RapHits: the enemy by its index in the
 *  wave's script) and taken off the same enemy everywhere -- each game kills it, explodes it and drops its bonus for
 *  its own pilot to pick up, the money only the killer's -- and a pilot blown up (RapDead) blows up everybody's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "wcnet_rap.h"
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
    kHelloWaitMs = 20000,
    kPollMs = 10,
    kStateEveryMs = 100,            // out of a wave
    kStaleMs = 1500,                // a report older than this: the pilot not counted (launch, hold, drawing)
    kStartWaitMs = 20000,           // a wave's first frames held at most this long for the others to start
    kMaxShots = 40,
};
const double kFrameMs = 1000.0 * 3 / 70;    // (a frame: 3 of FRAME_COUNT's 70 a second: 23.3 frames a second)

struct Peer {
    Connection conn;
    int player;
    std::string callsign;
    bool hello;
    Peer() : player(-1), hello(false) {}
};

struct Remote {
    bool known;
    std::string callsign;
    double at;
    Bit32u launch, run, frame;
    int game, plrdiff;
    Bit32u diff;
    bool inWave, drawn;
    int x, y, shield;
    Bit32u pic;
    std::vector<Bit32u> shots;
    Remote() : known(false), at(-1e9), launch(0), run(0), frame(0), game(-1), plrdiff(0), diff(0), inWave(false), drawn(false),
               x(0), y(0), shield(0), pic(0) {}
};

struct RapNet {
    bool host;
    int self, seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    Remote remotes[kMaxSeats];
    RapNet() : host(false), self(-1), seats(2), listener(NULL) {}
    ~RapNet() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

RapNet *g_net = NULL;
double g_nextTryMs = 0, g_pollAt = 0, g_stateAt = -1e9, g_noticeAt = -1;
bool g_tried = false;
// the launch
bool g_atLaunch = false;
Bit32u g_launchSerial = 0;      // the host's launches
double g_launchSince = 0;
bool g_go = false;
// the wave flown together
Bit32u g_run = 0;
bool g_inWave = false;
double g_waveStart = 0;
Bit32u g_lastFrameSent = 0xFFFFFFFFu;
std::vector<Bit32u> g_hitsOut;  // (index, damage) pairs not yet sent
bool g_deadIn = false;          // another pilot blown up: ours at the next frame
bool g_deadOut = false;         // ours blown up (told)

void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "Raptor: %s", text.c_str());
}

bool configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') return false;
    if (getenv("WCSOLO") && getenv("WCSOLO")[0] == '1') return false;
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    return env && env[0] ? env : "Pilot";
}

bool start() {
    net_config.reset_from_env();
    RapNet *n = new RapNet();
    n->callsign = own_callsign();
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
        delete n;
        return false;
#endif
    } else {
#ifdef __EMSCRIPTEN__
        wclog(0, "Raptor: the browser build has no TCP transport: set WCROOM (a room code) to play");
        delete n;
        return false;
#endif
        client = net_config.host && net_config.host[0];
        where = client ? std::string(net_config.host) + ":" + net_config.portstr : std::string("port ") + net_config.portstr;
    }
    if (client) {
        Stream *s = NULL;
#ifdef C_LOBBYLINK
        if (g_lobby) s = g_lobby->open_peer(0, kHelloWaitMs);
        else
#endif
            s = tcp_connect(net_config.host, net_config.portstr);
        if (!s) {
            wclog(0, "Raptor: could not reach the host at %s", where.c_str());
            delete n;
            return false;
        }
        Peer *p = new Peer();
        p->conn.adopt(s);
        p->player = 0;
        n->peers.push_back(p);
        NetworkMessage msg;
        msg.mutable_rap_hello()->set_callsign(n->callsign);
        p->conn.send(msg);
    } else {
        Listener *l = NULL;
#ifdef C_LOBBYLINK
        if (g_lobby) l = g_lobby->make_listener();
        else
#endif
            l = tcp_listen(net_config.portstr);
        if (!l) {
            wclog(0, "Raptor: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
    }
    g_net = n;
    wclog(1, "Raptor: %s at %s, %d seats, callsign %s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats, n->callsign.c_str());
    if (n->host) notice("Room open: choose a sector at the ship computer and the others fly the wave with you");
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

template <class Send> void relay(Peer *from, Send send) {
    if (!g_net->host) return;
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *to = g_net->peers[i];
        if (to != from && to->hello && to->conn.is_open()) send(to);
    }
}

void send_all(NetworkMessage &msg, bool bestEffort) {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *to = g_net->peers[i];
        if (!to->hello || !to->conn.is_open()) continue;
        if (bestEffort) to->conn.send_best_effort(msg); else to->conn.send(msg);
    }
}

void take_state(const RapState &st, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) return;
    Remote &r = g_net->remotes[seat];
    const bool wasIn = r.inWave && r.run && PIC_FullIndex() - r.at < kStaleMs;
    r.known = true;
    r.at = PIC_FullIndex();
    r.launch = st.launch();
    r.run = st.run();
    r.frame = st.frame();
    r.game = st.game();
    r.diff = st.diff();
    r.plrdiff = st.plrdiff();
    r.inWave = st.in_wave();
    r.drawn = st.drawn();
    r.x = st.x();
    r.y = st.y();
    r.pic = st.pic();
    r.shield = st.shield();
    r.shots.assign(st.shot().begin(), st.shot().end());
    if (r.inWave && r.run && !wasIn) wclog(1, "Raptor: %s flies run %u (sector %d wave %d)", r.callsign.c_str(), r.run, r.game >> 4, r.game & 15);
}

void handle(Peer *p, NetworkMessage &msg) {
    if (msg.has_rap_hello()) {
        const RapHello &h = msg.rap_hello();
        if (g_net->host) {
            if (p->hello) return;
            const int player = free_player();
            if (player < 0) {
                wclog(1, "Raptor: %s came in with no seat left: turned away", h.callsign().c_str());
                p->conn.close();
                return;
            }
            p->player = player;
            p->callsign = h.callsign();
            p->hello = true;
            g_net->remotes[player] = Remote();
            g_net->remotes[player].callsign = h.callsign();
            NetworkMessage reply;
            reply.mutable_rap_hello()->set_callsign(g_net->callsign);
            reply.mutable_rap_hello()->set_player((Bit32u)player);
            reply.mutable_rap_hello()->set_seats((Bit32u)g_net->seats);
            p->conn.send(reply);
            char line[160];
            snprintf(line, sizeof(line), "%s joined as pilot %d of %d", p->callsign.c_str(), player + 1, g_net->seats);
            notice(line);
        } else {
            p->callsign = h.callsign();
            p->hello = true;
            g_net->self = h.has_player() ? (int)h.player() : 1;
            if (h.has_seats() && h.seats() >= 2) g_net->seats = std::min((int)kMaxSeats, (int)h.seats());
            g_net->remotes[0].callsign = h.callsign();
            char line[160];
            snprintf(line, sizeof(line), "In %s's room as pilot %d of %d", p->callsign.c_str(), g_net->self + 1, g_net->seats);
            notice(line);
        }
    } else if (msg.has_rap_state()) {
        if (!p->hello) return;
        const int seat = g_net->host ? p->player : (msg.rap_state().has_player() ? (int)msg.rap_state().player() : 0);
        if (g_net->host && seat >= 0 && seat < kMaxSeats) g_net->remotes[seat].callsign = p->callsign;
        take_state(msg.rap_state(), seat);
        NetworkMessage out;
        *out.mutable_rap_state() = msg.rap_state();
        out.mutable_rap_state()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send_best_effort(out); });
    } else if (msg.has_rap_hits()) {
        if (!p->hello) return;
        const RapHits &h = msg.rap_hits();
        if (h.run() && h.run() == g_run && g_inWave) {
            std::vector<std::pair<int, int> > hits;
            for (int i = 0; i + 1 < h.hit_size(); i += 2) hits.push_back(std::make_pair((int)h.hit(i), (int)h.hit(i + 1)));
            rap_take_hits(hits);
        }
        NetworkMessage out;
        *out.mutable_rap_hits() = h;
        out.mutable_rap_hits()->set_player((Bit32u)(g_net->host ? p->player : (int)h.player()));
        relay(p, [&](Peer *to) { to->conn.send(out); });
    } else if (msg.has_rap_dead()) {
        if (!p->hello) return;
        const RapDead &d = msg.rap_dead();
        const int seat = g_net->host ? p->player : (int)d.player();
        if (d.run() && d.run() == g_run && g_inWave && !g_deadIn && !g_deadOut) {
            g_deadIn = true;
            const std::string who = seat >= 0 && seat < kMaxSeats ? g_net->remotes[seat].callsign : std::string("a pilot");
            notice(who + " was shot down: the mission is over for everybody");
        }
        NetworkMessage out;
        *out.mutable_rap_dead() = d;
        out.mutable_rap_dead()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send(out); });
    }
}

void poll_peers() {
    if (g_net->host && g_net->listener) {
        for (int n = 0; n < 4; n++) {
            Stream *s = g_net->listener->accept(false);
            if (!s) break;
            Peer *p = new Peer();
            p->conn.adopt(s);
            g_net->peers.push_back(p);
            wclog(1, "Raptor: a guest at %s", s->describe().c_str());
        }
    }
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

// the pilots with us in the room (said hello)
int others() {
    if (!g_net) return 0;
    int n = 0;
    for (size_t i = 0; i < g_net->peers.size(); i++) n += g_net->peers[i]->hello && g_net->peers[i]->conn.is_open();
    return g_net->host ? n : (n ? std::max(1, n) : 0);
}

bool fresh(const Remote &r) { return r.known && PIC_FullIndex() - r.at < kStaleMs; }

void send_state(double now) {
    RapLocal l;
    rap_local(&l);
    NetworkMessage msg;
    RapState *st = msg.mutable_rap_state();
    st->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    st->set_launch(g_atLaunch ? (g_net->host ? g_launchSerial : 1) : 0);
    st->set_game(l.game >= 0 ? l.game * 16 + l.wave : -1);
    st->set_diff(l.diff);
    st->set_plrdiff(l.plrdiff);
    st->set_run(g_run);
    st->set_in_wave(l.inWave);
    st->set_frame(l.frame);
    st->set_x(l.x);
    st->set_y(l.y);
    st->set_pic(l.pic);
    st->set_drawn(l.drawn);
    st->set_shield(l.shield);
    if (l.inWave && g_run) {
        std::vector<Bit32u> shots;
        rap_shots(shots, kMaxShots);
        for (size_t i = 0; i < shots.size(); i++) st->add_shot(shots[i]);
    }
    send_all(msg, true);
    g_stateAt = now;
}

// Is the launch over (every pilot there, or gone)?  The guests take the host's wave then.
void launch_think(double now) {
    if (!g_atLaunch || g_go) return;
    if (!others()) {
        g_go = true;
        g_run = 0;
        notice("Alone in the room: flying this wave alone");
        return;
    }
    if (g_net->host) {
        bool all = true;
        for (size_t i = 0; i < g_net->peers.size(); i++) {
            Peer *p = g_net->peers[i];
            if (!p->hello || !p->conn.is_open() || p->player < 0) continue;
            const Remote &r = g_net->remotes[p->player];
            // (at the launch, or already gone on with this launch: its run ours)
            if (!fresh(r) || !(r.launch || r.run == g_launchSerial)) all = false;
        }
        if (all) {
            g_go = true;
            g_run = g_launchSerial;
            wclog(1, "Raptor: everybody at the launch: run %u", g_run);
        } else if (now - g_launchSince > 1000 && now - g_noticeAt > 4000) {
            notice("Waiting for the others to choose a sector at the ship computer");
        }
    } else {
        const Remote &h = g_net->remotes[0];
        // (the host at the launch, or gone on with it a moment ago)
        const bool hostLaunching = fresh(h) && h.launch && h.game >= 0;
        const bool hostJustGone = fresh(h) && !h.launch && h.run && h.run != g_run && h.game >= 0 &&
                                  (!h.inWave || h.frame < 140);
        if (hostLaunching || hostJustGone) {
            g_go = true;
            g_run = hostLaunching ? h.launch : h.run;
            rap_set_wave(h.game >> 4, h.game & 15, h.diff, h.plrdiff);
            wclog(1, "Raptor: the host's wave: sector %d wave %d, difficulty %d (sprites %02x): run %u", h.game >> 4, h.game & 15,
                  h.plrdiff, h.diff, g_run);
        } else if (now - g_launchSince > 1000 && now - g_noticeAt > 4000) {
            notice("Waiting for " + h.callsign + " (the host) to choose a sector at the ship computer");
        }
    }
}

}  // namespace

void rap_session_tick() {
    const double now = PIC_FullIndex();
    if (!g_net) {
        if (g_tried && now < g_nextTryMs) return;
        g_tried = true;
        g_nextTryMs = now + 10000;
        if (!configured() || !start()) return;
    }
    if (now - g_pollAt >= kPollMs) {
        g_pollAt = now;
        poll_peers();
        if (!g_net->host && g_net->peers.empty()) {
            rap_session_shutdown();     // (the host gone: tried again later, as at the start)
            return;
        }
        launch_think(now);
    }
    // our state: every frame of a wave, else ten times a second
    RapLocal l;
    rap_local(&l);
    if ((l.inWave && l.frame != g_lastFrameSent) || now - g_stateAt >= kStateEveryMs) {
        g_lastFrameSent = l.frame;
        send_state(now);
    }
    if (!g_hitsOut.empty()) {
        NetworkMessage msg;
        RapHits *h = msg.mutable_rap_hits();
        h->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
        h->set_run(g_run);
        for (size_t i = 0; i < g_hitsOut.size(); i++) h->add_hit(g_hitsOut[i]);
        g_hitsOut.clear();
        send_all(msg, false);
    }
    if (g_deadIn && g_inWave) rap_blow_up();
    if (g_noticeAt > 0 && now - g_noticeAt > 6000) {
        incoming_text.clear();
        g_noticeAt = -1;
    }
}

void rap_session_shutdown() {
    if (!g_net) return;
    wclog(1, "Raptor: session closed");
    delete g_net;
    g_net = NULL;
    g_tried = false;
    g_atLaunch = g_go = g_inWave = false;
    g_run = 0;
}

bool rap_session_active() { return others() > 0; }

bool rap_session_launch_arrive() {
    if (!g_net || !others()) {
        g_run = 0;
        return false;
    }
    g_atLaunch = true;
    g_go = false;
    g_launchSince = PIC_FullIndex();
    if (g_net->host) g_launchSerial++;
    send_state(PIC_FullIndex());
    launch_think(PIC_FullIndex());
    if (g_go) {
        g_atLaunch = false;
        return false;
    }
    return true;
}

bool rap_session_launch_go() {
    if (!g_go) return false;
    g_atLaunch = false;
    incoming_text.clear();
    return true;
}

void rap_session_wave_start() {
    g_inWave = true;
    g_deadIn = g_deadOut = false;
    g_waveStart = PIC_FullIndex();
    g_hitsOut.clear();
    if (g_run) wclog(1, "Raptor: the wave begins together (run %u)", g_run);
}

void rap_session_wave_end() {
    g_inWave = false;
    g_deadIn = false;
    if (g_run) wclog(1, "Raptor: our wave is over (run %u)", g_run);
    g_run = 0;
    if (g_net) send_state(PIC_FullIndex());
}

Bit32u rap_session_run() { return g_net ? g_run : 0; }

bool rap_session_hold_frame(Bit32u frame) {
    if (!g_net || !g_run) return false;
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self) continue;
        const Remote &r = g_net->remotes[s];
        if (!fresh(r) || r.run != g_run) continue;
        if (!r.inWave) {
            // (not in the wave yet: loading it; held at our start for it a while)
            if (frame <= 2 && now - g_waveStart < kStartWaitMs) return true;
            continue;
        }
        const double est = r.frame + std::min(now - r.at, 500.0) / kFrameMs;
        if ((double)frame > est + rap_wave_hold_frames()) return true;
    }
    return false;
}

void rap_session_damage(int index, int damage) {
    if (g_hitsOut.size() < 1024) {
        g_hitsOut.push_back((Bit32u)index);
        g_hitsOut.push_back((Bit32u)damage);
    }
}

void rap_session_dead() {
    if (!g_net || !g_run) return;
    NetworkMessage msg;
    msg.mutable_rap_dead()->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    msg.mutable_rap_dead()->set_run(g_run);
    send_all(msg, false);
    g_deadOut = true;
    wclog(1, "Raptor: our ship blown up: the mission is over for everybody (run %u)", g_run);
}

void rap_session_draw() {
    if (!g_net || !g_run) return;
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self) continue;
        const Remote &r = g_net->remotes[s];
        if (!fresh(r) || r.run != g_run || !r.inWave) continue;
        rap_draw_shots(r.shots, r.x, r.y);
        if (r.drawn) rap_draw_ship(r.pic, r.x, r.y);
    }
}

}  // namespace wc
