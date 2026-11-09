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

void release_obj(Bit16u obj);       // (below: a puppet given back)

// Far off, each object at its own spot (by its address: 4 km apart): objects piled at one point cost the game's
// collision handling some 8 KB at once (measured: 16 puppets parked together as their owner died)
void park_pos(Bit16u obj, Bit32s out[3]) {
    out[0] = 0x30000000 + (Bit32s)(obj >> 1) * 0x100000 / 64;
    out[1] = 0x30000000 + (Bit32s)((obj >> 1) % 64) * 0x100000;
    out[2] = 0x30000000;
}

namespace {

enum {
    kMaxSeats = 8,
    kRetryMs = 10000,
    kHelloWaitMs = 20000,
    kPollMs = 20,                   // the sockets looked at this often (emulated ms)
    kPresenceEveryMs = 15000,       // the user's 15 seconds
    kNoticeMs = 10000,              // a notice stays this long
    kSameSystemEveryMs = 60000,     // the line of who shares our system: when they change, else this often while somebody does
};
enum Why { WHY_PERIOD = 0, WHY_AUTOPILOT = 1, WHY_JUMP = 2, WHY_LAUNCH = 3, WHY_LAND = 4, WHY_HELLO = 5, WHY_NAV = 6 };
// Step 5: a nav point's owner (the user's rule): of the pilots present at it, one with a mission objective there, the
// host first (the lowest seat), each in turn once everybody has left and come back, until all have had it.  Step 6:
// the owner's encounter ships are the others' puppets (PrivScene, kSceneEveryMs), hits on a ship cross to its owner.
enum { kOwnersEveryMs = 3000, kDecideEveryMs = 500, kSceneEveryMs = 100, kNpcCap = 16, kNpcStaleMs = 3000, kFireMs = 60,
       kShotEveryMs = 120, kCastSlots = 7, kCastAt = 0x0074 };
const double kUnitsPerMetre = 256.0;    // (the destination display's range: the distance in world units, >> 8)
const double kNearMetres = 20000.0;     // a pilot this near is "near" (a nav point's sphere: 12,000 to 17,000)
// Step 4: the others in our space (puppets).  A pilot sends its ship's state while somebody shares its system within
// kStateMetres (at most kStatesPerSecond); a pilot within kPuppetMetres is a ship in our game, gone again past
// kPuppetGoneMetres (parked far off, kept for its return), or when its reports stop for kStateStaleMs.
const double kStateMetres = 60000.0;
const double kPuppetMetres = 30000.0;
const double kPuppetGoneMetres = 40000.0;
enum { kStatesPerSecond = 15, kStateStaleMs = 5000, kExtrapolateMs = 1000 };
// (the AI and side a pilot's ship is made with here, its face on the comm screen: the pilot's own choice, a merchant's
// by default; its AI is held)
const char kPuppetCast[] = "MER_DP";
// The game's near heap (wcnet_priv.cpp, priv_near_heap): some 17 KB free as a flight begins at Achilles, 15 KB once it
// settles, nothing growing it, and the program ends when a request finds no block (its error 1303).  A puppet (its AI
// held) costs some 400-550 bytes; an AI-flown ship 850 and more as it fights.  Measured, a room of 8 in a dogfight
// with 4 pirates added (docs section 15): the guests' heaps never below 10,800 with 7 pilots and 7 encounter ships of
// others'; the owner of the nav point -- the real encounter's AI at work, with 7 pilots' puppets -- down to 3,402,
// where the game alone in the same fight went to 6,920; 1,000-1,600 requests a second, the largest 142 bytes in a
// fight (1,593 once).  So the memory governor (govern_memory, every frame): a puppet is made only while the last 5
// seconds' low stays kCreateFloor above its cost, and below kReleaseFloor one puppet a second is given back -- moved
// far off and killed, the game's own death path freeing it (its destructor called by us crashed the game) -- parked
// ones first, then the encounter ships, then the pilots, the farthest first: our footprint goes to nothing before
// the game's own needs do.
enum { kPodShown = 6 };
const char kDroneType[] = "DRONTYPE";   // the Steltek drone (appearance DRONE, cast SDRONE)
const double kDroneMetres = 30000.0;    // "that region": the pilots within this of the drone, and two drones within it are one
enum { kArriveSettleMs = 1500 };        // a nav point's wing waits this long after our arrival when others are in the system                 // pods of others' shown at once (the rest wait their turn)
enum { kShipNearBytes = 600, kPodNearBytes = 200, kCreateFloor = 8000, kReleaseFloor = 6000, kReleaseEveryMs = 1000,
       kUrgentFloor = 4500, kUrgentEveryMs = 250, kCreateEveryMs = 2000, kCreateAfterReleaseMs = 10000, kLowWindows = 5,
       kWingFloor = 9000 };            // (a wing of the game's own is some 3-4 ships: 3-4 KB as they fight)
enum { kSayThinkFrames = 60 };          // 3 s of the page's 20 a second
const char kPodCast[] = "CARGO";        // (a pod's name at +0xE, where a ship has its cast's)

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
    int nav;                    // the nav sphere it is in (-1 none)
    unsigned missions;          // the nav spheres it has a mission at (a bit each)
    std::string cast;           // the cast its ship is made with here, its face on our comm screen (its hello)
    int faceId;                 // the head wanted: 0x21 and over the cast's other head (a woman), else its first (a man)
    Pilot() : known(false), flying(false), x(0), y(0), z(0), at(-1e9), nav(-1), missions(0), cast("MER_DP"), faceId(-1) {}
};

// A nav point and who runs it (the host decides; everybody keeps the host's word).
struct NavOwner {
    std::string system;
    int nav;
    int owner;                  // a seat, -1 nobody (vacant)
    unsigned done;              // the seats that have owned it this round
    bool occupied;              // somebody there (the host's view)
};

// An encounter ship of another machine's as its reports say (PrivNpc), and the puppet it is in our game.
struct NpcPuppet {
    bool used;
    int seat;
    Bit32u id;
    std::string type, appearance, cast;
    double at;
    double x, y, z, vx, vy, vz;
    Bit32s m[9];
    bool alive;
    Bit32s armor[4];
    Bit16u obj, otype;          // the ship in our game (0: none)
    bool pending, parked, killed;
    double fireUntil;           // its guns' fire bit held until then
    bool pod;                   // a cargo pod (section 13): no AI, no armour, never killed; its cargo records and units
    std::vector<std::string> cargo;
    int units;
    NpcPuppet() : used(false), seat(-1), id(0), at(-1e9), x(0), y(0), z(0), vx(0), vy(0), vz(0), alive(true), obj(0), otype(0),
                  pending(false), parked(false), killed(false), fireUntil(-1e9), pod(false), units(0) {
        for (int i = 0; i < 9; i++) m[i] = i % 4 == 0 ? 256 : 0;
        for (int i = 0; i < 4; i++) armor[i] = 0;
    }
};

// Our own encounter ships' last places (their velocities for the reports), by near pointer.
struct NpcTrack {
    Bit16u obj;
    double x, y, z, at;
    double vx, vy, vz;
    double shotAt;
};

// A pilot's ship as its reports say (PrivState), and the puppet it is in our game.
struct Puppet {
    bool reported;
    double at;                  // the last report's time (emulated ms)
    std::string system, appearance;
    double x, y, z, vx, vy, vz;
    Bit32s m[9];
    Bit16u obj, type;           // the ship in our game (0: none)
    bool pending;               // a spawn queued
    bool parked;                // out of reach: put far off, kept
    double deadUntil;           // its death told: reports still in flight are not to make it again
    Puppet() : reported(false), at(-1e9), x(0), y(0), z(0), vx(0), vy(0), vz(0), obj(0), type(0), pending(false), parked(false), deadUntil(-1e9) {
        for (int i = 0; i < 9; i++) m[i] = i % 4 == 0 ? 256 : 0;
    }
};

struct PrivNet {
    bool host;
    int self;                   // our seat (a guest: -1 until the host's hello)
    int seats;
    std::string callsign;
    Listener *listener;
    std::vector<Peer *> peers;
    Pilot pilots[kMaxSeats];
    Puppet puppets[kMaxSeats];
    NpcPuppet npcs[kNpcCap];
    std::vector<NavOwner> owners;
    std::vector<NpcTrack> tracks;
    double ownersAt, decidedAt, sceneAt;
    // (what the frame applies: the far heap's EMS window is the game's own at the frame's top, not at a tick)
    std::vector<std::pair<int, std::string> > chats;    // seat, line
    std::vector<std::pair<int, Bit32u> > shots;         // seat, id
    std::vector<Bit32u> podsTaken;                      // pods of ours another pilot's tractor beam took (put away, not reported)
    std::vector<std::pair<Bit16u, Bit16u> > dying;      // (ship, type) puppets given back, until the game has freed them
    double navSince;                                    // when we came to our nav point
    int wingWaitNav;                                    // (a guest waiting for the host's word on a nav point's wing: which, since)
    double wingWaitSince;
    std::string extrasSystem;                           // the co-op extras made at this nav point in this flight
    int extrasNav;
    Bit16u extrasFlight;
    int extrasMade;
    std::vector<std::pair<Bit32u, double> > podsGone;   // (seat << 16 | id, when) pods of others' our tractor took: their owner's
                                                        // next reports, already on their way, are not to make them again
    Bit64u sceneFrame;
    int nav;                    // the nav sphere we are in (-1 none)
    double sentAt;              // our last presence
    double stateAt;             // our last state
    Bit64u frame;
    Bit16u flightShip;          // the player's ship pointer the puppets were made under (a new flight: they are gone)
    double lastX, lastY, lastZ, lastAt;     // our place at the last frame (for our velocity)
    double vx, vy, vz;
    PrivNet() : navSince(-1e9), wingWaitNav(-1), wingWaitSince(-1e9), extrasNav(-1), extrasFlight(0), extrasMade(0), host(false), self(-1), seats(2), listener(NULL), ownersAt(-1e9), decidedAt(-1e9), sceneAt(-1e9), sceneFrame(0), nav(-1),
                sentAt(-1e9), stateAt(-1e9), frame(0), flightShip(0), lastX(0), lastY(0), lastZ(0), lastAt(-1e9), vx(0), vy(0), vz(0) {}
    ~PrivNet() {
        for (size_t i = 0; i < peers.size(); i++) {
            delete peers[i];
        }
        delete listener;
    }
};

