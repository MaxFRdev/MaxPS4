#import "PM4MetalRenderer.h"
#include "GCNTranslator.hpp"
#include "PM4ResourceScan.hpp"
#include "PM4Blend.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#pragma clang diagnostic pop
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
constexpr std::uint32_t rectList = 1, flipY = 2, scalarSites = 4;

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

id<MTLRenderPipelineState> PM4MetalRenderer::pipeline(const PM4Draw& d, MTLPixelFormat format, MTLPixelFormat depth) {
    const PipelineKey key{d.vs, d.ps, d.vsUser[0], d.vsUser[1], d.psInputAddr, inputKey(d), unsigned(format),
                          (d.blend >> 30) & 1 ? d.blend : 0u, d.mask, unsigned(depth)};
    if (asyncPipelines) {
        std::lock_guard<std::mutex> lock(async_->mutex);
        const auto ready = async_->ready.find(key);
        if (ready != async_->ready.end()) {
            id<MTLRenderPipelineState> built = ready->second;
            async_->ready.erase(ready);
            // A pipeline Metal refused stays deferred: its draws are skipped.
            if (built) {
                if (pipelines_.count(key)) ++pipelinesReplaced; else ++pipelinesBuilt;
                pipelines_[key] = built;
                return built;
            }
            if (!pipelines_.count(key)) async_->refused.insert(key);
        }
    }
    const auto found = pipelines_.find(key);
    if (found != pipelines_.end()) return found->second;
    if (asyncPipelines) {
        std::lock_guard<std::mutex> lock(async_->mutex);
        if (async_->pending.count(key) || async_->refused.count(key)) { deferred_ = true; return nil; }
    }
    GCNTranslateOptions vsOptions;
    vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = d.vsUserCount;
    for (unsigned k = 0; k < 16; ++k) vsOptions.userData[k] = d.vsUser[k];
    GCNTranslateOptions psOptions;
    psOptions.name = "ps_program"; psOptions.userCount = d.psUserCount;
    psOptions.directLoads = native_ || !guest_; // Each draw is scanned: its table is complete.
    for (unsigned k = 0; k < 16; ++k) psOptions.userData[k] = d.psUser[k];
    compiling = 3;
    const auto vs = translateGCN(code_, d.vs, vsOptions);
    // A depth-only draw may have no pixel program (Gnm's null PS): an empty one.
    GCNTranslation ps;
    if (d.ps || d.colour) ps = translateGCN(code_, d.ps, psOptions);
    else ps.source = "GCN_FN void ps_program(GCN_THREAD GCNLane& L, GCNMemory M, GCN_THREAD GCNQuad& Q) { (void)L; (void)M; (void)Q; }\n";
    compiling = 0;
    if (!vs) { fail("VS translation: " + vs.error); return nil; }
    if (!ps) { fail("PS translation: " + ps.error); return nil; }
    {
        ScalarSites& sites = scalarSites_[d.ps];
        sites.list = ps.sites; sites.words = ps.siteWords;
        scalarLoadsAll += unsigned(ps.sites.size());
        for (const auto& site : ps.sites) if (site.from[0]) ++scalarLoadsDirect;
        std::sort(sites.list.begin(), sites.list.end(), [](const GCNScalarSite& a, const GCNScalarSite& b) { return a.pc < b.pc; });
    }
    std::string source = gcnMetalPrelude(native_, gcnHighestUsed(vs.params), gcnHighestUsed(ps.attributes), gcnHighestUsed(ps.colors)) + vs.source + ps.source +
                               gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", d.psInputAddr, d.inputCntl, native_);
    // Tests: a sample Metal's sampler refuses reads as zero instead of filtering in
    // software, which shows whether a draw really takes the native path.
    if (native_ && nativeSamplesOnly) {
        const std::string sample = "if (gcn_native_sample(M, t, s, unorm, u, v, filtered)) return gcn_texture_swizzle(t, filtered);";
        const auto at = source.find(sample);
        if (at != std::string::npos) source.insert(at + sample.size(), "\n    if (true) return float4(0.0f, 0.0f, 0.0f, 0.0f);");
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    NSError* failure = nil;
    descriptor.depthAttachmentPixelFormat = depth;
    // Without colour, the pixel program only matters when it may discard.
    const bool fragment = d.colour || ps.mayKill;
    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
    color.pixelFormat = format;
    color.writeMask = ((d.mask & 1) ? MTLColorWriteMaskRed : 0) | ((d.mask & 2) ? MTLColorWriteMaskGreen : 0) |
                      ((d.mask & 4) ? MTLColorWriteMaskBlue : 0) | ((d.mask & 8) ? MTLColorWriteMaskAlpha : 0);
    if (d.colour && ((d.blend >> 30) & 1)) {
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
        std::string programs = programWords("VS", d.vs, d.vsUser, d.vsUserCount);
        if (d.vsUserCount >= 2) programs += programWords("Fetch", std::uint64_t(d.vsUser[0]) | std::uint64_t(d.vsUser[1]) << 32, d.vsUser, 0);
        programs += programWords("PS", d.ps, d.psUser, d.psUserCount);
        programs += "\nSource Metal : " + std::to_string(source.size()) + " octets";
        auto async = async_;
        { std::lock_guard<std::mutex> lock(async->mutex);
          async->pending[key] = {std::chrono::steady_clock::now(), "MSL compilation", std::move(programs)}; }
        // `fast`: built with default optimisation. A size build shows the draws
        // first; a fast build replaces it, and is never replaced by one.
        auto finish = [async, key](id<MTLRenderPipelineState> state, NSError* error, bool fast) {
            std::lock_guard<std::mutex> lock(async->mutex);
            const auto built = async->pending.find(key);
            if (built == async->pending.end() && (!state || !fast)) return;
            if (!fast && !state) return; // The default build may still succeed.
            if (built != async->pending.end())
                async->slowest = std::max(async->slowest, std::chrono::duration<double>(std::chrono::steady_clock::now() - built->second.since).count());
            async->pending.erase(key);
            async->ready[key] = state;
            if (!state) async->lastError = error ? error.localizedDescription.UTF8String : "unknown";
        };
        auto finished = [async, finish](id<MTLRenderPipelineState> state, NSError* error, bool fast) {
            finish(state, error, fast);
            { std::lock_guard<std::mutex> lock(async->mutex);
              if (fast) {
                  char done[48];
                  std::snprintf(done, sizeof done, " 0x%llx %.1f s%s", (unsigned long long)async->buildingPS,
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - async->buildingSince).count(), state ? "" : " (refus)");
                  async->defaultsDone += done;
                  async->running = false;
              } else --async->sizing; }
            async->next();
        };
        id<MTLDevice> device = device_;
        NSString* text = [NSString stringWithUTF8String:source.c_str()];
        auto build = [=](MTLCompileOptions* options, bool fast) {
            [device newLibraryWithSource:text options:options completionHandler:^(id<MTLLibrary> library, NSError* error) {
                if (!library) { finished(nil, error, fast); return; }
                { std::lock_guard<std::mutex> lock(async->mutex);
                  const auto pending = async->pending.find(key);
                  if (pending != async->pending.end()) pending->second.stage = "render pipeline creation"; }
                MTLRenderPipelineDescriptor* own = [descriptor copy];
                own.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
                own.fragmentFunction = fragment ? [library newFunctionWithName:@"gcn_fragment"] : nil;
                [device newRenderPipelineStateWithDescriptor:own completionHandler:^(id<MTLRenderPipelineState> state, NSError* failed) {
                    finished(state, failed, fast);
                }];
            }];
        };
        // Default optimisation did not finish some programs on the iPhone, large or
        // small (over 52 s for one, 2.8 s for size; 0.4 s on a Mac), their draws
        // deferred and the screen black. Started together, size builds waited behind
        // them (20.5 s, 2 of 19 first): size goes first, default builds follow one at
        // a time once none is left, and replace it. Metal caches whichever completes.
        // Fast math did not finish some programs in minutes either.
        MTLCompileOptions* fastest = compileOptions();
        MTLCompileOptions* size = compileOptions();
        if (@available(macOS 13.0, iOS 16.0, *)) size.optimizationLevel = MTLLibraryOptimizationLevelSize;
        { std::lock_guard<std::mutex> lock(async->mutex);
          ++async->sizing;
          async->later.push_back([async, build, fastest, ps = d.ps] {
              { std::lock_guard<std::mutex> lock(async->mutex);
                async->buildingPS = ps; async->buildingSince = std::chrono::steady_clock::now(); }
              build(fastest, true);
          }); }
        build(size, false);
        deferred_ = true;
        return nil;
    }
    id<MTLLibrary> library = this->library(source);
    if (!library) return nil;
    descriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
    descriptor.fragmentFunction = fragment ? [library newFunctionWithName:@"gcn_fragment"] : nil;
    compiling = 2;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor error:&failure];
    compiling = 0;
    if (!state) { fail(std::string("pipeline: ") + failure.localizedDescription.UTF8String); return nil; }
    ++pipelinesBuilt;
    pipelines_[key] = state;
    return state;
}

