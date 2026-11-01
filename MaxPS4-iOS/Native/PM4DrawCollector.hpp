#pragma once
// Graphics PM4 register state and the draws it describes. Each draw packet
// becomes a self-contained PM4Draw: programs and user SGPRs, PS inputs,
// primitive and resolved indices, viewport, scissor and colour target 0.
// The software renderer (CPU) and the Metal renderer execute the same
// PM4Draw, so both see one interpretation of the registers. Draws this
// subset cannot describe are counted as failed with a reason; draws that
// write no colour (target mask 0 or no target) are counted as skipped.
#include "GCNInterpreter.hpp"
#include "PM4Type3Decoder.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace maxps4::gpu {
struct PM4DrawStats {
    unsigned draws = 0, rendered = 0, skipped = 0, failed = 0, dispatches = 0;
    std::string error; // the first failure
    // Diagnostics: the registers of the last draw skipped for want of a colour
    // target, and the last dispatch (program address and group counts).
    std::string lastSkipped, lastDispatch;
    void fail(const std::string& message) { ++failed; if (error.empty()) error = message; }
};
struct PM4Draw {
    std::uint64_t vs = 0, ps = 0;                     // program addresses
    std::uint32_t vsUser[16] = {}, psUser[16] = {};   // user SGPRs
    unsigned vsUserCount = 0, psUserCount = 0;
    std::uint32_t psInputAddr = 0;                    // SPI_PS_INPUT_ADDR (or _ENA)
    std::uint32_t inputCntl[32] = {};                 // SPI_PS_INPUT_CNTL_n
    unsigned primitive = 4;                           // 4 list (also normalized fans), 6 strip, 0x11 rect list
    std::vector<std::uint32_t> indices;               // VGT_INDX_OFFSET applied
    std::uint32_t vte = 0;                            // PA_CL_VTE_CNTL
    float viewport[6] = {};                           // X/Y/Z scale and offset
    unsigned scissor[4] = {};                         // x0, y0, x1, y1 (exclusive), inside the target
    std::uint64_t target = 0;                         // CB_COLOR0 base: 8_8_8_8 UNORM, linear
    unsigned pitch = 0, height = 0;                   // pixels
    unsigned swap = 0, mask = 0;                      // CB COMP_SWAP, CB_TARGET_MASK RGBA
    std::uint32_t blend = 0;                          // CB_BLEND0_CONTROL
    float blendConstant[4] = {};                     // CB_BLEND_RED/GREEN/BLUE/ALPHA
};

