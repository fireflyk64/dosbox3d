/*
 *  The room's IPX network (wcnet_ipx.h, docs/armada.md): IPX packets carried by the room's links.
 *
 *  The host is seat 0 (the room's, or a rig's TCP listener); each guest opens one link to it and says hello (its
 *  callsign and its node).  A packet goes whole, its IPX header as the program wrote it.  The host is the network's
 *  hub: a packet for one node goes to that node's machine, a broadcast (node FF FF FF FF FF FF) to every machine but
 *  the one it came from, a packet for a node nobody has said is theirs to every machine (as a hub would; the node
 *  that is not there never answers).  Our own broadcasts and packets for ourselves are delivered here by the driver
 *  itself (ipx.cpp), as DOSBox's own does.
 *
 *  A guest whose host is gone has no network (every packet went through the host) until the host is back: looked
 *  for again every few seconds.  A guest gone is simply not there any more: the programs on the network notice it
 *  the way they would on a LAN (NetBIOS: its session ends; Armada: "NET LINK BROKEN").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include <random>
#include "wcnet_ipx.h"
#include "wcnet_transport.h"
#include "wcnet_lobby.h"
#include "wcnet_log.h"
#include "net_config.h"
#include "pic.h"

namespace wc {

namespace {

enum {
    kPollMs = 2,                // the links looked at this often (emulated time)
    kRetryMs = 3000,            // a room or a host not found: looked for again
    kOpenWaitMs = 250,          // a guest's wait for its link to the host, once the room says it is up
    kReportMs = 10000,          // the log's line about the traffic
    kMaxPacket = 1424,          // IPXBUFFERSIZE (ipx.h)
    kHeader = 30,
};

struct Peer {
    Connection conn;
    int seat;
    std::string callsign;
    Bit8u node[6];
    bool known;                 // its hello heard: its node is known
    Bit32u in, out;
    Peer() : seat(-1), known(false), in(0), out(0) { memset(node, 0, sizeof(node)); }
};

struct Net {
    bool host;
    bool lobby;
    std::string where;
    Listener *listener;
    std::vector<Peer *> peers;
    Net() : host(false), lobby(false), listener(NULL) {}
    ~Net() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

Net *g_net = NULL;
IpxReceiver g_rx = NULL;
Bit8u g_node[6];
bool g_nodeMade = false;
bool g_lossy = false;
bool g_started = false;         // ipx_room_init called
double g_nextTryMs = 0, g_pollAt = -1e9, g_reportAt = 0, g_hostTryAt = 0;
Bit32u g_sent = 0, g_got = 0, g_passed = 0, g_flooded = 0, g_bad = 0;
std::string g_line;

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    return env && env[0] ? env : "Player";
}

std::string node_text(const Bit8u *n) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", n[0], n[1], n[2], n[3], n[4], n[5]);
    return buf;
}

bool is_broadcast(const Bit8u *n) {
    for (int i = 0; i < 6; i++) {
        if (n[i] != 0xff) return false;
    }
    return true;
}

// the IPX header's destination node (bytes 10-15) and source node (22-27)
const Bit8u *dest_node(const std::string &p) { return (const Bit8u *)p.data() + 10; }
const Bit8u *src_node(const std::string &p) { return (const Bit8u *)p.data() + 22; }

void send_to(Peer *p, const NetworkMessage &msg) {
    if (!p->conn.is_open()) return;
    if (g_lossy) p->conn.send_best_effort(msg);
    else p->conn.send(msg);
    p->out++;
}

void say_hello(Peer *to) {
    NetworkMessage msg;
    IpxHello *h = msg.mutable_ipx_hello();
    h->set_callsign(own_callsign());
    h->set_node(std::string((const char *)g_node, 6));
    if (g_net->host && to->seat >= 0) h->set_seat((Bit32u)to->seat);
    to->conn.send(msg);     // (always reliably)
}

void deliver(const std::string &data) {
    if (!g_rx) return;
    Bit8u buf[kMaxPacket];
    memcpy(buf, data.data(), data.size());
    g_got++;
    g_rx(buf, (Bit16s)data.size());
}

// (the host's) a packet from `from` (NULL: our own) where it goes
void route(const std::string &data, Peer *from) {
    const Bit8u *dest = dest_node(data);
    NetworkMessage msg;
    msg.mutable_ipx_packet()->set_data(data);
    if (is_broadcast(dest)) {
        if (from) deliver(data);
        for (size_t i = 0; i < g_net->peers.size(); i++) {
            Peer *p = g_net->peers[i];
            if (p != from && p->known) send_to(p, msg);
        }
        if (from) g_passed++;
        return;
    }
    if (!memcmp(dest, g_node, 6)) {
        if (from) deliver(data);
        return;
    }
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (p->known && !memcmp(p->node, dest, 6)) {
            if (p != from) {
                send_to(p, msg);
                if (from) g_passed++;
            }
            return;
        }
    }
    // a node nobody has said is theirs: everybody's, as a hub would
    g_flooded++;
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (p != from && p->known) send_to(p, msg);
    }
}

void handle(Peer *p, const NetworkMessage &msg) {
    if (msg.has_ipx_hello()) {
        const IpxHello &h = msg.ipx_hello();
        const bool first = !p->known;
        p->callsign = h.callsign();
        if (h.node().size() == 6) {
            memcpy(p->node, h.node().data(), 6);
            p->known = true;
        }
        if (g_net->host) {
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *o = g_net->peers[i];
                if (o != p && o->known && p->known && !memcmp(o->node, p->node, 6)) {
                    wclog(0, "IPX: %s and %s have the same node %s: one of them will not be reached", o->callsign.c_str(),
                          p->callsign.c_str(), node_text(p->node).c_str());
                }
            }
            if (first) say_hello(p);
        }
        if (first) {
            wclog(1, "IPX: %s is on the network (node %s%s)", p->callsign.c_str(), node_text(p->node).c_str(),
                  g_net->host ? "" : ", the host");
        }
        return;
    }
    if (msg.has_ipx_packet()) {
        const std::string &data = msg.ipx_packet().data();
        if (data.size() < kHeader || data.size() > kMaxPacket) {
            g_bad++;
            return;
        }
        p->in++;
        if (g_net->host) {
            route(data, p);
        } else {
            const Bit8u *dest = dest_node(data);
            if (is_broadcast(dest) || !memcmp(dest, g_node, 6)) deliver(data);
        }
    }
}

bool start() {
    net_config.reset_from_env();
    Net *n = new Net();
    if (net_config.use_lobby()) {
#ifdef C_LOBBYLINK
        if (!g_lobby) g_lobby = LobbyHub::join(net_config);
        if (!g_lobby) {
            delete n;
            return false;
        }
        n->lobby = true;
        n->host = g_lobby->is_host();
        n->where = "room " + g_lobby->code();
#else
        delete n;
        return false;
#endif
    } else {
#ifdef __EMSCRIPTEN__
        wclog(0, "IPX: the browser build has no TCP transport: set WCROOM (a room code) to play");
        delete n;
        return false;
#endif
        n->host = !(net_config.host && net_config.host[0]);
        n->where = n->host ? std::string("port ") + net_config.portstr : std::string(net_config.host) + ":" + net_config.portstr;
    }
    if (n->host) {
#ifdef C_LOBBYLINK
        if (n->lobby) n->listener = g_lobby->make_listener();
        else
#endif
            n->listener = tcp_listen(net_config.portstr);
        if (!n->listener) {
            wclog(0, "IPX: could not listen on %s", n->where.c_str());
            delete n;
            return false;
        }
    }
    g_net = n;
    g_hostTryAt = 0;
    wclog(1, "IPX: the network of %s, this machine the %s, node %s, callsign %s%s", n->where.c_str(), n->host ? "host" : "guest",
          node_text(g_node).c_str(), own_callsign().c_str(), g_lossy ? " (packets best effort)" : "");
    return true;
}

// (a guest's) the link to the host, when it is there
void reach_host() {
    const double now = PIC_FullIndex();
    if (!g_net->peers.empty() || now < g_hostTryAt) return;
    g_hostTryAt = now + kRetryMs;
    Stream *s = NULL;
#ifdef C_LOBBYLINK
    if (g_net->lobby) {
        if (!g_lobby->peer_up(0)) return;
        s = g_lobby->open_peer(0, kOpenWaitMs);
    } else
#endif
        s = tcp_connect(net_config.host, net_config.portstr);
    if (!s) return;
    Peer *p = new Peer();
    p->seat = 0;
    p->conn.adopt(s);
    g_net->peers.push_back(p);
    wclog(1, "IPX: linked to the host at %s", s->describe().c_str());
    say_hello(p);
}

void poll_links() {
    if (g_net->host && g_net->listener) {
        for (int k = 0; k < 4; k++) {
            Stream *s = g_net->listener->accept(false);
            if (!s) break;
            Peer *p = new Peer();
            p->seat = s->room_seat();
            if (p->seat < 0) p->seat = (int)g_net->peers.size() + 1;
            p->conn.adopt(s);
            g_net->peers.push_back(p);
            wclog(1, "IPX: a machine at %s (seat %d)", s->describe().c_str(), p->seat);
        }
    }
    if (!g_net->host) reach_host();
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        NetworkMessage msg;
        for (int k = 0; k < 256 && p->conn.is_open(); k++) {
            RecvStatus st = p->conn.poll(CAT_GAME, msg);
            if (!st.ok()) {
                if (st.failed()) p->conn.close();
                break;
            }
            handle(p, msg);
        }
        for (int k = 0; k < 256 && p->conn.is_open(); k++) {
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
        wclog(1, "IPX: %s%s left the network (%u packets from it, %u to it)", p->callsign.empty() ? "a machine" : p->callsign.c_str(),
              g_net->host ? "" : " (the host)", p->in, p->out);
        g_net->peers.erase(g_net->peers.begin() + i);
        delete p;
        if (!g_net->host) g_hostTryAt = PIC_FullIndex() + kRetryMs;
    }
}

void make_line() {
    char buf[160];
    if (!g_net) {
        g_line = ipx_room_configured() ? "IPX: looking for the room" : "";
        return;
    }
    int known = 0;
    std::string names;
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (!g_net->peers[i]->known) continue;
        names += std::string(known ? ", " : "") + g_net->peers[i]->callsign;
        known++;
    }
    snprintf(buf, sizeof(buf), "IPX on %s: %s%s%s", g_net->where.c_str(),
             known ? (g_net->host ? "the host, with " : "a guest of ") : (g_net->host ? "the host, nobody else yet" : "a guest, the host not reached yet"),
             names.c_str(), "");
    g_line = buf;
}

}  // namespace

bool ipx_room_configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') return false;
    if (getenv("WCSOLO") && getenv("WCSOLO")[0] == '1') return false;
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

void ipx_room_node(Bit8u node[6]) {
    if (!g_nodeMade) {
        g_nodeMade = true;
        const char *env = getenv("WCIPXNODE");
        bool given = false;
        if (env && strlen(env) == 12) {
            given = true;
            for (int i = 0; i < 6 && given; i++) {
                unsigned v = 0;
                given = sscanf(env + 2 * i, "%2x", &v) == 1;
                g_node[i] = (Bit8u)v;
            }
        }
        if (!given) {
            // A node of its own, as a network card's: locally administered (02), five bytes at random.  Made before
            // the room is found (NETBIOS.EXE asks for it as it loads), so not from the seat.
            std::random_device rd;
            Bit32u a = rd() ^ (Bit32u)time(NULL), b = rd();
            g_node[0] = 0x02;
            g_node[1] = (Bit8u)a;
            g_node[2] = (Bit8u)(a >> 8);
            g_node[3] = (Bit8u)(a >> 16);
            g_node[4] = (Bit8u)(a >> 24);
            g_node[5] = (Bit8u)b;
        }
    }
    memcpy(node, g_node, 6);
}

void ipx_room_init(IpxReceiver rx) {
    Bit8u n[6];
    ipx_room_node(n);
    g_rx = rx;
    g_started = true;
    const char *lossy = getenv("WCIPXLOSSY");
    g_lossy = lossy && lossy[0] == '1';
    g_nextTryMs = 0;
    wclog(1, "IPX: carried by the room (node %s)", node_text(g_node).c_str());
}

void ipx_room_tick() {
    if (!g_started) return;
    const double now = PIC_FullIndex();
    if (!g_net) {
        if (now < g_nextTryMs) return;
        g_nextTryMs = now + kRetryMs;
        if (!start()) return;
    }
    if (now - g_pollAt >= kPollMs) {
        g_pollAt = now;
        poll_links();
    }
    if (now >= g_reportAt) {
        g_reportAt = now + kReportMs;
        make_line();
        if (g_sent || g_got) {
            wclog(1, "%s; packets sent %u, delivered %u, passed on %u, to everybody for want of a node %u%s", g_line.c_str(), g_sent,
                  g_got, g_passed, g_flooded, g_bad ? " (some malformed)" : "");
        }
    }
}

void ipx_room_send(const Bit8u *packet, int len) {
    if (!g_net || len < kHeader || len > kMaxPacket) return;
    g_sent++;
    std::string data((const char *)packet, (size_t)len);
    if (g_net->host) {
        route(data, NULL);
        return;
    }
    if (g_net->peers.empty()) return;       // (the host not reached: lost, as on a cable pulled out)
    NetworkMessage msg;
    msg.mutable_ipx_packet()->set_data(data);
    send_to(g_net->peers[0], msg);
}

void ipx_room_shutdown() {
    if (g_net) wclog(1, "IPX: off the network of %s", g_net->where.c_str());
    delete g_net;
    g_net = NULL;
    g_started = false;
    g_rx = NULL;
}

const char *ipx_room_line() {
    make_line();
    return g_line.c_str();
}

}  // namespace wc
