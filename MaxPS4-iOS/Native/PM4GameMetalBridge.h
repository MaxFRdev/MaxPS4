#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
NS_ASSUME_NONNULL_BEGIN
// Reader is invoked only synchronously during submit, while the guest's
// submitting thread is paused. Neither reader nor submit may reenter GNM.
typedef BOOL (^MaxPS4GameMemoryReader)(uint64_t, void*, size_t);
// Writer is invoked only synchronously while the guest submission is paused.
typedef int32_t (^MaxPS4GameMemoryWriter)(uint64_t, const void*, size_t, uint64_t*);
// The host bytes backing guest memory (maxps4_fex_engine_gpu_view), or NULL:
// large textures become no-copy buffers the GPU reads in place.
typedef const void* _Nullable (^MaxPS4GameMemoryView)(uint64_t, size_t);
@interface MaxPS4GameMetalBridge : NSObject
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                reader:(MaxPS4GameMemoryReader)reader;
// Experimental: native Metal texel reads (PM4MetalRenderer nativeTextures).
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                reader:(MaxPS4GameMemoryReader)reader
                        nativeTextures:(BOOL)nativeTextures;
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                reader:(MaxPS4GameMemoryReader)reader
                                  view:(nullable MaxPS4GameMemoryView)view
                        nativeTextures:(BOOL)nativeTextures;
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                reader:(MaxPS4GameMemoryReader)reader
                                writer:(nullable MaxPS4GameMemoryWriter)writer
                                  view:(nullable MaxPS4GameMemoryView)view
                        nativeTextures:(BOOL)nativeTextures;
- (int32_t)submit:(const uint32_t*)words count:(size_t)count;
// Returns an owned display copy, cropped to VideoOut dimensions. If displayEvent
// is non-null, wait for displayEventValue before sampling it; nil if not drawn.
- (nullable id<MTLTexture>)copyTargetAtAddress:(uint64_t)address
                                       width:(uint32_t)width height:(uint32_t)height
    NS_SWIFT_NAME(copyTarget(address:width:height:));
// Shared event signalled after the latest display copy; nil on older devices.
@property(nonatomic, readonly, nullable) id<MTLSharedEvent> displayEvent;
@property(nonatomic, readonly) uint64_t displayEventValue;
@property(nonatomic, readonly, copy) NSString* report;
// Milliseconds the game's submitting thread has spent in submit so far
// (encoding, GPU and waiting), every submission included.
@property(atomic, readonly) double submitMilliseconds;
@property(nonatomic, readonly, copy, nullable) NSString* failure;
// New pipelines build on Metal's threads and their draws are skipped until
// ready, instead of the submission waiting for them. Set before submitting.
@property(nonatomic) BOOL asynchronousPipelines;
// Opt-in diagnostic: periodically commits and waits after every draw to report
// per-program GPU time. It is deliberately off during normal game execution.
@property(nonatomic) BOOL programProfiling;
// Number of draw-level GPU timing barriers performed by this bridge.
@property(nonatomic, readonly) uint64_t profiledDraws;
// Opt-in bounded frame-time distribution. It records CPU submission/encoding,
// queue-cap waits and completed-command GPU duration without adding a GPU wait.
@property(nonatomic) BOOL frameTimingProfiling;
@property(nonatomic, readonly, copy) NSString* frameTimingReport;
@end
NS_ASSUME_NONNULL_END
#endif
