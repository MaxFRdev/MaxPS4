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
the dispatcher does `br x0` → host `pc=0`, LR still just after the `blr`
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
en charge par FEX à RIP …" instead of being killed. The game **still does not
run**: the instruction must be supported next.

The handler also appends a `contained-host-fault` line with up to 16 bytes at
the guest RIP (`guest-bytes=…`) to `Documents/maxps4-fatal-fault.txt`.

## Next step

Retest on device, read `guest-bytes`, disassemble them, and add FEX support for
that instruction. Then retest MK Trilogy and Sonic Mania.

## iPhone retest: curtain-to-black-screen (2026-11-12)

The SideStore IPA from #298 (`2ee8e6c`) no longer exits to SpringBoard when selecting **Legacy Kollection → Mortal Kombat Trilogy → Play**. The menu and curtain-closing animation render, followed by a persistent **black screen and no audible sound**, even after 1–2 minutes. This is **not** a verified game fix. See issue #296 for the full user report.

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
