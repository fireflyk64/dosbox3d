/*
 *  The TIE Fighter demo: finding TIEMISS.EXE and watching it (wcnet_tiedemo.h, docs/tiedemo.md).
 *
 *  TIEMISS.EXE (363,910 bytes: a 293,861-byte MZ image with 2,095 relocations, then the overlays) is loaded by DOS for
 *  TIEDEMO.BAT ("tiemiss demo >foo"), so its load segment is known from the start; the build is the one whose image
 *  holds "TIE Fighter Demo V1.0 12/04/93" at 0x30A70.  Places in it are image paragraphs from the load segment, found
 *  by the CD version's functions (OpenTIE's C, whose order the demo's code follows: docs/tiedemo.md section 4).
 *
 *  The places watched (each an IP, its CS checked):
 *    0FC2:000E  create_createmission's entry: the launch (held in the PSP's unused FCB bytes until the room is there)
 *    0FC2:0136  ... its mission file read, its first random number next: the host's seed and a guest's seat in the
 *               player's flight group written
 *    0090:06CE  tie_doframe's entry: the top of each flight frame
 *    054A:0000  move_moveobjects's entry: the others' craft written in; our calls into the program made here
 *    1338:0B16  laser_createprojectile(shooter, hardpoint, type)
 *    05B3:1B18  collide_laserhitcraft(bolt, target, offset)
 *    05B3:1CF8  collide_damagecraft(target, component, attacker, side)
 *  and 05B3:2CA8 collide_updatekills(shooter, victim, 1) is called.
 *
 *  For finding the rest (research aids, the environment's):
 *    WCNET_TDM_PROF=1     every ~1000 instructions while TIEMISS runs, where the CPU is (CS less the load segment, IP)
 *                         and DS counted; every 5 s the 30 commonest places and data segments logged
 *    WCNET_TDM_WATCH=<linear hex>:<bytes>   every instruction while TIEMISS runs, those bytes compared: a change logged
 *                         with the instruction after the writer (CS less the load segment, IP) and the old and new
 *                         bytes (at most 600 lines)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "wcnet_tiedemo.h"
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

const char kVersion[] = "TIE Fighter Demo V1.0 12/04/93";
const Bit32u kVersionImage = 0x30A70;       // its offset in the load image

// The program's places (image paragraph, offset)
struct Place { Bit16u para, off; };
const Place kCreateMission = { 0x0FC2, 0x000E };    // create_createmission
const Place kMissionRead = { 0x0FC2, 0x0136 };      // ... the mission file read (its flight groups at the mission segment's
                                                    // 0xD0), its first random number next, then the craft made
const Place kDoFrame = { 0x0090, 0x06CE };          // tie_doframe
const Place kMoveObjects = { 0x054A, 0x0000 };      // move_moveobjects
const Place kCreateProjectile = { 0x1338, 0x0B16 }; // laser_createprojectile
const Place kLaserHit = { 0x05B3, 0x1B18 };         // collide_laserhitcraft
const Place kDamage = { 0x05B3, 0x1CF8 };           // collide_damagecraft
const Place kUpdateKills = { 0x05B3, 0x2CA8 };      // collide_updatekills
const Place kBoard = { 0x0BBC, 0x17FE };            // paiman_boardmaneuver (the maneuver table's 18th, DS-image 0x43004)
const Place kFlightOver = { 0x0090, 0x04FB };       // the flight's frames over (the end flag set), its status not yet
                                                    // looked at: the campaign's (wcnet_tiedemo_campaign.cpp)
const Place kBank = { 0x1411, 0x1708 };             // user_inputforplane's bank into a turn: mov ax,0x58; mul [CD7C];
const Bit16u kBankDone = 0x1718;                    //   mov bx,ax; mov ax,[bp-42]; sub [bx+67EC],ax (the roll), then
const Bit8u kBankBytes[] = { 0xB8, 0x58, 0x00, 0xF7, 0x26, 0x7C, 0xCD, 0x8B, 0xD8, 0x8B, 0x46, 0xBE, 0x29, 0x87, 0xEC, 0x67,
                             0x5E, 0x8B, 0xE5, 0x5D, 0xCB };    // pop si; mov sp,bp; pop bp; retf (kBankDone)
enum {
    kDgroupPara = 0x421F,
    kStubOff = 0x5C,            // the launch hold's idle: "jmp $" written in the PSP's first FCB (unused by the program)
    kObjType = 0x04,
    kObjPos = 0x06,
};

bool g_found = false;
Bit16u g_load = 0, g_psp = 0, g_ds = 0;
double g_nextCheckMs = 0;
// the flight's frames
Bit32u g_frame = 0;
double g_lastFrameMs = -1e9, g_frameMs = 50, g_rateSince = 0;
Bit32u g_rateFrames = 0;
bool g_flightSaid = false;
// the Neon showroom (NEON.EXE, the first release's): its load, its wait checked, run ahead for a while
bool g_neonSeen = false;            // (the TIE demo is the game this run: its room's session lives from now on)
Bit16u g_neonLoad = 0, g_neonPsp = 0;
double g_neonSince = 0;
bool g_neonChecked = false, g_neonKnown = false;
unsigned g_neonSkipped = 0;
// the holds: the launch's (create_createmission's entry) and a guest's for the host's verdict (the flight over).  A
// hold let go jumps back to its place, whose hook may or may not come up for that first instruction (the dispatcher
// does not look again at an IP a hook has just set): `released` says so if it does, and is cleared further on in
// any case (the mission read; the next flight's first frame), never left to stop the next flight's hold.
enum Hold { HOLD_NONE, HOLD_LAUNCH, HOLD_VERDICT };
Hold g_holding = HOLD_NONE;
bool g_released = false, g_verdictReleased = false;

// (research aids)
int g_prof = -1;
std::map<Bit32u, unsigned> g_profAt;        // (CS - load) << 16 | IP -> samples
std::map<Bit16u, unsigned> g_profDs;
double g_profNextMs = 0;
unsigned g_profSamples = 0;
Bit32u g_watchLin = 0;
int g_watchLen = -1;
Bit8u g_watchSeen[64];
unsigned g_watchLines = 0;
// (the aid !tdmglue=dx:dy:dz: object 0 held beside the player, every frame after the moves, the player's record but its
// number copied with the offset added to its place)
bool g_glue = false;
Bit32s g_glueD[3] = { 0, 0, 0 };
std::vector<std::pair<double, int> > g_recAt;      // (the aid !tdmrec=<object>: its record logged once)

inline PhysPt dsa(Bit16u off) { return ((PhysPt)g_ds << 4) + off; }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline Bit32s r32s(Bit16u off) { return (Bit32s)mem_readd(dsa(off)); }
inline void w32(Bit16u off, Bit32u v) { mem_writed(dsa(off), v); }
inline Bit16u obj_at(int n) { return (Bit16u)(tdm::kObjects + n * tdm::kObjectSize); }
inline Bit16u seg_of(const Place &p) { return (Bit16u)(g_load + p.para); }
inline bool at(const Place &p, Bit32u ip) { return ip == p.off && SegValue(cs) == seg_of(p); }

void log_record(int n) {
    const Bit16u o = obj_at(n);
    std::string words;
    for (int k = 0; k < tdm::kObjectSize; k += 2) {
        char b[8];
        snprintf(b, sizeof(b), " %04x", r16((Bit16u)(o + k)));
        words += b;
    }
    wclog(1, "TIE demo: object %d at DS:%04X (type %u, at %d %d %d):%s", n, o, mem_readb(dsa((Bit16u)(o + kObjType))),
          r32s((Bit16u)(o + kObjPos)), r32s((Bit16u)(o + kObjPos + 4)), r32s((Bit16u)(o + kObjPos + 8)), words.c_str());
}

bool version_here(Bit16u load) {
    const PhysPt a = ((PhysPt)load << 4) + kVersionImage;
    for (size_t i = 0; i < sizeof(kVersion) - 1; i++) {
        if (mem_readb(a + (PhysPt)i) != (Bit8u)kVersion[i]) return false;
    }
    return true;
}

bool ours_running() { return g_found && dos.psp() == g_psp; }

void prof_sample() {
    const Bit16u codeSeg = SegValue(cs);
    g_profAt[((Bit32u)(Bit16u)(codeSeg - g_load) << 16) | (reg_eip & 0xFFFF)]++;
    g_profDs[(Bit16u)(SegValue(ds) - g_load)]++;
    g_profSamples++;
    const double now = PIC_FullIndex();
    if (now < g_profNextMs) return;
    g_profNextMs = now + 5000;
    std::vector<std::pair<unsigned, Bit32u> > top;
    for (std::map<Bit32u, unsigned>::const_iterator i = g_profAt.begin(); i != g_profAt.end(); ++i) top.push_back(std::make_pair(i->second, i->first));
    std::sort(top.rbegin(), top.rend());
    std::string atText;
    for (size_t i = 0; i < top.size() && i < 30; i++) {
        char b[40];
        snprintf(b, sizeof(b), " %04X:%04X=%u", top[i].second >> 16, top[i].second & 0xFFFF, top[i].first);
        atText += b;
    }
    wclog(1, "TIE demo profile: %u samples; the commonest places (image paragraph:IP=samples):%s", g_profSamples, atText.c_str());
    std::string dsText;
    std::vector<std::pair<unsigned, Bit16u> > dss;
    for (std::map<Bit16u, unsigned>::const_iterator i = g_profDs.begin(); i != g_profDs.end(); ++i) dss.push_back(std::make_pair(i->second, i->first));
    std::sort(dss.rbegin(), dss.rend());
    for (size_t i = 0; i < dss.size() && i < 6; i++) {
        char b[32];
        snprintf(b, sizeof(b), " %04X:%u", dss[i].second, dss[i].first);
        dsText += b;
    }
    wclog(1, "TIE demo profile: DS (image paragraph:samples)%s", dsText.c_str());
    g_profAt.clear();
    g_profDs.clear();
    g_profSamples = 0;
}

void watch_step() {
    if (g_watchLen < 0) {
        g_watchLen = 0;
        const char *env = getenv("WCNET_TDM_WATCH");
        if (env && env[0]) {
            g_watchLin = (Bit32u)strtoul(env, NULL, 16);
            const char *colon = strchr(env, ':');
            g_watchLen = colon ? atoi(colon + 1) : 2;
            if (g_watchLen > (int)sizeof(g_watchSeen)) g_watchLen = sizeof(g_watchSeen);
            for (int i = 0; i < g_watchLen; i++) g_watchSeen[i] = mem_readb(g_watchLin + (PhysPt)i);
            wclog(1, "TIE demo: watching %d bytes at %05X", g_watchLen, g_watchLin);
        }
    }
    if (!g_watchLen) return;
    wc_net_countdown = 0;                           // (every instruction comes back here)
    for (int i = 0; i < g_watchLen; i++) {
        const Bit8u now = mem_readb(g_watchLin + (PhysPt)i);
        if (now != g_watchSeen[i]) {
            if (g_watchLines < 600) {
                g_watchLines++;
                wclog(1, "TIE demo watch: %05X+%d %02x -> %02x before %04X:%04X (image %04X:%04X) DS %04X SS:SP %04X:%04X",
                      g_watchLin, i, g_watchSeen[i], now, (unsigned)SegValue(cs), (unsigned)(reg_eip & 0xFFFF),
                      (unsigned)(Bit16u)(SegValue(cs) - g_load), (unsigned)(reg_eip & 0xFFFF), (unsigned)SegValue(ds),
                      (unsigned)SegValue(ss), (unsigned)(reg_esp & 0xFFFF));
            }
            g_watchSeen[i] = now;
        }
    }
}

// --- the flight's frames ------------------------------------------------------------------------------------------

void frame_top() {
    const double now = PIC_FullIndex();
    if (now - g_lastFrameMs > 1000) {
        g_frame = 0;            // (a flight begun)
        g_rateSince = now;
        g_rateFrames = 0;
        g_flightSaid = true;
        wclog(1, "TIE demo: the flight's frames are coming (our object %d)", tdm_player_object());
        hooks_flight_began();
    }
    g_lastFrameMs = now;
    g_frame++;
    g_verdictReleased = false;
    tdm_controls_frame();
    tdm_campaign_frame();
    if (++g_rateFrames >= 20) {
        g_frameMs = (now - g_rateSince) / g_rateFrames;
        g_rateSince = now;
        g_rateFrames = 0;
    }
    tdm_session_frame();
}

void glue_step() {
    if (!g_glue) return;
    const Bit16u me = r16(tdm::kPlayer), other = obj_at(0);
    if (me && me != other) {
        for (int k = 6; k < tdm::kObjectSize - 4; k += 2) mem_writew(dsa((Bit16u)(other + k)), r16((Bit16u)(me + k)));
        for (int a = 0; a < 3; a++) w32((Bit16u)(other + kObjPos + 4 * a), (Bit32u)(r32s((Bit16u)(me + kObjPos + 4 * a)) + g_glueD[a]));
    }
}

// --- calls into the program (the CD port's tie_queue_call, in 16 bits) --------------------------------------------

struct Call {
    TdmFn fn;
    int nargs;
    Bit16u args[4];
    TdmCallBefore before;
    TdmCallDone done;
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

const Place &place_of(TdmFn fn) {
    switch (fn) {
    case TDM_CREATE_PROJECTILE: return kCreateProjectile;
    case TDM_LASER_HIT: return kLaserHit;
    case TDM_DAMAGE: return kDamage;
    case TDM_BOARD: return kBoard;
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
            wclog(1, "TIE demo: a call came back to move_moveobjects with SP %04X, not %04X: the calls given up", (unsigned)(reg_esp & 0xFFFF),
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
    glue_step();
    tdm_session_before_move();
    if (!g_calls.empty()) {
        save_state();
        start_next_call();
    }
}

// --- the launch: held at create_createmission's entry -------------------------------------------------------------

void hold_at_stub(Hold why) {
    const PhysPt stub = ((PhysPt)g_psp << 4) + kStubOff;
    mem_writeb(stub, 0xEB);         // jmp $
    mem_writeb(stub + 1, 0xFE);
    SegSet16(cs, g_psp);
    reg_eip = kStubOff;
    g_holding = why;
}

void at_create_mission() {
    if (g_released) {
        g_released = false;         // (back from the hold: the mission is made)
        return;
    }
    if (tdm_session_launch_hold()) {
        wclog(1, "TIE demo: the launch held for the room");
        hold_at_stub(HOLD_LAUNCH);
    }
}

void at_flight_over() {
    if (g_verdictReleased) {
        g_verdictReleased = false;  // (back from the hold: the status is written)
        return;
    }
    if (tdm_campaign_flight_over()) {
        wclog(1, "TIE demo: the flight over: held for the host's verdict");
        hold_at_stub(HOLD_VERDICT);
    }
}

void at_stub() {
    const bool launch = g_holding == HOLD_LAUNCH;
    if (launch ? !tdm_session_launch_hold() : !tdm_campaign_flight_over()) {
        g_holding = HOLD_NONE;
        const Place &back = launch ? kCreateMission : kFlightOver;
        (launch ? g_released : g_verdictReleased) = true;
        SegSet16(cs, seg_of(back));
        reg_eip = back.off;
        return;
    }
    CPU_IODelayRemoved += CPU_Cycles;      // (idle, as HLT is, until the room is there)
    CPU_Cycles = 0;
}

}  // namespace

// --- the game, for the session -----------------------------------------------------------------------------------

Bit16u tdm_load_segment() { return g_load; }
PhysPt tdm_ds(Bit16u off) { return dsa(off); }
PhysPt tdm_obj(int o) { return dsa(obj_at(o)); }
PhysPt tdm_craft(int o) {
    if (!g_found || o < 0 || o >= tdm::kCraftSlots) return 0;
    const Bit16u near = mem_readw(tdm_obj(o) + 0x54);
    return near ? dsa(near) : 0;
}
bool tdm_in_flight() {
    return g_found && ours_running() && PIC_FullIndex() - g_lastFrameMs < 500 && mem_readb(dsa(tdm::kMissionEnd)) == 0;
}
int tdm_player_object() {
    if (!g_found) return -1;
    const Bit16u o = r16(tdm::kPlayerObj);
    return o < tdm::kObjectCount ? (int)o : -1;
}
double tdm_frame_ms() { return g_frameMs; }

void tdm_queue_call(TdmFn fn, int nargs, Bit16u a0, Bit16u a1, Bit16u a2, Bit16u a3, TdmCallBefore before, TdmCallDone done, int tag) {
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
bool tdm_in_call() { return g_inCall; }
Bit16u tdm_arg(int k) {
    return mem_readw(((PhysPt)SegValue(ss) << 4) + (Bit16u)((reg_esp & 0xFFFF) + 4 + 2 * k));
}
void tdm_skip_call(Bit16u ax) {
    const Bit16u ip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    reg_eax = (reg_eax & 0xFFFF0000u) | ax;
    SegSet16(cs, seg);
    reg_eip = ip;
}

// --- the Dodge Neon showroom (NEON.EXE) ----------------------------------------------------------------------------
// The demo's first release (the Internet Archive's TIE_Fighter_Preview__Dodge_Neon.zip) starts with the Dodge Neon's
// showroom: TIEDEMO.BAT runs NEON.EXE first (any key but Esc on to the game, Esc out to DOS) and again after the
// ending.  Its introduction ("Say hello to Neon", the car, the buttons one by one) takes some 9 s, keys and clicks
// unheard until its menu is up: it is timed by one routine, wait(n) at load+0BB8:001B, which polls the timer chip until
// n units of 6 x 256 of its counts (1.3 ms) have passed.  For its first kNeonFastMs that routine returns at once (its
// clock jumped ahead): the menu is up as soon as the program is, and takes a click or a key at once.  (NEON.EXE is
// packed: its code is there to check only once it runs.)

const Bit16u kNeonWaitPara = 0x0BB8, kNeonWaitOff = 0x001B;
const Bit8u kNeonWait[] = { 0x55, 0x8B, 0xEC, 0x56, 0x57, 0xEB, 0x20, 0x0E, 0xE8, 0xE2, 0xFF, 0x8B, 0xF0 };
const double kNeonFastMs = 15000;

bool neon_running() { return g_neonPsp && dos.psp() == g_neonPsp; }

void at_neon_wait() {
    if (!g_neonChecked) {
        g_neonChecked = true;
        const PhysPt a = ((PhysPt)SegValue(cs) << 4) + kNeonWaitOff;
        g_neonKnown = true;
        for (size_t i = 0; i < sizeof(kNeonWait); i++) g_neonKnown = g_neonKnown && mem_readb(a + (PhysPt)i) == kNeonWait[i];
        if (!g_neonKnown) wclog(1, "TIE demo: NEON.EXE's wait is not the one known: the showroom keeps its own time");
    }
    if (!g_neonKnown || PIC_FullIndex() - g_neonSince >= kNeonFastMs) return;
    g_neonSkipped++;
    tdm_skip_call(0);
}

// Esc never leaves the demo for DOS (the user, 2026-11-15: "I tried tie demo...and hit esc or something and quit to dos
// prompt...which wasnt helpful ... We should probably trap esc if it's going to force people to dos prompt on their
// own").  While TIEMISS runs Esc ends it at once (to the ending, then the showroom or DOS): the key never reaches it,
// but for a guest whose launch waits on a host already flying, for whom it means "fly alone".  At the showroom Esc is
// its way out to DOS (its exit code, which TIEDEMO.BAT tests): that code becomes "go to game" (tdm_program_ended).
bool tdm_key(int key, bool pressed) {
    if (!g_found || !ours_running()) return false;
    if (key != KBD_esc) return tdm_controls_key(key, pressed);     // (the flight's keys: wcnet_tiedemo_controls.cpp)
    if (pressed && g_holding == HOLD_LAUNCH && tdm_session_fly_alone()) return true;
    static double toldAt = -1e9;
    if (pressed && PIC_FullIndex() - toldAt > 10000) {
        toldAt = PIC_FullIndex();
        wclog(1, "TIE demo: Esc kept from TIEMISS (it would end the demo at once)");
    }
    return true;
}

void tdm_program_ended(Bit16u psp) {
    if (!g_neonPsp || psp != g_neonPsp || dos.return_code != 27) return;
    dos.return_code = 'G';
    wclog(1, "TIE demo: Esc at the Neon showroom: on to the game (the demo never leaves for DOS)");
}

// --- the dispatcher's --------------------------------------------------------------------------------------------

bool tdm_found() { return g_found; }
bool tdm_watching() { return g_found || neon_running(); }

void tdm_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    if (strcmp(name, "NEON.EXE") == 0) {
        g_neonSeen = true;
        g_neonLoad = loadSeg;
        g_neonPsp = psp;
        g_neonSince = PIC_FullIndex();
        g_neonChecked = g_neonKnown = false;
        g_neonSkipped = 0;
        hooks_game_changed();       // (the dispatcher's table: the showroom's wait)
        wclog(1, "TIE demo: NEON.EXE (the Dodge Neon showroom) loaded at %04X (PSP %04X)", loadSeg, psp);
        return;
    }
    if (g_neonPsp) {
        g_neonPsp = 0;              // (another program: the showroom is gone, its PSP perhaps the next one's)
        hooks_game_changed();
    }
    if (strcmp(name, "TIEMISS.EXE") != 0) return;
    if (!version_here(loadSeg)) {
        wclog(0, "TIEMISS.EXE is not the TIE Fighter demo build the hooks know (\"%s\"): not watched", kVersion);
        return;
    }
    g_found = true;
    g_load = loadSeg;
    g_psp = psp;
    g_ds = (Bit16u)(loadSeg + kDgroupPara);
    g_nextCheckMs = PIC_FullIndex() + 1000;
    g_calls.clear();
    g_inCall = g_released = g_verdictReleased = false;
    g_holding = HOLD_NONE;
    hooks_game_changed();       // (the dispatcher's table: our places)
    wclog(1, "TIE demo: TIEMISS.EXE loaded at %04X (PSP %04X): %s", loadSeg, psp, kVersion);
    tdm_campaign_loaded();
}

// The TIE demo is the game of this run: the page says so (WCGAME), or its showroom or TIEMISS has been loaded.  Its
// room's session lives from then on, through the showroom, the opening and every TIEMISS (a host's launch waits for
// the guests that are in the room but not yet at theirs).
bool tdm_chosen() {
    static int env = -1;
    if (env < 0) env = getenv("WCGAME") && !strcmp(getenv("WCGAME"), "tiedemo") ? 1 : 0;
    return env || g_neonSeen || g_found;
}

void tdm_tick() {
    if (tdm_chosen()) tdm_session_tick();
    if (!g_found) return;
    const double now = PIC_FullIndex();
    if (now >= g_nextCheckMs) {
        g_nextCheckMs = now + 1000;
        if (!version_here(g_load)) {
            wclog(1, "TIE demo: TIEMISS.EXE is gone from memory: ended");
            g_found = false;
            g_calls.clear();
            g_inCall = false;
            tdm_session_flight_ended();
            return;
        }
    }
    if (g_flightSaid && now - g_lastFrameMs > 1000) {
        g_flightSaid = false;
        tdm_session_flight_ended();
    }
    if (!ours_running()) return;
    if (g_prof < 0) {
        const char *env = getenv("WCNET_TDM_PROF");
        g_prof = env && env[0] && env[0] != '0' ? 1 : 0;
        g_profNextMs = now + 5000;
    }
    if (g_prof) prof_sample();
    watch_step();
    for (size_t i = 0; i < g_recAt.size();) {
        if (now >= g_recAt[i].first) {
            log_record(g_recAt[i].second);
            g_recAt.erase(g_recAt.begin() + i);
        } else {
            i++;
        }
    }
}

void tdm_watch_list(void (*watch)(Bit16u ip)) {
    if (g_neonPsp) watch(kNeonWaitOff);
    if (!g_found) return;
    watch(kCreateMission.off);
    watch(kMissionRead.off);
    watch(kDoFrame.off);
    watch(kMoveObjects.off);
    watch(kCreateProjectile.off);
    watch(kLaserHit.off);
    watch(kDamage.off);
    watch(kBoard.off);
    watch(kFlightOver.off);
    watch(kBank.off);
    watch(kStubOff);
}

unsigned g_shotsMade, g_ourShots;      // laser_createprojectile's calls, and those for our craft (!tdmpose)

bool tdm_check_hooks(Bit32u ip) {
    if (neon_running()) {
        if (ip == kNeonWaitOff && SegValue(cs) == (Bit16u)(g_neonLoad + kNeonWaitPara)) {
            at_neon_wait();
            return true;
        }
        return false;
    }
    if (!g_found) return false;
    if (ip == kStubOff && SegValue(cs) == g_psp) {
        if (g_holding) at_stub();
        return true;
    }
    if (at(kMoveObjects, ip)) {
        at_move_objects();
        return true;
    }
    if (at(kDoFrame, ip)) {
        frame_top();
        return true;
    }
    if (at(kCreateMission, ip)) {
        at_create_mission();
        return true;
    }
    if (at(kMissionRead, ip)) {
        g_released = false;
        tdm_campaign_mission_read();        // (the mission written first: the session's seat is in it)
        tdm_session_before_create();
        return true;
    }
    if (at(kFlightOver, ip)) {
        at_flight_over();
        return true;
    }
    if (at(kBank, ip)) {
        // (point to aim: the bank into the turn stepped over, the function's own way out; its bytes checked once)
        static int known = -1;
        if (known < 0) {
            known = 1;
            const PhysPt a = ((PhysPt)seg_of(kBank) << 4) + kBank.off;
            for (unsigned k = 0; k < sizeof(kBankBytes); k++) known &= mem_readb(a + k) == kBankBytes[k] ? 1 : 0;
            wclog(1, "TIE demo: the bank into a turn at %04X:%04X %s", seg_of(kBank), kBank.off,
                  known ? "found (point to aim leaves it out)" : "NOT as expected: left alone");
        }
        if (known && tdm_controls_no_bank()) reg_eip = kBankDone;
        return true;
    }
    if (at(kCreateProjectile, ip)) {
        g_shotsMade++;
        if (tdm_arg(0) == (Bit16u)tdm_player_object()) g_ourShots++;
        tdm_session_shot();
        return true;
    }
    if (at(kLaserHit, ip)) {
        tdm_session_laser_hit();
        return true;
    }
    if (at(kDamage, ip)) {
        tdm_session_damage();
        return true;
    }
    if (at(kBoard, ip)) {
        // (its first bytes checked once: push bp; mov bp,sp; sub sp,2Ch; push di; push si; mov bx,[BF8C])
        static const Bit8u kBoardBytes[12] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x57, 0x56, 0x8B, 0x1E, 0x8C, 0xBF };
        static int known = -1;
        if (known < 0) {
            known = 1;
            for (int i = 0; i < 12; i++) known = known && mem_readb(SegPhys(cs) + ip + (PhysPt)i) == kBoardBytes[i];
            wclog(1, "TIE demo: paiman_boardmaneuver %s", known ? "found (dockings synced)" : "not the one known: dockings left to each machine");
        }
        if (known) tdm_session_board();
        return true;
    }
    return false;
}

bool tdm_script_item(const std::string &item, double t) {
    if (tdm_campaign_script_item(item, t)) return true;
    if (item.compare(0, 9, "!tdmglue=") == 0) {
        g_glue = sscanf(item.c_str() + 9, "%d:%d:%d", &g_glueD[0], &g_glueD[1], &g_glueD[2]) == 3;
        wclog(1, "key script %.1fs: TIE demo: object 0 %s the player (%d %d %d)", t, g_glue ? "held beside" : "let go of",
              g_glueD[0], g_glueD[1], g_glueD[2]);
        return true;
    }
    if (item.compare(0, 6, "!tdmr=") == 0) {
        // !tdmr=<DS offset hex>[/<another>...]: words of the data segment logged
        std::string out;
        for (const char *p = item.c_str() + 6; *p;) {
            const Bit16u off = (Bit16u)strtoul(p, NULL, 16);
            char b[32];
            snprintf(b, sizeof(b), " DS:%04X=%04X", off, g_found ? r16(off) : 0);
            out += b;
            const char *c = strchr(p, '/');
            if (!c) break;
            p = c + 1;
        }
        wclog(1, "key script %.1fs: TIE demo:%s", t, out.c_str());
        return true;
    }
    if (item.compare(0, 6, "!tdmw=") == 0) {
        // !tdmw=<DS offset hex>:<byte hex>: a byte of the data segment written
        unsigned off = 0, v = 0;
        if (g_found && sscanf(item.c_str() + 6, "%x:%x", &off, &v) == 2) mem_writeb(dsa((Bit16u)off), (Bit8u)v);
        wclog(1, "key script %.1fs: TIE demo: DS:%04X byte set to %02X", t, off, v);
        return true;
    }
    if (item.compare(0, 10, "!tdmsteer=") == 0) {
        tdm_controls_steer(item.substr(10));
        return true;
    }
    if (item.compare(0, 8, "!tdmrec=") == 0) {
        g_recAt.push_back(std::make_pair(PIC_FullIndex(), atoi(item.c_str() + 8)));
        return true;
    }
    if (item == "!tdmpose") {
        // our craft's attitude (heading, pitch, roll: 65536 to a turn), its nose (the move vector, x 32767), its speed,
        // and the bolts in flight (objects past the craft, a type set): the controls' measurements (docs/tiedemo.md)
        const int o = tdm_player_object();
        if (!g_found || o < 0) {
            wclog(1, "key script %.1fs: TIE demo: pose: no craft of ours", t);
            return true;
        }
        const PhysPt a = tdm_obj(o);
        int bolts = 0;
        for (int n = tdm::kCraftSlots; n < tdm::kObjectCount; n++) bolts += mem_readb(tdm_obj(n) + 4) ? 1 : 0;
        wclog(1, "key script %.1fs: TIE demo: pose: hpr %u %u %u nose %d %d %d speed %d target %d; shots made %u (ours %u), %d objects past the craft",
              t, mem_readw(a + 0x1E), mem_readw(a + 0x20), mem_readw(a + 0x22), (Bit16s)mem_readw(a + 0x3A), (Bit16s)mem_readw(a + 0x3C),
              (Bit16s)mem_readw(a + 0x3E), (Bit16s)mem_readw(a + 0x26), (Bit16s)r16(tdm::kTarget), g_shotsMade, g_ourShots, bolts);
        return true;
    }
    return false;
}

}  // namespace wc
