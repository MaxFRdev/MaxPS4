#pragma once
// C++ build of the runtime that translated GCN programs (GCNTranslator.hpp)
// call. The Metal build is gcnMetalPrelude() in GCNTranslator.hpp; both
// expose the same names so one generated source compiles as C++ (tests,
// against the GCN interpreter) and as MSL (the device). Semantics follow
// GCNInterpreter.hpp; the differential tests hold the three together.
//
// A translated program runs one lane per call. Scalar registers are local
// per-lane copies; lane masks (EXEC, VCC, compare results) keep this lane's
// bit in bit 0. Wave-level decisions (branches on VCC/EXEC, WQM, SCC of mask
// operations) are made per 2x2 quad through gcn_quad_any, so the four lanes
// of a quad always take the same path, as derivatives require.
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#define GCN_THREAD
#define GCN_FN inline

namespace maxps4::gpu::gcnrt {
typedef std::uint32_t uint;
typedef std::uint64_t ulong;
struct float4 {
    float c[4] = {0, 0, 0, 0};
    float4() = default;
    float4(float x, float y, float z, float w) : c{x, y, z, w} {}
    float& operator[](unsigned i) { return c[i]; }
    float operator[](unsigned i) const { return c[i]; }
};
struct uint4 {
    uint c[4] = {0, 0, 0, 0};
    uint4() = default;
    uint4(uint x, uint y, uint z, uint w) : c{x, y, z, w} {}
    uint& operator[](unsigned i) { return c[i]; }
    uint operator[](unsigned i) const { return c[i]; }
};
struct GCNRes8 { uint d[8]; };
struct GCNRes4 { uint d[4]; };
// Guest memory as seen by translated code: one window or a table of ranges
// (see gcn_locate).
struct GCNMemory { const unsigned char* p; ulong base; ulong size; const ulong* ranges = nullptr; uint count = 0; };
struct GCNPlane { float p0, p10, p20; };
struct GCNLane {
    uint user[20] = {};    // user SGPRs, then (pixel) the primitive mask or (compute) group ids
    uint vinit[24] = {};   // initial VGPRs (vertex ids; pixel input layout)
    bool exec = true;      // this lane starts active (not a helper lane)
    uint quadLane = 0;     // 0-3: (x & 1) + 2 * (y & 1)
    GCNPlane plane[32][4] = {};
    float4 pos, param[32], color[8];
    uint colorWritten = 0; // MRT bits
    bool killed = false;
};
// Lockstep exchange among the four lanes of a quad, each on its own thread.
struct GCNQuad {
    std::mutex m;
    std::condition_variable cv;
    unsigned arrived = 0, generation = 0;
    uint in[4] = {}, out[4] = {};
    void exchange(unsigned lane, uint value, uint result[4]) {
        std::unique_lock<std::mutex> lock(m);
        const unsigned gen = generation;
        in[lane] = value;
        if (++arrived == 4) { std::memcpy(out, in, sizeof out); arrived = 0; ++generation; cv.notify_all(); }
        else cv.wait(lock, [&] { return generation != gen; });
        std::memcpy(result, out, sizeof out);
    }
};

GCN_FN float gf(uint x) { float f; std::memcpy(&f, &x, 4); return f; }
GCN_FN uint gu(float f) { uint x; std::memcpy(&x, &f, 4); return x; }
GCN_FN ulong gcn_u64(uint lo, uint hi) { return ulong(lo) | ulong(hi) << 32; }
GCN_FN ulong gcn_mul_i64(uint a, uint b) { return ulong(std::int64_t(std::int32_t(a)) * std::int32_t(b)); }
GCN_FN float gcn_floor(float x) { return std::floor(x); }
GCN_FN float gcn_ceil(float x) { return std::ceil(x); }
GCN_FN float gcn_trunc(float x) { return std::trunc(x); }
GCN_FN float gcn_rint(float x) { return std::nearbyint(x); }
GCN_FN float gcn_exp2(float x) { return std::exp2(x); }
GCN_FN float gcn_log2(float x) { return std::log2(x); }
GCN_FN float gcn_sqrt(float x) { return std::sqrt(x); }
GCN_FN float gcn_sin(float x) { return std::sin(x); }
GCN_FN float gcn_cos(float x) { return std::cos(x); }
GCN_FN float gcn_fmin(float a, float b) { return std::fmin(a, b); }
GCN_FN float gcn_fmax(float a, float b) { return std::fmax(a, b); }
GCN_FN float gcn_fma(float a, float b, float c) { return std::fma(a, b, c); }
GCN_FN float gcn_ldexp(float a, int e) { return std::ldexp(a, e); }
GCN_FN bool gcn_isnan(float a) { return std::isnan(a); }
GCN_FN float gcn_med3(float a, float b, float c) { return gcn_fmax(gcn_fmin(a, b), gcn_fmin(gcn_fmax(a, b), c)); }
GCN_FN uint gcn_popcount(uint v) { uint n = 0; for (; v; v &= v - 1) ++n; return n; }
GCN_FN uint gcn_bfrev(uint a) { uint r = 0; for (uint b = 0; b < 32; ++b) r |= ((a >> b) & 1u) << (31 - b); return r; }
GCN_FN uint gcn_ffbh(uint a) { if (!a) return 0xffffffffu; uint n = 0; while (!((a << n) & 0x80000000u)) ++n; return n; }
GCN_FN uint gcn_ffbl(uint a) { if (!a) return 0xffffffffu; uint n = 0; while (!((a >> n) & 1u)) ++n; return n; }
// Guest address `a`, `n` bytes long, as an offset into M.p: either the one
// window [base, base + size) at p[0], or one of `count` ranges given as
// (guest start, length, offset in p) triples.
GCN_FN bool gcn_locate(GCNMemory M, ulong a, ulong n, ulong& offset) {
    if (M.count == 0) {
        if (a < M.base || a - M.base > M.size || M.size - (a - M.base) < n) return false;
        offset = a - M.base; return true;
    }
    for (uint k = 0; k < M.count; ++k) {
        const ulong start = M.ranges[3 * k], length = M.ranges[3 * k + 1];
        if (a >= start && a - start <= length && length - (a - start) >= n) { offset = M.ranges[3 * k + 2] + (a - start); return true; }
    }
    return false;
}
GCN_FN uint gcn_ld32(GCNMemory M, ulong a) { ulong o; if (!gcn_locate(M, a, 4, o)) return 0; uint v; std::memcpy(&v, M.p + o, 4); return v; }
GCN_FN uint gcn_ld16(GCNMemory M, ulong a) { ulong o; if (!gcn_locate(M, a, 2, o)) return 0; std::uint16_t v; std::memcpy(&v, M.p + o, 2); return v; }
GCN_FN uint gcn_ld8(GCNMemory M, ulong a) { ulong o; if (!gcn_locate(M, a, 1, o)) return 0; return M.p[o]; }
// Offsets below are used only after a complete image span was validated.
GCN_FN uint gcn_packed32(GCNMemory M, ulong o) { uint v; std::memcpy(&v, M.p + o, 4); return v; }
GCN_FN uint gcn_packed16(GCNMemory M, ulong o) { std::uint16_t v; std::memcpy(&v, M.p + o, 2); return v; }
GCN_FN uint gcn_packed8(GCNMemory M, ulong o) { return M.p[o]; }
// Stores (compute programs): the memory they are given is writable.
GCN_FN void gcn_st32(GCNMemory M, ulong a, uint v) { ulong o; if (gcn_locate(M, a, 4, o)) std::memcpy(const_cast<unsigned char*>(M.p) + o, &v, 4); }
GCN_FN void gcn_st16(GCNMemory M, ulong a, uint v) { ulong o; std::uint16_t h = std::uint16_t(v); if (gcn_locate(M, a, 2, o)) std::memcpy(const_cast<unsigned char*>(M.p) + o, &h, 2); }
GCN_FN void gcn_st8(GCNMemory M, ulong a, uint v) { ulong o; if (gcn_locate(M, a, 1, o)) const_cast<unsigned char*>(M.p)[o] = (unsigned char)v; }
GCN_FN bool gcn_native_texel(GCNMemory, GCNRes8, int, int, float4&) { return false; }
#define GCN_SHARED_SOURCE(...) __VA_ARGS__
#include "GCNShaderRuntimeShared.inc"
#undef GCN_SHARED_SOURCE
GCN_FN float gcn_interp1(GCN_THREAD GCNLane& L, uint attr, uint chan, float i) {
    return L.plane[attr & 31][chan & 3].p0 + i * L.plane[attr & 31][chan & 3].p10;
}
GCN_FN float gcn_interp2(GCN_THREAD GCNLane& L, uint attr, uint chan, float j, float previous) {
    return previous + j * L.plane[attr & 31][chan & 3].p20;
}
GCN_FN float gcn_interp_mov(GCN_THREAD GCNLane& L, uint attr, uint chan, uint select) {
    const GCNPlane& p = L.plane[attr & 31][chan & 3];
    return select == 0 ? p.p10 : select == 1 ? p.p20 : p.p0;
}
GCN_FN bool gcn_quad_any(GCN_THREAD GCNQuad& Q, GCN_THREAD GCNLane& L, bool b) {
    uint all[4];
    Q.exchange(L.quadLane, b ? 1u : 0u, all);
    return all[0] | all[1] | all[2] | all[3];
}
GCN_FN uint gcn_quad_shuffle(GCN_THREAD GCNQuad& Q, GCN_THREAD GCNLane& L, uint value, uint lane) {
    uint all[4];
    Q.exchange(L.quadLane, value, all);
    return all[lane & 3];
}
} // namespace maxps4::gpu::gcnrt
