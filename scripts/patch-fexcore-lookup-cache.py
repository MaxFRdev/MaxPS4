#!/usr/bin/env python3
"""Shrink the address space FEX reserves for each guest thread's lookup cache.

The pinned FEX reserves 272 MiB per thread (a page table for 64 GiB of guest
addresses, 128 MiB of block pointers, 16 MiB of L1) and does not check the
reservation. iOS gives the app little address space: with a usable mmap window,
Mortal Kombat: Legacy Kollection's threads ran out of it, and Mortal Kombat
Trilogy's 2 GiB direct map left room for about 24 threads at 64 MiB, short of
its two VP9 decoders' workers. The page table now covers 4 GiB (lookups mask
the address and compare the full guest address, so higher addresses only alias
and miss to the shared L3 map), block pointers get 24 MiB (a full table only
clears that thread's L1/L2, refilled from L3 without recompiling) and the
dynamic L1 grows to 256 Ki entries: 36 MiB per thread.

All edits are validated before writing; repeated application must match exactly.
"""
from pathlib import Path
import sys

TAG = '// MaxPS4 smaller per-thread lookup cache'
EDITS = {
    'FEXCore/Source/Interface/Context/Context.h': (
        '    uint64_t VirtualMemSize {1ULL << 36};',
        '    uint64_t VirtualMemSize {1ULL << 32}; ' + TAG,
    ),
    'FEXCore/Source/Interface/Core/LookupCache.h': (
        '  constexpr static size_t CODE_SIZE = 128 * 1024 * 1024;',
        '  constexpr static size_t CODE_SIZE = 24 * 1024 * 1024; ' + TAG,
    ),
}
# A second edit in the same file: applied after EDITS by adapt().
L1_EDIT = ('FEXCore/Source/Interface/Core/LookupCache.h',
           '  constexpr static size_t MAX_L1_ENTRIES = 1 * 1024 * 1024; // Must be a power of 2',
           '  constexpr static size_t MAX_L1_ENTRIES = 256 * 1024; // Must be a power of 2 ' + TAG)


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
    name, old, new = L1_EDIT
    text = texts[name]
    if text.count(new) != 1:
        if text.count(old) != 1:
            raise ValueError(f'Pinned lookup cache context diverged: {name}')
        texts[name] = text.replace(old, new, 1)
    for name, text in texts.items():
        (root / name).write_text(text)


if __name__ == '__main__':
    adapt(Path(sys.argv[1]))
