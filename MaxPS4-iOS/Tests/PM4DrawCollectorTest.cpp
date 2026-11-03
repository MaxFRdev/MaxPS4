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
    Fixture() { run(targetState); }
    void run(const std::vector<std::uint32_t>& words) {
        collector.execute(words.data(), words.size(), [this](const PM4Draw& draw) {
            draws.push_back(draw.indices); primitives.push_back(draw.primitive);
            blendConstants.push_back({draw.blendConstant[0], draw.blendConstant[1], draw.blendConstant[2], draw.blendConstant[3]});
        });
    }
};
}
int main() {
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
        assert(f.stats.failed == 1 && f.stats.error == error && f.draws.empty() && f.reads == reads);
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
