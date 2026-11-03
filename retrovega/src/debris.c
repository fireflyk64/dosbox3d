/* debris.c: a wreck's debris drawn (debris.h).  Plain C with C89's declarations (the host builds warn on others) and
 * floats: it runs only on the host, in the sprite accelerator and the native builds, never in RV.EXE. */
#include <math.h>
#include <string.h>
#include "debris.h"

#define DB_EMBERS_MAX 32                /* embers a cloud at most: half its shards */
#define DB_NEAR16 32.0f                 /* the nearest a shard's corner may be to the eye: 2 km, in 16ths */
#define DB_FADE_STEPS 45                /* the last steps of its life: the pieces go one by one */
#define DB_SCREEN_LIMIT 16000.0f        /* a corner projected past this: that piece is not drawn (no overflow) */
#define DB_TWO_PI 6.2831853f

typedef struct { float x, y, z; } DbV3;

typedef struct {                        /* where the pixels go */
    uint8_t *fb;
    int fb_y0, stride, y0, y1, xa, xb;
    const int16_t *ext;
    int ext_y0, ext_rows;
    uint32_t px;
} DbTarget;

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

static DbV3 db_rand_dir(uint32_t *s)         /* a direction, every one as likely */
{
    float z = 2.0f * db_unit(s) - 1.0f, a = DB_TWO_PI * db_unit(s), r = sqrtf(1.0f - z * z);
    return db_v3(r * cosf(a), r * sinf(a), z);
}

static DbV3 db_turn(DbV3 v, DbV3 k, float a)     /* v turned by a about the unit axis k (Rodrigues) */
{
    float c = cosf(a), s = sinf(a), d = (k.x * v.x + k.y * v.y + k.z * v.z) * (1.0f - c);
    return db_v3(v.x * c + (k.y * v.z - k.z * v.y) * s + k.x * d,
              v.y * c + (k.z * v.x - k.x * v.z) * s + k.y * d,
              v.z * c + (k.x * v.y - k.y * v.x) * s + k.z * d);
}

static DbV3 db_to_cam(const RvDebris *d, DbV3 w)   /* a world-space vector in camera space */
{
    const int16_t *m = d->m;
    return db_v3((m[0] * w.x + m[1] * w.y + m[2] * w.z) / 256.0f, (m[3] * w.x + m[4] * w.y + m[5] * w.z) / 256.0f,
              (m[6] * w.x + m[7] * w.y + m[8] * w.z) / 256.0f);
}

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

/* a triangle: the pixels whose centres are inside it (its corners within DB_SCREEN_LIMIT) */
static void db_tri(DbTarget *t, const float *sx, const float *sy, uint8_t c)
{
    int i, y, ya, yb;
    float top = sy[0], bot = sy[0];
    for (i = 1; i < 3; i++) {
        if (sy[i] < top)
            top = sy[i];
        if (sy[i] > bot)
            bot = sy[i];
    }
    ya = (int)ceilf(top - 0.5f);
    yb = (int)floorf(bot - 0.5f);
    if (ya < t->y0)
        ya = t->y0;
    if (yb > t->y1 - 1)
        yb = t->y1 - 1;
    for (y = ya; y <= yb; y++) {
        float yc = (float)y + 0.5f, lo = DB_SCREEN_LIMIT * 2, hi = -DB_SCREEN_LIMIT * 2;
        int a, b, xl, xr;
        for (i = 0; i < 3; i++) {       /* where each edge crosses the row's middle */
            int j = i == 2 ? 0 : i + 1;
            if ((yc >= sy[i] && yc < sy[j]) || (yc >= sy[j] && yc < sy[i])) {
                float x = sx[i] + (yc - sy[i]) * (sx[j] - sx[i]) / (sy[j] - sy[i]);
                if (x < lo)
                    lo = x;
                if (x > hi)
                    hi = x;
            }
        }
        if (lo > hi || !db_row_span(t, y, &a, &b))
            continue;
        xl = (int)ceilf(lo - 0.5f);
        xr = (int)floorf(hi - 0.5f);
        if (xl < a)
            xl = a;
        if (xr > b - 1)
            xr = b - 1;
        if (xl <= xr) {
            memset(t->fb + (long)(y - t->fb_y0) * t->stride + xl, c, (size_t)(xr - xl + 1));
            t->px += (uint32_t)(xr - xl + 1);
        }
    }
}

