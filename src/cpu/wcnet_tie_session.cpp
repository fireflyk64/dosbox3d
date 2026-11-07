/*
 *  TIE Fighter's session (wcnet_tie_session.h).
 *
 *  The host is player 0 (the room's, or the TCP listener); a guest opens a
 *  stream to it and says hello, and the host answers with the guest's
 *  player number and the room's seats.  Every machine runs the same mission
 *  from its own game; at create_createmission the player's flight group is
 *  given a craft a seat, and a guest's game is told that the guest flies
 *  its own craft of the group (the record's player-craft number), so that
 *  the mission's own setup makes everybody's craft in the same object slots
 *  everywhere.  In flight each machine sends its own craft every frame
 *  (best effort, a reliable one a second) and writes the others' craft into
 *  its own objects table (puppets); the host relays a guest's frames to the
 *  other guests.  What is not here yet: ownership of the AI craft by
 *  distance, shots and damage across, deaths and spawns (docs/tiefighter.md,
 *  sections 5 and 6).
 */
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <string>
#include <vector>
#include "wcnet_tie_session.h"
#include "wcnet_tie.h"
#include "wcnet_transport.h"
#include "wcnet_lobby.h"
#include "wcnet_session.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "net_config.h"
#include "pic.h"
#include "mem.h"

extern std::string incoming_text;   // the line the emulator draws over the picture (sdlmain.cpp)

namespace wc {

namespace {

// (kPollMs: the sockets are looked at that often outside a flight frame, in emulated ms; the periodic tick comes every
// 999 instructions, some 60 times an emulated ms at the game's 60,000 cycles, and a select() each time slowed the
// emulator to 85%: measured, docs/tiefighter.md section 7)
enum { kPlayerFgNone = -1, kRetryMs = 10000, kReliableEveryMs = 1000, kNoticeMs = 12000, kHelloWaitMs = 20000, kPollMs = 4 };

struct Peer {
    Connection conn;
    int player;                 // the seat it flies (the host's guests: 1..; a guest's one peer: the host, 0)
    std::string callsign;
    bool hello;                 // the hello exchanged
    bool flying;                // by its last frame
    Bit64u lastFrame;
    double lastFrameAt;
    Peer() : player(-1), hello(false), flying(false), lastFrame(0), lastFrameAt(-1) {}
};

struct Puppet {
    bool used;
    int owner;
    bool fresh;                 // a report not yet written into the game
    TieCraft state;
    double at;
};

// Where each pilot of the room is (its TiePresence), by seat.
struct Pilot {
    bool known;
    std::string callsign;
    int scene;
    bool flying;
    std::string mission;
    Pilot() : known(false), scene(-1), flying(false) {}
};
enum { kMaxSeats = 8, kPresenceEveryMs = 3000, kCallToActionEveryMs = 2000 };

struct TieNet {
    bool host;
    int self;                   // our player number (a guest: -1 until the host's hello)
    int seats;
    std::string callsign;
    Listener *listener;         // the host's
    std::vector<Peer *> peers;
    Bit64u frame;
    double lastReliableAt;
    Puppet puppets[120];
    int playerFg;               // this mission's player flight group, and the number (1-based) of the host's craft in it
    int hostCraft;
    std::vector<int> fgObjects; // the group's craft by number (1-based -> index 0..), as object slots
    bool wasFlying;
    Pilot pilots[kMaxSeats];
    int sentScene;              // (our presence as last sent, and when)
    bool sentFlying;
    std::string sentMission;
    double sentAt;
    std::string callToAction;   // the line shown while the host flies and we do not ("": none)
    TieNet() : host(false), self(-1), seats(2), listener(NULL), frame(0), lastReliableAt(0), playerFg(kPlayerFgNone),
               hostCraft(1), wasFlying(false), sentScene(-2), sentFlying(false), sentAt(-1e9) {
        memset(puppets, 0, sizeof(puppets));
    }
    ~TieNet() {
        for (size_t i = 0; i < peers.size(); i++) {
            delete peers[i];
        }
        delete listener;
    }
};

TieNet *g_net = NULL;
double g_nextTryMs = 0;
double g_noticeAt = -1;

void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "TIE: %s", text.c_str());
}

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    if (env && env[0]) {
        return env;
    }
    return "Pilot";
}

