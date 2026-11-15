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
#include "wcnet_lab.h"
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
    kSettleMs = 2000,               // a pilot new to the place (the level's start, a tunnel's way out) neither decides
                                    // owners, nor is given objects, nor reports any, this long: its copies are old
    kRemoteShotMs = 10000,
};
const double kUnit = 65536.0;
const double kTurn = 65536.0 / (2 * 3.14159265358979);
const double kOwnRange = 600.0;     // (world units) an object farther than this from every pilot keeps its owner
const char kFlame[] = "fireball.bin";   // the afterburner's flame (STARTUP.POD, the game's own)

// The craft a pilot's ship may be drawn as (the host's choice for each seat: the page's Craft menus, WCTVLOOKS): the
// game's own models.  A model is looked up in the game's archives before it is loaded (the loader ends the program on
// a name it cannot find); one not there, or "planet", is drawn as the level's own craft (its first craft type's
// model, loaded with the level), else as the TV-202.
const char *const kLooks[] = { "ship.bin", "icebrd.bin", "snowftr.bin", "hawk.bin", "gull1.bin", "planet" };
const char *const kLookNames[] = { "TV-202", "Ymir IceRider", "Ymir SnowScout", "Hawk", "Gull", "the level's own craft" };
const int kLookCount = 6;
enum { LOOK_TV202 = 0, LOOK_PLANET = 5 };

