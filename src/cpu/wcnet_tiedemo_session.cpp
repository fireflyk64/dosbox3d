/*
 *  The TIE Fighter demo's room (wcnet_tiedemo.h, docs/tiedemo.md).
 *
 *  The host is seat 0 (the room's, or the TCP listener); a guest opens a stream to it and says hello (TdmHello), the
 *  host answers with the guest's seat.  Each pilot flies the mission's player craft on its own machine (T/F Alpha's)
 *  and tells its craft (TdmState) every frame of its flight, else twice a second; the host relays a guest's to the
 *  other guests.  On every machine the others are put in T/F Alpha's other craft, the wingmen, in seat order: each
 *  frame, after the game has moved everything and before the picture, a wingman is given the pilot's last record,
 *  carried on by its own motion for the frames since it was told.  Nothing else of the mission is shared yet: each
 *  machine's enemies are its own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "wcnet_tiedemo.h"
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
    kMaxSeats = 3,                  // (T/F Alpha: three craft)
    kHelloWaitMs = 20000,
    kPollMs = 10,
    kStateEveryMs = 500,            // out of the flight
    kStaleMs = 1500,                // a report older than this: the pilot not put in
};

struct Peer {
    Connection conn;
    int player;
    std::string callsign;
    bool hello;
    Peer() : player(-1), hello(false) {}
};

struct Remote {
    bool known, inFlight;
    std::string callsign, rec;
    double at;
    Bit32u frame;
    bool shown;
    Remote() : known(false), inFlight(false), at(-1e9), frame(0), shown(false) {}
};

struct TdmNet {
    bool host;
    int self, seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    Remote remotes[kMaxSeats];
    TdmNet() : host(false), self(-1), seats(2), listener(NULL) {}
    ~TdmNet() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

TdmNet *g_net = NULL;
double g_nextTryMs = 0, g_pollAt = 0, g_stateAt = -1e9, g_noticeAt = -1, g_reportAt = 0;
bool g_tried = false;
unsigned g_put = 0, g_sent = 0;
bool g_placed = false;          // (this flight: a guest has taken its own place in the flight group)
Bit32u g_lastFrame = 0;

// The wingman another pilot is put in: a guest (seat m >= 1) took the place of wingman m - 1 at the start, so that
// wingman is where the host (seat 0) began; any other seat s is wingman s - 1, where it began.
int wingman_of(int seat) {
    const int m = g_net->self < 0 ? 0 : g_net->self;
    return seat == 0 ? m - 1 : seat - 1;
}

void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "TIE demo: %s", text.c_str());
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
    TdmNet *n = new TdmNet();
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
        wclog(0, "TIE demo: the browser build has no TCP transport: set WCROOM (a room code) to play");
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
            wclog(0, "TIE demo: could not reach the host at %s", where.c_str());
            delete n;
            return false;
        }
        Peer *p = new Peer();
        p->conn.adopt(s);
        p->player = 0;
        n->peers.push_back(p);
        NetworkMessage msg;
        msg.mutable_tdm_hello()->set_callsign(n->callsign);
        p->conn.send(msg);
    } else {
        Listener *l = NULL;
#ifdef C_LOBBYLINK
        if (g_lobby) l = g_lobby->make_listener();
        else
#endif
            l = tcp_listen(net_config.portstr);
        if (!l) {
            wclog(0, "TIE demo: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
    }
    g_net = n;
    wclog(1, "TIE demo: %s at %s, %d seats, callsign %s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats, n->callsign.c_str());
    if (n->host) notice("Room open: the others fly the mission as your wingmen in T/F Alpha");
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

void take_state(const TdmState &st, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) return;
    Remote &r = g_net->remotes[seat];
    const bool was = r.inFlight && PIC_FullIndex() - r.at < kStaleMs;
    r.known = true;
    r.at = PIC_FullIndex();
    r.inFlight = st.in_flight();
    r.frame = st.frame();
    r.rec = st.rec();
    if (r.inFlight && !was) notice(r.callsign + " is flying: a wingman of yours in T/F Alpha");
    if (!r.inFlight && was) notice(r.callsign + "'s flight is over");
}

void handle(Peer *p, NetworkMessage &msg) {
    if (msg.has_tdm_hello()) {
        const TdmHello &h = msg.tdm_hello();
        if (g_net->host) {
            if (p->hello) return;
            const int player = free_player();
            if (player < 0) {
                wclog(1, "TIE demo: %s came in with no seat left: turned away", h.callsign().c_str());
                p->conn.close();
                return;
            }
            p->player = player;
            p->callsign = h.callsign();
            p->hello = true;
            g_net->remotes[player] = Remote();
            g_net->remotes[player].callsign = h.callsign();
            NetworkMessage reply;
            reply.mutable_tdm_hello()->set_callsign(g_net->callsign);
            reply.mutable_tdm_hello()->set_player((Bit32u)player);
            reply.mutable_tdm_hello()->set_seats((Bit32u)g_net->seats);
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
    } else if (msg.has_tdm_state()) {
        if (!p->hello) return;
        const int seat = g_net->host ? p->player : (msg.tdm_state().has_player() ? (int)msg.tdm_state().player() : 0);
        if (g_net->host && seat >= 0 && seat < kMaxSeats) g_net->remotes[seat].callsign = p->callsign;
        take_state(msg.tdm_state(), seat);
        NetworkMessage out;
        *out.mutable_tdm_state() = msg.tdm_state();
        out.mutable_tdm_state()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send_best_effort(out); });
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
            wclog(1, "TIE demo: a guest at %s", s->describe().c_str());
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

bool fresh(const Remote &r) { return r.known && PIC_FullIndex() - r.at < kStaleMs; }

void send_state(const TdmLocal &l) {
    NetworkMessage msg;
    TdmState *st = msg.mutable_tdm_state();
    st->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    st->set_in_flight(l.inFlight);
    st->set_frame(l.frame);
    if (l.inFlight) st->set_rec(l.rec);
    send_all(msg, true);
    g_stateAt = PIC_FullIndex();
    g_sent++;
}

}  // namespace

void tdm_session_tick() {
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
            tdm_session_shutdown();     // (the host gone: tried again later, as at the start)
            return;
        }
    }
    TdmLocal l;
    tdm_local(&l);
    if (!l.inFlight && now - g_stateAt >= kStateEveryMs) send_state(l);
    if (now >= g_reportAt) {
        g_reportAt = now + 1000;
        std::string who;
        for (int s = 0; s < kMaxSeats; s++) {
            const Remote &r = g_net->remotes[s];
            if (s == g_net->self || !r.known) continue;
            char b[160];
            snprintf(b, sizeof(b), " %s%s (frame %u, %.0f ms ago)", r.callsign.c_str(), r.inFlight ? " flying" : "", r.frame, now - r.at);
            who += b;
        }
        wclog(2, "TIE demo: %.1fs: %s, our frame %u (%.1f ms a frame); %u states sent, %u wingmen put;%s", now / 1000.0,
              l.inFlight ? "flying" : "not flying", l.frame, tdm_frame_ms(), g_sent, g_put, who.c_str());
        g_sent = g_put = 0;
    }
    if (g_noticeAt > 0 && now - g_noticeAt > 6000) {
        incoming_text.clear();
        g_noticeAt = -1;
    }
}

void tdm_session_frame() {
    if (!g_net) return;
    TdmLocal l;
    if (!tdm_local(&l)) return;
    if (l.frame < g_lastFrame) g_placed = false;        // (a new flight)
    g_lastFrame = l.frame;
    if (!g_placed && g_net->self >= 0) {
        // a guest in its own place in the flight group, once a flight (the host flies the mission's own craft)
        g_placed = true;
        if (g_net->self >= 1 && !tdm_take_place(g_net->self - 1)) wclog(1, "TIE demo: no craft of our flight group to take the place of");
    }
    send_state(l);
    // the others, as our flight group's wingmen (wingman_of)
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self) continue;
        Remote &r = g_net->remotes[s];
        if (!fresh(r) || !r.inFlight || r.rec.empty()) continue;
        if (tdm_put_pilot(wingman_of(s), r.rec, (now - r.at) / tdm_frame_ms())) g_put++;
    }
}

void tdm_session_shutdown() {
    if (!g_net) return;
    wclog(1, "TIE demo: session closed");
    delete g_net;
    g_net = NULL;
    g_tried = false;
}

}  // namespace wc
