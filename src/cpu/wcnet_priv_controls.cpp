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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wcnet_priv.h"
#include "wcnet_log.h"
#include "mem.h"
#include "regs.h"
#include "pic.h"
#include "keyboard.h"
#include "joystick.h"

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
    kNoControlsA = 0x1142,      // either set: the frame reads no controls (seg088:0B65, 0B6E jump past the modes)
    kNoControlsB = 0x23B6,
};
const double kStepMs = 37.0;    // a ninth more of the stop this often: the stop in a third of a second
const double kStickMs = 250.0;  // the page says its stick every 16 ms: one unsaid this long has been let go

struct Axis {
    int dir;                    // -1, 0, 1: the game's sign for the key held
    double since;               // when that began (emulated ms)
};
Axis g_pitch, g_roll, g_yaw;
int g_ramp = -1;
double g_stickX, g_stickY, g_stickAt = -1e9;   // the page's stick (wc_web_stick): right and climbing positive
bool g_portWas;                 // stick A was on the game port at the last frame
bool g_emptyTold;
int g_pointerShown = -1;        // the page's pointer choice (-1: not read yet)
double g_targetKeyAt = -1e9;
unsigned g_frames;
int g_mouseInverted = -1;       // the page's "Invert mouse" (-1: not read yet; 1 the game's own way)

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

bool ramp_on() {
    if (g_ramp < 0) {
        const char *env = getenv("WCNET_PRIV_RAMP");
        g_ramp = !(env && env[0] == '0');
        wclog(1, "Privateer: the arrows %s", g_ramp ? "ramped (Wing Commander's way)" : "as the game has them (WCNET_PRIV_RAMP=0)");
    }
    return g_ramp != 0;
}

// The arrows held, as the record's three axes: ramped, or at the stop at once as the game has them (WCNET_PRIV_RAMP=0).
void keys_ramp(double now, Bit32s *pitch, Bit32s *yaw, Bit32s *roll) {
    const bool full = !ramp_on() || held(0x2A) || held(0x36);
    *pitch = ramp(g_pitch, axis_dir(0x50, 0x48), full, now);   // Down climbs (+), Up dives
    *yaw = ramp(g_yaw, axis_dir(0x4B, 0x4D), full, now);       // Left (+), Right
    *roll = ramp(g_roll, axis_dir(0x49, 0x47), full, now);     // keypad 9 (+), keypad 7
}

PhysPt control_record() {
    const Bit16u controller = (Bit16u)reg_si;
    const Bit16u off = priv_r16((Bit16u)(controller + 2)), seg = priv_r16((Bit16u)(controller + 4));
    return ((PhysPt)seg << 4) + off;
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
    if (!ramp_on() || !priv_r8(kModeKeyboard)) {
        return;
    }
    const PhysPt rec = control_record();
    const double now = PIC_FullIndex();
    Bit32s pitch, yaw, roll;
    keys_ramp(now, &pitch, &yaw, &roll);
    if (g_pitch.dir || g_yaw.dir || g_roll.dir) {
        wclog(3, "Privateer: arrows %+d %+d %+d (the game's %+d %+d %+d)", (int)pitch, (int)yaw, (int)roll,
              (int)(Bit32s)mem_readd(rec + 6), (int)(Bit32s)mem_readd(rec + 0xE), (int)(Bit32s)mem_readd(rec + 0xA));
    }
    mem_writed(rec + 0x6, (Bit32u)pitch);
    mem_writed(rec + 0xE, (Bit32u)yaw);
    mem_writed(rec + 0xA, (Bit32u)roll);
}

