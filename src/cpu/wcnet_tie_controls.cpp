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

// Test aid (the video rig's pilot, the key script's !tiesteer=; scripts/tfnet-video.py): flown through the game's own
// controls only, as a person would (the user, 2026-11-07: "it has to look like a natural flight that keeps viewers
// feeling like they could do the same thing with casual banks and turns, no jittery stuff"):
//   the target: the nearest of the ship indexes (or the nearest mine, a static object), kept while it lives unless
//     another is less than half as far, looked at once a second;
//   far off the nose (20 degrees or more), a pilot's turn: bank towards it with the roll keys, the way chosen kept
//     until it is above the nose, and pull, the more the further off, with a little rudder its way;
//   near the nose, the fine aim at its lead point: the stick by the error (full at 20 degrees), nothing within 0.7
//     degree (0.25 on a still one);
//   the stick eased (0.05 of its travel a frame at most) and given as mickeys from nothing: below the game's least
//     deflection that least is given now and then, so a slow drift is followed slowly, not by turns and stops;
//   the throttle by the game's keys, pressed on a change only: full while far, the target's speed near (on its tail;
//     a kilometre short of a still one), Backspace and Enter;
//   too near (150 m, 200 for a still one): up and away 1.2 s, straight on 1.6 s;
//   the fire button held while the point is within 2 degrees and the range, no craft of ours in the line.
struct Steer {
    bool on;
    int first, last;
    bool any;
    double km;
    int target;
    double pickedAt;
    bool firing;
    double breakUntil;      // (too near: climbing away until then)
    int throttle;           // 1 full (Backspace), 2 the target's speed (Enter): pressed on a change only
    int tap;                // a key pressed last frame, let go this one
    int nth;                // 0 the nearest, 1 the second nearest (two pilots apart)
    bool mines;             // the mines (static objects) instead of craft
    double sx, sy;          // the stick as eased
    double accX, accY;      // (below the game's least deflection: what is owed)
    int rolling, rollHeld;  // banking towards the target (1 right, -1 left), the roll key held
} g_steer;

int steer_mickeys(double v, int least, double &acc) {
    if (fabs(v) >= least) {
        acc = 0;
        return (int)lround(v);
    }
    acc += fabs(v);
    if (acc < least) {
        return 0;
    }
    acc -= least;
    return v < 0 ? -least : least;
}

void steer_tap(KBD_KEYS key) {
    add_key(key, true);
    g_steer.tap = (int)key;
}

