/* planet_view.c: a mission's planet as the card makes it, from the packed game data (tests/host/planet_view.sh).
 * The mission's planet record and nav points read from RVGAME.DAT, made as the card makes them (tr_from_records,
 * tr_from_km, tr_begin and tr_build), its ground nav points put over the ground as PL_PLACE puts them, then drawn as
 * the flight draws (the sky, then the ground in bands of 100 rows into 640 x 400) from each nav point towards the
 * next, from high over the first, and low beside a canyon's wall; PPMs in the game's palette, the times printed.
 *
 *   planet_view RVGAME.DAT PALETTE.BIN OUTDIR MISSION_ID
 *   planet_view --map PALETTE.BIN OUT.PPM SEED RADIUS_KM CX CY CZ X0 Z0 SPAN_KM   (the ground seen from straight over
 *       the world's +y, the seed's alone: heights shaded, a line every 5,000 km of the world's x and z, the launch
 *       marked: for laying out a mission's nav points) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/rvdata.h"
#include "../../src/terrain.h"

#define W 640
#define H 400
#define KM 256.0

static uint8_t g_pal[768];
static uint8_t g_fb[W * H];

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void unit(double v[3])
{
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
}

static void cross(const double a[3], const double b[3], double r[3])
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

static void look(TrCam *c, const double pos[3], const double fwd0[3], const double upish[3], double roll)
{
    double f[3], u[3], r[3], a = roll * 3.14159265 / 180;
    int k;
    memcpy(f, fwd0, sizeof f);
    unit(f);
    cross(upish, f, r);
    unit(r);
    cross(f, r, u);
    for (k = 0; k < 3; k++) {
        c->pos[k] = pos[k];
        c->fwd[k] = (float)f[k];
        c->right[k] = (float)(r[k] * cos(a) - u[k] * sin(a));
        c->up[k] = (float)(u[k] * cos(a) + r[k] * sin(a));
    }
    c->focal = 400;                     /* (the flight's FLIGHT_FOCAL) */
    c->cx = 320;
    c->cy = 137;
    c->w = W;
    c->h = H;
}

static void sky(void)
{
    uint32_t s = 12345;
    int k;
    memset(g_fb, 0, sizeof g_fb);
    for (k = 0; k < 900; k++) {
        s = s * 1664525u + 1013904223u;
        g_fb[(s >> 8) % (W * H)] = (uint8_t)(k % 3 ? 26 : 30);
    }
}

static void save_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
}

static void shot(TrPlanet *p, const TrCam *c, const char *dir, const char *name)
{
    static uint8_t rgb[W * H * 3];
    char path[512];
    double best_v = 1e9, best_b = 1e9, t0, t1, t2;
    int y, rep, i;
    for (rep = 0; rep < 4; rep++) {
        sky();
        t0 = now_ms();
        tr_view(p, c);
        t1 = now_ms();
        for (y = 0; y < H; y += 100)
            (void)tr_blit(p, g_fb + y * W, y, W, y, y + 100, 0, W, 0, 0, 0);
        t2 = now_ms();
        if (t1 - t0 < best_v) best_v = t1 - t0;
        if (t2 - t1 < best_b) best_b = t2 - t1;
    }
    printf("%-16s view %6.2f ms  blit %5.2f ms  %7u samples %6u skips\n", name, best_v, best_b, (unsigned)tr_samples(p),
           (unsigned)tr_skips(p));
    for (i = 0; i < W * H; i++) {
        rgb[3 * i] = (uint8_t)(g_pal[3 * g_fb[i]] * 255 / 63);
        rgb[3 * i + 1] = (uint8_t)(g_pal[3 * g_fb[i] + 1] * 255 / 63);
        rgb[3 * i + 2] = (uint8_t)(g_pal[3 * g_fb[i] + 2] * 255 / 63);
    }
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    save_ppm(path, rgb, W, H);
}

