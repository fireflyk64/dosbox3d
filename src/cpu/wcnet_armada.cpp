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
 *  The page's right-button drag steers the same way (wc_web_arm_steer): at the loop's end, on an axis no key and no
 *  stick moves, the packet's byte is the drag's share of the keys' stop (Armada has no mouse flight of its own).
 *
 *  Four builds are known (GOG's: the floppy and CD programs of each), each checked by its bytes at those places.
 */
#include <stdlib.h>
#include <string.h>
#include <string>
#include "wcnet_armada.h"
#include "wcnet_armada_hires.h"
#include "wcnet_game.h"
#include "wcnet_log.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"
#include "vga.h"
#include "cpu.h"
#include "paging.h"
#include "joystick.h"
#include <math.h>
#include <stdio.h>
#include <map>
#include <vector>
#include <algorithm>

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
    Bit16u joySeg, joyEntry;    // the game port's reader (the joystick's raw readings), from the load segment
    Bit16u joyData;             // its readings in the data segment: A x, A y, B x, B y, then the buttons, the axes' mask
};
const Build kBuilds[] = {
    { "ARMADA.EXE", "Armada", 0x2584, 0x14D0, 0x14EC, 0x1508, 0x1524, 0x1689, 0xEC, 0x8E56, 30, 0x5876, 0x003E, 0x5C7C },
    { "ARMADACD.EXE", "Armada (CD-ROM)", 0x2587, 0x14D0, 0x14EC, 0x1508, 0x1524, 0x1689, 0xEC, 0x8ED6, 30, 0x5879, 0x003E, 0x5D08 },
    { "PG.EXE", "Proving Grounds", 0x1B68, 0x0E2A, 0x0E46, 0x0E62, 0x0E7E, 0x0FE3, 0xFA, 0x7B1C, 20, 0x53F7, 0x0034, 0x4A02 },
    { "PGCD.EXE", "Proving Grounds (CD-ROM)", 0x1B6C, 0x0E2A, 0x0E46, 0x0E62, 0x0E7E, 0x0FE3, 0xFA, 0x7C2C, 20, 0x5416, 0x0042, 0x4B12 },
};

const double kStepMs = 37.0;    // a ninth more of the stop this often: the stop in a third of a second
const double kStaleMs = 400.0;  // no packet made this long (out of flight): what was held is forgotten

enum { AXIS_X, AXIS_Y };        // the packet's byte 3 (left, right), byte 4 (up, down)

struct Axis {
    int want;                   // this frame's key: -1, 0, 1 (the game's sign)
    int dir;                    // the key held, as the ramp has it
    double since;               // when that began (emulated ms)
    int frames, first, last;    // that key's frames so far, its first and latest value (the log's)
};

const Build *g_arm;
Bit16u g_load, g_psp;
Axis g_axis[2];
PhysPt g_packet;                // where this frame's keys wrote (0: none)
double g_lastEnd = -1e9;
int g_ramp = -1;
bool g_steerOn;                 // the page's right-button drag (wc_web_arm_steer): -1..1 each way, the keys' signs
double g_steerX, g_steerY;
bool g_chainTold;
bool g_joyHere;                 // the build's game-port reader is where the table says (its bytes checked at the load)
int g_joyGame = -1;             // WCNET_ARM_JOY=game: the game's own timing of the port
bool g_joyTold;

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

