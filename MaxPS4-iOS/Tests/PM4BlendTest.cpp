#include "../Native/PM4SoftwareRenderer.hpp"
#include "PM4BlendTestScene.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace pm4_blend_test;
    for (const auto& test : cases) {
        gcn_test::Arena arena;
        load(arena, test.source);
        const auto words = stream(test.control, test.mask, test.constant);
        maxps4::gpu::PM4SoftwareRenderer renderer(arena.memory);
        renderer.execute(words.data(), words.size());
        assert(renderer.stats.failed == 0 && renderer.stats.rendered == 2);
        constexpr unsigned bgra[] = {2, 1, 0, 3};
        for (unsigned pixel = 0; pixel < width * height; ++pixel) for (unsigned channel = 0; channel < 4; ++channel) {
            const unsigned actual = arena.bytes[target - gcn_test::Arena::start + pixel * 4 + bgra[channel]];
            // Float intermediates can put a half-byte tie on either side.
            const int difference = std::abs(int(actual) - int(test.expected[channel]));
            if (difference > 1)
                std::fprintf(stderr, "%s pixel %u channel %u: got %u, expected %u\n",
                             test.name, pixel, channel, actual, test.expected[channel]);
            assert(difference <= 1);
        }
    }
    for (const auto control : {enabled | (5u << 5), enabled | 15u,
                               enabled | separate | (7u << 21),
                               enabled | separate | (18u << 24), enabled | 11u, enabled | (12u << 8)}) {
        gcn_test::Arena arena;
        load(arena);
        const auto words = stream(control);
        maxps4::gpu::PM4SoftwareRenderer renderer(arena.memory);
        renderer.execute(words.data(), words.size());
        assert(renderer.stats.failed == 1 && renderer.stats.error == "blend mode" && renderer.stats.rendered == 1);
        for (unsigned pixel = 0; pixel < width * height; ++pixel) {
            const auto* color = arena.bytes.data() + target - gcn_test::Arena::start + pixel * 4;
            assert(color[0] == 128 && color[1] == 192 && color[2] == 64 && color[3] == 64);
        }
    }
    std::puts("PASS PM4 blending: add/subtract/reverse/min/max, separate alpha, masks and invalid-state rejection");
}
