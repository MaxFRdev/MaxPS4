// Which PRX files a game folder provides at boot (issue #296, MK Trilogy).
//
// A PS4 game ships system-replacement modules in /app0/sce_module, and may
// ship its own modules at the root of /app0, loaded later by path:
// Mortal Kombat: Legacy Kollection calls
// sceKernelLoadStartModule("/app0/pancake_libretro_ps4.prx") and then
// sceKernelDlsym for its retro_* functions. MaxPS4 prepares every module at
// boot (the HLE LoadStartModule returns the prepared module of that file
// name), so a root-level PRX that is not prepared makes LoadStartModule fail
// and leaves the game with null function pointers.
//
// Rules:
//  * Only regular *.prx files directly in /app0 and in /app0/sce_module (no
//    recursion into other subfolders).
//  * Modules are identified by file name. Within one folder, sce_module wins
//    over a root-level file of the same name; the update folder replaces the
//    base game's modules of the same name and adds its own.
//  * root_level marks modules that came from the root of /app0 (base or
//    update): the loader treats them as optional (see the runtime).
//  * The result is sorted by name. The Orbis linker orders modules by their
//    dependencies itself (PS4PKGRuntimePlan::orderModules).
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace maxps4 {

struct AppModuleFile {
    std::string name;            // file name: the module identity
    std::filesystem::path path;
    bool root_level = false;     // from the root of /app0, not sce_module
};

inline std::vector<AppModuleFile> discover_app_modules(const std::filesystem::path& root,
                                                       const std::filesystem::path& update) {
    std::map<std::string, AppModuleFile> found;
    auto scan = [&](const std::filesystem::path& folder, bool root_level) {
        std::error_code missing;
        for (std::filesystem::directory_iterator it(folder, missing), end; !missing && it != end; it.increment(missing)) {
            std::error_code kind;
            if (it->path().extension() != ".prx" || !it->is_regular_file(kind)) continue;
            const std::string name = it->path().filename().string();
            found[name] = AppModuleFile{name, it->path(), root_level};
        }
    };
    for (const auto& folder : {root, update}) {
        if (folder.empty()) continue;
        scan(folder, true);
        scan(folder / "sce_module", false);
    }
    std::vector<AppModuleFile> out;
    out.reserve(found.size());
    for (auto& [name, file] : found) out.push_back(std::move(file));
    return out;
}

// Variants of one module. MK Legacy Kollection ships
// libmipsaot-SLUS00330_TRILOGY.EXE_ps4.prx and libmipsaot-SLUS00824_ps4.prx:
// both provide module "libmipsaot" with the same exports, one per PS1 title,
// and the game loads the one it needs by path (sceKernelLoadStartModule) and
// reaches its exports only through sceKernelDlsym, by handle.
//
// Decisions, for optional (root-level) modules only:
//  * keep: no other module provides the same module name.
//  * private_exports: two or more optional modules provide the same module and
//    no other kept module imports a library they export. All are prepared;
//    their exports serve sceKernelDlsym only, not NID imports
//    (OrbisLinker::Input::privateExports), so they cannot collide. The game's
//    own path then selects the variant.
//  * skip: a required module (sce_module, eboot) provides the same module, or
//    another module imports a library of the variants (a NID import could
//    not tell them apart). Such modules are not prepared, as before #307.
// Required modules are always kept; two required providers are left to the
// linker, which refuses duplicate exports as before.
struct ModuleIdentity {
    std::string file;      // file name
    std::string provided;  // name of the module it provides (empty: unknown)
    bool optional = false;
    std::vector<std::string> exported_libraries, imported_libraries;
};
enum class ModuleChoice { keep, private_exports, skip };
struct ModuleDecision { ModuleChoice choice = ModuleChoice::keep; std::string reason; };
inline std::vector<ModuleDecision> choose_modules(const std::vector<ModuleIdentity>& modules) {
    std::vector<ModuleDecision> out(modules.size());
    std::map<std::string, std::string> required; // provided module -> file
    std::map<std::string, std::vector<size_t>> groups; // optional, by provided module
    for (size_t i = 0; i < modules.size(); ++i) {
        const auto& m = modules[i];
        if (m.provided.empty()) continue;
        if (!m.optional) required.emplace(m.provided, m.file);
        else groups[m.provided].push_back(i);
    }
    for (const auto& [provided, members] : groups) {
        const auto owner = required.find(provided);
        if (owner != required.end()) {
            for (auto i : members) out[i] = {ModuleChoice::skip, "same module as " + owner->second};
            continue;
        }
        if (members.size() < 2) continue; // keep
        bool imported = false;
        std::string importer;
        for (size_t j = 0; j < modules.size() && !imported; ++j) {
            if (modules[j].optional && modules[j].provided == provided) continue;
            for (const auto& library : modules[j].imported_libraries)
                for (auto i : members)
                    for (const auto& exported : modules[i].exported_libraries)
                        if (library == exported) { imported = true; importer = modules[j].file; }
        }
        for (auto i : members)
            out[i] = imported ? ModuleDecision{ModuleChoice::skip, "variant of " + provided + " imported by " + importer}
                              : ModuleDecision{ModuleChoice::private_exports, {}};
    }
    return out;
}

} // namespace maxps4
