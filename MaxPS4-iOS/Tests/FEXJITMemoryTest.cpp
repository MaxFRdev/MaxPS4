// Exercise the SAME pool implementation using real macOS Mach VM mappings.
// No fake FEX symbols, no fake executable allocation. Permission denial is
// tested as a real kernel outcome, not converted into a successful JIT result.
#include "maxps4_jit_memory.h"
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <pthread.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
bool unmapped(uintptr_t address) {
    vm_address_t base = address; vm_size_t size = 0;
    vm_region_basic_info_data_64_t info {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = vm_region_64(mach_task_self(), &base, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    return result != KERN_SUCCESS || base != address;
}
int main() {
    uint64_t address = 99, nonce = 99, page = 99;
    CHECK(maxps4_jit_begin(nullptr, &nonce, &page) == -EINVAL);
    CHECK(maxps4_jit_prepare() == -EACCES);
    CHECK(maxps4_jit_confirmation() == 0);
    CHECK(maxps4_jit_cancel() == 0);
    CHECK(maxps4_jit_begin(&address, &nonce, &page) == 0);
    CHECK(maxps4_jit_launch_failed(nonce ^ 1) == -EBUSY);
    CHECK(maxps4_jit_launch_failed(nonce) == 0);
    CHECK(maxps4_jit_prepare() == -ENOENT);
    CHECK(maxps4_jit_cancel() == 0);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        CHECK(maxps4_jit_begin(&address, &nonce, &page) == 0);
        CHECK(address && nonce && page);
        CHECK(maxps4_jit_state() == 1 && maxps4_jit_leases() == 0);
        CHECK(maxps4_jit_prepare() == -EAGAIN);
        CHECK(maxps4_jit_allocate(16384) == nullptr && errno == EACCES);
        CHECK(maxps4_jit_cancel() == -EBUSY);
        auto* mailbox = reinterpret_cast<volatile uint64_t*>(address);
        mailbox[1] = 2; mailbox[0] = nonce ^ 1;
        CHECK(maxps4_jit_prepare() == -EAGAIN);
        CHECK(maxps4_jit_confirmation() == 0);
        CHECK(maxps4_jit_launch_failed(nonce) == -EBUSY);
        mailbox[0] = nonce;
        CHECK(maxps4_jit_confirmation() == 2);
        CHECK(maxps4_jit_prepare() == -EACCES);
        CHECK(maxps4_jit_state() == 3 && maxps4_jit_error() == EACCES);
        CHECK(maxps4_jit_cancel() == 0);
    }
    CHECK(maxps4_jit_begin(&address, &nonce, &page) == 0);
    const std::array sizes {size_t{16384}, size_t{128 * 1024 * 1024}, static_cast<size_t>(page)};
    std::array<vm_address_t, 3> rx {};
    for (size_t i = 0; i < rx.size(); ++i) {
        CHECK(vm_allocate(mach_task_self(), &rx[i], sizes[i], VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
        CHECK(vm_protect(mach_task_self(), rx[i], sizes[i], FALSE,
            VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS);
    }
    constexpr std::array signal_numbers {SIGILL, SIGSEGV, SIGBUS, SIGTRAP};
    std::array<struct sigaction, signal_numbers.size()> original {};
    for (size_t i = 0; i < signal_numbers.size(); ++i)
        CHECK(sigaction(signal_numbers[i], nullptr, &original[i]) == 0);
    sigset_t mask {}; CHECK(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0);
    auto* mailbox = reinterpret_cast<volatile uint64_t*>(address);
    for (size_t i = 0; i < rx.size(); ++i) mailbox[i + 2] = rx[i];
    mailbox[1] = 1; mailbox[0] = nonce;
    CHECK(maxps4_jit_confirmation() == 1);
    const int prepared = maxps4_jit_prepare();
    if (prepared == 0) {
        CHECK(maxps4_jit_state() == 2);
        void* dispatcher = maxps4_jit_allocate(sizes[0]);
        void* cache = maxps4_jit_allocate(sizes[1]);
        CHECK(dispatcher && cache && maxps4_jit_leases() == 2);
        CHECK(maxps4_jit_usable_size(cache, sizes[1]) == sizes[1] - page);
        CHECK(maxps4_jit_usable_size(cache, sizes[1] - 1) == 0);
        CHECK(maxps4_jit_usable_size(dispatcher, sizes[0]) == 0);
        CHECK(maxps4_jit_allocate(sizes[0]) == nullptr && errno == ENOMEM);
        CHECK(maxps4_jit_allocate(4096) == nullptr && errno == ENOMEM);
        CHECK(maxps4_jit_cancel() == -EBUSY);
        CHECK(maxps4_jit_executable(static_cast<char*>(dispatcher) + 37) == reinterpret_cast<void*>(rx[0] + 37));
        CHECK(maxps4_jit_writable(reinterpret_cast<void*>(rx[0] + 37)) == static_cast<char*>(dispatcher) + 37);
        uintptr_t base = 0; size_t offset = 0, size = 0;
        CHECK(maxps4_jit_describe(reinterpret_cast<void*>(rx[0] + 37), &base, &offset, &size) == 1);
        CHECK(base == rx[0] && offset == 37 && size == sizes[0]);
        CHECK(maxps4_jit_free(dispatcher, 1) == -EINVAL);
        CHECK(maxps4_jit_leases() == 2);
        CHECK(maxps4_jit_free(cache, sizes[1]) == 1);
        CHECK(maxps4_jit_free(dispatcher, sizes[0]) == 1);
        CHECK(maxps4_jit_free(dispatcher, sizes[0]) == -EINVAL);
        CHECK(maxps4_jit_describe(dispatcher, nullptr, nullptr, nullptr) == 0);
        CHECK(maxps4_jit_leases() == 0);
        CHECK(maxps4_jit_allocate(sizes[0]) == dispatcher); // Prepared-region reuse.
        CHECK(maxps4_jit_free(dispatcher, sizes[0]) == 1);
        std::puts("PASS real Mach RW/RX aliases, native canary, lease bounds and reuse (macOS only)");
    } else {
        CHECK(prepared == -EACCES || prepared == -EFAULT);
        CHECK(maxps4_jit_state() == 3 && maxps4_jit_leases() == 0);
        std::printf("PASS actual host kernel rejection and full rollback: status=%d; iOS grant NOT simulated\n", prepared);
    }
    CHECK(maxps4_jit_cancel() == 0);
    CHECK(maxps4_jit_state() == 0 && maxps4_jit_error() == 0);
    CHECK(maxps4_jit_confirmation() == 0);
    for (const auto pointer : rx) CHECK(unmapped(pointer));
    for (size_t i = 0; i < signal_numbers.size(); ++i) {
        struct sigaction action {}; CHECK(sigaction(signal_numbers[i], nullptr, &action) == 0);
        CHECK(action.sa_handler == original[i].sa_handler && action.sa_flags == original[i].sa_flags);
        for (int signal = 1; signal < NSIG; ++signal)
            CHECK(sigismember(&action.sa_mask, signal) == sigismember(&original[i].sa_mask, signal));
    }
    sigset_t after {}; CHECK(pthread_sigmask(SIG_SETMASK, nullptr, &after) == 0);
    for (int signal = 1; signal < NSIG; ++signal) CHECK(sigismember(&mask, signal) == sigismember(&after, signal));
    std::puts("PASS real Mach memory cleanup, initial denial, signal preservation; iPhone NOT tested");
}
