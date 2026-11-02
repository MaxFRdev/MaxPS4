// Stage 2 of the translator test: compiles the functions stage 1 generated
// (GCN_GENERATED, a quoted path) as C++ and checks every lane against the
// GCN interpreter on the same programs and guest memory.
#include "../Native/GCNShaderRuntime.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <thread>

namespace maxps4::gpu::gcnrt {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include GCN_GENERATED
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}
using namespace maxps4::gpu;
using namespace gcn_test;
namespace rt = maxps4::gpu::gcnrt;
namespace {
bool same(float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); }
rt::GCNMemory window(const Arena& arena) { return {arena.bytes.data(), Arena::start, arena.bytes.size()}; }
}
int main() {
    Arena arena;
    loadEmbeddedVS(arena); loadQuadPS(arena); loadFetchVS(arena); loadResinfoVS(arena);
    const rt::GCNMemory M = window(arena);
    rt::GCNQuad unusedQuad;
    // Vertex programs: one lane per vertex, against one interpreter wave.
    {
        GCNWave wave(arena.memory, 3);
        for (unsigned lane = 0; lane < 3; ++lane) wave.v(0, lane) = lane;
        assert(wave.run(embeddedVS));
        for (unsigned lane = 0; lane < 3; ++lane) {
            rt::GCNLane L;
            L.vinit[0] = lane;
            rt::vs_embedded(L, M, unusedQuad);
            for (unsigned c = 0; c < 4; ++c) {
                assert(same(L.pos[c], gcn_detail::f32(wave.exports[0].value[c][lane])));
                assert(same(L.param[0][c], gcn_detail::f32(wave.exports[1].value[c][lane])));
            }
        }
    }
    {
        GCNWave wave(arena.memory, 4);
        wave.s[0] = std::uint32_t(fetchShader); wave.s[2] = std::uint32_t(vertexTable);
        for (unsigned lane = 0; lane < 4; ++lane) wave.v(0, lane) = lane;
        assert(wave.run(fetchVS));
        for (unsigned lane = 0; lane < 4; ++lane) {
            rt::GCNLane L;
            L.user[0] = std::uint32_t(fetchShader); L.user[2] = std::uint32_t(vertexTable);
            L.vinit[0] = lane;
            rt::vs_fetch(L, M, unusedQuad);
            for (unsigned c = 0; c < 4; ++c) {
                assert(same(L.pos[c], gcn_detail::f32(wave.exports[0].value[c][lane])));
                assert(same(L.param[0][c], gcn_detail::f32(wave.exports[1].value[c][lane])));
            }
        }
        // Spot check against the known data, not only the interpreter.
        rt::GCNLane L; L.user[0] = std::uint32_t(fetchShader); L.user[2] = std::uint32_t(vertexTable); L.vinit[0] = 2;
        rt::vs_fetch(L, M, unusedQuad);
        assert(L.pos[0] == 5 && L.pos[1] == 6 && L.param[0][0] == 5 * 9 + 6);
        // A fetch shader below the VS (Mortal Kombat: Legacy Kollection): the
        // program still starts at its own first block.
        rt::GCNLane B; B.user[0] = std::uint32_t(fetchShaderBelow); B.user[2] = std::uint32_t(vertexTable); B.vinit[0] = 2;
        rt::vs_fetch_below(B, M, unusedQuad);
        assert(B.pos[0] == 5 && B.pos[1] == 6 && B.param[0][0] == 5 * 9 + 6);
        // The same program on copies of two ranges only (the V# table, then the
        // vertices) packed at other offsets, as the Metal renderer feeds the GPU.
        std::vector<unsigned char> packed(0x100 + 0x40, 0);
        std::memcpy(packed.data(), arena.bytes.data() + (vertexTable - Arena::start), 16);
        std::memcpy(packed.data() + 0x100, arena.bytes.data() + (0x10e00 - Arena::start), 48);
        const rt::ulong ranges[6] = {vertexTable, 16, 0, 0x10e00, 48, 0x100};
        rt::GCNMemory P{packed.data(), 0, 0, ranges, 2};
        rt::GCNLane R; R.user[0] = std::uint32_t(fetchShader); R.user[2] = std::uint32_t(vertexTable); R.vinit[0] = 2;
        rt::vs_fetch(R, P, unusedQuad);
        assert(R.pos[0] == 5 && R.pos[1] == 6 && R.param[0][0] == 5 * 9 + 6);
        // Outside every range reads zero.
        assert(rt::gcn_ld32(P, 0x10e00 + 48) == 0 && rt::gcn_ld32(P, Arena::start) == 0);
    }
    // image_get_resinfo per mip level (the lane), against the interpreter.
    {
        GCNWave wave(arena.memory, 4);
        for (unsigned n = 0; n < 8; ++n) wave.s[n] = resinfoSgprs[n];
        for (unsigned lane = 0; lane < 4; ++lane) wave.v(0, lane) = lane;
        assert(wave.run(resinfoVS));
        for (unsigned lane = 0; lane < 4; ++lane) {
            rt::GCNLane L;
            for (unsigned n = 0; n < 8; ++n) L.user[n] = resinfoSgprs[n];
            L.vinit[0] = lane;
            rt::vs_resinfo(L, M, unusedQuad);
            for (unsigned c = 0; c < 4; ++c) assert(gcn_detail::u32(L.pos[c]) == wave.exports[0].value[c][lane]);
            const std::uint32_t expected[4] = {std::max(1024u >> lane, 1u), std::max(512u >> lane, 1u), 1, 4};
            for (unsigned c = 0; c < 4; ++c) assert(wave.exports[0].value[c][lane] == expected[c]);
        }
    }
    // The pixel quad: four lanes on four threads in lockstep, lane 3 a helper.
    {
        GCNWave wave(arena.memory, 4);
        for (unsigned n = 0; n < 16; ++n) wave.s[n] = quadSgprs[n];
        wave.setExec(0x7);
        for (unsigned lane = 0; lane < 4; ++lane) { wave.v(0, lane) = bits(quadI[lane]); wave.v(1, lane) = 0; }
        wave.attribute = [](unsigned, unsigned chan) { return quadPlanes[chan & 1]; };
        assert(wave.run(quadPS));
        rt::GCNQuad quad;
        rt::GCNLane lanes[4];
        std::thread threads[4];
        for (unsigned lane = 0; lane < 4; ++lane) {
            rt::GCNLane& L = lanes[lane];
            for (unsigned n = 0; n < 16; ++n) L.user[n] = quadSgprs[n];
            L.vinit[0] = bits(quadI[lane]); L.vinit[1] = 0;
            L.exec = lane != 3; L.quadLane = lane;
            for (unsigned c = 0; c < 2; ++c) L.plane[0][c] = {quadPlanes[c].p0, quadPlanes[c].p10, quadPlanes[c].p20};
            threads[lane] = std::thread([&, lane] { rt::ps_quad(lanes[lane], M, quad); });
        }
        for (auto& t : threads) t.join();
        const GCNExport& ddx = wave.exports[0];
        const GCNExport& color = wave.exports[1];
        for (unsigned lane = 0; lane < 4; ++lane) {
            const rt::GCNLane& L = lanes[lane];
            const bool live = (color.mask >> lane) & 1;
            assert(L.killed == !live && ((L.colorWritten & 1) != 0) == live && ((L.colorWritten & 2) != 0) == live);
            if (!live) continue;
            assert(same(L.color[1][0], gcn_detail::f32(ddx.value[0][lane])));
            const float expected[4] = {gcn_detail::f16(color.value[0][lane] & 0xffff), gcn_detail::f16(color.value[0][lane] >> 16),
                                       gcn_detail::f16(color.value[1][lane] & 0xffff), gcn_detail::f16(color.value[1][lane] >> 16)};
            for (unsigned c = 0; c < 4; ++c) assert(same(L.color[0][c], expected[c]));
        }
        assert(lanes[0].color[0][0] == 1.0f && lanes[0].color[0][1] == 0.5f && lanes[1].color[0][1] == 1.0f);
    }
    // Compute: two groups of 32 threads; the stores leave identical memory.
    {
        Arena interpreted, translated;
        loadComputeCS(interpreted); loadComputeCS(translated);
        const rt::GCNMemory T = window(translated);
        for (unsigned group = 0; group < 2; ++group) {
            GCNWave wave(interpreted.memory, 32);
            for (unsigned k = 0; k < 16; ++k) wave.s[k] = computeSgprs[k];
            wave.s[16] = group;
            for (unsigned lane = 0; lane < 32; ++lane) wave.v(0, lane) = lane;
            assert(wave.run(computeCS));
            for (unsigned lane = 0; lane < 32; ++lane) {
                rt::GCNLane L;
                for (unsigned k = 0; k < 16; ++k) L.user[k] = computeSgprs[k];
                L.user[16] = group; L.vinit[0] = lane;
                rt::cs_copy(L, T, unusedQuad);
            }
        }
        assert(interpreted.bytes == translated.bytes);
        // Record 33 (group 1, thread 1): z = (237 % 13) / 8 - 0.25 = 0.125, so 32 of 255.
        assert(translated.bytes[computeBuffer + 4 * 33 + 2 - Arena::start] == 32);
        // Pixel (1, 1) of the image holds the same record; x = 8 and beyond are outside it.
        assert(translated.bytes[computeImage + 4 * (8 + 1) + 2 - Arena::start] == 32);
        assert(translated.bytes[computeImage + 4 * 64 - Arena::start] == 0);
    }
    std::puts("PASS GCN translator stage 2: translated C++ matches the interpreter lane for lane (VS, fetch VS, pixel quad, compute stores)");
}
