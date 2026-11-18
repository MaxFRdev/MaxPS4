# JIT error -13 (EACCES) under LiveContainer: investigation

Device report: iPhone 15 Pro Max, iOS 27.2, MaxPS4 inside LiveContainer, with
StikDebug. The JIT screen showed:

```
Préparation du JIT : -13
État mémoire : 0
Erreur mémoire : 0
Étape : 0
Code Mach : 0
```

Baseline: `main` at b096920. JIT has **not** been shown to work on this
device, and this report does not claim it does.

## 1. Where the -13 comes from (confirmed)

The report is printed by `MaxPS4JITReadiness.check()`
(`MaxPS4-iOS/Sources/MaxPS4JITSetupView.swift:14-16`). That function calls
`maxps4_fex_engine_initialize()`
(`MaxPS4-iOS/Native/maxps4_fex_runtime.cpp:1469`). On iOS, initialization first
calls `maxps4_jit_prepare()` (line 1477) and returns its result unchanged
(lines 1478-1481).

`maxps4_jit_prepare()` (`MaxPS4-iOS/Native/maxps4_jit_memory.cpp:175-230`)
can return `-EACCES` in two ways:

| Path | Lines | State after | Error | Stage |
|---|---|---|---|---|
| **A.** No request in this process (`state != 1`, here 0) | 179 | **0** | **0** | **0** |
| **B.** StikDebug reported a failure (`mailbox.status != 1`), or a Mach check failed (RX protection, `vm_remap`, `vm_protect`, guard page) | 182, 195-214, via `reject_stage` (132-144) | 3 | 13 | 1 to 12 |

Path B always sets state 3, error 13 and a non-zero stage. The device report
shows state 0, error 0 and stage 0, so it is **path A**:
`maxps4_jit_begin()` (lines 156-174) never succeeded in the process that ran
the check, or `maxps4_jit_cancel()` (lines 252-263) cleared the request.

The CI test for the JIT memory provider already fixes both behaviors:
`MaxPS4-iOS/Tests/FEXJITMemoryTest.cpp:27` (prepare with no request returns
`-EACCES`) and lines 48-50 (a StikDebug failure gives `-EACCES` with state 3
and error 13).

### What is ruled out

- **A refusal by iOS of the memory or of `vm_remap`/`vm_protect`**: that is
  path B, which would show state 3 and a stage from 4 to 10.
- **The StikDebug script reporting a failure**: path B, stage 1
  (`mailbox-status`).
- **The StikDebug script running in another process** (for example the
  LiveContainer main process while MaxPS4 runs elsewhere): state would stay
  1. The script refuses a mailbox whose challenge does not match
  (`MaxPS4-iOS/Resources/JIT/maxps4-stik.js:26-28`) and writes nothing.
  `prepare` would then return `-EAGAIN` (**-35**), not -13.
- **A failing native canary**: `-EIO` or `-EFAULT`, state 3.

The -13 says nothing about the iOS permissions for executable memory, because
the code never got as far as checking them.

## 2. Why no request was pending: the possible causes

The flow is in `MaxPS4-iOS/Sources/MaxPS4StikActivation.swift:6-32`. It calls
`maxps4_jit_begin()` (line 18) only once these checks pass:

1. `canOpenURL("stikdebug://")` succeeds (line 8). Under LiveContainer the URL
   query allowlist is the host app's, so this can fail even when StikDebug is
   installed. Covered by draft PR #320.
2. The `maxps4-stik.js` script is found in `Bundle.main` (lines 11-16).

The activation then opens `stikdebug://enable-jit` with `bundle-id` and `pid`
(`MaxPS4StikRequest.swift:5-25`). Under LiveContainer, `Bundle.main` is
virtualized, so the `bundle-id` sent is MaxPS4's and not the host's. Covered
by draft PR #319.

There are three ways to reach state 0 on the check, and the current report
cannot tell them apart:

| Cause | Kind | Example |
|---|---|---|
| C1. The request was never sent from MaxPS4 | **configuration / usage** | JIT enabled directly in StikDebug on the LiveContainer process; or step 1 failed and its message went unnoticed |
| C2. MaxPS4 restarted between the request and the check | **configuration or environment** | StikDebug or LiveContainer relaunching the host process to attach; iOS killing the process in the background. The RX regions belonged to the old process |
| C3. The request was cleared | usage | « Réinitialiser le JIT » button (`maxps4_jit_cancel`) |

C1 and C2 are the most likely given « StikDebug détecte le processus hôte
LiveContainer ». **Not confirmed**: telling them apart needs a new report from
the iPhone.

### Implementation weaknesses (not the cause, but they hide it)

- Path A records no stage, so -13 with everything at 0 looks like a failure
  « sans information ».
- `requested = true` is set even when `begin` returned before sending
  anything (`MaxPS4JITSetupView.swift`, button action).
- The report does not show the PID that sent the request, so a process
  restart (C2) cannot be seen.

## 3. Minimal change in this branch (diagnostic only)

- `MaxPS4-iOS/Sources/MaxPS4JITRequestRecord.swift` (new) records the PID of
  the process that really sent a request. It does so only when
  `maxps4_jit_state() == 1` after `begin`, in `UserDefaults`, so the record
  survives a restart. After a failed check in state 0, it adds one line to the
  report: C1 (no request recorded), C2 (recorded PID ≠ current PID) or C3
  (same PID).
- `MaxPS4JITSetupView.swift`: two hooks, after the activation button and after
  the check. They do not touch the line that PR #322 changes.
- Not changed: `maxps4_jit_memory.cpp`, the StikDebug script, the
  protections (W^X kept), SideStore. Nothing is bypassed and no permission is
  granted.
- Test: `MaxPS4-iOS/Tests/JITRequestRecordTest.swift`, to run with
  `swiftc -parse-as-library MaxPS4-iOS/Sources/MaxPS4JITRequestRecord.swift
  MaxPS4-iOS/Tests/JITRequestRecordTest.swift`. It is not in CI yet: adding it
  to `fexcore-app.yml` needs a token with the `workflow` scope. The new file
  is still type-checked by the jobs that compile `MaxPS4-iOS/Sources/*.swift`.

## 4. Fix plan

1. **On the iPhone, with this branch's IPA**: tap « Activer avec StikDebug »
   *in MaxPS4*, finish the StikDebug script without closing LiveContainer,
   come back, then tap « Vérifier l'activation ». Read the « Cause : » line.
2. If the cause is **C1**:
   - check that the button opens StikDebug;
   - if it shows « StikDebug n'est pas disponible », apply #320 (host
     `LSApplicationQueriesSchemes`).
3. If the cause is **C2**:
   - apply #319, so StikDebug gets the host's bundle ID and PID;
   - check in StikDebug's logs that it attaches to the PID it was given
     without relaunching the app;
   - if StikDebug has to relaunch, the request must be sent again after the
     restart. The RX regions cannot survive a process restart.
4. If the state becomes **3** (path B), the failing stage and Mach code will
   name the real iOS limit (for example `vm-remap`). That would be a separate
   investigation.
5. Declare the JIT working only when the check gives backend
   `MAXPS4_CPU_FEX_ARM64_JIT` and result 42 (`MaxPS4JITSetupView.swift:18-22`),
   which is the native execution proof.