static int load(const char *path, void **out, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)*n);
    if (!*out || fread(*out, 1, (size_t)*n, f) != (size_t)*n) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static int map_mode(int argc, char **argv)
{
    TrPlanetDesc d;
    TrPlanet *p;
    TrColumn col;
    static uint8_t rgb[800 * 800 * 3];
    static float hgt[800 * 800];
    double R, cx, cy, cz, x0, z0, span, t0;
    int i, j, N = 800;
    if (argc < 11)
        return 2;
    memset(&d, 0, sizeof d);
    d.seed = (uint32_t)atoi(argv[4]);
    d.radius = (int32_t)(atof(argv[5]) * KM);
    R = atof(argv[5]);
    cx = atof(argv[6]);
    cy = atof(argv[7]);
    cz = atof(argv[8]);
    x0 = atof(argv[9]);
    z0 = atof(argv[10]);
    span = argc > 11 ? atof(argv[11]) : 60000;
    t0 = now_ms();
    p = tr_make(&d, 0, g_pal);
    if (!p)
        return 1;
    printf("made in %.0f ms, top %.0f km\n", now_ms() - t0, (tr_top(p) - d.radius) / KM);
    for (j = 0; j < N; j++)
        for (i = 0; i < N; i++) {
            double x = x0 - span / 2 + span * i / N - cx, z = z0 + span / 2 - span * j / N - cz, y2 = R * R - x * x - z * z;
            hgt[j * N + i] = -1e9f;
            if (y2 <= 0)
                continue;
            tr_column(p, (int32_t)(x * KM), (int32_t)(sqrt(y2) * KM), (int32_t)(z * KM), &col);
            hgt[j * N + i] = (float)((col.ground - d.radius) / KM);
        }
    for (j = 0; j < N; j++)
        for (i = 0; i < N; i++) {
            float h = hgt[j * N + i], hx = hgt[j * N + (i + 1 < N ? i + 1 : i)], hz = hgt[(j + 1 < N ? j + 1 : j) * N + i];
            double wx = x0 - span / 2 + span * i / N, wz = z0 + span / 2 - span * j / N, t, shade, cell = span / N;
            uint8_t *o = rgb + 3 * (j * N + i);
            if (h < -1e8f) {
                o[0] = o[1] = o[2] = 0;
                continue;
            }
            t = (h + 2500) / 7000;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            shade = 0.75 + ((hx - h) * -0.6 + (hz - h) * 0.8) / cell * 0.25;
            shade = shade < 0.3 ? 0.3 : shade > 1.3 ? 1.3 : shade;
            o[0] = (uint8_t)fmin(255, (60 + 190 * t) * shade);
            o[1] = (uint8_t)fmin(255, (110 + 90 * t - 120 * t * t) * shade + 40 * t);
            o[2] = (uint8_t)fmin(255, (70 + 40 * (1 - t)) * shade);
            if (fabs(fmod(wx + 1e6, 5000)) < cell * 0.6 || fabs(fmod(wz + 1e6, 5000)) < cell * 0.6)
                o[0] = o[1] = o[2] = (uint8_t)(fabs(fmod(wx + 1e6, 20000)) < cell * 0.6 || fabs(fmod(wz + 1e6, 20000)) < cell * 0.6 ? 255 : 150);
            if (fabs(wx) < cell * 4 && fabs(wz) < cell * 4)
                o[0] = 255, o[1] = o[2] = 0;
        }
    save_ppm(argv[3], rgb, N, N);
    printf("x %.0f .. %.0f, z %.0f (top) .. %.0f, a line each 5,000 km (white: 20,000), the launch red\n",
           x0 - span / 2, x0 + span / 2, z0 + span / 2, z0 - span / 2);
    tr_free(p);
    return 0;
}

