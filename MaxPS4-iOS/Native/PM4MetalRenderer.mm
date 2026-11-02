#import "PM4MetalRenderer.h"
#include "GCNTranslator.hpp"
#include "PM4ResourceScan.hpp"
#include "PM4Blend.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace maxps4::gpu {
namespace {
// Must match GCNDrawState in gcnMetalPrelude().
struct DrawState { std::uint64_t base, size; std::uint32_t vsUser[16], psUser[17]; std::uint32_t flags, rangeCount; std::uint64_t ranges[48]; };
static_assert(sizeof(DrawState) == 544, "GCNDrawState layout");
// Must match GCNDispatchState in gcnMetalComputeEntry().
struct DispatchState { std::uint64_t base, size; std::uint32_t user[16], userCount, rsrc2, rangeCount, pad; std::uint64_t ranges[48]; };
static_assert(sizeof(DispatchState) == 480, "GCNDispatchState layout");
constexpr std::uint32_t rectList = 1, flipY = 2;

// CB_BLEND factor codes to Metal; dual-source factors remain unsupported.
bool blendFactor(unsigned code, MTLBlendFactor& out) {
    static const MTLBlendFactor factors[] = {
        MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
        MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha,
        MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor};
    if (code < sizeof(factors) / sizeof(factors[0])) { out = factors[code]; return true; }
    switch (code) {
    case 10: out = MTLBlendFactorSourceAlphaSaturated; return true;
    case 13: out = MTLBlendFactorBlendColor; return true;
    case 14: out = MTLBlendFactorOneMinusBlendColor; return true;
    case 19: out = MTLBlendFactorBlendAlpha; return true;
    case 20: out = MTLBlendFactorOneMinusBlendAlpha; return true;
    default: return false;
    }
}
bool blendChannel(PM4BlendChannel channel, MTLBlendOperation& operation,
                  MTLBlendFactor& source, MTLBlendFactor& destination) {
    if (!channel.supported()) return false;
    static const MTLBlendOperation operations[] = {MTLBlendOperationAdd, MTLBlendOperationSubtract,
        MTLBlendOperationMin, MTLBlendOperationMax, MTLBlendOperationReverseSubtract};
    operation = operations[channel.operation];
    if (channel.ignoresFactors()) { source = destination = MTLBlendFactorOne; return true; }
    return blendFactor(channel.source, source) && blendFactor(channel.destination, destination);
}
std::string inputKey(const PM4Draw& d) {
    std::string key(reinterpret_cast<const char*>(d.inputCntl), sizeof d.inputCntl);
    return key;
}
}

