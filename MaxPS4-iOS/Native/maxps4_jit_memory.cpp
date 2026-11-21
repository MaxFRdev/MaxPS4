#include "maxps4_jit_memory.h"
#include <cerrno>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <os/log.h>
#else
#define TARGET_OS_IPHONE 0
#endif
#if TARGET_OS_IPHONE || defined(MAXPS4_TEST_DARWIN_JIT_POOL)
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <libkern/OSCacheControl.h>
#include <array>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ucontext.h>

namespace {
// StikDebug publishes nonce LAST while the task is stopped. Volatile reads are
// deliberate: writes come from an authorized external debugger, not C++ threads.
struct alignas(8) Mailbox {
    volatile uint64_t nonce = 0;
    volatile uint64_t status = 0;
    volatile uint64_t rx[3] = {};
};
static_assert(sizeof(Mailbox) == 40);
Mailbox mailbox;
struct Region { uintptr_t rw = 0, rx = 0; size_t size = 0; bool leased = false; };
std::array<Region, 3> regions {};
std::mutex lock;
// The thread inside `lock`, for maxps4_jit_held(): a signal handler that
// interrupted it, or a thread that suspended it, must not wait for `lock`.
std::atomic<pthread_t> owner {};
struct Held {
    Held() { lock.lock(); owner.store(::pthread_self(), std::memory_order_relaxed); }
    ~Held() { owner.store(pthread_t{}, std::memory_order_relaxed); lock.unlock(); }
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
};
uint64_t expected_nonce = 0;
int state = 0, failure = 0;
int failure_stage = 0, failure_mach = 0, failure_region = -1;
int active_region = -1;
size_t page_size = 0;

bool protection(uintptr_t address, size_t bytes, vm_prot_t required) {
    // vm_region_64 describes one VM entry, not necessarily the entire RX pool.
    // Verify every contiguous subregion without relaxing W^X or accepting gaps.
    if (!bytes || address > UINTPTR_MAX - bytes) return false;
    const uintptr_t end = address + bytes;
    uintptr_t cursor = address;
    while (cursor < end) {
        vm_address_t base = static_cast<vm_address_t>(cursor);
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info {};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        const auto kr = vm_region_64(mach_task_self(), &base, &size,
            VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || base > cursor || size == 0 ||
            base > UINTPTR_MAX - size || base + size <= cursor ||
            info.protection != required) return false;
        cursor = base + size;
    }
    return true;
}
void destroy_regions() {
    for (auto& region : regions) {
        if (region.rw && vm_deallocate(mach_task_self(), region.rw, region.size) != KERN_SUCCESS) std::abort();
        if (region.rx && vm_deallocate(mach_task_self(), region.rx, region.size) != KERN_SUCCESS) std::abort();
        region = {};
    }
}
// Recover only a fault IN the tiny native canary on THIS thread. No C++ stack
// frames are longjmp'd, no FEX faults are swallowed, other host handlers chain.
thread_local sigjmp_buf canary_jump;
thread_local volatile sig_atomic_t canary_active = 0;
thread_local uintptr_t canary_begin = 0;
constexpr std::array signals {SIGBUS, SIGSEGV, SIGILL};
std::array<struct sigaction, 3> previous {};
void canary_fault(int signal, siginfo_t* info, void* context) {
    const auto* machine = static_cast<ucontext_t*>(context);
    const uintptr_t pc = machine->uc_mcontext->__ss.__pc;
    if (canary_active && pc >= canary_begin && pc - canary_begin < 8) {
        canary_active = 0;
        siglongjmp(canary_jump, signal);
    }
    for (size_t i = 0; i < signals.size(); ++i) if (signals[i] == signal) {
        const auto& old = previous[i];
        if (old.sa_handler == SIG_IGN) return;
        if (old.sa_handler == SIG_DFL) {
            sigaction(signal, &old, nullptr);
            raise(signal);
        } else if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(signal, info, context);
        else old.sa_handler(signal);
        return;
    }
}
int execute_canary(uintptr_t address) {
    struct sigaction action {};
    action.sa_sigaction = canary_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    size_t installed = 0;
    for (; installed < signals.size(); ++installed) {
        if (sigaction(signals[installed], &action, &previous[installed]) != 0) break;
    }
    int error = installed == signals.size() ? 0 : errno;
    if (!error) {
        canary_begin = address;
        if (sigsetjmp(canary_jump, 1) == 0) {
            canary_active = 1;
            const auto value = reinterpret_cast<uint32_t (*)()>(address)();
            canary_active = 0;
            if (value != 42) error = EIO;
        } else error = EFAULT;
    }
    canary_active = 0;
    while (installed) {
        --installed;
        if (sigaction(signals[installed], &previous[installed], nullptr) != 0) std::abort();
    }
    return error;
}
int reject_stage(int error, const char* stage, kern_return_t kr = KERN_SUCCESS) {
    static constexpr const char* stages[] = {"mailbox-status", "rx-address-validation", "rx-address-overlap", "rx-protection-check", "vm-remap", "rw-current-protection", "rw-max-protection", "rx-max-protection", "alias-protection-check", "code-cache-guard-page", "canary-alias-coherence", "canary-execution"};
    failure_stage = 0;
    for (int i = 0; i < 12; ++i) if (std::strcmp(stage, stages[i]) == 0) { failure_stage = i + 1; break; }
    failure_mach = kr;
    failure_region = active_region;
    os_log_error(OS_LOG_DEFAULT, "MaxPS4 JIT memory rejected at %{public}s: errno=%{public}d kern_return=%{public}d", stage, error, kr);

    destroy_regions();
    state = 3;
    failure = error;
    return -error;
}
void* translate(void* pointer, bool writable) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region : regions) {
        const auto from = writable ? region.rx : region.rw;
        const auto to = writable ? region.rw : region.rx;
        if (region.leased && address >= from && address - from < region.size)
            return reinterpret_cast<void*>(to + address - from);
    }
    return pointer; // Non-JIT data pointers retain their identity, as FEX expects.
}
}
extern "C" int32_t maxps4_jit_begin(uint64_t* address, uint64_t* nonce, uint64_t* page) {
    if (!address || !nonce || !page) return -EINVAL;
    *address = *nonce = *page = 0;
    Held guard;
    if (state == 1 || state == 2) return -EBUSY;
    const long host_page = sysconf(_SC_PAGESIZE);
    if (host_page <= 0 || host_page > 16384 || 16384 % host_page != 0) return -ENOTSUP;
    page_size = static_cast<size_t>(host_page);
    do { arc4random_buf(&expected_nonce, sizeof(expected_nonce)); } while (expected_nonce <= 2);
    mailbox.nonce = 0;
    // Challenge before publication; 1/2 are reserved for final script status.
    mailbox.status = expected_nonce;
    for (auto& address : mailbox.rx) address = 0;
    state = 1; failure = 0;
    failure_stage = 0; failure_mach = 0; failure_region = -1; active_region = -1;
    *address = reinterpret_cast<uintptr_t>(&mailbox);
    *nonce = expected_nonce; *page = page_size;
    return 0;
}
extern "C" int32_t maxps4_jit_prepare() {
    Held guard;
    if (state == 2) return 0;
    if (state == 3) return -failure;
    if (state != 1) return -EACCES;
    if (mailbox.nonce != expected_nonce) return -EAGAIN;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (mailbox.status != 1) return reject_stage(EACCES, "mailbox-status");
    const std::array sizes {size_t{16384}, size_t{128 * 1024 * 1024}, page_size};
    // Claim ALL addresses before attempting remaps, so rollback releases every
    // debugger-owned RX allocation even if the first remap is denied.
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto rx = static_cast<uintptr_t>(mailbox.rx[i]);
        if (!rx || rx % page_size || rx > UINTPTR_MAX - sizes[i]) return reject_stage(EINVAL, "rx-address-validation");
        for (size_t j = 0; j < i; ++j)
            if (rx < regions[j].rx + regions[j].size && regions[j].rx < rx + sizes[i]) return reject_stage(EINVAL, "rx-address-overlap");
        regions[i] = {0, rx, sizes[i], false};
    }
    for (auto& region : regions) {
        active_region = static_cast<int>(&region - regions.data());
        if (!protection(region.rx, region.size, VM_PROT_READ | VM_PROT_EXECUTE)) return reject_stage(EACCES, "rx-protection-check");
        vm_address_t rw = 0;
        vm_prot_t current = 0, maximum = 0;
        auto kr = vm_remap(mach_task_self(), &rw, region.size, 0, VM_FLAGS_ANYWHERE,
            mach_task_self(), region.rx, FALSE, &current, &maximum, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) return reject_stage(EACCES, "vm-remap", kr);
        region.rw = rw;
        // Lower both ceilings: NEVER turn the executable alias writable.
        kr = vm_protect(mach_task_self(), rw, region.size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) return reject_stage(EACCES, "rw-current-protection", kr);
        kr = vm_protect(mach_task_self(), rw, region.size, TRUE, VM_PROT_READ | VM_PROT_WRITE);
        if (kr != KERN_SUCCESS) return reject_stage(EACCES, "rw-max-protection", kr);
        kr = vm_protect(mach_task_self(), region.rx, region.size, TRUE, VM_PROT_READ | VM_PROT_EXECUTE);
        if (kr != KERN_SUCCESS) return reject_stage(EACCES, "rx-max-protection", kr);
        if (!protection(rw, region.size, VM_PROT_READ | VM_PROT_WRITE) ||
            !protection(region.rx, region.size, VM_PROT_READ | VM_PROT_EXECUTE)) return reject_stage(EACCES, "alias-protection-check");
    }
    const auto guard_result = vm_protect(mach_task_self(), regions[1].rw + regions[1].size - page_size,
        page_size, FALSE, VM_PROT_NONE);
    if (guard_result != KERN_SUCCESS) return reject_stage(EACCES, "code-cache-guard-page", guard_result);
    // A native-memory test, NOT the guest/FEX test or evidence of FEX success.
    const uint32_t code[] = {0x52800540U, 0xD65F03C0U}; // mov w0,#42; ret
    auto& canary = regions[2];
    std::memcpy(reinterpret_cast<void*>(canary.rw), code, sizeof(code));
    if (std::memcmp(reinterpret_cast<void*>(canary.rx), code, sizeof(code)) != 0) return reject_stage(EIO, "canary-alias-coherence");
    sys_icache_invalidate(reinterpret_cast<void*>(canary.rw), sizeof(code));
    sys_icache_invalidate(reinterpret_cast<void*>(canary.rx), sizeof(code));
    const int error = execute_canary(canary.rx);
    if (error) return reject_stage(error, "canary-execution");
    // The canary isn't needed by FEX and must not remain allocated.
    if (vm_deallocate(mach_task_self(), canary.rw, canary.size) != KERN_SUCCESS ||
        vm_deallocate(mach_task_self(), canary.rx, canary.size) != KERN_SUCCESS) std::abort();
    canary = {};
    state = 2; failure = 0;
    return 0;
}
extern "C" int32_t maxps4_jit_confirmation() {
    Held guard;
    if (!expected_nonce || mailbox.nonce != expected_nonce) return 0;
    std::atomic_thread_fence(std::memory_order_acquire);
    return mailbox.status == 1 ? 1 : 2;
}
extern "C" int32_t maxps4_jit_launch_failed(uint64_t nonce) {
    Held guard;
    // Never cancel a request another script has already started to publish.
    if (state != 1 || !nonce || nonce != expected_nonce || mailbox.nonce || mailbox.status != expected_nonce)
        return -EBUSY;
    for (const auto address : mailbox.rx) if (address) return -EBUSY;
    state = 3; failure = ENOENT;
    return 0;
}
extern "C" uint32_t maxps4_jit_leases() {
    Held guard;
    uint32_t count = 0;
    for (const auto& region : regions) count += region.leased;
    return count;
}
extern "C" int32_t maxps4_jit_cancel() {
    Held guard;
    // Do not race a live debugger script; restart the app after a broken attach.
    if (state == 1) return -EBUSY;
    for (const auto& region : regions) if (region.leased) return -EBUSY;
    destroy_regions(); expected_nonce = 0; state = failure = 0;
    mailbox.nonce = 0;
    mailbox.status = 0;
    for (auto& address : mailbox.rx) address = 0;
    failure_stage = 0; failure_mach = 0; failure_region = -1; active_region = -1;
    return 0;
}
extern "C" int32_t maxps4_jit_state() { Held guard; return state; }
extern "C" int32_t maxps4_jit_error() { Held guard; return failure; }
extern "C" int32_t maxps4_jit_mailbox_diagnostic() {
    Held guard;
    if (state == 0) return 0;
    if (mailbox.nonce == 0) return 1;
    if (mailbox.nonce != expected_nonce) return 2;
    if (mailbox.status == 0) return 3;
    if (mailbox.status != 1) return 4;
    return 5;
}
extern "C" int32_t maxps4_jit_failure_stage() { Held guard; return failure_stage; }
extern "C" int32_t maxps4_jit_failure_mach() { Held guard; return failure_mach; }
extern "C" int32_t maxps4_jit_failure_region() { Held guard; return failure_region; }
extern "C" void* maxps4_jit_allocate(size_t size) {
    Held guard;
    if (state != 2) { errno = EACCES; return nullptr; }
    for (auto& region : regions) if (region.rw && !region.leased && region.size == size) {
        // The code-cache host-page guard was installed during preparation.
        region.leased = true;
        return reinterpret_cast<void*>(region.rw);
    }
    errno = ENOMEM; // No late debugger allocation after detach; bounded probe only.
    return nullptr;
}
extern "C" int32_t maxps4_jit_free(void* write, size_t size) {
    Held guard;
    for (auto& region : regions) if (region.rw == reinterpret_cast<uintptr_t>(write)) {
        if (!region.leased || region.size != size) return -EINVAL;
        region.leased = false; // Retain prepared pages for the next fixed probe.
        return 1;
    }
    return 0;
}
extern "C" size_t maxps4_jit_usable_size(void* write, size_t size) {
    Held guard;
    const auto& cache = regions[1];
    return cache.leased && cache.rw == reinterpret_cast<uintptr_t>(write) &&
        cache.size == size && size > page_size ? size - page_size : 0;
}
extern "C" void* maxps4_jit_executable(void* pointer) { Held guard; return translate(pointer, false); }
extern "C" void* maxps4_jit_writable(void* pointer) { Held guard; return translate(pointer, true); }
extern "C" int32_t maxps4_jit_held() { return ::pthread_equal(owner.load(std::memory_order_relaxed), ::pthread_self()) ? 1 : 0; }
extern "C" int32_t maxps4_jit_describe(void* pointer, uintptr_t* base, size_t* offset, size_t* size) {
    Held guard;
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region : regions) if (region.leased) {
        for (const auto start : {region.rw, region.rx}) if (address >= start && address - start < region.size) {
            if (base) *base = start;
            if (offset) *offset = address - start;
            if (size) *size = region.size;
            return 1;
        }
    }
    return 0;
}
#else
// The new session API is iOS-only. macOS keeps its existing real MAP_JIT path.
extern "C" int32_t maxps4_jit_begin(uint64_t* address, uint64_t* nonce, uint64_t* page) {
    if (!address || !nonce || !page) return -EINVAL;
    *address = *nonce = *page = 0; return -ENOTSUP;
}
extern "C" int32_t maxps4_jit_prepare() { return -ENOTSUP; }
extern "C" int32_t maxps4_jit_confirmation() { return 0; }
extern "C" int32_t maxps4_jit_launch_failed(uint64_t) { return -ENOTSUP; }
extern "C" int32_t maxps4_jit_cancel() { return 0; }
extern "C" int32_t maxps4_jit_state() { return 0; }
extern "C" int32_t maxps4_jit_error() { return 0; }
extern "C" int32_t maxps4_jit_mailbox_diagnostic() { return 0; }
extern "C" int32_t maxps4_jit_failure_stage() { return 0; }
extern "C" int32_t maxps4_jit_failure_mach() { return 0; }
extern "C" int32_t maxps4_jit_failure_region() { return -1; }
extern "C" uint32_t maxps4_jit_leases() { return 0; }
extern "C" size_t maxps4_jit_usable_size(void*, size_t) { return 0; }
extern "C" int32_t maxps4_jit_held() { return 0; }
#endif