// The words of a program, to study a slow pipeline: up to the first
// word that is no instruction, past s_endpgm for code it branches over.
std::string PM4MetalRenderer::programWords(const char* stage, std::uint64_t address, const std::uint32_t* user, unsigned userCount) const {
    std::vector<std::uint32_t> words;
    unsigned zeros = 0;
    for (std::uint32_t word = 0; words.size() < 4096 && zeros < 4 && code_.read(address + 4 * words.size(), &word, 4);) { words.push_back(word); zeros = word ? 0 : zeros + 1; }
    std::size_t end = 0;
    for (GCNInstruction i; end < words.size() && decodeGCNInstruction(words.data() + end, words.size() - end, end, i) == GCNError::none;) end += i.size;
    char text[64];
    std::snprintf(text, sizeof text, "\n%s 0x%llx, %zu mots, SGPR :", stage, (unsigned long long)address, end);
    std::string out = text;
    for (unsigned k = 0; k < userCount && k < 16; ++k) { std::snprintf(text, sizeof text, " %x", user[k]); out += text; }
    out += "\n   ";
    for (std::size_t k = 0; k < end; ++k) { std::snprintf(text, sizeof text, " %08x", words[k]); out += text; }
    return out;
}

void PM4MetalRenderer::listCostly(std::uint64_t address) {
    if (costly_.insert(address).second) programListings += programWords("PS couteux", address, nullptr, 0);
}

