#include "../Native/maxps4_ps4_libkernel.h"
#include "../Native/maxps4_fex_runtime.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace maxps4;
using namespace maxps4::ps4;
int main() {
    static_assert(kernel_error(fault) == -2147352562LL);
    assert(host_error(EAGAIN) == again && uint32_t(again) == 35);
    GuestELF image;
    auto elf = guest_test::elf(40, 2, true);
    guest_test::put(elf, 8192, 160, 8);
    assert(image.load(elf.data(), elf.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Libkernel kernel(memory);
    auto path = [&](const char* value) {
        assert(memory.write(0x10000, value, std::strlen(value) + 1));
    };
    auto call = [&](PS4KernelService service, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
        return kernel.invoke(service, {a, b, c});
    };
    using S = PS4KernelService;
    path("/app0/data.bin");
    assert(call(S::open, 0x10000) == kernel_error(noent));
    char root[] = "/tmp/maxps4-kernel-XXXXXX";
    assert(::mkdtemp(root));
    const std::string base(root), file = base + "/data.bin", dir = base + "/sub";
    assert(::mkdir(dir.c_str(), 0700) == 0);
    const char payload[] = "Kero fixture";
    const int host = ::open(file.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
    assert(host >= 0 && ::write(host, payload, sizeof(payload) - 1) == sizeof(payload) - 1);
    assert(::close(host) == 0);
    assert(::link(file.c_str(), (dir + "/nested").c_str()) == 0);
    assert(::symlink(file.c_str(), (base + "/link").c_str()) == 0);
    assert(::symlink("/tmp", (base + "/escape").c_str()) == 0);
    assert(::mkfifo((base + "/fifo").c_str(), 0600) == 0);
    assert(kernel.mount_app0(root) == 0);
    const auto fd = call(S::open, 0x10000);
    assert(fd == 3);
    assert(call(S::read, fd, 0x11000, 4) == 4);
    char bytes[32] = {};
    assert(memory.read(0x11000, bytes, 4) && std::memcmp(bytes, "Kero", 4) == 0);
    // Invalid buffers must not advance the file offset or partially mutate data.
    assert(call(S::read, fd, 0x1000, 4) == kernel_error(fault));
    assert(call(S::read, fd, 0x11fff, 4) == kernel_error(fault));
    assert(call(S::lseek, fd, 0, 1) == 4);
    assert(call(S::read, fd, UINT64_MAX, 0) == 0);
    assert(call(S::read, fd, 0x11000, UINT64_MAX) == kernel_error(inval));
    assert(call(S::lseek, fd, uint64_t(-7LL), 2) == 5);
    assert(call(S::read, fd, 0x11000, 32) == 7);
    assert(memory.read(0x11000, bytes, 7) && std::memcmp(bytes, "fixture", 7) == 0);
    assert(call(S::read, fd, 0x11000, 4) == 0);
    assert(call(S::lseek, fd, uint64_t(-1LL), 0) == kernel_error(inval));
    assert(call(S::lseek, fd, 0, 3) == kernel_error(inval));
    assert(call(S::stat, 0x10000, 0x11200) == 0);
    uint8_t wire[PS4Libkernel::stat_size];
    assert(memory.read(0x11200, wire, sizeof(wire)));
    assert(get_le(wire + 72, 8) == sizeof(payload) - 1);
    assert((get_le(wire + 8, 2) & 0170000) == 0100000);
    assert((get_le(wire + 8, 2) & 0222) == 0);
    assert(call(S::fstat, fd, 0x11300) == 0);
    uint8_t second[sizeof(wire)];
    assert(memory.read(0x11300, second, sizeof(second)));
    assert(get_le(second + 72, 8) == get_le(wire + 72, 8));
    assert(call(S::stat, 0x10000, 0x11fff) == kernel_error(fault));
    assert(call(S::close, fd) == 0 && call(S::close, fd) == kernel_error(badf));
    assert(call(S::read, fd, 0x11000, 0) == kernel_error(badf));
    assert(call(S::read, 0, 0x11000, 1) == kernel_error(badf));
    path("/app0/sub/nested");
    const auto nested = call(S::open, 0x10000);
    assert(nested == 3 && call(S::read, nested, 0x11000, 4) == 4);
    assert(call(S::close, nested) == 0);
    // Names in another case reach the same files (the PS4 ignores case);
    // a link reached that way is still refused.
    for (const char* folded : {"/app0/SUB/Nested", "/app0/Data.BIN"}) {
        path(folded);
        const auto other = call(S::open, 0x10000);
        assert(other == 3 && call(S::read, other, 0x11000, 4) == 4);
        assert(memory.read(0x11000, bytes, 4) && std::memcmp(bytes, "Kero", 4) == 0);
        assert(call(S::close, other) == 0);
    }
    path("/app0/LINK"); assert(call(S::open, 0x10000) == kernel_error(loop));
    for (const char* invalid : {"/app0/../data.bin", "/app0//data.bin", "/app0/./data.bin", "relative"}) {
        path(invalid); assert(call(S::open, 0x10000) == kernel_error(inval));
    }
    for (const char* missing : {"/app0/missing", "/app01/data.bin", "/savedata0/file"}) {
        path(missing); assert(call(S::open, 0x10000) == kernel_error(noent));
    }
    path("/app0/link"); assert(call(S::open, 0x10000) == kernel_error(loop));
    path("/app0/escape/file"); assert(call(S::open, 0x10000) < 0);
    path("/app0/fifo"); assert(call(S::open, 0x10000) == kernel_error(notsup));
    path("/app0/data.bin");
    for (uint32_t flags : {1U, 2U, 0x200U, 0x400U, 8U})
        assert(call(S::open, 0x10000, flags) == kernel_error(rofs));
    assert(call(S::open, 0x10000, 3) == kernel_error(inval));
    assert(call(S::open, 0x10000, 4) == kernel_error(notsup));
    assert(call(S::open, UINT64_MAX) == kernel_error(fault));
    assert(call(S::open, 0x10000, 0x20000) == kernel_error(notdir));
    path("/app0/");
    assert(call(S::stat, 0x10000, 0x11200) == 0);
    const auto directory = call(S::open, 0x10000, 0x20000);
    assert(directory == 3);
    assert(call(S::read, directory, 0x11000, 1) == kernel_error(isdir));
    assert(call(S::close, directory) == 0);
    path("/app0/data.bin");
    std::vector<int64_t> descriptors;
    for (size_t i = 3; i < PS4Libkernel::descriptor_limit; ++i) {
        const auto next = call(S::open, 0x10000);
        assert(next == int64_t(i)); descriptors.push_back(next);
    }
    assert(call(S::open, 0x10000) == kernel_error(mfile));
    assert(kernel.mount_app0("/path/that/does/not/exist") == kernel_error(noent));
    assert(call(S::fstat, descriptors[0], 0x11200) == 0);
    assert(kernel.mount_app0(root) == 0);
    assert(call(S::close, descriptors[0]) == kernel_error(badf));
    assert(call(S::open, 0x10000) == 3);
    assert(resolve_libkernel("1G3lF1Gg1k8", "libkernel", "libkernel", 1) == S::open);
    assert(resolve_libkernel("sceKernelRead", "libkernel", "libkernel", 1) == S::read);
    assert(maxps4_ps4_resolve_libkernel("sceKernelRead", "libkernel", "libkernel", 1) == uint32_t(S::read));
    assert(maxps4_ps4_resolve_libkernel(nullptr, "libkernel", "libkernel", 1) == 0);
    assert(resolve_libkernel("bY-PO6JhzhQ", "libkernel", "libkernel", 1) == S::close); // POSIX close, errno results.
    // libSceLibcInternal's internal_* functions are guest routines.
    {
        size_t count = 0;
        const auto* exports = libkernel_exports(count);
        size_t routines = 0;
        for (size_t i = 0; i < count; ++i)
            if (!std::strcmp(exports[i].library, "libSceLibcInternal") && exports[i].code) {
                assert(exports[i].code_size && exports[i].code_size <= 4096 && exports[i].code[exports[i].code_size - 1] == 0xc3);
                ++routines;
            }
        assert(routines == 21);
        assert(resolve_libkernel("Q3VBxCXhUHs", "libSceLibcInternal", "libSceLibcInternal", 1) != S::unresolved);
    }
    assert(resolve_libkernel("QOQtbeDqsT4", "libSceAudioOut", "libSceAudioOut", 1) == S::audio_output);
    assert(resolve_libkernel("ekNvsT22rsY", "libSceAudioOut", "libSceAudioOut", 1) == S::audio_open);
    assert(resolve_libkernel("JfEPXVxhFqA", "libSceAudioOut", "libSceAudioOut", 1) == S::audio_init);
    assert(resolve_libkernel("sceKernelOpen", "libScePosix", "libkernel", 1) == S::unresolved);
    assert(resolve_libkernel("sceKernelOpen", "libkernel", "other", 1) == S::unresolved);
    assert(resolve_libkernel("sceKernelOpen", "libkernel", "libkernel", 2) == S::unresolved);
    assert(resolve_libkernel("1G3lF1Gg1k8#A#B", "libkernel", "libkernel", 1) == S::unresolved);
    assert(call(S::unresolved) == kernel_error(nosys));
    // POSIX semaphores over kernel ones; a UUID; IPv4 text and back.
    assert(call(S::sem_init, 0x11100, 0, 1) == 0);
    assert(call(S::sem_wait, 0x11100) == 0 && call(S::sem_post, 0x11100) == 0 && call(S::sem_wait, 0x11100) == 0);
    {   // A deadline already past: taken if the count allows, otherwise timed out.
        const uint8_t past[16] {};
        uint8_t wrong[16] {};
        wrong[15] = 0x7f; // Nanoseconds out of range.
        assert(memory.write(0x11120, past, sizeof(past)) && memory.write(0x11130, wrong, sizeof(wrong)));
        assert(call(S::sem_timedwait, 0x11100, 0x11120) == kernel_error(timedout));
        assert(call(S::sem_timedwait, 0x11100, 0x11130) == kernel_error(inval));
        assert(call(S::sem_timedwait, 0x11100, 0) == kernel_error(inval));
        assert(call(S::sem_post, 0x11100) == 0 && call(S::sem_timedwait, 0x11100, 0x11120) == 0);
    }
    assert(resolve_libkernel("w5IHyvahg-o", "libScePosix", "libkernel", 1) == S::sem_timedwait);
    assert(resolve_libkernel("JGMio+21L4c", "libScePosix", "libkernel", 1) == S::mkdir);
    assert(resolve_libkernel("NN01qLRhiqU", "libScePosix", "libkernel", 1) == S::rename);
    assert(call(S::sem_init, 0x11108, 1, 0) == kernel_error(notsup));
    assert(call(S::sem_destroy, 0x11100) == 0 && call(S::sem_post, 0x11100) == kernel_error(srch));
    assert(call(S::uuid_create, 0x11200) == 0);
    {
        uint8_t uuid[16];
        assert(memory.read(0x11200, uuid, sizeof(uuid)) && (uuid[6] >> 4) == 4 && (uuid[8] >> 6) == 2);
    }
    path("192.168.0.42");
    assert(call(S::net_pton, 2, 0x10000, 0x11300) == 1);
    assert(kernel.invoke(S::net_ntop, {2, 0x11300, 0x11400, 16}) == 0x11400);
    {
        char text[13];
        assert(memory.read(0x11400, text, sizeof(text)) && std::strcmp(text, "192.168.0.42") == 0);
    }
    assert(kernel.invoke(S::net_ntop, {2, 0x11300, 0x11400, 12}) == 0);
    path("1.2.3.256");
    assert(call(S::net_pton, 2, 0x10000, 0x11300) == 0 && call(S::net_pton, 28, 0x10000, 0x11300) == 0);
    assert(resolve_libkernel("Q4qBuN-c0ZM", "libSceNet", "libSceNet", 1) == S::net_unavailable);
    kernel.reset();
    assert(call(S::close, 3) == kernel_error(badf));
    for (const char* name : {"data.bin", "link", "escape", "fifo"})
        assert(::unlink((base + "/" + name).c_str()) == 0);
    assert(::unlink((dir + "/nested").c_str()) == 0);
    assert(::rmdir(dir.c_str()) == 0 && ::rmdir(root) == 0);
    std::puts("PS4 libkernel filesystem and import checks passed");
}
