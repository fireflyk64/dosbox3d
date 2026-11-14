/* terrain_view.c: a planet made and looked at from a few places, without the game (tests/host/terrain_view.sh).
 * Each view is drawn as the flight would: the sky first (black and a few stars), then the ground in bands of 100
 * rows into a 640 x 400 picture, written as a PPM in the game's palette; the times printed.  Also: the same seed made
 * twice gives the same ground (a hash), and the ground under a few points (tr_column).
 *
 *   terrain_view PALETTE.BIN OUTDIR [seed] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

/* an eye at `pos` looking along `fwd` (any length), its up as near `upish` as can be, then rolled `roll` degrees
 * (left-handed as the game: x right, y up, z ahead; right = up x fwd) */
static void look(TrCam *c, const double pos[3], const double fwd0[3], const double upish[3], double roll)
{
    double f[3], u[3], r[3], ru[3], rr[3], a = roll * 3.14159265 / 180;
    int k;
    memcpy(f, fwd0, sizeof f);
    unit(f);
    cross(upish, f, r);
    unit(r);
    cross(f, r, u);
    for (k = 0; k < 3; k++) {
        rr[k] = r[k] * cos(a) - u[k] * sin(a);
        ru[k] = u[k] * cos(a) + r[k] * sin(a);
    }
    for (k = 0; k < 3; k++) {
        c->pos[k] = pos[k];
        c->fwd[k] = (float)f[k];
        c->right[k] = (float)rr[k];
        c->up[k] = (float)ru[k];
    }
    c->focal = 320;
    c->cx = 320;
    c->cy = 137;                         /* the Schroedinger's opening's middle */
    c->w = W;
    c->h = H;
}

/* a point `km` over the ground (or the arch's slab: under it) at the direction of `at` */
static void over(const TrPlanet *p, const double at[3], double km, double out[3])
{
    double u[3];
    TrColumn col;
    int k;
    memcpy(u, at, sizeof u);
    unit(u);
    tr_column(p, (int32_t)(u[0] * 8e6), (int32_t)(u[1] * 8e6), (int32_t)(u[2] * 8e6), &col);
    for (k = 0; k < 3; k++)
        out[k] = u[k] * (col.ground + km * KM);
}

static void sky(void)
{
    uint32_t s = 12345;
    int k;
    memset(g_fb, 0, sizeof g_fb);
    for (k = 0; k < 900; k++) {
        s = s * 1664525u + 1013904223u;
        g_fb[(s >> 8) % (W * H)] = (uint8_t)(k % 3 ? 26 : 30);   /* greys */
    }
}

