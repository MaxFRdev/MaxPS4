# Indexed draws with offsets

The game renderer's shared `PM4DrawCollector` now consumes `INDEX_BASE` (0x26)
and `DRAW_INDEX_OFFSET_2` (0x35). Previously the GNM writer could generate an
offset draw but the collector ignored it, so it produced no geometry.

The collector keeps the index-buffer base across submissions, including base
changes made by `DRAW_INDEX_2`. The offset is measured in indices and uses the
current 16-bit or 32-bit index width. `VGT_INDX_OFFSET` is applied separately
to the decoded vertex IDs. The resolved draw reaches both the software and
Metal renderers through their existing indexed geometry path.

Packet layouts and base-update semantics were checked against shadPS4's
[PM4 definitions](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/pm4_cmds.h)
and [Liverpool command processor](https://github.com/shadps4-emu/shadPS4/blob/main/src/video_core/amdgpu/liverpool.cpp).

Malformed base/draw packets, unaligned or missing bases, invalid counts,
48-bit address overflow, unreadable index ranges and unsupported offset-draw
flags report a failure. No guest index buffer is read before range validation.
The existing per-draw index allocation limit remains in place.

## Verification

- `PM4DrawCollectorTest` checks both index widths, addresses above 4 GiB,
  persistent base changes, vertex offsets, collector isolation, malformed
  packets, invalid counts and memory/address boundaries.
- `PM4SoftwareRendererTest` checks all pixels of a rendered rectangle whose
  index buffer starts with a junk index that the offset must skip.
- `PM4MetalRendererTest` adds 16/32-bit offset variants of its textured scene
  and compares the resulting Metal pixels to the direct-index software result.

Local Cygwin/GCC runs passed `PM4DrawCollectorTest`, `PM4SoftwareRendererTest`
and `PM4ResourceScanTest` with C++17 and warnings treated as errors, without
sanitizers. The PM4 CI workflow includes the new
collector test under ASan/UBSan; the existing macOS Metal job exercises the new
pixel comparisons. Metal execution and iPhone compilation passed on macOS in
[run 40988354863](https://github.com/MaxFRdev/MaxPS4/actions/runs/40988354863),
testing source commit `58b569b`. Both offset-index scenes had zero channel
difference. See the [validation report](macos-compatibility-validation.md).

This extends a GPU command path; it does not establish Spyro Reignited Trilogy
compatibility. Depth/stencil, further shader and texture capabilities and the
game's actual startup requirements still need separate validation.
