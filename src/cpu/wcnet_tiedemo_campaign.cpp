/*
 *  The TIE Fighter demo's campaign (wcnet_tiedemo.h, docs/tiedemo.md section 8).
 *
 *  The demo flies one mission: TIEMISS.EXE's main loop flies DEMO.TIE again after a death or an eject and ends only
 *  when a flight's status is 3 (the mission over: its time run out).  The layer makes a campaign of it without
 *  changing the game's files or code:
 *    * The missions.  Mission 0 is the demo's own; the next ones are scripts/tdmmissions.py's (the demo's own mission
 *      file layout, wcnet_tiedemo_missions.h).  create_createmission reads DEMO.TIE whole into memory (the header at
 *      DS:C280, the flight groups at the mission segment's 0xD0, the radio messages at its 0x3790); just after the read
 *      (0FC2:0136) the layer writes the mission to fly over that copy.
 *    * The end.  A mission is won when the game says its primary goals are complete (DS:614F, score_checkobjective);
 *      the layer then ends the flight (the end flag, DS:614D) once its rule allows (Mission: a few seconds, or once
 *      what may still come after has been quiet), or ends it a few seconds after a primary goal has failed.
 *    * The next.  Where the flight's frames end (0090:04FB, before its status is looked at) the status is written:
 *      3 (the program ends, as the demo's does) once the last mission is won, else one that has the game's own loop
 *      fly again, the next mission or the same one.
 *    * In a room the host decides: its go names the mission (TdmState.mission), its verdict on each flight the next
 *      one; a guest's game waits at 0090:04FB for the host's verdict on the flight it flew.
 *  The T/I and T/B have no cockpit of their own in the demo (their species records name none): the layer names the
 *  T/F's (TIEFTR) in theirs when the program is loaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include "wcnet_tiedemo.h"
#include "wcnet_tiedemo_missions.h"
#include "wcnet_log.h"
#include "mem.h"
#include "pic.h"

namespace wc {

namespace {

using namespace tdm;
using tdm_missions::kMissions;
using tdm_missions::kMissionCount;

enum {
    kHeader = 0xC280,               // DS: the mission file's header (0x1C8 bytes: the counts, then EMissionStruct)
    kHeaderSize = 0x1C8,
    kMsgBase = 0x3790,              // the mission segment: the radio messages, 90 bytes each
    kMsgSize = 90,
    kMsgCount = 10,
    kPrimaryDone = 0x614F,          // DS: 1 once the primary goals are complete (score_checkobjective)
    kFgStatus = 0xC492,             // DS: each flight group's primary goal's state (its record of 0x30 bytes from C46E,
                                    //   +0x24): 0 open, 1 met (never looked at again), 0xFF failed
    kFgStatusSize = 0x30,
    kFgPrimaryCond = 0x9E,          // a flight group's primary goal (its condition; 0 or 10: none)
    kFailDelayS = 8,
    kEndAfterMs = 600,              // the flight's end flag set this long after its end is decided (zero_warheads)
    kHostEndMs = 3000,              // a guest's flight ends this long after the host's verdict (the room's end)
    kWeaponSlots = 0x114,           // CraftData: the weapon slots, 6 bytes each: +0 type (0x14 a laser, 0x18 a torpedo
    kWeaponSlotSize = 6,            //   launcher), +1 charge, +2 its ammunition (a word)
    kWeaponSlotCount = 8,
    kVerdictWaitMs = 20000,         // a guest waits this long at most for the host's verdict
    kExplosionFirst = 0x40,
    kExplosionLast = 0x43,
};

// The T/I's and T/B's species records (TIEMISS's table, 0xDC each): their full names' place in the load image, the
// cockpit's name 0x3C past it (the T/F's: "TIEFTR").
const Bit32u kTfName = 0x32B03, kTiName = 0x32BDF, kTbName = 0x32CBB;
const Bit32u kCockpitName = 0x3C;

const char kDemoTitle[] = "The Ambush";
const char kDemoBrief[] = "Mission 1 of 4, T/F Alpha: the Chimaera's scouts have found the rebels' repair yard. Ambush "
                          "it, and every craft that comes to it.";

int g_next = 0;                     // the mission this machine launches next
int g_flying = -1;                  // the mission flown now (-1: none)
double g_primaryAt = -1, g_failAt = -1, g_tailAt = -1;
bool g_ending = false, g_won = false;
double g_endAt = -1;                // (the end flag to be set then)
// the layer's radio lines (Mission.timed): the flight's first frame, each flight group's first craft seen, the lines
// let go (one bit each)
double g_flightStartAt = -1;
double g_fgSeenAt[kFgCount];
unsigned g_timedDone = 0;
// (the aid !tdmrebels: the rebels' craft destroyed one at a time as they come, by the game's own damage)
bool g_killRebels = false;
int g_killing = -1, g_killCalls = 0;
double g_killNextAt = 0;
bool g_hostSaid = false;            // the host's verdict on this flight, kept when it came (its program may end, and the
int g_hostNext = 0;                 //   room with it, before this flight's end)
bool g_hostOver = false;
// a protected flight group's craft (Mission.protect): each slot's craft as last seen alive (its id, its flight group)
int g_seenId[kCraftSlots];
int g_seenFg[kCraftSlots];
double g_waitSince = -1;            // (a guest at 0090:04FB, waiting for the host's verdict)

int mission_total() { return kMissionCount + 1; }
const tdm_missions::Mission *mission_of(int n) { return n >= 1 && n <= kMissionCount ? &kMissions[n - 1] : NULL; }
std::string title_of(int n) { return n == 0 ? kDemoTitle : mission_of(n) ? mission_of(n)->title : "?"; }

inline Bit8u o8(int o, int k) { return mem_readb(tdm_obj(o) + (PhysPt)k); }
inline Bit16u o16(int o, int k) { return mem_readw(tdm_obj(o) + (PhysPt)k); }
inline Bit8u ds8(Bit16u off) { return mem_readb(tdm_ds(off)); }

bool alive_craft(int o) {
    const Bit8u type = o8(o, 4);
    return type != 0 && o8(o, 2) == 0 && (type < kExplosionFirst || type > kExplosionLast) && o16(o, 0x2C) == 0;
}

PhysPt mission_seg() { return (PhysPt)mem_readw(tdm_ds(kFgSeg)) << 4; }

bool name_at(PhysPt a, const char *name) {
    for (size_t i = 0; i <= strlen(name); i++) {
        if (mem_readb(a + (PhysPt)i) != (Bit8u)name[i]) return false;
    }
    return true;
}

void write_mission(const tdm_missions::Mission &m) {
    const unsigned char *d = m.data;
    for (int i = 0; i < kHeaderSize; i++) mem_writeb(tdm_ds((Bit16u)(kHeader + i)), d[i]);
    d += kHeaderSize;
    const PhysPt fgs = mission_seg() + kFgBase;
    for (int fg = 0; fg < kFgCount; fg++) {
        for (int i = 0; i < kFgSize; i++) mem_writeb(fgs + (PhysPt)(fg * kFgSize + i), fg < m.fgs ? d[fg * kFgSize + i] : 0);
    }
    d += m.fgs * kFgSize;
    const PhysPt msgs = mission_seg() + kMsgBase;
    for (int k = 0; k < kMsgCount; k++) {
        for (int i = 0; i < kMsgSize; i++) mem_writeb(msgs + (PhysPt)(k * kMsgSize + i), k < m.msgs ? d[k * kMsgSize + i] : 0);
    }
}

// A primary goal failed: a flight group with a primary goal whose state is "failed".
bool primary_failed() {
    const int nfg = (int)(Bit16s)mem_readw(tdm_ds(kHeader));
    const PhysPt fgs = mission_seg() + kFgBase;
    for (int fg = 0; fg < nfg && fg < kFgCount; fg++) {
        const Bit8u cond = mem_readb(fgs + (PhysPt)(fg * kFgSize + kFgPrimaryCond));
        if (cond == 0 || cond == 10) continue;
        if (ds8((Bit16u)(kFgStatus + fg * kFgStatusSize)) == 0xFF) return true;
    }
    return false;
}

bool listed(const unsigned char *list, int fg) {
    for (const unsigned char *t = list; t && *t != 255; t++) {
        if (*t == fg) return true;
    }
    return false;
}

// A craft of a protected flight group dying (seen alive, now its death under way): its name, else empty.
std::string protected_lost(const tdm_missions::Mission &m) {
    if (m.protect[0] == 255) return "";
    std::string lost;
    const PhysPt fgs = mission_seg() + kFgBase;
    for (int o = 0; o < kCraftSlots; o++) {
        const int id = o16(o, 0);
        if (alive_craft(o)) {
            g_seenId[o] = id;
            g_seenFg[o] = o8(o, 0x35);
            continue;
        }
        if (g_seenId[o] < 0) continue;
        const bool dying = o8(o, 4) != 0 && g_seenId[o] == id;     // (the same craft, its death under way)
        if (dying && listed(m.protect, g_seenFg[o]) && lost.empty()) {
            char name[13];
            for (int i = 0; i < 12; i++) name[i] = (char)mem_readb(fgs + (PhysPt)(g_seenFg[o] * kFgSize + i));
            name[12] = 0;
            lost = name;
        }
        if (!dying) g_seenId[o] = -1;
    }
    return lost;
}

// The cockpit's warhead counter is drawn only when its count changes and never for a craft without warheads: a T/F
// flown after the T/B kept the T/B's last count on its panel.  So our craft's warheads are emptied a few frames
// before a flight's end: the counter shows 0 when the next flight begins.
void zero_warheads() {
    const int me = tdm_player_object();
    const PhysPt cd = me >= 0 && me < kCraftSlots ? tdm_craft(me) : 0;
    if (!cd) return;
    for (int k = 0; k < kWeaponSlotCount; k++) {
        const PhysPt slot = cd + kWeaponSlots + (PhysPt)(k * kWeaponSlotSize);
        if (mem_readb(slot) >= 0x18) mem_writew(slot + 2, 0);
    }
}

// A radio line of the layer's let go: its two conditions made "always" (true at the game's next look; shown by it).
void release_timed(const tdm_missions::Mission &m, double now) {
    if (g_flightStartAt < 0) g_flightStartAt = now;
    for (int o = 0; o < kCraftSlots; o++) {
        if (!alive_craft(o)) continue;
        const int fg = o8(o, 0x35);
        if (fg < kFgCount && g_fgSeenAt[fg] < 0) g_fgSeenAt[fg] = now;
    }
    int k = 0;
    for (const unsigned char *t = m.timed; t && t[0] != 255; t += 3, k++) {
        if (g_timedDone & (1u << k)) continue;
        const double base = t[1] == 255 ? g_flightStartAt : t[1] < kFgCount ? g_fgSeenAt[t[1]] : -1;
        if (base < 0 || now - base < t[2] * 1000.0) continue;
        g_timedDone |= 1u << k;
        const PhysPt msg = mission_seg() + kMsgBase + (PhysPt)(t[0] * kMsgSize);
        for (int i = 64; i < 72; i++) mem_writeb(msg + (PhysPt)i, 0);
    }
}

void kill_done(int tag, Bit16u ax);
void kill_next(double now) {
    if (g_killing >= 0 || now < g_killNextAt || tdm_in_call()) return;
    const int me = tdm_player_object();
    for (int o = 0; o < kCraftSlots; o++) {
        if (o == me || !alive_craft(o) || o8(o, 0x33) != 0) continue;     // (+0x33: its side; 0 the rebels')
        g_killing = o;
        g_killCalls = 0;
        tdm_queue_call(TDM_DAMAGE, 4, (Bit16u)o, 0xFFFF, 0xFFFF, 0, NULL, kill_done, o);
        return;
    }
}
void kill_done(int tag, Bit16u ax) {
    (void)ax;
    if (alive_craft(tag) && ++g_killCalls < 40) {
        tdm_queue_call(TDM_DAMAGE, 4, (Bit16u)tag, 0xFFFF, 0xFFFF, 0, NULL, kill_done, tag);
        return;
    }
    g_killing = -1;
    g_killNextAt = PIC_FullIndex() + 400;
}

void end_flight(bool won) {
    g_ending = true;
    g_won = won;
    zero_warheads();
    g_endAt = PIC_FullIndex() + kEndAfterMs;
    wclog(1, "TIE demo campaign: mission %d (%s) %s: the flight ended", g_flying, title_of(g_flying).c_str(), won ? "won" : "failed");
}

}  // namespace

// --- the program --------------------------------------------------------------------------------------------------

void tdm_campaign_loaded() {
    g_next = 0;
    g_flying = -1;
    const char *env = getenv("TDM_MISSION");          // (a test's start: that mission first)
    if (env && env[0]) g_next = atoi(env) < 0 ? 0 : atoi(env) > kMissionCount ? kMissionCount : atoi(env);
    const PhysPt img = (PhysPt)tdm_load_segment() << 4;
    if (!name_at(img + kTfName, "TIE FIGHTER") || !name_at(img + kTfName + kCockpitName, "TIEFTR")) {
        wclog(0, "TIE demo campaign: the species table is not where it was expected: the T/I and T/B keep no cockpit");
        return;
    }
    const Bit32u names[2] = { kTiName, kTbName };
    const char *craft[2] = { "TIE INTERCEPTOR", "TIE BOMBER" };
    for (int k = 0; k < 2; k++) {
        if (!name_at(img + names[k], craft[k]) || mem_readb(img + names[k] + kCockpitName) != 0) continue;
        for (int i = 0; i < 7; i++) mem_writeb(img + names[k] + kCockpitName + (PhysPt)i, (Bit8u)"TIEFTR"[i]);
    }
    wclog(1, "TIE demo campaign: %d missions; the T/I and T/B fly with the T/F's cockpit%s", mission_total(),
          g_next ? " (starting at a later mission)" : "");
}

int tdm_campaign_next() { return g_next; }

void tdm_campaign_mission_read() {
    int n = g_next;
    if (tdm_session_role() == TDM_GUEST) n = tdm_session_launch_mission();
    if (n < 0 || n > kMissionCount) n = 0;
    g_flying = n;
    g_next = n;
    g_primaryAt = g_failAt = g_tailAt = -1;
    g_ending = g_won = false;
    g_waitSince = -1;
    g_endAt = -1;
    g_hostSaid = false;
    g_killing = -1;
    g_flightStartAt = -1;
    g_timedDone = 0;
    for (int fg = 0; fg < kFgCount; fg++) g_fgSeenAt[fg] = -1;
    for (int o = 0; o < kCraftSlots; o++) g_seenId[o] = g_seenFg[o] = -1;
    const tdm_missions::Mission *m = mission_of(n);
    if (m) write_mission(*m);
    const char *file = getenv("TDM_MISSION_FILE");     // (a test's: that mission file flown instead of a later mission)
    if (m && file && file[0]) {
        static std::vector<unsigned char> bytes;
        FILE *f = fopen(file, "rb");
        if (f) {
            bytes.assign(65536, 0);
            bytes.resize(fread(&bytes[0], 1, bytes.size(), f));
            fclose(f);
        }
        static tdm_missions::Mission fromFile;
        fromFile = *m;
        if (bytes.size() >= (size_t)kHeaderSize) {
            fromFile.data = &bytes[0];
            fromFile.size = (unsigned)bytes.size();
            fromFile.fgs = (Bit16s)(bytes[0] | bytes[1] << 8);
            fromFile.msgs = (Bit16s)(bytes[2] | bytes[3] << 8);
            if ((size_t)(kHeaderSize + fromFile.fgs * kFgSize + fromFile.msgs * kMsgSize) <= bytes.size()) {
                write_mission(fromFile);
                wclog(1, "TIE demo campaign: %s flown instead (%d flight groups)", file, fromFile.fgs);
            }
        }
    }
    char line[400];
    snprintf(line, sizeof(line), "%s", m ? m->brief : kDemoBrief);
    tdm_notice(line);
    wclog(1, "TIE demo campaign: mission %d of %d, %s%s", n + 1, mission_total(), title_of(n).c_str(),
          m ? " (written over the mission read)" : " (the demo's own)");
}

void tdm_campaign_flight_ending() { zero_warheads(); }

void tdm_campaign_host_verdict(int next, bool over) {
    if (g_flying < 0 || g_hostSaid) return;
    g_hostSaid = true;
    g_hostNext = next;
    g_hostOver = over;
    g_ending = true;                                  // (ended here as the room does, even if the room is gone first)
    if (g_endAt < 0) g_endAt = PIC_FullIndex() + kHostEndMs;
}

void tdm_campaign_frame() {
    if (g_endAt > 0 && PIC_FullIndex() >= g_endAt) {
        g_endAt = -1;
        mem_writeb(tdm_ds(kMissionEnd), 1);
    }
    if (g_flying < 0 || g_ending || !tdm_in_flight()) return;
    if (mission_of(g_flying)) release_timed(*mission_of(g_flying), PIC_FullIndex());
    if (g_killRebels) kill_next(PIC_FullIndex());
    if (tdm_session_role() == TDM_GUEST) return;          // (the host decides)
    const double now = PIC_FullIndex();
    if (g_primaryAt < 0 && ds8(kPrimaryDone) == 1) {
        g_primaryAt = now;
        const bool last = g_flying + 1 >= mission_total();
        tdm_notice(last ? "Primary goals complete: the campaign is won" : "Primary goals complete: next, " + title_of(g_flying + 1));
        wclog(1, "TIE demo campaign: mission %d's primary goals complete", g_flying);
    }
    const tdm_missions::Mission *m = mission_of(g_flying);
    if (g_primaryAt < 0 && g_failAt < 0) {
        const std::string lost = m ? protected_lost(*m) : std::string();
        if (!lost.empty() || primary_failed()) {
            g_failAt = now;
            tdm_notice((lost.empty() ? std::string("A primary goal has failed") : lost + " is lost") + ": the mission will be flown again");
            wclog(1, "TIE demo campaign: mission %d: %s", g_flying, lost.empty() ? "a primary goal failed" : (lost + " lost").c_str());
        }
    }
    if (m && m->tail[0] != 255) {
        for (int o = 0; o < kCraftSlots; o++) {
            if (!alive_craft(o)) continue;
            const Bit8u fg = o8(o, 0x35);
            for (const unsigned char *t = m->tail; *t != 255; t++) {
                if (*t == fg) g_tailAt = now;
            }
        }
    }
    if (g_primaryAt >= 0) {
        const double since = (now - g_primaryAt) / 1000.0;
        const int minDelay = m ? m->minDelay : 10;
        bool quiet = true;
        if (m && m->tail[0] != 255) quiet = (now - (g_tailAt > g_primaryAt ? g_tailAt : g_primaryAt)) / 1000.0 >= m->quiet;
        if (since >= minDelay && (quiet || (m && m->cap > 0 && since >= m->cap))) end_flight(true);
    } else if (g_failAt >= 0 && (now - g_failAt) / 1000.0 >= kFailDelayS) {
        end_flight(false);
    }
}

bool tdm_campaign_flight_over() {
    if (g_flying < 0) return false;
    const Bit8u status = ds8(kMissionStatus);
    if (status >= 10) return false;                       // (the film's: not ours)
    // A pilot's own end of the program (Esc): the mission over (3), the end flag 1, and not by us, the room or its
    // time (2): it ends as the demo's does.  The host's ends the room's campaign too (a guest's program then ends).
    if (status == 3 && ds8(kMissionEnd) == 1 && !g_ending && !g_hostSaid && !tdm_session_room_ended() && g_waitSince < 0) {
        if (tdm_session_role() == TDM_HOST) tdm_session_verdict(g_flying, true);
        wclog(1, "TIE demo campaign: mission %d ended by its pilot: the program ends", g_flying);
        g_flying = -1;
        return false;
    }
    int next = g_flying;
    bool over = false;
    if (g_hostSaid) {
        next = g_hostNext;                                // (the host's word, though its room may be gone)
        over = g_hostOver;
    } else if (tdm_session_role() == TDM_GUEST) {
        const double now = PIC_FullIndex();
        if (g_waitSince < 0) g_waitSince = now;
        if (tdm_session_verdict_wait(&next, &over)) {
            if (now - g_waitSince < kVerdictWaitMs) return true;
            next = g_flying;
            over = false;
            wclog(1, "TIE demo campaign: no verdict from the host: mission %d again", next);
        }
    } else {
        const bool won = g_won || ds8(kPrimaryDone) == 1;
        next = won ? g_flying + 1 : g_flying;
        over = next >= mission_total();                   // (next: the count, the campaign won)
        tdm_session_verdict(next, over);
    }
    g_waitSince = -1;
    const int flew = g_flying;
    g_flying = -1;
    g_next = over ? flew : next;
    if (over) {
        mem_writeb(tdm_ds(kMissionStatus), 3);
        mem_writeb(tdm_ds(kMissionEnd), 1);
        tdm_notice(next >= mission_total() ? "The campaign is won. Grand Admiral Thrawn will want to meet Alpha's pilots."
                                           : "The host has ended the demo");
        wclog(1, "TIE demo campaign: mission %d over, the campaign over: the program ends", flew);
        return false;
    }
    if (status == 3) mem_writeb(tdm_ds(kMissionStatus), 2);      // (the game's loop flies again)
    if (ds8(kMissionEnd) == 2) mem_writeb(tdm_ds(kMissionEnd), 1);  // (its time run out: not the program's end)
    wclog(1, "TIE demo campaign: mission %d over (status %u): mission %d next", flew, status, next);
    if (next != flew) {
        tdm_notice("Mission complete. Next: " + title_of(next));
    }
    return false;
}

bool tdm_campaign_script_item(const std::string &item, double t) {
    if (item.compare(0, 12, "!tdmmission=") == 0) {
        // the next flight's mission; the flight under way ended (flown again as that one)
        const int n = atoi(item.c_str() + 12);
        g_next = n < 0 ? 0 : n > kMissionCount ? kMissionCount : n;
        if (g_flying >= 0) {
            g_flying = g_next;
            g_won = false;
            g_ending = true;
            mem_writeb(tdm_ds(kMissionEnd), 1);
        }
        wclog(1, "key script %.1fs: TIE demo campaign: mission %d next", t, g_next);
        return true;
    }
    if (item == "!tdmcraft") {
        // every craft: its object, flight group, type, place (km from ours), speed, the goals' state of its group
        const int me = tdm_player_object();
        const double kUnitsPerKm = 40960.0;
        std::string out;
        for (int o = 0; o < kCraftSlots; o++) {
            if (!o8(o, 4)) continue;
            const Bit8u fg = o8(o, 0x35);
            char b[280];
            double dkm = -1;
            if (me >= 0) {
                double s = 0;
                for (int a = 0; a < 3; a++) {
                    const double d = (double)(Bit32s)mem_readd(tdm_obj(o) + 6 + 4 * a) - (double)(Bit32s)mem_readd(tdm_obj(me) + 6 + 4 * a);
                    s += d * d;
                }
                dkm = sqrt(s) / kUnitsPerKm;
            }
            const Bit16u cd = o16(o, 0x54);
            const unsigned hull = cd ? mem_readw(tdm_ds((Bit16u)(cd + kCraftHull))) : 0;
            const unsigned hullMax = cd ? mem_readw(tdm_ds((Bit16u)(cd + kCraftHullMax))) : 0;
            const int shields = cd ? (Bit16s)mem_readw(tdm_ds((Bit16u)(cd + kCraftShieldFront))) +
                                     (Bit16s)mem_readw(tdm_ds((Bit16u)(cd + kCraftShieldRear))) : 0;
            int warheads = 0;
            for (int k = 0; cd && k < kWeaponSlotCount; k++) {
                const Bit16u slot = (Bit16u)(cd + kWeaponSlots + k * kWeaponSlotSize);
                if (ds8(slot) >= 0x18) warheads += mem_readw(tdm_ds((Bit16u)(slot + 2)));
            }
            const unsigned id = tdm_session_ident(o);
            snprintf(b, sizeof(b), " [%d id%u.%u fg%u t%u %.2fkm (%.2f %.2f %.2f) v%d hull %u/%u sh %d wh %d%s g%u]", o, id >> 8, id & 0xFF, fg, o8(o, 4), dkm,
                     (Bit32s)mem_readd(tdm_obj(o) + 6) / kUnitsPerKm, (Bit32s)mem_readd(tdm_obj(o) + 10) / kUnitsPerKm,
                     (Bit32s)mem_readd(tdm_obj(o) + 14) / kUnitsPerKm, (Bit16s)o16(o, 0x26), hull, hullMax, shields,
                     warheads, alive_craft(o) ? "" : " dying", ds8((Bit16u)(kFgStatus + fg * kFgStatusSize)));
            out += b;
        }
        wclog(1, "key script %.1fs: TIE demo craft (primary %u):%s", t, ds8(kPrimaryDone), out.c_str());
        return true;
    }
    if (item == "!tdmcd") {
        // our craft's data (CraftData, 0x18C bytes) in hex
        const int me = tdm_player_object();
        const PhysPt cd = me >= 0 ? tdm_craft(me) : 0;
        std::string out;
        for (int i = 0; cd && i < 0x18C; i++) {
            char b[4];
            snprintf(b, sizeof(b), "%02x", mem_readb(cd + (PhysPt)i));
            out += b;
        }
        wclog(1, "key script %.1fs: TIE demo craft data: %s", t, out.c_str());
        return true;
    }
    if (item == "!tdmrebels") {
        g_killRebels = !g_killRebels;
        g_killing = -1;
        wclog(1, "key script %.1fs: TIE demo campaign: the rebels %s", t, g_killRebels ? "destroyed as they come" : "left alone");
        return true;
    }
    if (item == "!tdmwin") {
        // the primary goals marked complete (the end rule then runs as for a win)
        mem_writeb(tdm_ds(kPrimaryDone), 1);
        wclog(1, "key script %.1fs: TIE demo campaign: the primary goals marked complete", t);
        return true;
    }
    return false;
}

}  // namespace wc
