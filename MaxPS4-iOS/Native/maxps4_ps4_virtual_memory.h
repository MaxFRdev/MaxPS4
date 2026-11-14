#pragma once
#include "maxps4_guest_elf.h"
#include "maxps4_ps4_abi.h"
#include <array>
#include <mutex>
#include <utility>
#include <vector>

namespace maxps4 {
class PS4DynamicMemory {
public:
    virtual ~PS4DynamicMemory() = default;
    virtual const uint8_t* translate(uint64_t address, size_t length, uint32_t permissions) const = 0;
    virtual size_t contiguous(uint64_t address, size_t limit, uint32_t permissions) const = 0;
};
// PS4 anonymous mappings. For a general (identity-mapped) image they are host
// pages inside the image's mmap window, at their own addresses, so guest code
// dereferences the returned pointer directly; otherwise they are logical
// buffers from `base`. x86 executable bytes remain host DATA (FEX does not
// run them yet); this is not an iOS executable-code allocator.
// Owner serializes access and resets this object whenever it restages the ELF.
class PS4VirtualMemory final : public PS4DynamicMemory {
public:
    static constexpr uint64_t page_size = 0x4000;
    // Byte-vector mappings (bounded images) and host pages of a general
    // image's mmap window: games map hundreds of MiB (heap, GPU memory).
    static constexpr size_t budget = 32 * 1024 * 1024;
    // General images may map up to host_budget, in their window or at fixed
    // addresses outside it: the host only commits the pages a game touches,
    // and iOS bounds those.
    static constexpr uint64_t host_budget = uint64_t(64) << 30;
    // The start of the PS4 application map area, where games place fixed
    // mappings (Unreal Engine's memory pool at 0x1000000000): reserved in the
    // host when the library loads, before other allocations can take it.
    // Its host span, or {0, 0} when the host did not grant it, and the
    // host's answer (0: granted; a Mach kern_return_t on iOS, errno elsewhere).
    static constexpr uint64_t app_area_base = 0x1000000000ULL, app_area_size = uint64_t(16) << 30;
    static void app_area(uint64_t& base, uint64_t& size, int& status);
    // Unreal Engine games make thousands of small mappings.
    static constexpr size_t region_limit = 65536;
    static constexpr uint64_t base = 0x1000000000ULL;
    explicit PS4VirtualMemory(const GuestELF& image) : image_(image) {}
    ps4::Error map(uint64_t hint, uint64_t length, uint32_t protection,
                   uint32_t flags, int32_t descriptor, int64_t offset, uint64_t& result);
    // Anonymous zeroed pages: `hint` is a search start, or with `fixed` the
    // exact address (whose old pages are replaced; general images only);
    // `alignment` is a power of two of at least a page (0: a page).
    ps4::Error place(uint64_t hint, uint64_t length, uint64_t alignment, uint32_t protection,
                     bool fixed, uint64_t& result);
    ps4::Error unmap(uint64_t address, uint64_t length);
    ps4::Error protect(uint64_t address, uint64_t length, uint32_t protection);
    const uint8_t* translate(uint64_t address, size_t length, uint32_t permissions) const override;
    size_t contiguous(uint64_t address, size_t limit, uint32_t permissions) const override;
    void reset();
    size_t allocated() const { return used_; }
    // Placements that did not go as asked, newest last: a fixed PS4 address
    // outside the window the host refused, placed at `result` instead (a game
    // that ignores it writes into host memory at `hint`; `status` is the
    // host's answer), or a refused request (`result` 0, `status` the error).
    struct Moved { uint64_t hint = 0, size = 0, result = 0; int status = 0; };
    static constexpr size_t moved_limit = 16;
    // Copies the last records (oldest first); returns how many moves happened.
    size_t moved(std::array<Moved, moved_limit>& out, size_t& count) const;
    // The mapping holding `address` (or, with `next`, the first at or after it):
    // its span of pages with the same permissions and their PS4 protection
    // (READ 1, WRITE 2, EXEC 4). False when there is none.
    bool query(uint64_t address, bool next, uint64_t& start, uint64_t& end, uint32_t& protection) const;
private:
    struct Region {
        uint64_t address;
        uint64_t size;
        uint8_t* data; // bytes.data(), or the host pages at `address`.
        std::vector<uint8_t> bytes; // Empty for host pages.
        // ELF-style permissions R=4/W=2/X=1; 0xff denotes an unmapped page.
        std::vector<uint8_t> pages;
        // Host pages at their own address outside the window (a fixed PS4
        // address the host granted): returned to the host when released.
        bool outside = false;
    };
    const GuestELF& image_;
    std::vector<Region> regions_;
    size_t used_ = 0;
    // Read by the session report while the game maps memory.
    mutable std::mutex moved_lock_;
    std::array<Moved, moved_limit> moved_ {};
    size_t moved_total_ = 0;
    void record_move(uint64_t hint, uint64_t size, uint64_t result, int status);
    ps4::Error place_unrecorded(uint64_t hint, uint64_t length, uint64_t alignment, uint32_t protection,
                                bool fixed, uint64_t& result);
    static ps4::Error permissions(uint32_t protection, uint8_t& result);
    static bool rounded(uint64_t length, uint64_t& result);
    Region* range(uint64_t address, uint64_t length);
    // regions_ is sorted by address: the first region ending after `address`
    // (regions_.size() if none), the one holding it (SIZE_MAX if none).
    size_t first_ending_after(uint64_t address) const;
    size_t holding(uint64_t address) const;
    void insert(Region region);
    void release(uint64_t address, uint64_t size);
    // Splits a fully mapped span into per-region pieces (false on any hole).
    bool pieces(uint64_t address, uint64_t size, std::vector<std::pair<uint64_t, uint64_t>>& out) const;
    ps4::Error unmap_one(uint64_t address, uint64_t size);
    ps4::Error protect_one(uint64_t address, uint64_t size, uint8_t permission);
    // Host pages of the current image's mmap window (general images only).
    bool hosted(const Region& region) const;
    static bool host_protect(uint64_t address, uint64_t length, uint8_t permission);
};
} // namespace maxps4
