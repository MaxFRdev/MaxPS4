#pragma once
// Graphics PM4 register state and the draws it describes. Each draw packet
// becomes a self-contained PM4Draw: programs and user SGPRs, PS inputs,
// primitive and resolved indices, viewport, scissor, colour targets 0-7 (MRT), the
// depth buffer and face culling.
// The software renderer (CPU) and the Metal renderer execute the same
// PM4Draw, so both see one interpretation of the registers. Draws this
// subset cannot describe are counted as failed with a reason; draws that
// write no colour (target mask 0 or no target) are counted as skipped, unless
// depthOnlyDraws is set and they write or clear a depth buffer.
#include "GCNInterpreter.hpp"
#include "PM4Type3Decoder.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace maxps4::gpu {
struct PM4DrawStats {
    unsigned draws = 0, rendered = 0, skipped = 0, failed = 0, dispatches = 0;
    unsigned fastClears = 0; // ELIMINATE_FAST_CLEAR passes, applied as clears (PM4Draw::fastClear)
    unsigned dmaCopies = 0;  // DMA_DATA copies and fills done in guest memory
    std::uint32_t lastDmaControl = 0, lastDmaCommand = 0; // the last DMA_DATA met
    unsigned indirectBuffers = 0; // INDIRECT_BUFFER calls executed
    unsigned instancedDraws = 0;  // draws of more than one instance
    // Constant engine (the submission's CCBs): CE RAM writes and loads, dumps
    // to guest memory, and the DE waits that ran the CE up to its next counter.
    unsigned ceWrites = 0, ceLoads = 0, ceDumps = 0, ceWaits = 0, ceIgnored = 0;
    unsigned memoryWrites = 0; // WRITE_DATA to memory applied in stream order
    unsigned indirectCalls = 0; // indirect draws and dispatches run from their memory arguments
    std::string error; // the first failure
    // Diagnostics: the registers of the last draw skipped for want of a colour
    // target, and the last dispatch (program address and group counts).
    std::string lastSkipped, lastDispatch;
    // Diagnostics: draws with a depth test, those writing depth, those with
    // no colour, culled faces, the last depth state.
    unsigned depthTested = 0, depthWritten = 0, depthOnly = 0, cullFront = 0, cullBack = 0, stencilTested = 0;
    // Colour targets 1-7 drawn with target 0, and those left out (another size or format).
    unsigned extraTargets = 0, extraTargetsDropped = 0;
    std::string lastDepth;
    std::string depthReport() const {
        char text[200];
        std::snprintf(text, sizeof text, "\nProfondeur : %u dessins testés, %u écrivent, %u sans couleur, %u avec stencil · culling : %u avant, %u arrière",
                      depthTested, depthWritten, depthOnly, stencilTested, cullFront, cullBack);
        std::string more;
        if (extraTargets || extraTargetsDropped) {
            char line[96];
            std::snprintf(line, sizeof line, " · MRT : %u cibles en plus, %u écartées", extraTargets, extraTargetsDropped);
            more = line;
        }
        if (fastClears || dmaCopies || lastDmaControl || lastDmaCommand) {
            char line[256];
            std::snprintf(line, sizeof line, " · effacements rapides : %u · copies DMA : %u (dernier DMA_DATA : contrôle 0x%x, commande 0x%x) · appels de tampons : %u · dessins instanciés : %u",
                          fastClears, dmaCopies, lastDmaControl, lastDmaCommand, indirectBuffers, instancedDraws);
            more += line;
        }
        if (memoryWrites || indirectCalls) {
            char line[96];
            std::snprintf(line, sizeof line, " · écritures WRITE_DATA : %u · appels indirects : %u", memoryWrites, indirectCalls);
            more += line;
        }
        if (ceWrites || ceLoads || ceDumps || ceWaits || ceIgnored) {
            char line[192];
            std::snprintf(line, sizeof line, " · moteur de constantes : %u écritures, %u chargements, %u copies en mémoire, %u attentes, %u paquets ignorés",
                          ceWrites, ceLoads, ceDumps, ceWaits, ceIgnored);
            more += line;
        }
        return text + more + (lastDepth.empty() ? std::string() : " · dernier : " + lastDepth);
    }
    // error: the first failure; last: the latest (the host skips failed draws).
    std::string last;
    void fail(const std::string& message) { ++failed; if (error.empty()) error = message; last = message; }
    // Packets whose effect is not reproduced, by opcode: their draws, dispatches,
    // commands or copies are lost. Without this count a game using them only
    // shows a black screen and an empty report.
    std::array<unsigned, 256> ignored{};
    static const char* ignoredName(unsigned opcode) {
        switch (opcode) {
        case 0x16: return "DISPATCH_INDIRECT";
        case 0x24: return "DRAW_INDIRECT";
        case 0x25: return "DRAW_INDEX_INDIRECT";
        case 0x2c: return "DRAW_INDIRECT_MULTI";
        case 0x2e: return "DRAW_INDEX_IMMD";
        case 0x2f: return "NUM_INSTANCES > 1";
        case 0x30: return "DRAW_INDEX_MULTI_AUTO";
        case 0x38: return "DRAW_INDEX_INDIRECT_MULTI";
        case 0x3f: return "INDIRECT_BUFFER";
        case 0x41: return "CP_DMA";
        case 0x50: return "DMA_DATA";
        default: return nullptr;
        }
    }
    // "\nPaquets non gérés : DRAW_INDIRECT ×2, ..." or nothing when none was seen.
    std::string ignoredReport() const {
        std::string text;
        for (unsigned op = 0; op < ignored.size(); ++op) {
            if (!ignored[op]) continue;
            char item[64];
            std::snprintf(item, sizeof item, "%s%s ×%u", text.empty() ? "" : ", ", ignoredName(op), ignored[op]);
            text += item;
        }
        return text.empty() ? text : "\nPaquets non gérés : " + text;
    }
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
    std::uint64_t target = 0;                         // CB_COLOR0 base, linear
    // CB_COLOR0_INFO FORMAT (0xa 8_8_8_8, 0xc 16_16_16_16...) and NUMBER_TYPE
    // (0 UNORM, 1 SNORM, 6 SRGB, 7 FLOAT): 8_8_8_8 UNORM unless
    // otherTargetFormats lets others through.
    unsigned format = 0xa, number = 0;
    unsigned pitch = 0, height = 0;                   // pixels
    unsigned swap = 0, mask = 0;                      // CB COMP_SWAP, CB_TARGET_MASK RGBA
    std::uint32_t blend = 0;                          // CB_BLEND0_CONTROL
    // Colour targets 1-7 (MRT), drawn with target 0 when CB_TARGET_MASK enables
    // them and they share its size: more[k - 1] for slot k, base 0 when unused.
    struct ColourTarget { std::uint64_t base = 0; unsigned format = 0xa, number = 0, swap = 0, mask = 0; std::uint32_t blend = 0; };
    ColourTarget more[7];
    // A fast clear of target 0 is due before this draw: the game eliminated
    // one on this target (CB_COLOR_CONTROL MODE 2) since its last draw into
    // it. clearWords: CB_COLOR0_CLEAR_WORD0/1, a pixel in the target's format.
    bool fastClear = false;
    std::uint32_t clearWords[2] = {};
    std::uint32_t instances = 1; // NUM_INSTANCES
    std::uint32_t stepRate[2] = {1, 1}; // VGT_INSTANCE_STEP_RATE_0/1 (v1, v2 of the vertex program)
    float blendConstant[4] = {};                     // CB_BLEND_RED/GREEN/BLUE/ALPHA
    // False: a depth-only draw (no colour target, or mask 0); target is then 0
    // and pitch/height are the depth buffer's.
    bool colour = true;
    // Depth buffer (DB_Z_INFO, DB_Z_WRITE_BASE, DB_DEPTH_SIZE): 0 when the draw
    // neither tests nor clears depth. Kept by the renderer as a float texture;
    // guest memory never receives it, stencil is not emulated.
    std::uint64_t depthTarget = 0;
    unsigned depthWidth = 0, depthHeight = 0;         // pixels (8-pixel tiles)
    bool depthTest = false, depthWrite = false;       // DB_DEPTH_CONTROL Z_ENABLE, Z_WRITE_ENABLE
    unsigned depthFunc = 7;                           // ZFUNC: never, <, =, <=, >, !=, >=, always
    // DB_RENDER_CONTROL DEPTH_CLEAR_ENABLE: the draw's pixels get DB_DEPTH_CLEAR.
    bool depthClear = false;
    float depthClearValue = 1;
    // Stencil (DB_DEPTH_CONTROL STENCIL_ENABLE, DB_STENCIL_INFO, DB_STENCIL_CONTROL,
    // DB_STENCILREFMASK(_BF)): kept with the depth buffer, in the same texture.
    // Faces [0] front, [1] back (the front's unless BACKFACE_ENABLE). Functions
    // use ZFUNC's codes; operations are GCN's STENCILFAIL/ZPASS/ZFAIL codes.
    struct StencilFace { unsigned func = 7, fail = 0, zpass = 0, zfail = 0, ref = 0, mask = 0xff, writeMask = 0xff, opValue = 0; };
    bool stencilTest = false;
    StencilFace stencil[2];
    // DB_RENDER_CONTROL STENCIL_CLEAR_ENABLE: the draw's pixels get DB_STENCIL_CLEAR.
    bool stencilClear = false;
    unsigned stencilClearValue = 0;
    unsigned cull = 0;                                // PA_SU_SC_MODE_CNTL: bit 0 front, bit 1 back
    bool frontClockwise = false;                      // FACE: front faces are clockwise on screen
};