// The game port's reader (ARMADA.EXE seg5876:003E, the same code in all four builds): Origin's library times each
// axis of both sticks against the PC's timer (port 40h, channel 0 latched) until the port's bit for it clears, and
// keeps the times (joyData: A x, A y, B x, B y), the buttons (+8) and which axes answered (+9: an axis that never
// clears within 65535 reads is dropped from the mask for good).  The calibration (JOYA.DAT, or the options' JOY 1)
// is in those times, so they depend on how fast the CPU runs the loop on an emulated port: DOSBox's counting port
// ([joystick] timed=false, the page's) clears after so many reads, a third of the time at the high-resolution
// picture's 75,000 cycles (a stick at rest read 13 of the 32 steps left and down there), and the readings jitter by
// a few steps with the timer.  So while the page's stick is on the port (A; B not), the reader is not run: its
// readings are put where it keeps them, exact, in the units of the calibration the page writes as JOYA.DAT
// (web/gamefiles.js armadaPrepare): 0 hard over, 321 the centre, 642 the other way; the game's dead band 320..322
// (none: the page's dead zone is the one), its 32 steps 10 each.  The axes' mask
// keeps A's and drops B's (as the reader's own timeout would; a stick B the game thought there would be read with
// no calibration: a division by zero).  WCNET_ARM_JOY=game: the game's own reader.
// (The control packet then counts steps -2..2 as none: seg2584:0A0A in ARMADA.EXE.  So a stick past the page's dead
// zone is step 3 at least, the first one counted, and the rest of its travel shares steps 3..32 equally, each reading
// in the middle of its step: the page's dead zone is the only one.)
enum { kJoyCentre = 321, kJoyStep = 10, kJoySteps = 32, kJoyFirst = 3 };

bool joy_here() {
    const Bit8u a[] = { 0xC8, 0x04, 0x00, 0x00, 0x0E, 0xE8 };
    const Bit8u b[] = { 0x89, 0x46, 0xFE, 0xBA, 0x01, 0x02, 0x33, 0xC0, 0xC7, 0x46, 0xFC, 0xFF, 0xFF, 0x8B, 0xD8, 0x8A, 0x1E,
                        (Bit8u)(g_arm->joyData + 9), (Bit8u)((g_arm->joyData + 9) >> 8) };
    const Bit8u c[] = { 0xA3, (Bit8u)g_arm->joyData, (Bit8u)(g_arm->joyData >> 8) };
    const PhysPt at = ((PhysPt)(Bit16u)(g_load + g_arm->joySeg) << 4) + g_arm->joyEntry;
    return bytes_at(at, a, sizeof(a)) && bytes_at(at + 8, b, sizeof(b)) && bytes_at(at + 0x3C, c, sizeof(c));
}

// The reading for the page's stick v (-1..1, past its dead zone): the game's calibration (JOYA.DAT as the page writes
// it) makes step n of a reading r above the centre ceil((r - 322) / 10), below floor((r - 320) / 10).
Bit16u joy_reading(float v) {
    if (v == 0) return kJoyCentre;
    const float m = fabsf(v) > 1 ? 1 : fabsf(v);
    int n = kJoyFirst - 1 + (int)ceilf(m * (kJoySteps - kJoyFirst + 1) - 1e-4f);
    n = n < kJoyFirst ? kJoyFirst : n > kJoySteps ? kJoySteps : n;
    const int d = n * kJoyStep - kJoyStep / 2;
    return (Bit16u)(v > 0 ? kJoyCentre + 1 + d : kJoyCentre - 1 - d);
}

