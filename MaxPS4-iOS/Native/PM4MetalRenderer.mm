#import "PM4MetalRenderer.h"
#include "GCNTranslator.hpp"
#include "PM4ResourceScan.hpp"
#include "PM4Blend.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unistd.h>
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
    if (asyncPipelines) {
        std::lock_guard<std::mutex> lock(async_->mutex);
        const auto ready = async_->ready.find(key);
        if (ready != async_->ready.end()) {
            id<MTLRenderPipelineState> built = ready->second;
            async_->ready.erase(ready);
            // A pipeline Metal refused stays deferred: its draws are skipped.
            if (built) { ++pipelinesBuilt; pipelines_[key] = built; return built; }
            async_->refused.insert(key);
        }
        if (async_->pending.count(key) || async_->refused.count(key)) { deferred_ = true; return nil; }
    }
    GCNTranslateOptions vsOptions;
    vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = d.vsUserCount;
    for (unsigned k = 0; k < 16; ++k) vsOptions.userData[k] = d.vsUser[k];
    GCNTranslateOptions psOptions;
    psOptions.name = "ps_program"; psOptions.userCount = d.psUserCount;
    for (unsigned k = 0; k < 16; ++k) psOptions.userData[k] = d.psUser[k];
    compiling = 3;
    const auto vs = translateGCN(code_, d.vs, vsOptions);
    const auto ps = translateGCN(code_, d.ps, psOptions);
    compiling = 0;
    if (!vs) { fail("VS translation: " + vs.error); return nil; }
    if (!ps) { fail("PS translation: " + ps.error); return nil; }
    const std::string source = gcnMetalPrelude(native_, gcnHighestUsed(vs.params), gcnHighestUsed(ps.attributes), gcnHighestUsed(ps.colors)) + vs.source + ps.source +
                               gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", d.psInputAddr, d.inputCntl, native_);
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    NSError* failure = nil;
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
    if (asyncPipelines) {
        // Metal compiles on its own threads: this submission and the next go
        // on without the draws (a new screen's large programs took the iPhone
        // over 10 s each, the game's image frozen meanwhile).
        auto async = async_;
        { std::lock_guard<std::mutex> lock(async->mutex);
          async->pending[key] = {std::chrono::steady_clock::now(), "MSL compilation"}; }
        auto finish = [async, key](id<MTLRenderPipelineState> state, NSError* error) {
            std::lock_guard<std::mutex> lock(async->mutex);
            async->pending.erase(key);
            async->ready[key] = state;
            if (!state) async->lastError = error ? error.localizedDescription.UTF8String : "unknown";
        };
        id<MTLDevice> device = device_;
        [device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:compileOptions()
                    completionHandler:^(id<MTLLibrary> library, NSError* error) {
            if (!library) { finish(nil, error); return; }
            { std::lock_guard<std::mutex> lock(async->mutex);
              const auto pending = async->pending.find(key);
              if (pending != async->pending.end()) pending->second.stage = "render pipeline creation"; }
            descriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
            descriptor.fragmentFunction = [library newFunctionWithName:@"gcn_fragment"];
            [device newRenderPipelineStateWithDescriptor:descriptor completionHandler:^(id<MTLRenderPipelineState> state, NSError* failed) {
                finish(state, failed);
            }];
        }];
        deferred_ = true;
        return nil;
    }
    id<MTLLibrary> library = this->library(source);
    if (!library) return nil;
    descriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"gcn_fragment"];
    compiling = 2;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
    compiling = 0;
    if (!state) { fail(std::string("pipeline: ") + failure.localizedDescription.UTF8String); return nil; }
    ++pipelinesBuilt;
    pipelines_[key] = state;
    return state;
}

std::string PM4MetalRenderer::pipelineStatus() const {
    std::lock_guard<std::mutex> lock(async_->mutex);
    if (async_->pending.empty()) return {};
    const auto oldest = std::min_element(async_->pending.begin(), async_->pending.end(),
        [](const auto& a, const auto& b) { return a.second.since < b.second.since; });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - oldest->second.since).count();
    char text[192];
    std::snprintf(text, sizeof text, " · plus ancien : %s %.1f s, VS 0x%llx / PS 0x%llx",
                  oldest->second.stage, seconds, (unsigned long long)std::get<0>(oldest->first),
                  (unsigned long long)std::get<1>(oldest->first));
    return text;
}

MTLCompileOptions* PM4MetalRenderer::compileOptions() {
    MTLCompileOptions* options = [MTLCompileOptions new];
    if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    return options;
}

