// FatalFaultLog (issue #296): exact line format, truncation without overflow,
// append-only file, disabled without a directory.
#include "maxps4_fatal_fault_log.h"
#include "maxps4_dispatch_fault.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
using maxps4::FatalFault;
using maxps4::FatalFaultLog;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static std::string slurp(const char* path) {
    std::string out;
    if (FILE* f = std::fopen(path, "rb")) { char b[256]; size_t n; while ((n = std::fread(b, 1, sizeof b, f))) out.append(b, n); std::fclose(f); }
    return out;
}
int main() {
    FatalFault f;
    f.signal = 11; f.code = 1; f.address = 0; f.pc = 0; f.lr = 0x1029A0C50; f.fp = 0x16f0; f.sp = 0x16e0;
    f.x[0] = 1; f.x[1] = 0xffffffffffffffffull; f.x[2] = 0; f.x[3] = 0xabc;
    f.pc_where = FatalFault::other; f.lr_where = FatalFault::jit;
    f.guest_thread_known = true; f.guest_thread = 7; f.guest_rip = 0x800123456;
    char line[512];
    const size_t n = FatalFaultLog::format(f, line, sizeof line);
    const char* want = "fatal-host-fault signal=11 code=1 addr=0x0 pc=0x0(other) lr=0x1029a0c50(jit) fp=0x16f0 sp=0x16e0"
                       " x0=0x1 x1=0xffffffffffffffff x2=0x0 x3=0xabc guest-thread=7 guest-rip=0x800123456\n";
    CHECK(std::strcmp(line, want) == 0 && n == std::strlen(want));
    // A host thread without a guard.
    FatalFault host; host.signal = 10; host.pc = 0x10; host.lr = 0x20;
    FatalFaultLog::format(host, line, sizeof line);
    CHECK(std::strstr(line, "pc=0x10(other) lr=0x20(other)") && std::strstr(line, "guest-thread=none\n"));
    // Truncation: never writes past capacity, always NUL-terminated.
    char small[24]; std::memset(small, 'Z', sizeof small);
    const size_t t = FatalFaultLog::format(f, small, 20);
    CHECK(t == 19 && small[19] == 0 && small[20] == 'Z');
    // Disabled until a directory is set: nothing written.
    FatalFaultLog::append(f);
    CHECK(FatalFaultLog::path()[0] == 0);
    // Append-only file in the given directory.
    char dir[] = "/tmp/maxps4-fatal-XXXXXX";
    CHECK(::mkdtemp(dir));
    FatalFaultLog::set_path(dir);
    const std::string path = std::string(dir) + "/maxps4-fatal-fault.txt";
    CHECK(path == FatalFaultLog::path());
    FatalFaultLog::append(f);
    FatalFaultLog::append(host);
    const std::string text = slurp(path.c_str());
    CHECK(text.rfind(want, 0) == 0);
    CHECK(text.find("guest-thread=none\n") != std::string::npos && text.size() > std::strlen(want));
    std::remove(path.c_str()); ::rmdir(dir);
    // #296 signature: pc == 0 with LR inside the dispatcher (br x0 after a
    // CompileBlock that returned 0), only for SIGSEGV.
    {
        const uint64_t begin = 0x1031b4000, end = 0x1031b8000, lr = begin + 0x190;
        CHECK(maxps4::uncompilable_dispatch(SIGSEGV, 0, lr, begin, end));
        CHECK(!maxps4::uncompilable_dispatch(SIGSEGV, 4, lr, begin, end));        // a non-null PC is not this case
        CHECK(!maxps4::uncompilable_dispatch(SIGSEGV, 0, end, begin, end));       // LR past the dispatcher
        CHECK(!maxps4::uncompilable_dispatch(SIGSEGV, 0, begin - 4, begin, end)); // LR before it (e.g. app code)
        CHECK(!maxps4::uncompilable_dispatch(SIGBUS, 0, lr, begin, end));
        CHECK(!maxps4::uncompilable_dispatch(SIGSEGV, 0, 0, 0, 0));               // dispatcher range unknown
    }
    // The contained record names the guest bytes FEX could not compile.
    {
        FatalFault c; c.signal = SIGILL; c.code = maxps4::uncompilable_guest_block; c.address = 0x7001a14338;
        c.lr = 0x1031b4190; c.lr_where = FatalFault::dispatcher; c.guest_thread_known = true; c.guest_thread = 14;
        c.guest_rip = 0x7001a14338; c.contained = true;
        const uint8_t bytes[] = {0xf0, 0x0f, 0xc7, 0x0e};
        for (uint8_t i = 0; i < sizeof bytes; ++i) c.bytes[i] = bytes[i];
        c.byte_count = sizeof bytes;
        char out[512];
        FatalFaultLog::format(c, out, sizeof out);
        CHECK(std::strncmp(out, "contained-host-fault signal=4 code=19792 addr=0x7001a14338 pc=0x0(other) lr=0x1031b4190(dispatcher)", 99) == 0);
        CHECK(std::strstr(out, " guest-thread=14 guest-rip=0x7001a14338 guest-bytes=f00fc70e\n") != nullptr);
    }
    std::puts("PASS fatal fault log: exact format, bounded truncation, append-only, disabled without a directory");
    return 0;
}
