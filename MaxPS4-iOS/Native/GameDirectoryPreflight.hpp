#pragma once
#include "OrbisMetadata.hpp"
#include "maxps4_fex_runtime.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <memory>
#include <tuple>

namespace maxps4 {
// Read-only, descriptor-relative inventory. Never follows links, interprets a
// license/key, extracts PFS, guesses load biases, or dispatches guest code.
class GameDirectoryPreflight {
    struct FD {
        int value;
        explicit FD(int v) : value(v) {}
        ~FD() { if (value >= 0) close(value); }
        FD(const FD&) = delete;
        FD& operator=(const FD&) = delete;
    };
    struct File { std::string path; uint64_t size; bool regular; };
    struct Module { std::string path; OrbisMetadata metadata; };
    struct Failure : std::runtime_error {
        int status;
        Failure(int s, const std::string& text) : std::runtime_error(text), status(s) {}
    };
    std::vector<File> files;
    std::vector<Module> modules;
    std::vector<std::string> blockers;
    size_t entries = 0, bytesRead = 0, records = 0;
    static constexpr size_t reportLimit = 2 * 1024 * 1024 - 1;
    std::string output;
    void append(const std::string& text) {
        if (text.size() > reportLimit - output.size()) throw Failure(-E2BIG,"Rapport supérieur à 2 Mio ; inventaire incomplet");
        output += text;
    }
    void block(const std::string& reason) { blockers.push_back(reason); }
    static std::string display(const std::string& text) {
        // Keep control characters and newlines in filenames from forging logs.
        std::string result;
        const char* hex = "0123456789abcdef";
        for (unsigned char c : text) {
            if (c >= 32 && c < 127 && c != '\\') result += char(c);
            else { result += "\\x"; result += hex[c >> 4]; result += hex[c & 15]; }
        }
        return result;
    }
    static bool executableName(const std::string& name) {
        auto lower = name;
        for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
        return lower == "eboot.bin" || suffix(lower,".elf") || suffix(lower,".prx") || suffix(lower,".sprx");
    }
    static bool suffix(const std::string& text, const std::string& end) {
        return text.size() >= end.size() && text.compare(text.size()-end.size(),end.size(),end)==0;
    }
    void inspect(int fd, const std::string& path, const struct stat& expected) {
        if (modules.size() == 128) throw Failure(-E2BIG,"Limite de 128 exécutables ; inventaire incomplet");
        Module module{path,{}};
        struct stat st{};
        if (fd < 0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_dev != expected.st_dev ||
            st.st_ino != expected.st_ino || st.st_size != expected.st_size) {
            module.metadata.error = "Fichier inaccessible ou modifié pendant l'analyse";
        } else if (st.st_size < 64 || uint64_t(st.st_size) > OrbisMetadata::fileLimit) {
            module.metadata.error = "Taille ELF invalide (64 octets à 512 Mio)";
        } else {
            if (uint64_t(st.st_size) > OrbisMetadata::fileLimit - bytesRead)
                throw Failure(-E2BIG,"Budget cumulé de lecture ELF/PRX supérieur à 512 Mio ; inventaire incomplet");
            std::vector<uint8_t> bytes(size_t(st.st_size));
            bytesRead += bytes.size();
            size_t done = 0;
            while (done < bytes.size()) {
                const auto n = read(fd,bytes.data()+done,bytes.size()-done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                done += size_t(n);
            }
            if (done != bytes.size()) module.metadata.error = "Lecture ELF/PRX incomplète";
            else module.metadata.parse(bytes.data(),bytes.size(),[](const auto& s) {
                return s.moduleVersion == 0x0100 && s.kind == 2 &&
                    maxps4_ps4_resolve_libkernel(s.nid.c_str(),s.library.c_str(),s.module.c_str(),s.libraryVersion) != 0;
            });
        }
        const auto count = module.metadata.symbols.size()+module.metadata.relocations.size();
        if (count > 65536-records) throw Failure(-E2BIG,"Budget cumulé de 65536 symboles/relocations dépassé ; inventaire incomplet");
        records += count;
        modules.push_back(std::move(module));
    }
    void walk(int fd, const std::string& prefix, size_t depth) {
        if (depth > 16) throw Failure(-E2BIG,"Profondeur supérieure à 16 ; inventaire incomplet");
        const int duplicate = dup(fd);
        if (duplicate < 0) throw Failure(-EIO,"Duplication du descripteur impossible");
        auto closeDirectory = [](DIR* p) { closedir(p); };
        std::unique_ptr<DIR,decltype(closeDirectory)> directory(fdopendir(duplicate),closeDirectory);
        if (!directory) { close(duplicate); throw Failure(-EIO,"Répertoire inaccessible"); }
        std::vector<std::string> names;
        errno = 0;
        while (auto* entry = readdir(directory.get())) {
            std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            if (++entries > 4096) throw Failure(-E2BIG,"Limite de 4096 entrées ; inventaire incomplet");
            names.push_back(std::move(name));
        }
        if (errno) throw Failure(-EIO,"Lecture du répertoire incomplète");
        std::sort(names.begin(),names.end());
        for (const auto& name : names) {
            const auto path = prefix + name;
            struct stat st{};
            if (fstatat(fd,name.c_str(),&st,AT_SYMLINK_NOFOLLOW)) {
                files.push_back({path,0,false}); block(display(path)+": entrée inaccessible"); continue;
            }
            if (S_ISDIR(st.st_mode)) {
                FD child(openat(fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
                struct stat current{};
                if (child.value < 0 || fstat(child.value,&current) || st.st_dev != current.st_dev || st.st_ino != current.st_ino)
                    throw Failure(-EIO,"Sous-répertoire inaccessible ou modifié : "+display(path));
                walk(child.value,path+"/",depth+1);
            } else {
                const bool regular = S_ISREG(st.st_mode);
                files.push_back({path,regular ? uint64_t(st.st_size) : 0,regular});
                if (!regular) { block(display(path)+": lien ou fichier spécial refusé"); continue; }
                if (executableName(name)) {
                    FD file(openat(fd,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
                    inspect(file.value,path,st);
                }
            }
        }
    }
    void describe() {
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){ return a.path < b.path; });
        std::sort(modules.begin(),modules.end(),[](const auto& a,const auto& b){ return a.path < b.path; });
        append("MaxPS4 pré-démarrage dossier v1\nLecture seule ; aucun fichier copié ou modifié ; aucune exécution\n"
               "Limites : 4096 entrées, profondeur 16, 128 ELF/PRX, 65536 symboles/relocations, lecture cumulée 512 Mio, rapport 2 Mio\n");
        bool eboot = false, sfo = false, manifest = false;
        for (const auto& file : files) {
            append("Fichier "+display(file.path)+" ; octets="+std::to_string(file.size)+(file.regular ? "\n" : " ; REFUSÉ\n"));
            eboot |= file.path == "eboot.bin" && file.regular;
            sfo |= file.path == "sce_sys/param.sfo" && file.regular;
            manifest |= file.path == "maxps4.json" && file.regular;
            if (suffix(file.path,".pkg")) block(display(file.path)+": contenu non accessible par ce flux (PKG/PFS non extrait, aucun déchiffrement)");
        }
        if (!eboot) blockers.insert(blockers.begin(),"eboot.bin manquant à la racine (aucune recherche par nom de jeu)");
        append(sfo ? "SFO : sce_sys/param.sfo présent ; non analysé par ce flux\n" : "SFO : sce_sys/param.sfo manquant ; identité/version du jeu non vérifiées\n");
        for (const auto& module : modules) {
            append("\n=== "+display(module.path)+" ===\n"+module.metadata.report());
            if (!module.metadata.error.empty()) block(display(module.path)+": "+module.metadata.error);
            for (const auto& reason : module.metadata.blockers) block(display(module.path)+": "+reason);
        }
        // Availability is separate from binding: report exact metadata matches,
        // but never invent dependency edges or silently register HLE addresses.
        append("\nDépendances déclarées et services absents (inventaire, aucune liaison)\n");
        using Key = std::tuple<std::string,std::string,std::string,uint16_t,uint16_t,uint8_t>;
        auto key = [](const OrbisMetadata::Symbol& s) { return Key{s.nid,s.library,s.module,s.libraryVersion,s.moduleVersion,s.kind}; };
        std::map<Key,size_t> providers;
        using Descriptor = std::pair<std::string,uint16_t>;
        std::map<Descriptor,size_t> exportedModules, exportedLibraries;
        for (const auto& module : modules) {
            for (const auto& id : module.metadata.modules) if (id.exported) ++exportedModules[{id.name,id.version}];
            for (const auto& id : module.metadata.libraries) if (id.exported) ++exportedLibraries[{id.name,id.version}];
        }
        for (const auto& module : modules) for (const auto& s : module.metadata.symbols)
            if (s.qualified && s.defined && s.binding != 0 && s.visibility == 0) ++providers[key(s)];
        for (const auto& module : modules) {
            const auto& metadata = module.metadata;
            auto descriptors = [&](const auto& required, const auto& available, const std::string& kind) {
                for (const auto& id : required) if (!id.exported) {
                    const auto provider = available.find({id.name,id.version});
                    const auto count = provider == available.end() ? 0 : provider->second;
                    const auto reason = display(module.path)+": "+kind+" "+id.name+" version="+std::to_string(id.version)+
                        " ; fournisseurs déclarés="+std::to_string(count);
                    append(reason+(count == 1 ? " ; non lié\n" : " ; absent ou ambigu\n"));
                    if (count != 1) block(reason+" ; absent ou ambigu");
                }
            };
            descriptors(metadata.modules,exportedModules,"module PRX requis");
            descriptors(metadata.libraries,exportedLibraries,"bibliothèque PRX requise");
            for (const auto& needed : metadata.needed) {
                const auto parent = module.path.substr(0,module.path.find_last_of('/') == std::string::npos ? 0 : module.path.find_last_of('/')+1);
                const bool found = std::any_of(files.begin(),files.end(),[&](const auto& f){ return f.regular && f.path == parent+needed; });
                append(display(module.path)+": DT_NEEDED "+needed+(found ? " ; fichier présent, non lié\n" : " ; fichier adjacent absent (aucun chemin deviné)\n"));
            }
            for (const auto& symbol : metadata.symbols) {
                if (symbol.defined || symbol.encoded.empty()) continue;
                const auto candidate = providers.find(key(symbol));
                const size_t candidates = symbol.qualified && candidate != providers.end() ? candidate->second : 0;
                append(display(module.path)+": "+symbol.encoded+" ; fournisseurs PRX déclarés="+std::to_string(candidates)+
                    (symbol.resolved ? " ; service HLE reconnu, trampoline non lié\n" : candidates == 1 ? " ; export candidat, non lié\n" : " ; service/import absent ou ambigu\n"));
            }
        }
        append(manifest ? "\nmaxps4.json présent : validation du graphe, biais et instructions réservée au lancement expérimental\n" :
            "\nmaxps4.json absent : graphe PRX et biais explicites requis par le runtime actuel ; aucun manifeste généré\n");
        block("Préparation réelle non effectuée : graphe explicite, biais, relocations et admission CPU à valider");
        block("TLS par thread et ABI de démarrage d'un jeu non raccordés ; rendu/audio PS4 général non connecté");
        append("\nPremier blocage du précontrôle : "+blockers.front()+"\nBlocages / limites observés :\n");
        for (const auto& reason : blockers) append("• "+reason+"\n");
        append("Imports HLE reconnus et fichiers présents ne prouvent pas une liaison ni un démarrage.\n"
               "FEXCore JIT ARM64 : non tenté par le précontrôle ; aucun résultat Kero Blaster ou écran titre validé.\n");
    }
public:
    static int report(const char* path, std::string& text) {
        try {
            if (!path) throw Failure(-EINVAL,"Chemin du dossier absent");
            FD root(open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
            if (root.value < 0) throw Failure(-EACCES,"Dossier inaccessible ou lien symbolique refusé");
            GameDirectoryPreflight pending;
            pending.walk(root.value,"",0);
            pending.describe();
            text = std::move(pending.output); return 0;
        } catch (const Failure& e) { text = "MaxPS4 pré-démarrage dossier v1\nRejet : "+std::string(e.what())+"\nAucune exécution ; inventaire non validé\n"; return e.status; }
          catch (const std::bad_alloc&) { text = "Allocation du diagnostic impossible ; aucune exécution"; return -ENOMEM; }
          catch (const std::exception&) { text = "Erreur interne du diagnostic ; aucune exécution"; return -EIO; }
    }
};
}