PrivNet *g_net;
void forget_puppets(const char *why);
void take_scene(const PrivScene &sc, int seat);
void take_hit(const PrivHit &h);
void take_shot(int seat, Bit32u id);
void take_chat(const Chat &c, int seat);
void frame_chats_and_shots();
double g_nextTryMs;
double g_noticeAt = -1;
double g_sameSystemAt = -1e9;
int g_pendingWhy = -1;          // an occasion to send at the next tick (the A key comes from the keyboard's thread of events)
bool g_wasFlying;
std::string g_system;           // our system, as last seen in flight (a base's is the system it is in)

// The emulator's line over the picture: only what cannot go anywhere else (the room's open, a hello, the host gone);
// where the others are stays in the log (the user, 2026-11-08: "we shouldn't do that overlay thing with guest
// close--it kinda breaks immersion. Players can communicate themselves using the 0 key").
void notice(const std::string &text) {
    incoming_text = text;
    g_noticeAt = PIC_FullIndex();
    wclog(1, "Privateer: %s", text.c_str());
}
void tell(const std::string &text) {
    wclog(1, "Privateer: %s", text.c_str());
}

// "<cast>:<id>" from WCPRIVFACE (the page's Face choice), MER_DP:-1 without.
std::string own_face() {
    const char *env = getenv("WCPRIVFACE");
    std::string f = env && env[0] ? env : "MER_DP:-1";
    if (f.find(':') == std::string::npos) f += ":-1";
    return f;
}
void parse_face(const std::string &f, std::string &cast, int &id) {
    const size_t c = f.find(':');
    cast = f.substr(0, c);
    id = c == std::string::npos ? -1 : atoi(f.c_str() + c + 1);
    if (cast != "MER_DP" && cast != "MIL_AA" && cast != "CON_SA" && cast != "BOU_AA") cast = "MER_DP";   // (the page's four)
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
        msg.mutable_priv_hello()->set_face(own_face());
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
        notice("Room open: everybody flies their own Privateer");
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
    pr.set_nav(flying ? g_net->nav : -1);
    const unsigned navs = priv_mission_navs();
    for (int n = 0; n < 32; n++) {
        if (navs & (1u << n)) pr.add_missions((Bit32u)n);
    }
    pr.set_face(own_face());
    return pr;
}

// Who is at (system, nav) by the presences we hold (ourselves too), as a bitmask of seats; in our own system a pilot
// whose latest reported place (its state, 15 a second when near) is inside the sphere counts too: fresher than its
// presence, which goes out as it arrives (pilots arriving together)
unsigned present_at(const std::string &system, int nav) {
    unsigned mask = 0;
    PrivSphere sp[16];
    const int ns = system == g_system ? priv_spheres(sp, 16) : 0;
    const PrivSphere *sphere = NULL;
    for (int i = 0; i < ns; i++) {
        if (sp[i].id == nav) sphere = &sp[i];
    }
    const double now = PIC_FullIndex();
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self) {
            if (priv_in_flight() && g_system == system && g_net->nav == nav) mask |= 1u << s;
        } else if (g_net->pilots[s].known && g_net->pilots[s].flying && g_net->pilots[s].system == system && g_net->pilots[s].nav == nav) {
            mask |= 1u << s;
        } else if (sphere && g_net->puppets[s].reported && now - g_net->puppets[s].at < 2000 && g_net->puppets[s].system == system) {
            const Puppet &p = g_net->puppets[s];
            const double dx = p.x - sphere->x, dy = p.y - sphere->y, dz = p.z - sphere->z;
            const double r = (double)sphere->radius * kUnitsPerMetre;
            if (dx * dx + dy * dy + dz * dz <= r * r) mask |= 1u << s;
        }
    }
    return mask;
}

NavOwner *owner_entry(const std::string &system, int nav, bool make) {
    for (size_t i = 0; i < g_net->owners.size(); i++) {
        if (g_net->owners[i].system == system && g_net->owners[i].nav == nav) return &g_net->owners[i];
    }
    if (!make || g_net->owners.size() >= 256) {
        return NULL;
    }
    NavOwner o = { system, nav, -1, 0, false };
    g_net->owners.push_back(o);
    return &g_net->owners.back();
}

int owner_of(const std::string &system, int nav) {
    NavOwner *o = owner_entry(system, nav, false);
    return o ? o->owner : -1;
}

// The owner by the user's rule, the same on every machine: of those present, the ones with a mission objective there
// (else all present); of them the lowest seat that has not owned it this round; everybody done: a new round.
unsigned mission_holders(unsigned present, int nav) {
    unsigned mission = 0;
    for (int s = 0; s < kMaxSeats; s++) {
        const unsigned m = s == g_net->self ? priv_mission_navs() : g_net->pilots[s].missions;
        if ((present & (1u << s)) && nav >= 0 && nav < 32 && (m & (1u << nav))) mission |= 1u << s;
    }
    return mission;
}

int choose_owner(unsigned present, int nav, unsigned &done, unsigned *missionOut) {
    const unsigned mission = mission_holders(present, nav);
    if (missionOut) *missionOut = mission;
    unsigned cands = mission ? mission : present;
    if (!(cands & ~done)) {
        done = 0;
    }
    cands &= ~done;
    for (int s = 0; s < kMaxSeats; s++) {
        if (cands & (1u << s)) return s;
    }
    return -1;
}

// The host's decision for every nav point somebody is at (the user's rule).
void decide_owners(double now, bool &changed) {
    for (int s = 0; s < kMaxSeats; s++) {
        const bool me = s == g_net->self;
        const bool flying = me ? priv_in_flight() : g_net->pilots[s].known && g_net->pilots[s].flying;
        const int nav = me ? g_net->nav : g_net->pilots[s].nav;
        const std::string &system = me ? g_system : g_net->pilots[s].system;
        if (!flying || nav < 0 || system.empty()) continue;
        owner_entry(system, nav, true);
    }
    for (size_t i = 0; i < g_net->owners.size(); i++) {
        NavOwner &o = g_net->owners[i];
        const unsigned present = present_at(o.system, o.nav);
        if (!present) {
            if (o.owner >= 0 || o.occupied) {
                o.owner = -1;           // (vacant: the next comers choose again)
                o.occupied = false;
                changed = true;
            }
            continue;
        }
        o.occupied = true;
        if (o.owner >= 0 && (present & (1u << o.owner))) {
            continue;                   // (the owner is still there: it stays)
        }
        unsigned mission = 0;
        const int pick = choose_owner(present, o.nav, o.done, &mission);
        if (pick >= 0 && pick != o.owner) {
            o.owner = pick;
            o.done |= 1u << pick;
            changed = true;
            wclog(1, "Privateer: %s nav %d: pilot %d owns it (present %02x, with a mission %02x, had it %02x)", o.system.c_str(), o.nav,
                  pick + 1, present, mission, o.done);
        }
    }
    (void)now;
}

void send_owners() {
    NetworkMessage msg;
    PrivOwners *po = msg.mutable_priv_owners();
    for (size_t i = 0; i < g_net->owners.size(); i++) {
        if (g_net->owners[i].owner < 0 && !g_net->owners[i].occupied && !g_net->owners[i].done) continue;
        PrivOwner *o = po->add_owners();
        o->set_system(g_net->owners[i].system);
        o->set_nav(g_net->owners[i].nav);
        o->set_owner((Bit32u)(g_net->owners[i].owner < 0 ? 0xFF : g_net->owners[i].owner));
        o->set_done(g_net->owners[i].done);
    }
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
    g_net->ownersAt = PIC_FullIndex();
}

void take_owners(const PrivOwners &po) {
    for (size_t i = 0; i < g_net->owners.size(); i++) {
        g_net->owners[i].owner = -1;
    }
    for (int i = 0; i < po.owners_size(); i++) {
        const PrivOwner &o = po.owners(i);
        NavOwner *e = owner_entry(o.system(), (int)o.nav(), true);
        if (e) {
            const int was = e->owner;
            e->owner = o.owner() == 0xFF ? -1 : (int)o.owner();
            e->occupied = e->owner >= 0;
            e->done = o.done();
            if (e->owner != was && (e->owner == g_net->self || was == g_net->self)) {
                wclog(1, "Privateer: %s nav %d: %s", o.system().c_str(), (int)o.nav(), e->owner == g_net->self ? "ours to run" : "another's to run");
            }
        }
    }
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
        static const char *const kWhy[] = { "the period", "the autopilot", "a jump", "a launch", "a landing", "a hello", "a nav point" };
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
    p.nav = pr.has_nav() ? (int)pr.nav() : -1;
    if (pr.has_face() && !pr.face().empty()) {
        parse_face(pr.face(), p.cast, p.faceId);
    }
    p.missions = 0;
    for (int i = 0; i < pr.missions_size(); i++) {
        if (pr.missions(i) < 32) p.missions |= 1u << pr.missions(i);
    }
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
        if (!wasKnown || !wasFlying || wasSystem != p.system || why != WHY_PERIOD) {
            char nav[24] = "";
            if (p.nav >= 0) snprintf(nav, sizeof(nav), " at Nav %d", p.nav);
            what = std::string(why == WHY_AUTOPILOT ? "on autopilot " : why == WHY_JUMP ? "jumped in " : why == WHY_LAUNCH ? "launched " : "")
                   + "in " + p.system + nav + ", " + metres(d) + " from you" + (d < kNearMetres ? " (near)" : "");
        }
    } else if (!wasKnown || !wasFlying || wasSystem != p.system || why == WHY_JUMP || why == WHY_LAUNCH) {
        what = (why == WHY_JUMP ? "jumped to " : why == WHY_LAUNCH ? "launched in " : "in ") + p.system;
    }
    if (!what.empty()) {
        tell(p.callsign + " " + what);
    }
}