id<MTLRenderPipelineState> PM4MetalRenderer::pipeline(const PM4Draw& d, MTLPixelFormat format) {
    const PipelineKey key{d.vs, d.ps, d.vsUser[0], d.vsUser[1], d.psInputAddr, inputKey(d), unsigned(format),
                          (d.blend >> 30) & 1 ? d.blend : 0u, d.mask};
    const auto found = pipelines_.find(key);
    if (found != pipelines_.end()) return found->second;
    GCNTranslateOptions vsOptions;
    vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = d.vsUserCount;
    for (unsigned k = 0; k < 16; ++k) vsOptions.userData[k] = d.vsUser[k];
    GCNTranslateOptions psOptions;
    psOptions.name = "ps_program"; psOptions.userCount = d.psUserCount;
    for (unsigned k = 0; k < 16; ++k) psOptions.userData[k] = d.psUser[k];
    const auto vs = translateGCN(code_, d.vs, vsOptions);
    if (!vs) { fail("VS translation: " + vs.error); return nil; }
    const auto ps = translateGCN(code_, d.ps, psOptions);
    if (!ps) { fail("PS translation: " + ps.error); return nil; }
    const std::string source = gcnMetalPrelude(native_, gcnHighestUsed(vs.params), gcnHighestUsed(ps.attributes), gcnHighestUsed(ps.colors)) + vs.source + ps.source +
                               gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", d.psInputAddr, d.inputCntl, native_);
    id<MTLLibrary> library = this->library(source);
    if (!library) return nil;
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    NSError* failure = nil;
    descriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"gcn_fragment"];
    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
    color.pixelFormat = format;
    color.writeMask = ((d.mask & 1) ? MTLColorWriteMaskRed : 0) | ((d.mask & 2) ? MTLColorWriteMaskGreen : 0) |
                      ((d.mask & 4) ? MTLColorWriteMaskBlue : 0) | ((d.mask & 8) ? MTLColorWriteMaskAlpha : 0);
    if ((d.blend >> 30) & 1) {
        const auto blend = decodePM4Blend(d.blend);
        auto alpha = blend.alpha;
        // Saturation has an alpha factor of one, independently of RGB.
        if (alpha.source == 10) alpha.source = 1;
        if (alpha.destination == 10) alpha.destination = 1;
        MTLBlendFactor cs, cd, as, ad;
        MTLBlendOperation colorOperation, alphaOperation;
        if (!blendChannel(blend.rgb, colorOperation, cs, cd) ||
            !blendChannel(alpha, alphaOperation, as, ad)) {
            fail("blend mode");
            return nil;
        }
        color.blendingEnabled = YES;
        color.rgbBlendOperation = colorOperation; color.alphaBlendOperation = alphaOperation;
        color.sourceRGBBlendFactor = cs; color.destinationRGBBlendFactor = cd;
        color.sourceAlphaBlendFactor = as; color.destinationAlphaBlendFactor = ad;
    }
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
    if (!state) { fail(std::string("pipeline: ") + failure.localizedDescription.UTF8String); return nil; }
    ++pipelinesBuilt;
    pipelines_[key] = state;
    return state;
}

id<MTLLibrary> PM4MetalRenderer::library(const std::string& source) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    NSError* failure = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&failure];
    if (!library) fail(std::string("MSL: ") + failure.localizedDescription.UTF8String);
    return library;
}

id<MTLComputePipelineState> PM4MetalRenderer::kernel(const PM4Dispatch& c) {
    const auto key = std::make_pair(c.program, c.rsrc2);
    const auto found = kernels_.find(key);
    if (found != kernels_.end()) return found->second;
    GCNTranslateOptions options;
    options.stage = GCNStage::compute; options.name = "cs_program"; options.userCount = c.userCount;
    for (unsigned k = 0; k < 16; ++k) options.userData[k] = c.user[k];
    const auto cs = translateGCN(code_, c.program, options);
    if (!cs) { fail("CS translation: " + cs.error); return nil; }
    id<MTLLibrary> library = this->library(gcnMetalPrelude(native_, 1, 1, 1) + cs.source + gcnMetalComputeEntry("cs_program"));
    if (!library) return nil;
    NSError* failure = nil;
    id<MTLComputePipelineState> state = [device_ newComputePipelineStateWithFunction:[library newFunctionWithName:@"gcn_compute"]
                                                                               error:&failure];
    if (!state) { fail(std::string("compute pipeline: ") + failure.localizedDescription.UTF8String); return nil; }
    ++kernelsBuilt;
    kernels_[key] = state;
    return state;
}

