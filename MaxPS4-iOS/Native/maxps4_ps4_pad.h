#pragma once
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_abi.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

namespace maxps4 {
// One virtual standard controller for local user 1. Host updates never access
// guest memory or the global FEX execution lock. Unsupported sensors stay zero.
class PS4Pad final {
public:
    static constexpr size_t data_size = 120;
    static constexpr uint32_t button_mask = 0x10fffe;
    static int64_t error(uint32_t code) { return ps4::signed32(0x80920000U | code); }
    explicit PS4Pad(PS4GuestMemory& memory) : memory_(memory) {}
    int64_t initialize() { std::lock_guard<std::mutex> lock(lock_); initialized_ = true; return 0; }
    int64_t open(int32_t user, int32_t type, int32_t index, uint64_t parameters) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!initialized_) return error(5);
        if (user < 0) return ps4::signed32(0x809b0001U);
        if (user != 1) return ps4::signed32(0x809b0081U);
        if (type != 0 || index != 0) return error(2);
        if (parameters) {
            uint8_t bytes[8];
            if (!memory_.read(parameters, bytes, sizeof(bytes))) return error(1);
            for (auto byte : bytes) if (byte) return error(1);
        }
        if (handle_) return error(4);
        if (next_ == INT32_MAX) return error(0xff);
        handle_ = next_++; return handle_;
    }
    int64_t close(int32_t handle) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!valid(handle)) return error(3);
        handle_ = 0; return 0;
    }
    int64_t get_handle(int32_t user, int32_t type, int32_t index) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!initialized_) return error(5);
        return user == 1 && type == 0 && index == 0 && handle_ ? handle_ : error(8);
    }
    int64_t read(int32_t handle, uint64_t output, int32_t count, bool state) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!valid(handle)) return error(3);
        if (!output || count < 1 || count > 64) return error(1);
        std::array<uint8_t, data_size> wire{};
        // A press released before any read saw it is still reported once: a
        // tap shorter than a frame would otherwise never reach the game.
        ps4::put_le(wire.data(), 0, connected_ ? buttons_ | tapped_ : 0, 4);
        for (size_t i = 0; i < 4; ++i) wire[4+i] = connected_ ? axes_[i] : 128;
        wire[8] = connected_ ? l2_ : 0; wire[9] = connected_ ? r2_ : 0;
        ps4::put_le(wire.data(), 24, 0x3f800000, 4); // identity quaternion W
        wire[76] = connected_ ? 1 : 0;
        const auto now = now_us();
        ps4::put_le(wire.data(), 80, uint64_t(now), 8);
        wire[104] = connections_;
        if (!memory_.write(output, wire.data(), wire.size())) return error(1);
        tapped_ = unread_ = 0;
        if (pressed_at_) { const uint64_t delay = uint64_t(now) - pressed_at_; pressed_at_ = 0; ++press_reads_; press_total_ += delay; press_most_ = std::max(press_most_, delay); }
        return state ? 0 : 1;
    }
    int64_t information(int32_t handle, uint64_t output) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!valid(handle)) return error(3);
        uint8_t wire[28]{};
        ps4::put_le(wire, 0, 0x3f800000, 4);
        ps4::put_le(wire, 4, 1920, 2); ps4::put_le(wire, 6, 950, 2);
        wire[8] = wire[9] = 1; wire[11] = connections_; wire[12] = connected_ ? 1 : 0;
        return output && memory_.write(output, wire, sizeof(wire)) ? 0 : error(1);
    }
    int32_t update(uint32_t buttons, const std::array<uint8_t, 4>& axes,
                   uint8_t l2, uint8_t r2, bool connected) {
        if (buttons & ~button_mask) return -22;
        std::lock_guard<std::mutex> lock(lock_);
        if (connected && !connected_ && connections_ != UINT8_MAX) ++connections_;
        if (!connected) buttons = 0;
        if (buttons & ~buttons_ && !pressed_at_) pressed_at_ = now_us();
        unread_ |= buttons & ~buttons_;
        tapped_ |= buttons_ & ~buttons & unread_; unread_ &= buttons;
        connected_ = connected; buttons_ = buttons;
        axes_ = axes; l2_ = l2; r2_ = r2; return 0;
    }
    void latency(uint64_t& reads, uint64_t& total, uint64_t& most) {
        std::lock_guard<std::mutex> lock(lock_);
        reads = press_reads_; total = press_total_; most = press_most_;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(lock_);
        initialized_ = connected_ = false; handle_ = 0; buttons_ = tapped_ = unread_ = 0;
        pressed_at_ = 0;
        axes_.fill(128); l2_ = r2_ = connections_ = 0;
        // next_ survives reset so an old handle cannot name a reopened pad.
    }
private:
    bool valid(int32_t handle) const { return initialized_ && handle_ && handle_ == handle; }
    static uint64_t now_us() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    PS4GuestMemory& memory_;
    std::mutex lock_;
    bool initialized_ = false, connected_ = false;
    int32_t handle_ = 0, next_ = 1;
    // unread_: pressed since the last read; tapped_: those released before it.
    uint32_t buttons_ = 0, unread_ = 0, tapped_ = 0;
    // When the oldest unread press came, and how long presses waited for a read.
    uint64_t pressed_at_ = 0, press_reads_ = 0, press_total_ = 0, press_most_ = 0;
    std::array<uint8_t, 4> axes_{{128,128,128,128}};
    uint8_t l2_ = 0, r2_ = 0, connections_ = 0;
};
}
