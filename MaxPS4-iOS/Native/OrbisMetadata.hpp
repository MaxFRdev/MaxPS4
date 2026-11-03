#pragma once
// Plain ELF64 Orbis metadata, independent of the restricted executable FEX path.
// SCE tag/layout constants follow shadPS4 elf.h (see PS4ExecutableImage.hpp).
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include "SelfImage.hpp"

namespace maxps4 {
class OrbisMetadata {
public:
    // Files and memory plans up to 512 MiB; tables of sections and dynamic
    // entries up to recordLimit, symbols up to symbolLimit, relocations up to
    // relocationLimit (Unreal Engine executables have close to a million).
    static constexpr size_t fileLimit = 512 * 1024 * 1024;
    static constexpr size_t recordLimit = 4096, symbolLimit = 65536, relocationLimit = 4 * 1024 * 1024;
    struct Segment { uint32_t type, flags; uint64_t offset, address, filesz, memsz, align; };
    struct Identity { uint16_t id, version; std::string name; bool exported; };
    struct Symbol {
        std::string encoded, nid, library, module;
        uint16_t libraryVersion = 0, moduleVersion = 0;
        uint64_t value = 0, size = 0;
        uint8_t binding = 0, kind = 0, visibility = 0;
        bool defined = false, qualified = false, resolved = false;
    };
    struct Relocation { uint64_t target, symbol; int64_t addend; uint32_t type; bool supported; };
    using Resolver = std::function<bool(const Symbol&)>;
    uint16_t type = 0;
    uint64_t entry = 0, init = 0, fini = 0;
    std::vector<Segment> segments;
    std::vector<Identity> libraries, modules;
    std::vector<std::string> needed, blockers;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocations;
    std::string error;
    bool sce = false;
    bool container = false; // Read from a SELF with plaintext blocks.

