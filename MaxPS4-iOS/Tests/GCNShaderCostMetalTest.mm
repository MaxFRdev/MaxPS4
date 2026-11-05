// Research only: where a translated pixel program's GPU time goes. One
// full-screen 1920x1080 draw (PM4MetalRendererTest's textured program,
// u = x / 1920, v = y / 1080 over a 512x256 RGBA8 texture) rendered with
// variants of the generated MSL, each removing one part, next to an empty
// pass and trivial Metal fragments on the same device. Variants change
// pixels: only the timings matter. Never selects a game backend.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "../Native/PM4ResourceScan.hpp"
#include "GCNTestPrograms.h"
#include "GCNGamePrograms.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <iterator>
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
int main(int argc, char** argv) {
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
        // 32 transcendental instructions per pixel: sin, exp, log and sqrt in turn.
        constexpr uint64_t mathPS = 0x32000;
        for (unsigned n = 0; n < 32; ++n) {
            static const uint32_t ops[4] = {0x7e006b00, 0x7e004b00, 0x7e004f00, 0x7e006700};
            arena.put(mathPS + 4 * n, ops[n % 4]);
        }
        arena.code(mathPS + 4 * 32, {0xf800180f, 0x00000000, 0xbf810000});
        const auto math = translateGCN(arena.memory, mathPS, psOptions);
        assert(math);
        const auto scalars = translateGCN(arena.memory, scalarPS, psOptions);
        assert(scalars);
        // Mortal Kombat 1's costliest programs with their descriptor tables as the
        // words read them; constants all 0.5 (the game's own are not in the report).
        // PS 1800 samples four images: its fourth table entry can match the first
        // (all bound) or stand alone (the renderer binds three).
        constexpr uint64_t ps1800At = 0x34000, ps2000At = 0x35000, table1800 = 0x36000, loneTable1800 = 0x36800, table2000 = 0x37000;
        for (std::size_t n = 0; n < std::size(mk1PS1800); ++n) arena.put(ps1800At + 4 * n, mk1PS1800[n]);
        for (std::size_t n = 0; n < std::size(mk1PS2000); ++n) arena.put(ps2000At + 4 * n, mk1PS2000[n]);
        const uint32_t gameSampler[4] = {0, 0, 0x500000, 0};
        auto gameImage = [&](uint32_t mark, uint32_t* out) {
            const uint32_t words[8] = {std::uint32_t(image >> 8), 10u << 20, (imageW - 1) | ((imageH - 1) << 14), 0x90800facu, (imageW - 1) << 13, mark, 0, 0};
            std::memcpy(out, words, sizeof words);
        };
        auto putWords = [&](uint64_t at, const uint32_t* words, unsigned count) { for (unsigned n = 0; n < count; ++n) arena.put(at + 4 * n, words[n]); };
        auto putImage = [&](uint64_t at, uint32_t mark) { uint32_t words[8]; gameImage(mark, words); putWords(at, words, 8); };
        auto putBuffer = [&](uint64_t at, uint64_t constants) {
            const uint32_t words[4] = {std::uint32_t(constants), 0, 0x400, 0x9000};
            putWords(at, words, 4);
        };
        for (uint64_t table : {table1800, loneTable1800, table2000}) {
            const uint64_t constants = (table == loneTable1800 ? table1800 : table) + 0x100;
            for (unsigned n = 0; n < 0x100; ++n) arena.put(constants + 4 * n, bits(0.5f));
            putImage(table, 1); putImage(table + 0x20, 2);
            if (table == table2000) {
                putWords(table + 0x40, gameSampler, 4); putWords(table + 0x50, gameSampler, 4); putBuffer(table + 0x60, constants);
            } else {
                putImage(table + 0x40, table == loneTable1800 ? 4 : 1);
                putWords(table + 0x60, gameSampler, 4); putWords(table + 0x70, gameSampler, 4);
                for (unsigned n = 0; n < 2; ++n) putBuffer(table + 0x80 + 0x10 * n, constants);
                // Its first constant selects the path: 2.0 reads all four images, as the scan saw.
                putBuffer(table + 0xa0, constants + 0x200);
                arena.put(constants + 0x200, bits(2.0f));
            }
        }
        GCNTranslateOptions gameOptions = psOptions; gameOptions.userCount = 15;
        const auto game1800 = translateGCN(arena.memory, ps1800At, gameOptions);
        const auto game2000 = translateGCN(arena.memory, ps2000At, gameOptions);
        if (!game1800 || !game2000) std::printf("game programs: %s %s\n", game1800.error.c_str(), game2000.error.c_str());
        assert(game1800 && game2000);
        GCNTranslateOptions loopOptions = gameOptions; loopOptions.dispatcher = true;
        const auto loop1800 = translateGCN(arena.memory, ps1800At, loopOptions);
        const auto loop2000 = translateGCN(arena.memory, ps2000At, loopOptions);
        assert(loop1800 && loop2000);
        std::printf("SHADER FORM PS 1800 %s, PS 2000 %s\n", game1800.source.find("while (running)") == std::string::npos ? "in order" : "loop",
                    game2000.source.find("while (running)") == std::string::npos ? "in order" : "loop");
        std::printf("SHADER SOURCE PS 1800 %zu bytes, PS 2000 %zu bytes\n", game1800.source.size(), game2000.source.size());
        std::uint32_t inputCntl[32] = {};
        const std::string entry = gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", 0x302, inputCntl);
        const std::string scalarEntry = gcnMetalEntryPoints(vs, scalars, "vs_program", "ps_program", 0x302, inputCntl);
        const std::string mathEntry = gcnMetalEntryPoints(vs, math, "vs_program", "ps_program", 0x302, inputCntl);
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
        const std::string fastMath = replaced(prelude, "return precise::", "return fast::");
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
        // Texture coordinates from the pixel, as the game's full-screen quads give them.
        auto gameEntry = [&](const auto& program) {
            return replaced(gcnMetalEntryPoints(vs, program, "vs_program", "ps_program", 0x2, inputCntl, true),
                            "L.attr[0] = in.p0;", "L.attr[0] = float4(in.position.x / 1920.0f, in.position.y / 1080.0f, 0.0f, 1.0f);");
        };
        const std::string entry1800 = gameEntry(game1800), entry2000 = gameEntry(game2000);
        // No software path: a T# or S# no bound texture matches reads as zero.
        const std::string noFallback = replaced(nativePrelude, "if (gcn_native_sample(M, t, s, unorm, u, v, filtered)) return gcn_texture_swizzle(t, filtered);",
                                                "if (gcn_native_sample(M, t, s, unorm, u, v, filtered)) return gcn_texture_swizzle(t, filtered);\n    if (true) return float4(0.0f, 0.0f, 0.0f, 0.0f);");
        // No image read at all: what is left is the program's arithmetic and scalar loads.
        const std::string noImages = replaced(nativePrelude, "if (gcn_native_sample(M, t, s, unorm, u, v, filtered)) return gcn_texture_swizzle(t, filtered);",
                                              "if (true) return float4(u, v, 0.5f, 1.0f);");
        // Scalar loads read from a small constant table, no search and no pointer
        // choice: the best a table of values from the CPU scan could do.
        auto uniformLoads = [&](std::string source, uint64_t table) {
            std::string data = "GCN_FN bool gcn_uniform_span(ulong n, thread ulong& at, thread uint& hint) { return true; }\nconstant uint gcn_uniform[320] = {";
            for (unsigned n = 0; n < 320; ++n) {
                uint32_t word; std::memcpy(&word, arena.bytes.data() + (table - Arena::start) + 4 * n, 4);
                data += std::to_string(word) + "u,";
            }
            data += "};\n";
            const std::string first = "((a - " + std::to_string(table) + "ul) >> 2)";
            source = replaced(source, "gcn_ld32_hint(M, a, gcn_hint)", "gcn_uniform[" + first + "]");
            source = replaced(source, "gcn_locate_hint(M, a, ", "gcn_uniform_span(");
            source = replaced(source, "gcn_packed32(M, at + 4ul * k)", "gcn_uniform[" + first + " + k]");
            return data + source;
        };
        const std::string uniform1800 = uniformLoads(game1800.source, table1800), uniform2000 = uniformLoads(game2000.source, table2000);
        std::vector<std::uint32_t> sites1800, sites2000; // Filled by the scan, below.
        auto noSites = [&](const std::string& source) { return replaced(source, "gcn_site_hit(M, ", "false && gcn_site_hit(M, "); };
        // The table's cost, part by part: its address check kept without the search
        // behind it, then the check gone too (each load takes the table's words).
        auto noSearch = [&](std::string source) {
            source = replaced(source, "gcn_ld32_hint(M, a, gcn_hint)", "0u");
            source = replaced(source, "gcn_locate_hint(M, a, ", "gcn_no_span(");
            source = replaced(source, "gcn_packed32(M, at + 4ul * k)", "0u");
            return "GCN_FN bool gcn_no_span(ulong n, thread ulong& at, thread uint& hint) { return true; }\n" + source;
        };
        auto noCheck = [&](const std::string& source) {
            return "GCN_FN bool gcn_site_always(GCNMemory M, uint at, ulong a) { return true; }\n" + replaced(source, "gcn_site_hit(M, ", "gcn_site_always(M, ");
        };
        struct Variant { const char* name; std::string source; const char* fragment; bool draws; std::string prelude = {}; int native = -1; unsigned state = 0; const std::vector<std::uint32_t>* sites = nullptr; };
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
            {"PS 1800: fourth image in software", vs.source + game1800.source + entry1800, "gcn_fragment", true, nativePrelude, 3, 4},
            {"PS 1800: four images bound", vs.source + game1800.source + entry1800, "gcn_fragment", true, nativePrelude, 2, 4},
            {"PS 1800: no table code, three bound", vs.source + noSites(game1800.source) + entry1800, "gcn_fragment", true, nativePrelude, 3, 4},
            {"PS 1800: no table code, four bound", vs.source + noSites(game1800.source) + entry1800, "gcn_fragment", true, nativePrelude, 2, 4},
            {"PS 1800: scalar table, three bound", vs.source + game1800.source + entry1800, "gcn_fragment", true, nativePrelude, 3, 4, &sites1800},
            {"PS 1800: scalar table, four bound", vs.source + game1800.source + entry1800, "gcn_fragment", true, nativePrelude, 2, 4, &sites1800},
            {"PS 1800: table, no software path", vs.source + game1800.source + entry1800, "gcn_fragment", true, noFallback, 2, 4, &sites1800},
            {"PS 1800: no software path", vs.source + game1800.source + entry1800, "gcn_fragment", true, noFallback, 2, 5},
            {"PS 1800: no image reads", vs.source + game1800.source + entry1800, "gcn_fragment", true, noImages, 2, 5},
            {"PS 1800: constant scalar loads", vs.source + uniform1800 + entry1800, "gcn_fragment", true, nativePrelude, 2, 5},
            {"PS 1800: constant, no software path", vs.source + uniform1800 + entry1800, "gcn_fragment", true, noFallback, 2, 5},
            {"PS 2000: as bound", vs.source + game2000.source + entry2000, "gcn_fragment", true, nativePrelude, 2, 6},
            {"PS 2000: no software path", vs.source + game2000.source + entry2000, "gcn_fragment", true, noFallback, 2, 6},
            {"PS 2000: no image reads", vs.source + game2000.source + entry2000, "gcn_fragment", true, noImages, 2, 6},
            {"PS 2000: constant scalar loads", vs.source + uniform2000 + entry2000, "gcn_fragment", true, nativePrelude, 2, 6},
            {"PS 2000: constant, no software path", vs.source + uniform2000 + entry2000, "gcn_fragment", true, noFallback, 2, 6},
            {"PS 2000: scalar table", vs.source + game2000.source + entry2000, "gcn_fragment", true, nativePrelude, 2, 6, &sites2000},
            {"PS 1800: loop, table, no software path", vs.source + loop1800.source + entry1800, "gcn_fragment", true, noFallback, 2, 4, &sites1800},
            {"PS 1800: loop, no software path", vs.source + loop1800.source + entry1800, "gcn_fragment", true, noFallback, 2, 5},
            {"PS 1800: loop, constant, no software path", vs.source + uniformLoads(loop1800.source, table1800) + entry1800, "gcn_fragment", true, noFallback, 2, 5},
            {"PS 2000: loop, as bound", vs.source + loop2000.source + entry2000, "gcn_fragment", true, nativePrelude, 2, 6},
            {"PS 2000: loop, scalar table", vs.source + loop2000.source + entry2000, "gcn_fragment", true, nativePrelude, 2, 6, &sites2000},
            {"PS 1800: loop, scalar table, four bound", vs.source + loop1800.source + entry1800, "gcn_fragment", true, nativePrelude, 2, 4, &sites1800},
            {"PS 1800: table, no search", vs.source + noSearch(game1800.source) + entry1800, "gcn_fragment", true, nativePrelude, 2, 4, &sites1800},
            {"PS 1800: table, no check", vs.source + noCheck(game1800.source) + entry1800, "gcn_fragment", true, nativePrelude, 2, 4, &sites1800},
            {"PS 1800: table, no search, no software", vs.source + noSearch(game1800.source) + entry1800, "gcn_fragment", true, noFallback, 2, 4, &sites1800},
            {"PS 1800: table, no check, no software", vs.source + noCheck(game1800.source) + entry1800, "gcn_fragment", true, noFallback, 2, 4, &sites1800},
            {"PS 1800: loop, no check, no software", vs.source + noCheck(loop1800.source) + entry1800, "gcn_fragment", true, noFallback, 2, 4, &sites1800},
            {"PS 2000: table, no check", vs.source + noCheck(game2000.source) + entry2000, "gcn_fragment", true, nativePrelude, 2, 6, &sites2000},
            {"PS 2000: table, no check, no software", vs.source + noCheck(game2000.source) + entry2000, "gcn_fragment", true, noFallback, 2, 6, &sites2000},
            {"PS 2000: loop, no check", vs.source + noCheck(loop2000.source) + entry2000, "gcn_fragment", true, nativePrelude, 2, 6, &sites2000},
            {"PS 2000: loop, no check, no software", vs.source + noCheck(loop2000.source) + entry2000, "gcn_fragment", true, noFallback, 2, 6, &sites2000},
            {"math: 32 instructions, precise", vs.source + math.source + mathEntry, "gcn_fragment", true},
            {"math: 32 instructions, fast", vs.source + math.source + mathEntry, "gcn_fragment", true, fastMath},
            {"scalar: 32 loads, variable slot index", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, slotArray, -1, 2},
            {"scalar: 32 loads, no range table", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, {}, -1, 1},
            {"scalar: 32 loads, 2 ranges", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, {}, -1, 2},
            {"scalar: 32 loads, 16 ranges", vs.source + scalars.source + scalarEntry, "gcn_fragment", true, {}, -1, 3},
        };
        // A GPU timeout makes Metal ignore the whole process afterwards: each game
        // program variant runs in a process of its own, this one given by name.
        if (argc > 1) {
            variants.erase(std::remove_if(variants.begin(), variants.end(), [&](const Variant& v) { return std::string(v.name) != argv[1]; }), variants.end());
        } else {
            for (const auto& v : variants) {
                if (std::strncmp(v.name, "PS ", 3)) continue;
                const std::string command = std::string("\"") + argv[0] + "\" \"" + v.name + "\"";
                const int status = std::system(command.c_str());
                if (status) std::printf("SHADER CHILD %s: status %d\n", v.name, status);
            }
            variants.erase(std::remove_if(variants.begin(), variants.end(), [](const Variant& v) { return !std::strncmp(v.name, "PS ", 3); }), variants.end());
        }
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
            if (!library) { variants[k].draws = false; continue; }
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
        // single window; two ranges; sixteen ranges, the one it reads the twelfth.
        State states[7] = {state, state, state, state, state, state, state};
        // The game programs' user SGPRs: T#, S#, table pointer, attribute base.
        for (unsigned k = 4; k < 7; ++k) {
            uint32_t words[8]; gameImage(3, words);
            std::memcpy(states[k].ps, words, sizeof words); std::memcpy(states[k].ps + 8, gameSampler, sizeof gameSampler);
            states[k].ps[12] = std::uint32_t(k == 4 ? loneTable1800 : k == 5 ? table1800 : table2000); states[k].ps[13] = 0; states[k].ps[14] = 0;
        }
        for (unsigned k = 1; k < 4; ++k) { states[k].ps[0] = std::uint32_t(scalarData); states[k].ps[1] = 0; }
        states[1].count = 0; states[1].base = Arena::start; states[1].size = arena.bytes.size();
        states[3].count = 16;
        for (unsigned k = 0; k < 16; ++k) { // Sorted, as the renderer gives them.
            states[3].ranges[3 * k] = k < 11 ? 0x100ull * k : 0x7000000000ull + (std::uint64_t(k) << 20);
            states[3].ranges[3 * k + 1] = 0x10; states[3].ranges[3 * k + 2] = 0;
        }
        states[3].ranges[33] = Arena::start; states[3].ranges[34] = image - Arena::start; states[3].ranges[35] = 0;
        // The tables the renderer gives these draws, from the same scan.
        auto scanSites = [&](uint64_t program, const GCNTranslation& translation, const State& gameState, std::vector<std::uint32_t>& table) {
            PM4ResourceScanner scanner(arena.memory, true);
            PM4Draw draw{};
            draw.ps = program; draw.psUserCount = 15; draw.psInputAddr = 2;
            for (unsigned k = 0; k < 15; ++k) draw.psUser[k] = gameState.ps[k];
            draw.scissor[2] = screenW; draw.scissor[3] = screenH;
            scanner.scan(draw);
            auto sites = translation.sites;
            std::sort(sites.begin(), sites.end(), [](const GCNScalarSite& a, const GCNScalarSite& b) { return a.pc < b.pc; });
            const bool any = scanner.scalarTable(sites, translation.siteWords, table);
            std::printf("SHADER SITES 0x%llx: %zu sites, %u words, %zu loads scanned, %zu images, error '%s'\n", (unsigned long long)program, sites.size(),
                        translation.siteWords, scanner.scalarLoads().size(), scanner.images().size(), scanner.pixelError().c_str());
            assert(any);
        };
        scanSites(ps1800At, game1800, states[4], sites1800);
        scanSites(ps2000At, game2000, states[6], sites2000);
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
        MTLArgumentDescriptor* fields[21];
        const MTLDataType types[21] = {MTLDataTypeTexture, MTLDataTypeUInt, MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeTexture,
                                       MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeTexture, MTLDataTypeUInt4, MTLDataTypeUInt4,
                                       MTLDataTypeSampler, MTLDataTypeSampler, MTLDataTypeSampler, MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeUInt4,
                                       MTLDataTypeTexture, MTLDataTypeUInt4, MTLDataTypeUInt4, MTLDataTypeSampler, MTLDataTypeUInt4};
        for (unsigned k = 0; k < 21; ++k) {
            fields[k] = [MTLArgumentDescriptor argumentDescriptor];
            fields[k].dataType = types[k]; fields[k].index = k;
            if (types[k] == MTLDataTypeTexture) fields[k].textureType = MTLTextureType2D;
        }
        id<MTLArgumentEncoder> nativeArguments = [device newArgumentEncoderWithArguments:[NSArray arrayWithObjects:fields count:21]];
        // The S# (t[8..11]): wrap, bilinear, as PM4MetalRenderer builds its sampler.
        MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = samplerDescriptor.tAddressMode = MTLSamplerAddressModeRepeat;
        samplerDescriptor.supportArgumentBuffers = YES;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
        id<MTLBuffer> nativeTables[4];
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
        // Four bound images, as the renderer binds a game draw's (T#s marked 1 to 4),
        // or the first three only.
        for (unsigned table = 2; table < 4; ++table) {
            nativeTables[table] = [device newBufferWithLength:nativeArguments.encodedLength options:MTLResourceStorageModeShared];
            [nativeArguments setArgumentBuffer:nativeTables[table] offset:0];
            *static_cast<uint32_t*>([nativeArguments constantDataAtIndex:1]) = table == 2 ? 0xffu : 0x77u;
            for (unsigned k = 0; k < 4; ++k) {
                static const NSUInteger imageField[4] = {0, 4, 7, 16}, lowField[4] = {2, 5, 8, 17}, samplerField[4] = {10, 11, 12, 19}, wordField[4] = {13, 14, 15, 20};
                uint32_t words[8]; gameImage(k + 1, words);
                [nativeArguments setTexture:linearImage atIndex:imageField[k]];
                [nativeArguments setSamplerState:sampler atIndex:samplerField[k]];
                std::memcpy([nativeArguments constantDataAtIndex:wordField[k]], gameSampler, 16);
                std::memcpy([nativeArguments constantDataAtIndex:lowField[k]], words, 16);
                std::memcpy([nativeArguments constantDataAtIndex:lowField[k] + 1], words + 4, 16);
            }
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
                State drawn = states[variants[k].state];
                // As the renderer does, bound for whichever program declares it.
                static const std::vector<std::uint32_t> emptySites(1024, 0);
                if (variants[k].sites) drawn.flags |= 4; // GCN_DRAW_SITES
                const auto& siteWords = variants[k].sites ? *variants[k].sites : emptySites;
                [encoder setFragmentBytes:siteWords.data() length:siteWords.size() * 4 atIndex:4];
                [encoder setVertexBytes:&drawn length:sizeof state atIndex:1]; [encoder setFragmentBytes:&drawn length:sizeof state atIndex:1];
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
            if (command.status != MTLCommandBufferStatusCompleted || command.error) {
                std::printf("SHADER FAILED %s: %s\n", variants[k].name, command.error.localizedDescription.UTF8String);
                variants[k].draws = false;
                continue;
            }
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
