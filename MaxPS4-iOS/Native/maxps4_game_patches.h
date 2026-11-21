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
#include <tuple>
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
    // After its pools' set-up (0x582fd8..0x583670), before it starts a
    // thread (0x599870): the SharePlay-time fault is before or after this.
    {"Call of Duty: Modern Warfare Remastered", "eboot.bin", 0x583672, {0x48, 0x8d, 0x3d, 0x97, 0xf7, 0x20, 0x00},
     "fin de l'initialisation des pools"},
};
// A game's whole fixed virtual layout, moved as one block by a multiple of
// 16 GiB (1 GiB if that does not fit) so offsets inside it keep their low
// bits. CoD lays out its GPU and streaming pools in [0x1000000000,
// 0x1a40200000) (41 GiB, its PRT aperture) at 0x558ae4..0x558c7a, every base
// a MOVABS; the guard iOS keeps over [0x1000000000, 0x7000000000) refused
// them all.
struct GameLayoutConstant { uint64_t offset; uint8_t opcode[2]; uint64_t value; };
struct GameLayout {
    const char* game;
    const char* module;
    uint64_t base, size;
    GameLayoutConstant constants[7];
};
inline constexpr GameLayout game_layouts[] = {
    {"Call of Duty: Modern Warfare Remastered", "eboot.bin", 0x1000000000ULL, 0xa40200000ULL, {
        {0x558ae4, {0x48, 0xbb}, 0x1000000000ULL}, {0x558b23, {0x48, 0xb8}, 0x1080000000ULL},
        {0x558b69, {0x48, 0xb8}, 0x1100000000ULL}, {0x558b73, {0x48, 0xb9}, 0x1180000000ULL},
        {0x558bc0, {0x48, 0xbb}, 0x1200000000ULL}, {0x558c2d, {0x48, 0xb9}, 0x1600200000ULL},
        {0x558c7a, {0x48, 0xb8}, 0x1a00200000ULL}}},
};
// With a layout moved too, the window holds, in order: MaxPS4's own
// placements (game_own_room), the moved virtual base and its regions, then
// the layout from game_layout_start on.
inline constexpr uint64_t game_own_room = uint64_t(3) << 30, game_layout_start = uint64_t(5) << 30;
// The window a run needs for a layout to fit, whatever the window's own
// alignment: asked of the host first (GuestELF::prepare_orbis_general).
inline constexpr uint64_t game_layout_window = uint64_t(58) << 30;

// Whether a module's file bytes hold a layout's first constant: such a game
// wants a game_layout_window window. `segments` yields (address, bytes, size).
template <class Segments>
inline bool game_needs_layout_window(const Segments& segments) {
    for (const auto& layout : game_layouts) {
        const auto& first = layout.constants[0];
        uint8_t expected[10] = {first.opcode[0], first.opcode[1]};
        std::memcpy(expected + 2, &first.value, 8);
        for (const auto& [address, bytes, size] : segments)
            if (first.offset >= address && first.offset - address + sizeof(expected) <= size &&
                !std::memcmp(bytes + (first.offset - address), expected, sizeof(expected))) return true;
    }
    return false;
}

using GameWatchAddresses = std::vector<std::pair<std::string, uint64_t>>; // Label, guest address.

// Where moved bases go: this far into the window, and the window's top part
// from there on is the games' (PS4VirtualMemory::set_placement_end).
inline constexpr uint64_t game_virtual_offset = uint64_t(16) << 30;
// The moved base keeps the alignment games assume of a PS4 address: the
// window is only 16 KiB aligned, and CoD rounds its regions to 64 KiB pages
// from the base (an unaligned base made its first commit 0 pages long, then
// "Out of memory 28"). 2 MiB, for large pages too.
inline constexpr uint64_t game_virtual_alignment = uint64_t(2) << 20;

