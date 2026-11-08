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
#include <math.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <string>
#include <vector>
#include "wcnet_tie_session.h"
#include "keyboard.h"
#include "mouse.h"
#include "wcnet_tie.h"
#include "wcnet_transport.h"
#include "wcnet_lobby.h"
#include "wcnet_session.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "net_config.h"
#include "pic.h"
#include "mem.h"
#include "regs.h"

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
    double prevAt;              // the report before it
    // Far from this machine's pilot a puppet is flown by this machine's AI (the user, 2026-11-06: "have the AIs control
    // them and let them diverge a bit, but smoothly snap to the right place over ten+ seconds between updates"), its
    // shots made here for show; its owner's reports pull its place over (the error left to blend in, and the ticks).
    bool localAi;
    double ex, ey, ez;
    double blendTicks;
    double localHitAt;          // a hit of ours shown on it here (its owner's next reports do not know it yet)
};

// Where each pilot of the room is (its TiePresence), by seat.
struct Pilot {
    bool known;
    std::string callsign;
    int scene;
    bool flying;
    std::string mission;
    bool challenge = false;     // its mission flown as the Z-95 challenge (step 14)
    Pilot() : known(false), scene(-1), flying(false) {}
};
enum { kMaxSeats = 8, kPresenceEveryMs = 3000, kCallToActionEveryMs = 2000, kPaEveryMs = 20000 };

