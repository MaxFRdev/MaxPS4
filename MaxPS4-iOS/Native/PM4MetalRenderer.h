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
// a Depth32Float_Stencil8 texture (any DB_Z_INFO format, 8-bit stencil), kept across draws
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
#include "GCNTranslator.hpp"
#include <array>
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
#include <vector>
#include <algorithm>

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
    unsigned depthOnlyShared = 0; // depth-only draws on a pipeline without their pixel program
    unsigned depthOnlyUntranslated = 0; // of their programs, those MaxPS4 cannot translate (drawn without)
    unsigned kernelsBuilt = 0, ldsKernelsBuilt = 0, targetsSynced = 0, dispatchesWithoutEffect = 0;
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
    // Pixel programs with this many samples or more sample native textures only (see pipeline()).
    // 2: each inlined software sampler (every format, tiling, BC block) made
    // the Metal compile slow, and CoD's libraries queued by the hundred (547
    // waiting in a mission); native textures now cover almost every T# (64475
    // bound, 2252 refused), so only single-sample programs keep the fallback.
    static constexpr std::size_t manySamples = 2;
    unsigned nativeOnlyPrograms = 0;
    void listCostly(std::uint64_t address); // Adds a program's words to programListings, once.
    const std::string& lastDispatchNote() const { return lastDispatchNote_; }
    bool hasPendingGuestWrites() const { return !pendingGuestWrites_.empty(); }
    bool writebackGuestWrites();
    // The draws of the current batch, described for the session report, and
    // the disassembly of the first programs drawn (VS and PS, with user SGPRs).
    std::string batchDraws, programListings;
    // batchDraws: consecutive draws with the same target and programs (a shadow
    // pass, dozens of them) become one entry with its count.
    struct BatchRun { std::uint64_t serial = ~0ull, target = 0, vs = 0, ps = 0; unsigned repeats = 0; std::size_t suffix = 0; } batchRun_;
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
    // Draws with a stencil test or clear; those whose GCN stencil operations
    // Metal has no exact equivalent for (logic ops, a step other than 1, an
    // operation value other than the reference).
    unsigned stencilDraws = 0, stencilApproximated = 0, mrtDraws = 0;
    unsigned embeddedDepthDraws = 0; // Colourless draws with Gnm's embedded VS: not drawn.
    std::string depthReport() const;
    // The last draws and compute stores into colour target `address`, newest
    // last (recorded on detailed batches): what made the image shown.
    std::string drawsInto(std::uint64_t address) const;
    // Flipped buffers (the bridge says), whose centre pixel is read back after
    // each draw or fill into them: the readings that changed it, bytes in texture order.
    // Called on each flip. Probes run for the draws of one frame in 60 only:
    // each ends the render pass (a full store and load of the target on the
    // iPhone's tiled GPU), and every draw of the HUD and composite paid it.
    // Guest memory the command stream itself wrote (DMA_DATA, the constant
    // engine's dumps): copies of it taken earlier in this command buffer for
    // later draws are stale.
    void guestWritten(std::uint64_t address, std::uint64_t size) {
        if (!size || shared_.empty()) return;
        for (auto it = shared_.begin(); it != shared_.end();)
            it = it->first.first < address + size && address < it->first.first + it->first.second ? shared_.erase(it) : std::next(it);
    }
    void noteShown(std::uint64_t address) {
        if (displayProbes && shown_.size() < 8) shown_.insert(address);
        probeFrame_ = displayProbes && (flips_++ % 60) == 0;
    }
    bool displayProbes = true;
    std::string gpuProbes() const;
    // Full-screen quads whose first vertex changed colour (or every 10 s), with the time.
    std::deque<std::string> fades;
    // The Metal format of a colour target (CB_COLOR0_INFO FORMAT, NUMBER_TYPE,
    // COMP_SWAP), MTLPixelFormatInvalid when unsupported: 8_8_8_8 UNORM/SRGB
    // (swap 0 or 1) or SNORM, 16_16_16_16 and 16_16 and 16 FLOAT/UNORM/SNORM,
    // 32 32_32 32_32_32_32 FLOAT, 2_10_10_10 UNORM, 10_11_11 FLOAT, 8_8 and
    // 8 UNORM/SNORM; others swap 0 only.
    static MTLPixelFormat targetFormat(unsigned format, unsigned number, unsigned swap);
    static MTLClearColor fastClearColour(unsigned format, unsigned number, const std::uint32_t words[2]);
    unsigned fastClearsApplied = 0; // colour targets cleared for an eliminated fast clear
    // Bytes per pixel of a colour target format (0 for others).
    static unsigned bytesPerPixel(MTLPixelFormat format);
    // 1 for an unsigned integer pixel format, 2 for a signed one, else 0.
    static int integerFormat(MTLPixelFormat format);
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
    // GCN translations reused (same program and user SGPRs, another render
    // state), and new pipelines put off to keep the frame within its budget.
    unsigned translationsReused = 0, translationsPutOff = 0;
    double translateMs = 0; // GCN translation time on the submission thread, in all
    // CPU time of encode() for the draws it rendered, and of its parts: resource
    // scans (the GCN interpreter), guest copies, new Metal buffers.
    double encodeMs = 0, scanMs = 0, copyMs = 0, allocateMs = 0;
    unsigned encodedDraws = 0;
    // Draws left out because a program of theirs could not be translated
    // (the run goes on); the latest reason.
    unsigned untranslatedDraws = 0;
    std::string lastUntranslated;
    // Distinct reasons programs could not be translated (the instruction, not
    // its address), with the number of program pairs each stopped.
    std::map<std::string, unsigned> untranslatedReasons;
    std::string untranslatedSummary() const {
        std::vector<std::pair<unsigned, std::string>> sorted;
        for (const auto& [why, count] : untranslatedReasons) sorted.push_back({count, why});
        std::sort(sorted.rbegin(), sorted.rend());
        std::string text;
        for (std::size_t k = 0; k < sorted.size() && k < 8; ++k)
            text += "\n  - " + sorted[k].second + " : " + std::to_string(sorted[k].first) + " programme(s)";
        return text;
    }
    unsigned pipelinesPending() const { std::lock_guard<std::mutex> lock(async_->mutex); return unsigned(async_->pending.size()); }
    unsigned pipelinesRefused() const { std::lock_guard<std::mutex> lock(async_->mutex); return unsigned(async_->refused.size()); }
    std::string pipelineError() const { std::lock_guard<std::mutex> lock(async_->mutex); return async_->lastError; }
    std::string pipelineStatus() const;
    unsigned lastRanges = 0; // memory ranges copied for the last draw (copy mode)
    unsigned renderPasses = 0;
    unsigned buffersBuilt = 0, buffersReused = 0;
    unsigned nativeArgumentsShared = 0; // draws reusing the previous draw's native argument buffer
    unsigned depthCopiesMade = 0, depthReads = 0; // depth buffers copied for, and read by, programs
    unsigned rangesShared = 0; // large ranges copied once for a submission's draws (copy mode)
    unsigned rangesMapped = 0; // of which, textures read in place through hostView instead
    // The host bytes backing guest [address, address + size), or null. When
    // set, a large range holding a T# is a no-copy buffer over them: the GPU
    // reads the texture in place. Vertex and constant ranges stay copied (the
    // game may rewrite them while the GPU still runs).
    std::function<const void*(std::uint64_t, std::size_t)> hostView;
    unsigned nativeDraws = 0; // draws given a native texture (nativeTextures)
    unsigned nativeViews = 0; // native textures given to those draws, up to 3 each
    unsigned coalescedDraws = 0; // draws whose memory ranges were joined to fit 16
    unsigned rangeRefusedDraws = 0; // left out: joining their ranges would copy over 8 MiB more
    unsigned targetReads = 0; // of which, a colour target an earlier draw rendered
    // T#s refused: none captured (per draw), format/tiling, outside the copied
    // ranges, alignment, Metal view refused; and the last refused T#.
    unsigned nativeMisses[5] = {};
    std::string nativeLastMiss;
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
    // drawsInto(): per target (32 at most), the last 10 runs of writes by the same
    // program or fill, each with its first line and how many writes it holds.
    std::map<std::uint64_t, std::deque<std::pair<std::string, unsigned>>> into_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::set<std::uint64_t> shown_;
    std::uint64_t flips_ = 0;
    bool probeFrame_ = false;
    static constexpr unsigned probeSlots = 512;
    id<MTLBuffer> probeRing_ = nil;
    std::uint64_t probeNext_ = 0;
    struct Probe { unsigned slot; std::uint64_t address; std::string line; };
    std::deque<Probe> probes_;
    mutable char text_[64] = {};
    void probe(id<MTLTexture> texture, std::uint64_t address, std::string line, id<MTLCommandBuffer> commands);
    std::map<std::uint64_t, std::pair<double, std::array<float, 4>>> fadeLast_; // per pixel program
    void noteInto(std::uint64_t address, std::string line);
    std::map<std::uint64_t, float> depthClearTo_; // Clears waiting for a pass, by address.
    std::map<std::uint64_t, MTLClearColor> colourClearTo_; // fast clears waiting for a pass, by target
    std::map<std::uint64_t, unsigned> stencilClearTo_; // The same for stencil.
    // By depth function and write, then stencil faces (see depthState).
    std::map<std::array<std::uint32_t, 5>, id<MTLDepthStencilState>> depthStates_;
    id<MTLDepthStencilState> depthState(unsigned function, bool write);
    // The draw's depth and stencil state; its stencil reference values in `ref`.
    id<MTLDepthStencilState> depthStencilState(const PM4Draw& d, unsigned ref[2]);
    // The depth texture for `d` (d.pitch x d.height), copied from a texture of
    // another size when the buffer was drawn with other targets before.
    Target* depthTexture(const PM4Draw& d, id<MTLCommandBuffer> commands);
    // Depth buffers read as textures (deferred lighting, SSAO, fog, shadows):
    // they live only in their Metal depth texture, so a T# at a depth
    // buffer's address reads an R32Float copy, redone when the depth changed.
    struct DepthCopy { id<MTLTexture> texture = nil; std::uint64_t version = ~0ull; };
    std::map<std::uint64_t, DepthCopy> depthCopies_;
    std::map<std::uint64_t, std::uint64_t> depthVersion_;
    id<MTLRenderPipelineState> depthCopyPipeline_ = nil;
    bool depthCopyBroken_ = false;
    bool copyDepth(std::uint64_t address, id<MTLCommandBuffer> commands);
    // A T# inside a depth buffer (a shadow cascade in an atlas): the rows it
    // starts at, copied out of that buffer's R32Float copy, by T# address.
    struct DepthPart { id<MTLTexture> texture = nil; std::uint64_t version = ~0ull; };
    std::map<std::uint64_t, DepthPart> depthParts_;
    bool copyDepthPart(std::uint64_t address, unsigned width, unsigned height, id<MTLCommandBuffer> commands);
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
    // Pixel programs that may discard (GCNTranslation::mayKill), by address:
    // a depth-only draw whose program cannot runs no pixel program, so its
    // pipeline leaves the program out and is shared by every such draw.
    std::map<std::uint64_t, bool> psMayKill_;
    // Each pixel program's scalar loads, by program address, sorted by pc.
    struct ScalarSites { std::vector<GCNScalarSite> list; std::size_t words = 0; };
    std::map<std::uint64_t, ScalarSites> scalarSites_;
    std::vector<std::uint32_t> siteTable_;
    // Translations by program and options (translationKey); emptied past 48 MB.
    std::map<std::string, GCNTranslation> translations_;
    std::size_t translationBytes_ = 0;
    GCNTranslation translateCached(std::uint64_t address, const GCNTranslateOptions& options);
    // Translation time spent in the current 16 ms window (asyncPipelines).
    std::chrono::steady_clock::time_point translateWindow_{};
    double translateWindowMs_ = 0;
    // Pipelines Metal is building (asyncPipelines), shared with its completion handlers.
    struct AsyncPipelines : std::enable_shared_from_this<AsyncPipelines> {
        // `wanted`: when a draw last asked for the pipeline (steady clock, ns),
        // shared with its queued jobs: the queues start the latest wanted first.
        using Wanted = std::shared_ptr<std::atomic<std::int64_t>>;
        static std::int64_t nowNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        struct Pending { std::chrono::steady_clock::time_point since; const char* stage; std::string programs; Wanted wanted; };
        // A queued job and the pipeline's `wanted`.
        struct Queued { Wanted wanted; std::function<void()> job; };
        // The queued job whose pipeline a draw wanted last (with mutex held):
        // a loading screen's pipelines no longer drawn wait behind the frame's.
        static std::function<void()> takeNewest(std::deque<Queued>& queue) {
            auto best = queue.begin();
            std::int64_t newest = best->wanted ? best->wanted->load() : 0;
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                const std::int64_t at = it->wanted ? it->wanted->load() : 0;
                if (at > newest) { newest = at; best = it; }
            }
            std::function<void()> job = std::move(best->job);
            queue.erase(best);
            return job;
        }
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
        // Pipelines the iPhone compiled, kept on disk between launches (a
        // binary archive in Caches): a pipeline found there skips the backend
        // compile, a minute for some of CoD's. See openArchive.
        NSArray<id<MTLBinaryArchive>>* lookups = nil; // earlier launches', read only
        id<MTLBinaryArchive> building = nil;          // this launch's, on archiveQueue
        NSURL* buildingURL = nil;
        bool archiveOpened = false;
        id<MTLDevice> archiveDevice = nil; // for a new archive after a failed write
        unsigned archiveRestarts = 0;
        dispatch_queue_t archiveQueue = nil;
        unsigned archiveHits = 0, archiveMisses = 0, archiveAdded = 0, archiveUnsaved = 0, archiveSaves = 0;
        unsigned long long archiveBytes = 0;
        std::chrono::steady_clock::time_point archiveSavedAt;
        std::string archiveState, archiveError;
        void openArchive(id<MTLDevice> device);
        void remember(MTLRenderPipelineDescriptor* descriptor);
        // Builds the pipeline from source, then adds it to this launch's archive.
        void compile(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor,
                     std::function<void(id<MTLRenderPipelineState>, NSError*)> done, Wanted wanted = nullptr);
        void saveArchive(); // on archiveQueue
        std::string archiveReport() const; // with mutex held
        // Libraries by source (hash, length, optimisation): many pipelines
        // differ only in state and share one program pair, and CoD's are up
        // to 370 KB of Metal; each is compiled once. Waiters of a compile in
        // flight are called when it finishes.
        using LibraryKey = std::tuple<std::size_t, std::size_t, bool>;
        std::map<LibraryKey, id<MTLLibrary>> libraries;
        std::map<LibraryKey, std::vector<std::function<void(id<MTLLibrary>, NSError*)>>> compiling;
        unsigned librariesShared = 0, librariesCompiled = 0;
        // At most compileLimit library compiles in flight: hundreds started
        // together (CoD's mission) left the iPhone's compiler service
        // thrashing, each taking a minute. The rest wait their turn, in order.
        // Pipeline creations stay outside it (counted in pipelinesRunning).
        static constexpr unsigned compileLimit = 4;
        unsigned compilesRunning = 0, pipelinesRunning = 0;
        std::deque<Queued> compileQueue;
        void startCompile(std::function<void()> job, Wanted wanted = nullptr) {
            { std::lock_guard<std::mutex> lock(mutex);
              if (compilesRunning >= compileLimit) { compileQueue.push_back({std::move(wanted), std::move(job)}); return; }
              ++compilesRunning; }
            job();
        }
        // Pipeline creations have a queue of their own: unbounded, 49 ran at once
        // and the slowest took 70 s (59 built in a session, 756 waiting);
        // sharing the library limit starved them (38 built). Six at a time.
        static constexpr unsigned pipelineLimit = 6;
        static constexpr bool kUseBinaryArchive = false; // see openArchive
        std::deque<Queued> pipelineQueue;
        void startPipeline(std::function<void()> job, Wanted wanted = nullptr) {
            { std::lock_guard<std::mutex> lock(mutex);
              if (pipelinesRunning >= pipelineLimit) { pipelineQueue.push_back({std::move(wanted), std::move(job)}); return; }
              ++pipelinesRunning; }
            job();
        }
        void pipelineDone() {
            std::function<void()> job;
            { std::lock_guard<std::mutex> lock(mutex);
              if (pipelineQueue.empty()) { --pipelinesRunning; return; }
              job = takeNewest(pipelineQueue); }
            job();
        }
        void compileDone() {
            std::function<void()> job;
            { std::lock_guard<std::mutex> lock(mutex);
              if (compileQueue.empty()) { --compilesRunning; return; }
              job = takeNewest(compileQueue); }
            job();
        }
    };
    std::shared_ptr<AsyncPipelines> async_ = std::make_shared<AsyncPipelines>();
    bool deferred_ = false; // the last pipeline() call left its draw to a pending build
    bool untranslated_ = false; // the last pipeline() call met a program it cannot translate
    std::set<std::pair<std::uint64_t, std::uint64_t>> untranslatedPrograms_; // (VS, PS)
    std::string lastDispatchNote_;
    struct PendingGuestWrite { std::uint64_t address, size, offset; id<MTLBuffer> buffer; };
    std::vector<PendingGuestWrite> pendingGuestWrites_;
    static MTLCompileOptions* compileOptions();
    std::map<std::pair<std::uint64_t, std::uint32_t>, id<MTLComputePipelineState>> kernels_;
    // Kernels using LDS (GCNTranslation::lds), with its size in bytes (from
    // COMPUTE_PGM_RSRC2.LDS_SIZE, 512-byte units): threadgroup memory 0.
    std::map<std::pair<std::uint64_t, std::uint32_t>, unsigned> ldsKernels_;
    id<MTLComputePipelineState> kernel(const PM4Dispatch& dispatch);
    // 0: not a fill; 1: Gnmx's table fill; 2: a fill with one value from a constant buffer.
    int fillProgram(std::uint64_t address);
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
    id<MTLTexture> moreAttachments_[7] = {}; // colour targets 1-7 of the open encoder
    id<MTLTexture> drawingMore_[7] = {};     // colour targets 1-7 of the draw being encoded
    // `free`: its owner completed (checked once, Metal's status is a message send).
    // Pooled upload buffers by length, each length's in submission order:
    // command buffers finish in order, so only the oldest needs checking.
    struct Upload { id<MTLBuffer> buffer; id<MTLCommandBuffer> owner; };
    std::map<std::size_t, std::deque<Upload>> uploads_;
    std::size_t uploadCount_ = 0;
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
    // `checked`: when its guest memory was last sampled for changes; `bytes`: its decoded size.
    struct Decoded { id<MTLTexture> texture; std::uint64_t mark, batch; std::chrono::steady_clock::time_point checked; std::uint64_t bytes; };
    std::uint64_t batchSerial_ = 0;
    using DecodedKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::map<DecodedKey, Decoded> bc7Textures_;
    std::uint64_t decodedBytes_ = 0;
    // A decoded texture still current (nil otherwise, with its memory's mark).
    id<MTLTexture> decodedTexture(const DecodedKey& key, std::uint64_t base, std::uint64_t bytes, std::uint64_t& mark);
    void keepDecoded(const DecodedKey& key, id<MTLTexture> texture, std::uint64_t mark, std::uint64_t bytes);