double now_ms() { return PIC_FullIndex(); }

void take_state(const PrivState &st, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) {
        return;
    }
    Puppet &p = g_net->puppets[seat];
    p.reported = true;
    p.at = PIC_FullIndex();
    p.system = st.system();
    p.appearance = st.appearance();
    p.x = st.x();
    p.y = st.y();
    p.z = st.z();
    p.vx = st.vx();
    p.vy = st.vy();
    p.vz = st.vz();
    for (int i = 0; i < 9 && i < st.m_size(); i++) {
        p.m[i] = st.m(i);
    }
    if (now_ms() < p.deadUntil) {
        p.reported = false;         // (reports sent before its death, still arriving)
        return;
    }
    if (st.destroyed()) {
        // its ship destroyed in its own game: ours dies here too (an explosion; no cargo: its real pods are its game's)
        p.deadUntil = now_ms() + 5000;
        p.reported = false;
        p.pending = false;
        if (p.obj && !p.parked && priv_ship_alive(p.obj, p.type)) {
            if (g_net->dying.size() < 32) g_net->dying.push_back(std::make_pair(p.obj, p.type));
            priv_hold_ai(priv_ship_controller(p.obj), false);
            priv_ship_kill(p.obj);
            wclog(1, "Privateer: pilot %d's ship destroyed, as its game says", seat + 1);
        }
        p.obj = 0;
    }
}

void take_pod_taken(const PrivPodTaken &t);

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
            parse_face(h.face(), g_net->pilots[player].cast, g_net->pilots[player].faceId);
            NetworkMessage reply;
            reply.mutable_priv_hello()->set_callsign(g_net->callsign);
            reply.mutable_priv_hello()->set_player((Bit32u)player);
            reply.mutable_priv_hello()->set_seats((Bit32u)g_net->seats);
            reply.mutable_priv_hello()->set_face(own_face());
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
                pr->set_face(q.cast + ":" + std::to_string(q.faceId));
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
            parse_face(h.face(), g_net->pilots[0].cast, g_net->pilots[0].faceId);
            if (h.has_seats() && h.seats() >= 2) {
                g_net->seats = (int)h.seats();
            }
            g_pendingWhy = WHY_HELLO;
            char line[160];
            snprintf(line, sizeof(line), "In %s's room as pilot %d of %d", p->callsign.c_str(), g_net->self + 1, g_net->seats);
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
    } else if (msg.has_priv_state()) {
        if (!p->hello) {
            return;
        }
        const int seat = g_net->host ? p->player : (msg.priv_state().has_player() ? (int)msg.priv_state().player() : 0);
        take_state(msg.priv_state(), seat);
        if (g_net->host) {
            NetworkMessage out;
            *out.mutable_priv_state() = msg.priv_state();
            out.mutable_priv_state()->set_player((Bit32u)p->player);
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *to = g_net->peers[i];
                if (to != p && to->hello && to->conn.is_open()) {
                    if (msg.priv_state().destroyed()) to->conn.send(out); else to->conn.send_best_effort(out);
                }
            }
        }
    } else if (msg.has_priv_owners()) {
        if (p->hello && !g_net->host) {
            take_owners(msg.priv_owners());
        }
    } else if (msg.has_priv_scene()) {
        if (!p->hello) {
            return;
        }
        const int seat = g_net->host ? p->player : (msg.priv_scene().has_player() ? (int)msg.priv_scene().player() : 0);
        take_scene(msg.priv_scene(), seat);
        if (g_net->host) {
            NetworkMessage out;
            *out.mutable_priv_scene() = msg.priv_scene();
            out.mutable_priv_scene()->set_player((Bit32u)p->player);
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *to = g_net->peers[i];
                if (to != p && to->hello && to->conn.is_open()) {
                    to->conn.send_best_effort(out);
                }
            }
        }
    } else if (msg.has_priv_hit()) {
        if (!p->hello) {
            return;
        }
        const PrivHit &h = msg.priv_hit();
        if ((int)h.owner() == g_net->self) {
            take_hit(h);
        } else if (g_net->host) {
            NetworkMessage out;
            *out.mutable_priv_hit() = h;
            out.mutable_priv_hit()->set_player((Bit32u)p->player);
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *to = g_net->peers[i];
                if (to != p && to->hello && to->conn.is_open() && to->player == (int)h.owner()) {
                    to->conn.send(out);
                }
            }
        }
    } else if (msg.has_priv_pod_taken()) {
        if (!p->hello) {
            return;
        }
        PrivPodTaken t = msg.priv_pod_taken();
        if (g_net->host) {
            t.set_player((Bit32u)p->player);
            if ((int)t.owner() != g_net->self) {
                NetworkMessage out;
                *out.mutable_priv_pod_taken() = t;
                for (size_t i = 0; i < g_net->peers.size(); i++) {
                    Peer *q = g_net->peers[i];
                    if (q->hello && q->conn.is_open() && q->player == (int)t.owner()) q->conn.send(out);
                }
                return;
            }
        }
        take_pod_taken(t);
    } else if (msg.has_priv_shot()) {
        if (!p->hello) {
            return;
        }
        const int seat = g_net->host ? p->player : (msg.priv_shot().has_player() ? (int)msg.priv_shot().player() : 0);
        take_shot(seat, msg.priv_shot().id());
        if (g_net->host) {
            NetworkMessage out;
            *out.mutable_priv_shot() = msg.priv_shot();
            out.mutable_priv_shot()->set_player((Bit32u)p->player);
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *to = g_net->peers[i];
                if (to != p && to->hello && to->conn.is_open()) {
                    to->conn.send_best_effort(out);
                }
            }
        }
    } else if (msg.has_chat()) {
        if (!p->hello) {
            return;
        }
        take_chat(msg.chat(), g_net->host ? p->player : -1);
        if (g_net->host) {
            for (size_t i = 0; i < g_net->peers.size(); i++) {
                Peer *to = g_net->peers[i];
                if (to != p && to->hello && to->conn.is_open()) {
                    to->conn.send(msg);
                }
            }
        }
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
                Puppet &pp = g_net->puppets[p->player];
                pp.reported = false;    // (its ship parked at the next frame, kept)
                for (int k = 0; k < kNpcCap; k++) {
                    if (g_net->npcs[k].used && g_net->npcs[k].seat == p->player) g_net->npcs[k].at = -1e9;
                }
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
                // a new system, a new scene: the old one's ships (ours among them) are the game's to free; what we
                // had queued for them is dropped
                if (g_net) {
                    forget_puppets("a jump");
                }
                priv_session_event(WHY_JUMP);
            }
        }
    }
    if (flying != g_wasFlying) {
        priv_session_event(flying ? WHY_LAUNCH : WHY_LAND);
        g_wasFlying = flying;
        if (!flying && g_net) {
            forget_puppets("landed");
            g_net->flightShip = 0;
        }
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
    // our nav sphere, an occasion when it changes
    if (priv_in_flight()) {
        Bit32s pos[3];
        priv_player_pos(pos);
        const int nav = priv_nav_at(pos);
        if (nav != g_net->nav) {
            wclog(1, "Privateer: %s nav %d (was %d)", nav >= 0 ? "at" : "left", nav >= 0 ? nav : g_net->nav, g_net->nav);
            g_net->nav = nav;
            g_net->navSince = now;
            priv_session_event(WHY_NAV);
        }
    } else if (g_net->nav >= 0) {
        g_net->nav = -1;
    }
    if (g_net->host && now - g_net->decidedAt >= kDecideEveryMs) {
        g_net->decidedAt = now;
        bool changed = false;
        decide_owners(now, changed);
        if (changed || now - g_net->ownersAt >= kOwnersEveryMs) {
            send_owners();
        }
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
    // who shares our system: said when that changes (a pilot in or out), else once a minute while somebody does
    static std::string lastWho;
    std::string who;
    for (int s = 0; s < kMaxSeats; s++) {
        if (s != g_net->self && g_net->pilots[s].known && distance_to(g_net->pilots[s]) >= 0) {
            who += (char)('0' + s);
        }
    }
    if ((who != lastWho || now - g_sameSystemAt >= kSameSystemEveryMs) && priv_in_flight()) {
        g_sameSystemAt = now;
        lastWho = who;
        const std::string line = same_system_line();
        if (!line.empty()) {
            tell(line);
        }
    }
}

namespace {

// The NPC type the game makes a player's ship as, by the player's appearance.
const char *type_of(const std::string &appearance) {
    if (appearance == "TUG") return "ORIOTYPE";
    if (appearance == "FIGHTER") return "CENTTYPE";
    if (appearance == "MERCHANT") return "GLXYTYPE";
    return "TARSTYPE";
}

// The governor's view of the near heap: the lowest free of each second, the last kLowWindows of them
struct HeapWindows {
    double start;
    int low;                    // this second's
    int past[kLowWindows];      // the seconds before (1 << 30: none)
    double releasedAt, createdAt;
    int released;               // puppets given back in this flight
    int lowSinceRelease;        // the lowest since the last give-back (what the next one is decided on)
    HeapWindows() : start(-1e9), low(1 << 30), releasedAt(-1e9), createdAt(-1e9), released(0), lowSinceRelease(1 << 30) {
        for (int i = 0; i < kLowWindows; i++) past[i] = 1 << 30;
    }
} g_heap;

void heap_windows_frame(double now) {
    const int low = priv_near_heap_low_take();
    if (low >= 0 && low < g_heap.low) g_heap.low = low;
    if (low >= 0 && low < g_heap.lowSinceRelease) g_heap.lowSinceRelease = low;
    if (now - g_heap.start >= 1000 || now < g_heap.start) {
        for (int i = kLowWindows - 1; i > 0; i--) g_heap.past[i] = g_heap.past[i - 1];
        g_heap.past[0] = g_heap.low;
        g_heap.low = 1 << 30;
        g_heap.start = now;
    }
}

int heap_low_recent(int seconds) {
    int low = g_heap.low;
    for (int i = 0; i < seconds && i < kLowWindows; i++) {
        if (g_heap.past[i] < low) low = g_heap.past[i];
    }
    return low;
}

// Room in the game's near heap for one more ship of ours: the last seconds' low kCreateFloor above its cost, and now's
// too (logged when there is none, every 10 s)
bool room_for_a_ship(const char *whose, int bytes = kShipNearBytes) {
    // one at a time: a spawn queued and not yet made has not shown in the heap yet
    const double now = PIC_FullIndex();
    if (now - g_heap.createdAt < kCreateEveryMs || now - g_heap.releasedAt < kCreateAfterReleaseMs) {
        return false;
    }
    for (int s = 0; s < kMaxSeats; s++) {
        if (g_net->puppets[s].pending) return false;
    }
    for (int k = 0; k < kNpcCap; k++) {
        if (g_net->npcs[k].used && g_net->npcs[k].pending) return false;
    }
    int total, largest, blocks;
    const bool known = priv_near_heap_read(total, largest, blocks);
    const int recent = heap_low_recent(kLowWindows);
    if (known && total - bytes >= kCreateFloor && (recent >= (1 << 30) || recent - bytes >= kCreateFloor)) {
        g_heap.createdAt = now;
        return true;
    }
    static double saidAt = -1e9;
    if (PIC_FullIndex() - saidAt > 10000) {
        saidAt = PIC_FullIndex();
        wclog(1, "Privateer: no room in the game's memory for %s (near heap free %d, the last %d s' low %d, wanted %d over its %d bytes): not shown",
              whose, known ? total : -1, (int)kLowWindows, recent < (1 << 30) ? recent : -1, (int)kCreateFloor, bytes);
    }
    return false;
}

std::vector<std::vector<Bit8u> > cargo_records(const NpcPuppet &p) {
    std::vector<std::vector<Bit8u> > out;
    for (size_t r = 0; r < p.cargo.size(); r++) out.push_back(std::vector<Bit8u>(p.cargo[r].begin(), p.cargo[r].end()));
    return out;
}

void puppet_made(int seat, Bit16u obj) {
    if (!g_net || seat < 0 || seat >= kMaxSeats) {
        return;
    }
    Puppet &p = g_net->puppets[seat];
    p.pending = false;
    if (!obj) {
        wclog(1, "Privateer: pilot %d's ship could not be made", seat + 1);
        return;
    }
    p.type = priv_ship_type(obj);
    p.parked = false;
    if (!priv_hold_ai(priv_ship_controller(obj), true)) {
        // (its AI cannot be held: it would fly as it likes; taken away again)
        if (g_net->dying.size() < 32) g_net->dying.push_back(std::make_pair(obj, p.type));
        release_obj(obj);
        wclog(1, "Privateer: pilot %d's ship could not be held: not shown", seat + 1);
        return;
    }
    p.obj = obj;
    const std::string who = g_net->pilots[seat].callsign.empty() ? "a pilot" : g_net->pilots[seat].callsign;
    tell(who + "'s " + (p.appearance == "TUG" ? "Orion" : p.appearance == "FIGHTER" ? "Centurion" : p.appearance == "MERCHANT" ? "Galaxy" : "Tarsus")
         + " is in your space");
    wclog(2, "Privateer: pilot %d's ship is ds:%04X (type ds:%04X, controller ds:%04X held)", seat + 1, obj, p.type, priv_ship_controller(obj));
}

void forget_puppets(const char *why) {
    priv_calls_flush(why);
    priv_wings_flight_changed();
    bool any = false;
    for (int s = 0; s < kMaxSeats; s++) {
        Puppet &p = g_net->puppets[s];
        any = any || p.obj;
        p.obj = 0;
        p.type = 0;
        p.pending = false;
        p.parked = false;
    }
    for (int k = 0; k < kNpcCap; k++) {
        any = any || g_net->npcs[k].obj;
        g_net->npcs[k] = NpcPuppet();
    }
    g_net->tracks.clear();
    g_net->podsTaken.clear();
    g_net->podsGone.clear();
    g_net->dying.clear();
    priv_hold_ai_none();
    if (any) {
        wclog(2, "Privateer: the puppets forgotten (%s)", why);
    }
}

void send_state(double now) {
    Bit32s pos[3], m[9];
    priv_player_pos(pos);
    priv_player_matrix(m);
    NetworkMessage msg;
    PrivState *st = msg.mutable_priv_state();
    st->set_player((Bit32u)g_net->self);
    st->set_frame(++g_net->frame);
    st->set_system(g_system);
    st->set_x(pos[0]);
    st->set_y(pos[1]);
    st->set_z(pos[2]);
    for (int i = 0; i < 9; i++) {
        st->add_m(m[i]);
    }
    st->set_appearance(priv_player_appearance());
    st->set_vx((Bit32s)g_net->vx);
    st->set_vy((Bit32s)g_net->vy);
    st->set_vz((Bit32s)g_net->vz);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send_best_effort(msg);
        }
    }
    g_net->stateAt = now;
}

