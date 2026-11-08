/*
 *  Privateer's session (wcnet_priv.h, docs/privateer.md section 6, step 3: presence).
 *
 *  Privateer is flown together loosely (the user, 2026-11-07: "players need to be able to go wherever"): every
 *  machine runs its own game, its own career and its own saved games, and the room only says where everybody is.
 *  The host is player 0 (the room's, or the TCP listener); a guest opens a stream to it and says hello, the host
 *  answers with the guest's seat and the room's seats (up to 8).  Each pilot sends where it is (PrivPresence: flying
 *  or not, its system's name, its place) when it engages the autopilot (A), jumps, launches or lands, and every 15 s
 *  otherwise (the user: "whenever a player hits a or otherwise every 15 seconds players will broadcast their system
 *  and coordinates"); the host relays a guest's to the other guests.  The overlay says who is where: a pilot in the
 *  same system with its distance in the game's metres (as the destination display gives a range: the world's units
 *  over 256), the others by their system.  What comes next (step 4 on): the pilots near each other in each other's
 *  space, the nav points' owners, shots, cargo, the Steltek drone.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "wcnet_priv.h"
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
    kMaxSeats = 8,
    kRetryMs = 10000,
    kHelloWaitMs = 20000,
    kPollMs = 20,                   // the sockets looked at this often (emulated ms)
    kPresenceEveryMs = 15000,       // the user's 15 seconds
    kNoticeMs = 10000,              // a notice stays this long
    kSameSystemEveryMs = 15000,     // the line of who shares our system, this often while somebody does
};
enum Why { WHY_PERIOD = 0, WHY_AUTOPILOT = 1, WHY_JUMP = 2, WHY_LAUNCH = 3, WHY_LAND = 4, WHY_HELLO = 5 };
const double kUnitsPerMetre = 256.0;    // (the destination display's range: the distance in world units, >> 8)
const double kNearMetres = 20000.0;     // a pilot this near is "near" (a nav point's sphere: 12,000 to 17,000)

struct Peer {
    Connection conn;
    int player;                 // the seat it flies (the host's guests: 1..; a guest's one peer: the host, 0)
    std::string callsign;
    bool hello;
    Peer() : player(-1), hello(false) {}
};

// Where each pilot of the room is, by seat (its last PrivPresence).
struct Pilot {
    bool known;
    std::string callsign;
    bool flying;
    std::string system;
    double x, y, z;
    double at;
    Pilot() : known(false), flying(false), x(0), y(0), z(0), at(-1e9) {}
};

struct PrivNet {
    bool host;
    int self;                   // our seat (a guest: -1 until the host's hello)
    int seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    Pilot pilots[kMaxSeats];
    double sentAt;              // our last presence
    PrivNet() : host(false), self(-1), seats(2), listener(NULL), sentAt(-1e9) {}
    ~PrivNet() {
        for (size_t i = 0; i < peers.size(); i++) {
            delete peers[i];
        }
        delete listener;
    }
};

PrivNet *g_net;
double g_nextTryMs;
double g_noticeAt = -1;
double g_sameSystemAt = -1e9;
int g_pendingWhy = -1;          // an occasion to send at the next tick (the A key comes from the keyboard's thread of events)
bool g_wasFlying;
std::string g_system;           // our system, as last seen in flight (a base's is the system it is in)

void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "Privateer: %s", text.c_str());
}

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    return env && env[0] ? env : "Pilot";
}

bool configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') {
        return false;
    }
    if (getenv("WCSOLO") && getenv("WCSOLO")[0] == '1') {
        return false;               // (a room of one: the game as it is)
    }
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

std::string metres(double m) {
    char buf[32];
    const long v = (long)(m + 0.5);
    if (v >= 1000000) {
        snprintf(buf, sizeof(buf), "%ld,%03ld,%03ld m", v / 1000000, (v / 1000) % 1000, v % 1000);
    } else if (v >= 1000) {
        snprintf(buf, sizeof(buf), "%ld,%03ld m", v / 1000, v % 1000);
    } else {
        snprintf(buf, sizeof(buf), "%ld m", v);
    }
    return buf;
}

bool start() {
    net_config.reset_from_env();
    PrivNet *n = new PrivNet();
    n->callsign = own_callsign();
    n->seats = net_config.lobby_players > 0 ? net_config.lobby_players : 2;
    if (n->seats > kMaxSeats) {
        n->seats = kMaxSeats;
    }
    std::string where;
    bool client;
    if (net_config.use_lobby()) {
#ifdef C_LOBBYLINK
        if (!g_lobby) {
            g_lobby = LobbyHub::join(net_config);
        }
        if (!g_lobby) {
            delete n;
            return false;
        }
        client = !g_lobby->is_host();
        if (g_lobby->max_players() > 0) {
            n->seats = std::min((int)kMaxSeats, g_lobby->max_players());
        }
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
            wclog(0, "Privateer: could not reach the host at %s", where.c_str());
            delete n;
            return false;
        }
        Peer *p = new Peer();
        p->conn.adopt(s);
        p->player = 0;
        n->peers.push_back(p);
        n->host = false;
        NetworkMessage msg;
        msg.mutable_priv_hello()->set_callsign(n->callsign);
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
            wclog(0, "Privateer: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
        n->pilots[0].callsign = n->callsign;
    }
    g_net = n;
    wclog(1, "Privateer: %s at %s, %d seats, callsign %s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats, n->callsign.c_str());
    if (n->host) {
        notice("Room open: everybody flies their own Privateer; the others show up here as they go");
    }
    return true;
}

int free_player() {
    for (int p = 1; p < g_net->seats; p++) {
        bool taken = false;
        for (size_t i = 0; i < g_net->peers.size() && !taken; i++) {
            taken = g_net->peers[i]->player == p;
        }
        if (!taken) {
            return p;
        }
    }
    return -1;
}

// Ours, as the game has it now.
PrivPresence own_presence(int why) {
    PrivPresence pr;
    pr.set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    pr.set_callsign(g_net->callsign);
    const bool flying = priv_in_flight();
    pr.set_flying(flying);
    pr.set_system(g_system);
    if (flying) {
        Bit32s pos[3];
        priv_player_pos(pos);
        pr.set_x(pos[0]);
        pr.set_y(pos[1]);
        pr.set_z(pos[2]);
    }
    pr.set_why((Bit32u)why);
    return pr;
}

void send_presence(int why) {
    if (!g_net || g_net->self < 0) {
        return;
    }
    NetworkMessage msg;
    *msg.mutable_priv_presence() = own_presence(why);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
    g_net->sentAt = PIC_FullIndex();
    if (why != WHY_PERIOD) {
        static const char *const kWhy[] = { "the period", "the autopilot", "a jump", "a launch", "a landing", "a hello" };
        wclog(2, "Privateer: our presence sent (%s): %s, %s", kWhy[why], msg.priv_presence().flying() ? "flying" : "not flying",
              g_system.c_str());
    }
}

// Our distance to a pilot in the same system, in the game's metres (-1: not both flying there).
double distance_to(const Pilot &p) {
    if (!p.flying || !priv_in_flight() || p.system != g_system || g_system.empty()) {
        return -1;
    }
    Bit32s pos[3];
    priv_player_pos(pos);
    const double dx = p.x - pos[0], dy = p.y - pos[1], dz = p.z - pos[2];
    return sqrt(dx * dx + dy * dy + dz * dz) / kUnitsPerMetre;
}

// The line of who shares our system (nearest first), or "".
std::string same_system_line() {
    std::vector<std::pair<double, int> > near;
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self || !g_net->pilots[s].known) {
            continue;
        }
        const double d = distance_to(g_net->pilots[s]);
        if (d >= 0) {
            near.push_back(std::make_pair(d, s));
        }
    }
    if (near.empty()) {
        return "";
    }
    std::sort(near.begin(), near.end());
    std::string line = "In " + g_system + " with you:";
    for (size_t i = 0; i < near.size(); i++) {
        line += (i ? ", " : " ") + g_net->pilots[near[i].second].callsign + " " + metres(near[i].first);
    }
    return line;
}

void take_presence(const PrivPresence &pr, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) {
        return;
    }
    Pilot &p = g_net->pilots[seat];
    const bool wasKnown = p.known, wasFlying = p.flying;
    const std::string wasSystem = p.system;
    p.known = true;
    if (pr.has_callsign() && !pr.callsign().empty()) {
        p.callsign = pr.callsign();
    }
    p.flying = pr.flying();
    p.system = pr.system();
    p.x = pr.x();
    p.y = pr.y();
    p.z = pr.z();
    p.at = PIC_FullIndex();
    const int why = (int)pr.why();
    wclog(2, "Privateer: %s (pilot %d) %s in %s at %d, %d, %d (why %d)", p.callsign.c_str(), seat + 1, p.flying ? "flying" : "landed",
          p.system.c_str(), (int)pr.x(), (int)pr.y(), (int)pr.z(), why);
    // What the others are told: a change, an autopilot, or a pilot in our system (its distance).
    const double d = distance_to(p);
    std::string what;
    if (!p.flying) {
        if (!wasKnown || wasFlying || why == WHY_LAND) {
            what = p.system.empty() ? "is at a base" : "landed in " + p.system;
        }
    } else if (d >= 0) {
        if (!wasKnown || !wasFlying || wasSystem != p.system || why != WHY_PERIOD || d < kNearMetres) {
            what = std::string(why == WHY_AUTOPILOT ? "on autopilot " : why == WHY_JUMP ? "jumped in " : why == WHY_LAUNCH ? "launched " : "")
                   + "in " + p.system + ", " + metres(d) + " from you" + (d < kNearMetres ? " (near)" : "");
        }
    } else if (!wasKnown || !wasFlying || wasSystem != p.system || why == WHY_JUMP || why == WHY_LAUNCH) {
        what = (why == WHY_JUMP ? "jumped to " : why == WHY_LAUNCH ? "launched in " : "in ") + p.system;
    }
    if (!what.empty()) {
        notice(p.callsign + " " + what);
    }
}

void handle(Peer *p, NetworkMessage &msg) {
    if (msg.has_priv_hello()) {
        const PrivHello &h = msg.priv_hello();
        if (g_net->host) {
            if (p->hello) {
                return;
            }
            const int player = free_player();
            if (player < 0) {
                wclog(1, "Privateer: %s came in with no seat left: turned away", h.callsign().c_str());
                p->conn.close();
                return;
            }
            p->player = player;
            p->callsign = h.callsign();
            p->hello = true;
            g_net->pilots[player].callsign = h.callsign();
            NetworkMessage reply;
            reply.mutable_priv_hello()->set_callsign(g_net->callsign);
            reply.mutable_priv_hello()->set_player((Bit32u)player);
            reply.mutable_priv_hello()->set_seats((Bit32u)g_net->seats);
            p->conn.send(reply);
            // (everybody known so far, to the newcomer: their presences as the host has them)
            for (int s = 0; s < kMaxSeats; s++) {
                const Pilot &q = g_net->pilots[s];
                if (s == 0 || s == player || !q.known) {
                    continue;
                }
                NetworkMessage out;
                PrivPresence *pr = out.mutable_priv_presence();
                pr->set_player((Bit32u)s);
                pr->set_callsign(q.callsign);
                pr->set_flying(q.flying);
                pr->set_system(q.system);
                pr->set_x((Bit32s)q.x);
                pr->set_y((Bit32s)q.y);
                pr->set_z((Bit32s)q.z);
                pr->set_why(WHY_HELLO);
                p->conn.send(out);
            }
            g_pendingWhy = WHY_HELLO;      // (ours to everybody, the newcomer with them)
            char line[160];
            snprintf(line, sizeof(line), "%s joined as pilot %d of %d", p->callsign.c_str(), player + 1, g_net->seats);
            notice(line);
        } else {
            p->callsign = h.callsign();
            p->hello = true;
            g_net->self = h.has_player() ? (int)h.player() : 1;
            g_net->pilots[0].callsign = h.callsign();
            if (h.has_seats() && h.seats() >= 2) {
                g_net->seats = (int)h.seats();
            }
            g_pendingWhy = WHY_HELLO;
            char line[160];
            snprintf(line, sizeof(line), "In %s's room as pilot %d of %d: fly where you like, the others show up here as they go",
                     p->callsign.c_str(), g_net->self + 1, g_net->seats);
            notice(line);
        }
    } else if (msg.has_priv_presence()) {
        if (!p->hello) {
            return;
        }
        const int seat = g_net->host ? p->player : (msg.priv_presence().has_player() ? (int)msg.priv_presence().player() : 0);
        take_presence(msg.priv_presence(), seat);
        if (g_net->host) {
            NetworkMessage out;
            *out.mutable_priv_presence() = msg.priv_presence();
            out.mutable_priv_presence()->set_player((Bit32u)p->player);
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *to = g_net->peers[i];
                if (to != p && to->hello && to->conn.is_open()) {
                    to->conn.send(out);
                }
            }
        }
    } else if (msg.has_chat()) {
        notice(msg.chat().callsign() + ": " + msg.chat().message());
    }
}

void poll_peers() {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        NetworkMessage msg;
        for (int n = 0; n < 64; n++) {
            RecvStatus st = p->conn.poll(CAT_GAME, msg);
            if (!st.ok()) {
                if (st.failed()) {
                    p->conn.close();
                }
                break;
            }
            handle(p, msg);
        }
        for (int n = 0; n < 64 && p->conn.is_open(); n++) {
            RecvStatus st = p->conn.poll(CAT_CHAT, msg);
            if (!st.ok()) {
                break;
            }
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
            if (p->player >= 0 && p->player < kMaxSeats) {
                g_net->pilots[p->player] = Pilot();
            }
        }
        delete p;
        g_net->peers.erase(g_net->peers.begin() + i);
    }
}

void shutdown() {
    if (!g_net) {
        return;
    }
    wclog(1, "Privateer: session closed");
    delete g_net;
    g_net = NULL;
#ifdef C_LOBBYLINK
    if (g_lobby) {
        LobbyHub *l = g_lobby;
        g_lobby = NULL;
        delete l;
    }
#endif
}

}  // namespace

void priv_session_event(int why) {
    if (g_pendingWhy < 0 || why == WHY_JUMP || why == WHY_LAND || why == WHY_LAUNCH) {
        g_pendingWhy = why;
    }
}

void priv_session_tick() {
    const double now = PIC_FullIndex();
    if (g_noticeAt >= 0 && now - g_noticeAt > kNoticeMs) {
        g_noticeAt = -1;
        incoming_text = "";
    }
    if (!priv_found()) {
        shutdown();
        return;
    }
    // Our own occasions, seen here whatever the network does: a launch, a landing, a jump (the system's name changed).
    const bool flying = priv_in_flight();
    if (flying) {
        const std::string sys = priv_system_name();
        if (!sys.empty() && sys != g_system) {
            const bool jumped = !g_system.empty() && g_wasFlying;
            g_system = sys;
            if (jumped) {
                priv_session_event(WHY_JUMP);
            }
        }
    }
    if (flying != g_wasFlying) {
        priv_session_event(flying ? WHY_LAUNCH : WHY_LAND);
        g_wasFlying = flying;
    }
    if (!g_net) {
        if (!configured() || now < g_nextTryMs) {
            return;
        }
        g_nextTryMs = now + kRetryMs;
        start();
        return;
    }
    static double polledAt = -1e9;
    if (now - polledAt < kPollMs && now >= polledAt) {
        return;
    }
    polledAt = now;
    if (g_net->host && g_net->listener) {
        for (int n = 0; n < 4; n++) {
            Stream *s = g_net->listener->accept(false);
            if (!s) {
                break;
            }
            Peer *p = new Peer();
            p->conn.adopt(s);
            g_net->peers.push_back(p);
            wclog(1, "Privateer: a guest at %s", s->describe().c_str());
        }
    }
    poll_peers();
    if (!g_net->host && g_net->peers.empty()) {
        shutdown();     // (the host gone: tried again later, as at the start)
        return;
    }
    if (g_pendingWhy >= 0) {
        const int why = g_pendingWhy;
        g_pendingWhy = -1;
        send_presence(why);
        if (why == WHY_JUMP || why == WHY_LAUNCH) {
            g_sameSystemAt = -1e9;          // (who is here, at once)
        }
    } else if (now - g_net->sentAt >= kPresenceEveryMs) {
        send_presence(WHY_PERIOD);
    }
    if (now - g_sameSystemAt >= kSameSystemEveryMs && priv_in_flight()) {
        g_sameSystemAt = now;
        const std::string line = same_system_line();
        if (!line.empty() && (g_noticeAt < 0 || now - g_noticeAt > 3000)) {
            notice(line);
        }
    }
}

bool priv_session_active() {
    return g_net != NULL && (g_net->host || g_net->self >= 0);
}

}  // namespace wc