id<MTLLibrary> PM4MetalRenderer::library(const std::string& source) {
    NSError* failure = nil;
    compiling = 1;
    id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:compileOptions() error:&failure];
    compiling = 0;
    if (!library) fail(std::string("MSL: ") + failure.localizedDescription.UTF8String);
    return library;
}

id<MTLComputePipelineState> PM4MetalRenderer::kernel(const PM4Dispatch& c) {
    const auto key = std::make_pair(c.program, c.rsrc2);
    const auto found = kernels_.find(key);
    if (found != kernels_.end()) return found->second;
    GCNTranslateOptions options;
    options.stage = GCNStage::compute; options.name = "cs_program"; options.userCount = c.userCount;
    options.maxInstructions = 1024;
    for (unsigned k = 0; k < 16; ++k) options.userData[k] = c.user[k];
    compiling = 3;
    const auto cs = translateGCN(code_, c.program, options);
    compiling = 0;
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
    lastDispatchNote_.clear();
    if (!c.program || !c.groups[0] || !c.groups[1] || !c.groups[2]) return true; // nothing to run
    const std::uint64_t tx = std::max(1u, c.threads[0]), ty = std::max(1u, c.threads[1]), tz = std::max(1u, c.threads[2]);
    // Copies of the ranges the program addresses, as for a draw (copy mode only).
    if (guest_) return fail("compute with a guest memory window");
    const auto& ranges = scanner_.scan(c);
    if (!scanner_.dispatchError().empty())
        return fail("compute resource scan stopped: " + scanner_.dispatchError());
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
        if (tiling != 8 && stored(base, 1)) {
            lastDispatchNote_ = "compute refusé : écriture dans une image tuilée non prise en charge";
            return fail("compute writes a tiled image; tiled compute stores are unsupported");
        }
        if (dfmt == 10 && tiling == 8 && !targets_.count(base) && stored(base, std::uint64_t(pitch) * height * 4))
            outputs.push_back({base, pitch, height});
    }
    // Only stores to targets and new images are kept (guest memory never
    // receives the rest): a kernel storing nothing else changes nothing.
    bool visible = !outputs.empty();
    for (const auto& [address, target] : targets_)
        if (target.texture && stored(address, 1)) visible = true;
    if (!visible && (scanner_.stored().empty() || !writeGuest_)) {
        if (!scanner_.stored().empty()) {
            lastDispatchNote_ = "compute refusé : écritures mémoire hors des images visibles, writeback invité non pris en charge";
            return fail("compute writes guest memory outside visible images; guest writeback is unsupported");
        }
        ++dispatchesWithoutEffect;
        lastDispatchNote_ = "aucune écriture atteinte par la lane de diagnostic";
        return true;
    }
    lastDispatchNote_ = "écriture vers une image visible détectée";
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
    std::vector<PM4Range> stores = scanner_.stored();
    std::sort(stores.begin(), stores.end(), [](const PM4Range& a, const PM4Range& b) { return a.base < b.base; });
    std::vector<PM4Range> guestWrites;
    for (const auto& span : stores) {
        if (!guestWrites.empty() && span.base <= guestWrites.back().base + guestWrites.back().size) {
            const auto end = std::max(guestWrites.back().base + guestWrites.back().size, span.base + span.size);
            guestWrites.back().size = end - guestWrites.back().base;
        } else guestWrites.push_back(span);
    }
    auto subtractTexture = [&](std::uint64_t base, std::uint64_t length) {
        if (!length || base > UINT64_MAX - length) return;
        const std::uint64_t end = base + length;
        std::vector<PM4Range> remaining;
        for (const auto& span : guestWrites) {
            const auto spanEnd = span.base + span.size;
            if (end <= span.base || base >= spanEnd) { remaining.push_back(span); continue; }
            if (span.base < base) remaining.push_back({span.base, base - span.base});
            if (end < spanEnd) remaining.push_back({end, spanEnd - end});
        }
        guestWrites.swap(remaining);
    };
    for (const auto& output : outputs) subtractTexture(output.base, std::uint64_t(output.pitch) * output.height * 4);
    for (const auto& [address, target] : targets_) if (target.texture)
        subtractTexture(address, std::uint64_t(target.texture.width) * target.texture.height * 4);
    std::vector<PendingGuestWrite> pending;
    for (const auto& span : guestWrites) {
        std::uint64_t at = 0;
        if (!locate(span.base, span.size, at)) return fail("compute guest write is outside copied memory ranges");
        pending.push_back({span.base, span.size, std::size_t(at), memory});
    }
    if (!pending.empty() && !writeGuest_) return fail("compute guest-memory writeback is unavailable");
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
    ++dispatchesEncoded;
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
    pendingGuestWrites_.insert(pendingGuestWrites_.end(), pending.begin(), pending.end());
    return true;
}

