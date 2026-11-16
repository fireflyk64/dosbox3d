/*
 *  Wolfenstein 3-D's session (wcnet_wolf.h, docs/wolf3d.md): who plays, and the level kept the same for all.
 *
 *  The host is seat 0 (the room's, or a rig's TCP listener); a guest opens a stream to it, and everything goes through
 *  the host.  The room plays one level at a time, the host's: its layer keeps that level's story under a serial
 *  number (which actors have fallen and where, who thinks for which, the walls pushed, the keys found) and gives it
 *  to a player's game whenever that game comes into the level: at the start, late, or back from the dead.  A guest's
 *  game is made to load the room's level; one out of step is sent there by the game's own level warp.
 *
 *  Actors.  Every machine's game makes the same actors from the map (the order is their number).  Until one has
 *  noticed a player it stands or walks its round on every machine by itself ("free").  The machine whose player it
 *  notices claims it (the host says who was first) and from then on thinks for it: it chases and shoots that
 *  machine's player, and its place and state go to the others many times a second, where the game's DoActor passes
 *  it by and it is shown.  A hit on a shown actor goes to its owner's game (the game's DamageActor called there);
 *  when it falls the owner says so and every game runs its own KillActor: the fall, the points, what it drops.  An
 *  owner whose frames stop (dead, in a menu, at the level's end) gives its actors up; the host hands them to the
 *  nearest player still playing.
 *
 *  Doors opened, walls pushed and keys picked up are told and done in every game (the pick-ups themselves are each
 *  game's own).  The level's end is everybody's: the exit one player takes ends the level in every game the game's
 *  own way (its tally, the next floor).  Lives do not run out: the page's rooms start the game by TEDLEVEL, where it
 *  takes none.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <string>
#include <vector>
#include <algorithm>
#include "wcnet_wolf.h"
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
    kRetryMs = 3000,
    kStateEveryMs = 66,         // a player's reports: 15 a second
    kQuietMs = 1500,            // a player not heard for this long is not shown
    kHoldMs = 15000,            // a guest's first level waits this long for the host's
    kSnapAgainMs = 2000,
    kActorsPerState = 48,
};

enum EventKind {
    EV_CLAIM = 1,       // a: the actor.  A guest's: it noticed our player
    EV_OWNER,           // a: the actor, b: its owner's seat + 1 (0: nobody's).  The host's
    EV_DAMAGE,          // a: the actor, b: the damage.  To its owner
    EV_DIED,            // a: the actor, b, c: where it fell
    EV_DOOR,            // a: the door opened
    EV_PUSH,            // a, b: the wall's tile, c: the way, d: the pusher's area
    EV_KEY,             // a: the keys found (bits)
    EV_DONE,            // a: the frame loop's end (1 completed, 9 the secret way, 6 the episode won)
    EV_SNAPREQ,         // a: our level's signature.  The level's story asked for
    EV_OUT,             // a: why our frames stopped (2: killed)
};

struct Peer {
    Connection conn;
    int seat;
    std::string callsign, build;
    bool hello, inPlay, refused;
    double heardAt;
    Peer() : seat(-1), hello(false), inPlay(false), refused(false), heardAt(-1e9) {}
};

struct WolfNet {
    bool host;
    int self, seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    WolfNet() : host(false), self(-1), seats(kWolfMaxPlayers), listener(NULL) {}
    ~WolfNet() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

struct Room {
    Bit32u serial;
    WolfLevel level;
    int exit;
    Bit32u sig;
    Room() : serial(0), exit(0), sig(0) {}
};

struct Player {
    bool known, inPlay, has;
    std::string callsign;
    Bit32u serial;
    Bit32u x[2], y[2];          // its last two reports ([1] the later), and when
    Bit32u clock[2];
    double at;                  // ... the later came
    int angle, health;
    double heardAt, movedAt;
    Player() : known(false), inPlay(false), has(false), serial(0), at(0), angle(0), health(0), heardAt(-1e9), movedAt(-1e9) {
        x[0] = x[1] = y[0] = y[1] = 0;
        clock[0] = clock[1] = 0;
    }
};

struct Report {
    bool has;
    int from;
    WolfAct a[2];
    Bit32u clock[2];
    double at;
    Report() : has(false), from(-1), at(0) { clock[0] = clock[1] = 0; }
};

struct Push { int x, y, dir, area; };

WolfNet *g_net = NULL;
double g_nextTryMs = 0, g_pollAt = -1e9, g_helloAt = -1e9, g_stateAt = -1e9, g_roomSaidAt = -1e9;
bool g_tried = false, g_alone = false;
std::string g_hostNote;

Room g_room;                            // the level the room plays (the host's word; the host's own)
Player g_players[kWolfMaxPlayers];
// the level's story: on the host the room's (kept while its own game is elsewhere), on a guest what it has heard
int g_owner[kWolfMaxActors];
bool g_dead[kWolfMaxActors];
Bit32u g_deadX[kWolfMaxActors], g_deadY[kWolfMaxActors];
std::vector<Push> g_pushes;
int g_keys = 0;

// this machine's game in it
bool g_synced = false, g_remoteExit = false, g_snapAsked = false, g_snapHere = false, g_refusedLevel = false;
Bit32u g_mySerial = 0;
double g_snapAskedAt = -1e9, g_holdSince = -1, g_pushExpectUntil = -1e9;
WolfSnap g_snap;
bool g_pending[kWolfMaxActors], g_aliveWas[kWolfMaxActors];
int g_hpWas[kWolfMaxActors], g_claimHp[kWolfMaxActors];
Report g_report[kWolfMaxActors];
int g_doorWas[kWolfMaxDoors];
bool g_pushWas = false;
int g_keysWas = 0;
unsigned g_statesSent = 0, g_statesGot = 0, g_eventsSent = 0, g_eventsGot = 0, g_claims = 0, g_killsTold = 0, g_killsHeard = 0;
double g_loggedAt = 0;

std::string own_callsign() {
    const char *env = getenv("WCCALLSIGN");
    return env && env[0] ? env : "Player";
}

bool is_host() { return g_net && g_net->host; }
int self_seat() { return g_net ? g_net->self : -1; }

bool start() {
    net_config.reset_from_env();
    WolfNet *n = new WolfNet();
    n->callsign = own_callsign();
    n->seats = net_config.lobby_players > 0 ? std::min((int)kWolfMaxPlayers, net_config.lobby_players) : kWolfMaxPlayers;
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
        if (g_lobby->max_players() > 0) n->seats = std::min((int)kWolfMaxPlayers, g_lobby->max_players());
        where = "room " + g_lobby->code();
#else
        delete n;
        return false;
#endif
    } else {
#ifdef __EMSCRIPTEN__
        wclog(0, "Wolfenstein 3-D: the browser build has no TCP transport: set WCROOM (a room code) to play");
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
            wclog(0, "Wolfenstein 3-D: could not reach the host at %s", where.c_str());
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
            wclog(0, "Wolfenstein 3-D: could not listen on %s", where.c_str());
            delete n;
            return false;
        }
        n->listener = l;
        n->host = true;
        n->self = 0;
    }
    g_net = n;
    g_helloAt = -1e9;
    g_hostNote.clear();
    for (int s = 0; s < kWolfMaxPlayers; s++) g_players[s] = Player();
    if (n->host) {
        g_players[0].known = true;
        g_players[0].callsign = n->callsign;
    } else {
        g_room = Room();
    }
    wclog(1, "Wolfenstein 3-D: %s at %s, %d seats, callsign %s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats, n->callsign.c_str());
    return true;
}

Peer *peer_of_seat(int seat) {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (p->seat == seat && p->hello && !p->refused && p->conn.is_open()) return p;
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

void clear_story() {
    for (int i = 0; i < kWolfMaxActors; i++) {
        g_owner[i] = -1;
        g_dead[i] = false;
        g_deadX[i] = g_deadY[i] = 0;
    }
    g_pushes.clear();
    g_keys = 0;
}

NetworkMessage make_event(int kind, int seat, int a, int b, int c, int d, Bit32u serial) {
    NetworkMessage msg;
    WolfEvent *e = msg.mutable_wolf_event();
    e->set_serial(serial);
    e->set_seat((Bit32u)seat);
    e->set_kind((Bit32u)kind);
    e->set_a(a);
    if (b) e->set_b(b);
    if (c) e->set_c(c);
    if (d) e->set_d(d);
    return msg;
}

// (the host's) to every guest taken, but one
void host_send_all(const NetworkMessage &msg, int exceptSeat, bool bestEffort = false) {
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (!p->hello || p->refused || p->seat == exceptSeat || !p->conn.is_open()) continue;
        if (bestEffort) p->conn.send_best_effort(msg);
        else p->conn.send(msg);
    }
}

void fill_room(WolfRoom *r) {
    r->set_serial(g_room.serial);
    r->set_episode((Bit32u)g_room.level.episode);
    r->set_map((Bit32u)g_room.level.map);
    r->set_difficulty((Bit32u)g_room.level.difficulty);
    r->set_exit((Bit32u)g_room.exit);
    r->set_sig(g_room.sig);
    for (int s = 0; s < kWolfMaxPlayers; s++) {
        if (!g_players[s].known) continue;
        WolfPlayer *p = r->add_player();
        p->set_seat((Bit32u)s);
        p->set_callsign(g_players[s].callsign);
        p->set_in_play(g_players[s].inPlay);
    }
}

void host_say_room() {
    if (!is_host()) return;
    NetworkMessage msg;
    fill_room(msg.mutable_wolf_room());
    host_send_all(msg, -1);
    g_roomSaidAt = PIC_FullIndex();
}

// (the host's) a level begun: its story empty, the one before ended by `exit`
void host_new_level(const WolfLevel &level, int exit) {
    if (!g_room.serial) g_room.serial = ((Bit32u)time(NULL) & 0xFFFFF) << 8;
    g_room.serial++;
    g_room.level = level;
    g_room.exit = exit;
    g_room.sig = 0;
    clear_story();
    wclog(1, "Wolfenstein 3-D: the room's level %u: episode %d floor %d, difficulty %d%s", g_room.serial, level.episode + 1, level.map + 1,
          level.difficulty, exit == 1 ? " (the floor before completed)" : exit == 9 ? " (by the secret way)" : exit == 6 ? " (the episode won)" : "");
    host_say_room();
}

// The floor after one left by `exit` (the game's own rule, GameLoop's).
WolfLevel next_level(const WolfLevel &l, int exit) {
    static const int kBack[6] = { 1, 1, 7, 3, 5, 3 };     // (ElevatorBackTo: the floor after each episode's secret one)
    WolfLevel n = l;
    if (l.map == 9) n.map = kBack[std::max(0, std::min(5, l.episode))];
    else if (exit == 9) n.map = 9;
    else n.map = l.map + 1;
    return n;
}

void client_event(int kind, int seat, int a, int b, int c, int d);

// who is playing in the room's level, nearest to an actor (not `except`): -1 nobody
int nearest_player(int actor, int except) {
    WolfAct act;
    const bool have = wolf_found() && wolf_actor(actor, &act);
    int best = -1;
    double bestD = 1e30;
    const double now = PIC_FullIndex();
    for (int s = 0; s < kWolfMaxPlayers; s++) {
        const Player &p = g_players[s];
        if (s == except || !p.known || !p.inPlay || p.serial != g_room.serial) continue;
        if (s != self_seat() && now - p.heardAt > kQuietMs) continue;
        double d = 0;
        if (have && p.has) {
            const double dx = ((double)p.x[1] - act.x) / 65536.0, dy = ((double)p.y[1] - act.y) / 65536.0;
            d = dx * dx + dy * dy;
        }
        if (d < bestD) {
            bestD = d;
            best = s;
        }
    }
    return best;
}

// (the host's) who thinks for an actor, told to everybody (the host's own game included)
void host_set_owner(int actor, int seat) {
    g_owner[actor] = seat;
    host_send_all(make_event(EV_OWNER, 0, actor, seat + 1, 0, 0, g_room.serial), -1);
    client_event(EV_OWNER, 0, actor, seat + 1, 0, 0);
}

// (the host's) a player's frames have stopped: its actors to the nearest player still playing, or nobody's
void host_player_out(int seat) {
    if (seat >= 0 && seat < kWolfMaxPlayers) g_players[seat].inPlay = false;
    int given = 0;
    for (int i = 0; i < kWolfMaxActors; i++) {
        if (g_owner[i] != seat || g_dead[i]) continue;
        host_set_owner(i, nearest_player(i, seat));
        given++;
    }
    if (given) wclog(1, "Wolfenstein 3-D: %s out of play: %d of its actors handed on", g_players[seat].callsign.c_str(), given);
}

// (the host's) an event of a player's (a guest's as it comes, the host's own game's directly)
void host_event(int seat, int kind, int a, int b, int c, int d, Bit32u serial) {
    if (kind == EV_OUT) {
        host_player_out(seat);
        NetworkMessage msg = make_event(EV_OUT, seat, a, 0, 0, 0, g_room.serial);
        host_send_all(msg, seat);
        if (seat != 0) client_event(EV_OUT, seat, a, 0, 0, 0);
        host_say_room();
        return;
    }
    if (serial != g_room.serial || !g_room.serial) return;      // (of a level the room has left)
    switch (kind) {
    case EV_CLAIM:
        if (a < 0 || a >= kWolfMaxActors) return;
        if (!g_dead[a] && g_owner[a] < 0) {
            host_set_owner(a, seat);
        } else if (seat != 0) {
            Peer *p = peer_of_seat(seat);       // (somebody was first, or it has fallen: the claimant told)
            if (p) p->conn.send(make_event(g_dead[a] ? EV_DIED : EV_OWNER, 0, a, g_dead[a] ? (int)g_deadX[a] : g_owner[a] + 1,
                                           g_dead[a] ? (int)g_deadY[a] : 0, 0, g_room.serial));
        }
        break;
    case EV_DIED:
        // (the host's own game has written its fall into the story already: the same arrays)
        if (a < 0 || a >= kWolfMaxActors || (seat != 0 && g_dead[a])) return;
        g_dead[a] = true;
        g_deadX[a] = (Bit32u)b;
        g_deadY[a] = (Bit32u)c;
        g_owner[a] = -1;
        host_send_all(make_event(EV_DIED, seat, a, b, c, 0, g_room.serial), seat);
        if (seat != 0) client_event(EV_DIED, seat, a, b, c, 0);
        break;
    case EV_DAMAGE:
        if (a < 0 || a >= kWolfMaxActors || g_dead[a]) return;
        if (g_owner[a] < 0) {
            host_set_owner(a, seat);            // (nobody's any more: the one who shot it thinks for it now)
        } else if (g_owner[a] == 0) {
            client_event(EV_DAMAGE, seat, a, b, 0, 0);
        } else if (g_owner[a] != seat) {
            Peer *p = peer_of_seat(g_owner[a]);
            if (p) p->conn.send(make_event(EV_DAMAGE, seat, a, b, 0, 0, g_room.serial));
        }
        break;
    case EV_DOOR:
        host_send_all(make_event(EV_DOOR, seat, a, 0, 0, 0, g_room.serial), seat);
        if (seat != 0) client_event(EV_DOOR, seat, a, 0, 0, 0);
        break;
    case EV_PUSH: {
        if (seat != 0) {
            for (size_t i = 0; i < g_pushes.size(); i++) {
                if (g_pushes[i].x == a && g_pushes[i].y == b) return;
            }
            Push p = { a, b, c, d };
            g_pushes.push_back(p);
        }
        host_send_all(make_event(EV_PUSH, seat, a, b, c, d, g_room.serial), seat);
        if (seat != 0) client_event(EV_PUSH, seat, a, b, c, d);
        break;
    }
    case EV_KEY:
        if (seat != 0 && (g_keys | a) == g_keys) return;
        g_keys |= a;
        host_send_all(make_event(EV_KEY, seat, a, 0, 0, 0, g_room.serial), seat);
        if (seat != 0) client_event(EV_KEY, seat, a, 0, 0, 0);
        break;
    case EV_DONE:
        if (a != 1 && a != 9 && a != 6) return;
        wclog(1, "Wolfenstein 3-D: %s %s", g_players[seat].callsign.c_str(), a == 6 ? "has won the episode" : a == 9 ? "found the secret floor's lift" : "reached the lift");
        if (seat != 0) client_event(EV_DONE, seat, a, 0, 0, 0);
        host_send_all(make_event(EV_DONE, seat, a, 0, 0, 0, g_room.serial), seat);
        host_new_level(a == 6 ? g_room.level : next_level(g_room.level, a), a);
        break;
    case EV_SNAPREQ: {
        Peer *p = peer_of_seat(seat);
        if (!p) return;
        NetworkMessage msg;
        WolfSnap *s = msg.mutable_wolf_snap();
        s->set_serial(g_room.serial);
        s->set_sig(g_room.sig);
        for (int i = 0; i < kWolfMaxActors; i++) {
            if (g_dead[i]) {
                s->add_dead((Bit32u)i);
                s->add_dead(g_deadX[i]);
                s->add_dead(g_deadY[i]);
            } else if (g_owner[i] >= 0) {
                s->add_owner((Bit32u)i);
                s->add_owner((Bit32u)g_owner[i]);
            }
        }
        for (size_t i = 0; i < g_pushes.size(); i++) {
            s->add_push((Bit32u)g_pushes[i].x);
            s->add_push((Bit32u)g_pushes[i].y);
            s->add_push((Bit32u)g_pushes[i].dir);
            s->add_push((Bit32u)(g_pushes[i].area + 1));
        }
        s->set_keys((Bit32u)g_keys);
        if (g_synced && wolf_in_play()) {
            for (int dnum = 0; dnum < wolf_doors(); dnum++) {
                const int act = wolf_door_action(dnum);
                if (act == 0 || act == 2) s->add_door((Bit32u)dnum);
            }
        }
        p->conn.send(msg);
        wclog(1, "Wolfenstein 3-D: the level's story to %s: %d fallen, %d owned, %d walls pushed, keys %d", p->callsign.c_str(), s->dead_size() / 3,
              s->owner_size() / 2, s->push_size() / 4, g_keys);
        break;
    }
    default:
        break;
    }
}

// An event of ours: to the host (the host's own: straight to its log).
void send_event(int kind, int a, int b = 0, int c = 0, int d = 0) {
    if (!g_net) return;
    g_eventsSent++;
    const Bit32u serial = kind == EV_SNAPREQ ? g_room.serial : g_mySerial;
    if (g_net->host) {
        host_event(0, kind, a, b, c, d, serial);
    } else if (!g_net->peers.empty()) {
        g_net->peers[0]->conn.send(make_event(kind, g_net->self, a, b, c, d, serial));
    }
}

const char *name_of(int seat) {
    static std::string s;
    s = seat >= 0 && seat < kWolfMaxPlayers && g_players[seat].known && !g_players[seat].callsign.empty() ? g_players[seat].callsign : "A player";
    return s.c_str();
}

// An event for this machine's game (the host's word, or the host's own log's).
void client_event(int kind, int seat, int a, int b, int c, int d) {
    g_eventsGot++;
    const int self = self_seat();
    switch (kind) {
    case EV_OWNER: {
        if (a < 0 || a >= kWolfMaxActors) return;
        const int owner = b - 1;
        const bool pending = g_pending[a];
        g_pending[a] = false;
        g_owner[a] = owner;
        if (!g_synced || g_dead[a]) return;
        if (owner == self) {
            // ours to think for: from where its last owner had it
            if (wolf_actor_is_puppet(a) && g_report[a].has) wolf_actor_show(a, g_report[a].a[1], true);
            wolf_actor_puppet(a, false);
        } else if (owner >= 0) {
            WolfAct now;
            if (pending && wolf_actor(a, &now) && g_claimHp[a] > now.hp) {
                // (somebody was first: what we took off it while we thought it ours goes to them)
                send_event(EV_DAMAGE, a, g_claimHp[a] - now.hp);
            }
            if (g_report[a].from != owner) g_report[a].has = false;
            wolf_actor_puppet(a, true);
        } else {
            wolf_actor_puppet(a, false);        // (nobody's: every game its own again; whoever it goes for claims it)
        }
        break;
    }
    case EV_DAMAGE:
        if (g_synced && a >= 0 && a < kWolfMaxActors && !g_dead[a]) wolf_hurt(a, b);
        break;
    case EV_DIED:
        if (a < 0 || a >= kWolfMaxActors) return;
        g_killsHeard++;
        g_dead[a] = true;
        g_deadX[a] = (Bit32u)b;
        g_deadY[a] = (Bit32u)c;
        g_owner[a] = -1;
        g_pending[a] = false;
        if (g_synced) wolf_kill(a, (Bit32u)b, (Bit32u)c, false);
        break;
    case EV_DOOR:
        if (g_synced && a >= 0 && a < wolf_doors()) {
            wolf_door_open(a);
            g_doorWas[a] = wolf_door_action(a);
        }
        break;
    case EV_PUSH: {
        bool had = false;
        for (size_t i = 0; i < g_pushes.size(); i++) had = had || (g_pushes[i].x == a && g_pushes[i].y == b);
        if (!had) {
            Push p = { a, b, c, d };
            g_pushes.push_back(p);
        }
        if (g_synced) {
            g_pushExpectUntil = PIC_FullIndex() + 1500;
            wolf_push(a, b, c, d);
        }
        break;
    }
    case EV_KEY:
        g_keys |= a;
        if (g_synced) {
            g_keysWas |= a;
            wolf_give_keys(a);
            wolf_notice(std::string(name_of(seat)) + " found the " + ((a & 1) ? "gold" : "silver") + " key: yours too");
        }
        break;
    case EV_DONE:
        if (g_synced && wolf_in_play()) {
            g_remoteExit = true;
            g_synced = false;
            wolf_end_level(a);
            wolf_notice(std::string(name_of(seat)) + (a == 6 ? " has won the episode" : a == 9 ? " found the way to the secret floor" : " reached the lift"));
        }
        break;
    case EV_OUT:
        if (a == 2 && seat != self) wolf_notice(std::string(name_of(seat)) + " was killed");
        if (seat >= 0 && seat < kWolfMaxPlayers) g_players[seat].inPlay = false;
        break;
    default:
        break;
    }
}

void take_state(int seat, const WolfState &st) {
    if (seat < 0 || seat >= kWolfMaxPlayers || seat == self_seat()) return;
    g_statesGot++;
    const double now = PIC_FullIndex();
    Player &p = g_players[seat];
    p.heardAt = now;
    p.inPlay = st.in_play();
    p.serial = st.serial();
    p.angle = (int)st.angle();
    p.health = st.health();
    if (st.has_x()) {
        if (p.has && (p.x[1] != st.x() || p.y[1] != st.y())) p.movedAt = now;
        p.x[0] = p.has ? p.x[1] : st.x();
        p.y[0] = p.has ? p.y[1] : st.y();
        p.clock[0] = p.has ? p.clock[1] : st.clock();
        p.x[1] = st.x();
        p.y[1] = st.y();
        p.clock[1] = st.clock();
        p.at = now;
        p.has = true;
    }
    if (!g_room.serial || st.serial() != g_room.serial) return;
    for (int k = 0; k < st.actor_size(); k++) {
        const WolfActor &m = st.actor(k);
        const int i = (int)m.index();
        if (i < 0 || i >= kWolfMaxActors) continue;
        Report &r = g_report[i];
        WolfAct a;
        a.cls = 0;
        a.x = m.x();
        a.y = m.y();
        a.tilex = (int)m.tilex();
        a.tiley = (int)m.tiley();
        a.dir = (int)m.dir();
        a.state = (int)m.state() - 0x8000;
        a.ticcount = m.ticcount();
        a.flags = (int)m.flags();
        a.hp = m.hp();
        a.distance = m.distance();
        const bool again = r.has && r.from == seat;
        r.a[0] = again ? r.a[1] : a;
        r.clock[0] = again ? r.clock[1] : st.clock();
        r.a[1] = a;
        r.clock[1] = st.clock();
        r.at = now;
        r.from = seat;
        r.has = true;
    }
}

void refuse(Peer *p, const std::string &why) {
    p->refused = true;
    NetworkMessage msg;
    WolfHello *h = msg.mutable_wolf_hello();
    h->set_callsign(g_net->callsign);
    h->set_seat((Bit32u)p->seat);
    h->set_note(why);
    p->conn.send(msg);
    wclog(1, "Wolfenstein 3-D: %s is not taken: %s", p->callsign.c_str(), why.c_str());
}

void handle(Peer *p, NetworkMessage &msg) {
    const double now = PIC_FullIndex();
    if (msg.has_wolf_hello()) {
        const WolfHello &h = msg.wolf_hello();
        if (g_net->host) {
            if (!p->hello) {
                if (p->seat <= 0) p->seat = free_seat();
                if (p->seat <= 0 || p->seat >= kWolfMaxPlayers) {
                    wclog(1, "Wolfenstein 3-D: %s came in with no seat left: turned away", h.callsign().c_str());
                    p->conn.close();
                    return;
                }
                p->hello = true;
                p->callsign = h.callsign();
                p->build = h.build();
                wclog(1, "Wolfenstein 3-D: %s is in the room (seat %d)", h.callsign().c_str(), p->seat);
                if (!wolf_build().empty() && !p->build.empty() && p->build != wolf_build()) {
                    refuse(p, "Not the same Wolfenstein 3-D as " + g_net->callsign + "'s (another version of the program)");
                    return;
                }
                g_players[p->seat] = Player();
                g_players[p->seat].known = true;
                g_players[p->seat].callsign = p->callsign;
                NetworkMessage out;
                WolfHello *o = out.mutable_wolf_hello();
                o->set_callsign(g_net->callsign);
                o->set_seat((Bit32u)p->seat);
                p->conn.send(out);
                host_say_room();
                if (g_synced) wolf_notice(p->callsign + " has joined");
            }
            p->heardAt = now;
            p->inPlay = h.in_play();
        } else {
            p->hello = true;
            p->callsign = h.callsign();
            p->heardAt = now;
            if (h.has_seat()) g_net->self = (int)h.seat();
            if (h.has_note() && !h.note().empty() && g_hostNote != h.note()) {
                g_hostNote = h.note();
                g_alone = true;
                wolf_notice(g_hostNote + ": playing alone", 9000);
            }
        }
        return;
    }
    if (g_net->host && (!p->hello || p->refused)) return;
    if (msg.has_wolf_room()) {
        if (g_net->host) return;
        const WolfRoom &r = msg.wolf_room();
        const bool news = r.serial() != g_room.serial;
        g_room.serial = r.serial();
        g_room.level.episode = (int)r.episode();
        g_room.level.map = (int)r.map();
        g_room.level.difficulty = (int)r.difficulty();
        g_room.exit = (int)r.exit();
        g_room.sig = r.sig();
        bool seen[kWolfMaxPlayers] = { false, false, false, false };
        for (int k = 0; k < r.player_size(); k++) {
            const int s = (int)r.player(k).seat();
            if (s < 0 || s >= kWolfMaxPlayers) continue;
            seen[s] = true;
            if (!g_players[s].known && s != g_net->self && g_synced) wolf_notice(r.player(k).callsign() + " has joined");
            g_players[s].known = true;
            g_players[s].callsign = r.player(k).callsign();
            if (s == 0 || !r.player(k).in_play()) g_players[s].inPlay = r.player(k).in_play() && g_players[s].inPlay;
        }
        for (int s = 0; s < kWolfMaxPlayers; s++) {
            if (g_players[s].known && !seen[s]) {
                if (g_synced) wolf_notice(g_players[s].callsign + " has left");
                g_players[s] = Player();
            }
        }
        if (news) {
            wclog(1, "Wolfenstein 3-D: the room's level is %u: episode %d floor %d, difficulty %d", g_room.serial, g_room.level.episode + 1,
                  g_room.level.map + 1, g_room.level.difficulty);
            // (a new level's story begins empty; ours is asked for when our game is in it)
            clear_story();
            g_snapHere = false;
            g_snapAsked = false;
        }
        return;
    }
    if (msg.has_wolf_state()) {
        const WolfState &st = msg.wolf_state();
        const int seat = g_net->host ? p->seat : (int)st.seat();
        if (g_net->host) {
            NetworkMessage out;
            *out.mutable_wolf_state() = st;
            out.mutable_wolf_state()->set_seat((Bit32u)seat);
            host_send_all(out, seat, true);     // (passed on to the other guests)
        }
        take_state(seat, st);
        return;
    }
    if (msg.has_wolf_event()) {
        const WolfEvent &e = msg.wolf_event();
        if (g_net->host) {
            host_event(p->seat, (int)e.kind(), e.a(), e.b(), e.c(), e.d(), e.serial());
        } else if (e.kind() == EV_OUT || (e.serial() == g_room.serial && g_room.serial)) {
            client_event((int)e.kind(), (int)e.seat(), e.a(), e.b(), e.c(), e.d());
        }
        return;
    }
    if (msg.has_wolf_snap()) {
        if (g_net->host) return;
        g_snap = msg.wolf_snap();
        g_snapHere = true;
    }
}

void drop_room() {
    // (a guest's host gone: every actor this game's own again, nobody else to show)
    g_room = Room();
    g_synced = false;
    g_mySerial = 0;
    clear_story();
    if (wolf_found()) {
        for (int i = 0; i < wolf_actors(); i++) wolf_actor_puppet(i, false);
    }
    for (int s = 0; s < kWolfMaxPlayers; s++) g_players[s] = Player();
}

void poll_peers() {
    if (g_net->host && g_net->listener) {
        for (int n = 0; n < 4; n++) {
            Stream *s = g_net->listener->accept(false);
            if (!s) break;
            Peer *p = new Peer();
            p->seat = s->room_seat();
            wclog(1, "Wolfenstein 3-D: a guest at %s", s->describe().c_str());
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
        if (p->hello) wclog(1, "Wolfenstein 3-D: %s %s", p->callsign.c_str(), g_net->host ? "left the room" : "(the host) is gone");
        g_net->peers.erase(g_net->peers.begin() + i);
        if (g_net->host) {
            if (p->hello && !p->refused && p->seat > 0 && p->seat < kWolfMaxPlayers) {
                host_player_out(p->seat);
                if (g_synced) wolf_notice(p->callsign + " has left");
                g_players[p->seat] = Player();
                host_say_room();
            }
        } else {
            if (g_synced) wolf_notice("The host is gone: playing on alone", 8000);
            drop_room();
        }
        delete p;
    }
}

void send_hellos() {
    if (g_net->host) {
        if (PIC_FullIndex() - g_roomSaidAt >= kHelloEveryMs) host_say_room();
        return;
    }
    if (g_net->peers.empty()) return;
    NetworkMessage msg;
    WolfHello *h = msg.mutable_wolf_hello();
    h->set_callsign(g_net->callsign);
    h->set_build(wolf_build());
    h->set_in_play(wolf_in_play());
    g_net->peers[0]->conn.send(msg);
}

// --- this machine's game in the room's level ------------------------------------------------------------------------

void reset_level_state() {
    g_synced = false;
    g_remoteExit = false;
    g_snapAsked = false;
    for (int i = 0; i < kWolfMaxActors; i++) {
        g_pending[i] = false;
        g_report[i] = Report();
        g_hpWas[i] = 0;
        g_aliveWas[i] = false;
        g_claimHp[i] = 0;
    }
    WolfAct a;
    for (int i = 0; i < wolf_actors(); i++) {
        if (!wolf_actor(i, &a)) continue;
        g_hpWas[i] = a.hp;
        g_aliveWas[i] = (a.flags & WOLF_SHOOTABLE) != 0;
    }
    for (int d = 0; d < kWolfMaxDoors; d++) g_doorWas[d] = d < wolf_doors() ? wolf_door_action(d) : 1;
    int x, y, dir;
    g_pushWas = wolf_push_moving(&x, &y, &dir);
    g_keysWas = wolf_keys();
}

// The level's story put into our game (the host's own log, or the snapshot a guest was sent).  quiet: our game has
// just made the level (what fell before falls without a sound).
void apply_story(bool quiet) {
    WolfAct a;
    int fallen = 0, shown = 0;
    for (int i = 0; i < wolf_actors(); i++) {
        if (!wolf_actor(i, &a)) continue;
        if (g_dead[i]) {
            if (a.flags & WOLF_SHOOTABLE) {
                wolf_kill(i, g_deadX[i], g_deadY[i], quiet);
                fallen++;
            }
        } else if (g_owner[i] >= 0 && g_owner[i] != self_seat()) {
            wolf_actor_puppet(i, true);
            shown++;
        } else if (!quiet && wolf_actor_fighter(a.cls) && !(a.flags & WOLF_SHOOTABLE) && a.hp <= 0) {
            // (fallen in our game while it played by itself: the room is told)
            g_dead[i] = true;
            g_deadX[i] = a.x;
            g_deadY[i] = a.y;
            send_event(EV_DIED, i, (int)a.x, (int)a.y);
        }
    }
    for (size_t i = 0; i < g_pushes.size(); i++) wolf_push_done(g_pushes[i].x, g_pushes[i].y, g_pushes[i].dir, g_pushes[i].area);
    if (g_keys) wolf_give_keys(g_keys);
    g_keysWas = wolf_keys() | g_keys;
    wclog(1, "Wolfenstein 3-D: in step with the room's level %u: %d fallen before, %d thought for elsewhere, %d walls pushed, keys %d", g_room.serial,
          fallen, shown, (int)g_pushes.size(), g_keys);
}

void take_snapshot(bool quiet) {
    clear_story();
    for (int k = 0; k + 2 < g_snap.dead_size(); k += 3) {
        const int i = (int)g_snap.dead(k);
        if (i < 0 || i >= kWolfMaxActors) continue;
        g_dead[i] = true;
        g_deadX[i] = g_snap.dead(k + 1);
        g_deadY[i] = g_snap.dead(k + 2);
    }
    for (int k = 0; k + 1 < g_snap.owner_size(); k += 2) {
        const int i = (int)g_snap.owner(k);
        if (i >= 0 && i < kWolfMaxActors) g_owner[i] = (int)g_snap.owner(k + 1);
    }
    for (int k = 0; k + 3 < g_snap.push_size(); k += 4) {
        Push p = { (int)g_snap.push(k), (int)g_snap.push(k + 1), (int)g_snap.push(k + 2), (int)g_snap.push(k + 3) - 1 };
        g_pushes.push_back(p);
    }
    g_keys = (int)g_snap.keys();
    apply_story(quiet);
    for (int k = 0; k < g_snap.door_size(); k++) {
        const int d = (int)g_snap.door(k);
        if (d < 0 || d >= wolf_doors()) continue;
        wolf_door_open(d);
        g_doorWas[d] = wolf_door_action(d);
    }
}

// where a player or an actor is now, between its last two reports (one report behind: no guessing ahead)
void between(Bit32u x0, Bit32u y0, Bit32u x1, Bit32u y1, Bit32u c0, Bit32u c1, double at, double now, Bit32u *x, Bit32u *y) {
    const double span = (double)(Bit32s)(c1 - c0);
    const double dx = (double)x1 - (double)x0, dy = (double)y1 - (double)y0;
    if (span <= 0 || span > 500 || fabs(dx) > 2 * 65536.0 || fabs(dy) > 2 * 65536.0) {
        *x = x1;
        *y = y1;
        return;
    }
    const double f = std::max(0.0, std::min(1.0, (now - at) / span));
    *x = (Bit32u)((double)x0 + dx * f);
    *y = (Bit32u)((double)y0 + dy * f);
}

void show_others(double now) {
    const int self = self_seat();
    // the actors thought for elsewhere
    for (int i = 0; i < wolf_actors(); i++) {
        if (g_dead[i] || g_owner[i] < 0 || g_owner[i] == self) continue;
        const Report &r = g_report[i];
        if (!r.has || r.from != g_owner[i]) continue;
        WolfAct a = r.a[1];
        between(r.a[0].x, r.a[0].y, r.a[1].x, r.a[1].y, r.clock[0], r.clock[1], r.at, now, &a.x, &a.y);
        wolf_actor_show(i, a, false);
    }
    // the other players
    for (int s = 0; s < kWolfMaxPlayers; s++) {
        if (s == self) continue;
        const int slot = s < self ? s : s - 1;
        const Player &p = g_players[s];
        const bool shown = p.known && p.inPlay && p.has && p.serial == g_mySerial && now - p.heardAt < kQuietMs;
        Bit32u x = 0, y = 0;
        if (shown) between(p.x[0], p.y[0], p.x[1], p.y[1], p.clock[0], p.clock[1], p.at, now, &x, &y);
        wolf_avatar(slot, shown, x, y, now - p.movedAt < 250, p.callsign.c_str());
        // a door a player stands in stays open in every game
        int d;
        if (shown && wolf_door_at((int)(p.x[1] >> 16), (int)(p.y[1] >> 16), &d)) wolf_door_keep(d);
    }
}

void watch_level(double now) {
    WolfAct a;
    const int self = self_seat();
    for (int i = 0; i < wolf_actors(); i++) {
        if (!wolf_actor(i, &a) || !wolf_actor_fighter(a.cls)) continue;
        const bool alive = (a.flags & WOLF_SHOOTABLE) != 0;
        if (!g_dead[i] && (g_owner[i] < 0 || g_owner[i] == self)) {
            if (g_aliveWas[i] && !alive && a.hp <= 0) {
                // fallen in our game: every game's to fell
                g_dead[i] = true;
                g_deadX[i] = a.x;
                g_deadY[i] = a.y;
                g_owner[i] = -1;
                g_pending[i] = false;
                g_killsTold++;
                send_event(EV_DIED, i, (int)a.x, (int)a.y);
            } else if (alive && g_owner[i] < 0 && (a.flags & WOLF_ATTACKMODE) && !g_pending[i]) {
                // it has noticed our player (or we shot it): ours to think for, if nobody was first
                g_pending[i] = true;
                g_claimHp[i] = g_aliveWas[i] ? g_hpWas[i] : a.hp;
                g_claims++;
                send_event(EV_CLAIM, i);
            }
        }
        g_aliveWas[i] = alive;
        g_hpWas[i] = a.hp;
    }
    // doors: one that begins to open here opens everywhere
    for (int d = 0; d < wolf_doors(); d++) {
        const int act = wolf_door_action(d);
        if (act == 2 && (g_doorWas[d] == 1 || g_doorWas[d] == 3)) send_event(EV_DOOR, d);
        g_doorWas[d] = act;
    }
    // a wall pushed here
    int x, y, dir;
    const bool moving = wolf_push_moving(&x, &y, &dir);
    if (moving && !g_pushWas) {
        if (now < g_pushExpectUntil) {
            g_pushExpectUntil = -1e9;       // (another machine's, begun here by us)
        } else {
            Push p = { x, y, dir, wolf_player_area() };
            g_pushes.push_back(p);
            send_event(EV_PUSH, x, y, dir, p.area);
        }
    }
    g_pushWas = moving;
    // a key found here is everybody's
    const int keys = wolf_keys();
    const int found = keys & ~g_keysWas;
    if (found) {
        g_keys |= found;
        send_event(EV_KEY, found);
    }
    g_keysWas = keys;
}

void send_state(double now, bool inPlay) {
    if (!g_net) return;
    NetworkMessage msg;
    WolfState *st = msg.mutable_wolf_state();
    st->set_serial(g_mySerial);
    st->set_seat((Bit32u)g_net->self);
    st->set_in_play(inPlay);
    st->set_clock((Bit32u)now);
    if (inPlay) {
        const WolfBody b = wolf_player();
        st->set_x(b.x);
        st->set_y(b.y);
        st->set_angle((Bit32u)b.angle);
        st->set_health(b.health);
        st->set_attack((Bit32u)b.attack);
        Player &me = g_players[g_net->self >= 0 && g_net->self < kWolfMaxPlayers ? g_net->self : 0];
        me.x[1] = b.x;
        me.y[1] = b.y;
        me.has = true;
        me.inPlay = true;
        me.serial = g_mySerial;
        me.heardAt = now;
        if (g_synced) {
            // the actors we think for (the first of them every time; past kActorsPerState the rest by turns)
            static int turn = 0;
            std::vector<int> mine;
            WolfAct a;
            for (int i = 0; i < wolf_actors(); i++) {
                if (g_owner[i] == g_net->self && !g_dead[i] && wolf_actor(i, &a) && (a.flags & WOLF_SHOOTABLE)) mine.push_back(i);
            }
            const int n = (int)mine.size();
            const int count = std::min(n, (int)kActorsPerState);
            const int from = n > kActorsPerState ? (turn += kActorsPerState) % n : 0;
            for (int k = 0; k < count; k++) {
                const int i = mine[(size_t)((from + k) % n)];
                wolf_actor(i, &a);
                WolfActor *m = st->add_actor();
                m->set_index((Bit32u)i);
                m->set_x(a.x);
                m->set_y(a.y);
                m->set_tilex((Bit32u)a.tilex);
                m->set_tiley((Bit32u)a.tiley);
                m->set_dir((Bit32u)a.dir);
                m->set_state((Bit32u)(a.state + 0x8000));
                m->set_ticcount(a.ticcount);
                m->set_flags((Bit32u)a.flags);
                m->set_hp(a.hp);
                m->set_distance(a.distance);
            }
        }
    }
    g_statesSent++;
    if (g_net->host) host_send_all(msg, -1, true);
    else if (!g_net->peers.empty()) g_net->peers[0]->conn.send_best_effort(msg);
}

}  // namespace

bool wolf_session_configured() {
    const char *off = getenv("WCNET");
    if (off && off[0] == '0') return false;
    if (getenv("WCSOLO") && getenv("WCSOLO")[0] == '1') return false;
    return getenv("WCROOM") || getenv("WCHOST") || getenv("WCPORT");
}

void wolf_session_tick() {
    const double now = PIC_FullIndex();
    if (!g_net) {
        if (g_tried && now < g_nextTryMs) return;
        g_tried = true;
        g_nextTryMs = now + kRetryMs;
        if (!wolf_session_configured() || !start()) return;
    }
    if (now - g_pollAt >= kPollMs) {
        g_pollAt = now;
        poll_peers();
        if (!g_net->host && g_net->peers.empty()) {
            wolf_session_shutdown();    // (the host gone: looked for again)
            return;
        }
    }
    if (now - g_helloAt >= kHelloEveryMs) {
        g_helloAt = now;
        send_hellos();
        if (!wolf_in_play()) send_state(now, false);
    }
    if (now - g_loggedAt >= 5000 && wolf_in_play()) {
        g_loggedAt = now;
        int mine = 0, theirs = 0, fallen = 0, others = 0;
        for (int i = 0; i < wolf_actors(); i++) {
            fallen += g_dead[i] ? 1 : 0;
            mine += !g_dead[i] && g_owner[i] == g_net->self ? 1 : 0;
            theirs += !g_dead[i] && g_owner[i] >= 0 && g_owner[i] != g_net->self ? 1 : 0;
        }
        for (int s = 0; s < kWolfMaxPlayers; s++) others += s != g_net->self && g_players[s].known && g_players[s].inPlay && now - g_players[s].heardAt < kQuietMs;
        wclog(1, "Wolfenstein 3-D: %.1fs: level %u %s, %d others playing; actors: %d ours, %d shown, %d fallen; reports sent %u, got %u; events "
                 "sent %u, got %u", now / 1000.0, g_mySerial, g_synced ? "in step" : "not in step", others, mine, theirs, fallen, g_statesSent,
              g_statesGot, g_eventsSent, g_eventsGot);
    }
}

void wolf_session_shutdown() {
    if (!g_net) return;
    wclog(1, "Wolfenstein 3-D: session closed");
    delete g_net;
    g_net = NULL;
    g_tried = false;
    drop_room();
}

bool wolf_session_level_hold(bool esc) {
    if (!wolf_session_configured() || g_alone) return false;
    const double now = PIC_FullIndex();
    const WolfLevel level = wolf_level();
    if (is_host()) {
        // the room's level is the host's game's: one it has not begun yet begins here
        if (!g_room.serial || level != g_room.level) host_new_level(level, 0);
        return false;
    }
    if (g_net && g_room.serial) {
        g_holdSince = -1;
        if (!wolf_has_level(g_room.level)) {
            if (!g_refusedLevel) {
                g_refusedLevel = true;
                wolf_notice("The room plays an episode the shareware does not have: playing alone", 9000);
            }
            return false;
        }
        g_refusedLevel = false;
        if (level != g_room.level) {
            wclog(1, "Wolfenstein 3-D: the room's level instead of ours: episode %d floor %d, difficulty %d", g_room.level.episode + 1,
                  g_room.level.map + 1, g_room.level.difficulty);
            wolf_set_level(g_room.level);
        }
        return false;
    }
    // a guest (or nobody yet) and the host's level not known: wait for it a while
    if (esc) {
        g_alone = true;
        wolf_notice("Playing alone");
        return false;
    }
    if (g_holdSince < 0) {
        g_holdSince = now;
        wolf_notice("Waiting for the host's game...  (Esc: play alone)", kHoldMs);
    }
    if (now - g_holdSince < kHoldMs) return true;
    g_holdSince = -1;
    wclog(1, "Wolfenstein 3-D: the host's level is not known after %d s: our own for now", kHoldMs / 1000);
    return false;
}

void wolf_session_frame(bool fresh) {
    const double now = PIC_FullIndex();
    if (fresh) reset_level_state();
    if (!g_net || g_alone) return;
    const WolfLevel level = wolf_level();
    if (g_net->host) {
        if (!g_room.serial || level != g_room.level) host_new_level(level, 0);       // (a game loaded, a level never set up by us)
        if (!g_synced && !g_remoteExit) {
            g_mySerial = g_room.serial;
            g_room.sig = wolf_level_sig();
            g_synced = true;
            apply_story(fresh);
            host_say_room();
        }
    } else if (!g_synced && !g_remoteExit) {
        if (!g_room.serial || g_refusedLevel) {
            // (no room's level yet: our own game by itself)
        } else if (level != g_room.level) {
            if (wolf_has_level(g_room.level)) {
                wclog(1, "Wolfenstein 3-D: out of step with the room (its episode %d floor %d, difficulty %d): our game sent there",
                      g_room.level.episode + 1, g_room.level.map + 1, g_room.level.difficulty);
                wolf_set_level(g_room.level);
                wolf_reload();
                return;
            }
        } else if (g_snapHere && g_snap.serial() == g_room.serial) {
            g_snapHere = false;
            if (g_snap.sig() && g_snap.sig() != wolf_level_sig()) {
                g_refusedLevel = true;
                wolf_notice("This floor is not the same as the host's (other game files): playing alone", 9000);
                wclog(1, "Wolfenstein 3-D: the level's actors are not the host's (%08x, ours %08x)", g_snap.sig(), wolf_level_sig());
                return;
            }
            g_mySerial = g_room.serial;
            g_synced = true;
            take_snapshot(fresh);
        } else if (!g_snapAsked || now - g_snapAskedAt > kSnapAgainMs) {
            g_snapAsked = true;
            g_snapAskedAt = now;
            send_event(EV_SNAPREQ, (int)wolf_level_sig());
        }
    }
    if (g_synced && g_room.serial != g_mySerial) {
        // the room has gone on to another level without this game: there by the game's own warp
        g_synced = false;
        if (wolf_has_level(g_room.level)) {
            wclog(1, "Wolfenstein 3-D: the room has moved on to episode %d floor %d: our game sent there", g_room.level.episode + 1, g_room.level.map + 1);
            wolf_set_level(g_room.level);
            wolf_reload();
        }
        return;
    }
    if (g_synced) {
        show_others(now);
        watch_level(now);
    }
    if (now - g_stateAt >= kStateEveryMs) {
        g_stateAt = now;
        send_state(now, true);
    }
}

void wolf_session_loop_exit(int playstate) {
    if (!g_net || g_alone) return;
    const bool wasSynced = g_synced;
    if (wasSynced && !g_remoteExit && (playstate == 1 || playstate == 9 || playstate == 6)) send_event(EV_DONE, playstate);
    g_synced = false;
    g_remoteExit = false;
    send_event(EV_OUT, playstate);
    send_state(PIC_FullIndex(), false);
    wclog(1, "Wolfenstein 3-D: the frames over (playstate %d)%s", playstate, wasSynced ? "" : ", not in step with the room");
}

void wolf_session_idle() {
    if (!g_net || g_alone) return;
    send_event(EV_OUT, 0);
    send_state(PIC_FullIndex(), false);
}

bool wolf_session_damage(int i, int damage) {
    if (!g_net || !g_synced || i < 0 || i >= kWolfMaxActors || g_dead[i]) return false;
    if (g_owner[i] < 0 || g_owner[i] == g_net->self) return false;
    send_event(EV_DAMAGE, i, damage);
    return true;
}

std::string wolf_session_status() {
    char b[512];
    if (!g_net) {
        snprintf(b, sizeof(b), "no room (%s)", wolf_session_configured() ? "looked for" : "none asked for");
        return b;
    }
    int mine = 0, theirs = 0, fallen = 0;
    for (int i = 0; i < wolf_actors(); i++) {
        fallen += g_dead[i] ? 1 : 0;
        mine += !g_dead[i] && g_owner[i] == g_net->self ? 1 : 0;
        theirs += !g_dead[i] && g_owner[i] >= 0 && g_owner[i] != g_net->self ? 1 : 0;
    }
    std::string who;
    const double now = PIC_FullIndex();
    for (int s = 0; s < kWolfMaxPlayers; s++) {
        const Player &p = g_players[s];
        if (!p.known || s == g_net->self) continue;
        char pb[96];
        snprintf(pb, sizeof(pb), " %s(seat %d%s at %.1f %.1f)", p.callsign.c_str(), s, p.inPlay && now - p.heardAt < kQuietMs ? ", playing" : "",
                 p.x[1] / 65536.0, p.y[1] / 65536.0);
        who += pb;
    }
    snprintf(b, sizeof(b), "%s seat %d, the room's level %u (E%dL%d skill %d), ours %u %s; actors %d ours, %d shown, %d fallen; claims %u, kills told %u, "
                           "heard %u; walls pushed %d, keys %d; others:%s", g_net->host ? "host" : "guest", g_net->self, g_room.serial,
             g_room.level.episode + 1, g_room.level.map + 1, g_room.level.difficulty, g_mySerial, g_synced ? "in step" : "not in step", mine, theirs,
             fallen, g_claims, g_killsTold, g_killsHeard, (int)g_pushes.size(), g_keys, who.empty() ? " none" : who.c_str());
    return b;
}

}  // namespace wc
