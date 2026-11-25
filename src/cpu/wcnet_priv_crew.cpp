/*
 *  Privateer's crew (wcnet_priv.h, docs/privateer.md section 30): one ship flown by its pilot with the room's other
 *  players in its turrets.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "pic.h"
#include "dos_inc.h"
#include "dos_system.h"
#include "regs.h"
#include "mem.h"
#include "keyboard.h"
#include <math.h>
#include <vector>

namespace wc {

namespace {

struct OpenFile { std::string name; bool created; unsigned read, written; };
std::map<Bit16u, OpenFile> g_files;         // the open files we follow, by handle entry
int g_fileLog = -1;                         // PRIVFILELOG=1: every file of the game's opened, read, written (a rig's aid)

bool file_log() {
    if (g_fileLog < 0) {
        const char *e = getenv("PRIVFILELOG");
        g_fileLog = e && atoi(e) ? 1 : 0;
    }
    return g_fileLog == 1;
}

// The ship a gunner's flight is flown in: the pilot's, not the gunner's own.  The game writes its ship to PLAYTYPE.IFF
// (an IFF FORM REAL: guns, launchers, shields, armour, the hold and the credits) at every launch, the flight reads the
// file back at once, and the landing reads it once more for the base (measured, PRIVFILELOG=1: created and written in
// pieces; opened and read whole; in flight nothing; at the landing opened and read whole).  The hull it is flown as is
// a byte of the game's data (ds:655E beside the credits at ds:655A: 0 Tarsus, 1 Orion, 2 Centurion, 3 Galaxy; another
// hull's file with the wrong byte is the game's error 6482).  So for a gunner's flight the pilot's file is written over
// the game's before the flight's read, with the pilot's hull byte, and the gunner's own file and byte are put back
// before the landing's read: its ship, hold and credits are its own again at the base.
enum { kHullByte = 0x655E, kCredits = 0x655A, kBaseByte = 0x6556, kBaseSystemByte = 0x6557 };
enum Phase { PHASE_BASE, PHASE_WRITTEN, PHASE_LAUNCH, PHASE_FLIGHT };
struct Ship {
    std::vector<Bit8u> file;
    int hull;
    Ship() : hull(-1) {}
};
Ship g_pilot;                               // the pilot's ship as last told (or a rig's: PRIVCREWTYPE=<file>:<hull>)
Ship g_own;                                 // ours, as the game wrote it at this launch
Phase g_phase = PHASE_BASE;
Bit8u g_drive = 2;                          // the drive PLAYTYPE.IFF is on (the game's own directory)
bool g_ownFlight;                           // this flight is flown in our own ship (no pilot's ship was known at its launch)
Bit32u g_farDump[2];
int g_rigJump;
bool g_rigJumpAsked;
int g_role, g_turret;                       // the room's word: 1 the pilot, 2 a gunner in that turret
Ship g_mine;                                // (the pilot's side) our ship as the game wrote it at our last launch
unsigned g_mineSerial;
unsigned g_pilotSerial;
// The pilot as the room tells of it (a gunner's side).
struct PilotNow {
    bool known, flying;
    std::string system;
    int systemId;
    int base, baseSystem;       // the base it is at or launched from (the game's two bytes), -1 not told
    std::vector<int> path;      // the systems it has flown through since that launch, the launch's first
    bool state;
    double at;
    std::string stateSystem;
    double pos[3], vel[3];
    Bit32s m[9], armor[4], shield[4];
    bool damage, destroyed;
    PilotNow() : known(false), flying(false), systemId(0), base(-1), baseSystem(-1), state(false), at(-1e9), damage(false), destroyed(false) {}
} g_pilotNow;
// (the pilot's side) where we are: the base we are at or launched from, the systems flown through since.
struct OwnWay {
    int base, baseSystem;
    std::vector<int> path;
    OwnWay() : base(-1), baseSystem(-1) {}
} g_way;
int g_ownBase = -1, g_ownBaseSystem = -1;   // (a gunner's) our own base's two bytes, kept while a launch from the pilot's is tried
// A gunner's flight.
struct Flight {
    Bit16u ship;
    double since, viewAt, jumpAt, landAt, toldAt;
    bool joined, firing, killed;
    int systemId;
} g_flight;
// The gunners' turrets as their games tell them (the pilot's side).
struct TurretNow {
    double at;
    Bit32s yaw, pitch, m[9];
    bool fire;
} g_turrets[3];

bool read_whole(Bit8u drive, const char *fullname, std::vector<Bit8u> &out) {
    DOS_File *f = NULL;
    char name[DOS_PATHLENGTH];
    strcpy(name, fullname);
    out.clear();
    if (drive >= DOS_DRIVES || !Drives[drive] || !Drives[drive]->FileOpen(&f, name, OPEN_READ) || !f) return false;
    f->AddRef();                // (a local file is closed only by its last holder)
    Bit8u buf[4096];
    Bit16u n = sizeof(buf);
    if (f->Read(buf, &n)) out.assign(buf, buf + n);
    f->Close();
    delete f;
    return !out.empty();
}

bool write_whole(Bit8u drive, const char *fullname, const std::vector<Bit8u> &in) {
    DOS_File *f = NULL;
    char name[DOS_PATHLENGTH];
    strcpy(name, fullname);
    if (in.empty() || drive >= DOS_DRIVES || !Drives[drive] || !Drives[drive]->FileCreate(&f, name, 0) || !f) return false;
    f->AddRef();
    Bit16u n = (Bit16u)in.size();
    const bool ok = f->Write(const_cast<Bit8u *>(&in[0]), &n) && n == in.size();
    f->Close();
    delete f;
    return ok;
}

void rig_ship_file() {
    static bool looked;
    if (looked) return;
    looked = true;
    const char *e = getenv("PRIVCREWTYPE");
    if (!e || !*e) return;
    std::string path = e;
    const size_t c = path.rfind(':');
    if (c == std::string::npos) return;
    g_pilot.hull = atoi(path.c_str() + c + 1);
    path.resize(c);
    if (FILE *f = fopen(path.c_str(), "rb")) {
        Bit8u buf[4096];
        const size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        g_pilot.file.assign(buf, buf + n);
        wclog(1, "Privateer: crew: a rig's pilot ship %s, %u bytes, hull %d", path.c_str(), (unsigned)n, g_pilot.hull);
    }
}

bool rig_gunner() {
    rig_ship_file();
    return getenv("PRIVCREWTYPE") != NULL;
}
// This launch is a gunner's into the pilot's ship: the pilot flies (its ship told at its launch), or a rig's file.
bool gunner() {
    if (rig_gunner()) return true;
    return g_role == 2 && g_pilotNow.known && g_pilotNow.flying;
}

}  // namespace

void priv_file_opening(const char *fullname, Bit8u drive, Bit8u flags) {
    if (!priv_found() || (flags & 3) != 0) return;
    if (file_log() && strcasecmp(fullname, "PLAYTYPE.IFF") == 0) {
        // (a rig's: who opens it -- the far return addresses up the frames, as paragraphs from the program's load)
        char line[400];
        int at = snprintf(line, sizeof(line), "Privateer: file %s opening from", fullname);
        Bit16u bp = reg_bp;
        const PhysPt stack = SegPhys(ss);
        for (int i = 0; i < 14 && at < (int)sizeof(line) - 12; i++) {
            const Bit16u ip = mem_readw(stack + (Bit16u)(bp + 2)), cs = mem_readw(stack + (Bit16u)(bp + 4));
            at += snprintf(line + at, sizeof(line) - at, " %04X:%04X", (Bit16u)(cs - priv_load_seg()), ip);
            const Bit16u next = mem_readw(stack + bp);
            if (next <= bp) break;
            bp = next;
        }
        wclog(1, "%s", line);
    }
    if (strcasecmp(fullname, "PLAYSCOR.IFF") == 0 && g_phase == PHASE_WRITTEN) {
        // (a launch reads the pilot's score between writing the ship and reading it; a new pilot's registration and a
        // load write the ship and read it back with nothing between: no flight follows those)
        g_phase = PHASE_LAUNCH;
        return;
    }
    if (strcasecmp(fullname, "PLAYTYPE.IFF") != 0) return;
    g_drive = drive;
    if (g_phase == PHASE_WRITTEN) {
        g_phase = PHASE_BASE;
        if (!g_ownFlight) priv_w8(kHullByte, (Bit8u)g_own.hull);       // (no launch after all)
        if (g_ownBase >= 0) {
            priv_w8(kBaseByte, (Bit8u)g_ownBase);
            priv_w8(kBaseSystemByte, (Bit8u)g_ownBaseSystem);
            g_ownBase = -1;
        }
        g_ownFlight = true;
        return;
    }
    if (g_phase == PHASE_LAUNCH) {
        // the launch: the game has just written its own ship, the flight is about to read it
        g_phase = PHASE_FLIGHT;
        g_flight.ship = 0;
        g_flight.joined = false;
        if (g_role == 1) {
            // (the pilot's: our ship as the game has just written it, for the gunners' flights)
            if (read_whole(drive, fullname, g_mine.file)) {
                g_mine.hull = priv_r8(kHullByte);
                g_mineSerial++;
                wclog(1, "Privateer: crew: our ship for the gunners: hull %d, %u bytes (launch %u)", g_mine.hull, (unsigned)g_mine.file.size(), g_mineSerial);
            }
            return;
        }
        if (g_ownFlight) return;
        g_ownFlight = true;
        if (!read_whole(drive, fullname, g_own.file)) {
            priv_w8(kHullByte, (Bit8u)g_own.hull);
            wclog(1, "Privateer: crew: our own ship's file could not be read: flown as it is");
            return;
        }
        if (const char *dir = getenv("PRIVCREWDUMP")) {             // (a rig's: our own ship's file at each launch)
            static int n;
            char path[512];
            snprintf(path, sizeof(path), "%s/own-%d.iff", dir, ++n);
            if (FILE *o = fopen(path, "wb")) { fwrite(&g_own.file[0], 1, g_own.file.size(), o); fclose(o); }
        }
        if (!write_whole(drive, fullname, g_pilot.file)) {
            priv_w8(kHullByte, (Bit8u)g_own.hull);
            wclog(1, "Privateer: crew: the pilot's ship could not be written over %s: flown in our own", fullname);
            write_whole(drive, fullname, g_own.file);
            return;
        }
        g_ownFlight = false;
        wclog(1, "Privateer: crew: this flight is flown in the pilot's ship (hull %d, %u bytes; ours hull %d, %u bytes, kept)", g_pilot.hull,
              (unsigned)g_pilot.file.size(), g_own.hull, (unsigned)g_own.file.size());
        return;
    }
}

// The game builds the base's ship (ovr273:0000(base ship near, hull, the flight's records far), cdecl far, entered by
// its thunk stub273:0020): for a new pilot and a loaded game from PLAYTYPE.IFF (records 0: ovr269:4B05 reads the
// file), at a landing from the ship just flown (records: ovr269:49F0 takes the guns, the shields and armour, the hold
// and the credits from the flight's objects; it opens the file only to see that it is one).  A gunner's flight was
// flown in the pilot's ship: at its landing the call is turned into the file's kind -- our own ship's file written
// back over PLAYTYPE.IFF, the hull argument and the hull byte our own, the records 0 -- and the base has our own
// ship, hold and credits as they were at the launch (hull byte late or the records kept: the game's error 7022).
void priv_crew_ship_load() {
    const PhysPt sp = SegPhys(ss) + reg_sp;
    const Bit16u self = mem_readw(sp + 4);
    const Bit8u hull = mem_readb(sp + 6);
    const Bit32u records = mem_readd(sp + 8);
    if (file_log()) {
        wclog(1, "Privateer: crew: the base's ship built: object ds:%04X, hull %u, flight records %04X:%04X, from %04X:%04X (%s)", self, hull,
              (Bit16u)(records >> 16), (Bit16u)records, (Bit16u)(mem_readw(sp + 2) - priv_load_seg()), mem_readw(sp), priv_in_flight() ? "in flight" : "not in flight");
    }
    if (g_phase != PHASE_FLIGHT) return;
    g_phase = PHASE_BASE;
    if (g_ownFlight || g_own.file.empty() || !records) return;
    g_ownFlight = true;
    const bool ok = write_whole(g_drive, "PLAYTYPE.IFF", g_own.file);
    if (!ok) {
        wclog(1, "Privateer: crew: landed: OUR OWN SHIP'S FILE COULD NOT BE WRITTEN: the base has the pilot's ship");
        return;
    }
    priv_w8(kHullByte, (Bit8u)g_own.hull);
    mem_writeb(sp + 6, (Bit8u)g_own.hull);
    mem_writed(sp + 8, 0);
    wclog(1, "Privateer: crew: landed: the base's ship is our own again (hull %d, %u bytes)", g_own.hull, (unsigned)g_own.file.size());
}

bool priv_crew_script_item(const std::string &item, double t) {
    if (item.compare(0, 8, "!privw8=") == 0 && priv_found()) {
        // !privw8=<offset>:<value> (hex): a byte of the game's data segment written (a rig's aid)
        unsigned off = 0, v = 0;
        if (sscanf(item.c_str() + 8, "%x:%x", &off, &v) == 2) {
            wclog(1, "key script %.1fs: ds:%04X %02X -> %02X", t, off, priv_r8((Bit16u)off), v);
            priv_w8((Bit16u)off, (Bit8u)v);
        }
        return true;
    }
    if (item == "!privkillme" && priv_found()) {
        // !privkillme: our own ship destroyed (the game's own death; a rig's aid)
        wclog(1, "key script %.1fs: our own ship killed", t);
        if (priv_in_flight()) priv_ship_kill(priv_player_ship());
        return true;
    }
    if (item.compare(0, 9, "!privjump") == 0 && priv_found()) {
        // !privjump[=<system id>]: our ship put at a jump point of the system (the one to that system; the first without)
        // and J pressed, at the next frame (a rig's aid)
        g_rigJump = item.size() > 10 ? atoi(item.c_str() + 10) : 0;
        g_rigJumpAsked = true;
        return true;
    }
    if (item.compare(0, 9, "!privfar=") == 0 && priv_found()) {
        // !privfar=<seg>:<off>:<count> (hex, hex, decimal): far bytes, read at the next frame (a rig's aid); seg "obj"
        // = the far pointer at that offset of the data segment
        unsigned seg = 0, off = 0, n = 32;
        if (item.compare(9, 4, "obj:") == 0) {
            sscanf(item.c_str() + 13, "%x:%u", &off, &n);
            g_farDump[0] = 0x10000 | off;
        } else {
            sscanf(item.c_str() + 9, "%x:%x:%u", &seg, &off, &n);
            g_farDump[0] = ((Bit32u)seg << 4) + off;
            g_farDump[0] |= 0x80000000u;
        }
        g_farDump[1] = n > 96 ? 96 : n;
        return true;
    }
    if (item.compare(0, 8, "!privr8=") == 0 && priv_found()) {
        // !privr8=<offset>:<count> (hex, decimal): bytes of the game's data segment
        unsigned off = 0, n = 1;
        sscanf(item.c_str() + 8, "%x:%u", &off, &n);
        char hex[3 * 64 + 1] = "";
        for (unsigned i = 0; i < n && i < 64; i++) snprintf(hex + 3 * i, 4, "%02X ", priv_r8((Bit16u)(off + i)));
        wclog(1, "key script %.1fs: ds:%04X: %s", t, off, hex);
        return true;
    }
    return false;
}

bool priv_crew_gunner() { return g_role == 2; }
bool priv_crew_pilot() { return g_role == 1; }

void priv_crew_role(int role, int turret) {
    if (role != g_role || turret != g_turret) {
        wclog(1, "Privateer: crew: %s", role == 1 ? "we are the pilot: the others man our turrets" :
              role == 2 ? (turret == 1 ? "we are the gunner of turret 1" : turret == 2 ? "we are the gunner of turret 2" : "we ride as a passenger") :
              "no crew: everybody flies their own ship");
    }
    g_role = role;
    g_turret = turret;
    if (role != 2) g_pilotNow = PilotNow();
}

unsigned priv_crew_own_ship(std::string *file, int *hull) {
    if (g_role != 1 || !g_mineSerial || g_mine.file.empty()) return 0;
    if (file) file->assign((const char *)&g_mine.file[0], g_mine.file.size());
    if (hull) *hull = g_mine.hull;
    return g_mineSerial;
}

void priv_crew_take_ship(const std::string &file, int hull, unsigned serial) {
    if (g_role != 2 || file.empty() || file.size() > 4096 || hull < 0 || hull > 3) return;
    g_pilot.file.assign(file.begin(), file.end());
    g_pilot.hull = hull;
    g_pilotSerial = serial;
    wclog(1, "Privateer: crew: the pilot's ship told: hull %d, %u bytes (its launch %u)", hull, (unsigned)file.size(), serial);
}

void priv_crew_pilot_presence(bool flying, const std::string &system, int systemId, int base, int baseSystem, const std::vector<int> &path) {
    if (g_role != 2) return;
    PilotNow &p = g_pilotNow;
    if (!p.known || p.flying != flying || p.system != system || p.base != base || p.path.size() != path.size()) {
        std::string way;
        for (size_t i = 0; i < path.size(); i++) way += (i ? " > " : "") + std::to_string(path[i]);
        wclog(1, "Privateer: crew: the pilot %s%s (system id %d; its base %d of system %d; its way since: %s)", flying ? "flies in " : "is at a base",
              flying ? system.c_str() : "", systemId, base, baseSystem, way.empty() ? "-" : way.c_str());
    }
    p.known = true;
    if (!flying) {
        p.state = false;
        p.destroyed = false;
    }
    p.flying = flying;
    p.system = system;
    p.systemId = systemId;
    p.base = base;
    p.baseSystem = baseSystem;
    p.path = path;
}

void priv_crew_own_way(int *base, int *baseSystem, std::vector<int> *path) {
    *base = g_way.base;
    *baseSystem = g_way.baseSystem;
    *path = g_way.path;
}

void priv_crew_pilot_state(const std::string &system, const double pos[3], const double vel[3], const Bit32s m[9], const Bit32s armor[4],
                           const Bit32s shield[4], bool damage, bool destroyed) {
    if (g_role != 2) return;
    PilotNow &p = g_pilotNow;
    if (destroyed) {
        p.destroyed = true;
        p.state = false;
        return;
    }
    p.state = true;
    p.at = PIC_FullIndex();
    p.stateSystem = system;
    for (int i = 0; i < 3; i++) { p.pos[i] = pos[i]; p.vel[i] = vel[i]; }
    for (int i = 0; i < 9; i++) p.m[i] = m[i];
    p.damage = damage;
    for (int i = 0; i < 4; i++) { p.armor[i] = armor[i]; p.shield[i] = shield[i]; }
    // (a state is a flying pilot's, whatever its last presence said: presences come every 15 s and on occasions)
    if (!p.known || !p.flying || p.system != system) {
        p.known = true;
        p.flying = true;
        p.system = system;
    }
}

namespace {

double dist2(const PrivPlace &p, const Bit32s pos[3]) {
    const double dx = (double)p.x - pos[0], dy = (double)p.y - pos[1], dz = (double)p.z - pos[2];
    return dx * dx + dy * dy + dz * dz;
}

// Our ship put at the nearest base of the system: the game lands it there (its own landing, the base's screens after).
bool land_nearest(const PrivPlace pl[], int n, const Bit32s pos[3], const char *why) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (pl[i].kind == 1 && (best < 0 || dist2(pl[i], pos) < dist2(pl[best], pos))) best = i;
    }
    if (best < 0) return false;
    const Bit32s at[3] = { pl[best].x, pl[best].y, pl[best].z };
    priv_ship_set(priv_player_ship(), at, NULL);
    wclog(1, "Privateer: crew: put at the base of sphere %d to land (%s)", pl[best].id, why);
    return true;
}

// Our ship taken after the pilot: the next system of the pilot's way after ours, by our system's jump point that leads
// there -- our ship put at it and J pressed: the game's own jump (the destination is the jump point's own; a record's
// byte written did not change it, measured).  False: no such way from here.
bool jump_after(const PrivPlace pl[], int n, int ours, const std::vector<int> &path, int *nextOut) {
    int next = 0;
    for (size_t i = 0; i + 1 < path.size(); i++) {
        if (path[i] == ours) next = path[i + 1];        // (the last time the pilot left this system)
    }
    *nextOut = next;
    if (next <= 0) return false;
    for (int i = 0; i < n; i++) {
        if (pl[i].kind != 2 || pl[i].what != next) continue;
        const Bit32s at[3] = { pl[i].x, pl[i].y, pl[i].z };
        priv_ship_set(priv_player_ship(), at, NULL);
        priv_tap_key(KBD_j);
        wclog(1, "Privateer: crew: put at the jump point to system %d (sphere %d) and J pressed: after the pilot", next, pl[i].id);
        return true;
    }
    return false;
}

void gunner_frame(double now) {
    const Bit16u ship = priv_player_ship();
    Flight &f = g_flight;
    if (ship != f.ship) {
        // (a new ship object: a launch -- joined cleared there -- or a jump's new scene)
        f.ship = ship;
        f.since = now;
        f.viewAt = f.jumpAt = f.landAt = f.toldAt = -1e9;
        f.killed = false;
    }
    Bit32s pos[3];
    priv_player_pos(pos);
    PrivPlace pl[48];
    const int n = priv_places(pl, 48);
    f.systemId = n ? pl[0].system : 0;
    const bool rig = rig_gunner() && g_role != 2;
    if (g_ownFlight && !rig) {
        // launched in our own ship (the pilot was not flying): no flight of a gunner's -- back to the base
        if (now - f.since > 2000 && now - f.landAt > 6000) {
            f.landAt = now;
            land_nearest(pl, n, pos, "the pilot has not launched");
            priv_session_notice("Your pilot has not launched: launch after the pilot does");
        }
        return;
    }
    // the seat: our turret's guns ours, every other gun off; the view our turret's
    Bit32s yaw, pitch;
    const int seat = (g_turret == 1 || g_turret == 2 || rig) && priv_turret(rig ? 1 : g_turret, &yaw, &pitch, NULL) ? (rig ? 1 : g_turret) : 0;
    priv_guns_seat(ship, seat ? seat : -1);
    if (seat && priv_view() != 3 + seat && now - f.viewAt > 800 && now - f.since > 500) {
        f.viewAt = now;
        priv_tap_key(seat == 1 ? KBD_f5 : KBD_f6);
    }
    const PilotNow &p = g_pilotNow;
    if (rig || !p.known) return;
    if (p.destroyed) {
        if (!f.killed && priv_ship_is_alive(ship)) {
            f.killed = true;
            priv_ship_kill(ship);
            wclog(1, "Privateer: crew: the pilot's ship was destroyed: ours with it");
        }
        return;
    }
    if (!p.flying) {
        // the pilot landed: so do we, at the base we are at (its ship's place was ours)
        if (now - f.landAt > 6000) {
            f.landAt = now;
            land_nearest(pl, n, pos, "the pilot landed");
        }
        return;
    }
    const std::string system = priv_system_name();
    if (!p.system.empty() && !system.empty() && p.system != system) {
        if (now - f.jumpAt > 12000 && now - f.since > 1500) {
            int next = 0;
            if (jump_after(pl, n, f.systemId, p.path, &next)) {
                f.jumpAt = now;
            } else if (now - f.toldAt > 3000) {
                // (the pilot's way is told a moment after its new system's name: looked at again at the next frame)
                f.toldAt = now;
                wclog(1, "Privateer: crew: no way yet from %s (system %d) to the pilot's %s (the next system of its way: %d)", system.c_str(), f.systemId,
                      p.system.c_str(), next);
            }
        }
        return;
    }
    if (!p.state || p.stateSystem != system || now - p.at > 2000) return;
    const double t = (now - p.at < 250 ? now - p.at : 250) / 1000.0;
    const Bit32s at[3] = { (Bit32s)(p.pos[0] + p.vel[0] * t), (Bit32s)(p.pos[1] + p.vel[1] * t), (Bit32s)(p.pos[2] + p.vel[2] * t) };
    priv_ship_set(ship, at, p.m);
    if (p.damage) {
        priv_ship_set_armor(ship, p.armor);
        priv_ship_set_shields(ship, p.shield);
    }
    if (!f.joined) {
        f.joined = true;
        wclog(1, "Privateer: crew: aboard the pilot's ship in %s (%s)", system.c_str(), seat ? "in our turret" : "no turret of ours on it: a passenger");
    }
}

void pilot_frame(double now) {
    // (our way since the launch: the systems' ids as the sphere records have them, far data read here at the frame)
    PrivPlace one[2];
    const int id = priv_places(one, 2) ? one[0].system : 0;
    g_flight.systemId = id;
    if (id > 0 && (g_way.path.empty() || g_way.path.back() != id) && g_way.path.size() < 48) {
        g_way.path.push_back(id);
        priv_session_event(6);      // (told at once: the gunners follow)
    }
    for (int k = 1; k <= 2; k++) {
        TurretNow &t = g_turrets[k];
        if (now - t.at > 400 || now < t.at) continue;
        if (priv_view() != 3 + k) priv_turret_set(k, t.yaw, t.pitch, t.m);      // (not while the pilot sits in it)
        if (t.fire && now - t.at < 250) priv_fire_station(k);
    }
}

}  // namespace

void priv_crew_frame() {
    if (!priv_in_flight()) return;
    if (g_rigJumpAsked) {
        g_rigJumpAsked = false;
        PrivPlace pl[48];
        const int n = priv_places(pl, 48);
        std::string all;
        int pick = -1;
        for (int i = 0; i < n; i++) {
            if (pl[i].kind != 2) continue;
            all += " " + std::to_string(pl[i].what);
            if (pick < 0 || (g_rigJump && pl[i].what == g_rigJump && pl[pick].what != g_rigJump)) pick = i;
        }
        if (pick >= 0) {
            const Bit32s at[3] = { pl[pick].x, pl[pick].y, pl[pick].z };
            priv_ship_set(priv_player_ship(), at, NULL);
            priv_tap_key(KBD_j);
        }
        wclog(1, "Privateer: a rig's jump: system %d's jump points lead to%s: %s", n ? pl[0].system : 0, all.c_str(),
              pick >= 0 ? ("put at the one to " + std::to_string(pl[pick].what) + ", J pressed").c_str() : "none");
    }
    if (g_farDump[1]) {
        PhysPt at = g_farDump[0] & 0x7fffffff;
        if (!(g_farDump[0] & 0x80000000u)) {
            const Bit16u o = (Bit16u)g_farDump[0];
            at = ((PhysPt)priv_r16((Bit16u)(o + 2)) << 4) + priv_r16(o);
        }
        char hex[3 * 96 + 1] = "";
        for (unsigned i = 0; i < g_farDump[1]; i++) snprintf(hex + 3 * i, 4, "%02X ", mem_readb(at + i));
        wclog(1, "Privateer: far %05X: %s", at, hex);
        g_farDump[1] = 0;
    }
    const double now = PIC_FullIndex();
    if (g_role == 1) pilot_frame(now);
    else if (g_role == 2 || rig_gunner()) gunner_frame(now);
}

int priv_crew_system_id() {
    return priv_in_flight() ? g_flight.systemId : 0;        // (as the last frame read it)
}

bool priv_crew_turret_report(int *turret, Bit32s *yaw, Bit32s *pitch, Bit32s m[9], bool *fire) {
    if (g_role != 2 || !priv_in_flight() || g_ownFlight || !g_flight.joined) return false;
    if (g_turret < 1 || g_turret > 2 || !priv_turret(g_turret, yaw, pitch, m)) return false;
    *turret = g_turret;
    *fire = g_flight.firing && priv_view() == 3 + g_turret;
    return true;
}

void priv_crew_take_turret(int turret, Bit32s yaw, Bit32s pitch, const Bit32s m[9], bool fire) {
    if (g_role != 1 || turret < 1 || turret > 2) return;
    TurretNow &t = g_turrets[turret];
    t.at = PIC_FullIndex();
    t.yaw = yaw;
    t.pitch = pitch;
    for (int i = 0; i < 9; i++) t.m[i] = m[i];
    t.fire = fire;
}

bool priv_crew_key(int kbdKey, bool pressed) {
    if (!pressed || priv_tapping() || !priv_in_flight() || g_ownFlight || (g_role != 2 && !rig_gunner())) return false;
    switch (kbdKey) {
    case KBD_a: case KBD_j: case KBD_x: case KBD_enter: case KBD_tab:
    case KBD_f1: case KBD_f2: case KBD_f3: case KBD_f4: case KBD_f5: case KBD_f6: case KBD_f7: case KBD_f8: case KBD_f9:
        // the autopilot, the jump, the hold's ejection, the missiles and the afterburner are the pilot's; the view is
        // our turret's (a passenger looks where it likes)
        if (kbdKey >= KBD_f1 && kbdKey <= KBD_f4 && !(g_turret >= 1 && g_turret <= 2 && priv_turret(g_turret, NULL, NULL, NULL))) return false;
        return true;
    default:
        return false;
    }
}

void priv_crew_controls(Bit32u rec) {
    if (g_ownFlight || (g_role != 2 && !rig_gunner())) return;
    const Bit8u b = mem_readb(rec + 0x1E);
    g_flight.firing = (b & 0x04) != 0;
    if (b & 0x02) mem_writeb(rec + 0x1E, (Bit8u)(b & ~0x02));       // (no missile is a gunner's to launch)
}

bool priv_crew_no_wings() { return g_role == 2; }

bool priv_crew_hit(Bit16u target, Bit16u source) {
    (void)source;
    // (a gunner's copy of the ship: the pilot's game decides what hits it; its shields and armour are told)
    return g_role == 2 && !g_ownFlight && target && target == priv_player_ship();
}

void priv_crew_tick() {
    if (g_role == 1 && priv_found() && !priv_in_flight()) {
        // (the pilot's, at a base: the game's own two bytes of where it is; a flight's way begins anew)
        if (priv_r8(kBaseByte) != 0xFF) {       // (0xFF: no base -- the title after a death, until a game is loaded)
            g_way.base = priv_r8(kBaseByte);
            g_way.baseSystem = priv_r8(kBaseSystemByte);
        } else {
            g_way.base = g_way.baseSystem = -1;
        }
        g_way.path.clear();
    }
}

void priv_file_opened(const char *fullname, Bit16u entry, bool created) {
    if (!priv_found()) return;
    OpenFile f;
    f.name = fullname;
    f.created = created;
    f.read = f.written = 0;
    g_files[entry] = f;
    if (created && strcasecmp(fullname, "PLAYTYPE.IFF") == 0 && !priv_in_flight() && g_phase == PHASE_BASE) {
        // the game begins to write its ship: a launch (or a registration, or a load: PHASE_WRITTEN's read tells).  The
        // hull byte is set now: the flight's set-up reads it before it reads the ship's file (set at that read, or at
        // the score's read before it, the game's error 6482)
        g_ownFlight = true;
        bool loading = false;
        for (std::map<Bit16u, OpenFile>::const_iterator k = g_files.begin(); k != g_files.end(); ++k) {
            const std::string &n = k->second.name;
            if (n.size() > 4 && (!strcasecmp(n.c_str() + n.size() - 4, ".SAV") || !strcasecmp(n.c_str() + n.size() - 4, ".PRS"))) loading = true;
        }
        if (!loading && gunner() && !g_pilot.file.empty() && g_pilot.hull >= 0 && g_pilot.hull <= 3) {
            g_own.hull = priv_r8(kHullByte);
            priv_w8(kHullByte, (Bit8u)g_pilot.hull);
            g_ownFlight = false;
        }
        g_ownBase = -1;
        if (!loading && g_role == 2 && g_pilotNow.known && g_pilotNow.base >= 0 && g_pilotNow.baseSystem > 0 &&
            (g_pilotNow.base != priv_r8(kBaseByte) || g_pilotNow.baseSystem != priv_r8(kBaseSystemByte))) {
            // The launch begins where the pilot is, or set out from: the game's own two bytes of where it is (the base,
            // its system: ds:6556, ds:6557 -- a new pilot's game at Achilles with Oxford's two launched over Oxford,
            // measured), which the landing writes again.
            g_ownBase = priv_r8(kBaseByte);
            g_ownBaseSystem = priv_r8(kBaseSystemByte);
            priv_w8(kBaseByte, (Bit8u)g_pilotNow.base);
            priv_w8(kBaseSystemByte, (Bit8u)g_pilotNow.baseSystem);
            wclog(1, "Privateer: crew: this launch is from the pilot's base (base %d of system %d; ours was base %d of system %d)", g_pilotNow.base,
                  g_pilotNow.baseSystem, g_ownBase, g_ownBaseSystem);
        }
    }
}

void priv_file_read(Bit16u entry, Bit8u *data, Bit16u amount) {
    (void)data;
    std::map<Bit16u, OpenFile>::iterator it = g_files.find(entry);
    if (it == g_files.end() || !priv_found()) return;
    it->second.read += amount;
}

void priv_file_written(Bit16u entry, const Bit8u *data, Bit16u amount) {
    (void)data;
    std::map<Bit16u, OpenFile>::iterator it = g_files.find(entry);
    if (it == g_files.end() || !priv_found()) return;
    it->second.written += amount;
}

void priv_file_closed(Bit16u entry) {
    std::map<Bit16u, OpenFile>::iterator it = g_files.find(entry);
    if (it == g_files.end()) return;
    const OpenFile &f = it->second;
    if (file_log() && priv_found() && f.name.find(".TRE") == std::string::npos && f.name.find("PRCD.EXE") == std::string::npos) {
        wclog(1, "Privateer: file %s %s, %u read, %u written, closed (%s)", f.name.c_str(), f.created ? "created" : "opened", f.read, f.written,
              priv_in_flight() ? "in flight" : "not in flight");
    }
    if (priv_found() && f.created && f.written && strcasecmp(f.name.c_str(), "PLAYTYPE.IFF") == 0 && !priv_in_flight()) {
        // (a load writes it too, from the saved game, and reads it back: that read finds no pilot's ship wanted --
        // told from a launch's by the saved game still being open)
        bool loading = false;
        for (std::map<Bit16u, OpenFile>::const_iterator k = g_files.begin(); k != g_files.end(); ++k) {
            const std::string &n = k->second.name;
            if (n.size() > 4 && (!strcasecmp(n.c_str() + n.size() - 4, ".SAV") || !strcasecmp(n.c_str() + n.size() - 4, ".PRS"))) loading = true;
        }
        g_phase = loading ? PHASE_BASE : PHASE_WRITTEN;
    }
    g_files.erase(it);
}

}  // namespace wc
