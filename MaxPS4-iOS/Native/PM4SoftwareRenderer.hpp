#pragma once
// Software execution of graphics PM4 streams through the GCN interpreter:
// the draws PM4DrawCollector describes run their real vertex and pixel
// programs, with viewport, scissor, rasterization in 8x8 tiles of 2x2 quads
// and colour target 0 writes (8_8_8_8 UNORM).
// Known limits, each reported or documented rather than guessed: no
// clipping (triangles with w <= 0 are dropped), no depth/stencil, no
// culling, colour target 0 only, render targets written linearly (PS4
// tiling is not implemented; only the display reads these buffers), and
// compute dispatches are counted but not run, instance 0 only. A reference,
// not a fast path.
#include "PM4DrawCollector.hpp"
#include "PM4Blend.hpp"
#include <cmath>

namespace maxps4::gpu {
class PM4SoftwareRenderer {
public:
    using Stats = PM4DrawStats;
    explicit PM4SoftwareRenderer(const GCNMemory& memory) : memory_(memory), collector_(memory, stats) {}
    Stats stats;
    // Executes one submission; register state persists across submissions.
    void execute(const std::uint32_t* words, std::size_t count) {
        collector_.execute(words, count, [&](const PM4Draw& d) { if (render(d)) ++stats.rendered; });
    }
    std::uint32_t context(unsigned offset) const { return collector_.context(offset); }

private:
    struct Vertex { float pos[4] = {0, 0, 0, 1}; std::array<std::array<float, 4>, 32> param{}; };
    const GCNMemory& memory_;
    PM4DrawCollector collector_;

    static float f(std::uint32_t x) { return gcn_detail::f32(x); }

    bool runVertices(const PM4Draw& d, std::vector<Vertex>& out, GCNCodeCache& cache) {
        out.assign(d.indices.size(), Vertex{});
        for (std::size_t first = 0; first < d.indices.size(); first += 64) {
            const unsigned lanes = unsigned(std::min<std::size_t>(64, d.indices.size() - first));
            GCNWave wave(memory_, lanes, &cache);
            for (unsigned k = 0; k < d.vsUserCount; ++k) wave.s[k] = d.vsUser[k];
            for (unsigned lane = 0; lane < lanes; ++lane) { wave.v(0, lane) = d.indices[first + lane]; wave.v(3, lane) = 0; }
            if (!wave.run(d.vs)) { stats.fail("VS: " + wave.error); return false; }
            for (const auto& e : wave.exports) {
                for (unsigned lane = 0; lane < lanes; ++lane) {
                    if (!((e.mask >> lane) & 1)) continue;
                    float value[4];
                    for (unsigned c = 0; c < 4; ++c) value[c] = f(e.value[c][lane]);
                    if (e.compressed) {
                        value[0] = gcn_detail::f16(e.value[0][lane] & 0xffff); value[1] = gcn_detail::f16(e.value[0][lane] >> 16);
                        value[2] = gcn_detail::f16(e.value[1][lane] & 0xffff); value[3] = gcn_detail::f16(e.value[1][lane] >> 16);
                    }
                    Vertex& vertex = out[first + lane];
                    if (e.target == 12) for (unsigned c = 0; c < 4; ++c) vertex.pos[c] = value[c];
                    else if (e.target >= 32) for (unsigned c = 0; c < 4; ++c) vertex.param[e.target - 32][c] = value[c];
                }
            }
        }
        return true;
    }

