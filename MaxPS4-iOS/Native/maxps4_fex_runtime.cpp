#include "maxps4_guest_frame.h"
#include "maxps4_fex_runtime.h"
#include "maxps4_guest_stack.h"
#include "maxps4_orbis_v4_execution_gate.h"
#include <algorithm>
#include <map>
#include <iterator>
#include <filesystem>
#include <fstream>
#include "maxps4_jit_memory.h"
#include "maxps4_guest_elf.h"
#include "SelfImage.hpp"
#include "OrbisMetadata.hpp"
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_libkernel.h"
#include "maxps4_ps4_linker.h"
#include "maxps4_ps4_synthetic_elf.h"
#include "PM4GuestSubmission.hpp"
#include "Common/HostFeatures.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/SignalDelegator.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#else
#define TARGET_OS_IPHONE 0
#endif
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <limits>
#include <unistd.h>
#include <cerrno>
#include <mutex>
#include <memory>
#include <new>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif
#else
#include <ucontext.h>
#endif

extern "C" int maxps4_native_guest_x86_run(const uint8_t*, size_t, uint32_t,
                                           uint64_t*) noexcept;
namespace {
// Unaligned TSO/atomic accesses repaired in the SIGBUS handler (signal-safe
// lock-free counters), reported with the thread activity.
std::atomic<uint64_t> alignment_repairs {0};
std::atomic<uint64_t> last_repair_pc {0};
// maxps4_fex_set_tso_emulation: applies to engines opened afterwards.
std::atomic<bool> tso_emulation {true};
class GuestThreads;
// scePthreadCreate/Join/Exit and the entry's exit function for a general run
// (see GuestThreads), defined once GuestThreads is complete.
bool guest_thread_ending(const GuestThreads& threads);
int64_t guest_thread_service(GuestThreads& threads, maxps4::PS4KernelService service,
                             const maxps4::PS4KernelCall& call);
// First exported name of an HLE service, for the boot probe's trace.
const char* service_name(maxps4::PS4KernelService service) {
    size_t count = 0;
    const auto* exports = maxps4::libkernel_exports(count);
    for (size_t i = 0; i < count; ++i) if (exports[i].service == service) return exports[i].name;
    return "?";
}
// Anonymous immutable guest memory, not a PS4 or Linux syscall ABI.
// A syscall has no permitted host effect and returns the real ENOSYS error.
class ProbeAddressSpace final : public FEXCore::HLE::SyscallHandler {
public:
    ProbeAddressSpace() { OSABI = FEXCore::HLE::SyscallOSABI::OS_GENERIC; }
    maxps4::GuestELF* image = nullptr;
    maxps4::PS4DynamicLinker* linker = nullptr;
    maxps4::PS4Libkernel* kernel = nullptr;
    maxps4::PS4GuestMemory* memory = nullptr;
    GuestThreads* threads = nullptr; // General runs only.
    // Boot probe stubs: "<library> <NID>" per missing_import_token index, and
    // how many times each was called (the first call is also reported).
    const std::vector<std::string>* missing_imports = nullptr;
    std::vector<std::atomic<uint32_t>>* missing_reported = nullptr;
    // One stderr line per HLE call (boot probe only): thousands per second,
    // which a game on the iPhone paid on its own thread, through the debugger.
    bool trace = false;
    uint64_t private_stack = 0;
    size_t private_stack_size = 0;
    // Guest threads of a general run call in concurrently.
    std::atomic<uint32_t> calls {0};
    std::atomic<int> callback_error {0};
    std::atomic<bool> exited {false};
    std::atomic<int32_t> exit_status {0};
    // Per guest thread: the HLE call in progress, or the last one and when it
    // returned, so a stalled game shows where each of its threads waits.
    struct Activity {
        std::atomic<uint64_t> thread {0}, since {0}, returned {0}, rsp {0}, rbp {0}, argument {0};
        std::atomic<uint32_t> service {0};
        std::atomic<bool> inside {false};
    };
    std::array<Activity, 32> activity;
    // The last HLE calls, for the stop report (why a game quit): locks, time
    // and thread-specific data are left out, as they would push out the rest.
    struct RecentCall {
        uint32_t service = 0; uint64_t thread = 0, a0 = 0, a1 = 0, a2 = 0, a3 = 0; int64_t result = 0;
        char path[64] = {};
    };
    std::mutex recent_lock;
    std::array<RecentCall, 128> recent {};
    uint64_t recent_next = 0;
    // The last calls that returned a PS4 error, kept apart (same lock): the
    // recent calls are often all audio output by the time a game stops.
    std::array<RecentCall, 64> failed {};
    uint64_t failed_next = 0;
    // The last files the game did open (same lock): where its data really is.
    std::array<RecentCall, 32> opened {};
    uint64_t opened_next = 0;
    static bool error_result(int64_t result) {
        const uint64_t bits = uint64_t(result);
        return (result < 0 && result >= std::numeric_limits<int32_t>::min()) || (bits >> 32 == 0 && (bits & 0xff000000u) == 0x80000000u);
    }
    static bool frequent(maxps4::PS4KernelService s) {
        using S = maxps4::PS4KernelService;
        switch (s) {
        case S::mutex_lock: case S::mutex_unlock: case S::mutex_trylock: case S::cond_wait: case S::cond_timedwait:
        case S::cond_signal: case S::cond_broadcast: case S::rwlock_rdlock: case S::rwlock_wrlock: case S::rwlock_unlock:
        case S::rwlock_tryrdlock: case S::rwlock_trywrlock: case S::clock_gettime: case S::gettimeofday:
        case S::process_counter: case S::process_time: case S::get_specific: case S::set_specific: case S::thread_self:
        case S::yield: case S::errno_location: case S::tls_get_addr:
        // Lock set-up: games create hundreds of locks at start-up.
        case S::mutex_init: case S::mutex_destroy: case S::cond_init: case S::cond_destroy:
        case S::mutexattr_init: case S::mutexattr_destroy: case S::mutexattr_settype: case S::mutexattr_gettype:
        case S::mutexattr_accept: case S::rwlock_init: case S::rwlock_destroy:
            return true;
        default: return false;
        }
    }
    static uint64_t now_ns() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // This guest thread's slot; null once all slots are taken.
    Activity* activity_slot() {
        for (auto& a : activity) if (a.thread.load(std::memory_order_relaxed) == guest_thread) return &a;
        for (auto& a : activity) {
            uint64_t free = 0;
            if (a.thread.compare_exchange_strong(free, guest_thread)) return &a;
        }
        return nullptr;
    }
    // PS4 identity (PS4Threads) of the guest thread on this host thread.
    static inline thread_local uint64_t guest_thread = 1;
    // Static TLS thread pointer of a child guest thread (0: the image's own).
    static inline thread_local uint64_t guest_tls = 0;
    // The stop, for the report: when it was asked, when threads were told to
    // end, and how many ended at a call (the rest stop on a signal).
    std::atomic<uint64_t> stop_asked_ns {0}, stop_told_ns {0};
    std::atomic<uint32_t> stop_exits {0};
    uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame* frame, FEXCore::HLE::SyscallArguments*) override {
        namespace x86 = FEXCore::X86State;
        auto& state = frame->State;
        auto& r = state.gregs;
        maxps4::PS4KernelService service{};
        const uint64_t rsp = r[x86::REG_RSP];
        int64_t result = maxps4::ps4::kernel_error(maxps4::ps4::nosys);
        if (!linker && image && kernel && memory && r[x86::REG_RAX] <= UINT32_MAX &&
            image->hle_gate(state.rip, static_cast<uint32_t>(r[x86::REG_RAX]))) {
            // Orbis path: SYSCALL from a linker-synthesized trampoline. The token is
            // the service; arguments follow SysV (RCX already moved to R10). The
            // seventh (stack) argument is readable for general images only.
            // The posix_token bit selects the POSIX convention (errno, -1).
            const bool posix = (r[x86::REG_RAX] & maxps4::posix_token) != 0;
            const bool pthread = (r[x86::REG_RAX] & maxps4::pthread_token) != 0;
            const auto orbis = static_cast<maxps4::PS4KernelService>(
                r[x86::REG_RAX] & ~uint64_t{maxps4::posix_token | maxps4::pthread_token});
            // Admitted code builds pointers with RIP-relative LEA, i.e. rebased host
            // addresses. libkernel works on logical PS4 addresses and re-checks
            // permissions, so translate any argument that lies in the image arena.
            auto logical = [&](uint64_t value) {
                uint64_t guest = 0;
                return image->guest_address(value, guest) ? guest : value;
            };
            maxps4::PS4KernelCall call{logical(r[x86::REG_RDI]),logical(r[x86::REG_RSI]),
                logical(r[x86::REG_RDX]),logical(r[x86::REG_R10]),logical(r[x86::REG_R8]),
                logical(r[x86::REG_R9]),
                // A general image's stack is guest memory: stack arguments (the
                // seventh, at RSP+8 past the trampoline's return address) are
                // readable. Bounded images keep 0, which makes them fault.
                image->general() ? r[x86::REG_RSP] : 0};
            call.r14 = r[x86::REG_R14];
            call.r15 = r[x86::REG_R15];
            call.r12 = r[x86::REG_R12];
            call.rbp = r[x86::REG_RBP];
            // POSIX pthread_* (see pthread_token): no name arguments, and an
            // absolute CLOCK_REALTIME deadline turned into the microseconds left.
            if (pthread) {
                using S = maxps4::PS4KernelService;
                if (orbis == S::mutex_init || orbis == S::cond_init || orbis == S::rwlock_init) call.rdx = 0;
                if (orbis == S::thread_create) call.r8 = 0;
                if (orbis == S::cond_timedwait) {
                    uint8_t wire[16] {};
                    timespec now {};
                    ::clock_gettime(CLOCK_REALTIME, &now);
                    if (!call.rdx || !memory->read(call.rdx, wire, sizeof(wire))) call.rdx = 0;
                    else {
                        int64_t seconds = 0, nanoseconds = 0;
                        std::memcpy(&seconds, wire, 8); std::memcpy(&nanoseconds, wire + 8, 8);
                        const int64_t left = (seconds - int64_t(now.tv_sec)) * 1000000 + (nanoseconds - int64_t(now.tv_nsec)) / 1000;
                        call.rdx = uint64_t(std::max<int64_t>(left, 0));
                    }
                }
            }
            Activity* slot = activity_slot();
            if (slot) {
                slot->service = static_cast<uint32_t>(orbis); slot->since = now_ns();
                slot->rsp = r[x86::REG_RSP]; slot->rbp = r[x86::REG_RBP]; slot->argument = r[x86::REG_RDI];
                slot->inside = true;
            }
            const uint32_t stub = static_cast<uint32_t>(orbis) - maxps4::missing_import_token;
            if (static_cast<uint32_t>(orbis) >= maxps4::missing_import_token) {
                if (missing_imports && stub < missing_imports->size()) {
                    if ((*missing_reported)[stub].fetch_add(1, std::memory_order_relaxed) == 0)
                        std::fprintf(stderr, "missing import %s\n", (*missing_imports)[stub].c_str());
                    result = 0;
                } else {
                    callback_error = -EPERM;
                }
            } else if (orbis == maxps4::PS4KernelService::tls_get_addr) {
                // __tls_get_addr(&tls_index{module, offset}): the guest may have changed
                // tls_index, so validate it now. The admitted next instruction reads
                // through RAX, so never return an invalid pointer: on failure return the
                // (mapped) TCB and fail the run once FEX returns.
                uint64_t index[2]{};
                const uint8_t* address = !memory->read(call.rdi,index,sizeof(index)) ? nullptr :
                    guest_tls ? image->tls_address(guest_tls,index[0],index[1]) :
                    image->tls_address(index[0],index[1]);
                if (!address) {
                    callback_error = -EFAULT;
                    address = guest_tls ? reinterpret_cast<const uint8_t*>(guest_tls) : image->thread_pointer_host();
                }
                result = static_cast<int64_t>(reinterpret_cast<uintptr_t>(address));
            } else if (orbis == maxps4::PS4KernelService::process_exit || orbis == maxps4::PS4KernelService::abort_process) {
                // The entry's exit function: the status (EDI) becomes RAX, and the
                // HLT that follows this SYSCALL ends the calling thread. The first
                // exit sets the status; from a child thread it also ends the run.
                // exit() is the same; __stack_chk_fail and sceKernelDebugRaiseException
                // end the process with PS4Libkernel::abort_status.
                const bool abort = orbis == maxps4::PS4KernelService::abort_process;
                result = abort ? maxps4::PS4Libkernel::abort_status : static_cast<int32_t>(r[x86::REG_RDI]);
                bool first = false;
                if (exited.compare_exchange_strong(first, true)) exit_status = static_cast<int32_t>(result);
                if (threads) guest_thread_service(*threads,maxps4::PS4KernelService::process_exit,call);
            } else if (orbis == maxps4::PS4KernelService::thread_create ||
                       orbis == maxps4::PS4KernelService::thread_join ||
                       orbis == maxps4::PS4KernelService::thread_exit ||
                       orbis == maxps4::PS4KernelService::thread_detach) {
                // scePthreadExit's trampoline and the thread-return trampoline
                // both end with HLT after their SYSCALL.
                result = threads ? guest_thread_service(*threads,orbis,call) :
                    maxps4::ps4::kernel_error(maxps4::ps4::nosys);
            } else {
                result = kernel->invoke(orbis,call,guest_thread,posix);
            }
            if (pthread) result = maxps4::pthread_result(result);
            if (slot) { slot->since = now_ns(); slot->inside = false; ++slot->returned; }
            if (!frequent(orbis)) {
                RecentCall entry{static_cast<uint32_t>(orbis), guest_thread, call.rdi, call.rsi, call.rdx, call.rcx, result, {}};
                if (orbis == maxps4::PS4KernelService::open || orbis == maxps4::PS4KernelService::stat)
                    (void)memory->read_cstring(call.rdi, entry.path, sizeof(entry.path));
                std::lock_guard<std::mutex> guard(recent_lock);
                recent[recent_next++ % recent.size()] = entry;
                if (error_result(result)) failed[failed_next++ % failed.size()] = entry;
                else if (orbis == maxps4::PS4KernelService::open) opened[opened_next++ % opened.size()] = entry;
            }
            ++calls;
            char path[256] = {};
            if (trace && (orbis == maxps4::PS4KernelService::open || orbis == maxps4::PS4KernelService::stat) &&
                memory->read_cstring(call.rdi, path, sizeof(path)))
                std::fprintf(stderr, "hle path \"%s\"\n", path);
            if (trace && static_cast<uint32_t>(orbis) < maxps4::missing_import_token)
                std::fprintf(stderr, "hle %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%llx rsp 0x%llx -> 0x%llx\n",
                    service_name(orbis), (unsigned long long)call.rdi, (unsigned long long)call.rsi,
                    (unsigned long long)call.rdx, (unsigned long long)call.rcx, (unsigned long long)result,
                    (unsigned long long)rsp, (unsigned long long)r[x86::REG_RSP]);
            r[x86::REG_RAX] = static_cast<uint64_t>(result);
            state.rip += 2;
            // A stop: waits now return at once, so a game loop around one spins
            // almost only in host code, where stop signals retry. FEX resumes after
            // the SYSCALL whatever State.rip says, and the thunk's next instruction
            // is RET: the return address becomes the thread's return trampoline,
            // whose own exit call ends it.
            if (threads && orbis != maxps4::PS4KernelService::thread_exit && orbis != maxps4::PS4KernelService::process_exit &&
                orbis != maxps4::PS4KernelService::abort_process && guest_thread_ending(*threads)) {
                uint8_t back[8];
                const uint64_t exit_at = image->thread_return();
                std::memcpy(back, &exit_at, sizeof back);
                if (memory->write(rsp, back, sizeof back)) stop_exits.fetch_add(1, std::memory_order_relaxed);
            }
            return static_cast<uint64_t>(result);
        }
        if (!image || !linker || !kernel || !memory || r[x86::REG_RAX] > UINT32_MAX ||
            !linker->gate(*image,state.rip,static_cast<uint32_t>(r[x86::REG_RAX]),service)) {
            callback_error = -EPERM;
        } else if (rsp < private_stack || private_stack_size < 16 ||
                   rsp - private_stack > private_stack_size - 16 || (rsp & 7)) {
            callback_error = -EFAULT;
            result = maxps4::ps4::kernel_error(maxps4::ps4::fault);
        } else {
            // Never expose the rebased CPU RSP or CALL return pointer. Copy only
            // the seventh argument from the proven private CPU stack into a
            // logical guest frame consumed by existing sceKernelMmap.
            uint8_t argument[8];
            std::memcpy(argument,reinterpret_cast<const void*>(rsp+8),sizeof(argument));
            const uint64_t guest_rsp = linker->stack_top()-16;
            if (!memory->write(guest_rsp+8,argument,sizeof(argument))) {
                callback_error = -EFAULT;
                result = maxps4::ps4::kernel_error(maxps4::ps4::fault);
            } else {
                const maxps4::PS4KernelCall call{r[x86::REG_RDI],r[x86::REG_RSI],r[x86::REG_RDX],r[x86::REG_R10],
                    r[x86::REG_R8],r[x86::REG_R9],guest_rsp};
                result = kernel->invoke(service,call,1);
                ++calls;
            }
        }
        // OS_GENERIC spills/refills CPU registers, ignores the C++ return value
        // and resumes from State.rip. SYSCALL has already clobbered RCX/R11.
        r[x86::REG_RAX] = static_cast<uint64_t>(result);
        state.rip += 2;
        return static_cast<uint64_t>(result);
    }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(
        FEXCore::Core::InternalThreadState*, uint64_t address) override {
        if (image) for (const auto& segment : image->segments()) {
            if (!(segment.permissions & 1)) continue;
            // FEX receives rebased host addresses only for the restricted
            // admitted program. Resolve executable ranges through the same
            // guest permission contract used by entry dispatch.
            maxps4::PS4GuestMemory memory(*image);
            const size_t executable_bytes = memory.contiguous(
                segment.address, segment.file_size, 5);
            if (!executable_bytes) continue;
            // The executable file extent must be entirely backed by a
            // permission-checked segment. Never advertise a truncated range
            // to FEX as though the complete code extent were mapped.
            if (executable_bytes != segment.file_size) continue;
            const auto* pointer = image->translate(segment.address, executable_bytes, 5);
            if (!pointer) continue;
            const auto begin = reinterpret_cast<uintptr_t>(pointer);
            if (address >= begin && address - begin < executable_bytes) {
                // Verify the exact requested instruction address in the PS4
                // logical address domain, not only the segment's first byte.
                const uint64_t displacement = address - begin;
                if (displacement > UINT64_MAX - segment.address) continue;
                const uint64_t guest_address = segment.address + displacement;
                if (memory.contiguous(guest_address, 1, 5) != 1) continue;
                return {begin, executable_bytes, false};
            }
        }
        return {0, 0, false};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(
        FEXCore::Core::InternalThreadState*, uint64_t) override {
        return std::nullopt; // Anonymous mapping has no executable file section.
    }
};
class Mapping final {
public:
    Mapping(size_t bytes, int protection) : size(bytes), address(
        ::mmap(nullptr, bytes, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) {}
    ~Mapping() {
        if (address != MAP_FAILED && ::munmap(address, size) != 0) {
            std::fprintf(stderr, "FEX probe mapping cleanup failed: errno=%d\n", errno);
            std::abort();
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    size_t size;
    void* address;
};
// Guest fault containment for one ExecuteThread on an ARM64 host. A SIGSEGV,
// SIGBUS, SIGILL or SIGTRAP raised by this thread's JIT code (or by the
// dispatcher's guest-signal stubs) ends the run: the host stack is reset to
// the dispatcher frame and the PC moved to FEX's thread-stop handler, so
// ExecuteThread returns normally. Any other fault, including one inside FEX
// or HLE C++ code, chains to the previous handler. Under the VIXL simulator
// the JIT code is interpreted by C++, so nothing is contained there.
// Time limit: after `limit_ms`, a watchdog sends SIGURG to the executing thread
// every millisecond until it is stopped the same way, from JIT or dispatcher
// code (signal SIGURG). While the thread is in FEX or HLE C++ code the signal
// chains (SIGURG is ignored by default) and the next one retries. `expire`
// runs once when the limit elapses (it wakes HLE waits such as a join).
// A child guest thread's guard has `install` false: the main run's guard owns
// the process handlers for the whole run, this one only routes its thread.
// Where a fault that kills the app (one no handler takes) is described, set by
// the app for each run: the iOS crash log only shows which thread got the
// signal back, not the host code that faulted.
static char host_crash_note[1024] {};
// Defined by the patched iOS FEX (patch-fexcore-code-cache-reuse.py); absent elsewhere.
extern "C" __attribute__((weak)) uint64_t maxps4_fex_code_cache_used();
extern "C" __attribute__((weak)) uint64_t maxps4_fex_code_cache_reuses();
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
// A return address without its pointer authentication bits.
static uint64_t ptrauth_strip_address(uint64_t address) {
#if defined(__APPLE__)
#if __has_feature(ptrauth_calls)
    return reinterpret_cast<uint64_t>(ptrauth_strip(reinterpret_cast<void*>(address), ptrauth_key_return_address));
#endif
#endif
    return address & 0x0000ffffffffffffULL;
}
// Image and symbol of a host code address, for the note.
static int describe_host_address(char* out, size_t size, uint64_t address) {
    Dl_info info {};
    if (!address || !::dladdr(reinterpret_cast<void*>(address), &info) || !info.dli_fbase)
        return std::snprintf(out, size, " 0x%llx", (unsigned long long)address);
    const char* image = info.dli_fname ? std::strrchr(info.dli_fname, '/') : nullptr;
    image = image ? image + 1 : (info.dli_fname ? info.dli_fname : "?");
    const auto base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    if (info.dli_sname && info.dli_saddr)
        return std::snprintf(out, size, " %s+0x%llx (%s+0x%llx)", image, (unsigned long long)(address - base),
            info.dli_sname, (unsigned long long)(address - reinterpret_cast<uintptr_t>(info.dli_saddr)));
    return std::snprintf(out, size, " %s+0x%llx", image, (unsigned long long)(address - base));
}
// The app is about to die of `number`: write what faulted, its host call stack
// (frame pointers, within this thread's stack) and the guest thread it ran.
static void write_host_crash_note(int number, const siginfo_t* info, const ucontext_t* machine,
                                  uint64_t guest_thread, uint64_t guest_rip) {
    static std::atomic<bool> writing {false};
    if (!host_crash_note[0] || writing.exchange(true)) return;
    const int fd = ::open(host_crash_note, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    static char text[8192];
    size_t used = 0;
    const auto add = [&](int n) { if (n > 0) used = std::min(sizeof text - 1, used + size_t(n)); };
#if defined(__APPLE__)
    const auto& state = machine->uc_mcontext->__ss;
    const uint64_t pc = state.__pc, lr = state.__lr, fp = state.__fp, sp = state.__sp;
    const uint64_t* x = reinterpret_cast<const uint64_t*>(state.__x);
#else
    const auto& state = machine->uc_mcontext;
    const uint64_t pc = state.pc, lr = state.regs[30], fp = state.regs[29], sp = state.sp;
    const uint64_t* x = reinterpret_cast<const uint64_t*>(state.regs);
#endif
    add(std::snprintf(text + used, sizeof text - used,
        "Plantage hôte : signal %d code %d adresse 0x%llx", number, info->si_code,
        (unsigned long long)reinterpret_cast<uintptr_t>(info->si_addr)));
    add(std::snprintf(text + used, sizeof text - used, ", thread du jeu %llu, RIP invité 0x%llx\nPC",
        (unsigned long long)guest_thread, (unsigned long long)guest_rip));
    add(describe_host_address(text + used, sizeof text - used, pc));
    add(std::snprintf(text + used, sizeof text - used, "\n" "LR"));
    add(describe_host_address(text + used, sizeof text - used, ptrauth_strip_address(lr)));
    add(std::snprintf(text + used, sizeof text - used, "\n" "Pile :"));
#if defined(__APPLE__)
    const auto high = reinterpret_cast<uintptr_t>(::pthread_get_stackaddr_np(::pthread_self()));
    const auto low = high - ::pthread_get_stacksize_np(::pthread_self());
#else
    const uintptr_t low = sp, high = sp + 0x100000;
#endif
    uint64_t frame = fp;
    for (int i = 0; i < 24 && frame && !(frame & 7) && frame >= low && frame + 16 <= high; ++i) {
        const auto* words = reinterpret_cast<const uint64_t*>(frame);
        add(std::snprintf(text + used, sizeof text - used, "\n" "  "));
        add(describe_host_address(text + used, sizeof text - used, ptrauth_strip_address(words[1])));
        if (words[0] <= frame) break;
        frame = words[0];
    }
    add(std::snprintf(text + used, sizeof text - used, "\n" "Registres :"));
    for (int i = 0; i < 29; ++i)
        add(std::snprintf(text + used, sizeof text - used, " x%d 0x%llx", i, (unsigned long long)x[i]));
    add(std::snprintf(text + used, sizeof text - used, " sp 0x%llx\n", (unsigned long long)sp));
    for (size_t done = 0; done < used;) {
        const ssize_t n = ::write(fd, text + done, used - done);
        if (n <= 0) break;
        done += size_t(n);
    }
    ::close(fd);
}
#endif
class GuestFaultGuard final {
public:
    static constexpr bool available =
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        true;
#else
        false;
#endif
    GuestFaultGuard(FEXCore::Context::Context* context, FEXCore::Core::InternalThreadState* thread,
                    const FEXCore::SignalDelegatorConfig& config, uint32_t limit_ms,
                    bool install = true, std::function<void()> expire = {},
                    const std::atomic<bool>* cancelled = nullptr,
                    std::function<bool(uint64_t,size_t,uint32_t)> memory_access = {})
        : context(context), thread(thread), config(config), memory_access(std::move(memory_access)) {
        if (!available) return;
        active = this;
        if (!install) return;
        struct sigaction action {};
        action.sa_sigaction = handle;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        for (; installed < signals.size(); ++installed)
            if (::sigaction(signals[installed], &action, &previous[installed]) != 0) break;
        if ((limit_ms || cancelled) && installed == signals.size()) {
            const pthread_t target = ::pthread_self();
            watchdog = std::thread([this, target, limit_ms, cancelled, expire = std::move(expire)] {
                std::unique_lock lock(watch_lock);
                const auto start = std::chrono::steady_clock::now();
                while (!finished && !(cancelled && cancelled->load()) &&
                       (!limit_ms || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(limit_ms)))
                    watch.wait_for(lock, std::chrono::milliseconds(10), [this] { return finished; });
                if (finished) return;
                if (expire) expire();
                while (!finished && !signal.load()) {
                    ::pthread_kill(target, SIGURG);
                    watch.wait_for(lock, std::chrono::milliseconds(1), [this] { return finished; });
                }
            });
        }
    }
    ~GuestFaultGuard() {
        if (watchdog.joinable()) {
            { std::lock_guard lock(watch_lock); finished = true; }
            watch.notify_all();
            watchdog.join();
        }
        active = nullptr;
        while (installed) {
            --installed;
            if (::sigaction(signals[installed], &previous[installed], nullptr) != 0) std::abort();
        }
    }
    GuestFaultGuard(const GuestFaultGuard&) = delete;
    GuestFaultGuard& operator=(const GuestFaultGuard&) = delete;
    std::atomic<int> signal {0};
    int32_t code = 0;
    uint64_t address = 0, fault_rip = 0;
    // maxps4_fex_engine_sample_main_thread: the next SIGURG seen while
    // `sample_request` is set records where the guarded thread is, then
    // lets it run on. `where`: 1 JIT code, 2 FEX dispatcher, 3 other host code.
    struct Sample { uint64_t pc = 0, lr = 0, rip = 0; uint32_t where = 0; };
    std::atomic<bool> sample_request {false};
    std::atomic<uint32_t> sample_sequence {0};
    Sample sample {};
    const pthread_t host = ::pthread_self();
    // For a sample taken from outside the thread. in_jit and guest_rip take
    // the JIT pool lock: never call them while the thread is suspended, and in
    // its signal handler only when it is not inside that lock (maxps4_jit_held).
    uint64_t stored_rip() const { return thread->CurrentFrame->State.rip; }
    bool in_dispatcher(uint64_t pc) const { return pc >= config.DispatcherBegin && pc < config.DispatcherEnd; }
    bool in_jit(uint64_t pc) const { return context->IsAddressInCodeBuffer(thread, pc); }
    uint64_t guest_rip(uint64_t pc) const { return context->RestoreRIPFromHostPC(thread, pc); }
private:
    struct AlignmentAccess { uint64_t address; size_t bytes; uint32_t permissions; };
    // Operand forms recognized by the pinned FEX unaligned handler. Include
    // its ordinary-load/store backpatch forms for concurrent repair races.
    static std::optional<AlignmentAccess> alignment_access(uint32_t i,const std::array<uint64_t,32>& gprs) {
        size_t bytes=size_t{1}<<(i>>30);
        uint32_t permissions=6; int64_t offset=0;
        const auto is=[i](uint32_t mask,uint32_t opcode) { return (i&mask)==opcode; };
        if (is(0xbfe0fc00,0x0860fc00) || is(0xbfff8000,0x887f8000) || is(0xbfe08000,0x88208000))
            bytes=(i&(1u<<30)) ? 16 : 8; // CASPAL / exclusive pair (read-modify-write)
        else if (is(0x3ffffc00,0x08dffc00) || is(0x3ffffc00,0x38bfc000)) permissions=4; // LDAR / LDAPR
        else if (is(0x3ffffc00,0x089ffc00)) permissions=2; // STLR
        else if (is(0x3fe00c00,0x19400000) || is(0x3fe00c00,0x19000000) ||
                 is(0x3be00c00,0x38400000) || is(0x3be00c00,0x38000000)) {
            permissions=(i&(1u<<22)) ? 4 : 2; // LDAPUR / STLUR / patched LDUR / STUR
            offset=(i>>12)&0x1ff; if (offset&0x100) offset-=512;
        } else if (is(0x3ffffc00,0x387f6800) || is(0x3ffffc00,0x383f6800))
            permissions=(i&(1u<<22)) ? 4 : 2; // patched LDR / STR, register offset XZR
        else if (!(is(0x3fe0fc00,0x08e0fc00) || is(0x3b200c00,0x38200000) ||
                   is(0x3ffffc00,0x085ffc00) || is(0x3fe0fc00,0x0800fc00))) return std::nullopt;
        return AlignmentAccess{gprs[(i>>5)&31]+offset,bytes,permissions};
    }
    static constexpr std::array<int, 5> signals {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGURG};
    static inline std::array<struct sigaction, 5> previous {};
    static inline thread_local GuestFaultGuard* active = nullptr;
    static void handle(int number, siginfo_t* info, void* raw) {
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        auto* machine = static_cast<ucontext_t*>(raw);
#if defined(__APPLE__)
        auto& pc = machine->uc_mcontext->__ss.__pc;
        auto& sp = machine->uc_mcontext->__ss.__sp;
#else
        auto& pc = machine->uc_mcontext.pc;
        auto& sp = machine->uc_mcontext.sp;
#endif
        auto* guard = active;
        if (number == SIGURG && guard && guard->sample_request.exchange(false)) {
            // Inside the JIT pool lock the thread runs host code; asking FEX
            // would wait for that lock on this very thread.
            const bool jit = !maxps4_jit_held() && guard->context->IsAddressInCodeBuffer(guard->thread, pc);
            const bool dispatcher = pc >= guard->config.DispatcherBegin && pc < guard->config.DispatcherEnd;
#if defined(__APPLE__)
            guard->sample.lr = machine->uc_mcontext->__ss.__lr;
#else
            guard->sample.lr = machine->uc_mcontext.regs[30];
#endif
            guard->sample.pc = pc;
            guard->sample.where = jit ? 1 : dispatcher ? 2 : 3;
            // Outside JIT code, the guest position FEX last stored (block granularity).
            guard->sample.rip = jit ? guard->context->RestoreRIPFromHostPC(guard->thread, pc) :
                guard->thread->CurrentFrame->State.rip;
            guard->sample_sequence.fetch_add(1, std::memory_order_release);
            return;
        }
        if (guard && !guard->signal.load()) {
            const bool in_jit = !maxps4_jit_held() && guard->context->IsAddressInCodeBuffer(guard->thread, pc);
            // x86 permits unaligned accesses. ARM64 TSO loads/stores and locked
            // operations can raise BUS_ADRALN for otherwise valid guest memory.
            // This frontend must route those faults to FEX before containing
            // actual guest faults, as the upstream Linux signal frontend does.
            if (number == SIGBUS && info->si_code == BUS_ADRALN && in_jit) {
                std::array<uint64_t,32> gprs{};
#if defined(__APPLE__)
                auto& saved = machine->uc_mcontext->__ss;
                std::copy(std::begin(saved.__x),std::end(saved.__x),gprs.begin());
                gprs[29]=saved.__fp; gprs[30]=saved.__lr; gprs[31]=saved.__sp;
#else
                auto& saved = machine->uc_mcontext;
                std::copy(std::begin(saved.regs),std::end(saved.regs),gprs.begin());
                gprs[31]=saved.sp;
#endif
                // FEX may backpatch a TSO access with ordered loads/stores.
                // iOS uses distinct RW/RX aliases: patch RW, resume RX, and
                // invalidate the executable alias as well as FEX's RW flush.
                auto* writable=FEXCore::Allocator::GetWritableAddress(reinterpret_cast<void*>(pc));
                const auto access=alignment_access(*static_cast<const uint32_t*>(writable),gprs);
                std::optional<int32_t> repaired;
                // Darwin may report BUS_ADRALN for protected memory too. Never
                // emulate an atomic access or retry a patched store unless the
                // entire guest operand has the required read/write permission.
                if (access && guard->memory_access && guard->memory_access(access->address,access->bytes,access->permissions)) {
#if defined(__APPLE__)
                    FEXCore::Allocator::ScopedJITWriteProtect jit_write;
#endif
                    repaired=FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(guard->thread,
                        FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier,
                        reinterpret_cast<uintptr_t>(writable),gprs.data());
                }
                if (repaired) {
#if defined(__APPLE__)
                    std::copy_n(gprs.begin(),29,std::begin(saved.__x));
                    saved.__fp=gprs[29]; saved.__lr=gprs[30]; saved.__sp=gprs[31];
#else
                    std::copy_n(gprs.begin(),31,std::begin(saved.regs));
                    saved.sp=gprs[31];
#endif
                    if (reinterpret_cast<uintptr_t>(writable)!=pc) {
#if defined(__APPLE__)
                        sys_icache_invalidate(reinterpret_cast<void*>(pc-4),12);
#else
                        __builtin___clear_cache(reinterpret_cast<char*>(pc)-4,reinterpret_cast<char*>(pc)+8);
#endif
                    }
                    pc+=*repaired;
                    alignment_repairs.fetch_add(1, std::memory_order_relaxed);
                    last_repair_pc.store(pc, std::memory_order_relaxed);
                    return;
                }
            }
            // JIT code keeps guest registers in host registers: spill them first.
            // The dispatcher's guest-signal stubs have already spilled.
            uint64_t stop = 0;
            if (in_jit)
                stop = guard->config.ThreadStopHandlerAddressSpillSRA;
            else if (pc >= guard->config.DispatcherBegin && pc < guard->config.DispatcherEnd)
                stop = guard->config.ThreadStopHandlerAddress;
            if (stop) {
                guard->code = number == SIGURG ? 0 : info->si_code;
                guard->address = number == SIGURG ? 0 : reinterpret_cast<uintptr_t>(info->si_addr);
                // The exact guest instruction (FEX's stored RIP is only its block).
                guard->fault_rip = in_jit ? guard->context->RestoreRIPFromHostPC(guard->thread, pc) : 0;
                guard->signal.store(number);
                sp = guard->thread->CurrentFrame->ReturningStackLocation;
                pc = stop;
                return;
            }
        }
#endif
        for (size_t i = 0; i < signals.size(); ++i) if (signals[i] == number) {
            const auto& old = previous[i];
            // SIGURG's default action is to ignore it: keep this handler installed
            // so the watchdog's next signal is still seen.
            if (old.sa_handler == SIG_IGN || (number == SIGURG && old.sa_handler == SIG_DFL)) return;
            if (old.sa_handler == SIG_DFL) {
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
                // The default action ends the app: say where first.
                if (number != SIGURG)
                    // Thread 0: a host thread that runs no guest code (Metal, audio, the app).
                    write_host_crash_note(number, info, static_cast<const ucontext_t*>(raw),
                                          guard ? ProbeAddressSpace::guest_thread : 0,
                                          guard ? guard->thread->CurrentFrame->State.rip : 0);
#endif
                ::sigaction(number, &old, nullptr);
                ::raise(number);
            } else if (old.sa_flags & SA_SIGINFO) old.sa_sigaction(number, info, raw);
            else old.sa_handler(number);
            return;
        }
    }
    FEXCore::Context::Context* context;
    FEXCore::Core::InternalThreadState* thread;
    FEXCore::SignalDelegatorConfig config;
    std::function<bool(uint64_t,size_t,uint32_t)> memory_access;
    size_t installed = 0;
    std::thread watchdog;
    std::mutex watch_lock;
    std::condition_variable watch;
    bool finished = false;
};
// Initial FEX state of one guest thread (main or child): zero integer
// registers, RIP/RSP, FS base, a guarded CALL/RET stack and a GDT holding one
// 64-bit user code segment. Owns that host memory until the thread is gone.
class FEXThreadSetup final {
public:
    static constexpr size_t callret_size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    explicit FEXThreadSetup(size_t page) : page(page), callret(callret_size + 2 * page, PROT_NONE) {}
    bool mapped() const { return callret.address != MAP_FAILED; }
    int prepare(FEXCore::Core::InternalThreadState* thread, uint64_t rip, uint64_t rsp, uint64_t fs) {
        if (!mapped()) return -ENOMEM;
        auto* base = static_cast<uint8_t*>(callret.address) + page;
        if (::mprotect(base, callret_size, PROT_READ | PROT_WRITE) != 0) return -errno;
        auto& state = thread->CurrentFrame->State;
        std::fill(std::begin(state.gregs), std::end(state.gregs), uint64_t{});
        state.gregs[FEXCore::X86State::REG_RSP] = rsp;
        state.rip = rip;
        if (fs) state.fs_cached = fs;
        thread->CallRetStackBase = base;
        state.callret_sp = reinterpret_cast<uintptr_t>(base) + callret_size / 4;
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = gdt.data();
        state.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = gdt.data();
        state.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
        auto* segment = FEXCore::Core::CPUState::GetSegmentFromIndex(state, state.cs_idx);
        FEXCore::Core::CPUState::SetGDTBase(segment, 0);
        FEXCore::Core::CPUState::SetGDTLimit(segment, 0xF'FFFFU);
        segment->L = 1;
        segment->D = 0;
        state.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*segment);
        return 0;
    }
private:
    size_t page;
    Mapping callret;
    std::array<FEXCore::Core::CPUState::gdt_segment, 32> gdt {};
};
// Child guest threads of one general run (scePthreadCreate). Each runs on its
// own host thread with its own FEX thread state: RIP = entry, RDI = arg, RSP =
// its GuestELF slot's stack top - 8 holding the thread-return trampoline, FS =
// the slot's TCB. Returning from the entry or calling scePthreadExit ends it on
// the trampoline's HLT; scePthreadJoin waits for that and frees the slot.
// The attribute object gives the detach state (a detached thread's slot is
// reused once it ends; scePthreadDetach does the same later) and the stack
// size, which must fit the slot's 1 MiB stack (larger: EAGAIN). Scheduling
// and affinity are not applied. After the main thread's scePthreadExit the run waits for the other
// threads. When the main thread's run ends, every remaining thread is stopped
// (SIGURG, as the watchdog does; without fault containment they are waited
// for) and joined. A contained fault in a child thread, or a child calling the
// entry's exit function, stops the main thread too; a fault becomes the run's
// result.
class GuestThreads final {
public:
    GuestThreads(FEXCore::Context::Context* context, const FEXCore::SignalDelegatorConfig& config,
                 maxps4::GuestELF& image, maxps4::PS4GuestMemory& memory,
                 maxps4::PS4Threads& identities, size_t page)
        : context(context), config(config), image(image), memory(memory), identities(identities),
          page(page), main_thread(::pthread_self()) { identities.resume(); }
    // Locks and waits of a stopping run return EINTR (PS4Threads::interrupt);
    // later runs block normally again.
    ~GuestThreads() { finish(); identities.resume(); }
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;
    // scePthreadCreate(PthreadT* out, attr, entry, arg, name): *out = the new
    // thread's PS4 identity (also what scePthreadSelf returns in it).
    int64_t create(const maxps4::PS4KernelCall& call) {
        using namespace maxps4::ps4;
        if (!call.rdi || !image.translate(call.rdx, 1, 5)) return kernel_error(inval);
        bool detached = false;
        uint64_t stack = 0;
        if (const auto error = identities.thread_attributes(call.rsi, detached, stack)) return kernel_error(error);
        // Every slot has a generalStack stack: a larger request gets it rather
        // than failing (refusing a thread usually ends the game).
        (void)stack;
        std::unique_lock guard(lock);
        if (stopping) return kernel_error(again);
        reap(guard);
        if (stopping) return kernel_error(again);
        const auto slot = std::find_if(threads.begin(), threads.end(), [](const Thread& t) { return !t.used; });
        if (slot == threads.end()) return kernel_error(again);
        // FEX does not check the reservation of the new thread's lookup cache
        // and crashes on it: refuse the thread while that space is missing.
        if (void* spare = ::mmap(nullptr, size_t(maxps4::GuestELF::fexLookupCacheBytes), PROT_NONE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); spare != MAP_FAILED)
            ::munmap(spare, size_t(maxps4::GuestELF::fexLookupCacheBytes));
        else
            return kernel_error(again);
        uint64_t top = 0, tp = 0;
        if (!image.prepare_thread(static_cast<size_t>(slot - threads.begin()), top, tp)) return kernel_error(nomem);
        const uint64_t id = identities.attach();
        if (!id) return kernel_error(again);
        uint8_t handle[8], back[8];
        put_le(handle, 0, id, 8);
        put_le(back, 0, image.thread_return(), 8);
        if (!memory.write(call.rdi, handle, sizeof(handle)) || !memory.write(top - 8, back, sizeof(back))) {
            identities.detach(id);
            return kernel_error(fault);
        }
        slot->id = id; slot->entry = call.rdx; slot->arg = call.rcx;
        slot->rsp = top - 8; slot->fs = tp; slot->value = 0;
        slot->finished = slot->joining = slot->ends_process = false;
        slot->detached = detached;
        try {
            slot->host = std::thread([this, thread = &*slot] { run(*thread); });
        } catch (const std::system_error&) {
            identities.detach(id);
            return kernel_error(again);
        }
        slot->used = true;
        return 0;
    }
    // scePthreadJoin(thread, void** value).
    int64_t join(uint64_t id, uint64_t out) {
        using namespace maxps4::ps4;
        if (id == ProbeAddressSpace::guest_thread) return kernel_error(deadlk);
        std::unique_lock guard(lock);
        const auto target = std::find_if(threads.begin(), threads.end(),
            [id](const Thread& t) { return t.used && t.id == id; });
        if (target == threads.end()) return kernel_error(srch);
        if (target->joining || target->detached) return kernel_error(inval);
        target->joining = true;
        changed.wait(guard, [&] { return target->finished || stopping; });
        // Stopping (time limit or a child's fault): return to guest code, where
        // the stop signals land, and leave the thread to finish(). A faulting
        // child only exits once the main run is done, so joining it here would
        // never return.
        if (stopping) { target->joining = false; return kernel_error(intr); }
        // finish() may join hosts too: whoever moves the handle out joins it.
        std::thread host = std::move(target->host);
        const uint64_t value = target->value;
        guard.unlock();
        if (host.joinable()) host.join();
        guard.lock();
        // A thread that still owns a mutex keeps its identity (PS4Threads).
        identities.detach(id);
        target->used = target->joining = false;
        guard.unlock();
        if (!out) return 0;
        uint8_t wire[8];
        put_le(wire, 0, value, 8);
        return memory.write(out, wire, sizeof(wire)) ? 0 : kernel_error(fault);
    }
    // scePthreadDetach(thread): its slot is reused once it ends.
    int64_t detach(uint64_t id) {
        using namespace maxps4::ps4;
        std::unique_lock guard(lock);
        const auto target = std::find_if(threads.begin(), threads.end(),
            [id](const Thread& t) { return t.used && t.id == id; });
        if (target == threads.end()) return kernel_error(srch);
        if (target->joining || target->detached) return kernel_error(inval);
        target->detached = true;
        reap(guard);
        return 0;
    }
    // scePthreadExit(value), or the return of a child's entry: the HLT after
    // the SYSCALL ends the calling thread. On the main thread the process
    // lives on until its other threads end (wait()).
    void exit(uint64_t value) {
        if (current) { current->value = value; return; }
        std::lock_guard guard(lock);
        main_exited = true;
    }
    // The entry's exit function: called from a child thread, that thread ends
    // on the HLT after the SYSCALL and then stops the main run.
    void process_exit() { if (current) current->ends_process = true; }
    bool main_thread_exited() const {
        std::lock_guard guard(lock);
        return main_exited;
    }
    // After the main thread's scePthreadExit: until every child has ended, or
    // a stop (time limit, fault, exit function) is requested.
    void wait() {
        std::unique_lock guard(lock);
        changed.wait(guard, [&] {
            return stopping || std::none_of(threads.begin(), threads.end(),
                [](const Thread& t) { return t.used && !t.finished; });
        });
    }
    // Time limit expired: wake every join, lock and wait so the stop signals
    // can land.
    void interrupt() {
        { std::lock_guard guard(lock); stopping = timed_out = true; }
        ending_.store(true, std::memory_order_relaxed);
        changed.notify_all();
        identities.interrupt();
    }
    // Once a stop began: a thread's next return to the guest ends it. The main
    // thread too: it spins on calls like the others, and run_orbis still reports
    // the stop (expired()).
    bool ending() const { return ending_.load(std::memory_order_relaxed); }
    bool expired() const {
        std::lock_guard guard(lock);
        return timed_out;
    }
    // The main thread's run ended: stop and join every child thread.
    void finish() {
        std::unique_lock guard(lock);
        if (main_done) return;
        stopping = main_done = true;
        ending_.store(true, std::memory_order_relaxed);
        changed.notify_all();
        identities.interrupt();
        // All at once: a thread spinning in JIT code stops on its signal, and
        // none has to wait for the ones before it.
        for (bool running = true; running; ) {
            running = false;
            for (auto& thread : threads) if (thread.used && !thread.finished) {
                running = true;
                if (GuestFaultGuard::available && thread.host.joinable())
                    ::pthread_kill(thread.host.native_handle(), SIGURG);
            }
            if (running) changed.wait_for(guard, std::chrono::milliseconds(1));
        }
        // Every child has finished, so every join in progress has returned.
        for (auto& thread : threads) {
            if (!thread.used) continue;
            std::thread host = std::move(thread.host);
            guard.unlock();
            if (host.joinable()) host.join();
            guard.lock();
            identities.detach(thread.id);
            thread.used = false;
        }
    }
    // For reports: where a child thread making no HLE call runs, from 8 reads
    // 1 ms apart: the distinct guest addresses in JIT code (a spinning loop
    // shows as a few), else the host function and its caller (a thread held
    // inside FEX), and the guest RIP FEX last stored.
    // `regs`, when given, receives x0-x30 of a sample taken in JIT code, and
    // `jit_rip` that sample's guest RIP (0: no such sample).
    std::string sample(uint64_t id, uint64_t relocation, uint64_t* regs_out = nullptr, uint64_t* jit_rip = nullptr) {
        std::string out;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        std::lock_guard held(lock); // Keeps the thread's guard alive.
        const auto at = std::find_if(threads.begin(), threads.end(),
            [&](const Thread& t) { return t.used && !t.finished && t.id == id && t.guard; });
        if (at == threads.end()) return " · thread hôte introuvable";
        GuestFaultGuard* const fault = at->guard;
        std::vector<uint64_t> rips;
        const thread_act_t port = ::pthread_mach_thread_np(fault->host);
        uint64_t pcs[8] {}, lrs[8] {};
        // Host registers of one sample: in JIT code FEX keeps the guest's
        // registers in host registers, so they show a spinning loop's state.
        uint64_t regs[31] {};
        bool have_regs = false;
        size_t count = 0;
        rips.reserve(8);
        // Nothing that may lock runs while the thread is suspended.
        for (int k = 0; k < 8; ++k) {
            if (k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            arm_thread_state64_t state {};
            mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
            if (::thread_suspend(port) != KERN_SUCCESS) break;
            const bool read = ::thread_get_state(port, ARM_THREAD_STATE64,
                reinterpret_cast<thread_state_t>(&state), &words) == KERN_SUCCESS;
            ::thread_resume(port);
            if (!read) continue;
            uint64_t pc = arm_thread_state64_get_pc(state), lr = arm_thread_state64_get_lr(state);
#if __has_feature(ptrauth_calls)
            pc = uint64_t(reinterpret_cast<uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(pc), ptrauth_key_return_address)));
            lr = uint64_t(reinterpret_cast<uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(lr), ptrauth_key_return_address)));
#endif
            if (!have_regs) {
                std::copy(std::begin(state.__x), std::end(state.__x), regs);
                regs[29] = uint64_t(arm_thread_state64_get_fp(state)); // FEX keeps guest r15 there.
                regs[30] = lr;
                have_regs = true;
            }
            lrs[count] = lr; pcs[count++] = pc;
        }
        auto relative = [&](uint64_t rip) { return rip >= relocation ? rip - relocation : rip; };
        auto name = [](uint64_t address) {
            Dl_info info {};
            if (!::dladdr(reinterpret_cast<void*>(address), &info) || !info.dli_sname) return std::string("?");
            char text[160];
            std::snprintf(text, sizeof text, "%s+0x%llx", info.dli_sname,
                (unsigned long long)(address - reinterpret_cast<uintptr_t>(info.dli_saddr)));
            return std::string(text);
        };
        std::string host;
        char line[96];
        for (size_t k = 0; k < count; ++k) {
            if (fault->in_jit(pcs[k])) {
                const uint64_t rip = fault->guest_rip(pcs[k]);
                if (std::find(rips.begin(), rips.end(), rip) == rips.end()) rips.push_back(rip);
            } else if (host.empty()) {
                host = fault->in_dispatcher(pcs[k]) ? std::string("répartiteur FEX") : name(pcs[k]) + " < " + name(lrs[k]);
            }
        }
        const char* separator = " · tourne en";
        for (const uint64_t rip : rips) {
            std::snprintf(line, sizeof line, "%s 0x%llx", separator, (unsigned long long)relative(rip));
            out += line; separator = ",";
        }
        if (!host.empty()) out += " · dans l'émulateur : " + host;
        std::snprintf(line, sizeof line, " · RIP FEX 0x%llx", (unsigned long long)relative(fault->stored_rip()));
        out += line;
        if (!count) out += " · illisible";
        if (regs_out && have_regs && !rips.empty()) {
            std::copy(std::begin(regs), std::end(regs), regs_out);
            if (jit_rip) *jit_rip = relative(rips.front());
        }
        // The JIT code around the first sample in it: the ARM64 FEX made of the loop.
        for (size_t k = 0; k < count; ++k) {
            if (!fault->in_jit(pcs[k])) continue;
            const uint64_t from = (pcs[k] & ~uint64_t(3)) - 32 * 4;
            if (!fault->in_jit(from) || !fault->in_jit(from + 63 * 4)) continue; // Stays inside the code buffer.
            std::snprintf(line, sizeof line, " · code JIT depuis PC-0x80 (PC 0x%llx) :", (unsigned long long)pcs[k]);
            out += line;
            for (int w = 0; w < 64; ++w) {
                uint32_t word = 0;
                std::memcpy(&word, reinterpret_cast<const void*>(from + uint64_t(w) * 4), 4);
                std::snprintf(line, sizeof line, " %08x", word);
                out += line;
            }
            break;
        }
        if (have_regs && !rips.empty()) {
            out += " · registres hôte :";
            for (int r = 0; r < 31; ++r) {
                std::snprintf(line, sizeof line, " x%d=0x%llx", r, (unsigned long long)regs[r]);
                out += line;
            }
        }
#else
        (void)id; (void)relocation;
#endif
        return out;
    }
    // Where every guest thread is right now, one sample each: its guest RIP in
    // JIT code, else the RIP FEX last stored (in a service or FEX itself).
    std::string where_all(uint64_t relocation, bool registers = false) {
        std::string out;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        std::lock_guard held(lock);
        char line[64];
        for (const Thread& t : threads) {
            if (!t.used || t.finished || !t.guard) continue;
            const thread_act_t port = ::pthread_mach_thread_np(t.guard->host);
            arm_thread_state64_t state {};
            mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
            if (::thread_suspend(port) != KERN_SUCCESS) continue;
            const bool read = ::thread_get_state(port, ARM_THREAD_STATE64,
                reinterpret_cast<thread_state_t>(&state), &words) == KERN_SUCCESS;
            ::thread_resume(port);
            if (!read) continue;
            uint64_t pc = arm_thread_state64_get_pc(state);
#if __has_feature(ptrauth_calls)
            pc = uint64_t(reinterpret_cast<uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(pc), ptrauth_key_return_address)));
#endif
            const bool jit = t.guard->in_jit(pc);
            uint64_t rip = jit ? t.guard->guest_rip(pc) : t.guard->stored_rip();
            if (rip >= relocation) rip -= relocation;
            std::snprintf(line, sizeof line, " t%llu%s0x%llx", (unsigned long long)t.id, jit ? "@" : "~", (unsigned long long)rip);
            out += line;
            // In JIT code the host registers hold the guest's (x4 rax, x5 rdx,
            // x6 rbx, x7 rcx, x8 rsp, x9 rbp, x10 rsi, x11 rdi, x12-x17 r8-r13,
            // x19 r14, x29 r15 in this FEX build).
            if (registers && jit) {
                out += " [";
                for (int r : {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 29}) {
                    const uint64_t v = r == 29 ? uint64_t(arm_thread_state64_get_fp(state)) : state.__x[r];
                    std::snprintf(line, sizeof line, "%sx%d=0x%llx", r == 4 ? "" : " ", r, (unsigned long long)v);
                    out += line;
                }
                out += "]";
            }
        }
#else
        (void)relocation;
#endif
        return out;
    }
    // How a thread that is no longer running ended (false: unknown).
    bool ending_of(uint64_t id, uint64_t& rip, uint64_t& ns, int& signal, int& code, uint64_t& address,
                   std::array<uint64_t, 16>* gprs = nullptr) const {
        std::lock_guard held(lock);
        for (size_t k = 0; k < ended.size(); ++k) {
            const Ended& e = ended[(ended_next + ended.size() - 1 - k) % ended.size()];
            if (e.id == id && e.ns) {
                rip = e.rip; ns = e.ns; signal = e.signal; code = e.code; address = e.address;
                if (gprs) *gprs = e.gprs;
                return true;
            }
        }
        return false;
    }
    bool faulted(maxps4_fex_guest_fault& out) const {
        std::lock_guard guard(lock);
        out = first_fault;
        return first_fault.signal != 0;
    }
    // The thread that faulted first and its guest registers at the fault.
    bool fault_thread(uint64_t& id, std::array<uint64_t, 16>& gprs) const {
        std::lock_guard guard(lock);
        id = first_fault_thread; gprs = first_fault_gprs;
        return first_fault.signal != 0;
    }
private:
    struct Thread {
        bool used = false, joining = false, finished = false, ends_process = false, detached = false;
        uint64_t id = 0, entry = 0, arg = 0, rsp = 0, fs = 0, value = 0;
        GuestFaultGuard* guard = nullptr; // While it runs guest code (under `lock`).
        std::thread host;
    };
    // Gives back the slots of detached threads that have ended. Never while
    // stopping: a thread that stops the main run waits for it to be done.
    void reap(std::unique_lock<std::mutex>& guard) {
        for (auto& thread : threads) {
            if (stopping || !thread.used || !thread.detached || !thread.finished) continue;
            std::thread host = std::move(thread.host);
            const uint64_t id = thread.id;
            thread.used = false;
            guard.unlock();
            if (host.joinable()) host.join(); // It has ended: only returns from run().
            guard.lock();
            identities.detach(id);
        }
    }
    void run(Thread& thread) {
        ProbeAddressSpace::guest_thread = thread.id;
        ProbeAddressSpace::guest_tls = thread.fs;
        current = &thread;
        maxps4_fex_guest_fault fault {};
        uint64_t ended_rip = 0, ended_address = 0;
        std::array<uint64_t, 16> ended_gprs {};
        int ended_signal = 0, ended_code = 0;
        bool skip = false;
        { std::lock_guard guard(lock); skip = stopping; }
        if (!skip) {
            FEXThreadSetup setup(page);
            if (auto* fex = context->CreateThread(thread.entry, thread.rsp)) {
                if (setup.prepare(fex, thread.entry, thread.rsp, thread.fs) == 0) {
                    fex->CurrentFrame->State.gregs[FEXCore::X86State::REG_RDI] = thread.arg;
                    GuestFaultGuard guard(context, fex, config, 0, false, {}, nullptr,
                        [this](uint64_t a,size_t n,uint32_t p) { return memory.permitted(a,n,p); });
                    { std::lock_guard held(lock); thread.guard = &guard; }
                    context->ExecuteThread(fex);
                    { std::lock_guard held(lock); thread.guard = nullptr; }
                    const int signal = guard.signal.load();
                    ended_rip = guard.fault_rip ? guard.fault_rip : fex->CurrentFrame->State.rip; ended_signal = signal;
                    ended_code = guard.code; ended_address = guard.address;
                    // Guest registers as the stop handler spilled them (x86 encoding order).
                    std::copy_n(std::begin(fex->CurrentFrame->State.gregs), 16, ended_gprs.begin());
                    // SIGURG only comes from finish(): a stop, not a fault.
                    if (signal && signal != SIGURG)
                        fault = {signal, guard.code, guard.address, ended_rip};
                }
                context->DestroyThread(fex);
            }
        }
        current = nullptr;
        bool stop_main = false;
        {
            std::lock_guard guard(lock);
            if (fault.signal && !first_fault.signal) {
                first_fault = fault; first_fault_thread = thread.id; first_fault_gprs = ended_gprs;
                stop_main = stopping = true;
            }
            if (thread.ends_process) stop_main = stopping = true;
            ended[ended_next++ % ended.size()] = {thread.id, ended_rip, ProbeAddressSpace::now_ns(), ended_address, ended_signal, ended_code, ended_gprs};
            thread.finished = true; // `thread` may be reused from here on.
        }
        changed.notify_all();
        if (!stop_main) return;
        // A faulting or exiting thread ends the process: stop the main thread too.
        // Its next HLE call ends it (a game loop around waits that now return
        // at once otherwise spins forever).
        ending_.store(true, std::memory_order_relaxed);
        identities.interrupt();
        std::unique_lock guard(lock);
        while (!main_done) {
            ::pthread_kill(main_thread, SIGURG);
            changed.wait_for(guard, std::chrono::milliseconds(1));
        }
    }
    FEXCore::Context::Context* context;
    FEXCore::SignalDelegatorConfig config;
    maxps4::GuestELF& image;
    maxps4::PS4GuestMemory& memory;
    maxps4::PS4Threads& identities;
    size_t page;
    pthread_t main_thread;
    mutable std::mutex lock;
    std::condition_variable changed;
    std::array<Thread, maxps4::GuestELF::generalThreads> threads {};
    // The last threads that ended, for reports: where FEX stopped them.
    struct Ended { uint64_t id = 0, rip = 0, ns = 0, address = 0; int signal = 0, code = 0; std::array<uint64_t, 16> gprs {}; };
    std::array<Ended, 32> ended {};
    size_t ended_next = 0;
    bool stopping = false, main_done = false, main_exited = false, timed_out = false;
    maxps4_fex_guest_fault first_fault {};
    uint64_t first_fault_thread = 0;
    std::array<uint64_t, 16> first_fault_gprs {};
    static inline thread_local Thread* current = nullptr;
    std::atomic<bool> ending_ {false};
};
bool guest_thread_ending(const GuestThreads& threads) { return threads.ending(); }
int64_t guest_thread_service(GuestThreads& threads, maxps4::PS4KernelService service,
                             const maxps4::PS4KernelCall& call) {
    using maxps4::PS4KernelService;
    switch (service) {
    case PS4KernelService::thread_create: return threads.create(call);
    case PS4KernelService::thread_join: return threads.join(call.rdi, call.rsi);
    case PS4KernelService::thread_exit: threads.exit(call.rdi); return 0;
    case PS4KernelService::thread_detach: return threads.detach(call.rdi);
    case PS4KernelService::process_exit: threads.process_exit(); return 0;
    default: return maxps4::ps4::kernel_error(maxps4::ps4::nosys);
    }
}
}
// CPU profile of the main guest thread (maxps4_fex_engine_set_profiling):
// a host thread suspends it through Mach every 5 ms and records where it
// is: a guest RIP (JIT code, 64-byte buckets), the FEX dispatcher, or a host
// function (HLE services, FEX helpers). Only the registers are read while it
// is suspended: it may hold the JIT pool lock the FEX lookups take (it did,
// compiling Mortal Kombat: Legacy Kollection, and the game and app froze).
struct CPUProfile {
    std::mutex lock;
    uint64_t jit = 0, dispatcher = 0, host = 0;
    std::unordered_map<uint64_t, uint64_t> guest;   // RIP bucket -> samples
    std::unordered_map<uint64_t, uint64_t> symbols; // host symbol start -> samples
    void clear() { std::lock_guard g(lock); jit = dispatcher = host = 0; guest.clear(); symbols.clear(); }
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
    void run(const GuestFaultGuard& guard, const std::atomic<bool>& done) {
        const thread_act_t port = ::pthread_mach_thread_np(guard.host);
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (done.load()) break;
            arm_thread_state64_t state {};
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            if (::thread_suspend(port) != KERN_SUCCESS) break;
            const bool read = ::thread_get_state(port, ARM_THREAD_STATE64,
                reinterpret_cast<thread_state_t>(&state), &count) == KERN_SUCCESS;
            ::thread_resume(port);
            if (!read) continue;
            uint64_t pc = arm_thread_state64_get_pc(state), rip = 0;
#if __has_feature(ptrauth_calls)
            pc = uint64_t(reinterpret_cast<uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(pc), ptrauth_key_return_address)));
