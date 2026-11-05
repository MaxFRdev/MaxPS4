// Research only: where a translated pixel program's GPU time goes. One
// full-screen 1920x1080 draw (PM4MetalRendererTest's textured program,
// u = x / 1920, v = y / 1080 over a 512x256 RGBA8 texture) rendered with
// variants of the generated MSL, each removing one part, next to an empty
// pass and trivial Metal fragments on the same device. Variants change
// pixels: only the timings matter. Never selects a game backend.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
struct State { uint64_t base, size; uint32_t vs[16], ps[17], flags, count; uint64_t ranges[48]; };
static_assert(sizeof(State) == 544, "GCNDrawState layout");
std::string replaced(std::string s, const std::string& from, const std::string& to) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size(), ++count) s.replace(at, from.size(), to);
    if (!count) std::printf("variant pattern not found: %s\n", from.c_str());
    assert(count);
    return s;
}
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP shader cost: no Metal device"); return 0; }
        constexpr unsigned screenW = 1920, screenH = 1080, imageW = 512, imageH = 256;
        constexpr uint64_t texturedPS = 0x11400, image = 0x40000;
        Arena arena;
        arena.bytes.resize(image + imageW * imageH * 4 - Arena::start);
        loadFetchVS(arena);
        const float triangle[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
        for (unsigned n = 0; n < 3; ++n) for (unsigned c = 0; c < 4; ++c) arena.put(0x10e00 + 16 * n + 4 * c, bits(triangle[n][c]));
        arena.code(texturedPS, {
            0xbe94047e, 0xbefe0a7e, 0xc8280000, 0xc8290001, 0x100804ff, bits(1.0f / screenW), 0x100a06ff, bits(1.0f / screenH),
            0xf0800f00, 0x00400604, 0xd8d48055, 0x0b000004, 0xd8d48000, 0x0c000004, 0x0816190b,
            0x100e16ff, 0x41000000, 0x061014ff, 0x41200000, 0x101010ff, 0x3ccccccd, 0x7c0208f4, 0xbf860001,
            0x7e120280, 0xbefe0414, 0xf800180f, 0x09080706, 0xbf810000});
        for (unsigned n = 0; n < imageW * imageH; ++n) arena.put(image + 4 * n, n * 2654435761u);
        GCNTranslateOptions vsOptions; vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = 4;
        vsOptions.userData[0] = std::uint32_t(fetchShader); vsOptions.userData[2] = std::uint32_t(vertexTable);
        GCNTranslateOptions psOptions; psOptions.name = "ps_program"; psOptions.userCount = 12;
        const auto vs = translateGCN(arena.memory, fetchVS, vsOptions);
        const auto ps = translateGCN(arena.memory, texturedPS, psOptions);
        assert(vs && ps);
        // 32 scalar loads per pixel, as the game's costliest programs make, each a
        // gcn_locate: what the search through a draw's ranges costs.
        constexpr uint64_t scalarPS = 0x30000, scalarData = 0x31000;
        constexpr unsigned scalarLoads = 32;
        for (unsigned n = 0; n < scalarLoads; ++n) {
            arena.put(scalarPS + 8 * n, 0xc0080100u | n);     // s_load_dword s16, s[0:1], n
            arena.put(scalarPS + 8 * n + 4, 0x06000010u);     // v_add_f32 v0, s16, v0
            arena.put(scalarData + 4 * n, bits(1.0f / scalarLoads));
        }
        arena.code(scalarPS + 8 * scalarLoads, {0xf800180f, 0x00000000, 0xbf810000}); // exp mrt0, v0 x4 done vm
        const auto scalars = translateGCN(arena.memory, scalarPS, psOptions);
        assert(scalars);
        std::uint32_t inputCntl[32] = {};
        const std::string entry = gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", 0x302, inputCntl);
        const std::string scalarEntry = gcnMetalEntryPoints(vs, scalars, "vs_program", "ps_program", 0x302, inputCntl);
        const std::string trivial = R"MSL(
fragment float4 trivial_fragment(GCNVaryings in [[stage_in]]) { return float4(in.position.x / 1920.0f, in.position.y / 1080.0f, in.p0.x, 1.0f); }
fragment float4 metal_sample_fragment(GCNVaryings in [[stage_in]], texture2d<float> image [[texture(0)]]) {
    constexpr sampler linear(coord::normalized, address::repeat, filter::linear);
    return image.sample(linear, in.position.xy / float2(1920.0f, 1080.0f), level(0.0f));
}
)MSL";
        const std::string noSample = replaced(ps.source, "gcn_sample(M, t, sm, false, gf(v4), gf(v5))", "float4(gf(v4), gf(v5), 0.0f, 1.0f)");
        auto noQuad = [&](std::string s) {
            s = replaced(s, "gcn_quad_any(Q, L, ", "(");
            return replaced(s, "gcn_quad_shuffle(Q, L, ", "gcn_no_shuffle(");
        };
        const std::string noShuffle = "GCN_FN uint gcn_no_shuffle(uint value, uint lane) { return value + lane; }\n";
        const std::string noMask = replaced(replaced(entry, "uint sampleCoverage [[sample_mask]],", ""), "sampleCoverage != 0u", "true");
        const std::string entryOnly = replaced(entry, "    ps_program(L, M, Q);\n", "");
        // Before the fix: entry points zeroed every attr/param/color per invocation.
        const std::string fullClear = replaced(entry, "GCNLane L; gcn_lane_init(L);", "GCNLane L; gcn_clear(L);");
        // gcn_sample parts removed one at a time (pixels change, timings only).
        const std::string prelude = gcnMetalPrelude();
        const std::string identitySwizzle = replaced(prelude, "return gcn_texture_swizzle(t, float4(c0, c1, c2, c3));", "return float4(c0, c1, c2, c3);");
        const std::string clampOnly = replaced(prelude, "GCN_FN int gcn_wrap(int x, int size, uint mode, GCN_THREAD bool& border) {",
            "GCN_FN int gcn_wrap(int x, int size, uint mode, GCN_THREAD bool& border) { return x < 0 ? 0 : (x >= size ? size - 1 : x);");
        const std::string oneTap = replaced(prelude, "uint taps = linear ? 4u : 1u;", "uint taps = 1u;");
        const std::string noLookup = replaced(prelude, "resolved = gcn_locate(M, base, footprint, offset);",
            "offset = M.ranges[5]; resolved = true;");
        // The runtime before per-axis wrapping (same results, measured in the same run).
        // The translator's text keeps its own line ending: match each line alone.
        const std::string slotArray = replaced(replaced(prelude, "return (k == 0u ? M.p : k == 1u ? M.s.x[0] : k == 2u ? M.s.x[1] : k == 3u ? M.s.x[2] :", "return k == 0u ? M.p + o : M.s.x[min(k, GCN_SLOTS) - 1u] + o;"),
            "            k == 4u ? M.s.x[3] : k == 5u ? M.s.x[4] : M.s.x[5]) + o;", "");
        std::string previousWrap = replaced(prelude, "if (x >= 0 && x < size) return x; ", "");
        previousWrap = replaced(previousWrap, "int m = x; if (m < 0 || m >= period) { m = x % period; if (m < 0) m += period; }",
                                "int m = x % period; if (m < 0) m += period;");
        previousWrap = replaced(previousWrap, "gcn_texel_at(M, t, right ? wx1 : wx0, down ? wy1 : wy0, resolved, offset)",
                                "gcn_fetch_at(M, t, s, right ? x0 + 1 : x0, down ? y0 + 1 : y0, resolved, offset)");
        // Native textures as the game binds them (GCNNativeTexture at buffer(3)):
        // Metal's sampler filters (50aaa47), the texture is only read per texel
        // (before 50aaa47), or no T# matches and GCN filters with the larger code.
        const std::string nativeEntry = gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", 0x302, inputCntl, true);
        const std::string nativePrelude = gcnMetalPrelude(true);
        const std::string texelOnly = replaced(nativePrelude, "    if (k < 0) return false;\n    const uint4 sd",
                                               "    if (true) return false;\n    const uint4 sd");
        struct Variant { const char* name; std::string source; const char* fragment; bool draws; std::string prelude = {}; int native = -1; unsigned state = 0; };
        std::vector<Variant> variants = {
            {"empty pass (clear only)", "", "", false},
            {"trivial Metal fragment", vs.source + ps.source + entry + trivial, "trivial_fragment", true},
            {"trivial + Metal bilinear sample", vs.source + ps.source + entry + trivial, "metal_sample_fragment", true},
            {"translated program", vs.source + ps.source + entry, "gcn_fragment", true},
            {"translated, no gcn_sample", vs.source + noSample + entry, "gcn_fragment", true},
            {"translated, no quad ops", noShuffle + vs.source + noQuad(ps.source) + entry, "gcn_fragment", true},
            {"translated, no sample/quad", noShuffle + vs.source + noQuad(noSample) + entry, "gcn_fragment", true},
            {"translated, no sample_mask input", vs.source + ps.source + noMask, "gcn_fragment", true},
            {"entry only (program not run)", vs.source + ps.source + entryOnly, "gcn_fragment", true},
            {"translated, full gcn_clear (before)", vs.source + ps.source + fullClear, "gcn_fragment", true},
            {"sample: previous wrap code", vs.source + ps.source + entry, "gcn_fragment", true, previousWrap},
            {"sample: identity swizzle", vs.source + ps.source + entry, "gcn_fragment", true, identitySwizzle},
            {"sample: clamp instead of wrap", vs.source + ps.source + entry, "gcn_fragment", true, clampOnly},
            {"sample: one tap (nearest)", vs.source + ps.source + entry, "gcn_fragment", true, oneTap},
            {"sample: no range lookup", vs.source + ps.source + entry, "gcn_fragment", true, noLookup},
            {"native: Metal sampler filters", vs.source + ps.source + nativeEntry, "gcn_fragment", true, nativePrelude, 1},
            {"native: texel reads (before 50aaa47)", vs.source + ps.source + nativeEntry, "gcn_fragment", true, texelOnly, 1},
            {"native: no T# match (GCN filters)", vs.source + ps.source + nativeEntry, "gcn_fragment", true, nativePrelude, 0},
            {"scalar: 32 loads, variable slot index", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, slotArray, -1, 2},
            {"scalar: 32 loads, no range table", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, {}, -1, 1},
            {"scalar: 32 loads, 2 ranges", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, {}, -1, 2},
            {"scalar: 32 loads, 16 ranges", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, {}, -1, 3},
        };
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
        std::vector<id<MTLRenderPipelineState>> pipelines(variants.size());
        for (std::size_t k = 0; k < variants.size(); ++k) {
            if (!variants[k].draws) continue;
            NSError* error = nil;
            const std::string source = (variants[k].prelude.empty() ? prelude : variants[k].prelude) + variants[k].source;
            NSDate* compiling = [NSDate date];
            id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
            std::printf("SHADER COMPILE %-36s %8.1f ms\n", variants[k].name, -[compiling timeIntervalSinceNow] * 1000);
            if (!library) std::printf("%s MSL: %s\n", variants[k].name, error.localizedDescription.UTF8String);
            assert(library);
            MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
            descriptor.fragmentFunction = [library newFunctionWithName:[NSString stringWithUTF8String:variants[k].fragment]];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            compiling = [NSDate date];
            pipelines[k] = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            std::printf("SHADER PIPELINE %-35s %8.1f ms\n", variants[k].name, -[compiling timeIntervalSinceNow] * 1000);
            if (!pipelines[k]) std::printf("%s pipeline: %s\n", variants[k].name, error.localizedDescription.UTF8String);
            assert(pipelines[k]);
        }
        id<MTLBuffer> memory = [device newBufferWithBytes:arena.bytes.data() length:arena.bytes.size() options:MTLResourceStorageModeShared];
        // Two copied ranges over the same buffer, as PM4MetalRenderer uploads them.
        State state{}; state.count = 2;
        state.ranges[0] = Arena::start; state.ranges[1] = image - Arena::start; state.ranges[2] = 0;
        state.ranges[3] = image; state.ranges[4] = imageW * imageH * 4; state.ranges[5] = image - Arena::start;
        state.vs[0] = std::uint32_t(fetchShader); state.vs[2] = std::uint32_t(vertexTable);
        const uint32_t t[12] = {std::uint32_t(image >> 8), 10u << 20, (imageW - 1) | ((imageH - 1) << 14), 0x90800facu,
                                (imageW - 1) << 13, 0, 0, 0, 0, 0, 0x500000, 0};
        std::memcpy(state.ps, t, sizeof t);
        // The scalar program's user SGPRs point at its data. Its three tables: a
        // single window; two ranges; fifteen ranges before the one it reads.
        State states[4] = {state, state, state, state};
        for (unsigned k = 1; k < 4; ++k) { states[k].ps[0] = std::uint32_t(scalarData); states[k].ps[1] = 0; }
        states[1].count = 0; states[1].base = Arena::start; states[1].size = arena.bytes.size();
        states[3].count = 16;
        for (unsigned k = 0; k < 15; ++k) {
            states[3].ranges[3 * k] = 0x7000000000ull + (std::uint64_t(k) << 20);
            states[3].ranges[3 * k + 1] = 0x1000; states[3].ranges[3 * k + 2] = 0;
        }
        states[3].ranges[45] = Arena::start; states[3].ranges[46] = image - Arena::start; states[3].ranges[47] = 0;
        const uint32_t indices[3] = {0, 1, 2};
        auto* targetDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:screenW height:screenH mipmapped:NO];
        targetDescriptor.usage = MTLTextureUsageRenderTarget; targetDescriptor.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
        auto* imageDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:imageW height:imageH mipmapped:NO];
        imageDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> metalImage = [device newTextureWithDescriptor:imageDescriptor];
        [metalImage replaceRegion:MTLRegionMake2D(0, 0, imageW, imageH) mipmapLevel:0
                        withBytes:arena.bytes.data() + (image - Arena::start) bytesPerRow:imageW * 4];
        // The game's native textures are linear views over guest memory.
        id<MTLBuffer> linearBytes = [device newBufferWithBytes:arena.bytes.data() + (image - Arena::start) length:imageW * imageH * 4
                                                       options:MTLResourceStorageModeShared];
        imageDescriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> linearImage = [linearBytes newTextureWithDescriptor:imageDescriptor offset:0 bytesPerRow:imageW * 4];
        assert(linearImage);
        MTLArgumentDescriptor* fields[16];
        const MTLDataType types[16] = {MTLDataTypeTexture, MTLDataTypeUInt, MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeTexture,
                                       MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeTexture, MTLDataTypeUInt4, MTLDataTypeUInt4,
                                       MTLDataTypeSampler, MTLDataTypeSampler, MTLDataTypeSampler, MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeUInt4};
        for (unsigned k = 0; k < 16; ++k) {
            fields[k] = [MTLArgumentDescriptor argumentDescriptor];
            fields[k].dataType = types[k]; fields[k].index = k;
            if (types[k] == MTLDataTypeTexture) fields[k].textureType = MTLTextureType2D;
        }
        id<MTLArgumentEncoder> nativeArguments = [device newArgumentEncoderWithArguments:[NSArray arrayWithObjects:fields count:16]];
        // The S# (t[8..11]): wrap, bilinear, as PM4MetalRenderer builds its sampler.
        MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
        samplerDescriptor.supportArgumentBuffers = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
        id<MTLBuffer> nativeTables[2];
        for (unsigned enabled = 0; enabled < 2; ++enabled) {
            nativeTables[enabled] = [device newBufferWithLength:nativeArguments.encodedLength options:MTLResourceStorageModeShared];
            [nativeArguments setArgumentBuffer:nativeTables[enabled] offset:0];
            [nativeArguments setTexture:linearImage atIndex:0];
            [nativeArguments setSamplerState:sampler atIndex:10];
            std::memcpy([nativeArguments constantDataAtIndex:13], t + 8, 16);
            *static_cast<uint32_t*>([nativeArguments constantDataAtIndex:1]) = enabled ? 0x11u : 0u;
            std::memcpy([nativeArguments constantDataAtIndex:2], t, 16);
            std::memcpy([nativeArguments constantDataAtIndex:3], t + 4, 16);
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        std::vector<std::vector<double>> samples(variants.size());
        constexpr unsigned runs = 34, warmup = 4;
        for (unsigned run = 0; run < runs; ++run) for (std::size_t turn = 0; turn < variants.size(); ++turn) {
            const std::size_t k = (turn + run) % variants.size(); // rotate the order between runs
            id<MTLCommandBuffer> command = [queue commandBuffer];
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            if (variants[k].draws) {
                [encoder setRenderPipelineState:pipelines[k]];
                [encoder setViewport:MTLViewport{0, 0, double(screenW), double(screenH), 0, 1}];
                [encoder setVertexBuffer:memory offset:0 atIndex:0]; [encoder setFragmentBuffer:memory offset:0 atIndex:0];
                [encoder setVertexBytes:&states[variants[k].state] length:sizeof state atIndex:1]; [encoder setFragmentBytes:&states[variants[k].state] length:sizeof state atIndex:1];
                [encoder setVertexBytes:indices length:sizeof indices atIndex:2];
                [encoder setFragmentTexture:metalImage atIndex:0];
                if (variants[k].native >= 0) {
                    [encoder setVertexBuffer:nativeTables[variants[k].native] offset:0 atIndex:3];
                    [encoder setFragmentBuffer:nativeTables[variants[k].native] offset:0 atIndex:3];
                    [encoder useResource:linearImage usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
                }
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
            assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
            if (run >= warmup && command.GPUStartTime > 0) samples[k].push_back((command.GPUEndTime - command.GPUStartTime) * 1000);
        }
        // The shared CI GPU is noisy: the median and the minimum resist outliers better than a mean.
        std::printf("SHADER COST on %s, %ux%u full-screen draw (GPU ms over %u runs):\n", device.name.UTF8String, screenW, screenH, runs - warmup);
        for (std::size_t k = 0; k < variants.size(); ++k) {
            auto& times = samples[k];
            std::sort(times.begin(), times.end());
            const double median = times.empty() ? -1 : times[times.size() / 2], fastest = times.empty() ? -1 : times.front();
            std::printf("SHADER COST %-36s median %8.3f ms  min %8.3f ms  %6.2f ns/pixel\n", variants[k].name, median, fastest,
                        median * 1e6 / (screenW * screenH));
        }
        std::puts("PASS shader cost research (timings only, CI GPU, not iPhone game FPS)");
    }
}
