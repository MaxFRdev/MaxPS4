// Small, allocation-free decoder used only by the fatal-fault diagnostic.
// AArch64 BLR writes LR to the address after the instruction.  Therefore when
// a null PC has an LR in FEX's dispatcher, LR - 4 is the instruction that
// supplied the failed indirect target.  Keep this independent of FEX so it
// can be tested on every host architecture.
#pragma once

#include <cstdint>

namespace maxps4 {
enum class Arm64IndirectBranch : uint8_t { None, Br, Blr, Ret };

struct Arm64IndirectBranchInfo {
    Arm64IndirectBranch kind = Arm64IndirectBranch::None;
    uint8_t register_number = 0;
};

constexpr Arm64IndirectBranchInfo decode_arm64_indirect_branch(uint32_t instruction) {
    constexpr uint32_t RegisterMask = 0xfffffc1fu;
    const uint32_t opcode = instruction & RegisterMask;
    const uint8_t reg = static_cast<uint8_t>((instruction >> 5) & 31u);
    if (opcode == 0xd61f0000u) return {Arm64IndirectBranch::Br, reg};
    if (opcode == 0xd63f0000u) return {Arm64IndirectBranch::Blr, reg};
    if (opcode == 0xd65f0000u) return {Arm64IndirectBranch::Ret, reg};
    return {};
}

constexpr const char* arm64_indirect_branch_name(Arm64IndirectBranch kind) {
    switch (kind) {
    case Arm64IndirectBranch::Br: return "br";
    case Arm64IndirectBranch::Blr: return "blr";
    case Arm64IndirectBranch::Ret: return "ret";
    default: return "";
    }
}
} // namespace maxps4
