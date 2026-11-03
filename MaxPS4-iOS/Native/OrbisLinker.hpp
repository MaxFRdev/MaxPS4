#pragma once
#include "PS4PKGRuntimePlan.hpp"
#include "OrbisMetadata.hpp"
#include <map>
#include <set>
#include <tuple>
#include <stdexcept>
#include <sstream>

namespace maxps4 {
// Staging linker only. All addresses are guest addresses; vectors are ordinary
// host data, never executable mappings. See docs/ORBIS-LINKER.md for the subset.
class OrbisLinker {
public:
    struct Identity {
        std::string nid, library, module;
        uint16_t libraryVersion = 0, moduleVersion = 0;
        uint8_t type = 0;
        auto key() const { return std::tie(nid, library, module, libraryVersion, moduleVersion, type); }
        bool operator<(const Identity& b) const { return key() < b.key(); }
    };
    struct Report {
        std::string phase = "inputs", failure;
        size_t validatedModules = 0, segments = 0, memoryBytes = 0;
        size_t dependencies = 0, imports = 0, resolvedImports = 0;
        size_t relocations = 0, validatedRelocations = 0, ignoredRelocations = 0;
        size_t tlsModules = 0; uint64_t staticTLSBytes = 0;
        bool committed = false;
        std::string text() const {
            std::ostringstream out;
            out << "Préparation runtime Orbis (aucune exécution)\n"
                << "Étape : " << phase << "\n"
                << "ELF validés : " << validatedModules << " ; segments validés : " << segments << "\n"
                << "Dépendances PRX vérifiées : " << dependencies << "\n"
                << "Imports NID examinés : " << imports << " ; résolus : " << resolvedImports << " ; non résolus : " << imports - resolvedImports << "\n"
                << "Relocations examinées : " << relocations << " ; validées : " << validatedRelocations
                << " ; appliquées : " << (committed ? validatedRelocations : 0)
                << " ; différées : " << (committed ? 0 : validatedRelocations)
                << " ; refusées : " << (relocations - validatedRelocations) << "\n"
                << "Relocations NONE ignorées (aucune écriture) : " << ignoredRelocations << "\n"
                << "TLS statique : " << tlsModules << " module(s) ; " << staticTLSBytes
                << " octets par thread (disposition calculée, blocs non créés)\n"
                << "Mémoire : " << memoryBytes << " octets validés ; "
                << (committed ? "image engagée" : "image non engagée, allocations provisoires libérées") << "\n";
            if (!failure.empty()) out << "Refus : " << failure << "\n";
            out << "Préparation : " << (committed ? "réussie" : "refusée")
                << " ; compteurs partiels, étapes suivantes non évaluées en cas de refus\n"
                << "Exécution : impossible — backend d'espace d'adresses FEX Orbis, pile/TLS et ABI requis";
            return out.str();
        }
    };
    // Guest trampoline, never a host function pointer. guestAddress=0 with a
    // non-zero token asks the linker to synthesize a trampoline (see Thunk).
    // A `noreturn` service's trampoline ends with HLT instead of RET.
    // A data object (identity type 1, STT_OBJECT, such as libc's stdout or
    // libkernel's __stack_chk_guard) with guestAddress=0 gets `token` zeroed
    // bytes in the RW module "<hle-data>"; the runtime may fill them (DataObject).
    // A function with `code` (and guestAddress=0, token=0) is that guest
    // routine itself, placed in the RX module "<hle-code>" (16-byte aligned):
    // for HLE that must call guest code back, such as pthread_once.
    struct HLE {
        Identity identity; uint64_t guestAddress; uint32_t token = 0; bool noreturn = false;
        std::vector<uint8_t> code = {};
    };
    static constexpr uint64_t codeLimit = 4096;
    struct DataObject { uint64_t address, size; std::string nid; };
    static constexpr uint64_t dataObjectLimit = 4096;
    // Synthesized trampoline: MOV R10,RCX; MOV EAX,token; SYSCALL; RET. The
    // runtime gate accepts a SYSCALL only at `address + syscallOffset` with
    // that exact token; the token is the HLE service number.
    struct Thunk { uint64_t address; uint32_t token; };
    static constexpr uint64_t thunkSize = 11, syscallOffset = 8;
    struct Input { PS4PKGRuntimePlan::ModuleBytes file; uint64_t bias = 0; };
    struct Module { std::string path; uint64_t bias = 0, entry = 0; PS4ExecutableImage image; };
    struct Mapping { uint64_t address, size; uint32_t permissions; const uint8_t* data; };
    // Static TLS, x86-64 variant II (FreeBSD/Orbis): each block ends at or below
    // the thread pointer, at TP - offset. Module IDs start at 1 in load order.
    // A backend must copy fileSize template bytes from guest address `image`,
    // zero the rest of memorySize, per thread. Nothing here creates threads.
    struct TLSBlock { size_t module; uint64_t id, image, fileSize, memorySize, alignment, offset; };
    static constexpr uint64_t tlsBudget = 1024 * 1024;
    struct Image {
        std::vector<Module> modules;
        std::vector<TLSBlock> tls;
        uint64_t staticTLSSize = 0, staticTLSAlignment = 1;
        std::vector<Thunk> thunks; // In module "<hle>", an RX segment after all modules.
        std::vector<DataObject> data; // In module "<hle-data>", RW, after "<hle>".
        // module_start of every PRX (0xfe18) in load order (dependencies first):
        // on PS4 a PRX's DT_INIT is its module_start(args, argp, param), and it
        // runs its own DT_INIT_ARRAY (shadPS4 Module::Start). Executables get
        // nothing here: their own startup code runs their initializers from the
        // entry. All DT_INIT/FINI(_ARRAY) entries are still validated.
        std::vector<uint64_t> initializers;
        // Every defined, non-TLS export of the supplied modules (by module index),
        // for a runtime's sceKernelDlsym.
        struct Exported { size_t module; std::string nid; uint64_t address; };
        std::vector<Exported> exported;
        // A backend must copy/map these bytes at their guest addresses and apply
        // the advertised permissions. No identity mapping is implied.
        std::vector<Mapping> mappings() const {
            std::vector<Mapping> result;
            for (const auto& m : modules) for (const auto& s : m.image.segments)
                result.push_back({m.bias + s.header.address, s.header.memorySize, s.header.flags, s.bytes.data()});
            return result;
        }
        const uint8_t* translate(uint64_t address, size_t count, uint32_t permissions) const {
            for (const auto& m : modules) if (address >= m.bias)
                if (const auto* p = m.image.translate(address - m.bias, count, permissions)) return p;
            return nullptr;
        }
    };
    // Strong transaction: output is unchanged on every failure, including OOM.
    static bool link(const std::vector<Input>& inputs, const std::vector<HLE>& hle,
                     Image& output, std::string& diagnostic,
                     size_t budget = PS4PKGRuntimePlan::batchMemoryBudget, Report* report = nullptr) {
        Report progress;
        try {
            Image pending;
            build(inputs, hle, pending, budget, progress);
            progress.phase = "complete"; progress.committed = true;
            if (report) *report = progress;
            output = std::move(pending);
            diagnostic.clear();
            return true;
        } catch (const std::bad_alloc&) { diagnostic = "linker: allocation failed"; }
          catch (const std::runtime_error& e) { diagnostic = e.what(); }
        progress.committed = false;
        progress.failure = diagnostic;
        if (report) *report = std::move(progress);
        return false;
    }
private:
    struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
    static void require(bool ok, const std::string& message) { if (!ok) throw Failure(message); }
    static uint64_t read(const uint8_t* p, size_t width) {
        uint64_t v = 0; for (size_t i = 0; i < width; ++i) v |= uint64_t(p[i]) << (i * 8); return v;
    }
    static bool range(uint64_t p, uint64_t n, uint64_t size) { return p <= size && n <= size - p; }
    static uint64_t add(uint64_t a, uint64_t b) {
        require(b <= UINT64_MAX - a, "guest address overflow"); return a + b;
    }
    static uint64_t signedAdd(uint64_t a, uint64_t bits) {
        if (!(bits >> 63)) return add(a, bits);
        const uint64_t magnitude = (~bits) + 1;
        require(a >= magnitude, "relocation underflow"); return a - magnitude;
    }
    static std::string encoded(uint16_t id) {
        const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
        std::string out; do { out.insert(out.begin(), alphabet[id & 63]); id >>= 6; } while (id); return out;
    }
    struct Descriptor { std::string name; uint16_t version; };
    struct Symbol { Identity identity; uint64_t value = 0, size = 0; uint8_t type = 0; bool defined = false, local = false; };
    struct Rela { uint64_t offset, info, addend; };
    struct Dynamic {
        std::vector<Symbol> symbols; std::vector<Rela> relas;
        std::vector<Descriptor> imports, exports, importLibraries, exportLibraries;
        std::vector<std::string> needed;
        uint64_t init = 0, fini = 0, initArray = 0, finiArray = 0, initArraySize = 0, finiArraySize = 0;
        bool hasInit = false; // DT_INIT 0 is a real address: libc.prx's module_start.
    };
    static Dynamic parse(const Input& input, const Module& m, uint64_t tlsMemory, size_t& symbolBudget, size_t& relocationBudget) {
        const auto* bytes = input.file.bytes; const auto size = input.file.size;
        const PS4ExecutableImage::Header *dyn = nullptr, *data = nullptr;
        for (const auto& h : m.image.headers) {
            if (h.type == 2) { require(!dyn, "duplicate PT_DYNAMIC"); dyn = &h; }
            if (h.type == 0x61000000) { require(!data, "duplicate PT_SCE_DYNLIBDATA"); data = &h; }
        }
        if (!dyn && !data && m.image.type == 0xfe00 && !m.image.requiresLinker) return {};
        require(dyn && data, "SCE PT_DYNAMIC and PT_SCE_DYNLIBDATA required");
        require(dyn->fileSize && dyn->fileSize % 16 == 0 && dyn->fileSize / 16 <= 4096 &&
                range(dyn->offset, dyn->fileSize, size) && range(data->offset, data->fileSize, size),
                "dynamic metadata out of bounds");
        std::map<uint64_t, uint64_t> tags;
        std::vector<std::pair<uint64_t, uint64_t>> descriptors;
        std::vector<uint64_t> needed;
        bool terminated = false;
        for (size_t i = 0; i < dyn->fileSize; i += 16) {
            const auto* p = bytes + dyn->offset + i;
            const auto tag = read(p, 8), value = read(p + 8, 8);
            if (!tag) { terminated = true; break; }
            if (tag == 1) { needed.push_back(value); continue; } // DT_NEEDED, SCE string table offset
            if (tag == 0x6100000d || tag == 0x6100000f || tag == 0x61000013 || tag == 0x61000015) {
                descriptors.push_back({tag, value}); continue;
            }
            // Module and library attributes: one per descriptor, not interpreted.
            if (tag == 0x61000011 || tag == 0x61000017 || tag == 0x61000019) continue;
            // Only interpreted, verified SCE table tags. Unknown semantics fail closed.
            switch (tag) {
            case 0x61000035: case 0x61000037: case 0x61000039: case 0x6100003b: case 0x6100003f:
            case 0x6100002f: case 0x61000031: case 0x61000033:
            case 0x61000029: case 0x6100002b: case 0x6100002d:
            // DT_INIT/DT_FINI and their arrays (module-relative addresses).
            case 12: case 13: case 25: case 26: case 27: case 28: break;
            // Informational tags real games carry, which linking by NID does not
            // need (shadPS4 ignores them too): SCE ID table entry size,
            // fingerprint, original file name, module/library attributes, stub
            // names and versions, symbol hash table and its size, SCE PLTGOT;
            // DT_SONAME, DT_RPATH, DT_DEBUG, DT_TEXTREL (relocation targets are
            // still checked one by one), DT_BIND_NOW, DT_FLAGS, DT_RELACOUNT,
            // DT_FLAGS_1.
            case 0x61000005: case 0x61000007: case 0x61000009: case 0x61000011:
            case 0x61000017: case 0x61000019: case 0x6100001d: case 0x6100001f:
            case 0x61000021: case 0x61000023: case 0x61000025: case 0x61000027: case 0x6100003d:
            case 14: case 15: case 21: case 22: case 24: case 30: case 0x6ffffff9: case 0x6ffffffb: break;
            // DT_PREINIT_ARRAY(SZ): an executable's own startup code runs it,
            // like its DT_INIT_ARRAY.
            case 32: case 33: break;
            default: throw Failure("unsupported dynamic tag " + std::to_string(tag));
            }
            require(tags.emplace(tag, value).second, "duplicate dynamic tag");
        }
        require(terminated, "unterminated dynamic table");
        auto get = [&](uint64_t tag) {
            auto it = tags.find(tag); require(it != tags.end(), "missing dynamic table tag"); return it->second;
        };
        auto table = [&](uint64_t offset, uint64_t length) {
            require(range(offset, length, data->fileSize), "SCE table out of bounds");
            return bytes + data->offset + offset;
        };
        const auto stringSize = get(0x61000037);
        require(stringSize && stringSize <= 1024 * 1024, "string table budget");
        const auto* strings = table(get(0x61000035), stringSize);
        require(strings[0] == 0, "missing null string");
        auto stringAt = [&](uint64_t offset) {
            require(offset < stringSize, "string offset out of bounds");
            const auto available = std::min<uint64_t>(stringSize - offset, 256);
            const auto* end = static_cast<const uint8_t*>(std::memchr(strings + offset, 0, available));
            require(end, "unterminated or oversized string");
            for (auto* p = strings + offset; p != end; ++p) require(*p >= 0x21 && *p <= 0x7e, "invalid identity string");
            return std::string(reinterpret_cast<const char*>(strings + offset), end - (strings + offset));
        };
        std::map<std::string, Descriptor> importLib, exportLib, importMod, exportMod;
        for (const auto& d : descriptors) {
            auto& dest = d.first == 0x6100000d ? exportMod : d.first == 0x6100000f ? importMod :
                         d.first == 0x61000013 ? exportLib : importLib;
            const auto name = stringAt(uint32_t(d.second));
            require(!name.empty(), "empty descriptor");
            require(dest.emplace(encoded(uint16_t(d.second >> 48)), Descriptor{name, uint16_t(d.second >> 32)}).second,
                    "duplicate descriptor ID");
        }
        require(get(0x6100003b) == 24, "unsupported symbol entry size");
        const auto symbolSize = get(0x6100003f);
        require(symbolSize >= 24 && symbolSize % 24 == 0 && symbolSize / 24 <= symbolBudget, "symbol table budget or size");
        symbolBudget -= static_cast<size_t>(symbolSize / 24);
        const auto* symbols = table(get(0x61000039), symbolSize);
        Dynamic out;
        for (const auto& d : importLib) out.importLibraries.push_back(d.second);
        for (const auto& d : exportLib) out.exportLibraries.push_back(d.second);
        for (const auto& d : importMod) out.imports.push_back(d.second);
        for (const auto& d : exportMod) out.exports.push_back(d.second);
        for (auto offset : needed) {
            const auto name = stringAt(offset);
            require(!name.empty() && std::find(out.needed.begin(), out.needed.end(), name) == out.needed.end(),
                    "empty or duplicate DT_NEEDED");
            out.needed.push_back(name);
        }
        for (size_t i = 0; i < symbolSize; i += 24) {
            const auto* p = symbols + i;
            Symbol s; const auto name = stringAt(read(p, 4));
            const uint8_t bind = p[4] >> 4, type = p[4] & 15;
            const auto section = read(p + 6, 2);
            s.value = read(p + 8, 8); s.size = read(p + 16, 8); s.defined = section != 0; s.local = bind == 0; s.type = type;
            if (!i) { require(read(p, 8) == 0 && s.value == 0 && s.size == 0, "invalid null symbol"); out.symbols.push_back(s); continue; }
            // Local STT_SECTION symbols (ordinary ELF bookkeeping) are kept as locals.
            // Defined symbols may be hidden or protected (system PRX); imports are default.
            require(bind <= 2 && (type == 0 || type == 1 || type == 2 || type == 6 || (type == 3 && bind == 0)) &&
                    (p[5] & ~3) == 0 && (!p[5] || section != 0) && section < 0xff00,
                    "unsupported symbol binding/type/visibility/section: " + name + " bind=" + std::to_string(bind) +
                    " type=" + std::to_string(type) + " visibility=" + std::to_string(p[5]) +
                    " section=" + std::to_string(section));
            // STT_TLS values are offsets inside the module's TLS block, not addresses.
            if (s.defined && type == 6) require(tlsMemory && s.value <= tlsMemory && s.size <= tlsMemory - s.value,
                                                "TLS symbol outside module TLS block");
            else if (s.defined) require(m.image.translate(s.value, s.size ? s.size : 1, type == 2 ? 5 : 4), "export outside valid segment");
            else if (type != 3) require(s.value == 0 && !s.local, "invalid undefined symbol");
            if (s.defined && type == 2) {
                bool backed = false;
                for (const auto& region : m.image.segments) {
                    if ((region.header.flags & 5) != 5 || s.value < region.header.address) continue;
                    const auto offset = s.value - region.header.address;
                    backed |= offset < region.header.fileSize &&
                        (s.size ? s.size : 1) <= region.header.fileSize - offset;
                }
                require(backed, "function export outside file-backed code");
            }
            // A plain name (module_start, __start__Ztext) is not an NID identity:
            // defined, it binds nothing, like a local; a weak undefined one stays
            // 0, as an unresolved weak symbol does, and so do undefined section
            // bounds (__start_X/__stop_X both 0: an empty range).
            if (!s.local && name.find('#') == std::string::npos) {
                const bool bound = name.rfind("__start_", 0) == 0 || name.rfind("__stop_", 0) == 0;
                require(s.defined || bind == 2 || bound, "undefined non-NID symbol: " + name);
                s.local = true;
            }
            if (!s.local) {
                const auto a = name.find('#'), b = a == std::string::npos ? a : name.find('#', a + 1);
                require(a == 11 && b != std::string::npos && name.find('#', b + 1) == std::string::npos, "invalid NID identity: " + name);
                s.identity.nid = name.substr(0, a);
                require(s.identity.nid.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-") == std::string::npos,
                        "invalid NID alphabet");
                const auto& libs = s.defined ? exportLib : importLib; const auto& mods = s.defined ? exportMod : importMod;
                auto l = libs.find(name.substr(a + 1, b - a - 1)), mod = mods.find(name.substr(b + 1));
                require(l != libs.end() && mod != mods.end(), "symbol references missing library/module ID");
                s.identity.library = l->second.name; s.identity.libraryVersion = l->second.version;
                s.identity.module = mod->second.name; s.identity.moduleVersion = mod->second.version; s.identity.type = type;
            }
            out.symbols.push_back(std::move(s));
        }
        auto relocations = [&](uint64_t offsetTag, uint64_t sizeTag, uint64_t kindTag, uint64_t expected) {
            if (!tags.count(offsetTag) && !tags.count(sizeTag) && !tags.count(kindTag)) return;
            require(get(kindTag) == expected, "unsupported relocation table encoding");
            const auto n = get(sizeTag); require(n % 24 == 0 && n / 24 <= relocationBudget, "relocation table budget or size");
            relocationBudget -= static_cast<size_t>(n / 24);
            const auto* p = table(get(offsetTag), n);
            for (size_t i = 0; i < n; i += 24) out.relas.push_back({read(p + i, 8), read(p + i + 8, 8), read(p + i + 16, 8)});
        };
        relocations(0x6100002f, 0x61000031, 0x61000033, 24);
        relocations(0x61000029, 0x6100002d, 0x6100002b, 7); // DT_RELA
        auto optional = [&](uint64_t tag) { auto it = tags.find(tag); return it == tags.end() ? uint64_t{0} : it->second; };
        out.init = optional(12); out.fini = optional(13); out.hasInit = tags.count(12) != 0;
        out.initArray = optional(25); out.finiArray = optional(26);
        out.initArraySize = optional(27); out.finiArraySize = optional(28);
        // An empty array (size 0) is no array, whatever its address.
        if (!out.initArraySize) out.initArray = 0;
        if (!out.finiArraySize) out.finiArray = 0;
        require(!out.initArray == !out.initArraySize && !out.finiArray == !out.finiArraySize,
                "DT_INIT_ARRAY/DT_FINI_ARRAY need both address and size");
        return out;
    }
    static void build(const std::vector<Input>& inputs, const std::vector<HLE>& hle, Image& out, size_t budget, Report& report) {
        require(!inputs.empty() && inputs.size() <= PS4PKGRuntimePlan::maxDependencies, "invalid module count (1..128)");
        for (const auto& input : inputs) {
            require(input.file.request.logicalPath.size() <= 255 && input.file.request.dependencies.size() <= 128,
                    "module request budget exceeded");
            for (const auto& dependency : input.file.request.dependencies)
                require(dependency.size() <= 255, "dependency path budget exceeded");
        }
        require(budget <= PS4PKGRuntimePlan::batchMemoryBudget, "budget exceeds existing limit");
        require(hle.size() <= 65536, "HLE registry budget");
        std::vector<size_t> sorted(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) sorted[i] = i;
        std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) { return inputs[a].file.request.logicalPath < inputs[b].file.request.logicalPath; });
        std::vector<PS4PKGRuntimePlan::ModuleRequest> requests;
        for (auto i : sorted) {
            auto r = inputs[i].file.request; std::sort(r.dependencies.begin(), r.dependencies.end()); requests.push_back(std::move(r));
        }
        std::vector<size_t> order; std::string error;
        require(PS4PKGRuntimePlan::orderModules(requests, order, error), error);
        std::vector<Dynamic> dynamic;
        report.phase = "ELF/segments";
        // Check the complete segment budget before allocating any staged image.
        // The existing loader still validates every field independently below.
        size_t planned = 0;
        for (auto index : sorted) {
            const auto& input = inputs[index];
            const auto* b = input.file.bytes; const auto n = input.file.size;
            require(b && n >= 64 && n <= OrbisMetadata::fileLimit, "chargeur: invalid ELF file size");
            const auto phoff = read(b + 32, 8), phnum = read(b + 56, 2);
            require(phoff >= 64 && phnum && phnum <= 128 && range(phoff, phnum * 56, n), "chargeur: invalid program table");
            for (size_t i = 0; i < phnum; ++i) {
                const auto* p = b + phoff + i * 56; const auto type = read(p, 4);
                if (type != 1 && type != 0x61000010) continue;
                const auto size = read(p + 40, 8);
                require(size <= budget - planned, "batch memory budget exceeded"); planned += static_cast<size_t>(size);
            }
        }
        size_t allocated = 0, symbolBudget = 4 * OrbisMetadata::symbolLimit, relocationBudget = 2 * OrbisMetadata::relocationLimit;
        for (auto i : order) {
            report.phase = "ELF/segments";
            const auto& in = inputs[sorted[i]];
            PS4PKGRuntimePlan::PreparedModule staged;
            require(PS4PKGRuntimePlan::prepareStandalone(in.file.request.logicalPath, in.file.bytes, in.file.size,
                    in.file.request.dependencies, staged, error), error);
            ++report.validatedModules;
            Module m {staged.logicalPath, in.bias, 0, std::move(staged.image)};
            uint64_t tlsMemory = 0;
            const PS4ExecutableImage::Header* tls = nullptr;
            // An empty PT_TLS (some games keep the header) declares no TLS.
            for (const auto& h : m.image.headers) if (h.type == 7 && (h.memorySize || h.fileSize)) {
                require(!tls, "duplicate PT_TLS"); tls = &h;
            }
            if (tls) {
                report.phase = "TLS";
                const auto alignment = tls->alignment ? tls->alignment : 1;
                require(!(alignment & (alignment - 1)) && alignment <= 4096,
                        "invalid PT_TLS alignment " + std::to_string(tls->alignment));
                require(tls->memorySize && tls->fileSize <= tls->memorySize && tls->memorySize <= tlsBudget,
                        "invalid PT_TLS size: filesz=" + std::to_string(tls->fileSize) +
                        " memsz=" + std::to_string(tls->memorySize));
                // The initialization image must lie in loaded, readable, file-backed bytes.
                bool backed = !tls->fileSize;
                for (const auto& region : m.image.segments) {
                    if (!(region.header.flags & 4) || tls->address < region.header.address) continue;
                    const auto offset = tls->address - region.header.address;
                    backed |= offset < region.header.fileSize && tls->fileSize <= region.header.fileSize - offset;
                }
                require(backed, "TLS template outside file-backed segment");
                // Variant II: blocks are stacked downward from the thread pointer.
                const auto end = add(out.staticTLSSize, tls->memorySize);
                const auto offset = add(end, (alignment - end % alignment) % alignment);
                require(offset <= tlsBudget, "static TLS budget exceeded");
                out.tls.push_back({out.modules.size(), out.tls.size() + 1, add(m.bias, tls->address),
                                   tls->fileSize, tls->memorySize, alignment, offset});
                out.staticTLSSize = offset;
                out.staticTLSAlignment = std::max(out.staticTLSAlignment, alignment);
                tlsMemory = tls->memorySize;
                ++report.tlsModules; report.staticTLSBytes = offset;
                report.phase = "ELF/segments";
            }
            // Metadata games carry (interpreter path, process/module parameters, unwind tables,
            // stack/RELRO hints, comment, library version) is not mapped here.
            for (const auto& h : m.image.headers) {
                require(h.type == 0 || h.type == 1 || h.type == 2 || h.type == 4 || h.type == 6 ||
                        h.type == 7 || h.type == 0x61000000 || h.type == 0x61000010 ||
                        h.type == 3 || h.type == 0x61000001 || h.type == 0x61000002 || h.type == 0x6474e550 ||
                        h.type == 0x6474e551 || h.type == 0x6474e552 || h.type == 0x6fffff00 || h.type == 0x6fffff01,
                        "unsupported runtime program header " + std::to_string(h.type));
            }
            // PT_SCE_RELRO is written by relocations, then made read-only below.
            for (auto& s : m.image.segments) if (s.header.type == 0x61000010) s.header.flags |= 2U;
            for (const auto& s : m.image.segments) {
                require(s.bytes.size() <= budget - allocated, "batch memory budget exceeded"); allocated += s.bytes.size();
                require((s.header.flags & 3) != 3, "writable executable guest segment refused");
                require(s.header.alignment <= 1 || m.bias % s.header.alignment == 0, "misaligned load bias");
                const auto start = add(m.bias, s.header.address), end = add(start, s.header.memorySize);
                require(end <= (uint64_t{1} << 47), "non-canonical user guest address");
                for (const auto& old : out.modules) for (const auto& region : old.image.segments) {
                    const auto a = add(old.bias, region.header.address), b = add(a, region.header.memorySize);
                    require(start >= b || a >= end, "overlapping guest modules");
                }
                ++report.segments; report.memoryBytes += s.bytes.size();
            }
            if (m.image.entry) {
                bool valid = false;
                for (const auto& segment : m.image.segments)
                    valid |= (segment.header.flags & 1) && m.image.entry >= segment.header.address &&
                             m.image.entry - segment.header.address < segment.header.fileSize;
                require(valid, "invalid module entry"); m.entry = add(m.bias, m.image.entry);
            }
            report.phase = "dynamic metadata";
            dynamic.push_back(parse(in, m, tlsMemory, symbolBudget, relocationBudget)); out.modules.push_back(std::move(m));
        }
        report.phase = "PRX dependencies";
        // Validate even unused module descriptors: symbols alone cannot prove a
        // closed dependency graph. Only explicit supplied files can be providers.
        for (size_t i = 0; i < dynamic.size(); ++i) {
            const auto& deps = requests[order[i]].dependencies;
            auto validateDescriptor = [&](const Descriptor& required, bool library) {
                // A system library/module implemented by the HLE registry needs no
                // PRX file, as long as no supplied module also claims that name.
                bool offeredByModule = false;
                for (const auto& d : dynamic) for (const auto& offered : library ? d.exportLibraries : d.exports)
                    offeredByModule |= offered.name == required.name;
                // A system one none of whose symbols is imported has nothing to
                // bind (games list modules they barely use). Supplied modules
                // keep their explicit dependency either way.
                bool used = false;
                for (const auto& s : dynamic[i].symbols)
                    used |= !s.defined && !s.local && (library ? s.identity.library : s.identity.module) == required.name;
                if (!offeredByModule && !used) return;
                if (!offeredByModule) for (const auto& h : hle)
                    if ((library ? h.identity.library : h.identity.module) == required.name &&
                        (!library || h.identity.libraryVersion == required.version)) return;
                size_t named = 0, compatible = 0, declared = 0;
                for (size_t j = 0; j < dynamic.size(); ++j) {
                    const auto& offeredDescriptors = library ? dynamic[j].exportLibraries : dynamic[j].exports;
                    for (const auto& offered : offeredDescriptors) if (offered.name == required.name) {
                        ++named;
                        if (offered.version != required.version) continue;
                        ++compatible;
                        if (std::find(deps.begin(), deps.end(), out.modules[j].path) != deps.end()) ++declared;
                    }
                }
                const auto kind = library ? " PRX library: " : " PRX module: ";
                require(named != 0, "missing" + std::string(kind) + required.name);
                require(compatible != 0, "incompatible" + std::string(kind) + required.name);
                require(declared != 0, "missing explicit dependency:" + std::string(kind) + required.name);
                require(declared == 1, "ambiguous" + std::string(kind) + required.name);
            };
            for (const auto& required : dynamic[i].imports) {
                validateDescriptor(required, false); ++report.dependencies;
            }
            for (const auto& required : dynamic[i].importLibraries) validateDescriptor(required, true);
            for (const auto& required : dynamic[i].needed) {
                // A system module that no supplied file exports (libkernel.prx,
                // libSceGnmDriver.prx, ...) is served by HLE: no file needed.
                const auto stem = required.substr(0, required.find('.'));
                bool offeredByModule = false;
                for (const auto& d : dynamic) for (const auto& offered : d.exports) offeredByModule |= offered.name == stem;
                if (!offeredByModule && std::find(deps.begin(), deps.end(), required) == deps.end()) continue;
                require(std::find(deps.begin(), deps.end(), required) != deps.end(),
                        "DT_NEEDED missing explicit dependency: " + required);
                ++report.dependencies;
            }
        }
        report.phase = "NID imports";
        struct Export { uint64_t address; size_t owner; };
        std::map<Identity, Export> exports, hleExports; // HLE keys have module version 0.
        // TLS exports carry their block offset; the owner gives the TLS module.
        for (size_t i = 0; i < dynamic.size(); ++i) for (const auto& s : dynamic[i].symbols) if (s.defined && !s.local)
            require(exports.emplace(s.identity, Export{s.type == 6 ? s.value : add(out.modules[i].bias, s.value), i}).second,
                    "duplicate or ambiguous export: " + s.identity.nid);
        for (size_t i = 0; i < dynamic.size(); ++i) for (const auto& s : dynamic[i].symbols)
            if (s.defined && !s.local && s.type != 6) out.exported.push_back({i, s.identity.nid, add(out.modules[i].bias, s.value)});
        // Synthesized trampolines live in one RX module after every supplied
        // module, separated by a 16 KiB guard (iOS page size).
        size_t synthesized = 0;
        uint64_t dataBytes = 0, codeBytes = 0;
        auto isData = [](const HLE& h) { return h.identity.type == 1; };
        std::set<Identity> exportKeys; // Module version 0, filled on first use.
        for (const auto& h : hle) {
            if (!h.code.empty()) {
                require(!h.guestAddress && !h.token && !isData(h) && h.code.size() <= codeLimit,
                        "HLE code entry needs no address, no token and at most 4 KiB");
                codeBytes += (h.code.size() + 15) & ~size_t{15};
                continue;
            }
            require(!h.guestAddress || !h.token, "HLE entry has both an address and a token");
            require(h.guestAddress || h.token, "HLE entry needs an address or a token");
            if (h.guestAddress) continue;
            if (!isData(h)) { ++synthesized; continue; }
            require(h.token <= dataObjectLimit, "HLE data object too large");
            dataBytes += (uint64_t(h.token) + 15) & ~uint64_t{15};
        }
        uint64_t thunkBase = 0;
        if (synthesized) {
            uint64_t highest = 0;
            for (const auto& m : out.modules) for (const auto& s : m.image.segments)
                highest = std::max(highest, add(add(m.bias, s.header.address), s.header.memorySize));
            constexpr uint64_t page = 0x4000;
            thunkBase = add(add(highest, page - 1) & ~(page - 1), page);
            const auto size = synthesized * thunkSize;
            require(add(thunkBase, size) <= (uint64_t{1} << 47), "non-canonical HLE trampoline address");
            Module thunks {"<hle>", thunkBase, 0, PS4ExecutableImage{}};
            thunks.image.type = 0xfe18;
            PS4ExecutableImage::Header header {1, 5, 0, 0, size, size, page};
            std::vector<uint8_t> code(static_cast<size_t>(size));
            size_t k = 0;
            for (const auto& h : hle) if (!h.guestAddress && !isData(h) && h.code.empty()) {
                const uint8_t body[thunkSize] = {0x49,0x89,0xca,0xb8,uint8_t(h.token),uint8_t(h.token>>8),
                    uint8_t(h.token>>16),uint8_t(h.token>>24),0x0f,0x05,uint8_t(h.noreturn ? 0xf4 : 0xc3)};
                std::copy(std::begin(body), std::end(body), code.begin() + k * thunkSize);
                out.thunks.push_back({thunkBase + k * thunkSize, h.token}); ++k;
            }
            thunks.image.headers.push_back(header);
            thunks.image.segments.push_back({header, std::move(code)});
            out.modules.push_back(std::move(thunks));
        }
        uint64_t codeBase = 0;
        if (codeBytes) {
            uint64_t highest = 0;
            for (const auto& m : out.modules) for (const auto& s : m.image.segments)
                highest = std::max(highest, add(add(m.bias, s.header.address), s.header.memorySize));
            constexpr uint64_t page = 0x4000;
            codeBase = add(add(highest, page - 1) & ~(page - 1), page);
            const auto size = (codeBytes + page - 1) & ~(page - 1);
            require(add(codeBase, size) <= (uint64_t{1} << 47), "non-canonical HLE code address");
            Module routines {"<hle-code>", codeBase, 0, PS4ExecutableImage{}};
            routines.image.type = 0xfe18;
            PS4ExecutableImage::Header header {1, 5, 0, 0, codeBytes, size, page};
            std::vector<uint8_t> bytes(static_cast<size_t>(size), 0xcc); // INT3 between routines.
            uint64_t offset = 0;
            for (const auto& h : hle) if (!h.code.empty()) {
                std::copy(h.code.begin(), h.code.end(), bytes.begin() + static_cast<long>(offset));
                offset += (h.code.size() + 15) & ~size_t{15};
            }
            routines.image.headers.push_back(header);
            routines.image.segments.push_back({header, std::move(bytes)});
            out.modules.push_back(std::move(routines));
        }
        uint64_t dataBase = 0;
        if (dataBytes) {
            uint64_t highest = 0;
            for (const auto& m : out.modules) for (const auto& s : m.image.segments)
                highest = std::max(highest, add(add(m.bias, s.header.address), s.header.memorySize));
            constexpr uint64_t page = 0x4000;
            dataBase = add(add(highest, page - 1) & ~(page - 1), page);
            const auto size = (dataBytes + page - 1) & ~(page - 1);
            require(add(dataBase, size) <= (uint64_t{1} << 47), "non-canonical HLE data address");
            Module data {"<hle-data>", dataBase, 0, PS4ExecutableImage{}};
            data.image.type = 0xfe18;
            PS4ExecutableImage::Header header {1, 6, 0, 0, 0, size, page};
            data.image.headers.push_back(header);
            data.image.segments.push_back({header, std::vector<uint8_t>(static_cast<size_t>(size), 0)});
            out.modules.push_back(std::move(data));
        }
        size_t k = 0;
        uint64_t dataOffset = 0, codeOffset = 0;
        for (const auto& h : hle) {
            uint64_t address = h.guestAddress;
            if (!h.code.empty()) {
                address = codeBase + codeOffset;
                codeOffset += (h.code.size() + 15) & ~size_t{15};
            } else if (!address && isData(h)) {
                address = dataBase + dataOffset;
                dataOffset += (uint64_t(h.token) + 15) & ~uint64_t{15};
                out.data.push_back({address, h.token, h.identity.nid});
            } else if (!address) {
                address = thunkBase + (k++) * thunkSize;
            }
            if (isData(h)) require(out.translate(address, h.token ? h.token : 1, 6), "HLE data object outside RW guest memory");
            else require(h.identity.type == 2 && out.translate(address, 1, 5), "HLE requires mapped executable guest trampoline");
            // HLE functions match any module version, as in shadPS4: games import
            // libkernel 1.1, libSceScreenShot 0.9, ... for the same functions.
            auto key = h.identity;
            key.moduleVersion = 0;
            if (exportKeys.empty())
                for (const auto& [identity, unused] : exports) {
                    (void)unused;
                    auto exported = identity;
                    exported.moduleVersion = 0;
                    exportKeys.insert(exported);
                }
            require(!exportKeys.count(key), "ambiguous HLE/export");
            require(hleExports.emplace(key, Export{address, SIZE_MAX}).second, "ambiguous HLE/export");
        }
        // The system's libSceLibcInternal exports libc's functions under their
        // libc NIDs (malloc, free, __cxa_atexit...); a game ships libc.prx, so
        // its "libc" exports serve the PRX modules that import them there.
        std::map<std::string, std::map<Identity, Export>::const_iterator> libc;
        for (auto it = exports.cbegin(); it != exports.cend(); ++it)
            if (it->first.library == "libc") libc.emplace(it->first.nid, it);
        // Resolve every import (including unreferenced/weak imports) before writes.
        std::vector<std::vector<uint64_t>> resolved(dynamic.size());
        std::vector<std::vector<size_t>> owners(dynamic.size()); // Defining module, for TLS module IDs.
        for (size_t i = 0; i < dynamic.size(); ++i) {
            for (size_t j = 0; j < dynamic[i].symbols.size(); ++j) {
                const auto& s = dynamic[i].symbols[j]; uint64_t address = 0; size_t definer = i;
                if (s.defined) address = s.type == 6 ? s.value : add(out.modules[i].bias, s.value);
                else if (j && !s.local) { // Local (section) symbols are never imported.
                    ++report.imports;
                    auto it = exports.find(s.identity);
                    // An alias link stands for the system module: libc.prx may
                    // itself depend on the importer (Fios2), so no declared
                    // dependency or load order is required for it.
                    bool aliased = false;
                    if (it == exports.end() && s.identity.library == "libSceLibcInternal") {
                        const auto alias = libc.find(s.identity.nid);
                        if (alias != libc.end() && alias->second->first.type == s.identity.type) {
                            it = exports.find(alias->second->first);
                            aliased = true;
                        }
                    }
                    if (it == exports.end()) {
                        auto key = s.identity;
                        key.moduleVersion = 0;
                        it = hleExports.find(key);
                        if (it == hleExports.end()) it = exports.end();
                    }
                    require(it != exports.end(), out.modules[i].path + ": unresolved NID " + s.identity.nid +
                            " library=" + s.identity.library + " module=" + s.identity.module);
                    const auto owner = it->second.owner;
                    if (owner != SIZE_MAX && owner != i && !aliased) {
                        const auto& deps = requests[order[i]].dependencies;
                        require(std::find(deps.begin(), deps.end(), out.modules[owner].path) != deps.end(), "import provider missing explicit dependency");
                        require(owner < i, "import provider not loaded first");
                    }
                    address = it->second.address; definer = owner; ++report.resolvedImports;
                }
                resolved[i].push_back(address); owners[i].push_back(definer);
            }
        }
        auto tlsBlock = [&](size_t module) -> const TLSBlock& {
            for (const auto& block : out.tls) if (block.module == module) return block;
            throw Failure(out.modules[module].path + ": TLS relocation without PT_TLS");
        };
        report.phase = "relocations";
        struct Write { uint8_t* target; uint64_t value; };
        std::vector<Write> writes;
        for (size_t i = 0; i < dynamic.size(); ++i) {
            std::set<uint64_t> targets;
            auto& m = out.modules[i];
            for (const auto& r : dynamic[i].relas) {
                const auto type = uint32_t(r.info); const auto symbol = r.info >> 32;
                // AMD64 R_X86_64_NONE has no effect: neither the target nor the
                // symbol/addend is used, and it cannot overlap a real write.
                if (type == 0) { ++report.ignoredRelocations; continue; }
                ++report.relocations;
                require(type == 1 || type == 6 || type == 7 || type == 8 || (type >= 16 && type <= 18),
                        "unsupported x86-64 relocation " + std::to_string(type));
                const bool tlsRelocation = type >= 16;
                // TLS relocations may use symbol 0 (local-dynamic: this module, offset = A).
                require(symbol < resolved[i].size() && (type != 8 || symbol == 0) && (type == 8 || tlsRelocation || symbol != 0),
                        "invalid relocation symbol");
                // An undefined local STT_SECTION symbol in a TLS relocation names this
                // module's TLS block (local-dynamic model), like symbol 0.
                const auto& referenced = dynamic[i].symbols[symbol];
                const bool sectionTLS = symbol && tlsRelocation && referenced.local && referenced.type == 3 && !referenced.defined;
                const bool tlsSymbol = symbol && (referenced.type == 6 || sectionTLS);
                require(tlsRelocation ? (!symbol || tlsSymbol) : !tlsSymbol, "TLS symbol/relocation kind mismatch: relocation " +
                        std::to_string(type) + " symbol " + std::to_string(symbol) + " type " +
                        std::to_string(symbol ? dynamic[i].symbols[symbol].type : 0) + " local " +
                        std::to_string(symbol ? dynamic[i].symbols[symbol].local : 0) + " nid " + (symbol ? dynamic[i].symbols[symbol].identity.nid : ""));
                auto* target = const_cast<uint8_t*>(m.image.translate(r.offset, 8, 6));
                require(target && !m.image.translate(r.offset, 8, 1), "relocation target outside RW/non-X guest segment");
                auto next = targets.lower_bound(r.offset);
                require((next == targets.end() || *next - r.offset >= 8) &&
                        (next == targets.begin() || r.offset - *std::prev(next) >= 8), "overlapping relocation writes");
                targets.insert(r.offset);
                // GLOB_DAT/JUMP_SLOT = S; 64 = S+A; RELATIVE = B+A (AMD64 ABI).
                // DTPMOD64 = module ID; DTPOFF64 = S+A in the block; TPOFF64 = S+A-offset,
                // a negative thread-pointer offset in variant II.
                uint64_t value;
                if (tlsRelocation) {
                    const auto& block = tlsBlock(symbol ? owners[i][symbol] : i);
                    const auto inBlock = signedAdd(symbol ? resolved[i][symbol] : 0, r.addend);
                    require(inBlock <= block.memorySize, "TLS offset outside module TLS block");
                    value = type == 16 ? block.id : type == 17 ? inBlock : inBlock - block.offset; // Intentional two's complement.
                } else {
                    value = type == 8 ? signedAdd(m.bias, r.addend) : type == 1 ? signedAdd(resolved[i][symbol], r.addend) : resolved[i][symbol];
                }
                writes.push_back({target, value}); ++report.validatedRelocations;
            }
        }
        report.phase = "memory permissions";
        for (const auto& w : writes) for (size_t i = 0; i < 8; ++i) w.target[i] = uint8_t(w.value >> (i * 8));
        report.phase = "initializers";
        // Initializer arrays are read after relocation (RELATIVE fills them).
        for (size_t i = 0; i < dynamic.size(); ++i) {
            const auto& d = dynamic[i]; const auto& m = out.modules[i];
            auto code = [&](uint64_t address, const char* what) {
                bool backed = false;
                if (address >= m.bias) for (const auto& s : m.image.segments) {
                    if ((s.header.flags & 5) != 5 || address - m.bias < s.header.address) continue;
                    backed |= address - m.bias - s.header.address < s.header.fileSize;
                }
                require(backed, m.path + ": " + what + " outside file-backed module code");
            };
            auto array = [&](uint64_t start, uint64_t size, const char* what) {
                if (!start) return;
                require(size % 8 == 0 && size / 8 <= 256, m.path + ": " + what + " size");
                const auto* entries = m.image.translate(start, static_cast<size_t>(size), 4);
                require(entries, m.path + ": " + what + " outside readable module memory");
                for (uint64_t k = 0; k < size; k += 8) {
                    const auto entry = read(entries + k, 8);
                    if (!entry || entry == UINT64_MAX) continue; // ELF sentinels
                    code(entry, what);
                }
            };
            if (d.hasInit) {
                code(add(m.bias, d.init), "DT_INIT");
                if (m.image.type == 0xfe18) out.initializers.push_back(add(m.bias, d.init)); // module_start
            }
            if (d.fini) code(add(m.bias, d.fini), "DT_FINI");
            array(d.initArray, d.initArraySize, "DT_INIT_ARRAY");
            array(d.finiArray, d.finiArraySize, "DT_FINI_ARRAY");
            require(out.initializers.size() <= 256, "initializer budget (256)");
        }
        // RELRO may be writable while linking, then becomes read-only for the backend.
        for (auto& m : out.modules) for (auto& s : m.image.segments)
            if (s.header.type == 0x61000010) s.header.flags &= ~2U;
    }
};
}
