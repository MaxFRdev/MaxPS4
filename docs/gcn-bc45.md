# Linear BC4 and BC5 UNORM textures

Adds AMD data formats 38 (BC4) and 39 (BC5) to the existing linear compressed
texture decoder. Format IDs follow the [AMD definitions used by shadPS4](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/pixel_format.h).
The block layout follows [Microsoft block compression documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression).

BC4 stores one red channel in an eight-byte 4x4 block. BC5 stores red then green
in two independent eight-byte blocks. Each channel uses the existing shared BC3
alpha decoder, including both endpoint orders, equal endpoints, explicit zero
and one selectors, and selectors spanning 32-bit words. Missing channels are
zero and alpha is one, before descriptor swizzling. No normal reconstruction is
performed; shaders receive the stored R/RG values.

Interpreter cache storage, resource scanning and translated C++/Metal buffer
sampling use the compressed footprint, including padded row pitch and partial
blocks. Filtering operates on decoded channels. Native-texture mode retains
buffer fallback.

Scope: level-zero linear 2D UNORM reads only. SNORM is explicitly rejected,
as are other number formats and pitch smaller than width. Tiling, sRGB, mip
chains and compressed writes are unsupported. Game compatibility is unverified.

## Local checks, 2026-10-30

Cygwin GCC 14.4, C++17, warnings treated as errors, without sanitizers:

- GCNBC45Test passed fixed expected palettes, all selectors, independent R/RG
  channels, missing-channel defaults, swizzle, cached/uncached interpreter and
  translated runtime loads, partial/padded rows and cross-block filtering.
- The whole-cache read can fail on omitted row padding while a valid individual
  block still loads. Truncated final blocks and invalid descriptors are rejected.
- GCNBC1Test, GCNBC23Test and GCNInterpreterTest regressions passed.
- GCNTextureFootprintTest passed 1,228,800 exact checked/resolved comparisons,
  including split, physically truncated and missing mappings.
- GCNTranslatorTest and generated C++ GCNTranslatorHarness passed existing
  shader generation and lane-for-lane interpreter comparisons.

Logs are in build/toolchain/test-results/*-bc45.log.

## GPU checks and CI results

GCNTextureFootprintMetalTest adds BC4/BC5 sampling at 16x8 and 256x128, independent
channel patterns, checked/resolved output comparisons and CPU/GPU comparisons.
PM4MetalRendererTest adds translated pixel-shader scenes for each format with
native-texture mode disabled/enabled and comparison to software output.
The local workflow adds GCNBC45Test under ASan/UBSan; workflow edits are not
part of the publication payload.

Published source commit `c43a57af56b6876ab0f8fbf8d1b885e088a7e21e` passed
[run 40995163063](https://github.com/MaxFRdev/MaxPS4/actions/runs/40995163063):

- Linux ASan/UBSan regressions passed, including 1,228,800 footprint comparisons
  and translated C++/interpreter equivalence.
- Actual Metal BC4/BC5 sampling passed at both sizes, with checked/resolved
  output equality and CPU/GPU channel comparisons.
- Both translated pixel-shader scenes and both native-texture fallback scenes
  had zero channel difference versus software output. No Metal tests skipped.
- iPhone ARM64 SDK compilation and Swift checks passed.

The standalone GCNBC45Test remains locally validated; the published workflow
executes the expanded existing footprint and Metal tests, rather than this new
standalone test. Logs: build/toolchain/macos-bc45-c43a57a.log and
build/toolchain/linux-bc45-c43a57a.log.

No physical iPhone execution or IPA packaging was performed. Spyro remains
unverified. This validation record was updated locally after the run.

Publication payload (seven files): Native/GCNInterpreter.hpp,
Native/GCNShaderRuntimeShared.inc, Tests/GCNBC45Test.cpp,
Tests/GCNTextureFootprintTest.cpp, Tests/GCNTextureFootprintMetalTest.mm,
Tests/PM4MetalRendererTest.mm (all under MaxPS4-iOS), and this document.
