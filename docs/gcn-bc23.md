# Linear BC2/DXT3 and BC3/DXT5 textures

Extends the existing linear BC1 path to AMD data formats 36 (BC2) and 37 (BC3).
Format IDs follow [shadPS4 definitions](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/pixel_format.h).
Block layout and alpha interpolation follow [Microsoft documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression).

Both formats use 16 bytes per 4x4 block. The first eight bytes contain explicit
4-bit BC2 alpha or interpolated BC3 alpha; the last eight contain opaque RGB565
colour. RGB always uses the four-colour palette, including ascending or equal
endpoints. BC3 handles both endpoint orders, equal endpoints, explicit zero/one
selectors, and selectors spanning the two 32-bit words.

The decoder is shared by the interpreter, translated C++ runtime and generated
Metal source. Cache size, resource scanning, block addressing and resolved
sampling account for padded pitch and partial edge blocks. Swizzling and
filtering use decoded RGBA. Experimental native textures retain buffer fallback.
Unsupported number formats and pitch smaller than width are rejected.

Scope: level-zero, linear 2D UNORM reads. Tiling, sRGB, mip chains and compressed
writes remain unsupported. This does not establish compatibility with a game.

## Local validation, 2026-10-30

Cygwin GCC 14.4, C++17, warnings treated as errors; no local sanitizers:

- GCNBC23Test passed independent expected RGB/alpha palettes, all selectors,
  word boundary, both BC3 alpha modes and equal endpoints, padded/partial blocks,
  cached/uncached interpreter reads, translated runtime reads, swizzling,
  cross-block bilinear filtering, rejected descriptors and truncated resources.
- GCNBC1Test and GCNInterpreterTest passed existing regressions.
- GCNTextureFootprintTest passed 1,064,960 exact checked/resolved comparisons,
  including split, physically truncated and absent memory.
- GCNTranslatorTest and generated C++ GCNTranslatorHarness passed existing
  shader translation and lane-for-lane interpreter comparisons.

Logs: build/toolchain/test-results/*-bc23.log.

## macOS/Metal and Linux CI validation

GCNTextureFootprintMetalTest adds both formats to actual GPU sampling at 16x8
and 256x128, checked/resolved output comparisons and CPU/GPU sampled colours.
PM4MetalRendererTest adds translated pixel-shader scenes for both formats,
including experimental native-texture fallback versus software-rendered pixels.
The local PM4 workflow also includes the standalone GCNBC23Test under ASan/UBSan.

Published source commit `ea1da3d83eeaaaffadb6290b87611c08d2002110` passed
[run 40992734442](https://github.com/MaxFRdev/MaxPS4/actions/runs/40992734442):

- Linux ASan/UBSan tests passed, including 1,064,960 footprint comparisons.
- Actual Metal BC2/BC3 sampling passed at both sizes, including CPU/GPU alpha.
- BC2/BC3 translated pixel-shader and native-texture fallback scenes each had
  zero channel difference versus software output. No Metal tests skipped.
- iPhone ARM64 SDK compilation and Swift checks passed.

The standalone GCNBC23Test remains locally validated: the published workflow
has not been changed to execute that new standalone test. The existing workflow
executes the expanded footprint, translation and Metal tests.
Logs: build/toolchain/macos-bc23-ea1da3d.log and
build/toolchain/linux-bc23-ea1da3d.log.

No physical iPhone execution or IPA packaging was performed. Spyro playability
remains unverified. This validation record was updated locally after the run.

Publication payload: GCNBC1Shared.inc, GCNInterpreter.hpp,
GCNShaderRuntimeShared.inc, GCNBC23Test.cpp, GCNTextureFootprintTest.cpp,
GCNTextureFootprintMetalTest.mm, PM4MetalRendererTest.mm and this document
(eight files). Workflow edits remain local.
