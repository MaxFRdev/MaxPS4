// Last-chance record of a host fault the guest fault guard does NOT contain
// (issue #296: MK Trilogy 2, SIGSEGV with host pc=0). Written from the signal
// handler just before the fault is chained to the previous handler, so the
// crash itself is unchanged. Only async-signal-safe calls (open/write/close)
// and no allocation; formatting is done by hand.
#pragma once
#include <cstddef>
#include <cstdint>
#include "maxps4_dispatch_fault.h"
#include "maxps4_exit_link_trace.h"
#include "maxps4_indirect_zero_trace.h"
#include <fcntl.h>
#include <unistd.h>

namespace maxps4 {
struct FatalFault {
    enum Where : uint8_t { other = 0, jit = 1, dispatcher = 2 };
    int signal = 0, code = 0;
    uint64_t address = 0, pc = 0, lr = 0, fp = 0, sp = 0, x[4] {};
    Where pc_where = other, lr_where = other;
    bool guest_thread_known = false;
    uint64_t guest_thread = 0, guest_rip = 0; // guest_rip: from LR when it is in JIT code, else FEX's last stored RIP.
    // Contained as a guest fault (the app survives) rather than chained.
    bool contained = false;
    // Guest code bytes at guest_rip, when readable (uncompilable block).
    uint8_t bytes[16] {};
    uint8_t byte_count = 0;
    // Null dispatch (#296): the instruction before LR names the dispatcher
    // call that returned 0 (dispatch: 1 compile-block, 2 exit-link).
    bool dispatch_known = false;
    uint32_t lr_insn = 0;
    uint8_t dispatch = 0;
    // Top of FEX's call-ret stack {guest return RIP, host return PC}.
    bool callret_known = false;
    uint64_t callret_rip = 0, callret_host = 0;
    uint32_t callret_previous_instruction = 0;
    ExitLinkTrace exit_link_trace = ExitLinkTrace::unavailable;
    // The jump thunk the top call went through, validated: its target RIP
    // and the HostCode ExitFunctionLink stored (0: the target failed).
    bool link_known = false;
    uint64_t link_rip = 0, link_host = 0;
    uint8_t link_bytes[16] {};
    uint8_t link_byte_count = 0;
    // Code-buffer scan for thunks ExitFunctionLink patched with HostCode 0.
    bool scan_known = false;
    uint32_t null_links = 0;
    // Scan coverage: distinguish no matching thunk from no readable JIT pages.
    uint64_t scan_anchor = 0, scan_begin = 0, scan_end = 0;
    uint32_t scan_pages = 0;
    // This thread's last ExitFunctionLink call, seen by MaxPS4's pass-through
    // wrapper (maxps4_exit_link_trace.h): the real target GuestRIP, the result,
    // and whether the thunk record survived the call (H2) and until now (H3).
    bool efl_known = false;
    uint64_t efl_seq = 0, efl_rip = 0, efl_result = 0, efl_record = 0;
    uint8_t efl_state = 0, efl_now = 0; // LinkRecordState
    uint64_t efl_overwrites_before = 0, efl_overwrites_after = 0;
    uint64_t efl_calls = 0, efl_zero = 0, efl_overwrites = 0, efl_unreadable = 0;
    uint64_t null_link_caller = 0;
    bool null_link_call = false;
    // Which branch reached pc=0 (maxps4_indirect_zero_trace.h): registers,
    // call-ret pop, L1 slot 0, guest stack slot, and a candidate path.
    IndirectBranchTrace indirect;
};

class FatalFaultLog {
public:
    // Not from the handler: called once when a guard installs its handlers.
    static void set_path(const char* directory) {
        size_t n = 0;
        for (const char* p = directory; p && *p && n + 1 < sizeof(path_); ++p) path_[n++] = *p;
        static constexpr char name[] = "/maxps4-fatal-fault.txt";
        for (size_t i = 0; name[i] && n + 1 < sizeof(path_); ++i) path_[n++] = name[i];
        path_[n] = 0;
        enabled_ = directory && *directory;
    }
    // One line, NUL-terminated; returns its length (without NUL).
    static size_t format(const FatalFault& f, char* out, size_t capacity) {
        Writer w {out, capacity};
        w.text(f.contained ? "contained-host-fault signal=" : "fatal-host-fault signal="); w.dec(uint64_t(f.signal));
        w.text(" code="); w.dec(uint64_t(uint32_t(f.code)));
        w.text(" addr="); w.hex(f.address);
        w.text(" pc="); w.hex(f.pc); w.text("("); w.text(where(f.pc_where)); w.text(")");
        w.text(" lr="); w.hex(f.lr); w.text("("); w.text(where(f.lr_where)); w.text(")");
        w.text(" fp="); w.hex(f.fp); w.text(" sp="); w.hex(f.sp);
        for (int i = 0; i < 4; ++i) { w.text(" x"); w.dec(uint64_t(i)); w.text("="); w.hex(f.x[i]); }
        if (f.guest_thread_known) {
            w.text(" guest-thread="); w.dec(f.guest_thread);
            w.text(" guest-rip="); w.hex(f.guest_rip);
        } else w.text(" guest-thread=none");
        if (f.byte_count) { w.text(" guest-bytes="); w.bytes(f.bytes, f.byte_count, sizeof f.bytes); }
        if (f.dispatch_known) {
            w.text(" lr-insn="); w.hex(f.lr_insn);
            w.text(" dispatch="); w.text(f.dispatch == 1 ? "compile-block" : f.dispatch == 2 ? "exit-link" : "unknown");
        }
        if (f.callret_known) {
            w.text(" callret-rip="); w.hex(f.callret_rip);
            w.text(" callret-host="); w.hex(f.callret_host);
            w.text(" callret-prev="); w.hex(f.callret_previous_instruction);
            w.text(" exit-link-trace="); w.text(exit_link_trace_name(f.exit_link_trace));
        }
        if (f.link_known) {
            w.text(" link-rip="); w.hex(f.link_rip);
            w.text(" link-host="); w.hex(f.link_host);
            if (f.link_byte_count) { w.text(" link-bytes="); w.bytes(f.link_bytes, f.link_byte_count, sizeof f.link_bytes); }
        }
        if (f.scan_known) {
            w.text(" null-links="); w.dec(f.null_links);
            w.text(" scan-anchor="); w.hex(f.scan_anchor);
            w.text(" scan-begin="); w.hex(f.scan_begin);
            w.text(" scan-end="); w.hex(f.scan_end);
            w.text(" scan-pages="); w.dec(f.scan_pages);
            if (f.null_links) { w.text(" null-link-caller="); w.hex(f.null_link_caller); w.text(f.null_link_call ? " null-link-kind=call" : " null-link-kind=jump"); }
        }
        if (f.efl_known) {
            w.text(" efl-seq="); w.dec(f.efl_seq);
            w.text(" efl-rip="); w.hex(f.efl_rip);
            w.text(" efl-ret="); w.hex(f.efl_result);
            w.text(" efl-record="); w.hex(f.efl_record);
            w.text(" efl-after="); w.text(link_record_state_name(static_cast<LinkRecordState>(f.efl_state)));
            w.text(" efl-now="); w.text(link_record_state_name(static_cast<LinkRecordState>(f.efl_now)));
            w.text(" efl-ow="); w.dec(f.efl_overwrites_before); w.text(".."); w.dec(f.efl_overwrites_after);
        }
        if (f.efl_calls) {
            w.text(" efl-calls="); w.dec(f.efl_calls);
            w.text(" efl-zero="); w.dec(f.efl_zero);
            w.text(" efl-overwrites="); w.dec(f.efl_overwrites);
            w.text(" efl-unreadable="); w.dec(f.efl_unreadable);
        }
        if (f.indirect.known) {
            // Observed: ind-x*, ind-frame, ind-callret-*, ind-popped, ind-l1-0,
            // ind-rsp-slot ("na": not read safely). Reconstructed: ind-target.
            // Hypothesis: ind-path (cand-*), never a confirmation.
            const auto& t = f.indirect;
            w.text(" ind-path="); w.text(indirect_path_name(t.path));
            w.text(" ind-x0="); w.hex(t.x0);
            w.text(" ind-x1="); w.hex(t.x1);
            w.text(" ind-x8="); w.hex(t.x8);
            w.text(" ind-x25="); w.hex(t.x25);
            w.text(" ind-x28="); w.hex(t.x28);
            w.text(t.state_ok() ? " ind-frame=match" : " ind-frame=mismatch");
            w.text(" ind-callret-sp="); w.hex(t.state_callret_sp);
            w.text(" ind-callret-delta="); w.shex(t.callret_delta());
            w.text(" ind-popped=");
            if (t.popped_known) { w.hex(t.popped_guest); w.text(":"); w.hex(t.popped_host); } else w.text("na");
            w.text(" ind-l1-0=");
            if (t.l1_known) { w.hex(t.l1_host); w.text(":"); w.hex(t.l1_guest); } else w.text("na");
            w.text(" ind-rsp-slot=");
            if (t.rsp_slot_known) w.hex(t.rsp_slot); else w.text("na");
            w.text(" ind-target=");
            if (t.target_known) { w.hex(t.target); w.text("(reconstructed)"); } else w.text("unknown");
        }
        w.text("\n");
        return w.finish();
    }
    // From the signal handler.
    static void append(const FatalFault& f) {
        if (!enabled_) return;
        char line[2048];
        const size_t n = format(f, line, sizeof line);
        const int fd = ::open(path_, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) return;
        for (size_t done = 0; done < n;) {
            const ssize_t w = ::write(fd, line + done, n - done);
            if (w <= 0) break;
            done += size_t(w);
        }
        ::close(fd);
    }
    static const char* path() { return path_; }
private:
    static const char* where(FatalFault::Where w) { return w == FatalFault::jit ? "jit" : w == FatalFault::dispatcher ? "dispatcher" : "other"; }
    struct Writer {
        char* out; size_t capacity; size_t n = 0;
        void put(char c) { if (n + 1 < capacity) out[n++] = c; }
        void text(const char* s) { while (*s) put(*s++); }
        void hex(uint64_t v) {
            put('0'); put('x');
            char digits[16]; int k = 0;
            do { digits[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
            while (k) put(digits[--k]);
        }
        void bytes(const uint8_t* b, size_t count, size_t limit) {
            for (size_t i = 0; i < count && i < limit; ++i) { put("0123456789abcdef"[b[i] >> 4]); put("0123456789abcdef"[b[i] & 15]); }
        }
        void shex(int64_t v) { if (v < 0) { put('-'); hex(uint64_t(0) - uint64_t(v)); } else { put('+'); hex(uint64_t(v)); } }
        void dec(uint64_t v) {
            char digits[20]; int k = 0;
            do { digits[k++] = char('0' + v % 10); v /= 10; } while (v);
            while (k) put(digits[--k]);
        }
        size_t finish() { if (capacity) out[n] = 0; return n; }
    };
    static inline char path_[1024] {};
    static inline bool enabled_ = false;
};
} // namespace maxps4