void joy_read() {
    if (g_joyGame < 0) {
        const char *env = getenv("WCNET_ARM_JOY");
        g_joyGame = env && strcmp(env, "game") == 0;
    }
    if (g_joyGame || !JOYSTICK_IsEnabled(0) || JOYSTICK_IsEnabled(1)) return;
    const PhysPt d = SegPhys(ds) + g_arm->joyData;
    const Bit8u mask = mem_readb(d + 9);
    if (mask & 1) mem_writew(d, joy_reading(JOYSTICK_GetMove_X(0)));
    if (mask & 2) mem_writew(d + 2, joy_reading(JOYSTICK_GetMove_Y(0)));
    mem_writeb(d + 8, (Bit8u)((JOYSTICK_GetButton(0, 0) ? 1 : 0) | (JOYSTICK_GetButton(0, 1) ? 2 : 0)));
    mem_writeb(d + 9, (Bit8u)(mask & 3));
    if (!g_joyTold) {
        g_joyTold = true;
        wclog(1, "Armada: the joystick's readings exact (centre %d, %d either way; the axes %X)", kJoyCentre,
              kJoyCentre, (unsigned)(mask & 3));
    }
    const PhysPt sp = SegPhys(ss) + reg_sp;     // (retf: the reader is a far procedure without arguments)
    const Bit16u retIp = mem_readw(sp), retCs = mem_readw(sp + 2);
    reg_sp = (Bit16u)(reg_sp + 4);
    SegSet16(cs, retCs);
    reg_eip = retIp;
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
int ramped(int axis, bool shift, double now) {
    Axis &a = g_axis[axis];
    const int dir = a.want;
    a.want = 0;
    if (dir != a.dir) {
        if (a.dir && a.frames) {
            static const char *const kNames[2][2] = { { "Left", "Right" }, { "Up", "Down" } };
            wclog(2, "Armada: %s held %.2f s, %d frames, ramped %+d to %+d", kNames[axis][a.dir > 0],
                  (now - a.since) / 1000.0, a.frames, a.first, a.last);
        }
        a.dir = dir;
        a.since = now;
        a.frames = 0;
    }
    if (!dir) {
        return 0;
    }
    int step = shift ? 9 : 1 + (int)((now - a.since) / kStepMs);
    if (step > 9) {
        step = 9;
    }
    const int v = dir * (int)g_arm->full * step / 9;
    if (!a.frames++) {
        a.first = v;
    }
    a.last = v;
    return v;
}

// The frame's control packet, as the game finds it: the controller (the far pointer the function was given, [bp+6]),
// its +5 (near), that one's +0xF: the packet, in the data segment.
PhysPt frame_packet() {
    const PhysPt arg = SegPhys(ss) + (Bit16u)(reg_bp + 6);
    const Bit16u off = mem_readw(arg), seg = mem_readw(arg + 2);
    const Bit16u ctl = mem_readw(((PhysPt)seg << 4) + (Bit16u)(off + 5));
    return SegPhys(ds) + mem_readw(SegPhys(ds) + (Bit16u)(ctl + 0xF));
}

// The drag's share of the stop on an axis (0: none).
int steered(double v) {
    int s = (int)(v * g_arm->full + (v < 0 ? -0.5 : 0.5));
    return s > g_arm->full ? g_arm->full : s < -(int)g_arm->full ? -(int)g_arm->full : s;
}

// The loop's end: the keys' writes of this frame made again, ramped; an axis no key or stick moves, the drag's.

// WCNET_ARM_TRACE=<seconds>: research, every call and return of two whole frames from then (frame = the control
// packet's loop end), to $WCNET_DUMP_DIR/trace.txt: "<instructions> C|R|I <CS>:<IP> -> <CS>:<IP> <mode>".
int g_traceState = -1;          // -1 unset, 0 off, 1 armed (waiting for the time), 2 waiting for a frame's start, 3 on, 4 done
double g_traceAt;
FILE *g_traceFile;
unsigned g_traceFrames;
Bit64u g_traceN;
int g_tracePending;             // 0 none, 'C' call, 'R' return, 'I' int/iret, 'J' far jump
Bit16u g_tracePrevCs;
Bit32u g_tracePrevIp;

void trace_frame_mark() {
    if (g_traceState == 2) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        g_traceFile = fopen((std::string(dir ? dir : ".") + "/trace.txt").c_str(), "w");
        g_traceState = g_traceFile ? 3 : 4;
        g_traceFrames = 0;
        g_traceN = 0;
        wclog(1, "Armada trace: on (load %04X)", g_load);
    } else if (g_traceState == 3) {
        fprintf(g_traceFile, "%llu F frame %u ends\n", (unsigned long long)g_traceN, g_traceFrames);
        if (++g_traceFrames >= 2) {
            fclose(g_traceFile);
            g_traceFile = NULL;
            g_traceState = 4;
            wclog(1, "Armada trace: done, %llu instructions", (unsigned long long)g_traceN);
        }
    }
}

