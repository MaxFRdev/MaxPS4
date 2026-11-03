#include "../Native/GCNShaderRuntime.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>

namespace rt = maxps4::gpu::gcnrt;
using namespace maxps4::gpu;
namespace {
constexpr std::uint64_t textureBase = 0x14000, program = 0x10400;
constexpr unsigned width = 7, height = 9, pitch = 12, bytes = 72;
const std::uint32_t endpoints[] = {0x07e0f800,0xf80007e0,0xffffffff,0x42088410,0};
const float palettes[5][4][4] = {
    {{1,0,0,1},{0,1,0,1},{2.0f/3,1.0f/3,0,1},{1.0f/3,2.0f/3,0,1}},
    {{0,1,0,1},{1,0,0,1},{0.5f,0.5f,0,1},{0,0,0,0}},
    {{1,1,1,1},{1,1,1,1},{1,1,1,1},{0,0,0,0}},
    {{16.0f/31,32.0f/63,16.0f/31,1},{8.0f/31,16.0f/63,8.0f/31,1},
     {40.0f/93,80.0f/189,40.0f/93,1},{32.0f/93,64.0f/189,32.0f/93,1}},
    {{0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,0,0}},
};
rt::GCNRes8 descriptor() {
    return {{unsigned(textureBase >> 8),35u << 20,(width-1) | ((height-1) << 14),
             0x90800fac,(pitch-1) << 13,0,0,0}};
}
}
int main() {
    gcn_test::Arena arena;
    arena.code(program, {0xf0000f00,0x00000400,0xbf810000}); // image_load v[4:7], v[0:1], s[0:7]
    for (unsigned block = 0; block < 9; ++block) {
        arena.put(textureBase + block * 8, endpoints[block % 5]);
        arena.put(textureBase + block * 8 + 4, 0x934e39e4); // index (x+y) % 4
    }
    const auto texture = descriptor();
    rt::GCNMemory memory{arena.bytes.data(),gcn_test::Arena::start,arena.bytes.size()};
    for (bool cached : {false,true}) {
        GCNImageCache cache;
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            GCNWave wave(arena.memory,1);
            if (cached) wave.images = &cache;
            std::copy_n(texture.d,8,wave.s.begin()); wave.v(0,0) = x; wave.v(1,0) = y;
            std::uint64_t extent = 0;
            wave.resource = [&](std::uint64_t address,std::uint64_t size) {
                assert(address == textureBase); extent = size;
            };
            if (!wave.run(program)) std::fprintf(stderr,"%s\n",wave.error.c_str());
            assert(wave.error.empty() && extent == bytes);
            const auto translated = rt::gcn_image_load(memory,texture,x,y);
            const unsigned block = (y / 4) * 3 + x / 4, index = (x + y) % 4;
            for (unsigned c = 0; c < 4; ++c) {
                const float expected = palettes[block % 5][index][c];
                assert(std::fabs(gcn_detail::f32(wave.v(4+c,0)) - expected) < 0.000001f);
                assert(std::fabs(translated[c] - expected) < 0.000001f);
            }
        }
        if (cached) assert(cache.at(textureBase).size() == bytes);
    }
    // Cached blocks must not allocate or read the old uncompressed footprint.
    auto limited = arena.memory;
    limited.read = [&](std::uint64_t a,void* d,std::size_t n) {
        if (a >= textureBase && (a-textureBase > bytes || n > bytes-(a-textureBase))) return false;
        return arena.memory.read(a,d,n);
    };
    GCNWave valid(limited,1);
    std::copy_n(texture.d,8,valid.s.begin()); valid.v(0,0)=6; valid.v(1,0)=8;
    assert(valid.run(program));
    for (unsigned number : {7u,9u}) {
        GCNWave unsupported(arena.memory,1);
        std::copy_n(texture.d,8,unsupported.s.begin()); unsupported.s[1] |= number << 26;
        assert(!unsupported.run(program));
        assert(unsupported.error.find("BC1 requires UNORM and pitch >= width") == 0);
    }
    // Bilinear filtering interpolates decoded colours across a block boundary.
    rt::GCNRes4 sampler{{0x12,0,1u << 20,0}};
    const auto filtered = rt::gcn_sample(memory,texture,sampler,true,4.0f,0.5f);
    for (unsigned c=0;c<4;++c) {
        const float expected = (palettes[0][3][c] + palettes[1][0][c]) * 0.5f;
        assert(std::fabs(filtered[c]-expected) < 0.000001f);
    }
    std::puts("PASS BC1: independent palettes, RGB565 endpoints, transparency, block/padded pitch, partial dimensions, cache and filtering");
}
