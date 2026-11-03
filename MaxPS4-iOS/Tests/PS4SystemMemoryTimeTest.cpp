#include "../Native/maxps4_ps4_libkernel.h"
#include "GuestELFFixture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unistd.h>

using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    GuestELF image;
    auto elf = guest_test::elf(40, 2, true);
    guest_test::put(elf, 8192, 160, 8);
    assert(image.load(elf.data(), elf.size(), 6) == 0);
    PS4VirtualMemory dynamic(image);
    PS4GuestMemory memory(image, &dynamic);
    PS4Libkernel kernel(memory, &dynamic);
    using S = PS4KernelService;
    const auto invoke = [&](S s, const PS4KernelCall& c) { return kernel.invoke(s, c); };
    uint8_t word[8]{};
    put_le(word, 0, 0x10100, 8);
    assert(memory.write(0x11008, word, 8));
    PS4KernelCall map{0, 3 * PS4VirtualMemory::page_size, 3, 0x1002, uint64_t(-1LL), 0, 0x11000};
    assert(invoke(S::mmap, map) == 0);
    assert(memory.read(0x10100, word, 8));
    const uint64_t address = get_le(word, 8), page = PS4VirtualMemory::page_size;
    assert(address >= PS4VirtualMemory::base && address % page == 0);
    assert(dynamic.allocated() == 3 * page);
    uint8_t byte = 0xff;
    assert(memory.read(address, &byte, 1) && byte == 0);
    byte = 42;
    assert(memory.write(address + page, &byte, 1));
    assert(!memory.fetch(address, &byte, 1));
    assert(memory.permitted(address, 3 * page, 6));
    assert(memory.contiguous(address + 1, SIZE_MAX, 6) == 3 * page - 1);
    assert(!memory.read(address + 3 * page - 1, word, 8));
    assert(invoke(S::mprotect, {address + page, page, 1}) == 0);
    assert(!memory.write(address + page, word, 1));
    assert(memory.read(address + page, &byte, 1) && byte == 42);
    assert(memory.contiguous(address, 3 * page, 2) == page);
    assert(invoke(S::mprotect, {address, page, 5}) == 0);
    assert(memory.fetch(address, &byte, 1));
    assert(!memory.write(address, word, 1));
    assert(invoke(S::mprotect, {address, page, 7}) == kernel_error(ps4::access));
    assert(memory.executable(address, 1)); // Failed protect preserves permissions.
    assert(invoke(S::mprotect, {address + 1, page, 3}) == kernel_error(inval));
    assert(invoke(S::mprotect, {address, page, 8}) == kernel_error(notsup));
    assert(invoke(S::mprotect, {address + 2 * page, page, 0}) == 0);
    assert(!memory.read(address + 2 * page, &byte, 1));
    assert(invoke(S::munmap, {address + page, page}) == 0);
    assert(!memory.read(address + page, &byte, 1));
    assert(!memory.read(address + page - 1, word, 8));
    assert(invoke(S::mprotect, {address, 3 * page, 3}) == kernel_error(nomem));
    assert(memory.executable(address, 1)); // No partial protect across a hole.
    assert(invoke(S::munmap, {address + page, page}) == kernel_error(nomem));
    assert(invoke(S::munmap, {address + 1, page}) == kernel_error(inval));
    assert(invoke(S::munmap, {address, 0}) == kernel_error(inval));
    assert(invoke(S::munmap, {address, page}) == 0);
    assert(invoke(S::munmap, {address + 2 * page, page}) == 0);
    assert(dynamic.allocated() == 0);
    // All map failures preserve the output pointer and allocation accounting.
    put_le(word, 0, 0xdeadbeef, 8); assert(memory.write(0x10100, word, 8));
    auto invalid = map; invalid.rdi = 0x10000;
    assert(invoke(S::mmap, invalid) == kernel_error(notsup));
    invalid = map; invalid.rcx = 0x1012;
    assert(invoke(S::mmap, invalid) == kernel_error(notsup));
    invalid = map; invalid.r8 = 3;
    assert(invoke(S::mmap, invalid) == kernel_error(inval));
    invalid = map; invalid.r9 = 1;
    assert(invoke(S::mmap, invalid) == kernel_error(inval));
    invalid = map; invalid.rsi = UINT64_MAX;
    assert(invoke(S::mmap, invalid) == kernel_error(inval));
    invalid = map; invalid.rsi = PS4VirtualMemory::budget + 1;
    assert(invoke(S::mmap, invalid) == kernel_error(nomem));
    invalid = map; invalid.rsp = UINT64_MAX;
    assert(invoke(S::mmap, invalid) == kernel_error(fault));
    assert(memory.read(0x10100, word, 8) && get_le(word, 8) == 0xdeadbeef);
    assert(dynamic.allocated() == 0);
    put_le(word, 0, 0x1000, 8); assert(memory.write(0x11008, word, 8));
    assert(invoke(S::mmap, map) == kernel_error(fault));
    assert(dynamic.allocated() == 0);
    uint64_t output = 0xdeadbeef;
    assert(dynamic.map(0, 1, 3, 0x1002, -1, 0, output) == ok);
    assert(dynamic.allocated() == page);
    dynamic.reset();
    assert(dynamic.map(0, PS4VirtualMemory::budget, 3, 0x1002, -1, 0, output) == ok);
    uint64_t unchanged = 42;
    assert(dynamic.map(0, 1, 3, 0x1002, -1, 0, unchanged) == nomem && unchanged == 42);
    kernel.reset(); assert(dynamic.allocated() == 0);
    // Direct memory: physical ranges allocated first-fit, then mapped.
    constexpr uint64_t phys = 0x11200, where = 0x11208, name = 0x11210;
    assert(invoke(S::direct_size, {}) == int64_t(5056ULL << 20));
    assert(invoke(S::direct_allocate, {0, 1ULL << 30, 0x8000, 0, 0, phys}) == 0);
    assert(memory.read(phys, word, 8) && get_le(word, 8) == 0);
    assert(invoke(S::direct_allocate, {0, 1ULL << 30, 0x4000, 0x10000, 0, phys}) == 0);
    assert(memory.read(phys, word, 8) && get_le(word, 8) == 0x10000);
    assert(invoke(S::direct_allocate, {0, 0x8000, 0x4000, 0, 0, phys}) == kernel_error(again));
    assert(invoke(S::direct_allocate, {0, 1ULL << 30, 0x1000, 0, 0, phys}) == kernel_error(inval));
    put_le(word, 0, 0, 8); assert(memory.write(where, word, 8));
    assert(invoke(S::direct_map, {where, 0x8000, 0x33, 0, 0, 0}) == 0 && memory.read(where, word, 8));
    assert(memory.permitted(get_le(word, 8), 0x8000, 6)); // GPU+CPU read/write: CPU read/write.
    assert(invoke(S::direct_map, {where, 0x8000, 3, 0, 0x20000, 0}) == kernel_error(inval)); // Not allocated.
    assert(invoke(S::direct_map, {where, 0x4000, 7, 0, 0, 0}) == kernel_error(inval)); // Executable.
    // Neither a past-end offset nor a wrapping physical range is allocated.
    assert(invoke(S::direct_map, {where, 0x4000, 3, 0, 0x14000, 0}) == kernel_error(inval));
    assert(invoke(S::direct_map, {where, UINT64_MAX - 0x3fff, 3, 0, 0x10000, 0}) == kernel_error(inval));
    assert(invoke(S::direct_release, {0, 0x8000}) == 0 && invoke(S::direct_release, {0, 0x8000}) == kernel_error(noent));
    // The largest free direct range: [0, 0x10000) before the one at 0x10000.
    constexpr uint64_t size_out = 0x11220;
    assert(invoke(S::direct_available, {0, 0x20000, 0x4000, phys, size_out}) == 0);
    assert(memory.read(phys, word, 8) && get_le(word, 8) == 0 && memory.read(size_out, word, 8) && get_le(word, 8) == 0x10000);
    assert(invoke(S::direct_allocate_main, {0x4000, 0, 0, phys}) == 0 && memory.read(phys, word, 8) && get_le(word, 8) == 0);
    assert(invoke(S::direct_available, {0x10000, 0x14000, 0x4000, phys, size_out}) == kernel_error(nomem));
    // Flexible memory needs a name; POSIX mmap returns the address, or -1.
    const char flexible[] = "heap";
    assert(memory.write(name, flexible, sizeof(flexible)));
    put_le(word, 0, 0, 8); assert(memory.write(where, word, 8));
    assert(invoke(S::flexible_map, {where, 0x4000, 3, 0, name}) == 0 && memory.read(where, word, 8));
    assert(memory.permitted(get_le(word, 8), 0x4000, 6));
    assert(invoke(S::flexible_map, {where, 0x4000, 3, 0, 0}) == kernel_error(fault));
    // Unnamed flexible memory, queried and released.
    put_le(word, 0, 0, 8); assert(memory.write(where, word, 8));
    assert(invoke(S::flexible_map_unnamed, {where, 0x8000, 1, 0}) == 0 && memory.read(where, word, 8));
    const uint64_t flexible_at = get_le(word, 8);
    constexpr uint64_t start_out = 0x11228, end_out = 0x11230, prot_out = 0x11238, info = 0x11300;
    assert(invoke(S::query_protection, {flexible_at + 0x4000, start_out, end_out, prot_out}) == 0);
    assert(memory.read(start_out, word, 8) && get_le(word, 8) == flexible_at);
    assert(memory.read(end_out, word, 8) && get_le(word, 8) == flexible_at + 0x8000);
    assert(memory.read(prot_out, word, 4) && get_le(word, 4) == 1); // Read only.
    assert(invoke(S::virtual_query, {flexible_at, 0, info, 0x48}) == 0 && memory.read(info, word, 8) && get_le(word, 8) == flexible_at);
    assert(invoke(S::flexible_release, {flexible_at, 0x8000}) == 0);
    assert(invoke(S::query_protection, {flexible_at, start_out, end_out, prot_out}) == kernel_error(ps4::access));
    assert(invoke(S::sdk_version, {prot_out}) == 0 && memory.read(prot_out, word, 4) && get_le(word, 4) == 0x09008001);
    const int64_t anonymous = kernel.invoke(S::posix_mmap, {0, 0x4000, 3, 0x1002, uint64_t(-1LL), 0}, 1, true);
    assert(anonymous > 0 && memory.permitted(uint64_t(anonymous), 0x4000, 6));
    assert(kernel.invoke(S::posix_mmap, {0, 0x4000, 3, 0x2, 3, 0}, 1, true) == -1); // File mapping.
    // A reserved range has no access until Mtypeprotect opens it (GPU bits: CPU read/write).
    put_le(word, 0, 0, 8); assert(memory.write(where, word, 8));
    const uint64_t before_reserve = dynamic.allocated();
    assert(invoke(S::reserve_range, {where, 0x8000, 0, 1}) == kernel_error(inval));
    assert(invoke(S::reserve_range, {where, 0x8000, 0, 0xc000}) == kernel_error(inval));
    assert(invoke(S::reserve_range, {where, 0x8000, 0, 0x100000000ULL}) == kernel_error(inval));
    assert(invoke(S::reserve_range, {where, 0x8000, 0x10, 0}) == kernel_error(inval));
    assert(invoke(S::flexible_map_unnamed, {where, 0x4000, 3, 0x10}) == kernel_error(inval));
    assert(invoke(S::flexible_map, {where, 0x4000, 3, 0x10, name}) == kernel_error(inval));
    assert(dynamic.allocated() == before_reserve && memory.read(where, word, 8) && get_le(word, 8) == 0);
    assert(invoke(S::reserve_range, {where, 0x8000, 0, 0x8000}) == 0 && memory.read(where, word, 8));
    const uint64_t reserved = get_le(word, 8);
    assert(reserved % 0x8000 == 0 && !memory.permitted(reserved, 1, 4));
    assert(invoke(S::mtype_protect, {reserved, 0x4000, 3, 0x32}) == 0 && memory.permitted(reserved, 0x4000, 6));
    assert(!memory.permitted(reserved + 0x4000, 1, 4) && invoke(S::mtype_protect, {reserved, 0x4000, 3, 4}) == kernel_error(inval));
    assert(invoke(S::flexible_release, {reserved, 0x8000}) == 0);
    // Direct allocations [0, 0x4000) and [0x10000, 0x14000): the one holding an offset, or the next.
    assert(invoke(S::direct_query, {0x2000, 0, info, 24}) == 0 && memory.read(info + 8, word, 8) && get_le(word, 8) == 0x4000);
    assert(invoke(S::direct_query, {0x8000, 0, info, 24}) == kernel_error(noent));
    assert(invoke(S::direct_query, {0x8000, 3, info, 24}) == kernel_error(noent));
    assert(invoke(S::direct_query, {0x8000, 1, info, 24}) == 0 && memory.read(info, word, 8) && get_le(word, 8) == 0x10000);
    assert(invoke(S::direct_query, {0x20000, 1, info, 24}) == kernel_error(noent) && invoke(S::direct_query, {0, 0, info, 8}) == kernel_error(inval));
    // BatchMap2: map flexible and direct memory, then protect, unmap, and stop at an unknown operation.
    constexpr uint64_t entries = 0x11400, done_out = 0x11480;
    auto entry = [&](size_t i, uint64_t start, uint64_t offset, uint64_t length, uint8_t protection, int32_t operation) {
        uint8_t e[32]{};
        put_le(e, 0, start, 8); put_le(e, 8, offset, 8); put_le(e, 16, length, 8);
        e[24] = protection; put_le(e, 28, uint32_t(operation), 4);
        assert(memory.write(entries + 32 * i, e, sizeof(e)));
    };
    entry(0, 0, 0, 0x4000, 3, 3); entry(1, 0, 0, 0x4000, 0x33, 0);
    assert(invoke(S::batch_map2, {entries, 2, done_out, 0}) == 0 && memory.read(done_out, word, 4) && get_le(word, 4) == 2);
    assert(memory.read(entries, word, 8));
    const uint64_t batch_flexible = get_le(word, 8);
    assert(memory.read(entries + 32, word, 8));
    const uint64_t batch_direct = get_le(word, 8);
    assert(memory.permitted(batch_flexible, 0x4000, 6) && memory.permitted(batch_direct, 0x4000, 6));
    entry(0, batch_flexible, 0, 0x4000, 1, 2); entry(1, batch_direct, 0, 0x4000, 0, 1); entry(2, 0, 0, 0x4000, 3, 9);
    assert(invoke(S::batch_map2, {entries, 3, done_out, 0}) == kernel_error(inval) && memory.read(done_out, word, 4) && get_le(word, 4) == 2);
    assert(memory.permitted(batch_flexible, 0x4000, 4) && !memory.permitted(batch_flexible, 1, 2));
    // Type-protect (operation 4) also translates GPU-only access to CPU access.
    entry(0, batch_flexible, 0, 0x4000, 0x30, 4);
    assert(invoke(S::batch_map2, {entries, 1, done_out, 0}) == 0);
    assert(memory.permitted(batch_flexible, 0x4000, 6));
    assert(!memory.permitted(batch_direct, 1, 4) && invoke(S::flexible_release, {batch_flexible, 0x4000}) == 0);
    // Memory pools: a 2 MiB reservation, committed and decommitted by 64 KiB.
    put_le(word, 0, 0, 8); assert(memory.write(where, word, 8));
    assert(invoke(S::pool_reserve, {0, 0x200000, 0, 0, where}) == 0 && memory.read(where, word, 8));
    const uint64_t pool = get_le(word, 8);
    assert(pool % 0x200000 == 0 && !memory.permitted(pool, 1, 4));
    assert(invoke(S::pool_commit, {pool, 0x10000, 0, 0x33, 0}) == 0 && memory.permitted(pool, 0x10000, 6));
    assert(invoke(S::pool_decommit, {pool, 0x10000, 0}) == 0 && !memory.permitted(pool, 1, 4));
    assert(invoke(S::pool_commit, {pool, 0x8000, 0, 3, 0}) == kernel_error(inval));
    assert(invoke(S::pool_reserve, {0, 0x100000, 0, 0, where}) == kernel_error(inval));
    assert(invoke(S::flexible_available, {where}) == 0 && memory.read(where, word, 8));
    assert(get_le(word, 8) == 448ULL << 20);
    assert(invoke(S::flexible_release, {pool, 0x200000}) == 0);
    assert(invoke(S::pool_expand, {0, 1ULL << 30, 0x10000, 0, phys}) == 0 && memory.read(phys, word, 8));
    // BatchMap maps at the given address: outside a general image's window, refused.
    entry(0, PS4VirtualMemory::base, 0, 0x4000, 3, 3);
    assert(invoke(S::batch_map, {entries, 1, done_out}) == kernel_error(inval) && memory.read(done_out, word, 4) && get_le(word, 4) == 0);
    assert(invoke(S::batch_map2, {UINT64_MAX - 15, 1, done_out, 0}) == kernel_error(fault));
    assert(memory.read(done_out, word, 4) && get_le(word, 4) == 0);
    assert(invoke(S::batch_map2, {entries, uint64_t(-1LL), done_out, 0}) == kernel_error(inval));
    assert(invoke(S::batch_map2, {entries, 0, done_out, 0}) == 0);
    assert(memory.read(done_out, word, 4) && get_le(word, 4) == 0);
    assert(invoke(S::gnm_unsupported, {}) == signed32(0x8eee00ffU));
    assert(invoke(S::gnm_gs_rings, {0x400000, 0x800000}) == 0);
    assert(invoke(S::gnm_gs_rings, {0x300000, 0x800000}) == 0x08000000);
    assert(invoke(S::gnm_gs_rings, {0x400001, 0x800000}) == 0x08000000);
    assert(invoke(S::gnm_gs_rings, {0x400000, 0x900000}) == 0x08000000);
    kernel.reset(); assert(dynamic.allocated() == 0);
    // Allocation must skip a staged ELF segment at the default virtual base.
    GuestELF high_image;
    const long host_page = ::sysconf(_SC_PAGESIZE);
    assert(host_page > 0);
    const uint64_t data_gap = uint64_t(host_page) > page ? uint64_t(host_page) : page;
    guest_test::put(elf, PS4VirtualMemory::base, 80, 8);
    guest_test::put(elf, PS4VirtualMemory::base, 24, 8);
    guest_test::put(elf, PS4VirtualMemory::base + data_gap, 136, 8);
    assert(high_image.load(elf.data(), elf.size(), 6) == 0);
    PS4VirtualMemory high_dynamic(high_image);
    assert(high_dynamic.map(0, 1, 3, 0x1002, -1, 0, output) == ok);
    assert(output >= PS4VirtualMemory::base + page);
    assert(output + page <= PS4VirtualMemory::base + data_gap ||
           output >= PS4VirtualMemory::base + data_gap + page);
    // Clock wire structs use two signed 64-bit words, including on Apple ARM64.
    uint8_t time[16]{};
    for (uint32_t id : {0U, 4U}) {
        assert(invoke(S::clock_gettime, {id, 0x11100}) == 0);
        assert(memory.read(0x11100, time, sizeof(time)));
        assert(get_le(time, 8) > 0 && get_le(time + 8, 8) < 1000000000);
        const uint64_t seconds = get_le(time, 8), nanos = get_le(time + 8, 8);
        assert(invoke(S::clock_gettime, {id, 0x11100}) == 0);
        assert(memory.read(0x11100, time, sizeof(time)));
        if (id == 4) assert(get_le(time, 8) > seconds ||
            (get_le(time, 8) == seconds && get_le(time + 8, 8) >= nanos));
        assert(invoke(S::clock_getres, {id, 0x11100}) == 0);
        assert(memory.read(0x11100, time, sizeof(time)));
        assert(get_le(time, 8) || get_le(time + 8, 8));
    }
    assert(invoke(S::clock_gettime, {1, 0x11100}) == kernel_error(inval));
    assert(invoke(S::clock_gettime, {4, 0x1000}) == kernel_error(fault));
    assert(invoke(S::gettimeofday, {0x11100}) == 0);
    assert(memory.read(0x11100, time, sizeof(time)));
    assert(get_le(time, 8) > 0 && get_le(time + 8, 8) < 1000000);
    assert(invoke(S::gettimeofday, {0}) == kernel_error(fault));
    const auto elapsed = invoke(S::process_time, {});
    assert(elapsed >= 0 && invoke(S::process_time, {}) >= elapsed);
    assert(resolve_libkernel("PGhQHd-dzv8", "libkernel", "libkernel", 1) == S::mmap);
    assert(resolve_libkernel("QBi7HCK03hw", "libkernel", "libkernel", 1) == S::clock_gettime);
    assert(resolve_libkernel("7oxv3PPCumo", "libkernel", "libkernel", 1) == S::reserve_range);
    assert(resolve_libkernel("2SKEx6bSq-4", "libkernel", "libkernel", 1) == S::batch_map);
    assert(resolve_libkernel("kBJzF8x4SyE", "libkernel", "libkernel", 1) == S::batch_map2);
    assert(resolve_libkernel("nvEwfYAImTs", "libSceGnmDriver", "libSceGnmDriver", 1) == S::gnm_unsupported);
    assert(resolve_libkernel("jtkqXpAOY6w", "libSceGnmDriver", "libSceGnmDriver", 1) == S::gnm_gs_rings);
    std::puts("PS4 virtual memory and clock checks passed");
}