// A compute dispatch (DISPATCH_DIRECT): program, user SGPRs, COMPUTE_PGM_RSRC2
// (user SGPR count, thread group id and thread id enables), threads per
// group and group counts.
struct PM4Dispatch {
    std::uint64_t program = 0;
    std::uint32_t user[16] = {};
    unsigned userCount = 0;
    std::uint32_t rsrc2 = 0;
    std::uint32_t threads[3] = {}, groups[3] = {};
};

class PM4DrawCollector {
public:
    PM4DrawCollector(const GCNMemory& memory, PM4DrawStats& stats) : memory_(memory), stats_(stats) {}
    // Executes one submission and calls draw(const PM4Draw&) for each colour
    // draw; register state persists across submissions.
    template <class Draw> void execute(const std::uint32_t* words, std::size_t count, Draw&& draw) {
        execute(words, count, draw, [](const PM4Dispatch&) {});
    }
    // The same, with dispatch(const PM4Dispatch&) for each compute dispatch,
    // in submission order among the draws.
    template <class Draw, class Dispatch>
    void execute(const std::uint32_t* words, std::size_t count, Draw&& draw, Dispatch&& dispatch) {
        const auto parsed = decodePM4Type3(words, count);
        if (!parsed) { stats_.fail("unframed PM4 stream"); return; }
        // The submission's constant engine stream comes first, in NOPs marked
        // ceMarker (PS4GnmQueue). Without a DE wait on it, it runs up front
        // (the CE runs ahead); otherwise each wait runs it to its next counter.
        const bool top = depth_ == 0;
        if (top) {
            ce_.clear(); cePos_ = 0;
            bool waits = false;
            for (const auto& packet : parsed.packets) {
                const std::uint32_t* data = words + packet.payloadWord;
                if (packet.opcode == 0x10 && packet.payloadCount >= 1 && data[0] == ceMarker)
                    ce_.insert(ce_.end(), data + 1, data + packet.payloadCount);
                waits = waits || packet.opcode == 0x86;
            }
            if (!ce_.empty() && !waits) runConstantEngine(false);
        }
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
            case 0x15:                                                  // DISPATCH_DIRECT
                runDispatch(n >= 1 ? data[0] : 0, n >= 2 ? data[1] : 0, n >= 3 ? data[2] : 0, n >= 3, dispatch);
                break;
            // SET_BASE: base 1 holds the arguments of indirect draws and dispatches.
            case 0x11: if (n >= 3 && (data[0] & 0xf) == 1) indirectBase_ = (std::uint64_t(data[1]) | std::uint64_t(data[2] & 0xffff) << 32) & ~7ull; break;
            case 0x16: {                                                // DISPATCH_INDIRECT: x, y, z groups
                std::uint32_t groups[3];
                if (n < 2 || !indirectBase_ || !memory_.read(indirectBase_ + data[0], groups, sizeof groups)) { ++stats_.ignored[0x16]; break; }
                ++stats_.indirectCalls;
                runDispatch(groups[0], groups[1], groups[2], true, dispatch);
                break;
            }
            case 0x24: case 0x25: {
                // DRAW_INDIRECT (vertices, instances, first vertex, first instance)
                // and DRAW_INDEX_INDIRECT (indices, instances, first index, base
                // vertex, first instance): the counts from memory; the base
                // vertex and first instance go to the user SGPRs named by
                // BASE_VTX_LOC and START_INST_LOC, as the CP writes them.
                const bool indexed = packet.opcode == 0x25;
                std::uint32_t args[5] = {};
                if (n < 3 || !indirectBase_ || !memory_.read(indirectBase_ + data[0], args, indexed ? 20 : 16)) {
                    ++stats_.ignored[packet.opcode]; break;
                }
                ++stats_.indirectCalls;
                if (!args[0] || !args[1]) break;
                const std::uint32_t base = indexed ? args[3] : args[2], first = indexed ? args[4] : args[3];
                // Both offsets count from the SH register base (0x2c00).
                if ((data[1] & 0xffff) && (data[1] & 0xffff) < shader_.size()) shader_[data[1] & 0xffff] = base;
                if ((data[2] & 0xffff) && (data[2] & 0xffff) < shader_.size()) shader_[data[2] & 0xffff] = first;
                const std::uint32_t saved = instances_;
                instances_ = std::max(1u, std::min(args[1], 1u << 16));
                if (indexed) collect(args[0], true, indexBase_ + std::uint64_t(args[2]) * indexBytes_, draw);
                else collect(args[0], false, 0, draw);
                instances_ = saved;
                break;
            }
            // NUM_INSTANCES: the draws after it run that many instances (the
            // vertex program reads the instance in v3), until the next one.
            case 0x2f: if (n >= 1) instances_ = std::max(1u, std::min(data[0], 1u << 16)); break;
            case 0x50: if (!dmaData(data, n)) ++stats_.ignored[0x50]; break;
            case 0x86: if (!ce_.empty()) { ++stats_.ceWaits; runConstantEngine(true); } break; // WAIT_ON_CE_COUNTER
            case 0x37: writeData(data, n); break;
            case 0x41: {
                // CP_DMA: DMA_DATA's older form, the selects beside the high
                // address halves: rewritten as a DMA_DATA packet.
                if (n < 5) { ++stats_.ignored[0x41]; break; }
                const std::uint32_t as[6] = {(data[1] & (3u << 29)) | (data[3] & (3u << 20)), data[0], data[1] & 0xffff,
                                             data[2], data[3] & 0xffff, data[4]};
                if (!dmaData(as, 6)) ++stats_.ignored[0x41];
                break;
            }
            case 0x3f: {
                // INDIRECT_BUFFER: a call into another command buffer (IB_BASE,
                // IB_SIZE in dwords), executed here in order with the same state.
                // A buffer that cannot be read or framed stays counted as lost.
                if (n < 3 || depth_ >= 4) { ++stats_.ignored[0x3f]; break; }
                const std::uint64_t ib = (std::uint64_t(data[0]) & ~3ull) | std::uint64_t(data[1] & 0xffff) << 32;
                const std::uint32_t size = data[2] & 0xfffff;
                std::vector<std::uint32_t> nested(size);
                if (!size || !memory_.read(ib, nested.data(), std::size_t(size) * 4) || !decodePM4Type3(nested.data(), size)) {
                    ++stats_.ignored[0x3f]; break;
                }
                ++depth_; ++stats_.indirectBuffers;
                execute(nested.data(), size, draw, dispatch);
                --depth_;
                break;
            }
            default:
                // Synchronization and prefetch packets have nothing to draw. The
                // others named by ignoredName are lost: count them for the report.
                if (PM4DrawStats::ignoredName(packet.opcode)) ++stats_.ignored[packet.opcode];
                break;
            }
        }
        if (top && !ce_.empty()) { runConstantEngine(false); ce_.clear(); }
    }
    // First word of the NOPs carrying a submission's constant engine stream.
    static constexpr std::uint32_t ceMarker = 0x4543584d; // "MXCE"
    // Registers by offset from their bank base (context 0xa000, shader 0x2c00, uconfig 0xc000).
    std::uint32_t context(unsigned offset) const { return context_[offset & 0x3ff]; }
    // Also report draws that write no colour but write or clear a depth
    // buffer (depth pre-passes, shadow maps, depth clears), colour false.
    bool depthOnlyDraws = false;
    // Also report colour targets in formats other than 8_8_8_8 UNORM (the
    // Metal renderer decides which it can draw; the software one cannot).
    bool otherTargetFormats = false;