bool PM4MetalRenderer::dispatch(const PM4Dispatch& c, id<MTLCommandBuffer> commands) {
    error.clear();
    if (!c.program || !c.groups[0] || !c.groups[1] || !c.groups[2]) return true; // nothing to run
    const std::uint64_t tx = std::max(1u, c.threads[0]), ty = std::max(1u, c.threads[1]), tz = std::max(1u, c.threads[2]);
    // Copies of the ranges the program addresses, as for a draw (copy mode only).
    if (guest_) return fail("compute with a guest memory window");
    const auto& ranges = scanner_.scan(c);
    if (ranges.empty()) return fail("compute program addresses no memory");
    if (ranges.size() > 16) return fail("compute reads more than 16 memory ranges");
    auto stored = [&](std::uint64_t address, std::uint64_t length) {
        for (const auto& s : scanner_.stored()) if (address < s.base + s.size && s.base < address + length) return true;
        return false;
    };
    // Linear 8_8_8_8 images the kernel stores that no target holds yet.
    struct Output { std::uint64_t base; unsigned pitch, height; };
    std::vector<Output> outputs;
    for (const auto& t : scanner_.images()) {
        const std::uint64_t base = (std::uint64_t(t[0]) | std::uint64_t(t[1] & 0xff) << 32) << 8;
        const unsigned dfmt = (t[1] >> 20) & 63, tiling = (t[3] >> 20) & 31;
        const unsigned pitch = ((t[4] >> 13) & 0x3fff) + 1, height = ((t[2] >> 14) & 0x3fff) + 1;
        if (dfmt == 10 && tiling == 8 && !targets_.count(base) && stored(base, std::uint64_t(pitch) * height * 4))
            outputs.push_back({base, pitch, height});
    }
    // Only stores to targets and new images are kept (guest memory never
    // receives the rest): a kernel storing nothing else changes nothing.
    bool visible = !outputs.empty();
    for (const auto& [address, target] : targets_)
        if (target.texture && stored(address, std::uint64_t(target.texture.width) * 4 * target.texture.height)) visible = true;
    if (!visible) { ++dispatchesWithoutEffect; return true; }
    id<MTLComputePipelineState> state = kernel(c);
    if (!state) return false;
    if (tx * ty * tz > state.maxTotalThreadsPerThreadgroup) return fail("threads per group");
    std::uint64_t total = 0;
    for (const auto& r : ranges) total += r.size + 255;
    if (total > (256ull << 20)) return fail("compute reads more than 256 MiB");
    id<MTLBuffer> memory = uploadBuffer(std::max<std::uint64_t>(total, 256), commands);
    if (!memory) return fail("compute memory buffer");
    DispatchState dispatchState{};
    std::uint64_t offset = 0;
    auto* bytes = static_cast<std::uint8_t*>(memory.contents);
    for (std::size_t k = 0; k < ranges.size(); ++k) {
        offset += (ranges[k].base - offset) & 255;
        copyGuest(ranges[k].base, bytes + offset, ranges[k].size);
        dispatchState.ranges[3 * k] = ranges[k].base;
        dispatchState.ranges[3 * k + 1] = ranges[k].size;
        dispatchState.ranges[3 * k + 2] = offset;
        offset += ranges[k].size;
    }
    dispatchState.rangeCount = std::uint32_t(ranges.size());
    dispatchState.userCount = c.userCount; dispatchState.rsrc2 = c.rsrc2;
    for (unsigned k = 0; k < 16; ++k) dispatchState.user[k] = c.user[k];
    // Where [address, address + length) lies in `memory`, if inside one range.
    auto locate = [&](std::uint64_t address, std::uint64_t length, std::uint64_t& at) {
        for (std::size_t k = 0; k < ranges.size(); ++k)
            if (address >= ranges[k].base && address - ranges[k].base <= ranges[k].size && length <= ranges[k].size - (address - ranges[k].base)) {
                at = dispatchState.ranges[3 * k + 2] + (address - ranges[k].base);
                return true;
            }
        return false;
    };
    if (encoder_) { [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; }
    // Colour targets live in their textures: the kernel reads their current contents.
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!blit) return fail("compute blit encoder");
    for (auto& [address, target] : targets_) {
        std::uint64_t at = 0;
        const std::uint64_t row = target.texture.width * 4, length = row * target.texture.height;
        if (!target.texture || target.fresh || !locate(address, length, at)) continue;
        [blit copyFromTexture:target.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(target.texture.width, target.texture.height, 1)
                     toBuffer:memory destinationOffset:at destinationBytesPerRow:row destinationBytesPerImage:length];
    }
    [blit endEncoding];
    id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
    if (!compute) return fail("compute encoder");
    [compute setComputePipelineState:state];
    [compute setBuffer:memory offset:0 atIndex:0];
    [compute setBytes:&dispatchState length:sizeof dispatchState atIndex:1];
    [compute dispatchThreadgroups:MTLSizeMake(c.groups[0], c.groups[1], c.groups[2])
            threadsPerThreadgroup:MTLSizeMake(tx, ty, tz)];
    [compute endEncoding];
    for (const auto& o : outputs) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:o.pitch height:o.height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
        if (texture) targets_[o.base] = {texture, false};
    }
    // What the kernel stored in targets goes back to their textures.
    blit = [commands blitCommandEncoder];
    if (!blit) return fail("compute blit encoder");
    for (auto& [address, target] : targets_) {
        std::uint64_t at = 0;
        const std::uint64_t row = target.texture.width * 4, length = row * target.texture.height;
        if (!target.texture || !stored(address, length) || !locate(address, length, at)) continue;
        [blit copyFromBuffer:memory sourceOffset:at sourceBytesPerRow:row sourceBytesPerImage:length
                  sourceSize:MTLSizeMake(target.texture.width, target.texture.height, 1)
                   toTexture:target.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        target.fresh = false;
        ++targetsSynced;
    }
    [blit endEncoding];
    return true;
}

