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

// Variants of one module cannot share a boot-time image. MK Legacy Kollection
// ships libmipsaot-SLUS00330_TRILOGY.EXE_ps4.prx and libmipsaot-SLUS00824_ps4.prx:
// both provide module "libmipsaot" with the same exports, and the game loads
// only the one of the title it starts. Preparing both is refused by the linker
// (duplicate export). Required modules (sce_module, eboot) are kept first; then
// each optional module is kept unless a module already kept provides the same
// module name. For each entry: empty when kept, else the file it duplicates.
struct ModuleIdentity {
    std::string file;      // file name
    std::string provided;  // name of the module it provides (empty: unknown)
    bool optional = false;
};
inline std::vector<std::string> duplicate_providers(const std::vector<ModuleIdentity>& modules) {
    std::vector<std::string> out(modules.size());
    std::map<std::string, std::string> kept; // provided module -> file
    for (const bool optional : {false, true})
        for (size_t i = 0; i < modules.size(); ++i) {
            const auto& m = modules[i];
            if (m.optional != optional || m.provided.empty()) continue;
            const auto found = kept.find(m.provided);
            if (found == kept.end()) kept.emplace(m.provided, m.file);
            else if (optional) out[i] = found->second;
            // Two required providers: left to the linker, as before.
        }
    return out;
}

} // namespace maxps4
