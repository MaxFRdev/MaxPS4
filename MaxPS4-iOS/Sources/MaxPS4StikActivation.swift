import Foundation
import UIKit

@MainActor
enum MaxPS4StikActivation {
    static func begin(completion: @escaping (String) -> Void) -> String {
#if MAXPS4_FEX_APP
        let isLiveContainer = MaxPS4StikEnvironment.isLiveContainer(environment: ProcessInfo.processInfo.environment)
        guard let installedURL = URL(string: "stikdebug://") else {
            return "URL StikDebug invalide : activation JIT refusee."
        }
        if MaxPS4StikEnvironment.shouldPreflightStikDebug(isLiveContainer: isLiveContainer),
           !UIApplication.shared.canOpenURL(installedURL) {
            return String(localized: "StikDebug n’est pas disponible. Installer et configurer StikDebug, le pairing et le VPN selon ses instructions.", bundle: .maxps4)
        }
        guard let target = MaxPS4StikEnvironment.target(bundleIdentifier: Bundle.main.bundleIdentifier,
                                                        processID: getpid()) else {
            return isLiveContainer
                ? "Identifiant du processus absent. Dans LiveContainer, activer Use LiveContainer's Bundle ID. Activation refusee."
                : "Impossible d'identifier le processus MaxPS4 ; activation JIT refusee."
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
        guard let url = MaxPS4StikRequest.url(bundleID: target.bundleIdentifier, pid: target.processID,
            mailbox: mailbox, nonce: nonce, pageSize: page, script: source) else {
            _ = maxps4_jit_launch_failed(nonce)
            return String(localized: "URL StikDebug invalide : JIT indisponible.", bundle: .maxps4)
        }
        UIApplication.shared.open(url, options: [:]) { opened in
            if !opened { _ = maxps4_jit_launch_failed(nonce) }
            if opened {
                completion(String(localized: "Termine l’activation dans StikDebug, puis reviens dans MaxPS4.", bundle: .maxps4))
            } else if isLiveContainer {
                completion("iOS n'a pas ouvert StikDebug. Verifier sa configuration dans LiveContainer, le JIT enabler et Use LiveContainer's Bundle ID. JIT indisponible.")
            } else {
                completion(String(localized: "Ouverture de StikDebug refusée ; JIT indisponible. Réessayer l’activation.", bundle: .maxps4))
            }
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