// Step 5 (docs/tiefighter.md): who flies each craft, and how often its owner reports it.  Distances in world units
// (40,960 a km).
enum {
    kCraftSlots = 32,
    kReach = 102400,            // 2.5 km, the lasers' reach: within it a craft is near
    kTargetReach = 163840,      // 4 km: a pilot's target within it is that pilot's to fly
    kMidReach = 409600,         // 10 km: a craft within it is reported every kMidEvery frames
    kMidEvery = 10,
    kFarEvery = 30,             // one far craft every this many frames, in turn: 30 far craft, each every 15 s
    kOwnerHoldMs = 1000,        // a craft changes owner at most this often
    kOwnersEveryMs = 2000,      // the host's whole table, this often
    kDecideEveryMs = 250,
    kPilotStaleMs = 3000,       // a pilot whose frames stopped this long is not counted
    // Far craft (the user's design): this machine's AI flies another machine's craft beyond kLocalAiOut from our pilot,
    // and holds it again within kLocalAiIn (there the owner's shots come over).
    kLocalAiIn = 204800,        // 5 km
    kLocalAiOut = 225280,       // 5.5 km
    kExactError = 400,          // a near craft's report this close is written as it is (10 m)
    kPairOffset = 64,
    kZ95 = 14,                  // the Z-95 Headhunter's craft type
    kPilotRecord = 0xFABC8,     // the pilot record in memory: +0x66 the Combat Chamber's craft row, +0x67 each row's mission
    kRadioMsg = 0xE2F24,        // the mission's radio messages, 90 bytes each: the text (64), two triggers (+64, +68), the delay (+88), and/or (+89)
    kVoiceA = 0xD4190,          // the radio voices' bank: the Combat Chamber's craft row (an index of "FIBAGDM"), at each launch
    kVoiceB = 0xD418E,          // from the pilot record, and the row's mission (VOICE\<letter>M<mission + 1>)
    kSpeciesConvertImage = 0xC1834,
    kSpeciesTableImage = 0xCBBC4,
    // The room's end of a flight (the user, 2026-11-06: "it just delivers you to the debrief room with mission failed and
    // you can quickly try again with your friends"): RUNTIME_MissionState mission, +0x248 the player's status (0 dead,
    // 1 captured, 2 rescued, 3 the mission ended: what create_createmission sets and the game's own quit), +0x249 the
    // end flag the flight's loop runs until.
    kMission = 0xEB29C,
    kMissionStatus = 0x248,
    kMissionEnd = 0x249,
    kRoomEndMs = 3000,          // (a moment to see the explosion)           // a pairing's second flight group starts and flies 0.4 km to the side (waypoints: 160 a km)
    kFgStatus = 0xDEDCC,        // FGStatus fgstatus[48], 0x30 each: +6 its place's reference
    kFgStatusSize = 0x30,
    kLocalHitHoldMs = 600,      // a hit of ours shown on another's craft: its owner's damage figures wait this long
};
const double kCorrectSpeed = 12288.0;       // a place pulled to its owner's at most this fast, units a second (300 m/s)
const double kFarBlendS = 10.0;             // a craft our AI flies: its first report blended in over this many seconds,
const double kFarBlendMinS = 1.0;           // ... the next ones over the time since the one before, at least this
const double kFarBlendMaxS = 15.0;          // ... and at most this
const double kTicksPerSecond = 236.0;       // (the game's clock: frameticks)

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
    double endAt = 0;           // this machine's flight ends for the room then (0: not)
    // the Z-95 challenge (step 14): this flight's mission flown as it, its source's file; the presence's last word of it
    bool challenge = false, sentChallenge = false;
    std::string challengeFile;
    // a guest's launch waiting for the host (step 13): waiting, Esc said to fly alone, the room's last mission (a launch
    // of it again, Fly Mission Again, waits too), the notice's last time
    bool waiting = false, waitCancelled = false;
    // a guest's launch click held while the host was not flying, given to the game once the host flies (its scene
    // and the pointer's place; scene -1: none) and its button let go after
    int heldScene = -1, heldX = 0, heldY = 0;
    double heldReleaseAt = -1;
    std::string roomMission;
    double waitSaidAt = -1e9;
    int pairedFg = -1;          // this mission's second flight group, the guests' (step 9), or -1
    int pairedType = 0;         // ... the pairing's craft (the guests'), 0: none
    int hostFg = -1;            // ... and the host's, the mission's player group
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
    // Step 5: the owner of each craft slot (a seat; the host's word), when it last changed, each pilot's own craft and
    // target and when its frames were last seen (this mission's), the far craft's turn, and what was sent (the report).
    int owner[kCraftSlots];
    double ownerAt[kCraftSlots];
    int pilotObj[kMaxSeats];
    int pilotTarget[kMaxSeats];
    double pilotSeenAt[kMaxSeats];
    double decidedAt, ownersSentAt;
    int farCursor;
    unsigned sentCraft, sentNear, sentMid, sentFar, sentBytes, ownerChanges;
    // Step 6: shots and hits for the next frame, hits taken in for this game to apply, deaths being applied.
    std::vector<TieShot> shotsOut;
    std::vector<TieHit> hitsOut;
    std::vector<TieEvent> eventsOut;    // the mission's events of ours to send (watch_events)
    bool reinforcedKnown = false;       // reinforcements called this flight, here or by another pilot
    bool inspectKnown[kCraftSlots];     // each craft slot's last-seen state: there, its idnumber, inspected
    Bit16u inspectId[kCraftSlots];
    Bit8u inspectWas[kCraftSlots];
    TieHit hitsIn[64];
    int hitsInNext;
    int killing[kCraftSlots];   // calls made to kill a copy whose owner reported it dying (0: none)
    int killer[kCraftSlots];    // a craft of ours: the shooter whose hit, applied here, began its death, + 1 (0: none)
    unsigned shotsSent, shotsMade, hitsSent, hitsApplied, damageSkipped, kills, credited;
    // far craft and hits shown here (the report)
    unsigned localAiNow = 0, localAiStarts = 0, localAiEnds = 0, localShots = 0, farReports = 0, nearBlends = 0, hitsShown = 0;
    double farErrSum = 0, farErrMax = 0;
    TieNet() : host(false), self(-1), seats(2), listener(NULL), frame(0), lastReliableAt(0), playerFg(kPlayerFgNone),
               hostCraft(1), wasFlying(false), sentScene(-2), sentFlying(false), sentAt(-1e9), decidedAt(-1e9), ownersSentAt(-1e9),
               farCursor(0), sentCraft(0), sentNear(0), sentMid(0), sentFar(0), sentBytes(0), ownerChanges(0), hitsInNext(0),
               shotsSent(0), shotsMade(0), hitsSent(0), hitsApplied(0), damageSkipped(0), kills(0), credited(0) {
        clear_flight();
    }
    // A puppet table and owners for a new flight (a Puppet holds a protobuf message: never memset).
    void clear_flight() {
        for (int o = 0; o < 120; o++) {
            puppets[o].used = false;
            puppets[o].owner = -1;
            puppets[o].fresh = false;
            puppets[o].state.Clear();
            puppets[o].at = 0;
            puppets[o].prevAt = 0;
            puppets[o].localAi = false;
            puppets[o].ex = puppets[o].ey = puppets[o].ez = 0;
            puppets[o].blendTicks = 0;
            puppets[o].localHitAt = -1e9;
        }
        for (int o = 0; o < kCraftSlots; o++) {
            owner[o] = 0;
            ownerAt[o] = -1e9;
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
        reinforcedKnown = false;
        for (int o = 0; o < kCraftSlots; o++) {
            inspectKnown[o] = false;
        }
        for (int o = 0; o < kCraftSlots; o++) {
            killing[o] = 0;
            killer[o] = 0;
        }
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

void take_owners(const TieOwners &t);
void begin_kill(int obj, int killer);
void room_end(int seat, const char *what);
void make_shot(const TieShot &shot, int owner);
void take_hit(const TieHit &hit);

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
void take_event(const TieEvent &e, int from);   // (below: the game's functions' addresses)

void take_frame(Peer *from, const TieFrame &f) {
    const int owner = g_net->host ? from->player : (f.has_player() ? (int)f.player() : 0);
    if (owner == g_net->self) {
        return;
    }
    from->lastFrame = f.frame();
    from->lastFrameAt = PIC_FullIndex();
    // (the host's flight over: every guest's ends too)
    const bool hostEnded = owner == 0 && !g_net->host && from->flying && !f.flying() && from->player == 0;
    if (!g_net->host || owner == from->player) {
        from->flying = f.flying();
    }
    if (hostEnded) {
        room_end(0, "'s flight is over");
    }
    // (a pilot flying another mission: its craft are not this mission's; the host still relays them)
    const bool sameMission = !f.has_mission() || !tie_in_flight() || f.mission() == tie_mission_name();
    if (sameMission && f.flying() && owner >= 0 && owner < kMaxSeats && f.has_own() && f.own() < (Bit32u)kCraftSlots) {
        g_net->pilotObj[owner] = (int)f.own();
        g_net->pilotTarget[owner] = f.has_target() ? (int)f.target() : 0xFFFF;
        g_net->pilotSeenAt[owner] = PIC_FullIndex();
        g_net->owner[f.own()] = owner;      // (a pilot's own craft is its own)
    }
    for (int i = 0; i < f.craft_size() && sameMission; i++) {
        const TieCraft &c = f.craft(i);
        if (c.obj() >= 120) {
            continue;
        }
        if (c.obj() < (Bit32u)kCraftSlots && g_net->owner[c.obj()] != owner && !(f.has_own() && c.obj() == f.own())) {
            continue;   // (not its craft by the owners we have: a report of a former owner's, or ahead of the host's word)
        }
        Puppet &p = g_net->puppets[c.obj()];
        p.used = true;
        p.owner = owner;
        p.fresh = true;
        p.state = c;
        p.prevAt = p.at;
        p.at = PIC_FullIndex();
        if (c.dying() && c.obj() < (Bit32u)kCraftSlots && tie_in_flight()) {
            begin_kill((int)c.obj(), c.has_killer() ? (int)c.killer() : -1);
            if (f.has_own() && c.obj() == f.own()) {
                room_end(owner, " is down");
            }
        }
    }
    for (int i = 0; i < f.shots_size() && sameMission && tie_in_flight(); i++) {
        make_shot(f.shots(i), owner);
    }
    for (int i = 0; i < f.hits_size() && sameMission && tie_in_flight(); i++) {
        take_hit(f.hits(i));
    }
    for (int i = 0; i < f.events_size() && sameMission && tie_in_flight(); i++) {
        take_event(f.events(i), owner);
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

// The room's mission (docs/tiefighter.md, step 8): the file the page's menu picked for everybody, flown whenever a pilot
// launches a Combat Chamber mission; none, the Combat Chamber's own.  WCNET_TIE_MISSION at the launch,
// wc_web_tie_mission while the game runs (the host may change it between flights; its guests' pages follow).
std::string g_pick;
bool g_pickRead;

// A mission file's name as the front end keeps it: the Combat Chamber's (h, the craft's letter, its number, w or m)
// or a battle's (b<n>m<m>, the craft's letter, w or m), lower case.
bool mission_file_name(const std::string &m) {
    if (m.size() < 4 || m.size() > 13 || (m[0] != 'h' && m[0] != 'b')) {
        return false;
    }
    for (size_t i = 0; i < m.size(); i++) {
        if (!islower((unsigned char)m[i]) && !isdigit((unsigned char)m[i])) {
            return false;
        }
    }
    return true;
}

const std::string &picked_mission() {
    if (!g_pickRead) {
        g_pickRead = true;
        const char *env = getenv("WCNET_TIE_MISSION");
        if (env && env[0]) {
            tie_session_set_mission(env);
        }
    }
    return g_pick;
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
    p.challenge = pr.challenge();
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
    if (scene == g_net->sentScene && flying == g_net->sentFlying && mission == g_net->sentMission && g_net->challenge == g_net->sentChallenge &&
        now - g_net->sentAt < kPresenceEveryMs) {
        return;
    }
    g_net->sentChallenge = g_net->challenge;
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
    pr->set_challenge(g_net->challenge);
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
        line = pilot_name(0) + (h.flying ? " is flying " : " is at the briefing for ") + mission_words(h.mission) + ": launch a Combat Chamber mission to fly it together";
    }
    // (every kPaEveryMs while the guest is not at a briefing: the ship's address system, RetroVega's call, with its line)
    const int mine = tie_scene();
    const bool atBriefing = (mine >= 133 && mine <= 137) || (mine >= 179 && mine <= 183);
    static double paAt = -1e9;
    if (!line.empty() && !atBriefing && now - paAt >= kPaEveryMs) {
        paAt = now;
        tie_pa_play();
        line = "PA: All pilots, report to the briefing room. " + line;
        g_net->callToAction.clear();    // (said now, with the PA's words)
    } else if (!line.empty() && g_net->callToAction.compare(0, 4, "PA: ") == 0 && now - paAt < 6000) {
        line = g_net->callToAction;     // (the PA's words stay up while it speaks)
    }
    if (line.empty()) {
        paAt = -1e9;                    // (the next call comes at once)
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
    } else if (msg.has_tie_owners()) {
        if (p->hello && !g_net->host) {
            take_owners(msg.tie_owners());
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

// The craft slots' state as this machine has them.
Bit32u obj_base(int obj) { return tie::kObjects + obj * tie::kObjectSize; }
bool is_craft(int obj) {
    const Bit32u o = obj_base(obj);
    return obj >= 0 && obj < kCraftSlots && tie_r8(o + 4) != 0 && tie_r8(o + 2) == 0;     // (a ship, not debris: category 0)
}
double distance(int a, int b) {
    const Bit32u oa = obj_base(a), ob = obj_base(b);
    const double dx = (double)(Bit32s)tie_r32(oa + 6) - (double)(Bit32s)tie_r32(ob + 6);
    const double dy = (double)(Bit32s)tie_r32(oa + 0xA) - (double)(Bit32s)tie_r32(ob + 0xA);
    const double dz = (double)(Bit32s)tie_r32(oa + 0xE) - (double)(Bit32s)tie_r32(ob + 0xE);
    return sqrt(dx * dx + dy * dy + dz * dz);
}
// The pilots flying this mission now (their frames seen lately), ourselves among them.
bool pilot_present(int seat, double now) {
    if (seat == g_net->self) {
        return tie_player_object() >= 0;
    }
    return seat >= 0 && seat < kMaxSeats && g_net->pilotObj[seat] >= 0 && now - g_net->pilotSeenAt[seat] < kPilotStaleMs;
}
int pilot_object(int seat) { return seat == g_net->self ? tie_player_object() : g_net->pilotObj[seat]; }

// Who flies a craft for this machine: the table's word, but a craft whose owner is not in this flight (gone, landed,
// another mission) is ours: a guest whose host stopped flying flies the rest alone.
int owner_of(int obj) {
    if (obj < 0 || obj >= kCraftSlots) {
        return g_net->self;
    }
    const int o = g_net->owner[obj];
    return o == g_net->self || pilot_present(o, PIC_FullIndex()) ? o : g_net->self;
}
bool mine(int obj) { return owner_of(obj) == g_net->self || obj == tie_player_object(); }

// A craft this machine is to fly from now on: its AI to plan at once (the countdown held full while another flew it).
void release_ai(int obj) {
    const Bit32u craft = tie_r32(obj_base(obj) + 0x54);
    if (obj < kCraftSlots && craft >= 0x100000) {
        mem_writew(craft + 0x2A, 0);
    }
    g_net->puppets[obj].used = false;
}

// --- the guns (step 6) ---------------------------------------------------------------------------------------------

enum { kShotReach = 2 * kReach, kKillCalls = 12 };
// The collision test's bolt segment (tie.c's laserx.. and laserxold..), which collide_laserhitcraft reads.
enum { kLaserX = 0xEAB94, kLaserY = 0xEAB98, kLaserZ = 0xEAB90, kLaserXOld = 0xEABD4, kLaserYOld = 0xEABD8, kLaserZOld = 0xEABD0 };
// The impact's place along the bolt's segment, from its start (laserxold..), as the collision test leaves it for
// collide_laserhitcraft, which puts the bolt's flash there (OpenTIE's tie.c: collidexoff 0xEAB7C, y 0xEAB78, z 0xEAB74).
enum { kCollideXOff = 0xEAB7C, kCollideYOff = 0xEAB78, kCollideZOff = 0xEAB74 };
enum { kFnCreateProjectile = 0x2E0E4, kFnLaserHit = 0x1433C, kFnDamage = 0x148F0, kFnUpdateKills = 0x16218, kWarheads = 0xE61FC };

// -- the mission's events a pilot causes on its own machine (the user, 2026-11-07: "If it fails to show up for a player
// that's a serious bug") ---------------------------------------------------------------------------------------------
// The mission's triggers run on every machine alike, and most of what they read is the same everywhere (the time, a
// craft's arrival or death: deaths are the owners' and sent).  Two things a pilot does are kept by its machine alone:
// reinforcements called (S, then Space: the mission's penalty flag, +0x2E0, which a "reinforced" arrival waits for;
// B5M1's frigate Osprey came only for the pilot who called) and a craft inspected (COLLIDE_collisions: the pilot's
// target near enough, the craft's +0xB8 set, its flight group's inspected count, +0x11 of its FGStatus, and the
// special craft's, +0x23; goals and arrivals read them).  Each is watched here every frame and sent; the others do the
// game's own bookkeeping and show its own message.
enum {
    kPenaltyFlag = 0x2E0,           // (of the mission's state, kMission)
    kInspected = 0xB8,              // (of a craft's CraftData)
    kCraftIndexInFg = 0x90,
    kFgSpecialCraft = 0x30,         // (of a flight group's record)
    kFgInspectedCount = 0x08 + 9,   // FGStatus counts[FG_COUNT_INSPECTED]
    kFgInspectedSpecial = 0x1A + 9, // ... special_counts[FG_COUNT_INSPECTED]
    kFnCraftMessage = 0x33D10,      // msg_craftmessage(obj, craft, template): 0xAB "has been inspected"
    kMsgInspected = 0xAB,
};

void watch_events() {
    const Bit32u m = tie_linear(kMission);
    if (!g_net->reinforcedKnown && mem_readb(m + kPenaltyFlag)) {
        g_net->reinforcedKnown = true;
        TieEvent e;
        e.set_kind(1);
        g_net->eventsOut.push_back(e);
        wclog(1, "TIE: we called for reinforcements: sent");
    }
    for (int o = 0; o < kCraftSlots; o++) {
        const Bit32u b = tie_linear(tie::kObjects + (Bit32u)o * tie::kObjectSize);
        const bool there = mem_readb(b + 4) != 0;
        const Bit16u id = mem_readw(b);
        const Bit32u craft = mem_readd(b + 0x54);
        const Bit8u inspected = there && craft >= 0x100000 ? mem_readb(craft + kInspected) : 0;
        if (there && g_net->inspectKnown[o] && g_net->inspectId[o] == id && !g_net->inspectWas[o] && inspected) {
            TieEvent e;
            e.set_kind(2);
            e.set_obj((Bit32u)o);
            e.set_type(mem_readb(b + 4));
            e.set_fg(mem_readb(b + 0x35));
            g_net->eventsOut.push_back(e);
            wclog(1, "TIE: obj %d (type %u, flight group %u) inspected here: sent", o, mem_readb(b + 4), mem_readb(b + 0x35));
        }
        g_net->inspectKnown[o] = there;
        g_net->inspectId[o] = id;
        g_net->inspectWas[o] = inspected;
    }
}

void take_event(const TieEvent &e, int from) {
    if (e.kind() == 1) {
        const Bit32u m = tie_linear(kMission);
        g_net->reinforcedKnown = true;
        if (!mem_readb(m + kPenaltyFlag)) {
            mem_writeb(m + kPenaltyFlag, 1);
            notice(pilot_name(from) + " called for reinforcements");
            wclog(1, "TIE: pilot %d called for reinforcements: the mission's flag set here", from);
        }
        return;
    }
    if (e.kind() != 2 || e.obj() >= (Bit32u)kCraftSlots) {
        return;
    }
    const int o = (int)e.obj();
    const Bit32u b = tie_linear(tie::kObjects + (Bit32u)o * tie::kObjectSize);
    const Bit32u craft = mem_readd(b + 0x54);
    const int fg = mem_readb(b + 0x35);
    if (mem_readb(b + 4) != e.type() || (Bit32u)fg != e.fg() || fg >= 48 || craft < 0x100000) {
        wclog(1, "TIE: obj %d inspected by pilot %d: not the same craft here (type %u, flight group %d)", o, from, mem_readb(b + 4), fg);
        return;
    }
    g_net->inspectWas[o] = 1;       // (ours to apply, not to send back)
    if (mem_readb(craft + kInspected)) {
        return;                     // (inspected here too)
    }
    mem_writeb(craft + kInspected, 1);
    const Bit32u st = tie_linear(kFgStatus + (Bit32u)fg * kFgStatusSize);
    mem_writeb(st + kFgInspectedCount, (Bit8u)(mem_readb(st + kFgInspectedCount) + 1));
    if ((Bit8s)tie_r8(tie::kFlightGroups + (Bit32u)fg * tie::kFlightGroupSize + kFgSpecialCraft) == (Bit8s)mem_readb(craft + kCraftIndexInFg)) {
        mem_writeb(st + kFgInspectedSpecial, 1);
    }
    tie_queue_call(kFnCraftMessage, (Bit32u)o, craft, kMsgInspected, 0, NULL, NULL, o);
    wclog(1, "TIE: obj %d (flight group %d) inspected by pilot %d: counted here", o, fg, from);
}

bool someone_near(int obj, double reach) {
    const double now = PIC_FullIndex();
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && is_craft(pilot_object(k)) && distance(obj, pilot_object(k)) < reach) {
            return true;
        }
    }
    return false;
}

// Another machine's shot, made here from its craft by the game's own call.
void shot_made(int tag, Bit32u eax) {
    if ((eax & 0xFFFF) != 0xFFFF) {
        g_net->shotsMade++;
        static unsigned logged = 0;
        if (logged < 12) {
            logged++;
            wclog(2, "TIE: a shot of obj %d made here: bolt %u of type %u", tag, (unsigned)(eax & 0xFFFF), tie_r8(obj_base((int)(eax & 0xFFFF)) + 4));
        }
    }
}
void make_shot(const TieShot &shot, int owner) {
    const int obj = (int)shot.obj();
    if (obj < 0 || obj >= kCraftSlots || owner_of(obj) != owner || !is_craft(obj) || tie_r16(obj_base(obj) + 0x2C) != 0) {
        return;
    }
    tie_queue_call(kFnCreateProjectile, (Bit32u)obj, shot.hardpoint(), shot.type(), 0, NULL, shot_made, obj);
}

// A hit for us to apply: a stand-in bolt with the shooter's figures in a free projectile slot, the bolt's segment in
// the collision test's globals, then the game's own collide_laserhitcraft (damage, shields, components, messages,
// the flash where it struck).
int standin_slot = -1;
bool hit_before(int tag) {
    const TieHit &h = g_net->hitsIn[tag];
    standin_slot = -1;
    for (int s = 32; s < 80; s++) {
        if (tie_r8(obj_base(s) + 4) == 0) {
            standin_slot = s;
            break;
        }
    }
    if (standin_slot < 0 || !is_craft((int)h.target()) || !mine((int)h.target())) {
        standin_slot = -1;
        return false;       // (no free projectile slot, or the target gone or no longer ours by now)
    }
    const Bit32u b = obj_base(standin_slot);
    for (int k = 0; k < tie::kObjectSize; k += 4) {
        if (k != 0x54) {
            tie_w32(b + (Bit32u)k, 0);
        }
    }
    tie_w8(b + 2, 1);                               // category: a projectile
    tie_w8(b + 3, (Bit8u)h.bolt_genus());
    tie_w8(b + 4, (Bit8u)h.bolt_type());
    tie_w32(b + 6, (Bit32u)h.x());
    tie_w32(b + 0xA, (Bit32u)h.y());
    tie_w32(b + 0xE, (Bit32u)h.z());
    tie_w32(b + 0x12, (Bit32u)h.ox());
    tie_w32(b + 0x16, (Bit32u)h.oy());
    tie_w32(b + 0x1A, (Bit32u)h.oz());
    tie_w16(b + 0x2A, (Bit16u)h.bolt_radius());
    tie_w16(b + 0x2E, 1);                           // age
    tie_w16(b + 0x30, (Bit16u)h.shooter());
    tie_w8(b + 0x33, tie_r8(obj_base((int)h.shooter() < 120 ? (int)h.shooter() : 0) + 0x33));   // the shooter's side
    tie_w32(b + 0x54, tie_linear(kWarheads + 8 * (Bit32u)(standin_slot - 32)));
    tie_w32(kLaserX, (Bit32u)h.x());
    tie_w32(kLaserY, (Bit32u)h.y());
    tie_w32(kLaserZ, (Bit32u)h.z());
    tie_w32(kLaserXOld, (Bit32u)h.ox());
    tie_w32(kLaserYOld, (Bit32u)h.oy());
    tie_w32(kLaserZOld, (Bit32u)h.oz());
    if (h.has_cx()) {                               // (the flash where the bolt met the craft on the shooter's machine)
        tie_w32(kCollideXOff, (Bit32u)h.cx());
        tie_w32(kCollideYOff, (Bit32u)h.cy());
        tie_w32(kCollideZOff, (Bit32u)h.cz());
    }
    // the call's first argument is the stand-in (it was not known when the call was queued)
    reg_eax = (Bit32u)standin_slot;
    wclog(2, "TIE: applying a hit on obj %u from obj %u: stand-in bolt %d (type %u, radius %u, offset %u)", h.target(), h.shooter(),
          standin_slot, h.bolt_type(), h.bolt_radius(), h.offset());
    return true;
}
void hit_done(int tag, Bit32u eax) {
    const TieHit &h = g_net->hitsIn[tag];
    wclog(2, "TIE: the hit on obj %u applied (returned %08x; the target's hull %u of %u)", h.target(), (unsigned)eax,
          (unsigned)mem_readw(tie_r32(obj_base((int)h.target()) + 0x54) + 0xA2), (unsigned)mem_readw(tie_r32(obj_base((int)h.target()) + 0x54) + 0xA6));
    if (standin_slot >= 0) {
        const Bit32u b = obj_base(standin_slot), t = obj_base((int)h.target());
        if (tie_r8(b + 4) == (Bit8u)h.bolt_type()) {
            tie_w8(b + 4, 0);                       // (not turned into its flash: gone)
        } else {
            const double fx = (double)(Bit32s)tie_r32(b + 6) - (Bit32s)tie_r32(t + 6), fy = (double)(Bit32s)tie_r32(b + 0xA) - (Bit32s)tie_r32(t + 0xA),
                         fz = (double)(Bit32s)tie_r32(b + 0xE) - (Bit32s)tie_r32(t + 0xE);
            wclog(2, "TIE: its flash %.0f from the target's middle (%d %d %d; the shooter's impact %d %d %d)", sqrt(fx * fx + fy * fy + fz * fz),
                  (int)tie_r32(b + 6), (int)tie_r32(b + 0xA), (int)tie_r32(b + 0xE), h.ox() + h.cx(), h.oy() + h.cy(), h.oz() + h.cz());
        }
        g_net->hitsApplied++;
    }
    standin_slot = -1;
    const int t = (int)h.target();
    if (t < kCraftSlots && !g_net->killer[t] && h.shooter() < 120 && (tie_r16(obj_base(t) + 0x2C) != 0 || tie_r8(obj_base(t) + 4) >= 127)) {
        g_net->killer[t] = (int)h.shooter() + 1;    // (the hit that began its death: its shooter's machine credits the kill)
        wclog(2, "TIE: obj %d dying of obj %u's hit, applied here", t, h.shooter());
    }
}
void take_hit(const TieHit &hit) {
    const int target = (int)hit.target();
    if (!is_craft(target) || !mine(target) || hit.shooter() >= 120) {
        wclog(2, "TIE: a hit on obj %d from obj %u not ours to apply (craft %d, ours %d)", target, hit.shooter(), (int)is_craft(target),
              is_craft(target) ? (int)mine(target) : -1);
        return;
    }
    const int tag = g_net->hitsInNext;
    g_net->hitsInNext = (g_net->hitsInNext + 1) % 64;
    g_net->hitsIn[tag] = hit;
    tie_queue_call(kFnLaserHit, 0, (Bit32u)target, hit.offset(), 0, hit_before, hit_done, tag);
}

// A copy whose owner reports it dying is killed here by the game's own damage (a collision's, called until its death
// is under way), so its explosion and the mission's count of it are this game's too.  The game lets a craft die only
// with its flight flag (CraftData +0x60) clear: a copy whose AI was held here can still have its hyperspace arrival's
// (6, cleared by its own AI when the arrival ends), so a flag other than a death's (3, 4) is cleared first.
bool kill_before(int tag) {
    const Bit32u craft = tie_r32(obj_base(tag) + 0x54);
    if (craft >= 0x100000) {
        const Bit8u flag = mem_readb(craft + 0x60);
        if (flag != 0 && flag != 3 && flag != 4) {
            mem_writeb(craft + 0x60, 0);
        }
    }
    return true;
}
void kill_done(int tag, Bit32u eax) {
    (void)eax;
    const int obj = tag;
    if (!is_craft(obj) || tie_r16(obj_base(obj) + 0x2C) != 0 || g_net->killing[obj] >= kKillCalls) {
        if (g_net->killing[obj] > 0) {
            g_net->kills++;
            wclog(2, "TIE: obj %d (type %u) killed here after %d calls: %s", obj, tie_r8(obj_base(obj) + 4), g_net->killing[obj],
                  !is_craft(obj) ? "gone" : tie_r16(obj_base(obj) + 0x2C) != 0 ? "dying" : "NOT dying");
        }
        g_net->killing[obj] = 0;
        return;
    }
    g_net->killing[obj]++;
    tie_queue_call(kFnDamage, (Bit32u)obj, 0xFFFF, 0xFFFF, 0, kill_before, kill_done, obj);
}
// The kill credited to our pilot when its shot began the death on the owner's machine (the open item of step 6: "a
// kill the shooter's machine did not apply is not scored there"): the game's own collide_updatekills(shooter, victim,
// 1), as its damage does for a kill of its own (the craft's kills, the player's kills by species, the congratulations,
// a friendly kill's message), before the copy is killed.
void credit_done(int tag, Bit32u eax) {
    (void)eax;
    g_net->credited++;
    wclog(1, "TIE: obj %d's kill credited to us (our shot began its death on its owner's machine)", tag);
}
void begin_kill(int obj, int killer) {
    if (mine(obj) || g_net->killing[obj] != 0 || !is_craft(obj) || tie_r16(obj_base(obj) + 0x2C) != 0) {
        return;
    }
    g_net->killing[obj] = 1;
    if (killer >= 0 && killer == tie_player_object()) {
        tie_queue_call(kFnUpdateKills, (Bit32u)killer, (Bit32u)obj, 1, 0, NULL, credit_done, obj);
    }
    tie_queue_call(kFnDamage, (Bit32u)obj, 0xFFFF, 0xFFFF, 0, kill_before, kill_done, obj);
}

void send_owners() {
    NetworkMessage msg;
    TieOwners *t = msg.mutable_tie_owners();
    for (int o = 0; o < kCraftSlots; o++) {
        t->add_owner((Bit32u)g_net->owner[o]);
    }
    t->set_mission(tie_mission_name());
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        if (g_net->peers[i]->hello && g_net->peers[i]->conn.is_open()) {
            g_net->peers[i]->conn.send(msg);
        }
    }
}

// A guest: the host's table.  A craft that becomes ours is released to our AI; one that leaves us is a puppet from the
// next report on (its AI held from the next frame).
void take_owners(const TieOwners &t) {
    if (!tie_in_flight() || (t.has_mission() && t.mission() != tie_mission_name())) {
        return;
    }
    const int own = tie_player_object();
    for (int o = 0; o < kCraftSlots && o < t.owner_size(); o++) {
        const int now = (int)t.owner(o);
        if (o == own || now == g_net->owner[o]) {
            continue;
        }
        if (now == g_net->self) {
            release_ai(o);
        }
        g_net->owner[o] = now;
    }
}

// Is a craft in a pilot's view?  Within kViewCone of the nose: the pilot's craft's move vector (+0x3A, Q15) is its
// heading, a TIE craft does not slide.  `cone` the cosine to beat.
bool in_view(int pilotObj, int obj, double cone) {
    const Bit32u pb = obj_base(pilotObj), ob = obj_base(obj);
    const double fx = (Bit16s)tie_r16(pb + 0x3A), fy = (Bit16s)tie_r16(pb + 0x3C), fz = (Bit16s)tie_r16(pb + 0x3E);
    const double dx = (double)(Bit32s)tie_r32(ob + 6) - (double)(Bit32s)tie_r32(pb + 6);
    const double dy = (double)(Bit32s)tie_r32(ob + 0xA) - (double)(Bit32s)tie_r32(pb + 0xA);
    const double dz = (double)(Bit32s)tie_r32(ob + 0xE) - (double)(Bit32s)tie_r32(pb + 0xE);
    const double fl = sqrt(fx * fx + fy * fy + fz * fz), dl = sqrt(dx * dx + dy * dy + dz * dz);
    if (fl < 1 || dl < 1) {
        return dl < 1;
    }
    return (fx * dx + fy * dy + fz * dz) / (fl * dl) >= cone;
}

// The host's rule (the user, 2026-11-06): "host should own most craft except the ones both within range and on the
// guests viewport or specifically targeted by guest and not targeted by the host".  A guest gets a craft that is
// within the lasers' reach of it and in its view, or that it has targeted (within 4 km); a craft the host has targeted
// stays the host's.  To leave a guest a craft must be clearly out (1.25 times the reach, 60 degrees off the nose, or
// no longer its target), and a craft changes owner at most once a second.  Two guests wanting one craft: its target's
// first, then the nearer.
enum { kViewCone45 = 7071, kViewCone60 = 5000 };   // (cosines times 10,000)
bool guest_wants(int k, int o, bool keep) {
    const int po = pilot_object(k);
    const double d = distance(o, po);
    if (g_net->pilotTarget[k] == o && d < kTargetReach * (keep ? 1.25 : 1.0)) {
        return true;
    }
    return d < kReach * (keep ? 1.25 : 1.0) && in_view(po, o, (keep ? kViewCone60 : kViewCone45) / 10000.0);
}
void decide_owners(double now) {
    if (!g_net->host || now - g_net->decidedAt < kDecideEveryMs) {
        return;
    }
    g_net->decidedAt = now;
    int guests[kMaxSeats], n = 0;
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && is_craft(pilot_object(k))) {
            guests[n++] = k;
        }
    }
    const int hostObj = tie_player_object();
    const int hostTarget = tie_r16(tie::kPstate + 0x12);
    bool changed = false;
    for (int o = 0; o < kCraftSlots; o++) {
        bool someonesOwn = o == hostObj;
        for (int i = 0; i < n; i++) {
            someonesOwn = someonesOwn || pilot_object(guests[i]) == o;
        }
        if (someonesOwn || !is_craft(o)) {
            continue;
        }
        const int cur = g_net->owner[o];
        int want = 0;
        if (o != hostTarget) {
            int bestScore = -1;
            double bestDist = 1e30;
            for (int i = 0; i < n; i++) {
                const int k = guests[i];
                if (!guest_wants(k, o, k == cur)) {
                    continue;
                }
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
        for (int i = 0; i < n; i++) {
            curPresent = curPresent || guests[i] == cur;
        }
        if (want != cur && (now - g_net->ownerAt[o] >= kOwnerHoldMs || !curPresent)) {
            g_net->owner[o] = want;
            g_net->ownerAt[o] = now;
            g_net->ownerChanges++;
            changed = true;
            if (want == g_net->self) {
                release_ai(o);
            }
            wclog(2, "TIE: obj %d to pilot %d (was pilot %d)", o, want + 1, cur + 1);
        }
    }
    if (changed || now - g_net->ownersSentAt >= kOwnersEveryMs) {
        g_net->ownersSentAt = now;
        send_owners();
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
    if (tie_r16(o + 0x2C) != 0 || tie_r8(o + 4) >= 127) {
        c->set_dying(true);     // (its death timer running, or already its explosion)
        if (obj < kCraftSlots && g_net->killer[obj]) {
            c->set_killer((Bit32u)(g_net->killer[obj] - 1));
        }
    } else if (obj < kCraftSlots) {
        g_net->killer[obj] = 0;
    }
    const Bit32u craft = obj < kCraftSlots ? tie_r32(o + 0x54) : 0;
    if (craft >= 0x100000) {
        c->set_hull(mem_readw(craft + 0xA2));
        c->set_shield_front((Bit16s)mem_readw(craft + 0xCA));
        c->set_shield_rear((Bit16s)mem_readw(craft + 0xCC));
        c->set_systems(mem_readw(craft + 0xAE));
    }
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
    if (tie_r8(o + 4) == 0 || c.type() == 0 || tie_r8(o + 4) != c.type() || tie_r16(o + 0x2C) != 0) {
        return;             // (not there on this machine, gone on its owner's, another ship in the slot here, or its death under
                            // way here: nothing to move)
    }
    const Bit32u craft = obj < kCraftSlots ? tie_r32(o + 0x54) : 0;
    if (p.fresh) {
        // Where its owner has it less where it is here: written at once when small (a near craft, reported every frame),
        // else blended in, never faster than kCorrectSpeed, and for a craft our AI flies over kFarBlendS at least.
        const double ex = (double)c.x() - (double)(Bit32s)tie_r32(o + 6), ey = (double)c.y() - (double)(Bit32s)tie_r32(o + 0xA),
                     ez = (double)c.z() - (double)(Bit32s)tie_r32(o + 0xE);
        const double err = sqrt(ex * ex + ey * ey + ez * ez);
        double seconds = err / kCorrectSpeed;
        if (p.localAi) {
            // (over the time to its next report, as the last one came: 10 s and more for the far ones reported in turn,
            // a second for those within 10 km of a pilot, reported every few frames)
            const double interval = p.prevAt > 0 ? (p.at - p.prevAt) / 1000.0 : kFarBlendS;
            seconds = fmin(kFarBlendMaxS, fmax(kFarBlendMinS, interval));
            g_net->farReports++;
            g_net->farErrSum += err;
            g_net->farErrMax = fmax(g_net->farErrMax, err);
        } else if (err <= kExactError) {
            seconds = 0;
        }
        if (seconds <= 0) {
            tie_w32(o + 6, (Bit32u)c.x());
            tie_w32(o + 0xA, (Bit32u)c.y());
            tie_w32(o + 0xE, (Bit32u)c.z());
            p.blendTicks = 0;
        } else {
            p.ex = ex;
            p.ey = ey;
            p.ez = ez;
            p.blendTicks = seconds * kTicksPerSecond;
            if (!p.localAi) {
                g_net->nearBlends++;
            }
        }
        // its damage, its owner's (unless a hit of ours here is newer than this report can know)
        if (craft >= 0x100000 && c.has_hull() && PIC_FullIndex() - p.localHitAt > kLocalHitHoldMs) {
            mem_writew(craft + 0xA2, (Bit16u)c.hull());
            mem_writew(craft + 0xCA, (Bit16u)(Bit16s)c.shield_front());
            mem_writew(craft + 0xCC, (Bit16u)(Bit16s)c.shield_rear());
            mem_writew(craft + 0xAE, (Bit16u)c.systems());
        }
        p.fresh = false;
    }
    if (p.blendTicks > 0) {
        const double ticks = fmax(1.0, (double)tie_r16(tie::kFrameticks));
        const double f = fmin(1.0, ticks / p.blendTicks);
        tie_w32(o + 6, (Bit32u)(Bit32s)lround((double)(Bit32s)tie_r32(o + 6) + p.ex * f));
        tie_w32(o + 0xA, (Bit32u)(Bit32s)lround((double)(Bit32s)tie_r32(o + 0xA) + p.ey * f));
        tie_w32(o + 0xE, (Bit32u)(Bit32s)lround((double)(Bit32s)tie_r32(o + 0xE) + p.ez * f));
        p.ex *= 1 - f;
        p.ey *= 1 - f;
        p.ez *= 1 - f;
        p.blendTicks -= ticks;
    }
    if (p.localAi) {
        return;             // (our AI flies it: its attitude, speed and course are its own here)
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

// A pilot's own craft (a human flies it: never this machine's AI, however far).
bool pilot_flies(int obj) {
    const double now = PIC_FullIndex();
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && g_net->pilotObj[k] == obj) {
            return true;
        }
    }
    return false;
}

// WCNET_TIE_FAR_AI=0: every craft another machine flies held here, as before (the rigs' comparison).
bool far_ai() {
    static int on = -1;
    if (on < 0) {
        const char *env = getenv("WCNET_TIE_FAR_AI");
        on = env && env[0] == '0' ? 0 : 1;
    }
    return on != 0;
}

// A hit of a bolt of ours on another machine's craft, shown here at once (the user: the laser's owner applies its
// damage, for precise aiming): the game's own sums (collide_damagecraft: the bolt's radius as the damage, a quarter on a
// freighter, a sixteenth on a starship or platform; the struck side's shield first, then the hull), but never its death
// or its systems here: its owner applies the hit for good (sent with our frame) and its reports bring its damage.
void show_hit(int target, int bolt, int side) {
    const Bit32u craft = tie_r32(obj_base(target) + 0x54);
    if (craft < 0x100000 || side < 0 || side > 1) {
        return;
    }
    long dmg = tie_r16(obj_base(bolt) + 0x2A);
    const int genus = tie_r8(obj_base(target) + 3), boltType = tie_r8(obj_base(bolt) + 4);
    if (genus == 4 || genus == 5) dmg >>= 4;
    if (genus == 3) dmg >>= 2;
    if (genus == 14) dmg = 0;
    if (dmg >= 0x7FFF) dmg = 32766;
    const Bit32u shieldAt = craft + 0xCA + 2 * (Bit32u)side;
    const long shield = (Bit16s)mem_readw(shieldAt);
    if (shield > dmg) {
        mem_writew(shieldAt, (Bit16u)(Bit16s)(shield - dmg));
    } else {
        const long over = dmg - shield;
        mem_writew(shieldAt, 0);
        if (over > 0 && boltType != 141 && boltType != 142) {      // (an ion bolt's overflow goes to systems: the owner's)
            const long hullMax = mem_readw(craft + 0xA6);
            long hull = (long)mem_readw(craft + 0xA2) + over;
            if (hullMax > 0 && hull >= hullMax) {
                hull = hullMax - 1;                                 // (its death is its owner's to say)
            }
            mem_writew(craft + 0xA2, (Bit16u)hull);
        }
    }
    g_net->puppets[target].localHitAt = PIC_FullIndex();
    g_net->hitsShown++;
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
        // our target's damage as this machine has it (the shown hits and the owner's figures)
        const int tgt = tie_r16(tie::kPstate + 0x12);
        if (tgt < kCraftSlots && is_craft(tgt) && tie_r32(obj_base(tgt) + 0x54) >= 0x100000) {
            const Bit32u cr = tie_r32(obj_base(tgt) + 0x54);
            char one[120];
            snprintf(one, sizeof(one), " target obj %d (%s): hull %u of %u, shields %d %d;", tgt, mine(tgt) ? "ours" : "another's",
                     mem_readw(cr + 0xA2), mem_readw(cr + 0xA6), (Bit16s)mem_readw(cr + 0xCA), (Bit16s)mem_readw(cr + 0xCC));
            line += one;
        }
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
            } else if (o < kCraftSlots && g_net->owner[o] == g_net->self && !p.used) {
                continue;
            } else {
                snprintf(one, sizeof(one), " obj %d of pilot %d: here %d %d %d hpr %u %u %u speed %d type %u, reported %d %d %d %.0f ms ago;", o,
                         p.owner + 1, (int)tie_r32(ob + 6), (int)tie_r32(ob + 0xA), (int)tie_r32(ob + 0xE), tie_r16(ob + 0x1E), tie_r16(ob + 0x20),
                         tie_r16(ob + 0x22), (Bit16s)tie_r16(ob + 0x26), tie_r8(ob + 4), p.state.x(), p.state.y(), p.state.z(), now - p.at);
            }
            line += one;
        }
        // the craft this machine flies for the others, as its game has them (scripts/tfnet-join.py's truth for them)
        int owned[kMaxSeats] = { 0 };
        for (int o = 0; o < kCraftSlots; o++) {
            if (!is_craft(o)) {
                continue;
            }
            if (g_net->owner[o] >= 0 && g_net->owner[o] < kMaxSeats) {
                owned[g_net->owner[o]]++;
            }
            if (o == own || g_net->owner[o] != g_net->self) {
                continue;
            }
            const Bit32u ob = obj_base(o);
            char one[160];
            snprintf(one, sizeof(one), " mine obj %d: %d %d %d hpr %u %u %u speed %d type %u;", o, (int)tie_r32(ob + 6), (int)tie_r32(ob + 0xA),
                     (int)tie_r32(ob + 0xE), tie_r16(ob + 0x1E), tie_r16(ob + 0x20), tie_r16(ob + 0x22), (Bit16s)tie_r16(ob + 0x26), tie_r8(ob + 4));
            line += one;
        }
        char sums[400];
        snprintf(sums, sizeof(sums), " owners %d/%d/%d; sent %u craft (%u near, %u mid, %u far) in %u bytes, %u owner changes; guns: %u shots"
                 " sent, %u made, %u hits sent, %u applied, %u damage skipped, %u kills, %u credited;", owned[0], owned[1], owned[2],
                 g_net->sentCraft, g_net->sentNear, g_net->sentMid, g_net->sentFar, g_net->sentBytes, g_net->ownerChanges, g_net->shotsSent,
                 g_net->shotsMade, g_net->hitsSent, g_net->hitsApplied, g_net->damageSkipped, g_net->kills, g_net->credited);
        g_net->sentCraft = g_net->sentNear = g_net->sentMid = g_net->sentFar = g_net->sentBytes = g_net->ownerChanges = 0;
        g_net->shotsSent = g_net->shotsMade = g_net->hitsSent = g_net->hitsApplied = g_net->damageSkipped = g_net->kills = g_net->credited = 0;
        char far[240];
        snprintf(far, sizeof(far), " far: %u flown by our AI (%u began, %u ended), %u of their shots here, %u reports pulled in (error mean %.0f, worst "
                 "%.0f), %u near ones blended; %u hits of ours shown here;", g_net->localAiNow, g_net->localAiStarts, g_net->localAiEnds,
                 g_net->localShots, g_net->farReports, g_net->farReports ? g_net->farErrSum / g_net->farReports : 0.0, g_net->farErrMax,
                 g_net->nearBlends, g_net->hitsShown);
        g_net->localAiStarts = g_net->localAiEnds = g_net->localShots = g_net->farReports = g_net->nearBlends = g_net->hitsShown = 0;
        g_net->farErrSum = g_net->farErrMax = 0;
        wclog(2, "TIE: wall %.0f frame %llu as pilot %d, %u peers;%s%s%s", wall_ms(), (unsigned long long)g_net->frame, g_net->self + 1,
              (unsigned)g_net->peers.size(), sums, far, line.c_str());
    }
}

}  // namespace

