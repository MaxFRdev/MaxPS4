# Mortal Kombat Trilogy CUSA53178 — iPhone crash triage (2026-11-18)

## Observed on device
- MaxPS4 build 1000187, LiveContainer + StikDebug JIT active.
- Exit -14, backend 0, 0 frames; Metal awaiting first render.
- SIGSEGV signal 11 code 2, fault address 0x38, guest RIP 0x7003f31320, relative RIP 0x4331320, relocation 0x6fffc00000.
- Direct memory allocation succeeded for physical 0x80000000 (2 GiB); subsequent `sceKernelMapDirectMemory` failed with guest code `0x8002000c`.
- `/app0/assets/anims.pie` opened and read; missing libkernel NID `GBUY7ywdULE` was called once.
- iOS denied the preferred guest memory zone 0x1000000000 (code 3), but a 3 GiB alternate window was reported.

## Code investigation
`PS4Libkernel::memory_service` in `MaxPS4-iOS/Native/maxps4_ps4_libkernel.cpp` checks whether the requested physical span belongs to a direct allocation before calling `dynamic_->place(...)`. The return code alone does not distinguish guest physical range validation, host virtual address reservation, alignment or fixed mapping collisions.

This branch adds a failure-only diagnostic on `dynamic_->place` in `S::direct_map`, logging guest hint, size, physical offset, owning allocation, requested alignment, protection, fixed flag and returned error. **No mapping fallback, changed return code, or allocation policy change.** Logs go to stderr; whether they are included in the in-app report must be verified.

## Next evidence needed
1. Capture the new `[MaxPS4 direct_map]` line from the native log on device.
2. Capture x86-64 registers and instruction bytes around guest RIP `0x7003f31320` at SIGSEGV; fault address `0x38` is consistent with, but does not prove, a null-base dereference.
3. Resolve `GBUY7ywdULE` from the game's libkernel import metadata and identify the calling module before adding any stub.
4. Verify if the crash reproduces after a successful mapping, or whether mapping failure is causally related.

## Validation
No iPhone execution or local Xcode build was performed. CI status must be checked separately. Do not merge without validating that stderr diagnostics are captured and the game still behaves identically apart from logging.