// At seg088:0BBD, where every mode's reading of the controls meets (the record filled, or left cleared: see kNoControls).
// Three things of the page's controller (the user, 2026-11-21: "I tried using the joystick but I was given 3 bad
// options... if I selected keyboard in the options menu the joystick was inert. If I select mouse, the joystick drove
// around the blue dot ... way too slow to steer. And if I selected "joystick" as the option my ship was spinning around
// like absolute crazy--I think the joystick option is actually the right one but it wouldn't let me control at all"):
//  - a pad "built in" (stick A on the game port, the page's choice at the launch): the options' JOYSTICK picked for the
//    pilot when the stick comes onto the port, as that button does (seg037:0004 writes the three flags, nothing else);
//    a mode the pilot picks afterwards stays theirs.  The game then reads the stick its own way (GOG's JOYA.DAT has it
//    calibrated 0 / 64 / 128, the untimed port's readings: [joystick] timed=false);
//  - the options' JOYSTICK with no stick on the port: every reading times out, the stick reads hard over and the ship
//    spins; the arrows steer instead, ramped as in the keyboard's mode;
//  - a pad that is not built in (or a phone's touch stick): its stick, said by the page every 16 ms (wc_web_stick),
//    steers in any mode, the stop being the stick at its end, the way the keyboard's ramp writes the record; an axis
//    an arrow is held on stays the arrow's.
void priv_controls_join() {
    g_frames++;                     // (every mode's frames: the report's "keyboard frames", its old name, which rigs read)
    if (g_mouseInverted < 0) {
        priv_mouse_inverted();      // (the launch's choice said in the log at the first flight, whatever the mode)
    }
    const bool port = JOYSTICK_IsEnabled(0);
    if (port != g_portWas) {
        g_portWas = port;
        if (port && !priv_r8(kModeJoystick)) {
            wclog(1, "Privateer: a joystick on the game port: the options' JOYSTICK picked (was %s)", priv_control_mode());
            priv_w8(kModeJoystick, 1);
            priv_w8(kModeMouse, 0);
            priv_w8(kModeKeyboard, 0);
        }
    }
    if (priv_r8(kNoControlsA) || priv_r8(kNoControlsB)) {
        return;
    }
    const double now = PIC_FullIndex();
    const bool live = now - g_stickAt < kStickMs;
    const bool empty = priv_r8(kModeJoystick) && !port;
    const PhysPt rec = control_record();
    Bit32s pitch = (Bit32s)mem_readd(rec + 0x6), yaw = (Bit32s)mem_readd(rec + 0xE), roll = (Bit32s)mem_readd(rec + 0xA);
    static unsigned told;           // (the rigs' and the page test's evidence, twice a second while it steers)
    if (!live && priv_r8(kModeMouse) && !priv_r8(kModeJoystick) && !priv_mouse_inverted()) {
        // The mouse's pitch the other way up (see priv_mouse_inverted): the record's only, its turn, roll and the speed
        // (button 2 and the mouse up or down, seg088:0909) stay the game's
        pitch = -pitch;
        mem_writed(rec + 0x6, (Bit32u)pitch);
        static unsigned toldMouse;
        if (pitch && toldMouse++ % 14 == 0) {
            wclog(3, "Privateer: the mouse steers, up climbing: pitch %+d yaw %+d (the game's pitch %+d)", (int)pitch,
                  (int)yaw, (int)-pitch);
        }
    }
    if (!live && !empty) {
        if (port && priv_r8(kModeJoystick) && (pitch || yaw || roll) && told++ % 14 == 0) {
            wclog(3, "Privateer: the joystick (built in): pitch %+d yaw %+d roll %+d", (int)pitch, (int)yaw, (int)roll);
        }
        return;
    }
    if (empty) {
        if (!g_emptyTold) {
            g_emptyTold = true;
            wclog(1, "Privateer: the options' JOYSTICK with no joystick on the port (it read %+d %+d %+d): the arrows steer",
                  (int)pitch, (int)yaw, (int)roll);
        }
        keys_ramp(now, &pitch, &yaw, &roll);
    }
    if (live) {
        // (an arrow held keeps its axis where the arrows steer: the keyboard's mode, or the empty port's)
        const bool arrows = empty || priv_r8(kModeKeyboard);
        if (!(arrows && axis_dir(0x50, 0x48))) {
            pitch = (Bit32s)lround(g_stickY * 0x100);
        }
        if (!(arrows && axis_dir(0x4B, 0x4D))) {
            yaw = (Bit32s)lround(-g_stickX * 0x100);
        }
        if (!arrows && axis_dir(0x49, 0x47)) {
            // (the pad rolls by the keypad's 7 and 9, which the mouse's mode does not read: ramped here)
            Bit32s p, y;
            keys_ramp(now, &p, &y, &roll);
        }
    }
    mem_writed(rec + 0x6, (Bit32u)pitch);
    mem_writed(rec + 0xE, (Bit32u)yaw);
    mem_writed(rec + 0xA, (Bit32u)roll);
    if ((pitch || yaw || roll) && told++ % 14 == 0) {
        wclog(3, "Privateer: steered by %s (%s mode): pitch %+d yaw %+d roll %+d", live ? "the page's stick" : "the arrows",
              priv_control_mode(), (int)pitch, (int)yaw, (int)roll);
    }
}

