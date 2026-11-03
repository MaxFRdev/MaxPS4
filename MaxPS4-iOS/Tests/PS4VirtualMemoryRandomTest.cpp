// Random operations on PS4VirtualMemory (bounded image: byte-vector regions),
// checked page by page against a simple model after every step.
#include "maxps4_ps4_virtual_memory.h"
#include "GuestELFFixture.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
using namespace maxps4;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d step %zu: %s\n", __LINE__, step, #c); return 1; } } while (0)
int main() {
    GuestELF image;
    auto elf = guest_test::elf(40, 2, true);
    guest_test::put(elf, 8192, 160, 8);
    if (image.load(elf.data(), elf.size(), 6)) { std::puts("load failed"); return 1; }
    PS4VirtualMemory vm(image);
    constexpr uint64_t page = PS4VirtualMemory::page_size;
    std::map<uint64_t, uint8_t> model; // page -> PS4 protection (1 read, 2 write)
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    std::mt19937_64 random(42);
    size_t step = 0;
    auto expect = [&](uint64_t address) -> int { auto it = model.find(address); return it == model.end() ? -1 : it->second; };
    for (step = 0; step < 20000; ++step) {
        const unsigned op = unsigned(random() % 10);
        if (op < 4 || spans.empty()) {
            const uint64_t pages = 1 + random() % 4;
            const uint32_t protection = (random() % 2) ? 3 : 1;
            uint64_t at = 0;
            if (vm.place(0, pages * page, 0, protection, false, at) != ps4::ok) {
                CHECK(vm.allocated() + pages * page > PS4VirtualMemory::budget);
                continue;
            }
            CHECK(at % page == 0);
            for (uint64_t i = 0; i < pages; ++i) { CHECK(!model.count(at + i * page)); model[at + i * page] = uint8_t(protection == 3 ? 3 : 1); }
            spans.push_back({at, pages});
        } else if (op < 7) {
            auto& s = spans[random() % spans.size()];
            const uint64_t first = random() % s.second, count = 1 + random() % (s.second - first);
            const uint64_t at = s.first + first * page;
            bool mapped = true;
            for (uint64_t i = 0; i < count; ++i) mapped = mapped && model.count(at + i * page);
            const auto e = vm.unmap(at, count * page);
            // A span crossing two regions is refused even when all its pages are mapped.
            if (e == ps4::ok) { CHECK(mapped); for (uint64_t i = 0; i < count; ++i) model.erase(at + i * page); }
            else CHECK(e == ps4::nomem);
        } else {
            auto& s = spans[random() % spans.size()];
            const uint64_t first = random() % s.second, count = 1 + random() % (s.second - first);
            const uint64_t at = s.first + first * page;
            const uint32_t protection = (random() % 2) ? 3 : 1;
            bool mapped = true;
            for (uint64_t i = 0; i < count; ++i) mapped = mapped && model.count(at + i * page);
            const auto e = vm.protect(at, count * page, protection);
            // A span crossing two regions is refused even when all its pages are mapped.
            if (e == ps4::ok) { CHECK(mapped); for (uint64_t i = 0; i < count; ++i) model[at + i * page] = uint8_t(protection); }
            else CHECK(e == ps4::nomem);
        }
        // Check a few random known pages and the neighbourhood of a span.
        for (int k = 0; k < 4 && !spans.empty(); ++k) {
            const auto& s = spans[random() % spans.size()];
            for (uint64_t i = 0; i < s.second; ++i) {
                const uint64_t at = s.first + i * page;
                const int want = expect(at);
                CHECK((vm.translate(at, 1, 4) != nullptr) == (want != -1));
                CHECK((vm.translate(at, 1, 2) != nullptr) == (want == 3));
                uint64_t start = 0, end = 0; uint32_t protection = 0;
                const bool found = vm.query(at, false, start, end, protection);
                CHECK(found == (want != -1));
                if (found) CHECK(start <= at && at < end && protection == uint32_t(want == 3 ? 3 : 1));
                if (vm.query(at, true, start, end, protection)) {
                    auto next = model.lower_bound(at);
                    CHECK(next != model.end() && start <= next->first && next->first < end);
                } else CHECK(model.lower_bound(at) == model.end());
            }
        }
    }
    std::printf("PASS %zu random steps, %zu pages mapped, %zu bytes allocated\n", step, model.size(), vm.allocated());
    return 0;
}
