#pragma once
// Per-game patches of loaded code, for games iOS cannot run as they are.
//
// Call of Duty: Modern Warfare Remastered (CUSA05379, default.elf booted in
// place of its launcher as "eboot.bin") reserves its virtual memory from the
// fixed PS4 address 0x2000000000 upward (the allocator at 0x61cafe:
// MOVABS RBX,0x2000000000). With extended virtual addressing iOS keeps
// [0x1000000000, 0x7000000000) behind a permanent guard no app may map, so
// that base moves into the top of MaxPS4's mmap window, which takes fixed
// mappings and where MaxPS4 then places nothing of its own.
//
// A patch applies only where the loaded bytes are exactly the expected ones,
// so another game, or another version of this one, stays untouched.
#include "maxps4_guest_elf.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace maxps4 {
struct GameVirtualBase {
    const char* game;         // For the report.
    const char* module;       // ModuleRecord name.
    uint64_t offset;          // Of the MOVABS from the module base.
    uint8_t opcode[2];        // REX.W + B8+r.
    uint64_t ps4_base;        // Its immediate.
};
inline constexpr GameVirtualBase game_virtual_bases[] = {
    {"Call of Duty: Modern Warfare Remastered", "eboot.bin", 0x61cafe, {0x48, 0xbb}, 0x2000000000ULL},
};
// Game globals the stop report reads when the game asks to be relaunched
// (sceSystemServiceLoadExec), for games that restart on an error: a 32-bit
// count of 64 KiB pages each. CoD's page pools (filled at boot with all the
// direct memory it can get, 0x5586ea) are what its allocator draws from before
// it gives up with "Out of memory 28" (0x61e093).
struct GameWatch {
    const char* game;
    const char* module;
    uint64_t offset;          // From the module base.
    const char* label;
};
inline constexpr GameWatch game_watches[] = {
    {"Call of Duty: Modern Warfare Remastered", "eboot.bin", 0xeb24760, "pages libres réserve 0"},
    {"Call of Duty: Modern Warfare Remastered", "eboot.bin", 0xeb24764, "pages libres réserve 1"},
};
// Diagnostic traps: an error path replaced by UD2 (and NOPs), so the game
// stops there with its registers in the report instead of relaunching itself.
// CoD's "Out of memory 28" (0x61e093, LEA RDI of the message): its allocator's
// commit found no pages to map though its pools are full.
struct GameTrap {
    const char* game;
    const char* module;
    uint64_t offset;
    uint8_t expected[7];
    const char* label;
};
inline constexpr GameTrap game_traps[] = {
    {"Call of Duty: Modern Warfare Remastered", "eboot.bin", 0x61e093, {0x48, 0x8d, 0x3d, 0xb4, 0x73, 0x33, 0x00},
     "Out of memory 28"},
};
using GameWatchAddresses = std::vector<std::pair<std::string, uint64_t>>; // Label, guest address.

// Where moved bases go: this far into the window, and the window's top part
// from there on is the games' (PS4VirtualMemory::set_placement_end).
inline constexpr uint64_t game_virtual_offset = uint64_t(16) << 30;

// Applies the patches that match `image` and returns what was done, one line
// each ("" when nothing matched). `placement_end` gets the address where
// MaxPS4's own placements must stop (0: no patch applied).
inline std::string apply_game_patches(GuestELF& image, uint64_t window, uint64_t window_size, uint64_t& placement_end,
                                      GameWatchAddresses* watches = nullptr) {
    placement_end = 0;
    if (watches) watches->clear();
    std::string report;
    if (!window || window_size <= game_virtual_offset) return report;
    const uint64_t target = window + game_virtual_offset;
    for (const auto& patch : game_virtual_bases) {
        for (const auto& module : image.module_records()) {
            if (module.name != patch.module) continue;
            uint8_t expected[10], replacement[10];
            expected[0] = replacement[0] = patch.opcode[0];
            expected[1] = replacement[1] = patch.opcode[1];
            std::memcpy(expected + 2, &patch.ps4_base, 8);
            std::memcpy(replacement + 2, &target, 8);
            if (!image.patch(module.base + patch.offset, expected, replacement, sizeof(expected))) continue;
            placement_end = target;
            char line[200];
            std::snprintf(line, sizeof line, "[MaxPS4] %s : mémoire virtuelle du jeu 0x%llx -> 0x%llx (zone interdite par iOS)\n",
                          patch.game, (unsigned long long)patch.ps4_base, (unsigned long long)target);
            report += line;
            for (const auto& trap : game_traps) {
                if (std::strcmp(trap.game, patch.game)) continue;
                for (const auto& m : image.module_records()) {
                    if (m.name != trap.module) continue;
                    const uint8_t ud2[7] = {0x0f, 0x0b, 0x90, 0x90, 0x90, 0x90, 0x90};
                    const bool armed = image.patch(m.base + trap.offset, trap.expected, ud2, sizeof(ud2));
                    std::snprintf(line, sizeof line, "[MaxPS4] %s : piège de diagnostic sur « %s » à 0x%llx%s\n", patch.game,
                                  trap.label, (unsigned long long)trap.offset, armed ? "" : " (octets différents, non posé)");
                    report += line;
                }
            }
            if (watches)
                for (const auto& watch : game_watches)
                    if (!std::strcmp(watch.game, patch.game))
                        for (const auto& m : image.module_records())
                            if (m.name == watch.module) watches->push_back({watch.label, m.base + watch.offset});
        }
    }
    return report;
}
} // namespace maxps4
