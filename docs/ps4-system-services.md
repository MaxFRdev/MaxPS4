# PS4 system services: implemented scope

The first libkernel service layer operates on checked logical PS4 addresses.
It can be exercised independently of FEX/JIT and never calls an x86 guest
function as an ARM64 host function. It does not make Kero Blaster playable.

## Filesystem

`PS4Libkernel::mount_app0` mounts a host directory read-only. `mount` also
supports writable `/savedata0`, `/download0` and `/temp0` directories. The C API
`maxps4_fex_engine_mount` exposes these mounts after staging an image; staging
resets all mounts. Guest descriptors 3–255 have independent host descriptors
and offsets. Writes to guest stdout/stderr (1–2) go to host stderr; these
descriptors do not alias host file descriptors for other operations.
`sceKernelOpen`, `sceKernelRead`, `sceKernelWrite`, `sceKernelClose`,
`sceKernelLseek`, `sceKernelStat` and `sceKernelFstat` work on regular files
and directories, with file I/O rejecting directories. Open supports RDONLY,
WRONLY, RDWR, APPEND, SYNC, CREAT, TRUNC, EXCL and DIRECTORY. Mutations on
`/app0` return EROFS; unsupported flags (including NONBLOCK) return ENOTSUP.
Transfers are bounded to 1 MiB per call.
Seeking supports SET/CUR/END with signed 64-bit offsets. Invalid guest buffers
are checked before read changes a file offset. Stat uses an explicit 120-byte
little-endian Orbis layout, not the host struct, and clears write mode bits
on the read-only mount, including for fstat.
Linux does not supply birthtime through stat, so that field remains zero there.

`sceKernelPread`/`sceKernelPwrite` preserve the descriptor offset;
`sceKernelFsync`, `sceKernelFtruncate` and `sceKernelTruncate` operate on host
files. `sceKernelMkdir`, `sceKernelRmdir`, `sceKernelRename` and
`sceKernelUnlink` operate within writable mounts. Rename across mounts returns
EXDEV; mount roots cannot be removed or renamed. Mounts must already exist on
the host; an unconfigured mount returns ENOENT.

`sceKernelGetdirentries` returns a snapshot of directory entries encoded as
FreeBSD dirents in complete 512-byte blocks (32-bit fileno, 16-bit record size,
8-bit type and name length, NUL-terminated name, 4-byte record alignment).
The optional base pointer receives the starting byte offset. Directory seeks
must be nonnegative and block aligned; seeking to zero rereads the snapshot.

Paths must be absolute under a configured mount; its root accepts either
`/savedata0` or `/savedata0/`, for example.
Traversal, duplicate separators and ambiguous components are rejected.
Directory-relative `openat` walks reject symlinks at each component; special
files (including FIFOs) are unsupported. Remount/reset closes descriptors;
a failed mount leaves the old mount intact. The host must treat mounted content
as stable while the guest runs. Image mutation and dispatch must be serialized
by the owner. PS4 errors use Orbis errno numbers and signed `0x80020000 | errno`.

## Imports and ABI

`resolve_libkernel` recognizes the implemented sceKernel names and NIDs only
for library/module `libkernel`, version 1. ABI metadata was checked against
[shadPS4 file_system.h](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/file_system.h)
and its registration table. POSIX variants such as `read` and `write` use
`-1` with the calling thread's errno cell, as recorded by their export metadata.
An ELF/PRX loader must decode
library/module IDs before using the resolver. Encoded `nid#library#module`
strings are not guessed. The result is a service ID, never a host code pointer.
`invoke` consumes RDI/RSI/RDX/RCX/R8/R9 register values. Guest results are returned
as signed 64-bit values for the CPU adapter to place in RAX.

This begins import resolution; it does not parse dynamic tables, apply
relocations or install guest trampolines. Existing linker/TLS/address-space
execution blockers remain in force, and the restricted FEX syscall handler
remains ENOSYS.

## Anonymous virtual memory and clocks

