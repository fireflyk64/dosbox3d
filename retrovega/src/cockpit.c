/* cockpit.c: the cockpit's picture (docs/engine.md, "Flight").
 *
 * A band of rows is drawn back to front: the base picture restored from XMS
 * (one move a band: the opening is black in it), the world inside the
 * opening's row extents, the struts the base paints inside those extents,
 * then the instruments: the survival display (shields fore and aft, armour
 * on four sides), speed and fuel, the radar's contacts, the MFDs, the
 * throttle slid by the set speed, the joystick at one of nine places, and
 * the damage overlays whose bits are set, each with its control when it is
 * attached to one (a canopy of cracks: random ones at random places, as the
 * hull gives: cracks_update); the panel's lamps, knobs, needles and readouts (panel.c)
 * over the instruments.  cockpit_dirty marks the rows a frame changes: the
 * opening and the radar always, the rest only when their values moved. */
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "rvhooks.h"
#include "cockpit.h"
#include "panel.h"
#include "damage.h"
#include "lightfield.h"

#define EXT_SHORTS (2 * SCR_H + 1)       /* an opening's rows: a pair each */
MEM_ARRAY(RvCockpit, g_cp_arr, 1)
MEM_ARRAY(short, g_ext_arr, EXT_SHORTS)
MEM_ARRAY(short, g_ext_full_arr, EXT_SHORTS)
static RvCockpit __far *g_cp;
static short __far *g_ext;
static int g_ext_y0, g_ext_rows;
static CockpitState g_last;
static int g_last_valid;

/* the glass's cracks (tools/glasscracks.py; the user, 2026-11-06: "load in random ones at random places when those
 * damages hit. Hopefully that makes it feel less wrote"): a cockpit's canopy layer RV_ATTACH_GLASS is a row of cracks
 * and a table of orders, each RV_CRACKS of them at places spread over the glass (RV.DAT's, so the choosing costs no
 * conventional memory).  At the canopy's first hurt a random order, its first two; one more every tenth of the hull
 * lost after and at each of the other canopy bits; they stay till the canopy bits are clear again (a new flight, or the
 * mission again) */
#define CANOPY_BITS (1UL << RV_CPD_CANOPY | 1UL << RV_CPD_CANOPY_RIGHT | 1UL << RV_CPD_CANOPY_OVERHEAD)
typedef struct { unsigned short sprite; short dx, dy; } Crack;   /* (an order's entry: the sprite from the first crack) */
static Crack g_crack[RV_CRACKS];        /* (the list's cap RV_CRACKS, an order's length: crack_add keeps it) */
static unsigned char g_ncrack, g_crack_order;
static uint16_t g_crack_rand = 0x2545;  /* (the cracks' own random stream, never the play's) */
static int g_cockpit_index = -1;
static unsigned char __far g_last_page;   /* the survival display's page last drawn */

#define STICK_DX 4
#define STICK_DY 3

const RvCockpit __far *cockpit_record(void) { return g_cp; }

static short __far *g_ext_full;         /* the opening's rows at full width (the external views) */
static int g_external;

const short __far *cockpit_extents(int *y0, int *rows)
{
    *y0 = g_ext_y0;
    *rows = g_ext_rows;
    return g_external && g_ext_full ? g_ext_full : g_ext;
}

/* --- the simulator's windshield (the user, 2026-11-01: the rig in the bar is brown with a square screen, so the cockpit
 * stays brown and "looks like a windshield": "squarier ... cut off part of the diagonals above the VDUs ... draw edges of
 * the screen as glassy") --- */

#define PANE_MARGIN 3                   /* the pane's gap from the screen's edges */
#define PANE_TOP_R 14                   /* its corners' radii */
#define PANE_LOW_R 7
#define GLASS_LEVELS 3

static int g_shield_y;                  /* above this row: no frame */

static int corner_inset(int r, int from_end)   /* how far a rounded corner of radius r comes in, from_end rows from its edge */
{
    long d = r - from_end;
    return from_end >= r ? 0 : r - (int)lf_sqrt((unsigned long)((long)r * r - d * d));
}

