/* vdu.c: the cockpit's VDU pages (vdu.h) */
#include <stdio.h>
#include <string.h>
#include "rv.h"
#include "rvhooks.h"
#include "campaign.h"
#include "collide.h"
#include "panel.h"
#include "world.h"
#include "logic.h"
#include "vdu.h"
#include "vduparts.h"

#define SHIELD_LIT 79                   /* the survival display's colours (panel.c; the user's, 2026-10-23) */
#define ARMOUR_LIT 70
#define WIRE UI_WHITE
#define TEXT UI_AMBER3                  /* the pages' text: the readouts' small font (panel_tiny; the user: the VDUs
                                           take the small font, 2026-10-31), amber, the chosen line white */
#define FH (PANEL_TINY_H + 1)           /* a line of it: five rows and a gap */

static int text(int x, int y, const char *s, int lit, int y0, int y1)
{
    return panel_tiny(x, y, s, lit ? UI_WHITE : TEXT, y0, y1);
}
#define BAR 2                           /* an armour bar's thickness, px */
#define GAP 2                           /* between the wire and the armour, the armour and the shield */

/* a line cut to a width: the text's last characters dropped until it fits */
static void fit(char *s, int w)
{
    int n = (int)strlen(s);
    while (n > 0 && panel_tiny_width(s) > w)
        s[--n] = 0;
}

static unsigned char __far g_page[2];
static unsigned char __far g_cursor;    /* the damage page's line: which hurt system it names (D again: the next) */
static unsigned char __far g_comm_level;   /* the comm menu's page (LEVEL_*) */
#define MSHIP_PILOT(r) (missionShipTable[missionShipTable_SIZE * ((r) & 63) + 16])

void vdu_flight_start(void)
{
    g_page[0] = VDU_L_STATUS;
    g_page[1] = VDU_R_TARGET;
    g_cursor = 0;
    g_comm_level = 0;
}

/* --- the comm menu --- */

enum { LEVEL_WHOM, LEVEL_WINGMAN, LEVEL_TARGET };   /* its pages */
#define COMM_ORDERS 5                   /* what a wingman can be told, the keys 1 .. 5 */
static const char __far k_order_line[COMM_ORDERS][17] = {
    "FLY IN FORMATION", "BREAK FORMATION", "ATTACK MY TARGET", "HELP ME OUT", "RETURN TO BASE"
};
static const uint8_t __far k_order_key[COMM_ORDERS] = { ORDER_FORMATION, ORDER_BREAK, ORDER_ATTACK, ORDER_HELP, ORDER_RTB };
enum { TARGET_NONE, TARGET_ENEMY, TARGET_HOME, TARGET_WING, TARGET_FRIEND };   /* what the target page offers */

/* a ship's callsign (its mission record's pilot), else its type's name */
static void ship_name(int s, char *out, int max)
{
    int r = slotMissionShip[s], pilot = r < 64 ? MSHIP_PILOT(r) : RV_PILOT_NONE;
    if (pilot != RV_PILOT_NONE && pilot < (int)g_npilots && g_pilots[pilot].callsign[0])
        _fstrncpy(out, g_pilots[pilot].callsign, max - 1);
    else if (slot_type((int16_t)s))
        _fstrncpy(out, slot_type((int16_t)s)->name, max - 1);
    else
        strcpy(out, "UNKNOWN");
    out[max - 1] = 0;
}

static int comm_wrecked(void)
{
    return systemDamage[SYS_INTERCOM] >= SYS_WRECKED;
}

/* the wingman the menu's 1 talks to: the first of the wing, -1 none left */
static int comm_wingman(void)
{
    int16_t wing[1];
    return logic_wingmen(wing, 1) ? wing[0] : -1;
}

static int comm_target(void)
{
    int t = g_sys.target;
    if (t < 0 || !slot_is_ship((int16_t)t) || shipAiState[t] == 9)
        return TARGET_NONE;
    if (commAnimInfo[t] != commAnimInfo[0])
        return TARGET_ENEMY;
    if (logic_is_home((int16_t)t))
        return TARGET_HOME;
    return logic_is_wingman((int16_t)t) ? TARGET_WING : TARGET_FRIEND;
}

/* the page that can still be shown: the wingman dead, the target gone or the intercom wrecked under an open page puts
 * the menu back at its first (the target's page follows the target: T changes it) */
static int comm_level(void)
{
    int kind;
    if (g_comm_level == LEVEL_WINGMAN && (comm_wrecked() || comm_wingman() < 0))
        g_comm_level = LEVEL_WHOM;
    if (g_comm_level == LEVEL_TARGET) {
        kind = comm_target();
        if (kind == TARGET_NONE || (comm_wrecked() && kind != TARGET_HOME))
            g_comm_level = LEVEL_WHOM;
    }
    return g_comm_level;
}

void vdu_comm_open(void)
{
    g_comm_level = LEVEL_WHOM;
    g_page[0] = VDU_L_COMM;
}

