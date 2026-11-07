#pragma once
#include "maxps4_ps4_threads.h"
#include "maxps4_ps4_videoout_abi.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace maxps4 {
// Bounded, CPU-visible linear VideoOut surface. Submission takes an owned
// snapshot; only a host presentation confirmation advances flip status/events.
// Lock order: VideoOut guard -> PS4Threads guard. No runtime lock in host callbacks.
class PS4VideoOut final {
public:
    static constexpr size_t attribute_size = 40, buffer_limit = 16, group_limit = 4;
    static constexpr uint32_t bgra = 0x80000000U, rgba = 0x80002200U;
    static int64_t error(uint32_t value) { return ps4::signed32(0x80290000U | value); }
    PS4VideoOut(PS4GuestMemory& memory, PS4Threads& events) : memory_(memory), events_(events) {}
    int64_t open(int32_t bus, int32_t index, uint64_t parameter) {
        std::lock_guard<std::mutex> lock(guard_);
        if (bus != 0 || index != 0 || parameter) return error(1);
        if (port_) return error(9);
        if (next_port_ > INT32_MAX) return error(0x17);
        port_ = next_port_++;
        return int64_t(port_);
    }
    int64_t close(uint64_t port) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        clear(); return 0;
    }
    void reset() { std::lock_guard<std::mutex> lock(guard_); clear(); }
    int64_t set_attribute(uint64_t output, uint32_t format, uint32_t tiling, uint32_t aspect,
                          uint32_t width, uint32_t height, uint32_t pitch) {
        uint8_t wire[attribute_size]{};
        const uint32_t values[] = {format, tiling, aspect, width, height, pitch};
        for (size_t i = 0; i < 6; ++i) ps4::put_le(wire, i * 4, values[i], 4);
        return memory_.write(output, wire, sizeof(wire)) ? 0 : error(2);
    }
    int64_t register_buffers(uint64_t port, int32_t start, uint64_t addresses,
                             int32_t count, uint64_t attribute) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        if (start < 0 || count < 1 || count > int32_t(buffer_limit) || start > int32_t(buffer_limit) - count) return error(10);
        uint8_t wire[attribute_size]{}, pointers[buffer_limit * 8]{};
        if (!memory_.read(attribute, wire, sizeof(wire)) || !memory_.read(addresses, pointers, size_t(count) * 8)) return error(2);
        Attribute attr{};
        attr.format = uint32_t(ps4::get_le(wire, 4));
        attr.width = uint32_t(ps4::get_le(wire + 12, 4)); attr.height = uint32_t(ps4::get_le(wire + 16, 4));
        attr.pitch = uint32_t(ps4::get_le(wire + 20, 4));
        if (attr.format != rgba && attr.format != bgra) return error(3);
        // Tiling 0 (tiled, what games use) or 1 (linear). Tiled frames are
        // marked as such, never presented as linear rows.
        const uint64_t tiling = ps4::get_le(wire + 4, 4);
        if (tiling > 1) return error(7);
        attr.tiled = tiling == 0;
        if (ps4::get_le(wire + 8, 4) != 0) return error(8);
        if (!attr.width || !attr.height || attr.width > 1920 || attr.height > 1080) return error(5);
        if (attr.pitch < attr.width || attr.pitch > 2048) return error(4);
        for (size_t i = 24; i < sizeof(wire); ++i) if (wire[i]) return error(0x1a);
        size_t group = 0;
        while (group < groups_.size() && groups_[group].used) ++group;
        if (group == groups_.size()) return error(15);
        for (int32_t i = 0; i < count; ++i) {
            if (buffers_[size_t(start + i)].used) return error(16);
            const uint64_t address = ps4::get_le(pointers + size_t(i) * 8, 8);
            if (!address || (address & 3) || !memory_.permitted(address, attr.bytes(), 4) || memory_.permitted(address, attr.bytes(), 1)) return error(0x13);
        }
        groups_[group] = {true, attr};
        for (int32_t i = 0; i < count; ++i)
            buffers_[size_t(start + i)] = {true, group, ps4::get_le(pointers + size_t(i) * 8, 8)};
        return int64_t(group);
    }
    int64_t unregister_buffers(uint64_t port, int32_t group) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        if (group < 0 || size_t(group) >= groups_.size() || !groups_[size_t(group)].used) return error(10);
        if (frame_.ticket && buffers_[frame_.buffer_index].group == size_t(group)) return error(9);
        for (auto& buffer : buffers_) if (buffer.used && buffer.group == size_t(group)) buffer = {};
        groups_[size_t(group)] = {}; return 0;
    }
    int64_t add_event(uint64_t queue, uint64_t port, uint64_t id, uint64_t user_data, bool remove = false) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        const auto result = remove ? events_.video_event_delete(queue, port, id) : events_.video_event_add(queue, port, id, user_data);
        return result == ps4::ok ? 0 : result == ps4::badf ? error(12) : error(0x100c);
    }
    // `copy`: keep a CPU copy of the buffer for snapshot(). A host that
    // presents the GPU-rendered target through target() passes false: the
    // copy (8 MB at 1080p) would only cost the game thread, under the lock
    // that the host's target(), complete() and vblank() wait on.
    // `replace`: a pending flip the host has not taken yet (target/snapshot)
    // gives way to this newer one, counted as dropped, instead of refusing
    // it. The host polls on its own refresh, which is not the game's vblank,
    // so keeping the older flip lost every other image.
    int64_t submit(uint64_t port, int32_t index, int32_t mode, uint64_t argument, bool copy = true, bool replace = false) {
        submit_stage_since_.store(now(), std::memory_order_relaxed);
        submit_stage_.store(1, std::memory_order_relaxed);
        struct ResetActivity { std::atomic<uint32_t>& stage; ~ResetActivity() { stage.store(0, std::memory_order_relaxed); } } reset{submit_stage_};
        std::unique_lock<std::mutex> lock(guard_);
        submit_stage_since_.store(now(), std::memory_order_relaxed);
        submit_stage_.store(2, std::memory_order_relaxed);
        if (!valid(port)) return error(11);
        if (index < 0 || size_t(index) >= buffers_.size() || !buffers_[size_t(index)].used) return error(10);
        if (mode != 1) return error(6);
        if (frame_.ticket && (!replace || taken_)) return error(0x12);
        if (next_frame_ == UINT64_MAX) return error(0x17);
        const auto& buffer = buffers_[size_t(index)]; const auto& attr = groups_[buffer.group].attribute;
        try {
            std::vector<uint8_t> pixels(copy ? attr.bytes() : 0);
            if (copy && !memory_.read(buffer.address, pixels.data(), pixels.size())) return error(0x13);
            // The replaced flip still owes its event, before this one's.
            if (frame_.ticket) { ++dropped_; ++replaced_; owed_before_.push_back(frame_.flip_argument); }
            frame_ = {next_frame_++, port_, argument, attr.width, attr.height, attr.pitch, attr.format, uint32_t(index), attr.tiled ? 1U : 0U, attr.bytes()};
            pixels_.swap(pixels); taken_ = false;
        } catch (const std::bad_alloc&) { return error(0x100c); }
        return 0;
    }
    // sceVideoOutSetFlipRate: 0, 1 or 2 for 60, 30 or 20 flips per second.
    int64_t set_flip_rate(uint64_t port, int32_t rate) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        if (rate < 0 || rate > 2) return error(0x14);
        flip_rate_ = uint32_t(rate); return 0;
    }
    // A vsync flip happens at a vblank, at most one per 1 + flip rate vblanks:
    // the caller (the game's flip) waits for that vblank, which is what holds
    // a game to 60 images per second when the emulated CPU runs faster. A host
    // that never sent a vblank (tests) is not paced; the wait is bounded so a
    // stopping run is never held for long.
    void pace() {
        std::unique_lock<std::mutex> lock(guard_);
        if (!port_ || !vblank_count_) return;
        const uint64_t due = paced_vblank_ + flip_rate_ + 1;
        wait_paced(lock, [&] { return vblank_count_ >= due; });
        paced_vblank_ = vblank_count_;
    }
    // sceGnmSubmitAndFlipCommandBuffers: the prepareFlip label write and
    // interrupt happen at the vblank the flip is due on, as on the hardware,
    // while the game goes on preparing its next frame (holding the game until
    // that vblank would halve its rate whenever a frame takes over 16 ms).
    // Only a flip whose predecessor still waits holds the caller, bounded,
    // then retires that one at once. False if an immediate label write fails.
    // A game that took under sleep_below_us since its previous flip was
    // retired (or returned, whichever is later: not the time it spent spinning
    // on that flip's label) would only spin on this one's label until the
    // vblank (Sonic Mania: 93 % of its main thread): it sleeps here instead
    // until that vblank retires the flip.
    bool flip_at_vblank(bool label, uint64_t address, uint32_t value, bool interrupt) {
        flip_stage_since_.store(now(), std::memory_order_relaxed);
        flip_stage_.store(1, std::memory_order_relaxed);
        struct ResetActivity { PS4VideoOut& videoout; ~ResetActivity() { videoout.flip_stage_.store(0, std::memory_order_relaxed); } } reset{*this};
        std::unique_lock<std::mutex> lock(guard_);
        flip_stage_since_.store(now(), std::memory_order_relaxed);
        flip_stage_.store(2, std::memory_order_relaxed);
        const uint64_t worked = now() - std::max(flip_returned_, retired_at_);
        bool written = true;
        if (port_ && vblank_count_) {
            flip_stage_since_.store(now(), std::memory_order_relaxed);
            flip_stage_.store(3, std::memory_order_relaxed);
            wait_paced(lock, [&] { return !deferred_.pending; });
            if (deferred_.pending) retire();
            deferred_ = {true, label, interrupt, address, value};
            if (port_ && vblank_count_ < paced_vblank_ + flip_rate_ + 1) {
                if (worked < sleep_below_us) {
                    flip_stage_since_.store(now(), std::memory_order_relaxed);
                    flip_stage_.store(4, std::memory_order_relaxed);
                    wait_paced(lock, [&] { return !deferred_.pending; });
                    if (deferred_.pending) written = retire(); // Bounded wait over.
                }
                flip_returned_ = now(); return written;
            }
        } else deferred_ = {true, label, interrupt, address, value};
        written = retire(); flip_returned_ = now(); return written;
    }
    std::string flip_activity() const {
        const uint32_t stage = flip_stage_.load(std::memory_order_relaxed);
        if (!stage) return {};
        static constexpr const char* stages[] = {"", "attente du verrou VideoOut", "verrou VideoOut", "attente du flip précédent", "attente du vblank"};
        const uint64_t started = flip_stage_since_.load(std::memory_order_relaxed);
        const uint64_t current = now();
        const uint64_t elapsed = current - std::min(current, started);
        char line[128];
        const size_t count = sizeof(stages) / sizeof(stages[0]);
        std::snprintf(line, sizeof line, "Flip VideoOut depuis %.1f s : %s%s\n", double(elapsed) / 1e6,
            stage < count ? stages[stage] : "état inconnu",
            paused_report_.load(std::memory_order_relaxed) ? " (jeu en pause)" : "");
        return line;
    }
    std::string submit_activity() const {
        const uint32_t stage = submit_stage_.load(std::memory_order_relaxed);
        if (!stage) return {};
        const uint64_t started = submit_stage_since_.load(std::memory_order_relaxed);
        const uint64_t current = now();
        const uint64_t elapsed = current - std::min(current, started);
        char line[128];
        std::snprintf(line, sizeof line, "Soumission VideoOut depuis %.1f s : %s\n", double(elapsed) / 1e6,
            stage == 1 ? "attente du verrou" : "validation et publication du buffer");
        return line;
    }
    // What sceVideoOutGetResolutionStatus reports (1920x1080 by default);
    // kept across reset, set by the host before the game runs.
    bool set_output(uint32_t width, uint32_t height) {
        if (!((width == 1920 && height == 1080) || (width == 1280 && height == 720))) return false;
        std::lock_guard<std::mutex> lock(guard_); output_width_ = width; output_height_ = height; return true;
    }
    // A flip refused because another is still pending (error 0x12) and skipped
    // by sceGnmSubmitAndFlipCommandBuffers instead of failing the submission.
    // A skipped flip still gets its flip event, after the pending one's: on
    // the hardware every flip is shown, and Call of Duty counts the frame
    // buffers it may reuse by these events (one lost each froze it for good).
    void count_dropped(uint64_t argument) {
        std::lock_guard<std::mutex> lock(guard_); ++dropped_;
        if (owed_after_.size() < 64) owed_after_.push_back(argument);
    }
    uint64_t dropped() { std::lock_guard<std::mutex> lock(guard_); return dropped_; }
    // Flips the game asked for: accepted (tickets issued) plus skipped.
    uint64_t requested() { std::lock_guard<std::mutex> lock(guard_); return next_frame_ - 1 + dropped_ - replaced_; }
    // Metadata of the pending flip, without copying CPU surface pixels.
    int32_t target(maxps4_videoout_frame& info, uint64_t& address) {
        info={}; address=0;
        std::lock_guard<std::mutex> lock(guard_);
        if (!frame_.ticket) return -EAGAIN;
        info=frame_; address=buffers_[frame_.buffer_index].address; taken_ = true; return 0;
    }
    int32_t snapshot(maxps4_videoout_frame& info, uint8_t* pixels, size_t capacity) {
        info = {};
        std::lock_guard<std::mutex> lock(guard_);
        if (!frame_.ticket) return -EAGAIN;
        if (pixels_.size() != frame_.byte_count) return -ENODATA; // Submitted without a CPU copy.
        if ((!pixels && capacity) || (pixels && capacity < pixels_.size())) return -EINVAL;
        if (pixels) std::memcpy(pixels, pixels_.data(), pixels_.size());
        info = frame_; taken_ = true; return 0;
    }
    int64_t complete(uint64_t ticket, bool presented) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!ticket || ticket != frame_.ticket) return error(1);
        if (presented) {
            current_buffer_ = int32_t(frame_.buffer_index); flip_time_ = now();
            auto flipped = [&](uint64_t argument) {
                ++flip_count_; last_argument_ = argument;
                events_.video_event_notify(port_, 6, argument, flip_time_);
            };
            for (const uint64_t argument : owed_before_) flipped(argument);
            flipped(frame_.flip_argument);
            for (const uint64_t argument : owed_after_) flipped(argument);
        }
        owed_before_.clear(); owed_after_.clear();
        frame_ = {}; pixels_.clear(); taken_ = false; return 0;
    }
    // The host's pause: vblanks stop counting and flips waiting for one keep
    // waiting past the usual bound, so a game paced on them stops where it is.
    // The host must resume before stopping a run.
    void set_paused(bool paused) {
        std::lock_guard<std::mutex> lock(guard_); paused_ = paused; paused_report_ = paused; paced_.notify_all();
    }
    // A stop was requested: no flip waits any more, so the game's thread goes
    // back to guest code where the stop signal lands (a thread kept waiting
    // for vblanks that no longer come would almost never take it).
    void interrupt() {
        std::lock_guard<std::mutex> lock(guard_); stopping_ = true; paced_.notify_all();
    }
    int64_t vblank(uint64_t port) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        if (paused_) return 0;
        ++vblank_count_; vblank_time_ = now(); events_.video_event_notify(port_, 7, 0, vblank_time_);
        if (deferred_.pending && vblank_count_ >= paced_vblank_ + flip_rate_ + 1) retire();
        paced_.notify_all(); return 0;
    }
    // sceVideoOutGetResolutionStatus: a 1080p 59.94 Hz television (shadPS4
    // layout: four s32 sizes, u64 refresh rate 3, float inches, flags, zeroes).
    int64_t resolution(uint64_t port, uint64_t output) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        uint8_t wire[44]{};
        const uint32_t sizes[] = {output_width_, output_height_, output_width_, output_height_};
        for (size_t i = 0; i < 4; ++i) ps4::put_le(wire, i * 4, sizes[i], 4);
        ps4::put_le(wire, 16, 3, 8);
        const float inches = 50.0f;
        std::memcpy(wire + 24, &inches, 4);
        return output && memory_.write(output, wire, sizeof(wire)) ? 0 : error(2);
    }
    int64_t pending(uint64_t port) {
        std::lock_guard<std::mutex> lock(guard_); return valid(port) ? (frame_.ticket ? 1 : 0) : error(11);
    }
    int64_t flip_status(uint64_t port, uint64_t output) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        uint8_t wire[64]{};
        ps4::put_le(wire, 0, flip_count_, 8); ps4::put_le(wire, 8, flip_time_, 8);
        ps4::put_le(wire, 24, last_argument_, 8); ps4::put_le(wire, 52, frame_.ticket ? 1 : 0, 4);
        ps4::put_le(wire, 56, uint32_t(current_buffer_), 4);
        return memory_.write(output, wire, sizeof(wire)) ? 0 : error(2);
    }
    int64_t vblank_status(uint64_t port, uint64_t output) {
        std::lock_guard<std::mutex> lock(guard_);
        if (!valid(port)) return error(11);
        uint8_t wire[40]{};
        ps4::put_le(wire, 0, vblank_count_, 8); ps4::put_le(wire, 8, vblank_time_, 8);
        return memory_.write(output, wire, sizeof(wire)) ? 0 : error(2);
    }
    int64_t event_field(uint64_t address, uint64_t output, uint32_t field) {
        uint8_t event[32]{};
        if (!memory_.read(address, event, sizeof(event))) return error(2);
        const uint64_t id = ps4::get_le(event, 8), data = ps4::get_le(event + 16, 8);
        if (ps4::get_le(event + 8, 2) != 0xfff3 || (id != 6 && id != 7)) return error(13);
        if (field == 0) return id == 6 ? 0 : 1;
        if (field == 2) return int64_t((data >> 12) & 15);
        uint64_t argument = data >> 16;
        if (id == 6 && (data >> 63)) argument |= 0xffff000000000000ULL;
        uint8_t wire[8]; ps4::put_le(wire, 0, argument, 8);
        return memory_.write(output, wire, sizeof(wire)) ? 0 : error(2);
    }
