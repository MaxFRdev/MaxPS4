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
