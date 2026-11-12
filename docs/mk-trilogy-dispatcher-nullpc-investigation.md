# MK Trilogy host null-PC: dispatcher investigation (experimental)

Related: #296, #298, #300. This is an investigation plan, **not a crash fix**.

## Evidence from physical iPhone

- After selecting **Mortal Kombat Trilogy** → Play, the host reports `SIGSEGV`, fault address `0x0`, host `pc=0x0`.
- Two device runs report `lr=dispatcher_base+0x190` despite ASLR moving the absolute dispatcher address.
- Guest thread 14, last stored guest RIP `0x7001a14338` on both runs. This is **not necessarily the faulting guest instruction**.
- In the second run, `x16=0x76052a1c30`, `x17=0x701de012c8`. Neither is proven to be the branch target.
- A PM4 resource-scanning stack was captured on a **different** thread; GPU causality is unproven.

## Investigation gates

1. Preserve the exact IPA build SHA, matching dSYMs, original private .ips, generated dispatcher base/end, and JIT code-map metadata. Do not publish device identifiers from the .ips.
2. Dump/disassemble the **matching generated AArch64 dispatcher bytes** around offset `0x18c` (and sufficient preceding instructions to reconstruct the control flow). A return LR at `+0x190` can point **after** the call instruction; do not label `+0x190` the faulting branch without disassembly.
3. Identify the exact `blr`/`br`/return instruction and its source register or memory slot. Correlate with register snapshot and stack validity, then trace how the target is populated.
4. Map guest RIP `0x7001a14338` against the actual loaded ELF/PRX ranges and ASLR offsets. Verify the source of the saved RIP before inferring the guest instruction.
5. Audit dispatcher and JIT cache lifetime, thread-local state, callback addresses, mapping invalidation, and guest↔host transition paths. Check differences across #298/#300 and earlier known-good IPA builds; do not attribute regression without evidence.
6. Once a concrete null-target provenance is identified, add a **minimal failing regression test** and fix at the ownership/validation boundary. Do not hide faults with a blanket null-branch skip or signal recovery.
7. Run focused Linux/macOS tests, iOS IPA packaging, then repeat physical-device launch with profiling off. Confirm Sonic Mania baseline and MK Trilogy behavior separately.

## Safety / release gate

Only work in experimental branches based on `experiment/claude-codex-integration`. Keep `main` and MaxPS4-Beta untouched. No merge or release until device validation. Green CI alone does not establish that the crash is fixed.

## Current status

The two runs narrow the investigation to the dispatcher return site but do **not** prove the null branch target's provenance. No code fix or on-device validation is claimed here.