    bool parse(const uint8_t* bytes, size_t size, Resolver resolver = {}) {
        *this = OrbisMetadata{};
        OrbisMetadata pending;
        try {
            pending.decode(bytes, size, resolver);
            *this = std::move(pending);
            return true;
        } catch (const std::exception& e) {
            // Keep what was decoded before the refusal: the report shows it.
            error = e.what();
            type = pending.type; entry = pending.entry; container = pending.container;
            segments = std::move(pending.segments);
            return false;
        }
    }
    static bool range(uint64_t offset, uint64_t count, uint64_t size) {
        return offset <= size && count <= size - offset;
    }
    static uint64_t read(const uint8_t* p, size_t width) {
        uint64_t n = 0;
        for (size_t i = 0; i < width; ++i) n |= uint64_t(p[i]) << (8*i);
        return n;
    }
    static const char* typeName(uint16_t t) {
        switch (t) {
        case 2: return "ET_EXEC"; case 3: return "ET_DYN";
        case 0xfe00: return "ET_SCE_EXEC"; case 0xfe04: return "ET_SCE_REPLAY_EXEC";
        case 0xfe0c: return "ET_SCE_STUBLIB"; case 0xfe10: return "ET_SCE_DYNEXEC";
        case 0xfe18: return "ET_SCE_DYNAMIC / PRX"; default: return "inconnu";
        }
    }
    static const char* segmentName(uint32_t type) {
        switch (type) {
        case 0: return "PT_NULL"; case 1: return "PT_LOAD"; case 2: return "PT_DYNAMIC";
        case 3: return "PT_INTERP"; case 4: return "PT_NOTE"; case 6: return "PT_PHDR";
        case 7: return "PT_TLS"; case 0x60000000: return "PT_SCE_RELA (non pris en charge)";
        case 0x61000000: return "PT_SCE_DYNLIBDATA"; case 0x61000001: return "PT_SCE_PROCPARAM";
        case 0x61000002: return "PT_SCE_MODULE_PARAM"; case 0x61000010: return "PT_SCE_RELRO";
        case 0x6474e550: return "PT_GNU_EH_FRAME"; case 0x6474e551: return "PT_GNU_STACK";
        case 0x6474e552: return "PT_GNU_RELRO"; case 0x6fffff00: return "PT_SCE_COMMENT";
        case 0x6fffff01: return "PT_SCE_LIBVERSION"; default: return "inconnu";
        }
    }
    std::string report() const {
        std::ostringstream out;
        out << "Analyse Orbis — lecture seule, aucune exécution\n";
        if (!error.empty()) {
            out << "Rejet : " << error << '\n';
            if (container) out << "Conteneur : SELF à blocs en clair (fSELF ou dump déchiffré), lu comme son ELF\n";
            if (type) out << "Type : " << typeName(type) << " (0x" << std::hex << type << std::dec << ")\n";
            for (const auto& s : segments)
                out << "Segment " << segmentName(s.type) << " type=0x" << std::hex << s.type << " adresse=0x" << s.address
                    << " flags=" << s.flags << std::dec << " fichier=" << s.offset << "+" << s.filesz
                    << " mémoire=" << s.memsz << " align=" << s.align << '\n';
            return out.str();
        }
        if (container) out << "Conteneur : SELF à blocs en clair (fSELF ou dump déchiffré), lu comme son ELF\n";
        out << "Type : " << typeName(type) << " (0x" << std::hex << type << ")\n"
            << "Architecture : x86-64 ; entrée : 0x" << entry << std::dec << '\n';
        for (const auto& s : segments) {
            out << "Segment " << segmentName(s.type) << " type=0x" << std::hex << s.type << " adresse=0x" << s.address
                << " flags=" << s.flags << std::dec << " fichier=" << s.offset << "+" << s.filesz
                << " mémoire=" << s.memsz << " align=" << s.align << '\n';
            if (s.type == 7) out << "TLS : initial=" << s.filesz << " total=" << s.memsz
                << " align=" << s.align << " ; allocation par thread non implémentée\n";
        }
        if (std::none_of(segments.begin(),segments.end(),[](const Segment& s){ return s.type == 7; })) out << "TLS : aucun PT_TLS déclaré\n";
        for (const auto& n : needed) out << "DT_NEEDED : " << n << '\n';
        for (const auto& m : modules) out << (m.exported ? "Module fourni : " : "Module requis : ")
            << m.name << " version=" << (m.version>>8) << "." << (m.version & 255) << " id=" << m.id << '\n';
        for (const auto& l : libraries) out << (l.exported ? "Bibliothèque exportée : " : "Bibliothèque requise : ")
            << l.name << " version=" << l.version << " id=" << l.id << '\n';
        size_t imports = 0, resolved = 0;
        for (size_t i = 1; i < symbols.size(); ++i) {
            const auto& s = symbols[i];
            if (s.encoded.empty()) continue;
            if (!s.defined) { ++imports; resolved += s.resolved; }
            out << (s.defined ? "Export " : "Import ") << s.encoded << " ; NID=" << s.nid
                << " ; bibliothèque=" << s.library << " v" << s.libraryVersion
                << " ; module=" << s.module << " v" << (s.moduleVersion>>8) << "." << (s.moduleVersion & 255)
                << (s.defined ? " ; déclaré" : s.resolved ? " ; résolu HLE (analyse)" : " ; NON RÉSOLU") << '\n';
        }
        out << "Imports : " << imports << " ; résolus : " << resolved << " ; non résolus : " << imports-resolved << '\n';
        for (const auto& r : relocations) out << "RELA type=" << r.type << " cible=0x" << std::hex
            << r.target << std::dec << " symbole=" << r.symbol << " addend=" << r.addend
            << (r.supported ? " ; prise en charge par préparation PRX" : " ; REFUSÉE") << '\n';
        out << "Initialisation déclarée=0x" << std::hex << init << " ; finalisation=0x" << fini << std::dec
            << " (non appelées)\nObstacles empêchant l'exécution :\n";
        for (const auto& b : blockers) out << "• " << b << '\n';
        out << "• Analyse seule ; aucun dispatch FEX, aucune allocation invitée RX/RW\n";
        return out.str();
    }
private:
    static void require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    void block(const std::string& message) {
        if (std::find(blockers.begin(), blockers.end(), message) == blockers.end()) blockers.push_back(message);
    }
    static uint16_t id(const std::string& encoded) {
        const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
        require(!encoded.empty() && encoded.size() <= 3, "Identifiant NID encodé invalide");
        uint32_t n = 0;
        for (char c : encoded) {
            auto p = alphabet.find(c);
            require(p != std::string::npos, "Alphabet NID invalide");
            n = n*64 + uint32_t(p);
        }
        require(n <= UINT16_MAX, "Identifiant NID trop grand");
        return uint16_t(n);
    }
    void decode(const uint8_t* f, size_t size, const Resolver& resolver) {
        require(f && size >= 64 && size <= fileLimit, "Taille ELF invalide (limite 512 Mio)");
        // A SELF with plaintext blocks (homebrew fSELF, dump decrypted on the
        // console) is read as its plain ELF; an encrypted one is refused.
        std::vector<uint8_t> plain;
        std::string why;
        const int unwrapped = SelfImage::unwrap(f, size, plain, why);
        if (unwrapped < 0) throw std::runtime_error(why);
        if (unwrapped) {
            container = true;
            f = plain.data();
            size = plain.size();
            require(size >= 64 && size <= fileLimit, "Taille ELF invalide (limite 512 Mio)");
        }
        require(!std::memcmp(f,"\177ELF\2\1\1",7), "ELF64 little-endian requis");
        require((f[7] == 0 || f[7] == 9) && f[8] == 0, "OSABI/version ABI non pris en charge");
        require(read(f+18,2) == 62 && read(f+20,4) == 1, "Architecture/version ELF non prise en charge");
        require(read(f+52,2) == 64 && read(f+54,2) == 56, "Tailles d'en-tête ELF invalides");
        type = uint16_t(read(f+16,2));
        require(type == 2 || type == 3 || type == 0xfe00 || type == 0xfe04 || type == 0xfe0c ||
            type == 0xfe10 || type == 0xfe18, "Type ELF non pris en charge");
        if (type == 0xfe04 || type == 0xfe0c) block("Replay/stub : analyse seulement, chargement refusé");
        entry = read(f+24,8);
        uint64_t table = read(f+32,8), count = read(f+56,2);
        require(table >= 64 && count && count <= 128 && range(table,count*56,size), "Table de segments invalide");
        uint64_t sh = read(f+40,8), sc = read(f+60,2);
        require(sc != 0xffff && read(f+62,2) != 0xffff, "Index ELF étendus non pris en charge");
        if (sc) require(sh >= 64 && read(f+58,2) == 64 && sc <= recordLimit && range(sh,sc*64,size), "Table de sections invalide");
        const Segment* dynamic = nullptr;
        const Segment* dynlib = nullptr;
        uint64_t planned = 0;
        bool tls = false;
        segments.reserve(size_t(count));
        for (uint64_t i = 0; i < count; ++i) {
            const auto* p = f + table + i*56;
            Segment s{uint32_t(read(p,4)),uint32_t(read(p+4,4)),read(p+8,8),read(p+16,8),
                read(p+32,8),read(p+40,8),read(p+48,8)};
            require(range(s.offset,s.filesz,size), "Segment hors limites du fichier");
            require(s.address <= UINT64_MAX-s.memsz, "Débordement d'adresse de segment");
            require(s.align <= 1 || !(s.align & (s.align-1)), "Alignement de segment non puissance de deux");
            if (s.type == 1 || s.type == 7 || s.type == 0x61000010) {
                require(s.address < (uint64_t(1)<<47) && s.memsz <= (uint64_t(1)<<47)-s.address, "Adresse hors espace utilisateur x86-64 inférieur");
                require(s.filesz <= s.memsz && !(s.flags & ~7U), "Taille/protection de segment invalide");
                require(s.align <= 1 || s.address%s.align == s.offset%s.align, "Adresse/offset non congruents");
            }
            if (s.type == 1 || s.type == 0x61000010) {
                require(s.memsz <= fileLimit-planned, "Plan mémoire supérieur à 512 Mio");
                planned += s.memsz;
                for (const auto& old : segments) if (old.type == 1 || old.type == 0x61000010)
                    require(!s.memsz || !old.memsz || s.address >= old.address+old.memsz ||
                        old.address >= s.address+s.memsz, "Segments chargeables se chevauchant");
                if ((s.flags & 3) == 3) block("Segment W+X refusé par la préparation PRX");
            }
            segments.push_back(s);
            if (s.type == 2) { require(!dynamic, "Plusieurs PT_DYNAMIC"); dynamic = &segments.back(); }
            if (s.type == 0x61000000) { require(!dynlib, "Plusieurs PT_SCE_DYNLIBDATA"); dynlib = &segments.back(); }
            if (s.type == 7) { require(!tls, "Plusieurs PT_TLS"); tls = true; block("TLS Orbis par thread non implémenté"); }
            if (s.type == 0x61000001 || s.type == 0x61000002) block("Paramètres processus/module SCE inventoriés mais non interprétés");
            if (s.type == 3) block("PT_INTERP non pris en charge");
            if (s.type != 0 && s.type != 1 && s.type != 2 && s.type != 3 && s.type != 4 && s.type != 6 &&
                s.type != 7 && s.type != 0x61000000 && s.type != 0x61000010 && s.type != 0x61000001 &&
                s.type != 0x61000002 && s.type != 0x6474e550 && s.type != 0x6fffff00 && s.type != 0x6fffff01 && s.type != 0x6474e551 && s.type != 0x6474e552)
                block("Type de segment inconnu : " + std::to_string(s.type));
        }
        auto addressOffset = [&](uint64_t address, uint64_t bytes) -> uint64_t {
            for (const auto& s : segments) if ((s.type == 1 || s.type == 0x61000010) &&
                address >= s.address && range(address-s.address,bytes,s.filesz)) return s.offset+address-s.address;
            throw std::runtime_error("Table dynamique hors des octets PT_LOAD");
        };
        auto covered = [&](uint64_t address, uint64_t bytes, uint32_t flags) {
            for (const auto& s : segments) if ((s.type == 1 || s.type == 0x61000010) &&
                (s.flags & flags) == flags && address >= s.address && range(address-s.address,bytes,s.memsz)) return true;
            return false;
        };
        auto relro = [&](uint64_t address, uint64_t bytes) {
            for (const auto& s : segments) if (s.type == 0x61000010 &&
                address >= s.address && range(address-s.address,bytes,s.memsz)) return true;
            return false;
        };
        for (const auto& s : segments) if (s.type == 7) {
            require(covered(s.address,s.memsz,4), "TLS hors PT_LOAD readable");
            if (s.filesz) require(addressOffset(s.address,s.filesz) == s.offset, "Image TLS différente du PT_LOAD");
        }
        bool loads = false;
        for (const auto& s : segments) loads |= s.type == 1;
        require(loads, "Aucun PT_LOAD");
        if (entry) {
            require(covered(entry,1,1), "Entrée hors segment exécutable");
            (void)addressOffset(entry,1);
        } else if (type == 2 || type == 0xfe00 || type == 0xfe10) block("Point d'entrée nul");
        if (!dynamic) { block("Aucune table dynamique : imports/exports non inventoriés"); return; }
        require(dynamic->filesz && dynamic->filesz%16 == 0 && dynamic->filesz/16 <= recordLimit, "Taille PT_DYNAMIC invalide");
        std::map<uint64_t,uint64_t> tags;
        std::vector<std::pair<uint64_t,uint64_t>> identities;
        std::vector<uint64_t> neededOffsets;
        bool terminated = false;
        for (uint64_t i = 0; i < dynamic->filesz; i += 16) {
            auto p = f + dynamic->offset+i;
            uint64_t tag = read(p,8), value = read(p+8,8);
            if (!tag) { terminated = true; break; }
            if (tag >= 0x61000000 && tag <= 0x61ffffff) sce = true;
            if (tag == 1) { neededOffsets.push_back(value); continue; }
            if (tag == 0x6100000d || tag == 0x6100000f || tag == 0x61000013 || tag == 0x61000015) {
                identities.emplace_back(tag,value); continue;
            }
            if (tag == 0x61000007 || tag == 0x61000017 || tag == 0x61000019) {
                block("Attribut/fingerprint SCE opaque : " + std::to_string(tag));
                continue; // These tags can occur once per library/fingerprint word.
            }
            require(tags.emplace(tag,value).second, "Tag dynamique singleton dupliqué");
            switch (tag) {
            case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
            case 12: case 13: case 14: case 15: case 20: case 21: case 23: case 24: case 30:
            case 0x61000005: case 0x61000007: case 0x61000009: case 0x61000011:
            case 0x61000017: case 0x61000019: case 0x61000025: case 0x61000027:
            case 0x61000029: case 0x6100002b: case 0x6100002d: case 0x6100002f:
            case 0x61000031: case 0x61000033: case 0x61000035: case 0x61000037:
            case 0x61000039: case 0x6100003b: case 0x6100003d: case 0x6100003f: break;
            default: block("Tag dynamique non pris en charge : " + std::to_string(tag));
            }
        }
        require(terminated, "PT_DYNAMIC sans DT_NULL");
        require(!sce || dynlib, "Tables SCE sans PT_SCE_DYNLIBDATA");
        auto get = [&](uint64_t tag) { auto it = tags.find(tag); return it == tags.end() ? uint64_t(0) : it->second; };
        auto locate = [&](uint64_t value, uint64_t bytes, bool isSCE) {
            if (!isSCE) return addressOffset(value,bytes);
            require(dynlib && range(value,bytes,dynlib->filesz), "Table SCE hors DYNLIBDATA");
            return dynlib->offset+value;
        };
        auto tableTag = [&](uint64_t standard, uint64_t sony) {
            require(!(tags.count(standard) && tags.count(sony)), "Tables standard/SCE ambiguës");
            return tags.count(sony) ? sony : standard;
        };
        uint64_t strTag = tableTag(5,0x61000035), strSizeTag = tableTag(10,0x61000037);
        require(tags.count(strTag) && tags.count(strSizeTag), "Table de chaînes dynamique manquante");
        require((strTag == 5) == (strSizeTag == 10), "STRTAB/STRSZ de formats différents");
        uint64_t strSize = get(strSizeTag);
        require(strSize && strSize <= 1024*1024, "Taille STRTAB invalide");
        uint64_t strOffset = locate(get(strTag),strSize,strTag != 5);
        auto stringAt = [&](uint64_t offset) -> std::string {
            require(offset < strSize, "Index chaîne hors limites");
            const char* p = reinterpret_cast<const char*>(f+strOffset+offset);
            size_t available = size_t(std::min(uint64_t(1025),strSize-offset));
            const char* end = static_cast<const char*>(std::memchr(p,0,available));
            require(end, "Chaîne non terminée ou supérieure à 1024 octets");
            std::string result(p,end);
            for (unsigned char c : result) require(c >= 32 && c < 127, "Chaîne ELF non ASCII imprimable");
            return result;
        };
        if (tags.count(0x61000009)) (void)stringAt(get(0x61000009));
        if (tags.count(14)) (void)stringAt(get(14));
        if (tags.count(15)) { (void)stringAt(get(15)); block("DT_RPATH ignoré ; aucune recherche automatique de fichiers"); }
        if (tags.count(0x61000011)) block("Attribut module SCE non appliqué");
        if (tags.count(24) || (get(30)&4)) block("DT_TEXTREL/DF_TEXTREL non pris en charge");
        if (tags.count(0x61000025) || tags.count(0x6100003d)) {
            require(tags.count(0x61000025) && tags.count(0x6100003d), "Paire HASH/HASHSZ SCE incomplète");
            uint64_t bytes = get(0x6100003d);
            require(bytes >= 8 && bytes%4 == 0 && bytes <= 1024*1024, "HASHSZ SCE invalide");
            (void)locate(get(0x61000025),bytes,true);
        }
        if (tags.count(3)) require(covered(get(3),8,4), "DT_PLTGOT hors mémoire readable");
        if (tags.count(0x61000027)) require(covered(get(0x61000027),8,4), "DT_SCE_PLTGOT hors mémoire readable");
        for (uint64_t n : neededOffsets) needed.push_back(stringAt(n));
        for (const auto& pair : identities) {
            uint64_t tag = pair.first, value = pair.second;
            bool module = tag == 0x6100000d || tag == 0x6100000f;
            bool exported = tag == 0x6100000d || tag == 0x61000013;
            Identity item{uint16_t(value>>48),uint16_t(value>>32),stringAt(uint32_t(value)),exported};
            require(!item.name.empty(), "Identité SCE vide");
            auto& collection = module ? modules : libraries;
            for (const auto& old : collection) require(old.id != item.id || old.exported != exported, "Identité SCE dupliquée");
            collection.push_back(std::move(item));
        }
        init = get(12); fini = get(13);
        require(!init || covered(init,1,1), "DT_INIT hors code");
        require(!fini || covered(fini,1,1), "DT_FINI hors code");
        if (init) (void)addressOffset(init,1);
        if (fini) (void)addressOffset(fini,1);
        if (init || fini) block("Initialisation/finalisation PRX déclarée mais non exécutée");
        uint64_t symTag = tableTag(6,0x61000039), symEnt = tableTag(11,0x6100003b);
        if (tags.count(symTag)) {
            require((symTag == 6) == (symEnt == 11), "SYMTAB/SYMENT de formats différents");
            require(get(symEnt) == 24, "SYMENT différent de 24");
            uint64_t symbolCount = 0;
            if (symTag != 6) {
                require(tags.count(0x6100003f) && get(0x6100003f)%24 == 0, "SYMTABSZ SCE invalide");
                symbolCount = get(0x6100003f)/24;
            } else {
                require(tags.count(4), "Comptage symboles standard sans DT_HASH non pris en charge");
                uint64_t hash = locate(get(4),8,false);
                uint64_t buckets = read(f+hash,4);
                symbolCount = read(f+hash+4,4);
                require(buckets && buckets <= symbolLimit && symbolCount <= symbolLimit, "DT_HASH trop grand");
                (void)locate(get(4),8+4*(buckets+symbolCount),false);
            }
            require(symbolCount && symbolCount <= symbolLimit, "Nombre de symboles invalide");
            uint64_t symOffset = locate(get(symTag),symbolCount*24,symTag != 6);
            for (uint64_t i = 0; i < symbolCount; ++i) {
                const auto* p = f+symOffset+i*24;
                Symbol s;
                s.encoded = stringAt(read(p,4)); s.nid = s.encoded;
                s.binding = p[4]>>4; s.kind = p[4]&15; s.visibility = p[5];
                if (s.visibility) block("Visibilité symbolique non prise en charge par préparation PRX");
                uint16_t section = uint16_t(read(p+6,2));
                s.defined = section != 0; s.value = read(p+8,8); s.size = read(p+16,8);
                if (i == 0) require(read(p,8) == 0 && s.value == 0 && s.size == 0, "Symbole nul invalide");
                if (s.defined && i) {
                    if (section >= 0xff00 || s.kind == 6) block("Symbole spécial/TLS non pris en charge");
                    else require(covered(s.value,std::max(uint64_t(1),s.size),0), "Export hors mémoire chargeable");
                }
                auto first = s.encoded.find('#');
                if (first != std::string::npos) {
                    auto second = s.encoded.find('#',first+1);
                    require(second != std::string::npos && s.encoded.find('#',second+1) == std::string::npos,
                        "Nom NID qualifié invalide");
                    s.nid = s.encoded.substr(0,first);
                    require(s.nid.size() == 11 && s.nid.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-") == std::string::npos,
                        "NID SCE invalide");
                    uint16_t libID = id(s.encoded.substr(first+1,second-first-1));
                    uint16_t modID = id(s.encoded.substr(second+1));
                    for (const auto& l : libraries) if (l.id == libID && l.exported == s.defined) {
                        s.library = l.name; s.libraryVersion = l.version;
                    }
                    for (const auto& m : modules) if (m.id == modID && m.exported == s.defined) {
                        s.module = m.name; s.moduleVersion = m.version;
                    }
                    require(!s.library.empty() && !s.module.empty(), "NID référence une identité SCE absente");
                    s.qualified = true;
                }
                if (!s.defined && i && !s.encoded.empty()) {
                    s.resolved = s.qualified && resolver && resolver(s);
                    if (!s.resolved) block("Imports non résolus ; aucune réussite simulée");
                }
                symbols.push_back(std::move(s));
            }
        }
        std::set<uint64_t> targets;
        auto rela = [&](uint64_t pointerTag, uint64_t sizeTag, bool sony, bool plt) {
            if (!tags.count(pointerTag) && !tags.count(sizeTag)) return;
            require(tags.count(pointerTag) && tags.count(sizeTag), "Paire RELA incomplète");
            uint64_t bytes = get(sizeTag);
            require(bytes%24 == 0 && bytes/24 <= relocationLimit-relocations.size(), "Taille RELA invalide");
            if (plt) require(get(sony ? 0x6100002b : 20) == 7, "PLT REL non pris en charge");
            else require(get(sony ? 0x61000033 : 9) == 24, "RELAENT différent de 24");
            uint64_t offset = locate(get(pointerTag),bytes,sony);
            for (uint64_t i = 0; i < bytes; i += 24) {
                auto p = f+offset+i;
                uint64_t info = read(p+8,8), rawAddend = read(p+16,8);
                int64_t addend; std::memcpy(&addend,&rawAddend,8);
                uint32_t kind = uint32_t(info);
                uint64_t sym = info>>32;
                require(kind == 0 || sym < symbols.size() || (sym == 0 && symbols.empty()), "Index RELA hors SYMTAB");
                bool supported = kind == 0 || kind == 1 || kind == 6 || kind == 7 || kind == 8;
                Relocation r{read(p,8),sym,addend,kind,supported};
                if (kind == 8) require(sym == 0, "R_RELATIVE avec symbole");
                // PT_SCE_RELRO is written while relocating, then becomes read-only.
                if (kind != 0 && !covered(r.target,8,2) && !relro(r.target,8)) {
                    std::ostringstream why;
                    why << "Cible RELA hors segment writable : cible=0x" << std::hex << r.target
                        << " type=" << std::dec << kind;
                    throw std::runtime_error(why.str());
                }
                // Sorted targets: no 8-byte write may overlap another (a pairwise
                // scan is quadratic, hours for Unreal Engine's ~800k relocations).
                if (kind) {
                    const auto next = targets.lower_bound(r.target);
                    require((next == targets.end() || *next - r.target >= 8) &&
                            (next == targets.begin() || r.target - *std::prev(next) >= 8), "Cibles RELA se chevauchant");
                    targets.insert(r.target);
                }
                if (!supported) block("Relocations non prises en charge");
                relocations.push_back(r);
            }
        };
        require(!(tags.count(7) && tags.count(0x6100002f)) && !(tags.count(23) && tags.count(0x61000029)), "RELA standard/SCE ambiguës");
        rela(7,8,false,false); rela(23,2,false,true);
        rela(0x6100002f,0x61000031,true,false); rela(0x61000029,0x6100002d,true,true);
        if (!needed.empty() || std::any_of(modules.begin(),modules.end(),[](const Identity& m){return !m.exported;}))
            block("Dépendances PRX externes : fichiers non sélectionnés par cette analyse");
    }
};
} // namespace maxps4
