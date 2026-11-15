/* planetbake.c: the planet missions' ground baked (src/bake.h): each of RVGAME.DAT's planet missions made with
 * terrain.c, as the card makes it, and written as ID.PLN: the grid BK_LEVEL halvings down, its halvings, the mission's
 * patches.  Then the file is read back through bake.c and asked what the game will ask: inside a patch its answers
 * must be terrain.c's own to the position; outside, how far they are from terrain.c's finest ground is printed.
 *
 *   planetbake RVGAME.DAT OUTDIR [--stamp N] [--force] [--only ID] [--quiet]
 *
 * A file whose key (its mission's records) and stamp (build.sh's: the sources' sum) are these is left as it is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "terrain.c"
#include "rvdata.h"
#include "bake.h"

static uint8_t *g_file;
static uint32_t g_size;

/* (bake.c reads the file through this: here it is in memory, handle and base 0) */
int xms_get_at(unsigned handle, unsigned long at, void *dst, unsigned long n)
{
    (void)handle;
    if (at + n > g_size) {
        fprintf(stderr, "planetbake: a read past the file's end (%lu + %lu of %u)\n", at, n, g_size);
        exit(1);
    }
    memcpy(dst, g_file + at, n);
    return 1;
}

static void put(FILE *f, const void *p, size_t n)
{
    if (fwrite(p, 1, n, f) != n) {
        perror("planetbake: write");
        exit(1);
    }
}

