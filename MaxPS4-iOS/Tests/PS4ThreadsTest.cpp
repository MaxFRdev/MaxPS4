#include "../Native/maxps4_ps4_libkernel.h"
#include "GuestELFFixture.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 0x10000, 160, 8); // 64 KiB: room for mutex_limit mutexes.
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Libkernel kernel(memory);
    auto& threads = kernel.threads();
    using S = PS4KernelService;
    auto call = [&](S service, uint64_t slot = 0x10000, uint64_t thread = 1) {
        return kernel.invoke(service, {slot}, thread);
    };
    const auto second = threads.attach(); assert(second == 2);
    assert(call(S::thread_self) == 1 && call(S::thread_self, 0, second) == int64_t(second));
    assert(call(S::thread_self, 0, 99) == kernel_error(inval));
    assert(call(S::mutex_init, 0x1000) == kernel_error(fault));
    assert(kernel.invoke(S::mutex_init, {0x10000, 1}) == kernel_error(fault)); // Attribute object address.
    assert(kernel.invoke(S::mutex_init, {0x10000, 0, UINT64_MAX}) == kernel_error(fault));
    assert(call(S::mutex_init) == 0 && call(S::mutex_init) == kernel_error(busy));
    uint8_t wire[8]; assert(memory.read(0x10000, wire, sizeof(wire)));
    const auto token = get_le(wire, 8); assert(token >= 0x10000);
    assert(call(S::mutex_trylock) == 0);
    assert(call(S::mutex_trylock) == kernel_error(busy));
    assert(call(S::mutex_trylock, 0x10000, second) == kernel_error(busy));
    assert(call(S::mutex_unlock, 0x10000, second) == kernel_error(perm));
    assert(call(S::mutex_destroy) == kernel_error(busy));
    assert(call(S::mutex_unlock) == 0);
    assert(call(S::mutex_unlock) == kernel_error(perm));
    assert(call(S::mutex_trylock, 0x10000, second) == 0);
    assert(threads.detach(second) == busy);
    assert(call(S::mutex_unlock, 0x10000, second) == 0);
    assert(threads.detach(second) == ok && threads.detach(second) == inval);
    assert(call(S::mutex_destroy) == 0);
    assert(call(S::mutex_destroy) == kernel_error(inval));
    assert(call(S::mutex_trylock) == kernel_error(inval));
    assert(call(S::mutex_trylock, UINT64_MAX) == kernel_error(fault));
    assert(call(S::mutex_init, 0x10001) == kernel_error(fault));
    assert(call(S::mutex_init) == 0);
    assert(memory.read(0x10000, wire, sizeof(wire)) && get_le(wire, 8) > token);
    // Host workers represent externally scheduled guest IDs; this test exercises
    // actual exclusion but does not execute guest thread entry functions.
    std::vector<std::thread> workers;
    std::atomic<uint64_t> total{0};
    uint64_t protected_counter = 0;
    std::vector<uint64_t> ids;
    for (int i = 0; i < 8; ++i) ids.push_back(threads.attach());
    for (auto id : ids) workers.emplace_back([&, id] {
        for (int n = 0; n < 100; ++n) {
            while (true) {
                const auto error = call(S::mutex_trylock, 0x10000, id);
                if (error == 0) break;
                assert(error == kernel_error(busy)); std::this_thread::yield();
            }
            ++protected_counter;
            assert(call(S::mutex_unlock, 0x10000, id) == 0);
            ++total;
        }
    });
    for (auto& worker : workers) worker.join();
    assert(total == 800 && protected_counter == 800);
    for (auto id : ids) assert(threads.detach(id) == ok);
    assert(call(S::mutex_destroy) == 0);
    for (size_t i = 0; i < PS4Threads::mutex_limit; ++i)
        assert(call(S::mutex_init, 0x10000 + i * 8) == 0);
    assert(call(S::mutex_init, 0x10000 + PS4Threads::mutex_limit * 8) == kernel_error(again));
    while (threads.attach()) {}
    assert(threads.attach() == 0);
    uint8_t stale[8]; assert(memory.read(0x10000, stale, 8));
    kernel.reset();
    assert(call(S::mutex_trylock) == kernel_error(inval));
    assert(call(S::thread_self, 0, ids[0]) == kernel_error(inval));
    assert(call(S::mutex_init) == 0);
    assert(memory.write(0x11000, stale, 8));
    assert(call(S::mutex_trylock, 0x11000) == kernel_error(inval));
    // A thread blocked in scePthreadMutexLock holds no libkernel lock: other
    // calls (including the unlock that frees it) go through.
    const auto blocked = threads.attach(); assert(blocked);
    assert(call(S::mutex_lock, 0x10000) == 0 && call(S::mutex_lock, 0x10000) == kernel_error(deadlk));
    std::atomic<bool> locked{false};
    std::thread locker([&] { assert(call(S::mutex_lock, 0x10000, blocked) == 0); locked = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!locked && kernel.invoke(S::process_time, {}) >= 0 && call(S::thread_self) == 1);
    assert(call(S::mutex_unlock) == 0);
    locker.join();
    assert(locked && call(S::mutex_unlock, 0x10000, blocked) == 0 && threads.detach(blocked) == ok);
    assert(resolve_libkernel("upoVrzMHFeE", "libkernel", "libkernel", 1) == S::mutex_trylock);
    assert(resolve_libkernel("9UK1vLZQft4", "libkernel", "libkernel", 1) == S::mutex_lock);
    assert(resolve_libkernel("scePthreadCondWait", "libkernel", "libkernel", 1) == S::cond_wait);
    assert(resolve_libkernel("BmMjYxmew1w", "libkernel", "libkernel", 1) == S::cond_timedwait);
    assert(resolve_libkernel("4qGrR6eoP9Y", "libkernel", "libkernel", 1) == S::thread_detach);
    assert(resolve_libkernel("scePthreadMutexattrSettype", "libkernel", "libkernel", 1) == S::mutexattr_settype);
    assert(resolve_libkernel("UTXzJbWhhTE", "libkernel", "libkernel", 1) == S::attr_set_stack);
    for (const char* unsupported : {"scePthreadCancel", "scePthreadRename", "scePthreadAttrSetstack"})
        assert(resolve_libkernel(unsupported, "libkernel", "libkernel", 1) == S::unresolved);
    // POSIX convention: -1 and the error in the calling thread's errno cell.
    constexpr uint64_t errnos = 0x18000, progname = 0x11a00, guard = 0x11a40, request = 0x11a60;
    assert(kernel.invoke(S::errno_location, {}, 1, true) == -1); // No errno table yet.
    assert(kernel.initialize_data(S::errno_table, errnos, PS4Threads::thread_limit * 8));
    assert(kernel.invoke(S::errno_location, {}, 1, true) == int64_t(errnos));
    const uint8_t badRequest[16] = {0,0,0,0,0,0,0,0, 0,0xca,0x9a,0x3b,0,0,0,0}; // 1e9 ns: invalid.
    assert(memory.write(request, badRequest, sizeof(badRequest)));
    assert(kernel.invoke(S::nanosleep, {request}, 1, false) == kernel_error(inval));
    assert(kernel.invoke(S::nanosleep, {request}, 1, true) == -1);
    uint8_t cell[4]; assert(memory.read(errnos, cell, 4) && get_le(cell, 4) == inval);
    const uint8_t shortRequest[16] = {0,0,0,0,0,0,0,0, 0x40,0x42,0x0f,0,0,0,0,0}; // 1 ms.
    assert(memory.write(request, shortRequest, sizeof(shortRequest)));
    assert(kernel.invoke(S::nanosleep, {request}, 1, true) == 0);
    assert(kernel.invoke(S::page_size, {}) == 16384 && kernel.invoke(S::thread_equal, {5, 5}) == 1);
    // Data objects: __progname points at "eboot.bin", the stack guard is set.
    assert(kernel.initialize_data(S::progname, progname, 64));
    assert(memory.read(progname, wire, 8) && get_le(wire, 8) == progname + 16);
    char name[10]{}; assert(memory.read(progname + 16, name, sizeof(name)) && !std::strcmp(name, "eboot.bin"));
    assert(kernel.initialize_data(S::stack_chk_guard, guard, 8) && !kernel.initialize_data(S::open, guard, 8));
    // Bindings: library, POSIX convention, data and guest code are declared.
    size_t count = 0; const auto* exports = libkernel_exports(count);
    auto binding = [&](const char* nid) { for (size_t i = 0; i < count; ++i) if (!std::strcmp(exports[i].nid, nid)) return &exports[i]; return (const PS4KernelExport*)nullptr; };
    assert(binding("9BcDykPmo1I")->posix && binding("9BcDykPmo1I")->service == S::errno_location);
    assert(binding("6Z83sYWFlA8")->noreturn && binding("Ou3iL1abvng")->service == S::abort_process);
    assert(binding("f7uOxY9mM1U")->data == 8 && binding("14bOACANTBo")->code && binding("14bOACANTBo")->code_size == 0x2b);
    assert(resolve_libkernel("__Error", "libkernel", "libkernel", 1) == S::errno_location);
    assert(resolve_libkernel("9BcDykPmo1I", "libc", "libc", 1) == S::unresolved);
    std::puts("PS4 guest thread identity, mutex and libkernel lock checks passed");
}
