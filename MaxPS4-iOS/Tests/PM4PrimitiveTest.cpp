#include "../Native/PM4SoftwareRenderer.hpp"
#include "PM4PrimitiveTestScene.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace pm4_primitive_test;
    for (const unsigned indexBytes : {0u, 2u, 4u}) {
        gcn_test::Arena arena;
        const auto words = scene(arena, 5, indexBytes);
        maxps4::gpu::PM4SoftwareRenderer renderer(arena.memory);
        renderer.execute(words.data(), words.size());
        assert(renderer.stats.failed == 0 && renderer.stats.rendered == 1);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const auto* pixel = arena.bytes.data() + target - gcn_test::Arena::start + 4 * (y * width + x);
            const unsigned bgra[] = {16, 32, 64, 64};
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned expected = x < 8 ? bgra[c] : 0;
                if (pixel[c] != expected) std::fprintf(stderr, "fan index width %u at %u,%u channel %u: %u != %u\n",
                    indexBytes, x, y, c, unsigned(pixel[c]), expected);
                assert(pixel[c] == expected);
            }
        }
    }
    for (const unsigned primitive : {5u, 6u}) for (const unsigned indexBytes : {2u, 4u}) {
        gcn_test::Arena arena;
        const auto words = restartScene(arena, primitive, indexBytes);
        maxps4::gpu::PM4SoftwareRenderer renderer(arena.memory);
        renderer.execute(words.data(), words.size());
        assert(renderer.stats.failed == 0 && renderer.stats.rendered == 1);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            const auto* pixel = arena.bytes.data() + target - gcn_test::Arena::start + 4 * (y * width + x);
            const unsigned bgra[] = {16,32,64,64};
            for (unsigned c = 0; c < 4; ++c) assert(pixel[c] == (x < 4 || x >= 12 ? bgra[c] : 0));
        }
    }
    std::puts("PASS PM4 triangle fans: auto/16/32-bit offset draws, winding and single coverage of shared edges");
}
