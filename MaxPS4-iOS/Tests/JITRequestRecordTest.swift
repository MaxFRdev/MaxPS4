import Foundation

@main
struct JITRequestRecordTest {
    static func main() {
        let suite = "maxps4.jit.request.test.\(getpid())"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        // A request is pending (state 1), prepared (2) or refused (3): nothing to add.
        for state: Int32 in [1, 2, 3] {
            precondition(MaxPS4JITRequestRecord.explain(state: state, requestPID: 7, currentPID: 7) == nil)
        }
        // State 0: never sent, sent by an earlier process, or reset here.
        precondition(MaxPS4JITRequestRecord.requestPID(in: defaults) == nil)
        let never = MaxPS4JITRequestRecord.explain(state: 0, requestPID: nil, currentPID: 42)!
        precondition(never.contains("aucune demande") && never.contains("42"))
        MaxPS4JITRequestRecord.remember(pid: 41, in: defaults)
        precondition(MaxPS4JITRequestRecord.requestPID(in: defaults) == 41)
        let relaunched = MaxPS4JITRequestRecord.explain(state: 0, requestPID: 41, currentPID: 42)!
        precondition(relaunched.contains("redémarré") && relaunched.contains("41 → 42"))
        let reset = MaxPS4JITRequestRecord.explain(state: 0, requestPID: 42, currentPID: 42)!
        precondition(reset.contains("réinitialisée"))
        defaults.set(0, forKey: MaxPS4JITRequestRecord.key)
        precondition(MaxPS4JITRequestRecord.requestPID(in: defaults) == nil)
        print("PASS JIT request record: state 0 explained as never sent, relaunched process or reset; other states untouched")
    }
}