void priv_controls_stick(double x, double y) {
    g_stickX = x < -1 ? -1 : x > 1 ? 1 : x;
    g_stickY = y < -1 ? -1 : y > 1 ? 1 : y;
    g_stickAt = PIC_FullIndex();
}

// The mouse's own pointer in flight (the options' MOUSE): the game steers by where the pointer is -- seg088:02E5, its x
// (ds:6A7E) less the middle's (ds:3C5B) over 10 the turn, its y (ds:6A80) less ds:3C5D over 3 the pitch -- and shows
// none, so nothing says where the middle is (the user, 2026-11-20: "it still steered the ship but was not available").
// The emulator draws TIE Fighter's ring there (sdlmain.cpp), the game's mouse being 640 x 200, while the flight runs
// (not over the options screen or the nav map, which stop the frame).
// The page's pointer choice (WCPRIVPOINTER at the launch, wc_web_priv_pointer since): "ring" (the default) shows it,
// "game" leaves the game as it is -- no pointer in flight, the options screen's black (the user, 2026-11-20: "make the
// amber ring selectable ... if that's how the original has it then many purists may want it that way"; 2026-11-21: in
// the Controller list, "Kbd + mouse (shown)" and "Keyboard + mouse")
bool priv_pointer_shown() {
    if (g_pointerShown < 0) {
        const char *env = getenv("WCPRIVPOINTER");
        priv_pointer_set(!(env && strcmp(env, "game") == 0));
    }
    return g_pointerShown != 0;
}

void priv_pointer_set(bool shown) {
    if ((int)shown != g_pointerShown) {
        wclog(1, "Privateer: the mouse's pointer %s", shown ? "shown (a ring in flight, white on the options screen)" : "as the game has it (none in flight)");
    }
    g_pointerShown = shown ? 1 : 0;
}

// The mouse flying the ship (the options' MOUSE) climbs as a flight stick does: the game's pitch is the pointer's y
// less the middle's (seg088:0373, not negated, where the turn is: seg088:0327), so the pointer below the middle climbs
// and pushing the mouse up dives, while left and right turn toward the pointer (the user, 2026-11-22: "The mouse is
// inverted. Is there a way to change it back. An option or toggle").  The game has no setting for it.  The page's
// "Invert mouse" (WCPRIVMOUSEINVERT=1 at the launch, wc_web_priv_mouse_invert since): unticked, the default, the
// mouse pushed up climbs, toward the pointer as the turn is; ticked, the game's own.
bool priv_mouse_inverted() {
    if (g_mouseInverted < 0) {
        const char *env = getenv("WCPRIVMOUSEINVERT");
        priv_mouse_invert_set(env && env[0] == '1');
    }
    return g_mouseInverted != 0;
}

void priv_mouse_invert_set(bool inverted) {
    if ((int)inverted != g_mouseInverted) {
        wclog(1, "Privateer: the mouse %s", inverted ? "inverted, as the game has it (pushed up, the nose goes down)"
                                                     : "pushed up raises the nose (the game's pitch the other way up)");
    }
    g_mouseInverted = inverted ? 1 : 0;
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
