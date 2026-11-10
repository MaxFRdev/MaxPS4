# Call of Duty: typed buffer loads

The new device stop is a vertex shader `tbuffer_load_format_xyzw`, words
`ebf33000 80030f01`. It loads four FLOAT32 components into v15:v18,
using descriptor s12:s15, index v1, byte offset v2 and scalar offset zero.

MTBUF loads X, XY, XYZ and XYZW now reuse the existing format-buffer load
path in both the translator and resource-scanning interpreter. The instruction's
DFMT/NFMT replace those descriptor fields in a temporary descriptor; destination
swizzles, base address, stride and record bounds still come from the original.
The guest descriptor is never modified. The previously supported equal-width
component layouts are accepted; packed formats, ADDR64, typed stores and
fault-status return remain refused.

Tests cover the exact device instruction, descriptor format override, combined
index and vector byte offset, four distinct values and an out-of-record index.
The generated C++ and interpreter are checked against known values. A Metal
test renders a full-screen triangle whose positions are loaded by the exact
instruction; the descriptor deliberately declares an incompatible RGBA8 format,
so ignoring the instruction's FLOAT32x4 format cannot pass the image check.

Call of Duty gameplay remains to be confirmed on the iPhone.
