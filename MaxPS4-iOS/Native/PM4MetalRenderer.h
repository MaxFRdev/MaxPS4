#pragma once
// Executes PM4Draws (PM4DrawCollector.hpp) on a Metal device with the
// game's own programs translated by GCNTranslator.hpp. Objective-C++ only.
// Guest memory reaches the GPU as one MTLBuffer window, or, without one, as
// per-draw copies of the ranges PM4ResourceScan finds; code is read through
// a GCNMemory when a pipeline is built. Each colour target address owns a
// Metal texture (BGRA8 or RGBA8 per COMP_SWAP), created cleared and kept
// across draws (any format targetFormat() accepts). Pipelines are cached by program addresses, the fetch-shader
// SGPR pair (s[0:1]), PS inputs, formats and blend. Limits as the software
// renderer: colour target 0 only, instance 0. Each depth buffer address owns
// a Depth32Float texture (any DB_Z_INFO format, no stencil), kept across draws
// at the size of the targets it is drawn with; draws test and write it as
// DB_DEPTH_CONTROL says, depth clears write DB_DEPTH_CLEAR, and faces are
// culled as PA_SU_SC_MODE_CNTL says (not rect lists). Depth-only draws
// (colour false) render with no colour attachment. Guest memory never
// receives depth: a game sampling its depth buffer reads stale memory.
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
#include <atomic>
#include <chrono>
#include <functional>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace maxps4::gpu {
class PM4MetalRenderer {
public:
    // `guest` nil: each draw copies the memory ranges its programs read.
    PM4MetalRenderer(id<MTLDevice> device, const GCNMemory& code, id<MTLBuffer> guest, std::uint64_t guestBase,
                     bool nativeTextures = false,
                     std::function<std::int32_t(std::uint64_t, const void*, std::size_t, std::uint64_t*)> writeGuest = {})
        : device_(device), code_(code), guest_(guest), guestBase_(guestBase),
          scanMemory_{code.read, [](std::uint64_t, const void*, std::size_t) { return true; }},
          scanner_(scanMemory_, nativeTextures), native_(nativeTextures), writeGuest_(std::move(writeGuest)) {}
    // Encodes one draw as a render pass into `commands`. False with `error`
    // set (and nothing encoded) when the draw cannot be expressed.
    bool encode(const PM4Draw& draw, id<MTLCommandBuffer> commands);
    // Encodes one compute dispatch into `commands`, false with `error` set
    // (and nothing encoded) when it cannot be expressed.
    bool dispatch(const PM4Dispatch& dispatch, id<MTLCommandBuffer> commands);
    unsigned kernelsBuilt = 0, targetsSynced = 0, dispatchesWithoutEffect = 0;
    unsigned dispatchesEncoded = 0;
    // Compute traffic with guest memory, cumulated: copied in while encoding,
    // written back after the GPU.
    double dispatchCopyMs = 0, writebackMs = 0;
    std::uint64_t dispatchCopyBytes = 0, writebackBytes = 0;
    unsigned fillsDone = 0; // Fill dispatches done without their kernel.
    unsigned fillsRefused = 0; // Recognised fills sent to their kernel, the last with its reason.
    std::string fillRefusal;
    unsigned pipelinesReplaced = 0; // Size builds replaced by their default build.
    unsigned scalarLoadsDirect = 0, scalarLoadsAll = 0; // Pixel programs translated so far.
    bool nativeSamplesOnly = false; // Tests only: no software filtering, see the pipeline source.
    void listCostly(std::uint64_t address); // Adds a program's words to programListings, once.
    const std::string& lastDispatchNote() const { return lastDispatchNote_; }
    bool hasPendingGuestWrites() const { return !pendingGuestWrites_.empty(); }
    bool writebackGuestWrites();
    // The draws of the current batch, described for the session report, and
    // the disassembly of the first programs drawn (VS and PS, with user SGPRs).
    std::string batchDraws, programListings;
    // Opt-in for a single submission: consecutive draws to the same texture
    // share a render pass. Always end the batch before committing commands.
    // `details`: also describe its draws and count their samples, which costs
    // every draw CPU and GPU time.
    void beginBatch(bool details = true) {
        endBatch(); batching_ = true; batchDraws.clear(); ++batchSerial_;
        details_ = details;
        startVisibility();
    }
    // The samples each draw of the last batch that drew rasterized (Metal
    // visibility results, readable once its commands completed).
    id<MTLBuffer> lastVisibility = nil;
    unsigned lastVisibilityCount = 0;
    void endBatch();
    // Start another render segment in the same logical PM4 submission after
    // a synchronous compute writeback barrier. Existing draw diagnostics stay.
    void resumeBatch() {
        batching_ = true;
        startVisibility();
    }
    // The texture holding colour target `address`, or nil if never drawn.
    id<MTLTexture> target(std::uint64_t address) const;
    std::size_t targetCount() const { return targets_.size(); }
    // The depth texture of depth buffer `address`, or nil if never drawn.
    id<MTLTexture> depthTarget(std::uint64_t address) const;
    // Draws given a depth buffer, of which without colour; draws whose faces
    // were all culled (front and back); depth textures resized with a copy.
    unsigned depthDraws = 0, depthOnlyDraws = 0, culledDraws = 0, depthResized = 0;
    unsigned embeddedDepthDraws = 0; // Colourless draws with Gnm's embedded VS: not drawn.
    std::string depthReport() const;
    // The Metal format of a colour target (CB_COLOR0_INFO FORMAT, NUMBER_TYPE,
    // COMP_SWAP), MTLPixelFormatInvalid when unsupported: 8_8_8_8 UNORM/SRGB
    // (swap 0 or 1) or SNORM, 16_16_16_16 and 16_16 and 16 FLOAT/UNORM/SNORM,
    // 32 32_32 32_32_32_32 FLOAT, 2_10_10_10 UNORM, 10_11_11 FLOAT, 8_8 and
    // 8 UNORM/SNORM; others swap 0 only.
    static MTLPixelFormat targetFormat(unsigned format, unsigned number, unsigned swap);
    // Bytes per pixel of a colour target format (0 for others).
    static unsigned bytesPerPixel(MTLPixelFormat format);
    // f(address, texture) for every colour target.
    template <class F> void forEachTarget(F&& f) const { for (const auto& [address, t] : targets_) f(address, t.texture); }
    std::string error;
    unsigned pipelinesBuilt = 0;
    // Read from other threads while encoding: 1 compiling MSL, 2 building a
    // render pipeline, 3 translating GCN programs, 0 none of these.
    std::atomic<int> compiling{0};
    // Build new pipelines on Metal's threads: their draws are skipped (counted
    // in drawsDeferred) until ready, instead of blocking the submission.
    bool asyncPipelines = false;
    unsigned drawsDeferred = 0;
    unsigned pipelinesPending() const { std::lock_guard<std::mutex> lock(async_->mutex); return unsigned(async_->pending.size()); }
    unsigned pipelinesRefused() const { std::lock_guard<std::mutex> lock(async_->mutex); return unsigned(async_->refused.size()); }
    std::string pipelineError() const { std::lock_guard<std::mutex> lock(async_->mutex); return async_->lastError; }
    std::string pipelineStatus() const;
    unsigned lastRanges = 0; // memory ranges copied for the last draw (copy mode)
    unsigned renderPasses = 0;
    unsigned buffersBuilt = 0, buffersReused = 0;
    unsigned rangesShared = 0; // large ranges copied once for a submission's draws (copy mode)
    unsigned rangesMapped = 0; // of which, textures read in place through hostView instead
    // The host bytes backing guest [address, address + size), or null. When
    // set, a large range holding a T# is a no-copy buffer over them: the GPU
    // reads the texture in place. Vertex and constant ranges stay copied (the
    // game may rewrite them while the GPU still runs).
    std::function<const void*(std::uint64_t, std::size_t)> hostView;
    unsigned nativeDraws = 0; // draws given a native texture (nativeTextures)
    unsigned nativeViews = 0; // native textures given to those draws, up to 3 each
    unsigned targetReads = 0; // of which, a colour target an earlier draw rendered
    // T#s refused: none captured (per draw), format/tiling, outside the copied
    // ranges, alignment, Metal view refused; and the last refused T#.
    unsigned nativeMisses[5] = {};
    std::string nativeLastMiss;
    // The last 8-bit (video plane) view: non-zero bytes Metal sees, out of 64.
    std::string nativeLastPlane;
    // A Metal sampler with S# `s`'s address modes and filters, for an argument
    // buffer; nil for modes Metal has no match for (border, mirror once) or
    // depth compare, which keep GCN's filtering.
    static MTLSamplerDescriptor* nativeSamplerDescriptor(const std::array<std::uint32_t, 4>& s);

private:
    // Programs, fetch SGPRs, PS inputs, colour format, blend, mask, depth format.
    using PipelineKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::string, unsigned, std::uint32_t, unsigned, unsigned>;
    // `format`: CB FORMAT (also its T# data format) of a colour target.
    struct Target { id<MTLTexture> texture; bool fresh; unsigned format = 0xa; };
    std::map<std::uint64_t, Target> depthTargets_;
    std::map<std::uint64_t, float> depthClearTo_; // Clears waiting for a pass, by address.
    // By compare function (0-7) and write: 16 states at most.
    std::map<unsigned, id<MTLDepthStencilState>> depthStates_;
    id<MTLDepthStencilState> depthState(unsigned function, bool write);
    // The depth texture for `d` (d.pitch x d.height), copied from a texture of
    // another size when the buffer was drawn with other targets before.
    Target* depthTexture(const PM4Draw& d, id<MTLCommandBuffer> commands);
    id<MTLTexture> depthAttachment_ = nil;
    id<MTLDevice> device_;
    const GCNMemory& code_;
    id<MTLBuffer> guest_;
    std::uint64_t guestBase_;
    GCNMemory scanMemory_; // code_ reads; scanned lanes' stores are dropped
    PM4ResourceScanner scanner_;
    bool native_;
    std::function<std::int32_t(std::uint64_t, const void*, std::size_t, std::uint64_t*)> writeGuest_;
    id<MTLArgumentEncoder> nativeArguments_ = nil;
    std::map<PipelineKey, id<MTLRenderPipelineState>> pipelines_;
    // Each pixel program's scalar loads, by program address, sorted by pc.
    struct ScalarSites { std::vector<GCNScalarSite> list; std::size_t words = 0; };
    std::map<std::uint64_t, ScalarSites> scalarSites_;
    std::vector<std::uint32_t> siteTable_;
    // Pipelines Metal is building (asyncPipelines), shared with its completion handlers.
    struct AsyncPipelines {
        struct Pending { std::chrono::steady_clock::time_point since; const char* stage; std::string programs; };
        std::mutex mutex;
        std::map<PipelineKey, Pending> pending;
        std::set<PipelineKey> refused;
        std::map<PipelineKey, id<MTLRenderPipelineState>> ready; // nil: refused
        // Default builds wait here until no size build is left, then run one at a time.
        std::deque<std::function<void()>> later;
        unsigned sizing = 0;
        bool running = false;
        std::uint64_t buildingPS = 0; // The default build running, for the report.
        std::chrono::steady_clock::time_point buildingSince;
        std::string defaultsDone;
        void next() {
            std::function<void()> start;
            { std::lock_guard<std::mutex> lock(mutex);
              if (sizing || running || later.empty()) return;
              start = std::move(later.front()); later.pop_front(); running = true; }
            start();
        }
        std::string lastError;
        double slowest = 0; // Seconds the slowest finished build took.
    };
    std::shared_ptr<AsyncPipelines> async_ = std::make_shared<AsyncPipelines>();
    bool deferred_ = false; // the last pipeline() call left its draw to a pending build
    std::string lastDispatchNote_;
    struct PendingGuestWrite { std::uint64_t address, size, offset; id<MTLBuffer> buffer; };
    std::vector<PendingGuestWrite> pendingGuestWrites_;
    static MTLCompileOptions* compileOptions();
    std::map<std::pair<std::uint64_t, std::uint32_t>, id<MTLComputePipelineState>> kernels_;
    id<MTLComputePipelineState> kernel(const PM4Dispatch& dispatch);
    bool fillProgram(std::uint64_t address);
    bool fillDispatch(const PM4Dispatch& dispatch, id<MTLCommandBuffer> commands);
    id<MTLLibrary> library(const std::string& source);
    std::map<std::uint64_t, Target> targets_;
    std::vector<std::uint64_t> listed_;
    id<MTLBuffer> visibility_ = nil;
    unsigned visibilityCount_ = 0;
    bool details_ = true;
    void startVisibility() {
        visibility_ = details_ ? [device_ newBufferWithLength:8 * 64 options:MTLResourceStorageModeShared] : nil;
        if (visibility_) std::memset(visibility_.contents, 0, 8 * 64);
        visibilityCount_ = 0;
    }
    void listProgram(const char* stage, std::uint64_t address, const std::uint32_t* user, unsigned userCount, bool always = false);
    std::string programWords(const char* stage, std::uint64_t address, const std::uint32_t* user, unsigned userCount) const;
    bool batching_ = false;
    id<MTLRenderCommandEncoder> encoder_ = nil;
    id<MTLCommandBuffer> commands_ = nil;
    id<MTLTexture> attachment_ = nil;
    struct Upload { id<MTLBuffer> buffer; id<MTLCommandBuffer> owner; };
    std::vector<Upload> uploads_;
    std::uint64_t pooledBytes_ = 0;
    id<MTLBuffer> uploadBuffer(std::size_t bytes, id<MTLCommandBuffer> commands);
    // Ranges of at least sharedRangeBytes get one of the shader's GCN_SLOTS
    // buffers (buffer(5) up), copied once per command buffer: the draws of a
    // submission sampling one atlas share its copy.
    static constexpr unsigned slotCount = 6;
    static constexpr std::uint64_t sharedRangeBytes = 1ull << 20;
    struct Shared { id<MTLBuffer> buffer; std::uint64_t offset; }; // the range's first byte in `buffer`
    std::map<std::pair<std::uint64_t, std::uint64_t>, Shared> shared_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> inPlace_; // Guest ranges the GPU reads directly.
    std::set<std::uint64_t> costly_;
    struct Decoded { id<MTLTexture> texture; std::uint64_t mark, batch; };
    std::uint64_t batchSerial_ = 0;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, Decoded> bc7Textures_;
    id<MTLTexture> bc7Texture(std::uint64_t base, std::uint64_t width, std::uint64_t height, std::uint64_t pitch, unsigned dfmt);
    std::uint64_t decodedBytes_ = 0;
    unsigned nativeBound_ = 0, nativeFiltered_ = 0; // Of the draw being encoded.
    std::map<std::uint64_t, bool> fillPrograms_;
    std::map<std::uint32_t, id<MTLBuffer>> fills_; // Immutable, by fill value.
    id<MTLCommandBuffer> sharedOwner_ = nil;
    Shared sharedRange(std::uint64_t base, std::uint64_t size, bool texture, id<MTLCommandBuffer> commands);
    // Guest [address, address + size) into `out`; unreadable pages read as zero.
    void copyGuest(std::uint64_t address, std::uint8_t* out, std::uint64_t size) const;
    // `format` MTLPixelFormatInvalid: no colour attachment (a depth-only draw).
    id<MTLRenderPipelineState> pipeline(const PM4Draw& draw, MTLPixelFormat format, MTLPixelFormat depth);
    // `ranges`: (guest start, length, buffer offset) triples; none for the guest
    // window. An offset's bits 48 and up select slots[n - 1] instead of `memory`.
    id<MTLTexture> nativeTexture(const std::array<std::uint32_t, 8>& t, id<MTLBuffer> memory, id<MTLBuffer> const* slots,
                                 const std::uint64_t* ranges, unsigned rangeCount, id<MTLTexture> drawing);
    // The Metal sampler for S# `s` (cached), nil when Metal's cannot match it.
    std::map<std::array<std::uint32_t, 4>, id<MTLSamplerState>> samplers_;
    id<MTLSamplerState> nativeSampler(const std::array<std::uint32_t, 4>& s);
    bool fail(const std::string& message) { error = message; return false; }
};
} // namespace maxps4::gpu
#endif
