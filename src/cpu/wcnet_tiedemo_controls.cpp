/*
 *  The TIE Fighter demo's flight keys (docs/tiedemo.md section 9; the user, 2026-11-15: "the keyboard suffers some of
 *  the same problems as TIECD, so I think that will also need to be improved accordingly"): the CD port's keyboard
 *  (wcnet_tie_controls.cpp, docs/tiefighter.md step 7) for TIEMISS.EXE.
 *
 *  How the demo steers (its input, image 0x3190 and 0x3654): the mouse's motion counters (INT 33h function 0Bh) are a
 *  frame's stick, 128 to a mickey across and 64 up and down (at most 191 and 127 mickeys), nothing within 2,048 across
 *  and 1,536 up and down; the arrows come one a key event from the BIOS's buffer, each a push of the stick that grows
 *  while the same arrow repeats (across 2,112, then 4,096 more an event up to 12,288; up and down 1,600, then 1,536
 *  more up to 8,192): held, the keyboard's own repeat gave one push, nothing for half a second, then the rest
 *  (measured: Left held, the nose still for 0.55 s).  Space fires nothing; Ins and Del do nothing.  The right button
 *  held is the X-roll (the stick across rolls) once it has been held 78 frame ticks (0.33 s, pstate+0x2C against
 *  last frame's buttons at pstate+0x2A, image 0x143C5); let go sooner it is a click, which picks a target.
 *
 *  In flight, so:
 *  - The arrows are taken from the game and steer by RetroVega's ramp (WC1's), as mickeys added at the frame's top: an
 *    axis's step goes one further every other frame its arrow is held, 1 to 9, then 8 and 9 in turn; the opposite arrow
 *    or letting go puts it to 0.  The ramp runs between the game's own first push and its arrows' most (17 to 96
 *    mickeys across, 25 to 127 up and down); Shift with an arrow is the whole stick at once (191 across: the mouse's
 *    most).  Up dives, as a stick pushed forward.
 *  - The keypad's Ins and Del roll on the same ramp: the right button held, marked as held long enough already (so
 *    the roll starts at once and letting go is no click), and the stick across, to 69 mickeys (some 75 degrees a
 *    second in a T/F, the CD port's keyboard roll); Shift the whole stick.  (Up and down is the X-roll's throttle: none
 *    while rolling.)
 *  - Space fires: the mouse's left button, the game's trigger, held while Space is.  The game's own Space confirms what
 *    waits for it (pstate+0x20, DS:CD98: 1 the target it offers, 2 another; its switch's Space, image 0x1458C): then
 *    Space is the game's.
 *  - The mouse and a controller as the CD port has them (the user, 2026-11-15: "point to aim mouse mode should be
 *    default. They can select drag to aim just like on cd version. also joystick should be like cd version too"): the
 *    mode, the pointer and the page's pad are wcnet_tie_controls.cpp's (tie_controls_aim), the mickeys this game's
 *    (17 to 191 across, 25 to 127 up and down).  "Point to aim" leaves the game's bank into a turn out (the demo's
 *    `sub roll,turn` at image 0x15818 stepped over: tdm_controls_no_bank); its right button held rolls with the
 *    pointer once the game takes it as held (0.33 s: a shorter press stays the game's click, a target).  The ring is
 *    drawn round the gunsight's middle, (159.5, 76.5) of the 320 x 200 picture, measured.  F12 in flight cycles aim,
 *    aim banking, drag, the keyboard.
 */
#include <stdlib.h>
#include <math.h>
#include <string>
#include "wcnet_tiedemo.h"
#include "wcnet_tie.h"
#include "wcnet_log.h"
#include "keyboard.h"
#include "mouse.h"
#include "mem.h"
#include "pic.h"

