/*
 *  The TIE Fighter demo's room (wcnet_tiedemo.h, docs/tiedemo.md).
 *
 *  The CD version's session (wcnet_tie_session.cpp, docs/tiefighter.md sections 5 to 7), ported: the demo is an earlier
 *  build of the same game, its craft the same records, so its flight is shared the same way, with the CD port's own
 *  messages (TieFrame, TieOwners).
 *
 *  * The room.  The host is seat 0 (the room's, or the TCP listener); a guest says hello (TdmHello), the host answers
 *    with its seat.  Rooms of up to 3: T/F Alpha's craft.
 *  * The launch.  Each machine's program waits at create_createmission's entry (TIEMISS.EXE, after the opening) until
 *    every pilot of the room is there; the host then says go with its random seeds, and each makes the mission at
 *    once: a guest with the host's seeds and its own craft of the player's flight group (the record's player-craft
 *    number: seat 1 the group's first craft the host does not fly, seat 2 the next), so that the mission's own setup
 *    makes the same craft in the same object slots everywhere and starts them together.
 *  * Who flies a craft (the CD's step 5, the user's rule): the host owns most; a guest gets a craft within the lasers'
 *    reach of it and within 45 degrees of its nose, or its target within 4 km, unless the host targets it; each pilot
 *    its own craft.  The owner's game flies it (its AI, moves, damage); the others write its reports into their game
 *    before the move and hold their AI off it (the planner's countdown, CraftData +0x2C, kept full).  Far craft (beyond
 *    5.5 km of our pilot) are flown by our AI between their owner's sparse reports, pulled to them smoothly.
 *  * Rates: our own craft every frame; the craft we fly for the others within 2.5 km of another pilot every frame,
 *    within 10 km every 10th, the rest one at a time every 30 frames in turn.
 *  * Shots: a shot of a craft we fly, near another pilot, goes with the next frame; the others make it from that craft
 *    with the game's own laser_createprojectile.  A craft another machine flies does not shoot here.
 *  * Hits: decided by the shooter's machine (its collide_laserhitcraft), applied by the target's owner with the game's
 *    own call and a stand-in bolt; this game damages only the craft it flies (collide_damagecraft's entry), and shows
 *    its own hits on the others' craft at once (their owner's figures follow).
 *  * Deaths: an owner's craft dying is sent; the others kill their copy with the game's own damage, the kill credited
 *    to the pilot whose hit began it (collide_updatekills).  A craft gone without dying (its exit) is gone everywhere.
 *  * The room's end (the user's rule for the CD version): a pilot down, or the host's flight over, ends everybody's
 *    flight 3 s later (the mission's end flag).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

using namespace tdm;

enum {
    kMaxSeats = 3,                  // (T/F Alpha: three craft)
    kHelloWaitMs = 20000,
    kPollMs = 4,
    kStateEveryMs = 500,            // our TdmState outside the flight
    kStaleMs = 1500,                // a TdmState older than this: the pilot not at the launch
    kNoticeMs = 8000,
    kLaunchWaitMs = 120000,         // the host waits this long at most for a guest still in the opening
    kReliableEveryMs = 1000,
    // the CD's step 5 (world units: 40,960 a km)
    kReach = 102400,                // 2.5 km, the lasers' reach: within it a craft is near
    kTargetReach = 163840,          // 4 km: a pilot's target within it is that pilot's to fly
    kMidReach = 409600,             // 10 km: a craft within it is reported every kMidEvery frames
    kMidEvery = 10,
    kFarEvery = 30,                 // one far craft every this many frames, in turn
    kOwnerHoldMs = 1000,            // a craft changes owner at most this often
    kOwnersEveryMs = 2000,          // the host's whole table, this often
    kDecideEveryMs = 250,
    kPilotStaleMs = 3000,           // a pilot whose frames stopped this long is not counted
    kLocalAiIn = 204800,            // 5 km: within it another machine's craft is held here, beyond kLocalAiOut our AI flies it
    kLocalAiOut = 225280,           // 5.5 km
    kExactError = 400,              // a near craft's report this close is written as it is
    kRoomEndMs = 3000,
    kLocalHitHoldMs = 600,          // a hit of ours shown on another's craft: its owner's damage figures wait this long
    kShotReach = 2 * kReach,
    kKillCalls = 12,
    kExplosionFirst = 0x40,         // a dying craft's slot shows its explosion (move_moveobjects: 0x40 or 0x41)
    kExplosionLast = 0x43,
    kHitsIn = 64,
};
const double kCorrectSpeed = 12288.0;       // a place pulled to its owner's at most this fast, units a second (300 m/s)
const double kFarBlendS = 10.0;             // a craft our AI flies: its first report blended in over this many seconds,
const double kFarBlendMinS = 1.0;           // ... the next ones over the time since the one before, at least this
const double kFarBlendMaxS = 15.0;          // ... and at most this
const double kTicksPerSecond = 236.0;       // (the game's clock: frameticks)
const char kMissionName[] = "DEMO";

struct Peer {
    Connection conn;
    int player;
    std::string callsign;
    bool hello;
    bool flying;
    Peer() : player(-1), hello(false), flying(false) {}
};

struct Remote {                     // a pilot's TdmState
    bool known, atLaunch, inFlight;
    double at;
    std::string callsign;
    Bit32u go, seed, seed2, mission;
    Bit32u verdict, next;           // (the host's: its verdict on its flight `verdict`)
    bool over;
    Remote() : known(false), atLaunch(false), inFlight(false), at(-1e9), go(0), seed(0), seed2(0), mission(0), verdict(0), next(0), over(false) {}
};

struct Puppet {
    bool used;
    int owner;
    bool fresh;                     // a report not yet written into the game
    TieCraft state;
    double at, prevAt;
    bool localAi;                   // far from our pilot: flown by our AI, pulled to its owner's reports
    double ex, ey, ez, blendTicks;
    double localHitAt;
};

struct Net {
    bool host;
    int self, seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    Remote remotes[kMaxSeats];
    // the launch
    bool atLaunch;
    double launchSince;
    Bit32u launchSerial, flownGo;
    Bit32u hostSeed, hostSeed2, hostMission;
    bool alone;                     // this flight flown alone (nobody came)
    Bit32u verdictGo, verdictNext;  // the host's latest verdict (its flight verdictGo; the campaign's next mission)
    bool verdictOver;
    bool roomEnded;                 // this flight ended by the room (room_end), not by its pilot
    bool soloNext;                  // a guest's launch, held for a host already flying: flown alone (Esc)
    bool lateSaid;                  // ... and told so, once a launch
    // the flight
    Bit64u frame;
    double lastReliableAt, endAt;
    Puppet puppets[kObjectCount];
    int owner[kCraftSlots];
    double ownerAt[kCraftSlots];
    int pilotObj[kMaxSeats], pilotTarget[kMaxSeats];
    double pilotSeenAt[kMaxSeats];
    double decidedAt, ownersSentAt;
    int farCursor;
    std::vector<TieShot> shotsOut;
    std::vector<TieHit> hitsOut;
    std::vector<TieEvent> eventsOut;
    Bit8u lastType[kCraftSlots], lastFg[kCraftSlots];
    int deathSentId[kCraftSlots];
    TieHit hitsIn[kHitsIn];
    int hitsInNext;
    int killing[kCraftSlots], killer[kCraftSlots];
    // the report's
    unsigned sentCraft, sentNear, sentMid, sentFar, sentBytes, ownerChanges, shotsSent, shotsMade, hitsSent, hitsApplied,
        damageSkipped, kills, credited, localAiNow, localAiStarts, localAiEnds, localShots, farReports, nearBlends, hitsShown;
    double farErrSum, farErrMax;
    Net() : host(false), self(-1), seats(2), listener(NULL), atLaunch(false), launchSince(0), launchSerial(0), flownGo(0),
            hostSeed(0), hostSeed2(0), hostMission(0), alone(false), verdictGo(0), verdictNext(0), verdictOver(false), roomEnded(false), soloNext(false), lateSaid(false), frame(0),
            lastReliableAt(0), endAt(0), decidedAt(-1e9), ownersSentAt(-1e9), farCursor(0), hitsInNext(0) {
        zero_counts();
        clear_flight();
    }
    void zero_counts() {
        sentCraft = sentNear = sentMid = sentFar = sentBytes = ownerChanges = shotsSent = shotsMade = hitsSent = hitsApplied = 0;
        damageSkipped = kills = credited = localAiNow = localAiStarts = localAiEnds = localShots = farReports = nearBlends = hitsShown = 0;
        farErrSum = farErrMax = 0;
    }
    // A puppet table and owners for a new flight (a Puppet holds a protobuf message: never memset).
    void clear_flight() {
        for (int o = 0; o < kObjectCount; o++) {
            Puppet &p = puppets[o];
            p.used = false;
            p.owner = -1;
            p.fresh = false;
            p.state.Clear();
            p.at = p.prevAt = 0;
            p.localAi = false;
            p.ex = p.ey = p.ez = p.blendTicks = 0;
            p.localHitAt = -1e9;
        }
        for (int o = 0; o < kCraftSlots; o++) {
            owner[o] = 0;
            ownerAt[o] = -1e9;
            lastType[o] = lastFg[o] = 0;
            deathSentId[o] = -1;
            killing[o] = killer[o] = 0;
        }
        for (int k = 0; k < kMaxSeats; k++) {
            pilotObj[k] = -1;
            pilotTarget[k] = 0xFFFF;
            pilotSeenAt[k] = -1e9;
        }
        farCursor = 0;
        shotsOut.clear();
        hitsOut.clear();
        eventsOut.clear();
        endAt = 0;
        frame = 0;
    }
    ~Net() {
        for (size_t i = 0; i < peers.size(); i++) delete peers[i];
        delete listener;
    }
};

Net *g_net = NULL;
double g_nextTryMs = 0, g_pollAt = -1e9, g_stateAt = -1e9, g_noticeAt = -1;
bool g_tried = false;

void begin_kill(int obj, int killer);
void room_end(int seat, const char *what);
void end_for_room(const std::string &line);

// --- memory ---------------------------------------------------------------------------------------------------------

inline Bit8u o8(int o, int k) { return mem_readb(tdm_obj(o) + (PhysPt)k); }
inline Bit16u o16(int o, int k) { return mem_readw(tdm_obj(o) + (PhysPt)k); }
inline Bit32s o32(int o, int k) { return (Bit32s)mem_readd(tdm_obj(o) + (PhysPt)k); }
inline void wo8(int o, int k, Bit8u v) { mem_writeb(tdm_obj(o) + (PhysPt)k, v); }
inline void wo16(int o, int k, Bit16u v) { mem_writew(tdm_obj(o) + (PhysPt)k, v); }
inline void wo32(int o, int k, Bit32u v) { mem_writed(tdm_obj(o) + (PhysPt)k, v); }
inline Bit16u ds16(Bit16u off) { return mem_readw(tdm_ds(off)); }

void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "TIE demo: %s", text.c_str());
}

std::string pilot_name(int seat) {
    if (!g_net || seat < 0 || seat >= kMaxSeats) return "a pilot";
    if (seat == g_net->self) return g_net->callsign;
    return g_net->remotes[seat].callsign.empty() ? std::string("pilot ") + (char)('1' + seat) : g_net->remotes[seat].callsign;
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

// --- the room ---------------------------------------------------------------------------------------------------

bool start() {
    net_config.reset_from_env();
    Net *n = new Net();
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
    if (n->host) notice("Room open: the others fly the mission with you in T/F Alpha, launched together");
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

// the pilots with us in the room (said hello)
int others() {
    if (!g_net) return 0;
    int n = 0;
    for (size_t i = 0; i < g_net->peers.size(); i++) n += g_net->peers[i]->hello && g_net->peers[i]->conn.is_open();
    return n;
}

void send_state(bool reliable) {
    NetworkMessage msg;
    TdmState *st = msg.mutable_tdm_state();
    st->set_player((Bit32u)(g_net->self < 0 ? 0 : g_net->self));
    st->set_in_flight(tdm_in_flight());
    st->set_at_launch(g_net->atLaunch);
    if (g_net->host && g_net->launchSerial) {
        st->set_go(g_net->launchSerial);
        st->set_seed(g_net->hostSeed);
        st->set_seed2(g_net->hostSeed2);
        st->set_mission(g_net->hostMission);
    }
    if (g_net->host && g_net->verdictGo) {
        st->set_verdict(g_net->verdictGo);
        st->set_next(g_net->verdictNext);
        st->set_over(g_net->verdictOver);
    }
    send_all(msg, !reliable);
    g_stateAt = PIC_FullIndex();
}

void take_state(const TdmState &st, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) return;
    Remote &r = g_net->remotes[seat];
    r.known = true;
    r.at = PIC_FullIndex();
    const bool wasAtLaunch = r.atLaunch;
    r.atLaunch = st.at_launch();
    r.inFlight = st.in_flight();
    // (a guest come to its launch while this host flies: it launches with the next mission; said here)
    if (g_net->host && r.atLaunch && !wasAtLaunch && tdm_in_flight()) {
        notice(pilot_name(seat) + " is waiting to launch: you fly the next mission together");
    }
    if (st.has_go() && st.go() > r.go) {
        r.go = st.go();
        r.seed = st.seed();
        r.seed2 = st.seed2();
        r.mission = st.mission();
    }
    if (st.has_verdict() && st.verdict() > r.verdict) {
        r.verdict = st.verdict();
        r.next = st.next();
        r.over = st.over();
        // The host's flight over (its verdict on it: its frames stop with it, never saying so): ours ends too.
        if (!g_net->host && seat == 0 && st.verdict() >= g_net->flownGo && g_net->flownGo) {
            tdm_campaign_host_verdict((int)st.next(), st.over());
            end_for_room(pilot_name(0) + "'s flight is over: the flight ends for everybody");
        }
    }
}

// --- the flight (the CD port's) -----------------------------------------------------------------------------------

bool is_craft(int obj) {
    return obj >= 0 && obj < kCraftSlots && o8(obj, 4) != 0 && o8(obj, 2) == 0 && (o8(obj, 4) < kExplosionFirst || o8(obj, 4) > kExplosionLast);
}
bool dying(int obj) {
    return o16(obj, 0x2C) != 0 || (o8(obj, 4) >= kExplosionFirst && o8(obj, 4) <= kExplosionLast);
}
double distance(int a, int b) {
    const double dx = (double)o32(a, 6) - (double)o32(b, 6), dy = (double)o32(a, 0xA) - (double)o32(b, 0xA),
                 dz = (double)o32(a, 0xE) - (double)o32(b, 0xE);
    return sqrt(dx * dx + dy * dy + dz * dz);
}
// The pilots flying this mission now (their frames seen lately), ourselves among them.
bool pilot_present(int seat, double now) {
    if (seat == g_net->self) return tdm_player_object() >= 0;
    return seat >= 0 && seat < kMaxSeats && g_net->pilotObj[seat] >= 0 && now - g_net->pilotSeenAt[seat] < kPilotStaleMs;
}
int pilot_object(int seat) { return seat == g_net->self ? tdm_player_object() : g_net->pilotObj[seat]; }

// Who flies a craft for this machine: the table's word, but a craft whose owner is not in this flight is ours.
int owner_of(int obj) {
    if (obj < 0 || obj >= kCraftSlots) return g_net->self;
    const int o = g_net->owner[obj];
    return o == g_net->self || pilot_present(o, PIC_FullIndex()) ? o : g_net->self;
}
bool mine(int obj) { return owner_of(obj) == g_net->self || obj == tdm_player_object(); }

// A craft this machine is to fly from now on: its AI to plan at once.
void release_ai(int obj) {
    const PhysPt craft = tdm_craft(obj);
    if (craft) mem_writew(craft + kCraftAiCountdown, 0);
    g_net->puppets[obj].used = false;
}
// This machine's AI kept off a puppet: the planner runs a craft only when its countdown has run out.
void hold_ai(int obj) {
    const PhysPt craft = tdm_craft(obj);
    if (craft) mem_writew(craft + kCraftAiCountdown, 0x7FFF);
}

bool someone_near(int obj, double reach) {
    const double now = PIC_FullIndex();
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && is_craft(pilot_object(k)) && distance(obj, pilot_object(k)) < reach) return true;
    }
    return false;
}
bool others_flying() {
    const double now = PIC_FullIndex();
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now)) return true;
    }
    return false;
}

void fill_craft(TieCraft *c, int obj) {
    c->set_obj((Bit32u)obj);
    c->set_owner((Bit32u)g_net->self);
    c->set_type(o8(obj, 4));
    c->set_x(o32(obj, 6));
    c->set_y(o32(obj, 0xA));
    c->set_z(o32(obj, 0xE));
    c->set_heading(o16(obj, 0x1E));
    c->set_pitch(o16(obj, 0x20));
    c->set_roll(o16(obj, 0x22));
    c->set_speed((Bit16s)o16(obj, 0x26));
    c->set_mx((Bit16s)o16(obj, 0x3A));
    c->set_my((Bit16s)o16(obj, 0x3C));
    c->set_mz((Bit16s)o16(obj, 0x3E));
    char axes[18];
    for (int k = 0; k < 18; k++) axes[k] = (char)o8(obj, 0x42 + k);
    c->set_axes(std::string(axes, sizeof(axes)));
    if (obj < kCraftSlots && dying(obj)) {
        c->set_dying(true);
        if (g_net->killer[obj]) c->set_killer((Bit32u)(g_net->killer[obj] - 1));
    } else if (obj < kCraftSlots) {
        g_net->killer[obj] = 0;
    }
    const PhysPt craft = tdm_craft(obj);
    if (craft) {
        c->set_hull(mem_readw(craft + kCraftHull));
        c->set_shield_front((Bit16s)mem_readw(craft + kCraftShieldFront));
        c->set_shield_rear((Bit16s)mem_readw(craft + kCraftShieldRear));
        c->set_systems(mem_readw(craft + kCraftSystems));
    }
}

// A puppet into the game, just before the game moves its objects (move_moveobjects): whatever this machine's AI and
// dynamics did to it this frame is overwritten, and the game's own integration then carries it one frame along the
// owner's move vector at the owner's speed.  The place is written only from a report not yet written.
void write_puppet(Puppet &p) {
    const TieCraft &c = p.state;
    const int obj = (int)c.obj();
    if (o8(obj, 4) == 0 || c.type() == 0 || o8(obj, 4) != c.type() || o16(obj, 0x2C) != 0) return;
    const PhysPt craft = tdm_craft(obj);
    if (p.fresh) {
        const double ex = (double)c.x() - (double)o32(obj, 6), ey = (double)c.y() - (double)o32(obj, 0xA), ez = (double)c.z() - (double)o32(obj, 0xE);
        const double err = sqrt(ex * ex + ey * ey + ez * ez);
        double seconds = err / kCorrectSpeed;
        if (p.localAi) {
            const double interval = p.prevAt > 0 ? (p.at - p.prevAt) / 1000.0 : kFarBlendS;
            seconds = fmin(kFarBlendMaxS, fmax(kFarBlendMinS, interval));
            g_net->farReports++;
            g_net->farErrSum += err;
            g_net->farErrMax = fmax(g_net->farErrMax, err);
        } else if (err <= kExactError) {
            seconds = 0;
        }
        if (seconds <= 0) {
            wo32(obj, 6, (Bit32u)c.x());
            wo32(obj, 0xA, (Bit32u)c.y());
            wo32(obj, 0xE, (Bit32u)c.z());
            p.blendTicks = 0;
        } else {
            p.ex = ex;
            p.ey = ey;
            p.ez = ez;
            p.blendTicks = seconds * kTicksPerSecond;
            if (!p.localAi) g_net->nearBlends++;
        }
        if (craft && c.has_hull() && PIC_FullIndex() - p.localHitAt > kLocalHitHoldMs) {
            mem_writew(craft + kCraftHull, (Bit16u)c.hull());
            mem_writew(craft + kCraftShieldFront, (Bit16u)(Bit16s)c.shield_front());
            mem_writew(craft + kCraftShieldRear, (Bit16u)(Bit16s)c.shield_rear());
            mem_writew(craft + kCraftSystems, (Bit16u)c.systems());
        }
        p.fresh = false;
    }
    if (p.blendTicks > 0) {
        const double ticks = fmax(1.0, (double)ds16(kFrameticks));
        const double f = fmin(1.0, ticks / p.blendTicks);
        wo32(obj, 6, (Bit32u)(Bit32s)lround((double)o32(obj, 6) + p.ex * f));
        wo32(obj, 0xA, (Bit32u)(Bit32s)lround((double)o32(obj, 0xA) + p.ey * f));
        wo32(obj, 0xE, (Bit32u)(Bit32s)lround((double)o32(obj, 0xE) + p.ez * f));
        p.ex *= 1 - f;
        p.ey *= 1 - f;
        p.ez *= 1 - f;
        p.blendTicks -= ticks;
    }
    if (p.localAi) return;              // (our AI flies it: its attitude, speed and course are its own here)
    wo16(obj, 0x1E, (Bit16u)c.heading());
    wo16(obj, 0x20, (Bit16u)c.pitch());
    wo16(obj, 0x22, (Bit16u)c.roll());
    wo16(obj, 0x26, (Bit16u)(Bit16s)c.speed());
    wo16(obj, 0x3A, (Bit16u)(Bit16s)c.mx());
    wo16(obj, 0x3C, (Bit16u)(Bit16s)c.my());
    wo16(obj, 0x3E, (Bit16u)(Bit16s)c.mz());
    const std::string &axes = c.axes();
    for (size_t k = 0; k < axes.size() && k < 18; k++) wo8(obj, 0x42 + (int)k, (Bit8u)axes[k]);
}

// A pilot's own craft (a human flies it: never this machine's AI, however far).
bool pilot_flies(int obj) {
    const double now = PIC_FullIndex();
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && g_net->pilotObj[k] == obj) return true;
    }
    return false;
}

// A hit of a bolt of ours on another machine's craft, shown here at once (its owner applies it for good): the game's
// own sums, the struck side's shield first, then the hull, never its death here.
void show_hit(int target, int bolt, int side) {
    const PhysPt craft = tdm_craft(target);
    if (!craft || side < 0 || side > 1) return;
    long dmg = o16(bolt, 0x2A);
    const int genus = o8(target, 3);
    if (genus == 4 || genus == 5) dmg >>= 4;
    if (genus == 3) dmg >>= 2;
    if (dmg >= 0x7FFF) dmg = 32766;
    const PhysPt shieldAt = craft + kCraftShieldFront + 2 * (PhysPt)side;
    const long shield = (Bit16s)mem_readw(shieldAt);
    if (shield > dmg) {
        mem_writew(shieldAt, (Bit16u)(Bit16s)(shield - dmg));
    } else {
        const long over = dmg - shield;
        mem_writew(shieldAt, 0);
        if (over > 0) {
            const long hullMax = mem_readw(craft + kCraftHullMax);
            long hull = (long)mem_readw(craft + kCraftHull) + over;
            if (hullMax > 0 && hull >= hullMax) hull = hullMax - 1;     // (its death is its owner's to say)
            mem_writew(craft + kCraftHull, (Bit16u)hull);
        }
    }
    g_net->puppets[target].localHitAt = PIC_FullIndex();
    g_net->hitsShown++;
}

// --- shots, hits, deaths ----------------------------------------------------------------------------------------

void shot_made(int tag, Bit16u ax) {
    if (ax != 0xFFFF) {
        g_net->shotsMade++;
        static unsigned logged = 0;
        if (logged < 12) {
            logged++;
            wclog(2, "TIE demo: a shot of obj %d made here: bolt %u of type %u", tag, ax, ax < kObjectCount ? o8(ax, 4) : 0);
        }
    }
}
void make_shot(const TieShot &shot, int owner) {
    const int obj = (int)shot.obj();
    if (obj < 0 || obj >= kCraftSlots || owner_of(obj) != owner || !is_craft(obj) || o16(obj, 0x2C) != 0) return;
    tdm_queue_call(TDM_CREATE_PROJECTILE, 3, (Bit16u)obj, (Bit16u)shot.hardpoint(), (Bit16u)shot.type(), 0, NULL, shot_made, obj);
}

// A hit for us to apply: a stand-in bolt with the shooter's figures in a free projectile slot, then the game's own
// collide_laserhitcraft (damage, shields, components, its attacker, messages).
int g_standin = -1;
bool hit_before(int tag, Bit16u *args) {
    const TieHit &h = g_net->hitsIn[tag];
    g_standin = -1;
    for (int s = kCraftSlots; s < kObjectCount; s++) {
        if (o8(s, 4) == 0) {
            g_standin = s;
            break;
        }
    }
    if (g_standin < 0 || !is_craft((int)h.target()) || !mine((int)h.target())) {
        g_standin = -1;
        return false;
    }
    const int b = g_standin;
    const Bit16u id = o16(b, 0);
    for (int k = 0; k < kObjectSize; k += 2) wo16(b, k, 0);
    wo16(b, 0, id);
    wo8(b, 2, 1);                                   // category: a projectile
    wo8(b, 3, (Bit8u)h.bolt_genus());
    wo8(b, 4, (Bit8u)h.bolt_type());
    wo32(b, 6, (Bit32u)h.x());
    wo32(b, 0xA, (Bit32u)h.y());
    wo32(b, 0xE, (Bit32u)h.z());
    wo32(b, 0x12, (Bit32u)h.ox());
    wo32(b, 0x16, (Bit32u)h.oy());
    wo32(b, 0x1A, (Bit32u)h.oz());
    wo16(b, 0x2A, (Bit16u)h.bolt_radius());
    wo16(b, 0x2E, 1);                               // age
    wo16(b, 0x30, (Bit16u)h.shooter());
    wo8(b, 0x33, o8((int)h.shooter() < kObjectCount ? (int)h.shooter() : 0, 0x33));    // the shooter's side
    wo16(b, 0x54, (Bit16u)(kWarheads + 4 * (b - kCraftSlots)));
    wo16(b, 0x56, (Bit16u)(tdm_ds(0) >> 4));
    args[0] = (Bit16u)b;
    wclog(2, "TIE demo: applying a hit on obj %u from obj %u: stand-in bolt %d (type %u, radius %u, offset %u)", h.target(), h.shooter(),
          b, h.bolt_type(), h.bolt_radius(), h.offset());
    return true;
}
void hit_done(int tag, Bit16u ax) {
    (void)ax;
    const TieHit &h = g_net->hitsIn[tag];
    if (g_standin >= 0) {
        if (o8(g_standin, 4) == (Bit8u)h.bolt_type()) wo8(g_standin, 4, 0);     // (not turned into its flash: gone)
        g_net->hitsApplied++;
        const PhysPt craft = tdm_craft((int)h.target());
        wclog(2, "TIE demo: the hit on obj %u applied (its hull's damage %u of %u)", h.target(), craft ? mem_readw(craft + kCraftHull) : 0,
              craft ? mem_readw(craft + kCraftHullMax) : 0);
    }
    g_standin = -1;
    const int t = (int)h.target();
    if (t < kCraftSlots && !g_net->killer[t] && h.shooter() < (Bit32u)kObjectCount && dying(t)) {
        g_net->killer[t] = (int)h.shooter() + 1;     // (the hit that began its death: its shooter's machine credits the kill)
        wclog(2, "TIE demo: obj %d dying of obj %u's hit, applied here", t, h.shooter());
    }
}
void take_hit(const TieHit &hit) {
    const int target = (int)hit.target();
    if (!is_craft(target) || !mine(target) || hit.shooter() >= (Bit32u)kObjectCount) return;
    const int tag = g_net->hitsInNext;
    g_net->hitsInNext = (g_net->hitsInNext + 1) % kHitsIn;
    g_net->hitsIn[tag] = hit;
    tdm_queue_call(TDM_LASER_HIT, 3, 0, (Bit16u)target, (Bit16u)hit.offset(), 0, hit_before, hit_done, tag);
}

// A copy whose owner reports it dying is killed here by the game's own damage (a collision's, called until its death
// is under way); its flight flag cleared first unless a death's (3, 4).
bool kill_before(int tag, Bit16u *args) {
    (void)args;
    const PhysPt craft = tdm_craft(tag);
    if (craft) {
        const Bit8u flag = mem_readb(craft + kCraftFlightFlag);
        if (flag != 0 && flag != 3 && flag != 4) mem_writeb(craft + kCraftFlightFlag, 0);
    }
    return true;
}
void kill_done(int tag, Bit16u ax) {
    (void)ax;
    const int obj = tag;
    if (!is_craft(obj) || o16(obj, 0x2C) != 0 || g_net->killing[obj] >= kKillCalls) {
        if (g_net->killing[obj] > 0) {
            g_net->kills++;
            wclog(2, "TIE demo: obj %d killed here after %d calls: %s", obj, g_net->killing[obj],
                  !is_craft(obj) ? "gone" : o16(obj, 0x2C) != 0 ? "dying" : "NOT dying");
        }
        g_net->killing[obj] = 0;
        return;
    }
    g_net->killing[obj]++;
    tdm_queue_call(TDM_DAMAGE, 4, (Bit16u)obj, 0xFFFF, 0xFFFF, 0, kill_before, kill_done, obj);
}
void credit_done(int tag, Bit16u ax) {
    (void)ax;
    g_net->credited++;
    wclog(1, "TIE demo: obj %d's kill credited to us (our shot began its death on its owner's machine)", tag);
}
void begin_kill(int obj, int killer) {
    if (mine(obj) || g_net->killing[obj] != 0 || !is_craft(obj) || o16(obj, 0x2C) != 0) return;
    g_net->killing[obj] = 1;
    if (killer >= 0 && killer == tdm_player_object()) {
        tdm_queue_call(TDM_UPDATE_KILLS, 3, (Bit16u)killer, (Bit16u)obj, 1, 0, NULL, credit_done, obj);
    }
    tdm_queue_call(TDM_DAMAGE, 4, (Bit16u)obj, 0xFFFF, 0xFFFF, 0, kill_before, kill_done, obj);
}

// Our craft's deaths (sent once, reliably) and exits (a craft of ours gone without dying: gone everywhere).
void watch_events() {
    for (int o = 0; o < kCraftSlots; o++) {
        const Bit8u type = o8(o, 4);
        const bool there = type != 0;
        const int id = o16(o, 0);
        const bool wasCraft = g_net->lastType[o] && (g_net->lastType[o] < kExplosionFirst || g_net->lastType[o] > kExplosionLast);
        if (there && dying(o) && mine(o) && o != tdm_player_object() && g_net->deathSentId[o] != id && wasCraft) {
            g_net->deathSentId[o] = id;
            TieEvent e;
            e.set_kind(3);
            e.set_obj((Bit32u)o);
            e.set_type(g_net->lastType[o]);
            e.set_fg(g_net->lastFg[o]);
            if (g_net->killer[o]) e.set_killer((Bit32u)(g_net->killer[o] - 1));
            g_net->eventsOut.push_back(e);
            wclog(2, "TIE demo: obj %d (type %u) of ours dying: sent", o, g_net->lastType[o]);
        }
        if (!there && wasCraft && g_net->deathSentId[o] != id && owner_of(o) == g_net->self && o != tdm_player_object()) {
            TieEvent e;
            e.set_kind(4);
            e.set_obj((Bit32u)o);
            e.set_type(g_net->lastType[o]);
            e.set_fg(g_net->lastFg[o]);
            g_net->eventsOut.push_back(e);
            wclog(1, "TIE demo: obj %d (type %u) of ours gone: sent", o, g_net->lastType[o]);
        }
        if (there && (type < kExplosionFirst || type > kExplosionLast)) {
            g_net->lastType[o] = type;
            g_net->lastFg[o] = o8(o, 0x35);
        } else if (!there) {
            g_net->lastType[o] = 0;
        }
    }
}

void take_event(const TieEvent &e, int from) {
    if (e.obj() >= (Bit32u)kCraftSlots) return;
    const int o = (int)e.obj();
    if (o8(o, 4) != e.type() || o8(o, 0x35) != e.fg()) {
        wclog(1, "TIE demo: obj %d's %s on pilot %d's machine: not the same craft here (type %u, flight group %u)", o,
              e.kind() == 3 ? "death" : "exit", from + 1, o8(o, 4), o8(o, 0x35));
        return;
    }
    if (e.kind() == 3) {
        begin_kill(o, e.has_killer() ? (int)e.killer() : -1);
    } else if (e.kind() == 4 && !mine(o)) {
        wo8(o, 4, 0);
        g_net->lastType[o] = 0;
        wclog(1, "TIE demo: obj %d gone as on pilot %d's machine", o, from + 1);
    }
}

// --- who flies each craft (the host's) ---------------------------------------------------------------------------

void send_owners() {
    NetworkMessage msg;
    TieOwners *t = msg.mutable_tie_owners();
    for (int o = 0; o < kCraftSlots; o++) t->add_owner((Bit32u)g_net->owner[o]);
    t->set_mission(kMissionName);
    send_all(msg, false);
}

void take_owners(const TieOwners &t) {
    if (!tdm_in_flight()) return;
    const int own = tdm_player_object();
    for (int o = 0; o < kCraftSlots && o < t.owner_size(); o++) {
        const int now = (int)t.owner(o);
        if (o == own || now == g_net->owner[o]) continue;
        if (now == g_net->self) release_ai(o);
        g_net->owner[o] = now;
    }
}

// Is a craft in a pilot's view?  Within the cone of the nose (its move vector, +0x3A: a TIE craft does not slide).
bool in_view(int pilotObj, int obj, double cone) {
    const double fx = (Bit16s)o16(pilotObj, 0x3A), fy = (Bit16s)o16(pilotObj, 0x3C), fz = (Bit16s)o16(pilotObj, 0x3E);
    const double dx = (double)o32(obj, 6) - o32(pilotObj, 6), dy = (double)o32(obj, 0xA) - o32(pilotObj, 0xA),
                 dz = (double)o32(obj, 0xE) - o32(pilotObj, 0xE);
    const double fl = sqrt(fx * fx + fy * fy + fz * fz), dl = sqrt(dx * dx + dy * dy + dz * dz);
    if (fl < 1 || dl < 1) return dl < 1;
    return (fx * dx + fy * dy + fz * dz) / (fl * dl) >= cone;
}

// The host's rule (the user's, for the CD version): "host should own most craft except the ones both within range and
// on the guests viewport or specifically targeted by guest and not targeted by the host".
enum { kViewCone45 = 7071, kViewCone60 = 5000 };
bool guest_wants(int k, int o, bool keep) {
    const int po = pilot_object(k);
    const double d = distance(o, po);
    if (g_net->pilotTarget[k] == o && d < kTargetReach * (keep ? 1.25 : 1.0)) return true;
    return d < kReach * (keep ? 1.25 : 1.0) && in_view(po, o, (keep ? kViewCone60 : kViewCone45) / 10000.0);
}
void decide_owners(double now) {
    if (!g_net->host || now - g_net->decidedAt < kDecideEveryMs) return;
    g_net->decidedAt = now;
    int guests[kMaxSeats], n = 0;
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && is_craft(pilot_object(k))) guests[n++] = k;
    }
    const int hostObj = tdm_player_object();
    const int hostTarget = ds16(kTarget);
    bool changed = false;
    for (int o = 0; o < kCraftSlots; o++) {
        bool someonesOwn = o == hostObj;
        for (int i = 0; i < n; i++) someonesOwn = someonesOwn || pilot_object(guests[i]) == o;
        if (someonesOwn || !is_craft(o)) continue;
        const int cur = g_net->owner[o];
        int want = 0;
        if (o != hostTarget) {
            int bestScore = -1;
            double bestDist = 1e30;
            for (int i = 0; i < n; i++) {
                const int k = guests[i];
                if (!guest_wants(k, o, k == cur)) continue;
                const int score = (g_net->pilotTarget[k] == o ? 2 : 0) + (k == cur ? 1 : 0);
                const double d = distance(o, pilot_object(k));
                if (score > bestScore || (score == bestScore && d < bestDist)) {
                    bestScore = score;
                    bestDist = d;
                    want = k;
                }
            }
        }
        bool curPresent = cur == 0;
        for (int i = 0; i < n; i++) curPresent = curPresent || guests[i] == cur;
        if (want != cur && (now - g_net->ownerAt[o] >= kOwnerHoldMs || !curPresent)) {
            g_net->owner[o] = want;
            g_net->ownerAt[o] = now;
            g_net->ownerChanges++;
            changed = true;
            if (want == g_net->self) release_ai(o);
            wclog(2, "TIE demo: obj %d to pilot %d (was pilot %d)", o, want + 1, cur + 1);
        }
    }
    if (changed || now - g_net->ownersSentAt >= kOwnersEveryMs) {
        g_net->ownersSentAt = now;
        send_owners();
    }
}

// --- the frames --------------------------------------------------------------------------------------------------

void make_shot_list(const TieFrame &f, int owner) {
    for (int i = 0; i < f.shots_size() && tdm_in_flight(); i++) make_shot(f.shots(i), owner);
}

void take_frame(Peer *from, const TieFrame &f) {
    const int owner = g_net->host ? from->player : (f.has_player() ? (int)f.player() : 0);
    if (owner == g_net->self || owner < 0 || owner >= kMaxSeats) return;
    // (the host's flight over: every guest's ends too)
    const bool hostEnded = owner == 0 && !g_net->host && from->flying && !f.flying() && from->player == 0;
    if (!g_net->host || owner == from->player) from->flying = f.flying();
    if (hostEnded) room_end(0, "'s flight is over");
    if (f.flying() && f.has_own() && f.own() < (Bit32u)kCraftSlots && tdm_in_flight()) {
        g_net->pilotObj[owner] = (int)f.own();
        g_net->pilotTarget[owner] = f.has_target() ? (int)f.target() : 0xFFFF;
        g_net->pilotSeenAt[owner] = PIC_FullIndex();
        g_net->owner[f.own()] = owner;      // (a pilot's own craft is its own)
    }
    for (int i = 0; i < f.craft_size() && tdm_in_flight(); i++) {
        const TieCraft &c = f.craft(i);
        if (c.obj() >= (Bit32u)kObjectCount) continue;
        if (c.obj() < (Bit32u)kCraftSlots && g_net->owner[c.obj()] != owner && !(f.has_own() && c.obj() == f.own())) continue;
        Puppet &p = g_net->puppets[c.obj()];
        p.used = true;
        p.owner = owner;
        p.fresh = true;
        p.state = c;
        p.prevAt = p.at;
        p.at = PIC_FullIndex();
        const bool same = c.type() == o8((int)c.obj(), 4);
        if (c.dying() && c.obj() < (Bit32u)kCraftSlots && (same || (f.has_own() && c.obj() == f.own()))) {
            begin_kill((int)c.obj(), c.has_killer() ? (int)c.killer() : -1);
            if (f.has_own() && c.obj() == f.own()) room_end(owner, " is down");
        }
    }
    make_shot_list(f, owner);
    for (int i = 0; i < f.hits_size() && tdm_in_flight(); i++) take_hit(f.hits(i));
    for (int i = 0; i < f.events_size() && tdm_in_flight(); i++) take_event(f.events(i), owner);
    if (g_net->host) {
        NetworkMessage msg;
        *msg.mutable_tie_frame() = f;
        msg.mutable_tie_frame()->set_player((Bit32u)owner);
        relay(from, [&](Peer *to) { to->conn.send_best_effort(msg); });
    }
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
            send_state(true);
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
        relay(p, [&](Peer *to) { to->conn.send(out); });
    } else if (msg.has_tie_frame()) {
        if (p->hello) take_frame(p, msg.tie_frame());
    } else if (msg.has_tie_owners()) {
        if (p->hello && !g_net->host) take_owners(msg.tie_owners());
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
        for (int n = 0; n < 256 && p->conn.is_open(); n++) {
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
            if (p->player >= 0 && p->player < kMaxSeats) {
                g_net->remotes[p->player] = Remote();
                g_net->pilotObj[p->player] = -1;
            }
        }
        delete p;
        g_net->peers.erase(g_net->peers.begin() + i);
    }
}

// The room's end of a flight: a pilot down or the host's flight over ends every pilot's, after a moment.
void end_for_room(const std::string &line) {
    if (!tdm_in_flight() || g_net->endAt > 0) return;
    g_net->endAt = PIC_FullIndex() + kRoomEndMs;
    tdm_campaign_flight_ending();
    wclog(1, "TIE demo: %s", line.c_str());
    notice(line);
}
void room_end(int seat, const char *what) {
    if (!tdm_in_flight() || g_net->endAt > 0 || !others_flying()) return;
    const std::string who = seat == g_net->self ? std::string("You") : pilot_name(seat);
    const bool youDown = seat == g_net->self && !strcmp(what, " is down");
    end_for_room(who + (youDown ? " are down" : what) + ": the flight ends for everybody");
}
void room_end_frame() {
    const int own = tdm_player_object();
    if (own >= 0 && own < kCraftSlots && o8(own, 4) != 0 && dying(own) && others_flying()) room_end(g_net->self, " is down");
    if (g_net->endAt > 0 && PIC_FullIndex() >= g_net->endAt) {
        g_net->endAt = 0;
        g_net->roomEnded = true;
        mem_writeb(tdm_ds(kMissionEnd), 1);
        wclog(1, "TIE demo: the flight ended for the room (the mission's end flag set)");
    }
}

// A line a second: our craft, the others' as this game has them, the craft we fly for them, the counts.
void report_frame(double now) {
    static double reportAt = 0;
    if (now - reportAt < 1000 && now >= reportAt) return;
    reportAt = now;
    std::string line;
    const int own = tdm_player_object();
    const int tgt = ds16(kTarget);
    if (tgt < kCraftSlots && is_craft(tgt) && tdm_craft(tgt)) {
        const PhysPt cr = tdm_craft(tgt);
        char one[120];
        snprintf(one, sizeof(one), " target obj %d (%s): hull damage %u of %u;", tgt, mine(tgt) ? "ours" : "another's",
                 mem_readw(cr + kCraftHull), mem_readw(cr + kCraftHullMax));
        line += one;
    }
    for (int o = 0; o < kObjectCount; o++) {
        const Puppet &p = g_net->puppets[o];
        if (!p.used && o != own) continue;
        char one[220];
        if (o == own) {
            snprintf(one, sizeof(one), " own obj %d: %d %d %d hpr %u %u %u speed %d;", o, o32(o, 6), o32(o, 0xA), o32(o, 0xE), o16(o, 0x1E),
                     o16(o, 0x20), o16(o, 0x22), (Bit16s)o16(o, 0x26));
        } else if (o < kCraftSlots && g_net->owner[o] == g_net->self) {
            continue;
        } else {
            snprintf(one, sizeof(one), " obj %d of pilot %d: here %d %d %d speed %d type %u, reported %d %d %d %.0f ms ago;", o, p.owner + 1,
                     o32(o, 6), o32(o, 0xA), o32(o, 0xE), (Bit16s)o16(o, 0x26), o8(o, 4), p.state.x(), p.state.y(), p.state.z(), now - p.at);
        }
        line += one;
    }
    int owned[kMaxSeats] = { 0 };
    int craft = 0;
    for (int o = 0; o < kCraftSlots; o++) {
        if (!is_craft(o)) continue;
        craft++;
        if (g_net->owner[o] >= 0 && g_net->owner[o] < kMaxSeats) owned[g_net->owner[o]]++;
        if (o == own || g_net->owner[o] != g_net->self) continue;
        char one[160];
        snprintf(one, sizeof(one), " mine obj %d: %d %d %d speed %d type %u;", o, o32(o, 6), o32(o, 0xA), o32(o, 0xE), (Bit16s)o16(o, 0x26), o8(o, 4));
        line += one;
    }
    char sums[560];
    snprintf(sums, sizeof(sums), " %d craft, owners %d/%d/%d; sent %u craft (%u near, %u mid, %u far) in %u bytes, %u owner changes; guns: %u shots"
             " sent, %u made, %u hits sent, %u applied, %u damage skipped, %u kills, %u credited; far: %u flown by our AI, %u reports pulled"
             " in, %u near ones blended, %u hits of ours shown;", craft, owned[0], owned[1], owned[2], g_net->sentCraft, g_net->sentNear,
             g_net->sentMid, g_net->sentFar, g_net->sentBytes, g_net->ownerChanges, g_net->shotsSent, g_net->shotsMade, g_net->hitsSent,
             g_net->hitsApplied, g_net->damageSkipped, g_net->kills, g_net->credited, g_net->localAiNow, g_net->farReports, g_net->nearBlends,
             g_net->hitsShown);
    g_net->zero_counts();
    wclog(2, "TIE demo: %.1fs: frame %llu as pilot %d (%.1f ms a frame);%s%s", now / 1000.0, (unsigned long long)g_net->frame,
          g_net->self + 1, tdm_frame_ms(), sums, line.c_str());
}

}  // namespace

// --- the game's hooks ---------------------------------------------------------------------------------------------

void tdm_session_tick() {
    const double now = PIC_FullIndex();
    if (g_noticeAt > 0 && now - g_noticeAt > kNoticeMs) {
        incoming_text.clear();
        g_noticeAt = -1;
    }
    if (!g_net) {
        if (g_tried && now < g_nextTryMs) return;
        g_tried = true;
        g_nextTryMs = now + (getenv("WCHOST") && getenv("WCHOST")[0] ? 1000 : 10000);   // (a TCP guest early for its host: soon)
        if (!configured() || !start()) return;
    }
    if (now - g_pollAt >= kPollMs || now < g_pollAt) {
        g_pollAt = now;
        poll_peers();
        if (!g_net->host && g_net->peers.empty()) {
            tdm_session_shutdown();     // (the host gone: tried again later, as at the start)
            return;
        }
    }
    if (!tdm_in_flight() && now - g_stateAt >= kStateEveryMs) send_state(false);
}

// --- the campaign's (wcnet_tiedemo_campaign.cpp) -------------------------------------------------------------------

TdmRole tdm_session_role() {
    if (!g_net || g_net->self < 0 || g_net->alone) return TDM_SOLO;
    return g_net->host ? TDM_HOST : TDM_GUEST;
}

int tdm_session_launch_mission() { return g_net ? (int)g_net->hostMission : 0; }

bool tdm_session_room_ended() { return g_net && g_net->roomEnded; }

bool tdm_session_fly_alone() {
    if (!g_net || g_net->host || !g_net->atLaunch || g_net->self < 1) return false;
    g_net->soloNext = true;
    notice("Flying alone: the room's next launch takes you in again");
    wclog(1, "TIE demo: Esc at the launch: flown alone (the host was flying)");
    return true;
}

void tdm_session_verdict(int next, bool over) {
    if (!g_net || !g_net->host || !g_net->flownGo) return;
    g_net->verdictGo = g_net->flownGo;
    g_net->verdictNext = (Bit32u)next;
    g_net->verdictOver = over;
    send_state(true);
    wclog(1, "TIE demo: the verdict on launch %u sent: next mission %d%s", g_net->verdictGo, next, over ? ", the campaign over" : "");
}

bool tdm_session_verdict_wait(int *next, bool *over) {
    if (!g_net || g_net->host || g_net->peers.empty()) return false;
    poll_peers();
    if (!g_net) return false;
    const Remote &h = g_net->remotes[0];
    if (h.verdict < g_net->flownGo) return true;
    *next = (int)h.next;
    *over = h.over;
    return false;
}

void tdm_notice(const std::string &text) { notice(text); }

void tdm_session_shutdown() {
    if (!g_net) return;
    wclog(1, "TIE demo: session closed");
    delete g_net;
    g_net = NULL;
    g_tried = false;
}

// The launch (create_createmission's entry, polled while held): every pilot of the room there, then the host's go.
bool tdm_session_launch_hold() {
    if (!g_net) return false;
    const double now = PIC_FullIndex();
    if (!g_net->atLaunch) {
        g_net->atLaunch = true;
        g_net->launchSince = now;
        g_net->alone = false;
        send_state(true);
        if (others()) notice(g_net->host ? "Waiting for everybody to finish the opening: then you launch together" : "Waiting for the host's launch");
    }
    if (now - g_stateAt >= kStateEveryMs) send_state(false);
    if (g_net->host) {
        if (!others()) {
            g_net->alone = true;
            return false;
        }
        bool all = true;
        std::string late;
        for (size_t i = 0; i < g_net->peers.size(); i++) {
            const Peer *p = g_net->peers[i];
            if (!p->hello || !p->conn.is_open() || p->player < 0) continue;
            const Remote &r = g_net->remotes[p->player];
            if (!(r.atLaunch && now - r.at < kStaleMs)) {
                all = false;
                late += (late.empty() ? "" : ", ") + p->callsign;
            }
        }
        if (!all && now - g_net->launchSince < kLaunchWaitMs) return true;
        if (!all) notice("Launching without " + late + " (still in the opening after two minutes)");
        g_net->launchSerial++;
        g_net->flownGo = g_net->launchSerial;
        g_net->hostSeed = ds16(kSeed);
        g_net->hostSeed2 = ds16(kSeed2);
        g_net->hostMission = (Bit32u)tdm_campaign_next();
        send_state(true);
        wclog(1, "TIE demo: launch %u: everybody there, the seeds %04X %04X, mission %u", g_net->launchSerial, g_net->hostSeed,
              g_net->hostSeed2, g_net->hostMission);
        return false;
    }
    if (g_net->peers.empty() || g_net->soloNext) {
        g_net->alone = true;
        g_net->soloNext = false;
        return false;
    }
    const Remote &h = g_net->remotes[0];
    if (h.inFlight && h.go <= g_net->flownGo && !g_net->lateSaid) {
        g_net->lateSaid = true;
        notice("The host is flying a mission: you launch together at its next one. Esc flies alone meanwhile");
    }
    if (h.go > g_net->flownGo) {
        g_net->lateSaid = false;
        g_net->flownGo = h.go;
        g_net->hostSeed = h.seed;
        g_net->hostSeed2 = h.seed2;
        g_net->hostMission = h.mission;
        return false;
    }
    if (g_net->self >= 0 && now - g_net->launchSince >= kLaunchWaitMs * 2) {
        g_net->alone = true;            // (the host never launched: flown alone)
        return false;
    }
    return true;
}

// Let go at create_createmission's entry: a guest's game given the host's seeds and its own craft of the player's flight
// group (its record's player-craft number), before the mission is made.
void tdm_session_before_create() {
    if (!g_net) return;
    g_net->atLaunch = false;
    g_net->roomEnded = false;
    g_net->clear_flight();
    send_state(true);
    if (g_net->host || g_net->alone || g_net->self < 1) return;
    mem_writew(tdm_ds(kSeed), (Bit16u)g_net->hostSeed);
    mem_writew(tdm_ds(kSeed2), (Bit16u)g_net->hostSeed2);
    const PhysPt fgs = ((PhysPt)ds16(kFgSeg) << 4) + kFgBase;
    for (int fg = 0; fg < kFgCount; fg++) {
        const PhysPt rec = fgs + (PhysPt)fg * kFgSize;
        const int hostCraft = mem_readb(rec + kFgPlayerCraft);
        if (!hostCraft) continue;
        const int count = mem_readb(rec + kFgCraft);
        std::vector<int> spare;
        for (int k = 1; k <= count; k++) {
            if (k != hostCraft) spare.push_back(k);
        }
        if (g_net->self - 1 < (int)spare.size()) {
            mem_writeb(rec + kFgPlayerCraft, (Bit8u)spare[(size_t)(g_net->self - 1)]);
            wclog(1, "TIE demo: the host's seeds %04X %04X; we fly craft %d of flight group %d (the host craft %d of %d)",
                  g_net->hostSeed, g_net->hostSeed2, spare[(size_t)(g_net->self - 1)], fg, hostCraft, count);
        } else {
            wclog(1, "TIE demo: flight group %d has no craft left for seat %d (%d craft): we fly the host's", fg, g_net->self + 1, count);
        }
        break;
    }
}

void tdm_session_frame() {
    if (!g_net || g_net->self < 0 || !tdm_in_flight()) return;
    poll_peers();
    room_end_frame();
    report_frame(PIC_FullIndex());
    const int own = tdm_player_object();
    if (own < 0 || own >= kObjectCount) return;
    const double now = PIC_FullIndex();
    decide_owners(now);
    if (own < kCraftSlots) g_net->owner[own] = g_net->self;
    NetworkMessage msg;
    TieFrame *f = msg.mutable_tie_frame();
    f->set_player((Bit32u)g_net->self);
    f->set_frame(++g_net->frame);
    f->set_ticks(ds16(kFrameticks));
    f->set_flying(true);
    f->set_mission(kMissionName);
    f->set_own((Bit32u)own);
    f->set_target(ds16(kTarget));
    fill_craft(f->add_craft(), own);
    g_net->sentCraft++;
    int othersSeats[kMaxSeats], n = 0;
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && is_craft(pilot_object(k))) othersSeats[n++] = k;
    }
    bool far = false;
    if (n > 0) {
        int farPick = -1;
        if (g_net->frame % kFarEvery == 0) {
            for (int step = 1; step <= kCraftSlots && farPick < 0; step++) {
                const int o = (g_net->farCursor + step) % kCraftSlots;
                if (o == own || g_net->owner[o] != g_net->self || !is_craft(o)) continue;
                double d = 1e30;
                for (int i = 0; i < n; i++) d = fmin(d, distance(o, pilot_object(othersSeats[i])));
                if (d >= kMidReach) farPick = o;
            }
            if (farPick >= 0) g_net->farCursor = farPick;
        }
        for (int o = 0; o < kCraftSlots; o++) {
            if (o == own || g_net->owner[o] != g_net->self || !is_craft(o)) continue;
            double d = 1e30;
            for (int i = 0; i < n; i++) d = fmin(d, distance(o, pilot_object(othersSeats[i])));
            if (d < kReach) {
                g_net->sentNear++;
            } else if (d < kMidReach && (g_net->frame + o) % kMidEvery == 0) {
                g_net->sentMid++;
            } else if (o == farPick) {
                g_net->sentFar++;
                far = true;
            } else {
                continue;
            }
            fill_craft(f->add_craft(), o);
            g_net->sentCraft++;
        }
    }
    for (size_t i = 0; i < g_net->shotsOut.size(); i++) *f->add_shots() = g_net->shotsOut[i];
    for (size_t i = 0; i < g_net->hitsOut.size(); i++) *f->add_hits() = g_net->hitsOut[i];
    watch_events();
    for (size_t i = 0; i < g_net->eventsOut.size(); i++) *f->add_events() = g_net->eventsOut[i];
    g_net->shotsSent += (unsigned)g_net->shotsOut.size();
    g_net->hitsSent += (unsigned)g_net->hitsOut.size();
    const bool hits = !g_net->hitsOut.empty() || !g_net->eventsOut.empty();
    g_net->shotsOut.clear();
    g_net->hitsOut.clear();
    g_net->eventsOut.clear();
    const bool reliable = far || hits || now - g_net->lastReliableAt >= kReliableEveryMs;
    if (reliable) g_net->lastReliableAt = now;
    const unsigned bytes = (unsigned)msg.ByteSizeLong();
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (!p->hello || !p->conn.is_open()) continue;
        g_net->sentBytes += bytes;
        if (reliable) p->conn.send(msg); else p->conn.send_best_effort(msg);
    }
    // This machine's AI kept off every craft another flies near our pilot (before the planner runs this frame); a far
    // one is our AI's to fly (its place pulled to its owner's).
    g_net->localAiNow = 0;
    for (int o = 0; o < kCraftSlots; o++) {
        Puppet &p = g_net->puppets[o];
        if (o == own || mine(o) || !is_craft(o)) {
            p.localAi = false;
            continue;
        }
        if (pilot_flies(o)) {
            p.localAi = false;      // (another pilot's own craft: its reports alone move it)
            hold_ai(o);
            continue;
        }
        const bool was = p.localAi;
        const double d = distance(o, own);
        p.localAi = was ? d >= kLocalAiIn : d >= kLocalAiOut;
        if (p.localAi) {
            g_net->localAiNow++;
            if (!was) {
                const PhysPt craft = tdm_craft(o);
                if (craft) mem_writew(craft + kCraftAiCountdown, 0);
                g_net->localAiStarts++;
            }
        } else {
            if (was) g_net->localAiEnds++;
            hold_ai(o);
        }
    }
}

void tdm_session_before_move() {
    if (!g_net || g_net->self < 0 || !tdm_in_flight()) return;
    const int own = tdm_player_object();
    for (int o = 0; o < kObjectCount; o++) {
        Puppet &p = g_net->puppets[o];
        if (p.used && p.owner != g_net->self && o != own && (o >= kCraftSlots || owner_of(o) == p.owner)) write_puppet(p);
    }
}

// laser_createprojectile(shooter, hardpoint, type): a shot of a craft we fly, near another pilot, goes with our next
// frame; a craft another machine flies does not shoot here (its owner's shots are made from reports).
void tdm_session_shot() {
    if (!g_net || g_net->self < 0 || !tdm_in_flight() || tdm_in_call()) return;
    const int shooter = tdm_arg(0);
    if (shooter >= kCraftSlots) return;
    if (!mine(shooter)) {
        if (g_net->puppets[shooter].localAi) {
            g_net->localShots++;    // (a far craft our AI flies: its shot made here for show; it damages nothing here)
            return;
        }
        tdm_skip_call(0xFFFF);      // (no slot: its callers expect that)
        return;
    }
    if (g_net->shotsOut.size() < 32 && someone_near(shooter, kShotReach)) {
        TieShot shot;
        shot.set_obj((Bit32u)shooter);
        shot.set_hardpoint(tdm_arg(1));
        shot.set_type(tdm_arg(2));
        g_net->shotsOut.push_back(shot);
    }
}

// collide_laserhitcraft(bolt, target, offset): a hit by a bolt of a craft we fly on a craft another machine flies is
// that machine's to apply: sent with our next frame (the damage is skipped at collide_damagecraft).
void tdm_session_laser_hit() {
    if (!g_net || g_net->self < 0 || !tdm_in_flight() || tdm_in_call()) return;
    const int bolt = tdm_arg(0), target = tdm_arg(1);
    if (bolt < kCraftSlots || bolt >= kObjectCount || !is_craft(target)) return;
    const int shooter = o16(bolt, 0x30);
    if (!mine(shooter) || mine(target) || g_net->hitsOut.size() >= 32) return;
    TieHit h;
    h.set_target((Bit32u)target);
    h.set_offset(tdm_arg(2));
    h.set_bolt_type(o8(bolt, 4));
    h.set_bolt_genus(o8(bolt, 3));
    h.set_bolt_radius(o16(bolt, 0x2A));
    h.set_shooter((Bit32u)shooter);
    h.set_x(o32(bolt, 6));
    h.set_y(o32(bolt, 0xA));
    h.set_z(o32(bolt, 0xE));
    h.set_ox(o32(bolt, 0x12));
    h.set_oy(o32(bolt, 0x16));
    h.set_oz(o32(bolt, 0x1A));
    g_net->hitsOut.push_back(h);
}

// collide_damagecraft(target, component, attacker, side): this game damages only the craft it flies, and from a bolt
// only one whose shooter it flies.  Our own calls (a hit taken in, a kill) go through.
void tdm_session_damage() {
    if (!g_net || g_net->self < 0 || !tdm_in_flight() || tdm_in_call()) return;
    const int target = tdm_arg(0), attacker = tdm_arg(2);
    bool skip = target < kCraftSlots && !mine(target);
    if (skip && attacker >= kCraftSlots && attacker < kObjectCount && mine(o16(attacker, 0x30))) {
        const int genus = o8(target, 3);
        if (tdm_arg(1) == 0xFFFF || (genus != 4 && genus != 5)) show_hit(target, attacker, tdm_arg(3));
    }
    if (!skip && attacker >= kCraftSlots && attacker < kObjectCount) skip = !mine(o16(attacker, 0x30));
    if (skip) {
        g_net->damageSkipped++;
        tdm_skip_call(0);
    }
}

void tdm_session_flight_ended() {
    if (!g_net) return;
    wclog(1, "TIE demo: our flight is over");
    send_state(true);
    g_net->clear_flight();
}

}  // namespace wc