bool PM4MetalRenderer::writebackGuestWrites() {
    if (pendingGuestWrites_.empty()) return true;
    if (!writeGuest_) return fail("compute guest-memory writeback is unavailable");
    for (const auto& write : pendingGuestWrites_) {
        const auto* source = static_cast<const std::uint8_t*>(write.buffer.contents) + write.offset;
        std::uint64_t failedAddress = write.address;
        const std::int32_t status = writeGuest_(write.address, source, write.size, &failedAddress);
        if (status != 0) {
            char detail[192];
            std::snprintf(detail, sizeof(detail),
                          "compute guest-memory writeback failed at 0x%llx (fault 0x%llx, %llu bytes, guest memory status %d)",
                          static_cast<unsigned long long>(write.address),
                          static_cast<unsigned long long>(failedAddress),
                          static_cast<unsigned long long>(write.size), int(status));
            pendingGuestWrites_.clear();
            return fail(detail);
        }
    }
    pendingGuestWrites_.clear();
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

PM4MetalRenderer::Shared PM4MetalRenderer::sharedRange(std::uint64_t base, std::uint64_t size, bool texture,
                                                        id<MTLCommandBuffer> commands) {
    const auto found = shared_.find({base, size});
    if (found != shared_.end()) return found->second;
    Shared shared{nil, 0};
    if (texture && hostView) {
        // The whole host pages around the range, read in place.
        const std::uint64_t page = getpagesize(), start = base & ~(page - 1), end = (base + size + page - 1) & ~(page - 1);
        const void* host = hostView(start, end - start);
        if (host && reinterpret_cast<std::uintptr_t>(host) % page == 0)
            shared = {[device_ newBufferWithBytesNoCopy:const_cast<void*>(host) length:end - start
                                                options:MTLResourceStorageModeShared deallocator:nil], base - start};
        if (shared.buffer) ++rangesMapped;
    }
    if (!shared.buffer) {
        // At its guest address modulo 256, as in a draw's buffer (native texture views).
        shared = {uploadBuffer(size + (base & 255), commands), base & 255};
        if (!shared.buffer) return shared;
        copyGuest(base, static_cast<std::uint8_t*>(shared.buffer.contents) + shared.offset, size);
    }
    ++rangesShared;
    shared_[{base, size}] = shared;
    return shared;
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
        // Shared ranges are atlases of up to 64 MiB: keep a frame's worth or two.
        constexpr std::uint64_t budget = 384ull << 20;
        if (uploads_.size() < 256 && bytes <= budget - pooledBytes_) {
            uploads_.push_back({buffer, commands});
            pooledBytes_ += bytes;
        }
    }
    return buffer;
}

id<MTLTexture> PM4MetalRenderer::nativeTexture(const std::array<std::uint32_t, 8>& t, id<MTLBuffer> memory, id<MTLBuffer> const* slots,
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
    // 8_8_8_8, 5_6_5 or 8 (video planes): read, and filtered by Metal's sampler, through a view.
    if ((dfmt != 10 && dfmt != 16 && dfmt != 1) || (dfmt == 1 && ((t[1] >> 26) & 15) != 0) ||
        ((t[3] >> 20) & 31) != 8 || pitch < width) return miss(1, 0);
    const MTLPixelFormat format = dfmt == 10 ? MTLPixelFormatRGBA8Unorm : dfmt == 16 ? MTLPixelFormatB5G6R5Unorm : MTLPixelFormatR8Unorm;
    const std::uint64_t rowBytes = pitch * (dfmt == 10 ? 4 : dfmt == 16 ? 2 : 1), bytes = rowBytes * height;
    std::uint64_t offset = 0;
    bool inside = false;
    if (!rangeCount) {
        inside = base >= guestBase_ && base - guestBase_ <= memory.length && bytes <= memory.length - (base - guestBase_);
        offset = base - guestBase_;
    }
    id<MTLBuffer> holder = memory;
    for (unsigned k = 0; k < rangeCount && !inside; ++k) {
        const std::uint64_t start = ranges[3 * k], size = ranges[3 * k + 1], at = ranges[3 * k + 2];
        inside = base >= start && base - start <= size && bytes <= size - (base - start);
        // Bits 48 and up of a range offset name its shared buffer (slot 1 up).
        holder = at >> 48 ? slots[(at >> 48) - 1] : memory;
        offset = (at & 0xffffffffffffull) + (base - start);
    }
    if (!inside) return miss(2, bytes);
    const NSUInteger alignment = [device_ minimumLinearTextureAlignmentForPixelFormat:format];
    if (!alignment || offset % alignment || rowBytes % alignment) return miss(3, alignment ? offset % alignment : 0);
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:width height:height mipmapped:NO];
    descriptor.storageMode = holder.storageMode;
    descriptor.cpuCacheMode = holder.cpuCacheMode;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> view = [holder newTextureWithDescriptor:descriptor offset:offset bytesPerRow:rowBytes];
    return view ? view : miss(4, alignment);
}