class PM4DrawCollector {
public:
    PM4DrawCollector(const GCNMemory& memory, PM4DrawStats& stats) : memory_(memory), stats_(stats) {}
    // Executes one submission and calls draw(const PM4Draw&) for each colour
    // draw; register state persists across submissions.
    template <class Draw> void execute(const std::uint32_t* words, std::size_t count, Draw&& draw) {
        const auto parsed = decodePM4Type3(words, count);
        if (!parsed) { stats_.fail("unframed PM4 stream"); return; }
        for (const auto& packet : parsed.packets) {
            const std::uint32_t* data = words + packet.payloadWord;
            const std::size_t n = packet.payloadCount;
            switch (packet.opcode) {
            case 0x69: setRegisters(context_, data, n); break;
            case 0x76: setRegisters(shader_, data, n); break;
            case 0x79: setRegisters(uconfig_, data, n); break;
            case 0x12: context_.fill(0); break;                           // CLEAR_STATE
            case 0x2a: indexBytes_ = (data[0] & 3) == 1 ? 4 : 2; break;     // INDEX_TYPE
            case 0x26:                                                  // INDEX_BASE
                if (n != 2 || data[1] > 0xffff) { stats_.fail("index base packet"); return; }
                indexBase_ = std::uint64_t(data[0]) | std::uint64_t(data[1]) << 32;
                break;
            case 0x27:                                                  // DRAW_INDEX_2 also updates INDEX_BASE
                if (n != 5 || data[2] > 0xffff) { stats_.fail("indexed draw packet"); return; }
                indexBase_ = std::uint64_t(data[1]) | std::uint64_t(data[2]) << 32;
                collect(data[3], true, indexBase_, draw);
                break;
            case 0x35: {                                                // DRAW_INDEX_OFFSET_2
                if (n != 4) { stats_.fail("index offset draw packet"); return; }
                const std::uint64_t offset = std::uint64_t(data[1]) * indexBytes_;
                constexpr std::uint64_t addressLimit = (std::uint64_t{1} << 48) - 1;
                if ((words[packet.headerWord] & 0xff) || data[3] != 0) {
                    stats_.fail("index offset draw flags"); return;
                }
                if (!indexBase_ || indexBase_ % indexBytes_ || !data[2] || data[2] > data[0] ||
                    offset > addressLimit - indexBase_ ||
                    std::uint64_t(data[2]) * indexBytes_ - 1 > addressLimit - indexBase_ - offset) {
                    stats_.fail("index offset draw range"); return;
                }
                collect(data[2], true, indexBase_ + offset, draw);
                break;
            }
            case 0x2d: if (n >= 1) collect(data[0], false, 0, draw); break;
            case 0x15: {                                                // DISPATCH_DIRECT
                ++stats_.dispatches;
                // COMPUTE_PGM_LO/HI (0x2e0c), COMPUTE_NUM_THREAD_X/Y/Z (0x2e07).
                const std::uint64_t program = (std::uint64_t(shader_[0x20c]) | std::uint64_t(shader_[0x20d] & 0xff) << 32) << 8;
                char text[160];
                std::snprintf(text, sizeof text, "programme 0x%llx, groupes %ux%ux%u de %ux%ux%u",
                              (unsigned long long)program, n > 0 ? data[0] : 0, n > 1 ? data[1] : 0, n > 2 ? data[2] : 0,
                              shader_[0x207] & 0xffff, shader_[0x208] & 0xffff, shader_[0x209] & 0xffff);
                stats_.lastDispatch = text;
                break;
            }
            default: break; // synchronization and prefetch packets: nothing to draw
            }
        }
    }
    // Registers by offset from their bank base (context 0xa000, shader 0x2c00, uconfig 0xc000).
    std::uint32_t context(unsigned offset) const { return context_[offset & 0x3ff]; }

private:
    const GCNMemory& memory_;
    PM4DrawStats& stats_;
    std::array<std::uint32_t, 0x400> context_{}, shader_{}, uconfig_{};
    unsigned indexBytes_ = 2;
    std::uint64_t indexBase_ = 0; // Persists across submissions, like INDEX_TYPE.

