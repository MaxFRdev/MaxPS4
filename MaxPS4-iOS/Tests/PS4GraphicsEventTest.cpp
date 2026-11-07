#include "../Native/maxps4_ps4_threads.h"
#include "GuestELFFixture.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 8192, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Threads threads(memory);
    constexpr uint64_t slot = 0x10000, name = slot + 8, events = slot + 0x100,
                       count = slot + 0x20, timeout = slot + 0x28;
    const char label[] = "graphics";
    assert(memory.write(name, label, sizeof(label)));
    auto read = [&](uint64_t address, size_t width = 8) {
        uint8_t wire[8]{}; assert(memory.read(address, wire, width)); return get_le(wire, width);
    };
    const uint8_t zero[4]{};
    assert(memory.write(timeout, zero, 4));
    assert(threads.equeue_create(slot, name) == ok);
    const uint64_t queue = read(slot);
    uint64_t ticket = 99;
    assert(threads.graphics_event_ticket(queue, 0x40, ticket) == noent && ticket == 0);
    assert(threads.graphics_event_add(queue, 7, 1) == inval);
    assert(threads.graphics_event_add(queue, UINT64_MAX, 1) == inval);
    assert(threads.graphics_event_add(queue + 1, 0x40, 1) == badf);
    assert(threads.graphics_event_complete(0) == inval);
    assert(threads.graphics_event_complete(12345) == noent);
    assert(threads.graphics_event_add(queue, 0x40, 0xfeedface) == ok);
    assert(threads.graphics_event_ticket(queue, 0x40, ticket) == ok && ticket);
    // Registering and capturing are not completion: no event must appear yet.
    assert(threads.equeue_wait(queue, events, 1, count, timeout) == timedout);
    // A user event with the same ident is a different (ident, filter) key.
    assert(threads.user_event_add(queue, 0x40, true) == ok);
    assert(threads.user_event_trigger(queue, 0x40, 123) == ok);
    assert(threads.graphics_event_complete(ticket) == ok);
    assert(threads.equeue_wait(queue, events, 2, count, timeout) == ok && read(count, 4) == 2);
    assert(read(events) == 0x40 && read(events + 8, 2) == 0xfff2); // GraphicsCore = -14.
    assert(read(events + 10, 2) == 0x20 && read(events + 12, 4) == 0);
    assert(read(events + 16) == 0x40 && read(events + 24) == 0xfeedface);
    assert(read(events + 32 + 8, 2) == 0xfff5 && read(events + 32 + 24) == 123);
    assert(threads.equeue_wait(queue, events, 2, count, timeout) == timedout);
    // An invalid output must preserve a completed event for the next valid wait.
    assert(threads.graphics_event_complete(ticket) == ok);
    assert(threads.equeue_wait(queue, 0x1000, 1, count, timeout) == fault);
    assert(threads.equeue_wait(queue, events, 1, count, timeout) == ok);
    // Late completions cannot target a replacement subscription.
    const uint64_t old = ticket;
    assert(threads.graphics_event_add(queue, 0x40, 999) == ok);
    assert(threads.graphics_event_ticket(queue, 0x40, ticket) == ok && ticket != old);
    assert(threads.graphics_event_complete(old) == noent);
    assert(threads.equeue_wait(queue, events, 1, count, timeout) == timedout);
    // Completion from a backend thread wakes an indefinite guest wait.
    std::atomic<bool> entered{false};
    std::thread waiter([&] {
        entered = true;
        assert(threads.equeue_wait(queue, events, 1, count, 0) == ok);
    });
    while (!entered) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(threads.graphics_event_complete(ticket) == ok);
    waiter.join();
    assert(read(events + 24) == 999);
    assert(threads.graphics_event_delete(queue, 0x40) == ok);
    assert(threads.graphics_event_delete(queue, 0x40) == ok);
    assert(threads.graphics_event_complete(ticket) == noent);
    assert(threads.user_event_trigger(queue, 0x40, 77) == ok); // GPU delete leaves user event intact.
    assert(threads.equeue_wait(queue, events, 1, count, timeout) == ok && read(events + 8, 2) == 0xfff5);
    // All seven compute release-memory event IDs retain their own type/data.
    for (uint64_t id = 0; id <= 6; ++id) {
        assert(threads.graphics_event_add(queue, id, 100 + id) == ok);
        assert(threads.graphics_event_ticket(queue, id, ticket) == ok);
        assert(threads.graphics_event_complete(ticket) == ok);
        assert(threads.equeue_wait(queue, events, 1, count, timeout) == ok);
        assert(read(events) == id && read(events + 16) == id && read(events + 24) == 100 + id);
    }
    assert(threads.equeue_create(slot, name) == ok);
    const uint64_t other = read(slot);
    uint64_t other_ticket = 0;
    assert(threads.graphics_event_add(other, 6, 222) == ok);
    assert(threads.graphics_event_ticket(other, 6, other_ticket) == ok && other_ticket != ticket);
    assert(threads.graphics_event_complete(ticket) == ok);
    assert(threads.equeue_wait(other, events, 1, count, timeout) == timedout);
    assert(threads.equeue_delete(queue) == ok);
    assert(threads.graphics_event_complete(ticket) == noent);
    assert(threads.graphics_event_complete(other_ticket) == ok);
    assert(threads.equeue_wait(other, events, 1, count, timeout) == ok && read(events + 24) == 222);
    threads.reset();
    assert(threads.graphics_event_complete(other_ticket) == noent);
    ticket = 99;
    assert(threads.graphics_event_ticket(other, 6, ticket) == badf && ticket == 0);
    {   // Two EOP interrupts before the handler waits are two events, not one.
        assert(memory.write(timeout, zero, 4) && threads.equeue_create(slot, name) == ok);
        const uint64_t eop = read(slot);
        assert(threads.graphics_event_add(eop, 0x40, 5) == ok);
        threads.graphics_interrupt();
        threads.graphics_interrupt();
        assert(threads.equeue_wait(eop, events, 1, count, timeout) == ok && read(count, 4) == 1);
        assert(threads.equeue_wait(eop, events, 1, count, timeout) == ok && read(count, 4) == 1);
        assert(memory.write(timeout, zero, 4) && threads.equeue_wait(eop, events, 1, count, timeout) == timedout);
    }
    std::puts("PASS GNM completion events: typed delivery, backend wakeup, stale tickets, queue isolation and reset");
}
