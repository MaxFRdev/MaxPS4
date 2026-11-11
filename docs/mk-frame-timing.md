# Mortal Kombat 55–60 FPS jitter: HLE / frame timing (issue #293, Claude track)

Opt-in diagnostics for the FEXCore / memory side. Nothing here claims a
bottleneck yet: the numbers below must come from a device run.

## How to collect

Enable the existing **profiling** option for the game (the same switch as the
CPU profile; off by default). The profile report shown by the app now ends with:

```
Images (600 dernières) : 58.7 FPS · médiane 16.67 ms · p95 18.90 · p99 24.10 · max 33.50 · >17,5 ms 41 · >33,4 ms 1
HLE (temps hôte cumulé, attentes incluses) : sceKernelWaitEqueue 3600× 2400.0 ms (moy 666.7 µs, max 16.80 ms) · …
```

* **Images**: intervals between flip requests (`sceVideoOutSubmitFlip`,
  `sceGnmSubmitAndFlipCommandBuffers`, its workload variant), last 600 (~10 s).
  Median, p95, p99, max, and how many frames missed 60 FPS (> 17.5 ms) or 30 FPS.
* **HLE**: per service, calls, total host time (blocking waits included), average
  and worst call. The top 10 by total time.

Record one repeatable combat segment, note device model, iOS version and
thermal state, and compare with Sonic Mania (expected: p95 ≈ 16.7 ms).

## Cost

Disabled: one relaxed atomic load per HLE call. Enabled: two `steady_clock`
reads and three relaxed atomics per call, no lock, no allocation, no logging;
the report is built only when asked.

## Reading the result

| Observation | Likely cause | Next step |
|---|---|---|
| Slow frames line up with a high max on a wait service (equeue, cond, sem, `gnm_wait_flip`) | the game thread waits for GPU / Metal completion or another guest thread | Codex track: Metal completion / drawable waits |
| Slow frames without HLE waits, CPU profile shows a high *FEX dispatcher* / host share | JIT compilation of new code (new combat moves, effects) or block lookup misses | warm-up, larger code cache, check lookup cache size |
| High `sceAudioOutOutput` max | audio output blocks longer than one buffer | check audio pacing |
| Memory services (mmap/munmap/mprotect) in the top list during combat | per-frame allocation churn | cache mappings |

## Hot-path review (code, not measurements)

* `HandleSyscall` (every HLE call): activity slot scan (32 atomics), two
  `now_ns()`, a mutex for the "recent calls" ring on non-frequent services.
  Cheap and uncontended in practice; left unchanged until measured.
* Removed: a 256-byte buffer zeroed on every HLE call although only used in trace mode.
* `PS4VirtualMemory::translate` is a binary search + page scan, without lock;
  fine for HLE argument checks.
* JIT safety (W^X, `sys_icache_invalidate`) untouched.