// The cast names our puppets keep a pointer to (an encounter ship's "PIR_DA"...): seven of 8 bytes in the program's
// usage line at DGROUP:0074, which no flight reads (its signature was checked at the load).
Bit16u cast_slot(const std::string &cast) {
    static std::string names[kCastSlots];
    for (int i = 0; i < kCastSlots; i++) {
        if (names[i] == cast) return (Bit16u)(kCastAt + 8 * i);
    }
    for (int i = 0; i < kCastSlots; i++) {
        if (names[i].empty()) {
            names[i] = cast;
            for (int k = 0; k < 8; k++) {
                priv_w8((Bit16u)(kCastAt + 8 * i + k), (Bit8u)(k < (int)cast.size() ? cast[k] : 0));
            }
            return (Bit16u)(kCastAt + 8 * i);
        }
    }
    return (Bit16u)kCastAt;      // (all seven taken: the first's)
}

NpcTrack *track_of(Bit16u obj, bool make) {
    for (size_t i = 0; i < g_net->tracks.size(); i++) {
        if (g_net->tracks[i].obj == obj) return &g_net->tracks[i];
    }
    if (!make || g_net->tracks.size() >= 48) return NULL;
    NpcTrack t = { obj, 0, 0, 0, -1e9, 0, 0, 0, -1e9 };
    g_net->tracks.push_back(t);
    return &g_net->tracks.back();
}

bool is_puppet_obj(Bit16u obj) {
    for (size_t i = 0; i < g_net->dying.size(); i++) {
        if (g_net->dying[i].first == obj) return true;
    }
    for (int s = 0; s < kMaxSeats; s++) {
        if (g_net->puppets[s].obj == obj) return true;
    }
    for (int k = 0; k < kNpcCap; k++) {
        if (g_net->npcs[k].used && g_net->npcs[k].obj == obj) return true;
    }
    return false;
}

// Another pilot flying in our system within d metres of us.
bool somebody_within(double d) {
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self) continue;
        const double x = distance_to(g_net->pilots[s]);
        if (x >= 0 && x < d) return true;
    }
    return false;
}

// Our encounter ships to the others (the owner of the nav we are in): every ship of the scene that is not ours and
// not a puppet, within reach of us.
bool pod_taken(Bit32u id) {
    return std::find(g_net->podsTaken.begin(), g_net->podsTaken.end(), id) != g_net->podsTaken.end();
}