int look_index(const std::string &look0) {
    std::string look = look0;
    for (size_t i = 0; i < look.size(); i++) look[i] = (char)tolower((unsigned char)look[i]);
    static const char *const kAlias[][2] = { { "tv-202", "ship.bin" }, { "tv202", "ship.bin" }, { "icerider", "icebrd.bin" },
        { "snowscout", "snowftr.bin" }, { "hawk", "hawk.bin" }, { "gull", "gull1.bin" }, { "level", "planet" } };
    for (size_t i = 0; i < sizeof(kAlias) / sizeof(kAlias[0]); i++) {
        if (look == kAlias[i][0]) look = kAlias[i][1];
    }
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
    bool done;                  // its level done (its exit flown)
    int difficulty;
    std::string levelName;
    double flyingSince;         // since when it flies its level (a start of it: a death's retry, a late start)
    bool inLevel;               // a level of its campaign under way (its briefing, its flight)
    int space;                  // where in the level: 0 outdoors, 1 + n in tunnel n
    double arate[3];            // its angles' rates (16-bit units a second), from its last two reports
    double disp[6];             // where we draw it (smooth_remote): carried on by its rates, eased towards its reports
    bool dispOn;
    double dispAt;
    Remote() : known(false), look("ship.bin"), at(-1e9), flying(false), level(-1), shield(0), boost(false), done(false),
               difficulty(-1), flyingSince(-1e9), inLevel(false), space(0), dispOn(false), dispAt(-1e9) {
        memset(pos, 0, sizeof(pos));
        memset(rate, 0, sizeof(rate));
        memset(arate, 0, sizeof(arate));
        memset(disp, 0, sizeof(disp));
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
    int modelState[kLookCount + 1]; // MODEL_*
    Bit32u modelsFrame;         // the frame count at the level's start (a new level: loaded again)
    TvNet() : host(false), self(-1), seats(2), listener(NULL), stateAt(-1e9), modelsFrame(0) {
        memset(models, 0, sizeof(models));
        memset(modelState, 0, sizeof(modelState));
    }
    ~TvNet() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

TvNet *g_net;
void show_line(const std::string &line0);
enum { MODEL_UNKNOWN = 0, MODEL_LOOKING, MODEL_LOADING, MODEL_READY, MODEL_MISSING };
// The craft of each seat (the host's word; the host's own from WCTVLOOKS: "<its own>,<the others'>" or one per seat).
std::string g_looks[4] = { "ship.bin", "ship.bin", "ship.bin", "ship.bin" };

// The level's objects as the room flies them, by index.
struct ObjSync {
    int owner;                  // the seat whose machine flies it (the host's by default)
    double ownedAt;             // since when
    bool mobile;                // seen moving (or of a moving class)
    bool updated;               // its update made here since our last report
    Bit32s last[3];             // its place at its last update here (mobile: it moved)
    double at, prevAt;          // its owner's last two reports
    Bit32s pos[6], prev[6];
    double disp[6];             // where we put it as a puppet (smoothed: place_puppets)
    bool dispOn;
    double dispAt;
    ObjSync() : owner(0), ownedAt(-1e9), mobile(false), updated(false), at(-1e9), prevAt(-1e9), dispOn(false), dispAt(-1e9) {
        memset(last, 0, sizeof(last)); memset(pos, 0, sizeof(pos)); memset(prev, 0, sizeof(prev)); memset(disp, 0, sizeof(disp));
    }
};
std::vector<ObjSync> g_objs;
int g_objsLevel = -2;
Bit32u g_objsFrames = 0;
std::vector<std::pair<int, Bit32s> > g_hits;            // others' hits, applied at the next frame (index, damage)
std::vector<std::pair<int, TvSpawn> > g_spawnsIn;       // others' new objects, written at the next frame (at most 64)
std::vector<char> g_spawned;                            // the slots of the objects made in flight: 2 made here, 1 told
bool g_spawnPending = false;                            // a launch of ours under way: the table as it stood before
int g_spawnFrom = -1, g_spawnCount = 0;
std::vector<char> g_spawnAlive;
const int kSpawnMax = 499;                              // (the game's own cap of its table: 0x1F3)
std::vector<std::pair<Bit32u, double> > g_remoteShots;  // our copies of others' shots (the projectile, until)
double g_objectsAt = -1e9, g_ownersAt = -1e9, g_decidedAt = -1e9;
double g_boostAt = -1e9;
double g_levelStartAt = -1e9;   // when we started flying our level (its start, a retry's)
int g_space = 0;                // where in it we are (tv_space): 0 outdoors, 1 + n in tunnel n
double g_joinAt = -1e9;         // our last start of a place (the level's start, a tunnel's way in or out)
bool g_inLevel = false;         // our campaign's level under way (from its call to its end: its briefing too)
unsigned g_levelStarts = 0;     // the level function's starts (a retry of a level is a new start: everything of the last
                                // one's forgotten, its models the game's to free)
double g_holdSince = -1;        // a guest's level held since (waiting for the host to fly)
double g_offLevelSince = -1;    // the host flying another level than ours, since
double g_deadSentFor[4] = { -1e9, -1e9, -1e9, -1e9 };   // (a seat's start we sent the gone objects for)
Bit32s g_lastFuel = 0;
unsigned g_hitsSent = 0, g_hitsTaken = 0, g_kills = 0, g_skipped = 0;
double g_nextTryMs;
double g_noticeAt = -1;
std::vector<std::pair<int, TvShot> > g_shots;   // others' shots, made at the next frame (seat, shot)
unsigned g_shotsMade = 0, g_shotsSent = 0, g_drawn = 0;
unsigned g_enemyShotsMade = 0, g_enemyShotsSent = 0;

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
    return kLooks[look_index(env && env[0] ? env : "ship.bin")];
}

// The host's craft for each seat (the page's Craft menus): WCTVLOOKS = "<seat 0's>,<seat 1's>,...", the last given
// for the seats after it ("icebrd.bin,snowftr.bin": the host an IceRider, every guest a SnowScout); none, WCTVLOOK
// for every seat.
void host_looks() {
    const char *env = getenv("WCTVLOOKS");
    std::vector<std::string> parts;
    if (env && env[0]) {
        std::string cur;
        for (const char *c = env;; c++) {
            if (*c == ',' || !*c) {
                parts.push_back(cur);
                cur.clear();
                if (!*c) break;
            } else {
                cur += *c;
            }
        }
    } else {
        parts.push_back(own_look());
    }
    for (int i = 0; i < 4; i++) g_looks[i] = kLooks[look_index(parts[std::min((size_t)i, parts.size() - 1)])];
}

void send_looks(Peer *to) {
    NetworkMessage msg;
    for (int i = 0; i < 4; i++) msg.mutable_tv_looks()->add_look(g_looks[i]);
    to->conn.send(msg);
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
    if (n->host) host_looks();
    wclog(1, "Terminal Velocity: %s at %s, %d seats, callsign %s%s", n->host ? "HOST" : "GUEST", where.c_str(), n->seats,
          n->callsign.c_str(), n->host ? (std::string(", the host's craft ") + kLookNames[look_index(g_looks[0])] + ", the guests' " +
          kLookNames[look_index(g_looks[1])]).c_str() : "");
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
    const int lastLevel = r.level;
    // (its angles' rates from its last two reports: the rates it sends are its ship's own turn rates, about its own
    // axes, not its angles'; a banked turn carried on by them wobbled)
    const double now = PIC_FullIndex(), gap = now - r.at;
    for (int k = 0; k < 3; k++) {
        const int a = k + 3 < st.pos_size() ? st.pos(k + 3) : 0;
        const double rate = wasFlying && gap > 20 && gap < 600 ? (double)(Bit16s)(Bit16u)(a - r.pos[k + 3]) * 1000.0 / gap : 0.0;
        r.arate[k] = wasFlying ? 0.5 * r.arate[k] + 0.5 * rate : 0.0;
    }
    r.known = true;
    r.at = now;
    r.flying = st.flying();
    r.level = st.level();
    if (st.has_look()) r.look = kLooks[look_index(st.look())];
    for (int i = 0; i < 6; i++) {
        r.pos[i] = i < st.pos_size() ? st.pos(i) : 0;
        r.rate[i] = i < st.rate_size() ? st.rate(i) : 0;
    }
    r.shield = st.shield();
    r.boost = st.boost();
    r.done = st.done();
    r.inLevel = st.in_level();
    const int space = st.space();
    if (wasFlying && r.flying && st.level() == lastLevel && space != r.space) {
        // (into a tunnel or out of one: a new place for it, as a start is -- told the objects gone there)
        r.flyingSince = now;
        show_line(r.callsign + (space ? (r.space ? " flew into another tunnel" : " flew into the tunnel") : " came out of the tunnel"));
    }
    r.space = space;
    if (st.has_difficulty()) r.difficulty = st.difficulty();
    if (st.has_level_name()) r.levelName = st.level_name();
    if (r.flying && (!wasFlying || st.level() != lastLevel)) r.flyingSince = PIC_FullIndex();
    if (r.flying && !wasFlying) {
        wclog(1, "Terminal Velocity: %s flies level %d (%s)", r.callsign.c_str(), r.level, st.level_name().c_str());
    }
}

// The level's objects as we know them: made again for a new level (or the same level flown again: the frame count
// starts over), grown as the level adds objects (a pickup a bunker drops: such additions can differ from machine to
// machine, the level's own objects before them are the same everywhere).
bool objs_ready() {
    const int level = (int)g_levelStarts * 64 + tv_space();     // (a level's start, or the place within it: a tunnel's
                                                                  // objects are others in the same table)
    const int n = tv_object_count();
    if (level != g_objsLevel || n < (int)g_objs.size()) {
        g_objs.clear();
        g_spawned.clear();
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

void send_all(NetworkMessage &msg, bool bestEffort);

// A line on the game's own screen, as its net game shows another player's (5 s), in flight; the emulator's line
// over the picture otherwise.
void show_line(const std::string &line0) {
    std::string line = line0.substr(0, 70);
    for (size_t i = 0; i < line.size(); i++) if ((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126) line[i] = '?';
    wclog(1, "Terminal Velocity: comms: %s", line.c_str());
    if (!tv_in_flight() || !tv_fn(TV_FN_MESSAGE)) {
        notice(line);
        return;
    }
    std::vector<Bit32u> scratch((line.size() + 4) / 4, 0);
    memcpy(&scratch[0], line.c_str(), line.size());
    std::vector<Bit32u> args;
    args.push_back(TV_SCRATCH);
    args.push_back(0x50000);
    tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_MESSAGE), args, scratch, NULL, 0);
}

// A newcomer to our level: the objects gone in the others' games go in ours (their hit points none: the objects'
// loop leaves them out, drawn no more, counted gone by the level's goals).
void take_dead(const TvDead &d) {
    const double now = PIC_FullIndex();
    if (d.level() != tv_level() || d.space() != tv_space() || !tv_in_flight() || now - std::max(g_levelStartAt, g_joinAt) > 20000) return;
    int gone = 0;
    const int n = tv_object_count();
    for (int k = 0; k < d.index_size(); k++) {
        const int i = (int)d.index(k);
        const Bit32u o = i < n ? tv_object_addr(i) : 0;
        if (o && (Bit32s)tv_r32(o + 0x1c) > 0) {
            tv_w32(o + 0x1c, 0);
            gone++;
        }
    }
    wclog(1, "Terminal Velocity: joined the level as it stands: %d of %d objects gone already", gone, d.index_size());
}

void send_dead(int forSeat) {
    NetworkMessage msg;
    TvDead *d = msg.mutable_tv_dead();
    d->set_level(tv_level());
    d->set_space(tv_space());
    const int n = tv_object_count();
    for (int i = 0; i < n; i++) {
        const Bit32u o = tv_object_addr(i);
        if (o && (Bit32s)tv_r32(o + 0x1c) <= 0) d->add_index((Bit32u)i);
    }
    send_all(msg, false);
    // ... and the objects made in flight still alive, as they are here (it has not got them)
    NetworkMessage sm;
    TvSpawn *sp = sm.mutable_tv_spawn();
    sp->set_level(tv_level());
    sp->set_space(tv_space());
    sp->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    sp->set_spawner(-1);
    sp->set_for_seat(forSeat);
    for (int i = 0; i < n && i < (int)g_spawned.size() && sp->index_size() < 40; i++) {
        const Bit32u o = tv_object_addr(i);
        if (!g_spawned[i] || !o || (Bit32s)tv_r32(o + 0x1c) <= 0) continue;
        sp->add_index((Bit32u)i);
        for (int w = 0; w < kTvObjSize / 4; w++) sp->add_record(tv_r32(o + 4 * w));
    }
    if (sp->index_size()) send_all(sm, false);
    wclog(1, "Terminal Velocity: %s starts our level: told the %d objects gone, the %d made in flight", g_net->remotes[forSeat].callsign.c_str(),
          d->index_size(), sp->index_size());
}

// An object made here, told: its slot and its whole record.
void tell_spawn(int i, int from) {
    NetworkMessage msg;
    TvSpawn *sp = msg.mutable_tv_spawn();
    sp->set_level(tv_level());
    sp->set_space(tv_space());
    sp->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    sp->set_spawner(from);
    sp->add_index((Bit32u)i);
    const Bit32u o = tv_object_addr(i);
    for (int w = 0; w < kTvObjSize / 4; w++) sp->add_record(tv_r32(o + 4 * w));
    send_all(msg, false);
}

// Others' new objects (the launcher's machine's game made them): written in our table at their slots, flown by the
// machine that made them until the owners are decided.  Two games that made one each in the same slot at once: the
// lower seat's keeps the slot, the other's machine moves its own to the table's end and tells it again.
void take_spawns(double now) {
    const int self = g_net->self < 0 ? 0 : g_net->self;
    for (size_t k = 0; k < g_spawnsIn.size(); k++) {
        const int seat = g_spawnsIn[k].first;
        const TvSpawn &sp = g_spawnsIn[k].second;
        if (sp.level() != tv_level() || sp.space() != tv_space() || !tv_in_flight() || !objs_ready()) continue;
        if (sp.has_for_seat() && (sp.for_seat() != self || now - std::max(g_levelStartAt, g_joinAt) > 20000)) continue;
        const int words = kTvObjSize / 4;
        const Bit32u table = tv_r32(tv_data(tvsig::D_OBJ_TABLE)), countAt = tv_data(tvsig::D_OBJ_COUNT);
        int made = 0;
        for (int j = 0; j < sp.index_size() && (j + 1) * words <= sp.record_size(); j++) {
            const int i = (int)sp.index(j);
            if (i < 0 || i >= kSpawnMax) continue;
            const int n = (int)tv_r32(countAt);
            if (i < n && i < (int)g_spawned.size() && g_spawned[i] == 2 && (Bit32s)tv_r32(table + (Bit32u)i * kTvObjSize + 0x1c) > 0) {
                if (self < seat) continue;          // (ours stays: theirs comes again elsewhere)
                const Bit32u from = table + (Bit32u)i * kTvObjSize;
                if (n < kSpawnMax && objs_ready() && i < (int)g_objs.size()) {
                    for (int w = 0; w < words; w++) tv_w32(table + (Bit32u)n * kTvObjSize + 4 * w, tv_r32(from + 4 * w));
                    tv_w32(countAt, (Bit32u)(n + 1));
                    objs_ready();
                    if (n < (int)g_objs.size()) g_objs[n] = g_objs[i];
                    if ((int)g_spawned.size() <= n) g_spawned.resize(n + 1, 0);
                    g_spawned[n] = 2;
                    tell_spawn(n, -3);
                    wclog(1, "Terminal Velocity: our object %d and %s's made at once in one slot: ours moved to %d", i,
                          g_net->remotes[seat].callsign.c_str(), n);
                }
            }
            const int n2 = (int)tv_r32(countAt);
            for (int g = n2; g < i; g++) tv_w32(table + (Bit32u)g * kTvObjSize + 0x1c, 0);     // (a gap: none there)
            for (int w = 0; w < words; w++) tv_w32(table + (Bit32u)i * kTvObjSize + 4 * w, sp.record(j * words + w));
            if (i >= n2) tv_w32(countAt, (Bit32u)(i + 1));
            objs_ready();
            if (i >= (int)g_objs.size()) continue;
            ObjSync fresh;
            const int c = tv_object_class(i);
            fresh.mobile = c == 7 || c == 10;
            fresh.owner = seat;
            fresh.ownedAt = now;
            g_objs[i] = fresh;
            if ((int)g_spawned.size() <= i) g_spawned.resize(i + 1, 0);
            g_spawned[i] = 1;
            made++;
        }
        if (made) {
            wclog(1, "Terminal Velocity: %d object%s made by %s's game written here (%s, slot %d '%s')", made, made == 1 ? "" : "s",
                  g_net->remotes[seat].callsign.c_str(), sp.spawner() == -1 ? "a catch-up" : sp.spawner() == -2 ? "its reinforcements" : sp.spawner() == -3 ? "moved" : "a launch", (int)sp.index(0),
                  tv_object_name((int)sp.index(0)).c_str());
        }
    }
    g_spawnsIn.clear();
}

bool i_decide(double now);

void take_objects(const TvObjects &o, int seat) {
    if (o.level() != tv_level() || o.space() != tv_space() || tv_space() != 0 || !objs_ready()) return;
    const double now = PIC_FullIndex();
    const bool decider = i_decide(now);
    const int self = g_net->self < 0 ? 0 : g_net->self;
    for (int k = 0; k < o.objects_size(); k++) {
        const TvObject &ob = o.objects(k);
        const int i = (int)ob.index();
        if (i < 0 || i >= (int)g_objs.size() || ob.pos_size() != 6) continue;
        ObjSync &os = g_objs[i];
        if (os.owner != seat) {
            // (a report from a machine that flew it before the host's word reached it: the host's word stands; the host
            // takes it as the owner's while it decides nothing else)
            if (!decider || os.owner == self) continue;
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
    if (w.level() != tv_level() || w.space() != tv_space() || tv_space() != 0 || !objs_ready()) return;
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
            send_looks(p);
            char line[160];
            snprintf(line, sizeof(line), "%s joined as pilot %d of %d (in a %s)", p->callsign.c_str(), player + 1, g_net->seats,
                     kLookNames[look_index(g_looks[player])]);
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
    } else if (msg.has_chat()) {
        if (!p->hello) return;
        show_line(msg.chat().callsign() + ": " + msg.chat().message());
        relay(p, [&](Peer *to) { to->conn.send(msg); });
    } else if (msg.has_tv_looks()) {
        if (!p->hello || g_net->host) return;
        for (int i = 0; i < 4 && i < msg.tv_looks().look_size(); i++) g_looks[i] = kLooks[look_index(msg.tv_looks().look(i))];
        wclog(1, "Terminal Velocity: the host's craft: %s for the host, %s for the guests", kLookNames[look_index(g_looks[0])],
              kLookNames[look_index(g_looks[1])]);
    } else if (msg.has_tv_dead()) {
        if (!p->hello) return;
        take_dead(msg.tv_dead());
        NetworkMessage out;
        *out.mutable_tv_dead() = msg.tv_dead();
        relay(p, [&](Peer *to) { to->conn.send(out); });
    } else if (msg.has_tv_spawn()) {
        if (!p->hello) return;
        const int seat = g_net->host ? p->player : (int)msg.tv_spawn().player();
        if (g_spawnsIn.size() < 64) g_spawnsIn.push_back(std::make_pair(seat, msg.tv_spawn()));   // (a list has a cap)
        NetworkMessage out;
        *out.mutable_tv_spawn() = msg.tv_spawn();
        out.mutable_tv_spawn()->set_player((Bit32u)seat);
        relay(p, [&](Peer *to) { to->conn.send(out); });
    } else if (msg.has_tv_hit()) {
        if (!p->hello) return;
        const TvHit &h = msg.tv_hit();
        if (h.level() == tv_level() && h.space() == tv_space() && g_hits.size() < 256) g_hits.push_back(std::make_pair((int)h.index(), (Bit32s)h.damage()));
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
        // (the decider's word: the host's, or a guest's while the host is not settled outdoors -- the host relays it)
        if (!p->hello) return;
        take_owners(msg.tv_owners());
        relay(p, [&](Peer *to) { to->conn.send(msg); });
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
            RecvStatus st = p->conn.poll(CAT_CHAT, msg);        // (the 0 key's lines go on a channel of their own)
            if (!st.ok()) break;
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
    st->set_done(flying && tv_r32(tv_data(tvsig::D_LEVEL_DONE)) != 0);
    st->set_in_level(g_inLevel);
    st->set_space(tv_space());
    st->set_difficulty((Bit32s)tv_r32(tv_data(tvsig::D_DIFFICULTY)));
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

// The level's own craft (its first type of class 7 among its objects): its model awake (the type's +4).
Bit32u planet_model() {
    const int n = tv_object_count();
    for (int i = 0; i < n; i++) {
        if (tv_object_class(i) == 7) return tv_r32(tv_object_type_addr(i) + 4);
    }
    return 0;
}
Bit32u fallback_model() {
    const Bit32u m = planet_model();
    return m ? m : tv_r32(tv_data(tvsig::D_SHIP_MODEL));
}

void model_loaded(int look, Bit32u eax) {
    if (!g_net || look < 0 || look > kLookCount) return;
    g_net->models[look] = eax;
    g_net->modelState[look] = eax ? MODEL_READY : MODEL_MISSING;
    wclog(1, "Terminal Velocity: the model %s loaded: %08x", model_name(look), (unsigned)eax);
}

std::vector<Bit32u> name_scratch(const std::string &name) {
    std::vector<Bit32u> scratch((name.size() + 4) / 4, 0);
    memcpy(&scratch[0], name.c_str(), name.size());
    return scratch;
}

// The archives' answer for a model: its size (0: not there, never to be loaded: the loader would end the program).
void model_looked(int look, Bit32u eax) {
    if (!g_net || look < 0 || look > kLookCount) return;
    if (!eax) {
        g_net->modelState[look] = MODEL_MISSING;
        wclog(1, "Terminal Velocity: the model %s is not in this game's archives: %s", model_name(look),
              look == kLookCount ? "no flame" : "drawn as the level's own craft");
        return;
    }
    g_net->modelState[look] = MODEL_LOADING;
    tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_LOAD_MODEL), std::vector<Bit32u>(1, TV_SCRATCH), name_scratch(model_name(look)), model_loaded, look);
}

// The model a look is drawn with in this level (kLookCount: the flame): the TV-202 is the player's own ship's (ship.bin,
// loaded with the level), "planet" the level's own craft; another is looked up in the archives and loaded once a
// level at a frame's top, drawn as the level's own craft meanwhile and when the archives have none.
Bit32u model_for(int look) {
    if (look == LOOK_TV202) return tv_r32(tv_data(tvsig::D_SHIP_MODEL));
    if (look == LOOK_PLANET) return fallback_model();
    int &st = g_net->modelState[look];
    if (st == MODEL_READY) return g_net->models[look];
    if (st == MODEL_UNKNOWN) {
        const Bit32u lookup = tv_fn(TV_FN_POD_LOOKUP);
        if (lookup) {
            st = MODEL_LOOKING;
            std::vector<Bit32u> args;
            args.push_back(tv_data(tvsig::D_POD_TABLE));
            args.push_back(TV_SCRATCH);
            tv_queue_call(TV_AT_FRAME, lookup, args, name_scratch(model_name(look)), model_looked, look);
        } else {
            st = MODEL_MISSING;     // (no lookup known: never a blind load)
        }
    }
    return look == kLookCount ? 0 : fallback_model();
}

// One of the others' shots made here: the projectile kept as theirs (its hits are told by its shooter).
void shot_made(int seat, Bit32u eax) {
    (void)seat;
    g_shotsMade++;
    if (eax && g_remoteShots.size() < 256) g_remoteShots.push_back(std::make_pair(eax, PIC_FullIndex() + kRemoteShotMs));
}

void enemy_shot_made(int seat, Bit32u eax) {
    (void)seat;
    g_enemyShotsMade++;
    if (eax && g_remoteShots.size() < 256) g_remoteShots.push_back(std::make_pair(eax, PIC_FullIndex() + kRemoteShotMs));
}

// Another pilot flying our level in our place (a tunnel or outdoors), reports fresh: worth telling an enemy's shot.
bool others_here() {
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s != g_net->self && r.known && r.flying && r.level == tv_level() && r.space == tv_space() && now - r.at < kStateStaleMs) return true;
    }
    return false;
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

// A pilot settled outdoors in our level: flying it, out of the tunnels, for kSettleMs (a pilot just out of a tunnel
// has the outdoor objects as they were when it went in: the game keeps them in enemy.sav meanwhile).
bool settled_here(double now) {
    return tv_space() == 0 && tv_in_flight() && now - std::max(g_levelStartAt, g_joinAt) >= kSettleMs;
}
bool settled_remote(const Remote &r, double now) {
    return r.known && r.flying && r.level == tv_level() && r.space == 0 && now - r.at < kStateStaleMs &&
           now - r.flyingSince >= kSettleMs;
}

// Who decides the owners: the host while it is settled outdoors in our level; while it is not (in a tunnel, dead, a
// newcomer) the lowest seat that is.
bool i_decide(double now) {
    if (!g_net || !settled_here(now)) return false;
    for (int s = 0; s < g_net->self && s < kMaxSeats; s++) {
        if (settled_remote(g_net->remotes[s], now)) return false;
    }
    return true;
}

// The decider: each moving object near a pilot to the nearest pilot's machine (an object changes hands only when the
// other is a fifth nearer, and not twice in kOwnerHoldMs); the objects of a pilot not settled outdoors to those that
// are.  The word sent every kOwnersEveryMs, at each change, and at once to a pilot new to the place (one out of a
// tunnel has its owners forgotten: it learns them before it decides).
void decide_owners(double now) {
    TvShip me;
    tv_player(&me);
    Bit32s where[kMaxSeats][3];
    bool flying[kMaxSeats];
    for (int s = 0; s < kMaxSeats; s++) {
        flying[s] = false;
        if (s == g_net->self) {
            flying[s] = true;
            memcpy(where[s], me.pos, sizeof(where[s]));
        } else {
            const Remote &r = g_net->remotes[s];
            flying[s] = settled_remote(r, now);
            for (int k = 0; k < 3; k++) where[s][k] = r.pos[k];
        }
    }
    bool changed = false;
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s != g_net->self && r.known && r.space == 0 && r.flyingSince > g_ownersAt) changed = true;
    }
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
        const bool ownerHere = os.owner >= 0 && os.owner < kMaxSeats && flying[os.owner];
        if (best < 0 || best == os.owner || now - os.ownedAt < kOwnerHoldMs) continue;
        if (ownerHere && bd > kOwnRange * kOwnRange) continue;     // (an owner gone from the level gives up its objects)
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
    w->set_space(0);
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
    // (in a tunnel each game flies the tunnel's craft itself; a pilot new to the place has old copies)
    if (tv_space() != 0 || PIC_FullIndex() - std::max(g_levelStartAt, g_joinAt) < kSettleMs) return;
    NetworkMessage msg;
    TvObjects *ob = msg.mutable_tv_objects();
    ob->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    ob->set_level(tv_level());
    ob->set_space(0);
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
const double kEaseMs = 120.0, kSnapUnits = 20.0;
double angle_diff(double a, double b) { return (double)(Bit16s)(Bit16u)(Bit32s)lrint(a - b) + ((a - b) - lrint(a - b)); }

void ease(double disp[6], const double target[6], const double rate[6], double fdtMs, bool &on) {
    double gap2 = 0;
    for (int k = 0; k < 3; k++) {
        const double d = (target[k] - disp[k]) / kUnit;
        gap2 += d * d;
    }
    if (!on || fdtMs <= 0 || fdtMs > 500 || gap2 > kSnapUnits * kSnapUnits) {
        memcpy(disp, target, sizeof(double) * 6);
        on = true;
        return;
    }
    const double a = 1.0 - exp(-fdtMs / kEaseMs);
    for (int k = 0; k < 6; k++) {
        disp[k] += rate[k] * fdtMs / 1000.0;
        const double gap = k < 3 ? target[k] - disp[k] : angle_diff(target[k], disp[k]);
        disp[k] += gap * a;
    }
}

// The puppets where their owners' reports put them, carried on to now by their last two reports, smoothed as the
// other pilots' ships are (the reports come ten times a second: a craft jumped at each).
void place_puppets(double now) {
    for (size_t i = 0; i < g_objs.size(); i++) {
        ObjSync &os = g_objs[i];
        if (!is_puppet((int)i)) {
            os.dispOn = false;
            continue;
        }
        const Bit32u o = tv_object_addr((int)i);
        double target[6], rate[6] = { 0, 0, 0, 0, 0, 0 };
        const double span = os.at - os.prevAt;
        const bool moving = os.prevAt > 0 && span > 10 && span < 1000;
        const double f = moving ? std::min(now - os.at, 500.0) / span : 0.0;
        for (int k = 0; k < 3; k++) {
            target[k] = os.pos[k] + (double)(os.pos[k] - os.prev[k]) * f;
            rate[k] = moving ? (double)(os.pos[k] - os.prev[k]) * 1000.0 / span : 0.0;
        }
        for (int k = 3; k < 6; k++) target[k] = os.pos[k];
        ease(os.disp, target, rate, now - os.dispAt, os.dispOn);
        os.dispAt = now;
        for (int k = 0; k < 3; k++) tv_w32(o + 4 * k, (Bit32u)(Bit32s)lrint(os.disp[k]));
        for (int k = 3; k < 6; k++) tv_w32(o + 4 * k, (Bit32u)(Bit32s)(Bit16s)(Bit16u)(Bit32s)lrint(os.disp[k]));
    }
}

// The level flown together: any pilot's level done (its exit flown) ends everybody's, as done; a guest whose host flies
// another level for 3 s ends its own (the campaign's next call takes the host's); a pilot who starts our level after us
// (a retry after a death, a late start) is told the objects already gone.
void level_together(double now, int level) {
    const Bit32u doneAt = tv_data(tvsig::D_LEVEL_DONE);
    const bool done = tv_r32(doneAt) != 0;
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == g_net->self || !r.known || now - r.at > kStateStaleMs || !r.flying) continue;
        if (r.level == level && r.done && !done) {
            tv_w32(doneAt, 1);
            notice(r.callsign + " flew the exit: the level is done");
        }
        if (r.level == level && r.space == tv_space() && r.flyingSince > std::max(g_levelStartAt, g_joinAt) + 3000 &&
            r.flyingSince > g_deadSentFor[s]) {
            g_deadSentFor[s] = r.flyingSince;
            send_dead(s);
        }
    }
    const Remote &h = g_net->remotes[0];
    if (!g_net->host && h.known && now - h.at < kStateStaleMs && h.inLevel && h.level != level && !done) {
        if (g_offLevelSince < 0) g_offLevelSince = now;
        if (now - g_offLevelSince > 3000) {
            tv_w32(doneAt, 1);
            g_offLevelSince = -1;
            notice(h.callsign + " flies another level: yours ends, theirs is next");
        }
    } else {
        g_offLevelSince = -1;
    }
}

}  // namespace