int vdu_comm_key(int ch, int *who, int *order)
{
    int d, level, kind;
    *who = -1;
    *order = ORDER_FREE;
    if (g_page[0] != VDU_L_COMM)
        return COMM_NONE;
    if (ch == 27 || ch == '0') {        /* (Esc, 0: never mind) */
        g_page[0] = VDU_L_STATUS;
        return COMM_CLOSE;
    }
    if (ch < '1' || ch > '9')
        return COMM_NONE;
    d = ch - '0';
    level = comm_level();
    if (level == LEVEL_WHOM) {
        if (d == 1)
            g_comm_level = LEVEL_WINGMAN;
        else if (d == 2)
            g_comm_level = LEVEL_TARGET;
        (void)comm_level();             /* (none to talk to: it stays) */
        return COMM_NONE;
    }
    kind = comm_target();
    if (level == LEVEL_WINGMAN || kind == TARGET_WING) {
        if (d > COMM_ORDERS)
            return COMM_NONE;
        *who = level == LEVEL_WINGMAN ? comm_wingman() : g_sys.target;
        *order = k_order_key[d - 1];
        g_page[0] = VDU_L_STATUS;
        return COMM_ORDER;
    }
    if (d != 1 || kind == TARGET_FRIEND)
        return COMM_NONE;
    *who = g_sys.target;
    g_page[0] = VDU_L_STATUS;
    return kind == TARGET_ENEMY ? COMM_TAUNT : COMM_LANDING;
}

/* static on a VDU: rows of bright dashes, as many as `amount` (1 a few, 6 every row), the same in every band of a frame */
void vdu_static(const RvRect __far *r, int amount, int y0, int y1)
{
    static const unsigned char __far colours[8] = { UI_GREY2, UI_GREY2, UI_GREY2, UI_GREY4, UI_GREY4, UI_GREY4, UI_GREY5, UI_WHITE };
    uint16_t seed = (uint16_t)(frameCounter * 40503U + r->x);
    int row, n, top = r->y + 2, bottom = r->y + r->h - 2, step = amount >= 6 ? 1 : 8 / amount, left = r->x + 2, w = r->w - 4;
    if (w < 8)
        return;
    for (row = top + (seed & 3) % step; row < bottom; row += step) {
        for (n = amount >= 6 ? 3 : 1; n > 0; n--) {
            int x, len;
            seed = (uint16_t)(seed * 25173U + 13849U);
            x = (int)((uint16_t)(seed >> 3) % (uint16_t)w);
            seed = (uint16_t)(seed * 25173U + 13849U);
            len = 1 + (int)((uint16_t)(seed >> 3) % (uint16_t)(w / 3));
            if (x + len > w)
                len = w - x;
            if (row >= y0 && row < y1)
                gfx_fill(left + x, row, len, 1, colours[(seed >> 12) & 7], y0, y1);
        }
    }
}

/* a page seen through broken glass: heavy, every other row out */
void vdu_broken(const RvRect __far *r, int level, int y0, int y1)
{
    int row;
    if (level >= 3)
        for (row = r->y + 2; row < r->y + r->h - 2; row += 2)
            if (row >= y0 && row < y1)
                gfx_fill(r->x + 2, row, r->w - 4, 1, UI_BLACK, y0, y1);
    if (level)
        vdu_static(r, level, y0, y1);
}

void vdu_show(int which, int page)
{
    if (which == 0 && page == VDU_L_DAMAGE && g_page[0] == VDU_L_DAMAGE)
        g_cursor++;                     /* (D on the damage page: the next hurt system) */
    g_page[which & 1] = (unsigned char)page;
}

int vdu_page(int which)
{
    return g_page[which & 1];
}

/* --- the wireframe --- */

#define SN 64
#define SROW 8
#define TOP(m, row, col) ((m)[(row) * SROW + ((col) >> 3)] & (1 << ((col) & 7)))

/* the outline's column under a pixel across a box w wide: the right half worked out from the edge as the left is, so a pixel and its
 * mirror image read mirror-image columns and a symmetric ship comes out symmetric (px * SN / w alone reads them one apart) */
static int col_of(int px, int w)
{
    return px * 2 < w ? px * SN / w : SN - 1 - (w - 1 - px) * SN / w;   /* (no long: a VDU is under 512 px) */
}

/* the cell under a pixel of the fitted box: solid or not (outside the box: not) */
static int solid(const unsigned char __far *m, int px, int py, int w, int h)
{
    int row;
    if (px < 0 || py < 0 || px >= w || py >= h)
        return 0;
    row = SN - 1 - py * SN / h;         /* (the nose, row 63, at the top) */
    return TOP(m, row, col_of(px, w)) != 0;
}

/* the box the type's outline is fitted in: the w x h box at (x, y) shrunk to the ship's proportions (hit: the
 * silhouettes' half extents, 127ths of the radius) */
static void wire_box(int type, int x, int y, int w, int h, int *bx, int *by, int *bw, int *bh)
{
    const RvShipType __far *t = ship_type((uint16_t)type);
    int hx = t ? t->hit[0] : 100, hz = t ? t->hit[2] : 100, most = hx > hz ? hx : hz;
    if (!most)
        most = 1;
    *bw = (int)((long)w * hx / most);
    *bh = (int)((long)h * hz / most);
    if (!(*bw & 1))                     /* (odd: a pixel on the centre line, w / 2 + d and w / 2 - d mirror images) */
        (*bw)--;
    if (*bw < 3) *bw = 3;
    if (*bh < 2) *bh = 2;
    *bx = x + (w - *bw) / 2;
    *by = y + (h - *bh) / 2;
}

/* the outline: a pixel of the ship with a side that is not.  (This runs every frame a page shows a ship: the columns are worked out
 * once, not per pixel.) */