static void save(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    int i;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        uint8_t px[3];
        px[0] = (uint8_t)(g_pal[3 * g_fb[i]] * 255 / 63);
        px[1] = (uint8_t)(g_pal[3 * g_fb[i] + 1] * 255 / 63);
        px[2] = (uint8_t)(g_pal[3 * g_fb[i] + 2] * 255 / 63);
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void shot(TrPlanet *p, const TrCam *c, const char *dir, const char *name)
{
    double t0, t1, t2;
    int y;
    uint32_t px = 0;
    sky();
    t0 = now_ms();
    tr_view(p, c);
    t1 = now_ms();
    for (y = 0; y < H; y += 100)
        px += tr_blit(p, g_fb + y * W, y, W, y, y + 100, 0, W, 0, 0, 0);
    t2 = now_ms();
    printf("%-14s view %6.2f ms  blit %5.2f ms  %6u px  %7u samples\n", name, t1 - t0, t2 - t1, (unsigned)px, (unsigned)tr_samples(p));
    save(dir, name);
}

int main(int argc, char **argv)
{
    TrPlanetDesc d;
    TrStamp st[2];
    TrPlanet *p, *p2;
    TrCam c;
    FILE *f;
    double t0, R, pos[3], fwd[3], upv[3], base[3], along[3];
    TrColumn col;
    int k;
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: terrain_view PALETTE.BIN OUTDIR [seed]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f || fread(g_pal, 1, 768, f) != 768) {
        fprintf(stderr, "no palette\n");
        return 1;
    }
    fclose(f);
    memset(&d, 0, sizeof d);
    d.seed = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 0) : 1;
    d.radius = (int32_t)(32000 * KM);
    d.kind = TR_KIND_DESERT;
    R = d.radius;
    /* the base under an arch: a pad, and an arch over it, on the face of +y towards +z */
    base[0] = 0.10;
    base[1] = 0.80;
    base[2] = 0.59;
    unit(base);
    along[0] = 1;
    along[1] = 0;
    along[2] = 0;
    memset(st, 0, sizeof st);
    st[0].kind = TR_STAMP_PAD;
    st[1].kind = TR_STAMP_ARCH;
    for (k = 0; k < 3; k++) {
        st[0].at[k] = st[1].at[k] = (int32_t)(base[k] * R);
        st[0].to[k] = st[1].to[k] = (int32_t)(base[k] * R + along[k] * 1000 * KM);
    }
    st[0].size[0] = (int32_t)(500 * KM);   /* a pad 500 km across the middle */
    st[0].size[1] = (int32_t)(400 * KM);
    st[1].size[0] = (int32_t)(400 * KM);   /* an arch 800 km between its feet, 250 km wide, 300 km of air under it */
    st[1].size[1] = (int32_t)(250 * KM);
    st[1].size[2] = (int32_t)(300 * KM);
    st[1].size[3] = (int32_t)(120 * KM);
    d.nstamps = 2;
    t0 = now_ms();
    p = tr_make(&d, st, g_pal);
    printf("made in %.0f ms; top %.0f km over the datum\n", now_ms() - t0, (tr_top(p) - R) / KM);
    if (!p)
        return 1;
    p2 = tr_make(&d, st, g_pal);
    for (k = 0; k < 2000; k++) {
        TrColumn a, b;
        int32_t x = (int32_t)(((int)((unsigned)k * 7919u % 2001u) - 1000) * 8000), y = (int32_t)(((int)((unsigned)k * 104729u % 2001u) - 1000) * 8000),
                z = (int32_t)(((int)((unsigned)k * 1299709u % 2001u) - 1000) * 8000);
        tr_column(p, x, y, z, &a);
        tr_column(p2, x, y, z, &b);
        if (memcmp(&a, &b, sizeof a)) {
            printf("TWICE DIFFERENT at %d\n", k);
            return 1;
        }
    }
    tr_free(p2);
    tr_column(p, (int32_t)(base[0] * R), (int32_t)(base[1] * R), (int32_t)(base[2] * R), &col);
    printf("under the arch's crown: ground %.1f km, slab %.1f .. %.1f km over the datum\n", (col.ground - R) / KM,
           col.slab_lo ? (col.slab_lo - R) / KM : 0, col.slab_hi ? (col.slab_hi - R) / KM : 0);

    /* 1: from far out, the whole planet */
    for (k = 0; k < 3; k++) {
        pos[k] = (k == 2 ? -3.2 : k == 1 ? 0.6 : 0.3) * R;
        fwd[k] = -pos[k];
    }
    upv[0] = 0; upv[1] = 1; upv[2] = 0;
    look(&c, pos, fwd, upv, 0);
    shot(p, &c, argv[2], "01-orbit");
    /* 2: coming in, 8,000 km up over the base, looking at the horizon */
    for (k = 0; k < 3; k++) {
        pos[k] = base[k] * (R + 8000 * KM) - along[k] * 9000 * KM;
        fwd[k] = along[k] - base[k] * 0.45;
        upv[k] = base[k];
    }
    look(&c, pos, fwd, upv, 0);
    shot(p, &c, argv[2], "02-approach");
    {
        double side[3], at[3], tgt[3];
        cross(base, along, side);
        unit(side);
        /* 3: low, 600 km over the ground, level, towards the arch from 4,000 km off */
        for (k = 0; k < 3; k++)
            at[k] = base[k] * R - side[k] * 4000 * KM;
        over(p, at, 600, pos);
        for (k = 0; k < 3; k++) {
            fwd[k] = side[k] - base[k] * 0.08;
            upv[k] = base[k];
        }
        look(&c, pos, fwd, upv, 0);
        shot(p, &c, argv[2], "03-low");
        look(&c, pos, fwd, upv, 35);
        shot(p, &c, argv[2], "04-banked");
        /* 5: 120 km over the pad, 1,000 km from the arch, looking at its crown */
        for (k = 0; k < 3; k++)
            at[k] = base[k] * R - side[k] * 1000 * KM;
        over(p, at, 120, pos);
        over(p, base, 250, tgt);
        for (k = 0; k < 3; k++) {
            fwd[k] = tgt[k] - pos[k];
            upv[k] = base[k];
        }
        look(&c, pos, fwd, upv, 0);
        shot(p, &c, argv[2], "05-arch");
        /* 5b: under it, looking out along the pad */
        over(p, base, 120, pos);
        for (k = 0; k < 3; k++) {
            fwd[k] = side[k];
            upv[k] = base[k];
        }
        look(&c, pos, fwd, upv, 0);
        shot(p, &c, argv[2], "05b-under");
    }
    /* 6: straight down from 3,000 km */
    for (k = 0; k < 3; k++) {
        pos[k] = base[k] * (R + 3000 * KM);
        fwd[k] = -base[k];
        upv[k] = along[k];
    }
    look(&c, pos, fwd, upv, 0);
    shot(p, &c, argv[2], "06-down");
    /* 7: low, looking up at the sky */
    {
        double at[3];
        for (k = 0; k < 3; k++)
            at[k] = base[k] * R + along[k] * 6000 * KM;
        over(p, at, 400, pos);
    }
    for (k = 0; k < 3; k++) {
        fwd[k] = along[k] + base[k] * 0.9;
        upv[k] = base[k];
    }
    look(&c, pos, fwd, upv, 0);
    shot(p, &c, argv[2], "07-up");
    {
        double e[3], q[3];
        double at[3], near_[3];
        for (k = 0; k < 3; k++) {
            e[k] = c.pos[k];
            at[k] = base[k] * R - along[k] * 9000 * KM;   /* 15,000 km off: under the horizon */
        }
        over(p, at, 200, q);
        printf("seen (from 07's eye): a ship 200 km up 15,000 km off %d (0 wanted)", tr_seen(p, e, q));
        for (k = 0; k < 3; k++)
            at[k] = e[k] + along[k] * 1500 * KM;
        over(p, at, 300, near_);
        printf(", one 1,500 km off %d (1 wanted), one overhead %d (1 wanted)\n", tr_seen(p, e, near_),
               (q[0] = e[0] * 1.5, q[1] = e[1] * 1.5, q[2] = e[2] * 1.5, tr_seen(p, e, q)));
    }
    tr_free(p);
    return 0;
}
