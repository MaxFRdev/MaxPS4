import Foundation

@main
struct StikRequestTest {
    static func main() {
        let script = "// préparation RX UTF-8 🐱\nlog('MaxPS4');"
        let url = MaxPS4StikRequest.url(bundleID: "fr.enermax.MaxPS4", pid: 42,
            mailbox: 0x100000, nonce: 0xfedcba9876543210, pageSize: 16384, script: script)!
        let parts = URLComponents(url: url, resolvingAgainstBaseURL: false)!
        precondition(parts.scheme == "stikdebug" && parts.host == "enable-jit")
        let query = Dictionary(uniqueKeysWithValues: parts.queryItems!.map { ($0.name, $0.value!) })
        precondition(query.count == 3 && query["bundle-id"] == "fr.enermax.MaxPS4" && query["pid"] == "42")
        let encoded = query["script-data"]!
        precondition(!encoded.contains("+") && !encoded.contains("/") && !encoded.contains("="))
        var base64 = encoded.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        base64 += String(repeating: "=", count: (4 - base64.count % 4) % 4)
        let decoded = String(data: Data(base64Encoded: base64)!, encoding: .utf8)!
        precondition(decoded == "const MAXPS4_MAILBOX=0x100000n;const MAXPS4_NONCE=0xfedcba9876543210n;const MAXPS4_SIZES=[16384,16777216,16384];\n" + script)
        let liveContainerTarget = MaxPS4StikEnvironment.target(
            bundleIdentifier: "com.kdt.livecontainer.example", processID: 314)
        precondition(liveContainerTarget?.bundleIdentifier == "com.kdt.livecontainer.example")
        precondition(liveContainerTarget?.processID == 314)
        precondition(MaxPS4StikEnvironment.isLiveContainer(environment: ["LC_HOME_PATH": "/container"]))
        precondition(!MaxPS4StikEnvironment.isLiveContainer(environment: [:]))
        precondition(!MaxPS4StikEnvironment.shouldPreflightStikDebug(isLiveContainer: true))
        precondition(MaxPS4StikEnvironment.shouldPreflightStikDebug(isLiveContainer: false))
        precondition(MaxPS4StikEnvironment.target(bundleIdentifier: nil, processID: 314) == nil)
        precondition(MaxPS4StikEnvironment.target(bundleIdentifier: "host", processID: 0) == nil)
        precondition(MaxPS4JITFailureDiagnostic.explanation(initializationStatus: -13,
            memoryState: 0, isLiveContainer: true).contains("Use LiveContainer's Bundle ID"))
        precondition(MaxPS4JITFailureDiagnostic.explanation(initializationStatus: -13,
            memoryState: 2, isLiveContainer: false).contains("EACCES"))
        precondition(MaxPS4JITFailureDiagnostic.explanation(initializationStatus: -1,
            memoryState: 0, isLiveContainer: false).isEmpty)
        for page: UInt64 in [0, 3, 32768] {
            precondition(MaxPS4StikRequest.url(bundleID: "fr.enermax.MaxPS4", pid: 42,
                mailbox: 0x100000, nonce: 1, pageSize: page, script: script) == nil)
        }
        precondition(MaxPS4StikRequest.url(bundleID: "", pid: 42, mailbox: 8, nonce: 1, pageSize: 16384, script: script) == nil)
        precondition(MaxPS4StikRequest.url(bundleID: "MaxPS4", pid: 0, mailbox: 8, nonce: 1, pageSize: 16384, script: script) == nil)
        precondition(MaxPS4StikRequest.url(bundleID: "MaxPS4", pid: 42, mailbox: 8, nonce: 0, pageSize: 16384, script: script) == nil)
        print("PASS Stik URL identity, LiveContainer detection/query behavior, -13 diagnostics, nonce and base64url UTF-8; no device permission claimed")
    }
}