// A guest's campaign: the host's level, at the host's difficulty, once the host flies one (held meanwhile, the
// overlay saying so); the host's own, and a guest alone, as the game has it.
bool tv_session_level_call(int *index) {
    if (!g_net || g_net->host) return false;
    const double now = PIC_FullIndex();
    bool hostHere = false;
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        hostHere = hostHere || (g_net->peers[i]->hello && g_net->peers[i]->player == 0 && g_net->peers[i]->conn.is_open());
    }
    if (!hostHere) return false;
    const Remote &h = g_net->remotes[0];
    if (h.known && now - h.at < kStateStaleMs && (h.flying || h.inLevel) && h.level >= 0) {
        if (*index != h.level) wclog(1, "Terminal Velocity: the host's level %d (%s) flown, not %d", h.level, h.levelName.c_str(), *index);
        *index = h.level;
        if (!h.levelName.empty() && h.levelName.size() < 0x27) {
            // (the host's level file under that index: the same game, an episode of other levels chosen here)
            const Bit32u at = tv_data(tvsig::D_LEVEL_NAMES) + (Bit32u)h.level * 0x28;
            Bit32u words[10] = { 0 };
            memcpy(words, h.levelName.c_str(), h.levelName.size());
            for (int k = 0; k < 10; k++) tv_w32(at + 4 * k, words[k]);
        }
        if (h.difficulty >= 0) tv_w32(tv_data(tvsig::D_DIFFICULTY), (Bit32u)h.difficulty);
        notice("Flying " + (h.levelName.empty() ? std::string("the host's level") : h.levelName) + " with " + h.callsign);
        g_holdSince = -1;
        return false;
    }
    if (g_holdSince < 0) {
        g_holdSince = now;
        notice("Waiting for " + (h.callsign.empty() ? std::string("the host") : h.callsign) + " to launch a level: you fly it together");
    }
    if (!h.known && now - g_holdSince > 15000) {
        g_holdSince = -1;
        return false;               // (no word from the host at all: as the game has it)
    }
    if (g_noticeAt >= 0 && now - g_noticeAt > kNoticeMs - 1000) g_noticeAt = now;     // (the line stays while it waits)
    return true;
}

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
                     "hits sent %u taken %u skipped %u, stages %u; objects it flies %d, puppets here %d; enemies' shots made %u sent %u",
                  s, r.callsign.c_str(), r.flying ? "flying" : "not flying", r.level, r.pos[0], r.pos[1], r.pos[2], now - r.at,
                  r.boost ? ", BOOSTING" : "", g_shotsMade, g_shotsSent, g_drawn, g_hitsSent, g_hitsTaken, g_skipped, g_kills, owned, puppets,
                  g_enemyShotsMade, g_enemyShotsSent);
        }
    }
}

