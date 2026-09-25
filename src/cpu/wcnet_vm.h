/*
 *  Running Wing Commander code from inside the emulator.
 *
 *  Two mechanisms:
 *
 *  1. Interception.  wc_net_check_cpu_hooks() runs before every instruction.
 *     When CS:IP is the entry of a game function we care about, the stack
 *     holds [ret_ip, ret_seg, arg0, arg1, ...] (Borland far cdecl) and we
 *     can read the arguments, let the call proceed, or `return_from_call()`
 *     which emulates `retf` with a chosen AX so the caller never notices.
 *
 *  2. The trampoline.  To *invoke* game functions (replaying a remote event)
 *     we write a tiny far-call thunk ("shellcode") into an unused string in
 *     the data segment and point CS:IP at it.  The trampoline is a queue of
 *     such jobs: a 6-byte stub `push bp / mov bp,sp / NOP / pop bp / retf`
 *     whose NOP is a hook point.  Each bounce through the NOP starts the next
 *     job (which returns to the NOP when the game function is done), and when
 *     the queue is empty the stub's retf returns to wherever the trampoline
 *     was entered.  Jobs may inspect the game state after their call
 *     (e.g. to learn which slot a spawn used).
 */
#ifndef WCNET_VM_H_
#define WCNET_VM_H_

#include <deque>
#include <vector>
#include "dosbox.h"
#include "wcnet_code.h"

namespace wc {

// --- interception helpers ---------------------------------------------------

// Segment currently stored in an overlay stub's `jmp far` thunk, or 0 when the
// overlay is not loaded (the thunk still reads `int 3F`).
Bit16u overlay_segment(Bit16u stubSeg, Bit16u stubOff);

// True when CS:IP is exactly the entry of `fn`.
bool at_function(const code::OverlayFn &fn);
// True when CS:IP is the given location inside an overlay.
bool at_location(const code::OverlayLoc &loc);
bool at_location(const code::RootLoc &loc);

// Read the n-th 16-bit argument of the function whose entry we are at.
Bit16u call_arg16(int n);
// Emulate `retf` at a function entry, returning `ax` to the caller.  The
// caller cleans up the arguments (cdecl), so nothing else is needed.
void return_from_call(Bit16u ax);

// --- invoking game code -----------------------------------------------------

// Builds a far call with 16-bit arguments pushed right-to-left.  invoke()
// pushes the current CS:IP as the return address and jumps into the thunk, so
// the caller must be at a point where "continue here afterwards" makes sense
// (in practice: the trampoline NOP).
class GameCall {
public:
    GameCall(Bit16u seg, Bit16u off) : seg_(seg), off_(off) {}
    GameCall &arg(Bit16u v) { args_.push_back(v); return *this; }
    void invoke() const;

private:
    Bit16u seg_, off_;
    std::vector<Bit16u> args_;
};

// A unit of work executed by the trampoline.
class VmJob {
public:
    virtual ~VmJob() {}
    // Start the job.  Return true when a game call was started (the trampoline
    // will call finish() when it returns), false when the job completed
    // synchronously (finish() is called immediately).
    virtual bool start() = 0;
    virtual void finish() {}
    virtual const char *describe() const { return "job"; }
};

class Trampoline {
public:
    Trampoline();

    // Queue a job; ownership is taken.
    void enqueue(VmJob *job);
    bool has_pending() const { return !jobs_.empty(); }
    bool is_running() const { return running_; }

    // Start executing queued jobs *instead of* the game function whose entry
    // we are at: the trampoline's final retf returns to that function's
    // caller, so the intercepted call itself never runs.
    void run_instead_of_current_call();
    // Start executing queued jobs and then resume at the current CS:IP.
    void run_before_current_instruction();

    // Called by the hook dispatcher when CS:IP is the trampoline's NOP.
    void on_bounce();

    // Invoked once when the queue drains (after the last job finished).
    typedef void (*IdleFn)();
    void set_idle_callback(IdleFn fn) { idle_ = fn; }

    // The NOP hook point.
    static bool at_hook();

private:
    void jump_to_stub();
    void start_next();

    std::deque<VmJob *> jobs_;
    VmJob *current_;
    bool running_;
    IdleFn idle_;
};

extern Trampoline g_trampoline;

}  // namespace wc

#endif
