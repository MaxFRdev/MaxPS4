#pragma once
#include <stddef.h>
#include <stdint.h>
#pragma GCC visibility push(default)
#ifdef __cplusplus
extern "C" {
#endif
// Process-local, debugger-written mailbox. No entitlement flags are changed.
// begin refuses replacement of pending/ready sessions. cancel requires no leases.
int32_t maxps4_jit_begin(uint64_t *mailbox, uint64_t *nonce, uint64_t *page_size);
int32_t maxps4_jit_prepare(void);
// Confirmation is only debugger publication, NEVER permission or FEX execution.
int32_t maxps4_jit_confirmation(void); // 0 pending/idle, 1 published, 2 script failure
// Only call after UIApplication reports that the activation URL was NOT opened.
int32_t maxps4_jit_launch_failed(uint64_t nonce);
int32_t maxps4_jit_cancel(void);
int32_t maxps4_jit_state(void); // 0 idle, 1 waiting, 2 ready, 3 failed
int32_t maxps4_jit_error(void);
int32_t maxps4_jit_mailbox_diagnostic(void); // 0 idle, 1 absent, 2 stale, 3..4 rejected, 5 published
int32_t maxps4_jit_failure_stage(void); // 0 unknown; 1..12 documented in Swift diagnostic
int32_t maxps4_jit_failure_mach(void);
int32_t maxps4_jit_failure_region(void); // -1 before per-region checks
uint32_t maxps4_jit_leases(void);
void *maxps4_jit_allocate(size_t size);
int32_t maxps4_jit_free(void *write, size_t size);
void *maxps4_jit_executable(void *write);
void *maxps4_jit_writable(void *execute);
int32_t maxps4_jit_describe(void *address, uintptr_t *base, size_t *offset, size_t *size);
size_t maxps4_jit_usable_size(void *write, size_t size);
// 1 when the calling thread is inside the JIT pool lock (async-signal-safe):
// it is then running MaxPS4 host code, not JIT code.
int32_t maxps4_jit_held(void);
#ifdef __cplusplus
}
#endif

#pragma GCC visibility pop
