#include "maxps4_fex_runtime.h"
#include "maxps4_native.h"
#include "GuestELFFixture.h"
#include <FEXCore/Utils/AllocatorHooks.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <algorithm>
#include <array>
#include <vector>
#include <csignal>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <TargetConditionals.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
// Real CPU -> the already staged arena -> logical-address readback.
// Bounded rebased fixture only; no general PS4 virtual address-space claim.
static int stagedCPU(maxps4_fex_engine* engine, bool initialized) {
    auto file = guest_test::generated_pm4();
    file[7] = 9;
    guest_test::put(file, 0xfe00, 16, 2);
    uint64_t entry = 99, result = 99;
    uint32_t segments = 99, blockers = 99, backend = 99;
    CHECK(maxps4_fex_engine_run_staged_ps4(nullptr, 6, &result, &backend, &entry) == -EINVAL);
    CHECK(result == 0 && backend == 0 && entry == 0);
    for (uint32_t value : {3U, 6U, 9U}) {
        const uint64_t base = value == 3 ? 0 : 0x100000000ULL;
        const uint64_t code_address = base + (value == 9 ? 0x20000 : 0x1000);
        const uint64_t data_address = base + 0x10000;
        guest_test::put(file, code_address, 24, 8);
        guest_test::put(file, code_address, 80, 8);
        guest_test::put(file, data_address, 136, 8);
        for (size_t i = 0; i < 3; ++i)
            guest_test::put(file, (data_address + i * 4) - (code_address + 20 + i * 10), 196 + i * 10, 4);
        guest_test::put(file, value, 210, 4);
        CHECK(maxps4_fex_engine_stage_ps4(engine, file.data(), file.size(), &entry, &segments, &blockers) == 0);
        CHECK(entry == code_address && segments == 2 && blockers == 4);
        uint8_t data[12] = {};
        CHECK(maxps4_fex_engine_read_ps4(engine, data_address, data, sizeof(data)) == 0);
        for (auto byte : data) CHECK(byte == 0);
        CHECK(maxps4_fex_engine_run_staged_ps4(engine, 5, &result, &backend, &entry) == -E2BIG);
        CHECK(result == 0 && backend == 0 && entry == 0);
        const int status = maxps4_fex_engine_run_staged_ps4(engine, 6, &result, &backend, &entry);
        CHECK(status == (initialized ? 0 : -EAGAIN));
        CHECK(maxps4_fex_engine_read_ps4(engine, data_address, data, sizeof(data)) == 0);
        if (initialized) {
            CHECK(result == 42 && backend == MAXPS4_CPU_FEX_ARM64_JIT && entry == code_address);
            CHECK(data[0] == 0 && data[1] == 0x2d && data[2] == 1 && data[3] == 0xc0);
            CHECK(data[4] == value && data[8] == 2);
            // Running again uses the same staged mapping, never a copied image.
            const uint8_t zero[12] = {};
            CHECK(maxps4_fex_engine_write_ps4(engine, data_address, zero, sizeof(zero)) == 0);
            CHECK(maxps4_fex_engine_run_staged_ps4(engine, 6, &result, &backend, &entry) == 0);
            CHECK(maxps4_fex_engine_read_ps4(engine, data_address, data, sizeof(data)) == 0 && data[4] == value);
        } else {
            CHECK(result == 0 && backend == 0 && entry == 0);
            for (auto byte : data) CHECK(byte == 0);
        }
    }
    // An invalid late store must prevent ALL stores, even earlier valid ones.
    file = guest_test::generated_pm4();
    file[7] = 9; guest_test::put(file, 0xfe00, 16, 2);
    auto bad = file;
    guest_test::put(bad, 0x1000 - 0x1028, 216, 4);
    CHECK(maxps4_fex_engine_stage_ps4(engine, bad.data(), bad.size(), &entry, &segments, &blockers) == 0);
    CHECK(maxps4_fex_engine_run_staged_ps4(engine, 6, &result, &backend, &entry) == -ENOTSUP);
    CHECK(result == 0 && backend == 0 && entry == 0);
    uint8_t data[12] = {};
    CHECK(maxps4_fex_engine_read_ps4(engine, 0x10000, data, sizeof(data)) == 0);
    for (auto byte : data) CHECK(byte == 0);
    bad = file; guest_test::put(bad, 0xfe10, 16, 2);
    CHECK(maxps4_fex_engine_stage_ps4(engine, bad.data(), bad.size(), &entry, &segments, &blockers) == 0);
    CHECK(maxps4_fex_engine_run_staged_ps4(engine, 6, &result, &backend, &entry) == -ENOTSUP);
    CHECK(result == 0 && backend == 0 && entry == 0);
    const uint8_t invalid[] = {0, 1};
    CHECK(maxps4_fex_engine_stage_ps4(engine, invalid, sizeof(invalid), &entry, &segments, &blockers) != 0);
    CHECK(maxps4_fex_engine_run_staged_ps4(engine, 6, &result, &backend, &entry) == -EFAULT);
    CHECK(result == 0 && backend == 0 && entry == 0);
    std::puts(initialized ? "PASS real FEX stores -> same staged PS4 bytes; bounded rebased fixture only" :
                           "PASS staged CPU refuses dispatch before initialization");
    return 0;
}
// Exercise the engine's actual staged-memory/service bridge without InitCore
// or guest CPU dispatch. The plaintext PS4 fixture remains blocked for linking.
static int systemServices(maxps4_fex_engine* engine) {
    auto file = guest_test::elf(40, 2, true);
    file[7] = 9;
    guest_test::put(file, 0xfe00, 16, 2);
    guest_test::put(file, 8192, 160, 8);
    uint64_t entry = 0; uint32_t segments = 0, blockers = 0;
    CHECK(maxps4_fex_engine_stage_ps4(engine, file.data(), file.size(), &entry, &segments, &blockers) == 0);
    CHECK(blockers == MAXPS4_PS4_CPU_ADDRESS_SPACE_REQUIRED);
    char directory[] = "/tmp/maxps4-service-bridge-XXXXXX";
    CHECK(::mkdtemp(directory));
    char host_path[1024]; std::snprintf(host_path, sizeof(host_path), "%s/fixture", directory);
    const int host = ::open(host_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(host >= 0 && ::write(host, "Kero", 4) == 4 && ::close(host) == 0);
    CHECK(maxps4_fex_engine_mount_app0(engine, directory) == 0);
    const char guest_path[] = "/app0/fixture";
    CHECK(maxps4_fex_engine_write_ps4(engine, 0x10000,
        reinterpret_cast<const uint8_t*>(guest_path), sizeof(guest_path)) == 0);
    auto resolve = [](const char* symbol) { return maxps4_ps4_resolve_libkernel(symbol, "libkernel", "libkernel", 1); };
    maxps4_ps4_kernel_call registers{0x10000, 0, 0, 0, 0, 0, 0};
    const int64_t descriptor = maxps4_fex_engine_call_libkernel(engine, resolve("1G3lF1Gg1k8"), &registers, 1);
    CHECK(descriptor == 3);
    registers = {uint64_t(descriptor), 0x11000, 4, 0, 0, 0, 0};
    CHECK(maxps4_fex_engine_call_libkernel(engine, resolve("sceKernelRead"), &registers, 1) == 4);
    uint8_t bytes[16]{};
    CHECK(maxps4_fex_engine_read_ps4(engine, 0x11000, bytes, 4) == 0 && std::memcmp(bytes, "Kero", 4) == 0);
    registers = {4, 0x11000, 0, 0, 0, 0, 0};
    CHECK(maxps4_fex_engine_call_libkernel(engine, resolve("sceKernelClockGettime"), &registers, 1) == 0);
    const uint8_t output_pointer[] = {0, 1, 1, 0, 0, 0, 0, 0}; // logical 0x10100
    CHECK(maxps4_fex_engine_write_ps4(engine, 0x11108, output_pointer, 8) == 0);
    registers = {0, 0x4000, 3, 0x1002, uint64_t(-1LL), 0, 0x11100};
    CHECK(maxps4_fex_engine_call_libkernel(engine, resolve("sceKernelMmap"), &registers, 1) == 0);
    CHECK(maxps4_fex_engine_read_ps4(engine, 0x10100, bytes, 8) == 0);
    uint64_t mapped = 0;
    for (size_t i = 0; i < 8; ++i) mapped |= uint64_t(bytes[i]) << (8 * i);
    CHECK(mapped >= 0x1000000000ULL);
    CHECK(maxps4_fex_engine_write_ps4(engine, mapped, reinterpret_cast<const uint8_t*>("test"), 4) == 0);
    CHECK(maxps4_fex_engine_read_ps4(engine, mapped, bytes, 4) == 0 && std::memcmp(bytes, "test", 4) == 0);
    CHECK(maxps4_fex_engine_call_libkernel(engine, 0, &registers, 1) == -2147352498LL); // Orbis ENOSYS
    // A failed restage must close descriptors, clear the mount and drop mappings.
    CHECK(maxps4_fex_engine_stage_ps4(engine, nullptr, 0, &entry, &segments, &blockers) != 0);
    CHECK(maxps4_fex_engine_read_ps4(engine, mapped, bytes, 4) == -EFAULT);
    CHECK(maxps4_fex_engine_stage_ps4(engine, file.data(), file.size(), &entry, &segments, &blockers) == 0);
    registers = {uint64_t(descriptor), 0, 0, 0, 0, 0, 0};
    CHECK(maxps4_fex_engine_call_libkernel(engine, resolve("sceKernelClose"), &registers, 1) == -2147352567LL); // EBADF
    CHECK(::unlink(host_path) == 0 && ::rmdir(directory) == 0);
    std::puts("PASS staged libkernel VFS, virtual memory and clock bridge; no guest CPU dispatch");
    return 0;
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("BEGIN native ABI and real FEX context lifecycle test");
    // Same allocator implementation embedded in the native runtime. These
    // sizes/alignments are used by FEX's char and uint32_t containers.
    for (size_t alignment : {size_t{1}, size_t{4}, size_t{8}, size_t{64}}) {
        auto* memory = FEXCore::Allocator::aligned_alloc(alignment, 97);
        CHECK(memory && reinterpret_cast<uintptr_t>(memory) % alignment == 0);
        std::memset(memory, 0x5A, 97);
        FEXCore::Allocator::aligned_free(memory);
    }
    CHECK(FEXCore::Allocator::aligned_alloc(0, 97) == nullptr);
    CHECK(FEXCore::Allocator::aligned_alloc(3, 97) == nullptr);
    CHECK(maxps4_fex_engine_open(nullptr) == -EINVAL);
    CHECK(maxps4_fex_engine_stage(nullptr) == 0);
    maxps4_fex_engine_close(nullptr);
    CHECK(maxps4_jit_begin(nullptr, nullptr, nullptr) == -EINVAL);
#if !TARGET_OS_IPHONE
    uint64_t mailbox = 99, nonce = 99, page = 99;
    CHECK(maxps4_jit_begin(&mailbox, &nonce, &page) == -ENOTSUP);
    CHECK(mailbox == 0 && nonce == 0 && page == 0);
    CHECK(maxps4_jit_prepare() == -ENOTSUP);
#endif
    CHECK(maxps4_jit_cancel() == 0);
    CHECK(maxps4_jit_leases() == 0);
    CHECK(maxps4_fex_engine_initialize(nullptr) == -EINVAL);
    uint64_t probe_result = 99; uint32_t probe_backend = 99;
    CHECK(maxps4_fex_engine_probe(nullptr, &probe_result, &probe_backend) == -EINVAL);
    CHECK(probe_result == 0 && probe_backend == 0);
    CHECK(maxps4_native_capabilities() == 0);
    CHECK(maxps4_native_launch("probe") == -38);
    CHECK(maxps4_native_aether_cpu_probe() == 1);
    constexpr std::array signal_numbers {SIGILL, SIGSEGV, SIGBUS, SIGTRAP};
    std::array<struct sigaction, signal_numbers.size()> original_actions {};
    for (size_t i = 0; i < signal_numbers.size(); ++i)
        CHECK(::sigaction(signal_numbers[i], nullptr, &original_actions[i]) == 0);
    sigset_t original_mask {};
    CHECK(::pthread_sigmask(SIG_SETMASK, nullptr, &original_mask) == 0);
    for (unsigned repeat = 0; repeat != 3; ++repeat) {
        maxps4_fex_engine *first = nullptr, *second = nullptr;
        std::printf("OPEN contexts cycle=%u\n", repeat);
        CHECK(maxps4_fex_engine_open(&first) == 0 && first);
        CHECK(maxps4_fex_engine_open(&second) == 0 && second);
        CHECK(maxps4_fex_engine_stage(first) == MAXPS4_FEX_CONTEXT_CREATED);
        CHECK(systemServices(first) == 0);
        CHECK(stagedCPU(first, false) == 0);
        auto not_initialized = guest_test::elf();
        uint64_t pending_entry = 99;
        CHECK(maxps4_fex_engine_run_elf(first, not_initialized.data(), not_initialized.size(), 3,
            &probe_result, &probe_backend, &pending_entry) == -EAGAIN);
        CHECK(probe_result == 0 && probe_backend == 0 && pending_entry == 0x1000);
#if TARGET_OS_IPHONE
        CHECK(maxps4_fex_engine_probe(first, &probe_result, &probe_backend) == -EAGAIN);
        CHECK(probe_result == 0 && probe_backend == 0);
        CHECK(maxps4_fex_engine_initialize(first) == -EACCES);
        CHECK(maxps4_fex_engine_stage(first) == MAXPS4_FEX_CONTEXT_CREATED);
#else
        CHECK(maxps4_fex_engine_probe(first, &probe_result, &probe_backend) == -EAGAIN);
        CHECK(probe_result == 0 && probe_backend == 0);
        CHECK(maxps4_fex_engine_initialize(first) == 0);
        CHECK(maxps4_fex_engine_stage(first) == MAXPS4_FEX_DISPATCHER_INITIALIZED);
        CHECK(maxps4_fex_engine_initialize(first) == 0);
        CHECK(maxps4_fex_engine_initialize(second) == 0);
        std::puts("PASS real InitCore dispatcher initialized twice without reallocation");
        CHECK(stagedCPU(first, true) == 0);
#endif
#if !TARGET_OS_IPHONE
        CHECK(maxps4_fex_engine_probe(first, nullptr, &probe_backend) == -EINVAL);
        CHECK(probe_backend == 0);
        uint64_t elf_entry = 99;
        auto elf = guest_test::elf(100, 23, true);
        CHECK(maxps4_fex_engine_run_elf(first, elf.data(), elf.size(), 3,
            &probe_result, &probe_backend, &elf_entry) == 0);
        CHECK(elf_entry == 0x1000 && probe_result == 123 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT);
        {
            // Call of Duty's string lookup walk (default.elf 0x5b163a-0x5b165b, its exact
            // bytes) over the two table entries an iPhone run showed it spinning on: entry
            // 0x81 = [00049ba7:000020c0] holds key 0x20c0 (r15d) and 0x9ba7 = [00080081:00002ac1]
            // points back to it, so a walk from 0x9ba7 has to stop at 0x81 (ebx).
            const std::vector<uint8_t> code = {
                0x49, 0xbc, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,       // mov r12, 0x100000
                0x41, 0xc7, 0x84, 0x24, 0x08, 0x04, 0x00, 0x00, 0xa7, 0x9b, 0x04, 0x00, // [r12+0x81*8] = 0x49ba7
                0x41, 0xc7, 0x84, 0x24, 0x0c, 0x04, 0x00, 0x00, 0xc0, 0x20, 0x00, 0x00, // [r12+0x81*8+4] = 0x20c0
                0x41, 0xc7, 0x84, 0x24, 0x38, 0xdd, 0x04, 0x00, 0x81, 0x00, 0x08, 0x00, // [r12+0x9ba7*8] = 0x80081
                0x41, 0xc7, 0x84, 0x24, 0x3c, 0xdd, 0x04, 0x00, 0xc1, 0x2a, 0x00, 0x00, // [r12+0x9ba7*8+4] = 0x2ac1
                0x41, 0xbf, 0xc0, 0x20, 0x00, 0x00,                               // mov r15d, 0x20c0
                0xb8, 0xa7, 0x9b, 0x00, 0x00,                                     // mov eax, 0x9ba7
                0xbb, 0x83, 0x00, 0x00, 0x00,                                     // mov ebx, 0x83
                0x48, 0x89, 0xc2, 0x0f, 0x1f, 0x00, 0x89, 0xc1, 0x89, 0xde, 0x89, 0xc8, // default.elf 0x5b163a
                0x41, 0x8b, 0x04, 0xc4, 0x89, 0xcb, 0x25, 0xff, 0xff, 0x03, 0x00,
                0x45, 0x39, 0x7c, 0xd4, 0x04, 0x48, 0x89, 0xc2, 0x75, 0xe5,       // ... jne back to 0x5b1640
                0x89, 0xd8,                                                       // mov eax, ebx
                0xf4};                                                            // hlt
            std::vector<uint8_t> walk(184 + code.size() + 1, 0);
            const uint8_t ident[] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
            std::copy(std::begin(ident), std::end(ident), walk.begin());
            guest_test::put(walk, 2, 16, 2); guest_test::put(walk, 62, 18, 2); guest_test::put(walk, 1, 20, 4);
            guest_test::put(walk, 0x1000, 24, 8); guest_test::put(walk, 64, 32, 8); guest_test::put(walk, 64, 52, 2);
            guest_test::put(walk, 56, 54, 2); guest_test::put(walk, 2, 56, 2);
            guest_test::put(walk, 1, 64, 4); guest_test::put(walk, 5, 68, 4); guest_test::put(walk, 184, 72, 8);
            guest_test::put(walk, 0x1000, 80, 8); guest_test::put(walk, code.size(), 96, 8);
            guest_test::put(walk, code.size(), 104, 8); guest_test::put(walk, 1, 112, 8);
            guest_test::put(walk, 1, 120, 4); guest_test::put(walk, 6, 124, 4); guest_test::put(walk, 184 + code.size(), 128, 8);
            guest_test::put(walk, 0x100000, 136, 8); guest_test::put(walk, 1, 152, 8);
            guest_test::put(walk, 0x50000, 160, 8); guest_test::put(walk, 1, 168, 8);
            std::copy(code.begin(), code.end(), walk.begin() + 184);
            uint64_t found = 0, walk_entry = 0;
            uint32_t walk_backend = 0;
            const int32_t walked = maxps4_fex_engine_run_elf(first, walk.data(), walk.size(), 3, &found, &walk_backend, &walk_entry);
            std::printf("Call of Duty lookup walk: status %d, found 0x%llx, backend %u\n", walked,
                (unsigned long long)found, walk_backend);
            CHECK(walked == 0 && found == 0x81 && walk_backend == MAXPS4_CPU_FEX_ARM64_JIT);
            std::puts("PASS Call of Duty's string lookup walk stops at the entry holding its key under FEX");
        }
        auto pm4_elf = guest_test::elf(40, 2, true);
        pm4_elf.resize(207);
        guest_test::put(pm4_elf, 12, 152, 8);
        guest_test::put(pm4_elf, 0xc0012d00, 195, 4);
        guest_test::put(pm4_elf, 3, 199, 4);
        guest_test::put(pm4_elf, 2, 203, 4);
        uint32_t submitted[3] {99, 99, 99}; size_t written = 99;
        CHECK(maxps4_fex_engine_run_elf_pm4(first, pm4_elf.data(), pm4_elf.size(), 3,
            0x10000, 3, submitted, 3, &written, &probe_result, &probe_backend, &elf_entry) == 0);
        CHECK(written == 3 && submitted[0] == 0xc0012d00 && submitted[1] == 3 && submitted[2] == 2);
        CHECK(probe_result == 42 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT && elf_entry == 0x1000);
        submitted[0] = 99;
        for (uint64_t address : {uint64_t{0}, uint64_t{0x1003c}, uint64_t{0x10001}, UINT64_MAX - 3}) {
            CHECK(maxps4_fex_engine_run_elf_pm4(first, pm4_elf.data(), pm4_elf.size(), 3,
                address, 3, submitted, 3, &written, &probe_result, &probe_backend, &elf_entry) != 0);
            CHECK(written == 0 && submitted[0] == 99 && probe_result == 0 && probe_backend == 0);
        }
        CHECK(maxps4_fex_engine_run_elf_pm4(first, pm4_elf.data(), pm4_elf.size(), 3,
            0x10000, 3, submitted, 2, &written, &probe_result, &probe_backend, &elf_entry) == -EINVAL);
        CHECK(written == 0 && submitted[0] == 99);
        guest_test::put(pm4_elf, 0xc001ff00, 195, 4);
        CHECK(maxps4_fex_engine_run_elf_pm4(first, pm4_elf.data(), pm4_elf.size(), 3,
            0x10000, 3, submitted, 3, &written, &probe_result, &probe_backend, &elf_entry) == -ENOTSUP);
        CHECK(written == 0 && submitted[0] == 99 && probe_backend == 0);
        std::puts("PASS actual FEX execution -> same ELF arena PM4 snapshot; no CPU-generated commands claimed");
        for (uint32_t count : {3U, 6U, 3U}) {
            auto generated = guest_test::generated_pm4(count);
            CHECK(maxps4_fex_engine_run_elf_pm4(first, generated.data(), generated.size(), 6,
                0x10000, 3, submitted, 3, &written, &probe_result, &probe_backend, &elf_entry) == 0);
            CHECK(written == 3 && submitted[0] == 0xc0012d00 && submitted[1] == count && submitted[2] == 2);
            CHECK(probe_result == 42 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT);
            // The data segment had no file payload. These words came from real
            // x86 RIP-relative stores executed through FEX, not Swift or an interpreter.
            CHECK(maxps4_fex_engine_run_elf_pm4(first, generated.data(), generated.size(), 5,
                0x10000, 3, submitted, 3, &written, &probe_result, &probe_backend, &elf_entry) == -E2BIG);
            CHECK(written == 0 && probe_result == 0 && probe_backend == 0);
        }
        std::puts("PASS real FEX RIP-relative stores generate PM4 in initially zero-filled guest data");
        // Entry offset is honored; bytes preceding it must never execute.
        guest_test::put(elf, 0x1005, 24, 8);
        elf[189] = 0xB8;
        guest_test::put(elf, 77, 190, 4);
        CHECK(maxps4_fex_engine_run_elf(first, elf.data(), elf.size(), 2,
            &probe_result, &probe_backend, &elf_entry) == 0);
        CHECK(elf_entry == 0x1005 && probe_result == 77 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT);
        CHECK(maxps4_fex_engine_run_elf(first, elf.data(), elf.size(), 1,
            &probe_result, &probe_backend, &elf_entry) == -E2BIG);
        CHECK(probe_result == 0 && probe_backend == 0 && elf_entry == 0);
        elf[194] = 0xC3;
        CHECK(maxps4_fex_engine_run_elf(first, elf.data(), elf.size(), 3,
            &probe_result, &probe_backend, &elf_entry) == -ENOTSUP);
        CHECK(probe_result == 0 && probe_backend == 0);
        elf = guest_test::elf(UINT32_MAX, 2);
        CHECK(maxps4_fex_engine_run_elf(first, elf.data(), elf.size(), 3,
            &probe_result, &probe_backend, &elf_entry) == 0);
        CHECK(probe_result == 1 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT);
        // Reused mmap addresses must not run a cached prior ELF translation.
        for (uint32_t value = 50; value < 54; ++value) {
            elf = guest_test::elf(value, 9);
            CHECK(maxps4_fex_engine_run_elf(first, elf.data(), elf.size(), 3,
                &probe_result, &probe_backend, &elf_entry) == 0);
            CHECK(probe_result == value + 9 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT);
        }
        std::puts("PASS real ELF segments/entry -> FEX ARM64 JIT; varied RAX, overflow, admission and budget");
        for (unsigned probe = 0; probe != 2; ++probe) {
            std::printf("BEGIN real FEX ARM64 JIT fixed guest probe cycle=%u attempt=%u\n", repeat, probe);
            CHECK(maxps4_fex_engine_probe(first, &probe_result, &probe_backend) == 0);
            CHECK(probe_result == 42 && probe_backend == MAXPS4_CPU_FEX_ARM64_JIT);
            std::printf("PASS real FEX ARM64 JIT guest RAX=%llu backend=%u\n",
                static_cast<unsigned long long>(probe_result), probe_backend);
        }
#endif
        uint32_t cpuid[4]{};
        CHECK(maxps4_fex_engine_cpuid(first, 0, 0, cpuid) == 0);
        // Real FEX virtual CPU leaf, not an adapter-supplied fake CPUID response.
        char vendor[13]{};
        std::memcpy(vendor, &cpuid[1], 4);
        std::memcpy(vendor + 4, &cpuid[3], 4);
        std::memcpy(vendor + 8, &cpuid[2], 4);
        CHECK(cpuid[0] > 0 && std::strlen(vendor) == 12);
        std::printf("FEX context CPUID vendor=%s leaf=%u\n",vendor,cpuid[0]);
        maxps4_fex_engine_close(first);
        CHECK(maxps4_fex_engine_cpuid(second, 0, 0, cpuid) == 0);
        constexpr uint8_t block[] = {0xB8,40,0,0,0,0x05,2,0,0,0,0xC3};
        uint64_t result = 99; uint32_t backend = 99;
        CHECK(maxps4_fex_engine_run(second, block, sizeof(block), 32, &result, &backend) == 0);
        CHECK(result == 42 && backend == MAXPS4_CPU_RESTRICTED_INTERPRETER);
        CHECK(maxps4_fex_engine_run(second, block, sizeof(block), 1, &result, &backend) == -ENOTSUP);
        CHECK(result == 0 && backend == 0);
        constexpr uint8_t invalid[] = {0x0F,0x0B};
        CHECK(maxps4_fex_engine_run(second, invalid, sizeof(invalid), 32, &result, &backend) == -ENOTSUP);
        CHECK(result == 0 && backend == 0);
        CHECK(maxps4_fex_engine_run(nullptr, block, sizeof(block), 32, &result, &backend) == -EINVAL);
        CHECK(maxps4_fex_engine_cpuid(second, 0, 0, nullptr) == -EINVAL);
        maxps4_fex_engine_close(second);
        for (size_t i = 0; i < signal_numbers.size(); ++i) {
            struct sigaction action {};
            CHECK(::sigaction(signal_numbers[i], nullptr, &action) == 0);
            CHECK(action.sa_handler == original_actions[i].sa_handler);
            CHECK(action.sa_flags == original_actions[i].sa_flags);
            for (int signal = 1; signal < NSIG; ++signal)
                CHECK(sigismember(&action.sa_mask, signal) ==
                      sigismember(&original_actions[i].sa_mask, signal));
        }
        sigset_t mask {};
        CHECK(::pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0);
        for (int signal = 1; signal < NSIG; ++signal)
            CHECK(sigismember(&mask, signal) == sigismember(&original_mask, signal));
        std::puts("PASS runtime cleanup preserves host signal handlers and mask");
    }
    std::puts("PASS real FEX context lifecycle/CPUID and explicit interpreter fallback; device JIT NOT tested");
}



