#include "maxps4_ps4_libkernel.h"
#include "maxps4_fex_runtime.h"
#include "maxps4_ps4_kernel_path.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <random>
#include <chrono>
#include <thread>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <cstdlib>
#include <unistd.h>
#include <vector>

namespace maxps4::ps4 {
Error host_error(int e) {
    switch (e) {
    case 0: return ok;
    case EPERM: return perm;
    case ENOENT: return noent;
    case EINTR: return intr;
    case EIO: return io;
    case EBADF: return badf;
    case ENOMEM: return nomem;
    case EACCES: return access;
    case EFAULT: return fault;
    case EBUSY: return busy;
    case EEXIST: return exist;
    case EXDEV: return xdev;
    case ENOSPC: return nospc;
    case ENOTEMPTY: return notempty;
    case ENOTDIR: return notdir;
    case EISDIR: return isdir;
    case EINVAL: return inval;
    case EMFILE: case ENFILE: return mfile;
    case EROFS: return rofs;
    case EAGAIN: return again;
    case ELOOP: return loop;
    case ENAMETOOLONG: return nametoolong;
    case ENOSYS: return nosys;
    case EOVERFLOW: return overflow;
    case ENOTSUP: return notsup;
    default: return io; // Never leak a host errno with a different Orbis meaning.
    }
}
}
namespace maxps4 {
using namespace ps4;
namespace {
using Binding = PS4KernelExport;
// scePthreadOnce(once, init) as guest code, since init is guest code. The
// control word (FreeBSD): 0 never done, 1 done, 2 in progress. The first
// caller moves it 0 -> 2, calls init and stores 1; others wait for 1.
// Returns 0. Called with RSP = 8 (mod 16), so PUSH RDI aligns the CALL.
constexpr uint8_t once_code[] = {
    0x8b,0x07, 0x83,0xf8,0x01, 0x74,0x21,      // 00 MOV EAX,[RDI]; CMP EAX,1; JE done
    0x31,0xc0, 0xb9,0x02,0x00,0x00,0x00,       // 07 XOR EAX,EAX; MOV ECX,2
    0xf0,0x0f,0xb1,0x0f, 0x75,0x0d,            // 0e LOCK CMPXCHG [RDI],ECX; JNZ wait
    0x57, 0xff,0xd6, 0x5f,                     // 14 PUSH RDI; CALL RSI; POP RDI
    0xc7,0x07,0x01,0x00,0x00,0x00,             // 18 MOV DWORD [RDI],1
    0x31,0xc0, 0xc3,                           // 1e XOR EAX,EAX; RET
    0xf3,0x90, 0x83,0x3f,0x01, 0x75,0xf9,      // 21 wait: PAUSE; CMP DWORD [RDI],1; JNE wait
    0x31,0xc0, 0xc3};                          // 28 done: XOR EAX,EAX; RET
static_assert(sizeof(once_code) == 0x2b, "pthread_once routine layout");
// libSceLibcInternal's internal_* functions, which the PRX modules games ship
// (Fios2, NpToolkit2...) import: SysV x86-64 routines, string and memory ones
// with REP MOVSB/STOSB, float ones through x87 (FSIN, FPATAN, F2XM1...).
// Assembled from MaxPS4-iOS/Tools/libc-internal-routines.s and checked
// natively against the host libc and libm.
constexpr uint8_t internal_memcpy_code[] = {0x48,0x89,0xf8,0x48,0x89,0xd1,0xf3,0xa4,0xc3};
constexpr uint8_t internal_memmove_code[] = {0x48,0x89,0xf8,0x48,0x89,0xd1,0x48,0x39,0xf7,0x76,0x18,0x4c,0x8d,0x04,0x16,0x4c,0x39,0xc7,0x73,0x0f,0x48,0x8d,0x74,0x16,0xff,0x48,0x8d,0x7c,0x17,0xff,0xfd,0xf3,0xa4,0xfc,0xc3,0xf3,0xa4,0xc3};
constexpr uint8_t internal_memset_code[] = {0x49,0x89,0xf9,0x89,0xf0,0x48,0x89,0xd1,0xf3,0xaa,0x4c,0x89,0xc8,0xc3};
constexpr uint8_t internal_memcmp_code[] = {0x31,0xc0,0x48,0x85,0xd2,0x74,0x15,0x0f,0xb6,0x07,0x0f,0xb6,0x0e,0x29,0xc8,0x75,0x0b,0x48,0xff,0xc7,0x48,0xff,0xc6,0x48,0xff,0xca,0xeb,0xe6,0xc3};
constexpr uint8_t internal_strlen_code[] = {0x48,0x89,0xf8,0x80,0x38,0x00,0x74,0x05,0x48,0xff,0xc0,0xeb,0xf6,0x48,0x29,0xf8,0xc3};
constexpr uint8_t internal_strcmp_code[] = {0x0f,0xb6,0x07,0x0f,0xb6,0x0e,0x29,0xc8,0x75,0x0c,0x85,0xc9,0x74,0x08,0x48,0xff,0xc7,0x48,0xff,0xc6,0xeb,0xea,0xc3};
constexpr uint8_t internal_strncmp_code[] = {0x31,0xc0,0x48,0x85,0xd2,0x74,0x19,0x0f,0xb6,0x07,0x0f,0xb6,0x0e,0x29,0xc8,0x75,0x0f,0x85,0xc9,0x74,0x0b,0x48,0xff,0xc7,0x48,0xff,0xc6,0x48,0xff,0xca,0xeb,0xe2,0xc3};
constexpr uint8_t internal_strncpy_code[] = {0x48,0x89,0xf8,0x48,0x85,0xd2,0x74,0x24,0x0f,0xb6,0x0e,0x88,0x0f,0x48,0xff,0xc7,0x48,0xff,0xca,0x85,0xc9,0x74,0x05,0x48,0xff,0xc6,0xeb,0xe7,0x48,0x85,0xd2,0x74,0x0b,0xc6,0x07,0x00,0x48,0xff,0xc7,0x48,0xff,0xca,0xeb,0xf0,0xc3};
constexpr uint8_t internal_memcpy_s_code[] = {0x48,0x85,0xff,0x74,0x2c,0x48,0x85,0xd2,0x74,0x20,0x48,0x39,0xf1,0x77,0x0e,0x48,0x87,0xd6,0x48,0x89,0xc8,0x48,0x89,0xc1,0xf3,0xa4,0x31,0xc0,0xc3,0x48,0x89,0xf1,0x31,0xc0,0xf3,0xaa,0xb8,0x22,0x00,0x00,0x00,0xc3,0x48,0x89,0xf1,0x31,0xc0,0xf3,0xaa,0xb8,0x16,0x00,0x00,0x00,0xc3};
constexpr uint8_t internal_strncpy_s_code[] = {0x48,0x85,0xff,0x74,0x42,0x48,0x85,0xf6,0x74,0x3d,0x48,0x85,0xd2,0x74,0x35,0x49,0x89,0xf8,0x48,0x85,0xc9,0x74,0x1d,0x0f,0xb6,0x02,0x85,0xc0,0x74,0x16,0x48,0x83,0xfe,0x01,0x76,0x16,0x88,0x07,0x48,0xff,0xc7,0x48,0xff,0xc2,0x48,0xff,0xce,0x48,0xff,0xc9,0xeb,0xde,0xc6,0x07,0x00,0x31,0xc0,0xc3,0x41,0xc6,0x00,0x00,0xb8,0x22,0x00,0x00,0x00,0xc3,0xc6,0x07,0x00,0xb8,0x16,0x00,0x00,0x00,0xc3};
constexpr uint8_t internal_sinf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xd9,0xfe,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_cosf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xd9,0xff,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_tanf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xd9,0xf2,0xdd,0xd8,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_atanf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xd9,0xe8,0xd9,0xf3,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_atan2f_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xf3,0x0f,0x11,0x0c,0x24,0xd9,0x04,0x24,0xd9,0xf3,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_expf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xd9,0xea,0xde,0xc9,0xd9,0xc0,0xd9,0xfc,0xdc,0xe9,0xd9,0xc9,0xd9,0xf0,0xd9,0xe8,0xde,0xc1,0xd9,0xfd,0xdd,0xd9,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_logf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0xed,0xd9,0x04,0x24,0xd9,0xf1,0xd9,0x1c,0x24,0xf3,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_sincosf_code[] = {0x48,0x83,0xec,0x08,0xf3,0x0f,0x11,0x04,0x24,0xd9,0x04,0x24,0xd9,0xfb,0xd9,0x1e,0xd9,0x1f,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_atan_code[] = {0x48,0x83,0xec,0x08,0xf2,0x0f,0x11,0x04,0x24,0xdd,0x04,0x24,0xd9,0xe8,0xd9,0xf3,0xdd,0x1c,0x24,0xf2,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_cos_code[] = {0x48,0x83,0xec,0x08,0xf2,0x0f,0x11,0x04,0x24,0xdd,0x04,0x24,0xd9,0xff,0xdd,0x1c,0x24,0xf2,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};
constexpr uint8_t internal_tan_code[] = {0x48,0x83,0xec,0x08,0xf2,0x0f,0x11,0x04,0x24,0xdd,0x04,0x24,0xd9,0xf2,0xdd,0xd8,0xdd,0x1c,0x24,0xf2,0x0f,0x10,0x04,0x24,0x48,0x83,0xc4,0x08,0xc3};

// ABI metadata checked against shadPS4 kernel/file_system.{h,cpp}; original
// implementation below. POSIX variants carry their errno/TLS convention.
constexpr Binding bindings[] = {
    {"sceKernelOpen", "1G3lF1Gg1k8", PS4KernelService::open},
    {"sceKernelRead", "Cg4srZ6TKbU", PS4KernelService::read},
    {"sceKernelClose", "UK2Tl2DWUns", PS4KernelService::close},
    {"sceKernelLseek", "oib76F-12fk", PS4KernelService::lseek},
    {"sceKernelStat", "eV9wAD2riIA", PS4KernelService::stat},
    {"sceKernelFstat", "kBwCPsYX-m4", PS4KernelService::fstat},
    {"sceKernelMmap", "PGhQHd-dzv8", PS4KernelService::mmap},
    {"sceKernelMunmap", "cQke9UuBQOk", PS4KernelService::munmap},
    {"sceKernelMprotect", "vSMAm3cxYTY", PS4KernelService::mprotect},
    {"sceKernelClockGettime", "QBi7HCK03hw", PS4KernelService::clock_gettime},
    {"sceKernelClockGetres", "wRYVA5Zolso", PS4KernelService::clock_getres},
    {"sceKernelGettimeofday", "ejekcaNQNq0", PS4KernelService::gettimeofday},
    {"sceKernelGetProcessTime", "4J2sUJmuHZQ", PS4KernelService::process_time},
    // One nanosecond counter stands for both the process time counter and the TSC.
    {"sceKernelGetProcessTimeCounter", "fgxnMeTNUtY", PS4KernelService::process_counter},
    {"sceKernelGetProcessTimeCounterFrequency", "BNowx2l588E", PS4KernelService::counter_frequency},
    {"sceKernelReadTsc", "-2IRUCO--PM", PS4KernelService::process_counter},
    {"sceKernelGetTscFrequency", "1j3S3n-tTW4", PS4KernelService::counter_frequency},
    {"sceKernelSleep", "-ZR+hG7aDHw", PS4KernelService::kernel_sleep},
    {"sceKernelNanosleep", "QvsZxomvUHs", PS4KernelService::nanosleep},
    {"sceKernelGetCpumode", "VOx8NGmHXTs", PS4KernelService::succeed},
    {"sceKernelGetCurrentCpu", "g0VTBxfJyu0", PS4KernelService::succeed},
    // Semaphores and event flags (shadPS4 kernel/threads/semaphore.cpp, event_flag.cpp).
    {"sceKernelCreateSema", "188x57JYp0g", PS4KernelService::sema_create},
    {"sceKernelDeleteSema", "R1Jvn8bSCW8", PS4KernelService::sema_delete},
    {"sceKernelWaitSema", "Zxa0VhQVTsk", PS4KernelService::sema_wait},
    {"sceKernelPollSema", "12wOHk8ywb0", PS4KernelService::sema_poll},
    {"sceKernelSignalSema", "4czppHBiriw", PS4KernelService::sema_signal},
    {"sceKernelCancelSema", "4DM06U2BNEY", PS4KernelService::sema_cancel},
    {"sceKernelCreateEventFlag", "BpFoboUJoZU", PS4KernelService::flag_create},
    {"sceKernelDeleteEventFlag", "8mql9OcQnd4", PS4KernelService::flag_delete},
    {"sceKernelSetEventFlag", "IOnSvHzqu6A", PS4KernelService::flag_set},
    {"sceKernelClearEventFlag", "7uhBFWRAS60", PS4KernelService::flag_clear},
    {"sceKernelWaitEventFlag", "JTvBflhYazQ", PS4KernelService::flag_wait},
    {"sceKernelPollEventFlag", "9lvj5DjHZiA", PS4KernelService::flag_poll},
    {"sceKernelCancelEventFlag", "PZku4ZrXJqg", PS4KernelService::flag_cancel},
    {"scePthreadSelf", "aI+OeCz8xrQ", PS4KernelService::thread_self},
    {"scePthreadMutexInit", "cmo1RIYva9o", PS4KernelService::mutex_init},
    {"scePthreadMutexTrylock", "upoVrzMHFeE", PS4KernelService::mutex_trylock},
    {"scePthreadMutexUnlock", "tn3VlD0hG60", PS4KernelService::mutex_unlock},
    {"scePthreadMutexDestroy", "2Of0f+3mhhE", PS4KernelService::mutex_destroy},
    // NIDs from shadPS4 kernel/threads/mutex.cpp and condvar.cpp (Orbis variants).
    {"scePthreadMutexLock", "9UK1vLZQft4", PS4KernelService::mutex_lock},
    {"scePthreadCondInit", "2Tb92quprl0", PS4KernelService::cond_init},
    {"scePthreadCondDestroy", "g+PZd2hiacg", PS4KernelService::cond_destroy},
    {"scePthreadCondWait", "WKAXJ4XBPQ4", PS4KernelService::cond_wait},
    {"scePthreadCondTimedwait", "BmMjYxmew1w", PS4KernelService::cond_timedwait},
    {"scePthreadCondSignal", "kDh-NfxgMtE", PS4KernelService::cond_signal},
    {"scePthreadCondBroadcast", "JGgj7Uvrl+A", PS4KernelService::cond_broadcast},
    // NIDs from shadPS4 kernel/threads/pthread.cpp, pthread_attr.cpp and
    // mutex.cpp (Orbis variants). Scheduling, affinity, guard size and the
    // mutex protocol are accepted without effect.
    {"scePthreadDetach", "4qGrR6eoP9Y", PS4KernelService::thread_detach},
    {"scePthreadAttrInit", "nsYoNRywwNg", PS4KernelService::attr_init},
    {"scePthreadAttrDestroy", "62KCwEMmzcM", PS4KernelService::attr_destroy},
    {"scePthreadAttrSetdetachstate", "-Wreprtu0Qs", PS4KernelService::attr_set_detach},
    {"scePthreadAttrGetdetachstate", "JaRMy+QcpeU", PS4KernelService::attr_get_detach},
    {"scePthreadAttrSetstacksize", "UTXzJbWhhTE", PS4KernelService::attr_set_stack},
    {"scePthreadAttrGetstacksize", "-fA+7ZlGDQs", PS4KernelService::attr_get_stack},
    {"scePthreadAttrSetschedparam", "DzES9hQF4f4", PS4KernelService::attr_accept},
    {"scePthreadAttrSetinheritsched", "eXbUSpEaTsA", PS4KernelService::attr_accept},
    {"scePthreadAttrSetschedpolicy", "4+h9EzwKF4I", PS4KernelService::attr_accept},
    {"scePthreadAttrSetaffinity", "3qxgM4ezETA", PS4KernelService::attr_accept},
    {"scePthreadAttrSetguardsize", "El+cQ20DynU", PS4KernelService::attr_accept},
    {"scePthreadMutexattrInit", "F8bUHwAG284", PS4KernelService::mutexattr_init},
    {"scePthreadMutexattrInitForInternalLibc", "n2MMpvU8igI", PS4KernelService::mutexattr_init},
    {"scePthreadMutexattrDestroy", "smWEktiyyG0", PS4KernelService::mutexattr_destroy},
    {"scePthreadMutexattrSettype", "iMp8QpE+XO4", PS4KernelService::mutexattr_settype},
    {"scePthreadMutexattrGettype", "gquEhBrS2iw", PS4KernelService::mutexattr_gettype},
    {"scePthreadMutexattrSetprotocol", "1FGvU0i9saQ", PS4KernelService::mutexattr_accept},
    // NIDs from shadPS4 kernel/threads/pthread.cpp (Orbis variants).
    {"scePthreadCreate", "6UgtwV+0zb4", PS4KernelService::thread_create},
    {"scePthreadJoin", "onNY9Byn-W8", PS4KernelService::thread_join},
    {"scePthreadExit", "3kg7rT0NQIs", PS4KernelService::thread_exit},
    // NID from shadPS4 kernel/threads/tcb.cpp (RegisterRtld).
    {"__tls_get_addr", "vNe1w4diLCs", PS4KernelService::tls_get_addr},
    // What a game's libc.prx imports. NIDs from shadPS4 kernel/*.cpp and
    // threads/*.cpp; the last few, absent there, from the NID of their name.
    // POSIX-convention functions set the thread's errno and return -1.
    {"__Error", "9BcDykPmo1I", PS4KernelService::errno_location, "libkernel", true},
    {"exit", "6Z83sYWFlA8", PS4KernelService::process_exit, "libkernel", true, true},
    {"__stack_chk_fail", "Ou3iL1abvng", PS4KernelService::abort_process, "libkernel", true, true},
    {"sceKernelDebugRaiseException", "OMDRKKAZ8I4", PS4KernelService::abort_process, "libkernel", false, true},
    {"sceKernelDebugRaiseExceptionOnReleaseMode", "zE-wXIZjLoM", PS4KernelService::abort_process, "libkernel", false, true},
    {"nanosleep", "NhpspxdjEKU", PS4KernelService::nanosleep, "libkernel", true},
    {"_nanosleep", "yS8U2TGCe1A", PS4KernelService::nanosleep, "libkernel", true},
    {"sceKernelUsleep", "1jfXLRVzisc", PS4KernelService::usleep},
    {"clock_gettime", "lLMT9vJAck0", PS4KernelService::clock_gettime, "libkernel", true},
    {"gettimeofday", "n88vx3C5nW8", PS4KernelService::gettimeofday, "libkernel", true},
    {"getpagesize", "k+AXqu2-eBc", PS4KernelService::page_size, "libkernel", true},
    // POSIX pthread_* over the same services (pthread_token: errno results).
    {"pthread_mutex_init", "ttHNfU+qDBU", PS4KernelService::mutex_init},
    {"pthread_mutex_destroy", "ltCfaGr2JGE", PS4KernelService::mutex_destroy},
    {"pthread_mutex_lock", "7H0iTOciTLo", PS4KernelService::mutex_lock},
    {"pthread_mutex_unlock", "2Z+PpY6CaJg", PS4KernelService::mutex_unlock},
    {"pthread_mutex_trylock", "K-jXhbt2gn4", PS4KernelService::mutex_trylock},
    {"pthread_mutexattr_init", "dQHWEsJtoE4", PS4KernelService::mutexattr_init},
    {"pthread_mutexattr_destroy", "HF7lK46xzjY", PS4KernelService::mutexattr_destroy},
    {"pthread_mutexattr_settype", "mDmgMOGVUqg", PS4KernelService::mutexattr_settype},
    {"pthread_mutexattr_gettype", "GZFlI7RhuQo", PS4KernelService::mutexattr_gettype},
    {"pthread_mutexattr_setprotocol", "5txKfcMUAok", PS4KernelService::mutexattr_accept},
    {"pthread_cond_init", "0TyVk4MSLt0", PS4KernelService::cond_init},
    {"pthread_cond_destroy", "RXXqi4CtF8w", PS4KernelService::cond_destroy},
    {"pthread_cond_wait", "Op8TBGY5KHg", PS4KernelService::cond_wait},
    {"pthread_cond_timedwait", "27bAgiJmOh0", PS4KernelService::cond_timedwait},
    {"pthread_cond_signal", "2MOy+rUfuhQ", PS4KernelService::cond_signal},
    {"pthread_cond_broadcast", "mkx2fVhNMsg", PS4KernelService::cond_broadcast},
    {"pthread_condattr_init", "mKoTx03HRWA", PS4KernelService::succeed},
    {"pthread_condattr_destroy", "dJcuQVn6-Iw", PS4KernelService::succeed},
    {"pthread_create", "OxhIB8LB-PQ", PS4KernelService::thread_create},
    {"pthread_join", "h9CcP3J0oVM", PS4KernelService::thread_join},
    {"pthread_detach", "+U1R4WtXvoc", PS4KernelService::thread_detach},
    {"pthread_exit", "FJrT5LuUBAU", PS4KernelService::thread_exit},
    {"pthread_self", "EotR8a3ASf4", PS4KernelService::thread_self},
    {"pthread_key_create", "mqULNdimTn0", PS4KernelService::key_create},
    {"pthread_key_delete", "6BpEZuDT7YI", PS4KernelService::key_delete},
    {"pthread_setspecific", "WrOLvHU0yQM", PS4KernelService::set_specific},
    {"pthread_attr_init", "wtkt-teR1so", PS4KernelService::attr_init},
    {"pthread_attr_destroy", "zHchY8ft5pk", PS4KernelService::attr_destroy},
    {"pthread_attr_setstacksize", "2Q0z6rnBrTE", PS4KernelService::attr_set_stack},
    {"pthread_attr_getstacksize", "0qOtCR-ZHck", PS4KernelService::attr_get_stack},
    {"pthread_attr_setdetachstate", "E+tyo3lp5Lw", PS4KernelService::attr_set_detach},
    {"pthread_attr_getdetachstate", "VUT1ZSrHT0I", PS4KernelService::attr_get_detach},
    {"pthread_attr_setschedparam", "euKRgm0Vn2M", PS4KernelService::attr_accept},
    {"pthread_attr_setinheritsched", "7ZlAakEf0Qg", PS4KernelService::attr_accept},
    {"pthread_attr_setguardsize", "JKyG3SWyA10", PS4KernelService::attr_accept},
    {"pthread_rwlock_init", "ytQULN-nhL4", PS4KernelService::rwlock_init},
    {"pthread_rwlock_destroy", "1471ajPzxh0", PS4KernelService::rwlock_destroy},
    {"pthread_rwlock_rdlock", "iGjsr1WAtI0", PS4KernelService::rwlock_rdlock},
    {"pthread_rwlock_wrlock", "sIlRvQqsN2Y", PS4KernelService::rwlock_wrlock},
    {"pthread_rwlock_tryrdlock", "SFxTMOfuCkE", PS4KernelService::rwlock_tryrdlock},
    {"pthread_rwlock_trywrlock", "XhWHn6P5R7U", PS4KernelService::rwlock_trywrlock},
    {"pthread_rwlock_unlock", "EgmLo6EWgso", PS4KernelService::rwlock_unlock},
    {"pthread_setname_np", "cB4rMoKU4UI", PS4KernelService::succeed},
    {"pthread_set_name_np", "oxMp8uPqa+U", PS4KernelService::succeed},
    {"pthread_once", "Z4QosVuAsA0", PS4KernelService::once, "libkernel", false, false, 0, once_code, sizeof(once_code)},
    {"sched_yield", "6XG4B33N09g", PS4KernelService::yield, "libkernel", true},
    {"pthread_yield", "T72hz6ffq08", PS4KernelService::yield, "libkernel", true},
    {"pthread_equal", "3PtV6p3QNX4", PS4KernelService::thread_equal, "libkernel", true},
    {"pthread_getspecific", "eoht7mQOCmo", PS4KernelService::get_specific, "libkernel", true},
    {"scePthreadSetspecific", "+BzXYkqYeLE", PS4KernelService::set_specific},
    {"scePthreadKeyCreate", "geDaqgH9lTg", PS4KernelService::key_create},
    {"scePthreadKeyDelete", "PrdHuuDekhY", PS4KernelService::key_delete},
    {"scePthreadOnce", "14bOACANTBo", PS4KernelService::once, "libkernel", false, false, 0, once_code, sizeof(once_code)},
    {"scePthreadRwlockInit", "6ULAa0fq4jA", PS4KernelService::rwlock_init},
    {"scePthreadRwlockDestroy", "BB+kb08Tl9A", PS4KernelService::rwlock_destroy},
    {"scePthreadRwlockRdlock", "Ox9i0c7L5w0", PS4KernelService::rwlock_rdlock},
    {"scePthreadRwlockWrlock", "mqdNorrB+gI", PS4KernelService::rwlock_wrlock},
    {"scePthreadRwlockTryrdlock", "XD3mDeybCnk", PS4KernelService::rwlock_tryrdlock},
    {"scePthreadRwlockTrywrlock", "bIHoZCTomsI", PS4KernelService::rwlock_trywrlock},
    {"scePthreadRwlockUnlock", "+L98PIbGttk", PS4KernelService::rwlock_unlock},
    {"scePthreadSetprio", "W0Hpm2X0uPE", PS4KernelService::succeed},
    {"scePthreadGetprio", "1tKyG7RlMJo", PS4KernelService::thread_getprio},
    {"scePthreadGetaffinity", "rcrVFJsQWRY", PS4KernelService::thread_getaffinity},
    {"scePthreadSetaffinity", "bt3CTBKmGyI", PS4KernelService::succeed},
    {"scePthreadAttrGetaffinity", "8+s5BzZjxSg", PS4KernelService::attr_getaffinity},
    {"scePthreadAttrGet", "x1X76arYMxU", PS4KernelService::attr_get_np},
    {"sigprocmask", "aPcyptbOiZs", PS4KernelService::succeed, "libkernel", true},
    {"_sigprocmask", "6xVpy0Fdq+I", PS4KernelService::succeed, "libkernel", true},
    {"sigfillset", "VkTAsrZDcJ0", PS4KernelService::sigfillset, "libkernel", true},
    {"signal", "VADc3MNQ3cM", PS4KernelService::signal, "libkernel", true},
    {"sceKernelIsNeoMode", "WslcK1FQcGI", PS4KernelService::succeed},
    {"sceKernelIsAddressSanitizerEnabled", "jh+8XiK4LeE", PS4KernelService::succeed},
    {"sceKernelConvertUtcToLocaltime", "-o5uEDpN+oY", PS4KernelService::utc_to_local},
    {"sceKernelConvertLocaltimeToUtc", "0NTHN1NKONI", PS4KernelService::local_to_utc},
    {"sceKernelGetProcParam", "959qrazPIrg", PS4KernelService::proc_param},
    {"_sceKernelRtldSetApplicationHeapAPI", "p5EcQeEeJAE", PS4KernelService::succeed},
    {"sceKernelGetModuleInfoFromAddr", "f7KBOafysXo", PS4KernelService::module_info_ex},
    {"sceKernelGetModuleInfo", "kUpgrXIrz7Q", PS4KernelService::module_info},
    {"sceKernelGetModuleInfoInternal", "HZO7xOos4xc", PS4KernelService::module_info_ex},
    {"sceKernelGetModuleInfoForUnwind", "RpQJJVKTiFM", PS4KernelService::module_info_unwind},
    // The game's own modules are all loaded at boot (load_game): loading one
    // again only gives its handle, unloading has nothing to do.
    {"sceKernelLoadStartModule", "wzvqT4UqKX8", PS4KernelService::module_load_start},
    {"sceKernelStopUnloadModule", "QKd0qM58Qes", PS4KernelService::succeed},
    {"sceKernelDlsym", "LwG8g3niqwA", PS4KernelService::module_dlsym},
    {"sceKernelGetModuleList", "IuxnUuXk6Bg", PS4KernelService::module_list},
    {"sceKernelInternalMemoryGetModuleSegmentInfo", "-YTW+qXc3CQ", PS4KernelService::module_info_from_addr},
    {"__elf_phdr_match_addr", "Fjc4-n1+y2g", PS4KernelService::succeed, "libkernel", true},
    {"sceKernelPrintBacktraceWithModuleInfo", "Wl2o5hOVZdw", PS4KernelService::succeed},
    {"_is_signal_return", "crb5j7mkk1c", PS4KernelService::succeed, "libkernel", true},
    {"sigreturn", "mo0bFmWppIw", PS4KernelService::succeed, "libkernel", true},
    {"__pthread_cxa_finalize", "kbw4UHHSYy0", PS4KernelService::succeed, "libkernel", true},
    {"(libkernel hHlZQUnlxSM)", "hHlZQUnlxSM", PS4KernelService::succeed},
    // Event queue ABI/NIDs: shadPS4 kernel/equeue.cpp. GPU/display filters are
    // deliberately not registered until their actual producers exist.
    {"sceKernelCreateEqueue", "D0OdFMjp46I", PS4KernelService::equeue_create},
    {"sceKernelDeleteEqueue", "jpFjmgAC5AE", PS4KernelService::equeue_delete},
    {"sceKernelWaitEqueue", "fzyMKs9kim0", PS4KernelService::equeue_wait},
    {"sceKernelAddUserEvent", "4R6-OvI2cEA", PS4KernelService::user_event_add},
    {"sceKernelAddUserEventEdge", "WDszmSbWuDk", PS4KernelService::user_event_add_edge},
    {"sceKernelDeleteUserEvent", "LJDwdSNTnDg", PS4KernelService::user_event_delete},
    {"sceKernelTriggerUserEvent", "F6e0kwo4cnk", PS4KernelService::user_event_trigger},
    {"sceKernelGetEventId", "mJ7aghmgvfc", PS4KernelService::event_id},
    {"sceKernelGetEventFilter", "23CPPI1tyBY", PS4KernelService::event_filter},
    {"sceKernelGetEventData", "kwGyyjohI50", PS4KernelService::event_data},
    {"sceKernelGetEventFflags", "Q0qr9AyqJSk", PS4KernelService::event_fflags},
    {"sceKernelGetEventError", "Uu-iDFC9aUc", PS4KernelService::event_error},
    {"sceKernelGetEventUserData", "vz+pg2zdopI", PS4KernelService::event_user_data},
    {"sceGnmAddEqEvent", "b0xyllnVY-I", PS4KernelService::graphics_event_add, "libSceGnmDriver"},
    {"sceGnmDeleteEqEvent", "PVT+fuoS9gU", PS4KernelService::graphics_event_delete, "libSceGnmDriver"},
    {"sceGnmGetEqEventType", "UoYY0DWMC0U", PS4KernelService::event_data, "libSceGnmDriver"},
    {"scePadInit", "hv1luiJrqQM", PS4KernelService::pad_init, "libScePad"},
    {"sceGnmDrawIndexAuto", "GGsn7jMTxw4", PS4KernelService::gnm_draw_auto, "libSceGnmDriver"},
    {"sceGnmSubmitCommandBuffers", "zwY0YV91TTI", PS4KernelService::gnm_submit, "libSceGnmDriver"},
    {"sceGnmAreSubmitsAllowed", "b08AgtPlHPg", PS4KernelService::gnm_allowed, "libSceGnmDriver"},
    {"sceGnmSubmitDone", "yvZ73uQUqrk", PS4KernelService::gnm_done, "libSceGnmDriver"},
    {"sceGnmSubmitAndFlipCommandBuffers", "xbxNatawohc", PS4KernelService::gnm_submit_flip, "libSceGnmDriver"},
    {"sceGnmDrawIndex", "HlTPoZ-oY7Y", PS4KernelService::gnm_draw_index, "libSceGnmDriver"},
    {"sceGnmSetVgtControl", "cFCp0NX8wf0", PS4KernelService::gnm_vgt, "libSceGnmDriver"},
    {"sceGnmSetVsShader", "gAhCn6UiU4Y", PS4KernelService::gnm_vs, "libSceGnmDriver"},
    {"sceGnmSetPsShader350", "5uFKckiJYRM", PS4KernelService::gnm_ps350, "libSceGnmDriver"},
    {"sceGnmSetCsShaderWithModifier", "Kx-h-nWQJ8A", PS4KernelService::gnm_cs, "libSceGnmDriver"},
    {"sceGnmDispatchDirect", "0BzLGljcwBo", PS4KernelService::gnm_dispatch, "libSceGnmDriver"},
    {"sceGnmSetEmbeddedVsShader", "+AFvOEXrKJk", PS4KernelService::gnm_embedded_vs, "libSceGnmDriver"},
    {"sceGnmInsertWaitFlipDone", "1qXLHIpROPE", PS4KernelService::gnm_wait_flip, "libSceGnmDriver"},
    {"sceGnmInsertPushMarker", "W1Etj-jlW7Y", PS4KernelService::gnm_push_marker, "libSceGnmDriver"},
    {"sceGnmInsertPopMarker", "7qZVNgEu+SY", PS4KernelService::gnm_pop_marker, "libSceGnmDriver"},
    {"sceGnmDrawInitDefaultHardwareState350", "yb2cRhagD1I", PS4KernelService::gnm_init350, "libSceGnmDriver"},
    // More command writers (shadPS4 gnmdriver.cpp): the Update* calls write the
    // same registers as their Set* counterparts, as plain register packets.
    {"sceGnmSetPsShader", "bQVd5YzCal0", PS4KernelService::gnm_ps, "libSceGnmDriver"},
    {"sceGnmUpdatePsShader", "4MgRw-bVNQU", PS4KernelService::gnm_ps, "libSceGnmDriver"},
    {"sceGnmUpdatePsShader350", "mLVL7N7BVBg", PS4KernelService::gnm_ps350, "libSceGnmDriver"},
    {"sceGnmUpdateVsShader", "V31V01UiScY", PS4KernelService::gnm_vs, "libSceGnmDriver"},
    {"sceGnmSetCsShader", "KXltnCwEJHQ", PS4KernelService::gnm_cs_plain, "libSceGnmDriver"},
    {"sceGnmDrawIndexOffset", "oYM+YzfCm2Y", PS4KernelService::gnm_draw_index_offset, "libSceGnmDriver"},
    {"sceGnmDrawIndirect", "4v+otIIdjqg", PS4KernelService::gnm_draw_indirect, "libSceGnmDriver"},
    {"sceGnmDrawIndexIndirect", "ED9-Fjr8Ta4", PS4KernelService::gnm_draw_index_indirect, "libSceGnmDriver"},
    {"sceGnmDispatchIndirect", "Z43vKp5k7r0", PS4KernelService::gnm_dispatch_indirect, "libSceGnmDriver"},
    {"sceGnmDispatchInitDefaultHardwareState", "nF6bFRUBRAU", PS4KernelService::gnm_dispatch_init, "libSceGnmDriver"},
    {"sceGnmDrawInitDefaultHardwareState", "Idffwf3yh8s", PS4KernelService::gnm_init350, "libSceGnmDriver"},
    {"sceGnmDrawInitDefaultHardwareState175", "QhnyReteJ1M", PS4KernelService::gnm_init350, "libSceGnmDriver"},
    {"sceGnmDrawInitDefaultHardwareState200", "0H2vBYbTLHI", PS4KernelService::gnm_init350, "libSceGnmDriver"},
    {"sceGnmDrawInitToDefaultContextState", "8lH54sfjfmU", PS4KernelService::gnm_context_init, "libSceGnmDriver"},
    {"sceGnmDrawInitToDefaultContextState400", "im2ZuItabu4", PS4KernelService::gnm_context_init400, "libSceGnmDriver"},
    {"sceGnmResetVgtControl", "MYRtYhojKdA", PS4KernelService::gnm_reset_vgt, "libSceGnmDriver"},
    {"sceGnmInsertSetMarker", "jiItzS6+22g", PS4KernelService::gnm_set_marker, "libSceGnmDriver"},
    {"sceGnmGetGpuCoreClockFrequency", "Fwvh++m9IQI", PS4KernelService::gnm_clock, "libSceGnmDriver"},
    // Workloads (one stream, as shadPS4) carry the frames of Unreal Engine 4;
    // compute queues retire their labels (see PS4GnmQueue::compute).
    {"sceGnmCreateWorkloadStream", "5udAm+6boVg", PS4KernelService::gnm_workload_stream, "libSceGnmDriver"},
    {"sceGnmDestroyWorkloadStream", "UtObDRQiGbs", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmBeginWorkload", "ihxrbsoSKWc", PS4KernelService::gnm_workload_begin, "libSceGnmDriver"},
    {"sceGnmEndWorkload", "Fa3x75OOLRA", PS4KernelService::gnm_workload_end, "libSceGnmDriver"},
    {"sceGnmSubmitCommandBuffersForWorkload", "jRcI8VcgTz4", PS4KernelService::gnm_submit_workload, "libSceGnmDriver"},
    {"sceGnmSubmitAndFlipCommandBuffersForWorkload", "Ga6r7H6Y0RI", PS4KernelService::gnm_submit_flip_workload, "libSceGnmDriver"},
    {"sceGnmMapComputeQueue", "29oKvKXzEZo", PS4KernelService::gnm_map_compute, "libSceGnmDriver"},
    {"sceGnmMapComputeQueueWithPriority", "A+uGq+3KFtQ", PS4KernelService::gnm_map_compute, "libSceGnmDriver"},
    {"sceGnmUnmapComputeQueue", "ArSg-TGinhk", PS4KernelService::gnm_unmap_compute, "libSceGnmDriver"},
    {"sceGnmDingDong", "bX5IbRvECXk", PS4KernelService::gnm_ding_dong, "libSceGnmDriver"},
    {"sceGnmDingDongForWorkload", "byXlqupd8cE", PS4KernelService::gnm_ding_dong, "libSceGnmDriver"},
    {"sceGnmGetOffChipTessellationBufferSize", "FFVZcCu3zWU", PS4KernelService::gnm_tessellation_offchip, "libSceGnmDriver"},
    // Command writers that fill the size the game reserved (PS4Gnm::fill).
    {"sceGnmSetLsShader", "vckdzbQ46SI", PS4KernelService::gnm_ls, "libSceGnmDriver"},
    {"sceGnmSetHsShader", "VJNjFtqiF5w", PS4KernelService::gnm_hs, "libSceGnmDriver"},
    {"sceGnmUpdateHsShader", "GNlx+y7xPdE", PS4KernelService::gnm_hs_update, "libSceGnmDriver"},
    {"sceGnmSetEsShader", "FUHG8sQ3R58", PS4KernelService::gnm_es, "libSceGnmDriver"},
    {"sceGnmSetGsShader", "UJwNuMBcUAk", PS4KernelService::gnm_gs, "libSceGnmDriver"},
    {"sceGnmUpdateGsShader", "nLM2i2+65hA", PS4KernelService::gnm_gs, "libSceGnmDriver"},
    {"sceGnmInsertPushColorMarker", "aPIZJTXC+cU", PS4KernelService::gnm_push_color_marker, "libSceGnmDriver"},
    {"sceGnmInsertSetColorMarker", "aj3L-iaFmyk", PS4KernelService::gnm_set_color_marker, "libSceGnmDriver"},
    {"sceGnmInsertDingDongMarker", "NfvOrNzy6sk", PS4KernelService::gnm_ding_dong_marker, "libSceGnmDriver"},
    {"sceGnmSetEmbeddedPsShader", "X9Omw9dwv5M", PS4KernelService::gnm_embedded_ps, "libSceGnmDriver"},
    {"sceGnmComputeWaitOnAddress", "ffrNQOshows", PS4KernelService::gnm_compute_wait_address, "libSceGnmDriver"},
    {"sceGnmComputeWaitSemaphore", "EJapNl2+pgU", PS4KernelService::gnm_compute_wait_semaphore, "libSceGnmDriver"},
    {"sceGnmDispatchIndirectOnMec", "wED4ZXCFJT0", PS4KernelService::gnm_dispatch_indirect_mec, "libSceGnmDriver"},
    // Resource registration is for the debugger, absent from retail firmware:
    // it fails there too. Garlic flushes and status queries have nothing to do.
    {"sceGnmRegisterResource", "nvEwfYAImTs", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmRegisterOwner", "ZFqKFl23aMc", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmRegisterGdsResource", "9Mv61HaMhfA", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmUnregisterResource", "k8EXkhIP+lM", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmUnregisterOwnerAndResources", "fhKwCVVj9nk", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmUnregisterAllResourcesForOwner", "yhFCnaz5daw", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmFlushGarlic", "iBt3Oe00Kvc", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmIsUserPaEnabled", "jg33rEKLfVs", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetGpuInfoStatus", "tZCSL5ulnB4", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetShaderStatus", "k7iGTvDQPLQ", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetNumTcaUnits", "KnldROUkWJY", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmSetGsRingSizes", "jtkqXpAOY6w", PS4KernelService::gnm_gs_rings, "libSceGnmDriver"},
    {"scePadOpen", "xk0AcarP3V4", PS4KernelService::pad_open, "libScePad"},
    {"scePadClose", "6ncge5+l5Qs", PS4KernelService::pad_close, "libScePad"},
    {"scePadRead", "q1cHNfGycLI", PS4KernelService::pad_read, "libScePad"},
    {"scePadReadState", "YndgXqQVV7c", PS4KernelService::pad_read_state, "libScePad"},
    {"scePadGetControllerInformation", "gjP9-KQzoUk", PS4KernelService::pad_information, "libScePad"},
    {"scePadGetHandle", "u1GRHp+oWoY", PS4KernelService::pad_handle, "libScePad"},
    {"sceUserServiceInitialize", "j3YMu1MVNNo", PS4KernelService::user_initialize, "libSceUserService"},
    {"sceUserServiceTerminate", "bwFjS+bX9mA", PS4KernelService::user_terminate, "libSceUserService"},
    {"sceUserServiceGetInitialUser", "CdWp0oHWGr0", PS4KernelService::user_initial, "libSceUserService"},
    {"sceUserServiceGetLoginUserIdList", "fPhymKNvK-A", PS4KernelService::user_login_list, "libSceUserService"},
    {"sceUserServiceGetEvent", "yH17Q6NWtVg", PS4KernelService::user_event, "libSceUserService"},
    {"sceUserServiceGetUserName", "1xxcMiGu2fo", PS4KernelService::user_name, "libSceUserService"},
    {"sceVideoOutOpen", "Up36PTk687E", PS4KernelService::video_open, "libSceVideoOut"},
    {"sceVideoOutClose", "uquVH4-Du78", PS4KernelService::video_close, "libSceVideoOut"},
    {"sceVideoOutSetBufferAttribute", "i6-sR91Wt-4", PS4KernelService::video_attribute, "libSceVideoOut"},
    {"sceVideoOutRegisterBuffers", "w3BY+tAEiQY", PS4KernelService::video_register, "libSceVideoOut"},
    {"sceVideoOutUnregisterBuffers", "N5KDtkIjjJ4", PS4KernelService::video_unregister, "libSceVideoOut"},
    {"sceVideoOutSubmitFlip", "U46NwOiJpys", PS4KernelService::video_submit, "libSceVideoOut"},
    {"sceVideoOutIsFlipPending", "zgXifHT9ErY", PS4KernelService::video_pending, "libSceVideoOut"},
    {"sceVideoOutGetFlipStatus", "SbU3dwp80lQ", PS4KernelService::video_flip_status, "libSceVideoOut"},
    {"sceVideoOutGetVblankStatus", "1FZBKy8HeNU", PS4KernelService::video_vblank_status, "libSceVideoOut"},
    {"sceVideoOutAddFlipEvent", "HXzjK9yI30k", PS4KernelService::video_add_flip, "libSceVideoOut"},
    {"sceVideoOutAddVblankEvent", "Xru92wHJRmg", PS4KernelService::video_add_vblank, "libSceVideoOut"},
    {"sceVideoOutDeleteFlipEvent", "-Ozn0F1AFRg", PS4KernelService::video_delete_flip, "libSceVideoOut"},
    {"sceVideoOutDeleteVblankEvent", "oNOQn3knW6s", PS4KernelService::video_delete_vblank, "libSceVideoOut"},
    {"sceVideoOutGetEventId", "U2JJtSqNKZI", PS4KernelService::video_event_id, "libSceVideoOut"},
    {"sceVideoOutGetEventData", "rWUTcKdkUzQ", PS4KernelService::video_event_data, "libSceVideoOut"},
    {"sceVideoOutGetEventCount", "Mt4QHHkxkOc", PS4KernelService::video_event_count, "libSceVideoOut"},
    {"sceVideoOutGetResolutionStatus", "6kPnj51T62Y", PS4KernelService::video_resolution, "libSceVideoOut"},
    // Flips follow the host display; a requested rate changes nothing.
    {"sceVideoOutSetFlipRate", "CBiu4mCE1DA", PS4KernelService::video_flip_rate, "libSceVideoOut"},
    // Memory (shadPS4 kernel/memory.cpp).
    {"sceKernelGetDirectMemorySize", "pO96TwzOm5E", PS4KernelService::direct_size},
    {"sceKernelAllocateDirectMemory", "rTXw65xmLIA", PS4KernelService::direct_allocate},
    {"sceKernelReleaseDirectMemory", "MBuItvba6z8", PS4KernelService::direct_release},
    {"sceKernelMapDirectMemory", "L-Q3LEjIbgA", PS4KernelService::direct_map},
    {"sceKernelMapNamedFlexibleMemory", "mL8NDH86iQI", PS4KernelService::flexible_map},
    {"sceKernelMapFlexibleMemory", "IWIBBdTHit4", PS4KernelService::flexible_map_unnamed},
    {"sceKernelReleaseFlexibleMemory", "teiItL2boFw", PS4KernelService::flexible_release},
    {"sceKernelAvailableDirectMemorySize", "C0f7TJcbfac", PS4KernelService::direct_available},
    {"sceKernelAllocateMainDirectMemory", "B+vc2AO2Zrc", PS4KernelService::direct_allocate_main},
    {"sceKernelCheckedReleaseDirectMemory", "hwVSPCmp5tM", PS4KernelService::direct_release},
    {"sceKernelVirtualQuery", "rVjRvHJ0X6c", PS4KernelService::virtual_query},
    {"sceKernelReserveVirtualRange", "7oxv3PPCumo", PS4KernelService::reserve_range},
    {"sceKernelMtypeprotect", "9bfdLIyuwCY", PS4KernelService::mtype_protect},
    {"sceKernelDirectMemoryQuery", "BHouLQzh0X0", PS4KernelService::direct_query},
    {"sceKernelBatchMap", "2SKEx6bSq-4", PS4KernelService::batch_map},
    {"sceKernelBatchMap2", "kBJzF8x4SyE", PS4KernelService::batch_map2},
    // The name (seventh argument) only labels the range.
    {"sceKernelMapNamedDirectMemory", "NcaWUxfMNIQ", PS4KernelService::direct_map},
    {"sceKernelSetVirtualRangeName", "DGMG3JshrZU", PS4KernelService::succeed},
    {"sceKernelQueryMemoryProtection", "WFcfL2lzido", PS4KernelService::query_protection},
    {"sceKernelGetCompiledSdkVersion", "WB66evu8bsU", PS4KernelService::sdk_version},
    {"sceKernelGetSystemSwVersion", "Mv1zUObHvXI", PS4KernelService::system_version},
    {"mmap", "BPE9s9vQQXo", PS4KernelService::posix_mmap, "libkernel", true},
    {"sceLibcHeapGetTraceInfo", "NWtTN10cJzE", PS4KernelService::heap_trace_info, "libSceLibcInternalExt",
     false, false, 0, nullptr, 0, "libSceLibcInternal"},
    {"sceSystemServiceParamGetInt", "fZo48un7LK4", PS4KernelService::system_param_int, "libSceSystemService"},
    {"sceSystemServiceGetStatus", "rPo6tV8D9bM", PS4KernelService::system_status, "libSceSystemService"},
    {"sceSystemServiceLoadExec", "JoBqSQt1yyA", PS4KernelService::load_exec, "libSceSystemService"},
    {"sceSystemServiceReceiveEvent", "656LMQSrg6U", PS4KernelService::system_event, "libSceSystemService"},
    {"sceSystemServiceHideSplashScreen", "Vo5V8KAwCmk", PS4KernelService::succeed, "libSceSystemService"},
    // Save data (shadPS4 libraries/save_data): one directory at a time.
    {"sceSaveDataInitialize3", "TywrFKCoLGY", PS4KernelService::save_init, "libSceSaveData"},
    {"sceSaveDataTerminate", "yKDy8S5yLA0", PS4KernelService::save_term, "libSceSaveData"},
    {"sceSaveDataMount2", "0z45PIH+SNI", PS4KernelService::save_mount2, "libSceSaveData"},
    {"sceSaveDataUmount", "BMR4F-Uek3E", PS4KernelService::save_umount, "libSceSaveData"},
    {"sceSaveDataMount", "32HQAQdwM2o", PS4KernelService::save_mount1, "libSceSaveData"},
    {"sceSaveDataUmount2", "uW4vfTwMQVo", PS4KernelService::save_umount2, "libSceSaveData"},
    {"sceSaveDataDirNameSearch", "dyIhnXq-0SM", PS4KernelService::save_search, "libSceSaveData"},
    {"sceSaveDataSetParam", "85zul--eGXs", PS4KernelService::save_set_param, "libSceSaveData"},
    {"sceSaveDataGetParam", "XgvSuIdnMlw", PS4KernelService::save_get_param, "libSceSaveData"},
    {"sceSaveDataSaveIcon", "c88Yy54Mx0w", PS4KernelService::save_save_icon, "libSceSaveData"},
    {"sceSaveDataLoadIcon", "cGjO3wM3V28", PS4KernelService::save_load_icon, "libSceSaveData"},
    {"sceSaveDataDelete", "S1GkePI17zQ", PS4KernelService::save_delete, "libSceSaveData"},
    {"sceSaveDataGetMountInfo", "65VH0Qaaz6s", PS4KernelService::save_mount_info, "libSceSaveData"},
    {"sceSaveDataSetupSaveDataMemory", "v7AAAMo0Lz4", PS4KernelService::save_memory_setup, "libSceSaveData"},
    {"sceSaveDataGetSaveDataMemory", "7Bt5pBC-Aco", PS4KernelService::save_memory_get, "libSceSaveData"},
    {"sceSaveDataSetSaveDataMemory", "h3YURzXGSVQ", PS4KernelService::save_memory_set, "libSceSaveData"},
    {"sceSaveDataInitialize", "ZkZhskCPXFw", PS4KernelService::save_init, "libSceSaveData"},
    {"sceSaveDataInitialize2", "l1NmDeDpNGU", PS4KernelService::save_init, "libSceSaveData"},
    {"sceSaveDataGetEventResult", "j8xKtiFj0SY", PS4KernelService::save_event, "libSceSaveData"},
    {"sceSaveDataRegisterEventCallback", "hsKd5c21sQc", PS4KernelService::succeed, "libSceSaveData"},
    {"sceSaveDataGetProgress", "ANmSWUiyyGQ", PS4KernelService::save_progress, "libSceSaveData"},
    {"sceSaveDataClearProgress", "Wz-4JZfeO9g", PS4KernelService::succeed, "libSceSaveData"},
    // The save dialog has no screen: it finishes at once, answered OK/Yes.
    {"sceSaveDataDialogInitialize", "s9e3+YpRnzw", PS4KernelService::save_dialog_init, "libSceSaveDataDialog"},
    {"sceSaveDataDialogOpen", "4tPhsP6FpDI", PS4KernelService::save_dialog_open, "libSceSaveDataDialog"},
    {"sceSaveDataDialogUpdateStatus", "KK3Bdg1RWK0", PS4KernelService::save_dialog_update, "libSceSaveDataDialog"},
    {"sceSaveDataDialogGetResult", "yEiJ-qqr6Cg", PS4KernelService::save_dialog_result, "libSceSaveDataDialog"},
    {"sceSaveDataDialogTerminate", "YuH2FA7azqQ", PS4KernelService::save_dialog_term, "libSceSaveDataDialog"},
    // PSN: always signed out, so games take their offline path (shadPS4 np_manager).
    {"sceNpGetState", "eQH7nWPcAgc", PS4KernelService::np_state, "libSceNpManager"},
    {"sceNpGetOnlineId", "XDncXQIJUSk", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpGetNpId", "p-o74CnoNzY", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpGetAccountId", "a8R9-75u4iM", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpGetAccountIdA", "rbknaUjpqWo", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpCheckNpAvailability", "2rsFmlGWleQ", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpCheckNpAvailabilityA", "8Z2Jc5GvGDI", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpCheckPlus", "r6MyYJkryz8", PS4KernelService::np_signed_out, "libSceNpManager"},
    {"sceNpCreateRequest", "GpLQDNKICac", PS4KernelService::np_create_request, "libSceNpManager"},
    {"sceNpDeleteRequest", "S7QTn72PrDw", PS4KernelService::succeed, "libSceNpManager"},
    // Application content: the add-ons the host lists, a full (not trial) game, /temp0.
    {"sceAppContentInitialize", "R9lA82OraNs", PS4KernelService::app_init, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentGetAddcontInfoList", "xnd8BJzAxmk", PS4KernelService::app_addcont_list, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentGetAddcontInfo", "m47juOmH0VE", PS4KernelService::app_addcont_info, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentAddcontMount", "VANhIWcqYak", PS4KernelService::app_addcont_mount, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentAddcontUnmount", "3rHWaV-1KC4", PS4KernelService::app_addcont_unmount, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentAppParamGetInt", "99b82IKXpH4", PS4KernelService::app_param_int, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentTemporaryDataMount2", "buYbeLOGWmA", PS4KernelService::app_temp_mount, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    {"sceAppContentTemporaryDataUnmount", "bcolXMmp6qQ", PS4KernelService::succeed, "libSceAppContent", false, false, 0, nullptr, 0, "libSceAppContentUtil"},
    // Real-time clock: ticks are microseconds since 0001-01-01 00:00 UTC.
    {"sceRtcGetCurrentTick", "18B2NS1y9UU", PS4KernelService::rtc_current_tick, "libSceRtc"},
    {"sceRtcGetCurrentNetworkTick", "zO9UL3qIINQ", PS4KernelService::rtc_current_tick, "libSceRtc"},
    {"sceRtcGetCurrentClock", "8lfvnRMqwEM", PS4KernelService::rtc_current_clock, "libSceRtc"},
    {"sceRtcGetCurrentClockLocalTime", "ZPD1YOKI+Kw", PS4KernelService::rtc_current_local, "libSceRtc"},
    {"sceRtcGetTick", "8w-H19ip48I", PS4KernelService::rtc_get_tick, "libSceRtc"},
    {"sceRtcSetTick", "ueega6v3GUw", PS4KernelService::rtc_set_tick, "libSceRtc"},
    {"sceRtcGetTickResolution", "jMNwqYr4R-k", PS4KernelService::rtc_resolution, "libSceRtc"},
    {"sceRtcConvertUtcToLocalTime", "M1TvFst-jrM", PS4KernelService::rtc_utc_to_local, "libSceRtc"},
    {"sceSystemServiceGetDisplaySafeAreaInfo", "1n37q1Bvc5Y", PS4KernelService::safe_area, "libSceSystemService"},
    // Trophies: valid context and handle IDs, unlocks accepted (nothing shown).
    {"sceNpTrophyCreateContext", "XbkjbobZlCY", PS4KernelService::trophy_create_context, "libSceNpTrophy"},
    {"sceNpTrophyCreateHandle", "q7U6tEAQf7c", PS4KernelService::trophy_create_handle, "libSceNpTrophy"},
    {"sceNpTrophyRegisterContext", "TJCAxto9SEU", PS4KernelService::succeed, "libSceNpTrophy"},
    {"sceNpTrophyUnlockTrophy", "28xmRUFao68", PS4KernelService::trophy_unlock, "libSceNpTrophy"},
    // No trophy data: every query answers zero trophies, nothing unlocked.
    {"sceNpTrophyGetGameInfo", "YYP3f2W09og", PS4KernelService::trophy_game_info, "libSceNpTrophy"},
    {"sceNpTrophyGetGroupInfo", "wTUwGfspKic", PS4KernelService::trophy_group_info, "libSceNpTrophy"},
    {"sceNpTrophyGetTrophyInfo", "qqUVGDgQBm0", PS4KernelService::trophy_group_info, "libSceNpTrophy"},
    {"sceNpTrophyGetTrophyUnlockState", "LHuSmO3SLd8", PS4KernelService::trophy_unlock_state, "libSceNpTrophy"},
    {"sceNpTrophyDestroyContext", "E1Wrwd07Lr8", PS4KernelService::succeed, "libSceNpTrophy"},
    {"sceNpTrophyDestroyHandle", "GNcF4oidY0Y", PS4KernelService::succeed, "libSceNpTrophy"},
    {"sceNpTrophyAbortHandle", "aTnHs7W-9Uk", PS4KernelService::succeed, "libSceNpTrophy"},
    {"sceNpTrophyShowTrophyList", "d9jpdPz5f-8", PS4KernelService::succeed, "libSceNpTrophy"},
    {"sceNpTrophyCaptureScreenshot", "cqGkYAN-gRw", PS4KernelService::succeed, "libSceNpTrophy"},
    {"sceRtcGetTime_t", "BtqmpTRXHgk", PS4KernelService::rtc_time_t, "libSceRtc"},
    {"sceCoredumpRegisterCoredumpHandler", "8zLSfEfW5AU", PS4KernelService::succeed, "libSceCoredump", false, false, 0, nullptr, 0, "libkernel"},
    // Message and error dialogs have no screen: they finish at once, answered
    // OK/Yes, so a game waiting for FINISHED goes on.
    {"sceMsgDialogInitialize", "lDqxaY1UbEo", PS4KernelService::msg_init, "libSceMsgDialog"},
    {"sceMsgDialogOpen", "b06Hh0DPEaE", PS4KernelService::msg_open, "libSceMsgDialog"},
    {"sceMsgDialogUpdateStatus", "6fIC3XKt2k0", PS4KernelService::msg_status, "libSceMsgDialog"},
    {"sceMsgDialogGetStatus", "CWVW78Qc3fI", PS4KernelService::msg_status, "libSceMsgDialog"},
    {"sceMsgDialogGetResult", "Lr8ovHH9l6A", PS4KernelService::msg_result, "libSceMsgDialog"},
    {"sceMsgDialogClose", "HTrcDKlFKuM", PS4KernelService::msg_close, "libSceMsgDialog"},
    {"sceMsgDialogTerminate", "ePw-kqZmelo", PS4KernelService::msg_term, "libSceMsgDialog"},
    {"sceErrorDialogInitialize", "I88KChlynSs", PS4KernelService::error_init, "libSceErrorDialog"},
    {"sceErrorDialogOpen", "M2ZF-ClLhgY", PS4KernelService::error_open, "libSceErrorDialog"},
    {"sceErrorDialogUpdateStatus", "WWiGuh9XfgQ", PS4KernelService::error_status, "libSceErrorDialog"},
    {"sceErrorDialogGetStatus", "t2FvHRXzgqk", PS4KernelService::error_status, "libSceErrorDialog"},
    {"sceErrorDialogClose", "ekXHb1kDBl0", PS4KernelService::error_close, "libSceErrorDialog"},
    {"sceErrorDialogTerminate", "9XAxK2PMwk8", PS4KernelService::error_term, "libSceErrorDialog"},
    // Networking: never connected, so games take their offline path.
    {"sceNetCtlInit", "gky0+oaNM4k", PS4KernelService::succeed, "libSceNetCtl"},
    {"sceNetCtlGetState", "uBPlr0lbuiI", PS4KernelService::netctl_state, "libSceNetCtl"},
    {"sceNetCtlGetInfo", "obuxdTiwkF8", PS4KernelService::netctl_info, "libSceNetCtl"},
    {"sceNetCtlRegisterCallback", "UJ+Z7Q+4ck0", PS4KernelService::netctl_callback, "libSceNetCtl"},
    {"sceNetCtlCheckCallback", "iQw3iQPhvUQ", PS4KernelService::succeed, "libSceNetCtl"},
    {"sceNetInit", "Nlev7Lg8k3A", PS4KernelService::succeed, "libSceNet"},
    {"sceNetPoolCreate", "dgJBaeJnGpo", PS4KernelService::net_pool, "libSceNet"},
    // PlayGo: every chunk is installed and local, nothing is left to download.
    {"scePlayGoInitialize", "ts6GlZOKRrE", PS4KernelService::succeed, "libScePlayGo"},
    {"scePlayGoOpen", "M1Gma1ocrGE", PS4KernelService::playgo_open, "libScePlayGo"},
    {"scePlayGoClose", "Uco1I0dlDi8", PS4KernelService::succeed, "libScePlayGo"},
    {"scePlayGoTerminate", "MPe0EeBGM-E", PS4KernelService::succeed, "libScePlayGo"},
    {"scePlayGoGetLocus", "uWIYLFkkwqk", PS4KernelService::playgo_locus, "libScePlayGo"},
    {"scePlayGoGetProgress", "-RJWNMK3fC8", PS4KernelService::playgo_progress, "libScePlayGo"},
    {"scePlayGoGetToDoList", "Nn7zKwnA5q0", PS4KernelService::playgo_todo, "libScePlayGo"},
    {"scePlayGoGetEta", "v6EZ-YWRdMs", PS4KernelService::playgo_eta, "libScePlayGo"},
    {"scePlayGoGetInstallSpeed", "rvBSfTimejE", PS4KernelService::playgo_speed, "libScePlayGo"},
    {"scePlayGoSetInstallSpeed", "4AAcTU9R3XM", PS4KernelService::succeed, "libScePlayGo"},
    {"scePlayGoGetLanguageMask", "3OMbYZBaa50", PS4KernelService::playgo_language, "libScePlayGo"},
    {"scePlayGoSetLanguageMask", "LosLlHOpNqQ", PS4KernelService::succeed, "libScePlayGo"},
    {"scePlayGoGetChunkId", "73fF1MFU8hA", PS4KernelService::playgo_chunks, "libScePlayGo"},
    {"sceRandomGetRandomNumber", "PI7jIZj4pcE", PS4KernelService::random_number, "libSceRandom"},
    {"sceKernelGetFsSandboxRandomWord", "JGfTMBOdUJo", PS4KernelService::sandbox_word},
    // System modules are part of the HLE: loading one has nothing to do.
    {"sceSysmoduleLoadModule", "g8cM39EUZ6o", PS4KernelService::succeed, "libSceSysmodule"},
    // Audio (shadPS4 libraries/audio/audioout.cpp).
    {"sceAudioOutInit", "JfEPXVxhFqA", PS4KernelService::audio_init, "libSceAudioOut"},
    {"sceAudioOutOpen", "ekNvsT22rsY", PS4KernelService::audio_open, "libSceAudioOut"},
    {"sceAudioOutClose", "s1--uE9mBFw", PS4KernelService::audio_close, "libSceAudioOut"},
    {"sceAudioOutOutput", "QOQtbeDqsT4", PS4KernelService::audio_output, "libSceAudioOut"},
    {"sceAudioOutOutputs", "w3PdaSTSwGE", PS4KernelService::audio_outputs, "libSceAudioOut"},
    {"sceAudioOutGetPortState", "GrQ9s4IrNaQ", PS4KernelService::audio_port_state, "libSceAudioOut"},
    {"sceAudioOutSetVolume", "b+uAV89IlxE", PS4KernelService::succeed, "libSceAudioOut"},
    {"msync", "tZY4+SZNFhA", PS4KernelService::succeed, "libkernel", true},
    {"sceKernelMlock", "3k6kx-zOOSQ", PS4KernelService::succeed},
    // Files (shadPS4 kernel/file_system.cpp).
    {"sceKernelWrite", "4wSze92BhLI", PS4KernelService::write},
    {"write", "FxVZqBAA7ks", PS4KernelService::write, "libkernel", true},
    {"read", "DRuBt2pvICk", PS4KernelService::read, "libkernel", true},
    {"sceKernelPread", "+r3rMFwItV4", PS4KernelService::pread},
    {"sceKernelPwrite", "nKWi-N2HBV4", PS4KernelService::pwrite},
    {"sceKernelFsync", "fTx66l5iWIA", PS4KernelService::fsync},
    {"sceKernelFtruncate", "VW3TVZiM4-E", PS4KernelService::ftruncate},
    {"sceKernelTruncate", "WlyEA-sLDf0", PS4KernelService::truncate},
    {"sceKernelMkdir", "1-LFLmRFxxM", PS4KernelService::mkdir},
    {"mkdir", "JGMio+21L4c", PS4KernelService::mkdir, "libkernel", true},
    {"sceKernelRmdir", "naInUjYt3so", PS4KernelService::rmdir},
    {"sceKernelRename", "52NcYU9+lEo", PS4KernelService::rename},
    {"rename", "NN01qLRhiqU", PS4KernelService::rename, "libkernel", true},
    {"sceKernelUnlink", "AUXVxWeJU-A", PS4KernelService::unlink},
    {"sceKernelGetdirentries", "taRWhTJFTgE", PS4KernelService::getdirentries},
    // Thread names are not kept: an empty one (Unreal Engine 4 logs them).
    {"scePthreadGetname", "How7B8Oet6k", PS4KernelService::thread_getname},
    // Imported by the libc games ship (Orbis error codes).
    {"scePthreadAttrGetschedparam", "FXPWHNk8Of0", PS4KernelService::attr_getschedparam},
    {"scePthreadMutexReltimedlockNp", "IafI2PxcPnQ", PS4KernelService::mutex_reltimedlock},
    {"pthread_getname_np", "9HzfhdtESio", PS4KernelService::thread_getname, "libkernel", true},
    // libSceLibcInternal (see internal_memcpy_code).
    {"internal_atan", "OXmauLdQ8kY", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_atan_code, sizeof(internal_atan_code)},
    {"internal_atan2f", "EH-x713A99c", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_atan2f_code, sizeof(internal_atan2f_code)},
    {"internal_atanf", "weDug8QD-lE", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_atanf_code, sizeof(internal_atanf_code)},
    {"internal_cos", "2WE3BTYVwKM", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_cos_code, sizeof(internal_cos_code)},
    {"internal_cosf", "-P6FNMzk2Kc", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_cosf_code, sizeof(internal_cosf_code)},
    {"internal_expf", "8zsu04XNsZ4", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_expf_code, sizeof(internal_expf_code)},
    {"internal_logf", "RQXLbdT2lc4", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_logf_code, sizeof(internal_logf_code)},
    {"internal_memcmp", "DfivPArhucg", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_memcmp_code, sizeof(internal_memcmp_code)},
    {"internal_memcpy", "Q3VBxCXhUHs", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_memcpy_code, sizeof(internal_memcpy_code)},
    {"internal_memcpy_s", "NFLs+dRJGNg", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_memcpy_s_code, sizeof(internal_memcpy_s_code)},
    {"internal_memmove", "+P6FRGH4LfA", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_memmove_code, sizeof(internal_memmove_code)},
    {"internal_memset", "8zTFvBIAIN8", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_memset_code, sizeof(internal_memset_code)},
    {"internal_sincosf", "pztV4AF18iI", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_sincosf_code, sizeof(internal_sincosf_code)},
    {"internal_sinf", "Q4rRL34CEeE", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_sinf_code, sizeof(internal_sinf_code)},
    {"internal_strcmp", "Ovb2dSJOAuE", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_strcmp_code, sizeof(internal_strcmp_code)},
    {"internal_strlen", "j4ViWNHEgww", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_strlen_code, sizeof(internal_strlen_code)},
    {"internal_strncmp", "aesyjrHVWy4", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_strncmp_code, sizeof(internal_strncmp_code)},
    {"internal_strncpy", "6sJWiWSRuqk", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_strncpy_code, sizeof(internal_strncpy_code)},
    {"internal_strncpy_s", "YNzNkJzYqEg", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_strncpy_s_code, sizeof(internal_strncpy_s_code)},
    {"internal_tan", "T7uyNqP7vQA", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_tan_code, sizeof(internal_tan_code)},
    {"internal_tanf", "ZE6RNL+eLbk", PS4KernelService::succeed, "libSceLibcInternal", false, false, 0, internal_tanf_code, sizeof(internal_tanf_code)},
    // libc start-up (Crash Bandicoot 4): thread exit handlers and sanitizer
    // replacement tables, none here.
    {"_sceKernelSetThreadAtexitCount", "pB-yGZ2nQ9o", PS4KernelService::succeed},
    {"_sceKernelSetThreadAtexitReport", "WhCc1w3EhSI", PS4KernelService::succeed},
    {"_sceKernelRtldThreadAtexitIncrement", "Tz4RNUCBbGI", PS4KernelService::succeed},
    {"_sceKernelRtldThreadAtexitDecrement", "8OnWXlgQlvo", PS4KernelService::succeed},
    {"sceKernelGetSanitizerMallocReplaceExternal", "py6L8jiVAN8", PS4KernelService::succeed},
    {"sceKernelGetSanitizerNewReplaceExternal", "bnZxYgAFeA0", PS4KernelService::succeed},
    // Crash Bandicoot 4 (Unreal Engine 4), after shadPS4. A schedule
    // or timestamp change does nothing here; select only times out.
    {"getpid", "HoLVWNanBBc", PS4KernelService::process_id, "libkernel", true},
    {"close", "bY-PO6JhzhQ", PS4KernelService::close, "libkernel", true},
    {"stat", "E6ao34wPw+U", PS4KernelService::stat, "libkernel", true},
    {"usleep", "QcteRwbsnV0", PS4KernelService::usleep, "libkernel", true},
    {"rmdir", "c7ZnT7V1B98", PS4KernelService::rmdir, "libkernel", true},
    {"select", "T8fER+tIGgk", PS4KernelService::succeed, "libkernel", true},
    {"pthread_getspecific", "0-KXaS70xy4", PS4KernelService::get_specific, "libkernel", true},
    {"pthread_getthreadid_np", "EI-5-jlq2dE", PS4KernelService::thread_id, "libkernel", true},
    {"pthread_getschedparam", "FIs3-UQT9sg", PS4KernelService::sched_param_get, "libkernel", true},
    {"pthread_setschedparam", "Xs9hdiD7sAA", PS4KernelService::succeed, "libkernel", true},
    {"sched_get_priority_max", "CBNtXOoef-E", PS4KernelService::sched_priority_max, "libkernel", true},
    {"sched_get_priority_min", "m0iS6jNsXds", PS4KernelService::sched_priority_min, "libkernel", true},
    {"_sceKernelSetThreadDtors", "rNhWz+lvOMU", PS4KernelService::succeed},
    {"sceKernelGetdents", "j2AIqSqJP0w", PS4KernelService::getdents},
    {"sceKernelChmod", "fgIsQ10xYVA", PS4KernelService::succeed},
    {"sceKernelUtimes", "0Cq8ipKr9n0", PS4KernelService::succeed},
    {"sceKernelAvailableFlexibleMemorySize", "aNz11fnnzi4", PS4KernelService::flexible_available},
    {"sceKernelGetPrtAperture", "L0v2Go5jOuM", PS4KernelService::prt_aperture},
    {"sceKernelAioInitializeImpl", "vYU8P9Td2Zo", PS4KernelService::succeed},
    {"sceKernelAioInitializeParam", "nu4a0-arQis", PS4KernelService::succeed},
    {"sceKernelAioDeleteRequest", "5TgME6AYty4", PS4KernelService::succeed},
    {"sceKernelAioSubmitReadCommands", "HgX7+AORI58", PS4KernelService::aio_read},
    {"sceKernelAioSubmitWriteCommands", "XQ8C8y+de+E", PS4KernelService::aio_write},
    {"sceKernelAioWaitRequest", "KOF-oJbQVvc", PS4KernelService::aio_wait},
    {"sceKernelMemoryPoolReserve", "pU-QydtGcGY", PS4KernelService::pool_reserve},
    {"sceKernelMemoryPoolCommit", "Vzl66WmfLvk", PS4KernelService::pool_commit},
    {"sceKernelMemoryPoolDecommit", "LXo1tpFqJGs", PS4KernelService::pool_decommit},
    {"sceKernelMemoryPoolExpand", "qCSfqDILlns", PS4KernelService::pool_expand},
    // No network: every socket call fails as if the protocol were unsupported.
    {"accept", "3e+4Iv7IJ8U", PS4KernelService::net_unavailable, "libkernel", true},
    {"bind", "KuOmgKoqCdY", PS4KernelService::net_unavailable, "libkernel", true},
    {"connect", "XVL8So3QJUk", PS4KernelService::net_unavailable, "libkernel", true},
    {"getpeername", "TXFFFiNldU8", PS4KernelService::net_unavailable, "libkernel", true},
    {"getsockname", "RenI1lL1WFk", PS4KernelService::net_unavailable, "libkernel", true},
    {"getsockopt", "6O8EwYOgH9Y", PS4KernelService::net_unavailable, "libkernel", true},
    {"listen", "pxnCmagrtao", PS4KernelService::net_unavailable, "libkernel", true},
    {"recv", "Ez8xjo9UF4E", PS4KernelService::net_unavailable, "libkernel", true},
    {"recvfrom", "lUk6wrGXyMw", PS4KernelService::net_unavailable, "libkernel", true},
    {"send", "fZOeZIOEmLw", PS4KernelService::net_unavailable, "libkernel", true},
    {"sendto", "oBr313PppNE", PS4KernelService::net_unavailable, "libkernel", true},
    {"setsockopt", "fFxGkxF2bVo", PS4KernelService::net_unavailable, "libkernel", true},
    {"shutdown", "TUuiYS2kE8s", PS4KernelService::net_unavailable, "libkernel", true},
    {"socket", "TU-d9PfIHPM", PS4KernelService::net_unavailable, "libkernel", true},
    {"sceVideoOutWaitVblank", "j6RaAUlaLv0", PS4KernelService::succeed, "libSceVideoOut"},
    {"sceVideoOutGetDeviceCapabilityInfo", "kGVLc3htQE8", PS4KernelService::video_capability, "libSceVideoOut"},
    {"sceVideoOutConfigureOutputMode_", "N1bEoJ4SRw4", PS4KernelService::succeed, "libSceVideoOut"},
    {"sceVideoOutModeSetAny_", "pjkDsgxli6c", PS4KernelService::succeed, "libSceVideoOut"},
    {"sceVideoOutSetWindowModeMargins", "MTxxrOCeSig", PS4KernelService::succeed, "libSceVideoOut"},
    {"scePadSetLightBar", "RR4novUEENY", PS4KernelService::succeed, "libScePad"},
    {"scePadResetLightBar", "DscD1i9HX1w", PS4KernelService::succeed, "libScePad"},
    {"scePadSetVibration", "yFVnOdGxvZY", PS4KernelService::succeed, "libScePad"},
    {"scePadResetOrientation", "rIZnR6eSpvk", PS4KernelService::succeed, "libScePad"},
    {"sceSysmoduleIsLoaded", "fMP5NHUOaMk", PS4KernelService::succeed, "libSceSysmodule"},
    {"sceSysmoduleLoadModuleInternalWithArg", "hHrGoGoNf+s", PS4KernelService::succeed, "libSceSysmodule"},
    {"sceSysmoduleUnloadModule", "eR2bZFAAU0Q", PS4KernelService::succeed, "libSceSysmodule"},
    {"sceSysmoduleGetModuleInfoForUnwind", "4fU5yvOkVG4", PS4KernelService::module_info_unwind, "libSceSysmodule"},
    {"sceDiscMapIsRequestOnHDD", "lbQKqsERhtE", PS4KernelService::discmap_no_bitmap, "libSceDiscMap"},
    {"Func_7C980FFB0AA27E7A", "fJgP+wqifno", PS4KernelService::discmap_flags, "libSceDiscMap"},
    {"sceCommonDialogInitialize", "uoUpLGNkygk", PS4KernelService::succeed, "libSceCommonDialog"},
    {"sceScreenShotEnable", "2xxUtuC-RzE", PS4KernelService::succeed, "libSceScreenShot"},
    {"sceScreenShotDisable", "tIYf0W5VTi8", PS4KernelService::succeed, "libSceScreenShot"},
    {"sceNetCtlTerm", "Z4wwCFiBELQ", PS4KernelService::succeed, "libSceNetCtl"},
    // Mortal Kombat: Legacy Kollection (Digital Eclipse), after shadPS4.
    // POSIX names the PlayFab and Party libraries import from libScePosix.
    {"open", "wuCroIGjt2g", PS4KernelService::open, "libkernel", true},
    {"read", "AqBioC2vF3I", PS4KernelService::read, "libkernel", true},
    {"write", "FN4gaPmuFV8", PS4KernelService::write, "libkernel", true},
    {"lseek", "Oy6IpwgtYOk", PS4KernelService::lseek, "libkernel", true},
    {"fstat", "mqQMh1zPPT8", PS4KernelService::fstat, "libkernel", true},
    {"ftruncate", "ih4CD9-gghM", PS4KernelService::ftruncate, "libkernel", true},
    {"munmap", "UqDGjXA5yUM", PS4KernelService::munmap, "libkernel", true},
    {"mprotect", "YQOfxL4QfeU", PS4KernelService::mprotect, "libkernel", true},
    {"mlock", "mTBZfEal2Bw", PS4KernelService::succeed, "libkernel", true},
    {"madvise", "Jahsnh4KKkg", PS4KernelService::succeed, "libkernel", true},
    {"flock", "9eMlfusH4sU", PS4KernelService::succeed, "libkernel", true},
    {"fcntl", "8nY19bKoiZk", PS4KernelService::succeed, "libkernel", true},
    {"clock_getres", "smIj7eqzZE8", PS4KernelService::clock_getres, "libkernel", true},
    {"pthread_equal", "7Xl257M4VNI", PS4KernelService::thread_equal, "libkernel", true},
    {"pthread_condattr_setclock", "EjllaAqAPZo", PS4KernelService::succeed, "libkernel", true},
    {"sem_init", "pDuPEf3m4fI", PS4KernelService::sem_init, "libkernel", true},
    {"sem_destroy", "cDW233RAwWo", PS4KernelService::sem_destroy, "libkernel", true},
    {"sem_wait", "YCV5dGGBcCo", PS4KernelService::sem_wait, "libkernel", true},
    {"sem_timedwait", "w5IHyvahg-o", PS4KernelService::sem_timedwait, "libkernel", true},
    {"sem_post", "IKP8typ0QUk", PS4KernelService::sem_post, "libkernel", true},
    {"recvmsg", "hI7oVeOluPM", PS4KernelService::net_unavailable, "libkernel", true},
    {"sendmsg", "aNeavPDNKzA", PS4KernelService::net_unavailable, "libkernel", true},
    {"inet_ntop", "5jRCs2axtr4", PS4KernelService::net_ntop, "libkernel", true},
    {"inet_pton", "4n51s0zEf0c", PS4KernelService::net_pton, "libkernel", true},
    {"sceKernelUuidCreate", "Xjoosiw+XPI", PS4KernelService::uuid_create},
    {"sceKernelCheckReachability", "uWyW3v98sU4", PS4KernelService::succeed},
    // libSceNet without a network: sockets and the resolver fail, the rest is accepted.
    {"sceNetAccept", "PIWqhn9oSxc", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetBind", "bErx49PgxyY", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetConnect", "OXXX4mUk3uk", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetGetsockname", "hoOAofhhRvE", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetGetsockopt", "xphrZusl78E", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetListen", "kOj1HiAGE54", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetRecvmsg", "wvuUDv0jrMI", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetSendmsg", "2eKbgcboJso", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetSetsockopt", "2mKX2Spso7I", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetShutdown", "TSM6whtekok", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetSocket", "Q4qBuN-c0ZM", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetResolverCreate", "C4UgDHHPvdw", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetResolverStartNtoaMultipleRecords", "RCCY01Xd+58", PS4KernelService::net_unavailable, "libSceNet"},
    {"sceNetSocketClose", "45ggEzakPJQ", PS4KernelService::succeed, "libSceNet"},
    {"sceNetTerm", "cTGkc6-TBlI", PS4KernelService::succeed, "libSceNet"},
    {"sceNetPoolDestroy", "K7RlrTkI-mw", PS4KernelService::succeed, "libSceNet"},
    {"sceNetResolverDestroy", "kJlYH5uMAWI", PS4KernelService::succeed, "libSceNet"},
    {"sceNetResolverGetError", "J5i3hiLJMPk", PS4KernelService::succeed, "libSceNet"},
    {"sceNetErrnoLoc", "HQOwnfMGipQ", PS4KernelService::errno_location, "libSceNet"},
    {"sceNetInetNtop", "9vA2aW+CHuA", PS4KernelService::net_ntop, "libSceNet"},
    {"sceNetInetPton", "8Kcp5d-q1Uo", PS4KernelService::net_pton, "libSceNet"},
    // HTTP, HTTP/2 and Web API contexts open; every request fails (offline).
    {"sceHttpInit", "A9cVMUtEp4Y", PS4KernelService::net_pool, "libSceHttp"},
    {"sceHttpTerm", "Ik-KpLTlf7Q", PS4KernelService::succeed, "libSceHttp"},
    {"sceHttpDeleteTemplate", "4I8vEpuEhZ8", PS4KernelService::succeed, "libSceHttp"},
    {"sceHttpDeleteConnection", "P6A3ytpsiYc", PS4KernelService::succeed, "libSceHttp"},
    {"sceHttpDeleteRequest", "qe7oZ+v4PWA", PS4KernelService::succeed, "libSceHttp"},
    {"sceHttpAbortRequest", "hvG6GfBMXg8", PS4KernelService::succeed, "libSceHttp"},
    {"sceHttpCreateTemplate", "0gYjPTR-6cY", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpCreateConnectionWithURL", "qgxDBjorUxs", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpCreateRequestWithURL", "Aeu5wVKkF9w", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpAddRequestHeader", "EY28T2bkN7k", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpSendRequest", "1e2BNwI-XzE", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpReadData", "P5pdoykPYTk", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpGetStatusCode", "0a2TBNfE3BU", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpGetResponseContentLength", "yuO2H2Uvnos", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttpSetRequestContentLength", "PTiFIUxCpJc", PS4KernelService::net_unavailable, "libSceHttp"},
    {"sceHttp2Init", "3JCe3lCbQ8A", PS4KernelService::net_pool, "libSceHttp2"},
    {"sceHttp2Term", "YiBUtz-pGkc", PS4KernelService::succeed, "libSceHttp2"},
    {"sceHttp2DeleteTemplate", "pDom5-078DA", PS4KernelService::succeed, "libSceHttp2"},
    {"sceHttp2DeleteRequest", "c8D9qIjo8EY", PS4KernelService::succeed, "libSceHttp2"},
    {"sceHttp2CreateTemplate", "+wCt7fCijgk", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2CreateRequestWithURL", "mmyOCxQMVYQ", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2AddRequestHeader", "nrPfOE8TQu0", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2SendRequestAsync", "A+NVAFu4eCg", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2ReadDataAsync", "bGN-6zbo7ms", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2WaitAsync", "MOp-AUhdfi8", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2GetStatusCode", "9XYJwCf3lEA", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceHttp2GetAllResponseHeaders", "-rdXUi2XW90", PS4KernelService::net_unavailable, "libSceHttp2"},
    {"sceNpWebApi2Initialize", "+o9816YQhqQ", PS4KernelService::net_pool, "libSceNpWebApi2"},
    {"sceNpWebApi2Terminate", "bEvXpcEk200", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2DeleteUserContext", "9X9+cneTGUU", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2DeleteRequest", "vvzWO-DvG1s", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventDeleteFilter", "KJdPcOGmK58", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventDeleteHandle", "fIATVMo4Y1w", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventDeletePushContext", "QafxeZM3WK4", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventUnregisterCallback", "hOnIlcGrO6g", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventUnregisterPushContextCallback", "PmyrbbJSFz0", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2AddWebTraceTag", "MgsTa76wlEk", PS4KernelService::succeed, "libSceNpWebApi2"},
    {"sceNpWebApi2CreateUserContext", "sk54bi6FtYM", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2CreateRequest", "3EI-OSJ65Xc", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2AddHttpRequestHeader", "egOOvrnF6mI", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2SendRequest", "lQOCF84lvzw", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2ReadData", "OOY9+ObfKec", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2GetHttpResponseHeaderValue", "hksbskNToEA", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventCreateFilter", "MsaFhR+lPE4", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventCreateHandle", "WV1GwM32NgY", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventCreatePushContext", "NNVf18SlbT8", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventRegisterCallback", "fY3QqeNkF8k", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventRegisterPushContextCallback", "lxtHJMwBsaU", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceNpWebApi2PushEventStartPushContextCallback", "AAj9X+4aGYA", PS4KernelService::np_signed_out, "libSceNpWebApi2"},
    {"sceSslInit", "hdpVEUDFW3s", PS4KernelService::net_pool, "libSceSsl"},
    {"sceSslTerm", "0K1yQ6Lv-Yc", PS4KernelService::succeed, "libSceSsl"},
    {"sceNetCtlGetResult", "0cBgduPRR+M", PS4KernelService::netctl_info, "libSceNetCtl"},
    // Voice chat: no microphone, so no port opens.
    {"sceVoiceInit", "9TrhuGzberQ", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceEnd", "Oo0S5PH7FIQ", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceStart", "54phPH2LZls", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceStop", "Ao2YNSA7-Qo", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceDeletePort", "b7kJI+nx2hg", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceSetVolume", "QBFoAIjJoXQ", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceDisconnectIPortFromOPort", "ajVj3QG2um4", PS4KernelService::succeed, "libSceVoice"},
    {"sceVoiceCreatePort", "nXpje5yNpaE", PS4KernelService::net_unavailable, "libSceVoice"},
    {"sceVoiceConnectIPortToOPort", "oV9GAdJ23Gw", PS4KernelService::net_unavailable, "libSceVoice"},
    {"sceVoiceGetPortInfo", "CrLqDwWLoXM", PS4KernelService::net_unavailable, "libSceVoice"},
    {"sceVoiceReadFromOPort", "cQ6DGsQEjV4", PS4KernelService::net_unavailable, "libSceVoice"},
    {"sceVoiceWriteToIPort", "YeJl6yDlhW0", PS4KernelService::net_unavailable, "libSceVoice"},
    {"sceNpCheckCallback", "3Zl8BePTh9Y", PS4KernelService::succeed, "libSceNpManager"},
    {"sceNpNotifyPlusFeature", "Gaxrp3EWY-M", PS4KernelService::succeed, "libSceNpManager"},
    {"sceNpRegisterNpReachabilityStateCallback", "hw5KNqAAels", PS4KernelService::succeed, "libSceNpManager"},
    {"sceNpRegisterStateCallbackA", "qQJfO8HAiaY", PS4KernelService::succeed, "libSceNpManager"},
    {"sceNpSetContentRestriction", "A2CQ3kgSopQ", PS4KernelService::succeed, "libSceNpManager"},
    {"sceNpGetParentalControlInfoA", "m9L3O6yst-U", PS4KernelService::np_signed_out, "libSceNpManager"},
    // Game intents (activities started from the system menu): none arrives.
    {"sceNpGameIntentInitialize", "m87BHxt-H60", PS4KernelService::succeed, "libSceNpGameIntent"},
    {"sceNpGameIntentTerminate", "0HBYxYAjmf0", PS4KernelService::succeed, "libSceNpGameIntent"},
    {"sceNpGameIntentReceiveIntent", "jEIXUAr9XE8", PS4KernelService::np_signed_out, "libSceNpGameIntent"},
    {"sceNpGameIntentGetPropertyValueString", "rPl0INNc-M8", PS4KernelService::np_signed_out, "libSceNpGameIntent"},
    {"sceNpAuthCreateRequest", "6bwFkosYRQg", PS4KernelService::np_create_request, "libSceNpAuth"},
    {"sceNpAuthDeleteRequest", "H8wG9Bk-nPc", PS4KernelService::succeed, "libSceNpAuth"},
    {"sceNpAuthGetAuthorizationCodeV3", "KI4dHLlTNl0", PS4KernelService::np_signed_out, "libSceNpAuth"},
    // The store and the PlayGo dialog finish at once, like message dialogs.
    {"sceNpCommerceDialogInitialize", "0aR2aWmQal4", PS4KernelService::msg_init, "libSceNpCommerce"},
    {"sceNpCommerceDialogOpen", "DfSCDRA3EjY", PS4KernelService::msg_open, "libSceNpCommerce"},
    {"sceNpCommerceDialogUpdateStatus", "LR5cwFMMCVE", PS4KernelService::msg_status, "libSceNpCommerce"},
    {"sceNpCommerceDialogGetResult", "r42bWcQbtZY", PS4KernelService::msg_result, "libSceNpCommerce"},
    {"sceNpCommerceDialogTerminate", "m-I92Ab50W8", PS4KernelService::msg_term, "libSceNpCommerce"},
    {"scePlayGoDialogInitialize", "fECamTJKpsM", PS4KernelService::msg_init, "libScePlayGoDialog"},
    {"scePlayGoDialogOpen", "kHd72ukqbxw", PS4KernelService::msg_open, "libScePlayGoDialog"},
    {"scePlayGoDialogUpdateStatus", "Yb60K7BST48", PS4KernelService::msg_status, "libScePlayGoDialog"},
    {"scePlayGoDialogGetResult", "wx9TDplJKB4", PS4KernelService::msg_result, "libScePlayGoDialog"},
    {"scePlayGoDialogTerminate", "okgIGdr5Iz0", PS4KernelService::msg_term, "libScePlayGoDialog"},
    // No on-screen keyboard: the IME dialog does not open.
    {"sceImeDialogInit", "NUeBrN7hzf0", PS4KernelService::net_unavailable, "libSceImeDialog"},
    {"sceImeDialogGetResult", "x01jxu+vxlc", PS4KernelService::net_unavailable, "libSceImeDialog"},
    {"sceImeDialogGetStatus", "IADmD4tScBY", PS4KernelService::succeed, "libSceImeDialog"},
    {"sceImeDialogTerm", "gyTyVn+bXMw", PS4KernelService::succeed, "libSceImeDialog"},
    // GPU debugging, validation and resource registration: off and empty.
    {"sceGnmDebugHardwareStatus", "qpGITzPE+Zc", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmDisableMipStatsReport", "HHo1BAljZO8", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmDriverCaptureInProgress", "TLV4mswiZ4A", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmDriverTraceInProgress", "R6z1xM3pW-w", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmDriverTriggerCapture", "d88anrgNoKY", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmRequestMipStatsReportAndReset", "f85orjx7qts", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmSetupMipStatsReport", "+xuDhxlWRPg", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmSetWaveLimitMultipliers", "XiyzNZ9J4nQ", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateDisableDiagnostics", "SXw4dZEkgpA", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateDisableDiagnostics2", "BgM3t3LvcNk", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateDispatchCommandBuffers", "qGP74T5OWJc", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateDrawCommandBuffers", "hsZPf1lON7E", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateGetDiagnosticInfo", "RX7XCNSaL6I", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateGetDiagnostics", "5SHGNwLXBV4", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateGetVersion", "HzMN7ANqYEc", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateOnSubmitEnabled", "rTIV11nMQuM", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidateResetState", "MBMa6EFu4Ko", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmValidationRegisterMemoryCheckCallback", "Q7t4VEYLafI", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceRazorCaptureImmediate", "u9YKpRRHe-M", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceRazorIsLoaded", "f33OrruQYbM", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmSetResourceRegistrationUserMemory", "q-qhDxP67Hg", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmQueryResourceRegistrationUserMemoryRequirements", "6IMbpR7nTzA", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmSetResourceUserData", "K3BKBBYKUSE", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetGpuBlockStatus", "oL4hGI1PMpw", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetLastWaitedAddress", "iFirFzgYsvw", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetEqTimeStamp", "H7-fgvEutM0", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmGetProtectionFaultTimeStamp", "fzJdEihTFV4", PS4KernelService::succeed, "libSceGnmDriver"},
    {"sceGnmFindResourcesPublic", "4Mv9OXypBG8", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetOwnerName", "QJjPjlmPAL0", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetResourceBaseAddressAndSizeInBytes", "4PKnYXOhcx4", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetResourceName", "O0S96YnD04U", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetResourceShaderGuid", "UBv7FkVfzcQ", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetResourceType", "bdqdvIkLPIU", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetResourceUserData", "UoBuWAhKk7U", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetShaderProgramBaseAddress", "nEyFbYUloIM", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetPhysicalCounterFromVirtualized", "dewXw5roLs0", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmGetTheTessellationFactorRingBufferBaseAddress", "ln33zjBrfjk", PS4KernelService::gnm_tessellation_ring, "libSceGnmDriver"},
    {"sceGnmLogicalCuIndexToPhysicalCuIndex", "26PM5Mzl8zc", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmLogicalCuMaskToPhysicalCuMask", "RU74kek-N0c", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmLogicalTcaUnitToPhysical", "Kl0Z3LH07QI", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmDrawIndexIndirectCountMulti", "thbPcG7E7qk", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmDrawIndirectCountMulti", "cUCo8OvArrw", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmDrawIndexMultiInstanced", "jHdPvIzlpKc", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmDrawOpaqueAuto", "stDSYW2SBVM", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmRequestFlipAndSubmitDone", "gObODli-OH8", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    {"sceGnmRequestFlipAndSubmitDoneForWorkload", "6YRHhh5mHCs", PS4KernelService::gnm_unsupported, "libSceGnmDriver"},
    // Data objects, filled by PS4Libkernel::initialize_data. "MaxPS4errno" is
    // engine-internal (no game imports it): the per-thread errno cells.
    {"__stack_chk_guard", "f7uOxY9mM1U", PS4KernelService::stack_chk_guard, "libkernel", false, false, 8},
    {"__progname", "djxxOmW6-aw", PS4KernelService::progname, "libkernel", false, false, 64},
    {"(errno table)", "MaxPS4errno", PS4KernelService::errno_table, "libkernel", false, false,
        uint32_t(PS4Threads::thread_limit * 8)}
};
uint64_t monotonic_us() {
    timespec t{};
    if (::clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return uint64_t(t.tv_sec) * 1000000 + uint64_t(t.tv_nsec) / 1000;
}
class HostFD {
public:
    explicit HostFD(int fd) : fd(fd) {}
    ~HostFD() { if (fd >= 0) ::close(fd); }
    HostFD(const HostFD&) = delete;
    HostFD& operator=(const HostFD&) = delete;
    int fd;
};
}
const PS4KernelExport* libkernel_exports(size_t& count) {
    count = sizeof(bindings) / sizeof(bindings[0]);
    return bindings;
}
PS4KernelService resolve_libkernel(std::string_view symbol, std::string_view library,
                                   std::string_view module, uint32_t version) {
    // A binding's library names its module too (libkernel/libkernel).
    if (version != 1 || library != module) return PS4KernelService::unresolved;
    const std::string_view owner = library == "libScePosix" ? std::string_view("libkernel") : library;
    for (const auto& binding : bindings)
        if (owner == binding.library && (symbol == binding.name || symbol == binding.nid)) return binding.service;
    return PS4KernelService::unresolved;
}
PS4Libkernel::PS4Libkernel(PS4GuestMemory& memory, PS4VirtualMemory* dynamic)
    : memory_(memory), dynamic_(dynamic), threads_(memory), videoout_(memory, threads_), user_service_(memory), pad_(memory), audio_(memory, threads_), gnm_(memory), graphics_(memory), started_us_(monotonic_us()) {}
PS4Libkernel::~PS4Libkernel() {
    reset();
}
void PS4Libkernel::close_files() {
    for (auto& file : files_) { if (file.fd >= 0) ::close(file.fd); file = File{}; }
}
void PS4Libkernel::reset() {
    std::lock_guard<std::mutex> guard(lock_);
    close_files();
    for (auto& mount : mounts_) {
        if (mount.root >= 0) { ::close(mount.root); mount.root = -1; }
        if (mount.overlay >= 0) { ::close(mount.overlay); mount.overlay = -1; }
    }
    videoout_.reset();
    user_service_.reset();
    pad_.reset();
    audio_.reset();
    graphics_.reset();
    if (dynamic_) dynamic_->reset();
    threads_.reset();
    errno_table_ = errno_table_size_ = process_param_ = heap_trace_ = 0;
    console_.clear();
    compute_.fill({});
    save_root_.clear(); save_dir_.clear(); save_dialog_status_ = msg_dialog_status_ = error_dialog_status_ = 0; sandbox_word_ = 0; system_language_ = 1; addcont_.clear(); modules_.clear();
    direct_.clear();
    started_us_ = monotonic_us();
}
int64_t PS4Libkernel::mount(const char* guest, const char* directory) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!guest || !directory || !*directory) return kernel_error(inval);
    const auto target = std::find_if(mounts_.begin(), mounts_.end(),
        [guest](const Mount& m) { return !std::strcmp(m.prefix, guest); });
    if (target == mounts_.end()) return kernel_error(inval);
    const int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return kernel_error(host_error(errno));
    close_files();
    if (target->root >= 0) ::close(target->root);
    if (target->overlay >= 0) { ::close(target->overlay); target->overlay = -1; }
    target->root = fd;
    return 0;
}
int64_t PS4Libkernel::mount_app0_update(const char* directory) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!directory || !*directory || mounts_[0].root < 0) return kernel_error(inval);
    const int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return kernel_error(host_error(errno));
    if (mounts_[0].overlay >= 0) ::close(mounts_[0].overlay);
    mounts_[0].overlay = fd;
    return 0;
}
int64_t PS4Libkernel::set_savedata_root(const char* directory) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!directory || !*directory) return kernel_error(inval);
    save_root_ = directory;
    return 0;
}
namespace {
int64_t save_error(uint32_t code) { return signed32(0x809f0000U | code); }
constexpr uint32_t save_parameter = 0, save_busy = 3, save_not_mounted = 4, save_exists = 7, save_not_found = 8;
}
// sceSaveDataMount2(const OrbisSaveDataMount2*, OrbisSaveDataMountResult*):
// userId@0, dirName pointer@8 (char[32]), blocks@16, mountMode@24; the result
// is mountPoint char[16]@0, requiredBlocks@16, mountStatus@28 (1 = created).
// Only /savedata0 exists; its directory is opened without closing the game's
// other descriptors (a remount through mount() would close them all).
int64_t PS4Libkernel::save_mount(uint64_t request, uint64_t result, bool v1) {
    constexpr uint32_t read_only = 1, read_write = 2, create = 4, create2 = 32;
    uint8_t wire[48];
    if (!request || !result || !memory_.read(request, wire, v1 ? 48 : 32)) return save_error(save_parameter);
    const uint32_t mode = uint32_t(get_le(wire + (v1 ? 40 : 24), 4));
    char name[33];
    if (!memory_.read_cstring(get_le(wire + (v1 ? 16 : 8), 8), name, sizeof(name)) || !name[0]) return save_error(save_parameter);
    for (const char* c = name; *c; ++c)
        if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '-' && *c != '_') return save_error(save_parameter);
    std::lock_guard<std::mutex> guard(lock_);
    auto& save = *std::find_if(mounts_.begin(), mounts_.end(), [](const Mount& m) { return !std::strcmp(m.prefix, "/savedata0"); });
    if (save_root_.empty()) return save_error(save_not_found);
    if (save.root >= 0) return save_error(save_busy);
    const std::string path = save_root_ + "/" + name;
    bool created = false;
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        if (!(mode & (create | create2))) return save_error(save_not_found);
        if (::mkdir(path.c_str(), 0755) != 0) return kernel_error(host_error(errno));
        created = true;
    } else if ((mode & create) && !(mode & create2)) return save_error(save_exists);
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return kernel_error(host_error(errno));
    uint8_t out[64]{};
    std::memcpy(out, "/savedata0", 10);
    put_le(out, 28, created ? 1 : 0, 4);
    if (!memory_.write(result, out, sizeof(out))) { ::close(fd); return save_error(save_parameter); }
    save.root = fd;
    save.writable = (mode & (read_write | create | create2)) != 0 || !(mode & read_only);
    save_dir_ = path;
    return 0;
}
int64_t PS4Libkernel::save_umount(uint64_t mount_point) {
    // 0 (sceSaveDataTerminate): whatever is mounted.
    char point[16] = "/savedata0";
    if (mount_point && !memory_.read_cstring(mount_point, point, sizeof(point))) return save_error(save_parameter);
    std::lock_guard<std::mutex> guard(lock_);
    auto& save = *std::find_if(mounts_.begin(), mounts_.end(), [](const Mount& m) { return !std::strcmp(m.prefix, "/savedata0"); });
    if (std::strcmp(point, "/savedata0") || save.root < 0) return save_error(save_not_mounted);
    ::close(save.root); save.root = -1;
    save_dir_.clear();
    return 0;
}
namespace {
// OrbisSaveDataParam: title[128], subTitle[128], detail[1024], u32 userParam,
// 4 reserved, time_t mtime, 32 reserved. Kept as these bytes in the save's
// sce_sys/maxps4-param.bin.
constexpr size_t save_param_size = 1328;
bool read_host(const std::string& path, std::vector<uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}
bool write_host(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return out && out.write(reinterpret_cast<const char*>(data), std::streamsize(size)).good();
}
// A DirNameSearch pattern: '%' matches any run of characters.
bool save_matches(const char* pattern, const char* name) {
    if (!*pattern) return !*name;
    if (*pattern == '%') return save_matches(pattern + 1, name) || (*name && save_matches(pattern, name + 1));
    return *name == *pattern && save_matches(pattern + 1, name + 1);
}
}
int64_t PS4Libkernel::save_service(PS4KernelService service, const PS4KernelCall& c) {
    using S = PS4KernelService;
    auto put = [&](uint64_t address, uint64_t value, size_t width) {
        uint8_t wire[8]; put_le(wire, 0, value, width); return address && memory_.write(address, wire, width);
    };
    auto mounted = [&](uint64_t point) {
        char text[16]{};
        return point && memory_.read_cstring(point, text, sizeof(text)) && !std::strcmp(text, "/savedata0") && !save_dir_.empty();
    };
    auto load_param = [&](std::vector<uint8_t>& param) {
        param.assign(save_param_size, 0);
        std::vector<uint8_t> stored;
        if (read_host(save_dir_ + "/sce_sys/maxps4-param.bin", stored) && stored.size() == save_param_size) param = stored;
    };
    switch (service) {
    case S::save_mount1: return save_mount(c.rdi, c.rsi, true);
    case S::save_umount2: return save_umount(c.rsi); // Umount2(mode, mountPoint*).
    case S::save_event: return save_error(save_not_found); // No event pending.
    case S::save_progress: return c.rdi && put(c.rdi, 0x3f800000, 4) ? 0 : save_error(save_parameter); // 1.0f.
    // GetMountInfo(mountPoint*, {u64 blocks, u64 freeBlocks}*): 32 KiB blocks, plenty free.
    case S::save_mount_info: {
        std::lock_guard<std::mutex> guard(lock_);
        if (!mounted(c.rdi)) return save_error(save_not_mounted);
        return put(c.rsi, 0x8000, 8) && put(c.rsi + 8, 0x8000, 8) ? 0 : save_error(save_parameter);
    }
    // SetParam(mountPoint*, type, buffer, size): 0 all, 1 title, 2 subtitle,
    // 3 detail, 4 user parameter; the mtime is the time of the change.
    case S::save_set_param: case S::save_get_param: {
        std::lock_guard<std::mutex> guard(lock_);
        if (!mounted(c.rdi)) return save_error(save_not_mounted);
        static const size_t offsets[] = {0, 0, 128, 256, 1280}, sizes[] = {save_param_size, 128, 128, 1024, 4};
        if (c.rsi > 4 || !c.rdx) return save_error(save_parameter);
        const size_t offset = offsets[c.rsi], size = sizes[c.rsi];
        std::vector<uint8_t> param;
        load_param(param);
        if (service == S::save_get_param) {
            const size_t count = std::min<size_t>(size, size_t(c.rcx));
            if (!memory_.write(c.rdx, param.data() + offset, count)) return save_error(save_parameter);
            return !c.r8 || put(c.r8, count, 4) ? 0 : save_error(save_parameter);
        }
        if (c.rcx < size || !memory_.read(c.rdx, param.data() + offset, size)) return save_error(save_parameter);
        put_le(param.data(), 1288, uint64_t(::time(nullptr)), 8);
        ::mkdir((save_dir_ + "/sce_sys").c_str(), 0755);
        return write_host(save_dir_ + "/sce_sys/maxps4-param.bin", param.data(), param.size()) ? 0 : kernel_error(io);
    }
    // Save/LoadIcon(mountPoint*, {void* buffer, size_t bufferSize, size_t dataSize}*): sce_sys/icon0.png.
    case S::save_save_icon: case S::save_load_icon: {
        std::lock_guard<std::mutex> guard(lock_);
        if (!mounted(c.rdi)) return save_error(save_not_mounted);
        uint8_t icon[24];
        if (!c.rsi || !memory_.read(c.rsi, icon, sizeof(icon))) return save_error(save_parameter);
        const uint64_t buffer = get_le(icon, 8), capacity = get_le(icon + 8, 8), data = get_le(icon + 16, 8);
        const std::string path = save_dir_ + "/sce_sys/icon0.png";
        if (service == S::save_save_icon) {
            if (!buffer || !data || data > capacity || data > 4 * 1024 * 1024) return save_error(save_parameter);
            std::vector<uint8_t> bytes(static_cast<size_t>(data));
            if (!memory_.read(buffer, bytes.data(), bytes.size())) return save_error(save_parameter);
            ::mkdir((save_dir_ + "/sce_sys").c_str(), 0755);
            return write_host(path, bytes.data(), bytes.size()) ? 0 : kernel_error(io);
        }
        std::vector<uint8_t> bytes;
        if (!read_host(path, bytes)) return save_error(save_not_found);
        if (!buffer || bytes.size() > capacity) return save_error(save_parameter);
        if (!memory_.write(buffer, bytes.data(), bytes.size())) return save_error(save_parameter);
        return put(c.rsi + 16, bytes.size(), 8) ? 0 : save_error(save_parameter);
    }
    // DirNameSearch(cond {userId, titleId*, dirName* pattern@16, key@24, order@28},
    // result {u32 hitNum@0, dirNames*@8, u32 dirNamesNum@16, u32 setNum@20,
    // params*@24, infos*@32}): the save directories, by name.
    case S::save_search: {
        uint8_t cond[32], result[40];
        if (!c.rdi || !c.rsi || !memory_.read(c.rdi, cond, sizeof(cond)) || !memory_.read(c.rsi, result, sizeof(result)))
            return save_error(save_parameter);
        char pattern[33] = "%";
        if (get_le(cond + 16, 8) && !memory_.read_cstring(get_le(cond + 16, 8), pattern, sizeof(pattern))) return save_error(save_parameter);
        std::lock_guard<std::mutex> guard(lock_);
        std::vector<std::string> names;
        if (!save_root_.empty()) if (DIR* dir = ::opendir(save_root_.c_str())) {
            while (const dirent* entry = ::readdir(dir)) {
                const std::string name = entry->d_name;
                if (name.empty() || name[0] == '.' || name.size() > 31 || !save_matches(pattern, name.c_str())) continue;
                struct stat info{};
                if (::stat((save_root_ + "/" + name).c_str(), &info) == 0 && S_ISDIR(info.st_mode)) names.push_back(name);
            }
            ::closedir(dir);
        }
        std::sort(names.begin(), names.end());
        if (get_le(cond + 28, 4) == 1) std::reverse(names.begin(), names.end()); // Descending.
        const uint64_t dir_names = get_le(result + 8, 8), params = get_le(result + 24, 8), infos = get_le(result + 32, 8);
        const uint32_t capacity = uint32_t(get_le(result + 16, 4));
        uint32_t set = 0;
        for (; set < names.size() && set < capacity && dir_names; ++set) {
            uint8_t name[32]{};
            std::memcpy(name, names[set].data(), names[set].size());
            if (!memory_.write(dir_names + 32ULL * set, name, sizeof(name))) return save_error(save_parameter);
            if (params) {
                std::vector<uint8_t> param(save_param_size, 0), stored;
                if (read_host(save_root_ + "/" + names[set] + "/sce_sys/maxps4-param.bin", stored) && stored.size() == save_param_size) param = stored;
                if (!memory_.write(params + uint64_t(save_param_size) * set, param.data(), param.size())) return save_error(save_parameter);
            }
            if (infos) {
                uint8_t info[48]{};
                put_le(info, 0, 0x8000, 8); put_le(info, 8, 0x8000, 8);
                if (!memory_.write(infos + 48ULL * set, info, sizeof(info))) return save_error(save_parameter);
            }
        }
        return put(c.rsi, names.size(), 4) && put(c.rsi + 20, set, 4) ? 0 : save_error(save_parameter);
    }
    // Delete({userId, titleId*, dirName*@16}*): the directory and what it holds.
    case S::save_delete: {
        uint8_t request[24];
        char name[33];
        if (!c.rdi || !memory_.read(c.rdi, request, sizeof(request)) ||
            !memory_.read_cstring(get_le(request + 16, 8), name, sizeof(name)) || !name[0]) return save_error(save_parameter);
        for (const char* ch = name; *ch; ++ch)
            if (!std::isalnum(static_cast<unsigned char>(*ch)) && *ch != '-' && *ch != '_') return save_error(save_parameter);
        std::lock_guard<std::mutex> guard(lock_);
        if (save_root_.empty()) return save_error(save_not_found);
        const std::string path = save_root_ + "/" + name;
        if (path == save_dir_) return save_error(save_busy);
        std::error_code error;
        if (!std::filesystem::is_directory(path, error)) return save_error(save_not_found);
        std::filesystem::remove_all(path, error);
        return error ? kernel_error(io) : 0;
    }
    // Save data memory (v1): SetupSaveDataMemory(userId, size, size_t* existed),
    // Get/SetSaveDataMemory(userId, buffer, size, offset): one file per game.
    case S::save_memory_setup: case S::save_memory_get: case S::save_memory_set: {
        std::lock_guard<std::mutex> guard(lock_);
        if (save_root_.empty()) return save_error(save_not_found);
        const std::string path = save_root_ + "/sdmemory.bin";
        if (service == S::save_memory_setup) {
            if (!c.rsi || c.rsi > 16 * 1024 * 1024) return save_error(save_parameter);
            struct stat info{};
            const uint64_t existed = ::stat(path.c_str(), &info) == 0 ? uint64_t(info.st_size) : 0;
            if (c.rdx && !put(c.rdx, existed, 8)) return save_error(save_parameter);
            if (existed < c.rsi) {
                const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
                if (fd < 0) return kernel_error(host_error(errno));
                const int resized = ::ftruncate(fd, off_t(c.rsi));
                ::close(fd);
                if (resized) return kernel_error(io);
            }
            return 0;
        }
        if (!c.rsi || !c.rdx || c.rdx > 16 * 1024 * 1024 || c.rcx > 16 * 1024 * 1024) return save_error(save_parameter);
        std::vector<uint8_t> bytes(static_cast<size_t>(c.rdx));
        const int fd = ::open(path.c_str(), (service == S::save_memory_set ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        if (fd < 0) return save_error(save_not_found);
        ssize_t done;
        if (service == S::save_memory_get) {
            done = ::pread(fd, bytes.data(), bytes.size(), off_t(c.rcx));
            ::close(fd);
            if (done < 0) return kernel_error(io);
            // A short read past the end leaves the rest zeroed.
            return memory_.write(c.rsi, bytes.data(), bytes.size()) ? 0 : save_error(save_parameter);
        }
        if (!memory_.read(c.rsi, bytes.data(), bytes.size())) { ::close(fd); return save_error(save_parameter); }
        done = ::pwrite(fd, bytes.data(), bytes.size(), off_t(c.rcx));
        ::close(fd);
        return done == ssize_t(bytes.size()) ? 0 : kernel_error(io);
    }
    default: return kernel_error(nosys);
    }
}
// sceAppContentTemporaryDataMount2(option, OrbisAppContentMountPoint*):
// /temp0 on a fresh directory under the host's temporary directory, opened
// without closing the game's files; the mount point string is written back.
int64_t PS4Libkernel::temp_mount(uint64_t mount_point) {
    if (!mount_point) return signed32(0x80D90002U);
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/maxps4-temp0";
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return kernel_error(host_error(errno));
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return kernel_error(host_error(errno));
    uint8_t point[16]{};
    std::memcpy(point, "/temp0", 6);
    if (!memory_.write(mount_point, point, sizeof(point))) { ::close(fd); return signed32(0x80D90002U); }
    std::lock_guard<std::mutex> guard(lock_);
    auto& temp = *std::find_if(mounts_.begin(), mounts_.end(), [](const Mount& m) { return !std::strcmp(m.prefix, "/temp0"); });
    if (temp.root >= 0) ::close(temp.root);
    temp.root = fd;
    return 0;
}
namespace {
// SHA-1 (FIPS 180-4), for the NID of a symbol name sceKernelDlsym is given.
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(data, data + size);
    const uint64_t bits = uint64_t(size) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (i * 8)));
    auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t chunk = 0; chunk < m.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(m[chunk + 4 * i]) << 24 | uint32_t(m[chunk + 4 * i + 1]) << 16 |
                   uint32_t(m[chunk + 4 * i + 2]) << 8 | uint32_t(m[chunk + 4 * i + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            const uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            const uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 4; ++j) out[size_t(4 * i + j)] = uint8_t(h[i] >> (24 - 8 * j));
    return out;
}
}
// A symbol's NID: the first 8 bytes of SHA-1(name + fixed suffix), reversed,
// in base64 with '-' for '/' and no padding (11 characters).
std::string ps4_nid(const std::string& name) {
    static const uint8_t suffix[16] = {0x51,0x8D,0x64,0xA6,0x35,0xDE,0xD8,0xC1,0xE6,0xB0,0x39,0xB1,0xC3,0xE5,0x52,0x30};
    std::vector<uint8_t> input(name.begin(), name.end());
    input.insert(input.end(), std::begin(suffix), std::end(suffix));
    const auto digest = sha1(input.data(), input.size());
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) bytes[i] = digest[size_t(7 - i)];
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string out;
    uint32_t buffer = 0; int held = 0;
    for (uint8_t byte : bytes) {
        buffer = buffer << 8 | byte; held += 8;
        while (held >= 6) { held -= 6; out += alphabet[(buffer >> held) & 63]; }
    }
    if (held) out += alphabet[(buffer << (6 - held)) & 63];
    return out;
}
int64_t PS4Libkernel::module_service(PS4KernelService service, const PS4KernelCall& c) {
    using S = PS4KernelService;
    std::lock_guard<std::mutex> guard(lock_);
    auto put = [&](uint64_t address, uint64_t value, size_t width) {
        uint8_t wire[8]; put_le(wire, 0, value, width); return address && memory_.write(address, wire, width);
    };
    switch (service) {
    // LoadStartModule(path, args, argp, flags, opt, int* result): the loaded
    // module of that file name (any directory), started at boot.
    case S::module_load_start: {
        char path[PS4KernelPath::max_path];
        if (!memory_.read_cstring(c.rdi, path, sizeof(path))) return kernel_error(fault);
        const char* slash = std::strrchr(path, '/');
        const std::string file = slash ? slash + 1 : path;
        for (size_t i = 0; i < modules_.size(); ++i) {
            const auto& name = modules_[i].name;
            const char* tail = std::strrchr(name.c_str(), '/');
            if (::strcasecmp(tail ? tail + 1 : name.c_str(), file.c_str()) != 0) continue;
            if (c.r9 && !put(c.r9, 0, 4)) return kernel_error(fault);
            return int64_t(i + 1);
        }
        return kernel_error(noent);
    }
    // Dlsym(handle, const char* symbol, void** address).
    case S::module_dlsym: {
        if (!c.rdi || c.rdi > modules_.size()) return kernel_error(srch);
        char symbol[256];
        if (!memory_.read_cstring(c.rsi, symbol, sizeof(symbol)) || !c.rdx) return kernel_error(fault);
        const auto& exports = modules_[size_t(c.rdi - 1)].exports;
        const auto found = exports.find(ps4_nid(symbol));
        if (found == exports.end()) return kernel_error(srch);
        return put(c.rdx, found->second, 8) ? 0 : kernel_error(fault);
    }
    // GetModuleList(int* handles, size_t capacity, size_t* count).
    case S::module_list: {
        const size_t n = std::min<size_t>(modules_.size(), c.rsi);
        for (size_t i = 0; i < n; ++i) if (!put(c.rdi + 4 * i, i + 1, 4)) return kernel_error(fault);
        return put(c.rdx, modules_.size(), 8) ? 0 : kernel_error(fault);
    }
    // GetModuleInfoFromAddr(address, flags, OrbisKernelModuleInfoEx*) and
    // GetModuleInfoInternal(handle, Ex*): 0x1a8 bytes; GetModuleInfo(handle,
    // OrbisKernelModuleInfo*): 0x160 bytes. Name, handle, eh_frame, segments.
    case S::module_info: case S::module_info_ex: {
        const bool by_address = service == S::module_info_ex && c.rdi > modules_.size();
        size_t index = SIZE_MAX;
        if (by_address) { for (size_t i = 0; i < modules_.size(); ++i) if (c.rdi >= modules_[i].base && c.rdi < modules_[i].end) index = i; }
        else if (c.rdi && c.rdi <= modules_.size()) index = size_t(c.rdi - 1);
        const uint64_t out = by_address ? c.rdx : c.rsi;
        if (index == SIZE_MAX || !out) return kernel_error(srch);
        const LoadedModule& m = modules_[index];
        uint64_t eh_frame = 0, eh_size = 0;
        eh_frame_of(m, eh_frame, eh_size);
        const char* tail = std::strrchr(m.name.c_str(), '/');
        const char* name = tail ? tail + 1 : m.name.c_str();
        const size_t segments = std::min<size_t>(m.segments.size(), 4);
        if (service == S::module_info) {
            uint8_t info[0x160]{};
            put_le(info, 0, sizeof(info), 8);
            std::memcpy(info + 8, name, std::min<size_t>(std::strlen(name), 255));
            for (size_t s = 0; s < segments; ++s) {
                put_le(info, 264 + 16 * s, m.segments[s].address, 8);
                put_le(info, 272 + 16 * s, m.segments[s].size, 4);
                put_le(info, 276 + 16 * s, m.segments[s].protection, 4);
            }
            put_le(info, 328, segments, 4);
            return memory_.write(out, info, sizeof(info)) ? 0 : kernel_error(fault);
        }
        uint8_t info[0x1a8]{};
        put_le(info, 0, sizeof(info), 8);
        std::memcpy(info + 8, name, std::min<size_t>(std::strlen(name), 255));
        put_le(info, 264, index + 1, 4);
        put_le(info, 328, m.eh_frame_hdr, 8); put_le(info, 336, eh_frame, 8);
        put_le(info, 344, m.eh_frame_hdr_size, 4); put_le(info, 348, eh_size, 4);
        for (size_t s = 0; s < segments; ++s) {
            put_le(info, 352 + 16 * s, m.segments[s].address, 8);
            put_le(info, 360 + 16 * s, m.segments[s].size, 4);
            put_le(info, 364 + 16 * s, m.segments[s].protection, 4);
        }
        put_le(info, 416, segments, 4); put_le(info, 420, 1, 4); // One reference.
        return memory_.write(out, info, sizeof(info)) ? 0 : kernel_error(fault);
    }
    // GetModuleInfoForUnwind(address, flags, info*): u64 size (0x130), name
    // char[256], eh_frame_hdr, eh_frame, eh_frame size, first segment and size.
    case S::module_info_unwind: {
        const LoadedModule* found = nullptr;
        for (const auto& m : modules_) if (c.rdi >= m.base && c.rdi < m.end) found = &m;
        if (!found || !c.rdx) return kernel_error(srch);
        uint64_t eh_frame = 0, eh_size = 0;
        eh_frame_of(*found, eh_frame, eh_size);
        uint8_t info[0x130]{};
        put_le(info, 0, sizeof(info), 8);
        const char* tail = std::strrchr(found->name.c_str(), '/');
        const char* name = tail ? tail + 1 : found->name.c_str();
        std::memcpy(info + 8, name, std::min<size_t>(std::strlen(name), 255));
        put_le(info, 264, found->eh_frame_hdr, 8);
        put_le(info, 272, eh_frame, 8);
        put_le(info, 280, eh_size, 8);
        put_le(info, 288, found->base, 8);
        put_le(info, 296, found->end - found->base, 8);
        return memory_.write(c.rdx, info, sizeof(info)) ? 0 : kernel_error(fault);
    }
    default: return kernel_error(nosys);
    }
}
// A module's eh_frame, from its PT_GNU_EH_FRAME header, and its size.
void PS4Libkernel::eh_frame_of(const LoadedModule& module, uint64_t& eh_frame, uint64_t& eh_size) const {
    const LoadedModule* found = &module;
    eh_frame = eh_size = 0;
    {
        uint8_t hdr[12]{};
        if (found->eh_frame_hdr && memory_.read(found->eh_frame_hdr, hdr, sizeof(hdr)) && hdr[0] == 1) {
            // eh_frame_ptr, encoded by hdr[1]: (pc-relative 0x10 |) udata4 3,
            // sdata4 0x0b, udata8 4 or sdata8 0x0c.
            const uint8_t encoding = hdr[1];
            const uint64_t field = found->eh_frame_hdr + 4;
            int64_t value = 0;
            switch (encoding & 0x0f) {
            case 0x03: value = int64_t(get_le(hdr + 4, 4)); break;
            case 0x0b: value = int64_t(int32_t(get_le(hdr + 4, 4))); break;
            case 0x04: case 0x0c: value = int64_t(get_le(hdr + 4, 8)); break;
            default: break;
            }
            if ((encoding & 0x70) == 0x10) eh_frame = uint64_t(int64_t(field) + value);
            else if ((encoding & 0x70) == 0) eh_frame = uint64_t(value);
            // Its size: CIE/FDE records up to the zero terminator, in the module.
            for (uint64_t at = eh_frame; eh_frame && at + 4 <= found->end;) {
                uint8_t word[12];
                if (!memory_.read(at, word, 4)) break;
                uint64_t length = get_le(word, 4), header = 4;
                if (length == 0) { eh_size = at + 4 - eh_frame; break; }
                if (length == 0xffffffffULL) {
                    if (!memory_.read(at + 4, word + 4, 8)) break;
                    length = get_le(word + 4, 8); header = 12;
                }
                if (length > found->end - at) break;
                at += header + length;
            }
        }
    }
}
int64_t PS4Libkernel::add_addcont(const char* label, const char* directory) {
    if (!label || std::strlen(label) != 16 || !directory || !*directory) return kernel_error(inval);
    std::lock_guard<std::mutex> guard(lock_);
    addcont_.push_back({label, directory});
    return 0;
}
const PS4Libkernel::Addcont* PS4Libkernel::find_addcont(uint64_t label) const {
    char text[17]{};
    if (!label || !memory_.read(label, text, 16)) return nullptr;
    for (const auto& add : addcont_) if (add.label == text) return &add;
    return nullptr;
}
// OrbisAppContentAddcontInfo: entitlement label char[17] + 3 padding, u32
// status (4: installed). GetAddcontInfoList(serviceLabel, list, listNum, hitNum*).
int64_t PS4Libkernel::addcont_list(uint64_t list, uint32_t count, uint64_t hit) {
    constexpr int64_t parameter = int32_t(0x80D90002U);
    std::lock_guard<std::mutex> guard(lock_);
    if (!hit) return parameter;
    uint32_t written = 0;
    if (list) for (const auto& add : addcont_) {
        if (written == count) break;
        uint8_t entry[24]{};
        std::memcpy(entry, add.label.data(), 16);
        put_le(entry, 20, 4, 4);
        if (!memory_.write(list + uint64_t(written) * 24, entry, sizeof(entry))) return parameter;
        ++written;
    }
    uint8_t total[4]; put_le(total, 0, list ? written : uint32_t(addcont_.size()), 4);
    return memory_.write(hit, total, 4) ? 0 : parameter;
}
int64_t PS4Libkernel::addcont_info(uint64_t label, uint64_t info) {
    std::lock_guard<std::mutex> guard(lock_);
    const Addcont* add = find_addcont(label);
    if (!add) return int32_t(0x80D90005U); // Not found.
    uint8_t entry[24]{};
    std::memcpy(entry, add->label.data(), 16);
    put_le(entry, 20, 4, 4);
    return info && memory_.write(info, entry, sizeof(entry)) ? 0 : int32_t(0x80D90002U);
}
// AddcontMount(serviceLabel, label*, mountPoint*): the add-on's directory at
// /addcont0 (a later mount replaces it), opened without closing other files.
int64_t PS4Libkernel::addcont_mount(uint64_t label, uint64_t mount_point) {
    std::string path;
    {
        std::lock_guard<std::mutex> guard(lock_);
        const Addcont* add = find_addcont(label);
        if (!add) return int32_t(0x80D90005U);
        path = add->path;
    }
    if (!mount_point) return int32_t(0x80D90002U);
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return kernel_error(host_error(errno));
    uint8_t point[16]{};
    std::memcpy(point, "/addcont0", 9);
    if (!memory_.write(mount_point, point, sizeof(point))) { ::close(fd); return int32_t(0x80D90002U); }
    std::lock_guard<std::mutex> guard(lock_);
    auto& mount = *std::find_if(mounts_.begin(), mounts_.end(), [](const Mount& m) { return !std::strcmp(m.prefix, "/addcont0"); });
    if (mount.root >= 0) ::close(mount.root);
    mount.root = fd;
    return 0;
}
int64_t PS4Libkernel::addcont_unmount(uint64_t mount_point) {
    char point[16]{};
    if (!mount_point || !memory_.read_cstring(mount_point, point, sizeof(point))) return int32_t(0x80D90002U);
    std::lock_guard<std::mutex> guard(lock_);
    auto& mount = *std::find_if(mounts_.begin(), mounts_.end(), [](const Mount& m) { return !std::strcmp(m.prefix, "/addcont0"); });
    if (std::strcmp(point, "/addcont0") || mount.root < 0) return int32_t(0x80D90002U);
    ::close(mount.root); mount.root = -1;
    return 0;
}
namespace {
// Days from 1970-01-01 to a civil date and back (H. Hinnant's algorithms).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = int64_t(yoe) + era * 400 + (m <= 2);
}
constexpr uint64_t unix_epoch_tick = 62135596800ULL * 1000000ULL; // 0001-01-01 to 1970-01-01.
int64_t local_offset_us() {
    const time_t now = ::time(nullptr);
    tm local{};
    return ::localtime_r(&now, &local) ? int64_t(local.tm_gmtoff) * 1000000 : 0;
}
}
// OrbisRtcDateTime: u16 year, month, day, hour, minute, second; u32 microsecond.
int64_t PS4Libkernel::rtc(PS4KernelService service, const PS4KernelCall& c) {
    using S = PS4KernelService;
    constexpr int64_t invalid = int32_t(0x80B50002U), bad_pointer = int32_t(0x80B50001U);
    auto now_tick = [] {
        timespec t{};
        ::clock_gettime(CLOCK_REALTIME, &t);
        return unix_epoch_tick + uint64_t(t.tv_sec) * 1000000 + uint64_t(t.tv_nsec) / 1000;
    };
    auto put_tick = [&](uint64_t address, uint64_t tick) {
        uint8_t wire[8]; put_le(wire, 0, tick, 8); return address && memory_.write(address, wire, 8);
    };
    auto get_tick = [&](uint64_t address, uint64_t& tick) {
        uint8_t wire[8]; if (!address || !memory_.read(address, wire, 8)) return false;
        tick = get_le(wire, 8); return true;
    };
    auto put_date = [&](uint64_t address, uint64_t tick) {
        if (!address) return false;
        const int64_t micros = int64_t(tick) - int64_t(unix_epoch_tick);
        int64_t days = micros / 86400000000LL, rest = micros % 86400000000LL;
        if (rest < 0) { rest += 86400000000LL; --days; }
        int64_t y; unsigned m, d;
        civil_from_days(days, y, m, d);
        uint8_t wire[16]{};
        const uint64_t seconds = uint64_t(rest) / 1000000;
        put_le(wire, 0, uint64_t(y), 2); put_le(wire, 2, m, 2); put_le(wire, 4, d, 2);
        put_le(wire, 6, seconds / 3600, 2); put_le(wire, 8, seconds / 60 % 60, 2); put_le(wire, 10, seconds % 60, 2);
        put_le(wire, 12, uint64_t(rest) % 1000000, 4);
        return memory_.write(address, wire, sizeof(wire));
    };
    switch (service) {
    case S::rtc_current_tick: return put_tick(c.rdi, now_tick()) ? 0 : bad_pointer;
    // GetCurrentClock(dateTime*, timeZone minutes).
    case S::rtc_current_clock:
        return put_date(c.rdi, now_tick() + uint64_t(int64_t(signed32(c.rsi)) * 60000000)) ? 0 : bad_pointer;
    case S::rtc_current_local: return put_date(c.rdi, now_tick() + uint64_t(local_offset_us())) ? 0 : bad_pointer;
    // GetTick(const dateTime*, tick*).
    case S::rtc_get_tick: {
        uint8_t wire[16];
        if (!c.rdi || !c.rsi || !memory_.read(c.rdi, wire, sizeof(wire))) return bad_pointer;
        const unsigned year = unsigned(get_le(wire, 2)), month = unsigned(get_le(wire + 2, 2)), day = unsigned(get_le(wire + 4, 2));
        const uint64_t hour = get_le(wire + 6, 2), minute = get_le(wire + 8, 2), second = get_le(wire + 10, 2), micro = get_le(wire + 12, 4);
        if (!year || year > 9999 || !month || month > 12 || !day || day > 31 || hour > 23 || minute > 59 || second > 59 || micro > 999999)
            return invalid;
        const int64_t days = days_from_civil(year, month, day);
        const uint64_t tick = uint64_t(int64_t(unix_epoch_tick) + days * 86400000000LL) + ((hour * 60 + minute) * 60 + second) * 1000000 + micro;
        return put_tick(c.rsi, tick) ? 0 : bad_pointer;
    }
    // GetTime_t(const dateTime*, time_t*): seconds since 1970 (UTC).
    case S::rtc_time_t: {
        uint8_t wire[16];
        if (!c.rdi || !c.rsi || !memory_.read(c.rdi, wire, sizeof(wire))) return bad_pointer;
        const unsigned year = unsigned(get_le(wire, 2)), month = unsigned(get_le(wire + 2, 2)), day = unsigned(get_le(wire + 4, 2));
        const int64_t hour = int64_t(get_le(wire + 6, 2)), minute = int64_t(get_le(wire + 8, 2)), second = int64_t(get_le(wire + 10, 2));
        if (!year || year > 9999 || !month || month > 12 || !day || day > 31 || hour > 23 || minute > 59 || second > 59)
            return invalid;
        const int64_t seconds = days_from_civil(year, month, day) * 86400 + (hour * 60 + minute) * 60 + second;
        uint8_t out[8]; put_le(out, 0, uint64_t(seconds), 8);
        return memory_.write(c.rsi, out, 8) ? 0 : bad_pointer;
    }
    // SetTick(dateTime*, const tick*).
    case S::rtc_set_tick: {
        uint64_t tick = 0;
        if (!get_tick(c.rsi, tick)) return bad_pointer;
        return put_date(c.rdi, tick) ? 0 : bad_pointer;
    }
    case S::rtc_resolution: return 1000000;
    // ConvertUtcToLocalTime(const utc tick*, local tick*).
    case S::rtc_utc_to_local: {
        uint64_t tick = 0;
        if (!get_tick(c.rdi, tick)) return bad_pointer;
        return put_tick(c.rsi, tick + uint64_t(local_offset_us())) ? 0 : bad_pointer;
    }
    default: return kernel_error(nosys);
    }
}
Error PS4Libkernel::resolve(uint64_t address, size_t& mount, char* relative) const {
    char path[PS4KernelPath::max_path];
    if (!memory_.read_cstring(address, path, sizeof(path))) return fault;
    // Absolute virtual paths only; no "." or ".." components, no "//", no
    // control characters or backslashes, no trailing "/" (except a mount root).
    if (path[0] != '/') return inval;
    size_t length = std::strlen(path);
    for (size_t i = 1; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(path[i]);
        if (byte < 0x20 || byte == 0x7f || byte == '\\') return inval;
        if (path[i] == '/' && path[i - 1] == '/') return inval;
        if (path[i - 1] == '/' && path[i] == '.' &&
            (path[i + 1] == '/' || !path[i + 1] || (path[i + 1] == '.' && (path[i + 2] == '/' || !path[i + 2]))))
            return inval;
    }
    for (size_t m = 0; m < mounts_.size(); ++m) {
        const size_t n = std::strlen(mounts_[m].prefix);
        if (std::strncmp(path, mounts_[m].prefix, n) || (path[n] && path[n] != '/')) continue;
        const char* rest = path + n + (path[n] == '/' ? 1 : 0);
        if (*rest && rest[std::strlen(rest) - 1] == '/') return inval;
        mount = m;
        std::strcpy(relative, rest);
        return mounts_[m].root < 0 ? noent : ok;
    }
    return length > 1 && path[length - 1] == '/' ? inval : noent;
}
int PS4Libkernel::parent(size_t mount, const char* relative, char* leaf, Error& error) const {
    // An update overlay answers for the paths it holds (not the mount root).
    if (mounts_[mount].overlay >= 0 && *relative) {
        Error missing = ok;
        HostFD over(parent_from(mounts_[mount].overlay, relative, leaf, missing));
        struct stat info{};
        if (over.fd >= 0 && ::fstatat(over.fd, leaf, &info, AT_SYMLINK_NOFOLLOW) == 0) {
            error = ok;
            const int result = over.fd; over.fd = -1;
            return result;
        }
    }
    return parent_from(mounts_[mount].root, relative, leaf, error);
}
// The PS4's file system ignores case and the iPhone's does not: a name that
// does not exist is looked up again among the directory's entries, ignoring
// ASCII case (Mortal Kombat: Legacy Kollection opens /app0/video/... and
// /app0/properties/... in lower case). The entry found replaces the name in
// place (same length); the caller still opens it without following links.
static void fold_case(int directory, char* name) {
    struct stat info{};
    if (!*name || ::fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) return;
    const int listing = ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (listing < 0) return;
    DIR* entries = ::fdopendir(listing);
    if (!entries) { ::close(listing); return; }
    const size_t length = std::strlen(name);
    while (const dirent* entry = ::readdir(entries)) {
        if (std::strlen(entry->d_name) == length && ::strncasecmp(entry->d_name, name, length) == 0) {
            std::memcpy(name, entry->d_name, length);
            break;
        }
    }
    ::closedir(entries);
}
int PS4Libkernel::parent_from(int root, const char* relative, char* leaf, Error& error) const {
    // Walk from a held directory descriptor; reject symlinks at EVERY component.
    // Do not canonicalize then reopen a host path (a race can escape the mount).
    HostFD directory(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    if (directory.fd < 0) { error = host_error(errno); return -1; }
    char path[PS4KernelPath::max_path];
    std::strcpy(path, *relative ? relative : ".");
    char* component = path;
    while (char* separator = std::strchr(component, '/')) {
        *separator = '\0';
        fold_case(directory.fd, component);
        const int next = ::openat(directory.fd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) { error = host_error(errno); return -1; }
        ::close(directory.fd); directory.fd = next;
        component = separator + 1;
    }
    fold_case(directory.fd, component);
    std::strcpy(leaf, component);
    error = ok;
    const int result = directory.fd;
    directory.fd = -1;
    return result;
}
bool PS4Libkernel::writable(uint64_t address, size_t size) const {
    return address && memory_.permitted(address, size, 2) && !memory_.permitted(address, size, 1);
}
PS4Libkernel::File* PS4Libkernel::file(int32_t fd) {
    return fd >= 3 && size_t(fd) < files_.size() && files_[fd].fd >= 0 ? &files_[fd] : nullptr;
}
int64_t PS4Libkernel::open(const PS4KernelCall& c) {
    // FreeBSD flags: access mode (0/1/2), APPEND 8, SYNC 0x80, CREAT 0x200,
    // TRUNC 0x400, EXCL 0x800, DIRECTORY 0x20000.
    const uint32_t flags = uint32_t(c.rsi), access = flags & 3U;
    if (access == 3U) return kernel_error(inval);
    const bool writing = access != 0 || (flags & (0x8U | 0x200U | 0x400U));
    size_t mount = 0;
    char relative[PS4KernelPath::max_path], leaf[PS4KernelPath::max_path];
    Error e = resolve(c.rdi, mount, relative);
    if (e) return kernel_error(e);
    if (writing && !mounts_[mount].writable) return kernel_error(rofs);
    if (flags & ~(3U | 0x8U | 0x80U | 0x200U | 0x400U | 0x800U | 0x20000U)) return kernel_error(notsup);
    size_t slot = 3; // Standard streams are not host stdin/stdout/stderr aliases.
    while (slot < files_.size() && files_[slot].fd >= 0) ++slot;
    if (slot == files_.size()) return kernel_error(mfile);
    HostFD directory(parent(mount, relative, leaf, e));
    if (directory.fd < 0) return kernel_error(e);
    // NONBLOCK avoids blocking on FIFOs/devices before fstat rejects them.
    const int host_flags = (access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR) |
        ((flags & 0x8U) ? O_APPEND : 0) | ((flags & 0x80U) ? O_SYNC : 0) |
        ((flags & 0x200U) ? O_CREAT : 0) | ((flags & 0x400U) ? O_TRUNC : 0) |
        ((flags & 0x800U) ? O_EXCL : 0) | ((flags & 0x20000U) ? O_DIRECTORY : 0) |
        O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    const int fd = ::openat(directory.fd, leaf, host_flags, 0666);
    if (fd < 0) return kernel_error(host_error(errno));
    struct ::stat info{};
    if (::fstat(fd, &info) != 0) { const Error error = host_error(errno); ::close(fd); return kernel_error(error); }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) { ::close(fd); return kernel_error(notsup); }
    files_[slot] = File{fd, access != 1, access != 0, S_ISDIR(info.st_mode), false, !mounts_[mount].writable, {}, 0};
    return int64_t(slot);
}
int64_t PS4Libkernel::close(int32_t fd) {
    File* f = file(fd);
    if (!f) return kernel_error(badf);
    const int host = f->fd;
    *f = File{};
    // Never retry close after EINTR: the host descriptor may already be released.
    return ::close(host) == 0 ? 0 : kernel_error(host_error(errno));
}
int64_t PS4Libkernel::read(const PS4KernelCall& c) {
    File* f = file(signed32(c.rdi));
    if (!f || !f->readable) return kernel_error(badf);
    if (c.rdx > transfer_limit) return kernel_error(inval);
    const size_t count = size_t(c.rdx);
    if (count && !writable(c.rsi, count)) return kernel_error(fault);
    if (f->directory) return kernel_error(isdir);
    if (!count) return 0;
    std::vector<uint8_t> buffer(count); // Allocate before advancing file offset.
    ssize_t got;
    do { got = ::read(f->fd, buffer.data(), count); } while (got < 0 && errno == EINTR);
    if (got < 0) return kernel_error(host_error(errno));
    if (got && !memory_.write(c.rsi, buffer.data(), size_t(got))) return kernel_error(fault);
    return got;
}
int64_t PS4Libkernel::lseek(const PS4KernelCall& c) {
    File* f = file(signed32(c.rdi));
    if (!f) return kernel_error(badf);
    int whence;
    switch (uint32_t(c.rdx)) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default: return kernel_error(inval);
    }
    if (f->directory) {
        // A directory's offset is its position in the dirent records.
        const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(f->position) : int64_t(f->entries.size());
        const int64_t offset = signed64(c.rsi);
        if (offset > INT64_MAX - base) return kernel_error(overflow);
        if (offset < -base) return kernel_error(inval);
        const int64_t target = base + offset;
        if (target % 512) return kernel_error(inval);
        f->position = uint64_t(target);
        return target;
    }
    static_assert(sizeof(off_t) == 8, "PS4 requires 64-bit file offsets");
    const off_t result = ::lseek(f->fd, signed64(c.rsi), whence);
    return result < 0 ? kernel_error(host_error(errno)) : int64_t(result);
}
int64_t PS4Libkernel::stat(const PS4KernelCall& c, bool by_descriptor) {
    if (!writable(c.rsi, stat_size)) return kernel_error(fault);
    struct ::stat info{};
    bool read_only = true;
    if (by_descriptor) {
        File* f = file(signed32(c.rdi));
        if (!f) return kernel_error(badf);
        if (::fstat(f->fd, &info) != 0) return kernel_error(host_error(errno));
        read_only = f->read_only_mount;
    } else {
        size_t mount = 0;
        char relative[PS4KernelPath::max_path], leaf[PS4KernelPath::max_path];
        Error e = resolve(c.rdi, mount, relative);
        if (e) return kernel_error(e);
        HostFD directory(parent(mount, relative, leaf, e));
        if (directory.fd < 0) return kernel_error(e);
        if (::fstatat(directory.fd, leaf, &info, AT_SYMLINK_NOFOLLOW) != 0) return kernel_error(host_error(errno));
        if (S_ISLNK(info.st_mode)) return kernel_error(loop);
        if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) return kernel_error(notsup);
        read_only = !mounts_[mount].writable;
    }
    uint8_t wire[stat_size] = {};
    // OrbisKernelStat is 120 bytes; encode fields explicitly, never copy host stat.
    put_le(wire, 0, info.st_dev, 4); put_le(wire, 4, info.st_ino, 4);
    const uint16_t mode = (S_ISDIR(info.st_mode) ? 0040000 : 0100000) |
        (uint16_t(info.st_mode) & 0777U & (read_only ? ~0222U : 0777U));
    put_le(wire, 8, mode, 2); put_le(wire, 10, info.st_nlink, 2);
    put_le(wire, 12, info.st_uid, 4); put_le(wire, 16, info.st_gid, 4);
