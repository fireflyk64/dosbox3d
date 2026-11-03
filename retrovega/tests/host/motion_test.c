/* The visual effects' bounded pools, camera parallax and settling, without running the game. The source is
 * included to inspect the private effects state; unused engine paths are discarded by motion_test.sh. */
#include <assert.h>
#include <stdio.h>
#include "../../src/fx.c"
#include "../../src/explode.c"
#include "../../src/hud.c"

static unsigned char picture[SCR_H][SCR_W];
unsigned char *vid_row(int y) { assert(y >= 0 && y < SCR_H); return picture[y]; }

static void reset_effects(void)
{
    int i;
    g_ready = 1;
    g_n = g_nord = g_nfrag = g_frag_next = 0;
    g_nvar = 2;
    g_nramp = 20; g_nspark = 10; g_ndebris = 4;
    for (i = 0; i < 34; i++) g_cols[i] = (uint8_t)(64 + i);
}

int main(void)
{
    RvVec3 at = {0, 0, 1500 * 256L}, vel = {128, 0, 0};
    LfVec eye = {0, 0, 0};
    LfBasis cam = {{0, 0, 256}, {256, 0, 0}, {0, 256, 0}};
    int i, j, n, pixels;
    int16_t x[WRECK_PIECES], ext[200];
    uint16_t phase, slow;
    uint32_t rng = g_vis;

    motion_frame(0, 400, 0);
    phase = g_flow_phase;
    for (i = 0; i < 60; i++) motion_frame(0, 400, 0);
    assert(g_flow_phase == phase && g_motion_bob == 0);
    motion_frame(200, 400, 0);
    assert(g_motion_bob > 0 && g_motion_bob <= 2 * 256);
    slow = g_flow_phase;
    motion_frame(400, 400, 0);
    assert((uint16_t)(g_flow_phase - slow) > slow - phase);
    motion_frame(200, 400, 0);
    assert(g_motion_bob < 0);           /* immediate braking reverses the kick, even during its cooldown */
    motion_frame(400, 400, 0);
    for (i = 0; i < 60; i++) motion_frame(400, 400, 0);
    assert(g_motion_bob == 0);           /* steady speed has no ongoing acceleration kick */
    motion_frame(200, 400, 0);
    assert(g_motion_bob < 0);
    for (i = 0; i < 60; i++) motion_frame(200, 400, 0);
    motion_frame(600, 400, 1);
    assert(g_motion_bob > 2 * 256 && g_motion_bob <= 3 * 256);
    for (i = 0; i < 60; i++) motion_frame(0, 400, 0);
    for (i = 0; i < 60; i++) motion_frame((uint16_t)(i / 2 * 10), 400, 0);
    assert(g_motion_bob == 0);           /* repeated samples during smooth acceleration must not restart the kick */
    for (i = 0; i < 10000; i++) {
        motion_frame((uint16_t)i, 1, i & 1);
        assert(g_flow_phase < 48 * 256 && abs(g_motion_bob) <= 3 * 256);
    }
    assert(g_vis == rng);

    reset_effects();
    for (i = 0; i < 100; i++) {
        explode_add(&at, &vel, 100, BOOM_WRECK, 0, (uint16_t)i);
        explode_frame();
        assert(g_n <= WRECK_CLOUDS);
    }
    for (i = 0; i < 30; i++) explode_frame();
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_nfrag > 0 && g_nfrag <= WRECK_PIECES);
    for (i = 1; i < g_nfrag; i++) assert(g_fragment[i - 1].depth >= g_fragment[i].depth);
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
    n = g_nfrag;
    assert(n > 0);
    for (i = 0; i < n; i++) x[i] = g_fragment[i].x;
    eye.x = 100 * 256L;
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_nfrag == n);
    for (i = 0; i < n; i++) assert(g_fragment[i].x < x[i]);
    cam.front.z = -256; cam.right.x = -256;
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_nfrag == 0);               /* looking away from the wreck */
    cam.front.z = 256; cam.right.x = 256;
    explode_place(&eye, &cam, 320, 140, 400);
    for (i = 0; i < 100; i++) { ext[2 * i] = 200; ext[2 * i + 1] = 440; }
    hud_band(100, 200, ext, 100, 100);
    fx_hud(320, 140);
    for (i = 0; i < g_nfrag; i++) fragment_draw(&g_fragment[i]);
    pixels = 0;
    for (i = 0; i < SCR_H; i++) for (j = 0; j < SCR_W; j++)
        if (picture[i][j]) {
            assert(i >= 100 && i < 200 && j >= 200 && j < 440);
            pixels++;
        }
    assert(pixels > 0);
    for (i = 60; i < 210; i++) explode_frame();
    explode_place(&eye, &cam, 320, 140, 400);
    assert(g_n == 0 && g_nfrag == 0);
    printf("MOTION OK: speed flow, acceleration settling, bounded wrecks, parallax, lifetime and canopy clipping; %u bytes of projected fragments\n",
           (unsigned)sizeof g_fragment);
    return 0;
}
