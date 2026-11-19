# MaxPS4 — embedded FEXCore and SideStore IPA

> **Historical report (PR #130, October 2026).** It describes the first FEXCore integration, before the JIT worked on iPhone. The JIT has since been enabled through StikDebug (see [docs/fexcore-stikdebug-jit.md](fexcore-stikdebug-jit.md) and the [README](../README.md#enabling-the-jit-with-livecontainer)).

Branch `codex/fexcore-app-integration`, main base
`fecb7ef15b91ae051fe7a82058f412f6f06a4b9d` (PR #129 merged).
Pinned Aether Darwin runtime `3347fc13f19860855ce91b66356940989ea6b6a6`.

## Integration

The repository used only swiftc to assemble an IPA. The new
`MaxPS4-iOS/project.yml` generates a real Xcode project through XcodeGen.
The target builds all the existing Swift sources, keeps the resources and
embeds `MaxPS4FEXRuntime.framework`. That framework contains the coherent FEX
runtime, the native bridge and the existing CPU adapters, with no generic FEX
objects and no second shared FEX library. The Aether allocator ABI adapter is
added to keep its existing Swift diagnostic; it allocates nothing.
The Swift/C boundary uses the opaque header from #128/#129.

**Diagnostics → Test FEXCore** tab: a screen runs the test on a task off the UI
thread, prevents simultaneous runs from this button and shows the system
actually detected, the context stage, guest CPUID and vendor, the detected
debugger (without inferring a JIT right from it), errors, value and backend.
The handle is closed by defer after each test.
On iOS, InitCore explicitly returns ENOTSUP as in #129; the real MaxPS4 native
interpreter runs mov eax,40/add eax,2/ret with a budget of 32. The result 42 is
not presented as a FEX JIT execution. Other errors do not trigger a silent
fallback. On macOS, the same Swift layer calls the #129 FEX JIT probe,
mov/add/hlt, without the interpreter.

The shipped runtime defines MAXPS4_PLATFORM_JIT_ONLY: the old experimental BRK
allocator is refused on iOS, even with a debugger attached. The interfaces and
diagnostics remain available but do not attempt this external protocol.
No BreakpointJIT framework is shipped or loaded by the new diagnostic.
Compliant iOS executable-memory mechanisms remained to be integrated before
enabling the FEX JIT. Neither SideStore, nor a development signature, nor a
debugger flag is enough to prove JIT rights.

## Build and verification

**MaxPS4 FEXCore SideStore IPA** workflow (`fexcore-app.yml`), automatic on this
PR and available manually. The old workflows/tests stay active; the old IPA
workflow is named Legacy to avoid confusing the artifacts. Old configurations
without MAXPS4_FEX_APP show that FEX is not embedded instead of inventing a
context.

The workflow runs the #129 native tests, eight real Swift diagnostics on macOS,
then builds the iOS runtime and the whole application with Xcode.
`verify-fexcore-ipa.py` checks the ZIP, the executable, the framework, ARM64,
iOS LC_BUILD_VERSION, dyld paths, the C imports/exports used by the app, the
real FEX CreateNewContext and the absence of non-SDK dependencies or missing
FEX symbols. The existing icon is kept through an Xcode asset.

macOS commands (iPhoneOS SDK, CMake, Ninja, XcodeGen required):

```sh
bash scripts/build-fexcore-app.sh /path/to/pinned/AetherPS4
python3 scripts/verify-fexcore-ipa.py build/MaxPS4-FEXCore-SideStore.ipa
```

The build needs no personal certificate. The app is not provisioned and the
framework is ad hoc signed after its install name is edited and after Xcode
strips its headers when embedding it: **SideStore must re-sign the app and its
framework**. An ad hoc signature is neither an iPhone install authorization nor
a JIT entitlement. Minimum deployment iOS 18, ARM64 architecture; format-wise
compatible with iOS 27.2, subject to its signing and install checks.
The runner uses Xcode 26.6 and the iOS 26.5 SDK. This build with an SDK older
than 27.2 is not a test on that system.

## Installing or updating with SideStore

1. Download the **MaxPS4-FEXCore-SideStore** artifact from the successful
   workflow page. Extract the Actions ZIP to get `MaxPS4-FEXCore-SideStore.ipa`.
2. Have a working SideStore install, with its pairing/network prerequisites and
   an Apple account usable for signing.
3. In SideStore, import the IPA with the add-app button. SideStore must also
   sign the embedded framework. Respect the app/App ID limits and the profile
   duration indicated by SideStore/Apple.
4. To update, reuse the same account and the same final bundle identity. Back
   up game imports before any uninstall: deleting the app can delete its local
   library.
5. Open MaxPS4 → Diagnostics → Test FEXCore. Keep the displayed report, the iOS
   version and any install/crash messages.

If framework signing fails, do not announce the IPA as installed: collect the
exact SideStore error and the profile/identity without publishing secrets.
Do not add private entitlements or workarounds to get the install through.
No PS4 game and no iPhone JIT was declared working at that stage.

## Verified results

Final code: `883781beace17cb1e0e3ce495a8a75dd9165eee4`.

| Validation | Result | Run |
| --- | --- | --- |
| Whole iOS application through Xcode, embedded framework, IPA | Passed | [38133153640](https://github.com/MaxFRdev/MaxPS4/actions/runs/38133153640) |
| Swift/C ABI, real context, CPUID and RAX=42 by FEX JIT on macOS | Eight cycles passed | same run |
| Heap cleanup through leaks --atExit | Eight more cycles; 0 leaks / 0 bytes lost | same run |
| #129 native tests, cycles, signals, interpreted fallback | Passed | same run and [38133153657](https://github.com/MaxFRdev/MaxPS4/actions/runs/38133153657) |
| Strict FEX Darwin linking on iOS and macOS | Passed | 38133153657 |
| Existing smoke, including the Swift frontend | Passed | [38133153684](https://github.com/MaxFRdev/MaxPS4/actions/runs/38133153684) |
| Existing heavy | Passed | [38133153653](https://github.com/MaxFRdev/MaxPS4/actions/runs/38133153653) |
| SideStore install/signing on iOS 27.2 | Not run | device needed |
| Touch interface and diagnostic on iPhone | Not run; build verified | device needed |
| FEX JIT on iPhone | Not run and deliberately unavailable | compliant provider missing |

The log of October 11, 2026 contains `Process 6817: 0 leaks for 0 total leaked bytes`
at 10:50:02 UTC, `BUILD SUCCEEDED` at 10:53:49, then the framework `valid on disk`
and `satisfies its Designated Requirement`. This is an ad hoc signature, not
an iPhone profile. The leaks check covers the host program's heap; it does not
measure all VM allocations, SwiftUI tasks or memory on a real iPhone.

Nine Python patch/diagnostic tests passed locally, shell syntax and
git diff --check passed. The NativeJITPipelineTest.cpp test was rebuilt with
g++ and the seven programs passed (ARM64 treated as data, real interpreter).
This Linux host has neither swiftc nor Xcode; Apple builds and runs were done by
the CI runners.

## Delivered IPA and independent inspection

[Verified IPA artifact — MaxPS4-FEXCore-SideStore](https://github.com/MaxFRdev/MaxPS4/actions/runs/38133153640/artifacts/11699684286),
ID `11699684286`, retained until October 25, 2026.
Artifact `11699414712` from the same run contains the build, test, leaks and
inspection logs. The first versions of commits cd812d2 and cdc49cc also
produced a real IPA and passed their checks; the artifact above adds
verification of the framework as shipped.

The archive was downloaded and checked again with verify-fexcore-ipa.py on the
Linux host. It contains:
- Payload/MaxPS4.app/MaxPS4: real iOS ARM64 Mach-O executable.
- Payload/MaxPS4.app/Frameworks/MaxPS4FEXRuntime.framework: real Darwin/FEX
  dylib and Info.plist, recomputed ad hoc signature.
- Info.plist, Assets.car and the application's existing resources.

Both binaries have MH_NOUNDEFS, are executable and target iOS, not the
simulator. The app actually imports open/initialize/cpuid/run/close/stage from
the framework, which exports these functions and the real CreateNewContext.
Its only dynamic dependencies are libSystem and libc++; its 230 SDK imports are
not missing FEX symbols. No generic FEX object, other shared FEX or
BreakpointJIT framework is embedded. Identity com.enermaxfr.maxps4, version 0.2,
build 3, iOS 18.0 deployment, iphoneos26.5 SDK.

| Verified file | SHA-256 |
| --- | --- |
| IPA | `ab8208861130b525e0f2ee9d138a0fb5f1225e8c7f028d6de062ac05ab1b86c9` |
| MaxPS4 executable | `669c280b22a236071ac90856757a9e22464d1c3897ebeb09471b2ab483223d7b` |
| Native framework | `75eda8a68853f9333249f5056c73de015427ce4f9a84b6cf637b474ffe1c6ed4` |

A verified build IPA is not an IPA whose SideStore install has been run. The
personal signing of the app and its framework, profile acceptance and launch on
iOS 27.2 remained to be verified. If refused, keep the exact error; no fake
package replaces the build.

## iPhone protocol and remaining blockers

After install, check in turn the launch and existing screens, then
Diagnostics → Test FEXCore. Expected result from the iOS code: context
created/stage 1, guest CPUID GenuineIntel, InitCore ENOTSUP, result 42 by the
MaxPS4 native interpreter, without the FEX JIT. This is an expectation, not an
observed device result.

Repeat twenty times, close/reopen the screen, suspend and relaunch the app,
record latency, any crash and heap/VM changes with the authorized tools
(Instruments/Xcode if available). Also test under memory pressure.
Diagnostic tasks are bounded but FEX internal OOM errors/assertions are not
guaranteed to be recoverable. No absence of crash/hang/leak on iPhone is
claimed before these measurements.

At the time, the iOS JIT still needed an authorized executable-memory
provider, RW/RX mappings with controlled lifetime, complete Darwin delegation of
faults/stops/suspensions, cache invalidation and safepoints. The app does not
try to enable these functions from the mere presence of a debugger.
PS4 syscalls/HLE, GPU and game execution are outside this probe.

## Commits and Git state

| Commit | Change |
| --- | --- |
| `cd812d20ff442f90fd8bf9081c3bc686f2078ccd` | Xcode application, real framework, Swift screen and verified IPA |
| `cdc49ccd29b3ffa14ba94477dcabe8f2d689b48b` | Detected CPUID/debugger and heap leak test |
| `883781beace17cb1e0e3ce495a8a75dd9165eee4` | Framework signing after copy, executable modes and icon asset |

The following documentation commit completes this report and the README. Its CI
results are recorded in the description of [PR #130](https://github.com/MaxFRdev/MaxPS4/pull/130).
No earlier test is removed. No merge commit and no direct change to main; the
PR is not merged automatically.
