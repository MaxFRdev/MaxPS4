// Stage 1 of the translator test: translates the shared test programs and
// writes the generated functions to argv[1] for the C++ harness (stage 2,
// GCNTranslatorHarness.cpp), which runs them against the interpreter.
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace maxps4::gpu;
using namespace gcn_test;
int main(int argc, char** argv) {
    assert(argc == 2);
    Arena arena;
    loadEmbeddedVS(arena); loadQuadPS(arena); loadFetchVS(arena); loadResinfoVS(arena);
    GCNTranslateOptions vs; vs.stage = GCNStage::vertex; vs.name = "vs_embedded";
    const auto embedded = translateGCN(arena.memory, embeddedVS, vs);
    assert(embedded && embedded.position && embedded.params == 1 && !embedded.quad);
    GCNTranslateOptions ps; ps.name = "ps_quad";
    const auto quad = translateGCN(arena.memory, quadPS, ps);
    if (!quad) std::printf("%s\n", quad.error.c_str());
    assert(quad && quad.colors == 3 && quad.attributes == 1 && quad.flat == 0 && quad.quad);
    assert(!quad.mayKill); // save EXEC, enable WQM, restore the original coverage
    {
        // v_movrels/v_movreld/v_movrelsd: M0 selects among the VGPRs after the base.
        constexpr uint64_t relative = 0x11900;
        arena.code(relative, {0xbefc0382,                    // s_mov_b32 m0, 2
                              0x7e028704,                    // v_movrels_b32 v1, v4   (v1 = v6)
                              0x7e048502,                    // v_movreld_b32 v2, v2   (v4 = v2)
                              0x7e0a8908,                    // v_movrelsd_b32 v5, v8  (v7 = v10)
                              0xbf810000});
        GCNTranslateOptions rel; rel.stage = GCNStage::vertex; rel.name = "vs_relative";
        const auto moved = translateGCN(arena.memory, relative, rel);
        if (!moved) std::printf("%s\n", moved.error.c_str());
        assert(moved && moved.source.find("case 2u: r = v6;") != std::string::npos &&
               moved.source.find("case 2u: v4 = r;") != std::string::npos &&
               moved.source.find("case 2u: r = v10;") != std::string::npos && moved.source.find("case 2u: v7 = r;") != std::string::npos);
    }
    {
        // ds_swizzle_b32 v1, v0 offset:0x041f (bitmask: and 0x1f, or 0, xor 1).
        constexpr uint64_t swizzle = 0x11a00;
        arena.code(swizzle, {0xd8d4041f, 0x01000000, 0xbf810000});
        const auto swapped = translateGCN(arena.memory, swizzle, ps);
        if (!swapped) std::printf("%s\n", swapped.error.c_str());
        assert(swapped && swapped.quad && swapped.source.find("(((L.quadLane & 0x1fu) | 0x0u) ^ 0x1u) & 3u") != std::string::npos);
    }
    constexpr uint64_t coverageProgram=0x11800;
    arena.code(coverageProgram,{0xbeeb03ff,7,0xbe94047e,0xbefe0a7e,0xbefe0414,0xf800180f,0,0xbf810000});
    const auto prologue=translateGCN(arena.memory,coverageProgram,ps);
    assert(prologue && !prologue.mayKill);
    arena.code(coverageProgram,{0xbe94047e,0xbe940480,0xbefe0414,0xf800180f,0,0xbf810000});
    const auto overwritten=translateGCN(arena.memory,coverageProgram,ps);
    assert(overwritten && overwritten.mayKill); // the saved mask was overwritten
    arena.code(coverageProgram,{0xbefe0480,0xf800180f,0,0xbf810000});
    const auto killed=translateGCN(arena.memory,coverageProgram,ps);
    assert(killed && killed.mayKill);
    // Forward branches only: blocks in order, without the dispatcher's loop.
    arena.code(coverageProgram,{0xbf840000,0xbf810000});
    const auto ordered=translateGCN(arena.memory,coverageProgram,ps);
    assert(ordered && ordered.source.find("while (running)")==std::string::npos && ordered.source.find("if (pc == 1u) do {")!=std::string::npos);
    GCNTranslateOptions dispatched=ps; dispatched.dispatcher=true;
    assert(translateGCN(arena.memory,coverageProgram,dispatched).source.find("while (running)")!=std::string::npos);
    arena.code(coverageProgram,{0xbf84ffff,0xbf810000}); // A branch back to itself.
    assert(translateGCN(arena.memory,coverageProgram,ps).source.find("while (running)")!=std::string::npos);
    // Scalar loads whose address only comes from user SGPRs and other such loads
    // take the table's words; one through a register changed otherwise keeps the check.
    GCNTranslateOptions direct=ps; direct.directLoads=true;
    arena.code(coverageProgram,{0xc0880d00,0xc20a1100,0xbf810000});
    const auto chained=translateGCN(arena.memory,coverageProgram,direct);
    assert(chained && chained.source.find("gcn_site_hit")==std::string::npos && chained.sites.size()==2);
    assert(chained.sites[0].from[0]==13 && chained.sites[0].from[1]==14 && chained.sites[1].from[0]==0x1000 && chained.sites[1].from[3]==0x1003);
    arena.code(coverageProgram,{0xc0880d00,0xbe900380,0xc20a1100,0xbf810000});
    const auto changed=translateGCN(arena.memory,coverageProgram,direct);
    assert(changed && changed.source.find("gcn_site_hit")!=std::string::npos && changed.sites[1].from[0]==0);
    // A register written on one path only is not proved after the paths join.
    arena.code(coverageProgram,{0xc0880d00,0xbf840001,0xc0880d04,0xc20a1100,0xbf810000});
    const auto joined=translateGCN(arena.memory,coverageProgram,direct);
    assert(joined && joined.sites.size()==3 && joined.sites[1].from[0]==13 && joined.sites[2].from[0]==0);
    // image_sample_d v[8:11], v[2:7]: derivatives in v2..v5, coordinates in v6..v7.
    arena.code(coverageProgram,{0xf0880f00,0x00400802,0xf800180f,0x0b0a0908,0xbf810000});
    const auto derivatives=translateGCN(arena.memory,coverageProgram,ps);
    if (!derivatives) std::printf("%s\n", derivatives.error.c_str());
    assert(derivatives && derivatives.source.find("gcn_sample(M, t, sm, false, gf(v6), gf(v7))")!=std::string::npos);
    // v_writelane_b32 v1, s0, 1: in a pixel program, the quad lane the selector names.
    arena.code(coverageProgram,{0xd2040001,0x00010200,0xf800180f,0x01010101,0xbf810000});
    const auto written=translateGCN(arena.memory,coverageProgram,ps);
    if (!written) std::printf("%s\n", written.error.c_str());
    assert(written && written.source.find("== L.quadLane) v1 = ")!=std::string::npos);
    // image_sample v[8:11], v[2:4] da: the slice (v4) picks the array layer or cube face.
    arena.code(coverageProgram,{0xf0804f00,0x00400802,0xf800180f,0x0b0a0908,0xbf810000});
    const auto layered=translateGCN(arena.memory,coverageProgram,ps);
    if (!layered) std::printf("%s\n", layered.error.c_str());
    assert(layered && layered.source.find("t = gcn_slice(t, gf(v4));")!=std::string::npos);
    {
        // An integer target gets the exported bits, typed, through the struct return.
        std::uint32_t cntl[32] = {};
        const std::string uintEntry = gcnMetalEntryPoints(embedded, quad, "vs_embedded", "ps_quad", 0x302, cntl, false, 1, 1);
        assert(uintEntry.find("uint4 c0 [[color(0)]]") != std::string::npos && uintEntry.find("out.c0 = as_type<uint4>(L.color[0]);") != std::string::npos);
        const std::string plain = gcnMetalEntryPoints(embedded, quad, "vs_embedded", "ps_quad", 0x302, cntl);
        assert(plain.find("as_type<uint4>") == std::string::npos && plain.find("GCNFragmentMRT") == std::string::npos);
    }
    GCNTranslateOptions fetch; fetch.stage = GCNStage::vertex; fetch.name = "vs_fetch";
    fetch.userCount = 4; fetch.userData[0] = std::uint32_t(fetchShader); fetch.userData[2] = std::uint32_t(vertexTable);
    const auto fetched = translateGCN(arena.memory, fetchVS, fetch);
    if (!fetched) std::printf("%s\n", fetched.error.c_str());
    assert(fetched && fetched.position && fetched.params == 1);
    GCNTranslateOptions below = fetch; below.name = "vs_fetch_below"; below.userData[0] = std::uint32_t(fetchShaderBelow);
    const auto fetchedBelow = translateGCN(arena.memory, fetchVS, below);
    assert(fetchedBelow && fetchedBelow.position && fetchedBelow.params == 1);
    GCNTranslateOptions sizes; sizes.stage = GCNStage::vertex; sizes.name = "vs_resinfo";
    const auto resinfo = translateGCN(arena.memory, resinfoVS, sizes);
    if (!resinfo) std::printf("%s\n", resinfo.error.c_str());
    assert(resinfo && resinfo.position);
    // Refusals: no fetch-shader address, interpolation in a vertex program,
    // unmapped code.
    GCNTranslateOptions noUser = fetch; noUser.userCount = 0;
    assert(translateGCN(arena.memory, fetchVS, noUser).error == "s_swappc_b64 target not in user SGPRs");
    GCNTranslateOptions psAsVertex = ps; psAsVertex.stage = GCNStage::vertex;
    assert(translateGCN(arena.memory, quadPS, psAsVertex).error.find("interpolation outside a pixel program") == 0);
    assert(translateGCN(arena.memory, 0x40000, ps).error == "unreadable code");
    // A compute program with buffer and image stores; stores elsewhere are refused.
    loadComputeCS(arena);
    GCNTranslateOptions cs; cs.stage = GCNStage::compute; cs.name = "cs_copy"; cs.userCount = 16;
    const auto copied = translateGCN(arena.memory, computeCS, cs);
    if (!copied) std::printf("%s\n", copied.error.c_str());
    assert(copied && copied.stores);
    GCNTranslateOptions csAsPixel = cs; csAsPixel.stage = GCNStage::pixel;
    assert(translateGCN(arena.memory, computeCS, csAsPixel).error.find("buffer store outside a compute program") == 0);
    assert(gcnMetalComputeEntry("cs_copy").find("kernel void gcn_compute(") != std::string::npos);
    // LDS and s_barrier: the group's LDS after Q, branches decided for the group.
    loadLdsCS(arena);
    GCNTranslateOptions ldsOptions = cs; ldsOptions.name = "cs_lds";
    const auto shared = translateGCN(arena.memory, ldsCS, ldsOptions);
    if (!shared) std::printf("%s\n", shared.error.c_str());
    assert(shared && shared.lds && shared.stores);
    assert(shared.source.find("GCN_TG gcn_lds_word* T, uint ldsBytes)") != std::string::npos);
    assert(shared.source.find("gcn_lds_store(T, ldsBytes, a + 0u, v0)") != std::string::npos);
    assert(shared.source.find("v3 = gcn_lds_load(T, ldsBytes, a + 0u)") != std::string::npos);
    assert(shared.source.find("gcn_barrier();") != std::string::npos && shared.source.find("gcn_group_any(T, ") != std::string::npos);
    assert(!copied.lds && copied.source.find("gcn_group_any") == std::string::npos);
    assert(gcnMetalComputeEntry("cs_lds", true).find("threadgroup atomic_uint* gcn_group [[threadgroup(0)]]") != std::string::npos);
    assert(gcnMetalComputeEntry("cs_lds", true).find("cs_lds(L, M, Q, gcn_group + 4, D.ldsBytes)") != std::string::npos);
    // LDS in a vertex program is still refused.
    GCNTranslateOptions ldsAsVertex = ldsOptions; ldsAsVertex.stage = GCNStage::vertex;
    assert(translateGCN(arena.memory, ldsCS, ldsAsVertex).error.find("LDS access other than a swizzle") == 0);
    // The Metal prelude carries the shared runtime text.
    const std::string prelude = gcnMetalPrelude();
    assert(prelude.find("#include <metal_stdlib>") != std::string::npos && prelude.find("GCN_FN float4 gcn_sample(") != std::string::npos);
    std::ofstream out(argv[1]);
    out << embedded.source << quad.source << fetched.source << fetchedBelow.source << resinfo.source << copied.source << shared.source;
    std::puts("PASS GCN translator stage 1: embedded VS, quad PS, fetch-shader VS, compute stores and LDS translated; refusals reported");
}
