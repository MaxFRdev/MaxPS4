# macOS compatibility validation — 2026-10-30

Tested source commit: `58b569b835c03f947acf14913790985c01bf3618`, published on
`codex/macos-compatibility-20261030`. The validation worktree preserves the
original working branch and its local changes. Existing repository workflows
were dispatched manually; no application packaging workflow was launched.

## Graphics and iPhone compilation

The [PM4 workflow](https://github.com/MaxFRdev/MaxPS4/actions/runs/40988354863)
passed both jobs: Linux CPU checks and the macOS Metal/Swift bridge followed
by iPhone ARM64 compilation. The Metal runner reported macOS 26.6.2,
image `macos-26-arm64`, version `20261013.0424.1`.

The completed Metal job contains 84 pixel-comparison results with a maximum
channel difference of 1 on a 0–255 scale, within the two-level tolerance.
There are no skipped-test messages in that job. In particular:

- 16/32-bit index-offset draws: channel difference 0.
- Triangle-fan scenes: channel difference 0.
- Restarted fans/strips: channel difference 0.
- All 24 blend scenarios passed, including constant colour/alpha, inverses,
  saturation, clamping and NaN handling.
- Changed constants with a cached pipeline and batched encoder: difference 0.

These are actual Metal rendering/readback comparisons, not compile-only
results. The same job compiled the changed renderer and bridge for the iPhone
SDK and checked the Swift frontend. The compiler reported existing Swift
deprecation/concurrency warnings; the job succeeded.

## System and CPU execution

The [system-services workflow](https://github.com/MaxFRdev/MaxPS4/actions/runs/40988358263)
passed all five jobs: FEX ARM64-native execution, the FEX instruction simulator,
iPhone ARM64 compilation and both Linux/macOS service suites.

All 24 service tests and both C ABI compilation probes succeeded on macOS under
ASan/UBSan, with no skipped-test messages. This includes the new general-image
reservation/fixed-direct-map case and the tests that were unavailable or failed
under Cygwin. The macOS suite finished within its ten-minute limit. All 17
modified source/test/script files were also checked to match the published
source snapshot exactly; subsequent local changes only record validation.

The complete logs are retained locally as
`build/toolchain/macos-metal-58b569b.log` and
`build/toolchain/macos-services-58b569b.log`.

## Scope

The subsequent [BC1 validation](https://github.com/MaxFRdev/MaxPS4/actions/runs/40991103330)
passed on source commit `7473e31`. It added actual BC1 Metal sampling, a
translated textured scene, native-texture fallback and iPhone compilation.
Both rendered BC1 images matched the software output with zero channel
difference. See [the BC1 report](gcn-bc1.md) for scope and CPU/sanitizer results.

The initial snapshot `58b569b` includes the 22 prepared source, test and
documentation files. BC1 added eight changed/added files, followed by its
runtime correction. All 23 modified source/test/script files match the final
tested commit `7473e31`. Local workflow-definition edits were not included; the runs use the
existing workflows. Standalone added CPU tests were already checked locally,
and their rendering scenes were exercised by the updated Metal regression.

This validates synthetic scenes and system fixtures. It does not demonstrate
Spyro Reignited Trilogy gameplay or execution on the user's iPhone 15 Pro Max.
No IPA was built or installed by these runs.