#if defined(__APPLE__)
    const auto at = info.st_atimespec, mt = info.st_mtimespec, ct = info.st_ctimespec;
    put_le(wire, 104, info.st_birthtimespec.tv_sec, 8);
    put_le(wire, 112, info.st_birthtimespec.tv_nsec, 8);
#else
    const auto at = info.st_atim, mt = info.st_mtim, ct = info.st_ctim;
#endif
    put_le(wire, 24, at.tv_sec, 8); put_le(wire, 32, at.tv_nsec, 8);
    put_le(wire, 40, mt.tv_sec, 8); put_le(wire, 48, mt.tv_nsec, 8);
    put_le(wire, 56, ct.tv_sec, 8); put_le(wire, 64, ct.tv_nsec, 8);
    put_le(wire, 72, info.st_size, 8); put_le(wire, 80, info.st_blocks, 8);
    put_le(wire, 88, info.st_blksize, 4);
    return memory_.write(c.rsi, wire, sizeof(wire)) ? 0 : kernel_error(fault);
}
int64_t PS4Libkernel::file_service(PS4KernelService service, const PS4KernelCall& c) {
    using S = PS4KernelService;
    switch (service) {
    // write(fd, buffer, count) and pwrite(fd, buffer, count, offset). The
    // game's stdout and stderr (1, 2) go to the engine's standard error.
    case S::write: case S::pwrite: {
        if (c.rdx > transfer_limit) return kernel_error(inval);
        if (service == S::pwrite && signed64(c.rcx) < 0) return kernel_error(inval);
        const size_t count = size_t(c.rdx);
        std::vector<uint8_t> buffer(count);
        if (count && !memory_.read(c.rsi, buffer.data(), count)) return kernel_error(fault);
        const int32_t fd = signed32(c.rdi);
        if ((fd == 1 || fd == 2) && service == S::write) {
            if (count) std::fwrite(buffer.data(), 1, count, stderr);
            // The last bytes, for the session report: a game's own message
            // usually says why it stops.
            console_.insert(console_.end(), buffer.begin(), buffer.begin() + std::ptrdiff_t(count));
            if (console_.size() > console_limit) console_.erase(console_.begin(), console_.end() - std::ptrdiff_t(console_limit));
            return int64_t(count);
        }
        File* f = file(fd);
        if (!f || !f->writable) return kernel_error(badf);
        if (f->directory) return kernel_error(isdir);
        ssize_t put;
        do {
            put = service == S::write ? ::write(f->fd, buffer.data(), count) :
                ::pwrite(f->fd, buffer.data(), count, off_t(signed64(c.rcx)));
        } while (put < 0 && errno == EINTR);
        return put < 0 ? kernel_error(host_error(errno)) : int64_t(put);
    }
    // pread(fd, buffer, count, offset).
    case S::pread: {
        File* f = file(signed32(c.rdi));
        if (!f || !f->readable) return kernel_error(badf);
        if (c.rdx > transfer_limit || signed64(c.rcx) < 0) return kernel_error(inval);
        const size_t count = size_t(c.rdx);
        if (count && !writable(c.rsi, count)) return kernel_error(fault);
        if (f->directory) return kernel_error(isdir);
        std::vector<uint8_t> buffer(count);
        ssize_t got;
        do { got = ::pread(f->fd, buffer.data(), count, off_t(signed64(c.rcx))); } while (got < 0 && errno == EINTR);
        if (got < 0) return kernel_error(host_error(errno));
        if (got && !memory_.write(c.rsi, buffer.data(), size_t(got))) return kernel_error(fault);
        return got;
    }
    case S::fsync: {
        File* f = file(signed32(c.rdi));
        if (!f) return kernel_error(badf);
        return ::fsync(f->fd) == 0 ? 0 : kernel_error(host_error(errno));
    }
    case S::ftruncate: {
        File* f = file(signed32(c.rdi));
        if (!f || !f->writable) return kernel_error(badf);
        if (signed64(c.rsi) < 0) return kernel_error(inval);
        return ::ftruncate(f->fd, off_t(c.rsi)) == 0 ? 0 : kernel_error(host_error(errno));
    }
    // Path operations, on writable mounts only.
    case S::truncate: case S::mkdir: case S::rmdir: case S::unlink: case S::rename: {
        size_t mount = 0, other = 0;
        char relative[PS4KernelPath::max_path], leaf[PS4KernelPath::max_path];
        char relative2[PS4KernelPath::max_path], leaf2[PS4KernelPath::max_path];
        Error e = resolve(c.rdi, mount, relative);
        if (e) return kernel_error(e);
        if (!mounts_[mount].writable) return kernel_error(rofs);
        if (!*relative && service != S::truncate) return kernel_error(service == S::mkdir ? exist : busy);
        HostFD directory(parent(mount, relative, leaf, e));
        if (directory.fd < 0) return kernel_error(e);
        int result = -1;
        switch (service) {
        case S::truncate: {
            if (signed64(c.rsi) < 0) return kernel_error(inval);
            HostFD target(::openat(directory.fd, leaf, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
            if (target.fd < 0) return kernel_error(host_error(errno));
            struct ::stat info{};
            if (::fstat(target.fd, &info) != 0) return kernel_error(host_error(errno));
            if (!S_ISREG(info.st_mode)) return kernel_error(notsup);
            result = ::ftruncate(target.fd, off_t(c.rsi));
            break;
        }
        case S::mkdir: result = ::mkdirat(directory.fd, leaf, 0777); break;
        case S::rmdir: result = ::unlinkat(directory.fd, leaf, AT_REMOVEDIR); break;
        case S::unlink: result = ::unlinkat(directory.fd, leaf, 0); break;
        default: { // rename(from, to): both in the same writable mount.
            e = resolve(c.rsi, other, relative2);
            if (e) return kernel_error(e);
            if (other != mount) return kernel_error(xdev);
            if (!*relative2) return kernel_error(busy);
            HostFD directory2(parent(other, relative2, leaf2, e));
            if (directory2.fd < 0) return kernel_error(e);
            result = ::renameat(directory.fd, leaf, directory2.fd, leaf2);
        }
        }
        return result == 0 ? 0 : kernel_error(host_error(errno));
    }
    // getdirentries(fd, buffer, bytes, s64* base): whole 512-byte blocks of
    // FreeBSD dirent records {u32 fileno; u16 reclen; u8 type; u8 namlen;
    // name}, each record 4-byte aligned and the last of a block stretched to
    // its end (as shadPS4's NormalDirectory).
    case S::getdirentries: {
        File* f = file(signed32(c.rdi));
        if (!f) return kernel_error(badf);
        if (!f->directory) return kernel_error(inval);
        if (c.rdx < 512 || c.rdx > transfer_limit) return kernel_error(inval);
        if (c.rcx && !writable(c.rcx, 8)) return kernel_error(fault);
        if (!f->listed) {
            const int copy = ::fcntl(f->fd, F_DUPFD_CLOEXEC, 0);
            if (copy < 0) return kernel_error(host_error(errno));
            struct Listing {
                DIR* value;
                ~Listing() { if (value) ::closedir(value); }
            } listing{::fdopendir(copy)};
            if (!listing.value) { const Error error = host_error(errno); ::close(copy); return kernel_error(error); }
            ::rewinddir(listing.value);
            std::vector<uint8_t> entries;
            size_t offset = 0, ceiling = 0, last = 0;
            uint32_t number = 1;
            for (;;) {
                errno = 0;
                const dirent* entry = ::readdir(listing.value);
                if (!entry) {
                    if (errno) return kernel_error(host_error(errno));
                    break;
                }
                const size_t length = std::strlen(entry->d_name);
                if (length > 255) continue;
                const size_t record = (8 + length + 1 + 3) & ~size_t{3};
                if (offset + record > ceiling) {
                    if (ceiling) put_le(entries.data(), last + 4, get_le(entries.data() + last + 4, 2) + (ceiling - offset), 2);
                    offset = ceiling;
                    ceiling += 512;
                    entries.resize(ceiling, 0);
                }
                last = offset;
                put_le(entries.data(), offset, number++, 4);
                put_le(entries.data(), offset + 4, record, 2);
                entries[offset + 6] = entry->d_type == DT_DIR ? 4 : entry->d_type == DT_REG ? 8 :
                    entry->d_type == DT_LNK ? 10 : 0; // FreeBSD DT_DIR / DT_REG / DT_LNK / DT_UNKNOWN.
                entries[offset + 7] = uint8_t(length);
                std::memcpy(entries.data() + offset + 8, entry->d_name, length);
                offset += record;
            }
            if (ceiling) put_le(entries.data(), last + 4, get_le(entries.data() + last + 4, 2) + (ceiling - offset), 2);
            f->entries = std::move(entries);
            f->listed = true;
        }
        const uint64_t start = std::min<uint64_t>(f->position, f->entries.size());
        const uint64_t count = std::min<uint64_t>(c.rdx & ~uint64_t{511}, f->entries.size() - start);
        if (count && !writable(c.rsi, size_t(count))) return kernel_error(fault);
        if (c.rcx) {
            uint8_t wire[8];
            put_le(wire, 0, start, 8);
            if (!memory_.write(c.rcx, wire, 8)) return kernel_error(fault);
        }
        if (count && !memory_.write(c.rsi, f->entries.data() + start, size_t(count))) return kernel_error(fault);
        f->position = start + count;
        return int64_t(count);
    }
    default: return kernel_error(nosys);
    }
}
int64_t PS4Libkernel::mmap(const PS4KernelCall& c) {
    if (!dynamic_) return kernel_error(nosys);
    // The seventh System V argument is at entry RSP+8, after return address.
    uint8_t pointer[8];
    if (!c.rsp || c.rsp > UINT64_MAX - 8 || !memory_.read(c.rsp + 8, pointer, 8))
        return kernel_error(fault);
    const uint64_t output = get_le(pointer, 8);
    if (!writable(output, 8)) return kernel_error(fault);
    uint64_t mapped = 0;
    const Error e = dynamic_->map(c.rdi, c.rsi, uint32_t(c.rdx), uint32_t(c.rcx),
                                 signed32(c.r8), signed64(c.r9), mapped);
    if (e) return kernel_error(e);
    put_le(pointer, 0, mapped, 8);
    if (!memory_.write(output, pointer, 8)) {
        dynamic_->unmap(mapped, c.rsi);
        return kernel_error(fault);
    }
    return 0;
}
int64_t PS4Libkernel::clock(const PS4KernelCall& c, bool resolution) {
    clockid_t id;
    switch (uint32_t(c.rdi)) {
    case 0: id = CLOCK_REALTIME; break;
    case 4: id = CLOCK_MONOTONIC; break;
    default: return kernel_error(inval);
    }
    if (!writable(c.rsi, 16)) return kernel_error(fault);
    timespec time{};
    const int result = resolution ? ::clock_getres(id, &time) : ::clock_gettime(id, &time);
    if (result != 0) return kernel_error(host_error(errno));
    uint8_t wire[16]{};
    put_le(wire, 0, time.tv_sec, 8); put_le(wire, 8, time.tv_nsec, 8);
    return memory_.write(c.rsi, wire, sizeof(wire)) ? 0 : kernel_error(fault);
}
int64_t PS4Libkernel::gettimeofday(uint64_t output) {
    if (!writable(output, 16)) return kernel_error(fault);
    timeval time{};
    if (::gettimeofday(&time, nullptr) != 0) return kernel_error(host_error(errno));
    uint8_t wire[16]{};
    put_le(wire, 0, time.tv_sec, 8); put_le(wire, 8, time.tv_usec, 8);
    return memory_.write(output, wire, sizeof(wire)) ? 0 : kernel_error(fault);
}
int64_t PS4Libkernel::posix_result(int64_t result, uint64_t thread) {
    // Orbis kernel errors are kernel_error(1..) = -(0x7ffe0000 - e).
    constexpr int64_t first = -int64_t(0x7ffe0000) + 1, last = -int64_t(0x7ffe0000) + 0xffff;
    if (result < first || result > last) return result;
    const size_t index = threads_.index(thread);
    if (errno_table_ && index < errno_table_size_ / 8) {
        uint8_t wire[4];
        put_le(wire, 0, uint64_t(result + 0x7ffe0000), 4);
        (void)memory_.write(errno_table_ + index * 8, wire, sizeof(wire));
    }
    return -1;
}
int64_t PS4Libkernel::invoke(PS4KernelService service, const PS4KernelCall& c, uint64_t thread, bool posix) {
    bool handled = false;
    int64_t result = extended(service, c, thread, handled);
    if (!handled) result = invoke_service(service, c, thread);
    return posix ? posix_result(result, thread) : result;
}
int64_t PS4Libkernel::extended(PS4KernelService service, const PS4KernelCall& c, uint64_t thread, bool& handled) {
    handled = true;
    auto write = [&](uint64_t address, uint64_t value, size_t width) {
        uint8_t wire[8];
        put_le(wire, 0, value, width);
        return address && memory_.write(address, wire, width);
    };
    using S = PS4KernelService;
    switch (service) {
    case S::user_initialize: return user_service_.initialize(c.rdi);
    case S::pad_init: return pad_.initialize();
    case S::gnm_draw_auto: return gnm_.draw_auto(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),uint32_t(c.rcx));
    case S::gnm_submit:
    case S::gnm_submit_flip: {
        // Flip arguments after the VideoOut handle (R9): buffer index, flip
        // mode and flip argument, the seventh to ninth System V arguments.
        uint8_t stack[24];
        const bool flip = service == S::gnm_submit_flip;
        if (flip && !graphics_.null_backend()) return PS4GnmQueue::error(0x3fff);
        if (flip && (!c.rsp || c.rsp > UINT64_MAX - 32 || !memory_.read(c.rsp + 8, stack, sizeof(stack))))
            return PS4GnmQueue::error(0x1000);
        uint32_t interrupts = 0;
        PS4GnmQueue::PreparedFlip prepared;
        const int64_t result = graphics_.submit(uint32_t(c.rdi),c.rsi,c.rdx,c.rcx,c.r8,&interrupts,
                                                flip ? &prepared : nullptr);
        if (result) return result;
        if (interrupts) threads_.graphics_interrupt();
        if (!flip) return 0;
        // The host backend presents the GPU-rendered target: no CPU copy.
        const int64_t shown = videoout_.submit(c.r9, signed32(get_le(stack, 4)), signed32(get_le(stack + 8, 4)),
                                               get_le(stack + 16, 8), !graphics_.host_backend(), true);
        // The host presents asynchronously, so the previous flip may still be
        // pending. Skip this image rather than fail: the game ignores the error
        // and spins forever on the prepareFlip label (Sonic Mania after its
        // first flip on the iPhone).
        if (shown == PS4VideoOut::error(0x12)) videoout_.count_dropped();
        else if (shown) return shown;
        // The work is already retired: what prepareFlip asks for happens at
        // the flip's vblank.
        if (!videoout_.flip_at_vblank(prepared.label, prepared.address, prepared.value, prepared.interrupt))
            return PS4GnmQueue::error(0x1000);
        return 0;
    }
    case S::gnm_allowed: return graphics_.allowed();
    case S::gnm_done: return graphics_.done();
    case S::gnm_draw_index: return gnm_.draw_index(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),c.rcx,uint32_t(c.r8));
    case S::gnm_vgt: return gnm_.vgt(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),uint32_t(c.rcx),uint32_t(c.r8));
    case S::gnm_vs: return gnm_.vs(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx));
    case S::gnm_ps350: return gnm_.ps350(c.rdi,uint32_t(c.rsi),c.rdx);
    case S::gnm_cs: return gnm_.cs(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx));
    case S::gnm_dispatch: return gnm_.dispatch(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),uint32_t(c.rcx),uint32_t(c.r8),uint32_t(c.r9));
    case S::gnm_embedded_vs: return gnm_.embedded_vs(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx));
    case S::gnm_wait_flip: return gnm_.wait_flip_done(c.rdi,uint32_t(c.rsi));
    case S::gnm_push_marker: return gnm_.push_marker(c.rdi,uint32_t(c.rsi),c.rdx);
    case S::gnm_pop_marker: return gnm_.pop_marker(c.rdi,uint32_t(c.rsi));
    case S::gnm_init350: return gnm_.init_default_state350(c.rdi,uint32_t(c.rsi));
    case S::gnm_ps: return gnm_.ps(c.rdi,uint32_t(c.rsi),c.rdx);
    case S::gnm_cs_plain: return gnm_.cs(c.rdi,uint32_t(c.rsi),c.rdx,0);
    case S::gnm_draw_index_offset: return gnm_.draw_index_offset(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),uint32_t(c.rcx),uint32_t(c.r8));
    // Draw(Index)Indirect(cmdbuf, size, data_offset, stage, vertex_sgpr, instance_sgpr, flags):
    // flags is the seventh argument, on the stack.
    case S::gnm_draw_indirect: case S::gnm_draw_index_indirect: {
        uint8_t stack[8]{};
        const uint32_t flags = c.rsp && memory_.read(c.rsp + 8, stack, 4) ? uint32_t(get_le(stack, 4)) : 0;
        return gnm_.draw_indirect(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),uint32_t(c.rcx),uint32_t(c.r8),uint32_t(c.r9),
                                  flags, service == S::gnm_draw_index_indirect);
    }
    case S::gnm_dispatch_indirect: return gnm_.dispatch_indirect(c.rdi,uint32_t(c.rsi),uint32_t(c.rdx),uint32_t(c.rcx));
    case S::gnm_dispatch_init: return gnm_.dispatch_init_default_state(c.rdi,uint32_t(c.rsi));
    case S::gnm_context_init: return gnm_.init_context_state(c.rdi,uint32_t(c.rsi),0x20);
    case S::gnm_context_init400: return gnm_.init_context_state(c.rdi,uint32_t(c.rsi),0x100);
    case S::gnm_reset_vgt: return gnm_.reset_vgt(c.rdi,uint32_t(c.rsi));
    case S::gnm_set_marker: return gnm_.set_marker(c.rdi,uint32_t(c.rsi),c.rdx);
    case S::gnm_clock: return 800000000; // Base PS4 GPU clock.
    case S::gnm_unsupported: return signed32(0x8eee00ffU);
    // CreateWorkloadStream(name, u32* stream), BeginWorkload(stream, u64*
    // workload), EndWorkload(workload): shadPS4's values (streams below 16).
    case S::gnm_workload_stream: return c.rdi && c.rsi && write(c.rsi, 1, 4) ? 0 : 3;
    case S::gnm_workload_begin:
        if (!c.rsi) return 3;
        return write(c.rsi, uint32_t(c.rdi) < 16 ? 1 : 0, 8) ? (uint32_t(c.rdi) > 15 ? 1 : 0) : 3;
    case S::gnm_workload_end: return c.rdi ? (((c.rdi >> 56) & 0xff) > 15 ? 2 : 0) : 2;
    // Submit(AndFlip)CommandBuffersForWorkload(workload, count, ...): the
    // same submissions after the workload, so every argument moves one
    // register (and the flip's stack arguments one slot) down.
    case S::gnm_submit_workload: case S::gnm_submit_flip_workload: {
        PS4KernelCall shifted{c.rsi, c.rdx, c.rcx, c.r8, c.r9, 0, c.rsp ? c.rsp + 8 : 0};
        if (service == S::gnm_submit_flip_workload) {
            uint8_t handle[8];
            if (!c.rsp || !memory_.read(c.rsp + 8, handle, 8)) return PS4GnmQueue::error(0x1000);
            shifted.r9 = get_le(handle, 4);
        }
        bool nested = false;
        return extended(service == S::gnm_submit_workload ? S::gnm_submit : S::gnm_submit_flip, shifted, thread, nested);
    }
    // MapComputeQueue(pipe, queue, ring, ring dwords, u32* read pointer[,
    // priority]): 7 pipes of 8 queues, a 256-byte aligned ring; the vqid.
    case S::gnm_map_compute: {
        const uint32_t pipe = uint32_t(c.rdi), queue = uint32_t(c.rsi), size = uint32_t(c.rcx);
        if (pipe >= 7) return signed32(0x80d17000U);
        if (queue >= 8) return signed32(0x80d17001U);
        if (!c.rdx || (c.rdx & 0xff)) return signed32(0x80d17003U);
        if (!size || (size & (size - 1))) return signed32(0x80d17002U);
        if (!c.r8 || (c.r8 & 3)) return signed32(0x80d17004U);
        std::lock_guard<std::mutex> guard(lock_);
        auto& q = compute_[pipe * 8 + queue];
        q = {c.rdx, c.r8, size, 0};
        return write(c.r8, 0, 4) ? int64_t(pipe * 8 + queue + 1) : kernel_error(fault);
    }
    case S::gnm_unmap_compute: {
        std::lock_guard<std::mutex> guard(lock_);
        if (c.rdi && c.rdi <= compute_.size()) compute_[c.rdi - 1] = {};
        return 0;
    }
    // DingDong(vqid, next offset in dwords): the ring's new commands, up to
    // two spans when it wraps; the read pointer then reaches the offset.
    case S::gnm_ding_dong: {
        std::lock_guard<std::mutex> guard(lock_);
        if (!c.rdi || c.rdi > compute_.size() || !compute_[c.rdi - 1].ring) return 0;
        auto& q = compute_[c.rdi - 1];
        const uint32_t next = uint32_t(c.rsi);
        if (next > q.size) return 0;
        int64_t interrupts = 0;
        auto span = [&](uint32_t from, uint32_t to) {
            if (to > from) { const int64_t raised = graphics_.compute(q.ring + uint64_t(from) * 4, uint64_t(to - from) * 4); if (raised > 0) interrupts += raised; }
        };
        if (next >= q.offset) span(q.offset, next);
        else { span(q.offset, q.size); span(0, next); }
        q.offset = next == q.size ? 0 : next;
        if (interrupts) threads_.graphics_interrupt();
        return write(q.read_pointer, next, 4) ? 0 : kernel_error(fault);
    }
    case S::gnm_tessellation_offchip: return 0x800000;
    // The ring's address, where the game maps it (MAP_FIXED): the top of the
    // mmap window, since iOS refuses the PS4's own 0xff0000000.
    case S::gnm_tessellation_ring: {
        uint64_t window = 0, bytes = 0;
        if (dynamic_ && dynamic_->window(window, bytes) && bytes >= 0x200000)
            return int64_t((window + bytes - 0x100000) & ~uint64_t{0xffff});
        return 0xff0000000;
    }
    case S::gnm_ls: return gnm_.ls(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx));
    case S::gnm_hs: case S::gnm_hs_update: return gnm_.hs(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx),service == S::gnm_hs_update);
    case S::gnm_es: return gnm_.es(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx));
    case S::gnm_gs: return gnm_.gs(c.rdi,uint32_t(c.rsi),c.rdx);
    case S::gnm_push_color_marker: return gnm_.push_color_marker(c.rdi,uint32_t(c.rsi),c.rdx);
    case S::gnm_set_color_marker: case S::gnm_embedded_ps: return gnm_.nop_fill(c.rdi,uint32_t(c.rsi),0);
    case S::gnm_ding_dong_marker: return gnm_.nop_fill(c.rdi,uint32_t(c.rsi),4);
    case S::gnm_compute_wait_address:
        return gnm_.compute_wait_address(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx),uint32_t(c.r8),uint32_t(c.r9));
    case S::gnm_compute_wait_semaphore: return gnm_.compute_wait_semaphore(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx));
    case S::gnm_dispatch_indirect_mec: return gnm_.dispatch_indirect_mec(c.rdi,uint32_t(c.rsi),c.rdx,uint32_t(c.rcx));
    case S::process_id: return 1;
    // AttrGetschedparam(attr*, sched_param*): the default priority.
    case S::attr_getschedparam: return c.rdi && c.rsi && write(c.rsi, 700, 4) ? 0 : kernel_error(inval);
    // MutexReltimedlockNp(mutex*, microseconds): tries until the delay runs out.
    case S::mutex_reltimedlock: {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(c.rsi);
        for (;;) {
            const Error e = threads_.trylock(c.rdi, thread);
            if (e != busy) return kernel_error(e);
            if (std::chrono::steady_clock::now() >= deadline) return kernel_error(timedout);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    // scePthreadGetname(thread, char name[32]).
    case S::thread_getname: {
        const uint8_t empty[32] = {};
        return c.rsi && memory_.write(c.rsi, empty, sizeof(empty)) ? 0 : kernel_error(fault);
    }
    // POSIX semaphores: sem_t holds a kernel semaphore's handle.
    case S::sem_init:
        if (c.rsi) return kernel_error(notsup); // Shared between processes.
        if (c.rdx > uint64_t(INT32_MAX)) return kernel_error(inval);
        return kernel_error(threads_.sema_create(c.rdi, 0, signed32(c.rdx), INT32_MAX));
    case S::sem_destroy: case S::sem_wait: case S::sem_timedwait: case S::sem_post: {
        uint8_t cell[8];
        if (!c.rdi || !memory_.read(c.rdi, cell, sizeof(cell))) return kernel_error(inval);
        const uint64_t handle = get_le(cell, 8);
        if (service == S::sem_destroy) return kernel_error(threads_.sema_delete(handle));
        if (service == S::sem_post) return kernel_error(threads_.sema_signal(handle, 1));
        if (service == S::sem_timedwait) {
            // An absolute CLOCK_REALTIME deadline, at most a year away.
            uint8_t wire[16];
            if (!c.rsi || !memory_.read(c.rsi, wire, sizeof(wire))) return kernel_error(inval);
            const int64_t seconds = int64_t(get_le(wire, 8)), nanoseconds = int64_t(get_le(wire + 8, 8));
            if (nanoseconds < 0 || nanoseconds >= 1000000000) return kernel_error(inval);
            timespec now{};
            ::clock_gettime(CLOCK_REALTIME, &now);
            const int64_t whole = std::min<int64_t>(seconds - int64_t(now.tv_sec), 31536000);
            const uint64_t left = uint64_t(std::max<int64_t>(whole * 1000000 + (nanoseconds - int64_t(now.tv_nsec)) / 1000, 0));
            return kernel_error(threads_.sema_wait(handle, 1, 0, false, &left));
        }
        return kernel_error(threads_.sema_wait(handle, 1, 0, false));
    }
    // sceKernelUuidCreate(SceKernelUuid*): a random version 4 UUID.
    case S::uuid_create: {
        if (!c.rdi) return kernel_error(inval);
        std::random_device device;
        uint8_t uuid[16];
        for (auto& byte : uuid) byte = uint8_t(device());
        uuid[6] = uint8_t((uuid[6] & 0x0f) | 0x40); uuid[8] = uint8_t((uuid[8] & 0x3f) | 0x80);
        return memory_.write(c.rdi, uuid, sizeof(uuid)) ? 0 : kernel_error(fault);
    }
    // inet_pton(af, const char*, void*) and inet_ntop(af, const void*, char*,
    // size): IPv4 (AF_INET, 2) only, so 0/null for anything else.
    case S::net_pton: {
        if (c.rdi != 2) return 0;
        char text[16] = {};
        for (size_t i = 0; i < sizeof(text); ++i) {
            if (!memory_.read(c.rsi + i, &text[i], 1)) return kernel_error(fault);
            if (!text[i]) break;
        }
        if (text[sizeof(text) - 1]) return 0;
        unsigned part[4]; char end = 0;
        if (std::sscanf(text, "%u.%u.%u.%u%c", &part[0], &part[1], &part[2], &part[3], &end) != 4) return 0;
        uint8_t address[4];
        for (int i = 0; i < 4; ++i) { if (part[i] > 255) return 0; address[i] = uint8_t(part[i]); }
        return memory_.write(c.rdx, address, sizeof(address)) ? 1 : kernel_error(fault);
    }
    case S::net_ntop: {
        uint8_t address[4];
        if (c.rdi != 2 || !memory_.read(c.rsi, address, sizeof(address))) return 0;
        char text[16];
        const int length = std::snprintf(text, sizeof(text), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
        if (uint64_t(length) >= c.rcx || !memory_.write(c.rdx, text, size_t(length) + 1)) return 0;
        return int64_t(c.rdx);
    }
    case S::thread_id: return threads_.known(thread) ? int64_t(uint32_t(thread)) : kernel_error(inval);
    // FIFO/round-robin priorities: 256 lowest to 767 highest.
    case S::sched_priority_max: case S::sched_priority_min:
        if (c.rdi != 1 && c.rdi != 3) return kernel_error(inval);
        return service == S::sched_priority_max ? 767 : 256;
    // pthread_getschedparam(thread, s32* policy, sched_param*): FIFO, default priority.
    case S::sched_param_get:
        return (!c.rsi || write(c.rsi, 1, 4)) && (!c.rdx || write(c.rdx, 700, 4)) ? 0 : kernel_error(fault);
    case S::net_unavailable: return kernel_error(notsup);
    // No PRT aperture is set: index, void** address, size_t* size.
    case S::prt_aperture: return (!c.rsi || write(c.rsi, 0, 8)) && (!c.rdx || write(c.rdx, 0, 8)) ? 0 : kernel_error(fault);
    // DiscMap: the game files are all installed, so there is no disc bitmap;
    // Func_7C98(path, offset, size, s32* flags, s32*, s32*) clears all three.
    case S::discmap_no_bitmap: return signed32(0x81100004U);
    case S::discmap_flags:
        return (!c.rcx || write(c.rcx, 0, 4)) && (!c.r8 || write(c.r8, 0, 4)) && (!c.r9 || write(c.r9, 0, 4)) ? 0 : kernel_error(fault);
    // GetDeviceCapabilityInfo(handle, info*): u64 capability, none (no HDR or VR).
    case S::video_capability: return write(c.rsi, 0, 8) ? 0 : kernel_error(fault);
    // SetGsRingSizes(esgs, gsvs): 1 MiB multiples in [4, 8] MiB each. Ring sizes
    // do not matter to the emulated GPU.
    case S::gnm_gs_rings: {
        auto valid = [](uint64_t size) { return size >= 0x400000 && size <= 0x800000 && !(size & 0xfffff); };
        return valid(uint32_t(c.rdi)) && valid(uint32_t(c.rsi)) ? 0 : 0x08000000;
    }
    case S::pad_open: return pad_.open(signed32(c.rdi), signed32(c.rsi), signed32(c.rdx), c.rcx);
    case S::pad_close: return pad_.close(signed32(c.rdi));
    case S::pad_handle: return pad_.get_handle(signed32(c.rdi), signed32(c.rsi), signed32(c.rdx));
    case S::pad_read: return pad_.read(signed32(c.rdi), c.rsi, signed32(c.rdx), false);
    case S::pad_read_state: return pad_.read(signed32(c.rdi), c.rsi, 1, true);
    case S::pad_information: return pad_.information(signed32(c.rdi), c.rsi);
    case S::audio_init: return audio_.initialize();
    case S::audio_open:
        return audio_.open(signed32(c.rdi), signed32(c.rsi), signed32(c.rdx), uint32_t(c.rcx), uint32_t(c.r8), uint32_t(c.r9));
    case S::audio_close: return audio_.close(signed32(c.rdi));
    case S::audio_output: return audio_.output(signed32(c.rdi), c.rsi);
    case S::user_terminate: return user_service_.terminate();
    case S::user_initial: return user_service_.initial_user(c.rdi);
    case S::user_login_list: return user_service_.login_users(c.rdi);
    case S::user_event: return user_service_.event(c.rdi);
    case S::user_name: return user_service_.user_name(signed32(c.rdi), c.rsi, c.rdx);
    case S::video_open: return videoout_.open(signed32(c.rsi), signed32(c.rdx), c.rcx);
    case S::video_close: return videoout_.close(c.rdi);
    case S::video_attribute: {
        uint8_t pitch[8];
        if (!c.rsp || c.rsp > UINT64_MAX - 8 || !memory_.read(c.rsp + 8, pitch, 8)) return PS4VideoOut::error(2);
        return videoout_.set_attribute(c.rdi, uint32_t(c.rsi), uint32_t(c.rdx), uint32_t(c.rcx), uint32_t(c.r8), uint32_t(c.r9), uint32_t(get_le(pitch, 8)));
    }
    case S::video_register: return videoout_.register_buffers(c.rdi, signed32(c.rsi), c.rdx, signed32(c.rcx), c.r8);
    case S::video_unregister: return videoout_.unregister_buffers(c.rdi, signed32(c.rsi));
    case S::video_submit: videoout_.pace(); return videoout_.submit(c.rdi, signed32(c.rsi), signed32(c.rdx), c.rcx);
    case S::video_pending: return videoout_.pending(c.rdi);
    case S::video_resolution: return videoout_.resolution(c.rdi, c.rsi);
    case S::video_flip_rate: return videoout_.set_flip_rate(c.rdi, signed32(c.rsi));
    case S::sema_create: return kernel_error(threads_.sema_create(c.rdi, c.rsi, signed32(c.rcx), signed32(c.r8)));
    case S::sema_delete: return kernel_error(threads_.sema_delete(c.rdi));
    case S::sema_wait: case S::sema_poll:
        return kernel_error(threads_.sema_wait(c.rdi, signed32(c.rsi), service == S::sema_wait ? c.rdx : 0, service == S::sema_poll));
    case S::sema_signal: return kernel_error(threads_.sema_signal(c.rdi, signed32(c.rsi)));
    case S::sema_cancel: return kernel_error(threads_.sema_cancel(c.rdi, signed32(c.rsi), c.rdx));
    case S::flag_create: return kernel_error(threads_.flag_create(c.rdi, c.rsi, uint32_t(c.rdx), c.rcx));
    case S::flag_delete: return kernel_error(threads_.flag_delete(c.rdi));
    case S::flag_set: case S::flag_clear: return kernel_error(threads_.flag_set(c.rdi, c.rsi, service == S::flag_clear));
    case S::flag_wait: case S::flag_poll:
        return kernel_error(threads_.flag_wait(c.rdi, c.rsi, uint32_t(c.rdx), c.rcx,
                                               service == S::flag_wait ? c.r8 : 0, service == S::flag_poll));
    case S::flag_cancel: return kernel_error(threads_.flag_cancel(c.rdi, c.rsi, c.rdx));
    case S::process_counter: {
        timespec t{};
        if (::clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
        const uint64_t now = uint64_t(t.tv_sec) * 1000000000ULL + uint64_t(t.tv_nsec);
        return int64_t(now - std::min(now, started_us_ * 1000));
    }
    case S::counter_frequency: return 1000000000;
    case S::kernel_sleep: return kernel_error(threads_.sleep(uint64_t(uint32_t(c.rdi)) * 1000000));
    case S::np_state: return c.rsi && write(c.rsi, 1, 4) ? 0 : signed32(0x80550003U); // 1: signed out.
    case S::np_signed_out: return signed32(0x80550006U);
    case S::np_create_request: return next_np_request_++;
    // sceAppContentInitialize(initParam, bootParam*): bootParam.attr 0 (normal boot).
    case S::app_init: return !c.rsi || write(c.rsi, 0, 4) ? 0 : signed32(0x80D90002U);
    case S::module_load_start: case S::module_dlsym: case S::module_list: case S::module_info_unwind:
    case S::module_info: case S::module_info_ex:
        return module_service(service, c);
    case S::app_addcont_list: return addcont_list(c.rsi, uint32_t(c.rdx), c.rcx);
    case S::app_addcont_info: return addcont_info(c.rsi, c.rdx);
    case S::app_addcont_mount: return addcont_mount(c.rsi, c.rdx);
    case S::app_addcont_unmount: return addcont_unmount(c.rdi);
    // AppParamGetInt(paramId, value*): SKU flag (0) 3 = full game, user params 0.
    case S::app_param_int:
        if (c.rdi > 4 || !c.rsi) return signed32(0x80D90002U);
        return write(c.rsi, c.rdi == 0 ? 3 : 0, 4) ? 0 : signed32(0x80D90002U);
    case S::app_temp_mount: return temp_mount(c.rsi);
    case S::rtc_current_tick: case S::rtc_current_clock: case S::rtc_current_local:
    case S::rtc_get_tick: case S::rtc_set_tick: case S::rtc_resolution: case S::rtc_utc_to_local: case S::rtc_time_t:
        return rtc(service, c);
    // GetGameInfo(context, handle, details*, data*), GetGroupInfo and
    // GetTrophyInfo(context, handle, id, details*, data*): each structure
    // starts with its size (u64); everything after it is zeroed.
    case S::trophy_game_info: case S::trophy_group_info: {
        auto zero_sized = [&](uint64_t address) {
            uint8_t wire[8];
            if (!address) return true;
            if (!memory_.read(address, wire, 8)) return false;
            const uint64_t size = std::min<uint64_t>(get_le(wire, 8), 4096);
            if (size <= 8) return true;
            const std::vector<uint8_t> zeros(size_t(size - 8), 0);
            return memory_.write(address + 8, zeros.data(), zeros.size());
        };
        const bool game = service == S::trophy_game_info;
        return zero_sized(game ? c.rdx : c.rcx) && zero_sized(game ? c.rcx : c.r8) ? 0 : signed32(0x80551604U);
    }
    // GetTrophyUnlockState(context, handle, flag array* (128 bits), u32* count).
    case S::trophy_unlock_state: {
        const uint8_t none[16] = {};
        return (!c.rdx || memory_.write(c.rdx, none, sizeof(none))) && (!c.rcx || write(c.rcx, 0, 4)) ? 0 : signed32(0x80551604U);
    }
    // GetDisplaySafeAreaInfo(info*): float ratio 1.0 (the whole screen).
    case S::safe_area: return c.rdi && write(c.rdi, 0x3f800000, 4) ? 0 : signed32(0x80A10003U);
    case S::trophy_create_context: case S::trophy_create_handle:
        return c.rdi && write(c.rdi, 1, 4) ? 0 : signed32(0x80551604U);
    // UnlockTrophy(context, handle, trophyId, platinumId*): no platinum unlocked.
    case S::trophy_unlock: return !c.rcx || write(c.rcx, uint32_t(-1), 4) ? 0 : signed32(0x80551604U);
    case S::save_init: return 0;
    case S::save_mount1: case S::save_umount2: case S::save_search: case S::save_set_param: case S::save_get_param:
    case S::save_save_icon: case S::save_load_icon: case S::save_delete: case S::save_mount_info: case S::save_event:
    case S::save_progress: case S::save_memory_setup: case S::save_memory_get: case S::save_memory_set:
        return save_service(service, c);
    case S::save_term: save_umount(0); return 0;
    case S::save_mount2: return save_mount(c.rdi, c.rsi);
    case S::save_umount: return save_umount(c.rdi);
    case S::save_dialog_init: save_dialog_status_ = 1; return 0;
    case S::save_dialog_open: save_dialog_status_ = 3; return 0;
    case S::save_dialog_update: return save_dialog_status_;
    // OrbisSaveDataDialogResult: mode@0 (kept), result@4, buttonId@8 (1 = OK/Yes).
    case S::save_dialog_result:
        if (save_dialog_status_ != 3) return signed32(0x80b80008U); // Not finished.
        return write(c.rdi + 4, 0, 4) && write(c.rdi + 8, 1, 4) ? 0 : signed32(0x80b80004U);
    case S::save_dialog_term: save_dialog_status_ = 0; return 0;
    // Dialog status: 0 none, 1 initialized, 2 running, 3 finished.
    case S::msg_init: msg_dialog_status_ = 1; return 0;
    case S::msg_open: msg_dialog_status_ = 3; return 0;
    case S::msg_status: return msg_dialog_status_;
    // OrbisMsgDialogResult: mode@0 (kept), result@4, buttonId@8 (1 = OK/Yes).
    case S::msg_result:
        if (msg_dialog_status_ != 3) return signed32(0x80B80008U);
        return c.rdi && write(c.rdi + 4, 0, 4) && write(c.rdi + 8, 1, 4) ? 0 : signed32(0x80B80004U);
    case S::msg_close: if (msg_dialog_status_) msg_dialog_status_ = 3; return 0;
    case S::msg_term: msg_dialog_status_ = 0; return 0;
    case S::error_init: error_dialog_status_ = 1; return 0;
    case S::error_open: error_dialog_status_ = 3; return 0;
    case S::error_status: return error_dialog_status_;
    case S::error_close: if (error_dialog_status_) error_dialog_status_ = 3; return 0;
    case S::error_term: error_dialog_status_ = 0; return 0;
    // GetState(s32*): 0, disconnected. GetInfo: not connected.
    case S::netctl_state: return c.rdi && write(c.rdi, 0, 4) ? 0 : signed32(0x80412101U);
    case S::netctl_info: return signed32(0x80412108U);
    // RegisterCallback(func, arg, s32* id): never called back.
    case S::netctl_callback: return !c.rdx || write(c.rdx, 1, 4) ? 0 : signed32(0x80412101U);
    case S::net_pool: return 1;
    case S::playgo_open: return c.rdi && write(c.rdi, 1, 4) ? 0 : signed32(0x80B20002U);
    // GetLocus(handle, u16* chunks, count, s8* loci): 3, local and fast.
    case S::playgo_locus:
        if (!c.rsi || !c.rcx || c.rdx > 0xffff) return signed32(0x80B20002U);
        for (uint64_t i = 0; i < c.rdx; ++i) if (!write(c.rcx + i, 3, 1)) return signed32(0x80B20002U);
        return 0;
    // GetProgress(handle, chunks, count, {u64 progress, total}*): complete.
    case S::playgo_progress: return c.rcx && write(c.rcx, 1, 8) && write(c.rcx + 8, 1, 8) ? 0 : signed32(0x80B20002U);
    // GetToDoList(handle, list, count, u32* entries): nothing to do.
    case S::playgo_todo: return c.rcx && write(c.rcx, 0, 4) ? 0 : signed32(0x80B20002U);
    case S::playgo_eta: return c.rcx && write(c.rcx, 0, 8) ? 0 : signed32(0x80B20002U);
    case S::playgo_speed: return c.rsi && write(c.rsi, 2, 4) ? 0 : signed32(0x80B20002U); // Full speed.
    case S::playgo_language: return c.rsi && write(c.rsi, UINT64_MAX, 8) ? 0 : signed32(0x80B20002U);
    // GetChunkId(handle, u16* list, count, u32* entries): one chunk, 0.
    case S::playgo_chunks:
        if (!c.rcx) return signed32(0x80B20002U);
        if (c.rsi && c.rdx && !write(c.rsi, 0, 2)) return signed32(0x80B20002U);
        return write(c.rcx, 1, 4) ? 0 : signed32(0x80B20002U);
    // GetRandomNumber(void* buffer, size): at most 64 bytes.
    case S::random_number: {
        if (!c.rdi || c.rsi > 64) return signed32(0x817C0016U);
        std::random_device device;
        uint8_t bytes[64];
        for (uint64_t i = 0; i < c.rsi; ++i) bytes[i] = uint8_t(device());
        return memory_.write(c.rdi, bytes, size_t(c.rsi)) ? 0 : signed32(0x817C0016U);
    }
    // GetFsSandboxRandomWord(): a const char* to 10 random letters, kept in
    // one read-only page mapped on first use.
    case S::sandbox_word: {
        std::lock_guard<std::mutex> guard(lock_);
        if (sandbox_word_) return int64_t(sandbox_word_);
        if (!dynamic_) return 0;
        uint64_t page = 0;
        if (dynamic_->place(0, PS4VirtualMemory::page_size, 0, 3, false, page) != ok) return 0;
        static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
        std::random_device device;
        char word[11]{};
        for (int i = 0; i < 10; ++i) word[i] = letters[device() % 26];
        if (!memory_.write(page, word, sizeof(word))) return 0;
        (void)dynamic_->protect(page, PS4VirtualMemory::page_size, 1);
        sandbox_word_ = page;
        return int64_t(page);
    }
    // Outputs(OrbisAudioOutOutputParam* {s32 handle; u32; const void* samples}, count).
    case S::audio_outputs: {
        if (!c.rdi || !c.rsi || c.rsi > 25) return signed32(0x8026000AU);
        int64_t result = 0;
        for (uint64_t i = 0; i < c.rsi && !result; ++i) {
            uint8_t entry[16];
            if (!memory_.read(c.rdi + 16 * i, entry, sizeof(entry))) return signed32(0x80260005U);
            result = audio_.output(signed32(get_le(entry, 4)), get_le(entry + 8, 8));
        }
        return result;
    }
    // GetPortState(handle, state*): the main output, stereo, full volume.
    case S::audio_port_state: {
        uint8_t state[32]{};
        put_le(state, 0, 1, 2); state[2] = 2; put_le(state, 4, 127, 2);
        return c.rsi && memory_.write(c.rsi, state, sizeof(state)) ? 0 : signed32(0x80260005U);
    }
    case S::video_flip_status: return videoout_.flip_status(c.rdi, c.rsi);
    case S::video_vblank_status: return videoout_.vblank_status(c.rdi, c.rsi);
    case S::video_add_flip: case S::video_add_vblank:
        return videoout_.add_event(c.rdi, c.rsi, service == S::video_add_flip ? 6 : 7, c.rdx);
    case S::video_delete_flip: case S::video_delete_vblank:
        return videoout_.add_event(c.rdi, c.rsi, service == S::video_delete_flip ? 6 : 7, 0, true);
    case S::video_event_id: case S::video_event_data: case S::video_event_count:
        return videoout_.event_field(c.rdi, c.rsi, service == S::video_event_id ? 0 : service == S::video_event_data ? 1 : 2);
    case S::graphics_event_add: return kernel_error(threads_.graphics_event_add(c.rdi, c.rsi, c.rdx));
    case S::graphics_event_delete: return kernel_error(threads_.graphics_event_delete(c.rdi, c.rsi));
    case S::equeue_create: return kernel_error(threads_.equeue_create(c.rdi, c.rsi));
    case S::equeue_delete: return kernel_error(threads_.equeue_delete(c.rdi));
    case S::equeue_wait:
        return kernel_error(threads_.equeue_wait(c.rdi, c.rsi, signed32(c.rdx), c.rcx, c.r8));
    case S::user_event_add: case S::user_event_add_edge:
        return kernel_error(threads_.user_event_add(c.rdi, signed32(c.rsi), service == S::user_event_add_edge));
    case S::user_event_delete: return kernel_error(threads_.user_event_delete(c.rdi, signed32(c.rsi)));
    case S::user_event_trigger: return kernel_error(threads_.user_event_trigger(c.rdi, signed32(c.rsi), c.rdx));
    case S::event_id: case S::event_filter: case S::event_data: case S::event_fflags:
    case S::event_error: case S::event_user_data: {
        uint8_t event[PS4Threads::event_size];
        if (!memory_.read(c.rdi, event, sizeof(event))) return kernel_error(fault);
        if (service == S::event_id) return signed64(get_le(event, 8));
        if (service == S::event_data) return signed64(get_le(event + 16, 8));
        if (service == S::event_user_data) return signed64(get_le(event + 24, 8));
        if (service == S::event_filter) {
            const uint32_t filter = uint32_t(get_le(event + 8, 2));
            return filter <= INT16_MAX ? int64_t(filter) : int64_t(filter) - 0x10000;
        }
        const uint32_t flags = uint32_t(get_le(event + 12, 4));
        return service == S::event_error && flags ? signed32(0x80020000U | flags) : int64_t(flags);
    }
    case S::errno_location: {
        const size_t index = threads_.index(thread);
        return errno_table_ && index < errno_table_size_ / 8 ? int64_t(errno_table_ + index * 8) : kernel_error(fault);
    }
    case S::nanosleep: {
        // nanosleep(const timespec* request, timespec* remaining).
        uint8_t wire[16];
        if (!memory_.read(c.rdi, wire, sizeof(wire))) return kernel_error(fault);
        const int64_t seconds = signed64(get_le(wire, 8)), nanoseconds = signed64(get_le(wire + 8, 8));
        if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000) return kernel_error(inval);
        const Error slept = threads_.sleep(uint64_t(seconds) * 1000000 + uint64_t(nanoseconds) / 1000);
        if (c.rsi && (!write(c.rsi, 0, 8) || !write(c.rsi + 8, 0, 8))) return kernel_error(fault);
        return kernel_error(slept);
    }
    case S::usleep: return kernel_error(threads_.sleep(c.rdi));
    case S::page_size: return 16384; // PS4 pages are 16 KiB.
    case S::yield: std::this_thread::yield(); return 0;
    case S::thread_equal: return c.rdi == c.rsi;
    case S::key_create: return kernel_error(threads_.key_create(c.rdi, c.rsi));
    case S::key_delete: return kernel_error(threads_.key_delete(c.rdi));
    case S::get_specific: return int64_t(threads_.get_specific(c.rdi, thread));
    case S::set_specific: return kernel_error(threads_.set_specific(c.rdi, c.rsi, thread));
    case S::rwlock_init: return kernel_error(threads_.rwlock_init(c.rdi));
    case S::rwlock_destroy: return kernel_error(threads_.rwlock_destroy(c.rdi));
    case S::rwlock_rdlock: return kernel_error(threads_.rwlock_lock(c.rdi, thread, false, true));
    case S::rwlock_wrlock: return kernel_error(threads_.rwlock_lock(c.rdi, thread, true, true));
    case S::rwlock_tryrdlock: return kernel_error(threads_.rwlock_lock(c.rdi, thread, false, false));
    case S::rwlock_trywrlock: return kernel_error(threads_.rwlock_lock(c.rdi, thread, true, false));
    case S::rwlock_unlock: return kernel_error(threads_.rwlock_unlock(c.rdi, thread));
    // scePthreadGetprio(thread, int* prio): the default priority, 700.
    case S::thread_getprio: return write(c.rsi, 700, 4) ? 0 : kernel_error(fault);
    // Affinity masks: the 7 cores a game may use.
    case S::thread_getaffinity: return write(c.rsi, 0x7f, 8) ? 0 : kernel_error(fault);
    case S::attr_getaffinity:
        if (const Error e = threads_.attr_accept(c.rdi)) return kernel_error(e);
        return write(c.rsi, 0x7f, 8) ? 0 : kernel_error(fault);
    // pthread_attr_get_np(thread, attr*): the attribute object keeps its defaults.
    case S::attr_get_np: return kernel_error(threads_.attr_accept(c.rsi));
    case S::sigfillset: // sigset_t: 128 bits.
        return write(c.rdi, UINT64_MAX, 8) && write(c.rdi + 8, UINT64_MAX, 8) ? 0 : kernel_error(fault);
    case S::signal: return 0; // SIG_DFL: signals are never delivered.
    // sceKernelConvertUtcToLocaltime(time, time_t* local, Timesec* st, u64* dst):
    // the local time zone is UTC.
    case S::utc_to_local:
        if (c.rsi && !write(c.rsi, c.rdi, 8)) return kernel_error(fault);
        if (c.rdx && (!write(c.rdx, c.rdi, 8) || !write(c.rdx + 8, 0, 8))) return kernel_error(fault);
        if (c.rcx && !write(c.rcx, 0, 8)) return kernel_error(fault);
        return 0;
    // sceKernelConvertLocaltimeToUtc(time, unused, time_t* utc, Timezone* tz, s32* dst).
    case S::local_to_utc:
        if (!c.r8 && !c.rcx) return kernel_error(inval);
        if (c.rcx && !write(c.rcx, 0, 8)) return kernel_error(fault);
        if (c.rdx && !write(c.rdx, c.rdi, 8)) return kernel_error(fault);
        if (c.r8 && !write(c.r8, 0, 4)) return kernel_error(fault);
        return 0;
    case S::write: case S::pread: case S::pwrite: case S::fsync: case S::ftruncate: case S::truncate:
    case S::mkdir: case S::rmdir: case S::rename: case S::unlink: case S::getdirentries: {
        std::lock_guard<std::mutex> guard(lock_);
        if (!threads_.known(thread)) return kernel_error(inval);
        try { return file_service(service, c); } catch (const std::bad_alloc&) { return kernel_error(nomem); }
    }
    case S::direct_size: case S::direct_allocate: case S::direct_release:
    case S::direct_map: case S::flexible_map: case S::posix_mmap:
    case S::direct_available: case S::direct_allocate_main: case S::flexible_map_unnamed: case S::flexible_release:
    case S::virtual_query: case S::query_protection: case S::sdk_version: case S::system_version:
    case S::reserve_range: case S::mtype_protect: case S::direct_query:
    case S::flexible_available: case S::pool_reserve: case S::pool_commit: case S::pool_decommit:
        return memory_service(service, c);
    // MemoryPoolExpand(start, end, length, alignment, s64* physical): direct memory for the pool.
    case S::pool_expand: return memory_service(S::direct_allocate, {c.rdi, c.rsi, c.rdx, c.rcx, 0, c.r8});
    // Getdents(fd, buffer, size): Getdirentries without the position.
    case S::getdents: {
        std::lock_guard<std::mutex> guard(lock_);
        if (!threads_.known(thread)) return kernel_error(inval);
        try { return file_service(S::getdirentries, {c.rdi, c.rsi, c.rdx, 0}); }
        catch (const std::bad_alloc&) { return kernel_error(nomem); }
    }
    // AioSubmitRead/WriteCommands(requests, count, priority, s32* id): done at
    // once, as shadPS4. Request (40 bytes): s64 offset, s64 size, void* buffer,
    // result* {s64 value, u32 state: completed 3, aborted 4}, s32 fd. Every
    // submission shares id 1, always complete for AioWaitRequest.
    case S::aio_read: case S::aio_write: {
        if (!c.rdi || !c.rcx) return kernel_error(fault);
        const int32_t count = signed32(c.rsi);
        if (count < 0) return kernel_error(inval);
        std::lock_guard<std::mutex> guard(lock_);
        if (!threads_.known(thread)) return kernel_error(inval);
        for (int32_t i = 0; i < count; ++i) {
            uint8_t r[40];
            if (!memory_.read(c.rdi + uint64_t(i) * 40, r, sizeof(r))) return kernel_error(fault);
            int64_t done;
            try {
                done = file_service(service == S::aio_read ? S::pread : S::pwrite,
                                    {uint64_t(signed32(uint32_t(get_le(r + 32, 4)))), get_le(r + 16, 8), get_le(r + 8, 8), get_le(r, 8)});
            } catch (const std::bad_alloc&) { done = kernel_error(nomem); }
            const uint64_t result = get_le(r + 24, 8);
            if (result && (!write(result, uint64_t(done), 8) || !write(result + 8, done < 0 ? 4 : 3, 4))) return kernel_error(fault);
        }
        return write(c.rcx, 1, 4) ? 0 : kernel_error(fault);
    }
    // AioWaitRequest(id, s32* state, u32* usec).
    case S::aio_wait: return write(c.rsi, 3, 4) ? 0 : kernel_error(fault);
    // BatchMap(entries, count, s32* done) / BatchMap2(..., flags): BatchMap maps
    // at the given addresses. Entry (32 bytes): void* start, u64 offset, u64
    // length, u8 protection, u8 type, u16, s32 operation: map direct 0, unmap
    // 1, protect 2, map flexible 3, type protect 4. Stops at the first error;
    // *done counts the entries carried out.
    case S::batch_map: case S::batch_map2: {
        const uint64_t flags = service == S::batch_map ? 0x10 : c.rcx;
        const int32_t count = signed32(c.rsi);
        if (!c.rdi || count < 0) return kernel_error(inval);
        int64_t result = 0;
        int32_t done = 0;
        for (; done < count; ++done) {
            // Reject a wrapped guest range before it aliases a low address.
            if (c.rdi > UINT64_MAX - uint64_t(done) * 32 - 31) { result = kernel_error(fault); break; }
            const uint64_t entry = c.rdi + uint64_t(done) * 32;
            uint8_t e[32];
            if (!memory_.read(entry, e, sizeof(e))) { result = kernel_error(fault); break; }
            const uint64_t start = get_le(e, 8), offset = get_le(e + 8, 8), length = get_le(e + 16, 8);
            const uint64_t protection = e[24];
            switch (signed32(uint32_t(get_le(e + 28, 4)))) {
            case 0: result = memory_service(S::direct_map, {entry, length, protection, flags, offset, 0}); break;
            case 1: result = memory_service(S::flexible_release, {start, length}); break;
            case 2: case 4: result = memory_service(S::mtype_protect, {start, length, e[25], protection}); break;
            case 3: result = memory_service(S::flexible_map_unnamed, {entry, length, protection, flags}); break;
            default: result = kernel_error(inval); break;
            }
            if (result) break;
        }
        if (c.rdx && !write(c.rdx, uint32_t(done), 4)) return kernel_error(fault);
        return result;
    }
    case S::proc_param: return int64_t(process_param_);
    // sceLibcHeapGetTraceInfo(info{u64 size; u32 flag, get_segment_info;
    // u64* mspace_atomic_id_mask; u64* mstate_table}), as shadPS4: no segment
    // info, a zero mask and a zeroed 64-entry table that libc's heap updates.
    case S::heap_trace_info: {
        uint8_t size[8];
        if (!c.rdi || !memory_.read(c.rdi, size, 8)) return kernel_error(fault);
        if (get_le(size, 8) < 0x20) return kernel_error(inval);
        std::lock_guard<std::mutex> guard(lock_);
        if (!heap_trace_ && (!dynamic_ ||
                dynamic_->place(0, PS4VirtualMemory::page_size, 0, 3, false, heap_trace_))) {
            heap_trace_ = 0;
            return kernel_error(nomem);
        }
        return write(c.rdi + 12, 0, 4) && write(c.rdi + 16, heap_trace_, 8) &&
               write(c.rdi + 24, heap_trace_ + 8, 8) ? 0 : kernel_error(fault);
    }
    // sceSystemServiceParamGetInt(id, s32* value): language 1 (English US),
    // cross to enter (1000), UTC with default formats and no parental level.
    case S::system_param_int: {
        constexpr int64_t parameter_error = int32_t(0x80A10003);
        int32_t value = 0;
        switch (c.rdi) {
        case 1: value = system_language_; break;
        case 2: case 3: case 4: case 5: case 7: value = 0; break;
        case 1000: value = 1; break;
        default: return parameter_error;
        }
        if (!c.rsi) return parameter_error;
        return write(c.rsi, uint32_t(value), 4) ? 0 : kernel_error(fault);
    }
    // sceSystemServiceGetStatus(status): no events, in the foreground, CPU
    // mode 7 normal (offset 6); 128 reserved bytes stay zero.
    case S::system_status: {
        if (!c.rdi) return int32_t(0x80A10003);
        const uint8_t status[136] = {0, 0, 0, 0, 0, 0, 1};
        return memory_.write(c.rdi, status, sizeof(status)) ? 0 : kernel_error(fault);
    }
    case S::system_event: return int32_t(0x80A10004); // No event.
    // sceSystemServiceLoadExec(path, argv): the process is not replaced. What it
    // asked for goes to the report: the path, then argv[0], which Call of Duty
    // encodes as base64 (- and _, ~ for =): its text and the raw argument.
    case S::load_exec: {
        char path[256], argument[2400];
        if (!c.rdi || !memory_.read_cstring(c.rdi, path, sizeof(path))) return int32_t(0x80A10003);
        std::string text = path;
        uint8_t cell[8];
        if (c.rsi && memory_.read(c.rsi, cell, sizeof(cell)) && get_le(cell, 8) &&
            memory_.read_cstring(get_le(cell, 8), argument, sizeof(argument))) {
            static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            std::string decoded, run;
            uint32_t bits = 0;
            int count = 0;
            for (const char* p = argument; *p; ++p) {
                const char* at = std::strchr(alphabet, *p);
                if (!at) break;
                bits = (bits << 6) | uint32_t(at - alphabet);
                count += 6;
                if (count >= 8) { count -= 8; decoded.push_back(char(bits >> count)); }
            }
            text += " (" + std::to_string(decoded.size()) + " octets) :";
            decoded.push_back('\0');
            for (const char ch : decoded) {
                if (ch >= 32 && ch < 127) { run.push_back(ch); continue; }
                if (run.size() >= 4) text += " " + run;
                run.clear();
            }
            text += " · brut " + std::string(argument).substr(0, 1500);
        }
        std::lock_guard<std::mutex> guard(load_exec_lock_);
        load_exec_ = std::move(text);
        return 0;
    }
    case S::module_info_from_addr: return kernel_error(srch);
    case S::succeed: return 0;
    default: handled = false; return 0;
    }
}
int64_t PS4Libkernel::memory_service(PS4KernelService service, const PS4KernelCall& c) {
    using S = PS4KernelService;
    // The PS4's direct memory: 5056 MiB of physical address space.
    constexpr uint64_t direct_total = uint64_t(5056) * 1024 * 1024, page = PS4VirtualMemory::page_size;
    if (service == S::direct_size) return int64_t(direct_total);
    auto put = [&](uint64_t address, uint64_t value, size_t width) {
        uint8_t wire[8]; put_le(wire, 0, value, width); return address && memory_.write(address, wire, width);
    };
    // GetCompiledSdkVersion(s32*): the SDK the game reports building with does
    // not change anything here; 9.00. GetSystemSwVersion(info*): u64 size 0x28,
    // char text[0x1c], u32 hex version.
    if (service == S::sdk_version) return put(c.rdi, 0x09008001, 4) ? 0 : kernel_error(fault);
    if (service == S::system_version) {
        uint8_t info[0x28]{};
        put_le(info, 0, sizeof(info), 8);
        std::memcpy(info + 8, " 9.000", 6);
        put_le(info, 0x24, 0x09008001, 4);
        return c.rdi && memory_.write(c.rdi, info, sizeof(info)) ? 0 : kernel_error(fault);
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (!dynamic_) return kernel_error(nosys);
    auto aligned = [&](uint64_t value) { return value % page == 0; };
    // CPU read/write bits; GPU-only memory (0x10/0x20) is CPU read/write here,
    // since the GPU is emulated on the CPU. Execution is refused.
    auto cpu = [](uint64_t protection, uint32_t& out) {
        if (protection & ~uint64_t{0x33}) return false;
        out = uint32_t(protection & 3);
        // x86 pages are never write-only (games map CPU-write + GPU memory,
        // 0x32, for frame buffers), and the emulated GPU reads on the CPU.
        if (out & 2) out = 3;
        if (!out && (protection & 0x30)) out = 3;
        return true;
    };
    auto read_pointer = [&](uint64_t address, uint64_t& value) {
        uint8_t wire[8];
        if (!address || !memory_.read(address, wire, 8)) return false;
        value = get_le(wire, 8);
        return true;
    };
    auto write_pointer = [&](uint64_t address, uint64_t value) {
        uint8_t wire[8]; put_le(wire, 0, value, 8);
        return memory_.write(address, wire, 8);
    };
    switch (service) {
    // sceKernelAllocateDirectMemory(start, end, length, alignment, type, s64* physical):
    // the first fit in [start, end), aligned.
    case S::direct_allocate: case S::direct_allocate_main: {
        // AllocateMainDirectMemory(length, alignment, type, s64* physical): anywhere.
        const PS4KernelCall a = service == S::direct_allocate ? c :
            PS4KernelCall{0, direct_total, c.rdi, c.rsi, c.rdx, c.rcx, c.rsp};
        const int64_t start = signed64(a.rdi), end = signed64(a.rsi);
        const uint64_t length = a.rdx, alignment = a.rcx ? a.rcx : page;
        if (start < 0 || end < 0 || !length || !aligned(length) || !aligned(alignment) ||
            (alignment & (alignment - 1)) || uint32_t(a.r8) > 10 || !a.r9) return kernel_error(inval);
        if (length > direct_total) return kernel_error(again);
        const uint64_t limit = std::min<uint64_t>(uint64_t(end), direct_total);
        uint64_t candidate = (uint64_t(start) + alignment - 1) & ~(alignment - 1);
        for (const auto& [at, bytes] : direct_) {
            if (at + bytes <= candidate) continue;
            if (candidate + length <= at) break;
            candidate = (at + bytes + alignment - 1) & ~(alignment - 1);
        }
        if (candidate > limit || length > limit - candidate) return kernel_error(again);
        if (!write_pointer(a.r9, candidate)) return kernel_error(fault);
        direct_[candidate] = length;
        return 0;
    }
    // AvailableDirectMemorySize(start, end, alignment, s64* physical, size_t* size):
    // the largest free aligned gap of direct memory in [start, end).
    case S::direct_available: {
        const int64_t start = signed64(c.rdi), end = signed64(c.rsi);
        const uint64_t alignment = c.rdx ? c.rdx : page;
        if (start < 0 || end < start || !aligned(alignment) || (alignment & (alignment - 1)) || !c.r8) return kernel_error(inval);
        const uint64_t limit = std::min<uint64_t>(uint64_t(end), direct_total);
        uint64_t best_at = 0, best = 0, cursor = uint64_t(start);
        auto consider = [&](uint64_t gap_start, uint64_t gap_end) {
            const uint64_t at = (gap_start + alignment - 1) & ~(alignment - 1);
            if (at < gap_end && gap_end - at > best) { best = gap_end - at; best_at = at; }
        };
        for (const auto& [at, bytes] : direct_) {
            if (at + bytes <= cursor) continue;
            if (at >= limit) break;
            if (at > cursor) consider(cursor, std::min(at, limit));
            cursor = std::max(cursor, at + bytes);
        }
        if (cursor < limit) consider(cursor, limit);
        if (!best) return kernel_error(nomem);
        if (c.rcx && !write_pointer(c.rcx, best_at)) return kernel_error(fault);
        return write_pointer(c.r8, best) ? 0 : kernel_error(fault);
    }
    // MapFlexibleMemory(void** address, length, protection, flags): unnamed.
    case S::flexible_map_unnamed: {
        uint64_t hint = 0;
        uint32_t protection = 0;
        if (!read_pointer(c.rdi, hint)) return kernel_error(fault);
        if (!c.rsi || !aligned(c.rsi) || !cpu(c.rdx, protection)) return kernel_error(inval);
        uint64_t mapped = 0;
        const Error e = dynamic_->place(hint, c.rsi, page, protection, (c.rcx & 0x10) != 0, mapped);
        if (e) return kernel_error(e);
        return write_pointer(c.rdi, mapped) ? 0 : (dynamic_->unmap(mapped, c.rsi), kernel_error(fault));
    }
    case S::flexible_release: return kernel_error(dynamic_->unmap(c.rdi, c.rsi));
    // VirtualQuery(address, flags, info*, size): flags 1 finds the next
    // mapping. OrbisVirtualQueryInfo: start, end, offset (u64), protection,
    // memory type (s32), flag bits (flexible 1, direct 2, stack 4, pooled 8,
    // committed 16), name char[32]: 0x48 bytes.
    case S::virtual_query: {
        uint64_t start = 0, end = 0;
        uint32_t protection = 0;
        if (!c.rdx || c.rcx < 0x48) return kernel_error(inval);
        if (!dynamic_->query(c.rdi, (c.rsi & 1) != 0, start, end, protection)) return kernel_error(access);
        uint8_t info[0x48]{};
        put_le(info, 0, start, 8); put_le(info, 8, end, 8);
        put_le(info, 24, protection, 4);
        info[32] = 1 | 16; // Flexible, committed.
        return memory_.write(c.rdx, info, sizeof(info)) ? 0 : kernel_error(fault);
    }
    // ReserveVirtualRange(void** address, length, flags, alignment): pages
    // with no access, for later fixed mappings; *address is a hint, or exact
    // with MAP_FIXED.
    case S::reserve_range: {
        uint64_t hint = 0;
        if (!read_pointer(c.rdi, hint)) return kernel_error(fault);
        if (!c.rsi || !aligned(c.rsi)) return kernel_error(inval);
        if (c.rcx && (!aligned(c.rcx) || (c.rcx & (c.rcx - 1)) || c.rcx > 0x80000000ULL))
            return kernel_error(inval);
        uint64_t mapped = 0;
        const Error e = dynamic_->place(hint, c.rsi, c.rcx ? c.rcx : page, 0, (c.rdx & 0x10) != 0, mapped);
        if (e) return kernel_error(e);
        return write_pointer(c.rdi, mapped) ? 0 : (dynamic_->unmap(mapped, c.rsi), kernel_error(fault));
    }
    // AvailableFlexibleMemorySize(size_t*): the PS4's default flexible memory,
    // 448 MiB. Flexible use is not counted apart from direct mappings.
    case S::flexible_available: return write_pointer(c.rdi, uint64_t(448) * 1024 * 1024) ? 0 : kernel_error(fault);
    // MemoryPoolReserve(address, length, alignment, flags, void** out): no
    // access until committed; 2 MiB multiples and alignment.
    case S::pool_reserve: {
        constexpr uint64_t mb2 = 2 * 1024 * 1024;
        if (!c.rsi || c.rsi % mb2 || (c.rdx && (c.rdx & (c.rdx - 1)))) return kernel_error(inval);
        uint64_t mapped = 0;
        const Error e = dynamic_->place(c.rdi, c.rsi, std::max<uint64_t>(c.rdx, mb2), 0, (c.rcx & 0x10) != 0 && c.rdi, mapped);
        if (e) return kernel_error(e);
        return write_pointer(c.r8, mapped) ? 0 : (dynamic_->unmap(mapped, c.rsi), kernel_error(fault));
    }
    // MemoryPoolCommit(address, length, type, protection, flags) /
    // Decommit(address, length, flags): 64 KiB multiples. Decommitted pages
    // lose their access, not their contents.
    case S::pool_commit: case S::pool_decommit: {
        uint32_t protection = 0;
        if (!c.rdi || !c.rsi || c.rsi % 0x10000) return kernel_error(inval);
        if (service == S::pool_commit && !cpu(c.rcx, protection)) return kernel_error(inval);
        return kernel_error(dynamic_->protect(c.rdi, c.rsi, protection));
    }
    // Mtypeprotect(address, length, memory type, protection): the type does
    // not change how the CPU sees the pages.
    case S::mtype_protect: {
        uint32_t protection = 0;
        if (!cpu(c.rcx, protection)) return kernel_error(inval);
        return kernel_error(dynamic_->protect(c.rdi, c.rsi, protection));
    }
    // DirectMemoryQuery(offset, flags, info*, size 24): the allocation holding
    // `offset` (flags 1: the first at or after it): s64 start, end, s32 type.
    // Types are not kept: 0.
    case S::direct_query: {
        if (!c.rdx || c.rcx != 24) return kernel_error(inval);
        auto it = direct_.upper_bound(c.rdi);
        if (it != direct_.begin() && std::prev(it)->first + std::prev(it)->second > c.rdi) --it;
        else if (signed32(c.rsi) != 1) return kernel_error(noent);
        if (it == direct_.end()) return kernel_error(noent);
        uint8_t info[24]{};
        put_le(info, 0, it->first, 8); put_le(info, 8, it->first + it->second, 8);
        return memory_.write(c.rdx, info, sizeof(info)) ? 0 : kernel_error(fault);
    }
    // QueryMemoryProtection(address, void** start, void** end, s32* protection).
    case S::query_protection: {
        uint64_t start = 0, end = 0;
        uint32_t protection = 0;
        if (!dynamic_->query(c.rdi, false, start, end, protection)) return kernel_error(access);
        if ((c.rsi && !write_pointer(c.rsi, start)) || (c.rdx && !write_pointer(c.rdx, end)) ||
            (c.rcx && !put(c.rcx, protection, 4))) return kernel_error(fault);
        return 0;
    }
    // sceKernelReleaseDirectMemory(start, length): whole allocations in the range.
    case S::direct_release: {
        if (!aligned(c.rdi) || !c.rsi || !aligned(c.rsi) || c.rsi > UINT64_MAX - c.rdi) return kernel_error(inval);
        // Only the range: what an allocation keeps on either side stays allocated.
        const uint64_t end = c.rdi + c.rsi;
        bool released = false;
        auto it = direct_.upper_bound(c.rdi);
        if (it != direct_.begin()) --it;
        while (it != direct_.end() && it->first < end) {
            const uint64_t at = it->first, past = at + it->second;
            if (past <= c.rdi) { ++it; continue; }
            direct_.erase(it);
            if (at < c.rdi) direct_[at] = c.rdi - at;
            if (past > end) direct_[end] = past - end;
            released = true;
            it = direct_.lower_bound(std::min(past, end));
        }
        return released ? 0 : kernel_error(noent);
    }
    // sceKernelMapDirectMemory(void** address, length, protection, flags, physical, alignment):
    // the physical range must be allocated; *address is a hint, or exact with MAP_FIXED.
    case S::direct_map: {
        uint64_t hint = 0;
        uint32_t protection = 0;
        if (!read_pointer(c.rdi, hint)) return kernel_error(fault);
        if (!c.rsi || !aligned(c.rsi) || !aligned(c.r8) || !cpu(c.rdx, protection)) return kernel_error(inval);
        auto owner = direct_.upper_bound(c.r8);
        if (owner == direct_.begin()) return kernel_error(inval);
        --owner;
        // Adjacent allocations form one range: Call of Duty pools them and
        // maps runs of pages across their boundaries.
        uint64_t reach = owner->first + owner->second;
        if (c.r8 >= reach) return kernel_error(inval);
        for (auto next = std::next(owner); next != direct_.end() && next->first == reach && c.rsi > reach - c.r8; ++next)
            reach += next->second;
        if (c.rsi > reach - c.r8) return kernel_error(inval);
        uint64_t mapped = 0;
        const Error e = dynamic_->place(hint, c.rsi, std::max<uint64_t>(c.r9, page), protection, (c.rcx & 0x10) != 0, mapped);
        if (e) return kernel_error(e);
        return write_pointer(c.rdi, mapped) ? 0 : (dynamic_->unmap(mapped, c.rsi), kernel_error(fault));
    }
    // sceKernelMapNamedFlexibleMemory(void** address, length, protection, flags, name).
    case S::flexible_map: {
        uint64_t hint = 0;
        uint32_t protection = 0;
        char name[32];
        if (!read_pointer(c.rdi, hint)) return kernel_error(fault);
        if (!c.r8 || !memory_.read_cstring(c.r8, name, sizeof(name))) return kernel_error(fault);
        if (!c.rsi || !aligned(c.rsi) || !cpu(c.rdx, protection)) return kernel_error(inval);
        uint64_t mapped = 0;
        const Error e = dynamic_->place(hint, c.rsi, page, protection, (c.rcx & 0x10) != 0, mapped);
        if (e) return kernel_error(e);
        return write_pointer(c.rdi, mapped) ? 0 : (dynamic_->unmap(mapped, c.rsi), kernel_error(fault));
    }
    // mmap(address, length, protection, flags, fd, offset): anonymous mappings only.
    case S::posix_mmap: {
        uint32_t protection = 0;
        if (!c.rsi || !cpu(c.rdx, protection)) return kernel_error(inval);
        if (!(c.rcx & 0x1000) || signed32(c.r8) != -1) return kernel_error(nosys);
        uint64_t mapped = 0;
        const Error e = dynamic_->place(c.rdi, c.rsi, page, protection, (c.rcx & 0x10) != 0, mapped);
        return e ? kernel_error(e) : int64_t(mapped);
    }
    default: return kernel_error(nosys);
    }
}
bool PS4Libkernel::initialize_data(PS4KernelService service, uint64_t address, uint64_t size) {
    switch (service) {
    case PS4KernelService::stack_chk_guard: {
        std::random_device random;
        const uint64_t guard = (uint64_t(random()) << 32) | random();
        uint8_t wire[8];
        put_le(wire, 0, guard, 8);
        return size >= 8 && memory_.write(address, wire, 8);
    }
    case PS4KernelService::progname: {
        // char* __progname, pointing at the name stored right after it.
        static constexpr char name[] = "eboot.bin";
        uint8_t wire[8];
        put_le(wire, 0, address + 16, 8);
        return size >= 16 + sizeof(name) && memory_.write(address, wire, 8) &&
            memory_.write(address + 16, name, sizeof(name));
    }
    case PS4KernelService::errno_table:
        errno_table_ = address;
        errno_table_size_ = size;
        return true;
    default: return false;
    }
}
int64_t PS4Libkernel::invoke_service(PS4KernelService service, const PS4KernelCall& c, uint64_t thread) {
    // Thread services use only PS4Threads (its own lock): a thread blocked in
    // a lock or a wait must not hold up the thread that would release it.
    switch (service) {
    case PS4KernelService::thread_self: return threads_.known(thread) ? int64_t(thread) : kernel_error(inval);
    case PS4KernelService::mutex_init:
        return threads_.known(thread) ? kernel_error(threads_.init(c.rdi, c.rsi, c.rdx)) : kernel_error(inval);
    case PS4KernelService::mutex_trylock: return kernel_error(threads_.trylock(c.rdi, thread));
    case PS4KernelService::mutex_lock: return kernel_error(threads_.lock(c.rdi, thread));
    case PS4KernelService::mutex_unlock: return kernel_error(threads_.unlock(c.rdi, thread));
    case PS4KernelService::mutex_destroy:
        return threads_.known(thread) ? kernel_error(threads_.destroy(c.rdi)) : kernel_error(inval);
    case PS4KernelService::cond_init:
        return threads_.known(thread) ? kernel_error(threads_.cond_init(c.rdi, c.rsi, c.rdx)) : kernel_error(inval);
    case PS4KernelService::cond_destroy:
        return threads_.known(thread) ? kernel_error(threads_.cond_destroy(c.rdi)) : kernel_error(inval);
    case PS4KernelService::cond_wait: return kernel_error(threads_.cond_wait(c.rdi, c.rsi, thread, false, 0));
    // scePthreadCondTimedwait(cond, mutex, usec): a relative timeout.
    case PS4KernelService::cond_timedwait: return kernel_error(threads_.cond_wait(c.rdi, c.rsi, thread, true, c.rdx));
    case PS4KernelService::cond_signal:
    case PS4KernelService::cond_broadcast:
        return threads_.known(thread) ?
            kernel_error(threads_.cond_signal(c.rdi, service == PS4KernelService::cond_broadcast)) : kernel_error(inval);
    case PS4KernelService::attr_init: return kernel_error(threads_.attr_init(c.rdi));
    case PS4KernelService::attr_destroy: return kernel_error(threads_.attr_destroy(c.rdi));
    case PS4KernelService::attr_set_detach: return kernel_error(threads_.attr_set_detach(c.rdi, c.rsi));
    case PS4KernelService::attr_get_detach: return kernel_error(threads_.attr_get_detach(c.rdi, c.rsi));
    case PS4KernelService::attr_set_stack: return kernel_error(threads_.attr_set_stack(c.rdi, c.rsi));
    case PS4KernelService::attr_get_stack: return kernel_error(threads_.attr_get_stack(c.rdi, c.rsi));
    case PS4KernelService::attr_accept: return kernel_error(threads_.attr_accept(c.rdi));
    case PS4KernelService::mutexattr_init: return kernel_error(threads_.mutexattr_init(c.rdi));
    case PS4KernelService::mutexattr_destroy: return kernel_error(threads_.mutexattr_destroy(c.rdi));
    case PS4KernelService::mutexattr_settype: return kernel_error(threads_.mutexattr_settype(c.rdi, c.rsi));
    case PS4KernelService::mutexattr_gettype: return kernel_error(threads_.mutexattr_gettype(c.rdi, c.rsi));
    case PS4KernelService::mutexattr_accept: return kernel_error(threads_.mutexattr_accept(c.rdi));
    default: break;
    }
    std::lock_guard<std::mutex> guard(lock_);
    try {
        if (!threads_.known(thread)) return kernel_error(inval);
        switch (service) {
        case PS4KernelService::open: return open(c);
        case PS4KernelService::read: return read(c);
        case PS4KernelService::close: return close(signed32(c.rdi));
        case PS4KernelService::lseek: return lseek(c);
        case PS4KernelService::stat: return stat(c, false);
        case PS4KernelService::fstat: return stat(c, true);
        case PS4KernelService::mmap: return mmap(c);
        case PS4KernelService::munmap: return kernel_error(dynamic_ ? dynamic_->unmap(c.rdi, c.rsi) : nosys);
        case PS4KernelService::mprotect: return kernel_error(dynamic_ ? dynamic_->protect(c.rdi, c.rsi, uint32_t(c.rdx)) : nosys);
        case PS4KernelService::clock_gettime: return clock(c, false);
        case PS4KernelService::clock_getres: return clock(c, true);
        case PS4KernelService::gettimeofday: return gettimeofday(c.rdi);
        case PS4KernelService::process_time: {
            const uint64_t now = monotonic_us();
            return now >= started_us_ ? int64_t(now - started_us_) : 0;
        }
        default: return kernel_error(nosys);
        }
    } catch (const std::bad_alloc&) { return kernel_error(nomem); }
}
} // namespace maxps4
extern "C" uint32_t maxps4_ps4_resolve_libkernel(const char* symbol, const char* library,
                                               const char* module, uint32_t version) {
    if (!symbol || !library || !module) return 0;
    return uint32_t(maxps4::resolve_libkernel(symbol, library, module, version));
}