bool configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') {
        return false;
    }
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

// The room joined or the port opened; the hello sent (a guest).
bool start() {
    net_config.reset_from_env();
    TieNet *n = new TieNet();
    n->callsign = own_callsign();
    n->seats = net_config.lobby_players;
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
            n->seats = g_lobby->max_players();
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
            wclog(0, "TIE: could not reach the host at %s", where.c_str());
            delete n;
            return false;
        }
        Peer *p = new Peer();
        p->conn.adopt(s);
        p->player = 0;
        n->peers.push_back(p);
        n->host = false;
        NetworkMessage msg;
        msg.mutable_tie_hello()->set_callsign(n->callsign);
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
            wclog(0, "TIE: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
    }
    g_net = n;
    wclog(1, "TIE: %s at %s, %d seats, callsign %s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats, n->callsign.c_str());
    if (n->host) {
        notice("Room open: your flight is the host's; guests fly the same mission with you");
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

// A frame of craft from a peer: into the puppets, and (the host) on to the
// other guests with the sender's number.
void take_frame(Peer *from, const TieFrame &f) {
    const int owner = g_net->host ? from->player : (f.has_player() ? (int)f.player() : 0);
    if (owner == g_net->self) {
        return;
    }
    from->lastFrame = f.frame();
    from->lastFrameAt = PIC_FullIndex();
    if (!g_net->host || owner == from->player) {
        from->flying = f.flying();
    }
    for (int i = 0; i < f.craft_size(); i++) {
        const TieCraft &c = f.craft(i);
        if (c.obj() >= 120) {
            continue;
        }
        Puppet &p = g_net->puppets[c.obj()];
        p.used = true;
        p.owner = owner;
        p.fresh = true;
        p.state = c;
        p.at = PIC_FullIndex();
    }
    if (g_net->host) {
        NetworkMessage msg;
        *msg.mutable_tie_frame() = f;
        msg.mutable_tie_frame()->set_player((Bit32u)owner);
        for (size_t i = 0; i < g_net->peers.size(); i++) {
            Peer *to = g_net->peers[i];
            if (to != from && to->hello) {
                to->conn.send_best_effort(msg);
            }
        }
    }
}

// A mission file's name in words: the Combat Chamber's (H, the craft's letter, the number, W or M), a battle's (B<n>M<m>),
// the training course.
std::string mission_words(const std::string &raw) {
    std::string m;
    for (size_t i = 0; i < raw.size(); i++) {
        m += (char)toupper((unsigned char)raw[i]);
    }
    static const struct { char letter; const char *craft; } kCraft[] = {
        { 'F', "T/F" }, { 'I', "T/I" }, { 'B', "T/B" }, { 'A', "T/A" }, { 'D', "T/D" }, { 'G', "Gunboat" }, { 'M', "Missile Boat" },
    };
    if (m.size() >= 3 && m[0] == 'H' && isdigit((unsigned char)m[2])) {
        for (size_t k = 0; k < sizeof(kCraft) / sizeof(kCraft[0]); k++) {
            if (kCraft[k].letter == m[1]) {
                return std::string("the Combat Chamber's ") + kCraft[k].craft + " mission " + m[2];
            }
        }
    }
    int battle = 0, mission = 0;
    if (sscanf(m.c_str(), "B%dM%d", &battle, &mission) == 2) {
        char out[64];
        snprintf(out, sizeof(out), "battle %d, mission %d", battle, mission);
        return out;
    }
    if (m == "TRAIN") {
        return "the training course";
    }
    return m.empty() ? std::string("a mission") : "mission " + m;
}

// Where a scene is, in words ("" for the scenes nobody needs told of: the opening, a cutscene).
const char *scene_words(int scene) {
    if (scene >= 2 && scene <= 4) return "flying";
    if (scene == 100) return "at the registration desk";
    if (scene == 110) return "on the concourse";
    if (scene >= 120 && scene <= 123) return "in the training simulator";
    if (scene >= 130 && scene <= 132) return "in the Combat Chamber";
    if (scene >= 133 && scene <= 137) return "at a Combat Chamber briefing";
    if (scene == 140) return "in the film room";
    if (scene == 150) return "in the tech room";
    if (scene == 160 || scene == 170) return "at the tour desk";
    if (scene >= 179 && scene <= 183) return "at a briefing";
    if (scene >= 190 && scene <= 192) return "at a debriefing";
    if (scene == 275) return "arming a ship";
    return "";
}

std::string pilot_name(int seat) {
    if (seat >= 0 && seat < kMaxSeats && !g_net->pilots[seat].callsign.empty()) {
        return g_net->pilots[seat].callsign;
    }
    char n[32];
    snprintf(n, sizeof(n), seat == 0 ? "The host" : "Pilot %d", seat + 1);
    return n;
}

// Another pilot's presence: kept, and its changes said (a launch, a flight over, a newcomer).
void take_presence(const TiePresence &pr, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) {
        return;
    }
    Pilot &p = g_net->pilots[seat];
    const bool first = !p.known;
    const bool launched = pr.flying() && (!p.flying || first);
    const bool landed = !pr.flying() && p.flying && !first;
    p.known = true;
    if (!pr.callsign().empty()) {
        p.callsign = pr.callsign();
    }
    p.scene = pr.scene();
    p.flying = pr.flying();
    p.mission = pr.mission();
    if (launched) {
        notice(pilot_name(seat) + " launched: " + mission_words(p.mission));
    } else if (landed) {
        notice(pilot_name(seat) + "'s flight is over");
    } else if (first && !g_net->host && seat != 0) {
        const char *where = scene_words(p.scene);
        notice(pilot_name(seat) + " is in the room" + (where[0] ? std::string(", ") + where : std::string("")));
    }
}

// Ours, when it changes and every few seconds: the host's to every guest, a guest's to the host (who relays it).
void presence_tick(double now) {
    if (g_net->self < 0) {
        return;
    }
    const int scene = tie_scene();
    const bool flying = tie_in_flight();
    const std::string mission = tie_mission_name();
    if (scene == g_net->sentScene && flying == g_net->sentFlying && mission == g_net->sentMission && now - g_net->sentAt < kPresenceEveryMs) {
        return;
    }
    g_net->sentScene = scene;
    g_net->sentFlying = flying;
    g_net->sentMission = mission;
    g_net->sentAt = now;
    NetworkMessage msg;
    TiePresence *pr = msg.mutable_tie_presence();
    pr->set_player((Bit32u)g_net->self);
    pr->set_callsign(g_net->callsign);
    pr->set_scene(scene);
    pr->set_flying(flying);
    pr->set_mission(mission);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
}

// The call to action: a guest out of the flight while the host flies, or is at a briefing, is told which mission to
// launch, and the line stays (said again every few seconds) until it flies or the host is back.
void call_to_action(double now) {
    if (g_net->host) {
        return;
    }
    const Pilot &h = g_net->pilots[0];
    std::string line;
    const bool briefing = h.known && ((h.scene >= 133 && h.scene <= 137) || (h.scene >= 179 && h.scene <= 183));
    if (h.known && !tie_in_flight() && (h.flying || briefing)) {
        line = pilot_name(0) + (h.flying ? " is flying " : " is at the briefing for ") + mission_words(h.mission) + ": launch it to fly together";
    }
    static double shownAt = -1e9;
    if (line.empty()) {
        if (!g_net->callToAction.empty() && incoming_text == g_net->callToAction) {
            incoming_text = "";
        }
        g_net->callToAction.clear();
        return;
    }
    if (line != g_net->callToAction || now - shownAt >= kCallToActionEveryMs) {
        if (line != g_net->callToAction) {
            wclog(1, "TIE: %s", line.c_str());
        }
        g_net->callToAction = line;
        incoming_text = line;
        g_noticeAt = now;
        shownAt = now;
    }
}

void handle(Peer *p, NetworkMessage &msg) {
    if (msg.has_tie_hello()) {
        const TieHello &h = msg.tie_hello();
        if (g_net->host) {
            if (p->hello) {
                return;
            }
            int player = free_player();
            if (player < 0) {
                wclog(1, "TIE: %s came in with no seat left: turned away", h.callsign().c_str());
                p->conn.close();
                return;
            }
            p->player = player;
            p->callsign = h.callsign();
            p->hello = true;
            if (player < kMaxSeats) {
                g_net->pilots[player].callsign = h.callsign();
            }
            g_net->sentAt = -1e9;   // (our presence to the newcomer at once)
            NetworkMessage reply;
            reply.mutable_tie_hello()->set_callsign(g_net->callsign);
            reply.mutable_tie_hello()->set_player((Bit32u)player);
            reply.mutable_tie_hello()->set_seats((Bit32u)g_net->seats);
            p->conn.send(reply);
            char line[160];
            snprintf(line, sizeof(line), "%s joined as pilot %d of %d: everybody flies the same mission", p->callsign.c_str(), player + 1,
                     g_net->seats);
            notice(line);
        } else {
            p->callsign = h.callsign();
            p->hello = true;
            g_net->self = h.has_player() ? (int)h.player() : 1;
            g_net->pilots[0].callsign = h.callsign();
            if (h.has_seats() && h.seats() >= 2) {
                g_net->seats = (int)h.seats();
            }
            char line[160];
            snprintf(line, sizeof(line), "In %s's room as pilot %d of %d: fly the mission the host flies", p->callsign.c_str(), g_net->self + 1,
                     g_net->seats);
            notice(line);
        }
    } else if (msg.has_tie_frame()) {
        static unsigned seen = 0;
        if (++seen <= 3) {
            wclog(2, "TIE: a frame from pilot %d (hello %d): frame %llu, %d craft, flying %d", p->player + 1, (int)p->hello,
                  (unsigned long long)msg.tie_frame().frame(), msg.tie_frame().craft_size(), (int)msg.tie_frame().flying());
        }
        if (p->hello) {
            take_frame(p, msg.tie_frame());
        }
    } else if (msg.has_tie_presence()) {
        if (!p->hello) {
            return;
        }
        const int seat = g_net->host ? p->player : (msg.tie_presence().has_player() ? (int)msg.tie_presence().player() : 0);
        take_presence(msg.tie_presence(), seat);
        if (g_net->host) {
            // relayed to the other guests with the sender's seat
            NetworkMessage out;
            *out.mutable_tie_presence() = msg.tie_presence();
            out.mutable_tie_presence()->set_player((Bit32u)p->player);
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
        for (int n = 0; n < 64 && p->conn.is_open(); n++) {
            RecvStatus st = p->conn.poll_best_effort(msg);
            if (!st.ok()) {
                break;
            }
            handle(p, msg);
        }
    }
    // the gone
    for (size_t i = 0; i < g_net->peers.size();) {
        Peer *p = g_net->peers[i];
        if (p->conn.is_open()) {
            i++;
            continue;
        }
        if (p->hello) {
            notice(p->callsign + (g_net->host ? " left the room" : " (the host) is gone: flying on alone"));
            if (p->player >= 0 && p->player < kMaxSeats) {
                g_net->pilots[p->player] = Pilot();
            }
        }
        for (int o = 0; o < 120; o++) {
            if (g_net->puppets[o].used && g_net->puppets[o].owner == p->player) {
                g_net->puppets[o].used = false;
            }
        }
        delete p;
        g_net->peers.erase(g_net->peers.begin() + i);
    }
}

void fill_craft(TieCraft *c, int obj) {
    const Bit32u o = tie::kObjects + obj * tie::kObjectSize;
    c->set_obj((Bit32u)obj);
    c->set_owner((Bit32u)g_net->self);
    c->set_type(tie_r8(o + 4));
    c->set_x((Bit32s)tie_r32(o + 6));
    c->set_y((Bit32s)tie_r32(o + 0xA));
    c->set_z((Bit32s)tie_r32(o + 0xE));
    c->set_heading(tie_r16(o + 0x1E));
    c->set_pitch(tie_r16(o + 0x20));
    c->set_roll(tie_r16(o + 0x22));
    c->set_speed((Bit16s)tie_r16(o + 0x26));
    c->set_mx((Bit16s)tie_r16(o + 0x3A));
    c->set_my((Bit16s)tie_r16(o + 0x3C));
    c->set_mz((Bit16s)tie_r16(o + 0x3E));
    char axes[18];
    for (int k = 0; k < 18; k++) {
        axes[k] = (char)tie_r8(o + 0x42 + k);
    }
    c->set_axes(std::string(axes, sizeof(axes)));
}

// A puppet into the game, just before the game moves its objects
// (move_moveobjects): whatever this machine's AI and dynamics did to it this
// frame is overwritten, and the game's own integration then carries it one
// frame along the owner's move vector at the owner's speed.  The place is
// written only from a report not yet written: between reports the game's own
// motion is the dead reckoning (a report every frame near, fewer far).
void write_puppet(Puppet &p) {
    const TieCraft &c = p.state;
    const int obj = (int)c.obj();
    const Bit32u o = tie::kObjects + obj * tie::kObjectSize;
    if (tie_r8(o + 4) == 0 || c.type() == 0) {
        return;             // (not there on this machine, or gone on its owner's: nothing to move)
    }
    if (p.fresh) {
        tie_w32(o + 6, (Bit32u)c.x());
        tie_w32(o + 0xA, (Bit32u)c.y());
        tie_w32(o + 0xE, (Bit32u)c.z());
        p.fresh = false;
    }
    tie_w16(o + 0x1E, (Bit16u)c.heading());
    tie_w16(o + 0x20, (Bit16u)c.pitch());
    tie_w16(o + 0x22, (Bit16u)c.roll());
    tie_w16(o + 0x26, (Bit16u)(Bit16s)c.speed());
    tie_w16(o + 0x3A, (Bit16u)(Bit16s)c.mx());
    tie_w16(o + 0x3C, (Bit16u)(Bit16s)c.my());
    tie_w16(o + 0x3E, (Bit16u)(Bit16s)c.mz());
    const std::string &axes = c.axes();
    for (size_t k = 0; k < axes.size() && k < 18; k++) {
        tie_w8(o + 0x42 + (Bit32u)k, (Bit8u)axes[k]);
    }
}

// This machine's AI kept off a puppet: the planner runs a craft only when
// its countdown (CraftData +0x2A, ai_update_rate_copy) has run out, so it is
// kept full.  The craft's own record is where its object points (+0x54, a
// linear address).
void hold_ai(int obj) {
    const Bit32u o = tie::kObjects + obj * tie::kObjectSize;
    const Bit32u craft = tie_r32(o + 0x54);
    if (obj < 32 && craft >= 0x100000) {
        mem_writew(craft + 0x2A, 0x7FFF);
    }
}

double wall_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)(tv.tv_sec % 100000) * 1000.0 + tv.tv_usec / 1000.0;
}

