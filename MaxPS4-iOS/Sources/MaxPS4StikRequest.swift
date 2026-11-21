import Foundation

/// Process identity and preflight decisions for the StikDebug URL handoff.
/// Kept independent of UIKit so LiveContainer behavior is regression tested.
enum MaxPS4StikEnvironment {
    struct Target: Equatable {
        let bundleIdentifier: String
        let processID: Int32
    }

    static func isLiveContainer(environment: [String: String]) -> Bool {
        environment["LC_HOME_PATH"] != nil
    }

    static func target(bundleIdentifier: String?, processID: Int32) -> Target? {
        guard let bundleIdentifier, !bundleIdentifier.isEmpty, processID > 0 else { return nil }
        return Target(bundleIdentifier: bundleIdentifier, processID: processID)
    }

    /// LiveContainer's host Info.plist may not declare stikdebug even when the
    /// guest does. Let iOS resolve the URL and inspect UIApplication.open's result.
    static func shouldPreflightStikDebug(isLiveContainer: Bool) -> Bool {
        !isLiveContainer
    }
}

enum MaxPS4JITFailureDiagnostic {
    static func explanation(initializationStatus: Int32, memoryState: Int32,
                            isLiveContainer: Bool) -> String {
        guard initializationStatus == -13 else { return "" }
        if memoryState == 0 {
            let host = isLiveContainer
                ? " Dans LiveContainer, activer Use LiveContainer's Bundle ID et verifier que StikDebug est choisi comme JIT enabler."
                : " Verifier que StikDebug a termine l'activation pour ce processus."
            return "Acces JIT refuse (EACCES); aucune session memoire JIT active." + host + " Ce code ne confirme pas la cause exacte du refus."
        }
        return "Acces JIT refuse (EACCES). Consulter l'etat de session, la mailbox et l'etape ci-dessus; aucune autorisation iOS n'a ete contournee."
    }
}

/// Pure URL construction: the only supported adapter is the bundled script.
enum MaxPS4StikRequest {
    static func url(bundleID: String, pid: Int32, mailbox: UInt64, nonce: UInt64,
                    pageSize: UInt64, script: String) -> URL? {
        guard !bundleID.isEmpty, pid > 0, mailbox != 0, mailbox % 8 == 0,
              nonce != 0, pageSize > 0, pageSize <= 16384,
              16384 % pageSize == 0, !script.isEmpty else { return nil }
        let prefix = "const MAXPS4_MAILBOX=0x\(String(mailbox, radix: 16))n;const MAXPS4_NONCE=0x\(String(nonce, radix: 16))n;const MAXPS4_SIZES=[16384,134217728,\(pageSize)];\n"
        // StikDebug accepts base64url; avoid '+' ambiguity in query decoding.
        let data = Data((prefix + script).utf8).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        var request = URLComponents()
        request.scheme = "stikdebug"
        request.host = "enable-jit"
        request.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleID),
            URLQueryItem(name: "pid", value: String(pid)),
            URLQueryItem(name: "script-data", value: data)
        ]
        return request.url
    }
}

/// The game an automatic test run asks for, read once per launch.
enum MaxPS4AutoLaunch {
    static let fileName = "MaxPS4-autolaunch.txt"
    static func consume(arguments: [String] = ProcessInfo.processInfo.arguments,
                        environment: [String: String] = ProcessInfo.processInfo.environment) -> String? {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let file = documents.appendingPathComponent(fileName)
        if let text = try? String(contentsOf: file, encoding: .utf8) {
            try? FileManager.default.removeItem(at: file) // One run per request.
            let title = text.trimmingCharacters(in: .whitespacesAndNewlines)
            if !title.isEmpty { return title }
        }
        if let index = arguments.firstIndex(of: "-MaxPS4AutoLaunch"), index + 1 < arguments.count {
            return arguments[index + 1]
        }
        if let title = environment["MAXPS4_AUTOLAUNCH"], !title.isEmpty { return title }
        return nil
    }
}
