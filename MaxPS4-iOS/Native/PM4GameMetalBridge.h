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
typedef int32_t (^MaxPS4GameMemoryWriter)(uint64_t, const void*, size_t);
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
// Returns an owned, immutable display copy, cropped to VideoOut dimensions.
// Waits for a submission being encoded; nil if the address was not drawn.
- (nullable id<MTLTexture>)copyTargetAtAddress:(uint64_t)address
                                       width:(uint32_t)width height:(uint32_t)height
    NS_SWIFT_NAME(copyTarget(address:width:height:));
@property(nonatomic, readonly, copy) NSString* report;
// Milliseconds the game's submitting thread has spent in submit so far
// (encoding, GPU and waiting), every submission included.
@property(atomic, readonly) double submitMilliseconds;
@property(nonatomic, readonly, copy, nullable) NSString* failure;
// New pipelines build on Metal's threads and their draws are skipped until
// ready, instead of the submission waiting for them. Set before submitting.
@property(nonatomic) BOOL asynchronousPipelines;
@end
NS_ASSUME_NONNULL_END
#endif
