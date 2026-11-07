#include "maxps4_ps4_virtual_memory.h"
#include <algorithm>
#include <iterator>
#include <cerrno>
#include <new>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace maxps4 {
using namespace ps4;
namespace {
// PROT_NONE host pages at exactly [address, address + size), never replacing
// a mapping: Darwin's mmap may ignore an address hint even for a free span,
// so iOS asks vm_allocate for that fixed span (it fails if anything is
// there). `status` is the host's answer (0: granted).
bool reserve_exact(uint64_t address, uint64_t size, int& status) {
#if defined(__APPLE__)
    vm_address_t at = vm_address_t(address);
    const kern_return_t kr = ::vm_allocate(::mach_task_self(), &at, vm_size_t(size), VM_FLAGS_FIXED);
    status = int(kr);
    if (kr != KERN_SUCCESS) return false;
    if (at != vm_address_t(address) || ::mprotect(reinterpret_cast<void*>(address), size_t(size), PROT_NONE) != 0) {
        ::vm_deallocate(::mach_task_self(), at, vm_size_t(size));
        status = -1;
        return false;
    }
    return true;
#else
    const auto want = reinterpret_cast<void*>(address);
    void* got = ::mmap(want, size_t(size), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (got == want) { status = 0; return true; }
    status = got == MAP_FAILED ? errno : -1;
    if (got != MAP_FAILED) ::munmap(got, size_t(size));
    return false;
#endif
}
// PROT_NONE until a fixed mapping commits pages in it; never released.
struct AppArea {
    uint64_t base = 0, size = 0;
    int status = 0;
    AppArea() {
        if (reserve_exact(PS4VirtualMemory::app_area_base, PS4VirtualMemory::app_area_size, status)) {
            base = PS4VirtualMemory::app_area_base; size = PS4VirtualMemory::app_area_size;
        }
    }
};
const AppArea app_area_reservation;
bool in_app_area(uint64_t address, uint64_t size) {
    const auto& a = app_area_reservation;
    return a.size && address >= a.base && size <= a.size && address - a.base <= a.size - size;
}
}
void PS4VirtualMemory::app_area(uint64_t& base, uint64_t& size, int& status) {
    base = app_area_reservation.base; size = app_area_reservation.size; status = app_area_reservation.status;
}
Error PS4VirtualMemory::permissions(uint32_t protection, uint8_t& out) {
    // PS4 PROT_READ=1, WRITE=2, EXEC=4, unlike ELF p_flags.
    if (protection & ~7U) return notsup;
    if ((protection & 6U) == 6U) return access; // Enforce guest W^X.
    out = uint8_t(((protection & 1U) << 2) | (protection & 2U) | ((protection & 4U) >> 2));
    return ok;
}
// regions_ is sorted by address and its regions never overlap.
size_t PS4VirtualMemory::first_ending_after(uint64_t address) const {
    auto it = std::upper_bound(regions_.begin(), regions_.end(), address,
        [](uint64_t value, const Region& region) { return value < region.address; });
    if (it != regions_.begin()) {
        const auto& previous = *std::prev(it);
        if (address - previous.address < previous.size) --it;
    }
    return size_t(it - regions_.begin());
}
size_t PS4VirtualMemory::holding(uint64_t address) const {
    const size_t i = first_ending_after(address);
    return i < regions_.size() && address >= regions_[i].address ? i : SIZE_MAX;
}
void PS4VirtualMemory::insert(Region region) {
    const auto at = std::lower_bound(regions_.begin(), regions_.end(), region.address,
        [](const Region& r, uint64_t value) { return r.address < value; });
    regions_.insert(at, std::move(region));
}
bool PS4VirtualMemory::query(uint64_t address, bool next, uint64_t& start, uint64_t& end, uint32_t& protection) const {
    // The page holding `address` or, with `next`, the first mapped one after it.
    const Region* best = nullptr;
    size_t first = 0;
    for (size_t r = first_ending_after(address); r < regions_.size() && !best; ++r) {
        const Region& region = regions_[r];
        const size_t from = address > region.address ? size_t((address - region.address) / page_size) : 0;
        if (!next && (address < region.address || regions_[r].pages[from] == 0xff)) return false;
        for (size_t i = from; i < region.pages.size(); ++i)
            if (region.pages[i] != 0xff) { best = &region; first = i; break; }
        if (!next) break;
    }
    if (!best) return false;
    const uint8_t p = best->pages[first];
    size_t last = first;
    while (last + 1 < best->pages.size() && best->pages[last + 1] == p) ++last;
    size_t begin = first;
    while (begin > 0 && best->pages[begin - 1] == p) --begin;
    start = best->address + begin * page_size;
    end = best->address + (last + 1) * page_size;
    protection = ((p & 4U) ? 1U : 0U) | (p & 2U) | ((p & 1U) ? 4U : 0U);
    return true;
}
bool PS4VirtualMemory::rounded(uint64_t length, uint64_t& out) {
    if (!length || length > UINT64_MAX - (page_size - 1)) return false;
    out = (length + page_size - 1) / page_size * page_size;
    return true;
}
bool PS4VirtualMemory::hosted(const Region& region) const {
    uint64_t window = 0, bytes = 0;
    if (region.bytes.empty() && (region.outside || in_app_area(region.address, region.size))) return true;
    return region.bytes.empty() && image_.heap(window, bytes) &&
           region.address >= window && region.size <= bytes &&
           region.address - window <= bytes - region.size;
}
bool PS4VirtualMemory::host_protect(uint64_t address, uint64_t length, uint8_t permission) {
    // Executable pages stay host data (read-only): FEX does not run them yet.
    // 0xff (unmapped) and 0 become PROT_NONE; an unmapped page is also zeroed
    // by replacing it inside the window, which the image owns.
    auto* pointer = reinterpret_cast<void*>(address);
    if (permission == 0xff)
        return ::mmap(pointer, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == pointer;
    const int protection = ((permission & 5U) ? PROT_READ : 0) |
                           ((permission & 2U) ? PROT_READ | PROT_WRITE : 0);
    return ::mprotect(pointer, length, protection) == 0;
}
Error PS4VirtualMemory::map(uint64_t hint, uint64_t length, uint32_t protection,
    uint32_t flags, int32_t descriptor, int64_t offset, uint64_t& result) {
    // Anonymous PRIVATE only, never file mappings. General images (host
    // pages) also take an address hint and MAP_FIXED (0x10); bounded images
    // take neither: never pretend to implement them.
    uint64_t window = 0, window_bytes = 0;
    const bool host = image_.heap(window, window_bytes);
    if ((hint && !host) || (flags != 0x1002U && !(host && flags == 0x1012U))) return notsup;
    if (descriptor != -1 || offset != 0) return inval;
    return place(hint, length, page_size, protection, flags == 0x1012U, result);
}
void PS4VirtualMemory::release(uint64_t address, uint64_t size) {
    // Every mapped page of [address, address + size), whatever its region;
    // whole runs of mapped pages at once.
    const uint64_t end = address + size;
    for (size_t r = first_ending_after(address); r < regions_.size() && regions_[r].address < end;) {
        const Region& region = regions_[r];
        const uint64_t from = std::max(address, region.address);
        const uint64_t to = std::min(end, region.address + region.size);
        size_t page = size_t((from - region.address) / page_size);
        const size_t stop = size_t((to - region.address + page_size - 1) / page_size);
        while (page < stop && region.pages[page] == 0xff) ++page;
        if (page == stop) { ++r; continue; }
        size_t run = page;
        while (run < stop && region.pages[run] != 0xff) ++run;
        const uint64_t at = region.address + page * page_size;
        (void)unmap(at, (run - page) * page_size); // Re-sorts: look again from here.
        r = first_ending_after(at);
    }
}
Error PS4VirtualMemory::place(uint64_t hint, uint64_t length, uint64_t alignment, uint32_t protection,
    bool fixed, uint64_t& result) {
    // Refusals are recorded too (result 0, the PS4 error): a game that runs
    // out of memory usually only says so in its own way (Mortal Kombat's
    // video decoder: VPX_CODEC_MEM_ERROR).
    const Error e = place_unrecorded(hint, length, alignment, protection, fixed, result);
    if (e) record_move(hint, length, 0, int(e));
    return e;
}
Error PS4VirtualMemory::place_unrecorded(uint64_t hint, uint64_t length, uint64_t alignment, uint32_t protection,
    bool fixed, uint64_t& result) {
    uint8_t permission;
    Error e = permissions(protection, permission);
    if (e) return e;
    uint64_t size;
    if (!rounded(length, size)) return inval;
    if (!alignment) alignment = page_size;
    if (alignment < page_size || (alignment & (alignment - 1)) || alignment > (uint64_t{1} << 32)) return inval;
    // General images: a span of the image's mmap window (host = guest). Else
    // an available logical span, also excluding ELF segments. No MAP_FIXED
    // host operation can clobber the app: the window belongs to the image.
    uint64_t window = 0, window_bytes = 0;
    const bool host = image_.heap(window, window_bytes);
    const uint64_t limit = host ? host_budget : budget;
    if (size > limit - std::min<uint64_t>(used_, limit) || regions_.size() >= region_limit) return nomem;
    // Guest and host addresses coincide: a fixed PS4 address outside the
    // window is honoured when the host grants exactly that span (Unreal
    // Engine pools at 0x1000000000), never by replacing a host mapping. Else
    // the span goes elsewhere in the window and the caller reads it back
    // (Crash Bandicoot 4's libc maps its heap this way).
    uint64_t moved_hint = 0;
    int moved_status = 0;
    if (fixed && host && (hint < window || size > window_bytes || hint - window > window_bytes - size)) {
        if (hint % page_size == 0 && hint <= UINT64_MAX - size && in_app_area(hint, size)) {
            // Inside the reserved application area: ours, so its pages are
            // committed in place (and go back to PROT_NONE when released).
            release(hint, size);
            if (!host_protect(hint, size, permission)) return nomem;
            try {
                insert({hint, size, reinterpret_cast<uint8_t*>(hint), {},
                        std::vector<uint8_t>(size_t(size / page_size), permission)});
            } catch (const std::bad_alloc&) { host_protect(hint, size, 0xff); return nomem; }
            used_ += size_t(size); result = hint;
            return ok;
        }
        if (hint % page_size == 0 && hint <= UINT64_MAX - size) {
            release(hint, size); // What we mapped there before, as MAP_FIXED replaces it.
            int& status = moved_status;
            if (reserve_exact(hint, size, status)) {
                auto* granted = reinterpret_cast<void*>(hint);
                if (!host_protect(hint, size, permission)) { ::munmap(granted, size_t(size)); return nomem; }
                try {
                    insert({hint, size, reinterpret_cast<uint8_t*>(hint), {},
                            std::vector<uint8_t>(size_t(size / page_size), permission), true});
                } catch (const std::bad_alloc&) { ::munmap(granted, size_t(size)); return nomem; }
                used_ += size_t(size); result = hint;
                return ok;
            }
        }
        if (size > window_bytes) return nomem;
        moved_hint = hint;
        fixed = false;
        hint = 0;
    }
    if (fixed) {
        // MAP_FIXED replaces whatever the span held, as on the PS4.
        if (!host || hint % page_size || hint < window || size > window_bytes || hint - window > window_bytes - size)
            return inval;
        release(hint, size);
        if (size > limit - std::min<uint64_t>(used_, limit)) return nomem;
    }
    auto align = [&](uint64_t value) {
        return value > UINT64_MAX - (alignment - 1) ? UINT64_MAX : (value + alignment - 1) & ~(alignment - 1);
    };
    auto page_end = [](uint64_t end) {
        return end > UINT64_MAX - (page_size - 1) ? UINT64_MAX : (end + page_size - 1) / page_size * page_size;
    };
    uint64_t start = host ? window : base;
    if (host && hint > window && hint < window + window_bytes) start = hint; // A search start, not a demand.
    uint64_t candidate = fixed ? hint : align(start);
    bool wrapped = start == (host ? window : base);
    for (;;) {
        if (candidate > UINT64_MAX - size) return nomem;
        if (host && candidate + size > window + window_bytes) {
            if (wrapped) return nomem;
            wrapped = true;
            candidate = align(window);
            continue;
        }
        // The first region ending after the candidate is the only one that
        // can start inside it (regions are sorted and disjoint).
        uint64_t next = candidate;
        const size_t r = first_ending_after(candidate);
        if (r < regions_.size() && regions_[r].address < candidate + size)
            next = page_end(regions_[r].address + regions_[r].size);
        if (!host)
            for (const auto& segment : image_.segments()) {
                if (segment.memory_size > UINT64_MAX - segment.address) { next = UINT64_MAX; break; }
                if (candidate < segment.address + segment.memory_size && segment.address < candidate + size)
                    next = std::max(next, page_end(segment.address + segment.memory_size));
            }
        if (next == candidate) break;
        if (fixed) return nomem; // Still in use after release: not ours to replace.
        candidate = align(next);
    }
    try {
        Region pending{candidate, size, nullptr, {}, std::vector<uint8_t>(size_t(size / page_size), permission)};
        if (host) {
            regions_.reserve(regions_.size() + 1);
            if (!host_protect(candidate, size, permission)) return nomem;
            pending.data = reinterpret_cast<uint8_t*>(candidate);
        } else {
            pending.bytes.assign(size_t(size), 0);
            pending.data = pending.bytes.data();
        }
        insert(std::move(pending));
    } catch (const std::bad_alloc&) { return nomem; }
    used_ += size_t(size); result = candidate;
    if (moved_hint) record_move(moved_hint, size, candidate, moved_status);
    return ok;
}
void PS4VirtualMemory::record_move(uint64_t hint, uint64_t size, uint64_t result, int status) {
    std::lock_guard guard(moved_lock_);
    moved_[moved_total_++ % moved_limit] = {hint, size, result, status};
}
size_t PS4VirtualMemory::moved(std::array<Moved, moved_limit>& out, size_t& count) const {
    std::lock_guard guard(moved_lock_);
    count = std::min(moved_total_, moved_limit);
    for (size_t i = 0; i < count; ++i) out[i] = moved_[(moved_total_ - count + i) % moved_limit];
    return moved_total_;
}
PS4VirtualMemory::Region* PS4VirtualMemory::range(uint64_t address, uint64_t size) {
    const size_t r = holding(address);
    if (r == SIZE_MAX) return nullptr;
    Region& region = regions_[r];
    const uint64_t relative = address - region.address;
    if (size > region.size - relative) return nullptr;
    const size_t first = size_t(relative / page_size);
    const size_t last = size_t((relative + size - 1) / page_size);
    for (size_t i = first; i <= last; ++i) if (region.pages[i] == 0xff) return nullptr;
    return &region;
}
Error PS4VirtualMemory::unmap(uint64_t address, uint64_t length) {
    uint64_t size;
    if (address % page_size || !rounded(length, size)) return inval;
    Region* region = range(address, size);
    if (!region) return nomem;
    const size_t first = size_t((address - region->address) / page_size);
    if (hosted(*region)) {
        // Host pages: the span leaves the region, which keeps what lies before
        // and after it, so the span can be mapped again (MAP_FIXED included).
        if (region->outside ? ::munmap(reinterpret_cast<void*>(address), size_t(size)) != 0 :
                              !host_protect(address, size, 0xff)) return nomem;
        const size_t index = size_t(region - regions_.data());
        Region old = std::move(*region);
        regions_.erase(regions_.begin() + long(index));
        const size_t last = first + size_t(size / page_size);
        auto keep = [&](size_t from, size_t to) {
            if (from == to) return;
            const uint64_t at = old.address + from * page_size;
            insert({at, (to - from) * page_size, reinterpret_cast<uint8_t*>(at), {},
                std::vector<uint8_t>(old.pages.begin() + long(from), old.pages.begin() + long(to)), old.outside});
        };
        keep(0, first);
        keep(last, old.pages.size());
        used_ -= size_t(size);
        return ok;
    }
    for (size_t i = first; i < first + size / page_size; ++i) {
        region->pages[i] = 0xff;
        if (!region->bytes.empty()) std::fill_n(region->bytes.begin() + long(i * page_size), page_size, uint8_t(0));
    }
    if (std::all_of(region->pages.begin(), region->pages.end(), [](uint8_t p) { return p == 0xff; })) {
        used_ -= size_t(region->size);
        const size_t index = size_t(region - regions_.data());
        regions_.erase(regions_.begin() + long(index));
    }
    return ok;
}
Error PS4VirtualMemory::protect(uint64_t address, uint64_t length, uint32_t protection) {
    uint8_t permission;
    Error e = permissions(protection, permission);
    if (e) return e;
    uint64_t size;
    if (address % page_size || !rounded(length, size)) return inval;
    Region* region = range(address, size);
    if (!region) return nomem;
    if (hosted(*region) && !host_protect(address, size, permission)) return nomem;
    const size_t first = size_t((address - region->address) / page_size);
    std::fill_n(region->pages.begin() + long(first), size_t(size / page_size), permission);
    return ok;
}
void PS4VirtualMemory::reset() {
    // Host pages go back to PROT_NONE (and zero) only while they still lie in
    // the current image's window; a cleared image already released them.
    for (const auto& region : regions_) {
        if (region.outside) ::munmap(reinterpret_cast<void*>(region.address), size_t(region.size));
        else if (hosted(region)) host_protect(region.address, region.size, 0xff);
    }
    regions_.clear(); used_ = 0;
}
size_t PS4VirtualMemory::contiguous(uint64_t address, size_t limit, uint32_t permissions) const {
    if (!limit || !permissions || (permissions & ~7U)) return 0;
    size_t r = holding(address);
    if (r == SIZE_MAX) return 0;
    // Host-backed regions that touch are one range (guest and host addresses
    // coincide): Call of Duty maps its frame buffers 64 KiB at a time.
    size_t total = 0;
    for (;;) {
        const Region& region = regions_[r];
        const uint64_t relative = address + total - region.address;
        const size_t first = size_t(relative / page_size);
        // Only the pages `limit` reaches: a region can hold a million of them.
        const uint64_t reach = (relative + std::min<uint64_t>(limit - total, region.size - relative) + page_size - 1) / page_size;
        const size_t end = size_t(std::min<uint64_t>(reach, region.pages.size()));
        size_t last = first;
        while (last < end && region.pages[last] != 0xff &&
               (region.pages[last] & permissions) == permissions) ++last;
        if (last == first) return total;
        total += std::min(limit - total, size_t(last * page_size - relative));
        if (total == limit || last < region.pages.size() || r + 1 == regions_.size()) return total;
        const Region& next = regions_[r + 1];
        if (next.address != region.address + region.size || !hosted(region) || !hosted(next)) return total;
        ++r;
    }
}
const uint8_t* PS4VirtualMemory::translate(uint64_t address, size_t size, uint32_t permissions) const {
    if (!size || contiguous(address, size, permissions) != size) return nullptr;
    const size_t r = holding(address);
    return r == SIZE_MAX ? nullptr : regions_[r].data + (address - regions_[r].address);
}
} // namespace maxps4
