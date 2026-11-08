// File reads and writes larger than one host buffer (1 MiB): sceKernelRead,
// sceKernelPread, sceKernelWrite and sceKernelPwrite move them whole. A read
// over 1 MiB used to fail with EINVAL, and Call of Duty's fastfile streaming
// then left stale data in its zone buffers.
#include "../Native/maxps4_ps4_libkernel.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace maxps4;
using namespace maxps4::ps4;
using S = PS4KernelService;

int main() {
    GuestELF image;
    auto elf = guest_test::elf(40, 2, true);
    guest_test::put(elf, 0x400000, 160, 8); // 4 MiB of guest memory from 0x10000.
    assert(image.load(elf.data(), elf.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Libkernel kernel(memory);
    auto path = [&](const char* value) {
        assert(memory.write(0x10000, value, std::strlen(value) + 1));
        return uint64_t(0x10000);
    };
    auto call = [&](S service, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) {
        return kernel.invoke(service, {a, b, c, d});
    };
    char root[] = "/tmp/maxps4-large-XXXXXX";
    assert(::mkdtemp(root));
    const std::string base(root);
    constexpr size_t big = 0x280000; // 2.5 MiB: three host pieces.
    std::vector<uint8_t> pattern(big + 5);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = uint8_t((i * 7 + 3) % 251);
    { std::ofstream out(base + "/big", std::ios::binary); out.write(reinterpret_cast<const char*>(pattern.data()), std::streamsize(pattern.size())); }
    assert(kernel.mount("/temp0", root) == 0);
    constexpr uint64_t buffer = 0x20000;
    std::vector<uint8_t> got(big);
    auto zero = [&] { std::vector<uint8_t> z(big, 0); assert(memory.write(buffer, z.data(), big)); };

    const auto fd = call(S::open, path("/temp0/big"), 0);
    assert(fd >= 0);
    zero();
    assert(call(S::pread, uint64_t(fd), buffer, big, 5) == int64_t(big)); // pread leaves the offset.
    assert(memory.read(buffer, got.data(), big) && std::memcmp(got.data(), pattern.data() + 5, big) == 0);
    zero();
    assert(call(S::read, uint64_t(fd), buffer, big) == int64_t(big));
    assert(memory.read(buffer, got.data(), big) && std::memcmp(got.data(), pattern.data(), big) == 0);
    assert(call(S::read, uint64_t(fd), buffer, big) == 5); // The rest, then end of file.
    assert(memory.read(buffer, got.data(), 5) && std::memcmp(got.data(), pattern.data() + big, 5) == 0);
    assert(call(S::read, uint64_t(fd), buffer, big) == 0);
    assert(call(S::close, uint64_t(fd)) == 0);

    const auto out = call(S::open, path("/temp0/out"), 0x601); // O_WRONLY | O_CREAT | O_TRUNC
    assert(out >= 0);
    assert(memory.write(buffer, pattern.data(), big));
    assert(call(S::write, uint64_t(out), buffer, big) == int64_t(big));
    assert(call(S::pwrite, uint64_t(out), buffer, 5, big) == 5);
    assert(call(S::close, uint64_t(out)) == 0);
    std::ifstream in(base + "/out", std::ios::binary);
    std::vector<uint8_t> written((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(written.size() == big + 5);
    assert(std::memcmp(written.data(), pattern.data(), big) == 0 && std::memcmp(written.data() + big, pattern.data(), 5) == 0);

    std::filesystem::remove_all(base);
    std::puts("PASS large file transfers: read, pread, write and pwrite over 1 MiB move every byte, offsets kept");
}
