/*
 *  Terminal Velocity's session (wcnet_tv.h, docs/terminalvelocity.md section 5).
 *
 *  Everybody in the room flies the same campaign level together.  The host is seat 0 (the room's, or the TCP
 *  listener); a guest opens a stream to it and says hello, the host answers with the guest's seat.  Each pilot sends
 *  its ship (TvState: place, angles, their rates, shield) some 15 times a second while it flies; the others draw it,
 *  carried on between reports by its rates, with the game's own call that puts a model in the frame's draw list
 *  (where the game's net mode draws its other players), as the model of the pilot's choice (its `look`: the TV-202,
 *  the Ymir IceRider or SnowScout).  A pilot's shots are the game's fire call's arguments (TvShot), made again on the
 *  others' machines as shots of their own player's: they fly there as they do here, but what they hit is told by
 *  the shooter (TvHit): every hit a pilot's own shot makes on an object is applied in its game and sent, and every
 *  machine applies it to the same object (the level's objects are the same everywhere, named by their index) the
 *  game's way, its hit points less the damage and its next stage at none left: the same hits, the same deaths.  The
 *  level's moving objects (its craft, its ground vehicles) are flown by one machine each, the nearest pilot's (the
 *  host decides, TvOwners); the others' copies are puppets (their update skipped, their place from the owner's
 *  reports, TvObjects).  A pilot's afterburner (its fuel going down) shows on the others' screens as a flame behind
 *  its ship (the game's own fireball model).
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
    kObjectsEveryMs = 100,          // our moving objects' places, ten times a second
    kOwnersEveryMs = 2000,          // the host's word on owners, this often besides each change
    kDecideEveryMs = 300,
    kObjFreshMs = 1500,             // a moving object whose owner's report is older is flown here meanwhile
    kOwnerHoldMs = 1500,            // an object keeps its owner at least this long
    kRemoteShotMs = 10000,
};
const double kUnit = 65536.0;
const double kTurn = 65536.0 / (2 * 3.14159265358979);
const double kOwnRange = 600.0;     // (world units) an object farther than this from every pilot keeps its owner
const char kFlame[] = "fireball.bin";   // the afterburner's flame (STARTUP.POD, the game's own)

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
    bool boost;
    Remote() : known(false), look("ship.bin"), at(-1e9), flying(false), level(-1), shield(0), boost(false) {
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
    Bit32u models[kLookCount + 1];  // (the last: the flame)
    bool loading[kLookCount + 1];
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

// The level's objects as the room flies them, by index.
struct ObjSync {
    int owner;                  // the seat whose machine flies it (the host's by default)
    double ownedAt;             // since when
    bool mobile;                // seen moving (or of a moving class)
    bool updated;               // its update made here since our last report
    Bit32s last[3];             // its place at its last update here (mobile: it moved)
    double at, prevAt;          // its owner's last two reports
    Bit32s pos[6], prev[6];
    ObjSync() : owner(0), ownedAt(-1e9), mobile(false), updated(false), at(-1e9), prevAt(-1e9) {
        memset(last, 0, sizeof(last)); memset(pos, 0, sizeof(pos)); memset(prev, 0, sizeof(prev));
    }
};
std::vector<ObjSync> g_objs;
int g_objsLevel = -2;
Bit32u g_objsFrames = 0;
std::vector<std::pair<int, Bit32s> > g_hits;            // others' hits, applied at the next frame (index, damage)
std::vector<std::pair<Bit32u, double> > g_remoteShots;  // our copies of others' shots (the projectile, until)
double g_objectsAt = -1e9, g_ownersAt = -1e9, g_decidedAt = -1e9;
double g_boostAt = -1e9;
Bit32s g_lastFuel = 0;
unsigned g_hitsSent = 0, g_hitsTaken = 0, g_kills = 0, g_skipped = 0;
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
    r.boost = st.boost();
    if (r.flying && !wasFlying) {
        wclog(1, "Terminal Velocity: %s flies level %d (%s)", r.callsign.c_str(), r.level, st.level_name().c_str());
    }
}

// The level's objects as we know them: made again for a new level (or the same level flown again: the frame count
// starts over), grown as the level adds objects (a pickup a bunker drops: such additions can differ from machine to
// machine, the level's own objects before them are the same everywhere).
bool objs_ready() {
    const int level = tv_level();
    const int n = tv_object_count();
    if (level != g_objsLevel || tv_frames() < g_objsFrames || n < (int)g_objs.size()) {
        g_objs.clear();
        g_objsLevel = level;
        g_hits.clear();
        g_remoteShots.clear();
    }
    for (int i = (int)g_objs.size(); i < n; i++) {
        g_objs.push_back(ObjSync());
        const int c = tv_object_class(i);
        g_objs.back().mobile = c == 7 || c == 10;       // (the craft, the ground vehicles: others once seen moving)
    }
    g_objsFrames = tv_frames();
    return n > 0;
}

void take_objects(const TvObjects &o, int seat) {
    if (o.level() != tv_level() || !objs_ready()) return;
    const double now = PIC_FullIndex();
    for (int k = 0; k < o.objects_size(); k++) {
        const TvObject &ob = o.objects(k);
        const int i = (int)ob.index();
        if (i < 0 || i >= (int)g_objs.size() || ob.pos_size() != 6) continue;
        ObjSync &os = g_objs[i];
        if (os.owner != seat) {
            // (a report from a machine that flew it before the host's word reached it: the host's word stands; the host
            // takes it as the owner's while it decides nothing else)
            if (!g_net->host) continue;
            os.owner = seat;
            os.ownedAt = now;
        }
        memcpy(os.prev, os.pos, sizeof(os.prev));
        os.prevAt = os.at;
        for (int c = 0; c < 6; c++) os.pos[c] = ob.pos(c);
        os.at = now;
        os.mobile = true;
    }
}

void take_owners(const TvOwners &w) {
    if (w.level() != tv_level() || !objs_ready()) return;
    const double now = PIC_FullIndex();
    std::vector<int> owner(g_objs.size(), 0);
    for (int k = 0; k < w.index_size() && k < w.owner_size(); k++) {
        if ((int)w.index(k) < (int)owner.size()) owner[w.index(k)] = (int)w.owner(k);
    }
    for (size_t i = 0; i < g_objs.size(); i++) {
        if (g_objs[i].owner != owner[i]) {
            g_objs[i].owner = owner[i];
            g_objs[i].ownedAt = now;
            g_objs[i].at = -1e9;            // (a new owner's reports from now)
        }
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
    } else if (msg.has_tv_hit()) {
        if (!p->hello) return;
        const TvHit &h = msg.tv_hit();
        if (h.level() == tv_level() && g_hits.size() < 256) g_hits.push_back(std::make_pair((int)h.index(), (Bit32s)h.damage()));
        const int seat = g_net->host ? p->player : (int)h.player();
        NetworkMessage out;
        *out.mutable_tv_hit() = h;
        out.mutable_tv_hit()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send(out); });
    } else if (msg.has_tv_objects()) {
        if (!p->hello) return;
        const TvObjects &o = msg.tv_objects();
        const int seat = g_net->host ? p->player : (int)o.player();
        take_objects(o, seat);
        NetworkMessage out;
        *out.mutable_tv_objects() = o;
        out.mutable_tv_objects()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send_best_effort(out); });
    } else if (msg.has_tv_owners()) {
        if (!p->hello || g_net->host) return;
        take_owners(msg.tv_owners());
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
    st->set_boost(now - g_boostAt < 300);
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

const char *model_name(int look) { return look == kLookCount ? kFlame : kLooks[look]; }

void model_loaded(int look, Bit32u eax) {
    if (!g_net || look < 0 || look > kLookCount) return;
    g_net->models[look] = eax;
    g_net->loading[look] = false;
    wclog(1, "Terminal Velocity: the model %s loaded: %08x", model_name(look), (unsigned)eax);
}

// The model a look is drawn with in this level (kLookCount: the flame): the player's own ship's is the game's
// (ship.bin, loaded with the level); another is loaded once a level, by the game's loader, at a frame's top.
Bit32u model_for(int look) {
    if (look == 0) return tv_r32(tv_data(tvsig::D_SHIP_MODEL));
    if (g_net->models[look]) return g_net->models[look];
    if (!g_net->loading[look]) {
        const std::string name = model_name(look);
        std::vector<Bit32u> scratch((name.size() + 4) / 4, 0);
        memcpy(&scratch[0], name.c_str(), name.size());
        std::vector<Bit32u> args(1, TV_SCRATCH);
        g_net->loading[look] = true;
        tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_LOAD_MODEL), args, scratch, model_loaded, look);
    }
    return 0;
}

// One of the others' shots made here: the projectile kept as theirs (its hits are told by its shooter).
void shot_made(int seat, Bit32u eax) {
    (void)seat;
    g_shotsMade++;
    if (eax && g_remoteShots.size() < 256) g_remoteShots.push_back(std::make_pair(eax, PIC_FullIndex() + kRemoteShotMs));
}

void destroyed(int index, Bit32u eax) {
    (void)eax;
    g_kills++;
    wclog(2, "Terminal Velocity: object %d at its next stage (a hit told by another pilot)", index);
}

// Another pilot's hit, applied the game's way (0x292B0's): its hit points less the damage, its next stage at none.
void apply_hit(int i, Bit32s damage) {
    const Bit32u o = tv_object_addr(i);
    if (!o) return;
    const Bit32s hp = (Bit32s)tv_r32(o + 0x1c);
    if (hp <= 0) return;
    tv_w32(o + 0x1c, (Bit32u)(hp - damage));
    g_hitsTaken++;
    if (hp - damage <= 0) {
        std::vector<Bit32u> args;
        args.push_back(o);
        args.push_back(tv_object_type_addr(i));
        tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_DESTROY), args, std::vector<Bit32u>(), destroyed, i);
    }
}

double dist2(const Bit32s a[3], const Bit32s b[3]) {
    double d = 0;
    for (int k = 0; k < 3; k++) {
        const double v = ((double)a[k] - (double)b[k]) / kUnit;
        d += v * v;
    }
    return d;
}

// The host: each moving object near a pilot to the nearest pilot's machine (an object changes hands only when the
// other is a fifth nearer, and not twice in kOwnerHoldMs).
void decide_owners(double now) {
    TvShip me;
    tv_player(&me);
    Bit32s where[kMaxSeats][3];
    bool flying[kMaxSeats];
    for (int s = 0; s < kMaxSeats; s++) {
        flying[s] = false;
        if (s == g_net->self) {
            flying[s] = tv_in_flight();
            memcpy(where[s], me.pos, sizeof(where[s]));
        } else {
            const Remote &r = g_net->remotes[s];
            flying[s] = r.known && r.flying && r.level == tv_level() && now - r.at < kStateStaleMs;
            for (int k = 0; k < 3; k++) where[s][k] = r.pos[k];
        }
    }
    bool changed = false;
    for (size_t i = 0; i < g_objs.size(); i++) {
        ObjSync &os = g_objs[i];
        if (!os.mobile) continue;
        const Bit32u o = tv_object_addr((int)i);
        if (!o || (Bit32s)tv_r32(o + 0x1c) <= 0) continue;
        Bit32s at[3];
        for (int k = 0; k < 3; k++) at[k] = (Bit32s)tv_r32(o + 4 * k);
        int best = -1;
        double bd = 1e30;
        for (int s = 0; s < kMaxSeats; s++) {
            if (!flying[s]) continue;
            const double d = dist2(at, where[s]);
            if (d < bd) { bd = d; best = s; }
        }
        if (best < 0 || best == os.owner || bd > kOwnRange * kOwnRange || now - os.ownedAt < kOwnerHoldMs) continue;
        const bool ownerHere = os.owner >= 0 && os.owner < kMaxSeats && flying[os.owner];
        if (ownerHere && bd > 0.64 * dist2(at, where[os.owner])) continue;
        os.owner = best;
        os.ownedAt = now;
        os.at = -1e9;
        changed = true;
    }
    if (changed || now - g_ownersAt >= kOwnersEveryMs) {
        g_ownersAt = now;
        NetworkMessage msg;
        TvOwners *w = msg.mutable_tv_owners();
        w->set_level(tv_level());
        for (size_t i = 0; i < g_objs.size(); i++) {
            if (g_objs[i].owner != 0) {
                w->add_index((Bit32u)i);
                w->add_owner((Bit32u)g_objs[i].owner);
            }
        }
        send_all(msg, false);
    }
}

// Our moving objects' places (those flown here, updated since the last report).
void send_objects() {
    NetworkMessage msg;
    TvObjects *ob = msg.mutable_tv_objects();
    ob->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    ob->set_level(tv_level());
    int n = 0;
    for (size_t i = 0; i < g_objs.size() && n < 48; i++) {
        ObjSync &os = g_objs[i];
        if (!os.updated) continue;
        os.updated = false;
        if (!os.mobile || os.owner != (g_net->self < 0 ? 0 : g_net->self)) continue;
        const Bit32u o = tv_object_addr((int)i);
        TvObject *t = ob->add_objects();
        t->set_index((Bit32u)i);
        for (int k = 0; k < 6; k++) t->add_pos((Bit32s)tv_r32(o + 4 * k));
        t->set_hp((Bit32s)tv_r32(o + 0x1c));
        t->set_state((Bit32s)tv_r32(o + 0x64));
        n++;
    }
    if (n) send_all(msg, true);
}

bool is_puppet(int i) {
    if (!g_net || i < 0 || i >= (int)g_objs.size()) return false;
    const ObjSync &os = g_objs[i];
    const int self = g_net->self < 0 ? 0 : g_net->self;
    return os.owner != self && PIC_FullIndex() - os.at < kObjFreshMs;
}

// The puppets where their owners' reports put them, carried on to now (by their last two reports).
void place_puppets(double now) {
    for (size_t i = 0; i < g_objs.size(); i++) {
        if (!is_puppet((int)i)) continue;
        const ObjSync &os = g_objs[i];
        const Bit32u o = tv_object_addr((int)i);
        double f = 0;
        if (os.prevAt > 0 && os.at - os.prevAt > 10 && os.at - os.prevAt < 1000) f = std::min(now - os.at, 500.0) / (os.at - os.prevAt);
        for (int k = 0; k < 3; k++) tv_w32(o + 4 * k, (Bit32u)(Bit32s)(os.pos[k] + (os.pos[k] - os.prev[k]) * f));
        for (int k = 3; k < 6; k++) tv_w32(o + 4 * k, (Bit32u)os.pos[k]);
    }
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
            int owned = 0, puppets = 0;
            for (size_t i = 0; i < g_objs.size(); i++) {
                if (g_objs[i].owner == s) owned++;
                if (is_puppet((int)i)) puppets++;
            }
            wclog(2, "Terminal Velocity: seat %d %s: %s level %d at %d %d %d, %.0f ms old%s; shots made %u sent %u, drawn %u; "
                     "hits sent %u taken %u skipped %u, stages %u; objects it flies %d, puppets here %d",
                  s, r.callsign.c_str(), r.flying ? "flying" : "not flying", r.level, r.pos[0], r.pos[1], r.pos[2], now - r.at,
                  r.boost ? ", BOOSTING" : "", g_shotsMade, g_shotsSent, g_drawn, g_hitsSent, g_hitsTaken, g_skipped, g_kills, owned, puppets);
        }
    }
}

void tv_session_frame() {
    if (!g_net) return;
    const double now = PIC_FullIndex();
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
    // the afterburner burning: its fuel going down
    const Bit32s fuel = (Bit32s)tv_r32(tv_data(tvsig::D_BURN_FUEL));
    if (fuel < g_lastFuel) g_boostAt = now;
    g_lastFuel = fuel;
    if (!objs_ready()) return;
    // our copies of others' shots that are spent (their slots free again) forgotten
    for (size_t i = 0; i < g_remoteShots.size();) {
        if (now > g_remoteShots[i].second || tv_r32(g_remoteShots[i].first + 8) == 0) {
            g_remoteShots.erase(g_remoteShots.begin() + i);
        } else {
            i++;
        }
    }
    // others' hits, applied
    for (size_t i = 0; i < g_hits.size(); i++) apply_hit(g_hits[i].first, g_hits[i].second);
    g_hits.clear();
    place_puppets(now);
    if (now - g_objectsAt >= kObjectsEveryMs) {
        g_objectsAt = now;
        send_objects();
    }
    if (g_net->host && now - g_decidedAt >= kDecideEveryMs) {
        g_decidedAt = now;
        decide_owners(now);
    }
    // the others' shots, made as our own player's (they fly here as there; their hits are their shooter's to tell)
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
        if (r.boost && now - r.at < 500) {
            // the afterburner: a trail of flame behind it, three fireballs smaller and smaller, flickering
            const Bit32u flame = model_for(kLookCount);
            if (!flame) continue;
            const double h = (Bit32s)scratch[5] / kTurn, p = -(Bit16s)(Bit16u)scratch[3] / kTurn;
            const double fwd[3] = { sin(h) * cos(p), sin(p), cos(h) * cos(p) };
            for (int k = 0; k < 3; k++) {
                const unsigned flick = (tv_frames() * (7 + 4 * k) + 3 * k) % 9;     // (0..8)
                const double back = 0.9 + 0.75 * k + 0.08 * flick;
                const double scale = (1.15 - 0.3 * k) * (0.8 + 0.05 * flick);
                std::vector<Bit32u> fs(scratch);
                for (int c = 0; c < 3; c++) fs[c] = (Bit32u)((Bit32s)scratch[c] - (Bit32s)(fwd[c] * back * kUnit));
                std::vector<Bit32u> fa;
                fa.push_back(TV_SCRATCH);
                fa.push_back(flame);
                fa.push_back((Bit32u)(scale * 0x10000));
                tv_queue_call(TV_AT_DRAW, tv_fn(TV_FN_DRAW_MODEL), fa, fs, NULL, s);
            }
        }
    }
}

bool tv_session_puppet(int obj) { return is_puppet(obj); }

void tv_session_updated(int obj) {
    if (!g_net || obj < 0 || obj >= (int)g_objs.size()) return;
    ObjSync &os = g_objs[obj];
    os.updated = true;
    const Bit32u o = tv_object_addr(obj);
    Bit32s at[3];
    for (int k = 0; k < 3; k++) at[k] = (Bit32s)tv_r32(o + 4 * k);
    if (!os.mobile && (os.last[0] || os.last[1] || os.last[2]) && memcmp(at, os.last, sizeof(at)) != 0) os.mobile = true;
    memcpy(os.last, at, sizeof(at));
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

bool tv_session_hit(int obj, Bit32s damage, Bit32u projectile) {
    if (!g_net) return true;
    for (size_t i = 0; i < g_remoteShots.size(); i++) {
        if (g_remoteShots[i].first == projectile) {
            g_skipped++;
            return false;           // (another pilot's shot: its shooter tells what it hit)
        }
    }
    NetworkMessage msg;
    TvHit *h = msg.mutable_tv_hit();
    h->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    h->set_level(tv_level());
    h->set_index((Bit32u)obj);
    h->set_damage(damage);
    send_all(msg, false);
    g_hitsSent++;
    return true;
}

void tv_session_level(int index, const std::string &name) {
    (void)index;
    (void)name;
}

void tv_session_shutdown() { shutdown(); }

bool tv_session_active() { return g_net != NULL; }

bool tv_session_partner(TvShip *out) {
    memset(out, 0, sizeof(*out));
    if (!g_net) return false;
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == g_net->self || !r.known || !r.flying || r.level != tv_level() || now - r.at > kStateStaleMs) continue;
        const double dt = std::min(now - r.at, (double)kCarryMs) / 1000.0;
        for (int k = 0; k < 3; k++) {
            out->pos[k] = r.pos[k] + (Bit32s)(r.rate[k] * dt);
            out->ang[k] = r.pos[3 + k] + (Bit32s)(r.rate[3 + k] * dt);
            out->vel[k] = r.rate[k];
            out->spin[k] = r.rate[3 + k];
        }
        out->shield = r.shield;
        return true;
    }
    return false;
}

}  // namespace wc