void tv_session_frame() {
    if (!g_net) return;
    const double now = PIC_FullIndex();
    // a new start of a level: its models are the game's to free; ours loaded again
    const int level = tv_level();
    static unsigned modelsStart = 0;
    if (g_levelStarts != modelsStart) {
        modelsStart = g_levelStarts;
        memset(g_net->models, 0, sizeof(g_net->models));
        memset(g_net->modelState, 0, sizeof(g_net->modelState));
        g_offLevelSince = -1;
    }
    // into a tunnel or out of one: the object table is the place's (the game swaps it through enemy.sav/enemy2.sav),
    // what we knew of the last place's objects forgotten, the others asked for the gone ones here (they see our place
    // change), our models loaded again (the tunnel loads types of its own)
    if (tv_space() != g_space) {
        wclog(1, "Terminal Velocity: %s", tv_space() ? "into a tunnel" : "out of the tunnel");
        g_space = tv_space();
        g_joinAt = now;
        g_hits.clear();
        g_remoteShots.clear();
        g_shots.clear();
        memset(g_net->models, 0, sizeof(g_net->models));
        memset(g_net->modelState, 0, sizeof(g_net->modelState));
        g_net->stateAt = -1e9;
    }
    level_together(now, level);
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
    // others' new objects, written; others' hits, applied
    take_spawns(now);
    for (size_t i = 0; i < g_hits.size(); i++) apply_hit(g_hits[i].first, g_hits[i].second);
    g_hits.clear();
    place_puppets(now);
    if (now - g_objectsAt >= kObjectsEveryMs) {
        g_objectsAt = now;
        send_objects();
    }
    if (now - g_decidedAt >= kDecideEveryMs) {
        g_decidedAt = now;
        const bool decide = i_decide(now);
        static bool decided = false;
        if (decide != decided && (decide || !g_net->host)) {
            int mine = 0, mobile = 0;
            for (size_t i = 0; i < g_objs.size(); i++) {
                if (!g_objs[i].mobile) continue;
                mobile++;
                if (g_objs[i].owner == (g_net->self < 0 ? 0 : g_net->self)) mine++;
            }
            wclog(1, "Terminal Velocity: %s the owners (%d of %d moving objects ours)", decide ? "we decide" : "we no longer decide", mine, mobile);
        }
        decided = decide;
        if (decide) decide_owners(now);
    }
    // the others' shots, made as our own player's (they fly here as there; their hits are their shooter's to tell)
    for (size_t i = 0; i < g_shots.size(); i++) {
        const TvShot &sh = g_shots[i].second;
        if (sh.posmat_size() != 16 || sh.space() != tv_space()) continue;
        std::vector<Bit32u> scratch(16);
        for (int k = 0; k < 16; k++) scratch[k] = (Bit32u)sh.posmat(k);
        std::vector<Bit32u> args;
        args.push_back(TV_SCRATCH);
        args.push_back(sh.weapon());
        args.push_back((Bit32u)sh.damage());
        args.push_back(sh.owner());
        args.push_back(sh.kind());
        tv_queue_call(TV_AT_FRAME, tv_fn(TV_FN_FIRE), args, scratch, sh.owner() ? enemy_shot_made : shot_made, g_shots[i].first);
    }
    g_shots.clear();
}

