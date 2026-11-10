# Call of Duty: image_gather4_lz_o

The next device report refuses opcode 87, words `f15c0200 0040040c`.
This selects the green component, with a packed offset in v12, coordinates
in v13:v14 and four results in v4:v7. The previous GET_LOD refusal is no
longer the reported stop reason.

Translation and resource scanning now support 2D `image_gather4_lz` and
`image_gather4_lz_o` at the existing level-zero texture view. Four unfiltered
values are returned in lower-left, lower-right, upper-right, upper-left order.
Offset components are signed six-bit integers at bits 0..5 and 8..13.
Existing texture swizzles, address modes, border colours and native texel
views are reused. Multiple component bits are rejected. Other gather variants,
explicit nonzero mip levels and depth-comparison gathers remain unsupported.

Tests use the exact device words, known distinct texels, signed negative
offsets, normalized and forced unnormalized coordinates, and border colour.
The generated C++ and interpreter results are checked against known values.
A Metal render checks the four outputs in every pixel.

References: [AMD gather ordering](https://docs.amd.com/api/khub/documents/UkT_UPQL21KfKAMUBFnZTw/content),
[AMD packed offset fields](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/vega-7nm-shader-instruction-set-architecture.pdf).

Actual Call of Duty gameplay still requires device validation.
