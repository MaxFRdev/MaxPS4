#pragma once
#include "maxps4_ps4_abi.h"
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_threads.h"
#include "maxps4_ps4_videoout.h"
#include "maxps4_ps4_user_service.h"
#include "maxps4_ps4_pad.h"
#include "maxps4_ps4_audio_out.h"
#include "maxps4_ps4_gnm.h"
#include "maxps4_ps4_gnm_queue.h"
#include <array>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <string_view>

namespace maxps4 {
// Entry snapshot for PS4 x86-64 System V calls. Host function addresses must
// never be installed directly into guest GOT/PLT slots on ARM64.
struct PS4KernelCall { uint64_t rdi = 0, rsi = 0, rdx = 0, rcx = 0, r8 = 0, r9 = 0, rsp = 0; };
enum class PS4KernelService : uint32_t {
    unresolved, open, read, close, lseek, stat, fstat,
    mmap, munmap, mprotect, clock_gettime, clock_getres, gettimeofday, process_time,
    thread_self, mutex_init, mutex_trylock, mutex_unlock, mutex_destroy,
    // Needs the image's TLS layout: served by the FEX Orbis HLE gate, not invoke().
    tls_get_addr,
    // Engine-internal, no NID: the exit function a general image's entry gets
    // in RSI. Served by the FEX Orbis HLE gate, which ends the run.
    process_exit,
    // Guest threads run on FEX: served by its Orbis HLE gate for general
    // images; invoke() answers ENOSYS. thread_exit is also the service of the
    // engine's thread-return trampoline.
    thread_create, thread_join, thread_exit,
    // Blocking synchronization (PS4Threads), served by invoke() without the
    // libkernel lock so that a blocked thread never holds up the others.
    mutex_lock, cond_init, cond_destroy, cond_wait, cond_timedwait, cond_signal, cond_broadcast,
    // scePthreadDetach: a guest thread service, like thread_create.
    thread_detach,
    // Thread and mutex attribute objects (PS4Threads). *_accept validates the
    // object for a setting that does not change anything here.
    attr_init, attr_destroy, attr_set_detach, attr_get_detach, attr_set_stack, attr_get_stack, attr_accept,
    mutexattr_init, mutexattr_destroy, mutexattr_settype, mutexattr_gettype, mutexattr_accept,
    // What a game's own libc.prx needs from libkernel (shadPS4 names):
    // __Error (the calling thread's errno cell), stack_chk_fail and
    // sceKernelDebugRaiseException (end the process like the exit function,
    // with status 134), sleeps and yields, thread identity and priority,
    // thread-specific keys, read/write locks, signals (no delivery: masks
    // and handlers are accepted), process facts, time zone (UTC).
    errno_location, abort_process, nanosleep, usleep, page_size, yield, thread_equal,
    key_create, key_delete, get_specific, set_specific,
    rwlock_init, rwlock_destroy, rwlock_rdlock, rwlock_wrlock, rwlock_tryrdlock, rwlock_trywrlock, rwlock_unlock,
    thread_getprio, thread_getaffinity, attr_get_np, attr_getaffinity,
    sigfillset, signal, utc_to_local, local_to_utc, proc_param, module_info_from_addr,
    // Memory: direct memory (physical ranges, then mapped as zeroed host
    // pages; no aliasing), flexible memory and POSIX mmap.
    direct_size, direct_allocate, direct_release, direct_map, flexible_map, posix_mmap,
    // Files beyond read-only /app0: writes (stdout/stderr go to the engine log),
    // positioned I/O, sync, truncation, directories and their entries.
    write, pread, pwrite, fsync, ftruncate, truncate, mkdir, rmdir, rename, unlink, getdirentries,
    // Accepted without effect, answering 0 (masks, priorities, heap API, ...).
    succeed,
    // Data objects (PS4KernelExport::data): filled by initialize_data().
    stack_chk_guard, progname, errno_table,
    // Guest code (PS4KernelExport::code), never a SYSCALL: pthread_once calls
    // its init routine from guest code.
    once,
    equeue_create, equeue_delete, equeue_wait, user_event_add, user_event_add_edge,
    user_event_delete, user_event_trigger, event_id, event_filter, event_data,
    event_fflags, event_error, event_user_data,
    graphics_event_add, graphics_event_delete,
    video_open, video_close, video_attribute, video_register, video_unregister,
    video_submit, video_pending, video_flip_status, video_vblank_status,
    video_add_flip, video_add_vblank, video_delete_flip, video_delete_vblank,
    video_event_id, video_event_data, video_event_count, video_resolution, video_flip_rate,
    save_init, save_term, save_mount2, save_umount,
    save_mount1, save_umount2, save_search, save_set_param, save_get_param, save_save_icon, save_load_icon,
    save_delete, save_mount_info, save_event, save_progress, save_memory_setup, save_memory_get, save_memory_set,
    sema_create, sema_delete, sema_wait, sema_poll, sema_signal, sema_cancel,
    flag_create, flag_delete, flag_set, flag_clear, flag_wait, flag_poll, flag_cancel,
    process_counter, counter_frequency, kernel_sleep,
    np_state, np_signed_out, np_create_request, app_init, app_addcont_list, app_param_int,
    app_addcont_info, app_addcont_mount, app_addcont_unmount,
    module_load_start, module_dlsym, module_list, module_info_unwind, module_info, module_info_ex,
    direct_available, direct_allocate_main, flexible_map_unnamed, flexible_release,
    virtual_query, query_protection, sdk_version, system_version,
    msg_init, msg_open, msg_status, msg_result, msg_close, msg_term,
    error_init, error_open, error_status, error_close, error_term,
    netctl_state, netctl_info, netctl_callback, net_pool, audio_outputs, audio_port_state,
    playgo_open, playgo_locus, playgo_progress, playgo_todo, playgo_eta, playgo_speed, playgo_language,
    playgo_chunks, random_number, sandbox_word,
    app_temp_mount, rtc_current_tick, rtc_current_clock, rtc_current_local, rtc_get_tick, rtc_set_tick,
    rtc_resolution, rtc_utc_to_local, safe_area, trophy_create_context, trophy_create_handle, trophy_unlock,
    save_dialog_init, save_dialog_open, save_dialog_update, save_dialog_result, save_dialog_term,
    user_initialize, user_terminate, user_initial, user_login_list, user_event, user_name,
    pad_init, pad_open, pad_close, pad_read, pad_read_state, pad_information, pad_handle,
    gnm_draw_auto, gnm_draw_index, gnm_vgt, gnm_vs, gnm_ps350,
    gnm_ps, gnm_cs_plain, gnm_draw_index_offset, gnm_draw_indirect, gnm_draw_index_indirect,
    gnm_dispatch_indirect, gnm_dispatch_init, gnm_context_init, gnm_context_init400,
    gnm_reset_vgt, gnm_set_marker, gnm_clock,
    gnm_submit, gnm_allowed, gnm_done,
    // sceGnmSubmitAndFlipCommandBuffers: null GPU backend only (the flip is
    // requested once the work is retired, at submission).
    gnm_submit_flip,
    gnm_cs, gnm_dispatch, gnm_embedded_vs, gnm_wait_flip, gnm_push_marker, gnm_pop_marker, gnm_init350,
    // What a game's own libc.prx needs from libSceLibcInternal.
    heap_trace_info,
    // libSceSystemService (shadPS4 system_service.cpp): settings of an
    // English console, a foreground application status, no system events.
    system_param_int, system_status, system_event,
    // libSceAudioOut (PS4AudioOut). Output blocks for one buffer's duration.
    audio_init, audio_open, audio_close, audio_output,
    // Append new services to preserve existing trampoline service IDs.
    reserve_range, mtype_protect, direct_query, batch_map, batch_map2, gnm_unsupported, gnm_gs_rings,
    getdents, flexible_available, prt_aperture, aio_read, aio_write, aio_wait,
    pool_reserve, pool_commit, pool_decommit, pool_expand, thread_id, process_id,
    sched_priority_max, sched_priority_min, sched_param_get, net_unavailable,
    discmap_no_bitmap, discmap_flags, video_capability,
    gnm_workload_stream, gnm_workload_begin, gnm_workload_end, gnm_submit_workload, gnm_submit_flip_workload,
    gnm_map_compute, gnm_unmap_compute, gnm_ding_dong, gnm_tessellation_offchip,
    gnm_ls, gnm_hs, gnm_hs_update, gnm_es, gnm_gs, gnm_push_color_marker, gnm_set_color_marker,
    gnm_ding_dong_marker, gnm_embedded_ps, gnm_compute_wait_address, gnm_compute_wait_semaphore,
    gnm_dispatch_indirect_mec, thread_getname,
    trophy_game_info, trophy_group_info, trophy_unlock_state, rtc_time_t,
    attr_getschedparam, mutex_reltimedlock, sem_init, sem_destroy, sem_wait, sem_post, uuid_create,
    net_pton, net_ntop
};
// Token bit of a trampoline whose function follows the POSIX convention: -1
// with the error in the calling thread's errno (__Error) instead of an Orbis
// kernel error code.
constexpr uint32_t posix_token = 0x40000000;
// Token bit of a POSIX pthread_* trampoline over a scePthread* service: the
// result is returned as an errno number (pthread_result), the services' name
// arguments are absent (passed as 0) and pthread_cond_timedwait's deadline is
// absolute (CLOCK_REALTIME) instead of a relative microsecond count.
constexpr uint32_t pthread_token = 0x10000000;
constexpr int64_t pthread_result(int64_t result) {
    return result <= -int64_t(0x7ffe0000) + 0xffff && result > -int64_t(0x7ffe0000) ? result + 0x7ffe0000 : result;
}
// Tokens from missing_import_token on are the boot probe's stubs (see
// maxps4_fex_engine_set_import_probe): token - missing_import_token indexes
// the engine's list of stubbed imports. Never a PS4KernelService.
constexpr uint32_t missing_import_token = 0x20000000;
// Services only general images get: elsewhere their imports stay unresolved.
// A condition wait needs another thread to end it.
constexpr bool general_only(PS4KernelService service) {
    return service == PS4KernelService::thread_create || service == PS4KernelService::thread_join ||
           service == PS4KernelService::thread_exit || service == PS4KernelService::thread_detach ||
           service == PS4KernelService::cond_wait || service == PS4KernelService::cond_timedwait ||
           service == PS4KernelService::equeue_wait || service == PS4KernelService::video_attribute ||
           service == PS4KernelService::sema_wait || service == PS4KernelService::flag_wait;
}
// Implemented libkernel functions (name, NID, service), e.g. for an HLE registry.
// `posix`: POSIX convention (see posix_token). `noreturn`: the trampoline ends
// with HLT. `data`: a data object of that many bytes instead of a function.
// `code`: a guest routine of `code_size` bytes instead of a trampoline.
struct PS4KernelExport {
    const char* name; const char* nid; PS4KernelService service;
    const char* library = "libkernel";
    bool posix = false, noreturn = false;
    uint32_t data = 0;
    const uint8_t* code = nullptr;
    size_t code_size = 0;
    const char* module = nullptr; // When it is not the library's own name.
};
const PS4KernelExport* libkernel_exports(size_t& count);
// The NID of a PS4 symbol name (what sceKernelDlsym looks up).
std::string ps4_nid(const std::string& name);
// Library/module/version are decoded by an ELF/PRX loader before resolution.
// This resolver deliberately does not guess the meaning of encoded # IDs.
PS4KernelService resolve_libkernel(std::string_view symbol,
    std::string_view library, std::string_view module, uint32_t version);

class PS4Libkernel final {
public:
    static constexpr size_t descriptor_limit = 256;
    static constexpr size_t transfer_limit = 1024 * 1024;
    static constexpr size_t stat_size = 120;
    explicit PS4Libkernel(PS4GuestMemory& memory, PS4VirtualMemory* dynamic = nullptr);
    ~PS4Libkernel();
    PS4Libkernel(const PS4Libkernel&) = delete;
    PS4Libkernel& operator=(const PS4Libkernel&) = delete;
    // Host-side mount configuration, never a guest pathname: /app0 (read-only),
    // /savedata0, /download0 and /temp0 (writable). A failed mount preserves
    // the previous one. A successful remount closes guest descriptors.
    int64_t mount(const char* guest, const char* host_directory);
    int64_t mount_app0(const char* host_directory) { return mount("/app0", host_directory); }
    // A game update over /app0: a path the update holds is read from it, any
    // other from the game. Read-only, like /app0; cleared by reset() or a remount.
    int64_t mount_app0_update(const char* host_directory);
    // Host directory holding this game's save directories: sceSaveDataMount2
    // mounts <root>/<dirName> at /savedata0. Cleared by reset().
    int64_t set_savedata_root(const char* host_directory);
    // The console language games read (sceSystemServiceParamGetInt 1): a PS4
    // language ID, 1 (English US) until set. Cleared by reset().
    void set_system_language(int32_t language) { system_language_ = language; }
    // An installed add-on content: its 16-character entitlement label and the
    // host directory sceAppContentAddcontMount mounts at /addcont0. Cleared by reset().
    int64_t add_addcont(const char* label, const char* host_directory);
    // The game's modules as loaded, for sceKernelLoadStartModule (by file
    // name; handles are 1-based positions), sceKernelDlsym (exports by NID),
    // sceKernelGetModuleList and sceKernelGetModuleInfoForUnwind (by address).
    struct LoadedModule {
        std::string name;
        uint64_t base = 0, end = 0, eh_frame_hdr = 0, eh_frame_hdr_size = 0;
        std::map<std::string, uint64_t> exports;
        struct Segment { uint64_t address, size; uint32_t protection; };
        std::vector<Segment> segments;
    };
    void set_modules(std::vector<LoadedModule> modules) { std::lock_guard<std::mutex> guard(lock_); modules_ = std::move(modules); }
    void reset();
    // `posix`: the trampoline's token had posix_token; an Orbis error result
    // becomes -1 with that error in the thread's errno cell.
    int64_t invoke(PS4KernelService service, const PS4KernelCall& call, uint64_t thread = 1, bool posix = false);
    // Fills a linked data object (stack_chk_guard, progname, errno_table) at
    // its guest address. Returns false for an unknown service or bad memory.
    bool initialize_data(PS4KernelService service, uint64_t address, uint64_t size);
    // The root module's PT_SCE_PROCPARAM (sceKernelGetProcParam), 0 if none.
    void set_process_param(uint64_t address) { process_param_ = address; }
    PS4Threads& threads() { return threads_; }
    PS4VideoOut& videoout() { return videoout_; }
    PS4Pad& pad() { return pad_; }
    PS4AudioOut& audio() { return audio_; }
    PS4GnmQueue& graphics() { return graphics_; }
    // The last console_limit bytes the game wrote to its stdout and stderr.
    std::string console() { std::lock_guard<std::mutex> guard(lock_); return std::string(console_.begin(), console_.end()); }
    static constexpr size_t console_limit = 4096;
    static constexpr int32_t abort_status = 134; // As a process killed by SIGABRT.
private:
    PS4GuestMemory& memory_;
    PS4VirtualMemory* dynamic_;
    PS4Threads threads_;
    PS4VideoOut videoout_;
    PS4UserService user_service_;
    PS4Pad pad_;
    PS4AudioOut audio_;
    PS4Gnm gnm_;
    PS4GnmQueue graphics_;
    uint64_t errno_table_ = 0, errno_table_size_ = 0, process_param_ = 0;
    uint64_t heap_trace_ = 0; // sceLibcHeapGetTraceInfo's zeroed tables, mapped on first use.
    std::vector<char> console_;
    // sceGnmMapComputeQueue rings (vqid = index + 1): base, size and read
    // pointer the game gave, and how far DingDong has consumed it (dwords).
    struct ComputeQueue { uint64_t ring = 0, read_pointer = 0; uint32_t size = 0, offset = 0; };
    std::array<ComputeQueue, 64> compute_{};
    std::map<uint64_t, uint64_t> direct_; // Allocated physical ranges: start -> length.
    std::string save_root_;
    int32_t system_language_ = 1;
    uint32_t next_np_request_ = 0x10000001;
    uint32_t save_dialog_status_ = 0; // OrbisCommonDialogStatus: none, initialized, running, finished.
    uint32_t msg_dialog_status_ = 0, error_dialog_status_ = 0; // The same, for sceMsgDialog / sceErrorDialog.
    uint64_t sandbox_word_ = 0; // Guest address of the sandbox word (mapped on first use).
    // `v1`: sceSaveDataMount's layout (dirName pointer at 16, mode at 40)
    // instead of sceSaveDataMount2's (8 and 24).
    int64_t save_mount(uint64_t request, uint64_t result, bool v1 = false);
    int64_t save_service(PS4KernelService service, const PS4KernelCall& call);
    std::string save_dir_; // Host directory mounted at /savedata0, empty when none.
    int64_t save_umount(uint64_t mount_point);
    int64_t temp_mount(uint64_t mount_point);
    std::vector<LoadedModule> modules_;
    int64_t module_service(PS4KernelService service, const PS4KernelCall& call);
    void eh_frame_of(const LoadedModule& module, uint64_t& eh_frame, uint64_t& eh_size) const;
    struct Addcont { std::string label, path; };
    std::vector<Addcont> addcont_;
    // The add-on content named by a guest OrbisNpUnifiedEntitlementLabel, or null.
    const Addcont* find_addcont(uint64_t label) const;
    int64_t addcont_list(uint64_t list, uint32_t count, uint64_t hit);
    int64_t addcont_info(uint64_t label, uint64_t info);
    int64_t addcont_mount(uint64_t label, uint64_t mount_point);
    int64_t addcont_unmount(uint64_t mount_point);
    int64_t rtc(PS4KernelService service, const PS4KernelCall& call);
    int64_t memory_service(PS4KernelService service, const PS4KernelCall& call);
    int64_t posix_result(int64_t result, uint64_t thread);
    int64_t invoke_service(PS4KernelService service, const PS4KernelCall& call, uint64_t thread);
    int64_t extended(PS4KernelService service, const PS4KernelCall& call, uint64_t thread, bool& handled);
    std::mutex lock_;
    struct Mount { const char* prefix; bool writable; int root; int overlay = -1; };
    std::array<Mount, 5> mounts_{{{"/app0", false, -1}, {"/savedata0", true, -1},
                                  {"/download0", true, -1}, {"/temp0", true, -1}, {"/addcont0", false, -1}}};
    // A guest descriptor: its host descriptor, access, and for a directory
    // the FreeBSD dirent records (512-byte blocks) read by getdirentries.
    struct File {
        int fd = -1;
        bool readable = false, writable = false, directory = false, listed = false, read_only_mount = true;
        std::vector<uint8_t> entries;
        uint64_t position = 0;
    };
    std::array<File, descriptor_limit> files_;
    void close_files();
    File* file(int32_t descriptor);
    // A validated guest path: its mount and the path inside it ("" for the root).
    ps4::Error resolve(uint64_t address, size_t& mount, char* relative) const;
    // The directory holding `relative`'s last component (a new host
    // descriptor) and that component ("." for the mount root). Every
    // component is opened without following symbolic links.
    int parent(size_t mount, const char* relative, char* leaf, ps4::Error& error) const;
    int parent_from(int root, const char* relative, char* leaf, ps4::Error& error) const;
    int64_t file_service(PS4KernelService service, const PS4KernelCall& call);
    int64_t open(const PS4KernelCall& call);
    int64_t read(const PS4KernelCall& call);
    int64_t close(int32_t descriptor);
    int64_t lseek(const PS4KernelCall& call);
    int64_t stat(const PS4KernelCall& call, bool by_descriptor);
    int64_t mmap(const PS4KernelCall& call);
    int64_t clock(const PS4KernelCall& call, bool resolution);
    int64_t gettimeofday(uint64_t output);
    uint64_t started_us_ = 0;
    bool writable(uint64_t address, size_t size) const;
};
} // namespace maxps4