void make_markers(double now);

// What we draw of another pilot's ship, smoothed (the user: "the other ship is kinda bouncing around ... visual
// smoothing of the locations at the other end. We don't need to alter physics"): its reports come some 15 a second, not
// evenly, and drawing each where its last report and its rates put it jumped at every report.  Now the drawn place is
// carried on each frame by the ship's velocity and its angles' rates, then eased towards where the last report puts
// it now by a share of the gap (a time constant of 0.12 s: within some 0.2 s of the report); a gap of more than 20
// world units (a jump: a level's start, a retry) is closed at once.  Only the drawing: the game is not touched.
void smooth_remote(Remote &r, double now) {
    const double dt = std::min(now - r.at, 500.0) / 1000.0;
    double target[6], rate[6];
    for (int k = 0; k < 3; k++) {
        target[k] = r.pos[k] + r.rate[k] * dt;
        target[k + 3] = r.pos[k + 3] + r.arate[k] * dt;
        rate[k] = r.rate[k];
        rate[k + 3] = r.arate[k];
    }
    ease(r.disp, target, rate, now - r.dispAt, r.dispOn);
    r.dispAt = now;
    static const bool log = getenv("WCNET_TV_SMOOTHLOG") != NULL;     // (a rig's: the drawn place against the raw one)
    if (log) {
        const double rdt = std::min(now - r.at, (double)kCarryMs) / 1000.0;
        wclog(1, "Terminal Velocity: smooth %.1f raw %.0f %.0f %.0f %d drawn %.0f %.0f %.0f %.0f", now, r.pos[0] + r.rate[0] * rdt,
              r.pos[1] + r.rate[1] * rdt, r.pos[2] + r.rate[2] * rdt, (int)(r.pos[5] + r.rate[5] * rdt), r.disp[0], r.disp[1], r.disp[2], r.disp[5]);
    }
}