void cockpit_windshield(int bottom)
{
    int y;
    if (!g_ext || bottom > g_ext_y0 + g_ext_rows)
        return;
    for (y = g_ext_y0; y < bottom; y++) {
        int k = y - g_ext_y0, in;
        if (y < PANE_MARGIN) {          /* (an empty span) */
            g_ext[2 * k] = g_ext[2 * k + 1] = SCR_W / 2;
            continue;
        }
        in = corner_inset(PANE_TOP_R, y - PANE_MARGIN);
        if (bottom - 1 - y < PANE_LOW_R)
            in = corner_inset(PANE_LOW_R, bottom - 1 - y);
        g_ext[2 * k] = (short)(PANE_MARGIN + in);
        g_ext[2 * k + 1] = (short)(SCR_W - PANE_MARGIN - in);
    }
    g_shield_y = bottom;
}

/* the edge's three levels, along every edge of the view (the span's ends, and what a row has that the rows beside it have
 * not), brightest outermost; and a reflection's two streaks across a corner of the pane */
static void glass_edges(int y0, int y1)
{
    static const unsigned char __far k_tint[GLASS_LEVELS][2] = { { 0, 2 }, { 2, 1 }, { 0, 0 } };   /* (c3, level): white
                                           75%, grey 50%, white 25% */
    int y, d, k;
    const int ey1 = g_ext_y0 + g_ext_rows;
    for (y = y0 > g_ext_y0 ? y0 : g_ext_y0; y < y1 && y < ey1; y++) {
        int x0 = g_ext[2 * (y - g_ext_y0)], x1 = g_ext[2 * (y - g_ext_y0) + 1];
        if (x0 >= x1)
            continue;
        for (d = 1; d <= GLASS_LEVELS; d++) {
            int a = x0 + 1, b = x1 - 1;         /* (this level's inside: past it, the next level's) */
            for (k = 1; k <= d; k++) {
                int up = y - k - g_ext_y0, dn = y + k - g_ext_y0;
                if (up < 0 || dn >= g_ext_rows) {
                    a = b;                      /* (off the view: nothing inside at this level) */
                    break;
                }
                if (g_ext[2 * up] > a) a = g_ext[2 * up];
                if (g_ext[2 * dn] > a) a = g_ext[2 * dn];
                if (g_ext[2 * up + 1] < b) b = g_ext[2 * up + 1];
                if (g_ext[2 * dn + 1] < b) b = g_ext[2 * dn + 1];
            }
            if (a >= b) {
                gfx_mix_run(x0, x1, y, k_tint[d - 1][0], k_tint[d - 1][1], y0, y1);
                x0 = x1;
                break;
            }
            gfx_mix_run(x0, a, y, k_tint[d - 1][0], k_tint[d - 1][1], y0, y1);
            gfx_mix_run(b, x1, y, k_tint[d - 1][0], k_tint[d - 1][1], y0, y1);
            x0 = a;
            x1 = b;
        }
    }
    /* the reflection: top right, slanting as the light through a canopy does */
    for (y = y0 > PANE_MARGIN + 4 ? y0 : PANE_MARGIN + 4; y < y1 && y < 64; y++) {
        int x = SCR_W - 96 - y;
        gfx_mix_run(x, x + 16, y, 0, 0, y0, y1);
        gfx_mix_run(x + 24, x + 28, y, 0, 0, y0, y1);
    }
}

void cockpit_external(int on)
{
    g_external = on;
    vid_dirty(0, SCR_H);
}

int cockpit_is_external(void) { return g_external; }