private:
    template <class Dispatch> void runDispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z, bool complete, Dispatch& dispatch) {
        ++stats_.dispatches;
        // COMPUTE_PGM_LO/HI (0x2e0c), COMPUTE_NUM_THREAD_X/Y/Z (0x2e07).
        const std::uint64_t program = (std::uint64_t(shader_[0x20c]) | std::uint64_t(shader_[0x20d] & 0xff) << 32) << 8;
        char text[160];
        std::snprintf(text, sizeof text, "programme 0x%llx, groupes %ux%ux%u de %ux%ux%u",
                      (unsigned long long)program, x, y, z,
                      shader_[0x207] & 0xffff, shader_[0x208] & 0xffff, shader_[0x209] & 0xffff);
        stats_.lastDispatch = text;
        if (!complete) return;
        PM4Dispatch c;
        c.program = program;
        c.rsrc2 = shader_[0x213];                                // COMPUTE_PGM_RSRC2
        c.userCount = std::min(16u, unsigned((c.rsrc2 >> 1) & 31));
        for (unsigned k = 0; k < 16; ++k) c.user[k] = shader_[0x240 + k]; // COMPUTE_USER_DATA_0..15
        const std::uint32_t groups[3] = {x, y, z};
        for (unsigned k = 0; k < 3; ++k) { c.threads[k] = shader_[0x207 + k] & 0xffff; c.groups[k] = groups[k]; }
        dispatch(c);
    }
    std::uint64_t indirectBase_ = 0; // SET_BASE 1: indirect draw and dispatch arguments
    // The constant engine: CE RAM (48 KiB, kept across submissions like the
    // GPU's) filled by WRITE/LOAD_CONST_RAM and copied to guest memory by
    // DUMP_CONST_RAM. Gnmx writes the shaders' resource and constant tables
    // this way: without it they stayed zero (CoD's post-processing wrote NaN).
    // Runs ce_ from cePos_, through its next INCREMENT_CE_COUNTER when toCounter.
    void runConstantEngine(bool toCounter) {
        while (cePos_ < ce_.size()) {
            const std::uint32_t header = ce_[cePos_];
            if ((header >> 30) == 2) { ++cePos_; continue; } // type-2 filler
            const std::size_t n = ((header >> 16) & 0x3fff) + 1;
            if ((header >> 30) != 3 || n > ce_.size() - cePos_ - 1) { cePos_ = ce_.size(); ++stats_.ceIgnored; return; }
            const std::uint32_t* data = ce_.data() + cePos_ + 1;
            const unsigned opcode = (header >> 8) & 0xff;
            cePos_ += 1 + n;
            if (constantPacket(opcode, data, n, 0)) continue;
            if (opcode == 0x84 && toCounter) return; // INCREMENT_CE_COUNTER
        }
    }
    // One CE packet; false for the counter and others with nothing to do here.
    bool constantPacket(unsigned opcode, const std::uint32_t* data, std::size_t n, unsigned depth) {
        switch (opcode) {
        case 0x81: { // WRITE_CONST_RAM: byte offset, then the dwords
            if (n < 2) break;
            const std::size_t offset = data[0] & 0xfffc;
            for (std::size_t k = 1; k < n && offset + 4 * k <= ceRam_.size(); ++k)
                std::memcpy(ceRam_.data() + offset + 4 * (k - 1), &data[k], 4);
            ++stats_.ceWrites;
            return true;
        }
        case 0x80: { // LOAD_CONST_RAM: address, dwords, CE RAM byte offset
            if (n < 4) break;
            const std::uint64_t address = std::uint64_t(data[0]) | std::uint64_t(data[1]) << 32;
            const std::size_t offset = data[3] & 0xfffc, bytes = std::size_t(data[2] & 0x7fff) * 4;
            if (offset + bytes <= ceRam_.size() && memory_.read(address, ceRam_.data() + offset, bytes)) ++stats_.ceLoads;
            else ++stats_.ceIgnored;
            return true;
        }
        case 0x83: { // DUMP_CONST_RAM: CE RAM byte offset, dwords, address
            if (n < 4 || !memory_.write) break;
            const std::size_t offset = data[0] & 0xfffc, bytes = std::size_t(data[1] & 0x7fff) * 4;
            const std::uint64_t address = std::uint64_t(data[2]) | std::uint64_t(data[3]) << 32;
            if (offset + bytes <= ceRam_.size() && memory_.write(address, ceRam_.data() + offset, bytes)) ++stats_.ceDumps;
            else ++stats_.ceIgnored;
            return true;
        }
        case 0x33: { // INDIRECT_BUFFER_CONST: a CE command buffer, run whole
            if (n < 3 || depth >= 4) break;
            const std::uint64_t ib = (std::uint64_t(data[0]) & ~3ull) | std::uint64_t(data[1] & 0xffff) << 32;
            const std::uint32_t size = data[2] & 0xfffff;
            std::vector<std::uint32_t> nested(size);
            if (!size || !memory_.read(ib, nested.data(), std::size_t(size) * 4)) break;
            for (std::size_t at = 0; at < size;) {
                const std::uint32_t header = nested[at];
                if ((header >> 30) == 2) { ++at; continue; }
                const std::size_t count = ((header >> 16) & 0x3fff) + 1;
                if ((header >> 30) != 3 || count > size - at - 1) break;
                constantPacket((header >> 8) & 0xff, nested.data() + at + 1, count, depth + 1);
                at += 1 + count;
            }
            return true;
        }
        default: return false;
        }
        ++stats_.ceIgnored;
        return true;
    }
    std::vector<std::uint32_t> ce_;
    std::size_t cePos_ = 0;
    std::array<std::uint8_t, 0xc000> ceRam_{};
    // WRITE_DATA to memory (DST_SEL 1, 2 or 5), where it is in the stream: the
    // draws after it read what it wrote (the queue retires it again after the
    // submission, for its labels). WR_ONE_ADDR writes every dword to one address.
    void writeData(const std::uint32_t* data, std::size_t n) {
        if (n < 4 || !memory_.write) return;
        const unsigned dst = (data[0] >> 8) & 0xf;
        if (dst != 1 && dst != 2 && dst != 5) return;
        const std::uint64_t address = std::uint64_t(data[1]) | std::uint64_t(data[2]) << 32;
        if ((data[0] >> 16) & 1) { if (memory_.write(address, &data[n - 1], 4)) ++stats_.memoryWrites; return; }
        if (memory_.write(address, data + 3, 4 * (n - 3))) ++stats_.memoryWrites;
    }
    // DMA_DATA (CP DMA): a copy or fill of guest memory in submission order,
    // so the draws after it see it (CoD updates its constant buffers this way:
    // without it a post-processing pass read zeros and wrote NaN). Memory to
    // memory only; GDS, registers and swaps stay ignored (returns false).
    bool dmaData(const std::uint32_t* data, std::size_t n) {
        if (n < 6 || !memory_.write) return false;
        const std::uint32_t control = data[0], command = data[5];
        stats_.lastDmaControl = control; stats_.lastDmaCommand = command;
        // SRC_SEL 0/3 and DST_SEL 0/3: memory (3 through the L2, as Gnm does).
        unsigned srcSel = (control >> 29) & 3, dstSel = (control >> 20) & 3;
        if (srcSel == 3) srcSel = 0;
        if (dstSel == 3) dstSel = 0;
        const bool srcRegister = (command >> 26) & 1, dstRegister = (command >> 27) & 1;
        const bool srcFixed = (command >> 28) & 1, dstFixed = (command >> 29) & 1;
        const std::uint32_t bytes = command & 0x1fffff;
        if (dstSel != 0 || dstRegister || dstFixed || ((command >> 22) & 15) || (srcSel != 0 && srcSel != 2)) return false;
        const std::uint64_t dst = std::uint64_t(data[3]) | std::uint64_t(data[4]) << 32;
        if (!bytes) return true;
        std::vector<std::uint8_t> buffer(bytes);
        if (srcSel == 2) { // DATA: fill with the dword in SRC_ADDR_LO
            for (std::uint32_t k = 0; k < bytes; ++k) buffer[k] = std::uint8_t(data[1] >> (8 * (k & 3)));
        } else {
            if (srcRegister) return false;
            const std::uint64_t src = std::uint64_t(data[1]) | std::uint64_t(data[2]) << 32;
            if (srcFixed) {
                std::uint32_t word = 0;
                if (!memory_.read(src, &word, 4)) return false;
                for (std::uint32_t k = 0; k < bytes; ++k) buffer[k] = std::uint8_t(word >> (8 * (k & 3)));
            } else if (!memory_.read(src, buffer.data(), bytes)) return false;
        }
        if (!memory_.write(dst, buffer.data(), bytes)) return false;
        ++stats_.dmaCopies;
        return true;
    }
    const GCNMemory& memory_;
    PM4DrawStats& stats_;
    std::array<std::uint32_t, 0x400> context_{}, shader_{}, uconfig_{};
    unsigned indexBytes_ = 2;
    std::uint64_t indexBase_ = 0; // Persists across submissions, like INDEX_TYPE.
    std::map<std::uint64_t, std::array<std::uint32_t, 2>> pendingClears_; // by target, see PM4Draw::fastClear
    unsigned depth_ = 0; // INDIRECT_BUFFER nesting
    std::uint32_t instances_ = 1; // NUM_INSTANCES, persists like the hardware's

    static void setRegisters(std::array<std::uint32_t, 0x400>& bank, const std::uint32_t* data, std::size_t n) {
        for (std::size_t k = 1; k < n; ++k) if (data[0] + k - 1 < bank.size()) bank[data[0] + k - 1] = data[k];
    }
    static float f(std::uint32_t x) { return gcn_detail::f32(x); }
    // DB_DEPTH_CONTROL, DB_Z_INFO, DB_Z_READ/WRITE_BASE, DB_DEPTH_SIZE (8-pixel
    // tiles), DB_RENDER_CONTROL, DB_DEPTH_CLEAR, DB_HTILE_DATA_BASE, PA_SU_SC_MODE_CNTL.
    void noteDepth() {
        const std::uint32_t control = context_[0x200], mode = context_[0x205];
        if (mode & 1) ++stats_.cullFront;
        if (mode & 2) ++stats_.cullBack;
        if ((control & 1) && (context_[0x11] & 1)) ++stats_.stencilTested;
        if (!(control & 2) || !(context_[0x10] & 3)) return;
        const bool writes = control & 4;
        ++stats_.depthTested;
        if (writes) ++stats_.depthWritten;
        if (!(context_[0x8e] & 0xf) || !context_[0x318]) ++stats_.depthOnly;
        char text[192];
        std::snprintf(text, sizeof text, "format %u, lecture 0x%llx, écriture 0x%llx, %ux%u, fonction %u%s, contrôle 0x%x, rendu 0x%x, effacement %g, HTILE 0x%llx, faces 0x%x",
                      context_[0x10] & 3, (unsigned long long)(std::uint64_t(context_[0x12]) << 8),
                      (unsigned long long)(std::uint64_t(context_[0x14]) << 8),
                      ((context_[0x16] & 0x7ff) + 1) * 8, (((context_[0x16] >> 11) & 0x7ff) + 1) * 8,
                      (control >> 4) & 7, writes ? " avec écriture" : "", control, context_[0x00], f(context_[0x0b]),
                      (unsigned long long)(std::uint64_t(context_[0x05]) << 8), mode);
        stats_.lastDepth = text;
    }
    // The depth and culling state of the next draw.
    void depthState(PM4Draw& d) const {
        const std::uint32_t control = context_[0x200], mode = context_[0x205];
        d.cull = mode & 3;
        d.frontClockwise = (mode >> 2) & 1;
        d.depthTest = control & 2;
        d.depthWrite = d.depthTest && (control & 4);
        d.depthFunc = (control >> 4) & 7;
        d.depthClear = context_[0x00] & 1;
        d.depthClearValue = std::clamp(f(context_[0x0b]), 0.0f, 1.0f);
        // DB_STENCIL_INFO FORMAT 1: an 8-bit stencil buffer beside the depth one.
        const bool stencilBuffer = context_[0x11] & 1, depthBuffer = context_[0x10] & 3;
        if (!depthBuffer) d.depthTest = d.depthWrite = d.depthClear = false;
        if (stencilBuffer) {
            d.stencilTest = control & 1;
            d.stencilClear = context_[0x00] & 2;
            d.stencilClearValue = context_[0x0a] & 0xff;
            const std::uint32_t ops = context_[0x10b], refs[2] = {context_[0x10c], context_[0x10d]};
            const bool back = control & 0x80;
            for (unsigned k = 0; k < 2; ++k) {
                const unsigned face = back ? k : 0;
                auto& s = d.stencil[k];
                s.func = (control >> (face ? 20 : 8)) & 7;
                s.fail = (ops >> (face ? 12 : 0)) & 15; s.zpass = (ops >> (face ? 16 : 4)) & 15; s.zfail = (ops >> (face ? 20 : 8)) & 15;
                s.ref = refs[face] & 0xff; s.mask = (refs[face] >> 8) & 0xff;
                s.writeMask = (refs[face] >> 16) & 0xff; s.opValue = refs[face] >> 24;
            }
        }
        // The depth texture's address: the depth buffer's, else the stencil's.
        const std::uint64_t zBase = std::uint64_t(context_[0x14] ? context_[0x14] : context_[0x12]) << 8;
        const std::uint64_t sBase = std::uint64_t(context_[0x15] ? context_[0x15] : context_[0x13]) << 8;
        const std::uint64_t base = depthBuffer && zBase ? zBase : stencilBuffer ? sBase : 0;
        if (!base || !(d.depthTest || d.depthClear || d.stencilTest || d.stencilClear)) {
            d.depthTest = d.depthWrite = d.depthClear = d.stencilTest = d.stencilClear = false;
            return;
        }
        d.depthTarget = base;
        d.depthWidth = ((context_[0x16] & 0x7ff) + 1) * 8;
        d.depthHeight = (((context_[0x16] >> 11) & 0x7ff) + 1) * 8;
    }
    template <class Draw> void collect(std::uint32_t count, bool indexed, std::uint64_t indexAddress, Draw& draw) {
        ++stats_.draws;
        noteDepth();
        PM4Draw d;
        depthState(d);
        d.colour = (context_[0x8e] & 0xf) && context_[0x318];
        // A stencil write without colour (a mask, a shadow volume) is a depth-only draw too.
        const bool stencilWrites = d.stencilClear || (d.stencilTest && (d.stencil[0].writeMask || d.stencil[1].writeMask) &&
            (d.stencil[0].fail | d.stencil[0].zpass | d.stencil[0].zfail | d.stencil[1].fail | d.stencil[1].zpass | d.stencil[1].zfail));
        // ELIMINATE_FAST_CLEAR: the GPU writes the clear colour into the tiles
        // a fast clear left untouched. Targets live in Metal textures with no
        // CMASK: the clear is applied whole, before the target's next draw
        // (the next frame's), instead of leaving its first contents (CoD's
        // HDR scene stayed NaN, and its bloom and tone mapping black).
        if (((context_[0x202] >> 4) & 7) == 2 && context_[0x318]) {
            ++stats_.skipped;
            ++stats_.fastClears;
            pendingClears_[std::uint64_t(context_[0x318]) << 8] = {context_[0x323], context_[0x324]};
            return;
        }
        if (!d.colour && !(depthOnlyDraws && (d.depthWrite || d.depthClear || stencilWrites))) {
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
        d.format = (info >> 2) & 0x1f;
        d.number = (info >> 8) & 7;
        if (d.colour && !otherTargetFormats && (d.format != 0xa || d.number != 0)) { stats_.fail("colour target format"); return; }
        d.primitive = uconfig_[0x242];
        d.instances = instances_;
        d.stepRate[0] = std::max(1u, context_[0x2a8]); d.stepRate[1] = std::max(1u, context_[0x2a9]);
        if (instances_ > 1) ++stats_.instancedDraws;
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
            if (!memory_.read(indexAddress, bytes.data(), bytes.size())) {
                char where[96];
                std::snprintf(where, sizeof where, " 0x%llx (%u index de %u octets)", (unsigned long long)indexAddress, count, indexBytes_);
                stats_.fail(std::string("unreadable index buffer") + where); return;
            }
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
        if (d.colour) {
            // CB_COLOR0_PITCH in 8-pixel units, CB_COLOR0_SLICE in 64-pixel units.
            d.target = std::uint64_t(context_[0x318]) << 8;
            if (const auto due = pendingClears_.find(d.target); due != pendingClears_.end()) {
                d.fastClear = true;
                d.clearWords[0] = due->second[0]; d.clearWords[1] = due->second[1];
                pendingClears_.erase(due);
            }
            d.pitch = ((context_[0x319] & 0x7ff) + 1) * 8;
            d.height = unsigned((std::uint64_t((context_[0x31a] & 0x3fffff) + 1) * 64) / d.pitch);
            d.swap = (info >> 11) & 3;
            d.mask = context_[0x8e] & 0xf;
            d.blend = context_[0x1e0];
            for (unsigned k = 1; k < 8; ++k) {
                const unsigned mask = (context_[0x8e] >> (4 * k)) & 0xf, at = 0x318 + 15 * k;
                if (!mask || !context_[at]) continue;
                const std::uint32_t moreInfo = context_[at + 4];
                const unsigned pitch = ((context_[at + 1] & 0x7ff) + 1) * 8;
                const unsigned height = unsigned((std::uint64_t((context_[at + 2] & 0x3fffff) + 1) * 64) / pitch);
                PM4Draw::ColourTarget t;
                t.base = std::uint64_t(context_[at]) << 8;
                t.format = (moreInfo >> 2) & 0x1f; t.number = (moreInfo >> 8) & 7; t.swap = (moreInfo >> 11) & 3;
                t.mask = mask; t.blend = context_[0x1e0 + k];
                // Metal draws its attachments at one size.
                if (pitch != d.pitch || height != d.height || t.base == d.target ||
                    (!otherTargetFormats && (t.format != 0xa || t.number != 0))) { ++stats_.extraTargetsDropped; continue; }
                d.more[k - 1] = t;
                ++stats_.extraTargets;
            }
        } else {
            d.pitch = d.depthWidth;
            d.height = d.depthHeight;
        }
        for (unsigned k = 0; k < 4; ++k) d.blendConstant[k] = f(context_[0x105 + k]);
        d.scissor[0] = std::min(context_[0x0c] & 0xffff, d.pitch);
        d.scissor[1] = std::min(context_[0x0c] >> 16, d.height);
        d.scissor[2] = std::min(context_[0x0d] & 0xffff, d.pitch);
        d.scissor[3] = std::min(context_[0x0d] >> 16, d.height);
        // The hardware scissor is the screen scissor met with the window,
        // generic and (when PA_SC_MODE_CNTL_0 enables it) viewport 0
        // scissors: CoD clips its menu highlight and scrolling text with the
        // generic scissor. A pair never written (BR 0) restricts nothing.
        auto meet = [&](std::uint32_t tl, std::uint32_t br, bool offset) {
            if (!br) return;
            std::int32_t dx = 0, dy = 0;
            if (offset && !(tl >> 31)) {
                dx = std::int16_t(context_[0x80] & 0xffff);
                dy = std::int16_t(context_[0x80] >> 16);
            }
            auto clampTo = [](std::int32_t v, unsigned limit) { return unsigned(std::clamp<std::int32_t>(v, 0, std::int32_t(limit))); };
            d.scissor[0] = std::max(d.scissor[0], clampTo(std::int32_t(tl & 0x7fff) + dx, d.pitch));
            d.scissor[1] = std::max(d.scissor[1], clampTo(std::int32_t((tl >> 16) & 0x7fff) + dy, d.height));
            d.scissor[2] = std::min(d.scissor[2], clampTo(std::int32_t(br & 0x7fff) + dx, d.pitch));
            d.scissor[3] = std::min(d.scissor[3], clampTo(std::int32_t((br >> 16) & 0x7fff) + dy, d.height));
        };
        meet(context_[0x81], context_[0x82], true);  // PA_SC_WINDOW_SCISSOR_TL/BR
        meet(context_[0x90], context_[0x91], true);  // PA_SC_GENERIC_SCISSOR_TL/BR
        if (context_[0x292] & 2) meet(context_[0x94], context_[0x95], false); // PA_SC_VPORT_SCISSOR_0
        d.scissor[2] = std::max(d.scissor[2], d.scissor[0]);
        d.scissor[3] = std::max(d.scissor[3], d.scissor[1]);
        draw(static_cast<const PM4Draw&>(d));
    }
};
} // namespace maxps4::gpu
