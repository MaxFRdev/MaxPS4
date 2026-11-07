#include "../Native/GCNInterpreter.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace maxps4::gpu;
using gcn_detail::f32; using gcn_detail::u32;
namespace {
// A flat guest window at [0x10000, 0x20000).
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
    void put(std::uint64_t a, std::uint32_t value) { std::memcpy(bytes.data() + (a - start), &value, 4); }
    void put16(std::uint64_t a, std::uint16_t value) { std::memcpy(bytes.data() + (a - start), &value, 2); }
    void code(std::uint64_t a, std::initializer_list<std::uint32_t> words) { for (auto w : words) { put(a, w); a += 4; } }
};
bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
}
int main() {
    for (const bool bulk : {true, false}) {
        Arena arena;
        for (unsigned k = 0; k < 128; ++k) arena.put(Arena::start + k * 4, 0xbf800000); // s_nop
        arena.put(Arena::start + 128 * 4, 0xbf810000); // s_endpgm
        unsigned calls = 0;
        GCNMemory observed{[&](std::uint64_t address, void* destination, std::size_t bytes) {
            ++calls;
            return (bulk || bytes == 4) && arena.memory.read(address, destination, bytes);
        }, {}};
        GCNWave wave(observed, 1);
        assert(wave.run(Arena::start));
        assert(calls == 129 * (bulk ? 1u : 4u));
    }
    {
        const std::uint32_t end = 0xbf810000;
        GCNMemory edge{[&](std::uint64_t address, void* destination, std::size_t bytes) {
            if (address != 0x10000 || bytes != 4) return false;
            std::memcpy(destination, &end, 4); return true;
        }, {}};
        GCNWave wave(edge, 1);
        assert(wave.run(0x10000));
    }
    std::puts("PASS GCN fetch: 129 instruction windows use 129 reads, bulk refusal and mapping-edge fallback preserved");
    // 1. shadPS4's embedded full-screen VS, one lane per vertex id.
    {
        Arena arena;
        arena.code(0x10000, {0xbeeb03ff, 7, 0x36020081, 0x34020281, 0x360000c2, 0x4a0202c1, 0x4a0000c1, 0x7e020b01,
                             0x7e000b00, 0x7e040280, 0x7e0602f2, 0xf80008cf, 0x03020001, 0xf800020f, 0x03030303, 0xbf810000});
        GCNWave wave(arena.memory, 3);
        for (unsigned lane = 0; lane < 3; ++lane) wave.v(0, lane) = lane;
        assert(wave.run(0x10000));
        assert(wave.exports.size() == 2 && wave.exports[0].target == 12 && wave.exports[0].done && wave.exports[1].target == 32);
        const float x[] = {-1, 1, -1}, y[] = {-1, -1, 1};
        for (unsigned lane = 0; lane < 3; ++lane) {
            assert(f32(wave.exports[0].value[0][lane]) == x[lane] && f32(wave.exports[0].value[1][lane]) == y[lane]);
            assert(f32(wave.exports[0].value[2][lane]) == 0 && f32(wave.exports[0].value[3][lane]) == 1);
            assert(f32(wave.exports[1].value[0][lane]) == 1);
        }
        // The last v_add_i32 (-1 + (vid & ~1)) carries out only where vid & ~1 is nonzero.
        assert(wave.vccMask() == 0x4);
    }
    // 2. A pixel quad: interpolation, constant buffer, bilinear RGB565 sample,
    //    quad derivative, VCC branch, WQM helper lane and compressed export.
    {
        Arena arena;
        arena.code(0x10000, {
            0xbe94047e,             // s_mov_b64 s[20:21], exec
            0xbefe0a7e,             // s_wqm_b64 exec, exec
            0xc2080d00,             // s_buffer_load_dword s16, s[12:15], 0x0
            0xc8080000, 0xc8090001, // v_interp_p1/p2_f32 v2, attr0.x
            0xc80c0100, 0xc80d0101, // v_interp_p1/p2_f32 v3, attr0.y
            0xbf8c007f,             // s_waitcnt lgkmcnt(0)
            0xf0800f00, 0x00400402, // image_sample v[4:7], v2, s[0:7], s[8:11] dmask:0xf
            0xbf8c1f70,             // s_waitcnt vmcnt(0)
            0x10080810,             // v_mul_f32 v4, s16, v4
            0xd8d48055, 0x08000002, // ds_swizzle_b32 v8, v2 (quad lane 1)
            0xd8d48000, 0x09000002, // ds_swizzle_b32 v9, v2 (quad lane 0)
            0x08101308,             // v_sub_f32 v8, v8, v9: ddx(u)
            0x7c0204f0,             // v_cmp_lt_f32 vcc, 0.5, v2
            0xbf860001,             // s_cbranch_vccz +1
            0x7e0e0280,             // v_mov_b32 v7, 0 (skipped)
            0xbefe0414,             // s_mov_b64 exec, s[20:21]
            0x5e000b04, 0x5e020f06, // v_cvt_pkrtz_f16_f32 v0, v4, v5 / v1, v6, v7
            0xf8000011, 0x08080808, // exp mrt1, v8
            0xf8001c0f, 0x00000100, // exp mrt0, v0, v1, v0, v0 compr done vm
            0xbf810000});
        // 4x2 RGB565 texture (red, green, blue, white per row), linear at 0x12000.
        const std::uint16_t texels[] = {0xf800, 0x07e0, 0x001f, 0xffff};
        for (unsigned row = 0; row < 2; ++row) for (unsigned x = 0; x < 4; ++x) arena.put16(0x12000 + 2 * (row * 4 + x), texels[x]);
        arena.put(0x13000, u32(2.0f));
        GCNWave wave(arena.memory, 4);
        GCNImageCache images; // texels come from one copy of the surface
        wave.images = &images;
        const std::uint32_t tsharp[8] = {0x120, 0x01000000, 0x4003, 0x9080032e, 0x6000, 0, 0, 0};
        const std::uint32_t ssharp[4] = {0x12, 0, 0x500000, 0};
        // As Sonic Mania's constant buffers: STRIDE 16, one record (16 bytes).
        const std::uint32_t vsharp[4] = {0x13000, 16u << 16, 1, 0};
        for (unsigned n = 0; n < 8; ++n) wave.s[n] = tsharp[n];
        for (unsigned n = 0; n < 4; ++n) { wave.s[8 + n] = ssharp[n]; wave.s[12 + n] = vsharp[n]; }
        wave.setExec(0x7); // lane 3 is a helper lane
        const float i[] = {0, 0.25f, 0, 0.25f};
        for (unsigned lane = 0; lane < 4; ++lane) { wave.v(0, lane) = u32(i[lane]); wave.v(1, lane) = 0; }
        wave.attribute = [](unsigned attr, unsigned chan) {
            assert(attr == 0);
            return chan == 0 ? GCNAttributePlane{0.25f, 0.5f, 0} : GCNAttributePlane{0.5f, 0, 0};
        };
        if (!wave.run(0x10000)) { std::printf("%s\n", wave.error.c_str()); assert(false); }
        assert(wave.exports.size() == 2 && wave.exports[1].compressed && wave.exports[1].done);
        assert(images.size() == 1 && images.begin()->first == 0x12000 && images.begin()->second.size() == 4 * 2 * 2);
        assert(wave.exports[1].mask == 0x7 && wave.exports[0].mask == 0x7 && wave.exec() == 0x7);
        for (unsigned lane = 0; lane < 4; ++lane) assert(near(f32(wave.exports[0].value[0][lane]), 0.125f));
        // u = 0.25: halfway between red and green, red doubled to 1.0; u = 0.375: green.
        assert(wave.exports[1].value[0][0] == 0x38003c00 && wave.exports[1].value[1][0] == 0x3c000000);
        assert(wave.exports[1].value[0][1] == 0x3c000000 && wave.exports[1].value[1][1] == 0x3c000000);
        assert(gcn_detail::f16(0x3800) == 0.5f && gcn_detail::f16Rtz(0.5f) == 0x3800 && gcn_detail::f16Rtz(1e6f) == 0x7bff);
        // A 2D-tiled texture (index 14) is refused, not read as linear.
        arena.put(0x12000 - 0x2000, 0); // unrelated write keeps the arena in use
        GCNWave tiled(arena.memory, 4);
        for (unsigned n = 0; n < 8; ++n) tiled.s[n] = tsharp[n];
        for (unsigned n = 0; n < 4; ++n) { tiled.s[8 + n] = ssharp[n]; tiled.s[12 + n] = vsharp[n]; }
        tiled.s[3] = 0x90e0032e;
        tiled.attribute = wave.attribute;
        assert(!tiled.run(0x10000) && tiled.error.find("tiled image (tile mode 14)") == 0);
    }
    // 3. A VS calling a fetch shader (s_swappc/s_setpc), a scalar loop and VOP3.
    {
        Arena arena;
        arena.code(0x10400, {
            0xbe802100,             // s_swappc_b64 s[0:1], s[0:1]
            0xbe840380,             // s_mov_b32 s4, 0
            0x80048304,             // s_add_u32 s4, s4, 3
            0xbf0a8904,             // s_cmp_lt_u32 s4, 9
            0xbf85fffd,             // s_cbranch_scc1 -3
            0x7e100c04,             // v_cvt_f32_u32 v8, s4
            0xd2820009, 0x04161104, // v_mad_f32 v9, v4, v8, v5
            0xf80008cf, 0x07060504, // exp pos0, v4, v5, v6, v7 done
            0xf800020f, 0x09090909, // exp param0, v9, v9, v9, v9
            0xbf810000});
        arena.code(0x10600, {
            0xc0840300,             // s_load_dwordx4 s[8:11], s[2:3], 0x0
            0xbf8c007f,             // s_waitcnt lgkmcnt(0)
            0xe00c2000, 0x80020400, // buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
            0xbf8c1f70,             // s_waitcnt vmcnt(0)
            0xbe802000});           // s_setpc_b64 s[0:1]
        arena.code(0x10800, {0x10900, 16u << 16, 3, 0x77fac}); // V#: stride 16, 3 records, float xyzw
        const float vertices[3][4] = {{1, 2, 0, 1}, {3, 4, 0, 1}, {5, 6, 0, 1}};
        for (unsigned n = 0; n < 3; ++n) for (unsigned c = 0; c < 4; ++c) arena.put(0x10900 + 16 * n + 4 * c, u32(vertices[n][c]));
        GCNCodeCache cache;
        GCNWave wave(arena.memory, 4, &cache);
        wave.s[0] = 0x10600; wave.s[2] = 0x10800;
        for (unsigned lane = 0; lane < 4; ++lane) wave.v(0, lane) = lane; // lane 3 is out of range
        if (!wave.run(0x10400)) { std::printf("%s\n", wave.error.c_str()); assert(false); }
        assert(wave.s[4] == 9 && !cache.empty());
        for (unsigned lane = 0; lane < 3; ++lane) {
            for (unsigned c = 0; c < 4; ++c) assert(f32(wave.exports[0].value[c][lane]) == vertices[lane][c]);
            assert(f32(wave.exports[1].value[0][lane]) == vertices[lane][0] * 9 + vertices[lane][1]);
        }
        for (unsigned c = 0; c < 4; ++c) assert(wave.exports[0].value[c][3] == 0);
        // Unmapped code fails with a message instead of running garbage.
        GCNWave lost(arena.memory, 1);
        assert(!lost.run(0x30000) && lost.error == "unreadable code");
    }
    std::puts("PASS GCN interpreter: embedded VS, quad PS (interp, sample, swizzle, WQM, branch, compressed export), fetch-shader VS");
}
