// 1D micro tiling: GCNTiling.hpp (interpreter, resource scan) and the shader
// runtime's copy (GCNShaderRuntimeShared.inc) give the same offsets, each
// tile is a permutation of its 64 elements, and AddrLib's known orders hold.
#include "../Native/GCNShaderRuntime.hpp"
#include "../Native/GCNTiling.hpp"
#include <cassert>
#include <cstdio>
#include <set>

using namespace maxps4::gpu;
int main() {
    // Thin and depth: Morton order x0 y0 x1 y1 x2 y2.
    assert(gcnMicroTileIndex(13, 1, 0, 4) == 1 && gcnMicroTileIndex(13, 0, 1, 4) == 2 && gcnMicroTileIndex(13, 2, 0, 4) == 4);
    assert(gcnMicroTileIndex(13, 7, 7, 16) == 63 && gcnMicroTileIndex(5, 4, 2, 4) == 0x18);
    // Display 32 bpp: x0 x1 y0 x2 y1 y2; 8 bpp: x0 x1 x2 y1 y0 y2; 128 bpp: y0 x0 x1 x2 y1 y2.
    assert(gcnMicroTileIndex(9, 0, 1, 4) == 4 && gcnMicroTileIndex(9, 4, 0, 4) == 8);
    assert(gcnMicroTileIndex(9, 0, 1, 1) == 16 && gcnMicroTileIndex(9, 0, 2, 1) == 8);
    assert(gcnMicroTileIndex(9, 0, 1, 16) == 1 && gcnMicroTileIndex(9, 1, 0, 16) == 2);
    // Tiles in rows: the second tile of the second row of a 24-wide surface.
    assert(gcnElementOffset(13, 8, 8, 24, 4) == (3 + 1) * 64 * 4);
    // Partial tiles pad to 8: a 20 x 10 surface spans 3 x 2 tiles.
    assert(gcnSurfaceBytes(13, 20, 10, 4) == 6 * 64 * 4 && gcnSurfaceBytes(8, 20, 10, 4) == 800);
    for (unsigned tiling : {5u, 8u, 9u, 13u}) for (unsigned bytes : {1u, 2u, 4u, 8u, 16u}) {
        std::set<std::uint64_t> offsets;
        for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 24; ++x) {
            const auto offset = gcnElementOffset(tiling, x, y, 24, bytes);
            assert(offset == gcnrt::gcn_element_offset(tiling, x, y, 24, bytes));
            assert(offset % bytes == 0 && offset < gcnSurfaceBytes(tiling, 24, 16, bytes));
            offsets.insert(offset);
        }
        assert(offsets.size() == 24 * 16); // no two elements share bytes
        assert(gcnSurfaceBytes(tiling, 24, 16, bytes) == gcnrt::gcn_surface_bytes(tiling, 24, 16, bytes));
    }
    std::puts("PASS GCN 1D micro tiling: AddrLib element orders, tile rows, padding, shader and CPU agree");
}
