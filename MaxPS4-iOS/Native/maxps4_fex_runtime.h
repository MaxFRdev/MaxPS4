#pragma once
#include <stddef.h>
#include <stdint.h>
#include "maxps4_jit_memory.h"
#include "OrbisLinkerBridge.h"
#include "maxps4_ps4_videoout_abi.h"
#include "maxps4_gnm_submission_abi.h"
#ifdef __cplusplus
extern "C" {
#endif
// Opaque engine: no FEX C++ types cross the native/Swift boundary.
typedef struct maxps4_fex_engine maxps4_fex_engine;
// Self-contained synthetic Orbis test, using the existing loader/linker/FEX
// runtime. Status -ECANCELED means an operation was not attempted. Values are
// valid only when their operation succeeded; no interpreter fallback occurs.
typedef struct maxps4_ps4_elf_test_report {
    int32_t open_status, load_status, link_status, initialize_status;
    int32_t execute_status, read_status, verification_status;
    uint32_t backend, segments, imports, resolved_service, calls, resumed;
    uint32_t jit_regions_before, jit_regions_after, guest_segments_after;
    uint64_t entry, kernel_return, final_memory, rax;
    char error[256];
} maxps4_ps4_elf_test_report;
// Returns 0 only for verified FEX execution (backend 2 native, 3 simulator).
// Owns and closes its engine on every path. Call from the app's CPU queue.
int32_t maxps4_fex_test_ps4_libkernel(maxps4_ps4_elf_test_report *report);
// PS4 System V entry registers, not native ARM64 registers or host pointers.
typedef struct maxps4_ps4_kernel_call {
    uint64_t rdi, rsi, rdx, rcx, r8, r9, rsp;
} maxps4_ps4_kernel_call;
// Resolve a decoded sceKernel/scePthread import to an opaque service ID.
// Returns 0 for unknown symbols/versions. Does not apply ELF/PRX relocations.
uint32_t maxps4_ps4_resolve_libkernel(const char *symbol, const char *library,
    const char *module, uint32_t version);
// Mount configuration is host-side. Return 0 or signed Orbis kernel error.
// Must mount AFTER staging; each staging attempt resets all system services.
int64_t maxps4_fex_engine_mount_app0(maxps4_fex_engine *engine, const char *directory);
// The same for "/app0" (read-only), "/savedata0", "/download0" or "/temp0".
int64_t maxps4_fex_engine_mount(maxps4_fex_engine *engine, const char *guest, const char *directory);
// The directory holding this game's save data directories (sceSaveDataMount2
// mounts <directory>/<dirName> at /savedata0). Also after staging.
int64_t maxps4_fex_engine_set_savedata_root(maxps4_fex_engine *engine, const char *directory);
// The PS4 language ID games are told (0 Japanese, 1 English US, 2 French,
// 3 Spanish, 4 German, 5 Italian, 17 Portuguese (Brazil), ...). After staging.
int32_t maxps4_fex_engine_set_system_language(maxps4_fex_engine *engine, int32_t language);
// An installed add-on content (DLC) of the game: its 16-character entitlement
// label (the end of its CONTENT_ID) and its extracted directory. After staging.
int64_t maxps4_fex_engine_add_addcont(maxps4_fex_engine *engine, const char *label, const char *directory);
// Explicit service dispatch, serialized against staging and engine accesses.
// Returns a guest ABI result, including signed Orbis errors. Default bootstrap
// guest thread identity is 1. No guest CPU import trampoline is installed.
int64_t maxps4_fex_engine_call_libkernel(maxps4_fex_engine *engine, uint32_t service,
    const maxps4_ps4_kernel_call *call, uint64_t guest_thread);
// Backend-side GNM completion bridge. Capture a registration-scoped ticket
// when scheduling work; signal it only after real GPU completion. No automatic
// producer/rendering is installed. Errors use Orbis kernel codes; capture
// clears *ticket on failure. Deletion/re-registration/restaging cancels tickets.
// These two calls may run concurrently with guest execution (no runtime lock),
// so a completion can wake sceKernelWaitEqueue. The caller MUST keep the engine
// alive until every completion callback finishes and serialize close itself.
int64_t maxps4_fex_engine_graphics_event_ticket(maxps4_fex_engine *engine,
    uint64_t queue, uint64_t id, uint64_t *ticket);
int64_t maxps4_fex_engine_complete_graphics_event(maxps4_fex_engine *engine,
    uint64_t ticket);
// Explicitly attach/detach the native triangle diagnostic consumer (0/1).
// It accepts no guest shader/register state, indexed draws, CCB or flips.
// 2 attaches the null backend instead (boot probe): it draws nothing and
// queues no ticket; submissions apply their label writes and EOP interrupts,
// and sceGnmSubmitAndFlipCommandBuffers then requests the VideoOut flip.
// 3 attaches the software backend: the null backend plus a CPU execution of
// every submission (GCN interpreter), slow but drawing what the game drew.
// Default/restaged state is detached; detach cancels all outstanding tickets.
// All three calls avoid the FEX runtime lock; caller retains engine and
// serializes close. Snapshot capacity is in dwords; null/0 queries metadata.
// Complete only the front ticket, after real GPU command completion. success=0
// cancels remaining work and latches failure until reattachment/reset. This
// never confirms presentation or emits GNM events without a command requesting it.
int32_t maxps4_fex_engine_gnm_backend(maxps4_fex_engine* engine, uint32_t enabled);
// Backend 4: synchronous app consumer. Attach after load_game, before run.
// Return zero only after GPU completion. No GNM reentry from the callback.
// Caller owns context until run returns and the backend is detached.
typedef int32_t (*maxps4_gnm_host_consumer)(void*, const uint32_t*, size_t);
int32_t maxps4_fex_engine_gnm_host_consumer(maxps4_fex_engine* engine,
    void* context, maxps4_gnm_host_consumer consumer);
// Checked guest-memory copy. Safe from a synchronous host-consumer callback;
// caller serializes against engine close and guest memory mutation.
int32_t maxps4_fex_engine_read_memory(maxps4_fex_engine* engine,
    uint64_t address, void* destination, size_t bytes);
// Checked guest-memory write for a synchronous host-consumer callback. The
// guest submission thread is paused until the callback returns; caller keeps
// the engine alive and serializes close, as for read_memory.
int32_t maxps4_fex_engine_write_memory(maxps4_fex_engine* engine,
    uint64_t address, const void* source, size_t bytes);
// The host bytes backing readable guest [address, address + bytes), for the
// GPU to read guest textures in place (a no-copy Metal buffer). Valid until
// the game unmaps them; same calling rules as read_memory.
int32_t maxps4_fex_engine_gpu_view(maxps4_fex_engine* engine,
    uint64_t address, size_t bytes, const void** pointer);
// Pending flip metadata and its registered colour-target address, no CPU copy.
int32_t maxps4_fex_engine_videoout_target(maxps4_fex_engine* engine,
    maxps4_videoout_frame* frame, uint64_t* address);
int32_t maxps4_fex_engine_gnm_snapshot(maxps4_fex_engine* engine,
    maxps4_gnm_submission* submission, uint32_t* words, size_t capacity);
int32_t maxps4_fex_engine_gnm_complete(maxps4_fex_engine* engine, uint64_t ticket, uint32_t success);
// Software backend counters (any pointer may be null) and its first error,
// NUL-terminated and truncated to `capacity`. No FEX runtime lock.
int32_t maxps4_fex_engine_gnm_software_status(maxps4_fex_engine* engine, uint32_t* draws,
    uint32_t* rendered, uint32_t* failed, char* error, size_t capacity);
// Concurrent host input. Values use PS4 button bits and 0..255 stick/trigger
// samples (sticks centered at 128). No global execution lock is acquired.
// Caller retains the engine and serializes against close; connected is 0 or 1.
int32_t maxps4_fex_engine_pad_update(maxps4_fex_engine* engine, uint32_t buttons,
    uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry, uint8_t l2, uint8_t r2, uint32_t connected);
// Concurrent host audio: mixes up to `frames` 48 kHz interleaved stereo float
// frames of the game's AudioOut ports into `samples` (zeros where none).
// Returns the frames that had data, or -EINVAL. Same ownership rule as
// pad_update; no FEX runtime lock.
int32_t maxps4_fex_engine_audio_read(maxps4_fex_engine* engine, float* samples, uint32_t frames);
// AudioOut counters (any pointer may be null): Output calls, frames written,
// frames dropped (ring full, no reader), silent frames read, open ports,
// written frames with a non-zero sample.
int32_t maxps4_fex_engine_audio_status(maxps4_fex_engine* engine, uint64_t* outputs,
    uint64_t* written, uint64_t* dropped, uint64_t* silent, uint32_t* ports, uint64_t* audible);
// Owned linear VideoOut frame (raw RGBA/BGRA bytes, including row padding).
// With pixels=NULL/capacity=0, query metadata only. Otherwise capacity must
// cover byte_count. Returns 0, -EAGAIN if no frame, or -EINVAL for bad outputs;
// metadata is zero on failure and pixel output is untouched on failure.
// Completion(false) cancels without advancing flip status or signalling events.
// vblank must be called by a real display timing source, not SubmitFlip.
// These calls may run concurrently with guest execution, but the caller must
// keep the engine alive until callbacks finish and serialize close itself.
int32_t maxps4_fex_engine_videoout_snapshot(maxps4_fex_engine *engine,
    maxps4_videoout_frame *frame, uint8_t *pixels, size_t capacity);
int64_t maxps4_fex_engine_videoout_complete(maxps4_fex_engine *engine,
    uint64_t ticket, uint32_t presented);
int64_t maxps4_fex_engine_videoout_vblank(maxps4_fex_engine *engine, uint64_t port);
// Host pause (game menu): vblanks are ignored and flips waiting for one keep
// waiting, which holds a vblank-paced game. Resume before stopping the run.
void maxps4_fex_engine_videoout_pause(maxps4_fex_engine *engine, uint32_t paused);
// Flips of sceGnmSubmitAndFlipCommandBuffers skipped because the previous one
// was still pending (their prepareFlip label is still written).
uint64_t maxps4_fex_engine_videoout_dropped(maxps4_fex_engine *engine);
// Output resolution reported to the game: 1920x1080 (default) or 1280x720.
// Set after loading, before running; kept across reloads. -EINVAL otherwise.
int32_t maxps4_fex_engine_videoout_output(maxps4_fex_engine *engine, uint32_t width, uint32_t height);
// Flips the game asked for so far: accepted plus skipped.
uint64_t maxps4_fex_engine_videoout_requested(maxps4_fex_engine *engine);
// One line per guest thread of the current run: the HLE call it is in and for
// how long, or how long it has run guest code since its last call. Diagnostic
// only, lock-free, NUL-terminated and truncated to `capacity`.
int32_t maxps4_fex_engine_thread_activity(maxps4_fex_engine *engine, char *text, size_t capacity);
// Where the main guest thread of the running image is right now, sampled by
// a signal that lets it continue. Refused (-EBUSY) while it is inside an HLE
// call or returned from one less than 2 s ago, -ESRCH when nothing runs. Diagnostic only. On Apple devices a thread
// that never takes the signal (signals masked, e.g. inside a FEX lock) is
// suspended briefly through Mach instead (`method` 1), 16 times 1 ms apart:
// the first read gives its registers and frame-pointer chain, every read in
// JIT code a guest RIP translated back from the host PC.
typedef struct maxps4_fex_thread_sample {
    uint64_t host_pc, host_lr;
    uint64_t guest_rip; // JIT: reconstructed; otherwise FEX's last stored RIP.
    uint32_t location;  // 1: JIT code, 2: FEX dispatcher, 3: other host code.
    uint32_t method;    // 0: signal, 1: Mach suspension (the signal was not taken).
    char symbol[128];   // Host symbol of host_pc, "" when unknown.
    char caller[128];   // Host symbol of host_lr.
    char backtrace[768]; // Method 1: return addresses up the frame chain, " < " separated.
    uint64_t registers[29]; // Method 1: host x0..x28 at the first read.
    uint64_t rips[8];       // Method 1: distinct guest RIPs of the reads in JIT code.
    uint32_t rip_count;
} maxps4_fex_thread_sample;
int32_t maxps4_fex_engine_sample_main_thread(maxps4_fex_engine *engine, maxps4_fex_thread_sample *sample);
// CPU profile of the main guest thread for the next run (diagnostic, off by
// default; Apple devices only): sampled every 5 ms through Mach suspension.
int32_t maxps4_fex_engine_set_profiling(maxps4_fex_engine *engine, uint32_t enabled);
// Since the run started: JIT code / FEX dispatcher / host shares, the hottest
// guest RIPs (64-byte buckets, relative to the game's relocation) and host
// functions. NUL-terminated, truncated to `capacity`; empty without samples.
int32_t maxps4_fex_engine_profile_report(maxps4_fex_engine *engine, char *text, size_t capacity);
// Experimental: x86 memory-order (TSO) emulation for engines opened after
// this call (default 1). The VIXL simulator always runs with it off.
int32_t maxps4_fex_set_tso_emulation(uint32_t enabled);
enum { MAXPS4_CPU_RESTRICTED_INTERPRETER = 1, MAXPS4_CPU_FEX_ARM64_JIT = 2,
       MAXPS4_CPU_FEX_ARM64_SIMULATOR = 3 };
enum { MAXPS4_FEX_CONTEXT_CREATED = 1, MAXPS4_FEX_DISPATCHER_INITIALIZED = 2 };
// Creates the real executable dispatcher; does not execute guest code.
// iOS requires successful authorized debugger-region preparation first.
// Repeated initialization succeeds without allocating another dispatcher.
int32_t maxps4_fex_engine_initialize(maxps4_fex_engine *engine);
// Returns 0 or a negative errno. Engine owns its real FEX context/configuration.
int32_t maxps4_fex_engine_open(maxps4_fex_engine **out);
void maxps4_fex_engine_close(maxps4_fex_engine *engine);
int32_t maxps4_fex_engine_stage(const maxps4_fex_engine *engine);
// CPU entry snapshot from the last successfully completed guest execution.
// exited: a general image called its entry's exit function (RSI) with
// exit_status, which is also the returned RAX; 0 when it stopped on HLT.
typedef struct maxps4_fex_cpu_report {
    uint64_t entry, rax, argc, page_size;
    uint32_t rsp_alignment, zero_integer_registers, entry_matches,
             argv_env_null, auxv_valid, completed;
    uint32_t exited;
    int32_t exit_status;
} maxps4_fex_cpu_report;
int32_t maxps4_fex_engine_last_cpu_report(const maxps4_fex_engine *engine,
                                        maxps4_fex_cpu_report *report);
// Guest fault that ended the last execution: a SIGSEGV/SIGBUS (run returned
// -EFAULT) or SIGILL/SIGTRAP (-EILSEQ) raised by FEX-generated code or its
// guest-signal stubs, contained on ARM64 hosts (not under the VIXL simulator).
// address is the faulting address (identity-mapped images: a guest address);
// rip is the guest RIP FEX last stored, at block granularity. Returns -ENOENT
// when the last execution did not fault.
typedef struct maxps4_fex_guest_fault {
    int32_t signal;
    int32_t code; // host si_code (previously a reserved 32-bit slot)
    uint64_t address, rip;
} maxps4_fex_guest_fault;
int32_t maxps4_fex_engine_last_guest_fault(const maxps4_fex_engine *engine,
                                           maxps4_fex_guest_fault *fault);
// Wall-clock limit for each later FEX execution of this engine (0: none, at
// most one hour). When it expires while the guest runs generated code, the
// run ends with -ETIMEDOUT and last_guest_fault reports signal SIGURG,
// address 0. Time spent in FEX or HLE C++ code is not interrupted; the next
// guest instruction is. -ENOTSUP for a nonzero limit where faults cannot be
// contained (non-ARM64 host or VIXL simulator).
int32_t maxps4_fex_engine_set_time_limit(maxps4_fex_engine *engine, uint32_t milliseconds);
// Concurrent sticky cancellation, including a request before run starts.
// Runs return -ECANCELED. Interruption during execution requires native ARM64
// fault containment, as for set_time_limit; HLE waits are woken before stopping.
// Engine remains owned by the worker until run returns; close is not concurrent.
int32_t maxps4_fex_engine_request_stop(maxps4_fex_engine* engine);
int32_t maxps4_fex_engine_cpuid(maxps4_fex_engine *engine, uint32_t leaf,
                              uint32_t subleaf, uint32_t out[4]);
// Compatibility synthetic ELF fixture routed through run_elf; no host syscalls.
// Requires initialization and an available executable-memory provider.
// No interpreter fallback within this function.
int32_t maxps4_fex_engine_probe(maxps4_fex_engine *engine, uint64_t *result,
                              uint32_t *backend);
// Executes a bounded static ELF64 ET_EXEC image through real FEX only.
// Admits MOV EAX,imm32 / ADD EAX,imm32 / NOP / final HLT, and exact
// MOV dword [RIP+disp32],imm32 stores proven to target guest RW/non-X data.
// No branches, arbitrary memory access, relocations or syscalls. Every
// instruction, including a store, counts against the finite admission budget.
// Segments are rebased into owned host mappings, preserving relative offsets;
// entry reports the original ELF address, never a host pointer. No fallback.
// Caller keeps file bytes immutable during the call and serializes engine close.
int32_t maxps4_fex_engine_run_elf(maxps4_fex_engine *engine, const uint8_t *file,
                                size_t length, uint32_t budget, uint64_t *result,
                                uint32_t *backend, uint64_t *entry);
// Explicit ELF interpreter fallback; backend is never reported as FEX JIT.
// Reports logical next RIP and completed instruction count, including on errors.
int32_t maxps4_fex_engine_interpret_elf(maxps4_fex_engine *engine,
    const uint8_t *file, size_t length, uint32_t budget, uint64_t *result,
    uint32_t *backend, uint64_t *entry, uint64_t *rip, uint32_t *instructions);
// Same restricted real FEX execution, followed by a PM4 snapshot from that
// image's readable PT_LOAD memory before unmapping it. Caller supplies the guest
// address; admitted RIP-relative stores can generate commands in RW data.
// Range checks precede execution; PM4 interpretation follows CPU completion.
// Maximum 65536 words. On failure written=0 and words remain untouched. The
// copied stream has valid supported PM4 semantics, not necessarily Metal state.
int32_t maxps4_fex_engine_run_elf_pm4(maxps4_fex_engine *engine, const uint8_t *file,
    size_t length, uint32_t budget, uint64_t guest_address, size_t word_count,
    uint32_t *words, size_t capacity, size_t *written, uint64_t *result,
    uint32_t *backend, uint64_t *entry);
enum { MAXPS4_PS4_LINKER_REQUIRED = 1, MAXPS4_PS4_TLS_REQUIRED = 2,
       MAXPS4_PS4_CPU_ADDRESS_SPACE_REQUIRED = 4 };
// Stage a validated plaintext PS4 ELF/SELF in engine-owned, non-host-executable
// guest mappings. Does not initialize JIT, dispatch CPU or resolve imports.
// Guest addresses must be translated; rebasing is unsafe for arbitrary execution.
// On failure output fields are zero and any previous staging is discarded.
int32_t maxps4_fex_engine_stage_ps4(maxps4_fex_engine *engine, const uint8_t *file,
    size_t length, uint64_t *entry, uint32_t *segments, uint32_t *blockers);
// Real FEX execution of the already staged image, limited to the same finite
// instruction whitelist as run_elf. RIP-relative stores reach staged RW/non-X
// data in place. Requires initialized FEX; no fallback, linker or TLS support.
// Uses rebased CPU addresses internally, NOT a general PS4 virtual address
// translator. The CPU_ADDRESS_SPACE_REQUIRED blocker remains set. Returns the
// logical entry; on any failure outputs are zero. Stage/execute are serialized.
int32_t maxps4_fex_engine_run_staged_ps4(maxps4_fex_engine *engine,
    uint32_t budget, uint64_t *result, uint32_t *backend, uint64_t *entry);
// Link and admit a bounded Orbis ELF with standard dynamic tables, one
// libkernel dependency and imported GOT calls. No fallback, SCE tables, TLS,
// general PRX graph or arbitrary guest memory instructions. Clears old staging
// and services on each attempt. CPU_ADDRESS_SPACE_REQUIRED remains set.
int32_t maxps4_fex_engine_link_ps4(maxps4_fex_engine *engine, const uint8_t *file,
    size_t length, uint32_t budget, uint64_t *entry, uint32_t *imports);
// Execute that linked program through real FEX. The syscall trampoline captures
// SysV arguments, dispatches existing libkernel, updates RAX and resumes CPU.
// Only the program admitted at link time can execute; no interpreter fallback.
int32_t maxps4_fex_engine_run_linked_ps4(maxps4_fex_engine *engine,
    uint64_t *result, uint32_t *backend, uint64_t *entry, uint32_t *calls);
// SCE ELF/PRX graph -> exact NID/RELA linker -> owned pages -> finite CPU
// admission. Dependencies supplied explicitly; no disk discovery or decryption.
// Diagnostic describes the first refusal. Invalid preparation clears old state.
int32_t maxps4_fex_engine_prepare_orbis(maxps4_fex_engine *engine,
    const maxps4_orbis_module *modules, size_t count, const char *root,
    uint32_t budget, char *diagnostic, size_t capacity);
// Experimental general execution. Same graph, linked so that PS4 addresses are
// host addresses (ET_SCE_DYNEXEC/PRX only), with a guarded 1 MiB guest stack in
// the image and NO instruction admission: FEX runs the code unmodified, with
// branches, loops, stack and absolute pointers. *relocation is added to every
// requested load_bias (a multiple of 1 MiB). Then run with run_orbis.
// On ARM64 hosts a guest fault in generated code ends the run with -EFAULT
// or -EILSEQ (see last_guest_fault); faults inside FEX or HLE C++ code, and
// any fault under the VIXL simulator, still end the host process. Not for
// untrusted code: the app only runs its built-in guest thread images with it.
int32_t maxps4_fex_engine_prepare_orbis_general(maxps4_fex_engine *engine,
    const maxps4_orbis_module *modules, size_t count, const char *root,
    uint64_t *relocation, char *diagnostic, size_t capacity);
// Diagnostic boot probe for later prepare_orbis_general calls (0: off, the
// default). When on, every import that neither a given module nor the HLE
// provides gets a stub: a function writes "missing import <library> <NID>"
// to stderr on its first call and returns 0, a data object is 64 zero bytes.
// With 1, every HLE call of a run is also traced to stderr with its result;
// 2 keeps the stubs without that trace (thousands of lines per second).
// It shows how far a game gets; it never makes it correct.
int32_t maxps4_fex_engine_set_import_probe(maxps4_fex_engine *engine, uint32_t enabled);
// The loaded game's imports that neither its modules nor the HLE provide
// (stubbed, returning 0): one "<library> <NID> <calls>" line each, most
// called first, as many whole lines as fit. Returns how many there are.
int32_t maxps4_fex_engine_missing_imports(maxps4_fex_engine *engine, char *text, size_t capacity);
// The last PS4 calls of the run (at most 128, oldest first; lock set-up and use, time
// and thread-data calls left out): "t<thread> <name>(rdi, rsi, rdx, rcx) =
// <result>" with the path of an open or stat, as many whole lines as fit.
// Returns how many calls there are.
int32_t maxps4_fex_engine_recent_calls(maxps4_fex_engine *engine, char *text, size_t capacity);
// The run's last PS4 calls that returned an error, same format, oldest first.
int32_t maxps4_fex_engine_failed_calls(maxps4_fex_engine *engine, char *text, size_t capacity);
// The last 32 sceKernelOpen calls that succeeded, with their paths (same format).
int32_t maxps4_fex_engine_opened_files(maxps4_fex_engine *engine, char *text, size_t capacity);
// Fixed PS4 addresses the host refused, one "0x<hint> + 0x<size> -> 0x<result>
// (code <status>)" line each, oldest first; returns how many moves happened.
int32_t maxps4_fex_engine_moved_mappings(maxps4_fex_engine *engine, char *text, size_t capacity);
// The last bytes (at most 4096) the game wrote to its stdout and stderr, as
// a C string truncated at the front to fit. Returns their length.
int32_t maxps4_fex_engine_console(maxps4_fex_engine *engine, char *text, size_t capacity);
// Where game memory lives: the PS4 application area reserved at load (0
// when the host refused it, with the host's answer in app_area_status) and
// the loaded game's mmap window (0 if none).
// Address space iOS still granted right after the game's reservation, in
// 256 MiB steps (16 GiB at most).
uint64_t maxps4_fex_engine_spare_address_space(maxps4_fex_engine *engine);
int32_t maxps4_fex_engine_memory_layout(maxps4_fex_engine *engine, uint64_t *app_area,
    uint64_t *app_area_size, uint64_t *window, uint64_t *window_size, int32_t *app_area_status);
// A game directory as the boot probe loads it: <directory>/sce_module/*.prx
// (sorted) then <directory>/eboot.bin, each module's DT_NEEDED resolved among
// them, placed 1 MiB aligned and apart from 0x400000, linked with
// prepare_orbis_general, then <directory> mounted as /app0. Files are only
// read. Returns 0, a negative errno, or the failing call's status.
// A game update for the next load_game (NULL or "" for none): its eboot.bin
// and sce_module/*.prx replace the game's, and it overlays /app0.
int32_t maxps4_fex_engine_set_update(maxps4_fex_engine *engine, const char *directory);
int32_t maxps4_fex_engine_load_game(maxps4_fex_engine *engine, const char *directory,
    uint64_t *relocation, char *diagnostic, size_t capacity);
// Optional bounded RGBA8 output/input contract for an admitted SCE process.
// buttons: left=1, right=2, up=4, down=8, action=16. No host pointer returned.
// Invalid configuration clears previous output. Dimensions <=320x240; disjoint RW/non-X.
int32_t maxps4_fex_engine_configure_frame(maxps4_fex_engine *engine,
    uint64_t pixels, uint64_t input, uint32_t width, uint32_t height);
// One finite FEX dispatch to HLT, then exact copy of guest pixels. Output pixels
// unchanged on error; metadata zeroed. Caller serializes close against invocation.
int32_t maxps4_fex_engine_step_frame(maxps4_fex_engine *engine,
    uint32_t buttons, uint8_t *pixels, size_t capacity, uint64_t *sequence, uint32_t *backend);
// Experimental V4 direct-memory preflight. Requires explicitly mapped V4 pages;
// does not dispatch guest instructions or alter the V3 execution path.
// Returns an error until a genuine FEX-direct guest mapping is admitted.
int32_t maxps4_fex_engine_v4_preflight(maxps4_fex_engine *engine,
    uint64_t entry, uint64_t stack, size_t stack_bytes);
// Only an admitted SCE process may run. Real FEX only, with guarded private
// SysV stack, acyclic guest CALL/RET and terminal HLT. No implicit init/fini,
// syscall, TLS, arbitrary instructions, absolute pointer dereference or fallback.
int32_t maxps4_fex_engine_run_orbis(maxps4_fex_engine *engine,
    uint64_t *result, uint32_t *backend, uint64_t *entry);
// Exact read from staged or anonymous readable memory; no host pointers cross the ABI.
// Failure leaves caller bytes untouched. Serialized against stage/close.
int32_t maxps4_fex_engine_read_ps4(maxps4_fex_engine *engine, uint64_t address,
    uint8_t *output, size_t length);
// Fetch instructions only from staged readable AND executable guest memory.
// Invalid ranges leave output unchanged. No host pointers cross the C ABI.
int32_t maxps4_fex_engine_fetch_ps4(maxps4_fex_engine *engine, uint64_t address,
    uint8_t *output, size_t length);
// Exact write to staged or anonymous writable, non-executable guest memory.
// No host pointers cross the ABI. Rejected accesses leave guest bytes intact.
// Serialized against stage/close; does not dispatch CPU or initialize JIT.
int32_t maxps4_fex_engine_write_ps4(maxps4_fex_engine *engine, uint64_t address,
    const uint8_t *input, size_t length);
// Query whether a staged PS4 guest range is readable (4), writable (2)
// or executable (1). Requires nonzero valid permission bits and a nonempty
// fully mapped range. Writes 0 or 1 to permitted, never a host pointer.
// Does not dispatch guest instructions.
int32_t maxps4_fex_engine_query_ps4(maxps4_fex_engine *engine, uint64_t address,
    size_t length, uint32_t permissions, uint32_t *permitted);
// Query the maximum contiguous bytes at a staged guest address, capped by
// limit (1 MiB). Never exposes a host pointer or crosses a segment boundary.
// On invalid arguments, available is zero; an unmapped address returns zero
// with success. Serialized against staging and close.
int32_t maxps4_fex_engine_contiguous_ps4(maxps4_fex_engine *engine,
    uint64_t address, size_t limit, uint32_t permissions, size_t *available);
// Existing bounded interpreter fallback; never describes its output as FEX JIT.
// Caller keeps engine alive during calls and serializes close against them.
int32_t maxps4_fex_engine_run(maxps4_fex_engine *engine, const uint8_t *code,
                            size_t length, uint32_t budget, uint64_t *result,
                            uint32_t *backend);
#ifdef __cplusplus
}
#endif




