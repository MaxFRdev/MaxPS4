#import <Foundation/Foundation.h>
#include "MaxPS4CrashLog.h"
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

namespace {
char crash_path[1024];
struct sigaction previous_abort, previous_fpe;

void put(int fd, const char* text) { if (text) (void)!::write(fd, text, strlen(text)); }
void put_hex(int fd, uint64_t value) {
    char text[19] = "0x";
    for (int i = 0; i < 16; ++i) text[2 + i] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 0xf];
    text[18] = 0;
    put(fd, text);
}
int open_log() { return crash_path[0] ? ::open(crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1; }

void write_backtrace(int fd) {
    void* frames[64];
    const int count = backtrace(frames, 64);
    put(fd, "Pile hôte :\n");
    backtrace_symbols_fd(frames, count, fd);
}

// Only async-signal-safe calls (open, write, backtrace on a warmed-up libunwind).
void on_signal(int signal, siginfo_t* info, void* context) {
    const int fd = open_log();
    if (fd >= 0) {
        put(fd, "\n=== Fermeture : signal ");
        put(fd, signal == SIGABRT ? "SIGABRT (abort)" : signal == SIGFPE ? "SIGFPE" : "?");
        put(fd, " · adresse "); put_hex(fd, uint64_t(uintptr_t(info ? info->si_addr : nullptr)));
#if defined(__arm64__)
        if (context) {
            auto* uc = static_cast<ucontext_t*>(context);
            put(fd, " · PC "); put_hex(fd, uc->uc_mcontext->__ss.__pc);
            put(fd, " · LR "); put_hex(fd, uc->uc_mcontext->__ss.__lr);
        }
#endif
        put(fd, " · thread mach "); put_hex(fd, uint64_t(mach_thread_self()));
        put(fd, "\n");
        write_backtrace(fd);
        ::close(fd);
    }
    // Let the crash go on as before (system crash report, debugger).
    struct sigaction& previous = signal == SIGABRT ? previous_abort : previous_fpe;
    ::sigaction(signal, &previous, nullptr);
    ::raise(signal);
}

void on_exception(NSException* exception) {
    const int fd = open_log();
    if (fd < 0) return;
    NSString* text = [NSString stringWithFormat:@"\n=== Fermeture : exception %@ : %@\n%@\n", exception.name, exception.reason,
                      [exception.callStackSymbols componentsJoinedByString:@"\n"]];
    put(fd, text.UTF8String);
    ::close(fd);
}
}

extern "C" void maxps4_crash_log_install(void) {
    NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!documents) return;
    NSString* path = [documents stringByAppendingPathComponent:@"maxps4-fermeture.txt"];
    strlcpy(crash_path, path.fileSystemRepresentation, sizeof crash_path);
    // Warm up the unwinder outside a signal handler.
    void* frames[4];
    (void)backtrace(frames, 4);
    struct sigaction action {};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGABRT, &action, &previous_abort);
    ::sigaction(SIGFPE, &action, &previous_fpe);
    NSSetUncaughtExceptionHandler(&on_exception);
}