#endif
            uint32_t where = 0;
            if (guard.in_jit(pc)) { where = 1; rip = guard.guest_rip(pc); }
            else where = guard.in_dispatcher(pc) ? 2 : 3;
            uint64_t symbol = 0;
            if (where == 3) {
                Dl_info info {};
                if (::dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_saddr)
                    symbol = reinterpret_cast<uintptr_t>(info.dli_saddr);
            }
            std::lock_guard g(lock);
            if (where == 1) { ++jit; ++guest[rip & ~uint64_t{0x3f}]; }
            else if (where == 2) ++dispatcher;
            else { ++host; ++symbols[symbol]; }
        }
    }
#endif
};
// Diagnostic for Call of Duty: Modern Warfare Remastered (CUSA05379). Its
// string table (65536 entries of 8 bytes: low 18 bits the next index of a
// circular chain, then the key) ends up with a chain that loops without its
// head, and a lookup spins forever (default.elf 0x5b1640), though every change
// holds one mutex and no mutex call fails. On each lock of that mutex (at most
// every 4 ms) the table is copied and the entries pointed at by two others are
// counted; the first time that count grows, the entries changed since the
// previous check and the last lock/unlock calls are kept for the report.
class StringTableWatch {
public:
    StringTableWatch(maxps4::PS4GuestMemory& memory, uint64_t table, uint64_t pool, uint64_t relocation)
        : memory_(memory), relocation_(relocation), table_(table), pool_(pool), start_(clock()), in_degree_(entries) {}
    ~StringTableWatch() {
        stop_ = true;
        if (guard_thread_.joinable()) guard_thread_.join();
    }
    // Polls the 32 bytes of pool string `key` every 200 us; each change is kept
    // with the bytes before and after and where every guest thread was (`who`).
    // Call of Duty's release lookup spun on string 0x14e found zeroed while
    // still in the table: this catches what wrote it.
    void guard_string(uint32_t key, std::function<std::string(bool)> who) {
        guard_key_ = key;
        guard_thread_ = std::thread([this, key, who = std::move(who)] {
            // The string's 32 bytes sit at offset `half` of a 4 KiB window.
            constexpr size_t window = 4096, half = 2048;
            std::vector<uint8_t> before_window(window), now_window(window);
            uint8_t* const before = before_window.data() + half;
            uint8_t* const now = now_window.data() + half;
            bool have = false;
            int last_bit = -1;
            uint32_t last_word = 0;
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                {
                    uint8_t head[8];
                    uint32_t word = 0;
                    if (block_state(key, head, word)) {
                        const int bit = key_free(key, word) ? 1 : 0;
                        if (last_bit >= 0 && bit != last_bit) {
                            const bool live = (head[0] | head[1]) && head[3];
                            std::string where = who(bit && live);
                            std::lock_guard guard(lock_);
                            ++word_changes_;
                            char line[200];
                            std::snprintf(line, sizeof line, "  %.3f s : %08x -> %08x, bit %d, chaîne %02x%02x%02x%02x%s · threads",
                                double(clock() - start_) / 1e9, last_word, word, bit, head[0], head[1], head[2], head[3],
                                bit && live ? " LIBRE ALORS QUE VIVANTE" : "");
                            const std::string entry = line + where;
                            if (word_log_.size() < 6) word_log_.push_back(entry);
                            else word_tail_[word_tail_next_++ % word_tail_.size()] = entry;
                        }
                        last_bit = bit; last_word = word;
                    }
                }
                uint8_t base_bytes[8];
                if (!memory_.read(pool_, base_bytes, 8)) continue;
                uint64_t base;
                std::memcpy(&base, base_bytes, 8);
                const uint64_t at = base + uint64_t(key) * 16;
                if (!base || !memory_.read(at - half, now_window.data(), window)) continue;
                if (have && !std::memcmp(before, now, 32)) { std::memcpy(before_window.data(), now_window.data(), window); continue; }
                if (have) {
                    // A string wiped (its length byte gone): the writer's registers too.
                    const bool wiped = before[3] && !now[3];
                    std::string where = who(wiped);
                    // The changed span of the window and how much of it is now zero.
                    size_t first = window, last = 0, zero = 0;
                    for (size_t k = 0; k < window; ++k)
                        if (before_window[k] != now_window[k]) {
                            first = std::min(first, k); last = k;
                            if (!now_window[k]) ++zero;
                        }
                    std::lock_guard guard(lock_);
                    ++guard_changes_;
                    char line[200];
                    std::snprintf(line, sizeof line, "  %.3f s :", double(clock() - start_) / 1e9);
                    std::string entry = line;
                    entry += " avant " + bytes_text(before) + " · après " + bytes_text(now);
                    if (first < window) {
                        std::snprintf(line, sizeof line, " · fenêtre 0x%llx : changé de 0x%llx à 0x%llx (%zu octets dont %zu à zéro)",
                            (unsigned long long)(at - half), (unsigned long long)(at - half + first),
                            (unsigned long long)(at - half + last + 1), last + 1 - first, zero);
                        entry += line;
                    }
                    entry += " · threads" + where;
                    if (guard_log_.size() < 6) guard_log_.push_back(entry);
                    else { guard_tail_[guard_tail_next_++ % guard_tail_.size()] = entry; }
                }
                std::memcpy(before_window.data(), now_window.data(), window);
                have = true;
            }
        });
    }
    void event(uint64_t thread, uint64_t caller, bool unlock, uint64_t r15 = 0, uint64_t r14 = 0, uint64_t r12 = 0, uint64_t rbp = 0, uint64_t rsp = 0) {
        std::lock_guard guard(lock_);
        const uint64_t now = clock();
        events_[next_event_++ % events_.size()] = {now, thread, caller, unlock};
        note_creation(caller, unlock, uint32_t(r12), rbp);
        note_release(thread, caller, unlock, uint32_t(r15), uint32_t(r14), now, rbp, rsp);
        check_freed(thread, caller, unlock, now);
        if (unlock || captured_.load() || (checks_ && now - last_check_ < 4000000)) return;
        last_check_ = now;
        check(thread, caller, now);
    }
    std::string report() const {
        std::lock_guard guard(lock_);
        char line[200];
        std::snprintf(line, sizeof line, "Table de chaînes (CoD) : %llu vérifications, %llu entrées pointées deux fois (au départ %lld)%s\n",
            (unsigned long long)checks_, (unsigned long long)last_bad_, (long long)baseline_,
            unreadable_ ? " · table illisible" : "");
        std::string out = std::string(line) + capture_;
        if (guard_thread_.joinable()) {
            std::snprintf(line, sizeof line, "Chaîne 0x%x surveillée : %llu changements (@ = code du jeu, ~ = dans un service)\n",
                guard_key_.load(), (unsigned long long)guard_changes_);
            out += line;
            for (const auto& entry : guard_log_) out += entry + "\n";
            const size_t tail = std::min<size_t>(guard_tail_next_, guard_tail_.size());
            if (guard_tail_next_ > tail) out += "  …\n";
            for (size_t k = 0; k < tail; ++k)
                out += guard_tail_[(guard_tail_next_ - tail + k) % guard_tail_.size()] + "\n";
            std::snprintf(line, sizeof line, "  bitmap, mot 0x%x (bit de la chaîne : 1 = libre) : %llu changements de ce bit\n",
                guard_key_.load() >> 5, (unsigned long long)word_changes_);
            out += line;
            for (const auto& entry : word_log_) out += entry + "\n";
            const size_t words = std::min<size_t>(word_tail_next_, word_tail_.size());
            if (word_tail_next_ > words) out += "  …\n";
            for (size_t k = 0; k < words; ++k)
                out += word_tail_[(word_tail_next_ - words + k) % word_tail_.size()] + "\n";
            out += freed_capture_.empty() ? "  jamais vue libérée vivante\n" : freed_capture_;
            std::snprintf(line, sizeof line, "  libérations de chaînes : %llu, dont %llu de plus de 256 octets sur une clé hors début de mot\n",
                (unsigned long long)releases_, (unsigned long long)bad_releases_);
            out += line;
            for (const auto& entry : bad_log_) out += entry + "\n";
            std::snprintf(line, sizeof line, "  créations suivies : %llu · libérations d'une chaîne suivie avec une autre longueur : %llu · libérations non suivies : %llu\n",
                (unsigned long long)creations_, (unsigned long long)mismatches_, (unsigned long long)unknown_releases_);
            out += line;
            for (const auto& entry : mismatch_log_) out += entry + "\n";
            for (const auto& entry : unknown_log_) out += entry + "\n";
        }
        return out;
    }
    // The lookup spinning at default.elf 0x5b1640 walks a chain for an entry
    // whose second word is `key` (guest r15d; FEX keeps it in x29), now at
    // `node` (rdx, x5): the chain it walks, every entry holding `key` with
    // its own chain, and the pool string `key` names (16-byte headers at the
    // pointer in `pool_`: u16 references, byte 3 = length + 1 mod 256, text
    // at +8) with the bucket its length and text give.
    std::string explain(uint32_t key, uint32_t node) const {
        std::vector<uint8_t> table(entries * 8);
        if (node >= entries || !memory_.read(table_, table.data(), table.size())) return " · table illisible";
        char line[96];
        std::snprintf(line, sizeof line, " · recherche 0x%x, chaîne :", key);
        std::string out = line;
        uint32_t at = node;
        for (int step = 0; step < 24; ++step) {
            std::snprintf(line, sizeof line, " 0x%x[%08x:%08x]", at, word(table, at), word(table, at, 1));
            out += line;
            at = word(table, at) & 0x3ffff;
            if (at >= entries || at == node) break;
        }
        out += " · entrées portant la clé :";
        int found = 0;
        for (size_t i = 0; i < entries && found < 8; ++i)
            if (word(table, i, 1) == key) {
                std::snprintf(line, sizeof line, " 0x%zx[%08x]", i, word(table, i));
                out += line; ++found;
            }
        if (!found) out += " aucune";
        for (size_t i = 0, shown = 0; i < entries && shown < 2; ++i) {
            if (word(table, i, 1) != key || i == node) continue;
            std::snprintf(line, sizeof line, " · chaîne de 0x%zx :", i);
            out += line; ++shown;
            uint32_t at = uint32_t(i);
            for (int step = 0; step < 12; ++step) {
                std::snprintf(line, sizeof line, " 0x%x[%08x:%08x]", at, word(table, at), word(table, at, 1));
                out += line;
                at = word(table, at) & 0x3ffff;
                if (at >= entries || at == i) break;
            }
        }
        out += describe_string(key);
        return out;
    }
