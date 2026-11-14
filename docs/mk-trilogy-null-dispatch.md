# MK Trilogy: host `pc=0` from FEX's dispatcher (issue #296)

## Device evidence (#298, #300)

```
fatal-host-fault signal=11 code=2 addr=0x0 pc=0x0(other) lr=0x1031b4190(dispatcher)
x0=0x0 x1=0x0 x2=0x13 x3=0x0 lr-dispatch-offset=0x190 guest-thread=14 guest-rip=0x7001a14338
```

LR = dispatcher base + 0x190 in two runs despite ASLR.

## Root cause

FEX source pinned by CI (AetherPS4 `3347fc13`):

* `Dispatcher.cpp`, `NoBlock` path: `blr x4` to `CompileBlock(CTX, Frame, RIP, 0)`,
  then `mov TMP1, x0` (TMP1 *is* x0 on non-EC ARM64: `TMP_ABIARGS = true`),
  `isb`, `br TMP1`. **No null check.** `ExitFunctionLink` and
  `CompileSingleStep` use the same pattern.
* `Core.cpp`, `ContextImpl::CompileBlock`: `if (CodePtr == nullptr) return 0;`.
* `GenerateIR` returns no IR (`return {std::nullopt, ...}`) when the **first**
  instruction of a block gets a dispatch error: FEX decoded it but its opcode
  handler failed (decode failure, or a LOCK prefix without a lock handler).
  Invalid opcodes do not take this path: they compile to a SIGILL block.

So: guest thread 14 reaches `0x7001a14338` (the RIP the dispatcher read from
`State.rip`), FEX cannot lower its first instruction, `CompileBlock` returns 0,
the dispatcher does `br x0` â†’ host `pc=0`, LR still just after the `blr`
(+0x190). `x0=0` is that return value. `br` does not change LR. `x2` is a
caller-saved register clobbered by the call, so `0x13` is not the RIP.

Host `pc=0` is neither JIT nor dispatcher code, so `GuestFaultGuard` chained
the fault and iOS killed the app.

## Fix (this branch)

`maxps4_dispatch_fault.h`: `pc == 0 && LR in [DispatcherBegin, DispatcherEnd)`
on SIGSEGV is exactly this case. `GuestFaultGuard::handle` now contains it as a
guest fault: SIGILL, code `0x4d50`, address = guest RIP, thread stopped through
`ThreadStopHandlerAddressSpillSRA` (static registers were refilled after the
call). The run ends with `-EILSEQ`. The app reports "Instruction x86 non prise
en charge par FEX Ã  RIP â€¦" instead of being killed. The game **still does not
run**: the instruction must be supported next.

The handler also appends a `contained-host-fault` line with up to 16 bytes at
the guest RIP (`guest-bytes=â€¦`) to `Documents/maxps4-fatal-fault.txt`.

## Next step

Retest on device, read `guest-bytes`, disassemble them, and add FEX support for
that instruction. Then retest MK Trilogy and Sonic Mania.

## iPhone retest: curtain-to-black-screen (2026-11-12)

The SideStore IPA from #298 (`2ee8e6c`) no longer exits to SpringBoard when selecting **Legacy Kollection â†’ Mortal Kombat Trilogy â†’ Play**. The menu and curtain-closing animation render, followed by a persistent **black screen and no audible sound**, even after 1â€“2 minutes. This is **not** a verified game fix. See issue #296 for the full user report.

Observed diagnostic snapshot (not a causal proof): JIT uptime ~37 s; 1,943 previously displayed frames, but current presentation rate 0 FPS; `sceGnmSubmitAndFlipCommandBuffers` reported waiting for VBlank ~1.5 s; repeated `sceKernelUsleep(0x3e8, ...)` returns `-1`; audio calls occur but output frames are overwhelmingly dropped; Metal reports 0 draw failures. This snapshot contains **no** `contained-host-fault` or `guest-bytes` line. Do not assume the original null-dispatch fault still occurs, or that `sceKernelUsleep` is causal, without more evidence.

