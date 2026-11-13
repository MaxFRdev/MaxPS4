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
    // Device reproduction (#296): actual captured bytes start with RET, not an
    // unsupported LOCK opcode. Preserve the exact fault record for triage.
    {
        FatalFault c; c.signal = SIGILL; c.code = maxps4::uncompilable_guest_block;
        c.address = 0x7001a14338; c.pc = 0; c.lr = 0x10131c190;
        c.lr_where = FatalFault::dispatcher; c.sp = 0x17f0e6bf0;
        c.guest_thread_known = true; c.guest_thread = 14;
        c.guest_rip = 0x7001a14338; c.contained = true;
        const uint8_t bytes[] = {0xc3, 0x49, 0x89, 0xca, 0xb8, 0x11, 0x00, 0x00,
                                 0x00, 0x0f, 0x05, 0xc3, 0x49, 0x89, 0xca, 0xb8};
        for (size_t i = 0; i < sizeof bytes; ++i) c.bytes[i] = bytes[i];
        c.byte_count = sizeof bytes;
        char out[512];
        FatalFaultLog::format(c, out, sizeof out);
        CHECK(std::strstr(out, "contained-host-fault signal=4 code=19792 addr=0x7001a14338") != nullptr);
        CHECK(std::strstr(out, "lr=0x10131c190(dispatcher)") != nullptr);
        CHECK(std::strstr(out, "guest-thread=14 guest-rip=0x7001a14338 guest-bytes=c34989cab8110000000f05c34989cab8\n") != nullptr);
    }
    // Which dispatcher call returned 0, from the instruction before LR.
    {
        using maxps4::DispatchCall;
        CHECK(maxps4::dispatch_call(0xd63f0080u) == DispatchCall::compile_block); // blr x4
        CHECK(maxps4::dispatch_call(0xd63f0040u) == DispatchCall::exit_link);     // blr x2
        CHECK(maxps4::dispatch_call(0xd63f0000u) == DispatchCall::unknown);       // blr x0
        CHECK(std::strcmp(maxps4::dispatch_call_name(DispatchCall::exit_link), "exit-link") == 0);
    }
    // bl: forward, backward, and not a bl.
    {
        uint64_t t = 0;
        CHECK(maxps4::bl_target(0x1000, 0x94000010u, t) && t == 0x1040);
        CHECK(maxps4::bl_target(0x1000, 0x97fffffcu, t) && t == 0x1000 - 16);
        CHECK(!maxps4::bl_target(0x1000, 0x14000010u, t)); // b, not bl
    }
    // Jump thunk + ExitFunctionLinkData, unlinked and linked to a null block.
    {
        const uint64_t thunk = 0x10000200, caller = 0x10000100;
        maxps4::JumpThunk t {{0x14000002u, 0xd61f0000u, 0x58000100u, 0xd63f0000u}, 0, 0x7001a20000, caller - thunk};
        CHECK(maxps4::jump_thunk_record(t, thunk, caller));
        t.insn[0] = 0x58000080u; // ldr x0, #0x10 after ExitFunctionLink stored HostCode 0
        CHECK(maxps4::jump_thunk_record(t, thunk, caller));
        CHECK(!maxps4::jump_thunk_record(t, thunk, caller + 4)); // another call site
        t.insn[1] = 0xd61f0020u;                                  // br x1: not a thunk
        CHECK(!maxps4::jump_thunk_record(t, thunk, caller));
    }
    // The line names the dispatcher call and the linked target.
    {
        FatalFault c; c.signal = SIGILL; c.code = maxps4::uncompilable_guest_block; c.address = 0x7001a20000;
        c.guest_thread_known = true; c.guest_thread = 14; c.guest_rip = 0x7001a14338; c.contained = true;
        c.bytes[0] = 0xc3; c.byte_count = 1;
        c.dispatch_known = true; c.lr_insn = 0xd63f0040u; c.dispatch = 2;
        c.callret_known = true; c.callret_rip = 0x7001a10005; c.callret_host = 0x10000104;
        c.callret_previous_instruction = 0x94000010;
        c.exit_link_trace = maxps4::ExitLinkTrace::validated;
        c.link_known = true; c.link_rip = 0x7001a20000; c.link_host = 0;
        c.link_bytes[0] = 0x66; c.link_bytes[1] = 0xcf; c.link_byte_count = 2;
        char out[1024];
        FatalFaultLog::format(c, out, sizeof out);
        CHECK(std::strstr(out, " guest-rip=0x7001a14338 guest-bytes=c3 lr-insn=0xd63f0040 dispatch=exit-link"
                               " callret-rip=0x7001a10005 callret-host=0x10000104 callret-prev=0x94000010 exit-link-trace=validated"
                               " link-rip=0x7001a20000 link-host=0x0 link-bytes=66cf\n") != nullptr);
        CHECK(std::strcmp(maxps4::exit_link_trace_name(maxps4::ExitLinkTrace::direct_bl), "direct-bl") == 0);
        CHECK(std::strcmp(maxps4::exit_link_trace_name(maxps4::ExitLinkTrace::unreadable), "unreadable") == 0);
    }
    // Code-buffer scan: a JMP thunk patched with HostCode 0 is found and its
    // caller validated; unlinked or non-null thunks and a wrong caller are not.
    {
        alignas(8) static uint8_t buf[0x200];
        std::memset(buf, 0, sizeof buf);
        const uint64_t b = reinterpret_cast<uint64_t>(buf);
        auto put_thunk = [&](size_t off, uint32_t first, uint64_t host, uint64_t rip, size_t caller_off, uint32_t caller_op) {
            maxps4::JumpThunk t {{first, 0xd61f0000u, 0x58000100u, 0xd63f0000u}, host, rip, uint64_t(int64_t(caller_off) - int64_t(off))};
            std::memcpy(buf + off, &t, sizeof t);
            const uint32_t insn = caller_op | uint32_t(((int64_t(off) - int64_t(caller_off)) / 4) & 0x03ffffff);
            std::memcpy(buf + caller_off, &insn, 4);
        };
        put_thunk(0x40, 0x14000002u, 0, 0x7000000100, 0x00, 0x14000000u);  // unlinked
        put_thunk(0x80, 0x58000080u, 0x1234, 0x7000000200, 0x04, 0x14000000u); // linked, non-null
        auto s0 = maxps4::scan_null_links(b, b + sizeof buf);
        CHECK(s0.count == 0);
        put_thunk(0xc0, 0x58000080u, 0, 0x7000c67e20, 0x08, 0x14000000u);  // b -> null thunk
        auto s1 = maxps4::scan_null_links(b, b + sizeof buf);
        CHECK(s1.count == 1 && s1.thunk == b + 0xc0 && s1.guest_rip == 0x7000c67e20 && s1.caller == b + 0x08 && !s1.call);
        put_thunk(0x100, 0x58000080u, 0, 0x7000c68000, 0x0c, 0x94000000u); // bl -> null thunk
        auto s2 = maxps4::scan_null_links(b, b + sizeof buf);
        CHECK(s2.count == 2 && s2.guest_rip == 0x7000c67e20);
        const uint32_t other = 0x14000001u; std::memcpy(buf + 0x08, &other, 4); // caller no longer branches there
        auto s3 = maxps4::scan_null_links(b, b + sizeof buf);
        CHECK(s3.count == 1 && s3.guest_rip == 0x7000c68000 && s3.call);
        FatalFault c; c.signal = SIGILL; c.scan_known = true; c.null_links = 1; c.null_link_caller = 0x1122f0000; c.null_link_call = false;
        char out[1024];
        FatalFaultLog::format(c, out, sizeof out);
        CHECK(std::strstr(out, " null-links=1 scan-anchor=0x0 scan-begin=0x0 scan-end=0x0 scan-pages=0 null-link-caller=0x1122f0000 null-link-kind=jump\n") != nullptr);
    }
    std::puts("PASS fatal fault log: exact format, bounded truncation, append-only, disabled without a directory, null-dispatch call and link target");
    return 0;
}
