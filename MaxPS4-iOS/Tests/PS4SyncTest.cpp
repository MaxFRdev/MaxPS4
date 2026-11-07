#include "../Native/maxps4_ps4_threads.h"
#include "GuestELFFixture.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

// Blocking guest mutexes and condition variables (PS4Threads). Host workers
// stand for guest threads with their own identities.
using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    using namespace std::chrono_literals;
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 8192, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Threads threads(memory);
    constexpr uint64_t m = 0x10000, adaptive = 0x10008, cv = 0x10010, data = 0x10018;
    uint8_t wire[8]{};
    const uint8_t zero[8]{}, one[8]{1};
    assert(memory.write(m, zero, 8) && memory.write(adaptive, one, 8) && memory.write(cv, zero, 8));
    // Static initializers create the object on first use.
    assert(threads.unlock(m, 1) == perm);
    assert(threads.lock(m, 1) == ok && memory.read(m, wire, 8) && get_le(wire, 8) > 2);
    assert(threads.lock(m, 1) == deadlk && threads.trylock(m, 1) == busy);
    assert(threads.mutex_owner(m) == 1 && threads.mutex_owner(0) == 0); // For reports: who holds it.
    assert(threads.trylock(adaptive, 1) == ok && threads.unlock(adaptive, 1) == ok);
    assert(threads.lock(data + 1, 1) == fault && threads.lock(UINT64_MAX - 7, 1) == fault);
    assert(threads.lock(m, 99) == inval);
    const uint64_t a = threads.attach(), b = threads.attach();
    assert(a && b);
    // A blocked lock returns once the owner unlocks.
    std::atomic<bool> taken{false};
    std::thread waiter([&] { assert(threads.lock(m, a) == ok); taken = true; assert(threads.unlock(m, a) == ok); });
    std::this_thread::sleep_for(20ms);
    assert(!taken);
    assert(threads.unlock(m, 1) == ok);
    waiter.join();
    assert(taken);
    // interrupt() ends blocked locks with EINTR until resume().
    assert(threads.lock(m, 1) == ok);
    std::thread stopped([&] { assert(threads.lock(m, a) == intr); });
    std::this_thread::sleep_for(20ms);
    threads.interrupt();
    stopped.join();
    assert(threads.lock(m, b) == intr);
    threads.resume();
    assert(threads.trylock(m, b) == busy);
    // Condition waits need the mutex; a timed wait times out and retakes it.
    assert(threads.cond_wait(cv, m, a, false, 0) == perm);
    assert(threads.cond_wait(cv, m, 1, true, 1000) == timedout);
    assert(threads.trylock(m, a) == busy && threads.unlock(m, 1) == ok);
    // Signal wakes one waiter, broadcast the rest; each returns holding the mutex.
    int arrived = 0, woken = 0; // Guarded by the guest mutex.
    std::vector<std::thread> waiters;
    for (uint64_t id : {a, b}) waiters.emplace_back([&, id] {
        assert(threads.lock(m, id) == ok);
        ++arrived;
        assert(threads.cond_wait(cv, m, id, false, 0) == ok);
        ++woken;
        assert(threads.unlock(m, id) == ok);
    });
    auto count = [&](int& value) {
        assert(threads.lock(m, 1) == ok);
        const int seen = value;
        assert(threads.unlock(m, 1) == ok);
        return seen;
    };
    while (count(arrived) < 2) std::this_thread::yield();
    assert(threads.cond_signal(cv, false) == ok);
    while (count(woken) < 1) std::this_thread::yield();
    std::this_thread::sleep_for(20ms);
    assert(count(woken) == 1);
    assert(threads.cond_destroy(cv) == busy);
    assert(threads.cond_signal(cv, true) == ok);
    for (auto& thread : waiters) thread.join();
    assert(woken == 2);
    // A signal with nobody waiting is lost.
    assert(threads.cond_signal(cv, false) == ok && threads.lock(m, 1) == ok);
    assert(threads.cond_wait(cv, m, 1, true, 1000) == timedout && threads.unlock(m, 1) == ok);
    // Destroyed objects stay invalid until initialized again.
    assert(threads.cond_destroy(cv) == ok && threads.cond_destroy(cv) == inval);
    assert(threads.cond_signal(cv, false) == inval);
    assert(threads.cond_init(cv, 0, 0) == ok && threads.cond_init(cv, 0, 0) == busy);
    assert(threads.destroy(m) == ok && threads.lock(m, 1) == inval && threads.trylock(m, 1) == inval);
    // Thread attributes: defaults, detach state, stack size; initializing a
    // slot again gives a fresh object.
    constexpr uint64_t attr = 0x10020, mattr = 0x10028, rm = 0x10030, nm = 0x10038, out = 0x10040;
    for (uint64_t slot : {attr, mattr, rm, nm}) assert(memory.write(slot, zero, 8));
    bool detached = true;
    uint64_t stack = 0;
    assert(threads.thread_attributes(0, detached, stack) == ok && !detached && stack == PS4Threads::default_stack);
    assert(threads.attr_set_detach(attr, 1) == inval && threads.thread_attributes(attr, detached, stack) == inval);
    assert(threads.attr_init(attr) == ok && threads.attr_set_detach(attr, 2) == inval);
    assert(threads.attr_set_detach(attr, 1) == ok && threads.attr_set_stack(attr, 4096) == inval);
    assert(threads.attr_set_stack(attr, 512 * 1024) == ok && threads.attr_accept(attr) == ok);
    assert(threads.thread_attributes(attr, detached, stack) == ok && detached && stack == 512 * 1024);
    assert(threads.attr_get_detach(attr, out) == ok && memory.read(out, wire, 4) && get_le(wire, 4) == 1);
    assert(threads.attr_get_stack(attr, out) == ok && memory.read(out, wire, 8) && get_le(wire, 8) == 512 * 1024);
    assert(threads.attr_get_stack(attr, 0) == fault);
    assert(threads.attr_init(attr) == ok);
    assert(threads.thread_attributes(attr, detached, stack) == ok && !detached && stack == PS4Threads::default_stack);
    assert(threads.attr_destroy(attr) == ok && threads.attr_accept(attr) == inval && threads.attr_destroy(attr) == inval);
    // Recursive mutexes count; a condition wait releases every count and
    // restores it.
    assert(threads.mutexattr_init(mattr) == ok);
    assert(threads.mutexattr_settype(mattr, 0) == inval && threads.mutexattr_settype(mattr, 5) == inval);
    assert(threads.mutexattr_settype(mattr, PS4Threads::recursive) == ok && threads.mutexattr_accept(mattr) == ok);
    assert(threads.mutexattr_gettype(mattr, out) == ok && memory.read(out, wire, 4) && get_le(wire, 4) == 2);
    assert(threads.init(rm, mattr, 0) == ok);
    assert(threads.lock(rm, 1) == ok && threads.lock(rm, 1) == ok && threads.trylock(rm, 1) == ok);
    assert(threads.trylock(rm, a) == busy);
    assert(threads.unlock(rm, 1) == ok && threads.unlock(rm, 1) == ok && threads.trylock(rm, a) == busy);
    assert(threads.lock(rm, 1) == ok); // Count 2 again.
    std::thread probe([&] {
        std::this_thread::sleep_for(5ms);
        assert(threads.trylock(rm, a) == ok && threads.unlock(rm, a) == ok);
    });
    assert(threads.cond_wait(cv, rm, 1, true, 50000) == timedout);
    probe.join();
    assert(threads.unlock(rm, 1) == ok && threads.trylock(rm, a) == busy);
    assert(threads.unlock(rm, 1) == ok && threads.unlock(rm, 1) == perm);
    // A normal mutex locked again by its owner blocks until interrupted.
    assert(threads.mutexattr_settype(mattr, PS4Threads::normal) == ok && threads.init(nm, mattr, 0) == ok);
    assert(threads.lock(nm, 1) == ok);
    std::thread unstick([&] { std::this_thread::sleep_for(20ms); threads.interrupt(); });
    assert(threads.lock(nm, 1) == intr);
    unstick.join();
    threads.resume();
    assert(threads.unlock(nm, 1) == ok);
    // The adaptive static initializer gives EDEADLK to its owner.
    assert(threads.lock(adaptive, 1) == ok && threads.lock(adaptive, 1) == deadlk && threads.unlock(adaptive, 1) == ok);
    assert(threads.mutexattr_destroy(mattr) == ok && threads.init(0x10048, mattr, 0) == inval);
    // Read/write locks: readers share, a writer excludes; a static initializer works.
    constexpr uint64_t rw = 0x10050, keyOut = 0x10058;
    assert(memory.write(rw, zero, 8));
    assert(threads.rwlock_lock(rw, 1, false, true) == ok && threads.rwlock_lock(rw, a, false, false) == ok);
    assert(threads.rwlock_lock(rw, b, true, false) == busy);
    std::atomic<bool> wrote{false};
    std::thread writer([&] { assert(threads.rwlock_lock(rw, b, true, true) == ok); wrote = true; assert(threads.rwlock_unlock(rw, b) == ok); });
    std::this_thread::sleep_for(20ms);
    assert(!wrote);
    assert(threads.rwlock_unlock(rw, 1) == ok && threads.rwlock_unlock(rw, a) == ok);
    writer.join();
    assert(wrote && threads.rwlock_lock(rw, a, true, false) == ok && threads.rwlock_lock(rw, a, true, true) == deadlk);
    assert(threads.rwlock_destroy(rw) == busy && threads.rwlock_unlock(rw, a) == ok && threads.rwlock_destroy(rw) == ok);
    assert(threads.rwlock_lock(rw, 1, false, false) == inval && threads.rwlock_init(rw) == ok);
    // Thread-specific keys: a value per thread, 0 until set; a deleted key is invalid.
    assert(threads.key_create(keyOut, 0x1234) == ok && memory.read(keyOut, wire, 4));
    const uint64_t key = get_le(wire, 4);
    assert(threads.get_specific(key, 1) == 0 && threads.set_specific(key, 7, 1) == ok && threads.set_specific(key, 9, a) == ok);
    assert(threads.get_specific(key, 1) == 7 && threads.get_specific(key, a) == 9 && threads.get_specific(key, b) == 0);
    assert(threads.key_delete(key) == ok && threads.key_delete(key) == inval && threads.set_specific(key, 1, 1) == inval);
    assert(threads.key_create(0, 0) == fault);
    // Thread indices (errno cells) and interruptible sleeps.
    assert(threads.index(1) == 0 && threads.index(a) < PS4Threads::thread_limit && threads.index(12345) == PS4Threads::thread_limit);
    assert(threads.sleep(1000) == ok);
    std::thread stopper([&] { std::this_thread::sleep_for(20ms); threads.interrupt(); });
    assert(threads.sleep(10000000) == intr);
    stopper.join();
    threads.resume();
    assert(threads.detach(a) == ok && threads.detach(b) == ok);
    std::puts("PS4 blocking mutex, condition variable and attribute checks passed");
}
