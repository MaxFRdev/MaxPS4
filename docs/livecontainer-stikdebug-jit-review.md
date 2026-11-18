# LiveContainer / StikDebug JIT review

Date: 2026-11-18  
Branch: `fix/codex-livecontainer-jit` (based on `main` at `b096920`)

## PR dependency review

| PR | Scope | Dependency | Review |
| --- | --- | --- | --- |
| [#319](https://github.com/MaxFRdev/MaxPS4/pull/319) | Resolve an executable `.app` bundle and pass its ID/PID to StikDebug | `main` | The process PID is useful, but deriving a bundle ID from `_NSGetExecutablePath` bypasses LiveContainer's supported `Use LiveContainer's Bundle ID` behavior. Keep `getpid()` and the bundle ID reported by `Bundle.main`; fail closed when unavailable. |
| [#320](https://github.com/MaxFRdev/MaxPS4/pull/320) | Explain a failed `canOpenURL(stikdebug://)` query | Stacked on #319 | Correctly treats a failed query as inconclusive, but text alone leaves activation blocked. In LiveContainer, skip this query and let the documented `UIApplication.open` route report whether iOS dispatched the URL. |
| [#322](https://github.com/MaxFRdev/MaxPS4/pull/322) | Expand JIT `-13` / EACCES details | Independent, based on `main` | Useful and fail-closed. Incorporated with state, region, mailbox and native stage/Mach error fields. It explains likely setup checks without claiming to know the exact denial cause. |

PR #320 is based on #319's head; #322 is independent. This branch starts from current `main` and reimplements the supported behavior; it does not merge or modify any of those PRs.

## Findings

### Process identity

The StikDebug contract requires the target's current PID and bundle ID. Its integration guidance uses `Bundle.main.bundleIdentifier` and `getpid()`. LiveContainer documents `Use LiveContainer's Bundle ID` for this URL flow. A physical executable-path lookup can return or infer a different bundle identity and bypass that setting. The implementation therefore uses the process's own reported bundle identifier and `getpid()`, and detects LiveContainer via `LC_HOME_PATH` for guidance.

### URL scheme query

The app plist declares `stikdebug` in `LSApplicationQueriesSchemes`. Apple's `canOpenURL` rules still depend on the active host/query context, so a false result inside a container is not proof that StikDebug is absent. In LiveContainer, MaxPS4 now skips only the preflight query and calls `UIApplication.open`; iOS remains responsible for routing or rejecting the URL, and the completion handler reports that result. Outside LiveContainer, the existing preflight remains.

### Script and target communication

`maxps4-stik.js` is a fixed bundled script. The request carries the selected target bundle ID, current PID and base64url script data. The script attaches to that PID, checks the mailbox nonce, asks StikDebug's debugger transport for authorized executable regions, publishes the response and detaches. It does not claim JIT readiness merely because URL dispatch succeeded. The existing IPA verifier checks that the script is packaged; no IPA was built locally in this Windows environment.

### JIT `-13`

The JIT preparation path returns `-EACCES` (`-13`) on access denial. The report now retains session state, native error, failing stage, Mach status, region and mailbox diagnostic. A concise explanation points LiveContainer users to its bundle-ID setting and StikDebug JIT configuration. It does not change native FEXCore/JIT memory code or relax iOS checks.

## Changes and regression coverage

- Extracted pure LiveContainer detection, target validation, URL-query policy and `-13` explanation helpers into the existing Swift request module.
- LiveContainer uses the actual process PID and `Bundle.main` identity; it avoids the unreliable scheme-query preflight and reports URL dispatch failure.
- Added regression cases for `LC_HOME_PATH`, bundle-ID/PID validation, preflight behavior and JIT `-13` explanation.
- Kept the existing FEXCore workflow unchanged; its current Swift regression-test command already compiles the helper module.
- Expanded the readiness report with memory region and mailbox diagnostics.
- No controller, multiplayer, pairing-file, or native FEXCore files changed.

## Validation state

- Source review and `git diff --check`: run locally.
- Swift/Node/Python/Xcode command-line tools are unavailable or blocked in this Windows session; local unit execution and iOS compilation were not possible.
- GitHub Actions are expected to run on the draft PR. The observed checks on the source PRs were not all complete: #319 had several cancelled jobs; #320 and #322 had queued/in-progress jobs when reviewed.
- No physical iPhone/LiveContainer test was performed. JIT compatibility is **not confirmed**.

## iPhone validation checklist

1. Install a CI-built IPA signed through the user's existing SideStore process; keep the same pairing and VPN setup.
2. In LiveContainer, enable `Use LiveContainer's Bundle ID`; select StikDebug as JIT enabler and configure MaxPS4's fixed script if LiveContainer requests one.
3. Launch MaxPS4 in LiveContainer and request StikDebug. Confirm the reported target bundle ID is the LiveContainer ID and the PID is MaxPS4's current process.
4. Confirm StikDebug attaches to that PID, runs `maxps4-stik.js`, returns to MaxPS4, and mailbox confirmation completes.
5. Run the existing FEX readiness probe and record its backend/result; URL opening or script completion alone is not a JIT pass.
6. Repeat after installing/launching MaxPS4 directly through SideStore to check the standalone path remains intact.
7. If it fails, capture MaxPS4's full readiness report and StikDebug's own status without sharing pairing-file contents.