std::string PM4MetalRenderer::pipelineStatus() const {
    std::lock_guard<std::mutex> lock(async_->mutex);
    // Which programs still run their size build, and what the queue is doing.
    char queue[96];
    std::snprintf(queue, sizeof queue, " · builds par défaut : %zu en attente", async_->later.size());
    std::string defaults = queue;
    if (async_->running) {
        std::snprintf(queue, sizeof queue, ", PS 0x%llx depuis %.1f s", (unsigned long long)async_->buildingPS,
                      std::chrono::duration<double>(std::chrono::steady_clock::now() - async_->buildingSince).count());
        defaults += queue;
    }
    if (!async_->defaultsDone.empty()) defaults += ", finies :" + async_->defaultsDone;
    if (async_->pending.empty()) {
        if (async_->slowest <= 0) return {};
        char built[48];
        std::snprintf(built, sizeof built, " · plus lent construit %.1f s", async_->slowest);
        return built + defaults;
    }
    const auto oldest = std::min_element(async_->pending.begin(), async_->pending.end(),
        [](const auto& a, const auto& b) { return a.second.since < b.second.since; });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - oldest->second.since).count();
    char text[192];
    std::snprintf(text, sizeof text, " · plus lent construit %.1f s · plus ancien : %s %.1f s, VS 0x%llx / PS 0x%llx", async_->slowest,
                  oldest->second.stage, seconds, (unsigned long long)std::get<0>(oldest->first),
                  (unsigned long long)std::get<1>(oldest->first));
    // Long enough to be worth rebuilding on a Mac.
    return seconds < 5 ? text + defaults : text + defaults + oldest->second.programs;
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
    if (kernelsBuilt <= 4) listProgram("CS", c.program, c.user, c.userCount, true);
    kernels_[key] = state;
    return state;
}

// Gnmx's buffer fill (cs_set_uint): out[i] = table[i & mask] below a count.
bool PM4MetalRenderer::fillProgram(std::uint64_t address) {
    const auto known = fillPrograms_.find(address);
    if (known != fillPrograms_.end()) return known->second;
    static const char* const expected[] = {
        "s_mov_b32 vcc_hi, 0xd", "s_lshl_b32 vcc_lo, s12, 6", "s_buffer_load_dword s12, s[8:11], 0x0",
        "v_add_i32 v0, vcc, vcc_lo, v0", "s_waitcnt lgkmcnt(0)", "v_cmpx_gt_u32 vcc, s12, v0", "s_cbranch_execz 18",
        "s_buffer_load_dword s8, s[8:11], 0x1", "s_waitcnt lgkmcnt(0)", "v_and_b32 v1, s8, v0",
        "buffer_load_format_x v1, v1, s[0:3], 0 idxen", "v_cmpx_gt_u32 s[0:1], s6, v0", "s_waitcnt vmcnt(0) lgkmcnt(15)",
        "buffer_store_format_x v1, v0, s[4:7], 0 idxen", "s_endpgm"};
    std::uint32_t words[32] = {};
    bool same = code_.read(address, words, sizeof words);
    std::size_t at = 0;
    for (const char* line : expected) {
        GCNInstruction i;
        if (!same || decodeGCNInstruction(words + at, 32 - at, at, i) != GCNError::none) { same = false; break; }
        const std::string text = gcnDisassemble(i);
        // Sonic Mania's compiler multiplies where Mortal Kombat's shifts.
        if (text != line && !(line == expected[1] && text == "s_mul_i32 vcc_lo, s12, 64")) { same = false; break; }
        at += i.size;
    }
    return fillPrograms_[address] = same;
}

