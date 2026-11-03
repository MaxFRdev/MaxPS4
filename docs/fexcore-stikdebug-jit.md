# PR #131 — FEXCore ARM64 JIT, SideStore and authorized StikDebug regions

## Base and measured starting state

PR #130 is OPEN, not merged at the start of this work. This change is stacked on
`codex/fexcore-app-integration`, commit
`1c22d1eab9ab85677a0122862ac65db61f987e6b`; main remains
`fecb7ef15b91ae051fe7a82058f412f6f06a4b9d`. PR #131 must merge after #130;
retarget it to main only after that dependency is merged. No automatic merge.

The user reports a physical iPhone running iOS 27.2 (24B5099f): real FEX context
and GenuineIntel CPUID, debugger absent, InitCore ENOTSUP, native interpreter 42.
That is the user's observed #130 result, not a successful device JIT test.

> **État de la version intégrée (PR #159, iPhone) : chemin expérimental désactivé.**
> Le code iOS livré garde `maxps4_fex_engine_initialize()` et l'exécution FEX JIT
> derrière un refus explicite `-ENOTSUP` (sur Darwin : -45). La préparation
> mémoire StikDebug décrite ci-dessous est un **protocole de conception non activé**,
> et l'interface actuelle ne déclenche pas `maxps4_jit_prepare()`. Ni la signature
> SideStore, ni le résultat 42 de l'interpréteur, ni le succès d'un script StikDebug
> ne démontrent que le JIT FEX fonctionne sur iPhone. Les critères de réussite JIT
> cités dans ce document ne sont **pas atteignables avec cette version** ; ne pas
> les utiliser comme procédure d'acceptation sur appareil tant que le garde-fou
> n'a pas été réévalué et validé de manière reproductible.

## Exact reason for ENOTSUP and the proposed experimental path

#129/#130 explicitly returned ENOTSUP in `maxps4_fex_engine_initialize` and
`maxps4_fex_engine_probe` on iOS. The coherent pinned Aether Darwin runtime
`3347fc13f19860855ce91b66356940989ea6b6a6` otherwise dynamically loaded an absent
BreakpointJIT framework. Adding debugger detection would not fix either cause.

The experimental provider design would remove both runtime gates, replacing the pinned iOS allocator
with a checked adapter to the real prepared-region pool, and keeps every other
Darwin adaptation and the opaque C ABI. No generic FEX objects, fake symbols,
builtin fake interpreter, or second FEX dylib are linked. macOS retains MAP_JIT
and its existing per-thread pthread_jit_write_protect_np guards.

The developer-defined script is `Resources/JIT/maxps4-stik.js`. It uses the
same authorized debugger services as StikDebug's universal protocol, but
prepares the complete bounded workload BEFORE resuming the process. There is
no app BRK instruction and therefore no crash from a missing breakpoint script.

1. MaxPS4 creates a fresh nonce and process-local mailbox, then opens the
   documented `stikdebug://enable-jit` URL with its actual bundle ID, current PID
   and this ONE fixed script supplied through `script-data`.
2. StikDebug must actually attach through its authorized debugserver connection.
   It requests three RX mappings (`_M<size>,rx`): 16 KiB dispatcher, 16 MiB
   code cache and one host page for a native canary. Each must pass
   `prepare_memory_region` before publication. No kernel/CS flag or entitlement
   is patched. Allocation/preparation failures are reported and debugger-owned
   regions are deallocated through `_m<address>`.
3. While the task is stopped, the script writes the addresses/status and the
   per-request nonce last. It detaches before host execution. An ordinary
   debugger flag, URL acceptance or a prior app's JIT session cannot supply
   these process-specific regions or establish readiness.
4. MaxPS4 consumes only that completed mailbox. It validates addresses, sizes,
   non-overlap, actual RX protections via vm_region_64 and constructs shared
   writable aliases with vm_remap(copy=FALSE). vm_protect restricts RW/RX
   protections and maximums separately: no individual address is writable AND
   executable. It verifies alias coherence, invalidates both instruction-cache
   aliases with sys_icache_invalidate and executes a tiny native ARM64 canary.
   The canary is NOT guest execution and never selects the FEX backend.
5. In the proposed (currently disabled) design only successful preparation and native execution would permit real InitCore,
   dispatcher emission, real CreateThread and ExecuteThread. The fixed guest
   `mov eax,40; add eax,2; hlt` must yield real guest RAX=42. Only that call
   selects `MAXPS4_CPU_FEX_ARM64_JIT`. The interpreter fallback remains separate
   and is explicitly labelled; JIT errors remain visible when fallback succeeds.

This is a proposed experimental integration path, not an enabled iPhone feature or proof it works on iOS 27.2. Public Mach
VM APIs implement local aliasing/protection/cleanup; the initial executable grant
comes from an authorized external debugger, NOT a public on-device entitlement
API. No private csops/SecTask/IOKit API is used by MaxPS4, no jailbreak or
code-signing bypass is implemented. If the OS denies the grant or writable
remap, initialization fails safely with the actual negative error.

## Ownership, synchronization and limitations

Only one initialized iOS FEX context may use the bounded pool at once; another
receives EBUSY. Existing runtime and pool mutexes serialize initialization,
execution, lease changes and cleanup. C callers must still keep handles alive
and serialize close against use. The program is fixed, finite, HLT-terminated,
with guarded guest/call-return stacks; arbitrary games/guest code are not enabled.

There are no late RX allocations or reconnecting app breakpoints after detach.
One fixed-size code cache is enough for the probe; another allocation fails
explicitly instead of requesting an absent debugger. The code-cache write guard
uses the HOST page size (normally 16 KiB), not FEX's 4 KiB guest page size.
Prepared buffers are leased to FEX, returned by real context destruction, and
retained to permit repeated tests after StikDebug detaches. They are a bounded
reservation, not unbounded leaked allocations. The diagnostic reports outstanding
leases AFTER closing its context; expected count is zero. “Libérer la mémoire
JIT” deallocates both mappings when no lease remains; activation is required again.

The native canary temporarily preserves/restores SIGBUS/SIGSEGV/SIGILL handlers
and the signal mask. It recovers only a fault at the canary's exact PC on that
thread; unrelated host handlers chain. It does not longjmp through FEX/C++ guest
frames or suppress FEX faults. An OS SIGKILL/code-sign termination is not
recoverable; only a real device test can exclude that behavior. Full guest
signal handling/safepoints/exception routing are still prerequisites for games.

A pending script is not cancelled/replaced while an external debugger may still
write to the mailbox. If attach fails before the script can report a status,
MaxPS4 remains safely pending (EAGAIN, interpreter fallback); stop the session
and restart MaxPS4. Neither the UI nor its worker waits indefinitely for a grant.

## Upstream contracts examined

* StikJIT `32287268fa5824f9edce4cb359f5833ce0cf7b00`:
  [INTEGRATION.md](https://github.com/StikDebug/StikJIT/blob/32287268fa5824f9edce4cb359f5833ce0cf7b00/INTEGRATION.md),
  `Resources/universal.js`, `Sources/ScriptRunner.swift`.
* StikDebug `775fba15f3fbadc18f7f1444dc6ffb1566adaa4c`:
  `Views/HomeView.swift`, `JSSupport/RunJSView.swift`,
  `JSSupport/JSDebugSupport.swift`, `Scripts/universal.js`.

The source confirms `get-task-allow` is required; TXM/SPTM devices need actual
per-region preparation, not just debugger attachment. It supports custom
base64 script-data with bundle ID/PID and Number-valued preparation arguments.
It currently runs custom scripts only when StikDebug detects TXM (or its own
script-detection override is enabled). If it skips this adapter, MaxPS4 refuses
JIT because the mailbox never completes; no unsupported classic-JIT success is
assumed. Use the developer-provided adapter from MaxPS4's activation button,
not a manually selected ManicEMU/universal/legacy script for another engine.

iOS 27.2 beta support is NOT guaranteed by upstream's broad “26.0+” statement.
DDI/pairing/VPN/signing/script behavior and per-region grants must be confirmed
on the actual device. Do not work around a permission denial.

## SideStore + StikDebug physical test protocol

1. Download the PR #131 Actions artifact, extract its ZIP and obtain the actual
   `MaxPS4-FEXCore-SideStore.ipa`. Keep the same signing account/bundle identity
   when updating #130; back up saves/library before any uninstall. Application
   features/storage paths are preserved; a different signing identity can still
   change the installed sandbox. Do not reinstall destructively to test JIT.
2. Re-sign the app AND embedded framework with SideStore. The artifact carries
   an ad-hoc signature with ONLY `get-task-allow=true`; this grants no device
   rights. The final provisioning profile/signing method must legally permit
   and preserve that entitlement. No dynamic-codesigning/allow-jit entitlement
   is fabricated. If debugserver refuses attach, inspect the ACTUAL re-signed
   entitlements/profile; an ad-hoc build's requested entitlement proves nothing.
3. Follow StikDebug's official setup: Developer Mode where required, import the
   same PairingFile used by SideStore (for example `ALTPairingFile.mobiledevicepairing`),
   and configure the appropriate developer disk image, VPN and supported connection.
   Keep this device-specific file in StikDebug; do not add it to the MaxPS4 IPA or logs.
4. Open MaxPS4 → Diagnostic → Tester FEXCore first WITHOUT activation. Expected:
   real CPUID, initialization denied, interpreter result 42, no “JIT réussi”.
5. Press **Activer avec StikDebug** in MaxPS4. Approve StikDebug's own request for
   THIS PID and supplied fixed script. Its log must show all three RX regions
   prepared and successful detach. Return to the SAME MaxPS4 process.
6. Press **Tester FEXCore**. Acceptance requires initialisation 0, FEX execution
   0, actual result 42, backend **FEXCore JIT ARM64**, **JIT FEXCore réussi** and
   zero outstanding leases after cleanup. A native/interpreter 42 is a failure
   to meet the JIT criterion, regardless of debugger indicators.
7. Repeat 20 times. Check UI responsiveness, app logs, crash reports and memory
   with authorized device tooling. Background/resume and memory pressure also
   need testing. The reserved 16 MiB code cache should remain bounded; “Libérer
   la mémoire JIT” must release it and return subsequent tests to interpreter.
8. Test absent signing entitlement, rejected attach, disconnected VPN, denied
   allocation/preparation/remap, stopped script and restart. Capture actual
   statuses/StikDebug log. Do not interpret EAGAIN/EACCES/EFAULT as JIT success.

Record device model, iOS build, SideStore/StikDebug versions, final entitlements,
script log, diagnostic text/backend and reproducibility. No physical-device
result is currently claimed. The user's final criterion remains pending.

## Validation results

Local Linux: 12 Python patch/diagnostic tests pass, shipped JS controller packet
and failure-cleanup tests pass, shell syntax and git diff checks pass. No Apple
SDK or iPhone is attached locally.

CI is pending publication. Existing native/macOS FEX/Swift JIT lifecycle tests,
smoke and heavy workflows remain enabled. New CI exercises the same pool source
with real Mach VM memory on macOS (or actual kernel rejection with rollback),
handler/mask preservation, lease bounds, translations/reuse/cleanup and heap
checks; these are host tests, not simulated device grants. iOS cross-compilation,
complete Xcode build, ad-hoc signature and IPA inspection are required before
publishing a test artifact. Results and artifact link will be added after CI.
