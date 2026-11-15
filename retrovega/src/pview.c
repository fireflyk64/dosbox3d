/* pview.c: a planet's ground drawn without the card, 128 x 80 (pview.h).  terrain.c's march (tr_view, tr_quarter,
 * tr_line, tr_fill_ground) in integers, from the bake's pyramid instead of terrain.c's grids.
 *
 * The frame (pv_frame) is worked out here in C; the parts that run a line, a step or a pixel at a time are the C twins
 * below (c_pv_*), which pview.asm's (rv_pv_*) do to the bit on DOS.  Their arrays are where the frame says: on DOS the
 * band buffer (the picture, the steps, the lines, the slab cells: free while the frame is made) and the 32 KB scratch
 * buffer (the window of the level marched, the haze's and the glow's tables), here the host's two of the same sizes. */
#include <string.h>
#ifdef PV_DEBUG
#include <stdio.h>
#include <stdlib.h>
#endif
#include "pview.h"
#include "bake.h"
#ifdef __WATCOMC__
#include <dos.h>
#include <fcntl.h>
#include "rv.h"
#define PV_O_CODE PV_BUF_END            /* the band buffer's tail: the march's code (PVIEW.BIN's), copied in for each
                                           frame */
#else
#define _fmemcpy memcpy
#define _fmemset memset
int xms_get_at(unsigned handle, unsigned long off, void PV_FAR *dst, unsigned long n);
#endif

typedef long long i64;
typedef unsigned long long u64;
#define M64(a, b) ((i64)(a) * (i64)(b))

#define PV_DEN_MIN (1L << 26)           /* a direction with less than this along the plane's middle is past its levels */
#define PV_NEAR_DS 192                  /* the least step, plane units (three quarters of level 0's cell) */
#define PV_SUBS 16                      /* a step's samples at most where it crosses a slab's grid */
#define PV_SUBS_WIDE 6                  /* (the coarser details': pv_detail) */

/* the arrays' places: buf (64,000 bytes) and scr (32,768) */
#define PV_O_PIC 0
#define PV_O_ST (PV_O_PIC + PV_LW * PV_LH * 2)
#define PV_O_LN (PV_O_ST + PV_STEPS * 28)
#define PV_O_SLAB (PV_O_LN + PV_LINES * 64)
#define PV_BUF_END (PV_O_SLAB + PV_SLAB_MEM * 4)
#define PV_WIN_BYTES (PV_W * PV_W * PV_CELL + 2)
#define PV_O_WIN 0
#define PV_O_BWIN (PV_WIN_BYTES)
#define PV_O_FOGLEV (PV_O_BWIN + PV_BW * PV_BW * 2)
#define PV_O_FOGM (PV_O_FOGLEV + PV_FOG_TAUS)
#define PV_O_GLOW (PV_O_FOGM + PV_FOG_HB * 4)
#define PV_O_UNDER (PV_O_GLOW + PV_GLOW_CZ)
#define PV_O_THIN (PV_O_UNDER + 64)
#define PV_SCR_END (PV_O_THIN + 4 * PV_LW)
#define PV_O_SETCODE (PV_O_WIN + 2048)  /* (DOS: the set-up's code, in the window's room past the haze's and glow's rows) */
#ifndef __WATCOMC__
typedef char pv_buf_fits[PV_BUF_END <= 64000L ? 1 : -1];
typedef char pv_scr_fits[PV_SCR_END <= 32768L ? 1 : -1];
#endif

/* --- integers --- */

#ifndef __WATCOMC__
/* the floor of the root: Newton's steps down from a power of two over it (bake.c c_isqrt3's, bake.asm isqrt's) */
static uint32_t pv_isqrt(u64 n)
{
    uint32_t r, q;
    int s = 0;
    if (!n)
        return 0;
    while (s < 32 && (n >> (2 * s)))
        s++;
    r = s >= 32 ? 0xFFFFFFFFUL : (uint32_t)1 << s;
    for (;;) {
        q = (uint32_t)(((u64)r + n / r) >> 1);
        if (q >= r)
            break;
        r = q;
    }
    return r;
}

#endif

/* the floor of the root of a 32-bit number */
static uint32_t pv_isqrt32(uint32_t n)
{
    uint32_t r = 65535, q;
    if (n < 2)
        return n;
    for (;;) {
        q = (r + n / r) / 2;
        if (q >= r)
            return r;
        r = q;
    }
}

#ifndef __WATCOMC__
static int32_t pv_dot30(const int32_t PV_FAR *a, const int32_t PV_FAR *b)   /* (2^30 vectors) */
{
    return (int32_t)((M64(a[0], b[0]) + M64(a[1], b[1]) + M64(a[2], b[2])) >> 30);
}

static int32_t pv_div(i64 a, int32_t b) { return (int32_t)(a / b); }   /* (toward zero, as idiv; the quotient fits) */
static int32_t pv_floordiv(i64 a, int32_t b)   /* (b > 0) */
{
    i64 q = a / b;
    return (int32_t)(q - ((a % b != 0) && (a < 0)));
}
static int32_t pv_ceildiv(i64 a, int32_t b) { return -pv_floordiv(-a, b); }

#endif

/* --- the file --- */

static struct {
    int on;
    unsigned handle;
    uint32_t base;                      /* the section's start in the handle's memory */
    PvHead h;
    int32_t fog_k, fog_hstep;
    int32_t glow_amax;
#ifdef __WATCOMC__
    XmsBlock code, pic, win[PV_LEVELS]; /* the code, the frame's picture, each level's window kept */
    XmsBlock most;                      /* (the last of them: freed when XMS gives back past it) */
    uint16_t code_bytes;
    int32_t wox[PV_LEVELS], woy[PV_LEVELS];
    uint8_t wok[PV_LEVELS];
    uint8_t made;                       /* a picture is in pic */
#endif
} PV_FAR g_pv;

static void pv_get(uint32_t at, void PV_FAR *dst, uint32_t n)
{
    (void)xms_get_at(g_pv.handle, g_pv.base + at, dst, (n + 1) & ~1UL);
}

#ifndef __WATCOMC__
static PvFrame g_frame;
#define g_f g_frame
#else
#define PV_O_FRAME (32768U - sizeof(PvFrame))   /* (DOS: the frame's record in the scratch buffer's last bytes) */
#define g_f (*(PvFrame __far *)(gfx_scratch() + PV_O_FRAME))
typedef char pv_frame_fits[PV_O_FRAME >= PV_SCR_END ? 1 : -1];
#endif

#ifndef __WATCOMC__
/* the host's: the two buffers, the levels' windows kept, the tables */
static uint8_t g_buf[64000], g_scr[32768];
static uint8_t g_win[PV_LEVELS][PV_WIN_BYTES];
static int16_t g_bwin[PV_LEVELS][PV_BW * PV_BW];
static int32_t g_wox[PV_LEVELS], g_woy[PV_LEVELS];
static int g_wok[PV_LEVELS];
static PvDither g_dither[PV_FOGS * 256], g_sky[PV_FOGS];
#endif

#ifdef __WATCOMC__
/* --- DOS: the code from PVIEW.BIN, run in the band buffer's tail; the windows kept in XMS --- */

typedef void (__far __cdecl *PvFnF)(PvFrame __far *);
typedef void (__far __cdecl *PvFnL)(PvFrame __far *, const PvQuarter __far *, const uint8_t __far *);
typedef int (__far __cdecl *PvFnS)(PvFrame __far *, const PvCam __far *, const PvHead __far *);
typedef void (__far __cdecl *PvFnQ)(const PvFrame __far *, int, PvQuarter __far *, uint8_t __far *);
typedef void (__far __cdecl *PvFnB)(const PvBlit __far *);
typedef void (__far *PvFn)(void);

/* the code's head: 'PVB2', its entries (steps, lines, march, far, glow, setup, quarter, blit), then its parts' ranges
 * (the set-up's, the march's, the blit's) and its end (pview.asm) */
#define PV_ENTRIES 8
#define PV_SET0 8
#define PV_SET1 9
#define PV_MARCH0 10
#define PV_MARCH1 11
#define PV_BLIT0 12
#define PV_BLIT1 13
#define PV_BLIT_CODE 1024               /* the blit's code at most (it runs from the scratch buffer's tail) */

static uint16_t __far g_head[PV_ENTRIES + 7];   /* (the code's head, kept) */
static PvFn __far g_fn[PV_ENTRIES];     /* (its entries where they lie now, in the head's order) */

/* a part of the code where it lies now: its entries bound (a part at `at` holds the code from its first byte) */
static void pv_bind_part(const uint8_t __far *at, unsigned first, unsigned end)
{
    unsigned seg = FP_SEG(at), off = FP_OFF(at) - first;
    int k;
    for (k = 0; k < PV_ENTRIES; k++)
        if (g_head[k] >= first && g_head[k] < end)
            g_fn[k] = (PvFn)MK_FP(seg, off + g_head[k]);
}

/* the blit's part copied apart (the twins test: to its work's tail), bound there */
void pv_bind_blit(const uint8_t __far *at)
{
    pv_bind_part(at, g_head[PV_BLIT0], g_head[PV_BLIT1]);
}

/* the whole code where it lies (the twins test: one copy), every entry bound */
int pv_bind(const uint8_t __far *code)
{
    if (*(const uint32_t __far *)code != 0x32425650UL)   /* ('PVB2') */
        return -1;
    _fmemcpy(g_head, code + 4, sizeof g_head);
    pv_bind_part(code, 0, 0xFFFF);
    return 0;
}

/* a part of the code copied from XMS to `at`, and bound there */
static void pv_put(uint8_t __far *at, int part)
{
    unsigned first = g_head[part], end = g_head[part + 1];
    xms_get(g_pv.code, first, at, end - first + 1UL);
    pv_bind_part(at, first, end);
}

void rv_pv_steps(PvFrame __far *f) { ((PvFnF)g_fn[0])(f); }
void rv_pv_lines(PvFrame __far *f, const PvQuarter __far *q, const uint8_t __far *thin) { ((PvFnL)g_fn[1])(f, q, thin); }
void rv_pv_march(PvFrame __far *f) { ((PvFnF)g_fn[2])(f); }
void rv_pv_far(PvFrame __far *f) { ((PvFnF)g_fn[3])(f); }
void rv_pv_glow(PvFrame __far *f) { ((PvFnF)g_fn[4])(f); }
int rv_pv_setup(PvFrame __far *f, const PvCam __far *cam, const PvHead __far *h) { return ((PvFnS)g_fn[5])(f, cam, h); }
void rv_pv_quarter(const PvFrame __far *f, int which, PvQuarter __far *q, uint8_t __far *thin)
{
    ((PvFnQ)g_fn[6])(f, which, q, thin);
}
void rv_pv_blit(const PvBlit __far *b) { ((PvFnB)g_fn[7])(b); }

