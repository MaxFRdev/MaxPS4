#include "../Native/maxps4_ps4_user_service.h"
#include "GuestELFFixture.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 4096, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4UserService users(memory), other(memory);
    constexpr uint64_t output = 0x10000, parameters = output + 0x80, name = output + 0x100;
    auto write = [&](uint64_t address, uint64_t value, size_t width = 8) {
        uint8_t bytes[8]{}; put_le(bytes, 0, value, width); assert(memory.write(address, bytes, width));
    };
    auto read = [&](uint64_t address, size_t width = 8) {
        uint8_t bytes[8]{}; assert(memory.read(address, bytes, width)); return get_le(bytes, width);
    };
    write(output, 0xa5a5a5a5a5a5a5a5ULL);
    assert(users.initial_user(output) == PS4UserService::error(2));
    assert(users.login_users(output) == PS4UserService::error(2));
    assert(users.event(output) == PS4UserService::error(2));
    assert(users.user_name(1, name, 7) == PS4UserService::error(2));
    assert(users.terminate() == PS4UserService::error(2));
    assert(read(output) == 0xa5a5a5a5a5a5a5a5ULL);
    assert(users.initialize(UINT64_MAX) == PS4UserService::error(5));
    for (uint32_t priority : {0u, 255u, 768u, UINT32_MAX}) {
        write(parameters, priority, 4);
        assert(users.initialize(parameters) == PS4UserService::error(5));
        assert(users.initial_user(output) == PS4UserService::error(2));
    }
    for (uint32_t priority : {256u, 700u, 767u}) {
        write(parameters, priority, 4);
        assert(users.initialize(parameters) == 0);
        assert(users.initialize(0) == PS4UserService::error(3));
        assert(users.terminate() == 0);
    }
    assert(users.initialize(0) == 0); // Null parameters select defaults.
    assert(other.initial_user(output) == PS4UserService::error(2)); // No process-global session.
    assert(users.initial_user(0) == PS4UserService::error(5));
    assert(users.initial_user(0x1000) == PS4UserService::error(5));
    assert(users.initial_user(output) == 0 && read(output, 4) == 1 && read(output + 4, 4) == 0xa5a5a5a5);
    assert(users.login_users(UINT64_MAX - 7) == PS4UserService::error(5));
    write(output + 16, 0x0123456789abcdefULL);
    assert(users.login_users(output) == 0 && read(output, 4) == 1);
    for (size_t i = 1; i < 4; ++i) assert(read(output + i * 4, 4) == UINT32_MAX);
    assert(read(output + 16) == 0x0123456789abcdefULL);
    // Bad event destinations never consume the pending login event.
    assert(users.event(0x1000) == PS4UserService::error(5));
    assert(users.event(UINT64_MAX) == PS4UserService::error(5));
    assert(users.event(output) == 0 && read(output, 4) == 0 && read(output + 4, 4) == 1);
    write(output, 0x1122334455667788ULL);
    assert(users.event(output) == PS4UserService::error(7) && read(output) == 0x1122334455667788ULL);
    uint8_t sentinel[16]; std::memset(sentinel, 0xa5, sizeof(sentinel));
    assert(memory.write(name, sentinel, sizeof(sentinel)));
    assert(users.user_name(-1, name, 7) == PS4UserService::error(5));
    assert(users.user_name(2, name, 7) == PS4UserService::error(9));
    assert(users.user_name(1, 0, 7) == PS4UserService::error(5));
    assert(users.user_name(1, name, 6) == PS4UserService::error(10) && read(name) == 0xa5a5a5a5a5a5a5a5ULL);
    assert(users.user_name(1, 0x1000, 7) == PS4UserService::error(5));
    assert(users.user_name(1, name, UINT64_MAX) == 0); // Size does not cause allocation/overflow.
    uint8_t named[16]{}; assert(memory.read(name, named, sizeof(named)));
    assert(std::memcmp(named, "MaxPS4", 7) == 0 && named[7] == 0xa5);
    assert(users.terminate() == 0 && users.terminate() == PS4UserService::error(2));
    assert(users.event(output) == PS4UserService::error(2));
    assert(users.initialize(0) == 0 && users.event(output) == 0); // Reinitialization creates one fresh event.
    users.reset();
    assert(users.initial_user(output) == PS4UserService::error(2));
    // Initialization is atomic; simultaneous callers cannot both create sessions.
    std::array<int64_t, 2> results{};
    std::thread a([&] { results[0] = users.initialize(0); });
    std::thread b([&] { results[1] = users.initialize(0); });
    a.join(); b.join();
    assert((results[0] == 0 && results[1] == PS4UserService::error(3)) ||
           (results[1] == 0 && results[0] == PS4UserService::error(3)));
    std::thread first([&] { results[0] = users.event(output); });
    std::thread second([&] { results[1] = users.event(output + 16); });
    first.join(); second.join();
    assert((results[0] == 0 && results[1] == PS4UserService::error(7)) ||
           (results[1] == 0 && results[0] == PS4UserService::error(7)));
    users.reset();
    assert(users.initialize(0) == 0 && users.event(output) == 0);
    {
        // Local multiplayer: users 2..4 come from the logged-in pad players.
        PS4UserService local(memory);
        assert(local.initialize(0) == 0);
        assert(local.login_users(output, 0b11) == 0 && read(output, 4) == 1 && read(output + 4, 4) == 2 &&
               read(output + 8, 4) == UINT32_MAX && read(output + 12, 4) == UINT32_MAX);
        assert(local.login_users(output, 0b1010) == 0 && read(output, 4) == 1 && read(output + 4, 4) == 2 &&
               read(output + 8, 4) == 4 && read(output + 12, 4) == UINT32_MAX);    // user 1 always listed
        assert(local.login_users(output) == 0 && read(output, 4) == 1 && read(output + 4, 4) == UINT32_MAX);
        // LOGIN events: user 1, then user 2 once its controller logged it in.
        assert(local.event(output, 1) == 0 && read(output + 4, 4) == 1);
        assert(local.event(output, 1) == PS4UserService::error(7));
        assert(local.event(output, 0b11) == 0 && read(output, 4) == 0 && read(output + 4, 4) == 2);
        assert(local.event(output, 0b11) == PS4UserService::error(7));                 // once per user
        assert(memory.write(name, sentinel, sizeof(sentinel)));
        assert(local.user_name(2, name, 16, 0b11) == 0);
        char second_name[16]{}; assert(memory.read(name, second_name, 9) && std::strcmp(second_name, "MaxPS4 2") == 0);
        assert(local.user_name(2, name, 8, 0b11) == PS4UserService::error(10));
        assert(local.user_name(2, name, 16, 0b1) == PS4UserService::error(9));       // not logged in
        assert(local.user_name(5, name, 16, 0b1111) == PS4UserService::error(9));
        local.reset(); assert(local.initialize(0) == 0 && local.event(output, 0b11) == 0 && read(output + 4, 4) == 1);
        assert(local.event(output, 0b11) == 0 && read(output + 4, 4) == 2);           // re-announced after reset
    }
    std::puts("PASS local UserService: lifecycle, user/list/name/event ABI, invalid memory, per-engine isolation and concurrent consumers");
}