int cockpit_load(int ship_type)
{
    const RvDatHeader __far *h = dat_header();
    unsigned i;
    cockpit_unload();
    for (i = 0; i < h->ncockpits; i++) {
        const RvCockpit __far *c = dat_cockpit(i);
        if (c->ship_type == ship_type) {
            const RvAsset __far *ex = dat_asset(c->extents);
            g_cp = MEM_TAKE(RvCockpit, g_cp_arr, 1);
            RV_ASSERT(g_cp != 0);
            _fmemcpy(g_cp, c, sizeof(RvCockpit));
            g_ext_y0 = c->open_y0;
            g_ext_rows = c->open_y1 - c->open_y0;
            g_ext = MEM_TAKE(short, g_ext_arr, ex->size / 2 + 1);
            RV_ASSERT(g_ext != 0 && ex->size == 4UL * g_ext_rows);
            dat_read(ex->at, g_ext, ex->size);
            g_ext_full = MEM_TAKE(short, g_ext_full_arr, ex->size / 2 + 1);
            if (g_ext_full) {
                int r;
                for (r = 0; r < g_ext_rows; r++) {
                    g_ext_full[2 * r] = 0;
                    g_ext_full[2 * r + 1] = SCR_W;
                }
            }
            g_cockpit_index = (int)i;   /* (the palette goes in with cockpit_palette, once the flight's loading is done) */
            g_last_valid = 0;
            g_ncrack = 0;
            panel_load(g_cp);
            RV_LOG("cockpit: %.8Fs for ship type %d, rows %d-%d open, %d damage layers", (char __far *)c->key,
                   ship_type, c->open_y0, c->open_y1, c->nlayers);
            return 1;
        }
    }
    RV_LOG("cockpit: none for ship type %d", ship_type);
    return 0;
}

void cockpit_palette(void)
{
    if (g_cp)
        gfx_use_palette(g_cp->palette);
}

void cockpit_unload(void)
{
    panel_unload();
    if (g_cp) MEM_GIVE(g_cp);
    if (g_ext) MEM_GIVE(g_ext);
    if (g_ext_full) MEM_GIVE(g_ext_full);
    g_ext_full = 0;
    g_external = 0;
    g_shield_y = 0;
    g_cp = 0;
    g_ext = 0;
    g_cockpit_index = -1;
}

/* --- what a frame changes --- */

static void dirty_rect(const RvRect __far *r)
{
    if (r->w)
        vid_dirty(r->y, r->y + r->h);
}

static void dirty_sprite(unsigned a, int dy0, int dy1)
{
    const RvAsset __far *as;
    if (a == 0xFFFF)
        return;
    as = dat_asset(a);
    vid_dirty(as->y + (dy0 < dy1 ? dy0 : dy1), as->y + as->h + (dy0 > dy1 ? dy0 : dy1));
}

/* a crack more: the next of this flight's order (a random one at the first).  0: none to add */
static int crack_add(const RvDamageLayer __far *l)
{
    const RvAsset __far *o = dat_asset(l->sprite + l->n);
    unsigned n = (unsigned)o->size / (RV_CRACKS * sizeof(Crack));
    Crack *c = &g_crack[g_ncrack];
    if (!n || g_ncrack >= RV_CRACKS)
        return 0;
    if (!g_ncrack) {                    /* (when the hit came: a flight's cracks are not the last one's) */
        g_crack_rand = (uint16_t)((g_crack_rand + frameCounter) * 25173U + 13849U);
        g_crack_order = (unsigned char)((g_crack_rand >> 8) % n);
    }
    dat_read(o->at + sizeof(Crack) * ((unsigned long)g_crack_order * RV_CRACKS + g_ncrack), c, sizeof(Crack));
    c->sprite += l->sprite;
    g_ncrack++;
    dirty_sprite(c->sprite, c->dy, c->dy);
    return 1;
}

/* how many cracks the damage asks for */
static int cracks_wanted(const CockpitState *st)
{
    int n;
    if (!(st->damage & CANOPY_BITS))
        return 0;
    n = 2 + (st->hull < 50 ? (50 - st->hull) / 10 : 0);
    if (st->damage & 1UL << RV_CPD_CANOPY_RIGHT)
        n++;
    if (st->damage & 1UL << RV_CPD_CANOPY_OVERHEAD)
        n++;
    return n < RV_CRACKS ? n : RV_CRACKS;
}

