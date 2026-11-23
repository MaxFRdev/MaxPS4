# Call of Duty: image_get_lod

The device report identifies `f1800100 00e40a08` at the movie-to-scene
transition. This is GCN MIMG opcode 96 (`image_get_lod`). The decoder's gather
table omitted eight reserved slots, placing the mnemonic at the wrong index.
Those slots are restored; reserved opcodes remain unnamed.

Pixel translation now computes an isotropic 2D texture footprint from quad
coordinate differences. It returns clamped LOD in X and raw biased LOD in Y,
packs enabled components into consecutive destinations, and captures coordinates
before writes so overlapping source/destination registers work. Helper lanes
participate in quad exchange; only active lanes write destinations. Compute and
vertex stages still reject the instruction. The calculation reads descriptors,
not texture contents, and applies normalized/texel coordinates, signed sampler
bias, sampler LOD bounds and resource mip limits.

This is a 2D isotropic implementation, consistent with the current texture
sampling scope. It does not reproduce anisotropic hardware LOD heuristics or
add cube/3D sampling. Synthetic tests do not establish Call of Duty playability.

Validation covers the exact device words, decoder reserved slots, generated
C++ execution across four quad lanes, analytic LOD=3, min/max clamps, negative
bias and zero gradients. The Metal test renders the generated shader and checks
both outputs against LOD=3 at every pixel.

References:
- [LLVM GCN opcode definitions](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AMDGPU/MIMGInstructions.td)
- [AMD instruction semantics: clamped X and raw Y](https://docs.amd.com/api/khub/documents/UkT_UPQL21KfKAMUBFnZTw/content)
