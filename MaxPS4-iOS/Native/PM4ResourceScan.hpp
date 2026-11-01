#pragma once
// The guest memory a draw's programs read on the GPU, found on the CPU: one
// lane of the vertex program (first index) and one of the pixel program (a
// pixel at the scissor centre) run in the GCN interpreter, which reports the
// whole extent of every scalar load, V# and T# they address. The GPU then
// receives copies of these ranges only (PM4MetalRenderer), never the whole
// guest address space. A resource used only on a path that representative
// lane does not take is missed and reads as zero on the GPU, never outside.
#include "PM4DrawCollector.hpp"
#include <algorithm>
#include <array>

namespace maxps4::gpu {
struct PM4Range { std::uint64_t base = 0, size = 0; };

class PM4ResourceScanner {
public:
    explicit PM4ResourceScanner(const GCNMemory& memory, bool captureImages = false)
        : wave_(memory, 1, &cache_), captureImages_(captureImages) {}
    PM4ResourceScanner(const PM4ResourceScanner&) = delete;
    PM4ResourceScanner& operator=(const PM4ResourceScanner&) = delete;
    // Valid until the next scan. Only storage is reused: code, descriptors and
    // guest values are read again on every draw. Not a concurrent scanner.
    const std::vector<PM4Range>& scan(const PM4Draw& d) {
    constexpr std::uint64_t largest = 256ull << 20;
    found_.clear(); merged_.clear(); cache_.clear(); images_.clear();
    auto note = [&](std::uint64_t base, std::uint64_t bytes) {
        if (bytes && bytes <= largest && base + bytes > base) found_.push_back({base, bytes});
    };
    auto& wave = wave_;
    {
        wave.reset();
        wave.resource = note;
        if (captureImages_) wave.imageResource = [this](const auto& descriptor) { image(descriptor); };
        for (unsigned k = 0; k < d.vsUserCount; ++k) wave.s[k] = d.vsUser[k];
        wave.v(0, 0) = d.indices.empty() ? 0 : d.indices[0];
        wave.run(d.vs); // a failure keeps what was found before it
    }
    {
        wave.reset();
        wave.resource = note;
        if (captureImages_) wave.imageResource = [this](const auto& descriptor) { image(descriptor); };
        for (unsigned k = 0; k < d.psUserCount; ++k) wave.s[k] = d.psUser[k];
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
        wave.run(d.ps);
    }
    // Sorted, with overlapping or nearly adjacent (4 KiB) ranges merged.
    wave.resource = {}; // Do not retain the callback to this scan's stack.
    wave.imageResource = {};
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
        found_.clear(); merged_.clear(); cache_.clear(); images_.clear(); stored_.clear();
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
        for (unsigned k = 0; k < c.userCount; ++k) wave.s[k] = c.user[k];
        wave.run(c.program);
        wave.resource = {}; wave.stored = {}; wave.imageResource = {};
        std::sort(found_.begin(), found_.end(), [](const PM4Range& a, const PM4Range& b) { return a.base < b.base; });
        for (const auto& r : found_) {
            if (!merged_.empty() && r.base <= merged_.back().base + merged_.back().size + 4096) {
                merged_.back().size = std::max(merged_.back().size, r.base + r.size - merged_.back().base);
            } else merged_.push_back(r);
        }
        return merged_;
    }
    const std::vector<PM4Range>& stored() const { return stored_; }
    const std::vector<std::array<std::uint32_t, 8>>& images() const { return images_; }
private:
    GCNCodeCache cache_;
    GCNWave wave_;
    std::vector<PM4Range> found_, merged_, stored_;
    std::vector<std::array<std::uint32_t, 8>> images_;
    bool captureImages_;
    void image(const std::array<std::uint32_t, 8>& descriptor) {
        if (images_.size() < 8 && std::find(images_.begin(), images_.end(), descriptor) == images_.end()) images_.push_back(descriptor);
    }
};

inline std::vector<PM4Range> scanDrawResources(const GCNMemory& memory, const PM4Draw& d) {
    PM4ResourceScanner scanner(memory);
    return scanner.scan(d);
}
} // namespace maxps4::gpu
