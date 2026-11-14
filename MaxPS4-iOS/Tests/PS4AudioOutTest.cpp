#include "../Native/maxps4_ps4_audio_out.h"
#include "GuestELFFixture.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace maxps4;
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }
}
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 65536, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Threads threads(memory);
    PS4AudioOut audio(memory, threads);
    constexpr uint64_t samples = 0x10000;
    std::vector<float> out(4096 * 2);

    assert(audio.open(1,0,0,256,48000,1) == PS4AudioOut::error(0x0f));
    assert(audio.output(1,0) == PS4AudioOut::error(0x03));
    assert(audio.initialize() == 0 && audio.initialize() == PS4AudioOut::error(0x0e));
    assert(audio.open(1,5,0,256,48000,1) == PS4AudioOut::error(0x0a));
    assert(audio.open(1,0,1,256,48000,1) == PS4AudioOut::error(0x03));
    assert(audio.open(1,0,0,0,48000,1) == PS4AudioOut::error(0x06));
    assert(audio.open(1,0,0,300,48000,1) == PS4AudioOut::error(0x06));
    assert(audio.open(1,0,0,4096,48000,1) == PS4AudioOut::error(0x06));
    assert(audio.open(1,0,0,256,44100,1) == PS4AudioOut::error(0x08));
    assert(audio.open(1,0,0,256,48000,8) == PS4AudioOut::error(0x07));

    // Stereo S16: exact conversion, read in two parts, then silence.
    const int32_t stereo = int32_t(audio.open(1,0,0,256,48000,1));
    assert(stereo == 1);
    std::vector<uint8_t> wire(256 * 2 * 2);
    for (size_t i = 0; i < 512; ++i) ps4::put_le(wire.data(), 2 * i, uint16_t(int16_t(i % 2 ? -16384 : 8192)), 2);
    assert(memory.write(samples, wire.data(), wire.size()));
    assert(audio.output(stereo, samples) == 0);
    assert(audio.read(out.data(), 100) == 100);
    assert(near(out[0], 0.25f) && near(out[1], -0.5f) && near(out[198], 0.25f) && near(out[199], -0.5f));
    assert(audio.read(out.data(), 200) == 156);
    assert(near(out[310], 0.25f) && out[312] == 0.0f && out[399] == 0.0f);
    assert(audio.read(out.data(), 10) == 0 && out[0] == 0.0f);
    assert(audio.output(stereo, UINT64_MAX) == PS4AudioOut::error(0x04));

    // Output paces the thread at one buffer per 256/48000 s (5.33 ms): the
    // first returns at once, then each waits for the previous buffer.
    assert(audio.close(stereo) == 0 && audio.close(stereo) == PS4AudioOut::error(0x03));
    const int32_t paced = int32_t(audio.open(1,0,0,256,48000,1));
    constexpr double period = 256.0 * 1000.0 / 48000.0; // 5.33 ms
    constexpr uint64_t period_us = 256ull * 1000000 / 48000;
    auto timed = [](auto&& body) {
        std::array<double, 20> each{};
        for (double& duration : each) { const auto before = Clock::now(); body(); duration = milliseconds(before); }
        std::array<double, 20> sorted = each;
        std::sort(sorted.begin(), sorted.end());
        double total = 0; for (double d : each) total += d;
        struct { double total, median, most; } out{total, sorted[sorted.size() / 2], sorted.back()};
        return out;
    };
    // Reference: the same wait primitive Output uses (PS4Threads::sleep for one
    // period), on this machine, right now. Wake-up accuracy is a property of
    // the host, not of AudioOut: Windows rounds waits to its timer tick (calls
    // up to 16 ms measured), loaded macOS runners under ASan coalesce timers.
    const auto reference = timed([&] { assert(threads.sleep(period_us) == ps4::ok); });
    const auto output = timed([&] { assert(audio.output(paced, 0) == 0); });
    std::printf("AudioOut pacing: 20 outputs in %.2f ms (per call median %.2f, max %.2f) | "
                "20 sleeps of one period (%.2f ms) in %.2f ms (median %.2f, max %.2f)\n",
                output.total, output.median, output.most, period, reference.total, reference.median, reference.most);
    // Output really blocks: never earlier than the 19 deadlines it waits for
    // (the first returns at once, each next one waits for the previous buffer).
    assert(output.total > 19 * period * 0.9);
    // One period per call, not more: a call never waits longer than this host
    // takes to sleep one period. Deadlines are absolute (port due time), so a
    // late wake-up shortens the next wait instead of adding up. A pacing bug
    // that waits two periods per call doubles the median on any host with a
    // timer finer than the period. Medians ignore a few stalled wake-ups.
    // This replaces a fixed bound on the total (20 * 5.33 * 3 ms) that failed
    // on loaded macOS runners (#311) and did not even catch such a bug (two
    // periods per call total about 213 ms).
    assert(output.median <= 1.5 * reference.median + 0.5);
    // No call blocks indefinitely (a lost wake-up).
    assert(output.most < 1000);
    // An interrupted run (stop request) does not wait.
    threads.interrupt();
    auto start = Clock::now();
    for (int i = 0; i < 20; ++i) assert(audio.output(paced, 0) == 0);
    assert(milliseconds(start) < 20);
    threads.resume();

    // Mono float and 8-channel S16 mix into the same read; the mix clamps.
    const int32_t mono = int32_t(audio.open(1,1,0,256,48000,3));
    const int32_t surround = int32_t(audio.open(1,2,0,256,48000,2));
    assert(mono == 2 && surround == 3);
    std::vector<uint8_t> floats(256 * 4);
    for (size_t i = 0; i < 256; ++i) {
        const float value = i == 0 ? 0.75f : i == 1 ? NAN : 0.125f;
        uint32_t bits; std::memcpy(&bits, &value, 4);
        ps4::put_le(floats.data(), 4 * i, bits, 4);
    }
    assert(memory.write(samples, floats.data(), floats.size()));
    assert(audio.output(mono, samples) == 0);
    std::vector<uint8_t> eight(256 * 8 * 2, 0);
    // FL 0.25, FR 0, C 0.5, SL 0.5, SR -1 -> L 0.25+0.25+0.25, R 0.25-0.5.
    const int16_t frame[8] = {8192, 0, 16384, 0, 16384, -32768, 0, 0};
    for (size_t f = 0; f < 256; ++f)
        for (size_t c = 0; c < 8; ++c) ps4::put_le(eight.data(), 2 * (8 * f + c), uint16_t(frame[c]), 2);
    assert(memory.write(samples, eight.data(), eight.size()));
    assert(audio.output(surround, samples) == 0);
    assert(audio.read(out.data(), 256) == 256);
    assert(out[0] == 1.0f && near(out[1], 0.75f - 0.25f)); // 0.75 + 0.75 clamps to 1.
    assert(near(out[2], 0.75f) && near(out[3], -0.25f));   // NaN played as silence.
    assert(near(out[4], 0.875f) && near(out[5], -0.125f));

    // Ports full, then the ring drops the oldest frames without a reader.
    for (int32_t i = 4; i <= int32_t(PS4AudioOut::port_limit); ++i)
        assert(audio.open(1,0,0,2048,48000,0) == i);
    assert(audio.open(1,0,0,256,48000,1) == PS4AudioOut::error(0x05));
    threads.interrupt();
    for (int i = 0; i < 7; ++i) assert(audio.output(4, samples) == 0); // 7 x 2048 > 12000 frames.
    threads.resume();
    const auto stats = audio.stats();
    assert(stats.ports == PS4AudioOut::port_limit);
    assert(stats.dropped == 7 * 2048 - PS4AudioOut::ring_frames);
    assert(stats.outputs > 40 && stats.written == 256 * 3 + 7 * 2048 && stats.silent > 0);
    assert(stats.audible > 0 && stats.audible < stats.written); // The NaN mono frame is muted.
    assert(audio.read(out.data(), 4096) == 4096);

    // Concurrent host reads while a game thread outputs.
    std::thread game([&] { for (int i = 0; i < 50; ++i) assert(audio.output(paced, samples) == 0); });
    for (int i = 0; i < 200; ++i) { (void)audio.read(out.data(), 128); std::this_thread::yield(); }
    game.join();

    audio.reset();
    assert(audio.output(paced, 0) == PS4AudioOut::error(0x03));
    assert(audio.stats().ports == 0 && audio.initialize() == 0);
    std::puts("PASS PS4 AudioOut validation, S16/float/8-channel conversion, mixing, pacing, interrupt and ring overflow");
}
