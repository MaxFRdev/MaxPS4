#pragma once
// CPU reference interpreter for GCN (Sea Islands / PS4 Liverpool) programs.
// One wave of up to 64 lanes runs with real EXEC/VCC masks, so branches,
// cross-lane swizzles and helper lanes behave as on hardware. Code, scalar
// and buffer data are read from guest memory through callbacks while the
// program runs (a fetch shader called with s_swappc_b64 just works).
// Textures must be linear (tile mode 8); tiled surfaces are refused with an
// error rather than read as garbage. It is a correctness reference for the
// renderer, not a fast path.
#include "GCNDecoder.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace maxps4::gpu {
struct GCNMemory {
    std::function<bool(std::uint64_t, void*, std::size_t)> read;
    std::function<bool(std::uint64_t, const void*, std::size_t)> write;
};
// Fetch the same three-word decode window with one permission-checked read.
// Preserve word-sized fallback at mapping edges or for readers that refuse a
// bulk read; no instruction cache or immutable-code assumption is introduced.
// A pixel program's scalar load: its instruction, and where its address and
// words go in the draw's table of scalar values (see GCNTranslation::sites).
struct GCNScalarSite { std::uint64_t pc; std::uint32_t count, offset; };
inline std::size_t readGCNWords(const GCNMemory& memory, std::uint64_t pc, std::uint32_t words[3]) {
    if (memory.read(pc, words, 3 * sizeof(std::uint32_t))) return 3;
    std::fill(words, words + 3, 0);
    std::size_t available = 0;
    while (available < 3 && memory.read(pc + 4 * available, &words[available], 4)) ++available;
    return available;
}
// One attribute channel of the current primitive, as the parameter cache
// holds it: value at vertex 0 and differences to vertices 1 and 2.
struct GCNAttributePlane { float p0 = 0, p10 = 0, p20 = 0; };
struct GCNExport {
    unsigned target = 0;        // 0-7 MRT, 8 MRTZ, 9 null, 12-15 position, 32-63 parameter
    bool compressed = false, done = false;
    std::uint8_t enable = 0;    // EN channel mask
    std::uint64_t mask = 0;     // EXEC when exported
    std::array<std::array<std::uint32_t, 64>, 4> value{};
};
// Decoded instructions by guest address, shared by the waves of one draw.
using GCNCodeCache = std::unordered_map<std::uint64_t, GCNInstruction>;
// Texture level-0 copies by base address, valid while guest memory does not
// change (one draw); image stores through the same wave drop their entry.
using GCNImageCache = std::unordered_map<std::uint64_t, std::vector<std::uint8_t>>;

namespace gcn_detail {
#define GCN_BC1_SOURCE(...) __VA_ARGS__
#include "GCNBC1Shared.inc"
#undef GCN_BC1_SOURCE
inline float f32(std::uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline std::uint32_t u32(float f) { std::uint32_t v; std::memcpy(&v, &f, 4); return v; }
inline std::uint16_t f16Rtz(float f) {
    const std::uint32_t x = u32(f), sign = (x >> 16) & 0x8000, biased = (x >> 23) & 0xff;
    std::uint32_t mant = x & 0x7fffff;
    if (biased == 0xff) return std::uint16_t(sign | 0x7c00 | (mant ? 0x200 : 0));
    const int exp = int(biased) - 112;
    if (exp >= 31) return std::uint16_t(sign | 0x7bff); // toward zero: largest finite
    if (exp <= 0) {
        if (exp < -10) return std::uint16_t(sign);
        mant |= 0x800000;
        return std::uint16_t(sign | (mant >> (14 - exp)));
    }
    return std::uint16_t(sign | (std::uint32_t(exp) << 10) | (mant >> 13));
}
inline float f16(std::uint32_t h) {
    const std::uint32_t sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    if (!e) { const float v = std::ldexp(float(m), -24); return sign ? -v : v; }
    if (e == 31) return f32(sign | 0x7f800000 | (m << 13));
    return f32(sign | ((e + 112) << 23) | (m << 13));
}
inline std::uint32_t cvtU32(float f) {
    if (std::isnan(f) || f <= 0) return 0;
    if (f >= 4294967295.0f) return 0xffffffffU;
    return std::uint32_t(f);
}
inline std::int32_t cvtI32(float f) {
    if (std::isnan(f)) return 0;
    if (f <= -2147483648.0f) return std::numeric_limits<std::int32_t>::min();
    if (f >= 2147483647.0f) return std::numeric_limits<std::int32_t>::max();
    return std::int32_t(f);
}
inline float med3(float a, float b, float c) { return std::fmax(std::fmin(a, b), std::fmin(std::fmax(a, b), c)); }
template <class T> T med3i(T a, T b, T c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }
inline unsigned popcount64(std::uint64_t v) { unsigned n = 0; for (; v; v &= v - 1) ++n; return n; }
inline bool compareF(unsigned cond, float a, float b) {
    switch (cond) {
    case 0: return false;            case 1: return a < b;
    case 2: return a == b;           case 3: return a <= b;
    case 4: return a > b;            case 5: return a < b || a > b;
    case 6: return a >= b;           case 7: return !std::isnan(a) && !std::isnan(b);
    case 8: return std::isnan(a) || std::isnan(b);
    case 9: return !(a >= b);        case 10: return !(a < b || a > b);
    case 11: return !(a > b);        case 12: return !(a <= b);
    case 13: return !(a == b);       case 14: return !(a < b);
    default: return true;
    }
}
template <class T> bool compareI(unsigned cond, T a, T b) {
    switch (cond) {
    case 0: return false; case 1: return a < b; case 2: return a == b; case 3: return a <= b;
    case 4: return a > b; case 5: return a != b; case 6: return a >= b; default: return true;
    }
}
// v_cmp_class_f32: bit 0 sNaN, 1 qNaN, 2 -inf, 3 -normal, 4 -denormal, 5 -0,
// 6 +0, 7 +denormal, 8 +normal, 9 +inf.
inline bool floatClass(float f, std::uint32_t mask) {
    const std::uint32_t x = u32(f);
    const bool neg = x >> 31;
    unsigned c;
    if (std::isnan(f)) c = (x & 0x400000) ? 1 : 0;
    else if (std::isinf(f)) c = neg ? 2 : 9;
    else if (f == 0) c = neg ? 5 : 6;
    else if (std::fpclassify(f) == FP_SUBNORMAL) c = neg ? 4 : 7;
    else c = neg ? 3 : 8;
    return (mask >> c) & 1;
}
} // namespace gcn_detail

class GCNWave {
public:
    static constexpr unsigned maxLanes = 64;
    static constexpr unsigned vcc = 106, m0 = 124, execLo = 126;
    GCNWave(const GCNMemory& memory, unsigned lanes, GCNCodeCache* cache = nullptr)
        : memory_(memory), lanes_(lanes > maxLanes ? maxLanes : lanes), cache_(cache), v_(256 * lanes_, 0) {
        setExec(lanes_ == 64 ? ~0ull : ((1ull << lanes_) - 1));
    }
    std::array<std::uint32_t, 128> s{}; // SGPRs, VCC at 106, M0 at 124, EXEC at 126
    bool scc = false;
    std::vector<GCNExport> exports;
    std::string error;
    // Pixel shaders: the parameter plane of attribute `attr`, channel `chan`.
    std::function<GCNAttributePlane(unsigned attr, unsigned chan)> attribute;
    GCNImageCache* images = nullptr;
    // Called with the whole extent of every memory resource an instruction
    // addresses (scalar loads, V#, T#), whatever EXEC: a draw's GPU copy of
    // guest memory is built from these (PM4ResourceScan.hpp).
    std::function<void(std::uint64_t base, std::uint64_t bytes)> resource;
    std::function<void(const std::array<std::uint32_t, 8>&)> imageResource;
    // Each scalar load: its instruction, address and the words it read.
    std::function<void(std::uint64_t pc, std::uint64_t address, const std::uint32_t* words, unsigned count)> scalarLoad;
    // During imageResource: the S# of a sampling instruction, zeros otherwise.
    std::array<std::uint32_t, 4> imageSampler{};
    // Resource scans only: a tiled image reads as zeros instead of ending the run.
    bool tiledReadsZero = false;
    // Called as `resource` for the resources a store instruction writes.
    std::function<void(std::uint64_t base, std::uint64_t bytes)> stored;
    // Called with every V# access: its extent, stride and the address the
    // first active lane reads or writes (base when no lane is active).
    std::function<void(std::uint64_t base, std::uint64_t bytes, std::uint32_t stride, std::uint64_t address)> buffer;