namespace wc {

namespace {

const Bit16u kSpaceConfirm = 0xCD98;    // pstate+0x20: what Space confirms (0 nothing)
const Bit16u kLastButtons = 0xCDA2;     // pstate+0x2A: last frame's buttons
const Bit16u kRightHeld = 0xCDA4;       // pstate+0x2C: frame ticks the right button alone has been held
const Bit16u kRightHold = 0x4E;         // ... from which it is the X-roll (and no click when let go)
// (mickeys: the ramp's first step and its most, the whole stick)
const int kLeastX = 17, kMostX = 96, kLeastY = 25, kMostY = 127, kMostRoll = 69, kWholeX = 191;
const double kSightX = 159.5 / 320, kSightY = 76.5 / 200;     // the gunsight's middle (the TIE cockpit's)

bool g_left, g_right, g_up, g_down, g_shiftL, g_shiftR, g_rollLeft, g_rollRight;
int g_stepX, g_stepY, g_rollStep;       // the ramps' steps, -9..9
bool g_rollHeld;                        // the right button held by us (the X-roll)
bool g_spaceFire, g_spaceGame;          // Space held: the fire button; or the game's (a confirmation waits)
bool g_wasFlying;
unsigned g_frames;

// Test aid (the video rig, !tdmsteer=; the inside video's demo scene, the user: "a really cool fight scene or at least a
// flight scene ... maybe facing each other shooting then going to chase down some of those disabled shuttles"): our
// craft flown at another, the stick eased towards where it will be (its lead point, the CD rig pilot's way: tie_aim)
// and the trigger held while that is within 2 degrees and `km`, at a craft of the other side only.  No roll (this
// build's X-roll waits a third of a second for its button, a click before).
//   !tdmsteer=type:9[:km]   the nearest of that type alive (9 the shuttle, 1 the X-wing, 2 the Y-wing; X-Wing's numbers)
//   !tdmsteer=hostile[:km]  the nearest of the other side
//   !tdmsteer=friend        the nearest of our side but us (the other pilot: a pass at each other), never fired at
//   !tdmsteer=off
//   !tdmsteer=god / nogod   our craft's hull damage kept at none (no pilot shot down mid-scene)
struct DemoSteer {
    int mode;           // 0 off, 1 a type, 2 hostile, 3 friend
    int type;
    double km;          // fire within
    int target;
    double pickedAt;
    double sx, sy;      // the stick as eased
    bool firing;
    double breakUntil;  // too near: pulling up and away until 1.6 s before this, then straight on
} g_ds = { 0, 0, 1.0, -1, 0, 0, 0, false, 0 };
bool g_god;             // !tdmsteer=god / nogod: our craft's hull damage kept at none (a scene's pilots never shot down)

PhysPt obj_at(int o) { return tdm_obj(o); }
bool obj_alive(int o) {
    const PhysPt a = obj_at(o);
    const Bit8u type = mem_readb(a + 4);
    return type && type < 127 && mem_readb(a + 2) == 0 && mem_readw(a + 0x2C) == 0;
}
double obj_km(int a, int b) {
    double d = 0;
    for (int i = 0; i < 3; i++) {
        const double v = (double)(Bit32s)mem_readd(obj_at(a) + 6 + 4 * i) - (Bit32s)mem_readd(obj_at(b) + 6 + 4 * i);
        d += v * v;
    }
    return sqrt(d) / 40960.0;
}

// where `target`'s lead point lies off our nose: right and climb (radians), how far (km)
bool steer_aim(int own, int target, double *right, double *climb, double *km) {
    const PhysPt a = obj_at(own), b = obj_at(target);
    double dx = (double)(Bit32s)mem_readd(b + 6) - (Bit32s)mem_readd(a + 6);
    double dy = (double)(Bit32s)mem_readd(b + 0xA) - (Bit32s)mem_readd(a + 0xA);
    double dz = (double)(Bit32s)mem_readd(b + 0xE) - (Bit32s)mem_readd(a + 0xE);
    *km = sqrt(dx * dx + dy * dy + dz * dz) / 40960.0;
    const double k = 4660.0 / 256.0, vt = (Bit16s)mem_readw(b + 0x26) * k / 32768.0;
    const double vx = vt * (Bit16s)mem_readw(b + 0x3A), vy = vt * (Bit16s)mem_readw(b + 0x3C), vz = vt * (Bit16s)mem_readw(b + 0x3E);
    const double sb = ((Bit16s)mem_readw(a + 0x26) + 1000) * k;
    const double qa = vx * vx + vy * vy + vz * vz - sb * sb, qb = 2 * (dx * vx + dy * vy + dz * vz), qc = dx * dx + dy * dy + dz * dz;
    const double disc = qb * qb - 4 * qa * qc;
    if (qa < 0 && disc >= 0) {
        const double t = (-qb - sqrt(disc)) / (2 * qa);
        if (t > 0 && t < 10) {
            dx += vx * t;
            dy += vy * t;
            dz += vz * t;
        }
    }
    double ax[9];
    for (int i = 0; i < 9; i++) ax[i] = (Bit16s)mem_readw(a + 0x42 + 2 * i) / 32768.0;
    const double mv = ax[0] * (Bit16s)mem_readw(a + 0x3A) + ax[1] * (Bit16s)mem_readw(a + 0x3C) + ax[2] * (Bit16s)mem_readw(a + 0x3E);
    const double sf = mv < 0 ? -1 : 1;
    const double f = sf * (dx * ax[0] + dy * ax[1] + dz * ax[2]), sd = dx * ax[3] + dy * ax[4] + dz * ax[5], u = dx * ax[6] + dy * ax[7] + dz * ax[8];
    *right = atan2(sd, f);
    *climb = atan2(u, sqrt(f * f + sd * sd));
    return true;
}

int steer_pick(int own) {
    const Bit8u side = mem_readb(obj_at(own) + 0x33);
    int best = -1;
    double bestKm = 1e9;
    for (int o = 0; o < tdm::kCraftSlots; o++) {
        if (o == own || !obj_alive(o)) continue;
        const Bit8u type = mem_readb(obj_at(o) + 4), s = mem_readb(obj_at(o) + 0x33);
        const bool want = g_ds.mode == 1 ? type == g_ds.type : g_ds.mode == 2 ? s != side : s == side;
        if (!want) continue;
        const double km = obj_km(own, o);
        if (km < bestKm) {
            bestKm = km;
            best = o;
        }
    }
    return best;
}

void steer_frame(int *mx, int *my) {
    const int own = tdm_player_object();
    const double now = PIC_FullIndex();
    if (own < 0 || !obj_alive(own)) return;
    if (g_ds.target < 0 || !obj_alive(g_ds.target) || now - g_ds.pickedAt > 1500) {
        // (kept while it lives and nothing else is less than half as far: the CD rig pilot's rule, no dithering
        // between the three shuttles of a flight)
        const int o0 = steer_pick(own);
        const bool keep = g_ds.target >= 0 && obj_alive(g_ds.target) && o0 >= 0 &&
                          obj_km(own, o0) > obj_km(own, g_ds.target) / 2;
        const int o = keep ? g_ds.target : o0;
        if (o != g_ds.target) wclog(1, "TIE demo: the steering aid flies at obj %d (type %u)", o, o >= 0 ? mem_readb(obj_at(o) + 4) : 0);
        g_ds.target = o;
        g_ds.pickedAt = now;
    }
    double right = 0, climb = 0, km = -1, wantX = 0, wantY = 0;
    const bool have = g_ds.target >= 0 && steer_aim(own, g_ds.target, &right, &climb, &km);
    bool breaking = false;
    if (have && (now < g_ds.breakUntil || km < 0.25)) {
        // (too near: up and away 1.2 s, straight on 1.6 s, then at it again: a collision kills in one frame, before
        // the god switch's next write, and a pilot down ends the room's flight)
        if (now >= g_ds.breakUntil) g_ds.breakUntil = now + 2800;
        wantY = g_ds.breakUntil - now > 1600 ? 1.0 : 0;
        breaking = true;
    } else if (have) {
        wantX = fmax(-1.0, fmin(1.0, right / 0.35));
        wantY = fabs(right) > 1.2 ? 0.3 : fmax(-1.0, fmin(1.0, climb / 0.35));      // (behind: the turn first)
    }
    g_ds.sx += fmax(-0.06, fmin(0.06, wantX - g_ds.sx));
    g_ds.sy += fmax(-0.06, fmin(0.06, wantY - g_ds.sy));
    *mx = fabs(g_ds.sx) < 0.02 ? 0 : tie_stick_mickeys(g_ds.sx, kLeastX, kWholeX);
    *my = fabs(g_ds.sy) < 0.02 ? 0 : tie_stick_mickeys(g_ds.sy, kLeastY, kMostY);
    const bool hostile = have && mem_readb(obj_at(g_ds.target) + 0x33) != mem_readb(obj_at(own) + 0x33);
    const bool fire = hostile && !breaking && g_ds.mode != 3 && km <= g_ds.km && fabs(right) < 0.035 && fabs(climb) < 0.035;
    if (fire != g_ds.firing) {
        if (fire) Mouse_ButtonPressed(0);
        else Mouse_ButtonReleased(0);
        g_ds.firing = fire;
    }
}

void let_go() {
    if (g_spaceFire) {
        g_spaceFire = false;
        Mouse_ButtonReleased(0);
    }
    if (g_rollHeld) {
        g_rollHeld = false;
        Mouse_ButtonReleased(1);
    }
    g_left = g_right = g_up = g_down = g_rollLeft = g_rollRight = false;
    g_stepX = g_stepY = g_rollStep = 0;
}

}  // namespace

bool tdm_controls_key(int key, bool pressed) {
    if (!tdm_found()) {
        return false;
    }
    switch (key) {
    case KBD_leftshift: g_shiftL = pressed; return false;
    case KBD_rightshift: g_shiftR = pressed; return false;
    default: break;
    }
    const bool flying = tdm_in_flight();
    if (key == KBD_space) {
        if (!pressed) {
            if (g_spaceFire) {
                g_spaceFire = false;
                Mouse_ButtonReleased(0);
                return true;
            }
            const bool game = g_spaceGame;
            g_spaceGame = false;
            return !game && flying;
        }
        if (g_spaceFire) {
            return true;                    // (its repeats)
        }
        if (g_spaceGame || !flying || mem_readb(tdm_ds(kSpaceConfirm))) {
            g_spaceGame = true;
            return false;
        }
        g_spaceFire = true;
        Mouse_ButtonPressed(0);
        return true;
    }
    if (!flying) {
        g_left = g_right = g_up = g_down = g_rollLeft = g_rollRight = false;
        return false;
    }
    switch (key) {
    case KBD_left: g_left = pressed; return true;
    case KBD_right: g_right = pressed; return true;
    case KBD_up: g_up = pressed; return true;
    case KBD_down: g_down = pressed; return true;
    case KBD_kp0: g_rollLeft = pressed; return true;        // Ins
    case KBD_kpperiod: g_rollRight = pressed; return true;  // Del
    case KBD_f12:                                           // the mouse's mode (the CD port's: aim, banking, drag, keys)
        if (pressed) tie_controls_next_mode();
        return true;
    case KBD_q: return true;    // (the game's eject, Q then Space, and Space is our trigger: never passed in flight)
    default: return false;
    }
}

void tdm_controls_frame() {
    const bool flying = tdm_in_flight();
    if (flying != g_wasFlying) {
        g_wasFlying = flying;
        let_go();
        tie_controls_aim_reset();           // (a flight starts with the pointer in the middle)
    }
    if (!flying) {
        return;
    }
    tie_controls_sight(kSightX, kSightY);
    const bool shift = g_shiftL || g_shiftR;
    const int rollDir = (g_rollRight ? 1 : 0) - (g_rollLeft ? 1 : 0);
    if (++g_frames % 2 == 0) {              // (a notch further every other frame: full in some 0.3 s)
        g_stepX = tie_key_ramp(g_stepX, (g_right ? 1 : 0) - (g_left ? 1 : 0), shift);
        g_stepY = tie_key_ramp(g_stepY, (g_down ? 1 : 0) - (g_up ? 1 : 0), shift);
    }
    if (!rollDir) {
        g_rollStep = 0;
    } else if (!g_rollStep || g_frames % 2 == 0) {
        g_rollStep = tie_key_ramp(g_rollStep, rollDir, shift);    // (the press's own frame rolls at once)
    }
    if ((g_rollStep != 0) != g_rollHeld) {
        g_rollHeld = g_rollStep != 0;
        if (g_rollHeld) {
            Mouse_ButtonPressed(1);
            mem_writew(tdm_ds(kLastButtons), 2);    // (held long enough already: the roll at once, no click)
            mem_writew(tdm_ds(kRightHeld), kRightHold);
        } else {
            Mouse_ButtonReleased(1);
        }
    }
    // the pointer's and the page's pad's (the CD port's), the keys' over them
    int mx = 0, my = 0;
    if (tie_controls_aim(kLeastX, kWholeX, kLeastY, kMostY, &mx, &my) &&
        !(mem_readw(tdm_ds(kRightHeld)) >= kRightHold && (mem_readb(tdm_ds(kLastButtons)) & 0xE) == 2)) {
        mx = 0;     // (the pointer's roll: none until the game takes its right button as held, or the motion would turn)
    }
    if (g_rollStep) {
        mx = tie_stick_mickeys(g_rollStep / 9.0, kLeastX, shift ? kWholeX : kMostRoll);     // (the X-roll: across)
        my = 0;
    } else {
        if (g_stepX) mx = tie_stick_mickeys(g_stepX / 9.0, kLeastX, shift ? kWholeX : kMostX);
        if (g_stepY) my = tie_stick_mickeys(g_stepY / 9.0, kLeastY, kMostY);   // (Up dives: the mouse's forward, negative)
    }
    if (g_ds.mode && !g_stepX && !g_stepY && !g_rollStep) {
        steer_frame(&mx, &my);
    }
    if (g_god) {
        const int own = tdm_player_object();
        const PhysPt craft = own >= 0 ? tdm_craft(own) : 0;
        if (craft) mem_writew(craft + tdm::kCraftHull, 0);
    }
    if (mx || my) {
        Mouse_AddMickeys((float)mx, (float)my);
    }
}

void tdm_controls_steer(const std::string &spec) {
    const std::string what = spec.substr(0, spec.find(':'));
    const size_t c = spec.find(':');
    const std::string rest = c == std::string::npos ? "" : spec.substr(c + 1);
    if (what == "god" || what == "nogod") {
        g_god = what == "god";
        wclog(1, "TIE demo: our craft %s", g_god ? "takes no hull damage (the video rig)" : "takes damage again");
        return;
    }
    if (g_ds.firing) {
        Mouse_ButtonReleased(0);
        g_ds.firing = false;
    }
    g_ds.target = -1;
    g_ds.km = 1.0;
    if (what == "type") {
        g_ds.mode = 1;
        g_ds.type = atoi(rest.c_str());
        const size_t c2 = rest.find(':');
        if (c2 != std::string::npos) g_ds.km = atof(rest.c_str() + c2 + 1);
    } else if (what == "hostile") {
        g_ds.mode = 2;
        if (!rest.empty()) g_ds.km = atof(rest.c_str());
    } else if (what == "friend") {
        g_ds.mode = 3;
    } else {
        g_ds.mode = 0;
        g_ds.sx = g_ds.sy = 0;
    }
    wclog(1, "TIE demo: the steering aid: %s", spec.c_str());
}

// At the bank into a turn (user_inputforplane's `sub [roll],turn` at image 0x15818, the turn not the X-roll's): true to
// step over it (the pointer without the bank).
bool tdm_controls_no_bank() {
    return tie_controls_aim_only();
}

}  // namespace wc