private:
    struct Attribute { uint32_t format = 0, width = 0, height = 0, pitch = 0; bool tiled = false; size_t bytes() const { return size_t(pitch) * height * 4; } };
    struct Group { bool used = false; Attribute attribute; };
    struct Buffer { bool used = false; size_t group = 0; uint64_t address = 0; };
    struct Deferred { bool pending = false, label = false, interrupt = false; uint64_t address = 0; uint32_t value = 0; };
    static constexpr int pace_timeout_ms = 50;
    static constexpr uint64_t sleep_below_us = 8000; // Half a 60 Hz frame.
    PS4GuestMemory& memory_; PS4Threads& events_; std::mutex guard_; std::condition_variable paced_;
    std::array<Group, group_limit> groups_{}; std::array<Buffer, buffer_limit> buffers_{};
    uint64_t port_ = 0, next_port_ = 1, next_frame_ = 1, flip_count_ = 0, vblank_count_ = 0, dropped_ = 0;
    uint64_t last_argument_ = UINT64_MAX, flip_time_ = 0, vblank_time_ = 0, paced_vblank_ = 0;
    uint32_t flip_rate_ = 0;
    int32_t current_buffer_ = -1;
    uint32_t output_width_ = 1920, output_height_ = 1080;
    maxps4_videoout_frame frame_{}; std::vector<uint8_t> pixels_; Deferred deferred_{};
    // Flip arguments of replaced (before frame_) and skipped (after) flips,
    // whose events come with frame_'s.
    std::vector<uint64_t> owed_before_, owed_after_;
    bool taken_ = false; // The host holds frame_'s ticket: it may complete it.
    bool paused_ = false; // Kept across reset: the host's state, not the game's.
    bool stopping_ = false; // Until reset: the run is ending.
    uint64_t replaced_ = 0; // Dropped flips that had a ticket (requested() counts them once).
    uint64_t flip_returned_ = 0, retired_at_ = 0; // When the last flip returned / was retired (now()).
    std::atomic<uint32_t> flip_stage_{0};
    std::atomic<uint64_t> flip_stage_since_{0};
    std::atomic<uint32_t> submit_stage_{0};
    std::atomic<uint64_t> submit_stage_since_{0};
    std::atomic<bool> paused_report_{false};
    bool valid(uint64_t port) const { return port && port == port_; }
    // Bounded wait for `done`, extended while the host has paused the game.
    template <class Done> void wait_paced(std::unique_lock<std::mutex>& lock, Done done) {
        if (stopping_) return;
        do paced_.wait_for(lock, std::chrono::milliseconds(pace_timeout_ms), [&] { return !port_ || stopping_ || done(); });
        while (paused_ && !stopping_ && port_ && !done());
    }
    bool retire() {
        const Deferred flip = deferred_; deferred_ = {}; paced_vblank_ = vblank_count_; paced_.notify_all();
        retired_at_ = now();
        uint8_t value[4]; ps4::put_le(value, 0, flip.value, 4);
        const bool written = !flip.label || memory_.write(flip.address, value, sizeof(value));
        if (flip.interrupt) events_.graphics_interrupt();
        return written;
    }
    static uint64_t now() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void clear() {
        if (port_) events_.video_event_remove_source(port_);
        port_ = 0; frame_ = {}; pixels_.clear(); groups_.fill({}); buffers_.fill({}); deferred_ = {}; taken_ = false;
        owed_before_.clear(); owed_after_.clear();
        flip_count_ = vblank_count_ = flip_time_ = vblank_time_ = dropped_ = paced_vblank_ = replaced_ = flip_returned_ = retired_at_ = 0; stopping_ = false; flip_rate_ = 0;
        last_argument_ = UINT64_MAX; current_buffer_ = -1; paced_.notify_all();
    }
};
}
