/* mem.h: where the game's buffers live.  RV.EXE has no heap (the user, 2026-11-01: "make sure everything has a fixed
 * location", "sidestep malloc and free with fixed-sized arrays"; the far heap's freed memory hid from DOS, and from
 * the second flight on the bolts were not drawn): every buffer of conventional memory is an array of a fixed size, one
 * segment each, its address in RV.MAP.  build.sh refuses an RV.EXE with _fmalloc in it.
 *   MEM_ARRAY(type, name, n)   at file level: the array, with a guard behind it (no semicolon after it)
 *   MEM_TAKE(type, name, n)    the array for use, n of them at most: cleared, its guard set, and on the ledger
 *   MEM_GIVE(p)                done with it
 * The ledger (mem_ledger) counts what is out at each boundary and logs "LEAK:" for what has not come back.
 * The test builds, RVT.EXE and RVP.EXE, have 60 KB more code and no room for the arrays: RV_HEAP keeps the far heap
 * there, so the demos and the self-tests run as they did.  What the game does is tested with RV.EXE itself. */
#ifndef MEM_H
#define MEM_H

#if defined(RV_TESTS) && !defined(RV_HEAP)
#define RV_HEAP
#endif

#define MEM_GUARD_BYTES 8

#ifdef RV_HEAP
#include <malloc.h>
#define MEM_ARRAY(type, name, n)
#define MEM_TAKE(type, name, n) ((type __far *)_fmalloc((size_t)((unsigned long)(n) * sizeof(type))))
#define MEM_GIVE(p) _ffree(p)
#else
#define MEM_ARRAY(type, name, n) \
    static struct { type a[n]; unsigned char guard[MEM_GUARD_BYTES]; } __far name; \
    static const char __far name##_s[] = #name;
#define MEM_TAKE(type, name, n) \
    ((type __far *)mem_take(&name, sizeof name.a, (unsigned long)(n) * sizeof(type), name##_s))
#define MEM_GIVE(p) mem_give(p)
void __far *mem_take(void __far *p, unsigned long bytes, unsigned long want, const char __far *name);
void mem_give(void __far *p);
#endif

void mem_ledger(const char __far *when);   /* what is out, what DOS, the near heap and XMS have, the files open, the
                                              interrupt table; against the first such look: "LEAK: ..." when it differs */
void mem_flying(unsigned frame, int slots, int shots, int booms);
void mem_ledger_always(int on);            /* /ledger: a line at every look, not only when something differs */
unsigned mem_bad(void);                    /* how many "LEAK:" lines so far */
void mem_watch(const char __far *where, unsigned frame);   /* once a frame, the rooms' and the flight's: a line only when
                                              free conventional memory has fallen since the frame before */
void mem_watch_report(const char __far *when);   /* how the watch stands: its falls, what is free, the lowest seen */
void mem_leak_test(unsigned frames);       /* /leaktest N: memory taken every N frames and not given back */

#endif
