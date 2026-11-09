/*
 *  Privateer: finding the program and watching it (wcnet_priv.h, docs/privateer.md).
 *
 *  PRCD.EXE (the CD-ROM edition's program, 937,760 bytes: a Borland C++ 1991 overlay program of 454 segments and 183
 *  overlays, DGROUP at paragraph 0x6E4D of the image) is loaded by DOS for PRIV.EXE, Origin's loader, so the layer
 *  knows where it is from the start.  scripts/wcexe.py reads it as it reads the Wing Commanders
 *  (`scripts/wcexe.py -e <game>/PRCD.EXE segs`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>
#include <deque>
#include <algorithm>
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "wcnet_game.h"
#include "wc_net.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"
#include "cpu.h"
#include "lazyflags.h"
#include "paging.h"
#include <math.h>
#include <ctype.h>

namespace wc {

namespace {

enum {
    kDgroupPara = 0x6E4D,           // DGROUP, paragraphs from the image's start
    kSeg088 = 0x3632,               // seg088's paragraph: the controls (wcnet_priv_controls.cpp)
    kAfterKeyboard = 0x0BAD,        // seg088:0BAD, after the call of the keyboard's routine
    kControlsEntry = 0x0B2E,        // seg088:0B2E, the player's controls, once a frame in flight: where our calls are made
    kSeg099 = 0x3D67,               // seg099: placing an object (seg099:0074)
    kPlace = 0x0074,                // seg099:0074(this, type far, cast near, appearance far, pos near, matrix near): spawn
                                    // (seg111:07B1: the type found or loaded, its factory, the appearance), the place and the
                                    // attitude set, the object added to the scene's list (ds:66CE); returns the object
    kPlayerShip = 0x66B4,           // the player's ship object (a near pointer: the objects are in DGROUP's heap); 0 at a base
    kPlayerRecord = 0x66B2,         // the player's record in flight ("PLAYTYPE", "PLAYER"); 0 at a base
    kRecordSystemName = 0x54,       // ... its near pointer to the system's name (the destination display's "System")
    kShipPos = 0x14,                // its place, int32 x, y, z (z up; Troy's Achilles at 15360000, 0, -10240000)
    kShipMatrix = 0x23,             // its attitude, 3x3 int32 (1.0 = 256: the constructor seg053:064B makes it at +0x25 of
                                    // the object, which begins 2 bytes before what ds:66B4 points at)
    kShipVtable = 0x20EF,           // a ship's vtable (at the pointer's +0)
    kShipType = 0x06,               // its type object (near: "TARSTYPE" at the type's +2)
    kShipController = 0x4C,         // its controller (near: the AI's, vtable 0x14A8; the player's, 0x2A06)
    kRecordAppearance = 0x75,       // the player record's appearance name ("CLUNKER" the Tarsus), what the game spawns us with
    kShipAlive = 0x20,              // its alive flag (base+0x22: cleared by the hit routine's death path, seg053:1385)
    kShipAppearance = 0x10,         // its appearance object (far; the name at +0x18)
    kShipModel = 0x56,              // its damage model (far; base+0x58, seg053:175F): armour quadrants int16 at +0x35+6q
    kModelArmor = 0x35,
    kSphereList = 0x66C2,           // the system's spheres: a count, then (+2) an array of far pointers to 19-byte records
    kSceneList = 0x66CE,            // the scene's objects: +2 the first node; a node is {next, object}
    kSeg077 = 0x31D3,               // seg077: the encounter wings
    kWingSpawn = 0x0265,            // seg077:0265(wing far): the wing's ships made (the user's nav-point owner decides)
    kWingSphere = 0x18,             // the wing's sphere id (a byte; the next the system's)
    kWingCastIndex = 0x16,          // the wing's cast: its index in the list at +0x1A (near: +0x12 the count, +0x14 8-byte names)
    kWingCastList = 0x1A,
    kWingShips = 0x3B,              // its ships (a near array, one word a ship, 0 not yet made)
    kWingCount = 0x53,              // how many (the spawn's loop bound; the formation is made for that many, seg077:003B)
    kWingMaxCount = 8,              // (no wing extended past 8: the formations')
    kWingArrayWords = 8,
    kHitPlace = 0x1206,             // seg053:1206, in the hit routine (0F6D) just before the damage goes to the model:
                                    // [bp+0xA] the ship, [bp+0xC] the quadrant, [bp-6] the damage
    kSeg101 = 0x3EF1,               // seg101: the damage model
    kModelShields = 0x0CF0,         //   (model far, quadrant, damage) -> what the shields let through
    kModelArmorHit = 0x0C7D,        //   (model far, quadrant, damage): off the armour
    kModelIntact = 0x0C53,          //   (model far) -> AL 1 while every quadrant's armour holds
    kSeg034 = 0x1835,
    kDamageDisplay = 0x0489,        // seg034:0489(0x6803, quadrant, damage): the cockpit's damage display (the player's)
    kDamageDisplayObj = 0x6803,
    kSeg120 = 0x489B,
    kFireGuns = 0x0004,             // seg120:0004(weapons far, n): a ship's guns fired (the weapons' +0xA is the ship)
    kControlFire = 0x1E,            // the control record's fire bits: 4 the guns, 2 the launcher
    kPodVtable = 0x1584,            // a cargo pod (CDBRTYPE, made by seg036:0C90 as cargo is expelled or a ship dies)
    kPodList = 0x4B,                // its cargo list: a word, the first record's node (far) at +2, the last at +6; a node
                                    // is {next far, record far}; a record (the commodity, its names, its units) some 0x28 bytes
    kPodUnits = 0x56,               // the units in the pod (the records' sum)
    kCargoRecordBytes = 0x28,
    kCargoBlockBytes = 0x38,        // a cargo record lives at +0x10 of a far block that begins with the commodity's name (12
                                    // bytes, "Iron"), a word -3 and the block's own segment; the record's +0 points back at the block
    kCargoRecordAt = 0x10,
    kFarMalloc = 0x089D,            // seg000:089D(size dword): the game's far malloc, dx:ax
    kCredits = 0x655A,              // the player's credits, a 32-bit global (the exchange's add and sub at ovr286:103C)
    kNearHeapTop = 0x3FFC,          // the near heap's end (the game's own allocator, seg142:012B: error 1308 when SP is below it)
    kNearFreeList = 0x3FF8,
    kNearHeapLock = 0x4001,         // set by the allocator while it works on the ring (a walk then could see it torn)         // the head of its free blocks' ring (a block: size at +0, bit 0 used; +2 the block before; +4/+6 the ring)
    // A ship's cast (its AI file, DATA/AIDS: name, lines, face), the ship's own copy in the far heap: the controller's
    // +0xA is a far pointer to it; the name block (far) at its +0xA ("Merchant", 12 bytes), its face at +0xE (the
    // cast file's INFO; 14 the merchant woman), the message sets (far) at +0xF.. : the lines the player may send
    // (friendly, neutral, hostile), then the lines received (+0x1B, +0x1F, +0x23), then the fight's; a set is a
    // count and a far pointer to its block of that many ids and the strings.  The pilot object (near) is at the plan's +0x23
    // (the plan: the controller's +2, far): +6 the line said, +8 its timer, +0xA bit 0 speaking.
    kCtlCast = 0x0A,
    kCtlPlan = 0x02,
    kCastName = 0x0A,
    kCastRecv0 = 0x1B,
    kPlanPilot = 0x23,
    kPilotLine = 0x06,
    kPilotTimer = 0x08,
    kPilotFlags = 0x0A,
    kPilotFlags2 = 0x1D,            // bit 4 set as the game's own hail functions (seg013:0A60..) set a line
    kCommPending = 0x0E23,          // ds:0E23: a byte the same functions set to 1 when 0 (a hail waits to be played)
    kComm = 0x67E4,                 // ds:67E4: the comm line (seg098: 01F9 takes a line, 03A0 runs it each frame)
    kCtlComm = 0x08,                // a controller's comm record (the speaker: +8 its face frame, +0xA its voice)
    kCastFace = 0x0E,               // the cast copy's portrait id (chosen as the ship is made: a merchant's 14 or 47)
    kSeg081 = 0x332F,
    kSceneRemove = 0x00AD,          // seg081:00AD(list, object): the object out of the scene
    kShipDelete = 0x3BA7,           // seg053:3BA7(ship, 1): the ship's destructor (the vtable's +4), its memory given back
    kSignatureOff = 0x0074,         // "Type PRIV to run Privateer, or RF to run Righteous Fire."
};
const char kSignature[] = "Type PRIV to run Privateer";

bool g_found;
bool g_rf;
Bit16u g_frameSp;
Bit16u g_privLoadSeg, g_ds, g_psp;
double g_nextReportMs;

// The code trace (a test aid, "!privtrace=<name>:<seconds>"): every instruction's CS:IP counted for a while, written
// to WCNET_DUMP_DIR/<name>.trace as "cs:ip count" lines (cs - the load segment is scripts/wcexe.py's paragraph).
bool g_tracing;
double g_traceEndMs;
std::string g_traceName;
std::map<Bit32u, unsigned> g_trace;

Bit16u g_watch, g_watchLast;     // (with the trace: "!privwatchctl=<offset>:<seconds>" logs who changes a word of the
Bit32u g_watchPrev;             // last spawned ship's controller)
void trace_step(double now) {
    const Bit32u here = ((Bit32u)SegValue(cs) << 16) | (reg_eip & 0xffff);
    if (g_watch) {
        const Bit16u v = priv_r16(g_watch);
        if (v != g_watchLast) {
            wclog(1, "Privateer: watch ds:%04X %04X -> %04X by %04x:%04x (load segment %04X)", g_watch, g_watchLast, v, g_watchPrev >> 16,
                  g_watchPrev & 0xffff, g_privLoadSeg);
            g_watchLast = v;
        }
        g_watchPrev = here;
    }
    g_trace[here]++;
    if (now < g_traceEndMs) {
        wc_net_countdown = 0;   // (the next instruction comes here too)
        return;
    }
    g_tracing = false;
    g_watch = 0;
    const char *dir = getenv("WCNET_DUMP_DIR");
    std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + g_traceName + ".trace";
    FILE *f = fopen(path.c_str(), "w");
    if (f) {
        for (std::map<Bit32u, unsigned>::const_iterator it = g_trace.begin(); it != g_trace.end(); ++it) {
            fprintf(f, "%04x:%04x %u\n", it->first >> 16, it->first & 0xffff, it->second);
        }
        fclose(f);
    }
    wclog(1, "Privateer: code trace of %u places to %s (load segment %04X)", (unsigned)g_trace.size(), path.c_str(), g_privLoadSeg);
    g_trace.clear();
}

PhysPt ds_at(Bit16u off) { return ((PhysPt)g_ds << 4) + off; }

// Calls of the game's functions from the layer (TIE Fighter's way, for a 16-bit program): at seg088:0B2E's entry the
// registers are kept, the call's data put on the stack (SS is DGROUP there: the game passes stack addresses as near
// pointers itself), its arguments pushed and a far return to that same entry, and the function entered; back at the
// entry with the stack where it was left, its result is taken, everything given back, and the game's own call goes on.
struct PrivCall {
    Bit16u para, off;               // the function (a root segment's paragraph, as scripts/wcexe.py gives it)
    std::vector<Bit8u> data;        // put on the stack first; an argument kStackArg + n is the address of data[n]
    std::vector<Bit32u> args;       // the words in the order the function declares them
    void (*done)(int tag, Bit16u ax, Bit16u dx);
    void (*doneObj)(int tag, Bit16u obj);      // (a spawn's: the object made, 0 none)
    void (*cancel)(int tag);                    // the call dropped before it ran (its guard failed, or the flight changed)
    int tag;
    int castSlot;                               // (a spawn's cast name: its slot, -1 none)
    // the object the call works on: checked just before it runs (still that kind, in the scene, its damage model the
    // same): what was queued in one frame may have died, been freed or been left behind by the next
    Bit16u guardObj, guardVtable, guardModelOff, guardModelSeg;
    PrivCall() : para(0), off(0), done(NULL), doneObj(NULL), cancel(NULL), tag(0), castSlot(-1), guardObj(0), guardVtable(0), guardModelOff(0),
                 guardModelSeg(0) {}
};
enum { kStackArg = 0x10000, kCastArg = 0x40000 };     // kCastArg + slot: the near address of that cast's name
// Our ships' cast names (a ship keeps a near pointer to its cast's name, +0xE): one 9-byte slot a cast in a block of the
// game's own near heap (16 slots, 144 bytes, made once a program run by its malloc, seg000:0861, at the first spawn);
// until it is there, or should it fail, the old scratch in the program's unused usage line (one name for all)
enum { kCastSlots = 16, kCastSlotBytes = 9, kNearMallocRoot = 0x0861 };
Bit16u g_castBlock;
bool g_castBlockAsked;
std::string g_castNames[kCastSlots];
int g_castNext;
Bit16u cast_slot_address(int slot);
std::deque<PrivCall> g_calls;
struct Saved {
    bool active;
    Bit16u ip, cs, sp, back;        // the place, the stack pointer before and after our part
    Bit32u eax, ebx, ecx, edx, esi, edi, ebp, flags;
    Bit16u ds, es;
} g_saved;

static bool call_guard_ok(const PrivCall &c) {
    if (!c.guardObj) {
        return true;
    }
    if (priv_r16(c.guardObj) != c.guardVtable || !priv_scene_has(c.guardObj)) {
        return false;
    }
    if (c.guardModelSeg && (priv_r16((Bit16u)(c.guardObj + 0x56)) != c.guardModelOff || priv_r16((Bit16u)(c.guardObj + 0x58)) != c.guardModelSeg)) {
        return false;
    }
    return true;
}

static void call_dropped(const PrivCall &c, const char *why) {
    wclog(2, "Privateer: a call of ours to %04X:%04X dropped (%s)", c.para, c.off, why);
    if (c.cancel) {
        c.cancel(c.tag);
    } else if (c.doneObj) {
        c.doneObj(c.tag, 0);
    }
}

// Every call queued and not yet running dropped: the flight they were queued in is over (a landing, a death, a jump)
void calls_flush(const char *why) {
    const size_t keep = g_saved.active ? 1 : 0;     // (the one running finishes: it returns to our place)
    while (g_calls.size() > keep) {
        PrivCall c = g_calls.back();
        g_calls.pop_back();
        call_dropped(c, why);
    }
}

bool run_calls() {
    if (g_saved.active && reg_ip == g_saved.ip && SegValue(cs) == g_saved.cs && reg_sp == g_saved.back) {
        const Bit16u ax = reg_ax, dx = reg_dx;
        PrivCall done = g_calls.front();
        g_calls.pop_front();
        reg_esp = (reg_esp & 0xffff0000) | g_saved.sp;
        reg_eax = g_saved.eax;
        reg_ebx = g_saved.ebx;
        reg_ecx = g_saved.ecx;
        reg_edx = g_saved.edx;
        reg_esi = g_saved.esi;
        reg_edi = g_saved.edi;
        reg_ebp = g_saved.ebp;
        SegSet16(ds, g_saved.ds);
        SegSet16(es, g_saved.es);
        reg_flags = g_saved.flags;
        lflags.type = t_UNKNOWN;
        g_saved.active = false;
        if (done.done) {
            done.done(done.tag, ax, dx);
        }
        if (done.doneObj) {
            done.doneObj(done.tag, ax);
        }
    } else if (g_saved.active) {
        return false;               // (the place reached again inside our call: the game's own business)
    }
    while (!g_calls.empty() && !call_guard_ok(g_calls.front())) {
        PrivCall c = g_calls.front();
        g_calls.pop_front();
        call_dropped(c, "its object is gone");
    }
    if (g_calls.empty()) {
        return false;
    }
    FillFlags();
    const PrivCall &c = g_calls.front();
    const Bit16u castAt = cast_slot_address(c.castSlot);
    g_saved.active = true;
    g_saved.ip = reg_ip;
    g_saved.cs = SegValue(cs);
    g_saved.sp = reg_sp;
    g_saved.eax = reg_eax;
    g_saved.ebx = reg_ebx;
    g_saved.ecx = reg_ecx;
    g_saved.edx = reg_edx;
    g_saved.esi = reg_esi;
    g_saved.edi = reg_edi;
    g_saved.ebp = reg_ebp;
    g_saved.ds = SegValue(ds);
    g_saved.es = SegValue(es);
    g_saved.flags = reg_flags;
    const Bit16u dataAt = (Bit16u)((reg_sp - c.data.size()) & ~1u);
    for (size_t i = 0; i < c.data.size(); i++) {
        mem_writeb(SegPhys(ss) + (Bit16u)(dataAt + i), c.data[i]);
    }
    reg_sp = dataAt;
    for (size_t i = c.args.size(); i-- > 0;) {
        const Bit32u a = c.args[i];
        CPU_Push16(a >= kCastArg ? castAt : a >= kStackArg ? (Bit16u)(dataAt + (a - kStackArg)) : (Bit16u)a);
    }
    g_saved.back = reg_sp;
    CPU_Push16(g_saved.cs);
    CPU_Push16(g_saved.ip);
    SegSet16(ds, g_ds);
    SegSet16(cs, (Bit16u)(g_privLoadSeg + c.para));
    reg_eip = c.off;
    return true;
}

bool queue_call(const PrivCall &c) {
    if (g_calls.size() >= 16) {
        wclog(1, "Privateer: calls queued past 16: %04X:%04X dropped", c.para, c.off);   // (a list has a cap)
        return false;
    }
    g_calls.push_back(c);
    return true;
}

// The cast name a spawn of ours gives its ship (the ship keeps a near pointer to it, so it must stay): in the tail of
// the program's usage line at DGROUP:0074 ("..., or RF to run Righteous Fire.", printed only when PRCD.EXE is run
// alone), which no flight reads.  The type's and appearance's names go on the stack with the call's data.
enum { kScratch = 0x0074 + 28, kScratchSize = 29 };
// The AI held for these ships' controllers (puppets: wcnet_priv_session.cpp): their think, seg042:0944, returns at once.
enum { kSeg042 = 0x1DFE, kNpcThink = 0x0944, kHeldCap = 32, kSeg053 = 0x23BA };   // (held: 7 pilots and 16 encounter ships at most)
// The game's error report, seg050:0006(code): 1303 is its near heap with no block big enough (the program then ends,
// and PRIV.EXE starts it again at its title).
enum { kSeg050 = 0x20F8, kGameError = 0x0006 };
// The game's near malloc, seg142:012B(size): watched for the largest request and the stack's headroom at each (SP less
// the heap's end at ds:3FFC: the allocator's own test, error 1308 when SP is below it), ours (a call of the layer's in
// progress) apart from the game's
enum { kSeg142 = 0x4E42, kNearMalloc = 0x012B };
// A dying ship's hold ejected as cargo pods: seg053:0C0B calls seg036:1692(hold far, scene, 1) with the ship's
// allocation in DI (the ship less 2; measured with PRIVWATCH: a merchant killed, its 3-4 pods made by seg036:0C90).  A puppet's death must drop nothing: its owner's game drops the real pods and shares
// them (else every game drops its own and shares them too: the cargo of one death in every game times every game)
enum { kDeathEject = 0x0C0B, kDeathEjectAfter = 0x0C10 };
// The stack's high-water mark (a measurement): DGROUP's top 0xFFF6 - _stklen (ds:620A) up is the stack, below it the
// heap (seg142:0004).  At a frame's entry the unused stack below SP (less 256 bytes) is painted with a pattern; the
// lowest byte no longer the pattern is the deepest the stack has been since, interrupts and all.
enum { kStackLen = 0x620A, kStackPaint = 0x5A };
struct StackPaint {
    bool painted;
    Bit16u from, to;            // painted [from, to)
    int deepest;                // the lowest headroom over the heap's end seen (1 << 30: none)
    StackPaint() : painted(false), from(0), to(0), deepest(1 << 30) {}
} g_paint;

// A ship's AI target (found by the scan below): its plan (far, the controller's +2) is a chain of maneuver records, and
// the target appears in several of them -- its ship (near) and its damage model's segment (the ship's +0x58; far, at
// offset 4) -- at offsets that vary with the plan (a pirate on a militia ship: +114 +13A +170 +2CA, the model at +146
// +1AE; another: +2F0, +32E).  So a new target is given by replacing the old one wherever it stands in the plan's first
// 1 KB, ship and model alike.  Written at the frame.
enum { kPlanScan = 0x400 };
std::vector<std::pair<Bit16u, Bit16u> > g_retargets;    // (ship, target) to write at the next frame

// The ship its AI is after: the first near pointer in its plan to a live ship of the scene (0 none)
static Bit16u plan_target(Bit16u ship) {
    const Bit16u ctl = priv_r16((Bit16u)(ship + 0x4C));
    const Bit16u poff = ctl ? priv_r16((Bit16u)(ctl + 2)) : 0, pseg = ctl ? priv_r16((Bit16u)(ctl + 4)) : 0;
    if (!pseg) return 0;
    const PhysPt plan = ((PhysPt)pseg << 4) + poff;
    for (int i = 0; i < kPlanScan; i += 2) {
        Bit16u w = 0;
        if (mem_readw_checked(plan + i, &w)) break;
        if (w && w != ship && w > 0x6C00 && w < 0xF000 && priv_r16(w) == kShipVtable && priv_ship_is_alive(w) && priv_scene_has(w)) {
            // (the model's segment beside it in the same record confirms it: the word 0x32-0x40 on)
            return w;
        }
    }
    return 0;
}

static void retarget_frame() {
    for (size_t i = 0; i < g_retargets.size(); i++) {
        const Bit16u ship = g_retargets[i].first, target = g_retargets[i].second;
        if (!priv_ship_is_alive(ship) || !priv_ship_is_alive(target)) continue;
        const Bit16u old = plan_target(ship);
        if (!old || old == target) continue;
        const Bit16u ctl = priv_r16((Bit16u)(ship + 0x4C));
        const Bit16u poff = priv_r16((Bit16u)(ctl + 2)), pseg = priv_r16((Bit16u)(ctl + 4));
        const PhysPt plan = ((PhysPt)pseg << 4) + poff;
        const Bit16u oldSeg = priv_r16((Bit16u)(old + 0x58)), newSeg = priv_r16((Bit16u)(target + 0x58));
        int n = 0;
        for (int k = 0; k < kPlanScan; k += 2) {
            Bit16u w = 0;
            if (mem_readw_checked(plan + k, &w)) break;
            if (w == old) { mem_writew(plan + k, target); n++; }
            else if (w == oldSeg && oldSeg) { mem_writew(plan + k, newSeg); n++; }
        }
        wclog(2, "Privateer: ds:%04X's AI turned from ds:%04X to ds:%04X (%d words)", ship, old, target, n);
    }
    g_retargets.clear();
}

Bit16u priv_ship_target_now(Bit16u ship) { return plan_target(ship); }

void retarget_queue(Bit16u ship, Bit16u target) {
    if (g_retargets.size() < 16) g_retargets.push_back(std::make_pair(ship, target));
}

// Co-op extras (wcnet_priv_session.cpp, priv_session_wing_extras): the wing about to spawn is given more ships of its
// own -- a larger ship array from the game's heap, the old entries copied, its count raised -- so that the game itself
// makes them, with the wing's orders, in its formation, counted wherever the wing is (a mission's objectives too).
// Arrays are taken from the game's heap ahead of time (its malloc, seg000:0861, at a frame's entry: two kept ready
// while in a room); the old array is given back by its free (seg000:03BA) at the next frame.
std::vector<Bit16u> g_wingArrays, g_wingFrees;
bool g_wingArrayAsked;
std::vector<PhysPt> g_wingsExtended;        // (this flight's: a wing extended once)

void wing_array_made(int tag, Bit16u ax, Bit16u dx) {
    (void)tag; (void)dx;
    g_wingArrayAsked = false;
    if (ax && g_wingArrays.size() < 4) g_wingArrays.push_back(ax);
}

void wing_array_cancel(int tag) {
    (void)tag;
    g_wingArrayAsked = false;
}

bool queue_call(const PrivCall &c);
static void wing_arrays_frame() {
    // the old arrays given back
    for (size_t i = 0; i < g_wingFrees.size(); i++) {
        PrivCall c;
        c.para = 0;
        c.off = 0x03BA;
        c.args.push_back(g_wingFrees[i]);
        queue_call(c);
    }
    g_wingFrees.clear();
    if (!priv_session_active() || g_wingArrays.size() >= 2 || g_wingArrayAsked) {
        return;
    }
    PrivCall c;
    c.para = 0;
    c.off = kNearMallocRoot;
    c.args.push_back(kWingArrayWords * 2);
    c.done = wing_array_made;
    c.cancel = wing_array_cancel;
    if (queue_call(c)) g_wingArrayAsked = true;
}

static void wing_extend(PhysPt wing, int sphere) {
    if (std::find(g_wingsExtended.begin(), g_wingsExtended.end(), wing) != g_wingsExtended.end()) {
        return;                 // (the game calls the spawn again while the wing is armed: extended once)
    }
    const int count = (Bit16s)mem_readw(wing + kWingCount);
    const Bit16u ships = mem_readw(wing + kWingShips);
    // the wing's cast: hostile ones only
    const Bit16u list = mem_readw(wing + kWingCastList), idx = mem_readw(wing + kWingCastIndex);
    std::string cast;
    if (list && idx < priv_r16((Bit16u)(list + 0x12))) {
        const Bit16u at = (Bit16u)(priv_r16((Bit16u)(list + 0x14)) + 8 * idx);
        for (int i = 0; i < 8; i++) {
            const Bit8u c = priv_r8((Bit16u)(at + i));
            if (!c) break;
            cast += (char)c;
        }
    }
    wclog(2, "Privateer: a wing of %d (%s) spawns at nav %d", count, cast.c_str(), sphere);
    int extras = priv_session_wing_extras(sphere, cast.c_str(), count);
    if (extras <= 0 || count <= 0 || !ships) {
        return;
    }
    if (count + extras > kWingMaxCount) extras = kWingMaxCount - count;
    if (extras <= 0 || g_wingArrays.empty()) {
        wclog(1, "Privateer: co-op: the wing at nav %d (%d %s) not extended: %s", sphere, count, cast.c_str(),
              g_wingArrays.empty() ? "no array ready" : "it is full");
        return;
    }
    const Bit16u arr = g_wingArrays.back();
    g_wingArrays.pop_back();
    for (int i = 0; i < kWingArrayWords; i++) {
        priv_w16((Bit16u)(arr + 2 * i), i < count ? priv_r16((Bit16u)(ships + 2 * i)) : 0);
    }
    mem_writew(wing + kWingShips, arr);
    mem_writew(wing + kWingCount, (Bit16u)(count + extras));
    g_wingFrees.push_back(ships);
    if (g_wingsExtended.size() < 64) g_wingsExtended.push_back(wing);
    priv_session_wing_extended(sphere, extras);
    wclog(1, "Privateer: co-op: the wing at nav %d (%d %s) gets %d more of its own", sphere, count, cast.c_str(), extras);
}

void wings_flight_changed() {
    g_wingsExtended.clear();
}

// A ship made angry at the player, as a hit from the player does (seg053:116A: its pilot -- the plan's +0x23, near --
// has +0x1A raised past the threshold at ds:0E1E and +0x19 bit 1 set); at the frame (the plan is far data)
enum { kPilotAnger = 0x1A, kPilotMood = 0x19, kAngerThreshold = 0x0E1E };
std::vector<Bit16u> g_angers;

bool g_pilotDump;
extern Bit16u g_lastSpawned;
static void pilot_dump_frame() {
    if (!g_pilotDump) return;
    g_pilotDump = false;
    const Bit16u ship = g_lastSpawned;
    const Bit16u ctl = priv_r16((Bit16u)(ship + 0x4C));
    const Bit16u poff = ctl ? priv_r16((Bit16u)(ctl + 2)) : 0, pseg = ctl ? priv_r16((Bit16u)(ctl + 4)) : 0;
    Bit16u pilot = 0;
    if (pseg) mem_readw_checked(((PhysPt)pseg << 4) + (Bit16u)(poff + kPlanPilot), &pilot);
    std::string hex;
    for (int i = 0; pilot && i < 0x40; i++) {
        char b[4];
        snprintf(b, sizeof b, "%02X", priv_r8((Bit16u)(pilot + i)));
        hex += b;
        if (i % 2 == 1) hex += ' ';
    }
    wclog(1, "Privateer: pilot of ds:%04X at ds:%04X: %s", ship, pilot, hex.c_str());
}

// (a test aid: PRIVCOMMLOG=1 logs the game's comm line object, ds:67E4..67F7, whenever it changes: +0 the speaker (its
// ship's controller + 8), +2 the line (far), +6 its category, +7 the line's time, +0xB bit 0 a line on, bit 1 free,
// +0xC the "NAME: text" string (near), +0xE bit 1 shown, +0xF the string's time; and the speaker's voice, +0xA)
static void comm_log_frame() {
    static int on = -1;
    if (on < 0) on = getenv("PRIVCOMMLOG") ? 1 : 0;
    if (!on) return;
    static std::string last;
    std::string hex;
    for (int i = 0; i < 0x13; i++) {
        char b[4];
        snprintf(b, sizeof b, "%02X", priv_r8((Bit16u)(kComm + i)));
        hex += b;
    }
    const Bit16u k = priv_r16(kComm);
    char more[96] = "";
    if (k) { const Bit16u c = priv_ship_controller(k); if (c) snprintf(more, sizeof more, " frame %u voice %04X", priv_r16((Bit16u)(c + kCtlComm + 8)), priv_r16((Bit16u)(c + kCtlComm + 0xA))); }
    const Bit16u str = priv_r16((Bit16u)(kComm + 0xC));
    std::string text;
    for (int i = 0; str && i < 60; i++) {
        const Bit8u c = priv_r8((Bit16u)(str + i));
        if (!c) break;
        text += (char)c;
    }
    const std::string now = hex + more + " \"" + text + "\"";
    if (now != last) {
        wclog(1, "Privateer: comm %s (speech.pak %d, digital %d)", now.c_str(), priv_r8(0x13EB), priv_r8(0x6930));
        last = now;
    }
}

static void anger_frame() {
    for (size_t i = 0; i < g_angers.size(); i++) {
        const Bit16u ship = g_angers[i];
        if (!priv_ship_is_alive(ship)) continue;
        const Bit16u ctl = priv_r16((Bit16u)(ship + 0x4C));
        const Bit16u poff = ctl ? priv_r16((Bit16u)(ctl + 2)) : 0, pseg = ctl ? priv_r16((Bit16u)(ctl + 4)) : 0;
        if (!pseg) continue;
        Bit16u pilot = 0;
        if (mem_readw_checked(((PhysPt)pseg << 4) + (Bit16u)(poff + kPlanPilot), &pilot) || !pilot) continue;
        const Bit8u threshold = priv_r8(kAngerThreshold);
        if (priv_r8((Bit16u)(pilot + kPilotAnger)) <= threshold) priv_w8((Bit16u)(pilot + kPilotAnger), (Bit8u)(threshold + 1));
        priv_w8((Bit16u)(pilot + kPilotMood), (Bit8u)(priv_r8((Bit16u)(pilot + kPilotMood)) | 2));
        wclog(2, "Privateer: ds:%04X's pilot (ds:%04X) angry at the player (anger %d over %d)", ship, pilot, priv_r8((Bit16u)(pilot + kPilotAnger)), threshold);
    }
    g_angers.clear();
}

// A test aid's scan for where a ship's AI keeps its target (!privtargetscan=<value>, run at the next frame: the plan
// is far data): the last spawned ship's object, controller and plan searched for a word (our ship by default)
int g_targetScan = -1;
extern Bit16u g_lastSpawned, g_prevSpawned;
static void target_scan_frame() {
    if (g_targetScan < 0) return;
    const Bit16u own = g_targetScan == 1 ? g_prevSpawned : priv_r16(kPlayerShip);     // (=prev: the ship spawned before the last)
    g_targetScan = -1;
    // what of ours the AI may hold: the ship (and its allocation), its controller, the player record and its +0x2C
    Bit16u wants[12];
    int nw = 0;
    wants[nw++] = own;
    wants[nw++] = (Bit16u)(own - 2);
    wants[nw++] = priv_r16((Bit16u)(own + 0x4C));
    wants[nw++] = priv_r16(kPlayerRecord);
    wants[nw++] = (Bit16u)(priv_r16(kPlayerRecord) + 0x2C);
    wants[nw++] = priv_r16((Bit16u)(own + 0x58));       // our damage model (far: its segment word)
    wants[nw++] = priv_r16((Bit16u)(priv_r16((Bit16u)(own + 0x4C)) + 0xC));   // our cast copy (far: its segment)
    wants[nw++] = priv_r16((Bit16u)(priv_r16((Bit16u)(own + 0x4C)) + 0x4));   // our plan (far: its segment)
    const Bit16u ship = g_lastSpawned;
    std::string at;
    char b[48];
    auto check = [&](const char *where, int i, Bit16u w) {
        for (int k = 0; k < nw; k++) {
            if (w == wants[k] && w) { snprintf(b, sizeof b, " %s+%02X=%04X(%d)", where, i, w, k); at += b; }
        }
    };
    for (int i = 0; i < 0x90; i++) check("ship", i, priv_r16((Bit16u)(ship + i)));
    const Bit16u ctl = priv_r16((Bit16u)(ship + 0x4C));
    for (int i = 0; ctl && i < 0x80; i++) check("ctl", i, priv_r16((Bit16u)(ctl + i)));
    const Bit16u poff = ctl ? priv_r16((Bit16u)(ctl + 2)) : 0, pseg = ctl ? priv_r16((Bit16u)(ctl + 4)) : 0;
    for (int i = 0; pseg && i < 0x400; i++) {
        Bit16u w = 0;
        if (mem_readw_checked(((PhysPt)pseg << 4) + (Bit16u)(poff + i), &w)) break;
        check("plan", i, w);
    }
    // the near objects the plan points at (a word pair DS-segment far pointer in the plan)
    for (int i = 0; pseg && i + 3 < 0x400; i += 2) {
        Bit16u o = 0, sg = 0;
        mem_readw_checked(((PhysPt)pseg << 4) + (Bit16u)(poff + i), &o);
        mem_readw_checked(((PhysPt)pseg << 4) + (Bit16u)(poff + i + 2), &sg);
        if (sg == g_ds && o > 0x6C00 && o < 0xF000) {
            for (int j = 0; j < 0x40; j++) {
                const Bit16u w = priv_r16((Bit16u)(o + j));
                for (int k = 0; k < nw; k++) {
                    if (w == wants[k] && w) { snprintf(b, sizeof b, " plan+%02X->%04X+%02X(%d)", i, o, j, k); at += b; }
                }
            }
        }
    }
    wclog(1, "Privateer: target scan of ds:%04X (controller %04X, plan %04X:%04X) for %04X/%04X/%04X/%04X/%04X/%04X/%04X/%04X:%s", ship, ctl, pseg,
          poff, wants[0], wants[1], wants[2], wants[3], wants[4], wants[5], wants[6], wants[7], at.empty() ? " nothing" : at.c_str());
}

static void stack_paint_load() {
    // at the program's load, before anything ran: the stack's place below its top 1 KB
    if (!getenv("PRIVSTACK")) return;
    const Bit16u stklen = priv_r16(kStackLen);
    g_paint.from = (Bit16u)((0xFFF6 - stklen) & ~1) + 16;
    g_paint.to = 0xFFF6 - 1024;
    for (Bit32u a = g_paint.from; a < g_paint.to; a++) priv_w8((Bit16u)a, kStackPaint);
    g_paint.painted = true;
    g_paint.deepest = 1 << 30;
    wclog(1, "Privateer: stack painted %04X-%04X at the load (_stklen %04X: the stack %04X-FFF6; the heap from [0163] %04X)", g_paint.from,
          g_paint.to, stklen, (Bit16u)(0xFFF6 - stklen), priv_r16(0x0163));
}

static void stack_paint_frame() {
    if (g_paint.painted) {
        Bit32u a = g_paint.from;
        while (a < g_paint.to && priv_r8((Bit16u)a) == kStackPaint) a++;
        const int headroom = (int)a - (int)g_paint.from;
        if (headroom < g_paint.deepest) {
            g_paint.deepest = headroom;
            wclog(1, "Privateer: the stack has been within %d bytes of the heap's end (%04X; painted down to %04X, SP now %04X)", headroom + 16,
                  (Bit16u)a, g_paint.from, reg_sp);
        }
    }
}

struct AllocWatch {
    int largest, largestSecond, count;
    int headroomGame, headroomOurs;     // the lowest seen (1 << 30: none yet)
    AllocWatch() : largest(0), largestSecond(0), count(0), headroomGame(1 << 30), headroomOurs(1 << 30) {}
} g_alloc;
std::vector<Bit16u> g_heldAi;
// A held controller let think for some frames: a puppet's line is played (its text on the comm line) only as its
// own AI's think runs; the ship's place and attitude are ours again at the next frame, and its guns are kept quiet
std::vector<std::pair<Bit16u, int> > g_thinkFrames;

// A hit applied in steps, each a call of the game's (priv_hit_apply): the shields, the armour, the display, the
// death check.
struct HitJob {
    Bit16u obj, modelOff, modelSeg;
    int loc, damage, left;
    bool ours;                  // the player's own ship (the cockpit's display)
};
std::vector<HitJob> g_hits;     // (by tag; a slot is reused when its job is done)
void hit_step(int tag, Bit16u ax, Bit16u dx);
void hit_cancel(int tag) {
    if (tag >= 0 && tag < (int)g_hits.size()) g_hits[tag].obj = 0;
}

bool queue_call(const PrivCall &c);
void hit_call(int tag, Bit16u para, Bit16u off, const std::vector<Bit32u> &args) {
    PrivCall c;
    c.para = para;
    c.off = off;
    c.args = args;
    c.done = hit_step;
    c.cancel = hit_cancel;
    c.guardObj = g_hits[tag].obj;
    c.guardVtable = kShipVtable;
    c.guardModelOff = g_hits[tag].modelOff;
    c.guardModelSeg = g_hits[tag].modelSeg;
    c.tag = tag;
    if (!queue_call(c)) {
        g_hits[tag].obj = 0;
    }
}

// tag's step done: the result ax (dx) of the last call; the next call queued.  The off/seg words of the call's data
// tell which step it was by the job's own state (left < 0: the shields' call is out).
void hit_step(int tag, Bit16u ax, Bit16u dx) {
    (void)dx;
    if (tag < 0 || tag >= (int)g_hits.size() || !g_hits[tag].obj) {
        return;
    }
    HitJob &h = g_hits[tag];
    std::vector<Bit32u> a;
    if (h.left == -1) {
        // the shields' word: what they let through
        h.left = (Bit16s)ax;
        if (h.left > 0) {
            a.push_back(h.modelOff); a.push_back(h.modelSeg); a.push_back((Bit16u)h.loc); a.push_back((Bit16u)h.left);
            hit_call(tag, kSeg101, kModelArmorHit, a);
            h.left = -2;
            return;
        }
        h.left = -3;        // (nothing through: no armour call; the display still)
    }
    if (h.left == -2 || h.left == -3) {
        h.left = -4;
        if (h.ours) {
            a.push_back(kDamageDisplayObj); a.push_back((Bit16u)h.loc); a.push_back((Bit16u)h.damage);
            hit_call(tag, kSeg034, kDamageDisplay, a);
            return;
        }
    }
    if (h.left == -4) {
        h.left = -5;
        a.push_back(h.modelOff); a.push_back(h.modelSeg);
        hit_call(tag, kSeg101, kModelIntact, a);
        return;
    }
    if (h.left == -5) {
        if ((ax & 0xff) == 0 && priv_ship_is_alive(h.obj)) {
            priv_ship_kill(h.obj);
            wclog(1, "Privateer: a hit from another machine destroyed ds:%04X", h.obj);
        }
        h.obj = 0;
    }
}
Bit16u g_lastSpawned, g_prevSpawned;
void spawn_done(int tag, Bit16u ax, Bit16u dx) {
    (void)dx;
    g_prevSpawned = g_lastSpawned;
    g_lastSpawned = ax;
    wclog(1, "Privateer: spawn %d made the object ds:%04X", tag, ax);
}

// The cast names' slots (above): the slot of a cast (one of the 16, the oldest taken over when they are all used: a
// ship made long ago with that slot's old cast then reads the new one, as all of them did before)
int cast_slot(const char *cast) {
    for (int i = 0; i < kCastSlots; i++) {
        if (g_castNames[i] == cast) return i;
    }
    const int slot = g_castNext;
    g_castNext = (g_castNext + 1) % kCastSlots;
    g_castNames[slot] = cast;
    return slot;
}

void cast_block_made(int tag, Bit16u ax, Bit16u dx) {
    (void)tag; (void)dx;
    g_castBlock = ax;
    wclog(1, "Privateer: our ships' cast names at ds:%04X (%d slots)%s", ax, (int)kCastSlots, ax ? "" : ": none given, the old scratch");
}

// The slot's name written (each time a call uses it: cheap, and the block is ours alone) and its address
Bit16u cast_slot_address(int slot) {
    if (slot < 0 || slot >= kCastSlots) {
        return 0;
    }
    const std::string &name = g_castNames[slot];
    const Bit16u at = g_castBlock ? (Bit16u)(g_castBlock + kCastSlotBytes * slot) : (Bit16u)kScratch;
    for (int i = 0; i < kCastSlotBytes; i++) {
        priv_w8((Bit16u)(at + i), (Bit8u)(i < (int)name.size() && i < 8 ? name[i] : 0));
    }
    return at;
}

void cast_block_cancel(int tag) {
    (void)tag;
    g_castBlockAsked = false;   // (dropped with a flight's calls: asked again at the next spawn)
}

bool queue_call(const PrivCall &c);
// The block asked for (once a program run) before the first spawn's call
void cast_block_ask() {
    if (g_castBlock || g_castBlockAsked) {
        return;
    }
    PrivCall c;
    c.para = 0;
    c.off = kNearMallocRoot;
    c.args.push_back(kCastSlots * kCastSlotBytes);
    c.done = cast_block_made;
    c.cancel = cast_block_cancel;
    if (queue_call(c)) g_castBlockAsked = true;
}

// A test aid: PRIVWATCH=seg053:064B,seg029:0277,... (root segments, scripts/wcexe.py's names) logs each call of those
// places with its caller (the far return address, as a segment name) and its first four argument words.
struct WatchPlace {
    Bit16u para, off;
    std::string name;
};
std::vector<WatchPlace> g_watchPlaces;
bool g_watchParsed;

const std::vector<WatchPlace> &watch_places() {
    if (!g_watchParsed) {
        g_watchParsed = true;
        const char *env = getenv("PRIVWATCH");
        std::string all = env ? env : "";
        size_t pos = 0;
        while (pos < all.size()) {
            size_t comma = all.find(',', pos);
            if (comma == std::string::npos) comma = all.size();
            std::string tok = all.substr(pos, comma - pos);
            unsigned para = 0, off = 0;
            // "para:off" in hex (the paragraph from scripts/wcexe.py segs)
            if (sscanf(tok.c_str(), "%x:%x", &para, &off) == 2) {
                WatchPlace w = { (Bit16u)para, (Bit16u)off, tok };
                g_watchPlaces.push_back(w);
            }
            pos = comma + 1;
        }
    }
    return g_watchPlaces;
}

// (a far pointer's text, for the watch's log: up to 12 printable characters, or "")
std::string far_text(Bit16u off, Bit16u seg) {
    std::string out;
    for (int i = 0; i < 12; i++) {
        const Bit8u c = mem_readb(((PhysPt)seg << 4) + (Bit16u)(off + i));
        if (!c) break;
        if (c < 32 || c >= 127) return "";
        out += (char)c;
    }
    return out;
}

void log_watched(const WatchPlace &w) {
    // (PRIVWATCH_TRACE=<seconds>: the first watched call starts a code trace of that long, "watched.trace")
    static bool traced;
    const char *tr = getenv("PRIVWATCH_TRACE");
    if (tr && tr[0] && !traced) {
        traced = true;
        g_traceName = "watched";
        g_traceEndMs = PIC_FullIndex() + 1000.0 * atof(tr);
        g_tracing = true;
        g_trace.clear();
        wc_net_countdown = 0;
    }
    const PhysPt sp = SegPhys(ss) + (reg_esp & 0xffff);
    const Bit16u retIp = mem_readw(sp), retCs = mem_readw(sp + 2);
    Bit16u a[6];
    for (int i = 0; i < 6; i++) {
        a[i] = mem_readw(sp + 4 + 2 * i);
    }
    wclog(1, "Privateer: watch %s from %04X:%04X (para %04X) args %04X %04X %04X %04X %04X %04X (\"%s\" \"%s\"); si %04X di %04X",
          w.name.c_str(), retCs, retIp, (Bit16u)(retCs - g_privLoadSeg), a[0], a[1], a[2], a[3], a[4], a[5],
          far_text(a[1], a[2]).c_str(), far_text(a[3], a[4]).c_str(), reg_si, reg_di);
    // (PRIVWATCH_FAR=1: the first argument as a far pointer, 0x60 bytes of it, as words)
    const char *far = getenv("PRIVWATCH_FAR");
    if (far && far[0] == '1') {
        char line[400];
        int n = 0;
        for (int i = 0; i < 0x30 && n < (int)sizeof(line) - 6; i++) {
            n += snprintf(line + n, sizeof(line) - n, "%04X ", mem_readw(((PhysPt)a[1] << 4) + (Bit16u)(a[0] + 2 * i)));
        }
        wclog(1, "Privateer:   %04X:%04X: %s", a[1], a[0], line);
    }
}

}  // namespace

bool priv_found() { return g_found; }
Bit16u priv_load_seg() { return g_found ? g_privLoadSeg : 0; }
Bit16u priv_ds() { return g_found ? g_ds : 0; }
bool priv_righteous_fire() { return g_found && g_rf; }

Bit8u priv_r8(Bit16u off) { return g_found ? mem_readb(ds_at(off)) : 0; }
Bit16u priv_r16(Bit16u off) { return g_found ? mem_readw(ds_at(off)) : 0; }
Bit32u priv_r32(Bit16u off) { return g_found ? mem_readd(ds_at(off)) : 0; }
void priv_w8(Bit16u off, Bit8u v) { if (g_found) mem_writeb(ds_at(off), v); }
void priv_w16(Bit16u off, Bit16u v) { if (g_found) mem_writew(ds_at(off), v); }

void priv_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    if (strcmp(name, "PRCD.EXE") != 0) {
        return;
    }
    const Bit16u ds = (Bit16u)(loadSeg + kDgroupPara);
    for (size_t i = 0; i < sizeof(kSignature) - 1; i++) {
        if (mem_readb(((PhysPt)ds << 4) + kSignatureOff + i) != (Bit8u)kSignature[i]) {
            wclog(0, "PRCD.EXE is not the Privateer build the hooks know (GOG's CD-ROM edition): not watched");
            return;
        }
    }
    // The command line (PSP:80): PRIV.EXE passes "r" for Righteous Fire.
    std::string tail;
    const Bit8u len = mem_readb(((PhysPt)psp << 4) + 0x80);
    for (Bit8u i = 0; i < len && i < 126; i++) {
        tail += (char)mem_readb(((PhysPt)psp << 4) + 0x81 + i);
    }
    g_rf = false;
    for (size_t i = 0; i < tail.size(); i++) {
        if ((tail[i] == 'r' || tail[i] == 'R') && (i == 0 || tail[i - 1] == ' ') && (i + 1 == tail.size() || tail[i + 1] == ' ')) {
            g_rf = true;
        }
    }
    g_found = true;
    g_wingArrays.clear();       // (a new run of the program: a new heap)
    g_wingFrees.clear();
    g_wingArrayAsked = false;
    g_wingsExtended.clear();
    g_castBlock = 0;
    g_castBlockAsked = false;
    for (int i = 0; i < kCastSlots; i++) g_castNames[i].clear();
    g_castNext = 0;
    g_privLoadSeg = loadSeg;
    g_ds = ds;
    g_psp = psp;
    g_nextReportMs = PIC_FullIndex() + 1000;
    stack_paint_load();
    hooks_game_changed();       // (the dispatcher's table: our places)
    wclog(1, "Privateer%s: PRCD.EXE loaded at %04X (PSP %04X, data segment %04X), command line \"%s\"",
          g_rf ? " (Righteous Fire)" : "", loadSeg, psp, ds, tail.c_str());
}

bool priv_program_ended(Bit16u psp) {
    if (!g_found || psp != g_psp) {
        return false;
    }
    wclog(1, "Privateer: PRCD.EXE ended");
    g_found = false;
    g_ds = g_privLoadSeg = g_psp = 0;
    hooks_game_changed();
    return true;
}

extern int g_heapLowSecond;
static void watch_tick();

void priv_tick() {
    priv_session_tick();
    if (!g_found) {
        return;
    }
    const double now = PIC_FullIndex();
    if (g_tracing) {
        trace_step(now);
    }
    watch_tick();
    if (now < g_nextReportMs) {
        return;
    }
    g_nextReportMs = now + 1000;
    stack_paint_frame();
    static unsigned lastFrames;
    const unsigned frames = priv_controls_frames();
    if (frames != lastFrames) {
        int total, largest, blocks, pilots, npcs, pods;
        if (!priv_near_heap_read(total, largest, blocks)) total = largest = blocks = -1;
        priv_session_counts(pilots, npcs, pods);
        // the scene's ships alive but ours, and of them the pirates (a measure for the co-op tests)
        Bit16u ships[48];
        const int n = priv_scene_ships(ships, 48);
        int alive = 0, pirates = 0;
        for (int i = 0; i < n; i++) {
            if (ships[i] == priv_player_ship() || !priv_ship_is_alive(ships[i])) continue;
            alive++;
            if (priv_ship_cast_name(ships[i]).compare(0, 3, "PIR") == 0) pirates++;
        }
        wclog(2, "Privateer: t=%.1f, %u keyboard frames, controls: %s, near heap free %d (lowest this second %d, ever %d; largest block %d, %d blocks); "
                 "ours here: %d pilots, %d encounter ships, %d pods; %d allocations, the largest %d (ever %d); stack headroom at one: the game's %d, ours %d; "
                 "ships in the scene %d, pirates %d; in %s",
              now / 1000.0, frames - lastFrames, priv_control_mode(), total, g_heapLowSecond < (1 << 30) ? g_heapLowSecond : -1,
              priv_near_heap_lowest(), largest, blocks, pilots, npcs, pods, g_alloc.count, g_alloc.largestSecond, g_alloc.largest,
              g_alloc.headroomGame < (1 << 30) ? g_alloc.headroomGame : -1, g_alloc.headroomOurs < (1 << 30) ? g_alloc.headroomOurs : -1, alive, pirates,
              priv_in_flight() ? priv_system_name().c_str() : "(not flying)");
        g_alloc.count = 0;
        g_alloc.largestSecond = 0;
        lastFrames = frames;
    }
    g_heapLowSecond = 1 << 30;
}

// The video aid's steady turn (!privturn): each frame our matrix is rotated toward the target by at most the rate,
// about the axis perpendicular to our forward and the target's direction (the second row is forward).
struct { bool pod; double rate, until; bool pirate; } g_turn = { true, 0, -1e9, false };

static void turn_step() {
    const double now = PIC_FullIndex();
    if (now > g_turn.until || !priv_in_flight()) {
        return;
    }
    Bit16u objs[48];
    const int n = g_turn.pod ? priv_scene_pods(objs, 48) : priv_scene_ships(objs, 48);
    const Bit16u own = priv_player_ship();
    Bit32s pos[3];
    priv_player_pos(pos);
    double best = 1e18, tgt[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        if (objs[i] == own || (g_turn.pirate && priv_ship_cast_name(objs[i]).compare(0, 3, "PIR") != 0)) continue;
        if (g_turn.pirate && !priv_ship_is_alive(objs[i])) continue;
        Bit32s p[3];
        priv_ship_pos(objs[i], p);
        const double d = sqrt((double)(p[0] - pos[0]) * (p[0] - pos[0]) + (double)(p[1] - pos[1]) * (p[1] - pos[1]) + (double)(p[2] - pos[2]) * (p[2] - pos[2]));
        if (d > 256.0 * 100000) continue;       // (a parked one, far off)
        if (d < best) { best = d; for (int k = 0; k < 3; k++) tgt[k] = (p[k] - pos[k]) / d; }
    }
    if (best > 1e17) return;
    Bit32s m[9];
    priv_player_matrix(m);
    double r[3], f[3], u[3];
    for (int k = 0; k < 3; k++) { r[k] = m[k] / 256.0; f[k] = m[3 + k] / 256.0; u[k] = m[6 + k] / 256.0; }
    const double dot = f[0] * tgt[0] + f[1] * tgt[1] + f[2] * tgt[2];
    const double ang = acos(dot > 1 ? 1 : dot < -1 ? -1 : dot);
    const double step = g_turn.rate / 20.0;         // (a frame of the page's 20)
    if (ang < 0.002) return;
    double axis[3] = { f[1] * tgt[2] - f[2] * tgt[1], f[2] * tgt[0] - f[0] * tgt[2], f[0] * tgt[1] - f[1] * tgt[0] };
    const double al = sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (al < 1e-6) { axis[0] = u[0]; axis[1] = u[1]; axis[2] = u[2]; } else { for (int k = 0; k < 3; k++) axis[k] /= al; }
    const double th = ang < step ? ang : step, c = cos(th), sn = sin(th);
    double *rows[3] = { r, f, u };
    for (int i = 0; i < 3; i++) {
        double *v = rows[i];
        const double ad = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const double cr[3] = { axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0] };
        double o[3];
        for (int k = 0; k < 3; k++) o[k] = v[k] * c + cr[k] * sn + axis[k] * ad * (1 - c);
        for (int k = 0; k < 3; k++) m[3 * i + k] = (Bit32s)(o[k] * 256.0 + (o[k] < 0 ? -0.5 : 0.5));
    }
    priv_ship_set(own, pos, m);
}

bool priv_check_hooks(Bit32u ip) {
    if (!g_found) {
        return false;
    }
    const Bit16u codeSeg = (Bit16u)SegValue(cs);
    if (ip == kAfterKeyboard && codeSeg == (Bit16u)(g_privLoadSeg + kSeg088)) {
        priv_controls_frame();
        return true;
    }
    if (ip == kControlsEntry && codeSeg == (Bit16u)(g_privLoadSeg + kSeg088)) {
        g_frameSp = reg_sp;
        if (!g_saved.active) {
            pilot_dump_frame();
            comm_log_frame();
            wing_arrays_frame();
            anger_frame();
            retarget_frame();
            target_scan_frame();
            stack_paint_frame();
            turn_step();
            priv_spheres_refresh();
            priv_session_frame();       // (once a frame in flight: the puppets placed, our state sent)
        }
        run_calls();
        return true;
    }
    if (ip == kDeathEject && codeSeg == (Bit16u)(g_privLoadSeg + kSeg053)) {
        const Bit16u ship = (Bit16u)(reg_di + 2);       // (DI is the ship's allocation; our pointers are 2 bytes in)
        if (priv_session_is_puppet(ship, 0)) {
            reg_eip = kDeathEjectAfter;     // (its add sp,8 cleans the call's arguments)
            static unsigned skipped;
            if (++skipped % 10 == 1) {
                wclog(2, "Privateer: a puppet's death (ds:%04X) drops no cargo here: its owner's game shares the real pods (%u so far)", ship, skipped);
            }
        }
        return true;
    }
    if (ip == kNearMalloc && codeSeg == (Bit16u)(g_privLoadSeg + kSeg142)) {
        const int size = mem_readw(SegPhys(ss) + reg_sp + 4);
        const int headroom = (int)reg_sp - (int)priv_r16(kNearHeapTop);
        if (size > g_alloc.largest) g_alloc.largest = size;
        if (size > g_alloc.largestSecond) g_alloc.largestSecond = size;
        g_alloc.count++;
        int &low = g_saved.active ? g_alloc.headroomOurs : g_alloc.headroomGame;
        if (headroom < low) low = headroom;
        return true;
    }
    if (ip == kGameError && codeSeg == (Bit16u)(g_privLoadSeg + kSeg050)) {
        const Bit16u code = mem_readw(SegPhys(ss) + reg_sp + 4);
        int total, largest, blocks;
        priv_near_heap(total, largest, blocks);
        wclog(1, "Privateer: the game reports error %04X%s (near heap free %d, largest block %d)", code,
              code == 0x1303 ? ": its near heap has no block big enough (out of memory: the program ends)" : "", total, largest);
        return true;
    }
    if (ip == kWingSpawn && codeSeg == (Bit16u)(g_privLoadSeg + kSeg077)) {
        const PhysPt sp = SegPhys(ss) + reg_sp;
        const Bit16u off = mem_readw(sp + 4), seg = mem_readw(sp + 6);
        const int sphere = mem_readb(((PhysPt)seg << 4) + (Bit16u)(off + kWingSphere));
        static unsigned wingCalls;
        const bool allowed = priv_session_allow_wing(sphere);
        wclog(2, "Privateer: the game's wing spawn at nav %d (call %u, flags %02X): %s", sphere, ++wingCalls,
              mem_readb(((PhysPt)seg << 4) + (Bit16u)(off + 0x2A)), allowed ? "allowed" : "refused");
        if (!allowed) {
            // (the wing stays armed: it spawns when this machine owns the nav point)
            reg_eip = mem_readw(sp);
            SegSet16(cs, mem_readw(sp + 2));
            reg_sp += 4;
        } else {
            wing_extend((((PhysPt)seg << 4) + off), sphere);
        }
        return true;
    }
    if (ip == kHitPlace && codeSeg == (Bit16u)(g_privLoadSeg + kSeg053)) {
        const PhysPt bp = SegPhys(ss) + reg_bp;
        const Bit16u target = mem_readw(bp + 0xA), hitter = mem_readw(bp + 0xE);
        // (a ship bumping a ship: harmless when either is another machine's, as RetroVega's wing bumps are: two
        // pilots launch from the same pad into the same place)
        if (hitter && priv_r16(hitter) == kShipVtable && priv_session_is_puppet(target, hitter)) {
            mem_writew(bp - 6, 0);
        }
        if (wclog_level() >= 3) {
            wclog(3, "Privateer: hit on ds:%04X (%s) by ds:%04X (vtable %04X): quadrant %d, %d", target, priv_ship_cast_name(target).c_str(), hitter,
                  hitter ? priv_r16(hitter) : 0, (Bit16s)mem_readw(bp + 0xC), (Bit16s)mem_readw(bp - 6));
        }
        priv_session_hit(target, (Bit16s)mem_readw(bp + 0xC), (Bit16s)mem_readw(bp - 6));
        // a puppet takes no damage here: its owner's game applies the hit (sent above) and says how it stands (else it
        // would die here alone, explode and be made again over and over under fire)
        if (priv_session_is_puppet(target, 0)) {
            mem_writew(bp - 6, 0);
        }
        return true;
    }
    if (ip == kFireGuns && codeSeg == (Bit16u)(g_privLoadSeg + kSeg120)) {
        const PhysPt sp = SegPhys(ss) + reg_sp;
        const Bit16u off = mem_readw(sp + 4), seg = mem_readw(sp + 6);
        priv_session_fired(mem_readw(((PhysPt)seg << 4) + (Bit16u)(off + 0xA)));
        return true;
    }
    if (ip == kNpcThink && codeSeg == (Bit16u)(g_privLoadSeg + kSeg042) && !g_heldAi.empty()) {
        const PhysPt sp = SegPhys(ss) + reg_sp;
        const Bit16u self = mem_readw(sp + 4);
        if (std::find(g_heldAi.begin(), g_heldAi.end(), self) != g_heldAi.end()) {
            for (size_t i = 0; i < g_thinkFrames.size(); i++) {
                if (g_thinkFrames[i].first == self) {
                    if (--g_thinkFrames[i].second <= 0) g_thinkFrames.erase(g_thinkFrames.begin() + i);
                    return false;           // (let think this frame)
                }
            }
            // a puppet's AI: its think returns at once, as it would (AL 1), and the ship flies as its reports say
            reg_al = 1;
            reg_eip = mem_readw(sp);
            SegSet16(cs, mem_readw(sp + 2));
            reg_sp += 4;
            return true;
        }
        return false;
    }
    const std::vector<WatchPlace> &w = watch_places();
    for (size_t i = 0; i < w.size(); i++) {
        if (ip == w[i].off && codeSeg == (Bit16u)(g_privLoadSeg + w[i].para)) {
            log_watched(w[i]);
            return true;
        }
    }
    return false;
}

void priv_watch_list(void (*watch)(Bit16u ip)) {
    watch(kAfterKeyboard);
    watch(kControlsEntry);
    watch(kNpcThink);
    watch(kWingSpawn);
    watch(kGameError);
    watch(kNearMalloc);
    watch(kDeathEject);
    watch(kHitPlace);
    watch(kFireGuns);
    const std::vector<WatchPlace> &w = watch_places();
    for (size_t i = 0; i < w.size(); i++) {
        watch(w[i].off);
    }
}

static std::string ds_name(Bit16u at, int max);
static bool far_of(Bit16u at, PhysPt *out);
static bool far_at(PhysPt at, PhysPt *out);

bool priv_script_item(const std::string &item, double t) {
    if (item.compare(0, 8, "!privds=") == 0 && g_found) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        std::string path = std::string(dir && dir[0] ? dir : ".") + "/" + item.substr(8) + ".ds";
        FILE *f = fopen(path.c_str(), "wb");
        if (f) {
            for (Bit32u off = 0; off < 0x10000; off++) {
                fputc(priv_r8((Bit16u)off), f);
            }
            fclose(f);
        }
        wclog(1, "key script %.1fs: Privateer's data segment (%04X) to %s%s", t, g_ds, path.c_str(), f ? "" : " failed");
        return true;
    }
    if (item.compare(0, 14, "!privwatchctl=") == 0 && g_found) {
        unsigned off = 0;
        double secs = 1;
        sscanf(item.c_str() + 14, "%x:%lf", &off, &secs);
        const Bit16u ctl = priv_ship_controller(priv_last_spawned());
        g_watch = ctl ? (Bit16u)(ctl + off) : 0;
        g_watchLast = g_watch ? priv_r16(g_watch) : 0;
        g_traceName = "watch";
        g_traceEndMs = PIC_FullIndex() + 1000.0 * secs;
        g_tracing = g_watch != 0;
        g_trace.clear();
        wc_net_countdown = 0;
        wclog(1, "key script %.1fs: watching ds:%04X (%04X) for %.1f s", t, g_watch, g_watchLast, secs);
        return true;
    }
    if (item.compare(0, 11, "!privtrace=") == 0 && g_found) {
        // !privtrace=<name>:<seconds>
        std::string spec = item.substr(11);
        size_t colon = spec.find(':');
        g_traceName = spec.substr(0, colon);
        g_traceEndMs = PIC_FullIndex() + 1000.0 * (colon == std::string::npos ? 1.0 : atof(spec.c_str() + colon + 1));
        g_tracing = true;
        g_trace.clear();
        wc_net_countdown = 0;
        wclog(1, "key script %.1fs: Privateer's code traced to %s.trace", t, g_traceName.c_str());
        return true;
    }
    if (item.compare(0, 11, "!privspawn=") == 0 && g_found) {
        // !privspawn=<type>:<appearance>:<cast>:<dx>:<dy>:<dz>: a ship of that type placed that far from ours (world
        // units), our attitude, by the game's own seg099:0074 (TARSTYPE:CLUNKER:MER_DP: a merchant's Tarsus)
        char type[16] = "", app[16] = "", cast[16] = "";
        int dx = 0, dy = 0, dz = 0;
        sscanf(item.c_str() + 11, "%15[^:]:%15[^:]:%15[^:]:%d:%d:%d", type, app, cast, &dx, &dy, &dz);
        const Bit16u ship = priv_r16(kPlayerShip);
        if (!ship) {
            wclog(1, "key script %.1fs: !privspawn: not in flight", t);
            return true;
        }
        Bit32s pos[3], m[9];
        priv_player_pos(pos);
        pos[0] += dx;
        pos[1] += dy;
        pos[2] += dz;
        priv_player_matrix(m);
        static int spawns;
        const bool ok = priv_spawn_ship(type, app, cast, pos, m, NULL, ++spawns);
        wclog(1, "key script %.1fs: spawn %d %s: %s (%s, cast %s) at %d, %d, %d from us", t, spawns, ok ? "queued" : "refused", type, app, cast, dx, dy, dz);
        return true;
    }
    if (item.compare(0, 8, "!privdel") == 0 && g_found) {
        // !privdel=mode: the last ship a spawn of ours made is deleted (0: the destructor alone; 1: out of the scene,
        // then the destructor; 2: out of the scene, its owner word cleared, the destructor)
        const Bit16u ship = priv_last_spawned();
        const int mode = item.size() > 9 ? atoi(item.c_str() + 9) : 1;
        wclog(1, "key script %.1fs: ds:%04X deleted, mode %d (near heap free %d)", t, ship, mode, priv_near_free());
        if (priv_ship_is_alive(ship)) priv_ship_delete(ship, mode);     // (every mode has crashed the game so far: section 12)
        return true;
    }
    if (item.compare(0, 10, "!privscene") == 0 && g_found) {
        // !privscene: every object of the scene list (ds:66CE): a ship with its names, place and life; anything else
        // with its first 48 bytes
        Bit16u node = priv_r16((Bit16u)(kSceneList + 2));
        int n = 0;
        for (int guard = 0; node && guard < 256; guard++, n++) {
            const Bit16u obj = priv_r16((Bit16u)(node + 2));
            if (obj && priv_r16(obj) == kShipVtable) {
                Bit32s p[3];
                priv_ship_pos(obj, p);
                wclog(1, "key script %.1fs: scene ds:%04X ship %s (%s, %s) at %d, %d, %d m, alive %d%s", t, obj, priv_ship_type_name(obj).c_str(),
                      priv_ship_appearance_name(obj).c_str(), priv_ship_cast_name(obj).c_str(), p[0] / 256, p[1] / 256, p[2] / 256,
                      priv_ship_is_alive(obj), obj == priv_player_ship() ? " (ours)" : "");
            } else if (priv_pod_is(obj)) {
                std::vector<std::vector<Bit8u> > records;
                int units = 0;
                Bit32s p[3];
                priv_ship_pos(obj, p);
                std::string hex;
                if (priv_pod_cargo(obj, records, units)) {
                    for (size_t r = 0; r < records.size(); r++) {
                        std::string name;
                        for (int i = 0; i < 12 && records[r][i]; i++) name += (char)records[r][i];
                        hex += "\"" + name + "\" ";
                        for (size_t i = kCargoRecordAt; i < records[r].size(); i++) {
                            char b[4];
                            snprintf(b, sizeof b, "%02X", records[r][i]);
                            hex += b;
                            if (i % 2 == 1) hex += ' ';
                        }
                        hex += "| ";
                    }
                }
                wclog(1, "key script %.1fs: scene ds:%04X pod %s (%s) at %d, %d, %d m, %d units, record %s", t, obj, priv_ship_type_name(obj).c_str(),
                      priv_ship_appearance_name(obj).c_str(), p[0] / 256, p[1] / 256, p[2] / 256, units, hex.empty() ? "(none)" : hex.c_str());
                // the record's first far pointer: what it points at (0x40 bytes)
                PhysPt node, rec, target;
                std::string hex2;
                if (far_of((Bit16u)(obj + kPodList + 2), &node) && far_at(node + 4, &rec) && far_at(rec, &target)) {
                    for (int i = 0; i < 0x40; i++) {
                        Bit8u b = 0;
                        char bb[4];
                        if (mem_readb_checked(target + i, &b)) break;
                        snprintf(bb, sizeof bb, "%02X", b);
                        hex2 += bb;
                        if (i % 2 == 1) hex2 += ' ';
                    }
                    wclog(1, "key script %.1fs:   its record's +0 -> %05X: %s", t, (unsigned)target, hex2.c_str());
                }
            } else if (obj) {
                std::string hex, rec;
                for (int i = 0; i < 0x70; i++) {
                    char b[4];
                    snprintf(b, sizeof b, "%02X", priv_r8((Bit16u)(obj + i)));
                    hex += b;
                    if (i % 2 == 1) hex += ' ';
                }
                const Bit16u type = priv_r16((Bit16u)(obj + kShipType));
                for (int i = 0; type && i < 64; i++) {
                    char b[4];
                    snprintf(b, sizeof b, "%02X", priv_r8((Bit16u)(type + i)));
                    rec += b;
                    if (i % 2 == 1) rec += ' ';
                }
                Bit32s p[3];
                priv_ship_pos(obj, p);
                wclog(1, "key script %.1fs: scene ds:%04X vtable %04X: %s", t, obj, priv_r16(obj), hex.c_str());
                wclog(1, "key script %.1fs:   as a ship: type %s, appearance %s, name at +0xE \"%s\", at %d, %d, %d m; its +6 record ds:%04X: %s", t,
                      priv_ship_type_name(obj).c_str(), priv_ship_appearance_name(obj).c_str(), ds_name(priv_r16((Bit16u)(obj + 0xE)), 16).c_str(),
                      p[0] / 256, p[1] / 256, p[2] / 256, type, rec.c_str());
            }
            node = priv_r16(node);
        }
        wclog(1, "key script %.1fs: %d objects in the scene", t, n);
        return true;
    }
    if (item.compare(0, 9, "!privhold") == 0 && g_found) {
        // !privhold: the last ship a spawn of ours made has its AI held, as a puppet's is
        const Bit16u ship = priv_last_spawned();
        wclog(1, "key script %.1fs: ds:%04X's AI (controller ds:%04X) held", t, ship, priv_ship_controller(ship));
        if (priv_ship_is_alive(ship)) priv_hold_ai(priv_ship_controller(ship), true);
        return true;
    }
    if (item.compare(0, 9, "!privaim=") == 0 && g_found) {
        // !privaim=<metres>:<variant>: our ship put that far from the nearest cargo pod, turned to face it (a test aid:
        // the variant says which of the matrix's rows or columns is "forward", 0..5, until the convention is known)
        int metres = 150, variant = 0;
        sscanf(item.c_str() + 9, "%d:%d", &metres, &variant);
        Bit16u pods[16];
        const int n = priv_scene_pods(pods, 16);
        Bit32s pos[3];
        priv_player_pos(pos);
        Bit16u best = 0;
        double bestD = 1e18;
        for (int i = 0; i < n; i++) {
            Bit32s p[3];
            priv_ship_pos(pods[i], p);
            const double d = sqrt((double)(p[0] - pos[0]) * (p[0] - pos[0]) + (double)(p[1] - pos[1]) * (p[1] - pos[1]) + (double)(p[2] - pos[2]) * (p[2] - pos[2]));
            if (d < bestD) { bestD = d; best = pods[i]; }
        }
        if (!best) {
            wclog(1, "key script %.1fs: !privaim: no pod in the scene", t);
            return true;
        }
        Bit32s p[3];
        priv_ship_pos(best, p);
        double f[3] = { (double)(p[0] - pos[0]), (double)(p[1] - pos[1]), (double)(p[2] - pos[2]) };
        const double len = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (len < 1) return true;
        for (int i = 0; i < 3; i++) f[i] /= len;
        // an "up" not along f, the right-hand side from it
        double u[3] = { 0, 0, 1 };
        if (fabs(f[2]) > 0.9) { u[0] = 0; u[1] = 1; u[2] = 0; }
        double r[3] = { u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0] };
        const double rl = sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        for (int i = 0; i < 3; i++) r[i] /= rl;
        u[0] = f[1] * r[2] - f[2] * r[1]; u[1] = f[2] * r[0] - f[0] * r[2]; u[2] = f[0] * r[1] - f[1] * r[0];
        // the three axes in the order the variant says, as rows (variants 0-2) or columns (3-5)
        const double *axes[3];
        switch (variant % 3) {
            case 0: axes[0] = r; axes[1] = u; axes[2] = f; break;
            case 1: axes[0] = r; axes[1] = f; axes[2] = u; break;
            default: axes[0] = f; axes[1] = r; axes[2] = u; break;
        }
        Bit32s m[9];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                const double v = axes[i][j];
                if (variant < 3) m[3 * i + j] = (Bit32s)(v * 256); else m[3 * j + i] = (Bit32s)(v * 256);
            }
        }
        Bit32s at[3];
        for (int i = 0; i < 3; i++) at[i] = (Bit32s)(p[i] - f[i] * metres * 256.0);
        priv_ship_set(priv_player_ship(), at, m);
        wclog(1, "key script %.1fs: !privaim: at %d m from pod ds:%04X (was %d m), variant %d", t, metres, best, (int)(bestD / 256), variant);
        return true;
    }
    if (item.compare(0, 12, "!privspheres") == 0 && g_found) {
        // !privspheres: every record of the system's sphere list (ds:66C2; far), 32 bytes each: the nav spheres
        // (kind 0xFF at +16), jump points, bases
        const int n = priv_r16(kSphereList);
        const Bit16u arr = priv_r16((Bit16u)(kSphereList + 2));
        for (int i = 0; i < n && i < 32; i++) {
            const Bit16u off = priv_r16((Bit16u)(arr + 4 * i)), seg = priv_r16((Bit16u)(arr + 4 * i + 2));
            std::string hex;
            for (int j = 0; seg && j < 32; j++) {
                Bit8u b = 0;
                char bb[4];
                if (mem_readb_checked(((PhysPt)seg << 4) + off + j, &b)) break;
                snprintf(bb, sizeof bb, "%02X", b);
                hex += bb;
                if (j % 2 == 1) hex += ' ';
            }
            wclog(1, "key script %.1fs: sphere %d at %04X:%04X: %s", t, i, seg, off, hex.c_str());
        }
        return true;
    }
    if (item.compare(0, 10, "!privturn=") == 0 && g_found) {
        // !privturn=<pod|ship>:<degrees a second>:<seconds>: our ship turned steadily toward the nearest pod (or other
        // ship) for that long, a video aid (the matrix's second row is "forward")
        char what[8] = "pod";
        int degs = 30, secs = 4;
        sscanf(item.c_str() + 10, "%7[a-z]:%d:%d", what, &degs, &secs);
        g_turn.pod = strcmp(what, "ship") != 0 && strcmp(what, "pirate") != 0;
        g_turn.pirate = strcmp(what, "pirate") == 0;
        g_turn.rate = degs * 3.14159265 / 180.0;
        g_turn.until = PIC_FullIndex() + 1000.0 * secs;
        wclog(1, "key script %.1fs: turning toward the nearest %s at %d degrees a second for %d s", t, g_turn.pod ? "pod" : "ship", degs, secs);
        return true;
    }
    if (item.compare(0, 12, "!privrelease") == 0 && g_found) {
        wclog(1, "key script %.1fs: one puppet to be given back (near heap free %d)", t, priv_near_free());
        priv_session_force_release();
        return true;
    }
    if (item.compare(0, 18, "!privretargetpirates=") == 0 || item.compare(0, 21, "!privretargetpirates=") == 0) {
        // !privretargetpirates=<pilot>: every live pirate of our scene (not a puppet) given that pilot's ship as target
        const int seat = atoi(item.c_str() + 21) - 1;
        const Bit16u target = priv_session_pilot_ship(seat);
        Bit16u ships[48];
        const int n = priv_scene_ships(ships, 48);
        int k = 0;
        for (int i = 0; target && i < n; i++) {
            if (!priv_ship_is_alive(ships[i]) || priv_session_is_puppet(ships[i], 0) || priv_ship_cast_name(ships[i]).compare(0, 3, "PIR")) continue;
            retarget_queue(ships[i], target);
            k++;
        }
        wclog(1, "key script %.1fs: %d pirates given pilot %d's ship ds:%04X as target", t, k, seat + 1, target);
        return true;
    }
    if (item.compare(0, 14, "!privpilotdump") == 0 && g_found) {
        g_pilotDump = true;         // (at the next frame: the plan is far data)
        return true;
    }
    if (item.compare(0, 10, "!privanger") == 0 && g_found) {
        priv_ship_anger(g_lastSpawned);
        return true;
    }
    if (item.compare(0, 14, "!privretarget=") == 0 && g_found) {
        // !privretarget=me|prev: the last spawned ship's AI given our ship (or the ship spawned before it) as its target
        const bool prev = item.compare(14, 4, "prev") == 0;
        retarget_queue(g_lastSpawned, prev ? g_prevSpawned : priv_player_ship());
        wclog(1, "key script %.1fs: ds:%04X's target: %s (its plan's now ds:%04X)", t, g_lastSpawned, prev ? "the ship before it" : "our ship",
              plan_target(g_lastSpawned));
        return true;
    }
    if (item.compare(0, 15, "!privtargetscan") == 0 && g_found) {
        g_targetScan = item.compare(15, 5, "=prev") == 0 ? 1 : 0;
        return true;
    }
    if (item.compare(0, 11, "!privgonav=") == 0 && g_found) {
        // !privgonav=<sphere>[:<metres>]: our ship put in that nav sphere (its centre, offset along x)
        int id = 0, off = 0;
        sscanf(item.c_str() + 11, "%d:%d", &id, &off);
        PrivSphere sp[16];
        const int n = priv_spheres(sp, 16);
        for (int i = 0; i < n; i++) {
            if (sp[i].id != id) continue;
            const Bit16u ship = priv_r16(kPlayerShip);
            const Bit32s v[3] = { sp[i].x + off * 256, sp[i].y, sp[i].z };
            for (int k = 0; ship && k < 3; k++) mem_writed(ds_at((Bit16u)(ship + kShipPos + 4 * k)), (Bit32u)v[k]);
            wclog(1, "key script %.1fs: put in nav sphere %d (%d m off)", t, id, off);
            return true;
        }
        wclog(1, "key script %.1fs: !privgonav: no sphere %d", t, id);
        return true;
    }
    if (item.compare(0, 13, "!privcredits=") == 0 && g_found) {
        // !privcredits=<n>: the player's credits set (a test rig's aid only: the 32-bit global ds:655A the commodity
        // exchange adds to and takes from, ovr286:103C)
        const Bit32u v = (Bit32u)strtoul(item.c_str() + 13, NULL, 0);
        wclog(1, "key script %.1fs: credits %u -> %u", t, priv_r32(kCredits), v);
        mem_writed(ds_at(kCredits), v);
        return true;
    }
    if (item.compare(0, 10, "!privfind=") == 0 && g_found) {
        // !privfind=<value>: every place in the data segment holding that value as a 32-bit word (a test aid), and the
        // player record's offset (ds:66B2) for reference
        const Bit32u v = (Bit32u)strtoul(item.c_str() + 10, NULL, 0);
        std::string at;
        int n = 0;
        for (Bit32u a = 0; a + 3 < 0x10000 && n < 40; a++) {
            if (priv_r32((Bit16u)a) == v) {
                char b[8];
                snprintf(b, sizeof b, "%04X ", a);
                at += b;
                n++;
            }
        }
        wclog(1, "key script %.1fs: %u found at %s(the player record at %04X)", t, v, at.c_str(), priv_r16(kPlayerRecord));
        return true;
    }
    if (item.compare(0, 14, "!privwingtest=") == 0 && g_found) {
        // !privwingtest=<n>: as if the game spawned a wing of n pirates here (a test aid for the co-op extras: the
        // scene noted, n pirates spawned 2.5 km ahead, the extras' reckoning a second later)
        const int n = atoi(item.c_str() + 14);
        Bit32s pos[3], m[9];
        priv_player_pos(pos);
        priv_player_matrix(m);
        for (int i = 0; i < n && i < 8; i++) {
            Bit32s at[3] = { pos[0] + (i - n / 2) * 60000, pos[1] + 640000, pos[2] + (i % 2) * 40000 };
            priv_spawn_ship("TALNWIMP", "TALPIR", "PIR_DA", at, m, NULL, 100 + i);
        }
        wclog(1, "key script %.1fs: a wing of %d pirates as if the game's", t, n);
        return true;
    }
    if (item.compare(0, 10, "!privpeace") == 0 && g_found) {
        // !privpeace: every ship of the scene but ours and the puppets dies (a test aid: the launch point's encounter out
        // of the way of the autopilot)
        Bit16u ships[48];
        const int n = priv_scene_ships(ships, 48);
        int killed = 0;
        for (int i = 0; i < n; i++) {
            if (ships[i] == priv_player_ship() || priv_session_is_puppet(ships[i], 0)) continue;
            priv_ship_kill(ships[i]);
            killed++;
        }
        wclog(1, "key script %.1fs: !privpeace: %d ships of the scene killed", t, killed);
        return true;
    }
    if (item.compare(0, 9, "!privkill") == 0 && g_found) {
        // !privkill: the last ship a spawn of ours made dies (the game's own death: explosion, pods, the reaping)
        const Bit16u ship = priv_last_spawned();
        wclog(1, "key script %.1fs: ds:%04X killed", t, ship);
        if (priv_ship_is_alive(ship)) priv_ship_kill(ship);
        return true;
    }
    if (item.compare(0, 10, "!privface=") == 0 && g_found) {
        // !privface=<n>: the first ship of the scene that is not ours gets face n in its cast copy (+0xE)
        Bit16u ships[48];
        const int n = priv_scene_ships(ships, 48);
        for (int i = 0; i < n; i++) {
            if (ships[i] == priv_player_ship()) continue;
            const Bit16u ctl = priv_ship_controller(ships[i]);
            const Bit16u off = ctl ? priv_r16((Bit16u)(ctl + kCtlCast)) : 0, seg = ctl ? priv_r16((Bit16u)(ctl + kCtlCast + 2)) : 0;
            if (seg) {
                const PhysPt cast = ((PhysPt)seg << 4) + off;
                wclog(1, "key script %.1fs: ds:%04X's cast byte +0xE %d -> %d", t, ships[i], mem_readb(cast + 0x0E), atoi(item.c_str() + 10));
                mem_writeb(cast + 0x0E, (Bit8u)atoi(item.c_str() + 10));
            }
            break;
        }
        return true;
    }
    if (item.compare(0, 9, "!privsay=") == 0 && g_found) {
        // !privsay=<name>:<text with _ for spaces>: the last ship a spawn of ours made (else the first of the scene that
        // is not ours) says it
        std::string spec = item.substr(9);
        const size_t c1 = spec.find(':');
        std::string name = spec.substr(0, c1), text = c1 == std::string::npos ? "" : spec.substr(c1 + 1);
        for (size_t i = 0; i < text.size(); i++) if (text[i] == '_') text[i] = ' ';
        Bit16u ships[48];
        const int n = priv_scene_ships(ships, 48);
        bool said = false;
        for (int i = -1; i < n && !said; i++) {
            const Bit16u ship = i < 0 ? priv_last_spawned() : ships[i];
            if (!ship || ship == priv_player_ship() || !priv_ship_is_alive(ship)) continue;
            said = priv_ship_say(ship, name.c_str(), text.c_str(), getenv("PRIVSAYFACE") ? atoi(getenv("PRIVSAYFACE")) : -1);
            if (said) priv_let_think(priv_ship_controller(ship), getenv("PRIVTHINK") ? atoi(getenv("PRIVTHINK")) : 60);
            wclog(1, "key script %.1fs: ds:%04X (%s, face %d) says \"%s\" as %s: %s", t, ship, priv_ship_cast_name(ship).c_str(),
                  priv_ship_face(ship), text.c_str(), name.c_str(), said ? "told" : "no cast");
        }
        return true;
    }
    if (item.compare(0, 9, "!privpos=") == 0 && g_found) {
        // !privpos=<x>:<y>:<z>: the player's ship put there (the world's units; a nav sphere's place from SECTORS.IFF)
        const Bit16u ship = priv_r16(kPlayerShip);
        Bit32s v[3] = { 0, 0, 0 };
        sscanf(item.c_str() + 9, "%d:%d:%d", &v[0], &v[1], &v[2]);
        for (int i = 0; ship && i < 3; i++) {
            mem_writed(ds_at((Bit16u)(ship + kShipPos + 4 * i)), (Bit32u)v[i]);
        }
        wclog(1, "key script %.1fs: the player's ship (ds:%04X) put at %d, %d, %d", t, ship, v[0], v[1], v[2]);
        return true;
    }
    return false;
}

bool priv_spawn_ship(const char *type, const char *appearance, const char *cast, const Bit32s pos[3], const Bit32s m[9],
                     void (*done)(int tag, Bit16u obj), int tag) {
    if (!priv_in_flight() || strlen(type) > 8 || strlen(appearance) > 8 || strlen(cast) > 8) {
        return false;
    }
    cast_block_ask();
    PrivCall c;
    c.para = kSeg099;
    c.off = kPlace;
    c.done = done ? (void (*)(int, Bit16u, Bit16u))NULL : spawn_done;
    c.doneObj = done;
    c.tag = tag;
    // the data on the stack: the place (3 int32), the attitude (9 int32), the type's name, the appearance's
    for (int i = 0; i < 3; i++) {
        for (int b = 0; b < 4; b++) c.data.push_back((Bit8u)(pos[i] >> (8 * b)));
    }
    for (int i = 0; i < 9; i++) {
        for (int b = 0; b < 4; b++) c.data.push_back((Bit8u)(m[i] >> (8 * b)));
    }
    const size_t typeAt = c.data.size();
    for (size_t i = 0; i <= strlen(type); i++) c.data.push_back((Bit8u)type[i]);
    const size_t appAt = c.data.size();
    for (size_t i = 0; i <= strlen(appearance); i++) c.data.push_back((Bit8u)appearance[i]);
    if (c.data.size() & 1) c.data.push_back(0);
    const Bit16u rec = priv_r16(kPlayerRecord);
    c.args.push_back(rec ? (Bit16u)(rec + 0x2C) : 0);   // this (the game's own callers pass the player record's +0x2C)
    c.args.push_back(kStackArg + (Bit32u)typeAt);
    c.args.push_back(g_ds);                             // (SS is DGROUP where the call is made)
    c.castSlot = cast_slot(cast);
    c.args.push_back(kCastArg);
    c.args.push_back(kStackArg + (Bit32u)appAt);
    c.args.push_back(g_ds);
    c.args.push_back(kStackArg + 0);                    // the place
    c.args.push_back(kStackArg + 12);                   // the attitude
    return queue_call(c);
}

bool priv_hold_ai(Bit16u controller, bool hold) {
    std::vector<Bit16u>::iterator it = std::find(g_heldAi.begin(), g_heldAi.end(), controller);
    if (hold && it == g_heldAi.end() && controller && g_heldAi.size() < kHeldCap) {
        g_heldAi.push_back(controller);
        return true;
    } else if (!hold && it != g_heldAi.end()) {
        g_heldAi.erase(it);
    }
    return !hold || it != g_heldAi.end();
}

void priv_hold_ai_none() {
    g_heldAi.clear();
    g_thinkFrames.clear();
}

void priv_let_think(Bit16u controller, int frames) {
    for (size_t i = 0; i < g_thinkFrames.size(); i++) {
        if (g_thinkFrames[i].first == controller) {
            g_thinkFrames[i].second = frames;
            return;
        }
    }
    if (controller && g_thinkFrames.size() < kHeldCap) g_thinkFrames.push_back(std::make_pair(controller, frames));
}

bool priv_thinking(Bit16u controller) {
    for (size_t i = 0; i < g_thinkFrames.size(); i++) {
        if (g_thinkFrames[i].first == controller) return true;
    }
    return false;
}

Bit16u priv_ship_controller(Bit16u obj) {
    return priv_r16((Bit16u)(obj + kShipController));
}

bool priv_ship_alive(Bit16u obj, Bit16u type) {
    return obj && priv_r16(obj) == kShipVtable && priv_r16((Bit16u)(obj + kShipType)) == type;
}

Bit16u priv_ship_type(Bit16u obj) {
    return priv_r16((Bit16u)(obj + kShipType));
}

void priv_ship_set(Bit16u obj, const Bit32s pos[3], const Bit32s m[9]) {
    for (int i = 0; i < 3; i++) {
        mem_writed(ds_at((Bit16u)(obj + kShipPos + 4 * i)), (Bit32u)pos[i]);
    }
    for (int i = 0; m && i < 9; i++) {
        mem_writed(ds_at((Bit16u)(obj + kShipMatrix + 4 * i)), (Bit32u)m[i]);
    }
}

void priv_player_matrix(Bit32s m[9]) {
    const Bit16u ship = priv_in_flight() ? priv_r16(kPlayerShip) : 0;
    for (int i = 0; i < 9; i++) {
        m[i] = ship ? (Bit32s)priv_r32((Bit16u)(ship + kShipMatrix + 4 * i)) : (i % 4 == 0 ? 256 : 0);
    }
}

std::string priv_player_appearance() {
    const Bit16u rec = priv_in_flight() ? priv_r16(kPlayerRecord) : 0;
    std::string out;
    for (int i = 0; rec && i < 8; i++) {
        const Bit8u c = priv_r8((Bit16u)(rec + kRecordAppearance + i));
        if (!c) break;
        if (c < 32 || c >= 127) return "";
        out += (char)c;
    }
    return out;
}

// (The records are in the game's far heap, in the EMS window at D000: read at the frame's top, where the game itself
// reads them, and kept for the flight; a read at another moment can see another page there.)
static PrivSphere g_spheres[16];
static int g_nSpheres;
static Bit16u g_spheresShip;

static int read_spheres(PrivSphere out[], int cap) {
    const int n = priv_r16(kSphereList);
    const Bit16u arr = priv_r16((Bit16u)(kSphereList + 2));
    int k = 0;
    for (int i = 0; i < n && i < 32 && k < cap; i++) {
        const Bit16u off = priv_r16((Bit16u)(arr + 4 * i)), seg = priv_r16((Bit16u)(arr + 4 * i + 2));
        if (!seg) continue;
        const PhysPt rec = ((PhysPt)seg << 4) + off;
        Bit8u b[19];
        bool ok = true;
        for (int j = 0; j < 19 && ok; j++) {
            ok = !mem_readb_checked(rec + j, &b[j]);
        }
        if (!ok || b[16] != 0xFF) {
            continue;       // (a jump point or a base in the sphere: not the sphere itself)
        }
        PrivSphere &s = out[k++];
        s.id = b[0];
        s.x = (Bit32s)(b[2] | (b[3] << 8) | (b[4] << 16) | ((Bit32u)b[5] << 24));
        s.y = (Bit32s)(b[6] | (b[7] << 8) | (b[8] << 16) | ((Bit32u)b[9] << 24));
        s.z = (Bit32s)(b[10] | (b[11] << 8) | (b[12] << 16) | ((Bit32u)b[13] << 24));
        s.radius = b[14] | (b[15] << 8);
    }
    return k;
}

// The player's accepted missions' nav points (the nav map's NAV/MIS page, ovr375:1782..: ovr371:054F gives the
// index-th of 4 near mission pointers at the player record's +0x5D; a mission's objectives are 5-byte entries -- a far
// pointer and a byte -- at its +0x2C, their count at +0x2E (seg078:0117); an objective's byte 0 its kind (0xD0-0xE2),
// byte 1 the same for a mission's objectives in Troy (0x58: its system's number, as far as is known), byte 2 9 for a
// nav point and byte 3 then its "Nav N").  Read at the frame (the objectives are far data), cached.  Measured: a
// scout mission "Nav 3 in Troy" D0/58/09/03, a base defence D6/58/09/04 and E2/01/00/00; the map rang Nav 3 and Nav 4.
enum { kRecordMissions = 0x5D, kMissionObjectives = 0x2C, kMissionSlots = 4 };
unsigned g_missionNavs;
std::string g_missionText;

static void missions_refresh() {
    const Bit16u rec = priv_r16(kPlayerRecord);
    unsigned navs = 0;
    std::string text;
    for (int m = 0; rec && m < kMissionSlots; m++) {
        const Bit16u mission = priv_r16((Bit16u)(rec + kRecordMissions + 2 * m));
        if (!mission) continue;
        const Bit16u arr = priv_r16((Bit16u)(mission + kMissionObjectives));
        const int count = (Bit16s)priv_r16((Bit16u)(mission + kMissionObjectives + 2));
        char b[32];
        snprintf(b, sizeof b, "%smission %d:", text.empty() ? "" : "; ", m);
        text += b;
        for (int i = 0; arr && i < count && i < 24; i++) {
            const Bit16u off = priv_r16((Bit16u)(arr + 5 * i)), seg = priv_r16((Bit16u)(arr + 5 * i + 2));
            if (!seg) continue;
            const PhysPt o = ((PhysPt)seg << 4) + off;
            Bit8u kind = 0, nav = 0xFF, k2 = 0, k3 = 0;
            if (mem_readb_checked(o, &kind) || mem_readb_checked(o + 1, &nav)) continue;
            mem_readb_checked(o + 2, &k2);
            mem_readb_checked(o + 3, &k3);
            snprintf(b, sizeof b, " %02X/%02X/%02X/%02X", kind, nav, k2, k3);
            text += b;
            // a nav point's objective: byte 2 is 9 and byte 3 the map's "Nav N" (the nav map tests the same,
            // seg078:0153), the sphere N - 1 (Troy's Nav 1, the jump point, is sphere 0; Achilles, Nav 2, sphere 1)
            if (k2 == 9 && k3 >= 1 && k3 <= 32) navs |= 1u << (k3 - 1);
        }
    }
    if (navs != g_missionNavs || text != g_missionText) {
        wclog(2, "Privateer: our missions: %s (their nav points %08X)", text.empty() ? "none" : text.c_str(), navs);
    }
    g_missionNavs = navs;
    g_missionText = text;
}

unsigned priv_mission_navs() {
    // (PRIVMISSIONS=<sphere,...>: a test rig's missions, added to the real ones)
    unsigned extra = 0;
    if (const char *e = getenv("PRIVMISSIONS")) {
        for (const char *q = e; *q; ) {
            const int n = atoi(q);
            if (n >= 0 && n < 32) extra |= 1u << n;
            while (*q && *q != ',') q++;
            if (*q == ',') q++;
        }
    }
    return g_missionNavs | extra;
}

void priv_spheres_refresh() {
    missions_refresh();
    const Bit16u ship = priv_player_ship();
    if (ship && (ship != g_spheresShip || g_nSpheres == 0)) {
        g_nSpheres = read_spheres(g_spheres, 16);
        g_spheresShip = ship;
        if (g_nSpheres) {
            wclog(2, "Privateer: %d nav spheres read (ids %d..%d)", g_nSpheres, g_spheres[0].id, g_spheres[g_nSpheres - 1].id);
        }
    } else if (!ship) {
        g_nSpheres = 0;
        g_spheresShip = 0;
    }
}

int priv_spheres(PrivSphere out[], int cap) {
    if (!priv_in_flight()) {
        return 0;
    }
    const int n = g_nSpheres < cap ? g_nSpheres : cap;
    for (int i = 0; i < n; i++) {
        out[i] = g_spheres[i];
    }
    return n;
}

int priv_nav_at(const Bit32s pos[3]) {
    PrivSphere sp[16];
    const int n = priv_spheres(sp, 16);
    int best = -1;
    double bestD = 0;
    for (int i = 0; i < n; i++) {
        const double dx = (double)pos[0] - sp[i].x, dy = (double)pos[1] - sp[i].y, dz = (double)pos[2] - sp[i].z;
        const double d = sqrt(dx * dx + dy * dy + dz * dz) / 256.0;
        if (d < sp[i].radius && (best < 0 || d < bestD)) {
            best = sp[i].id;
            bestD = d;
        }
    }
    return best;
}

Bit16u priv_last_spawned() { return g_lastSpawned; }

int priv_scene_ships(Bit16u out[], int cap) {
    if (!priv_in_flight()) {
        return 0;
    }
    int k = 0;
    Bit16u node = priv_r16((Bit16u)(kSceneList + 2));
    for (int guard = 0; node && guard < 256 && k < cap; guard++) {
        const Bit16u obj = priv_r16((Bit16u)(node + 2));
        if (obj && priv_r16(obj) == kShipVtable) {
            out[k++] = obj;
        }
        node = priv_r16(node);
    }
    return k;
}

static int scene_objects(Bit16u out[], int cap, Bit16u vtable) {
    if (!priv_in_flight()) {
        return 0;
    }
    int k = 0;
    Bit16u node = priv_r16((Bit16u)(kSceneList + 2));
    for (int guard = 0; node && guard < 256 && k < cap; guard++) {
        const Bit16u obj = priv_r16((Bit16u)(node + 2));
        if (obj && priv_r16(obj) == vtable) {
            out[k++] = obj;
        }
        node = priv_r16(node);
    }
    return k;
}

int priv_scene_pods(Bit16u out[], int cap) { return scene_objects(out, cap, kPodVtable); }

bool priv_scene_has(Bit16u obj) {
    if (!obj || !priv_in_flight()) {
        return false;
    }
    Bit16u node = priv_r16((Bit16u)(kSceneList + 2));
    for (int guard = 0; node && guard < 256; guard++) {
        if (priv_r16((Bit16u)(node + 2)) == obj) return true;
        node = priv_r16(node);
    }
    return false;
}

bool priv_pod_is(Bit16u obj) { return obj && priv_r16(obj) == kPodVtable; }

bool priv_pod_cargo(Bit16u obj, std::vector<std::vector<Bit8u> > &records, int &units) {
    records.clear();
    units = 0;
    PhysPt node;
    if (!priv_pod_is(obj) || !far_of((Bit16u)(obj + kPodList + 2), &node)) {
        return false;
    }
    for (int n = 0; node && n < 8; n++) {
        PhysPt rec, block;
        if (!far_at(node + 4, &rec)) break;
        // the block from its name; a record not at +0x10 of what it points at: the record alone after 16 zero bytes
        const bool whole = far_at(rec, &block) && block + kCargoRecordAt == rec;
        std::vector<Bit8u> record;
        for (int i = 0; i < kCargoBlockBytes; i++) {
            Bit8u b = 0;
            if (whole) {
                if (mem_readb_checked(block + i, &b)) return false;
            } else if (i >= kCargoRecordAt) {
                if (mem_readb_checked(rec + i - kCargoRecordAt, &b)) return false;
            }
            record.push_back(b);
        }
        records.push_back(record);
        if (!far_at(node, &node)) break;        // the next node (none: the end)
    }
    units = (Bit16s)priv_r16((Bit16u)(obj + kPodUnits));
    return !records.empty();
}

void priv_pod_set(Bit16u obj, const Bit32s pos[3]) {
    for (int i = 0; i < 3; i++) {
        mem_writed(ds_at((Bit16u)(obj + kShipPos + 4 * i)), (Bit32u)pos[i]);
    }
}

// A pod of ours given its cargo: a record already there (a pod reused) is written over; else the game's far heap
// gives a record and a node (two calls, one after the other), and the list is made of them.
struct PodFill {
    Bit16u obj;
    std::vector<std::vector<Bit8u> > records;
    size_t next;                // the record being made
    int units;
    Bit16u recOff, recSeg;      // the record's far address once given
    int step;
};
std::vector<PodFill> g_podFills;

static void pod_fill_cancel(int tag) {
    if (tag >= 0 && tag < (int)g_podFills.size()) g_podFills[tag].obj = 0;
}

static void pod_fill_step(int tag, Bit16u ax, Bit16u dx) {
    if (tag < 0 || tag >= (int)g_podFills.size() || !g_podFills[tag].obj) {
        return;
    }
    PodFill &f = g_podFills[tag];
    if (!ax && !dx) {
        wclog(1, "Privateer: the game's far heap gave nothing for a pod's cargo (ds:%04X)", f.obj);
        f.obj = 0;
        return;
    }
    if (f.step == 1) {
        f.recOff = ax;
        f.recSeg = dx;
        f.step = 2;
        PrivCall c;
        c.para = 0;
        c.off = kFarMalloc;
        c.args.push_back(8);
        c.args.push_back(0);
        c.done = pod_fill_step;
        c.cancel = pod_fill_cancel;
        c.guardObj = f.obj;
        c.guardVtable = kPodVtable;
        c.tag = tag;
        if (!queue_call(c)) f.obj = 0;
        return;
    }
    if (!priv_pod_is(f.obj) || !priv_scene_has(f.obj)) {
        f.obj = 0;          // (the pod gone while its blocks were given: they stay the game's far heap's, unused)
        return;
    }
    // the node given: the block written (its own address in it: the record's +0 and the word before the record),
    // the node appended to the list (its tail's next, or the head when empty)
    const PhysPt node = ((PhysPt)dx << 4) + ax, block = ((PhysPt)f.recSeg << 4) + f.recOff;
    const std::vector<Bit8u> &record = f.records[f.next];
    for (size_t i = 0; i < record.size() && i < (size_t)kCargoBlockBytes; i++) {
        mem_writeb(block + i, record[i]);
    }
    mem_writew(block + kCargoRecordAt - 2, f.recSeg);
    mem_writew(block + kCargoRecordAt, f.recOff);
    mem_writew(block + kCargoRecordAt + 2, f.recSeg);
    mem_writed(node, 0);
    mem_writew(node + 4, (Bit16u)(f.recOff + kCargoRecordAt));
    mem_writew(node + 6, f.recSeg);
    PhysPt last;
    if (far_of((Bit16u)(f.obj + kPodList + 6), &last)) {
        mem_writew(last, ax);
        mem_writew(last + 2, dx);
    } else {
        priv_w16((Bit16u)(f.obj + kPodList + 2), ax);
        priv_w16((Bit16u)(f.obj + kPodList + 4), dx);
    }
    priv_w16((Bit16u)(f.obj + kPodList + 6), ax);
    priv_w16((Bit16u)(f.obj + kPodList + 8), dx);
    if (++f.next < f.records.size()) {
        f.step = 1;
        PrivCall c;
        c.para = 0;
        c.off = kFarMalloc;
        c.args.push_back(kCargoBlockBytes);
        c.args.push_back(0);
        c.done = pod_fill_step;
        c.cancel = pod_fill_cancel;
        c.guardObj = f.obj;
        c.guardVtable = kPodVtable;
        c.tag = tag;
        if (!queue_call(c)) f.obj = 0;
        return;
    }
    priv_w16((Bit16u)(f.obj + kPodUnits), (Bit16u)f.units);
    wclog(2, "Privateer: pod ds:%04X given its cargo (%d units in %d records)", f.obj, f.units, (int)f.records.size());
    f.obj = 0;
}

void priv_pod_fill(Bit16u obj, const std::vector<std::vector<Bit8u> > &records, int units) {
    if (!priv_pod_is(obj) || records.empty()) {
        return;
    }
    PhysPt node, rec;
    if (far_of((Bit16u)(obj + kPodList + 2), &node) && far_at(node + 4, &rec)) {
        // records there already (the pod reused): the first written over (its name and its record's fields, not its
        // pointers), the units set (more than one record of a reused pod: only the first kept; the rest's units go to it)
        const std::vector<Bit8u> &record = records[0];
        for (size_t i = 0; i < record.size() && i < (size_t)kCargoBlockBytes; i++) {
            if (i < 12) mem_writeb(rec - kCargoRecordAt + i, record[i]);
            else if (i >= (size_t)kCargoRecordAt + 4) mem_writeb(rec - kCargoRecordAt + i, record[i]);
        }
        PhysPt next;
        if (far_at(node, &next) && next) {
            mem_writed(node, 0);
            priv_w16((Bit16u)(obj + kPodList + 6), priv_r16((Bit16u)(obj + kPodList + 2)));
            priv_w16((Bit16u)(obj + kPodList + 8), priv_r16((Bit16u)(obj + kPodList + 4)));
        }
        if (records.size() > 1) mem_writew(rec + 0x18, (Bit16u)units);
        priv_w16((Bit16u)(obj + kPodUnits), (Bit16u)units);
        return;
    }
    size_t tag = 0;
    while (tag < g_podFills.size() && g_podFills[tag].obj) tag++;
    if (tag == g_podFills.size()) {
        if (tag >= 16) return;      // (a list has a cap)
        g_podFills.push_back(PodFill());
    }
    PodFill &f = g_podFills[tag];
    f.obj = obj;
    f.records = records;
    f.next = 0;
    f.units = units;
    f.recOff = f.recSeg = 0;
    f.step = 1;
    PrivCall c;
    c.para = 0;
    c.off = kFarMalloc;
    c.args.push_back(kCargoBlockBytes);
    c.args.push_back(0);
    c.done = pod_fill_step;
    c.cancel = pod_fill_cancel;
    c.guardObj = f.obj;
    c.guardVtable = kPodVtable;
    c.tag = (int)tag;
    if (!queue_call(c)) f.obj = 0;
}

static std::string ds_name(Bit16u at, int max) {
    std::string out;
    for (int i = 0; at && i < max; i++) {
        const Bit8u c = priv_r8((Bit16u)(at + i));
        if (!c) break;
        if (c < 32 || c >= 127) return "";
        out += (char)c;
    }
    return out;
}

std::string priv_ship_type_name(Bit16u obj) {
    const Bit16u type = priv_r16((Bit16u)(obj + kShipType));
    return type ? ds_name((Bit16u)(type + 2), 8) : "";
}

std::string priv_ship_appearance_name(Bit16u obj) {
    const Bit16u off = priv_r16((Bit16u)(obj + kShipAppearance)), seg = priv_r16((Bit16u)(obj + kShipAppearance + 2));
    std::string out;
    for (int i = 0; seg && i < 8; i++) {
        Bit8u c = 0;
        if (mem_readb_checked(((PhysPt)seg << 4) + (Bit16u)(off + 0x18 + i), &c) || c < 32 || c >= 127) {
            return c ? "" : out;
        }
        out += (char)c;
    }
    return out;
}

std::string priv_ship_cast_name(Bit16u obj) {
    return ds_name(priv_r16((Bit16u)(obj + 0x0E)), 8);
}

void priv_ship_pos(Bit16u obj, Bit32s pos[3]) {
    for (int i = 0; i < 3; i++) {
        pos[i] = (Bit32s)priv_r32((Bit16u)(obj + kShipPos + 4 * i));
    }
}

void priv_ship_matrix(Bit16u obj, Bit32s m[9]) {
    for (int i = 0; i < 9; i++) {
        m[i] = (Bit32s)priv_r32((Bit16u)(obj + kShipMatrix + 4 * i));
    }
}

bool priv_ship_is_alive(Bit16u obj) {
    return obj && priv_r16(obj) == kShipVtable && priv_r8((Bit16u)(obj + kShipAlive)) != 0;
}

static PhysPt ship_model(Bit16u obj) {
    const Bit16u off = priv_r16((Bit16u)(obj + kShipModel)), seg = priv_r16((Bit16u)(obj + kShipModel + 2));
    return seg ? ((PhysPt)seg << 4) + off : 0;
}

void priv_ship_armor(Bit16u obj, Bit32s a[4]) {
    const PhysPt model = ship_model(obj);
    for (int q = 0; q < 4; q++) {
        Bit16u v = 0;
        if (!model || mem_readw_checked(model + kModelArmor + 6 * q, &v)) {
            v = 0;
        }
        a[q] = (Bit16s)v;
    }
}

void priv_ship_set_armor(Bit16u obj, const Bit32s a[4]) {
    const PhysPt model = ship_model(obj);
    for (int q = 0; model && q < 4; q++) {
        mem_writew(model + kModelArmor + 6 * q, (Bit16u)a[q]);
    }
}

void priv_ship_kill(Bit16u obj) {
    if (priv_ship_is_alive(obj)) {
        priv_w8((Bit16u)(obj + kShipAlive), 0);
    }
}

void priv_ship_fire(Bit16u obj, bool on) {
    const Bit16u ctl = priv_ship_controller(obj);
    const Bit16u off = ctl ? priv_r16((Bit16u)(ctl + 2)) : 0, seg = ctl ? priv_r16((Bit16u)(ctl + 4)) : 0;
    if (!seg) {
        return;
    }
    const PhysPt rec = ((PhysPt)seg << 4) + off;
    Bit8u b = 0;
    if (mem_readb_checked(rec + kControlFire, &b)) {
        return;
    }
    mem_writeb(rec + kControlFire, on ? (Bit8u)(b | 4) : (Bit8u)(b & ~4));
}

static bool far_of(Bit16u at, PhysPt *out) {
    const Bit16u off = priv_r16(at), seg = priv_r16((Bit16u)(at + 2));
    if (!seg) {
        return false;
    }
    *out = ((PhysPt)seg << 4) + off;
    return true;
}

static bool far_at(PhysPt at, PhysPt *out) {
    Bit16u off = 0, seg = 0;
    if (mem_readw_checked(at, &off) || mem_readw_checked(at + 2, &seg) || !seg) {
        return false;
    }
    *out = ((PhysPt)seg << 4) + off;
    return true;
}

static int g_sayMask = 7;       // (a test aid: PRIVSAY=<mask> leaves out the name (1), the lines (2), the pilot (4))

int priv_ship_face(Bit16u obj) {
    const Bit16u ctl = priv_ship_controller(obj);
    PhysPt cast;
    Bit8u b = 0;
    if (!ctl || !far_of((Bit16u)(ctl + kCtlCast), &cast) || mem_readb_checked(cast + kCastFace, &b)) {
        return -1;
    }
    return b;
}

void priv_ship_delete(Bit16u obj, int mode) {
    if (mode != 0) {
        PrivCall c;
        c.para = kSeg081;
        c.off = kSceneRemove;
        c.args.push_back(kSceneList);
        c.args.push_back(obj);
        queue_call(c);
    }
    if (mode == 2) {
        priv_w16((Bit16u)(obj - 2), 0);
    }
    PrivCall d;
    d.para = kSeg053;
    d.off = kShipDelete;
    d.args.push_back(obj);
    d.args.push_back(1);
    queue_call(d);
}

bool priv_ship_say(Bit16u obj, const char *name, const char *text, int face) {
    const Bit16u ctl = priv_ship_controller(obj);
    PhysPt cast, plan, block;
    if (!ctl || !far_of((Bit16u)(ctl + kCtlCast), &cast) || !far_of((Bit16u)(ctl + kCtlPlan), &plan)) {
        return false;
    }
    // (the face: below, the comm record's frame; the cast's portrait id at +0xE is left as the game has it -- it is only
    // the speech's voice, and a foreign id there crashed the game)
    if (getenv("PRIVSAY")) g_sayMask = atoi(getenv("PRIVSAY"));
    Bit16u pilot = 0;
    if (mem_readw_checked(plan + kPlanPilot, &pilot) || !pilot) {
        return false;
    }
    // the name: the block's 12 bytes
    if ((g_sayMask & 1) && far_at(cast + kCastName, &block)) {
        for (int i = 0; i < 12; i++) {
            mem_writeb(block + i, (Bit8u)(i < 11 && i < (int)strlen(name) ? toupper((unsigned char)name[i]) : 0));
        }
    }
    // the first received line of each attitude's set (friendly, neutral, hostile: the sets' objects at the cast's
    // +0x1B, +0x1F, +0x23: a count, then a far pointer to the block of that many ids and the strings): the text
    // over the set's strings, the first id the line said
    int firstId = 1;
    for (int k = 0; k < 3 && (g_sayMask & 2); k++) {
        PhysPt setObj, blk;
        if (!far_at(cast + kCastRecv0 + 4 * k, &setObj)) {
            continue;
        }
        Bit16u count = 0;
        if (mem_readw_checked(setObj, &count) || count < 1 || count > 8 || !far_at(setObj + 2, &blk)) {
            continue;
        }
        const PhysPt strings = blk + 2 * count;
        Bit32u room = 0;
        for (int n = 0; n < count && room < 400; n++) {
            Bit8u c = 1;
            while (c && room < 400) {
                if (mem_readb_checked(strings + room, &c)) { c = 0; break; }
                room++;
            }
        }
        if (room < 8) {
            continue;
        }
        if (k == 1) {
            firstId = mem_readw(blk);
        }
        std::string ids;
        for (int n = 0; n < count; n++) {
            char b[8];
            snprintf(b, sizeof b, " %02X", mem_readw(blk + 2 * n));
            ids += b;
        }
        wclog(2, "Privateer: say: set %d at %05x: %u ids (%s), strings at %05x, room %u", k, (unsigned)setObj, count, ids.c_str(), (unsigned)strings,
              (unsigned)room);
        // the text as the first string; the set's size kept for the next line (the strings are measured again then):
        // the rest of the room spaces, as the second string (or the first's tail when there is one), the others empty
        const Bit32u len = (Bit32u)std::min(strlen(text), (size_t)(room - count));
        for (Bit32u i = 0; i < room; i++) {
            Bit8u b;
            if (i < len) b = (Bit8u)text[i];
            else if (count == 1) b = i + 1 < room ? ' ' : 0;
            else if (i == len) b = 0;
            else b = i < room - (count - 1) ? ' ' : 0;
            mem_writeb(strings + i, b);
        }
    }
    if (!(g_sayMask & 4)) {
        return true;
    }
    if (const char *e = getenv("PRIVSAYID")) firstId = (int)strtoul(e, NULL, 0);     // (a test's: another line id)
    // The text on the cockpit's message bar, the face on the comm screen (the user: "the lines of written text should go
    // as text in the hud not in the vdu itself").  The speaker's comm record is its controller + 8 (seg042:0AA0): +2 the
    // cast (far), +8 the face picture's frame, +0xA its voice.  The game picks the frame once, the first time the face
    // is drawn (seg096:0168 -> seg097:007F: a random frame of the cast's +2 first, +4 count, the last +6 of them the
    // other head; the voice then the cast's portrait id, +0x21 for the other head), and with a voice and speech.pak a
    // line is played as the recorded speech of its kind (another sentence: ours not shown, measured) and the bar's text
    // dropped as the speech ends (seg098:03A0).  So the frame is ours, the head the pilot chose, and the voice none
    // (0xFF: the text on the bar for the line's time, measured from the first line on, the face beside it).
    {
        const Bit16u rec = (Bit16u)(ctl + kCtlComm);
        const Bit16u first = mem_readw(cast + 2), count = mem_readw(cast + 4), other = mem_readw(cast + 6);
        Bit16u frame = first;
        if (face >= 0x21 && other > 0 && other < count) frame = (Bit16u)(first + count - other);    // (a woman in each of
                                                                        // MER_DP, MIL_AA, CON_SA, BOU_AA; the first a man)
        const Bit16u had = priv_r16((Bit16u)(rec + 8));
        if (face >= 0 || had < first || had >= first + count) priv_w16((Bit16u)(rec + 8), frame);
        wclog(2, "Privateer: say: ds:%04X's face frames %u+%u (other head %u): frame %u -> %u, voice %04X -> none", obj, first, count, other, had,
              priv_r16((Bit16u)(rec + 8)), priv_r16((Bit16u)(rec + 0xA)));
        priv_w16((Bit16u)(rec + 0xA), 0xFF);
    }
    priv_w16((Bit16u)(pilot + kPilotLine), (Bit16u)firstId);
    priv_w16((Bit16u)(pilot + kPilotTimer), 0);
    priv_w8((Bit16u)(pilot + kPilotFlags), (Bit8u)(priv_r8((Bit16u)(pilot + kPilotFlags)) | 1));
    // as the game's own hail does (seg013:0AA0): its second flag byte's bit 4, and the comm's pending byte
    priv_w8((Bit16u)(pilot + kPilotFlags2), (Bit8u)(priv_r8((Bit16u)(pilot + kPilotFlags2)) | 0x10));
    if (priv_r8(kCommPending) == 0) priv_w8(kCommPending, 1);
    return true;
}

void priv_calls_flush(const char *why) {
    calls_flush(why);
}

void priv_ship_set_target(Bit16u ship, Bit16u target) {
    retarget_queue(ship, target);
}

void priv_wings_flight_changed() {
    wings_flight_changed();
}

void priv_ship_anger(Bit16u ship) {
    if (g_angers.size() < 16) g_angers.push_back(ship);
}

void priv_hit_apply(Bit16u obj, int loc, int damage) {
    if (!priv_ship_is_alive(obj) || loc < 0 || loc > 3 || damage <= 0) {
        return;
    }
    const Bit16u off = priv_r16((Bit16u)(obj + kShipModel)), seg = priv_r16((Bit16u)(obj + kShipModel + 2));
    if (!seg) {
        return;
    }
    int tag = -1;
    for (size_t i = 0; i < g_hits.size() && tag < 0; i++) {
        if (!g_hits[i].obj) tag = (int)i;
    }
    if (tag < 0) {
        if (g_hits.size() >= 32) {
            return;         // (a list has a cap: a hit beyond 32 in flight is dropped)
        }
        g_hits.push_back(HitJob());
        tag = (int)g_hits.size() - 1;
    }
    HitJob &h = g_hits[tag];
    h.obj = obj;
    h.modelOff = off;
    h.modelSeg = seg;
    h.loc = loc;
    h.damage = damage;
    h.left = -1;
    h.ours = obj == priv_player_ship();
    std::vector<Bit32u> a;
    a.push_back(off); a.push_back(seg); a.push_back((Bit16u)loc); a.push_back((Bit16u)damage);
    hit_call(tag, kSeg101, kModelShields, a);
}

bool priv_near_heap_read(int &total, int &largest, int &blocks) {
    // The game's near heap (seg142:012B, its own allocator, not Borland's): a ring of free blocks from ds:3FF8, each
    // its size at +0 and the next at +6; a request walks the ring for the first block big enough (error 1303 when
    // there is none).  Nothing grows it: what the ring holds is all there is.  False: not to be trusted (the
    // allocator at work, or a ring that does not close)
    total = largest = blocks = 0;
    if (!g_found || priv_r8(kNearHeapLock)) {
        return false;
    }
    const Bit16u head = priv_r16(kNearFreeList);
    if (!head) {
        return true;        // (nothing free at all)
    }
    Bit16u b = head;
    for (int i = 0; i < 1000; i++) {
        const int size = priv_r16(b) & ~1;
        if (size <= 0 || size >= 0x8000) return false;
        total += size;
        blocks++;
        if (size > largest) largest = size;
        b = priv_r16((Bit16u)(b + 6));
        if (b == head) return true;
        if (!b) return false;
    }
    return false;
}

void priv_near_heap(int &total, int &largest, int &blocks) {
    priv_near_heap_read(total, largest, blocks);
}

// The low-water mark of the near heap, sampled every tick (the allocator's lock respected), and our ship's death
int g_heapLowSecond = 1 << 30, g_heapLowEver = 1 << 30, g_heapLowTake = 1 << 30;
Bit16u g_watchShip;
bool g_watchAlive;

static void watch_tick() {
    int total, largest, blocks;
    if (priv_in_flight() && priv_near_heap_read(total, largest, blocks)) {
        if (total < g_heapLowSecond) g_heapLowSecond = total;
        if (total < g_heapLowEver) g_heapLowEver = total;
        if (total < g_heapLowTake) g_heapLowTake = total;
    }
    const Bit16u ship = priv_r16(kPlayerShip);
    if (ship != g_watchShip) {
        g_watchShip = ship;
        g_watchAlive = ship && priv_r8((Bit16u)(ship + kShipAlive)) != 0;
    } else if (ship) {
        const bool alive = priv_r8((Bit16u)(ship + kShipAlive)) != 0;
        if (g_watchAlive && !alive) {
            priv_session_our_death();
            int pilots, npcs, pods;
            priv_session_counts(pilots, npcs, pods);
            wclog(1, "Privateer: our ship was destroyed (near heap free %d, its lowest %d; ships of others here: %d pilots, %d encounter ships, %d pods)",
                  priv_near_free(), g_heapLowEver, pilots, npcs, pods);
        }
        g_watchAlive = alive;
    }
}

int priv_near_heap_lowest() { return g_heapLowEver < (1 << 30) ? g_heapLowEver : -1; }

int priv_near_heap_low_take() {
    // the lowest free since the last call (every tick's sample), or now's when none was taken; -1 unknown
    int low = g_heapLowTake;
    g_heapLowTake = 1 << 30;
    if (low >= (1 << 30)) {
        int total, largest, blocks;
        low = priv_near_heap_read(total, largest, blocks) ? total : -1;
    }
    return low;
}

int priv_near_free() {
    int total, largest, blocks;
    priv_near_heap(total, largest, blocks);
    return g_found ? total : -1;
}

bool priv_in_flight() {
    return g_found && priv_r16(kPlayerShip) != 0 && priv_r16(kPlayerRecord) != 0;
}

Bit16u priv_player_ship() {
    return priv_in_flight() ? priv_r16(kPlayerShip) : 0;
}

void priv_player_pos(Bit32s pos[3]) {
    const Bit16u ship = priv_in_flight() ? priv_r16(kPlayerShip) : 0;
    for (int i = 0; i < 3; i++) {
        pos[i] = ship ? (Bit32s)priv_r32((Bit16u)(ship + kShipPos + 4 * i)) : 0;
    }
}

std::string priv_system_name() {
    const Bit16u rec = priv_in_flight() ? priv_r16(kPlayerRecord) : 0;
    const Bit16u name = rec ? priv_r16((Bit16u)(rec + kRecordSystemName)) : 0;
    std::string out;
    for (int i = 0; name && i < 32; i++) {
        const Bit8u c = priv_r8((Bit16u)(name + i));
        if (!c) {
            return out;
        }
        if (c < 32 || c >= 127) {
            return "";      // (not a name: a pointer the game has not set yet)
        }
        out += (char)c;
    }
    return "";
}

}  // namespace wc