/* PVIEW.BIN read into dst (n bytes room): its size, or 0 */
unsigned pv_code_read(uint8_t __far *dst, unsigned room)
{
    unsigned got, at = 0;
    int fd;
    if (_dos_open("PVIEW.BIN", O_RDONLY, &fd) != 0)
        return 0;
    while (at < room) {
        unsigned want = room - at < sizeof rv_filebuf ? room - at : sizeof rv_filebuf;
        if (_dos_read(fd, rv_filebuf, want, &got) != 0 || !got)
            break;
        _fmemcpy(dst + at, rv_filebuf, got);
        at += got;
    }
    _dos_close(fd);
    return at;
}

static XmsBlock pv_take(unsigned long n)
{
    XmsBlock b = xms_alloc(n, XMS_MISSION);
    if (b > g_pv.most)
        g_pv.most = b;
    return b;
}

/* the code into XMS (through the scratch buffer), the frame's picture's and the windows' blocks */
static int pv_dos_begin(void)
{
    uint8_t __far *tmp = gfx_scratch();
    int l;
    g_pv.most = 0;
    g_pv.made = 0;
    g_pv.code_bytes = pv_code_read(tmp, 32768U);
    if (g_pv.code_bytes < 32 || pv_bind(tmp) < 0 || g_head[PV_SET1] - g_head[PV_SET0] > PV_WIN_BYTES - PV_O_SETCODE - 2
        || g_head[PV_MARCH1] - g_head[PV_MARCH0] > 64000U - PV_O_CODE - 2
        || g_head[PV_BLIT1] - g_head[PV_BLIT0] > PV_BLIT_CODE - 2 || g_head[PV_BLIT1] > g_pv.code_bytes) {
        RV_LOG("pview: no PVIEW.BIN fit to run (%u bytes)", g_pv.code_bytes);
        return -1;
    }
    g_pv.code = pv_take(g_pv.code_bytes + 2UL);
    g_pv.pic = pv_take((unsigned long)PV_LW * PV_LH * 2);
    for (l = 0; l < g_pv.h.nlev; l++) {
        g_pv.win[l] = pv_take((unsigned long)PV_WIN_BYTES + PV_BW * PV_BW * 2);
        g_pv.wok[l] = 0;
    }
    if (!g_pv.code || !g_pv.pic || !g_pv.win[g_pv.h.nlev - 1]) {
        RV_LOG("pview: no XMS for the picture (%lu KB free)", xms_free_kb());
        return -1;
    }
    xms_put(g_pv.code, 0, tmp, g_pv.code_bytes + 1UL);
    return 0;
}

void pv_xms_freed(unsigned nblocks)
{
    if (g_pv.on && g_pv.most > nblocks)
        g_pv.on = 0;
}

/* level l's window, its corner at cell (ox, oy), from its block (made again when the corner moved) */
static void pv_window(PvFrame PV_FAR *f, int l, int32_t ox, int32_t oy)
{
    PvLevel lvr, *lv = &lvr;
    unsigned wh, j;
    unsigned long wo;
    xms_where(g_pv.win[l], &wh, &wo);
    if (!g_pv.wok[l] || g_pv.wox[l] != ox || g_pv.woy[l] != oy) {
        PvRow *rows = (PvRow *)rv_filebuf;   /* (96 of them: 768 bytes of the 1 KB) */
        int32_t r0, bx, by;
        int n0 = 0, n1 = PV_W;
        pv_get(g_pv.h.lev_at + (uint32_t)l * sizeof(PvLevel), lv, sizeof(PvLevel));
        r0 = oy - lv->y0;
        bx = ox / PV_BLK - lv->bx0;
        by = oy / PV_BLK - lv->by0;
        xms_move_at(wh, wo, g_pv.handle, g_pv.base + g_pv.h.absent_at, PV_WIN_BYTES);
        xms_move_at(wh, wo + PV_WIN_BYTES, g_pv.handle, g_pv.base + g_pv.h.noblk_at, PV_BW * PV_BW * 2);
        if (r0 < 0)
            n0 = (int)-r0;
        if (r0 + PV_W > lv->h)
            n1 = (int)(lv->h - r0);
        if (n0 < n1)
            pv_get(lv->rows_at + (uint32_t)(r0 + n0) * sizeof(PvRow), rows, (uint32_t)(n1 - n0) * sizeof(PvRow));
        for (j = n0; (int)j < n1; j++) {
            const PvRow *row = &rows[j - n0];
            int32_t ax0, a0, a1;
            if (!row->n)
                continue;
            ax0 = lv->x0 + row->x0;
            a0 = ox > ax0 ? ox : ax0;
            a1 = ox + PV_W < ax0 + row->n ? ox + PV_W : ax0 + row->n;
            if (a0 < a1)
                xms_move_at(wh, wo + ((uint32_t)j * PV_W + (a0 - ox)) * PV_CELL, g_pv.handle,
                            g_pv.base + row->at + (uint32_t)(a0 - ax0) * PV_CELL, (uint32_t)(a1 - a0) * PV_CELL);
        }
        for (j = 0; j < PV_BW; j++) {   /* the blocks' highest, a row at a time */
            int32_t i0 = bx < 0 ? -bx : 0, i1 = bx + PV_BW > lv->bw ? lv->bw - bx : PV_BW;
            if (by + (int32_t)j < 0 || by + (int32_t)j >= lv->bh || i0 >= i1)
                continue;
            xms_move_at(wh, wo + PV_WIN_BYTES + ((uint32_t)j * PV_BW + i0) * 2, g_pv.handle,
                        g_pv.base + lv->bmax_at + ((uint32_t)(by + j) * lv->bw + bx + i0) * 2, (uint32_t)(i1 - i0) * 2);
        }
        g_pv.wox[l] = ox;
        g_pv.woy[l] = oy;
        g_pv.wok[l] = 1;
    }
    xms_get(g_pv.win[l], 0, f->scr + f->o_win, PV_WIN_BYTES + PV_BW * PV_BW * 2);
}

/* the scratch buffer at a band: the dither table and the sky's, the band's picture rows, the work, the blit's code */
#define PV_B_DITHER 0
#define PV_B_PIC ((PV_FOGS * 256 + PV_FOGS) * 3)
#define PV_B_WORK (PV_B_PIC + 21 * PV_LW * 2)
#define PV_B_CODE (PV_B_WORK + PV_BLIT_WORK)
typedef char pv_band_fits[PV_B_CODE + PV_BLIT_CODE <= 32768L ? 1 : -1];

void pv_band(int y0, int y1, const short __far *ext, int ext_y0, int ext_rows)
{
    uint8_t __far *scr = gfx_scratch();
    PvBlit b;
    int ly0, ly1;
    if (!g_pv.on || !g_pv.made)
        return;
    if (y1 > PV_LH * PV_BLOCK)
        y1 = PV_LH * PV_BLOCK;
    if (y0 >= y1)
        return;
    ly0 = y0 / PV_BLOCK;
    ly1 = (y1 + PV_BLOCK - 1) / PV_BLOCK;
    pv_get(g_pv.h.dither_at, scr + PV_B_DITHER, PV_FOGS * 256 * 3);
    pv_get(g_pv.h.glow_at + 4, scr + PV_B_DITHER + PV_FOGS * 256 * 3, PV_FOGS * 3);
    xms_get(g_pv.pic, (unsigned long)ly0 * PV_LW * 2, scr + PV_B_PIC, (unsigned long)(ly1 - ly0) * PV_LW * 2);
    pv_put(scr + PV_B_CODE, PV_BLIT0);
    b.band = vid_row(y0);
    b.ext = ext;
    b.pic = (const uint16_t __far *)(scr + PV_B_PIC);
    b.dither = scr + PV_B_DITHER;
    b.work = scr + PV_B_WORK;
    b.y0 = (int16_t)y0;
    b.y1 = (int16_t)y1;
    b.ext_y0 = (int16_t)ext_y0;
    b.ext_rows = (int16_t)ext_rows;
    b.ly0 = (int16_t)ly0;
    b.pad_ = 0;
    rv_pv_blit(&b);
}
#endif

int pv_begin(unsigned handle, uint32_t base, uint32_t at)
{
    PvHead PV_FAR *h = &g_pv.h;
    int32_t fg[3];                      /* (PvFog's h0, hstep, k: the rest is read each frame) */
    uint8_t lv[256];
    uint32_t j;
    int k;
    uint32_t n = 0;
    g_pv.on = 0;
    g_pv.handle = handle;
    g_pv.base = base + at;
    pv_get(0, h, sizeof *h);
    if (h->magic[0] != 'R' || h->magic[1] != 'V' || h->magic[2] != 'P' || h->magic[3] != 'V' || h->nlev < 1
        || h->nlev > PV_LEVELS || h->w != PV_W || h->snap != PV_SNAP || h->radius <= 0 || h->shift0 < 8
        || h->shift0 + h->nlev > 30)
        return -1;
    {
        PvSlabs sl;
        pv_get(h->slabs_at, &sl, sizeof sl);
        if (sl.n > PV_SLABS || (sl.n && (sl.shift < 8 || sl.shift > 20)))
            return -1;
        for (k = 0; k < sl.n; k++)
            n += sl.s[k].w * sl.s[k].h;
        if (n > PV_SLAB_MEM)
            return -1;
        for (k = 0; k < 64; k++)
            if (sl.under[k] >= PV_LIGHTS)
                return -1;
    }
    /* the haze's levels under PV_FOGS and the undersides' light under PV_LIGHTS: pv_fill's sum of the two counts on
     * it (neither half of it carries into the other) */
    for (j = 0; j < PV_FOG_TAUS; j += sizeof lv) {
        pv_get(h->fog_at + (uint32_t)sizeof(int32_t) * (3 + PV_FOG_GS) + j, lv, sizeof lv);
        for (k = 0; k < (int)sizeof lv; k++)
            if (lv[k] >= PV_FOGS)
                return -1;
    }
    pv_get(h->fog_at, fg, 12);
    g_pv.fog_k = fg[2];
    g_pv.fog_hstep = fg[1] > 0 ? fg[1] : 1;
    pv_get(h->glow_at, &g_pv.glow_amax, 4);
#ifndef __WATCOMC__
    pv_get(h->dither_at, g_dither, sizeof g_dither);
    pv_get(h->glow_at + 4, g_sky, sizeof g_sky);
    memset(g_wok, 0, sizeof g_wok);
#else
    if (pv_dos_begin() < 0)
        return -1;
#endif
    g_pv.on = 1;
    return 0;
}

void pv_end(void) { g_pv.on = 0; }
int pv_on(void) { return g_pv.on; }
const PvFrame PV_FAR *pv_frame_now(void) { return &g_f; }

/* --- the C twins (the host's: DOS runs pview.asm's, the host's sums are held to its) --- */
#ifndef __WATCOMC__