void tv_session_draw() {
    if (!g_net || !tv_in_flight()) return;
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        Remote &r = g_net->remotes[s];
        if (s != g_net->self && r.known && r.flying && r.space == tv_space() && now - r.at <= kStateStaleMs) smooth_remote(r, now);
        else r.dispOn = false;
    }
    make_markers(now);
    const int level = tv_level();
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == g_net->self || !r.known || !r.flying || r.level != level || r.space != tv_space() || now - r.at > kStateStaleMs) continue;
        const int look = look_index(g_looks[s]);
        const Bit32u model = model_for(look);
        if (!model) continue;
        std::vector<Bit32u> scratch(6);
        for (int k = 0; k < 3; k++) scratch[k] = (Bit32u)(Bit32s)lrint(r.disp[k]);
        for (int k = 3; k < 6; k++) scratch[k] = (Bit32u)(Bit32s)(Bit16s)(Bit16u)(Bit32s)lrint(r.disp[k]);
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

// A launcher (or a generator) about to make an object: only the game of the pilot nearest it makes one (each
// game's launchers run their own clocks: with two pilots near one, both would launch, and a craft made in one game
// only is fought by one pilot only); the one made is told to the others (tv_session_spawn_end).  Alone, the game
// as it is.
// (the table as it stands before the game makes an object: the made one found after)
static void spawn_snapshot(int from) {
    g_spawnPending = true;
    g_spawnFrom = from;
    g_spawnCount = tv_object_count();
    g_spawnAlive.assign(g_spawnCount, 0);
    for (int i = 0; i < g_spawnCount; i++) g_spawnAlive[i] = (Bit32s)tv_r32(tv_object_addr(i) + 0x1c) > 0;
}