static void cracks_update(const RvCockpit __far *c, const CockpitState *st)
{
    const RvDamageLayer __far *l = &c->layer[RV_CPD_CANOPY];
    int want = cracks_wanted(st), i;
    if (l->sprite == 0xFFFF || l->attach != RV_ATTACH_GLASS)
        return;
    if (!want) {
        for (i = 0; i < g_ncrack; i++)
            dirty_sprite(g_crack[i].sprite, g_crack[i].dy, g_crack[i].dy);
        g_ncrack = 0;
        return;
    }
    while (g_ncrack < want && crack_add(l))
        ;
}

/* the hand's pose on the stick (RvCockpit.stick): row forward (nose down: the steering's y < 0), upright, pulled
 * back (nose up: y > 0, the pointer below the middle); column left, upright, right */
static int stick_pose(const CockpitState *st)
{
    return (st->stick_y < 0 ? 0 : st->stick_y > 0 ? 2 : 1) * 3 + st->stick_x + 1;
}

/* the throttle's handle, by the set speed: its way at full (RvCockpit.throttle_dx, _dy: along the lever, its foot
 * staying in its track, gfx_sprite_lean), a part of it */
static int throttle_dy(const CockpitState *st)
{
    return st->max_speed ? (int)((long)st->set_speed * g_cp->throttle_dy / st->max_speed) : 0;
}

static int throttle_dx(const CockpitState *st)
{
    return st->max_speed ? (int)((long)st->set_speed * g_cp->throttle_dx / st->max_speed) : 0;
}

void cockpit_dirty(const CockpitState *st)
{
    const RvCockpit __far *c = g_cp;
    unsigned long changed;
    int i;
    if (!c)
        return;
    vid_dirty(c->open_y0, c->open_y1);
    vid_dirty(c->radar_y - c->radar_r, c->radar_y + c->radar_r + 1);
    panel_dirty(st);
    if (!g_last_valid) {
        vid_dirty(0, SCR_H);
        g_last = *st;
        g_last_valid = 1;
        return;
    }
    if (st->shield_f != g_last.shield_f || st->shield_r != g_last.shield_r || memcmp(st->armor, g_last.armor, sizeof st->armor)
        || g_sys.surv_page != g_last_page || (g_sys.surv_page && (frameCounter & 15) == 0))
        dirty_rect(&c->survival);
    g_last_page = g_sys.surv_page;
    if (st->speed != g_last.speed || st->set_speed != g_last.set_speed)
        dirty_rect(&c->speed);
    if (st->fuel != g_last.fuel)
        dirty_rect(&c->fuel);
    for (i = 0; i < 2; i++) {
        int k, diff = 0;
        for (k = 0; k < 4; k++)
            diff |= st->mfd_text[i][k] != g_last.mfd_text[i][k]
                    || (st->mfd_text[i][k] && strcmp(st->mfd_text[i][k], g_last.mfd_text[i][k]) != 0);
        if (diff || st->mfd_draw)
            dirty_rect(&c->mfd[i]);
    }
    if (st->set_speed != g_last.set_speed || st->max_speed != g_last.max_speed)
        dirty_sprite(c->throttle.sprite, throttle_dy(&g_last), throttle_dy(st));
    if (st->stick_x != g_last.stick_x || st->stick_y != g_last.stick_y) {
        if (c->stick != 0xFFFF) {       /* (the hand's old pose and its new one) */
            dirty_sprite(c->stick + stick_pose(&g_last), 0, 0);
            dirty_sprite(c->stick + stick_pose(st), 0, 0);
        } else {
            dirty_sprite(c->joystick.sprite, g_last.stick_y * STICK_DY, st->stick_y * STICK_DY);
        }
    }
    changed = st->damage ^ g_last.damage;
    for (i = 0; i < RV_CP_LAYERS; i++)
        if ((changed >> i) & 1)
            dirty_sprite(c->layer[i].sprite, 0, 0);
    cracks_update(c, st);
    g_last = *st;
}

/* --- the instruments --- */

static unsigned char level_colour(int pct)
{
    return pct > 66 ? UI_HILITE : pct > 33 ? UI_AMBER4 : UI_RED;
}