`PS4VirtualMemory` supplies zero-filled logical anonymous PRIVATE mappings in
16 KiB PS4 pages with a 32 MiB backing budget and at most 128 allocations.
`sceKernelMmap` supports only flags `0x1002`, a zero address hint, fd=-1 and
offset=0. Its seventh output-pointer argument is read at guest entry RSP+8.
PROT_READ/WRITE/EXEC are translated to ELF permissions; writable/executable
mappings are rejected. Guest RX is host data, never an iOS JIT allocation.
`sceKernelMprotect` changes permissions per page; `sceKernelMunmap` removes
whole or partial page intervals. Both currently require the entire interval
to lie in one live anonymous allocation; holes and ELF segments return ENOMEM.
Partially unmapped pages become inaccessible immediately, but the backing
budget is released only when the allocation's last page is unmapped. File
and shared mappings are unsupported. The bounded image path rejects fixed
mappings; general images support them inside the image's owned host window.
The extended direct-memory layer is described below.
Allocations avoid staged ELF segments and other anonymous allocations.
The optional `PS4DynamicMemory` provider preserves existing ELF-only clients.
The owner must reset dynamic memory on restaging and serialize all accesses.

The extended kernel layer also exposes direct-memory allocation and mapping.
`sceKernelReserveVirtualRange` reserves inaccessible pages;
its alignment must be zero or a power of two from 16 KiB to 2 GiB.
`sceKernelMtypeprotect` changes their CPU permissions, translating GPU access
bits for the emulated GPU. Memory types are not retained.
`sceKernelDirectMemoryQuery` returns the allocation containing an offset, or
the next allocation when flag 1 is set, in a 24-byte guest structure.
`sceKernelMapNamedDirectMemory` maps direct memory without retaining its name;
`sceKernelSetVirtualRangeName` accepts labels without storing them.
`sceKernelBatchMap` and `sceKernelBatchMap2` process 32-byte entries in order
and report the number completed before the first error. BatchMap uses fixed
addresses; BatchMap2 takes mapping flags from its fourth argument. Fixed
mapping requires a general image's owned host window.
Fixed mappings with a null address are rejected. Direct mapping validates
the entire physical interval without allowing address arithmetic to wrap.
The new service IDs are appended to preserve existing trampoline tokens.

Argument layouts and reservation alignment checks follow the
[shadPS4 kernel memory implementation](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp).

GNM debugger resource registration returns the unsupported status
`0x8eee00ff`. Garlic flush and the added status queries return zero.
`sceGnmSetGsRingSizes` validates each size as a 1 MiB multiple from 4 to 8 MiB;
it does not allocate GPU rings.

`sceKernelClockGettime` and `sceKernelClockGetres` support PS4 REALTIME=0 and
MONOTONIC=4, using the corresponding host clocks. `sceKernelGettimeofday`
returns Unix seconds/microseconds; `sceKernelGetProcessTime` returns elapsed
monotonic microseconds since service creation/reset (elapsed time, not CPU time).
Unsupported clock IDs return EINVAL. No fabricated network/TSC clock is exposed.

## Thread and synchronization preparation

`PS4Threads` registers up to 64 guest identities; identity 1 is the bootstrap
thread and the FEX guest threads of general images attach/detach the others
(see ORBIS-LINKER.md). `scePthreadSelf` returns the calling thread's
identity. Up to 256 default private/error-check mutexes support
`scePthreadMutexInit`, `scePthreadMutexLock` (blocking; the owner gets
EDEADLK), `scePthreadMutexTrylock`, `scePthreadMutexUnlock` and
`scePthreadMutexDestroy`. Up to 256 condition variables support
`scePthreadCondInit`, `scePthreadCondWait`, `scePthreadCondTimedwait`
(relative timeout in microseconds; ETIMEDOUT still retakes the mutex),
`scePthreadCondSignal`, `scePthreadCondBroadcast` and `scePthreadCondDestroy`
(EBUSY while a thread waits). A signal with no waiter is lost, as in POSIX.
Guest slots contain opaque 64-bit handles, never host pointers. A slot holding
0 (or 1, the adaptive mutex initializer) is a static initializer: the object
is created on first use. Destruction leaves 2, which stays invalid until the
next init. Ownership is checked, locked mutex destruction/detach returns
EBUSY, and stale handles are invalid after reset. Named objects validate the
guest string. Mutex attribute objects (`scePthreadMutexattrInit/Destroy/
Settype/Gettype`, the protocol accepted without effect) give the type:
error-checking (the default; its owner relocking gets EDEADLK), recursive
(counted; a condition wait releases and restores every count), normal (its
owner relocking blocks until the run stops) or adaptive (as error-checking).
Thread attribute objects (`scePthreadAttrInit/Destroy`, detach state, stack
size of at least 16 KiB; scheduling, inheritance, affinity and guard size are
accepted without effect) are read by `scePthreadCreate`. Initializing an
attribute slot again replaces its object. Condition attributes (clock,
process sharing) are ignored. ScePthread errors use Orbis kernel codes,
as their upstream Orbis wrappers do, rather than the POSIX pthread
positive-errno convention.

