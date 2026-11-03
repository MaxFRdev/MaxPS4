#pragma once
#include "PS4PKGInventory.hpp"
#include "PS4ExecutableImage.hpp"
#include <algorithm>

namespace maxps4 {
// A display-only package reference never grants access to executable bytes.
// A future provider must supply legally accessible standalone plaintext files.
// This adapter stages with the existing Orbis ELF/PRX loader; it never executes,
// decrypts, resolves NIDs, relocates, or touches host executable memory.
class PS4PKGRuntimePlan {
public:
    enum class Kind { executable, module, metadata };
    struct Reference { std::string path; Kind kind; PS4PKGInventory::Access descriptorAccess; };
    std::vector<Reference> references;
    static constexpr size_t maxDependencies = 128;
    static constexpr size_t batchMemoryBudget = 512 * 1024 * 1024;
    void inventory(const PS4PKGInventory& pkg, const std::vector<std::string>& names) {
        references.clear();
        if (names.size() != pkg.entries.size()) return;
        for (size_t i = 0; i < names.size(); ++i) {
            std::string name = names[i];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
            });
            const auto slash = name.find_last_of('/');
            const auto base = name.substr(slash == std::string::npos ? 0 : slash + 1);
            if (base == "eboot.bin") references.push_back({names[i], Kind::executable, pkg.entries[i].access});
            else if (ends(name, ".prx") || ends(name, ".sprx")) references.push_back({names[i], Kind::module, pkg.entries[i].access});
            else if (base == "param.sfo") references.push_back({names[i], Kind::metadata, pkg.entries[i].access});
        }
    }
    struct PreparedModule {
        std::string logicalPath;
        PS4ExecutableImage image;
        // Explicit dependency requests from a future validated Orbis import reader.
        // No dependencies are inferred from arbitrary strings or PKG descriptors.
        std::vector<std::string> dependencies;
    };
    // Validate a closed, explicitly supplied module graph before any ELF staging.
    // Order is dependency-first; this does not resolve imports or execute code.
    struct ModuleRequest {
        std::string logicalPath;
        std::vector<std::string> dependencies;
    };
    static bool orderModules(const std::vector<ModuleRequest>& modules,
                             std::vector<size_t>& order, std::string& diagnostic) {
        order.clear();
        diagnostic.clear();
        if (modules.empty() || modules.size() > maxDependencies) {
            diagnostic = "Modules : nombre invalide (1 à 128 requis)"; return false;
        }
        for (size_t i = 0; i < modules.size(); ++i) {
            const auto& module = modules[i];
            if (!safePath(module.logicalPath) || module.dependencies.size() > maxDependencies) {
                diagnostic = "Modules : chemin invalide ou trop de dépendances"; return false;
            }
            for (size_t j = 0; j < i; ++j) {
                if (modules[j].logicalPath == module.logicalPath) {
                    diagnostic = "Modules : chemin dupliqué"; return false;
                }
            }
        }
        std::vector<std::vector<size_t>> edges(modules.size());
        for (size_t i = 0; i < modules.size(); ++i) {
            for (const auto& dependency : modules[i].dependencies) {
                if (!safePath(dependency) || dependency == modules[i].logicalPath) {
                    diagnostic = "Modules : dépendance invalide ou réflexive"; return false;
                }
                size_t found = modules.size();
                for (size_t j = 0; j < modules.size(); ++j)
                    if (modules[j].logicalPath == dependency) { found = j; break; }
                if (found == modules.size()) {
                    diagnostic = "Modules : dépendance manquante : " + dependency; return false;
                }
                if (std::find(edges[i].begin(), edges[i].end(), found) != edges[i].end()) {
                    diagnostic = "Modules : dépendance dupliquée"; return false;
                }
                edges[i].push_back(found);
            }
        }
        std::vector<uint8_t> state(modules.size(), 0);
        std::vector<size_t> pending;
        pending.reserve(modules.size());
        // Iterative DFS avoids recursion and returns a deterministic dependency-first order.
        for (size_t root = 0; root < modules.size(); ++root) {
            if (state[root]) continue;
            std::vector<std::pair<size_t, size_t>> stack{{root, 0}};
            state[root] = 1;
            while (!stack.empty()) {
                auto& frame = stack.back();
                const size_t node = frame.first;
                if (frame.second == edges[node].size()) {
                    state[node] = 2;
                    pending.push_back(node);
                    stack.pop_back();
                    continue;
                }
                const size_t next = edges[node][frame.second++];
                if (state[next] == 1) {
                    diagnostic = "Modules : dépendance circulaire"; return false;
                }
                if (state[next] == 0) {
                    state[next] = 1;
                    stack.push_back({next, 0});
                }
            }
        }
        order = std::move(pending);
        diagnostic = "Graphe valide ; ordre de préparation établi, aucune exécution";
        return true;
    }
    // Caller-owned plaintext buffers only. Transactional: a failed module leaves
    // no partially staged batch; no PKG/PFS reads, relocations or execution.
    struct ModuleBytes {
        ModuleRequest request;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
    };
    static bool prepareModules(const std::vector<ModuleBytes>& inputs,
                               std::vector<PreparedModule>& output,
                               std::string& diagnostic) {
        output.clear();
        diagnostic.clear();
        std::vector<ModuleRequest> requests;
        requests.reserve(inputs.size());
        for (const auto& input : inputs) requests.push_back(input.request);
        std::vector<size_t> order;
        if (!orderModules(requests, order, diagnostic)) return false;
        std::vector<PreparedModule> pending;
        pending.reserve(inputs.size());
        size_t stagedBytes = 0;
        for (size_t index : order) {
            PreparedModule module;
            if (!prepareStandalone(inputs[index].request.logicalPath,
                                   inputs[index].bytes, inputs[index].size,
                                   inputs[index].request.dependencies, module, diagnostic)) {
                diagnostic = "Modules : " + inputs[index].request.logicalPath + " : " + diagnostic;
                return false;
            }
            for (const auto& segment : module.image.segments) {
                if (segment.bytes.size() > batchMemoryBudget - stagedBytes) {
                    diagnostic = "Modules : budget mémoire total de 512 Mio dépassé";
                    return false;
                }
                stagedBytes += segment.bytes.size();
            }
            pending.push_back(std::move(module));
        }
        output = std::move(pending);
        diagnostic = "Modules ELF/PRX préparés en ordre de dépendance ; imports non résolus, aucune exécution";
        return true;
    }
    // Lookup only within an already validated, staged batch. The address is
    // module-local: no cross-module address-space mapping or NID resolution.
    static const uint8_t* translateModule(const std::vector<PreparedModule>& modules,
                                          const std::string& logicalPath,
                                          uint64_t address, size_t count,
                                          uint32_t permissions) {
        if (!safePath(logicalPath)) return nullptr;
        const PreparedModule* found = nullptr;
        for (const auto& module : modules) {
            if (module.logicalPath != logicalPath) continue;
            // Reject ambiguous caller-constructed batches, even though
            // prepareModules itself already rejects duplicate paths.
            if (found) return nullptr;
            found = &module;
        }
        return found ? found->image.translate(address, count, permissions) : nullptr;
    }
    static bool prepareStandalone(const std::string& logicalPath, const uint8_t* bytes, size_t size,
                                  const std::vector<std::string>& dependencies, PreparedModule& output,
                                  std::string& diagnostic) {
        output = PreparedModule{}; diagnostic.clear();
        if (!safePath(logicalPath) || dependencies.size() > maxDependencies) {
            diagnostic = "Module : chemin relatif invalide ou plus de 128 dépendances"; return false;
        }
        std::vector<std::string> seen;
        for (const auto& dependency : dependencies) {
            if (!safePath(dependency) || dependency == logicalPath ||
                std::find(seen.begin(), seen.end(), dependency) != seen.end()) {
                diagnostic = "Module : dépendance invalide, réflexive ou dupliquée"; return false;
            }
            seen.push_back(dependency);
        }
        // Require Orbis ELF, not SELF or CNT. No protected container processing.
        if (!bytes || size < 4 || std::memcmp(bytes, "\177ELF", 4)) {
            diagnostic = "Module : ELF Orbis autonome non chiffré requis ; PKG/SELF refusé"; return false;
        }
        PreparedModule pending;
        const int result = pending.image.load(bytes, size);
        if (result) {
            diagnostic = "Module : rejet du chargeur Orbis ELF/PRX (code " + std::to_string(result) + ")";
            return false;
        }
        pending.logicalPath = logicalPath;
        pending.dependencies = dependencies;
        output = std::move(pending);
        diagnostic = "Segments préparés en mémoire ; dépendances non résolues, aucune exécution";
        return true;
    }
private:
    static bool ends(const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    static bool safePath(const std::string& path) {
        if (path.empty() || path.size() > 255 || path.front() == '/' || path.find('\\') != std::string::npos ||
            path.find(':') != std::string::npos) return false;
        size_t start = 0;
        while (start < path.size()) {
            const size_t end = path.find('/', start);
            const auto component = path.substr(start, end == std::string::npos ? end : end - start);
            if (component.empty() || component == "." || component == "..") return false;
            for (unsigned char c : component) if (c < 0x20 || c > 0x7e) return false;
            if (end == std::string::npos) return true;
            start = end + 1;
        }
        return false;
    }
};
}
