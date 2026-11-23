/*
 *  Wing Commander Armada's arrow keys, ramped (wcnet_armada.h, docs/armada.md "The arrow keys").
 *
 *  Every flight frame the game builds its pilot's control packet, a few bytes it sends to the other machines and
 *  steers every pilot's ship by (seg2584:02A7 in ARMADA.EXE; its decoding at seg2584:1A11): byte 3 the stick's left
 *  and right, byte 4 its up and down, each a 7-bit signed number (bit 7 is a button), the joystick's reading in
 *  them.  Then a loop over the key bindings (3 bytes each: scan code, action, kind) looks at the game's own held-key
 *  table (DS:8E56 + scan code) and, for a held key bound to a stick action, writes the stop at once: action A (Down)
 *  byte 4 = +30, B (Up) -30, C (Right) byte 3 = +30, D (Left) -30; Proving Grounds 20.  So a tap of an arrow turns
 *  at the full rate for a frame or more and the nose jumps past what it was aimed at, as Privateer's did.
 *
 *  The fix is Wing Commander's, and Privateer's (wcnet_priv_controls.cpp): the four writes are watched (which way
 *  each axis was asked for this frame, the last write winning as in the game), and at the loop's end the packet's
 *  byte is written again, ramped: from a ninth of the stop to the stop in a third of a second (a ninth more every
 *  37 ms), Shift with it the stop at once, the opposite key or letting go nothing.  The packet is the one the game
 *  then sends, so every machine of a room flies the ramped turn: nothing differs between them.  Only the keys'
 *  writes are touched: a joystick's reading is the game's.  WCNET_ARM_RAMP=0 leaves the game's own.
 *
 *  Four builds are known (GOG's: the floppy and CD programs of each), each checked by its bytes at those places.
 */
#include <stdlib.h>
#include <string.h>
#include "wcnet_armada.h"
#include "wcnet_game.h"
#include "wcnet_log.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"

namespace wc {

namespace {

struct Build {
    const char *exe;
    const char *title;
    Bit16u seg;                 // the control packet's code, from the load segment
    Bit16u orA, orB, orC, orD;  // each `or byte ptr [bx + 4|3], V` of the actions A, B, C, D (BX: the packet)
    Bit16u loopEnd;             // the bindings loop's end: `cmp byte ptr [bp + d], 3`
    Bit8u loopEndDisp;
    Bit16u held;                // the held-key table in the data segment (1: held), by scan code
    Bit8u full;                 // a key's stop
};
const Build kBuilds[] = {
    { "ARMADA.EXE", "Armada", 0x2584, 0x14D0, 0x14EC, 0x1508, 0x1524, 0x1689, 0xEC, 0x8E56, 30 },
    { "ARMADACD.EXE", "Armada (CD-ROM)", 0x2587, 0x14D0, 0x14EC, 0x1508, 0x1524, 0x1689, 0xEC, 0x8ED6, 30 },
    { "PG.EXE", "Proving Grounds", 0x1B68, 0x0E2A, 0x0E46, 0x0E62, 0x0E7E, 0x0FE3, 0xFA, 0x7B1C, 20 },
    { "PGCD.EXE", "Proving Grounds (CD-ROM)", 0x1B6C, 0x0E2A, 0x0E46, 0x0E62, 0x0E7E, 0x0FE3, 0xFA, 0x7C2C, 20 },
};

const double kStepMs = 37.0;    // a ninth more of the stop this often: the stop in a third of a second
const double kStaleMs = 400.0;  // no packet made this long (out of flight): what was held is forgotten

enum { AXIS_X, AXIS_Y };        // the packet's byte 3 (left, right), byte 4 (up, down)

struct Axis {
    int want;                   // this frame's key: -1, 0, 1 (the game's sign)
    int dir;                    // the key held, as the ramp has it
    double since;               // when that began (emulated ms)
};

const Build *g_arm;
Bit16u g_load, g_psp;
Axis g_axis[2];
PhysPt g_packet;                // where this frame's keys wrote (0: none)
double g_lastEnd = -1e9;
int g_ramp = -1;

bool ramp_on() {
    if (g_ramp < 0) {
        const char *env = getenv("WCNET_ARM_RAMP");
        g_ramp = !(env && env[0] == '0');
        wclog(1, "Armada: the arrows %s", g_ramp ? "ramped (Wing Commander's way)" : "as the game has them (WCNET_ARM_RAMP=0)");
    }
    return g_ramp != 0;
}

PhysPt code_at(Bit16u off) {
    return ((PhysPt)(Bit16u)(g_load + g_arm->seg) << 4) + off;
}

bool bytes_at(PhysPt at, const Bit8u *want, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (mem_readb(at + (PhysPt)i) != want[i]) {
            return false;
        }
    }
    return true;
}

// The build's places hold what they held when they were read (docs/armada.md): the four writes and the loop's end.
bool build_here() {
    const Bit8u v = g_arm->full, n = (Bit8u)((0x80 - v) & 0x7F);
    const Bit8u a[] = { 0x80, 0x67, 0x04, 0x80, 0x80, 0x4F, 0x04, v };
    const Bit8u b[] = { 0x80, 0x67, 0x04, 0x80, 0x80, 0x4F, 0x04, n };
    const Bit8u c[] = { 0x80, 0x67, 0x03, 0x80, 0x80, 0x4F, 0x03, v };
    const Bit8u d[] = { 0x80, 0x67, 0x03, 0x80, 0x80, 0x4F, 0x03, n };
    const Bit8u e[] = { 0x80, 0x7E, g_arm->loopEndDisp, 0x03 };
    return bytes_at(code_at((Bit16u)(g_arm->orA - 4)), a, sizeof(a)) &&
           bytes_at(code_at((Bit16u)(g_arm->orB - 4)), b, sizeof(b)) &&
           bytes_at(code_at((Bit16u)(g_arm->orC - 4)), c, sizeof(c)) &&
           bytes_at(code_at((Bit16u)(g_arm->orD - 4)), d, sizeof(d)) &&
           bytes_at(code_at(g_arm->loopEnd), e, sizeof(e));
}

bool held(int scan) {
    return mem_readb(SegPhys(ds) + (Bit16u)(g_arm->held + scan)) == 1;
}

// A key's write (the instruction about to run is its `or`: BX the packet, in the data segment).
void key_wrote(int axis, int dir) {
    g_axis[axis].want = dir;
    g_packet = SegPhys(ds) + (Bit16u)reg_bx;
}

// The axis's value this frame (the game's sign; 0: no key, the packet left as it is).
int ramped(Axis &a, bool shift, double now) {
    const int dir = a.want;
    a.want = 0;
    if (dir != a.dir) {
        a.dir = dir;
        a.since = now;
    }
    if (!dir) {
        return 0;
    }
    int step = shift ? 9 : 1 + (int)((now - a.since) / kStepMs);
    if (step > 9) {
        step = 9;
    }
    return dir * (int)g_arm->full * step / 9;
}

// The loop's end: the keys' writes of this frame made again, ramped.
void loop_end() {
    const double now = PIC_FullIndex();
    if (now - g_lastEnd > kStaleMs) {
        g_axis[AXIS_X].dir = g_axis[AXIS_Y].dir = 0;
    }
    g_lastEnd = now;
    const bool shift = held(0x2A) || held(0x36);
    const int x = ramped(g_axis[AXIS_X], shift, now), y = ramped(g_axis[AXIS_Y], shift, now);
    if (g_packet && (x || y)) {
        const Bit8u gx = mem_readb(g_packet + 3), gy = mem_readb(g_packet + 4);
        if (x) {
            mem_writeb(g_packet + 3, (Bit8u)((gx & 0x80) | (x & 0x7F)));
        }
        if (y) {
            mem_writeb(g_packet + 4, (Bit8u)((gy & 0x80) | (y & 0x7F)));
        }
        wclog(3, "Armada: arrows %+d %+d (the game's %+d %+d)", x, y,
              (int)(Bit8s)(Bit8u)(gx << 1) >> 1, (int)(Bit8s)(Bit8u)(gy << 1) >> 1);
    }
    g_packet = 0;
}

}  // namespace

