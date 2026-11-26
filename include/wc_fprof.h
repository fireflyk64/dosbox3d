/*
 *  wc_fprof.h: where a frame's host time went (a test aid).
 *
 *  Off unless WCPRIVSLOW=<ms> is set (natively, or on the page as ?env.WCPRIVSLOW=47).  The main loop's parts are
 *  timed on the host's clock into buckets; Privateer's frame hook (wcnet_priv.cpp) takes the buckets' deltas at each
 *  flight frame, with the software interrupts the guest made in it (by number, AH and caller) and the guest's code
 *  sampled every 1000 instructions, and logs every frame longer than the threshold (wall clock).
 *
 *  Defined in src/dosbox.cpp (the main object: every library can reach it).
 */
#ifndef WC_FPROF_H_
#define WC_FPROF_H_

#include "dosbox.h"

enum {
    FP_CORE,        // the CPU core running guest code (Normal_Loop's decoder; in the browser the core hooks too)
    FP_CALLBACK,    // callback handlers: DOS/BIOS/EMS services, the layer's main-loop hooks (frame hook, exchange)
    FP_EVENTS,      // PIC_RunQueue: timed events (VGA lines and retrace with the picture's present, sound DMA, mixer)
    FP_TICKS,       // TIMER_AddTick (the ms tick: PIT, mixer) and GFX_Events (input)
    FP_YIELD,       // emscripten_sleep(0) every 10 ms: the browser's turn (its tasks, painting, audio, network)
    FP_IDLE,        // emscripten_sleep(1) / SDL_Delay(1): ahead of real time, nothing to do
    FP_VSYNC,       // (in FP_EVENTS) Privateer's 640 x 400 retrace: compose, render, present
    FP_PRESENT,     // (in FP_EVENTS) GFX_EndUpdate: the picture to the window/canvas
    FP_COREHOOK,    // (in FP_CORE) Privateer's core hooks: hi-res drawing calls and native twins
    FP_NET,         // (in FP_CALLBACK) real time blocked on the network (perf_wait)
    FP_FRAMEHOOK,   // (in FP_CALLBACK) the layer's work at the top of a flight frame (Privateer's controls entry), this log too
    FP_N
};

extern bool wc_fprof_on;
extern double wc_fprof_ms[FP_N];
extern unsigned wc_fprof_count[FP_N];
extern double wc_fprof_max[FP_N];
double wc_fprof_now();
// The longest single callback of the frame so far: its number and where the guest was (CS:IP) as it was called.
extern double wc_fprof_cb_max;
extern Bitu wc_fprof_cb_max_id;
extern Bit32u wc_fprof_cb_max_at;
// The core hooks: counted by place (WCPRIVSLOW_HOOKS=1, the busiest logged now and then), timed only with
// WCPRIVSLOW_HOOKS=2 (two clock reads a call: some thousands of calls a frame).
extern int wc_fprof_hooks;
// DOSBox's TLB (src/cpu/paging.cpp): its clears (each mov to CR3) and the pages looked up again after them
extern Bit32u wc_tlb_clears, wc_tlb_inits;
extern void (*wc_tlb_clear_probe)();       // (each clear, with WCPRIVSLOW_HOOKS: counted by the guest's CS:IP)
void wc_fprof_hook(Bit32u ip, bool handled);
// A software interrupt (CPU_Interrupt with CPU_INT_SOFTWARE), when the probe is set.
extern void (*wc_fprof_int)(Bitu num, Bitu oldeip);

// The main loop's slices (some thousands a frame: the core leaves for the layer's periodic call every 1000
// instructions) are timed one in 16 and counted 16 times: a clock read from the browser's wasm is about a microsecond.
extern unsigned wc_fprof_slice;
struct WcFprofSampled {
    int bucket;
    double start;
    explicit WcFprofSampled(int b) : bucket(b), start(wc_fprof_on && (++wc_fprof_slice & 15) == 0 ? wc_fprof_now() : -1.0) {}
    ~WcFprofSampled() {
        if (start < 0) return;
        wc_fprof_ms[bucket] += 16 * (wc_fprof_now() - start);
        wc_fprof_count[bucket] += 16;
    }
};

struct WcFprof {
    int bucket;
    double start;
    explicit WcFprof(int b) : bucket(b), start(wc_fprof_on && b >= 0 ? wc_fprof_now() : -1.0) {}
    ~WcFprof() {
        if (start < 0) return;
        const double d = wc_fprof_now() - start;
        wc_fprof_ms[bucket] += d;
        wc_fprof_count[bucket]++;
        if (d > wc_fprof_max[bucket]) wc_fprof_max[bucket] = d;
    }
};

#endif
