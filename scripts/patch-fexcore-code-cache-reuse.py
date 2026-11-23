#!/usr/bin/env python3
"""Make the iOS in-place code cache reuse restart the cursor without deadlocking.

When a block does not fit in the rest of the code buffer, the pinned FEX on
iOS clears the code cache in place (ClearCodeCache(Thread, false)) instead of
asking for a new buffer. Two faults lived on that path:

* It never moved the cursor back to the start of the buffer: the block was
  copied at the old, nearly full offset and ran into the guard page (Call of
  Duty: Modern Warfare Remastered crashed with SIGBUS in
  Arm64JITCore::CompileCode's memcpy when its first mission started).
* It asked for CodeInvalidationMutex exclusively while still holding
  CodeBufferWriteMutex. Every other compiling thread holds
  CodeInvalidationMutex shared and then waits for CodeBufferWriteMutex, so a
  clear that came while another thread compiled deadlocked both, and then
  every thread behind them (Call of Duty stopped at boot with 0 images).

The thread that finds the buffer full now marks it full and lets go of both
locks. Clears are serialized by their own mutex and taken with no other lock
held; the locks are then taken again in the usual order (invalidation, then
buffer). The first thread back under the buffer lock after the clear restarts
the cursor at offset 0 behind a fresh detection string, as ClearCache() does
for a new buffer. Threads that find the buffer marked full while a clear is
pending wait for it instead of writing into the buffer being reused.

All edits are validated before writing; repeated application must match exactly.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4 code cache reuse restarts the cursor without deadlocking'
FILE = 'FEXCore/Source/Interface/Core/JIT/JIT.cpp'

STATE_ANCHOR = 'namespace FEXCore::CPU {\n'
STATE = '''#include <atomic>
#include <mutex>
namespace {
''' + TAG + '''
// The in-place clears of the iOS code buffer, one at a time and with no other lock held.
std::mutex MaxPS4ReuseMutex;
// Under CodeBufferWriteMutex: the buffer is full and waits for a clear, and that clear happened.
bool MaxPS4ReuseFull = false;
bool MaxPS4ReuseCleared = false;
// For MaxPS4's report: in-place clears so far, and the code buffer's fill.
std::atomic<uint64_t> MaxPS4ReuseCount {0}, MaxPS4CodeUsed {0};
} // namespace
extern "C" uint64_t maxps4_fex_code_cache_reuses() { return MaxPS4ReuseCount.load(std::memory_order_relaxed); }
extern "C" uint64_t maxps4_fex_code_cache_used() { return MaxPS4CodeUsed.load(std::memory_order_relaxed); }

'''

START = '      if ((GetCursorOffset() + TempSize) > CurrentCodeBuffer->UsableSize()) {\n#if defined(__APPLE__) && TARGET_OS_IPHONE\n'
END = '''        CTX->ClearCodeCache(ThreadState, false);
        InvalidationMutex.lock_shared();
#else
        CTX->ClearCodeCache(ThreadState);
#endif
      }
'''
NEW = '''#if defined(__APPLE__) && TARGET_OS_IPHONE
      // ''' + TAG[3:] + '''
      // The buffer is reused in place (NewCodeBuffer=false): StikDebug cannot hand out a new
      // one each time it fills. ClearCodeCache then takes CodeInvalidationMutex exclusively,
      // while every compiling thread holds it shared and waits for CodeBufferWriteMutex: keep
      // neither lock while clearing, and take them back in that same order afterwards.
      while (MaxPS4ReuseFull || (GetCursorOffset() + TempSize) > CurrentCodeBuffer->UsableSize()) {
        MaxPS4ReuseFull = true;
        auto& InvalidationMutex = CTX->GetCodeInvalidationMutex();
        CodeBufferLock.unlock();
        InvalidationMutex.unlock_shared();
        {
          std::lock_guard Reuse {MaxPS4ReuseMutex};
          std::unique_lock Buffers {CodeBuffers.CodeBufferWriteMutex};
          const bool Clear = MaxPS4ReuseFull && !MaxPS4ReuseCleared;
          Buffers.unlock();
          if (Clear) {
            CTX->ClearCodeCache(ThreadState, false);
            Buffers.lock();
            MaxPS4ReuseCleared = true;
            MaxPS4ReuseCount.fetch_add(1, std::memory_order_relaxed);
          }
        }
        InvalidationMutex.lock_shared();
        CodeBufferLock.lock();
        if (MaxPS4ReuseFull && MaxPS4ReuseCleared) {
          // The buffer's old code is gone: write from its start again, as ClearCache()
          // does for a new buffer, instead of past the end of the reused one.
          MaxPS4ReuseFull = MaxPS4ReuseCleared = false;
          SetBuffer(CurrentCodeBuffer->Ptr, CurrentCodeBuffer->AllocatedSize);
          SetCursorOffset(0);
          EmitDetectionString();
          Align16B();
          CodeBuffers.LatestOffset = GetCursorOffset();
        }
        SetBuffer(CurrentCodeBuffer->Ptr, CurrentCodeBuffer->AllocatedSize);
        SetCursorOffset(CodeBuffers.LatestOffset);
        Align16B();
      }
#else
      if ((GetCursorOffset() + TempSize) > CurrentCodeBuffer->UsableSize()) {
        CTX->ClearCodeCache(ThreadState);
      }
#endif
'''


USED_OLD = '''    SetCursorOffset(CodeBuffers.LatestOffset + TempSize);

    CodeBuffers.LatestOffset = GetCursorOffset();
  }
'''
USED_NEW = '''    SetCursorOffset(CodeBuffers.LatestOffset + TempSize);

    CodeBuffers.LatestOffset = GetCursorOffset();
#if defined(__APPLE__) && TARGET_OS_IPHONE
    MaxPS4CodeUsed.store(CodeBuffers.LatestOffset, std::memory_order_relaxed);
#endif
  }
'''


def adapt(root):
    path = root / FILE
    text = path.read_text()
    if text.count(TAG) == 2:
        return
    if (text.count(STATE_ANCHOR) != 1 or text.count(START) != 1 or text.count(END) != 1 or
            text.count(USED_OLD) != 1):
        raise ValueError(f'Pinned code cache reuse context diverged: {FILE}')
    start = text.index(START)
    end = text.index(END, start) + len(END)
    text = text[:start] + NEW + text[end:]
    text = text.replace(USED_OLD, USED_NEW, 1)
    text = text.replace(STATE_ANCHOR, STATE + STATE_ANCHOR, 1)
    path.write_text(text)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: patch-fexcore-code-cache-reuse.py FEX_ROOT')
    adapt(Path(sys.argv[1]))
