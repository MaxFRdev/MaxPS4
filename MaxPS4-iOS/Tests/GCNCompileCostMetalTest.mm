// Measurement, the same at every commit: what Metal's compiler spends on
// translated programs (library from MSL, then render pipeline), the cost
// that kept CoD's scene from the iPhone's screen (up to 70 s a pipeline).
// Each case compiles cold `runs` times (its entry points renamed every run,
// so neither the MSL nor the pipeline cache answers) and prints medians in
// one table: blocks in order against the dispatcher's loop, size against
// default optimisation, a depth-only pipeline (vertex program alone)
// against one with its pixel program, the source as built against it
// pruned (gcnPruneMetal). Timings only, on the CI's GPU: they
// compare forms, not the iPhone's speed. Never fails on a timing.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include "GCNGamePrograms.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
std::string renamed(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}
double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP compile cost: no Metal device"); return 0; }
        constexpr unsigned runs = 3;
        Arena arena;
        loadFetchVS(arena);
        Arena straightArena;
        loadStraightFetchVS(straightArena);
        constexpr uint64_t ps1800At = 0x14000, ps2000At = 0x15000;
        for (std::size_t n = 0; n < std::size(mk1PS1800); ++n) arena.put(ps1800At + 4 * n, mk1PS1800[n]);
        for (std::size_t n = 0; n < std::size(mk1PS2000); ++n) arena.put(ps2000At + 4 * n, mk1PS2000[n]);
        GCNTranslateOptions vsOptions; vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = 4;
        vsOptions.userData[0] = std::uint32_t(fetchShader); vsOptions.userData[2] = std::uint32_t(vertexTable);
        GCNTranslateOptions vsLoopOptions = vsOptions; vsLoopOptions.dispatcher = true;
        GCNTranslateOptions psOptions; psOptions.name = "ps_program"; psOptions.userCount = 15;
        GCNTranslateOptions psLoopOptions = psOptions; psLoopOptions.dispatcher = true;
        const auto vs = translateGCN(straightArena.memory, fetchVS, vsOptions);
        const auto vsLoop = translateGCN(straightArena.memory, fetchVS, vsLoopOptions);
        const auto ps1800 = translateGCN(arena.memory, ps1800At, psOptions), loop1800 = translateGCN(arena.memory, ps1800At, psLoopOptions);
        const auto ps2000 = translateGCN(arena.memory, ps2000At, psOptions), loop2000 = translateGCN(arena.memory, ps2000At, psLoopOptions);
        if (!vs || !vsLoop || !ps1800 || !loop1800 || !ps2000 || !loop2000) { std::puts("FAIL compile cost: translation"); return 1; }
        assert(vs.source.find("while (running)") == std::string::npos && vsLoop.source.find("while (running)") != std::string::npos);
        GCNTranslation noPS;
        noPS.source = "GCN_FN void ps_program(GCN_THREAD GCNLane& L, GCNMemory M, GCN_THREAD GCNQuad& Q) { (void)L; (void)M; (void)Q; }\n";
        const std::uint32_t inputCntl[32] = {};
        auto program = [&](const GCNTranslation& v, const GCNTranslation& p) {
            return gcnMetalPrelude(false, gcnHighestUsed(v.params), gcnHighestUsed(p.attributes), gcnHighestUsed(p.colors)) + v.source + p.source +
                   gcnMetalEntryPoints(v, p, "vs_program", "ps_program", 0x302, inputCntl);
        };
        struct Case { const char* name; std::string source; bool fragment; bool size; };
        const std::vector<Case> cases = {
            {"PS 1800 in order, size", program(vs, ps1800), true, true},
            {"PS 1800 loop, size", program(vs, loop1800), true, true},
            {"PS 1800 in order, default", program(vs, ps1800), true, false},
            {"PS 1800 loop, default", program(vs, loop1800), true, false},
            {"PS 2000 in order, size", program(vs, ps2000), true, true},
            {"PS 2000 loop, size", program(vs, loop2000), true, true},
            {"fetch VS in order, depth only", program(vs, noPS), false, true},
            {"fetch VS loop, depth only", program(vsLoop, noPS), false, true},
            {"fetch VS + PS 1800, depth only", program(vs, ps1800), false, true},
            // What the renderer compiles since gcnPruneMetal: the functions reached.
            {"PS 1800 in order, size, pruned", gcnPruneMetal(program(vs, ps1800)), true, true},
            {"PS 1800 in order, default, pruned", gcnPruneMetal(program(vs, ps1800)), true, false},
            {"fetch VS in order, depth, pruned", gcnPruneMetal(program(vs, noPS)), false, true},
        };
        MTLCompileOptions* defaults = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) defaults.mathMode = MTLMathModeSafe;
        MTLCompileOptions* size = [defaults copy];
        if (@available(macOS 13.0, iOS 16.0, *)) size.optimizationLevel = MTLLibraryOptimizationLevelSize;
        std::printf("COMPILE COST on %s, cold compiles, median of %u (ms):\n", device.name.UTF8String, runs);
        std::printf("COMPILE %-34s %10s %10s %10s %9s\n", "case", "library", "pipeline", "total", "MSL KiB");
        static unsigned nonce = 0;
        for (const auto& c : cases) {
            std::vector<double> library, pipeline, total;
            for (unsigned r = 0; r < runs; ++r) {
                // New entry point names: a new function to the caches.
                const std::string tag = "_" + std::to_string(++nonce);
                const std::string vertexName = "gcn_vertex" + tag, fragmentName = "gcn_fragment" + tag;
                std::string source = renamed(renamed(c.source, "gcn_vertex", vertexName), "gcn_fragment", fragmentName);
                NSError* error = nil;
                NSDate* started = [NSDate date];
                id<MTLLibrary> built = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:c.size ? size : defaults error:&error];
                const double libraryMs = -[started timeIntervalSinceNow] * 1000;
                if (!built) { std::printf("FAIL compile cost %s: %s\n", c.name, error.localizedDescription.UTF8String); return 1; }
                MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
                descriptor.vertexFunction = [built newFunctionWithName:[NSString stringWithUTF8String:vertexName.c_str()]];
                if (c.fragment) descriptor.fragmentFunction = [built newFunctionWithName:[NSString stringWithUTF8String:fragmentName.c_str()]];
                descriptor.colorAttachments[0].pixelFormat = c.fragment ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
                descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
                NSDate* piped = [NSDate date];
                id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
                const double pipelineMs = -[piped timeIntervalSinceNow] * 1000;
                if (!state) { std::printf("FAIL compile cost %s pipeline: %s\n", c.name, error.localizedDescription.UTF8String); return 1; }
                library.push_back(libraryMs); pipeline.push_back(pipelineMs); total.push_back(libraryMs + pipelineMs);
            }
            std::printf("COMPILE %-34s %10.1f %10.1f %10.1f %9.1f\n", c.name, median(library), median(pipeline), median(total), double(c.source.size()) / 1024);
        }
        std::puts("PASS compile cost measured (timings only, CI GPU, not the iPhone)");
    }
    return 0;
}