// A fill without its kernel, whose lanes cost far more than the stores: a blit
// into a target, or a write to guest memory. False: run it as any dispatch.
bool PM4MetalRenderer::fillDispatch(const PM4Dispatch& c, id<MTLCommandBuffer> commands) {
    if (c.userCount != 12 || (c.rsrc2 & 0x380) != 0x80 || c.threads[0] != 64 || c.threads[1] != 1 || c.threads[2] != 1 ||
        c.groups[1] != 1 || c.groups[2] != 1 || !fillProgram(c.program)) return false;
    // A fill that goes to its kernel costs a GPU wait: the report says why.
    auto refuse = [&](const char* why, std::uint64_t at = 0, std::uint64_t length = 0) {
        ++fillsRefused;
        char text[128];
        std::snprintf(text, sizeof text, "%s 0x%llx, %llu octets", why, (unsigned long long)at, (unsigned long long)length);
        fillRefusal = text;
        return false;
    };
    auto base = [&](unsigned k) { return std::uint64_t(c.user[k]) | std::uint64_t(c.user[k + 1] & 0xfff) << 32; };
    auto dwords = [&](unsigned k) { // One 32-bit uint per 4-byte record, unswizzled.
        return ((c.user[k + 1] >> 16) & 0x3fff) == 4 && !(c.user[k + 1] >> 30) && (c.user[k + 3] & 7) == 4 &&
               ((c.user[k + 3] >> 12) & 7) == 4 && ((c.user[k + 3] >> 15) & 15) == 4;
    };
    std::uint32_t limits[2], value = 0; // Count and mask.
    if (!dwords(0) || !dwords(4)) return refuse("format", base(4));
    if (!c.user[10] || !code_.read(base(8), limits, sizeof limits)) return refuse("limites illisibles", base(8));
    if (limits[1]) return refuse("motif de plusieurs valeurs", base(4), limits[1]);
    if (c.user[2] && !code_.read(base(0), &value, 4)) return refuse("valeur illisible", base(0));
    const std::uint64_t bytes = 4 * std::min<std::uint64_t>({std::uint64_t(c.groups[0]) * 64, limits[0], c.user[6]});
    const std::uint64_t address = base(4);
    if (!bytes || bytes > (256ull << 20)) return refuse("taille", address, bytes);
    auto overlaps = [&](std::uint64_t at, std::uint64_t length) { return address < at + length && at < address + bytes; };
    Target* target = nullptr;
    for (auto& [at, candidate] : targets_) {
        if (!candidate.texture) continue;
        const std::uint64_t row = candidate.texture.width * 4, length = row * candidate.texture.height;
        if (!overlaps(at, length)) continue;
        if (target) return refuse("deux cibles", address, bytes);
        if (at != address) return refuse("cible decalee", at, bytes);
        if (bytes % row) return refuse("lignes partielles", at, bytes);
        if (bytes > length) return refuse("plus grand que la cible", at, bytes);
        if (candidate.fresh && bytes < length) return refuse("cible neuve partielle", at, bytes);
        target = &candidate;
    }
    if (!target && !writeGuest_) return refuse("sans ecriture", address, bytes);
    if (!target) for (const auto& [at, length] : inPlace_) if (overlaps(at, length)) return refuse("zone partagee", address, bytes);
    auto fill = fills_.find(value);
    if (fill == fills_.end() || fill->second.length < bytes) {
        if (fills_.size() >= 4) fills_.clear();
        id<MTLBuffer> buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!buffer) return false;
        std::fill_n(static_cast<std::uint32_t*>(buffer.contents), bytes / 4, value);
        fill = fills_.insert_or_assign(value, buffer).first;
    }
    if (target) {
        if (encoder_) { [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil; }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!blit) return false;
        const std::uint64_t row = target->texture.width * 4;
        [blit copyFromBuffer:fill->second sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:bytes
                  sourceSize:MTLSizeMake(target->texture.width, bytes / row, 1)
                   toTexture:target->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        target->fresh = false;
        ++dispatchesEncoded;
        ++targetsSynced;
    } else {
        std::uint64_t failedAddress = address;
        if (writeGuest_(address, fill->second.contents, bytes, &failedAddress) != 0) return false;
        writebackBytes += bytes;
        // Copies taken for this command buffer predate the fill.
        for (auto it = shared_.begin(); it != shared_.end();)
            it = overlaps(it->first.first, it->first.second) ? shared_.erase(it) : std::next(it);
    }
    ++fillsDone;
    lastDispatchNote_ = target ? "remplissage d'une cible" : "remplissage de la memoire du jeu";
    return true;
}

