// Issue #296 (MK Trilogy): which branch reached host pc=0?
//
// The null-dispatch signature (pc=0, LR just after the dispatcher's `blr x2`)
// only proves that no BL/BLR ran since ExitFunctionLink returned. Two kinds of
// branch keep LR unchanged and can still reach pc=0 (FEX f2b679f):
//  * the dispatcher linker stub: `blr x2; FillStaticRegs; br x0`
//    (Dispatcher.cpp:285-305). x25 is refilled from State.callret_sp.
//  * JIT inline indirect branches (BranchOps.cpp:186-231), no null check:
//      RET: ldp x0, x1, [x25], #0x10 ; sub x0, x0, rip ; cbz x0, hit
//      L1:  ldp x1, x0, [L1 + (rip & mask) * 16] ; sub x0, x0, rip ; cbz x0, hit
//      hit: ret x1 / br x1
//    A match leaves x0 = 0 and branches to x1. An entry {guest 0, host 0}
//    (call-ret: CALL with no return block pushes `stp xzr, xzr`; L1: a slot
//    never filled, since invalidation clears only GuestCode) matches only
//    guest RIP 0, i.e. slot 0 of L1.
//
// Register roles in FEX f2b679f (non-arm64ec, Arm64Emitter.h/.cpp):
//   x28 = STATE (CpuStateFrame*), x25 = REG_CALLRET_SP, x0/x1 = TMP1/TMP2,
//   x8  = guest RSP (x64::SRA[REG_RSP = 4]), valid only while static
//         registers are filled (JIT code and after FillStaticRegs).
// These are tied to that revision: re-check them when FEX is updated.
//
// Nothing here reads memory: the fault handler does the probed reads and
// fills the *_known flags. classify() is pure, so it is unit-tested.
#pragma once
#include <cstdint>

namespace maxps4 {

// Hypotheses only: the line names them `cand-*` and never says "confirmed".
enum class IndirectPath : uint8_t {
    unavailable = 0,        // trace not captured
    state_mismatch,         // x28 is not this thread's frame: registers not FEX's
    other_branch,           // x0 != 0: not a matched inline lookup nor `br x0`
    ret_callret_zero,       // one call-ret pop, popped {0,0}, x1 = 0
    ret_miss_l1_zero,       // one call-ret pop missed, L1 slot 0 {0,0}, x1 = 0
    l1_zero,                // no pop, L1 slot 0 {0,0}, x1 = 0, dispatcher excluded by efl-ret
    l1_zero_or_dispatcher,  // same, but the dispatcher's `br x0` is not excluded
    dispatcher_or_unknown,  // no pop and L1 slot 0 does not explain x1 = 0
    callret_delta_other,    // x25 moved by something other than 0 or one pop
    ret_unexplained,        // one pop, but neither the entry nor L1 slot 0 explains it
};
inline const char* indirect_path_name(IndirectPath p) {
    switch (p) {
    case IndirectPath::state_mismatch: return "state-mismatch";
    case IndirectPath::other_branch: return "other-branch";
    case IndirectPath::ret_callret_zero: return "cand-ret-callret-zero";
    case IndirectPath::ret_miss_l1_zero: return "cand-ret-l1-zero";
    case IndirectPath::l1_zero: return "cand-l1-zero";
    case IndirectPath::l1_zero_or_dispatcher: return "cand-l1-zero-or-dispatcher";
    case IndirectPath::dispatcher_or_unknown: return "dispatcher-or-unknown";
    case IndirectPath::callret_delta_other: return "callret-delta-other";
    case IndirectPath::ret_unexplained: return "ret-unexplained";
    default: return "unavailable";
    }
}

struct IndirectBranchTrace {
    bool known = false;
    // Observed: from the signal context.
    uint64_t x0 = 0, x1 = 0, x8 = 0, x25 = 0, x28 = 0;
    // Observed: this thread's frame and its spilled call-ret pointer.
    uint64_t frame = 0, state_callret_sp = 0;
    // Observed when *_known: memory read after a bounds check and a probe.
    bool popped_known = false;          // the entry at x25 - 0x10
    uint64_t popped_guest = 0, popped_host = 0;
    bool l1_known = false;              // L1 slot 0 {HostCode, GuestCode}
    uint64_t l1_host = 0, l1_guest = 0;
    bool rsp_slot_known = false;        // guest qword at x8 - 8 (what a RET popped)
    uint64_t rsp_slot = 0;
    // Reconstructed.
    IndirectPath path = IndirectPath::unavailable;
    bool target_known = false;          // guest RIP the matched entry implies
    uint64_t target = 0;

    bool state_ok() const { return x28 && x28 == frame; }
    int64_t callret_delta() const { return int64_t(x25 - state_callret_sp); }
};

// efl_result_nonzero: this thread's last ExitFunctionLink returned a non-null
// HostCode, so the dispatcher's following `br x0` did not branch to 0.
inline void classify_indirect(IndirectBranchTrace& t, bool efl_known, bool efl_result_nonzero) {
    t.target_known = false; t.target = 0;
    if (!t.known) { t.path = IndirectPath::unavailable; return; }
    if (!t.state_ok()) { t.path = IndirectPath::state_mismatch; return; }
    if (t.x0 != 0) { t.path = IndirectPath::other_branch; return; }
    const bool l1_zero = t.l1_known && t.l1_host == 0 && t.l1_guest == 0 && t.x1 == 0;
    const int64_t delta = t.callret_delta();
    if (delta == 0x10) {
        if (!t.popped_known) { t.path = IndirectPath::ret_unexplained; return; }
        if (t.popped_guest == 0 && t.popped_host == 0 && t.x1 == 0) {
            t.path = IndirectPath::ret_callret_zero; t.target_known = true; return;
        }
        if (l1_zero) { t.path = IndirectPath::ret_miss_l1_zero; t.target_known = true; return; }
        t.path = IndirectPath::ret_unexplained;
        return;
    }
    if (delta == 0) {
        if (l1_zero) {
            t.path = efl_known && efl_result_nonzero ? IndirectPath::l1_zero : IndirectPath::l1_zero_or_dispatcher;
            t.target_known = true;
            return;
        }
        t.path = IndirectPath::dispatcher_or_unknown;
        return;
    }
    t.path = IndirectPath::callret_delta_other;
}

} // namespace maxps4
