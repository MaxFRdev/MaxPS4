#include "maxps4_ps4_linker.h"
#include "PS4ExecutableImage.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <new>
#include <set>

namespace maxps4 {
namespace {
uint64_t u(const uint8_t* p, size_t n = 8) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
void put(uint8_t* p, uint64_t v, size_t n = 8) {
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
bool add(uint64_t value, uint64_t raw_signed, uint64_t& out) {
    if (raw_signed >> 63) {
        const uint64_t magnitude = (~raw_signed) + 1;
        if (value < magnitude) return false;
        out = value - magnitude;
    } else {
        if (value > UINT64_MAX - raw_signed) return false;
        out = value + raw_signed;
    }
    return true;
}
bool relative(uint64_t to, uint64_t from, uint32_t& displacement) {
    if (to >= from) {
        if (to - from > INT32_MAX) return false;
        displacement = static_cast<uint32_t>(to - from);
    } else {
        if (from - to > uint64_t{INT32_MAX} + 1) return false;
        displacement = 0U - static_cast<uint32_t>(from - to);
    }
    return true;
}
uint64_t signed32(uint64_t v) { return v & 0x80000000U ? v | 0xffffffff00000000ULL : v; }
constexpr size_t thunk_size = 11;
}
int PS4DynamicLinker::load(GuestELF& image, const uint8_t* file, size_t length, uint32_t budget) {
    image.clear(); reset();
    auto fail = [&](int error, const char* message) {
        image.clear(); imports_.clear(); stack_ = 0;
        try { diagnostic_ = message; } catch (const std::bad_alloc&) { diagnostic_.clear(); }
        return error;
    };
    if (!budget || budget > 1000000) return fail(-EINVAL, "invalid instruction budget");
    try {
        PS4ExecutableImage parsed;
        int error = parsed.load(file, length);
        if (error) return fail(error, "invalid plaintext PS4 ELF");
        if (parsed.isSelf || parsed.requiresTLS) return fail(-ENOTSUP, "SELF/TLS execution is not implemented");
        const PS4ExecutableImage::Header* dynamic = nullptr;
        for (const auto& h : parsed.headers) {
            if (h.type == 2) {
                if (dynamic) return fail(-ENOEXEC, "duplicate PT_DYNAMIC");
                dynamic = &h;
            } else if (h.type != 1) return fail(-ENOTSUP, "unsupported program header (including SCE tables)");
        }
        if (!dynamic || !dynamic->fileSize || dynamic->fileSize % 16 ||
            dynamic->fileSize > 65536 || dynamic->fileSize > dynamic->memorySize ||
            dynamic->memorySize > 65536 || dynamic->address > UINT64_MAX - dynamic->memorySize ||
            (dynamic->flags & ~7U) || !(dynamic->flags & 4U) ||
            (dynamic->alignment > 1 && ((dynamic->alignment & (dynamic->alignment-1)) ||
                dynamic->address % dynamic->alignment != dynamic->offset % dynamic->alignment)) ||
            dynamic->offset > length || dynamic->fileSize > length - dynamic->offset)
            return fail(-ENOEXEC, "invalid PT_DYNAMIC");
        if (!parsed.translate(dynamic->address,dynamic->memorySize,4))
            return fail(-ENOEXEC, "PT_DYNAMIC exceeds its containing segment");
        const auto* table = parsed.translate(dynamic->address, dynamic->fileSize, 4);
        if (!table || std::memcmp(table, file + dynamic->offset, dynamic->fileSize))
            return fail(-ENOEXEC, "dynamic table is not the declared file-backed memory");
        std::map<uint64_t, uint64_t> tags;
        std::vector<uint64_t> needed;
        bool terminated = false;
        for (size_t at = 0; at < dynamic->fileSize; at += 16) {
            const uint64_t tag = u(table + at), value = u(table + at + 8);
            if (!tag) { terminated = true; break; }
            if (tag == 1) { needed.push_back(value); continue; }
            switch (tag) {
            case 2: case 4: case 5: case 6: case 7: case 8: case 9: case 10:
            case 11: case 12: case 13: case 14: case 20: case 23: case 24:
            case 25: case 26: case 27: case 28: case 30: case 0x6ffffffb: break;
            default: return fail(-ENOTSUP, "unsupported dynamic tag");
            }
            if (!tags.emplace(tag, value).second) return fail(-ENOEXEC, "duplicate dynamic tag");
        }
        if (!terminated || tags[11] != 24 || !tags[4] || !tags[5] || !tags[6] ||
            !tags[10] || tags[10] > 65536 || needed.size() != 1)
            return fail(-ENOEXEC, "missing or invalid symbol/dependency tables");
        if ((tags[9] && tags[9] != 24) || tags[24]) return fail(-ENOEXEC, "invalid dynamic entry size/marker");
        // No initialization is silently skipped. Future module scheduling must
        // explicitly execute DT_INIT/arrays before these are accepted.
        for (uint64_t t : {12U,13U,25U,26U,27U,28U,30U,0x6ffffffbU})
            if (tags[t]) return fail(-ENOTSUP, "module initialization/flags unsupported");
        const auto* strings = parsed.translate(tags[5], tags[10], 4);
        auto name = [&](uint64_t offset, std::string& out) {
            if (!strings || offset >= tags[10]) return false;
            const auto* start = reinterpret_cast<const char*>(strings + offset);
            const auto* end = static_cast<const char*>(std::memchr(start, 0, tags[10] - offset));
            if (!end || end - start > 256) return false;
            out.assign(start, end); return true;
        };
        std::string dependency;
        if (!name(needed[0], dependency)) return fail(-ENOEXEC, "invalid dependency string");
        if (dependency != "libkernel")
            return fail(-ENOTSUP, "unavailable dependency (only libkernel supported)");
        if (tags[14] && !name(tags[14], dependency)) return fail(-ENOEXEC, "invalid DT_SONAME");
        const auto* hash = parsed.translate(tags[4], 8, 4);
        if (!hash) return fail(-ENOEXEC, "invalid DT_HASH");
        const uint64_t buckets = u(hash,4), symbols = u(hash+4,4);
        if (!buckets || !symbols || buckets > 4096 || symbols > 4096 ||
            !parsed.translate(tags[4], (2 + buckets + symbols) * 4, 4))
            return fail(-ENOEXEC, "invalid symbol count");
        hash = parsed.translate(tags[4], (2 + buckets + symbols) * 4, 4);
        for (size_t i = 0; i < buckets + symbols; ++i)
            if (u(hash + 8 + i * 4,4) >= symbols) return fail(-ENOEXEC, "hash index outside symbol table");
        const auto* symtab = parsed.translate(tags[6], symbols * 24, 4);
        if (!symtab) return fail(-ENOEXEC, "symbol table outside memory");
        uint64_t end = 0;
        for (const auto& s : parsed.segments) end = std::max(end, s.header.address + s.header.memorySize);
        if (end > UINT64_MAX - 0x10000) return fail(-EOVERFLOW, "synthetic segment overflow");
        // 16KiB alignment also separates host pages on Darwin ARM64.
        const uint64_t thunk_base = (end + 0x3fff) & ~uint64_t{0x3fff};
        stack_ = thunk_base + 0x4000;
        std::map<uint64_t, Import> by_symbol;
        std::vector<std::pair<uint64_t, uint64_t>> tables;
        if (tags[8]) {
            if (!tags[7] || tags[9] != 24) return fail(-ENOEXEC, "invalid DT_RELA");
            tables.push_back({tags[7], tags[8]});
        } else if (tags[7]) return fail(-ENOEXEC, "RELA without size");
        if (tags[2]) {
            if (!tags[23] || tags[20] != 7) return fail(-ENOTSUP, "PLT requires RELA");
            tables.push_back({tags[23], tags[2]});
        } else if (tags[23]) return fail(-ENOEXEC, "JMPREL without size");
        if (tables.empty()) return fail(-ENOEXEC, "no relocations");
        struct Write { uint64_t address, value; size_t width; };
        std::vector<Write> writes;
        for (const auto& t : tables) {
            if (!t.second || t.second % 24 || t.second > 4096 * 24)
                return fail(-ENOEXEC, "invalid relocation size");
            const auto* rel = parsed.translate(t.first, t.second, 4);
            if (!rel) return fail(-ENOEXEC, "relocations outside memory");
            for (size_t at = 0; at < t.second; at += 24) {
                const uint64_t target = u(rel+at), info = u(rel+at+8), a = u(rel+at+16);
                const uint32_t type = static_cast<uint32_t>(info);
                const uint64_t index = info >> 32;
                if (type == 0) {
                    if (index || a) return fail(-ENOEXEC, "malformed R_NONE");
                    continue;
                }
                if (type != 1 && type != 2 && type != 6 && type != 7 && type != 8 && type != 10 && type != 11)
                    return fail(-ENOTSUP, "unsupported x86-64 relocation");
                if ((type == 6 || type == 7) && a) return fail(-ENOTSUP, "GOT relocation with addend");
                const size_t width = type == 2 || type == 10 || type == 11 ? 4 : 8;
                if (target % width || !parsed.translate(target, width, 2) || parsed.translate(target, width, 1))
                    return fail(-ENOEXEC, "relocation target must be aligned RW/non-X");
                for (const auto& w : writes)
                    if (target < w.address + w.width && w.address < target + width)
                        return fail(-ENOEXEC, "overlapping relocation targets");
                uint64_t value = 0;
                if (type == 8) {
                    // Fixed logical-address model: load bias is zero. Negative
                    // addends are not valid guest pointers in this model.
                    if ((a >> 63) || !parsed.translate(a, 1, 4)) // symbol index ignored
                        return fail(-ENOEXEC, "invalid R_RELATIVE");
                    value = a;
                } else {
                    if (!index || index >= symbols) return fail(-ENOEXEC, "invalid relocation symbol index");
                    const auto* sym = symtab + index * 24;
                    const uint64_t section = u(sym+6,2), size = u(sym+16);
                    std::string symbol;
                    if (!name(u(sym,4), symbol) || (sym[5] & 3) || (sym[4] >> 4) != 1)
                        return fail(-ENOTSUP, "unsupported symbol binding/visibility");
                    if (!section) {
                        if ((sym[4] & 15) != 2 || u(sym+8) || size || (type != 6 && type != 7) || a)
                            return fail(-ENOTSUP, "unsupported imported-symbol relocation");
                        const auto service = resolve_libkernel(symbol, "libkernel", "libkernel", 1);
                        if (service == PS4KernelService::unresolved || general_only(service)) {
                            diagnostic_ = "unknown essential import: " + symbol;
                            imports_.clear(); stack_ = 0; return -ENOENT;
                        }
                        auto found = by_symbol.find(index);
                        if (found == by_symbol.end()) {
                            if (by_symbol.size() >= 128) return fail(-E2BIG, "too many imports");
                            const Import binding{target, thunk_base + by_symbol.size() * thunk_size, service};
                            found = by_symbol.emplace(index, binding).first;
                        }
                        Import binding = found->second; binding.slot = target;
                        imports_.push_back(binding); value = binding.thunk;
                    } else {
                        if ((sym[4] & 15) > 2) return fail(-ENOTSUP, "unsupported defined symbol type (including TLS)");
                        if (section >= 0xff00 || !parsed.translate(u(sym+8), size ? size : 1, 4) ||
                            !add(u(sym+8), a, value)) return fail(-EOVERFLOW, "invalid defined symbol/addend");
                        if (!parsed.translate(value,1,4)) return fail(-ENOEXEC, "symbol addend outside guest memory");
                    }
                }
                if (type == 2) {
                    uint32_t displacement = 0;
                    if (!relative(value, target, displacement)) return fail(-EOVERFLOW, "PC32 overflow");
                    value = displacement;
                } else if (type == 10 && value > UINT32_MAX) return fail(-EOVERFLOW, "R32 overflow");
                else if (type == 11 && signed32(static_cast<uint32_t>(value)) != value)
                    return fail(-EOVERFLOW, "R32S overflow");
                writes.push_back({target,value,width});
            }
        }
        if (imports_.empty()) return fail(-ENOEXEC, "demo requires a real libkernel import");
        // Relocation tables were completely verified before any patch.
        for (const auto& w : writes) put(const_cast<uint8_t*>(parsed.translate(w.address,w.width,2)),w.value,w.width);
        PS4ExecutableImage::Segment thunks{{1,5,0,thunk_base,by_symbol.size()*thunk_size,0x4000,0x4000},
            std::vector<uint8_t>(0x4000)};
        for (const auto& pair : by_symbol) {
            const auto& binding = pair.second;
            auto* code = thunks.bytes.data() + binding.thunk - thunk_base;
            // SysV fourth argument RCX -> syscall-safe R10; opaque service
            // selector -> EAX. No ARM64 or host function address is installed.
            const uint8_t body[] = {0x49,0x89,0xca,0xb8,0,0,0,0,0x0f,0x05,0xc3};
            std::memcpy(code,body,sizeof(body));
            put(code+4,static_cast<uint32_t>(binding.service),4);
        }
        // Prove a finite, straight-line entry program. Rewrite only known GOT
        // CALLs to same-length direct calls; this preserves logical addresses
        // under the bounded FEX rebasing model. Generic loads/branches are refused.
        PS4ExecutableImage::Segment* code_segment = nullptr;
        for (auto& s : parsed.segments)
            if ((s.header.flags & 1) && parsed.entry >= s.header.address &&
                parsed.entry - s.header.address < s.header.fileSize) code_segment = &s;
        if (!code_segment) return fail(-ENOTSUP, "PRX has no executable demo entry");
        auto* code = code_segment->bytes.data() + parsed.entry - code_segment->header.address;
        const size_t code_size = code_segment->header.fileSize - (parsed.entry - code_segment->header.address);
        if (code_size > 65536) return fail(-E2BIG, "entry program too large");
        size_t offset = 0; uint32_t instructions = 0, initialized = 0;
        bool halted = false, stack_arg = false;
        while (offset < code_size) {
            const size_t left = code_size - offset;
            auto* p = code + offset; size_t width = 0;
            if (++instructions > budget) return fail(-E2BIG, "instruction budget exhausted");
            if (p[0] >= 0xb8 && p[0] <= 0xbf && p[0] != 0xbc && left >= 5) {
                width = 5; initialized |= 1U << (p[0]-0xb8);
            }
            else if ((p[0] == 0x48 || p[0] == 0x49) && left >= 10 && p[1] >= 0xb8 && p[1] <= 0xbf &&
                     !(p[0] == 0x48 && p[1] == 0xbc)) {
                width = 10; initialized |= 1U << (p[1]-0xb8+(p[0] == 0x49 ? 8 : 0));
            }
            else if (p[0] == 0x41 && left >= 6 && (p[1] == 0xb8 || p[1] == 0xb9)) {
                width = 6; initialized |= 1U << (p[1]-0xb8+8);
            }
            else if (p[0] == 0x05 && left >= 5 && (initialized & 1)) width = 5;
            else if (p[0] == 0x90) width = 1;
            else if (left >= 3 && p[0] == 0x48 && p[1] == 0x89 &&
                     (p[2] == 0xc7 || p[2] == 0xc6) && (initialized & 1)) width = 3;
            else if (left >= 4 && p[0] == 0x48 && p[1] == 0x83 && p[3] == 0x10 &&
                     ((p[2] == 0xec && !stack_arg) || (p[2] == 0xc4 && stack_arg))) {
                stack_arg = p[2] == 0xec; width = 4;
            } else if (left >= 8 && stack_arg && !std::memcmp(p,"\x48\xc7\x04\x24",4)) width = 8;
            else if (left >= 6 && p[0] == 0xff && p[1] == 0x15) {
                uint64_t slot = 0;
                if (!add(parsed.entry + offset + 6,signed32(u(p+2,4)),slot)) return fail(-EOVERFLOW,"CALL slot overflow");
                auto found = std::find_if(imports_.begin(),imports_.end(),[&](const Import& i){return i.slot == slot;});
                if (found == imports_.end()) return fail(-ENOENT,"CALL to unresolved GOT slot");
                const auto service = found->service;
                if ((service == PS4KernelService::mmap || service == PS4KernelService::pread ||
                     service == PS4KernelService::pwrite || service == PS4KernelService::getdirentries) &&
                    !(initialized & (1U<<1)))
                    return fail(-ENOTSUP,"service requires an explicit fourth SysV argument");
                // Four generated instructions plus the callee RET count too.
                if (budget - instructions < 4) return fail(-E2BIG,"trampoline budget exhausted");
                instructions += 4;
                uint32_t displacement = 0;
                if (!relative(found->thunk,parsed.entry+offset+5,displacement)) return fail(-EOVERFLOW,"CALL displacement overflow");
                p[0] = 0xe8; put(p+1,displacement,4); p[5] = 0x90; width = 6;
                initialized |= 1; // A verified service returns a logical value in RAX.
                initialized &= ~(1U<<1); // SYSCALL RCX contains a private CPU return address.
                if (++instructions > budget) return fail(-E2BIG,"CALL padding budget exhausted");
            } else if ((left >= 7 && (p[0] == 0x48 || p[0] == 0x4c) && p[1] == 0x89 && (p[2] & 0xc7) == 5) ||
                       (left >= 10 && p[0] == 0xc7 && p[1] == 0x05)) {
                const bool qword = p[0] != 0xc7; width = qword ? 7 : 10;
                if (qword) {
                    const uint32_t source = ((p[2] >> 3) & 7) + (p[0] == 0x4c ? 8 : 0);
                    constexpr uint32_t permitted = (1U<<0)|(1U<<3)|(1U<<5)|(1U<<12)|(1U<<13)|(1U<<14)|(1U<<15);
                    if (!(permitted & (1U<<source)) || !(initialized & (1U<<source)))
                        return fail(-ENOTSUP,"store could disclose private CPU pointers");
                }
                uint64_t target = 0;
                if (!add(parsed.entry+offset+width,signed32(u(p+(qword ? 3 : 2),4)),target) ||
                    !parsed.translate(target,qword ? 8 : 4,2) || parsed.translate(target,qword ? 8 : 4,1))
                    return fail(-EFAULT,"store outside RW guest memory");
                // Code cannot overwrite import capabilities after admission.
                for (const auto& i : imports_)
                    if (target < i.slot+8 && i.slot < target+(qword ? 8 : 4)) return fail(-ENOTSUP,"store overlaps GOT");
            } else if (p[0] == 0xf4 && left == 1 && !stack_arg) { width = 1; halted = true; }
            else return fail(-ENOTSUP,"instruction outside bounded import-call envelope");
            offset += width;
        }
        if (!halted) return fail(-ENOTSUP,"missing terminal HLT or unbalanced stack");
        parsed.segments.push_back(std::move(thunks));
        parsed.segments.push_back({{1,6,0,stack_,0,stack_size,0x4000},std::vector<uint8_t>(stack_size)});
        std::vector<GuestELF::Segment> regions;
        std::vector<const uint8_t*> sources;
        for (const auto& s : parsed.segments) {
            regions.push_back({s.header.address,s.header.offset,s.header.fileSize,s.header.memorySize,s.header.flags});
            sources.push_back(s.bytes.data());
        }
        error = image.map_segments(std::move(regions),sources,parsed.entry);
        if (error) return fail(error,"guest mapping failed");
        // Still not a general PS4 address space or commercial-game admission.
        image.blockers = 4;
        return 0;
    } catch (const std::bad_alloc&) { return fail(-ENOMEM,"linker allocation failed"); }
}
bool PS4DynamicLinker::gate(const GuestELF& image, uint64_t cpu_rip, uint32_t token,
                            PS4KernelService& service) const {
    for (const auto& i : imports_) {
        const auto* p = image.translate(i.thunk+8,2,5);
        if (p && cpu_rip == reinterpret_cast<uintptr_t>(p) && token == static_cast<uint32_t>(i.service)) {
            service = i.service; return true;
        }
    }
    return false;
}
}