void steer_frame() {
    const double now = PIC_FullIndex();
    if (g_steer.tap) {
        add_key((KBD_KEYS)g_steer.tap, false);
        g_steer.tap = 0;
    }
    if (g_steer.target < 0 || now - g_steer.pickedAt > 1000) {
        // (kept while it lives and nothing else is less than half as far)
        const int o0 = g_steer.mines ? tie_nearest_mine() : tie_nearest(g_steer.first, g_steer.last, g_steer.any, g_steer.nth);
        double r0 = 0, c0 = 0, k0 = -1, r1 = 0, c1 = 0, k1 = -1;
        const bool alive = g_steer.target >= 0 && tie_aim(g_steer.target, &r0, &c0, &k0) && tie_alive(g_steer.target);
        const int o = alive && o0 >= 0 && tie_aim(o0, &r1, &c1, &k1) && k1 > k0 / 2 ? g_steer.target : o0;
        g_steer.pickedAt = now;
        if (o != g_steer.target && o >= 0) {
            tie_target(o);
            wclog(2, "TIE: the steering aid's target obj %d", o);
        }
        g_steer.target = o;
    }
    double right = 0, climb = 0, km = -1, wantX = 0, wantY = 0;
    int roll = 0;
    if (g_steer.target < 0 || !tie_aim(g_steer.target, &right, &climb, &km)) {
        g_steer.target = -1;
    } else if (now < g_steer.breakUntil || km < (tie_speed(g_steer.target) > 0 ? 0.15 : 0.2)) {
        if (now >= g_steer.breakUntil) {
            g_steer.breakUntil = now + 2800;    // (too near: up and away for 1.2 s, straight on 1.6 s, then at it again)
        }
        wantY = g_steer.breakUntil - now > 1600 ? 1.0 : 0;
        g_steer.rolling = 0;
        right = climb = 1;                      // (no fire meanwhile)
    } else {
        // (the throttle: full while far, the target's speed near: on its tail, or still before a still one)
        // (a still one, a mine: stopped a kilometre off, shot from there)
        const bool still = g_steer.mines || tie_speed(g_steer.target) <= 0;
        const int want = km > (still ? 1.5 : 1.2) ? 1 : km < (still ? 1.05 : 0.7) ? 2 : g_steer.throttle;
        if (want != g_steer.throttle && !g_steer.tap) {
            steer_tap(want == 1 ? KBD_backspace : KBD_enter);
            g_steer.throttle = want;
        }
        // (where it is around the nose: theta off it, phi round it from straight up, right positive)
        const double f = cos(climb) * cos(right), sd = cos(climb) * sin(right), u = sin(climb);
        const double theta = acos(fmax(-1.0, fmin(1.0, f))), phi = atan2(sd, u);
        if (!g_steer.rolling && theta > 0.35 && fabs(phi) > 0.44) {
            g_steer.rolling = phi > 0 ? 1 : -1;     // (far off the nose and not above it: bank towards it, the way
        }                                           // chosen kept until it is above, however it swings round...)
        if (g_steer.rolling && (fabs(phi) < 0.14 || theta < 0.2)) {
            g_steer.rolling = 0;
        }
        roll = g_steer.rolling;
        if (theta > 0.2) {
            // (... and pull, the more the further off, some rudder its way: a pilot's turn)
            const double k = fmin(1.0, theta / 0.6);
            wantY = k * fmax(0.0, cos(phi));
            wantX = 0.35 * k * sin(phi);
        } else {
            // (near the nose: the fine aim, nothing within 0.7 degree)
            const double dead = still ? 0.004 : 0.012;
            wantX = fabs(right) < dead ? 0 : fmax(-1.0, fmin(1.0, right / 0.35));
            wantY = fabs(climb) < dead ? 0 : fmax(-1.0, fmin(1.0, climb / 0.35));
        }
        static double loggedAt;
        if (now - loggedAt > 500) {
            loggedAt = now;
            wclog(2, "TIE: the steering aid: obj %d %.2f km, %.1f deg off, %.0f round, roll %d, stick %.2f %.2f", g_steer.target, km,
                  theta * 57.3, phi * 57.3, roll, g_steer.sx, g_steer.sy);
        }
    }
    // (the stick eased, 0.05 a frame at most: no snatching)
    g_steer.sx += fmax(-0.05, fmin(0.05, wantX - g_steer.sx));
    g_steer.sy += fmax(-0.05, fmin(0.05, wantY - g_steer.sy));
    if (wantX == 0 && fabs(g_steer.sx) < 0.03) g_steer.sx = 0;
    if (wantY == 0 && fabs(g_steer.sy) < 0.03) g_steer.sy = 0;
    // (the stick as mickeys from nothing, not from the game's least deflection: below it that least is given now and
    // then, so a slow drift is followed slowly, not by turns at the least rate and stops)
    const int mx = steer_mickeys(g_steer.sx * 191, 17, g_steer.accX), my = steer_mickeys(g_steer.sy * 127, 33, g_steer.accY);
    if (mx || my) {
        Mouse_AddMickeys((float)mx, (float)my);
    }
    if (roll != g_steer.rollHeld) {
        if (g_steer.rollHeld) add_key(g_steer.rollHeld > 0 ? KBD_right : KBD_left, false);
        if (roll) add_key(roll > 0 ? KBD_right : KBD_left, true);
        g_steer.rollHeld = roll;
    }
    const bool fire = g_steer.target >= 0 && km >= 0 && km <= g_steer.km && fabs(right) < 0.035 && fabs(climb) < 0.035 &&
                      !tie_friend_in_line(right, climb, km);
    if (fire != g_steer.firing) {
        if (fire) Mouse_ButtonPressed(0);
        else Mouse_ButtonReleased(0);
        g_steer.firing = fire;
    }
}

void tie_controls_frame() {
    const bool flying = tie_in_flight();
    if (g_steer.on && flying) {
        steer_frame();
    }
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

void tie_controls_steer(const std::string &spec) {
    if (spec == "off") {
        if (g_steer.firing) Mouse_ButtonReleased(0);
        if (g_steer.rollHeld) add_key(g_steer.rollHeld > 0 ? KBD_right : KBD_left, false);
        g_steer.rollHeld = g_steer.rolling = 0;
        g_steer.on = g_steer.firing = false;
        wclog(1, "TIE: the steering aid off");
        return;
    }
    g_steer.mines = spec.compare(0, 5, "mines") == 0;
    int first = 5, last = -1;
    char rest[32] = { 0 };
    sscanf(spec.c_str(), "%d-%d", &first, &last);
    if (last < 0) last = first;
    const size_t c = spec.find(':');
    g_steer.km = c != std::string::npos ? atof(spec.c_str() + c + 1) : 1.0;
    if (g_steer.km <= 0) g_steer.km = 1.0;
    g_steer.any = spec.find(":any") != std::string::npos;
    g_steer.nth = spec.find(":2nd") != std::string::npos ? 1 : 0;
    g_steer.first = first;
    g_steer.last = last;
    g_steer.target = -1;
    g_steer.throttle = 0;
    g_steer.on = true;
    (void)rest;
    wclog(1, "TIE: the steering aid at ship indexes %d..%d%s, firing within %.2f km", first, last, g_steer.any ? " (any)" : "", g_steer.km);
}

void tie_controls_stick(double x, double y) {
    g_stickX = fmax(-1.0, fmin(1.0, x));
    g_stickY = fmax(-1.0, fmin(1.0, y));
    g_stickAt = PIC_FullIndex();
}

}  // namespace wc
