/* flight.h: the flight (flight.c): the loop's C side, the simulation, the
 * cockpit's state and the drawing benchmark (docs/engine.md, "Flight") */
#ifndef FLIGHT_H
#define FLIGHT_H

#include "rvdata.h"
#include "lightfield.h"

/* the flight camera's focal length, px on the 640-wide screen: +-45 degrees across, as the old game's camera (160 on its
 * 320-wide screen, measured 2026-11-02 from its rocks' screen places); ours was 400, +-39 (the user's C: the wider
 * view).  The ships' art keeps its own reference (their full size at a focal length of 400: scale_of); the
 * bar's window keeps 400 (WIN_FOCAL); the stars are placed with the same in lowlevel.asm (STAR_FOCAL) */
#define FLIGHT_FOCAL 320

/* flies the mission: loads its assets, runs the loop in RVHOOK (rvflight.inc)
 * until missionStatus is set, frees them; returns the final missionStatus */
int flight_fly(const RvMission __far *m);
int flight_simulator(const RvMission __far *m);   /* the same in the simulator: none of the flight loop's hooks */

/* a room's window on space (flight.c): the room's window rows (RvRoom.win_*) show the flight's stars and a far thing, with
 * ships going by, or asteroids; seed: what is out there (the system's number).  Open once the room is entered (its level is taken: what
 * it loads is given back with the room's), draw as the room's underlay, tick once a pass of the game's loop, close
 * as the room is left (g_scene.leave) */
enum { WINDOW_SKY, WINDOW_ROCKS, WINDOW_EDGE, WINDOW_EMPTY };   /* what's outside: RvNode.outside, campaign.csv's (rvdata.py) */
void flight_window_open(int room, unsigned seed, int outside);
void flight_window_close(void);
void flight_window_tick(void);
void flight_window_draw(int y0, int y1);

#ifdef RV_TESTS
/* the benchmark's result, after a flight: sprites a frame at 30 fps, and the
 * sprite's size (the test build, RVT.EXE) */
int flight_bench_result(int *w, int *h);
#endif

/* --- where a ship is on a screen: targeting, its lock, the cockpit's animations --- */

/* a camera: where it is, its frame, its projection and the part of the screen it shows */
typedef struct {
    LfVec pos;                          /* positions (256 a km) */
    LfBasis b;                          /* front, right, up (256 = 1.0) */
    int16_t cx, cy, focal;              /* the view's middle on the screen, the focal length (px) */
    int16_t vx0, vy0, vx1, vy1;         /* the part of the screen it shows: vx0 <= x < vx1, vy0 <= y < vy1 */
} RvCamera;

/* a ship as a camera sees it */
typedef struct {
    int16_t x0, y0, x1, y1;             /* the box of its sprite as drawn: x0 <= x < x1, y0 <= y < y1 (pixels); when it
                                           would not be drawn (too small, its sprite not in), the square below */
    int16_t sx, sy;                     /* its middle (its origin) on the screen */
    int16_t half;                       /* half its size there (px): (sx, sy) +- half is a box that does not change as
                                           it turns (lock brackets) */
    int16_t dir;                        /* the way to it on the screen, 0..1023 clockwise from up, even behind the
                                           camera (an arrow at the view's edge when it is off it) */
    int32_t X, Y, Z;                    /* it from the camera: right, up, ahead (sixteenths of a km) */
    uint8_t ahead;                      /* in front of the camera (else only dir, X, Y, Z mean anything) */
    uint8_t visible;                    /* ahead and its box touches the camera's part of the screen */
    uint8_t exact;                      /* drawn exactly this frame (the box the turned image's) */
    uint8_t pad;
} EntBox;

void flight_camera(RvCamera *cam);      /* the player's view: the camera the flight is drawn with */
int16_t flight_ent_box(int16_t slot, const RvCamera *cam, EntBox *out);   /* 0: no ship in that slot */

#endif
