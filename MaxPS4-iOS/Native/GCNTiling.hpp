#pragma once
// GCN 1D micro tiling (T# TILING_INDEX 5 Depth_1dThin, 9 Display_1dThin,
// 13 Thin_1dThin): elements (pixels, or 4x4 blocks for BC) in 8x8 tiles laid
// row by row; inside a tile, AddrLib's ComputePixelIndexWithinMicroTile order.
// Index 8 is linear. 2D (macro) tilings are not described here.
// GCNShaderRuntimeShared.inc carries the same arithmetic for shaders.
#include <cstdint>

namespace maxps4::gpu {
inline bool gcnMicroTiled(unsigned tiling) { return tiling == 5 || tiling == 9 || tiling == 13; }
inline bool gcnTilingReadable(unsigned tiling) { return tiling == 8 || gcnMicroTiled(tiling); }

// The element's index in its 8x8 tile. `bytes`: element size (Display_1dThin
// orders its bits by it).
inline unsigned gcnMicroTileIndex(unsigned tiling, unsigned x, unsigned y, unsigned bytes) {
    const unsigned x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1, y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    if (tiling == 9) switch (bytes) {
        case 1: return x0 | x1 << 1 | x2 << 2 | y1 << 3 | y0 << 4 | y2 << 5;
        case 2: return x0 | x1 << 1 | x2 << 2 | y0 << 3 | y1 << 4 | y2 << 5;
        case 4: return x0 | x1 << 1 | y0 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
        case 8: return x0 | y0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
        default: return y0 | x0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5;
    }
    return x0 | y0 << 1 | x1 << 2 | y1 << 3 | x2 << 4 | y2 << 5;
}
// Byte offset of element (x, y) in a surface `pitch` elements wide.
inline std::uint64_t gcnElementOffset(unsigned tiling, unsigned x, unsigned y, unsigned pitch, unsigned bytes) {
    if (!gcnMicroTiled(tiling)) return (std::uint64_t(y) * pitch + x) * bytes;
    const std::uint64_t tile = std::uint64_t(y >> 3) * ((pitch + 7) >> 3) + (x >> 3);
    return (tile * 64 + gcnMicroTileIndex(tiling, x, y, bytes)) * bytes;
}
// Bytes a surface of `pitch` x `height` elements spans.
inline std::uint64_t gcnSurfaceBytes(unsigned tiling, unsigned pitch, unsigned height, unsigned bytes) {
    if (!gcnMicroTiled(tiling)) return std::uint64_t(pitch) * height * bytes;
    return std::uint64_t((pitch + 7) >> 3) * ((height + 7) >> 3) * 64 * bytes;
}
} // namespace maxps4::gpu
