#import "PM4GameMetalBridge.h"
#include "PM4MetalRenderer.h"
#include <cerrno>
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
    bool nativeTextures;
    NSString* report() const {
        const unsigned* m = renderer.nativeMisses;
        NSString* native = nativeTextures ? [NSString stringWithFormat:
            @"\nTextures natives : %u dessins (%u images rendues par le GPU) · refus : %u sans T#, %u format, %u hors plage, %u alignement, %u vue Metal%@%s",
            renderer.nativeDraws, renderer.targetReads, m[0], m[1], m[2], m[3], m[4],
            renderer.nativeLastMiss.empty() ? @"" : @"\nDernier refus : ", renderer.nativeLastMiss.c_str()] : @"";
        return [NSString stringWithFormat:@"Metal : %u dessins, %u rendus, %u échecs, %u passes, %u buffers créés, %u réutilisés\nIgnorés : %u dessins sans cible couleur, %u calculs (dispatch) · %zu cibles\nDernier ignoré : %s\nDernier calcul : %s\nAffichage : %u images copiées, %u sans cible Metal (dernière 0x%llx)\nDernier lot : CPU %.2f ms · GPU %@ ms · attente %.2f ms\nCopie image : CPU %.2f ms · GPU %@ ms%@",
            stats.draws, stats.rendered, stats.failed, renderer.renderPasses, renderer.buffersBuilt, renderer.buffersReused,
            stats.skipped, stats.dispatches, renderer.targetCount(), stats.lastSkipped.c_str(), stats.lastDispatch.c_str(), shown, absent, (unsigned long long)lastShown,
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
    if ((self = [super init])) {
        self.lastReport = @"Metal : préparation du premier rendu";
        state_ = std::make_unique<GameMetalState>(device, [reader copy], nativeTextures);
        if (!state_->queue) return nil;
    }
    return self;
}
- (int32_t)submit:(const uint32_t*)words count:(size_t)count {
    @autoreleasepool {
        auto& s = *state_;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.error.empty()) return -EIO;
        if (!words || !count || count > 65536) return -EINVAL;
        const auto started = Clock::now();
        // The game does not wait for its work's GPU completion: every byte a
        // draw reads is copied while encoding (upload buffers are reused only
        // once their command buffer has completed, constants go by value),
        // targets are GPU-only, and display copies follow on the same queue.
        // One submission at a time stays in flight: the next one waits for it
        // here, after the game's CPU work in between.
        if (s.inflight) {
            [s.inflight waitUntilCompleted];
            id<MTLCommandBuffer> previous = s.inflight;
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
        });
        s.renderer.endBatch();
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
    std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock || !s.error.empty() || !width || !height || width > 1920 || height > 1080) return nil;
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
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted || command.error) {
        s.error = "Metal display copy failed"; return nil;
    }
    s.copyMs = elapsedMs(started); s.copyGpuMs = gpuMs(command);
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
