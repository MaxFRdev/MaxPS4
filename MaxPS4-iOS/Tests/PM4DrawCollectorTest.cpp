#include "../Native/PM4DrawCollector.hpp"
#include <cassert>
#include <cstdio>

using namespace maxps4::gpu;
namespace {
const std::vector<std::uint32_t> targetState = {
    0xc0016900, 0x8e, 0xf,
    0xc0056900, 0x318, 0x110, 1, 1, 0, 0xa << 2,
    0xc0017900, 0x242, 4,
};
struct Fixture {
    std::uint64_t base = 0x10000;
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(64);
    unsigned reads = 0;
    GCNMemory memory{[this](std::uint64_t address, void* output, std::size_t size) {
        ++reads;
        if (address < base || address - base > bytes.size() || size > bytes.size() - (address - base)) return false;
        std::memcpy(output, bytes.data() + (address - base), size);
        return true;
    }, {}};
    PM4DrawStats stats;
    PM4DrawCollector collector{memory, stats};
    std::vector<std::vector<std::uint32_t>> draws;
    std::vector<unsigned> primitives;
    std::vector<std::array<float, 4>> blendConstants;
    std::vector<PM4Draw> full; // Every draw, with its depth and culling state.
    Fixture() { run(targetState); }
    void run(const std::vector<std::uint32_t>& words) {
        collector.execute(words.data(), words.size(), [this](const PM4Draw& draw) {
            full.push_back(draw);
            draws.push_back(draw.indices); primitives.push_back(draw.primitive);
            blendConstants.push_back({draw.blendConstant[0], draw.blendConstant[1], draw.blendConstant[2], draw.blendConstant[3]});
        });
    }
};
}
int main() {
    {
        // DMA_DATA copies and fills guest memory in order; GDS stays ignored.
        std::vector<std::uint8_t> ram(256);
        for (unsigned k = 0; k < 16; ++k) ram[k] = std::uint8_t(k + 1);
        const std::uint64_t base = 0x20000;
        auto inside = [&](std::uint64_t a, std::size_t n) { return a >= base && a - base + n <= ram.size(); };
        GCNMemory memory{
            [&](std::uint64_t a, void* out, std::size_t n) { return inside(a, n) && std::memcpy(out, ram.data() + (a - base), n); },
            [&](std::uint64_t a, const void* in, std::size_t n) { return inside(a, n) && std::memcpy(ram.data() + (a - base), in, n); }};
        PM4DrawStats stats;
        PM4DrawCollector collector{memory, stats};
        const std::vector<std::uint32_t> words = {
            0xc0055000, 0, std::uint32_t(base), 0, std::uint32_t(base + 0x20), 0, 8,                // copy 8 bytes
            0xc0055000, 2u << 29, 0xaabbccdd, 0, std::uint32_t(base + 0x30), 0, 8,                 // fill 8 bytes
            0xc0055000, 1u << 20, std::uint32_t(base), 0, 0x100, 0, 4,                              // to GDS: ignored
        };
        collector.execute(words.data(), words.size(), [](const PM4Draw&) {});
        assert(std::memcmp(ram.data() + 0x20, ram.data(), 8) == 0);
        assert(ram[0x30] == 0xdd && ram[0x33] == 0xaa && ram[0x37] == 0xaa && ram[0x38] == 0);
        assert(stats.dmaCopies == 2 && stats.ignored[0x50] == 1 && stats.failed == 0);
        std::puts("PASS DMA_DATA copies and fills guest memory in submission order");
    }
    {
        // ELIMINATE_FAST_CLEAR (CB_COLOR_CONTROL MODE 2) draws nothing; the
        // target's next draw carries the clear (CB_COLOR0_CLEAR_WORD0/1), once.
        Fixture f;
        f.run({0xc0016900, 0x202, 2u << 4, 0xc0026900, 0x323, 0x11223344, 0x55667788, 0xc0012d00, 3, 2});
        assert(f.full.empty() && f.stats.fastClears == 1);
        f.run({0xc0016900, 0x202, 1u << 4, 0xc0012d00, 3, 2, 0xc0012d00, 3, 2});
        assert(f.full.size() == 2 && f.full[0].fastClear && !f.full[1].fastClear);
        assert(f.full[0].clearWords[0] == 0x11223344 && f.full[0].clearWords[1] == 0x55667788 && f.full[0].target == 0x11000);
        assert(f.stats.depthReport().find("effacements rapides : 1") != std::string::npos);
        std::puts("PASS fast clear eliminations become a clear of the target's next draw");
    }
    {
        // Packets whose effect is lost are counted by name; sync packets and
        // NUM_INSTANCES 1 are not, and the stream goes on to the next draw.
        Fixture f;
        assert(f.stats.ignoredReport().empty());
        f.run({0xc0032400, 0, 0, 0, 0,                   // DRAW_INDIRECT
               0xc0002f00, 1,                            // NUM_INSTANCES 1: nothing lost
               0xc0002f00, 4,                            // NUM_INSTANCES 4: one instance drawn
               0xc0003800, 0, 0xc0003800, 0,             // DRAW_INDEX_INDIRECT_MULTI twice
               0xc0055000, 0, 0, 0, 0, 0, 0,             // DMA_DATA
               0xc0004600, 0,                            // EVENT_WRITE: synchronization only
               0xc0012d00, 3, 2});                       // DRAW_INDEX_AUTO
        assert(f.stats.failed == 0 && f.draws.size() == 1);
        assert(f.stats.ignored[0x24] == 1 && f.stats.ignored[0x2f] == 1 && f.stats.ignored[0x38] == 2 &&
               f.stats.ignored[0x50] == 1 && f.stats.ignored[0x46] == 0);
        assert(f.stats.ignoredReport() ==
               "\nPaquets non gérés : DRAW_INDIRECT ×1, NUM_INSTANCES > 1 ×1, DRAW_INDEX_INDIRECT_MULTI ×2, DMA_DATA ×1");
    }
    {
        // Depth state is only counted: Z enabled and written, back faces culled,
        // then the same with no colour target mask.
        Fixture f;
        f.run({0xc0016900, 0x10, 3, 0xc0016900, 0x14, 0x1000, 0xc0016900, 0x16, 1 | (2 << 11),
               0xc0016900, 0x200, 2 | 4 | (3 << 4), 0xc0016900, 0x205, 2, 0xc0012d00, 3, 2});
        assert(f.stats.failed == 0 && f.draws.size() == 1);
        assert(f.stats.depthTested == 1 && f.stats.depthWritten == 1 && f.stats.depthOnly == 0 && f.stats.cullBack == 1 && !f.stats.cullFront);
        f.run({0xc0016900, 0x8e, 0, 0xc0012d00, 3, 2});
        assert(f.stats.depthTested == 2 && f.stats.depthOnly == 1 && f.stats.skipped == 1 && f.draws.size() == 1);
        assert(f.stats.lastDepth.find("16x24") != std::string::npos && f.stats.lastDepth.find("0x100000") != std::string::npos);
        assert(f.stats.depthReport().find("2 dessins") != std::string::npos);
        // The colour draw carries the decoded state: Z_32_FLOAT at 0x100000,
        // 16x24 pixels, test and write with <=, back faces culled, CCW front.
        const PM4Draw& d = f.full[0];
        assert(d.colour && d.depthTarget == 0x100000 && d.depthWidth == 16 && d.depthHeight == 24);
        assert(d.depthTest && d.depthWrite && d.depthFunc == 3 && !d.depthClear);
        assert(d.cull == 2 && !d.frontClockwise);
    }
    {
        // With depthOnlyDraws, a draw writing depth and no colour is reported,
        // sized as its depth buffer; one that only tests depth stays skipped.
        Fixture f;
        f.collector.depthOnlyDraws = true;
        f.run({0xc0016900, 0x10, 3, 0xc0016900, 0x12, 0x2000, 0xc0016900, 0x16, 3 | (1 << 11),
               0xc0016900, 0x200, 2 | 4 | (1 << 4), 0xc0016900, 0x205, 1 | 4, 0xc0016900, 0x8e, 0,
               0xc0016900, 0x0c, 0, 0xc0016900, 0x0d, 0x40004000, 0xc0012d00, 3, 2});
        assert(f.stats.failed == 0 && f.full.size() == 1 && f.stats.skipped == 0);
        const PM4Draw& d = f.full[0];
        // DB_Z_READ_BASE stands for a missing write base.
        assert(!d.colour && d.target == 0 && d.mask == 0 && d.depthTarget == 0x200000);
        assert(d.pitch == 32 && d.height == 16 && d.scissor[2] == 32 && d.scissor[3] == 16);
        assert(d.depthFunc == 1 && d.cull == 1 && d.frontClockwise);
        f.run({0xc0016900, 0x200, 2 | (1 << 4), 0xc0012d00, 3, 2});
        assert(f.full.size() == 1 && f.stats.skipped == 1);
        // A depth clear (DB_RENDER_CONTROL bit 0, DB_DEPTH_CLEAR) is reported even
        // without Z_ENABLE; its value is clamped to [0, 1].
        f.run({0xc0016900, 0x00, 1, 0xc0016900, 0x0b, gcn_detail::u32(0.25f), 0xc0016900, 0x200, 0, 0xc0012d00, 3, 2});
        assert(f.full.size() == 2 && f.full[1].depthClear && f.full[1].depthClearValue == 0.25f && !f.full[1].depthTest);
        f.run({0xc0016900, 0x0b, gcn_detail::u32(2.0f), 0xc0012d00, 3, 2});
        assert(f.full.size() == 3 && f.full[2].depthClearValue == 1.0f);
        // Without a depth format (DB_Z_INFO 0), nothing is depth: skipped again.
        f.run({0xc0016900, 0x10, 0, 0xc0012d00, 3, 2});
        assert(f.full.size() == 3 && f.stats.skipped == 2);
    }
    {
        // Stencil: DB_STENCIL_INFO 8-bit, STENCIL_ENABLE with EQUAL front and
        // NOTEQUAL back (BACKFACE_ENABLE), ops and ref/mask per face.
        Fixture f;
        f.run({0xc0016900, 0x10, 3, 0xc0016900, 0x11, 1, 0xc0016900, 0x14, 0x1000, 0xc0016900, 0x15, 0x1000,
               0xc0016900, 0x16, 1 | (2 << 11),
               0xc0016900, 0x10b, 1 | (3 << 4) | (5 << 8) | (7 << 12) | (8 << 16) | (9 << 20),
               0xc0016900, 0x10c, 0x01ff0f80, 0xc0016900, 0x10d, 0x02f0ff40,
               0xc0016900, 0x200, 1 | 2 | (3 << 4) | 0x80 | (2 << 8) | (5u << 20), 0xc0012d00, 3, 2});
        assert(f.stats.failed == 0 && f.full.size() == 1 && f.stats.stencilTested == 1);
        const PM4Draw& d = f.full[0];
        assert(d.stencilTest && !d.stencilClear && d.depthTest && !d.depthWrite && d.depthTarget == 0x100000);
        assert(d.stencil[0].func == 2 && d.stencil[0].fail == 1 && d.stencil[0].zpass == 3 && d.stencil[0].zfail == 5);
        assert(d.stencil[0].ref == 0x80 && d.stencil[0].mask == 0x0f && d.stencil[0].writeMask == 0xff && d.stencil[0].opValue == 1);
        assert(d.stencil[1].func == 5 && d.stencil[1].fail == 7 && d.stencil[1].zpass == 8 && d.stencil[1].zfail == 9);
        assert(d.stencil[1].ref == 0x40 && d.stencil[1].mask == 0xff && d.stencil[1].writeMask == 0xf0 && d.stencil[1].opValue == 2);
        assert(f.stats.depthReport().find("1 avec stencil") != std::string::npos);
        // Without BACKFACE_ENABLE the back face takes the front's state.
        f.run({0xc0016900, 0x200, 1 | (2 << 8), 0xc0012d00, 3, 2});
        assert(f.full.size() == 2 && f.full[1].stencilTest && !f.full[1].depthTest);
        assert(f.full[1].stencil[1].func == 2 && f.full[1].stencil[1].zpass == 3 && f.full[1].stencil[1].ref == 0x80);
        // Without a stencil format, stencil is off and the draw has no depth buffer.
        f.run({0xc0016900, 0x11, 0, 0xc0012d00, 3, 2});
        assert(f.full.size() == 3 && !f.full[2].stencilTest && f.full[2].depthTarget == 0);
    }
    {
        // A stencil-only buffer (DB_Z_INFO 0): a mask written without colour is
        // reported with depthOnlyDraws, at the stencil address; a stencil clear too.
        Fixture f;
        f.collector.depthOnlyDraws = true;
        f.run({0xc0016900, 0x11, 1, 0xc0016900, 0x13, 0x3000, 0xc0016900, 0x16, 3 | (1 << 11), 0xc0016900, 0x8e, 0,
               0xc0016900, 0x10b, 3 << 4, 0xc0016900, 0x10c, 0x00ffff01,
               0xc0016900, 0x200, 1 | (7 << 8), 0xc0012d00, 3, 2});
        assert(f.stats.failed == 0 && f.full.size() == 1 && f.stats.skipped == 0);
        assert(!f.full[0].colour && f.full[0].depthTarget == 0x300000 && f.full[0].stencilTest && !f.full[0].depthTest);
        // KEEP everywhere writes nothing: skipped.
        f.run({0xc0016900, 0x10b, 0, 0xc0012d00, 3, 2});
        assert(f.full.size() == 1 && f.stats.skipped == 1);
        f.run({0xc0016900, 0x00, 2, 0xc0016900, 0x0a, 0x1a5, 0xc0016900, 0x200, 0, 0xc0012d00, 3, 2});
        assert(f.full.size() == 2 && f.full[1].stencilClear && f.full[1].stencilClearValue == 0xa5 && !f.full[1].depthClear);
    }
    {
        // MRT: CB_TARGET_MASK enables slots 1 and 2 beside 0. Slot 1 matches
        // target 0's size and is drawn with its blend; slot 2 (another size)
        // is dropped; slot 3 has no mask.
        Fixture f;
        f.run({0xc0016900, 0x8e, 0x0f5f,
               0xc0056900, 0x318 + 15, 0x200, 1, 1, 0, (0xau << 2) | (1u << 11),
               0xc0056900, 0x318 + 30, 0x300, 3, 1, 0, 0xau << 2,
               0xc0056900, 0x318 + 45, 0x400, 1, 1, 0, 0xau << 2,
               0xc0016900, 0x1e1, 0x40000000 | 5, 0xc0012d00, 3, 2});
        assert(f.stats.failed == 0 && f.full.size() == 1);
        const PM4Draw& d = f.full[0];
        assert(d.colour && d.target == 0x11000 && d.pitch == 16 && d.height == 8);
        assert(d.more[0].base == 0x20000 && d.more[0].mask == 5 && d.more[0].swap == 1 && d.more[0].blend == (0x40000000u | 5));
        assert(d.more[1].base == 0 && d.more[2].base == 0);
        assert(f.stats.extraTargets == 1 && f.stats.extraTargetsDropped == 1);
        assert(f.stats.depthReport().find("MRT : 1 cibles en plus, 1 écartées") != std::string::npos);
        // A slot on target 0's own address is dropped too.
        f.run({0xc0056900, 0x318 + 15, 0x110, 1, 1, 0, 0xau << 2, 0xc0012d00, 3, 2});
        assert(f.full.size() == 2 && f.full[1].more[0].base == 0 && f.stats.extraTargetsDropped == 3);
    }
    {
        // 16_16_16_16 FLOAT fails unless otherTargetFormats, then carries its format.
        Fixture f;
        f.run({0xc0016900, 0x31c, (0xcu << 2) | (7u << 8), 0xc0012d00, 3, 2});
        assert(f.stats.failed == 1 && f.stats.error == "colour target format" && f.full.empty());
        Fixture g;
        g.collector.otherTargetFormats = true;
        g.run({0xc0016900, 0x31c, (0xcu << 2) | (7u << 8), 0xc0012d00, 3, 2});
        assert(g.stats.failed == 0 && g.full.size() == 1 && g.full[0].format == 0xc && g.full[0].number == 7);
        g.run({0xc0016900, 0x31c, 0xau << 2, 0xc0012d00, 3, 2});
        assert(g.full.size() == 2 && g.full[1].format == 0xa && g.full[1].number == 0);
    }
    {
        // A colour draw whose depth test is off has no depth buffer, whatever
        // the other DB registers hold: Sonic Mania's and Mortal Kombat's draws.
        Fixture f;
        f.run({0xc0016900, 0x10, 3, 0xc0016900, 0x14, 0x1000, 0xc0016900, 0x200, 4, 0xc0012d00, 3, 2});
        assert(f.full.size() == 1 && f.full[0].colour && f.full[0].depthTarget == 0 && !f.full[0].depthWrite);
        assert(f.full[0].cull == 0);
    }
    for (const bool allMarkers : {false, true}) {
        Fixture f;
        // This input would exceed the expansion cap as a single fan. A dense
        // restart stream is legal when it emits no triangles instead.
        constexpr unsigned count = (1u << 20) / 3 + 3;
        f.bytes.resize(count * 2); // All raw indices are zero.
        f.run({0xc0017900, 0x242, 5, 0xc0016900, 0x103, allMarkers ? 0u : 99u,
               0xc0016900, 0x2a5, 1, 0xc0042700, count, 0x10000, 0, count, 0});
        assert(f.draws.empty() && f.reads == 1);
        if (allMarkers) assert(f.stats.failed == 0 && f.stats.skipped == 1);
        else assert(f.stats.failed == 1 && f.stats.error == "expanded draw count");
    }
    for (const unsigned primitive : {5u, 6u}) for (const unsigned width : {2u, 4u}) {
        Fixture f;
        const std::uint32_t source[] = {99,1,2,3,4,99,99,5,6,7,99};
        for (unsigned k = 0; k < 11; ++k) std::memcpy(f.bytes.data() + width * k, &source[k], width);
        f.run({0xc0017900, 0x242, primitive, 0xc0002a00, width == 4 ? 1u : 0u,
               0xc0016900, 0x103, 99, 0xc0016900, 0x2a5, 1, 0xc0016900, 0x102, 10,
               0xc0042700, 11, 0x10000, 0, 11, 0});
        assert(f.stats.failed == 0 && f.primitives.back() == 4);
        const std::vector<std::uint32_t> expected = primitive == 5 ?
            std::vector<std::uint32_t>{11,12,13,11,13,14,15,16,17} :
            std::vector<std::uint32_t>{11,12,13,13,12,14,15,16,17};
        assert(f.draws.back() == expected);
        // Restart is scoped to this draw, not a continuation into the next submission.
        f.run({0xc0042700, 3, 0x10000 + width * 7, 0, 3, 0});
        assert((f.draws.back() == std::vector<std::uint32_t>{15,16,17}));
    }
    for (const unsigned width : {2u, 4u}) {
        Fixture f;
        const std::uint32_t marker = width == 2 ? 0xffffu : 0xffffffffu;
        const std::uint32_t source[] = {98,1,2,marker,4,5,6};
        for (unsigned k = 0; k < 7; ++k) std::memcpy(f.bytes.data() + width * k, &source[k], width);
        f.run({0xc0017900, 0x242, 5, 0xc0002a00, width == 4 ? 1u : 0u,
               0xc0016900, 0x103, 0xffffffff, 0xc0016900, 0x2a5, 1,
               0xc0016900, 0x102, 1, 0xc0042700, 7, 0x10000, 0, 7, 0});
        assert((f.draws.back() == std::vector<std::uint32_t>{99,2,3,5,6,7}));
        // A restart marker becomes an ordinary index when restart is disabled.
        f.run({0xc0016900, 0x2a5, 0, 0xc0042700, 7, 0x10000, 0, 7, 0});
        assert(f.stats.failed == 0 && f.draws.back().size() == 15);
    }
    {
        Fixture f;
        const std::uint16_t source[] = {99,1,99,99,2,3,99};
        std::memcpy(f.bytes.data(), source, sizeof(source));
        f.run({0xc0017900, 0x242, 6, 0xc0016900, 0x103, 99, 0xc0016900, 0x2a5, 1,
               0xc0042700, 7, 0x10000, 0, 7, 0});
        assert(f.stats.failed == 0 && f.stats.skipped == 1 && f.draws.empty());
        // Auto draws do not use restart markers.
        f.run({0xc0017900, 0x242, 5, 0xc0016900, 0x103, 1, 0xc0012d00, 4, 2});
        assert((f.draws.back() == std::vector<std::uint32_t>{0,1,2,0,2,3}));
    }
    {
        Fixture f;
        f.run({0xc0046900, 0x105, gcn_detail::u32(0.25f), gcn_detail::u32(0.5f),
               gcn_detail::u32(0.75f), gcn_detail::u32(1.0f), 0xc0012d00, 3, 2});
        f.run({0xc0016900, 0x106, gcn_detail::u32(0.125f), 0xc0012d00, 3, 2});
        assert((f.blendConstants[0] == std::array<float,4>{0.25f,0.5f,0.75f,1.0f}));
        assert((f.blendConstants[1] == std::array<float,4>{0.25f,0.125f,0.75f,1.0f}));
        f.run({0xc0001200, 0}); // CLEAR_STATE resets context registers.
        f.run(targetState);
        f.run({0xc0012d00, 3, 2});
        assert((f.blendConstants[2] == std::array<float,4>{0,0,0,0}));
    }
    {
        Fixture f;
        f.run({0xc0017900, 0x242, 5, 0xc0012d00, 5, 2});
        assert(f.stats.failed == 0 && f.primitives.back() == 4);
        assert((f.draws.back() == std::vector<std::uint32_t>{0,1,2,0,2,3,0,3,4}));
        f.run({0xc0012d00, 4, 2}); // The persistent register still selects a fan.
        assert((f.draws.back() == std::vector<std::uint32_t>{0,1,2,0,2,3}));
        assert(f.stats.failed == 0 && f.primitives.back() == 4 && f.reads == 0);
    }
    for (const unsigned width : {2u, 4u}) {
        Fixture f;
        const std::uint32_t source[] = {7,3,9,1};
        for (unsigned k = 0; k < 4; ++k) std::memcpy(f.bytes.data() + width * k, &source[k], width);
        f.run({0xc0017900, 0x242, 5, 0xc0002a00, width == 4 ? 1u : 0u,
               0xc0016900, 0x102, 4, 0xc0042700, 4, 0x10000, 0, 4, 0});
        assert(f.stats.failed == 0 && f.primitives.back() == 4);
        assert((f.draws.back() == std::vector<std::uint32_t>{11,7,13,11,13,5}));
    }
    {
        Fixture f;
        for (unsigned count = 1; count < 3; ++count)
            f.run({0xc0017900, 0x242, 5, 0xc0042700, count, 0xdead0000, 0, count, 0});
        assert(f.stats.failed == 0 && f.stats.skipped == 2);
        assert(f.draws.empty() && f.reads == 0);
        f.run({0xc0012d00, 1u << 20, 2});
        assert(f.stats.failed == 1 && f.stats.error == "expanded draw count" && f.reads == 0);
    }
    {
        Fixture f;
        const std::uint16_t indices[] = {999, 12, 7, 9};
        std::memcpy(f.bytes.data(), indices, sizeof(indices));
        f.run({0xc0012600, 0x10000, 0}); // INDEX_BASE in an earlier submission.
        f.run({0xc0016900, 0x102, 4});   // VGT_INDX_OFFSET affects vertex IDs, not buffer bytes.
        f.run({0xc0033500, 3, 1, 3, 0});
        assert(f.stats.failed == 0 && f.stats.draws == 1 && f.reads == 1);
        assert((f.draws == std::vector<std::vector<std::uint32_t>>{{16, 11, 13}}));
        // max_size limits the draw count, not offset + count: the writer emits count as max_size.
        f.run({0xc0033500, 1, 3, 1, 0});
        assert(f.stats.failed == 0 && f.draws.back() == std::vector<std::uint32_t>{13});
    }
    {
        Fixture f;
        f.base = 0x100020000ULL;
        const std::uint32_t indices[] = {0xdeadbeef, 0x10000, 8, 3};
        std::memcpy(f.bytes.data(), indices, sizeof(indices));
        // A direct indexed draw also establishes the persistent base for later offset draws.
        f.run({0xc0002a00, 1, 0xc0042700, 4, 0x20000, 1, 1, 0});
        f.run({0xc0033500, 3, 1, 3, 0});
        assert(f.stats.failed == 0 && f.draws.size() == 2);
        assert((f.draws.back() == std::vector<std::uint32_t>{0x10000, 8, 3}));
        // Switch back to 16-bit indices without changing the base.
        f.run({0xc0002a00, 0, 0xc0033500, 1, 2, 1, 0});
        assert(f.stats.failed == 0 && f.draws.back() == std::vector<std::uint32_t>{0});
    }
    auto reject = [](const std::vector<std::uint32_t>& words, const char* error, unsigned reads = 0) {
        Fixture f;
        f.run(words);
        assert(f.stats.failed == 1 && f.stats.error.rfind(error, 0) == 0 && f.draws.empty() && f.reads == reads);
    };
    reject({0xc0016900, 0x2a5, 1, 0xc0042700, 3, 0x10000, 0, 3, 0}, "primitive restart topology", 1);
    {
        Fixture f;
        f.run({0xc0016900, 0x103, 99, 0xc0016900, 0x2a5, 1, 0xc0042700, 3, 0x10000, 0, 3, 0});
        assert(f.stats.failed == 0 && f.draws.size() == 1); // No marker: an enabled triangle list still draws.
    }
    reject({0xc0017900, 0x242, 0x13, 0xc0012d00, 4, 2}, "primitive type 19");
    reject({0xc0033500, 3, 0, 3, 0}, "index offset draw range"); // No base.
    reject({0xc0012600, 0x10001, 0, 0xc0033500, 3, 0, 3, 0}, "index offset draw range");
    reject({0xc0012600, 0x10000, 0, 0xc0033500, 2, 0, 3, 0}, "index offset draw range");
    reject({0xc0012600, 0x10000, 0, 0xc0033500, 3, 0, 0, 0}, "index offset draw range");
    reject({0xc0012600, 0x10000, 0, 0xc0033500, 3, 32, 3, 0}, "unreadable index buffer", 1);
    reject({0xc0012600, 0xfffffffc, 0xffff, 0xc0033500, 3, 2, 3, 0}, "index offset draw range");
    reject({0xc0012600, 0xfffffffc, 0xffff, 0xc0033500, 3, 0, 3, 0}, "index offset draw range");
    reject({0xc0012600, 0x10000, 0x10000}, "index base packet");
    reject({0xc0002600, 0x10000}, "index base packet");
    reject({0xc0023500, 3, 0, 3}, "index offset draw packet");
    reject({0xc0012600, 0x10000, 0, 0xc0033501, 3, 0, 3, 0}, "index offset draw flags");
    reject({0xc0012600, 0x10000, 0, 0xc0033500, 3, 0, 3, 1}, "index offset draw flags");
    // Index state belongs to its collector, not to another game's submission.
    Fixture a, b;
    a.run({0xc0012600, 0x10000, 0});
    b.run({0xc0033500, 3, 0, 3, 0});
    assert(a.stats.failed == 0 && b.stats.error == "index offset draw range");
    std::puts("PASS PM4 index-offset draws: 16/32-bit indices, persistent 48-bit bases, vertex offsets, bounds and isolation");
}