void loop_end() {
    trace_frame_mark();
    const double now = PIC_FullIndex();
    {
        // WCNET_ARM_FPS=1: research, the flight's frames a second (control packets), every 5 s
        static int on = -1;
        static double from = 0;
        static unsigned n = 0;
        if (on < 0) on = getenv("WCNET_ARM_FPS") != NULL;
        if (on) {
            n++;
            if (now - from >= 5000) {
                if (from > 0) wclog(1, "Armada fps: %.1f", n * 1000.0 / (now - from));
                from = now;
                n = 0;
            }
        }
    }
    if (now - g_lastEnd > kStaleMs) {
        g_axis[AXIS_X].dir = g_axis[AXIS_Y].dir = 0;
        g_axis[AXIS_X].frames = g_axis[AXIS_Y].frames = 0;
    }
    g_lastEnd = now;
    const bool shift = held(0x2A) || held(0x36);
    const int x = ramped(AXIS_X, shift, now), y = ramped(AXIS_Y, shift, now);
    const PhysPt packet = frame_packet();
    {
        // WCNET_ARM_PACKET=1 (a test aid): the packet's stick bytes as the game made them (the stick, the keys), when they change
        static int on = -1;
        static Bit32u was = ~0u;
        if (on < 0) on = getenv("WCNET_ARM_PACKET") != NULL;
        if (on && packet) {
            const Bit32u now = mem_readb(packet + 3) | (mem_readb(packet + 4) << 8) | (mem_readb(packet + 5) << 16);
            if (now != was) {
                was = now;
                wclog(1, "Armada packet: %02x %02x %02x (x %+d, y %+d)", now & 0xFF, (now >> 8) & 0xFF, now >> 16,
                      (int)(Bit8s)((now & 0x7F) << 1) / 2, (int)(Bit8s)(((now >> 8) & 0x7F) << 1) / 2);
            }
        }
    }
    if (g_packet && g_packet != packet && !g_chainTold) {
        g_chainTold = true;     // (the keys wrote somewhere else: the drag's packet would be wrong; said once)
        wclog(0, "Armada: the keys wrote the packet at %05X, the frame's is at %05X", (unsigned)g_packet, (unsigned)packet);
    }
    const Bit8u gx = mem_readb(packet + 3), gy = mem_readb(packet + 4);
    int wx = x, wy = y;
    if (g_steerOn) {
        // (the drag where nothing else steers: no key this frame, the stick's reading nothing)
        if (!wx && !(gx & 0x7F)) wx = steered(g_steerX);
        if (!wy && !(gy & 0x7F)) wy = steered(g_steerY);
    }
    if (wx) {
        mem_writeb(packet + 3, (Bit8u)((gx & 0x80) | (wx & 0x7F)));
    }
    if (wy) {
        mem_writeb(packet + 4, (Bit8u)((gy & 0x80) | (wy & 0x7F)));
    }
    if (wx || wy) {
        wclog(3, "Armada: %s %+d %+d (the game's %+d %+d)", x || y ? "arrows" : "drag", wx, wy,
              (int)(Bit8s)(Bit8u)(gx << 1) >> 1, (int)(Bit8s)(Bit8u)(gy << 1) >> 1);
    }
    g_packet = 0;
}


// WCNET_ARM_PROF=1: where the emulated CPU is, sampled every thousand instructions, logged every 5 s (paragraph from
// the load segment:IP), those with ES at the VGA's A000 apart, and the VGA's mode and display start.
int g_prof = -1;
double g_profNext;
unsigned g_profSamples, g_startChanges;
Bitu g_lastStart = ~(Bitu)0;
std::map<Bit32u, unsigned> g_profAt, g_profVga;
std::map<Bit16u, unsigned> g_profEs, g_profCs;