Blocking calls go through `PS4Threads` only, never under the libkernel lock,
so a blocked thread never holds up the one that would release it. When a run
stops (time limit, fault, exit function, end of the entry), every blocked lock
and wait returns EINTR so the thread goes back to guest code, where the stop
signal lands. Condition waits are general-image services: a bounded image has
no other thread to end them. Semaphores, scheduling, priorities and
cancellation remain unresolved. Host-worker tests
(`PS4ThreadsTest`, `PS4SyncTest`) verify mutual exclusion, blocking, signal,
broadcast, timeouts and interruption.

## What a game's libc needs

A game ships its own `libc.prx`, loaded like any PRX; it calls into libkernel
for about a hundred functions. Bindings now carry their library, and three
kinds besides plain trampolines:

- **POSIX convention** (`posix`): the trampoline token has `posix_token`; an
  Orbis error result becomes -1 with the error in the calling thread's errno
  cell. `__Error()` returns that cell's address: the cells are the engine's
  `MaxPS4errno` data object (8 bytes per thread index).
- **Data objects** (`data`): `__stack_chk_guard` (random), `__progname`
  (points at `"eboot.bin"`) and the errno cells, linked in `<hle-data>` and
  filled by `PS4Libkernel::initialize_data` after preparation.
- **Guest routines** (`code`): `scePthreadOnce` must call its init routine,
  which is guest code, so it is x86 in `<hle-code>`: the first caller moves
  the control word 0 → 2, calls init and stores 1; others wait for 1.