void PM4MetalRenderer::listProgram(const char* stage, std::uint64_t address, const std::uint32_t* user, unsigned userCount) {
    if (listed_.size() >= 9 || std::find(listed_.begin(), listed_.end(), address) != listed_.end()) return;
    listed_.push_back(address);
    std::vector<std::uint32_t> words;
    for (std::uint32_t word = 0; words.size() < 1024 && code_.read(address + 4 * words.size(), &word, 4);) words.push_back(word);
    // Up to s_endpgm, or s_setpc_b64 (a fetch shader's return).
    std::vector<GCNInstruction> code;
    for (std::size_t at = 0; at < words.size() && code.size() < 120;) {
        GCNInstruction i;
        if (decodeGCNInstruction(words.data() + at, words.size() - at, at, i) != GCNError::none) break;
        code.push_back(i);
        at += i.size;
        if ((i.encoding == GCNEncoding::sopp && i.opcode == 1) || (i.encoding == GCNEncoding::sop1 && i.opcode == 32)) break;
    }
    char text[160];
    std::snprintf(text, sizeof text, "\n%s 0x%llx, %zu instructions, SGPR utilisateur :", stage, (unsigned long long)address, code.size());
    programListings += text;
    for (unsigned k = 0; k < userCount && k < 16; ++k) { std::snprintf(text, sizeof text, " %x", user[k]); programListings += text; }
    for (const auto& i : code) programListings += "\n    " + gcnDisassemble(i);
}

void PM4MetalRenderer::endBatch() {
    if (visibility_ && visibilityCount_) { lastVisibility = visibility_; lastVisibilityCount = visibilityCount_; }
    visibility_ = nil; visibilityCount_ = 0;
    [encoder_ endEncoding];
    encoder_ = nil; commands_ = nil; attachment_ = nil; batching_ = false;
}

void PM4MetalRenderer::copyGuest(std::uint64_t address, std::uint8_t* out, std::uint64_t size) const {
    if (code_.read(address, out, size)) return;
    // A descriptor's extent may run past mapped memory (a constant buffer
    // declaring 64 KiB of which the program reads 64 bytes): keep every
    // readable page instead of zeroing the whole range.
    constexpr std::uint64_t page = 4096;
    for (std::uint64_t done = 0; done < size;) {
        const std::uint64_t chunk = std::min(size - done, page - (address + done) % page);
        if (!code_.read(address + done, out + done, chunk)) std::memset(out + done, 0, chunk);
        done += chunk;
    }
}

