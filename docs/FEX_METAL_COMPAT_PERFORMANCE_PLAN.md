# FEXCore + Metal: performance and compatibility workstream

Status: investigation / no runtime optimizations claimed.

## Baseline to preserve
- Keep the working MaxPS4-Beta release unchanged.
- User-reported device results: Sonic Mania at 60 FPS, Mortal Kombat playable, no observed crashes. These are reports, not reproducible CI benchmarks.
- Build an iOS IPA and run the existing FEXCore/Metal/PKG regression tests before merging.

## Initial code review
- `MaxPS4MetalPipeline.swift` contains a native smoke-test triangle and textured quad, not a general PS4 GPU implementation. Optimize only after profiling actual guest-frame paths.
- `MaxPS4FEXMetalState.swift` serializes FEX execution and passes bounded value snapshots to Metal. Preserve the safety boundary; measure queue wait, guest execution time and frame delivery separately.
- `PS4PKGBridge.cpp` handles read-only PKG inventory with file-identity and range checks. Do not confuse recognizing a PKG with decrypting or running its game.

## Work in order
1. Add opt-in low-overhead timing around guest execution, frame submission and presentation; report p50/p95 and frame drops.
2. Add reproducible tests for memory lifetime, guest-to-host translation, GPU synchronization and invalid PKG inputs.
3. Profile on device before changing FEXCore or Metal resource lifetimes. Keep rollback and W^X guarantees.
4. Expand compatibility only for validated, legally obtained test cases; record title, device, iOS version, result and logs.
5. Require green CI, IPA signature audit and a repeat device smoke test for Sonic Mania and Mortal Kombat before promotion.

## Release gate
No merge into the working beta until performance comparisons and regressions are reviewed. A successful CI build alone does not prove game compatibility.
