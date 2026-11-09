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
#include <ctype.h>
#include <math.h>
#include <string>
#include "wcnet_tie.h"
#include "wcnet_tie_session.h"
#include "wcnet_log.h"
#include "wc_net.h"
#include "keyboard.h"
#include "mouse.h"
#include "mem.h"
#include "pic.h"
#include "vga.h"

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
int g_rollStep;                     // the roll keys' ramp (the arrows' own): -9..9
double g_rollOwed;                  // ... the roll events it owes, a fraction of one a frame
int g_stepX, g_stepY;
double g_px, g_py;                  // the pointer, -1..1 from the middle (down and right positive)
unsigned g_frames;
bool g_wasFlying;
double g_accX, g_accY;              // the pointer's turns below the game's least: what is owed (pointer_mickeys)
double g_sightX = 0.5, g_sightY = 0.375;    // the gunsight's middle, a share of the screen (the game's projection's)
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

// The pointer's deflection (-1..1) as the share of the game's full turn: nothing within the dead zone, then the square
// of the way out (the user, 2026-11-09: "moving the mouse just a bit away from the center results in a huge turn
// already--it's hard to fine tune"): a quarter of the turn with the pointer half way, the whole at the edge.
double pointer_curve(double p) {
    const double a = fabs(p);
    if (a < kPointerDeadZone) {
        return 0;
    }
    const double e = (a - kPointerDeadZone) / (1.0 - kPointerDeadZone);
    return p < 0 ? -e * e : e * e;
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
    case KBD_kp0:                           // Ins: roll as the game's Left arrow does (tie_controls_frame)
        g_rollLeft = pressed;
        return true;
    case KBD_kpperiod:                      // Del: roll as its Right arrow does
        g_rollRight = pressed;
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

void throttle_to(int want) {
    if (want != g_steer.throttle && !g_steer.tap) {
        steer_tap(want == 1 ? KBD_backspace : want == 2 ? KBD_enter : KBD_backslash);
        g_steer.throttle = want;
    }
}

// The pilot's hands, from where the point to fly at lies off the nose (right, climb, radians) and how far (km): a bank
// and a pull far off the nose, the fine aim near it, the stick eased and given as mickeys from nothing, the roll keys,
// the fire button (allowed: `fireOk`, within `fireKm` and 2 degrees, no friend in the line); within `breakKm` (0: never)
// up and away 1.2 s and straight on 1.6 s.  !have: the stick let go.
void hands(bool have, double right, double climb, double km, bool still, bool fireOk, double fireKm, double breakKm) {
    const double now = PIC_FullIndex();
    double wantX = 0, wantY = 0;
    int roll = 0;
    if (have && (now < g_steer.breakUntil || (breakKm > 0 && km < breakKm))) {
        if (now >= g_steer.breakUntil) {
            g_steer.breakUntil = now + 2800;    // (too near: up and away for 1.2 s, straight on 1.6 s, then at it again)
        }
        wantY = g_steer.breakUntil - now > 1600 ? 1.0 : 0;
        g_steer.rolling = 0;
        fireOk = false;
    } else if (have) {
        // (where it is around the nose: theta off it, phi round it from straight up, right positive)
        const double f = cos(climb) * cos(right), sd = cos(climb) * sin(right), u = sin(climb);
        const double theta = acos(fmax(-1.0, fmin(1.0, f))), phi = atan2(sd, u);
        if (!g_steer.rolling && theta > 0.35 && fabs(phi) > 0.44) {
            g_steer.rolling = phi > 0 ? 1 : -1;     // (far off the nose and not above it: bank towards it, the way
        }                                           // chosen kept until it is above, however it swings round...)
        // (... and stopped once it is within some 15 degrees of overhead or past it: the roll turns more than a narrow
        // window a frame, which it would roll past for ever; not judged near straight below, where the angle flips)
        if (g_steer.rolling && (theta < 0.2 || (fabs(phi) < 2.6 && ((g_steer.rolling > 0 && phi < 0.25) ||
                                                                   (g_steer.rolling < 0 && phi > -0.25))))) {
            g_steer.rolling = 0;
        }
        roll = g_steer.rolling;
        if (theta > 0.2) {
            // (... and pull once it is above the nose, the more the further off, some rudder its way when not banking:
            // a pilot's turn)
            const double k = fmin(1.0, theta / 0.6);
            wantY = cos(phi) > 0.5 ? k * cos(phi) : 0;
            // (banking: the stick across rolls while the second button is held, the game's own X-roll, a bank the
            // faster the further round it is)
            wantX = roll ? roll * fmin(1.0, 0.35 + fabs(phi) / 2.0) : 0.35 * k * sin(phi);
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
    } else {
        g_steer.rolling = 0;
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
    // (the roll: the game's X-roll, the second button held while banking; TIE's arrow keys roll a step a key event, so
    // a key held rolls next to nothing)
    if ((roll != 0) != (g_steer.rollHeld != 0)) {
        if (roll) Mouse_ButtonPressed(1);
        else Mouse_ButtonReleased(1);
    }
    g_steer.rollHeld = roll;
    const bool fire = have && fireOk && km >= 0 && km <= fireKm && fabs(right) < 0.035 && fabs(climb) < 0.035 &&
                      !tie_friend_in_line(right, climb, km);
    if (fire != g_steer.firing) {
        if (fire) Mouse_ButtonPressed(0);
        else Mouse_ButtonReleased(0);
        g_steer.firing = fire;
    }
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
    double right = 0, climb = 0, km = -1;
    const bool have = g_steer.target >= 0 && tie_aim(g_steer.target, &right, &climb, &km);
    if (!have) {
        g_steer.target = -1;
    }
    const bool still = have && (g_steer.mines || tie_speed(g_steer.target) <= 0);
    if (have) {
        // (the throttle: full while far, the target's speed near: on its tail, or still before a still one, a
        // kilometre off a mine, shot from there)
        throttle_to(km > (still ? 1.5 : 1.2) ? 1 : km < (still ? 1.05 : 0.7) ? 2 : g_steer.throttle);
    }
    hands(have, right, climb, km, still, have, g_steer.km, have ? (still ? 0.2 : 0.15) : 0);
}

// The DEMO pilot's wingman (the user, 2026-11-08: "if I sign in with pilot DEMO for tie fighter can you give my controls
// to the mind you created to record the demo reels? that way I can do two player tests where I fly one and the other
// flies the other... please make it work with both host and guest... ideally it at least works well enough to stay out of
// trouble and help where possible"; then "can we not use the CALLSIGN field on the webpage... could pass a flag to
// dosbox?").  A room's callsign with DEMO in it (any case; WCCALLSIGN, as Wing Commander's CHEAT is read; the rigs:
// WCNET_TIE_DEMO=1) has that machine's craft flown by the video rig's pilot
// (the same hands: banks, pulls, an eased stick, the game's own keys), deciding each frame:
//   out of trouble first: away from a mine within 1.6 km, and from a capital ship, container or platform its own size
//     and 0.35 km off;
//   then help, the first of: an enemy at the other pilot (its AI's target), the other pilot's own target if hostile, an
//     enemy at us, an enemy within 3 km of the other pilot (or of us, alone); hostile: a ship of the mission's other side
//     (0 and 1, the Rebels' and the Empire's) or one attacking our side, so neutrals to inspect are left alone;
//   else on the other pilot's wing, 0.35 km out and a little behind, at its speed (Enter, the other pilot its target).
// Its fire only at a hostile target, within 1 km and 2 degrees, no friend in the line.
struct Mind {
    int state;          // -1 not yet decided this flight, 0 off, 1 on
    double flightAt;
    int target, prio, mode;
    double pickedAt;
} g_mind = { -1, 0, -1, 9, 0, 0 };

const double kKm = 40960.0;

bool mind_hostile(int o, const TieObjInfo &t, int ourSide) {
    if (!t.alive || t.type < 1 || t.type > 25 || t.side == ourSide || o == tie_player_object()) return false;
    if ((ourSide == 1 && t.side == 0) || (ourSide == 0 && t.side == 1)) return true;
    TieObjInfo v;
    return t.aiTarget >= 0 && t.aiTarget < 120 && tie_obj_info(t.aiTarget, &v) && v.side == ourSide;
}

double mind_km(const TieObjInfo &a, const TieObjInfo &b) {
    return sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)) / kKm;
}

void mind_said(int mode) {
    static int said = -1;
    if (mode != said) {
        said = mode;
        static const char *const kModes[] = { "idle", "out of trouble", "attacking", "on the other pilot's wing" };
        wclog(1, "TIE: the DEMO wingman: %s", kModes[mode & 3]);
    }
}

void mind_frame() {
    const double now = PIC_FullIndex();
    if (g_steer.tap) {
        add_key((KBD_KEYS)g_steer.tap, false);
        g_steer.tap = 0;
    }
    if (now - g_mind.flightAt < 3000) {
        return;     // (the arrival, the opening's lines)
    }
    const int own = tie_player_object();
    TieObjInfo me;
    if (own < 0 || !tie_obj_info(own, &me) || !me.alive) {
        hands(false, 0, 0, 0, false, false, 0, 0);
        return;
    }
    int ptarget = 0xFFFF;
    const int partner = tie_session_partner(&ptarget);
    TieObjInfo pal;
    const bool havePal = partner >= 0 && tie_obj_info(partner, &pal) && pal.alive;
    int mode = 0;
    double right = 0, climb = 0, km = -1;
    // out of trouble: a mine near, a big ship near
    const int mine = tie_nearest_mine();
    double mr = 0, mc = 0, mk = -1;
    if (mine >= 0 && tie_aim(mine, &mr, &mc, &mk, false) && mk < 1.6) {
        right = mr > 0 ? mr - M_PI : mr + M_PI;
        climb = -mc;
        km = 2;
        mode = 1;
    }
    for (int o = 0; o < 120 && !mode; o++) {
        TieObjInfo t;
        if (o == own || !tie_obj_info(o, &t) || !t.alive || t.type < 26 || t.type == 75) continue;
        const double d = mind_km(me, t), size = t.radius / kKm;
        if (d - size < 0.35) {
            double r = 0, c = 0;
            tie_aim_dir(t.x - me.x, t.y - me.y, t.z - me.z, &r, &c);
            if (fabs(r) < 1.2) {        // (ahead of us: turn away; behind, flying on takes us off)
                right = r > 0 ? r - M_PI : r + M_PI;
                climb = -c;
                km = 2;
                mode = 1;
            }
        }
    }
    if (mode == 1) {
        throttle_to(1);
        hands(true, right, climb, km, false, false, 0, 0);
        g_mind.mode = 1;
        mind_said(1);
        return;
    }
    // help: the target, picked again each second (kept unless a more pressing one has come)
    if (now - g_mind.pickedAt > 1000 || g_mind.target < 0) {
        g_mind.pickedAt = now;
        int best = -1, bestPrio = 9;
        double bestD = 1e30;
        for (int o = 0; o < 32; o++) {
            TieObjInfo t;
            if (!tie_obj_info(o, &t) || !mind_hostile(o, t, me.side)) continue;
            int prio = 9;
            if (havePal && t.aiTarget == partner) prio = 1;
            else if (havePal && o == ptarget) prio = 2;
            else if (t.aiTarget == own) prio = 3;
            else if (mind_km(havePal ? pal : me, t) < 3.0) prio = 4;
            if (prio > 4) continue;
            const double d = mind_km(me, t);
            if (prio < bestPrio || (prio == bestPrio && d < bestD)) {
                best = o;
                bestPrio = prio;
                bestD = d;
            }
        }
        TieObjInfo cur;
        const bool keep = g_mind.target >= 0 && tie_obj_info(g_mind.target, &cur) && mind_hostile(g_mind.target, cur, me.side) &&
                          g_mind.prio <= bestPrio;
        if (!keep) {
            if (best != g_mind.target && best >= 0) {
                tie_target(best);
                wclog(2, "TIE: the DEMO wingman's target obj %d (type %d, %s)", best, -1, bestPrio == 1 ? "at the other pilot"
                      : bestPrio == 2 ? "the other pilot's target" : bestPrio == 3 ? "at us" : "near");
            }
            g_mind.target = best;
            g_mind.prio = bestPrio;
        }
    }
    if (g_mind.target >= 0 && tie_aim(g_mind.target, &right, &climb, &km)) {
        g_steer.target = g_mind.target;
        throttle_to(km > 1.2 ? 1 : km < 0.7 ? 2 : g_steer.throttle);
        hands(true, right, climb, km, false, true, 1.0, 0.15);
        g_mind.mode = 2;
        mind_said(2);
        return;
    }
    g_mind.target = -1;
    if (havePal) {
        // on its wing: 0.35 km out to its side, 0.15 km behind; near the spot, its heading
        const double px = pal.x + (pal.sx * 0.35 - pal.fx * 0.15) * kKm, py = pal.y + (pal.sy * 0.35 - pal.fy * 0.15) * kKm,
                     pz = pal.z + (pal.sz * 0.35 - pal.fz * 0.15) * kKm;
        const double dx = px - me.x, dy = py - me.y, dz = pz - me.z, d = sqrt(dx * dx + dy * dy + dz * dz) / kKm;
        if (g_mind.mode != 3) {
            tie_target(partner);    // (Enter matches the other pilot's speed)
        }
        if (d < 0.15) tie_aim_dir(pal.fx, pal.fy, pal.fz, &right, &climb);
        else tie_aim_dir(dx, dy, dz, &right, &climb);
        throttle_to(d > 1.0 ? 1 : d < 0.4 ? 2 : g_steer.throttle);
        hands(true, right, climb, d, false, false, 0, 0);
        g_mind.mode = 3;
        mind_said(3);
        static double loggedAt;
        if (now - loggedAt > 2000) {
            loggedAt = now;
            wclog(2, "TIE: the DEMO wingman on obj %d's wing, %.2f km from its spot", partner, d);
        }
        return;
    }
    hands(false, 0, 0, 0, false, false, 0, 0);
    g_mind.mode = 0;
    mind_said(0);
}

void mind_stop() {
    if (g_steer.firing) Mouse_ButtonReleased(0);
    if (g_steer.rollHeld) Mouse_ButtonReleased(1);
    if (g_steer.tap) add_key((KBD_KEYS)g_steer.tap, false);
    g_steer.firing = false;
    g_steer.rollHeld = g_steer.rolling = 0;
    g_steer.tap = 0;
    g_steer.sx = g_steer.sy = 0;
    g_steer.throttle = 0;
    g_mind.target = -1;
    g_mind.mode = 0;
}

// Test aid (the key script's !tiegod=1, 0 off): the game's damage calls on our craft skipped (the session's damage gate,
// tie_session_damage): a rig's measurement of speed and reach, not of a fight; never the page's.
bool g_god;

void tie_controls_god(bool on) {
    g_god = on;
    wclog(1, "TIE: the no-damage aid %s", on ? "on" : "off");
}

bool tie_controls_godly() {
    return g_god;
}

// The gunsight's middle as a share of the screen: the projection's centre (transfm2_getscreenx and y: displaycorner's
// column and line, half the view's width and depth, transfm2_screenyoffset), read at the frame's top (the CMD's picture
// in a picture has its own while it is drawn).
void sample_sight() {
    double w = 320, h = 200;
    if (vga.mode == M_LIN8) {
        w = (double)vga.draw.width;
        h = (double)vga.draw.height;
    } else if (vga.mode != M_VGA) {
        return;
    }
    const Bit32u cols = tie_r32(0xD4C04), lines = tie_r32(0xD4C00);
    const int halfW = tie_r16(0xD4C18), halfD = tie_r16(0xD4C16), yoff = (Bit32s)tie_r32(0xEC170);
    if (w < 1 || h < 1 || cols > 4096 || lines > 4096) {
        return;
    }
    const double sx = (cols + halfW) / w, sy = (lines + halfD + yoff) / h;
    if (sx < 0.2 || sx > 0.8 || sy < 0.1 || sy > 0.8) {
        return;     // (not a view's)
    }
    if (fabs(sx - g_sightX) > 0.002 || fabs(sy - g_sightY) > 0.002) {
        wclog(1, "TIE: the gunsight's middle at %u+%d, %u+%d%+d of %.0fx%.0f: %.3f, %.3f of the screen", (unsigned)cols, halfW,
              (unsigned)lines, halfD, yoff, w, h, sx, sy);
    }
    g_sightX = sx;
    g_sightY = sy;
}

void tie_controls_frame() {
    const bool flying = tie_in_flight();
    if (g_steer.on && flying) {
        steer_frame();
    }
    // (a DEMO callsign: decided at each flight's start, its wingman's mind on while it flies)
    if (flying && g_mind.state < 0) {
        // (the room's callsign, the page's Callsign box: WCCALLSIGN, as Wing Commander's CHEAT is read)
        const char *cs = getenv("WCCALLSIGN"), *env = getenv("WCNET_TIE_DEMO");
        std::string callsign = cs ? cs : "";
        for (size_t i = 0; i < callsign.size(); i++) callsign[i] = (char)toupper((unsigned char)callsign[i]);
        g_mind.state = callsign.find("DEMO") != std::string::npos || (env && env[0] == '1') ? 1 : 0;
        wclog(1, "TIE: %s flies (the pilot %s)%s", cs ? cs : "we", tie_pilot_name().c_str(),
              g_mind.state ? ": the DEMO wingman flies its craft" : "");
        g_mind.flightAt = PIC_FullIndex();
        if (g_mind.state == 1) {
            mind_stop();
            say("DEMO flies itself: a wingman stays out of trouble and helps the other pilot");
        }
    } else if (!flying && g_mind.state >= 0) {
        if (g_mind.state == 1) mind_stop();
        g_mind.state = -1;
    }
    if (flying && g_mind.state == 1 && !g_steer.on) {
        mind_frame();
    }
    // (the keypad's Ins and Del, a controller's roll buttons.  The game rolls a step a key event, its own arrows' as
    // each frame's: held, the keyboard's own repeat gave one step, then nothing for half a second, then some 30 a second.
    // Now the arrows' own ramp (RetroVega's, WC1's: a notch further every other frame, 1 to 9, Shift 9 at once; the user:
    // "Do we need the retrovega or privateer style key ramp for a few frames?"): an event on the frame of the press, then
    // the ramp's ninths of one a frame, full in some 0.3 s; each a press and its release, so that the keyboard's repeat
    // adds none)
    const int rollDir = (g_rollRight ? 1 : 0) - (g_rollLeft ? 1 : 0);
    if (!flying || !rollDir) {
        g_rollStep = 0;
        g_rollOwed = 1;                     // (the next press rolls at once)
    } else {
        if (!g_rollStep || g_frames % 2 == 0) {
            g_rollStep = key_ramp(g_rollStep, rollDir, g_shiftL || g_shiftR);
        }
        g_rollOwed += abs(g_rollStep) / 9.0;
        if (g_rollOwed >= 1) {
            g_rollOwed -= 1;
            const KBD_KEYS arrow = rollDir < 0 ? KBD_left : KBD_right;
            add_key(arrow, true);
            add_key(arrow, false);
        }
    }
    if (flying != g_wasFlying) {
        g_wasFlying = flying;
        g_accX = g_accY = 0;
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
    sample_sight();
    // this frame's mickeys: the pointer's from nothing (below the game's least, that least now and then: a slow turn
    // for a fine aim, the game's own easing of its stick smoothing it), a pad's and the arrows' from the least
    int mx = 0, my = 0;
    if (mode() == MOUSE_POINTER) {
        // (the pointer above the middle climbs: the mouse's forward dives)
        mx = steer_mickeys(pointer_curve(g_px) * 191, 17, g_accX);
        my = steer_mickeys(-pointer_curve(g_py) * 127, 33, g_accY);
    }
    // the page's pad, while it keeps saying (a pad gone still is no turn)
    if (PIC_FullIndex() - g_stickAt < 500) {
        mx = mickeys(g_stickX, 17, 191);
        my = mickeys(g_stickY, 33, 127);
    }
    if (g_stepX) {
        mx = mickeys(g_stepX / 9.0, 17, 191);
    }
    if (g_stepY) {
        my = mickeys(g_stepY / 9.0, 33, 127);   // (Up dives, Down climbs: the mouse's forward, negative, dives, as a stick pushed)
    }
    if (mx || my) {
        Mouse_AddMickeys((float)mx, (float)my);
    }
}

// The game's stick, scaled by the craft's turn rate (user_inputforplane, at its `imul eax,ebx; sar eax,15`): the axis is
// read unsigned there (xor eax,eax; mov ax,inputdelta), so a stick left or down came out as 2 x scale - |delta| x
// scale / 32768, kept as 16 bits.  That is the signed product only for a scale near 0x8000, the Empire's fighters' (a
// T/F's roll 0x3000 and pitch 0x1400 give 0x7FFF): a craft slower to roll or pitch was given a wrong turn to the left
// and down, the Z-95 (roll 0x2000: scale 0x5555) a hard left for the least left and, past some two thirds of the stick,
// a hard right (the user, 2026-11-09: "when I go left with either arrows it turns left then rapidly turns right"), the
// shuttle and the gunboat's pitch a bias down of 4,096 and more.  The product signed, as OpenTIE's port has it
// (TieAxisInputHook_Scale): the same both ways.
int tie_controls_axis(bool across, int delta, int scale, int game) {
    const int fixed = scale * delta / 32768;
    static int told[2];
    if ((Bit16s)game != fixed && abs((Bit16s)game - fixed) > 64 && told[across ? 0 : 1] < 1) {
        told[across ? 0 : 1]++;
        wclog(1, "TIE: the stick %s %d, scale %04x: the game's %d, signed %d", across ? "across" : "up and down", delta, scale,
              (Bit16s)game, fixed);
    }
    return fixed;
}

bool tie_controls_marker(double *x, double *y) {
    if (!tie_found() || !tie_in_flight() || mode() != MOUSE_POINTER) {
        return false;
    }
    // (round the gunsight's middle, where the stick is still: the user, 2026-11-09: "the center of the mouse is the bottom
    // of the crosshair instead of the dot in the center...but the blasters do fire against the dot")
    *x = fmax(0.01, fmin(0.99, g_sightX + 0.42 * g_px));
    *y = fmax(0.01, fmin(0.99, g_sightY + 0.30 * g_py));
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
        if (g_steer.rollHeld) Mouse_ButtonReleased(1);
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
