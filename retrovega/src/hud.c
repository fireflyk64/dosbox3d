/* hud.c: drawing over the flight's view (hud.h), clipped to the band and the opening's extents. */
#include <string.h>
#include <stdlib.h>
#include "rv.h"
#include "hud.h"

static int16_t __far g_y0, __far g_y1, __far g_ey0, __far g_rows;
static const int16_t __far *__far g_ext;

void hud_band(int16_t y0, int16_t y1, const int16_t __far *ext, int16_t ext_y0, int16_t ext_rows)
{
    g_y0 = y0;
    g_y1 = y1;
    g_ext = ext;
    g_ey0 = ext_y0;
    g_rows = ext_rows;
}

void hud_run(int16_t xa, int16_t xb, int16_t y, int16_t colour)
{
    uint8_t __far *row;
    if (y < g_y0 || y >= g_y1)
        return;
    if (g_ext) {
        if (y < g_ey0 || y >= g_ey0 + g_rows)
            return;
        if (xa < g_ext[2 * (y - g_ey0)]) xa = g_ext[2 * (y - g_ey0)];
        if (xb > g_ext[2 * (y - g_ey0) + 1]) xb = g_ext[2 * (y - g_ey0) + 1];
    }
    if (xa < 0) xa = 0;
    if (xb > SCR_W) xb = SCR_W;
    if (xa >= xb)
        return;
    row = vid_row(y);
    _fmemset(row + xa, colour, xb - xa);
}

void hud_dot(int16_t x, int16_t y, int16_t colour) { hud_run(x, x + 1, y, colour); }

void hud_vline(int16_t x, int16_t ya, int16_t yb, int16_t colour)
{
    if (ya < g_y0) ya = g_y0;
    if (yb > g_y1) yb = g_y1;
    for (; ya < yb; ya++)
        hud_run(x, x + 1, ya, colour);
}

/* a line, a point a step along its longer axis (Bresenham's: no division, a 32-bit one being a library call on DOS),
 * and nothing at all for a band it does not touch: the flight draws its view in bands and calls the HUD's lines in
 * each (the rails and the wreck fragments cost 2.6 ms a frame that way, 2026-11-02) */
void hud_line(int16_t xa, int16_t ya, int16_t xb, int16_t yb, int16_t colour)
{
    int16_t dx = abs(xb - xa), dy = abs(yb - ya), sx = xa < xb ? 1 : -1, sy = ya < yb ? 1 : -1, err, n;
    if ((ya < g_y0 && yb < g_y0) || (ya >= g_y1 && yb >= g_y1))
        return;
    if (dx > 2000 || dy > 2000)
        return;
    err = (dx > dy ? dx : -dy) / 2;
    for (n = dx > dy ? dx : dy; n >= 0; n--) {
        int16_t e = err;
        hud_run(xa, xa + 1, ya, colour);
        if (e > -dx) {
            err -= dy;
            xa += sx;
        }
        if (e < dy) {
            err += dx;
            ya += sy;
        }
    }
}

void hud_cross(int16_t x, int16_t y, int16_t arm, int16_t colour)
{
    hud_run(x - arm, x + arm + 1, y, colour);
    hud_vline(x, y - arm, y + arm + 1, colour);
}

void hud_brackets(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t colour)
{
    int16_t aw = (x1 - x0) / 4, ah = (y1 - y0) / 4;
    if (aw > 10) aw = 10;
    if (ah > 10) ah = 10;
    if (aw < 2) aw = 2;
    if (ah < 2) ah = 2;
    hud_run(x0, x0 + aw, y0, colour);
    hud_run(x1 - aw, x1, y0, colour);
    hud_run(x0, x0 + aw, y1 - 1, colour);
    hud_run(x1 - aw, x1, y1 - 1, colour);
    hud_vline(x0, y0, y0 + ah, colour);
    hud_vline(x1 - 1, y0, y0 + ah, colour);
    hud_vline(x0, y1 - ah, y1, colour);
    hud_vline(x1 - 1, y1 - ah, y1, colour);
}

void hud_diamond(int16_t x, int16_t y, int16_t r, int16_t colour)
{
    int16_t k;
    for (k = 0; k <= r; k++) {
        hud_dot(x - r + k, y - k, colour);
        hud_dot(x + r - k, y - k, colour);
        hud_dot(x - r + k, y + k, colour);
        hud_dot(x + r - k, y + k, colour);
    }
}
