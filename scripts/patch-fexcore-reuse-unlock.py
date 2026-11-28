#!/usr/bin/env python3
"""Reusing the iOS code buffer in place without a deadlock, from its start.

When the single iOS code buffer is full, Arm64JITCore::CompileCode releases
its shared CodeInvalidationMutex and calls ClearCodeCache(false), which takes
that mutex exclusively, but it still held CodeBufferWriteMutex. Another thread
compiling at the same time holds the invalidation mutex shared and waits for
CodeBufferWriteMutex: neither can go on, and every thread then blocks on the
write-priority mutex (CoD froze after about 80 s, all threads in FEX's
LookupCache/ExitFunctionLink locks).

The pinned code also kept writing at the old cursor after the in-place clear,
past the buffer's end. This patch releases CodeBufferWriteMutex around the
clear, takes the locks back in their usual order and restarts at the buffer's
start (after its detection string), unless another thread already did.
Validated before writing; applying twice changes nothing.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4: reuse the code buffer without holding CodeBufferWriteMutex'
JIT = 'FEXCore/Source/Interface/Core/JIT/JIT.cpp'
OLD = '''        auto& InvalidationMutex = CTX->GetCodeInvalidationMutex();
        InvalidationMutex.unlock_shared();
        CTX->ClearCodeCache(ThreadState, false);
        InvalidationMutex.lock_shared();
#else'''
NEW = '''        ''' + TAG + '''
        // (another compiling thread holds the invalidation mutex shared while
        // it waits for CodeBufferWriteMutex), then restart at the start.
        auto& InvalidationMutex = CTX->GetCodeInvalidationMutex();
        const auto SeenOffset = CodeBuffers.LatestOffset;
        CodeBufferLock.unlock();
        InvalidationMutex.unlock_shared();
        CTX->ClearCodeCache(ThreadState, false);
        InvalidationMutex.lock_shared();
        CodeBufferLock.lock();
        SetBuffer(CurrentCodeBuffer->Ptr, CurrentCodeBuffer->AllocatedSize);
        if (CodeBuffers.LatestOffset == SeenOffset) {
          SetCursorOffset(0);
          EmitDetectionString();
        } else {
          SetCursorOffset(CodeBuffers.LatestOffset);
          Align16B();
        }
#else'''


def adapt(root):
    path = root / JIT
    text = path.read_text()
    if TAG in text:
        if text.count(NEW) != 1:
            raise ValueError('Applied reuse patch diverged')
        return
    if text.count(OLD) != 1:
        raise ValueError('Pinned in-place reuse call diverged')
    path.write_text(text.replace(OLD, NEW, 1))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('Usage: patch-fexcore-reuse-unlock.py FEX_SOURCE')
    adapt(Path(sys.argv[1]))