    bool render(const PM4Draw& d) {
        if (!decodePM4Blend(d.blend).supported()) { stats.fail("blend mode"); return false; }
        GCNCodeCache vsCache, psCache;
        GCNImageCache images; // textures as they are for this draw
        std::vector<Vertex> vertices;
        if (!runVertices(d, vertices, vsCache)) return false;
        std::vector<std::array<Vertex, 3>> triangles;
        if (d.primitive == 4) for (std::size_t k = 0; k + 2 < vertices.size(); k += 3) triangles.push_back({vertices[k], vertices[k + 1], vertices[k + 2]});
        else if (d.primitive == 6) for (std::size_t k = 0; k + 2 < vertices.size(); ++k) triangles.push_back({vertices[k], vertices[k + 1], vertices[k + 2]});
        else for (std::size_t k = 0; k + 2 < vertices.size(); k += 3) {
            // A rectangle from three corners; the fourth is v1 + v2 - v0.
            Vertex fourth;
            for (unsigned c = 0; c < 4; ++c) fourth.pos[c] = vertices[k + 1].pos[c] + vertices[k + 2].pos[c] - vertices[k].pos[c];
            for (unsigned p = 0; p < 32; ++p) for (unsigned c = 0; c < 4; ++c)
                fourth.param[p][c] = vertices[k + 1].param[p][c] + vertices[k + 2].param[p][c] - vertices[k].param[p][c];
            triangles.push_back({vertices[k], vertices[k + 1], vertices[k + 2]});
            triangles.push_back({vertices[k + 2], vertices[k + 1], fourth});
        }
        for (const auto& triangle : triangles) if (!rasterize(d, triangle, psCache, images)) return false;
        return true;
    }