bool PM4MetalRenderer::dispatch(const PM4Dispatch& c, id<MTLCommandBuffer> commands) {
    error.clear();
    lastDispatchNote_.clear();
    if (!c.program || !c.groups[0] || !c.groups[1] || !c.groups[2]) return true; // nothing to run
    const std::uint64_t tx = std::max(1u, c.threads[0]), ty = std::max(1u, c.threads[1]), tz = std::max(1u, c.threads[2]);
    // Copies of the ranges the program addresses, as for a draw (copy mode only).
    if (guest_) return fail("compute with a guest memory window");
    if (fillDispatch(c, commands)) return true;
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
    const auto copyStarted = std::chrono::steady_clock::now();
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
    dispatchCopyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStarted).count();
    dispatchCopyBytes += offset;
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
    if (encoder_) { [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil; }
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
    std::uint64_t written = 0;
    for (const auto& write : pending) written += write.size;
    char note[96];
    std::snprintf(note, sizeof note, "%zu zones, %.1f Mio en entree, %.1f Mio en sortie, %zu images", ranges.size(),
                  double(offset) / (1 << 20), double(written) / (1 << 20), outputs.size());
    lastDispatchNote_ = note;
    pendingGuestWrites_.insert(pendingGuestWrites_.end(), pending.begin(), pending.end());
    return true;
}

bool PM4MetalRenderer::writebackGuestWrites() {
    if (pendingGuestWrites_.empty()) return true;
    if (!writeGuest_) return fail("compute guest-memory writeback is unavailable");
    const auto writeStarted = std::chrono::steady_clock::now();
    for (const auto& write : pendingGuestWrites_) {
        const auto* source = static_cast<const std::uint8_t*>(write.buffer.contents) + write.offset;
        writebackBytes += write.size;
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
    writebackMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - writeStarted).count();
    pendingGuestWrites_.clear();
    return true;
}