### Required diagnostics before a behavior change

1. **FEX / guest progress:** record bounded per-thread guest RIP progress, guest thread stop reason, and any contained fault; determine whether execution advances after Play. If a contained fault occurs, capture `guest-bytes` from `maxps4-fatal-fault.txt` and decode the actual instruction before changing FEX.
2. **PS4 sleep / synchronization:** for `sceKernelUsleep`, log raw guest return, relevant guest errno and whether the intended sleep duration is honored; audit calling convention and return translation. For `scePthreadCondWait`, capture wait and wake counts at the curtain transition. Repeated `-1` alone is not evidence of a hang.
3. **VideoOut / Metal:** correlate monotonic timestamps for flip submission, VBlank wake, flip completion, and final Metal present; identify the last completed flip and any queue stall. Avoid synthetic VBlank or bypassing a wait without a failing regression test.
4. **Audio:** correlate audio-output timestamps and frame drops with the first stalled flip; determine whether audio is causally blocked or only starved.
5. **Validation:** add focused automated tests; compare Sonic Mania; build a fresh experimental SideStore IPA and retest on iPhone. Keep PRs #298/#302 isolated, no merge to `main` or `MaxPS4-Beta` without explicit approval.

## Device fault captured after retest (2026-11-12)

The device now reports:

```text
contained-host-fault signal=4 code=19792 addr=0x7001a14338 pc=0x0(other) lr=0x10131c190(dispatcher) fp=0x0 sp=0x17f0e6bf0 x0=0x0 x1=0x0 x2=0x0 x3=0x0 guest-thread=14 guest-rip=0x7001a14338 guest-bytes=c34989cab8110000000f05c34989cab8
```

The first instruction at the reported guest RIP is **`c3` = `ret`**. The following bytes decode as `mov r10, rcx; mov eax, 17; syscall; ret`. These are adjacent instructions, **not** evidence that the `syscall` was reached. The previous hypothesis that this is necessarily an unsupported x86 opcode is therefore **not established**. Check whether the guest RIP is a valid instruction boundary, whether the return stack / memory mapping is valid, and why FEX's CompileBlock returned zero for a `ret` entry point. Do not special-case or skip `ret` without proving the cause.

The corresponding regression in `MaxPS4-iOS/Tests/FatalFaultLogTest.cpp` locks down the observed record's formatting; **it does not reproduce or fix the JIT compilation failure**. Next investigate FEX's GenerateIR / CompileBlock return path and capture the guest stack top and mapping permissions at RIP, with bounded diagnostics, before changing translation semantics.

## Why `CompileBlock` "returned 0 for a RET" (analysis, 2026-11-12)

A plain `c3` cannot make `CompileBlock` return 0 in the pinned FEX
(AetherPS4 `3347fc13`):

* `RETOp` never sets `DecodeFailure` (`OpcodeDispatcher.cpp:150`).
* A non-executable or unreadable RIP does not return 0 either: the decoder
  marks `NOEXEC_INST` and `GenerateIR` emits a faulting block (`NoExecOp`).
* `CompileBlock` only returns 0 when `GenerateIR` gets a dispatch error on the
  **first** instruction (`Core.cpp:763`).

So the RIP that failed is not `0x7001a14338`. The likely explanation: the
dispatcher calls `CompileBlock` from **two** places, both followed by
`br x0` without a null check:

| path | call | compiles |
|---|---|---|
| `NoBlock` | `blr x4` â†’ `CompileBlock` | `State.rip` (just loaded) |
| `ExitFunctionLinker` | `blr x2` â†’ `Arm64JITCore::ExitFunctionLink` â†’ `CompileBlock` | `Record->GuestRIP`, the target of a **direct** JMP/CALL |

A linked direct branch never writes `State.rip`. The reported value is exactly
the address after the `syscall` of the previous libkernel stub (`0f 05` at
`â€¦336`, then `c3` at `â€¦338`), i.e. where `ProbeAddressSpace::HandleSyscall`
left it (`state.rip += 2`). FEX then ran the RET and the caller's code inline,
up to a direct JMP/CALL to a block it could not compile. That block's RIP is
in the jump thunk's `ExitFunctionLinkData`, not in `State.rip`.