// The player flight group's craft as the game made them: the objects of
// that group in slot order are its craft 1, 2, ... (the mission's own
// setup makes them so on every machine).
void map_group() {
    if (g_net->playerFg == kPlayerFgNone || !g_net->fgObjects.empty()) {
        return;
    }
    for (int i = 0; i < 32; i++) {
        const Bit32u o = tie::kObjects + i * tie::kObjectSize;
        if (tie_r8(o + 4) != 0 && tie_r8(o + 0x35) == (Bit8u)g_net->playerFg) {
            g_net->fgObjects.push_back(i);
        }
    }
    if (g_net->fgObjects.empty()) {
        return;
    }
    std::string line;
    for (size_t k = 0; k < g_net->fgObjects.size(); k++) {
        char one[32];
        snprintf(one, sizeof(one), " #%d:obj%d", (int)k + 1, g_net->fgObjects[k]);
        line += one;
    }
    wclog(1, "TIE: the player flight group %d has %u craft (the host's #%d, ours obj %d):%s", g_net->playerFg,
          (unsigned)g_net->fgObjects.size(), g_net->hostCraft, tie_player_object(), line.c_str());
}

// A line now and then (WCNET_TIE_REPORT_MS, a second by default) of our craft and of the others' as this game has
// them, stamped with the wall clock, so two machines' lines line up by it.  Taken at the top of a flight frame: what
// the last frame drew (between a frame's dynamics and its move a puppet's attitude is this machine's own).
void report_frame(double now) {
    static double reportAt = 0;
    static int reportMs = -1;
    if (reportMs < 0) {
        const char *env = getenv("WCNET_TIE_REPORT_MS");
        reportMs = env && atoi(env) > 0 ? atoi(env) : 1000;
    }
    if (now - reportAt >= reportMs) {
        reportAt = now;
        std::string line;
        const int own = tie_player_object();
        for (int o = 0; o < 120; o++) {
            const Puppet &p = g_net->puppets[o];
            if (!p.used && o != own) {
                continue;
            }
            const Bit32u ob = tie::kObjects + o * tie::kObjectSize;
            char one[200];
            if (o == own) {
                snprintf(one, sizeof(one), " own obj %d: %d %d %d hpr %u %u %u speed %d;", o, (int)tie_r32(ob + 6), (int)tie_r32(ob + 0xA),
                         (int)tie_r32(ob + 0xE), tie_r16(ob + 0x1E), tie_r16(ob + 0x20), tie_r16(ob + 0x22), (Bit16s)tie_r16(ob + 0x26));
            } else {
                snprintf(one, sizeof(one), " obj %d of pilot %d: here %d %d %d hpr %u %u %u speed %d type %u, reported %d %d %d %.0f ms ago;", o,
                         p.owner + 1, (int)tie_r32(ob + 6), (int)tie_r32(ob + 0xA), (int)tie_r32(ob + 0xE), tie_r16(ob + 0x1E), tie_r16(ob + 0x20),
                         tie_r16(ob + 0x22), (Bit16s)tie_r16(ob + 0x26), tie_r8(ob + 4), p.state.x(), p.state.y(), p.state.z(), now - p.at);
            }
            line += one;
        }
        wclog(1, "TIE: wall %.0f frame %llu as pilot %d, %u peers;%s", wall_ms(), (unsigned long long)g_net->frame, g_net->self + 1,
              (unsigned)g_net->peers.size(), line.c_str());
    }
}

}  // namespace

