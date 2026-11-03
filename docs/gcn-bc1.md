# Linear BC1 textures

The GCN interpreter and translated C++/Metal shader runtime now decode linear
BC1/DXT1 UNORM textures (AMD data format 35). The format code follows
[shadPS4's pixel-format definitions](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/pixel_format.h);
the block layout, RGB565 endpoints, interpolated palettes and 1-bit transparent
mode follow [Microsoft's BC1 description](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression).

Each 4x4 block uses 8 bytes. Address calculation, image-cache storage, resource
scanning and resolved texture footprints use block rows, including padded pitch
and partial final blocks. Filtering and component swizzling operate on decoded
colours. The palette implementation in `GCNBC1Shared.inc` is shared by the
interpreter, translated C++ and generated Metal source. No platform BC1 texture
support is required: experimental native-texture mode retains the buffer path.

Scope is level-zero linear 2D UNORM. Tiled textures, BC1 sRGB, other BC formats,
mip chains and compressed writes are not implemented by this change. The
interpreter explicitly rejects unsupported BC1 number formats or pitch smaller
than width. The translated helper returns zero for these descriptors.

## Verification

- `GCNBC1Test` checks independently specified palettes, opaque and transparent
  modes, equal endpoints, RGB565 channel order, padded rows, partial dimensions,
  cached/uncached reads, compressed resource size and cross-block filtering.
- `GCNTextureFootprintTest` includes BC1 in the checked/resolved differential
  matrix, including split, truncated and absent memory: 901,120 comparisons.
- Existing interpreter and translator generation/execution regressions exercise
  unchanged uncompressed paths.
- The Metal footprint test adds BC1 to checked/resolved sampling, with CPU/GPU
  sample comparisons. The renderer test adds a translated textured scene and
  verifies native-texture fallback against the software image.

Local CPU tests passed with Cygwin GCC, C++17 and warnings treated as errors,
without sanitizers. [Run 40991103330](https://github.com/MaxFRdev/MaxPS4/actions/runs/40991103330)
then passed on source commit `7473e31e222e4016c7162e2a1628133b1f41eb4c`:

- Linux ASan/UBSan regressions, including 901,120 texture-footprint comparisons
  and translated-C++/interpreter equivalence.
- Actual Metal BC1 sampling at 16x8 and 256x128, with checked/resolved and
  CPU/GPU colour/transparency comparisons.
- The translated BC1 textured scene and native-texture fallback, both with
  zero channel difference from the software image.
- iPhone ARM64 SDK compilation and Swift checks.

The initial run exposed a shader-cost probe's dependency on the uncompressed
sampling expression. The final source retains that path and gives BC1 its own
footprint branch; the benchmark passed in the final run. Logs are retained in
`build/toolchain/macos-bc1-7473e31.log` and
`build/toolchain/linux-bc1-7473e31.log`.

This does not establish Spyro playability or support for its actual texture
layouts. No physical iPhone execution or IPA packaging was performed.
