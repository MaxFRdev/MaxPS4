#include "../Native/PM4SoftwareRenderer.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace maxps4::gpu;
using gcn_detail::u32;
namespace {
struct Arena {
    static constexpr std::uint64_t start = 0x10000;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x10000, 0);
    GCNMemory memory{
        [this](std::uint64_t a, void* d, std::size_t n) {
            if (a < start || a - start > bytes.size() || n > bytes.size() - (a - start)) return false;
            std::memcpy(d, bytes.data() + (a - start), n); return true; },
        [this](std::uint64_t a, const void* d, std::size_t n) {
            if (a < start || a - start > bytes.size() || n > bytes.size() - (a - start)) return false;
            std::memcpy(bytes.data() + (a - start), d, n); return true; }};
    void code(std::uint64_t a, std::initializer_list<std::uint32_t> words) {
        for (auto w : words) { std::memcpy(bytes.data() + (a - start), &w, 4); a += 4; }
    }
    std::uint8_t* at(std::uint64_t a) { return bytes.data() + (a - start); }
};
struct Stream {
    std::vector<std::uint32_t> words;
    void set(std::uint8_t opcode, std::uint32_t offset, std::initializer_list<std::uint32_t> values) {
        words.push_back(0xc0000000U | (std::uint32_t(values.size()) << 16) | (std::uint32_t(opcode) << 8));
        words.push_back(offset);
        words.insert(words.end(), values);
    }
    void context(std::uint32_t offset, std::initializer_list<std::uint32_t> v) { set(0x69, offset, v); }
    void shader(std::uint32_t offset, std::initializer_list<std::uint32_t> v) { set(0x76, offset, v); }
    void drawAuto(std::uint32_t count) { words.insert(words.end(), {0xc0012d00U, count, 2}); }
};
}
int main() {
    Arena arena;
    constexpr std::uint64_t vs = 0x10000, ps = 0x10100, target = 0x11000;
    arena.code(vs, {0xbeeb03ff, 7, 0x36020081, 0x34020281, 0x360000c2, 0x4a0202c1, 0x4a0000c1, 0x7e020b01,
                    0x7e000b00, 0x7e040280, 0x7e0602f2, 0xf80008cf, 0x03020001, 0xf800020f, 0x03030303, 0xbf810000});
    arena.code(ps, {
        0xc80c0000, 0xc80d0001, // v_interp_p1/p2_f32 v3, attr0.x (= 1.0)
        0x100804ff, 0x3d800000, // v_mul_f32 v4, 0.0625, v2 (pos_x / 16)
        0x7e0a0280,             // v_mov_b32 v5, 0
        0xf800180f, 0x03050504, // exp mrt0, v4, v5, v5, v3 done vm
        0xbf810000});
    Stream s;
    s.shader(0x48, {std::uint32_t(vs >> 8), 0, 0, 0});   // VS program, no user SGPRs
    s.shader(0x08, {std::uint32_t(ps >> 8), 0, 0, 0});   // PS program
    s.context(0x318, {std::uint32_t(target >> 8), 1, 1, 0, (0xaU << 2) | (1U << 11), 0}); // 16x8 BGRA8
    s.context(0x8e, {0xf});
    s.context(0x10f, {u32(8), u32(8), u32(-4), u32(4), u32(0.5f), u32(0.5f)});
    s.context(0x206, {0x43f});
    s.context(0x0c, {0, 16 | (8U << 16)});
    s.context(0x191, {0});                              // PS attribute 0 = VS parameter 0
    s.context(0x1b3, {0x102, 0x102});                   // PERSP_CENTER, POS_X: v0, v1 = i, j; v2 = x
    s.set(0x79, 0x242, {0x11});                         // rect list
    s.drawAuto(3);
    PM4SoftwareRenderer renderer(arena.memory);
    renderer.execute(s.words.data(), s.words.size());
    if (!renderer.stats.error.empty()) std::printf("%s\n", renderer.stats.error.c_str());
    assert(renderer.stats.draws == 1 && renderer.stats.rendered == 1 && renderer.stats.failed == 0);
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 16; ++x) {
        const std::uint8_t* p = arena.at(target + (y * 16 + x) * 4);
        const auto red = std::uint8_t(std::lround((float(x) + 0.5f) / 16 * 255));
        assert(p[0] == 0 && p[1] == 0 && p[2] == red && p[3] == 255); // B, G, R, A
    }
    // A narrower screen scissor leaves the other columns alone; every pixel is
    // written once, so the shared rectangle diagonal is not drawn twice.
    std::memset(arena.at(target), 0x11, 16 * 8 * 4);
    Stream narrow;
    narrow.context(0x0d, {4 | (8U << 16)});
    narrow.drawAuto(3);
    renderer.execute(narrow.words.data(), narrow.words.size());
    assert(renderer.stats.rendered == 2);
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 16; ++x)
        assert((arena.at(target + (y * 16 + x) * 4)[3] == 255) == (x < 4));
    // The same pixels must be produced through INDEX_BASE + DRAW_INDEX_OFFSET_2.
    const std::uint16_t indices[] = {0xffff, 0, 1, 2};
    std::memcpy(arena.at(0x10800), indices, sizeof(indices));
    std::memset(arena.at(target), 0, 16 * 8 * 4);
    Stream offset;
    offset.context(0x0d, {16 | (8U << 16)});
    offset.words.insert(offset.words.end(), {0xc0012600, 0x10800, 0, 0xc0033500, 3, 1, 3, 0});
    renderer.execute(offset.words.data(), offset.words.size());
    assert(renderer.stats.rendered == 3 && renderer.stats.failed == 0);
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 16; ++x) {
        const auto* p = arena.at(target + (y * 16 + x) * 4);
        const auto red = std::uint8_t(std::lround((float(x) + 0.5f) / 16 * 255));
        assert(p[0] == 0 && p[1] == 0 && p[2] == red && p[3] == 255);
    }
    // Target mask 0 skips the draw; an unsupported primitive is reported.
    Stream masked;
    masked.context(0x8e, {0});
    masked.drawAuto(3);
    masked.context(0x8e, {0xf});
    masked.set(0x79, 0x242, {0x2});
    masked.drawAuto(3);
    renderer.execute(masked.words.data(), masked.words.size());
    assert(renderer.stats.draws == 5 && renderer.stats.skipped == 1 && renderer.stats.failed == 1);
    assert(renderer.stats.error == "primitive type 2");
    std::puts("PASS PM4 software renderer: rect list, VS/PS programs, viewport, PS inputs, BGRA target, scissor, skips");
}
