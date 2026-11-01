/* mem.c: the ledger of the fixed arrays (mem.h), and of everything else a mission might leave behind. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include <malloc.h>
#include "rv.h"

#define ROWS 48

typedef struct {
    void __far *p;
    unsigned long bytes;
    const char __far *name;
    unsigned char out;
} Row;

#ifndef RV_HEAP
static Row __far g_row[ROWS];
static int __far g_rows;
static const unsigned char __far k_guard[MEM_GUARD_BYTES] = { 0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0xF0, 0x0F };
#endif
static unsigned __far g_bad;
static int __far g_always;

/* what the first look found (and the later ones must) */
typedef struct {
    unsigned long dos_kb, xms_kb;
    unsigned near_free, ivt;
    unsigned char files, out, xms_blocks, have;
} Look;
static Look __far g_first;

static unsigned near_free(void);
unsigned mem_bad(void) { return g_bad; }
void mem_ledger_always(int on) { g_always = on; }

/* /ledger in a flight: what could pile up as it goes, every 15 s (src/flight.c); a soak's log shows a slope */
void mem_flying(unsigned frame, int slots, int shots, int booms)
{
    static const char __far s_line[] = "flying: frame %u: slots %d, shots %d, booms %d, XMS %lu KB in %u blocks, near %u, leaks %u";
    if (g_always)
        rv_logf(s_line, frame, slots, shots, booms, xms_free_kb(), xms_blocks(), near_free(), g_bad);
}

#ifndef RV_HEAP
void __far *mem_take(void __far *p, unsigned long bytes, unsigned long want, const char __far *name)
{
    static const char __far s_big[] = "LEAK: %Fs asked for %lu bytes and has %lu";
    static const char __far s_twice[] = "LEAK: %Fs taken again, not given back";
    int i;
    Row __far *r;
    for (i = 0; i < g_rows; i++)
        if (g_row[i].p == p)
            break;
    if (i == g_rows) {
        RV_CHECK(i, ROWS);
        g_rows++;
        g_row[i].p = p;
        g_row[i].bytes = bytes;
        g_row[i].name = name;
    }
    r = &g_row[i];
    if (want > bytes) {
        g_bad++;
        rv_logf(s_big, name, want, bytes);
        rv_fail(__FILE__, __LINE__, "a fixed array too small", (long)want, (long)bytes);
    }
    if (r->out) {
        g_bad++;
        rv_logf(s_twice, name);
    }
    r->out = 1;
    _fmemset(p, 0, (size_t)bytes);      /* (a clean slate: nothing of its last use) */
    _fmemcpy((unsigned char __far *)p + bytes, k_guard, MEM_GUARD_BYTES);
    return p;
}

void mem_give(void __far *p)
{
    static const char __far s_who[] = "LEAK: a buffer given back that is no array's";
    int i;
    for (i = 0; i < g_rows; i++)
        if (g_row[i].p == p) {
            g_row[i].out = 0;
            return;
        }
    g_bad++;
    rv_logf(s_who);
}

/* the guards, every array that has been used: one overwritten is an overrun, and it is named here */
static void guards(const char __far *when)
{
    static const char __far s_over[] = "LEAK: %Fs ran over its end (seen at %Fs)";
    int i;
    for (i = 0; i < g_rows; i++)
        if (_fmemcmp((unsigned char __far *)g_row[i].p + g_row[i].bytes, k_guard, MEM_GUARD_BYTES)) {
            g_bad++;
            rv_logf(s_over, g_row[i].name, when);
            _fmemcpy((unsigned char __far *)g_row[i].p + g_row[i].bytes, k_guard, MEM_GUARD_BYTES);
        }
}

static unsigned arrays_out(void)
{
    int i;
    unsigned n = 0;
    for (i = 0; i < g_rows; i++)
        n += g_row[i].out;
    return n;
}

static void arrays_list(void)
{
    static const char __far s_out[] = "  out: %Fs, %lu bytes";
    int i;
    for (i = 0; i < g_rows; i++)
        if (g_row[i].out)
            rv_logf(s_out, g_row[i].name, g_row[i].bytes);
}
#else
static void guards(const char __far *when) { (void)when; }
static unsigned arrays_out(void) { return 0; }
static void arrays_list(void) { }
#endif

#ifdef RV_HOST
static unsigned dos_paras(unsigned *largest) { *largest = 0; return 0; }
static unsigned files_open(void) { return 0; }
static unsigned ivt_hash(void) { return 0; }
static unsigned near_free(void) { return 0; }
#else
/* free in DOS below 640 KB, in paragraphs of 16 bytes, and the largest block of it (the arena chain: the largest-block
 * call would not see two blocks) */
