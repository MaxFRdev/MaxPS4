#pragma once
// SELF container -> plain ELF, as shadPS4 core/loader/elf.cpp reads it
// (GPL-2.0-or-later): the ELF header follows the SELF header and segment
// table; a blocked SELF segment (flag 0x800) with ID n (bits 20..31) holds
// the file bytes of ELF program header n, and an ELF file offset is read from
// the blocked segment whose program header contains it. Here those bytes are
// laid out at their ELF offsets, which gives the same reads.
// Only plaintext blocks: homebrew fake-signed SELF (fSELF) and dumps already
// decrypted on the console. Encrypted (0x2) or deflated (0x8) blocks are
// refused; MaxPS4 never decrypts or decompresses them.
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace maxps4 {
struct SelfImage {
    static constexpr uint32_t magic = 0x1d3d154f;
    // The largest plain image (OrbisMetadata::fileLimit): Unreal Engine
    // executables are over 128 MiB.
    static constexpr uint64_t limit = uint64_t(512) << 20;
    static uint64_t read(const uint8_t* p, size_t width) {
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value |= uint64_t(p[i]) << (8 * i);
        return value;
    }
    static bool range(uint64_t offset, uint64_t count, uint64_t size) {
        return offset <= size && count <= size - offset;
    }
    static bool isSelf(const uint8_t* file, size_t size) {
        return file && size >= 4 && read(file, 4) == magic;
    }
    // A SELF with at least one encrypted program block (what a console
    // installs). Only its headers are read.
    static bool encrypted(const uint8_t* file, size_t size) {
        if (!isSelf(file, size) || size < 32) return false;
        const uint64_t count = read(file + 24, 2);
        for (uint64_t i = 0; i < count && range(32 + i * 32, 32, size); ++i) {
            const uint64_t flags = read(file + 32 + i * 32, 8);
            if ((flags & 0x800) && (flags & 0x2)) return true;
        }
        return false;
    }
    // 0: not a SELF (`elf` untouched, use the bytes as they are). 1: `elf`
    // holds the plain ELF. Negative errno with `error` set otherwise.
    static int unwrap(const uint8_t* file, size_t size, std::vector<uint8_t>& elf, std::string& error) {
        if (!isSelf(file, size)) return 0;
        auto fail = [&](int code, const char* message) { error = message; elf.clear(); return code; };
        // shadPS4 IsSelfFile: version 0, mode 1, little-endian, attributes 0x12.
        if (size < 32 || file[4] != 0 || file[5] != 1 || file[6] != 1 || file[7] != 0x12)
            return fail(-ENOEXEC, "en-tête SELF invalide");
        const uint64_t count = read(file + 24, 2);
        if (!count || count > 128 || !range(32, count * 32, size)) return fail(-ENOEXEC, "table des segments SELF invalide");
        const uint64_t origin = 32 + count * 32;
        if (!range(origin, 64, size) || std::memcmp(file + origin, "\177ELF\2\1\1", 7))
            return fail(-ENOEXEC, "ELF absent du SELF");
        const uint8_t* header = file + origin;
        const uint64_t phoff = read(header + 32, 8), phnum = read(header + 56, 2);
        if (phoff < 64 || !phnum || phnum > 128 || read(header + 54, 2) != 56 ||
            !range(origin + phoff, phnum * 56, size)) return fail(-ENOEXEC, "table des segments ELF invalide");
        struct Block { uint64_t offset, size; bool present; };
        std::vector<Block> blocks(phnum, Block{0, 0, false});
        for (uint64_t i = 0; i < count; ++i) {
            const uint8_t* entry = file + 32 + i * 32;
            const uint64_t flags = read(entry, 8), offset = read(entry + 8, 8);
            const uint64_t stored = read(entry + 16, 8), decoded = read(entry + 24, 8);
            if (!(flags & 0x800)) continue; // Signature and digest metadata.
            if (flags & 0x2) return fail(-ENOTSUP, "SELF chiffré : MaxPS4 ne déchiffre pas");
            if (flags & 0x8) return fail(-ENOTSUP, "SELF compressé : non pris en charge");
            const uint64_t id = (flags >> 20) & 0xfff;
            if (id >= phnum || blocks[id].present || stored != decoded || !range(offset, stored, size))
                return fail(-ENOEXEC, "bloc SELF invalide");
            blocks[id] = {offset, stored, true};
        }
        // Plain image: ELF header and program headers, then every program
        // header's bytes at its ELF offset. Sections are not in a SELF.
        uint64_t length = phoff + phnum * 56;
        for (uint64_t i = 0; i < phnum; ++i) {
            const uint8_t* p = header + phoff + i * 56;
            const uint64_t offset = read(p + 8, 8), filesz = read(p + 32, 8);
            if (!filesz) continue;
            if (offset > limit || filesz > limit)
                return fail(-E2BIG, "segment ELF trop grand");
            if (blocks[i].present && blocks[i].size < filesz) return fail(-ENOEXEC, "bloc SELF trop court");
            if (offset + filesz > length) length = offset + filesz;
        }
        elf.assign(size_t(length), 0);
        std::memcpy(elf.data(), header, size_t(phoff + phnum * 56));
        for (size_t i = 58; i < 64; ++i) elf[i] = 0;          // e_shentsize/e_shnum/e_shstrndx
        std::memset(elf.data() + 40, 0, 8);                   // e_shoff
        for (uint64_t i = 0; i < phnum; ++i) {
            const uint8_t* p = header + phoff + i * 56;
            const uint64_t offset = read(p + 8, 8), filesz = read(p + 32, 8);
            if (filesz && blocks[i].present) std::memcpy(elf.data() + offset, file + blocks[i].offset, size_t(filesz));
        }
        // A program header without its own block must lie inside one that has
        // one (PT_DYNAMIC or PT_TLS in the data segment, for example).
        for (uint64_t i = 0; i < phnum; ++i) {
            const uint8_t* p = header + phoff + i * 56;
            const uint64_t offset = read(p + 8, 8), filesz = read(p + 32, 8);
            if (!filesz || blocks[i].present) continue;
            bool covered = false;
            for (uint64_t j = 0; j < phnum && !covered; ++j) {
                if (!blocks[j].present) continue;
                const uint8_t* q = header + phoff + j * 56;
                const uint64_t start = read(q + 8, 8), bytes = read(q + 32, 8);
                covered = offset >= start && offset - start <= bytes && filesz <= bytes - (offset - start);
            }
            if (!covered) {
                // Metadata kept outside the blocks (comment, library version):
                // dropped from the plain image rather than guessed.
                const uint32_t type = uint32_t(read(p, 4));
                if (type != 0x6fffff00 && type != 0x6fffff01) return fail(-ENOEXEC, "segment ELF sans bloc SELF");
                std::memset(elf.data() + phoff + i * 56 + 8, 0, 8);
                std::memset(elf.data() + phoff + i * 56 + 32, 0, 8);
            }
        }
        error.clear();
        return 1;
    }
};
}