void tie_session_tick() {
    const double now = PIC_FullIndex();
    if (g_noticeAt >= 0 && now - g_noticeAt > kNoticeMs && !tie_in_flight()) {
        g_noticeAt = -1;
        incoming_text = "";     // (a call to action standing is said again by call_to_action)
    }
    if (!tie_found()) {
        if (g_net) {
            tie_session_shutdown();
        }
        return;
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
            wclog(1, "TIE: a guest at %s", s->describe().c_str());
        }
    }
    poll_peers();
    presence_tick(now);
    call_to_action(now);
    // the flight's end seen here too (the frame hook stops coming)
    const bool flying = tie_in_flight();
    if (g_net->wasFlying && !flying) {
        tie_session_flight_ended();
    }
    g_net->wasFlying = flying;
}

void tie_session_frame() {
    if (!g_net || g_net->self < 0 || !tie_in_flight()) {
        return;
    }
    poll_peers();       // (what came in since the last frame: written before this frame's move)
    map_group();
    report_frame(PIC_FullIndex());
    const int own = tie_player_object();
    if (own < 0 || own >= 120) {
        return;
    }
    const double now = PIC_FullIndex();
    NetworkMessage msg;
    TieFrame *f = msg.mutable_tie_frame();
    f->set_player((Bit32u)g_net->self);
    f->set_frame(++g_net->frame);
    f->set_ticks(tie_r16(tie::kFrameticks));
    f->set_flying(true);
    fill_craft(f->add_craft(), own);
    const bool reliable = now - g_net->lastReliableAt >= kReliableEveryMs;
    if (reliable) {
        g_net->lastReliableAt = now;
    }
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (!p->hello || !p->conn.is_open()) {
            continue;
        }
        if (reliable) {
            p->conn.send(msg);
        } else {
            p->conn.send_best_effort(msg);
        }
    }
    for (int o = 0; o < 120; o++) {
        const Puppet &p = g_net->puppets[o];
        if (p.used && p.owner != g_net->self && o != own) {
            hold_ai(o);     // (before the planner runs this frame)
        }
    }
}