// The owner's encounter ships (withShips), and anybody's cargo pods (expelled, or a ship's death's: section 13)
void send_scene(double now, bool withShips) {
    Bit16u ships[48], pods[16];
    int n = withShips ? priv_scene_ships(ships, 48) : 0;
    const int np = priv_scene_pods(pods, 16);
    const Bit16u own = priv_player_ship();
    Bit32s pos[3];
    priv_player_pos(pos);
    NetworkMessage msg;
    PrivScene *sc = msg.mutable_priv_scene();
    sc->set_player((Bit32u)g_net->self);
    sc->set_frame(++g_net->sceneFrame);
    sc->set_system(g_system);
    int sent = 0;
    for (int i = 0; i < np; i++) {
        const Bit16u o = pods[i];
        if (is_puppet_obj(o) || pod_taken(o)) continue;
        Bit32s p[3];
        priv_ship_pos(o, p);
        const double d = sqrt((double)(p[0] - pos[0]) * (p[0] - pos[0]) + (double)(p[1] - pos[1]) * (p[1] - pos[1])
                              + (double)(p[2] - pos[2]) * (p[2] - pos[2])) / kUnitsPerMetre;
        if (d > kStateMetres) continue;
        std::vector<std::vector<Bit8u> > records;
        int units = 0;
        if (!priv_pod_cargo(o, records, units)) continue;
        NpcTrack *t = track_of(o, true);
        if (t) {
            if (now > t->at && now - t->at < 1000) {
                const double dt = (now - t->at) / 1000.0;
                t->vx = (p[0] - t->x) / dt;
                t->vy = (p[1] - t->y) / dt;
                t->vz = (p[2] - t->z) / dt;
            }
            t->x = p[0]; t->y = p[1]; t->z = p[2]; t->at = now;
        }
        PrivNpc *c = sc->add_npc();
        c->set_id(o);
        c->set_type(priv_ship_type_name(o));
        c->set_appearance(priv_ship_appearance_name(o));
        c->set_cast(kPodCast);
        c->set_x(p[0]); c->set_y(p[1]); c->set_z(p[2]);
        if (t) { c->set_vx((Bit32s)t->vx); c->set_vy((Bit32s)t->vy); c->set_vz((Bit32s)t->vz); }
        c->set_alive(true);
        c->set_pod(true);
        for (size_t r = 0; r < records.size(); r++) c->add_cargo(std::string(records[r].begin(), records[r].end()));
        c->set_units((Bit32u)units);
        sent++;
    }
    // a Steltek drone of ours is shared by whichever machine runs it, as the pods are (the user: "if a steltek drone is
    // following a player it should spawn but not more than 1.  And it should target the lowest player number in that
    // region")
    if (!withShips) {
        Bit16u all[48];
        const int na = priv_scene_ships(all, 48);
        for (int i = 0; i < na; i++) {
            if (all[i] != own && !is_puppet_obj(all[i]) && priv_ship_is_alive(all[i]) && priv_ship_type_name(all[i]) == kDroneType) {
                ships[n < 48 ? n : 47] = all[i];
                if (n < 48) n++;
            }
        }
    }
    if (!withShips && !sent && !n) {
        return;             // (nothing of ours to tell)
    }
    for (int i = 0; i < n; i++) {
        const Bit16u o = ships[i];
        if (o == own || is_puppet_obj(o)) continue;
        Bit32s p[3], m[9], a[4];
        priv_ship_pos(o, p);
        const double d = sqrt((double)(p[0] - pos[0]) * (p[0] - pos[0]) + (double)(p[1] - pos[1]) * (p[1] - pos[1])
                              + (double)(p[2] - pos[2]) * (p[2] - pos[2])) / kUnitsPerMetre;
        if (d > kStateMetres) continue;
        priv_ship_matrix(o, m);
        priv_ship_armor(o, a);
        NpcTrack *t = track_of(o, true);
        if (t) {
            if (now > t->at && now - t->at < 1000) {
                const double dt = (now - t->at) / 1000.0;
                t->vx = (p[0] - t->x) / dt;
                t->vy = (p[1] - t->y) / dt;
                t->vz = (p[2] - t->z) / dt;
            }
            t->x = p[0]; t->y = p[1]; t->z = p[2]; t->at = now;
        }
        PrivNpc *c = sc->add_npc();
        c->set_id(o);
        c->set_type(priv_ship_type_name(o));
        c->set_appearance(priv_ship_appearance_name(o));
        c->set_cast(priv_ship_cast_name(o));
        c->set_x(p[0]); c->set_y(p[1]); c->set_z(p[2]);
        for (int k = 0; k < 9; k++) c->add_m(m[k]);
        if (t) { c->set_vx((Bit32s)t->vx); c->set_vy((Bit32s)t->vy); c->set_vz((Bit32s)t->vz); }
        c->set_alive(priv_ship_is_alive(o));
        for (int k = 0; k < 4; k++) c->add_armor(a[k]);
    }
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send_best_effort(msg);
        }
    }
    g_net->sceneAt = now;
}

NpcPuppet *npc_of(int seat, Bit32u id, bool make) {
    for (int k = 0; k < kNpcCap; k++) {
        if (g_net->npcs[k].used && g_net->npcs[k].seat == seat && g_net->npcs[k].id == id) return &g_net->npcs[k];
    }
    if (!make) return NULL;
    for (int k = 0; k < kNpcCap; k++) {
        if (!g_net->npcs[k].used) {
            g_net->npcs[k] = NpcPuppet();
            g_net->npcs[k].used = true;
            g_net->npcs[k].seat = seat;
            g_net->npcs[k].id = id;
            return &g_net->npcs[k];
        }
    }
    return NULL;        // (a list has a cap: 12 encounter ships of others' at once; the game's near heap is some 30 KB)
}

// One drone a place: a drone of a lower seat's machine reported near our own (one our game made itself) makes ours its
// puppet -- our drone kept (the game's own object: its mission sees it live and die), its AI held, flown by the reports
void drone_adopt(int seat, const PrivNpc &c) {
    NpcPuppet *had = npc_of(seat, c.id(), false);
    if (had && had->obj) {
        return;
    }
    Bit16u ships[48];
    const int n = priv_scene_ships(ships, 48);
    for (int i = 0; i < n; i++) {
        const Bit16u d = ships[i];
        if (is_puppet_obj(d) || !priv_ship_is_alive(d) || priv_ship_type_name(d) != kDroneType) continue;
        Bit32s p[3];
        priv_ship_pos(d, p);
        const double dx = (double)p[0] - c.x(), dy = (double)p[1] - c.y(), dz = (double)p[2] - c.z();
        if (dx * dx + dy * dy + dz * dz > kDroneMetres * kDroneMetres * kUnitsPerMetre * kUnitsPerMetre) continue;
        NpcPuppet *np = npc_of(seat, c.id(), true);
        if (!np) return;
        np->obj = d;
        np->otype = priv_ship_type(d);
        np->type = c.type();
        np->appearance = c.appearance();
        np->cast = c.cast();
        np->parked = false;
        np->pending = false;
        priv_hold_ai(priv_ship_controller(d), true);
        wclog(1, "Privateer: our Steltek drone ds:%04X is now pilot %d's (one a place): its puppet here", d, seat + 1);
        return;
    }
}

// Our real drones (not puppets) given the lowest-numbered pilot within kDroneMetres as their target, each second
void drones_frame(double now) {
    static double at = -1e9;
    if (now - at < 1000) return;
    at = now;
    Bit16u ships[48];
    const int n = priv_scene_ships(ships, 48);
    for (int i = 0; i < n; i++) {
        const Bit16u d = ships[i];
        if (is_puppet_obj(d) || !priv_ship_is_alive(d) || priv_ship_type_name(d) != kDroneType) continue;
        Bit32s dp[3];
        priv_ship_pos(d, dp);
        Bit16u target = 0;
        int seat = -1;
        for (int s = 0; s < kMaxSeats && !target; s++) {
            const Bit16u ship = priv_session_pilot_ship(s);
            if (!ship || !priv_ship_is_alive(ship)) continue;
            Bit32s p[3];
            priv_ship_pos(ship, p);
            const double dx = (double)p[0] - dp[0], dy = (double)p[1] - dp[1], dz = (double)p[2] - dp[2];
            if (dx * dx + dy * dy + dz * dz <= kDroneMetres * kDroneMetres * kUnitsPerMetre * kUnitsPerMetre) { target = ship; seat = s; }
        }
        if (target) {
            priv_ship_set_target(d, target);    // (re-aimed only when it is after someone else: wcnet_priv.cpp)
            static Bit16u saidFor;
            if (saidFor != target) {
                saidFor = target;
                wclog(1, "Privateer: the Steltek drone ds:%04X is after pilot %d (the lowest here)", d, seat + 1);
            }
        }
    }
}

void take_scene(const PrivScene &sc, int seat) {
    if (seat < 0 || seat >= kMaxSeats || seat == g_net->self) {
        return;
    }
    const double now = PIC_FullIndex();
    for (int i = 0; i < sc.npc_size(); i++) {
        const PrivNpc &c = sc.npc(i);
        if (c.type() == kDroneType && seat < g_net->self) {
            drone_adopt(seat, c);
        }
        if (c.pod()) {
            bool gone = false;
            for (size_t g = 0; g < g_net->podsGone.size() && !gone; g++) {
                gone = g_net->podsGone[g].first == (((Bit32u)seat << 16) | c.id()) && now - g_net->podsGone[g].second < 5000;
            }
            if (gone) continue;
        }
        NpcPuppet *p = npc_of(seat, c.id(), true);
        if (!p) continue;
        p->at = now;
        p->type = c.type();
        p->appearance = c.appearance();
        p->cast = c.cast();
        p->x = c.x(); p->y = c.y(); p->z = c.z();
        p->vx = c.vx(); p->vy = c.vy(); p->vz = c.vz();
        for (int k = 0; k < 9 && k < c.m_size(); k++) p->m[k] = c.m(k);
        p->alive = c.alive();
        for (int k = 0; k < 4 && k < c.armor_size(); k++) p->armor[k] = c.armor(k);
        p->pod = c.pod();
        if (p->pod) {
            p->cargo.clear();
            for (int k = 0; k < c.cargo_size(); k++) p->cargo.push_back(c.cargo(k));
            p->units = (int)c.units();
        }
    }
}

