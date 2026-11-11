#include "../Native/PM4ResourceScan.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
#include <chrono>
using namespace maxps4::gpu;
using namespace gcn_test;
int main(int argc, char** argv) {
    Arena arena;
    {
        Arena relativeArena; loadEmbeddedVS(relativeArena); loadRelativeSourcePS(relativeArena);
        relativeArena.put(0x13000,0x12345678);
        relativeArena.put(relativeSourcePS+4*19,0xc0060900); // load after the relative reads
        relativeArena.put(relativeSourcePS+4*20,0xbf8c007f);
        relativeArena.put(relativeSourcePS+4*21,0xbf810000);
        PM4Draw draw; draw.vs=embeddedVS; draw.ps=relativeSourcePS; draw.psVGPRCount=16; draw.psUserCount=16;
        draw.psUser[0]=9; draw.psUser[8]=0x13000;
        PM4ResourceScanner scanner(relativeArena.memory);
        const auto& ranges=scanner.scan(draw);
        assert(scanner.pixelError().empty() && scanner.scalarLoads().size()==1 && scanner.scalarLoads()[0].words[0]==0x12345678);
        bool found=false;
        for(const auto& range:ranges) found |= range.base<=0x13000 && range.base+range.size>=0x13004;
        assert(found);
        std::puts("PASS resource scan uses the PS VGPR allocation and continues after a relative source read");
    }
    {
        Arena spilled; loadEmbeddedVS(spilled); loadScalarSpillPS(spilled);
        spilled.put(0x13000,0xdecafbad);
        // A scalar load after the four restores must be discovered, even though
        // the scan initially represents only lane 0 and the spill uses lane 63.
        spilled.put(scalarSpillPS+4*13,0xc0060900); // s_load_dword s12,s[8:9],0
        PM4Draw draw; draw.vs=embeddedVS; draw.ps=scalarSpillPS; draw.psUserCount=4;
        draw.psUser[0]=0x13000;
        PM4ResourceScanner scanner(spilled.memory);
        for(unsigned repeat=0;repeat<2;++repeat) {
            const auto& discovered=scanner.scan(draw);
            assert(scanner.pixelError().empty() && scanner.scalarLoads().size()==1);
            assert(scanner.scalarLoads()[0].words[0]==0xdecafbad);
            bool found=false;
            for(const auto& range:discovered) found |= range.base<=0x13000 && range.base+range.size>=0x13004;
            assert(found);
        }
        GCNWave representative(spilled.memory,1); representative.s[0]=0x13000;
        assert(!representative.run(scalarSpillPS)); // no silent widening in normal interpretation
        representative.reset(); representative.s[0]=0x13000; representative.expandResourceLanes=true;
        assert(representative.run(scalarSpillPS) && representative.lanes()==1 && representative.s[12]==0xdecafbad);
        representative.reset(); assert(!representative.expandResourceLanes && representative.lanes()==1);
        std::puts("PASS representative resource scan resumes through scalar lane spills and finds the following constant load");
    }
    loadFetchVS(arena); loadQuadPS(arena);
    PM4Draw d;
    d.vs = fetchVS; d.vsUserCount = 4;
    d.vsUser[0] = std::uint32_t(fetchShader); d.vsUser[2] = std::uint32_t(vertexTable);
    d.ps = quadPS; d.psUserCount = 16;
    for (unsigned k = 0; k < 16; ++k) d.psUser[k] = quadSgprs[k];
    d.psInputAddr = 2; d.indices = {0, 1, 2};
    d.scissor[2] = 16; d.scissor[3] = 8;
    const auto ranges = scanDrawResources(arena.memory, d);
    for (const auto& r : ranges) std::printf("range 0x%llx + 0x%llx\n", (unsigned long long)r.base, (unsigned long long)r.size);
    // VS: the V# table read by s_load (0x10c00, 16 bytes) and the 3 vertices it
    // describes (0x10e00, 48 bytes), merged. PS: the 4x2 RGB565 texture
    // (0x12000, 16 bytes) and the constant buffer (0x13000, 16 bytes), merged.
    assert(ranges.size() == 2);
    assert(ranges[0].base == 0x10c00 && ranges[0].size == 0x230);
    assert(ranges[1].base == 0x12000 && ranges[1].size == 0x1010);
    // A program that faults keeps what it found before; nothing is invented.
    PM4Draw lost = d; lost.vs = 0x40000; lost.ps = 0x40000;
    assert(scanDrawResources(arena.memory, lost).empty());
    // With image capture, the pixel program's T# (s[0:7]) is kept as read, once.
    PM4ResourceScanner capturing(arena.memory, true);
    capturing.scan(d);
    assert(capturing.images().size() == 1);
    for (unsigned k = 0; k < 8; ++k) assert(capturing.images()[0][k] == quadSgprs[k]);
    assert(capturing.scan(lost).empty() && capturing.images().empty());
    // A tiled T# the CPU cannot read does not hide what the program reads after it.
    constexpr std::uint64_t afterImagePS = 0x14000;
    arena.code(afterImagePS, {
        0xf0800f00, 0x00400402, // image_sample v[4:7], v2, s[0:7], s[8:11] dmask:0xf
        0xc2080d00,             // s_buffer_load_dword s16, s[12:15], 0x0
        0xbf810000});
    PM4Draw tiledDraw = d;
    tiledDraw.ps = afterImagePS;
    tiledDraw.psUser[3] = 0x90d0032e;
    bool constants = false;
    for (const auto& range : capturing.scan(tiledDraw)) constants |= range.base <= 0x13000 && 0x13000 < range.base + range.size;
    assert(constants && capturing.images().size() == 1 && capturing.pixelError().empty());
    PM4ResourceScanner reused(arena.memory);
    auto check = [&] {
        const auto& current = reused.scan(d);
        assert(current.size() == ranges.size());
        for (std::size_t k = 0; k < ranges.size(); ++k)
            assert(current[k].base == ranges[k].base && current[k].size == ranges[k].size);
    };
    check();
    assert(reused.scan(lost).empty());
    check(); // Errors, registers and exports from the failed scan must be cleared.
    arena.code(quadPS, {0xbf810000});
    assert(reused.scan(d).size() == 1); // Re-read edited code, not a prior decode.
    loadQuadPS(arena);
    check();
    PM4Draw noUser = d; noUser.vsUserCount = 0;
    const auto& onlyPS = reused.scan(noUser);
    assert(onlyPS.size() == 1 && onlyPS[0].base == 0x12000);
    check();
    arena.put(vertexTable, 0x18000);
    const auto& changed = reused.scan(d);
    bool observed = false;
    for (const auto& range : changed) observed |= range.base == 0x18000 && range.size == 48;
    assert(observed); // Re-read resource descriptors and vertex data too.
    arena.put(vertexTable, 0x10e00);
    check();
    // The pixel program's constant load reaches its table; a site not run stays 0.
    reused.scan(d);
    assert(reused.scalarLoads().size() == 1);
    const auto load = reused.scalarLoads()[0];
    assert(load.pc == quadPS + 8 && load.count == 1);
    const std::vector<GCNScalarSite> sites = {{quadPS + 4, 2, 3}, {quadPS + 8, 1, 0}};
    std::vector<std::uint32_t> table;
    assert(reused.scalarTable(sites, 7, table));
    assert(table[0] == std::uint32_t(load.address) && table[1] == std::uint32_t(load.address >> 32) && table[2] == load.words[0]);
    assert(table[3] == 0 && table[4] == 0);
    const std::vector<GCNScalarSite> elsewhere = {{quadPS + 4, 2, 0}};
    assert(!reused.scalarTable(elsewhere, 4, table));
    // A load the scan branches over is read from the user SGPRs all the same.
    constexpr std::uint64_t skipping = 0x11000;
    arena.code(skipping, {0xbf820001, 0xc0080d00, 0xbf810000}); // s_branch +1, s_load_dword s16, s[12:13], 0, s_endpgm
    arena.put(0x13800, 0xabcd1234);
    PM4Draw branching = d;
    branching.ps = skipping; branching.psUserCount = 14; branching.psUser[12] = 0x13800; branching.psUser[13] = 0;
    reused.scan(branching);
    assert(reused.scalarLoads().empty());
    const std::vector<GCNScalarSite> skipped = {{skipping + 4, 1, 0}};
    assert(reused.scalarTable(skipped, 3, table));
    assert(table[0] == 0x13800 && table[1] == 0 && table[2] == 0xabcd1234);
    arena.code(skipping, {0, 0, 0});
    arena.put(0x13800, 0);
    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
        constexpr unsigned rounds = 6, count = 2000;
        volatile std::uint64_t checksum = 0;
        double times[2]{};
        for (unsigned warm = 0; warm < 100; ++warm) reused.scan(d);
        for (unsigned round = 0; round < rounds; ++round) for (unsigned turn = 0; turn < 2; ++turn) {
            const unsigned mode = (round + turn) % 2;
            const auto started = std::chrono::steady_clock::now();
            for (unsigned n = 0; n < count; ++n) {
                if (mode) { const auto& r = reused.scan(d); checksum += r[0].size + r.size(); }
                else { PM4ResourceScanner fresh(arena.memory); const auto& r = fresh.scan(d); checksum += r[0].size + r.size(); }
            }
            times[mode] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() / (rounds * count);
        }
        assert(checksum != 0);
        std::printf("RESOURCE SCAN benchmark: fresh %.3f us/draw, reused %.3f us/draw (CPU fixture, not game FPS)\n", times[0], times[1]);
    }
    std::puts("PASS PM4 resource scan: scalar table, vertex buffer, texture and constant buffer extents, merged");
}
