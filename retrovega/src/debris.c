/* debris.c: a wreck's debris drawn (debris.h).  Plain C with C89's declarations (the host builds warn on others) and
 * floats: it runs only on the host, in the sprite accelerator and the native builds, never in RV.EXE.
 *
 * The pieces are chunks of armour cut from the plate's damage stamps (debris_art.h, tools/debris.py; the user,
 * 2026-11-03: "Like those armor chunks in the interstitial ... those could be almost reused directly. I guess they
 * would need to tumble"), drawn as the asteroid fields' rocks are ("axis aligned asteroids are the best analogy ...
 * they will be tumbling so rolling won't be noticeable"): a picture per tumble frame, upright, only scaled.  The
 * tumble is made here, once (rv_debris_prepare): each chunk turns as a flat piece of plating would, about an axis in
 * its plane, narrowing to a sliver edge on, its back the mirror of its front and darker, its light the stamp's own
 * times how squarely it faces the key light (up and to the left, in front).  Kept as DEBRIS_METAL levels of light,
 * so the same pictures glow hot and cool to grey metal.  Each piece of a cloud takes a chunk, a tumble speed and a
 * phase from the cloud's seed; nothing is kept from frame to frame. */
#include <math.h>
#include <string.h>
#include "debris.h"
#include "debris_art.h"

#define DB_EMBERS_MAX 32                /* embers a cloud at most: half its pieces */
#define DB_NEAR16 32.0f                 /* the nearest a piece may be to the eye: 2 km, in 16ths */
#define DB_FADE_STEPS 45                /* the last steps of its life: the pieces go one by one */
#define DB_SCREEN_LIMIT 16000.0f        /* a piece projected past this: not drawn (no overflow) */
#define DB_TWO_PI 6.2831853f

#define DB_FRAMES 16                    /* a tumble's pictures, a whole turn */
#define DB_BIG DB_ART_SIDE              /* the pictures' sizes: 48, 24 and 12 pixels a side */
#define DB_MID (DB_BIG / 2)
#define DB_SMALL (DB_BIG / 4)
#define DB_SPAN 1.3f                    /* a picture's side over its piece's length (a chunk fills about 3/4 of it) */
#define DB_LARGEST 320                  /* a piece bigger than this on the screen is passing the eye: not drawn */
#define DB_EDGE_ON 0.12f                /* the narrowest a chunk gets edge on, of its width */

typedef struct { float x, y, z; } DbV3;

typedef struct {                        /* where the pixels go */
    uint8_t *fb;
    int fb_y0, stride, y0, y1, xa, xb;
    const int16_t *ext;
    int ext_y0, ext_rows;
    uint32_t px;
} DbTarget;

/* the pictures: for each chunk and frame, the 48, 24 and 12 pixel ones, a pixel's light 1 .. DEBRIS_METAL (0 clear),
 * and a far speck's */
static uint8_t db_pics[DB_ART_KINDS][DB_FRAMES][DB_BIG * DB_BIG + DB_MID * DB_MID + DB_SMALL * DB_SMALL];
static uint8_t db_speck[DB_ART_KINDS][DB_FRAMES];
static int db_made;

static uint32_t db_mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352DU;
    x ^= x >> 15;
    x *= 0x846CA68BU;
    x ^= x >> 16;
    return x;
}

static float db_unit(uint32_t *s)          /* 0 .. 1 */
{
    *s = db_mix(*s + 0x9E3779B9U);
    return (float)(*s >> 8) * (1.0f / 16777216.0f);
}