/* the frame's steps: every line's samples at the same angles from straight down */
void c_pv_steps(PvFrame PV_FAR *f)
{
    PvStep PV_FAR *st = (PvStep PV_FAR *)(f->buf + f->o_st);
    int32_t c = 1L << 30, s = 0, R = f->R, D = f->D, cosmax;
    int n = 0;
    if (D <= f->rb)
        cosmax = -(1L << 30);
    else {
        /* past acos(rb / D) + acos(rb / rt) no ground can be seen */
        int32_t cd = pv_div(M64(f->rb, 1L << 30), D), ct = pv_div(M64(f->rb, 1L << 30), f->rt);
        int32_t sd = (int32_t)pv_isqrt((u64)((1LL << 60) - M64(cd, cd)));
        int32_t stt = (int32_t)pv_isqrt((u64)((1LL << 60) - M64(ct, ct)));
        cosmax = (int32_t)((M64(cd, ct) - M64(sd, stt)) >> 30);
    }
    for (;;) {
        PvStep PV_FAR *p = &st[n++];
        int32_t a = (int32_t)(M64(R, c) >> 30) - D, b = (int32_t)(M64(R, s) >> 30), dist, ds, g, dt, cd, sd, x;
        int l;
        dist = (int32_t)pv_isqrt((u64)(M64(a, a) + M64(b, b)));
        ds = pv_div(M64(dist, (int32_t)f->step_px * 256), f->lf);   /* (about step_px pixels' worth of the distance) */
        if (ds < PV_NEAR_DS)
            ds = PV_NEAR_DS;
        g = -(int32_t)((M64(a, c) + M64(b, s)) >> 30);   /* (the distance times the sine of the ray's angle to the ground) */
        if (g < dist) {
            int32_t q = g > dist / 4 ? g : dist / 4;
            ds = pv_div(M64(ds, dist), q > 0 ? q : 1);
        }
        p->c = c;
        p->s = s;
        p->a = a >> f->vsh;
        p->b = b >> f->vsh;
        p->uc = (int32_t)(M64(f->ue, c) >> 30);
        p->dist = dist;
        p->ds = (int16_t)(ds > 32767 ? 32767 : ds);
        for (l = 0; l < f->nlev - 1 && (2L << (f->shift0 + l)) < ds; l++)
            ;
        p->lev = (uint8_t)l;
        p->pad_ = 0;
        if (c <= cosmax || n >= f->max_steps)
            break;
        /* on round by dt (its sine and cosine to the third power) */
        dt = pv_div(M64(ds, 1L << 30), R);
        cd = (1L << 30) - (int32_t)(M64(dt, dt) >> 31);
        sd = dt - (int32_t)((M64((int32_t)(M64(dt, dt) >> 30), dt) >> 30) / 6);
        x = (int32_t)((M64(c, cd) - M64(s, sd)) >> 30);
        s = (int32_t)((M64(s, cd) + M64(c, sd)) >> 30);
        c = x;
    }
    f->nsteps = (int16_t)n;
}

/* a picture point's ray (256ths of a pixel), the world's way, 2^24 */
static void pv_ray(const PvFrame PV_FAR *f, int32_t x, int32_t y, int32_t *w)
{
    int32_t rx = pv_div(M64(x - f->lcx, 65536), f->lf), ry = pv_div(M64(y - f->lcy, 65536), f->lf);
    int k;
    for (k = 0; k < 3; k++)
        w[k] = (int32_t)((M64(f->cf[k], 65536) + M64(f->cr[k], rx) - M64(f->cu[k], ry)) >> 6);
}