std::string top_of(const std::map<Bit32u, unsigned> &m, size_t n) {
    std::vector<std::pair<unsigned, Bit32u> > top;
    for (std::map<Bit32u, unsigned>::const_iterator i = m.begin(); i != m.end(); ++i) top.push_back(std::make_pair(i->second, i->first));
    std::sort(top.rbegin(), top.rend());
    std::string t;
    for (size_t i = 0; i < top.size() && i < n; i++) {
        char b[40];
        snprintf(b, sizeof(b), " %04X:%04X=%u", top[i].second >> 16, top[i].second & 0xFFFF, top[i].first);
        t += b;
    }
    return t;
}

void prof_sample() {
    const Bit16u code = SegValue(cs);
    const Bit32u key = ((Bit32u)(Bit16u)(code - g_load) << 16) | (reg_eip & 0xFFFF);
    const Bit16u rel = (Bit16u)(code - g_load), ip = (Bit16u)reg_eip;
    const bool waiting = (rel == 0x34BD && ip >= 0x07EF && ip <= 0x0874) || (rel == 0x3FD8 && ip >= 0x0443 && ip <= 0x046E);
    static unsigned waits;
    if (waiting) { waits++; } else { g_profAt[key]++; g_profCs[rel]++; }
    if (SegValue(es) == 0xA000) g_profVga[key]++;
    g_profEs[(Bit16u)(SegValue(es) - g_load)]++;
    g_profSamples++;
    if (vga.config.display_start != g_lastStart) {
        g_lastStart = vga.config.display_start;
        g_startChanges++;
    }
    const double now = PIC_FullIndex();
    if (now < g_profNext) return;
    g_profNext = now + 5000;
    wclog(1, "Armada profile: %u samples; mode %d %ux%u chained %d memory_mode %02x start %lu (changed %u times); top:%s",
          g_profSamples, (int)vga.mode, (unsigned)vga.draw.width, (unsigned)vga.draw.height, vga.config.chained ? 1 : 0,
          vga.seq.memory_mode, (unsigned long)vga.config.display_start, g_startChanges, top_of(g_profAt, 40).c_str());
    {
        std::map<Bit32u, unsigned> c;
        for (std::map<Bit16u, unsigned>::const_iterator i = g_profCs.begin(); i != g_profCs.end(); ++i) c[(Bit32u)i->first << 16] = i->second;
        wclog(1, "Armada profile: waiting %u of %u; CS (paragraph from load):%s", waits, g_profSamples, top_of(c, 12).c_str());
        waits = 0;
        g_profCs.clear();
    }
    wclog(1, "Armada profile: ES=A000 at:%s", top_of(g_profVga, 20).c_str());
    std::map<Bit32u, unsigned> es;
    for (std::map<Bit16u, unsigned>::const_iterator i = g_profEs.begin(); i != g_profEs.end(); ++i) es[(Bit32u)i->first << 16] = i->second;
    wclog(1, "Armada profile: ES (paragraph from load):%s", top_of(es, 8).c_str());
    g_profAt.clear();
    g_profVga.clear();
    g_profEs.clear();
    g_profSamples = 0;
    g_startChanges = 0;
}


}  // namespace

bool arm_tracing() {
    if (g_traceState < 0) {
        const char *env = getenv("WCNET_ARM_TRACE");
        g_traceState = env && env[0] ? 1 : 0;
        if (g_traceState) g_traceAt = atof(env) * 1000.0;
    }
    if (g_traceState == 1 && g_arm && PIC_FullIndex() >= g_traceAt) g_traceState = 2;
    return g_traceState == 3;
}

