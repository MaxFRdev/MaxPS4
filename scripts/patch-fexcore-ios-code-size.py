#!/usr/bin/env python3
"""Give the iOS JIT code cache FEX's largest size, 128 MiB, from the start.

On iOS the pinned FEX keeps one code buffer of INITIAL_CODE_SIZE (16 MiB) and,
once it is full, clears and reuses it in place while the other guest threads
keep running. Their call-return stacks, lookup caches and direct branches can
still point into the old code, which is overwritten: Call of Duty: Modern
Warfare Remastered filled 16 MiB while its first mission loaded and then
crashed at random places (bad pointers, a jump to address 0).

128 MiB is FEX's own MAX_CODE_SIZE elsewhere (the reach of ARM64 direct
branches). MaxPS4 prepares a code region of the same size through StikDebug
(maxps4_jit_memory.cpp, MaxPS4StikRequest.swift), so a game fills it far
later, if at all.

All edits are validated before writing; repeated application must match exactly.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4 iOS code cache size'
FILE = 'FEXCore/Source/Interface/Core/CPUBackend.cpp'
OLD = '  static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;\n'
NEW = '''#if defined(__APPLE__) && TARGET_OS_IPHONE
  ''' + TAG + '''
  // One buffer, prepared by StikDebug at this exact size (maxps4_jit_memory.cpp): the largest
  // FEX supports, so the in-place reuse below (with other threads still running) stays rare.
  static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 128;
#else
  static constexpr size_t INITIAL_CODE_SIZE = 1024 * 1024 * 16;
#endif
'''


def adapt(root):
    path = root / FILE
    text = path.read_text()
    if text.count(TAG) == 1:
        return
    if text.count(OLD) != 1:
        raise ValueError(f'Pinned code size context diverged: {FILE}')
    path.write_text(text.replace(OLD, NEW, 1))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: patch-fexcore-ios-code-size.py FEX_ROOT')
    adapt(Path(sys.argv[1]))
