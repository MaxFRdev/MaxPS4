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
// Where the binary archives of compiled pipelines live (AsyncPipelines::openArchive).
static NSURL* archiveFolder() {
    NSURL* caches = [[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    return caches ? [caches URLByAppendingPathComponent:@"MaxPS4/metal" isDirectory:YES] : nil;
}
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
    // Colour targets 1-7: their formats, masks and blends shape the pipeline.
    for (const auto& t : d.more) {
        if (!t.base) { key += '\0'; continue; }
        const std::uint32_t words[5] = {t.format | t.number << 8 | t.swap << 16, t.mask, (t.blend >> 30) & 1 ? t.blend : 0u, 1, 0};
        key.append(reinterpret_cast<const char*>(words), sizeof words);
    }
    return key;
}
// The colour attachments a draw renders: bit k for slot k.
unsigned colourSlots(const PM4Draw& d) {
    if (!d.colour) return 0;
    unsigned slots = 1;
    for (unsigned k = 1; k < 8; ++k) if (d.more[k - 1].base) slots |= 1u << k;
    return slots;
}
}

namespace {
// Adds the time until it goes out of scope to `total`, in milliseconds.
struct Stopwatch {
    double& total; std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    ~Stopwatch() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count(); }
};
}

id<MTLRenderPipelineState> PM4MetalRenderer::pipeline(const PM4Draw& d, MTLPixelFormat format, MTLPixelFormat depth) {
    const PipelineKey key{d.vs, d.ps, d.vsUser[0], d.vsUser[1], d.psInputAddr, inputKey(d), unsigned(format),
                          (d.blend >> 30) & 1 ? d.blend : 0u, d.mask, unsigned(depth)};
    untranslated_ = false;
    if (untranslatedPrograms_.count({d.vs, d.ps})) { untranslated_ = true; return nil; }
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
    // A mission's first frames met hundreds of new pipelines, each translated
    // here on the game's submission thread (some programs make 372 KB of MSL):
    // 1-2 images/s. Past 6 ms of translation in a 16 ms window, the draw waits
    // for a later frame, like a pipeline Metal is still building.
    const auto translateStart = std::chrono::steady_clock::now();
    if (asyncPipelines) {
        if (translateStart - translateWindow_ > std::chrono::milliseconds(16)) { translateWindow_ = translateStart; translateWindowMs_ = 0; }
        if (translateWindowMs_ > 6.0) { ++translationsPutOff; deferred_ = true; return nil; }
    }
    struct Spent {
        PM4MetalRenderer& r; std::chrono::steady_clock::time_point since;
        ~Spent() {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
            r.translateMs += ms; r.translateWindowMs_ += ms;
        }
    } spent{*this, translateStart};
    GCNTranslateOptions vsOptions;
    vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = d.vsUserCount;
    for (unsigned k = 0; k < 16; ++k) vsOptions.userData[k] = d.vsUser[k];
    GCNTranslateOptions psOptions;
    psOptions.name = "ps_program"; psOptions.userCount = d.psUserCount;
    psOptions.directLoads = native_ || !guest_; // Each draw is scanned: its table is complete.
    for (unsigned k = 0; k < 16; ++k) psOptions.userData[k] = d.psUser[k];
    compiling = 3;
    const auto vs = translateCached(d.vs, vsOptions);
    // A depth-only draw may have no pixel program (Gnm's null PS): an empty one.
    GCNTranslation ps;
    if (d.ps || d.colour) ps = translateCached(d.ps, psOptions);
    else ps.source = "GCN_FN void ps_program(GCN_THREAD GCNLane& L, GCNMemory M, GCN_THREAD GCNQuad& Q) { (void)L; (void)M; (void)Q; }\n";
    compiling = 0;
    // A program MaxPS4 cannot translate yet: its draws are left out and the
    // game goes on (CoD's missions met a new instruction every few minutes).
    if (!vs || !ps) {
        char where[96];
        std::snprintf(where, sizeof where, " (VS 0x%llx, PS 0x%llx)", (unsigned long long)d.vs, (unsigned long long)d.ps);
        lastUntranslated = (!vs ? "VS : " + vs.error : "PS : " + ps.error) + where;
        untranslatedPrograms_.insert({d.vs, d.ps});
        untranslated_ = true;
        return nil;
    }
    {
        ScalarSites& sites = scalarSites_[d.ps];
        sites.list = ps.sites; sites.words = ps.siteWords;
        scalarLoadsAll += unsigned(ps.sites.size());
        for (const auto& site : ps.sites) if (site.from[0]) ++scalarLoadsDirect;
        std::sort(sites.list.begin(), sites.list.end(), [](const GCNScalarSite& a, const GCNScalarSite& b) { return a.pc < b.pc; });
    }
    std::string source = gcnMetalPrelude(native_, gcnHighestUsed(vs.params), gcnHighestUsed(ps.attributes), gcnHighestUsed(ps.colors)) + vs.source + ps.source +
                               gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", d.psInputAddr, d.inputCntl, native_, colourSlots(d));
    // Tests: a sample Metal's sampler refuses reads as zero instead of filtering in
    // software, which shows whether a draw really takes the native path.
    // Programs with many samples get the same: each inlined gcn_sample carries
    // the whole software sampler (every format, tiling, BC blocks), and CoD's
    // final composite (19 samples of colour targets, always native) never
    // finished its pipeline on the iPhone, so the menu never reached the screen.
    std::size_t sampleSites = 0;
    for (std::size_t at = 0; (at = ps.source.find("gcn_sample(M", at)) != std::string::npos; ++at) ++sampleSites;
    if (native_ && (nativeSamplesOnly || sampleSites >= manySamples)) {
        if (!nativeSamplesOnly) ++nativeOnlyPrograms;
        const std::string sample = "if (gcn_native_sample(M, t, s, unorm, u, v, filtered)) return gcn_texture_swizzle(t, filtered);";
        const auto at = source.find(sample);
        if (at != std::string::npos) source.insert(at + sample.size(), "\n    if (true) return float4(0.0f, 0.0f, 0.0f, 0.0f);");
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    NSError* failure = nil;
    descriptor.depthAttachmentPixelFormat = depth;
    // The depth texture carries the stencil too.
    descriptor.stencilAttachmentPixelFormat = depth == MTLPixelFormatDepth32Float_Stencil8 ? depth : MTLPixelFormatInvalid;
    // Without colour, the pixel program only matters when it may discard.
    const bool fragment = d.colour || ps.mayKill;
    // Slot 0 is CB_COLOR0; slots 1-7 are the draw's other targets.
    auto attach = [&](unsigned slot, MTLPixelFormat pixelFormat, unsigned mask, std::uint32_t blendControl, bool exported) {
        MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[slot];
        color.pixelFormat = pixelFormat;
        // A target the program does not export keeps its pixels.
        if (!exported) mask = 0;
        color.writeMask = ((mask & 1) ? MTLColorWriteMaskRed : 0) | ((mask & 2) ? MTLColorWriteMaskGreen : 0) |
                          ((mask & 4) ? MTLColorWriteMaskBlue : 0) | ((mask & 8) ? MTLColorWriteMaskAlpha : 0);
        if (!d.colour || !((blendControl >> 30) & 1)) return true;
        const auto blend = decodePM4Blend(blendControl);
        auto alpha = blend.alpha;
        // Saturation has an alpha factor of one, independently of RGB.
        if (alpha.source == 10) alpha.source = 1;
        if (alpha.destination == 10) alpha.destination = 1;
        MTLBlendFactor cs, cd, as, ad;
        MTLBlendOperation colorOperation, alphaOperation;
        if (!blendChannel(blend.rgb, colorOperation, cs, cd) ||
            !blendChannel(alpha, alphaOperation, as, ad)) {
            fail("blend mode");
            return false;
        }
        color.blendingEnabled = YES;
        color.rgbBlendOperation = colorOperation; color.alphaBlendOperation = alphaOperation;
        color.sourceRGBBlendFactor = cs; color.destinationRGBBlendFactor = cd;
        color.sourceAlphaBlendFactor = as; color.destinationAlphaBlendFactor = ad;
        return true;
    };
    if (!attach(0, format, d.mask, d.blend, true)) return nil;
    for (unsigned k = 1; k < 8; ++k) {
        const auto& t = d.more[k - 1];
        if (t.base && !attach(k, targetFormat(t.format, t.number, t.swap), t.mask, t.blend, (ps.colors >> k) & 1)) return nil;
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
        async->openArchive(device);
        NSString* text = [NSString stringWithUTF8String:source.c_str()];
        const std::size_t sourceHash = std::hash<std::string>{}(source), sourceLength = source.size();
        auto build = [=](MTLCompileOptions* options, bool fast) {
            auto made = [=](id<MTLLibrary> library, NSError* error) {
                if (!library) { finished(nil, error, fast); return; }
                { std::lock_guard<std::mutex> lock(async->mutex);
                  const auto pending = async->pending.find(key);
                  if (pending != async->pending.end()) pending->second.stage = "render pipeline creation"; }
                MTLRenderPipelineDescriptor* own = [descriptor copy];
                own.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
                own.fragmentFunction = fragment ? [library newFunctionWithName:@"gcn_fragment"] : nil;
                // The blocks below capture plain locals only: a block in a lambda
                // nested in this one, reading that lambda's captured descriptor,
                // was copied by Metal from a stack address (the app died in
                // _Block_copy on the first archive builds).
                std::shared_ptr<AsyncPipelines> pool = async;
                std::function<void(id<MTLRenderPipelineState>, NSError*)> done =
                    [finished, fast](id<MTLRenderPipelineState> state, NSError* error) { finished(state, error, fast); };
                MTLRenderPipelineDescriptor* desc = own;
                id<MTLDevice> gpu = device;
                NSArray<id<MTLBinaryArchive>>* lookups = nil;
                { std::lock_guard<std::mutex> lock(pool->mutex); lookups = pool->lookups; }
                if (!lookups) { pool->compile(gpu, desc, done); return; }
                // From earlier launches' archives only: a miss compiles as before.
                // Synchronous (a lookup is quick): the asynchronous call with
                // FailOnBinaryArchiveMiss crashed the iPhone's driver, copying a
                // block from a stack address inside dispatch_async.
                desc.binaryArchives = lookups;
                id<MTLRenderPipelineState> found = nil;
                @try {
                    NSError* missed = nil;
                    found = [gpu newRenderPipelineStateWithDescriptor:desc options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                           reflection:nil error:&missed];
                } @catch (NSException*) { found = nil; }
                bool first = false;
                { std::lock_guard<std::mutex> lock(pool->mutex);
                  ++(found ? pool->archiveHits : pool->archiveMisses);
                  first = found && pool->archiveHits == 1; }
                // A lookup that worked: this launch got past the archive code.
                if (first) [[NSFileManager defaultManager] removeItemAtURL:[archiveFolder() URLByAppendingPathComponent:@"unfinished"] error:nil];
                if (found) done(found, nil); else pool->compile(gpu, desc, done);
            };
            const AsyncPipelines::LibraryKey libraryKey{sourceHash, sourceLength, fast};
            id<MTLLibrary> have = nil;
            bool start = false;
            { std::lock_guard<std::mutex> lock(async->mutex);
              const auto found = async->libraries.find(libraryKey);
              if (found != async->libraries.end()) { have = found->second; ++async->librariesShared; }
              else {
                  auto& waiters = async->compiling[libraryKey];
                  start = waiters.empty();
                  if (start) ++async->librariesCompiled; else ++async->librariesShared;
                  waiters.push_back(made);
              } }
            if (have) { made(have, nil); return; }
            if (!start) return; // the compile in flight calls it
            async->startCompile([=] {
                [device newLibraryWithSource:text options:options completionHandler:^(id<MTLLibrary> library, NSError* error) {
                    std::vector<std::function<void(id<MTLLibrary>, NSError*)>> waiters;
                    { std::lock_guard<std::mutex> lock(async->mutex);
                      if (library) async->libraries[libraryKey] = library;
                      waiters.swap(async->compiling[libraryKey]);
                      async->compiling.erase(libraryKey); }
                    async->compileDone();
                    for (auto& waiter : waiters) waiter(library, error);
                }];
            });
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

// One program draws with many render states (targets, blends, depth): each
// state is a pipeline, but the program's translation is the same.
GCNTranslation PM4MetalRenderer::translateCached(std::uint64_t address, const GCNTranslateOptions& options) {
    std::string key(reinterpret_cast<const char*>(&address), sizeof address);
    const std::uint32_t flags[3] = {std::uint32_t(options.stage), options.userCount, options.directLoads};
    key.append(reinterpret_cast<const char*>(flags), sizeof flags);
    key.append(reinterpret_cast<const char*>(options.userData), sizeof options.userData);
    key += options.name;
    // The program's memory may hold another program later (a level loaded):
    // its first words are part of the key.
    std::uint32_t words[8] = {};
    for (unsigned k = 0; k < 8 && code_.read(address + 4 * k, &words[k], 4); ++k) {}
    key.append(reinterpret_cast<const char*>(words), sizeof words);
    const auto found = translations_.find(key);
    if (found != translations_.end()) { ++translationsReused; return found->second; }
    GCNTranslation made = translateGCN(code_, address, options);
    if (translationBytes_ > (48u << 20)) { translations_.clear(); translationBytes_ = 0; }
    translationBytes_ += made.source.size() + key.size();
    translations_.emplace(std::move(key), made);
    return made;
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
    char queue[200];
    std::snprintf(queue, sizeof queue, " · bibliothèques : %u compilées, %u partagées, %u en compilation (dont %u pipelines), %zu en file", async_->librariesCompiled, async_->librariesShared, async_->compilesRunning, async_->pipelinesRunning, async_->compileQueue.size());
    std::string defaults = queue;
    std::snprintf(queue, sizeof queue, " · builds par défaut : %zu en attente", async_->later.size());
    defaults = async_->archiveReport() + defaults + queue;
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

// Gnmx's buffer fill (cs_set_uint): out[i] = table[i & mask] below a count (1).
// CoD's clear: out[i] = constant buffer's first word below out's record count (2).
int PM4MetalRenderer::fillProgram(std::uint64_t address) {
    const auto known = fillPrograms_.find(address);
    if (known != fillPrograms_.end()) return known->second;
    static const char* const table[] = {
        "s_mov_b32 vcc_hi, 0xd", "s_lshl_b32 vcc_lo, s12, 6", "s_buffer_load_dword s12, s[8:11], 0x0",
        "v_add_i32 v0, vcc, vcc_lo, v0", "s_waitcnt lgkmcnt(0)", "v_cmpx_gt_u32 vcc, s12, v0", "s_cbranch_execz 18",
        "s_buffer_load_dword s8, s[8:11], 0x1", "s_waitcnt lgkmcnt(0)", "v_and_b32 v1, s8, v0",
        "buffer_load_format_x v1, v1, s[0:3], 0 idxen", "v_cmpx_gt_u32 s[0:1], s6, v0", "s_waitcnt vmcnt(0) lgkmcnt(15)",
        "buffer_store_format_x v1, v0, s[4:7], 0 idxen", "s_endpgm", nullptr};
    // CoD's missions ran it about 75 times a second, each a GPU wait and 2.3 MiB
    // copied in and out (22 s of waiting in 80 s, 2 images/s).
    static const char* const constant[] = {
        "s_mov_b32 vcc_hi, 0x9", "s_buffer_load_dword s4, s[4:7], 0x0", "s_mul_i32 vcc_lo, s8, 64",
        "v_sad_u32 v0, vcc_lo, 0, v0", "s_waitcnt lgkmcnt(0)", "v_mov_b32 v1, s4", "v_cmpx_gt_u32 s[4:5], s2, v0",
        "buffer_store_format_x v1, v0, s[0:3], 0 idxen", "s_endpgm", nullptr};
    std::uint32_t words[32] = {};
    const bool readable = code_.read(address, words, sizeof words);
    auto matches = [&](const char* const* expected, const char* shifted, const char* multiplied) {
        if (!readable) return false;
        std::size_t at = 0;
        for (const char* const* line = expected; *line; ++line) {
            GCNInstruction i;
            if (decodeGCNInstruction(words + at, 32 - at, at, i) != GCNError::none) return false;
            const std::string text = gcnDisassemble(i);
            // Sonic Mania's compiler multiplies where Mortal Kombat's shifts.
            if (text != *line && !(std::string(*line) == shifted && text == multiplied) &&
                !(std::string(*line) == multiplied && text == shifted)) return false;
            at += i.size;
        }
        return true;
    };
    int kind = matches(table, "s_lshl_b32 vcc_lo, s12, 6", "s_mul_i32 vcc_lo, s12, 64") ? 1
             : matches(constant, "s_lshl_b32 vcc_lo, s8, 6", "s_mul_i32 vcc_lo, s8, 64") ? 2 : 0;
    return fillPrograms_[address] = kind;
}

// A fill without its kernel, whose lanes cost far more than the stores: a blit
// into a target, or a write to guest memory. False: run it as any dispatch.
bool PM4MetalRenderer::fillDispatch(const PM4Dispatch& c, id<MTLCommandBuffer> commands) {
    if ((c.rsrc2 & 0x380) != 0x80 || c.threads[0] != 64 || c.threads[1] != 1 || c.threads[2] != 1 ||
        c.groups[1] != 1 || c.groups[2] != 1) return false;
    const int kind = fillProgram(c.program);
    if (!kind || c.userCount != (kind == 1 ? 12u : 8u)) return false;
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
    std::uint64_t bytes = 0, address = 0;
    if (kind == 1) {
        if (!dwords(0) || !dwords(4)) return refuse("format", base(4));
        if (!c.user[10] || !code_.read(base(8), limits, sizeof limits)) return refuse("limites illisibles", base(8));
        if (limits[1]) return refuse("motif de plusieurs valeurs", base(4), limits[1]);
        if (c.user[2] && !code_.read(base(0), &value, 4)) return refuse("valeur illisible", base(0));
        bytes = 4 * std::min<std::uint64_t>({std::uint64_t(c.groups[0]) * 64, limits[0], c.user[6]});
        address = base(4);
    } else {
        // s[0:3]: the buffer written, its record count the limit; s[4:7]: the value's constant buffer.
        if (!dwords(0)) return refuse("format", base(0));
        if (!c.user[6] || !code_.read(base(4), &value, 4)) return refuse("valeur illisible", base(4));
        bytes = 4 * std::min<std::uint64_t>(std::uint64_t(c.groups[0]) * 64, c.user[2]);
        address = base(0);
    }
    if (!bytes || bytes > (256ull << 20)) return refuse("taille", address, bytes);
    auto overlaps = [&](std::uint64_t at, std::uint64_t length) { return address < at + length && at < address + bytes; };
    Target* target = nullptr;
    for (auto& [at, candidate] : targets_) {
        if (!candidate.texture) continue;
        const std::uint64_t row = candidate.texture.width * bytesPerPixel(candidate.texture.pixelFormat), length = row * candidate.texture.height;
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
        const std::uint64_t row = target->texture.width * bytesPerPixel(target->texture.pixelFormat);
        [blit copyFromBuffer:fill->second sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:bytes
                  sourceSize:MTLSizeMake(target->texture.width, bytes / row, 1)
                   toTexture:target->texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        target->fresh = false;
        ++dispatchesEncoded;
        ++targetsSynced;
        char line[96];
        std::snprintf(line, sizeof line, "\n  - remplissage 0x%08x, %llu octets", value, (unsigned long long)bytes);
        if (shown_.count(address)) {
            char probeLine[128];
            std::snprintf(probeLine, sizeof probeLine, "\n  - 0x%llx apres remplissage 0x%08x :", (unsigned long long)address, value);
            probe(target->texture, address, probeLine, commands);
        }
        noteInto(address, line);
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
    const auto& scanned = scanner_.scan(c);
    if (!scanner_.dispatchError().empty())
        return fail("compute resource scan stopped: " + scanner_.dispatchError());
    // A kernel that addresses no memory (LDS only, or registers alone) has
    // nothing visible to change.
    if (scanned.empty() && !scanner_.stored().empty()) return fail("compute program addresses no memory");
    if (scanned.empty()) { ++dispatchesWithoutEffect; lastDispatchNote_ = "aucune mémoire adressée"; return true; }
    // As for draws: over 16 ranges, joined across their smallest gaps, unless
    // that copies 8 MiB more than the kernel's own ranges.
    const std::vector<PM4Range> ranges = scanned.size() > 16 ? coalesceRanges(scanned, 16) : scanned;
    if (scanned.size() > 16) {
        std::uint64_t own = 0, joined = 0;
        for (const auto& r : scanned) own += r.size;
        for (const auto& r : ranges) joined += r.size;
        if (joined > own + (8ull << 20)) return fail("compute reads more than 16 memory ranges");
    }
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
        subtractTexture(address, std::uint64_t(target.texture.width) * target.texture.height * bytesPerPixel(target.texture.pixelFormat));
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
        const std::uint64_t row = target.texture.width * bytesPerPixel(target.texture.pixelFormat), length = row * target.texture.height;
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
        const std::uint64_t row = target.texture.width * bytesPerPixel(target.texture.pixelFormat), length = row * target.texture.height;
        if (!target.texture || !stored(address, length) || !locate(address, length, at)) continue;
        [blit copyFromBuffer:memory sourceOffset:at sourceBytesPerRow:row sourceBytesPerImage:length
                  sourceSize:MTLSizeMake(target.texture.width, target.texture.height, 1)
                   toTexture:target.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        target.fresh = false;
        ++targetsSynced;
        char line[160];
        std::snprintf(line, sizeof line, "\n  - calcul programme 0x%llx, groupes %ux%ux%u", (unsigned long long)c.program,
                      c.groups[0], c.groups[1], c.groups[2]);
        noteInto(address, line);
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
        Stopwatch watch{copyMs};
        copyGuest(base, static_cast<std::uint8_t*>(shared.buffer.contents) + shared.offset, size);
    }
    ++rangesShared;
    shared_[{base, size}] = shared;
    return shared;
}

id<MTLBuffer> PM4MetalRenderer::uploadBuffer(std::size_t bytes, id<MTLCommandBuffer> commands) {
    // Each draw keeps its own immutable snapshot until its command buffer has
    // finished. In particular, two draws in the same batch cannot share a slot.
    // Small requests are rounded up to a power of two so that slots match again
    // (CoD's missions made 126 000 new buffers in 75 s with exact sizes).
    if (bytes <= (1u << 20)) { std::size_t rounded = 4096; while (rounded < bytes) rounded <<= 1; bytes = rounded; }
    Upload* best = nullptr;
    for (auto& upload : uploads_) {
        if (upload.length < bytes || (best && upload.length >= best->length)) continue;
        if (!upload.free) {
            const auto status = upload.owner.status;
            upload.free = status == MTLCommandBufferStatusCompleted || status == MTLCommandBufferStatusError;
        }
        if (upload.free) best = &upload;
    }
    if (best) {
        best->owner = commands;
        best->free = false;
        ++buffersReused;
        return best->buffer;
    }
    Stopwatch watch{allocateMs};
    id<MTLBuffer> buffer = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer) {
        ++buffersBuilt;
        // Shared ranges are atlases of up to 64 MiB: keep a frame's worth or two.
        constexpr std::uint64_t budget = 384ull << 20;
        if (uploads_.size() < 4096 && bytes <= budget - pooledBytes_) {
            uploads_.push_back({buffer, commands, bytes, false});
            pooledBytes_ += bytes;
        }
    }
    return buffer;
}

// A BC1-BC5 or BC7 T# (dfmt 35-39, 41) in tile mode 13 (Thin_1dThin:
// blocks in 8x8 micro tiles, Morton order inside) decoded once into a
// texture: Metal cannot sample BC formats on iPhone. CoD's menus and fonts
// are BC3 and BC1.
namespace {
// A signed (SNORM) BC4 block: 4x4 int8 values, every `step` bytes in a row of
// `pitch` bytes (bcdec decodes BC4/BC5 unsigned only). BC5 normal maps are
// often SNORM.
void decodeBC4Signed(const std::uint8_t* block, std::uint8_t* out, int pitch, int step) {
    int a = std::int8_t(block[0]), b = std::int8_t(block[1]);
    a = std::max(a, -127); b = std::max(b, -127);
    int palette[8] = {a, b};
    if (a > b) for (int k = 1; k < 7; ++k) palette[k + 1] = ((7 - k) * a + k * b) / 7;
    else { for (int k = 1; k < 5; ++k) palette[k + 1] = ((5 - k) * a + k * b) / 5; palette[6] = -127; palette[7] = 127; }
    std::uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= std::uint64_t(block[2 + k]) << (8 * k);
    for (int i = 0; i < 16; ++i)
        out[(i / 4) * pitch + (i % 4) * step] = std::uint8_t(std::int8_t(palette[(bits >> (3 * i)) & 7]));
}
}

// Decoded textures stay until 512 MB of them, least recently used first out
// (a mission uses many more than the 32 kept before: each was decoded again
// on the CPU, batches of 400 ms). A game fills its atlases as it loads: 16
// bytes of every 4 KiB tell whether any part has changed since the decode,
// sampled at most every 250 ms.
id<MTLTexture> PM4MetalRenderer::decodedTexture(const DecodedKey& key, std::uint64_t base, std::uint64_t bytes, std::uint64_t& mark) {
    const auto found = bc7Textures_.find(key);
    const auto now = std::chrono::steady_clock::now();
    if (found != bc7Textures_.end()) {
        if (found->second.batch == batchSerial_) return found->second.texture;
        if (now - found->second.checked < std::chrono::milliseconds(250)) { found->second.batch = batchSerial_; return found->second.texture; }
    }
    mark = 1469598103934665603ull;
    for (std::uint64_t at = 0; at < bytes; at += 4096) {
        std::uint8_t sample[16] = {};
        copyGuest(base + at, sample, std::min<std::uint64_t>(bytes - at, sizeof sample));
        for (const std::uint8_t byte : sample) mark = (mark ^ byte) * 1099511628211ull;
    }
    if (found != bc7Textures_.end() && found->second.mark == mark) {
        found->second.batch = batchSerial_; found->second.checked = now;
        return found->second.texture;
    }
    return nil;
}

void PM4MetalRenderer::keepDecoded(const DecodedKey& key, id<MTLTexture> texture, std::uint64_t mark, std::uint64_t bytes) {
    if (const auto old = bc7Textures_.find(key); old != bc7Textures_.end()) { decodedBytes_ -= old->second.bytes; bc7Textures_.erase(old); }
    constexpr std::uint64_t budget = 512ull << 20;
    while (!bc7Textures_.empty() && decodedBytes_ + bytes > budget) {
        auto oldest = bc7Textures_.begin();
        for (auto at = bc7Textures_.begin(); at != bc7Textures_.end(); ++at) if (at->second.batch < oldest->second.batch) oldest = at;
        decodedBytes_ -= oldest->second.bytes;
        bc7Textures_.erase(oldest);
    }
    bc7Textures_[key] = {texture, mark, batchSerial_, std::chrono::steady_clock::now(), bytes};
    decodedBytes_ += bytes;
}

id<MTLTexture> PM4MetalRenderer::bc7Texture(std::uint64_t base, std::uint64_t width, std::uint64_t height, std::uint64_t pitch, unsigned dfmt, bool snorm) {
    const std::uint64_t blockBytes = dfmt == 35 || dfmt == 38 ? 8 : 16;
    // BC4 decodes to one channel, BC5 to two, the others to RGBA8.
    const std::uint64_t pixelBytes = dfmt == 38 ? 1 : dfmt == 39 ? 2 : 4;
    const std::uint64_t blocksWide = (pitch + 3) / 4, blocksHigh = (height + 3) / 4;
    const std::uint64_t tilesWide = (blocksWide + 7) / 8, tilesHigh = (blocksHigh + 7) / 8;
    const std::uint64_t bytes = tilesWide * tilesHigh * 64 * blockBytes;
    if (!bytes || bytes > (64ull << 20)) return nil;
    const DecodedKey key = std::make_tuple(base, width | std::uint64_t(dfmt) << 32 | std::uint64_t(snorm) << 40, height);
    std::uint64_t mark = 0;
    if (id<MTLTexture> kept = decodedTexture(key, base, bytes, mark)) return kept;
    Stopwatch watch{textureMs};
    ++texturesDecoded;
    std::vector<std::uint8_t> blocks(bytes);
    copyGuest(base, blocks.data(), bytes);
    const std::uint64_t rowBytes = blocksWide * 4 * pixelBytes;
    std::vector<std::uint8_t> pixels(rowBytes * blocksHigh * 4);
    for (std::uint64_t y = 0; y < blocksHigh; ++y) for (std::uint64_t x = 0; x < blocksWide; ++x) {
        const std::uint64_t morton = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
        const std::uint64_t at = ((y / 8 * tilesWide + x / 8) * 64 + morton) * blockBytes;
        auto* out = pixels.data() + y * 4 * rowBytes + x * 4 * pixelBytes;
        switch (dfmt) {
        case 38:
            if (snorm) decodeBC4Signed(blocks.data() + at, out, int(rowBytes), 1);
            else bcdec_bc4(blocks.data() + at, out, int(rowBytes));
            break;
        case 39:
            if (snorm) { decodeBC4Signed(blocks.data() + at, out, int(rowBytes), 2); decodeBC4Signed(blocks.data() + at + 8, out + 1, int(rowBytes), 2); }
            else bcdec_bc5(blocks.data() + at, out, int(rowBytes));
            break;
        case 35: bcdec_bc1(blocks.data() + at, out, int(rowBytes)); break;
        case 36: bcdec_bc2(blocks.data() + at, out, int(rowBytes)); break;
        case 37: bcdec_bc3(blocks.data() + at, out, int(rowBytes)); break;
        default: bcdec_bc7(blocks.data() + at, out, int(rowBytes)); break;
        }
    }
    const MTLPixelFormat pixelFormat = dfmt == 38 ? (snorm ? MTLPixelFormatR8Snorm : MTLPixelFormatR8Unorm)
                                     : dfmt == 39 ? (snorm ? MTLPixelFormatRG8Snorm : MTLPixelFormatRG8Unorm) : MTLPixelFormatRGBA8Unorm;
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:rowBytes];
    keepDecoded(key, texture, mark, pixels.size());
    return texture;
}

// An uncompressed T# in tile mode 13 (Thin_1dThin, non-displayable micro
// tiles: 8x8 pixels, x0 y0 x1 y1 x2 y2 order) read into a linear texture.
MTLPixelFormat PM4MetalRenderer::nativeFormat(unsigned dfmt, unsigned nfmt, unsigned& pixelBytes) const {
    // 32-bit float channels filter only where the GPU says so (A14 and later).
    bool floatFilter = false;
    if (@available(macOS 11.0, iOS 14.0, *)) floatFilter = device_.supports32BitFloatFiltering;
    pixelBytes = 0;
    switch (dfmt) {
    case 1: if (nfmt == 0) { pixelBytes = 1; return MTLPixelFormatR8Unorm; } break;
    case 2: if (nfmt == 0 || nfmt == 7) { pixelBytes = 2; return nfmt ? MTLPixelFormatR16Float : MTLPixelFormatR16Unorm; } break;
    case 3: if (nfmt == 0) { pixelBytes = 2; return MTLPixelFormatRG8Unorm; } break;
    case 4: if (nfmt == 7 && floatFilter) { pixelBytes = 4; return MTLPixelFormatR32Float; } break;
    case 5: if (nfmt == 0 || nfmt == 7) { pixelBytes = 4; return nfmt ? MTLPixelFormatRG16Float : MTLPixelFormatRG16Unorm; } break;
    // 2_10_10_10 lists its channels high to low: X in the low 10 bits, as RGB10A2.
    case 9: if (nfmt == 0) { pixelBytes = 4; return MTLPixelFormatRGB10A2Unorm; } break;
    case 10: pixelBytes = 4; return MTLPixelFormatRGBA8Unorm; // gcn_texel_at reads it as unorm whatever the nfmt
    case 11: if (nfmt == 7 && floatFilter) { pixelBytes = 8; return MTLPixelFormatRG32Float; } break;
    case 12: if (nfmt == 0 || nfmt == 7) { pixelBytes = 8; return nfmt ? MTLPixelFormatRGBA16Float : MTLPixelFormatRGBA16Unorm; } break;
    case 14: if (nfmt == 7 && floatFilter) { pixelBytes = 16; return MTLPixelFormatRGBA32Float; } break;
    case 16: pixelBytes = 2; return MTLPixelFormatB5G6R5Unorm;
    default: break;
    }
    return MTLPixelFormatInvalid;
}

id<MTLTexture> PM4MetalRenderer::thinTexture(std::uint64_t base, std::uint64_t width, std::uint64_t height, std::uint64_t pitch, unsigned dfmt,
                                             MTLPixelFormat format, unsigned pixelBytesIn) {
    const std::uint64_t pixelBytes = pixelBytesIn;
    const std::uint64_t tilesWide = (pitch + 7) / 8, tilesHigh = (height + 7) / 8;
    const std::uint64_t bytes = tilesWide * tilesHigh * 64 * pixelBytes;
    if (!bytes || bytes > (64ull << 20) || width > pitch) return nil;
    const DecodedKey key = std::make_tuple(base, width | std::uint64_t(dfmt) << 32 | std::uint64_t(format) << 40, height);
    std::uint64_t mark = 0;
    if (id<MTLTexture> kept = decodedTexture(key, base, bytes, mark)) return kept;
    Stopwatch watch{textureMs};
    ++texturesDecoded;
    std::vector<std::uint8_t> tiled(bytes);
    copyGuest(base, tiled.data(), bytes);
    const std::uint64_t rowBytes = width * pixelBytes;
    std::vector<std::uint8_t> pixels(rowBytes * height);
    for (std::uint64_t y = 0; y < height; ++y) for (std::uint64_t x = 0; x < width; ++x) {
        const std::uint64_t morton = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
        const std::uint64_t at = ((y / 8 * tilesWide + x / 8) * 64 + morton) * pixelBytes;
        std::memcpy(pixels.data() + y * rowBytes + x * pixelBytes, tiled.data() + at, pixelBytes);
    }
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (!texture) return nil;
    [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:rowBytes];
    keepDecoded(key, texture, mark, pixels.size());
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
    // A colour target lives only in its Metal texture, never written back to
    // guest memory: a T# at its address in its format reads it there (an
    // emulator renders its console's frame, then draws that frame scaled on
    // screen; a 3D game samples its 16-bit float scene to tone-map it). Before
    // the tiled decoders below: a tiled T# of a target reads the target too.
    if (const auto rendered = targets_.find(base); rendered != targets_.end() && rendered->second.format == dfmt) {
        id<MTLTexture> texture = rendered->second.texture;
        bool attached = texture == drawing;
        for (id<MTLTexture> other : drawingMore_) attached = attached || (other && texture == other);
        if (texture && !attached && !rendered->second.fresh && texture.width >= width && texture.height >= height) {
            ++targetReads;
            return texture;
        }
    }
    if (dfmt == 41 || (dfmt >= 35 && dfmt <= 39)) {
        // NFMT 0 unorm; BC4/BC5 also 1 snorm (normal maps).
        const unsigned nfmt = (t[1] >> 26) & 15;
        const bool snorm = nfmt == 1 && (dfmt == 38 || dfmt == 39);
        if (((t[3] >> 20) & 31) != 13 || (nfmt != 0 && !snorm)) return miss(1, nfmt);
        id<MTLTexture> decoded = bc7Texture(base, width, height, pitch, dfmt, snorm);
        return decoded ? decoded : miss(4, 0);
    }
    // Unorm and float formats (nativeFormat). Programs with many samples
    // read any other T# as zero (see manySamples): CoD's lighting reads
    // 16-bit float and 16_16 images that were all refused, 68861 in a mission.
    unsigned pixelBytes = 0;
    const unsigned nfmt = (t[1] >> 26) & 15, tiling = (t[3] >> 20) & 31;
    const MTLPixelFormat format = nativeFormat(dfmt, nfmt, pixelBytes);
    if (format == MTLPixelFormatInvalid) return miss(1, nfmt);
    // Tile mode 13: 8x8 pixel micro tiles, Morton order inside, decoded once
    // (CoD's menu frames and gradients). 5_6_5 stays linear only.
    if (tiling == 13 && dfmt != 16) {
        id<MTLTexture> decoded = thinTexture(base, width, height, pitch, dfmt, format, pixelBytes);
        return decoded ? decoded : miss(4, 0);
    }
    // Linear (tile mode 8): read, and filtered by Metal's sampler, through a view.
    if (tiling != 8 || pitch < width) return miss(1, 100 + tiling);
    const std::uint64_t rowBytes = pitch * pixelBytes, bytes = rowBytes * height;
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
    Stopwatch watch{encodeMs};
    ++encodedDraws;
    error.clear();
    if (!d.colour && !d.depthTarget) return fail("draw with neither colour nor depth");
    // Gnm's embedded full-screen VS has no guest code (address 0). A game clears
    // depth with it, several times a frame: no draw, the next pass that uses
    // this buffer clears it on load, so an unused buffer costs nothing.
    if (!d.vs && !d.colour) {
        ++embeddedDepthDraws;
        if (!d.depthClear && !d.stencilClear) return true;
        Target& cleared = depthTargets_[d.depthTarget];
        if (cleared.texture && encoder_ && depthAttachment_ == cleared.texture) {
            [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil;
        }
        // Depth and stencil load separately: a clear of one keeps the other.
        if (d.depthClear) depthClearTo_[d.depthTarget] = d.depthClearValue;
        if (d.stencilClear) stencilClearTo_[d.depthTarget] = d.stencilClearValue;
        return true;
    }
    if ((d.vte & 0x3f) != 0x3f) return fail("viewport without scale and offset");
    const MTLPixelFormat format = d.colour ? targetFormat(d.format, d.number, d.swap) : MTLPixelFormatInvalid;
    if (d.colour && format == MTLPixelFormatInvalid)
        return fail(d.format == 0xa && d.number == 0 ? "colour target component swap " + std::to_string(d.swap) :
                    "colour target format " + std::to_string(d.format) + " number " + std::to_string(d.number) + " swap " + std::to_string(d.swap));
    // Apple GPUs do not blend 32-bit float targets.
    if (d.colour && ((d.blend >> 30) & 1) &&
        (format == MTLPixelFormatR32Float || format == MTLPixelFormatRG32Float || format == MTLPixelFormatRGBA32Float))
        return fail("blending into a 32-bit float colour target");
    MTLPixelFormat moreFormats[7] = {};
    for (unsigned k = 0; k < 7; ++k) {
        const auto& t = d.more[k];
        if (!t.base) continue;
        moreFormats[k] = targetFormat(t.format, t.number, t.swap);
        if (moreFormats[k] == MTLPixelFormatInvalid)
            return fail("colour target " + std::to_string(k + 1) + " format " + std::to_string(t.format) + " number " + std::to_string(t.number));
        if (((t.blend >> 30) & 1) && (moreFormats[k] == MTLPixelFormatR32Float || moreFormats[k] == MTLPixelFormatRG32Float ||
                                      moreFormats[k] == MTLPixelFormatRGBA32Float))
            return fail("blending into a 32-bit float colour target");
    }
    const MTLPixelFormat depthFormat = d.depthTarget ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
    if (d.scissor[2] <= d.scissor[0] || d.scissor[3] <= d.scissor[1]) return true; // nothing visible
    // Front and back faces culled: no triangle is left (rectangles are never culled).
    if (d.cull == 3 && d.primitive != 0x11 && !d.depthClear) { ++culledDraws; return true; }
    // A fast clear due on this target (PM4Draw::fastClear) loads its next
    // pass cleared, even if this draw waits for its pipeline.
    if (d.colour && d.fastClear) {
        colourClearTo_[d.target] = fastClearColour(d.format, d.number, d.clearWords);
        ++fastClearsApplied;
        const auto open = targets_.find(d.target);
        if (encoder_ && open != targets_.end() && attachment_ == open->second.texture) {
            [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil;
        }
    }
    deferred_ = false;
    id<MTLRenderPipelineState> state = pipeline(d, format, depthFormat);
    if (!state && deferred_) { ++drawsDeferred; return true; } // its pipeline is still being built
    if (!state && untranslated_) { ++untranslatedDraws; return true; }
    if (!state) return false;
    DrawState drawState{};
    id<MTLBuffer> memory = guest_;
    // buffer(5) up: the shared ranges (see sharedRange), else the draw's buffer.
    id<MTLBuffer> slots[slotCount] = {};
    unsigned slotsUsed = 0;
    if (memory) {
        drawState.base = guestBase_; drawState.size = memory.length;
        if (native_) { Stopwatch watch{scanMs}; scanner_.scan(d); } // only for its T#s
    } else {
        // Copies of the ranges this draw's programs read. Each starts at a buffer
        // offset congruent to its guest address modulo 256, so guest alignment
        // (up to 256 bytes) holds in the buffer: a T# base inside a merged range
        // stays aligned for a native texture view.
        const auto& scanned = [&]() -> const auto& { Stopwatch watch{scanMs}; return scanner_.scan(d); }();
        // The draw state holds 16 ranges: more are joined across their
        // smallest gaps (CoD's scene draws read more, and the refusal stopped
        // the whole renderer). The 256 MiB check below still bounds the copy.
        const std::vector<PM4Range> ranges = scanned.size() > 16 ? coalesceRanges(scanned, 16) : scanned;
        if (scanned.size() > 16) {
            // Joined across wide gaps, a draw would copy megabytes it never
            // reads, every frame (the iPhone ends the app for less): over
            // 8 MiB more than its own ranges, the draw is left out instead.
            std::uint64_t own = 0, joined = 0;
            for (const auto& r : scanned) own += r.size;
            for (const auto& r : ranges) joined += r.size;
            if (joined > own + (8ull << 20)) { ++rangeRefusedDraws; return true; }
            ++coalescedDraws;
        }
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
            { Stopwatch watch{copyMs}; copyGuest(ranges[k].base, bytes + offset, ranges[k].size); }
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
        target = {[device_ newTextureWithDescriptor:descriptor], true, d.format};
        if (!target.texture) return fail("colour target texture");
    }
    // The other colour targets, at target 0's size (see PM4DrawCollector).
    Target* more[7] = {};
    for (unsigned k = 0; k < 7; ++k) {
        if (!d.colour || !d.more[k].base) continue;
        Target& extra = targets_[d.more[k].base];
        if (!extra.texture || extra.texture.width != d.pitch || extra.texture.height != d.height || extra.texture.pixelFormat != moreFormats[k]) {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:moreFormats[k]
                                                                                                  width:d.pitch height:d.height mipmapped:NO];
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            descriptor.storageMode = MTLStorageModePrivate;
            extra = {[device_ newTextureWithDescriptor:descriptor], true, d.more[k].format};
            if (!extra.texture) return fail("colour target texture");
        }
        more[k] = &extra;
    }
    if (more[0] || more[1] || more[2] || more[3] || more[4] || more[5] || more[6]) ++mrtDraws;
    // A draw never samples its own attachments (see nativeTexture).
    for (unsigned k = 0; k < 7; ++k) drawingMore_[k] = more[k] ? more[k]->texture : nil;
    bool sameMore = true;
    for (unsigned k = 0; k < 7; ++k) sameMore = sameMore && moreAttachments_[k] == (more[k] ? more[k]->texture : nil);
    Target* depth = nullptr;
    if (d.depthTarget && !(depth = depthTexture(d, commands))) return false;
    id<MTLTexture> depthView = depth ? depth->texture : nil;
    if (encoder_ && (commands_ != commands || attachment_ != target.texture || depthAttachment_ != depthView || !sameMore)) {
        [encoder_ endEncoding]; encoder_ = nil;
    }
    id<MTLRenderCommandEncoder> encoder = encoder_;
    if (!encoder) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (target.texture) {
        pass.colorAttachments[0].texture = target.texture;
        const auto clearTo = colourClearTo_.find(d.target);
        pass.colorAttachments[0].loadAction = target.fresh || clearTo != colourClearTo_.end() ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[0].clearColor = clearTo != colourClearTo_.end() ? clearTo->second : MTLClearColorMake(0, 0, 0, 0);
        if (clearTo != colourClearTo_.end()) colourClearTo_.erase(clearTo);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    for (unsigned k = 0; k < 7; ++k) {
        if (!more[k]) continue;
        pass.colorAttachments[k + 1].texture = more[k]->texture;
        pass.colorAttachments[k + 1].loadAction = more[k]->fresh ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[k + 1].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[k + 1].storeAction = MTLStoreActionStore;
    }
    if (depth) {
        // A new depth buffer starts at the clear value the game set.
        pass.depthAttachment.texture = depth->texture;
        const auto clearTo = depthClearTo_.find(d.depthTarget);
        pass.depthAttachment.loadAction = depth->fresh || clearTo != depthClearTo_.end() ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.clearDepth = clearTo != depthClearTo_.end() ? clearTo->second : d.depthClearValue;
        if (clearTo != depthClearTo_.end()) depthClearTo_.erase(clearTo);
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.texture = depth->texture;
        const auto stencilTo = stencilClearTo_.find(d.depthTarget);
        pass.stencilAttachment.loadAction = depth->fresh || stencilTo != stencilClearTo_.end() ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.stencilAttachment.clearStencil = stencilTo != stencilClearTo_.end() ? stencilTo->second : d.stencilClearValue;
        if (stencilTo != stencilClearTo_.end()) stencilClearTo_.erase(stencilTo);
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    if (batching_) pass.visibilityResultBuffer = visibility_;
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return fail("render encoder");
    target.fresh = false;
    for (auto* extra : more) if (extra) extra->fresh = false;
    if (depth) depth->fresh = false;
    ++renderPasses;
    if (batching_) {
        encoder_ = encoder; commands_ = commands; attachment_ = target.texture; depthAttachment_ = depthView;
        for (unsigned k = 0; k < 7; ++k) moreAttachments_[k] = more[k] ? more[k]->texture : nil;
    }
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
    // A stencil-only clear leaves depth where it is: the clear draw's depth
    // writes are off (see depthStencilState).
    [encoder setViewport:viewport];
    [encoder setScissorRect:MTLScissorRect{d.scissor[0], d.scissor[1], d.scissor[2] - d.scissor[0], d.scissor[3] - d.scissor[1]}];
    // Encoders are shared by draws: every draw sets its own depth and faces.
    unsigned stencilRef[2] = {0, 0};
    [encoder setDepthStencilState:depthStencilState(d, stencilRef)];
    [encoder setStencilFrontReferenceValue:stencilRef[0] backReferenceValue:stencilRef[1]];
    if (d.stencilTest || d.stencilClear) ++stencilDraws;
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
    [encoder drawPrimitives:d.primitive == 6 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count
              instanceCount:std::max<NSUInteger>(1, d.instances)];
    if (counted) [encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    if (!batching_) [encoder endEncoding];
    // The GPU's own result: a flipped buffer's centre pixel right after each
    // draw into it. The pass ends here (the next draw reopens it, loading what
    // is there) and a blit copies the pixel to a ring the report reads.
    if (probeFrame_ && d.colour && shown_.count(d.target) && target.texture && commands) {
        float pos[4] = {0, 0, 0, 0}, colour[4] = {-1, -1, -1, -1};
        for (const auto& e : scanner_.vertexExports()) {
            float* into = e[0] == 12 ? pos : e[0] == 32 ? colour : nullptr;
            if (into) for (unsigned k = 0; k < 4; ++k) into[k] = gcn_detail::f32(e[1 + k]);
        }
        char line[200];
        std::snprintf(line, sizeof line, "\n  - 0x%llx apres PS 0x%llx (%zu sommets, blend 0x%x, pos %.3g %.3g, param0 %.3g %.3g %.3g %.3g) :",
                      (unsigned long long)d.target, (unsigned long long)d.ps, d.indices.size(), d.blend, pos[0], pos[1],
                      colour[0], colour[1], colour[2], colour[3]);
        probe(target.texture, d.target, line, commands);
    }
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
        if (d.colour) {
            std::string line;
            std::snprintf(text, sizeof text, "\n  - dessin PS 0x%llx, %zu sommets, primitive %u, masque 0x%x, blend 0x%x, format %u/%u, profondeur %s%s",
                          (unsigned long long)d.ps, d.indices.size(), d.primitive, d.mask, d.blend, d.format, d.number,
                          d.depthTarget ? (d.depthTest ? "testée" : "effacée") : "aucune", scanner_.pixelError().empty() ? "" : ", PS interprété : ");
            line = text + (scanner_.pixelError().empty() ? std::string() : scanner_.pixelError());
            std::snprintf(text, sizeof text, ", natives %u sur %zu", nativeBound_, scanner_.images().size());
            line += text;
            // The first vertex as interpreted: position and parameters 0-1
            // (a fade quad's colour and alpha).
            float pos[4] = {0, 0, 0, 0}, colour[4] = {-1, -1, -1, -1};
            for (const auto& e : scanner_.vertexExports()) {
                if (e[0] != 12 && e[0] != 32 && e[0] != 33) continue;
                std::snprintf(text, sizeof text, ", %s (%g %g %g %g)", e[0] == 12 ? "pos" : e[0] == 32 ? "param0" : "param1",
                              gcn_detail::f32(e[1]), gcn_detail::f32(e[2]), gcn_detail::f32(e[3]), gcn_detail::f32(e[4]));
                line += text;
                float* into = e[0] == 12 ? pos : e[0] == 32 ? colour : nullptr;
                if (into) for (unsigned k = 0; k < 4; ++k) into[k] = gcn_detail::f32(e[1 + k]);
            }
            // Full-screen quads over time: a fade's colour and alpha, to see
            // whether it moves on or stays put.
            if (d.indices.size() == 6 && pos[0] <= -0.99f && pos[1] <= -0.99f) {
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
                auto& last = fadeLast_[d.ps];
                bool moved = now - last.first > 10;
                for (unsigned k = 0; k < 4; ++k) moved |= std::fabs(colour[k] - last.second[k]) > 0.02f;
                if (moved) {
                    last.first = now;
                    for (unsigned k = 0; k < 4; ++k) last.second[k] = colour[k];
                    std::snprintf(text, sizeof text, "\n  - %.1f s : PS 0x%llx dans 0x%llx, param0 (%.3g %.3g %.3g %.3g), blend 0x%x", now,
                                  (unsigned long long)d.ps, (unsigned long long)d.target, colour[0], colour[1], colour[2], colour[3], d.blend);
                    fades.push_back(text);
                    if (fades.size() > 24) fades.pop_front();
                }
            }
            unsigned shown = 0;
            for (const auto& t : scanner_.images()) {
                if (shown++ == 4) break;
                const std::uint64_t base = (std::uint64_t(t[0]) | std::uint64_t(t[1] & 0xff) << 32) << 8;
                const unsigned dfmt = (t[1] >> 20) & 63, tiling = (t[3] >> 20) & 31;
                const unsigned w = (t[2] & 0x3fff) + 1, h = ((t[2] >> 14) & 0x3fff) + 1, pitch = ((t[4] >> 13) & 0x3fff) + 1;
                std::snprintf(text, sizeof text, ", texture dfmt %u nfmt %u %ux%u tuilage %u 0x%llx%s", dfmt, (t[1] >> 26) & 15,
                              w, h, tiling, (unsigned long long)base, targets_.count(base) ? " (rendue GPU)" : "");
                line += text;
                // Its memory at the centre, and the mean of 256 samples, for linear 8 and 8_8_8_8
                // textures the GPU did not render (a video plane, a fade's white texel).
                const unsigned bpp = dfmt == 1 ? 1 : dfmt == 10 ? 4 : 0;
                if (bpp && (tiling == 8 || (w == 1 && h == 1)) && !targets_.count(base)) {
                    std::uint8_t px[4] = {};
                    const std::uint64_t centre = base + (std::uint64_t(h / 2) * pitch + w / 2) * bpp;
                    if (code_.read(centre, px, bpp)) {
                        unsigned sum = 0, read = 0;
                        for (unsigned k = 0; k < 256; ++k) {
                            std::uint8_t b = 0;
                            const std::uint64_t at = base + (std::uint64_t(h) * k / 256 * pitch + std::uint64_t(w) * (k * 37 % 256) / 256) * bpp;
                            if (code_.read(at, &b, 1)) { sum += b; ++read; }
                        }
                        std::snprintf(text, sizeof text, " (memoire : centre %u %u %u %u, moyenne %u)", px[0], px[1], px[2], px[3],
                                      read ? sum / read : 0);
                        line += text;
                    } else line += " (memoire illisible)";
                }
            }
            noteInto(d.target, std::move(line));
        }
        listProgram("VS", d.vs, d.vsUser, d.vsUserCount);
        if (d.vsUserCount >= 2) listProgram("Fetch", std::uint64_t(d.vsUser[0]) | std::uint64_t(d.vsUser[1]) << 32, d.vsUser, 0);
        listProgram("PS", d.ps, d.psUser, d.psUserCount, listed_.size() < 20); // every pixel program of the batch (menus)
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
                // A texture the CPU writes (Mortal Kombat Trilogy's PS1 frame):
                // whether its memory holds anything but black, from 1024
                // pixels spread over it (tiling only reorders them).
                if (!targets_.count(base) && ((t[1] >> 20) & 63) == 10) {
                    const std::uint64_t pixels = std::uint64_t((t[2] & 0x3fff) + 1) * (((t[2] >> 14) & 0x3fff) + 1);
                    unsigned lit = 0, read = 0;
                    for (unsigned k = 0; k < 1024; ++k) {
                        std::uint8_t px[4];
                        if (!code_.read(base + 4 * (pixels * k / 1024), px, 4)) continue;
                        ++read;
                        lit += (px[0] | px[1] | px[2]) != 0;
                    }
                    std::snprintf(text, sizeof text, " (memoire : %u/%u pixels non noirs)", lit, read);
                    batchDraws += text;
                }
            }
        }
    }
    return true;
}

id<MTLTexture> PM4MetalRenderer::target(std::uint64_t address) const {
    const auto found = targets_.find(address);
    return found == targets_.end() ? nil : found->second.texture;
}

// CB_COLOR0_CLEAR_WORD0/1 hold one pixel in the target's format: its value
// as Metal's clear colour (formats without a decoding clear to zero).
MTLClearColor PM4MetalRenderer::fastClearColour(unsigned format, unsigned number, const std::uint32_t words[2]) {
    auto half = [](std::uint32_t h) -> double {
        const int e = int((h >> 10) & 31); const double m = double(h & 1023), s = (h & 0x8000) ? -1 : 1;
        if (e == 0) return s * m * std::ldexp(1.0, -24);
        if (e == 31) return 0; // inf or NaN: a clear never wants either
        return s * (1 + m / 1024) * std::ldexp(1.0, e - 15);
    };
    auto small = [](std::uint32_t bits, int mantissa) -> double { // unsigned 11- or 10-bit float
        const int e = int(bits >> mantissa); const double m = double(bits & ((1u << mantissa) - 1));
        if (e == 0) return m * std::ldexp(1.0, -14 - mantissa);
        if (e == 31) return 0;
        return (1 + m / double(1u << mantissa)) * std::ldexp(1.0, e - 15);
    };
    auto unorm = [](std::uint32_t v, unsigned bits) { return double(v) / double((1u << bits) - 1); };
    auto snorm8 = [](std::uint32_t v) { return std::max(-1.0, double(std::int8_t(v)) / 127); };
    const std::uint32_t w0 = words[0], w1 = words[1];
    switch (format) {
    case 0xa:
        if (number == 1) return MTLClearColorMake(snorm8(w0), snorm8(w0 >> 8), snorm8(w0 >> 16), snorm8(w0 >> 24));
        return MTLClearColorMake(unorm(w0 & 255, 8), unorm((w0 >> 8) & 255, 8), unorm((w0 >> 16) & 255, 8), unorm(w0 >> 24, 8));
    case 0xc:
        if (number == 7) return MTLClearColorMake(half(w0 & 0xffff), half(w0 >> 16), half(w1 & 0xffff), half(w1 >> 16));
        return MTLClearColorMake(unorm(w0 & 0xffff, 16), unorm(w0 >> 16, 16), unorm(w1 & 0xffff, 16), unorm(w1 >> 16, 16));
    case 0x5:
        if (number == 7) return MTLClearColorMake(half(w0 & 0xffff), half(w0 >> 16), 0, 1);
        return MTLClearColorMake(unorm(w0 & 0xffff, 16), unorm(w0 >> 16, 16), 0, 1);
    case 0x2: return MTLClearColorMake(number == 7 ? half(w0 & 0xffff) : unorm(w0 & 0xffff, 16), 0, 0, 1);
    case 0x4: { float v; std::memcpy(&v, &w0, 4); return MTLClearColorMake(std::isfinite(v) ? v : 0, 0, 0, 1); }
    case 0x6: return MTLClearColorMake(small(w0 & 0x7ff, 6), small((w0 >> 11) & 0x7ff, 6), small(w0 >> 22, 5), 1);
    case 0x9: return MTLClearColorMake(unorm(w0 & 1023, 10), unorm((w0 >> 10) & 1023, 10), unorm((w0 >> 20) & 1023, 10), unorm(w0 >> 30, 2));
    case 0x3: return MTLClearColorMake(unorm(w0 & 255, 8), unorm((w0 >> 8) & 255, 8), 0, 1);
    case 0x1: return MTLClearColorMake(unorm(w0 & 255, 8), 0, 0, 1);
    default: return MTLClearColorMake(0, 0, 0, 0);
    }
}

MTLPixelFormat PM4MetalRenderer::targetFormat(unsigned format, unsigned number, unsigned swap) {
    if (format == 0xa && swap == 1) // 8_8_8_8 with COMP_SWAP ALT: BGRA in memory.
        return number == 0 ? MTLPixelFormatBGRA8Unorm : number == 6 ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatInvalid;
    if (swap != 0) return MTLPixelFormatInvalid;
    switch (format) {
    case 0xa: return number == 0 ? MTLPixelFormatRGBA8Unorm : number == 1 ? MTLPixelFormatRGBA8Snorm :
                     number == 6 ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatInvalid;
    case 0xc: return number == 7 ? MTLPixelFormatRGBA16Float : number == 0 ? MTLPixelFormatRGBA16Unorm :
                     number == 1 ? MTLPixelFormatRGBA16Snorm : MTLPixelFormatInvalid;
    case 0x5: return number == 7 ? MTLPixelFormatRG16Float : number == 0 ? MTLPixelFormatRG16Unorm :
                     number == 1 ? MTLPixelFormatRG16Snorm : MTLPixelFormatInvalid;
    case 0x2: return number == 7 ? MTLPixelFormatR16Float : number == 0 ? MTLPixelFormatR16Unorm :
                     number == 1 ? MTLPixelFormatR16Snorm : MTLPixelFormatInvalid;
    case 0x4: return number == 7 ? MTLPixelFormatR32Float : MTLPixelFormatInvalid;
    case 0xb: return number == 7 ? MTLPixelFormatRG32Float : MTLPixelFormatInvalid;
    case 0xe: return number == 7 ? MTLPixelFormatRGBA32Float : MTLPixelFormatInvalid;
    case 0x9: return number == 0 ? MTLPixelFormatRGB10A2Unorm : MTLPixelFormatInvalid;  // R in the low 10 bits
    case 0x6: return number == 7 ? MTLPixelFormatRG11B10Float : MTLPixelFormatInvalid;  // R in the low 11 bits
    case 0x3: return number == 0 ? MTLPixelFormatRG8Unorm : number == 1 ? MTLPixelFormatRG8Snorm : MTLPixelFormatInvalid;
    case 0x1: return number == 0 ? MTLPixelFormatR8Unorm : number == 1 ? MTLPixelFormatR8Snorm : MTLPixelFormatInvalid;
    default: return MTLPixelFormatInvalid;
    }
}

unsigned PM4MetalRenderer::bytesPerPixel(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: return 1;
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatR16Float:
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: return 2;
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRG32Float: return 8;
    case MTLPixelFormatRGBA32Float: return 16;
    case MTLPixelFormatInvalid: return 0;
    default: return 4; // 8_8_8_8 in any order or number, RG16, R32, RGB10A2, RG11B10.
    }
}

void PM4MetalRenderer::noteInto(std::uint64_t address, std::string line) {
    if (!into_.count(address) && into_.size() >= 32) into_.erase(into_.begin());
    auto& runs = into_[address];
    // A run is writes whose line starts alike up to the first comma (same pixel
    // program, same fill value): text drawn glyph by glyph then leaves room for
    // what drew the background before it.
    const auto key = [](const std::string& s) { return s.substr(0, s.find(',')); };
    if (!runs.empty() && key(runs.back().first) == key(line)) { ++runs.back().second; return; }
    runs.emplace_back(std::move(line), 1u);
    if (runs.size() > 10) runs.pop_front();
}

void PM4MetalRenderer::probe(id<MTLTexture> texture, std::uint64_t address, std::string line, id<MTLCommandBuffer> commands) {
    if (!probeRing_) {
        probeRing_ = [device_ newBufferWithLength:16 * probeSlots options:MTLResourceStorageModeShared];
        if (!probeRing_) return;
        std::memset(probeRing_.contents, 0xee, 16 * probeSlots);
    }
    if (encoder_) { [encoder_ endEncoding]; encoder_ = nil; commands_ = nil; attachment_ = nil; depthAttachment_ = nil; }
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!blit) return;
    const unsigned slot = unsigned(probeNext_++ % probeSlots);
    std::memset(static_cast<std::uint8_t*>(probeRing_.contents) + 16 * slot, 0xee, 16);
    const std::size_t bpp = bytesPerPixel(texture.pixelFormat);
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(texture.width / 2, texture.height / 2, 0)
               sourceSize:MTLSizeMake(1, 1, 1) toBuffer:probeRing_ destinationOffset:16 * slot
   destinationBytesPerRow:bpp ? bpp : 4 destinationBytesPerImage:bpp ? bpp : 4];
    [blit endEncoding];
    probes_.push_back({slot, address, std::move(line)});
    if (probes_.size() > probeSlots - 8) probes_.pop_front();
}

std::string PM4MetalRenderer::gpuProbes() const {
    // Only the readings where a buffer's pixel changed (and its first one):
    // what turned it from one colour to another.
    std::deque<std::string> changes; // the last 60
    std::map<std::uint64_t, std::uint32_t> last;
    for (const auto& p : probes_) {
        const auto* b = static_cast<const std::uint8_t*>(probeRing_.contents) + 16 * p.slot;
        std::uint32_t value; std::memcpy(&value, b, 4);
        if (value == 0xeeeeeeeeu) continue; // not run yet
        const auto found = last.find(p.address);
        if (found != last.end() && found->second == value) continue;
        last[p.address] = value;
        char text[40];
        std::snprintf(text, sizeof text, " %u %u %u %u", b[0], b[1], b[2], b[3]);
        changes.push_back(p.line + text);
        if (changes.size() > 60) changes.pop_front();
    }
    std::string out;
    for (const auto& change : changes) out += change;
    std::snprintf(text_, sizeof text_, "\n  (%zu lectures, les 60 derniers changements au plus)", probes_.size());
    return out + text_;
}

std::string PM4MetalRenderer::drawsInto(std::uint64_t address) const {
    const auto found = into_.find(address);
    if (found == into_.end()) return " aucun relevé";
    std::string out;
    for (const auto& [line, count] : found->second) {
        out += line;
        if (count > 1) out += " (x" + std::to_string(count) + ")";
    }
    return out;
}

id<MTLTexture> PM4MetalRenderer::depthTarget(std::uint64_t address) const {
    const auto found = depthTargets_.find(address);
    return found == depthTargets_.end() ? nil : found->second.texture;
}

std::string PM4MetalRenderer::depthReport() const {
    char text[320];
    std::snprintf(text, sizeof text, "\nProfondeur Metal : %u effacements sans programme · %u dessins avec profondeur, dont %u sans couleur · %u dessins entièrement cullés · %zu tampons, %u redimensionnés · stencil : %u dessins, %u états approchés · MRT : %u dessins",
                  embeddedDepthDraws, depthDraws, depthOnlyDraws, culledDraws, depthTargets_.size(), depthResized, stencilDraws, stencilApproximated, mrtDraws);
    return text;
}

// ZFUNC/STENCILFUNC codes in order: never, less, equal, less or equal, greater, not equal, greater or equal, always.
static const MTLCompareFunction pm4CompareFunctions[8] = {
    MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
    MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};

id<MTLDepthStencilState> PM4MetalRenderer::depthState(unsigned function, bool write) {
    PM4Draw d;
    d.depthTest = true; d.depthFunc = function & 7; d.depthWrite = write;
    unsigned ref[2];
    return depthStencilState(d, ref);
}

// GCN stencil operation (DB_STENCIL_CONTROL): KEEP, ZERO, ONES, REPLACE_TEST,
// REPLACE_OP, ADD_CLAMP, SUB_CLAMP, INVERT, ADD_WRAP, SUB_WRAP, AND, OR, XOR,
// NAND, NOR, XNOR. Metal replaces with the reference and steps by one;
// `exact` turns false where the result can differ.
static MTLStencilOperation pm4StencilOperation(unsigned op, const PM4Draw::StencilFace& face, bool& exact) {
    switch (op) {
    case 0: return MTLStencilOperationKeep;
    case 1: return MTLStencilOperationZero;
    case 2: exact = exact && face.ref == 0xff; return MTLStencilOperationReplace;
    case 3: return MTLStencilOperationReplace;
    case 4: exact = exact && face.opValue == face.ref; return MTLStencilOperationReplace;
    case 5: exact = exact && face.opValue == 1; return MTLStencilOperationIncrementClamp;
    case 6: exact = exact && face.opValue == 1; return MTLStencilOperationDecrementClamp;
    case 7: return MTLStencilOperationInvert;
    case 8: exact = exact && face.opValue == 1; return MTLStencilOperationIncrementWrap;
    case 9: exact = exact && face.opValue == 1; return MTLStencilOperationDecrementWrap;
    default: exact = false; return MTLStencilOperationKeep;
    }
}

id<MTLDepthStencilState> PM4MetalRenderer::depthStencilState(const PM4Draw& d, unsigned ref[2]) {
    // A depth clear passes everywhere and writes; a stencil-only clear keeps depth.
    const bool depthTest = d.depthClear || d.depthTest;
    const unsigned function = d.depthClear ? 7 : d.depthTest ? d.depthFunc : 7;
    const bool write = d.depthClear || (d.depthTest && d.depthWrite);
    // A stencil clear replaces the stencil with DB_STENCIL_CLEAR wherever the draw covers.
    PM4Draw::StencilFace faces[2] = {d.stencil[0], d.stencil[1]};
    const bool stencil = d.stencilTest || d.stencilClear;
    if (d.stencilClear) for (auto& face : faces) face = {7, 3, 3, 3, d.stencilClearValue, 0xff, 0xff, d.stencilClearValue};
    ref[0] = stencil ? faces[0].ref : 0; ref[1] = stencil ? faces[1].ref : 0;
    // The key packs both faces; an ONES operation also changes the reference.
    std::array<std::uint32_t, 5> key{(function & 7) | (write ? 8u : 0u) | (depthTest ? 16u : 0u) | (stencil ? 32u : 0u), 0, 0, 0, 0};
    if (stencil) for (unsigned k = 0; k < 2; ++k) {
        const auto& f = faces[k];
        key[1 + 2 * k] = f.func | f.fail << 4 | f.zpass << 8 | f.zfail << 12 | f.opValue << 16;
        key[2 + 2 * k] = f.mask | f.writeMask << 8 | f.ref << 16;
    }
    const auto found = depthStates_.find(key);
    if (found != depthStates_.end()) return found->second;
    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction = depthTest ? pm4CompareFunctions[function & 7] : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = write;
    if (stencil) {
        bool exact = true;
        MTLStencilDescriptor* metal[2] = {[MTLStencilDescriptor new], [MTLStencilDescriptor new]};
        for (unsigned k = 0; k < 2; ++k) {
            const auto& f = faces[k];
            metal[k].stencilCompareFunction = pm4CompareFunctions[f.func & 7];
            metal[k].stencilFailureOperation = pm4StencilOperation(f.fail, f, exact);
            metal[k].depthStencilPassOperation = pm4StencilOperation(f.zpass, f, exact);
            metal[k].depthFailureOperation = pm4StencilOperation(f.zfail, f, exact);
            metal[k].readMask = f.mask;
            metal[k].writeMask = f.writeMask;
        }
        if (!exact) ++stencilApproximated;
        descriptor.frontFaceStencil = metal[0];
        descriptor.backFaceStencil = metal[1];
    }
    id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:descriptor];
    depthStates_[key] = state;
    return state;
}

PM4MetalRenderer::Target* PM4MetalRenderer::depthTexture(const PM4Draw& d, id<MTLCommandBuffer> commands) {
    Target& depth = depthTargets_[d.depthTarget];
    if (depth.texture && depth.texture.width == d.pitch && depth.texture.height == d.height) return &depth;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
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
            pass.stencilAttachment.texture = texture;
            pass.stencilAttachment.loadAction = MTLLoadActionClear;
            pass.stencilAttachment.clearStencil = d.stencilClearValue;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
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


// Archives from earlier launches are only read (lookups never race with
// additions); this launch's compiles go to a new archive of its own, touched
// on archiveQueue only and written to its own file. The newest 8 files are
// kept. A marker counts launches that did not reach their first write: after
// two in a row (a crash in the archive code?), the archive is left off.
void PM4MetalRenderer::AsyncPipelines::openArchive(id<MTLDevice> device) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (archiveOpened) return;
        archiveOpened = true;
    }
    NSFileManager* files = [NSFileManager defaultManager];
    NSURL* folder = archiveFolder();
    std::string state;
    NSMutableArray<id<MTLBinaryArchive>>* found = [NSMutableArray array];
    id<MTLBinaryArchive> fresh = nil;
    NSURL* freshURL = nil;
    @try {
        if (!folder) { std::lock_guard<std::mutex> lock(mutex); archiveState = "pas de dossier Caches"; return; }
        [files createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
        // Old single-file archive of the first version: dropped.
        [files removeItemAtURL:[[folder URLByDeletingLastPathComponent] URLByAppendingPathComponent:@"metal-pipelines.bin"] error:nil];
        NSURL* marker = [folder URLByAppendingPathComponent:@"unfinished"];
        const int unfinished = [[NSString stringWithContentsOfURL:marker encoding:NSUTF8StringEncoding error:nil] intValue];
        if (unfinished >= 2) {
            for (NSURL* url in [files contentsOfDirectoryAtURL:folder includingPropertiesForKeys:nil options:0 error:nil]) [files removeItemAtURL:url error:nil];
            std::lock_guard<std::mutex> lock(mutex);
            archiveState = "coupee (deux lancements arretes avant leur premiere ecriture), effacee";
            return;
        }
        [[NSString stringWithFormat:@"%d", unfinished + 1] writeToURL:marker atomically:YES encoding:NSUTF8StringEncoding error:nil];
        NSArray<NSURL*>* listed = [files contentsOfDirectoryAtURL:folder includingPropertiesForKeys:@[NSURLContentModificationDateKey, NSURLFileSizeKey]
                                                          options:0 error:nil];
        NSMutableArray<NSURL*>* archives = [NSMutableArray array];
        for (NSURL* url in listed) if ([url.pathExtension isEqualToString:@"bin"]) [archives addObject:url];
        [archives sortUsingComparator:^NSComparisonResult(NSURL* x, NSURL* y) {
            NSDate *dx = nil, *dy = nil;
            [x getResourceValue:&dx forKey:NSURLContentModificationDateKey error:nil];
            [y getResourceValue:&dy forKey:NSURLContentModificationDateKey error:nil];
            return [dy compare:dx ?: [NSDate distantPast]]; // newest first
        }];
        unsigned long long total = 0;
        for (NSUInteger k = 0; k < archives.count; ++k) {
            NSNumber* size = nil;
            [archives[k] getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
            total += size.unsignedLongLongValue;
            if (k >= 8 || total > (1ull << 30)) { [files removeItemAtURL:archives[k] error:nil]; continue; }
            MTLBinaryArchiveDescriptor* descriptor = [MTLBinaryArchiveDescriptor new];
            descriptor.url = archives[k];
            NSError* error = nil;
            id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
            if (archive) [found addObject:archive]; else [files removeItemAtURL:archives[k] error:nil];
        }
        char text[96];
        std::snprintf(text, sizeof text, "%lu fichiers relus (%.1f Mo)", (unsigned long)found.count, double(total) / (1 << 20));
        state = text;
        NSError* error = nil;
        fresh = [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
        freshURL = [folder URLByAppendingPathComponent:[NSString stringWithFormat:@"%lld.bin", (long long)[NSDate date].timeIntervalSince1970]];
    } @catch (NSException* exception) {
        state = std::string("exception a l'ouverture : ") + exception.reason.UTF8String;
        [found removeAllObjects];
        fresh = nil;
    }
    std::lock_guard<std::mutex> lock(mutex);
    lookups = found.count ? [found copy] : nil;
    building = fresh;
    buildingURL = freshURL;
    if (fresh) archiveQueue = dispatch_queue_create("maxps4.metal-archive", DISPATCH_QUEUE_SERIAL);
    archiveSavedAt = std::chrono::steady_clock::now();
    archiveState = state;
}

void PM4MetalRenderer::AsyncPipelines::compile(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor,
                                               std::function<void(id<MTLRenderPipelineState>, NSError*)> done) {
    auto self = shared_from_this();
    // Through the compile limit, ahead of libraries waiting (see compileLimit).
    startCompile([self, device, descriptor, done] {
        { std::lock_guard<std::mutex> lock(self->mutex); ++self->pipelinesRunning; }
        [device newRenderPipelineStateWithDescriptor:descriptor completionHandler:^(id<MTLRenderPipelineState> state, NSError* failed) {
            { std::lock_guard<std::mutex> lock(self->mutex); --self->pipelinesRunning; }
            self->compileDone();
            if (state) self->remember(descriptor);
            done(state, failed);
        }];
    }, true);
}

void PM4MetalRenderer::AsyncPipelines::remember(MTLRenderPipelineDescriptor* descriptor) {
    { std::lock_guard<std::mutex> lock(mutex); if (!building || !archiveQueue) return; }
    MTLRenderPipelineDescriptor* plain = [descriptor copy];
    plain.binaryArchives = nil;
    // The renderer may go before the queue drains: the block keeps this alive.
    auto self = shared_from_this();
    dispatch_async(archiveQueue, ^{
        NSError* error = nil;
        bool added = false;
        std::string failure;
        @try { added = [self->building addRenderPipelineFunctionsWithDescriptor:plain error:&error]; }
        @catch (NSException* exception) { failure = std::string("exception : ") + exception.reason.UTF8String; }
        bool save = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (added) { ++self->archiveAdded; ++self->archiveUnsaved; }
            else self->archiveError = !failure.empty() ? failure : error ? error.localizedDescription.UTF8String : "ajout refuse";
            save = self->archiveUnsaved >= 16 ||
                (self->archiveUnsaved && std::chrono::steady_clock::now() - self->archiveSavedAt > std::chrono::seconds(30));
        }
        if (save) self->saveArchive();
    });
}

void PM4MetalRenderer::AsyncPipelines::saveArchive() {
    NSFileManager* files = [NSFileManager defaultManager];
    NSURL* folder = archiveFolder();
    if (!building || !buildingURL || !folder) return;
    NSURL* partial = [folder URLByAppendingPathComponent:@"partial.tmp"];
    NSError* error = nil;
    bool written = false;
    std::string failure;
    @try {
        [files removeItemAtURL:partial error:nil];
        // Written beside it, then swapped in: a launch cut short never leaves half a file.
        written = [building serializeToURL:partial error:&error] &&
            ([files fileExistsAtPath:buildingURL.path]
                 ? [files replaceItemAtURL:buildingURL withItemAtURL:partial backupItemName:nil options:0 resultingItemURL:nil error:&error]
                 : [files moveItemAtURL:partial toURL:buildingURL error:&error]);
        if (written) [files removeItemAtURL:[folder URLByAppendingPathComponent:@"unfinished"] error:nil];
    } @catch (NSException* exception) { failure = std::string("exception : ") + exception.reason.UTF8String; }
    NSNumber* size = nil;
    if (written) [buildingURL getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    std::lock_guard<std::mutex> lock(mutex);
    archiveSavedAt = std::chrono::steady_clock::now();
    if (written) { archiveUnsaved = 0; ++archiveSaves; archiveBytes = size ? size.unsignedLongLongValue : 0; }
    else archiveError = !failure.empty() ? failure : error ? error.localizedDescription.UTF8String : "ecriture refusee";
}

std::string PM4MetalRenderer::AsyncPipelines::archiveReport() const {
    if (!archiveOpened) return std::string();
    char text[256];
    std::snprintf(text, sizeof text, " · archive Metal %s : %u trouvés, %u compilés, %u ajoutés, %u écritures (%.1f Mo)%s%s",
                  archiveState.c_str(), archiveHits, archiveMisses, archiveAdded, archiveSaves, double(archiveBytes) / (1 << 20),
                  archiveError.empty() ? "" : ", erreur : ", archiveError.c_str());
    return text;
}

} // namespace maxps4::gpu