#define WIRE_MOST 128                   /* the widest box it draws; a VDU's is under 90 */
void vdu_wire(int type, int x, int y, int w, int h, unsigned char colour, int y0, int y1)
{
    const unsigned char __far *m = collide_mask((uint16_t)type);
    unsigned char col[WIRE_MOST];
    int bw, bh, bx, by, px, py, up, here, down;
    if (w < 4 || h < 4)
        return;
    wire_box(type, x, y, w, h, &bx, &by, &bw, &bh);
    if (!m || bw > WIRE_MOST) {         /* (no silhouettes: its box) */
        gfx_frame(bx, by, bw, bh, colour, y0, y1);
        return;
    }
    for (px = 0; px < bw; px++)
        col[px] = (unsigned char)col_of(px, bw);
    for (py = 0; py < bh; py++) {
        unsigned char __far *d;
        if (by + py < y0 || by + py >= y1)
            continue;
        d = vid_row(by + py) + bx;
        here = SN - 1 - py * SN / bh;   /* (the nose, row 63, at the top) */
        up = py ? SN - 1 - (py - 1) * SN / bh : -1;
        down = py + 1 < bh ? SN - 1 - (py + 1) * SN / bh : -1;
        for (px = 0; px < bw; px++)
            if (TOP(m, here, col[px])
                && (!px || !TOP(m, here, col[px - 1]) || px + 1 == bw || !TOP(m, here, col[px + 1])
                    || up < 0 || !TOP(m, up, col[px]) || down < 0 || !TOP(m, down, col[px])))
                d[px] = colour;
    }
}

/* a level in a hundred: 0 .. 100 */
static int pct_of(unsigned long have, unsigned long most)
{
    if (!most)
        return 0;
    if (have >= most)
        return 100;
    return (int)(have * 100 / most);
}

void vdu_ship(int type, int s, int x, int y, int side, int y0, int y1)
{
    const RvShipType __far *t = ship_type((uint16_t)type);
    int inner = side - 2 * (BAR + GAP) - 2 * (2 + GAP), ix, iy, k, n, pct[6];
    if (!t || inner < 8)
        return;
    ix = x + (side - inner) / 2;
    iy = y + (side - inner) / 2;
    vdu_wire(type, ix, iy, inner, inner, WIRE, y0, y1);
    /* the levels: the front and rear shields, the front, rear, left and right armour */
    pct[0] = pct_of(SHIELD(s, 0), SHIELD_MAX(s, 0));
    pct[1] = pct_of(SHIELD(s, 1), SHIELD_MAX(s, 1));
    pct[2] = pct_of(ARMOR(s, 0), t->armor_f);
    pct[3] = pct_of(ARMOR(s, 1), t->armor_r);
    pct[4] = pct_of(ARMOR(s, 2), t->armor_l);
    pct[5] = pct_of(ARMOR(s, 3), t->armor_rt);
    /* the armour: bars just outside the wire, as long as what is left, centred on their side */
    n = (int)((long)inner * pct[2] / 100);
    gfx_fill(ix + (inner - n) / 2, iy - GAP - BAR, n, BAR, ARMOUR_LIT, y0, y1);
    n = (int)((long)inner * pct[3] / 100);
    gfx_fill(ix + (inner - n) / 2, iy + inner + GAP, n, BAR, ARMOUR_LIT, y0, y1);
    n = (int)((long)inner * pct[4] / 100);
    gfx_fill(ix - GAP - BAR, iy + (inner - n) / 2, BAR, n, ARMOUR_LIT, y0, y1);
    n = (int)((long)inner * pct[5] / 100);
    gfx_fill(ix + inner + GAP, iy + (inner - n) / 2, BAR, n, ARMOUR_LIT, y0, y1);
    /* the shields: a bar beyond the front and the rear armour, the full side wide */
    k = side - 4;
    n = (int)((long)k * pct[0] / 100);
    gfx_fill(x + (side - n) / 2, y, n, 2, SHIELD_LIT, y0, y1);
    n = (int)((long)k * pct[1] / 100);
    gfx_fill(x + (side - n) / 2, y + side - 2, n, 2, SHIELD_LIT, y0, y1);
}

/* --- the pages --- */

/* the hull left, in a hundred */
static int hull_pct(int s)
{
    const RvShipType __far *t = slot_type((int16_t)s);
    if (!t || !t->hull)
        return 100;
    return damagePoints[s] >= t->hull ? 0 : (int)(100 - (long)damagePoints[s] * 100 / t->hull);
}


/* a ship's page (the target's, the own ship's): the wire with its bars, and three lines of text beside it on a wide
 * VDU, else one above and one below */
static void ship_page(int s, int type, const RvRect __far *r, char *top, const char *a, const char *b, int y0, int y1)
{
    int side, x, y;
    if (r->w >= 2 * r->h) {             /* wide and short: the ship at the left, the lines at the right */
        side = r->h - 4;
        vdu_ship(type, s, r->x + 2, r->y + 2, side, y0, y1);
        x = r->x + side + 8;
        y = r->y + 3;
        fit(top, r->x + r->w - 3 - x);
        text(x, y, top, 0, y0, y1);
        text(x, y + FH + 1, a, 0, y0, y1);
        text(x, y + 2 * (FH + 1), b, 0, y0, y1);
    } else {                            /* a line above, the ship, a line below */
        side = r->h - 2 * (FH + 2) - 2;
        if (side > r->w - 4)
            side = r->w - 4;
        fit(top, r->w - 6);
        text(r->x + 3, r->y + 3, top, 0, y0, y1);
        vdu_ship(type, s, r->x + (r->w - side) / 2, r->y + FH + 3, side, y0, y1);
        x = r->x + 3;
        y = r->y + r->h - FH - 2;
        text(x, y, a, 0, y0, y1);
        text(x + panel_tiny_width(a) + 6, y, b, 0, y0, y1);
    }
}