    std::uint32_t& v(unsigned reg, unsigned lane) { return v_[std::size_t(reg & 0xff) * lanes_ + lane]; }
    std::uint64_t exec() const { return std::uint64_t(s[126]) | std::uint64_t(s[127]) << 32; }
    void setExec(std::uint64_t mask) { s[126] = std::uint32_t(mask); s[127] = std::uint32_t(mask >> 32); }
    std::uint64_t vccMask() const { return std::uint64_t(s[106]) | std::uint64_t(s[107]) << 32; }
    unsigned lanes() const { return lanes_; }

    // Reuse allocated lane/export storage, but never reuse runtime values or
    // callbacks. The owner decides the lifetime of the external code cache.
    void reset() {
        s.fill(0); scc = false; exports.clear(); error.clear();
        std::fill(v_.begin(), v_.end(), 0);
        attribute = {}; resource = {}; imageResource = {}; stored = {}; buffer = {}; images = nullptr; tiledReadsZero = false; scalarLoad = {};
        setExec(lanes_ == 64 ? ~0ull : ((1ull << lanes_) - 1));
    }

    // Runs from `pc` until s_endpgm. False with `error` set on anything not
    // implemented or invalid (unmapped code or data, unsupported formats).
    bool run(std::uint64_t pc, std::size_t maxInstructions = 1u << 20) {
        for (std::size_t steps = 0; steps < maxInstructions; ++steps) {
            GCNInstruction i;
            if (!fetch(pc, i)) return false;
            std::uint64_t next = pc + 4ull * i.size;
            bool ended = false;
            if (!execute(i, pc, next, ended)) {
                if (error.empty()) error = "unsupported " + gcnMnemonic(i);
                char at[32]; std::snprintf(at, sizeof at, " at 0x%llx", (unsigned long long)pc);
                error += at;
                return false;
            }
            if (ended) return true;
            pc = next;
        }
        error = "instruction limit reached";
        return false;
    }

private:
    const GCNMemory& memory_;
    unsigned lanes_;
    GCNCodeCache* cache_;
    std::vector<std::uint32_t> v_;

    bool fail(const std::string& message) { error = message; return false; }
    bool active(unsigned lane) const { return lane < lanes_ && ((exec() >> lane) & 1); }
    bool fetch(std::uint64_t pc, GCNInstruction& i) {
        if (cache_) { auto found = cache_->find(pc); if (found != cache_->end()) { i = found->second; return true; } }
        std::uint32_t words[3] = {};
        const auto available = readGCNWords(memory_, pc, words);
        if (!available) return fail("unreadable code");
        if (decodeGCNInstruction(words, available, std::size_t(pc / 4), i) != GCNError::none) {
            char text[48]; std::snprintf(text, sizeof text, "undecodable 0x%08x at 0x%llx", words[0], (unsigned long long)pc);
            return fail(text);
        }
        if (cache_) (*cache_)[pc] = i;
        return true;
    }

    // ---- operands ----
    std::uint32_t scalar(unsigned code, const GCNInstruction& i) const {
        if (code <= 127) return s[code];
        if (code == 128) return 0;
        if (code <= 192) return code - 128;
        if (code <= 208) return std::uint32_t(192 - int(code));
        switch (code) {
        case 240: return 0x3f000000; case 241: return 0xbf000000; case 242: return 0x3f800000;
        case 243: return 0xbf800000; case 244: return 0x40000000; case 245: return 0xc0000000;
        case 246: return 0x40800000; case 247: return 0xc0800000;
        case 251: return vccMask() == 0; case 252: return exec() == 0; case 253: return scc;
        case 255: return i.literal;
        default: return 0;
        }
    }
    std::uint64_t scalar64(unsigned code, const GCNInstruction& i) const {
        if (code <= 126) return std::uint64_t(s[code]) | std::uint64_t(s[code + 1]) << 32;
        if (code >= 193 && code <= 208) return std::uint64_t(std::int64_t(192 - int(code)));
        return scalar(code, i);
    }
    void setScalar(unsigned code, std::uint32_t value) { if (code <= 127) s[code] = value; }
    void setScalar64(unsigned code, std::uint64_t value) {
        if (code <= 126) { s[code] = std::uint32_t(value); s[code + 1] = std::uint32_t(value >> 32); }
    }
    std::uint32_t source(unsigned code, unsigned lane, const GCNInstruction& i) {
        return code >= 256 ? v(code - 256, lane) : scalar(code, i);
    }

    bool execute(const GCNInstruction& i, std::uint64_t pc, std::uint64_t& next, bool& ended) {
        switch (i.encoding) {
        case GCNEncoding::sop2: return sop2(i);
        case GCNEncoding::sopk: return sopk(i);
        case GCNEncoding::sop1: return sop1(i, pc, next);
        case GCNEncoding::sopc: return sopc(i);
        case GCNEncoding::sopp: return sopp(i, pc, next, ended);
        case GCNEncoding::smrd: return smrd(i);
        case GCNEncoding::vop2: case GCNEncoding::vop1: case GCNEncoding::vopc: case GCNEncoding::vop3: return vector(i);
        case GCNEncoding::vintrp: return vintrp(i);
        case GCNEncoding::ds: return ds(i);
        case GCNEncoding::mubuf: return mubuf(i);
        case GCNEncoding::mimg: return mimg(i);
        case GCNEncoding::exp: return exportOp(i);
        default: return false;
        }
    }

