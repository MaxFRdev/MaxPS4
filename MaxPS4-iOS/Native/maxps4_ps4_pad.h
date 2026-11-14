#pragma once
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_abi.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>

namespace maxps4 {
// Virtual standard controllers for local users 1..4 (player = user - 1). The
// first is always logged in (the touch controls and the first controller); a
// further player logs in when the host first reports its controller, and stays
// logged in after a disconnection, as a PS4 user does: its pad reads as
// disconnected and keeps its handle, so a reconnection needs no reopen.
// Host updates never access guest memory or the global FEX execution lock.
// Unsupported sensors stay zero.
class PS4Pad final {
public:
    static constexpr size_t data_size = 120;
    static constexpr size_t players = 4;
    static constexpr uint32_t button_mask = 0x10fffe;
    static int64_t error(uint32_t code) { return ps4::signed32(0x80920000U | code); }
    explicit PS4Pad(PS4GuestMemory& memory) : memory_(memory) { slots_[0].logged_in = true; }
    int64_t initialize() { std::lock_guard<std::mutex> lock(lock_); initialized_ = true; return 0; }
    int64_t open(int32_t user, int32_t type, int32_t index, uint64_t parameters) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!initialized_) return error(5);
        if (user < 0) return ps4::signed32(0x809b0001U);
        Slot* slot = slot_for_user(user);
        if (!slot || !slot->logged_in) return ps4::signed32(0x809b0081U);
        if (type != 0 || index != 0) return error(2);
        if (parameters) {
            uint8_t bytes[8];
            if (!memory_.read(parameters, bytes, sizeof(bytes))) return error(1);
            for (auto byte : bytes) if (byte) return error(1);
        }
        if (slot->handle) return error(4);
        if (next_ == INT32_MAX) return error(0xff);
        slot->handle = next_++; return slot->handle;
    }
    int64_t close(int32_t handle) {
        std::lock_guard<std::mutex> lock(lock_);
        Slot* slot = by_handle(handle);
        if (!slot) return error(3);
        slot->handle = 0; return 0;
    }
    int64_t get_handle(int32_t user, int32_t type, int32_t index) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!initialized_) return error(5);
        const Slot* slot = slot_for_user(user);
        return slot && type == 0 && index == 0 && slot->handle ? slot->handle : error(8);
    }
    int64_t read(int32_t handle, uint64_t output, int32_t count, bool state) {
        std::lock_guard<std::mutex> lock(lock_);
        Slot* found = by_handle(handle);
        if (!found) return error(3);
        Slot& s = *found;
        if (!output || count < 1 || count > 64) return error(1);
        std::array<uint8_t, data_size> wire{};
        // A press released before any read saw it is still reported once: a
        // tap shorter than a frame would otherwise never reach the game.
        ps4::put_le(wire.data(), 0, s.connected ? s.buttons | s.tapped : 0, 4);
        for (size_t i = 0; i < 4; ++i) wire[4+i] = s.connected ? s.axes[i] : 128;
        wire[8] = s.connected ? s.l2 : 0; wire[9] = s.connected ? s.r2 : 0;
        ps4::put_le(wire.data(), 24, 0x3f800000, 4); // identity quaternion W
        wire[76] = s.connected ? 1 : 0;
        const auto now = now_us();
        ps4::put_le(wire.data(), 80, uint64_t(now), 8);
        wire[104] = s.connections;
        if (!memory_.write(output, wire.data(), wire.size())) return error(1);
        s.tapped = s.unread = 0;
        if (s.pressed_at) { const uint64_t delay = uint64_t(now) - s.pressed_at; s.pressed_at = 0; ++s.press_reads; s.press_total += delay; s.press_most = std::max(s.press_most, delay); }
        return state ? 0 : 1;
    }
    int64_t information(int32_t handle, uint64_t output) {
        std::lock_guard<std::mutex> lock(lock_);
        const Slot* slot = by_handle(handle);
        if (!slot) return error(3);
        uint8_t wire[28]{};
        ps4::put_le(wire, 0, 0x3f800000, 4);
        ps4::put_le(wire, 4, 1920, 2); ps4::put_le(wire, 6, 950, 2);
        wire[8] = wire[9] = 1; wire[11] = slot->connections; wire[12] = slot->connected ? 1 : 0;
        return output && memory_.write(output, wire, sizeof(wire)) ? 0 : error(1);
    }
    // Player 1 (user 1): the single-player path, unchanged.
    int32_t update(uint32_t buttons, const std::array<uint8_t, 4>& axes,
                   uint8_t l2, uint8_t r2, bool connected) {
        return update_player(0, buttons, axes, l2, r2, connected);
    }
    // player 0..3 is user 1..4. The first report of a connected controller logs
    // that player in; a disconnected report before that is ignored.
    int32_t update_player(size_t player, uint32_t buttons, const std::array<uint8_t, 4>& axes,
                          uint8_t l2, uint8_t r2, bool connected) {
        if (player >= players || (buttons & ~button_mask)) return -22;
        std::lock_guard<std::mutex> lock(lock_);
        Slot& s = slots_[player];
        if (!s.logged_in) {
            if (!connected) return 0;
            s.logged_in = true;
        }
        if (connected && !s.connected && s.connections != UINT8_MAX) ++s.connections;
        if (!connected) buttons = 0;
        if (buttons & ~s.buttons && !s.pressed_at) s.pressed_at = now_us();
        s.unread |= buttons & ~s.buttons;
        s.tapped |= s.buttons & ~buttons & s.unread; s.unread &= buttons;
        s.connected = connected; s.buttons = buttons;
        s.axes = axes; s.l2 = l2; s.r2 = r2; return 0;
    }
    // Users logged in, for sceUserServiceGetLoginUserIdList (bit p: user p + 1).
    uint32_t logged_in_users() {
        std::lock_guard<std::mutex> lock(lock_);
        uint32_t mask = 0;
        for (size_t p = 0; p < players; ++p) if (slots_[p].logged_in) mask |= 1u << p;
        return mask;
    }
    bool connected(size_t player) {
        std::lock_guard<std::mutex> lock(lock_);
        return player < players && slots_[player].connected;
    }
    // Player 1's press-to-read delays (the latency report).
    void latency(uint64_t& reads, uint64_t& total, uint64_t& most) {
        std::lock_guard<std::mutex> lock(lock_);
        reads = slots_[0].press_reads; total = slots_[0].press_total; most = slots_[0].press_most;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(lock_);
        initialized_ = false;
        for (auto& s : slots_) s = Slot{};
        slots_[0].logged_in = true;
        // next_ survives reset so an old handle cannot name a reopened pad.
    }
private:
    struct Slot {
        bool logged_in = false, connected = false;
        int32_t handle = 0;
        // unread: pressed since the last read; tapped: those released before it.
        uint32_t buttons = 0, unread = 0, tapped = 0;
        // When the oldest unread press came, and how long presses waited for a read.
        uint64_t pressed_at = 0, press_reads = 0, press_total = 0, press_most = 0;
        std::array<uint8_t, 4> axes{{128,128,128,128}};
        uint8_t l2 = 0, r2 = 0, connections = 0;
    };
    Slot* slot_for_user(int32_t user) {
        return user >= 1 && size_t(user) <= players ? &slots_[size_t(user - 1)] : nullptr;
    }
    Slot* by_handle(int32_t handle) {
        if (!initialized_ || !handle) return nullptr;
        for (auto& s : slots_) if (s.handle == handle) return &s;
        return nullptr;
    }
    static uint64_t now_us() {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    PS4GuestMemory& memory_;
    std::mutex lock_;
    bool initialized_ = false;
    int32_t next_ = 1;
    std::array<Slot, players> slots_{};
};
}