/* the target page: its name, its range, its hull and the ship */
static void draw_target(const RvRect __far *r, int y0, int y1)
{
    char name[16], a[16], b[16];
    int s = g_sys.target;
    if (s < 0 || !slot_type((int16_t)s)) {
        text(r->x + 4, r->y + 3, "NO TARGET", 0, y0, y1);
        panel_nav_name(a);              /* (the chosen nav point instead: where to go, how far) */
        text(r->x + 4, r->y + 3 + 2 * FH, a, 1, y0, y1);
        if (g_sys.nav_km >= 0) {
            sprintf(b, "%ld KM", g_sys.nav_km);
            text(r->x + 4, r->y + 3 + 3 * FH, b, 0, y0, y1);
        }
        return;
    }
    _fstrncpy(name, g_sys.target_name, sizeof name - 1);
    name[sizeof name - 1] = 0;
    if (g_sys.target_km >= 0 && sys_level(SYS_TRACK) < 3)   /* (the tracking badly hurt: no range, no reading of it) */
        sprintf(a, "%ld KM", g_sys.target_km);
    else
        strcpy(a, "-");
    if (sys_level(SYS_TRACK) < 3)
        sprintf(b, "HULL %d%%", hull_pct(s));
    else
        strcpy(b, "HULL -");
    ship_page(s, gInstanceShipTypes[s], r, name, a, b, y0, y1);
}

/* --- the own ship's pages, damage and weapons (the user's, 2026-11-01): the wire with the whole of its square, no bars (the survival
 * display has the shields and the armour), a dim green the shapes stand out on (tools/vduparts.py draws them, vduparts.h) --- */

#define OWN_WIRE 68
#define PART_HI UI_RED                  /* the part the damage page names */
#define PART_DIM 98                     /* the other hurt parts */
#define PART_LO 96
#define WEAPON_HI 71                    /* the weapons pages: a weapon that fires, light green on the wire (the user: "they contrast") */
#define WEAPON_LO 69
#define MOUNT_OFF UI_GREY4              /* a weapon the choice leaves out */
#define MOUNT_OFF_LO UI_GREY2

typedef struct {
    const RvRect __far *r;              /* the VDU */
    const unsigned char __far *m;       /* the ship's outline */
    int x, y, w, h;                     /* its box on the screen */
    int y0, y1;
} Own;

/* the square for the ship below `above` lines and above one, or at the left of a wide VDU; the wire in it */
static void own_begin(Own *o, const RvRect __far *r, int above, int *side, int y0, int y1)
{
    int x, y;
    if (r->w >= 2 * r->h) {
        *side = r->h - 4;
        x = r->x + 2;
        y = r->y + 2;
    } else {
        *side = r->h - (above + 1) * FH - 6;
        if (*side > r->w - 4)
            *side = r->w - 4;
        x = r->x + (r->w - *side) / 2;
        y = r->y + 3 + above * FH + 1;
    }
    o->r = r;
    o->m = collide_mask((uint16_t)gInstanceShipTypes[0]);
    o->y0 = y0;
    o->y1 = y1;
    wire_box(gInstanceShipTypes[0], x, y, *side, *side, &o->x, &o->y, &o->w, &o->h);
    vdu_wire(gInstanceShipTypes[0], x, y, *side, *side, OWN_WIRE, y0, y1);
}

/* a shape centred at a place on the ship's box, kept on the VDU */
static void stamp(const Own *o, int k, int px, int py, int mirror, unsigned char hi, unsigned char lo)
{
    const unsigned short __far *a = g_part[k];
    int cx = o->x + px - PART_N / 2, cy = o->y + py - PART_N / 2, row, col;
    if (cx < o->r->x + 1) cx = o->r->x + 1;
    if (cx > o->r->x + o->r->w - PART_N - 1) cx = o->r->x + o->r->w - PART_N - 1;
    if (cy < o->r->y + 1) cy = o->r->y + 1;
    if (cy > o->r->y + o->r->h - PART_N - 1) cy = o->r->y + o->r->h - PART_N - 1;
    for (row = 0; row < PART_N; row++) {
        unsigned char __far *d;
        unsigned short b = a[row], l = a[row + PART_N];
        if (cy + row < o->y0 || cy + row >= o->y1 || !(b | l))
            continue;
        d = vid_row(cy + row) + cx;
        for (col = 0; col < PART_N; col++) {
            int bit = mirror ? col : PART_N - 1 - col;
            if (b >> bit & 1)
                d[col] = hi;
            else if (l >> bit & 1)
                d[col] = lo;
        }
    }
}

/* how far the hull reaches from the centre line along a row */
static int half_at(const Own *o, int py)
{
    int d;
    if (!o->m)
        return o->w / 2;
    for (d = o->w / 2; d > 0; d--)
        if (solid(o->m, o->w / 2 - d, py, o->w, o->h) || solid(o->m, o->w / 2 + d, py, o->w, o->h))
            return d;
    return 0;
}

/* where the hull's leading edge is at a place across the ship and its mirror image, the same row for both: a stray cell of one
 * side's outline must not put a pair of guns out of line; -1 no hull there */
