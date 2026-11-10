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
    loadEmbeddedVS(arena); loadQuadPS(arena); loadFetchVS(arena); loadResinfoVS(arena); loadDepthCompareVS(arena);
    const rt::GCNMemory M = window(arena);
    rt::GCNQuad unusedQuad;
    {
        rt::GCNLane L;
        L.vinit[0]=bits(0.95f);
        rt::ps_depth_export(L,M,unusedQuad);
        assert(L.depth==0.95f && L.colorWritten==0 && !L.killed);
        rt::GCNLane inactive; inactive.exec=false; inactive.depth=0.4f;
        inactive.vinit[0]=bits(0.95f);
        rt::ps_depth_export(inactive,M,unusedQuad);
        assert(inactive.depth==0.4f && inactive.killed);
    }
    {
        // Exact device operation: green component, signed x offset -1.
        arena.code(0x11900,{0xf15c0200,0x0040040c,0xf800180f,0x07060504,0xbf810000});
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            arena.put(0x15000+4*(4*y+x),0xff000000u | ((4*y+x)*16u<<8));
        const uint32_t descriptor[8]={0x150,10u<<20,3u|(3u<<14),0x90800fac,3u<<13,0,0,0};
        GCNWave wave(arena.memory,1);
        rt::GCNLane L;
        for (unsigned n=0;n<8;++n) L.user[n]=wave.s[n]=descriptor[n];
        L.user[8]=wave.s[8]=0x12; // clamp-to-edge
        L.vinit[12]=wave.v(12,0)=63; // -1 in signed six-bit encoding
        L.vinit[13]=wave.v(13,0)=bits(0.375f);
        L.vinit[14]=wave.v(14,0)=bits(0.375f);
        assert(wave.run(0x11900));
        rt::ps_gather_lz(L,M,unusedQuad);
        const unsigned expected[4]={128,144,80,64};
        for (unsigned n=0;n<4;++n) {
            const float value=float(expected[n])/255;
            assert(std::abs(L.color[0][n]-value)<0.00001f);
            assert(std::abs(gcn_detail::f32(wave.exports[0].value[n][0])-value)<0.00001f);
        }
        rt::GCNRes8 t{}; for (unsigned n=0;n<8;++n) t.d[n]=descriptor[n];
        rt::GCNRes4 sampler{{0x12,0,0x500000,0}}; // filtering must not blend gather taps
        auto got=rt::gcn_gather_lz(M,t,sampler,false,0.375f,0.375f,63,1);
        for (unsigned n=0;n<4;++n) assert(std::abs(got[n]-float(expected[n])/255)<0.00001f);
        sampler.d[0]=0x8000|0x12;
        got=rt::gcn_gather_lz(M,t,sampler,false,1.5f,1.5f,63,1);
        for (unsigned n=0;n<4;++n) assert(std::abs(got[n]-float(expected[n])/255)<0.00001f);
        sampler.d[0]=4u|(4u<<3); sampler.d[3]=2u<<30;
        got=rt::gcn_gather_lz(M,t,sampler,true,-20,-20,0,1);
        for (unsigned n=0;n<4;++n) assert(got[n]==1); // white border
    }
    // Analytic 2D footprint: eight texels per pixel gives log2(8) = 3.
    // Exercise the generated shader, all four lanes and the real quad exchange.
    {
        rt::GCNQuad Q;
        arena.code(0x11800,{0xf1800300,0x00400a00,0xf800180f,0x0d0c0b0a,0xbf810000});
        GCNWave lodWave(arena.memory,4);
        lodWave.s[2]=255u | (127u<<14); lodWave.s[3]=0x90070000; lodWave.s[9]=0xfff000;
        for (unsigned lane=0;lane<4;++lane) {
            lodWave.v(0,lane)=bits(float(lane&1u)/32.0f);
            lodWave.v(1,lane)=bits(float(lane>>1)/16.0f);
        }
        assert(lodWave.run(0x11800));
        for (unsigned lane=0;lane<4;++lane) {
            assert(gcn_detail::f32(lodWave.exports[0].value[0][lane])==3.0f);
            assert(gcn_detail::f32(lodWave.exports[0].value[1][lane])==3.0f);
        }
        GCNWave scanWave(arena.memory,1);
        scanWave.s=lodWave.s;
        assert(scanWave.run(0x11800)); // resource scans continue beyond GET_LOD
        rt::GCNLane lanes[4];
        std::thread workers[4];
        for (unsigned lane=0; lane<4; ++lane) {
            auto& L=lanes[lane]; L.quadLane=lane;
            L.user[2]=255u | (127u << 14);
            L.user[3]=0x90070000; // 2D, levels 0..7
            L.user[9]=0xfff000; // max LOD 15.996
            L.vinit[0]=bits(float(lane & 1u)/32.0f);
            L.vinit[1]=bits(float(lane >> 1)/16.0f);
            workers[lane]=std::thread([&,lane] { rt::ps_lod(lanes[lane], M, Q); });
        }
        for (auto& worker:workers) worker.join();
        for (const auto& L:lanes) assert(L.color[0][0]==3.0f && L.color[0][1]==3.0f);
        rt::GCNRes8 t{{0,0,255u | (127u << 14),0x90070000,0,0,0,0}};
        rt::GCNRes4 s{{0,0xfff000,0,0}};
        auto value=rt::gcn_image_lod_2d(t,s,false,1.0f/32,0,0,1.0f/16);
        assert(value[0]==3.0f && value[1]==3.0f);
        s.d[1]=512u | (1024u<<12); // min 2, max 4
        value=rt::gcn_image_lod_2d(t,s,true,32,0,0,32);
        assert(value[0]==4.0f && value[1]==5.0f);
        s.d[2]=0x3f00; // signed bias -1
        value=rt::gcn_image_lod_2d(t,s,true,8,0,0,8);
        assert(value[0]==2.0f && value[1]==2.0f);
        s.d[1]=0xfff000; s.d[2]=0;
        value=rt::gcn_image_lod_2d(t,s,false,0,0,0,0);
        assert(value[0]==0 && std::isinf(value[1]) && value[1]<0);
    }
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
    // image_sample_c (LESS against depth 0.75), reference 0.5 * lane: 1, 1, 0, 0.
    {
        GCNWave wave(arena.memory, 4);
        for (unsigned n = 0; n < 12; ++n) wave.s[n] = depthCompareSgprs[n];
        for (unsigned lane = 0; lane < 4; ++lane) wave.v(0, lane) = lane;
        assert(wave.run(depthCompareVS));
        for (unsigned lane = 0; lane < 4; ++lane) {
            rt::GCNLane L;
            for (unsigned n = 0; n < 12; ++n) L.user[n] = depthCompareSgprs[n];
            L.vinit[0] = lane;
            rt::vs_depth_compare(L, M, unusedQuad);
            const float expected = lane < 2 ? 1.0f : 0.0f;
            for (unsigned c = 0; c < 4; ++c) {
                assert(gcn_detail::u32(L.pos[c]) == wave.exports[0].value[c][lane]);
                assert(wave.exports[0].value[c][lane] == bits(expected));
            }
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
    std::puts("PASS GCN translator stage 2: translated C++ matches the interpreter lane for lane (VS, fetch VS, depth compare, pixel quad, compute stores)");
}
