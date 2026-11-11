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
    loadEmbeddedVS(arena); loadQuadPS(arena); loadFetchVS(arena); loadResinfoVS(arena); loadDepthCompareVS(arena);
    GCNTranslateOptions vs; vs.stage = GCNStage::vertex; vs.name = "vs_embedded";
    const auto embedded = translateGCN(arena.memory, embeddedVS, vs);
    assert(embedded && embedded.position && embedded.params == 1 && !embedded.quad);
    GCNTranslateOptions ps; ps.name = "ps_quad";
    const auto quad = translateGCN(arena.memory, quadPS, ps);
    if (!quad) std::printf("%s\n", quad.error.c_str());
    assert(quad && quad.colors == 3 && quad.attributes == 1 && quad.flat == 0 && quad.quad);
    assert(!quad.mayKill); // save EXEC, enable WQM, restore the original coverage
    constexpr uint64_t coverageProgram=0x11800;
    loadRelativeSourcePS(arena);
    GCNTranslateOptions relativeOptions=ps; relativeOptions.name="ps_relative_source"; relativeOptions.vgprCount=16;
    const auto relativeSource=translateGCN(arena.memory,relativeSourcePS,relativeOptions);
    assert(relativeSource && relativeSource.source.find("uint relativeValue = v0")!=std::string::npos &&
           relativeSource.source.find("switch (s124)")!=std::string::npos &&
           relativeSource.source.find("case 11u: relativeValue = v15")!=std::string::npos &&
           relativeSource.source.find("relativeValue = v16")==std::string::npos);
    GCNTranslateOptions unknownAllocation=relativeOptions; unknownAllocation.vgprCount=0;
    assert(translateGCN(arena.memory,relativeSourcePS,unknownAllocation).error.find("relative VGPR allocation unavailable")==0);
    unknownAllocation.vgprCount=4;
    assert(translateGCN(arena.memory,relativeSourcePS,unknownAllocation).error.find("relative VGPR allocation unavailable")==0);
    // VOP3 uses the same unsigned M0 indexing. Relative destinations remain
    // refused because they cannot be represented by the bounded source switch.
    arena.code(coverageProgram,{0xbefc0300,0xd3860014,0x00000104,0xbf810000});
    assert(translateGCN(arena.memory,coverageProgram,relativeOptions));
    for(const uint32_t unsupported:{0x7e288504u,0x7e288904u}) {
        arena.code(coverageProgram,{unsupported,0xbf810000});
        assert(!translateGCN(arena.memory,coverageProgram,relativeOptions));
    }
    loadScalarSpillPS(arena);
    GCNTranslateOptions spillOptions=ps; spillOptions.name="ps_scalar_spill";
    const auto scalarSpill=translateGCN(arena.memory,scalarSpillPS,spillOptions);
    assert(scalarSpill && !scalarSpill.laneIndex && scalarSpill.source.find("scalarSpill3455")!=std::string::npos);
    arena.put(scalarSpillPS,0xbf800000); // full EXEC throughout the vector reuse
    arena.put(scalarSpillPS+4*13,0x7e6a02f2); // overwrite bank with 1.0
    arena.put(scalarSpillPS+4*14,0x7e000335); // ordinary read after overwrite
    spillOptions.name="ps_scalar_spill_reuse";
    const auto scalarSpillReuse=translateGCN(arena.memory,scalarSpillPS,spillOptions);
    assert(scalarSpillReuse);
    arena.code(coverageProgram,{0xd2040035,2u|(191u<<9),0xd2020008,309u|(191u<<9),0xbf810000});
    assert(translateGCN(arena.memory,coverageProgram,spillOptions)); // VOP3 constant selectors
    // Refuse reads without initialization, dynamic lane selectors, ordinary
    // vector reads of spilled data, EXEC changes and branches across the spill.
    for(const auto& mutation:std::vector<std::pair<unsigned,uint32_t>>{
        {1,0xbf800000}, {1,0x046a0200}, {5,0x7e080335},
        {5,0xbefe04c1}, {5,0xbf820001}, {5,0x7e6a0280},
        {9,0x02110f35}}) {
        loadScalarSpillPS(arena); arena.put(scalarSpillPS+4*mutation.first,mutation.second);
        assert(!translateGCN(arena.memory,scalarSpillPS,spillOptions));
    }
    loadScalarSpillPS(arena);
    for(unsigned form=0;form<2;++form) for(unsigned lane=0;lane<64;++lane) {
        const uint32_t write=form ? 0xd2040035u : 0x046a0a02u;
        const uint32_t read=form ? 0xd2020008u : 0x02100b35u;
        if(form) arena.code(coverageProgram,{write,2u|(5u<<9),read,309u|(5u<<9),0xbf810000});
        else arena.code(coverageProgram,{write,read,0xbf810000});
        GCNWave wave(arena.memory,64); wave.setExec(0); wave.s[2]=0x12340000u+lane; wave.s[5]=128+lane;
        assert(wave.run(coverageProgram) && wave.s[8]==0x12340000u+lane);
        for(unsigned n=0;n<64;++n) assert(wave.v(53,n)==(n==lane ? 0x12340000u+lane : 0u));
        assert(!translateGCN(arena.memory,coverageProgram,spillOptions));
    }
    loadPrivateLDSPs(arena);
    loadJoinedLDSPs(arena);
    GCNTranslateOptions joinedOptions=ps; joinedOptions.name="ps_joined_lds";
    const auto joinedLDS=translateGCN(arena.memory,joinedLDSPs,joinedOptions);
    assert(joinedLDS && joinedLDS.source.find("privateLDS2")!=std::string::npos);
    arena.put(joinedLDSPs+4*16,0x02000002); // first read overwrites the address VGPR
    assert(!translateGCN(arena.memory,joinedLDSPs,joinedOptions));
    loadJoinedLDSPs(arena);
    // A branch can bypass this store; rebuilding the address at the join does
    // not make the store dominate the read. It must remain refused.
    arena.code(0x1a400,{
        0xbefc03ff,65536,0xd2480002,0x000100c1,0x460404c1,0x34040482,
        0xbf850002,0xd8340000,0x00000002,
        0xbefc03ff,65536,0xd2480003,0x000100c1,0x460606c1,0x34060682,
        0xd8d80000,0x04000003,0xbf810000});
    assert(!translateGCN(arena.memory,0x1a400,joinedOptions));
    GCNTranslateOptions privateOptions=ps; privateOptions.name="ps_private_lds";
    const auto privateLDS=translateGCN(arena.memory,privateLDSPs,privateOptions);
    assert(privateLDS && privateLDS.source.find("privateLDS0")!=std::string::npos && privateLDS.source.find("privateLDS1")!=std::string::npos);
    for (const auto& mutation : std::vector<std::pair<unsigned,uint32_t>>{
        {5,0x34040483}, // address stride 8 can alias another slot
        {8,0xd8340104}, // unproved LDS offset
        {1,512},       // different M0 limit
        {17,512},      // different limit before reads
        {6,0xd8d80000}, // read before the entry-block store
        {6,0xd8360000}  // GDS flag
    }) {
        loadPrivateLDSPs(arena);
        arena.put(privateLDSPs+4*mutation.first,mutation.second);
        assert(!translateGCN(arena.memory,privateLDSPs,privateOptions));
    }
    loadPrivateLDSPs(arena);
    arena.code(0x11c00,{0xd2480022,0x000100c1,0xbf810000});
    const auto exactMbcnt=translateGCN(arena.memory,0x11c00,ps);
    assert(exactMbcnt && exactMbcnt.laneIndex);
    arena.code(0x11c00,{0xd2460021,0x000100c1,0xd2480022,0x000242c1,0xf8001801,0x22,0xbf810000});
    GCNTranslateOptions countOptions=ps; countOptions.name="ps_mbcnt";
    const auto counted=translateGCN(arena.memory,0x11c00,countOptions);
    assert(counted && counted.laneIndex);
    arena.code(0x11b00,{0xebf33000,0x80030f01,0xf80008cf,0x1211100f,0xbf810000});
    GCNTranslateOptions typedOptions; typedOptions.stage=GCNStage::vertex; typedOptions.name="vs_typed";
    const auto typedProgram=translateGCN(arena.memory,0x11b00,typedOptions);
    assert(typedProgram && typedProgram.position);
    for (unsigned op=0;op<4;++op) {
        arena.code(0x11b40,{0xebf03000|(op<<16),0x80030f01,0xbf810000});
        assert(translateGCN(arena.memory,0x11b40,typedOptions));
    }
    arena.code(0x11b40,{0xe8040000,0x80030f01,0xbf810000});
    assert(!translateGCN(arena.memory,0x11b40,typedOptions)); // typed stores remain refused
    arena.code(0x11b40,{0xebf33000,0x80830f01,0xbf810000});
    assert(!translateGCN(arena.memory,0x11b40,typedOptions)); // TFE adds a status result, still unsupported
    arena.code(0x11a00,{0xf8001881,0x01010100,0xbf810000});
    GCNTranslateOptions depthExportOptions=ps; depthExportOptions.name="ps_depth_export";
    const auto depthExport=translateGCN(arena.memory,0x11a00,depthExportOptions);
    assert(depthExport && depthExport.depth && !depthExport.colors);
    const uint32_t emptyInputs[32]={};
    const auto depthEntry=gcnMetalEntryPoints(embedded,depthExport,"vs_embedded","ps_depth_export",0,emptyInputs);
    assert(depthEntry.find("float depth [[depth(any)]]")!=std::string::npos);
    assert(depthEntry.find("float4 color [[color(0)]]")==std::string::npos);
    arena.code(0x11a20,{0xf8001883,0x01010100,0xbf810000});
    assert(!translateGCN(arena.memory,0x11a20,depthExportOptions)); // stencil remains unsupported
    arena.code(0x11900,{0xf15c0200,0x0040040c,0xf800180f,0x07060504,0xbf810000});
    GCNTranslateOptions gatherOptions=ps; gatherOptions.name="ps_gather_lz";
    const auto gatherProgram=translateGCN(arena.memory,0x11900,gatherOptions);
    assert(gatherProgram && gatherProgram.colors==1);
    arena.code(0x11920,{0xf15c0300,0x0040040c,0xbf810000});
    assert(!translateGCN(arena.memory,0x11920,gatherOptions)); // component selection must be singular
    // The exact device instruction is GET_LOD (0x60), not a reserved MIMG.
    arena.code(coverageProgram,{0xf1800100,0x00e40a08,0xbf810000});
    const auto deviceLOD = translateGCN(arena.memory, coverageProgram, ps);
    assert(deviceLOD && deviceLOD.quad);
    assert(std::string(gcn_detail::mimg(96)) == "image_get_lod");
    assert(gcn_detail::mimg(66) == nullptr && gcn_detail::mimg(90) == nullptr);
    assert(std::string(gcn_detail::mimg(68)) == "image_gather4_l");
    assert(std::string(gcn_detail::mimg(92)) == "image_gather4_c_l_o");
    arena.code(coverageProgram,{0xf1800300,0x00400a00,0xf800180f,0x0d0c0b0a,0xbf810000});
    GCNTranslateOptions lodOptions=ps; lodOptions.name="ps_lod";
    const auto lodProgram=translateGCN(arena.memory,coverageProgram,lodOptions);
    assert(lodProgram && lodProgram.quad && lodProgram.colors == 1);
    GCNTranslateOptions lodVertex=lodOptions; lodVertex.stage=GCNStage::vertex;
    assert(!translateGCN(arena.memory,coverageProgram,lodVertex));
    // Unknown MIMG operations must retain their raw encoding in device reports.
    arena.code(coverageProgram,{0xf07c0f00,0x00030201,0xbf810000}); // reserved opcode 31
    const auto unknownImage = translateGCN(arena.memory, coverageProgram, ps);
    assert(!unknownImage);
    assert(unknownImage.error.find("unsupported image access") != std::string::npos);
    assert(unknownImage.error.find("opcode 31 words f07c0f00 00030201 size 2") != std::string::npos);
    assert(unknownImage.error.find("at 0x11800") != std::string::npos);
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
    GCNTranslateOptions depth; depth.stage = GCNStage::vertex; depth.name = "vs_depth_compare";
    const auto compared = translateGCN(arena.memory, depthCompareVS, depth);
    if (!compared) std::printf("%s\n", compared.error.c_str());
    assert(compared && compared.position);
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
    out << embedded.source << quad.source << fetched.source << fetchedBelow.source << resinfo.source << compared.source << copied.source << lodProgram.source << gatherProgram.source << depthExport.source << typedProgram.source << counted.source << privateLDS.source << joinedLDS.source << scalarSpill.source << scalarSpillReuse.source << relativeSource.source;
    std::puts("PASS GCN translator stage 1: embedded VS, quad PS, fetch-shader VS and compute stores translated; refusals reported");
}