bool tv_session_spawn_begin(int spawner) {
    g_spawnPending = false;
    if (!g_net || !objs_ready()) return true;
    const Bit32u o = tv_object_addr(spawner);
    if (!o) return true;
    const double now = PIC_FullIndex();
    const int self = g_net->self < 0 ? 0 : g_net->self;
    Bit32s at[3];
    for (int k = 0; k < 3; k++) at[k] = (Bit32s)tv_r32(o + 4 * k);
    TvShip me;
    tv_player(&me);
    const double mine = dist2(at, me.pos);
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == self || !r.known || !r.flying || r.level != tv_level() || r.space != tv_space() || now - r.at > kStateStaleMs) continue;
        const Bit32s rp[3] = { r.pos[0], r.pos[1], r.pos[2] };
        const double d = dist2(at, rp);
        if (d < mine || (d == mine && s < self)) {
            wclog(1, "Terminal Velocity: object %d's launch left to %s's game (nearer it)", spawner, r.callsign.c_str());
            return false;
        }
    }
    spawn_snapshot(spawner);
    return true;
}

// The level's reinforcements (each game's, every 4 s, a craft put near its own pilot while few hostiles are awake
// near it): pilots near one another (their awake squares overlapping: within 11 of the game's 16-unit cells) have one
// reinforcer between them, the lowest seat's; a pilot apart has its own, as alone.  The reinforcer's game reinforces
// the group as so many pilots (the user: "Were we able to up enemy spawns due to ally?"): its cap on the hostiles
// awake times the pilots, its clock wound as fast times the pilots (wcnet_tv.cpp), each new craft put about the next
// pilot of the group in turn.
int g_groupSeats[kMaxSeats];
int g_groupSize = 0;
unsigned g_reinforcements = 0;

// (the pilots in our group, ourselves first; false when a lower seat's game reinforces it)
bool reinforce_group(bool log) {
    g_groupSize = 0;
    const int self = g_net->self < 0 ? 0 : g_net->self;
    g_groupSeats[g_groupSize++] = self;
    const double now = PIC_FullIndex(), reach = 11.0 * 16 * kUnit;
    TvShip me;
    tv_player(&me);
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == self || !r.known || !r.flying || r.level != tv_level() || r.space != tv_space() || now - r.at > kStateStaleMs) continue;
        if (fabs((double)r.pos[0] - me.pos[0]) >= reach || fabs((double)r.pos[2] - me.pos[2]) >= reach) continue;
        if (s < self) {
            if (log) wclog(1, "Terminal Velocity: the level's reinforcements here left to %s's game (it is near)", r.callsign.c_str());
            return false;
        }
        g_groupSeats[g_groupSize++] = s;
    }
    return true;
}

bool tv_session_reinforce_begin() {
    g_spawnPending = false;
    if (!g_net || !objs_ready()) return true;
    if (!reinforce_group(true)) return false;
    spawn_snapshot(-2);
    return true;
}

int tv_session_reinforce_pilots() {
    if (!g_net || !tv_in_flight()) return 1;
    return reinforce_group(false) ? g_groupSize : 0;
}

void tv_session_spawn_end() {
    if (!g_spawnPending) return;
    g_spawnPending = false;
    if (!g_net) return;
    const int n = tv_object_count();
    int made = -1;
    for (int i = 0; i < n && made < 0; i++) {
        const Bit32u o = tv_object_addr(i);
        if (o && (Bit32s)tv_r32(o + 0x1c) > 0 && (i >= g_spawnCount || !g_spawnAlive[i])) made = i;
    }
    if (made < 0 || made >= kSpawnMax || !objs_ready() || made >= (int)g_objs.size()) return;
    const double now = PIC_FullIndex();
    const int self = g_net->self < 0 ? 0 : g_net->self;
    ObjSync fresh;
    const int c = tv_object_class(made);
    fresh.mobile = c == 7 || c == 10;
    fresh.owner = self;
    fresh.ownedAt = now;
    g_objs[made] = fresh;
    if ((int)g_spawned.size() <= made) g_spawned.resize(made + 1, 0);
    g_spawned[made] = 2;
    std::string about;
    if (g_spawnFrom == -2 && reinforce_group(false) && g_groupSize > 1) {
        // (a group's reinforcement: about the next pilot in turn, where the game put it about ours -- as high above
        // that pilot as it was above ours, or level with it)
        const int s = g_groupSeats[g_reinforcements++ % (unsigned)g_groupSize];
        if (s != self) {
            const Remote &r = g_net->remotes[s];
            TvShip me;
            tv_player(&me);
            const Bit32u o = tv_object_addr(made);
            const Bit32s above = std::max<Bit32s>(0, (Bit32s)tv_r32(o + 4) - me.pos[1]);
            tv_w32(o + 0, (Bit32u)((Bit32s)tv_r32(o + 0) - me.pos[0] + r.pos[0]));
            tv_w32(o + 4, (Bit32u)(r.pos[1] + above));
            tv_w32(o + 8, (Bit32u)((Bit32s)tv_r32(o + 8) - me.pos[2] + r.pos[2]));
            about = " about " + r.callsign;
        }
    }
    tell_spawn(made, g_spawnFrom);
    wclog(1, "Terminal Velocity: object %d '%s' made here (%s%s): told", made, tv_object_name(made).c_str(),
          g_spawnFrom == -2 ? "the level's reinforcements" : tv_object_name(g_spawnFrom).c_str(), about.c_str());
}

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

