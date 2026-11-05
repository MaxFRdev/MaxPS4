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
    // The Metal prelude carries the shared runtime text.
    const std::string prelude = gcnMetalPrelude();
    assert(prelude.find("#include <metal_stdlib>") != std::string::npos && prelude.find("GCN_FN float4 gcn_sample(") != std::string::npos);
    std::ofstream out(argv[1]);
    out << embedded.source << quad.source << fetched.source << fetchedBelow.source << resinfo.source << copied.source;
    std::puts("PASS GCN translator stage 1: embedded VS, quad PS, fetch-shader VS and compute stores translated; refusals reported");
}
