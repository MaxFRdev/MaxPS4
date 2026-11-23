#include "../Native/maxps4_ps4_videoout.h"
#include "GuestELFFixture.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 65536, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Threads queues(memory);
    PS4VideoOut video(memory, queues);
    constexpr uint64_t slot = 0x10000, name = slot + 8, count = slot + 0x20,
        timeout = slot + 0x28, attribute = slot + 0x80, addresses = slot + 0x100,
        events = slot + 0x200, status = slot + 0x300, pixels = slot + 0x400;
    auto write = [&](uint64_t address, uint64_t value, size_t width = 8) {
        uint8_t wire[8]{}; put_le(wire, 0, value, width); assert(memory.write(address, wire, width));
    };
    auto read = [&](uint64_t address, size_t width = 8) {
        uint8_t wire[8]{}; assert(memory.read(address, wire, width)); return get_le(wire, width);
    };
    const char label[] = "video";
    assert(memory.write(name, label, sizeof(label)));
    assert(queues.equeue_create(slot, name) == ok);
    const uint64_t queue = read(slot);
    write(timeout, 0, 4);
    auto poll = [&] { return queues.equeue_wait(queue, events, 4, count, timeout); };
    assert(video.open(5, 0, 0) == PS4VideoOut::error(1));
    assert(video.open(0, 1, 0) == PS4VideoOut::error(1));
    assert(video.open(0, 0, 1) == PS4VideoOut::error(1));
    const int64_t opened = video.open(0, 0, 0);
    assert(opened > 0);
    const uint64_t port = uint64_t(opened);
    assert(video.open(0, 0, 0) == PS4VideoOut::error(9));
    // Reported output resolution: 1080p by default, 720p on request, nothing else.
    assert(video.resolution(port, status) == 0 && read(status, 4) == 1920 && read(status + 4, 4) == 1080);
    assert(!video.set_output(1280, 1080) && video.set_output(1280, 720));
    assert(video.resolution(port, status) == 0 && read(status, 4) == 1280 && read(status + 4, 4) == 720);
    assert(video.set_output(1920, 1080));
    assert(video.close(port + 1) == PS4VideoOut::error(11));
    assert(video.add_event(queue + 1, port, 6, 55) == PS4VideoOut::error(12));
    assert(video.add_event(queue, port, 6, 55) == 0 && video.add_event(queue, port, 7, 66) == 0);
    assert(queues.equeue_create(slot, name) == ok);
    const uint64_t full_queue = read(slot);
    for (size_t i = 0; i < PS4Threads::event_limit; ++i) assert(queues.user_event_add(full_queue, int32_t(i), true) == ok);
    assert(video.add_event(full_queue, port, 6, 0) == PS4VideoOut::error(0x100c));
    assert(queues.user_event_delete(full_queue, 0) == ok && video.add_event(full_queue, port, 6, 0) == 0);
    assert(video.set_attribute(0x1000, PS4VideoOut::rgba, 1, 0, 3, 2, 4) == PS4VideoOut::error(2));
    assert(video.set_attribute(attribute, PS4VideoOut::rgba, 2, 0, 3, 2, 4) == 0);
    write(addresses, pixels); write(addresses + 8, pixels + 0x100);
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == PS4VideoOut::error(7));
    write(attribute + 4, 1, 4); write(attribute, 0, 4);
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == PS4VideoOut::error(3));
    write(attribute, PS4VideoOut::rgba, 4); write(attribute + 20, 2, 4);
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == PS4VideoOut::error(4));
    write(attribute + 20, 4, 4); write(attribute + 24, 1, 4);
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == PS4VideoOut::error(0x1a));
    write(attribute + 24, 0, 4); write(addresses + 8, UINT64_MAX);
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == PS4VideoOut::error(0x13));
    write(addresses + 8, pixels + 0x100);
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == 0); // Failed batch registered nothing.
    assert(video.register_buffers(port, 0, addresses, 2, attribute) == PS4VideoOut::error(16));
    assert(video.register_buffers(port, 15, addresses, 2, attribute) == PS4VideoOut::error(10));
    for (int32_t group = 1; group < 4; ++group)
        assert(video.register_buffers(port, group + 1, addresses, 1, attribute) == group);
    assert(video.register_buffers(port, 5, addresses, 1, attribute) == PS4VideoOut::error(15));
    assert(video.unregister_buffers(port, 1) == 0);
    assert(video.register_buffers(port, 2, addresses, 1, attribute) == 1);
    uint8_t original[32];
    for (size_t i = 0; i < sizeof(original); ++i) original[i] = uint8_t(i * 7);
    assert(memory.write(pixels, original, sizeof(original)));
    assert(video.submit(port, 99, 1, 0) == PS4VideoOut::error(10));
    assert(video.submit(port, 0, 0, 0) == PS4VideoOut::error(6));
    assert(video.submit(port, 0, 1, uint64_t(int64_t(-123))) == 0);
    assert(video.pending(port) == 1 && video.flip_status(port, status) == 0);
    assert(read(status) == 0 && read(status + 52, 4) == 1 && read(status + 56, 4) == UINT32_MAX);
    assert(video.submit(port, 1, 1, 0) == PS4VideoOut::error(0x12));
    assert(video.dropped() == 0); video.count_dropped(5); assert(video.dropped() == 1);
    // A GNM flip replaces a pending one the host has not taken, once taken not.
    maxps4_videoout_frame replaced{}; uint64_t replaced_address = 0;
    const uint64_t first = (video.target(replaced, replaced_address), replaced.ticket);
    assert(video.complete(first, false) == 0 && video.submit(port, 0, 1, uint64_t(int64_t(-123))) == 0);
    assert(video.submit(port, 1, 1, 0, true, true) == 0 && video.dropped() == 2);
    assert(video.target(replaced, replaced_address) == 0 && replaced.buffer_index == 1);
    assert(video.submit(port, 0, 1, 0, true, true) == PS4VideoOut::error(0x12) && video.dropped() == 2);
    assert(video.complete(replaced.ticket, false) == 0);
    assert(video.submit(port, 0, 1, uint64_t(int64_t(-123))) == 0);
    assert(video.unregister_buffers(port, 0) == PS4VideoOut::error(9));
    maxps4_videoout_frame frame{};
    assert(video.snapshot(frame, nullptr, 0) == 0 && frame.ticket && frame.port == port);
    maxps4_videoout_frame target{}; uint64_t target_address=0;
    assert(video.target(target,target_address)==0 && target.ticket==frame.ticket && target_address==pixels);
    assert(frame.width == 3 && frame.height == 2 && frame.pitch_in_pixels == 4 && frame.byte_count == 32 && frame.pixel_format == PS4VideoOut::rgba);
    const uint64_t cancelled = frame.ticket;
    uint8_t snapshot[32]; std::memset(snapshot, 0xa5, sizeof(snapshot));
    assert(video.snapshot(frame, snapshot, 31) == -EINVAL && frame.ticket == 0 && snapshot[0] == 0xa5);
    write(pixels, UINT64_MAX); // Guest changes must not mutate the submitted snapshot.
    assert(video.snapshot(frame, snapshot, sizeof(snapshot)) == 0 && !std::memcmp(snapshot, original, 32));
    assert(poll() == timedout); // Neither registration, submission nor snapshot means presented.
    assert(video.complete(cancelled, false) == 0 && video.pending(port) == 0 && poll() == timedout);
    assert(video.complete(cancelled, true) == PS4VideoOut::error(1));
    // Without a CPU copy: the target is known, a snapshot has no pixels.
    assert(video.submit(port, 0, 1, 0, false) == 0);
    assert(video.target(target, target_address) == 0 && target.byte_count == 32 && target_address == pixels);
    assert(video.snapshot(frame, nullptr, 0) == -ENODATA);
    assert(video.complete(target.ticket, false) == 0 && poll() == timedout);
    assert(video.submit(port, 0, 1, uint64_t(int64_t(-123))) == 0);
    assert(video.snapshot(frame, snapshot, sizeof(snapshot)) == 0 && snapshot[0] == 0xff);
    const uint64_t presented = frame.ticket;
    assert(presented != cancelled);
    std::atomic<bool> entered{false};
    std::thread waiter([&] {
        entered = true;
        assert(queues.equeue_wait(queue, events, 4, count, 0) == ok);
    });
    while (!entered) std::this_thread::yield();
    assert(video.complete(presented, true) == 0);
    waiter.join();
    assert(read(count, 4) == 1 && read(events) == 6 && read(events + 8, 2) == 0xfff3 && read(events + 24) == 55);
    assert(video.event_field(events, 0, 0) == 0 && video.event_field(events, 0, 2) == 1);
    assert(video.event_field(events, status, 1) == 0 && signed64(read(status)) == -123);
    assert(video.flip_status(port, status) == 0 && read(status) == 1 && read(status + 24) == uint64_t(int64_t(-123)));
    assert(read(status + 52, 4) == 0 && read(status + 56, 4) == 0 && poll() == timedout);
    assert(video.vblank_status(port, status) == 0 && read(status) == 0); // Flip did not invent a vblank.
    for (int i = 0; i < 20; ++i) assert(video.vblank(port) == 0);
    assert(poll() == ok && read(count, 4) == 1 && read(events) == 7 && read(events + 24) == 66);
    assert(video.event_field(events, 0, 0) == 1 && video.event_field(events, 0, 2) == 15);
    assert(video.vblank_status(port, status) == 0 && read(status) == 20 && poll() == timedout);
    // Flips wait for 1 + flip rate vblanks since the previous one: 20 have
    // passed, so this one does not wait; the next needs two more.
    assert(video.set_flip_rate(port, 3) == PS4VideoOut::error(0x14) && video.set_flip_rate(port, 1) == 0);
    video.pace();
    std::atomic<bool> paced{false};
    std::thread pacer([&] { video.pace(); paced = true; });
    assert(video.vblank(port) == 0 && video.vblank(port) == 0);
    pacer.join();
    assert(paced && video.vblank_status(port, status) == 0 && read(status) == 22);
    // A GNM flip's label is written at its vblank, without holding the caller.
    assert(video.set_flip_rate(port, 0) == 0 && video.flip_at_vblank(true, status, 7, false));
    assert(read(status, 4) == 22);
    assert(video.vblank(port) == 0 && read(status, 4) == 7);
    // The second comes right after the first returned: it sleeps until its
    // vblank and returns with its label written (no spin for the game).
    std::atomic<bool> flipped{false};
    std::thread flipper([&] { assert(video.flip_at_vblank(true, status, 8, false)); flipped = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(!flipped && read(status, 4) == 7);
    assert(video.vblank(port) == 0);
    flipper.join();
    assert(flipped && read(status, 4) == 8);
    // Paused: vblanks do not count and a waiting flip outlasts the 50 ms bound.
    video.set_paused(true);
    assert(video.vblank(port) == 0 && video.vblank_status(port, status) == 0 && read(status) == 24);
    std::atomic<bool> resumed{false};
    std::thread held([&] { assert(video.flip_at_vblank(true, status, 9, false)); resumed = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(!resumed && read(status, 4) == 24);
    video.set_paused(false);
    assert(video.vblank(port) == 0);
    held.join();
    assert(resumed && read(status, 4) == 9);
    // A requested stop: even paused, a flip neither sleeps nor waits for a vblank.
    video.set_paused(true); video.interrupt();
    const auto stop_started = std::chrono::steady_clock::now();
    assert(video.flip_at_vblank(true, status, 10, false) && read(status, 4) == 10);
    assert(video.flip_at_vblank(true, status, 11, false) && read(status, 4) == 11);
    assert(std::chrono::steady_clock::now() - stop_started < std::chrono::milliseconds(40));
    video.set_paused(false);
    while (poll() == ok) {}
    {   // Replaced and skipped flips get their events with the shown one, in order.
        assert(video.flip_status(port, status) == 0);
        const uint64_t before = read(status);
        assert(video.submit(port, 0, 1, 31) == 0 && video.submit(port, 1, 1, 32, true, true) == 0);
        maxps4_videoout_frame shown{}; uint64_t shown_address = 0;
        assert(video.target(shown, shown_address) == 0);
        assert(video.submit(port, 0, 1, 33, true, true) == PS4VideoOut::error(0x12));
        video.count_dropped(33);
        assert(poll() == timedout && video.complete(shown.ticket, true) == 0);
        assert(poll() == ok && read(count, 4) == 1 && read(events) == 6 && video.event_field(events, 0, 2) == 3);
        assert(video.event_field(events, status, 1) == 0 && read(status) == 33);
        assert(video.flip_status(port, status) == 0 && read(status) == before + 3 && poll() == timedout);
        // Not shown (a stop): nothing is owed any more.
        assert(video.submit(port, 0, 1, 34) == 0 && video.target(shown, shown_address) == 0);
        video.count_dropped(35);
        assert(video.complete(shown.ticket, false) == 0 && poll() == timedout);
    }
    // Closing removes only VideoOut subscriptions, not user or GNM events.
    assert(queues.user_event_add(queue, 6, true) == ok && queues.user_event_trigger(queue, 6, 77) == ok);
    assert(queues.graphics_event_add(queue, 6, 88) == ok);
    uint64_t gpu_ticket = 0;
    assert(queues.graphics_event_ticket(queue, 6, gpu_ticket) == ok && queues.graphics_event_complete(gpu_ticket) == ok);
    assert(video.submit(port, 0, 1, 44) == 0 && video.snapshot(frame, nullptr, 0) == 0);
    const uint64_t stale = frame.ticket;
    assert(video.close(port) == 0);
    assert(video.snapshot(frame, nullptr, 0) == -EAGAIN && frame.ticket == 0);
    assert(video.target(target,target_address)==-EAGAIN && !target.ticket && !target_address);
    assert(video.complete(stale, true) == PS4VideoOut::error(1));
    assert(poll() == ok && read(count, 4) == 2);
    assert(read(events + 8, 2) != 0xfff3 && read(events + 32 + 8, 2) != 0xfff3);
    const int64_t reopened = video.open(0, 0, 0);
    assert(reopened > 0 && uint64_t(reopened) != port && video.vblank(port) == PS4VideoOut::error(11));
    video.reset();
    assert(video.pending(uint64_t(reopened)) == PS4VideoOut::error(11));
    std::puts("PASS linear VideoOut: checked buffers, owned pixels, explicit presentation/vblank, status, events, cancellation and stale handles");
}