id<MTLBuffer> PM4MetalRenderer::uploadBuffer(std::size_t bytes, id<MTLCommandBuffer> commands) {
    // Each draw keeps its own immutable snapshot until its command buffer has
    // finished. In particular, two draws in the same batch cannot share a slot.
    Upload* best = nullptr;
    for (auto& upload : uploads_) {
        const auto status = upload.owner.status;
        if ((status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError) &&
            upload.buffer.length >= bytes && (!best || upload.buffer.length < best->buffer.length)) best = &upload;
    }
    if (best) {
        best->owner = commands;
        ++buffersReused;
        return best->buffer;
    }
    id<MTLBuffer> buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer) {
        ++buffersBuilt;
        constexpr std::uint64_t budget = 64ull << 20;
        if (uploads_.size() < 256 && bytes <= budget - pooledBytes_) {
            uploads_.push_back({buffer, commands});
            pooledBytes_ += bytes;
        }
    }
    return buffer;
}

id<MTLTexture> PM4MetalRenderer::nativeTexture(const std::array<std::uint32_t, 8>& t, id<MTLBuffer> memory,
                                               const std::uint64_t* ranges, unsigned rangeCount, id<MTLTexture> drawing) {
    // dfmt 8_8_8_8 or 5_6_5 (gcn_texel_at reads both as unorm whatever the nfmt), linear tiling.
    const unsigned dfmt = (t[1] >> 20) & 63;
    const std::uint64_t base = (std::uint64_t(t[0]) | std::uint64_t(t[1] & 0xff) << 32) << 8;
    const std::uint64_t width = (t[2] & 0x3fff) + 1, height = ((t[2] >> 14) & 0x3fff) + 1, pitch = ((t[4] >> 13) & 0x3fff) + 1;
    auto miss = [&](unsigned reason, std::uint64_t detail) -> id<MTLTexture> {
        ++nativeMisses[reason];
        char text[160];
        std::snprintf(text, sizeof text, "dfmt %u tiling %u %llux%llu pitch %llu base 0x%llx (%llu)", dfmt, (t[3] >> 20) & 31,
                      (unsigned long long)width, (unsigned long long)height, (unsigned long long)pitch,
                      (unsigned long long)base, (unsigned long long)detail);
        nativeLastMiss = text;
        return nil;
    };
    // A colour target lives only in its Metal texture, never written back to
    // guest memory: an 8_8_8_8 T# at its address reads it there (an emulator
    // renders its console's frame, then draws that frame scaled on screen).
    if (dfmt == 10) {
        const auto rendered = targets_.find(base);
        id<MTLTexture> texture = rendered == targets_.end() ? nil : rendered->second.texture;
        if (texture && texture != drawing && !rendered->second.fresh && texture.width >= width && texture.height >= height) {
            ++targetReads;
            return texture;
        }
    }
    if ((dfmt != 10 && dfmt != 16) || ((t[3] >> 20) & 31) != 8 || pitch < width) return miss(1, 0);
    const MTLPixelFormat format = dfmt == 10 ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatB5G6R5Unorm;
    const std::uint64_t rowBytes = pitch * (dfmt == 10 ? 4 : 2), bytes = rowBytes * height;
    std::uint64_t offset = 0;
    bool inside = false;
    if (!rangeCount) {
        inside = base >= guestBase_ && base - guestBase_ <= memory.length && bytes <= memory.length - (base - guestBase_);
        offset = base - guestBase_;
    }
    for (unsigned k = 0; k < rangeCount && !inside; ++k) {
        const std::uint64_t start = ranges[3 * k], size = ranges[3 * k + 1];
        inside = base >= start && base - start <= size && bytes <= size - (base - start);
        offset = ranges[3 * k + 2] + (base - start);
    }
    if (!inside) return miss(2, bytes);
    const NSUInteger alignment = [device_ minimumLinearTextureAlignmentForPixelFormat:format];
    if (!alignment || offset % alignment || rowBytes % alignment) return miss(3, alignment ? offset % alignment : 0);
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:width height:height mipmapped:NO];
    descriptor.storageMode = memory.storageMode;
    descriptor.cpuCacheMode = memory.cpuCacheMode;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> view = [memory newTextureWithDescriptor:descriptor offset:offset bytesPerRow:rowBytes];
    return view ? view : miss(4, alignment);
}

