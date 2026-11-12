// Issue #296 (MK Trilogy): FEX's generated dispatcher calls CompileBlock /
// CompileSingleStep / ExitFunctionLink with BLR and then branches to the
// returned host address with `br x0`, without a null check. CompileBlock
// returns 0 when the first guest instruction of a block gets a dispatch error
// (FEX decoded it but its opcode handler cannot lower it). The host then runs
// at pc=0 with LR still pointing just after the BLR inside the dispatcher.
// That PC is neither JIT nor dispatcher code, so it escaped guest fault
// containment and killed the app.
//
// The signature is exact: pc == 0 and LR inside the dispatcher. It is a guest
// fault (the guest reached code FEX cannot compile), reported as SIGILL at the
// guest RIP FEX stored before the call.
#pragma once
#include <cstdint>
#include <csignal>

namespace maxps4 {
// si_code reported for this fault (not a kernel code): "FEX could not compile
// the guest block at the fault address".
constexpr int32_t uncompilable_guest_block = 0x4d50; // 'MP'

inline bool uncompilable_dispatch(int signal, uint64_t pc, uint64_t lr,
                                  uint64_t dispatcher_begin, uint64_t dispatcher_end) {
    return signal == SIGSEGV && pc == 0 && dispatcher_begin < dispatcher_end &&
           lr >= dispatcher_begin && lr < dispatcher_end;
}

// Which dispatcher call returned 0: the instruction just before LR (non-EC
// ARM64, pinned FEX). The two paths compile a different guest RIP:
//  * blr x4: CompileBlock / CompileSingleStep (NoBlock path) compiles
//    State.rip, which the dispatcher has just loaded.
//  * blr x2: ExitFunctionLink, called from a JIT jump thunk for a direct
//    JMP/CALL, compiles Record->GuestRIP. State.rip is NOT updated by linked
//    branches, so it is a stale value (e.g. where the last HLE SYSCALL left it).
enum class DispatchCall : uint8_t { unknown = 0, compile_block = 1, exit_link = 2 };
inline DispatchCall dispatch_call(uint32_t instruction_before_lr) {
    if (instruction_before_lr == 0xd63f0080u) return DispatchCall::compile_block; // blr x4
    if (instruction_before_lr == 0xd63f0040u) return DispatchCall::exit_link;     // blr x2
    return DispatchCall::unknown;
}
inline const char* dispatch_call_name(DispatchCall c) {
    return c == DispatchCall::compile_block ? "compile-block" : c == DispatchCall::exit_link ? "exit-link" : "unknown";
}

// Target of an ARM64 `bl` at `at`.
inline bool bl_target(uint64_t at, uint32_t instruction, uint64_t& target) {
    if ((instruction & 0xfc000000u) != 0x94000000u) return false;
    int64_t words = int64_t(instruction & 0x03ffffffu);
    if (words & 0x02000000) words -= 0x04000000;
    target = at + uint64_t(words * 4);
    return true;
}

// A JIT jump thunk (JIT.cpp, PendingJumpThunks) followed by its
// ExitFunctionLinkData record:
//   00: b +8            (unlinked)  or  ldr x0, #0x10 (linked out of range:
//   04: br x0                            ExitFunctionLink stores HostCode and
//   08: ldr x0, <linker>                 patches word 0, even when HostCode is 0)
//   0c: blr x0
//   10: HostCode   18: GuestRIP   20: CallerOffset (caller - thunk)
struct JumpThunk { uint32_t insn[4]; uint64_t host_code, guest_rip, caller_offset; };
inline bool jump_thunk_record(const JumpThunk& t, uint64_t thunk, uint64_t caller) {
    const bool first = t.insn[0] == 0x14000002u || t.insn[0] == 0x58000080u;
    return first && t.insn[1] == 0xd61f0000u && (t.insn[2] & 0xff00001fu) == 0x58000000u &&
           t.insn[3] == 0xd63f0000u && thunk + t.caller_offset == caller;
}
} // namespace maxps4