private:
    static constexpr size_t entries = 0x10000;
    struct Event { uint64_t ns = 0, thread = 0, caller = 0; bool unlock = false; };
    static uint64_t clock() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static uint32_t word(const std::vector<uint8_t>& table, size_t index, size_t half = 0) {
        uint32_t value;
        std::memcpy(&value, table.data() + index * 8 + half * 4, 4);
        return value;
    }
    void check(uint64_t thread, uint64_t caller, uint64_t now) {
        current_.resize(entries * 8);
        if (!memory_.read(table_, current_.data(), current_.size())) { ++unreadable_; return; }
        std::fill(in_degree_.begin(), in_degree_.end(), 0);
        uint64_t bad = 0;
        size_t sample = 0;
        for (size_t i = 1; i < entries; ++i) {
            const uint32_t next = word(current_, i) & 0x3ffff;
            if (next >= entries) { ++bad; sample = i; continue; }
            if (next && ++in_degree_[next] == 2) { ++bad; sample = next; }
        }
        ++checks_;
        last_bad_ = bad;
        if (baseline_ < 0) baseline_ = int64_t(bad);
        if (int64_t(bad) > baseline_ && !previous_.empty()) {
            capture(thread, caller, now, bad, sample);
            captured_.store(true);
            return;
        }
        previous_.swap(current_);
        previous_check_ = now;
    }
    void capture(uint64_t thread, uint64_t caller, uint64_t now, uint64_t bad, size_t sample) {
        char line[256];
        std::snprintf(line, sizeof line, "  cassée à %.2f s (vérification %llu, la précédente %.2f ms avant) : %llu entrées doubles, vu par le thread %llu en prenant le verrou depuis 0x%llx\n",
            double(now - start_) / 1e9, (unsigned long long)checks_, double(now - previous_check_) / 1e6,
            (unsigned long long)bad, (unsigned long long)thread, (unsigned long long)caller);
        capture_ += line;
        std::snprintf(line, sizeof line, "  entrée 0x%zx (mot 0x%08x clé 0x%08x) pointée par :", sample, word(current_, sample), word(current_, sample, 1));
        capture_ += line;
        int shown = 0;
        for (size_t i = 1; i < entries && shown < 4; ++i)
            if ((word(current_, i) & 0x3ffff) == sample) {
                std::snprintf(line, sizeof line, " 0x%zx (mot 0x%08x clé 0x%08x, avant 0x%08x)", i, word(current_, i), word(current_, i, 1), word(previous_, i));
                capture_ += line; ++shown;
            }
        capture_ += "\n  entrées changées depuis la vérification précédente :";
        size_t changed = 0;
        for (size_t i = 0; i < entries; ++i) {
            if (std::memcmp(current_.data() + i * 8, previous_.data() + i * 8, 8) == 0) continue;
            if (++changed <= 32) {
                std::snprintf(line, sizeof line, " 0x%zx %08x:%08x→%08x:%08x", i, word(previous_, i), word(previous_, i, 1), word(current_, i), word(current_, i, 1));
                capture_ += line;
            }
        }
        std::snprintf(line, sizeof line, " (%zu en tout)\n  derniers appels au verrou, récents d'abord :", changed);
        capture_ += line;
        for (size_t k = 0; k < events_.size() && k < next_event_; ++k) {
            const Event& e = events_[(next_event_ - 1 - k) % events_.size()];
            std::snprintf(line, sizeof line, " t%llu %s 0x%llx (-%.2f ms)", (unsigned long long)e.thread, e.unlock ? "libère" : "prend",
                (unsigned long long)e.caller, double(now - e.ns) / 1e6);
            capture_ += line;
        }
        capture_ += "\n";
    }
    maxps4::PS4GuestMemory& memory_;
    const uint64_t relocation_;
    const uint64_t table_, pool_;
    // The pool string `key` names, as the game's release path reads it.
    std::string describe_string(uint32_t key) const {
        uint8_t base_bytes[8];
        if (!memory_.read(pool_, base_bytes, 8)) return " · pool illisible";
        uint64_t base;
        std::memcpy(&base, base_bytes, 8);
        const uint64_t at = base + uint64_t(key) * 16;
        uint8_t head[8], text[0x400] = {};
        if (!memory_.read(at, head, 8)) return " · chaîne illisible";
        const bool whole = memory_.read(at + 8, text, sizeof text);
        char line[160];
        std::snprintf(line, sizeof line, " · chaîne 0x%x en 0x%llx : en-tête %02x%02x%02x%02x %02x%02x%02x%02x",
            key, (unsigned long long)at, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
        std::string out = line;
        if (!whole) return out + ", texte illisible";
        // The length the release path finds (0x1882bf): byte 3 - 1 gives the
        // low byte, the terminator decides the 256s.
        const uint32_t low = uint8_t(head[3] - 1);
        uint32_t length = low + 1;
        for (uint32_t k = low; k < sizeof text && text[k]; k += 0x100) length += 0x100;
        uint32_t hash = 0;
        for (size_t k = 0; k < sizeof text && text[k]; ++k) hash = hash * 31 + uint32_t(int8_t(text[k]));
        if (length >= 0x100) hash = length >> 2;
        const uint32_t bucket = hash % 0xffff + 1;
        std::snprintf(line, sizeof line, ", longueur %u, seau 0x%x, texte « ", length, bucket);
        out += line;
        for (size_t k = 0; k < 48 && text[k]; ++k) out += (text[k] >= 32 && text[k] < 127) ? char(text[k]) : '.';
        return out + " »";
    }
    // The pool's allocation bitmap (MT_AllocIndex, default.elf 0x5af550): a
    // 32-bit word per 32 blocks of 16 bytes at pool + 0x400000, the first
    // block of a word in its top bit, a set bit free.
    bool block_state(uint32_t key, uint8_t head[8], uint32_t& word) const {
        uint8_t base_bytes[8];
        if (!memory_.read(pool_, base_bytes, 8)) return false;
        uint64_t base;
        std::memcpy(&base, base_bytes, 8);
        uint8_t w[4];
        if (!base || !memory_.read(base + uint64_t(key) * 16, head, 8) ||
            !memory_.read(base + 0x400000 + uint64_t(key >> 5) * 4, w, 4)) return false;
        std::memcpy(&word, w, 4);
        return true;
    }
    static bool key_free(uint32_t key, uint32_t word) { return (word >> (31 - (key & 31))) & 1; }
    // Under lock_. The release (default.elf 0x5b1530) takes the lock with the
    // key in r15d and the length the caller found in r14d, then frees the
    // block with MT_FreeIndex(key, length + 8). A block over 256 bytes is freed
    // as whole bitmap words, so a key that is not the first of a word with
    // such a length frees other strings: those releases are kept with the
    // string's header and text as the lock is taken.
    // Under lock_. A new string (default.elf 0x5b0dad..0x5b0df3) unlocks with
    // its key in r12d and its length with the terminator at [rbp - 0x48]: the
    // size MT_AllocIndex got, kept to check each release against.
    void note_creation(uint64_t caller, bool unlock, uint32_t key, uint64_t rbp) {
        if (!unlock || caller != 0x26b0df8 || key >= created_.size() || !rbp) return;
        uint8_t n[4];
        if (!memory_.read(rbp - 0x48, n, 4)) return;
        uint32_t length;
        std::memcpy(&length, n, 4);
        created_[key] = length ? length : 1;
        ++creations_;
    }
    static uint32_t blocks(uint32_t length) { return (length + 8 + 15) / 16; }
    void note_release(uint64_t thread, uint64_t caller, bool unlock, uint32_t key, uint32_t length, uint64_t now, uint64_t rbp, uint64_t rsp) {
        if (unlock || caller != 0x26b15d8) return;
        ++releases_;
        // The release must free the block size the creation allocated.
        if (key < created_.size() && created_[key]) {
            const uint32_t made = created_[key];
            created_[key] = 0;
            if (made != length && mismatch_log_.size() < 8) {
                uint8_t base_bytes[8], text[48] = {};
                uint64_t base = 0;
                if (memory_.read(pool_, base_bytes, 8)) std::memcpy(&base, base_bytes, 8);
                if (base) (void)memory_.read(base + uint64_t(key) * 16, text, sizeof text);
                char line[220];
                std::snprintf(line, sizeof line, "  %.3f s : t%llu libère la chaîne 0x%x avec la longueur %u (%u blocs), créée avec %u (%u blocs) · ",
                    double(now - start_) / 1e9, (unsigned long long)thread, key, length, blocks(length), made, blocks(made));
                mismatch_log_.push_back(line + bytes_text(text) + " · octets :" + hex_bytes(text, sizeof text) + release_context(rbp, rsp));
            }
            if (made != length) ++mismatches_;
        } else {
            // No creation since the last release: a string released twice, or
            // made by a path not followed.
            ++unknown_releases_;
            if (unknown_log_.size() < 8) {
                uint8_t base_bytes[8], text[48] = {}, w[4] = {};
                uint64_t base = 0;
                if (memory_.read(pool_, base_bytes, 8)) std::memcpy(&base, base_bytes, 8);
                if (base) {
                    (void)memory_.read(base + uint64_t(key) * 16, text, sizeof text);
                    (void)memory_.read(base + 0x400000 + uint64_t(key >> 5) * 4, w, 4);
                }
                uint32_t word;
                std::memcpy(&word, w, 4);
                char line[220];
                std::snprintf(line, sizeof line, "  %.3f s : t%llu libère la chaîne 0x%x jamais vue créée ou déjà libérée, longueur %u (%u blocs), mot 0x%x du bitmap = %08x · ",
                    double(now - start_) / 1e9, (unsigned long long)thread, key, length, blocks(length), key >> 5, word);
                unknown_log_.push_back(line + bytes_text(text) + " · octets :" + hex_bytes(text, sizeof text) + release_context(rbp, rsp));
            }
        }
        if (length + 8 <= 256 || !(key & 31)) return;
        ++bad_releases_;
        if (bad_log_.size() >= 8) return;
        uint8_t base_bytes[8], text[40] = {};
        uint64_t base = 0;
        if (memory_.read(pool_, base_bytes, 8)) std::memcpy(&base, base_bytes, 8);
        if (base) (void)memory_.read(base + uint64_t(key) * 16, text, sizeof text);
        char line[200];
        std::snprintf(line, sizeof line, "  %.3f s : t%llu libère la chaîne 0x%x avec la longueur %u (%u blocs de 16 octets) · ",
            double(now - start_) / 1e9, (unsigned long long)thread, key, length, (length + 8 + 15) / 16);
        bad_log_.push_back(line + bytes_text(text) + " · suite :" + hex_bytes(text + 8, 32));
    }
    // Only for the bounded anomaly logs, while the caller is at the service
    // boundary. The first key-0 release can free live strings much earlier
    // than their eventual corrupt lookup: retain its guest callers now.
    std::string release_context(uint64_t rbp, uint64_t rsp) const {
        char line[120];
        std::snprintf(line, sizeof line, " | rbp 0x%llx rsp 0x%llx | appels (rbp) :",
            (unsigned long long)rbp, (unsigned long long)rsp);
        std::string out = line;
        uint64_t frame = rbp;
        for (unsigned depth = 0; depth < 12 && frame && !(frame & 7); ++depth) {
            if (frame > UINT64_MAX - 15) break;
            uint8_t pair[16];
            if (!memory_.read(frame, pair, sizeof pair)) break;
            uint64_t next = 0, back = 0;
            std::memcpy(&next, pair, 8); std::memcpy(&back, pair + 8, 8);
            if (back < relocation_ || back - relocation_ >= 0x20000000) break;
            std::snprintf(line, sizeof line, " 0x%llx", (unsigned long long)(back - relocation_));
            out += line;
            if (next <= frame) break;
            frame = next;
        }
        // This caller is the script-save reader. Its saved r15 is the
        // MemoryFile*, and the value record is at caller rsp + 0x4c0.
        // Snapshot only: neither the save nor guest execution is changed.
        uint8_t returns[8];
        uint64_t back = 0;
        if (rbp && rbp <= UINT64_MAX - 15 && memory_.read(rbp + 8, returns, 8))
            std::memcpy(&back, returns, 8);
        if (back >= relocation_ && back - relocation_ == 0x2525ce8 && rbp >= 8) {
            uint8_t saved[8], stream[64], value[12];
            uint64_t pointer = 0;
            if (memory_.read(rbp - 8, saved, 8)) std::memcpy(&pointer, saved, 8);
            if (pointer && pointer <= UINT64_MAX - 63 && memory_.read(pointer, stream, sizeof stream)) {
                uint64_t base = 0, size = 0, position = 0;
                uint32_t mode = 0, compressed = 0;
                std::memcpy(&base, stream, 8); std::memcpy(&size, stream + 8, 8);
                std::memcpy(&position, stream + 24, 8);
                std::memcpy(&mode, stream + 52, 4); std::memcpy(&compressed, stream + 56, 4);
                std::snprintf(line, sizeof line, " | flux 0x%llx base 0x%llx taille %llu position %llu",
                    (unsigned long long)pointer, (unsigned long long)base,
                    (unsigned long long)size, (unsigned long long)position);
                out += line;
                std::snprintf(line, sizeof line, " mode %u compression %u erreur %u", mode, compressed, unsigned(stream[61]));
                out += line;
            }
            if (rbp <= UINT64_MAX - 0x4db && memory_.read(rbp + 0x4d0, value, sizeof value))
                out += " | valeur sauvegardee (8 octets puis type) :" + hex_bytes(value, sizeof value);
        }
        out += " | pile (mots bruts, pas une trace d'appels) :";
        for (unsigned word = 0; rsp && word < 16; ++word) {
            const uint64_t offset = uint64_t(word) * 8;
            if (rsp > UINT64_MAX - offset - 7) break;
            uint8_t bytes[8];
            if (!memory_.read(rsp + offset, bytes, sizeof bytes)) break;
            uint64_t value = 0;
            std::memcpy(&value, bytes, 8);
            std::snprintf(line, sizeof line, " +0x%x=0x%llx", word * 8, (unsigned long long)value);
            out += line;
        }
        return out;
    }
    static std::string hex_bytes(const uint8_t* b, size_t n) {
        std::string out;
        char line[8];
        for (size_t k = 0; k < n; ++k) { std::snprintf(line, sizeof line, "%s%02x", k % 4 ? "" : " ", b[k]); out += line; }
        return out;
    }
    // Under lock_: the first time the watched string is still referenced
    // (non-zero count and length) while its block is marked free, keep who
    // took or released the string table's lock then, and the calls before.
    void check_freed(uint64_t thread, uint64_t caller, bool unlock, uint64_t now) {
        const uint32_t key = guard_key_.load();
        if (!key || !freed_capture_.empty()) return;
        uint8_t head[8];
        uint32_t word = 0;
        if (!block_state(key, head, word)) return;
        const bool live = (head[0] | head[1]) && head[3];
        if (!live || !key_free(key, word)) return;
        char line[200];
        std::snprintf(line, sizeof line, "  libérée vivante à %.3f s, vu quand t%llu %s le verrou depuis 0x%llx : en-tête %02x%02x%02x%02x, mot 0x%x du bitmap = %08x\n  derniers appels au verrou, récents d'abord :",
            double(now - start_) / 1e9, (unsigned long long)thread, unlock ? "libère" : "prend", (unsigned long long)caller,
            head[0], head[1], head[2], head[3], key >> 5, word);
        freed_capture_ = line;
        for (size_t k = 0; k < events_.size() && k < next_event_; ++k) {
            const Event& e = events_[(next_event_ - 1 - k) % events_.size()];
            std::snprintf(line, sizeof line, " t%llu %s 0x%llx (-%.2f ms)", (unsigned long long)e.thread, e.unlock ? "libère" : "prend",
                (unsigned long long)e.caller, double(now - e.ns) / 1e6);
            freed_capture_ += line;
        }
        freed_capture_ += "\n";
    }
    // Header in hex, then the text as characters.
    static std::string bytes_text(const uint8_t* b) {
        char line[40];
        std::snprintf(line, sizeof line, "%02x%02x%02x%02x %02x%02x%02x%02x «", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        std::string out = line;
        for (int k = 8; k < 32 && b[k]; ++k) out += (b[k] >= 32 && b[k] < 127) ? char(b[k]) : '.';
        return out + "»";
    }
    std::atomic<bool> stop_ {false};
    std::thread guard_thread_;
    std::atomic<uint32_t> guard_key_ {0};
    std::string freed_capture_;
    uint64_t releases_ = 0, bad_releases_ = 0, creations_ = 0, mismatches_ = 0, unknown_releases_ = 0;
    std::vector<uint32_t> created_ = std::vector<uint32_t>(0x40000);
    std::vector<std::string> mismatch_log_, unknown_log_;
    std::vector<std::string> bad_log_;
    std::vector<std::string> word_log_;
    std::array<std::string, 10> word_tail_ {};
    uint64_t word_changes_ = 0, word_tail_next_ = 0;
    uint64_t guard_changes_ = 0, guard_tail_next_ = 0;
    std::vector<std::string> guard_log_;
    std::array<std::string, 10> guard_tail_ {};
    const uint64_t start_;
    mutable std::mutex lock_;
    std::array<Event, 48> events_ {};
    uint64_t next_event_ = 0, checks_ = 0, last_check_ = 0, previous_check_ = 0, last_bad_ = 0, unreadable_ = 0;
    int64_t baseline_ = -1;
    std::atomic<bool> captured_ {false};
    std::vector<uint8_t> current_, previous_;
    std::vector<uint16_t> in_degree_;
    std::string capture_;
};
struct maxps4_fex_engine {
    // This real FEX configuration holder installs no process signal handlers.
    // It outlives the context. Only admitted no-fault straight-line code executes.
    FEXCore::SignalDelegator signals;
    ProbeAddressSpace probe;
    maxps4::GuestELF staged_ps4;
    // V4 is opt-in and isolated from the stable V3 image/virtual memory.
    maxps4::GuestAddressSpace v4_address_space;
    maxps4::OrbisV4ExecutionGate v4_execution_gate{v4_address_space};
    bool orbis_admitted = false;
    maxps4_fex_cpu_report cpu_report{};
    maxps4_fex_guest_fault last_fault{};
    // The faulting child thread, its exact instruction, registers and target
    // memory, for the final report (the threads are gone by then).
    std::string last_fault_detail;
    uint32_t time_limit_ms = 0; // 0: no limit.
    std::atomic<bool> stop_requested{false};
    // The running main thread's guard, for sampling; guarded by sample_lock.
    std::mutex sample_lock;
    GuestFaultGuard* main_guard = nullptr;
    CPUProfile profile;
    std::atomic<bool> profiling{false};
    uint64_t relocation = 0; // Of the loaded game, for relative profile RIPs.
    std::string update_directory; // For load_game (set_update); empty: none.
    maxps4::GuestFrame frame;
    uint64_t frame_sequence = 0;
    bool frame_code_cached = false;
    maxps4::PS4VirtualMemory dynamic_ps4{staged_ps4};
    maxps4::PS4GuestMemory system_memory{staged_ps4, &dynamic_ps4};
    maxps4::PS4Libkernel libkernel{system_memory, &dynamic_ps4};
    maxps4::PS4DynamicLinker linker;
    bool import_probe = false, import_trace = false;
    std::vector<std::string> missing_imports;
    std::vector<std::atomic<uint32_t>> missing_reported;
    fextl::unique_ptr<FEXCore::Context::Context> context;
    int32_t stage = MAXPS4_FEX_CONTEXT_CREATED;
    // Call of Duty only (see StringTableWatch): its string table, checked under its mutex.
    // Last member: destroyed first, its polling thread joined while sample_lock
    // and the guest memory it reads still exist.
    std::unique_ptr<StringTableWatch> table_watch;
};
namespace {
// FEX configuration is process-global. This adapter owns it until the last
// engine closes. Future independent FEX consumers must share this ownership.
std::mutex runtime_lock;
size_t engines = 0;
#if TARGET_OS_IPHONE
// The prepared pool contains one dispatcher and one code cache.
maxps4_fex_engine* jit_owner = nullptr;
#endif
void release_config_if_unused() {
    if (engines == 0) FEXCore::Config::Shutdown();
}
}
extern "C" int32_t maxps4_fex_engine_open(maxps4_fex_engine **out) {
    if (!out) return -EINVAL;
    *out = nullptr;
    std::lock_guard lock(runtime_lock);
    auto *engine = new (std::nothrow) maxps4_fex_engine;
    if (!engine) return -ENOMEM;
    if (engines == 0) {
        FEXCore::Config::Initialize();
        FEXCore::Config::Load();
        FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");
        FEXCore::Config::Set(FEXCore::Config::CONFIG_ENABLECODECACHINGWIP, "0");
#if defined(VIXL_SIMULATOR)
        // The Darwin dependency defaults to native indirect runtime calls.
        // x86 CI's upstream ARM64 simulator must use its real C++ ABI wrappers.
        FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "0");
        // TSO loads are LDAPUR, which the simulator aborts on when a guest
        // access crosses 16 bytes (hardware faults and FEX fixes it up). Each
        // simulated access is an x86 host access, which keeps TSO ordering
        // between guest threads (scripts/patch-fexcore-simulator-threads.py).
        FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, "0");
        // The simulator offers SVE, which no iPhone has; match the device so
        // that AVX takes the same 128-bit pair path.
        FEXCore::Config::Set(FEXCore::Config::CONFIG_HOSTFEATURES,
            std::to_string(static_cast<uint64_t>(FEXCore::Config::HostFeatures::DISABLESVE)));
#endif
    }
