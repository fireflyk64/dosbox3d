/*
 *  Wolfenstein 3-D: finding WOLF3D.EXE and watching it (wcnet_wolf.h, docs/wolf3d.md).
 *
 *  WOLF3D.EXE is packed (LZEXE): what DOS loads unpacks itself, so the program is looked for in memory once it runs,
 *  by scripts/wolfsig.py's anchors (wcnet_wolf_sig.h): runs of its code with every address masked out, each found
 *  exactly once.  The shareware (Apogee 1.4) and the six-episode game (GOG's: Activision's 1.4) are the same source
 *  built twice -- the second for a 386, so wherever a long is handled its instructions differ; the anchors are runs
 *  both have.  Every data address is an operand of the found code; the code segments are the far calls' own (the
 *  loader has relocated them); the actor states are found in the data segment by what they are (the guard's first:
 *  sprite 50 turning with the view, no time; B.J.'s run: sprites 408..411).
 *
 *  The places watched (each an IP, its CS checked):
 *    SetupGameLevel's entry   a guest's level made the room's (held there, in the PSP's unused bytes, while the host's
 *                             is not known yet); the level's tables are new after it
 *    PlayLoop's frame         after the controls are read, before the doors and the actors: the session's work, and
 *                             our calls into the program made from here (DamageActor, KillActor, PushWall, DrawKeys,
 *                             GetNewActor: CPU state only, no code written)
 *    PlayLoop's end           playstate says why
 *    DoActor's entry          passed by for an actor thought for on another machine, and for the other players' own
 *    DamageActor's entry      a hit on such an actor is told to its owner instead
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include "wcnet_wolf.h"
#include "wcnet_wolf_sig.h"
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

extern std::string incoming_text;   // the line the emulator draws over the picture (sdlmain.cpp)

namespace wc {

using namespace wolfsig;

namespace {

enum {
    // an object (objtype, 60 bytes)
    OB_ACTIVE = 0x00, OB_TICCOUNT = 0x02, OB_CLASS = 0x04, OB_STATE = 0x06, OB_FLAGS = 0x08, OB_DISTANCE = 0x0A, OB_DIR = 0x0E,
    OB_X = 0x10, OB_Y = 0x14, OB_TILEX = 0x18, OB_TILEY = 0x1A, OB_AREA = 0x1C, OB_VIEWX = 0x1E, OB_VIEWHEIGHT = 0x20,
    OB_ANGLE = 0x2A, OB_HP = 0x2C, OB_SPEED = 0x2E, OB_TEMP1 = 0x32, OB_TEMP2 = 0x34, OB_TEMP3 = 0x36, OB_NEXT = 0x38, OB_PREV = 0x3A,
    OB_SIZE = 0x3C,
    // gamestate
    GS_DIFFICULTY = 0x00, GS_MAPON = 0x02, GS_SCORE = 0x08, GS_NEXTEXTRA = 0x0C, GS_LIVES = 0x10, GS_HEALTH = 0x12, GS_AMMO = 0x14,
    GS_KEYS = 0x16, GS_ATTACKFRAME = 0x20, GS_EPISODE = 0x26, GS_SECRETCOUNT = 0x28,
    // a state (statetype, 16 bytes)
    ST_ROTATE = 0, ST_SHAPE = 2, ST_TICS = 4, ST_THINK = 6, ST_ACTION = 10, ST_NEXT = 14, ST_SIZE = 16,
    // a door (doorobj_t, 10 bytes), from its action
    DOOR_SIZE = 10, DOOR_TILEX = -6, DOOR_TILEY = -5, DOOR_VERTICAL = -4, DOOR_TICCOUNT = 2,
    CLASS_INERT = 2,
    kCodeSpan = 0x3C000,        // the program's code and the start of its data, from its load segment
    kStubOff = 0x5C,            // the level hold's idle: "jmp $" written in the PSP's first FCB (unused by the program)
    kSeekEveryMs = 60,
    kSeekForMs = 30000,
    kAvatars = kWolfMaxPlayers - 1,
    kNumAreas = 37,
};

enum Stage { STAGE_NONE, STAGE_SEEKING, STAGE_FOUND, STAGE_UNKNOWN };
Stage g_stage = STAGE_NONE;
Bit16u g_load = 0, g_psp = 0, g_ds = 0;
double g_seekSince = 0, g_nextSeekMs = 0;
unsigned g_seeks = 0;
Bit32u g_anchor[A_COUNT];
Bit32u g_item[I_COUNT];
struct Code { Bit16u seg, ip; };
Code g_place[P_COUNT], g_doActor, g_getNewActor, g_killActor, g_drawKeys;
Bit16u g_stGuard = 0, g_stBj[4] = { 0, 0, 0, 0 };
bool g_full = false;                // the six-episode game's data beside the program
std::string g_wbuild;

// the level
bool g_levelSetup = false, g_tableValid = false, g_released = false, g_holding = false, g_escDown = false;
std::vector<Bit16u> g_actors;       // the level's actors (objects: offsets in the data segment)
Bit32u g_levelSig = 0;
Bit16s g_slotIndex[kWolfMaxActors]; // an object slot's actor (-1: none)
bool g_slotSkip[kWolfMaxActors];    // DoActor passes it by
double g_lastFrameMs = -1e9;
Bit32u g_frames = 0, g_levelFrames = 0;
bool g_inPlay = false, g_flightSaid = false;
unsigned g_levels = 0;

// the other players' actors
struct Avatar {
    Bit16u ob;
    bool asked, shown;
    char label[48];
    Avatar() : ob(0), asked(false), shown(false) { label[0] = 0; }
};
Avatar g_avatar[kAvatars];
WolfMarker g_markers[kAvatars];

// a pushed wall of another machine's: the tiles it leaves take the pusher's area as it goes
struct RemotePush { bool on; int x, y, dir, area; } g_remotePush = { false, 0, 0, 0, 0 };
struct PushWait { int x, y, dir, area; };
std::deque<PushWait> g_pushWait;

// the notice
std::string g_notice;
double g_noticeUntil = 0;

// (rig aids)
bool g_god = false;
unsigned g_callsMade = 0, g_doActorSkips = 0, g_damageTold = 0;

inline PhysPt dsa(Bit16u off) { return ((PhysPt)g_ds << 4) + off; }
inline Bit8u r8(Bit16u off) { return mem_readb(dsa(off)); }
inline Bit16u r16(Bit16u off) { return mem_readw(dsa(off)); }
inline Bit32u r32(Bit16u off) { return mem_readd(dsa(off)); }
inline void w8(Bit16u off, Bit8u v) { mem_writeb(dsa(off), v); }
inline void w16(Bit16u off, Bit16u v) { mem_writew(dsa(off), v); }
inline void w32(Bit16u off, Bit32u v) { mem_writed(dsa(off), v); }
inline Bit16u it(int item) { return (Bit16u)g_item[item]; }
inline Bit16u gs(int field) { return (Bit16u)(it(I_GAMESTATE) + field); }

bool ours_running() { return g_stage == STAGE_FOUND && dos.psp() == g_psp; }

int slot_of(Bit16u ob) {
    const int d = (int)ob - (int)it(I_OBJLIST);
    if (d < 0 || d % OB_SIZE) return -1;
    const int s = d / OB_SIZE;
    return s < kWolfMaxActors ? s : -1;
}

Bit16u player_ob() { return r16(it(I_PLAYER)); }

// actorat [tilex][tiley] and tilemap [tilex][tiley]
inline Bit16u actorat_at(int tx, int ty) { return (Bit16u)(it(I_ACTORAT) + (tx << 7) + (ty << 1)); }
inline Bit16u tilemap_at(int tx, int ty) { return (Bit16u)(it(I_TILEMAP) + (tx << 6) + ty); }
inline bool on_map(int tx, int ty) { return tx >= 0 && tx < 64 && ty >= 0 && ty < 64; }
// a map plane's word for a tile (plane 0: walls and areas; 1: the things)
PhysPt plane_at(int plane, int tx, int ty) {
    const Bit16u seg = r16((Bit16u)(it(I_MAPSEGS1) - 2 + 2 * plane));
    const Bit16u row = r16((Bit16u)(it(I_FARMAPYLOOKUP) + 2 * ty));
    return ((PhysPt)seg << 4) + (Bit16u)((row + tx) * 2);
}
int area_of_tile(int tx, int ty) {
    if (!on_map(tx, ty)) return -1;
    const int a = (int)mem_readw(plane_at(0, tx, ty)) - kWolfAreaTile;
    return a >= 0 && a < kNumAreas ? a : -1;
}

// --- finding the program ------------------------------------------------------------------------------------------

// one anchor in the code read: its place, or -1 (none, or more than one)
long find_anchor(const std::vector<Bit8u> &buf, const Anchor &a) {
    long found = -1;
    const Bit8u *key = a.bytes + a.key;
    const size_t n = buf.size();
    for (size_t i = (size_t)a.key; i + (size_t)(a.len - a.key) <= n; i++) {
        if (buf[i] != key[0] || memcmp(&buf[i], key, (size_t)a.keyLen) != 0) continue;
        const size_t s = i - (size_t)a.key;
        bool same = true;
        for (int k = 0; k < a.len && same; k++) same = !a.mask[k] || buf[s + (size_t)k] == a.bytes[k];
        if (!same) continue;
        if (found >= 0) return -1;
        found = (long)s;
    }
    return found;
}

bool code_of(Bit32u linear, Bit16u seg, Code *out) {
    const long ip = (long)linear - ((long)seg << 4);
    if (ip < 0 || ip > 0xFFFF) return false;
    out->seg = seg;
    out->ip = (Bit16u)ip;
    return true;
}

// the actor states, by what they are
bool find_states() {
    std::vector<Bit8u> d(0x7000);
    MEM_BlockRead(dsa(0), &d[0], d.size());
    #define W(o) ((Bit16u)(d[(o)] | (d[(o) + 1] << 8)))
    #define D(o) ((Bit32u)(W(o) | ((Bit32u)W((o) + 2) << 16)))
    g_stGuard = 0;
    for (size_t o = 0x100; o + ST_SIZE <= d.size(); o += 2) {
        if (W(o + ST_ROTATE) == 1 && W(o + ST_SHAPE) == 50 && W(o + ST_TICS) == 0 && D(o + ST_THINK) != 0 && D(o + ST_ACTION) == 0
            && W(o + ST_NEXT) == o) {
            g_stGuard = (Bit16u)o;
            break;
        }
    }
    g_stBj[0] = 0;
    for (size_t o = 0x100; o + 6 * ST_SIZE <= d.size(); o += 2) {
        if (W(o + ST_ROTATE) == 0 && W(o + ST_SHAPE) == 408 && W(o + ST_TICS) == 12 && D(o + ST_THINK) != 0 && W(o + ST_NEXT) == o + 16
            && W(o + 16 + ST_SHAPE) == 408 && W(o + 16 + ST_TICS) == 3 && D(o + 16 + ST_THINK) == 0 && W(o + 16 + ST_NEXT) == o + 32
            && W(o + 32 + ST_SHAPE) == 409 && W(o + 48 + ST_SHAPE) == 410 && W(o + 80 + ST_SHAPE) == 411) {
            g_stBj[0] = (Bit16u)o;
            g_stBj[1] = (Bit16u)(o + 32);
            g_stBj[2] = (Bit16u)(o + 48);
            g_stBj[3] = (Bit16u)(o + 80);
            break;
        }
    }
    // what the states are, for "the same game?": every state from the guard's first up to B.J.'s (their sprites,
    // times and order; not the functions' addresses, which are each build's own)
    Bit32u h = 2166136261u;
    if (g_stGuard && g_stBj[0] > g_stGuard) {
        for (size_t o = g_stGuard; o < g_stBj[0]; o += 2) {
            const Bit16u next = W(o + ST_NEXT);
            const Bit16u v[4] = { W(o + ST_ROTATE), W(o + ST_SHAPE), W(o + ST_TICS), (Bit16u)(next ? next - g_stGuard : 0xFFFF) };
            for (int k = 0; k < 4; k++) h = (h ^ v[k]) * 16777619u;
        }
    }
    #undef W
    #undef D
    char b[48];
    snprintf(b, sizeof(b), "states %08x+%u", h, (unsigned)(g_stBj[0] - g_stGuard));
    g_wbuild = b;
    return g_stGuard && g_stBj[0];
}

bool seek() {
    g_seeks++;
    const PhysPt base = (PhysPt)g_load << 4;
    static std::vector<Bit8u> buf;
    buf.resize(kCodeSpan);
    MEM_BlockRead(base, &buf[0], kCodeSpan);
    for (int a = 0; a < A_COUNT; a++) {
        const long at = find_anchor(buf, kAnchors[a]);
        if (at < 0) return false;
        g_anchor[a] = base + (Bit32u)at;
    }
    for (int i = 0; i < I_COUNT; i++) {
        const Item &im = kItems[i];
        const PhysPt p = g_anchor[im.anchor] + (Bit32u)im.at;
        const Bit16u v = mem_readw(p);
        g_item[i] = im.kind == K_REL ? (Bit32u)((long)p + 2 + (Bit16s)v) : v;
    }
    bool ok = true;
    for (int p = 0; p < P_COUNT; p++) {
        ok = ok && kPlaces[p].seg >= 0 && code_of(g_anchor[kPlaces[p].anchor] + (Bit32u)kPlaces[p].at, it(kPlaces[p].seg), &g_place[p]);
    }
    ok = ok && code_of(g_item[I_DOACTOR], it(I_SEG_PLAY), &g_doActor) && code_of(g_item[I_GETNEWACTOR], it(I_SEG_PLAY), &g_getNewActor)
         && code_of(g_item[I_KILLACTOR], it(I_SEG_STATE), &g_killActor);
    g_drawKeys.seg = it(I_SEG_AGENT);
    g_drawKeys.ip = it(I_DRAWKEYS_OFF);
    // the far calls' own addresses agree with the places found
    ok = ok && g_place[P_DAMAGE].ip == it(I_DAMAGE_OFF);
    if (!ok) {
        wclog(0, "Wolfenstein 3-D: WOLF3D.EXE's code is there but its places do not agree: not watched");
        return false;
    }
    // the data segment: the program's first instruction is Borland's "mov dx, DGROUP"
    if (mem_readb(base) != 0xBA) return false;
    g_ds = mem_readw(base + 1);
    if (!find_states()) {
        wclog(0, "Wolfenstein 3-D: WOLF3D.EXE's actor states are not where they are looked for: not watched");
        return false;
    }
    g_full = DOS_FileExists("VSWAP.WL6");
    return true;
}

// --- calls into the program -----------------------------------------------------------------------------------------
// As the TIE Fighter demo's (wcnet_tiedemo.cpp): at the frame's place the CPU's state is kept, the arguments and the
// place's own address pushed (the game's functions are far: Borland's medium model), CS:IP put at the function; the
// place's hook, reached again by its return, gives the state back and makes the next call or lets the frame go on.

enum CallKind { CALL_HURT, CALL_KILL, CALL_PUSH, CALL_KEYS, CALL_AVATAR };
struct Call {
    CallKind kind;
    int a, b, c, d;
    Bit32u x, y;
    bool quiet;
};
std::deque<Call> g_calls;
bool g_inCall = false;
Call g_cur;
Bit16u g_retSp = 0;
Bit32u g_keptScore = 0, g_keptExtra = 0;
struct Saved {
    Bit32u eax, ebx, ecx, edx, esi, edi, ebp, esp, flags;
    Bit16u ds, es;
} g_saved;

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

Bit16u actor_ob(int i) { return i >= 0 && i < (int)g_actors.size() ? g_actors[(size_t)i] : 0; }

void set_place(Bit16u ob, Bit32u x, Bit32u y) {
    w32((Bit16u)(ob + OB_X), x);
    w32((Bit16u)(ob + OB_Y), y);
    w16((Bit16u)(ob + OB_TILEX), (Bit16u)(x >> 16));
    w16((Bit16u)(ob + OB_TILEY), (Bit16u)(y >> 16));
    const int area = area_of_tile((int)(x >> 16), (int)(y >> 16));
    if (area >= 0) w8((Bit16u)(ob + OB_AREA), (Bit8u)area);
}

// the tile's mark of this object taken off (as DoActor does before an actor thinks)
void unmark(Bit16u ob) {
    const int tx = r16((Bit16u)(ob + OB_TILEX)), ty = r16((Bit16u)(ob + OB_TILEY));
    if (on_map(tx, ty) && r16(actorat_at(tx, ty)) == ob) w16(actorat_at(tx, ty), 0);
}
// ... and put on (as DoActor does after): only on a free tile, or another object's (never over a door's or a wall's number)
void mark(Bit16u ob) {
    const int flags = r8((Bit16u)(ob + OB_FLAGS));
    if (flags & WOLF_NEVERMARK) return;
    const int tx = r16((Bit16u)(ob + OB_TILEX)), ty = r16((Bit16u)(ob + OB_TILEY));
    if (!on_map(tx, ty)) return;
    const Bit16u there = r16(actorat_at(tx, ty));
    if (there && ((flags & WOLF_NONMARK) || there < 256)) return;
    w16(actorat_at(tx, ty), ob);
}

void init_avatar(int slot, Bit16u ob) {
    Avatar &v = g_avatar[slot];
    v.ob = ob;
    const int s = slot_of(ob);
    if (s < 0) {
        v.ob = 0;
        return;
    }
    g_slotSkip[s] = true;
    const Bit16u me = player_ob();
    w16((Bit16u)(ob + OB_ACTIVE), 1);
    w16((Bit16u)(ob + OB_CLASS), CLASS_INERT);
    w16((Bit16u)(ob + OB_STATE), r16((Bit16u)(me + OB_STATE)));     // (the player's own: no sprite)
    w8((Bit16u)(ob + OB_FLAGS), WOLF_NEVERMARK);
    w32((Bit16u)(ob + OB_DISTANCE), 0x7FFFFFFF);
    w16((Bit16u)(ob + OB_DIR), 0);
    set_place(ob, r32((Bit16u)(me + OB_X)), r32((Bit16u)(me + OB_Y)));
}

bool start_next_call() {
    while (!g_calls.empty()) {
        g_cur = g_calls.front();
        g_calls.pop_front();
        restore_state();
        Bit16u args[3];
        int nargs = 0;
        Code fn = g_place[P_DAMAGE];
        const Bit16u ob = actor_ob(g_cur.a);
        switch (g_cur.kind) {
        case CALL_HURT:
            if (!ob || !(r8((Bit16u)(ob + OB_FLAGS)) & WOLF_SHOOTABLE)) continue;
            args[0] = ob;
            args[1] = (Bit16u)g_cur.b;
            nargs = 2;
            break;
        case CALL_KILL:
            if (!ob || !(r8((Bit16u)(ob + OB_FLAGS)) & WOLF_SHOOTABLE)) continue;
            unmark(ob);
            set_place(ob, g_cur.x, g_cur.y);
            g_slotSkip[slot_of(ob)] = false;
            if (g_cur.quiet) {
                g_keptScore = r32(gs(GS_SCORE));
                g_keptExtra = r32(gs(GS_NEXTEXTRA));
            }
            args[0] = ob;
            nargs = 1;
            fn = g_killActor;
            break;
        case CALL_PUSH:
            if (r16(it(I_PWALLSTATE))) {        // (one moves: this one after it)
                PushWait w = { g_cur.a, g_cur.b, g_cur.c, g_cur.d };
                g_pushWait.push_back(w);
                continue;
            }
            if (!on_map(g_cur.a, g_cur.b) || !r8(tilemap_at(g_cur.a, g_cur.b))) continue;
            args[0] = (Bit16u)g_cur.a;
            args[1] = (Bit16u)g_cur.b;
            args[2] = (Bit16u)g_cur.c;
            nargs = 3;
            fn = g_place[P_PUSHWALL];
            break;
        case CALL_KEYS:
            fn = g_drawKeys;
            break;
        case CALL_AVATAR:
            if (r16(it(I_OBJCOUNT)) >= kWolfMaxActors - 4 || !r16(it(I_OBJFREELIST))) continue;
            fn = g_getNewActor;
            break;
        }
        for (int i = nargs - 1; i >= 0; i--) CPU_Push16(args[i]);
        g_retSp = (Bit16u)reg_esp;
        CPU_Push16(g_place[P_FRAME].seg);
        CPU_Push16(g_place[P_FRAME].ip);
        SegSet16(cs, fn.seg);
        reg_eip = fn.ip;
        g_inCall = true;
        g_callsMade++;
        return true;
    }
    restore_state();
    g_inCall = false;
    return false;
}

void call_done() {
    switch (g_cur.kind) {
    case CALL_KILL:
        if (g_cur.quiet) {
            // no points for what fell before we came, and its fall over: the last of its states
            w32(gs(GS_SCORE), g_keptScore);
            w32(gs(GS_NEXTEXTRA), g_keptExtra);
            const Bit16u ob = actor_ob(g_cur.a);
            Bit16u st = r16((Bit16u)(ob + OB_STATE));
            for (int n = 0; n < 24 && st; n++) {
                const Bit16u next = r16((Bit16u)(st + ST_NEXT));
                if (!next || next == st) break;
                st = next;
            }
            if (st) {
                w16((Bit16u)(ob + OB_STATE), st);
                w16((Bit16u)(ob + OB_TICCOUNT), r16((Bit16u)(st + ST_TICS)));
            }
        }
        break;
    case CALL_PUSH:
        if (!r16(it(I_PWALLSTATE))) {
            // the game would not push it here (something in its way on this machine): put where it ends
            wolf_push_done(g_cur.a, g_cur.b, g_cur.c, g_cur.d);
        } else {
            g_remotePush.on = true;
            g_remotePush.x = g_cur.a;
            g_remotePush.y = g_cur.b;
            g_remotePush.dir = g_cur.c;
            g_remotePush.area = g_cur.d;
        }
        break;
    case CALL_AVATAR:
        init_avatar(g_cur.a, r16(it(I_NEWOBJ)));
        break;
    default:
        break;
    }
}

Bit16u arg(int k) { return mem_readw(((PhysPt)SegValue(ss) << 4) + (Bit16u)((reg_esp & 0xFFFF) + 4 + 2 * k)); }

void skip_call() {
    const Bit16u ip = CPU_Pop16();
    const Bit16u seg = CPU_Pop16();
    SegSet16(cs, seg);
    reg_eip = ip;
}

// --- the level --------------------------------------------------------------------------------------------------------

void build_table() {
    g_actors.clear();
    for (int s = 0; s < kWolfMaxActors; s++) {
        g_slotIndex[s] = -1;
        g_slotSkip[s] = false;
    }
    for (int k = 0; k < kAvatars; k++) {
        g_avatar[k] = Avatar();
        g_markers[k].on = false;
    }
    g_calls.clear();
    g_pushWait.clear();
    g_remotePush.on = false;
    Bit32u h = 2166136261u;
    Bit16u ob = r16((Bit16u)(player_ob() + OB_NEXT));
    for (int n = 0; ob && n < kWolfMaxActors; n++) {
        const int s = slot_of(ob);
        if (s < 0) break;
        g_slotIndex[s] = (Bit16s)g_actors.size();
        g_actors.push_back(ob);
        const Bit16u v[3] = { r16((Bit16u)(ob + OB_CLASS)), r16((Bit16u)(ob + OB_TILEX)), r16((Bit16u)(ob + OB_TILEY)) };
        for (int k = 0; k < 3; k++) h = (h ^ v[k]) * 16777619u;
        ob = r16((Bit16u)(ob + OB_NEXT));
    }
    // (the game never stops a pushed wall at a level's set-up: one moving when its player died moved on in the new level)
    w16(it(I_PWALLSTATE), 0);
    g_levelSig = h ? h : 1;
    g_tableValid = true;
    g_levelFrames = 0;
    g_levels++;
    const WolfLevel l = wolf_level();
    wclog(1, "Wolfenstein 3-D: episode %d floor %d, difficulty %d: %u actors (%08x)", l.episode + 1, l.map + 1, l.difficulty,
          (unsigned)g_actors.size(), g_levelSig);
}

void update_markers() {
    const int vw = r16(it(I_VIEWWIDTH)), vh = r16(it(I_VIEWHEIGHT));
    const int left = (320 - vw) / 2, top = (200 - 40 - vh) / 2;
    for (int k = 0; k < kAvatars; k++) {
        WolfMarker &m = g_markers[k];
        const Avatar &v = g_avatar[k];
        m.on = false;
        if (!v.ob || !v.shown || !v.label[0] || vw <= 0 || vh <= 0) continue;
        const int flags = r8((Bit16u)(v.ob + OB_FLAGS));
        const int height = r16((Bit16u)(v.ob + OB_VIEWHEIGHT));
        if (!(flags & WOLF_VISABLE) || !height) continue;
        const int viewx = (Bit16s)r16((Bit16u)(v.ob + OB_VIEWX));
        if (viewx < 0 || viewx >= vw) continue;
        // (a sprite's cell is height / 4 pixels tall and wide, centred on the view's middle line; B.J. fills its middle)
        const double cell = std::min((double)height / 4.0, (double)vh);
        m.on = true;
        m.onScreen = true;
        m.x = (left + viewx) / 320.0;
        m.y = (top + vh / 2.0) / 200.0;
        m.size = cell * 0.85 / 320.0;
        snprintf(m.label, sizeof(m.label), "%s", v.label);
    }
}

void notice_tick(double now) {
    if (g_notice.empty()) return;
    if (now < g_noticeUntil && incoming_text == g_notice) return;
    if (incoming_text == g_notice) {
        incoming_text.clear();
        GFX_RedrawOverlay();
    }
    g_notice.clear();
}

// a remote pushed wall: the tiles it has left take the pusher's area (the game gives them our player's)
void remote_push_step() {
    if (!g_remotePush.on) return;
    static const int dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    const int d = g_remotePush.dir & 3;
    for (int k = 0; k < 2; k++) {
        const int tx = g_remotePush.x + k * dx[d], ty = g_remotePush.y + k * dy[d];
        if (on_map(tx, ty) && !r8(tilemap_at(tx, ty)) && g_remotePush.area >= 0) {
            mem_writew(plane_at(0, tx, ty), (Bit16u)(g_remotePush.area + kWolfAreaTile));
        }
    }
    if (!r16(it(I_PWALLSTATE))) g_remotePush.on = false;
}

void at_frame() {
    if (g_inCall) {
        if ((Bit16u)reg_esp != g_retSp) {
            wclog(1, "Wolfenstein 3-D: a call came back to the frame with SP %04X, not %04X: the calls given up", (unsigned)(reg_esp & 0xFFFF), g_retSp);
            g_calls.clear();
            restore_state();
            g_inCall = false;
            return;
        }
        call_done();
        start_next_call();          // (the next, or the frame goes on)
        return;
    }
    g_released = false;
    if (r16(it(I_DEMOPLAYBACK))) {
        g_levelSetup = false;       // (the title's demos: nothing of ours)
        g_tableValid = false;
        return;
    }
    const double now = PIC_FullIndex();
    const bool fresh = g_levelSetup || !g_tableValid;
    g_levelSetup = false;
    if (fresh) build_table();
    if (!g_inPlay) {
        g_inPlay = true;
        if (!g_flightSaid || fresh) {
            g_flightSaid = true;
            hooks_flight_began();
        }
    }
    g_lastFrameMs = now;
    g_frames++;
    g_levelFrames++;
    if (g_god) w16(gs(GS_HEALTH), 100);
    remote_push_step();
    while (!g_pushWait.empty() && !r16(it(I_PWALLSTATE)) && !g_remotePush.on) {
        const PushWait w = g_pushWait.front();
        g_pushWait.pop_front();
        wolf_push(w.x, w.y, w.dir, w.area);
        break;
    }
    update_markers();
    wolf_session_frame(fresh);
    if (!g_calls.empty()) {
        save_state();
        start_next_call();
    }
}

void hold_at_stub() {
    const PhysPt stub = ((PhysPt)g_psp << 4) + kStubOff;
    mem_writeb(stub, 0xEB);         // jmp $
    mem_writeb(stub + 1, 0xFE);
    SegSet16(cs, g_psp);
    reg_eip = kStubOff;
    g_holding = true;
    g_escDown = false;
}

void at_setup_level() {
    if (g_released) {
        g_released = false;         // (back from the hold: the level is set up)
        return;
    }
    g_levelSetup = true;
    g_inPlay = false;
    if (r16(it(I_DEMOPLAYBACK))) return;
    if (wolf_session_level_hold(false)) {
        wclog(1, "Wolfenstein 3-D: the level held for the room's");
        hold_at_stub();
    }
}

void at_stub() {
    if (!wolf_session_level_hold(g_escDown)) {
        g_holding = false;
        g_released = true;
        SegSet16(cs, g_place[P_SETUPLEVEL].seg);
        reg_eip = g_place[P_SETUPLEVEL].ip;
        return;
    }
    CPU_IODelayRemoved += CPU_Cycles;      // (idle, as HLT is, until the room's level is known)
    CPU_Cycles = 0;
}

void at_do_actor() {
    const int s = slot_of(arg(0));
    if (s >= 0 && g_slotSkip[s]) {
        g_doActorSkips++;
        skip_call();
    }
}

void at_damage() {
    if (g_inCall) return;           // (our own call)
    const int s = slot_of(arg(0));
    if (s < 0 || g_slotIndex[s] < 0) return;
    if (wolf_session_damage(g_slotIndex[s], (int)arg(1))) {
        g_damageTold++;
        skip_call();
    }
}

void ended() {
    if (g_stage == STAGE_NONE) return;
    wclog(1, "Wolfenstein 3-D: WOLF3D.EXE ended (%u frames, %u levels, %u calls made)", g_frames, g_levels, g_callsMade);
    g_stage = STAGE_NONE;
    g_calls.clear();
    g_inCall = g_holding = g_inPlay = g_tableValid = false;
    for (int k = 0; k < kAvatars; k++) g_markers[k].on = false;
    if (!g_notice.empty() && incoming_text == g_notice) incoming_text.clear();
    g_notice.clear();
    wolf_session_idle();
    hooks_game_changed();
}

}  // namespace

// --- the game, for the session ------------------------------------------------------------------------------------

WolfLevel wolf_level() {
    WolfLevel l;
    l.episode = (Bit16s)r16(gs(GS_EPISODE));
    l.map = (Bit16s)r16(gs(GS_MAPON));
    l.difficulty = (Bit16s)r16(gs(GS_DIFFICULTY));
    return l;
}

void wolf_set_level(const WolfLevel &l) {
    w16(gs(GS_EPISODE), (Bit16u)l.episode);
    w16(gs(GS_MAPON), (Bit16u)l.map);
    w16(gs(GS_DIFFICULTY), (Bit16u)l.difficulty);
}

bool wolf_has_level(const WolfLevel &l) {
    return l.map >= 0 && l.map <= 9 && l.episode >= 0 && l.episode <= (g_full ? 5 : 0) && l.difficulty >= 0 && l.difficulty <= 3;
}

void wolf_reload() { w16(it(I_PLAYSTATE), 3); }
void wolf_end_level(int playstate) { w16(it(I_PLAYSTATE), (Bit16u)playstate); }
std::string wolf_build() { return g_wbuild; }

WolfBody wolf_player() {
    WolfBody b;
    const Bit16u ob = player_ob();
    b.x = r32((Bit16u)(ob + OB_X));
    b.y = r32((Bit16u)(ob + OB_Y));
    b.angle = (Bit16s)r16((Bit16u)(ob + OB_ANGLE));
    b.health = (Bit16s)r16(gs(GS_HEALTH));
    b.attack = r16(gs(GS_ATTACKFRAME));
    return b;
}

int wolf_actors() { return (int)g_actors.size(); }
Bit32u wolf_level_sig() { return g_levelSig; }

bool wolf_actor(int i, WolfAct *out) {
    const Bit16u ob = actor_ob(i);
    if (!ob) return false;
    out->cls = r16((Bit16u)(ob + OB_CLASS));
    out->x = r32((Bit16u)(ob + OB_X));
    out->y = r32((Bit16u)(ob + OB_Y));
    out->tilex = r16((Bit16u)(ob + OB_TILEX));
    out->tiley = r16((Bit16u)(ob + OB_TILEY));
    out->dir = r16((Bit16u)(ob + OB_DIR));
    out->state = (int)r16((Bit16u)(ob + OB_STATE)) - (int)g_stGuard;
    out->ticcount = (Bit16s)r16((Bit16u)(ob + OB_TICCOUNT));
    out->flags = r8((Bit16u)(ob + OB_FLAGS));
    out->hp = (Bit16s)r16((Bit16u)(ob + OB_HP));
    out->distance = (Bit32s)r32((Bit16u)(ob + OB_DISTANCE));
    return true;
}

void wolf_actor_show(int i, const WolfAct &a, bool taken) {
    const Bit16u ob = actor_ob(i);
    if (!ob || !on_map(a.tilex, a.tiley)) return;
    const int st = a.state + (int)g_stGuard;
    if (st <= 0 || st > 0xFFF0) return;
    unmark(ob);
    w32((Bit16u)(ob + OB_X), a.x);
    w32((Bit16u)(ob + OB_Y), a.y);
    w16((Bit16u)(ob + OB_TILEX), (Bit16u)a.tilex);
    w16((Bit16u)(ob + OB_TILEY), (Bit16u)a.tiley);
    const int area = area_of_tile(a.tilex, a.tiley);
    if (area >= 0) w8((Bit16u)(ob + OB_AREA), (Bit8u)area);
    w16((Bit16u)(ob + OB_DIR), (Bit16u)a.dir);
    w16((Bit16u)(ob + OB_STATE), (Bit16u)st);
    // (whether it is drawn is this machine's own)
    w8((Bit16u)(ob + OB_FLAGS), (Bit8u)((a.flags & ~WOLF_VISABLE) | (r8((Bit16u)(ob + OB_FLAGS)) & WOLF_VISABLE)));
    w16((Bit16u)(ob + OB_HP), (Bit16u)a.hp);
    w16((Bit16u)(ob + OB_ACTIVE), 1);
    if (taken) {
        w16((Bit16u)(ob + OB_TICCOUNT), (Bit16u)a.ticcount);
        w32((Bit16u)(ob + OB_DISTANCE), (Bit32u)a.distance);
    }
    mark(ob);
}

void wolf_actor_place(int i, const WolfAct &a) {
    const Bit16u ob = actor_ob(i);
    if (!ob || !on_map(a.tilex, a.tiley)) return;
    const int st = a.state + (int)g_stGuard;
    if (st <= 0 || st > 0xFFF0) return;
    unmark(ob);
    w32((Bit16u)(ob + OB_X), a.x);
    w32((Bit16u)(ob + OB_Y), a.y);
    w16((Bit16u)(ob + OB_TILEX), (Bit16u)a.tilex);
    w16((Bit16u)(ob + OB_TILEY), (Bit16u)a.tiley);
    const int area = area_of_tile(a.tilex, a.tiley);
    if (area >= 0) w8((Bit16u)(ob + OB_AREA), (Bit8u)area);
    w16((Bit16u)(ob + OB_DIR), (Bit16u)a.dir);
    w16((Bit16u)(ob + OB_STATE), (Bit16u)st);
    w16((Bit16u)(ob + OB_TICCOUNT), (Bit16u)a.ticcount);
    w32((Bit16u)(ob + OB_DISTANCE), (Bit32u)a.distance);
    mark(ob);
}

void wolf_actor_puppet(int i, bool on) {
    const int s = slot_of(actor_ob(i));
    if (s >= 0) g_slotSkip[s] = on;
}

bool wolf_actor_is_puppet(int i) {
    const int s = slot_of(actor_ob(i));
    return s >= 0 && g_slotSkip[s];
}

// (classtype: 3 guard, 4 officer, 5 SS, 6 dog, 7..11 the bosses and the mutant, 16..19 Hitler, Gretel, Giftmacher, Fat
// Face; not 2 a corpse or a thing of ours, 12 13 20 shots, 14 B.J., 15 a ghost)
bool wolf_actor_fighter(int cls) { return (cls >= 3 && cls <= 11) || (cls >= 16 && cls <= 19); }

void wolf_hurt(int i, int damage) {
    Call c = { CALL_HURT, i, damage, 0, 0, 0, 0, false };
    if (g_calls.size() < 512) g_calls.push_back(c);
}

void wolf_kill(int i, Bit32u x, Bit32u y, bool quiet) {
    Call c = { CALL_KILL, i, 0, 0, 0, x, y, quiet };
    if (g_calls.size() < 512) g_calls.push_back(c);
}

int wolf_doors() { return std::min((int)r16(it(I_DOORNUM)), (int)kWolfMaxDoors); }
int wolf_door_action(int d) { return r16((Bit16u)(it(I_DOORACTION) + DOOR_SIZE * d)); }

void wolf_door_open(int d) {
    if (d < 0 || d >= wolf_doors()) return;
    const Bit16u a = (Bit16u)(it(I_DOORACTION) + DOOR_SIZE * d);
    if (r16(a) == 0) w16((Bit16u)(a + DOOR_TICCOUNT), 0);
    else w16(a, 2);
}

void wolf_door_keep(int d) {
    if (d < 0 || d >= wolf_doors()) return;
    const Bit16u a = (Bit16u)(it(I_DOORACTION) + DOOR_SIZE * d);
    if (r16(a) == 0) w16((Bit16u)(a + DOOR_TICCOUNT), 0);
    else if (r16(a) == 3) w16(a, 2);
}

bool wolf_door_at(int tilex, int tiley, int *d) {
    const int n = wolf_doors();
    for (int k = 0; k < n; k++) {
        const Bit16u a = (Bit16u)(it(I_DOORACTION) + DOOR_SIZE * k);
        if (r8((Bit16u)(a + DOOR_TILEX)) == tilex && r8((Bit16u)(a + DOOR_TILEY)) == tiley) {
            *d = k;
            return true;
        }
    }
    return false;
}

bool wolf_push_moving(int *x, int *y, int *dir) {
    if (!r16(it(I_PWALLSTATE))) return false;
    *x = r16(it(I_PWALLX));
    *y = r16(it(I_PWALLY));
    *dir = r16(it(I_PWALLDIR));
    return true;
}

void wolf_push(int x, int y, int dir, int area) {
    Call c = { CALL_PUSH, x, y, dir, area, 0, 0, false };
    if (g_calls.size() < 512) g_calls.push_back(c);
}

void wolf_push_done(int x, int y, int dir, int area) {
    static const int dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    const int d = dir & 3;
    if (!on_map(x, y)) return;
    const int tile = r8(tilemap_at(x, y));
    if (!tile || (tile & 0x80)) return;     // (no wall there: moved already; or a door)
    // two tiles on, one when the second is taken
    int steps = 0;
    for (int k = 1; k <= 2; k++) {
        const int tx = x + k * dx[d], ty = y + k * dy[d];
        if (!on_map(tx, ty) || r16(actorat_at(tx, ty))) break;
        steps = k;
    }
    if (!steps) return;
    w8(tilemap_at(x, y), 0);
    w16(actorat_at(x, y), 0);
    const int ex = x + steps * dx[d], ey = y + steps * dy[d];
    w8(tilemap_at(ex, ey), (Bit8u)tile);
    w16(actorat_at(ex, ey), (Bit16u)tile);
    mem_writew(plane_at(1, x, y), 0);       // (its mark on the info plane: pushed once)
    if (area >= 0) {
        for (int k = 0; k < steps; k++) mem_writew(plane_at(0, x + k * dx[d], y + k * dy[d]), (Bit16u)(area + kWolfAreaTile));
    }
    w16(gs(GS_SECRETCOUNT), (Bit16u)(r16(gs(GS_SECRETCOUNT)) + 1));
}

int wolf_player_area() { return r8((Bit16u)(player_ob() + OB_AREA)); }

int wolf_keys() { return r16(gs(GS_KEYS)); }

void wolf_give_keys(int bits) {
    const int have = r16(gs(GS_KEYS));
    if ((have | bits) == have) return;
    w16(gs(GS_KEYS), (Bit16u)(have | bits));
    Call c = { CALL_KEYS, 0, 0, 0, 0, 0, 0, false };
    g_calls.push_back(c);
}

void wolf_avatar(int slot, bool shown, Bit32u x, Bit32u y, bool walking, const char *label) {
    if (slot < 0 || slot >= kAvatars || !g_tableValid) return;
    Avatar &v = g_avatar[slot];
    snprintf(v.label, sizeof(v.label), "%s", label ? label : "");
    if (!v.ob) {
        if (shown && !v.asked) {
            v.asked = true;
            Call c = { CALL_AVATAR, slot, 0, 0, 0, 0, 0, false };
            g_calls.push_back(c);
        }
        return;
    }
    v.shown = shown;
    if (!shown) {
        w16((Bit16u)(v.ob + OB_STATE), r16((Bit16u)(player_ob() + OB_STATE)));
        return;
    }
    const int frame = walking ? (int)(PIC_FullIndex() / 110.0) & 3 : 0;
    w16((Bit16u)(v.ob + OB_STATE), g_stBj[frame]);
    w16((Bit16u)(v.ob + OB_ACTIVE), 1);
    set_place(v.ob, x, y);
}

void wolf_notice(const std::string &text, double ms) {
    if (!g_notice.empty() && incoming_text == g_notice) incoming_text.clear();
    g_notice = text;
    g_noticeUntil = PIC_FullIndex() + ms;
    incoming_text = text;
    GFX_RedrawOverlay();
    wclog(1, "Wolfenstein 3-D: \"%s\"", text.c_str());
}

const WolfMarker *wolf_markers() { return g_markers; }

// --- the dispatcher's --------------------------------------------------------------------------------------------

bool wolf_watching() { return g_stage == STAGE_SEEKING || g_stage == STAGE_FOUND; }
bool wolf_found() { return g_stage == STAGE_FOUND; }
bool wolf_in_play() { return ours_running() && g_inPlay && PIC_FullIndex() - g_lastFrameMs < 700; }

void wolf_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    if (g_stage != STAGE_NONE && psp != g_psp) ended();
    if (strcmp(name, "WOLF3D.EXE") != 0) return;
    g_stage = STAGE_SEEKING;
    g_load = loadSeg;
    g_psp = psp;
    g_seekSince = PIC_FullIndex();
    g_nextSeekMs = g_seekSince + 200;
    g_seeks = 0;
    g_calls.clear();
    g_inCall = g_released = g_holding = g_levelSetup = g_tableValid = g_inPlay = g_flightSaid = false;
    g_frames = g_levels = 0;
    wclog(1, "Wolfenstein 3-D: WOLF3D.EXE loaded at %04X (PSP %04X): looked for once it has unpacked itself", loadSeg, psp);
}

void wolf_tick() {
    if (g_stage == STAGE_NONE || g_stage == STAGE_UNKNOWN) return;
    const double now = PIC_FullIndex();
    if (dos.psp() != g_psp) {
        // (the shell's again: the program is gone; a DOS call of its own keeps its PSP)
        static double since = -1;
        if (since < 0) since = now;
        if (now - since > 300) {
            since = -1;
            ended();
        }
        return;
    }
    if (g_stage == STAGE_SEEKING) {
        if (now < g_nextSeekMs) return;
        g_nextSeekMs = now + kSeekEveryMs;
        if (seek()) {
            g_stage = STAGE_FOUND;
            hooks_game_changed();       // (the dispatcher's table: our places)
            wclog(1, "Wolfenstein 3-D: found after %.1f s (%u looks): data segment %04X, the frame at %04X:%04X, DoActor %04X:%04X, "
                     "DamageActor %04X:%04X, %s; %s", (now - g_seekSince) / 1000.0, g_seeks, g_ds, g_place[P_FRAME].seg, g_place[P_FRAME].ip,
                  g_doActor.seg, g_doActor.ip, g_place[P_DAMAGE].seg, g_place[P_DAMAGE].ip, g_full ? "the six episodes" : "the shareware episode",
                  g_wbuild.c_str());
        } else if (now - g_seekSince > kSeekForMs) {
            g_stage = STAGE_UNKNOWN;
            wclog(0, "Wolfenstein 3-D: this WOLF3D.EXE is not a build the hooks know (version 1.4's code was looked for): played as it is");
        }
        return;
    }
    wolf_session_tick();
    notice_tick(now);
    if (g_inPlay && now - g_lastFrameMs > 700) {
        g_inPlay = false;
        for (int k = 0; k < kAvatars; k++) g_markers[k].on = false;
        wolf_session_idle();
    }
}

void wolf_watch_list(void (*watch)(Bit16u ip)) {
    if (g_stage != STAGE_FOUND) return;
    for (int p = 0; p < P_COUNT; p++) {
        if (p != P_PUSHWALL) watch(g_place[p].ip);
    }
    watch(g_doActor.ip);
    watch(kStubOff);
}

bool wolf_check_hooks(Bit32u ip) {
    if (g_stage != STAGE_FOUND) return false;
    const Bit16u seg = SegValue(cs);
    if (ip == g_doActor.ip && seg == g_doActor.seg) {
        at_do_actor();
        return true;
    }
    if (ip == g_place[P_FRAME].ip && seg == g_place[P_FRAME].seg) {
        at_frame();
        return true;
    }
    if (ip == g_place[P_DAMAGE].ip && seg == g_place[P_DAMAGE].seg) {
        at_damage();
        return true;
    }
    if (ip == g_place[P_LOOP_EXIT].ip && seg == g_place[P_LOOP_EXIT].seg) {
        g_inPlay = false;
        for (int k = 0; k < kAvatars; k++) g_markers[k].on = false;
        if (!r16(it(I_DEMOPLAYBACK))) wolf_session_loop_exit(r16(it(I_PLAYSTATE)));
        return true;
    }
    if (ip == g_place[P_SETUPLEVEL].ip && seg == g_place[P_SETUPLEVEL].seg) {
        at_setup_level();
        return true;
    }
    if (ip == kStubOff && seg == g_psp) {
        if (g_holding) at_stub();
        return true;
    }
    return false;
}

bool wolf_key(int key, bool pressed) {
    if (g_stage != STAGE_FOUND || !g_holding || key != KBD_esc) return false;
    if (pressed) g_escDown = true;
    return true;
}

bool wolf_script_item(const std::string &item, double t) {
    if (item.compare(0, 5, "!wolf") != 0) return false;
    if (g_stage != STAGE_FOUND) {
        wclog(1, "key script %.1fs: %s: WOLF3D.EXE is not found", t, item.c_str());
        return true;
    }
    if (item.compare(0, 9, "!wolfgod=") == 0) {
        g_god = item[9] != '0';
        wclog(1, "key script %.1fs: Wolfenstein 3-D: health %s", t, g_god ? "kept at 100" : "the game's own again");
    } else if (item.compare(0, 8, "!wolfhp=") == 0) {
        w16(gs(GS_HEALTH), (Bit16u)atoi(item.c_str() + 8));
        wclog(1, "key script %.1fs: Wolfenstein 3-D: health set to %d", t, atoi(item.c_str() + 8));
    } else if (item.compare(0, 10, "!wolfhurt=") == 0) {
        // !wolfhurt=<n>:<damage>: actor n hurt as by our shot (told to its owner when it is thought for elsewhere)
        int i = 0, dmg = 0;
        if (sscanf(item.c_str() + 10, "%d:%d", &i, &dmg) == 2) {
            const bool told = wolf_session_damage(i, dmg);
            if (!told) wolf_hurt(i, dmg);
            wclog(1, "key script %.1fs: Wolfenstein 3-D: actor %d hurt by %d (%s)", t, i, dmg, told ? "told to its owner" : "in our game");
        }
    } else if (item.compare(0, 10, "!wolfkeys=") == 0) {
        w16(gs(GS_KEYS), (Bit16u)(r16(gs(GS_KEYS)) | atoi(item.c_str() + 10)));
        wclog(1, "key script %.1fs: Wolfenstein 3-D: keys found (%d)", t, atoi(item.c_str() + 10));
    } else if (item == "!wolfdoors") {
        std::string open;
        for (int d = 0; d < wolf_doors(); d++) {
            if (wolf_door_action(d) == 1) continue;
            char buf[32];
            snprintf(buf, sizeof(buf), " %d:%s", d, wolf_door_action(d) == 0 ? "open" : wolf_door_action(d) == 2 ? "opening" : "closing");
            open += buf;
        }
        int x = 0, y = 0, dir = 0;
        const bool moving = wolf_push_moving(&x, &y, &dir);
        wclog(1, "key script %.1fs: Wolfenstein 3-D: %d doors, not closed:%s; %s; secrets %d", t, wolf_doors(), open.empty() ? " none" : open.c_str(),
              moving ? "a wall is moving" : "no wall moving", (int)r16(gs(GS_SECRETCOUNT)));
    } else if (item == "!wolfpos") {
        const WolfBody b = wolf_player();
        const WolfLevel l = wolf_level();
        int alive = 0, attacking = 0;
        WolfAct a;
        for (int i = 0; i < wolf_actors(); i++) {
            if (!wolf_actor(i, &a) || !wolf_actor_fighter(a.cls) || !(a.flags & WOLF_SHOOTABLE)) continue;
            alive++;
            attacking += (a.flags & WOLF_ATTACKMODE) ? 1 : 0;
        }
        wclog(1, "key script %.1fs: Wolfenstein 3-D: E%dL%d at %.2f %.2f facing %d, health %d, keys %d, score %u; %d of the level's fighters "
                 "stand (%d after somebody), %u frames, %u DoActor passed by, %u hits told, %u calls", t, l.episode + 1, l.map + 1,
              b.x / 65536.0, b.y / 65536.0, b.angle, b.health, wolf_keys(), r32(gs(GS_SCORE)), alive, attacking, g_frames, g_doActorSkips,
              g_damageTold, g_callsMade);
    } else if (item.compare(0, 10, "!wolfwarp=") == 0) {
        // !wolfwarp=<x>:<y>:<angle>: our player put there (tiles, degrees), its area the tile's
        double x = 0, y = 0;
        int angle = 0;
        if (sscanf(item.c_str() + 10, "%lf:%lf:%d", &x, &y, &angle) >= 2 && g_tableValid) {
            const Bit16u ob = player_ob();
            set_place(ob, (Bit32u)(x * 65536.0), (Bit32u)(y * 65536.0));
            w16((Bit16u)(ob + OB_ANGLE), (Bit16u)(((angle % 360) + 360) % 360));
            const int area = area_of_tile((int)x, (int)y);
            if (area >= 0) w16((Bit16u)(it(I_AREABYPLAYER) + 2 * area), 1);
        }
        wclog(1, "key script %.1fs: Wolfenstein 3-D: our player put at %.2f %.2f facing %d", t, x, y, angle);
    } else if (item.compare(0, 9, "!wolfact=") == 0) {
        // !wolfact=<n>: actor n's record
        const int i = atoi(item.c_str() + 9);
        WolfAct a;
        if (wolf_actor(i, &a)) {
            wclog(1, "key script %.1fs: Wolfenstein 3-D: actor %d class %d at %.2f %.2f (tile %d %d) dir %d state %d flags %02x hp %d%s", t, i,
                  a.cls, a.x / 65536.0, a.y / 65536.0, a.tilex, a.tiley, a.dir, a.state, a.flags, a.hp, wolf_actor_is_puppet(i) ? " (shown)" : "");
            const std::string told = wolf_session_report(i);
            if (!told.empty()) wclog(1, "key script %.1fs: Wolfenstein 3-D: actor %d %s", t, i, told.c_str());
        }
    } else if (item == "!wolfnear") {
        // the fighters nearest our player
        const WolfBody b = wolf_player();
        std::vector<std::pair<double, int> > near;
        WolfAct a;
        for (int i = 0; i < wolf_actors(); i++) {
            if (!wolf_actor(i, &a) || !wolf_actor_fighter(a.cls)) continue;
            const double dx = ((double)a.x - b.x) / 65536.0, dy = ((double)a.y - b.y) / 65536.0;
            near.push_back(std::make_pair(dx * dx + dy * dy, i));
        }
        std::sort(near.begin(), near.end());
        std::string s;
        for (size_t k = 0; k < near.size() && k < 8; k++) {
            wolf_actor(near[k].second, &a);
            char buf[96];
            snprintf(buf, sizeof(buf), " #%d c%d %.1f,%.1f hp%d f%02x%s", near[k].second, a.cls, a.x / 65536.0, a.y / 65536.0, a.hp, a.flags,
                     wolf_actor_is_puppet(near[k].second) ? "P" : "");
            s += buf;
        }
        wclog(1, "key script %.1fs: Wolfenstein 3-D: nearest:%s", t, s.c_str());
    } else if (item == "!wolfstat") {
        wclog(1, "key script %.1fs: Wolfenstein 3-D: %s", t, wolf_session_status().c_str());
    } else {
        wclog(1, "key script %.1fs: %s: not an aid of Wolfenstein 3-D's", t, item.c_str());
    }
    return true;
}

}  // namespace wc
