#pragma once
#include "maxps4_ps4_abi.h"
#include "maxps4_ps4_guest_memory.h"
#include <array>
#include <condition_variable>
#include <mutex>

namespace maxps4 {
// Guest thread identities, thread and mutex attributes, mutexes and condition
// variables. Guest threads themselves run on FEX (GuestThreads in
// maxps4_fex_runtime.cpp), which attaches/detaches them here; x86 code never
// runs natively on a host thread. Every object lives behind an opaque 64-bit
// token in its guest slot, never a host pointer. With an object arena (general
// images), a token is the guest address of a zeroed object_size block, as on a
// PS4 where the slot points to the object: a game's libc writes into it.
// A guest slot holding 0 (or 1 for a mutex: the adaptive initializer) is a
// static initializer: the object is created on first use. Destroyed objects
// leave 2 in their slot, which stays invalid.
class PS4Threads final {
public:
    // PS4 mutex types (scePthreadMutexattrSettype). Error-checking is the default.
    enum MutexType : uint32_t { error_check = 1, recursive = 2, normal = 3, adaptive = 4 };
    static constexpr uint64_t default_stack = 1024 * 1024, minimum_stack = 16 * 1024;
    explicit PS4Threads(PS4GuestMemory& memory) : memory_(memory) { threads_[0] = 1; }
    uint64_t attach();
    ps4::Error detach(uint64_t thread);
    bool known(uint64_t thread) const;
    // Thread attributes: detach state (0 joinable, 1 detached) and stack size.
    // Scheduling, affinity and guard size are accepted on a valid attribute
    // object (attr_accept) and do not change anything here.
    ps4::Error attr_init(uint64_t slot);
    ps4::Error attr_destroy(uint64_t slot);
    ps4::Error attr_set_detach(uint64_t slot, uint64_t state);
    ps4::Error attr_get_detach(uint64_t slot, uint64_t output);
    ps4::Error attr_set_stack(uint64_t slot, uint64_t bytes);
    ps4::Error attr_get_stack(uint64_t slot, uint64_t output);
    ps4::Error attr_accept(uint64_t slot);
    // What scePthreadCreate gets from its attribute argument (0: defaults).
    ps4::Error thread_attributes(uint64_t slot, bool& detached, uint64_t& stack);
    // Mutex attributes: the type; the protocol is accepted (mutexattr_accept).
    ps4::Error mutexattr_init(uint64_t slot);
    ps4::Error mutexattr_destroy(uint64_t slot);
    ps4::Error mutexattr_settype(uint64_t slot, uint64_t type);
    ps4::Error mutexattr_gettype(uint64_t slot, uint64_t output);
    ps4::Error mutexattr_accept(uint64_t slot);
    ps4::Error init(uint64_t slot, uint64_t attributes, uint64_t name);
    ps4::Error trylock(uint64_t slot, uint64_t thread);
    // Blocks until the mutex is free. When the caller owns it: a recursive
    // mutex counts once more, an error-checking or adaptive one gives EDEADLK
    // and a normal one blocks (until interrupt()), as POSIX says.
    ps4::Error lock(uint64_t slot, uint64_t thread);
    ps4::Error unlock(uint64_t slot, uint64_t thread);
    ps4::Error destroy(uint64_t slot);
    // Condition attributes (clock, process sharing) do not change waits here.
    ps4::Error cond_init(uint64_t slot, uint64_t attributes, uint64_t name);
    ps4::Error cond_destroy(uint64_t slot);
    // Releases the caller's mutex (every recursive count), waits for a signal
    // (or `timeout_us` when `timed`), then takes the mutex again with the same
    // count. ETIMEDOUT still retakes it.
    ps4::Error cond_wait(uint64_t cond, uint64_t mutex, uint64_t thread, bool timed, uint64_t timeout_us);
    ps4::Error cond_signal(uint64_t slot, bool all);
    // Read/write locks: any number of readers or one writer. A slot holding 0
    // is a static initializer. Attributes are ignored.
    ps4::Error rwlock_init(uint64_t slot);
    ps4::Error rwlock_destroy(uint64_t slot);
    ps4::Error rwlock_lock(uint64_t slot, uint64_t thread, bool write, bool wait);
    ps4::Error rwlock_unlock(uint64_t slot, uint64_t thread);
    // Thread-specific data: 256 keys, a value per thread (0 until set).
    // Destructors are recorded, not run when a thread ends.
    ps4::Error key_create(uint64_t output, uint64_t destructor);
    ps4::Error key_delete(uint64_t key);
    uint64_t get_specific(uint64_t key, uint64_t thread) const;
    ps4::Error set_specific(uint64_t key, uint64_t value, uint64_t thread);
    // Position of a known thread in the identity table (0 for the main
    // thread), or thread_limit.
    size_t index(uint64_t thread) const;
    // Sleeps, returning EINTR early if interrupt() comes first.
    ps4::Error sleep(uint64_t microseconds);
    // Orbis event queues and user events. Queue handles are values, not slots.
    // Waits share the interruption mechanism of guest synchronization.
    static constexpr size_t equeue_limit = 32, event_limit = 64, event_size = 32;
    ps4::Error equeue_create(uint64_t output, uint64_t name);
    ps4::Error equeue_delete(uint64_t handle);
    ps4::Error user_event_add(uint64_t handle, int32_t id, bool edge);
    ps4::Error user_event_delete(uint64_t handle, int32_t id);
    ps4::Error user_event_trigger(uint64_t handle, int32_t id, uint64_t data);
    // GNM completion events (0..6 compute release-memory, 0x40 graphics EOP).
    // A backend captures a ticket when it schedules work, then signals that
    // ticket only after completion. Delete/re-register/reset invalidate it.
    ps4::Error graphics_event_add(uint64_t handle, uint64_t id, uint64_t user_data);
    ps4::Error graphics_event_delete(uint64_t handle, uint64_t id);
    ps4::Error graphics_event_ticket(uint64_t handle, uint64_t id, uint64_t& ticket);
    ps4::Error graphics_event_complete(uint64_t ticket);
    // Kernel semaphores and event flags (sceKernel*Sema, sceKernel*EventFlag):
    // handles are values written to the caller's output, like queues. A wait
    // with a `timeout` reads the microseconds left there and writes back what
    // remains (0 on ETIMEDOUT); cancel and delete end waits with ECANCELED.
    static constexpr size_t sema_limit = 64, flag_limit = 64;
    ps4::Error sema_create(uint64_t output, uint64_t name, int32_t initial, int32_t max);
    ps4::Error sema_delete(uint64_t handle);
    // `poll`: EBUSY instead of waiting.
    ps4::Error sema_wait(uint64_t handle, int32_t count, uint64_t timeout, bool poll);
    ps4::Error sema_signal(uint64_t handle, int32_t count);
    ps4::Error sema_cancel(uint64_t handle, int32_t count, uint64_t waiters);
    // Wait modes: and 1, or 2, then clear all 0x10 or the waited bits 0x20.
    // Attribute 0x10 (single): a second concurrent waiter gets EPERM.
    ps4::Error flag_create(uint64_t output, uint64_t name, uint32_t attributes, uint64_t pattern);
    ps4::Error flag_delete(uint64_t handle);
    // `clear`: sceKernelClearEventFlag (pattern &= bits) instead of |= bits.
    ps4::Error flag_set(uint64_t handle, uint64_t bits, bool clear);
    ps4::Error flag_wait(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t result, uint64_t timeout, bool poll);
    ps4::Error flag_cancel(uint64_t handle, uint64_t pattern, uint64_t waiters);
    // An end-of-pipe interrupt: triggers every registered EOP event (id 0x40).
    void graphics_interrupt();
    ps4::Error video_event_add(uint64_t queue, uint64_t source, uint64_t id, uint64_t user_data);
    ps4::Error video_event_delete(uint64_t queue, uint64_t source, uint64_t id);
    void video_event_remove_source(uint64_t source);
    void video_event_notify(uint64_t source, uint64_t id, uint64_t argument, uint64_t timestamp);
    ps4::Error equeue_wait(uint64_t handle, uint64_t events, int32_t count,
                          uint64_t output, uint64_t timeout);
    // Wakes every blocked lock and wait with EINTR until resume(): a stopping
    // run's threads go back to guest code, where the stop signal lands.
    void interrupt();
    void resume();
    // Writable guest range that later mutex, attribute, condition and rwlock
    // tokens are carved from, never reused (reset() forgets it).
    void set_object_arena(uint64_t base, uint64_t size);
    static constexpr uint64_t object_size = 64;
    void reset();
    // Unreal Engine games create dozens of threads and thousands of locks.
    // thread_limit: the main thread and GuestELF::generalThreads children.
    static constexpr size_t thread_limit = 128, mutex_limit = 2048, cond_limit = 2048, attr_limit = 256,
                            key_limit = 256, rwlock_limit = 512;
private:
    struct Mutex { uint64_t token = 0, owner = 0, slot = 0; uint32_t type = error_check, count = 0; };
    // A signal or broadcast starts a new generation with `wakeups` waiters of
    // older generations allowed to return.
    struct Cond { uint64_t token = 0, slot = 0, generation = 0; uint32_t waiters = 0, wakeups = 0; };
    struct Attr { uint64_t token = 0, slot = 0, stack = default_stack; bool detached = false; };
    struct MutexAttr { uint64_t token = 0, slot = 0; uint32_t type = error_check; };
    PS4GuestMemory& memory_;
    mutable std::mutex guard_;
    std::condition_variable changed_;
    std::array<uint64_t, thread_limit> threads_{};
    std::array<Mutex, mutex_limit> mutexes_{};
    std::array<Cond, cond_limit> conds_{};
    std::array<Attr, attr_limit> attrs_{};
    std::array<MutexAttr, attr_limit> mutexattrs_{};
    struct RwLock { uint64_t token = 0, slot = 0, writer = 0; uint32_t readers = 0; };
    std::array<RwLock, rwlock_limit> rwlocks_{};
    struct Key { bool used = false; uint64_t destructor = 0; };
    std::array<Key, key_limit> keys_{};
    struct Event {
        bool used = false, edge = false, triggered = false;
        uint64_t id = 0;
        uint64_t user_data = 0, data = 0, ticket = 0;
        int16_t filter = -11;
        uint64_t source = 0;
    };
    struct Equeue { uint64_t token = 0; std::array<Event, event_limit> events{}; };
    std::array<Equeue, equeue_limit> equeues_{};
    Equeue* find_equeue(uint64_t handle);
    struct Sema { uint64_t token = 0, generation = 0; int32_t count = 0, max = 0; uint32_t waiters = 0; };
    struct Flag { uint64_t token = 0, generation = 0, pattern = 0; uint32_t attributes = 0, waiters = 0; };
    std::array<Sema, sema_limit> semas_{};
    std::array<Flag, flag_limit> flags_{};
    // A readable name of at most 32 bytes (0: none).
    ps4::Error check_name(uint64_t name) const;
    // Waits on changed_ until `ready`, for the microseconds at `timeout` when
    // non-zero; writes back what remains. True when it timed out.
    template <typename Ready> bool wait_timeout(std::unique_lock<std::mutex>& lock, uint64_t timeout, uint64_t micros, Ready ready);
    // values_[thread index][key].
    std::array<std::array<uint64_t, key_limit>, thread_limit> values_{};
    RwLock* acquire_rwlock(uint64_t slot, ps4::Error& error);
    uint64_t next_thread_ = 2, next_mutex_ = 0x10000;
    uint64_t arena_next_ = 0, arena_end_ = 0; // arena_end_ 0: no arena.
    // A new object token (0: none left); see set_object_arena.
    uint64_t next_token();
    bool interrupted_ = false;
    bool known_locked(uint64_t thread) const;
    bool writable(uint64_t slot) const;
    bool read_token(uint64_t slot, uint64_t& token) const;
    bool write_token(uint64_t slot, uint64_t token);
    bool write_output(uint64_t address, uint64_t value, size_t width);
    Mutex* lookup(uint64_t slot);
    Mutex* acquire(uint64_t slot, ps4::Error& error);
    Cond* acquire_cond(uint64_t slot, ps4::Error& error);
    Attr* find_attr(uint64_t slot, ps4::Error& error);
    MutexAttr* find_mutexattr(uint64_t slot, ps4::Error& error);
    // Creates the object behind `slot` in `table` with a new token.
    template <typename Table> ps4::Error create(Table& table, uint64_t slot);
    template <typename Table> ps4::Error remove(Table& table, uint64_t slot);
};
} // namespace maxps4
