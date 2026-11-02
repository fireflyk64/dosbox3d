/* vdu.h: the cockpit's VDU pages (vdu.c): what the two multi-function displays show besides a comm.
 *
 * The user's brief (2026-10-31): T (even with the target unchanged) puts the target's top-down wireframe with its
 * shields and hull on a VDU; D a damage readout, and the damage and weapon-select displays show the player's own
 * ship the same way with the live guns and the chosen missile; N a nav menu; C a comm menu for the wingman's orders.
 * The right VDU carries the target, the weapons and the damage pages (the old games' way), the left the status
 * lines, the comm menu and the nav menu.  A comm face on a VDU draws over its page (comms.c). */
#ifndef RV_VDU_H
#define RV_VDU_H

#include "rvasset.h"

enum { VDU_R_TARGET, VDU_R_WEAPONS, VDU_R_DAMAGE };   /* the right VDU's pages */
enum { VDU_L_STATUS, VDU_L_COMM, VDU_L_NAV };          /* the left VDU's */

void vdu_flight_start(void);            /* the pages at their first (the target, the status lines) */

/* the comm menu (C; the user's layout, 2026-10-31): 1 the wingman, always, 2 the target.  The wingman (or a friendly
 * fighter as the target): 1 fly in formation, 2 break formation, 3 attack my target, 4 help me out, 5 return to base.  An enemy target: 1 taunt.  The home carrier as the
 * target: 1 request landing.  A digit picks, 0 or Esc closes.  The intercom wrecked, only the carrier can still be
 * asked (its deck sees the ship: a flight has to be able to end).  vdu_comm_key says what the pilot picked: COMM_ORDER
 * with the ship's slot and the order (logic.h ORDER_*), COMM_TAUNT with the ship's, COMM_LANDING, COMM_CLOSE, or
 * COMM_NONE (the key moved within the menu, or was not its) */
enum { COMM_NONE, COMM_ORDER, COMM_TAUNT, COMM_LANDING, COMM_CLOSE };
void vdu_comm_open(void);               /* the menu up at its first level */
int vdu_comm_key(int ch, int *who, int *order);
/* what a comm is saying, in the small font at the foot of its VDU (over the chin of the face; rows y0 .. y1-1) */
void vdu_subtitle(const RvRect __far *r, const char __far *s, int y0, int y1);
void vdu_static(const RvRect __far *r, int amount, int y0, int y1);   /* static on it: 1 a few dashes .. 6 every row */
void vdu_broken(const RvRect __far *r, int level, int y0, int y1);   /* the page drawn seen through glass broken to that level */
void vdu_show(int which, int page);     /* a key brings a page up (0 the left VDU, 1 the right) */
int vdu_page(int which);
/* the VDU's page into the run y0 .. y1-1 of its rect: 1 when it drew the VDU (the status lines are not drawn after) */
int vdu_draw(int which, const RvRect __far *r, int y0, int y1);

/* a ship type's top-down outline (its silhouette from above, collide.c's mask; its box when it has none) fitted in
 * the w x h box at (x, y), the nose up, in `colour`; rows y0 .. y1-1 only */
void vdu_wire(int type, int x, int y, int w, int h, unsigned char colour, int y0, int y1);
/* a ship (type, in slot s: its shields and armour) as the target and the own-ship pages show it: the wireframe
 * with the front and rear shields as bars outside it and the four armour faces as bars inside, each as long as
 * what is left (the survival display's colours: lit shield, lit armour, spent black) */
void vdu_ship(int type, int s, int x, int y, int side, int y0, int y1);

#endif
