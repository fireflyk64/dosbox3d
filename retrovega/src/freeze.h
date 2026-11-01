/* freeze.h: the freeze watch (freeze.c): the main loop says it is alive; if it stops, the music's interrupt takes down
 * where the program is and writes it to RVLOG.TXT ("FREEZE:"), and the game goes on as it was (a very slow load is not a
 * freeze, and must not be killed for looking like one).  tests/freezemap.py names the addresses from RV.MAP. */
#ifndef FREEZE_H
#define FREEZE_H

extern volatile unsigned __far g_freeze_ticks;   /* music ticks (1/240 s) since the main loop last ran */
extern volatile unsigned char __far g_freeze_hold;   /* a long load under way: no reports until a loop runs again */
#define freeze_pet() (g_freeze_ticks = 0, g_freeze_hold = 0)     /* the game's loop, and the flight's every frame */
#define freeze_hold() (g_freeze_hold = 1)

void freeze_start(void);                /* the watch on: the game's loop has begun */
void freeze_stop(void);
void freeze_tick(unsigned cs, unsigned ip, const void __far *frame);   /* the music's interrupt, after its EOI */

#endif
