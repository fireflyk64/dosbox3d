/*
 *  TIE Fighter's flight controls as the emulator gives them (docs/tiefighter.md, step 7; the user, 2026-11-06: "we will
 *  need to use mechanisms similar to RetroVega to get arrows to control motion--and we will need another mode of
 *  flight that is more similar to the current retrovega mouse in addition to the drag-to-steer methods built in...
 *  some users are going to want drag-to-steer to get back to the 90's era...so we will need a selector for both").
 *
 *  The game steers by the mouse's motion: a frame's mickeys are that frame's stick (feinput.c in OpenTIE: 128 to a
 *  mickey across, 64 up and down, nothing below 2,048, so 17 mickeys a frame across and 33 up and down are the least
 *  that turn, 191 and 127 the most).  So everything here becomes mickeys added once a frame, at the top of the
 *  frame (tie_updatetime), before the game reads them:
 *
 *  - The arrow keys steer as RetroVega's (and WC1's) do: an axis's step goes one further every other frame its arrow
 *    is held (some 30 steps a second), 1 to 9, then 8 and 9 in turn; the opposite arrow or letting go puts it to 0;
 *    Shift with an arrow is 9 at once.  Up dives, as a stick pushed forward.  The game's own arrows (Left and Right
 *    roll) are taken from it in flight: the keypad's Ins and Del roll instead (RetroVega's other roll keys), as the
 *    game's own Left and Right arrows passed to it.
 *  - The mouse, in one of two modes.  "drag" is the game's own (the 1994 way): its motion goes to the game as it is.
 *    "pointer" is RetroVega's: the motion moves a pointer the emulator draws over the picture, and the pointer's
 *    distance from the middle is the stick, held until the pointer moves (the nose goes where it points: above the
 *    middle climbs).  F12 in flight switches between them; WCNET_TIE_MOUSE=drag|pointer starts in one (pointer by
 *    default), and the page's selector sets it (wc_web_tie_mouse).
 *
 *  Nothing of the game is changed: keys are filtered and added at the keyboard, mickeys at the mouse driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include "wcnet_tie.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "keyboard.h"
#include "mouse.h"
#include "pic.h"

extern std::string incoming_text;   // the line the emulator draws over the picture (sdlmain.cpp)

namespace wc {

namespace {

// What steers besides the arrows (which always do): the game's own mouse, the pointer, the keyboard alone (the mouse
// steers nothing in flight), a joystick (the page's pad as the stick: wc_web_stick; the mouse steers nothing).
enum { MOUSE_DRAG = 0, MOUSE_POINTER = 1, MOUSE_KEYBOARD = 2, MOUSE_JOYSTICK = 3 };
const char *const kModeNames[] = { "drag", "pointer", "keyboard", "joystick" };
const char *const kModeLines[] = {
    "Flight controls: the mouse drags to steer, the game's own (F12: the keyboard alone)",
    "Flight controls: the mouse points to aim (F12: drag to steer)",
    "Flight controls: the keyboard alone, the arrows (F12: the mouse points to aim)",
    "Flight controls: a joystick",
};
int g_mode = -1;
double g_stickX, g_stickY, g_stickAt = -1e9;   // the page's pad (pitch positive climbs), and when it last said
bool g_left, g_right, g_up, g_down, g_shiftL, g_shiftR, g_rollLeft, g_rollRight;
int g_stepX, g_stepY;
double g_px, g_py;                  // the pointer, -1..1 from the middle (down and right positive)
unsigned g_frames;
bool g_wasFlying;
bool g_adding;                      // our own keys going in: not ours to filter

// The pointer's travel for the whole width: host pixels (after DOSBox's sensitivity).
const double kPointerSpan = 600.0;
const double kPointerDeadZone = 0.04;

int mode() {
    if (g_mode < 0) {
        const char *env = getenv("WCNET_TIE_MOUSE");
        g_mode = MOUSE_POINTER;
        for (int m = 0; env && m < 4; m++) {
            if (!strcmp(env, kModeNames[m])) {
                g_mode = m;
            }
        }
    }
    return g_mode;
}

// RetroVega's key_ramp (flight.c), WC1's: the step an axis's arrow gives this time.
int key_ramp(int was, int dir, bool shift) {
    if (!dir || (was > 0 && dir < 0) || (was < 0 && dir > 0)) {
        return 0;
    }
    if (shift) {
        return dir * 9;
    }
    if (was < 0) {
        was = -was;
    }
    return dir * (was < 9 ? was + 1 : 8);
}

// A stick's deflection (-1..1) as the mickeys of one frame: past the game's dead zone, `least` to `most`.
int mickeys(double d, int least, int most) {
    if (d == 0) {
        return 0;
    }
    const double m = least + (most - least) * fmin(1.0, fabs(d));
    return (int)(d < 0 ? -m : m);
}

void say(const std::string &line) {
    incoming_text = line;
    wclog(1, "TIE: %s", line.c_str());
}

void add_key(KBD_KEYS key, bool pressed) {
    g_adding = true;
    KEYBOARD_AddKey(key, pressed);
    g_adding = false;
}

}  // namespace

bool tie_controls_key(int key, bool pressed) {
    if (g_adding || !tie_found()) {
        return false;
    }
    switch (key) {
    case KBD_leftshift: g_shiftL = pressed; return false;
    case KBD_rightshift: g_shiftR = pressed; return false;
    default: break;
    }
    if (!tie_in_flight()) {
        g_left = g_right = g_up = g_down = false;
        return false;
    }
    switch (key) {
    case KBD_left:
        if (g_rollLeft) return false;       // (the roll key's own repeats of the game's arrow)
        g_left = pressed;
        return true;
    case KBD_right:
        if (g_rollRight) return false;
        g_right = pressed;
        return true;
    case KBD_up: g_up = pressed; return true;
    case KBD_down: g_down = pressed; return true;
    case KBD_kp0:                           // Ins: roll as the game's Left arrow does
        if (pressed != g_rollLeft) {
            g_rollLeft = pressed;
            add_key(KBD_left, pressed);
        }
        return true;
    case KBD_kpperiod:                      // Del: roll as its Right arrow does
        if (pressed != g_rollRight) {
            g_rollRight = pressed;
            add_key(KBD_right, pressed);
        }
        return true;
    case KBD_f12:                           // pointer, drag, keyboard, pointer, ...
        if (pressed) {
            g_mode = mode() == MOUSE_POINTER ? MOUSE_DRAG : mode() == MOUSE_DRAG ? MOUSE_KEYBOARD : MOUSE_POINTER;
            g_px = g_py = 0;
            say(kModeLines[g_mode]);
        }
        return true;
    default:
        return false;
    }
}

bool tie_controls_mouse(float xrel, float yrel) {
    if (!tie_found() || !tie_in_flight() || mode() == MOUSE_DRAG) {
        return false;
    }
    if (mode() != MOUSE_POINTER) {
        return true;                        // (the keyboard's or a joystick's flight: the mouse steers nothing)
    }
    g_px = fmax(-1.0, fmin(1.0, g_px + xrel / (kPointerSpan / 2)));
    g_py = fmax(-1.0, fmin(1.0, g_py + yrel / (kPointerSpan / 2)));
    return true;
}

void tie_controls_frame() {
    const bool flying = tie_in_flight();
    if (flying != g_wasFlying) {
        g_wasFlying = flying;
        g_px = g_py = 0;                    // (a flight starts with the pointer in the middle)
        g_stepX = g_stepY = 0;
        g_left = g_right = g_up = g_down = false;
    }
    if (!flying) {
        return;
    }
    const bool shift = g_shiftL || g_shiftR;
    if (++g_frames % 2 == 0) {              // (the ramp's step every other frame: full in some 0.3 s at 60)
        g_stepX = key_ramp(g_stepX, (g_right ? 1 : 0) - (g_left ? 1 : 0), shift);
        g_stepY = key_ramp(g_stepY, (g_down ? 1 : 0) - (g_up ? 1 : 0), shift);
    }
    double dx = 0, dy = 0;
    if (mode() == MOUSE_POINTER) {
        dx = fabs(g_px) < kPointerDeadZone ? 0 : g_px;
        dy = fabs(g_py) < kPointerDeadZone ? 0 : g_py;
        dy = -dy;                           // (the pointer above the middle climbs: the mouse's forward dives)
    }
    // the page's pad, while it keeps saying (a pad gone still is no turn)
    if (PIC_FullIndex() - g_stickAt < 500) {
        dx = g_stickX;
        dy = g_stickY;
    }
    if (g_stepX) {
        dx = g_stepX / 9.0;
    }
    if (g_stepY) {
        dy = g_stepY / 9.0;                 // (Up dives, Down climbs: the mouse's forward, negative, dives, as a stick pushed)
    }
    const int mx = mickeys(dx, 17, 191), my = mickeys(dy, 33, 127);
    if (mx || my) {
        Mouse_AddMickeys((float)mx, (float)my);
    }
}

bool tie_controls_marker(double *x, double *y) {
    if (!tie_found() || !tie_in_flight() || mode() != MOUSE_POINTER) {
        return false;
    }
    *x = g_px;
    *y = g_py;
    return true;
}

void tie_controls_set_mouse(int m) {
    g_mode = m >= 0 && m < 4 ? m : MOUSE_POINTER;
    g_px = g_py = 0;
    wclog(1, "TIE: flight controls: %s", kModeNames[g_mode]);
}

void tie_controls_stick(double x, double y) {
    g_stickX = fmax(-1.0, fmin(1.0, x));
    g_stickY = fmax(-1.0, fmin(1.0, y));
    g_stickAt = PIC_FullIndex();
}

}  // namespace wc