    // ---- scalar ALU ----
    bool sop2(const GCNInstruction& i) {
        const unsigned d = (i.word0 >> 16) & 0x7f, a = i.word0 & 0xff, b = (i.word0 >> 8) & 0xff;
        const std::uint32_t x = scalar(a, i), y = scalar(b, i);
        const std::uint64_t X = scalar64(a, i), Y = scalar64(b, i);
        auto r32 = [&](std::uint32_t r) { setScalar(d, r); scc = r != 0; return true; };
        auto r64 = [&](std::uint64_t r) { setScalar64(d, r); scc = r != 0; return true; };
        switch (i.opcode) {
        case 0: { const std::uint64_t r = std::uint64_t(x) + y; setScalar(d, std::uint32_t(r)); scc = r >> 32; return true; }
        case 1: setScalar(d, x - y); scc = y > x; return true;
        case 2: { const std::int64_t r = std::int64_t(std::int32_t(x)) + std::int32_t(y); setScalar(d, std::uint32_t(r)); scc = r != std::int32_t(r); return true; }
        case 3: { const std::int64_t r = std::int64_t(std::int32_t(x)) - std::int32_t(y); setScalar(d, std::uint32_t(r)); scc = r != std::int32_t(r); return true; }
        case 4: { const std::uint64_t r = std::uint64_t(x) + y + scc; setScalar(d, std::uint32_t(r)); scc = r >> 32; return true; }
        case 5: { const std::uint64_t sub = std::uint64_t(y) + scc; setScalar(d, std::uint32_t(x - sub)); scc = sub > x; return true; }
        case 6: scc = std::int32_t(x) < std::int32_t(y); setScalar(d, scc ? x : y); return true;
        case 7: scc = x < y; setScalar(d, scc ? x : y); return true;
        case 8: scc = std::int32_t(x) > std::int32_t(y); setScalar(d, scc ? x : y); return true;
        case 9: scc = x > y; setScalar(d, scc ? x : y); return true;
        case 10: setScalar(d, scc ? x : y); return true;
        case 11: setScalar64(d, scc ? X : Y); return true;
        case 14: return r32(x & y);   case 15: return r64(X & Y);
        case 16: return r32(x | y);   case 17: return r64(X | Y);
        case 18: return r32(x ^ y);   case 19: return r64(X ^ Y);
        case 20: return r32(x & ~y);  case 21: return r64(X & ~Y);
        case 22: return r32(x | ~y);  case 23: return r64(X | ~Y);
        case 24: return r32(~(x & y)); case 25: return r64(~(X & Y));
        case 26: return r32(~(x | y)); case 27: return r64(~(X | Y));
        case 28: return r32(~(x ^ y)); case 29: return r64(~(X ^ Y));
        case 30: return r32(x << (y & 31)); case 31: return r64(X << (y & 63));
        case 32: return r32(x >> (y & 31)); case 33: return r64(X >> (y & 63));
        case 34: return r32(std::uint32_t(std::int32_t(x) >> (y & 31)));
        case 35: return r64(std::uint64_t(std::int64_t(X) >> (y & 63)));
        case 36: setScalar(d, ((1u << (x & 31)) - 1) << (y & 31)); return true;
        case 38: setScalar(d, x * y); return true;
        case 39: { const unsigned off = y & 31, width = (y >> 16) & 0x7f;
                   const std::uint32_t r = width ? (x >> off) & (width >= 32 ? ~0u : ((1u << width) - 1)) : 0;
                   return r32(r); }
        case 40: { const unsigned off = y & 31, width = (y >> 16) & 0x7f;
                   if (!width) return r32(0);
                   if (width >= 32) return r32(std::uint32_t(std::int32_t(x) >> off));
                   const std::uint32_t field = (x >> off) & ((1u << width) - 1);
                   return r32(std::uint32_t(std::int32_t(field << (32 - width)) >> (32 - width))); }
        default: return false;
        }
    }
    bool sopk(const GCNInstruction& i) {
        const unsigned d = (i.word0 >> 16) & 0x7f;
        const std::uint32_t imm = i.word0 & 0xffff, simm = std::uint32_t(std::int32_t(std::int16_t(imm)));
        const std::uint32_t x = s[d];
        switch (i.opcode) {
        case 0: setScalar(d, simm); return true;
        case 2: if (scc) setScalar(d, simm); return true;
        case 3: case 4: case 5: case 6: case 7: case 8: {
            static const unsigned conds[] = {2, 5, 4, 6, 1, 3};
            scc = gcn_detail::compareI(conds[i.opcode - 3], std::int32_t(x), std::int32_t(simm)); return true; }
        case 9: case 10: case 11: case 12: case 13: case 14: {
            static const unsigned conds[] = {2, 5, 4, 6, 1, 3};
            scc = gcn_detail::compareI(conds[i.opcode - 9], x, imm); return true; }
        case 15: { const std::int64_t r = std::int64_t(std::int32_t(x)) + std::int32_t(simm); setScalar(d, std::uint32_t(r)); scc = r != std::int32_t(r); return true; }
        case 16: setScalar(d, x * simm); return true;
        case 18: setScalar(d, 0); return true; // s_getreg_b32: no hardware state is modelled
        case 19: case 21: return true;          // s_setreg: ignored (rounding/denormal modes)
        default: return false;
        }
    }
    bool sop1(const GCNInstruction& i, std::uint64_t pc, std::uint64_t& next) {
        const unsigned d = (i.word0 >> 16) & 0x7f, a = i.word0 & 0xff;
        const std::uint32_t x = scalar(a, i);
        const std::uint64_t X = scalar64(a, i);
        auto saveexec = [&](std::uint64_t value) {
            setScalar64(d, exec()); setExec(value); scc = value != 0; return true;
        };
        switch (i.opcode) {
        case 3: setScalar(d, x); return true;
        case 4: setScalar64(d, X); return true;
        case 5: if (scc) setScalar(d, x); return true;
        case 6: if (scc) setScalar64(d, X); return true;
        case 7: setScalar(d, ~x); scc = ~x != 0; return true;
        case 8: setScalar64(d, ~X); scc = ~X != 0; return true;
        case 10: { // s_wqm_b64: a quad is on if any of its lanes is
            std::uint64_t r = 0;
            for (unsigned q = 0; q < 64; q += 4) if ((X >> q) & 15) r |= 15ull << q;
            setScalar64(d, r); scc = r != 0; return true; }
        case 11: { std::uint32_t r = 0; for (unsigned b = 0; b < 32; ++b) r |= ((x >> b) & 1) << (31 - b); setScalar(d, r); return true; }
        case 15: { const unsigned n = gcn_detail::popcount64(x); setScalar(d, n); scc = n != 0; return true; }
        case 16: { const unsigned n = gcn_detail::popcount64(X); setScalar(d, n); scc = n != 0; return true; }
        case 19: { unsigned n = 0; while (n < 32 && !((x >> n) & 1)) ++n; setScalar(d, n == 32 ? ~0u : n); return true; }
        case 20: { unsigned n = 0; while (n < 64 && !((X >> n) & 1)) ++n; setScalar(d, n == 64 ? ~0u : n); return true; }
        case 21: { if (!x) { setScalar(d, ~0u); return true; } unsigned n = 0; while (!((x << n) & 0x80000000u)) ++n; setScalar(d, n); return true; }
        case 25: setScalar(d, std::uint32_t(std::int32_t(std::int8_t(x)))); return true;
        case 26: setScalar(d, std::uint32_t(std::int32_t(std::int16_t(x)))); return true;
        case 27: setScalar(d, s[d] & ~(1u << (x & 31))); return true;
        case 29: setScalar(d, s[d] | (1u << (x & 31))); return true;
        case 31: setScalar64(d, pc + 4); return true;
        case 32: next = X; return true;
        case 33: setScalar64(d, next); next = X; return true;
        case 36: return saveexec(X & exec());
        case 37: return saveexec(X | exec());
        case 38: return saveexec(X ^ exec());
        case 39: return saveexec(X & ~exec());
        case 40: return saveexec(X | ~exec());
        case 41: return saveexec(~(X & exec()));
        case 42: return saveexec(~(X | exec()));
        case 43: return saveexec(~(X ^ exec()));
        case 52: { const std::int32_t r = std::int32_t(x) < 0 ? -std::int32_t(x) : std::int32_t(x); setScalar(d, std::uint32_t(r)); scc = r != 0; return true; }
        default: return false;
        }
    }
    bool sopc(const GCNInstruction& i) {
        const std::uint32_t x = scalar(i.word0 & 0xff, i), y = scalar((i.word0 >> 8) & 0xff, i);
        static const unsigned conds[] = {2, 5, 4, 6, 1, 3};
        if (i.opcode < 6) scc = gcn_detail::compareI(conds[i.opcode], std::int32_t(x), std::int32_t(y));
        else if (i.opcode < 12) scc = gcn_detail::compareI(conds[i.opcode - 6], x, y);
        else if (i.opcode == 12) scc = !((x >> (y & 31)) & 1);
        else if (i.opcode == 13) scc = (x >> (y & 31)) & 1;
        else if (i.opcode == 14) scc = !((scalar64(i.word0 & 0xff, i) >> (y & 63)) & 1);
        else if (i.opcode == 15) scc = (scalar64(i.word0 & 0xff, i) >> (y & 63)) & 1;
        else return false;
        return true;
    }
    bool sopp(const GCNInstruction& i, std::uint64_t pc, std::uint64_t& next, bool& ended) {
        const std::uint64_t target = pc + 4 + 4 * std::int64_t(std::int16_t(i.word0 & 0xffff));
        switch (i.opcode) {
        case 0: case 10: case 12: case 13: case 14: case 15: case 16: case 17: case 19: case 20: case 21: case 22:
            return true; // nop, barrier, waitcnt, halt, sleep, priority, messages, cache/perf hints
        case 1: ended = true; return true;
        case 2: next = target; return true;
        case 4: if (!scc) next = target; return true;
        case 5: if (scc) next = target; return true;
        case 6: if (vccMask() == 0) next = target; return true;
        case 7: if (vccMask() != 0) next = target; return true;
        case 8: if (exec() == 0) next = target; return true;
        case 9: if (exec() != 0) next = target; return true;
        default: return false;
        }
    }
    bool smrd(const GCNInstruction& i) {
        const unsigned d = (i.word0 >> 15) & 0x7f, base = ((i.word0 >> 9) & 0x3f) * 2;
        static const unsigned counts[] = {1, 2, 4, 8, 16};
        if (i.opcode == 30) { setScalar64(d, 0); return true; }       // s_memtime
        if (i.opcode == 29 || i.opcode == 31) return true;            // cache invalidation
        if ((i.opcode & 7) > 4 || (i.opcode > 12)) return false;
        const unsigned count = counts[i.opcode & 7];
        std::uint64_t offset;
        if (i.word0 & 0x100) offset = 4ull * (i.word0 & 0xff);
        else if (i.hasLiteral) offset = 4ull * i.literal;
        else offset = scalar(i.word0 & 0xff, i);
        std::uint64_t address;
        std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
        if (i.opcode >= 8) { // s_buffer_load: V# base; NUM_RECORDS counts STRIDE-byte records (bytes if 0)
            address = (std::uint64_t(s[base]) | std::uint64_t(s[base + 1] & 0xfff) << 32) + offset;
            const std::uint32_t stride = (s[base + 1] >> 16) & 0x3fff;
            limit = stride ? std::uint64_t(s[base + 2]) * stride : s[base + 2];
        } else {
            address = ((std::uint64_t(s[base]) | std::uint64_t(s[base + 1]) << 32) & 0xffffffffffffull) + offset;
        }
        if (resource) {
            if (i.opcode >= 8) resource(address - offset, limit);
            else resource(address, 4ull * count);
        }
        std::uint32_t words[16];
        for (unsigned n = 0; n < count; ++n) {
            std::uint32_t value = 0;
            if (offset + 4 * n + 4 <= limit && !memory_.read(address + 4 * n, &value, 4)) return fail("unreadable scalar load");
            setScalar(d + n, value);
            words[n] = value;
        }
        if (scalarLoad) scalarLoad(4ull * i.word, address, words, count);
        return true;
    }

