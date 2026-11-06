/* util.c: the log (RVLOG.TXT), the bounds-failure stop, upper memory. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "rv.h"

static int g_log_started;
unsigned char __far g_rv_verbose;
unsigned char rv_filebuf[1024];

/* a line in RVLOG.TXT; the file is closed after each (DOSBox writes a file
 * out to its host only on a close: the browser page reads it while the game
 * runs) */
void rv_log(const char *fmt, ...)
{
    va_list ap;
    FILE *f = fopen("RVLOG.TXT", g_log_started ? "a" : "w");
    if (!f)
        return;
    g_log_started = 1;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* The log's formats in XMS (tools/logfmt.py): the DOS build is compiled from copies of its sources with each format
 * the log alone reads a token, "\001" and its number, and their texts a table at the end of the EXE, past its image.
 * logfmt_load puts it in XMS at the start; vlogf puts a token's text back.  A token before then, or with no table, is
 * logged as "[log line N]" */
static XmsBlock g_logfmt;
static unsigned g_logfmt_n;
static unsigned long g_logfmt_len;
static const char *g_logexe;            /* the EXE's path: the formats read from its end before XMS is up */

void logfmt_exe(const char *exe)
{
    g_logexe = exe;
}

/* (before the table is in XMS: a format read from the EXE's end, the file opened for the one line) */
static int fmt_from_exe(unsigned id, char *buf, unsigned n)
{
    unsigned char tail[12];
    unsigned long len, off;
    unsigned got = 0;
    FILE *f = g_logexe ? fopen(g_logexe, "rb") : 0;
    if (!f)
        return 0;
    if (fseek(f, -12L, SEEK_END) == 0 && fread(tail, 1, 12, f) == 12 && !memcmp(tail + 4, "RVLOGEND", 8)
        && (len = *(unsigned long *)tail) > 10 + 4UL * id
        && fseek(f, -12L - (long)len + 10 + 4L * id, SEEK_END) == 0 && fread(&off, 1, 4, f) == 4 && off < len
        && fseek(f, -12L - (long)len + (long)off, SEEK_END) == 0)
        got = (unsigned)fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = 0;
    return got != 0;
}

void logfmt_load(const char *exe)
{
    unsigned char tail[12];
    unsigned long k;
    FILE *f = fopen(exe, "rb");
    g_logexe = exe;
    if (!f)
        return;
    if (fseek(f, -12L, SEEK_END) == 0 && fread(tail, 1, 12, f) == 12 && !memcmp(tail + 4, "RVLOGEND", 8)
        && (g_logfmt_len = *(unsigned long *)tail) > 10 && fseek(f, -12L - (long)g_logfmt_len, SEEK_END) == 0
        && (g_logfmt = xms_alloc(g_logfmt_len + 16, XMS_RESIDENT)) != 0) {
        for (k = 0; k < g_logfmt_len; k += sizeof rv_filebuf) {
            unsigned m = (unsigned)(g_logfmt_len - k < sizeof rv_filebuf ? g_logfmt_len - k : sizeof rv_filebuf);
            if (fread(rv_filebuf, 1, m, f) != m)
                break;
            xms_put(g_logfmt, k, rv_filebuf, m);
        }
        xms_get(g_logfmt, 0, tail, 10);
        if (!memcmp(tail, "RVLOGFMT", 8))
            g_logfmt_n = tail[8] | (unsigned)tail[9] << 8;
    }
    fclose(f);
}

/* a format into buf (n bytes): its own text, or a token's from the table (the play tests' messages too: RVP.EXE's
 * table has them after the log's, tools/logfmt.py) */
void logfmt_text(const char __far *fmt, char *buf, unsigned n)
{
    if (fmt[0] == '\001') {
        const char __far *p = fmt + 1;
        unsigned id = 0;
        unsigned long off, most;
        while (*p >= '0' && *p <= '9')
            id = id * 10 + (unsigned)(*p++ - '0');
        if (id < g_logfmt_n && xms_ready() && xms_get(g_logfmt, 10 + 4UL * id, &off, 4) && off < g_logfmt_len) {
            most = g_logfmt_len - off < n - 1 ? g_logfmt_len - off : n - 1;
            xms_get(g_logfmt, off, buf, most);
            buf[most] = 0;
            return;
        }
        if (fmt_from_exe(id, buf, n))   /* (XMS not up yet, or given back at the end) */
            return;
        sprintf(buf, "[log line %u]", id);
        return;
    }
    _fstrncpy(buf, fmt, n - 1);
    buf[n - 1] = 0;
}

/* the same with its format far (out of DGROUP, which is nearly full: `static const char __far` formats), copied
 * to the stack for the library */
static void vlogf(const char __far *fmt, va_list ap)
{
    char buf[400];
    FILE *f;
    logfmt_text(fmt, buf, sizeof buf);
    f = fopen("RVLOG.TXT", g_log_started ? "a" : "w");
    if (!f)
        return;
    g_log_started = 1;
    vfprintf(f, buf, ap);
    fputc('\n', f);
    fclose(f);
}

void rv_logf(const char __far *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlogf(fmt, ap);
    va_end(ap);
}

/* the same, for a line about nothing wrong in a frame: only with /verbose (rv.h) */
void rv_notef(const char __far *fmt, ...)
{
    va_list ap;
    if (!g_rv_verbose)
        return;
    va_start(ap, fmt);
    vlogf(fmt, ap);
    va_end(ap);
}

void (*rv_fail_hook)(void);   /* the video module restores text mode here */

void rv_fail(const char *file, int line, const char *what, long value, long limit)
{
    if (rv_fail_hook)
        rv_fail_hook();
    RV_LOG("FAIL %s:%d: %s (value %ld, limit %ld)", file, line, what, value, limit);
    printf("RetroVega stopped: %s:%d: %s (value %ld, limit %ld)\n", file, line, what, value, limit);
    exit(3);
}

/* a block of upper memory (a UMB, above 640 KB) from DOS, or 0 when there is
 * none: DOSBox has 128 KB of them at D000 with EMS off (64 KB with it on).
 * The allocation strategy and the UMBs' link are set for the call and put
 * back; DOS frees the block when the program ends. */
void __far *rv_alloc_high(unsigned long bytes)
{
    union REGS r;
    unsigned strat, link, seg = 0;
    if (bytes > 0xFFF0UL)
        return 0;
    r.x.ax = 0x5800;
    int86(0x21, &r, &r);
    strat = r.x.ax;
    r.x.ax = 0x5802;
    int86(0x21, &r, &r);
    link = r.h.al;
    r.x.ax = 0x5803;                    /* the UMBs linked to the chain */
    r.x.bx = 1;
    int86(0x21, &r, &r);
    r.x.ax = 0x5801;                    /* high memory only, first fit */
    r.x.bx = 0x40;
    int86(0x21, &r, &r);
    r.h.ah = 0x48;
    r.x.bx = (unsigned)((bytes + 15) >> 4);
    int86(0x21, &r, &r);
    if (!r.x.cflag)
        seg = r.x.ax;
    r.x.ax = 0x5801;
    r.x.bx = strat;
    int86(0x21, &r, &r);
    r.x.ax = 0x5803;
    r.x.bx = link;
    int86(0x21, &r, &r);
    return seg ? MK_FP(seg, 0) : 0;
}

/* The stack's deepest use.  The program has no stack checks (-s) and the stack (4 KB, build.sh) sits on the BSS: an
 * overflow would write over the last variables unseen.  Painted at the start, the paint left untouched counted when
 * logged (the flight's end, log_free): 1.7 KB was the deepest seen (2026-10-31, a fight to the death screen). */
#ifndef RV_HOST
extern unsigned _STACKLOW, _STACKTOP;   /* (Watcom's startup's) */
#define STACK_PAINT 0xA5

void rv_stack_paint(void)
{
    unsigned char *p = (unsigned char *)_STACKLOW, *e = (unsigned char *)&p - 128;   /* (to below this frame) */
    while (p < e)
        *p++ = STACK_PAINT;
}

void rv_stack_log(const char __far *when)
{
    static const char __far s_used[] = "stack %Fs: %u of its %u bytes used at the deepest";
    const unsigned char *p = (const unsigned char *)_STACKLOW;
    while (p < (const unsigned char *)_STACKTOP && *p == STACK_PAINT)
        p++;
    rv_logf(s_used, when, _STACKTOP - (unsigned)p, _STACKTOP - _STACKLOW);
}
#else
void rv_stack_paint(void) {}
void rv_stack_log(const char __far *when) { (void)when; }
#endif
