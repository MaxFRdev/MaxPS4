#!/usr/bin/env python3
"""Keep FEX's register allocator off x18 on Apple hosts.

x18 is the platform register on iOS and macOS: the kernel zeroes it whenever
it takes the CPU (an exception, an interrupt, a thread switch). The pinned
FEX hands it out as the last of its 64-bit mode GPRs, so a value the JIT kept
there could vanish between two instructions. CoD showed it: MOVZ/MOVK built
an address in x18 and the STR right after, the first instruction of a new
code page (whose first execution enters the kernel), wrote to 0.

r18 is the RA list's last entry and outside its register pairs (PairRegs 4:
indices 0-3), so dropping it leaves six allocatable GPRs and nothing else
changes. The dynamic (spill) lists keep r18: saving and restoring it is
harmless. Validated before writing; applying twice changes nothing.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4: no x18 on Apple hosts'
EMITTER = 'FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp'
OLD = '''  constexpr std::array<ARMEmitter::Register, 7> RA = {
    // All these callee saved
    ARMEmitter::Reg::r20, ARMEmitter::Reg::r21, ARMEmitter::Reg::r22, ARMEmitter::Reg::r23,
    ARMEmitter::Reg::r24, ARMEmitter::Reg::r30, ARMEmitter::Reg::r18,
  };'''
NEW = '''#if defined(__APPLE__)
  // MaxPS4: no x18 on Apple hosts. It is the platform register, zeroed by the
  // kernel whenever it takes the CPU, so a value allocated there can vanish.
  constexpr std::array<ARMEmitter::Register, 6> RA = {
    ARMEmitter::Reg::r20, ARMEmitter::Reg::r21, ARMEmitter::Reg::r22, ARMEmitter::Reg::r23,
    ARMEmitter::Reg::r24, ARMEmitter::Reg::r30,
  };
#else
  constexpr std::array<ARMEmitter::Register, 7> RA = {
    // All these callee saved
    ARMEmitter::Reg::r20, ARMEmitter::Reg::r21, ARMEmitter::Reg::r22, ARMEmitter::Reg::r23,
    ARMEmitter::Reg::r24, ARMEmitter::Reg::r30, ARMEmitter::Reg::r18,
  };
#endif'''


def adapt(root):
    path = root / EMITTER
    text = path.read_text()
    if TAG in text:
        if text.count(NEW) != 1:
            raise ValueError('Applied x18 patch diverged')
        return
    if text.count(OLD) != 1:
        raise ValueError('Pinned 64-bit RA list diverged')
    path.write_text(text.replace(OLD, NEW, 1))


if __name__ == '__main__':
    adapt(Path(sys.argv[1]))
