# FEXCore / Metal optimization evidence

This work is isolated on `experiment/codex-fex-metal-optimization`, based on
`experiment/fex-metal-compat-performance` at `8e600f8`. It does not change or
merge `MaxPS4-Beta`.

## Baseline before runtime changes

The successful base run `42954364250` used the macOS 15.8.2 ARM64 runner image
`macos-15-arm64` version `20261104.0514.1`, Xcode 16.4, and the pinned FEXCore
runtime in a DEBUG build. Its normal integration pass reported:

- 24 synthetic FEX invocations: p50 0.45 ms, p95 0.54 ms;
- three 128x128 native Metal draws, GPU waits and CPU readbacks: 102.20 ms.

The leak-instrumented repetition reported p50 0.59 ms, p95 0.71 ms and
268.60 ms for Metal, with zero leaked bytes and zero remaining JIT leases.
Those instrumented numbers are recorded separately and are not compared with
the normal pass.

A fresh pre-change run on this isolated branch, `42956252913` at `d143d88`,
also completed successfully. Its normal pass reported p50 0.46 ms, p95
0.50 ms and 89.95 ms for the three Metal draw/readback operations. Under
`leaks` it reported p50 0.58 ms, p95 0.70 ms and 133.90 ms, again with zero
leaked bytes. The companion PM4/Metal run `42956253856` completed successfully.

These measurements are diagnostics, not game-frame timings. The FEX fixture
opens, initializes, executes and closes an engine for every invocation. The
Metal fixture uses a native quad, blocks after every command buffer, reads a
texture back to the CPU and scans its pixels. It does not execute PS4 shaders,
the game PM4 bridge, an MTKView presentation, or an iPhone workload. No FPS or
game-compatibility claim can be inferred from it.

## Profiled production paths

The game path opens one FEX engine and remains in `run_orbis`. Every admitted
HLE syscall reaches `GuestELF::hle_gate`, which currently scans every
linker-created trampoline and translates each candidate until it finds the
matching RIP and token. Libkernel-heavy games make frequent lock, condition,
time and TLS calls, so a sorted address lookup is a bounded optimization of a
real FEX-to-host transition.

The real GPU path is `MaxPS4GameSession` -> FEX GNM callback ->
`MaxPS4GameMetalBridge::submit` -> `PM4MetalRenderer` -> display copy. The
bridge currently enables per-program GPU timing on the first batch and every
ten seconds. That diagnostic ends the render batch, commits, waits for GPU
completion and creates another command buffer after every draw. It runs on the
guest submission thread even when the developer profiler is disabled.

## Planned bounded changes

1. Sort the immutable HLE trampoline table once and replace the linear gate
   scan with an exact binary address lookup followed by the existing token and
   RX translation checks.
2. Make the draw-by-draw Metal timing opt-in through the existing developer
   profiling setting. Required compute barriers, queue back-pressure, display
   events and resource ownership remain unchanged.

Tests must cover large HLE tables and boundary rejection, default/explicit
Metal profiling behavior, identical pixels, iPhone ARM64 compilation, W^X,
guest/host translation, engine cleanup and leak checks. Physical iPhone tests
of Sonic Mania and Mortal Kombat remain release gates; CI measurements are not
a substitute.