int main(int argc, char **argv)
{
    const RvGameHeader *hd;
    const RvMission *m = 0;
    void *dat;
    long n;
    FILE *f;
    TrMissionKm mk;
    TrPlanetDesc d;
    TrStamp st[TR_STAMPS_MAX];
    TrPlanet *p;
    TrCam c;
    double t0, navp[RV_NAVS][3], R, ctr[3];
    int navid[RV_NAVS], nn = 0, k, i, pieces = 0;
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--map")) {
        f = fopen(argv[2], "rb");
        if (!f || fread(g_pal, 1, 768, f) != 768)
            return 1;
        fclose(f);
        return map_mode(argc, argv);
    }
    if (argc < 5) {
        fprintf(stderr, "usage: planet_view RVGAME.DAT PALETTE.BIN OUTDIR MISSION_ID\n");
        return 2;
    }
    f = fopen(argv[2], "rb");
    if (!f || fread(g_pal, 1, 768, f) != 768) {
        fprintf(stderr, "no palette\n");
        return 1;
    }
    fclose(f);
    if (!load(argv[1], &dat, &n)) {
        fprintf(stderr, "no %s\n", argv[1]);
        return 1;
    }
    hd = (const RvGameHeader *)dat;
    if (hd->mission_size != sizeof(RvMission)) {
        fprintf(stderr, "mission records of %u bytes, this build's %u\n", hd->mission_size, (unsigned)sizeof(RvMission));
        return 1;
    }
    for (k = 0; k < hd->nmissions; k++) {
        const RvMission *q = (const RvMission *)((const char *)dat + hd->missions_at + (long)k * hd->mission_size);
        if (!strncmp(q->id, argv[4], sizeof q->id))
            m = q;
    }
    if (!m || !m->pradius) {
        fprintf(stderr, "no planet mission %s\n", argv[4]);
        return 1;
    }
    tr_from_records((const TrPlanetRec *)&m->px, (const TrNavRec *)m->nav, m->nnav, &mk);
    tr_from_km(&mk, &d, st);
    printf("%s: seed %u, radius %d km, centre %d %d %d, %d stamps\n", argv[4], (unsigned)mk.seed, (int)mk.radius,
           (int)mk.centre[0], (int)mk.centre[1], (int)mk.centre[2], d.nstamps);
    t0 = now_ms();
    p = tr_begin(&d, st, g_pal);
    while (p && tr_build(p) > 0)
        pieces++;
    if (!p || !tr_ready(p)) {
        fprintf(stderr, "not made\n");
        return 1;
    }
    printf("made in %.0f ms (%d pieces); top %.0f km over the datum\n", now_ms() - t0, pieces, (tr_top(p) - d.radius) / KM);
    R = d.radius;
    for (k = 0; k < 3; k++)
        ctr[k] = mk.centre[k];
    /* the nav points, put over the ground as the card's PL_PLACE puts them */
    for (k = 0; k < m->nnav && nn < RV_NAVS; k++) {
        const RvNav *v = &m->nav[k];
        int32_t q[3];
        TrColumn col;
        for (i = 0; i < 3; i++)
            q[i] = (int32_t)(((&v->x)[i] - mk.centre[i]) * 256);
        tr_column(p, q[0], q[1], q[2], &col);
        if (v->flags & 4)
            tr_place(p, q, col.r - tr_radius(p));
        tr_column(p, q[0], q[1], q[2], &col);
        printf("nav %d: world %d %d %d km -> %.0f %.0f %.0f; ground %.0f km over the datum, the point %.0f km over it%s\n",
               v->id, (int)v->x, (int)v->y, (int)v->z, q[0] / KM + ctr[0], q[1] / KM + ctr[1], q[2] / KM + ctr[2],
               (col.ground - R) / KM, (col.r - col.ground) / KM, col.slab_lo ? " (a slab over it)" : "");
        if (v->id == 0)
            continue;
        navid[nn] = v->id;
        for (i = 0; i < 3; i++)
            navp[nn][i] = q[i];
        nn++;
    }
    /* along the way: the line from each nav point to the next sampled, the least it clears the ground by */
    for (k = 0; k + 1 < nn; k++) {
        double least = 1e30, at = 0;
        for (i = 0; i <= 200; i++) {
            double t = i / 200.0, q[3];
            TrColumn col;
            int a;
            for (a = 0; a < 3; a++)
                q[a] = navp[k][a] + (navp[k + 1][a] - navp[k][a]) * t;
            tr_column(p, (int32_t)q[0], (int32_t)q[1], (int32_t)q[2], &col);
            if ((col.r - col.ground) / KM < least) {
                least = (col.r - col.ground) / KM;
                at = t;
            }
        }
        printf("nav %d -> nav %d: %.0f km; the line clears the ground by %.0f km at least (%.0f%% along)\n", navid[k],
               navid[k + 1], sqrt((navp[k + 1][0] - navp[k][0]) * (navp[k + 1][0] - navp[k][0])
                                  + (navp[k + 1][1] - navp[k][1]) * (navp[k + 1][1] - navp[k][1])
                                  + (navp[k + 1][2] - navp[k][2]) * (navp[k + 1][2] - navp[k][2])) / KM, least, at * 100);
    }
    {
        double eye[3], fwd[3], up[3], side[3];
        char name[64];
        /* from the launch (the world's origin) at the first nav point */
        for (k = 0; k < 3; k++) {
            eye[k] = -ctr[k] * KM;
            fwd[k] = navp[0][k] - eye[k];
            up[k] = eye[k];
        }
        unit(up);
        look(&c, eye, fwd, up, 0);
        shot(p, &c, argv[3], "00-launch");
        for (k = 0; k < nn; k++) {
            int to = k + 1 < nn ? k + 1 : k - 1;
            if (to < 0)
                break;
            for (i = 0; i < 3; i++) {
                up[i] = navp[k][i];
                fwd[i] = navp[to][i] - navp[k][i];
            }
            unit(up);
            snprintf(name, sizeof name, "%02d-nav%d", 2 * k + 1, navid[k]);
            look(&c, navp[k], fwd, up, 0);
            shot(p, &c, argv[3], name);
            /* from beside it, 2,000 km up, looking back at it and down */
            cross(up, fwd, side);
            unit(side);
            {
                double f2[3], len = sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
                for (i = 0; i < 3; i++) {
                    eye[i] = navp[k][i] - fwd[i] / len * 3500 * KM + up[i] * 5000 * KM + side[i] * 1000 * KM;
                    f2[i] = navp[k][i] - eye[i];
                }
                snprintf(name, sizeof name, "%02d-over%d", 2 * k + 2, navid[k]);
                look(&c, eye, f2, up, 0);
                shot(p, &c, argv[3], name);
            }
        }
        /* high over the first, looking along the way */
        if (nn > 1) {
            for (i = 0; i < 3; i++) {
                up[i] = navp[0][i];
                fwd[i] = navp[1][i] - navp[0][i];
            }
            unit(up);
            unit(fwd);
            for (i = 0; i < 3; i++) {
                eye[i] = navp[0][i] + up[i] * 9000 * KM - fwd[i] * 6000 * KM;
                fwd[i] = fwd[i] - up[i] * 0.45;
            }
            look(&c, eye, fwd, up, 0);
            shot(p, &c, argv[3], "90-high");
            for (i = 0; i < 3; i++) {
                eye[i] = navp[0][i] + up[i] * 30000 * KM;
                fwd[i] = -up[i];
                side[i] = navp[1][i] - navp[0][i];
            }
            look(&c, eye, fwd, side, 0);
            shot(p, &c, argv[3], "91-down");
        }
    }
    tr_free(p);
    free(dat);
    return 0;
}
