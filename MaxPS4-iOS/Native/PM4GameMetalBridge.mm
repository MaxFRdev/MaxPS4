#import "PM4GameMetalBridge.h"
#include "PM4MetalRenderer.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>

using namespace maxps4::gpu;
namespace {
using Clock = std::chrono::steady_clock;
double elapsedMs(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}
double gpuMs(id<MTLCommandBuffer> command) {
    return command.GPUStartTime > 0 && command.GPUEndTime >= command.GPUStartTime
        ? (command.GPUEndTime - command.GPUStartTime) * 1000 : -1;
}
NSString* gpuText(double ms) { return ms < 0 ? @"n/d" : [NSString stringWithFormat:@"%.2f", ms]; }
struct GameMetalState {
    std::mutex mutex;
    GCNMemory memory;
    PM4DrawStats stats;
    PM4DrawCollector collector;
    PM4MetalRenderer renderer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    // Completion handlers only hop here: they need the lock, and a submission
    // can wait for the GPU holding it (flushComputeWrites), while Metal may not
    // finish that work until the handlers of earlier command buffers return.
    dispatch_queue_t completions = dispatch_queue_create("maxps4.metal.completions", DISPATCH_QUEUE_SERIAL);
    id<MTLSharedEvent> displayEvent;
    std::atomic<uint64_t> displayEventValue{0};
    std::string error;
    // Keep at most two render submissions ahead of the GPU. Metal preserves
    // queue order; upload buffers remain owned by their command buffers.
    std::deque<id<MTLCommandBuffer>> inflight;
    double encodeMs = 0, waitMs = 0, renderGpuMs = -1, copyMs = 0, copyGpuMs = -1;
    // Display copies: flipped buffers found among the colour targets or not
    // (then what the game shows was never drawn by Metal), and the last one.
    unsigned shown = 0, absent = 0;
    uint64_t lastShown = 0;
    // Compute dispatches run as kernels, or refused (and skipped) with the last reason.
    unsigned computeRun = 0, computeRefused = 0;
    // Waits for compute results inside submissions, and how long batches took.
    unsigned barriers = 0, batchBarriers = 0, mostBarriers = 0, batches = 0, slowBatches = 0;
    double barrierMs = 0;
    std::string computeError;
    // The draws of the last batch that had any, and the centre pixel of the
    // last displayed image (its four bytes in texture order).
    std::string lastDraws;
    uint32_t centre = 0;
    std::string targetPixels; // the centre pixel of every colour target
    std::string samples;      // visibility results of the last batch's draws
    bool centreBGRA = false;
    bool nativeTextures;
    // Where the submission in progress is, read without the lock by the live
    // report: 0 none, 1 waiting for the previous one's GPU work, 2 waiting for
    // the lock, 3 encoding draw `phaseDraw` (PS `phasePS`), 4 committing,
    // 5 waiting for compute work, 6 writing its results to guest memory.
    std::atomic<int> phase{0};
    std::atomic<long long> phaseSince{0};
    std::atomic<unsigned> phaseDraw{0};
    std::atomic<unsigned long long> phasePS{0};
    void enter(int p) { phase = p; phaseSince = Clock::now().time_since_epoch().count(); }
    NSString* report() const {
        const unsigned* m = renderer.nativeMisses;
        NSString* native = nativeTextures ? [NSString stringWithFormat:
            @"\nTextures natives : %u dessins, %u textures (%u images rendues par le GPU) · refus : %u sans T#, %u format, %u hors plage, %u alignement, %u vue Metal%@%s",
            renderer.nativeDraws, renderer.nativeViews, renderer.targetReads, m[0], m[1], m[2], m[3], m[4],
            renderer.nativeLastMiss.empty() ? @"" : @"\nDernier refus : ", renderer.nativeLastMiss.c_str()] : @"";
        const std::string refusal = renderer.pipelineError();
        const std::string pending = renderer.pipelineStatus();
        native = [NSString stringWithFormat:@"\nPipelines : %u construits, %u en cours, %u refusés · %u dessins reportés%s%s%s%@",
            renderer.pipelinesBuilt, renderer.pipelinesPending(), renderer.pipelinesRefused(), renderer.drawsDeferred,
            refusal.empty() ? "" : " · dernier refus : ", refusal.c_str(), pending.c_str(), native];
        native = [NSString stringWithFormat:@"\nBarrières compute : %u (%u au plus par lot) · attente GPU %.0f ms · entrée %.0f ms (%.0f Mio) · sortie %.0f ms (%.0f Mio) · lots : %u, dont %u de plus de 33 ms%@",
            barriers, mostBarriers, barrierMs, renderer.dispatchCopyMs, double(renderer.dispatchCopyBytes) / (1 << 20),
            renderer.writebackMs, double(renderer.writebackBytes) / (1 << 20), batches, slowBatches, native];
        return [NSString stringWithFormat:@"Metal : %u dessins, %u rendus, %u échecs, %u passes, %u buffers créés, %u réutilisés, %u grandes zones partagées (%u lues sans copie)\nIgnorés : %u dessins sans cible couleur · %zu cibles\nCalculs : %u reçus, %u exécutés, %u sans effet visible, %u refusés%s%s · %u kernels · %u images synchronisées · dernier : %s\nPixel central affiché : R %u V %u B %u A %u\nPixel central des cibles (octets dans l'ordre de la texture) :%s\nÉchantillons dessinés par dessin (dernier lot) :%s\nDessins du dernier lot :%s\nProgrammes des premiers dessins :%s\nDernier ignoré : %s\nDernier calcul : %s\nAffichage : %u images copiées, %u sans cible Metal (dernière 0x%llx)\nDernier lot : CPU %.2f ms · GPU %@ ms · attente %.2f ms\nCopie image : CPU %.2f ms · GPU %@ ms%@%@",
            stats.draws, stats.rendered, stats.failed, renderer.renderPasses, renderer.buffersBuilt, renderer.buffersReused, renderer.rangesShared, renderer.rangesMapped,
            stats.skipped, renderer.targetCount(), stats.dispatches, computeRun - renderer.dispatchesWithoutEffect,
            renderer.dispatchesWithoutEffect, computeRefused,
            computeError.empty() ? "" : " : ", computeError.c_str(), renderer.kernelsBuilt, renderer.targetsSynced,
            renderer.lastDispatchNote().empty() ? "aucun détail" : renderer.lastDispatchNote().c_str(),
            (centre >> (centreBGRA ? 16 : 0)) & 0xff, (centre >> 8) & 0xff, (centre >> (centreBGRA ? 0 : 16)) & 0xff, centre >> 24, targetPixels.c_str(),
            samples.c_str(), lastDraws.empty() ? " aucun" : lastDraws.c_str(), renderer.programListings.c_str(), stats.lastSkipped.c_str(), stats.lastDispatch.c_str(), shown, absent, (unsigned long long)lastShown,
            encodeMs, gpuText(renderGpuMs), waitMs, copyMs, gpuText(copyGpuMs), native,
            // Why rendering stopped (the draw that failed is in no batch list).
            error.empty() ? @"" : [NSString stringWithFormat:@"\nÉchec Metal : %s", error.c_str()]];
    }
    GameMetalState(id<MTLDevice> device, MaxPS4GameMemoryReader reader,
                   MaxPS4GameMemoryWriter writer, bool nativeTextures)
        : memory{[reader](uint64_t a, void* d, size_t n) { return bool(reader(a,d,n)); }, {}},
          collector(memory, stats), renderer(device, memory, nil, 0, nativeTextures,
              writer ? std::function<int32_t(uint64_t,const void*,size_t,uint64_t*)>([writer](uint64_t a, const void* d, size_t n, uint64_t* f) { return int32_t(writer(a,d,n,f)); }) :
                       std::function<int32_t(uint64_t,const void*,size_t,uint64_t*)>{}), device(device),
          queue([device newCommandQueue]), displayEvent([device newSharedEvent]), nativeTextures(nativeTextures) {}
};
void recordDisplayCopy(GameMetalState& s, id<MTLCommandBuffer> command, Clock::time_point started,
                       id<MTLTexture> source, const std::vector<std::pair<uint64_t, id<MTLTexture>>>& targets,
                       id<MTLBuffer> pixel) {
    s.copyMs = elapsedMs(started); s.copyGpuMs = gpuMs(command);
    if (!pixel) return;
    const auto* bytes = static_cast<const uint8_t*>(pixel.contents);
    std::memcpy(&s.centre, bytes, 4); s.centreBGRA = source.pixelFormat == MTLPixelFormatBGRA8Unorm;
    s.targetPixels.clear();
    for (size_t k = 0; k < targets.size(); ++k) {
        char text[96];
        const uint8_t* p = bytes + 4 * (k + 1);
        std::snprintf(text, sizeof text, "\n  - 0x%llx %lux%lu %s : %u %u %u %u", (unsigned long long)targets[k].first,
                      (unsigned long)targets[k].second.width, (unsigned long)targets[k].second.height,
                      targets[k].second.pixelFormat == MTLPixelFormatBGRA8Unorm ? "BGRA" : "RGBA", p[0], p[1], p[2], p[3]);
        s.targetPixels += text;
    }
}
}
@interface MaxPS4GameMetalBridge ()
@property(atomic, copy) NSString* lastReport;
@property(atomic, readwrite) double submitMilliseconds;
@end
@implementation MaxPS4GameMetalBridge {
    std::shared_ptr<GameMetalState> state_;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader {
    return [self initWithDevice:device reader:reader writer:nil view:nil nativeTextures:NO];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader nativeTextures:(BOOL)nativeTextures {
    return [self initWithDevice:device reader:reader writer:nil view:nil nativeTextures:nativeTextures];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader
                          view:(MaxPS4GameMemoryView)view nativeTextures:(BOOL)nativeTextures {
    return [self initWithDevice:device reader:reader writer:nil view:view nativeTextures:nativeTextures];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader
                         writer:(MaxPS4GameMemoryWriter)writer
                           view:(MaxPS4GameMemoryView)view nativeTextures:(BOOL)nativeTextures {
    if ((self = [super init])) {
        self.lastReport = @"Metal : préparation du premier rendu";
        state_ = std::make_shared<GameMetalState>(device, [reader copy], [writer copy], nativeTextures);
        if (!state_->queue) return nil;
        if (view) {
            MaxPS4GameMemoryView held = [view copy];
            state_->renderer.hostView = [held](uint64_t a, size_t n) { return held(a, n); };
        }
    }
    return self;
}
- (int32_t)submit:(const uint32_t*)words count:(size_t)count {
    @autoreleasepool {
        auto& s = *state_;
        if (!words || !count || count > 65536) return -EINVAL;
        const auto started = Clock::now();
        // The game does not wait for its work's GPU completion: every byte a
        // draw reads, large textures excepted (read in place), is copied while
        // encoding (upload buffers are reused only once their command buffer
        // has completed, constants go by value), targets are GPU-only, and display
        // copies follow on the same queue. The queue preserves dependencies
        // between submissions; cap queued work at two so a slow GPU cannot
        // build an unbounded backlog.
        struct Leave { GameMetalState& s; ~Leave() { s.phase = 0; } } leave{s};
        id<MTLCommandBuffer> waitFor = nil;
        s.enter(2);
        {
            std::lock_guard<std::mutex> peek(s.mutex);
            while (!s.inflight.empty()) {
                id<MTLCommandBuffer> front = s.inflight.front();
                if (front.status != MTLCommandBufferStatusCompleted && front.status != MTLCommandBufferStatusError) break;
                if (front.status != MTLCommandBufferStatusCompleted || front.error)
                    s.error = front.error ? front.error.localizedDescription.UTF8String : "Metal GPU completion failed";
                else if (gpuMs(front) >= 0) s.renderGpuMs = gpuMs(front);
                s.inflight.pop_front();
            }
            if (s.inflight.size() >= 2) waitFor = s.inflight.front();
        }
        if (waitFor) {
            s.enter(1);
            [waitFor waitUntilCompleted];
        }
        s.enter(2);
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.error.empty()) return -EIO;
        s.waitMs = 0;
        if (waitFor) {
            s.waitMs = elapsedMs(started);
            while (!s.inflight.empty()) {
                id<MTLCommandBuffer> front = s.inflight.front();
                if (front.status != MTLCommandBufferStatusCompleted && front.status != MTLCommandBufferStatusError) break;
                if (front.status != MTLCommandBufferStatusCompleted || front.error) {
                    s.error = front.error ? front.error.localizedDescription.UTF8String : "Metal GPU completion failed";
                    return -EIO;
                }
                if (gpuMs(front) >= 0) s.renderGpuMs = gpuMs(front);
                s.inflight.pop_front();
            }
        }
        const auto encodingStarted = Clock::now();
        id<MTLCommandBuffer> command = [s.queue commandBuffer];
        if (!command) { s.error = "Metal command buffer allocation"; return -ENOMEM; }
        const unsigned failed = s.stats.failed;
        const unsigned rendered = s.stats.rendered;
        bool segmentEncoded = false;
        bool barrierFailure = false;
        const auto flushComputeWrites = [&]() {
            s.renderer.endBatch();
            [command commit];
            const auto barrierStarted = Clock::now();
            s.enter(5);
            [command waitUntilCompleted];
            s.waitMs += elapsedMs(barrierStarted);
            s.barrierMs += elapsedMs(barrierStarted);
            ++s.barriers; ++s.batchBarriers;
            if (command.status != MTLCommandBufferStatusCompleted || command.error) {
                s.error = command.error ? command.error.localizedDescription.UTF8String : "Metal compute barrier failed";
                barrierFailure = true;
                return;
            }
            if (gpuMs(command) >= 0) s.renderGpuMs = gpuMs(command);
            s.enter(6);
            if (!s.renderer.writebackGuestWrites()) {
                s.error = s.renderer.error;
                barrierFailure = true;
                return;
            }
            command = [s.queue commandBuffer];
            if (!command) {
                s.error = "Metal command buffer allocation after compute barrier";
                barrierFailure = true;
                return;
            }
            segmentEncoded = false;
            s.enter(3);
            s.renderer.resumeBatch();
        };
        s.renderer.beginBatch();
        s.phaseDraw = 0; s.phasePS = 0;
        s.batchBarriers = 0;
        s.enter(3);
        s.collector.execute(words, count, [&](const PM4Draw& draw) {
            ++s.phaseDraw; s.phasePS = draw.ps;
            if (s.stats.failed != failed || barrierFailure) return;
            if (s.renderer.encode(draw, command)) { ++s.stats.rendered; segmentEncoded = true; }
            else {
                char where[96];
                std::snprintf(where, sizeof where, " (VS 0x%llx, PS 0x%llx, cible 0x%llx)", (unsigned long long)draw.vs,
                              (unsigned long long)draw.ps, (unsigned long long)draw.target);
                s.stats.fail(s.renderer.error + where);
            }
        }, [&](const PM4Dispatch& dispatch) {
            // A dispatch that cannot run is skipped, as before compute support.
            if (s.stats.failed != failed || barrierFailure) return;
            const unsigned encodedBefore = s.renderer.dispatchesEncoded;
            if (s.renderer.dispatch(dispatch, command)) {
                ++s.computeRun;
                if (s.renderer.dispatchesEncoded != encodedBefore) segmentEncoded = true;
                if (s.renderer.hasPendingGuestWrites()) flushComputeWrites();
            }
            else { ++s.computeRefused; s.computeError = s.renderer.error; }
        });
        s.enter(4);
        s.renderer.endBatch();
        if (!s.renderer.batchDraws.empty()) s.lastDraws = s.renderer.batchDraws;
        if (barrierFailure) return -EIO;
        if (s.stats.failed != failed) { s.error = s.stats.error; return -ENOTSUP; }
        const double encoding = elapsedMs(encodingStarted);
        const bool hasRenderWork = segmentEncoded || s.stats.rendered != rendered;
        if (hasRenderWork) {
            auto heldState = state_;
            [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                dispatch_async(heldState->completions, ^{ @autoreleasepool {
                    auto& state = *heldState;
                    std::lock_guard<std::mutex> completionLock(state.mutex);
                    if (completed.status != MTLCommandBufferStatusCompleted || completed.error)
                        state.error = completed.error ? completed.error.localizedDescription.UTF8String : "Metal GPU completion failed";
                    else if (gpuMs(completed) >= 0) state.renderGpuMs = gpuMs(completed);
                    self.lastReport = state.report();
                }});
            }];
            s.inflight.push_back(command);
        }
        [command commit];
        if (hasRenderWork) s.encodeMs = encoding;
        ++s.batches; s.slowBatches += elapsedMs(started) > 33.3;
        s.mostBarriers = std::max(s.mostBarriers, s.batchBarriers);
        self.submitMilliseconds += elapsedMs(started);
        self.lastReport = s.report();
        // The native queue retires synchronization packets only after this return.
        return 0;
    }
}
- (id<MTLTexture>)copyTargetAtAddress:(uint64_t)address width:(uint32_t)width height:(uint32_t)height {
    auto& s = *state_;
    // Submissions hold the lock only while encoding (a few milliseconds):
    // waiting for it beats dropping the game's image.
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.error.empty() || !width || !height || width > 1920 || height > 1080) return nil;
    id<MTLTexture> source = s.renderer.target(address);
    s.lastShown = address;
    if (!source || source.width < width || source.height < height) {
        ++s.absent;
        self.lastReport = s.report();
        return nil;
    }
    ++s.shown;
    const auto started = Clock::now();
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
        width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [s.device newTextureWithDescriptor:descriptor];
    id<MTLCommandBuffer> command = [s.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = texture && command ? [command blitCommandEncoder] : nil;
    if (!blit) { s.error = "Metal display copy allocation"; return nil; }
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toTexture:texture destinationSlice:0 destinationLevel:0
        destinationOrigin:MTLOriginMake(0,0,0)];
    // Small readbacks keep the report useful without reading back an image.
    std::vector<std::pair<uint64_t, id<MTLTexture>>> targets;
    s.renderer.forEachTarget([&](uint64_t at, id<MTLTexture> t) { if (t && targets.size() < 8) targets.push_back({at, t}); });
    id<MTLBuffer> pixel = [s.device newBufferWithLength:4 * (1 + targets.size()) options:MTLResourceStorageModeShared];
    if (pixel) {
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(width / 2, height / 2, 0)
                   sourceSize:MTLSizeMake(1, 1, 1) toBuffer:pixel destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
        for (size_t k = 0; k < targets.size(); ++k)
            [blit copyFromTexture:targets[k].second sourceSlice:0 sourceLevel:0
                     sourceOrigin:MTLOriginMake(targets[k].second.width / 2, targets[k].second.height / 2, 0)
                       sourceSize:MTLSizeMake(1, 1, 1) toBuffer:pixel destinationOffset:4 * (k + 1)
           destinationBytesPerRow:4 destinationBytesPerImage:4];
    }
    [blit endEncoding];
    if (s.displayEvent) {
        const uint64_t eventValue = s.displayEventValue.fetch_add(1, std::memory_order_relaxed) + 1;
        [command encodeSignalEvent:s.displayEvent value:eventValue];
        auto heldState = state_;
        [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
            dispatch_async(heldState->completions, ^{ @autoreleasepool {
                auto& state = *heldState;
                std::lock_guard<std::mutex> completionLock(state.mutex);
                if (completed.status != MTLCommandBufferStatusCompleted || completed.error) {
                    state.error = "Metal display copy failed";
                    state.displayEvent.signaledValue = eventValue;
                } else {
                    recordDisplayCopy(state, completed, started, source, targets, pixel);
                }
                self.lastReport = state.report();
            }});
        }];
        [command commit];
        s.copyMs = elapsedMs(started); // CPU encoding only; GPU completion is asynchronous.
        self.lastReport = s.report();
        return texture;
    }

    // Older devices without shared events keep the synchronous fallback.
    [command commit];
    lock.unlock();
    [command waitUntilCompleted];
    lock.lock();
    if (command.status != MTLCommandBufferStatusCompleted || command.error) {
        s.error = "Metal display copy failed"; return nil;
    }
    recordDisplayCopy(s, command, started, source, targets, pixel);
    self.lastReport = s.report();
    return texture;
}
- (id<MTLSharedEvent>)displayEvent { return state_->displayEvent; }
- (uint64_t)displayEventValue {
    return state_->displayEventValue.load(std::memory_order_relaxed);
}
- (NSString*)report {
    // An immutable atomic snapshot: reading diagnostics must not wait for the
    // next submission's encoding, shader compilation or GPU completion.
    // A submission stuck for over half a second says where, from atomics.
    auto& s = *state_;
    const int phase = s.phase;
    const double seconds = std::chrono::duration<double>(Clock::now().time_since_epoch() -
                                                         Clock::duration(s.phaseSince.load())).count();
    if (!phase || seconds < 0.5) return self.lastReport;
    static const char* const phases[] = {"", "attente du GPU (lot précédent)", "attente du verrou Metal", "encodage", "fin du lot",
                                         "attente du GPU (écritures compute)", "écriture en mémoire du jeu"};
    static const char* const work[] = {"", " · compilation MSL", " · création du pipeline", " · traduction GCN"};
    const int compiling = s.renderer.compiling;
    return [NSString stringWithFormat:@"Soumission en cours depuis %.1f s : %s, dessin %u (PS 0x%llx)%s\n%@", seconds, phases[phase],
            s.phaseDraw.load(), s.phasePS.load(), compiling > 0 && compiling < 4 ? work[compiling] : "", self.lastReport];
}
- (BOOL)asynchronousPipelines { return state_->renderer.asyncPipelines; }
- (void)setAsynchronousPipelines:(BOOL)asynchronous {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->renderer.asyncPipelines = asynchronous;
}
- (NSString*)failure {
    auto& s = *state_;
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    return lock && !s.error.empty() ? [NSString stringWithUTF8String:s.error.c_str()] : nil;
}
@end