bool arm_found() { return g_arm != NULL; }

void arm_program_loaded(const char *name, Bit16u loadSeg, Bit16u psp) {
    for (size_t i = 0; i < sizeof(kBuilds) / sizeof(kBuilds[0]); i++) {
        if (strcmp(name, kBuilds[i].exe) != 0) {
            continue;
        }
        g_arm = &kBuilds[i];
        g_load = loadSeg;
        if (!build_here()) {
            wclog(0, "%s is not the %s build the hooks know (GOG's): its arrows as the game has them", name, kBuilds[i].title);
            g_arm = NULL;
            return;
        }
        g_psp = psp;
        g_packet = 0;
        g_lastEnd = -1e9;
        memset(g_axis, 0, sizeof(g_axis));
        hooks_game_changed();       // (the dispatcher's table: our places)
        wclog(1, "%s loaded at %04X (PSP %04X): the arrows %s", kBuilds[i].title, loadSeg, psp,
              ramp_on() ? "ramped" : "the game's own");
        return;
    }
}

void arm_program_ended(Bit16u psp) {
    if (g_arm && psp == g_psp) {
        wclog(1, "%s ended", g_arm->title);
        g_arm = NULL;
        hooks_game_changed();
    }
}

void arm_watch_list(void (*watch)(Bit16u ip)) {
    if (!g_arm || !ramp_on()) {
        return;
    }
    watch(g_arm->orA);
    watch(g_arm->orB);
    watch(g_arm->orC);
    watch(g_arm->orD);
    watch(g_arm->loopEnd);
}

bool arm_check_hooks(Bit32u ip) {
    if (!g_arm || SegValue(cs) != (Bit16u)(g_load + g_arm->seg)) {
        return false;
    }
    if (ip == g_arm->orA) {
        key_wrote(AXIS_Y, 1);       // Down
    } else if (ip == g_arm->orB) {
        key_wrote(AXIS_Y, -1);      // Up
    } else if (ip == g_arm->orC) {
        key_wrote(AXIS_X, 1);       // Right
    } else if (ip == g_arm->orD) {
        key_wrote(AXIS_X, -1);      // Left
    } else if (ip == g_arm->loopEnd) {
        loop_end();
    } else {
        return false;
    }
    return true;
}

}  // namespace wc
