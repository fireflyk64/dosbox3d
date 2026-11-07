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
    char counts[256];
    counts[0] = 0;
    for (size_t i = 1; i < sizeof(g_places) / sizeof(g_places[0]); i++) {
        char one[48];
        snprintf(one, sizeof(one), " %s %u", g_places[i].name + (strchr(g_places[i].name, '_') ? strchr(g_places[i].name, '_') - g_places[i].name + 1 : 0),
                 g_places[i].calls);
        g_places[i].calls = 0;
        strncat(counts, one, sizeof(counts) - strlen(counts) - 1);
    }
    if (player < kObjectCount) {
        const Bit32u o = kObjects + player * kObjectSize;
        wclog(1, "TIE: %.1fs: %u frames (%u ticks a frame, rate %u), hyper %u main %u, %d objects live; player #%u type %u at %d %d %d, "
                 "heading %u pitch %u, speed %d, scene %d, mission '%s'; calls:%s",
              now / 1000.0, g_frames, r16(kFrameticks), r16(kFramerate), r8(kHyperspaceFlag), r8(kMainGameFlag), live, player,
              r8(o + 4), (int)r32(o + 6), (int)r32(o + 0xA), (int)r32(o + 0xE), r16(o + 0x1E), r16(o + 0x20), (Bit16s)r16(o + 0x26),
              tie_scene(), tie_mission_name().c_str(), counts);
    } else {
        wclog(1, "TIE: %.1fs: %u frames (%u ticks a frame, rate %u), hyper %u main %u, %d objects live, no player object (#%u), "
                 "the front end's pointer at %d %d (button %u key %u), scene %d; calls:%s",
              now / 1000.0, g_frames, r16(kFrameticks), r16(kFramerate), r8(kHyperspaceFlag), r8(kMainGameFlag), live, player,
              (Bit16s)r16(kCursorX), (Bit16s)r16(kCursorY), r16(kDownButton), r16(kKey), tie_scene(), counts);
    }
    g_frames = 0;
}

void forget(const char *why) {
    wclog(1, "TIE Fighter: %s", why);
    tie_session_shutdown();
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

Bit32u tie_linear(Bit32u image) {
    return image < kDataImage ? g_codeBase + (image - kCodeImage) : g_dataBase + (image - kDataImage);
}

void tie_check_hooks(Bit32u eip) {
    if (!g_found) {
        return;
    }
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
        if (i == 0) {
            g_frames++;
            tie_session_frame();
        } else if (p.image == 0x16B8C) {
            tie_session_mission_create();
        } else if (p.image == 0x32448) {
            tie_session_before_move();
        } else if (p.image == 0x82504) {
            tie_session_mission_path();
        } else if (p.total <= 3) {
            // (Watcom's register convention: the first arguments in eax, edx, ebx, ecx)
            wclog(2, "TIE: %s call %u: eax %08x edx %08x ebx %08x ecx %08x", p.name, p.total, (unsigned)reg_eax, (unsigned)reg_edx,
                  (unsigned)reg_ebx, (unsigned)reg_ecx);
        }
        return;
    }
}

}  // namespace wc
