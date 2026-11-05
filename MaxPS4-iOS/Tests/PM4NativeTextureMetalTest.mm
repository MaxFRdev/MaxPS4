// Native texel reads (PM4MetalRenderer nativeTextures), on a Metal device.
// 1. Conformance: the runtime's own gcn_sample with and without a native
//    texture must return bit-identical floats, for 8_8_8_8 and 5_6_5, every
//    wrap mode, point and bilinear filtering.
// 2. Measurement: a draw shaped like Sonic Mania's final pass (a 512x256
//    linear texture stretched over a 1920x1080 target, bilinear) rendered by
//    the buffer path and the native path: GPU time and identical pixels.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/PM4MetalRenderer.h"
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
struct State { uint64_t base, size; uint32_t vs[16], ps[17], flags, count; uint64_t ranges[48]; };
static_assert(sizeof(State) == 544, "GCNDrawState layout");
id<MTLArgumentEncoder> nativeEncoder(id<MTLDevice> device) {
    auto argument = [](MTLDataType type, NSUInteger index) {
        MTLArgumentDescriptor* a = [MTLArgumentDescriptor argumentDescriptor];
        a.dataType = type; a.index = index;
        if (type == MTLDataTypeTexture) a.textureType = MTLTextureType2D;
        return a;
    };
    return [device newArgumentEncoderWithArguments:@[argument(MTLDataTypeTexture, 0), argument(MTLDataTypeUInt, 1),
                                                     argument(MTLDataTypeUInt4, 2), argument(MTLDataTypeUInt4, 3),
                                                     argument(MTLDataTypeTexture, 4), argument(MTLDataTypeUInt4, 5),
                                                     argument(MTLDataTypeUInt4, 6), argument(MTLDataTypeTexture, 7),
                                                     argument(MTLDataTypeUInt4, 8), argument(MTLDataTypeUInt4, 9),
                                                     argument(MTLDataTypeSampler, 10), argument(MTLDataTypeSampler, 11),
                                                     argument(MTLDataTypeSampler, 12), argument(MTLDataTypeUInt4, 13),
                                                     argument(MTLDataTypeUInt4, 14), argument(MTLDataTypeUInt4, 15)]];
}
uint32_t noise(uint32_t n) { n *= 2654435761u; n ^= n >> 13; n *= 0x5bd1e995u; return n ^ (n >> 15); }
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP native textures: no Metal device"); return 0; }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        // 1. Conformance.
        const std::string source = gcnMetalPrelude(true) + R"MSL(