void arm_trace_step() {
    const Bit16u code = SegValue(cs);
    const Bit32u ip = reg_eip;
    g_traceN++;
    {
        // WCNET_ARM_PWATCH=<phys hex>[,...]: research, a changed byte at a physical address, and the code just before
        static std::vector<PhysPt> at;
        static std::vector<Bit8u> was;
        static int read = 0;
        if (!read) {
            read = 1;
            const char *env = getenv("WCNET_ARM_PWATCH");
            for (const char *c = env; c && *c;) {
                at.push_back((PhysPt)strtoul(c, NULL, 16));
                was.push_back(phys_readb(at.back()));
                c = strchr(c, ',');
                if (c) c++;
            }
        }
        for (size_t k = 0; k < at.size(); k++) {
            const Bit8u now = phys_readb(at[k]);
            if (now != was[k]) {
                fprintf(g_traceFile, "%llu W %07X %02x -> %02x by %04X:%04X \n", (unsigned long long)g_traceN,
                        (unsigned)at[k], was[k], now, g_tracePrevCs, (unsigned)g_tracePrevIp);
                was[k] = now;
            }
        }
    }
    if (g_tracePending || code != g_tracePrevCs) {
        const char kind = g_tracePending ? (char)g_tracePending : 'X';
        fprintf(g_traceFile, "%llu %c %04X:%04X -> %04X:%04X %s\n", (unsigned long long)g_traceN, kind, g_tracePrevCs,
                (unsigned)g_tracePrevIp, code, (unsigned)ip, (reg_flags & FLAG_VM) ? "v86" : cpu.pmode ? (cpu.code.big ? "pm32" : "pm16") : "real");
    }
    g_tracePending = 0;
    // what this instruction is: past its prefixes
    PhysPt at = SegPhys(cs) + (cpu.code.big ? ip : (ip & 0xFFFF));
    Bit8u op = 0, modrm = 0;
    for (int k = 0; k < 6; k++) {
        if (mem_readb_checked(at, &op)) { op = 0; break; }
        if (op == 0x66 || op == 0x67 || op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65 ||
            op == 0xF2 || op == 0xF3) { at++; continue; }
        break;
    }
    if (op == 0xFF && !mem_readb_checked(at + 1, &modrm)) {
        const int r = (modrm >> 3) & 7;
        if (r == 2 || r == 3) g_tracePending = 'C';
        else if (r == 5) g_tracePending = 'J';
    } else if (op == 0xE8 || op == 0x9A) {
        g_tracePending = 'C';
    } else if (op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA) {
        g_tracePending = 'R';
    } else if (op == 0xCD || op == 0xCF || op == 0xCC) {
        g_tracePending = 'I';
    } else if (op == 0xEA) {
        g_tracePending = 'J';
    }
    g_tracePrevCs = code;
    g_tracePrevIp = ip;
}

void arm_tick() {
    arm_hires_tick();
    if (!g_arm) return;
    if (g_prof < 0) {
        const char *env = getenv("WCNET_ARM_PROF");
        g_prof = env && env[0] && env[0] != '0';
        g_profNext = PIC_FullIndex() + 5000;
    }
    if (g_prof) prof_sample();
}

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
        g_joyHere = joy_here();
        if (!g_joyHere) {
            wclog(0, "%s: the game port's reader is not where the hooks know it: the joystick read the game's way", name);
        }
        g_joyTold = false;
        g_psp = psp;
        g_packet = 0;
        g_lastEnd = -1e9;
        memset(g_axis, 0, sizeof(g_axis));
        arm_hires_loaded(kBuilds[i].exe, loadSeg);
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
        arm_hires_loaded(NULL, 0);
        hooks_game_changed();
    }
}

void arm_watch_list(void (*watch)(Bit16u ip)) {
    if (!g_arm) {
        return;
    }
    if (ramp_on()) {
        watch(g_arm->orA);
        watch(g_arm->orB);
        watch(g_arm->orC);
        watch(g_arm->orD);
    }
    watch(g_arm->loopEnd);      // (the ramp's, and the drag's)
    if (g_joyHere) {
        watch(g_arm->joyEntry);     // (the game port's reader)
    }
    arm_hires_watch(watch);     // (the high-resolution picture's, once it is first wanted)
}

void arm_steer(double x, double y, bool on) {
    g_steerX = x < -1 ? -1 : x > 1 ? 1 : x;
    g_steerY = y < -1 ? -1 : y > 1 ? 1 : y;
    if (on != g_steerOn) {
        wclog(2, "Armada: the drag %s", on ? "steers" : "let go");
    }
    g_steerOn = on;
}

