#pragma once
// Call of Duty: Modern Warfare Remastered (CUSA05379) lays its memory out at
// constant addresses from 0x1000000000 up, which iOS refuses, and keeps using
// them when a mapping is placed elsewhere. All of them come from nine MOVABS
// immediates in default.elf: the region table of one init function and the
// base of its address-range allocator. Moving them into the image's mmap
// window lets its fixed mappings succeed. The file on disk is never changed.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace maxps4 {
// Regions, from a 1 GiB aligned start one GiB into the window (the code ORs
// bits into these bases; what the window serves first stays below): the four
// 2 GiB ones and the small ones keep their spacing, the two 16 GiB ones shrink
// to 5 GiB (the game has 0x13c000000 bytes of memory), the allocator follows.
inline bool orbisRelocateFixedLayout(uint8_t* text, size_t size, uint64_t window, uint64_t windowBytes) {
    constexpr uint64_t gib = uint64_t{1} << 30, large = 5 * gib, odd = 0x200000;
    struct Site { size_t instruction; uint64_t original, placed; bool absolute; };
    const Site sites[] = {
        {0x558ae4, 0x1000000000, 0, false},
        {0x558b23, 0x1080000000, 2 * gib, false},
        {0x558b69, 0x1100000000, 4 * gib, false},
        {0x558b73, 0x1180000000, 6 * gib, false},
        {0x558bc0, 0x1200000000, 8 * gib, false},
        {0x558c23, 0x400000000, large, true}, // Size of the two large regions.
        {0x558c2d, 0x1600200000, 8 * gib + odd + large, false},
        {0x558c7a, 0x1a00200000, 8 * gib + odd + 2 * large, false},
        {0x61cafe, 0x2000000000, 20 * gib, false}};
    constexpr uint64_t span = 22 * gib;
    if (window > UINT64_MAX - windowBytes || window > UINT64_MAX - 2 * gib) return false;
    const uint64_t start = ((window + gib - 1) & ~(gib - 1)) + gib;
    if (start > UINT64_MAX - span || start + span > window + windowBytes) return false;
    for (const auto& site : sites) {
        if (site.instruction + 10 > size || (text[site.instruction] & 0xf8) != 0x48 || (text[site.instruction + 1] & 0xf8) != 0xb8)
            return false;
        uint64_t value = 0;
        std::memcpy(&value, text + site.instruction + 2, 8);
        if (value != site.original) return false;
    }
    for (const auto& site : sites) {
        const uint64_t value = site.absolute ? site.placed : start + site.placed;
        std::memcpy(text + site.instruction + 2, &value, 8);
    }
    return true;
}
}