static int leading_edge(const Own *o, int px)
{
    int py;
    if (!o->m)
        return 0;
    for (py = 0; py < o->h; py++)
        if (solid(o->m, px, py, o->w, o->h) || solid(o->m, o->w - 1 - px, py, o->w, o->h))
            return py;
    return -1;
}

/* the weapons of a kind on the wire, each at its mount across the ship: a gun from the hull's leading edge there; a launcher a missile
 * for each round it has left, side by side, further back.  The ones that fire are drawn last, over the others */
#define ROUNDS_DRAWN 8
#define ROUND_W 7
#define GUN_W 5
enum { MARK_GUNS, MARK_MISSILES, MARK_HURT_GUNS, MARK_HURT_RACKS };
static void mount_marks(const Own *o, int mode, unsigned char hi, unsigned char lo)
{
    int g, pass, l = logic_launcher(), want_guns = mode == MARK_GUNS || mode == MARK_HURT_GUNS, hurt = mode >= MARK_HURT_GUNS;
    for (pass = 0; pass < 2; pass++)
        for (g = 0; g < GUN_COUNT(0); g++) {
            const RvWeapon __far *w = weapon(GUN_TYPE(0, g));
            int gun, dead, px, py, fires, rounds, i, k, same = 0, racks = 0;
            if (!w)
                continue;
            gun = w->kind == RV_WK_GUN || w->kind == RV_WK_TURRET;
            if (gun != want_guns)
                continue;
            dead = (GUN_STATE(0, g) & GUN_DESTROYED) != 0;
            if (hurt) {
                if (!(dead || sys_level(gun ? SYS_GUNS : SYS_RACKS)))
                    continue;
                fires = 1;
            } else {
                if (dead)
                    continue;           /* (gone from the ship: the damage page says so) */
                fires = gun ? logic_gun_selected((int16_t)g) : l >= 0 && GUN_TYPE(0, g) == GUN_TYPE(0, l);
            }
            if (fires != pass)
                continue;
            for (k = 0; k < g; k++) {   /* (two kinds of gun share their mounts, and launchers all sit on the centre line: */
                const RvWeapon __far *e = weapon(GUN_TYPE(0, k));   /* each to be seen, the guns side by side, the racks behind) */
                if (e && (e->kind == RV_WK_GUN || e->kind == RV_WK_TURRET) == gun) {
                    same += GUN_MOUNT_X(0, k) == GUN_MOUNT_X(0, g);
                    racks++;
                }
            }
            px = (int)GUN_MOUNT_X(0, g) * (o->w / 2) / 32;   /* (32nds of the half box) */
            if (gun)
                px += (GUN_MOUNT_X(0, g) < 0 ? -same : same) * GUN_W;
            px += o->w / 2;
            if (px < 0) px = 0;
            if (px >= o->w) px = o->w - 1;
            if (gun) {
                py = leading_edge(o, px);
                stamp(o, SYS_GUNS, px, (py < 0 ? 0 : py) + PART_N / 2, 0, fires ? hi : MOUNT_OFF, fires ? lo : MOUNT_OFF_LO);
                continue;
            }
            rounds = hurt ? 1 : GUN_STATE(0, g) & 0x7F;
            if (rounds > ROUNDS_DRAWN)
                rounds = ROUNDS_DRAWN;
            for (i = 0; i < rounds; i++) {
                int at = px + (2 * i - rounds + 1) * ROUND_W / 2;
                py = at >= 0 && at < o->w ? leading_edge(o, at) : -1;
                py = (py < 0 ? o->h / 2 : py + o->h / 5) + racks * (PART_N + 1);
                if (py > o->h - PART_N / 2 - 1)   /* (inside the ship's box: the count is under it) */
                    py = o->h - PART_N / 2 - 1;
                stamp(o, SYS_RACKS, at, py, 0, fires ? hi : MOUNT_OFF, fires ? lo : MOUNT_OFF_LO);
            }
        }
}

/* the engine section's length: just the end by the nozzles (the user: a long one was "a bit tubular") */
#define ENGINE_ROWS(o) ((o)->h / 8 + 2)

/* the row of the rearmost engine, and how far across the engines reach (their places: data/engines.csv) */
static int engine_tail(const Own *o, int *xa, int *xb)
{
    const RvShipType __far *t = ship_type((uint16_t)gInstanceShipTypes[0]);
    int hx = t && t->hit[0] ? t->hit[0] : 100, hz = t && t->hit[2] ? t->hit[2] : 100;
    int n = t ? t->nengines : 0, tail = 0, k;
    if (n > RV_ENGINES)
        n = RV_ENGINES;
    *xa = o->w;
    *xb = 0;
    for (k = 0; k < n; k++) {
        int ex = o->w / 2 + t->engine[k][0] * (o->w / 2) / hx, ey = o->h / 2 - t->engine[k][2] * (o->h / 2) / hz;
        if (ex < *xa) *xa = ex;
        if (ex > *xb) *xb = ex;
        if (ey > tail) tail = ey;
    }
    if (!n) {
        *xa = *xb = o->w / 2;
        tail = o->h - 1;
    }
    return tail >= o->h ? o->h - 1 : tail;
}

