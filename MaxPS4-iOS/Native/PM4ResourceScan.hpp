#pragma once
// The guest memory a draw's programs read on the GPU, found on the CPU: one
// lane of the vertex program (first index) and one of the pixel program (a
// pixel at the scissor centre) run in the GCN interpreter, which reports the
// whole extent of every scalar load, V# and T# they address. The GPU then
// receives copies of these ranges only (PM4MetalRenderer), never the whole
// guest address space. A resource used only on a path that representative
// lane does not take is missed and reads as zero on the GPU, never outside.
// A tiled image, which the CPU cannot read, gives zeros here: the lane carries
// on, as constants read after it would otherwise read as zero on the GPU.
// A V# beyond the copy limit (engines give dynamic buffers an unbounded
// record count) is copied only where the draw reads it: for the vertex
// program, the elements of the draw's index range around the one the lane
// read; elsewhere, a window around the lane's address.
#include "PM4DrawCollector.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace maxps4::gpu {
struct PM4Range { std::uint64_t base = 0, size = 0; };

struct PM4ScalarLoad { std::uint64_t pc, address; std::uint32_t count; std::array<std::uint32_t, 16> words; };

class PM4ResourceScanner {
public:
    explicit PM4ResourceScanner(const GCNMemory& memory, bool captureImages = false)
        : memory_(memory), wave_(memory, 1, &cache_), captureImages_(captureImages) {}
    PM4ResourceScanner(const PM4ResourceScanner&) = delete;
    PM4ResourceScanner& operator=(const PM4ResourceScanner&) = delete;
    // Valid until the next scan. Only storage is reused: code, descriptors and
    // guest values are read again on every draw. Not a concurrent scanner.
    const std::vector<PM4Range>& scan(const PM4Draw& d) {
    constexpr std::uint64_t largest = 256ull << 20;
    found_.clear(); merged_.clear(); cache_.clear(); images_.clear(); samplers_.clear(); scalarLoads_.clear();
    auto note = [&](std::uint64_t base, std::uint64_t bytes) {
        if (bytes && bytes <= largest && base + bytes > base) found_.push_back({base, bytes});
    };
    auto& wave = wave_;
    std::uint32_t first = d.indices.empty() ? 0 : d.indices[0], lowest = first, highest = first;
    for (const auto i : d.indices) { lowest = std::min(lowest, i); highest = std::max(highest, i); }
    auto oversized = [&](bool vertex) {
        return [&, vertex](std::uint64_t base, std::uint64_t bytes, std::uint32_t stride, std::uint64_t address) {
            if (bytes <= largest || base + bytes <= base) return;
            const std::uint64_t end = base + bytes;
            std::uint64_t from, to;
            if (vertex && stride) {
                const std::uint64_t before = std::uint64_t(first - lowest) * stride, after = std::uint64_t(highest - first + 1) * stride;
                from = address - base >= before ? address - before : base;
                to = end - address >= after ? address + after : end;
            } else {
                from = address - base >= window ? address - window : base;
                to = end - address >= window ? address + window : end;
            }
            if (to > from) note(from, std::min<std::uint64_t>(to - from, largest));
        };
    };
    {
        wave.reset();
        wave.resource = note;
        wave.tiledReadsZero = true;
        wave.buffer = oversized(true);
        if (captureImages_) wave.imageResource = [this](const auto& descriptor) { image(descriptor); };
        for (unsigned k = 0; k < d.vsUserCount; ++k) wave.s[k] = d.vsUser[k];
        wave.v(0, 0) = d.indices.empty() ? 0 : d.indices[0];
        vsRan_ = wave.run(d.vs); // a failure keeps what was found before it
        vsError_ = wave.error;
        vsExports_.clear();
        for (const auto& e : wave.exports) {
            std::array<std::uint32_t, 5> x{e.target, e.value[0][0], e.value[1][0], e.value[2][0], e.value[3][0]};
            vsExports_.push_back(x);
        }
    }
    {
        wave.reset();
        wave.resource = note;
        wave.tiledReadsZero = true;
        wave.buffer = oversized(false);
        if (captureImages_) wave.imageResource = [this](const auto& descriptor) { image(descriptor); };
        for (unsigned k = 0; k < d.psUserCount; ++k) wave.s[k] = d.psUser[k];
        std::copy(d.psUser, d.psUser + 16, psUser_.begin());
        psUserCount_ = d.psUserCount;
        const float x = float(d.scissor[0] + d.scissor[2]) / 2, y = float(d.scissor[1] + d.scissor[3]) / 2;
        static const unsigned sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        unsigned reg = 0;
        for (unsigned bit = 0; bit < 16; ++bit) {
            if (!((d.psInputAddr >> bit) & 1)) continue;
            float value[3] = {1.0f / 3, 1.0f / 3, 1.0f / 3};
            if (bit == 8) value[0] = x + 0.5f;
            else if (bit == 9) value[0] = y + 0.5f;
            for (unsigned k = 0; k < sizes[bit]; ++k) wave.v(reg + k, 0) = gcn_detail::u32(value[k]);
            reg += sizes[bit];
        }
        wave.attribute = [](unsigned, unsigned) { return GCNAttributePlane{}; };
        wave.scalarLoad = [this](std::uint64_t pc, std::uint64_t address, const std::uint32_t* words, unsigned count) {
            PM4ScalarLoad load{pc, address, count, {}};
            std::copy(words, words + count, load.words.begin());
            scalarLoads_.push_back(load);
        };
        wave.run(d.ps);
        psError_ = wave.error;
    }
    // Sorted, with overlapping or nearly adjacent (4 KiB) ranges merged.
    wave.resource = {}; // Do not retain the callback to this scan's stack.
    wave.imageResource = {}; wave.buffer = {}; wave.scalarLoad = {};
    std::sort(found_.begin(), found_.end(), [](const PM4Range& a, const PM4Range& b) { return a.base < b.base; });
    for (const auto& r : found_) {
        if (!merged_.empty() && r.base <= merged_.back().base + merged_.back().size + 4096) {
            merged_.back().size = std::max(merged_.back().size, r.base + r.size - merged_.back().base);
        } else merged_.push_back(r);
    }
    return merged_;
    }
    // A dispatch: one lane (group 0, thread 0) of its program, whose stores
    // are not written; their resources are also given by stored(). Images
    // the program addresses are captured whatever captureImages.
    const std::vector<PM4Range>& scan(const PM4Dispatch& c) {
        found_.clear(); merged_.clear(); cache_.clear(); images_.clear(); samplers_.clear(); scalarLoads_.clear(); stored_.clear(); dispatchError_.clear();
        constexpr std::uint64_t largest = 256ull << 20;
        auto& wave = wave_;
        wave.reset();
        wave.resource = [&](std::uint64_t base, std::uint64_t bytes) {
            if (bytes && bytes <= largest && base + bytes > base) found_.push_back({base, bytes});
        };
        wave.stored = [&](std::uint64_t base, std::uint64_t bytes) {
            if (bytes && bytes <= largest && base + bytes > base) stored_.push_back({base, bytes});
        };
        wave.imageResource = [this](const auto& descriptor) { image(descriptor); };
        wave.buffer = [&](std::uint64_t base, std::uint64_t bytes, std::uint32_t, std::uint64_t address) {
            if (bytes <= largest || base + bytes <= base) return;
            const std::uint64_t from = address - base >= window ? address - window : base;
            const std::uint64_t to = base + bytes - address >= window ? address + window : base + bytes;
            if (to > from) found_.push_back({from, std::min<std::uint64_t>(to - from, largest)});
        };
        for (unsigned k = 0; k < c.userCount; ++k) wave.s[k] = c.user[k];
        // Resource discovery executes one representative lane. Some game
        // compute shaders contain work loops that are unsuitable for this
        // scan; stop promptly and let the caller refuse that dispatch.
        if (!wave.run(c.program, 4096) && wave.error == "instruction limit reached")
            dispatchError_ = wave.error;
        wave.resource = {}; wave.stored = {}; wave.imageResource = {}; wave.buffer = {};
        std::sort(found_.begin(), found_.end(), [](const PM4Range& a, const PM4Range& b) { return a.base < b.base; });
        for (const auto& r : found_) {
            if (!merged_.empty() && r.base <= merged_.back().base + merged_.back().size + 4096) {
                merged_.back().size = std::max(merged_.back().size, r.base + r.size - merged_.back().base);
            } else merged_.push_back(r);
        }
        return merged_;
    }
    const std::string& dispatchError() const { return dispatchError_; }
    const std::vector<PM4Range>& stored() const { return stored_; }
    const std::vector<std::array<std::uint32_t, 8>>& images() const { return images_; }
    // The pixel program's scalar loads, in the order the scan ran them.
    const std::vector<PM4ScalarLoad>& scalarLoads() const { return scalarLoads_; }
    // The table a draw's pixel program reads them from, for `sites` sorted by
    // pc: its uniform loads then skip the memory search on every pixel. The scan
    // runs one pixel's path: loads on the others are read here, in program order
    // from the user SGPRs, as most are constants of the same tables. A wrong guess
    // is harmless, the program checks the address and searches. False: no site.
    bool scalarTable(const std::vector<GCNScalarSite>& sites, std::size_t words, std::vector<std::uint32_t>& table) const {
        table.assign(words, 0);
        bool any = false;
        for (const auto& load : scalarLoads_) {
            const auto site = std::lower_bound(sites.begin(), sites.end(), load.pc, [](const GCNScalarSite& s, std::uint64_t pc) { return s.pc < pc; });
            if (site == sites.end() || site->pc != load.pc || site->count != load.count) continue;
            table[site->offset] = std::uint32_t(load.address);
            table[site->offset + 1] = std::uint32_t(load.address >> 32);
            std::copy(load.words.begin(), load.words.begin() + load.count, table.begin() + site->offset + 2);
            any = true;
        }
        std::array<std::uint32_t, 128> s{};
        std::copy(psUser_.begin(), psUser_.begin() + std::min<std::size_t>(psUserCount_, 16), s.begin());
        for (const auto& site : sites) {
            std::uint32_t word = 0;
            if (!memory_.read(site.pc, &word, 4) || (word >> 27) != 0x18) continue; // Not a scalar load.
            static const unsigned counts[] = {1, 2, 4, 8, 16};
            const unsigned opcode = (word >> 22) & 31, d = (word >> 15) & 0x7f, base = ((word >> 9) & 0x3f) * 2;
            if ((opcode & 7) > 4 || opcode > 12 || counts[opcode & 7] != site.count || d + site.count > 128 || base + 3 >= 128) continue;
            const bool reached = table[site.offset] || table[site.offset + 1];
            if (!reached) {
                if (!(word & 0x100) && (word & 0xff) >= 104) continue; // A literal or special offset.
                const std::uint64_t offset = word & 0x100 ? 4ull * (word & 0xff) : s[word & 0xff];
                std::uint64_t address, limit = std::numeric_limits<std::uint64_t>::max();
                if (opcode >= 8) {
                    address = (std::uint64_t(s[base]) | std::uint64_t(s[base + 1] & 0xfff) << 32) + offset;
                    const std::uint32_t stride = (s[base + 1] >> 16) & 0x3fff;
                    limit = stride ? std::uint64_t(s[base + 2]) * stride : s[base + 2];
                } else address = ((std::uint64_t(s[base]) | std::uint64_t(s[base + 1]) << 32) & 0xffffffffffffull) + offset;
                bool readable = address != 0;
                for (unsigned n = 0; readable && n < site.count; ++n) {
                    std::uint32_t value = 0;
                    readable = offset + 4 * n + 4 > limit || memory_.read(address + 4 * n, &value, 4);
                    table[site.offset + 2 + n] = value;
                }
                if (!readable) { std::fill_n(table.begin() + site.offset, 2 + site.count, 0u); continue; }
                table[site.offset] = std::uint32_t(address);
                table[site.offset + 1] = std::uint32_t(address >> 32);
                any = true;
            }
            std::copy_n(table.begin() + site.offset + 2, site.count, s.begin() + d);
        }
        return any;
    }
    // The S# first sampling each of images() (zeros: read without a sampler).
    const std::vector<std::array<std::uint32_t, 4>>& samplers() const { return samplers_; }
    // The last scanned draw's vertex lane: (target, x, y, z, w) per export.
    const std::vector<std::array<std::uint32_t, 5>>& vertexExports() const { return vsExports_; }
    bool vertexRan() const { return vsRan_; }
    const std::string& vertexError() const { return vsError_; }
    // Why the last scanned draw's pixel lane stopped early (empty: it ended).
    const std::string& pixelError() const { return psError_; }
private:
    // Half the span copied around an oversized V#'s access outside the vertex program.
    static constexpr std::uint64_t window = 1ull << 20;
    GCNCodeCache cache_;
    const GCNMemory& memory_;
    GCNWave wave_;
    std::vector<PM4Range> found_, merged_, stored_;
    std::string dispatchError_;
    std::vector<std::array<std::uint32_t, 5>> vsExports_;
    bool vsRan_ = false;
    std::string vsError_, psError_;
    std::vector<std::array<std::uint32_t, 8>> images_;
    std::vector<PM4ScalarLoad> scalarLoads_;
    std::array<std::uint32_t, 16> psUser_{};
    unsigned psUserCount_ = 0;
    bool captureImages_;
    std::vector<std::array<std::uint32_t, 4>> samplers_;
    void image(const std::array<std::uint32_t, 8>& descriptor) {
        if (images_.size() < 8 && std::find(images_.begin(), images_.end(), descriptor) == images_.end()) {
            images_.push_back(descriptor);
            samplers_.push_back(wave_.imageSampler);
        }
    }
};

inline std::vector<PM4Range> scanDrawResources(const GCNMemory& memory, const PM4Draw& d) {
    PM4ResourceScanner scanner(memory);
    return scanner.scan(d);
}
} // namespace maxps4::gpu