MTLSamplerDescriptor* PM4MetalRenderer::nativeSamplerDescriptor(const std::array<std::uint32_t, 4>& s) {
    // S# CLAMP_X/Y: wrap, mirror, clamp to last texel; XY_MAG/MIN_FILTER:
    // point, bilinear (anisotropic filters read as bilinear); DEPTH_COMPARE.
    auto mode = [](std::uint32_t clamp, MTLSamplerAddressMode& out) {
        if (clamp > 2) return false;
        out = clamp == 0 ? MTLSamplerAddressModeRepeat : clamp == 1 ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge;
        return true;
    };
    MTLSamplerAddressMode x, y;
    if (!mode(s[0] & 7, x) || !mode((s[0] >> 3) & 7, y) || ((s[0] >> 12) & 7)) return nil;
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.sAddressMode = x; descriptor.tAddressMode = y;
    descriptor.magFilter = ((s[2] >> 20) & 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.minFilter = ((s[2] >> 22) & 3) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    descriptor.supportArgumentBuffers = YES;
    return descriptor;
}

id<MTLSamplerState> PM4MetalRenderer::nativeSampler(const std::array<std::uint32_t, 4>& s) {
    const auto found = samplers_.find(s);
    if (found != samplers_.end()) return found->second;
    MTLSamplerDescriptor* descriptor = nativeSamplerDescriptor(s);
    id<MTLSamplerState> sampler = descriptor ? [device_ newSamplerStateWithDescriptor:descriptor] : nil;
    samplers_[s] = sampler;
    return sampler;
}

bool PM4MetalRenderer::encode(const PM4Draw& d, id<MTLCommandBuffer> commands) {
    error.clear();
    if (d.swap > 1) return fail("colour target component swap " + std::to_string(d.swap));
    if ((d.vte & 0x3f) != 0x3f) return fail("viewport without scale and offset");
    const MTLPixelFormat format = d.swap == 1 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    if (d.scissor[2] <= d.scissor[0] || d.scissor[3] <= d.scissor[1]) return true; // nothing visible
    deferred_ = false;
    id<MTLRenderPipelineState> state = pipeline(d, format);
    if (!state && deferred_) { ++drawsDeferred; return true; } // its pipeline is still being built
    if (!state) return false;
    DrawState drawState{};
    id<MTLBuffer> memory = guest_;
    // buffer(5) up: the shared ranges (see sharedRange), else the draw's buffer.
    id<MTLBuffer> slots[slotCount] = {};
    unsigned slotsUsed = 0;
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
        // A large range (a texture atlas, a vertex pool) gets a buffer of its
        // own, copied once per submission and shared by every draw reading it;
        // the rest is copied into this draw's buffer.
        if (commands != sharedOwner_) { shared_.clear(); sharedOwner_ = commands; }
        bool own[16] = {};
        std::uint64_t total = 0;
        for (std::size_t k = 0, n = 0; k < ranges.size(); ++k) {
            if (ranges[k].size >= sharedRangeBytes && n < slotCount) { own[k] = true; ++n; }
            else total += ranges[k].size + 255;
        }
        if (total > (256ull << 20)) return fail("draw reads more than 256 MiB");
        memory = uploadBuffer(std::max<std::uint64_t>(total, 256), commands);
        if (!memory) return fail("memory buffer");
        std::uint64_t offset = 0;
        auto* bytes = static_cast<std::uint8_t*>(memory.contents);
        for (std::size_t k = 0; k < ranges.size(); ++k) {
            drawState.ranges[3 * k] = ranges[k].base;
            drawState.ranges[3 * k + 1] = ranges[k].size;
            if (own[k]) {
                bool texture = false;
                for (const auto& t : scanner_.images()) {
                    const std::uint64_t image = (std::uint64_t(t[0]) | std::uint64_t(t[1] & 0xff) << 32) << 8;
                    texture = texture || (image >= ranges[k].base && image - ranges[k].base < ranges[k].size);
                }
                const Shared shared = sharedRange(ranges[k].base, ranges[k].size, texture, commands);
                if (!shared.buffer) return fail("shared memory buffer");
                slots[slotsUsed] = shared.buffer;
                drawState.ranges[3 * k + 2] = std::uint64_t(++slotsUsed) << 48 | shared.offset;
                continue;
            }
            offset += (ranges[k].base - offset) & 255;
            // An unreadable range stays zero rather than exposing anything else.
            copyGuest(ranges[k].base, bytes + offset, ranges[k].size);
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
    for (unsigned k = slotsUsed; k < slotCount; ++k) slots[k] = memory;
    const NSUInteger slotOffsets[slotCount] = {};
    [encoder setVertexBuffers:slots offsets:slotOffsets withRange:NSMakeRange(5, slotCount)];
    [encoder setFragmentBuffers:slots offsets:slotOffsets withRange:NSMakeRange(5, slotCount)];
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
                                                                          argument(MTLDataTypeUInt4, 2), argument(MTLDataTypeUInt4, 3),
                                                                          argument(MTLDataTypeTexture, 4), argument(MTLDataTypeUInt4, 5),
                                                                          argument(MTLDataTypeUInt4, 6), argument(MTLDataTypeTexture, 7),
                                                                          argument(MTLDataTypeUInt4, 8), argument(MTLDataTypeUInt4, 9),
                                                                          argument(MTLDataTypeSampler, 10), argument(MTLDataTypeSampler, 11),
                                                                          argument(MTLDataTypeSampler, 12), argument(MTLDataTypeUInt4, 13),
                                                                          argument(MTLDataTypeUInt4, 14), argument(MTLDataTypeUInt4, 15)]];
        }
        // Up to three T#s (a video's Y, U and V planes), each at texture
        // index 0, 4 or 7 with its descriptor's halves after it.
        id<MTLTexture> images[3] = {};
        id<MTLSamplerState> samplers[3] = {};
        std::array<std::uint32_t, 8> descriptors[3] = {};
        std::array<std::uint32_t, 4> samplerWords[3] = {};
        unsigned found = 0, sampled = 0;
        if (scanner_.images().empty()) ++nativeMisses[0];
        for (std::size_t k = 0; k < scanner_.images().size() && found < 3; ++k) {
            const auto& t = scanner_.images()[k];
            if (!(images[found] = nativeTexture(t, memory, slots, drawState.ranges, drawState.rangeCount, target.texture))) continue;
            samplerWords[found] = scanner_.samplers()[k];
            if ((samplers[found] = nativeSampler(samplerWords[found]))) sampled |= 16u << found;
            descriptors[found++] = t;
        }
        id<MTLBuffer> arguments = nativeArguments_ ? uploadBuffer(nativeArguments_.encodedLength, commands) : nil;
        if (!arguments) {
            if (!batching_) [encoder endEncoding];
            return fail("native texture arguments");
        }
        [nativeArguments_ setArgumentBuffer:arguments offset:0];
        *static_cast<std::uint32_t*>([nativeArguments_ constantDataAtIndex:1]) = ((1u << found) - 1) | sampled;
        static const NSUInteger fields[3] = {0, 4, 7};
        for (unsigned k = 0; k < 3; ++k) {
            const NSUInteger at = fields[k], low = k ? at + 1 : 2;
            [nativeArguments_ setTexture:images[k] atIndex:at];
            [nativeArguments_ setSamplerState:samplers[k] atIndex:10 + k];
            std::memcpy([nativeArguments_ constantDataAtIndex:13 + k], samplerWords[k].data(), 16);
            std::memcpy([nativeArguments_ constantDataAtIndex:low], descriptors[k].data(), 16);
            std::memcpy([nativeArguments_ constantDataAtIndex:low + 1], descriptors[k].data() + 4, 16);
            if (images[k]) [encoder useResource:images[k] usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
        }
        [encoder setVertexBuffer:arguments offset:0 atIndex:3];
        [encoder setFragmentBuffer:arguments offset:0 atIndex:3];
        if (found) ++nativeDraws;
        nativeViews += found;
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
            // A pixel lane that stopped before its image instructions names no texture: say why.
            if (!scanner_.pixelError().empty()) batchDraws += ", PS interprété : " + scanner_.pixelError();
            for (const auto& t : scanner_.images()) {
                if (shown++ == 3) break;
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