/* the engine section: the tail of the ship's own outline around its engines, ribbed, and the nozzles */
static void part_engine(const Own *o, unsigned char hi, unsigned char lo)
{
    const RvShipType __far *t = ship_type((uint16_t)gInstanceShipTypes[0]);
    int hx = t && t->hit[0] ? t->hit[0] : 100, n = t ? t->nengines : 0, xa, xb, tail = engine_tail(o, &xa, &xb), top, k, px, py;
    if (n > RV_ENGINES)
        n = RV_ENGINES;
    xa -= 4;
    xb += 4;
    top = tail - ENGINE_ROWS(o);
    for (py = top; py <= tail; py++) {
        unsigned char __far *d;
        if (o->y + py < o->y0 || o->y + py >= o->y1)
            continue;
        d = vid_row(o->y + py) + o->x;
        for (px = xa; px <= xb; px++)
            if (px >= 0 && px < o->w && (!o->m || solid(o->m, px, py, o->w, o->h)))
                d[px] = (tail - py) % 3 == 2 ? lo : hi;
    }
    for (k = 0; k < n; k++) {
        int ex = o->x + o->w / 2 + t->engine[k][0] * (o->w / 2) / hx;
        gfx_fill(ex - 2, o->y + tail + 1, 5, 1, hi, o->y0, o->y1);
        gfx_fill(ex - 3, o->y + tail + 2, 7, 1, lo, o->y0, o->y1);
    }
}

/* where a part sits on the ship: across, 32nds of the hull's half width at that row, then along, 64ths of the way from the nose to the
 * engine section; by SYS_ */
static const signed char __far g_part_at[SYS_COUNT][2] = {
    { 26, 46 }, { 0, 37 }, { -26, 46 }, { 0, 26 }, { -26, 36 }, { -24, 13 }, { 0, 15 }, { -30, 58 }, { 0, 0 },
    { 0, 0 }, { 0, 48 }, { 0, 5 }, { -26, 26 }, { 26, 26 }, { 26, 36 }, { 0, 0 }, { 0, 0 }, { 24, 13 }, { 30, 58 } };

static void part_draw(const Own *o, int k, unsigned char hi, unsigned char lo, int card)
{
    int py, half, i, xa, xb, cx;
    switch (k) {
    case SYS_GUNS: mount_marks(o, MARK_HURT_GUNS, hi, lo); return;
    case SYS_RACKS: mount_marks(o, MARK_HURT_RACKS, hi, lo); return;
    case SYS_ENGINE: part_engine(o, hi, lo); return;
    case SYS_THRUSTERS:                 /* (four, on the outline, firing out) */
        for (i = 0; i < 2; i++) {
            py = o->h * (i ? 44 : 18) / 64;
            half = half_at(o, py);
            stamp(o, k, o->w / 2 - half + 2, py, 1, hi, lo);
            stamp(o, k, o->w / 2 + half - 2, py, 0, hi, lo);
        }
        return;
    }
    py = (engine_tail(o, &xa, &xb) - ENGINE_ROWS(o)) * g_part_at[k][1] / 64;
    half = half_at(o, py);
    if (half < 8)
        half = 8;
    cx = o->w / 2 + g_part_at[k][0] * half / 32;
    if (card)                           /* (the one named: clear of the others) */
        gfx_fill(o->x + cx - PART_N / 2 - 1, o->y + py - PART_N / 2 - 1, PART_N + 2, PART_N + 2, UI_BLACK, o->y0, o->y1);
    stamp(o, k, cx, py, 0, hi, lo);
}

/* the damage page: the hull in a word above the ship, red on the reserve; the hurt parts on it as shapes where they sit in a ship
 * (the user: "engine section for engine", "a computer for computer", "radar for radar"), and one of them at a time bright, its name
 * below in red and no more (the user: not "so accurate", "just state the part if it's damaged"): they come round by themselves, and
 * D moves on at once */
#define DAMAGE_TURN 60                  /* frames a system is named: two seconds */
static void draw_damage(const RvRect __far *r, int y0, int y1)
{
    char top[16], name[12];
    Own o;
    int list[SYS_COUNT], n = 0, k, side, now, wide = r->w >= 2 * r->h, x, y;
    if (!slot_type(0))
        return;
    for (k = 0; k < SYS_COUNT; k++)
        if (damage_part_hurt((int16_t)k))
            list[n++] = k;
    own_begin(&o, r, 1, &side, y0, y1);
    now = n ? list[(g_cursor + frameCounter / DAMAGE_TURN) % n] : -1;
    for (k = 0; k < n; k++)
        if (list[k] != now)
            part_draw(&o, list[k], PART_DIM, PART_LO, 0);
    if (n)
        part_draw(&o, now, PART_HI, PART_LO, 1);
    x = wide ? r->x + side + 8 : r->x + 3;
    panel_hull_name(top);
    panel_tiny(x, r->y + 3, top, g_sys.reserve >= 0 ? UI_RED : TEXT, y0, y1);
    y = wide ? r->y + 3 + FH + 1 : r->y + r->h - FH - 2;
    if (n) {
        _fstrncpy(name, logic_system_name((int16_t)now), sizeof name - 1);
        name[sizeof name - 1] = 0;
        panel_tiny(x, y, name, UI_RED, y0, y1);
    } else {
        text(x, y, "NO DAMAGE", 0, y0, y1);
    }
}

/* a line across the middle of the VDU (the weapons page has its lines centred, as the old games' do) */
static void centred(const RvRect __far *r, int x, int w, int y, char *s, int y0, int y1)
{
    (void)r;
    fit(s, w);
    text(x + (w - panel_tiny_width(s)) / 2, y, s, 0, y0, y1);
}