    static void setRegisters(std::array<std::uint32_t, 0x400>& bank, const std::uint32_t* data, std::size_t n) {
        for (std::size_t k = 1; k < n; ++k) if (data[0] + k - 1 < bank.size()) bank[data[0] + k - 1] = data[k];
    }
    static float f(std::uint32_t x) { return gcn_detail::f32(x); }
    template <class Draw> void collect(std::uint32_t count, bool indexed, std::uint64_t indexAddress, Draw& draw) {
        ++stats_.draws;
        if (!(context_[0x8e] & 0xf) || !context_[0x318]) {
            ++stats_.skipped;
            // CB_TARGET_MASK, CB_COLOR0/1 BASE and INFO format, CB_COLOR_CONTROL,
            // DB_Z_INFO, PS program.
            char text[200];
            std::snprintf(text, sizeof text, "masque 0x%x, cible0 0x%llx format %u, cible1 0x%llx format %u, mode CB %u, profondeur %u, PS 0x%llx",
                          context_[0x8e], (unsigned long long)(std::uint64_t(context_[0x318]) << 8), (context_[0x31c] >> 2) & 0x1f,
                          (unsigned long long)(std::uint64_t(context_[0x327]) << 8), (context_[0x32b] >> 2) & 0x1f,
                          (context_[0x202] >> 4) & 7, context_[0x10] & 3,
                          (unsigned long long)((std::uint64_t(shader_[0x8]) | std::uint64_t(shader_[0x9] & 0xff) << 32) << 8));
            stats_.lastSkipped = text;
            return;
        }
        const std::uint32_t info = context_[0x31c];
        if (((info >> 2) & 0x1f) != 0xa || ((info >> 8) & 7) != 0) { stats_.fail("colour target format"); return; }
        PM4Draw d;
        d.primitive = uconfig_[0x242];
        if (d.primitive != 4 && d.primitive != 5 && d.primitive != 6 && d.primitive != 0x11) {
            stats_.fail("primitive type " + std::to_string(d.primitive)); return;
        }
        if (!count || count > (1u << 20)) { stats_.fail("draw count"); return; }
        const bool fan = d.primitive == 5;
        const bool restart = indexed && (context_[0x2a5] & 1);
        const bool split = restart && (fan || d.primitive == 6);
        const std::uint32_t marker = context_[0x103] & (indexBytes_ == 2 ? 0xffffu : 0xffffffffu);
        if (fan || split) {
            const std::size_t triangles = count >= 3 ? count - 2 : 0;
            if (!triangles) { ++stats_.skipped; return; }
            if (!split && triangles > (1u << 20) / 3) { stats_.fail("expanded draw count"); return; }
        }
        d.indices.resize(count);
        if (indexed) {
            std::vector<std::uint8_t> bytes(std::size_t(count) * indexBytes_);
            if (!memory_.read(indexAddress, bytes.data(), bytes.size())) { stats_.fail("unreadable index buffer"); return; }
            for (std::uint32_t k = 0; k < count; ++k) {
                std::uint32_t value = 0;
                std::memcpy(&value, bytes.data() + std::size_t(k) * indexBytes_, indexBytes_);
                d.indices[k] = value; // Restart compares raw indices, before VGT_INDX_OFFSET.
            }
        } else for (std::uint32_t k = 0; k < count; ++k) d.indices[k] = k;
        // A retained enable bit alone does not invalidate other topologies.
        // Refuse an actual restart there until their assembly rules are supported.
        if (restart && !split && std::find(d.indices.begin(), d.indices.end(), marker) != d.indices.end()) {
            stats_.fail("primitive restart topology"); return;
        }
        if (fan || split) {
            std::vector<std::uint32_t> triangles;
            triangles.reserve(std::min<std::size_t>((count - 2) * 3, 1u << 20));
            std::uint32_t anchor = 0, previous = 0, last = 0;
            unsigned segmentSize = 0;
            for (const auto raw : d.indices) {
                if (split && raw == marker) { segmentSize = 0; continue; }
                const auto value = raw + (indexed ? context_[0x102] : 0);
                if (!segmentSize) anchor = value;
                if (segmentSize >= 2) {
                    if (triangles.size() > (1u << 20) - 3) { stats_.fail("expanded draw count"); return; }
                    if (fan) triangles.insert(triangles.end(), {anchor, last, value});
                    else if (segmentSize & 1) triangles.insert(triangles.end(), {last, previous, value});
                    else triangles.insert(triangles.end(), {previous, last, value});
                }
                previous = last; last = value; ++segmentSize;
            }
            if (triangles.empty()) { ++stats_.skipped; return; }
            d.indices = std::move(triangles);
            d.primitive = 4;
        } else if (indexed) for (auto& value : d.indices) value += context_[0x102];
        d.vs = (std::uint64_t(shader_[0x48]) | std::uint64_t(shader_[0x49]) << 32) << 8;
        d.ps = (std::uint64_t(shader_[0x08]) | std::uint64_t(shader_[0x09]) << 32) << 8;
        d.vsUserCount = std::min(16u, (shader_[0x4b] >> 1) & 0x1f);
        d.psUserCount = std::min(16u, (shader_[0x0b] >> 1) & 0x1f);
        for (unsigned k = 0; k < 16; ++k) { d.vsUser[k] = shader_[0x4c + k]; d.psUser[k] = shader_[0x0c + k]; }
        d.psInputAddr = context_[0x1b4] ? context_[0x1b4] : context_[0x1b3];
        for (unsigned a = 0; a < 32; ++a) d.inputCntl[a] = context_[0x191 + a];
        d.vte = context_[0x206];
        for (unsigned k = 0; k < 6; ++k) d.viewport[k] = f(context_[0x10f + k]);
        // CB_COLOR0_PITCH in 8-pixel units, CB_COLOR0_SLICE in 64-pixel units.
        d.target = std::uint64_t(context_[0x318]) << 8;
        d.pitch = ((context_[0x319] & 0x7ff) + 1) * 8;
        d.height = unsigned((std::uint64_t((context_[0x31a] & 0x3fffff) + 1) * 64) / d.pitch);
        d.swap = (info >> 11) & 3;
        d.mask = context_[0x8e] & 0xf;
        d.blend = context_[0x1e0];
        for (unsigned k = 0; k < 4; ++k) d.blendConstant[k] = f(context_[0x105 + k]);
        d.scissor[0] = std::min(context_[0x0c] & 0xffff, d.pitch);
        d.scissor[1] = std::min(context_[0x0c] >> 16, d.height);
        d.scissor[2] = std::min(context_[0x0d] & 0xffff, d.pitch);
        d.scissor[3] = std::min(context_[0x0d] >> 16, d.height);
        draw(static_cast<const PM4Draw&>(d));
    }
};
} // namespace maxps4::gpu
