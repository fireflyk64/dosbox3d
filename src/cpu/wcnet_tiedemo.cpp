/*
 *  The TIE Fighter demo: finding TIEMISS.EXE and watching it (wcnet_tiedemo.h, docs/tiedemo.md).
 *
 *  TIEMISS.EXE (363,910 bytes: a 293,861-byte MZ image with 2,095 relocations, then the overlays) is loaded by DOS for
 *  TIEDEMO.BAT ("tiemiss demo >foo"), so its load segment is known from the start; the build is the one whose image
 *  holds "TIE Fighter Demo V1.0 12/04/93" at 0x30A70.  Places in it are image paragraphs from the load segment.
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
#include "dos_inc.h"

namespace wc {

namespace {

const char kVersion[] = "TIE Fighter Demo V1.0 12/04/93";
const Bit32u kVersionImage = 0x30A70;       // its offset in the load image

// The program (image paragraphs and offsets; the data segment DGROUP is image paragraph 0x421F)
enum {
    kDgroupPara = 0x421F,
    kFramePara = 0x0090,        // the flight's frame, 0090:06CE (called from 0090:0006's loop): ... 0090:07D1 call 054A:0000
    kAfterMove = 0x07D6,        //   (every object moved), 0090:07D6 call 1768:058E (next: the picture)
    kObjects = 0x67CA,          // DS: the objects, kObjectSlots records of kObjectSize (054A:0153's loop)
    kObjectSlots = 0x74,
    kObjectSize = 0x58,
    kObjType = 0x04,            //   +4 its type (a byte: 0 a free slot; 4 the T/F, 15 the corvette ...)
    kObjPos = 0x06,             //   +6 x, +0x0A y, +0x0E z (int32; 054A:05DC adds the frame's motion, kept within +-0x01000000)
    kPlayerObj = 0xCD78,        // DS: the player's object (a near pointer)
    kPlayerCraft = 0xCD7A,      // DS: its craft data (a near pointer; the objects' +0x54 is a far one)
    kObjPrev = 0x12,            //   +0x12 x, y, z the frame before (int32)
    kObjAngles = 0x1E,          //   +0x1E three angle words, +0x26 the speed (225 the T/F's full)
    kObjGroup = 0x34,           //   +0x34 its flight group (the high byte: 0 Alpha)
    kObjVectors = 0x3A,         //   +0x3A its forward unit vector (3 words, 0x7FFF one), +0x40 0, +0x42 forward again,
                                //   +0x48 right, +0x4E up
    kObjCraft = 0x54,           //   +0x54 its craft data (a far pointer)
};
// the record's runs a pilot's craft is told by (TdmState.rec): its place and the frame before's, its angles and speed;
// its attitude's vectors
const int kRun1 = kObjPos, kRun1Len = 0x28 - kObjPos, kRun2 = kObjVectors, kRun2Len = kObjCraft - kObjVectors;

bool g_found = false;
Bit16u g_load = 0, g_psp = 0;
double g_nextCheckMs = 0;
// the flight's frames
Bit32u g_frame = 0;
double g_lastFrameMs = -1e9, g_frameMs = 50, g_rateSince = 0;
Bit32u g_rateFrames = 0;

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
Bit16u g_ds = 0;
// (the aid !tdmglue=dx:dy:dz: object 0 held beside the player, every frame after the moves, the player's record but its
// number copied with the offset added to its place)
bool g_glue = false;
Bit32s g_glueD[3] = { 0, 0, 0 };
std::vector<std::pair<double, int> > g_recAt;      // (the aid !tdmrec=<object>: its record logged once)

inline PhysPt dsa(Bit16u off) { return ((PhysPt)g_ds << 4) + off; }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline Bit32s r32s(Bit16u off) { return (Bit32s)mem_readd(dsa(off)); }
inline void w32(Bit16u off, Bit32u v) { mem_writed(dsa(off), v); }
inline Bit16u obj_at(int n) { return (Bit16u)(kObjects + n * kObjectSize); }

void log_record(int n) {
    const Bit16u o = obj_at(n);
    std::string words;
    for (int k = 0; k < kObjectSize; k += 2) {
        char b[8];
        snprintf(b, sizeof(b), " %04x", r16((Bit16u)(o + k)));
        words += b;
    }
    wclog(1, "TIE demo: object %d at DS:%04X (type %u, at %d %d %d):%s", n, o, mem_readb(dsa((Bit16u)(o + kObjType))),
          r32s((Bit16u)(o + kObjPos)), r32s((Bit16u)(o + kObjPos + 4)), r32s((Bit16u)(o + kObjPos + 8)), words.c_str());
}

void after_move() {
    const double now = PIC_FullIndex();
    if (now - g_lastFrameMs > 1000) {
        g_frame = 0;            // (a flight begun)
        g_rateSince = now;
        g_rateFrames = 0;
        wclog(1, "TIE demo: the flight's frames are coming");
    }
    g_lastFrameMs = now;
    g_frame++;
    if (++g_rateFrames >= 20) {
        g_frameMs = (now - g_rateSince) / g_rateFrames;
        g_rateSince = now;
        g_rateFrames = 0;
    }
    tdm_session_frame();
    if (g_glue) {
        const Bit16u me = r16(kPlayerObj), other = obj_at(0);
        if (me && me != other) {
            for (int k = 6; k < kObjectSize - 4; k += 2) mem_writew(dsa((Bit16u)(other + k)), r16((Bit16u)(me + k)));
            for (int a = 0; a < 3; a++) w32((Bit16u)(other + kObjPos + 4 * a), (Bit32u)(r32s((Bit16u)(me + kObjPos + 4 * a)) + g_glueD[a]));
        }
    }
}

bool version_here(Bit16u load) {
    const PhysPt at = ((PhysPt)load << 4) + kVersionImage;
    for (size_t i = 0; i < sizeof(kVersion) - 1; i++) {
        if (mem_readb(at + (PhysPt)i) != (Bit8u)kVersion[i]) return false;
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
    std::string at;
    for (size_t i = 0; i < top.size() && i < 30; i++) {
        char b[40];
        snprintf(b, sizeof(b), " %04X:%04X=%u", top[i].second >> 16, top[i].second & 0xFFFF, top[i].first);
        at += b;
    }
    wclog(1, "TIE demo profile: %u samples; the commonest places (image paragraph:IP=samples):%s", g_profSamples, at.c_str());
    std::string ds;
    std::vector<std::pair<unsigned, Bit16u> > dss;
    for (std::map<Bit16u, unsigned>::const_iterator i = g_profDs.begin(); i != g_profDs.end(); ++i) dss.push_back(std::make_pair(i->second, i->first));
    std::sort(dss.rbegin(), dss.rend());
    for (size_t i = 0; i < dss.size() && i < 6; i++) {
        char b[32];
        snprintf(b, sizeof(b), " %04X:%u", dss[i].second, dss[i].first);
        ds += b;
    }
    wclog(1, "TIE demo profile: DS (image paragraph:samples)%s", ds.c_str());
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

}  // namespace

bool tdm_found() { return g_found; }

bool tdm_local(TdmLocal *out) {
    out->inFlight = false;
    out->frame = g_frame;
    out->rec.clear();
    if (!g_found || !ours_running() || PIC_FullIndex() - g_lastFrameMs > 500) return false;
    const Bit16u me = r16(kPlayerObj);
    if (!me) return false;
    out->inFlight = true;
    for (int k = 0; k < kRun1Len; k++) out->rec += (char)mem_readb(dsa((Bit16u)(me + kRun1 + k)));
    for (int k = 0; k < kRun2Len; k++) out->rec += (char)mem_readb(dsa((Bit16u)(me + kRun2 + k)));
    return true;
}

double tdm_frame_ms() { return g_frameMs; }

// our flight group's `k`-th craft but ours (the same type and group word), 0: none
Bit16u wingman(int k) {
    const Bit16u me = r16(kPlayerObj);
    if (!me) return 0;
    const Bit8u type = mem_readb(dsa((Bit16u)(me + kObjType)));
    const Bit16u group = r16((Bit16u)(me + kObjGroup));
    int seen = 0;
    for (int n = 0; n < kObjectSlots; n++) {
        const Bit16u o = obj_at(n);
        if (o == me || mem_readb(dsa((Bit16u)(o + kObjType))) != type || r16((Bit16u)(o + kObjGroup)) != group) continue;
        if (seen++ == k) return o;
    }
    return 0;
}

bool tdm_take_place(int k) {
    if (!g_found) return false;
    const Bit16u me = r16(kPlayerObj), o = wingman(k);
    if (!me || !o) return false;
    const int runs[2][2] = { { kRun1, kRun1Len }, { kRun2, kRun2Len } };
    for (int r = 0; r < 2; r++) {
        for (int b = 0; b < runs[r][1]; b++) {
            const PhysPt x = dsa((Bit16u)(me + runs[r][0] + b)), y = dsa((Bit16u)(o + runs[r][0] + b));
            const Bit8u t = mem_readb(x);
            mem_writeb(x, mem_readb(y));
            mem_writeb(y, t);
        }
    }
    wclog(1, "TIE demo: our craft takes the place of our flight group's craft %d (object at DS:%04X): now at %d %d %d", k,
          o, r32s((Bit16u)(me + kObjPos)), r32s((Bit16u)(me + kObjPos + 4)), r32s((Bit16u)(me + kObjPos + 8)));
    return true;
}

bool tdm_put_pilot(int k, const std::string &rec, double frames) {
    if (!g_found || (int)rec.size() != kRun1Len + kRun2Len) return false;
    const Bit16u o = wingman(k);
    {
        if (!o) return false;
        for (int b = 0; b < kRun1Len; b++) mem_writeb(dsa((Bit16u)(o + kRun1 + b)), (Bit8u)rec[(size_t)b]);
        for (int b = 0; b < kRun2Len; b++) mem_writeb(dsa((Bit16u)(o + kRun2 + b)), (Bit8u)rec[(size_t)(kRun1Len + b)]);
        // (its place carried on by its motion, the frame before's place to this one's, since it was told)
        if (frames > 0) {
            if (frames > 30) frames = 30;
            for (int a = 0; a < 3; a++) {
                const Bit32s at = r32s((Bit16u)(o + kObjPos + 4 * a)), before = r32s((Bit16u)(o + kObjPrev + 4 * a));
                const Bit32s step = at - before;
                const Bit32s now = at + (Bit32s)(step * frames);
                w32((Bit16u)(o + kObjPos + 4 * a), (Bit32u)now);
                w32((Bit16u)(o + kObjPrev + 4 * a), (Bit32u)(now - step));
            }
        }
        return true;
    }
    return false;
}

void tdm_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    if (strcmp(name, "TIEMISS.EXE") != 0) return;
    if (g_found) tdm_session_shutdown();
    if (!version_here(loadSeg)) {
        wclog(0, "TIEMISS.EXE is not the TIE Fighter demo build the hooks know (\"%s\"): not watched", kVersion);
        return;
    }
    g_found = true;
    g_load = loadSeg;
    g_psp = psp;
    g_ds = (Bit16u)(loadSeg + kDgroupPara);
    g_nextCheckMs = PIC_FullIndex() + 1000;
    hooks_game_changed();       // (the dispatcher's table: our places)
    wclog(1, "TIE demo: TIEMISS.EXE loaded at %04X (PSP %04X): %s", loadSeg, psp, kVersion);
}

void tdm_tick() {
    if (!g_found) return;
    const double now = PIC_FullIndex();
    if (now >= g_nextCheckMs) {
        g_nextCheckMs = now + 1000;
        if (!version_here(g_load)) {
            wclog(1, "TIE demo: TIEMISS.EXE is gone from memory: ended");
            g_found = false;
            tdm_session_shutdown();
            return;
        }
    }
    tdm_session_tick();
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
    if (!g_found) return;
    watch(kAfterMove);
}

bool tdm_check_hooks(Bit32u ip) {
    if (!g_found || SegValue(cs) != (Bit16u)(g_load + kFramePara)) return false;
    if (ip == kAfterMove) {
        after_move();
        return true;
    }
    return false;
}

bool tdm_script_item(const std::string &item, double t) {
    if (item.compare(0, 9, "!tdmglue=") == 0) {
        g_glue = sscanf(item.c_str() + 9, "%d:%d:%d", &g_glueD[0], &g_glueD[1], &g_glueD[2]) == 3;
        wclog(1, "key script %.1fs: TIE demo: object 0 %s the player (%d %d %d)", t, g_glue ? "held beside" : "let go of",
              g_glueD[0], g_glueD[1], g_glueD[2]);
        return true;
    }
    if (item.compare(0, 8, "!tdmrec=") == 0) {
        g_recAt.push_back(std::make_pair(PIC_FullIndex(), atoi(item.c_str() + 8)));
        return true;
    }
    return false;
}

}  // namespace wc