static DbV3 db_v3(float x, float y, float z)
{
    DbV3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static DbV3 db_rand_dir(uint32_t *s)    /* a direction, every one as likely */
{
    float z = 2.0f * db_unit(s) - 1.0f, a = DB_TWO_PI * db_unit(s), r = sqrtf(1.0f - z * z);
    return db_v3(r * cosf(a), r * sinf(a), z);
}

static DbV3 db_to_cam(const RvDebris *d, DbV3 w)   /* a world-space vector in camera space */
{
    const int16_t *m = d->m;
    return db_v3((m[0] * w.x + m[1] * w.y + m[2] * w.z) / 256.0f, (m[3] * w.x + m[4] * w.y + m[5] * w.z) / 256.0f,
                 (m[6] * w.x + m[7] * w.y + m[8] * w.z) / 256.0f);
}

static int db_level(float v)            /* a light 0 .. 1 as 1 .. DEBRIS_METAL */
{
    int l = 1 + (int)(v * DEBRIS_METAL);
    return l < 1 ? 1 : l > DEBRIS_METAL ? DEBRIS_METAL : l;
}

/* --- the tumble's pictures, made once --- */

void rv_debris_prepare(void)
{
    static float val[DB_BIG * DB_BIG];  /* a frame's light, below 0 clear */
    static uint8_t light[DB_BIG * DB_BIG];
    const float lx = -0.55f, ly = -0.65f, lz = 0.55f, ll = sqrtf(lx * lx + ly * ly + lz * lz);   /* (screen y down) */
    int kind, f, i, x, y;
    if (db_made)
        return;
    for (kind = 0; kind < DB_ART_KINDS; kind++) {
        float phi = db_art_axis[kind] * (DB_TWO_PI * 0.5f / 256.0f), c = cosf(phi), s = sinf(phi);
        for (i = 0; i < DB_BIG * DB_BIG; i++)
            light[i] = (uint8_t)((db_art_light[kind][i / 2] >> ((i & 1) * 4)) & 15);
        for (f = 0; f < DB_FRAMES; f++) {
            float th = DB_TWO_PI * (float)f / DB_FRAMES, k = cosf(th), sum = 0.0f;
            float face = fabsf(sinf(th) * c * lx + sinf(th) * s * ly + cosf(th) * lz) / ll;
            float lit = k > 0.0f ? 0.35f + 0.75f * face : 0.25f + 0.5f * face;   /* (its back: darker) */
            uint8_t *big = db_pics[kind][f], *mid = big + DB_BIG * DB_BIG, *small = mid + DB_MID * DB_MID;
            int on = 0, l;
            if (fabsf(k) < DB_EDGE_ON)
                k = k < 0.0f ? -DB_EDGE_ON : DB_EDGE_ON;
            for (y = 0; y < DB_BIG; y++)
                for (x = 0; x < DB_BIG; x++) {
                    /* across the axis squashed by the turn (mirrored past edge on), along it as it is */
                    float dx = x + 0.5f - DB_BIG / 2, dy = y + 0.5f - DB_BIG / 2;
                    float u = (dx * c + dy * s) / k, v = -dx * s + dy * c;
                    float sx = DB_BIG / 2 + u * c - v * s, sy = DB_BIG / 2 + u * s + v * c, lv;
                    int ix = (int)floorf(sx), iy = (int)floorf(sy), src;
                    src = ix >= 0 && ix < DB_BIG && iy >= 0 && iy < DB_BIG ? light[iy * DB_BIG + ix] : 0;
                    lv = src ? (float)src / 15.0f * lit : -1.0f;
                    val[y * DB_BIG + x] = lv < 0.999f ? lv : 0.999f;
                    big[y * DB_BIG + x] = (uint8_t)(src ? db_level(val[y * DB_BIG + x]) : 0);
                    if (src) {
                        sum += val[y * DB_BIG + x];
                        on++;
                    }
                }
            /* the smaller pictures: a pixel is the chunk where it mostly is, its light the mean */
            for (l = 0; l < 2; l++) {
                int box = l ? 4 : 2, side = DB_BIG / box, bx, by;
                uint8_t *out = l ? small : mid;
                for (by = 0; by < side; by++)
                    for (bx = 0; bx < side; bx++) {
                        float t = 0.0f;
                        int hits = 0, px, py;
                        for (py = by * box; py < (by + 1) * box; py++)
                            for (px = bx * box; px < (bx + 1) * box; px++)
                                if (val[py * DB_BIG + px] >= 0.0f) {
                                    t += val[py * DB_BIG + px];
                                    hits++;
                                }
                        out[by * side + bx] = (uint8_t)(hits * 5 >= box * box * 2 ? db_level(t / hits) : 0);
                    }
            }
            db_speck[kind][f] = (uint8_t)db_level(on ? sum / on : 0.3f);
        }
    }
    db_made = 1;
}

/* --- drawing --- */

static int db_row_span(const DbTarget *t, int y, int *a, int *b)   /* the columns row y may take: 0 when none */
{
    *a = t->xa;
    *b = t->xb;
    if (t->ext) {
        int i = y - t->ext_y0;
        if (i < 0 || i >= t->ext_rows)
            return 0;
        if (t->ext[2 * i] > *a)
            *a = t->ext[2 * i];
        if (t->ext[2 * i + 1] < *b)
            *b = t->ext[2 * i + 1];
    }
    return *a < *b;
}

static void db_dot(DbTarget *t, float fx, float fy, uint8_t c)
{
    int x, y, a, b;
    if (fx < -1.0f || fy < -1.0f || fx > DB_SCREEN_LIMIT || fy > DB_SCREEN_LIMIT)
        return;
    x = (int)floorf(fx);
    y = (int)floorf(fy);
    if (y < t->y0 || y >= t->y1 || !db_row_span(t, y, &a, &b) || x < a || x >= b)
        return;
    t->fb[(long)(y - t->fb_y0) * t->stride + x] = c;
    t->px++;
}

/* a picture (side x side, light 1 .. DEBRIS_METAL, 0 clear) drawn size x size with its top left at (x0, y0), each
 * light through ramp */
static void db_blit(DbTarget *t, const uint8_t *pic, int side, int x0, int y0, int size, const uint8_t *ramp)
{
    int y, ya = y0 < t->y0 ? t->y0 : y0, yb = y0 + size > t->y1 ? t->y1 : y0 + size;
    for (y = ya; y < yb; y++) {
        const uint8_t *row = pic + (long)(y - y0) * side / size * side;
        uint8_t *dst = t->fb + (long)(y - t->fb_y0) * t->stride;
        int a, b, x;
        if (!db_row_span(t, y, &a, &b))
            continue;
        if (x0 > a)
            a = x0;
        if (x0 + size < b)
            b = x0 + size;
        for (x = a; x < b; x++) {
            uint8_t s = row[(long)(x - x0) * side / size];
            if (s) {
                dst[x] = ramp[s - 1];
                t->px++;
            }
        }
    }
}

uint32_t rv_debris_draw(const RvDebris *d, uint8_t *fb, int fb_y0, int stride, int y0, int y1, int xa, int xb,
                        const int16_t *ext, int ext_y0, int ext_rows)
{
    float sx[DEBRIS_MAX], sy[DEBRIS_MAX], depth[DEBRIS_MAX], size[DEBRIS_MAX];
    uint8_t kind[DEBRIS_MAX], frame[DEBRIS_MAX], hot[DEBRIS_MAX], glow[DEBRIS_METAL];
    int order[DEBRIS_MAX], n = 0, i, j, k, count = d->count < DEBRIS_MAX ? d->count : DEBRIS_MAX;
    float age = d->age, life = d->life, focal = d->focal;
    DbTarget t;
    if (d->age < DEBRIS_SHOW_AGE || d->age >= d->life || !fb || stride < 1 || d->focal < 1)
        return 0;
    t.fb = fb;
    t.fb_y0 = fb_y0;
    t.stride = stride;
    t.y0 = y0 < fb_y0 ? fb_y0 : y0;
    t.y1 = y1;
    t.xa = xa < 0 ? 0 : xa;
    t.xb = xb > stride ? stride : xb;
    t.ext = ext;
    t.ext_y0 = ext_y0;
    t.ext_rows = ext_rows;
    t.px = 0;
    if (t.y0 >= t.y1 || t.xa >= t.xb)
        return 0;
    rv_debris_prepare();
    for (i = 0; i < count; i++) {
        /* the piece's own: its way out and how far along it, its kind and length, its tumble, how long it glows */
        uint32_t s = db_mix((uint32_t)d->seed * 2654435761U + (uint32_t)i * 40503U + 1U);
        DbV3 dir = db_rand_dir(&s), p;
        float reach = 0.25f + 0.75f * sqrtf(db_unit(&s)), len = (float)d->piece * (0.5f + db_unit(&s));
        float spin = 0.06f + 0.30f * db_unit(&s), phase = DB_FRAMES * db_unit(&s);
        float hot_for = (i & 3) ? 18.0f + 30.0f * db_unit(&s) : 50.0f + 60.0f * db_unit(&s);   /* one in four longer */
        float glow, px;
        int kd = (int)(db_unit(&s) * DB_ART_KINDS);
        if (i % 6 == 0)                 /* (one in six a big piece: a cloud has a few that catch the eye) */
            len *= 1.8f;
        if (age > life - DB_FADE_STEPS && ((d->age + i * 11) & 7) < (age - (life - DB_FADE_STEPS)) * 8.0f / DB_FADE_STEPS)
            continue;
        p = db_to_cam(d, db_v3(dir.x * d->spread * reach, dir.y * d->spread * reach, dir.z * d->spread * reach));
        p = db_v3(p.x + d->x, p.y + d->y, p.z + d->z);
        if (p.z < DB_NEAR16)
            continue;
        sx[n] = (float)d->cx + p.x * focal / p.z;
        sy[n] = (float)d->cy - p.y * focal / p.z;
        px = len * DB_SPAN * focal / p.z;   /* (the picture's side on the screen) */
        if (fabsf(sx[n]) > DB_SCREEN_LIMIT || fabsf(sy[n]) > DB_SCREEN_LIMIT || px > DB_LARGEST || px < 0.5f)
            continue;
        size[n] = px;
        kind[n] = (uint8_t)(kd < DB_ART_KINDS ? kd : DB_ART_KINDS - 1);
        frame[n] = (uint8_t)((int)(phase + spin * age) % DB_FRAMES);
        glow = 1.0f - age / hot_for;
        hot[n] = glow > 0.25f;
        depth[n] = p.z;
        order[n] = n;
        n++;
    }
    for (i = 1; i < n; i++)             /* far to near */
        for (j = i; j > 0 && depth[order[j - 1]] < depth[order[j]]; j--) {
            k = order[j];
            order[j] = order[j - 1];
            order[j - 1] = k;
        }
    for (i = 0; i < DEBRIS_METAL; i++)  /* (glowing: the metal's levels two by two onto the glow's) */
        glow[i] = d->hot[i * DEBRIS_HOT / DEBRIS_METAL];
    for (i = 0; i < n; i++) {
        int o = order[i], side, at, sz;
        const uint8_t *ramp = hot[o] ? glow : d->metal;
        const uint8_t *pics = db_pics[kind[o]][frame[o]];
        if (size[o] < 2.5f) {           /* far off: a speck of its mean light */
            db_dot(&t, sx[o], sy[o], ramp[db_speck[kind[o]][frame[o]] - 1]);
            continue;
        }
        side = size[o] >= DB_BIG * 0.7f ? DB_BIG : size[o] >= DB_MID * 0.7f ? DB_MID : DB_SMALL;
        at = side == DB_BIG ? 0 : side == DB_MID ? DB_BIG * DB_BIG : DB_BIG * DB_BIG + DB_MID * DB_MID;
        sz = (int)(size[o] + 0.5f);
        db_blit(&t, pics + at, side, (int)floorf(sx[o] - sz * 0.5f), (int)floorf(sy[o] - sz * 0.5f), sz, ramp);
    }
    /* the embers: points flying out ahead of the pieces, white to red, for their first second or so */
    for (i = 0; i < count / 2 && i < DB_EMBERS_MAX; i++) {
        uint32_t s = db_mix((uint32_t)d->seed * 2246822519U + (uint32_t)i * 9973U + 7U);
        DbV3 dir = db_rand_dir(&s), p;
        float reach = 1.1f + 0.6f * db_unit(&s), last = 20.0f + 30.0f * db_unit(&s), left = 1.0f - age / last;
        if (left <= 0.0f)
            continue;
        p = db_to_cam(d, db_v3(dir.x * d->spread * reach, dir.y * d->spread * reach, dir.z * d->spread * reach));
        p = db_v3(p.x + d->x, p.y + d->y, p.z + d->z);
        if (p.z < DB_NEAR16)
            continue;
        k = (int)(left * (DEBRIS_HOT - 0.01f));
        db_dot(&t, (float)d->cx + p.x * focal / p.z, (float)d->cy - p.y * focal / p.z,
               d->hot[k < 0 ? 0 : k > DEBRIS_HOT - 1 ? DEBRIS_HOT - 1 : k]);
    }
    return t.px;
}