    bool rasterize(const PM4Draw& d, const std::array<Vertex, 3>& t, GCNCodeCache& cache, GCNImageCache& images) {
        // Viewport transform (PA_CL_VPORT_*, PA_CL_VTE_CNTL).
        const std::uint32_t vte = d.vte;
        float sx[3], sy[3], sz[3], rw[3];
        for (unsigned k = 0; k < 3; ++k) {
            const float w = t[k].pos[3];
            if (!(w > 0)) return true; // behind the eye: clipping is not implemented
            const bool divide = !((vte >> 8) & 1);
            const float x = divide ? t[k].pos[0] / w : t[k].pos[0], y = divide ? t[k].pos[1] / w : t[k].pos[1];
            const float z = t[k].pos[2] / w;
            sx[k] = ((vte & 1) ? x * d.viewport[0] : x) + ((vte & 2) ? d.viewport[1] : 0);
            sy[k] = ((vte & 4) ? y * d.viewport[2] : y) + ((vte & 8) ? d.viewport[3] : 0);
            sz[k] = ((vte & 16) ? z * d.viewport[4] : z) + ((vte & 32) ? d.viewport[5] : 0);
            rw[k] = 1.0f / w;
        }
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (area == 0 || !std::isfinite(area)) return true;
        int minX = int(d.scissor[0]), minY = int(d.scissor[1]), maxX = int(d.scissor[2]), maxY = int(d.scissor[3]);
        minX = std::max(minX, int(std::floor(std::min({sx[0], sx[1], sx[2]}))));
        minY = std::max(minY, int(std::floor(std::min({sy[0], sy[1], sy[2]}))));
        maxX = std::min(maxX, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))) + 1);
        maxY = std::min(maxY, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))) + 1);
        if (minX >= maxX || minY >= maxY) return true;
        const float sign = area > 0 ? 1.0f : -1.0f;
        // Top-left rule: a pixel exactly on an edge belongs to top or left edges only.
        auto edge = [&](unsigned a, unsigned b, float px, float py) {
            return sign * ((sx[b] - sx[a]) * (py - sy[a]) - (sy[b] - sy[a]) * (px - sx[a]));
        };
        auto owns = [&](unsigned a, unsigned b) {
            const float dx = sign * (sx[b] - sx[a]), dy = sign * (sy[b] - sy[a]);
            return (dy == 0 && dx < 0) || dy > 0;
        };
        const bool own[3] = {owns(1, 2), owns(2, 0), owns(0, 1)};
        // Parameter planes as the parameter cache would hold them.
        std::array<std::array<GCNAttributePlane, 4>, 32> planes{};
        for (unsigned attr = 0; attr < 32; ++attr) {
            const std::uint32_t cntl = d.inputCntl[attr];
            const unsigned param = cntl & 0x1f;
            for (unsigned c = 0; c < 4; ++c) {
                if (cntl & 0x20) { // DEFAULT_VAL: (0,0,0,0), (0,0,0,1), (1,1,1,0), (1,1,1,1)
                    const unsigned dv = (cntl >> 8) & 3;
                    planes[attr][c] = {(c == 3 ? (dv & 1) : (dv >> 1)) ? 1.0f : 0.0f, 0, 0}; continue;
                }
                const float p0 = t[0].param[param][c];
                const bool flat = (cntl >> 10) & 1;
                planes[attr][c] = {p0, flat ? 0 : t[1].param[param][c] - p0, flat ? 0 : t[2].param[param][c] - p0};
            }
        }
        for (int ty = minY & ~7; ty < maxY; ty += 8) {
            for (int tx = minX & ~7; tx < maxX; tx += 8) {
                GCNWave wave(memory_, 64, &cache);
                wave.images = &images;
                std::uint64_t covered = 0;
                for (unsigned lane = 0; lane < 64; ++lane) {
                    const unsigned quad = lane >> 2;
                    const int x = tx + 2 * int(quad & 3) + int(lane & 1), y = ty + 2 * int(quad >> 2) + int((lane >> 1) & 1);
                    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                    const float e0 = edge(1, 2, px, py), e1 = edge(2, 0, px, py), e2 = edge(0, 1, px, py);
                    const bool inside = (e0 > 0 || (e0 == 0 && own[0])) && (e1 > 0 || (e1 == 0 && own[1])) &&
                                        (e2 > 0 || (e2 == 0 && own[2]));
                    if (inside && x >= minX && x < maxX && y >= minY && y < maxY) covered |= 1ull << lane;
                    // Barycentrics (also for helper lanes, by extrapolation).
                    const float b0 = e0 / (sign * area), b1 = e1 / (sign * area), b2 = e2 / (sign * area);
                    const float q0 = b0 * rw[0], q1 = b1 * rw[1], q2 = b2 * rw[2], qs = q0 + q1 + q2;
                    const float pi = qs != 0 ? q1 / qs : b1, pj = qs != 0 ? q2 / qs : b2;
                    // SPI_PS_INPUT_ADDR decides the input VGPR layout.
                    static const unsigned sizes[16] = {2, 2, 2, 3, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1};
                    unsigned reg = 0;
                    for (unsigned bit = 0; bit < 16; ++bit) {
                        if (!((d.psInputAddr >> bit) & 1)) continue;
                        float value[3] = {0, 0, 0};
                        if (bit <= 2) { value[0] = pi; value[1] = pj; }
                        else if (bit == 3) { value[0] = qs; value[1] = q1; value[2] = q2; }
                        else if (bit <= 6) { value[0] = b1; value[1] = b2; }
                        else if (bit == 8) value[0] = px;
                        else if (bit == 9) value[0] = py;
                        else if (bit == 10) value[0] = b0 * sz[0] + b1 * sz[1] + b2 * sz[2];
                        else if (bit == 11) value[0] = qs;
                        else if (bit == 12) value[0] = area > 0 ? 1.0f : 0.0f;
                        else if (bit == 14) { wave.v(reg, lane) = 1; ++reg; continue; }
                        for (unsigned k = 0; k < sizes[bit]; ++k) wave.v(reg + k, lane) = gcn_detail::u32(value[k]);
                        reg += sizes[bit];
                    }
                }
                if (!covered) continue;
                for (unsigned k = 0; k < d.psUserCount; ++k) wave.s[k] = d.psUser[k];
                wave.setExec(covered);
                wave.attribute = [&](unsigned attr, unsigned chan) { return planes[attr & 31][chan & 3]; };
                if (!wave.run(d.ps)) { stats.fail("PS: " + wave.error); return false; }
                for (const auto& e : wave.exports) {
                    if (e.target != 0) continue;
                    if (!writeColor(d, e, covered, tx, ty)) return false;
                }
            }
        }
        return true;
    }

    bool writeColor(const PM4Draw& d, const GCNExport& e, std::uint64_t covered, int tx, int ty) {
        for (unsigned row = 0; row < 8; ++row) {
            // Rows of the tile with no covered pixel may lie past the target.
            const std::uint64_t rowLanes = 0x3333ull << (16 * (row >> 1)) << (2 * (row & 1));
            if (!(covered & e.mask & rowLanes)) continue;
            std::uint8_t pixels[32];
            const std::uint64_t address = d.target + (std::uint64_t(ty + int(row)) * d.pitch + std::uint64_t(tx)) * 4;
            if (!memory_.read(address, pixels, sizeof pixels)) { stats.fail("unreadable colour target"); return false; }
            bool changed = false;
            for (unsigned column = 0; column < 8; ++column) {
                const unsigned lane = ((row >> 1) * 4 + (column >> 1)) * 4 + (column & 1) + 2 * (row & 1);
                if (!((covered >> lane) & 1) || !((e.mask >> lane) & 1)) continue;
                float c[4];
                if (e.compressed) {
                    c[0] = gcn_detail::f16(e.value[0][lane] & 0xffff); c[1] = gcn_detail::f16(e.value[0][lane] >> 16);
                    c[2] = gcn_detail::f16(e.value[1][lane] & 0xffff); c[3] = gcn_detail::f16(e.value[1][lane] >> 16);
                } else for (unsigned k = 0; k < 4; ++k) c[k] = (e.enable >> k) & 1 ? f(e.value[k][lane]) : (k == 3 ? 1.0f : 0.0f);
                // Byte positions of R, G, B, A for CB COMP_SWAP STD, ALT, STD_REV, ALT_REV.
                static const unsigned order[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {1, 2, 3, 0}};
                std::uint8_t* p = pixels + 4 * column;
                float dst[4];
                for (unsigned k = 0; k < 4; ++k) dst[k] = float(p[order[d.swap][k]]) / 255.0f;
                if ((d.blend >> 30) & 1) blendColor(d.blend, c, dst, d.blendConstant);
                for (unsigned k = 0; k < 4; ++k) {
                    if (!((d.mask >> k) & 1)) continue;
                    const float v = std::isnan(c[k]) ? 0.0f : std::fmin(std::fmax(c[k], 0.0f), 1.0f);
                    p[order[d.swap][k]] = std::uint8_t(std::lround(v * 255.0f));
                }
                changed = true;
            }
            if (changed && !memory_.write(address, pixels, sizeof pixels)) { stats.fail("unwritable colour target"); return false; }
        }
        return true;
    }
    // UNORM blending; MIN/MAX compare unweighted components.
    static void blendColor(std::uint32_t control, float c[4], const float dst[4], const float constant[4]) {
        for (unsigned k = 0; k < 4; ++k)
            c[k] = std::isnan(c[k]) ? 0.0f : std::fmin(std::fmax(c[k], 0.0f), 1.0f);
        auto factor = [&](unsigned code, unsigned k) -> float {
            switch (code) {
            case 0: return 0; case 1: return 1;
            case 2: return c[k]; case 3: return 1 - c[k];
            case 4: return c[3]; case 5: return 1 - c[3];
            case 6: return dst[3]; case 7: return 1 - dst[3];
            case 8: return dst[k]; case 9: return 1 - dst[k];
            case 10: return k == 3 ? 1.0f : std::fmin(c[3], 1 - dst[3]);
            case 13: return pm4BlendConstant(constant[k]);
            case 14: return 1 - pm4BlendConstant(constant[k]);
            case 19: return pm4BlendConstant(constant[3]);
            case 20: return 1 - pm4BlendConstant(constant[3]);
            default: return 1;
            }
        };
        const auto state = decodePM4Blend(control);
        float out[4];
        for (unsigned k = 0; k < 4; ++k) {
            const auto channel = k == 3 ? state.alpha : state.rgb;
            if (channel.operation == 2) { out[k] = std::fmin(c[k], dst[k]); continue; }
            if (channel.operation == 3) { out[k] = std::fmax(c[k], dst[k]); continue; }
            const float source = c[k] * factor(channel.source, k);
            const float destination = dst[k] * factor(channel.destination, k);
            out[k] = channel.operation == 1 ? source - destination :
                     channel.operation == 4 ? destination - source : source + destination;
        }
        for (unsigned k = 0; k < 4; ++k) c[k] = out[k];
    }
};
} // namespace maxps4::gpu