#if !defined(VIXL_SIMULATOR)
    // Read by the context created below; the simulator always runs without it.
    FEXCore::Config::Set(FEXCore::Config::CONFIG_TSOENABLED, tso_emulation.load() ? "1" : "0");
    // Full x86 ordering, not FEX's faster defaults: vector loads/stores, REP
    // MOVS/STOS and split-line locked operations ordered and untorn as well.
    // Call of Duty's string references are counted by lock-free code; with the
    // defaults it looked up a string already freed and spun forever (default.elf
    // 0x5b1640), its table intact and every mutex call successful.
    const char* strict = tso_emulation.load() ? "1" : "0";
    FEXCore::Config::Set(FEXCore::Config::CONFIG_VECTORTSOENABLED, strict);
    FEXCore::Config::Set(FEXCore::Config::CONFIG_MEMCPYSETTSOENABLED, strict);
    FEXCore::Config::Set(FEXCore::Config::CONFIG_STRICTINPROCESSSPLITLOCKS, strict);
#endif
    // Real Darwin sysctl-based feature discovery; never fabricated features.
    const auto features = FEX::FetchHostFeatures();
    engine->context = FEXCore::Context::Context::CreateNewContext(features);
    if (!engine->context) {
        delete engine;
        release_config_if_unused();
        return -ENOMEM;
    }
    // InitCore is explicit; opening a context never enables guest execution.
    ++engines;
    *out = engine;
    return 0;
}
extern "C" void maxps4_fex_engine_close(maxps4_fex_engine *engine) {
    if (!engine) return;
    std::lock_guard lock(runtime_lock);
#if TARGET_OS_IPHONE
    const bool owned_pool = jit_owner == engine;
#endif
    delete engine; // Context dies under runtime_lock before another owner opens.
#if TARGET_OS_IPHONE
    if (owned_pool) jit_owner = nullptr;
#endif
    --engines;
    release_config_if_unused();
}
extern "C" int32_t maxps4_fex_engine_initialize(maxps4_fex_engine *engine) {
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    if (engine->stage == MAXPS4_FEX_DISPATCHER_INITIALIZED) return 0;
#if TARGET_OS_IPHONE
    if (jit_owner && jit_owner != engine) return -EBUSY;
    // Verify the current nonce, Mach W^X aliases and native canary before FEX
    // emits code into the prepared dispatcher/cache pool.
    const int32_t prepared = maxps4_jit_prepare();
    if (prepared != 0) {
        std::fprintf(stderr, "MaxPS4 FEX JIT: RX/RW preparation failed (%d).\n", prepared);
        return prepared;
    }
    // Retain ownership even after a partial InitCore failure until close.
    jit_owner = engine;
#endif
    engine->context->SetSignalDelegator(&engine->signals);
    // Only admitted HLT-terminated straight-line programs are exposed.
    engine->context->SetSyscallHandler(&engine->probe);
    engine->context->EnableExitOnHLT();
    errno = 0;
    try {
        if (!engine->context->InitCore()) return -(errno ? errno : ENOMEM);
    } catch (const std::bad_alloc&) { return -ENOMEM; }
    engine->stage = MAXPS4_FEX_DISPATCHER_INITIALIZED;
    return 0;
}
extern "C" int32_t maxps4_fex_engine_stage(const maxps4_fex_engine *engine) {
    std::lock_guard lock(runtime_lock);
    return engine ? engine->stage : 0;
}
extern "C" int32_t maxps4_fex_engine_last_cpu_report(const maxps4_fex_engine* engine,
    maxps4_fex_cpu_report* report) {
    if (!report) return -EINVAL;
    *report = {};
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    *report = engine->cpu_report;
    return report->completed ? 0 : -EAGAIN;
}
extern "C" int32_t maxps4_fex_engine_last_guest_fault(const maxps4_fex_engine* engine,
    maxps4_fex_guest_fault* fault) {
    if (!fault) return -EINVAL;
    *fault = {};
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    *fault = engine->last_fault;
    return fault->signal ? 0 : -ENOENT;
}
extern "C" int32_t maxps4_fex_engine_set_time_limit(maxps4_fex_engine* engine,
    uint32_t milliseconds) {
    if (!engine || milliseconds > 3600 * 1000) return -EINVAL;
    if (milliseconds && !GuestFaultGuard::available) return -ENOTSUP;
    std::lock_guard lock(runtime_lock);
    engine->time_limit_ms = milliseconds;
    return 0;
}
extern "C" int32_t maxps4_fex_engine_request_stop(maxps4_fex_engine* engine) {
    if (!engine) return -EINVAL;
    engine->probe.stop_asked_ns = ProbeAddressSpace::now_ns();
    engine->stop_requested=true;
    engine->libkernel.videoout().interrupt(); // Flip waits end at once.
    return 0;
}
extern "C" int32_t maxps4_fex_engine_cpuid(maxps4_fex_engine *engine,
                                          uint32_t leaf, uint32_t subleaf,
                                          uint32_t out[4]) {
    if (!engine || !out) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    const auto values = engine->context->RunCPUIDFunction(leaf, subleaf);
    out[0] = values.eax;
    out[1] = values.ebx;
    out[2] = values.ecx;
    out[3] = values.edx;
    return 0;
}
extern "C" int32_t maxps4_fex_engine_run(maxps4_fex_engine *engine,
                                        const uint8_t *code, size_t length,
                                        uint32_t budget, uint64_t *result,
                                        uint32_t *backend) {
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (!engine || !code || !length || length > 65536 || !budget ||
        budget > 1000000 || !result || !backend) return -EINVAL;
    if (maxps4_native_guest_x86_run(code, length, budget, result) != 1) {
        *result = 0;
        return -ENOTSUP;
    }
    *backend = MAXPS4_CPU_RESTRICTED_INTERPRETER;
    return 0;
}