/* the weapons pages, G the guns, W the missile (the user's picture, 2026-11-01): what is active, its name, how hard it hits in a word,
 * the ship with it in green on its mounts, and how many.  On a VDU too short for all of it the name above and the rest below */
static void draw_weapons(const RvRect __far *r, int missile, int y0, int y1)
{
    char title[16], name[RV_WNAME + 1], word[24], count[20];
    Own o;
    uint16_t types[8];
    const RvWeapon __far *w = 0;
    unsigned hits = 0;
    int n, g, have = 0, side, l = logic_launcher(), wide = r->w >= 2 * r->h, full = wide || r->h >= 80, x, tw, y;
    if (!slot_type(0))
        return;
    if (missile) {
        strcpy(title, "ACTIVE MISSILE");
        if (l >= 0 && (w = weapon(GUN_TYPE(0, l))) != 0) {
            for (g = 0; g < GUN_COUNT(0); g++)   /* (the rounds of every launcher of that weapon) */
                if (GUN_TYPE(0, g) == GUN_TYPE(0, l) && !(GUN_STATE(0, g) & GUN_DESTROYED))
                    have += GUN_STATE(0, g) & 0x7F;
        }
        sprintf(count, "%d LOADED", have);
    } else {
        strcpy(title, "ACTIVE GUNS");
        n = logic_gun_types(types, 8);
        if (n == 1 || (g_sys.gun_sel && g_sys.gun_sel <= n))
            w = weapon(types[n == 1 ? 0 : g_sys.gun_sel - 1]);
        for (g = 0; g < GUN_COUNT(0); g++) {
            const RvWeapon __far *t = weapon(GUN_TYPE(0, g));
            if (t && (t->kind == RV_WK_GUN || t->kind == RV_WK_TURRET) && logic_gun_selected((int16_t)g)
                && !(GUN_STATE(0, g) & GUN_DESTROYED)) {
                have++;
                if (t->damage > hits)   /* (all of them at once, of two kinds: the harder) */
                    hits = t->damage;
            }
        }
        if (!full)
            sprintf(count, "%d ENGAGED", have);
        else
            sprintf(count, have == 1 ? "1 GUN ENGAGED" : "%d GUNS ENGAGED", have);
    }
    if (w) {
        _fstrncpy(name, w->name, RV_WNAME);
        name[RV_WNAME] = 0;
        hits = w->damage;
    } else {
        strcpy(name, missile ? "NO MISSILES" : have ? "ALL GUNS" : "NO GUNS");
        if (missile)
            count[0] = 0;
    }
    word[0] = 0;
    if (hits) {
        strcpy(word, full ? "DAMAGE: " : "");
        strcat(word, hits < 25 ? "LIGHT" : hits < 36 ? "MODERATE" : hits < 100 ? "HEAVY" : "SEVERE");
    }
    own_begin(&o, r, full && !wide ? 3 : 1, &side, y0, y1);
    mount_marks(&o, missile ? MARK_MISSILES : MARK_GUNS, WEAPON_HI, WEAPON_LO);
    x = wide ? r->x + side + 6 : r->x + 3;
    tw = r->x + r->w - 3 - x;
    y = r->y + 3;
    if (full) {
        centred(r, x, tw, y, title, y0, y1);
        gfx_fill(x + 2, y + FH - 1, tw - 4, 1, TEXT, y0, y1);
        centred(r, x, tw, y + FH + 1, name, y0, y1);
        centred(r, x, tw, y + 2 * FH + 1, word, y0, y1);
        centred(r, x, tw, wide ? y + 3 * FH + 1 : r->y + r->h - FH - 2, count, y0, y1);
    } else {
        centred(r, x, tw, y, name, y0, y1);
        y = r->y + r->h - FH - 2;
        fit(word, tw - panel_tiny_width(count) - 4);
        text(x, y, word, 0, y0, y1);
        text(x + tw - panel_tiny_width(count), y, count, 0, y0, y1);
    }
}


/* how many lines a VDU holds at once: its rows of the small font (six px a line: thirteen on an 85-px VDU, seven
 * on a 49-px) */
static int menu_rows(const RvRect __far *r)
{
    int rows = (r->h - 4) / FH;
    return rows < 1 ? 1 : rows;
}

/* a menu's lines down a VDU from line `from`, in columns when it is wide enough (each as wide as the widest
 * line, cut to the VDU), as many as fit; line `lit` in white, the rest amber.  Returns how many it showed */
static int menu(const RvRect __far *r, char lines[][20], int n, int from, int lit, int y0, int y1)
{
    int rows = menu_rows(r), k, cw = 0, w, x = r->x + 3, y, shown = 0;
    for (k = from; k < n; k++) {
        fit(lines[k], r->w - 6);
        if ((w = panel_tiny_width(lines[k])) > cw)
            cw = w;
    }
    cw += 8;
    for (k = from; k < n; k++) {
        y = r->y + 3 + ((k - from) % rows) * FH;
        if (x + cw - 8 > r->x + r->w)
            break;                      /* (no room for another column) */
        text(x, y, lines[k], k == lit, y0, y1);
        shown++;
        if ((k - from) % rows == rows - 1)
            x += cw;
    }
    return shown;
}

