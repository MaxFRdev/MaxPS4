#pragma once
// Hand-assembled GCN programs and their guest memory, shared by the
// translator test (stage 1), its C++ harness (stage 2) and the Metal test.
#include "../Native/GCNInterpreter.hpp"
#include <cstring>
#include <initializer_list>
#include <vector>

namespace gcn_test {
using namespace maxps4::gpu;
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
    Arena() = default;
    Arena(const Arena&) = delete;
    void put(std::uint64_t a, std::uint32_t value) { std::memcpy(bytes.data() + (a - start), &value, 4); }
    void put16(std::uint64_t a, std::uint16_t value) { std::memcpy(bytes.data() + (a - start), &value, 2); }
    void code(std::uint64_t a, std::initializer_list<std::uint32_t> words) { for (auto w : words) { put(a, w); a += 4; } }
};
inline std::uint32_t bits(float f) { std::uint32_t v; std::memcpy(&v, &f, 4); return v; }

// shadPS4's embedded full-screen VS (one lane per vertex id).
constexpr std::uint64_t embeddedVS = 0x10000;
inline void loadEmbeddedVS(Arena& arena) {
    arena.code(embeddedVS, {0xbeeb03ff, 7, 0x36020081, 0x34020281, 0x360000c2, 0x4a0202c1, 0x4a0000c1, 0x7e020b01,
                            0x7e000b00, 0x7e040280, 0x7e0602f2, 0xf80008cf, 0x03020001, 0xf800020f, 0x03030303, 0xbf810000});
}

// A pixel quad program: interpolation, strided constant buffer, bilinear
// RGB565 sample, quad derivative, VCC branch, WQM and compressed export.
constexpr std::uint64_t quadPS = 0x10400;
inline const std::uint32_t quadSgprs[16] = {
    0x120, 0x01000000, 0x4003, 0x9080032e, 0x6000, 0, 0, 0, // T#: 4x2 RGB565, linear
    0x12, 0, 0x500000, 0,                                   // S#: clamp, bilinear
    0x13000, 16u << 16, 1, 0};                              // V#: stride 16, 1 record
inline const float quadI[4] = {0, 0.25f, 0, 0.25f};
inline const GCNAttributePlane quadPlanes[2] = {{0.25f, 0.5f, 0}, {0.5f, 0, 0}}; // attr0.x, attr0.y
inline void loadQuadPS(Arena& arena) {
    arena.code(quadPS, {
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
    const std::uint16_t texels[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    for (unsigned row = 0; row < 2; ++row) for (unsigned x = 0; x < 4; ++x) arena.put16(0x12000 + 2 * (row * 4 + x), texels[x]);
    arena.put(0x13000, bits(2.0f));
}

// A VS calling a fetch shader (s_swappc/s_setpc), a scalar loop and VOP3.
constexpr std::uint64_t fetchVS = 0x10800, fetchShader = 0x10a00, vertexTable = 0x10c00;
inline const float fetchVertices[3][4] = {{1, 2, 0, 1}, {3, 4, 0, 1}, {5, 6, 0, 1}};
inline void loadFetchVS(Arena& arena) {
    arena.code(fetchVS, {
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
    arena.code(fetchShader, {
        0xc0840300,             // s_load_dwordx4 s[8:11], s[2:3], 0x0
        0xbf8c007f,             // s_waitcnt lgkmcnt(0)
        0xe00c2000, 0x80020400, // buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen
        0xbf8c1f70,             // s_waitcnt vmcnt(0)
        0xbe802000});           // s_setpc_b64 s[0:1]
    arena.code(vertexTable, {0x10e00, 16u << 16, 3, 0x77fac}); // V#: stride 16, 3 records, float xyzw
    for (unsigned n = 0; n < 3; ++n) for (unsigned c = 0; c < 4; ++c) arena.put(0x10e00 + 16 * n + 4 * c, bits(fetchVertices[n][c]));
}

// A compute program, 32 threads per group (thread group id x in s16): thread
// n of group g reads record 32g + n (float xyzw), halves x and stores the
// record as 8_8_8_8 UNORM, then as pixel (n, g) of a linear 8x8 image.
constexpr std::uint64_t computeCS = 0x11000, computeSource = 0x14000, computeBuffer = 0x15000, computeImage = 0x16000;
inline const std::uint32_t computeSgprs[16] = {
    0x14000, 16u << 16, 40, 0x77fac,                        // V# source: stride 16, 40 records, float xyzw
    0x15000, 4u << 16, 100, 0x50fac,                        // V# destination: stride 4, 8_8_8_8 UNORM
    0x160, 10u << 20, 7 | 7u << 14, 0x90800fac, 7u << 13, 0, 0, 0}; // T#: 8x8, pitch 8, 8_8_8_8 UNORM, linear 2D
inline void loadComputeCS(Arena& arena) {
    arena.code(computeCS, {
        0x8f118510,             // s_lshl_b32 s17, s16, 5
        0x4a080011,             // v_add_i32 v4, vcc, s17, v0
        0x7e0c0300,             // v_mov_b32 v6, v0
        0x7e0e0210,             // v_mov_b32 v7, s16
        0xe00c2000, 0x80000004, // buffer_load_format_xyzw v[0:3], v4, s[0:3], 0 idxen
        0xbf8c1f70,             // s_waitcnt vmcnt(0)
        0x100000f0,             // v_mul_f32 v0, 0.5, v0
        0xe01c2000, 0x80010004, // buffer_store_format_xyzw v[0:3], v4, s[4:7], 0 idxen
        0xf0200f00, 0x00020006, // image_store v[0:3], v[6:7], s[8:15] dmask:0xf
        0xbf810000});           // s_endpgm
    for (unsigned n = 0; n < 40; ++n)
        for (unsigned c = 0; c < 4; ++c) arena.put(computeSource + 16 * n + 4 * c, bits(float((n * 7 + c * 3) % 13) / 8.0f - 0.25f));
}
} // namespace gcn_test
