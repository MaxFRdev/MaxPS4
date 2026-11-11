#include "../Native/maxps4_ps4_guest_memory.h"
#include "../Native/maxps4_ps4_kernel_path.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
    maxps4::GuestELF image;
    auto file = guest_test::generated_pm4();
    // This test also writes 1025-byte path fixtures at data+0x100. The PM4
    // fixture's default 64-byte data extent cannot contain those strings.
    guest_test::put(file, 0x2000, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    maxps4::PS4GuestMemory memory(image);
    uint8_t bytes[4] = {1, 2, 3, 4};
    uint8_t output[4] = {};
    assert(memory.write(0x10000, bytes, sizeof(bytes)));
    assert(memory.read(0x10000, output, sizeof(output)));
    for (unsigned i = 0; i < 4; ++i) assert(output[i] == bytes[i]);
    assert(memory.permitted(0x10000, 4, 6));
    assert(memory.permitted(0x10000, 4, 4));
    assert(memory.permitted(0x10000, 4, 2));
    assert(!memory.permitted(0x10000, 4, 1));
    assert(!memory.permitted(0x10000, 4, 0));
    assert(!memory.permitted(0x10000, 4, 8));
    assert(!memory.permitted(UINT64_MAX, 4, 4));
    assert(!memory.executable(0x10000, 4));
    assert(memory.executable(0x1000, 1));
    assert(!memory.write(0x1000, bytes, sizeof(bytes)));
    // A GPU no-copy view is restricted to image mappings. It is safe to keep
    // past the synchronous GNM callback; dynamic mappings must use copies
    // because the guest can unmap them while Metal still reads a buffer.
    assert(memory.gpu_view(0x10000, sizeof(bytes)));
    assert(!memory.gpu_view(UINT64_MAX, sizeof(bytes)));
    // RX code must be readable and executable but never writable.
    uint8_t code_byte = 0;
    assert(memory.read(0x1000, &code_byte, 1));
    assert(code_byte == 0xB8);
    uint8_t fetched = 0;
    assert(memory.fetch(0x1000, &fetched, 1) && fetched == code_byte);
    assert(!memory.fetch(0x10000, &fetched, 1));
    assert(!memory.fetch(0x1000, nullptr, 1));
    assert(!memory.fetch(0x1000, &fetched, 0));
    assert(!memory.fetch(UINT64_MAX, &fetched, 1));
    // Kernel-facing guest strings must terminate inside the caller's bound.
    const char guest_path[] = "/app0/boot";
    assert(memory.write(0x10020, guest_path, sizeof(guest_path)));
    char path_buffer[32] = {};
    assert(memory.read_cstring(0x10020, path_buffer, sizeof(path_buffer)));
    assert(std::strcmp(path_buffer, guest_path) == 0);
    char rejected_path[4] = {'X', 'Y', 'Z', '\0'};
    assert(!memory.read_cstring(0x10020, rejected_path, sizeof(rejected_path)));
    assert(rejected_path[0] == 'X' && rejected_path[1] == 'Y');
    assert(!memory.read_cstring(UINT64_MAX, rejected_path, sizeof(rejected_path)));
    assert(!memory.read_cstring(0x10020, nullptr, 4));
    assert(!memory.read_cstring(0x10020, rejected_path, 0));
    assert(!memory.read_cstring(0x10020, rejected_path, 4097));
    // Minimal libkernel pathname admission: no host path resolution yet.
    char kernel_path[maxps4::PS4KernelPath::max_path] = {};
    using PathResult = maxps4::PS4KernelPathResult;
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::ok);
    assert(std::strcmp(kernel_path, guest_path) == 0);
    const char traversal[] = "/app0/../secret";
    assert(memory.write(0x10020, traversal, sizeof(traversal)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    assert(std::strcmp(kernel_path, guest_path) == 0);
    const char other_mount[] = "/savedata/file";
    assert(memory.write(0x10020, other_mount, sizeof(other_mount)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::unsupported_mount);
    const char duplicate_slash[] = "/app0//file";
    assert(memory.write(0x10020, duplicate_slash, sizeof(duplicate_slash)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    const char trailing_slash[] = "/app0/dir/";
    assert(memory.write(0x10020, trailing_slash, sizeof(trailing_slash)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    const char windows_separator[] = "/app0/dir\\file";
    assert(memory.write(0x10020, windows_separator, sizeof(windows_separator)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    const char control_character[] = "/app0/dir\nfile";
    assert(memory.write(0x10020, control_character, sizeof(control_character)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    const char current_directory[] = "/app0/./file";
    assert(memory.write(0x10020, current_directory, sizeof(current_directory)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    const char parent_directory[] = "/app0/assets/../file";
    assert(memory.write(0x10020, parent_directory, sizeof(parent_directory)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::invalid_path);
    const char dot_filename[] = "/app0/.config";
    assert(memory.write(0x10020, dot_filename, sizeof(dot_filename)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::ok);
    assert(std::strcmp(kernel_path, dot_filename) == 0);
    const char valid_nested[] = "/app0/assets/sprite.png";
    assert(memory.write(0x10020, valid_nested, sizeof(valid_nested)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10020, kernel_path) == PathResult::ok);
    assert(std::strcmp(kernel_path, valid_nested) == 0);
    assert(maxps4::PS4KernelPath::read(memory, UINT64_MAX, kernel_path) == PathResult::invalid_guest_address);
    // libkernel paths are bounded to 1024 bytes including the NUL.
    // A nonterminated guest path must be rejected without changing output.
    char long_path[maxps4::PS4KernelPath::max_path + 1];
    long_path[0] = '/';
    long_path[1] = 'a';
    long_path[2] = 'p';
    long_path[3] = 'p';
    long_path[4] = '0';
    long_path[5] = '/';
    for (size_t i = 6; i < sizeof(long_path) - 1; ++i) long_path[i] = 'x';
    long_path[sizeof(long_path) - 1] = '\0';
    assert(memory.write(0x10100, long_path, sizeof(long_path)));
    assert(maxps4::PS4KernelPath::read(memory, 0x10100, kernel_path) == PathResult::invalid_guest_address);
    assert(std::strcmp(kernel_path, valid_nested) == 0);
    // The last legal byte is the terminator at index 1023.
    long_path[maxps4::PS4KernelPath::max_path - 1] = '\0';
    assert(memory.write(0x10100, long_path, maxps4::PS4KernelPath::max_path));
    assert(maxps4::PS4KernelPath::read(memory, 0x10100, kernel_path) == PathResult::ok);
    assert(std::strlen(kernel_path) == maxps4::PS4KernelPath::max_path - 1);
    // Strings must not cross into an unmapped segment, even when the
    // destination has enough capacity. A failure leaves it untouched.
    for (const auto& segment : image.segments()) {
        if ((segment.permissions & 6U) != 6U || segment.memory_size == 0) continue;
        const uint64_t last = segment.address + segment.memory_size - 1;
        const char unterminated = 'Q';
        assert(memory.write(last, &unterminated, 1));
        char boundary_string[3] = {'A', 'B', 'C'};
        assert(!memory.read_cstring(last, boundary_string, sizeof(boundary_string)));
        assert(boundary_string[0] == 'A' && boundary_string[1] == 'B' &&
               boundary_string[2] == 'C');
        const char terminator = '\0';
        assert(memory.write(last, &terminator, 1));
        assert(memory.read_cstring(last, boundary_string, sizeof(boundary_string)));
        assert(boundary_string[0] == '\0');
    }
    // An invalid instruction fetch must not change the destination buffer.
    fetched = 0xA7;
    assert(!memory.fetch(0x10000, &fetched, 1));
    assert(fetched == 0xA7);
    assert(!memory.fetch(0x1000 - 1, &fetched, 1));
    assert(fetched == 0xA7);
    assert(!memory.fetch(0x1000, &fetched, SIZE_MAX));
    assert(fetched == 0xA7);
    // An instruction fetch must not partially modify output when it crosses
    // the executable segment boundary.
    uint8_t unchanged_fetch[4] = {0x11, 0x22, 0x33, 0x44};
    uint64_t executable_end = 0;
    for (const auto& segment : image.segments()) {
        if ((segment.permissions & 5U) == 5U) {
            executable_end = segment.address + segment.memory_size;
            break;
        }
    }
    assert(executable_end > 0);
    assert(!memory.fetch(executable_end - 1, unchanged_fetch, sizeof(unchanged_fetch)));
    assert(unchanged_fetch[0] == 0x11 && unchanged_fetch[1] == 0x22 &&
           unchanged_fetch[2] == 0x33 && unchanged_fetch[3] == 0x44);
    // Fetch into a writable caller buffer and compare with the RX source.
    // Never pass a read-only code mapping as a memmove destination.
    uint8_t code_snapshot[4] = {};
    assert(memory.fetch(0x1000, code_snapshot, sizeof(code_snapshot)));
    const auto* code = image.translate(0x1000, sizeof(code_snapshot), 5);
    assert(code);
    for (size_t i = 0; i < sizeof(code_snapshot); ++i)
        assert(code_snapshot[i] == code[i]);
    assert(memory.executable(0x1000, 1));
    assert(!memory.executable(0x10000, 1));
    // A CPU client may query a bounded contiguous span without acquiring a
    // host pointer or crossing the current guest segment.
    assert(memory.contiguous(0x1000, 1, 5) == 1);
    assert(memory.contiguous(0x1000, 16, 2) == 0);
    assert(memory.contiguous(0x10000, 4, 6) == 4);
    // CPU span queries must preserve the full permission mask and reject
    // requests starting outside the segment or ending past its boundary.
    assert(memory.contiguous(0x10000, SIZE_MAX, 6) > 4);
    assert(memory.contiguous(0x10000, 4, 7) == 0);
    assert(memory.contiguous(0x1000, 4, 7) == 0);
    assert(memory.contiguous(0x10000 - 1, 4, 6) == 0);
    assert(memory.contiguous(0x10000, 4, 5) == 0);
    assert(memory.contiguous(0x10000, 0, 4) == 0);
    assert(memory.contiguous(0x10000, 4, 0) == 0);
    assert(memory.contiguous(0x10000, 4, 8) == 0);
    assert(memory.contiguous(UINT64_MAX, 4, 4) == 0);
    for (const auto& segment : image.segments()) {
        const uint64_t last = segment.address + segment.memory_size - 1;
        assert(memory.contiguous(last, 4, 4) == 1);
        assert(memory.contiguous(last, SIZE_MAX, 4) == 1);
        assert(memory.contiguous(last, SIZE_MAX, segment.permissions) == 1);
        assert(memory.contiguous(last, 1, segment.permissions | 8U) == 0);
        assert(memory.contiguous(segment.address + segment.memory_size, 1,
                                 segment.permissions) == 0);
    }

    // Every mapped segment must reject a range extending beyond its last
    // byte. Permission queries must not silently accept a partial interval.
    for (const auto& segment : image.segments()) {
        assert(segment.memory_size > 0);
        const uint64_t last = segment.address + segment.memory_size - 1;
        const uint32_t readable = segment.permissions & 4U;
        assert(readable == 4U);
        assert(memory.permitted(last, 1, readable));
        assert(!memory.permitted(last, 2, readable));
        assert(!memory.permitted(last, SIZE_MAX, readable));
        assert(!memory.permitted(last, 1, 0));
        assert(!memory.permitted(last, 1, 8));
    }

    // Reject requests straddling an unmapped boundary.
    assert(!memory.read(0x10000 - 1, output, sizeof(output)));
    assert(!memory.write(0x10000 - 1, bytes, sizeof(bytes)));
    // A failed read must not overwrite the caller's destination buffer.
    uint8_t unchanged[4] = {0xA5, 0x5A, 0xC3, 0x3C};
    assert(!memory.read(0x10000 - 1, unchanged, sizeof(unchanged)));
    assert(unchanged[0] == 0xA5 && unchanged[1] == 0x5A &&
           unchanged[2] == 0xC3 && unchanged[3] == 0x3C);
    // A rejected write must leave the valid data segment untouched.
    uint8_t before_write[4] = {};
    uint8_t after_write[4] = {};
    assert(memory.read(0x10000, before_write, sizeof(before_write)));
    assert(!memory.write(0x10000 - 1, bytes, sizeof(bytes)));
    assert(memory.read(0x10000, after_write, sizeof(after_write)));
    for (unsigned i = 0; i < 4; ++i) assert(after_write[i] == before_write[i]);


    // Reject writes crossing the RW segment boundary, without partial mutation.
    const auto& regions = image.segments();
    bool checked_boundary = false;
    for (const auto& region : regions) {
        if ((region.permissions & 7U) != 6U || region.memory_size < 4) continue;
        checked_boundary = true;
        const uint64_t last = region.address + region.memory_size - 1;
        uint8_t sentinel = 0;
        assert(memory.read(last, &sentinel, 1));
        assert(!memory.write(last, bytes, sizeof(bytes)));
        uint8_t after = 0;
        assert(memory.read(last, &after, 1));
        assert(after == sentinel);
    }
    assert(checked_boundary);
    assert(!memory.read(0x1000, output, 0));
    assert(!memory.write(0x10000, bytes, 0));

    assert(!memory.read(UINT64_MAX, output, sizeof(output)));
    assert(!memory.write(UINT64_MAX, bytes, sizeof(bytes)));
    assert(!memory.read(0x10000, nullptr, 4));
    assert(!memory.write(0x10000, nullptr, 4));
    // Host callers may supply overlapping buffers inside the staged RW arena.
    // Transfers must have memmove semantics, not undefined memcpy overlap.
    auto* rw = const_cast<uint8_t*>(image.translate(0x10000, 8, 6));
    assert(rw);
    for (uint8_t i = 0; i < 8; ++i) rw[i] = i;
    assert(memory.write(0x10002, rw, 6));
    for (uint8_t i = 0; i < 6; ++i) assert(rw[i + 2] == i);
    assert(memory.read(0x10002, rw, 6));
    for (uint8_t i = 0; i < 6; ++i) assert(rw[i] == i);
    // A failed PS4 staging attempt must discard the previous ELF arena.
    const uint8_t invalid_ps4[] = {0, 1, 2, 3};
    assert(image.stage_ps4(invalid_ps4, sizeof(invalid_ps4)) != 0);
    assert(image.segments().empty());
    assert(image.entry() == 0);
    assert(!memory.permitted(0x10000, 4, 4));
    // The span API must not retain stale permissions after restaging fails.
    assert(memory.contiguous(0x10000, 4, 4) == 0);
    assert(memory.contiguous(0x1000, 15, 5) == 0);
    assert(!memory.read(0x10000, output, 4));
    assert(!memory.write(0x10000, bytes, 4));
    image.clear();
    assert(!memory.read(0x10000, output, 4));
    std::puts("PS4 guest memory adapter checks passed");
}
