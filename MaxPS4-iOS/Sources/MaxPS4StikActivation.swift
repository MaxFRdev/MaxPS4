import Foundation
import UIKit
import Darwin
import MachO

@MainActor
enum MaxPS4StikActivation {
    // Target the actual host executable when LiveContainer virtualizes Bundle.main.
    // This does not change the process permissions or grant JIT.
    private static func processIdentity() -> (bundleID: String, pid: Int32)? {
        let pid = getpid()
        guard pid > 0 else { return nil }
        // _NSGetExecutablePath is available on iOS; proc_pidpath is not
        // exposed by the iOS SDK used by the CI toolchain.
        var capacity: UInt32 = 0
        _ = _NSGetExecutablePath(nil, &capacity)
        guard capacity > 0, capacity < 65536 else { return nil }
        var path = [CChar](repeating: 0, count: Int(capacity))
        guard _NSGetExecutablePath(&path, &capacity) == 0 else { return nil }
        let executable = URL(fileURLWithPath: String(cString: path)).resolvingSymlinksInPath()
        let appURL = executable.deletingLastPathComponent()
        guard appURL.pathExtension == "app",
              let host = Bundle(url: appURL),
              let hostID = host.bundleIdentifier, !hostID.isEmpty,
              let declaredExecutable = host.object(forInfoDictionaryKey: "CFBundleExecutable") as? String,
              executable.lastPathComponent == declaredExecutable else { return nil }
        return (hostID, pid)
    }
    static func begin(completion: @escaping (String) -> Void) -> String {
#if MAXPS4_FEX_APP
        // LiveContainer may use the host app URL-query allowlist.
        // A failed canOpenURL query does not prove StikDebug is absent.
        guard let identity = processIdentity() else {
            return "Impossible d’identifier le processus hôte : activation JIT refusée."
        }
        guard let installedURL = URL(string: "stikdebug://") else {
            return "URL StikDebug invalide : activation JIT refusée."
        }
        guard UIApplication.shared.canOpenURL(installedURL) else {
            let visibleBundle = Bundle.main.bundleIdentifier ?? "inconnu"
            return "Schéma stikdebug:// inaccessible depuis ce processus. Hôte : \(identity.bundleID), PID : \(identity.pid), Bundle.main : \(visibleBundle). Vérifier LSApplicationQueriesSchemes dans l’application hôte LiveContainer et la présence de StikDebug. Aucune demande JIT envoyée."
        }

        guard let sourceURL = Bundle.main.url(forResource: "maxps4-stik", withExtension: "js") ??
                  Bundle.main.url(forResource: "maxps4-stik", withExtension: "js", subdirectory: "JIT"),
              let source = try? String(contentsOf: sourceURL, encoding: .utf8) else {
            return String(localized: "Script MaxPS4 manquant : activation refusée.", bundle: .maxps4)
        }
        var mailbox: UInt64 = 0, nonce: UInt64 = 0, page: UInt64 = 0
        let status = maxps4_jit_begin(&mailbox, &nonce, &page)
        guard status == 0 else {
            return String(localized: "Demande JIT refusée (\(status)). Vérifie l’activation ; si une ancienne session attend, termine StikDebug ou redémarre MaxPS4.", bundle: .maxps4)
        }
        guard let url = MaxPS4StikRequest.url(bundleID: identity.bundleID, pid: identity.pid,
            mailbox: mailbox, nonce: nonce, pageSize: page, script: source) else {
            _ = maxps4_jit_launch_failed(nonce)
            return String(localized: "URL StikDebug invalide : JIT indisponible.", bundle: .maxps4)
        }
        UIApplication.shared.open(url, options: [:]) { opened in
            if !opened { _ = maxps4_jit_launch_failed(nonce) }
            completion(opened ? String(localized: "Termine l’activation dans StikDebug, puis reviens dans MaxPS4.", bundle: .maxps4) :
                String(localized: "Ouverture de StikDebug refusée ; JIT indisponible. Réessayer l’activation.", bundle: .maxps4))
        }
        return String(localized: "Termine l’activation dans StikDebug, puis reviens dans MaxPS4.", bundle: .maxps4)
#else
        return String(localized: "Cette configuration ne contient pas le fournisseur FEXCore.", bundle: .maxps4)
#endif
    }
    static func confirmation() -> String {
#if MAXPS4_FEX_APP
        switch maxps4_jit_confirmation() {
        case 1: return String(localized: "Activation confirmée. Vérification du JIT…", bundle: .maxps4)
        case 2: return String(localized: "StikDebug a signalé un échec. Consulte les détails de l’activation.", bundle: .maxps4)
        default: return String(localized: "Aucune confirmation StikDebug pour cette demande. JIT indisponible. Vérifier la fin du script, le pairing, le VPN et la signature get-task-allow dans StikDebug. Si le script est ignoré, son option officielle de détection des scripts peut être nécessaire.", bundle: .maxps4)
        }
#else
        return String(localized: "Fournisseur absent.", bundle: .maxps4)
#endif
    }
    static func release() -> String {
#if MAXPS4_FEX_APP
        let status = maxps4_jit_cancel()
        return status == 0 ? String(localized: "Mémoire JIT libérée. Tu peux relancer l’activation.", bundle: .maxps4) :
            String(localized: "Libération refusée (\(status)) : une session ou une exécution utilise encore la mémoire.", bundle: .maxps4)
#else
        return String(localized: "Fournisseur absent.", bundle: .maxps4)
#endif
    }
}
