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

## Experimental fix (2026-11-18)
Hypothesis: the 2 GiB `sceKernelMapDirectMemory` request does not fit in the 3 GiB mmap window left under LiveContainer, `place` returns ENOMEM (`0x8002000c`), the game ignores the failure and dereferences null (fault address `0x38`).

`PS4VirtualMemory::place` now falls back, for non-fixed requests that the window cannot hold, to an aligned host span outside the window (`outside` region, released with `munmap`). The move is recorded with status `-2` in the session report's moved mappings. Risk: this consumes address space iOS would otherwise leave for FEX per-thread lookup caches; a later thread crash would point there. Needs on-device validation.

## Device result and follow-up (2026-11-19)
With the outside-window placement (`0x0 + 0x80000000 -> 0x70dd400000 (code -2)`), the game no longer crashes: it reaches the title flow at 60 FPS. After a button press it starts the intro videos; a `pthread_create` then returns EAGAIN (35), libvpx logs `vpx_codec_decode() returned [1]` (its tile-worker creation failure) and the game traps (SIGILL).

Cause: the 2 GiB placed outside the window leaves ~1.3 GiB of the 3.3 GiB spare; each new thread first needs `fexLookupCacheBytes` (112 MiB) of address space, so only ~11 threads fit. The per-thread FEX lookup cache is reduced to 64 MiB (page table for 8 GiB, 32 MiB of block pointers), about 20 threads in the same space.

With 64 MiB per thread the intro videos start creating their decoder threads, but around guest thread 25 FEX's own lookup-cache reservation fails and FEX writes through `MAP_FAILED` (fault address `0x301a107` = MAP_FAILED + the L1 offset). Per-thread lookup cache now 36 MiB (4 GiB page table, 24 MiB of block pointers, 256 Ki L1 entries); thread creation also requires 32 MiB of spare room for code-buffer growth, so running out returns EAGAIN instead of a FEX crash.

## Menu reached (2026-11-19, build e565565)
With 36 MiB per thread the intro videos play (audio audible), the game menu and its game carousel render (~46 FPS) and the player can pick Mortal Kombat Trilogy. Its embedded PS1 emulator (pancake/libretro) then loads the BIOS and disc image, but `sceKernelReserveVirtualRange(hint 0x8000000000, 0x1fc80000)` (508 MiB, non-fixed) fails with ENOMEM inside and outside the window, and the core logs `HW render: callback setup error`. Refusals are now recorded with the largest free span left in the window and the largest span iOS still grants, to tell whether the window or the host address space is exhausted.

Refusal diagnostics on device: largest window gap 467 MiB, largest iOS span between 254 and 508 MiB, for a 508 MiB request; the PS1 core then logs the HW render error and crashes (SIGSEGV at 0xb5f6c). The menu itself runs at 60 FPS. Thread slots go from 64 to 48 (peak observed ~33 live guest threads) and their space extends the window by ~80 MiB inside the same reservation, so the gap at the window's end can hold the reservation without taking address space from outside.