// A guest's launch click held (tie_session_click), given to the game once the host flies, as the notice said ("you
// take off together once the host is flying"; until 2026-11-07 it was dropped and the guest had to click again): the
// pointer put back where it was and the button pressed, let go 150 ms later; forgotten if the guest left that screen.
void launch_when_host_flies(double now) {
    if (g_net->heldReleaseAt >= 0 && now >= g_net->heldReleaseAt) {
        g_net->heldReleaseAt = -1;
        Mouse_ButtonReleased(0);
    }
    if (g_net->heldScene < 0 || g_net->host) {
        return;
    }
    if (tie_in_flight() || tie_scene() != g_net->heldScene) {
        g_net->heldScene = -1;
        return;
    }
    if (!g_net->pilots[0].flying) {
        return;
    }
    wclog(1, "TIE: the host flies: our held launch click (scene %d at %d,%d) given to the game", g_net->heldScene, g_net->heldX, g_net->heldY);
    tie_set_cursor(g_net->heldX, g_net->heldY);
    Mouse_ButtonPressed(0);
    g_net->heldReleaseAt = now + 150;
    g_net->heldScene = -1;
}

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
    launch_when_host_flies(now);
    // the flight's end seen here too (the frame hook stops coming)
    const bool flying = tie_in_flight();
    if (g_net->wasFlying && !flying) {
        tie_session_flight_ended();
    }
    g_net->wasFlying = flying;
}

