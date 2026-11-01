/* panel.h: the cockpit's panel (panel.c): every dial, lamp and readout showing the ship's systems.
 *
 * The user's ruling (2026-10-22): every dial, display and light of each cockpit does something, or is blanked;
 * the power-distribution knobs, which the game has no use for, repainted as a radio's frequency dials.  The table
 * is data/panels.json (docs/engine.md "The panel"); tools/panels.py makes each control's states from the kit's
 * art, RV.DAT keeps them (RvPanelControl), and here each frame's state of every control comes from the systems
 * (ShipSystems, kept by flight.c), a changed one marks its rows and is drawn over the base. */
#ifndef RV_PANEL_H
#define RV_PANEL_H

#include "cockpit.h"

enum { EMCON_OFF, EMCON_PASSIVE, EMCON_ACTIVE };
enum { SENSOR_RADAR, SENSOR_IR };
enum { CHAN_WING, CHAN_CARRIER, CHAN_GUARD };

/* the ship's systems as the panel shows and sets them (flight.c keeps them; flags combat will read are marked) */
typedef struct {
    uint8_t emcon;                      /* EMCON_*: OFF dark (no scope, no lock, no running lights), PASSIVE (the
                                           scope unidentified, no friend-or-foe lock), ACTIVE (the radar on) */
    uint8_t sensor;                     /* SENSOR_*: the lock's seeker (combat: radar-guided or IR missiles) */
    uint8_t iff;                        /* the IFF on: contacts identified (combat: friend-or-foe locks) */
    uint8_t armed;                      /* master arm (combat: the guns fire only armed) */
    uint8_t muted;                      /* comms muted: no comm faces, no hails */
    uint8_t hold;                       /* the lock held */
    uint8_t match, autotarget, autodecoy;   /* AUTO: match the target's speed, keep the nearest enemy
                                           targeted, decoys on a lock (combat) */
    uint8_t decoys;                     /* left (combat: X drops one) */
    uint8_t radar_range;                /* 0 4,000 km, 1 8,000, 2 16,000 */
    uint8_t surv_page;                  /* the survival display's buttons' page */
    uint8_t nav;                        /* the nav point chosen */
    uint8_t chan;                       /* the radio's channel (CHAN_*): the last that talked */
    uint8_t digit[3];                   /* the frequency knobs as they stand (they step to the channel's) */
    uint8_t talking;                    /* a transmission coming in */
    uint8_t lock, warn, burning, lights, autopilot_ready, target_hot;
    uint8_t lock_pct, lock_kind;        /* the missile lock building, 0 .. 100, and the chosen missile's seeker
                                           (RV_LOCK_*: none a dumbfire); logic.c's lock_tick */
    int16_t target;                     /* its slot, -1 none */
    uint8_t gun_sel;                    /* the guns chosen (G, logic_guns_next): 0 all, k the k-th gun type the ship carries */
    uint8_t launcher_sel;               /* the launcher chosen (W, logic_missile_next): the n-th of the ship's, from 0 */
    int16_t heat, signature;            /* 0 .. 100 */
    int16_t heat_shown, sig_shown;      /* as the HEAT and SIGNATURE displays show them: taken every 5 s */
    int16_t fuel;                       /* afterburner fuel, 0 .. 1000 */
    int16_t fuel_extra;                 /* Kit's reserve tank, burned first: the gauge reads full until it is gone */
    int16_t hull;                       /* percent */
    int32_t target_km, nav_km;          /* -1: none */
    char target_name[16];
} ShipSystems;
extern ShipSystems __far g_sys;

void panel_load(const RvCockpit __far *c);   /* its panel's controls (cockpit_load) */
void panel_unload(void);
void panel_dirty(const CockpitState *st);    /* each control's state: the rows of those that changed marked */
void panel_draw(const CockpitState *st, int y0, int y1);
int panel_at(int x, int y, int *fn, int *arg);   /* the control under a point: 1, its RV_PF_* and argument */
int panel_has(int fn);                       /* this cockpit's panel has a control of that function */
void panel_fuel_bar(int x, int y, int w, int h, int pct, int upright, int y0, int y1);   /* fuel: a backlit bar */
/* the readouts' small font (3 x 5, 4 px a character, capitals, digits, . % - :), for the VDU pages too (the
 * user, 2026-10-31): text at (x, y) in a palette colour, rows y0 .. y1-1; returns its width */
int panel_tiny(int x, int y, const char *s, unsigned char col, int y0, int y1);
int panel_tiny_width(const char *s);
void panel_nav_name(char *s);           /* the chosen nav point, as the displays name it: HOME or NAV n */
#define PANEL_TINY_H 5

#endif
