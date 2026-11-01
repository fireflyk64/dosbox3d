/* mem.c: the ledger of the fixed arrays (mem.h), and of everything else a mission might leave behind. */
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
static unsigned long dos_kb(void) { return 0; }
static unsigned files_open(void) { return 0; }
static unsigned ivt_hash(void) { return 0; }
static unsigned near_free(void) { return 0; }
#else
/* free in DOS below 640 KB, KB (the arena chain: the largest-block call would not see two blocks) */
static unsigned long dos_kb(void)
{
    union REGS r;
    struct SREGS sr;
    unsigned seg;
    unsigned long total = 0;
    segread(&sr);
    r.h.ah = 0x52;
    intdosx(&r, &r, &sr);
    seg = *(unsigned short __far *)MK_FP(sr.es, r.x.bx - 2);
    while (seg < 0xA000) {
        unsigned char __far *mcb = (unsigned char __far *)MK_FP(seg, 0);
        unsigned size = *(unsigned short __far *)(mcb + 3);
        if (*(unsigned short __far *)(mcb + 1) == 0)
            total += size;
        if (mcb[0] != 'M')
            break;
        seg += size + 1;
    }
    return total * 16 / 1024;
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
    DIFF(files, s_files)
    DIFF(ivt, s_ivt)
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