static unsigned dos_paras(unsigned *largest)
{
    static unsigned __far first;        /* (the chain's first block never moves: asked of DOS once) */
    unsigned seg, total = 0;
    if (!first) {
        union REGS r;
        struct SREGS sr;
        segread(&sr);
        r.h.ah = 0x52;
        intdosx(&r, &r, &sr);
        first = *(unsigned short __far *)MK_FP(sr.es, r.x.bx - 2);
    }
    *largest = 0;
    seg = first;
    while (seg < 0xA000) {
        unsigned char __far *mcb = (unsigned char __far *)MK_FP(seg, 0);
        unsigned size = *(unsigned short __far *)(mcb + 3);
        if (*(unsigned short __far *)(mcb + 1) == 0) {
            total += size;
            if (size > *largest)
                *largest = size;
        }
        if (mcb[0] != 'M')
            break;
        seg += size + 1;
    }
    return total;
}

/* the files this program has open: its job file table in the PSP */
static unsigned files_open(void)
{
    extern unsigned _psp;
    unsigned char __far *jft = *(unsigned char __far * __far *)MK_FP(_psp, 0x34);
    unsigned i, n = 0, cnt = *(unsigned short __far *)MK_FP(_psp, 0x32);
    for (i = 0; i < cnt; i++)
        n += jft[i] != 0xFF;
    return n;
}

/* a stray write through a null far pointer lands in the interrupt table, and the machine stops at the next interrupt:
 * the table's hash, the vectors the game itself holds included, so taken at the same place each time */
static unsigned ivt_hash(void)
{
    const unsigned short __far *t = (const unsigned short __far *)MK_FP(0, 0);
    unsigned i, h = 0;
    for (i = 0; i < 512; i++)
        h = (h << 3 | h >> 13) ^ t[i];
    return h;
}

static unsigned near_free(void) { return (unsigned)_memavl(); }
#endif

static unsigned long dos_kb(void)
{
    unsigned largest;
    return (unsigned long)dos_paras(&largest) * 16 / 1024;
}

void mem_ledger(const char __far *when)
{
    static const char __far s_line[] =
        "ledger %Fs: arrays out %u, DOS %lu KB, near %u, XMS %lu KB in %u blocks, files %u, vectors %04x, leaks so far %u";
    static const char __far s_n[] = "LEAK: at %Fs, %Fs was %lu and is %lu";
    static const char __far s_arrays[] = "arrays out", __far s_dos[] = "DOS KB", __far s_near[] = "the near heap",
        __far s_xms[] = "XMS KB", __far s_blocks[] = "XMS blocks", __far s_files[] = "files open",
        __far s_ivt[] = "the interrupt table";
    Look l;
    unsigned before;
    guards(when);
    l.out = (unsigned char)arrays_out();
    l.dos_kb = dos_kb();
    l.near_free = near_free();
    l.xms_kb = xms_free_kb();
    l.xms_blocks = (unsigned char)xms_blocks();
    l.files = (unsigned char)files_open();
    l.ivt = ivt_hash();
    if (!g_first.have) {
        _fmemcpy(&g_first, &l, sizeof l);
        g_first.have = 1;
        rv_logf(s_line, when, l.out, l.dos_kb, l.near_free, l.xms_kb, l.xms_blocks, l.files, l.ivt, g_bad);
        arrays_list();
        return;
    }
    before = g_bad;
#define DIFF(field, label) \
    if (l.field != g_first.field) { \
        g_bad++; \
        rv_logf(s_n, when, label, (unsigned long)g_first.field, (unsigned long)l.field); \
    }
    DIFF(out, s_arrays)
    DIFF(dos_kb, s_dos)
    DIFF(xms_kb, s_xms)
    DIFF(xms_blocks, s_blocks)
    DIFF(ivt, s_ivt)
    if (l.files > g_first.files) {       /* (more is what counts: a test script ending closes one) */
        g_bad++;
        rv_logf(s_n, when, s_files, (unsigned long)g_first.files, (unsigned long)l.files);
    }
    if (l.near_free < g_first.near_free) {   /* (the near heap: less is what counts, stdio's free list settles) */
        g_bad++;
        rv_logf(s_n, when, s_near, (unsigned long)g_first.near_free, (unsigned long)l.near_free);
    }
#undef DIFF
    if (g_always || g_bad != before) {
        rv_logf(s_line, when, l.out, l.dos_kb, l.near_free, l.xms_kb, l.xms_blocks, l.files, l.ivt, g_bad);
        if (g_bad != before)
            arrays_list();
    }
    if (g_bad != before)                 /* (what differed once is reported once: the new state is the one to hold) */
        _fmemcpy(&g_first, &l, sizeof l);
}

