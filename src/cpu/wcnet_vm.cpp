#include <stdio.h>
#include "wcnet_vm.h"
#include "wcnet_memory.h"
#include "wcnet_log.h"
#include "cpu.h"
#include "regs.h"
#include "paging.h"

namespace wc {

// Scratch areas inside the data segment.  These are string constants the game
// only uses for error messages, so we can overwrite them with code (CS is set
// to DS while it runs).
static Bit16u shellcode_off() { return ds::aSorryAnErrorHasOccured; }         // 249 bytes
static Bit16u trampoline_off() { return (Bit16u)(ds::aLoadingWingCom + 101); } // 6 bytes
static Bit16u trampoline_nop() { return (Bit16u)(trampoline_off() + 3); }

static const Bit8u kTrampolineCode[6] = {
    0x55,       // push bp
    0x8B, 0xEC, // mov bp,sp
    0x90,       // nop   <- hook point
    0x5D,       // pop bp
    0xCB,       // retf
};

Bit16u overlay_segment(Bit16u stubSeg, Bit16u stubOff) {
    PhysPt p = stubSeg * 0x10 + stubOff;
    if (mem_readb(p) != 0xEA) {
        return 0;
    }
    return mem_readw(p + 3);
}

bool at(const Loc &loc) {
    if (!loc.seg || reg_eip != loc.off) {
        return false;
    }
    if (!loc.overlay) {
        return SegValue(cs) == loc.seg;
    }
    Bit16u seg = overlay_segment(loc.seg, loc.stubOff);
    return seg != 0 && SegValue(cs) == seg;
}

Bit16u call_arg16(int n) {
    // SS == DS in this program; at a far function entry the return address
    // occupies the top 4 bytes.
    return mem_readw(DS_OFF + (reg_esp & 0xffff) + 4 + 2 * n);
}

void set_call_arg16(int n, Bit16u value) {
    mem_writew(DS_OFF + (reg_esp & 0xffff) + 4 + 2 * n, value);
}

// A pascal function's first argument was pushed first: it is the deepest.
static int stack_index(const Loc &fn, int n) {
    return fn.pascal ? fn.nargs - 1 - n : n;
}

Bit16u call_arg16(const Loc &fn, int n) {
    return call_arg16(stack_index(fn, n));
}

void set_call_arg16(const Loc &fn, int n, Bit16u value) {
    set_call_arg16(stack_index(fn, n), value);
}

void return_from_call(Bit16u ax) {
    Bit16u ip = CPU_Pop16();
    Bit16u seg = CPU_Pop16();
    reg_eax = ax;
    SegSet16(cs, seg);
    reg_eip = ip;
}

void return_from_call(const Loc &fn, Bit16u ax) {
    return_from_call(ax);
    if (fn.pascal) {
        reg_esp = (reg_esp & ~0xffffu) | ((reg_esp + 2 * fn.nargs) & 0xffff);  // retf n
    }
}

void GameCall::invoke() const {
    CPU_Push16((Bit16u)SegValue(cs));
    CPU_Push16((Bit16u)reg_eip);
    std::vector<Bit8u> code;
    code.push_back(0x00);  // keep the reused string NUL terminated
    if (!known_) {
        // The running game has no such function: return at once.
        code.push_back(0xCB);  // retf
    } else {
        code.push_back(0x56);  // push si (preserve callee-saved register)
        // cdecl pushes the last argument first; pascal the first.
        for (size_t k = 0; k < args_.size(); k++) {
            size_t i = pascal_ ? k : args_.size() - 1 - k;
            code.push_back(0xBE);  // mov si, imm16
            code.push_back((Bit8u)(args_[i] & 0xff));
            code.push_back((Bit8u)(args_[i] >> 8));
            code.push_back(0x56);  // push si
        }
        code.push_back(0x9A);  // call far seg:off
        code.push_back((Bit8u)(off_ & 0xff));
        code.push_back((Bit8u)(off_ >> 8));
        code.push_back((Bit8u)(seg_ & 0xff));
        code.push_back((Bit8u)(seg_ >> 8));
        for (size_t i = 0; i < args_.size() && !pascal_; i++) {
            code.push_back(0x5E);  // pop si (discard argument; a pascal callee did)
        }
        code.push_back(0x5E);  // pop si (restore)
        code.push_back(0xCB);  // retf
    }
    if (code.size() > 249) {
        wclog(0, "GameCall thunk too large (%d bytes)", (int)code.size());
        return;
    }
    for (size_t i = 0; i < code.size(); i++) {
        mem_writeb_checked(DS_OFF + shellcode_off() + i, code[i]);
    }
    SegSet16(cs, DS);
    reg_eip = shellcode_off() + 1;
}

void idle_call() {
    static const Loc kNowhere = { 0, 0, 0, 0, 0, 0, "nowhere" };  // an unknown function: the call returns at once
    GameCall(kNowhere).invoke();
    CPU_IODelayRemoved += CPU_Cycles;
    CPU_Cycles = 0;
}

Trampoline g_trampoline;

Trampoline::Trampoline() : current_(NULL), running_(false), idle_(NULL) {}

void Trampoline::enqueue(VmJob *job) {
    jobs_.push_back(job);
}

void Trampoline::enqueue_front(VmJob *job) {
    jobs_.push_front(job);
}

void Trampoline::jump_to_stub() {
    for (size_t i = 0; i < sizeof(kTrampolineCode); i++) {
        mem_writeb_checked(DS_OFF + trampoline_off() + i, kTrampolineCode[i]);
    }
    SegSet16(cs, DS);
    reg_eip = trampoline_off();
    running_ = true;
}

void Trampoline::run_instead_of_current_call(const Loc &fn) {
    if (running_) {
        wclog(0, "Trampoline re-entered while running; ignoring");
        return;
    }
    if (fn.pascal && fn.nargs) {
        // The caller of a pascal function leaves the arguments to the
        // callee's `retf n`.  Left on the stack they shift everything the
        // caller pops afterwards: WC2's "fire all guns" (ovr114:3EC3) then
        // returned into nowhere, a divide error with the first shot.
        Bit16u ip = CPU_Pop16();
        Bit16u seg = CPU_Pop16();
        reg_esp = (reg_esp & ~0xffffu) | ((reg_esp + 2 * fn.nargs) & 0xffff);
        CPU_Push16(seg);
        CPU_Push16(ip);
    }
    jump_to_stub();
}

void Trampoline::run_before_current_instruction() {
    if (running_) {
        wclog(0, "Trampoline re-entered while running; ignoring");
        return;
    }
    CPU_Push16((Bit16u)SegValue(cs));
    CPU_Push16((Bit16u)reg_eip);
    jump_to_stub();
}

bool Trampoline::at_hook() {
    return DS != 0 && SegValue(cs) == DS && reg_eip == trampoline_nop();
}

Bit16u Trampoline::hook_ip() {
    return trampoline_nop();
}

void Trampoline::start_next() {
    while (!jobs_.empty()) {
        current_ = jobs_.front();
        jobs_.pop_front();
        wclog(3, "trampoline: start %s", current_->describe());
        if (current_->start()) {
            return;  // a game call is in flight; finish() runs on the next bounce
        }
        current_->finish();
        delete current_;
        current_ = NULL;
    }
    // Queue drained: let the stub's retf take us back.
    running_ = false;
    if (idle_) {
        idle_();
    }
}

void Trampoline::on_bounce() {
    if (current_) {
        wclog(3, "trampoline: finish %s", current_->describe());
        current_->finish();
        delete current_;
        current_ = NULL;
    }
    start_next();
}

}  // namespace wc
