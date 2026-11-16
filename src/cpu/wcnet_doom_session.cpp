/*
 *  DOOM's session (wcnet_doom.h, docs/doom.md): who plays, and the game's packets carried.
 *
 *  The host is seat 0 (the room's, or a rig's TCP listener); a guest opens a stream to it.  While a player's launcher
 *  (DOOMNET.COM) waits it says hello once a second: its callsign, that it is ready, and the program and WADs it would
 *  run.  The host's launcher starts the game when everybody who is in the room is ready (or at once, Enter, with
 *  those who are): DoomStart names DOOM's players in order -- the host its "key player", number 0, then the guests by
 *  seat -- and gives the host's command line to all.  From there the game is DOOM's own: its packets (DoomPacket: the
 *  bytes as the game made them) go best effort from player to player, through the host when neither end is the host;
 *  the game arbitrates its own start, runs its own lockstep and asks again for what was lost.
 *
 *  A player whose machine goes, or whose game ends without DOOM's own goodbye, is told to the others' games the way
 *  DOOM tells it (a packet with its exit flag, made here: "Player 2 left the game") so that they play on instead of
 *  waiting for its moves for ever.  Nobody joins a game under way (DOOM has no way in): a launcher that comes late
 *  waits for the host's next start.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include "wcnet_doom.h"
#include "wcnet_transport.h"
#include "wcnet_lobby.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "net_config.h"
#include "pic.h"

namespace wc {

namespace {

enum {
    kHelloWaitMs = 20000,
    kPollMs = 5,
    kHelloEveryMs = 1000,
    kQuietMs = 6000,            // a guest not heard for this long is not counted ready
    kSettleMs = 1500,           // everybody ready for this long: the game starts
    kRetryMs = 3000,
    kGoneAgainMs = 1000,        // a gone player's exit told to our game again this often (one may be lost on DOOM's start)
    kMaxQueue = 512,
};

struct Peer {
    Connection conn;
    int seat;                   // the host's guests: the room's seat (a rig's: the first free); a guest's host: 0
    std::string callsign, game;
    bool hello, ready;
    double heardAt;
    Peer() : seat(-1), hello(false), ready(false), heardAt(-1e9) {}
};

struct DoomNet {
    bool host;
    int self, seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    DoomNet() : host(false), self(-1), seats(2), listener(NULL) {}
    ~DoomNet() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

struct Packet {
    int from;
    std::string data;
};

DoomNet *g_net = NULL;
double g_nextTryMs = 0, g_pollAt = -1e9, g_fastPollAt = -1e9, g_helloAt = -1e9;
bool g_tried = false;
// the launcher's
bool g_wanted = false, g_ready = false;
std::string g_gameId, g_args;
// the game
Bit32u g_serials = 0;
DoomGame g_dgame;
bool g_playing = false;             // our DOOM in it
int g_seatOf[kDoomMaxPlayers];
bool g_gone[kDoomMaxPlayers];
double g_goneSaid[kDoomMaxPlayers];
std::deque<Packet> g_in;
double g_allReadySince = -1;
// a guest's view of the host
Bit32u g_hostPlaying = 0;
bool g_hostReady = false;
std::string g_hostNote;

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    return env && env[0] ? env : "Player";
}

bool start() {
    net_config.reset_from_env();
    DoomNet *n = new DoomNet();
    n->callsign = own_callsign();
    n->seats = net_config.lobby_players > 0 ? std::min((int)kDoomMaxPlayers, net_config.lobby_players) : 2;
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
        if (g_lobby->max_players() > 0) n->seats = std::min((int)kDoomMaxPlayers, g_lobby->max_players());
        where = "room " + g_lobby->code();
#else
        delete n;
        return false;
#endif
    } else {
#ifdef __EMSCRIPTEN__
        wclog(0, "DOOM: the browser build has no TCP transport: set WCROOM (a room code) to play");
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
            wclog(0, "DOOM: could not reach the host at %s", where.c_str());
            delete n;
            return false;
        }
        Peer *p = new Peer();
        p->conn.adopt(s);
        p->seat = 0;
        n->peers.push_back(p);
    } else {
        Listener *l = NULL;
#ifdef C_LOBBYLINK
        if (g_lobby) l = g_lobby->make_listener();
        else
#endif
            l = tcp_listen(net_config.portstr);
        if (!l) {
            wclog(0, "DOOM: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
    }
    g_net = n;
    g_helloAt = -1e9;
    g_hostPlaying = 0;
    g_hostReady = false;
    g_hostNote.clear();
    wclog(1, "DOOM: %s at %s, %d seats, callsign %s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats, n->callsign.c_str());
    return true;
}

Peer *peer_of_seat(int seat) {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->seat == seat && g_net->peers[i]->conn.is_open()) return g_net->peers[i];
    }
    return NULL;
}

int free_seat() {
    for (int s = 1; s < g_net->seats; s++) {
        bool taken = false;
        for (size_t i = 0; i < g_net->peers.size() && !taken; i++) taken = g_net->peers[i]->hello && g_net->peers[i]->seat == s;
        if (!taken) return s;
    }
    return -1;
}

// DOOM's player number of a seat in the game under way (-1: not in it)
int player_of_seat(int seat) {
    if (!g_dgame.serial) return -1;
    for (int p = 0; p < g_dgame.players; p++) {
        if (g_seatOf[p] == seat) return p;
    }
    return -1;
}

void mark_gone(int player, const char *why) {
    if (player < 0 || player >= g_dgame.players || player == g_dgame.self || g_gone[player]) return;
    g_gone[player] = true;
    g_goneSaid[player] = -1e9;
    wclog(1, "DOOM: player %d (%s) is out of the game: %s", player + 1, g_dgame.names[player].c_str(), why);
}

// (the host's) a player gone: our game told, and the other guests' launchers
void host_tell_gone(int player, const char *why, Peer *except) {
    mark_gone(player, why);
    NetworkMessage msg;
    msg.mutable_doom_gone()->set_serial(g_dgame.serial);
    msg.mutable_doom_gone()->set_player((Bit32u)player);
    for (int p = 1; p < g_dgame.players; p++) {
        Peer *to = p == player ? NULL : peer_of_seat(g_seatOf[p]);
        if (to && to != except) to->conn.send(msg);
    }
}

bool same_game(const Peer *p) { return p->game == g_gameId; }

void host_hello(Peer *to) {
    NetworkMessage msg;
    DoomHello *h = msg.mutable_doom_hello();
    h->set_callsign(g_net->callsign);
    h->set_player((Bit32u)to->seat);
    h->set_seats((Bit32u)g_net->seats);
    h->set_ready(g_ready && !g_playing);
    h->set_playing(g_playing ? g_dgame.serial : 0);
    if (g_ready && to->ready && !same_game(to)) {
        h->set_note("Not the same game as " + g_net->callsign + "'s (theirs: " + g_gameId + "; yours: " + to->game + "): no game together");
    }
    to->conn.send(msg);
}

void handle(Peer *p, NetworkMessage &msg) {
    const double now = PIC_FullIndex();
    if (msg.has_doom_hello()) {
        const DoomHello &h = msg.doom_hello();
        if (g_net->host) {
            if (!p->hello) {
                if (p->seat <= 0) p->seat = free_seat();
                if (p->seat <= 0 || p->seat >= 32) {
                    wclog(1, "DOOM: %s came in with no seat left: turned away", h.callsign().c_str());
                    p->conn.close();
                    return;
                }
                p->hello = true;
                wclog(1, "DOOM: %s is in the room (seat %d)", h.callsign().c_str(), p->seat);
            }
            const bool was = p->ready;
            p->callsign = h.callsign();
            p->game = h.game();
            p->ready = h.ready();
            p->heardAt = now;
            if (p->ready != was) {
                wclog(1, "DOOM: %s %s", p->callsign.c_str(), p->ready ? (same_game(p) || !g_ready ? "is ready" : "is ready, with other game files")
                                                                  : "is not at the start any more");
                host_hello(p);
                // back at its launcher while the game it was in goes on: its game is over
                const int player = player_of_seat(p->seat);
                if (p->ready && player > 0 && !g_gone[player]) host_tell_gone(player, "back at its launcher", p);
            }
        } else {
            p->hello = true;
            p->callsign = h.callsign();
            p->heardAt = now;
            if (h.has_player()) g_net->self = (int)h.player();
            if (h.has_seats() && h.seats() >= 2) g_net->seats = std::min((int)kDoomMaxPlayers, (int)h.seats());
            g_hostPlaying = h.playing();
            g_hostReady = h.ready();
            g_hostNote = h.note();
        }
    } else if (msg.has_doom_start()) {
        if (g_net->host) return;
        const DoomStart &st = msg.doom_start();
        int self = -1;
        for (int i = 0; i < st.seat_size() && i < kDoomMaxPlayers; i++) {
            if ((int)st.seat(i) == g_net->self) self = i;
        }
        if (self <= 0 || !g_ready || g_playing || st.seat_size() < 2) {
            g_hostPlaying = st.serial();
            wclog(1, "DOOM: the host's game %u starts without us", st.serial());
            return;
        }
        g_dgame = DoomGame();
        g_dgame.serial = st.serial();
        g_dgame.players = std::min((int)kDoomMaxPlayers, st.seat_size());
        g_dgame.self = self;
        g_dgame.args = st.args();
        g_dgame.ticdup = st.has_ticdup() ? std::max(1, std::min(5, (int)st.ticdup())) : 1;
        g_dgame.extratics = st.has_extratics() ? std::min(1, (int)st.extratics()) : 1;
        for (int i = 0; i < g_dgame.players; i++) {
            g_seatOf[i] = (int)st.seat(i);
            g_dgame.names[i] = i < st.callsign_size() ? st.callsign(i) : std::string("Player");
            g_gone[i] = false;
        }
        g_in.clear();
        g_playing = true;
        wclog(1, "DOOM: game %u: player %d of %d", g_dgame.serial, self + 1, g_dgame.players);
    } else if (msg.has_doom_packet()) {
        const DoomPacket &pk = msg.doom_packet();
        if (!g_dgame.serial || pk.serial() != g_dgame.serial || pk.data().size() > kDoomPacketMax) return;
        int from = (int)pk.from();
        const int to = (int)pk.to();
        if (g_net->host) {
            from = player_of_seat(p->seat);
            if (from <= 0 || to < 0 || to >= g_dgame.players || to == from) return;
            if (to != 0) {      // between two guests: passed on
                Peer *dest = peer_of_seat(g_seatOf[to]);
                if (dest) {
                    NetworkMessage out;
                    *out.mutable_doom_packet() = pk;
                    out.mutable_doom_packet()->set_from((Bit32u)from);
                    dest->conn.send_best_effort(out);
                }
                return;
            }
        } else if (to != g_dgame.self || from < 0 || from >= g_dgame.players || from == g_dgame.self) {
            return;
        }
        if (!g_playing) return;
        if (g_in.size() >= kMaxQueue) g_in.pop_front();
        Packet in;
        in.from = from;
        in.data = pk.data();
        g_in.push_back(in);
    } else if (msg.has_doom_gone()) {
        const DoomGone &g = msg.doom_gone();
        if (!g_dgame.serial || g.serial() != g_dgame.serial) return;
        if (g_net->host) {
            const int player = player_of_seat(p->seat);
            if (player > 0) host_tell_gone(player, "its game ended", p);
        } else {
            mark_gone((int)g.player(), "the host's word");
        }
    }
}

void poll_peers() {
    if (g_net->host && g_net->listener) {
        for (int n = 0; n < 4; n++) {
            Stream *s = g_net->listener->accept(false);
            if (!s) break;
            Peer *p = new Peer();
            p->seat = s->room_seat();
            wclog(1, "DOOM: a guest at %s", s->describe().c_str());
            p->conn.adopt(s);
            g_net->peers.push_back(p);
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
        for (int n = 0; n < 128 && p->conn.is_open(); n++) {
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
        if (p->hello) wclog(1, "DOOM: %s %s", p->callsign.c_str(), g_net->host ? "left the room" : "(the host) is gone");
        if (g_net->host) {
            const int player = p->hello ? player_of_seat(p->seat) : -1;
            g_net->peers.erase(g_net->peers.begin() + i);
            if (player > 0) host_tell_gone(player, "its link is gone", p);
        } else {
            // every packet came through the host: nobody is left to play with
            for (int k = 0; k < g_dgame.players; k++) mark_gone(k, "the host's link is gone");
            g_net->peers.erase(g_net->peers.begin() + i);
        }
        delete p;
    }
}

void send_hellos() {
    if (g_net->host) {
        for (size_t i = 0; i < g_net->peers.size(); i++) {
            if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) host_hello(g_net->peers[i]);
        }
        return;
    }
    if (g_net->peers.empty()) return;
    NetworkMessage msg;
    DoomHello *h = msg.mutable_doom_hello();
    h->set_callsign(g_net->callsign);
    h->set_ready(g_ready && !g_playing);
    h->set_game(g_gameId);
    g_net->peers[0]->conn.send(msg);
}

// (the host's) the guests ready for a game, by seat, and how many of the room's are not
void ready_guests(std::vector<Peer *> &ready, int *waiting, std::vector<std::string> *others) {
    const double now = PIC_FullIndex();
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (!p->hello || !p->conn.is_open()) continue;
        if (p->ready && now - p->heardAt < kQuietMs && same_game(p) && (int)ready.size() < kDoomMaxPlayers - 1) ready.push_back(p);
        else if (others && p->ready && !same_game(p)) others->push_back(p->callsign + " has other game files");
    }
    std::sort(ready.begin(), ready.end(), [](const Peer *a, const Peer *b) { return a->seat < b->seat; });
    int n = 0;
    if (g_net->listener && g_net->listener->has_rooms()) {
        for (int s = 1; s < 32; s++) {
            if (g_net->listener->seat_held(s) != 1) continue;
            bool is = false;
            for (size_t i = 0; i < ready.size() && !is; i++) is = ready[i]->seat == s;
            n += !is;
        }
    } else {
        n = std::max(0, g_net->seats - 1 - (int)ready.size());
    }
    *waiting = n;
}

}  // namespace

bool doom_session_configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') return false;
    if (getenv("WCSOLO") && getenv("WCSOLO")[0] == '1') return false;
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

void doom_session_tick() {
    const double now = PIC_FullIndex();
    if (!g_net) {
        if (!g_wanted || (g_tried && now < g_nextTryMs)) return;
        g_tried = true;
        g_nextTryMs = now + kRetryMs;
        if (!doom_session_configured() || !start()) return;
    }
    if (now - g_pollAt >= kPollMs) {
        g_pollAt = now;
        poll_peers();
        if (!g_net->host && g_net->peers.empty()) {
            doom_session_shutdown();    // (the host gone: looked for again while a launcher waits)
            return;
        }
    }
    if (now - g_helloAt >= kHelloEveryMs) {
        g_helloAt = now;
        send_hellos();
    }
}

void doom_session_shutdown() {
    if (!g_net) return;
    wclog(1, "DOOM: session closed");
    delete g_net;
    g_net = NULL;
    g_tried = false;
    g_allReadySince = -1;
}

void doom_session_waiting(bool ready, const std::string &game, const std::string &args) {
    if (ready) g_wanted = true;
    if (ready != g_ready || game != g_gameId) g_helloAt = -1e9;     // (said at once)
    g_ready = ready;
    g_gameId = game;
    g_args = args;
}

bool doom_session_is_host() { return g_net && g_net->host; }

bool doom_session_start(bool now, bool *alone) {
    *alone = false;
    if (!g_net || !g_net->host || g_playing || !g_ready) {
        // (a guest's Enter, or a launcher with no room found yet: nobody to start with)
        if (now && (!g_net || g_net->host)) *alone = true;
        return false;
    }
    std::vector<Peer *> ready;
    int waiting = 0;
    ready_guests(ready, &waiting, NULL);
    const double t = PIC_FullIndex();
    if (now) {
        if (ready.empty()) {
            *alone = true;
            return false;
        }
    } else {
        if (ready.empty() || waiting > 0) {
            g_allReadySince = -1;
            return false;
        }
        if (g_allReadySince < 0) g_allReadySince = t;
        if (t - g_allReadySince < kSettleMs) return false;
    }
    g_allReadySince = -1;
    if (!g_serials) g_serials = ((Bit32u)time(NULL) & 0xFFFFF) << 8;
    g_dgame = DoomGame();
    g_dgame.serial = ++g_serials;
    g_dgame.players = 1 + (int)ready.size();
    g_dgame.self = 0;
    g_dgame.args = g_args;
    g_dgame.names[0] = g_net->callsign;
    g_seatOf[0] = 0;
    NetworkMessage msg;
    DoomStart *st = msg.mutable_doom_start();
    st->set_serial(g_dgame.serial);
    st->set_args(g_dgame.args);
    st->set_ticdup((Bit32u)g_dgame.ticdup);
    st->set_extratics((Bit32u)g_dgame.extratics);
    st->add_seat(0);
    st->add_callsign(g_net->callsign);
    for (size_t i = 0; i < ready.size(); i++) {
        g_seatOf[i + 1] = ready[i]->seat;
        g_dgame.names[i + 1] = ready[i]->callsign;
        st->add_seat((Bit32u)ready[i]->seat);
        st->add_callsign(ready[i]->callsign);
        ready[i]->ready = false;        // (until its launcher says so again: after this game)
    }
    for (int p = 0; p < kDoomMaxPlayers; p++) g_gone[p] = false;
    g_in.clear();
    for (size_t i = 0; i < ready.size(); i++) ready[i]->conn.send(msg);
    g_playing = true;
    wclog(1, "DOOM: game %u started: %d players%s", g_dgame.serial, g_dgame.players, now ? " (Enter)" : "");
    return true;
}

bool doom_session_game(DoomGame *out) {
    if (!g_playing) return false;
    *out = g_dgame;
    return true;
}

void doom_session_game_over() {
    if (!g_playing) return;
    g_playing = false;
    g_in.clear();
    if (!g_net) return;
    NetworkMessage msg;
    msg.mutable_doom_gone()->set_serial(g_dgame.serial);
    msg.mutable_doom_gone()->set_player((Bit32u)g_dgame.self);
    if (g_net->host) {
        for (int p = 1; p < g_dgame.players; p++) {
            Peer *to = peer_of_seat(g_seatOf[p]);
            if (to) to->conn.send(msg);
        }
    } else if (!g_net->peers.empty()) {
        g_net->peers[0]->conn.send(msg);
    }
    g_helloAt = -1e9;
}

std::string doom_session_line() {
    if (!g_net) return doom_session_configured() ? "Looking for the room..." : "";
    if (g_net->host) {
        std::vector<Peer *> ready;
        std::vector<std::string> others;
        int waiting = 0;
        ready_guests(ready, &waiting, &others);
        if (ready.empty() && !waiting && others.empty()) {
            return "Nobody else is in the room yet: waiting for them.  (Enter or Esc: play alone)";
        }
        std::string line = "In the room:";
        for (size_t i = 0; i < ready.size(); i++) line += std::string(i ? ", " : " ") + ready[i]->callsign + " ready";
        for (size_t i = 0; i < others.size(); i++) line += std::string(i || !ready.empty() ? ", " : " ") + others[i];
        const int starting = waiting - (int)others.size();
        if (starting > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s%d not at the start yet", ready.empty() && others.empty() ? " " : ", ", starting);
            line += buf;
        }
        if (waiting <= 0 && !ready.empty()) return line + ".  Starting...";
        return line + (ready.empty() ? ".  (Enter or Esc: play alone)" : ".  (Enter: start with those ready, Esc: play alone)");
    }
    const Peer *host = g_net->peers.empty() ? NULL : g_net->peers[0];
    if (!host || !host->hello) return "Waiting for the host's game to answer.  (Esc: play alone)";
    if (!g_hostNote.empty()) return g_hostNote + ".  (Esc: play alone)";
    if (g_hostPlaying) return host->callsign + "'s game is under way: waiting for the next one.  (Esc: play alone)";
    if (g_hostReady) return "Waiting for " + host->callsign + " to start the game.  (Esc: play alone)";
    return host->callsign + " is not at the start: waiting.  (Esc: play alone)";
}

void doom_session_send(int player, const Bit8u *data, int len) {
    if (!g_playing || !g_net || player < 0 || player >= g_dgame.players || player == g_dgame.self || len <= 0 || len > kDoomPacketMax) return;
    Peer *to = g_net->host ? peer_of_seat(g_seatOf[player]) : (g_net->peers.empty() ? NULL : g_net->peers[0]);
    if (!to || !to->conn.is_open()) return;
    NetworkMessage msg;
    DoomPacket *pk = msg.mutable_doom_packet();
    pk->set_serial(g_dgame.serial);
    pk->set_from((Bit32u)g_dgame.self);
    pk->set_to((Bit32u)player);
    pk->set_data(std::string((const char *)data, (size_t)len));
    to->conn.send_best_effort(msg);
}

bool doom_session_gone(int player) { return player >= 0 && player < kDoomMaxPlayers && g_gone[player]; }

int doom_session_recv(int *player, Bit8u *data, int max) {
    if (!g_playing) return 0;
    const double now = PIC_FullIndex();
    // (the game asks many times a tic: the links looked at here too, at most four times a millisecond)
    if (g_net && g_in.empty() && now - g_fastPollAt >= 0.25) {
        g_fastPollAt = now;
        poll_peers();
        if (!g_net->host && g_net->peers.empty()) doom_session_shutdown();
    }
    if (!g_in.empty()) {
        const Packet &in = g_in.front();
        const int len = std::min(max, (int)in.data.size());
        memcpy(data, in.data.data(), (size_t)len);
        *player = in.from;
        g_in.pop_front();
        return len;
    }
    // a player gone: DOOM's own exit packet from it (doomdata_t: the checksum with NCMD_EXIT, retransmitfrom,
    // starttic, player, numtics; the checksum as NetbufferChecksum makes it: 0x1234567 plus the one dword after it)
    for (int p = 0; p < g_dgame.players; p++) {
        if (!g_gone[p] || p == g_dgame.self || now - g_goneSaid[p] < kGoneAgainMs || max < 8) continue;
        g_goneSaid[p] = now;
        const Bit32u body = (Bit32u)p << 16;
        const Bit32u sum = ((0x1234567u + body) & 0x0FFFFFFFu) | 0x80000000u;
        for (int i = 0; i < 4; i++) {
            data[i] = (Bit8u)(sum >> (8 * i));
            data[4 + i] = (Bit8u)(body >> (8 * i));
        }
        *player = p;
        return 8;
    }
    return 0;
}

}  // namespace wc
