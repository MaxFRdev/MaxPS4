# PM4 colour blending

The software and Metal renderers now support all five basic
`CB_BLEND0_CONTROL` combine functions: Add (0), Subtract (1), Min (2), Max (3)
and ReverseSubtract (4). The function encoding follows shadPS4's
[AMD colour register definitions](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/regs_color.h).

Previously Metal rejected every non-additive function. The software renderer
silently kept the source colour for those functions. Both renderers now share
control decoding and validation, including independent RGB/alpha operations
when separate-alpha blending is enabled. Disabled blending ignores unused
control fields; otherwise unused alpha fields do not affect combined blending.

Min/Max compare the unweighted source and destination values, ignoring blend
factors. Their semantics match the basic operations documented by
[Khronos](https://docs.vulkan.org/refpages/latest/refpages/source/VkBlendOp.html).
UNORM source values are clamped before blending. Supported weighted factors
are 0–10 (including source-alpha saturation), constant colour and its inverse
(13/14), and constant alpha and its inverse (19/20). Dual-source factors and
reserved codes report `blend mode` before the software renderer writes the
draw. Colour write masks retain destination components as before.

Each draw snapshots `CB_BLEND_RED/GREEN/BLUE/ALPHA` from context offsets
0x105–0x108, following the
[AMD register definitions](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/asic_reg/gca/gfx_7_2_d.h).
Both backends clamp the constants to [0,1], treating NaN as zero. Metal applies
them as dynamic encoder state on every draw; they do not invalidate the
pipeline cache. Saturation uses `min(source alpha, 1 - destination alpha)` for
RGB and one for alpha, as described in the
[blend-factor specification](https://docs.vulkan.org/refpages/latest/refpages/source/VkBlendFactor.html).

## Verification

`PM4BlendTest` renders a known background and foreground through real GCN
programs in 24 scenarios: all five operations, source alpha, separate alpha,
ignored fields, disabled blending, write masks, source clamping, constant
colour/alpha, inverses, crossfades, saturation and constant clamping. Every
pixel is compared to fixed expected RGBA bytes, allowing one byte for floating
point rounding at quantization ties. Six invalid-state cases verify that the
background remains unchanged.

The collector test checks constant snapshots across submissions, partial
updates and CLEAR_STATE. The Metal regression also changes constants within
one batched render pass while reusing the same two pipelines.

`PM4BlendTest` and the existing `PM4SoftwareRendererTest` passed locally with
Cygwin GCC, C++17 and warnings treated as errors, without sanitizers.
The PM4 workflow includes the new test under ASan/UBSan on Linux.
`PM4MetalRendererTest` uses the same 24 scenes to compare actual GPU pixels
against fixed expectations and the software result, with its existing
two-byte tolerance. These Metal checks and iPhone SDK compilation passed in
[run 40988354863](https://github.com/MaxFRdev/MaxPS4/actions/runs/40988354863),
testing source commit `58b569b`. All 24 scenarios and the dynamic-constant
regression executed on Metal. See the
[validation report](macos-compatibility-validation.md).
No Spyro execution or playability is claimed.
