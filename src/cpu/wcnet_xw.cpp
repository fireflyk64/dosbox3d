/*
 *  X-Wing Collector's CD-ROM: finding BWING.EXE and its flight overlay, and watching them (wcnet_xw.h, docs/xwing.md).
 *
 *  BWING.EXE is loaded by DOS (XWINGCD.BAT), so its load segment is known from the start.  BFLIGHT.OVL, the flight
 *  (.RTLink/Plus: 1,855 far pointers to relocate, then the code), is loaded when a mission is flown at a fixed place:
 *  its file offset F at linear (load << 4) + F + 0x7500, its first code segment the load segment + 0920.  The places
 *  below are paragraphs from the load segment, found by lining the TIE Fighter demo's functions up with this build's
 *  (the same code, instruction for instruction, for most: docs/xwing.md section 3).  Each place's first bytes are
 *  checked when it comes up: the front end's overlay sits at the same addresses between flights.
 *
 *  The places watched (each an IP, its CS checked):
 *    1722:000A  create_createmission(path)'s entry: the launch (held in the PSP's unused FCB bytes until the room is
 *               there); a guest's path made the host's
 *    1722:00D0  ... its mission file read (the header at DS:C72C, the flight groups in the mission segment), its
 *               first random number next: the host's seed, the seats in the player's flight group written
 *    0920:06AE  the flight's frame: its top
 *    0DFD:000A  move_moveobjects's entry: the others' craft written in; our calls into the program made here
 *    19E8:0B0E  laser_createprojectile(shooter, hardpoint, type)
 *    0E65:1A84  collide_laserhitcraft(bolt, target, offset)
 *    0E65:1C52  collide_damagecraft(target, component, attacker, side)
 *  and 0E65:2C16 collide_updatekills(shooter, victim, 1) is called.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include "wcnet_xw.h"
#include "wcnet_log.h"
#include "wcnet_game.h"
#include "wc_net.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"
#include "cpu.h"
#include "lazyflags.h"
#include "dos_inc.h"
#include "keyboard.h"

namespace wc {

namespace {

// The program's places (paragraph from the load segment, offset) and the bytes each begins with
struct Place { Bit16u para, off; int n; Bit8u bytes[8]; };
const Place kCreateMission = { 0x1722, 0x000A, 8, { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x88, 0x00, 0x57 } };     // create_createmission
const Place kMissionRead = { 0x1722, 0x00D0, 3, { 0x9A, 0x23, 0xF7 } };                                    // (lcall the random number: 3 bytes checked)
const Place kDoFrame = { 0x0920, 0x06AE, 8, { 0x83, 0x3E, 0x78, 0xD3, 0x00, 0x74, 0x25, 0xB8 } };          // the frame
const Place kMoveObjects = { 0x0DFD, 0x000A, 8, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x57, 0x56 } };      // move_moveobjects
const Place kCreateProjectile = { 0x19E8, 0x0B0E, 8, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x57, 0x56 } }; // laser_createprojectile
const Place kLaserHit = { 0x0E65, 0x1A84, 8, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x57, 0x56 } };         // collide_laserhitcraft
const Place kDamage = { 0x0E65, 0x1C52, 8, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x57, 0x56 } };           // collide_damagecraft
// The flight's start (0920:0006) makes the mission (0920:011D), and when the player's flight group comes in through
// hyperspace flies the jump as a scene (the frames run with DS:E62E set) and makes the mission AGAIN (0920:0197, back
// at 019C); 0920:019F is next either way.  The room's launch is the last making: the first passes as it is, and where
// the game would not make it again (no jump) it is sent back to do so (0920:0193: push the path, the call).
const Place kAfterCreate = { 0x0920, 0x019F, 5, { 0x9A, 0x1A, 0x08, 0xC4, 0x18 } };
const Bit16u kCreateAgain = 0x0193, kSecondCreateReturn = 0x019C;
const Bit16u kSceneFlag = 0xE62E;       // (a byte: the jump's scene is being flown)
const Place kFlightOver = { 0x0920, 0x04B8, 7, { 0x80, 0x3E, 0x5A, 0x6F, 0x0A, 0x72, 0x0F } };   // the frames over, the status not yet looked at
const Place kUpdateKills = { 0x0E65, 0x2C16, 8, { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x57, 0x56 } };      // collide_updatekills
enum {
    kDgroupPara = 0x3F1C,
    kStubOff = 0x5C,            // the launch hold's idle: "jmp $" written in the PSP's first FCB (unused by the program)
};

bool g_found = false;
Bit16u g_load = 0, g_psp = 0, g_ds = 0;
double g_nextCheckMs = 0;
// the flight's frames
Bit32u g_frame = 0;
double g_lastFrameMs = -1e9, g_frameMs = 50, g_rateSince = 0;
Bit32u g_rateFrames = 0;
bool g_flightSaid = false;
// the launch's hold (create_createmission's entry).  A hold let go jumps back to its place, whose hook may or may not
// come up for that first instruction: `released` says so if it does, and is cleared at the mission read in any case.
bool g_holding = false, g_released = false;
bool g_real = false;            // the mission being made is the one flown (the second making)
Bit16u g_pathAt = 0;            // the mission path's place in DS (create_createmission's argument), at the launch
unsigned g_shotsMade, g_ourShots;
// (the aid !xwahead=<object>:<units>[:<seconds>]: that object held so far ahead of our craft's nose, every frame before
// the moves)
int g_aheadObj = -1;
Bit32s g_aheadUnits = 0;
double g_aheadUntil = 0;

inline PhysPt dsa(Bit16u off) { return ((PhysPt)g_ds << 4) + off; }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline Bit16u obj_at(int n) { return (Bit16u)(xw::kObjects + n * xw::kObjectSize); }
inline Bit16u seg_of(const Place &p) { return (Bit16u)(g_load + p.para); }
inline bool at(const Place &p, Bit32u ip) { return ip == p.off && SegValue(cs) == seg_of(p); }
bool bytes_at(const Place &p) {
    const PhysPt a = ((PhysPt)seg_of(p) << 4) + p.off;
    for (int i = 0; i < p.n; i++) {
        if (mem_readb(a + (PhysPt)i) != p.bytes[i]) return false;
    }
    return true;
}

bool ours_running() { return g_found && dos.psp() == g_psp; }

// --- the flight's frames -------------------------------------------------------------------------------------------

void frame_top() {
    const double now = PIC_FullIndex();
    if (now - g_lastFrameMs > 1000 && mem_readb(dsa(kSceneFlag)) == 0) {
        g_frame = 0;            // (a flight begun)
        g_rateSince = now;
        g_rateFrames = 0;
        g_flightSaid = true;
        wclog(1, "X-Wing: the flight's frames are coming (our object %d, DS %04X%s)", xw_player_object(), (unsigned)SegValue(ds),
              SegValue(ds) == g_ds ? "" : ": NOT the data segment expected");
        hooks_flight_began();
    }
    if (mem_readb(dsa(kSceneFlag)) != 0) return;       // (the jump's scene: no flight yet)
    g_lastFrameMs = now;
    g_frame++;
    if (++g_rateFrames >= 20) {
        g_frameMs = (now - g_rateSince) / g_rateFrames;
        g_rateSince = now;
        g_rateFrames = 0;
    }
    xw_session_frame();
}

// --- calls into the program (the TIE demo's tdm_queue_call) ---------------------------------------------------------

struct Call {
    XwFn fn;
    int nargs;
    Bit16u args[4];
    XwCallBefore before;
    XwCallDone done;
    int tag;
};
std::deque<Call> g_calls;
bool g_inCall = false;
Call g_cur;
Bit16u g_retSp = 0;             // SP once the call has returned to the place (its arguments still on the stack)
struct Saved {
    Bit32u eax, ebx, ecx, edx, esi, edi, ebp, esp, flags;
    Bit16u ds, es;
} g_saved;
unsigned g_callsMade = 0;

const Place &place_of(XwFn fn) {
    switch (fn) {
    case XW_CREATE_PROJECTILE: return kCreateProjectile;
    case XW_LASER_HIT: return kLaserHit;
    case XW_DAMAGE: return kDamage;
    default: return kUpdateKills;
    }
}

void save_state() {
    FillFlags();
    g_saved.eax = reg_eax; g_saved.ebx = reg_ebx; g_saved.ecx = reg_ecx; g_saved.edx = reg_edx;
    g_saved.esi = reg_esi; g_saved.edi = reg_edi; g_saved.ebp = reg_ebp; g_saved.esp = reg_esp;
    g_saved.flags = reg_flags;
    g_saved.ds = SegValue(ds);
    g_saved.es = SegValue(es);
}

void restore_state() {
    reg_eax = g_saved.eax; reg_ebx = g_saved.ebx; reg_ecx = g_saved.ecx; reg_edx = g_saved.edx;
    reg_esi = g_saved.esi; reg_edi = g_saved.edi; reg_ebp = g_saved.ebp; reg_esp = g_saved.esp;
    reg_flags = g_saved.flags;
    lflags.type = t_UNKNOWN;
    SegSet16(ds, g_saved.ds);
    SegSet16(es, g_saved.es);
}

// The next call made from the place (move_moveobjects's entry, the state saved): true when one is under way.
bool start_next_call() {
    while (!g_calls.empty()) {
        g_cur = g_calls.front();
        g_calls.pop_front();
        restore_state();
        if (g_cur.before && !g_cur.before(g_cur.tag, g_cur.args)) continue;
        for (int i = g_cur.nargs - 1; i >= 0; i--) CPU_Push16(g_cur.args[i]);
        g_retSp = (Bit16u)reg_esp;
        CPU_Push16(seg_of(kMoveObjects));
        CPU_Push16(kMoveObjects.off);
        const Place &fn = place_of(g_cur.fn);
        SegSet16(cs, seg_of(fn));
        reg_eip = fn.off;
        g_inCall = true;
        g_callsMade++;
        return true;
    }
    restore_state();
    g_inCall = false;
    return false;
}

// At move_moveobjects's entry: a call of ours come back, or the frame's own arrival.
void at_move_objects() {
    if (g_inCall) {
        if ((Bit16u)reg_esp != g_retSp) {
            wclog(1, "X-Wing: a call came back to move_moveobjects with SP %04X, not %04X: the calls given up", (unsigned)(reg_esp & 0xFFFF),
                  g_retSp);
            g_calls.clear();
            restore_state();
            g_inCall = false;
            return;
        }
        if (g_cur.done) g_cur.done(g_cur.tag, (Bit16u)reg_eax);
        start_next_call();          // (the next, or the frame goes on into move_moveobjects)
        return;
    }
    xw_session_before_move();
    if (g_aheadObj >= 0 && PIC_FullIndex() < g_aheadUntil && xw_player_object() >= 0) {
        const PhysPt me = xw_obj(xw_player_object()), it = xw_obj(g_aheadObj);
        for (int a = 0; a < 3; a++) {
            const Bit32s d = (Bit32s)(((Bit64s)g_aheadUnits * (Bit16s)mem_readw(me + xw::kOAxes + 2 * a)) / 32767);
            mem_writed(it + xw::kOPos + 4 * a, (Bit32u)((Bit32s)mem_readd(me + xw::kOPos + 4 * a) + d));
        }
    }
    if (!g_calls.empty()) {
        save_state();
        start_next_call();
    }
}

// --- the launch: held at create_createmission's entry -------------------------------------------------------------

void at_create_mission() {
    g_pathAt = xw_arg(0);
    const Bit16u back = mem_readw(((PhysPt)SegValue(ss) << 4) + (Bit16u)(reg_esp & 0xFFFF));
    if (!g_holding && !g_released) g_real = back == kSecondCreateReturn;
    if (!g_real) {
        wclog(1, "X-Wing: the mission made a first time (%s): not the launch yet", xw_mission_path().c_str());
        return;
    }
    if (g_released) {
        g_released = false;         // (back from the hold: the mission is made)
        return;
    }
    wclog(1, "X-Wing: a mission is about to be made: %s", xw_mission_path().c_str());
    if (xw_session_launch_hold()) {
        wclog(1, "X-Wing: the launch held for the room");
        const PhysPt stub = ((PhysPt)g_psp << 4) + kStubOff;
        mem_writeb(stub, 0xEB);         // jmp $
        mem_writeb(stub + 1, 0xFE);
        SegSet16(cs, g_psp);
        reg_eip = kStubOff;
        g_holding = true;
    }
}

void at_stub() {
    if (!xw_session_launch_hold()) {
        g_holding = false;
        g_released = true;
        SegSet16(cs, seg_of(kCreateMission));
        reg_eip = kCreateMission.off;
        return;
    }
    CPU_IODelayRemoved += CPU_Cycles;      // (idle, as HLT is, until the room is there)
    CPU_Cycles = 0;
}

}  // namespace

// --- the game, for the session -----------------------------------------------------------------------------------

Bit16u xw_load_segment() { return g_load; }
PhysPt xw_ds(Bit16u off) { return dsa(off); }
PhysPt xw_obj(int o) { return dsa(obj_at(o)); }
PhysPt xw_craft(int o) {
    if (!g_found || o < 0 || o >= xw::kCraftSlots) return 0;
    const Bit16u near = mem_readw(xw_obj(o) + xw::kOCraft);
    return near ? dsa(near) : 0;
}
bool xw_flight_loaded() { return g_found && bytes_at(kDoFrame) && bytes_at(kCreateProjectile); }
bool xw_in_flight() {
    return g_found && ours_running() && PIC_FullIndex() - g_lastFrameMs < 500 && mem_readb(dsa(xw::kMissionEnd)) == 0 &&
           mem_readb(dsa(kSceneFlag)) == 0;
}
int xw_player_object() {
    if (!g_found) return -1;
    const Bit16u o = r16(xw::kPlayerObj);
    return o < xw::kObjectCount ? (int)o : -1;
}
double xw_frame_ms() { return g_frameMs; }
bool xw_session_holding() { return g_holding; }

std::string xw_mission_path() {
    std::string path;
    if (!g_found || !g_pathAt) return path;
    for (int i = 0; i < 64; i++) {
        const char c = (char)mem_readb(dsa((Bit16u)(g_pathAt + i)));
        if (!c) break;
        path += c;
    }
    if (path.size() > 2 && path[1] == ':') path.erase(0, 2);
    return path;
}
// The mission made instead (a guest's game makes the host's): the path written over the game's own, its drive kept.
void xw_set_mission_path(const std::string &path) {
    if (!g_found || !g_pathAt || path.empty() || path.size() > 40 || path == xw_mission_path()) return;
    const bool drive = mem_readb(dsa((Bit16u)(g_pathAt + 1))) == ':';
    const Bit16u at = (Bit16u)(g_pathAt + (drive ? 2 : 0));
    for (size_t i = 0; i <= path.size(); i++) mem_writeb(dsa((Bit16u)(at + i)), i < path.size() ? (Bit8u)path[i] : 0);
    wclog(1, "X-Wing: the mission made is the host's: %s", path.c_str());
}

void xw_queue_call(XwFn fn, int nargs, Bit16u a0, Bit16u a1, Bit16u a2, Bit16u a3, XwCallBefore before, XwCallDone done, int tag) {
    if (g_calls.size() >= 256) return;
    Call c;
    c.fn = fn;
    c.nargs = nargs < 0 ? 0 : nargs > 4 ? 4 : nargs;
    c.args[0] = a0;
    c.args[1] = a1;
    c.args[2] = a2;
    c.args[3] = a3;
    c.before = before;
    c.done = done;
    c.tag = tag;
    g_calls.push_back(c);
}
bool xw_in_call() { return g_inCall; }
Bit16u xw_arg(int k) {
    return mem_readw(((PhysPt)SegValue(ss) << 4) + (Bit16u)((reg_esp & 0xFFFF) + 4 + 2 * k));
}
void xw_skip_call(Bit16u ax) {
    const Bit16u ip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    reg_eax = (reg_eax & 0xFFFF0000u) | ax;
    SegSet16(cs, seg);
    reg_eip = ip;
}

// --- the dispatcher's ---------------------------------------------------------------------------------------------

bool xw_found() { return g_found; }

void xw_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    if (strcmp(name, "BWING.EXE") != 0) {
        if (g_found && psp != g_psp && dos.psp() != g_psp) {
            // (another program, not a child of ours: the game is gone)
        }
        return;
    }
    g_found = true;
    g_load = loadSeg;
    g_psp = psp;
    g_ds = (Bit16u)(loadSeg + kDgroupPara);
    g_nextCheckMs = PIC_FullIndex() + 1000;
    g_calls.clear();
    g_inCall = g_released = g_holding = false;
    g_pathAt = 0;
    hooks_game_changed();       // (the dispatcher's table: our places)
    wclog(1, "X-Wing: BWING.EXE loaded at %04X (PSP %04X): its flight overlay's places watched", loadSeg, psp);
}

// X-Wing is the game of this run: the page says so (WCGAME), or BWING.EXE has been loaded.
static bool xw_chosen() {
    static int env = -1;
    if (env < 0) env = getenv("WCGAME") && !strcmp(getenv("WCGAME"), "xwing") ? 1 : 0;
    return env || g_found;
}

void xw_tick() {
    if (xw_chosen()) xw_session_tick();
    if (!g_found) return;
    const double now = PIC_FullIndex();
    if (now >= g_nextCheckMs) {
        g_nextCheckMs = now + 1000;
        // (the program gone: its PSP no longer a program's.  The MCB before a PSP names its owner: the PSP itself)
        if (mem_readw(((PhysPt)(g_psp - 1) << 4) + 1) != g_psp) {
            wclog(1, "X-Wing: BWING.EXE is gone from memory: ended");
            g_found = false;
            g_calls.clear();
            g_inCall = g_holding = false;
            xw_session_flight_ended();
            hooks_game_changed();
            return;
        }
    }
    if (g_flightSaid && now - g_lastFrameMs > 1000) {
        g_flightSaid = false;
        wclog(1, "X-Wing: the flight's frames have stopped (status %u, end flag %u)", mem_readb(dsa(xw::kMissionStatus)),
              mem_readb(dsa(xw::kMissionEnd)));
        xw_session_flight_ended();
    }
}

void xw_watch_list(void (*watch)(Bit16u ip)) {
    (void)watch;
    if (!g_found) return;
    watch_at(seg_of(kCreateMission), kCreateMission.off);
    watch_at(seg_of(kMissionRead), kMissionRead.off);
    watch_at(seg_of(kAfterCreate), kAfterCreate.off);
    watch_at(seg_of(kFlightOver), kFlightOver.off);
    watch_at(seg_of(kDoFrame), kDoFrame.off);
    watch_at(seg_of(kMoveObjects), kMoveObjects.off);
    watch_at(seg_of(kCreateProjectile), kCreateProjectile.off);
    watch_at(seg_of(kLaserHit), kLaserHit.off);
    watch_at(seg_of(kDamage), kDamage.off);
    watch_at(g_psp, kStubOff);
}

bool xw_check_hooks(Bit32u ip) {
    if (!g_found) return false;
    if (ip == kStubOff && SegValue(cs) == g_psp) {
        if (g_holding) at_stub();
        return true;
    }
    if (at(kMoveObjects, ip)) {
        if (g_inCall || bytes_at(kMoveObjects)) at_move_objects();
        return true;
    }
    if (at(kDoFrame, ip)) {
        if (bytes_at(kDoFrame)) frame_top();
        return true;
    }
    if (at(kCreateMission, ip)) {
        if (bytes_at(kCreateMission)) at_create_mission();
        return true;
    }
    if (at(kMissionRead, ip)) {
        if (!bytes_at(kMissionRead) || !bytes_at(kCreateMission)) return true;
        g_released = false;
        if (g_real) xw_session_before_create();
        return true;
    }
    if (at(kAfterCreate, ip)) {
        if (bytes_at(kAfterCreate) && !g_real) {
            wclog(1, "X-Wing: no jump's scene: the mission made again, the launch");
            reg_eip = kCreateAgain;
        }
        g_real = false;
        return true;
    }
    if (at(kFlightOver, ip)) {
        if (bytes_at(kFlightOver)) {
            wclog(1, "X-Wing: the flight's frames are over (status %u, end flag %u)", mem_readb(dsa(xw::kMissionStatus)), mem_readb(dsa(xw::kMissionEnd)));
            xw_session_flight_over();
        }
        return true;
    }
    if (at(kCreateProjectile, ip)) {
        if (!bytes_at(kCreateProjectile)) return true;
        g_shotsMade++;
        if (xw_arg(0) == (Bit16u)xw_player_object()) g_ourShots++;
        xw_session_shot();
        return true;
    }
    if (at(kLaserHit, ip)) {
        if (bytes_at(kLaserHit)) xw_session_laser_hit();
        return true;
    }
    if (at(kDamage, ip)) {
        if (bytes_at(kDamage)) xw_session_damage();
        return true;
    }
    return false;
}

// Esc while our launch is held for a host already flying: flown alone.  Every other key is the game's.
bool xw_key(int key, bool pressed) {
    if (!g_found || !g_holding) return false;
    if (key == KBD_esc && pressed && xw_session_fly_alone()) return true;
    return false;
}

bool xw_script_item(const std::string &item, double t) {
    if (item.compare(0, 8, "!xwkill=") == 0) {
        // (test aid) !xwkill=<object>: collide_damagecraft(object, -1, -1, 0) forty times: it blows up
        const int o = atoi(item.c_str() + 8);
        for (int k = 0; k < 40; k++) xw_queue_call(XW_DAMAGE, 4, (Bit16u)o, 0xFFFF, 0xFFFF, 0, NULL, NULL, o);
        return true;
    }
    if (item.compare(0, 9, "!xwahead=") == 0) {
        int o = -1, units = 0;
        double secs = 3600;
        sscanf(item.c_str() + 9, "%d:%d:%lf", &o, &units, &secs);
        g_aheadObj = o;
        g_aheadUnits = units;
        g_aheadUntil = PIC_FullIndex() + secs * 1000;
        wclog(1, "key script %.1fs: X-Wing: object %d held %d units ahead of our nose for %.0f s", t, o, units, secs);
        return true;
    }
    if (item.compare(0, 5, "!xwr=") == 0) {
        // !xwr=<DS offset hex>[/<another>...]: words of the data segment logged
        std::string out;
        for (const char *p = item.c_str() + 5; *p;) {
            const Bit16u off = (Bit16u)strtoul(p, NULL, 16);
            char b[32];
            snprintf(b, sizeof(b), " DS:%04X=%04X", off, g_found ? r16(off) : 0);
            out += b;
            const char *c = strchr(p, '/');
            if (!c) break;
            p = c + 1;
        }
        wclog(1, "key script %.1fs: X-Wing:%s", t, out.c_str());
        return true;
    }
    if (item.compare(0, 5, "!xww=") == 0) {
        // !xww=<DS offset hex>:<byte hex>: a byte of the data segment written
        unsigned off = 0, v = 0;
        if (g_found && sscanf(item.c_str() + 5, "%x:%x", &off, &v) == 2) mem_writeb(dsa((Bit16u)off), (Bit8u)v);
        wclog(1, "key script %.1fs: X-Wing: DS:%04X byte set to %02X", t, off, v);
        return true;
    }
    if (item == "!xwcraft") {
        // every craft logged: its slot, identity, flight group, type, place, speed, hull, shields, death
        if (!g_found || !xw_flight_loaded()) {
            wclog(1, "key script %.1fs: X-Wing: craft: no flight", t);
            return true;
        }
        for (int o = 0; o < xw::kCraftSlots; o++) {
            const PhysPt a = xw_obj(o);
            if (!mem_readb(a + xw::kOType)) continue;
            const PhysPt c = xw_craft(o);
            wclog(1, "key script %.1fs: X-Wing: craft %d id %05X fg %d type %u cat %u at %d %d %d speed %d hull %u/%u shields %d %d dying %u%s", t, o,
                  xw_session_ident(o), c ? mem_readb(c + xw::kCraftFg) : -1, mem_readb(a + xw::kOType), mem_readb(a + xw::kOCat),
                  (Bit32s)mem_readd(a + xw::kOPos), (Bit32s)mem_readd(a + xw::kOPos + 4), (Bit32s)mem_readd(a + xw::kOPos + 8),
                  (Bit16s)mem_readw(a + xw::kOSpeed), c ? mem_readw(c + xw::kCraftHull) : 0, c ? mem_readw(c + xw::kCraftHullMax) : 0,
                  c ? (Bit16s)mem_readw(c + xw::kCraftShieldFront) : 0, c ? (Bit16s)mem_readw(c + xw::kCraftShieldRear) : 0,
                  mem_readw(a + xw::kODeath), o == xw_player_object() ? " (ours)" : "");
        }
        return true;
    }
    if (item == "!xwpose") {
        const int o = xw_player_object();
        if (!g_found || o < 0 || !xw_flight_loaded()) {
            wclog(1, "key script %.1fs: X-Wing: pose: no craft of ours", t);
            return true;
        }
        const PhysPt a = xw_obj(o);
        int bolts = 0;
        for (int n = xw::kCraftSlots; n < xw::kObjectCount; n++) bolts += mem_readb(xw_obj(n) + xw::kOType) ? 1 : 0;
        wclog(1, "key script %.1fs: X-Wing: pose: obj %d at %d %d %d hpr %u %u %u nose %d %d %d speed %d target %d; shots made %u (ours %u), %d objects past the craft",
              t, o, (Bit32s)mem_readd(a + xw::kOPos), (Bit32s)mem_readd(a + xw::kOPos + 4), (Bit32s)mem_readd(a + xw::kOPos + 8),
              mem_readw(a + xw::kOHeading), mem_readw(a + xw::kOPitch), mem_readw(a + xw::kORoll), (Bit16s)mem_readw(a + xw::kOMove),
              (Bit16s)mem_readw(a + xw::kOMove + 2), (Bit16s)mem_readw(a + xw::kOMove + 4), (Bit16s)mem_readw(a + xw::kOSpeed),
              (Bit16s)r16(xw::kTarget), g_shotsMade, g_ourShots, bolts);
        return true;
    }
    return false;
}

}  // namespace wc