public:
    double textureMs = 0; unsigned texturesDecoded = 0;
private:
    id<MTLTexture> bc7Texture(std::uint64_t base, std::uint64_t width, std::uint64_t height, std::uint64_t pitch, unsigned dfmt, bool snorm = false);
    id<MTLTexture> thinTexture(std::uint64_t base, std::uint64_t width, std::uint64_t height, std::uint64_t pitch, unsigned dfmt,
                               MTLPixelFormat format, unsigned pixelBytes);
    // The Metal format a T#'s DFMT/NFMT reads as through texture2d<float>
    // (unorm or float only), and its bytes per pixel; Invalid when none.
    MTLPixelFormat nativeFormat(unsigned dfmt, unsigned nfmt, unsigned& pixelBytes) const;
    unsigned nativeBound_ = 0, nativeFiltered_ = 0; // Of the draw being encoded.
    // The last native argument buffer and what it holds: consecutive draws
    // binding the same textures in one command buffer share it.
    struct NativeBinding {
        id<MTLTexture> images[8] = {};
        id<MTLSamplerState> samplers[8] = {};
        std::array<std::uint32_t, 8> descriptors[8] = {};
        std::array<std::uint32_t, 4> samplerWords[8] = {};
        unsigned enabled = 0;
        id<MTLBuffer> arguments = nil;
        id<MTLCommandBuffer> commands = nil;
    } lastNative_;
    std::map<std::uint64_t, int> fillPrograms_;
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
