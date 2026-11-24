#!/usr/bin/env python3
"""Darwin WritePriorityMutex: wake every waiter on a writer hand-off.

Darwin's os_sync_wake_by_address_any has no bitset filter: on the Futex word
that writers and readers share it wakes one waiter, which can be a reader.
That reader sees the writer still waiting and sleeps again, and the writer is
never woken although the lock is free. CoD froze this way after 20-100 s:
one thread in LookupCache::AddBlockMapping (write lock), every other JIT
thread in lock_shared, and no thread holding the lock. Waking all waiters
lets each re-check the state, as the class's wait loops already allow.
Validated before writing; applying twice changes nothing.
"""
from pathlib import Path
import sys

HEADER = 'FEXCore/include/FEXCore/Utils/WritePriorityMutex.h'
OLD = '''  void FutexWakeWriter() {
    os_sync_wake_by_address_any(&Futex, sizeof(Futex), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  }'''
NEW = '''  void FutexWakeWriter() {
    // MaxPS4: all, not any -- "any" could wake a reader that sleeps again,
    // leaving the writer asleep on a free lock (no bitset filter on Darwin).
    os_sync_wake_by_address_all(&Futex, sizeof(Futex), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  }'''


def adapt(root):
    path = root / HEADER
    text = path.read_text()
    if NEW in text:
        return
    if text.count(OLD) != 1:
        raise ValueError('Pinned Darwin FutexWakeWriter diverged')
    path.write_text(text.replace(OLD, NEW, 1))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('Usage: patch-fexcore-wpmutex-wake.py FEX_SOURCE')
    adapt(Path(sys.argv[1]))
