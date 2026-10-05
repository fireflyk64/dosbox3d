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

// True when CS:IP is exactly that place (a function's entry, or an
// instruction), in an overlay or in the root image.  Never for a place the
// running game does not have.
bool at(const Loc &loc);
inline bool at_function(const Loc &fn) { return at(fn); }
inline bool at_location(const Loc &loc) { return at(loc); }

// Read the n-th 16-bit argument (as the function declares them) of the
// function whose entry we are at.  Without a function: cdecl, which is every
// WC.EXE function; with one, its own convention (WC2.EXE has pascal ones,
// whose first argument is the deepest on the stack).
Bit16u call_arg16(int n);
Bit16u call_arg16(const Loc &fn, int n);
// Replace the n-th argument before the function reads it.
void set_call_arg16(int n, Bit16u value);
void set_call_arg16(const Loc &fn, int n, Bit16u value);
// Emulate the function's return at its entry, returning `ax` to the caller.
// A cdecl caller cleans up the arguments itself; for a pascal function its
// arguments are popped here.
void return_from_call(Bit16u ax);
void return_from_call(const Loc &fn, Bit16u ax);

// --- invoking game code -----------------------------------------------------

// Builds a far call with 16-bit arguments pushed right-to-left.  invoke()
// pushes the current CS:IP as the return address and jumps into the thunk, so
// the caller must be at a point where "continue here afterwards" makes sense
// (in practice: the trampoline NOP).
class GameCall {
public:
    // A known function, with its convention (an overlay function is called
    // through its thunk).
    explicit GameCall(const Loc &fn)
        : seg_(fn.seg), off_(fn.overlay ? fn.stubOff : fn.off), pascal_(fn.pascal != 0), known_(fn.known()) {}
    GameCall(Bit16u seg, Bit16u off) : seg_(seg), off_(off), pascal_(false), known_(true) {}
    // Arguments in the order the function declares them.
    GameCall &arg(Bit16u v) { args_.push_back(v); return *this; }
    void invoke() const;

private:
    Bit16u seg_, off_;
    bool pascal_, known_;
    std::vector<Bit16u> args_;
};

// For a job that waits: a call that returns at once, with the emulated CPU
// idle until the next timer event (what HLT does).  The game's interrupts
// run on, emulated time passes, and the trampoline comes round again; a job
// that is not done yet queues itself again in front (enqueue_front).
void idle_call();

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
    // The same, ahead of everything queued (a job that continues itself).
    void enqueue_front(VmJob *job);
    bool has_pending() const { return !jobs_.empty(); }
    bool is_running() const { return running_; }
    // What the job in progress calls itself ("" when none is), for logs.
    const char *current_name() const { return current_ ? current_->describe() : ""; }

    // Start executing queued jobs *instead of* the game function `fn` whose
    // entry we are at: the trampoline's final retf returns to that
    // function's caller, so the intercepted call itself never runs.  A
    // pascal function would have taken its arguments off the stack when it
    // returned; that is done here.
    void run_instead_of_current_call(const Loc &fn);
    // Start executing queued jobs and then resume at the current CS:IP.
    void run_before_current_instruction();

    // Called by the hook dispatcher when CS:IP is the trampoline's NOP.
    void on_bounce();

    // Invoked once when the queue drains (after the last job finished).
    typedef void (*IdleFn)();
    void set_idle_callback(IdleFn fn) { idle_ = fn; }

    // The NOP hook point.
    static bool at_hook();
    static Bit16u hook_ip();

    // The scratch areas are the game's own error messages: what was there
    // goes back when the trampoline has run out (called now and then), and
    // is forgotten when another program is loaded.
    void restore_scratch_if_idle();
    void forget_scratch() { scratchSaved_ = false; }

private:
    void jump_to_stub();
    void save_scratch();
    bool scratchSaved_ = false;
    std::vector<Bit8u> savedCode_, savedData_;
    void start_next();

    std::deque<VmJob *> jobs_;
    VmJob *current_;
    bool running_;
    IdleFn idle_;
};

extern Trampoline g_trampoline;

}  // namespace wc

#endif
