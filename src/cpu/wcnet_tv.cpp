/*
 *  Terminal Velocity: finding the program in memory and watching it (wcnet_tv.h, docs/terminalvelocity.md).
 *
 *  GAME.EXE is bound with DOS/4G: DOS loads it, the extender puts its LE image (a code object, image 0x10000, and a
 *  data object, image 0x60000, in the shareware) into extended memory, flat (every selector's base 0, paging off),
 *  so EIP and every pointer the program holds are linear addresses.  Nothing tells the layer where: the code is
 *  looked for by scripts/tvsig.py's windows (wcnet_tv_sig.h), and every data address is read out of the found code's
 *  own operands, as the loader relocated them.
 */
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wcnet_tv.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "mem.h"
#include "paging.h"
#include "regs.h"
#include "cpu.h"
#include "pic.h"
#include "lazyflags.h"
#include "wcnet_tv_sig.h"

namespace wc {

namespace {

using namespace tvsig;

bool g_armed = false;           // GAME.EXE ran: the program is awaited (for a minute)
bool g_found = false;
double g_armedUntilMs = 0;
double g_nextScanMs = 0;
double g_nextReportMs = 0;
Bit32u g_anchor[A_COUNT];       // each anchor's window, linear
Bit32u g_data[D_COUNT];         // each data address, linear
Bit32u g_frames = 0, g_framesReported = 0;
double g_lastFrameMs = -1e9;
unsigned g_counts[8];           // hook calls since the last report: frame, draw, update, skipped, hit, fire, level, false
int g_level = -1;
std::string g_levelName;
Bit32u g_hitSkip = 0;

// The places watched (linear), by what they are.
enum Place { P_FRAME, P_DRAW, P_UPDATE, P_HIT, P_FIRE, P_LEVEL, P_LEVEL_END, P_COUNT };
Bit32u g_campaignCall = 0;     // the campaign loop's call of the level function (its return address: + 5)
Bit32u g_places[P_COUNT];

bool rd8(Bit32u lin, Bit8u *v) { return mem_readb_checked(lin, v) ? false : true; }
bool rd32(Bit32u lin, Bit32u *v) { return mem_readd_checked(lin, v) ? false : true; }

Bit32u place_of(int anchor) { return g_anchor[anchor] + (Bit32u)kAnchors[anchor].place; }

// Does the anchor's window stand at lin (its kept bytes equal)?
bool window_at(const Anchor &a, Bit32u lin) {
    const Bit32u top = (Bit32u)MEM_TotalPages() << 12;
    if (lin + (Bit32u)a.len > top) {
        return false;
    }
    for (int i = 0; i < a.len; i++) {
        if (a.mask[i] && MemBase[lin + (Bit32u)i] != a.bytes[i]) {
            return false;
        }
    }
    return true;
}

// The anchor's windows in [lo, hi), up to `cap` of them (their starts in out[]): how many there are.
int find_anchor(const Anchor &a, Bit32u lo, Bit32u hi, Bit32u *out, int cap) {
    const Bit32u top = (Bit32u)MEM_TotalPages() << 12;
    if (hi > top) hi = top;
    const Bit8u *key = a.bytes + a.key;
    int found = 0;
    for (Bit32u p = lo + (Bit32u)a.key; p + (Bit32u)a.keyLen <= hi;) {
        const Bit8u *hit = (const Bit8u *)memchr(MemBase + p, key[0], hi - (Bit32u)a.keyLen - p + 1);
        if (!hit) break;
        p = (Bit32u)(hit - MemBase);
        if (memcmp(hit, key, (size_t)a.keyLen) == 0 && p >= (Bit32u)a.key && window_at(a, p - (Bit32u)a.key)) {
            if (found < cap) out[found] = p - (Bit32u)a.key;
            found++;
        }
        p++;
    }
    return found;
}

// The program's code by the fire function's window, then every anchor near it.  Memory can hold a second copy of the
// code as the file has it (the extender's read buffer: measured, the shareware's fire function twice), whose windows
// match too, since the bytes the loader relocates are not compared: of each anchor's windows the one nearest where
// the shareware's layout puts it from the fire function is taken, and a copy is the program only when every data
// operand of its windows is relocated (a linear address past the first megabyte, where the extender loads).
bool resolve(Bit32u fire) {
    const Bit32u top = (Bit32u)MEM_TotalPages() << 12;
    const Bit32u lo = fire > 0x100000 + 0x80000 ? fire - 0x80000 : 0x100000;
    const Bit32u hi = std::min(top, fire + 0x80000);
    for (int i = 0; i < A_COUNT; i++) {
        Bit32u at[8];
        const int k = find_anchor(kAnchors[i], lo, hi, at, 8);
        if (k < 1) {
            wclog(1, "Terminal Velocity: code near %08x, but '%s' was not found: not a build the hooks know", fire, kAnchors[i].name);
            return false;
        }
        const Bit32s want = (Bit32s)(kAnchors[i].image - kAnchors[i].place) - (Bit32s)kAnchors[A_FIRE_FN].image;
        int best = 0;
        for (int j = 1; j < std::min(k, 8); j++) {
            if (labs((Bit32s)(at[j] - fire) - want) < labs((Bit32s)(at[best] - fire) - want)) best = j;
        }
        g_anchor[i] = at[best];
    }
    for (int i = 0; i < D_COUNT; i++) {
        Bit32u v = 0;
        rd32(g_anchor[kData[i].anchor] + (Bit32u)kData[i].at, &v);
        if (v < 0x100000 || v >= top) {
            return false;
        }
        g_data[i] = v;
    }
    return true;
}

bool scan() {
    static unsigned attempts = 0;
    attempts++;
    if (PAGING_Enabled()) {
        if (attempts % 8 == 1) wclog(2, "Terminal Velocity: paging is on: not looked for");
        return false;
    }
    const Bit32u top = (Bit32u)MEM_TotalPages() << 12;
    Bit32u fire[4];
    const int n = find_anchor(kAnchors[A_FIRE_FN], 0x100000, top, fire, 4);
    int ok = -1;
    for (int c = 0; c < std::min(n, 4) && ok < 0; c++) {
        if (resolve(fire[c])) ok = c;
    }
    if (ok < 0) {
        if (attempts % 8 == 1) wclog(2, "Terminal Velocity: scan %u: the fire function found %d times, none the program's", attempts, n);
        return false;
    }
    g_places[P_FRAME] = place_of(A_FRAME_TOP);
    g_places[P_DRAW] = place_of(A_REMOTE_DRAW);
    g_places[P_UPDATE] = place_of(A_OBJ_UPDATE);
    g_places[P_HIT] = place_of(A_HIT_APPLY);
    g_places[P_FIRE] = place_of(A_FIRE_FN);
    g_places[P_LEVEL] = place_of(A_LEVEL_FN);                             // (the level function's entry)
    g_campaignCall = place_of(A_CAMPAIGN) + (0x2EC79 - 0x2EC58);          // (the campaign's call of it: ebx its index)
    g_places[P_LEVEL_END] = g_campaignCall + 5;                             // (... and where it comes back: the level over)
    // the test's end, where a hit we answer "skip" goes on (counted, nothing applied): the same function's, as the
    // shareware has it, or none (every hit applied)
    g_hitSkip = place_of(A_HIT_END) - place_of(A_HIT_APPLY) == 0x2937F - 0x29327 ? place_of(A_HIT_END) : 0;
    return true;
}

void set_watch(bool on) {
    for (int i = 0; i < P_COUNT; i++) {
        if (g_places[i]) wc_net_watch32[g_places[i] & 0xffff] = on;
    }
}

void clear_calls();

void forget(const char *why) {
    wclog(1, "Terminal Velocity: %s", why);
    clear_calls();
    tv_session_shutdown();
    set_watch(false);
    memset(g_places, 0, sizeof(g_places));
    g_armed = g_found = false;
    g_level = -1;
    g_levelName.clear();
}

void report(double now) {
    TvShip s;
    const bool flying = tv_player(&s);
    const Bit32u frames = g_frames - g_framesReported;
    g_framesReported = g_frames;
    wclog(2, "Terminal Velocity: %.1fs: %u frames, %s, level %d '%s', %d objects, %d near; player at %d %d %d ang %d %d %d "
             "shield %d; calls: draw %u update %u skipped %u hit %u fire %u level %u",
          now / 1000.0, frames, flying ? "flying" : "not flying", g_level, g_levelName.c_str(), tv_object_count(),
          (int)tv_r32(g_data[D_NEAR_COUNT]), s.pos[0], s.pos[1], s.pos[2], s.ang[0], s.ang[1], s.ang[2], s.shield,
          g_counts[1], g_counts[2], g_counts[3], g_counts[4], g_counts[5], g_counts[6]);
    memset(g_counts, 0, sizeof(g_counts));
}

// -- calls into the program ----------------------------------------------------------------------------------
struct Call {
    int where;
    Bit32u fn;
    std::vector<Bit32u> args, scratch;
    TvCallDone done;
    int tag;
};
std::deque<Call> g_calls[2];
struct Saved {
    bool active;
    int where;
    Bit32u place;
    Bit32u eax, ebx, ecx, edx, esi, edi, ebp, esp, flags;
    Bit32u expectEsp;
    double since;
} g_saved;

void clear_calls() {
    g_calls[0].clear();
    g_calls[1].clear();
    g_saved.active = false;
}

// At a call's place: one of ours come back (true: the state given back, its done told).
bool call_returned(Bit32u eip) {
    if (!g_saved.active || eip != g_saved.place || reg_esp != g_saved.expectEsp) {
        return false;
    }
    const Bit32u result = reg_eax;
    Call c = g_calls[g_saved.where].front();
    g_calls[g_saved.where].pop_front();
    reg_eax = g_saved.eax;
    reg_ebx = g_saved.ebx;
    reg_ecx = g_saved.ecx;
    reg_edx = g_saved.edx;
    reg_esi = g_saved.esi;
    reg_edi = g_saved.edi;
    reg_ebp = g_saved.ebp;
    reg_esp = g_saved.esp;
    reg_flags = g_saved.flags;
    lflags.type = t_UNKNOWN;
    g_saved.active = false;
    if (c.done) {
        c.done(c.tag, result);
    }
    return true;
}

// The next of ours queued for this place, begun (true: EIP is its function's now).
bool call_next(int where, Bit32u eip) {
    if (g_saved.active || g_calls[where].empty() || !g_found) {
        return false;
    }
    const Call &c = g_calls[where].front();
    FillFlags();
    g_saved.active = true;
    g_saved.where = where;
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
    g_saved.since = PIC_FullIndex();
    Bit32u sp = reg_esp - 64;               // (clear of anything the place keeps just below its stack)
    sp -= (Bit32u)c.scratch.size() * 4;
    sp &= ~3u;
    const Bit32u scratchAt = sp;
    for (size_t i = 0; i < c.scratch.size(); i++) {
        mem_writed(SegPhys(ss) + scratchAt + (Bit32u)i * 4, c.scratch[i]);
    }
    for (size_t i = c.args.size(); i-- > 0;) {
        sp -= 4;
        mem_writed(SegPhys(ss) + sp, c.args[i] == TV_SCRATCH ? scratchAt : c.args[i]);
    }
    g_saved.expectEsp = sp;
    sp -= 4;
    mem_writed(SegPhys(ss) + sp, eip);
    reg_esp = sp;
    reg_eip = c.fn;
    return true;
}

// A call that never came back (the level ended under it): given up after 3 s, the program's state left as it is.
void calls_watchdog(double now) {
    if (g_saved.active && now - g_saved.since > 3000) {
        wclog(1, "Terminal Velocity: a call of ours to %08x never came back: given up", (unsigned)g_calls[g_saved.where].front().fn);
        g_calls[g_saved.where].pop_front();
        g_saved.active = false;
    }
}

}  // namespace

void tv_program_loaded(const char *name, Bit16u psp) {
    if (strcmp(name, "GAME.EXE") != 0) {
        return;
    }
    if (g_found) {
        forget("GAME.EXE started again");
    }
    g_armed = true;
    g_found = false;
    g_nextScanMs = PIC_FullIndex() + 500;
    g_armedUntilMs = PIC_FullIndex() + 60000;
    wclog(1, "GAME.EXE started (PSP %04X): looking for Terminal Velocity's program", psp);
}

void tv_tick() {
    if (g_found || g_armed) {
        tv_session_tick();
    }
    if (!g_armed) {
        return;
    }
    const double now = PIC_FullIndex();
    if (g_found) {
        calls_watchdog(now);
        if (now >= g_nextReportMs) {
            g_nextReportMs = now + 1000;
            if (!window_at(kAnchors[A_FIRE_FN], g_anchor[A_FIRE_FN])) {
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
        g_armed = false;
        wclog(1, "Terminal Velocity: no program of a build the hooks know appeared within a minute of GAME.EXE: not watched");
        return;
    }
    g_nextScanMs = now + 500;
    if (!scan()) {
        return;
    }
    g_found = true;
    g_frames = g_framesReported = 0;
    g_nextReportMs = now + 1000;
    set_watch(true);
    wclog(1, "Terminal Velocity: the program's code at %08x (the fire function; image 0x19080 in the shareware), the player "
             "at %08x, the objects' pointer at %08x, the campaign's level at %08x: watching %d places",
          g_anchor[A_FIRE_FN], g_data[D_PLAYER_POS], g_data[D_OBJ_TABLE], g_data[D_LEVEL_INDEX], (int)P_COUNT);
    for (int i = 0; i < D_COUNT; i++) {
        if (g_data[i] - kData[i].image != g_data[D_PLAYER_POS] - kData[D_PLAYER_POS].image) {
            wclog(2, "Terminal Velocity: %s at %08x (not where the shareware's layout puts it)", kData[i].name, g_data[i]);
        }
    }
}

bool tv_found() { return g_found; }

void tv_check_hooks(Bit32u eip) {
    if (!g_found) {
        return;
    }
    if (eip == g_places[P_FRAME]) {
        if (call_returned(eip)) {
            call_next(TV_AT_FRAME, eip);
            return;
        }
        if (g_saved.active) return;
        g_frames++;
        g_lastFrameMs = PIC_FullIndex();
        g_counts[0]++;
        tv_pilot_frame();
        tv_session_frame();
        call_next(TV_AT_FRAME, eip);
    } else if (eip == g_places[P_DRAW]) {
        if (call_returned(eip)) {
            call_next(TV_AT_DRAW, eip);
            return;
        }
        if (g_saved.active) return;
        g_counts[1]++;
        tv_session_draw();
        call_next(TV_AT_DRAW, eip);
    } else if (eip == g_places[P_UPDATE]) {
        // (the call of an object's update: the object pushed already, at [esp])
        g_counts[2]++;
        const Bit32u obj = mem_readd(SegPhys(ss) + reg_esp);
        const Bit32u table = tv_r32(g_data[D_OBJ_TABLE]);
        if (obj >= table && (obj - table) % kTvObjSize == 0) {
            const int i = (int)((obj - table) / kTvObjSize);
            if (tv_session_puppet(i)) {
                g_counts[3]++;
                reg_eip = eip + 5;          // (its update not made: the call skipped, its argument popped by the caller)
            } else {
                tv_session_updated(i);
            }
        }
    } else if (eip == g_places[P_HIT]) {
        // (a hit confirmed: ebx the object, [esp+0x1c] the damage, [esp+0x10] the caller's ebx -- the projectile, when
        // the projectiles' loop made the test; our answer: the game applies it, or it is counted and skipped)
        g_counts[4]++;
        const Bit32u obj = reg_ebx;
        const Bit32u sp = SegPhys(ss) + reg_esp;
        const Bit32s damage = (Bit32s)mem_readd(sp + 0x1c);
        const Bit32u table = tv_r32(g_data[D_OBJ_TABLE]);
        if (obj >= table && (obj - table) % kTvObjSize == 0 && !g_saved.active) {
            const bool apply = tv_session_hit((int)((obj - table) / kTvObjSize), damage, mem_readd(sp + 0x10));
            if (!apply && g_hitSkip) {
                mem_writed(sp, mem_readd(sp) + 1);     // (a hit all the same: the projectile is spent)
                reg_eip = g_hitSkip;
            }
        }
    } else if (eip == g_places[P_FIRE]) {
        g_counts[5]++;
        const Bit32u sp = SegPhys(ss) + reg_esp;
        const Bit32u owner = mem_readd(sp + 0x10);
        if (owner == 0 && !g_saved.active) {
            Bit32u posmat[16];
            const Bit32u p = mem_readd(sp + 4);
            for (int i = 0; i < 16; i++) posmat[i] = tv_r32(p + (Bit32u)i * 4);
            tv_session_fired(posmat, mem_readd(sp + 8), mem_readd(sp + 0xc), mem_readd(sp + 0x14));
        }
    } else if (eip == g_places[P_LEVEL_END]) {
        tv_session_level_end();
    } else if (eip == g_places[P_LEVEL]) {
        // The level function's entry: from the campaign's call (its return address), a level of the campaign about to be
        // flown, ebx its index, [esp+4] its file's name.  The session may give it another index (a guest flies the
        // host's level), or hold it: the call made again a time slice later (the program idles meanwhile), as long
        // as the session says so.
        const Bit32u ret = mem_readd(SegPhys(ss) + reg_esp);
        if (ret != g_campaignCall + 5) {
            return;
        }
        int index = (int)reg_ebx;
        if (tv_session_level_call(&index)) {
            CPU_Pop32();
            reg_eip = ret - 5;
            CPU_IODelayRemoved += CPU_Cycles;
            CPU_Cycles = 0;
            return;
        }
        if (index != (int)reg_ebx && index >= 0 && index < (int)tv_r32(g_data[D_LEVEL_COUNT])) {
            reg_ebx = (Bit32u)index;
            tv_w32(g_data[D_LEVEL_INDEX], (Bit32u)index);
            mem_writed(SegPhys(ss) + reg_esp + 4, g_data[D_LEVEL_NAMES] + (Bit32u)index * 0x28);
        }
        g_counts[6]++;
        g_level = (int)reg_ebx;
        g_levelName = tv_level_name();
        wclog(1, "Terminal Velocity: level %d (%s) about to be flown, difficulty %d", g_level, g_levelName.c_str(),
              (int)tv_r32(g_data[D_DIFFICULTY]));
        tv_session_level(g_level, g_levelName);
    }
}

bool tv_in_flight() {
    return g_found && PIC_FullIndex() - g_lastFrameMs < 500;
}

bool tv_player(TvShip *out) {
    memset(out, 0, sizeof(*out));
    if (!g_found) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        out->pos[i] = (Bit32s)tv_r32(g_data[D_PLAYER_POS] + 4 * i);
        out->ang[i] = (Bit32s)tv_r32(g_data[D_PLAYER_POS] + 12 + 4 * i);
        out->vel[i] = (Bit32s)tv_r32(g_data[D_PLAYER_VEL] + 4 * i);
        out->spin[i] = (Bit32s)tv_r32(g_data[D_PLAYER_SPIN] + 4 * i);
    }
    out->shield = (Bit32s)tv_r32(g_data[D_PLAYER_SHIELD]);
    return tv_in_flight();
}

int tv_level() { return g_found ? g_level : -1; }

std::string tv_level_name() {
    if (!g_found) return "";
    const Bit32u at = g_data[D_LEVEL_NAMES] + (Bit32u)(g_level < 0 ? 0 : g_level) * 0x28;
    std::string s;
    for (Bit32u i = 0; i < 0x28; i++) {
        Bit8u c = 0;
        if (!rd8(at + i, &c) || !c) break;
        s += (char)c;
    }
    return s;
}

Bit32u tv_frames() { return g_frames; }

int tv_object_count() {
    if (!g_found) return 0;
    const int n = (int)tv_r32(g_data[D_OBJ_COUNT]);
    return n < 0 ? 0 : std::min(n, (int)kTvMaxObjects);
}

Bit32u tv_object_addr(int i) {
    if (!g_found || i < 0 || i >= tv_object_count()) return 0;
    return tv_r32(g_data[D_OBJ_TABLE]) + (Bit32u)i * kTvObjSize;
}

int tv_object_class(int i) {
    const Bit32u o = tv_object_addr(i);
    if (!o) return -1;
    const Bit32u t = tv_r32(g_data[D_OBJ_TYPES]) + tv_r32(o + 0x18) * kTvTypeSize;
    return (int)tv_r32(t + 0xc);
}

// A type's words: its model files, then its name and what the briefing says of it ("CRAFT", "Ymir", "SnowScout")
Bit32u tv_object_type_addr(int i) {
    const Bit32u o = tv_object_addr(i);
    return o ? tv_r32(g_data[D_OBJ_TYPES]) + tv_r32(o + 0x18) * kTvTypeSize : 0;
}

std::string tv_object_name(int i) {
    const Bit32u o = tv_object_addr(i);
    if (!o) return "";
    const Bit32u t = tv_r32(g_data[D_OBJ_TYPES]) + tv_r32(o + 0x18) * kTvTypeSize;
    std::string out, word;
    int words = 0;
    for (Bit32u k = 0x10; k < kTvTypeSize && words < 6; k++) {
        Bit8u c = 0;
        rd8(t + k, &c);
        if (c >= 0x21 && c < 0x7f) {
            word += (char)c;
        } else {
            if (word.size() >= 3) {
                out += (out.empty() ? "" : " ") + word;
                words++;
            }
            word.clear();
        }
    }
    return out;
}

Bit32u tv_r32(Bit32u lin) { Bit32u v = 0; if (g_found) rd32(lin, &v); return v; }
void tv_w32(Bit32u lin, Bit32u v) { if (g_found) mem_writed(lin, v); }

void tv_queue_call(int where, Bit32u fn, const std::vector<Bit32u> &args, const std::vector<Bit32u> &scratch, TvCallDone done, int tag) {
    if (!g_found || !fn || where < 0 || where > 1) return;
    if (g_calls[where].size() >= 64) {
        wclog(1, "Terminal Velocity: calls queued past 64: %08x dropped", (unsigned)fn);     // (a list has a cap)
        return;
    }
    Call c;
    c.where = where;
    c.fn = fn;
    c.args = args;
    c.scratch = scratch;
    c.done = done;
    c.tag = tag;
    g_calls[where].push_back(c);
}

bool tv_in_call() { return g_saved.active; }

Bit32u tv_fn(TvFn f) {
    if (!g_found) return 0;
    switch (f) {
    case TV_FN_FIRE: return place_of(A_FIRE_FN);
    case TV_FN_DRAW_MODEL: return place_of(A_DRAW_MODEL_FN);
    case TV_FN_HIT: return place_of(A_HIT_FN);
    case TV_FN_LOAD_MODEL: return place_of(A_LOAD_MODEL_FN);
    case TV_FN_DESTROY: return place_of(A_DESTROY_FN);
    case TV_FN_MESSAGE: return place_of(A_MESSAGE_FN);
    case TV_FN_POD_LOOKUP: {
        // (the model loader's first call: the archives' lookup, by its near call's offset)
        const Bit32u call = place_of(A_LOAD_MODEL_FN) + (0x402F1 - 0x402E0);
        Bit8u op = 0;
        rd8(call, &op);
        return op == 0xE8 ? call + 5 + tv_r32(call + 1) : 0;
    }
    }
    return 0;
}

Bit32u tv_data(int id) { return g_found && id >= 0 && id < D_COUNT ? g_data[id] : 0; }

bool tv_script_item(const std::string &item, double t) {
    if (tv_pilot_item(item, t)) {
        return true;
    }
    if (item == "!tvpos") {
        TvShip s;
        tv_player(&s);
        wclog(1, "key script %.1fs: Terminal Velocity: player at %d %d %d ang %d %d %d vel %d %d %d spin %d %d %d shield %d, level %d",
              t, s.pos[0], s.pos[1], s.pos[2], s.ang[0], s.ang[1], s.ang[2], s.vel[0], s.vel[1], s.vel[2], s.spin[0], s.spin[1],
              s.spin[2], s.shield, g_level);
        return true;
    }
    if (item.compare(0, 7, "!tvobjs") == 0) {
        // !tvobjs[=km]: the objects within that far of the player (default 3 km... in the game's units / 65536)
        TvShip s;
        tv_player(&s);
        const double range = item.size() > 8 ? atof(item.c_str() + 8) : 200.0;
        const int n = tv_object_count();
        int shown = 0;
        for (int i = 0; i < n && shown < 60; i++) {
            const Bit32u o = tv_object_addr(i);
            double d2 = 0;
            for (int k = 0; k < 3; k++) {
                const double d = ((double)(Bit32s)tv_r32(o + 4 * k) - s.pos[k]) / 65536.0;
                d2 += d * d;
            }
            if (sqrt(d2) > range) continue;
            shown++;
            wclog(1, "key script %.1fs: object %d class %d '%s' at %d %d %d ang %d %d %d hp %d state %d +20 %d at %.0f",
                  t, i, tv_object_class(i), tv_object_name(i).c_str(), (Bit32s)tv_r32(o), (Bit32s)tv_r32(o + 4), (Bit32s)tv_r32(o + 8),
                  (Bit32s)tv_r32(o + 12), (Bit32s)tv_r32(o + 16), (Bit32s)tv_r32(o + 20), (Bit32s)tv_r32(o + 0x1c),
                  (Bit32s)tv_r32(o + 0x64), (Bit32s)tv_r32(o + 0x20), sqrt(d2));
        }
        return true;
    }
    return false;
}

}  // namespace wc
