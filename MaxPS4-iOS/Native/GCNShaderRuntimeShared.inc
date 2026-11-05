// Runtime helpers shared verbatim by the C++ and Metal builds of translated
// GCN programs. Write only the common subset of C++ and MSL here: scalar
// types, functional casts, local arrays, no pointers, no std::, no
// preprocessor lines inside the macro. Semantics mirror GCNInterpreter.hpp.
#define GCN_BC1_SOURCE(...) GCN_SHARED_SOURCE(__VA_ARGS__)
#include "GCNBC1Shared.inc"
#undef GCN_BC1_SOURCE
GCN_SHARED_SOURCE(
GCN_FN uint gcn_f16rtz(float f) {
    uint x = gu(f); uint sign = (x >> 16) & 0x8000u; uint biased = (x >> 23) & 0xffu; uint mant = x & 0x7fffffu;
    if (biased == 0xffu) return sign | 0x7c00u | (mant != 0u ? 0x200u : 0u);
    int e = int(biased) - 112;
    if (e >= 31) return sign | 0x7bffu;
    if (e <= 0) { if (e < -10) return sign; mant |= 0x800000u; return sign | (mant >> uint(14 - e)); }
    return sign | (uint(e) << 10) | (mant >> 13);
}
GCN_FN float gcn_f16(uint h) {
    uint sign = (h & 0x8000u) << 16; uint e = (h >> 10) & 0x1fu; uint m = h & 0x3ffu;
    if (e == 0u) { float v = gcn_ldexp(float(m), -24); return sign != 0u ? -v : v; }
    if (e == 31u) return gf(sign | 0x7f800000u | (m << 13));
    return gf(sign | ((e + 112u) << 23) | (m << 13));
}
GCN_FN uint gcn_cvt_u32(float f) {
    if (gcn_isnan(f) || f <= 0.0f) return 0u;
    if (f >= 4294967295.0f) return 0xffffffffu;
    return uint(f);
}
GCN_FN int gcn_cvt_i32(float f) {
    if (gcn_isnan(f)) return 0;
    if (f <= -2147483648.0f) return int(0x80000000u);
    if (f >= 2147483647.0f) return 2147483647;
    return int(f);
}
GCN_FN uint gcn_finish(uint r, uint omod, bool clamp) {
    float f = gf(r);
    if (omod == 1u) f = f * 2.0f; else if (omod == 2u) f = f * 4.0f; else if (omod == 3u) f = f * 0.5f;
    if (clamp) f = gcn_fmin(gcn_fmax(f, 0.0f), 1.0f);
    return gu(f);
}
GCN_FN bool gcn_cmpf(uint cond, float a, float b) {
    bool lt = a < b; bool eq = a == b; bool gt = a > b; bool un = gcn_isnan(a) || gcn_isnan(b);
    if (cond == 0u) return false;      if (cond == 1u) return lt;
    if (cond == 2u) return eq;         if (cond == 3u) return a <= b;
    if (cond == 4u) return gt;         if (cond == 5u) return lt || gt;
    if (cond == 6u) return a >= b;     if (cond == 7u) return !un;
    if (cond == 8u) return un;         if (cond == 9u) return !(a >= b);
    if (cond == 10u) return !(lt || gt); if (cond == 11u) return !gt;
    if (cond == 12u) return !(a <= b); if (cond == 13u) return !eq;
    if (cond == 14u) return !lt;
    return true;
}
GCN_FN bool gcn_class(float f, uint mask) {
    uint x = gu(f); bool neg = (x >> 31) != 0u; uint e = (x >> 23) & 0xffu; uint m = x & 0x7fffffu; uint c;
    if (e == 0xffu && m != 0u) c = (x & 0x400000u) != 0u ? 1u : 0u;
    else if (e == 0xffu) c = neg ? 2u : 9u;
    else if (e == 0u && m == 0u) c = neg ? 5u : 6u;
    else if (e == 0u) c = neg ? 4u : 7u;
    else c = neg ? 3u : 8u;
    return ((mask >> c) & 1u) != 0u;
}
GCN_FN uint gcn_cube(uint op, float x, float y, float z) {
    float ax = x < 0.0f ? -x : x; float ay = y < 0.0f ? -y : y; float az = z < 0.0f ? -z : z;
    float id; float sc; float tc; float ma;
    if (az >= ax && az >= ay) { id = z < 0.0f ? 5.0f : 4.0f; sc = z < 0.0f ? -x : x; tc = -y; ma = 2.0f * z; }
    else if (ay >= ax) { id = y < 0.0f ? 3.0f : 2.0f; sc = x; tc = y < 0.0f ? -z : z; ma = 2.0f * y; }
    else { id = x < 0.0f ? 1.0f : 0.0f; sc = x < 0.0f ? z : -z; tc = -y; ma = 2.0f * x; }
    return gu(op == 324u ? id : op == 325u ? sc : op == 326u ? tc : ma);
}
GCN_FN int gcn_mini(int a, int b) { return a < b ? a : b; }
GCN_FN int gcn_maxi(int a, int b) { return a > b ? a : b; }
GCN_FN uint gcn_minu(uint a, uint b) { return a < b ? a : b; }
GCN_FN uint gcn_maxu(uint a, uint b) { return a > b ? a : b; }
GCN_FN uint gcn_sel(uint sel, uint4 c, uint one) {
    if (sel >= 4u) return c[sel - 4u];
    return sel == 1u ? one : 0u;
}
GCN_FN uint gcn_convert(uint raw, uint bits, uint nfmt) {
    uint maxv = bits == 32u ? 0xffffffffu : ((1u << bits) - 1u);
    int sraw = bits == 32u ? int(raw) : (int(raw << (32u - bits)) >> (32u - bits));
    if (nfmt == 0u) return gu(bits == 32u ? float(raw) / 4294967295.0f : float(raw) / float(maxv));
    if (nfmt == 1u || nfmt == 6u) return gu(gcn_fmax(float(sraw) / float(maxv >> 1), -1.0f));
    if (nfmt == 2u) return gu(float(raw));
    if (nfmt == 3u) return gu(float(sraw));
    if (nfmt == 5u) return uint(sraw);
    if (nfmt == 7u && bits == 16u) return gu(gcn_f16(raw));
    return raw;
}
// buffer_load_format_*: V# data/number format and DST_SEL applied.
GCN_FN uint4 gcn_buffer_format(GCNMemory M, GCNRes4 r, uint index, uint voffset, uint soffset, uint inst, bool idxen, bool offen) {
    uint4 out;
    ulong base = ulong(r.d[0]) | (ulong(r.d[1] & 0xfffu) << 32); // 44 bits, then the memory type
    uint stride = (r.d[1] >> 16) & 0x3fffu; uint records = r.d[2];
    uint dfmt = (r.d[3] >> 15) & 15u; uint nfmt = (r.d[3] >> 12) & 7u;
    uint channels = 0u; uint bits = 32u;
    if (dfmt == 1u) { channels = 1u; bits = 8u; } else if (dfmt == 2u) { channels = 1u; bits = 16u; }
    else if (dfmt == 3u) { channels = 2u; bits = 8u; } else if (dfmt == 4u) { channels = 1u; bits = 32u; }
    else if (dfmt == 5u) { channels = 2u; bits = 16u; } else if (dfmt == 10u) { channels = 4u; bits = 8u; }
    else if (dfmt == 11u) { channels = 2u; bits = 32u; } else if (dfmt == 12u) { channels = 4u; bits = 16u; }
    else if (dfmt == 13u) { channels = 3u; bits = 32u; } else if (dfmt == 14u) { channels = 4u; bits = 32u; }
    uint idx = idxen ? index : 0u; uint off = offen ? voffset : 0u;
    bool inside = stride != 0u ? idx < records : ulong(inst) + off < ulong(records);
    ulong address = base + soffset + inst + ulong(idx) * stride + off;
    uint4 comp = uint4(0u, 0u, 0u, 0u);
    for (uint c = 0u; c < 4u; ++c) {
        uint raw = 0u;
        if (inside && c < channels) {
            ulong at = address + c * (bits / 8u);
            raw = bits == 32u ? gcn_ld32(M, at) : bits == 16u ? gcn_ld16(M, at) : gcn_ld8(M, at);
            comp[c] = gcn_convert(raw, bits, nfmt);
        }
    }
    uint one = (nfmt == 4u || nfmt == 5u) ? 1u : 0x3f800000u;
    for (uint c = 0u; c < 4u; ++c) out[c] = gcn_sel((r.d[3] >> (3u * c)) & 7u, comp, one);
    return out;
}
GCN_FN uint gcn_buffer_dword(GCNMemory M, GCNRes4 r, uint index, uint voffset, uint soffset, uint inst, bool idxen, bool offen, uint n) {
    ulong base = ulong(r.d[0]) | (ulong(r.d[1] & 0xfffu) << 32); // 44 bits, then the memory type
    uint stride = (r.d[1] >> 16) & 0x3fffu; uint records = r.d[2];
    uint idx = idxen ? index : 0u; uint off = offen ? voffset : 0u;
    bool inside = stride != 0u ? idx < records : ulong(inst) + off < ulong(records);
    if (!inside) return 0u;
    return gcn_ld32(M, base + soffset + inst + ulong(idx) * stride + off + 4u * n);
}
// The inverse of gcn_convert: a register value as `bits` bits of format nfmt.
GCN_FN uint gcn_pack(uint value, uint bits, uint nfmt) {
    uint top = bits == 32u ? 0xffffffffu : (1u << bits) - 1u;
    float f = gf(value);
    if (nfmt == 0u) return bits == 32u ? value : uint(gcn_rint(gcn_fmin(gcn_fmax(f, 0.0f), 1.0f) * float(top))) & top;
    if (nfmt == 1u) {
        if (bits == 32u) return value;
        float scale = float(top >> 1);
        return uint(int(gcn_rint(gcn_fmin(gcn_fmax(f, -1.0f), 1.0f) * scale))) & top;
    }
    if (nfmt == 2u) return uint(gcn_fmax(f, 0.0f)) & top;
    if (nfmt == 3u) return uint(int(f)) & top;
    if (nfmt == 7u && bits == 16u) return gcn_f16rtz(f);
    return value & top;
}
GCN_FN void gcn_store_bits(GCNMemory M, ulong at, uint bits, uint raw) {
    if (bits == 32u) gcn_st32(M, at, raw); else if (bits == 16u) gcn_st16(M, at, raw); else gcn_st8(M, at, raw);
}
// buffer_store_format_*: `count` components (x, xy, xyz, xyzw) of `value`.
GCN_FN void gcn_buffer_store_format(GCNMemory M, GCNRes4 r, uint index, uint voffset, uint soffset, uint inst, bool idxen, bool offen,
                                    uint4 value, uint count) {
    ulong base = ulong(r.d[0]) | (ulong(r.d[1] & 0xfffu) << 32); // 44 bits, then the memory type
    uint stride = (r.d[1] >> 16) & 0x3fffu; uint records = r.d[2];
    uint dfmt = (r.d[3] >> 15) & 15u; uint nfmt = (r.d[3] >> 12) & 7u;
    uint channels = 0u; uint bits = 32u;
    if (dfmt == 1u) { channels = 1u; bits = 8u; } else if (dfmt == 2u) { channels = 1u; bits = 16u; }
    else if (dfmt == 3u) { channels = 2u; bits = 8u; } else if (dfmt == 4u) { channels = 1u; bits = 32u; }
    else if (dfmt == 5u) { channels = 2u; bits = 16u; } else if (dfmt == 10u) { channels = 4u; bits = 8u; }
    else if (dfmt == 11u) { channels = 2u; bits = 32u; } else if (dfmt == 12u) { channels = 4u; bits = 16u; }
    else if (dfmt == 13u) { channels = 3u; bits = 32u; } else if (dfmt == 14u) { channels = 4u; bits = 32u; }
    uint idx = idxen ? index : 0u; uint off = offen ? voffset : 0u;
    bool inside = stride != 0u ? idx < records : ulong(inst) + off < ulong(records);
    if (!inside) return;
    ulong address = base + soffset + inst + ulong(idx) * stride + off;
    for (uint c = 0u; c < channels && c < count; ++c)
        gcn_store_bits(M, address + c * (bits / 8u), bits, gcn_pack(value[c], bits, nfmt));
}
GCN_FN void gcn_buffer_store_dword(GCNMemory M, GCNRes4 r, uint index, uint voffset, uint soffset, uint inst, bool idxen, bool offen,
                                   uint n, uint value) {
    ulong base = ulong(r.d[0]) | (ulong(r.d[1] & 0xfffu) << 32); // 44 bits, then the memory type
    uint stride = (r.d[1] >> 16) & 0x3fffu; uint records = r.d[2];
    uint idx = idxen ? index : 0u; uint off = offen ? voffset : 0u;
    bool inside = stride != 0u ? idx < records : ulong(inst) + off < ulong(records);
    if (inside) gcn_st32(M, base + soffset + inst + ulong(idx) * stride + off + 4u * n, value);
}
GCN_FN void gcn_buffer_store_bytes(GCNMemory M, GCNRes4 r, uint index, uint voffset, uint soffset, uint inst, bool idxen, bool offen,
                                   uint value, uint bytes) {
    ulong base = ulong(r.d[0]) | (ulong(r.d[1] & 0xfffu) << 32); // 44 bits, then the memory type
    uint stride = (r.d[1] >> 16) & 0x3fffu; uint records = r.d[2];
    uint idx = idxen ? index : 0u; uint off = offen ? voffset : 0u;
    bool inside = stride != 0u ? idx < records : ulong(inst) + off < ulong(records);
    if (inside) gcn_store_bits(M, base + soffset + inst + ulong(idx) * stride + off, bytes * 8u, value);
}
GCN_FN float4 gcn_texture_swizzle(GCNRes8 t, float4 value) {
    uint4 c; for (uint k = 0u; k < 4u; ++k) c[k] = gu(value[k]);
    return float4(gf(gcn_sel(t.d[3] & 7u, c, 0x3f800000u)), gf(gcn_sel((t.d[3] >> 3) & 7u, c, 0x3f800000u)),
                  gf(gcn_sel((t.d[3] >> 6) & 7u, c, 0x3f800000u)), gf(gcn_sel((t.d[3] >> 9) & 7u, c, 0x3f800000u)));
}
// Linear 2D images (T# tiling index 8). Other tilings read as 0.
GCN_FN float4 gcn_texel_at(GCNMemory M, GCNRes8 t, int x, int y, bool resolved, ulong offset) {
    float4 native;
    if (gcn_native_texel(M, t, x, y, native)) return gcn_texture_swizzle(t, native);
    ulong base = (ulong(t.d[0]) | (ulong(t.d[1] & 0xffu) << 32)) << 8;
    uint dfmt = (t.d[1] >> 20) & 0x3fu; uint pitch = ((t.d[4] >> 13) & 0x3fffu) + 1u;
    if (dfmt >= 35u && dfmt <= 39u) {
        uint number = (t.d[1] >> 26) & 15u;
        if ((number != 0u && !(dfmt >= 38u && number == 1u)) || pitch < (t.d[2] & 0x3fffu) + 1u)
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        uint blockBytes = (dfmt == 35u || dfmt == 38u) ? 8u : 16u;
        ulong relative = (ulong(uint(y) / 4u) * ((pitch + 3u) / 4u) + uint(x) / 4u) * blockBytes;
        if (dfmt >= 38u) {
            uint pixel = (uint(y) & 3u) * 4u + (uint(x) & 3u);
            float4 colour = float4(0.0f, 0.0f, 0.0f, 1.0f);
            for (uint c = 0u; c < (dfmt == 38u ? 1u : 2u); ++c) {
                ulong at = relative + 8ul * c;
                uint low = resolved ? gcn_packed32(M, offset + at) : gcn_ld32(M, base + at);
                uint high = resolved ? gcn_packed32(M, offset + at + 4ul) : gcn_ld32(M, base + at + 4ul);
                colour[c] = number == 1u ? gcn_bc45_snorm(low, high, pixel) : gcn_bc23_alpha(low, high, pixel, true);
            }
            return gcn_texture_swizzle(t, colour);
        }
        uint alphaLow = 0u, alphaHigh = 0u;
        if (dfmt != 35u) {
            alphaLow = resolved ? gcn_packed32(M, offset + relative) : gcn_ld32(M, base + relative);
            alphaHigh = resolved ? gcn_packed32(M, offset + relative + 4ul) : gcn_ld32(M, base + relative + 4ul);
            relative += 8ul;
        }
        uint endpoints = resolved ? gcn_packed32(M, offset + relative) : gcn_ld32(M, base + relative);
        uint selectors = resolved ? gcn_packed32(M, offset + relative + 4ul) : gcn_ld32(M, base + relative + 4ul);
        uint pixel = (uint(y) & 3u) * 4u + (uint(x) & 3u);
        float4 colour;
        for (uint c = 0u; c < 4u; ++c) colour[c] = gcn_bc1_component(endpoints, selectors, pixel, c, dfmt != 35u);
        if (dfmt != 35u) colour[3] = gcn_bc23_alpha(alphaLow, alphaHigh, pixel, dfmt == 37u);
        return gcn_texture_swizzle(t, colour);
    }
    uint bytes = 4u;
    if (dfmt == 1u) bytes = 1u; else if (dfmt == 2u || dfmt == 3u || (dfmt >= 16u && dfmt <= 19u)) bytes = 2u;
    else if (dfmt == 14u) bytes = 16u;
    ulong relative = (ulong(uint(y)) * pitch + uint(x)) * bytes;
    ulong at = base + relative;
    ulong packed = offset + relative;
    uint raw = resolved ? (bytes == 1u ? gcn_packed8(M, packed) : bytes == 2u ? gcn_packed16(M, packed) : gcn_packed32(M, packed))
        : (bytes == 1u ? gcn_ld8(M, at) : bytes == 2u ? gcn_ld16(M, at) : gcn_ld32(M, at));
    float c0 = 0.0f; float c1 = 0.0f; float c2 = 0.0f; float c3 = 1.0f;
    uint nfmt = (t.d[1] >> 26) & 15u;
    if (dfmt == 1u) c0 = float(raw & 0xffu) / 255.0f;
    else if (dfmt == 2u) c0 = nfmt == 7u ? gcn_f16(raw & 0xffffu) : float(raw & 0xffffu) / 65535.0f;
    else if (dfmt == 3u) { c0 = float(raw & 0xffu) / 255.0f; c1 = float((raw >> 8) & 0xffu) / 255.0f; }
    else if (dfmt == 4u) c0 = nfmt == 7u ? gf(raw) : float(raw);
    else if (dfmt == 10u) { c0 = float(raw & 0xffu) / 255.0f; c1 = float((raw >> 8) & 0xffu) / 255.0f;
                            c2 = float((raw >> 16) & 0xffu) / 255.0f; c3 = float((raw >> 24) & 0xffu) / 255.0f; }
    else if (dfmt == 16u) { c0 = float(raw & 31u) / 31.0f; c1 = float((raw >> 5) & 63u) / 63.0f; c2 = float((raw >> 11) & 31u) / 31.0f; }
    else if (dfmt == 17u) { c0 = float(raw & 1u); c1 = float((raw >> 1) & 31u) / 31.0f; c2 = float((raw >> 6) & 31u) / 31.0f; c3 = float((raw >> 11) & 31u) / 31.0f; }
    else if (dfmt == 18u) { c0 = float(raw & 31u) / 31.0f; c1 = float((raw >> 5) & 31u) / 31.0f; c2 = float((raw >> 10) & 31u) / 31.0f; c3 = float((raw >> 15) & 1u); }
    else if (dfmt == 19u) { c0 = float(raw & 15u) / 15.0f; c1 = float((raw >> 4) & 15u) / 15.0f; c2 = float((raw >> 8) & 15u) / 15.0f; c3 = float((raw >> 12) & 15u) / 15.0f; }
    else if (dfmt == 14u) { c0 = gf(raw); c1 = gf(resolved ? gcn_packed32(M, packed + 4u) : gcn_ld32(M, at + 4u));
        c2 = gf(resolved ? gcn_packed32(M, packed + 8u) : gcn_ld32(M, at + 8u)); c3 = gf(resolved ? gcn_packed32(M, packed + 12u) : gcn_ld32(M, at + 12u)); }
    return gcn_texture_swizzle(t, float4(c0, c1, c2, c3));
}
GCN_FN float4 gcn_texel(GCNMemory M, GCNRes8 t, int x, int y) { return gcn_texel_at(M, t, x, y, false, 0ul); }
GCN_FN int gcn_wrap(int x, int size, uint mode, GCN_THREAD bool& border) {
    // In-range coordinates, the common case, skip the integer division.
    if (mode == 0u) { if (x >= 0 && x < size) return x; int m = x % size; return m < 0 ? m + size : m; }
    if (mode == 1u) {
        int period = 2 * size; int m = x;
        if (m < 0 || m >= period) { m = x % period; if (m < 0) m += period; }
        return m < size ? m : period - 1 - m;
    }
    if (mode == 2u) return x < 0 ? 0 : (x >= size ? size - 1 : x);
    if (mode == 3u) { int m = x < 0 ? -x - 1 : x; return m >= size ? size - 1 : m; }
    if (x < 0 || x >= size) border = true;
    return x < 0 ? 0 : (x >= size ? size - 1 : x);
}
GCN_FN float4 gcn_border_color(GCNRes4 s) {
    uint type = s.d[3] >> 30;
    if (type == 1u) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (type == 2u) return float4(1.0f, 1.0f, 1.0f, 1.0f);
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
GCN_FN float4 gcn_fetch_at(GCNMemory M, GCNRes8 t, GCNRes4 s, int x, int y, bool resolved, ulong offset) {
    int w = int(t.d[2] & 0x3fffu) + 1; int h = int((t.d[2] >> 14) & 0x3fffu) + 1;
    bool border = false;
    int ax = gcn_wrap(x, w, s.d[0] & 7u, border); int ay = gcn_wrap(y, h, (s.d[0] >> 3) & 7u, border);
    if (border) return gcn_border_color(s);
    return gcn_texel_at(M, t, ax, ay, resolved, offset);
}
GCN_FN float4 gcn_fetch(GCNMemory M, GCNRes8 t, GCNRes4 s, int x, int y) { return gcn_fetch_at(M, t, s, x, y, false, 0ul); }
GCN_FN float4 gcn_sample_at(GCNMemory M, GCNRes8 t, GCNRes4 s, bool unorm, float u, float v, bool resolved, ulong offset) {
    float w = float((t.d[2] & 0x3fffu) + 1u); float h = float(((t.d[2] >> 14) & 0x3fffu) + 1u);
    bool un = unorm || ((s.d[0] >> 15) & 1u) != 0u;
    bool linear = ((s.d[2] >> 20) & 3u) == 1u || ((s.d[2] >> 22) & 3u) == 1u;
    float x = un ? u : u * w; float y = un ? v : v * h;
    float fx = linear ? x - 0.5f : x; float fy = linear ? y - 0.5f : y;
    int x0 = int(gcn_floor(fx)); int y0 = int(gcn_floor(fy));
    float ax = linear ? fx - float(x0) : 0.0f; float ay = linear ? fy - float(y0) : 0.0f;
    // Each axis wrapped once rather than per tap.
    int iw = int(w); int ih = int(h); uint mx = s.d[0] & 7u; uint my = (s.d[0] >> 3) & 7u;
    bool bx0 = false; bool bx1 = false; bool by0 = false; bool by1 = false;
    int wx0 = gcn_wrap(x0, iw, mx, bx0); int wx1 = gcn_wrap(x0 + 1, iw, mx, bx1);
    int wy0 = gcn_wrap(y0, ih, my, by0); int wy1 = gcn_wrap(y0 + 1, ih, my, by1);
    // One gcn_texel_at in a loop the compiler cannot unroll: it decodes every
    // format, and each inlined copy costs compile time and registers.
    float4 r = float4(0.0f, 0.0f, 0.0f, 0.0f);
    uint taps = linear ? 4u : 1u;
    for (uint n = 0u; n < taps; ++n) {
        bool right = (n & 1u) != 0u; bool down = (n & 2u) != 0u;
        float weight = (right ? ax : 1.0f - ax) * (down ? ay : 1.0f - ay);
        float4 c = (right ? bx1 : bx0) || (down ? by1 : by0) ? gcn_border_color(s) : gcn_texel_at(M, t, right ? wx1 : wx0, down ? wy1 : wy0, resolved, offset);
        for (uint k = 0u; k < 4u; ++k) r[k] += weight * c[k];
    }
    return r;
}
GCN_FN float4 gcn_sample(GCNMemory M, GCNRes8 t, GCNRes4 s, bool unorm, float u, float v) {
    float4 filtered;
    if (gcn_native_sample(M, t, s, unorm, u, v, filtered)) return gcn_texture_swizzle(t, filtered);
    // One gcn_sample_at for every case, for the reason given there.
    bool resolved = false;
    ulong offset = 0ul;
    // A tiled T# reads only through a native texture (a colour target), else as 0.
    if (((t.d[3] >> 20) & 0x1fu) != 8u) {
        if (!gcn_native_texel(M, t, 0, 0, filtered)) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    } else {
        ulong base = (ulong(t.d[0]) | (ulong(t.d[1] & 0xffu) << 32)) << 8;
        uint width = (t.d[2] & 0x3fffu) + 1u, height = ((t.d[2] >> 14) & 0x3fffu) + 1u;
        uint pitch = ((t.d[4] >> 13) & 0x3fffu) + 1u, dfmt = (t.d[1] >> 20) & 0x3fu;
        uint bytes = dfmt == 1u ? 1u : (dfmt == 2u || dfmt == 3u || (dfmt >= 16u && dfmt <= 19u)) ? 2u : dfmt == 14u ? 16u : 4u;
        // Address modes in gcn_fetch_at constrain every texel to [0,width)x[0,height).
        // Resolve that complete footprint once, not once per tap/channel. Sparse or
        // truncated resources retain the original checked-load path and zero reads.
        ulong footprint = dfmt >= 35u && dfmt <= 39u
            ? (ulong((height + 3u) / 4u - 1u) * ((pitch + 3u) / 4u) + (width + 3u) / 4u) * ((dfmt == 35u || dfmt == 38u) ? 8ul : 16ul)
            : ((ulong(height) - 1ul) * pitch + width) * bytes;
        resolved = gcn_locate(M, base, footprint, offset);
        if (!resolved) offset = 0ul;
    }
    return gcn_sample_at(M, t, s, unorm, u, v, resolved, offset);
}
// image_store to a linear 2D image (tiling index 8): components in channel
// order, those outside `dmask` stored as 0. Other formats and tilings: none.
GCN_FN void gcn_image_store(GCNMemory M, GCNRes8 t, uint x, uint y, uint4 value, uint dmask) {
    uint w = (t.d[2] & 0x3fffu) + 1u; uint h = ((t.d[2] >> 14) & 0x3fffu) + 1u;
    if (((t.d[3] >> 20) & 0x1fu) != 8u || x >= w || y >= h) return;
    ulong base = (ulong(t.d[0]) | (ulong(t.d[1] & 0xffu) << 32)) << 8;
    uint dfmt = (t.d[1] >> 20) & 0x3fu; uint nfmt = (t.d[1] >> 26) & 15u; uint pitch = ((t.d[4] >> 13) & 0x3fffu) + 1u;
    uint channels = 0u; uint bits = 32u;
    if (dfmt == 1u) { channels = 1u; bits = 8u; } else if (dfmt == 2u) { channels = 1u; bits = 16u; }
    else if (dfmt == 3u) { channels = 2u; bits = 8u; } else if (dfmt == 4u) { channels = 1u; bits = 32u; }
    else if (dfmt == 5u) { channels = 2u; bits = 16u; } else if (dfmt == 10u) { channels = 4u; bits = 8u; }
    else if (dfmt == 11u) { channels = 2u; bits = 32u; } else if (dfmt == 12u) { channels = 4u; bits = 16u; }
    else if (dfmt == 14u) { channels = 4u; bits = 32u; }
    if (channels == 0u) return;
    ulong at = base + (ulong(y) * pitch + x) * ulong(channels * bits / 8u);
    for (uint c = 0u; c < channels; ++c)
        gcn_store_bits(M, at + c * (bits / 8u), bits, ((dmask >> c) & 1u) != 0u ? gcn_pack(value[c], bits, nfmt) : 0u);
}
// image_get_resinfo: width, height, depth (3D) or slices, and mip levels
// of T# `t` at mip level `lod`, as integers.
GCN_FN uint4 gcn_image_resinfo(GCNRes8 t, uint lod) {
    uint w = (t.d[2] & 0x3fffu) + 1u; uint h = ((t.d[2] >> 14) & 0x3fffu) + 1u; uint d = (t.d[4] & 0x1fffu) + 1u;
    uint base = (t.d[3] >> 12) & 15u; uint last = (t.d[3] >> 16) & 15u; uint type = t.d[3] >> 28;
    uint shift = lod > 31u ? 31u : lod;
    w = w >> shift; h = h >> shift;
    if (type == 10u) d = d >> shift;
    return uint4(w > 0u ? w : 1u, h > 0u ? h : 1u, d > 0u ? d : 1u, last >= base ? last - base + 1u : 1u);
}
GCN_FN float4 gcn_image_load(GCNMemory M, GCNRes8 t, uint x, uint y) {
    uint w = (t.d[2] & 0x3fffu) + 1u; uint h = ((t.d[2] >> 14) & 0x3fffu) + 1u;
    if (((t.d[3] >> 20) & 0x1fu) != 8u || x >= w || y >= h) return float4(0.0f, 0.0f, 0.0f, 0.0f);
    return gcn_texel(M, t, int(x), int(y));
}
)
