// Mortal Kombat: Legacy Kollection's frame as the game draws it: a 4x4
// matrix VS (s_buffer_load_dwordx16) whose fetch shader loads three V# from
// a table and reads 24-byte vertices (float3 position, float2 UV, 8_8_8_8
// colour), the PS of PM4ScreenQuadMetalTest. The draw must rasterize.
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
        if (!device) { std::puts("SKIP matrix quad: no Metal device"); return 0; }
        Arena arena;
        arena.code(vs, {
            0xbeeb03ff, 0x00000014, // s_mov_b32 vcc_hi, 0x14
            0xbe802100,             // s_swappc_b64 s[0:1], s[0:1]
            0xc3000500,             // s_buffer_load_dwordx16 s[0:15], s[4:7], 0x0
            0xbf8c007f,             // s_waitcnt lgkmcnt(0)
            0x10040800, 0x10060803, 0x10000802, 0x10020801, // v_mul_f32 v2, v3, v0, v1 = s0, s3, s2, s1 * v4
            0x3e040a04, 0x3e060a07, 0x3e000a06, 0x3e020a05, // v_mac_f32 += s4, s7, s6, s5 * v5
            0x3e040c08, 0x3e060c0b, 0x3e000c0a, 0x3e020c09, // v_mac_f32 += s8, s11, s10, s9 * v6
            0x3e040e0c, 0x3e060e0f, 0x3e000e0e, 0x3e020e0d, // v_mac_f32 += s12, s15, s14, s13 * v7
            0xf80008cf, 0x03000102, // exp pos0, v2, v1, v0, v3 done
            0x7e080280,             // v_mov_b32 v4, 0
            0xf800021f, 0x0f0e0d0c, // exp param1, v12, v13, v14, v15
            0xf800020f, 0x04040908, // exp param0, v8, v9, v4, v4
            0xbf810000});
        arena.code(fetch, {
            0x4a000008,             // v_add_i32 v0, vcc, s8, v0
            0x4a060609,             // v_add_i32 v3, vcc, s9, v3
            0xc0860300,             // s_load_dwordx4 s[12:15], s[2:3], 0x0
            0xc0880304,             // s_load_dwordx4 s[16:19], s[2:3], 0x4
            0xc08a0308,             // s_load_dwordx4 s[20:23], s[2:3], 0x8
            0xbf8c007f,             // s_waitcnt lgkmcnt(0)
            0xe00c2000, 0x80030400, // buffer_load_format_xyzw v[4:7], v0, s[12:15], 0 idxen
            0xe0042000, 0x80040800, // buffer_load_format_xy v[8:9], v0, s[16:19], 0 idxen
            0xe00c2000, 0x80050c00, // buffer_load_format_xyzw v[12:15], v0, s[20:23], 0 idxen
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
        // Interleaved 24-byte vertices: float3 position, float2 UV, 8_8_8_8 UNORM colour.
        const float corners[4][5] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
        for (unsigned n = 0; n < 4; ++n) {
            for (unsigned c = 0; c < 5; ++c) arena.put(vertices + 24 * n + 4 * c, bits(corners[n][c]));
            arena.put(vertices + 24 * n + 20, 0xffffffff);
        }
        // Identity: x = s0 * px + s12 * pw, y = s5 * py + s13 * pw, z = s10 * pz + s14 * pw, w = s15 * pw.
        const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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
        for (const std::uint32_t constantRecords : {4u, 0x1000u})
        for (const std::uint32_t records : {4u, 0xffffffffu})
        for (const std::uint64_t high : {std::uint64_t(0), std::uint64_t(0x7000000000)}) {
        const GCNMemory memory{
            [&](std::uint64_t a, void* d, std::size_t n) { return a >= high && arena.memory.read(a - high, d, n); },
            [&](std::uint64_t a, const void* d, std::size_t n) { return a >= high && arena.memory.write(a - high, d, n); }};
        auto lo = [&](std::uint64_t a) { return std::uint32_t(a + high); };
        auto hi = [&](std::uint64_t a) { return std::uint32_t((a + high) >> 32); };
        // Position: 32_32_32 float, DST_SEL X, Y, Z, 1.
        arena.code(table, {lo(vertices), hi(vertices) | 24u << 16, records, 4 | 5 << 3 | 6 << 6 | 1 << 9 | 7 << 12 | 13 << 15,
                           lo(vertices + 12), hi(vertices + 12) | 24u << 16, records, 0x5ffac, // float2 UV
                           lo(vertices + 20), hi(vertices + 20) | 24u << 16, records, 0x50fac}); // 8_8_8_8 UNORM colour
        Stream s;
        s.set(0x76, 0x48, {std::uint32_t((vs + high) >> 8), std::uint32_t((vs + high) >> 40)}); // SPI_SHADER_PGM_LO/HI_VS
        s.set(0x76, 0x4b, {10u << 1});                                       // RSRC2_VS: 10 user SGPRs
        s.set(0x76, 0x4c, {lo(fetch), hi(fetch), lo(table), hi(table),
                           lo(constants), hi(constants) | 16u << 16, constantRecords, 0x20077fac, 0, 0});
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
            if (!high && !native) std::printf("draw: %s\nprobe: %s\n", renderer.batchDraws.c_str(), renderer.probeReport.c_str());
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
            std::printf("constants 0x%x, records 0x%x, guest 0x%llx, %s: %zu draws, %llu samples, centre %u %u %u %u\n", records, (unsigned long long)high,
                        native ? "native textures" : "buffer textures",
                        std::size_t(stats.rendered), (unsigned long long)samples, p[0], p[1], p[2], p[3]);
            auto near = [](unsigned a, unsigned b) { return a + 1 >= b && b + 1 >= a; };
            ok = ok && stats.rendered == 1 && samples > 0 && near(p[0], 200) && near(p[1], 100) && near(p[2], 50) && p[3] == 255;
        }
        }
        std::puts(ok ? "PASS matrix quad as Mortal Kombat draws its frame" : "FAIL matrix quad as Mortal Kombat draws its frame");
        return ok ? 0 : 1;
    }
}
