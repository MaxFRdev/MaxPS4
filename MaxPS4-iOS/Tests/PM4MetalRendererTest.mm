// One PM4 stream drawn by the software renderer (CPU, GCN interpreter) and
// by PM4MetalRenderer (GPU, translated programs); the colour targets must
// match. Scene A: two rect lists through the embedded VS, the second one
// scissored, so the Metal target must keep the first draw. Scene B: an
// indexed draw through a fetch-shader VS and a textured pixel program.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/PM4MetalRenderer.h"
#include "../Native/PM4GameMetalBridge.h"
#include "../Native/PM4SoftwareRenderer.hpp"
#include "GCNTestPrograms.h"
#include "PM4BlendTestScene.h"
#include "PM4PrimitiveTestScene.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <tuple>
#include <dispatch/dispatch.h>
#include <unistd.h>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
constexpr std::uint64_t gradientPS = 0x10100, greenPS = 0x10200, targetA = 0x11000;
constexpr std::uint64_t texturedPS = 0x11400, image = 0x14000, indexBuffer = 0x15000, targetB = 0x18000;
constexpr unsigned width = 16, height = 8;
struct Stream {
    std::vector<std::uint32_t> words;
    void set(std::uint8_t opcode, std::uint32_t offset, std::initializer_list<std::uint32_t> values) {
        words.push_back(0xc0000000U | (std::uint32_t(values.size()) << 16) | (std::uint32_t(opcode) << 8));
        words.push_back(offset);
        words.insert(words.end(), values);
    }
    void target(std::uint64_t base) {
        set(0x69, 0x318, {std::uint32_t(base >> 8), width / 8 - 1, width * height / 64 - 1, 0, (0xaU << 2) | (1U << 11), 0});
        set(0x69, 0x8e, {0xf});
        set(0x69, 0x10f, {bits(8), bits(8), bits(-4), bits(4), bits(0.5f), bits(0.5f)});
        set(0x69, 0x206, {0x43f});
        set(0x69, 0x0c, {0, width | (height << 16)});
        set(0x69, 0x191, {0});
    }
};
void readPixels(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLTexture> texture, std::uint8_t* pixels) {
    assert(texture);
    constexpr unsigned rowBytes=256;
    id<MTLBuffer> buffer=[device newBufferWithLength:rowBytes*height options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes*height];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    assert(command.status==MTLCommandBufferStatusCompleted && !command.error);
    for(unsigned y=0;y<height;++y) std::memcpy(pixels+y*width*4,static_cast<const uint8_t*>(buffer.contents)+y*rowBytes,width*4);
}
bool compare(const char* scene, const std::uint8_t* metal, const std::uint8_t* software) {
    int worst = 0;
    for (unsigned n = 0; n < width * height * 4; ++n) worst = std::max(worst, std::abs(int(metal[n]) - int(software[n])));
    if (worst > 2) {
        std::printf("%s differs (Metal/software, ARGB):\n", scene);
        for (unsigned y = 0; y < height; ++y, std::puts("")) for (unsigned x = 0; x < width; ++x) {
            const unsigned at = 4 * (y * width + x);
            std::printf("%02x%02x%02x%02x/%02x%02x%02x%02x ", metal[at + 3], metal[at + 2], metal[at + 1], metal[at],
                        software[at + 3], software[at + 2], software[at + 1], software[at]);
        }
    }
    std::printf("%s: worst channel difference %d\n", scene, worst);
    return worst <= 2;
}
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP PM4 Metal renderer: no Metal device"); return 0; }
        Arena arena;
        loadEmbeddedVS(arena); loadFetchVS(arena);
        const float triangle[3][4] = {{-1, -1, 0, 1}, {3, -1, 0, 1}, {-1, 3, 0, 1}};
        for (unsigned n = 0; n < 3; ++n) for (unsigned c = 0; c < 4; ++c) arena.put(0x10e00 + 16 * n + 4 * c, bits(triangle[n][c]));
        arena.code(gradientPS, {
            0xc80c0000, 0xc80d0001, // v_interp_p1/p2_f32 v3, attr0.x (= 1.0)
            0x100804ff, 0x3d800000, // v_mul_f32 v4, 0.0625, v2 (POS_X / 16)
            0x7e0a0280,             // v_mov_b32 v5, 0
            0xf800180f, 0x03050504, // exp mrt0, v4, v5, v5, v3 done vm
            0xbf810000});
        arena.code(greenPS, {
            0x7e080280,             // v_mov_b32 v4, 0
            0x7e0a02f2,             // v_mov_b32 v5, 1.0
            0xf800180f, 0x05040504, // exp mrt0, v4, v5, v4, v5 done vm
            0xbf810000});
        arena.code(texturedPS, {
            0xbe94047e, 0xbefe0a7e, 0xc8280000, 0xc8290001, 0x100804ff, 0x3d800000, 0x100a06ff, 0x3e000000,
            0xf0800f00, 0x00400604, 0xd8d48055, 0x0b000004, 0xd8d48000, 0x0c000004, 0x0816190b,
            0x100e16ff, 0x41000000, 0x061014ff, 0x41200000, 0x101010ff, 0x3ccccccd, 0x7c0208f4, 0xbf860001,
            0x7e120280, 0xbefe0414, 0xf800180f, 0x09080706, 0xbf810000}); // as GCNMetalTest's pixel program
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
            arena.put(image + 4 * (y * width + x), (x * 16) | ((y * 32) << 8) | (128u << 16) | (255u << 24));
        arena.put(indexBuffer, 0x00010000); arena.put(indexBuffer + 4, 0x00000002); // 16-bit 0, 1, 2
        Stream s;
        // Scene A.
        s.set(0x76, 0x48, {std::uint32_t(embeddedVS >> 8), 0, 0, 0});
        s.set(0x76, 0x08, {std::uint32_t(gradientPS >> 8), 0, 0, 0});
        s.target(targetA);
        s.set(0x69, 0x1b3, {0x102, 0x102}); // PERSP_CENTER, POS_X
        s.set(0x79, 0x242, {0x11});
        s.words.insert(s.words.end(), {0xc0012d00U, 3, 2});
        s.set(0x76, 0x08, {std::uint32_t(greenPS >> 8), 0, 0, 0});
        s.set(0x69, 0x0c, {0, 4 | (height << 16)});
        s.words.insert(s.words.end(), {0xc0012d00U, 3, 2});
        // Scene B.
        s.set(0x76, 0x48, {std::uint32_t(fetchVS >> 8), 0, 0, 4u << 1});
        s.set(0x76, 0x4c, {std::uint32_t(fetchShader), 0, std::uint32_t(vertexTable), 0});
        s.set(0x76, 0x08, {std::uint32_t(texturedPS >> 8), 0, 0, 12u << 1});
        s.set(0x76, 0x0c, {std::uint32_t(image >> 8), 10u << 20, (width - 1) | ((height - 1) << 14), 0x90800facu,
                           (width - 1) << 13, 0, 0, 0, 0x12, 0, 0x500000, 0});
        s.target(targetB);
        s.set(0x69, 0x1b3, {0x302, 0x302}); // PERSP_CENTER, POS_X, POS_Y
        s.set(0x79, 0x242, {4});
        s.words.insert(s.words.end(), {0xc0002a00U, 0});                                    // INDEX_TYPE 16-bit
        s.words.insert(s.words.end(), {0xc0042700U, 3, std::uint32_t(indexBuffer), 0, 3, 0}); // DRAW_INDEX_2
        PM4DrawStats metalStats;
        PM4DrawCollector collector(arena.memory, metalStats);
        // No guest window: each draw gets copies of the ranges its programs read,
        // taken from guest memory as it is before either renderer draws.
        PM4MetalRenderer metal(device, arena.memory, nil, 0);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        collector.execute(s.words.data(), s.words.size(), [&](const PM4Draw& d) {
            if (metal.encode(d, commands)) ++metalStats.rendered;
            else metalStats.fail(metal.error);
        });
        [commands commit]; [commands waitUntilCompleted];
        if (!metalStats.error.empty()) std::printf("metal: %s\n", metalStats.error.c_str());
        assert(commands.status == MTLCommandBufferStatusCompleted && !commands.error);
        assert(metalStats.draws == 3 && metalStats.rendered == 3 && metalStats.failed == 0 && metal.pipelinesBuilt == 3);
        // Scene B read the vertex table and vertices (one merged range) and the texture.
        assert(metal.lastRanges == 2);
        PM4SoftwareRenderer software(arena.memory);
        software.execute(s.words.data(), s.words.size());
        if (!software.stats.error.empty()) std::printf("software: %s\n", software.stats.error.c_str());
        assert(software.stats.rendered == 3 && software.stats.failed == 0);
        bool ok = true;
        for (const auto& [name, address] : {std::pair<const char*, std::uint64_t>{"scene A", targetA}, {"scene B", targetB}}) {
            id<MTLTexture> texture = metal.target(address);
            assert(texture && texture.pixelFormat == MTLPixelFormatBGRA8Unorm);
            assert(texture.storageMode == MTLStorageModePrivate);
            std::uint8_t pixels[width * height * 4] = {};
            readPixels(device, queue, texture, pixels);
            ok = compare(name, pixels, arena.bytes.data() + (address - Arena::start)) && ok;
        }
        assert(ok);
        // Offset-index packets must draw the same textured scene as DRAW_INDEX_2.
        // A junk leading index makes treating the offset as bytes (or ignoring it)
        // observable. Exercise both index widths through the real Metal renderer.
        for (const unsigned indexBytes : {2u, 4u}) {
            const std::uint64_t offsetBuffer = indexBuffer + indexBytes * 0x100;
            for (unsigned k = 0; k < 4; ++k) {
                const unsigned value = k ? k - 1 : 0xffff;
                if (indexBytes == 2) arena.put16(offsetBuffer + k * indexBytes, std::uint16_t(value));
                else arena.put(offsetBuffer + k * indexBytes, value);
            }
            auto words = s.words;
            words.resize(words.size() - 6); // Replace the final DRAW_INDEX_2.
            words.back() = indexBytes == 4 ? 1 : 0; // Its preceding INDEX_TYPE.
            words.insert(words.end(), {0xc0012600, std::uint32_t(offsetBuffer), 0,
                                       0xc0033500, 3, 1, 3, 0});
            PM4DrawStats offsetStats;
            PM4DrawCollector offsetCollector(arena.memory, offsetStats);
            PM4MetalRenderer offsetRenderer(device, arena.memory, nil, 0);
            id<MTLCommandBuffer> offsetCommands = [queue commandBuffer];
            offsetCollector.execute(words.data(), words.size(), [&](const PM4Draw& draw) {
                if (offsetRenderer.encode(draw, offsetCommands)) ++offsetStats.rendered;
                else offsetStats.fail(offsetRenderer.error);
            });
            [offsetCommands commit]; [offsetCommands waitUntilCompleted];
            assert(offsetCommands.status == MTLCommandBufferStatusCompleted && !offsetCommands.error);
            assert(offsetStats.rendered == 3 && offsetStats.failed == 0);
            std::uint8_t pixels[width * height * 4] = {};
            readPixels(device, queue, offsetRenderer.target(targetB), pixels);
            assert(compare(indexBytes == 2 ? "offset indices 16-bit" : "offset indices 32-bit",
                           pixels, arena.bytes.data() + (targetB - Arena::start)));
        }
        // Fan conversion reaches the real GPU through the shared collector.
        // Additive colour exposes double coverage on the shared diagonal.
        for (const unsigned indexBytes : {0u, 2u, 4u}) {
            Arena fanArena;
            const auto words = pm4_primitive_test::scene(fanArena, 5, indexBytes);
            PM4SoftwareRenderer reference(fanArena.memory);
            reference.execute(words.data(), words.size());
            assert(reference.stats.rendered == 1 && reference.stats.failed == 0);
            PM4DrawStats fanStats;
            PM4DrawCollector fanCollector(fanArena.memory, fanStats);
            PM4MetalRenderer fanRenderer(device, fanArena.memory, nil, 0);
            id<MTLCommandBuffer> fanCommands = [queue commandBuffer];
            fanCollector.execute(words.data(), words.size(), [&](const PM4Draw& draw) {
                if (fanRenderer.encode(draw, fanCommands)) ++fanStats.rendered;
                else fanStats.fail(fanRenderer.error);
            });
            [fanCommands commit]; [fanCommands waitUntilCompleted];
            assert(fanCommands.status == MTLCommandBufferStatusCompleted && !fanCommands.error);
            assert(fanStats.rendered == 1 && fanStats.failed == 0);
            std::uint8_t pixels[width * height * 4] = {}, expected[sizeof pixels] = {};
            const unsigned bgra[] = {16, 32, 64, 64};
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width / 2; ++x)
                for (unsigned channel = 0; channel < 4; ++channel)
                    expected[(y * width + x) * 4 + channel] = std::uint8_t(bgra[channel]);
            readPixels(device, queue, fanRenderer.target(pm4_primitive_test::target), pixels);
            assert(compare("triangle fan expected pixels", pixels, expected));
            assert(compare("triangle fan CPU/GPU", pixels,
                           fanArena.bytes.data() + (pm4_primitive_test::target - Arena::start)));
        }
        // Restart markers split fans/strips before they reach the shader.
        // The marker names a valid central vertex; no bridge may reach the gap.
        for (const unsigned primitive : {5u, 6u}) for (const unsigned indexBytes : {2u, 4u}) {
            Arena restartArena;
            const auto words = pm4_primitive_test::restartScene(restartArena, primitive, indexBytes);
            PM4SoftwareRenderer reference(restartArena.memory);
            reference.execute(words.data(), words.size());
            assert(reference.stats.rendered == 1 && reference.stats.failed == 0);
            PM4DrawStats restartStats;
            PM4DrawCollector restartCollector(restartArena.memory, restartStats);
            PM4MetalRenderer restartRenderer(device, restartArena.memory, nil, 0);
            id<MTLCommandBuffer> restartCommands = [queue commandBuffer];
            restartCollector.execute(words.data(), words.size(), [&](const PM4Draw& draw) {
                if (restartRenderer.encode(draw, restartCommands)) ++restartStats.rendered;
                else restartStats.fail(restartRenderer.error);
            });
            [restartCommands commit]; [restartCommands waitUntilCompleted];
            assert(restartCommands.status == MTLCommandBufferStatusCompleted && !restartCommands.error);
            assert(restartStats.rendered == 1 && restartStats.failed == 0);
            std::uint8_t pixels[width * height * 4] = {}, expected[sizeof pixels] = {};
            const unsigned bgra[] = {16,32,64,64};
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
                if (x < 4 || x >= 12) for (unsigned channel = 0; channel < 4; ++channel)
                    expected[(y * width + x) * 4 + channel] = std::uint8_t(bgra[channel]);
            readPixels(device, queue, restartRenderer.target(pm4_primitive_test::target), pixels);
            assert(compare("primitive restart, expected pixels", pixels, expected));
            assert(compare("primitive restart, CPU/GPU", pixels,
                           restartArena.bytes.data() + (pm4_primitive_test::target - Arena::start)));
        }
        // Depth and culling. Full-screen triangles through the fetch-shader VS,
        // at clip depth z (w 1, window depth 0.5 z + 0.5), in corner order
        // (-1,-1), (3,-1), (-1,3): counter-clockwise on screen with the target's
        // negative Y scale, clockwise with a positive one (Y flipped).
        {
            constexpr std::uint64_t redPS = 0x10300, depthTarget = 0x18000, depthBuffer = 0x1c000;
            enum Order { forward, reversed };
            struct Scene {
                Arena arena;
                Stream st;
                unsigned triangles = 0;
                Scene() {
                    loadFetchVS(arena);
                    arena.code(greenPS, {0x7e080280, 0x7e0a02f2, 0xf800180f, 0x05040504, 0xbf810000});
                    arena.code(redPS, {0x7e0802f2, 0x7e0a0280, 0xf800180f, 0x04050504, 0xbf810000});
                    st.set(0x76, 0x48, {std::uint32_t(fetchVS >> 8), 0, 0, 4u << 1});
                    st.target(depthTarget);
                    st.set(0x69, 0x1b3, {0x2, 0x2}); // PERSP_CENTER
                    st.set(0x79, 0x242, {4});
                }
                // DB_Z_INFO Z_32_FLOAT, DB_Z_WRITE_BASE, DB_DEPTH_SIZE 16x8, DB_DEPTH_CLEAR 1.
                void depthBufferOn() {
                    st.set(0x69, 0x10, {3});
                    st.set(0x69, 0x14, {std::uint32_t(depthBuffer >> 8)});
                    st.set(0x69, 0x16, {(width / 8 - 1) | ((height / 8 - 1) << 11)});
                    st.set(0x69, 0x0b, {bits(1.0f)});
                }
                void ps(std::uint64_t address) { st.set(0x76, 0x08, {std::uint32_t(address >> 8), 0, 0, 0}); }
                void triangle(float z, Order order) {
                    const std::uint64_t table = 0x13000 + 16 * triangles, vertices = 0x13400 + 48 * triangles;
                    ++triangles;
                    const float corners[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
                    for (unsigned n = 0; n < 3; ++n) {
                        const unsigned c = order == forward ? n : (3 - n) % 3;
                        const float v[4] = {corners[c][0], corners[c][1], z, 1};
                        for (unsigned k = 0; k < 4; ++k) arena.put(vertices + 16 * n + 4 * k, bits(v[k]));
                    }
                    arena.code(table, {std::uint32_t(vertices), 16u << 16, 3, 0x77fac});
                    st.set(0x76, 0x4c, {std::uint32_t(fetchShader), 0, std::uint32_t(table), 0});
                    st.words.insert(st.words.end(), {0xc0012d00U, 3, 2}); // DRAW_INDEX_AUTO, 3 vertices
                }
            };
            // Every pixel of the target must be `bgra`; `draws` reached the GPU.
            auto check = [&](const char* name, Scene& scene, const std::uint8_t (&bgra)[4], unsigned depthDraws, unsigned depthOnly) {
                PM4DrawStats stats;
                PM4DrawCollector sceneCollector(scene.arena.memory, stats);
                sceneCollector.depthOnlyDraws = true;
                PM4MetalRenderer renderer(device, scene.arena.memory, nil, 0);
                id<MTLCommandBuffer> sceneCommands = [queue commandBuffer];
                renderer.beginBatch();
                sceneCollector.execute(scene.st.words.data(), scene.st.words.size(), [&](const PM4Draw& d) {
                    if (renderer.encode(d, sceneCommands)) ++stats.rendered;
                    else stats.fail(renderer.error);
                });
                renderer.endBatch();
                [sceneCommands commit]; [sceneCommands waitUntilCompleted];
                if (!stats.error.empty()) std::printf("%s: %s\n", name, stats.error.c_str());
                assert(sceneCommands.status == MTLCommandBufferStatusCompleted && !sceneCommands.error);
                assert(stats.failed == 0 && stats.rendered == stats.draws - stats.skipped);
                if (renderer.depthDraws != depthDraws || renderer.depthOnlyDraws != depthOnly)
                    std::printf("%s: %u depth draws, %u without colour\n", name, renderer.depthDraws, renderer.depthOnlyDraws);
                assert(renderer.depthDraws == depthDraws && renderer.depthOnlyDraws == depthOnly);
                assert(!depthDraws || renderer.depthTarget(depthBuffer).pixelFormat == MTLPixelFormatDepth32Float);
                std::uint8_t pixels[width * height * 4] = {}, expected[sizeof pixels] = {};
                for (unsigned n = 0; n < width * height; ++n) std::memcpy(expected + 4 * n, bgra, 4);
                readPixels(device, queue, renderer.target(depthTarget), pixels);
                assert(compare(name, pixels, expected));
            };
            const std::uint8_t green[4] = {0, 255, 0, 255}, red[4] = {0, 0, 255, 255};
            {
                // A near triangle hides a far one drawn after it (LESS, written).
                Scene scene;
                scene.depthBufferOn();
                scene.st.set(0x69, 0x200, {2 | 4 | (1 << 4)});
                scene.ps(greenPS); scene.triangle(0.2f, forward);
                scene.ps(redPS); scene.triangle(0.8f, forward);
                check("depth: near hides far", scene, green, 2, 0);
                // Without the depth test, the last triangle wins.
                Scene flat;
                flat.ps(greenPS); flat.triangle(0.2f, forward);
                flat.ps(redPS); flat.triangle(0.8f, forward);
                check("depth off: last draw wins", flat, red, 0, 0);
            }
            {
                // A depth pre-pass with no colour and no pixel program (PS 0),
                // then a farther red triangle fails and a nearer green passes.
                Scene scene;
                scene.depthBufferOn();
                scene.st.set(0x69, 0x200, {2 | 4 | (1 << 4)});
                scene.st.set(0x69, 0x8e, {0});
                scene.ps(0); scene.triangle(0.2f, forward);
                scene.st.set(0x69, 0x8e, {0xf});
                scene.ps(redPS); scene.triangle(0.8f, forward);
                scene.ps(greenPS); scene.triangle(0.1f, forward);
                check("depth-only pre-pass", scene, green, 3, 1);
            }
            {
                // A depth clear (DB_RENDER_CONTROL) back to 1 lets a far triangle
                // replace a near one.
                Scene scene;
                scene.depthBufferOn();
                scene.st.set(0x69, 0x200, {2 | 4 | (1 << 4)});
                scene.ps(greenPS); scene.triangle(0.2f, forward);
                scene.st.set(0x69, 0x00, {1});
                scene.st.set(0x69, 0x8e, {0});
                scene.st.set(0x69, 0x200, {2 | 4 | (7 << 4)});
                scene.ps(0); scene.triangle(0.5f, forward);
                scene.st.set(0x69, 0x00, {0});
                scene.st.set(0x69, 0x8e, {0xf});
                scene.st.set(0x69, 0x200, {2 | 4 | (1 << 4)});
                scene.ps(redPS); scene.triangle(0.8f, forward);
                check("depth clear", scene, red, 3, 1);
            }
            {
                // Back faces culled, counter-clockwise front (FACE 0): the
                // reversed (clockwise) red triangle is culled.
                Scene scene;
                scene.st.set(0x69, 0x205, {2});
                scene.ps(greenPS); scene.triangle(0.5f, forward);
                scene.ps(redPS); scene.triangle(0.5f, reversed);
                check("cull back, CCW front", scene, green, 0, 0);
                // Front faces culled instead: the forward green one goes.
                Scene front;
                front.st.set(0x69, 0x205, {1});
                front.ps(redPS); front.triangle(0.5f, reversed);
                front.ps(greenPS); front.triangle(0.5f, forward);
                check("cull front, CCW front", front, red, 0, 0);
                // Positive Y scale (flipped): forward is clockwise on screen,
                // the front face with FACE 1, so the reversed one is culled.
                Scene flipped;
                flipped.st.set(0x69, 0x10f, {bits(8), bits(8), bits(4), bits(4), bits(0.5f), bits(0.5f)});
                flipped.st.set(0x69, 0x205, {2 | 4});
                flipped.ps(greenPS); flipped.triangle(0.5f, forward);
                flipped.ps(redPS); flipped.triangle(0.5f, reversed);
                check("cull back, CW front, Y flipped", flipped, green, 0, 0);
                // Both faces culled: nothing after the first draw.
                Scene both;
                both.ps(greenPS); both.triangle(0.5f, forward);
                both.st.set(0x69, 0x205, {3});
                both.ps(redPS); both.triangle(0.5f, forward);
                check("cull both faces", both, green, 0, 0);
            }
            std::puts("PASS depth test, depth-only pre-pass with no pixel program, depth clear, face culling (both windings, flipped Y)");
            // Other colour target formats: the green triangle into 16_16_16_16 FLOAT
            // (8 bytes a pixel, halves 0, 1, 0, 1) and 2_10_10_10 UNORM (G and A all ones).
            for (const auto& [info, expected, bytes] : {std::tuple<std::uint32_t, std::uint64_t, unsigned>{(0xcu << 2) | (7u << 8), 0x3c0000003c000000ull, 8},
                                                       {0x9u << 2, 0xc00ffc00ull, 4}}) {
                Scene scene;
                scene.st.set(0x69, 0x31c, {info});
                scene.ps(greenPS); scene.triangle(0.5f, forward);
                PM4DrawStats stats;
                PM4DrawCollector formatCollector(scene.arena.memory, stats);
                PM4MetalRenderer renderer(device, scene.arena.memory, nil, 0);
                id<MTLCommandBuffer> formatCommands = [queue commandBuffer];
                // Without otherTargetFormats the collector refuses the format.
                formatCollector.execute(scene.st.words.data(), scene.st.words.size(), [&](const PM4Draw&) { assert(false); });
                assert(stats.failed == 1 && stats.error == "colour target format");
                PM4DrawStats otherStats;
                PM4DrawCollector otherCollector(scene.arena.memory, otherStats);
                otherCollector.otherTargetFormats = true;
                otherCollector.execute(scene.st.words.data(), scene.st.words.size(), [&](const PM4Draw& d) {
                    if (renderer.encode(d, formatCommands)) ++otherStats.rendered;
                    else otherStats.fail(renderer.error);
                });
                if (!otherStats.error.empty()) std::printf("target format 0x%x: %s\n", info, otherStats.error.c_str());
                assert(otherStats.failed == 0 && otherStats.rendered == 1);
                id<MTLTexture> texture = renderer.target(depthTarget);
                assert(texture && PM4MetalRenderer::bytesPerPixel(texture.pixelFormat) == bytes);
                id<MTLBuffer> out = [device newBufferWithLength:width * height * bytes options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> blit = [formatCommands blitCommandEncoder];
                [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(width, height, 1) toBuffer:out destinationOffset:0
               destinationBytesPerRow:width * bytes destinationBytesPerImage:width * height * bytes];
                [blit endEncoding];
                [formatCommands commit]; [formatCommands waitUntilCompleted];
                assert(formatCommands.status == MTLCommandBufferStatusCompleted && !formatCommands.error);
                for (unsigned n = 0; n < width * height; ++n) {
                    std::uint64_t pixel = 0;
                    std::memcpy(&pixel, static_cast<const std::uint8_t*>(out.contents) + n * bytes, bytes);
                    if (pixel != expected) std::printf("target format 0x%x pixel %u: 0x%llx\n", info, n, (unsigned long long)pixel);
                    assert(pixel == expected);
                }
            }
            assert(PM4MetalRenderer::targetFormat(0xa, 0, 1) == MTLPixelFormatBGRA8Unorm);
            assert(PM4MetalRenderer::targetFormat(0xa, 6, 0) == MTLPixelFormatRGBA8Unorm_sRGB);
            assert(PM4MetalRenderer::targetFormat(0xc, 7, 1) == MTLPixelFormatInvalid);
            assert(PM4MetalRenderer::targetFormat(0xa, 4, 0) == MTLPixelFormatInvalid); // UINT
            std::puts("PASS colour targets in 16_16_16_16 FLOAT and 2_10_10_10 UNORM");
        }
        // Blend the same foreground/background with all supported operations.
        // Compare GPU pixels to fixed expected bytes and the software renderer.
        for (const auto& test : pm4_blend_test::cases) {
            Arena blendArena;
            pm4_blend_test::load(blendArena, test.source);
            const auto words = pm4_blend_test::stream(test.control, test.mask, test.constant);
            PM4SoftwareRenderer reference(blendArena.memory);
            reference.execute(words.data(), words.size());
            assert(reference.stats.rendered == 2 && reference.stats.failed == 0);
            PM4DrawStats blendStats;
            PM4DrawCollector blendCollector(blendArena.memory, blendStats);
            PM4MetalRenderer blendRenderer(device, blendArena.memory, nil, 0);
            id<MTLCommandBuffer> blendCommands = [queue commandBuffer];
            blendCollector.execute(words.data(), words.size(), [&](const PM4Draw& draw) {
                if (blendRenderer.encode(draw, blendCommands)) ++blendStats.rendered;
                else blendStats.fail(blendRenderer.error);
            });
            [blendCommands commit]; [blendCommands waitUntilCompleted];
            assert(blendCommands.status == MTLCommandBufferStatusCompleted && !blendCommands.error);
            assert(blendStats.rendered == 2 && blendStats.failed == 0);
            std::uint8_t pixels[width * height * 4] = {}, expected[sizeof pixels] = {};
            constexpr unsigned bgra[] = {2, 1, 0, 3};
            for (unsigned pixel = 0; pixel < width * height; ++pixel) for (unsigned channel = 0; channel < 4; ++channel)
                expected[pixel * 4 + bgra[channel]] = std::uint8_t(test.expected[channel]);
            readPixels(device, queue, blendRenderer.target(pm4_blend_test::target), pixels);
            assert(compare(test.name, pixels, expected));
            assert(compare(test.name, pixels, blendArena.bytes.data() + (pm4_blend_test::target - Arena::start)));
        }
        // Constants are dynamic state, not part of the pipeline cache key.
        // Reuse both the pipeline and render pass while changing them.
        {
            Arena constantArena;
            pm4_blend_test::load(constantArena);
            PM4DrawStats constantStats;
            PM4DrawCollector constantCollector(constantArena.memory, constantStats);
            PM4MetalRenderer constantRenderer(device, constantArena.memory, nil, 0);
            id<MTLCommandBuffer> constantCommands = [queue commandBuffer];
            constantRenderer.beginBatch();
            for (const auto value : {std::array<float,4>{0.25f,0.5f,0.75f,0.25f}, std::array<float,4>{1,0,0,1}}) {
                const auto words = pm4_blend_test::stream(pm4_blend_test::enabled | 13, 15, value);
                constantCollector.execute(words.data(), words.size(), [&](const PM4Draw& draw) {
                    if (constantRenderer.encode(draw, constantCommands)) ++constantStats.rendered;
                    else constantStats.fail(constantRenderer.error);
                });
            }
            constantRenderer.endBatch();
            [constantCommands commit]; [constantCommands waitUntilCompleted];
            assert(constantCommands.status == MTLCommandBufferStatusCompleted && !constantCommands.error);
            assert(constantStats.rendered == 4 && constantStats.failed == 0);
            assert(constantRenderer.pipelinesBuilt == 2 && constantRenderer.renderPasses == 1);
            std::uint8_t pixels[width * height * 4] = {}, expected[sizeof pixels] = {};
            for (unsigned pixel = 0; pixel < width * height; ++pixel) {
                expected[pixel * 4 + 2] = 191;
                expected[pixel * 4 + 3] = 128;
            }
            readPixels(device, queue, constantRenderer.target(pm4_blend_test::target), pixels);
            assert(compare("changed blend constants, cached pipeline and batched encoder", pixels, expected));
        }
        // A real translated pixel shader samples BC1 through BC5 through copied resource
        // ranges. Native-texture mode must fall back to the same buffer decoder.
        for (unsigned format : {35u,36u,37u,38u,39u}) for (unsigned number : {0u,1u}) {
            if (number && format < 38) continue;
            const unsigned blockBytes = (format == 35 || format == 38) ? 8 : 16;
            Arena compressed;
            compressed.bytes = arena.bytes;
            for (unsigned block = 0; block < width * height / 16; ++block) {
                auto at = image + block * blockBytes;
                if (format >= 38) {
                    for (unsigned c = 0; c < (format == 38 ? 1u : 2u); ++c) {
                        const std::uint64_t channel = ((c ? 0x053977053977ull : 0xfac688fac688ull) << 16) |
                            (number ? ((block + c) % 2 ? 0x7f80ull : 0x807full) : ((block + c) % 2 ? 0xff00ull : 0x00ffull));
                        compressed.put(at + c * 8, std::uint32_t(channel));
                        compressed.put(at + c * 8 + 4, std::uint32_t(channel >> 32));
                    }
                    continue;
                }
                if (format != 35) {
                    const std::uint64_t alpha = format == 36 ? 0xfedcba9876543210ull :
                        (0xfac688fac688ull << 16) | (block % 2 ? 0xff00ull : 0x00ffull);
                    compressed.put(at, std::uint32_t(alpha));
                    compressed.put(at + 4, std::uint32_t(alpha >> 32)); at += 8;
                }
                compressed.put(at, block % 2 ? 0xf80007e0 : 0x07e0f800);
                compressed.put(at + 4, 0x934e39e4);
            }
            Stream bc1;
            bc1.words = s.words; bc1.words.resize(bc1.words.size() - 6);
            bc1.set(0x76, 0x0c, {std::uint32_t(image >> 8),(format << 20) | (number << 26),(width-1)|((height-1)<<14),
                0x90800fac,(width-1)<<13,0,0,0,0x12,0,0x500000,0});
            bc1.words.insert(bc1.words.end(), {0xc0042700,3,std::uint32_t(indexBuffer),0,3,0});
            PM4SoftwareRenderer reference(compressed.memory);
            reference.execute(bc1.words.data(), bc1.words.size());
            assert(reference.stats.rendered == 3 && reference.stats.failed == 0);
            for (const bool native : {false,true}) {
                PM4DrawStats stats;
                PM4DrawCollector collector(compressed.memory,stats);
                PM4MetalRenderer renderer(device,compressed.memory,nil,0,native);
                id<MTLCommandBuffer> command = [queue commandBuffer];
                collector.execute(bc1.words.data(),bc1.words.size(),[&](const PM4Draw& draw) {
                    if (renderer.encode(draw,command)) ++stats.rendered;
                    else stats.fail(renderer.error);
                });
                [command commit]; [command waitUntilCompleted];
                assert(command.status == MTLCommandBufferStatusCompleted && !command.error);
                if (stats.rendered != 3 || stats.failed != 0 || renderer.lastRanges != 2)
                    std::printf("BC%u%s: %u rendered, %u failed (%s), %u ranges\n", format - 34, native ? " native" : "",
                                stats.rendered, stats.failed, stats.error.c_str(), renderer.lastRanges);
                assert(stats.rendered == 3 && stats.failed == 0 && renderer.lastRanges == 2);
                assert(renderer.nativeDraws == 0);
                std::uint8_t pixels[width * height * 4] = {};
                readPixels(device,queue,renderer.target(targetB),pixels);
                const std::string label = "BC" + std::to_string(format - 34) + (number ? " SNORM" : " UNORM") +
                    (native ? " native-texture fallback" : " translated pixel shader");
                assert(compare(label.c_str(),
                               pixels,compressed.bytes.data() + (targetB - Arena::start)));
            }
        }
        // Prototype native texel reads: same pixels, with scene B's T# read
        // through a texture viewing the draw's buffer whenever Metal accepts
        // its alignment. Per-draw copies, then the whole guest window.
        const NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm];
        const bool viewable = alignment && (width * 4) % alignment == 0 && (image - Arena::start) % alignment == 0 && 256 % alignment == 0;
        id<MTLBuffer> window = [device newBufferWithBytes:arena.bytes.data() length:arena.bytes.size() options:MTLResourceStorageModeShared];
        assert(window);
        id<MTLBuffer> guests[] = {nil, window};
        for (id<MTLBuffer> guest : guests) {
            PM4DrawStats nativeStats;
            PM4DrawCollector nativeCollector(arena.memory, nativeStats);
            PM4MetalRenderer native(device, arena.memory, guest, guest ? Arena::start : 0, true);
            id<MTLCommandBuffer> nativeCommands = [queue commandBuffer];
            nativeCollector.execute(s.words.data(), s.words.size(), [&](const PM4Draw& d) {
                if (!native.encode(d, nativeCommands)) std::printf("native: %s\n", native.error.c_str());
                else ++nativeStats.rendered;
            });
            [nativeCommands commit]; [nativeCommands waitUntilCompleted];
            assert(nativeCommands.status == MTLCommandBufferStatusCompleted && !nativeCommands.error);
            assert(nativeStats.rendered == 3 && native.nativeDraws == (viewable ? 1u : 0u));
            for (const auto address : {targetA, targetB}) {
                std::uint8_t pixels[width * height * 4] = {};
                readPixels(device, queue, native.target(address), pixels);
                assert(compare(guest ? "native textures, guest window" : "native textures, copies", pixels,
                               arena.bytes.data() + (address - Arena::start)));
                std::uint8_t buffered[sizeof pixels] = {};
                readPixels(device, queue, metal.target(address), buffered);
                // Metal's sampler filters natively: within 2 levels of the buffer path.
                assert(compare("native textures against the buffer path", pixels, buffered));
            }
        }
        std::printf("PASS native RGBA8 texture prototype matches the software renderer (%s, alignment %lu)\n",
                    viewable ? "native reads used" : "buffer fallback", (unsigned long)alignment);
        // Identical stream with target-compatible draws sharing tile memory.
        // State changes (pipeline/scissor), target switches, repeated batches
        // and the first clear must match the original single-draw passes.
        PM4DrawStats batchStats;
        PM4DrawCollector batchCollector(arena.memory, batchStats);
        PM4MetalRenderer batched(device, arena.memory, nil, 0);
        assert(metal.renderPasses == 3);
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            id<MTLCommandBuffer> batch = [queue commandBuffer];
            batched.beginBatch();
            batchCollector.execute(s.words.data(), s.words.size(), [&](const PM4Draw& draw) {
                assert(batched.encode(draw, batch));
            });
            batched.endBatch();
            [batch commit]; [batch waitUntilCompleted];
            assert(batch.status == MTLCommandBufferStatusCompleted && !batch.error);
            assert(batched.renderPasses == 2 * (iteration + 1));
            assert(batched.buffersBuilt == 3);
            assert(batched.buffersReused == 3 * iteration);
            for (const auto address : {targetA, targetB}) {
                std::uint8_t pixels[width * height * 4]{};
                readPixels(device, queue, batched.target(address), pixels);
                assert(compare("batched draws", pixels, arena.bytes.data() + (address - Arena::start)));
            }
        }
        std::puts("PASS batching: 3 draws in 2 passes, identical pixels across 3 submissions");
        std::puts("PASS uploads: 3 buffers allocated, 6 reused after GPU completion across 3 submissions");
        // Two draws in one command buffer must retain different texture bytes,
        // even if the guest overwrites that texture between their snapshots.
        PM4Draw textured;
        PM4DrawStats captureStats;
        PM4DrawCollector capture(arena.memory, captureStats);
        capture.execute(s.words.data(), s.words.size(), [&](const PM4Draw& draw) {
            if (draw.ps == texturedPS) textured = draw;
        });
        assert(textured.ps == texturedPS);
        Arena isolationArena;
        isolationArena.bytes = arena.bytes;
        PM4MetalRenderer isolation(device, isolationArena.memory, nil, 0);
        id<MTLCommandBuffer> isolated = [queue commandBuffer];
        isolation.beginBatch();
        textured.target = targetA;
        assert(isolation.encode(textured, isolated));
        std::memset(isolationArena.bytes.data() + (image - Arena::start), 0, width * height * 4);
        PM4SoftwareRenderer blankReference(isolationArena.memory);
        blankReference.execute(s.words.data(), s.words.size());
        assert(blankReference.stats.failed == 0);
        textured.target = targetB;
        assert(isolation.encode(textured, isolated));
        isolation.endBatch();
        assert(isolation.buffersBuilt == 2 && isolation.buffersReused == 0);
        [isolated commit]; [isolated waitUntilCompleted];
        assert(isolated.status == MTLCommandBufferStatusCompleted && !isolated.error);
        std::uint8_t isolatedPixels[width * height * 4]{};
        readPixels(device, queue, isolation.target(targetA), isolatedPixels);
        assert(compare("immutable upload before guest change", isolatedPixels, arena.bytes.data() + (targetB - Arena::start)));
        readPixels(device, queue, isolation.target(targetB), isolatedPixels);
        assert(compare("fresh upload after guest change", isolatedPixels, isolationArena.bytes.data() + (targetB - Arena::start)));
        // Isolate cross-command-buffer rendering from the bridge/display copy.
        PM4DrawStats separateStats;
        PM4DrawCollector separateCollector(arena.memory,separateStats);
        PM4MetalRenderer separate(device,arena.memory,nil,0);
        for (const auto& packet : decodePM4Type3(s.words.data(),s.words.size()).packets) {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            separateCollector.execute(s.words.data()+packet.headerWord,packet.payloadCount+1,[&](const PM4Draw& d) {
                assert(separate.encode(d,command));
            });
            [command commit]; [command waitUntilCompleted];
            assert(command.status==MTLCommandBufferStatusCompleted && !command.error);
        }
        std::uint8_t separatePixels[width*height*4]{};
        readPixels(device,queue,separate.target(targetB),separatePixels);
        const bool separateOK=compare("separate command buffers",separatePixels,arena.bytes.data()+(targetB-Arena::start));
        // The app bridge receives separate submissions; register state must
        // persist. It returns immutable cropped display copies, not live targets.
        Arena* reader = &arena;
        MaxPS4GameMetalBridge* bridge = [[MaxPS4GameMetalBridge alloc] initWithDevice:device
            reader:^BOOL(uint64_t address, void* destination, size_t count) {
                return reader->memory.read(address,destination,count);
            }];
        assert(bridge && !bridge.failure);
        for (const auto& packet : decodePM4Type3(s.words.data(),s.words.size()).packets)
            assert([bridge submit:s.words.data()+packet.headerWord count:packet.payloadCount+1]==0);
        auto readback = [&](id<MTLTexture> source, std::uint8_t* pixels) {
            // Display copies finish on the bridge's queue: wait as the screen does.
            while (bridge.displayEvent.signaledValue < bridge.displayEventValue) usleep(100);
            readPixels(device,queue,source,pixels);
        };
        id<MTLTexture> saved = [bridge copyTargetAtAddress:targetB width:width height:height];
        assert(saved && ![bridge copyTargetAtAddress:0 width:width height:height]);
        assert(saved.storageMode == MTLStorageModePrivate);
        assert([bridge.report containsString:@"Dernier lot : CPU"] && [bridge.report containsString:@"Copie image : CPU"]);
        assert(![bridge copyTargetAtAddress:targetA width:width+1 height:height]);
        std::uint8_t before[width*height*4]{}, after[sizeof before]{};
        readback([bridge copyTargetAtAddress:targetA width:width height:height],after);
        const bool bridgeA=compare("app bridge A",after,arena.bytes.data()+(targetA-Arena::start));
        readback(saved,before);
        const bool bridgeB=compare("app bridge B",before,arena.bytes.data()+(targetB-Arena::start));
        assert(separateOK && bridgeA && bridgeB);
        assert([bridge submit:s.words.data() count:s.words.size()]==0);
        assert([bridge.report containsString:@"5 passes"]);
        readback([bridge copyTargetAtAddress:targetB width:width height:height],after);
        assert(compare("batched app bridge",after,arena.bytes.data()+(targetB-Arena::start)));
        Stream next;
        next.set(0x76,0x08,{std::uint32_t(greenPS>>8),0,0,0});
        next.words.insert(next.words.end(),{0xc0012d00U,3,2});
        assert([bridge submit:next.words.data() count:next.words.size()]==0);
        readback(saved,after); assert(std::memcmp(before,after,sizeof before)==0);
        id<MTLTexture> changed = [bridge copyTargetAtAddress:targetB width:width height:height];
        readback(changed,after); assert(std::memcmp(before,after,sizeof before)!=0);
        // Clearing EXEC before a done export kills the pixel. The fragment
        // sample mask must preserve the existing target, without reclassifying
        // originally covered invocations as helper threads.
        constexpr uint64_t killedPS=0x11600;
        arena.code(killedPS,{0xbefe0480,0xf800180f,0x09080706,0xbf810000});
        std::uint8_t green[sizeof before]{}; std::memcpy(green,after,sizeof green);
        Stream killed;
        killed.set(0x76,0x08,{std::uint32_t(killedPS>>8),0,0,0});
        killed.words.insert(killed.words.end(),{0xc0012d00U,3,2});
        assert([bridge submit:killed.words.data() count:killed.words.size()]==0);
        readback([bridge copyTargetAtAddress:targetB width:width height:height],after);
        assert(std::memcmp(green,after,sizeof green)==0);
        // A diagnostic read cannot wait behind a submission holding the renderer
        // lock. The simulated slow reader is released even on a timeout.
        auto entered = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
        auto reported = dispatch_semaphore_create(0), submitted = dispatch_semaphore_create(0);
        std::atomic<bool> blockOnce{true}; auto* block = &blockOnce;
        MaxPS4GameMetalBridge* slow = [[MaxPS4GameMetalBridge alloc] initWithDevice:device
            reader:^BOOL(uint64_t address, void* destination, size_t count) {
                if (block->exchange(false)) {
                    dispatch_semaphore_signal(entered);
                    dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
                }
                return reader->memory.read(address,destination,count);
            }];
        const auto* streamWords = s.words.data(); const auto streamCount = s.words.size();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            assert([slow submit:streamWords count:streamCount] == 0);
            dispatch_semaphore_signal(submitted);
        });
        assert(dispatch_semaphore_wait(entered,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC))==0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            assert(slow.report.length > 0);
            dispatch_semaphore_signal(reported);
        });
        const bool nonblocking = dispatch_semaphore_wait(reported,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC))==0;
        dispatch_semaphore_signal(release);
        assert(dispatch_semaphore_wait(submitted,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC))==0);
        if (!nonblocking) dispatch_semaphore_wait(reported,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC));
        assert(nonblocking);
        std::puts("PASS diagnostic snapshot is readable while the Metal submission is blocked");
        // A compute dispatch writing guest memory waits for the GPU inside the
        // submission, lock held. The previous submission and display copy
        // complete during that time: their handlers must not stop Metal from
        // finishing the awaited work. The pause lets them fire under the lock.
        Arena computeArena;
        loadComputeCS(computeArena);
        Arena* computeMemory = &computeArena;
        std::atomic<bool> computing{false}, pauseOnce{false}; std::atomic<unsigned> guestWrites{0};
        auto* inCompute = &computing; auto* pause = &pauseOnce; auto* writes = &guestWrites;
        MaxPS4GameMetalBridge* barrier = [[MaxPS4GameMetalBridge alloc] initWithDevice:device
            reader:^BOOL(uint64_t address, void* destination, size_t count) {
                if (!inCompute->load()) return reader->memory.read(address,destination,count);
                if (pause->exchange(false)) usleep(50000);
                return computeMemory->memory.read(address,destination,count);
            }
            writer:^int32_t(uint64_t address, const void* source, size_t count, uint64_t*) {
                ++*writes;
                return computeMemory->memory.write(address,source,count) ? 0 : -1;
            } view:nil nativeTextures:NO];
        Stream computeStream;
        computeStream.set(0x76,0x207,{32,1,1});
        computeStream.set(0x76,0x20c,{std::uint32_t(computeCS>>8),0});
        computeStream.set(0x76,0x213,{0xa0}); // 16 user SGPRs, thread group id x
        computeStream.words.insert(computeStream.words.end(),{0xc0107600U,0x240}); // COMPUTE_USER_DATA_0..15
        computeStream.words.insert(computeStream.words.end(),computeSgprs,computeSgprs+16);
        computeStream.words.insert(computeStream.words.end(),{0xc0031500U,2,1,1,1}); // DISPATCH_DIRECT
        const auto* computeWords = computeStream.words.data(); const auto computeCount = computeStream.words.size();
        auto barrierDone = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            for (unsigned round = 0; round < 5; ++round) {
                inCompute->store(false);
                assert([barrier submit:streamWords count:streamCount] == 0);
                assert([barrier copyTargetAtAddress:targetB width:width height:height]);
                inCompute->store(true); pause->store(true);
                assert([barrier submit:computeWords count:computeCount] == 0);
            }
            dispatch_semaphore_signal(barrierDone);
        });
        const bool barrierFinished = dispatch_semaphore_wait(barrierDone,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC))==0;
        if (!barrierFinished) std::printf("FAIL compute barrier deadlocked:\n%s\n", barrier.report.UTF8String);
        assert(barrierFinished);
        if (!guestWrites.load()) std::printf("FAIL no guest write reached the barrier:\n%s\n", barrier.report.UTF8String);
        assert(guestWrites.load() >= 5);
        assert([barrier.report containsString:@"Barrières compute : 5 ("] && [barrier.report containsString:@"CS 0x11000"]);
        std::puts("PASS compute barrier completes with submissions and display copies in flight");
        // The same with Gnmx's fill: no kernel, no barrier, into guest memory
        // or a target.
        constexpr std::uint64_t fillCS = 0x12000, fillTable = 0x12200, fillLimits = 0x12300, fillGuest = 0x17000;
        constexpr std::uint32_t fillValue = 0x80402010;
        computeArena.code(fillCS, {0xbeeb03ff, 0xd, 0x8f6a860c, 0xc2060900, 0x4a00006a, 0xbf8c007f, 0x7da8000c, 0xbf88000a,
                                   0xc2040901, 0xbf8c007f, 0x36020008, 0xe0002000, 0x80000101, 0xd1a80000, 0x00020006,
                                   0xbf8c0f70, 0xe0102000, 0x80010100, 0xbf810000});
        // Sonic Mania's build of the same program: s_mul_i32 vcc_lo, s12, 64.
        constexpr std::uint64_t fillMulCS = 0x12400;
        computeArena.code(fillMulCS, {0xbeeb03ff, 0xd, 0x936ac00c, 0xc2060900, 0x4a00006a, 0xbf8c007f, 0x7da8000c, 0xbf88000a,
                                      0xc2040901, 0xbf8c007f, 0x36020008, 0xe0002000, 0x80000101, 0xd1a80000, 0x00020006,
                                      0xbf8c0f70, 0xe0102000, 0x80010100, 0xbf810000});
        computeArena.put(fillTable, fillValue);
        auto fill = [&](std::uint64_t destination, std::uint32_t count, std::uint64_t program = fillCS) {
            computeArena.put(fillLimits, count); computeArena.put(fillLimits + 4, 0);
            Stream stream;
            stream.set(0x76,0x207,{64,1,1});
            stream.set(0x76,0x20c,{std::uint32_t(program>>8),0});
            stream.set(0x76,0x213,{0x98}); // 12 user SGPRs, thread group id x
            stream.set(0x76,0x240,{std::uint32_t(fillTable),4u<<16,1,0x20024004,std::uint32_t(destination),0x7000|4u<<16,count,0x18024004,
                                   std::uint32_t(fillLimits),16u<<16,1,0x20077fac});
            stream.words.insert(stream.words.end(),{0xc0031500U,(count+63)/64,1,1,1});
            assert([barrier submit:stream.words.data() count:stream.words.size()] == 0);
        };
        fill(fillGuest,64);
        if (![barrier.report containsString:@"remplissages 1 "]) std::printf("FAIL fill not recognised:\n%s\n", barrier.report.UTF8String);
        assert([barrier.report containsString:@"Barrières compute : 5 ("] && [barrier.report containsString:@"remplissages 1 "]);
        std::uint32_t filled[2]; assert(computeArena.memory.read(fillGuest+63*4,filled,8));
        assert(filled[0] == fillValue && filled[1] == 0);
        fill(targetB,width*height);
        assert([barrier.report containsString:@"remplissages 2 "]);
        std::uint8_t cleared[width*height*4]{};
        id<MTLTexture> filledTarget = [barrier copyTargetAtAddress:targetB width:width height:height];
        while (barrier.displayEvent.signaledValue < barrier.displayEventValue) usleep(100);
        readPixels(device,queue,filledTarget,cleared);
        for (unsigned n = 0; n < width*height; ++n) { std::uint32_t pixel; std::memcpy(&pixel,cleared+4*n,4); assert(pixel == fillValue); }
        fill(fillGuest,64,fillMulCS);
        if (![barrier.report containsString:@"remplissages 3 "]) std::printf("FAIL multiplying fill not recognised:\n%s\n", barrier.report.UTF8String);
        assert([barrier.report containsString:@"remplissages 3 "]);
        std::puts("PASS fill dispatches reach guest memory and targets without a kernel");
        const uint32_t bad[]={0};
        assert([bridge submit:bad count:1]!=0 && bridge.failure);
        assert([bridge submit:s.words.data() count:s.words.size()]!=0);
        std::puts("PASS app Metal bridge: persistent submissions, GPU completion, display copies, failure latch");
        // Scene A: green where the second draw's scissor allowed, gradient elsewhere.
        const std::uint8_t* a = arena.bytes.data() + (targetA - Arena::start);
        assert(a[1] == 255 && a[2] == 0 && a[4 * 8 + 2] == 135 && a[4 * 8 + 1] == 0);
        std::puts("PASS PM4 stream -> Metal renderer (translated GCN programs) matches the software renderer: rect lists, scissor, persistence, indexed fetch-shader draw, texture sampling");
    }
}
