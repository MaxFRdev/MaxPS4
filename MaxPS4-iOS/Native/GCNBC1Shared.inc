// Intentionally unguarded: included as C++ and as Metal source text.
GCN_BC1_SOURCE(
inline float gcn_bc1_component(unsigned endpoints, unsigned selectors, unsigned pixel, unsigned channel, bool opaque = false) {
    unsigned a = endpoints & 65535u, b = endpoints >> 16;
    unsigned index = (selectors >> (2u * pixel)) & 3u;
    bool transparent = !opaque && a <= b && index == 3u;
    if (channel == 3u) return transparent ? 0.0f : 1.0f;
    if (transparent) return 0.0f;
    unsigned shift = channel == 0u ? 11u : channel == 1u ? 5u : 0u;
    unsigned mask = channel == 1u ? 63u : 31u;
    float c0 = float((a >> shift) & mask) / float(mask);
    float c1 = float((b >> shift) & mask) / float(mask);
    if (index == 0u) return c0;
    if (index == 1u) return c1;
    if (!opaque && a <= b) return (c0 + c1) * 0.5f;
    return index == 2u ? (2.0f * c0 + c1) / 3.0f : (c0 + 2.0f * c1) / 3.0f;
}
// The first two words of BC2/BC3 hold alpha, followed by opaque BC1 RGB.
inline float gcn_bc23_alpha(unsigned low, unsigned high, unsigned pixel, bool bc3) {
    if (!bc3) return float(((pixel < 8u ? low : high) >> (4u * (pixel & 7u))) & 15u) / 15.0f;
    unsigned a = low & 255u, b = (low >> 8) & 255u;
    unsigned shift = 3u * pixel;
    // Selectors occupy 48 bits after the two endpoints; pixel 5 straddles words.
    unsigned indices = (low >> 16) | (high << 16);
    unsigned index = shift < 16u ? ((indices >> shift) & 7u) : ((high >> (shift - 16u)) & 7u);
    if (index == 0u) return float(a) / 255.0f;
    if (index == 1u) return float(b) / 255.0f;
    if (a > b) return float((8u - index) * a + (index - 1u) * b) / (7.0f * 255.0f);
    if (index >= 6u) return index == 6u ? 0.0f : 1.0f;
    return float((6u - index) * a + (index - 1u) * b) / (5.0f * 255.0f);
}
// Signed BC4/BC5 endpoints use -128 as -1, but retain their encoded order.
inline float gcn_bc45_snorm(unsigned low, unsigned high, unsigned pixel) {
    int a = int(low & 255u), b = int((low >> 8) & 255u);
    if (a >= 128) a -= 256;
    if (b >= 128) b -= 256;
    bool descending = a > b;
    if (a == -128) a = -127;
    if (b == -128) b = -127;
    unsigned shift = 3u * pixel;
    unsigned combined = (low >> 16) | (high << 16);
    unsigned index = shift < 16u ? ((combined >> shift) & 7u) : ((high >> (shift - 16u)) & 7u);
    if (index == 0u) return float(a) / 127.0f;
    if (index == 1u) return float(b) / 127.0f;
    if (!descending && index >= 6u) return index == 6u ? -1.0f : 1.0f;
    int divisor = descending ? 7 : 5;
    int weight = int(index) - 1;
    return float((divisor - weight) * a + weight * b) / (float(divisor) * 127.0f);
}

)