Also: with `HostCode == 0`, `ExitFunctionLink` still patches the thunk to
`ldr x0, [HostCode]; br x0` (`JIT.cpp:633`), so a second thread reaching that
branch would also jump to 0. The thread stop on the first fault prevents it in
practice.

### Diagnostics added

`GuestFaultGuard` now also records, on a null dispatch:

* `lr-insn` / `dispatch`: the instruction before LR, `blr x4` =
  `compile-block`, `blr x2` = `exit-link`. On device this settles which path
  returned 0.
* `callret-rip` / `callret-host`: the top of FEX's call-ret stack (bounds
  checked against `CallRetStackBase`).
* `link-rip` / `link-host` / `link-bytes`: for a direct CALL, the `bl` before
  `callret-host` leads to the jump thunk. The thunk shape and its
  `CallerOffset` are validated. Its `GuestRIP` is the CALL target and
  `link-host=0x0` shows that compilation failed. The fault address
  (`addr=`, and the app message) then becomes that target instead of the
  stale `State.rip`.

For a direct JMP (no call-ret entry), `link-rip` is absent and `addr=` stays
`State.rip`; `dispatch=exit-link` still tells that this address is stale.

### Regression tests

* `PS4FEXLinkedExecutionTest` (native ARM64 FEX): three guest apps that reach
  `66 cf` (IRET with an operand-size override, which FEX decodes but rejects
  with `DecodeFailure`), so `CompileBlock` really returns 0:
  * via a direct CALL: contained as SIGILL `0x4d50`. `address` is the CALL
    target (`code+6`). `rip` is the startup stub after the image: the entry
    itself is reached by the stub's linked `JMP entry`, so `State.rip` never
    reaches the app's code. This reproduces the device pattern (CI, macOS
    ARM64: `address=0x7000004006 rip=0x7000128000`);
  * via a direct JMP inside the entry, and at the entry (reached by the
    stub's JMP): contained, with `address` = `rip` = the stub (a JMP target
    is not recovered).
* `FatalFaultLogTest`: `dispatch_call`, `bl_target`, thunk validation and the
  new line fields. Also fixes the device-record check, which searched for a
  literal `\n` (two characters) and failed the services job.

### Next step

Install the new IPA, reproduce, and send the new `contained-host-fault` line.
`link-bytes` (or, for a JMP, the guest code around `callret-rip`) gives the
instruction FEX rejects; support for it goes into a separate change.

## Device report after 88af4d6 (2026-11-12)

```
contained-host-fault signal=4 code=19792 addr=0x7001a14338 pc=0x0(other) lr=0x102be4190(dispatcher) ...
guest-rip=0x7001a14338 guest-bytes=c34989cab8110000000f05c34989cab8 lr-insn=0xd63f0040 dispatch=exit-link
callret-rip=0x7000c67e17 callret-host=0x1122f9a80
```

- `lr-insn=0xd63f0040` (`blr x2`) proves the null block came from
  `Arm64JITCore::ExitFunctionLink`, not CompileBlock. ExitFunctionLink
  compiled `Record->GuestRIP`, a direct-branch target. `State.rip`
  (0x7001a14338, the RET after an HLE SYSCALL) is stale. The RET is not the
  failing instruction.
- `callret-rip` / `callret-host` are the top of FEX's call-ret shadow stack
  (BranchOps.cpp: `adr TMP1, l_CallReturn; stp RetReg, TMP1, [callret_sp, -16]!`):
  - `callret-rip` 0x7000c67e17 is the guest return address of the innermost
    active direct CALL (a 5-byte `call rel32` at 0x7000c67e12). It is neither
    a branch target nor the failing address.
  - `callret-host` is the host PC of that call's return block, a JIT address.
