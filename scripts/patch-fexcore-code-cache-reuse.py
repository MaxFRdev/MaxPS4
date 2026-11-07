#!/usr/bin/env python3
"""Restart the JIT's write cursor after the iOS in-place code cache reuse.

When a block does not fit in the rest of the code buffer, the pinned FEX on
iOS clears the code cache in place (ClearCodeCache(Thread, false)) instead of
asking for a new buffer. That path clears the lookup caches but, unlike
ClearCache() on the new-buffer path, never moves the cursor back to the start
of the buffer: the block was then copied at the old, nearly full offset and
ran into the guard page (Call of Duty: Modern Warfare Remastered crashed with
SIGBUS in Arm64JITCore::CompileCode's memcpy when its first mission started).

The cursor now restarts at offset 0 behind a fresh detection string, as
ClearCache() does for a new buffer.

All edits are validated before writing; repeated application must match exactly.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4 code cache reuse restarts the cursor'
FILE = 'FEXCore/Source/Interface/Core/JIT/JIT.cpp'
OLD = '''        CTX->ClearCodeCache(ThreadState, false);
        InvalidationMutex.lock_shared();
'''
NEW = '''        CTX->ClearCodeCache(ThreadState, false);
        InvalidationMutex.lock_shared();
        ''' + TAG + '''
        // The buffer's old code is gone: write from its start again, as ClearCache()
        // does for a new buffer, instead of past the end of the reused one.
        SetCursorOffset(0);
        EmitDetectionString();
        Align16B();
'''


def adapt(root):
    path = root / FILE
    text = path.read_text()
    if text.count(TAG) == 1:
        return
    if text.count(OLD) != 1:
        raise ValueError(f'Pinned code cache reuse context diverged: {FILE}')
    path.write_text(text.replace(OLD, NEW, 1))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: patch-fexcore-code-cache-reuse.py FEX_ROOT')
    adapt(Path(sys.argv[1]))
