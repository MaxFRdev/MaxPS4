// HLETiming (issue #293): off by default and inert, exact per-service sums,
// frame percentiles over a bounded window, concurrent recording, bounded report.
#include "maxps4_hle_timing.h"
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
using maxps4::HLETiming;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static const char* name(uint32_t id) { return id == 7 ? "sceKernelWaitEqueue" : id == 9 ? "sceAudioOutOutput" : "x"; }
int main() {
    static HLETiming t; // Large: keep it off the stack.
    // Off by default: nothing is recorded, the report is empty.
    CHECK(!t.enabled());
    t.record(7, 1000); t.frame(1); t.frame(16'000'000);
    CHECK(t.report(name).empty() && t.frames().frames == 0);

    t.enable(true);
    t.record(7, 2'000'000); t.record(7, 4'000'000); t.record(9, 500'000);
    t.record(5000, 10); // Out-of-range IDs land in "other".
    // 100 frames: 98 at 16.6 ms, one at 20 ms, one at 40 ms.
    uint64_t now = 1'000'000'000;
    t.frame(now);
    for (int i = 0; i < 100; ++i) { now += i == 50 ? 40'000'000 : i == 60 ? 20'000'000 : 16'600'000; t.frame(now); }
    const auto f = t.frames();
    CHECK(f.frames == 100);
    CHECK(std::fabs(f.median_ms - 16.6) < 1e-9);
    CHECK(std::fabs(f.max_ms - 40.0) < 1e-9);
    CHECK(std::fabs(f.p99_ms - 40.0) < 1e-9 || std::fabs(f.p99_ms - 20.0) < 1e-9);
    CHECK(f.over_17ms == 2 && f.over_33ms == 1);
    CHECK(f.fps > 58.0 && f.fps < 60.0);
    const std::string r = t.report(name);
    CHECK(r.find("sceKernelWaitEqueue 2× 6.0 ms") != std::string::npos);
    CHECK(r.find("max 4.00 ms") != std::string::npos);
    CHECK(r.find("autre 1×") != std::string::npos);
    CHECK(r.find("sceKernelWaitEqueue") < r.find("sceAudioOutOutput")); // Sorted by total time.
    // A non-increasing timestamp adds no interval.
    t.frame(now); CHECK(t.frames().frames == 100);

    // The window is bounded: old intervals are overwritten.
    t.clear();
    CHECK(t.report(name).empty());
    now = 1;
    for (size_t i = 0; i <= HLETiming::frame_window * 3; ++i) t.frame(now += (i < HLETiming::frame_window * 2 ? 50'000'000 : 16'000'000));
    CHECK(t.frames().frames == HLETiming::frame_window && std::fabs(t.frames().max_ms - 16.0) < 1e-9);

    // Concurrent threads: exact counts and sums, no data race (TSan/ASan in CI).
    t.clear();
    std::vector<std::thread> threads;
    for (int k = 0; k < 8; ++k) threads.emplace_back([k] {
        for (uint64_t i = 1; i <= 20000; ++i) { t.record(uint32_t(k % 2 ? 9 : 7), i); if (k == 0) t.frame(i * 1000); }
    });
    for (auto& th : threads) th.join();
    const std::string c = t.report(name, 2);
    CHECK(c.find("sceKernelWaitEqueue 80000×") != std::string::npos && c.find("sceAudioOutOutput 80000×") != std::string::npos);
    CHECK(c.find("max 0.02 ms") != std::string::npos);
    // Disabled again: inert.
    t.enable(false); t.record(7, 1); CHECK(t.report(name, 2) == c);
    std::puts("PASS HLE timing: off by default, exact sums, bounded frame window, percentiles, concurrency");
    return 0;
}