    // ---- vector ALU ----
    // VOPC, VOP2 and VOP1 are normalized to their VOP3 opcode numbers.
    bool vector(const GCNInstruction& i) {
        unsigned op, dst, src[3] = {0, 0, 0};
        unsigned abs = 0, neg = 0, omod = 0, sdst = vcc;
        bool clamp = false, vop3 = false;
        const std::uint32_t w = i.word0;
        switch (i.encoding) {
        case GCNEncoding::vop2: op = 256 + i.opcode; dst = (w >> 17) & 0xff; src[0] = w & 0x1ff; src[1] = 256 + ((w >> 9) & 0xff); break;
        case GCNEncoding::vop1: op = 384 + i.opcode; dst = (w >> 17) & 0xff; src[0] = w & 0x1ff; break;
        case GCNEncoding::vopc: op = i.opcode; dst = 0; src[0] = w & 0x1ff; src[1] = 256 + ((w >> 9) & 0xff); break;
        default:
            vop3 = true; op = i.opcode; dst = w & 0xff;
            src[0] = i.word1 & 0x1ff; src[1] = (i.word1 >> 9) & 0x1ff; src[2] = (i.word1 >> 18) & 0x1ff;
            omod = (i.word1 >> 27) & 3; neg = i.word1 >> 29;
            if ((op >= 293 && op <= 298) || op == 365 || op == 366 || op == 374 || op == 375) sdst = (w >> 8) & 0x7f;
            else { abs = (w >> 8) & 7; clamp = (w >> 11) & 1; }
            if (op < 256) sdst = dst;
            break;
        }
        const std::string name = gcnMnemonic(i);
        if (name.empty()) return false;
        const bool floatIn = gcn_detail::endsWith(name, "_f32");
        const bool floatOut = (floatIn && name.compare(0, 5, "v_cmp") != 0 && name != "v_cvt_u32_f32" &&
                               name != "v_cvt_i32_f32" && name != "v_cvt_pkrtz_f16_f32" && name != "v_cvt_flr_i32_f32" &&
                               name != "v_cvt_rpi_i32_f32" && name != "v_frexp_exp_i32_f32" && name != "v_cvt_f16_f32") ||
                              name.compare(0, 10, "v_cvt_f32_") == 0;
        auto operand = [&](unsigned n, unsigned lane) {
            std::uint32_t x = source(src[n], lane, i);
            if (floatIn) { if ((abs >> n) & 1) x &= 0x7fffffffu; if ((neg >> n) & 1) x ^= 0x80000000u; }
            return x;
        };
        auto finish = [&](std::uint32_t x) {
            if (!floatOut || (!clamp && !omod)) return x;
            float f = gcn_detail::f32(x);
            if (omod == 1) f *= 2; else if (omod == 2) f *= 4; else if (omod == 3) f *= 0.5f;
            if (clamp) f = std::fmin(std::fmax(f, 0.0f), 1.0f);
            return gcn_detail::u32(f);
        };
        using gcn_detail::f32; using gcn_detail::u32;
        // Compares (VOPC 0-255) write a lane mask; v_cmpx also writes EXEC.
        if (op < 256) {
            const unsigned family = op >> 4, cond = op & 15;
            std::uint64_t result = 0;
            for (unsigned lane = 0; lane < lanes_; ++lane) {
                if (!active(lane)) continue;
                const std::uint32_t a = operand(0, lane), b = operand(1, lane);
                bool r;
                if (family == 0 || family == 1 || family == 4 || family == 5) r = gcn_detail::compareF(cond, f32(a), f32(b));
                else if ((family == 8 || family == 9) && cond == 8) r = gcn_detail::floatClass(f32(a), b);
                else if ((family == 8 || family == 9) && cond < 8) r = gcn_detail::compareI(cond, std::int32_t(a), std::int32_t(b));
                else if ((family == 12 || family == 13) && cond < 8) r = gcn_detail::compareI(cond, a, b);
                else return false;
                if (r) result |= 1ull << lane;
            }
            setScalar64(sdst, result);
            if (family & 1) setExec(result);
            return true;
        }
        if (op == 384 + 2) { // v_readfirstlane_b32
            for (unsigned lane = 0; lane < lanes_; ++lane) if (active(lane)) { setScalar(dst, source(src[0], lane, i)); return true; }
            setScalar(dst, source(src[0], 0, i)); return true;
        }
        if (op == 384 + 0 || op == 384 + 65) return true; // v_nop, v_clrexcp
        std::uint64_t carry = 0;
        const std::uint64_t carryIn = (op >= 296 && op <= 298) ? (vop3 ? scalar64(src[2], i) : vccMask()) : 0;
        const std::uint64_t select = op == 256 ? (vop3 ? scalar64(src[2], i) : vccMask()) : 0;
        std::array<std::uint32_t, 64> results{};
        std::array<std::uint32_t, 64> high{};
        const bool wide = op == 374 || op == 375;
        for (unsigned lane = 0; lane < lanes_; ++lane) {
            if (!active(lane)) continue;
            const std::uint32_t a = operand(0, lane), b = (op >= 384 && op < 512) ? 0 : operand(1, lane);
            const std::uint32_t c = vop3 && op >= 320 && op < 384 ? operand(2, lane) : 0;
            const float fa = f32(a), fb = f32(b), fc = f32(c);
            const std::uint32_t cin = (carryIn >> lane) & 1;
            std::uint32_t r = 0;
            switch (op) {
            case 256: r = (select >> lane) & 1 ? b : a; break;
            case 259: r = u32(fa + fb); break;
            case 260: r = u32(fa - fb); break;
            case 261: r = u32(fb - fa); break;
            case 263: r = (fa == 0 || fb == 0) ? 0 : u32(fa * fb); break;
            case 264: r = u32(fa * fb); break;
            case 265: r = std::uint32_t(std::int64_t(std::int32_t(a << 8) / 256) * (std::int32_t(b << 8) / 256)); break;
            case 267: r = (a & 0xffffff) * (b & 0xffffff); break;
            case 268: r = std::uint32_t((std::uint64_t(a & 0xffffff) * (b & 0xffffff)) >> 32); break;
            case 271: r = u32(std::fmin(fa, fb)); break;
            case 272: r = u32(std::fmax(fa, fb)); break;
            case 273: r = std::uint32_t(std::min(std::int32_t(a), std::int32_t(b))); break;
            case 274: r = std::uint32_t(std::max(std::int32_t(a), std::int32_t(b))); break;
            case 275: r = std::min(a, b); break;
            case 276: r = std::max(a, b); break;
            case 277: r = a >> (b & 31); break;
            case 278: r = b >> (a & 31); break;
            case 279: r = std::uint32_t(std::int32_t(a) >> (b & 31)); break;
            case 280: r = std::uint32_t(std::int32_t(b) >> (a & 31)); break;
            case 281: r = a << (b & 31); break;
            case 282: r = b << (a & 31); break;
            case 283: r = a & b; break;
            case 284: r = a | b; break;
            case 285: r = a ^ b; break;
            case 286: r = ((1u << (a & 31)) - 1) << (b & 31); break;
            case 287: r = u32(fa * fb + f32(v(dst, lane))); break;
            case 288: r = u32(fa * f32(i.literal) + fb); break;
            case 289: r = u32(fa * fb + f32(i.literal)); break;
            case 290: r = gcn_detail::popcount64(a) + b; break;
            case 291: r = gcn_detail::popcount64(lane < 32 ? a & ((1u << lane) - 1) : a) + b; break;
            case 292: r = (lane < 32 ? 0 : gcn_detail::popcount64(a & ((1u << (lane - 32)) - 1))) + b; break;
            case 293: { const std::uint64_t x = std::uint64_t(a) + b; r = std::uint32_t(x); if (x >> 32) carry |= 1ull << lane; break; }
            case 294: r = a - b; if (b > a) carry |= 1ull << lane; break;
            case 295: r = b - a; if (a > b) carry |= 1ull << lane; break;
            case 296: { const std::uint64_t x = std::uint64_t(a) + b + cin; r = std::uint32_t(x); if (x >> 32) carry |= 1ull << lane; break; }
            case 297: { const std::uint64_t sub = std::uint64_t(b) + cin; r = std::uint32_t(a - sub); if (sub > a) carry |= 1ull << lane; break; }
            case 298: { const std::uint64_t sub = std::uint64_t(a) + cin; r = std::uint32_t(b - sub); if (sub > b) carry |= 1ull << lane; break; }
            case 299: r = u32(std::ldexp(fa, std::int32_t(b))); break;
            case 303: r = gcn_detail::f16Rtz(fa) | std::uint32_t(gcn_detail::f16Rtz(fb)) << 16; break;
            case 320: r = (fa == 0 || fb == 0) ? u32(fc) : u32(fa * fb + fc); break;
            case 321: r = u32(fa * fb + fc); break;
            case 322: r = std::uint32_t(std::int64_t(std::int32_t(a << 8) / 256) * (std::int32_t(b << 8) / 256) + std::int32_t(c)); break;
            case 323: r = (a & 0xffffff) * (b & 0xffffff) + c; break;
            case 324: case 325: case 326: case 327: r = cube(op, fa, fb, fc); break;
            case 328: r = (c & 31) ? (a >> (b & 31)) & ((1u << (c & 31)) - 1) : 0; break;
            case 329: { const unsigned width = c & 31; if (!width) { r = 0; break; }
                        const std::uint32_t field = (a >> (b & 31)) & ((1u << width) - 1);
                        r = std::uint32_t(std::int32_t(field << (32 - width)) >> (32 - width)); break; }
            case 330: r = (a & b) | (~a & c); break;
            case 331: r = u32(std::fma(fa, fb, fc)); break;
            case 334: r = std::uint32_t(((std::uint64_t(a) << 32) | b) >> (c & 31)); break;
            case 335: r = std::uint32_t(((std::uint64_t(a) << 32) | b) >> (8 * (c & 3))); break;
            case 337: r = u32(std::fmin(std::fmin(fa, fb), fc)); break;
            case 338: r = std::uint32_t(std::min({std::int32_t(a), std::int32_t(b), std::int32_t(c)})); break;
            case 339: r = std::min({a, b, c}); break;
            case 340: r = u32(std::fmax(std::fmax(fa, fb), fc)); break;
            case 341: r = std::uint32_t(std::max({std::int32_t(a), std::int32_t(b), std::int32_t(c)})); break;
            case 342: r = std::max({a, b, c}); break;
            case 343: r = u32(gcn_detail::med3(fa, fb, fc)); break;
            case 344: r = std::uint32_t(gcn_detail::med3i(std::int32_t(a), std::int32_t(b), std::int32_t(c))); break;
            case 345: r = gcn_detail::med3i(a, b, c); break;
            case 349: r = (a > b ? a - b : b - a) + c; break;
            case 351: r = a; break;                 // v_div_fixup_f32: special cases not modelled
            case 361: case 363: r = a * b; break;
            case 362: r = std::uint32_t((std::uint64_t(a) * b) >> 32); break;
            case 364: r = std::uint32_t(std::uint64_t(std::int64_t(std::int32_t(a)) * std::int32_t(b)) >> 32); break;
            case 365: r = a; break;                 // v_div_scale_f32: no rescaling, VCC clear
            case 367: r = u32(std::fma(fa, fb, fc)); break;
            case 374: { const std::uint64_t x = std::uint64_t(a) * b + scalarOr64(src[2], lane, i);
                        r = std::uint32_t(x); high[lane] = std::uint32_t(x >> 32); break; }
            case 375: { const std::uint64_t x = std::uint64_t(std::int64_t(std::int32_t(a)) * std::int32_t(b)) + scalarOr64(src[2], lane, i);
                        r = std::uint32_t(x); high[lane] = std::uint32_t(x >> 32); break; }
            case 385: r = a; break;
            case 389: r = u32(float(std::int32_t(a))); break;
            case 390: r = u32(float(a)); break;
            case 391: r = gcn_detail::cvtU32(fa); break;
            case 392: r = std::uint32_t(gcn_detail::cvtI32(fa)); break;
            case 394: r = gcn_detail::f16Rtz(fa); break;
            case 395: r = u32(gcn_detail::f16(a & 0xffff)); break;
            case 396: r = std::uint32_t(gcn_detail::cvtI32(std::floor(fa + 0.5f))); break;
            case 397: r = std::uint32_t(gcn_detail::cvtI32(std::floor(fa))); break;
            case 398: r = u32(float(std::int32_t(a << 28) >> 28) / 16.0f); break;
            case 401: case 402: case 403: case 404: r = u32(float((a >> (8 * (op - 401))) & 0xff)); break;
            case 416: r = u32(fa - std::floor(fa)); break;
            case 417: r = u32(std::trunc(fa)); break;
            case 418: r = u32(std::ceil(fa)); break;
            case 419: r = u32(std::nearbyint(fa)); break;
            case 420: r = u32(std::floor(fa)); break;
            case 421: r = u32(std::exp2(fa)); break;
            case 422: case 423: r = u32(std::log2(fa)); break;
            case 424: case 425: case 426: case 427: r = u32(1.0f / fa); break;
            case 428: case 429: case 430: r = u32(1.0f / std::sqrt(fa)); break;
            case 435: r = u32(std::sqrt(fa)); break;
            case 437: r = u32(std::sin(fa * 6.28318530718f)); break;
            case 438: r = u32(std::cos(fa * 6.28318530718f)); break;
            case 439: r = ~a; break;
            case 440: { r = 0; for (unsigned bit = 0; bit < 32; ++bit) r |= ((a >> bit) & 1) << (31 - bit); break; }
            case 441: { if (!a) { r = ~0u; break; } r = 0; while (!((a << r) & 0x80000000u)) ++r; break; }
            case 442: { if (!a) { r = ~0u; break; } r = 0; while (!((a >> r) & 1)) ++r; break; }
            case 447: { int e = 0; std::frexp(fa, &e); r = std::uint32_t(fa == 0 || !std::isfinite(fa) ? 0 : e); break; }
            case 448: { int e = 0; r = std::isfinite(fa) ? u32(std::frexp(fa, &e)) : a; break; }
            default: return false;
            }
            results[lane] = finish(r);
        }
        for (unsigned lane = 0; lane < lanes_; ++lane) {
            if (!active(lane)) continue;
            v(dst, lane) = results[lane];
            if (wide) v(dst + 1, lane) = high[lane];
        }
        if ((op >= 293 && op <= 298)) setScalar64(sdst, carry);
        if (op == 365 || op == 374 || op == 375) setScalar64(sdst, 0);
        return true;
    }
    std::uint64_t scalarOr64(unsigned code, unsigned lane, const GCNInstruction& i) {
        if (code >= 256) return std::uint64_t(v(code - 256, lane)) | std::uint64_t(v(code - 255, lane)) << 32;
        return scalar64(code, i);
    }
    static std::uint32_t cube(unsigned op, float x, float y, float z) {
        const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
        float id, sc, tc, ma;
        if (az >= ax && az >= ay) { id = z < 0 ? 5 : 4; sc = z < 0 ? -x : x; tc = -y; ma = 2 * z; }
        else if (ay >= ax) { id = y < 0 ? 3 : 2; sc = x; tc = y < 0 ? -z : z; ma = 2 * y; }
        else { id = x < 0 ? 1 : 0; sc = x < 0 ? z : -z; tc = -y; ma = 2 * x; }
        const float r = op == 324 ? id : op == 325 ? sc : op == 326 ? tc : ma;
        return gcn_detail::u32(r);
    }