- `link-rip` is missing. The `bl` before `callret-host` does not lead to a
  jump thunk that holds HostCode 0, so that CALL is not the failing branch:
  it was already linked and its callee is running. The failing branch is a
  JMP/Jcc, or a CALL without a known return block, inside that callee. Such
  a branch pushes no call-ret entry.

### New diagnostic: null-link scan

When CompileBlock returns 0, ExitFunctionLink computes `BranchOffset = 0 -
caller/4`. That offset is out of `b` range, so FEX stores HostCode 0 in the
record and rewrites the first word of the thunk to `ldr x0, #0x10` (JIT.cpp).
That mark stays in the code buffer for JMPs too.

How the scan works:
- The handler finds the bounds of the code buffer page by page with
  `IsAddressInCodeBuffer`. It starts from the call-ret host PC, or else from
  an entry in FEX's L1 cache.
- It scans the buffer for such thunks and checks that the caller at
  `thunk + CallerOffset` is a `b`/`bl` to that thunk.
- The line gains `null-links=N null-link-caller= null-link-kind=jump|call`,
  and `link-rip`/`link-bytes` give the guest target that FEX refuses.
  `addr=` becomes that target.

Tests:
- FatalFaultLogTest covers the scan on a synthetic buffer.
- PS4FEXLinkedExecutionTest (ARM64 FEX on macOS) now expects kind 6 (JMP to
  `66 cf`) at `code+3` and kind 7 (entry reached through the stub's JMP) at
  `code`, instead of the stale RIP.

Not fixed: the cause stays unknown until the device reports `link-bytes`.

## `sceKernelUsleep(1000) = -1` (2026-11-13)

This is not a failing service.
- Both NIDs share one service, so recent calls named both `sceKernelUsleep`:
  - `1jfXLRVzisc`, sceKernelUsleep: returns an SCE code (0 or `0x8002xxxx`).
  - `QcteRwbsnV0`, libc `usleep`: POSIX, returns -1 + errno.
- `PS4Threads::sleep` fails only with EINTR, and EINTR only happens once
  `PS4Threads::interrupt()` has run. That happens at a stop (time limit,
  guest fault, exit), not at a VideoOut pause. A guest `usleep` returning -1
  therefore means a stop was already underway when the calls were captured.
- The stop report now tags these entries `[POSIX: -1 + errno]`.
- VideoOut pause vs. a stuck flip is handled separately (63f027b, 03e9a58,
  b08a616).

## Full device line after 5547456 (2026-11-13) and the JIT-reuse diagnostics

```
... dispatch=exit-link callret-rip=0x7000c67e17 callret-host=0x1128a2040 callret-prev=0x94000a8e
exit-link-trace=direct-bl null-links=0 scan-anchor=0x1128a2040 scan-begin=0x11257c000
scan-end=0x113578000 scan-pages=4092
```

- **H1 (scan missed the buffer): ruled out.** The scan covered
  0x11257c000..0x113578000: 16 MiB minus 16 KiB, all 4092 pages readable.
  That is the whole iOS JIT buffer (`MAX_CODE_SIZE = 16 MiB`, reused in place,
  never a second buffer). No thunk carries the HostCode-0 mark that every
  ordinary failure of `CompileBlock` under `ExitFunctionLink` leaves.
- **H2 (in-place reuse during ExitFunctionLink): still open.**
  - On iOS, `CompileCode` calls `ClearCodeCache(Thread, false)` when the
    buffer is full, and that reuses the same memory.
  - `ExitFunctionLink`'s guard (`LookupCache->Shared != CodeBuffer->LookupCache`)
    cannot see an in-place reuse.
  - It then writes the thunk and the caller inside memory that was just
    rewritten.
- **H3 (another thread cleared the buffer before the scan): still open.**
  MaxPS4 does not install FEX's `BeginBufferInvalidationSafepoint` /
  `OnBufferReusedInPlace` hooks.

### New fields (branch experiment/jit-cache-reuse-diagnostics)

MaxPS4 replaces `CpuStateFrame::Pointers.ExitFunctionLink` for each thread with
a pass-through wrapper (`exit_link_diag::wrapper`).
- What it does:
  - copies the thunk and its record before the call;
  - calls FEX's function unchanged and returns its result;
  - classifies the record afterwards.
- FEX only ever rewrites word 0 and HostCode. Any other change means the
  memory was reused during the call.
- It makes no FEX change and no JIT or fault-handling change. It uses no locks
  and no allocation. The post-call read is probed with a non-blocking pipe.

| Field | Meaning |
|---|---|
| `efl-rip` | real GuestRIP passed to ExitFunctionLink by this thread (the target FEX refuses) |
| `efl-ret` | HostCode returned (0 → the null dispatch) |
| `efl-after` | record right after the call: `intact`, `linked` (FEX's own edit) or `overwritten` (**H2**) |
| `efl-now` | record at fault time vs. right after the call: `overwritten` → **H3** |
| `efl-ow=a..b` | global overwrite count before and after this call |
| `efl-calls`, `efl-zero`, `efl-overwrites`, `efl-unreadable` | global counts since launch |

Reading:
- `efl-after=overwritten` proves H2.
- `efl-after=linked` with `efl-now=overwritten` points to H3.
- `efl-after=linked` with `efl-now=linked` and `efl-ret=0x0`: no reuse. FEX
  really refuses the guest code at `efl-rip`. Decode it with that address.

Cost: two system calls per ExitFunctionLink call (first link of each
branch only).

## Step 2: which branch reached pc=0 (`ind-*` fields)

The device line after PR #305 showed `efl-zero=0` and `efl-ret=0x113bca384`:
the last ExitFunctionLink of the faulting thread returned a non-null host
address, so the dispatcher's `br x0` that followed it did not branch to 0.
The `dispatch=exit-link` label only proves that no BL/BLR ran since that call
returned. JIT inline indirect branches keep LR too and have no null check
(FEX f2b679f, `BranchOps.cpp:186-231`):

```
RET: ldp x0, x1, [x25], #0x10 ; sub x0, x0, rip ; cbz x0, hit
L1:  ldp x1, x0, [L1 + (rip & mask) * 16] ; sub x0, x0, rip ; cbz x0, hit
hit: ret x1 / br x1
```

A match leaves `x0 = 0` and branches to `x1`. An entry `{guest 0, host 0}`
(call-ret: a CALL without a return block pushes `stp xzr, xzr`; L1: a slot
never filled) matches only guest RIP 0, which maps to L1 slot 0.

Register roles in FEX f2b679f (non-arm64ec): `x28` STATE, `x25`
REG_CALLRET_SP, `x0`/`x1` TMP1/TMP2, `x8` guest RSP (`x64::SRA[REG_RSP]`).
Re-check them whenever FEX is updated.

| Field | Kind | Meaning |
|---|---|---|
| `ind-x0`, `ind-x1`, `ind-x8`, `ind-x25`, `ind-x28` | observed | signal-context registers |
| `ind-frame` | observed | `match` when x28 is this thread's CpuStateFrame |
| `ind-callret-sp` | observed | `State.callret_sp`, spilled at the last dispatcher call |
| `ind-callret-delta` | observed | `x25 - State.callret_sp`: `+0x0` no pop, `+0x10` one RET pop |
| `ind-popped` | observed or `na` | `guest:host` at `x25 - 0x10` (bounds-checked, probed) |
| `ind-l1-0` | observed or `na` | L1 slot 0 `host:guest` (probed) |
| `ind-rsp-slot` | observed or `na` | guest qword at `x8 - 8` (guest view check + probe) |
| `ind-target` | reconstructed | guest RIP implied by the matched entry, or `unknown` |
| `ind-path` | hypothesis | see below; never a confirmation |

`ind-path` values: `cand-ret-callret-zero` (one pop of `{0,0}`, x1 = 0),
`cand-ret-l1-zero` (one pop that missed, then L1 slot 0 `{0,0}`),
`cand-l1-zero` (no pop, L1 slot 0 `{0,0}`, dispatcher excluded by a non-null
`efl-ret`), `cand-l1-zero-or-dispatcher` (same, dispatcher not excluded),
`dispatcher-or-unknown`, `ret-unexplained`, `callret-delta-other`,
`other-branch` (x0 != 0), `state-mismatch` (x28 is not the frame).

A `cand-*` path needs supporting evidence before it counts as a cause:
`ind-rsp-slot=0x0` for a RET, and the guest code at `efl-rip` showing a RET
or an indirect JMP that can reach it.

## Step 5: root-level PRX files of /app0

Static analysis of the eboot (CUSA53178) showed the cause of the jump to 0.
The loader at eboot 0x669360 calls
`sceKernelLoadStartModule("/app0/pancake_libretro_ps4.prx")`. MaxPS4 only
prepared `sce_module/*.prx`, so the HLE call returned ENOENT. The game's
wrapper then returned a NULL handle, every `retro_*` dlsym returned 0, and
`retro_api_version()` was called through `jmp [rdi+0x18]` (eboot 0x6696c0)
with a null pointer.

`maxps4_app_modules.h` now discovers modules as follows:

- regular `*.prx` files directly in `/app0` and in `/app0/sce_module` (no
  other subfolders);
- identified by file name; `sce_module` wins over the root of the same folder;
- the update folder replaces the game's modules of the same name and adds its
  own.

Root-level modules are optional:

- unreadable files, files the Orbis metadata parser rejects, and non-PRX files
  are skipped;
- a root-level file that provides the same module as one already kept is not
  prepared. MK ships two variants of module `libmipsaot`
  (`SLUS00330_TRILOGY.EXE` and `SLUS00824`, one per PS1 title, same exports),
  and the linker refuses duplicate exports, so the first one by name is kept;
- if preparation still fails with them, the game is prepared again without
  them, as before (stderr keeps the reason).

`sceKernelLoadStartModule` and `sceKernelDlsym` are unchanged:

- they already match modules by file name and look exports up by NID;
- the 25 `retro_*` NIDs match the 25 exports of `pancake_libretro_ps4.prx`.

Limits:

- modules are prepared and their `module_start` runs at boot, not when the
  game calls LoadStartModule;
- only one variant of a module can be loaded per boot: the second `libmipsaot`
  title stays unsupported;
- eboot imports exported by root-level modules (Party, PlayFab) now resolve to
  those modules instead of import-probe stubs;
- the guest image grows (CUSA53178: about 94 MB validated instead of 21 MB).

## Step 6: every libmipsaot variant, selected by the game's path

MK Legacy Kollection ships one `libmipsaot-*.prx` per PS1 title. Both provide
module `libmipsaot` with the same exports. The eboot loads the one it needs
with its full path (`/app0/libmipsaot-SLUS00330_TRILOGY.EXE_ps4.prx` or
`/app0/libmipsaot-SLUS00824_ps4.prx`) and reaches it only through
`sceKernelDlsym`. No module imports `libmipsaot` or `pancake_libretro_ps4` by
NID.

- `OrbisLinker::Input::privateExports`:
  - the module's exports are published per module (`Image::exported`, used
    by Dlsym) but are left out of the NID import table;
  - the duplicate-export check still applies to every other module;
  - an import served only by private modules stays unresolved.
- `choose_modules` (`maxps4_app_modules.h`):
  - optional modules that provide the same module are all prepared with
    private exports when no other module imports one of their libraries;
  - otherwise none of them is prepared;
  - a root-level file never replaces a required module.
- The HLE `sceKernelLoadStartModule` is unchanged. The file name in the
  game's path selects the variant's handle, and Dlsym on that handle returns
  that variant's addresses.

Local static check of the 11 modules of CUSA53178 (analyzer with the flag set):

- 96063/96063 relocations applied, 2295/2295 NID imports resolved;
- each `libmipsaot` publishes its 9 exports at its own base (0x4600000 and
  0x4c00000).

Not verified yet:

- that MK Trilogy still runs with both variants present (larger image, both
  `module_start` run at boot);
- that SLUS00824 runs at all.

Both need an iPhone test.
