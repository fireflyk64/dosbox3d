/*
 *  TIE Fighter: finding the program in memory and watching it (wcnet_tie.h).
 *
 *  Every image address here is the LE's (the code object at 0x10000, the
 *  data object at 0xC0000), as the OpenTIE decompilation of this very build
 *  names them (docs/tiefighter.md has the map and how it was checked against
 *  Z_TIE__.EXE).  DOS/4GW loads the two objects into extended memory keeping
 *  them 0xB0000 apart, with flat selectors (base 0), so EIP and every
 *  pointer the program holds are linear addresses, and an image address A
 *  is at linear tie_linear(A).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wcnet_tie.h"
#include "wcnet_tie_session.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "cpu.h"
#include "pic.h"
#include "lazyflags.h"
#include <deque>

namespace wc {

namespace {

enum {
    kCodeImage = 0x10000,       // the code object's image address
    kDataImage = 0xC0000,       // the data object's
    kDataPastCode = 0xB0000,    // where the data object is, from the code object's start

    // The data object's bytes the search goes by: the rank thresholds
    // (20000, 50000, 100000, 250000, 500000) and the resource directory.
    kRankScores = 0xC1E10,
    kResourcePath = 0xC1C98,

    // (the flight's tables: tie:: in wcnet_tie.h)

    // The front end's (Landru's) pointer: its own, moved by the mouse's
    // mickeys, and what its screens are clicked with.
    kCursorX = 0xD2BE6,
    kCursorY = 0xD2BE8,
    kDownButton = 0xD2BF8,
    kKey = 0xD2C04,
};

// The functions watched, and the bytes two of them begin with (the build
// the addresses belong to).
struct Place {
    const char *name;
    Bit32u image;
    unsigned calls;             // since the last report
    unsigned total;
};
Place g_places[] = {
    { "tie_updatetime", 0x577F4, 0, 0 },         // once a frame: the clock
    { "user_inputforplane", 0x5CDA0, 0, 0 },     // the player's controls
    { "move_moveobjects", 0x32448, 0, 0 },       // every object moved
    { "laser_createprojectile", 0x2E0E4, 0, 0 },
    { "collide_laserhitcraft", 0x1433C, 0, 0 },
    { "collide_damagecraft", 0x148F0, 0, 0 },
    { "create_createcraft", 0x17BF8, 0, 0 },
    { "create_createmission", 0x16B8C, 0, 0 },
    { "shipext_Get_Mission_Path", 0x82504, 0, 0 },   // the mission file's name made from mission_name
    { "fediskio_loadspecies", 0x218D8, 0, 0 },       // the mission read, its craft about to be loaded
};
const Bit8u kUpdateTimeBytes[8] = { 0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x8b, 0x2d };
const Bit8u kCreateProjectileBytes[8] = { 0x51, 0x56, 0x57, 0x55, 0x83, 0xec, 0x1c, 0x89 };

// Debugging: WCNET_TIE_TRACE=<image address>,... (hex) logs the registers
// whenever the program reaches one of those places.
Bit32u g_trace[16];
unsigned g_traces = 0;
bool g_traceRead = false;
bool g_armed = false;           // DOS4GW.EXE ran: the program is awaited (for a minute)
bool g_found = false;
double g_armedUntilMs = 0;
Bit32u g_codeBase = 0;          // linear address of the code object
Bit32u g_dataBase = 0;          // ... and of the data object
double g_nextScanMs = 0;
double g_nextReportMs = 0;
unsigned g_frames = 0;
unsigned g_falseHits = 0;       // hook calls at an EIP whose low word is watched but which is no place of ours (the report)

bool rd8(Bit32u lin, Bit8u *v) { return mem_readb_checked(lin, v) ? false : true; }
bool rd32(Bit32u lin, Bit32u *v) { return mem_readd_checked(lin, v) ? false : true; }
// (the checked reads answer true on a fault)

Bit32u r32(Bit32u image) { return tie_r32(image); }
Bit16u r16(Bit32u image) { return tie_r16(image); }
Bit8u r8(Bit32u image) { return tie_r8(image); }

bool bytes_at(Bit32u lin, const Bit8u *want, size_t n) {
    for (size_t i = 0; i < n; i++) {
        Bit8u b;
        if (!rd8(lin + (Bit32u)i, &b) || b != want[i]) {
            return false;
        }
    }
    return true;
}

// The data object, by its rank table and resource path (DOS/4GW puts the
// objects where its memory manager finds room: no alignment to count on,
// so every dword is tried), then the code object by tie_updatetime's bytes
// in the megabyte below the data (the LE lays the code 0xB0000 below the
// data, the loader somewhat less: the gap is measured, not assumed).
bool scan() {
    static const Bit32u kRanks[5] = { 20000, 50000, 100000, 250000, 500000 };
    static const Bit8u kPath[9] = { 'R', 'E', 'S', 'O', 'U', 'R', 'C', 'E', '\\' };
    const Bit32u top = (Bit32u)MEM_TotalPages() << 12;
    unsigned unreadable = 0, firstRank = 0;
    static unsigned attempts = 0;
    attempts++;
    for (Bit32u a = 0x110000; a + 0x10000 < top; a += 4) {
        Bit32u v;
        if (!rd32(a, &v)) {
            unreadable++;
            a = (((a >> 12) + 1) << 12) - 4;    // (a page not there: on to the next)
            continue;
        }
        if (v != kRanks[0]) {
            continue;
        }
        if (!firstRank) {
            firstRank = a;
        }
        bool same = true;
        for (int i = 1; i < 5 && same; i++) {
            same = rd32(a + 4 * i, &v) && v == kRanks[i];
        }
        if (!same || a < (kRankScores - kDataImage)) {
            continue;
        }
        const Bit32u data = a - (kRankScores - kDataImage);
        if (!bytes_at(data + (kResourcePath - kDataImage), kPath, sizeof(kPath))) {
            continue;
        }
        for (Bit32u c = data - 4; c > 0x100000 && data - c < 0x100000; c -= 4) {
            if (!bytes_at(c, kUpdateTimeBytes, 8) || c < (0x577F4 - kCodeImage)) {
                continue;
            }
            const Bit32u code = c - (0x577F4 - kCodeImage);
            if (!bytes_at(code + (0x2E0E4 - kCodeImage), kCreateProjectileBytes, 8)) {
                continue;
            }
            g_codeBase = code;
            g_dataBase = data;
            return true;
        }
        wclog(1, "TIE Fighter: its data is at %08x but its code was not found below it: not the build the hooks know", data);
        return false;
    }
    if (attempts % 8 == 1) {
        wclog(2, "TIE Fighter: scan %u: nothing up to %08x, %u pages unreadable, first 20000 at %08x, %s, pmode %d", attempts, top, unreadable,
              firstRank, PAGING_Enabled() ? "paging on" : "paging off", (int)cpu.pmode);
    }
    return false;
}

void set_watch() {
    memset(wc_net_watch32, 0, sizeof(wc_net_watch32));
    if (!g_found) {
        return;
    }
    for (size_t i = 0; i < sizeof(g_places) / sizeof(g_places[0]); i++) {
        wc_net_watch32[tie_linear(g_places[i].image) & 0xffff] = true;
    }
    if (!g_traceRead) {
        g_traceRead = true;
        const char *env = getenv("WCNET_TIE_TRACE");
        for (const char *p = env; p && *p && g_traces < 16; ) {
            char *end;
            Bit32u a = (Bit32u)strtoul(p, &end, 16);
            if (end == p) {
                break;
            }
            g_trace[g_traces++] = a;
            p = *end == ',' ? end + 1 : end;
        }
    }
    for (unsigned i = 0; i < g_traces; i++) {
        wc_net_watch32[tie_linear(g_trace[i]) & 0xffff] = true;
    }
}

void report(double now) {
    using namespace tie;
    const Bit16u player = r16(kPstate + 8);
    int live = 0;
    for (int i = 0; i < kObjectCount; i++) {
        if (r8(kObjects + i * kObjectSize + 4) != 0) {
            live++;
        }
    }
    char counts[320];
    snprintf(counts, sizeof(counts), " false hits %u", g_falseHits);
    g_falseHits = 0;
    for (size_t i = 1; i < sizeof(g_places) / sizeof(g_places[0]); i++) {
        char one[48];
        snprintf(one, sizeof(one), " %s %u", g_places[i].name + (strchr(g_places[i].name, '_') ? strchr(g_places[i].name, '_') - g_places[i].name + 1 : 0),
                 g_places[i].calls);
        g_places[i].calls = 0;
        strncat(counts, one, sizeof(counts) - strlen(counts) - 1);
    }
    if (player < kObjectCount) {
        const Bit32u o = kObjects + player * kObjectSize;
        wclog(2, "TIE: %.1fs: %u frames (%u ticks a frame, rate %u), hyper %u main %u, %d objects live; player #%u type %u at %d %d %d, "
                 "heading %u pitch %u roll %u nose %d %d %d, speed %d, scene %d, mission '%s'; calls:%s",
              now / 1000.0, g_frames, r16(kFrameticks), r16(kFramerate), r8(kHyperspaceFlag), r8(kMainGameFlag), live, player,
              r8(o + 4), (int)r32(o + 6), (int)r32(o + 0xA), (int)r32(o + 0xE), r16(o + 0x1E), r16(o + 0x20), r16(o + 0x22),
              (Bit16s)r16(o + 0x3A), (Bit16s)r16(o + 0x3C), (Bit16s)r16(o + 0x3E), (Bit16s)r16(o + 0x26),
              tie_scene(), tie_mission_name().c_str(), counts);
    } else {
        wclog(2, "TIE: %.1fs: %u frames (%u ticks a frame, rate %u), hyper %u main %u, %d objects live, no player object (#%u), "
                 "the front end's pointer at %d %d (button %u key %u), scene %d; calls:%s",
              now / 1000.0, g_frames, r16(kFrameticks), r16(kFramerate), r8(kHyperspaceFlag), r8(kMainGameFlag), live, player,
              (Bit16s)r16(kCursorX), (Bit16s)r16(kCursorY), r16(kDownButton), r16(kKey), tie_scene(), counts);
    }
    g_frames = 0;
}

void clear_calls();     // (the calls into the program: below)

void forget(const char *why) {
    wclog(1, "TIE Fighter: %s", why);
    clear_calls();
    tie_session_shutdown();
    tie_cockpit_forget();
    g_armed = g_found = false;
    g_codeBase = g_dataBase = 0;
    set_watch();
}

}  // namespace

// (The extender's own process comes and goes as it sets itself up: the
// program's end is seen in memory instead, its code gone from where it was.)
void tie_program_loaded(const char *name, Bit16u psp) {
    if (g_armed || g_found) {
        wclog(2, "TIE Fighter: DOS loaded %s (PSP %04X) meanwhile", name, psp);
    }
    if (strcmp(name, "DOS4GW.EXE") != 0) {
        return;
    }
    g_armed = true;
    g_found = false;
    g_nextScanMs = PIC_FullIndex() + 500;
    g_armedUntilMs = PIC_FullIndex() + 60000;
    set_watch();
    wclog(1, "DOS4GW.EXE started (PSP %04X): looking for TIE Fighter's program", psp);
}

bool tie_program_ended(Bit16u psp) {
    (void)psp;
    return false;
}


void tie_tick() {
    tie_session_tick();
    if (!g_armed) {
        return;
    }
    const double now = PIC_FullIndex();
    static bool said = false;
    if (!said) {
        said = true;
        wclog(2, "TIE Fighter: the first tick at %.1fs, the scan from %.1fs", now / 1000.0, g_nextScanMs / 1000.0);
    }
    if (g_found) {
        if (now >= g_nextReportMs) {
            g_nextReportMs = now + 1000;
            if (!bytes_at(tie_linear(0x577F4), kUpdateTimeBytes, 8)) {
                forget("the program is gone from memory: ended");
                return;
            }
            report(now);
        }
        return;
    }
    if (now < g_nextScanMs) {
        return;
    }
    if (now > g_armedUntilMs) {
        forget("no program of the build the hooks know appeared within a minute of DOS4GW.EXE: not watched");
        return;
    }
    g_nextScanMs = now + 250;
    if (!scan()) {
        return;
    }
    g_found = true;
    g_frames = 0;
    g_nextReportMs = now + 1000;
    for (size_t i = 0; i < sizeof(g_places) / sizeof(g_places[0]); i++) {
        g_places[i].calls = g_places[i].total = 0;
    }
    set_watch();
    wclog(1, "TIE Fighter: the program is at %08x (code) and %08x (data, %s below it), %s, CS base %08x: watching %u places",
          g_codeBase, g_dataBase, g_dataBase - g_codeBase == kDataPastCode ? "the LE's 0xB0000" : "not the LE's 0xB0000",
          PAGING_Enabled() ? "paging on" : "paging off", (unsigned)SegPhys(cs),
          (unsigned)(sizeof(g_places) / sizeof(g_places[0])));
}

bool tie_found() { return g_found; }

namespace {
struct Call {
    Bit32u fn, eax, edx, ebx, ecx;
    TieCallBefore before;
    TieCallDone done;
    int tag;
};
std::deque<Call> g_calls;
struct Saved {
    bool active;
    Bit32u place;
    Bit32u eax, ebx, ecx, edx, esi, edi, ebp, esp, flags;
} g_saved = { false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

void clear_calls() {
    g_calls.clear();
    g_saved.active = false;
}

// At the hook place: a call of ours come back (the state given back, its done told), and the next one made.  True
// while a call is under way: the place's own work waits for the last to come back.
bool run_calls(Bit32u eip) {
    if (g_saved.active && eip == g_saved.place && reg_esp == g_saved.esp) {
        const Bit32u result = reg_eax;
        Call done = g_calls.front();
        g_calls.pop_front();
        reg_eax = g_saved.eax;
        reg_ebx = g_saved.ebx;
        reg_ecx = g_saved.ecx;
        reg_edx = g_saved.edx;
        reg_esi = g_saved.esi;
        reg_edi = g_saved.edi;
        reg_ebp = g_saved.ebp;
        reg_flags = g_saved.flags;
        lflags.type = t_UNKNOWN;    // (the flags are reg_flags' again)
        g_saved.active = false;
        if (done.done) {
            done.done(done.tag, result);
        }
    } else if (g_saved.active) {
        return false;               // (the place reached again inside our call: the game's own business)
    }
    while (!g_calls.empty() && g_found) {
        FillFlags();
        g_saved.active = true;
        g_saved.place = eip;
        g_saved.eax = reg_eax;
        g_saved.ebx = reg_ebx;
        g_saved.ecx = reg_ecx;
        g_saved.edx = reg_edx;
        g_saved.esi = reg_esi;
        g_saved.edi = reg_edi;
        g_saved.ebp = reg_ebp;
        g_saved.esp = reg_esp;
        g_saved.flags = reg_flags;
        const Call c = g_calls.front();
        CPU_Push32(eip);
        reg_eax = c.eax;
        reg_edx = c.edx;
        reg_ebx = c.ebx;
        reg_ecx = c.ecx;
        reg_eip = tie_linear(c.fn);
        if (!c.before || c.before(c.tag)) {
            return true;
        }
        // (not to be made after all: the state as it was, on to the next)
        g_calls.pop_front();
        reg_esp = g_saved.esp;
        reg_eip = eip;
        reg_eax = g_saved.eax;
        reg_ebx = g_saved.ebx;
        reg_ecx = g_saved.ecx;
        reg_edx = g_saved.edx;
        g_saved.active = false;
    }
    return false;
}
}  // namespace

void tie_queue_call(Bit32u fnImage, Bit32u eax, Bit32u edx, Bit32u ebx, Bit32u ecx, TieCallBefore before, TieCallDone done, int tag) {
    if (g_calls.size() >= 64) {
        wclog(1, "TIE: calls queued past 64: %05x dropped", (unsigned)fnImage);   // (a list has a cap)
        return;
    }
    Call c = { fnImage, eax, edx, ebx, ecx, before, done, tag };
    g_calls.push_back(c);
}

bool tie_in_call() { return g_saved.active; }

void tie_skip_call(Bit32u eax) {
    reg_eip = CPU_Pop32();
    reg_eax = eax;
}

// At a function's entry: back to the near call that made it (E8 and four bytes, before the return address), so the
// program makes the same call again next: how a launch is held (step 13).  False, and nothing done, for another call.
bool tie_repeat_call() {
    const Bit32u ret = mem_readd(SegPhys(ss) + reg_esp);
    if (ret < 5 || mem_readb(ret - 5) != 0xE8) {
        return false;
    }
    CPU_Pop32();
    reg_eip = ret - 5;
    // (and this time slice given up, as a HLT does: the call made again once a slice, not on every instruction, so the
    // emulator idles meanwhile instead of falling behind real time)
    CPU_IODelayRemoved += CPU_Cycles;
    CPU_Cycles = 0;
    return true;
}

Bit8u tie_r8(Bit32u image) { Bit8u v = 0; if (g_found) rd8(tie_linear(image), &v); return v; }
Bit16u tie_r16(Bit32u image) { return (Bit16u)tie_r32(image); }
Bit32u tie_r32(Bit32u image) { Bit32u v = 0; if (g_found) rd32(tie_linear(image), &v); return v; }
void tie_w8(Bit32u image, Bit8u v) { if (g_found) mem_writeb(tie_linear(image), v); }
void tie_w16(Bit32u image, Bit16u v) { if (g_found) mem_writew(tie_linear(image), v); }
void tie_w32(Bit32u image, Bit32u v) { if (g_found) mem_writed(tie_linear(image), v); }
bool tie_in_flight() { return g_found && tie_r8(tie::kMainGameFlag) == 1; }
std::string tie_mission_name() {
    std::string name;
    for (int i = 0; g_found && i < 14; i++) {
        const Bit8u c = tie_r8(tie::kMissionName + i);
        if (c == 0) {
            break;
        }
        name += (c >= 32 && c < 127) ? (char)c : '?';
    }
    return name;
}
int tie_scene() {
    const Bit32u head = tie_r32(tie::kSceneHead);
    Bit32u v = 0;
    if (!g_found || head < 0x100000 || mem_readd_checked(head + tie::kSceneHeadScene, &v)) {
        return -1;
    }
    return (Bit16s)(Bit16u)v;
}
int tie_player_object() {
    if (!g_found) {
        return -1;
    }
    const Bit16u obj = tie_r16(tie::kPstate + 8);
    return obj < tie::kObjectCount ? (int)obj : -1;
}

void tie_set_cursor(int x, int y) {
    if (!g_found) {
        wclog(1, "TIE: no program to put the pointer in");
        return;
    }
    mem_writew(tie_linear(kCursorX), (Bit16u)x);
    mem_writew(tie_linear(kCursorY), (Bit16u)y);
}

// Test aid: our craft turned to face another object (the key script's !tieface=<obj>).  An object's heading is its
// angle in the x-y plane from +y towards +x, its pitch the angle from +z (0x4000 level), 65,536 a turn; the game
// makes its vectors again from them (+0x40 and +0x38 set), and the craft's own copies of the two (+6, +8) follow.
void tie_face(int target) {
    const int own = tie_player_object();
    if (!g_found || own < 0 || target < 0 || target >= (int)tie::kObjectCount || target == own) {
        wclog(1, "TIE: nothing to face (ours %d, the object %d)", own, target);
        return;
    }
    const Bit32u a = tie_linear(tie::kObjects + own * tie::kObjectSize), b = tie_linear(tie::kObjects + target * tie::kObjectSize);
    const double dx = (double)(Bit32s)mem_readd(b + 6) - (Bit32s)mem_readd(a + 6), dy = (double)(Bit32s)mem_readd(b + 0xA) - (Bit32s)mem_readd(a + 0xA),
                 dz = (double)(Bit32s)mem_readd(b + 0xE) - (Bit32s)mem_readd(a + 0xE);
    const double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0) {
        return;
    }
    const Bit16u h = (Bit16u)(Bit16s)lround(atan2(dx, dy) * 32768.0 / M_PI), p = (Bit16u)lround(acos(dz / len) * 32768.0 / M_PI);
    mem_writew(a + 0x1E, h);
    mem_writew(a + 0x20, p);
    mem_writew(a + 0x22, 0);
    mem_writeb(a + 0x40, 1);
    mem_writeb(a + 0x38, 1);
    const Bit32u craft = mem_readd(a + 0x54);
    if (craft >= 0x100000) {
        mem_writew(craft + 6, p);
        mem_writew(craft + 8, h);
    }
    wclog(1, "TIE: our craft faces obj %d, %.2f km away (heading %u, pitch %u)", target, len / 40960.0, h, p);
}

bool tie_cursor(int *x, int *y) {
    if (!g_found) {
        return false;
    }
    *x = (Bit16s)mem_readw(tie_linear(kCursorX));
    *y = (Bit16s)mem_readw(tie_linear(kCursorY));
    return true;
}

// Our target (the player state's +0x12), -1 if none.
int tie_player_target() {
    if (!g_found) {
        return -1;
    }
    const int t = (Bit16s)mem_readw(tie_linear(tie::kPstate + 0x12));
    return t >= 0 && t < (int)tie::kObjectCount ? t : -1;
}

// Test aid: our target set (the key script's !tietarget=<obj>; the player state's +0x12).
void tie_target(int obj) {
    if (!g_found || obj < 0 || obj >= (int)tie::kObjectCount) {
        return;
    }
    mem_writew(tie_linear(tie::kPstate + 0x12), (Bit16u)obj);
    wclog(1, "TIE: our target is obj %d", obj);
}

Bit32u tie_linear(Bit32u image) {
    return image < kDataImage ? g_codeBase + (image - kCodeImage) : g_dataBase + (image - kDataImage);
}

void tie_check_hooks(Bit32u eip) {
    if (!g_found) {
        return;
    }
    g_falseHits++;      // (taken back below when it is one of ours)
    for (unsigned i = 0; i < g_traces; i++) {
        if (eip == tie_linear(g_trace[i])) {
            wclog(1, "TIE trace %05x: eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x esp %08x", g_trace[i], (unsigned)reg_eax,
                  (unsigned)reg_ebx, (unsigned)reg_ecx, (unsigned)reg_edx, (unsigned)reg_esi, (unsigned)reg_edi, (unsigned)reg_esp);
        }
    }
    for (size_t i = 0; i < sizeof(g_places) / sizeof(g_places[0]); i++) {
        Place &p = g_places[i];
        if (eip != tie_linear(p.image)) {
            continue;
        }
        p.calls++;
        p.total++;
        g_falseHits--;
        if (i == 0) {
            g_frames++;
            tie_controls_frame();       // (this frame's mickeys, before the game reads them)
            tie_session_frame();
        } else if (p.image == 0x16B8C) {
            tie_session_mission_create();
        } else if (p.image == 0x32448) {
            if (!run_calls(eip)) {
                tie_session_before_move();
            }
        } else if (p.image == 0x82504) {
            if (!(tie_session_mission_wait() && tie_repeat_call())) {
                tie_session_mission_path();
            }
        } else if (p.image == 0x218D8) {
            tie_session_before_species();
            tie_cockpit_before_species();
        } else if (p.image == 0x2E0E4) {
            tie_session_shot();
        } else if (p.image == 0x1433C) {
            tie_session_laser_hit();
        } else if (p.image == 0x148F0) {
            tie_session_damage();
        } else if (p.total <= 3) {
            // (Watcom's register convention: the first arguments in eax, edx, ebx, ecx)
            wclog(2, "TIE: %s call %u: eax %08x edx %08x ebx %08x ecx %08x", p.name, p.total, (unsigned)reg_eax, (unsigned)reg_edx,
                  (unsigned)reg_ebx, (unsigned)reg_ecx);
        }
        return;
    }
}

}  // namespace wc
