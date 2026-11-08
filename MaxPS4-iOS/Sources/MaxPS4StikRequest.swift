import Foundation

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