bool arm_script_item(const std::string &item, double t) {
    if (arm_hires_script_item(item, t)) return true;
    // !armdump=<name>: research, the linear 0..10FFFF and 200000..23FFFF (as paged now) to $WCNET_DUMP_DIR/<name>.lo/.hi
    if (item.compare(0, 9, "!armdump=") == 0) {
        const char *dir = getenv("WCNET_DUMP_DIR");
        const std::string name = std::string(dir ? dir : ".") + "/" + item.substr(9);
        const struct { const char *ext; PhysPt from, to; } parts[] = { { ".lo", 0, 0x110000 }, { ".hi", 0x200000, 0x240000 } };
        for (size_t k = 0; k < 2; k++) {
            FILE *f = fopen((name + parts[k].ext).c_str(), "wb");
            if (!f) continue;
            unsigned bad = 0;
            for (PhysPt a = parts[k].from; a < parts[k].to; a++) {
                Bit8u b = 0;
                if (mem_readb_checked(a, &b)) { b = 0; bad++; }
                fputc(b, f);
            }
            fclose(f);
            wclog(1, "key script %.1fs: dumped %s%s (%u unreadable)", t, name.c_str(), parts[k].ext, bad);
        }
        for (Bitu sel = 8; sel < 0x200 && cpu.gdt.GetLimit(); sel += 8) {
            Descriptor dsc;
            if (!cpu.gdt.GetDescriptor(sel, dsc) || !dsc.saved.seg.p) continue;
            wclog(1, "GDT %04X: base %08X limit %08X type %02X big %u", (unsigned)sel, (unsigned)dsc.GetBase(),
                  (unsigned)dsc.GetLimit(), (unsigned)dsc.Type(), (unsigned)dsc.Big());
            if (dsc.Type() & 0x08) {    // code: save it (at most 256 KB, as far as the pages go)
                char nm[32];
                snprintf(nm, sizeof(nm), ".sel%04X", (unsigned)sel);
                FILE *f = fopen((name + nm).c_str(), "wb");
                if (!f) continue;
                const Bitu lim = dsc.GetLimit() < 0x3FFFF ? dsc.GetLimit() : 0x3FFFF;
                for (Bitu o = 0; o <= lim; o++) {
                    Bit8u b;
                    if (mem_readb_checked((PhysPt)(dsc.GetBase() + o), &b)) break;
                    fputc(b, f);
                }
                fclose(f);
            }
        }
        wclog(1, "key script: CS %04X:%08X DS %04X ES %04X SS %04X pmode %d v86 %d cr3 %08X load %04X",
              SegValue(cs), (unsigned)reg_eip, SegValue(ds), SegValue(es), SegValue(ss), cpu.pmode ? 1 : 0,
              (reg_flags & FLAG_VM) ? 1 : 0, (unsigned)paging.cr3, g_load);
        return true;
    }
    // !armsteer=<x>:<y> (-1..1, right and down positive) is the page's right-button drag held there; !armsteer=off
    if (item.compare(0, 10, "!armsteer=") != 0) {
        return false;
    }
    const char *v = item.c_str() + 10;
    if (strcmp(v, "off") == 0) {
        arm_steer(0, 0, false);
    } else {
        const char *colon = strchr(v, ':');
        arm_steer(atof(v), colon ? atof(colon + 1) : 0, true);
    }
    wclog(1, "key script %.1fs: the drag %s", t, v);
    return true;
}

bool arm_check_hooks(Bit32u ip) {
    if (arm_hires_hook(ip)) return true;    // (the high-resolution picture's: wcnet_armada_hires.cpp)
    if (g_arm && g_joyHere && ip == g_arm->joyEntry && SegValue(cs) == (Bit16u)(g_load + g_arm->joySeg)) {
        joy_read();
        return true;
    }
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
