// discover_app_modules (issue #296, MK Trilogy): which PRX files a game folder
// provides at boot. Synthetic folders only; no game files. The runtime then
// skips root-level files the Orbis metadata parser rejects (checked below).
#include "maxps4_app_modules.h"
#include "OrbisMetadata.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
using maxps4::AppModuleFile;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static void touch(const fs::path& p, const std::string& text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static const AppModuleFile* find(const std::vector<AppModuleFile>& v, const std::string& name) {
    for (const auto& m : v) if (m.name == name) return &m;
    return nullptr;
}

int main() {
    const fs::path base = fs::temp_directory_path() /
        ("maxps4-app-modules-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path game = base / "game", update = base / "update";

    // 1. Only sce_module PRX files: same set as before, none root-level.
    touch(game / "eboot.bin");
    touch(game / "sce_module" / "libc.prx");
    touch(game / "sce_module" / "libSceFios2.prx");
    auto v = maxps4::discover_app_modules(game, {});
    CHECK(v.size() == 2 && v[0].name == "libSceFios2.prx" && v[1].name == "libc.prx");
    CHECK(!v[0].root_level && !v[1].root_level);

    // 2. One root-level PRX (MK Legacy Kollection's libretro core).
    touch(game / "pancake_libretro_ps4.prx");
    v = maxps4::discover_app_modules(game, {});
    const auto* pancake = find(v, "pancake_libretro_ps4.prx");
    CHECK(v.size() == 3 && pancake && pancake->root_level && pancake->path == game / "pancake_libretro_ps4.prx");

    // 3. Several root-level PRX files; nothing else and no recursion: other
    //    extensions, the eboot, a directory named *.prx and PRX files in other
    //    subfolders are ignored.
    touch(game / "libmipsaot-SLUS00330_TRILOGY.EXE_ps4.prx");
    touch(game / "Party.prx");
    touch(game / "imgui_p.sb");
    touch(game / "notes.PRX.txt");
    fs::create_directories(game / "folder.prx");
    touch(game / "assets" / "nested.prx");
    touch(game / "sce_module" / "deeper" / "nested2.prx");
    v = maxps4::discover_app_modules(game, {});
    CHECK(v.size() == 5);
    CHECK(find(v, "libmipsaot-SLUS00330_TRILOGY.EXE_ps4.prx") && find(v, "Party.prx"));
    CHECK(!find(v, "folder.prx") && !find(v, "nested.prx") && !find(v, "nested2.prx") && !find(v, "imgui_p.sb"));
    for (size_t i = 1; i < v.size(); ++i) CHECK(v[i - 1].name < v[i].name); // sorted, unique

    // Same name at the root and in sce_module of one folder: sce_module wins.
    touch(game / "libc.prx");
    v = maxps4::discover_app_modules(game, {});
    const auto* libc = find(v, "libc.prx");
    CHECK(v.size() == 5 && libc && !libc->root_level && libc->path == game / "sce_module" / "libc.prx");

    // 4. The update replaces the game's module of the same name (root-level
    //    or sce_module) and adds its own.
    touch(update / "pancake_libretro_ps4.prx");
    touch(update / "sce_module" / "libSceFios2.prx");
    touch(update / "extra_update.prx");
    v = maxps4::discover_app_modules(game, update);
    CHECK(v.size() == 6);
    pancake = find(v, "pancake_libretro_ps4.prx");
    CHECK(pancake && pancake->root_level && pancake->path == update / "pancake_libretro_ps4.prx");
    const auto* fios = find(v, "libSceFios2.prx");
    CHECK(fios && !fios->root_level && fios->path == update / "sce_module" / "libSceFios2.prx");
    const auto* extra = find(v, "extra_update.prx");
    CHECK(extra && extra->root_level);
    // An update module of the same name in sce_module replaces a game root-level one.
    touch(update / "sce_module" / "Party.prx");
    v = maxps4::discover_app_modules(game, update);
    const auto* party = find(v, "Party.prx");
    CHECK(party && !party->root_level && party->path == update / "sce_module" / "Party.prx");

    // 5. Missing folders: nothing found, no exception.
    CHECK(maxps4::discover_app_modules(base / "absent", base / "absent-too").empty());
    CHECK(maxps4::discover_app_modules({}, {}).empty());

    // 6. An invalid PRX file is discovered (by name) but the metadata parser
    //    rejects it, so the runtime skips it when it is root-level.
    {
        const uint8_t garbage[] = {'n', 'o', 't', ' ', 'a', 'n', ' ', 'e', 'l', 'f'};
        maxps4::OrbisMetadata metadata;
        CHECK(!metadata.parse(garbage, sizeof garbage) && !metadata.error.empty());
        maxps4::OrbisMetadata empty;
        CHECK(!empty.parse(nullptr, 0));
    }

    // 7. Variants of one module (MK's two libmipsaot PRX), decided by what
    //    imports them, never by name order.
    {
        using maxps4::ModuleIdentity;
        using maxps4::ModuleChoice;
        // Real shape of CUSA53178: the eboot imports Party but nothing imports
        // libmipsaot or pancake (reached through Dlsym only).
        const std::vector<ModuleIdentity> mk = {
            {"Party.prx", "Party", true, {"Party"}, {"libkernel"}},
            {"libc.prx", "libc", false, {"libc"}, {"libkernel"}},
            {"libmipsaot-SLUS00330_TRILOGY.EXE_ps4.prx", "libmipsaot", true, {"libmipsaot"}, {"libkernel", "libc"}},
            {"libmipsaot-SLUS00824_ps4.prx", "libmipsaot", true, {"libmipsaot"}, {"libkernel", "libc"}},
            {"pancake_libretro_ps4.prx", "pancake_libretro_ps4", true, {"pancake_libretro_ps4"}, {"libkernel", "libc"}},
            {"eboot.bin", "", false, {}, {"libkernel", "Party", "libc"}},
        };
        auto d = maxps4::choose_modules(mk);
        CHECK(d.size() == mk.size());
        CHECK(d[0].choice == ModuleChoice::keep && d[1].choice == ModuleChoice::keep);
        CHECK(d[2].choice == ModuleChoice::private_exports && d[3].choice == ModuleChoice::private_exports);
        CHECK(d[4].choice == ModuleChoice::keep && d[5].choice == ModuleChoice::keep);
        // Same decision whatever the order of the entries (no name order).
        std::vector<ModuleIdentity> reversed(mk.rbegin(), mk.rend());
        d = maxps4::choose_modules(reversed);
        CHECK(d[2].choice == ModuleChoice::private_exports && d[3].choice == ModuleChoice::private_exports);
        // If another module imports a library of the variants, a NID import
        // could not tell them apart: none is prepared.
        auto imported = mk; imported[5].imported_libraries.push_back("libmipsaot");
        d = maxps4::choose_modules(imported);
        CHECK(d[2].choice == ModuleChoice::skip && d[3].choice == ModuleChoice::skip);
        CHECK(d[2].reason.find("eboot.bin") != std::string::npos);
        CHECK(d[0].choice == ModuleChoice::keep && d[4].choice == ModuleChoice::keep);
        // A variant importing its own library does not count as an importer.
        auto self = mk; self[2].imported_libraries.push_back("libmipsaot");
        d = maxps4::choose_modules(self);
        CHECK(d[2].choice == ModuleChoice::private_exports && d[3].choice == ModuleChoice::private_exports);
        // A root-level file cannot replace a required module of the same name.
        d = maxps4::choose_modules({{"a_libc_copy.prx", "libc", true, {"libc"}, {}}, {"libc.prx", "libc", false, {"libc"}, {}}});
        CHECK(d[0].choice == ModuleChoice::skip && d[0].reason == "same module as libc.prx" && d[1].choice == ModuleChoice::keep);
        // Two required providers: both kept (the linker decides, as before).
        d = maxps4::choose_modules({{"x.prx", "same", false, {}, {}}, {"y.prx", "same", false, {}, {}}});
        CHECK(d[0].choice == ModuleChoice::keep && d[1].choice == ModuleChoice::keep);
        // Unknown providers and single optional modules are kept as they are.
        d = maxps4::choose_modules({{"a.prx", "", true, {}, {}}, {"b.prx", "", true, {}, {}}, {"c.prx", "c", true, {"c"}, {}}});
        CHECK(d[0].choice == ModuleChoice::keep && d[1].choice == ModuleChoice::keep && d[2].choice == ModuleChoice::keep);
    }

    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::puts("PASS app modules: sce_module only, root-level PRX, several, update precedence, missing folders, invalid file, module variants");
    return 0;
}
