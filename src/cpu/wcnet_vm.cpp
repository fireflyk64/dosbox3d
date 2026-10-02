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
enum {
    kShellcodeOff = ds::aSorryAnErrorHasOccured,      // 249 bytes
    kTrampolineOff = ds::aLoadingWingCom + 101,       // 6 bytes
    kTrampolineNop = kTrampolineOff + 3,
};

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

bool at_function(const code::OverlayFn &fn) {
    if (reg_eip != fn.ovrOff) {
        return false;
    }
    Bit16u seg = overlay_segment(fn.stubSeg, fn.stubOff);
    return seg != 0 && SegValue(cs) == seg;
}

bool at_location(const code::OverlayLoc &loc) {
    if (reg_eip != loc.ovrOff) {
        return false;
    }
    Bit16u seg = overlay_segment(loc.stubSeg, loc.anyStubOff);
    return seg != 0 && SegValue(cs) == seg;
}

bool at_location(const code::RootLoc &loc) {
    return reg_eip == loc.off && SegValue(cs) == loc.seg;
}

Bit16u call_arg16(int n) {
    // SS == DS in this program; at a far function entry the return address
    // occupies the top 4 bytes.
    return mem_readw(DS_OFF + (reg_esp & 0xffff) + 4 + 2 * n);
}

void set_call_arg16(int n, Bit16u value) {
    mem_writew(DS_OFF + (reg_esp & 0xffff) + 4 + 2 * n, value);
}

void return_from_call(Bit16u ax) {
    Bit16u ip = CPU_Pop16();
    Bit16u seg = CPU_Pop16();
    reg_eax = ax;
    SegSet16(cs, seg);
    reg_eip = ip;
}

void GameCall::invoke() const {
    CPU_Push16((Bit16u)SegValue(cs));
    CPU_Push16((Bit16u)reg_eip);
    std::vector<Bit8u> code;
    code.push_back(0x00);  // keep the reused string NUL terminated
    code.push_back(0x56);  // push si (preserve callee-saved register)
    for (size_t i = args_.size(); i-- > 0;) {
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
    for (size_t i = 0; i < args_.size(); i++) {
        code.push_back(0x5E);  // pop si (discard argument)
    }
    code.push_back(0x5E);  // pop si (restore)
    code.push_back(0xCB);  // retf
    if (code.size() > 249) {
        wclog(0, "GameCall thunk too large (%d bytes)", (int)code.size());
        return;
    }
    for (size_t i = 0; i < code.size(); i++) {
        mem_writeb_checked(DS_OFF + kShellcodeOff + i, code[i]);
    }
    SegSet16(cs, DS);
    reg_eip = kShellcodeOff + 1;
}

Trampoline g_trampoline;

Trampoline::Trampoline() : current_(NULL), running_(false), idle_(NULL) {}

void Trampoline::enqueue(VmJob *job) {
    jobs_.push_back(job);
}

void Trampoline::jump_to_stub() {
    for (size_t i = 0; i < sizeof(kTrampolineCode); i++) {
        mem_writeb_checked(DS_OFF + kTrampolineOff + i, kTrampolineCode[i]);
    }
    SegSet16(cs, DS);
    reg_eip = kTrampolineOff;
    running_ = true;
}

void Trampoline::run_instead_of_current_call() {
    if (running_) {
        wclog(0, "Trampoline re-entered while running; ignoring");
        return;
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
    return SegValue(cs) == DS && reg_eip == kTrampolineNop;
}

Bit16u Trampoline::hook_ip() {
    return kTrampolineNop;
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
