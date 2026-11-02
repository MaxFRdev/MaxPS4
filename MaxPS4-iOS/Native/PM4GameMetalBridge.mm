#import "PM4GameMetalBridge.h"
#include "PM4MetalRenderer.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
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
    std::string error;
    // The previous submission, still running on the GPU while the game goes on.
    id<MTLCommandBuffer> inflight;
    double encodeMs = 0, waitMs = 0, renderGpuMs = -1, copyMs = 0, copyGpuMs = -1;
    // Display copies: flipped buffers found among the colour targets or not
    // (then what the game shows was never drawn by Metal), and the last one.
    unsigned shown = 0, absent = 0;
    uint64_t lastShown = 0;
    // Compute dispatches run as kernels, or refused (and skipped) with the last reason.
    unsigned computeRun = 0, computeRefused = 0;
    std::string computeError;
    // The draws of the last batch that had any, and the centre pixel of the
    // last displayed image (its four bytes in texture order).
    std::string lastDraws;
    uint32_t centre = 0;
    std::string targetPixels; // the centre pixel of every colour target
    std::string samples;      // visibility results of the last batch's draws
    bool centreBGRA = false;
    bool nativeTextures;
    NSString* report() const {
        const unsigned* m = renderer.nativeMisses;
        NSString* native = nativeTextures ? [NSString stringWithFormat:
            @"\nTextures natives : %u dessins (%u images rendues par le GPU) · refus : %u sans T#, %u format, %u hors plage, %u alignement, %u vue Metal%@%s",
            renderer.nativeDraws, renderer.targetReads, m[0], m[1], m[2], m[3], m[4],
            renderer.nativeLastMiss.empty() ? @"" : @"\nDernier refus : ", renderer.nativeLastMiss.c_str()] : @"";
        return [NSString stringWithFormat:@"Metal : %u dessins, %u rendus, %u échecs, %u passes, %u buffers créés, %u réutilisés, %u grandes zones partagées (%u lues sans copie)\nIgnorés : %u dessins sans cible couleur · %zu cibles\nCalculs : %u reçus, %u exécutés, %u sans effet visible, %u refusés%s%s · %u kernels · %u images synchronisées\nPixel central affiché : R %u V %u B %u A %u\nPixel central des cibles (octets dans l'ordre de la texture) :%s\nÉchantillons dessinés par dessin (dernier lot) :%s\nDessins du dernier lot :%s\nProgrammes des premiers dessins :%s\nDernier ignoré : %s\nDernier calcul : %s\nAffichage : %u images copiées, %u sans cible Metal (dernière 0x%llx)\nDernier lot : CPU %.2f ms · GPU %@ ms · attente %.2f ms\nCopie image : CPU %.2f ms · GPU %@ ms%@",
            stats.draws, stats.rendered, stats.failed, renderer.renderPasses, renderer.buffersBuilt, renderer.buffersReused, renderer.rangesShared, renderer.rangesMapped,
            stats.skipped, renderer.targetCount(), stats.dispatches, computeRun - renderer.dispatchesWithoutEffect,
            renderer.dispatchesWithoutEffect, computeRefused,
            computeError.empty() ? "" : " : ", computeError.c_str(), renderer.kernelsBuilt, renderer.targetsSynced,
            (centre >> (centreBGRA ? 16 : 0)) & 0xff, (centre >> 8) & 0xff, (centre >> (centreBGRA ? 0 : 16)) & 0xff, centre >> 24, targetPixels.c_str(),
            samples.c_str(), lastDraws.empty() ? " aucun" : lastDraws.c_str(), renderer.programListings.c_str(), stats.lastSkipped.c_str(), stats.lastDispatch.c_str(), shown, absent, (unsigned long long)lastShown,
            encodeMs, gpuText(renderGpuMs), waitMs, copyMs, gpuText(copyGpuMs), native];
    }
    GameMetalState(id<MTLDevice> device, MaxPS4GameMemoryReader reader, bool nativeTextures)
        : memory{[reader](uint64_t a, void* d, size_t n) { return bool(reader(a,d,n)); }, {}},
          collector(memory, stats), renderer(device, memory, nil, 0, nativeTextures), device(device),
          queue([device newCommandQueue]), nativeTextures(nativeTextures) {}
};
}
@interface MaxPS4GameMetalBridge ()
@property(atomic, copy) NSString* lastReport;
@property(atomic, readwrite) double submitMilliseconds;
@end
@implementation MaxPS4GameMetalBridge {
    std::unique_ptr<GameMetalState> state_;
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader {
    return [self initWithDevice:device reader:reader nativeTextures:NO];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader nativeTextures:(BOOL)nativeTextures {
    return [self initWithDevice:device reader:reader view:nil nativeTextures:nativeTextures];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device reader:(MaxPS4GameMemoryReader)reader
                          view:(MaxPS4GameMemoryView)view nativeTextures:(BOOL)nativeTextures {
    if ((self = [super init])) {
        self.lastReport = @"Metal : préparation du premier rendu";
        state_ = std::make_unique<GameMetalState>(device, [reader copy], nativeTextures);
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
        // copies follow on the same queue. One submission at a time stays in
        // flight: the next one waits for it here, after the game's CPU work in
        // between, without the lock, so display copies go on meanwhile.
        id<MTLCommandBuffer> previous = nil;
        { std::lock_guard<std::mutex> peek(s.mutex); previous = s.inflight; }
        if (previous) [previous waitUntilCompleted];
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.error.empty()) return -EIO;
        if (previous) {
            s.inflight = nil;
            s.waitMs = elapsedMs(started);
            if (previous.status != MTLCommandBufferStatusCompleted || previous.error) {
                s.error = previous.error ? previous.error.localizedDescription.UTF8String : "Metal GPU completion failed";
                return -EIO;
            }
            if (gpuMs(previous) >= 0) s.renderGpuMs = gpuMs(previous);
        }
        const auto encodingStarted = Clock::now();
        id<MTLCommandBuffer> command = [s.queue commandBuffer];
        if (!command) { s.error = "Metal command buffer allocation"; return -ENOMEM; }
        const unsigned failed = s.stats.failed;
        const unsigned rendered = s.stats.rendered;
        s.renderer.beginBatch();
        s.collector.execute(words, count, [&](const PM4Draw& draw) {
            if (s.stats.failed != failed) return;
            if (s.renderer.encode(draw, command)) ++s.stats.rendered;
            else s.stats.fail(s.renderer.error);
        }, [&](const PM4Dispatch& dispatch) {
            // A dispatch that cannot run is skipped, as before compute support.
            if (s.stats.failed != failed) return;
            if (s.renderer.dispatch(dispatch, command)) ++s.computeRun;
            else { ++s.computeRefused; s.computeError = s.renderer.error; }
        });
        s.renderer.endBatch();
        if (!s.renderer.batchDraws.empty()) s.lastDraws = s.renderer.batchDraws;
        if (s.stats.failed != failed) { s.error = s.stats.error; return -ENOTSUP; }
        const double encoding = elapsedMs(encodingStarted);
        [command commit];
        if (s.stats.rendered != rendered) {
            s.encodeMs = encoding;
            s.inflight = command;
        }
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
    // The centre pixel of the displayed image, then of every colour target,
    // read back for the session report.
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
    [command commit];
    // The copy follows the game's submission on the GPU: wait without the
    // lock, so the game can encode its next one meanwhile.
    lock.unlock();
    [command waitUntilCompleted];
    lock.lock();
    if (command.status != MTLCommandBufferStatusCompleted || command.error) {
        s.error = "Metal display copy failed"; return nil;
    }
    s.copyMs = elapsedMs(started); s.copyGpuMs = gpuMs(command);
    if (pixel) {
        std::memcpy(&s.centre, pixel.contents, 4); s.centreBGRA = source.pixelFormat == MTLPixelFormatBGRA8Unorm;
        s.targetPixels.clear();
        s.samples.clear();
        if (id<MTLBuffer> v = s.renderer.lastVisibility) {
            const auto* counts = static_cast<const uint64_t*>(v.contents);
            for (unsigned k = 0; k < s.renderer.lastVisibilityCount; ++k) {
                char text[32];
                std::snprintf(text, sizeof text, " %llu", (unsigned long long)counts[k]);
                s.samples += text;
            }
        }
        const auto* bytes = static_cast<const uint8_t*>(pixel.contents);
        for (size_t k = 0; k < targets.size(); ++k) {
            char text[96];
            const uint8_t* p = bytes + 4 * (k + 1);
            std::snprintf(text, sizeof text, "\n  - 0x%llx %lux%lu %s : %u %u %u %u", (unsigned long long)targets[k].first,
                          (unsigned long)targets[k].second.width, (unsigned long)targets[k].second.height,
                          targets[k].second.pixelFormat == MTLPixelFormatBGRA8Unorm ? "BGRA" : "RGBA", p[0], p[1], p[2], p[3]);
            s.targetPixels += text;
        }
    }
    self.lastReport = s.report();
    return texture;
}
- (NSString*)report {
    // An immutable atomic snapshot: reading diagnostics must not wait for the
    // next submission's encoding, shader compilation or GPU completion.
    return self.lastReport;
}
- (NSString*)failure {
    auto& s = *state_;
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    return lock && !s.error.empty() ? [NSString stringWithUTF8String:s.error.c_str()] : nil;
}
@end
