// The positions a translated vertex program produces in Metal's vertex stage
// (rasterization off, written to a buffer) must equal those it produces in a
// compute kernel and in the interpreter: Mortal Kombat's 4x4 matrix VS with a
// fetch shader reading 24-byte vertices, and an ImGui-style VS on the same data.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include <cstdio>
#include <cstring>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
struct DrawState { std::uint64_t base, size; std::uint32_t vsUser[16], psUser[17]; std::uint32_t flags, rangeCount; std::uint64_t ranges[48]; };
constexpr std::uint64_t matrixVS = 0x10000, imguiVS = 0x10100, fetch = 0x10200, ps = 0x10400, table = 0x10800, constants = 0x10e00, vertices = 0x1c000;
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP vertex stage: no Metal device"); return 0; }
        Arena arena;
        arena.code(matrixVS, {0xbeeb03ff, 0x00000014, 0xbe802100, 0xc3000500, 0xbf8c007f,
            0x10040800, 0x10060803, 0x10000802, 0x10020801, 0x3e040a04, 0x3e060a07, 0x3e000a06, 0x3e020a05,
            0x3e040c08, 0x3e060c0b, 0x3e000c0a, 0x3e020c09, 0x3e040e0c, 0x3e060e0f, 0x3e000e0e, 0x3e020e0d,
            0xf80008cf, 0x03000102, 0x7e080280, 0xf800021f, 0x0f0e0d0c, 0xf800020f, 0x04040908, 0xbf810000});
        arena.code(imguiVS, {0xbeeb03ff, 0x00000012, 0xbe802100, 0xc284050c, 0xc2c00500, 0xbf8c007f,
            0x7e02020b, 0x7e04020a, 0x7e060209, 0x7e000208, 0x3e020803, 0x3e040802, 0x3e060801, 0x3e000800,
            0x3e020a07, 0x3e040a06, 0x3e060a05, 0x3e000a04, 0xf80008cf, 0x01020300, 0x7e080280,
            0xf800021f, 0x0f0e0d0c, 0xf800020f, 0x04040908, 0xbf810000});
        arena.code(fetch, {0x4a000008, 0x4a060609, 0xc0860300, 0xc0880304, 0xc08a0308, 0xbf8c007f,
            0xe00c2000, 0x80030400, 0xe0042000, 0x80040800, 0xe00c2000, 0x80050c00, 0xbf8c0000, 0xbe802000});
        arena.code(ps, {0xbeeb03ff, 0x00000011, 0xbefc030c, 0xbeea047e, 0xbefe0a7e,
            0xc8080000, 0xc80c0100, 0xc8090001, 0xc80d0101, 0xf0800f00, 0x00400202,
            0xc8200400, 0xc81c0500, 0xc8180700, 0xc8000600, 0xc8210401, 0xc81d0501, 0xc8190701, 0xc8010601,
            0xbf8c0f70, 0x10021102, 0x10040707, 0x10060b06, 0x10000900, 0x5e020501, 0x5e000700, 0xbefe046a,
            0xf8001c0f, 0x00000001, 0xbf810000});
        const float corners[4][5] = {{-1, 1, 0, 0, 0}, {1, 1, 0, 1, 0}, {-1, -1, 0, 0, 1}, {1, -1, 0, 1, 1}};
        for (unsigned n = 0; n < 4; ++n) {
            for (unsigned c = 0; c < 5; ++c) arena.put(vertices + 24 * n + 4 * c, bits(corners[n][c]));
            arena.put(vertices + 24 * n + 20, 0xffffffff);
        }
        const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (unsigned k = 0; k < 16; ++k) arena.put(constants + 4 * k, bits(matrix[k]));
        arena.code(table, {std::uint32_t(vertices), 24u << 16, 4, 4 | 5 << 3 | 6 << 6 | 1 << 9 | 7 << 12 | 13 << 15,
                           std::uint32_t(vertices + 12), 24u << 16, 4, 0x5ffac, std::uint32_t(vertices + 20), 24u << 16, 4, 0x50fac});
        const std::uint32_t vsUser[10] = {std::uint32_t(fetch), 0, std::uint32_t(table), 0, std::uint32_t(constants), 16u << 16, 4, 0x20077fac, 0, 0};
        const std::uint32_t inputCntl[32] = {0, 1};
        id<MTLBuffer> memory = [device newBufferWithBytes:arena.bytes.data() length:arena.bytes.size() options:MTLResourceStorageModeShared];
        DrawState state{};
        state.base = Arena::start; state.size = arena.bytes.size();
        for (unsigned k = 0; k < 10; ++k) state.vsUser[k] = vsUser[k];
        const std::uint32_t indices[6] = {0, 1, 2, 2, 1, 3};
        id<MTLBuffer> indexBuffer = [device newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        bool ok = true;
        for (const std::uint64_t program : {matrixVS, imguiVS}) {
            GCNTranslateOptions vo; vo.stage = GCNStage::vertex; vo.name = "vs_program"; vo.userCount = 10;
            for (unsigned k = 0; k < 10; ++k) vo.userData[k] = vsUser[k];
            GCNTranslateOptions po; po.name = "ps_program"; po.userCount = 13;
            const auto vs = translateGCN(arena.memory, program, vo);
            const auto pst = translateGCN(arena.memory, ps, po);
            if (!vs || !pst) { std::printf("FAIL translation: %s %s\n", vs.error.c_str(), pst.error.c_str()); return 1; }
            const std::string source = gcnMetalPrelude() + vs.source + pst.source +
                gcnMetalEntryPoints(vs, pst, "vs_program", "ps_program", 2, inputCntl) +
                "kernel void probe(uint i [[thread_position_in_grid]], device const uchar* mem [[buffer(0)]],\n"
                "    constant GCNDrawState& D [[buffer(1)]], device const uint* indices [[buffer(2)]], device float4* out [[buffer(4)]]) {\n"
                "    out[i] = gcn_run_vertex(indices[i], 0u, mem, GCNSlots{}, D).position;\n}\n"
                "vertex void capture(uint vid [[vertex_id]], device const uchar* mem [[buffer(0)]],\n"
                "    constant GCNDrawState& D [[buffer(1)]], device const uint* indices [[buffer(2)]], device float4* out [[buffer(4)]]) {\n"
                "    out[vid] = gcn_run_vertex(indices[vid], 0u, mem, GCNSlots{}, D).position;\n}\n"
                "vertex float4 capture_positioned(uint vid [[vertex_id]], device const uchar* mem [[buffer(0)]],\n"
                "    constant GCNDrawState& D [[buffer(1)]], device const uint* indices [[buffer(2)]], device float4* out [[buffer(4)]]) {\n"
                "    float4 p = gcn_run_vertex(indices[vid], 0u, mem, GCNSlots{}, D).position; out[vid] = p; return p;\n}\n";
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
            if (!library) { std::printf("FAIL MSL: %s\n", error.localizedDescription.UTF8String); return 1; }
            float results[3][6][4] = {};
            // 0: compute kernel.
            {
                id<MTLComputePipelineState> state0 = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"probe"] error:&error];
                id<MTLBuffer> out = [device newBufferWithLength:6 * 16 options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> c = [queue commandBuffer];
                id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
                [e setComputePipelineState:state0]; [e setBuffer:memory offset:0 atIndex:0];
                [e setBytes:&state length:sizeof state atIndex:1]; [e setBuffer:indexBuffer offset:0 atIndex:2]; [e setBuffer:out offset:0 atIndex:4];
                [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(6, 1, 1)];
                [e endEncoding]; [c commit]; [c waitUntilCompleted];
                std::memcpy(results[0], out.contents, sizeof results[0]);
            }
            // 1 and 2: the vertex stage, without and with a rasterized position.
            for (unsigned variant = 1; variant <= 2; ++variant) {
                MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = [library newFunctionWithName:variant == 1 ? @"capture" : @"capture_positioned"];
                d.rasterizationEnabled = variant == 1 ? NO : YES;
                d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:d error:&error];
                if (!pipeline) { std::printf("FAIL pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
                id<MTLBuffer> out = [device newBufferWithLength:6 * 16 options:MTLResourceStorageModeShared];
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = [device newTextureWithDescriptor:td];
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                id<MTLCommandBuffer> c = [queue commandBuffer];
                id<MTLRenderCommandEncoder> e = [c renderCommandEncoderWithDescriptor:pass];
                [e setRenderPipelineState:pipeline]; [e setVertexBuffer:memory offset:0 atIndex:0];
                [e setVertexBytes:&state length:sizeof state atIndex:1]; [e setVertexBuffer:indexBuffer offset:0 atIndex:2];
                [e setVertexBuffer:out offset:0 atIndex:4];
                [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:6];
                [e endEncoding]; [c commit]; [c waitUntilCompleted];
                std::memcpy(results[variant], out.contents, sizeof results[variant]);
            }
            // 3: the full pipeline (gcn_vertex, gcn_fragment, the game's blend) over a 64x64 target.
            std::uint64_t samples = 0;
            {
                MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
                d.fragmentFunction = [library newFunctionWithName:@"gcn_fragment"];
                d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
                d.colorAttachments[0].blendingEnabled = YES;
                d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:d error:&error];
                if (!pipeline) { std::printf("FAIL full pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
                MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
                id<MTLBuffer> visibility = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
                std::memset(visibility.contents, 0, 8);
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = [device newTextureWithDescriptor:td];
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.visibilityResultBuffer = visibility;
                DrawState full = state;
                const std::uint32_t psUser[13] = {0x110, 10u << 20, 7 | (7u << 14), 0x90800fac, 7u << 13, 0, 0, 0, 0x12, 0, 0x500000, 0, 0};
                for (unsigned k = 0; k < 13; ++k) full.psUser[k] = psUser[k];
                for (unsigned k = 0; k < 64; ++k) arena.put(0x11000 + 4 * k, 200u | (100u << 8) | (50u << 16) | (255u << 24));
                std::memcpy(memory.contents, arena.bytes.data(), arena.bytes.size());
                id<MTLCommandBuffer> c = [queue commandBuffer];
                id<MTLRenderCommandEncoder> e = [c renderCommandEncoderWithDescriptor:pass];
                [e setRenderPipelineState:pipeline];
                [e setViewport:MTLViewport{0, 0, 64, 64, 0.5, 1.0}];
                [e setVertexBuffer:memory offset:0 atIndex:0]; [e setFragmentBuffer:memory offset:0 atIndex:0];
                [e setVertexBytes:&full length:sizeof full atIndex:1]; [e setFragmentBytes:&full length:sizeof full atIndex:1];
                [e setVertexBuffer:indexBuffer offset:0 atIndex:2];
                [e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [e endEncoding]; [c commit]; [c waitUntilCompleted];
                samples = *static_cast<const std::uint64_t*>(visibility.contents);
            }
            std::printf("VS 0x%llx, full pipeline: %llu samples\n", (unsigned long long)program, (unsigned long long)samples);
            ok = ok && samples == 64 * 64;
            static const char* const names[3] = {"kernel", "vertex stage", "vertex stage, positioned"};
            for (unsigned r = 0; r < 3; ++r) {
                std::printf("VS 0x%llx, %s:", (unsigned long long)program, names[r]);
                for (unsigned v = 0; v < 6; ++v) std::printf(" (%g %g %g %g)", results[r][v][0], results[r][v][1], results[r][v][2], results[r][v][3]);
                std::printf("\n");
                ok = ok && std::memcmp(results[r], results[0], sizeof results[0]) == 0;
            }
            ok = ok && results[0][0][0] == -1 && results[0][0][1] == 1 && results[0][0][3] == 1;
        }
        std::puts(ok ? "PASS vertex stage matches the kernel" : "FAIL vertex stage matches the kernel");
        return ok ? 0 : 1;
    }
}
