# Triangle-fan draws

`PM4DrawCollector` now accepts the AMD TriangleFan primitive (5) defined in
[shadPS4's vertex registers](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/regs_vertex.h).
Previously the game renderer rejected it with `primitive type 5`.

After resolving automatic or guest-buffer indices and applying the vertex
offset, the collector expands a fan into an ordinary triangle list:
`[a,b,c,d]` becomes `[a,b,c,a,c,d]`. This happens once in the common path,
so the software renderer, resource scanner and Metal renderer receive the
same geometry. The original primitive register remains unchanged between
submissions.

For indexed fans and strips, `VGT_MULTI_PRIM_IB_RESET_EN` (context 0x2a5)
enables restart using `VGT_MULTI_PRIM_IB_RESET_INDX` (0x103), following the
[AMD register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/asic_reg/gca/gfx_7_2_d.h).
The marker is compared with raw indices before the vertex offset, using the
current 16/32-bit width. Each marker resets fan anchors and strip winding;
incomplete groups are discarded. Restarted strips become triangle lists too.
Custom markers, leading/trailing/consecutive markers and all-marker draws are
handled. Non-indexed draws ignore restart state. For other topologies, an
actual restart marker reports `primitive restart topology`; a retained enable
bit with no marker does not reject the draw.

Fans with fewer than three vertices are skipped without reading an index
buffer. Expansion retains the existing limit of 1,048,576 output indices;
larger expanded draws report `expanded draw count` before any index-buffer
read or allocation when restart is disabled. With restart enabled, expansion
counts the triangles actually emitted, after a bounded guest-index read.
Guest memory continues through the existing checked index-fetch path.

## Verification and limits

The collector regression checks expansion order, persistent fan state, 16/32-bit
index buffers, vertex offsets, incomplete fans and expansion limits.
`PM4PrimitiveTest` renders a square through automatic and 16/32-bit indexed
offset draws, including rotated and reversed vertex order. Every pixel is
checked against a fixed result. Additive blending makes double coverage of
the shared diagonal observable. Four additional scenes split fans/strips in
both index widths into two separated shapes. Their restart marker refers to a
valid central vertex, so treating it as geometry produces visible bridges;
every pixel in the gap must remain clear.

`PM4DrawCollectorTest`, `PM4PrimitiveTest` and `PM4SoftwareRendererTest` passed
locally with Cygwin GCC, C++17 and warnings treated as errors, without sanitizers.
The PM4 workflow includes the new primitive test under ASan/UBSan.

The macOS Metal regression adds the same seven scenes and compares GPU pixels
against fixed expected bytes and the software result. All seven GPU scenes
passed with zero channel difference in
[run 40988354863](https://github.com/MaxFRdev/MaxPS4/actions/runs/40988354863),
testing source commit `58b569b`. See the
[validation report](macos-compatibility-validation.md).

This implements the existing renderer's smooth, single-instance triangle
subset. Complete provoking-vertex/flat-shading semantics,
depth/stencil and general quad interpolation are not added. Quad lists remain
explicitly unsupported; the test verifies that they are still rejected.
No game-specific Spyro playability is established by these synthetic scenes.
