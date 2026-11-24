import Foundation

/// Which process last sent "Activer avec StikDebug". JIT memory belongs to
/// one process: after a failed check with no request pending here (memory
/// state 0, the -13 of maxps4_jit_prepare with every field at 0), this tells
/// a request never sent from a relaunched process or a reset. Diagnostic
/// only: it grants nothing and changes no memory protection.
enum MaxPS4JITRequestRecord {
    static let key = "maxps4.jit.requestPID"

    static func remember(pid: Int32, in defaults: UserDefaults = .standard) {
        defaults.set(Int(pid), forKey: key)
    }

    static func requestPID(in defaults: UserDefaults = .standard) -> Int32? {
        guard let value = defaults.object(forKey: key) as? Int, value > 0, value <= Int(Int32.max) else { return nil }
        return Int32(value)
    }

    /// Why no request is pending in this process, or nil when one is (state != 0).
    static func explain(state: Int32, requestPID: Int32?, currentPID: Int32) -> String? {
        guard state == 0 else { return nil }
        guard let requestPID else {
            return "Cause : aucune demande « Activer avec StikDebug » n’a été envoyée par MaxPS4 (processus \(currentPID)). Activer le JIT directement depuis StikDebug ne prépare pas la mémoire de MaxPS4 : utiliser le bouton de MaxPS4."
        }
        if requestPID != currentPID {
            return "Cause : MaxPS4 a redémarré depuis la demande (processus \(requestPID) → \(currentPID)). La mémoire préparée par StikDebug appartenait à l’ancien processus. Relancer « Activer avec StikDebug » sans fermer MaxPS4 ni LiveContainer."
        }
        return "Cause : la demande de ce processus (\(currentPID)) a été réinitialisée. Relancer « Activer avec StikDebug »."
    }
}
