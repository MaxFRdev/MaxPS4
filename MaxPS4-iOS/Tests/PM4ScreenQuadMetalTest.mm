// A full-screen textured quad shaped like Mortal Kombat: Legacy Kollection's
// frame (an ImGui-style VS with a fetch shader reading V# from a table, a PS
// saving EXEC in VCC and exporting compressed colour), through the PM4 draw
// collector and the Metal renderer in copy mode with native textures, as the
// game session runs them. The draw must rasterize and write the texture.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/PM4MetalRenderer.h"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
struct Stream {
    std::vector<std::uint32_t> words;
    void set(std::uint8_t opcode, std::uint32_t offset, std::initializer_list<std::uint32_t> values) {
        words.push_back(0xc0000000U | (std::uint32_t(values.size()) << 16) | (std::uint32_t(opcode) << 8));
        words.push_back(offset);
        words.insert(words.end(), values);
    }
};
constexpr std::uint64_t vs = 0x10000, fetch = 0x10200, ps = 0x10400, table = 0x10800, vertices = 0x1c000,
                        constants = 0x10e00, indexBuffer = 0x10f00, texture = 0x11000, target = 0x7000000000;
constexpr unsigned width = 1920, height = 1080;
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP full-screen quad: no Metal device"); return 0; }
        Arena arena;
        arena.code(vs, {
            0xbeeb03ff, 0x00000012, // s_mov_b32 vcc_hi, 0x12 (OrbShdr prologue)
            0xbe802100,             // s_swappc_b64 s[0:1], s[0:1]
            0xc284050c,             // s_buffer_load_dwordx4 s[8:11], s[4:7], 0xc
            0xc2c00500,             // s_buffer_load_dwordx8 s[0:7], s[4:7], 0x0
            0xbf8c007f,             // s_waitcnt lgkmcnt(0)
            0x7e02020b, 0x7e04020a, 0x7e060209, 0x7e000208, // v_mov_b32 v1-v3, v0 <- s11, s10, s9, s8
            0x3e020803, 0x3e040802, 0x3e060801, 0x3e000800, // v_mac_f32 v1-v3, v0 += s3, s2, s1, s0 * v4
            0x3e020a07, 0x3e040a06, 0x3e060a05, 0x3e000a04, // v_mac_f32 v1-v3, v0 += s7, s6, s5, s4 * v5
            0xf80008cf, 0x01020300, // exp pos0, v0, v3, v2, v1 done
            0x7e080280,             // v_mov_b32 v4, 0
            0xf800021f, 0x0f0e0d0c, // exp param1, v12, v13, v14, v15
            0xf800020f, 0x04040908, // exp param0, v8, v9, v4, v4
            0xbf810000});
        arena.code(fetch, {
            0x4a000008,             // v_add_i32 v0, vcc, s8, v0
            0x4a060609,             // v_add_i32 v3, vcc, s9, v3
            0xc0860300, 0xbf8c007f, // s_load_dwordx4 s[12:15], s[2:3], 0x0; s_waitcnt lgkmcnt(0)
            0xe0042000, 0x80030400, // buffer_load_format_xy v[4:5], v0, s[12:15], 0 idxen
            0xc0860304, 0xbf8c007f, // s_load_dwordx4 s[12:15], s[2:3], 0x4
            0xe0042000, 0x80030800, // buffer_load_format_xy v[8:9], v0, s[12:15], 0 idxen
            0xc0860308, 0xbf8c007f, // s_load_dwordx4 s[12:15], s[2:3], 0x8
            0xe00c2000, 0x80030c00, // buffer_load_format_xyzw v[12:15], v0, s[12:15], 0 idxen
            0xbf8c0000,             // s_waitcnt vmcnt(0) expcnt(0) lgkmcnt(0)
            0xbe802000});           // s_setpc_b64 s[0:1]
        arena.code(ps, {
            0xbeeb03ff, 0x00000011, // s_mov_b32 vcc_hi, 0x11
            0xbefc030c,             // s_mov_b32 m0, s12
            0xbeea047e,             // s_mov_b64 vcc, exec
            0xbefe0a7e,             // s_wqm_b64 exec, exec
            0xc8080000, 0xc80c0100, 0xc8090001, 0xc80d0101, // v_interp_p1/p2 v2, v3 <- attr0.xy
            0xf0800f00, 0x00400202, // image_sample v[2:5], v2, s[0:7], s[8:11] dmask:0xf
            0xc8200400, 0xc81c0500, 0xc8180700, 0xc8000600, // v_interp_p1 v8, v7, v6, v0 <- attr1.x, y, w, z
            0xc8210401, 0xc81d0501, 0xc8190701, 0xc8010601, // v_interp_p2
            0xbf8c0f70,             // s_waitcnt vmcnt(0)
            0x10021102, 0x10040707, 0x10060b06, 0x10000900, // v_mul_f32: colour * vertex colour
            0x5e020501, 0x5e000700, // v_cvt_pkrtz_f16_f32 v1 (rg), v0 (ba)
            0xbefe046a,             // s_mov_b64 exec, vcc
            0xf8001c0f, 0x00000001, // exp mrt0, v1, v0, v0, v0 compr done vm
            0xbf810000});
        // Interleaved vertices: float2 position, float2 UV, 8_8_8_8 UNORM colour (stride 20).
        const float corners[4][4] = {{-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, -1, 1, 1}};
        for (unsigned n = 0; n < 4; ++n) {
            for (unsigned c = 0; c < 4; ++c) arena.put(vertices + 20 * n + 4 * c, bits(corners[n][c]));
            arena.put(vertices + 20 * n + 16, 0xffffffff);
        }
        // Constants: x = s0 * px + s8, y = s5 * py + s9, z = s10, w = s11.
        const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        for (unsigned k = 0; k < 16; ++k) arena.put(constants + 4 * k, bits(matrix[k]));
        const std::uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
        for (unsigned k = 0; k < 6; ++k) arena.put16(indexBuffer + 2 * k, quad[k]);
        for (unsigned k = 0; k < 64; ++k) arena.put(texture + 4 * k, 200u | (100u << 8) | (50u << 16) | (255u << 24));
        id<MTLCommandQueue> queue = [device newCommandQueue];
        bool ok = true;
        // Guest addresses as in the game (above 4 GiB, 0x70...) or as in the
        // other tests: the arena seen at `high` + its own addresses.
        // Vertex buffers sized exactly, or with the "unbounded" record count
        // engines give dynamic buffers (far beyond what a draw reads).
        for (const std::uint32_t records : {4u, 0xffffffffu})
        for (const std::uint64_t high : {std::uint64_t(0), std::uint64_t(0x7000000000)}) {
        const GCNMemory memory{
            [&](std::uint64_t a, void* d, std::size_t n) { return a >= high && arena.memory.read(a - high, d, n); },
            [&](std::uint64_t a, const void* d, std::size_t n) { return a >= high && arena.memory.write(a - high, d, n); }};
        auto lo = [&](std::uint64_t a) { return std::uint32_t(a + high); };
        auto hi = [&](std::uint64_t a) { return std::uint32_t((a + high) >> 32); };
        arena.code(table, {lo(vertices), hi(vertices) | 20u << 16, records, 0x5ffac,        // float2 position
                           lo(vertices + 8), hi(vertices + 8) | 20u << 16, records, 0x5ffac, // float2 UV
                           lo(vertices + 16), hi(vertices + 16) | 20u << 16, records, 0x50fac}); // 8_8_8_8 UNORM colour
        Stream s;
        s.set(0x76, 0x48, {std::uint32_t((vs + high) >> 8), std::uint32_t((vs + high) >> 40)}); // SPI_SHADER_PGM_LO/HI_VS
        s.set(0x76, 0x4b, {10u << 1});                                       // RSRC2_VS: 10 user SGPRs
        s.set(0x76, 0x4c, {lo(fetch), hi(fetch), lo(table), hi(table),
                           lo(constants), hi(constants) | 16u << 16, 4, 0x77fac, 0, 0});
        s.set(0x76, 0x08, {std::uint32_t((ps + high) >> 8), std::uint32_t((ps + high) >> 40)}); // SPI_SHADER_PGM_LO/HI_PS
        s.set(0x76, 0x0b, {13u << 1});                                       // RSRC2_PS: 13 user SGPRs
        s.set(0x76, 0x0c, {std::uint32_t((texture + high) >> 8), std::uint32_t((texture + high) >> 40) | 10u << 20,
                           7 | (7u << 14), 0x90800fac, 7u << 13, 0, 0, 0, 0x12, 0, 0x500000, 0, 0});
        s.set(0x69, 0x8e, {0xf});                                            // CB_TARGET_MASK
        s.set(0x69, 0x318, {std::uint32_t(target >> 8), width / 8 - 1, width * height / 64 - 1});
        s.set(0x69, 0x31c, {0xau << 2});                                     // CB_COLOR0_INFO: 8_8_8_8
        s.set(0x69, 0x1e0, {0x65040504});                                    // CB_BLEND0_CONTROL
        s.set(0x69, 0x0c, {0, width | (height << 16)});                      // scissor
        s.set(0x69, 0x10f, {bits(960), bits(960), bits(-540), bits(540), bits(0.5f), bits(0.5f)});
        s.set(0x69, 0x206, {0x43f});                                         // PA_CL_VTE_CNTL
        s.set(0x69, 0x1b3, {2, 2});                                          // SPI_PS_INPUT_ENA/ADDR
        s.set(0x69, 0x191, {0, 1});                                          // SPI_PS_INPUT_CNTL_0/1
        s.set(0x79, 0x242, {4});                                             // VGT_PRIMITIVE_TYPE: triangles
        s.words.insert(s.words.end(), {0xc0002a00, 0});                      // INDEX_TYPE: 16 bits
        s.words.insert(s.words.end(), {0xc0042700, 6, lo(indexBuffer), hi(indexBuffer), 6, 0}); // DRAW_INDEX_2
        for (const bool native : {false, true}) {
            PM4DrawStats stats;
            PM4DrawCollector collector(memory, stats);
            PM4MetalRenderer renderer(device, memory, nil, 0, native);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            renderer.beginBatch();
            collector.execute(s.words.data(), s.words.size(), [&](const PM4Draw& draw) {
                if (renderer.encode(draw, command)) ++stats.rendered;
                else stats.fail(renderer.error);
            });
            renderer.endBatch();
            if (!high && !native) std::printf("draw: %s\n", renderer.batchDraws.c_str());
            [command commit]; [command waitUntilCompleted];
            assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
            if (stats.failed) { std::printf("FAIL draw: %s\n", stats.error.c_str()); return 1; }
            const std::uint64_t samples = renderer.lastVisibility ? static_cast<const std::uint64_t*>(renderer.lastVisibility.contents)[0] : 0;
            id<MTLTexture> image = renderer.target(target);
            id<MTLBuffer> pixel = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> read = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
            [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(width / 2, height / 2, 0)
                       sourceSize:MTLSizeMake(1, 1, 1) toBuffer:pixel destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
            [blit endEncoding]; [read commit]; [read waitUntilCompleted];
            const auto* p = static_cast<const std::uint8_t*>(pixel.contents);
            std::printf("records 0x%x, guest 0x%llx, %s: %zu draws, %llu samples, centre %u %u %u %u\n", records, (unsigned long long)high,
                        native ? "native textures" : "buffer textures",
                        std::size_t(stats.rendered), (unsigned long long)samples, p[0], p[1], p[2], p[3]);
            auto near = [](unsigned a, unsigned b) { return a + 1 >= b && b + 1 >= a; };
            ok = ok && stats.rendered == 1 && samples > 0 && near(p[0], 200) && near(p[1], 100) && near(p[2], 50) && p[3] == 255;
        }
        }
        std::puts(ok ? "PASS full-screen quad shaped like Mortal Kombat's frame" : "FAIL full-screen quad shaped like Mortal Kombat's frame");
        return ok ? 0 : 1;
    }
}