void tv_session_fired(const Bit32u posmat[16], Bit32u weapon, Bit32u damage, Bit32u owner, Bit32u kind) {
    if (!g_net) return;
    if (owner != 0 && !others_here()) return;      // (an enemy's shot: only for a pilot in our place to see)
    NetworkMessage msg;
    TvShot *sh = msg.mutable_tv_shot();
    sh->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    for (int i = 0; i < 16; i++) sh->add_posmat((Bit32s)posmat[i]);
    sh->set_space(tv_space());
    sh->set_weapon(weapon);
    sh->set_damage((Bit32s)damage);
    sh->set_kind(kind);
    if (owner) sh->set_owner(owner);
    send_all(msg, false);
    if (owner) g_enemyShotsSent++;
    else g_shotsSent++;
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
    h->set_space(tv_space());
    h->set_index((Bit32u)obj);
    h->set_damage(damage);
    send_all(msg, false);
    g_hitsSent++;
    return true;
}

void tv_session_level(int index, const std::string &name) {
    (void)index;
    (void)name;
    g_levelStarts++;
    g_levelStartAt = PIC_FullIndex();
    g_inLevel = true;
    if (g_net) g_net->stateAt = -1e9;       // (the others told at once: a guest held waits for this)
}

void tv_session_level_end() {
    g_inLevel = false;
    if (g_net) g_net->stateAt = -1e9;
}

void tv_session_shutdown() { shutdown(); }

bool tv_session_active() { return g_net != NULL; }

void tv_session_chat(const std::string &text) {
    if (!g_net || text.empty()) return;
    NetworkMessage msg;
    Chat *chat = msg.mutable_chat();
    chat->set_ship_id(0);
    chat->set_message(text);
    chat->set_callsign(g_net->callsign);
    send_all(msg, false);
    show_line(g_net->callsign + ": " + text);
}

// The others marked on our picture.  The camera is our ship's (the cockpit's view: the view word 0): its place and its
// fixed-point attitude matrix (columns right, up, forward); the game's projection is 90 degrees across (its
// visibility test keeps |x| <= z), square pixels in the 640 x 480 mode: a point at camera x, y, z is at
// (0.5 + x/z/2) of the picture's width and (0.5 - y/z/2 * 640/480) of its height (WCNET_TV_PROJ="<f>:<cx>:<cy>" in
// shares, to tune).
TvMarker g_markers[kTvMarkers];

void make_markers(double now) {
    memset(g_markers, 0, sizeof(g_markers));
    if (!g_net || !tv_in_flight() || tv_r32(tv_data(tvsig::D_VIEW_MODE)) != 0) return;
    static double f = 0.5, cx = 0.5, cy = 0.5;
    static bool read = false;
    if (!read) {
        read = true;
        const char *e = getenv("WCNET_TV_PROJ");
        if (e) sscanf(e, "%lf:%lf:%lf", &f, &cx, &cy);
    }
    const double aspect = 640.0 / 480.0;
    TvShip me;
    tv_player(&me);
    const Bit32u m = tv_data(tvsig::D_PLAYER_POS) + 0x1c;
    double M[9];
    for (int k = 0; k < 9; k++) M[k] = (double)(Bit32s)tv_r32(m + 4 * k) / kUnit;
    const int level = tv_level();
    for (int s = 0, n = 0; s < kMaxSeats && n < kTvMarkers; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == g_net->self || !r.known || !r.flying || r.level != level || r.space != tv_space() || now - r.at > kStateStaleMs) continue;
        double d[3];
        for (int k = 0; k < 3; k++) d[k] = (r.disp[k] - me.pos[k]) / kUnit;
        // (the matrix's columns: right = (M0, M3, M6), up = (M1, M4, M7), forward = (M2, M5, M8))
        const double x = d[0] * M[0] + d[1] * M[3] + d[2] * M[6];
        const double y = d[0] * M[1] + d[1] * M[4] + d[2] * M[7];
        const double z = d[0] * M[2] + d[1] * M[5] + d[2] * M[8];
        const double dist = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        TvMarker &mk = g_markers[n++];
        mk.on = true;
        snprintf(mk.label, sizeof(mk.label), "%s %.0f", r.callsign.empty() ? "PILOT" : r.callsign.c_str(), dist);
        double sx = 0, sy = 0;
        if (z > 0.3) {
            sx = cx + x / z * f;
            sy = cy - y / z * f * aspect;
        }
        mk.onScreen = z > 0.3 && sx > 0.02 && sx < 0.98 && sy > 0.08 && sy < 0.98;
        if (mk.onScreen) {
            mk.x = sx;
            mk.y = sy;
            mk.size = std::max(0.02, std::min(0.25, 1.6 / std::max(z, 0.5) * f));    // (the ship some 3 units across)
        } else {
            // the edge, the way to turn: the direction in the picture (behind: the other way)
            double ax = x, ay = y * aspect;
            if (z <= 0.3 && fabs(ax) < 1e-6 && fabs(ay) < 1e-6) ay = -1;
            const double len = sqrt(ax * ax + ay * ay);
            ax /= len; ay /= len;
            const double k = std::min(0.45 / std::max(fabs(ax), 1e-6), 0.42 / std::max(fabs(ay), 1e-6));
            mk.x = 0.5 + ax * k;
            mk.y = 0.53 - ay * k;
            mk.size = 0.03;
        }
    }
    if (wclab_on) {
        // (into a recording: the video draws them as the overlay does, scripts/tvnet-video.py)
        std::string j = "{\"t\":\"tvmark\",\"m\":[";
        for (int i = 0; i < kTvMarkers && g_markers[i].on; i++) {
            char b[160];
            snprintf(b, sizeof(b), "%s[%.4f,%.4f,%.4f,%d,\"%s\"]", i ? "," : "", g_markers[i].x, g_markers[i].y, g_markers[i].size,
                     g_markers[i].onScreen ? 1 : 0, g_markers[i].label);
            j += b;
        }
        lab_event(j + "]}");
    }
}

const TvMarker *tv_markers() { return g_markers; }

bool tv_session_partner(TvShip *out) {
    memset(out, 0, sizeof(*out));
    if (!g_net) return false;
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        const Remote &r = g_net->remotes[s];
        if (s == g_net->self || !r.known || !r.flying || r.level != tv_level() || r.space != tv_space() || now - r.at > kStateStaleMs) continue;
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
