# Codex task — FEXCore / Metal performance

Base: `experiment/fex-metal-compat-performance` (PR #289). Work only on this branch; do not merge.

1. Inspect the actual guest execution and Metal presentation paths; distinguish synthetic test pipelines from PS4 game rendering.
2. Run existing tests, inspect the new FEX p50/p95 and Metal readback timing logs; document environment and baseline before changing runtime code.
3. Propose a small, measurable optimization with a regression test. Preserve guest/host memory translation, W^X, resource lifetime, and engine cleanup. Avoid unsupported performance claims.
4. Run relevant CI and produce a draft PR into `experiment/fex-metal-compat-performance` describing changed files, tests, performance evidence and rollback.
5. Never change or publish `MaxPS4-Beta`; preserve reported Sonic Mania and Mortal Kombat device behavior as a release gate.

Do not infer device FPS from CI measurements. Request physical iPhone testing for game claims.