Implemented for libc: `exit`, `__stack_chk_fail` and
`sceKernelDebugRaiseException*` (end the process; the last two with status
134), `nanosleep` (both NIDs), `sceKernelUsleep`, POSIX `clock_gettime` and
`gettimeofday`, `getpagesize` (16 KiB), `sched_yield`/`pthread_yield`,
`pthread_equal`, thread-specific keys (256, a value per thread; destructors
recorded, not run), read/write locks (static initializer, interruptible
waits), priority 700 and affinity 0x7f, `pthread_attr_get_np`, `sigfillset`,
`signal` (SIG_DFL: no signal is ever delivered), UTC time-zone conversions,
`sceKernelGetProcParam` (root module's `PT_SCE_PROCPARAM`). Module-information
queries answer ESRCH (no C++ exception unwinding yet). Masks, heap API,
backtrace printing and a few internal hooks answer 0.

## Orbis user event queues

`sceKernelCreateEqueue`, `sceKernelDeleteEqueue` and `sceKernelWaitEqueue`
now have an implementation, together with `sceKernelAddUserEvent`,
`sceKernelAddUserEventEdge`, `sceKernelTriggerUserEvent`,
`sceKernelDeleteUserEvent` and the six event-field getters. ABI and NIDs
follow [shadPS4's kernel event queues](https://github.com/shadps4-emu/shadPS4/blob/91e6afa69b0a64b0afaa0281b80f8fbb0cd88136/src/core/libraries/kernel/equeue.cpp).
An event is serialized as 32 little-endian bytes (ident, signed filter,
flags, fflags, data, opaque udata). Guest pointers are checked and udata is
never dereferenced.

A null timeout waits indefinitely; a pointer to zero microseconds polls;
a positive 32-bit microsecond timeout waits up to that duration. No event
returns ETIMEDOUT with out_count zero. Level user events remain ready;
edge user events clear only after successful delivery. Repeated triggers
coalesce and keep the latest udata. Removing a queue wakes its waiters with
EBADF; engine interruption wakes them with EINTR. Handles stay stale after
deletion/reset, including when the underlying queue slot is reused. Waits
run outside the libkernel lock, using the same interruption mechanism as
guest mutexes and conditions.

Limits: 32 queues per engine, 64 registered user events per queue, and 1–64
events per wait (larger requests return EINVAL). Waits are general-image
services, since they can block until another guest thread produces an
event. Timer, file, GPU and VideoOut filters and POSIX kqueue/kevent are not
implemented through this user-event interface. GNM completion subscriptions
are described below. This does not produce a game's display or validate Sonic Mania.

`PS4EventQueueTest` covers wire layout, level/edge delivery, polling,
timeouts, wakeup, interruption, deletion/reuse, stale handles, limits and
invalid buffers. The linked FEX suite executes a synthetic x86 guest that
creates a queue, triggers/consumes an edge, observes the next poll timing
out and deletes the queue, then exits with its returned user data (123).

## GNM completion-event subscriptions

`sceGnmAddEqEvent`, `sceGnmDeleteEqEvent` and `sceGnmGetEqEventType` are
registered under **libSceGnmDriver**, using the ABI/NIDs and event IDs from
[shadPS4's GNM driver](https://github.com/shadps4-emu/shadPS4/blob/91e6afa69b0a64b0afaa0281b80f8fbb0cd88136/src/core/libraries/gnmdriver/gnmdriver.cpp).
Supported IDs are compute release-memory 0–6 and graphics EOP 0x40. Other
IDs return EINVAL. Deleting an absent supported subscription from a valid
queue succeeds, matching shadPS4's GNM wrapper. The event's filter is GraphicsCore (-14), data holds the
event type and udata retains the caller's opaque value. Events clear after
successful delivery. Multiple completions before a wait coalesce. User and
GPU events with the same ident coexist as separate (ident, filter) keys;
both share the existing per-queue capacity.

The backend API `maxps4_fex_engine_graphics_event_ticket` captures an
engine-scoped **registration ticket**; `maxps4_fex_engine_complete_graphics_event`
delivers a notification for that subscription. Registration/capture alone
never signals completion. A backend must capture before scheduling work and
call completion only after successful execution. Deletion, re-registration,
queue deletion and restaging invalidate captured tickets, so a late callback
cannot signal a replacement subscription. This is a subscription ticket, not
a GPU fence or a per-submission completion counter.

These two host APIs use the event-table mutex rather than the global runtime
lock, allowing a backend thread to wake a guest blocked in `WaitEqueue`.
They do not access guest memory. The caller must keep the engine alive until
every callback has finished and serialize engine close with those callbacks.
They return Orbis kernel errors; capture clears the output ticket on failure.

`PS4GraphicsEventTest` checks wire fields, all supported IDs, invalid IDs,
coexisting filters, delayed callbacks, cross-thread wakeup, queue isolation
and reset. The FEX suite runs a synthetic guest waiting indefinitely while
a host test thread explicitly completes a subscription; the guest resolves
the real GNM event-type getter and exits with 64. This verifies notification
plumbing, **not a GPU workload**. No production GPU completion source, GNM
command submission, VideoOut port/flip/vblank service or Metal presentation
is connected by this change.

The next bounded output path is documented in [Linear VideoOut handoff](videoout-linear.md):
checked linear buffers, owned pixel snapshots, and explicit host flip/vblank
notifications. It does not connect a production renderer or display timing source.

The local profile lifecycle is documented in [Local UserService](ps4-user-service.md):
per-engine initialization, user/list/name queries and a consumed-once login event.
It provides a virtual profile without online or host-account identity.

## Engine integration

The engine owns a virtual-memory provider, checked memory adapter and libkernel
instance referencing its existing staged ELF. The C/Swift ABI exposes
`maxps4_ps4_resolve_libkernel`, `maxps4_fex_engine_mount_app0` and
`maxps4_fex_engine_call_libkernel`. Stage first, mount second, resolve a decoded
import, then dispatch a PS4 register snapshot with bootstrap guest_thread=1.
The return is the guest ABI result. IDs from the resolver are opaque and valid
only for this runtime's registry; they are not guest code addresses. Staging
attempts reset descriptors, mount, anonymous mappings and mutex/thread state,
including failed staging. Existing engine read/write/fetch/query calls also
access anonymous mappings. All engine operations use the existing runtime lock.
The Darwin/iOS CMake target includes the service implementations. This enables
explicit host-driven service calls; a guest CPU-to-HLE trampoline and dynamic
ELF/PRX relocation processing are still required to call them from a real game.

The full FEX runtime integration test now checks these APIs before InitCore:
file reads reach the existing staged image, mmap addresses are usable by engine
read/write, and failed restaging discards services. The local Linux environment
has no Apple SDK or vendored FEX dependencies, so the complete FEX/iPhone ARM64
build and this integration test must run in the existing macOS linkage CI.

## Validation

Run `bash scripts/test-ps4-system-services.sh` with C++17. The Ubuntu/macOS CI
matrix enables ASan/UBSan and treats compiler warnings as errors. Tests use
synthetic ELF/data fixtures only. A Linux ptrace environment may need
`ASAN_OPTIONS=detect_leaks=0` because LeakSanitizer cannot operate under ptrace;
address and undefined-behavior instrumentation remain enabled.
