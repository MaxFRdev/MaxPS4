#pragma once
#include "GCNTestPrograms.h"

namespace pm4_primitive_test {
constexpr std::uint64_t target = 0x18000, pixelPS = 0x10400, indexBuffer = 0x15000;
constexpr unsigned width = 16, height = 8;
inline std::vector<std::uint32_t> scene(gcn_test::Arena& arena, unsigned primitive, unsigned indexBytes) {
    using namespace gcn_test;
    loadFetchVS(arena);
    arena.put(vertexTable + 8, 4); // Four vertex records.
    const float vertices[4][4] = {{-1,-1,0,1}, {0,-1,0,1}, {0,1,0,1}, {-1,1,0,1}};
    for (unsigned k = 0; k < 4; ++k) for (unsigned c = 0; c < 4; ++c)
        arena.put(0x10e00 + k * 16 + c * 4, bits(vertices[k][c]));
    arena.code(pixelPS, {0x7e0802ff, bits(0.25f), 0x7e0a02ff, bits(0.125f),
        0x7e0c02ff, bits(0.0625f), 0x7e0e02ff, bits(0.25f), 0xf800180f, 0x07060504, 0xbf810000});
    std::vector<std::uint32_t> words;
    auto set = [&](unsigned opcode, unsigned offset, std::initializer_list<std::uint32_t> values) {
        words.push_back(0xc0000000u | (std::uint32_t(values.size()) << 16) | (opcode << 8));
        words.push_back(offset); words.insert(words.end(), values);
    };
    set(0x76, 0x48, {std::uint32_t(fetchVS >> 8), 0, 0, 4u << 1});
    set(0x76, 0x4c, {std::uint32_t(fetchShader), 0, std::uint32_t(vertexTable), 0});
    set(0x76, 0x08, {std::uint32_t(pixelPS >> 8), 0, 0, 0});
    set(0x69, 0x318, {std::uint32_t(target >> 8), 1, 1, 0, (0xa << 2) | (1 << 11), 0});
    set(0x69, 0x8e, {15});
    set(0x69, 0x10f, {bits(8), bits(8), bits(-4), bits(4), bits(0.5f), bits(0.5f)});
    set(0x69, 0x206, {0x43f});
    set(0x69, 0x0c, {0, width | (height << 16)});
    set(0x69, 0x1e0, {(1u << 30) | 1 | (1 << 8)}); // Additive: overlapping triangles are observable.
    set(0x79, 0x242, {primitive});
    if (!indexBytes) words.insert(words.end(), {0xc0012d00, 4, 2});
    else {
        // Skip a junk first index, then rotate or reverse the quad's winding.
        const unsigned indices16[] = {0xffff, 2, 3, 0, 1}, indices32[] = {0xffffffff, 3, 2, 1, 0};
        for (unsigned k = 0; k < 5; ++k) {
            if (indexBytes == 2) arena.put16(indexBuffer + k * 2, std::uint16_t(indices16[k]));
            else arena.put(indexBuffer + k * 4, indices32[k]);
        }
        words.insert(words.end(), {0xc0002a00, indexBytes == 4 ? 1u : 0u,
            0xc0012600, std::uint32_t(indexBuffer), 0, 0xc0033500, 4, 1, 4, 0});
    }
    return words;
}
inline std::vector<std::uint32_t> restartScene(gcn_test::Arena& arena, unsigned primitive, unsigned indexBytes) {
    using namespace gcn_test;
    auto words = scene(arena, primitive, indexBytes);
    const float vertices[9][4] = {
        {-1,-1,0,1}, {-0.5f,-1,0,1}, {-0.5f,1,0,1}, {-1,1,0,1},
        {0.5f,-1,0,1}, {1,-1,0,1}, {1,1,0,1}, {0.5f,1,0,1}, {0,0,0,1},
    };
    arena.put(vertexTable + 8, 9);
    for (unsigned k = 0; k < 9; ++k) for (unsigned c = 0; c < 4; ++c)
        arena.put(0x10e00 + k * 16 + c * 4, bits(vertices[k][c]));
    // Marker 8 points to a real central vertex: failing to split produces visible bridges.
    const unsigned fan[] = {8,0,1,2,3,8,8,4,5,6,7,8};
    const unsigned strip[] = {8,0,1,3,2,8,8,4,5,7,6,8};
    const auto* indices = primitive == 5 ? fan : strip;
    for (unsigned k = 0; k < 12; ++k) {
        if (indexBytes == 2) arena.put16(indexBuffer + (k + 1) * 2, std::uint16_t(indices[k]));
        else arena.put(indexBuffer + (k + 1) * 4, indices[k]);
    }
    words.resize(words.size() - 5);
    words.insert(words.end(), {0xc0016900, 0x103, 8, 0xc0016900, 0x2a5, 1,
                               0xc0033500, 12, 1, 12, 0});
    return words;
}
} // namespace pm4_primitive_test
