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
 *  The mouse is the game's own (drag to steer) and stays so.
 */
#include <stdlib.h>
#include "wcnet_tiedemo.h"
#include "wcnet_tie.h"
#include "wcnet_log.h"
#include "keyboard.h"
#include "mouse.h"
#include "mem.h"

namespace wc {

namespace {

const Bit16u kSpaceConfirm = 0xCD98;    // pstate+0x20: what Space confirms (0 nothing)
const Bit16u kLastButtons = 0xCDA2;     // pstate+0x2A: last frame's buttons
const Bit16u kRightHeld = 0xCDA4;       // pstate+0x2C: frame ticks the right button alone has been held
const Bit16u kRightHold = 0x4E;         // ... from which it is the X-roll (and no click when let go)
// (mickeys: the ramp's first step and its most, the whole stick)
const int kLeastX = 17, kMostX = 96, kLeastY = 25, kMostY = 127, kMostRoll = 69, kWholeX = 191;

bool g_left, g_right, g_up, g_down, g_shiftL, g_shiftR, g_rollLeft, g_rollRight;
int g_stepX, g_stepY, g_rollStep;       // the ramps' steps, -9..9
bool g_rollHeld;                        // the right button held by us (the X-roll)
bool g_spaceFire, g_spaceGame;          // Space held: the fire button; or the game's (a confirmation waits)
bool g_wasFlying;
unsigned g_frames;

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
    default: return false;
    }
}

void tdm_controls_frame() {
    const bool flying = tdm_in_flight();
    if (flying != g_wasFlying) {
        g_wasFlying = flying;
        let_go();
    }
    if (!flying) {
        return;
    }
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
    int mx = 0, my = 0;
    if (g_rollStep) {
        mx = tie_stick_mickeys(g_rollStep / 9.0, kLeastX, shift ? kWholeX : kMostRoll);     // (the X-roll: across)
    } else {
        if (g_stepX) mx = tie_stick_mickeys(g_stepX / 9.0, kLeastX, shift ? kWholeX : kMostX);
        if (g_stepY) my = tie_stick_mickeys(g_stepY / 9.0, kLeastY, kMostY);   // (Up dives: the mouse's forward, negative)
    }
    if (mx || my) {
        Mouse_AddMickeys((float)mx, (float)my);
    }
}

}  // namespace wc
