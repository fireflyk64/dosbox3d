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
static unsigned char __far g_comm_level;   /* the comm menu: 0 whom, 1 a wingman's orders, 2 the carrier's */
static int __far g_comm_who;            /* the wingman's slot (level 1) */
#define MSHIP_PILOT(r) (missionShipTable[missionShipTable_SIZE * ((r) & 63) + 16])

void vdu_flight_start(void)
{
    g_page[0] = VDU_L_STATUS;
    g_page[1] = VDU_R_TARGET;
    g_cursor = 0;
    g_comm_level = 0;
    g_comm_who = -1;
}

/* --- the comm menu --- */

#define COMM_WINGMEN 6                  /* at most this many wingmen listed */

static const char __far k_order_line[ORDER_COUNT][20] = {
    "BREAK AND ATTACK", "ATTACK MY TARGET", "FORM ON MY WING", "HELP ME OUT", "RETURN TO BASE"
};
static const uint8_t __far k_order_key[5] = { ORDER_ATTACK, ORDER_FREE, ORDER_FORMATION, ORDER_HELP, ORDER_RTB };   /* 1 .. 5 */

/* a wingman's callsign (its mission record's pilot), else its type's name */
static void wingman_name(int s, char *out, int max)
{
    int r = slotMissionShip[s], pilot = r < 64 ? MSHIP_PILOT(r) : RV_PILOT_NONE;
    if (pilot != RV_PILOT_NONE && pilot < (int)g_npilots && g_pilots[pilot].callsign[0])
        _fstrncpy(out, g_pilots[pilot].callsign, max - 1);
    else if (slot_type((int16_t)s))
        _fstrncpy(out, slot_type((int16_t)s)->name, max - 1);
    else
        strcpy(out, "WINGMAN");
    out[max - 1] = 0;
}

void vdu_comm_open(void)
{
    g_comm_level = 0;
    g_comm_who = -1;
    g_page[0] = VDU_L_COMM;
}

int vdu_comm_key(int ch, int *who, int *what)
{
    int16_t wing[COMM_WINGMEN];
    int n, d;
    *who = -1;
    *what = COMM_NONE;
    if (g_page[0] != VDU_L_COMM)
        return COMM_NONE;
    if (ch == 27 || ch == '0') {        /* (Esc, 0: never mind) */
        g_page[0] = VDU_L_STATUS;
        return *what = COMM_CLOSE;
    }
    if (ch < '1' || ch > '9')
        return COMM_NONE;
    d = ch - '0';
    if (g_comm_level == 0) {            /* whom: a wingman, or the carrier after them */
        n = logic_wingmen(wing, COMM_WINGMEN);
        if (d <= n) {
            g_comm_who = wing[d - 1];
            g_comm_level = 1;
        } else if (d == n + 1)
            g_comm_level = 2;
        return COMM_NONE;
    }
    if (g_comm_level == 1) {            /* an order */
        if (d > 5)
            return COMM_NONE;
        *who = g_comm_who;
        g_page[0] = VDU_L_STATUS;
        return *what = (int)k_order_key[d - 1] + 0x100;   /* (the order in the low byte: COMM_ORDER's) */
    }
    if (d == 1) {                       /* the carrier: request landing */
        g_page[0] = VDU_L_STATUS;
        return *what = COMM_LANDING;
    }
    return COMM_NONE;
}