namespace {

// The room's end of a flight: a pilot down (its craft dying: killed, or ejected) or the host's flight over ends every
// pilot's, after a moment, as the game's own quit does: the debriefing, the mission failed (unless its goals were met),
// and from there the Combat Chamber to fly it again together.  Our own death too: no killed, captured or rescued here.
// Only with another pilot in the room.
bool others_flying() {
    const double now = PIC_FullIndex();
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now)) {
            return true;
        }
    }
    return false;
}
void room_end(int seat, const char *what) {
    if (!tie_in_flight() || g_net->endAt > 0 || !others_flying()) {
        return;
    }
    g_net->endAt = PIC_FullIndex() + kRoomEndMs;
    const std::string who = seat == g_net->self ? std::string("You") : pilot_name(seat);
    wclog(1, "TIE: %s%s: the flight ends for everybody", who.c_str(), seat == g_net->self && !strcmp(what, " is down") ? " are down" : what);
    notice(who + (seat == g_net->self && !strcmp(what, " is down") ? " are down" : what) + ": the flight ends for everybody, mission failed");
}
void room_end_frame() {
    const int own = tie_player_object();
    const Bit32u m = tie_linear(kMission);
    // (our own death: killed, captured or rescued becomes the mission's end at once, for the debriefing; our craft by
    // then perhaps its explosion already)
    const bool down = mem_readb(m + kMissionStatus) < 3 || (own >= 0 && own < kCraftSlots && is_craft(own) && tie_r16(obj_base(own) + 0x2C) != 0);
    if (down && others_flying()) {
        if (mem_readb(m + kMissionStatus) < 3) {
            wclog(1, "TIE: our status %u (0 killed, 1 captured, 2 rescued) made 3, the mission's end, for the room", mem_readb(m + kMissionStatus));
            mem_writeb(m + kMissionStatus, 3);
        }
        room_end(g_net->self, " is down");
    }
    if (g_net->endAt > 0 && PIC_FullIndex() >= g_net->endAt) {
        g_net->endAt = 0;
        mem_writeb(m + kMissionStatus, 3);
        mem_writeb(m + kMissionEnd, 1);
        wclog(1, "TIE: the flight ended for the room (the mission's end flag set, its status 3)");
    }
}

}  // namespace