bool PM4MetalRenderer::encode(const PM4Draw& d, id<MTLCommandBuffer> commands) {
    error.clear();
    if (d.swap > 1) return fail("colour target component swap " + std::to_string(d.swap));
    if ((d.vte & 0x3f) != 0x3f) return fail("viewport without scale and offset");
    const MTLPixelFormat format = d.swap == 1 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    if (d.scissor[2] <= d.scissor[0] || d.scissor[3] <= d.scissor[1]) return true; // nothing visible
    id<MTLRenderPipelineState> state = pipeline(d, format);
    if (!state) return false;
    DrawState drawState{};
    id<MTLBuffer> memory = guest_;
    if (memory) {
        drawState.base = guestBase_; drawState.size = memory.length;
        if (native_) scanner_.scan(d); // only for its T#s
    } else {
        // Copies of the ranges this draw's programs read. Each starts at a buffer
        // offset congruent to its guest address modulo 256, so guest alignment
        // (up to 256 bytes) holds in the buffer: a T# base inside a merged range
        // stays aligned for a native texture view.
        const auto& ranges = scanner_.scan(d);
        if (ranges.size() > 16) return fail("more than 16 memory ranges");
        std::uint64_t total = 0;
        for (const auto& r : ranges) total += r.size + 255;
        if (total > (256ull << 20)) return fail("draw reads more than 256 MiB");
        memory = uploadBuffer(std::max<std::uint64_t>(total, 256), commands);
        if (!memory) return fail("memory buffer");
        std::uint64_t offset = 0;
        auto* bytes = static_cast<std::uint8_t*>(memory.contents);
        for (std::size_t k = 0; k < ranges.size(); ++k) {
            offset += (ranges[k].base - offset) & 255;
            // An unreadable range stays zero rather than exposing anything else.
            copyGuest(ranges[k].base, bytes + offset, ranges[k].size);
            drawState.ranges[3 * k] = ranges[k].base;
            drawState.ranges[3 * k + 1] = ranges[k].size;
            drawState.ranges[3 * k + 2] = offset;
            offset += ranges[k].size;
        }
        drawState.rangeCount = std::uint32_t(ranges.size());
        lastRanges = drawState.rangeCount;
    }
    Target& target = targets_[d.target];
    if (!target.texture || target.texture.width != d.pitch || target.texture.height != d.height ||
        target.texture.pixelFormat != format) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:d.pitch height:d.height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        // These colour targets are rendered, sampled and copied only by the
        // GPU. CPU-visible shared storage is unnecessary for this path.
        descriptor.storageMode = MTLStorageModePrivate;
        target = {[device_ newTextureWithDescriptor:descriptor], true};
        if (!target.texture) return fail("colour target texture");
    }
    if (encoder_ && (commands_ != commands || attachment_ != target.texture)) {
        [encoder_ endEncoding]; encoder_ = nil;
    }
    id<MTLRenderCommandEncoder> encoder = encoder_;
    if (!encoder) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target.texture;
    pass.colorAttachments[0].loadAction = target.fresh ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (batching_) pass.visibilityResultBuffer = visibility_;
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return fail("render encoder");
    target.fresh = false;
    ++renderPasses;
    if (batching_) { encoder_ = encoder; commands_ = commands; attachment_ = target.texture; }
    }
    [encoder setRenderPipelineState:state];
    [encoder setBlendColorRed:pm4BlendConstant(d.blendConstant[0]) green:pm4BlendConstant(d.blendConstant[1])
                        blue:pm4BlendConstant(d.blendConstant[2]) alpha:pm4BlendConstant(d.blendConstant[3])];
    // PA_CL_VPORT: x = xscale * ndc + xoffset. A positive Y scale points NDC +1 down: flip it.
    const float xs = d.viewport[0], xo = d.viewport[1], ys = d.viewport[2], yo = d.viewport[3];
    MTLViewport viewport;
    viewport.originX = xo - xs; viewport.width = 2 * xs;
    viewport.originY = yo - std::fabs(ys); viewport.height = 2 * std::fabs(ys);
    viewport.znear = d.viewport[5]; viewport.zfar = d.viewport[5] + d.viewport[4];
    [encoder setViewport:viewport];
    [encoder setScissorRect:MTLScissorRect{d.scissor[0], d.scissor[1], d.scissor[2] - d.scissor[0], d.scissor[3] - d.scissor[1]}];
    [encoder setCullMode:MTLCullModeNone];
    for (unsigned k = 0; k < 16; ++k) { drawState.vsUser[k] = d.vsUser[k]; drawState.psUser[k] = d.psUser[k]; }
    drawState.flags = (d.primitive == 0x11 ? rectList : 0) | (ys > 0 ? flipY : 0);
    [encoder setVertexBuffer:memory offset:0 atIndex:0];
    [encoder setFragmentBuffer:memory offset:0 atIndex:0];
    [encoder setVertexBytes:&drawState length:sizeof drawState atIndex:1];
    [encoder setFragmentBytes:&drawState length:sizeof drawState atIndex:1];
    if (native_) {
        // GCNNativeTexture at buffer(3), always bound: `enabled` 0 means none.
        if (!nativeArguments_) {
            auto argument = [](MTLDataType type, NSUInteger index) {
                MTLArgumentDescriptor* a = [MTLArgumentDescriptor argumentDescriptor];
                a.dataType = type; a.index = index;
                if (type == MTLDataTypeTexture) a.textureType = MTLTextureType2D;
                return a;
            };
            nativeArguments_ = [device_ newArgumentEncoderWithArguments:@[argument(MTLDataTypeTexture, 0), argument(MTLDataTypeUInt, 1),
                                                                          argument(MTLDataTypeUInt4, 2), argument(MTLDataTypeUInt4, 3)]];
        }
        id<MTLTexture> image = nil;
        std::array<std::uint32_t, 8> descriptor{};
        if (scanner_.images().empty()) ++nativeMisses[0];
        for (const auto& t : scanner_.images())
            if ((image = nativeTexture(t, memory, drawState.ranges, drawState.rangeCount, target.texture))) { descriptor = t; break; }
        id<MTLBuffer> arguments = nativeArguments_ ? uploadBuffer(nativeArguments_.encodedLength, commands) : nil;
        if (!arguments) {
            if (!batching_) [encoder endEncoding];
            return fail("native texture arguments");
        }
        [nativeArguments_ setArgumentBuffer:arguments offset:0];
        [nativeArguments_ setTexture:image atIndex:0];
        *static_cast<std::uint32_t*>([nativeArguments_ constantDataAtIndex:1]) = image ? 1 : 0;
        std::memcpy([nativeArguments_ constantDataAtIndex:2], descriptor.data(), 16);
        std::memcpy([nativeArguments_ constantDataAtIndex:3], descriptor.data() + 4, 16);
        [encoder setVertexBuffer:arguments offset:0 atIndex:3];
        [encoder setFragmentBuffer:arguments offset:0 atIndex:3];
        if (image) {
            [encoder useResource:image usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            ++nativeDraws;
        }
    }
    const std::size_t bytes = d.indices.size() * sizeof(std::uint32_t);
    if (bytes <= 4096) [encoder setVertexBytes:d.indices.data() length:bytes atIndex:2];
    else {
        id<MTLBuffer> indices = uploadBuffer(bytes, commands);
        if (!indices) {
            if (!batching_) [encoder endEncoding];
            return fail("index buffer");
        }
        std::memcpy(indices.contents, d.indices.data(), bytes);
        [encoder setVertexBuffer:indices offset:0 atIndex:2];
    }
    const NSUInteger count = d.primitive == 0x11 ? NSUInteger(d.indices.size() / 3 * 6) : NSUInteger(d.indices.size());
    const bool counted = batching_ && visibility_ && visibilityCount_ < 64;
    if (counted) [encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:8 * visibilityCount_++];
    [encoder drawPrimitives:d.primitive == 6 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count];
    if (counted) [encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    if (!batching_) [encoder endEncoding];
    if (batchDraws.size() < 4000) {
        char text[256];
        std::snprintf(text, sizeof text, "\n  - cible 0x%llx %ux%u (swap %u, masque 0x%x, blend 0x%x), %zu sommets, ciseaux %u,%u-%u,%u, PS 0x%llx",
                      (unsigned long long)d.target, d.pitch, d.height, d.swap, d.mask, d.blend, d.indices.size(),
                      d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3], (unsigned long long)d.ps);
        batchDraws += text;
        // The first vertex as the interpreter computes it (position, parameters).
        if (!scanner_.vertexRan()) batchDraws += ", VS interpreté : " + scanner_.vertexError();
        for (const auto& e : scanner_.vertexExports()) {
            if (e[0] != 12 && !(e[0] >= 32 && e[0] < 36)) continue;
            std::snprintf(text, sizeof text, ", %s%u (%g %g %g %g)", e[0] == 12 ? "pos" : "param", e[0] == 12 ? 0u : e[0] - 32,
                          gcn_detail::f32(e[1]), gcn_detail::f32(e[2]), gcn_detail::f32(e[3]), gcn_detail::f32(e[4]));
            batchDraws += text;
        }
        std::snprintf(text, sizeof text, ", primitive %u, viewport %g %g %g %g %g %g, vte 0x%x", d.primitive, d.viewport[0], d.viewport[1],
                      d.viewport[2], d.viewport[3], d.viewport[4], d.viewport[5], d.vte);
        batchDraws += text;
        std::snprintf(text, sizeof text, ", entrees PS 0x%x (attr0 0x%x, attr1 0x%x)", d.psInputAddr, d.inputCntl[0], d.inputCntl[1]);
        batchDraws += text;
        listProgram("VS", d.vs, d.vsUser, d.vsUserCount);
        if (d.vsUserCount >= 2) listProgram("Fetch", std::uint64_t(d.vsUser[0]) | std::uint64_t(d.vsUser[1]) << 32, d.vsUser, 0);
        listProgram("PS", d.ps, d.psUser, d.psUserCount);
        if (native_) {
            unsigned shown = 0;
            for (const auto& t : scanner_.images()) {
                if (shown++ == 2) break;
                const std::uint64_t base = (std::uint64_t(t[0]) | std::uint64_t(t[1] & 0xff) << 32) << 8;
                std::snprintf(text, sizeof text, ", texture dfmt %u nfmt %u %ux%u tuilage %u 0x%llx%s", (t[1] >> 20) & 63, (t[1] >> 26) & 15,
                              (t[2] & 0x3fff) + 1, ((t[2] >> 14) & 0x3fff) + 1, (t[3] >> 20) & 31, (unsigned long long)base,
                              targets_.count(base) ? " (rendue GPU)" : "");
                batchDraws += text;
            }
        }
    }
    return true;
}

id<MTLTexture> PM4MetalRenderer::target(std::uint64_t address) const {
    const auto found = targets_.find(address);
    return found == targets_.end() ? nil : found->second.texture;
}
} // namespace maxps4::gpu