static void put_grid(FILE *f, const TrPlanet *p, int level)
{
    int face, side = tr_side(level);
    size_t k, n = (size_t)side * side;
    uint8_t *row = (uint8_t *)malloc(n * BK_CELL);
    for (face = 0; face < 6; face++) {
        const TrCell *g = p->g[level][face];
        for (k = 0; k < n; k++) {
            row[k * BK_CELL] = (uint8_t)(g[k].h & 0xFF);
            row[k * BK_CELL + 1] = (uint8_t)((uint16_t)g[k].h >> 8);
            row[k * BK_CELL + 2] = g[k].c;
        }
        put(f, row, n * BK_CELL);
    }
    free(row);
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

/* the file asked as the game asks it, against terrain.c: 0 when every answer in a patch is terrain.c's */
static int check(const TrPlanet *p, const char *id, int quiet)
{
    enum { NS = 40000 };
    static int32_t diff[NS];
    int k, bad = 0, nd = 0, in_patch = 0, slabs_out = 0, f, i, j;
    long asked = 0;
    if (bk_begin(0, 0) < 0) {
        fprintf(stderr, "planetbake: %s: the file is no bake\n", id);
        return 1;
    }
    /* each patch's points and places between them, at the ground, 300 km over it and half a radius out */
    for (k = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        if (!pt->h)
            continue;
        for (j = 0; j <= pt->nb; j += 1 + (int)(rnd() % 5))
            for (i = 0; i <= pt->na; i += 1 + (int)(rnd() % 5)) {
                int64_t v[3];
                int alt;
                tr_patch_point(p, pt, i, j, v);
                for (alt = 0; alt < 3; alt++) {
                    static const double scale[3] = { 1.0, 1.005, 1.5 };
                    int32_t q[3];
                    TrColumn a;
                    BkColumn b;
                    q[0] = (int32_t)(v[0] * scale[alt]) + (int32_t)(rnd() % 4096) - 2048;
                    q[1] = (int32_t)(v[1] * scale[alt]) + (int32_t)(rnd() % 4096) - 2048;
                    q[2] = (int32_t)(v[2] * scale[alt]) + (int32_t)(rnd() % 4096) - 2048;
                    tr_column(p, q[0], q[1], q[2], &a);
                    bk_column(q[0], q[1], q[2], &b);
                    asked++;
                    /* (terrain.c's answer is a patch's when a patch holds the point: then the two must be one) */
                    {
                        int held = 0, n;
                        int64_t gx, gy;
                        for (n = 0; n < p->npatch && !held; n++)
                            held = p->patch[n].h && tr_patch_find(p, &p->patch[n], q[0], q[1], q[2], &gx, &gy);
                        if (!held)
                            continue;
                    }
                    if (a.ground != b.ground || a.slab_lo != b.slab_lo || a.slab_hi != b.slab_hi || a.r != b.r) {
                        if (bad++ < 5)
                            fprintf(stderr, "planetbake: %s: patch %d point (%d, %d) x %.3f: terrain.c ground %d slab %d..%d r %d, "
                                    "the bake %d, %d..%d, %d\n", id, k, i, j, scale[alt], a.ground, a.slab_lo, a.slab_hi, a.r,
                                    b.ground, b.slab_lo, b.slab_hi, b.r);
                    }
                }
            }
    }
    /* places all over the planet: outside the patches, how far the bake's ground is from terrain.c's */
    for (k = 0; k < NS; k++) {
        double d[3], len;
        int32_t q[3];
        int held = 0, n;
        int64_t gx, gy;
        TrColumn a;
        BkColumn b;
        do {
            d[0] = (double)(rnd() % 20001) - 10000;
            d[1] = (double)(rnd() % 20001) - 10000;
            d[2] = (double)(rnd() % 20001) - 10000;
            len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        } while (len < 100 || len > 10000);
        for (n = 0; n < 3; n++)
            q[n] = (int32_t)(d[n] / len * p->radius);
        for (n = 0; n < p->npatch && !held; n++)
            held = p->patch[n].h && tr_patch_find(p, &p->patch[n], q[0], q[1], q[2], &gx, &gy);
        tr_column(p, q[0], q[1], q[2], &a);
        bk_column(q[0], q[1], q[2], &b);
        if (held) {
            in_patch++;
            if (a.ground != b.ground || a.slab_lo != b.slab_lo || a.slab_hi != b.slab_hi)
                bad++;
            continue;
        }
        if (b.slab_lo || b.slab_hi)
            bad++;
        diff[nd++] = abs(a.ground - b.ground);
    }
    /* a slab of the planet's own grid outside every patch would be lost: there must be none */
    for (f = 0; f < 6; f++)
        for (j = 0; j < TR_N; j++)
            for (i = 0; i < TR_N; i++) {
                int lo, hi, n, held = 0;
                int64_t v[3], gx, gy;
                if (!p->tiles[f][(j / TR_TILE) * TR_TILES + i / TR_TILE]) {
                    i |= TR_TILE - 1;
                    continue;
                }
                if (!tr_slab(p, f, i, j, &lo, &hi))
                    continue;
                tr_cell_vec(f, i, j, TR_N, v);
                for (n = 0; n < 3; n++)
                    v[n] *= 4096;
                for (n = 0; n < p->npatch && !held; n++)
                    held = p->patch[n].h && tr_patch_find(p, &p->patch[n], v[0], v[1], v[2], &gx, &gy);
                slabs_out += !held;
            }
    qsort(diff, (size_t)nd, sizeof diff[0], cmp_i32);
    if (!quiet || bad || slabs_out)
        printf("  %s: %ld answers in the patches asked, %d not terrain.c's; outside them (%d places) the bake's ground from "
               "terrain.c's finest: half within %.1f km, 90%% %.1f, 99%% %.1f, the most %.1f; %d slab cells outside every patch\n",
               id, asked, bad, nd, nd ? diff[nd / 2] / 256.0 : 0.0, nd ? diff[nd * 9 / 10] / 256.0 : 0.0,
               nd ? diff[(int)(nd * 0.99)] / 256.0 : 0.0, nd ? diff[nd - 1] / 256.0 : 0.0, slabs_out);
    (void)in_patch;
    bk_end();
    return bad || slabs_out;
}

static int bake(const RvMission *m, const char *dir, uint32_t stamp, int force, int quiet)
{
    char id[RV_ID + 1], path[1024];
    TrMissionKm mk;
    TrPlanetDesc d;
    TrStamp st[TR_STAMPS_MAX];
    TrPlanet *p;
    static uint8_t pal[768];
    BkHead h;
    BkPatch bp[BK_PATCHES];
    FILE *f;
    int k, l, np = 0, r;
    uint32_t at, key;
    memcpy(id, m->id, RV_ID);
    id[RV_ID] = 0;
    if (strlen(id) > 8) {
        fprintf(stderr, "planetbake: %s: a planet mission's id names its file (8 letters at most)\n", id);
        return 1;
    }
    snprintf(path, sizeof path, "%s/%s.PLN", dir, id);
    key = bk_key(&m->px, m->nav, m->nnav);
    f = fopen(path, "rb");
    if (f) {
        size_t got = fread(&h, 1, sizeof h, f);
        fclose(f);
        if (!force && got == sizeof h && !memcmp(h.magic, "RVPL", 4) && h.version == BK_VERSION && h.key == key
            && h.stamp == stamp)
            return 0;
    }
    tr_from_records((const TrPlanetRec *)&m->px, (const TrNavRec *)m->nav, m->nnav, &mk);
    tr_from_km(&mk, &d, st);
    for (k = 0; k < 768; k++)
        pal[k] = (uint8_t)(k / 3 / 4);
    p = tr_begin(&d, st, pal);
    while (p && (r = tr_build(p)) > 0)
        ;
    if (!p || r < 0) {
        fprintf(stderr, "planetbake: %s: no memory for the planet\n", id);
        return 1;
    }
    memset(&h, 0, sizeof h);
    memset(bp, 0, sizeof bp);
    memcpy(h.magic, "RVPL", 4);
    h.version = BK_VERSION;
    h.n = (uint16_t)(TR_N >> BK_LEVEL);
    h.key = key;
    h.stamp = stamp;
    h.radius = p->radius;
    h.top = (int16_t)p->top;
    h.bottom = (int16_t)p->bottom;
    h.kind = p->desc.kind;
    h.nmips = BK_MIPS;
    at = sizeof h;
    h.grid_at = at;
    at += 6u * tr_side(BK_LEVEL) * tr_side(BK_LEVEL) * BK_CELL;
    for (l = 0; l < BK_MIPS; l++) {
        h.mip_at[l] = at;
        at += 6u * tr_side(BK_LEVEL + 1 + l) * tr_side(BK_LEVEL + 1 + l) * BK_CELL;
    }
    for (k = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        BkPatch *b = &bp[np];
        size_t n = (size_t)(pt->na + 1) * (pt->nb + 1), i;
        double cs;
        if (!pt->h)
            continue;
        if (pt->na >= 32767 || pt->nb >= 32767) {
            fprintf(stderr, "planetbake: %s: patch %d is %d x %d cells (32766 at most)\n", id, k, pt->na, pt->nb);
            return 1;
        }
        memcpy(b->c, pt->c, sizeof b->c);
        memcpy(b->a, pt->a, sizeof b->a);
        memcpy(b->b, pt->b, sizeof b->b);
        b->half_a = pt->half_a;
        b->half_b = pt->half_b;
        b->cell = pt->cell;
        b->na = (uint16_t)pt->na;
        b->nb = (uint16_t)pt->nb;
        b->top = pt->top;
        b->over = pt->over;
        /* its far corner's direction from its middle, less the quick test's rounding (bake.c bk_column) */
        cs = p->radius / sqrt((double)p->radius * p->radius + (double)pt->half_a * pt->half_a + (double)pt->half_b * pt->half_b);
        b->cosmin = (int16_t)(floor(cs * 16384) - 12);
        for (i = 0; i < n && !b->slabs; i++)
            b->slabs = pt->lo[i] < pt->hi[i];
        np++;
    }
    h.npatch = (uint8_t)np;
    h.patch_at = at;
    at += (uint32_t)np * sizeof(BkPatch);
    for (k = 0; k < np; k++) {
        size_t n = (size_t)(bp[k].na + 1) * (bp[k].nb + 1);
        bp[k].at = at;
        at += (uint32_t)(n * BK_CELL);
        at += at & 1;
        if (bp[k].slabs) {
            bp[k].slab_at = at;
            at += (uint32_t)(n * BK_SLAB);
        }
    }
    at += 8;                            /* (room past the last point: a read of two cells there stays in the file) */
    h.size = at;
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    put(f, &h, sizeof h);
    for (l = 0; l <= BK_MIPS; l++)
        put_grid(f, p, BK_LEVEL + l);
    put(f, bp, (size_t)np * sizeof(BkPatch));
    for (k = 0, np = 0; k < p->npatch; k++) {
        const TrPatch *pt = &p->patch[k];
        size_t n = (size_t)(pt->na + 1) * (pt->nb + 1), i;
        uint8_t *buf;
        if (!pt->h)
            continue;
        buf = (uint8_t *)malloc(n * BK_SLAB + 1);
        for (i = 0; i < n; i++) {
            buf[i * BK_CELL] = (uint8_t)(pt->h[i] & 0xFF);
            buf[i * BK_CELL + 1] = (uint8_t)((uint16_t)pt->h[i] >> 8);
            buf[i * BK_CELL + 2] = pt->col[i];
        }
        buf[n * BK_CELL] = 0;
        put(f, buf, n * BK_CELL + ((n * BK_CELL) & 1));
        if (bp[np].slabs) {
            for (i = 0; i < n; i++) {
                buf[i * 4] = (uint8_t)(pt->lo[i] & 0xFF);
                buf[i * 4 + 1] = (uint8_t)((uint16_t)pt->lo[i] >> 8);
                buf[i * 4 + 2] = (uint8_t)(pt->hi[i] & 0xFF);
                buf[i * 4 + 3] = (uint8_t)((uint16_t)pt->hi[i] >> 8);
            }
            put(f, buf, n * BK_SLAB);
        }
        free(buf);
        np++;
    }
    {
        static const uint8_t zero[8];
        put(f, zero, 8);
    }
    if ((uint32_t)ftell(f) != h.size) {
        fprintf(stderr, "planetbake: %s: wrote %ld bytes, the head says %u\n", id, ftell(f), h.size);
        return 1;
    }
    fclose(f);
    /* read back and asked */
    f = fopen(path, "rb");
    g_size = h.size;
    g_file = (uint8_t *)malloc(g_size);
    if (!f || fread(g_file, 1, g_size, f) != g_size) {
        perror(path);
        return 1;
    }
    fclose(f);
    printf("%s.PLN: %u bytes (%d patches: %u bytes of them)\n", id, h.size, h.npatch, h.size - h.patch_at);
    r = check(p, id, quiet);
    free(g_file);
    g_file = 0;
    tr_free(p);
    if (r) {
        remove(path);
        fprintf(stderr, "planetbake: %s: the bake's answers are not right (the file removed)\n", id);
    }
    return r;
}

int main(int argc, char **argv)
{
    const RvGameHeader *hd;
    uint8_t *dat;
    long n;
    FILE *f;
    int k, force = 0, quiet = 0, bad = 0, made = 0;
    uint32_t stamp = 0;
    const char *only = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: planetbake RVGAME.DAT OUTDIR [--stamp N] [--force] [--only ID] [--quiet]\n");
        return 2;
    }
    for (k = 3; k < argc; k++) {
        if (!strcmp(argv[k], "--force"))
            force = 1;
        else if (!strcmp(argv[k], "--quiet"))
            quiet = 1;
        else if (!strcmp(argv[k], "--stamp") && k + 1 < argc)
            stamp = (uint32_t)strtoul(argv[++k], 0, 0);
        else if (!strcmp(argv[k], "--only") && k + 1 < argc)
            only = argv[++k];
        else {
            fprintf(stderr, "planetbake: what is %s?\n", argv[k]);
            return 2;
        }
    }
    f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    dat = (uint8_t *)malloc((size_t)n);
    if (fread(dat, 1, (size_t)n, f) != (size_t)n) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);
    hd = (const RvGameHeader *)dat;
    if (hd->mission_size != sizeof(RvMission)) {
        fprintf(stderr, "planetbake: %s's missions are %u bytes, rvdata.h's %u\n", argv[1], hd->mission_size,
                (unsigned)sizeof(RvMission));
        return 1;
    }
    for (k = 0; k < hd->nmissions; k++) {
        const RvMission *m = (const RvMission *)(dat + hd->missions_at + (long)k * hd->mission_size);
        if (!m->pradius || (only && strncmp(only, m->id, RV_ID)))
            continue;
        bad += bake(m, argv[2], stamp, force, quiet);
        made++;
    }
    if (!made && only) {
        fprintf(stderr, "planetbake: no planet mission %s\n", only);
        return 1;
    }
    return bad ? 1 : 0;
}