void npc_made(int tag, Bit16u obj) {
    if (!g_net || tag < 0 || tag >= kNpcCap) return;
    NpcPuppet &p = g_net->npcs[tag];
    p.pending = false;
    if (!obj) {
        wclog(1, "Privateer: an encounter ship of pilot %d's (%s) could not be made", p.seat + 1, p.type.c_str());
        p.used = false;
        return;
    }
    p.obj = obj;
    p.otype = priv_ship_type(obj);
    p.parked = false;
    if (p.pod) {
        priv_pod_fill(obj, cargo_records(p), p.units);
    } else if (!priv_hold_ai(priv_ship_controller(obj), true)) {
        if (g_net->dying.size() < 32) g_net->dying.push_back(std::make_pair(obj, p.otype));
        release_obj(obj);
        p.obj = 0;
        p.used = false;
        wclog(1, "Privateer: pilot %d's %s could not be held: not shown", p.seat + 1, p.type.c_str());
        return;
    }
    wclog(2, "Privateer: pilot %d's %s (%s, %s) is ds:%04X here", p.seat + 1, p.type.c_str(), p.appearance.c_str(), p.cast.c_str(), obj);
}

// A pod of another's gone from our scene while it was here and fresh: our tractor beam took it (or the game let it
// go); its owner is told, to put its own away (section 13)
void send_pod_taken(int owner, Bit32u id) {
    NetworkMessage msg;
    PrivPodTaken *t = msg.mutable_priv_pod_taken();
    t->set_player((Bit32u)g_net->self);
    t->set_owner((Bit32u)owner);
    t->set_id(id);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (p->hello && p->conn.is_open() && (!g_net->host || p->player == owner)) {
            p->conn.send(msg);
        }
    }
    wclog(1, "Privateer: pilot %d's cargo pod (%u) taken here: told", owner + 1, id);
}

void take_pod_taken(const PrivPodTaken &t) {
    if ((int)t.owner() != g_net->self) {
        return;
    }
    const Bit16u obj = (Bit16u)t.id();
    if (!priv_pod_is(obj) || !priv_scene_has(obj) || is_puppet_obj(obj)) {
        return;
    }
    // (never deleted: put far off, where the game lets it expire; not reported any more)
    Bit32s away[3];
    park_pos(obj, away);
    priv_pod_set(obj, away);
    if (!pod_taken(obj) && g_net->podsTaken.size() < 64) g_net->podsTaken.push_back(obj);
    wclog(1, "Privateer: our cargo pod ds:%04X taken by pilot %d: put away", obj, (int)t.player() + 1);
}

void take_hit(const PrivHit &h) {
    Bit16u obj = 0;
    if (h.id() == 0) {
        obj = priv_player_ship();
    } else {
        Bit16u ships[48];
        const int n = priv_scene_ships(ships, 48);
        for (int i = 0; i < n && !obj; i++) {
            if (ships[i] == (Bit16u)h.id() && !is_puppet_obj(ships[i])) obj = ships[i];
        }
    }
    if (!obj) {
        return;
    }
    wclog(2, "Privateer: a hit from pilot %d on %s (ds:%04X): quadrant %d, %d", (int)h.player() + 1, h.id() ? "an encounter ship of ours" : "our ship",
          obj, (int)h.loc(), (int)h.damage());
    priv_hit_apply(obj, (int)h.loc(), (int)h.damage());
}

// A line of the room's chat: said by the sender's ship in our game when it is here (its name on the comm screen,
// the game's own merchant face), else on the emulator's line.
void take_chat(const Chat &c, int seat) {
    std::string who = c.callsign();
    for (int s = 0; s < kMaxSeats && seat < 0; s++) {
        if (g_net->pilots[s].known && g_net->pilots[s].callsign == who) seat = s;
    }
    if (who.empty() && seat >= 0) {
        who = g_net->pilots[seat].callsign;
    }
    wclog(1, "Privateer: %s: %s", who.c_str(), c.message().c_str());
    if (seat >= 0 && seat < kMaxSeats && g_net->puppets[seat].obj && !g_net->puppets[seat].parked && g_net->chats.size() < 8) {
        g_net->chats.push_back(std::make_pair(seat, c.message()));     // (said at the frame)
        return;
    }
    notice(who + ": " + c.message());
}

void frame_chats_and_shots() {
    for (size_t i = 0; i < g_net->chats.size(); i++) {
        const int seat = g_net->chats[i].first;
        Puppet &p = g_net->puppets[seat];
        const std::string &who = g_net->pilots[seat].callsign;
        if (p.obj && !p.parked && priv_ship_say(p.obj, who.c_str(), g_net->chats[i].second.c_str(), g_net->pilots[seat].faceId)) {
            // (the line is played -- its text on the comm line -- only as the pilot's own AI thinks: let it, a while;
            // without this the sets are written and nothing shows)
            priv_let_think(priv_ship_controller(p.obj), kSayThinkFrames);
        } else {
            notice(who + ": " + g_net->chats[i].second);
        }
    }
    g_net->chats.clear();
    const double now = PIC_FullIndex();
    for (size_t i = 0; i < g_net->shots.size(); i++) {
        NpcPuppet *p = npc_of(g_net->shots[i].first, g_net->shots[i].second, false);
        if (p && p->obj && !p->parked && !p->pod) {
            priv_ship_fire(p->obj, true);
            p->fireUntil = now + kFireMs;
        }
    }
    g_net->shots.clear();
}

void take_shot(int seat, Bit32u id) {
    NpcPuppet *p = npc_of(seat, id, false);
    static unsigned shots;
    if (p && p->obj && !p->parked && g_net->shots.size() < 32) {
        g_net->shots.push_back(std::make_pair(seat, id));              // (fired at the frame)
        if (++shots % 20 == 1) {
            wclog(2, "Privateer: pilot %d's %s (ds:%04X) fires here (%u shots of theirs so far)", seat + 1, p->type.c_str(), p->obj, shots);
        }
    }
}

void frame_npcs(double now, const Bit32s pos[3]) {
    int podsShown = 0;
    for (int k = 0; k < kNpcCap; k++) {
        if (g_net->npcs[k].used && g_net->npcs[k].pod && g_net->npcs[k].obj && !g_net->npcs[k].parked) podsShown++;
    }
    // two passes: the encounter ships first, then the pods (the ships' claim on the game's memory comes first)
    for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < kNpcCap; k++) {
        NpcPuppet &p = g_net->npcs[k];
        if (!p.used || (p.pod ? 1 : 0) != pass) continue;
        if (p.obj && p.pod && !(priv_pod_is(p.obj) && priv_scene_has(p.obj))) {
            // a pod gone from our scene: taken by our tractor beam (or let go by the game) while it was here
            if (!p.parked && now - p.at < kNpcStaleMs) {
                send_pod_taken(p.seat, p.id);
                if (g_net->podsGone.size() < 32) g_net->podsGone.push_back(std::make_pair(((Bit32u)p.seat << 16) | p.id, now));
            }
            p.obj = 0;
            p.used = false;
            continue;
        }
        if (p.obj && !p.pod && !priv_ship_alive(p.obj, p.otype)) {
            p.obj = 0;          // (gone from our game: exploded, or the game took it)
            if (p.killed) p.used = false;
        }
        if (p.obj && p.fireUntil > -1e8 && now >= p.fireUntil) {
            priv_ship_fire(p.obj, false);
            p.fireUntil = -1e9;
        }
        const double age = now - p.at;
        const bool fresh = age < kNpcStaleMs && priv_in_flight();
        if (!p.alive && p.obj && !p.killed && !p.pod) {
            priv_ship_kill(p.obj);
            p.killed = true;
            priv_hold_ai(priv_ship_controller(p.obj), false);
            wclog(2, "Privateer: pilot %d's %s (ds:%04X) destroyed, as its owner says", p.seat + 1, p.type.c_str(), p.obj);
            continue;
        }
        if (p.killed) {
            if (!p.obj) p.used = false;
            continue;
        }
        double ex = 0, ey = 0, ez = 0, d = -1;
        if (fresh) {
            const double t = (age < kExtrapolateMs ? age : kExtrapolateMs) / 1000.0;
            ex = p.x + p.vx * t;
            ey = p.y + p.vy * t;
            ez = p.z + p.vz * t;
            d = sqrt((ex - pos[0]) * (ex - pos[0]) + (ey - pos[1]) * (ey - pos[1]) + (ez - pos[2]) * (ez - pos[2])) / kUnitsPerMetre;
        }
        const bool near = fresh && d < (p.obj && !p.parked ? kPuppetGoneMetres : kPuppetMetres);
        Bit32s at[3] = { (Bit32s)ex, (Bit32s)ey, (Bit32s)ez };
        if (near && !p.obj && !p.pending && p.pod && podsShown >= kPodShown) {
            // (a pod beyond the ones shown: its turn comes when one of them goes)
        } else if (near && !p.obj && !p.pending) {
            // a parked ship of the same kind is taken over, else a new one made
            for (int j = 0; j < kNpcCap && !p.obj; j++) {
                NpcPuppet &q = g_net->npcs[j];
                if (j != k && q.used && q.obj && q.parked && q.pod == p.pod && q.type == p.type && q.appearance == p.appearance && q.cast == p.cast && !q.killed) {
                    p.obj = q.obj;
                    p.otype = q.otype;
                    q.obj = 0;
                    q.used = false;
                    if (p.pod) priv_pod_fill(p.obj, cargo_records(p), p.units);
                }
            }
            if (!p.obj && room_for_a_ship(p.pod ? "a cargo pod of another's" : "an encounter ship of another's", p.pod ? kPodNearBytes : kShipNearBytes)) {
                p.pending = priv_spawn_ship(p.type.c_str(), p.appearance.c_str(), p.cast.c_str(), at, p.m, npc_made, k);
            }
        }
        if (near && p.obj && p.pod) {
            priv_pod_set(p.obj, at);
            p.parked = false;
        } else if (near && p.obj) {
            priv_ship_set(p.obj, at, p.m);
            priv_ship_set_armor(p.obj, p.armor);
            p.parked = false;
        } else if (p.obj && !p.parked) {
            Bit32s away[3];
            park_pos(p.obj, away);
            if (p.pod) priv_pod_set(p.obj, away); else priv_ship_set(p.obj, away, NULL);
            p.parked = true;
            wclog(2, "Privateer: pilot %d's %s (ds:%04X) parked (%s)", p.seat + 1, p.type.c_str(), p.obj, fresh ? "out of reach" : "no reports");
        }
        if (!fresh && !p.obj) {
            p.used = false;
        }
    }
}

}  // namespace

