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
    // #296 H2/H3: ExitFunctionLink record classification, per-thread slots,
    // and the efl-* fields of the line.
    {
        using maxps4::LinkRecordState;
        maxps4::JumpThunk before {{0x14000002u, 0xd61f0000u, 0x58000100u, 0xd63f0000u}, 0, 0x7000c68000, 0x40};
        maxps4::JumpThunk after = before;
        CHECK(maxps4::classify_link_record(before, after) == LinkRecordState::intact);
        after.insn[0] = 0x58000080u; // FEX: ldr x0, #0x10 (out of range)
        CHECK(maxps4::classify_link_record(before, after) == LinkRecordState::linked);
        after.host_code = 0x1128a0000; // FEX: HostCode stored
        CHECK(maxps4::classify_link_record(before, after) == LinkRecordState::linked);
        maxps4::JumpThunk reused = after; reused.guest_rip = 0x7000aaaa00; // other code now
        CHECK(maxps4::classify_link_record(before, reused) == LinkRecordState::overwritten);
        reused = after; reused.insn[3] = 0xaa0103e0u;
        CHECK(maxps4::classify_link_record(before, reused) == LinkRecordState::overwritten);
        reused = before; reused.insn[0] = 0xd503201fu; // word 0 not one of FEX's values
        CHECK(maxps4::classify_link_record(before, reused) == LinkRecordState::overwritten);
        CHECK(std::strcmp(maxps4::link_record_state_name(LinkRecordState::overwritten), "overwritten") == 0);

        static maxps4::ExitLinkCalls trace;
        auto* a = trace.claim(0x1000);
        auto* b = trace.claim(0x2000);
        CHECK(a && b && a != b && trace.claim(0x1000) == a);
        maxps4::ExitLinkCall in; in.sequence = 7; in.guest_rip = 0x7000c68000; in.record = 0x1128a2050;
        in.state = LinkRecordState::overwritten; in.overwrites_before = 2; in.overwrites_after = 3;
        maxps4::ExitLinkCalls::store(*a, in);
        maxps4::ExitLinkCall out;
        CHECK(maxps4::ExitLinkCalls::load(*trace.find(0x1000), out) && out.sequence == 7 && out.guest_rip == 0x7000c68000);
        CHECK(!maxps4::ExitLinkCalls::load(*trace.find(0x2000), out)); // no call yet
        trace.release(0x1000);
        CHECK(trace.find(0x1000) == nullptr && trace.claim(0x3000) != nullptr);

        FatalFault c; c.signal = SIGILL; c.contained = true;
        c.efl_known = true; c.efl_seq = 7; c.efl_rip = 0x7000c68000; c.efl_result = 0; c.efl_record = 0x1128a2050;
        c.efl_state = uint8_t(LinkRecordState::overwritten); c.efl_now = uint8_t(LinkRecordState::intact);
        c.efl_overwrites_before = 2; c.efl_overwrites_after = 3;
        c.efl_calls = 912; c.efl_zero = 1; c.efl_overwrites = 3; c.efl_unreadable = 0;
        char out_line[1536];
        FatalFaultLog::format(c, out_line, sizeof out_line);
        CHECK(std::strstr(out_line, " efl-seq=7 efl-rip=0x7000c68000 efl-ret=0x0 efl-record=0x1128a2050"
                                    " efl-after=overwritten efl-now=intact efl-ow=2..3"
                                    " efl-calls=912 efl-zero=1 efl-overwrites=3 efl-unreadable=0\n") != nullptr);
    }
    {
        // #296 step 2: which branch reached pc=0. Classification and format
        // only; these tests do not exercise the runtime capture.
        using maxps4::IndirectBranchTrace;
        using maxps4::IndirectPath;
        auto base = [] {
            IndirectBranchTrace t; t.known = true; t.x28 = t.frame = 0x10a000000;
            t.state_callret_sp = 0x10b000ff0; t.x25 = t.state_callret_sp;
            return t;
        };
        IndirectBranchTrace t;
        maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::unavailable && !t.target_known);

        t = base(); t.x28 = 0x1234; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::state_mismatch);

        t = base(); t.x0 = 5; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::other_branch);

        // RET popped {0,0} and branched to x1 = 0.
        t = base(); t.x25 += 0x10; t.popped_known = true; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::ret_callret_zero && t.target_known && t.target == 0);

        // RET missed the call-ret entry, then L1 slot 0 {0,0} matched.
        t = base(); t.x25 += 0x10; t.popped_known = true; t.popped_guest = 0x7000c67e17; t.popped_host = 0x113bc7950;
        t.l1_known = true; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::ret_miss_l1_zero && t.target_known);

        // Same pop, but L1 slot 0 holds a real block: unexplained, no target.
        t.l1_host = 0x113000000; t.l1_guest = 0x7000000000; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::ret_unexplained && !t.target_known);

        // One pop but the entry was not readable.
        t = base(); t.x25 += 0x10; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::ret_unexplained);

        // No pop, L1 slot 0 {0,0}: the dispatcher is excluded only by a non-null efl-ret.
        t = base(); t.l1_known = true; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::l1_zero && t.target_known);
        maxps4::classify_indirect(t, true, false);
        CHECK(t.path == IndirectPath::l1_zero_or_dispatcher);
        maxps4::classify_indirect(t, false, false);
        CHECK(t.path == IndirectPath::l1_zero_or_dispatcher);

        // L1 slot 0 unreadable, or x1 != 0: nothing reconstructed.
        t = base(); maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::dispatcher_or_unknown && !t.target_known);
        t = base(); t.l1_known = true; t.x1 = 1; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::dispatcher_or_unknown);

        // Two pops or a push: not one of the modelled paths.
        t = base(); t.x25 += 0x20; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::callret_delta_other);
        t = base(); t.x25 -= 0x10; maxps4::classify_indirect(t, true, true);
        CHECK(t.path == IndirectPath::callret_delta_other && t.callret_delta() == -0x10);

        // Format: every memory field unreadable -> "na", target unknown.
        FatalFault c; c.signal = SIGILL; c.contained = true;
        c.indirect = base(); c.indirect.x25 -= 0x10; c.indirect.x8 = 0x7ffff000;
        maxps4::classify_indirect(c.indirect, false, false);
        char line[2048];
        FatalFaultLog::format(c, line, sizeof line);
        CHECK(std::strstr(line, " ind-path=callret-delta-other ind-x0=0x0 ind-x1=0x0 ind-x8=0x7ffff000"
                                " ind-x25=0x10b000fe0 ind-x28=0x10a000000 ind-frame=match"
                                " ind-callret-sp=0x10b000ff0 ind-callret-delta=-0x10"
                                " ind-popped=na ind-l1-0=na ind-rsp-slot=na ind-target=unknown\n") != nullptr);
        // Observed memory and a reconstructed target.
        c.indirect = base(); c.indirect.x25 += 0x10; c.indirect.popped_known = true;
        c.indirect.l1_known = true; c.indirect.l1_host = 0x113000000; c.indirect.l1_guest = 0x7000000000;
        c.indirect.rsp_slot_known = true;
        maxps4::classify_indirect(c.indirect, true, true);
        FatalFaultLog::format(c, line, sizeof line);
        CHECK(std::strstr(line, " ind-path=cand-ret-callret-zero ") != nullptr);
        CHECK(std::strstr(line, " ind-callret-delta=+0x10 ind-popped=0x0:0x0 ind-l1-0=0x113000000:0x7000000000"
                                " ind-rsp-slot=0x0 ind-target=0x0(reconstructed)\n") != nullptr);
        c.indirect.x28 = 1;
        FatalFaultLog::format(c, line, sizeof line);
        CHECK(std::strstr(line, " ind-frame=mismatch") != nullptr);
        // Not captured: no ind-* field at all.
        c.indirect = IndirectBranchTrace{};
        FatalFaultLog::format(c, line, sizeof line);
        CHECK(std::strstr(line, " ind-") == nullptr);

        // Every field set at maximum width: the efl-* fields stay present, the
        // ind-* fields follow them, and the line is not truncated.
        FatalFault m; m.signal = SIGILL; m.contained = true; m.code = -1;
        m.address = m.pc = m.lr = m.fp = m.sp = ~0ull; for (auto& x : m.x) x = ~0ull;
        m.guest_thread_known = true; m.guest_thread = ~0ull; m.guest_rip = ~0ull; m.byte_count = 16;
        m.dispatch_known = true; m.lr_insn = ~0u; m.dispatch = 2;
        m.callret_known = true; m.callret_rip = m.callret_host = ~0ull; m.callret_previous_instruction = ~0u;
        m.link_known = true; m.link_rip = m.link_host = ~0ull; m.link_byte_count = 16;
        m.scan_known = true; m.null_links = ~0u; m.scan_anchor = m.scan_begin = m.scan_end = ~0ull; m.scan_pages = ~0u;
        m.null_link_caller = ~0ull;
        m.efl_known = true; m.efl_seq = m.efl_rip = m.efl_result = m.efl_record = ~0ull;
        m.efl_overwrites_before = m.efl_overwrites_after = ~0ull;
        m.efl_calls = m.efl_zero = m.efl_overwrites = m.efl_unreadable = ~0ull;
        auto& w = m.indirect; w.known = true; w.x0 = w.x1 = w.x8 = w.x25 = ~0ull; w.x28 = w.frame = ~0ull;
        w.state_callret_sp = 0; w.popped_known = w.l1_known = w.rsp_slot_known = true;
        w.popped_guest = w.popped_host = w.l1_host = w.l1_guest = w.rsp_slot = ~0ull;
        w.path = IndirectPath::l1_zero_or_dispatcher; w.target_known = true; w.target = ~0ull;
        const size_t n = FatalFaultLog::format(m, line, sizeof line);
        CHECK(n + 1 < sizeof line && line[n - 1] == '\n');
        const char* efl = std::strstr(line, " efl-unreadable=");
        const char* ind = std::strstr(line, " ind-path=cand-l1-zero-or-dispatcher");
        CHECK(efl && ind && efl < ind && std::strstr(line, "(reconstructed)\n"));
        std::printf("max line length %zu of %zu\n", n, sizeof line);
    }
    std::puts("PASS fatal fault log: exact format, bounded truncation, append-only, disabled without a directory, null-dispatch call and link target");
    return 0;
}
