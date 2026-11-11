// Opt-in HLE timing for frame-time jitter (issue #293): per-service call
// count / total / max host time, and the interval between flip requests.
// Off by default; when off, record()/frame() are one relaxed load. When on,
// a few relaxed atomics per HLE call, no locks, no allocation, no logging:
// the report is built only on request.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace maxps4 {
class HLETiming {
public:
    static constexpr size_t services = 1024;   // Service IDs above this are counted as "other".
    static constexpr size_t frame_window = 600; // ~10 s at 60 FPS.

    void enable(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    void clear() {
        for (auto& s : stats_) { s.count.store(0, std::memory_order_relaxed); s.total.store(0, std::memory_order_relaxed); s.max.store(0, std::memory_order_relaxed); }
        for (auto& f : frames_) f.store(0, std::memory_order_relaxed);
        frame_next_.store(0, std::memory_order_relaxed);
        last_frame_.store(0, std::memory_order_relaxed);
    }
    // One completed HLE call of `service`, `ns` long.
    void record(uint32_t service, uint64_t ns) {
        if (!enabled()) return;
        Stat& s = stats_[std::min<size_t>(service, services)];
        s.count.fetch_add(1, std::memory_order_relaxed);
        s.total.fetch_add(ns, std::memory_order_relaxed);
        uint64_t seen = s.max.load(std::memory_order_relaxed);
        while (ns > seen && !s.max.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {}
    }
    // A flip request at `now_ns`: the interval since the previous one.
    void frame(uint64_t now_ns) {
        if (!enabled()) return;
        const uint64_t previous = last_frame_.exchange(now_ns, std::memory_order_relaxed);
        if (!previous || now_ns <= previous) return;
        const uint64_t at = frame_next_.fetch_add(1, std::memory_order_relaxed);
        frames_[at % frame_window].store(now_ns - previous, std::memory_order_relaxed);
    }
    struct FrameSummary { size_t frames = 0; double median_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0, fps = 0; size_t over_17ms = 0, over_33ms = 0; };
    FrameSummary frames() const {
        FrameSummary out;
        std::vector<uint64_t> v;
        v.reserve(frame_window);
        const size_t n = size_t(std::min<uint64_t>(frame_next_.load(std::memory_order_relaxed), frame_window));
        for (size_t i = 0; i < n; ++i) if (uint64_t x = frames_[i].load(std::memory_order_relaxed)) v.push_back(x);
        if (v.empty()) return out;
        std::sort(v.begin(), v.end());
        auto at = [&](double q) { return double(v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]) / 1e6; };
        uint64_t sum = 0;
        for (uint64_t x : v) { sum += x; out.over_17ms += x > 17'500'000; out.over_33ms += x > 33'400'000; }
        out.frames = v.size(); out.median_ms = at(0.5); out.p95_ms = at(0.95); out.p99_ms = at(0.99);
        out.max_ms = double(v.back()) / 1e6; out.fps = 1e9 * double(v.size()) / double(sum);
        return out;
    }
    // Bounded text: the frame summary and the `top` services by total time.
    template <class Name> std::string report(Name&& name, size_t top = 10) const {
        std::string out;
        char line[192];
        const FrameSummary f = frames();
        if (f.frames) {
            std::snprintf(line, sizeof line, "Images (%zu dernières) : %.1f FPS · médiane %.2f ms · p95 %.2f · p99 %.2f · max %.2f · >17,5 ms %zu · >33,4 ms %zu\n",
                f.frames, f.fps, f.median_ms, f.p95_ms, f.p99_ms, f.max_ms, f.over_17ms, f.over_33ms);
            out += line;
        }
        struct Row { size_t id; uint64_t count, total, max; };
        std::vector<Row> rows;
        for (size_t i = 0; i <= services; ++i) {
            const uint64_t c = stats_[i].count.load(std::memory_order_relaxed);
            if (c) rows.push_back({i, c, stats_[i].total.load(std::memory_order_relaxed), stats_[i].max.load(std::memory_order_relaxed)});
        }
        if (rows.empty()) return out;
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.total > b.total; });
        out += "HLE (temps hôte cumulé, attentes incluses) :";
        for (size_t i = 0; i < std::min(top, rows.size()); ++i) {
            const Row& r = rows[i];
            std::snprintf(line, sizeof line, " %.40s %llu× %.1f ms (moy %.1f µs, max %.2f ms)",
                r.id == services ? "autre" : name(uint32_t(r.id)), (unsigned long long)r.count, double(r.total) / 1e6,
                double(r.total) / 1e3 / double(r.count), double(r.max) / 1e6);
            out += line;
            if (i + 1 < std::min(top, rows.size())) out += " ·";
        }
        out += "\n";
        return out;
    }
private:
    struct Stat { std::atomic<uint64_t> count {0}, total {0}, max {0}; };
    std::atomic<bool> enabled_ {false};
    std::array<Stat, services + 1> stats_ {};
    std::array<std::atomic<uint64_t>, frame_window> frames_ {};
    std::atomic<uint64_t> frame_next_ {0}, last_frame_ {0};
};
} // namespace maxps4