// The game's own wing first: it spawns as it would (the game asks once: refused, it would be lost), and when our
// puppets squeeze the heap (the last 2 s' low under kWingFloor) some of ours are given back at once to make room
// (one each kWingFloor - low / 700 bytes, at the next frames)
void make_room_for_wing(int sphereId) {
    int pilots, npcs, pods;
    priv_session_counts(pilots, npcs, pods);
    const int low = heap_low_recent(2);
    if (pilots + npcs == 0 || low >= (1 << 30) || low >= kWingFloor) {
        return;
    }
    const int n = (kWingFloor - low) / 700 + 1;
    for (int i = 0; i < n; i++) priv_session_force_release();
    wclog(1, "Privateer: nav %d's wing comes while the game's memory is low (%d at the lowest): %d of ours given back", sphereId, low, n);
}

bool somebody_else_in_system() {
    for (int s = 0; s < kMaxSeats; s++) {
        if (s != g_net->self && g_net->pilots[s].known && g_net->pilots[s].flying && g_net->pilots[s].system == g_system) return true;
    }
    return false;
}

bool priv_session_allow_wing(int sphereId) {
    if (!g_net || g_net->self < 0 || g_system.empty()) {
        return true;
    }
    const double now = PIC_FullIndex();
    if (g_net->nav != sphereId) {
        // (the game spawns this sphere's wing as we enter it: before our tick has seen us there)
        g_net->nav = sphereId;
        g_net->navSince = now;
        priv_session_event(WHY_NAV);
    }
    // The game asks once, as we enter the sphere (a refusal loses the wing for this visit: measured), so the owner is
    // chosen now: the host's word when there is one, else by the same rule every machine follows, with what we know of
    // who is here (their presences, and their latest places: pilots arriving together)
    int owner = owner_of(g_system, sphereId);
    if (owner < 0) {
        NavOwner *o = owner_entry(g_system, sphereId, true);
        unsigned done = o ? o->done : 0;
        const unsigned present = present_at(g_system, sphereId) | (1u << g_net->self);
        owner = choose_owner(present, sphereId, done, NULL);
        if (o && owner >= 0) {
            o->owner = owner;
            o->occupied = true;
            o->done = done | (1u << owner);
            wclog(1, "Privateer: %s nav %d: pilot %d owns it by the rule, no word yet (present %02x)", g_system.c_str(), sphereId, owner + 1,
                  present);
            if (g_net->host) send_owners();
        }
    }
    (void)now;
    if (owner == g_net->self) {
        make_room_for_wing(sphereId);   // (never refused: the game's own encounter first; ours give way)
        return true;
    }
    if (owner >= 0) {
        static double saidAt = -1e9;
        if (PIC_FullIndex() - saidAt > 5000) {
            saidAt = PIC_FullIndex();
            wclog(2, "Privateer: nav %d's wings are pilot %d's to spawn, not ours", sphereId, owner + 1);
        }
        return false;
    }
    return true;        // (no owner could be chosen: as alone)
}

bool priv_session_chat(const std::string &text) {
    if (!g_net || g_net->self < 0) {
        return false;
    }
    NetworkMessage msg;
    msg.mutable_chat()->set_callsign(g_net->callsign);
    msg.mutable_chat()->set_message(text);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
    wclog(1, "Privateer: said \"%s\"", text.c_str());
    return true;
}

bool priv_session_is_puppet(Bit16u a, Bit16u b) {
    return g_net && g_net->self >= 0 && ((a && is_puppet_obj(a)) || (b && is_puppet_obj(b)));
}

void priv_session_hit(Bit16u target, int loc, int damage) {
    if (!g_net || g_net->self < 0 || damage <= 0) {
        return;
    }
    int owner = -1;
    Bit32u id = 0;
    for (int s = 0; s < kMaxSeats && owner < 0; s++) {
        if (g_net->puppets[s].obj == target && target) {
            if (g_net->puppets[s].parked) return;      // (far off: no hit there is real)
            owner = s;
        }
    }
    for (int k = 0; k < kNpcCap && owner < 0; k++) {
        if (g_net->npcs[k].used && g_net->npcs[k].obj == target && target) {
            if (g_net->npcs[k].parked) return;
            owner = g_net->npcs[k].seat;
            id = g_net->npcs[k].id;
        }
    }
    if (owner < 0) {
        return;
    }
    NetworkMessage msg;
    PrivHit *h = msg.mutable_priv_hit();
    h->set_player((Bit32u)g_net->self);
    h->set_owner((Bit32u)owner);
    h->set_id(id);
    h->set_loc(loc);
    h->set_damage(damage);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (p->hello && p->conn.is_open() && (!g_net->host || p->player == owner)) {
            p->conn.send(msg);
        }
    }
    wclog(2, "Privateer: our hit on pilot %d's %s (ds:%04X): quadrant %d, %d, sent", owner + 1, id ? "encounter ship" : "ship", target, loc, damage);
}

void priv_session_fired(Bit16u ship) {
    if (!g_net || g_net->self < 0 || !ship || ship == priv_player_ship() || is_puppet_obj(ship)) {
        return;
    }
    if (!somebody_within(kStateMetres)) {
        return;
    }
    const double now = PIC_FullIndex();
    NpcTrack *t = track_of(ship, true);
    if (t && now - t->shotAt < kShotEveryMs) {
        return;
    }
    if (t) t->shotAt = now;
    NetworkMessage msg;
    msg.mutable_priv_shot()->set_player((Bit32u)g_net->self);
    msg.mutable_priv_shot()->set_id(ship);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send_best_effort(msg);
        }
    }
}

// One puppet given back: moved far off, its AI let go, killed (the game frees it after its death)
void release_obj(Bit16u obj) {
    Bit32s away[3];
    park_pos(obj, away);
    if (priv_pod_is(obj)) {
        priv_pod_set(obj, away);
    } else {
        priv_ship_set(obj, away, NULL);
        priv_hold_ai(priv_ship_controller(obj), false);
    }
    priv_ship_kill(obj);
}

int g_forceRelease;                 // (a test aid's: !privrelease gives one back at the next frame)

void govern_memory(double now, const Bit32s pos[3]) {
    heap_windows_frame(now);
    for (size_t i = 0; i < g_net->dying.size(); ) {
        if (priv_ship_alive(g_net->dying[i].first, g_net->dying[i].second)) i++;
        else g_net->dying.erase(g_net->dying.begin() + i);     // (freed by the game)
    }
    int low = g_heap.lowSinceRelease;
    if (g_forceRelease > 0) {
        g_forceRelease--;
        low = 0;
    } else if (low >= (1 << 30) || low >= kReleaseFloor || now - g_heap.releasedAt < (low < kUrgentFloor ? kUrgentEveryMs : kReleaseEveryMs)) {
        return;
    }
    // the one to give back: parked first, then encounter ships and pods, then pilots; of a kind the farthest
    double best = -1;
    int bestSeat = -1, bestNpc = -1;
    for (int k = 0; k < kNpcCap; k++) {
        const NpcPuppet &p = g_net->npcs[k];
        if (!p.used || !p.obj || p.killed || p.pod) continue;     // (a pod is not freed by its death: it waits out its time)
        double d = sqrt((p.x - pos[0]) * (p.x - pos[0]) + (p.y - pos[1]) * (p.y - pos[1]) + (p.z - pos[2]) * (p.z - pos[2]));
        const double score = (p.parked ? 3e12 : 1e12) + d;      // (encounter ships last: an enemy not shown still shoots)
        if (score > best) { best = score; bestNpc = k; bestSeat = -1; }
    }
    for (int s = 0; s < kMaxSeats; s++) {
        const Puppet &p = g_net->puppets[s];
        if (!p.obj) continue;
        double d = sqrt((p.x - pos[0]) * (p.x - pos[0]) + (p.y - pos[1]) * (p.y - pos[1]) + (p.z - pos[2]) * (p.z - pos[2]));
        const double score = (p.parked ? 3e12 : 2e12) + d;
        if (score > best) { best = score; bestSeat = s; bestNpc = -1; }
    }
    if (bestNpc < 0 && bestSeat < 0) {
        return;                 // (nothing of ours left: the game's own needs)
    }
    g_heap.releasedAt = now;
    g_heap.released++;
    g_heap.lowSinceRelease = 1 << 30;
    if (bestNpc >= 0) {
        NpcPuppet &p = g_net->npcs[bestNpc];
        wclog(1, "Privateer: the game's memory low (%d free at the lowest): pilot %d's %s (ds:%04X) given back", low, p.seat + 1,
              p.pod ? "cargo pod" : p.type.c_str(), p.obj);
        release_obj(p.obj);
        p.killed = true;        // (its record goes when the game has freed it; made again when there is room)
    } else {
        Puppet &p = g_net->puppets[bestSeat];
        wclog(1, "Privateer: the game's memory low (%d free at the lowest): pilot %d's ship (ds:%04X) given back", low, bestSeat + 1, p.obj);
        if (g_net->dying.size() < 32) g_net->dying.push_back(std::make_pair(p.obj, p.type));
        release_obj(p.obj);
        p.obj = 0;              // (made again when there is room)
        p.pending = false;
        p.parked = false;
    }
}