/* stdio keeps a closed stream's record, 16 bytes of the near heap, for the next open: the most files ever open at once sets
 * how many there are.  That is settled here, at the start, so it cannot look like a leak later: the page's RVWANT.TXT, open
 * while a set is looked for, was one such record, once, in a room's frame (in DOSBox every set is on disk, and it never
 * happened).  Four: that file, the one looked for, a test script and the log. */
void mem_settle(void)
{
#ifndef RV_HOST
    static const char __far s_name[] = "RVLOG.TXT";
    char name[12];
    FILE *f[4];
    int i;
    _fstrcpy(name, s_name);
    for (i = 0; i < 4; i++)
        f[i] = fopen(name, "r");
    for (i = 0; i < 4; i++)
        if (f[i])
            fclose(f[i]);
#endif
}

/* --- the watch: free conventional memory, a frame at a time --- */
/* The rooms' loop and the flight's frame call this once a frame.  A line only when it has fallen since the frame before
 * (the user, 2026-11-01): silence means it held.  RV.EXE takes no conventional memory once it is running: the buffers are
 * fixed arrays, the ships and the rocks go to XMS, stdio gives back what it takes before the frame ends.  So there is no
 * normal operation to tell apart from a leak, and any fall at all is a "LEAK:" (measured 2026-11-01: 0 falls over every
 * mission's flight, the guns firing, waves and fields coming in, and the clicker's campaigns).  mem_bad() counts it. */
typedef struct {
    unsigned dos, big, near_;           /* free in DOS and DOS's largest block (paragraphs), free in the near heap (bytes) */
} Free;
static Free __far g_last, __far g_low, __far g_start;
static unsigned __far g_falls, __far g_leak_test;
static unsigned char __far g_watching;

/* /leaktest N: every N frames, memory taken from the near heap and from DOS and never given back (the watch's own test:
 * a watch that has never spoken proves nothing) */
void mem_leak_test(unsigned frames) { g_leak_test = frames; }
static void leak_test(void)
{
    static unsigned __far count;
#ifndef RV_HOST
    unsigned seg;
    if (!g_leak_test || ++count % g_leak_test)
        return;
    (void)malloc(64);
    (void)_dos_allocmem(4, &seg);
#else
    (void)count;
#endif
}

void mem_watch(const char __far *where, unsigned frame)
{
    static const char __far s_start[] =
        "conventional memory, the watch starts in %Fs: %lu bytes free in DOS (largest block %lu), %u in the near heap";
    static const char __far s_fell[] =
        "LEAK: conventional memory fell: %Fs frame %u: DOS %lu -> %lu bytes (largest %lu -> %lu), near heap %u -> %u";
    Free now, was;
    leak_test();
    now.dos = dos_paras(&now.big);
    now.near_ = near_free();
    _fmemcpy(&was, &g_last, sizeof was);
    _fmemcpy(&g_last, &now, sizeof now);
    if (!g_watching) {
        g_watching = 1;
        _fmemcpy(&g_start, &now, sizeof now);
        _fmemcpy(&g_low, &now, sizeof now);
        rv_logf(s_start, where, (unsigned long)now.dos * 16, (unsigned long)now.big * 16, now.near_);
        return;
    }
    if (now.dos < g_low.dos)
        g_low.dos = now.dos;
    if (now.near_ < g_low.near_)
        g_low.near_ = now.near_;
    if (now.dos < was.dos || now.near_ < was.near_) {
        g_falls++;
        g_bad++;
        rv_logf(s_fell, where, frame, (unsigned long)was.dos * 16, (unsigned long)now.dos * 16,
                (unsigned long)was.big * 16, (unsigned long)now.big * 16, was.near_, now.near_);
    }
}

void mem_watch_report(const char __far *when)
{
    static const char __far s_line[] =
        "conventional memory %Fs: %u falls; DOS %lu bytes free, started at %lu, lowest %lu; near heap %u, started at %u, lowest %u";
    rv_logf(s_line, when, g_falls, (unsigned long)g_last.dos * 16, (unsigned long)g_start.dos * 16,
            (unsigned long)g_low.dos * 16, g_last.near_, g_start.near_, g_low.near_);
}
