#pragma once
// PS4 ELF/SELF layouts follow shadPS4 elf.h/elf.cpp at
// 443f022450f6857256e780daa3d5a14029a748f7 (GPL-2.0-or-later).
// Plain file-backed segment staging only: no decryption, relocations, HLE,
// executable host memory or integration with the restricted FEX loader.
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cstring>
#include <cerrno>
#include <new>
#include <utility>

namespace maxps4 {
class PS4ExecutableImage {
public:
    struct Header {
        uint32_t type, flags;
        uint64_t offset, address, fileSize, memorySize, alignment;
    };
    struct Segment { Header header; std::vector<uint8_t> bytes; };
    static constexpr size_t memoryBudget = 512 * 1024 * 1024;
    uint64_t entry = 0;
    uint16_t type = 0;
    bool isSelf = false, requiresLinker = false, requiresTLS = false;
    std::vector<Header> headers; // Retain metadata; staging never applies it.
    std::vector<Segment> segments;

    const uint8_t* translate(uint64_t address, size_t count, uint32_t permissions) const {
        if (!count || (permissions & ~7U)) return nullptr;
        for (const auto& s : segments) {
            if (address < s.header.address || (s.header.flags & permissions) != permissions) continue;
            const uint64_t offset = address - s.header.address;
            if (offset <= s.bytes.size() && count <= s.bytes.size() - offset)
                return s.bytes.data() + offset;
        }
        return nullptr;
    }
    int load(const uint8_t* file, size_t size) {
        *this = PS4ExecutableImage{};
        if (!file || size < 64 || size > 512 * 1024 * 1024) return -EINVAL;
        try {
            PS4ExecutableImage pending;
            const int error = pending.parse(file, size);
            if (error) return error;
            *this = std::move(pending); // No partial image escapes a rejected file.
            return 0;
        } catch (const std::bad_alloc&) { return -ENOMEM; }
    }
private:
    static uint64_t read(const uint8_t* p, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value |= uint64_t(p[i]) << (8 * i);
        return value;
    }
    static bool range(uint64_t offset, uint64_t count, size_t size) {
        return offset <= size && count <= size - offset;
    }
    struct SelfSegment { uint64_t flags, offset, size, decodedSize; };
    int parse(const uint8_t* file, size_t size) {
        size_t origin = 0, payloadStart = 0;
        std::vector<SelfSegment> selfSegments;
        isSelf = read(file, 4) == 0x1d3d154f;
        if (isSelf) {
            if (file[4] != 0 || file[5] != 1 || file[6] != 1 || file[7] != 0x12 ||
                file[8] != 1 || file[9] != 1) return -ENOEXEC;
            const size_t count = read(file + 24, 2);
            if (!count || count > 128 || count > (size - 32) / 32) return -ENOEXEC;
            origin = 32 + count * 32;
            if (!range(origin, 64, size)) return -ENOEXEC;
            payloadStart = read(file + 12, 2) + read(file + 14, 2);
            if (payloadStart < origin + 64 || payloadStart > size || read(file + 16, 4) != size)
                return -ENOEXEC;
            for (size_t i = 0; i < count; ++i) {
                const auto* p = file + 32 + i * 32;
                SelfSegment s {read(p, 8), read(p + 8, 8), read(p + 16, 8), read(p + 24, 8)};
                // This narrow loader accepts only plaintext, uncompressed blocks.
                if (s.flags & 0xeU) return -ENOTSUP;
                if (s.flags & ~(uint64_t{0xfff} << 20 | uint64_t{0x801})) return -ENOTSUP;
                if (!range(s.offset, s.size, size) || (s.size && s.offset < payloadStart))
                    return -ENOEXEC;
                selfSegments.push_back(s);
            }
        }
        const auto* elf = file + origin;
        if (std::memcmp(elf, "\177ELF\2\1\1\11\0", 9) ||
            read(elf + 18, 2) != 62 || read(elf + 20, 4) != 1 ||
            read(elf + 52, 2) != 64 || read(elf + 54, 2) != 56) return -ENOEXEC;
        for (size_t i = 9; i < 16; ++i) if (elf[i]) return -ENOEXEC;
        if (read(elf + 48, 4)) return -ENOEXEC; // No x86-64 processor flags supported.
        type = static_cast<uint16_t>(read(elf + 16, 2));
        if (type != 0xfe00 && type != 0xfe10 && type != 0xfe18) return -ENOEXEC;
        entry = read(elf + 24, 8);
        const uint64_t table = read(elf + 32, 8), count = read(elf + 56, 2);
        if (table < 64 || !count || count > 128 ||
            !range(table, count * 56, size - origin)) return -ENOEXEC;
        if (isSelf && origin + table + count * 56 > payloadStart) return -ENOEXEC;
        const uint64_t shoff = read(elf + 40, 8), shnum = read(elf + 60, 2);
        if (shnum && (read(elf + 58, 2) != 64 || shnum > 4096 ||
            !range(shoff, shnum * 64, size - origin))) return -ENOEXEC;
        const uint64_t shstr = read(elf + 62, 2);
        if ((!shnum && (shoff || (read(elf + 58, 2) != 0 && read(elf + 58, 2) != 64) || shstr)) ||
            (shnum && (shoff < 64 || shstr >= shnum))) return -ENOEXEC;
        for (const auto& s : selfSegments)
            if ((s.flags & 0x800) && ((s.flags >> 20) & 0xfff) >= count) return -ENOEXEC;
        size_t allocated = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto* p = elf + table + i * 56;
            Header h {static_cast<uint32_t>(read(p, 4)), static_cast<uint32_t>(read(p + 4, 4)),
                read(p + 8, 8), read(p + 16, 8), read(p + 32, 8), read(p + 40, 8), read(p + 48, 8)};
            headers.push_back(h);
            requiresLinker |= h.type == 2 || h.type == 0x60000000 || h.type == 0x61000000;
            // An empty PT_TLS (some games keep the header) declares no TLS.
            requiresTLS |= h.type == 7 && (h.memorySize || h.fileSize);
            if (!isSelf && h.fileSize && !range(h.offset, h.fileSize, size)) return -ENOEXEC;
            if (h.type != 1 && h.type != 0x61000010) continue; // Metadata retained, not executed.
            if (!h.memorySize || h.fileSize > h.memorySize || h.memorySize > memoryBudget - allocated ||
                h.address > UINT64_MAX - h.memorySize || (h.flags & ~7U) || !(h.flags & 4U) ||
                (h.alignment > 1 && ((h.alignment & (h.alignment - 1)) ||
                    h.address % h.alignment != h.offset % h.alignment))) return -ENOEXEC;
            for (const auto& old : segments)
                if (h.address < old.header.address + old.header.memorySize &&
                    old.header.address < h.address + h.memorySize) return -ENOEXEC;
            uint64_t source = h.offset;
            if (isSelf && h.fileSize) {
                const SelfSegment* match = nullptr;
                for (const auto& s : selfSegments)
                    if ((s.flags & 0x800) && ((s.flags >> 20) & 0xfff) == i) {
                        if (match) return -ENOEXEC;
                        match = &s;
                    }
                if (!match || match->size != h.fileSize || match->decodedSize != h.fileSize)
                    return -ENOEXEC;
                source = match->offset; // SELF block ID identifies the ELF program header.
            }
            if (h.fileSize && !range(source, h.fileSize, size)) return -ENOEXEC;
            Segment segment {h, std::vector<uint8_t>(static_cast<size_t>(h.memorySize), 0)};
            if (h.fileSize) std::memcpy(segment.bytes.data(), file + source, static_cast<size_t>(h.fileSize));
            allocated += static_cast<size_t>(h.memorySize);
            segments.push_back(std::move(segment));
        }
        if (segments.empty()) return -ENOEXEC;
        // Shared libraries may have no entry. Executables require file-backed code.
        if (type != 0xfe18) {
            bool executableEntry = false;
            for (const auto& s : segments)
                executableEntry |= (s.header.flags & 1) && entry >= s.header.address &&
                    entry - s.header.address < s.header.fileSize;
            if (!executableEntry) return -ENOEXEC;
        }
        requiresLinker |= type != 0xfe00;
        return 0;
    }
};
}
