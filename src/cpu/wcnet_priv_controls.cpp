/*
 *  Privateer's flight controls as the emulator gives them (wcnet_priv.h, docs/privateer.md section 4).
 *
 *  The game reads its controls once a frame in seg088:0B2E: the control record (a far pointer at the controller's
 *  +2) is cleared (seg087:000D: pitch at +0x06, roll at +0x0A, yaw at +0x0E, int32, 0x100 = the stick at its stop),
 *  then the mode's routine fills it: the joystick's (seg088:0AEA) when dseg:13EF is set, the mouse's (seg088:0B0C)
 *  when dseg:13F0 is, the keyboard's (seg088:0439) when dseg:13F1 is.  The keyboard's routine reads the game's own
 *  held-key table (dseg:69DE + scan code, kept by its int 15h hook) and gives a held arrow the stop at once: Down
 *  +0x100 pitch, Up -0x100, Left +0x100 yaw, Right -0x100, keypad 7 -0x100 roll, keypad 9 +0x100.  So a tap of an
 *  arrow turns at the full rate for a frame or more and the nose jumps past what it was aimed at (the user,
 *  2026-11-07: "when you hit the key a bit it just hops over beyond the target").
 *
 *  The fix is Wing Commander's and RetroVega's ramp, at the instruction after that call (seg088:0BAD, a root
 *  segment: the place is fixed): an axis held builds from a ninth of the stop to the stop in a third of a second (a
 *  ninth more every 37 ms), Shift with it gives the stop at once, the opposite key or letting go puts it to
 *  nothing.  The game's own held-key table is what is read, so whatever the game takes for an arrow (the keypad's
 *  too) is ramped, and nothing reaches the game any other way.  WCNET_PRIV_RAMP=0 leaves the game's own.
 */
#include <stdlib.h>
#include <string.h>
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"
#include "keyboard.h"