/* the step a ray along (wu, wt) (in the plane of up and t) first reaches the top sphere at: -1 when never */
static int pv_enter(const PvFrame PV_FAR *f, int32_t wu, int32_t wt)
{
    const PvStep PV_FAR *st = (const PvStep PV_FAR *)(f->buf + f->o_st);
    int32_t n = (int32_t)pv_isqrt((u64)(M64(wu, wu) + M64(wt, wt))), b, sq, sx, qu, qt;
    i64 disc;
    int lo = 0, hi = f->nsteps;
    if (!n)
        return -1;
    b = pv_div(M64(f->D, wu), n);
    disc = M64(b, b) - (M64(f->D, f->D) - M64(f->rt, f->rt));
    if (disc <= 0)
        return -1;
    sq = (int32_t)pv_isqrt((u64)disc);
    sx = -b - sq;
    if (sx < 0)
        return -1;
    qu = f->D + pv_div(M64(sx, wu), n);
    qt = pv_div(M64(sx, wt), n);
    while (lo < hi) {                   /* (the first step at or past it) */
        int mid = (lo + hi) / 2;
        if (M64(st[mid].s, qu) - M64(st[mid].c, qt) >= 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* a quarter's lines set up (as many as PV_LINES lets) */
void c_pv_lines(PvFrame PV_FAR *f, const PvQuarter PV_FAR *q, const uint8_t PV_FAR *thin)
{
    PvLine PV_FAR *lines = (PvLine PV_FAR *)(f->buf + f->o_ln);
    int i;
    for (i = 0; i < q->lines && f->nlines < f->max_lines; i++) {
        PvLine PV_FAR *ln = &lines[f->nlines];
        if (f->wide && (i & 1))
            continue;                   /* (wide: every other line, each filling its neighbour's pixels: pv_fill) */
        int32_t X = (int32_t)(q->first + i) * 256, e8 = (int32_t)q->edge * 256 - q->va, c0, col, dcol, k0, k1;
        int32_t w[3], T[3], t[3], wup, tl, kh, kl, ta, tb, qx, qy;
        const int32_t PV_FAR *cv = q->vert ? f->cu : f->cr;
        int k, m, kmin;
        for (m = 0; m < 12 && !((i >> m) & 1); m++)
            ;
        for (kmin = 0; kmin < q->len && thin[kmin] > m; kmin++)
            ;
        if (kmin >= q->len)
            continue;
        c0 = q->vc + pv_div(M64(X - q->vc, (int32_t)q->inner * 256 - q->va), e8);
        dcol = pv_div(M64(X - q->vc, (int32_t)q->s * 65536), e8);
        col = c0 * 256 + 32768;
        /* the pixels wanted: past kmin, within the view's part of the picture */
        {
            int32_t a0 = q->vert ? f->by0 : f->bx0, a1 = q->vert ? f->by1 : f->bx1;
            int32_t c0v = (int32_t)(q->vert ? f->bx0 : f->by0) * 65536, c1v = (int32_t)(q->vert ? f->bx1 : f->by1) * 65536 - 1;
            int32_t lo, hi;
            k0 = kmin;
            k1 = q->len - 1;
            if (q->s > 0)
                lo = a0 - q->inner, hi = a1 - 1 - q->inner;
            else
                lo = q->inner - (a1 - 1), hi = q->inner - a0;
            if (lo > k0) k0 = lo;
            if (hi < k1) k1 = hi;
            if (dcol > 0) {
                lo = pv_ceildiv((i64)c0v - col, dcol);
                hi = pv_floordiv((i64)c1v - col, dcol);
            } else if (dcol < 0) {
                lo = pv_ceildiv((i64)col - c1v, -dcol);
                hi = pv_floordiv((i64)col - c0v, -dcol);
            } else if (col >= c0v && col <= c1v)
                lo = k0, hi = k1;
            else
                lo = 1, hi = 0;
            if (lo > k0) k0 = lo;
            if (hi < k1) k1 = hi;
            if (k0 > k1)
                continue;
        }
        /* its way t: the ray at the edge's crossing, less its part along up */
        if (q->vert)
            pv_ray(f, X, (int32_t)q->edge * 256, w);
        else
            pv_ray(f, (int32_t)q->edge * 256, X, w);
        wup = pv_dot30(w, f->up);
        for (k = 0; k < 3; k++)
            T[k] = w[k] - (int32_t)(M64(f->up[k], wup) >> 30);
        tl = (int32_t)pv_isqrt((u64)(M64(T[0], T[0]) + M64(T[1], T[1]) + M64(T[2], T[2])));
        if (tl < 16)
            continue;
        for (k = 0; k < 3; k++)
            t[k] = pv_div(M64(T[k], 1L << 30), tl);
        ln->te = pv_dot30(t, f->e);
        ta = pv_dot30(t, f->a);
        tb = pv_dot30(t, f->b);
        qx = (int32_t)((M64(ta, f->ue) - M64(f->ua, ln->te)) >> 30);
        qy = (int32_t)((M64(tb, f->ue) - M64(f->ub, ln->te)) >> 30);
        ln->kx = pv_div(M64(f->R, qx), f->ue);
        ln->ky = pv_div(M64(f->R, qy), f->ue);
        ln->us = (int16_t)((M64(f->up[0], cv[0]) + M64(f->up[1], cv[1]) + M64(f->up[2], cv[2])) >> 34);
        ln->ts = (int16_t)((M64(t[0], cv[0]) + M64(t[1], cv[1]) + M64(t[2], cv[2])) >> 34);
        ln->uf = (int16_t)((M64(f->up[0], f->cf[0]) + M64(f->up[1], f->cf[1]) + M64(f->up[2], f->cf[2])) >> 34);
        ln->tf = (int16_t)((M64(t[0], f->cf[0]) + M64(t[1], f->cf[1]) + M64(t[2], f->cf[2])) >> 34);
        if (q->vert) {
            ln->ra_ = (int16_t)(q->s * (f->lcy - (int32_t)q->inner * 256));
            ln->rb_ = (int16_t)(-q->s * f->lf);
        } else {
            ln->ra_ = (int16_t)(q->s * (f->lcx - (int32_t)q->inner * 256));
            ln->rb_ = (int16_t)(q->s * f->lf);
        }
        ln->col = col;
        ln->dcol = dcol;
        ln->row = (uint8_t)q->inner;
        ln->drow = (int8_t)q->s;
        ln->k0 = (uint8_t)k0;
        ln->k1 = (uint8_t)k1;
        ln->flags = (uint8_t)((q->vert ? PV_L_VERT : 0) | PV_L_HID);
        ln->mat = 0;
        ln->prho = ln->plo = ln->phi = ln->plight = ln->pfog = ln->ph = ln->pds = 0;
        /* the horizon: the ray just short of the first pixel wanted (past the last, looking up at the ground) */
        kh = f->nadir ? k0 - 1 : k1 + 1;
        {
            int32_t along = (int32_t)(q->inner + q->s * kh) * 256, across = (col - 32768 + kh * dcol) >> 8;
            if (q->vert)
                pv_ray(f, across, along, w);
            else
                pv_ray(f, along, across, w);
            ln->hu = pv_dot30(w, f->up);
            ln->ht = pv_dot30(w, t);
            ln->horizon = (int16_t)(kh * 256);
        }
        ln->step = 0;
        if (f->D > f->rt) {
            /* above every hill: nothing before the lowest of the line's rays reaches the highest ground can show */
            int32_t along, across;
            int s0;
            kl = f->nadir ? k0 : k1;
            along = (int32_t)(q->inner + q->s * kl) * 256;
            across = (col - 32768 + kl * dcol) >> 8;
            if (q->vert)
                pv_ray(f, across, along, w);
            else
                pv_ray(f, along, across, w);
            s0 = pv_enter(f, pv_dot30(w, f->up), pv_dot30(w, t));
            if (s0 < 0)
                continue;
            ln->step = (int16_t)s0;
        }
        f->nlines++;
    }
}

/* --- the march's parts --- */

/* the haze between the eye and a point (its distance, plane units; its height, BK_HU): a level */
static int pv_fog_at(const PvFrame PV_FAR *f, int32_t dist, int32_t h)
{
    const int32_t PV_FAR *fogm = (const int32_t PV_FAR *)(f->scr + f->o_fogm);
    int32_t b = (int32_t)(M64(h - f->fog_h0, f->fog_rcp) >> 16), k;
    if (b < 0) b = 0;
    if (b > PV_FOG_HB - 1) b = PV_FOG_HB - 1;
    k = (int32_t)(M64(dist, fogm[b]) >> 24);
    if (k < 0) k = 0;
    if (k > PV_FOG_TAUS - 1) k = PV_FOG_TAUS - 1;
    return f->scr[f->o_foglev + k];
}

#ifdef PV_DEBUG
unsigned long g_cnt[12];               /* fills, fill pixels, skip calls, skip scans, skip hits, overs, grounds, slab_ats, rhos, places, aboves, samples */
#define CNT(k, n) (g_cnt[k] += (n))
#else
#define CNT(k, n) (void)0
#endif
/* a line's pixels i0 .. i1 still empty filled with the ground between two samples: the light and the haze (256ths)
 * weighed from a's (pixel ra, 256ths) to b's (tr_fill_ground) */
static void pv_fill(const PvFrame PV_FAR *f, const PvLine PV_FAR *ln, int32_t i0, int32_t i1, int32_t ra, int32_t rb,
                    int32_t la, int32_t lb, int32_t fa, int32_t fb, int mat)
{
    uint16_t PV_FAR *pic = (uint16_t PV_FAR *)(f->buf + f->o_pic);
    int32_t span = rb - ra, t0, dt, l, fo, acc, dacc = 0, col, row, i, base = (int32_t)mat * PV_LIGHTS;
    CNT(0, 1);
    CNT(1, i1 - i0 + 1);
    if (span == 0) {
        t0 = 65536;
        dt = 0;
    } else {
        int32_t num = i0 * 256 - ra;
        if (num == 0 || (num < 0) != (span < 0))
            t0 = 0;
        else if ((num < 0 ? -num : num) >= (span < 0 ? -span : span))
            t0 = 65536;
        else
            t0 = pv_div(M64(num, 65536), span);
        dt = pv_div(256LL * 65536, span);
        if (M64(dt, i1 - i0) + t0 > 65536 + 7 || M64(dt, i1 - i0) + t0 < -7)
            dt = i1 > i0 ? (65536 - t0) / (i1 - i0) : 0;
    }
    l = la * 256 + ((lb - la) * t0 >> 8) + 32768;
    fo = fa * 256 + ((fb - fa) * t0 >> 8) + 32768;
    /* the light and the haze in one sum, 8.8 each, the haze's the high half: a pixel's light is its bits 8-15, its
     * haze its bits 24-31.  Both stay in 0 .. 32 (pv_begin checked the tables; t within -7 .. 65543 / 65536), so
     * neither half carries into the other, and no pixel needs holding to the ranges */
    acc = (fo >> 8) * 65536 + (l >> 8);
    if (i1 > i0)                        /* (one pixel: dt unused, and may be past what the sum holds) */
        dacc = (int32_t)(M64(fb - fa, dt) >> 16) * 65536 + (int32_t)(M64(lb - la, dt) >> 16);
    row = ln->row + i0 * ln->drow;
    col = ln->col + i0 * ln->dcol;
    for (i = i0; i <= i1; i++, row += ln->drow, col += ln->dcol, acc += dacc) {
        int32_t x = (ln->flags & PV_L_VERT) ? col >> 16 : row, y = (ln->flags & PV_L_VERT) ? row : col >> 16;
        uint16_t PV_FAR *p, v;
        if ((uint32_t)x >= PV_LW || (uint32_t)y >= PV_LH)
            continue;
        p = &pic[y * PV_LW + x];
        v = (uint16_t)((base + ((acc >> 8) & 255)) | (acc >> 24) << 8);
        if (*p == PV_EMPTY)
            *p = v;
        if (f->wide) {                  /* (and the next pixel across: the line not marched between this and the next) */
            int32_t x2 = (ln->flags & PV_L_VERT) ? x + 1 : x, y2 = (ln->flags & PV_L_VERT) ? y : y + 1;
            if (x2 < PV_LW && y2 < PV_LH && pic[y2 * PV_LW + x2] == PV_EMPTY)
                pic[y2 * PV_LW + x2] = v;
        }
    }
}

/* the pixels between the horizon and a sample's (both 256ths), as tr_span, within the line's wanted ones: 0 when none */
static int pv_span(const PvFrame PV_FAR *f, const PvLine PV_FAR *ln, int32_t rho, int32_t *i0, int32_t *i1)
{
    if (f->nadir) {
        *i0 = (ln->horizon >> 8) + 1;
        *i1 = rho >> 8;
    } else {
        *i0 = -((-rho) >> 8);
        *i1 = -((-(int32_t)ln->horizon) >> 8) - 1;
    }
    if (*i0 < ln->k0) *i0 = ln->k0;
    if (*i1 > ln->k1) *i1 = ln->k1;
    return *i0 <= *i1;
}

/* a point (a, b) from the eye, along up and t, is above the horizon's ray: seen */
static int pv_above(const PvLine PV_FAR *ln, int32_t a, int32_t b)
{
    return M64(a, ln->ht) - M64(b, ln->hu) > 0;
}

/* the point at a step at height h (BK_HU) from the eye */
static void pv_place(const PvFrame PV_FAR *f, const PvStep PV_FAR *st, int32_t h, int32_t *a, int32_t *b)
{
    *a = st->a + (int32_t)(M64(h, st->c) >> (28 + f->vsh));
    *b = st->b + (int32_t)(M64(h, st->s) >> (28 + f->vsh));
}

/* a point's pixel on the line, 256ths (held to 16 bits): 0 when it is behind the eye */
static int pv_rho(const PvLine PV_FAR *ln, int32_t ra, int32_t rb, int32_t *rho)
{
    int32_t z = ra * ln->uf + rb * ln->tf, num, r;
    CNT(8, 1);
    if (z <= 0)
        return 0;
    num = ra * ln->us + rb * ln->ts;
    if ((num < 0 ? -num : num) >> 16 >= z)
        r = (num < 0) == (ln->rb_ < 0) ? 1L << 30 : -(1L << 30);
    else
        r = pv_div(M64(num, ln->rb_), z);
    r += ln->ra_;
    *rho = r < -32768L ? -32768L : r > 32767 ? 32767 : r;
    return 1;
}

/* is the line over: the horizon's ray has left the top sphere behind it (or misses it) */
static int pv_over(const PvFrame PV_FAR *f, const PvLine PV_FAR *ln, const PvStep PV_FAR *st)
{
    int32_t nn = (int32_t)pv_isqrt((u64)(M64(ln->hu, ln->hu) + M64(ln->ht, ln->ht))), b, s1, x, y;
    i64 disc;
    CNT(5, 1);
    if (!nn)
        return 1;
    b = pv_div(M64(f->D, ln->hu), nn);
    disc = M64(b, b) - (M64(f->D, f->D) - M64(f->rt, f->rt));
    if (disc <= 0)
        return 1;
    s1 = -b + (int32_t)pv_isqrt((u64)disc);
    if (s1 <= 0)
        return 1;
    x = f->D + pv_div(M64(s1, ln->hu), nn);
    y = pv_div(M64(s1, ln->ht), nn);
    return M64(st->s, x) - M64(st->c, y) > 0;
}

/* a sample at the line's march: seen (filling the pixels it is the nearest ground for) or not; and the slab over it
 * (slab: lo .. hi, BK_HU) drawn where nothing nearer is.  h its height, light 256ths */
static void pv_sample(PvFrame PV_FAR *f, PvLine PV_FAR *ln, const PvStep PV_FAR *st, int32_t h, int mat, int32_t light,
                      int slab, int32_t lo, int32_t hi)
{
    int32_t ra, rb, rho, i0, i1;
    f->samples++;
    CNT(11, 1);
    pv_place(f, st, h, &ra, &rb);
    if (!pv_above(ln, ra, rb) || !pv_rho(ln, ra, rb, &rho)) {
        ln->flags = (uint8_t)((ln->flags | PV_L_HID) & ~PV_L_POK);
    } else {
        int32_t fog = (int32_t)pv_fog_at(f, st->dist, h) << 8;
        f->seen++;
        if (pv_span(f, ln, rho, &i0, &i1)) {
            if ((ln->flags & PV_L_POK) && ln->mat == mat)
                pv_fill(f, ln, i0, i1, ln->prho, rho, ln->plight, light, ln->pfog, fog, mat);
            else if ((ln->flags & PV_L_POK) && (h - ln->ph) * 4 < ln->pds / 2 && (ln->ph - h) * 4 < ln->pds / 2) {
                /* another material on gentle ground: the pixels nearer the last sample its, the rest this one's */
                int32_t mid = ((int32_t)ln->prho + rho) / 2, m0, m1;
                if (pv_span(f, ln, mid, &m0, &m1))
                    pv_fill(f, ln, m0, m1, ln->prho, ln->prho, ln->plight, ln->plight, ln->pfog, ln->pfog, ln->mat);
                pv_fill(f, ln, i0, i1, rho, rho, light, light, fog, fog, mat);
            } else
                pv_fill(f, ln, i0, i1, rho, rho, light, light, fog, fog, mat);
        }
        ln->pfog = (int16_t)fog;
        ln->horizon = (int16_t)rho;
        ln->hu = ra;
        ln->ht = rb;
        ln->flags = (uint8_t)((ln->flags & ~PV_L_HID) | PV_L_POK);
        if (f->nadir ? rho >= (int32_t)ln->k1 * 256 : rho <= (int32_t)ln->k0 * 256)
            ln->flags |= PV_L_DONE;
        ln->prho = (int16_t)rho;
        ln->plight = (int16_t)light;
        ln->ph = (int16_t)h;
        ln->mat = (uint8_t)mat;
        ln->pds = st->ds;
    }
    /* the slab: the rock between its underside and top, in front of whatever is still to come */
    if (slab) {
        int32_t la, lb, ha, hb, r0, r1, s0, s1;
        pv_place(f, st, lo, &la, &lb);
        pv_place(f, st, hi, &ha, &hb);
        if (pv_rho(ln, la, lb, &r0) && pv_rho(ln, ha, hb, &r1)) {
            int32_t sm, sl, sf;
            s0 = r0 < r1 ? r0 : r1;
            s1 = r0 < r1 ? r1 : r0;
            if (ln->flags & PV_L_PSLAB) {
                if (ln->plo < s0) s0 = ln->plo;
                if (ln->phi > s1) s1 = ln->phi;
            }
            sm = f->alt < lo ? 3 : 1;   /* (terrain.c's TR_UNDER, TR_ROCK) */
            if (f->alt > hi)
                sl = light >> 8;
            else {
                int32_t gap = lo > h ? (lo - h) * 16 / f->under_div : 0;
                sl = f->scr[f->o_under + (gap > 63 ? 63 : gap)];
            }
            sf = pv_fog_at(f, st->dist, hi);
            i0 = -((-s0) >> 8);
            i1 = s1 >> 8;
            if (i0 < ln->k0) i0 = ln->k0;
            if (i1 > ln->k1) i1 = ln->k1;
            if (i0 <= i1)
                pv_fill(f, ln, i0, i1, 0, 0, sl << 8, sl << 8, sf << 8, sf << 8, (int)sm);
            ln->plo = (int16_t)(r0 < r1 ? r0 : r1);
            ln->phi = (int16_t)(r0 < r1 ? r1 : r0);
            ln->flags |= PV_L_PSLAB;
            if (f->nadir ? s1 > ln->horizon : s0 < ln->horizon)
                ln->flags &= ~PV_L_HID;  /* (some of it is past the filled part: it may hide what follows) */
        } else
            ln->flags &= ~PV_L_PSLAB;
    } else
        ln->flags &= ~PV_L_PSLAB;
}

/* a line's plane point at a step: m (65536ths), 0 when the step is past the plane's reach */
static int pv_m(const PvLine PV_FAR *ln, const PvStep PV_FAR *st, int32_t *m)
{
    int32_t den = st->uc + (int32_t)(M64(ln->te, st->s) >> 30);
    if (den < PV_DEN_MIN)
        return 0;
    *m = pv_div((i64)st->s << 16, den);
    return 1;
}

/* the ground at a plane point from the level's window: its height (BK_HU), colour id and light (256ths): 0 when the
 * window has no ground there */
static int pv_ground(const PvFrame PV_FAR *f, int32_t X, int32_t Y, int32_t *h, int *c0, int32_t *light)
{
    int32_t ix = (X >> f->sh) - f->ox, iy = (Y >> f->sh) - f->oy, fx, fy, h0, h1;
    const uint8_t PV_FAR *c00, PV_FAR *c10, PV_FAR *c01, PV_FAR *c11, PV_FAR *cq;
    int diff;
    CNT(6, 1);
    if ((uint32_t)ix >= PV_W - 1 || (uint32_t)iy >= PV_W - 1)
        return 0;
    c00 = f->scr + f->o_win + (iy * PV_W + ix) * PV_CELL;
    if (c00[1] == PV_ABSENT)
        return 0;
    c10 = c00 + PV_CELL;
    c01 = c00 + PV_W * PV_CELL;
    c11 = c01 + PV_CELL;
    fx = (X >> (f->sh - 8)) & 255;
    fy = (Y >> (f->sh - 8)) & 255;
    {
        int32_t a = (int16_t)(c00[0] | c00[1] << 8), b = (int16_t)(c10[0] | c10[1] << 8);
        int32_t c = (int16_t)(c01[0] | c01[1] << 8), d = (int16_t)(c11[0] | c11[1] << 8);
        h0 = a + (((b - a) * fx) >> 8);
        h1 = c + (((d - c) * fx) >> 8);
        *h = h0 + (((h1 - h0) * fy) >> 8);
    }
    cq = (fy >= 128 ? c01 : c00) + (fx >= 128 ? PV_CELL : 0);
    *c0 = cq[2];
    diff = (c00[2] ^ *c0) | (c10[2] ^ *c0) | (c01[2] ^ *c0) | (c11[2] ^ *c0);
    if (diff < PV_LIGHTS) {
        int32_t l0 = (int32_t)(c00[2] & 31) * (256 - fx) + (int32_t)(c10[2] & 31) * fx;
        int32_t l1 = (int32_t)(c01[2] & 31) * (256 - fx) + (int32_t)(c11[2] & 31) * fx;
        *light = (l0 * (256 - fy) + l1 * fy) >> 8;
    } else
        *light = (int32_t)(*c0 & 31) << 8;
    return 1;
}

/* a plane point within one of the slab grids */
static int pv_in_slabs(const PvFrame PV_FAR *f, int32_t X, int32_t Y)
{
    int k;
    for (k = 0; k < f->nslab; k++) {
        const PvSlab PV_FAR *sl = &f->slab[k];
        if ((uint32_t)((X >> f->slab_shift) - sl->x0) < sl->w && (uint32_t)((Y >> f->slab_shift) - sl->y0) < sl->h)
            return 1;
    }
    return 0;
}

/* the slab over a plane point, its underside and top weighed between the four cells round it (a cell without one
 * counting as the slab pinched to nothing at the others' middle: terrain.c's tr_sample_patch): 1 when there is one */
static int pv_slab_at(const PvFrame PV_FAR *f, int32_t X, int32_t Y, int32_t *lo, int32_t *hi)
{
    const int16_t PV_FAR *cells = (const int16_t PV_FAR *)(f->buf + f->o_slab);
    int k;
    for (k = 0; k < f->nslab; k++) {
        const PvSlab PV_FAR *sl = &f->slab[k];
        int ssh = f->slab_shift, q, n = 0;
        int32_t sx = (X >> ssh) - sl->x0, sy = (Y >> ssh) - sl->y0, fx, fy, w[4], mid = 0, a = 0, b = 0, off[4];
        const int16_t PV_FAR *c;
        if ((uint32_t)sx >= (uint32_t)sl->w - 1 || (uint32_t)sy >= (uint32_t)sl->h - 1)
            continue;
        c = cells + 2 * (sl->at + sy * sl->w + sx);
        off[0] = 0;
        off[1] = 2;
        off[2] = 2 * (int32_t)sl->w;
        off[3] = 2 * (int32_t)sl->w + 2;
        for (q = 0; q < 4; q++)
            if (c[off[q]] < c[off[q] + 1]) {
                mid += (int32_t)c[off[q]] + c[off[q] + 1];
                n++;
            }
        if (!n)
            return 0;
        mid = mid * 128 / n;            /* (the middle, 256ths) */
        fx = (X >> (ssh - 8)) & 255;
        fy = (Y >> (ssh - 8)) & 255;
        w[0] = (256 - fx) * (256 - fy);
        w[1] = fx * (256 - fy);
        w[2] = (256 - fx) * fy;
        w[3] = fx * fy;
        for (q = 0; q < 4; q++) {
            int has = c[off[q]] < c[off[q] + 1];
            a += (int32_t)(M64(w[q], has ? (int32_t)c[off[q]] * 256 : mid) >> 8);
            b += (int32_t)(M64(w[q], has ? (int32_t)c[off[q] + 1] * 256 : mid) >> 8);
        }
        if (b - a < 32768)              /* (65536ths of a height unit: half a unit) */
            return 0;
        *lo = a >> 16;
        *hi = b >> 16;
        return 1;
    }
    return 0;
}

/* hidden ground skipped: the sample at step i is in block (bx, by) of the window (4 x 4 cells), whose ground is
 * nowhere over hb; while the line's samples stay in the block, and the block's highest is hidden at both ends of them
 * and the horizon's ray does not dip under it between (its nearest approach to the centre), none of them can be seen.
 * The step after them, or i when they cannot be skipped */
static int pv_skip(PvFrame PV_FAR *f, const PvLine PV_FAR *ln, int i, int32_t bx, int32_t by, int32_t hb)
{
    const PvStep PV_FAR *st = (const PvStep PV_FAR *)(f->buf + f->o_st);
    int sh = f->sh, j;
    int32_t a, b, m, mx = 0x7FFFFFFFL, my = 0x7FFFFFFFL, e;
    pv_place(f, &st[i], hb, &a, &b);
    if (pv_above(ln, a, b))
        return i;
    /* where the line leaves the block (m) */
    if (ln->kx) {
        e = ln->kx > 0 ? (f->ox + 4 * bx + 4) * (1L << sh) - f->px : (f->ox + 4 * bx) * (1L << sh) - f->px - 1;
        if ((e < 0 ? -e : e) >> 15 < (ln->kx < 0 ? -ln->kx : ln->kx))
            mx = pv_div(M64(e, 65536), ln->kx);
    }
    if (ln->ky) {
        e = ln->ky > 0 ? (f->oy + 4 * by + 4) * (1L << sh) - f->py : (f->oy + 4 * by) * (1L << sh) - f->py - 1;
        if ((e < 0 ? -e : e) >> 15 < (ln->ky < 0 ? -ln->ky : ln->ky))
            my = pv_div(M64(e, 65536), ln->ky);
    }
    if (my < mx)
        mx = my;
    CNT(2, 1);
    for (j = i + 1; j < f->nsteps && st[j].lev <= f->lev && pv_m(ln, &st[j], &m) && m < mx; j++)
        CNT(3, 1);
    if (--j <= i)
        return i;
    pv_place(f, &st[j], hb, &a, &b);
    if (pv_above(ln, a, b))
        return i;
    /* the ray's nearest approach to the centre, if it lies between and the block's highest reaches over it */
    if (ln->ht > 0 && M64(st[i].c, ln->hu) + M64(st[i].s, ln->ht) <= 0 && M64(st[j].c, ln->hu) + M64(st[j].s, ln->ht) >= 0) {
        int32_t hl = (int32_t)pv_isqrt((u64)(M64(ln->hu, ln->hu) + M64(ln->ht, ln->ht)));
        if (hl && pv_div(M64(f->D, ln->ht), hl) <= f->R + hb * 4)
            return i;
    }
    f->skips += j + 1 - i;
    CNT(4, 1);
    return j + 1;
}


/* every line marched through the frame's level (the window round the eye's point), each until it leaves the level */
void c_pv_march(PvFrame PV_FAR *f)
{
    PvStep PV_FAR *steps = (PvStep PV_FAR *)(f->buf + f->o_st);
    PvLine PV_FAR *lines = (PvLine PV_FAR *)(f->buf + f->o_ln);
    const int16_t PV_FAR *bw = (const int16_t PV_FAR *)(f->scr + f->o_bwin);
    int sh = f->sh, n;
    for (n = 0; n < f->nlines; n++) {
        PvLine PV_FAR *ln = &lines[n];
        while (!(ln->flags & PV_L_DONE) && ln->step < f->nsteps) {
            const PvStep PV_FAR *st = &steps[ln->step];
            int32_t m, X, Y, ix, iy, h, light, lo, hi;
            int c0, inr, sb;
            if (st->lev > f->lev || !pv_m(ln, st, &m))
                break;
            X = f->px + (int32_t)(M64(ln->kx, m) >> 16);
            Y = f->py + (int32_t)(M64(ln->ky, m) >> 16);
            ix = (X >> sh) - f->ox;
            iy = (Y >> sh) - f->oy;
            if ((uint32_t)ix >= PV_W - 1 || (uint32_t)iy >= PV_W - 1
                || f->scr[f->o_win + (iy * PV_W + ix) * PV_CELL + 1] == PV_ABSENT)
                break;
            if (ln->flags & PV_L_HID) {
                int nx = pv_skip(f, ln, ln->step, ix >> 2, iy >> 2, bw[(iy >> 2) * PV_BW + (ix >> 2)]);
                if (nx != ln->step) {
                    ln->step = (int16_t)nx;
                    ln->flags &= ~(PV_L_POK | PV_L_PRECT);
                    continue;
                }
            }
            /* crossing a slab's grid: samples between the last step and this one, half a slab cell apart (the low
             * picture's steps far off are longer than an arch is thick) */
            inr = pv_in_slabs(f, X, Y);
            if ((inr || (ln->flags & PV_L_PRECT)) && ln->step > 0) {
                const PvStep PV_FAR *a0 = &steps[ln->step - 1];
                int32_t m0, pX, pY, dx, dy, ns, j;
                if (pv_m(ln, a0, &m0)) {
                    pX = f->px + (int32_t)(M64(ln->kx, m0) >> 16);
                    pY = f->py + (int32_t)(M64(ln->ky, m0) >> 16);
                    dx = X - pX;
                    dy = Y - pY;
                    ns = ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy)) >> (f->slab_shift - 1 + f->wide);   /* (wide:
                                           a slab cell apart, PV_SUBS_WIDE at most) */
                    if (ns > (f->wide ? PV_SUBS_WIDE : PV_SUBS))
                        ns = f->wide ? PV_SUBS_WIDE : PV_SUBS;
                    for (j = 1; j < ns && !(ln->flags & PV_L_DONE); j++) {
                        PvStep t;
                        int32_t mj, Xj, Yj;
                        t.c = a0->c + pv_div(M64(st->c - a0->c, j), ns);
                        t.s = a0->s + pv_div(M64(st->s - a0->s, j), ns);
                        t.a = a0->a + pv_div(M64(st->a - a0->a, j), ns);
                        t.b = a0->b + pv_div(M64(st->b - a0->b, j), ns);
                        t.uc = a0->uc + pv_div(M64(st->uc - a0->uc, j), ns);
                        t.dist = a0->dist + pv_div(M64(st->dist - a0->dist, j), ns);
                        t.ds = (int16_t)(a0->ds / ns);
                        t.lev = st->lev;
                        if (!pv_m(ln, &t, &mj))
                            break;
                        Xj = f->px + (int32_t)(M64(ln->kx, mj) >> 16);
                        Yj = f->py + (int32_t)(M64(ln->ky, mj) >> 16);
                        if (!pv_ground(f, Xj, Yj, &h, &c0, &light))
                            break;
                        sb = pv_slab_at(f, Xj, Yj, &lo, &hi);
                        f->subs++;
                        pv_sample(f, ln, &t, h, c0 >> 5, light, sb, lo, hi);
                    }
                    if (ln->flags & PV_L_DONE)
                        break;
                }
            }
            ln->flags = (uint8_t)((ln->flags & ~PV_L_PRECT) | (inr ? PV_L_PRECT : 0));
            (void)pv_ground(f, X, Y, &h, &c0, &light);
            lo = hi = 0;
            sb = inr && pv_slab_at(f, X, Y, &lo, &hi);
            pv_sample(f, ln, st, h, c0 >> 5, light, sb, lo, hi);
            ln->step++;
            if ((ln->step & 15) == 0 && !(ln->flags & PV_L_DONE) && pv_over(f, ln, st))
                ln->flags |= PV_L_DONE;
        }
    }
}

/* the lines past every level: the plain sphere */
void c_pv_far(PvFrame PV_FAR *f)
{
    PvStep PV_FAR *steps = (PvStep PV_FAR *)(f->buf + f->o_st);
    PvLine PV_FAR *lines = (PvLine PV_FAR *)(f->buf + f->o_ln);
    int n, c = f->far_c;
    for (n = 0; n < f->nlines; n++) {
        PvLine PV_FAR *ln = &lines[n];
        while (!(ln->flags & PV_L_DONE) && ln->step < f->nsteps) {
            const PvStep PV_FAR *st = &steps[ln->step];
            pv_sample(f, ln, st, 0, c >> 5, (int32_t)(c & 31) << 8, 0, 0, 0);
            ln->step++;
            if ((ln->step & 15) == 0 && !(ln->flags & PV_L_DONE) && pv_over(f, ln, st))
                ln->flags |= PV_L_DONE;
        }
    }
}

/* the glow's table index for a picture point's ray (its cosine from straight up) */
static int32_t pv_glow_index(const PvFrame PV_FAR *f, int32_t x, int32_t y)
{
    int32_t dx = pv_div(M64(x * 256 - f->lcx, 65536), f->lf), dy = pv_div(M64(y * 256 - f->lcy, 65536), f->lf);
    int32_t num = f->skf + (int32_t)(M64(dx, f->skr) >> 16) - (int32_t)(M64(dy, f->sku) >> 16);
    int32_t den = (int32_t)pv_isqrt((u64)((1LL << 32) + M64(dx, dx) + M64(dy, dy)));
    int32_t cz = pv_div(M64(num, 65536), den), i = (int32_t)(M64(cz + 65536, PV_GLOW_CZ) >> 17);
    return i < 0 ? 0 : i > PV_GLOW_CZ - 1 ? PV_GLOW_CZ - 1 : i;
}

/* the picture's empty pixels in the view glowing where the air between the eye and space is thick enough: the rays of
 * every 8th pixel each way worked out, those between weighed */
void c_pv_glow(PvFrame PV_FAR *f)
{
    uint16_t PV_FAR *pic = (uint16_t PV_FAR *)(f->buf + f->o_pic);
    const uint8_t PV_FAR *glow = f->scr + f->o_glow;
    int16_t g[PV_LH / 8 + 1][PV_LW / 8 + 1], r[PV_LW / 8 + 1];
    int x, y, k;
    if (!f->glow_any)
        return;
    for (y = 0; y <= PV_LH / 8; y++)
        for (x = 0; x <= PV_LW / 8; x++)
            g[y][x] = (int16_t)pv_glow_index(f, x * 8, y * 8);
    for (y = f->by0; y < f->by1; y++) {
        uint16_t PV_FAR *row = pic + y * PV_LW;
        for (k = 0; k <= PV_LW / 8; k++)
            r[k] = (int16_t)(g[y >> 3][k] + (((g[(y >> 3) + 1][k] - g[y >> 3][k]) * (y & 7)) >> 3));
        for (x = f->bx0; x < f->bx1; x++)
            if (row[x] == PV_EMPTY) {
                int i = r[x >> 3] + (((r[(x >> 3) + 1] - r[x >> 3]) * (x & 7)) >> 3), v = glow[i];
                if (v >= 4)
                    row[x] = (uint16_t)(PV_GLOW | ((v + 4) >> 3));
            }
    }
}

/* --- the frame --- */

/* a quarter of the picture round V, and its lines' thinning near V (tr_quarter) */
void c_pv_quarter(const PvFrame PV_FAR *f, int which, PvQuarter PV_FAR *q, uint8_t PV_FAR *thin)
{
    int32_t amax, cmax, ext, xs, lo = 0x7FFFFFFFL, hi = -0x7FFFFFFFL, L, rn;
    int k, kk;
    q->on = 0;
    q->vert = which < 2;
    q->s = (which & 1) ? 1 : -1;
    q->va = q->vert ? f->vy : f->vx;
    q->vc = q->vert ? f->vx : f->vy;
    amax = (q->vert ? PV_LH : PV_LW) - 1;
    cmax = (q->vert ? PV_LW : PV_LH) - 1;
    q->edge = (int16_t)(q->s < 0 ? 0 : amax);
    if (q->s > 0) {
        int32_t nr = (q->va >> 8) + 1;
        if (nr < 0)
            nr = 0;
        if (nr > amax)
            return;
        q->inner = (int16_t)nr;
    } else {
        int32_t nr = -((-q->va) >> 8) - 1;
        if (nr > amax)
            nr = amax;
        if (nr < 0)
            return;
        q->inner = (int16_t)nr;
    }
    q->len = (int16_t)(q->s > 0 ? amax - q->inner + 1 : q->inner + 1);
    ext = (int32_t)q->edge * 256 - q->va;
    if (ext < 0)
        ext = -ext;
    if (q->vc - ext > cmax * 256 || q->vc + ext < 0)
        return;                         /* (its wedge misses the picture: V far off across it) */
    for (k = 0; k < 4; k++) {
        int32_t a = (int32_t)(k < 2 ? q->inner : q->edge) * 256, half = a - q->va, cc, num, den, e;
        if (half < 0)
            half = -half;
        cc = (k & 1) ? q->vc + half : q->vc - half;
        if (cc < 0) cc = 0;
        if (cc > cmax * 256) cc = cmax * 256;
        num = cc - q->vc;
        den = a - q->va;
        e = (int32_t)q->edge * 256 - q->va;
        if ((num < 0 ? -num : num) > (den < 0 ? -den : den))   /* (the corner outside the wedge there: past its edge) */
            xs = ((num < 0) != (den < 0)) != (e < 0) ? q->vc - ext : q->vc + ext;
        else
            xs = q->vc + bk_muldiv(num, e, den);
        if (xs < lo) lo = xs;
        if (xs > hi) hi = xs;
    }
    if (lo < q->vc - ext) lo = q->vc - ext;
    if (hi > q->vc + ext) hi = q->vc + ext;
    if (hi < lo)
        return;
    q->first = (int16_t)((lo >> 8) - 1);
    q->lines = (int16_t)(-((-(hi - (int32_t)q->first * 256)) >> 8) + 2);
    /* the lines' spacing at pixel k is (k + rn) / L: a line whose index has m trailing zero bits is wanted from where
     * twice 2^m of it reaches a pixel ((k + rn) 2^kk >= L: k + rn at least L over 2^kk, rounded up) */
    L = ext;
    rn = q->s * ((int32_t)q->inner * 256 - q->va);
    kk = 12;
    for (k = 0; k < q->len; k++) {
        int32_t sp = (int32_t)k * 256 + rn;
        while (kk > 0 && sp >= ((L + (1L << kk) - 1) >> kk))
            kk--;
        while (kk < 12 && sp < ((L + (2L << kk) - 1) >> (kk + 1)))
            kk++;
        thin[k] = (uint8_t)kk;
    }
    q->on = 1;
}

/* the picture's pixels as 5 x 5 blocks over a band of the screen: a picture row's two patterns (block rows 0, 2, 4 by
 * the first row of thresholds, 1 and 3 by the second) and its runs of pixels with ground (or glow) made at its first
 * screen row, each screen row then copied from its pattern a run at a time, within the opening */
static const uint8_t pv_th[2][PV_BLOCK] = { { 0, 6, 2, 8, 4 }, { 5, 1, 7, 3, 9 } };

void c_pv_blit(const PvBlit PV_FAR *b)
{
    uint8_t PV_FAR *pa = b->work, PV_FAR *pb = b->work + PV_LW * PV_BLOCK;
    int16_t PV_FAR *runs = (int16_t PV_FAR *)(b->work + 2 * PV_LW * PV_BLOCK);
    int y, ly = -1, nr = 0, k;
    for (y = b->y0; y < b->y1; y++) {
        int cy = y / PV_BLOCK, xa = 0, xb = PV_LW * PV_BLOCK;
        uint8_t PV_FAR *row = b->band + (int32_t)(y - b->y0) * (PV_LW * PV_BLOCK), PV_FAR *pat;
        if (cy != ly) {
            const uint16_t PV_FAR *src = b->pic + (int32_t)(cy - b->ly0) * PV_LW;
            int x, in = 0;
            nr = 0;
            for (x = 0; x < PV_LW; x++) {
                uint16_t v = src[x];
                const uint8_t PV_FAR *d;
                if (v == PV_EMPTY) {
                    if (in) {
                        runs[2 * nr + 1] = (int16_t)(x * PV_BLOCK);
                        nr++;
                        in = 0;
                    }
                    continue;
                }
                if (!in) {
                    runs[2 * nr] = (int16_t)(x * PV_BLOCK);
                    in = 1;
                }
                d = v >= PV_GLOW ? b->dither + (PV_FOGS * 256 + (v & 31)) * 3 : b->dither + (uint32_t)v * 3;
                for (k = 0; k < PV_BLOCK; k++) {
                    pa[x * PV_BLOCK + k] = pv_th[0][k] < d[2] ? d[1] : d[0];
                    pb[x * PV_BLOCK + k] = pv_th[1][k] < d[2] ? d[1] : d[0];
                }
            }
            if (in) {
                runs[2 * nr + 1] = PV_LW * PV_BLOCK;
                nr++;
            }
            ly = cy;
        }
        if (b->ext) {
            if (y < b->ext_y0 || y >= b->ext_y0 + b->ext_rows)
                continue;
            xa = b->ext[2 * (y - b->ext_y0)];
            xb = b->ext[2 * (y - b->ext_y0) + 1];
        }
        pat = ((y - cy * PV_BLOCK) & 1) ? pb : pa;
        for (k = 0; k < nr; k++) {
            int x0 = runs[2 * k] > xa ? runs[2 * k] : xa, x1 = runs[2 * k + 1] < xb ? runs[2 * k + 1] : xb;
            if (x0 < x1)
                _fmemcpy(row + x0, pat + x0, x1 - x0);
        }
    }
}
#endif /* (the C twins) */


/* the haze for the frame: by a sample's height, the sum's rate along the distance (2^24) */
static void pv_fog_frame(PvFrame PV_FAR *f)
{
    int32_t PV_FAR *fogm = (int32_t PV_FAR *)(f->scr + f->o_fogm);
    int32_t PV_FAR *G = (int32_t PV_FAR *)(f->scr + f->o_win);   /* (the window's room: none is in yet) */
    int32_t h0 = g_pv.h.bottom, hs = g_pv.fog_hstep, ha = f->alt, k = (ha - h0) / hs, fr, ga, b;
    pv_get(g_pv.h.fog_at + 12, G, sizeof(int32_t) * PV_FOG_GS);
    if (ha < h0)
        k = 0, fr = 0;
    else if (k >= PV_FOG_GS - 1)
        k = PV_FOG_GS - 2, fr = hs;
    else
        fr = ha - h0 - k * hs;
    ga = G[k] + bk_muldiv(G[k + 1] - G[k], fr, hs);
    for (b = 0; b < PV_FOG_HB; b++) {
        int32_t hb = h0 + b * hs, dh = hb - ha, dg = G[b] - ga;
        if (dh < hs / 2 && dh > -hs / 2) {
            dh = hs;                    /* (level with the eye: the rate there) */
            dg = G[k + 1] - G[k];
        }
        fogm[b] = bk_muldiv(dg, g_pv.fog_k, dh * 256);
    }
    f->fog_h0 = h0;
    f->fog_rcp = 65536L / hs;
}

/* the sky's glow for the frame: the table's two rows round the eye's height, weighed */
static void pv_glow_frame(PvFrame PV_FAR *f)
{
    uint8_t PV_FAR *out = f->scr + f->o_glow, PV_FAR *g2 = f->scr + f->o_win;
    int32_t a = f->D - f->R, j, fr;
    int k, any = 0;
    if (a < 0)
        a = 0;
    if (a > g_pv.glow_amax)
        a = g_pv.glow_amax;
    /* (rows at amax (j / 63)^2: j = 63 sqrt(a / amax), in 256ths) */
    j = (int32_t)pv_isqrt32((uint32_t)bk_muldiv(a, INT32_C(260112384), g_pv.glow_amax));
    if (j > 63 * 256)
        j = 63 * 256;
    fr = j & 255;
    j >>= 8;
    {   /* (the table's two rows, in the window's room: none is in yet) */
        int rows = j < PV_GLOW_AS - 1 ? 2 : 1;
        pv_get(g_pv.h.glow_at + 4 + sizeof(PvDither) * PV_FOGS + (uint32_t)j * PV_GLOW_CZ, g2, (uint32_t)rows * PV_GLOW_CZ);
        if (rows == 1)
            _fmemcpy(g2 + PV_GLOW_CZ, g2, PV_GLOW_CZ);
    }
    for (k = 0; k < PV_GLOW_CZ; k++) {
        int32_t g0 = g2[k], g1 = g2[PV_GLOW_CZ + k];
        out[k] = (uint8_t)(g0 + (((g1 - g0) * fr + 128) >> 8));
        any |= out[k] >= 4;
    }
    f->glow_any = (int16_t)any;
}

/* level l's window, its corner at cell (ox, oy): the level's cells there, no ground elsewhere, and its blocks'
 * highest, into the frame's scratch buffer */
#ifndef __WATCOMC__
static void pv_window(PvFrame PV_FAR *f, int l, int32_t ox, int32_t oy)
{
    PvLevel lvr, *lv = &lvr;
    uint8_t *w = g_win[l];
    int j, i;
    if (!g_wok[l] || g_wox[l] != ox || g_woy[l] != oy) {
        int16_t *b = g_bwin[l];
        pv_get(g_pv.h.lev_at + (uint32_t)l * sizeof(PvLevel), lv, sizeof(PvLevel));
        int32_t bx = ox / PV_BLK - lv->bx0, by = oy / PV_BLK - lv->by0;
        pv_get(g_pv.h.absent_at, w, PV_WIN_BYTES);
        for (j = 0; j < PV_W; j++) {
            int32_t r = oy + j - lv->y0, a0, a1, ax0;
            PvRow row;
            if (r < 0 || r >= lv->h)
                continue;
            pv_get(lv->rows_at + (uint32_t)r * sizeof row, &row, sizeof row);
            if (!row.n)
                continue;
            ax0 = lv->x0 + row.x0;
            a0 = ox > ax0 ? ox : ax0;
            a1 = ox + PV_W < ax0 + row.n ? ox + PV_W : ax0 + row.n;
            if (a0 >= a1)
                continue;
            pv_get(row.at + (uint32_t)(a0 - ax0) * PV_CELL, w + ((uint32_t)j * PV_W + (a0 - ox)) * PV_CELL,
                   (uint32_t)(a1 - a0) * PV_CELL);
        }
        for (j = 0; j < PV_BW; j++)
            for (i = 0; i < PV_BW; i++) {
                int16_t v = PV_NOGROUND;
                if (by + j >= 0 && by + j < lv->bh && bx + i >= 0 && bx + i < lv->bw)
                    pv_get(lv->bmax_at + ((uint32_t)(by + j) * lv->bw + (bx + i)) * 2, &v, 2);
                b[j * PV_BW + i] = v;
            }
        g_wox[l] = ox;
        g_woy[l] = oy;
        g_wok[l] = 1;
    }
    memcpy(f->scr + f->o_win, w, PV_WIN_BYTES);
    memcpy(f->scr + f->o_bwin, g_bwin[l], sizeof g_bwin[l]);
}
#endif

#ifndef __WATCOMC__
/* the frame's numbers from the camera and the bake's head: 0 when there is no ground to draw */
int c_pv_setup(PvFrame PV_FAR *f, const PvCam PV_FAR *cam, const PvHead PV_FAR *h)
{
    int32_t e[3], nx, ny, nz, maxd;
    int k;
    f->samples = f->seen = f->skips = f->subs = 0;
    f->nlines = f->nsteps = 0;
    f->R = h->radius;
    for (k = 0; k < 3; k++) {
        e[k] = cam->eye[k] / PV_PU;
        f->cr[k] = cam->m[k];
        f->cu[k] = cam->m[3 + k];
        f->cf[k] = cam->m[6 + k];
        f->e[k] = h->e[k];
        f->a[k] = h->a[k];
        f->b[k] = h->b[k];
    }
    f->D = (int32_t)pv_isqrt((u64)(M64(e[0], e[0]) + M64(e[1], e[1]) + M64(e[2], e[2])));
    if (f->D <= 0)
        return 0;
    for (k = 0; k < 3; k++)
        f->up[k] = bk_muldiv(e[k], 1L << 30, f->D);
    f->rt = f->R + (int32_t)h->top * 4 + 16;
    f->rb = f->R + (int32_t)h->bottom * 4 - 16;
    f->alt = (f->D - f->R) / 4;
    f->ue = pv_dot30(f->up, f->e);
    f->ua = pv_dot30(f->up, f->a);
    f->ub = pv_dot30(f->up, f->b);
    f->px = f->py = 0;
    if (f->ue > (1L << 30) / 8) {
        f->px = bk_muldiv(f->R, f->ua, f->ue);
        f->py = bk_muldiv(f->R, f->ub, f->ue);
    }
    /* V: straight down's point on the picture (straight up's when it is behind) */
    nx = -(int32_t)((M64(f->up[0], f->cr[0]) + M64(f->up[1], f->cr[1]) + M64(f->up[2], f->cr[2])) >> 30);
    ny = -(int32_t)((M64(f->up[0], f->cu[0]) + M64(f->up[1], f->cu[1]) + M64(f->up[2], f->cu[2])) >> 30);
    nz = -(int32_t)((M64(f->up[0], f->cf[0]) + M64(f->up[1], f->cf[1]) + M64(f->up[2], f->cf[2])) >> 30);
    if (nz < 1 && nz > -1)
        nz = nz < 0 ? -1 : 1;
    f->nadir = nz > 0;
    f->lcx = ((int32_t)cam->cx - 2) * 256 / PV_BLOCK;
    f->lcy = ((int32_t)cam->cy - 2) * 256 / PV_BLOCK;
    f->lf = (int32_t)cam->focal * 256 / PV_BLOCK;
    f->bx0 = (int16_t)(cam->x0 / PV_BLOCK);
    f->by0 = (int16_t)(cam->y0 / PV_BLOCK);
    f->bx1 = (int16_t)((cam->x1 + PV_BLOCK - 1) / PV_BLOCK);
    f->by1 = (int16_t)((cam->y1 + PV_BLOCK - 1) / PV_BLOCK);
    if (f->bx0 < 0) f->bx0 = 0;
    if (f->by0 < 0) f->by0 = 0;
    if (f->bx1 > PV_LW) f->bx1 = PV_LW;
    if (f->by1 > PV_LH) f->by1 = PV_LH;
    {
        i64 vx = f->lcx + M64(f->lf, nx) / nz, vy = f->lcy - M64(f->lf, ny) / nz;
        if (vx > (1L << 28)) vx = 1L << 28;
        if (vx < -(1L << 28)) vx = -(1L << 28);
        if (vy > (1L << 28)) vy = 1L << 28;
        if (vy < -(1L << 28)) vy = -(1L << 28);
        f->vx = (int32_t)vx;
        f->vy = (int32_t)vy;
    }
    /* the vertical numbers' scale: the farthest ground seen within 2^20 (2^23 at most: farther, nothing is drawn) */
    maxd = (f->D > f->rb ? (int32_t)pv_isqrt((u64)(M64(f->D, f->D) - M64(f->rb, f->rb))) : 0)
        + (int32_t)pv_isqrt((u64)(M64(f->rt, f->rt) - M64(f->rb, f->rb))) + 65536;
    for (f->vsh = 0; (maxd >> f->vsh) > (1L << 20); f->vsh++)
        ;
    if (f->vsh > 3)
        return 0;
    f->shift0 = h->shift0;
    f->nlev = h->nlev;
    f->far_c = h->far_c;
    f->skf = (int32_t)((M64(f->up[0], f->cf[0]) + M64(f->up[1], f->cf[1]) + M64(f->up[2], f->cf[2])) >> 28);
    f->skr = (int32_t)((M64(f->up[0], f->cr[0]) + M64(f->up[1], f->cr[1]) + M64(f->up[2], f->cr[2])) >> 28);
    f->sku = (int32_t)((M64(f->up[0], f->cu[0]) + M64(f->up[1], f->cu[1]) + M64(f->up[2], f->cu[2])) >> 28);
    return 1;
}

#endif

/* --- the detail (pv_pace) --- */
static const uint8_t PV_FAR g_step_px[PV_DETAILS] = { 2, 3, 4, 4, 6, 8 };
#define PV_DETAIL_WIDE 3                /* (this level on: wide) */
static struct {
    uint16_t avg;                       /* a frame's work, 16ths of a tick, weighed over some 8 frames */
    uint8_t level, hold;                /* the detail; frames before it may move again */
    uint8_t lo, hi;                     /* (the finest and coarsest since the last report: pv_pace_report) */
} PV_FAR g_pace = { 0, 1, 0, 1, 1 };

#ifndef __WATCOMC__                     /* (the host's tools': RV.EXE's frames set it, pv_pace) */
void pv_detail(int level) { g_pace.level = (uint8_t)(level < 0 ? 0 : level >= PV_DETAILS ? PV_DETAILS - 1 : level); }
int pv_detail_now(void) { return g_pace.level; }
#endif

void pv_pace(unsigned ticks)
{
    unsigned t = ticks > 30 ? 30 * 16 : ticks * 16;
    g_pace.avg = (uint16_t)(g_pace.avg - (g_pace.avg >> 3) + (t >> 3));
    if (g_pace.hold) {
        g_pace.hold--;
        return;
    }
    if (g_pace.avg > 8 * 16 + 8 && g_pace.level < PV_DETAILS - 1) {   /* (past 8 ticks, 1 / 30 s: coarser) */
        g_pace.level++;
        g_pace.hold = 15;
    } else if (g_pace.avg < 5 * 16 && g_pace.level > 0) {   /* (under 5: finer, and two seconds before it moves again) */
        g_pace.level--;
        g_pace.hold = 60;
    }
    if (g_pace.level < g_pace.lo)
        g_pace.lo = g_pace.level;
    if (g_pace.level > g_pace.hi)
        g_pace.hi = g_pace.level;
}

#ifdef __WATCOMC__
void pv_pace_report(void)
{
    RV_LOG("pview: the picture's detail %d to %d this flight, %d at its end (0 the finest, %d the coarsest)", g_pace.lo,
           g_pace.hi, g_pace.level, PV_DETAILS - 1);
    g_pace.lo = g_pace.hi = g_pace.level;
}
#endif

void pv_frame(const PvCam *cam)
{
    PvFrame PV_FAR *f = &g_f;
    int k;
#ifndef __WATCOMC__
    f->buf = g_buf;
    f->scr = g_scr;
#else
    if (!g_pv.on)
        return;
    f->buf = vid_band_buffer();
    f->scr = gfx_scratch();
    pv_put(f->buf + PV_O_CODE, PV_MARCH0);   /* the march's code in the band buffer's tail; the set-up's in the scratch */
    pv_put(f->scr + PV_O_SETCODE, PV_SET0);   /* buffer's window room (its end: the haze's and the glow's rows are
                                           fetched at its start), put back whenever the windows have had the room */
    g_pv.made = 0;
#endif
    f->o_pic = PV_O_PIC;
    f->o_st = PV_O_ST;
    f->o_ln = PV_O_LN;
    f->o_slab = PV_O_SLAB;
    f->o_win = PV_O_WIN;
    f->o_bwin = PV_O_BWIN;
    f->o_foglev = PV_O_FOGLEV;
    f->o_fogm = PV_O_FOGM;
    f->o_glow = PV_O_GLOW;
    f->o_under = PV_O_UNDER;
    f->max_steps = PV_STEPS;
    f->max_lines = PV_LINES;
    f->step_px = g_step_px[g_pace.level];
    f->wide = g_pace.level >= PV_DETAIL_WIDE;
    _fmemset(f->buf + f->o_pic, 0xFF, PV_LW * PV_LH * 2);
    f->nlines = f->nsteps = 0;
    if (!g_pv.on || !pv_setup(f, cam, &g_pv.h))
        return;
    {   /* the tables: the haze's levels, the undersides' light; the slabs' grids and cells */
        PvSlabs sl;
        uint32_t n = 0;
        pv_get(g_pv.h.slabs_at, &sl, sizeof sl);
        pv_get(g_pv.h.fog_at + (uint32_t)(sizeof(int32_t) * (3 + PV_FOG_GS)), f->scr + PV_O_FOGLEV, PV_FOG_TAUS);
        _fmemcpy(f->scr + PV_O_UNDER, sl.under, 64);
        f->nslab = sl.n;
        f->slab_shift = sl.shift;
        f->under_div = sl.under_div > 0 ? sl.under_div : 1;
        for (k = 0; k < sl.n; k++) {
            f->slab[k] = sl.s[k];
            f->slab[k].at = n;
            pv_get(sl.s[k].at, f->buf + PV_O_SLAB + n * 4, (uint32_t)sl.s[k].w * sl.s[k].h * 4);
            n += (uint32_t)sl.s[k].w * sl.s[k].h;
        }
    }
    pv_steps(f);
    pv_fog_frame(f);
    pv_glow_frame(f);
    /* a quarter at a time: its lines, each level's march of them, the plain sphere past the levels */
    for (k = 0; k < 4; k++) {
        PvQuarter q;
        uint8_t PV_FAR *thin = f->scr + PV_O_THIN;
        int l;
#ifdef __WATCOMC__
        if (k)
            pv_put(f->scr + PV_O_SETCODE, PV_SET0);
#endif
        pv_quarter(f, k, &q, thin);
        if (!q.on)
            continue;
        f->nlines = 0;
        pv_lines(f, &q, thin);
        if (f->ue > (1L << 30) / 8)
            for (l = 0; l < f->nlev; l++) {
                int sh = f->shift0 + l;
                f->lev = (int16_t)l;
                f->sh = (int16_t)sh;
                f->ox = ((f->px >> sh) - PV_W / 2 + PV_SNAP / 2) & ~(int32_t)(PV_SNAP - 1);
                f->oy = ((f->py >> sh) - PV_W / 2 + PV_SNAP / 2) & ~(int32_t)(PV_SNAP - 1);
                pv_window(f, l, f->ox, f->oy);
                pv_march(f);
            }
        pv_far(f);
        (void)l;
    }
#ifdef __WATCOMC__
    pv_put(f->scr + PV_O_SETCODE, PV_SET0);
#endif
    pv_glow(f);
#ifdef __WATCOMC__
    xms_put(g_pv.pic, 0, f->buf + f->o_pic, (unsigned long)PV_LW * PV_LH * 2);   /* (for the bands: pv_band) */
    g_pv.made = 1;
#endif
}

const uint16_t PV_FAR *pv_picture(void) { return (const uint16_t PV_FAR *)(g_f.buf + g_f.o_pic); }

#ifndef __WATCOMC__
/* the host's picture drawn on a screen (rows y0 .. y1-1 of fb, stride 640) */
void pv_blit_host(uint8_t *fb, int stride, int y0, int y1)
{
    static uint8_t work[PV_BLIT_WORK], dither[(PV_FOGS * 256 + PV_FOGS) * 3];
    PvBlit b;
    (void)stride;
    memcpy(dither, g_dither, sizeof g_dither);
    memcpy(dither + sizeof g_dither, g_sky, sizeof g_sky);
    b.band = fb + (size_t)y0 * PV_LW * PV_BLOCK;
    b.ext = 0;
    b.pic = pv_picture();
    b.dither = dither;
    b.work = work;
    b.y0 = (int16_t)y0;
    b.y1 = (int16_t)(y1 < PV_LH * PV_BLOCK ? y1 : PV_LH * PV_BLOCK);
    b.ext_y0 = b.ext_rows = 0;
    b.ly0 = 0;
    c_pv_blit(&b);
}
#endif

#ifdef PV_DEBUG
/* --- the host's checks (tools/pvshot.c) --- */
int pv_fog_dbg(int32_t dist, int32_t h) { return pv_fog_at(&g_f, dist, h); }

void pv_dbg_counts(void)
{
    printf("   counts: fills %lu (%lu px), skip calls %lu scans %lu hits %lu, overs %lu, grounds %lu, rhos %lu, samples %lu\n",
           g_cnt[0], g_cnt[1], g_cnt[2], g_cnt[3], g_cnt[4], g_cnt[5], g_cnt[6], g_cnt[8], g_cnt[11]);
    memset(g_cnt, 0, sizeof g_cnt);
    printf("   %u samples (%u seen), %u skipped, %u sub-samples; %d lines, %d steps\n", g_f.samples, g_f.seen, g_f.skips,
           g_f.subs, g_f.nlines, g_f.nsteps);
}
#endif