/* the nav page: the waypoints with how far each is, then home; the chosen one lit (N: the next) */
static void draw_nav(const RvRect __far *r, int y0, int y1)
{
    char lines[16][20];
    int n = 0, k, lit = 0;
    for (k = 1; k <= 16 && n < 16; k++) {
        int32_t km;
        int id = k % 16;                /* (home last: the way out, then the way back) */
        if (!logic_nav_km(id, &km))
            continue;
        if (id == g_sys.nav)
            lit = n;
        if (id)
            sprintf(lines[n], "NAV %d  %ldK", id, (km + 500) / 1000);
        else
            sprintf(lines[n], "HOME  %ldK", (km + 500) / 1000);
        n++;
    }
    if (!n) {
        text(r->x + 4, r->y + 3, "NO NAV", 0, y0, y1);
        return;
    }
    {   /* (the chosen one on the VDU: from the start of its page of rows) */
        int per = menu_rows(r);
        (void)menu(r, lines, n, lit - lit % per, lit, y0, y1);
    }
}

/* the comm page: 1 the wingman and 2 the target, or what can be said to the one picked */
static void draw_comm(const RvRect __far *r, int y0, int y1)
{
    char lines[COMM_ORDERS + 2][20], name[12];
    int n = 0, level = comm_level(), kind = comm_target();
    if (level == LEVEL_WHOM) {
        int w = comm_wingman();
        strcpy(lines[n++], comm_wrecked() ? "INTERCOM OUT" : "SEND TO:");
        if (!comm_wrecked()) {
            if (w >= 0)
                ship_name(w, name, 12);
            else
                strcpy(name, "NO WINGMAN");
            sprintf(lines[n++], "1 %s", name);
        }
        if (kind != TARGET_NONE && (!comm_wrecked() || kind == TARGET_HOME)) {
            ship_name(g_sys.target, name, 12);
            sprintf(lines[n++], "2 %s", name);
        } else if (!comm_wrecked())
            strcpy(lines[n++], "2 NO TARGET");
    } else {
        ship_name(level == LEVEL_WINGMAN ? comm_wingman() : g_sys.target, name, 12);
        sprintf(lines[n++], "%s:", name);
        if (level == LEVEL_WINGMAN || kind == TARGET_WING) {
            int k;
            for (k = 0; k < COMM_ORDERS; k++) {
                sprintf(lines[n], "%d ", k + 1);
                _fstrcpy(lines[n++] + 2, k_order_line[k]);
            }
        } else if (kind == TARGET_ENEMY)
            strcpy(lines[n++], "1 TAUNT");
        else if (kind == TARGET_HOME)
            strcpy(lines[n++], "1 REQUEST LANDING");
        else
            strcpy(lines[n++], "NOTHING TO SEND");
    }
    strcpy(lines[n++], "0 NEVER MIND");
    (void)menu(r, lines, n, 0, -1, y0, y1);
}

/* a comm's line, its words wrapped to the VDU, the last rows at its foot on black so they read over the face */
void vdu_subtitle(const RvRect __far *r, const char __far *s, int y0, int y1)
{
    enum { ROWS = 4, COLS = 28 };
    char row[ROWS][COLS + 1];
    int cols = (r->w - 4) / 4, n = 0, len = 0, k, y, top;
    if (cols > COLS)
        cols = COLS;
    if (cols < 4 || !s || !*s)
        return;
    while (*s && n < ROWS) {            /* (greedy: word by word; a word too long for a row is cut) */
        const char __far *e = s;
        int w;
        while (*e && *e != ' ')
            e++;
        w = (int)(e - s);
        if (w > cols)
            w = cols;
        if (len && len + 1 + w > cols) {
            row[n++][len] = 0;
            len = 0;
            if (n == ROWS)
                break;
        }
        if (len)
            row[n][len++] = ' ';
        for (k = 0; k < w; k++)
            row[n][len++] = (char)s[k];
        s = e;
        while (*s == ' ')
            s++;
    }
    if (n < ROWS && len)
        row[n++][len] = 0;
    if (sys_level(SYS_INTERCOM)) {      /* the intercom hurt: words lost, more the worse */
        static const unsigned char __far lost[5] = { 0, 1, 2, 4, 7 };
        int lv = sys_level(SYS_INTERCOM), i;
        for (k = 0; k < n; k++)
            for (i = 0; row[k][i]; i++)
                if (row[k][i] != ' ' && (((uint16_t)(k * 31 + i * 7 + (frameCounter >> 3)) * 40503U) >> 13) < lost[lv])
                    row[k][i] = '-';
    }
    top = r->y + r->h - n * FH - 1;
    for (y = top < y0 ? y0 : top; y < y1 && y < r->y + r->h; y++)
        _fmemset(vid_row(y) + r->x, 0, r->w);
    for (k = 0; k < n; k++)
        text(r->x + 2, top + 1 + k * FH, row[k], 1, y0, y1);
}

int vdu_draw(int which, const RvRect __far *r, int y0, int y1)
{
    if (!r->w || y1 <= r->y || y0 >= r->y + r->h)
        return 0;
    if (which == 0) {
        switch (g_page[0]) {
        case VDU_L_NAV: draw_nav(r, y0, y1); return 1;
        case VDU_L_COMM: draw_comm(r, y0, y1); return 1;
        case VDU_L_WEAPONS: draw_weapons(r, 0, y0, y1); return 1;
        case VDU_L_MISSILE: draw_weapons(r, 1, y0, y1); return 1;
        case VDU_L_DAMAGE: draw_damage(r, y0, y1); return 1;
        default: return 0;
        }
    }
    if (which == 1) {
        switch (g_page[1]) {
        case VDU_R_TARGET: draw_target(r, y0, y1); return 1;
        default: return 0;
        }
    }
    return 0;
}