/* a bar of the rect's width: the part up to pct lit */
static void bar(const RvRect __far *r, int pct, int y0, int y1)
{
    int w = (int)((long)r->w * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100);
    gfx_fill(r->x, r->y, r->w, r->h, UI_GREY1, y0, y1);
    if (w > 0)
        gfx_fill(r->x, r->y, w, r->h, level_colour(pct), y0, y1);
}

/* shields as arcs fore and aft, armour as four bars round a hull */
static void survival(const RvRect __far *r, const CockpitState *st, int y0, int y1)
{
    int cx = r->x + r->w / 2, cy = r->y + r->h / 2, hw = r->w * 3 / 10, hh = r->h * 3 / 10, i, k;
    int pct[4];
    if (!r->w || y1 <= r->y || y0 >= r->y + r->h || panel_has(RV_PF_SURVIVAL))   /* (the panel's map: panel.c) */
        return;
    if (g_sys.surv_page) {              /* (its other pages: the hull and how many systems are damaged; the nav) */
        char a[16], b[16];
        int n = 0, h = gfx_font_height(FONT_BODY);
        for (i = 0; i < SYS_COUNT; i++)
            n += sys_level((int16_t)i) != 0;
        if (g_sys.surv_page == 1) {
            panel_hull_name(a);
            sprintf(b, "DMG %d", n);
        } else {
            panel_nav_name(a);
            if (g_sys.nav_km < 0) strcpy(b, "-");
            else sprintf(b, "%ldK", g_sys.nav_km / 1000);
        }
        gfx_text(FONT_BODY, r->x + 2, r->y + (r->h - 2 * h) / 2, a, RV_TEXT_AMBER, y0, y1);
        gfx_text(FONT_BODY, r->x + 2, r->y + (r->h - 2 * h) / 2 + h, b, RV_TEXT_AMBER, y0, y1);
        return;
    }
    /* the hull */
    gfx_frame(cx - hw / 2, cy - hh / 2, hw, hh, UI_GREY4, y0, y1);
    /* armour: front and rear bars, left and right bars, shortened as they wear */
    for (k = 0; k < 4; k++)
        pct[k] = st->armor[k] < 0 ? 0 : st->armor[k] > 100 ? 100 : st->armor[k];
    i = (int)((long)hw * pct[0] / 100);
    gfx_fill(cx - i / 2, cy - hh / 2 - 4, i, 3, level_colour(pct[0]), y0, y1);
    i = (int)((long)hw * pct[1] / 100);
    gfx_fill(cx - i / 2, cy + hh / 2 + 1, i, 3, level_colour(pct[1]), y0, y1);
    i = (int)((long)hh * pct[2] / 100);
    gfx_fill(cx - hw / 2 - 4, cy - i / 2, 3, i, level_colour(pct[2]), y0, y1);
    i = (int)((long)hh * pct[3] / 100);
    gfx_fill(cx + hw / 2 + 1, cy - i / 2, 3, i, level_colour(pct[3]), y0, y1);
    /* shields: an arc of eight segments each side, lit from the middle outwards */
    for (k = 0; k < 2; k++) {
        int pc = k ? st->shield_r : st->shield_f, ry = hh / 2 + 7, rx = hw / 2 + 7, s;
        unsigned char col = level_colour(pc);
        if (pc < 0) pc = 0;
        for (s = 0; s < 8; s++) {
            /* segment s spans angles (s, s+1) of 8 over the half; lit when within pc of the middle */
            int from_mid = s < 4 ? 3 - s : s - 4;         /* 0 nearest the middle, 3 the ends */
            int xa, ya, xb, yb;
            if (from_mid * 25 >= pc)
                continue;
            xa = cx + (int)((long)rx * (s * 2 - 8) / 8);
            xb = cx + (int)((long)rx * ((s + 1) * 2 - 8) / 8);
            ya = cy + (k ? 1 : -1) * (int)((long)ry * (rx - (xa - cx < 0 ? cx - xa : xa - cx)) / rx);
            yb = cy + (k ? 1 : -1) * (int)((long)ry * (rx - (xb - cx < 0 ? cx - xb : xb - cx)) / rx);
            ya = k ? (ya > cy + ry ? cy + ry : ya) : (ya < cy - ry ? cy - ry : ya);
            yb = k ? (yb > cy + ry ? cy + ry : yb) : (yb < cy - ry ? cy - ry : yb);
            gfx_line(xa, ya, xb, yb, col, 0, y0, y1);
            gfx_line(xa, ya + (k ? 1 : -1), xb, yb + (k ? 1 : -1), col, 0, y0, y1);
        }
    }
}

