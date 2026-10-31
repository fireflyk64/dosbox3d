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
#define BAR 2                           /* an armour bar's thickness, px */
#define GAP 2                           /* between the wire and the armour, the armour and the shield */

static unsigned char __far g_page[2];
static unsigned char __far g_cursor;    /* the damage page's line: which hurt system it names (D again: the next) */

void vdu_flight_start(void)
{
    g_page[0] = VDU_L_STATUS;
    g_page[1] = VDU_R_TARGET;
    g_cursor = 0;
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

/* a line cut to a width: the text's last characters dropped until it fits */
static void fit(char *s, int w)
{
    int n = (int)strlen(s);
    while (n > 0 && gfx_text_width(FONT_BODY, s) > w)
        s[--n] = 0;
}

/* a ship's page (the target's, the own ship's): the wire with its bars, and three lines of text beside it on a wide
 * VDU, else one above and one below */
static void ship_page(int s, int type, const RvRect __far *r, char *top, const char *a, const char *b, int y0, int y1)
{
    int fh = gfx_font_height(FONT_BODY), side, x, y;
    if (r->w >= 2 * r->h) {             /* wide and short: the ship at the left, the lines at the right */
        side = r->h - 4;
        vdu_ship(type, s, r->x + 2, r->y + 2, side, y0, y1);
        x = r->x + side + 8;
        y = r->y + 2;
        fit(top, r->x + r->w - 3 - x);
        gfx_text(FONT_BODY, x, y, top, RV_TEXT_AMBER, y0, y1);
        gfx_text(FONT_BODY, x, y + fh + 1, a, RV_TEXT_AMBER, y0, y1);
        gfx_text(FONT_BODY, x, y + 2 * (fh + 1), b, RV_TEXT_AMBER, y0, y1);
    } else {                            /* a line above, the ship, a line below */
        side = r->h - 2 * (fh + 2) - 2;
        if (side > r->w - 4)
            side = r->w - 4;
        fit(top, r->w - 6);
        gfx_text(FONT_BODY, r->x + 3, r->y + 2, top, RV_TEXT_AMBER, y0, y1);
        vdu_ship(type, s, r->x + (r->w - side) / 2, r->y + fh + 3, side, y0, y1);
        x = r->x + 3;
        y = r->y + r->h - fh - 2;
        gfx_text(FONT_BODY, x, y, a, RV_TEXT_AMBER, y0, y1);
        gfx_text(FONT_BODY, x + gfx_text_width(FONT_BODY, a) + 6, y, b, RV_TEXT_AMBER, y0, y1);
    }
}

/* the target page: its name, its range, its hull and the ship */
static void draw_target(const RvRect __far *r, int y0, int y1)
{
    char name[12], a[16], b[16];
    int s = g_sys.target;
    if (s < 0 || !slot_type((int16_t)s)) {
        gfx_text(FONT_BODY, r->x + 4, r->y + 3, "NO TARGET", RV_TEXT_AMBER, y0, y1);
        return;
    }
    _fstrncpy(name, g_sys.target_name, sizeof name - 1);
    name[sizeof name - 1] = 0;
    if (g_sys.target_km >= 0)
        sprintf(a, "%ldK", (g_sys.target_km + 500) / 1000);
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
    int k, n = 0, at = -1, i = 0, fh = gfx_font_height(FONT_BODY), x, y, lv;
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
    sprintf(top, n ? "DAMAGE %d" : "INTACT", n);
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
        y = r->y + 2 + 2 * (fh + 1) + 1;
    } else {
        x = r->x + 3 + gfx_text_width(FONT_BODY, a) + 5;
        y = r->y + r->h - fh - 2 + 1;
    }
    for (k = 0; k < 4; k++)
        if (k < lv)
            gfx_fill(x + k * 5, y, 4, fh - 3, lv >= SYS_WRECKED ? UI_RED : UI_AMBER3, y0, y1);
        else
            gfx_frame(x + k * 5, y, 4, fh - 3, UI_GREY4, y0, y1);
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
        strcpy(top, "ALL GUNS");         /* (cut to the VDU's width where it must be: "ALL GUN" reads) */
    else {
        char code[RV_CODE + 1];
        _fstrncpy(code, weapon(types[g_sys.gun_sel - 1])->code, RV_CODE);
        code[RV_CODE] = 0;
        sprintf(top, "%s ONLY", code);
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

int vdu_draw(int which, const RvRect __far *r, int y0, int y1)
{
    if (!r->w || y1 <= r->y || y0 >= r->y + r->h)
        return 0;
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