void vdu_show(int which, int page)
{
    if (which == 1 && page == VDU_R_DAMAGE && g_page[1] == VDU_R_DAMAGE)
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

/* the cell under a pixel of the fitted box: solid or not (outside the box: not) */
static int solid(const unsigned char __far *m, int px, int py, int w, int h)
{
    int col, row;
    if (px < 0 || py < 0 || px >= w || py >= h)
        return 0;
    col = (int)((long)px * SN / w);
    row = SN - 1 - (int)((long)py * SN / h);   /* (the nose, row 63, at the top) */
    return TOP(m, row, col) != 0;
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
    if (*bw < 2) *bw = 2;
    if (*bh < 2) *bh = 2;
    *bx = x + (w - *bw) / 2;
    *by = y + (h - *bh) / 2;
}

void vdu_wire(int type, int x, int y, int w, int h, unsigned char colour, int y0, int y1)
{
    const unsigned char __far *m = collide_mask((uint16_t)type);
    int bw, bh, bx, by, px, py;
    if (w < 4 || h < 4)
        return;
    wire_box(type, x, y, w, h, &bx, &by, &bw, &bh);
    if (!m) {                           /* (no silhouettes: its box) */
        gfx_frame(bx, by, bw, bh, colour, y0, y1);
        return;
    }
    for (py = 0; py < bh; py++) {
        unsigned char __far *d;
        if (by + py < y0 || by + py >= y1)
            continue;
        d = vid_row(by + py);
        for (px = 0; px < bw; px++)
            if (solid(m, px, py, bw, bh)
                && (!solid(m, px - 1, py, bw, bh) || !solid(m, px + 1, py, bw, bh) || !solid(m, px, py - 1, bw, bh)
                    || !solid(m, px, py + 1, bw, bh)))
                d[bx + px] = colour;
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

/* the player's guns on the own ship's wire (the weapons page): a dot at each gun's mount across the ship, on
 * the hull's leading edge there: amber a gun the trigger fires, grey one the choice leaves out, red a destroyed one */
static void gun_marks(int type, int ix, int iy, int inner, int y0, int y1)
{
    const unsigned char __far *m = collide_mask((uint16_t)type);
    int bx, by, bw, bh, g;
    wire_box(type, ix, iy, inner, inner, &bx, &by, &bw, &bh);
    for (g = 0; g < GUN_COUNT(0); g++) {
        const RvWeapon __far *w = weapon(GUN_TYPE(0, g));
        int px, py, col;
        if (!w || (w->kind != RV_WK_GUN && w->kind != RV_WK_TURRET))
            continue;
        px = bw / 2 + (int)((long)GUN_MOUNT_X(0, g) * (bw / 2) / 32);   /* (the mount: 32nds of the half box) */
        if (px < 0) px = 0;
        if (px >= bw) px = bw - 1;
        for (py = 0; py < bh - 1 && m && !solid(m, px, py, bw, bh); py++)
            ;                           /* (the leading edge there) */
        col = GUN_STATE(0, g) & GUN_DESTROYED ? UI_RED : logic_gun_selected((int16_t)g) ? UI_AMBER3 : UI_GREY4;
        gfx_fill(bx + px - 1, by + py - 1, 3, 3, (unsigned char)col, y0, y1);
    }
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
    if (s == 0 && g_page[1] == VDU_R_WEAPONS)
        gun_marks(type, ix, iy, inner, y0, y1);
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
        return;
    }
    _fstrncpy(name, g_sys.target_name, sizeof name - 1);
    name[sizeof name - 1] = 0;
    if (g_sys.target_km >= 0)
        sprintf(a, "%ld KM", g_sys.target_km);
    else
        strcpy(a, "-");
    sprintf(b, "HULL %d%%", hull_pct(s));
    ship_page(s, gInstanceShipTypes[s], r, name, a, b, y0, y1);
}

/* the damage page: the own ship, how many systems are hurt, and one of them named with its level as four boxes (the
 * old game's light, moderate, heavy, destroyed); D again names the next */
static void draw_damage(const RvRect __far *r, int y0, int y1)
{
    char top[16], a[16], b[4];
    int k, n = 0, at = -1, i = 0, x, y, lv;
    if (!slot_type(0))
        return;
    for (k = 0; k < SYS_COUNT; k++)
        if (systemDamage[k])
            n++;
    if (n) {
        for (k = 0; k < SYS_COUNT; k++)
            if (systemDamage[k] && i++ == (int)(g_cursor % n))
                at = k;
    }
    sprintf(top, n ? "DAMAGE %d" : "NO DAMAGE", n);
    a[0] = b[0] = 0;
    if (at >= 0)
        _fstrncpy(a, logic_system_name((int16_t)at), sizeof a - 1), a[sizeof a - 1] = 0;
    ship_page(0, gInstanceShipTypes[0], r, top, a, b, y0, y1);
    if (at < 0)
        return;
    /* the level: four boxes after the name, lit to the damage, red when it is destroyed */
    lv = systemDamage[at];
    if (r->w >= 2 * r->h) {
        x = r->x + r->h - 4 + 8;
        y = r->y + 3 + 2 * (FH + 1);
    } else {
        x = r->x + 3 + panel_tiny_width(a) + 5;
        y = r->y + r->h - FH - 2;
    }
    for (k = 0; k < 4; k++)
        if (k < lv)
            gfx_fill(x + k * 5, y, 4, PANEL_TINY_H, lv >= SYS_WRECKED ? UI_RED : UI_AMBER3, y0, y1);
        else
            gfx_frame(x + k * 5, y, 4, PANEL_TINY_H, UI_GREY4, y0, y1);
}

/* the weapons page: the guns the trigger fires (all, or one type), the own ship with its guns marked, and the
 * missile the missile key fires with its rounds */
static void draw_weapons(const RvRect __far *r, int y0, int y1)
{
    char top[16], a[16], b[8];
    uint16_t types[8];
    int n = logic_gun_types(types, 8), l = logic_launcher(), g, rounds = 0;
    const RvWeapon __far *w;
    if (!slot_type(0))
        return;
    if (!g_sys.gun_sel || g_sys.gun_sel > n)
        strcpy(top, "GUNS: ALL");
    else {
        char code[RV_CODE + 1];
        _fstrncpy(code, weapon(types[g_sys.gun_sel - 1])->code, RV_CODE);
        code[RV_CODE] = 0;
        sprintf(top, "GUNS: %s", code);
    }
    if (l >= 0 && (w = weapon(GUN_TYPE(0, l))) != 0) {
        char code[RV_CODE + 1];
        _fstrncpy(code, w->code, RV_CODE);
        code[RV_CODE] = 0;
        for (g = 0; g < GUN_COUNT(0); g++)   /* (the rounds of every launcher of that weapon) */
            if (GUN_TYPE(0, g) == GUN_TYPE(0, l) && !(GUN_STATE(0, g) & GUN_DESTROYED))
                rounds += GUN_STATE(0, g) & 0x7F;
        sprintf(a, "%s", code);
        sprintf(b, "x%d", rounds);
    } else {
        strcpy(a, "NO MISSILE");
        b[0] = 0;
    }
    ship_page(0, gInstanceShipTypes[0], r, top, a, b, y0, y1);
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

/* the nav page: the mission's nav points with how far each is, the chosen one lit (N: the next) */
static void draw_nav(const RvRect __far *r, int y0, int y1)
{
    char lines[16][20];
    int n = 0, k;
    for (k = 0; k < 16 && n < 16; k++) {
        int32_t km;
        if (!logic_nav_km(k, &km))
            continue;
        if (k == 0)
            sprintf(lines[n], "HOME %ldK", (km + 500) / 1000);
        else
            sprintf(lines[n], "NAV %d  %ldK", k, (km + 500) / 1000);
        n++;
    }
    if (!n) {
        text(r->x + 4, r->y + 3, "NO NAV", 0, y0, y1);
        return;
    }
    {   /* (the chosen one on the VDU: from the start of its page of rows) */
        int per = menu_rows(r), lit = g_sys.nav < n ? g_sys.nav : 0;
        (void)menu(r, lines, n, lit - lit % per, lit, y0, y1);
    }
}

/* the comm page: SEND TO and the wingmen and the carrier, or a wingman's orders, or the carrier's */
static void draw_comm(const RvRect __far *r, int y0, int y1)
{
    char lines[COMM_WINGMEN + 3][20];
    int16_t wing[COMM_WINGMEN];
    int n = 0, k, w;
    if (systemDamage[SYS_INTERCOM] >= SYS_WRECKED) {
        text(r->x + 4, r->y + 3, "INTERCOM OUT", 0, y0, y1);
        return;
    }
    if (g_comm_level == 0) {
        char name[12];
        strcpy(lines[n++], "SEND TO:");
        w = logic_wingmen(wing, COMM_WINGMEN);
        for (k = 0; k < w; k++) {
            wingman_name(wing[k], name, 9);
            sprintf(lines[n++], "%d %s", k + 1, name);
        }
        sprintf(lines[n++], "%d CARRIER", w + 1);
    } else if (g_comm_level == 1) {
        char name[12];
        wingman_name(g_comm_who, name, 9);
        sprintf(lines[n++], "%s:", name);
        for (k = 0; k < 5; k++) {
            char line[18];
            _fstrncpy(line, k_order_line[k_order_key[k]], sizeof line - 1);
            line[sizeof line - 1] = 0;
            sprintf(lines[n++], "%d %s", k + 1, line);   /* (18 characters, 72 px: the small VDU is 84) */
        }
    } else {
        strcpy(lines[n++], "CARRIER:");
        strcpy(lines[n++], "1 LANDING");
    }
    strcpy(lines[n++], "0 NEVER MIND");
    (void)menu(r, lines, n, 0, -1, y0, y1);
}

int vdu_draw(int which, const RvRect __far *r, int y0, int y1)
{
    if (!r->w || y1 <= r->y || y0 >= r->y + r->h)
        return 0;
    if (which == 0) {
        switch (g_page[0]) {
        case VDU_L_NAV: draw_nav(r, y0, y1); return 1;
        case VDU_L_COMM: draw_comm(r, y0, y1); return 1;
        default: return 0;
        }
    }
    if (which == 1) {
        switch (g_page[1]) {
        case VDU_R_TARGET: draw_target(r, y0, y1); return 1;
        case VDU_R_WEAPONS: draw_weapons(r, y0, y1); return 1;
        case VDU_R_DAMAGE: draw_damage(r, y0, y1); return 1;
        default: return 0;
        }
    }
    return 0;
}
