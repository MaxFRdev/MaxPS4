#include "../Native/GCNShaderRuntime.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
namespace rt = maxps4::gpu::gcnrt;
int main() {
    unsigned cases = 0;
    constexpr rt::ulong base = 0x100000;
    const float coordinates[] = {-1.25f, -0.01f, 0.0f, 0.37f, 0.5f, 0.75f, 0.99f, 1.25f};
    for (const unsigned format : {1u,2u,3u,4u,10u,14u,16u,17u,18u,19u,35u,36u,37u,38u,39u}) {
        const unsigned bytes = (format == 35 || format == 38) ? 8 : format >= 36 ? 16 : format == 1 ? 1 : (format == 2 || format == 3 || format >= 16) ? 2 : format == 14 ? 16 : 4;
        constexpr unsigned width = 3, height = 2, pitch = 5;
        const unsigned span = format >= 35 ? (((height+3)/4-1)*((pitch+3)/4)+(width+3)/4)*bytes :
                                             ((height - 1) * pitch + width) * bytes;
        for (unsigned layout = 0; layout < 5; ++layout) {
            // The last layout omits the last byte physically as well as logically,
            // so ASan catches an unchecked access on the fallback path.
            std::vector<unsigned char> data(32 + span - (layout == 3 ? 1 : 0));
            for (std::size_t n = 0; n < data.size(); ++n) data[n] = static_cast<unsigned char>(n * 37 + 11);
            rt::ulong ranges[6] = {base - 256, 16, 0, base, span, 32};
            rt::GCNMemory memory{data.data() + 32, base, span};
            if (layout == 1) memory = {data.data(), 0, 0, ranges, 2};
            if (layout == 2) {
                ranges[0] = base; ranges[1] = span / 2; ranges[2] = 32;
                ranges[3] = base + span / 2; ranges[4] = span - span / 2; ranges[5] = 32 + span / 2;
                memory = {data.data(), 0, 0, ranges, 2};
            }
            if (layout == 3) { ranges[4] = span - 1; memory = {data.data(), 0, 0, ranges, 2}; }
            if (layout == 4) memory = {data.data(), base + span, 0};
            for (unsigned number : {0u, 1u, 7u}) for (unsigned swizzle : {0xfacu, 0x977u, 0x249u, 0x688u}) {
                rt::GCNRes8 texture{{unsigned(base >> 8), (format << 20) | (number << 26),
                    (width - 1) | ((height - 1) << 14), (8u << 20) | swizzle, (pitch - 1) << 13, 0, 0, 0}};
                for (unsigned mode = 0; mode < 8; ++mode) for (unsigned linear : {0u, 1u}) for (bool unorm : {false, true}) {
                    rt::GCNRes4 sampler{{mode | (((mode + 1) % 8) << 3), 0, linear << 20, (mode % 3) << 30}};
                    for (float u : coordinates) for (float v : coordinates) {
                        const auto optimized = rt::gcn_sample(memory, texture, sampler, unorm, u, v);
                        const auto checked = rt::gcn_sample_at(memory, texture, sampler, unorm, u, v, false, 0);
                        for (unsigned c = 0; c < 4; ++c)
                            assert(rt::gu(optimized[c]) == rt::gu(checked[c]) || (std::isnan(optimized[c]) && std::isnan(checked[c])));
                        ++cases;
                    }
                }
                // Unsupported tiling still returns zero before any unchecked read.
                texture.d[3] &= ~(31u << 20);
                const auto tiled = rt::gcn_sample(memory, texture, rt::GCNRes4{}, false, 0, 0);
                for (unsigned c = 0; c < 4; ++c) assert(tiled[c] == 0);
            }
        }
    }
    std::printf("PASS texture footprint: %u exact comparisons, padded pitch, formats, filters, wrap/border, packed/split/truncated/missing ranges\n", cases);
}
