/* pvshot.c: the planet as the card draws it (terrain.c, 640 x 400) beside RV.EXE's own picture of it without the card
 * (pview.c, 128 x 80 from the bake), from a mission's eyes: the launch, a dive, each nav point, banked on the way to
 * the next, from above beside it, low over open ground, 30,000 km up.  The host's (the pictures; the counts a DOS frame's
 * cost follows).
 *
 *   gcc -std=gnu99 -O2 -DPV_DEBUG -I src -o build/host/pvshot tools/pvshot.c src/pview.c -lm
 *   build/host/pvshot build/dos/RVGAME.DAT build/planets/flight.pal OUTDIR MISSION_ID build/planets/ID.PLN
 *     ONLY=nav1   the eyes whose names have this
 *     OPEN=x0,y0,x1,y1   the screen's part the ground is wanted in (the cockpit's opening)
 *     FOGDBG=1 SLABDBG=1   numbers
 *     PVEYES=file   each eye's PvCam written there, and each picture's sum (RV.EXE's bench renders the same eyes)
 * OUTDIR/NN-eye-card.ppm and -pv.ppm; tools/pvsheet.py puts them side by side. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "terrain.c"
#include <time.h>
#include "rvdata.h"
#include "bake.h"
#include "pview.h"

#define W 640
#define H 400
#define KM 256.0

static uint8_t g_pal[768];
static uint8_t g_fb[W * H];
static const char *g_out;

static void unit(double v[3])
{
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= l; v[1] /= l; v[2] /= l;
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
    c->focal = 400;
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
static void save(const char *name, const char *kind)
{
    static uint8_t rgb[W * H * 3];
    char path[512];
    FILE *f;
    int i;
    for (i = 0; i < W * H; i++) {
        rgb[3 * i] = (uint8_t)(g_pal[3 * g_fb[i]] * 255 / 63);
        rgb[3 * i + 1] = (uint8_t)(g_pal[3 * g_fb[i] + 1] * 255 / 63);
        rgb[3 * i + 2] = (uint8_t)(g_pal[3 * g_fb[i] + 2] * 255 / 63);
    }
    snprintf(path, sizeof path, "%s/%s-%s.ppm", g_out, name, kind);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(rgb, 1, sizeof rgb, f);
    fclose(f);
}


static const char *g_only;
static uint8_t *g_pln;
static long g_pln_n;
int xms_get_at(unsigned handle, unsigned long off, void *dst, unsigned long n)
{
    (void)handle;
    if (off + n > (unsigned long)g_pln_n) { fprintf(stderr, "read past the file: %lu + %lu\n", off, n); exit(1); }
    memcpy(dst, g_pln + off, n);
    return 1;
}
void pv_blit_host(uint8_t *fb, int stride, int y0, int y1);
static int g_shot;
static TrPlanet *g_p;
static void shot(TrPlanet *p, const TrCam *c, const char *what)
{
    char name[64];
    PvCam pc;
    int k;
    clock_t t0;
    snprintf(name, sizeof name, "%02d-%s", g_shot++, what);
    if (g_only && !strstr(name, g_only))
        return;
    sky();
    t0 = clock();
    tr_view(p, c);
    tr_blit(p, g_fb, 0, W, 0, H, 0, W, 0, 0, 0);
    save(name, "card");
    printf("%-18s card %6u samples %5.1f ms;", name, tr_samples(p), (clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    for (k = 0; k < 3; k++) {
        pc.eye[k] = (int32_t)c->pos[k];
        pc.m[k] = (int16_t)floor(c->right[k] * 16384 + 0.5);
        pc.m[3 + k] = (int16_t)floor(c->up[k] * 16384 + 0.5);
        pc.m[6 + k] = (int16_t)floor(c->fwd[k] * 16384 + 0.5);
    }
    pc.focal = (int16_t)c->focal;
    pc.cx = (int16_t)c->cx;
    pc.cy = (int16_t)c->cy;
    pc.x0 = 0; pc.y0 = 0; pc.x1 = W; pc.y1 = H;
    if (getenv("OPEN")) { int a, b, c2, d; if (sscanf(getenv("OPEN"), "%d,%d,%d,%d", &a, &b, &c2, &d) == 4) { pc.x0 = a; pc.y0 = b; pc.x1 = c2; pc.y1 = d; } }
    sky();
    t0 = clock();
    g_p = p;
    pv_frame(&pc);
    if (getenv("PVEYES")) {             /* (the eye for RV.EXE's bench, and the picture's sum: DOS's must be the same) */
        static FILE *ef;
        const uint16_t *pic = pv_picture();
        uint32_t sum = 0;
        int i;
        if (!ef && !(ef = fopen(getenv("PVEYES"), "wb")))
            exit(1);
        fwrite(&pc, sizeof pc, 1, ef);
        fflush(ef);
        for (i = 0; i < PV_LW * PV_LH; i++)
            sum = sum * 31 + pic[i];
        printf("   pvsum %s %08X samples %lu seen %lu skips %lu\n", name, sum, (unsigned long)pv_frame_now()->samples,
               (unsigned long)pv_frame_now()->seen, (unsigned long)pv_frame_now()->skips);
    }
    pv_blit_host(g_fb, W, 0, H);
    save(name, "pv");
    printf("  pview %6u samples %5.1f ms\n", pv_frame_now()->samples, (clock() - t0) * 1000.0 / CLOCKS_PER_SEC);
    { void pv_dbg_counts(void); pv_dbg_counts(); }
    if (getenv("FOGDBG")) {
        const uint16_t *pic = pv_picture();
        int hist[32] = {0}, i, y;
        for (i = 0; i < 128 * 80; i++) if (pic[i] != 0xFFFF) hist[pic[i] >> 8]++;
        printf("   pview haze:"); for (i = 0; i < 32; i++) if (hist[i]) printf(" %d:%d", i, hist[i]); printf("\n");
        for (y = 0; y < 80; y += 8) printf("   row %d: %04X %04X %04X\n", y, pic[y*128+10], pic[y*128+64], pic[y*128+110]);
        {
            double D = sqrt((double)c->pos[0]*c->pos[0] + (double)c->pos[1]*c->pos[1] + (double)c->pos[2]*c->pos[2]);
            {
                TrHit hh; double d0 = c->pos[0]/D, d1 = c->pos[1]/D, d2 = c->pos[2]/D;
                p->line_patches = ~0; tr_sample(p, d0, d1, d2, 0, &hh);
                printf("   card under the eye: mat %d light %.1f h %.0f km (patch %d)\n", hh.mat, hh.light, hh.h/256, hh.patch);
            }
            { int pv_fog_dbg(int32_t, int32_t); double dd[4] = {390, 1000, 5000, 20000}; int hh2[3] = {-400, 0, 1000}; int a, b;
              for (b = 0; b < 3; b++) for (a = 0; a < 4; a++) printf("   h %d km dist %.0f km: card %.1f pview %d\n", hh2[b], dd[a],
                 tr_fog(p, dd[a]*256, D - p->radius, hh2[b]*256.0), pv_fog_dbg((int32_t)(dd[a]*16), hh2[b]*4)); }
            printf("   alt %.0f km; card fog at dist 1000/5000/20000 km, ground 0: %.1f %.1f %.1f\n", (D - p->radius)/256,
                tr_fog(p, 1000*256., D - p->radius, 0), tr_fog(p, 5000*256., D - p->radius, 0), tr_fog(p, 20000*256., D - p->radius, 0));
        }
    }
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
    if (!*out || fread(*out, 1, (size_t)*n, f) != (size_t)*n)
        return 0;
    fclose(f);
    return 1;
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
    double navp[RV_NAVS][3], R, ctr[3];
    int nn = 0, k, i;
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc < 6)
        return 2;
    g_out = argv[3];
    g_only = getenv("ONLY");
    f = fopen(argv[2], "rb");
    if (!f || fread(g_pal, 1, 768, f) != 768 || !load(argv[1], &dat, &n))
        return 1;
    fclose(f);
    hd = (const RvGameHeader *)dat;
    for (k = 0; k < hd->nmissions; k++) {
        const RvMission *q = (const RvMission *)((const char *)dat + hd->missions_at + (long)k * hd->mission_size);
        if (!strncmp(q->id, argv[4], sizeof q->id))
            m = q;
    }
    if (!m || !m->pradius)
        return 1;
    tr_from_records((const TrPlanetRec *)&m->px, (const TrNavRec *)m->nav, m->nnav, &mk);
    tr_from_km(&mk, &d, st);
    p = tr_begin(&d, st, g_pal);
    while (p && tr_build(p) > 0)
        ;
    if (!p || !tr_ready(p))
        return 1;
    {
        void *pl;
        BkHead bh;
        if (!load(argv[5], &pl, &g_pln_n))
            return 1;
        g_pln = (uint8_t *)pl;
        memcpy(&bh, g_pln, sizeof bh);
        if (pv_begin(0, 0, bh.view_at) < 0) {
            fprintf(stderr, "no view in %s\n", argv[5]);
            return 1;
        }
    }
    R = d.radius;
    for (k = 0; k < 3; k++)
        ctr[k] = mk.centre[k];
    for (k = 0; k < m->nnav && nn < RV_NAVS; k++) {
        const RvNav *v = &m->nav[k];
        int32_t q[3];
        TrColumn col;
        for (i = 0; i < 3; i++)
            q[i] = (int32_t)(((&v->x)[i] - mk.centre[i]) * 256);
        tr_column(p, q[0], q[1], q[2], &col);
        if (v->flags & 4)
            tr_place(p, q, col.r - tr_radius(p));
        if (v->id == 0)
            continue;
        for (i = 0; i < 3; i++)
            navp[nn][i] = q[i];
        nn++;
    }
    {
        double eye[3], fwd[3], up[3], side[3], len = 1;
        /* from the launch at the first nav point */
        for (k = 0; k < 3; k++) {
            eye[k] = -ctr[k] * KM;
            fwd[k] = navp[0][k] - eye[k];
            up[k] = eye[k];
        }
        unit(up);
        look(&c, eye, fwd, up, 0);
        shot(p, &c, "launch");
        /* diving at the first nav point from half way */
        for (k = 0; k < 3; k++)
            eye[k] = (-ctr[k] * KM + navp[0][k]) / 2;
        look(&c, eye, fwd, up, 25);
        shot(p, &c, "dive");
        for (k = 0; k < nn && k < 3; k++) {
            int to = k + 1 < nn ? k + 1 : k - 1;
            char name[32];
            for (i = 0; i < 3; i++) {
                up[i] = navp[k][i];
                fwd[i] = navp[to][i] - navp[k][i];
            }
            unit(up);
            len = sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
            snprintf(name, sizeof name, "nav%d", k + 1);
            look(&c, navp[k], fwd, up, 0);
            shot(p, &c, name);
            /* on the way to the next, banked */
            for (i = 0; i < 3; i++)
                eye[i] = navp[k][i] + fwd[i] * 0.45;
            {
                int32_t q[3];
                TrColumn col;
                for (i = 0; i < 3; i++)
                    q[i] = (int32_t)eye[i];
                tr_column(p, q[0], q[1], q[2], &col);
                if (col.r - col.ground < 400 * KM)
                    tr_place(p, q, (int32_t)(400 * KM));
                for (i = 0; i < 3; i++)
                    eye[i] = q[i];
            }
            snprintf(name, sizeof name, "way%d-banked", k + 1);
            look(&c, eye, fwd, up, 35);
            shot(p, &c, name);
            /* from beside it, 5,000 km up, looking back at it and down */
            cross(up, fwd, side);
            unit(side);
            {
                double f2[3];
                for (i = 0; i < 3; i++) {
                    eye[i] = navp[k][i] - fwd[i] / len * 3500 * KM + up[i] * 5000 * KM + side[i] * 1000 * KM;
                    f2[i] = navp[k][i] - eye[i];
                }
                snprintf(name, sizeof name, "over%d", k + 1);
                look(&c, eye, f2, up, 0);
                shot(p, &c, name);
            }
        }
        /* low over open ground, away from the mission's shapes */
        {
            int32_t q[3];
            for (i = 0; i < 3; i++) {
                up[i] = navp[0][i];
                fwd[i] = navp[1][i] - navp[0][i];
            }
            unit(up);
            cross(up, fwd, side);
            unit(side);
            for (i = 0; i < 3; i++)
                q[i] = (int32_t)(navp[0][i] + side[i] * 9000 * KM);
            tr_place(p, q, (int32_t)(300 * KM));
            for (i = 0; i < 3; i++)
                eye[i] = q[i];
            look(&c, eye, fwd, up, 0);
            shot(p, &c, "open-low");
            tr_place(p, q, (int32_t)(1500 * KM));
            for (i = 0; i < 3; i++) {
                eye[i] = q[i];
                fwd[i] = fwd[i] / len - up[i] * 0.25;
            }
            look(&c, eye, fwd, up, -30);
            shot(p, &c, "open-1500");
            for (i = 0; i < 3; i++) {
                eye[i] = navp[0][i] + up[i] * 30000 * KM;
                fwd[i] = -up[i];
            }
            look(&c, eye, fwd, side, 0);
            shot(p, &c, "down-30000");
        }
    }
    (void)R;
    return 0;
}
