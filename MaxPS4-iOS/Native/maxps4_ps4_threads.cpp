#include "maxps4_ps4_threads.h"
#include <algorithm>
#include <chrono>

namespace maxps4 {
using namespace ps4;
namespace {
// Slot values with a meaning of their own (see PS4Threads).
constexpr uint64_t static_initializer = 0, adaptive_initializer = 1, destroyed = 2;
}
bool PS4Threads::known_locked(uint64_t thread) const {
    return thread && std::find(threads_.begin(), threads_.end(), thread) != threads_.end();
}
bool PS4Threads::known(uint64_t thread) const {
    std::lock_guard<std::mutex> lock(guard_); return known_locked(thread);
}
uint64_t PS4Threads::attach() {
    std::lock_guard<std::mutex> lock(guard_);
    if (next_thread_ == UINT64_MAX) return 0;
    for (size_t i = 0; i < threads_.size(); ++i) if (!threads_[i]) {
        threads_[i] = next_thread_++;
        values_[i].fill(0); // A reused slot starts with no thread-specific data.
        return threads_[i];
    }
    return 0;
}
Error PS4Threads::detach(uint64_t thread) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!known_locked(thread) || thread == 1) return inval;
    for (const auto& mutex : mutexes_) if (mutex.owner == thread) return busy;
    for (size_t i = 0; i < threads_.size(); ++i)
        if (threads_[i] == thread) { threads_[i] = 0; values_[i].fill(0); break; }
    return ok;
}
bool PS4Threads::writable(uint64_t slot) const {
    return slot && !(slot & 7U) && memory_.permitted(slot, 8, 6) && !memory_.permitted(slot, 8, 1);
}
bool PS4Threads::read_token(uint64_t slot, uint64_t& token) const {
    uint8_t wire[8];
    if (!slot || (slot & 7U) || !memory_.read(slot, wire, 8)) return false;
    token = get_le(wire, 8);
    return true;
}
bool PS4Threads::write_token(uint64_t slot, uint64_t token) {
    uint8_t wire[8]; put_le(wire, 0, token, 8);
    return memory_.write(slot, wire, 8);
}
PS4Threads::Mutex* PS4Threads::lookup(uint64_t slot) {
    uint64_t token = 0;
    if (!read_token(slot, token)) return nullptr;
    for (auto& mutex : mutexes_) if (token && mutex.token == token) return &mutex;
    return nullptr;
}
PS4Threads::Mutex* PS4Threads::acquire(uint64_t slot, Error& error) {
    uint64_t token = 0;
    if (!slot || !memory_.permitted(slot, 8, 4) || !read_token(slot, token)) { error = fault; return nullptr; }
    if (token != static_initializer && token != adaptive_initializer) {
        Mutex* mutex = lookup(slot);
        if (!mutex) error = inval;
        return mutex;
    }
    if (!writable(slot)) { error = fault; return nullptr; }
    for (auto& mutex : mutexes_) if (!mutex.token) {
        const uint64_t fresh = next_token();
        if (!fresh) { error = again; return nullptr; }
        if (!write_token(slot, fresh)) { error = fault; return nullptr; }
        mutex = {fresh, 0, slot, token == adaptive_initializer ? adaptive : error_check, 0};
        return &mutex;
    }
    error = again;
    return nullptr;
}
uint64_t PS4Threads::next_token() {
    if (arena_end_) {
        if (arena_end_ - arena_next_ < object_size) return 0;
        const uint8_t zero[object_size] = {};
        if (!memory_.write(arena_next_, zero, object_size)) return 0;
        const uint64_t token = arena_next_;
        arena_next_ += object_size;
        return token;
    }
    return next_mutex_ == UINT64_MAX ? 0 : next_mutex_++;
}
void PS4Threads::set_object_arena(uint64_t base, uint64_t size) {
    std::lock_guard<std::mutex> lock(guard_);
    arena_next_ = base;
    arena_end_ = base + size;
}
bool PS4Threads::write_output(uint64_t address, uint64_t value, size_t width) {
    uint8_t wire[8]; put_le(wire, 0, value, width);
    return address && memory_.write(address, wire, width);
}
// Attribute objects: initializing a slot again (common for an attribute on
// the stack) replaces its object instead of failing.
PS4Threads::Attr* PS4Threads::find_attr(uint64_t slot, Error& error) {
    uint64_t token = 0;
    if (!slot || !read_token(slot, token)) { error = fault; return nullptr; }
    for (auto& attr : attrs_) if (token && attr.token == token) return &attr;
    error = inval;
    return nullptr;
}
PS4Threads::MutexAttr* PS4Threads::find_mutexattr(uint64_t slot, Error& error) {
    uint64_t token = 0;
    if (!slot || !read_token(slot, token)) { error = fault; return nullptr; }
    for (auto& attr : mutexattrs_) if (token && attr.token == token) return &attr;
    error = inval;
    return nullptr;
}
template <typename Table> Error PS4Threads::create(Table& table, uint64_t slot) {
    if (!writable(slot)) return fault;
    auto entry = std::find_if(table.begin(), table.end(), [slot](const auto& e) { return e.token && e.slot == slot; });
    if (entry == table.end())
        entry = std::find_if(table.begin(), table.end(), [](const auto& e) { return !e.token; });
    if (entry == table.end()) return nomem;
    const uint64_t fresh = next_token();
    if (!fresh) return again;
    if (!write_token(slot, fresh)) return fault;
    *entry = {};
    entry->token = fresh;
    entry->slot = slot;
    return ok;
}
template <typename Table> Error PS4Threads::remove(Table& table, uint64_t slot) {
    if (!writable(slot)) return fault;
    uint64_t token = 0;
    if (!read_token(slot, token)) return fault;
    const auto entry = std::find_if(table.begin(), table.end(),
        [token](const auto& e) { return e.token && e.token == token; });
    if (entry == table.end()) return inval;
    if (!write_token(slot, destroyed)) return fault;
    *entry = {};
    return ok;
}
Error PS4Threads::attr_init(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    return create(attrs_, slot);
}
Error PS4Threads::attr_destroy(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    return remove(attrs_, slot);
}
Error PS4Threads::attr_set_detach(uint64_t slot, uint64_t state) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    Attr* attr = find_attr(slot, error);
    if (!attr) return error;
    if (uint32_t(state) > 1) return inval;
    attr->detached = uint32_t(state) == 1;
    return ok;
}
Error PS4Threads::attr_get_detach(uint64_t slot, uint64_t output) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    Attr* attr = find_attr(slot, error);
    if (!attr) return error;
    return write_output(output, attr->detached ? 1 : 0, 4) ? ok : fault;
}
Error PS4Threads::attr_set_stack(uint64_t slot, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    Attr* attr = find_attr(slot, error);
    if (!attr) return error;
    if (bytes < minimum_stack) return inval;
    attr->stack = bytes;
    return ok;
}
Error PS4Threads::attr_get_stack(uint64_t slot, uint64_t output) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    Attr* attr = find_attr(slot, error);
    if (!attr) return error;
    return write_output(output, attr->stack, 8) ? ok : fault;
}
Error PS4Threads::attr_accept(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    return find_attr(slot, error) ? ok : error;
}
Error PS4Threads::thread_attributes(uint64_t slot, bool& detached, uint64_t& stack) {
    std::lock_guard<std::mutex> lock(guard_);
    detached = false;
    stack = default_stack;
    if (!slot) return ok;
    Error error = ok;
    Attr* attr = find_attr(slot, error);
    if (!attr) return error;
    detached = attr->detached;
    stack = attr->stack;
    return ok;
}
Error PS4Threads::mutexattr_init(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    return create(mutexattrs_, slot);
}
Error PS4Threads::mutexattr_destroy(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    return remove(mutexattrs_, slot);
}
Error PS4Threads::mutexattr_settype(uint64_t slot, uint64_t type) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    MutexAttr* attr = find_mutexattr(slot, error);
    if (!attr) return error;
    if (uint32_t(type) < error_check || uint32_t(type) > adaptive) return inval;
    attr->type = uint32_t(type);
    return ok;
}
Error PS4Threads::mutexattr_gettype(uint64_t slot, uint64_t output) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    MutexAttr* attr = find_mutexattr(slot, error);
    if (!attr) return error;
    return write_output(output, attr->type, 4) ? ok : fault;
}
Error PS4Threads::mutexattr_accept(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    Error error = ok;
    return find_mutexattr(slot, error) ? ok : error;
}
Error PS4Threads::init(uint64_t slot, uint64_t attributes, uint64_t name) {
    std::lock_guard<std::mutex> lock(guard_);
    uint32_t type = error_check;
    if (attributes) {
        Error error = ok;
        MutexAttr* attr = find_mutexattr(attributes, error);
        if (!attr) return error;
        type = attr->type;
    }
    if (!writable(slot)) return fault;
    if (name) {
        char text[1024];
        if (!memory_.read_cstring(name, text, sizeof(text))) return fault;
    }
    for (const auto& mutex : mutexes_) if (mutex.token && mutex.slot == slot) return busy;
    for (auto& mutex : mutexes_) if (!mutex.token) {
        const uint64_t fresh = next_token();
        if (!fresh) return again;
        if (!write_token(slot, fresh)) return fault;
        mutex = {fresh, 0, slot, type, 0};
        return ok;
    }
    return again;
}
Error PS4Threads::trylock(uint64_t slot, uint64_t thread) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!known_locked(thread)) return inval;
    Error error = ok;
    Mutex* mutex = acquire(slot, error);
    if (!mutex) return error;
    if (mutex->owner == thread && mutex->type == recursive) {
        if (mutex->count == UINT32_MAX) return again;
        ++mutex->count;
        return ok;
    }
    if (mutex->owner) return busy;
    mutex->owner = thread;
    mutex->count = 1;
    return ok;
}
Error PS4Threads::lock(uint64_t slot, uint64_t thread) {
    std::unique_lock<std::mutex> lock(guard_);
    if (!known_locked(thread)) return inval;
    Error error = ok;
    Mutex* mutex = acquire(slot, error);
    if (!mutex) return error;
    if (mutex->owner == thread) {
        if (mutex->type == recursive) {
            if (mutex->count == UINT32_MAX) return again;
            ++mutex->count;
            return ok;
        }
        if (mutex->type != normal) return deadlk;
        // A normal mutex locked again by its owner deadlocks, as POSIX says.
        changed_.wait(lock, [&] { return interrupted_; });
        return intr;
    }
    // A free mutex can be destroyed before this thread wakes: never take
    // another mutex's record.
    const uint64_t token = mutex->token;
    changed_.wait(lock, [&] { return interrupted_ || mutex->token != token || !mutex->owner; });
    if (interrupted_) return intr;
    if (mutex->token != token) return inval;
    mutex->owner = thread;
    mutex->count = 1;
    return ok;
}
uint64_t PS4Threads::mutex_owner(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    const Mutex* mutex = slot ? lookup(slot) : nullptr;
    return mutex ? mutex->owner : 0;
}
Error PS4Threads::unlock(uint64_t slot, uint64_t thread) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        if (!known_locked(thread)) return inval;
        if (!slot || !memory_.permitted(slot, 8, 4)) return fault;
        uint64_t token = 0;
        if (read_token(slot, token) && (token == static_initializer || token == adaptive_initializer)) return perm;
        Mutex* mutex = lookup(slot);
        if (!mutex) return inval;
        if (mutex->owner != thread) return perm;
        if (--mutex->count) return ok;
        mutex->owner = 0;
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::destroy(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!writable(slot)) return fault;
    Mutex* mutex = lookup(slot);
    if (!mutex) return inval;
    if (mutex->owner) return busy;
    if (!write_token(slot, destroyed)) return fault;
    *mutex = {};
    return ok;
}
PS4Threads::Cond* PS4Threads::acquire_cond(uint64_t slot, Error& error) {
    uint64_t token = 0;
    if (!slot || !memory_.permitted(slot, 8, 4) || !read_token(slot, token)) { error = fault; return nullptr; }
    if (token != static_initializer) {
        for (auto& cond : conds_) if (cond.token == token) return &cond;
        error = inval;
        return nullptr;
    }
    if (!writable(slot)) { error = fault; return nullptr; }
    for (auto& cond : conds_) if (!cond.token) {
        const uint64_t fresh = next_token();
        if (!fresh) { error = again; return nullptr; }
        if (!write_token(slot, fresh)) { error = fault; return nullptr; }
        cond = {fresh, slot, 0, 0, 0};
        return &cond;
    }
    error = again;
    return nullptr;
}
Error PS4Threads::cond_init(uint64_t slot, uint64_t, uint64_t name) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!writable(slot)) return fault;
    if (name) {
        char text[1024];
        if (!memory_.read_cstring(name, text, sizeof(text))) return fault;
    }
    for (const auto& cond : conds_) if (cond.token && cond.slot == slot) return busy;
    for (auto& cond : conds_) if (!cond.token) {
        const uint64_t fresh = next_token();
        if (!fresh) return again;
        if (!write_token(slot, fresh)) return fault;
        cond = {fresh, slot, 0, 0, 0};
        return ok;
    }
    return again;
}
Error PS4Threads::cond_destroy(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!writable(slot)) return fault;
    uint64_t token = 0;
    if (!read_token(slot, token)) return fault;
    if (token == static_initializer) return ok; // Never used: nothing to free.
    const auto cond = std::find_if(conds_.begin(), conds_.end(),
        [token](const Cond& c) { return c.token && c.token == token; });
    if (cond == conds_.end()) return inval;
    if (cond->waiters) return busy;
    if (!write_token(slot, destroyed)) return fault;
    *cond = {};
    return ok;
}
Error PS4Threads::cond_wait(uint64_t cond_slot, uint64_t mutex_slot, uint64_t thread, bool timed, uint64_t timeout_us) {
    std::unique_lock<std::mutex> lock(guard_);
    if (!known_locked(thread)) return inval;
    Error error = ok;
    Cond* cond = acquire_cond(cond_slot, error);
    if (!cond) return error;
    if (!mutex_slot || !memory_.permitted(mutex_slot, 8, 4)) return fault;
    Mutex* mutex = lookup(mutex_slot);
    if (!mutex) return inval;
    if (mutex->owner != thread) return perm;
    // A waiter keeps the condition alive (destroy is EBUSY). The mutex is free
    // meanwhile: if it is destroyed, the wait ends with EINVAL.
    const uint64_t generation = cond->generation, mutex_token = mutex->token;
    const uint32_t count = mutex->count;
    ++cond->waiters;
    mutex->owner = 0;
    mutex->count = 0;
    changed_.notify_all();
    auto signalled = [&] { return cond->generation != generation && cond->wakeups; };
    auto woken = [&] { return interrupted_ || signalled(); };
    if (timed) {
        // Saturate: an enormous timeout is an untimed wait.
        const auto limit = std::chrono::microseconds(std::min<uint64_t>(timeout_us, uint64_t(1) << 50));
        changed_.wait_for(lock, limit, woken);
    } else {
        changed_.wait(lock, woken);
    }
    const bool consumed = !interrupted_ && signalled();
    --cond->waiters;
    if (consumed) --cond->wakeups;
    cond->wakeups = std::min(cond->wakeups, cond->waiters);
    changed_.wait(lock, [&] { return interrupted_ || mutex->token != mutex_token || !mutex->owner; });
    if (interrupted_) return intr;
    if (mutex->token != mutex_token) return inval;
    mutex->owner = thread;
    mutex->count = count;
    return consumed ? ok : timedout;
}
Error PS4Threads::cond_signal(uint64_t slot, bool all) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        Error error = ok;
        Cond* cond = acquire_cond(slot, error);
        if (!cond) return error;
        if (cond->wakeups >= cond->waiters) return ok; // Nobody left to wake.
        cond->wakeups = all ? cond->waiters : cond->wakeups + 1;
        ++cond->generation;
    }
    changed_.notify_all();
    return ok;
}
PS4Threads::RwLock* PS4Threads::acquire_rwlock(uint64_t slot, Error& error) {
    uint64_t token = 0;
    if (!slot || !memory_.permitted(slot, 8, 4) || !read_token(slot, token)) { error = fault; return nullptr; }
    if (token != static_initializer) {
        for (auto& lock : rwlocks_) if (lock.token == token) return &lock;
        error = inval;
        return nullptr;
    }
    if (!writable(slot)) { error = fault; return nullptr; }
    for (auto& lock : rwlocks_) if (!lock.token) {
        const uint64_t fresh = next_token();
        if (!fresh) { error = again; return nullptr; }
        if (!write_token(slot, fresh)) { error = fault; return nullptr; }
        lock = {fresh, slot, 0, 0};
        return &lock;
    }
    error = again;
    return nullptr;
}
Error PS4Threads::rwlock_init(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!writable(slot)) return fault;
    for (const auto& existing : rwlocks_) if (existing.token && existing.slot == slot) return busy;
    if (!write_token(slot, static_initializer)) return fault;
    Error error = ok;
    return acquire_rwlock(slot, error) ? ok : error;
}
Error PS4Threads::rwlock_destroy(uint64_t slot) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!writable(slot)) return fault;
    uint64_t token = 0;
    if (!read_token(slot, token)) return fault;
    if (token == static_initializer) return ok;
    const auto found = std::find_if(rwlocks_.begin(), rwlocks_.end(),
        [token](const RwLock& l) { return l.token && l.token == token; });
    if (found == rwlocks_.end()) return inval;
    if (found->writer || found->readers) return busy;
    if (!write_token(slot, destroyed)) return fault;
    *found = {};
    return ok;
}
Error PS4Threads::rwlock_lock(uint64_t slot, uint64_t thread, bool write, bool wait) {
    std::unique_lock<std::mutex> lock(guard_);
    if (!known_locked(thread)) return inval;
    Error error = ok;
    RwLock* rw = acquire_rwlock(slot, error);
    if (!rw) return error;
    if (rw->writer == thread) return deadlk;
    const uint64_t token = rw->token;
    auto free = [&] { return !rw->writer && (!write || !rw->readers); };
    if (!free()) {
        if (!wait) return busy;
        changed_.wait(lock, [&] { return interrupted_ || rw->token != token || free(); });
        if (interrupted_) return intr;
        if (rw->token != token) return inval;
    }
    if (write) rw->writer = thread;
    else ++rw->readers;
    return ok;
}
Error PS4Threads::rwlock_unlock(uint64_t slot, uint64_t thread) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        if (!known_locked(thread)) return inval;
        uint64_t token = 0;
        if (!slot || !read_token(slot, token)) return fault;
        const auto found = std::find_if(rwlocks_.begin(), rwlocks_.end(),
            [token](const RwLock& l) { return l.token && l.token == token; });
        if (found == rwlocks_.end()) return inval;
        if (found->writer == thread) found->writer = 0;
        else if (found->readers) --found->readers; // Readers are not tracked per thread.
        else return perm;
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::key_create(uint64_t output, uint64_t destructor) {
    std::lock_guard<std::mutex> lock(guard_);
    for (size_t key = 0; key < keys_.size(); ++key) if (!keys_[key].used) {
        uint8_t wire[4]; put_le(wire, 0, key, 4);
        if (!output || !memory_.write(output, wire, 4)) return fault;
        keys_[key] = {true, destructor};
        for (auto& values : values_) values[key] = 0;
        return ok;
    }
    return again;
}
Error PS4Threads::key_delete(uint64_t key) {
    std::lock_guard<std::mutex> lock(guard_);
    if (uint32_t(key) >= keys_.size() || !keys_[uint32_t(key)].used) return inval;
    keys_[uint32_t(key)] = {};
    return ok;
}
uint64_t PS4Threads::get_specific(uint64_t key, uint64_t thread) const {
    std::lock_guard<std::mutex> lock(guard_);
    for (size_t i = 0; i < threads_.size(); ++i)
        if (threads_[i] == thread && uint32_t(key) < key_limit && keys_[uint32_t(key)].used)
            return values_[i][uint32_t(key)];
    return 0;
}
Error PS4Threads::set_specific(uint64_t key, uint64_t value, uint64_t thread) {
    std::lock_guard<std::mutex> lock(guard_);
    if (uint32_t(key) >= key_limit || !keys_[uint32_t(key)].used) return inval;
    for (size_t i = 0; i < threads_.size(); ++i)
        if (threads_[i] == thread) { values_[i][uint32_t(key)] = value; return ok; }
    return inval;
}
size_t PS4Threads::index(uint64_t thread) const {
    std::lock_guard<std::mutex> lock(guard_);
    for (size_t i = 0; i < threads_.size(); ++i) if (thread && threads_[i] == thread) return i;
    return thread_limit;
}
Error PS4Threads::sleep(uint64_t microseconds) {
    std::unique_lock<std::mutex> lock(guard_);
    const auto limit = std::chrono::microseconds(std::min<uint64_t>(microseconds, uint64_t(1) << 50));
    return changed_.wait_for(lock, limit, [&] { return interrupted_; }) ? intr : ok;
}
PS4Threads::Equeue* PS4Threads::find_equeue(uint64_t handle) {
    for (auto& queue : equeues_) if (handle && queue.token == handle) return &queue;
    return nullptr;
}
Error PS4Threads::equeue_create(uint64_t output, uint64_t name) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!output || !name) return inval;
    if (!memory_.permitted(output, 8, 2) || memory_.permitted(output, 8, 1)) return fault;
    // Copy/validate the name without ever treating a guest address as a host pointer.
    for (size_t i = 0; ; ++i) {
        uint8_t byte = 0;
        if (name > UINT64_MAX - i || !memory_.read(name + i, &byte, 1)) return fault;
        if (!byte) break;
        if (i == 32) return nametoolong;
    }
    if (next_mutex_ == UINT64_MAX) return again;
    for (auto& queue : equeues_) if (!queue.token) {
        if (!write_output(output, next_mutex_, 8)) return fault;
        queue = {}; queue.token = next_mutex_++;
        return ok;
    }
    return nomem;
}
Error PS4Threads::equeue_delete(uint64_t handle) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto* queue = find_equeue(handle);
        if (!queue) return badf;
        *queue = {};
    }
    changed_.notify_all(); // An outstanding wait must not survive deletion/reuse.
    return ok;
}
Error PS4Threads::user_event_add(uint64_t handle, int32_t id, bool edge) {
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    const uint64_t ident = uint64_t(int64_t(id));
    Event* entry = nullptr;
    for (auto& event : queue->events) if (event.used && event.filter == -11 && event.id == ident) { entry = &event; break; }
    if (!entry) for (auto& event : queue->events) if (!event.used) { entry = &event; break; }
    if (!entry) return nomem;
    // Re-registration updates the mode/data without losing a pending trigger.
    const bool pending = entry->used && entry->triggered;
    *entry = {true, edge, pending, ident, 0};
    return ok;
}
Error PS4Threads::user_event_delete(uint64_t handle, int32_t id) {
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    for (auto& event : queue->events)
        if (event.used && event.filter == -11 && event.id == uint64_t(int64_t(id))) { event = {}; return ok; }
    return noent;
}
Error PS4Threads::user_event_trigger(uint64_t handle, int32_t id, uint64_t data) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto* queue = find_equeue(handle);
        if (!queue) return badf;
        auto event = std::find_if(queue->events.begin(), queue->events.end(),
            [id](const Event& e) { return e.used && e.filter == -11 && e.id == uint64_t(int64_t(id)); });
        if (event == queue->events.end()) return noent;
        event->user_data = data; event->triggered = true;
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::graphics_event_add(uint64_t handle, uint64_t id, uint64_t user_data) {
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    if (id > 6 && id != 0x40) return inval;
    if (next_mutex_ == UINT64_MAX) return again;
    Event* entry = nullptr;
    for (auto& event : queue->events)
        if (event.used && event.filter == -14 && event.id == id) { entry = &event; break; }
    if (!entry) for (auto& event : queue->events) if (!event.used) { entry = &event; break; }
    if (!entry) return nomem;
    // A fresh registration cancels the old subscription and any outstanding
    // completion tickets. New work must capture the new ticket explicitly.
    *entry = {true, true, false, id, user_data, 0, next_mutex_++, -14};
    return ok;
}
Error PS4Threads::graphics_event_delete(uint64_t handle, uint64_t id) {
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    if (id > 6 && id != 0x40) return inval;
    for (auto& event : queue->events)
        if (event.used && event.filter == -14 && event.id == id) { event = {}; return ok; }
    return ok; // sceGnmDeleteEqEvent is idempotent for a valid queue/event ID.
}
Error PS4Threads::graphics_event_ticket(uint64_t handle, uint64_t id, uint64_t& ticket) {
    ticket = 0;
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    if (id > 6 && id != 0x40) return inval;
    for (const auto& event : queue->events)
        if (event.used && event.filter == -14 && event.id == id) { ticket = event.ticket; return ok; }
    return noent;
}
Error PS4Threads::graphics_event_complete(uint64_t ticket) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        if (!ticket) return inval;
        Event* found = nullptr;
        for (auto& queue : equeues_) if (queue.token)
            for (auto& event : queue.events)
                if (event.used && event.filter == -14 && event.ticket == ticket) { found = &event; break; }
        if (!found) return noent;
        found->data = found->id; found->triggered = true;
    }
    changed_.notify_all();
    return ok;
}
void PS4Threads::graphics_interrupt() {
    {
        std::lock_guard<std::mutex> lock(guard_);
        for (auto& queue : equeues_) if (queue.token)
            for (auto& event : queue.events)
                if (event.used && event.filter == -14 && event.id == 0x40) { event.data = event.id; event.triggered = true; }
    }
    changed_.notify_all();
}
Error PS4Threads::video_event_add(uint64_t handle, uint64_t source, uint64_t id, uint64_t user_data) {
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    if (!source || (id != 6 && id != 7)) return inval;
    Event* entry = nullptr;
    for (auto& event : queue->events)
        if (event.used && event.filter == -13 && event.id == id) { entry = &event; break; }
    if (!entry) for (auto& event : queue->events) if (!event.used) { entry = &event; break; }
    if (!entry) return nomem;
    *entry = {true, true, false, id, user_data, 0, 0, -13, source};
    return ok;
}
Error PS4Threads::video_event_delete(uint64_t handle, uint64_t source, uint64_t id) {
    std::lock_guard<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    for (auto& event : queue->events)
        if (event.used && event.filter == -13 && event.id == id && event.source == source) event = {};
    return ok;
}
void PS4Threads::video_event_remove_source(uint64_t source) {
    std::lock_guard<std::mutex> lock(guard_);
    for (auto& queue : equeues_) for (auto& event : queue.events)
        if (event.used && event.filter == -13 && event.source == source) event = {};
}
void PS4Threads::video_event_notify(uint64_t source, uint64_t id, uint64_t argument, uint64_t timestamp) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        for (auto& queue : equeues_) if (queue.token) for (auto& event : queue.events)
            if (event.used && event.filter == -13 && event.source == source && event.id == id) {
                const uint64_t count = event.triggered ? std::min<uint64_t>(15, ((event.data >> 12) & 15) + 1) : 1;
                event.data = (timestamp & 0xfff) | (count << 12) | ((argument & 0x0000ffffffffffffULL) << 16);
                event.triggered = true;
            }
    }
    changed_.notify_all();
}
Error PS4Threads::equeue_wait(uint64_t handle, uint64_t events, int32_t count,
                            uint64_t output, uint64_t timeout) {
    std::unique_lock<std::mutex> lock(guard_);
    auto* queue = find_equeue(handle);
    if (!queue) return badf;
    if (!output || !memory_.permitted(output, 4, 2) || memory_.permitted(output, 4, 1)) return fault;
    if (count < 1 || size_t(count) > event_limit) {
        return write_output(output, 0, 4) ? inval : fault;
    }
    const size_t capacity = size_t(count) * event_size;
    if (!events || !memory_.permitted(events, capacity, 2) || memory_.permitted(events, capacity, 1)) return fault;
    // out_count must not corrupt a returned event, even with aliased guest inputs.
    if (events <= output ? output - events < capacity : events - output < 4) return inval;
    uint8_t wire_timeout[4]{};
    if (timeout && !memory_.read(timeout, wire_timeout, 4)) return fault;
    const uint64_t micros = get_le(wire_timeout, 4);
    auto ready = [&] {
        return interrupted_ || queue->token != handle ||
            std::any_of(queue->events.begin(), queue->events.end(),
                [](const Event& e) { return e.used && e.triggered; });
    };
    if (!ready()) {
        if (!timeout) changed_.wait(lock, ready);
        else changed_.wait_for(lock, std::chrono::microseconds(micros), ready);
    }
    if (!write_output(output, 0, 4)) return fault;
    if (interrupted_) return intr;
    if (queue->token != handle) return badf;
    // Validate again after the wait: another guest may have unmapped outputs.
    if (!memory_.permitted(events, capacity, 2) || memory_.permitted(events, capacity, 1)) return fault;
    std::array<uint8_t, event_limit * event_size> wire{};
    size_t returned = 0;
    for (const auto& event : queue->events) if (event.used && event.triggered && returned < size_t(count)) {
        const size_t offset = returned++ * event_size;
        put_le(wire.data(), offset, event.id, 8);
        put_le(wire.data(), offset + 8, uint16_t(event.filter), 2);
        put_le(wire.data(), offset + 10, event.edge ? 0x20 : 0, 2); // EV_CLEAR.
        put_le(wire.data(), offset + 16, event.data, 8);
        put_le(wire.data(), offset + 24, event.user_data, 8); // Opaque udata, never dereferenced.
    }
    if (!returned) return timedout;
    if (!memory_.write(events, wire.data(), returned * event_size) || !write_output(output, returned, 4)) return fault;
    size_t consumed = 0;
    for (auto& event : queue->events) if (event.used && event.triggered && consumed < returned) {
        ++consumed;
        if (event.edge) event.triggered = false;
    }
    return ok;
}
void PS4Threads::interrupt() {
    { std::lock_guard<std::mutex> lock(guard_); interrupted_ = true; }
    changed_.notify_all();
}
void PS4Threads::resume() {
    std::lock_guard<std::mutex> lock(guard_);
    interrupted_ = false;
}
void PS4Threads::reset() {
    {
        std::lock_guard<std::mutex> lock(guard_);
        threads_.fill(0); threads_[0] = 1;
        // Keep counters monotonic: stale handles never reactivate.
        mutexes_.fill({}); conds_.fill({}); attrs_.fill({}); mutexattrs_.fill({});
        rwlocks_.fill({}); keys_.fill({}); equeues_.fill({}); semas_.fill({}); flags_.fill({});
        for (auto& values : values_) values.fill(0);
        interrupted_ = false;
        arena_next_ = arena_end_ = 0; // Its pages went with the dynamic memory.
    }
    changed_.notify_all();
}

