/* The visual effects' bounded pools, camera parallax and settling, without running the game. The source is
 * included to inspect the private effects state; unused engine paths are discarded by motion_test.sh.  A wreck's
 * debris is drawn on the host (debris.c: the card's code, as the native builds draw it), so it is drawn here too. */
#include <assert.h>
#include <stdio.h>
#include "../../src/fx.c"
#include "../../src/explode.c"
#include "../../src/hud.c"

static unsigned char picture[SCR_H][SCR_W];
unsigned char *vid_row(int y) { assert(y >= 0 && y < SCR_H); return picture[y]; }
/* (the blasts' own drawing, linked in with draw_one: never reached here, where only wrecks are drawn) */
int xms_get(XmsBlock b, unsigned long off, void *dst, unsigned long n) { (void)b; (void)off; (void)dst; (void)n; abort(); }
void rv_scale_row(const unsigned char *line, const unsigned short *map, int xa, int xb, unsigned char *dst)
{
    (void)line; (void)map; (void)xa; (void)xb; (void)dst;
    abort();
}

static void reset_effects(void)
{
    int i;
    g_ready = 1;
    g_n = g_nord = 0;
    g_nvar = 2;
    g_nramp = 20; g_nspark = 10; g_ndebris = 4;
    g_fx_extra = 1;                     /* (the bunk menu's EXTRA FX: the wrecks' debris) */
    for (i = 0; i < 34; i++) g_cols[i] = (uint8_t)(64 + i);
}

int main(void)
{
    RvVec3 at = {0, 0, 1500 * 256L}, vel = {128, 0, 0};
    LfVec eye = {0, 0, 0};
    LfBasis cam = {{0, 0, 256}, {256, 0, 0}, {0, 256, 0}};
    int i, j, n, pixels;
    int32_t x0;
    int16_t ext[200];
    uint32_t rng = g_vis;

    motion_frame(0, 0);
    for (i = 0; i < 60; i++) motion_frame(0, 0);
    assert(g_motion_bob == 0);
    motion_frame(200, 0);
    assert(g_motion_bob > 0 && g_motion_bob <= 2 * 256);
    motion_frame(400, 0);
    motion_frame(200, 0);
    assert(g_motion_bob < 0);           /* immediate braking reverses the kick, even during its cooldown */
    motion_frame(400, 0);
    for (i = 0; i < 60; i++) motion_frame(400, 0);
    assert(g_motion_bob == 0);           /* steady speed has no ongoing acceleration kick */
    motion_frame(200, 0);
    assert(g_motion_bob < 0);
    for (i = 0; i < 60; i++) motion_frame(200, 0);
    motion_frame(600, 1);
    assert(g_motion_bob > 2 * 256 && g_motion_bob <= 3 * 256);
    for (i = 0; i < 60; i++) motion_frame(0, 0);
    for (i = 0; i < 60; i++) motion_frame((uint16_t)(i / 2 * 10), 0);
    assert(g_motion_bob == 0);           /* repeated samples during smooth acceleration must not restart the kick */
    for (i = 0; i < 10000; i++) {
        motion_frame((uint16_t)i, i & 1);
        assert(abs(g_motion_bob) <= 3 * 256);
    }
    assert(g_vis == rng);

    reset_effects();
    g_fx_extra = 0;
    explode_add(&at, &vel, 100, BOOM_WRECK, 0, 1);
    assert(g_n == 0);                   /* no debris without the extra effects (the default) */
    explode_add(&at, &vel, 100, BOOM_SHIP, 0, 1);
    assert(g_n == 1);                   /* (the blast all the same) */
    reset_effects();
    for (i = 0; i < 100; i++) {
        explode_add(&at, &vel, 100, BOOM_WRECK, 0, (uint16_t)i);
        explode_frame();
        assert(g_n <= WRECK_CLOUDS);
    }
    for (i = 0; i < 30; i++) explode_frame();
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_nord > 0 && g_nord <= WRECK_CLOUDS);
    for (i = 1; i < g_nord; i++) assert(g_b[g_order[i - 1]].depth >= g_b[g_order[i]].depth);
    for (i = 0; i < EXPLODE_MAX; i++) explode_add(&at, &vel, 100, BOOM_SHIP, 0, (uint16_t)i);
    assert(g_n == EXPLODE_MAX);
    for (i = 0; i < g_n; i++) assert(g_b[i].kind == BOOM_SHIP);
    explode_add(&at, &vel, 100, BOOM_WRECK, 0, 0);
    for (i = 0; i < g_n; i++) assert(g_b[i].kind == BOOM_SHIP); /* debris cannot displace a blast */

    reset_effects();
    explode_add(&at, &vel, 100, BOOM_WRECK, 0, 7);
    for (i = 0; i < 60; i++) explode_frame();
    assert(g_b[0].x == at.x + 60 * vel.x && g_b[0].vx == vel.x); /* coasting, without drag */
    explode_place(&eye, &cam, 320, 140, 400);
    n = g_nord;
    assert(n == 1 && g_b[g_order[0]].kind == BOOM_WRECK && g_b[g_order[0]].on);
    x0 = g_b[g_order[0]].cam_x;
    eye.x = 100 * 256L;
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_nord == n && g_b[g_order[0]].cam_x < x0);   /* the eye moved right: the cloud is further left */
    cam.front.z = -256; cam.right.x = -256;
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_nord == 0);                /* looking away from the wreck */
    cam.front.z = 256; cam.right.x = 256;
    explode_place(&eye, &cam, 320, 140, 400);
    for (i = 0; i < 100; i++) { ext[2 * i] = 200; ext[2 * i + 1] = 440; }
    hud_band(100, 200, ext, 100, 100);
    explode_draw_begin();
    explode_draw_rest(100, 200, ext, 100, 100);   /* (the debris, clipped to the canopy: debris.c) */
    pixels = 0;
    for (i = 0; i < SCR_H; i++) for (j = 0; j < SCR_W; j++)
        if (picture[i][j]) {
            assert(i >= 100 && i < 200 && j >= 200 && j < 440);
            pixels++;
        }
    assert(pixels > 0);
    for (i = 60; i < 210; i++) explode_frame();
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_n == 0 && g_nord == 0);
    printf("MOTION OK: acceleration settling, no wrecks without the extra effects, bounded wrecks, parallax, lifetime and canopy clipping; "
           "%d pixels of debris in the band\n", pixels);
    return 0;
}
