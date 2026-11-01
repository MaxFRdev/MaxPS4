#!/usr/bin/env python3
"""Shrink the address space FEX reserves for each guest thread's lookup cache.

The pinned FEX reserves 272 MiB per thread (a page table for 64 GiB of guest
addresses, 128 MiB of block pointers, 16 MiB of L1) and does not check the
reservation. iOS gives the app little address space: with a usable mmap window,
Mortal Kombat: Legacy Kollection's threads ran out of it. The page table now
covers 16 GiB (lookups mask the address and compare the full guest address, so
higher addresses only alias) and block pointers get 64 MiB (a full table only
clears that thread's cache): 112 MiB per thread.

All edits are validated before writing; repeated application must match exactly.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4 smaller per-thread lookup cache'
EDITS = {
    'FEXCore/Source/Interface/Context/Context.h': (
        '    uint64_t VirtualMemSize {1ULL << 36};',
        '    uint64_t VirtualMemSize {1ULL << 34}; ' + TAG,
    ),
    'FEXCore/Source/Interface/Core/LookupCache.h': (
        '  constexpr static size_t CODE_SIZE = 128 * 1024 * 1024;',
        '  constexpr static size_t CODE_SIZE = 64 * 1024 * 1024; ' + TAG,
    ),
}


def adapt(root):
    texts = {}
    for name, (old, new) in EDITS.items():
        text = (root / name).read_text()
        if text.count(new) == 1:
            texts[name] = text
        elif text.count(old) == 1:
            texts[name] = text.replace(old, new, 1)
        else:
            raise ValueError(f'Pinned lookup cache context diverged: {name}')
    for name, text in texts.items():
        (root / name).write_text(text)


if __name__ == '__main__':
    adapt(Path(sys.argv[1]))