static void radar(const RvCockpit __far *c, const CockpitState *st, int y0, int y1)
{
    int i, r = c->radar_r;
    if (y1 <= c->radar_y - r || y0 >= c->radar_y + r + 1)
        return;
    if (st->radar_level < SYS_WRECKED) {
        gfx_circle(c->radar_x, c->radar_y, r - 1, UI_GREY2, 1, y0, y1);
        gfx_fill(c->radar_x - 2, c->radar_y, 5, 1, UI_GREY4, y0, y1);
        gfx_fill(c->radar_x, c->radar_y - 2, 1, 5, UI_GREY4, y0, y1);
    }
    if (st->radar_level) {              /* the scope hurt: snow, more the worse, on a dead one a last few specks */
        uint16_t seed = (uint16_t)(frameCounter * 25173U + 13849U);
        int n = st->radar_level >= SYS_WRECKED ? 4 : st->radar_level * 6;
        for (i = 0; i < n; i++) {
            int x, y;
            seed = (uint16_t)(seed * 25173U + 13849U);
            x = (int)((uint16_t)(seed >> 4) % (uint16_t)(2 * r)) - r;
            seed = (uint16_t)(seed * 25173U + 13849U);
            y = (int)((uint16_t)(seed >> 4) % (uint16_t)(2 * r)) - r;
            if ((long)x * x + (long)y * y < (long)(r - 2) * (r - 2))
                gfx_fill(c->radar_x + x, c->radar_y + y, 1, 1, UI_GREY4, y0, y1);
        }
    }
    for (i = 0; i < st->ncontacts && i < CP_CONTACTS; i++) {
        int x = c->radar_x + (int)((long)st->contact[i].x * (r - 2) / 256);
        int y = c->radar_y + (int)((long)st->contact[i].y * (r - 2) / 256);
        if (st->contact[i].target) {    /* the chosen target: a 5-pixel + in its own colour, friend or foe (the user's) */
            gfx_fill(x - 2, y, 5, 1, st->contact[i].colour, y0, y1);
            gfx_fill(x, y - 2, 1, 5, st->contact[i].colour, y0, y1);
        } else
            gfx_fill(x - 1, y - 1, 2, 2, st->contact[i].colour, y0, y1);
    }
}

static void mfd(const RvRect __far *r, int which, const CockpitState *st, int y0, int y1)
{
    int k, y = r->y + 3;
    if (!r->w || y1 <= r->y || y0 >= r->y + r->h)
        return;
    if (st->mfd_draw)
        st->mfd_draw(which, r, y0, y1);
    for (k = 0; k < 4 && st->mfd_text[which][k]; k++) {
        if (y + gfx_font_height(FONT_BODY) > r->y + r->h)
            break;
        gfx_text(FONT_BODY, r->x + 4, y, st->mfd_text[which][k], RV_TEXT_AMBER, y0, y1);
        y += gfx_font_height(FONT_BODY) + 1;
    }
}

static void speed_fuel(const RvCockpit __far *c, const CockpitState *st, int y0, int y1)
{
    char s[24];
    const RvRect __far *r = &c->speed;
    if (r->w && y1 > r->y && y0 < r->y + r->h) {
        if (r->h >= gfx_font_height(FONT_BODY)) {
            sprintf(s, "%d", st->speed);
            gfx_text(FONT_BODY, r->x + 2, r->y + (r->h - gfx_font_height(FONT_BODY)) / 2, s, RV_TEXT_AMBER, y0, y1);
        } else {
            bar(r, st->max_speed ? (int)((long)st->speed * 100 / st->max_speed) : 0, y0, y1);
        }
    }
    r = &c->fuel;
    if (r->w >= 12 && y1 > r->y && y0 < r->y + r->h)    /* (a lamp-sized one is the panel's fuel lamp) */
        panel_fuel_bar(r->x, r->y, r->w, r->h, st->fuel, r->h > r->w, y0, y1);   /* (backlit: the user's) */
}

