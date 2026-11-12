// Last-chance record of a host fault the guest fault guard does NOT contain
// (issue #296: MK Trilogy 2, SIGSEGV with host pc=0). Written from the signal
// handler just before the fault is chained to the previous handler, so the
// crash itself is unchanged. Only async-signal-safe calls (open/write/close)
// and no allocation; formatting is done by hand.
#pragma once
#include <cstddef>
#include <cstdint>
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
    // The jump thunk the top call went through, validated: its target RIP
    // and the HostCode ExitFunctionLink stored (0: the target failed).
    bool link_known = false;
    uint64_t link_rip = 0, link_host = 0;
    uint8_t link_bytes[16] {};
    uint8_t link_byte_count = 0;
    // Code-buffer scan for thunks ExitFunctionLink patched with HostCode 0.
    bool scan_known = false;
    uint32_t null_links = 0;
    uint64_t null_link_caller = 0;
    bool null_link_call = false;
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
        }
        if (f.link_known) {
            w.text(" link-rip="); w.hex(f.link_rip);
            w.text(" link-host="); w.hex(f.link_host);
            if (f.link_byte_count) { w.text(" link-bytes="); w.bytes(f.link_bytes, f.link_byte_count, sizeof f.link_bytes); }
        }
        if (f.scan_known) {
            w.text(" null-links="); w.dec(f.null_links);
            if (f.null_links) { w.text(" null-link-caller="); w.hex(f.null_link_caller); w.text(f.null_link_call ? " null-link-kind=call" : " null-link-kind=jump"); }
        }
        w.text("\n");
        return w.finish();
    }
    // From the signal handler.
    static void append(const FatalFault& f) {
        if (!enabled_) return;
        char line[1024];
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