uint32_t rv_debris_draw(const RvDebris *d, uint8_t *fb, int fb_y0, int stride, int y0, int y1, int xa, int xb,
                        const int16_t *ext, int ext_y0, int ext_rows)
{
    float sx[DEBRIS_MAX][3], sy[DEBRIS_MAX][3], depth[DEBRIS_MAX];
    uint8_t col[DEBRIS_MAX];
    int order[DEBRIS_MAX], n = 0, i, j, k, count = d->count < DEBRIS_MAX ? d->count : DEBRIS_MAX;
    float age = d->age, life = d->life, focal = d->focal, ll;
    DbV3 light = db_v3(-0.45f, 0.65f, -0.62f); /* from above, to the left, from behind the eye */
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
    ll = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    light = db_v3(light.x / ll, light.y / ll, light.z / ll);
    for (i = 0; i < count; i++) {
        /* the shard's own: its way out and how far along it, its shape, its tumble, how long it glows */
        uint32_t s = db_mix((uint32_t)d->seed * 2654435761U + (uint32_t)i * 40503U + 1U);
        DbV3 dir = db_rand_dir(&s), axis = db_rand_dir(&s), p, c[3], e1, e2, nn;
        float reach = 0.25f + 0.75f * sqrtf(db_unit(&s)), len = (float)d->piece * (0.5f + db_unit(&s));
        float wid = len * (0.3f + 0.4f * db_unit(&s)), spin = 0.03f + 0.17f * db_unit(&s), phase = DB_TWO_PI * db_unit(&s);
        float hot_for = (i & 3) ? 18.0f + 30.0f * db_unit(&s) : 50.0f + 60.0f * db_unit(&s);   /* one in four an ember */
        float a = spin * age + phase, glow, shade, nl;
        DbV3 w[3];
        int ok = 1;
        if (age > life - DB_FADE_STEPS && ((d->age + i * 11) & 7) < (age - (life - DB_FADE_STEPS)) * 8.0f / DB_FADE_STEPS)
            continue;
        p = db_to_cam(d, db_v3(dir.x * d->spread * reach, dir.y * d->spread * reach, dir.z * d->spread * reach));
        p = db_v3(p.x + d->x, p.y + d->y, p.z + d->z);
        w[0] = db_v3(len, 0.0f, 0.0f);
        w[1] = db_v3(-0.5f * len, wid, 0.0f);
        w[2] = db_v3(-0.35f * len, -0.6f * wid, 0.5f * wid);
        for (j = 0; j < 3 && ok; j++) {
            DbV3 q = db_to_cam(d, db_turn(w[j], axis, a));
            c[j] = db_v3(p.x + q.x, p.y + q.y, p.z + q.z);
            if (c[j].z < DB_NEAR16) {
                ok = 0;
                break;
            }
            sx[n][j] = (float)d->cx + c[j].x * focal / c[j].z;
            sy[n][j] = (float)d->cy - c[j].y * focal / c[j].z;
            ok = fabsf(sx[n][j]) < DB_SCREEN_LIMIT && fabsf(sy[n][j]) < DB_SCREEN_LIMIT;
        }
        if (!ok)
            continue;
        /* lit by its face against the light, either side (a plate of hull); a glint as it turns full on */
        e1 = db_v3(c[1].x - c[0].x, c[1].y - c[0].y, c[1].z - c[0].z);
        e2 = db_v3(c[2].x - c[0].x, c[2].y - c[0].y, c[2].z - c[0].z);
        nn = db_v3(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
        nl = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
        shade = nl > 0.0f ? fabsf(nn.x * light.x + nn.y * light.y + nn.z * light.z) / nl : 0.5f;
        glow = 1.0f - age / hot_for;
        if (glow > 0.25f) {
            k = (int)(glow * (DEBRIS_SHADES - 0.01f));
            col[n] = d->hot[k < 0 ? 0 : k > DEBRIS_SHADES - 1 ? DEBRIS_SHADES - 1 : k];
        } else {
            k = (int)(shade * (DEBRIS_SHADES - 0.01f));
            col[n] = d->metal[k < 0 ? 0 : k > DEBRIS_SHADES - 1 ? DEBRIS_SHADES - 1 : k];
        }
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
    for (i = 0; i < n; i++) {
        const float *x = sx[order[i]], *y = sy[order[i]];
        float wx = fmaxf(fmaxf(x[0], x[1]), x[2]) - fminf(fminf(x[0], x[1]), x[2]);
        float wy = fmaxf(fmaxf(y[0], y[1]), y[2]) - fminf(fminf(y[0], y[1]), y[2]);
        if (wx < 1.5f && wy < 1.5f)     /* far off: a speck */
            db_dot(&t, (x[0] + x[1] + x[2]) / 3.0f, (y[0] + y[1] + y[2]) / 3.0f, col[order[i]]);
        else
            db_tri(&t, x, y, col[order[i]]);
    }
    /* the embers: points flying out ahead of the shards, white to red, for their first second or so */
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
        k = (int)(left * (DEBRIS_SHADES - 0.01f));
        db_dot(&t, (float)d->cx + p.x * focal / p.z, (float)d->cy - p.y * focal / p.z,
            d->hot[k < 0 ? 0 : k > DEBRIS_SHADES - 1 ? DEBRIS_SHADES - 1 : k]);
    }
    return t.px;
}