int cockpit_layer_box(int bit, int *x, int *y, int *w, int *h)
{
    const RvAsset __far *as;
    if (!g_cp || bit < 0 || bit >= RV_CP_LAYERS || g_cp->layer[bit].sprite == 0xFFFF)
        return 0;
    as = dat_asset(g_cp->layer[bit].sprite);
    *x = as->x;
    *y = as->y;
    if (g_cp->layer[bit].attach == RV_ATTACH_GLASS && g_ncrack) {   /* (cracks: the newest's) */
        as = dat_asset(g_crack[g_ncrack - 1].sprite);
        *x = as->x + g_crack[g_ncrack - 1].dx;
        *y = as->y + g_crack[g_ncrack - 1].dy;
    }
    *w = as->w;
    *h = as->h;
    return 1;
}

static void damage_layers(const RvCockpit __far *c, const CockpitState *st, int attach, int dx, int dy, int y0, int y1)
{
    int i;
    for (i = 0; i < RV_CP_LAYERS; i++)
        if (((st->damage >> i) & 1) && c->layer[i].sprite != 0xFFFF && c->layer[i].attach == attach)
            gfx_sprite(c->layer[i].sprite, dx, dy, y0, y1);
}

void cockpit_draw(int y0, int y1, const CockpitState *st, void (*world)(int y0, int y1))
{
    const RvCockpit __far *c = g_cp;
    int sdx, sdy, i;
    if (!c || g_external) {             /* (an outside view: the opening's rows full width between black bars) */
        gfx_fill(0, 0, SCR_W, SCR_H, UI_BLACK, y0, y1);
        if (world)
            world(y0, y1);
        return;
    }
    if (g_shield_y && y0 < g_shield_y) {
        gfx_fill(0, y0, SCR_W, (y1 < g_shield_y ? y1 : g_shield_y) - y0, UI_BLACK, y0, y1);
        if (y1 > g_shield_y)
            gfx_image(c->base, 0, 0, g_shield_y, y1);
    } else
        gfx_image(c->base, 0, 0, y0, y1);
    if (world && y1 > c->open_y0 && y0 < c->open_y1)
        world(y0, y1);
    if (g_shield_y) {
        glass_edges(y0, y1);
        if (y1 <= g_shield_y)           /* (nothing of the frame up here) */
            return;
        if (y0 < g_shield_y)
            y0 = g_shield_y;
    }
    if (c->struts != 0xFFFF)
        gfx_sprite(c->struts, 0, 0, y0, y1);
    survival(&c->survival, st, y0, y1);
    speed_fuel(c, st, y0, y1);
    radar(c, st, y0, y1);
    mfd(&c->mfd[0], 0, st, y0, y1);
    mfd(&c->mfd[1], 1, st, y0, y1);
    panel_draw(st, y0, y1);             /* (the lamps, the dials, the readouts: panel.c) */
    damage_layers(c, st, 0, 0, 0, y0, y1);
    for (i = 0; i < g_ncrack; i++)      /* (the glass's cracks: cracks_update) */
        gfx_sprite(g_crack[i].sprite, g_crack[i].dx, g_crack[i].dy, y0, y1);
    gfx_sprite_lean(c->throttle.sprite, c->throttle_pivot, throttle_dx(st), throttle_dy(st), y0, y1);
    damage_layers(c, st, 1, 0, 0, y0, y1);   /* (none are packed: rvpack, no broken throttle) */
    if (c->stick != 0xFFFF) {           /* the pilot's hand on the stick, in its pose */
        gfx_sprite(c->stick + stick_pose(st), 0, 0, y0, y1);
    } else {
        sdx = st->stick_x * STICK_DX;
        sdy = st->stick_y * STICK_DY;
        gfx_sprite(c->joystick.sprite, sdx, sdy, y0, y1);
        damage_layers(c, st, 2, sdx, sdy, y0, y1);
    }
}
