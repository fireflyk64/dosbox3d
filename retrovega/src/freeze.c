/* freeze.c: where a frozen program is (freeze.h; the user, 2026-11-01: "We need to aggressively track potential freezes...
 * Can we get a stack trace").  The timer interrupt keeps running when the main loop is stuck, so it can look: the last
 * second's interrupted addresses (one stuck place, or a loop's few) and, at the moment it calls it a freeze, the registers
 * and the stack.  The return addresses are in the stack as they are, tests/freezemap.py picks them out.
 * What it cannot see: a loop with interrupts off, or the music's interrupt itself stuck.  tests/freezetrace.sh takes
 * those from outside, through DOSBox, with gdb.  It writes through DOS directly: the C runtime may be what is stuck. */
#include <dos.h>
#include <i86.h>
#include <string.h>
#include "rv.h"
#include "freeze.h"

volatile unsigned __far g_freeze_ticks;
volatile unsigned char __far g_freeze_hold;

#ifdef RV_HOST
void freeze_start(void) { }
void freeze_stop(void) { }
void freeze_tick(unsigned cs, unsigned ip, const void __far *frame) { (void)cs; (void)ip; (void)frame; }
#else

#define LIMIT (240U * 20)               /* 20 s */
#define AGAIN (240U * 60)               /* a freeze that goes on: told again a minute later */
#define SAMPLES 32
#define STACK_WORDS 64

static unsigned char __far g_on, __far g_busy, __far g_due, __far g_nsample;
static unsigned __far g_since;          /* ticks since the last report, or since the watch began */
static unsigned __far g_sample[SAMPLES][2];
static unsigned __far g_regs[14];
static unsigned __far g_stack[STACK_WORDS];
static unsigned __far g_stack_ss, __far g_stack_sp;
static unsigned char __far *__far g_indos;
static unsigned __far g_reports;
static char __far g_out[200];

void freeze_start(void)
{
    union REGS r;
    struct SREGS s;
    segread(&s);
    r.h.ah = 0x34;
    intdosx(&r, &r, &s);
    g_indos = (unsigned char __far *)MK_FP(s.es, r.x.bx);
    g_freeze_ticks = 0;
    g_on = 1;
}

void freeze_stop(void) { g_on = 0; }

static char __far *hex4(char __far *p, unsigned v)
{
    static const char __far d[] = "0123456789abcdef";
    int i;
    for (i = 12; i >= 0; i -= 4)
        *p++ = d[(v >> i) & 15];
    return p;
}

static char __far *text(char __far *p, const char __far *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

static void write_line(int fd, char __far *end)
{
    union REGS r;
    struct SREGS s;
    *end++ = '\r';
    *end++ = '\n';
    segread(&s);
    s.ds = FP_SEG(g_out);
    r.h.ah = 0x40;
    r.x.bx = fd;
    r.x.cx = (unsigned)(end - g_out);
    r.x.dx = FP_OFF(g_out);
    intdosx(&r, &r, &s);
}

static void report(void)
{
    static const char __far s_name[] = "RVLOG.TXT";
    static const char __far s_head[] = "FREEZE: the main loop has not run for this many seconds (hex): ";
    static const char __far s_load[] = "freeze: program at ";
    static const char __far s_at[] = "freeze: cs:ip ax bx cx dx si di bp ds es fl ss:sp ";
    static const char __far s_samples[] = "freeze: the last second ";
    static const char __far s_stack[] = "freeze: stack ";
    union REGS r;
    struct SREGS s;
    int fd, i, k;
    char __far *p;
    segread(&s);
    s.ds = FP_SEG(s_name);
    r.x.ax = 0x3D02;                    /* open for writing, the end found; closed again after */
    r.x.dx = FP_OFF(s_name);
    intdosx(&r, &r, &s);
    if (r.x.cflag)
        return;
    fd = r.x.ax;
    r.x.ax = 0x4202;
    r.x.bx = fd;
    r.x.cx = r.x.dx = 0;
    intdos(&r, &r);
    p = text(g_out, s_head);
    p = hex4(p, g_since / 240);
    write_line(fd, p);
    {
        extern unsigned _psp;
        p = text(g_out, s_load);
        p = hex4(p, _psp + 0x10);
        write_line(fd, p);
    }
    p = text(g_out, s_at);
    for (i = 0; i < 12; i++) {
        p = hex4(p, g_regs[i]);
        *p++ = i == 0 ? ':' : ' ';
    }
    p = hex4(p, g_stack_ss);
    *p++ = ':';
    p = hex4(p, g_stack_sp);
    write_line(fd, p);
    for (k = 0; k < SAMPLES; k += 12) {
        p = text(g_out, s_samples);
        for (i = k; i < k + 12 && i < SAMPLES; i++) {
            p = hex4(p, g_sample[i][0]);
            *p++ = ':';
            p = hex4(p, g_sample[i][1]);
            *p++ = ' ';
        }
        write_line(fd, p);
    }
    for (k = 0; k < STACK_WORDS; k += 16) {
        p = text(g_out, s_stack);
        for (i = k; i < k + 16; i++) {
            p = hex4(p, g_stack[i]);
            *p++ = ' ';
        }
        write_line(fd, p);
    }
    r.h.ah = 0x3E;
    r.x.bx = fd;
    intdos(&r, &r);
}

void freeze_tick(unsigned cs, unsigned ip, const void __far *frame)
{
    const unsigned __far *f = (const unsigned __far *)frame;
    int i;
    if (!g_on || g_freeze_hold)
        return;
    g_since = ++g_freeze_ticks;
    if (g_freeze_ticks < LIMIT - 240)
        return;
    if (g_freeze_ticks <= LIMIT) {
        if (!(g_freeze_ticks & 7)) {    /* (the last second, every 8th tick: 30 places) */
            unsigned n = g_nsample;
            g_sample[n % SAMPLES][0] = cs;
            g_sample[n % SAMPLES][1] = ip;
            g_nsample = (unsigned char)(n + 1);
        }
        if (g_freeze_ticks == LIMIT) {  /* the INTPACK: gs fs es ds di si bp sp bx dx cx ax ip cs flags */
            g_regs[0] = cs;
            g_regs[1] = ip;
            g_regs[2] = f[11];
            g_regs[3] = f[8];
            g_regs[4] = f[10];
            g_regs[5] = f[9];
            g_regs[6] = f[5];
            g_regs[7] = f[4];
            g_regs[8] = f[6];
            g_regs[9] = f[3];
            g_regs[10] = f[2];
            g_regs[11] = f[14];
            g_stack_ss = FP_SEG(frame);
            g_stack_sp = FP_OFF(frame) + 15 * 2;
            for (i = 0; i < STACK_WORDS; i++)
                g_stack[i] = f[15 + i];
            g_due = 1;
        }
    }
    if (g_due && !g_busy && !(g_indos && *g_indos)) {
        g_busy = 1;
        g_due = 0;
        g_reports++;
        _enable();                      /* (the music goes on while the file is written: its nested tick stops at g_busy) */
        report();
        _disable();
        g_busy = 0;
    }
    if (g_freeze_ticks >= LIMIT + AGAIN)
        g_freeze_ticks = LIMIT - 240 - 1;   /* (and again, in a minute, if it has not moved) */
}
#endif
