/* gx.h: the game's side of RVGX, the sprite accelerator (docs/gx.md; the card itself is src/hardware/rvgx.cpp in the
 * emulator).  Only our DOSBox has one: gx_init finds that out, and on any other DOS or DOSBox the game never makes
 * another call and draws every ship itself.  With one, a mission's ships' full-size views stream in by themselves
 * while the game goes on, and each draw that finds its picture there is drawn a pixel a pixel, in no emulated time;
 * a draw that does not (a negative answer) is drawn the usual way, as blocks.  The game's own ` key turns it off and
 * on at any moment, since both pictures are always to hand. */
#ifndef GX_H
#define GX_H

#include "rv.h"

#ifdef RV_HOST                          /* (the native builds draw every ship exactly already: no card) */
#define gx_init() 0
#define gx_present() 0
#define gx_on() 0
#define gx_set(on) ((void)0)
#define gx_want(types, n) ((void)0)
#define gx_toggle() ((void)0)
#define gx_log() ((void)0)
#define gx_draw(type, view, mirror, x, y, scale, roll, y0, y1, xa, xb, ext, ext_y0, ext_rows) ((void)(view), (void)(mirror), -1)
#else

int gx_init(void);                      /* asks whether the card is there (once, after xms_init, before anything else in
                                           XMS but the resident): 1 when it is */
int gx_present(void);
int gx_on(void);                        /* it is there and turned on: draws go to it */
void gx_set(int on);                    /* the setting: the bunk menu's (RVSETUP.CFG) */
void gx_toggle(void);                   /* the ` key: the same setting, for now, at once */

/* the next flight's ship types: their views' bundles asked of the card (not waited for).  With the setting off the
 * list is only kept, for when it is turned on */
void gx_want(const unsigned char *types, int n);

/* a ship type's view drawn by the card (mirror: left to right, for a symmetric ship's other headings), its origin at
 * (x, y), scale 256 = as stored, roll clockwise 1024 a turn, into the run being drawn: rows y0 .. y1-1, columns xa ..
 * xb-1, and each row within its extent when ext is given, as ships_draw takes them.  0 when drawn, negative when not
 * and the caller is to draw it (GX_E_*) */
int gx_draw(unsigned type, int view, int mirror, int x, int y, int scale, int roll, int y0, int y1, int xa, int xb,
            const short __far *ext, int ext_y0, int ext_rows);

void gx_log(void);                      /* the flight's tally, then zero */
#endif

#endif
