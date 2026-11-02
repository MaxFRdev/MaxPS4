#pragma once
// Executes PM4Draws (PM4DrawCollector.hpp) on a Metal device with the
// game's own programs translated by GCNTranslator.hpp. Objective-C++ only.
// Guest memory reaches the GPU as one MTLBuffer window, or, without one, as
// per-draw copies of the ranges PM4ResourceScan finds; code is read through
// a GCNMemory when a pipeline is built. Each colour target address owns a
// Metal texture (BGRA8 or RGBA8 per COMP_SWAP), created cleared and kept
// across draws. Pipelines are cached by program addresses, the fetch-shader
// SGPR pair (s[0:1]), PS inputs, format and blend. Limits as the software
// renderer: colour target 0 only, no depth, instance 0.
// Compute dispatches run as Metal kernels on copies of the ranges one
// interpreted lane addresses; colour targets inside those ranges are copied
// in from their textures, and back after the kernel. A linear 8_8_8_8 image
// the kernel stores becomes a colour target texture, so draws (and the
// display) see it. Guest memory itself never receives what kernels store.
// Prototype, off by default: `nativeTextures` reads one linear 8_8_8_8 or 5_6_5 T# per
// draw through a Metal texture viewing the draw's memory buffer (no copy).
// Filtering stays the GCN arithmetic; any other T#, or a view Metal cannot
// make (alignment, partial range), keeps the buffer path.
#ifdef __OBJC__
#import <Metal/Metal.h>
#include "PM4DrawCollector.hpp"
#include "PM4ResourceScan.hpp"
#include <map>
#include <string>
#include <tuple>

namespace maxps4::gpu {
class PM4MetalRenderer {
public:
    // `guest` nil: each draw copies the memory ranges its programs read.
    PM4MetalRenderer(id<MTLDevice> device, const GCNMemory& code, id<MTLBuffer> guest, std::uint64_t guestBase,
                     bool nativeTextures = false)
        : device_(device), code_(code), guest_(guest), guestBase_(guestBase),
          scanMemory_{code.read, [](std::uint64_t, const void*, std::size_t) { return true; }},
          scanner_(scanMemory_, nativeTextures), native_(nativeTextures) {}
    // Encodes one draw as a render pass into `commands`. False with `error`
    // set (and nothing encoded) when the draw cannot be expressed.
    bool encode(const PM4Draw& draw, id<MTLCommandBuffer> commands);
    // Encodes one compute dispatch into `commands`, false with `error` set
    // (and nothing encoded) when it cannot be expressed.
    bool dispatch(const PM4Dispatch& dispatch, id<MTLCommandBuffer> commands);
    unsigned kernelsBuilt = 0, targetsSynced = 0, dispatchesWithoutEffect = 0;
    // The draws of the current batch, described for the session report, and
    // the disassembly of the first programs drawn (VS and PS, with user SGPRs).
    std::string batchDraws, programListings;
    // Opt-in for a single submission: consecutive draws to the same texture
    // share a render pass. Always end the batch before committing commands.
    void beginBatch() {
        endBatch(); batching_ = true; batchDraws.clear();
        visibility_ = [device_ newBufferWithLength:8 * 64 options:MTLResourceStorageModeShared];
        if (visibility_) std::memset(visibility_.contents, 0, 8 * 64);
        visibilityCount_ = 0;
    }
    // The samples each draw of the last batch that drew rasterized (Metal
    // visibility results, readable once its commands completed).
    id<MTLBuffer> lastVisibility = nil;
    unsigned lastVisibilityCount = 0;
    void endBatch();
    // The texture holding colour target `address`, or nil if never drawn.
    id<MTLTexture> target(std::uint64_t address) const;
    std::size_t targetCount() const { return targets_.size(); }
    // f(address, texture) for every colour target.
    template <class F> void forEachTarget(F&& f) const { for (const auto& [address, t] : targets_) f(address, t.texture); }
    std::string error;
    unsigned pipelinesBuilt = 0;
    unsigned lastRanges = 0; // memory ranges copied for the last draw (copy mode)
    unsigned renderPasses = 0;
    unsigned buffersBuilt = 0, buffersReused = 0;
    unsigned nativeDraws = 0; // draws given a native texture (nativeTextures)
    unsigned targetReads = 0; // of which, a colour target an earlier draw rendered
    // T#s refused: none captured (per draw), format/tiling, outside the copied
    // ranges, alignment, Metal view refused; and the last refused T#.
    unsigned nativeMisses[5] = {};
    std::string nativeLastMiss;

private:
    using PipelineKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::string, unsigned, std::uint32_t, unsigned>;
    struct Target { id<MTLTexture> texture; bool fresh; };
    id<MTLDevice> device_;
    const GCNMemory& code_;
    id<MTLBuffer> guest_;
    std::uint64_t guestBase_;
    GCNMemory scanMemory_; // code_ reads; scanned lanes' stores are dropped
    PM4ResourceScanner scanner_;
    bool native_;
    id<MTLArgumentEncoder> nativeArguments_ = nil;
    std::map<PipelineKey, id<MTLRenderPipelineState>> pipelines_;
    std::map<std::pair<std::uint64_t, std::uint32_t>, id<MTLComputePipelineState>> kernels_;
    id<MTLComputePipelineState> kernel(const PM4Dispatch& dispatch);
    id<MTLLibrary> library(const std::string& source);
    std::map<std::uint64_t, Target> targets_;
    std::vector<std::uint64_t> listed_;
    id<MTLBuffer> visibility_ = nil;
    unsigned visibilityCount_ = 0;
    void listProgram(const char* stage, std::uint64_t address, const std::uint32_t* user, unsigned userCount);
    bool batching_ = false;
    id<MTLRenderCommandEncoder> encoder_ = nil;
    id<MTLCommandBuffer> commands_ = nil;
    id<MTLTexture> attachment_ = nil;
    struct Upload { id<MTLBuffer> buffer; id<MTLCommandBuffer> owner; };
    std::vector<Upload> uploads_;
    std::uint64_t pooledBytes_ = 0;
    id<MTLBuffer> uploadBuffer(std::size_t bytes, id<MTLCommandBuffer> commands);
    // Guest [address, address + size) into `out`; unreadable pages read as zero.
    void copyGuest(std::uint64_t address, std::uint8_t* out, std::uint64_t size) const;
    id<MTLRenderPipelineState> pipeline(const PM4Draw& draw, MTLPixelFormat format);
    // `ranges`: (guest start, length, buffer offset) triples; none for the guest window.
    id<MTLTexture> nativeTexture(const std::array<std::uint32_t, 8>& t, id<MTLBuffer> memory,
                                 const std::uint64_t* ranges, unsigned rangeCount, id<MTLTexture> drawing);
    bool fail(const std::string& message) { error = message; return false; }
};
} // namespace maxps4::gpu
#endif
