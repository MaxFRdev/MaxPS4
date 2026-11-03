# Linear BC4/BC5 SNORM textures

Extends BC4/BC5 (AMD formats 38/39) with number format 1, SNORM. Existing
UNORM operation and BC1/BC2/BC3 restrictions are preserved.

The shared interpreter/C++/Metal helper decodes signed 8-bit endpoints to
[-1,1], maps -128 to -1, and interpolates with signed arithmetic. Palette mode
uses the original signed endpoint order, including -127 > -128 despite both
normalizing to -1. The ascending/equal palette has explicit -1 and +1 entries.
The layout and endpoint rule were checked against Microsoft's
[DirectXTex BC4_SNORM decoder](https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/BC4BC5.cpp)
and [block compression documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression).

BC4 exposes R; BC5 exposes independent R and G. Missing channels remain zero,
alpha remains one, and swizzling/filtering operates on decoded signed values.
The existing block addressing, padded pitch, cache and checked/resolved
footprint logic are reused. SNORM is accepted only for BC4/BC5 in this change.
Other number formats, tiled layouts, mip chains and compressed writes are not
added. No normal reconstruction is performed by the texture decoder.

## Local verification, 2026-10-30

Cygwin GCC 14.4, C++17, warnings treated as errors, without sanitizers:

- GCNBC45Test runs UNORM and SNORM. Fixed independent palettes cover both
  endpoint orders, equal endpoints, -128/-127 in both orders, mixed signs,
  all selectors, the cross-word selector, and independent green selectors.
- Cached/uncached interpreter and translated runtime loads agree with those
  palettes. Swizzling, cross-block filtering, padded/partial blocks, cache
  read fallback, invalid descriptors and truncated final blocks pass.
- GCNTextureFootprintTest passes 1,843,200 exact checked/resolved comparisons,
  now including number format 1 across the complete matrix.
- BC1/BC2/BC3 and interpreter regressions pass; generated C++ shader execution
  matches the interpreter lane for lane.

Logs: build/toolchain/test-results/*-snorm.log.

## macOS/Metal and Linux CI results

GCNTextureFootprintMetalTest adds signed BC4/BC5 at 16x8 and 256x128 with
independent R/G data, checked/resolved equality and CPU/GPU channel comparison.
PM4MetalRendererTest adds signed translated pixel-shader scenes, with native
textures disabled/enabled and software image comparison. Negative decoded
values are tested directly by the sampling test, before UNORM target clamping.

Published commit `1eb8d1337ead6b19f613d91462f8927f59fb5477` passed
[run 40997530350](https://github.com/MaxFRdev/MaxPS4/actions/runs/40997530350):

- Linux ASan/UBSan regressions passed, including 1,843,200 checked/resolved
  comparisons and generated C++/interpreter equivalence.
- Actual Metal BC4/BC5 SNORM sampling passed at both sizes with CPU/GPU channel
  comparisons. The negative values are compared before render-target clamping.
- Both SNORM translated pixel-shader scenes and native-texture fallback scenes
  had zero channel difference from software output. No Metal tests skipped.
- iPhone ARM64 SDK compilation and Swift checks passed.

The standalone GCNBC45Test remains locally validated; the published workflow
executes the expanded footprint and Metal tests, rather than that standalone
suite. Logs: build/toolchain/macos-snorm-1eb8d13.log and
build/toolchain/linux-snorm-1eb8d13.log.

No physical iPhone execution or IPA packaging was performed. Spyro remains
unverified. This record was updated locally after the run.

Publication payload (eight files): Native/GCNBC1Shared.inc,
Native/GCNInterpreter.hpp, Native/GCNShaderRuntimeShared.inc,
Tests/GCNBC45Test.cpp, Tests/GCNTextureFootprintTest.cpp,
Tests/GCNTextureFootprintMetalTest.mm, Tests/PM4MetalRendererTest.mm
(all under MaxPS4-iOS), and this document. Workflow edits remain local.
