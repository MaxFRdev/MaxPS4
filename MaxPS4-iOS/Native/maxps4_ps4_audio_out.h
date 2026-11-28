#pragma once
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_abi.h"
#include "maxps4_ps4_threads.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace maxps4 {
// libSceAudioOut (shadPS4 libraries/audio/audioout.cpp): 48 kHz ports. Output
// queues one buffer and returns once the previous one has played, as the
// hardware does, so a game's audio thread runs at the real rate instead of
// spinning. Each port keeps a stereo float ring that the host drains with
// read(), mixing the ports; a full ring drops its oldest half at once.
// 8-channel buffers are downmixed (front + half centre + half surround).
// Volume, other port parameters and output time queries are not modelled.
class PS4AudioOut final {
public:
    static constexpr uint32_t rate = 48000, port_limit = 8;
    // A quarter of a second per port: enough to absorb a host render period.
    static constexpr size_t ring_frames = rate / 4;
    static int64_t error(uint32_t code) { return ps4::signed32(0x80260000U | code); }
    // `audible`: written frames with a non-zero sample (a game that only
    // queues silence is told apart from audio that never reaches the host).
    struct Stats { uint64_t outputs = 0, written = 0, dropped = 0, silent = 0, audible = 0; uint32_t ports = 0; };
    PS4AudioOut(PS4GuestMemory& memory, PS4Threads& threads) : memory_(memory), threads_(threads) {}
    int64_t initialize() {
        std::lock_guard<std::mutex> lock(lock_);
        if (initialized_) return error(0x0e);
        initialized_ = true; return 0;
    }
    // sceAudioOutOpen(user, type, index, length, frequency, param): the handle.
    int64_t open(int32_t, int32_t type, int32_t index, uint32_t length, uint32_t frequency, uint32_t param) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!initialized_) return error(0x0f);
        // Main, BGM, voice, personal, pad speaker and aux ports.
        if (type < 0 || (type > 4 && type != 127)) return error(0x0a);
        if (index != 0) return error(0x03);
        if (!length || length > 2048 || length % 256) return error(0x06);
        if (frequency != rate) return error(0x08);
        const uint32_t format = param & 0xff;
        if (format > 7) return error(0x07);
        for (uint32_t slot = 0; slot < port_limit; ++slot) {
            Port& port = ports_[slot];
            if (port.open) continue;
            try { port.ring.assign(ring_frames * 2, 0.0f); } catch (const std::bad_alloc&) { return error(0x0d); }
            port.open = true; port.length = length; port.format = format;
            port.head = port.count = 0; port.due = {};
            return int64_t(slot) + 1;
        }
        return error(0x05);
    }
    int64_t close(int32_t handle) {
        std::lock_guard<std::mutex> lock(lock_);
        Port* port = find(handle);
        if (!port) return error(0x03);
        *port = Port{}; return 0;
    }
    // sceAudioOutOutput(handle, samples): a null pointer only waits.
    int64_t output(int32_t handle, uint64_t samples) {
        uint32_t length = 0, format = 0;
        {
            std::lock_guard<std::mutex> lock(lock_);
            Port* port = find(handle);
            if (!port) return error(0x03);
            length = port->length; format = port->format;
        }
        std::vector<float> stereo;
        if (samples) {
            const uint32_t channels = format == 0 || format == 3 ? 1 : format == 1 || format == 4 ? 2 : 8;
            const bool floating = format == 3 || format == 4 || format == 5 || format == 7;
            std::vector<uint8_t> wire(size_t(length) * channels * (floating ? 4 : 2));
            if (!memory_.read(samples, wire.data(), wire.size())) return error(0x04);
            stereo.resize(size_t(length) * 2);
            auto sample = [&](size_t i) {
                if (!floating) return float(int16_t(ps4::get_le(wire.data() + 2 * i, 2))) / 32768.0f;
                const uint32_t bits = uint32_t(ps4::get_le(wire.data() + 4 * i, 4));
                float value; std::memcpy(&value, &bits, 4);
                return value == value ? value : 0.0f; // NaN plays as silence.
            };
            for (size_t frame = 0; frame < length; ++frame) {
                const size_t base = frame * channels;
                if (channels == 1) { stereo[2*frame] = stereo[2*frame+1] = sample(base); continue; }
                float left = sample(base), right = sample(base + 1);
                if (channels == 8) {
                    const float centre = 0.5f * sample(base + 2);
                    left += centre + 0.5f * sample(base + 4);
                    right += centre + 0.5f * sample(base + 5);
                }
                stereo[2*frame] = left; stereo[2*frame+1] = right;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const auto period = std::chrono::microseconds(uint64_t(length) * 1000000 / rate);
        std::chrono::steady_clock::time_point previous;
        {
            std::lock_guard<std::mutex> lock(lock_);
            Port* port = find(handle);
            if (!port) return error(0x03); // Closed by another thread meanwhile.
            ++stats_.outputs;
            if (samples) push(*port, stereo);
            // The previous buffer ends at `due`; this one plays after it.
            previous = port->due;
            port->due = std::max(port->due, now) + period;
        }
        // Interrupted when the run stops: the thread goes back to guest code.
        if (previous > now)
            (void)threads_.sleep(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(previous - now).count()));
        return 0;
    }
    // Host: mixes up to `frames` interleaved stereo frames into `out` (zeros
    // where no port has data). Returns the frames that had data.
    size_t read(float* out, size_t frames) {
        std::fill(out, out + frames * 2, 0.0f);
        std::lock_guard<std::mutex> lock(lock_);
        size_t filled = 0;
        for (Port& port : ports_) {
            if (!port.open) continue;
            const size_t n = std::min(frames, port.count);
            for (size_t i = 0; i < n; ++i) {
                const size_t at = (port.head + i) % ring_frames;
                out[2*i] += port.ring[2*at]; out[2*i+1] += port.ring[2*at+1];
            }
            port.head = (port.head + n) % ring_frames; port.count -= n;
            filled = std::max(filled, n);
        }
        for (size_t i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        if (filled < frames) stats_.silent += frames - filled;
        return filled;
    }
    Stats stats() {
        std::lock_guard<std::mutex> lock(lock_);
        Stats copy = stats_;
        for (const Port& port : ports_) copy.ports += port.open ? 1 : 0;
        return copy;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(lock_);
        initialized_ = false;
        for (Port& port : ports_) port = Port{};
        stats_ = {};
    }
private:
    struct Port {
        bool open = false;
        uint32_t length = 0, format = 0;
        std::vector<float> ring; // ring_frames stereo frames once open.
        size_t head = 0, count = 0;
        std::chrono::steady_clock::time_point due{};
    };
    Port* find(int32_t handle) {
        return handle >= 1 && uint32_t(handle) <= port_limit && ports_[handle - 1].open ? &ports_[handle - 1] : nullptr;
    }
    void push(Port& port, const std::vector<float>& stereo) {
        const size_t frames = stereo.size() / 2;
        for (size_t i = 0; i < frames; ++i) {
            // Full (a reader slower than the game, or none): the oldest half
            // goes at once. Dropping a frame per frame written cut every buffer
            // the game sent (CoD: 432 790 frames, a click every 5 ms); this is
            // one skip, then room for 125 ms of drift.
            if (port.count == ring_frames) {
                const size_t drop = ring_frames / 2;
                port.head = (port.head + drop) % ring_frames; port.count -= drop; stats_.dropped += drop;
            }
            const size_t at = (port.head + port.count) % ring_frames;
            port.ring[2*at] = stereo[2*i]; port.ring[2*at+1] = stereo[2*i+1];
            ++port.count;
            if (stereo[2*i] != 0.0f || stereo[2*i+1] != 0.0f) ++stats_.audible;
        }
        stats_.written += frames;
    }
    PS4GuestMemory& memory_;
    PS4Threads& threads_;
    std::mutex lock_;
    bool initialized_ = false;
    std::array<Port, port_limit> ports_{};
    Stats stats_{};
};
}