void tie_session_frame() {
    if (!g_net || g_net->self < 0 || !tie_in_flight()) {
        return;
    }
    poll_peers();       // (what came in since the last frame: written before this frame's move)
    room_end_frame();
    map_group();
    report_frame(PIC_FullIndex());
    const int own = tie_player_object();
    if (own < 0 || own >= 120) {
        return;
    }
    const double now = PIC_FullIndex();
    decide_owners(now);
    g_net->owner[own] = g_net->self;
    NetworkMessage msg;
    TieFrame *f = msg.mutable_tie_frame();
    f->set_player((Bit32u)g_net->self);
    f->set_frame(++g_net->frame);
    f->set_ticks(tie_r16(tie::kFrameticks));
    f->set_flying(true);
    f->set_mission(tie_mission_name());
    f->set_own((Bit32u)own);
    f->set_target(tie_r16(tie::kPstate + 0x12));
    fill_craft(f->add_craft(), own);
    g_net->sentCraft++;
    // The craft we fly for the others, each as often as its distance from the nearest other pilot asks: near every
    // frame, within 10 km every kMidEvery frames, the far ones one at a time every kFarEvery frames in turn.
    int others[kMaxSeats], n = 0;
    for (int k = 0; k < kMaxSeats; k++) {
        if (k != g_net->self && pilot_present(k, now) && is_craft(pilot_object(k))) {
            others[n++] = k;
        }
    }
    bool far = false;
    if (n > 0) {
        int farPick = -1;
        if (g_net->frame % kFarEvery == 0) {
            // the next far craft of ours after the last one sent
            for (int step = 1; step <= kCraftSlots && farPick < 0; step++) {
                const int o = (g_net->farCursor + step) % kCraftSlots;
                if (o == own || g_net->owner[o] != g_net->self || !is_craft(o)) {
                    continue;
                }
                double d = 1e30;
                for (int i = 0; i < n; i++) {
                    d = fmin(d, distance(o, pilot_object(others[i])));
                }
                if (d >= kMidReach) {
                    farPick = o;
                }
            }
            if (farPick >= 0) {
                g_net->farCursor = farPick;
            }
        }
        for (int o = 0; o < kCraftSlots; o++) {
            if (o == own || g_net->owner[o] != g_net->self || !is_craft(o)) {
                continue;
            }
            double d = 1e30;
            for (int i = 0; i < n; i++) {
                d = fmin(d, distance(o, pilot_object(others[i])));
            }
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
    for (size_t i = 0; i < g_net->shotsOut.size(); i++) {
        *f->add_shots() = g_net->shotsOut[i];
    }
    for (size_t i = 0; i < g_net->hitsOut.size(); i++) {
        *f->add_hits() = g_net->hitsOut[i];
    }
    watch_events();
    for (size_t i = 0; i < g_net->eventsOut.size(); i++) {
        *f->add_events() = g_net->eventsOut[i];
    }
    g_net->shotsSent += (unsigned)g_net->shotsOut.size();
    g_net->hitsSent += (unsigned)g_net->hitsOut.size();
    const bool hits = !g_net->hitsOut.empty() || !g_net->eventsOut.empty();
    g_net->shotsOut.clear();
    g_net->hitsOut.clear();
    g_net->eventsOut.clear();
    // (a reliable copy once a second, whenever a far craft is in it, its next report many seconds away, and whenever a
    // hit is: a hit lost would be damage never done)
    const bool reliable = far || hits || now - g_net->lastReliableAt >= kReliableEveryMs;
    if (reliable) {
        g_net->lastReliableAt = now;
    }
    const unsigned bytes = (unsigned)msg.ByteSizeLong();
    for (size_t i = 0; i < g_net->peers.size(); i++) {
        Peer *p = g_net->peers[i];
        if (!p->hello || !p->conn.is_open()) {
            continue;
        }
        g_net->sentBytes += bytes;
        if (reliable) {
            p->conn.send(msg);
        } else {
            p->conn.send_best_effort(msg);
        }
    }
    // This machine's AI kept off every craft another flies near our pilot (before the planner runs this frame); a far
    // one is our AI's to fly (its place pulled to its owner's), hysteresis between kLocalAiIn and kLocalAiOut.
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
        p.localAi = far_ai() && (was ? d >= kLocalAiIn : d >= kLocalAiOut);
        if (p.localAi) {
            g_net->localAiNow++;
            if (!was) {
                const Bit32u craft = tie_r32(obj_base(o) + 0x54);
                if (craft >= 0x100000) {
                    mem_writew(craft + 0x2A, 0);      // (it plans at once)
                }
                g_net->localAiStarts++;
            }
        } else {
            if (was) {
                g_net->localAiEnds++;
            }
            hold_ai(o);
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
        if (p.used && p.owner != g_net->self && o != own && (o >= kCraftSlots || owner_of(o) == p.owner)) {
            write_puppet(p);
        }
    }
}

// laser_createprojectile(eax shooter, edx hardpoint, ebx type).  A shot of a craft we fly, near another pilot, goes
// with our next frame; a craft another machine flies does not shoot here (its owner's shots are made from reports).
void tie_session_shot() {
    if (!g_net || g_net->self < 0 || !tie_in_flight() || tie_in_call()) {
        return;
    }
    const int shooter = (int)(reg_eax & 0xFFFF);
    if (shooter >= kCraftSlots) {
        return;
    }
    if (!mine(shooter)) {
        if (g_net->puppets[shooter].localAi) {
            g_net->localShots++;    // (a far craft our AI flies: its shot made here for show; it damages nothing here)
            return;
        }
        tie_skip_call(0xFFFF);      // (no slot: its callers expect that)
        return;
    }
    static unsigned logged = 0;
    if (logged < 12 && shooter == tie_player_object()) {
        logged++;
        wclog(2, "TIE: our shot from obj %d: hardpoint %u, projectile type %u", shooter, (unsigned)(reg_edx & 0xFFFF), (unsigned)(reg_ebx & 0xFFFF));
    }
    if (g_net->shotsOut.size() < 32 && someone_near(shooter, kShotReach)) {
        TieShot shot;
        shot.set_obj((Bit32u)shooter);
        shot.set_hardpoint(reg_edx & 0xFFFF);
        shot.set_type(reg_ebx & 0xFFFF);
        g_net->shotsOut.push_back(shot);
    }
}

// collide_laserhitcraft(eax bolt, edx target, ebx offset).  A hit by a bolt of a craft we fly on a craft another machine
// flies is that machine's to apply: sent with our next frame (the flash still shows here; the damage is skipped at
// collide_damagecraft).
void tie_session_laser_hit() {
    if (!g_net || g_net->self < 0 || !tie_in_flight() || tie_in_call()) {
        return;
    }
    const int bolt = (int)(reg_eax & 0xFFFF), target = (int)(reg_edx & 0xFFFF);
    if (bolt < 32 || bolt >= 120 || !is_craft(target)) {
        return;
    }
    const Bit32u b = obj_base(bolt);
    const int shooter = tie_r16(b + 0x30);
    if (!mine(shooter) || mine(target) || g_net->hitsOut.size() >= 32) {
        return;
    }
    TieHit h;
    h.set_target((Bit32u)target);
    h.set_offset(reg_ebx & 0xFFFF);
    h.set_bolt_type(tie_r8(b + 4));
    h.set_bolt_genus(tie_r8(b + 3));
    h.set_bolt_radius(tie_r16(b + 0x2A));
    h.set_shooter((Bit32u)shooter);
    h.set_x((Bit32s)tie_r32(kLaserX));
    h.set_y((Bit32s)tie_r32(kLaserY));
    h.set_z((Bit32s)tie_r32(kLaserZ));
    h.set_ox((Bit32s)tie_r32(kLaserXOld));
    h.set_oy((Bit32s)tie_r32(kLaserYOld));
    h.set_oz((Bit32s)tie_r32(kLaserZOld));
    h.set_cx((Bit32s)tie_r32(kCollideXOff));
    h.set_cy((Bit32s)tie_r32(kCollideYOff));
    h.set_cz((Bit32s)tie_r32(kCollideZOff));
    g_net->hitsOut.push_back(h);
}

// collide_damagecraft(eax target, edx component, ebx attacker, ecx side).  This game damages only the craft it flies,
// and from a bolt only one whose shooter it flies (a shot made from another machine's report hits nothing here: its
// shooter's machine decides).  Our own calls (a hit taken in, a kill) go through.
void tie_session_damage() {
    if (!g_net || g_net->self < 0 || !tie_in_flight() || tie_in_call()) {
        return;
    }
    const int target = (int)(reg_eax & 0xFFFF), attacker = (int)(reg_ebx & 0xFFFF);
    bool skip = target < kCraftSlots && !mine(target);
    if (skip && attacker >= 32 && attacker < 120 && mine(tie_r16(obj_base(attacker) + 0x30))) {
        // (a starship's or platform's part has its own sums, starship_damagecomponent's: left to its owner)
        const int genus = tie_r8(obj_base(target) + 3);
        if ((reg_edx & 0xFFFF) == 0xFFFF || (genus != 4 && genus != 5)) {
            show_hit(target, attacker, (int)(reg_ecx & 0xFFFF));
        }
    }
    if (!skip && attacker >= 32 && attacker < 120) {
        skip = !mine(tie_r16(obj_base(attacker) + 0x30));
    }
    if (skip) {
        g_net->damageSkipped++;
        tie_skip_call(0);
    }
}

// Step 13: a guest's launch of a room's mission (a Combat Chamber mission, or the room's last one again: Fly Mission
// Again) waits for the host, held where the program asks for the mission's file (the call made again until it may go
// on): into the briefing until the host is at its briefing or flying, into the flight until the host flies.  The host
// decides the mission; the guest then flies it (tie_session_mission_path).  Not when no host is known, nor for a tour
// battle the guest launches (its own), nor after Esc (flown alone).
bool tie_session_mission_wait() {
    if (!g_net || g_net->host || g_net->self < 0) {
        return false;
    }
    const Pilot &h = g_net->pilots[0];
    const std::string ours = tie_mission_name();
    const bool room = !ours.empty() && (ours[0] == 'h' || ours[0] == 'H' || ours == g_net->roomMission);
    if (g_net->waitCancelled || !h.known || !room) {
        g_net->waiting = false;
        return false;
    }
    // (only a flight's start waits: the Combat Chamber asks for the file as its screen opens, the briefing as it loads,
    // and the guest may look at them; its launch buttons wait at the click, tie_session_click)
    const int scene = tie_scene();
    const bool intoFlight = (scene >= 2 && scene <= 4) || (scene >= 190 && scene <= 192);
    if (h.flying || !intoFlight) {
        if (g_net->waiting) {
            g_net->waiting = false;
            wclog(1, "TIE: the host is in: our launch goes on");
        }
        return false;
    }
    const double now = PIC_FullIndex();
    if (!g_net->waiting) {
        g_net->waiting = true;
        g_net->waitSaidAt = -1e9;
        wclog(1, "TIE: our launch waits for the host (our scene %d, the host's %d, flying %d)", scene, h.scene, (int)h.flying);
    }
    if (now - g_net->waitSaidAt >= 3000) {
        g_net->waitSaidAt = now;
        notice("Waiting for " + pilot_name(0) + (intoFlight ? " to fly" : " to launch") + ": you fly the host's mission (Esc: fly yours alone)");
    }
    return true;
}

// Step 13: a guest's click on a launch while the host is not flying is not given to the game (the user: "it might be
// simpler if the host did it and guest had to wait or didn't have the option like retro vega until the host is in"):
// the briefing's and the debriefing officer's bar, its launch button (map.c's map_rect: 225..257 x 172..194: Enter
// Mission, Fly Mission Again); the briefing room's door to the mission (brief.c: 190..248 x 118..138); the debriefing
// room's Fly Again (debrief.c: 0..70, the room's left).  Its release goes with it.
bool tie_session_click(int button, bool pressed) {
    static bool taken[3];
    if (button < 0 || button > 2) {
        return false;
    }
    if (!pressed) {
        const bool was = taken[button];
        taken[button] = false;
        return was;
    }
    if (!g_net || g_net->host || g_net->self < 0) {
        return false;
    }
    const Pilot &h = g_net->pilots[0];
    int x = 0, y = 0;
    if (!h.known || h.flying || !tie_cursor(&x, &y)) {
        return false;
    }
    const int scene = tie_scene();
    const bool bar = scene == 133 || (scene >= 181 && scene <= 183) || scene == 191 || scene == 192;
    const bool launch = (bar && x >= 225 && x <= 257 && y >= 172 && y <= 194) || (scene == 180 && x >= 190 && x <= 248 && y >= 118 && y <= 138) ||
                        (scene == 190 && x <= 70);
    if (!launch) {
        return false;
    }
    taken[button] = true;
    if (button == 0) {
        g_net->heldScene = scene;       // (given to the game when the host flies: launch_when_host_flies)
        g_net->heldX = x;
        g_net->heldY = y;
    }
    wclog(1, "TIE: our launch click (scene %d at %d,%d) waits: the host is not flying", scene, x, y);
    notice(pilot_name(0) + " has not launched yet: you take off together once " + pilot_name(0) + " is flying");
    return true;
}

bool tie_session_key(int kbdKey, bool pressed) {
    if (!g_net || !g_net->waiting || kbdKey != KBD_esc) {
        return false;
    }
    if (pressed) {
        g_net->waiting = false;
        g_net->waitCancelled = true;
        wclog(1, "TIE: Esc while waiting: our launch goes on alone");
        notice("Flying alone: your own mission");
    }
    return true;
}

void tie_session_set_mission(const char *name) {
    std::string m;
    for (const char *c = name ? name : ""; *c; c++) {
        m += (char)tolower((unsigned char)*c);
    }
    g_pickRead = true;
    if (!m.empty() && !mission_file_name(m)) {
        wclog(0, "TIE: '%s' is no mission file's name: none picked", m.c_str());
        m.clear();
    }
    if (m != g_pick) {
        wclog(1, "TIE: the room's mission: %s", m.empty() ? "none (the Combat Chamber's own)" : m.c_str());
    }
    g_pick = m;
}

// The game about to name the file of the mission it launches (the briefing's map loads it, then the flight), from the
// front end's mission_name.  A launch from the Combat Chamber flies, for a guest, the host's mission when the host is
// at its briefing or flying; otherwise the room's pick, if any.  A battle launched from the tour is the pilot's tour's:
// never changed.  Only the 14 bytes of mission_name are written; the game makes the file's name from them next.
// (step 14, below) the Combat Chamber's T/F missions and the challenge each is flown from
const char *const kChallenge[4][2] = { { "hf1w", "hg1w" }, { "hf2w", "hi2w" }, { "hf3m", "hg3m" }, { "hf4m", "hg4m" } };
std::string challenge_source(const std::string &m);   // (step 14, below)

void tie_session_mission_path() {
    if (!g_net || (!g_net->host && g_net->self < 0)) {
        return;
    }
    const std::string ours = tie_mission_name();
    const bool combat = ours.size() >= 2 && (ours[0] == 'h' || ours[0] == 'H');
    const std::string &pick = picked_mission();
    std::string want;
    bool hostsOwn = false;
    const Pilot *h = g_net->host ? NULL : &g_net->pilots[0];
    if (h) {
        const bool briefing = (h->scene >= 133 && h->scene <= 137) || (h->scene >= 179 && h->scene <= 183);
        if (h->known && (h->flying || briefing) && combat && mission_file_name(h->mission)) {
            want = h->mission;
            hostsOwn = true;
        }
    }
    if (want.empty() && combat && !pick.empty()) {
        want = pick;
    }
    std::string flies = want.empty() ? ours : want;
    // (step 14: in a room the Combat Chamber's T/F row is the Z-95 challenge, flown from its source: in the chamber the
    // row decides; a guest flying the host's mission takes the host's word; the same launch's later calls keep it)
    const int scene = tie_scene();
    if (hostsOwn) {
        g_net->challenge = h->challenge;
        g_net->challengeFile = h->challenge ? flies : std::string();
    } else {
        // (a T/F mission's file, the room's pick or ours, is the challenge at any of a launch's calls; with none such,
        // in the chamber its own selection says: the pilot record's craft row, +0x66, 0 the T/F's, and the row's
        // mission, +0x67 on, the file's name perhaps already ours from the chamber's first call)
        std::string source = challenge_source(flies);
        const bool chamber = scene == 130 || scene == 131;
        if (source.empty() && chamber && want.empty()) {
            const int row = tie_r8(kPilotRecord + 0x66), course = tie_r8(kPilotRecord + 0x67);
            if (row == 0 && course < 4) {
                source = kChallenge[course][1];
            }
            wclog(2, "TIE: the Combat Chamber's row %d, mission %d", row, course);
        }
        if (!source.empty()) {
            want = flies = source;
            if (!g_net->challenge || g_net->challengeFile != source) {
                notice("The Z-95 challenge: " + mission_words(source) + " flown in Z-95 Headhunters against the Empire's craft");
            }
            g_net->challenge = true;
            g_net->challengeFile = source;
        } else if (chamber || flies != g_net->challengeFile) {
            g_net->challenge = false;
            g_net->challengeFile.clear();
        }
    }
    wclog(2, "TIE: the mission's file: ours %s, flown %s (scene %d)%s", ours.c_str(), flies.c_str(), scene, g_net->challenge ? ", the Z-95 challenge" : "");
    if (h && h->known && h->flying && !h->mission.empty() && flies != h->mission) {
        notice("You launch " + mission_words(flies) + "; " + pilot_name(0) + " flies " + mission_words(h->mission) + ": not together");
    }
    if (!want.empty()) {
        g_net->roomMission = want;
    }
    if (want.empty() || want == ours) {
        return;
    }
    for (size_t i = 0; i < 14; i++) {
        tie_w8(tie::kMissionName + (Bit32u)i, i < want.size() ? (Bit8u)want[i] : 0);
    }
    if (hostsOwn) {
        notice("Flying " + pilot_name(0) + "'s mission, " + mission_words(want) + (g_net->challenge ? ", the Z-95 challenge" : "") +
               " (you had picked " + mission_words(ours) + ")");
    } else if (!g_net->challenge) {
        notice("Flying the room's mission, " + mission_words(want) + " (the Combat Chamber had " + mission_words(ours) + ")");
    }
}

// Step 9, the user's pairings: a mission flown alone (one craft in the player's flight group) gives the other pilots a
// craft that fits: by the mission's craft type, the guests' (0: the same craft, the seats' craft of the player's group).
// The user, 2026-11-06: "T/A -> T/A x 2 / T/D -> T/D + T/A / T/I -> T/F + T/F unless there's a timing aspect where T/F is
// too slow... / Gunboat -> Gunboat + Standard Imperial Shuttle / T/F -> T/F + T/F / Missile boat -> missile boat +
// gunboat (or missile boat + standard imperial shuttle depending on difficulty) / T/B -> T/B + T/F", then for the missile
// boat "we need to do missile boat + shuttle because otherwise the gunboat could do some ion cannon shenanigans"; a third pilot
// flies the same as the second ("The third should be same as the guest in all cases").
// The user, 2026-11-07: "When we aren't adding ships the types should remain as is": the mission's own craft keep their type
// (the T/I stays a T/I, its partner a T/F beside it); a group with craft for every pilot flies as it is.
// ... except where the user chose two T/Fs for a T/I flown alone (2026-11-07: "for the harkov minefield it would be best
// to replace the 1 T/I with 2 T/F more fitting in some ways can you examine other solo t/i missions and see if they are
// beatable with 2 t/f"): the player's group retyped T/F, a craft for each pilot.  B5M1 (Harkov's minefield: its goals
// inspections and the frigate's arrival, the mines a danger, not a goal) and HI1W (customs inspections: nothing leaves,
// no mines, its B-wings late and a bonus).  Not HI3M: its goal is every one of ten mines destroyed, which a T/F (half a
// T/I's hull and guns) would face with half the margin; as T/I + T/F the T/I can do it while the T/F keeps clear.
// WCNET_TIE_TI_AS_TF=<missions, comma-separated> (or "none") replaces the list (the rigs' comparisons).
bool ti_flown_as_tfs(const std::string &mission) {
    static const char *const kMissions[] = { "b5m1iw", "hi1w" };
    std::string m;
    for (size_t i = 0; i < mission.size(); i++) m += (char)tolower((unsigned char)mission[i]);
    const char *env = getenv("WCNET_TIE_TI_AS_TF");
    if (env) {
        const std::string list = std::string(",") + env + ",";
        return list.find("," + m + ",") != std::string::npos;
    }
    for (size_t k = 0; k < sizeof(kMissions) / sizeof(kMissions[0]); k++) {
        if (m == kMissions[k]) return true;
    }
    return false;
}

int paired_type(int type) {
    switch (type) {
    case 6: return 5;       // T/I + T/F (the mission's T/I as it is)
    case 7: return 5;       // T/B + T/F: one flies the torpedo work, the other point defense
    case 9: return 8;       // T/D + T/A
    case 12: return 17;     // missile boat + the standard Imperial shuttle (the user: "otherwise the gunboat could do some ion
                            // cannon shenanigans and the designers probably didn't think of that")
    case 16: return 17;     // gunboat + the standard Imperial shuttle (the gunboat's cockpit, made over: wcnet_tie_cockpit.cpp)
    default: return 0;      // T/F, T/A: the same craft
    }
}

// At fediskio_loadspecies's entry (the mission read, its craft not yet loaded), on every machine alike: a mission
// flown alone gets its pairing.  A pair of two crafts: the player's group copied as a second one at the end, in the
// guests' craft, one for each seat after the host's, starting and flying 0.4 km to the side; a guest flies its own
// craft of it.
// Step 14, the Z-95 challenge (the user, 2026-11-07: "replace all tie fighter missions in the combat trainer with the
// gunboat missions that don't need ion cannons or the tie interceptor missions if they do, and the ship you fly is the
// z95 headhunter but do a substitution of imperial equivalents of each rebel craft etc... These are meant to be
// challenge missions since the z-95 is hopelessly underpowered but there will be two of them").  In a room the
// Combat Chamber's T/F row flies these: the gunboat's missions where no goal needs its ion cannons (a primary goal
// to disable, capture or board), else the T/I's (HG2W's primary goals are to disable two craft: HI2W).
std::string challenge_source(const std::string &m) {
    std::string l;
    for (size_t i = 0; i < m.size(); i++) l += (char)tolower((unsigned char)m[i]);
    for (int k = 0; k < 4; k++) {
        if (l == kChallenge[k][0]) return kChallenge[k][1];
    }
    return std::string();
}

// The other side's craft for each craft of the mission (the mission files' types): the Rebels' as the Empire's and
// the Empire's as the Rebels', fighter for fighter by role, capital ship for capital ship; 0: the same (pirates',
// containers, transports, platforms, mines).  The player's group flies Z-95s.
int mirror_type(int t) {
    switch (t) {
    case 1: return 6;       // X-wing: T/I
    case 2: return 7;       // Y-wing: T/B
    case 3: return 6;       // A-wing: T/I
    case 4: return 16;      // B-wing: gunboat
    case 13: return 6;      // T-wing: T/I
    case 14: return 5;      // Z-95: T/F
    case 15: return 5;      // R-41: T/F
    case 5: return 14;      // T/F: Z-95
    case 6: return 3;       // T/I: A-wing
    case 7: return 2;       // T/B: Y-wing
    case 8: return 1;       // T/A: X-wing
    case 9: return 4;       // T/D: B-wing
    case 12: return 4;      // missile boat: B-wing
    case 16: return 4;      // gunboat: B-wing
    case 40: case 41: return 45;    // Corellian corvette, modified: Carrack cruiser
    case 42: case 43: return 46;    // Nebulon-B frigates: strike cruiser
    case 49: return 53;     // Calamari cruiser: Imperial Star Destroyer
    case 50: return 52;     // light Calamari cruiser: Victory Star Destroyer
    case 45: return 40;     // Carrack cruiser: Corellian corvette
    case 46: return 42;     // strike cruiser: Nebulon-B frigate
    case 52: return 50;     // Victory Star Destroyer: light Calamari cruiser
    case 51: case 53: return 49;    // Interdictor, Imperial Star Destroyer: Calamari cruiser
    default: return 0;
    }
}

// The challenge's lines (the user, 2026-11-07: "just replace them with those other side lines.... where it talks about
// the weak z-95 or others where they talk about rebel scum. lots of lines can sub in... find extant lines"): each
// radio message of the four missions that speaks for the Empire's side is replaced by a line of another mission, from
// the player's own files, that fits the mirrored sides: the player's side speaks of Imperial craft as the enemy (the
// battles against Zaarin's and Harkov's traitors, the Rebels' own lines in B10M1 and B12M6), the enemy of the Z-95s.
// A foe's line in the missions' own enemy lines' color (its first character '1': green), a friend's without one (the
// command's red), as each mission's own lines are.  Only
// where the lines come from is here; their words are read from the mission files at the launch.
struct ChallengeLine {
    const char *mission;    // the challenge's source mission
    int index;              // its radio message
    const char *from;       // the mission whose line it gets (MISSION\<from>.TIE)
    int fromIndex;
    bool foe;
};
const ChallengeLine kChallengeLines[] = {
    // HG1W, Mu 1: the transport that bolts and the fighters with it arrive
    { "hg1w", 13, "b10m1gw", 2, false },    // (for the rebel scum bolting: the Rebels' own Imperial scum)
    { "hg1w", 14, "b2m4gw", 11, true },     // (for the ion cannons, a Z-95 has none: the enemy saving its warheads)
    // HI2W, Gamma 2
    { "hi2w", 0, "b1m2fm", 1, false },      // (for engaging Rebel fighters: Beta and Gamma engaging the enemy)
    { "hi2w", 1, "b13m5aw", 3, false },     // (for T/I Gamma and the escaping transport: no traitor escapes)
    { "hi2w", 2, "hm1w", 12, true },        // (for T/I Eta's no prisoners: the enemy's word of the Z-95s)
    { "hi2w", 3, "b2m5fw", 4, false },      // (for the Rebels taking the bait: hostile ships in the system)
    { "hi2w", 4, "b11m6mw", 1, false },     // (for the X-wings, T/Is now: more enemy forces)
    { "hi2w", 5, "b9m1dw", 1, false },      // (for the Y-wings' torpedoes, T/Bs now: the TIE Bombers first)
    { "hi2w", 15, "b4m1fm", 4, true },      // (for the A-wings, T/Is now: the enemy launching Interceptors)
    // HG3M, Tau
    { "hg3m", 0, "ha2w", 7, true },         // (for surrender or die: the enemy on those who fly Z-95s)
    { "hg3m", 1, "hm4m", 6, true },         // (for one of our Y-wings: the enemy on you and your wingman)
    // HG4M, Tau
    { "hg4m", 0, "b12m3gm", 0, false },     // (for the Imperial Gunboats thanked: Mu leading Tau in)
    { "hg4m", 2, "b12m6mm", 5, false },     // (for Rebel war craft in the area: the Rebels' own alert of an Imperial)
};

// A mission file's radio message (its 64 bytes of text), from the player's files; empty if not read.
std::string mission_line(const std::string &file, int index) {
    std::string dos = "MISSION\\";
    for (size_t i = 0; i < file.size(); i++) dos += (char)toupper((unsigned char)file[i]);
    std::vector<Bit8u> d;
    if (!tie_game_file(dos + ".TIE", d) || d.size() < 2 + 456) {
        return std::string();
    }
    const size_t base = (Bit16s)(d[0] | d[1] << 8) <= 0 ? 2 : 0;      // (the version word, -1, first)
    const int numFg = (Bit16s)(d[base] | d[base + 1] << 8), numMsg = (Bit16s)(d[base + 2] | d[base + 3] << 8);
    if (numFg < 0 || index < 0 || index >= numMsg) {
        return std::string();
    }
    const size_t at = base + 456 + (size_t)numFg * 292 + (size_t)index * 90;
    std::string t;
    for (size_t k = 0; k < 64 && at + k < d.size() && d[at + k]; k++) t += (char)d[at + k];
    return t;
}

// A line as the challenge says it: its own speaker's color off, the foe's on (the missions' enemy lines' '1').
std::string challenge_text(const ChallengeLine &c) {
    std::string t = mission_line(c.from, c.fromIndex);
    while (!t.empty() && t[0] >= '1' && t[0] <= '3') t.erase(0, 1);
    while (!t.empty() && t[t.size() - 1] == ' ') t.erase(t.size() - 1);
    if (!t.empty() && c.foe) {
        t = "1" + t;
    }
    if (t.size() > 63) {
        t.resize(63);
    }
    return t;
}

// Each line the table has for the mission, written over its own; `from` (where each of its 16 is from, its own to
// begin with) is told.
void challenge_lines(const std::string &m, std::vector<TieVoiceLine> &from) {
    const int numMsg = (Bit16s)tie_r16(tie::kMissionHeader + 2);
    int put = 0, missing = 0;
    for (size_t i = 0; i < sizeof(kChallengeLines) / sizeof(kChallengeLines[0]); i++) {
        const ChallengeLine &c = kChallengeLines[i];
        if (m != c.mission || c.index >= numMsg || c.index >= 16) {
            continue;
        }
        const std::string t = challenge_text(c);
        if (t.empty()) {
            missing++;
            continue;
        }
        const Bit32u at = kRadioMsg + (Bit32u)c.index * 90;
        for (Bit32u k = 0; k < 64; k++) {
            tie_w8(at + k, k < t.size() ? (Bit8u)t[k] : 0);
        }
        from[c.index].mission = c.from;
        from[c.index].index = c.fromIndex;
        put++;
    }
    if (put || missing) {
        wclog(1, "TIE: the Z-95 challenge's lines: %d of %s's replaced by other missions'%s", put, m.c_str(), missing ? " (some not read)" : "");
    }
}

// The line the challenge's Imperials say of the Z-95s (the user: "ideally we use the line 'They're flying z-95
// headhunters they're brave' or something from one of the other missions"; 2026-11-07, its voice: "Especially the z95
// line"): HA2W's taunt of those who fly Z-95s, its words and its voice (no line of the game's says they are brave), a
// radio message of the mission's own, 25 s in, where the mission has a free one (16 at most) and does not say it
// already (HG3M's first does); in a mission with none free the emulator's line instead.  Again when the mission is
// read again (HG4M's after its jump in: the bank, loaded before it, has its voice in that slot still).
const ChallengeLine kBrave = { "", 0, "ha2w", 7, true };
const char kBraveNotice[] = "They're flying Z-95 Headhunters! They're brave...";
void challenge_line(const std::string &m, std::vector<TieVoiceLine> &from, bool again) {
    const int n = (Bit16s)tie_r16(tie::kMissionHeader + 2);
    for (size_t i = 0; i < sizeof(kChallengeLines) / sizeof(kChallengeLines[0]); i++) {
        const ChallengeLine &c = kChallengeLines[i];
        if (m == c.mission && !strcmp(c.from, kBrave.from) && c.fromIndex == kBrave.fromIndex) {
            return;
        }
    }
    const std::string t = n >= 0 && n < 16 ? challenge_text(kBrave) : std::string();
    if (t.empty()) {
        if (!again) {
            notice(kBraveNotice);
        }
        return;
    }
    const Bit32u at = kRadioMsg + (Bit32u)n * 90;
    for (Bit32u k = 0; k < 90; k++) {
        tie_w8(at + k, k < t.size() ? (Bit8u)t[k] : 0);
    }
    // (both triggers "always" (condition 0), and: at the first poll; its delay five polls, 1,180 ticks each: 25 s, past
    // the opening's own lines and new craft alerts, which a line fired with them is lost under: HG3M's at 10 s was)
    tie_w8(at + 88, 5);
    tie_w16(tie::kMissionHeader + 2, (Bit16u)(n + 1));
    from[n].mission = kBrave.from;
    from[n].index = kBrave.fromIndex;
}

// The challenge's voices: each line that has text in the voice of the mission it is from, the bank made from the CD's
// (wcnet_tie_voice.cpp); the game's own test of a slot (its first byte), all 16.
void challenge_voices(const std::string &m, const std::vector<TieVoiceLine> &from) {
    std::vector<TieVoiceLine> lines;
    for (int i = 0; i < 16; i++) {
        if (tie_r8(kRadioMsg + (Bit32u)i * 90)) {
            lines.push_back(from[i]);
        }
    }
    tie_voice_make(m, lines);
}

// The flight's radio voices: the game's bank is the Combat Chamber's row and its mission, from the pilot record at each
// launch (the voices of the messages in order).  In a room a picked mission flies its own (a chamber mission's file
// "h<letter><n>": the letter's index in "FIBAGDM", n - 1); the challenge its own bank, made of the CD's
// (challenge_voices): G and Z (4, 'Z' - '1'), VOICE\GMZ\GMZ.LFD, the name wcnet_tie_voice.cpp answers for; not made
// (no CD), none (7, past the letters, names no bank); a battle's are the tour's, as they are.
void room_voices() {
    std::string m = tie_mission_name();
    for (size_t i = 0; i < m.size(); i++) m[i] = (char)tolower((unsigned char)m[i]);
    if (g_net->challenge) {
        const bool made = tie_voice_made();
        tie_w8(kVoiceA, made ? 4 : 7);
        tie_w8(kVoiceB, (Bit8u)('Z' - '1'));
        wclog(2, "TIE: the Z-95 challenge flies %s", made ? "in its own voices (GMZ)" : "without the radio's voices");
        return;
    }
    static const char kLetters[] = "fibagdm";
    const char *l = m.size() >= 4 && m[0] == 'h' ? strchr(kLetters, m[1]) : NULL;
    if (l && *l && m[2] >= '1' && m[2] <= '9' && !isdigit((unsigned char)m[3])) {
        tie_w8(kVoiceA, (Bit8u)(l - kLetters));
        tie_w8(kVoiceB, (Bit8u)(m[2] - '1'));
    }
}

// The challenge into the mission just read: each flight group's craft mirrored, the player's Z-95s (its spec the
// player's: the Z-95's cockpit comes of it, wcnet_tie_cockpit.cpp), each new craft marked to be loaded.
void make_challenge(bool again) {
    const int numFg = (Bit16s)tie_r16(tie::kMissionHeader), pfg = tie_r8(tie::kPstate + 0x0A);
    int changed = 0;
    for (int i = 0; i < numFg && i < 48; i++) {
        const Bit32u fg = tie::kFlightGroups + (Bit32u)i * tie::kFlightGroupSize;
        const int t = tie_r8(fg + 0x32), to = i == pfg ? kZ95 : mirror_type(t);
        if (!to || to == t) {
            continue;
        }
        tie_w8(fg + 0x32, (Bit8u)to);
        const int species = tie_r8(kSpeciesConvertImage + (Bit32u)to);
        const Bit32u sp = kSpeciesTableImage + (Bit32u)species * 0x16;
        tie_w8(sp + 1, tie_r8(sp + 1) | 0x10);
        if (i == pfg) {
            tie_w8(tie::kPstate + 0x0E, tie_r8(sp + 0x13));
        }
        changed++;
    }
    std::string m;
    for (size_t i = 0; i < g_net->challengeFile.size(); i++) m += (char)tolower((unsigned char)g_net->challengeFile[i]);
    std::vector<TieVoiceLine> from(16);
    for (int i = 0; i < 16; i++) {
        from[i].mission = m;
        from[i].index = i;
    }
    challenge_lines(m, from);       // (again too: a mission read again has its own lines again)
    challenge_line(m, from, again);
    challenge_voices(m, from);
    wclog(1, "TIE: the Z-95 challenge%s: %d of %d flight groups' craft mirrored, ours Z-95s", again ? " (again: the mission read again)" : "",
          changed, numFg);
}

// (Made again at create_createmission when it is gone: a mission that begins with a hyperspace jump is read from its
// file a second time after the jump, its craft not loaded again.)
void make_pairing(bool again) {
    const Bit32u pstate = tie::kPstate;
    const int pfg = tie_r8(pstate + 0x0A), numFg = (Bit16s)tie_r16(tie::kMissionHeader);
    if (pfg >= numFg || pfg >= 48) {
        return;
    }
    const Bit32u fg = tie::kFlightGroups + (Bit32u)pfg * tie::kFlightGroupSize;
    const int type = tie_r8(fg + 0x32), count = tie_r8(fg + 0x33);
    if (count != 1) {
        return;             // (wingmen of its own: each pilot takes one, the seats' way)
    }
    if (type == 6 && ti_flown_as_tfs(tie_mission_name())) {
        tie_fly_group_as(pfg, 5);   // (the seats then give each pilot a T/F of the group)
        g_net->pairedType = 5;
        if (!again) {
            notice("Flown by two: T/Fs for the mission's T/I");
        }
        wclog(1, "TIE: flown by two%s: the mission's T/I a T/F, a T/F for each pilot", again ? " (again: the mission read again)" : "");
        return;
    }
    const int second = paired_type(type);
    if (!second || numFg >= 48) {
        return;
    }
    g_net->pairedType = second;
    const Bit32u to = tie::kFlightGroups + (Bit32u)numFg * tie::kFlightGroupSize;
    for (Bit32u k = 0; k < tie::kFlightGroupSize; k++) {
        tie_w8(to + k, tie_r8(fg + k));
    }
    tie_w8(to + 0x32, (Bit8u)second);
    tie_w8(to + 0x33, (Bit8u)(g_net->seats - 1));
    tie_w8(to + 0x40, 0);                           // (no waves)
    tie_w8(to + 0x42, 0);
    for (int w = 0; w < 12; w++) {                  // (the start points and the route, 0.4 km to the side: 160 a km)
        if (tie_r16(to + 0x102 + 2 * w)) {
            tie_w16(to + 0xA8 + 2 * w, (Bit16u)((Bit16s)tie_r16(to + 0xA8 + 2 * w) + kPairOffset));
        }
    }
    tie_w16(tie::kMissionHeader, (Bit16u)(numFg + 1));
    tie_w16(kFgStatus + (Bit32u)numFg * kFgStatusSize + 6, 0x8000);    // (create_loadmission's own: its place by waypoint)
    g_net->pairedFg = numFg;
    g_net->hostFg = pfg;
    if (!g_net->host) {
        tie_w8(fg + 0x42, 0);
        tie_w8(to + 0x42, (Bit8u)g_net->self);      // (the self'th guest flies its self'th craft)
        tie_w8(pstate + 0x0A, (Bit8u)numFg);        // (the player's group now: its spec becomes the player's below)
    }
    tie_fly_group_as(numFg, second);                // (its craft marked to be loaded)
    wclog(1, "TIE: flown by two%s: the mission's craft type %d, the guests' %d in flight group %d (%d craft)%s", again ? " (again: the mission read again)" : "",
          type, second, numFg, g_net->seats - 1, g_net->host ? "" : ", ours");
    if (!again) {
        notice(g_net->host ? "Flown by two: the guests fly beside you in the craft that pairs with yours"
                           : "Flown by two: you fly beside the host in the craft that pairs with the mission's");
    }
}

void tie_session_before_species() {
    if (!g_net || (!g_net->host && g_net->self < 0) || g_net->seats < 2) {
        return;
    }
    g_net->pairedFg = -1;
    g_net->pairedType = 0;
    if (g_net->challenge) {
        make_challenge(false);      // (before the pairing: the Z-95s pair with themselves)
    }
    room_voices();                  // (before create_createmission loads them)
    make_pairing(false);
}

// The pairing chosen at the species' loading, there still (or made again).
void keep_pairing() {
    if (!g_net->pairedType) {
        return;
    }
    const int numFg = (Bit16s)tie_r16(tie::kMissionHeader);
    if (g_net->pairedFg >= 0) {
        const Bit32u to = tie::kFlightGroups + (Bit32u)g_net->pairedFg * tie::kFlightGroupSize;
        if (numFg == g_net->pairedFg + 1 && tie_r8(to + 0x32) == g_net->pairedType) {
            return;
        }
    } else {
        const int pfg = tie_r8(tie::kPstate + 0x0A);
        if (pfg < numFg && tie_r8(tie::kFlightGroups + (Bit32u)pfg * tie::kFlightGroupSize + 0x32) == g_net->pairedType) {
            return;     // (the T/I's group still T/Fs)
        }
    }
    make_pairing(true);
}

void tie_session_mission_create() {
    if (!g_net) {
        return;
    }
    g_net->fgObjects.clear();
    g_net->playerFg = kPlayerFgNone;
    g_net->clear_flight();
    g_net->endAt = 0;
    g_net->waiting = false;
    g_net->waitCancelled = false;       // (Esc flew this one alone; the next launch waits again)
    if (g_net->challenge && tie_r8(tie::kFlightGroups + (Bit32u)tie_r8(tie::kPstate + 0x0A) * tie::kFlightGroupSize + 0x32) != kZ95) {
        make_challenge(true);           // (the mission read again after a jump: its craft as they were)
    }
    keep_pairing();
    if (g_net->pairedFg >= 0) {
        // (a pairing: the guests have their own flight group, made at the species' loading; the host's stays as it is)
        g_net->playerFg = g_net->host ? tie_r8(tie::kPstate + 0x0A) : g_net->pairedFg;
        g_net->hostCraft = 1;
        wclog(1, "TIE: mission set up, paired: the host's flight group %d, the guests' %d (ours %d)", g_net->hostFg, g_net->pairedFg,
              tie_r8(tie::kPstate + 0x0A));
        return;
    }
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
    g_net->clear_flight();
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