// Applies the patches that match `image` and returns what was done, one line
// each ("" when nothing matched). `placement_end` gets the address where
// MaxPS4's own placements must stop (0: no patch applied).
inline std::string apply_game_patches(GuestELF& image, uint64_t window, uint64_t window_size, uint64_t& placement_end,
                                      GameWatchAddresses* watches = nullptr) {
    placement_end = 0;
    if (watches) watches->clear();
    std::string report;
    if (!window || window_size <= game_virtual_offset + game_virtual_alignment) return report;
    auto movabs = [](uint8_t out[10], const uint8_t opcode[2], uint64_t value) {
        out[0] = opcode[0]; out[1] = opcode[1]; std::memcpy(out + 2, &value, 8);
    };
    char line[240];
    for (const auto& patch : game_virtual_bases) {
        for (const auto& module : image.module_records()) {
            if (module.name != patch.module) continue;
            uint8_t expected[10], replacement[10];
            movabs(expected, patch.opcode, patch.ps4_base);
            // Patching a value with itself: only checks the bytes.
            if (!image.patch(module.base + patch.offset, expected, expected, sizeof(expected))) continue;
            // The game's fixed layout, when it has one, all of its constants
            // match and it fits the window: moved after the virtual base.
            const GameLayout* moved_layout = nullptr;
            uint64_t layout_target = 0;
            for (const auto& layout : game_layouts) {
                if (std::strcmp(layout.game, patch.game) || layout.module != module.name) continue;
                bool matches = true;
                for (const auto& constant : layout.constants) {
                    uint8_t bytes[10];
                    movabs(bytes, constant.opcode, constant.value);
                    matches = matches && image.patch(module.base + constant.offset, bytes, bytes, sizeof(bytes));
                }
                for (const uint64_t alignment : {uint64_t(16) << 30, uint64_t(1) << 30}) {
                    const uint64_t at = (window + game_layout_start + alignment - 1) & ~(alignment - 1);
                    if (matches && at + layout.size <= window + window_size) { layout_target = at; break; }
                }
                if (!matches) continue;
                if (!layout_target) {
                    std::snprintf(line, sizeof line, "[MaxPS4] %s : disposition fixe 0x%llx (+%llu Mio) non déplacée, fenêtre trop petite (%llu Mio)\n",
                                  patch.game, (unsigned long long)layout.base, (unsigned long long)(layout.size >> 20),
                                  (unsigned long long)(window_size >> 20));
                    report += line;
                    continue;
                }
                moved_layout = &layout;
            }
            const uint64_t offset = moved_layout ? game_own_room : game_virtual_offset;
            const uint64_t target = (window + offset + game_virtual_alignment - 1) & ~(game_virtual_alignment - 1);
            movabs(replacement, patch.opcode, target);
            if (!image.patch(module.base + patch.offset, expected, replacement, sizeof(expected))) continue;
            placement_end = target;
            std::snprintf(line, sizeof line, "[MaxPS4] %s : mémoire virtuelle du jeu 0x%llx -> 0x%llx (zone interdite par iOS)\n",
                          patch.game, (unsigned long long)patch.ps4_base, (unsigned long long)target);
            report += line;
            if (moved_layout) {
                const uint64_t delta = layout_target - moved_layout->base;
                for (const auto& constant : moved_layout->constants) {
                    uint8_t from[10], to[10];
                    movabs(from, constant.opcode, constant.value);
                    movabs(to, constant.opcode, constant.value + delta);
                    (void)image.patch(module.base + constant.offset, from, to, sizeof(from));
                }
                std::snprintf(line, sizeof line, "[MaxPS4] %s : disposition fixe 0x%llx-0x%llx -> 0x%llx-0x%llx\n", patch.game,
                              (unsigned long long)moved_layout->base, (unsigned long long)(moved_layout->base + moved_layout->size),
                              (unsigned long long)layout_target, (unsigned long long)(layout_target + moved_layout->size));
                report += line;
            }
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