extern "C" int32_t maxps4_fex_engine_probe(maxps4_fex_engine *engine,
                                          uint64_t *result, uint32_t *backend) {
    // Compatibility wrapper: even the legacy probe goes through the ELF loader.
    std::array<uint8_t, 131> file {};
    auto put = [&](uint64_t value, size_t at, size_t width) {
        for (size_t i = 0; i < width; ++i) file[at + i] = value >> (i * 8);
    };
    std::memcpy(file.data(), "\177ELF\2\1\1", 7);
    put(2,16,2); put(62,18,2); put(1,20,4); put(0x1000,24,8);
    put(64,32,8); put(64,52,2); put(56,54,2); put(1,56,2);
    put(1,64,4); put(5,68,4); put(120,72,8); put(0x1000,80,8);
    put(11,96,8); put(11,104,8); put(1,112,8);
    constexpr uint8_t code[] = {0xB8,40,0,0,0,0x05,2,0,0,0,0xF4};
    std::memcpy(file.data() + 120, code, sizeof(code));
    uint64_t entry = 0;
    return maxps4_fex_engine_run_elf(engine, file.data(), file.size(), 3, result, backend, &entry);
}

static std::string fault_registers(maxps4_fex_engine* engine, const std::array<uint64_t, 16>& gprs);
static std::string fault_memory(maxps4_fex_engine* engine, uint64_t address);
// The first faulting child thread, its exact instruction, registers and target memory.
static std::string fault_detail(maxps4_fex_engine* engine, const maxps4_fex_guest_fault& fault, uint64_t id,
                                const std::array<uint64_t, 16>& gprs) {
    char line[256];
    const uint64_t rip = fault.rip >= engine->relocation ? fault.rip - engine->relocation : fault.rip;
    std::snprintf(line, sizeof line, "Plantage : thread %llu, signal %d code %d adresse 0x%llx, instruction exacte 0x%llx, octets",
        (unsigned long long)id, fault.signal, fault.code, (unsigned long long)fault.address, (unsigned long long)rip);
    std::string detail = line;
    uint8_t bytes[8] {};
    if (engine->system_memory.read(fault.rip, bytes, sizeof bytes))
        for (const uint8_t b : bytes) { std::snprintf(line, sizeof line, " %02x", b); detail += line; }
    else detail += " illisibles";
    detail += fault_memory(engine, fault.address);
    detail += fault_registers(engine, gprs);
    return detail + "\n";
}
// Call of Duty: Modern Warfare Remastered's string table check (StringTableWatch),
// when its code is the one known: the lookup's lea of the table at default.elf
// 0x5b15eb. Other games and versions keep no watch.
static void watch_string_table(maxps4_fex_engine* engine) {
    engine->libkernel.watch_mutex(0, {});
    engine->table_watch.reset();
    const uint64_t base = engine->relocation + 0x2100000; // default.elf's load address.
    static constexpr uint8_t lookup[] = {0x4c, 0x8d, 0x25, 0x0e, 0xe9, 0x69, 0x0c};
    uint8_t bytes[sizeof lookup] = {};
    if (!engine->system_memory.read(base + 0x5b15eb, bytes, sizeof bytes) || std::memcmp(bytes, lookup, sizeof lookup)) return;
    engine->table_watch = std::make_unique<StringTableWatch>(engine->system_memory, base + 0xcc4ff00, base + 0xc847d28, engine->relocation);
    StringTableWatch* watch = engine->table_watch.get();
    const uint64_t relocation = engine->relocation;
    engine->libkernel.watch_mutex(base + 0xc0bc978, [watch, relocation](uint64_t thread, uint64_t caller, bool unlock,
                                                                       const maxps4::PS4KernelCall& call) {
        watch->event(thread, caller >= relocation ? caller - relocation : caller, unlock, call.r15, call.r14, call.r12, call.rbp, call.rsp);
    });
    watch->guard_string(0x14e, [engine, relocation](bool registers) {
        std::lock_guard sampling(engine->sample_lock);
        return engine->probe.threads ? engine->probe.threads->where_all(relocation, registers) : std::string(" ?");
    });
}
namespace {
struct PM4Request {
    uint64_t address;
    size_t count;
    uint32_t* words;
    size_t* written;
};
int32_t executeImage(maxps4_fex_engine* engine, maxps4::GuestELF& image,
    uint64_t* result, uint32_t* backend, const PM4Request* submission, bool linked = false, bool persistent_frame = false) {
    if (engine->stop_requested.load()) return -ECANCELED;
    if (engine->stage != MAXPS4_FEX_DISPATCHER_INITIALIZED) return -EAGAIN;
    // FEX still uses rebased host addresses for the admitted instruction set.
    // Check the logical PS4 entry via the shared guest-memory contract before
    // giving FEX a host pointer. This is not general address-space translation.
    maxps4::PS4GuestMemory guest_memory(image);
    if (guest_memory.contiguous(image.start(), 1, 5) != 1) return -EFAULT;
    // Fetch a bounded instruction window through the same permission-checked
    // guest contract used by CPU clients. Never cross the executable segment.
    // The decoder still enforces the admitted instruction whitelist.
    std::array<uint8_t, 15> entry_bytes {};
    const size_t fetch_length = guest_memory.contiguous(image.start(), entry_bytes.size(), 5);
    if (!fetch_length || !guest_memory.fetch(image.start(), entry_bytes.data(), fetch_length))
        return -EFAULT;
    const auto* entry_pointer = image.start_pointer();
    if (!entry_pointer) return -EFAULT;
    const long host_page = ::sysconf(_SC_PAGESIZE);
    if (host_page <= 0) return -EIO;
    const size_t page = static_cast<size_t>(host_page);
    Mapping stack(3 * page, PROT_NONE);
    if (stack.address == MAP_FAILED) return -errno;
    FEXThreadSetup setup(page);
    if (!setup.mapped()) return -errno;
    auto* stack_base = static_cast<uint8_t*>(stack.address) + page;
    if (::mprotect(stack_base, page, PROT_READ | PROT_WRITE) != 0) return -errno;
    engine->probe.image = &image;
    engine->probe.linker = linked ? &engine->linker : nullptr;
    // Orbis images with synthesized HLE trampolines also reach libkernel.
    engine->probe.kernel = linked || image.has_hle() ? &engine->libkernel : nullptr;
    engine->probe.memory = linked || image.has_hle() ? &engine->system_memory : nullptr;
    // Probe mode only: stubs report themselves; mode 1 also traces every HLE call.
    engine->probe.missing_imports = engine->import_probe ? &engine->missing_imports : nullptr;
    engine->probe.missing_reported = engine->import_probe ? &engine->missing_reported : nullptr;
    engine->probe.trace = engine->import_trace;
    engine->probe.private_stack = reinterpret_cast<uintptr_t>(stack_base);
    engine->probe.private_stack_size = page;
    engine->probe.calls = 0;
    engine->probe.callback_error = 0;
    for (auto& a : engine->probe.activity) { a.thread = 0; a.inside = false; a.returned = 0; }
    { std::lock_guard<std::mutex> guard(engine->probe.recent_lock); engine->probe.recent_next = 0; }
    struct ClearRange {
        ProbeAddressSpace& space;
        ~ClearRange() {
            space.image = nullptr; space.linker = nullptr; space.kernel = nullptr;
            space.memory = nullptr; space.private_stack = 0; space.private_stack_size = 0;
            space.threads = nullptr; space.missing_imports = nullptr; space.missing_reported = nullptr;
        }
    } clear_range {engine->probe};
    engine->cpu_report = {};
    uint64_t initial_rsp = 0;
    const uint64_t initial_rip = reinterpret_cast<uintptr_t>(entry_pointer);
    // A general image owns its stack inside guest memory, so stack buffers are
    // valid HLE pointer arguments; the bounded paths keep the private page.
    // Its startup stub then lays out the PS4 entry stack from EntryParams at
    // the stack top, as shadPS4 does; the bounded paths keep the SysV auxv.
    const bool ps4_entry = image.general();
    if (ps4_entry) {
        initial_rsp = image.write_entry_params();
        if (!initial_rsp) return -EFAULT;
    } else {
        size_t guest_stack_size = page;
        uint8_t* guest_stack = image.stack_host(guest_stack_size);
        if (!guest_stack) { guest_stack = stack_base; guest_stack_size = page; }
        const int stack_status = maxps4::initialize_guest_stack(guest_stack, guest_stack_size, initial_rip, page, initial_rsp);
        if (stack_status) return stack_status;
    }
    engine->probe.exited = false;
    engine->probe.exit_status = 0;
    auto* thread = engine->context->CreateThread(initial_rip, initial_rsp);
    if (!thread) return -ENOMEM;
    struct DestroyThread {
        FEXCore::Context::Context* context;
        FEXCore::Core::InternalThreadState* thread;
        ~DestroyThread() { context->DestroyThread(thread); }
    } destroy {engine->context.get(), thread};
    auto& state = thread->CurrentFrame->State;
    // Static TLS: FS base is the host address of the image's TCB (variant II).
    const auto* tcb = image.thread_pointer_host();
    if (const int prepared = setup.prepare(thread, initial_rip, initial_rsp, reinterpret_cast<uintptr_t>(tcb)))
        return prepared;
    auto& observed = engine->cpu_report;
    observed.entry = image.entry();
    observed.rsp_alignment = state.gregs[FEXCore::X86State::REG_RSP] & 15;
    observed.entry_matches = state.rip == initial_rip;
    observed.zero_integer_registers = 1;
    for (size_t i = 0; i < std::size(state.gregs); ++i)
        if (i != FEXCore::X86State::REG_RSP && state.gregs[i]) observed.zero_integer_registers = 0;
    if (ps4_entry) {
        uint32_t argc = 0;
        std::memcpy(&argc, reinterpret_cast<const void*>(initial_rsp), sizeof(argc));
        observed.argc = argc; // EntryParams; no SysV argv/envp/auxv.
    } else {
        uint64_t stack_words[9]{};
        std::memcpy(stack_words, reinterpret_cast<const void*>(initial_rsp), sizeof(stack_words));
        observed.argc = stack_words[0];
        observed.argv_env_null = stack_words[1] == 0 && stack_words[2] == 0;
        observed.page_size = stack_words[4];
        observed.auxv_valid = stack_words[3] == 6 && stack_words[5] == 9 &&
            stack_words[6] == state.rip && stack_words[7] == 0 && stack_words[8] == 0;
    }
    // mmap can reuse a previous image's address. The context retains a shared
    // L3 guest-to-host cache across threads: discard those old translations
    // without allocating another iOS JIT buffer. All engine execution is locked.
    if (!persistent_frame || !engine->frame_code_cached) engine->context->ClearCodeCache(thread, false);
    engine->frame_code_cached = persistent_frame;
    engine->last_fault = {};
    { std::lock_guard sampling(engine->sample_lock); engine->last_fault_detail.clear(); }
    {
        // General images may create guest threads; all are joined before the
        // guard (which owns the signal handlers) goes away.
        std::optional<GuestThreads> threads;
        if (ps4_entry && engine->probe.kernel)
            threads.emplace(engine->context.get(), engine->signals.GetConfig(), image,
                            engine->system_memory, engine->libkernel.threads(), page);
        engine->probe.threads = threads ? &*threads : nullptr;
        GuestFaultGuard guard(engine->context.get(), thread, engine->signals.GetConfig(), engine->time_limit_ms,
                              true, [&threads, engine] {
                                  engine->probe.stop_told_ns = ProbeAddressSpace::now_ns();
                                  if (threads) threads->interrupt();
                              }, &engine->stop_requested,
                              [engine,stack_base,page](uint64_t a,size_t n,uint32_t p) {
                                  if (engine->system_memory.permitted(a,n,p)) return true;
                                  const auto low=reinterpret_cast<uintptr_t>(stack_base);
                                  return a>=low && a-low<=page && n<=page-(a-low) && !(p&~6u);
                              });
        { std::lock_guard sampling(engine->sample_lock); engine->main_guard = &guard; }
        std::atomic<bool> profiled {false};
        std::thread profiler;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        if (engine->profiling.load()) {
            engine->profile.clear();
            profiler = std::thread([engine, &guard, &profiled] { engine->profile.run(guard, profiled); });
        }
#endif
        // The app runs this on a libdispatch worker thread, which blocks SIGURG:
        // the stop (and sampling) signal then never lands in this thread's JIT
        // code, and a game spinning there without HLE calls never stopped.
        sigset_t urgent, saved;
        sigemptyset(&urgent); sigaddset(&urgent, SIGURG);
        ::pthread_sigmask(SIG_UNBLOCK, &urgent, &saved);
        engine->context->ExecuteThread(thread);
        ::pthread_sigmask(SIG_SETMASK, &saved, nullptr);
        // Stopped while this thread (the sampled one) still exists.
        profiled = true;
        if (profiler.joinable()) profiler.join();
        { std::lock_guard sampling(engine->sample_lock); engine->main_guard = nullptr; }
        int signal = guard.signal.load();
        // scePthreadExit on the main thread: the other threads carry on.
        const bool main_exit = threads && !signal && threads->main_thread_exited();
        if (main_exit) threads->wait();
        maxps4_fex_guest_fault child {};
        if (threads) threads->finish();
        { std::lock_guard sampling(engine->sample_lock); engine->probe.threads = nullptr; }
        if (main_exit && threads->expired() && !engine->probe.exited) signal = SIGURG;
        // A child's exit function stops the main thread with SIGURG: not a timeout.
        if (signal == SIGURG && engine->probe.exited) signal = 0;
        // RIP is the guest position FEX last stored (block granularity).
        if (threads && threads->faulted(child)) {
            engine->last_fault = child; signal = child.signal;
            uint64_t id = 0;
            std::array<uint64_t, 16> gprs {};
            threads->fault_thread(id, gprs);
            std::string detail = fault_detail(engine, child, id, gprs);
            std::lock_guard sampling(engine->sample_lock);
            engine->last_fault_detail = std::move(detail);
        }
        else if (signal) {
            // The main thread (1) faulted: its exact instruction when the fault
            // was in JIT code, and its guest registers as FEX spilled them.
            engine->last_fault = {signal, guard.code, guard.address, guard.fault_rip ? guard.fault_rip : state.rip};
            if (signal != SIGURG) {
                std::array<uint64_t, 16> gprs {};
                std::copy_n(std::begin(state.gregs), 16, gprs.begin());
                std::string detail = fault_detail(engine, engine->last_fault, 1, gprs);
                std::lock_guard sampling(engine->sample_lock);
                engine->last_fault_detail = std::move(detail);
            }
        }
        if (signal) {
            engine->frame_code_cached = false;
            return signal == SIGURG ? (engine->stop_requested.load() ? -ECANCELED : -ETIMEDOUT) :
                signal == SIGSEGV || signal == SIGBUS ? -EFAULT : -EILSEQ;
        }
        // The last thread ended after the main thread's scePthreadExit: exit(0).
        if (main_exit) engine->probe.exited = true;
    }
    if (engine->probe.callback_error) return engine->probe.callback_error;
    if (submission) {
        const auto snapshot = maxps4::gpu::snapshotPM4(image, submission->address, submission->count);
        if (snapshot.error) return snapshot.error;
        std::memcpy(submission->words, snapshot.words.data(), snapshot.words.size() * sizeof(uint32_t));
        *submission->written = snapshot.words.size();
    }
    *result = state.gregs[FEXCore::X86State::REG_RAX];
    observed.rax = *result;
    observed.exited = engine->probe.exited ? 1 : 0;
    observed.exit_status = engine->probe.exit_status;
    observed.completed = 1;
#if defined(VIXL_SIMULATOR)
    *backend = MAXPS4_CPU_FEX_ARM64_SIMULATOR;
#else
    *backend = MAXPS4_CPU_FEX_ARM64_JIT;
#endif
    return 0;
}
int32_t runELF(maxps4_fex_engine *engine,
    const uint8_t *file, size_t length, uint32_t budget, uint64_t *result,
    uint32_t *backend, uint64_t *entry, const PM4Request* submission) {
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (entry) *entry = 0;
    if (!engine || !result || !backend || !entry) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    // iOS uses the bounded Darwin ELF execution path after verified InitCore.
    try {
    maxps4::GuestELF image;
    const int loaded = image.load(file, length, budget);
    if (loaded) return loaded;
    *entry = image.entry();
    if (submission) {
        if ((submission->address & 3U) ||
            submission->address > UINT64_MAX - submission->count * 4) return -EINVAL;
        if (!image.translate(submission->address, submission->count * 4, 4)) return -EFAULT;
        // The CPU may produce PM4 in initially zero-filled RW data. Interpret
        // only after FEX completes; range/permissions are checked before execution.
    }
    return executeImage(engine, image, result, backend, submission);
    } catch (const std::bad_alloc&) { return -ENOMEM; }
}


} // namespace
extern "C" int32_t maxps4_fex_engine_stage_ps4(maxps4_fex_engine* engine,
    const uint8_t* file, size_t length, uint64_t* entry, uint32_t* segments, uint32_t* blockers) {
    if (entry) *entry = 0;
    if (segments) *segments = 0;
    if (blockers) *blockers = 0;
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->libkernel.reset();
    engine->linker.reset();
    engine->staged_ps4.clear();
    engine->orbis_admitted = false; engine->frame = {}; engine->frame_sequence = 0; engine->frame_code_cached = false;
    if (!entry || !segments || !blockers) return -EINVAL;
    const int error = engine->staged_ps4.stage_ps4(file, length);
    if (error) return error;
    *entry = engine->staged_ps4.entry();
    *segments = static_cast<uint32_t>(engine->staged_ps4.segments().size());
    *blockers = engine->staged_ps4.ps4_blockers();
    std::fprintf(stderr, "MaxPS4 PS4 native staging: entry=0x%llx segments=%u blockers=0x%x; CPU not dispatched\n",
        static_cast<unsigned long long>(*entry), *segments, *blockers);
    return 0;
}
extern "C" int32_t maxps4_fex_engine_link_ps4(maxps4_fex_engine* engine,
    const uint8_t* file, size_t length, uint32_t budget, uint64_t* entry, uint32_t* imports) {
    if (entry) *entry = 0;
    if (imports) *imports = 0;
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->libkernel.reset();
    engine->dynamic_ps4.reset();
    engine->linker.reset();
    engine->staged_ps4.clear();
    engine->orbis_admitted = false; engine->frame = {}; engine->frame_sequence = 0; engine->frame_code_cached = false;
    if (!entry || !imports) return -EINVAL;
    const int error = engine->linker.load(engine->staged_ps4,file,length,budget);
    if (error) return error;
    *entry = engine->staged_ps4.entry();
    *imports = static_cast<uint32_t>(engine->linker.imports().size());
    return 0;
}
extern "C" int32_t maxps4_fex_engine_run_linked_ps4(maxps4_fex_engine* engine,
    uint64_t* result, uint32_t* backend, uint64_t* entry, uint32_t* calls) {
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (entry) *entry = 0;
    if (calls) *calls = 0;
    if (!engine || !result || !backend || !entry || !calls) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    if (engine->linker.imports().empty()) return -EFAULT;
    try {
        const int error = executeImage(engine,engine->staged_ps4,result,backend,nullptr,true);
        if (error) return error;
    } catch (const std::bad_alloc&) { return -ENOMEM; }
    *entry = engine->staged_ps4.entry();
    *calls = engine->probe.calls;
    return 0;
}
namespace {
// Validates the C module table into linker inputs; libkernel is provided by
// HLE: one synthesized trampoline per implemented NID, identity
// libkernel/libkernel version 1 (as resolve_libkernel). Services only general
// images get are offered when `general`; scePthreadExit's trampoline ends
// with HLT: FEX goes on after a SYSCALL whatever the handler sets in RIP.
// A SELF with plaintext blocks (homebrew fSELF) is linked as its plain ELF,
// kept in `plain` until the image is prepared; an encrypted one is refused
// with `why`.
int orbisInputs(const maxps4_orbis_module* modules, size_t count, bool general,
    std::vector<std::vector<uint8_t>>& plain, std::string& why,
    std::vector<maxps4::OrbisLinker::Input>& inputs, std::vector<maxps4::OrbisLinker::HLE>& hle,
    std::vector<std::string>* missing = nullptr) {
    for (size_t i=0;i<count;++i) {
        const auto& m = modules[i];
        if (!m.path || !m.bytes || m.dependency_count > 128 ||
            (m.dependency_count && !m.dependencies)) return -EINVAL;
        if (::strnlen(m.path,256)>255 || m.size>maxps4::OrbisMetadata::fileLimit) return -E2BIG;
        const uint8_t* bytes = m.bytes;
        size_t size = m.size;
        std::vector<uint8_t> elf;
        const int unwrapped = maxps4::SelfImage::unwrap(bytes,size,elf,why);
        if (unwrapped < 0) { why = std::string(m.path) + ": " + why; return unwrapped; }
        if (unwrapped) {
            plain.push_back(std::move(elf)); // Moving keeps the buffer where it is.
            bytes = plain.back().data();
            size = plain.back().size();
        }
        maxps4::OrbisLinker::Input in{{{m.path,{}},bytes,size},m.load_bias};
        for (size_t j=0;j<m.dependency_count;++j) {
            if (!m.dependencies[j] || ::strnlen(m.dependencies[j],256)>255) return -EINVAL;
            in.file.request.dependencies.emplace_back(m.dependencies[j]);
        }
        inputs.push_back(std::move(in));
    }
    size_t exported = 0;
    const auto* kernel_exports = maxps4::libkernel_exports(exported);
    for (size_t i=0;i<exported;++i) {
        const auto& e = kernel_exports[i];
        // Data objects and guest routines serve the libc of general images.
        if (!general && (maxps4::general_only(e.service) || e.data || e.code)) continue;
        maxps4::OrbisLinker::Identity id{e.nid,e.library,e.module ? e.module : e.library,1,1,uint8_t(e.data ? 1 : 2)};
        if (e.data) { hle.push_back({id,0,e.data}); continue; }
        // libScePosix, a library of the libkernel module, exports libkernel's
        // functions under the same NIDs (Unreal Engine games import
        // pthread_mutex_init and others there).
        auto posix_alias = [&](maxps4::OrbisLinker::HLE h) {
            if (std::strcmp(e.library, "libkernel")) return;
            h.identity.library = "libScePosix";
            hle.push_back(std::move(h));
        };
        if (e.code) {
            hle.push_back({id,0,0,false,std::vector<uint8_t>(e.code,e.code+e.code_size)});
            posix_alias(hle.back());
            continue;
        }
        const uint32_t token = static_cast<uint32_t>(e.service) | (e.posix ? maxps4::posix_token : 0) |
            (!e.posix && !std::strncmp(e.name, "pthread_", 8) ? maxps4::pthread_token : 0);
        hle.push_back({id,0,token,e.noreturn || e.service == maxps4::PS4KernelService::thread_exit});
        posix_alias(hle.back());
    }
    if (!missing) return 0;
    // Boot probe: stub what neither the given modules' libraries nor the HLE
    // above provide, with the import's own identity.
    std::vector<maxps4::OrbisMetadata> metadata(count);
    std::set<std::string> exported_libraries;
    for (size_t i=0;i<count;++i) {
        if (!metadata[i].parse(modules[i].bytes,modules[i].size)) {
            why = std::string(modules[i].path) + ": " + metadata[i].error;
            return -ENOEXEC;
        }
        for (const auto& l : metadata[i].libraries) if (l.exported) exported_libraries.insert(l.name);
    }
    // libSceLibcInternal imports that libc.prx exports are linked to it (OrbisLinker).
    std::set<std::string> libc_nids;
    for (const auto& m : metadata)
        for (const auto& s : m.symbols) if (s.defined && s.library == "libc") libc_nids.insert(s.nid);
    // The linker matches library and module: an import the HLE lists under
    // another module still needs its stub.
    std::set<std::tuple<std::string,std::string,std::string>> provided;
    for (const auto& h : hle) provided.insert({h.identity.nid,h.identity.library,h.identity.module});
    std::set<maxps4::OrbisLinker::Identity> stubs;
    for (const auto& m : metadata)
        for (const auto& s : m.symbols)
            if (!s.defined && !s.nid.empty() && (s.kind == 1 || s.kind == 2) &&
                !exported_libraries.count(s.library) && !provided.count({s.nid,s.library,s.module}) &&
                !(s.library == "libSceLibcInternal" && libc_nids.count(s.nid)))
                stubs.insert({s.nid,s.library,s.module,s.libraryVersion,s.moduleVersion,s.kind});
    for (const auto& id : stubs) {
        if (id.type == 1) { hle.push_back({id,0,64}); continue; }
        hle.push_back({id,0,maxps4::missing_import_token + uint32_t(missing->size())});
        missing->push_back(id.library + " " + id.nid);
    }
    return 0;
}
}
extern "C" int32_t maxps4_fex_engine_prepare_orbis_general(maxps4_fex_engine* engine,
    const maxps4_orbis_module* modules, size_t count, const char* root,
    uint64_t* relocation, char* diagnostic, size_t capacity) {
    if (diagnostic && capacity) diagnostic[0] = 0;
    if (relocation) *relocation = 0;
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->libkernel.reset(); engine->dynamic_ps4.reset(); engine->linker.reset();
    engine->staged_ps4.clear(); engine->orbis_admitted = false; engine->frame = {}; engine->frame_sequence = 0; engine->frame_code_cached = false;
    auto message = [&](const std::string& text) {
        if (diagnostic && capacity) std::snprintf(diagnostic,capacity,"%s",text.c_str());
    };
    if (!modules || !count || count > 128 || !root || !relocation || !diagnostic || capacity < 2)
        return -EINVAL;
    if (::strnlen(root,256)>255) return -E2BIG;
    try {
        std::vector<maxps4::OrbisLinker::Input> inputs;
        std::vector<maxps4::OrbisLinker::HLE> hle;
        std::vector<std::vector<uint8_t>> plain;
        std::string why;
        std::vector<std::string> missing;
        const int collected = orbisInputs(modules,count,true,plain,why,inputs,hle,
            engine->import_probe ? &missing : nullptr);
        if (collected) { message(why); return collected; }
        engine->missing_imports = std::move(missing);
        engine->missing_reported = std::vector<std::atomic<uint32_t>>(engine->missing_imports.size());
        std::string reason;
        uint64_t moved = 0;
        int status = engine->staged_ps4.prepare_orbis_general(inputs,hle,root,reason,moved);
        if (!status) {
            // HLE data objects (stack guard, __progname, errno cells) and the
            // process parameters libc asks libkernel for.
            size_t exported = 0;
            const auto* kernel_exports = maxps4::libkernel_exports(exported);
            for (const auto& object : engine->staged_ps4.data_objects())
                for (size_t i = 0; i < exported; ++i)
                    if (kernel_exports[i].data && object.nid == kernel_exports[i].nid &&
                        !engine->libkernel.initialize_data(kernel_exports[i].service, object.address, object.size)) {
                        reason = "HLE data object " + object.nid + " could not be initialized";
                        status = -EFAULT;
                    }
            engine->libkernel.set_process_param(engine->staged_ps4.process_param());
            std::vector<maxps4::PS4Libkernel::LoadedModule> loaded;
            for (const auto& record : engine->staged_ps4.module_records()) {
                maxps4::PS4Libkernel::LoadedModule module{record.name, record.base, record.end,
                    record.eh_frame_hdr, record.eh_frame_hdr_size, {}, {}};
                for (const auto& [nid, address] : record.exports) module.exports.emplace(nid, address);
                for (const auto& s : record.segments) module.segments.push_back({s.address, s.size, s.protection});
                loaded.push_back(std::move(module));
            }
            engine->libkernel.set_modules(std::move(loaded));
            // A game's libc writes into the objects behind its mutexes and
            // conditions (PS4Threads::set_object_arena): give them memory.
            constexpr uint64_t arena = 4 * 1024 * 1024;
            uint64_t objects = 0;
            if (!status && engine->dynamic_ps4.place(0, arena, 0, 3, false, objects)) {
                reason = "synchronization object arena could not be mapped";
                status = -ENOMEM;
            }
            if (!status) engine->libkernel.threads().set_object_arena(objects, arena);
            if (status) engine->staged_ps4.clear();
        }
        message(status ? reason : "SCE image linked and identity-mapped; execution not attempted");
        engine->orbis_admitted = status == 0;
        if (!status) *relocation = engine->relocation = moved;
        if (!status) watch_string_table(engine);
        return status;
    } catch (const std::bad_alloc&) { message("Orbis allocation failed"); return -ENOMEM; }
}
extern "C" int32_t maxps4_fex_engine_set_import_probe(maxps4_fex_engine* engine, uint32_t enabled) {
    if (!engine || enabled > 2) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->import_probe = enabled != 0;
    engine->import_trace = enabled == 1;
    return 0;
}
extern "C" int32_t maxps4_fex_engine_prepare_orbis(maxps4_fex_engine* engine,
    const maxps4_orbis_module* modules, size_t count, const char* root,
    uint32_t budget, char* diagnostic, size_t capacity) {
    if (diagnostic && capacity) diagnostic[0] = 0;
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->libkernel.reset(); engine->dynamic_ps4.reset(); engine->linker.reset();
    engine->staged_ps4.clear(); engine->orbis_admitted = false; engine->frame = {}; engine->frame_sequence = 0; engine->frame_code_cached = false;
    auto message = [&](const std::string& text) {
        if (diagnostic && capacity) std::snprintf(diagnostic,capacity,"%s",text.c_str());
    };
    if (!modules || !count || count > 128 || !root || !diagnostic || capacity < 2)
        return -EINVAL;
    if (::strnlen(root,256)>255) return -E2BIG;
    try {
        std::vector<maxps4::OrbisLinker::Input> inputs;
        std::vector<maxps4::OrbisLinker::HLE> hle;
        std::vector<std::vector<uint8_t>> plain;
        std::string why;
        const int collected = orbisInputs(modules,count,false,plain,why,inputs,hle);
        if (collected) { message(why); return collected; }
        maxps4::OrbisLinker::Image linked;
        std::string reason;
        if (!maxps4::OrbisLinker::link(inputs,hle,linked,reason)) {
            message(reason); return -ENOEXEC;
        }
        const int status = engine->staged_ps4.prepare_orbis(linked,root,budget,reason);
        message(status ? reason : "SCE image linked, mapped and CPU-admitted; execution not attempted");
        engine->orbis_admitted = status == 0;
        return status;
    } catch (const std::bad_alloc&) { message("Orbis allocation failed"); return -ENOMEM; }
}
extern "C" int32_t maxps4_fex_engine_configure_frame(maxps4_fex_engine* engine,
    uint64_t pixels, uint64_t input, uint32_t width, uint32_t height) {
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->frame = {}; engine->frame_sequence = 0; engine->frame_code_cached = false;
    if (!engine->orbis_admitted) return -EFAULT;
    const maxps4::GuestFrame candidate{pixels,input,width,height};
    const int status = candidate.validate(engine->staged_ps4);
    if (!status) engine->frame = candidate;
    return status;
}
extern "C" int32_t maxps4_fex_engine_step_frame(maxps4_fex_engine* engine,
    uint32_t buttons, uint8_t* pixels, size_t capacity, uint64_t* sequence, uint32_t* backend) {
    if (sequence) *sequence = 0;
    if (backend) *backend = 0;
    if (!engine || !pixels || !sequence || !backend) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    if (!engine->orbis_admitted) return -EFAULT;
    // Validate every output before dispatch: failures never publish partial frames.
    const int valid = engine->frame.validate(engine->staged_ps4);
    if (valid) return valid;
    if (capacity != engine->frame.bytes() || (buttons & ~31u)) return -EINVAL;
    if (engine->stage != MAXPS4_FEX_DISPATCHER_INITIALIZED) return -EAGAIN;
    try {
        const int input_status = engine->frame.setInput(engine->staged_ps4,buttons);
        if (input_status) return input_status;
        uint64_t result = 0; uint32_t cpu = 0;
        const int status = executeImage(engine,engine->staged_ps4,&result,&cpu,nullptr,false,true);
        if (status) { engine->orbis_admitted = false; engine->frame = {}; return status; }
        const int copied = engine->frame.snapshot(engine->staged_ps4,pixels,capacity);
        if (copied) return copied;
        *sequence = ++engine->frame_sequence; *backend = cpu;
        return 0;
    } catch (const std::bad_alloc&) {
        engine->orbis_admitted = false; engine->frame = {}; return -ENOMEM;
    }
}
extern "C" int32_t maxps4_fex_engine_v4_preflight(maxps4_fex_engine* engine,
    uint64_t entry, uint64_t stack, size_t stack_bytes) {
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    if (engine->stage != MAXPS4_FEX_DISPATCHER_INITIALIZED) return -EAGAIN;
    const uint8_t* executable = nullptr;
    const uint8_t* writable_stack = nullptr;
    return engine->v4_execution_gate.preflight(entry,stack,stack_bytes,
                                               executable,writable_stack);
}
extern "C" int32_t maxps4_fex_engine_run_orbis(maxps4_fex_engine* engine,
    uint64_t* result, uint32_t* backend, uint64_t* entry) {
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (entry) *entry = 0;
    if (!engine || !result || !backend || !entry) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    if (!engine->orbis_admitted) return -EFAULT;
    try {
        const int status = executeImage(engine,engine->staged_ps4,result,backend,nullptr);
        if (!status) *entry = engine->staged_ps4.entry();
        return status;
    } catch (const std::bad_alloc&) { return -ENOMEM; }
}
extern "C" int32_t maxps4_fex_test_ps4_libkernel(maxps4_ps4_elf_test_report* report) {
    if (!report) return -EINVAL;
    *report = {};
    report->open_status = report->load_status = report->link_status =
        report->initialize_status = report->execute_status = report->read_status =
        report->verification_status = -ECANCELED;
    report->jit_regions_before = maxps4_jit_leases();
    maxps4_fex_engine* engine = nullptr;
    // Scope owns the engine even when fixture construction throws. The public
    // execution APIs remain the sole path to CPU dispatch and kernel services.
    int32_t status = [&]() -> int32_t {
        auto fail = [&](int32_t code, const char* operation, const char* detail = "") {
            std::snprintf(report->error, sizeof(report->error), "%s: %d %s", operation, code, detail);
            return code;
        };
        try {
            const auto file = maxps4_ps4_synthetic::minimal();
            report->open_status = maxps4_fex_engine_open(&engine);
            if (report->open_status) return fail(report->open_status, "open FEX");
            uint32_t blockers = 0;
            report->load_status = maxps4_fex_engine_stage_ps4(engine, file.data(), file.size(),
                &report->entry, &report->segments, &blockers);
            if (report->load_status) return fail(report->load_status, "load Orbis ELF");
            uint64_t linked_entry = 0;
            report->link_status = maxps4_fex_engine_link_ps4(engine, file.data(), file.size(),
                10, &linked_entry, &report->imports);
            {
                std::lock_guard lock(runtime_lock);
                if (report->link_status)
                    return fail(report->link_status, "link NID/RELA", engine->linker.diagnostic().c_str());
                if (engine->linker.imports().size() == 1)
                    report->resolved_service = static_cast<uint32_t>(engine->linker.imports()[0].service);
            }
            const auto self = maxps4_ps4_resolve_libkernel("aI+OeCz8xrQ", "libkernel", "libkernel", 1);
            if (!self || report->resolved_service != self || linked_entry != report->entry)
                return fail(-ENOENT, "resolve scePthreadSelf NID");
            report->initialize_status = maxps4_fex_engine_initialize(engine);
            if (report->initialize_status) return fail(report->initialize_status, "initialize FEX ARM64 JIT");
            uint64_t executed_entry = 0;
            report->execute_status = maxps4_fex_engine_run_linked_ps4(engine,
                &report->rax, &report->backend, &executed_entry, &report->calls);
            if (report->execute_status) return fail(report->execute_status, "execute linked ELF");
            uint8_t output[16]{};
            report->read_status = maxps4_fex_engine_read_ps4(engine,
                maxps4_ps4_synthetic::data + 0x100, output, sizeof(output));
            if (report->read_status) return fail(report->read_status, "read guest memory");
            for (size_t i = 0; i < 8; ++i) {
                report->final_memory |= uint64_t(output[i]) << (8 * i);
                report->kernel_return |= uint64_t(output[8 + i]) << (8 * i);
            }
            // Both stores follow CALL/RET. The second value is observed before
            // ADD, the first after ADD. Neither is assigned by this harness.
            report->resumed = report->calls == 1 && report->kernel_return == 1 &&
                report->final_memory == 2 && report->rax == 2 && executed_entry == report->entry;
            report->verification_status = report->resumed &&
                (report->backend == MAXPS4_CPU_FEX_ARM64_JIT ||
                 report->backend == MAXPS4_CPU_FEX_ARM64_SIMULATOR) ? 0 : -EIO;
            if (report->verification_status) return fail(report->verification_status, "verify call/return/stores");
            return 0;
        } catch (const std::bad_alloc&) { return fail(-ENOMEM, "allocate ELF/FEX"); }
    }();
    if (engine) {
        {
            std::lock_guard lock(runtime_lock);
            engine->libkernel.reset();
            engine->dynamic_ps4.reset();
            engine->linker.reset();
            engine->staged_ps4.clear();
            engine->orbis_admitted = false; engine->frame = {}; engine->frame_sequence = 0; engine->frame_code_cached = false;
            report->guest_segments_after = static_cast<uint32_t>(engine->staged_ps4.segments().size());
        }
        maxps4_fex_engine_close(engine);
    }
    report->jit_regions_after = maxps4_jit_leases();
    if (!status && (report->guest_segments_after || report->jit_regions_after != report->jit_regions_before)) {
        status = -EBUSY;
        std::snprintf(report->error, sizeof(report->error), "cleanup: guest segments=%u, RX/RW leases=%u (before=%u)",
            report->guest_segments_after, report->jit_regions_after, report->jit_regions_before);
    }
    return status;
}
extern "C" int32_t maxps4_fex_engine_run_staged_ps4(maxps4_fex_engine* engine,
    uint32_t budget, uint64_t* result, uint32_t* backend, uint64_t* entry) {
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (entry) *entry = 0;
    if (!engine || !result || !backend || !entry) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    try {
        const int admitted = engine->staged_ps4.admit_staged_cpu(budget);
        if (admitted) return admitted;
        const int status = executeImage(engine, engine->staged_ps4, result, backend, nullptr);
        if (!status) *entry = engine->staged_ps4.entry();
        return status;
    } catch (const std::bad_alloc&) { return -ENOMEM; }
}
extern "C" int64_t maxps4_fex_engine_mount_app0(maxps4_fex_engine* engine, const char* directory) {
    if (!engine) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    std::lock_guard lock(runtime_lock);
    if (engine->staged_ps4.segments().empty()) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    return engine->libkernel.mount_app0(directory);
}
extern "C" int32_t maxps4_fex_engine_set_update(maxps4_fex_engine* engine, const char* directory) {
    if (!engine) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->update_directory = directory ? directory : "";
    return 0;
}
extern "C" int32_t maxps4_fex_engine_load_game(maxps4_fex_engine* engine, const char* directory,
    uint64_t* relocation, char* diagnostic, size_t capacity) {
    if (relocation) *relocation = 0;
    if (diagnostic && capacity) diagnostic[0] = 0;
    if (!engine || !directory) return -EINVAL;
    auto report = [&](const std::string& text) {
        if (diagnostic && capacity) { const size_t n = std::min(capacity - 1, text.size()); std::memcpy(diagnostic, text.data(), n); diagnostic[n] = 0; }
    };
    struct Module { std::string name; std::vector<uint8_t> bytes; maxps4::OrbisMetadata metadata;
                    std::vector<std::string> dependencies; std::vector<const char*> pointers; };
    try {
        const std::filesystem::path root(directory), update(engine->update_directory);
        std::vector<std::filesystem::path> files;
        std::error_code error;
        // The update's modules replace the game's of the same name and add its own.
        std::map<std::string, std::filesystem::path> prx;
        for (const auto& folder : {root, update}) {
            if (folder.empty()) continue;
            std::error_code missing;
            for (const auto& entry : std::filesystem::directory_iterator(folder / "sce_module", missing))
                if (entry.path().extension() == ".prx") prx[entry.path().filename().string()] = entry.path();
        }
        for (const auto& [name, path] : prx) files.push_back(path);
        std::filesystem::path program = !update.empty() && std::filesystem::exists(update / "eboot.bin", error) ? update / "eboot.bin" : root / "eboot.bin";
        bool launched = false;
        // No sceSystemServiceLoadExec: a launcher is replaced by what it would start.
        {
            std::ifstream in(program, std::ios::binary);
            const std::vector<uint8_t> launcher{std::istreambuf_iterator<char>(in), {}};
            if (maxps4::orbisLauncherOfDefaultElf(launcher.data(), launcher.size())) {
                const auto next = program.parent_path() / "default.elf";
                if (std::filesystem::exists(next, error)) { program = next; launched = true; }
                else if (std::filesystem::exists(root / "default.elf", error)) { program = root / "default.elf"; launched = true; }
            }
        }
        const std::string programName = program.filename().string();
        files.push_back(program);
        // Providers first (the linker wants a module's dependencies before it).
        std::vector<Module> modules(files.size());
        for (size_t i = 0; i < files.size(); ++i) {
            std::ifstream in(files[i], std::ios::binary);
            if (!in) { report(files[i].filename().string() + ": unreadable"); return -ENOENT; }
            modules[i].bytes.assign(std::istreambuf_iterator<char>(in), {});
            modules[i].name = files[i].filename().string();
            if (!modules[i].metadata.parse(modules[i].bytes.data(), modules[i].bytes.size())) {
                report(modules[i].name + ": " + modules[i].metadata.error);
                return -ENOEXEC;
            }
        }
        // 1 MiB aligned, 1 MiB apart, as ps4-orbis-analyze places them.
        std::vector<maxps4_orbis_module> table;
        uint64_t bias = 0x400000;
        for (auto& m : modules) {
            for (const auto& needed : m.metadata.needed)
                for (const auto& other : modules)
                    if (&other != &m && other.name == needed) m.dependencies.push_back(needed);
            // libSceLibcInternal imports link to libc.prx (OrbisLinker), which
            // must be loaded and initialized first, as the system module is.
            const bool internal = std::any_of(m.metadata.libraries.begin(), m.metadata.libraries.end(),
                [](const auto& l) { return !l.exported && l.name == "libSceLibcInternal"; });
            if (internal && m.name != "libc.prx" &&
                std::find(m.dependencies.begin(), m.dependencies.end(), "libc.prx") == m.dependencies.end())
                for (const auto& other : modules)
                    if (other.name == "libc.prx") {
                        // Unless libc.prx needs this module, directly or not (Fios2).
                        std::set<std::string> seen;
                        std::vector<std::string> pending(other.metadata.needed.begin(), other.metadata.needed.end());
                        bool cycle = false;
                        while (!pending.empty() && !cycle) {
                            const std::string name = pending.back(); pending.pop_back();
                            if (!seen.insert(name).second) continue;
                            cycle = name == m.name;
                            for (const auto& next : modules)
                                if (next.name == name) pending.insert(pending.end(), next.metadata.needed.begin(), next.metadata.needed.end());
                        }
                        if (!cycle) m.dependencies.push_back(other.name);
                        break;
                    }
            for (const auto& d : m.dependencies) m.pointers.push_back(d.c_str());
            table.push_back({m.name.c_str(), m.bytes.data(), m.bytes.size(), bias, m.pointers.data(), m.pointers.size()});
            uint64_t end = 0;
            for (const auto& s : m.metadata.segments)
                if (s.type == 1 || s.type == 0x61000010) end = std::max(end, s.address + s.memsz);
            bias += ((end + 0xfffff) & ~uint64_t{0xfffff}) + 0x100000;
        }
        const int32_t prepared = maxps4_fex_engine_prepare_orbis_general(engine, table.data(), table.size(),
                                                                         programName.c_str(), relocation, diagnostic, capacity);
        if (prepared) return prepared;
        if (launched) engine->staged_ps4.start_without_program_name();
        const int64_t mounted = maxps4_fex_engine_mount_app0(engine, root.string().c_str());
        if (mounted) return int32_t(mounted);
        if (!update.empty()) {
            const int64_t overlaid = engine->libkernel.mount_app0_update(update.string().c_str());
            if (overlaid) { report("update: not readable"); return int32_t(overlaid); }
        }
        return 0;
    } catch (const std::bad_alloc&) { return -ENOMEM; }
    catch (const std::filesystem::filesystem_error&) { report("directory not readable"); return -ENOENT; }
}
extern "C" int64_t maxps4_fex_engine_mount(maxps4_fex_engine* engine, const char* guest, const char* directory) {
    if (!engine) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    std::lock_guard lock(runtime_lock);
    if (engine->staged_ps4.segments().empty()) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    return engine->libkernel.mount(guest, directory);
}
extern "C" int32_t maxps4_fex_engine_memory_layout(maxps4_fex_engine* engine, uint64_t* app_area,
    uint64_t* app_area_size, uint64_t* window, uint64_t* window_size, int32_t* app_area_status) {
    if (!engine || !app_area || !app_area_size || !window || !window_size || !app_area_status) return -EINVAL;
    int status = 0;
    maxps4::PS4VirtualMemory::app_area(*app_area, *app_area_size, status);
    *app_area_status = status;
    if (!engine->staged_ps4.heap(*window, *window_size)) *window = *window_size = 0;
    return 0;
}
extern "C" uint64_t maxps4_fex_engine_spare_address_space(maxps4_fex_engine* engine) {
    return engine ? engine->staged_ps4.spare_address_space() : 0;
}
extern "C" int32_t maxps4_fex_engine_console(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    std::string out = engine->libkernel.console();
    if (out.size() >= capacity) out.erase(0, out.size() - (capacity - 1));
    std::memcpy(text, out.data(), out.size()); text[out.size()] = 0;
    return int32_t(out.size());
}
extern "C" int32_t maxps4_fex_engine_moved_mappings(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    std::array<maxps4::PS4VirtualMemory::Moved, maxps4::PS4VirtualMemory::moved_limit> moved {};
    size_t count = 0;
    const size_t total = engine->dynamic_ps4.moved(moved, count);
    std::string out;
    char line[128];
    for (size_t i = 0; i < count; ++i) {
        std::snprintf(line, sizeof(line), "0x%llx + 0x%llx -> 0x%llx (code %d)\n",
            (unsigned long long)moved[i].hint, (unsigned long long)moved[i].size,
            (unsigned long long)moved[i].result, moved[i].status);
        if (out.size() + std::strlen(line) >= capacity) break;
        out += line;
    }
    std::memcpy(text, out.data(), out.size()); text[out.size()] = 0;
    return int32_t(std::min<size_t>(total, INT32_MAX));
}
// One call per line, oldest first, from a ring of RecentCall (under recent_lock).
template <size_t N>
static int32_t format_calls(maxps4_fex_engine* engine, const std::array<ProbeAddressSpace::RecentCall, N>& ring,
                            uint64_t next, char* text, size_t capacity) {
    const uint64_t count = std::min<uint64_t>(next, N);
    std::string out;
    char line[256];
    for (uint64_t i = next - count; i < next; ++i) {
        const auto& c = ring[i % N];
        const auto service = static_cast<maxps4::PS4KernelService>(c.service);
        std::string name;
        if (c.service >= maxps4::missing_import_token) {
            const uint32_t stub = c.service - maxps4::missing_import_token;
            name = stub < engine->missing_imports.size() ? "(absente) " + engine->missing_imports[stub] : "(absente)";
        } else {
            const char* known = service_name(service);
            name = known ? known : "service " + std::to_string(c.service);
        }
        std::snprintf(line, sizeof(line), "t%llu %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%llx%s%s%s\n",
            (unsigned long long)c.thread, name.c_str(), (unsigned long long)c.a0, (unsigned long long)c.a1,
            (unsigned long long)c.a2, (unsigned long long)c.a3, (unsigned long long)c.result,
            c.path[0] ? " \"" : "", c.path, c.path[0] ? "\"" : "");
        if (out.size() + std::strlen(line) >= capacity) break;
        out += line;
    }
    std::memcpy(text, out.data(), out.size()); text[out.size()] = 0;
    return int32_t(count);
}
extern "C" int32_t maxps4_fex_engine_recent_calls(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    std::lock_guard<std::mutex> guard(engine->probe.recent_lock);
    return format_calls(engine, engine->probe.recent, engine->probe.recent_next, text, capacity);
}
extern "C" int32_t maxps4_fex_engine_failed_calls(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    std::lock_guard<std::mutex> guard(engine->probe.recent_lock);
    return format_calls(engine, engine->probe.failed, engine->probe.failed_next, text, capacity);
}
extern "C" int32_t maxps4_fex_engine_opened_files(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    std::lock_guard<std::mutex> guard(engine->probe.recent_lock);
    return format_calls(engine, engine->probe.opened, engine->probe.opened_next, text, capacity);
}
extern "C" int32_t maxps4_fex_engine_missing_imports(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || (!text && capacity)) return -EINVAL;
    // Immutable once loaded; the counts are atomics the running game bumps.
    std::vector<std::pair<uint32_t, size_t>> order;
    for (size_t i = 0; i < engine->missing_imports.size(); ++i)
        order.push_back({engine->missing_reported[i].load(std::memory_order_relaxed), i});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string out;
    for (const auto& [calls, i] : order) {
        const std::string line = engine->missing_imports[i] + " " + std::to_string(calls) + "\n";
        if (out.size() + line.size() >= capacity) break;
        out += line;
    }
    if (capacity) { std::memcpy(text, out.data(), out.size()); text[out.size()] = 0; }
    return int32_t(order.size());
}
extern "C" int64_t maxps4_fex_engine_add_addcont(maxps4_fex_engine* engine, const char* label, const char* directory) {
    if (!engine) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    std::lock_guard lock(runtime_lock);
    return engine->libkernel.add_addcont(label, directory);
}
extern "C" int32_t maxps4_fex_engine_set_system_language(maxps4_fex_engine* engine, int32_t language) {
    if (!engine || language < 0 || language > 30) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    engine->libkernel.set_system_language(language);
    return 0;
}
extern "C" int64_t maxps4_fex_engine_set_savedata_root(maxps4_fex_engine* engine, const char* directory) {
    if (!engine) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    std::lock_guard lock(runtime_lock);
    if (engine->staged_ps4.segments().empty()) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    return engine->libkernel.set_savedata_root(directory);
}
extern "C" int64_t maxps4_fex_engine_call_libkernel(maxps4_fex_engine* engine, uint32_t service,
    const maxps4_ps4_kernel_call* call, uint64_t guest_thread) {
    if (!engine || !call) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    std::lock_guard lock(runtime_lock);
    if (engine->staged_ps4.segments().empty()) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    const maxps4::PS4KernelCall registers{call->rdi, call->rsi, call->rdx, call->rcx,
        call->r8, call->r9, call->rsp};
    return engine->libkernel.invoke(static_cast<maxps4::PS4KernelService>(service), registers, guest_thread);
}
extern "C" int64_t maxps4_fex_engine_graphics_event_ticket(maxps4_fex_engine* engine,
    uint64_t queue, uint64_t id, uint64_t* ticket) {
    if (ticket) *ticket = 0;
    if (!engine || !ticket) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    return maxps4::ps4::kernel_error(engine->libkernel.threads().graphics_event_ticket(queue, id, *ticket));
}
extern "C" int64_t maxps4_fex_engine_complete_graphics_event(maxps4_fex_engine* engine,
    uint64_t ticket) {
    if (!engine) return maxps4::ps4::kernel_error(maxps4::ps4::inval);
    // Never take runtime_lock: the guest may hold it while waiting for this
    // notification. PS4Threads protects its registration table independently;
    // completion touches no guest memory and no renderer or host pointer.
    return maxps4::ps4::kernel_error(engine->libkernel.threads().graphics_event_complete(ticket));
}
extern "C" int32_t maxps4_fex_engine_videoout_snapshot(maxps4_fex_engine* engine,
    maxps4_videoout_frame* frame, uint8_t* pixels, size_t capacity) {
    if (frame) *frame = {};
    if (!engine || !frame) return -EINVAL;
    return engine->libkernel.videoout().snapshot(*frame, pixels, capacity);
}
extern "C" int32_t maxps4_fex_engine_gnm_backend(maxps4_fex_engine* engine, uint32_t enabled) {
    if (!engine) return -EINVAL;
    return engine->libkernel.graphics().backend(enabled);
}
extern "C" int32_t maxps4_fex_engine_gnm_host_consumer(maxps4_fex_engine* engine,
    void* context, maxps4_gnm_host_consumer consumer) {
    if (!engine) return -EINVAL;
    return engine->libkernel.graphics().host_consumer(context,consumer);
}
extern "C" int32_t maxps4_fex_engine_read_memory(maxps4_fex_engine* engine,
    uint64_t address, void* destination, size_t bytes) {
    if (!engine || !destination || !bytes || bytes > (256ull << 20)) return -EINVAL;
    return engine->system_memory.read(address,destination,bytes) ? 0 : -EFAULT;
}
extern "C" int32_t maxps4_fex_engine_write_memory(maxps4_fex_engine* engine,
    uint64_t address, const void* source, size_t bytes, uint64_t* failed_address) {
    if (failed_address) *failed_address = address;
    if (!engine || !source || !bytes || bytes > (256ull << 20)) return -EINVAL;
    if (failed_address) *failed_address = 0;
    return engine->system_memory.write_gpu_result(address,source,bytes);
}
extern "C" int32_t maxps4_fex_engine_gpu_view(maxps4_fex_engine* engine,
    uint64_t address, size_t bytes, const void** pointer) {
    if (pointer) *pointer=nullptr;
    if (!engine || !pointer || !bytes) return -EINVAL;
    *pointer=engine->system_memory.gpu_view(address,bytes);
    return *pointer ? 0 : -EFAULT;
}
extern "C" int32_t maxps4_fex_engine_videoout_target(maxps4_fex_engine* engine,
    maxps4_videoout_frame* frame, uint64_t* address) {
    if (frame) *frame={};
    if (address) *address=0;
    if (!engine || !frame || !address) return -EINVAL;
    return engine->libkernel.videoout().target(*frame,*address);
}
extern "C" int32_t maxps4_fex_engine_gnm_snapshot(maxps4_fex_engine* engine,
    maxps4_gnm_submission* submission, uint32_t* words, size_t capacity) {
    if (submission) *submission={};
    if (!engine || !submission) return -EINVAL;
    return engine->libkernel.graphics().snapshot(*submission,words,capacity);
}
extern "C" int32_t maxps4_fex_engine_gnm_complete(maxps4_fex_engine* engine, uint64_t ticket, uint32_t success) {
    if (!engine || success>1) return -EINVAL;
    return engine->libkernel.graphics().complete(ticket,success!=0);
}
extern "C" int32_t maxps4_fex_engine_gnm_software_status(maxps4_fex_engine* engine, uint32_t* draws,
    uint32_t* rendered, uint32_t* failed, char* error, size_t capacity) {
    if (!engine) return -EINVAL;
    const auto stats = engine->libkernel.graphics().software_stats();
    if (draws) *draws = stats.draws;
    if (rendered) *rendered = stats.rendered;
    if (failed) *failed = stats.failed;
    if (error && capacity) {
        const size_t n = std::min(capacity - 1, stats.error.size());
        std::memcpy(error, stats.error.data(), n);
        error[n] = 0;
    }
    return 0;
}
extern "C" void maxps4_fex_engine_pad_latency(maxps4_fex_engine* engine, uint64_t* reads, uint64_t* total_us, uint64_t* most_us) {
    if (engine && reads && total_us && most_us) engine->libkernel.pad().latency(*reads, *total_us, *most_us);
}
extern "C" int32_t maxps4_fex_engine_pad_update(maxps4_fex_engine* engine, uint32_t buttons,
    uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2, uint8_t r2, uint32_t connected) {
    if (!engine || connected > 1) return -EINVAL;
    return engine->libkernel.pad().update(buttons, {{lx, ly, rx, ry}}, l2, r2, connected != 0);
}
extern "C" int32_t maxps4_fex_engine_audio_read(maxps4_fex_engine* engine, float* samples, uint32_t frames) {
    if (!engine || !samples || frames > 16384) return -EINVAL;
    return int32_t(engine->libkernel.audio().read(samples, frames));
}
extern "C" int32_t maxps4_fex_engine_audio_status(maxps4_fex_engine* engine, uint64_t* outputs,
    uint64_t* written, uint64_t* dropped, uint64_t* silent, uint32_t* ports, uint64_t* audible) {
    if (!engine) return -EINVAL;
    const auto stats = engine->libkernel.audio().stats();
    if (audible) *audible = stats.audible;
    if (outputs) *outputs = stats.outputs;
    if (written) *written = stats.written;
    if (dropped) *dropped = stats.dropped;
    if (silent) *silent = stats.silent;
    if (ports) *ports = stats.ports;
    return 0;
}
extern "C" int64_t maxps4_fex_engine_videoout_complete(maxps4_fex_engine* engine,
    uint64_t ticket, uint32_t presented) {
    if (!engine || presented > 1) return maxps4::PS4VideoOut::error(1);
    return engine->libkernel.videoout().complete(ticket, presented != 0);
}
extern "C" uint64_t maxps4_fex_engine_videoout_dropped(maxps4_fex_engine* engine) {
    return engine ? engine->libkernel.videoout().dropped() : 0;
}
extern "C" int32_t maxps4_fex_engine_videoout_output(maxps4_fex_engine* engine, uint32_t width, uint32_t height) {
    if (!engine) return -EINVAL;
    return engine->libkernel.videoout().set_output(width, height) ? 0 : -EINVAL;
}
extern "C" uint64_t maxps4_fex_engine_videoout_requested(maxps4_fex_engine* engine) {
    return engine ? engine->libkernel.videoout().requested() : 0;
}
extern "C" void maxps4_fex_engine_videoout_pause(maxps4_fex_engine* engine, uint32_t paused) {
    if (engine) engine->libkernel.videoout().set_paused(paused != 0);
}
extern "C" int64_t maxps4_fex_engine_videoout_vblank(maxps4_fex_engine* engine, uint64_t port) {
    if (!engine) return maxps4::PS4VideoOut::error(11);
    return engine->libkernel.videoout().vblank(port);
}
extern "C" int32_t maxps4_fex_engine_sample_main_thread(maxps4_fex_engine* engine, maxps4_fex_thread_sample* out) {
    if (!engine || !out) return -EINVAL;
    *out = {};
    if (!GuestFaultGuard::available) return -ENOTSUP;
    // SIGURG would interrupt the host syscalls of an HLE call: sample only
    // while the main guest thread runs outside HLE. A sample costs the game
    // up to ~70 ms (signal retries, then 16 suspensions) and the caller holds
    // its session lock meanwhile: only when no HLE call returned for 2 s,
    // never while the game runs normally.
    const uint64_t now = ProbeAddressSpace::now_ns();
    for (const auto& a : engine->probe.activity)
        if (a.thread.load() == 1 && (a.inside.load() || now - std::min(now, a.since.load()) < 2000000000ULL))
            return -EBUSY;
    std::lock_guard lock(engine->sample_lock);
    auto* guard = engine->main_guard;
    if (!guard) return -ESRCH;
    // The run's own thread (the main guest thread's host thread, once
    // run_orbis returned) would suspend itself and never resume.
    if (::pthread_equal(guard->host, ::pthread_self())) return -EDEADLK;
    const uint32_t before = guard->sample_sequence.load(std::memory_order_acquire);
    guard->sample_request = true;
    for (int i = 0; i < 50 && guard->sample_sequence.load(std::memory_order_acquire) == before; ++i) {
        ::pthread_kill(guard->host, SIGURG);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto name = [](uint64_t address, char* text, size_t capacity) {
        Dl_info info {};
        if (!::dladdr(reinterpret_cast<void*>(address), &info) || !info.dli_sname) return false;
        std::snprintf(text, capacity, "%s+0x%llx", info.dli_sname,
            (unsigned long long)(address - reinterpret_cast<uintptr_t>(info.dli_saddr)));
        return true;
    };
    if (guard->sample_sequence.load(std::memory_order_acquire) == before) {
        guard->sample_request = false;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        // The signal was not taken: read the suspended thread instead. Nothing
        // that may lock (allocation, FEX lookups, dladdr) runs while it is
        // suspended, since it may hold that lock.
        const thread_act_t port = ::pthread_mach_thread_np(guard->host);
        const auto top = reinterpret_cast<uintptr_t>(::pthread_get_stackaddr_np(guard->host));
        const uintptr_t low = top - ::pthread_get_stacksize_np(guard->host);
        auto strip = [](uint64_t address) {
#if __has_feature(ptrauth_calls)
            return uint64_t(reinterpret_cast<uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(address), ptrauth_key_return_address)));
#else
            return address;
#endif
        };
        std::array<uint64_t, 16> frames {};
        size_t depth = 0;
        // Several reads 1 ms apart: a waiting loop shows up as a few guest
        // addresses. FEX's stored RIP only moves when a block leaves JIT code,
        // so JIT positions are translated back from the host PC (after resume).
        constexpr int reads = 16;
        for (int k = 0; k < reads; ++k) {
            if (k) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            arm_thread_state64_t state {};
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            if (::thread_suspend(port) != KERN_SUCCESS) break;
            const kern_return_t read = ::thread_get_state(port, ARM_THREAD_STATE64,
                reinterpret_cast<thread_state_t>(&state), &count);
            if (read == KERN_SUCCESS && !k) {
                out->host_pc = strip(arm_thread_state64_get_pc(state));
                out->host_lr = strip(arm_thread_state64_get_lr(state));
                for (size_t r = 0; r < 29; ++r) out->registers[r] = state.__x[r];
                // JIT code may use x29 as a scratch register: only follow frames
                // that stay inside this thread's stack and move up it.
                uintptr_t fp = arm_thread_state64_get_fp(state);
                while (depth < frames.size() && fp >= low && fp % 16 == 0 && fp <= top - 16) {
                    const auto* frame = reinterpret_cast<const uint64_t*>(fp);
                    if (frame[1]) frames[depth++] = strip(frame[1]);
                    if (frame[0] <= fp) break;
                    fp = frame[0];
                }
                out->guest_rip = guard->stored_rip();
            }
            ::thread_resume(port);
            if (read != KERN_SUCCESS) break;
            if (!k) out->method = 1;
            const uint64_t pc = strip(arm_thread_state64_get_pc(state));
            if (!guard->in_jit(pc)) continue;
            const uint64_t rip = guard->guest_rip(pc);
            if (!k) out->guest_rip = rip;
            bool seen = false;
            for (uint32_t i = 0; i < out->rip_count; ++i) seen = seen || out->rips[i] == rip;
            if (!seen && out->rip_count < 8) out->rips[out->rip_count++] = rip;
        }
        if (out->method != 1) return -ETIMEDOUT;
        const uint64_t pc = out->host_pc;
        const bool dispatcher = guard->in_dispatcher(pc);
        const bool named = name(pc, out->symbol, sizeof out->symbol);
        out->location = guard->in_jit(pc) ? 1 : dispatcher ? 2 : named ? 3 : 1;
        name(out->host_lr, out->caller, sizeof out->caller);
        size_t used = 0;
        for (size_t i = 0; i < depth && used + 1 < sizeof out->backtrace; ++i) {
            char symbol[128] {};
            if (!name(frames[i], symbol, sizeof symbol))
                std::snprintf(symbol, sizeof symbol, "0x%llx", (unsigned long long)frames[i]);
            const int n = std::snprintf(out->backtrace + used, sizeof out->backtrace - used, "%s%s", i ? " < " : "", symbol);
            if (n < 0) break;
            used = std::min(sizeof out->backtrace - 1, used + size_t(n));
        }
        return 0;
#else
        return -ETIMEDOUT;
#endif
    }
    const auto sample = guard->sample;
    out->host_pc = sample.pc; out->host_lr = sample.lr; out->guest_rip = sample.rip; out->location = sample.where;
    name(sample.pc, out->symbol, sizeof out->symbol);
    name(sample.lr, out->caller, sizeof out->caller);
    return 0;
}
extern "C" int32_t maxps4_fex_engine_set_profiling(maxps4_fex_engine* engine, uint32_t enabled) {
    if (!engine || enabled > 1) return -EINVAL;
    engine->profiling = enabled != 0; return 0;
}
extern "C" int32_t maxps4_fex_engine_profile_report(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    text[0] = 0;
    auto& p = engine->profile;
    std::vector<std::pair<uint64_t, uint64_t>> guest, symbols;
    uint64_t jit, dispatcher, host;
    {
        std::lock_guard g(p.lock);
        jit = p.jit; dispatcher = p.dispatcher; host = p.host;
        guest.assign(p.guest.begin(), p.guest.end());
        symbols.assign(p.symbols.begin(), p.symbols.end());
    }
    const uint64_t total = jit + dispatcher + host;
    if (!total) return 0;
    auto by_count = [](const auto& a, const auto& b) { return a.second > b.second; };
    std::sort(guest.begin(), guest.end(), by_count);
    std::sort(symbols.begin(), symbols.end(), by_count);
    auto percent = [total](uint64_t n) { return 100.0 * double(n) / double(total); };
    char line[256];
    std::snprintf(line, sizeof line, "Profil thread 1 (%llu échantillons) : code du jeu %.0f %% · répartiteur FEX %.0f %% · émulateur %.0f %%\n",
        (unsigned long long)total, percent(jit), percent(dispatcher), percent(host));
    std::string out = line;
    out += "Code du jeu (RIP relatif) :";
    for (size_t i = 0; i < std::min<size_t>(10, guest.size()); ++i) {
        const uint64_t rip = guest[i].first >= engine->relocation ? guest[i].first - engine->relocation : guest[i].first;
        std::snprintf(line, sizeof line, " 0x%llx %.1f %%", (unsigned long long)rip, percent(guest[i].second));
        out += line;
    }
    out += "\nÉmulateur :";
    for (size_t i = 0; i < std::min<size_t>(8, symbols.size()); ++i) {
        Dl_info info {};
        const char* name = symbols[i].first && ::dladdr(reinterpret_cast<void*>(symbols[i].first), &info) && info.dli_sname
            ? info.dli_sname : "?";
        std::snprintf(line, sizeof line, " %.60s %.1f %%", name, percent(symbols[i].second));
        out += line;
    }
    out += "\n";
    const size_t n = std::min(capacity - 1, out.size());
    std::memcpy(text, out.data(), n);
    text[n] = 0;
    return 0;
}
extern "C" int32_t maxps4_fex_set_tso_emulation(uint32_t enabled) {
    if (enabled > 1) return -EINVAL;
    tso_emulation = enabled != 0; return 0;
}
// Where a thread's faulting access went: the game's view of the page (read,
// write), the host's protection of it, and the game's last map/unmap/protect
// calls that covered it.
// The guest registers of a faulting thread, and the first 0x30 bytes each
// readable register points at (a Gnm command buffer: begin, end, current,
// reserve callback and its argument).
static std::string fault_registers(maxps4_fex_engine* engine, const std::array<uint64_t, 16>& gprs) {
    static constexpr const char* names[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                              "r8","r9","r10","r11","r12","r13","r14","r15"};
    std::string out = " · registres :";
    char line[160];
    for (size_t i = 0; i < 16; ++i) {
        std::snprintf(line, sizeof line, " %s 0x%llx", names[i], (unsigned long long)gprs[i]);
        out += line;
    }
    // The guest call chain through saved frame pointers ([rbp] the caller's
    // rbp, [rbp + 8] its return address), while it stays within readable memory.
    {
        out += " · appels (rbp) :";
        uint64_t frame = gprs[5];
        for (int depth = 0; depth < 12 && frame && !(frame & 7); ++depth) {
            uint8_t pair[16];
            if (!engine->system_memory.read(frame, pair, sizeof pair)) break;
            const uint64_t next = maxps4::ps4::get_le(pair, 8), back = maxps4::ps4::get_le(pair + 8, 8);
            const bool code = back >= engine->relocation && back - engine->relocation < 0x20000000;
            if (!code) break;
            std::snprintf(line, sizeof line, " 0x%llx", (unsigned long long)(back - engine->relocation));
            out += line;
            if (next <= frame) break;
            frame = next;
        }
    }
    // The stack's top (16 words) holds the return addresses of the callers.
    for (const size_t i : {size_t(4), size_t(3), size_t(7), size_t(14)}) {
        uint8_t bytes[0x80];
        const size_t size = i == 4 ? sizeof bytes : 0x30;
        if (!gprs[i] || !engine->system_memory.read(gprs[i], bytes, size)) continue;
        std::snprintf(line, sizeof line, " · [%s] :", names[i]);
        out += line;
        for (size_t q = 0; q < size / 8; ++q) {
            const uint64_t value = maxps4::ps4::get_le(bytes + 8 * q, 8);
            const bool code = value >= engine->relocation && value - engine->relocation < 0x10000000;
            std::snprintf(line, sizeof line, code ? " code+0x%llx" : " 0x%llx",
                (unsigned long long)(code ? value - engine->relocation : value));
            out += line;
        }
    }
    return out;
}
static std::string fault_memory(maxps4_fex_engine* engine, uint64_t address) {
    std::string out;
    char line[320];
    std::snprintf(line, sizeof line, " · page du jeu : lecture %s, écriture %s",
        engine->system_memory.permitted(address, 1, 4) ? "oui" : "non",
        engine->system_memory.permitted(address, 1, 2) ? "oui" : "non");
    out += line;
#if defined(__APPLE__)
    vm_address_t region = vm_address_t(address);
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t info {};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t kr = vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (kr != KERN_SUCCESS) std::snprintf(line, sizeof line, " · hôte : erreur %d", int(kr));
    else if (region > address) std::snprintf(line, sizeof line, " · hôte : non mappée (suivante 0x%llx)", (unsigned long long)region);
    else std::snprintf(line, sizeof line, " · hôte : 0x%llx-0x%llx %c%c%c (max %c%c%c)",
        (unsigned long long)region, (unsigned long long)(region + size),
        (info.protection & VM_PROT_READ) ? 'r' : '-', (info.protection & VM_PROT_WRITE) ? 'w' : '-',
        (info.protection & VM_PROT_EXECUTE) ? 'x' : '-', (info.max_protection & VM_PROT_READ) ? 'r' : '-',
        (info.max_protection & VM_PROT_WRITE) ? 'w' : '-', (info.max_protection & VM_PROT_EXECUTE) ? 'x' : '-');
    out += line;
#endif
    uint64_t below_start = 0, below_end = 0, above_start = 0, above_end = 0;
    engine->dynamic_ps4.neighbours(address, below_start, below_end, above_start, above_end);
    std::snprintf(line, sizeof line, " · mappé avant : 0x%llx-0x%llx, après : 0x%llx-0x%llx",
        (unsigned long long)below_start, (unsigned long long)below_end,
        (unsigned long long)above_start, (unsigned long long)above_end);
    out += line;
    const uint64_t now = maxps4::PS4VirtualMemory::clock_ns();
    for (const uint64_t radius : {uint64_t(0), uint64_t(32) << 20}) {
        std::array<maxps4::PS4VirtualMemory::Operation, 6> operations {};
        size_t shown = 0, total = 0;
        const size_t found = engine->dynamic_ps4.operations_at(address, radius, operations, shown, total);
        std::snprintf(line, sizeof line, radius ? " · %zu appels mémoire à moins de 32 Mio (sur %zu)" :
            " · %zu appels mémoire sur cette page (sur %zu)", found, total);
        out += line;
        for (size_t i = 0; i < shown; ++i) {
            const auto& o = operations[i];
            const char* kind = o.kind == 'u' ? "unmap" : o.kind == 'p' ? "protect" : o.kind == 'f' ? "map fixe" : "map";
            std::snprintf(line, sizeof line, "%s il y a %.2f s %s 0x%llx +0x%llx prot 0x%x%s",
                i ? " ;" : " :", double(now - std::min(now, o.ns)) / 1e9, kind,
                (unsigned long long)o.address, (unsigned long long)o.size, o.protection, o.error ? " REFUSÉ" : "");
            out += line;
            if (o.result && o.result != o.address) {
                std::snprintf(line, sizeof line, " -> 0x%llx", (unsigned long long)o.result);
                out += line;
            }
        }
    }
    // Memory calls libkernel refused (BatchMap entries included), some before
    // reaching the mappings above.
    std::array<maxps4::PS4Libkernel::MemoryFailure, maxps4::PS4Libkernel::memory_failure_limit> failures {};
    size_t listed = 0;
    const size_t refused = engine->libkernel.memory_failures(failures, listed);
    if (!refused) out += " · aucun appel mémoire refusé";
    else {
        std::snprintf(line, sizeof line, " · %zu appels mémoire refusés, récents d'abord :", refused);
        out += line;
    }
    for (size_t i = 0; i < std::min<size_t>(listed, 6); ++i) {
        const auto& f = failures[i];
        std::snprintf(line, sizeof line, "%s %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) adresse 0x%llx = 0x%llx",
            i ? " ;" : "", service_name(f.service), (unsigned long long)f.call.rdi, (unsigned long long)f.call.rsi,
            (unsigned long long)f.call.rdx, (unsigned long long)f.call.rcx, (unsigned long long)f.call.r8,
            (unsigned long long)f.hint, (unsigned long long)f.result);
        out += line;
    }
    return out;
}
extern "C" void maxps4_fex_set_host_crash_note(const char* path) {
    const size_t length = path ? std::min(std::strlen(path), sizeof host_crash_note - 1) : 0;
    // The first byte last: a signal handler never sees a half-written path.
    host_crash_note[0] = 0;
    if (!length) return;
    std::memcpy(host_crash_note + 1, path + 1, length - 1);
    host_crash_note[length] = 0;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    host_crash_note[0] = path[0];
}
extern "C" int32_t maxps4_fex_engine_fault_detail(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    std::lock_guard sampling(engine->sample_lock);
    const size_t length = std::min(engine->last_fault_detail.size(), capacity - 1);
    std::memcpy(text, engine->last_fault_detail.data(), length);
    text[length] = 0;
    return int32_t(length);
}
extern "C" int32_t maxps4_fex_engine_thread_activity(maxps4_fex_engine* engine, char* text, size_t capacity) {
    if (!engine || !text || !capacity) return -EINVAL;
    char header[160];
    std::snprintf(header, sizeof header, "TSO %s · réparations d'alignement %llu (dernier PC hôte 0x%llx)\n",
        tso_emulation.load() ? "émulé" : "désactivé", (unsigned long long)alignment_repairs.load(),
        (unsigned long long)last_repair_pc.load());
    std::string out = header;
    out += "Interruptions GPU de fin de travail : " + std::to_string(engine->libkernel.threads().graphics_interrupts()) + "\n";
    if (const std::string asked = engine->libkernel.load_exec(); !asked.empty())
        out += "LoadExec demandé : " + asked + "\n";
    out += engine->libkernel.graphics().submit_activity();
    out += engine->libkernel.videoout().submit_activity();
    out += engine->libkernel.videoout().flip_activity();
    std::unique_lock sampling(engine->sample_lock);
    out += engine->last_fault_detail;
    if (engine->last_fault_detail.empty()) {
        // While the run stops, before the threads are gone: the live report
        // keeps it even if the app itself dies on the way.
        maxps4_fex_guest_fault fault {};
        uint64_t id = 0;
        std::array<uint64_t, 16> gprs {};
        if (engine->probe.threads && engine->probe.threads->faulted(fault) && engine->probe.threads->fault_thread(id, gprs))
            out += fault_detail(engine, fault, id, gprs);
    }
    sampling.unlock();
    {
        std::array<maxps4::PS4Libkernel::SaveCall, maxps4::PS4Libkernel::save_call_limit> calls {};
        size_t count = 0;
        std::string root;
        const size_t total = engine->libkernel.save_calls(calls, count, root);
        if (total) {
            char line[256];
            std::snprintf(line, sizeof line, "Sauvegarde : dossier %s · %zu appels, récents d'abord :\n",
                root.empty() ? "AUCUN" : root.c_str(), total);
            out += line;
            for (size_t i = 0; i < std::min<size_t>(count, 12); ++i) {
                const auto& c = calls[i];
                std::snprintf(line, sizeof line, "  %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%llx%s%s\n",
                    service_name(c.service), (unsigned long long)c.call.rdi, (unsigned long long)c.call.rsi,
                    (unsigned long long)c.call.rdx, (unsigned long long)c.call.rcx, (unsigned long long)c.result,
                    c.path[0] ? " " : "", c.path);
                out += line;
            }
        }
    }
    {
        const auto ajm = engine->libkernel.ajm().stats();
        if (ajm.batches || ajm.instances) {
            char line[200];
            std::snprintf(line, sizeof line, "Ajm : %u instances · %llu lots, %llu tâches (%llu en erreur, dernier résultat 0x%x) · %llu trames MP3, %llu échantillons\n",
                ajm.instances, (unsigned long long)ajm.batches, (unsigned long long)ajm.jobs,
                (unsigned long long)ajm.failed_jobs, unsigned(ajm.last_result), (unsigned long long)ajm.frames,
                (unsigned long long)ajm.samples);
            out += line;
        }
    }
    if (engine->table_watch) out += engine->table_watch->report();
    if (maxps4_fex_code_cache_used) {
        // The iOS code buffer's fill and in-place clears (patch-fexcore-code-cache-reuse.py):
        // a clear while other threads run can leave them on stale code.
        char line[160];
        std::snprintf(line, sizeof line, "Cache JIT : %.1f Mio utilisés sur 128 · %llu vidages en place\n",
            double(maxps4_fex_code_cache_used()) / (1024.0 * 1024.0),
            (unsigned long long)(maxps4_fex_code_cache_reuses ? maxps4_fex_code_cache_reuses() : 0));
        out += line;
    }
    {
        // A failed lock the game ignores leaves its critical section unprotected.
        size_t used = 0, peak = 0;
        engine->libkernel.mutex_usage(used, peak);
        const auto failed = engine->libkernel.mutex_failures();
        char line[400];
        int n = std::snprintf(line, sizeof line, "Mutex : %zu en service (au plus %zu sur %zu) · échecs : lock %llu, trylock %llu, unlock %llu",
            used, peak, maxps4::PS4Threads::mutex_limit, (unsigned long long)failed.lock,
            (unsigned long long)failed.trylock, (unsigned long long)failed.unlock);
        if (failed.lock || failed.trylock || failed.unlock)
            n += std::snprintf(line + n, sizeof line - size_t(n), " · dernier %s(0x%llx) thread %llu = 0x%llx",
                service_name(failed.last_service), (unsigned long long)failed.last_slot,
                (unsigned long long)failed.last_thread, (unsigned long long)failed.last_result);
        // An init over a mutex another thread holds lets two threads into its critical section.
        uint64_t reinit_live = 0, reinit_held = 0, reinit_slot = 0;
        engine->libkernel.mutex_reinits(reinit_live, reinit_held, reinit_slot);
        if (reinit_live && n > 0 && size_t(n) < sizeof line)
            n += std::snprintf(line + n, sizeof line - size_t(n), " · réinitialisés vivants %llu, dont tenus %llu (dernier 0x%llx)",
                (unsigned long long)reinit_live, (unsigned long long)reinit_held, (unsigned long long)reinit_slot);
        out += line;
        out += "\n";
    }
    const uint64_t now = ProbeAddressSpace::now_ns();
    if (const uint64_t asked = engine->probe.stop_asked_ns.load()) {
        const uint64_t told = engine->probe.stop_told_ns.load();
        char stop[160];
        std::snprintf(stop, sizeof stop, "Arrêt demandé depuis %.1f s · threads prévenus %s%.1f s · %u terminés à leur appel\n",
            double(now - std::min(now, asked)) / 1e9, told ? "depuis " : "NON, attente ", double(now - std::min(now, told ? told : asked)) / 1e9,
            engine->probe.stop_exits.load());
        out += stop;
    }
    for (const auto& a : engine->probe.activity) {
        const uint64_t thread = a.thread.load();
        if (!thread) continue;
        const uint32_t raw = a.service.load();
        const char* name = raw >= maxps4::missing_import_token ? "import manquant" :
            service_name(static_cast<maxps4::PS4KernelService>(raw));
        const double seconds = double(now - std::min(now, a.since.load())) / 1e9;
        const auto returned = (unsigned long long)a.returned.load();
        char line[192];
        if (!a.inside.load()) {
            std::snprintf(line, sizeof line, "thread %llu : code du jeu depuis %.1f s, dernier %s (%llu appels)",
                (unsigned long long)thread, seconds, name, returned);
            out += line;
            // Over a second without a call: where it spins.
            if (seconds > 1 && thread != 1) {
                std::lock_guard sampling(engine->sample_lock);
                if (engine->probe.threads) {
                    uint64_t regs[31] {}, jit_rip = 0;
                    out += engine->probe.threads->sample(thread, engine->relocation, regs, &jit_rip);
                    // Spinning in Call of Duty's string lookup: what it looks for, and where.
                    if (engine->table_watch && jit_rip >= 0x26b1600 && jit_rip < 0x26b1670)
                        out += engine->table_watch->explain(uint32_t(regs[29]), uint32_t(regs[5]));
                    uint64_t rip = 0, ended_ns = 0, address = 0;
                    int signal = 0, code = 0;
                    std::array<uint64_t, 16> gprs {};
                    if (engine->probe.threads->ending_of(thread, rip, ended_ns, signal, code, address, &gprs)) {
                        std::snprintf(line, sizeof line, " · terminé il y a %.1f s en 0x%llx (signal %d code %d adresse 0x%llx), octets",
                            double(now - std::min(now, ended_ns)) / 1e9,
                            (unsigned long long)(rip >= engine->relocation ? rip - engine->relocation : rip), signal, code,
                            (unsigned long long)address);
                        out += line;
                        uint8_t code[8] {};
                        if (engine->system_memory.read(rip, code, sizeof code))
                            for (const uint8_t byte : code) { std::snprintf(line, sizeof line, " %02x", byte); out += line; }
                        else out += " illisibles";
                        if (address && signal != SIGILL) {
                            out += fault_memory(engine, address);
                            out += fault_registers(engine, gprs);
                        }
                    }
                }
                else out += " · threads inconnus";
            }
            out += "\n";
            continue;
        }
        std::snprintf(line, sizeof line, "thread %llu : dans %s depuis %.1f s (%llu appels)",
            (unsigned long long)thread, name, seconds, returned);
        out += line;
        // A thread waiting for a mutex: who holds it (itself: a deadlock).
        // A semaphore or an event queue a thread waits on: its state.
        if (raw == static_cast<uint32_t>(maxps4::PS4KernelService::sema_wait))
            out += " · sémaphore " + engine->libkernel.threads().sema_state(a.argument.load());
        if (raw == static_cast<uint32_t>(maxps4::PS4KernelService::equeue_wait))
            out += " · file : " + engine->libkernel.threads().equeue_state(a.argument.load());
        if (raw == static_cast<uint32_t>(maxps4::PS4KernelService::mutex_lock)) {
            const uint64_t owner = engine->libkernel.threads().mutex_owner(a.argument.load());
            if (owner == thread) out += " · verrou déjà tenu par ce thread";
            else if (owner) {
                std::snprintf(line, sizeof line, " · verrou tenu par le thread %llu", (unsigned long long)owner);
                out += line;
            }
        }
        // Where the game called from: the return address, then the frame pointer
        // chain (the stack stays put while the thread is inside the service).
        auto relative = [&](uint64_t rip) { return rip >= engine->relocation ? rip - engine->relocation : rip; };
        uint64_t words[2] {};
        const char* separator = " depuis";
        if (engine->system_memory.read(a.rsp.load(), words, 8)) {
            std::snprintf(line, sizeof line, "%s 0x%llx", separator, (unsigned long long)relative(words[0]));
            out += line;
            separator = " <";
        }
        uint64_t frame = a.rbp.load();
        for (int depth = 0; depth < 6 && frame && engine->system_memory.read(frame, words, 16); ++depth) {
            std::snprintf(line, sizeof line, "%s 0x%llx", separator, (unsigned long long)relative(words[1]));
            out += line;
            separator = " <";
            if (words[0] <= frame) break; // Frames only go up the stack.
            frame = words[0];
        }
        out += "\n";
    }
    const size_t n = std::min(capacity - 1, out.size());
    std::memcpy(text, out.data(), n);
    text[n] = 0;
    return 0;
}
extern "C" int32_t maxps4_fex_engine_read_ps4(maxps4_fex_engine* engine,
    uint64_t address, uint8_t* output, size_t length) {
    if (!engine || !output || !length || length > 1024 * 1024) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    auto& memory = engine->system_memory;
    return memory.read(address, output, length) ? 0 : -EFAULT;
}
extern "C" int32_t maxps4_fex_engine_fetch_ps4(maxps4_fex_engine* engine,
    uint64_t address, uint8_t* output, size_t length) {
    if (!engine || !output || !length || length > 1024 * 1024) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    auto& memory = engine->system_memory;
    return memory.fetch(address, output, length) ? 0 : -EFAULT;
}
extern "C" int32_t maxps4_fex_engine_write_ps4(maxps4_fex_engine* engine,
    uint64_t address, const uint8_t* input, size_t length) {
    if (!engine || !input || !length || length > 1024 * 1024) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    auto& memory = engine->system_memory;
    return memory.write(address, input, length) ? 0 : -EFAULT;
}
extern "C" int32_t maxps4_fex_engine_query_ps4(maxps4_fex_engine* engine,
    uint64_t address, size_t length, uint32_t permissions, uint32_t* permitted) {
    if (permitted) *permitted = 0;
    if (!engine || !permitted || !length || length > 1024 * 1024 ||
        !permissions || (permissions & ~7U)) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    auto& memory = engine->system_memory;
    *permitted = memory.permitted(address, length, permissions) ? 1U : 0U;
    return 0;
}
extern "C" int32_t maxps4_fex_engine_contiguous_ps4(maxps4_fex_engine* engine,
    uint64_t address, size_t limit, uint32_t permissions, size_t* available) {
    if (available) *available = 0;
    if (!engine || !available || !limit || limit > 1024 * 1024 ||
        !permissions || (permissions & ~7U)) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    auto& memory = engine->system_memory;
    *available = memory.contiguous(address, limit, permissions);
    return 0;
}
extern "C" int32_t maxps4_fex_engine_run_elf(maxps4_fex_engine* engine,
    const uint8_t* file, size_t length, uint32_t budget, uint64_t* result,
    uint32_t* backend, uint64_t* entry) {
    return runELF(engine, file, length, budget, result, backend, entry, nullptr);
}
extern "C" int32_t maxps4_fex_engine_run_elf_pm4(maxps4_fex_engine* engine,
    const uint8_t* file, size_t length, uint32_t budget, uint64_t address, size_t count,
    uint32_t* words, size_t capacity, size_t* written, uint64_t* result,
    uint32_t* backend, uint64_t* entry) {
    if (written) *written = 0;
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (entry) *entry = 0;
    if (!written || !words || !count || count > maxps4::gpu::kMaxSubmissionWords || capacity < count)
        return -EINVAL;
    const PM4Request request {address, count, words, written};
    return runELF(engine, file, length, budget, result, backend, entry, &request);
}


extern "C" int32_t maxps4_fex_engine_interpret_elf(maxps4_fex_engine* engine,
    const uint8_t* file, size_t length, uint32_t budget, uint64_t* result,
    uint32_t* backend, uint64_t* entry, uint64_t* rip, uint32_t* instructions) {
    if (result) *result = 0;
    if (backend) *backend = 0;
    if (entry) *entry = 0;
    if (rip) *rip = 0;
    if (instructions) *instructions = 0;
    if (!engine || !result || !backend || !entry || !rip || !instructions) return -EINVAL;
    std::lock_guard lock(runtime_lock);
    try {
        maxps4::GuestELF image;
        const int loaded = image.load(file, length, budget);
        if (loaded) return loaded;
        *entry = image.entry();
        maxps4::GuestELF::Execution execution;
        const int status = image.interpret(budget, execution);
        *result = execution.rax;
        *rip = execution.rip;
        *instructions = execution.instructions;
        *backend = MAXPS4_CPU_RESTRICTED_INTERPRETER;
        return status;
    } catch (const std::bad_alloc&) { return -ENOMEM; }
}


