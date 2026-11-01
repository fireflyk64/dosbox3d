/* gx.c: the game's side of RVGX, the sprite accelerator: gx.h, docs/gx.md.
 *
 * Everything the card is told is in one block at a fixed place, g_block, which it is told of once; each call is then
 * one interrupt with the function in AX.  So the install check is the one call a game without the card ever makes. */
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <string.h>
#include "gx.h"

#define GX_AH 0xE7
#define GX_INSTALLED 0xE700
#define GX_ATTACH 0xE701
#define GX_LIST 0xE710
#define GX_STATUS 0xE711
#define GX_DRAW 0xE720
#define GX_SIG_IN_BX 0x5256             /* 'RV' */
#define GX_SIG_IN_CX 0x4758             /* 'GX' */
#define GX_SIG_OUT_BX 0x7867            /* 'gx' */
#define GX_MAJOR 1

/* the result word: nothing drawn, for this reason */
#define GX_E_NO_ANSWER (-100)           /* (what the block holds until the card answers: a silent card is no card) */
#define GX_E_INDEX (-1)                 /* no such bundle in the list */
#define GX_E_NOT_FOUND (-2)             /* it is not to be had */
#define GX_E_NOT_YET (-3)               /* it has not come yet */
#define GX_E_VIEW (-4)                  /* no such view in it */
#define GX_E_TARGET (-5)                /* the frame buffer is not memory the card can write */

#define GX_LIST_BYTES 512
#define GX_TYPES 64

#pragma pack(1)
typedef struct {
    short result;
    unsigned short bundle, view, flags;
    short x, y, scale, roll;
    unsigned char __far *fb;            /* the frame buffer: the first pixel of screen row fb_y0 */
    unsigned short stride;
    short fb_y0, y0, y1, xa, xb;
    const short __far *ext;
    short ext_y0, ext_rows;
    unsigned long list_addr;            /* the names, in XMS: its linear address */
    unsigned short list_len, pad;
    unsigned long stat_draws, stat_px, stat_us;
    unsigned short stat_ready, stat_waiting;
} GxBlock;
#pragma pack()
typedef char gx_block_is_64_bytes[sizeof(GxBlock) == 64 ? 1 : -1];

static GxBlock __far g_block;
static int g_present, g_setting = 1;    /* (high detail is on by default) */
static XmsBlock g_list;
static unsigned char __far g_bundle_of[256];   /* a ship type's place in the list, 0xFF: not in it */
static unsigned char __far g_types[GX_TYPES];
static int g_ntypes, g_sent;
static unsigned long __far g_fell[6];   /* draws refused, by reason: none listed, then -1 .. -5 */
static unsigned long __far g_drawn;

/* (the card's interrupt: it leaves every register alone but these calls' AX) */
void gx_int(unsigned short ax);
#pragma aux gx_int = "int 2Fh" parm [ax] modify [ax bx cx dx si di es];

int gx_present(void) { return g_present; }
int gx_on(void) { return g_present && g_setting; }

int gx_init(void)
{
    union REGS r;
    struct SREGS s;
    r.x.ax = GX_INSTALLED;
    r.x.bx = GX_SIG_IN_BX;
    r.x.cx = GX_SIG_IN_CX;
    r.x.dx = 0;
    int86(0x2f, &r, &r);
    /* AL FFh and the signature back: DOS's own answer to a function nobody has is AL as it was, 0; some other program
     * using these numbers would leave the signature alone */
    if (r.h.al != 0xFF || r.x.bx != GX_SIG_OUT_BX || r.h.ch != GX_MAJOR) {
        RV_LOG("gx: no sprite accelerator: the game draws its ships itself");
        return 0;
    }
    RV_LOG("gx: the sprite accelerator answers, version %u.%u", (unsigned)r.h.ch, (unsigned)r.h.cl);
    segread(&s);
    s.es = FP_SEG((GxBlock __far *)&g_block);
    r.x.bx = FP_OFF((GxBlock __far *)&g_block);
    r.x.ax = GX_ATTACH;
    int86x(0x2f, &r, &r, &s);
    if (r.x.ax != 0)
        return 0;
    g_list = xms_alloc(GX_LIST_BYTES, XMS_RESIDENT);
    if (!g_list)
        return 0;
    _fmemset(g_bundle_of, 0xFF, sizeof g_bundle_of);
    g_present = 1;
    return 1;
}

/* the list to the card: the names into XMS, the block taken to its linear address for the call (locked only that
 * long: the card reads it within the interrupt) */
