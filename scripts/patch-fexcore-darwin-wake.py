"""Fix the pinned Darwin shared-word writer wake; preserve other platforms."""
from pathlib import Path
import sys

FILE = 'FEXCore/include/FEXCore/Utils/WritePriorityMutex.h'
OLD = '''  void FutexWakeWriter() {
    os_sync_wake_by_address_any(&Futex, sizeof(Futex), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  }'''
NEW = '''  void FutexWakeWriter() {
    // MaxPS4: readers share this address with writers. A reader can consume
    // a wake-one, fail the writer-priority predicate and sleep again, leaving
    // the eligible writer asleep with nobody left to unlock. Wake both kinds.
    os_sync_wake_by_address_all(&Futex, sizeof(Futex), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  }'''


def adapt(root):
    path = root / FILE
    text = path.read_text()
    if text.count(NEW) == 1 and OLD not in text:
        return
    if text.count(OLD) != 1 or NEW in text:
        raise ValueError('Pinned Darwin writer wake context diverged')
    path.write_text(text.replace(OLD, NEW, 1))


if __name__ == '__main__':
    adapt(Path(sys.argv[1]))