void priv_session_frame() {
    if (!g_net || g_net->self < 0 || !priv_in_flight()) {
        return;
    }
    const double now = PIC_FullIndex();
    // a new flight (a launch, a jump, a load): the old scene's ships are gone
    const Bit16u ship = priv_player_ship();
    if (ship != g_net->flightShip) {
        forget_puppets("a new flight");
        g_net->flightShip = ship;
        g_net->lastAt = -1e9;
    }
    Bit32s pos[3];
    priv_player_pos(pos);
    if (now > g_net->lastAt && now - g_net->lastAt < 1000) {
        const double dt = (now - g_net->lastAt) / 1000.0;
        g_net->vx = (pos[0] - g_net->lastX) / dt;
        g_net->vy = (pos[1] - g_net->lastY) / dt;
        g_net->vz = (pos[2] - g_net->lastZ) / dt;
    }
    g_net->lastX = pos[0];
    g_net->lastY = pos[1];
    g_net->lastZ = pos[2];
    g_net->lastAt = now;
    // ours, while somebody shares our system near enough
    bool wanted = false;
    for (int s = 0; s < kMaxSeats && !wanted; s++) {
        const double d = s == g_net->self ? -1 : distance_to(g_net->pilots[s]);
        wanted = d >= 0 && d < kStateMetres;
    }
    if (wanted && now - g_net->stateAt >= 1000.0 / kStatesPerSecond) {
        send_state(now);
    }
    // the encounter ships of a nav point we own, and our cargo pods, to the others near
    if (wanted && now - g_net->sceneAt >= kSceneEveryMs) {
        send_scene(now, g_net->nav >= 0 && owner_of(g_system, g_net->nav) == g_net->self);
    }
    govern_memory(now, pos);
    // the encounter ships first (the first claim on the game's memory: co-op, an enemy not shown still shoots), then
    // the other pilots
    frame_npcs(now, pos);
    for (int s = 0; s < kMaxSeats; s++) {
        if (s == g_net->self) {
            continue;
        }
        Puppet &p = g_net->puppets[s];
        if (p.obj && !priv_ship_alive(p.obj, p.type)) {
            wclog(1, "Privateer: pilot %d's ship is gone from our game (destroyed?)", s + 1);
            priv_hold_ai(priv_ship_controller(p.obj), false);
            p.obj = 0;
        }
        const double age = now - p.at;
        const bool fresh = p.reported && age < kStateStaleMs && p.system == g_system && !g_system.empty();
        double ex = 0, ey = 0, ez = 0, d = -1;
        if (fresh) {
            const double t = (age < kExtrapolateMs ? age : kExtrapolateMs) / 1000.0;
            ex = p.x + p.vx * t;
            ey = p.y + p.vy * t;
            ez = p.z + p.vz * t;
            d = sqrt((ex - pos[0]) * (ex - pos[0]) + (ey - pos[1]) * (ey - pos[1]) + (ez - pos[2]) * (ez - pos[2])) / kUnitsPerMetre;
        }
        const bool near = fresh && d < (p.obj && !p.parked ? kPuppetGoneMetres : kPuppetMetres);
        Bit32s at[3] = { (Bit32s)ex, (Bit32s)ey, (Bit32s)ez };
        if (near && !p.obj && !p.pending && room_for_a_ship("a pilot's ship")) {
            p.pending = priv_spawn_ship(type_of(p.appearance), p.appearance.empty() ? "CLUNKER" : p.appearance.c_str(),
                                        g_net->pilots[s].cast.c_str(), at, p.m, puppet_made, s);
        } else if (near && p.obj) {
            priv_ship_set(p.obj, at, p.m);
            if (priv_thinking(priv_ship_controller(p.obj))) priv_ship_fire(p.obj, false);
            p.parked = false;
        } else if (p.obj && !p.parked) {
            // out of reach: far off, kept for its return
            Bit32s away[3];
            park_pos(p.obj, away);
            priv_ship_set(p.obj, away, NULL);
            p.parked = true;
            wclog(2, "Privateer: pilot %d's ship parked (%s)", s + 1, fresh ? "out of reach" : "no reports");
        }
    }
    frame_chats_and_shots();
    drones_frame(now);
}

void priv_session_force_release() { g_forceRelease++; }

// A pilot's ship in our game: ours, or its puppet (0 none here)
Bit16u priv_session_pilot_ship(int seat) {
    if (!g_net || seat < 0 || seat >= kMaxSeats) return 0;
    if (seat == g_net->self) return priv_player_ship();
    const Puppet &p = g_net->puppets[seat];
    return p.obj && !p.parked ? p.obj : 0;
}

// Co-op (the user: "This is mostly coop not PvP and if no one can spawn when players are present then it may be
// boring"): a hostile wing the game spawns where other pilots are present gets more ships of its own, made by the game
// itself (wcnet_priv.cpp, wing_extend: a larger ship array, a higher count, its orders and formation): +1 for 2-3
// pilots, +2 for 4-5, +3 for 6-8, at most that many a nav point a flight, and only while the owner's heap can take
// some 1.2 KB each over the create floor (measured, docs section 15: 8 pilots together and the natural encounter + 3
// is safe, + 5 the edge).
enum { kExtrasMax = 3, kExtraNearBytes = 1200 };
const double kComingMetres = 20000.0;

bool hostile_cast(const std::string &cast) {
    return cast.compare(0, 3, "PIR") == 0 || cast.compare(0, 3, "KIL") == 0 || cast.compare(0, 3, "RET") == 0 || cast.compare(0, 3, "BOU") == 0;
}

int extras_for(int pilots) {
    return pilots <= 1 ? 0 : (pilots / 2 < kExtrasMax ? pilots / 2 : kExtrasMax);
}

int priv_session_wing_extras(int sphereId, const char *cast, int count) {
    if (!g_net || g_net->self < 0 || !priv_in_flight() || !hostile_cast(cast)) {
        return 0;
    }
    // the pilots here or coming: present, or within 20 km of the sphere's edge by their latest places (a group flying
    // together arrives seconds apart, and the game spawns the encounter as the first one enters)
    unsigned present = present_at(g_system, sphereId) | (1u << g_net->self);
    PrivSphere sp[16];
    const int ns = priv_spheres(sp, 16);
    const double now = PIC_FullIndex();
    for (int i = 0; i < ns; i++) {
        if (sp[i].id != sphereId) continue;
        const double reach = ((double)sp[i].radius + kComingMetres) * kUnitsPerMetre;
        for (int s = 0; s < kMaxSeats; s++) {
            const Puppet &p = g_net->puppets[s];
            if (s == g_net->self || !p.reported || now - p.at > 2000 || p.system != g_system) continue;
            const double dx = p.x - sp[i].x, dy = p.y - sp[i].y, dz = p.z - sp[i].z;
            if (dx * dx + dy * dy + dz * dz <= reach * reach) present |= 1u << s;
        }
    }
    int pilots = 0;
    for (int s = 0; s < kMaxSeats; s++) {
        if (present & (1u << s)) pilots++;
    }
    if (g_net->extrasSystem != g_system || g_net->extrasNav != sphereId || g_net->extrasFlight != g_net->flightShip) {
        g_net->extrasSystem = g_system;
        g_net->extrasNav = sphereId;
        g_net->extrasFlight = g_net->flightShip;
        g_net->extrasMade = 0;
    }
    int n = extras_for(pilots) - g_net->extrasMade;
    if (n <= 0) {
        return 0;
    }
    const int low = heap_low_recent(2);
    const int afford = low >= (1 << 30) ? n : (low - kCreateFloor) / kExtraNearBytes;
    if (afford < n) {
        wclog(1, "Privateer: co-op: %d pilots at nav %d; the game's memory (%d at the lowest) takes %d more ships of %d", pilots, sphereId, low,
              afford < 0 ? 0 : afford, n);
        n = afford;
    }
    (void)count;
    return n > 0 ? n : 0;
}

void priv_session_wing_extended(int sphereId, int extras) {
    if (!g_net) return;
    g_net->extrasMade += extras;
    (void)sphereId;
}

void priv_session_our_death() {
    if (!g_net || g_net->self < 0) {
        return;
    }
    NetworkMessage msg;
    PrivState *st = msg.mutable_priv_state();
    st->set_player((Bit32u)g_net->self);
    st->set_frame(++g_net->frame);
    st->set_system(g_system);
    st->set_x((Bit32s)g_net->lastX);
    st->set_y((Bit32s)g_net->lastY);
    st->set_z((Bit32s)g_net->lastZ);
    st->set_destroyed(true);
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
    wclog(1, "Privateer: our ship's destruction told to the room");
}

void priv_session_counts(int &pilots, int &npcs, int &pods) {
    pilots = npcs = pods = 0;
    if (!g_net) {
        return;
    }
    for (int s = 0; s < kMaxSeats; s++) {
        if (g_net->puppets[s].obj) pilots++;
    }
    for (int k = 0; k < kNpcCap; k++) {
        if (g_net->npcs[k].used && g_net->npcs[k].obj) {
            if (g_net->npcs[k].pod) pods++; else npcs++;
        }
    }
}

bool priv_session_active() {
    return g_net != NULL && (g_net->host || g_net->self >= 0);
}

}  // namespace wc
