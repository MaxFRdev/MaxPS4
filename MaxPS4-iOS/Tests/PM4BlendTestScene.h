#pragma once
#include "GCNTestPrograms.h"
#include <limits>

namespace pm4_blend_test {
constexpr std::uint64_t target = 0x18000, sourcePS = 0x10400, backgroundPS = 0x10500;
constexpr unsigned width = 16, height = 8;
constexpr std::uint32_t enabled = 1u << 30, separate = 1u << 29;
struct Case {
    const char* name;
    std::uint32_t control;
    std::array<unsigned, 4> expected; // RGBA bytes, independent of either renderer.
    unsigned mask = 15;
    std::array<float, 4> source = {0.75f, 0.25f, 0.5f, 0.5f};
    std::array<float, 4> constant = {0.25f, 0.5f, 0.75f, 0.25f};
};
inline const Case cases[] = {
    {"add", enabled | 1 | (1 << 8), {255, 255, 255, 192}},
    {"source alpha", enabled | 4 | (5 << 8), {128, 128, 128, 96}},
    {"subtract", enabled | 1 | (1 << 5) | (1 << 8), {127, 0, 0, 64}},
    {"reverse subtract", enabled | 1 | (4 << 5) | (1 << 8), {0, 128, 1, 0}},
    {"min with zero factors", enabled | (2 << 5), {64, 64, 128, 64}},
    {"max with zero factors", enabled | (3 << 5), {191, 192, 128, 128}},
    {"min ignores unused factors", enabled | 31 | (2 << 5) | (31 << 8), {64, 64, 128, 64}},
    {"separate alpha add", enabled | separate | 1 | (1 << 5) | (1 << 8) | (1 << 16) | (1 << 24),
        {127, 0, 0, 192}},
    {"separate alpha reverse", enabled | separate | 1 | (1 << 8) | (1 << 16) | (4 << 21) | (1 << 24),
        {255, 255, 255, 0}},
    {"unused alpha fields", enabled | 1 | (1 << 8) | (31 << 16) | (7 << 21) | (31 << 24),
        {255, 255, 255, 192}},
    {"disabled blend", (7 << 5) | 31 | (31 << 8), {191, 64, 128, 128}},
    {"red and alpha mask", enabled | 1 | (1 << 8), {255, 192, 128, 192}, 9},
    {"UNORM source clamping", enabled | 4, {255, 0, 128, 255}, 15, {1.5f, -0.5f, 0.5f, 1.25f}},
    {"constant colour", enabled | 13, {48, 32, 96, 32}},
    {"inverse constant colour", enabled | 14, {143, 32, 32, 96}},
    {"constant alpha", enabled | 19, {48, 16, 32, 32}},
    {"inverse constant alpha", enabled | 20, {143, 48, 96, 96}},
    {"destination constant colour", enabled | (13 << 8), {16, 96, 96, 16}},
    {"destination constant alpha", enabled | (19 << 8), {16, 48, 32, 16}},
    {"constant crossfade", enabled | 13 | (14 << 8), {96, 128, 128, 80}},
    {"alpha saturation", enabled | 10, {96, 32, 64, 128}},
    {"alpha saturation destination limit", enabled | 10, {143, 48, 96, 230}, 15, {0.75f,0.25f,0.5f,0.9f}},
    {"constant zero", enabled | 13, {0,0,0,0}, 15, {0.75f,0.25f,0.5f,0.5f}, {0,0,0,0}},
    {"constant clamp and NaN", enabled | 13, {0,64,0,128}, 15, {0.75f,0.25f,0.5f,0.5f},
        {-1,2,std::numeric_limits<float>::quiet_NaN(),1.5f}},
};
inline void load(gcn_test::Arena& arena, std::array<float, 4> source = {0.75f, 0.25f, 0.5f, 0.5f}) {
    gcn_test::loadEmbeddedVS(arena);
    auto constant = [&](std::uint64_t address, float r, float g, float b, float a) {
        arena.code(address, {0x7e0802ff, gcn_test::bits(r), 0x7e0a02ff, gcn_test::bits(g),
            0x7e0c02ff, gcn_test::bits(b), 0x7e0e02ff, gcn_test::bits(a),
            0xf800180f, 0x07060504, 0xbf810000});
    };
    constant(sourcePS, source[0], source[1], source[2], source[3]);
    constant(backgroundPS, 64.0f / 255, 192.0f / 255, 128.0f / 255, 64.0f / 255);
}
inline std::vector<std::uint32_t> stream(std::uint32_t control, unsigned mask = 15,
                                       std::array<float, 4> constant = {0.25f,0.5f,0.75f,0.25f}) {
    std::vector<std::uint32_t> words;
    auto set = [&](unsigned opcode, unsigned offset, std::initializer_list<std::uint32_t> values) {
        words.push_back(0xc0000000u | (std::uint32_t(values.size()) << 16) | (opcode << 8));
        words.push_back(offset); words.insert(words.end(), values);
    };
    set(0x76, 0x48, {std::uint32_t(gcn_test::embeddedVS >> 8), 0, 0, 0});
    set(0x76, 0x08, {std::uint32_t(backgroundPS >> 8), 0, 0, 0});
    set(0x69, 0x318, {std::uint32_t(target >> 8), 1, 1, 0, (0xa << 2) | (1 << 11), 0});
    set(0x69, 0x8e, {15});
    set(0x69, 0x1e0, {0}); // The background must also overwrite on a persistent collector.
    set(0x69, 0x10f, {gcn_test::bits(8), gcn_test::bits(8), gcn_test::bits(-4), gcn_test::bits(4),
                      gcn_test::bits(0.5f), gcn_test::bits(0.5f)});
    set(0x69, 0x206, {0x43f});
    set(0x69, 0x0c, {0, width | (height << 16)});
    set(0x79, 0x242, {0x11});
    words.insert(words.end(), {0xc0012d00, 3, 2});
    set(0x76, 0x08, {std::uint32_t(sourcePS >> 8), 0, 0, 0});
    set(0x69, 0x1e0, {control});
    set(0x69, 0x105, {gcn_test::bits(constant[0]), gcn_test::bits(constant[1]),
                       gcn_test::bits(constant[2]), gcn_test::bits(constant[3])});
    set(0x69, 0x8e, {mask});
    words.insert(words.end(), {0xc0012d00, 3, 2});
    return words;
}
} // namespace pm4_blend_test
