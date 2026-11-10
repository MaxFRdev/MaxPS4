// Translated GCN programs on a real Metal GPU, against the software renderer:
// a fetch-shader VS draws a covering triangle; the PS reads POS_X/POS_Y,
// samples a linear RGBA8 texture bilinearly, takes a quad derivative,
// branches on VCC and interpolates a VS parameter. Both images must match.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "../Native/PM4SoftwareRenderer.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
constexpr std::uint64_t pixelProgram = 0x11000, texture = 0x14000, target = 0x18000;
constexpr unsigned width = 16, height = 8;
struct DrawState { std::uint64_t base, size; std::uint32_t vsUser[16], psUser[17]; std::uint32_t flags, rangeCount; std::uint64_t ranges[48]; };
struct Stream {
    std::vector<std::uint32_t> words;
    void set(std::uint8_t opcode, std::uint32_t offset, std::initializer_list<std::uint32_t> values) {
        words.push_back(0xc0000000U | (std::uint32_t(values.size()) << 16) | (std::uint32_t(opcode) << 8));
        words.push_back(offset);
        words.insert(words.end(), values);
    }
};
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP GCN Metal translation: no Metal device"); return 0; }
        Arena arena;
        loadFetchVS(arena);
        const float triangle[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}}; // covers the target
        for (unsigned n = 0; n < 3; ++n) for (unsigned c = 0; c < 4; ++c) arena.put(0x10e00 + 16 * n + 4 * c, bits(triangle[n][c]));
        arena.code(pixelProgram, {
            0xbe94047e,             // s_mov_b64 s[20:21], exec
            0xbefe0a7e,             // s_wqm_b64 exec, exec
            0xc8280000, 0xc8290001, // v_interp_p1/p2_f32 v10, attr0.x
            0x100804ff, 0x3d800000, // v_mul_f32 v4, 1/16, v2 (POS_X)
            0x100a06ff, 0x3e000000, // v_mul_f32 v5, 1/8, v3 (POS_Y)
            0xf0800f00, 0x00400604, // image_sample v[6:9], v4, s[0:7], s[8:11] dmask:0xf
            0xd8d48055, 0x0b000004, // ds_swizzle_b32 v11, v4 (quad lane 1)
            0xd8d48000, 0x0c000004, // ds_swizzle_b32 v12, v4 (quad lane 0)
            0x0816190b,             // v_sub_f32 v11, v11, v12: ddx(u) = 1/16
            0x100e16ff, 0x41000000, // v_mul_f32 v7, 8.0, v11: green 0.5
            0x061014ff, 0x41200000, // v_add_f32 v8, 10.0, v10
            0x101010ff, 0x3ccccccd, // v_mul_f32 v8, 0.025, v8: blue from the parameter
            0x7c0208f4,             // v_cmp_lt_f32 vcc, 2.0, v4 (never)
            0xbf860001,             // s_cbranch_vccz +1
            0x7e120280,             // v_mov_b32 v9, 0 (skipped)
            0xbefe0414,             // s_mov_b64 exec, s[20:21]
            0xf800180f, 0x09080706, // exp mrt0, v6, v7, v8, v9 done vm
            0xbf810000});
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
            arena.put(texture + 4 * (y * width + x), (x * 16) | ((y * 32) << 8) | (128u << 16) | (255u << 24));
        const std::uint32_t tsharp[8] = {std::uint32_t(texture >> 8), 10u << 20, (width - 1) | ((height - 1) << 14),
                                         0x90800facu, (width - 1) << 13, 0, 0, 0};
        const std::uint32_t ssharp[4] = {0x12, 0, 0x500000, 0};
        const std::uint32_t psInputAddr = 0x302; // PERSP_CENTER, POS_X, POS_Y: v0-v1, v2, v3
        std::uint32_t inputCntl[32] = {};
        // Software reference through the PM4 renderer.
        Stream s;
        s.set(0x76, 0x48, {std::uint32_t(fetchVS >> 8), 0, 0, 4u << 1});
        s.set(0x76, 0x4c, {std::uint32_t(fetchShader), 0, std::uint32_t(vertexTable), 0});
        s.set(0x76, 0x08, {std::uint32_t(pixelProgram >> 8), 0, 0, 12u << 1});
        s.set(0x76, 0x0c, {tsharp[0], tsharp[1], tsharp[2], tsharp[3], tsharp[4], tsharp[5], tsharp[6], tsharp[7],
                           ssharp[0], ssharp[1], ssharp[2], ssharp[3]});
        s.set(0x69, 0x318, {std::uint32_t(target >> 8), width / 8 - 1, width * height / 64 - 1, 0, (0xaU << 2) | (1U << 11), 0});
        s.set(0x69, 0x8e, {0xf});
        s.set(0x69, 0x10f, {bits(8), bits(8), bits(-4), bits(4), bits(0.5f), bits(0.5f)});
        s.set(0x69, 0x206, {0x43f});
        s.set(0x69, 0x0c, {0, width | (height << 16)});
        s.set(0x69, 0x191, {0});
        s.set(0x69, 0x1b3, {psInputAddr, psInputAddr});
        s.set(0x79, 0x242, {4});
        s.words.insert(s.words.end(), {0xc0012d00U, 3, 2});
        PM4SoftwareRenderer renderer(arena.memory);
        renderer.execute(s.words.data(), s.words.size());
        if (!renderer.stats.error.empty()) std::printf("software: %s\n", renderer.stats.error.c_str());
        assert(renderer.stats.rendered == 1 && renderer.stats.failed == 0);
        // The same programs translated to Metal.
        GCNTranslateOptions vsOptions; vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program";
        vsOptions.userCount = 4; vsOptions.userData[0] = std::uint32_t(fetchShader); vsOptions.userData[2] = std::uint32_t(vertexTable);
        GCNTranslateOptions psOptions; psOptions.name = "ps_program";
        const auto vs = translateGCN(arena.memory, fetchVS, vsOptions);
        const auto ps = translateGCN(arena.memory, pixelProgram, psOptions);
        if (!vs || !ps) { std::printf("translation: %s %s\n", vs.error.c_str(), ps.error.c_str()); return 1; }
        const std::string source = gcnMetalPrelude() + vs.source + ps.source +
                                   gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", psInputAddr, inputCntl);
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if (!library) {
            std::printf("MSL compile failed: %s\n", error.localizedDescription.UTF8String);
            unsigned line = 1;
            std::printf("%4u ", line);
            for (char c : source) { std::putchar(c); if (c == '\n') std::printf("%4u ", ++line); }
            return 1;
        }
        MTLRenderPipelineDescriptor* pipelineDescriptor = [MTLRenderPipelineDescriptor new];
        pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
        pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"gcn_fragment"];
        pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipeline) { std::printf("pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> image = [device newTextureWithDescriptor:descriptor];
        id<MTLBuffer> memory = [device newBufferWithBytes:arena.bytes.data() length:arena.bytes.size() options:MTLResourceStorageModeShared];
        DrawState state{};
        state.base = Arena::start; state.size = arena.bytes.size();
        state.vsUser[0] = std::uint32_t(fetchShader); state.vsUser[2] = std::uint32_t(vertexTable);
        for (unsigned n = 0; n < 8; ++n) state.psUser[n] = tsharp[n];
        for (unsigned n = 0; n < 4; ++n) state.psUser[8 + n] = ssharp[n];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = image; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore; pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:memory offset:0 atIndex:0];
        [encoder setFragmentBuffer:memory offset:0 atIndex:0];
        [encoder setVertexBytes:&state length:sizeof state atIndex:1];
        [encoder setFragmentBytes:&state length:sizeof state atIndex:1];
        const std::uint32_t indices[3] = {0, 1, 2};
        [encoder setVertexBytes:indices length:sizeof indices atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [buffer commit]; [buffer waitUntilCompleted];
        assert(buffer.status == MTLCommandBufferStatusCompleted && !buffer.error);
        std::uint8_t pixels[width * height * 4] = {};
        [image getBytes:pixels bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
        const std::uint8_t* reference = arena.bytes.data() + (target - Arena::start);
        int worst = 0;
        for (unsigned n = 0; n < width * height * 4; ++n) worst = std::max(worst, std::abs(int(pixels[n]) - int(reference[n])));
        if (worst > 2) {
            for (unsigned y = 0; y < height; ++y, std::puts("")) for (unsigned x = 0; x < width; ++x) {
                const unsigned at = 4 * (y * width + x);
                std::printf("%02x%02x%02x%02x/%02x%02x%02x%02x ", pixels[at + 2], pixels[at + 1], pixels[at], pixels[at + 3],
                            reference[at + 2], reference[at + 1], reference[at], reference[at + 3]);
            }
        }
        assert(worst <= 2);
        // Known values: red = texel x * 16, green = 8 * ddx(u) = 0.5, alpha 1.
        assert(pixels[4 * 5 + 2] == 80 && std::abs(int(pixels[1]) - 128) <= 1 && pixels[3] == 255);
        std::printf("PASS GCN programs translated to Metal match the software renderer (worst channel difference %d)\n", worst);
        // GET_LOD at the movie-to-3D transition: analytic footprint of eight
        // texels per pixel, so both results are 3 (displayed divided by eight).
        arena.code(pixelProgram, {
            0x100004ff,0x3d000000, // v0 = POS_X / 32
            0x100206ff,0x3d800000, // v1 = POS_Y / 16
            0xf1800300,0x00400a00, // image_get_lod v[10:11], v0, s[0:7], s[8:11]
            0x101414ff,0x3e000000, // v10 *= 1/8
            0x101616ff,0x3e000000, // v11 *= 1/8
            0x7e1a02f2,            // v13 = 1
            0xf800180f,0x0d0c0b0a,0xbf810000});
        const auto lodPS=translateGCN(arena.memory,pixelProgram,psOptions);
        assert(lodPS && lodPS.quad);
        const std::string lodSource=gcnMetalPrelude()+vs.source+lodPS.source+
            gcnMetalEntryPoints(vs,lodPS,"vs_program","ps_program",psInputAddr,inputCntl);
        library=[device newLibraryWithSource:[NSString stringWithUTF8String:lodSource.c_str()] options:options error:&error];
        if (!library) { std::printf("LOD MSL: %s\n",error.localizedDescription.UTF8String); return 1; }
        pipelineDescriptor.vertexFunction=[library newFunctionWithName:@"gcn_vertex"];
        pipelineDescriptor.fragmentFunction=[library newFunctionWithName:@"gcn_fragment"];
        pipeline=[device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipeline) { std::printf("LOD pipeline: %s\n",error.localizedDescription.UTF8String); return 1; }
        state.psUser[2]=255u | (127u<<14);
        state.psUser[3]=0x90070000;
        state.psUser[9]=0xfff000;
        buffer=[queue commandBuffer]; encoder=[buffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:memory offset:0 atIndex:0];
        [encoder setFragmentBuffer:memory offset:0 atIndex:0];
        [encoder setVertexBytes:&state length:sizeof state atIndex:1];
        [encoder setFragmentBytes:&state length:sizeof state atIndex:1];
        [encoder setVertexBytes:indices length:sizeof indices atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [buffer commit]; [buffer waitUntilCompleted];
        assert(buffer.status==MTLCommandBufferStatusCompleted && !buffer.error);
        [image getBytes:pixels bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        for (unsigned n=0;n<width*height;++n) {
            assert(std::abs(int(pixels[4*n+2])-96)<=1);
            assert(std::abs(int(pixels[4*n+1])-96)<=1);
            assert(pixels[4*n+3]==255);
        }
        std::puts("PASS image_get_lod on Metal: clamped and raw 2D LOD = 3 in every pixel");
        // Exact failing gather operation: four distinct green texels, offset -1.
        arena.code(pixelProgram,{
            0x7e1802ff,63,0x7e1a02ff,0x3ec00000,0x7e1c02ff,0x3ec00000,
            0xf15c0200,0x0040040c,0xf800180f,0x07060504,0xbf810000});
        const auto gatherPS=translateGCN(arena.memory,pixelProgram,psOptions);
        assert(gatherPS);
        const std::string gatherSource=gcnMetalPrelude()+vs.source+gatherPS.source+
            gcnMetalEntryPoints(vs,gatherPS,"vs_program","ps_program",psInputAddr,inputCntl);
        library=[device newLibraryWithSource:[NSString stringWithUTF8String:gatherSource.c_str()] options:options error:&error];
        if (!library) { std::printf("gather MSL: %s\n",error.localizedDescription.UTF8String); return 1; }
        pipelineDescriptor.vertexFunction=[library newFunctionWithName:@"gcn_vertex"];
        pipelineDescriptor.fragmentFunction=[library newFunctionWithName:@"gcn_fragment"];
        pipeline=[device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipeline) { std::printf("gather pipeline: %s\n",error.localizedDescription.UTF8String); return 1; }
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            arena.put(0x15000+4*(4*y+x),0xff000000u|((4*y+x)*16u<<8));
        std::memcpy(memory.contents,arena.bytes.data(),arena.bytes.size());
        state.psUser[0]=0x150; state.psUser[1]=10u<<20;
        state.psUser[2]=3u|(3u<<14); state.psUser[3]=0x90800fac;
        state.psUser[4]=3u<<13; state.psUser[9]=0;
        buffer=[queue commandBuffer]; encoder=[buffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:memory offset:0 atIndex:0];
        [encoder setFragmentBuffer:memory offset:0 atIndex:0];
        [encoder setVertexBytes:&state length:sizeof state atIndex:1];
        [encoder setFragmentBytes:&state length:sizeof state atIndex:1];
        [encoder setVertexBytes:indices length:sizeof indices atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [buffer commit]; [buffer waitUntilCompleted];
        assert(buffer.status==MTLCommandBufferStatusCompleted && !buffer.error);
        [image getBytes:pixels bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        const int gatherBGRA[4]={80,144,128,64};
        for (unsigned n=0;n<width*height*4;++n) assert(std::abs(int(pixels[n])-gatherBGRA[n%4])<=1);
        std::puts("PASS image_gather4_lz_o on Metal: channel, tap order and signed offset at every pixel");
        // Real typed vertex fetch. The descriptor deliberately says RGBA8;
        // the exact device instruction must instead load FLOAT32x4 positions.
        arena.code(0x11b00,{0x7e020300,0xebf33000,0x80030f01,0xf80008cf,0x1211100f,0xbf810000});
        arena.code(0x11b80,{0x7e080280,0x7e0a02f2,0xf800180f,0x05040504,0xbf810000});
        const auto typedVS=translateGCN(arena.memory,0x11b00,vsOptions);
        const auto greenPS=translateGCN(arena.memory,0x11b80,psOptions);
        assert(typedVS && greenPS);
        const std::string typedSource=gcnMetalPrelude()+typedVS.source+greenPS.source+
            gcnMetalEntryPoints(typedVS,greenPS,"vs_program","ps_program",0,inputCntl);
        library=[device newLibraryWithSource:[NSString stringWithUTF8String:typedSource.c_str()] options:options error:&error];
        if (!library) { std::printf("typed MSL: %s\n",error.localizedDescription.UTF8String); return 1; }
        pipelineDescriptor.vertexFunction=[library newFunctionWithName:@"gcn_vertex"];
        pipelineDescriptor.fragmentFunction=[library newFunctionWithName:@"gcn_fragment"];
        pipeline=[device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipeline) { std::printf("typed pipeline: %s\n",error.localizedDescription.UTF8String); return 1; }
        std::memcpy(memory.contents,arena.bytes.data(),arena.bytes.size());
        state.vsUser[12]=0x10e00; state.vsUser[13]=16u<<16;
        state.vsUser[14]=3; state.vsUser[15]=0x50fac;
        buffer=[queue commandBuffer]; encoder=[buffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:memory offset:0 atIndex:0];
        [encoder setFragmentBuffer:memory offset:0 atIndex:0];
        [encoder setVertexBytes:&state length:sizeof state atIndex:1];
        [encoder setFragmentBytes:&state length:sizeof state atIndex:1];
        [encoder setVertexBytes:indices length:sizeof indices atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding]; [buffer commit]; [buffer waitUntilCompleted];
        assert(buffer.status==MTLCommandBufferStatusCompleted && !buffer.error);
        [image getBytes:pixels bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        for (unsigned n=0;n<width*height;++n)
            assert(pixels[4*n]==0 && pixels[4*n+1]==255 && pixels[4*n+2]==0 && pixels[4*n+3]==255);
        std::puts("PASS typed vertex fetch on Metal: instruction format overrides descriptor at every pixel");
    }
}
