#include "../Native/maxps4_ps4_libkernel.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace maxps4;
using namespace maxps4::ps4;
using S = PS4KernelService;

int main() {
    GuestELF image;
    auto elf = guest_test::elf(40, 2, true);
    guest_test::put(elf, 8192, 160, 8);
    assert(image.load(elf.data(), elf.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Libkernel kernel(memory);
    auto path = [&](const char* value, uint64_t address = 0x10000) {
        assert(memory.write(address, value, std::strlen(value) + 1));
        return address;
    };
    auto call = [&](S service, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) {
        return kernel.invoke(service, {a, b, c, d});
    };
    char root[] = "/tmp/maxps4-writable-XXXXXX";
    assert(::mkdtemp(root));
    const std::string base(root);
    assert(call(S::open, path("/savedata0/save"), 0x202) == kernel_error(noent));
    assert(kernel.mount("/unknown", root) == kernel_error(inval));
    assert(kernel.mount(nullptr, root) == kernel_error(inval));
    assert(kernel.mount("/savedata0", root) == 0);
    assert(kernel.mount("/download0", root) == 0);
    assert(kernel.mount("/temp0", root) == 0);
    assert(kernel.mount_app0(root) == 0);
    assert(call(S::open, path("/app0/save"), 0x202) == kernel_error(rofs));
    for (const char* invalid : {"/savedata0/../save", "/savedata0//save", "/savedata0/./save", "/savedata0/save/"})
        assert(call(S::open, path(invalid), 0x202) == kernel_error(inval));
    assert(call(S::open, path("/savedata0/save"), 0x206) == kernel_error(notsup));
    const auto fd = call(S::open, path("/savedata0/save"), 0xa02); // RDWR | CREAT | EXCL.
    assert(fd == 3);
    assert(call(S::open, 0x10000, 0xa02) == kernel_error(exist));
    assert(memory.write(0x11000, "abcdef", 6));
    assert(call(S::write, fd, 0x11000, 6) == 6);
    assert(call(S::write, fd, 0x11fff, 4) == kernel_error(fault));
    assert(call(S::write, fd, 0x11000, UINT64_MAX) == kernel_error(inval));
    assert(call(S::lseek, fd, 0, 1) == 6);
    assert(call(S::pread, fd, 0x11100, 3, 1) == 3);
    char bytes[8]{};
    assert(memory.read(0x11100, bytes, 3) && std::memcmp(bytes, "bcd", 3) == 0);
    assert(call(S::pwrite, fd, 0x11000, 2, 2) == 2);
    assert(call(S::lseek, fd, 0, 1) == 6); // Positioned I/O preserves the offset.
    assert(call(S::pread, fd, 0x11100, 6, 0) == 6);
    assert(memory.read(0x11100, bytes, 6) && std::memcmp(bytes, "ababef", 6) == 0);
    // AIO reads complete at submission: the result holds the size, state 3 (completed).
    {
        uint8_t request[40]{};
        put_le(request, 0, 1, 8); put_le(request, 8, 4, 8); put_le(request, 16, 0x11180, 8);
        put_le(request, 24, 0x11440, 8); put_le(request, 32, uint64_t(fd), 4);
        assert(memory.write(0x11400, request, sizeof(request)));
        assert(call(S::aio_read, 0x11400, 1, 0, 0x11460) == 0);
        uint8_t result[12]{}; assert(memory.read(0x11440, result, 12) && get_le(result, 8) == 4 && get_le(result + 8, 4) == 3);
        assert(memory.read(0x11180, bytes, 4) && std::memcmp(bytes, "babe", 4) == 0);
        uint8_t id[4]{}; assert(memory.read(0x11460, id, 4));
        assert(call(S::aio_wait, get_le(id, 4), 0x11468) == 0 && memory.read(0x11468, id, 4) && get_le(id, 4) == 3);
        put_le(request, 32, 99, 4); assert(memory.write(0x11400, request, sizeof(request)));
        assert(call(S::aio_read, 0x11400, 1, 0, 0x11460) == 0); // A bad descriptor aborts that request only.
        assert(memory.read(0x11440, result, 12) && int64_t(get_le(result, 8)) == kernel_error(badf) && get_le(result + 8, 4) == 4);
        assert(call(S::aio_read, 0x11400, 1, 0, 0) == kernel_error(fault));
    }
    assert(call(S::pread, fd, 0x11100, 1, UINT64_MAX) == kernel_error(inval));
    assert(call(S::pwrite, fd, 0x11000, 1, UINT64_MAX) == kernel_error(inval));
    assert(call(S::pread, fd, 0x11fff, 4, 0) == kernel_error(fault));
    assert(call(S::fsync, fd) == 0);
    assert(call(S::ftruncate, fd, UINT64_MAX) == kernel_error(inval));
    assert(call(S::ftruncate, fd, 3) == 0);
    assert(call(S::close, fd) == 0);
    assert(call(S::write, fd, 0x11000, 1) == kernel_error(badf));
    assert(call(S::fsync, fd) == kernel_error(badf));
    const auto readonly = call(S::open, path("/savedata0/save"));
    assert(readonly == 3);
    assert(call(S::write, readonly, 0x11000, 1) == kernel_error(badf));
    assert(call(S::ftruncate, readonly, 0) == kernel_error(badf));
    assert(call(S::fstat, readonly, 0x11200) == 0);
    uint8_t stat[PS4Libkernel::stat_size];
    assert(memory.read(0x11200, stat, sizeof(stat)));
    assert(get_le(stat + 72, 8) == 3 && (get_le(stat + 8, 2) & 0200));
    assert(call(S::close, readonly) == 0);
    const auto append = call(S::open, 0x10000, 9); // WRONLY | APPEND.
    assert(append == 3 && call(S::lseek, append, 0) == 0);
    assert(call(S::write, append, 0x11000, 2) == 2);
    assert(call(S::lseek, append, 0, 2) == 5);
    assert(call(S::read, append, 0x11100, 1) == kernel_error(badf));
    assert(call(S::close, append) == 0);
    assert(call(S::truncate, path("/savedata0/save"), 2) == 0);
    assert(call(S::truncate, path("/app0/save"), 0) == kernel_error(rofs));
    assert(call(S::rename, path("/savedata0/save"), path("/download0/save", 0x10100)) == kernel_error(xdev));
    assert(call(S::mkdir, path("/savedata0/sub"), 0700) == 0);
    assert(call(S::mkdir, 0x10000, 0700) == kernel_error(exist));
    assert(call(S::rename, path("/savedata0/save"), path("/savedata0/sub/renamed", 0x10100)) == 0);
    assert(call(S::rmdir, path("/savedata0/sub")) == kernel_error(notempty));
    assert(call(S::unlink, path("/savedata0/sub/renamed")) == 0);
    assert(call(S::rmdir, path("/savedata0/sub")) == 0);
    assert(call(S::rmdir, path("/savedata0")) == kernel_error(busy));
    // A guest path cannot follow a link into another host directory.
    assert(::symlink("/tmp", (base + "/escape").c_str()) == 0);
    assert(call(S::open, path("/savedata0/escape")) == kernel_error(loop));
    assert(call(S::open, path("/savedata0/escape/outside"), 0x202) < 0);
    assert(call(S::mkdir, path("/savedata0/escape/outside"), 0700) < 0);
    assert(::unlink((base + "/escape").c_str()) == 0);
    // Force several dirent blocks and validate their wire layout and offsets.
    std::set<std::string> expected{".", ".."}, actual;
    for (unsigned i = 0; i < 60; ++i) {
        const std::string name = "entry-" + std::to_string(i);
        expected.insert(name);
        assert(::mkdir((base + "/" + name).c_str(), 0700) == 0);
    }
    const auto directory = call(S::open, path("/savedata0/"), 0x20000);
    assert(directory == 3);
    assert(call(S::getdirentries, directory, 0x11000, 511) == kernel_error(inval));
    assert(call(S::getdirentries, directory, 0x11000, UINT64_MAX) == kernel_error(inval));
    assert(call(S::getdirentries, directory, 0x11fff, 512) == kernel_error(fault));
    assert(call(S::getdirentries, directory, 0x11000, 512, 0x11fff) == kernel_error(fault));
    assert(call(S::lseek, directory, 0, 1) == 0);
    uint64_t position = 0;
    for (;;) {
        const auto count = call(S::getdirentries, directory, 0x11000, 512, 0x11300);
        assert(count == 0 || count == 512);
        uint8_t offset[8];
        assert(memory.read(0x11300, offset, 8) && get_le(offset, 8) == position);
        if (!count) break;
        uint8_t block[512];
        assert(memory.read(0x11000, block, sizeof(block)));
        size_t cursor = 0;
        while (cursor < sizeof(block)) {
            const auto size = get_le(block + cursor + 4, 2);
            const auto length = block[cursor + 7];
            assert(size >= 8U + length + 1U && size % 4 == 0 && cursor + size <= sizeof(block));
            assert(block[cursor + 6] == 4 && block[cursor + 8 + length] == 0);
            assert(actual.insert(std::string(reinterpret_cast<char*>(block + cursor + 8), length)).second);
            cursor += size;
        }
        position += 512;
    }
    assert(position > 512 && actual == expected);
    assert(call(S::lseek, directory, 1) == kernel_error(inval));
    assert(call(S::lseek, directory, uint64_t(INT64_MAX), 1) == kernel_error(overflow));
    assert(call(S::lseek, directory, 0) == 0);
    assert(call(S::getdirentries, directory, 0x11000, 512) == 512);
    assert(kernel.mount("/savedata0", "/path/that/does/not/exist") == kernel_error(noent));
    assert(call(S::lseek, directory, 0, 1) == 512);
    assert(kernel.mount("/savedata0", root) == 0);
    assert(call(S::close, directory) == kernel_error(badf));
    assert(resolve_libkernel("4wSze92BhLI", "libkernel", "libkernel", 1) == S::write);
    assert(resolve_libkernel("taRWhTJFTgE", "libkernel", "libkernel", 1) == S::getdirentries);
    assert(resolve_libkernel("write", "libkernel", "libkernel", 1) == S::write);
    assert(call(S::write, 1, UINT64_MAX, 0) == 0);
    assert(call(S::write, 2, UINT64_MAX, 0) == 0);
    kernel.reset();
    assert(call(S::open, path("/savedata0")) == kernel_error(noent));
    for (const auto& name : expected)
        if (name != "." && name != "..") assert(::rmdir((base + "/" + name).c_str()) == 0);
    assert(::rmdir(root) == 0);
    // sceSaveDataMount2 mounts <save root>/<dirName> at /savedata0, keeping
    // the game's other descriptors open.
    char saves[] = "/tmp/maxps4-saves-XXXXXX";
    assert(::mkdtemp(saves));
    const std::string game = std::string(saves) + "/game";
    { const int host = ::open(game.c_str(), O_CREAT | O_WRONLY, 0644); assert(host >= 0); ::close(host); }
    assert(kernel.mount_app0(saves) == 0);
    const auto app = call(S::open, path("/app0/game"), 0);
    assert(app >= 0);
    auto put = [&](uint64_t address, uint64_t value, size_t width) {
        uint8_t wire[8]; put_le(wire, 0, value, width); assert(memory.write(address, wire, width));
    };
    auto mount = [&](uint32_t mode) {
        uint8_t zero[96]{}; assert(memory.write(0x11400, zero, sizeof(zero)));
        put(0x11400, 1, 4); put(0x11408, path("SAVEDATA00", 0x11500), 8); put(0x11418, mode, 4);
        return call(S::save_mount2, 0x11400, 0x11600);
    };
    const int64_t not_found = signed32(0x809f0008U);
    assert(call(S::save_init) == 0 && mount(0x22) == not_found); // No save root yet.
    assert(kernel.set_savedata_root(saves) == 0);
    assert(mount(0x2) == not_found);
    assert(mount(0x22) == 0); // RDWR | CREATE2: created.
    char point[16]{};
    assert(memory.read(0x11600, point, sizeof(point)) && !std::strcmp(point, "/savedata0"));
    uint8_t status[4]{}; assert(memory.read(0x11600 + 28, status, 4) && get_le(status, 4) == 1);
    assert(mount(0x22) == signed32(0x809f0003U)); // Already mounted.
    const auto file = call(S::open, path("/savedata0/data"), 0x202);
    assert(file >= 0 && call(S::close, file) == 0);
    assert(::access((std::string(saves) + "/SAVEDATA00/data").c_str(), F_OK) == 0);
    assert(call(S::save_umount, path("/savedata0", 0x11700)) == 0);
    assert(call(S::save_umount, 0x11700) == signed32(0x809f0004U));
    assert(call(S::open, path("/savedata0/data")) == kernel_error(noent));
    assert(mount(0x2) == 0 && memory.read(0x11600 + 28, status, 4) && get_le(status, 4) == 0);
    assert(mount(0x4) != 0 && call(S::save_term) == 0 && mount(0x4) == signed32(0x809f0007U));
    assert(call(S::close, app) == 0); // Untouched by the save mounts.
    assert(call(S::save_dialog_init) == 0 && call(S::save_dialog_update) == 1);
    assert(call(S::save_dialog_result, 0x11800) == signed32(0x80b80008U));
    assert(call(S::save_dialog_open, 0x11800) == 0 && call(S::save_dialog_update) == 3);
    assert(call(S::save_dialog_result, 0x11800) == 0 && memory.read(0x11808, status, 4) && get_le(status, 4) == 1);
    assert(call(S::save_dialog_term) == 0);
    assert(resolve_libkernel("0z45PIH+SNI", "libSceSaveData", "libSceSaveData", 1) == S::save_mount2);
    kernel.reset();
    assert(::unlink((std::string(saves) + "/SAVEDATA00/data").c_str()) == 0);
    assert(::unlink(game.c_str()) == 0);
    assert(::rmdir((std::string(saves) + "/SAVEDATA00").c_str()) == 0 && ::rmdir(saves) == 0);
    // A game update over /app0: its files first, the game's for the rest.
    char base_dir[] = "/tmp/maxps4-base-XXXXXX", update_dir[] = "/tmp/maxps4-update-XXXXXX";
    assert(::mkdtemp(base_dir) && ::mkdtemp(update_dir));
    auto host_file = [](const std::string& name, const char* text) {
        const int host = ::open(name.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        assert(host >= 0 && ::write(host, text, std::strlen(text)) == ssize_t(std::strlen(text))); ::close(host);
    };
    host_file(std::string(base_dir) + "/data", "base"); host_file(std::string(base_dir) + "/only", "game");
    host_file(std::string(update_dir) + "/data", "upd!");
    assert(kernel.mount_app0_update(update_dir) == kernel_error(inval)); // No /app0 yet (reset above).
    assert(kernel.mount_app0(base_dir) == 0 && kernel.mount_app0_update(update_dir) == 0);
    auto app0 = [&](const char* name) {
        const auto fd = call(S::open, path(name), 0);
        assert(fd >= 0);
        char text[8]{};
        assert(call(S::read, fd, 0x11000, 4) == 4 && memory.read(0x11000, text, 4) && call(S::close, fd) == 0);
        return std::string(text);
    };
    assert(app0("/app0/data") == "upd!" && app0("/app0/only") == "game");
    assert(call(S::open, path("/app0/data"), 0x202) == kernel_error(rofs));
    assert(kernel.mount_app0(base_dir) == 0 && app0("/app0/data") == "base"); // A remount drops the update.
    kernel.reset();
    for (const auto& name : {std::string(base_dir) + "/data", std::string(base_dir) + "/only", std::string(update_dir) + "/data"})
        assert(::unlink(name.c_str()) == 0);
    assert(::rmdir(base_dir) == 0 && ::rmdir(update_dir) == 0);
    // System services games query at start: language, clock, PSN, content.
    auto value32 = [&](uint64_t address) { uint8_t w[4]{}; assert(memory.read(address, w, 4)); return get_le(w, 4); };
    kernel.set_system_language(2);
    assert(call(S::system_param_int, 1, 0x11900) == 0 && value32(0x11900) == 2);
    uint8_t date[16]{}; // 2026-10-29 12:34:56.000789
    put_le(date, 0, 2026, 2); put_le(date, 2, 10, 2); put_le(date, 4, 29, 2);
    put_le(date, 6, 12, 2); put_le(date, 8, 34, 2); put_le(date, 10, 56, 2); put_le(date, 12, 789, 4);
    assert(memory.write(0x11a00, date, sizeof(date)));
    assert(call(S::rtc_get_tick, 0x11a00, 0x11a20) == 0);
    uint8_t tick[8]{}; assert(memory.read(0x11a20, tick, 8));
    assert(get_le(tick, 8) == 62135596800000000ULL + (20755ULL * 86400 + 12 * 3600 + 34 * 60 + 56) * 1000000 + 789);
    assert(call(S::rtc_set_tick, 0x11a40, 0x11a20) == 0);
    uint8_t back[16]{}; assert(memory.read(0x11a40, back, 16) && !std::memcmp(back, date, 16));
    put_le(date, 2, 13, 2); assert(memory.write(0x11a00, date, sizeof(date)));
    assert(call(S::rtc_get_tick, 0x11a00, 0x11a20) == signed32(0x80b50002U)); // Month 13.
    assert(call(S::rtc_current_tick, 0x11a20) == 0 && call(S::rtc_resolution) == 1000000);
    assert(call(S::np_state, 1, 0x11900) == 0 && value32(0x11900) == 1);
    assert(call(S::np_signed_out, 1, 0x11900) == signed32(0x80550006U));
    assert(call(S::app_param_int, 0, 0x11900) == 0 && value32(0x11900) == 3);
    assert(call(S::app_addcont_list, 0, 0, 0, 0x11900) == 0 && value32(0x11900) == 0);
    // Add-on content: listed as installed, found by its label, mounted at /addcont0.
    char dlc_dir[] = "/tmp/maxps4-dlc-XXXXXX";
    assert(::mkdtemp(dlc_dir));
    host_file(std::string(dlc_dir) + "/extra", "dlc!");
    assert(call(S::app_addcont_list, 0, 0, 0, 0x11900) == 0 && value32(0x11900) == 0);
    assert(kernel.add_addcont("SHORT", dlc_dir) == kernel_error(inval));
    assert(kernel.add_addcont("SONICMANIAPLUS00", dlc_dir) == 0);
    assert(call(S::app_addcont_list, 0, 0, 0, 0x11900) == 0 && value32(0x11900) == 1);
    assert(call(S::app_addcont_list, 0, 0x11b00, 4, 0x11900) == 0 && value32(0x11900) == 1);
    char label[17]{}; assert(memory.read(0x11b00, label, 16) && !std::strcmp(label, "SONICMANIAPLUS00"));
    assert(value32(0x11b00 + 20) == 4); // Installed.
    assert(memory.write(0x11c00, "SONICMANIAPLUS00", 17) && memory.write(0x11c20, "SOMETHINGELSE000", 17));
    assert(call(S::app_addcont_info, 0, 0x11c20, 0x11d00) == signed32(0x80d90005U));
    assert(call(S::app_addcont_info, 0, 0x11c00, 0x11d00) == 0 && value32(0x11d00 + 20) == 4);
    assert(call(S::app_addcont_mount, 0, 0x11c00, 0x11d40) == 0);
    char where[16]{}; assert(memory.read(0x11d40, where, 16) && !std::strcmp(where, "/addcont0"));
    const auto extra = call(S::open, path("/addcont0/extra"), 0);
    assert(extra >= 0 && call(S::read, extra, 0x11000, 4) == 4 && call(S::close, extra) == 0);
    char dlc_text[5]{}; assert(memory.read(0x11000, dlc_text, 4) && !std::strcmp(dlc_text, "dlc!"));
    assert(call(S::app_addcont_unmount, 0x11d40) == 0 && call(S::app_addcont_unmount, 0x11d40) == signed32(0x80d90002U));
    assert(call(S::open, path("/addcont0/extra"), 0) == kernel_error(noent));
    assert(::unlink((std::string(dlc_dir) + "/extra").c_str()) == 0 && ::rmdir(dlc_dir) == 0);
    assert(resolve_libkernel("18B2NS1y9UU", "libSceRtc", "libSceRtc", 1) == S::rtc_current_tick);
    // Loaded modules: LoadStartModule by file name, Dlsym by NID, unwind info.
    assert(ps4_nid("sceGnmSetPsShader350") == "5uFKckiJYRM" && ps4_nid("sceKernelDlsym") == "LwG8g3niqwA");
    kernel.set_modules({{"libfoo.prx", 0x10000, 0x20000, 0, 0, {{ps4_nid("fooFunction"), 0x12345}}, {}}});
    assert(call(S::module_load_start, path("/app0/sce_module/LIBFOO.prx"), 0, 0, 0) == 1);
    assert(call(S::module_load_start, path("/app0/sce_module/libbar.prx"), 0, 0, 0) == kernel_error(noent));
    assert(memory.write(0x11c40, "fooFunction", 12) && memory.write(0x11c60, "barFunction", 12));
    assert(call(S::module_dlsym, 1, 0x11c40, 0x11c80) == 0);
    uint8_t address[8]{}; assert(memory.read(0x11c80, address, 8) && get_le(address, 8) == 0x12345);
    assert(call(S::module_dlsym, 1, 0x11c60, 0x11c80) == kernel_error(srch));
    assert(call(S::module_dlsym, 2, 0x11c40, 0x11c80) == kernel_error(srch));
    assert(call(S::module_info_unwind, 0x10800, 0, 0x11e00) == 0);
    char unwind_name[16]{}; assert(memory.read(0x11e08, unwind_name, 11) && !std::strcmp(unwind_name, "libfoo.prx"));
    uint8_t segment[8]{}; assert(memory.read(0x11e00 + 288, segment, 8) && get_le(segment, 8) == 0x10000);
    assert(call(S::module_info_unwind, 0x30000, 0, 0x11e00) == kernel_error(srch));
    // Module info by address (Ex, 0x1a8 bytes: handle at 264) and by handle (0x160).
    assert(call(S::module_info_ex, 0x10800, 0, 0x10400) == 0 && value32(0x10400 + 264) == 1);
    char info_name[16]{}; assert(memory.read(0x10408, info_name, 11) && !std::strcmp(info_name, "libfoo.prx"));
    assert(call(S::module_info, 1, 0x10600) == 0 && value32(0x10600) == 0x160);
    assert(call(S::module_info, 2, 0x10600) == kernel_error(srch));
    // Dialogs finish at once answered OK; networking is never connected.
    assert(call(S::msg_init) == 0 && call(S::msg_status) == 1);
    assert(call(S::msg_result, 0x11c80) == signed32(0x80b80008U));
    assert(call(S::msg_open, 0x11c80) == 0 && call(S::msg_status) == 3);
    assert(call(S::msg_result, 0x11c80) == 0 && value32(0x11c88) == 1);
    assert(call(S::msg_term) == 0 && call(S::msg_status) == 0);
    assert(call(S::error_init) == 0 && call(S::error_open, 0) == 0 && call(S::error_status) == 3 && call(S::error_term) == 0);
    assert(call(S::netctl_state, 0x11900) == 0 && value32(0x11900) == 0);
    assert(call(S::netctl_info, 1, 0x11c80) == signed32(0x80412108U));
    assert(call(S::netctl_callback, 0, 0, 0x11900) == 0 && value32(0x11900) == 1);
    assert(call(S::audio_port_state, 1, 0x11c80) == 0 && value32(0x11c80) == (1 | 2 << 16));
    // PlayGo: installed and local; sceRandom fills; no sandbox word without dynamic memory.
    assert(call(S::playgo_open, 0x11900, 0) == 0 && value32(0x11900) == 1);
    uint8_t chunks[4] = {0, 0, 1, 0}; assert(memory.write(0x11c80, chunks, 4));
    assert(call(S::playgo_locus, 1, 0x11c80, 2, 0x11ca0) == 0);
    uint8_t loci[2]{}; assert(memory.read(0x11ca0, loci, 2) && loci[0] == 3 && loci[1] == 3);
    assert(call(S::playgo_todo, 1, 0x11cc0, 4, 0x11900) == 0 && value32(0x11900) == 0);
    assert(call(S::random_number, 0x11cc0, 65) == signed32(0x817c0016U));
    uint8_t zero[32]{}; assert(memory.write(0x11cc0, zero, 32) && call(S::random_number, 0x11cc0, 32) == 0);
    uint8_t random[32]{}; assert(memory.read(0x11cc0, random, 32) && std::memcmp(random, zero, 32) != 0);
    assert(call(S::sandbox_word) == 0);
    // Save data 2: Mount (v1), params, DirNameSearch, delete, save data memory.
    char saves2[] = "/tmp/maxps4-saves2-XXXXXX";
    assert(::mkdtemp(saves2) && kernel.set_savedata_root(saves2) == 0);
    {
        uint8_t request[48]{};
        put_le(request, 16, path("SLOT01", 0x11500), 8); put_le(request, 40, 0x22, 4); // RDWR | CREATE2.
        assert(memory.write(0x11400, request, sizeof(request)) && call(S::save_mount1, 0x11400, 0x11600) == 0);
        assert(memory.write(0x11700, "/savedata0", 11));
        assert(memory.write(0x10400, "Green Hill", 11) && call(S::save_set_param, 0x11700, 1, 0x10400, 128) == 0);
        uint8_t title[16]{};
        assert(kernel.invoke(S::save_get_param, {0x11700, 1, 0x10500, 128, 0x11900}) == 0 && value32(0x11900) == 128);
        assert(memory.read(0x10500, title, 11) && !std::strcmp(reinterpret_cast<char*>(title), "Green Hill"));
        assert(call(S::save_mount_info, 0x11700, 0x11a00) == 0);
        assert(call(S::save_umount2, 0, 0x11700) == 0);
        put_le(request, 16, path("SLOT02", 0x11500), 8);
        assert(memory.write(0x11400, request, sizeof(request)) && call(S::save_mount1, 0x11400, 0x11600) == 0);
        assert(call(S::save_umount, 0x11700) == 0);
        // Search "SLOT%": both, by name, the first's title in its param.
        uint8_t cond[32]{}, result[40]{};
        put_le(cond, 16, path("SLOT%", 0x11500), 8);
        put_le(result, 8, 0x10a00, 8); put_le(result, 16, 4, 4); put_le(result, 24, 0x10b00, 8);
        assert(memory.write(0x11400, cond, sizeof(cond)) && memory.write(0x11440, result, sizeof(result)));
        assert(call(S::save_search, 0x11400, 0x11440) == 0 && value32(0x11440) == 2 && value32(0x11440 + 20) == 2);
        char found[8]{};
        assert(memory.read(0x10a00, found, 7) && !std::strcmp(found, "SLOT01") && memory.read(0x10a20, found, 7) && !std::strcmp(found, "SLOT02"));
        assert(memory.read(0x10b00, title, 11) && !std::strcmp(reinterpret_cast<char*>(title), "Green Hill"));
        // Delete SLOT02, then only SLOT01 is found.
        uint8_t del[24]{};
        put_le(del, 16, path("SLOT02", 0x11500), 8);
        assert(memory.write(0x11480, del, sizeof(del)) && call(S::save_delete, 0x11480) == 0);
        assert(call(S::save_search, 0x11400, 0x11440) == 0 && value32(0x11440) == 1);
        // Save data memory: setup, write, read back.
        assert(call(S::save_memory_setup, 1, 64, 0x11900) == 0);
        uint8_t existed[8]{}; assert(memory.read(0x11900, existed, 8) && get_le(existed, 8) == 0);
        assert(memory.write(0x10400, "memory!", 8) && call(S::save_memory_set, 1, 0x10400, 8, 16) == 0);
        assert(call(S::save_memory_get, 1, 0x10500, 8, 16) == 0);
        char back_text[8]{}; assert(memory.read(0x10500, back_text, 8) && !std::strcmp(back_text, "memory!"));
    }
    kernel.reset();
    std::filesystem::remove_all(saves2);
    // POSIX pthread_* reuse the scePthread services and return errno numbers.
    assert(resolve_libkernel("7H0iTOciTLo", "libkernel", "libkernel", 1) == S::mutex_lock);
    assert(resolve_libkernel("OxhIB8LB-PQ", "libkernel", "libkernel", 1) == S::thread_create);
    assert(pthread_result(0) == 0 && pthread_result(kernel_error(busy)) == 16 && pthread_result(kernel_error(timedout)) == 60);
    assert(pthread_result(42) == 42);
    // libScePosix shares libkernel's POSIX functions; sockets fail (no network).
    assert(resolve_libkernel("bY-PO6JhzhQ", "libScePosix", "libScePosix", 1) == S::close);
    assert(resolve_libkernel("bY-PO6JhzhQ", "libkernel", "libkernel", 1) == S::close);
    assert(resolve_libkernel("TU-d9PfIHPM", "libScePosix", "libScePosix", 1) == S::net_unavailable);
    assert(resolve_libkernel("lbQKqsERhtE", "libSceDiscMap", "libSceDiscMap", 1) == S::discmap_no_bitmap);
    assert(kernel.invoke(S::net_unavailable, {2, 1, 0}, 1, true) == -1);
    assert(call(S::sched_priority_max, 1) == 767 && call(S::sched_priority_min, 3) == 256);
    assert(call(S::sched_priority_max, 0) == kernel_error(inval));
    assert(call(S::sched_param_get, 0, 0x11900, 0x11904) == 0 && value32(0x11900) == 1 && value32(0x11904) == 700);
    assert(call(S::discmap_no_bitmap, 0x10000, 0, 16, 0x11900) == int32_t(0x81100004U));
    const uint8_t ones[12] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    assert(memory.write(0x11900, ones, sizeof(ones)));
    assert(kernel.invoke(S::discmap_flags, {0x10000, 0, 16, 0x11900, 0x11904, 0x11908}) == 0);
    assert(value32(0x11900) == 0 && value32(0x11904) == 0 && value32(0x11908) == 0);
    assert(call(S::video_capability, 1, 0x11900) == 0 && value32(0x11900) == 0);
    // GNM workloads: one stream; begin/end as shadPS4.
    assert(call(S::gnm_workload_stream, 1, 0x11900) == 0 && value32(0x11900) == 1);
    assert(call(S::gnm_workload_stream, 0, 0x11900) == 3);
    assert(call(S::gnm_workload_begin, 1, 0x11900) == 0 && value32(0x11900) == 1 && call(S::gnm_workload_begin, 16, 0x11900) == 1);
    assert(call(S::gnm_workload_end, 1) == 0 && call(S::gnm_workload_end, 0) == 2 && call(S::gnm_workload_end, 16ULL << 56) == 2);
    // Without a graphics backend, a workload submission fails like a plain one.
    assert(call(S::gnm_submit_workload, 1, 1, 0x11900, 0x11910) == call(S::gnm_submit, 1, 0x11900, 0x11910));
    // A compute ring at 0x11c00 (64 dwords) whose WRITE_DATA stores a label:
    // DingDong retires it and moves the read pointer.
    {
        constexpr uint64_t ring = 0x11c00, read_pointer = 0x11d00, label = 0x11d10;
        assert(call(S::gnm_map_compute, 7, 0, ring, 64) == int32_t(0x80d17000U));
        assert(call(S::gnm_map_compute, 1, 0, ring + 4, 64) == int32_t(0x80d17003U));
        assert(call(S::gnm_map_compute, 1, 0, ring, 48) == int32_t(0x80d17002U));
        const int64_t vqid = kernel.invoke(S::gnm_map_compute, {1, 2, ring, 64, read_pointer});
        assert(vqid == 11 && value32(read_pointer) == 0);
        uint8_t packet[20]{};
        put_le(packet, 0, 0xc0033700U, 4); put_le(packet, 4, 5U << 8, 4);
        put_le(packet, 8, label, 4); put_le(packet, 12, 0, 4); put_le(packet, 16, 0xfeedU, 4);
        assert(memory.write(ring, packet, sizeof(packet)) && memory.write(label, "\0\0\0\0", 4));
        assert(call(S::gnm_ding_dong, uint64_t(vqid), 5) == 0 && value32(label) == 0xfeed && value32(read_pointer) == 5);
        assert(call(S::gnm_ding_dong, 99, 5) == 0 && call(S::gnm_unmap_compute, uint64_t(vqid)) == 0);
        assert(call(S::gnm_ding_dong, uint64_t(vqid), 9) == 0 && value32(read_pointer) == 5); // Unmapped: ignored.
    }
    assert(call(S::gnm_tessellation_offchip) == 0x800000);
    assert(memory.write(0x11900, "named", 6) && call(S::thread_getname, 1, 0x11900) == 0 && value32(0x11900) == 0);
    assert(call(S::thread_getname, 1, 0) == kernel_error(fault));
    // The game's stdout/stderr text is kept for the session report.
    assert(memory.write(0x11900, "Fatal error!", 12) && call(S::write, 1, 0x11900, 12) == 12 && call(S::write, 2, 0x11900, 5) == 5);
    assert(kernel.console() == "Fatal error!Fatal");
    // Trophy queries: zero trophies past each structure's size field.
    {
        uint8_t details[0x40]; std::memset(details, 0xff, sizeof(details)); put_le(details, 0, 0x20, 8);
        assert(memory.write(0x11a00, details, sizeof(details)) && memory.write(0x11a80, details, sizeof(details)));
        assert(call(S::trophy_game_info, 1, 1, 0x11a00, 0x11a80) == 0);
        assert(value32(0x11a00) == 0x20 && value32(0x11a08) == 0 && value32(0x11a1c) == 0 && value32(0x11a20) == 0xffffffffU);
        assert(value32(0x11a88) == 0 && value32(0x11aa0) == 0xffffffffU);
        assert(memory.write(0x11a00, details, sizeof(details)) && call(S::trophy_unlock_state, 1, 1, 0x11a00, 0x11a10) == 0);
        assert(value32(0x11a00) == 0 && value32(0x11a0c) == 0 && value32(0x11a10) == 0);
    }
    // MutexReltimedlockNp locks, or times out while another owner holds it.
    assert(call(S::mutex_init, 0x11b00) == 0 && call(S::mutex_reltimedlock, 0x11b00, 1000) == 0);
    assert(call(S::mutex_reltimedlock, 0x11b00, 2000) == kernel_error(timedout) || call(S::mutex_reltimedlock, 0x11b00, 2000) == kernel_error(deadlk));
    assert(call(S::mutex_unlock, 0x11b00) == 0 && call(S::mutex_destroy, 0x11b00) == 0);
    assert(call(S::attr_getschedparam, 0x11b40, 0x11b48) == 0 && value32(0x11b48) == 700);
    // sceRtcGetTime_t: 2000-01-01 00:00:01 UTC.
    {
        uint8_t date[16]{}; put_le(date, 0, 2000, 2); put_le(date, 2, 1, 2); put_le(date, 4, 1, 2); put_le(date, 10, 1, 2);
        assert(memory.write(0x11a00, date, sizeof(date)) && call(S::rtc_time_t, 0x11a00, 0x11a40) == 0 && value32(0x11a40) == 946684801U);
    }
    assert(resolve_libkernel("How7B8Oet6k", "libkernel", "libkernel", 1) == S::thread_getname);
    kernel.reset();
    assert(call(S::system_param_int, 1, 0x11900) == 0 && value32(0x11900) == 1);
    std::puts("PS4 writable files, positioned I/O and directory checks passed");
}