static void send_list(void)
{
    static char __far names[GX_LIST_BYTES];
    RvRegs x;
    unsigned handle, n = 0;
    unsigned long off;
    int i;
    _fmemset(g_bundle_of, 0xFF, sizeof g_bundle_of);
    for (i = 0; i < g_ntypes; i++) {
        char name[16];
        int len = sprintf(name, "SHIP%02X.RV", g_types[i]);
        _fmemcpy(names + n, (char __far *)name, len + 1);
        n += len + 1;
        g_bundle_of[g_types[i]] = (unsigned char)i;
    }
    names[n++] = 0;
    xms_put(g_list, 0, names, n);
    xms_where(g_list, &handle, &off);
    x.eax = 0x0C00; x.edx = handle; x.ebx = x.ecx = 0;
    rv_xms(&x);
    if ((x.eax & 0xFFFF) != 1) {
        RV_LOG("gx: XMS would not lock the list (error %02lx)", x.ebx & 0xFF);
        return;
    }
    g_block.list_addr = (((x.edx & 0xFFFF) << 16) | (x.ebx & 0xFFFF)) + off;
    g_block.list_len = n;
    g_block.result = GX_E_NO_ANSWER;
    gx_int(GX_LIST);
    x.eax = 0x0D00; x.edx = handle; x.ebx = x.ecx = 0;
    rv_xms(&x);
    g_sent = 1;
    RV_LOG("gx: %d bundles asked of the card", (int)g_block.result);
}

void gx_want(const unsigned char *types, int n)
{
    int i, k, m = 0, same;
    unsigned char list[GX_TYPES];
    if (!g_present)
        return;
    for (i = 0; i < n; i++) {
        for (k = 0; k < m && list[k] != types[i]; k++)
            ;
        if (k == m && m < GX_TYPES)
            list[m++] = types[i];
    }
    same = g_sent && m == g_ntypes;
    for (i = 0; i < m; i++) {
        same = same && g_types[i] == list[i];
        g_types[i] = list[i];
    }
    g_ntypes = m;
    if (same)
        return;                         /* (the briefing's list again at the launch) */
    g_sent = 0;
    if (g_setting)
        send_list();
}

void gx_set(int on)
{
    g_setting = on != 0;
    if (g_present && g_setting && !g_sent && g_ntypes)
        send_list();
}

void gx_toggle(void)
{
    if (!g_present)
        return;
    gx_set(!g_setting);
    RV_NOTE("gx: high detail %s", g_setting ? "on" : "off");
}

int gx_draw(unsigned type, int view, int mirror, int x, int y, int scale, int roll, int y0, int y1, int xa, int xb,
            const short __far *ext, int ext_y0, int ext_rows)
{
    GxBlock __far *b = &g_block;
    int k = g_bundle_of[type & 0xFF], res;
    if (k == 0xFF) {
        g_fell[0]++;
        return GX_E_INDEX;
    }
    if (y0 >= y1)
        return 0;
    b->bundle = (unsigned short)k;
    b->view = (unsigned short)view;
    b->flags = (unsigned short)(mirror ? 1 : 0);
    b->x = (short)x;
    b->y = (short)y;
    b->scale = (short)scale;
    b->roll = (short)roll;
    b->fb = vid_row(y0);
    b->stride = SCR_W;
    b->fb_y0 = (short)y0;
    b->y0 = (short)y0;
    b->y1 = (short)y1;
    b->xa = (short)xa;
    b->xb = (short)xb;
    b->ext = ext;
    b->ext_y0 = (short)ext_y0;
    b->ext_rows = (short)ext_rows;
    b->result = GX_E_NO_ANSWER;
    gx_int(GX_DRAW);
    res = b->result;
    if (res < 0)
        g_fell[res >= -5 ? -res : 1]++;
    else
        g_drawn++;
    return res;
}

void gx_log(void)
{
    static const char __far s_f[] =
        "gx: this flight %lu ships by the card, %lu fell back: %lu not there yet, %lu not to be had, %lu not listed, %lu else";
    static const char __far s_c[] =
        "gx: the card since the start: %lu draws, %lu KB of pixels in %lu ms of the host's; %u bundles in, %u on the way";
    if (!g_present)
        return;
    rv_logf(s_f, g_drawn, g_fell[0] + g_fell[1] + g_fell[2] + g_fell[3] + g_fell[4] + g_fell[5], g_fell[3], g_fell[2],
            g_fell[0], g_fell[1] + g_fell[4] + g_fell[5]);
    g_block.result = GX_E_NO_ANSWER;
    gx_int(GX_STATUS);
    if (g_block.result >= 0)
        rv_logf(s_c, g_block.stat_draws, g_block.stat_px / 1024, g_block.stat_us / 1000, g_block.stat_ready, g_block.stat_waiting);
    g_drawn = 0;
    _fmemset(g_fell, 0, sizeof g_fell);
}