    // ---- interpolation, cross-lane, memory, export ----
    bool vintrp(const GCNInstruction& i) {
        if (!attribute) return fail("interpolation without attributes");
        const unsigned dst = (i.word0 >> 18) & 0xff, src = i.word0 & 0xff;
        const unsigned attr = (i.word0 >> 10) & 0x3f, chan = (i.word0 >> 8) & 3;
        const GCNAttributePlane plane = attribute(attr, chan);
        for (unsigned lane = 0; lane < lanes_; ++lane) {
            if (!active(lane)) continue;
            const float weight = gcn_detail::f32(v(src, lane));
            float r;
            if (i.opcode == 0) r = plane.p0 + weight * plane.p10;
            else if (i.opcode == 1) r = gcn_detail::f32(v(dst, lane)) + weight * plane.p20;
            else r = (src & 3) == 0 ? plane.p10 : (src & 3) == 1 ? plane.p20 : plane.p0;
            v(dst, lane) = gcn_detail::u32(r);
        }
        return true;
    }
    bool ds(const GCNInstruction& i) {
        if (i.opcode != 53) return false; // only ds_swizzle_b32 (no LDS is modelled)
        const unsigned dst = i.word1 >> 24, src = i.word1 & 0xff;
        const unsigned offset = i.word0 & 0xffff;
        std::array<std::uint32_t, 64> r{};
        for (unsigned lane = 0; lane < lanes_; ++lane) {
            unsigned from;
            if (offset & 0x8000) from = (lane & ~3u) | ((offset >> (2 * (lane & 3))) & 3);
            else from = (lane & 0x20) | ((((lane & 31) & (offset & 31)) | ((offset >> 5) & 31)) ^ ((offset >> 10) & 31));
            r[lane] = from < lanes_ && active(from) ? v(src, from) : 0;
        }
        for (unsigned lane = 0; lane < lanes_; ++lane) if (active(lane)) v(dst, lane) = r[lane];
        return true;
    }
    // Buffer formats (V# DATA_FORMAT/NUM_FORMAT): 8/16/32-bit channel layouts.
    static bool bufferLayout(unsigned dfmt, unsigned& channels, unsigned& bits) {
        switch (dfmt) {
        case 1: channels = 1; bits = 8; return true;   case 2: channels = 1; bits = 16; return true;
        case 3: channels = 2; bits = 8; return true;   case 4: channels = 1; bits = 32; return true;
        case 5: channels = 2; bits = 16; return true;  case 10: channels = 4; bits = 8; return true;
        case 11: channels = 2; bits = 32; return true; case 12: channels = 4; bits = 16; return true;
        case 13: channels = 3; bits = 32; return true; case 14: channels = 4; bits = 32; return true;
        default: return false;
        }
    }
    static std::uint32_t convertIn(std::uint32_t raw, unsigned bits, unsigned nfmt) {
        const std::uint32_t max = bits == 32 ? ~0u : ((1u << bits) - 1);
        const std::int32_t sraw = bits == 32 ? std::int32_t(raw) : std::int32_t(raw << (32 - bits)) >> (32 - bits);
        switch (nfmt) {
        case 0: return gcn_detail::u32(bits == 32 ? float(raw) / 4294967295.0f : float(raw) / float(max));
        case 1: case 6: return gcn_detail::u32(std::fmax(float(sraw) / float(max >> 1), -1.0f));
        case 2: return gcn_detail::u32(float(raw));
        case 3: return gcn_detail::u32(float(sraw));
        case 4: return raw;
        case 5: return std::uint32_t(sraw);
        case 7: return bits == 16 ? gcn_detail::u32(gcn_detail::f16(raw)) : raw;
        default: return raw;
        }
    }
    bool mubuf(const GCNInstruction& i) {
        const std::uint32_t w = i.word0, w1 = i.word1;
        const unsigned vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff, rsrc = ((w1 >> 16) & 0x1f) * 4;
        const bool offen = (w >> 12) & 1, idxen = (w >> 13) & 1;
        if ((w >> 15) & 1) return fail("addr64 buffer access");
        const std::uint32_t* d = &s[rsrc];
        const std::uint64_t base = std::uint64_t(d[0]) | std::uint64_t(d[1] & 0xfff) << 32; // 44 bits, then the memory type
        const std::uint32_t stride = (d[1] >> 16) & 0x3fff, records = d[2];
        const std::uint32_t soffset = scalar(w1 >> 24, i), instOffset = w & 0xfff;
        const unsigned dst_sel[4] = {d[3] & 7, (d[3] >> 3) & 7, (d[3] >> 6) & 7, (d[3] >> 9) & 7};
        const unsigned nfmt = (d[3] >> 12) & 7, dfmt = (d[3] >> 15) & 15;
        const unsigned op = i.opcode;
        const bool format = op <= 7, store = (op >= 4 && op <= 7) || (op >= 24 && op <= 31);
        unsigned count;
        if (format) count = (op & 3) + 1;
        else if (op == 12 || op == 28) count = 1; else if (op == 13 || op == 29) count = 2;
        else if (op == 14 || op == 30) count = 4; else if (op == 15 || op == 31) count = 3;
        else return false;
        unsigned channels = 1, bits = 32;
        if (format && !bufferLayout(dfmt, channels, bits)) return fail("buffer data format " + std::to_string(dfmt));
        if (resource) resource(base, stride ? std::uint64_t(records) * stride : records);
        if (store && stored) stored(base, stride ? std::uint64_t(records) * stride : records);
        if (buffer) {
            std::uint64_t address = base;
            for (unsigned lane = 0; lane < lanes_; ++lane) {
                if (!active(lane)) continue;
                address = base + soffset + instOffset + std::uint64_t(idxen ? v(vaddr, lane) : 0) * stride +
                          (offen ? v(vaddr + (idxen ? 1 : 0), lane) : 0);
                break;
            }
            buffer(base, stride ? std::uint64_t(records) * stride : records, stride, address);
        }
        for (unsigned lane = 0; lane < lanes_; ++lane) {
            if (!active(lane)) continue;
            const std::uint32_t index = idxen ? v(vaddr, lane) : 0;
            const std::uint32_t voffset = offen ? v(vaddr + (idxen ? 1 : 0), lane) : 0;
            const std::uint64_t address = base + soffset + instOffset + std::uint64_t(index) * stride + voffset;
            const bool inside = stride ? index < records : std::uint64_t(instOffset) + voffset < records;
            const unsigned bytes = format ? channels * bits / 8 : count * 4;
            if (store) {
                if (!inside) continue;
                std::uint8_t data[16] = {};
                if (format) {
                    if (bits != 32 && !(bits == 8 && nfmt == 0)) return fail("buffer store format");
                    for (unsigned c = 0; c < channels && c < count; ++c) {
                        const std::uint32_t x = v(vdata + c, lane);
                        if (bits == 32) std::memcpy(data + 4 * c, &x, 4);
                        else data[c] = std::uint8_t(std::lround(std::fmin(std::fmax(gcn_detail::f32(x), 0.0f), 1.0f) * 255.0f));
                    }
                } else for (unsigned c = 0; c < count; ++c) { const std::uint32_t x = v(vdata + c, lane); std::memcpy(data + 4 * c, &x, 4); }
                if (!memory_.write(address, data, format ? std::min(bytes, count * bits / 8) : bytes)) return fail("unwritable buffer store");
                continue;
            }
            std::uint8_t data[16] = {};
            if (inside && !memory_.read(address, data, bytes)) return fail("unreadable buffer load");
            if (!format) { for (unsigned c = 0; c < count; ++c) std::memcpy(&v(vdata + c, lane), data + 4 * c, 4); continue; }
            std::uint32_t component[4] = {0, 0, 0, 0};
            for (unsigned c = 0; c < channels; ++c) {
                std::uint32_t raw = 0;
                std::memcpy(&raw, data + c * bits / 8, bits / 8);
                component[c] = inside ? convertIn(raw, bits, nfmt) : 0;
            }
            const std::uint32_t one = (nfmt == 4 || nfmt == 5) ? 1 : 0x3f800000;
            for (unsigned c = 0; c < count; ++c) {
                const unsigned sel = dst_sel[c];
                v(vdata + c, lane) = sel >= 4 ? component[sel - 4] : sel == 1 ? one : 0;
            }
        }
        return true;
    }
    // Image resources (T#) for the linear 2D case.
    struct Image {
        std::uint64_t base; unsigned width, height, pitch, dfmt, nfmt, tiling, type, sel[4];
    };
    static bool imageBytes(unsigned dfmt, unsigned& bytes) {
        switch (dfmt) {
        case 1: bytes = 1; return true; case 2: case 3: case 16: case 17: case 18: case 19: bytes = 2; return true;
        case 4: case 5: case 10: bytes = 4; return true; case 11: case 12: bytes = 8; return true;
        case 14: bytes = 16; return true; case 35: case 38: bytes = 8; return true; case 36: case 37: case 39: bytes = 16; return true; default: return false;
        }
    }
    static std::uint64_t imageSize(const Image& t, unsigned bytes) {
        return (t.dfmt >= 35 && t.dfmt <= 39) ? std::uint64_t((t.pitch + 3) / 4) * ((t.height + 3) / 4) * bytes :
                              std::uint64_t(t.pitch) * t.height * bytes;
    }
    bool texel(const Image& t, int x, int y, float out[4]) {
        unsigned bytes = 0;
        imageBytes(t.dfmt, bytes);
        std::uint8_t data[16] = {};
        const std::uint64_t offset = (t.dfmt >= 35 && t.dfmt <= 39) ?
            (std::uint64_t(std::uint32_t(y) / 4) * ((t.pitch + 3) / 4) + std::uint32_t(x) / 4) * bytes :
            (std::uint64_t(std::uint32_t(y)) * t.pitch + std::uint32_t(x)) * bytes;
        const std::vector<std::uint8_t>* surface = nullptr;
        if (images) {
            // Whole level 0 read once per cache lifetime (one draw).
            const std::size_t size = std::size_t(imageSize(t, bytes));
            auto found = images->find(t.base);
            if (found == images->end() || found->second.size() != size) {
                std::vector<std::uint8_t> copy(size);
                if (memory_.read(t.base, copy.data(), size)) found = images->insert_or_assign(t.base, std::move(copy)).first;
                else found = images->end();
            }
            if (found != images->end()) surface = &found->second;
        }
        if (surface && offset + bytes <= surface->size()) std::memcpy(data, surface->data() + offset, bytes);
        else if (!memory_.read(t.base + offset, data, bytes)) return fail("unreadable texel");
        std::uint32_t raw = 0;
        std::memcpy(&raw, data, std::min(bytes, 4u));
        auto unorm = [](std::uint32_t value, unsigned bits) { return float(value) / float((1u << bits) - 1); };
        float c[4] = {0, 0, 0, 1};
        switch (t.dfmt) {
        case 35: case 36: case 37: {
            const bool alpha = t.dfmt != 35;
            std::uint32_t alphaHigh = 0;
            std::memcpy(&alphaHigh, data + 4, 4);
            if (alpha) std::memcpy(&raw, data + 8, 4);
            std::uint32_t selectors = 0;
            std::memcpy(&selectors, data + (alpha ? 12 : 4), 4);
            const unsigned pixel = (unsigned(y) & 3) * 4 + (unsigned(x) & 3);
            for (unsigned n = 0; n < 4; ++n) c[n] = gcn_detail::gcn_bc1_component(raw, selectors, pixel, n, alpha);
            if (alpha) {
                std::uint32_t alphaLow = 0;
                std::memcpy(&alphaLow, data, 4);
                c[3] = gcn_detail::gcn_bc23_alpha(alphaLow, alphaHigh, pixel, t.dfmt == 37);
            }
            break;
        }
        case 38: case 39: {
            const unsigned pixel = (unsigned(y) & 3) * 4 + (unsigned(x) & 3);
            for (unsigned n = 0; n < (t.dfmt == 38 ? 1u : 2u); ++n) {
                std::uint32_t low = 0, high = 0;
                std::memcpy(&low, data + n * 8, 4);
                std::memcpy(&high, data + n * 8 + 4, 4);
                c[n] = t.nfmt == 1 ? gcn_detail::gcn_bc45_snorm(low, high, pixel) :
                    gcn_detail::gcn_bc23_alpha(low, high, pixel, true);
            }
            break;
        }
        case 1: c[0] = unorm(raw & 0xff, 8); break;
        case 2: c[0] = t.nfmt == 7 ? gcn_detail::f16(raw & 0xffff) : unorm(raw & 0xffff, 16); break;
        case 3: c[0] = unorm(raw & 0xff, 8); c[1] = unorm((raw >> 8) & 0xff, 8); break;
        case 4: c[0] = t.nfmt == 7 ? gcn_detail::f32(raw) : float(raw); break;
        case 10: for (unsigned n = 0; n < 4; ++n) c[n] = unorm((raw >> (8 * n)) & 0xff, 8); break;
        case 16: c[0] = unorm(raw & 31, 5); c[1] = unorm((raw >> 5) & 63, 6); c[2] = unorm((raw >> 11) & 31, 5); break;
        case 17: c[0] = unorm(raw & 1, 1); c[1] = unorm((raw >> 1) & 31, 5); c[2] = unorm((raw >> 6) & 31, 5); c[3] = unorm((raw >> 11) & 31, 5); break;
        case 18: c[0] = unorm(raw & 31, 5); c[1] = unorm((raw >> 5) & 31, 5); c[2] = unorm((raw >> 10) & 31, 5); c[3] = unorm((raw >> 15) & 1, 1); break;
        case 19: for (unsigned n = 0; n < 4; ++n) c[n] = unorm((raw >> (4 * n)) & 15, 4); break;
        case 14: for (unsigned n = 0; n < 4; ++n) std::memcpy(&c[n], data + 4 * n, 4); break;
        default: return fail("image data format " + std::to_string(t.dfmt));
        }
        if (t.dfmt != 4 && t.dfmt != 14 && !(t.dfmt >= 38 && t.dfmt <= 39 && t.nfmt == 1) && t.nfmt != 0 && t.nfmt != 7 && t.nfmt != 9)
            return fail("image number format " + std::to_string(t.nfmt));
        for (unsigned n = 0; n < 4; ++n) {
            const unsigned sel = t.sel[n];
            out[n] = sel >= 4 ? c[sel - 4] : sel == 1 ? 1.0f : 0.0f;
        }
        return true;
    }
    static int address(int x, int size, unsigned mode, bool& border) {
        border = false;
        switch (mode) {
        case 0: { const int m = x % size; return m < 0 ? m + size : m; }
        case 1: { const int period = 2 * size; int m = x % period; if (m < 0) m += period; return m < size ? m : period - 1 - m; }
        case 2: return x < 0 ? 0 : x >= size ? size - 1 : x;
        case 3: { const int m = x < 0 ? -x - 1 : x; return m >= size ? size - 1 : m; }
        default: if (x < 0 || x >= size) border = true; return x < 0 ? 0 : x >= size ? size - 1 : x;
        }
    }
    bool sample(const Image& t, const std::uint32_t* ssharp, bool unnormalized, float u, float vcoord, float out[4]) {
        const unsigned clampX = ssharp[0] & 7, clampY = (ssharp[0] >> 3) & 7;
        const bool forceUnnorm = unnormalized || ((ssharp[0] >> 15) & 1);
        const bool linear = ((ssharp[2] >> 20) & 3) == 1 || ((ssharp[2] >> 22) & 3) == 1;
        const unsigned borderType = ssharp[3] >> 30;
        const float borderColor[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}};
        const float x = forceUnnorm ? u : u * float(t.width), y = forceUnnorm ? vcoord : vcoord * float(t.height);
        auto fetchAt = [&](int ix, int iy, float c[4]) {
            bool bx, by;
            const int ax = address(ix, int(t.width), clampX, bx), ay = address(iy, int(t.height), clampY, by);
            if (bx || by) { std::memcpy(c, borderColor[borderType], sizeof(float) * 4); return true; }
            return texel(t, ax, ay, c);
        };
        if (!linear) return fetchAt(int(std::floor(x)), int(std::floor(y)), out);
        const float fx = x - 0.5f, fy = y - 0.5f;
        const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        const float ax = fx - float(x0), ay = fy - float(y0);
        float c00[4], c10[4], c01[4], c11[4];
        if (!fetchAt(x0, y0, c00) || !fetchAt(x0 + 1, y0, c10) || !fetchAt(x0, y0 + 1, c01) || !fetchAt(x0 + 1, y0 + 1, c11)) return false;
        for (unsigned n = 0; n < 4; ++n)
            out[n] = (c00[n] * (1 - ax) + c10[n] * ax) * (1 - ay) + (c01[n] * (1 - ax) + c11[n] * ax) * ay;
        return true;
    }
    bool mimg(const GCNInstruction& i) {
        const std::uint32_t w = i.word0, w1 = i.word1;
        const unsigned vaddr = w1 & 0xff, vdata = (w1 >> 8) & 0xff;
        const unsigned rsrc = ((w1 >> 16) & 0x1f) * 4, samp = ((w1 >> 21) & 0x1f) * 4, dmask = (w >> 8) & 15;
        if (rsrc > s.size() - 8) return fail("image resource register range");
        const std::uint32_t* d = &s[rsrc];
        if (i.opcode == 14) {
            // image_get_resinfo: width, height, depth (3D) or slices, and mip
            // levels at the lane's mip level, as integers; any image type.
            const unsigned base = (d[3] >> 12) & 15, last = (d[3] >> 16) & 15;
            for (unsigned lane = 0; lane < lanes_; ++lane) {
                if (!active(lane)) continue;
                const unsigned shift = std::min<std::uint32_t>(v(vaddr, lane), 31);
                const std::uint32_t depth = (d[4] & 0x1fff) + 1;
                const std::uint32_t r[4] = {std::max<std::uint32_t>(((d[2] & 0x3fff) + 1) >> shift, 1),
                                            std::max<std::uint32_t>((((d[2] >> 14) & 0x3fff) + 1) >> shift, 1),
                                            (d[3] >> 28) == 10 ? std::max<std::uint32_t>(depth >> shift, 1) : depth,
                                            last >= base ? last - base + 1 : 1};
                for (unsigned n = 0, out = 0; n < 4; ++n) if ((dmask >> n) & 1) v(vdata + out++, lane) = r[n];
            }
            return true;
        }
        Image t{(std::uint64_t(d[0]) | std::uint64_t(d[1] & 0xff) << 32) << 8,
                (d[2] & 0x3fff) + 1, ((d[2] >> 14) & 0x3fff) + 1, ((d[4] >> 13) & 0x3fff) + 1,
                (d[1] >> 20) & 0x3f, (d[1] >> 26) & 15, (d[3] >> 20) & 0x1f, d[3] >> 28,
                {d[3] & 7, (d[3] >> 3) & 7, (d[3] >> 6) & 7, (d[3] >> 9) & 7}};
        if (t.type != 9) return fail("image type " + std::to_string(t.type));
        // Preserve the destination address even when its tiled layout cannot
        // be interpreted by this CPU-side resource scan. The renderer uses
        // this to refuse unsupported writes instead of silently dropping them.
        if (i.opcode == 8 && stored) stored(t.base, 1);
        if (t.tiling != 8) {
            // Still named to imageResource: a tiled T# is often a colour target
            // the Metal renderer holds as a texture.
            if (imageResource) {
                imageSampler = {};
                if (i.opcode >= 32 && i.opcode < 64 && samp <= s.size() - 4) std::copy_n(&s[samp], 4, imageSampler.begin());
                std::array<std::uint32_t, 8> descriptor{};
                std::copy_n(d, 8, descriptor.begin()); imageResource(descriptor);
            }
            // A scan goes on: resources read after this image must still be found.
            if (tiledReadsZero) {
                if (i.opcode == 8) return true;
                const unsigned written = i.opcode >= 64 ? 15 : dmask;
                for (unsigned lane = 0; lane < lanes_; ++lane) {
                    if (!active(lane)) continue;
                    for (unsigned n = 0, out = 0; n < 4; ++n) if ((written >> n) & 1) v(vdata + out++, lane) = 0;
                }
                return true;
            }
            return fail("tiled image (tile mode " + std::to_string(t.tiling) + ")");
        }
        unsigned bytes;
        if (!imageBytes(t.dfmt, bytes)) return fail("image data format " + std::to_string(t.dfmt));
        if (t.dfmt >= 35 && t.dfmt <= 39 &&
            ((t.nfmt != 0 && !(t.dfmt >= 38 && t.nfmt == 1)) || t.pitch < t.width))
            return fail("BC" + std::to_string(t.dfmt - 34) +
                (t.dfmt >= 38 ? " requires UNORM/SNORM and pitch >= width" : " requires UNORM and pitch >= width"));
        if (resource) resource(t.base, imageSize(t, bytes));
        if (i.opcode == 8 && stored) stored(t.base, imageSize(t, bytes));
        if (imageResource) {
            imageSampler = {};
            if (i.opcode >= 32 && i.opcode < 64 && samp <= s.size() - 4) std::copy_n(&s[samp], 4, imageSampler.begin());
            std::array<std::uint32_t, 8> descriptor{};
            std::copy_n(d, 8, descriptor.begin()); imageResource(descriptor);
        }
        const unsigned op = i.opcode;
        // Address layout: [offset][bias][compare] coordinates [lod/clamp].
        unsigned first = 0;
        if (op == 37 || op == 38 || op == 53 || op == 54) first = 1;      // bias variants
        if (op >= 48 && op < 64) first += 1;                              // _o: offsets first
        if ((op >= 40 && op < 48) || (op >= 56 && op < 64)) return fail("depth-compare sampling");
        const bool sampleOp = op >= 32 && op < 64, load = op == 0 || op == 1, store = op == 8;
        if (!sampleOp && !load && !store) return false;
        for (unsigned lane = 0; lane < lanes_; ++lane) {
            if (!active(lane)) continue;
            float c[4] = {0, 0, 0, 0};
            if (sampleOp) {
                if (!sample(t, &s[samp], (w >> 12) & 1, gcn_detail::f32(v(vaddr + first, lane)),
                            gcn_detail::f32(v(vaddr + first + 1, lane)), c)) return false;
            } else {
                const std::uint32_t x = v(vaddr, lane), y = v(vaddr + 1, lane);
                if (op == 1 && v(vaddr + 2, lane)) return fail("image mip level above 0");
                if (x >= t.width || y >= t.height) { if (!store) for (unsigned n = 0, out = 0; n < 4; ++n) if ((dmask >> n) & 1) v(vdata + out++, lane) = 0; continue; }
                if (store) {
                    if (t.dfmt != 10 || t.nfmt != 0) return fail("image store format");
                    std::uint32_t packed = 0;
                    for (unsigned n = 0, in = 0; n < 4; ++n) {
                        if (!((dmask >> n) & 1)) continue;
                        const float f = std::fmin(std::fmax(gcn_detail::f32(v(vdata + in++, lane)), 0.0f), 1.0f);
                        packed |= std::uint32_t(std::lround(f * 255.0f)) << (8 * n);
                    }
                    if (images) images->erase(t.base);
                    if (!memory_.write(t.base + (std::uint64_t(y) * t.pitch + x) * 4, &packed, 4)) return fail("unwritable image store");
                    continue;
                }
                if (!texel(t, int(x), int(y), c)) return false;
            }
            for (unsigned n = 0, out = 0; n < 4; ++n)
                if ((dmask >> n) & 1) v(vdata + out++, lane) = gcn_detail::u32(c[n]);
        }
        return true;
    }
    bool exportOp(const GCNInstruction& i) {
        GCNExport e;
        e.target = (i.word0 >> 4) & 0x3f;
        e.enable = i.word0 & 15;
        e.compressed = (i.word0 >> 10) & 1;
        e.done = (i.word0 >> 11) & 1;
        e.mask = exec();
        for (unsigned c = 0; c < 4; ++c) {
            const unsigned reg = (i.word1 >> (8 * c)) & 0xff;
            for (unsigned lane = 0; lane < lanes_; ++lane) e.value[c][lane] = v(reg, lane);
        }
        exports.push_back(e);
        return true;
    }
};
} // namespace maxps4::gpu
