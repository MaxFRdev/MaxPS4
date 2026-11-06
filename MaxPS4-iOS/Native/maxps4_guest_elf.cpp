#include "OrbisFixedLayout.hpp"
#include "maxps4_guest_elf.h"
#include "maxps4_guest_range.h"
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_libkernel.h"
#include "PS4ExecutableImage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <functional>
#include <set>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace maxps4 {
namespace {
uint64_t number(const uint8_t* p, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
constexpr uint64_t limit = 32 * 1024 * 1024;
}
static int validateGuestProgram(const uint8_t* bytes, size_t length, uint32_t budget,
    uint64_t guestAddress, const std::vector<GuestELF::Segment>* regions) {
    if (!bytes || !length || length > 65536 || !budget || budget > 1000000) return -EINVAL;
    bool initialized = false;
    uint32_t instructions = 0;
    for (size_t offset = 0; offset < length;) {
        if (++instructions > budget) return -E2BIG;
        const uint8_t op = bytes[offset++];
        if (op == 0xB8 || op == 0x05) {
            if (length - offset < 4 || (op == 0x05 && !initialized)) return -ENOTSUP;
            initialized = true;
            offset += 4;
        } else if (op == 0xC7) {
            // Exact x86-64 MOV dword ptr [RIP+disp32],imm32 (C7 /0, ModRM 05).
            // Admit only a complete 4-byte store to a declared RW, non-X segment.
            // Address proof happens before mapping/execution, never by host dereference.
            if (!regions || length - offset < 9 || bytes[offset] != 0x05 ||
                guestAddress > UINT64_MAX - (offset + 9)) return -ENOTSUP;
            const uint64_t raw = number(bytes + offset + 1, 4);
            const int64_t displacement = raw >= 0x80000000ULL ?
                static_cast<int64_t>(raw) - 0x100000000LL : static_cast<int64_t>(raw);
            const uint64_t next = guestAddress + offset + 9;
            uint64_t target = 0;
            if (displacement < 0) {
                const uint64_t distance = static_cast<uint64_t>(-displacement);
                if (next < distance) return -ENOTSUP;
                target = next - distance;
            } else {
                if (next > UINT64_MAX - static_cast<uint64_t>(displacement)) return -ENOTSUP;
                target = next + static_cast<uint64_t>(displacement);
            }
            bool writable = false;
            for (const auto& region : *regions) {
                if ((region.permissions & 7U) != 6U) continue;
                writable |= guest_range_permitted(target, 4, 2, region.address,
                                                  region.memory_size, region.permissions);
            }
            if (!writable) return -ENOTSUP;
            offset += 9;
        } else if (op == 0x90) {
            // NOP has no architectural side effect.
        } else if (op == 0xF4) {
            return initialized && offset == length ? 0 : -ENOTSUP;
        } else return -ENOTSUP;
    }
    return -ENOTSUP;
}
int validate_guest_program(const uint8_t* bytes, size_t length, uint32_t budget) {
    // Without an ELF address-space proof, every memory operand remains rejected.
    return validateGuestProgram(bytes, length, budget, 0, nullptr);
}
GuestELF::~GuestELF() { clear(); }
void GuestELF::clear() {
    if (arena) ::munmap(arena, arena_size);
    arena = nullptr;
    arena_size = 0;
    guest_base = guest_entry = thread_pointer = start_address = 0;
    identity = false;
    stack_address = stack_bytes = heap_address = heap_bytes = entry_params_address = heap_window = 0;
    threads_address = thread_slot_bytes = thread_return_address = 0;
    program_named = true;
    tls_image.clear();
    thunks.clear();
    hle_data.clear();
    process_param_address = 0;
    records.clear();
    tls_blocks.clear();
    blockers = 0;
    regions.clear();
}
const uint8_t* GuestELF::translate(uint64_t address, size_t length, uint32_t permissions) const {
    if (!arena) return nullptr;
    for (const auto& segment : regions) {
        if (guest_range_permitted(address, length, permissions, segment.address,
                                  segment.memory_size, segment.permissions))
            return static_cast<const uint8_t*>(arena) + (address - guest_base);
    }
    return nullptr;
}
int GuestELF::load(const uint8_t* file, size_t length, uint32_t budget) {
    clear();
    if (!file || length < 64 || length > limit || !budget || budget > 1000000) return -EINVAL;
    if (std::memcmp(file, "\177ELF\2\1\1", 7) || number(file + 16, 2) != 2 ||
        number(file + 18, 2) != 62 || number(file + 20, 4) != 1 ||
        number(file + 52, 2) != 64 || number(file + 54, 2) != 56 ||
        number(file + 40, 8) || number(file + 60, 2)) return -ENOEXEC;
    const uint64_t table = number(file + 32, 8), count = number(file + 56, 2);
    if (table < 64 || table > length || !count || count > 64 || count > (length - table) / 56) return -ENOEXEC;
    const long host_page = ::sysconf(_SC_PAGESIZE);
    if (host_page <= 0) return -EIO;
    const uint64_t page = static_cast<uint64_t>(host_page);
    const uint64_t entry = number(file + 24, 8);
    uint64_t begin = UINT64_MAX, end = 0;
    size_t code_offset = 0, code_size = 0;
    std::vector<Segment> parsed;
    try {
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* ph = file + table + i * 56;
            // Reject dynamic linking, interpreters, TLS and all unhandled metadata.
            if (number(ph, 4) != 1) return -ENOEXEC;
            Segment s {number(ph + 16, 8), number(ph + 8, 8), number(ph + 32, 8),
                       number(ph + 40, 8), static_cast<uint32_t>(number(ph + 4, 4))};
            const uint64_t align = number(ph + 48, 8);
            if (!s.memory_size || s.memory_size > limit || s.file_size > s.memory_size ||
                s.file_offset > length || s.file_size > length - s.file_offset ||
                s.address > UINT64_MAX - s.memory_size ||
                s.address + s.memory_size > UINT64_MAX - (page - 1) ||
                !(s.permissions & 4) || (s.permissions & ~7U) || (s.permissions & 3) == 3 ||
                (align > 1 && ((align & (align - 1)) || s.address % align != s.file_offset % align))) return -ENOEXEC;
            const uint64_t low = s.address / page * page;
            const uint64_t high = (s.address + s.memory_size + page - 1) / page * page;
            // Host pages must not alias two independently protected guest regions.
            for (const auto& old : parsed) {
                const uint64_t old_low = old.address / page * page;
                const uint64_t old_high = (old.address + old.memory_size + page - 1) / page * page;
                if (low < old_high && old_low < high) return -ENOEXEC;
            }
            begin = std::min(begin, low);
            end = std::max(end, high);
            if (end - begin > limit) return -E2BIG;
            if ((s.permissions & 1) && entry >= s.address && entry - s.address < s.file_size) {
                code_offset = static_cast<size_t>(s.file_offset + entry - s.address);
                code_size = static_cast<size_t>(s.file_size - (entry - s.address));
            }
            parsed.push_back(s);
        }
    } catch (const std::bad_alloc&) { return -ENOMEM; }
    if (!code_size) return -ENOEXEC;
    const int admitted = validateGuestProgram(file + code_offset, code_size, budget, entry, &parsed);
    if (admitted) return admitted;
    std::vector<const uint8_t*> sources;
    try {
        for (const auto& segment : parsed) sources.push_back(file + segment.file_offset);
    } catch (const std::bad_alloc&) { return -ENOMEM; }
    return map_segments(std::move(parsed), sources, entry);
}
int GuestELF::interpret(uint32_t budget, Execution& out) {
    out = {};
    if (!arena || blockers || !budget || budget > 1000000) return -EINVAL;
    out.rip = guest_entry;
    bool initialized = false;
    PS4GuestMemory memory(*this);
    while (out.instructions < budget) {
        uint8_t op = 0;
        if (!memory.fetch(out.rip, &op, 1)) return -EFAULT;
        const size_t width = (op == 0xB8 || op == 0x05) ? 5 : op == 0xC7 ? 10 : 1;
        uint8_t code[10] = {};
        if (out.rip > UINT64_MAX - width || !memory.fetch(out.rip, code, width)) return -EFAULT;
        const uint64_t next = out.rip + width;
        if (op == 0xB8 || op == 0x05) {
            if (op == 0x05 && !initialized) return -ENOTSUP;
            const uint32_t immediate = static_cast<uint32_t>(number(code + 1, 4));
            out.rax = op == 0xB8 ? immediate : static_cast<uint32_t>(out.rax + immediate);
            initialized = true;
        } else if (op == 0xC7) {
            if (code[1] != 0x05) return -ENOTSUP;
            const uint64_t raw = number(code + 2, 4);
            const int64_t displacement = raw >= 0x80000000ULL ?
                static_cast<int64_t>(raw) - 0x100000000LL : static_cast<int64_t>(raw);
            if ((displacement < 0 && next < static_cast<uint64_t>(-displacement)) ||
                (displacement >= 0 && next > UINT64_MAX - static_cast<uint64_t>(displacement))) return -EFAULT;
            const uint64_t target = displacement < 0 ? next - static_cast<uint64_t>(-displacement) :
                next + static_cast<uint64_t>(displacement);
            if (!memory.write(target, code + 6, 4)) return -EFAULT;
        } else if (op != 0x90 && op != 0xF4) return -ENOTSUP;
        ++out.instructions;
        out.rip = next;
        if (op == 0xF4) return initialized ? 0 : -ENOTSUP;
    }
    return -E2BIG;
}
int GuestELF::prepare_orbis(const OrbisLinker::Image& linked, const std::string& root,
    uint32_t budget, std::string& diagnostic) {
    clear(); diagnostic.clear();
    return prepare_linked(linked, root, budget, diagnostic, false);
}
int GuestELF::prepare_orbis_general(const std::vector<OrbisLinker::Input>& inputs,
    const std::vector<OrbisLinker::HLE>& hle, const std::string& root,
    std::string& diagnostic, uint64_t& relocation) {
    clear(); diagnostic.clear(); relocation = 0;
    auto fail = [&](int error, const std::string& message) {
        clear(); diagnostic = message; return error;
    };
    try {
        OrbisLinker::Image measured;
        if (!OrbisLinker::link(inputs, hle, measured, diagnostic)) return fail(-ENOEXEC, diagnostic);
        const long host_page = ::sysconf(_SC_PAGESIZE);
        if (host_page <= 0) return fail(-EIO, "host page size unavailable");
        const uint64_t page = static_cast<uint64_t>(host_page);
        uint64_t low = UINT64_MAX, high = 0;
        for (const auto& module : measured.modules) {
            // An ET_SCE_EXEC has absolute addresses fixed at link time: it cannot move.
            if (module.image.type == 0xfe00)
                return fail(-ENOEXEC, module.path + ": ET_SCE_EXEC is not relocatable (identity mapping needs ET_SCE_DYNEXEC or PRX)");
            for (const auto& segment : module.image.segments) {
                low = std::min(low, module.bias + segment.header.address);
                high = std::max(high, module.bias + segment.header.address + segment.header.memorySize);
            }
        }
        if (low >= high) return fail(-ENOEXEC, "image has no loadable segment");
        // After the image: startup stub, static TLS and stack, each behind a guard
        // page (see prepare_linked), page rounding, a guard page after the stack
        // and base alignment, then the child thread slots (stack + TLS + guard).
        low &= ~(generalAlignment - 1);
        if (high - low > generalSpan)
            return fail(-E2BIG, "image span exceeds the general address-space reservation");
        uint64_t window = 0, tail = 0;
        size_t size = 0;
        auto layout = [&](uint64_t heap) {
            window = heap;
            tail = 10 * page + OrbisLinker::tlsBudget + 2 * 4096 + generalStack + 2 * generalHeapPage + heap +
                generalThreads * (generalStack + OrbisLinker::tlsBudget + 2 * 4096 + 2 * page);
            size = static_cast<size_t>(high - low + tail + generalAlignment);
        };
        // GPU-visible memory (the mmap window) must fit the PS4's 40-bit GPU
        // addresses: games keep them >> 8 in 32-bit fields and dereference
        // them on the CPU. Linux places mappings near 2^47 (or above it on a
        // 48-bit ARM64 layout). Try the default placement, then hints below
        // the limit; without MAP_FIXED no mapping is ever replaced. Where
        // nothing fits below 2^40 (AddressSanitizer's x86-64 shadow covers it),
        // fall back to the x86-64 user limit, 2^47.
        uint64_t user_end = 0;
        auto reserve = [&](uint64_t hint) -> void* {
            void* mapped = ::mmap(reinterpret_cast<void*>(hint), size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapped == MAP_FAILED || reinterpret_cast<uintptr_t>(mapped) <= user_end - size) return mapped;
            ::munmap(mapped, size);
            return MAP_FAILED;
        };
        // The largest window that still leaves generalHeadroom of address
        // space for what the run maps later (FEX's ~272 MiB lookup cache per
        // guest thread: Mortal Kombat: Legacy Kollection's threads crashed
        // when a 24 GiB window left none). Without that room anywhere, the
        // largest window that fits, as before.
        auto room = [] {
            void* spare = ::mmap(nullptr, size_t(generalHeadroom), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (spare == MAP_FAILED) return false;
            ::munmap(spare, size_t(generalHeadroom));
            return true;
        };
        void* reserved = MAP_FAILED;
        for (const bool headroom : {true, false}) {
            for (const uint64_t heap : generalHeapSizes) {
                layout(heap);
                for (const unsigned bits : {40U, 47U}) {
                    user_end = uint64_t{1} << bits;
                    reserved = reserve(0);
                    for (uint64_t hint = uint64_t{1} << 36; reserved == MAP_FAILED && hint <= user_end - size; hint <<= 1)
                        reserved = reserve(hint);
                    if (reserved != MAP_FAILED) break;
                }
                if (reserved != MAP_FAILED && headroom && !room()) {
                    ::munmap(reserved, size);
                    reserved = MAP_FAILED;
                }
                if (reserved != MAP_FAILED) break;
            }
            if (reserved != MAP_FAILED) break;
        }
        if (reserved == MAP_FAILED) return fail(-ENOMEM, "general address-space reservation failed below 2^47");
        // Owned from here: clear() releases it on every later failure. Guest and
        // host addresses coincide inside it (translate is the identity).
        arena = reserved; arena_size = size; heap_window = window;
        // What iOS still grants after it, in 256 MiB spans (up to 16 GiB):
        // the room left for FEX's per-thread caches and everything else.
        {
            constexpr size_t span = size_t(256) << 20;
            std::vector<void*> spans;
            for (int i = 0; i < 64; ++i) {
                void* spare = ::mmap(nullptr, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (spare == MAP_FAILED) break;
                spans.push_back(spare);
            }
            spare_bytes = uint64_t(spans.size()) * span;
            for (void* spare : spans) ::munmap(spare, span);
        }
        guest_base = reinterpret_cast<uintptr_t>(reserved);
        const uint64_t base = (guest_base + generalAlignment - 1) & ~(generalAlignment - 1);
        if (base + (high - low) + tail > user_end)
            return fail(-E2BIG, "general reservation outside the user address range");
        auto moved = inputs;
        for (auto& input : moved) {
            // bias - low + base: the same layout, shifted by a multiple of the alignment.
            if (input.bias < low && low - input.bias > base) return fail(-ENOEXEC, "load bias below the image");
            input.bias = input.bias - low + base;
        }
        OrbisLinker::Image linked;
        if (!OrbisLinker::link(moved, hle, linked, diagnostic)) return fail(-ENOEXEC, diagnostic);
        relocation = base - low;
        const int status = prepare_linked(linked, root, 0, diagnostic, true);
        if (status) relocation = 0;
        return status;
    } catch (const std::bad_alloc&) { clear(); relocation = 0; return -ENOMEM; }
}
int GuestELF::prepare_linked(const OrbisLinker::Image& linked, const std::string& root,
    uint32_t budget, std::string& diagnostic, bool mapped_identity) {
    auto fail = [&](int error, const char* message) {
        clear(); diagnostic = message; return error;
    };
    if (!mapped_identity && (!budget || budget > 65536)) return fail(-EINVAL, "invalid Orbis CPU budget");
    try {
        // Patch only our private copy. Linker's logical relocations stay intact.
        auto copy = linked;
        uint64_t entry = 0, highest = 0;
        for (const auto& module : copy.modules) {
            if (module.path == root) entry = module.entry;
            for (const auto& segment : module.image.segments)
                highest = std::max(highest, module.bias + segment.header.address + segment.header.memorySize);
        }
        if (!entry) return fail(-ENOEXEC, "root has no process entry");
        const long host_page = ::sysconf(_SC_PAGESIZE);
        if (host_page <= 0) return fail(-EIO, "host page size unavailable");
        const uint64_t page = static_cast<uint64_t>(host_page);
        // Dynamic startup: an RX stub after a guard page. For each PRX, in load
        // order, module_start(0, NULL, NULL): XOR EDI,EDI; XOR ESI,ESI;
        // XOR EDX,EDX; CALL init. Then JMP entry. Admission starts at the stub,
        // so every module_start is admitted as a function, the entry as the body.
        // General images always get the stub and enter like a PS4 process (as
        // shadPS4 RunMainEntry): after the module_start calls, SUB RSP,8; RDI =
        // &EntryParams; PUSH [RDI+8]; PUSH [RDI]; RSI = exit function; JMP
        // entry. The exit function follows: an engine trampoline (SYSCALL at
        // +8 like the linker's) whose HLT ends the run. Then the thread-return
        // trampoline, a child thread's return address: MOV RDI,RAX; MOV EAX,
        // token; SYSCALL (thread_exit, at +8); HLT.
        constexpr uint64_t call_block = 11, entry_block = 34, exit_block = 14, return_block = 14;
        uint64_t start = entry;
        size_t stub_module = SIZE_MAX, entry_at = 0;
        if (!copy.initializers.empty() || mapped_identity) {
            start = (highest + page - 1) / page * page + page;
            entry_at = static_cast<size_t>(call_block * copy.initializers.size());
            const uint64_t size = entry_at + (mapped_identity ? entry_block + exit_block + return_block : 5);
            if (start > (uint64_t{1} << 47) - size) return fail(-E2BIG, "startup stub outside user range");
            std::vector<uint8_t> stub(static_cast<size_t>(size));
            auto branch = [&](size_t at, uint8_t opcode, uint64_t target) {
                const int64_t relative = static_cast<int64_t>(target) - static_cast<int64_t>(start + at + 5);
                if (relative < INT32_MIN || relative > INT32_MAX) return false;
                stub[at] = opcode;
                for (size_t b = 0; b < 4; ++b) stub[at + 1 + b] = uint8_t(uint64_t(relative) >> (8 * b));
                return true;
            };
            for (size_t k = 0; k < copy.initializers.size(); ++k) {
                const size_t at = static_cast<size_t>(call_block * k);
                const uint8_t zero[] = {0x31,0xff,0x31,0xf6,0x31,0xd2};
                std::copy(std::begin(zero), std::end(zero), stub.begin() + at);
                if (!branch(at + 6, 0xe8, copy.initializers[k])) return fail(-EOVERFLOW, "module_start CALL displacement overflow");
            }
            size_t jump = entry_at;
            if (mapped_identity) {
                // SUB RSP,8; MOV RDI,imm64; PUSH [RDI+8]; PUSH [RDI]; MOV RSI,imm64;
                // both immediates are patched once the stack is placed.
                const uint8_t enter[] = {0x48,0x83,0xec,0x08, 0x48,0xbf,0,0,0,0,0,0,0,0,
                    0xff,0x77,0x08, 0xff,0x37, 0x48,0xbe,0,0,0,0,0,0,0,0};
                std::copy(std::begin(enter), std::end(enter), stub.begin() + entry_at);
                jump = entry_at + sizeof(enter);
                // Exit function: MOV R10,RCX; MOV EAX,token; SYSCALL; HLT; (padding).
                const uint32_t token = static_cast<uint32_t>(PS4KernelService::process_exit);
                const uint8_t leave[] = {0x49,0x89,0xca, 0xb8,uint8_t(token),uint8_t(token>>8),
                    uint8_t(token>>16),uint8_t(token>>24), 0x0f,0x05, 0xf4, 0xcc,0xcc,0xcc};
                static_assert(sizeof(enter) + 5 == entry_block && sizeof(leave) == exit_block, "stub layout");
                std::copy(std::begin(leave), std::end(leave), stub.begin() + entry_at + entry_block);
                copy.thunks.push_back({start + entry_at + entry_block, token});
                const uint32_t ends = static_cast<uint32_t>(PS4KernelService::thread_exit);
                const uint8_t back[] = {0x48,0x89,0xc7, 0xb8,uint8_t(ends),uint8_t(ends>>8),
                    uint8_t(ends>>16),uint8_t(ends>>24), 0x0f,0x05, 0xf4, 0xcc,0xcc,0xcc};
                static_assert(sizeof(back) == return_block, "stub layout");
                const size_t back_at = entry_at + entry_block + exit_block;
                std::copy(std::begin(back), std::end(back), stub.begin() + back_at);
                copy.thunks.push_back({start + back_at, ends});
            }
            if (!branch(jump, 0xe9, entry))
                return fail(-EOVERFLOW, "entry JMP displacement overflow");
            stub_module = copy.modules.size();
            OrbisLinker::Module module {"<start>", start, 0, PS4ExecutableImage{}};
            PS4ExecutableImage::Header header {1, 5, 0, 0, size, size, page};
            module.image.headers.push_back(header);
            module.image.segments.push_back({header, std::move(stub)});
            copy.modules.push_back(std::move(module));
            highest = start + size;
        }
        // Static TLS (variant II): one RW region after a guard page, blocks
        // ending at the thread pointer, then a 16-byte TCB at TP.
        uint64_t tls_low = 0, tp = 0;
        std::vector<uint8_t> tls_bytes;
        if (copy.tls.size() != static_cast<size_t>(std::count_if(copy.modules.begin(), copy.modules.end(),
                [](const auto& module) { return module.image.requiresTLS; })))
            return fail(-ENOEXEC, "TLS layout does not match modules");
        if (!copy.tls.empty()) {
            const uint64_t align = std::max<uint64_t>(copy.staticTLSAlignment, 16);
            const uint64_t blocks = (copy.staticTLSSize + align - 1) / align * align;
            if (blocks > OrbisLinker::tlsBudget + align || highest > UINT64_MAX - 2 * page - blocks - 16)
                return fail(-E2BIG, "static TLS region too large");
            tls_low = (highest + page - 1) / page * page + page;
            tp = tls_low + blocks;
            tls_bytes.assign(static_cast<size_t>(blocks + 16), 0);
            for (const auto& block : copy.tls) {
                const auto* source = block.fileSize ? copy.translate(block.image, block.fileSize, 4) : nullptr;
                if (block.fileSize && !source) return fail(-EFAULT, "TLS template unreadable");
                if (block.fileSize) std::memcpy(tls_bytes.data() + (blocks - block.offset), source, block.fileSize);
            }
        }
        // General mode: a zeroed RW stack after a guard page, then the mmap
        // window, then the child thread slots, each followed by a guard page.
        uint64_t stack_low = 0, heap_low = 0, slots_low = 0, slot_stride = 0;
        if (mapped_identity) {
            const uint64_t after = tp ? tp + 16 : highest;
            stack_low = (after + page - 1) / page * page + page;
            heap_low = (stack_low + generalStack + page + generalHeapPage - 1) / generalHeapPage * generalHeapPage;
            slots_low = heap_low + heap_window + page;
            slot_stride = (generalStack + tls_bytes.size() + page - 1) / page * page + page;
            // EntryParams sit at the top of the stack; the stub points RDI at
            // them and RSI at its exit function.
            const uint64_t params = stack_low + generalStack - entryParamsBytes;
            const uint64_t exit_function = start + entry_at + entry_block;
            auto& bytes = copy.modules[stub_module].image.segments[0].bytes;
            for (size_t b = 0; b < 8; ++b) {
                bytes[entry_at + 6 + b] = uint8_t(params >> (8 * b));
                bytes[entry_at + 21 + b] = uint8_t(exit_function >> (8 * b));
            }
            for (auto& module : copy.modules) for (auto& segment : module.image.segments)
                if ((segment.header.flags & 5) == 5 && !segment.header.address)
                    orbisRelocateFixedLayout(segment.bytes.data(), size_t(segment.header.fileSize), heap_low, heap_window);
        }
        // Local-exec read through FS must stay inside the blocks or the TCB.
        auto tls_readable = [&](int64_t displacement, uint64_t bytes) {
            if (!tp) return false;
            const uint64_t target = tp + static_cast<uint64_t>(displacement);
            return target >= tls_low && target <= tp + 16 - bytes;
        };
        auto fetch = [&](uint64_t address, size_t count) -> uint8_t* {
            for (auto& module : copy.modules) for (auto& segment : module.image.segments) {
                const uint64_t begin = module.bias + segment.header.address;
                if ((segment.header.flags & 5) == 5 && address >= begin &&
                    address - begin <= segment.header.fileSize &&
                    count <= segment.header.fileSize - (address - begin))
                    return segment.bytes.data() + address - begin;
            }
            return nullptr;
        };
        struct Patch { uint8_t* code; uint32_t displacement; };
        std::vector<Patch> patches;
        std::set<uint64_t> active;
        std::map<uint64_t,size_t> instruction_ranges;
        bool initialized = false;
        bool edx_initialized = false, ebx_initialized = false, esi_initialized = false, ecx_initialized = false, compared = false;
        bool rax_tp = false, rax_tpoff = false, rax_tls_pointer = false;
        int64_t tpoff = 0;
        uint32_t remaining = budget;
        uint64_t refusal_pc = entry;
        uint8_t refusal_opcode = 0;
        std::function<int(uint64_t,bool,size_t)> admit = [&](uint64_t pc, bool function, size_t depth) -> int {
            if (depth > 32 || !active.insert(pc).second) return -ELOOP;
            const uint64_t start = pc;
            struct Erase { std::set<uint64_t>& active; uint64_t start; ~Erase() { active.erase(start); } } erase{active,start};
            while (remaining) {
                --remaining;
                refusal_pc = pc; refusal_opcode = 0;
                auto* code = fetch(pc,1);
                if (!code) return -EFAULT;
                const uint8_t op = code[0];
                refusal_opcode = op;
                size_t width = op == 0xb8 || op == 0xb9 || op == 0xba || op == 0xbb || op == 0xbe || op == 0xbf ||
                    op == 0x05 || op == 0x25 || op == 0x0d ? 5 :
                    op == 0xff || op == 0x8b || op == 0x81 ? 6 :
                    op == 0xc7 ? 10 : op == 0xe8 || op == 0xe9 ? 5 : op == 0x64 ? 8 : op == 0x48 ? 7 : op == 0xc1 || op == 0x0f ? 3 :
                    op == 0x01 || op == 0x29 || op == 0x31 || op == 0x39 || op == 0x89 ? 2 : 1;
                // TLS forms are told apart by their second byte.
                if ((op == 0x8b || op == 0x64) && fetch(pc,2)) {
                    if (op == 0x8b && code[1] == 0x40) width = 3;
                    if (op == 0x8b && code[1] == 0x00) width = 2;
                    if (op == 0x64 && code[1] == 0x48) width = 9;
                    if (op == 0x64 && code[1] == 0x8b && fetch(pc,3) && code[2] == 0x00) width = 3;
                }
                if (!fetch(pc,width)) return -EFAULT;
                // RAX holds the thread pointer (or a GOT TLS offset) only for the
                // instruction right after the load; anything in between invalidates it.
                const bool tp_in_rax = rax_tp, tpoff_in_rax = rax_tpoff, tls_pointer_in_rax = rax_tls_pointer;
                rax_tp = rax_tpoff = rax_tls_pointer = false;
                if (op == 0x89 && code[1] != 0xc3 && code[1] != 0xc6) {
                    width = 6;
                    if (!fetch(pc,width)) return -EFAULT;
                }
                // A CALL target may not enter another instruction's operand.
                // Otherwise rewriting one decoded stream could invalidate a
                // different stream that was admitted over overlapping bytes.
                const auto next = instruction_ranges.lower_bound(pc);
                if (next != instruction_ranges.end() && next->first == pc) {
                    if (next->second != width) return -ENOTSUP;
                } else {
                    if (next != instruction_ranges.end() && next->first < pc+width) return -ENOTSUP;
                    if (next != instruction_ranges.begin()) {
                        const auto previous = std::prev(next);
                        if (previous->first+previous->second > pc) return -ENOTSUP;
                    }
                    instruction_ranges.emplace_hint(next,pc,width);
                }
                if (op == 0xb8 || op == 0x05) {
                    width = 5;
                    if (!fetch(pc,width) || (op == 0x05 && !initialized)) return -ENOTSUP;
                    initialized = true;
                    if (op == 0x05) compared = false;
                } else if (op == 0xb9) {
                    ecx_initialized = true;
                } else if (op == 0xba || op == 0xbb) {
                    if (op == 0xba) edx_initialized = true; else ebx_initialized = true;
                } else if (op == 0xbe || op == 0xbf) {
                    // MOV ESI/EDI,imm32: integer arguments for HLE calls.
                    if (op == 0xbe) esi_initialized = true;
                } else if (op == 0x89 && code[1] == 0xc6) {
                    if (!initialized) return -ENOTSUP;
                    esi_initialized = true;
                } else if (op == 0x89 && code[1] == 0xc3) {
                    if (!initialized) return -ENOTSUP;
                    ebx_initialized = true; // MOV EBX,EAX
                } else if (op == 0x8b && code[1] == 0x00) {
                    // MOV EAX,[RAX] right after CALL __tls_get_addr. The returned pointer
                    // is at most TP-1, so a 4-byte read ends before the TCB's end.
                    if (!tls_pointer_in_rax) return -ENOTSUP;
                    initialized = true; compared = false;
                } else if (op == 0x8b && (code[1] == 0x40 || code[1] == 0x80)) {
                    // MOV EAX,[RAX+disp8/disp32] with RAX = thread pointer: TLS read.
                    if (!tp_in_rax) return -ENOTSUP;
                    const int64_t disp = code[1] == 0x40 ? static_cast<int8_t>(code[2]) :
                        static_cast<int32_t>(number(code+2,4));
                    if (!tls_readable(disp, 4)) return -EFAULT;
                    initialized = true; compared = false;
                } else if (op == 0x8b || op == 0x89) {
                    // Only EAX/EDX RIP-relative operands, never register pointers.
                    const bool eax = code[1] == 0x05, edx = code[1] == 0x15;
                    if (!eax && !edx) return -ENOTSUP;
                    const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc+6) +
                        static_cast<int32_t>(number(code+2,4)));
                    if (op == 0x8b) {
                        if (!copy.translate(target,4,4) || copy.translate(target,4,1)) return -EFAULT;
                        if (eax) initialized = true; else edx_initialized = true;
                    } else {
                        if (!(eax ? initialized : edx_initialized)) return -ENOTSUP;
                        if (!copy.translate(target,4,2) || copy.translate(target,4,1)) return -EFAULT;
                    }
                } else if (op == 0x25 || op == 0x0d) {
                    if (!initialized) return -ENOTSUP;
                    compared = false;
                } else if (op == 0xc1) {
                    // SHL/SHR EAX or SHL EDX, with bounded immediate count.
                    if (code[2] > 31 || (code[1] != 0xe0 && code[1] != 0xe8 && code[1] != 0xe2)) return -ENOTSUP;
                    if (!(code[1] == 0xe2 ? edx_initialized : initialized)) return -ENOTSUP;
                    compared = false;
                } else if (op == 0x01 || op == 0x29) {
                    if (code[1] != 0xc2 || !initialized || !edx_initialized) return -ENOTSUP;
                    compared = false; // ADD/SUB EDX,EAX
                } else if (op == 0x39) {
                    if (code[1] != 0xd6 || !esi_initialized || !edx_initialized) return -ENOTSUP;
                    compared = true;
                } else if (op == 0x81) {
                    if (code[1] == 0xfe) {
                        if (!esi_initialized) return -ENOTSUP;
                    } else if (!edx_initialized || (code[1] != 0xfa && code[1] != 0xe2)) return -ENOTSUP;
                    compared = code[1] == 0xfa || code[1] == 0xfe; // CMP or AND EDX,imm32
                } else if (op == 0x0f) {
                    if (code[1] != 0x44 || !compared || !initialized ||
                        !((code[2] == 0xc3 && ebx_initialized) || (code[2] == 0xc1 && ecx_initialized) ||
                          (code[2] == 0xcb && ecx_initialized && ebx_initialized))) return -ENOTSUP;
                    // CMOVE EAX,EBX; MOV does not alter the comparison flags.
                } else if (op == 0xff) {
                    width = 6;
                    if (!fetch(pc,width) || code[1] != 0x15) return -ENOTSUP;
                    const int64_t disp = static_cast<int32_t>(number(code+2,4));
                    const uint64_t slot = static_cast<uint64_t>(static_cast<int64_t>(pc+6)+disp);
                    const auto* pointer = copy.translate(slot,8,4);
                    if (!pointer) return -EFAULT;
                    const uint64_t target = number(pointer,8);
                    if (target >= (uint64_t{1}<<47) || pc >= (uint64_t{1}<<47)) return -EFAULT;
                    const int64_t relative = static_cast<int64_t>(target)-static_cast<int64_t>(pc+5);
                    if (relative < INT32_MIN || relative > INT32_MAX) return -EOVERFLOW;
                    // A linker-synthesized HLE trampoline is engine code, not guest
                    // code: it is not admitted instruction by instruction. It returns
                    // in EAX; SYSCALL clobbers RCX (and R11, which is not tracked).
                    const auto thunk = std::find_if(copy.thunks.begin(), copy.thunks.end(),
                        [&](const auto& t) { return t.address == target; });
                    const bool system = thunk != copy.thunks.end();
                    if (system) {
                        initialized = true; ecx_initialized = false; compared = false;
                        // __tls_get_addr returns a pointer inside the static TLS region
                        // (the gate validates tls_index at run time and never returns
                        // anything else), usable by the very next instruction.
                        if (thunk->token == static_cast<uint32_t>(PS4KernelService::tls_get_addr)) {
                            if (!tp) return -EFAULT;
                            rax_tls_pointer = true;
                        }
                    } else {
                        const int error = admit(target,true,depth+1);
                        if (error) return error;
                    }
                    patches.push_back({code,static_cast<uint32_t>(relative)});
                } else if (op == 0x31) {
                    // XOR EDI,EDI / ESI,ESI / EDX,EDX: zero arguments (startup stub).
                    if (code[1] != 0xff && code[1] != 0xf6 && code[1] != 0xd2) return -ENOTSUP;
                    if (code[1] == 0xf6) esi_initialized = true;
                    if (code[1] == 0xd2) edx_initialized = true;
                    compared = false;
                } else if (op == 0xe8) {
                    // CALL rel32 (startup stub): the target is admitted as a function.
                    const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc+5) +
                        static_cast<int32_t>(number(code+1,4)));
                    const int error = admit(target,true,depth+1);
                    if (error) return error;
                } else if (op == 0xe9) {
                    // JMP rel32 (startup stub -> entry): admission continues at the
                    // target in the same context; loops exhaust the budget.
                    pc = static_cast<uint64_t>(static_cast<int64_t>(pc+5) + static_cast<int32_t>(number(code+1,4)));
                    continue;
                } else if (op == 0xc7) {
                    width = 10;
                    if (!fetch(pc,width) || code[1] != 5) return -ENOTSUP;
                    const int64_t disp = static_cast<int32_t>(number(code+2,4));
                    const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc+10)+disp);
                    if (!copy.translate(target,4,2) || copy.translate(target,4,1)) return -EFAULT;
                } else if (op == 0x48 && code[1] == 0x8d) {
                    // LEA RDI/RSI/RDX,[RIP+rel32] (48 8D 3D/35/15): pointer argument for an
                    // HLE call. It may only point into mapped, non-executable guest data;
                    // the callee still checks permissions and length on every access.
                    if (code[2] != 0x3d && code[2] != 0x35 && code[2] != 0x15) return -ENOTSUP;
                    const uint64_t target = static_cast<uint64_t>(static_cast<int64_t>(pc+7) +
                        static_cast<int32_t>(number(code+3,4)));
                    if (!copy.translate(target,1,4) || copy.translate(target,1,1)) return -EFAULT;
                    if (code[2] == 0x35) esi_initialized = true;
                    if (code[2] == 0x15) edx_initialized = true;
                } else if (op == 0x48) {
                    // MOV RAX,[RIP+rel32] (48 8B 05): initial-exec GOT load of a TPOFF64
                    // value. The slot must be read-only at run time (RELRO after
                    // linking), so no admitted store can change the offset later.
                    if (code[1] != 0x8b || code[2] != 0x05) return -ENOTSUP;
                    const uint64_t slot = static_cast<uint64_t>(static_cast<int64_t>(pc+7) +
                        static_cast<int32_t>(number(code+3,4)));
                    const auto* value = copy.translate(slot,8,4);
                    if (!value || copy.translate(slot,8,2) || copy.translate(slot,8,1)) return -EFAULT;
                    rax_tpoff = true; tpoff = static_cast<int64_t>(number(value,8));
                    initialized = true; compared = false;
                } else if (op == 0x64 && code[1] == 0x8b && code[2] == 0x00) {
                    // MOV EAX,FS:[RAX] (64 8B 00) right after the GOT load.
                    if (!tpoff_in_rax) return -ENOTSUP;
                    if (!tls_readable(tpoff, 4)) return -EFAULT;
                    initialized = true; compared = false;
                } else if (op == 0x64 && code[1] == 0x48) {
                    // MOV RAX, FS:[0] (64 48 8B 04 25 00000000): load the thread pointer
                    // from the TCB self-pointer.
                    if (code[2] != 0x8b || code[3] != 0x04 || code[4] != 0x25 || number(code+5,4)) return -ENOTSUP;
                    if (!tls_readable(0, 8)) return -EFAULT;
                    rax_tp = true; initialized = true; compared = false;
                } else if (op == 0x64) {
                    // MOV EAX, FS:[disp32] (64 8B 04 25): static TLS local-exec read.
                    if (code[1] != 0x8b || code[2] != 0x04 || code[3] != 0x25) return -ENOTSUP;
                    if (!tls_readable(static_cast<int32_t>(number(code+4,4)), 4)) return -EFAULT;
                    initialized = true; compared = false;
                } else if (op == 0xc3 || op == 0xf4) {
                    if (!initialized || (function ? op != 0xc3 : op != 0xf4)) return -ENOTSUP;
                    return 0;
                } else if (op != 0x90) return -ENOTSUP;
                pc += width;
            }
            return -E2BIG;
        };
        // General mode admits no instruction list and rewrites nothing: GOT slots
        // already hold host (= guest) addresses, so CALL [GOT] runs as is.
        const int admitted = mapped_identity ? 0 : admit(start,false,0);
        if (admitted) {
            const char* cause = admitted == -EFAULT ? "invalid guest memory range" :
                admitted == -ELOOP ? "recursive call or depth limit" :
                admitted == -E2BIG ? "instruction admission budget exhausted" :
                admitted == -EOVERFLOW ? "CALL displacement overflow" : "unsupported instruction or termination";
            std::ostringstream detail;
            detail << "Orbis CPU admission refused: " << cause << "; guest PC=0x" << std::hex
                   << refusal_pc << "; opcode=0x" << unsigned(refusal_opcode);
            clear(); diagnostic = detail.str(); return admitted;
        }
        for (const auto& patch : patches) {
            patch.code[0] = 0xe8;
            for (size_t i=0;i<4;++i) patch.code[1+i] = uint8_t(patch.displacement >> (8*i));
            patch.code[5] = 0x90;
        }
        std::vector<Segment> parsed;
        std::vector<const uint8_t*> sources;
        for (const auto& module : copy.modules) for (const auto& segment : module.image.segments) {
            const auto& h = segment.header;
            // Copy relocated BSS as well as file bytes; retain file extent for CPU queries.
            parsed.push_back({module.bias+h.address,h.offset,h.memorySize,h.memorySize,h.flags});
            sources.push_back(segment.bytes.data());
        }
        if (tp) { // RW, never executable.
            parsed.push_back({tls_low,0,tls_bytes.size(),tls_bytes.size(),6});
            sources.push_back(tls_bytes.data());
        }
        if (stack_low) { // Zero-filled: no file bytes to copy.
            parsed.push_back({stack_low,0,0,generalStack,6});
            sources.push_back(nullptr);
            // Thread slots: TLS bytes are copied when a thread is created.
            for (size_t k = 0; k < generalThreads; ++k) {
                parsed.push_back({slots_low + k * slot_stride,0,0,generalStack + tls_bytes.size(),6});
                sources.push_back(nullptr);
            }
        }
        const int mapped = map_segments(std::move(parsed),sources,entry);
        if (mapped) return fail(mapped,"Orbis page mapping refused (page overlap, span or protection)");
        size_t i=0;
        for (const auto& module : copy.modules) for (const auto& segment : module.image.segments)
            regions[i++].file_size = segment.header.fileSize;
        thunks = copy.thunks;
        hle_data = copy.data;
        for (const auto& module : copy.modules) if (module.path == root)
            for (const auto& h : module.image.headers) if (h.type == 0x61000001) process_param_address = module.bias + h.address;
        for (size_t m = 0; m < copy.modules.size(); ++m) {
            const auto& module = copy.modules[m];
            if (module.path.empty() || module.path[0] == '<') continue; // Synthesized.
            ModuleRecord record;
            record.name = module.path;
            record.base = UINT64_MAX;
            for (const auto& segment : module.image.segments) {
                record.base = std::min(record.base, module.bias + segment.header.address);
                record.end = std::max(record.end, module.bias + segment.header.address + segment.header.memorySize);
                const uint32_t f = segment.header.flags; // ELF R 4, W 2, X 1.
                record.segments.push_back({module.bias + segment.header.address, segment.header.memorySize,
                                           ((f & 4U) ? 1U : 0U) | (f & 2U) | ((f & 1U) ? 4U : 0U)});
            }
            if (record.base == UINT64_MAX) continue;
            for (const auto& h : module.image.headers) if (h.type == 0x6474e550) { // PT_GNU_EH_FRAME
                record.eh_frame_hdr = module.bias + h.address; record.eh_frame_hdr_size = h.memorySize;
            }
            for (const auto& e : copy.exported) if (e.module == m) record.exports.push_back({e.nid, e.address});
            records.push_back(std::move(record));
        }
        start_address = start;
        if (tp) tls_blocks = copy.tls;
        if (tp) {
            // x86-64 TCB word 0 is its own address, as FEX will see it (host).
            thread_pointer = tp;
            auto* tcb = const_cast<uint8_t*>(translate(tp,16,6));
            if (!tcb) return fail(-EFAULT,"TLS thread control block unmapped");
            const uint64_t self = reinterpret_cast<uintptr_t>(tcb);
            std::memcpy(tcb,&self,sizeof(self));
        }
        if (stack_low) {
            stack_address = stack_low; stack_bytes = generalStack;
            entry_params_address = stack_low + generalStack - entryParamsBytes;
            // sceKernelMmap window: after a guard page, PROT_NONE until mapped.
            heap_address = heap_low;
            heap_bytes = heap_window;
            // A guard page must follow it inside the reservation.
            if (heap_address + heap_bytes + page > guest_base + arena_size)
                return fail(-E2BIG,"mmap window outside the general reservation");
            threads_address = slots_low; thread_slot_bytes = slot_stride;
            thread_return_address = start + entry_at + entry_block + exit_block;
            tls_image = std::move(tls_bytes);
        }
        identity = mapped_identity;
        // Restricted rebasing never establishes general PS4 addressing; the
        // identity mapping does.
        blockers = mapped_identity ? 0 : 4;
        return 0;
    } catch (const std::bad_alloc&) { clear(); return -ENOMEM; }
}
uint64_t GuestELF::write_entry_params() {
    // EntryParams {int argc; u32 pad; const char* argv[3]} then argv[0].
    if (!entry_params_address) return 0;
    auto* params = const_cast<uint8_t*>(translate(entry_params_address, entryParamsBytes, 6));
    if (!params) return 0;
    static constexpr char program[] = "eboot.bin";
    const uint64_t words[4] = {1, entry_params_address + 32, 0, 0};
    std::memset(params, 0, entryParamsBytes);
    std::memcpy(params, words, sizeof(words));
    if (program_named) std::memcpy(params + 32, program, sizeof(program));
    return entry_params_address;
}
bool GuestELF::prepare_thread(size_t slot, uint64_t& stack_top, uint64_t& tp) {
    stack_top = tp = 0;
    if (!threads_address || slot >= generalThreads) return false;
    const uint64_t low = threads_address + slot * thread_slot_bytes;
    if (!translate(low, static_cast<size_t>(generalStack + tls_image.size()), 6)) return false;
    stack_top = low + generalStack;
    if (tls_image.empty()) return true;
    // Same variant II layout as the main thread: blocks, then the TCB at TP
    // whose word 0 is its own (host = guest) address.
    auto* tls = const_cast<uint8_t*>(translate(stack_top, tls_image.size(), 6));
    std::memcpy(tls, tls_image.data(), tls_image.size());
    tp = stack_top + tls_image.size() - 16;
    std::memcpy(tls + tls_image.size() - 16, &tp, sizeof(tp));
    return true;
}
int GuestELF::stage_ps4(const uint8_t* file, size_t length) {
    clear();
    PS4ExecutableImage image;
    const int error = image.load(file, length);
    if (error) return error;
    // A shared library has no process entry and needs separate linker ownership.
    if (image.type == 0xfe18) return -ENOTSUP;
    try {
        std::vector<Segment> parsed;
        std::vector<const uint8_t*> sources;
        for (const auto& segment : image.segments) {
            const auto& h = segment.header;
            parsed.push_back({h.address, h.offset, h.fileSize, h.memorySize, h.flags});
            sources.push_back(segment.bytes.data());
        }
        const int mapped = map_segments(std::move(parsed), sources, image.entry);
        if (mapped) return mapped;
        // Staging cannot authorize CPU dispatch: rebased absolute addresses,
        // arbitrary instructions and PS4 ABI/services remain unresolved.
        blockers = 4U | (image.requiresLinker ? 1U : 0U) | (image.requiresTLS ? 2U : 0U);
        return 0;
    } catch (const std::bad_alloc&) { clear(); return -ENOMEM; }
}
int GuestELF::admit_staged_cpu(uint32_t budget) const {
    if (!budget || budget > 1000000) return -EINVAL;
    if (!arena) return -EFAULT;
    if (blockers & 3U) return -ENOTSUP; // Linker/TLS are never bypassed.
    for (const auto& s : regions) {
        if (!(s.permissions & 1U) || guest_entry < s.address ||
            guest_entry - s.address >= s.file_size) continue;
        const size_t bytes = static_cast<size_t>(s.file_size - (guest_entry - s.address));
        const auto* code = translate(guest_entry, bytes, 5);
        if (!code) return -EFAULT;
        // No absolute operands, branches or stack accesses: rebasing preserves
        // every admitted RIP-relative store, with a full RW/non-X range proof.
        return validateGuestProgram(code, bytes, budget, guest_entry, &regions);
    }
    return -EFAULT;
}
int GuestELF::map_segments(std::vector<Segment> parsed,
    const std::vector<const uint8_t*>& sources, uint64_t entry) {
    const long host_page = ::sysconf(_SC_PAGESIZE);
    if (host_page <= 0) return -EIO;
    const uint64_t page = static_cast<uint64_t>(host_page);
    const bool reserved = arena != nullptr;
    const uint64_t span = reserved ? arena_size : limit;
    uint64_t begin = UINT64_MAX, end = 0;
    for (size_t i = 0; i < parsed.size(); ++i) {
        const auto& s = parsed[i];
        if ((s.permissions & 3U) == 3U || s.address > UINT64_MAX - s.memory_size ||
            s.address + s.memory_size > UINT64_MAX - (page - 1)) return -ENOEXEC;
        const uint64_t low = s.address / page * page;
        const uint64_t high = (s.address + s.memory_size + page - 1) / page * page;
        for (size_t j = 0; j < i; ++j) {
            const auto& old = parsed[j];
            const uint64_t old_low = old.address / page * page;
            const uint64_t old_high = (old.address + old.memory_size + page - 1) / page * page;
            if (low < old_high && old_low < high) return -ENOEXEC;
        }
        begin = std::min(begin, low); end = std::max(end, high);
        if (end - begin > span) return -E2BIG;
    }
    if (parsed.empty() || sources.size() != parsed.size()) return -ENOEXEC;
    if (reserved) {
        // Every page must lie inside the reservation, which keeps guard pages free.
        if (begin < guest_base || end - guest_base > arena_size) return -E2BIG;
    } else {
        arena_size = static_cast<size_t>(end - begin);
        void* mapped = ::mmap(nullptr, arena_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped == MAP_FAILED) { arena_size = 0; return -errno; }
        arena = mapped;
        guest_base = begin;
    }
    guest_entry = entry;
    regions = std::move(parsed);
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& s = regions[i];
        const size_t low = static_cast<size_t>(s.address / page * page - guest_base);
        const size_t bytes = static_cast<size_t>((s.address + s.memory_size + page - 1) / page * page - guest_base - low);
        auto* destination = static_cast<uint8_t*>(arena) + low;
        if (::mprotect(destination, bytes, PROT_READ | PROT_WRITE) != 0) {
            const int error = -errno; clear(); return error;
        }
        if (s.file_size)
            std::memcpy(static_cast<uint8_t*>(arena) + (s.address - guest_base), sources[i], s.file_size);
        const int protection = PROT_READ | ((s.permissions & 2) ? PROT_WRITE : 0);
        if (::mprotect(destination, bytes, protection) != 0) {
            const int error = -errno; clear(); return error;
        }
    }
    return 0;
}
}