namespace wc {

namespace {

enum {
    kHeld = 0x69DE,             // the held-key table, by scan code (1 held)
    kModeJoystick = 0x13EF,     // the control mode's flags
    kModeMouse = 0x13F0,
    kModeKeyboard = 0x13F1,
    kMouseX = 0x6A7E,           // the mouse's place as the game keeps it (seg232:021B, 0233), on its 320 x 200 picture
    kMouseY = 0x6A80,
    kMouseMidX = 0x3C5B,        // the middle the mouse's flying measures from
    kMouseMidY = 0x3C5D,
};
const double kStepMs = 37.0;    // a ninth more of the stop this often: the stop in a third of a second

struct Axis {
    int dir;                    // -1, 0, 1: the game's sign for the key held
    double since;               // when that began (emulated ms)
};
Axis g_pitch, g_roll, g_yaw;
int g_ramp = -1;
double g_targetKeyAt = -1e9;
unsigned g_frames;

bool held(int scan) { return priv_r8((Bit16u)(kHeld + scan)) == 1; }

// The axis's value this frame: dir is the game's sign of the key held (0: none, or both ways).
Bit32s ramp(Axis &a, int dir, bool shift, double now) {
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
    return (Bit32s)(dir * 0x100 * step / 9);
}

int axis_dir(int plus, int minus) {
    const bool p = held(plus), m = held(minus);
    return p == m ? 0 : p ? 1 : -1;
}

}  // namespace

bool priv_controls_key(int kbdKey, bool pressed) {
    // (the keys go to the game as they are: the ramp works on what it read; A in flight is an occasion for the room)
    if (pressed && kbdKey == KBD_a && priv_in_flight()) {
        priv_session_event(1);
    }
    if (pressed && kbdKey == KBD_t && priv_in_flight()) {
        g_targetKeyAt = PIC_FullIndex();    // (a target chosen by the pilot: friendly fire's "targeting each other")
    }
    return false;
}

// At seg088:0BAD, the keyboard's routine just returned: the control record as it wrote it.
void priv_controls_frame() {
    if (g_ramp < 0) {
        const char *env = getenv("WCNET_PRIV_RAMP");
        g_ramp = !(env && env[0] == '0');
        wclog(1, "Privateer: the arrows %s", g_ramp ? "ramped (Wing Commander's way)" : "as the game has them (WCNET_PRIV_RAMP=0)");
    }
    g_frames++;
    if (!g_ramp || !priv_r8(kModeKeyboard)) {
        return;
    }
    const Bit16u controller = (Bit16u)reg_si;
    const Bit16u off = priv_r16((Bit16u)(controller + 2)), seg = priv_r16((Bit16u)(controller + 4));
    const PhysPt rec = ((PhysPt)seg << 4) + off;
    const double now = PIC_FullIndex();
    const bool shift = held(0x2A) || held(0x36);
    const Bit32s pitch = ramp(g_pitch, axis_dir(0x50, 0x48), shift, now);   // Down climbs (+), Up dives
    const Bit32s yaw = ramp(g_yaw, axis_dir(0x4B, 0x4D), shift, now);       // Left (+), Right
    const Bit32s roll = ramp(g_roll, axis_dir(0x49, 0x47), shift, now);     // keypad 9 (+), keypad 7
    if (g_pitch.dir || g_yaw.dir || g_roll.dir) {
        wclog(3, "Privateer: arrows %+d %+d %+d (the game's %+d %+d %+d)", (int)pitch, (int)yaw, (int)roll,
              (int)(Bit32s)mem_readd(rec + 6), (int)(Bit32s)mem_readd(rec + 0xE), (int)(Bit32s)mem_readd(rec + 0xA));
    }
    mem_writed(rec + 0x6, (Bit32u)pitch);
    mem_writed(rec + 0xE, (Bit32u)yaw);
    mem_writed(rec + 0xA, (Bit32u)roll);
}

// The mouse's own pointer in flight (the options' MOUSE): the game steers by where the pointer is -- seg088:02E5, its x
// (ds:6A7E) less the middle's (ds:3C5B) over 10 the turn, its y (ds:6A80) less ds:3C5D over 3 the pitch -- and shows
// none, so nothing says where the middle is (the user, 2026-11-20: "it still steered the ship but was not available").
// The emulator draws TIE Fighter's ring there (sdlmain.cpp), the game's mouse being 640 x 200, while the flight runs
// (not over the options screen or the nav map, which stop the frame).
// The page's Pointer choice (WCPRIVPOINTER): "ring" (the default) shows it, "game" leaves the game as it is -- no pointer
// in flight, the options screen's black (the user, 2026-11-20: "make the amber ring selectable ... if that's how the
// original has it then many purists may want it that way")
bool priv_pointer_shown() {
    static int shown = -1;
    if (shown < 0) {
        const char *env = getenv("WCPRIVPOINTER");
        shown = !(env && strcmp(env, "game") == 0);
        wclog(1, "Privateer: the mouse's pointer %s", shown ? "shown (a ring in flight, white on the options screen)" : "as the game has it (WCPRIVPOINTER=game)");
    }
    return shown != 0;
}

bool priv_controls_marker(double *x, double *y) {
    if (!priv_found() || !priv_r8(kModeMouse) || !priv_flight_frame_recent() || !priv_pointer_shown()) {
        return false;
    }
    const int mx = (Bit16s)priv_r16(kMouseX), my = (Bit16s)priv_r16(kMouseY);
    static bool told;
    if (!told) {
        told = true;
        wclog(1, "Privateer: the mouse flies the ship: its pointer drawn at %d, %d (the middle %d, %d)", mx, my,
              (Bit16s)priv_r16(kMouseMidX), (Bit16s)priv_r16(kMouseMidY));
    }
    *x = (mx + 0.5) / 640.0;         // (the mouse's 640 across, as the driver gives it; the middle 320, 100)
    *y = (my + 0.5) / 200.0;
    return true;
}

double priv_controls_target_key_at() {
    return g_targetKeyAt;
}

unsigned priv_controls_frames() {
    return g_frames;
}

const char *priv_control_mode() {
    return priv_r8(kModeJoystick) ? "joystick" : priv_r8(kModeMouse) ? "mouse" : priv_r8(kModeKeyboard) ? "keyboard" : "none";
}

}  // namespace wc
