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
} // namespace maxps4