void PM4MetalRenderer::listProgram(const char* stage, std::uint64_t address, const std::uint32_t* user, unsigned userCount, bool always) {
    if ((!always && listed_.size() >= 9) || std::find(listed_.begin(), listed_.end(), address) != listed_.end()) return;
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
    encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil; batching_ = false;
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
        if (shared.buffer) { ++rangesMapped; inPlace_.insert({start, end - start}); }
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

// A BC7 T# in tile mode 13 (Thin_1dThin: blocks in 8x8 micro tiles, Morton
// order inside) decoded once into a texture: Metal cannot sample it on iPhone.
id<MTLTexture> PM4MetalRenderer::bc7Texture(std::uint64_t base, std::uint64_t width, std::uint64_t height, std::uint64_t pitch) {
    const std::uint64_t blocksWide = (pitch + 3) / 4, blocksHigh = (height + 3) / 4;
    const std::uint64_t tilesWide = (blocksWide + 7) / 8, tilesHigh = (blocksHigh + 7) / 8;
    const std::uint64_t bytes = tilesWide * tilesHigh * 64 * 16;
    if (!bytes || bytes > (64ull << 20)) return nil;
    const auto key = std::make_tuple(base, width, height);
    const auto found = bc7Textures_.find(key);
    if (found != bc7Textures_.end() && found->second.batch == batchSerial_) return found->second.texture;
    // A game fills its atlases as it loads: 16 bytes of every 4 KiB, once a
    // batch, tell whether any part has changed since it was decoded.
    std::uint64_t mark = 1469598103934665603ull;
    for (std::uint64_t at = 0; at < bytes; at += 4096) {
        std::uint8_t sample[16] = {};
        copyGuest(base + at, sample, std::min<std::uint64_t>(bytes - at, sizeof sample));
        for (const std::uint8_t byte : sample) mark = (mark ^ byte) * 1099511628211ull;
    }
    if (found != bc7Textures_.end() && found->second.mark == mark) { found->second.batch = batchSerial_; return found->second.texture; }
    std::vector<std::uint8_t> blocks(bytes);
    copyGuest(base, blocks.data(), bytes);
    const std::uint64_t rowBytes = blocksWide * 16;
    std::vector<std::uint8_t> pixels(rowBytes * blocksHigh * 4);
    for (std::uint64_t y = 0; y < blocksHigh; ++y) for (std::uint64_t x = 0; x < blocksWide; ++x) {
        const std::uint64_t morton = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
        const std::uint64_t at = ((y / 8 * tilesWide + x / 8) * 64 + morton) * 16;
        bcdec_bc7(blocks.data() + at, pixels.data() + y * 4 * rowBytes + x * 16, int(rowBytes));
    }
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:rowBytes];
    if (bc7Textures_.size() >= 32) bc7Textures_.clear();
    bc7Textures_[key] = {texture, mark, batchSerial_};
    return texture;
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
    if (dfmt == 41) {
        if (((t[3] >> 20) & 31) != 13 || ((t[1] >> 26) & 15) != 0) return miss(1, 0);
        id<MTLTexture> decoded = bc7Texture(base, width, height, pitch);
        return decoded ? decoded : miss(4, 0);
    }
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
    // S# CLAMP_X/Y: wrap, mirror, clamp to last texel, or to a transparent black
    // border (BORDER_COLOR_TYPE 0); XY_MAG/MIN_FILTER: point, bilinear
    // (anisotropic filters read as bilinear); DEPTH_COMPARE.
    auto mode = [&s](std::uint32_t clamp, MTLSamplerAddressMode& out) {
        if (clamp == 6 && !(s[3] >> 30)) { out = MTLSamplerAddressModeClampToZero; return true; }
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
    if (d.colour && d.swap > 1) return fail("colour target component swap " + std::to_string(d.swap));
    if (!d.colour && !d.depthTarget) return fail("draw with neither colour nor depth");
    // Gnm's embedded full-screen VS has no guest code (address 0). A game clears
    // depth with it, several times a frame: no draw, the next pass that uses
    // this buffer clears it on load, so an unused buffer costs nothing.
    if (!d.vs && !d.colour) {
        ++embeddedDepthDraws;
        if (!d.depthClear) return true;
        Target& cleared = depthTargets_[d.depthTarget];
        if (cleared.texture && encoder_ && depthAttachment_ == cleared.texture) {
            [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil;
        }
        cleared.fresh = true;
        depthClearTo_[d.depthTarget] = d.depthClearValue;
        return true;
    }
    if ((d.vte & 0x3f) != 0x3f) return fail("viewport without scale and offset");
    const MTLPixelFormat format = !d.colour ? MTLPixelFormatInvalid : d.swap == 1 ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA8Unorm;
    const MTLPixelFormat depthFormat = d.depthTarget ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;
    if (d.scissor[2] <= d.scissor[0] || d.scissor[3] <= d.scissor[1]) return true; // nothing visible
    // Front and back faces culled: no triangle is left (rectangles are never culled).
    if (d.cull == 3 && d.primitive != 0x11 && !d.depthClear) { ++culledDraws; return true; }
    deferred_ = false;
    id<MTLRenderPipelineState> state = pipeline(d, format, depthFormat);
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
    Target none{nil, false};
    Target& target = d.colour ? targets_[d.target] : none;
    if (d.colour && (!target.texture || target.texture.width != d.pitch || target.texture.height != d.height ||
        target.texture.pixelFormat != format)) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                              width:d.pitch height:d.height mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        // These colour targets are rendered, sampled and copied only by the
        // GPU. CPU-visible shared storage is unnecessary for this path.
        descriptor.storageMode = MTLStorageModePrivate;
        target = {[device_ newTextureWithDescriptor:descriptor], true};
        if (!target.texture) return fail("colour target texture");
    }
    Target* depth = nullptr;
    if (d.depthTarget && !(depth = depthTexture(d, commands))) return false;
    id<MTLTexture> depthView = depth ? depth->texture : nil;
    if (encoder_ && (commands_ != commands || attachment_ != target.texture || depthAttachment_ != depthView)) {
        [encoder_ endEncoding]; encoder_ = nil;
    }
    id<MTLRenderCommandEncoder> encoder = encoder_;
    if (!encoder) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (target.texture) {
        pass.colorAttachments[0].texture = target.texture;
        pass.colorAttachments[0].loadAction = target.fresh ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (depth) {
        // A new depth buffer starts at the clear value the game set.
        pass.depthAttachment.texture = depth->texture;
        pass.depthAttachment.loadAction = depth->fresh ? MTLLoadActionClear : MTLLoadActionLoad;
        const auto clearTo = depthClearTo_.find(d.depthTarget);
        pass.depthAttachment.clearDepth = clearTo != depthClearTo_.end() ? clearTo->second : d.depthClearValue;
        if (clearTo != depthClearTo_.end()) depthClearTo_.erase(clearTo);
        pass.depthAttachment.storeAction = MTLStoreActionStore;
    }
    if (batching_) pass.visibilityResultBuffer = visibility_;
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return fail("render encoder");
    target.fresh = false;
    if (depth) depth->fresh = false;
    ++renderPasses;
    if (batching_) { encoder_ = encoder; commands_ = commands; attachment_ = target.texture; depthAttachment_ = depthView; }
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
    // A depth clear writes DB_DEPTH_CLEAR wherever the draw covers.
    if (d.depthClear) viewport.znear = viewport.zfar = d.depthClearValue;
    [encoder setViewport:viewport];
    [encoder setScissorRect:MTLScissorRect{d.scissor[0], d.scissor[1], d.scissor[2] - d.scissor[0], d.scissor[3] - d.scissor[1]}];
    // Encoders are shared by draws: every draw sets its own depth and faces.
    [encoder setDepthStencilState:d.depthClear ? depthState(7, true) : d.depthTest ? depthState(d.depthFunc, d.depthWrite) : depthState(7, false)];
    // Screen positions match the PS4's (Y flipped above when needed), so
    // FACE's winding on screen is Metal's.
    const bool culls = d.primitive != 0x11 && !d.depthClear && d.cull;
    [encoder setCullMode:!culls ? MTLCullModeNone : d.cull == 1 ? MTLCullModeFront : MTLCullModeBack];
    [encoder setFrontFacingWinding:d.frontClockwise ? MTLWindingClockwise : MTLWindingCounterClockwise];
    if (depth) { ++depthDraws; if (!d.colour) ++depthOnlyDraws; }
    for (unsigned k = 0; k < 16; ++k) { drawState.vsUser[k] = d.vsUser[k]; drawState.psUser[k] = d.psUser[k]; }
    drawState.flags = (d.primitive == 0x11 ? rectList : 0) | (ys > 0 ? flipY : 0);
    // The pixel program's scalar values as this draw's scan read them. Always
    // bound for a program that declares them, even empty: the compiler may read
    // a buffer argument before the test that guards it.
    id<MTLBuffer> siteBuffer = nil;
    bool siteBytes = false;
    const auto sites = scalarSites_.find(d.ps);
    if (sites != scalarSites_.end() && sites->second.words) {
        if (guest_ && !native_) siteTable_.assign(sites->second.words, 0); // Not scanned.
        else scanner_.scalarTable(sites->second.list, sites->second.words, siteTable_);
        const std::size_t length = siteTable_.size() * sizeof(std::uint32_t);
        if (length <= 4096) siteBytes = true;
        else if ((siteBuffer = uploadBuffer(length, commands))) std::memcpy(siteBuffer.contents, siteTable_.data(), length);
        else {
            if (!batching_) [encoder endEncoding];
            return fail("scalar value table");
        }
        drawState.flags |= scalarSites;
    }
    [encoder setVertexBuffer:memory offset:0 atIndex:0];
    [encoder setFragmentBuffer:memory offset:0 atIndex:0];
    for (unsigned k = slotsUsed; k < slotCount; ++k) slots[k] = memory;
    const NSUInteger slotOffsets[slotCount] = {};
    [encoder setVertexBuffers:slots offsets:slotOffsets withRange:NSMakeRange(5, slotCount)];
    [encoder setFragmentBuffers:slots offsets:slotOffsets withRange:NSMakeRange(5, slotCount)];
    [encoder setVertexBytes:&drawState length:sizeof drawState atIndex:1];
    [encoder setFragmentBytes:&drawState length:sizeof drawState atIndex:1];
    if (siteBytes) [encoder setFragmentBytes:siteTable_.data() length:siteTable_.size() * sizeof(std::uint32_t) atIndex:4];
    else if (siteBuffer) [encoder setFragmentBuffer:siteBuffer offset:0 atIndex:4];
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
                                                                          argument(MTLDataTypeUInt4, 14), argument(MTLDataTypeUInt4, 15),
                                                                          argument(MTLDataTypeTexture, 16), argument(MTLDataTypeUInt4, 17),
                                                                          argument(MTLDataTypeUInt4, 18), argument(MTLDataTypeSampler, 19),
                                                                          argument(MTLDataTypeUInt4, 20)]];
        }
        // Up to four T#s (a video's Y, U and V planes, Mortal Kombat 1's lighting),
        // each at texture index 0, 4, 7 or 16 with its descriptor's halves after it.
        id<MTLTexture> images[4] = {};
        id<MTLSamplerState> samplers[4] = {};
        std::array<std::uint32_t, 8> descriptors[4] = {};
        std::array<std::uint32_t, 4> samplerWords[4] = {};
        unsigned found = 0, sampled = 0;
        if (scanner_.images().empty()) ++nativeMisses[0];
        for (std::size_t k = 0; k < scanner_.images().size() && found < 4; ++k) {
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
        nativeBound_ = found; nativeFiltered_ = __builtin_popcount(sampled);
        static const NSUInteger fields[4] = {0, 4, 7, 16}, samplerFields[4] = {10, 11, 12, 19}, wordFields[4] = {13, 14, 15, 20};
        for (unsigned k = 0; k < 4; ++k) {
            const NSUInteger at = fields[k], low = k ? at + 1 : 2;
            [nativeArguments_ setTexture:images[k] atIndex:at];
            [nativeArguments_ setSamplerState:samplers[k] atIndex:samplerFields[k]];
            std::memcpy([nativeArguments_ constantDataAtIndex:wordFields[k]], samplerWords[k].data(), 16);
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
    if (details_ && batchDraws.size() < 20000) {
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
            std::snprintf(text, sizeof text, ", natives %u dont %u filtrees sur %zu", nativeBound_, nativeFiltered_, scanner_.images().size());
            batchDraws += text;
            for (const auto& t : scanner_.images()) {
                if (shown++ == 4) break;
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

id<MTLTexture> PM4MetalRenderer::depthTarget(std::uint64_t address) const {
    const auto found = depthTargets_.find(address);
    return found == depthTargets_.end() ? nil : found->second.texture;
}

std::string PM4MetalRenderer::depthReport() const {
    char text[240];
    std::snprintf(text, sizeof text, "\nProfondeur Metal : %u effacements sans programme · %u dessins avec profondeur, dont %u sans couleur · %u dessins entièrement cullés · %zu tampons, %u redimensionnés",
                  embeddedDepthDraws, depthDraws, depthOnlyDraws, culledDraws, depthTargets_.size(), depthResized);
    return text;
}

id<MTLDepthStencilState> PM4MetalRenderer::depthState(unsigned function, bool write) {
    const unsigned key = (function & 7) | (write ? 8u : 0u);
    const auto found = depthStates_.find(key);
    if (found != depthStates_.end()) return found->second;
    // ZFUNC codes in order: never, less, equal, less or equal, greater, not equal, greater or equal, always.
    static const MTLCompareFunction functions[8] = {
        MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
        MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction = functions[function & 7];
    descriptor.depthWriteEnabled = write;
    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:descriptor];
    depthStates_[key] = state;
    return state;
}

PM4MetalRenderer::Target* PM4MetalRenderer::depthTexture(const PM4Draw& d, id<MTLCommandBuffer> commands) {
    Target& depth = depthTargets_[d.depthTarget];
    if (depth.texture && depth.texture.width == d.pitch && depth.texture.height == d.height) return &depth;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                          width:d.pitch height:d.height mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) { fail("depth texture"); return nullptr; }
    // Drawn with a target of another size (a depth pre-pass at the depth
    // buffer's size, then colour at the target's): keep what overlaps.
    if (depth.texture && !depth.fresh) {
        if (encoder_) { [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil; }
        if (texture.width > depth.texture.width || texture.height > depth.texture.height) {
            // Pixels the copy does not reach start at the clear value.
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.depthAttachment.texture = texture;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = d.depthClearValue;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> clear = [commands renderCommandEncoderWithDescriptor:pass];
            if (!clear) { fail("depth clear encoder"); return nullptr; }
            [clear endEncoding];
        }
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!blit) { fail("depth copy encoder"); return nullptr; }
        [blit copyFromTexture:depth.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(std::min(depth.texture.width, texture.width), std::min(depth.texture.height, texture.height), 1)
                    toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        ++depthResized;
        depth = {texture, false};
        return &depth;
    }
    depth = {texture, true};
    return &depth;
}
} // namespace maxps4::gpu