Error PS4Threads::check_name(uint64_t name) const {
    for (size_t i = 0; name; ++i) {
        uint8_t byte = 0;
        if (name > UINT64_MAX - i || !memory_.read(name + i, &byte, 1)) return fault;
        if (!byte) break;
        if (i == 32) return nametoolong;
    }
    return ok;
}
template <typename Ready>
bool PS4Threads::wait_timeout(std::unique_lock<std::mutex>& lock, uint64_t timeout, uint64_t micros, Ready ready) {
    if (!timeout) { changed_.wait(lock, ready); return false; }
    const auto start = std::chrono::steady_clock::now();
    const bool done = changed_.wait_for(lock, std::chrono::microseconds(micros), ready);
    const auto spent = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    write_output(timeout, done && spent < micros ? micros - spent : 0, 4);
    return !done;
}
Error PS4Threads::sema_create(uint64_t output, uint64_t name, int32_t initial, int32_t max) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!output || !memory_.permitted(output, 8, 2) || memory_.permitted(output, 8, 1)) return fault;
    if (initial < 0 || max <= 0 || initial > max) return inval;
    if (const Error named = check_name(name)) return named;
    if (next_mutex_ == UINT64_MAX) return again;
    for (auto& sema : semas_) if (!sema.token) {
        if (!write_output(output, next_mutex_, 8)) return fault;
        sema = {}; sema.token = next_mutex_++; sema.count = initial; sema.max = max;
        return ok;
    }
    return again;
}
Error PS4Threads::sema_delete(uint64_t handle) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto found = std::find_if(semas_.begin(), semas_.end(), [&](const Sema& s) { return s.token && s.token == handle; });
        if (found == semas_.end()) return srch;
        *found = {};
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::sema_wait(uint64_t handle, int32_t count, uint64_t timeout, bool poll, const uint64_t* left) {
    std::unique_lock<std::mutex> lock(guard_);
    auto found = std::find_if(semas_.begin(), semas_.end(), [&](const Sema& s) { return s.token && s.token == handle; });
    if (found == semas_.end()) return srch;
    Sema& sema = *found;
    if (count <= 0 || count > sema.max) return inval;
    if (sema.count >= count) { sema.count -= count; return ok; }
    if (poll) return busy;
    uint8_t wire[4]{};
    if (timeout && !memory_.read(timeout, wire, 4)) return fault;
    const uint64_t generation = sema.generation;
    ++sema.waiters;
    auto ready = [&] { return interrupted_ || sema.token != handle || sema.generation != generation || sema.count >= count; };
    const bool timed_out = left ? !changed_.wait_for(lock, std::chrono::microseconds(*left), ready)
                                : wait_timeout(lock, timeout, get_le(wire, 4), ready);
    if (sema.token == handle) --sema.waiters;
    if (interrupted_) return intr;
    if (sema.token != handle || sema.generation != generation) return canceled;
    if (sema.count < count) return timed_out ? timedout : intr;
    sema.count -= count;
    return ok;
}
Error PS4Threads::sema_signal(uint64_t handle, int32_t count) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto found = std::find_if(semas_.begin(), semas_.end(), [&](const Sema& s) { return s.token && s.token == handle; });
        if (found == semas_.end()) return srch;
        if (count <= 0 || count > found->max - found->count) return inval;
        found->count += count;
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::sema_cancel(uint64_t handle, int32_t count, uint64_t waiters) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto found = std::find_if(semas_.begin(), semas_.end(), [&](const Sema& s) { return s.token && s.token == handle; });
        if (found == semas_.end()) return srch;
        if (count > found->max) return inval;
        if (waiters && !write_output(waiters, found->waiters, 4)) return fault;
        ++found->generation;
        if (count >= 0) found->count = count;
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::flag_create(uint64_t output, uint64_t name, uint32_t attributes, uint64_t pattern) {
    std::lock_guard<std::mutex> lock(guard_);
    if (!output || !memory_.permitted(output, 8, 2) || memory_.permitted(output, 8, 1)) return fault;
    if (attributes & ~0x33U) return inval;
    if (const Error named = check_name(name)) return named;
    if (next_mutex_ == UINT64_MAX) return again;
    for (auto& flag : flags_) if (!flag.token) {
        if (!write_output(output, next_mutex_, 8)) return fault;
        flag = {}; flag.token = next_mutex_++; flag.pattern = pattern; flag.attributes = attributes;
        return ok;
    }
    return again;
}
Error PS4Threads::flag_delete(uint64_t handle) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto found = std::find_if(flags_.begin(), flags_.end(), [&](const Flag& f) { return f.token && f.token == handle; });
        if (found == flags_.end()) return srch;
        *found = {};
    }
    changed_.notify_all();
    return ok;
}
Error PS4Threads::flag_set(uint64_t handle, uint64_t bits, bool clear) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto found = std::find_if(flags_.begin(), flags_.end(), [&](const Flag& f) { return f.token && f.token == handle; });
        if (found == flags_.end()) return srch;
        if (clear) found->pattern &= bits; else found->pattern |= bits;
    }
    if (!clear) changed_.notify_all();
    return ok;
}
Error PS4Threads::flag_wait(uint64_t handle, uint64_t bits, uint32_t mode, uint64_t result, uint64_t timeout, bool poll) {
    constexpr uint32_t all = 1, any = 2, clear_all = 0x10, clear_bits = 0x20, single = 0x10;
    std::unique_lock<std::mutex> lock(guard_);
    auto found = std::find_if(flags_.begin(), flags_.end(), [&](const Flag& f) { return f.token && f.token == handle; });
    if (found == flags_.end()) return srch;
    Flag& flag = *found;
    const uint32_t kind = mode & 3, clearing = mode & 0x30;
    if (!bits || (kind != all && kind != any) || clearing == 0x30 || (mode & ~0x33U)) return inval;
    auto met = [&] { return kind == all ? (flag.pattern & bits) == bits : (flag.pattern & bits) != 0; };
    auto take = [&] {
        if (result) write_output(result, flag.pattern, 8);
        if (clearing == clear_all) flag.pattern = 0;
        else if (clearing == clear_bits) flag.pattern &= ~bits;
        return ok;
    };
    if (met()) return take();
    if (poll) { if (result) write_output(result, flag.pattern, 8); return busy; }
    if ((flag.attributes & single) && flag.waiters) return perm;
    uint8_t wire[4]{};
    if (timeout && !memory_.read(timeout, wire, 4)) return fault;
    const uint64_t generation = flag.generation;
    ++flag.waiters;
    auto ready = [&] { return interrupted_ || flag.token != handle || flag.generation != generation || met(); };
    const bool timed_out = wait_timeout(lock, timeout, get_le(wire, 4), ready);
    if (flag.token == handle) --flag.waiters;
    if (interrupted_) return intr;
    if (flag.token != handle || flag.generation != generation) {
        if (result && flag.token == handle) write_output(result, flag.pattern, 8);
        return canceled;
    }
    if (!met()) { if (result) write_output(result, flag.pattern, 8); return timed_out ? timedout : intr; }
    return take();
}
Error PS4Threads::flag_cancel(uint64_t handle, uint64_t pattern, uint64_t waiters) {
    {
        std::lock_guard<std::mutex> lock(guard_);
        auto found = std::find_if(flags_.begin(), flags_.end(), [&](const Flag& f) { return f.token && f.token == handle; });
        if (found == flags_.end()) return srch;
        if (waiters && !write_output(waiters, found->waiters, 4)) return fault;
        ++found->generation; found->pattern = pattern;
    }
    changed_.notify_all();
    return ok;
}
} // namespace maxps4