void tie_session_before_move() {
    if (!g_net || g_net->self < 0 || !tie_in_flight()) {
        return;
    }
    const int own = tie_player_object();
    for (int o = 0; o < 120; o++) {
        Puppet &p = g_net->puppets[o];
        if (p.used && p.owner != g_net->self && o != own) {
            write_puppet(p);
        }
    }
}

void tie_session_mission_create() {
    if (!g_net) {
        return;
    }
    g_net->fgObjects.clear();
    g_net->playerFg = kPlayerFgNone;
    memset(g_net->puppets, 0, sizeof(g_net->puppets));
    const int numFg = (Bit16s)tie_r16(tie::kMissionHeader);
    for (int i = 0; i < numFg && i < 48; i++) {
        const Bit32u fg = tie::kFlightGroups + i * tie::kFlightGroupSize;
        const int playerCraft = tie_r8(fg + 0x42);
        if (playerCraft == 0) {
            continue;
        }
        g_net->playerFg = i;
        g_net->hostCraft = playerCraft;
        int count = tie_r8(fg + 0x33);
        if (count < g_net->seats) {
            tie_w8(fg + 0x33, (Bit8u)g_net->seats);
        }
        if (!g_net->host && g_net->self > 0) {
            // ours is the self'th craft that is not the host's
            int mine = 0, seen = 0;
            for (int k = 1; k <= (count < g_net->seats ? g_net->seats : count); k++) {
                if (k == playerCraft) {
                    continue;
                }
                if (++seen == g_net->self) {
                    mine = k;
                    break;
                }
            }
            if (mine) {
                tie_w8(fg + 0x42, (Bit8u)mine);
            }
            wclog(1, "TIE: mission set up: the player flight group is %d of %d, %d craft (%d before), the host flies #%d, we fly #%d", i, numFg,
                  tie_r8(fg + 0x33), count, playerCraft, mine);
        } else {
            wclog(1, "TIE: mission set up: the player flight group is %d of %d, %d craft (%d before), we fly #%d as the host", i, numFg,
                  tie_r8(fg + 0x33), count, playerCraft);
        }
        break;
    }
}

void tie_session_flight_ended() {
    if (!g_net) {
        return;
    }
    wclog(1, "TIE: the flight is over here");
    NetworkMessage msg;
    TieFrame *f = msg.mutable_tie_frame();
    f->set_player((Bit32u)g_net->self);
    f->set_frame(++g_net->frame);
    f->set_flying(false);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
    g_net->fgObjects.clear();
    g_net->playerFg = kPlayerFgNone;
    memset(g_net->puppets, 0, sizeof(g_net->puppets));
}

void tie_session_shutdown() {
    if (!g_net) {
        return;
    }
    wclog(1, "TIE: session closed");
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

bool tie_session_active() { return g_net != NULL && (g_net->host || g_net->self >= 0); }

bool tie_is_puppet(int obj) {
    return g_net != NULL && obj >= 0 && obj < 120 && g_net->puppets[obj].used && g_net->puppets[obj].owner != g_net->self;
}

}  // namespace wc