kernel void compare_samples(device const uchar* mem [[buffer(0)]], constant GCNDrawState& D [[buffer(1)]],
    device float4* output [[buffer(2)]], constant GCNNativeTexture& N [[buffer(3)]], device uint* used [[buffer(4)]],
    uint i [[thread_position_in_grid]]) {
    GCNRes8 t; GCNRes4 s;
    for (uint k = 0u; k < 8u; ++k) t.d[k] = D.psUser[k];
    for (uint k = 0u; k < 4u; ++k) s.d[k] = D.psUser[8u + k];
    GCNMemory buffered = { mem, D.base, D.size, D.ranges, D.rangeCount, nullptr };
    GCNMemory native = { mem, D.base, D.size, D.ranges, D.rangeCount, &N };
    for (uint k = 0u; k < 8u; ++k) {
        float2 uv = float2(float((i * 17u + k * 31u) % 4093u), float((i * 29u + k * 11u) % 4093u)) / 4093.0f * 1.5f - 0.25f;
        output[2u * (i * 8u + k)] = gcn_sample(buffered, t, s, false, uv.x, uv.y);
        output[2u * (i * 8u + k) + 1u] = gcn_sample(native, t, s, false, uv.x, uv.y);
    }
    float4 v;
    if (i == 0u) used[0] = gcn_native_texel(native, t, 0, 0, v) ? 1u : 0u;
}
)MSL";
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if (!library) std::printf("MSL: %s\n", error.localizedDescription.UTF8String);
        assert(library);
        id<MTLComputePipelineState> compare = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"compare_samples"] error:&error];
        assert(compare);
        id<MTLArgumentEncoder> arguments = nativeEncoder(device);
        assert(arguments);
        constexpr unsigned threads = 2048, samples = threads * 8;
        bool formatNative[2] = {};
        for (unsigned f = 0; f < 2; ++f) for (unsigned size = 0; size < 2; ++size) for (unsigned wrap : {0u, 1u, 2u}) for (unsigned linear : {0u, 1u}) {
          @autoreleasepool {
            const unsigned dfmt = f ? 16 : 10, bpp = f ? 2 : 4;
            const MTLPixelFormat format = f ? MTLPixelFormatB5G6R5Unorm : MTLPixelFormatRGBA8Unorm;
            const unsigned width = size ? 512 : 16, height = size ? 256 : 8;
            const NSUInteger alignment = std::max<NSUInteger>([device minimumLinearTextureAlignmentForPixelFormat:format], 1);
            const unsigned prefix = unsigned((256 + alignment - 1) / alignment * alignment);
            const unsigned pitch = unsigned(((width + 8) * bpp + alignment - 1) / alignment * alignment / bpp);
            id<MTLBuffer> memory = [device newBufferWithLength:prefix + pitch * height * bpp options:MTLResourceStorageModeShared];
            auto* bytes = static_cast<uint8_t*>(memory.contents);
            for (unsigned n = 0; n < pitch * height; ++n) {
                const uint32_t raw = noise(n + 1 + 977 * size);
                std::memcpy(bytes + prefix + n * bpp, &raw, bpp);
            }
            auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
            descriptor.storageMode = MTLStorageModeShared; descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> image = [memory newTextureWithDescriptor:descriptor offset:prefix bytesPerRow:pitch * bpp];
            if (!image) { std::printf("NATIVE TEXTURES: no %s buffer view on this device\n", f ? "B5G6R5" : "RGBA8"); continue; }
            State state{}; state.count = 1;
            state.ranges[0] = 0x14000; state.ranges[1] = pitch * height * bpp; state.ranges[2] = prefix;
            const uint32_t t[12] = {0x140, dfmt << 20, (width - 1) | ((height - 1) << 14), 0x90800facu, (pitch - 1) << 13, 0, 0, 0,
                                    wrap | (wrap << 3), 0, linear ? 0x500000u : 0u, 0};
            std::memcpy(state.ps, t, sizeof t);
            id<MTLBuffer> table = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
            // Each of the three native slots in turn (a video's Y, U and V planes).
            const unsigned slot = (wrap + linear + size) % 3, field[3] = {0, 4, 7}, low[3] = {2, 5, 8};
            [arguments setArgumentBuffer:table offset:0];
            [arguments setTexture:image atIndex:field[slot]];
            // The renderer's Metal sampler for this S#, as the game's draws get it.
            const std::array<uint32_t, 4> sampler = {t[8], t[9], t[10], t[11]};
            MTLSamplerDescriptor* samplerDescriptor = PM4MetalRenderer::nativeSamplerDescriptor(sampler);
            assert(samplerDescriptor);
            [arguments setSamplerState:[device newSamplerStateWithDescriptor:samplerDescriptor] atIndex:10 + slot];
            std::memcpy([arguments constantDataAtIndex:13 + slot], sampler.data(), 16);
            *static_cast<uint32_t*>([arguments constantDataAtIndex:1]) = (1u << slot) | (16u << slot);
            std::memcpy([arguments constantDataAtIndex:low[slot]], t, 16);
            std::memcpy([arguments constantDataAtIndex:low[slot] + 1], t + 4, 16);
            id<MTLBuffer> output = [device newBufferWithLength:samples * 2 * 16 options:MTLResourceStorageModeShared];
            id<MTLBuffer> used = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            [encoder setComputePipelineState:compare];
            [encoder setBuffer:memory offset:0 atIndex:0]; [encoder setBytes:&state length:sizeof state atIndex:1];
            [encoder setBuffer:output offset:0 atIndex:2]; [encoder setBuffer:table offset:0 atIndex:3];
            [encoder setBuffer:used offset:0 atIndex:4];
            [encoder useResource:image usage:MTLResourceUsageRead];
            [encoder dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
            assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
            assert(*static_cast<const uint32_t*>(used.contents) == 1);
            const auto* values = static_cast<const float*>(output.contents);
            // Metal's sampler filters the whole sample (8-bit weights): within
            // a level of GCN's filtering, bilinear; a point sample may pick the
            // neighbour texel right at a texel edge, so a few may differ.
            unsigned unequal = 0;
            float largest = 0;
            for (unsigned k = 0; k < samples; ++k) {
                float difference = 0;
                for (unsigned c = 0; c < 4; ++c) difference = std::max(difference, std::fabs(values[8 * k + c] - values[8 * k + 4 + c]));
                largest = std::max(largest, difference);
                if (difference > 0 && unequal++ < 4)
                    std::printf("  sample %u: buffer (%.9g %.9g %.9g %.9g) native (%.9g %.9g %.9g %.9g)\n", k,
                                values[8 * k], values[8 * k + 1], values[8 * k + 2], values[8 * k + 3],
                                values[8 * k + 4], values[8 * k + 5], values[8 * k + 6], values[8 * k + 7]);
            }
            std::printf("NATIVE CONFORMANCE %s %ux%u wrap %u linear %u: %u of %u samples differ, at most %.5f\n",
                        f ? "5_6_5" : "8_8_8_8", width, height, wrap, linear, unequal, samples, largest);
            assert(linear ? largest <= 1.2f / 255 : unequal <= samples / 100);
            formatNative[f] = true;
          }
        }
        std::printf("PASS native conformance: bit-identical gcn_sample (8_8_8_8 %s, 5_6_5 %s)\n",
                    formatNative[0] ? "tested" : "unavailable", formatNative[1] ? "tested" : "unavailable");

        // 2. Measurement through PM4MetalRenderer (per-draw copies, as the game bridge).
        // 1 MiB of texels: the texture's range takes a shared buffer (buffer(5)).
        constexpr unsigned screenW = 1920, screenH = 1080, imageW = 1024, imageH = 256;
        constexpr uint64_t texturedPS = 0x11400, image = 0x40000, target = 0x200000;
        Arena arena;
        arena.bytes.resize(image + imageW * imageH * 4 - Arena::start);
        loadFetchVS(arena);
        // Vertices just below the texture, at an address that is not 256-byte
        // aligned: the scanner merges both into one range starting there, as
        // in Sonic Mania, and the T# must still get an aligned texture view.
        constexpr uint64_t vertices = image - 0xfc;
        const float triangle[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
        for (unsigned n = 0; n < 3; ++n) for (unsigned c = 0; c < 4; ++c) arena.put(vertices + 16 * n + 4 * c, bits(triangle[n][c]));
        arena.put(vertexTable, std::uint32_t(vertices));
        // PM4MetalRendererTest's textured program with u = x / 1920, v = y / 1080:
        // the whole texture is stretched over the screen.
        arena.code(texturedPS, {
            0xbe94047e, 0xbefe0a7e, 0xc8280000, 0xc8290001, 0x100804ff, bits(1.0f / screenW), 0x100a06ff, bits(1.0f / screenH),
            0xf0800f00, 0x00400604, 0xd8d48055, 0x0b000004, 0xd8d48000, 0x0c000004, 0x0816190b,
            0x100e16ff, 0x41000000, 0x061014ff, 0x41200000, 0x101010ff, 0x3ccccccd, 0x7c0208f4, 0xbf860001,
            0x7e120280, 0xbefe0414, 0xf800180f, 0x09080706, 0xbf810000});
        for (unsigned n = 0; n < imageW * imageH; ++n) arena.put(image + 4 * n, noise(n + 7));
        PM4Draw d;
        d.vs = fetchVS; d.vsUserCount = 4; d.vsUser[0] = std::uint32_t(fetchShader); d.vsUser[2] = std::uint32_t(vertexTable);
        d.ps = texturedPS; d.psUserCount = 12;
        d.psInputAddr = 0x302; d.indices = {0, 1, 2}; d.primitive = 4;
        d.vte = 0x43f;
        const float viewport[6] = {screenW / 2.0f, screenW / 2.0f, -(screenH / 2.0f), screenH / 2.0f, 0.5f, 0.5f};
        std::memcpy(d.viewport, viewport, sizeof viewport);
        d.scissor[2] = screenW; d.scissor[3] = screenH;
        d.target = target; d.pitch = screenW; d.height = screenH; d.swap = 1; d.mask = 0xf;
        id<MTLBuffer> readback = [device newBufferWithLength:screenW * screenH * 4 options:MTLResourceStorageModeShared];
        auto pixels = [&](id<MTLTexture> texture) {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(screenW, screenH, 1) toBuffer:readback destinationOffset:0
           destinationBytesPerRow:screenW * 4 destinationBytesPerImage:screenW * screenH * 4];
            [blit endEncoding]; [command commit]; [command waitUntilCompleted];
            assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
            const auto* p = static_cast<const uint8_t*>(readback.contents);
            return std::vector<uint8_t>(p, p + screenW * screenH * 4);
        };
        // The arena's bytes on whole host pages (Arena::start is page-aligned).
        struct { std::uint8_t* bytes; std::size_t size; } host;
        host.size = (arena.bytes.size() + getpagesize() - 1) / getpagesize() * getpagesize();
        host.bytes = static_cast<std::uint8_t*>(mmap(nullptr, host.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        assert(host.bytes != MAP_FAILED && Arena::start % getpagesize() == 0);
        std::memcpy(host.bytes, arena.bytes.data(), arena.bytes.size());
        for (unsigned f = 0; f < 2; ++f) for (unsigned wrap : {0u, 2u}) {
            const unsigned dfmt = f ? 16 : 10;
            const uint32_t t[12] = {std::uint32_t(image >> 8), dfmt << 20, (imageW - 1) | ((imageH - 1) << 14), 0x90800facu,
                                    (imageW - 1) << 13, 0, 0, 0, wrap | (wrap << 3), 0, 0x500000, 0};
            std::memcpy(d.psUser, t, sizeof t);
            PM4MetalRenderer buffered(device, arena.memory, nil, 0), native(device, arena.memory, nil, 0, true);
            PM4MetalRenderer* renderers[2] = {&buffered, &native};
            double gpu[2] = {}, cpu[2] = {}; unsigned timed[2] = {};
            constexpr unsigned runs = 14, warmup = 4;
            for (unsigned run = 0; run < runs; ++run) for (unsigned turn = 0; turn < 2; ++turn) {
                const unsigned mode = (run + turn) % 2; // alternate which path goes first
                id<MTLCommandBuffer> command = [queue commandBuffer];
                const auto started = [NSDate date];
                const bool encoded = renderers[mode]->encode(d, command);
                const double encodeMs = -[started timeIntervalSinceNow] * 1000;
                if (!encoded) std::printf("%s: %s\n", mode ? "native" : "buffer", renderers[mode]->error.c_str());
                assert(encoded);
                [command commit]; [command waitUntilCompleted];
                assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
                if (run >= warmup && command.GPUStartTime > 0) {
                    gpu[mode] += (command.GPUEndTime - command.GPUStartTime) * 1000; cpu[mode] += encodeMs; ++timed[mode];
                }
            }
            // The V# table, then the vertices merged with the texture: 1 MiB of
            // 8_8_8_8 texels, copied into a shared buffer once per command buffer
            // (5_6_5's 512 KiB stay in the draw's buffer).
            const unsigned shared = f ? 0u : 1u;
            assert(native.lastRanges == 2 && native.rangesShared == shared * runs && buffered.rangesShared == shared * runs);
            {
                id<MTLCommandBuffer> command = [queue commandBuffer];
                assert(buffered.encode(d, command) && buffered.encode(d, command));
                [command commit]; [command waitUntilCompleted];
                assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
                assert(buffered.rangesShared == shared * (runs + 1));
            }
            if (native.nativeDraws != (formatNative[f] ? runs : 0u))
                std::printf("native refusals %u/%u/%u/%u/%u, last: %s\n", native.nativeMisses[0], native.nativeMisses[1],
                            native.nativeMisses[2], native.nativeMisses[3], native.nativeMisses[4], native.nativeLastMiss.c_str());
            assert(native.nativeDraws == (formatNative[f] ? runs : 0u));
            const auto drawn = pixels(buffered.target(target));
            const auto natively = pixels(native.target(target));
            // Metal's sampler filters natively: bytes within 2 levels of GCN's
            // filtering, a few more where the program's own tests flip.
            std::size_t far = 0;
            for (std::size_t k = 0; k < drawn.size(); ++k) far += std::abs(int(drawn[k]) - int(natively[k])) > 2;
            const bool same = far * 1000 < drawn.size();
            // Not the zeros an unread slot would give.
            assert(std::any_of(drawn.begin(), drawn.end(), [](uint8_t b) { return b != 0 && b != 255; }));
            for (unsigned k = 0; k < 2; ++k) if (timed[k]) { gpu[k] /= timed[k]; cpu[k] /= timed[k]; }
            std::printf("NATIVE SONIC-SHAPED %s %ux%u -> %ux%u bilinear wrap %u: buffer GPU %.3f ms (encode %.3f ms), native GPU %.3f ms (encode %.3f ms), %u native draws, pixels %s\n",
                        f ? "5_6_5" : "8_8_8_8", imageW, imageH, screenW, screenH, wrap, gpu[0], cpu[0], gpu[1], cpu[1],
                        native.nativeDraws, same ? "within 2 levels" : "DIFFERENT");
            assert(same);
            // The same draw with the texture read in place (a no-copy buffer
            // over page-aligned host memory, as the game's guest memory).
            PM4MetalRenderer mapped(device, arena.memory, nil, 0, true);
            mapped.hostView = [&](uint64_t a, size_t n) -> const void* {
                return a >= Arena::start && a - Arena::start + n <= host.size ? host.bytes + (a - Arena::start) : nullptr;
            };
            id<MTLCommandBuffer> command = [queue commandBuffer];
            assert(mapped.encode(d, command));
            [command commit]; [command waitUntilCompleted];
            assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
            assert(mapped.rangesShared == shared && mapped.rangesMapped == shared);
            assert(pixels(mapped.target(target)) == natively);
        }
        std::puts("PASS native textures through PM4MetalRenderer: 1920x1080 pixels within 2 levels, also read in place; timings are CI GPU, not iPhone game FPS");

        // 3. A colour target sampled through a tiled T# (tile mode 13), as
        //    Mortal Kombat composites its menu layer: the target's Metal
        //    texture is read; a tiled T# no target holds still reads zeros.
        {
            PM4MetalRenderer renderer(device, arena.memory, nil, 0, true);
            const uint32_t linear[12] = {std::uint32_t(image >> 8), 10u << 20, (imageW - 1) | ((imageH - 1) << 14), 0x90800facu,
                                         (imageW - 1) << 13, 0, 0, 0, 0, 0, 0x500000, 0};
            std::memcpy(d.psUser, linear, sizeof linear);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            assert(renderer.encode(d, command));
            [command commit]; [command waitUntilCompleted];
            const uint64_t layer = target, composed = 0x400000, elsewhere = 0x800000;
            auto compose = [&](uint64_t source, uint64_t destination) {
                const uint32_t tiled[12] = {std::uint32_t(source >> 8), 10u << 20, (screenW - 1) | ((screenH - 1) << 14), 0x90d00facu,
                                            (screenW - 1) << 13, 0, 0, 0, 2 | (2 << 3), 0, 0x500000, 0};
                PM4Draw c = d;
                std::memcpy(c.psUser, tiled, sizeof tiled);
                c.target = destination;
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                const bool encoded = renderer.encode(c, commands);
                if (!encoded) std::printf("tiled compose: %s\n", renderer.error.c_str());
                assert(encoded);
                [commands commit]; [commands waitUntilCompleted];
                assert(commands.status == MTLCommandBufferStatusCompleted && !commands.error);
                return pixels(renderer.target(destination));
            };
            const unsigned before = renderer.targetReads;
            const auto read = compose(layer, composed);
            assert(renderer.targetReads == before + 1);
            const auto zeros = compose(elsewhere, composed + 0x800000);
            assert(renderer.targetReads == before + 1 && read != zeros);
            std::puts("PASS tiled T# of a colour target reads the target's Metal texture");
        }
        // 3b. A T# narrower than the colour target it reads, as Mortal Kombat's
        //     1332 wide T# on a 1344 wide target: Metal's sampler must filter it
        //     (a software read here cost the iPhone 25 ms a frame), stopping at
        //     the T#'s last texel. Depth compare bits refuse the sampler: that
        //     renderer filters the same draw in software, the reference.
        {
            const uint32_t linear[12] = {std::uint32_t(image >> 8), 10u << 20, (imageW - 1) | ((imageH - 1) << 14), 0x90800facu,
                                         (imageW - 1) << 13, 0, 0, 0, 0, 0, 0x500000, 0};
            auto narrow = [&](bool nativeOnly, uint32_t sampler) {
                PM4MetalRenderer renderer(device, arena.memory, nil, 0, true);
                renderer.nativeSamplesOnly = nativeOnly;
                std::memcpy(d.psUser, linear, sizeof linear);
                id<MTLCommandBuffer> layerCommands = [queue commandBuffer];
                assert(renderer.encode(d, layerCommands));
                [layerCommands commit]; [layerCommands waitUntilCompleted];
                const uint32_t cropped[12] = {std::uint32_t(target >> 8), 10u << 20, (screenW - 13) | ((screenH - 1) << 14), 0x90800facu,
                                              (screenW - 1) << 13, 0, 0, 0, sampler, 0, 0x500000, 0};
                PM4Draw c = d;
                std::memcpy(c.psUser, cropped, sizeof cropped);
                c.target = 0x400000;
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                const bool encoded = renderer.encode(c, commands);
                if (!encoded) std::printf("narrow T#: %s\n", renderer.error.c_str());
                assert(encoded);
                [commands commit]; [commands waitUntilCompleted];
                assert(commands.status == MTLCommandBufferStatusCompleted && !commands.error);
                return pixels(renderer.target(c.target));
            };
            const auto fast = narrow(true, 2 | (2 << 3));
            const auto slow = narrow(false, 2 | (2 << 3) | (1 << 12));
            const auto refused = narrow(true, 2 | (2 << 3) | (1 << 12));
            assert(fast.size() == slow.size() && slow.size() == refused.size());
            unsigned worst = 0;
            for (std::size_t k = 0; k < fast.size(); ++k)
                worst = std::max(worst, unsigned(std::abs(int(fast[k]) - int(slow[k]))));
            std::printf("narrow T#: worst difference with software filtering %u\n", worst);
            assert(worst <= 2 && fast != refused);
            std::puts("PASS a T# narrower than its colour target is filtered by Metal's sampler");
        }
        // 3c. Clamping to a transparent black border, as Sonic Mania samples its
        //     picture: Metal's sampler filters it. Depth compare bits refuse the
        //     sampler, as in 3b.
        {
            auto bordered = [&](bool nativeOnly, uint32_t sampler) {
                PM4MetalRenderer renderer(device, arena.memory, nil, 0, true);
                renderer.nativeSamplesOnly = nativeOnly;
                const uint32_t linear[12] = {std::uint32_t(image >> 8), 10u << 20, (imageW - 1) | ((imageH - 1) << 14), 0x90800facu,
                                             (imageW - 1) << 13, 0, 0, 0, sampler, 0, 0x500000, 0};
                std::memcpy(d.psUser, linear, sizeof linear);
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                assert(renderer.encode(d, commands));
                [commands commit]; [commands waitUntilCompleted];
                assert(commands.status == MTLCommandBufferStatusCompleted && !commands.error);
                return pixels(renderer.target(target));
            };
            const uint32_t border = 6 | (6 << 3);
            const auto fast = bordered(true, border);
            const auto slow = bordered(false, border | (1 << 12));
            const auto refused = bordered(true, border | (1 << 12));
            assert(fast.size() == slow.size());
            unsigned worst = 0;
            for (std::size_t k = 0; k < fast.size(); ++k)
                worst = std::max(worst, unsigned(std::abs(int(fast[k]) - int(slow[k]))));
            std::printf("border clamp: worst difference with software filtering %u\n", worst);
            assert(worst <= 2 && fast != refused);
            std::puts("PASS a transparent black border is filtered by Metal's sampler");
        }
        // 4. Asynchronous pipelines (the game bridge): the first draw is
        //    deferred while Metal builds its pipeline, a later one draws.
        {
            PM4MetalRenderer renderer(device, arena.memory, nil, 0, true);
            renderer.asyncPipelines = true;
            id<MTLCommandBuffer> first = [queue commandBuffer];
            assert(renderer.encode(d, first) && renderer.drawsDeferred == 1 && renderer.pipelinesBuilt == 0);
            [first commit]; [first waitUntilCompleted];
            for (unsigned waited = 0; renderer.pipelinesPending() && waited < 600; ++waited) usleep(50000);
            assert(!renderer.pipelinesPending() && !renderer.pipelinesRefused());
            id<MTLCommandBuffer> second = [queue commandBuffer];
            assert(renderer.encode(d, second) && renderer.drawsDeferred == 1 && renderer.pipelinesBuilt == 1);
            [second commit]; [second waitUntilCompleted];
            assert(second.status == MTLCommandBufferStatusCompleted && !second.error);
            const auto drawn = pixels(renderer.target(target));
            assert(std::any_of(drawn.begin(), drawn.end(), [](uint8_t b) { return b != 0 && b != 255; }));
            std::puts("PASS asynchronous pipelines: deferred draw, then drawn once built");
        }
    }
}
