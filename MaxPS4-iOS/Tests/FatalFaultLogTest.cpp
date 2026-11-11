// FatalFaultLog (issue #296): exact line format, truncation without overflow,
// append-only file, disabled without a directory.
#include "maxps4_fatal_fault_log.h"
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
    f.x16 = 0xdead; f.x17 = 0xbeef; f.dispatcher_begin = 0x1029a0000;
    f.x[0] = 1; f.x[1] = 0xffffffffffffffffull; f.x[2] = 0; f.x[3] = 0xabc;
    f.pc_where = FatalFault::other; f.lr_where = FatalFault::dispatcher;
    f.guest_thread_known = true; f.guest_thread = 7; f.guest_rip = 0x800123456;
    char line[512];
    const size_t n = FatalFaultLog::format(f, line, sizeof line);
    const char* want = "fatal-host-fault signal=11 code=1 addr=0x0 pc=0x0(other) lr=0x1029a0c50(dispatcher) fp=0x16f0 sp=0x16e0"
                       " x0=0x1 x1=0xffffffffffffffff x2=0x0 x3=0xabc x16=0xdead x17=0xbeef lr-dispatch-offset=0xc50 guest-thread=7 guest-rip=0x800123456\n";
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
    std::puts("PASS fatal fault log: exact format, bounded truncation, append-only, disabled without a directory");
    return 0;
}
