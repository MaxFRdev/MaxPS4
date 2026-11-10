# Call of Duty: MRTZ depth export

The device stops on `f8001881 01010100`, an EXP to target 8 with only
component X enabled. This is a pixel shader depth export, not a colour target.

Translation now carries an explicit depth result to Metal `[[depth(any)]]`.
The fragment starts with interpolated depth, and active exports replace it.
Depth-only programs return a structure without a colour output. Programs with
colour and depth return both. Coverage output is retained for programs that
can discard. Depth exports in vertex/compute programs, compressed depth, and
stencil/sample-mask components remain explicitly unsupported.

The Metal renderer keeps the fragment function for depth-only shaders with a
depth export; otherwise it would use raster depth even after successful
translation. Existing depth test and write-enable state still controls writes.

Tests cover the exact device instruction, active/inactive lanes, explicit refusal
of unsupported export components, and generated depth-only output. Real Metal
renderer tests draw a triangle with raster depth 0.6 but shader depth 0.95,
followed by a red triangle at 0.9 with LESS testing. Red must win: merely skipping
the pixel shader or ignoring the export would produce a different image. The
same test runs with a depth-only pre-pass and with colour plus depth exports.

References: [LLVM export targets](https://llvm.org/docs/doxygen/AMDGPUBaseInfo_8cpp_source.html),
[Metal shading language specification](https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf).

Actual Call of Duty gameplay remains to be checked on the iPhone.
