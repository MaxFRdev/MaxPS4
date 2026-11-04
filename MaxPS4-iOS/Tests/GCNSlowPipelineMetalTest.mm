// Research only: how long Metal takes to build the pixel program that never
// finished on the iPhone (Mortal Kombat: Legacy Kollection, after choosing a
// game; over 52 s, its draws deferred and the screen black), with its VS and
// fetch shader as the session report listed them (it stopped at a zero word:
// the last three words of the PS are the usual null export). Prints timings; a build
// that outlasts its limit is reported and left running. Included by
// GCNShaderCostMetalTest, which CI builds.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unistd.h>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
constexpr std::uint64_t vsAt = 0x11000, fetchAt = 0x11400, psAt = 0x12000;
constexpr const char* vsWords = "beeb03ff 00000014 be802100 c3000500 bf8c007f 10040800 10060803 10000802 10020801 3e040a04 3e060a07 3e000a06 3e020a05 3e040c08 3e060c0b 3e000c0a 3e020c09 3e040e0c 3e060e0f 3e000e0e 3e020e0d f80008cf 03000102 7e080280 f800021f 0f0e0d0c f800020f 04040908 bf810000";
constexpr const char* fetchWords = "4a000008 4a060609 c0860300 c0880304 c08a0308 bf8c007f e00c2000 80030400 e0042000 80040800 e00c2000 80050c00 bf8c0000 be802000";
constexpr const char* psWords = "beeb03ff 000000ab befc0310 beea03ff 3dcccccd be90047e befe0a7e c8080000 c80c0100 c8090001 c80d0101 f0800800 00401302 bf8c0f70 7c08266a 8a906a10 bf84013a befe0a10 7e080281 7e0202bf 7e0a0302 7e0c0303 f0dc0700 00400d01 f0dc0700 00400a04 7e0802ff 00000100 f0dc0700 00400704 7e0802ff 00003f00 f0dc0700 00401004 f09c0700 00400402 bf8c0f74 402c1b0e 3ffb4a7e bf8c0f73 4000150b 3ffb4a7e bf8c0f72 40020f08 3ffb4a7e bf8c0f71 402e2111 3ffb4a7e bf8c0f70 40280905 3ffb4a7e d2a80015 045a2f14 d2a80018 04022b01 d2a20015 045a2f14 d2a20015 04022b01 082a2b18 103030ff 3e000000 203030ff 3d2aaaab 7c1c3115 be92246a bf880101 061a1b10 0614150d 061a1d11 7e1c0302 0616170d 7e1a02ff 00003f01 06360f0a 060e1f12 7e1e0303 060e1907 063c1307 060e0116 0634110b f0dc0700 00400a0d 7e1a02ff 00003f3f f0dc0700 0040100d 7e1a02ff 00000101 06100317 d282001c 84520ef0 d2820018 845210f0 f0dc0700 0040070d 7e1a02ff 0000013f f0dc0700 00400d0d c2470d00 06322d17 beea03ff 3e800000 06320119 06320319 d2820019 8452326a beea03ff be800000 7e2a5515 d2820115 01aa2b19 202a2a80 10322aff 3faaaaab 1e3232ff 3f400000 be940380 7e2a0280 be8c047e bf8c0f73 403e150b 3ffb4a7e 103a3eff 3e800000 bf8c0f72 0624251e 06241912 0618231a 0618170c 403c2111 3ffb4a7e 0620211b 06141510 bf8c0f70 061e1f12 06181d0c 10163cff 3e800000 0618110c 0612130f 061e1b0a 40141b0e 3ffb4a7e 061a0f0f 400e0f08 3ffb4a7e 401c3b07 3e800000 0608090d 401a170a 3e800000 3e1a2cf1 3e1c00f1 060c0d09 d2820009 042e2ef1 4012131f 3e800000 d2060309 00023909 060a0b0c 101814ff 3e800000 3e1802f1 400e1907 3e800000 d206020c 00020f09 bf8c007f 7e0e540e 7e14540f 101008ff 3de38e39 d2060304 0002310d d206020e 00021d04 d00c000e 0002190e d2000004 003a2f16 d2000001 003a0300 08002904 d200000f 003a0e80 d2000010 0039010a 10120aff 3de38e39 d2000005 603a1507 10160cff 3de38e39 080c2901 d00c036a 00020d00 d200000d 21aa0b05 00080901 d2200301 00020d00 d2060011 18022904 10081af0 d2000000 00390104 06000102 7e240311 7e2c0311 102e02ff 3e800000 d2000001 003a0880 06020303 08081f00 0600010f 080a2101 06020310 7e300280 bf880033 d18a000e 00023080 d122006a 00013014 bf88002e beea080e beea246a bf880005 f09c0300 00400604 bf8c0f70 402c0d07 3ffb4a7e befe046a 7d842a80 be8e246a bf880005 f09c0300 00400600 bf8c0f70 40240d07 3ffb4a7e befe040e 080c2316 d00c016a 00022f06 d2000006 01a90280 38300d18 080c2312 d00c016a 00022f06 d2000006 01a90280 382a0d15 360c2b18 7d8a0c80 8afe6a0e bf88000c 060c1f00 7d842a80 00000d00 060c2101 00020d01 080c1f04 7d843080 00080d04 080c2105 000a0d05 81148114 bf82ffcc befe040c d00c000c 0002190e 08080902 080a0b03 080c0500 08020701 d2000000 00320905 d2000001 00320d01 7c020300 00082d12 7c022304 d002000e 00022314 d2000004 01a90280 d2000005 00390280 060c0300 7d040905 d3540004 20000106 1e000300 d2000001 01a9010d d2820000 03c20900 10020101 d2000000 00310101 d2000001 00320280 06000102 06020303 f09c0700 00400000 bf8c0f70 d2820004 44023300 d23e0001 40023301 d23e0002 40023302 d2820005 04061319 d2820006 040a1719 3e083308 befe0412 5e000b04 5e022706 befe0410 f8001c0f 00000100 bf810000 befe0480 f8001890 00000000 bf810000";
void load(Arena& arena, std::uint64_t at, const char* words) {
    std::istringstream text(words);
    for (std::uint32_t word; text >> std::hex >> word; at += 4) arena.put(at, word);
}
// One build as the game starts it, waiting at most `limit` seconds.
void build(id<MTLDevice> device, const std::string& source, MTLCompileOptions* options, const char* name, double limit) {
    NSDate* started = [NSDate date];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block std::string stage = "library";
    [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options
               completionHandler:^(id<MTLLibrary> library, NSError* error) {
        std::printf("SLOW %-18s library %8.1f s%s\n", name, -[started timeIntervalSinceNow], library ? "" : " REFUSED");
        std::fflush(stdout);
        if (!library) { std::printf("%s\n", error.localizedDescription.UTF8String); dispatch_semaphore_signal(done); return; }
        stage = "pipeline";
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"gcn_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"gcn_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        [device newRenderPipelineStateWithDescriptor:descriptor completionHandler:^(id<MTLRenderPipelineState> state, NSError* failed) {
            std::printf("SLOW %-18s pipeline %7.1f s%s\n", name, -[started timeIntervalSinceNow], state ? "" : " REFUSED");
            if (!state) std::printf("%s\n", failed.localizedDescription.UTF8String);
            std::fflush(stdout);
            dispatch_semaphore_signal(done);
        }];
    }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, std::int64_t(limit * NSEC_PER_SEC))))
        std::printf("SLOW %-18s still in %s after %.0f s\n", name, stage.c_str(), limit);
    std::fflush(stdout);
}
}
int slowPipelineTimes() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP slow pipeline: no Metal device"); return 0; }
        Arena arena;
        load(arena, vsAt, vsWords); load(arena, fetchAt, fetchWords); load(arena, psAt, psWords);
        GCNTranslateOptions vsOptions; vsOptions.stage = GCNStage::vertex; vsOptions.name = "vs_program"; vsOptions.userCount = 10;
        vsOptions.userData[0] = std::uint32_t(fetchAt);
        GCNTranslateOptions psOptions; psOptions.name = "ps_program"; psOptions.userCount = 16;
        const auto vs = translateGCN(arena.memory, vsAt, vsOptions);
        const auto ps = translateGCN(arena.memory, psAt, psOptions);
        if (!vs || !ps) { std::printf("FAIL translation: %s %s\n", vs.error.c_str(), ps.error.c_str()); return 1; }
        std::uint32_t inputCntl[32] = {};
        const std::string source = gcnMetalPrelude(true, gcnHighestUsed(vs.params), gcnHighestUsed(ps.attributes), gcnHighestUsed(ps.colors)) +
                                   vs.source + ps.source + gcnMetalEntryPoints(vs, ps, "vs_program", "ps_program", 0x2, inputCntl, true);
        std::size_t lines = 0, samples = 0;
        for (const char c : ps.source) lines += c == '\n';
        for (std::size_t at = 0; (at = ps.source.find("gcn_sample", at)) != std::string::npos; at += 10) ++samples;
        std::printf("SLOW source %zu bytes, PS %zu bytes, %zu lines, %zu sample calls\n", source.size(), ps.source.size(), lines, samples);
        MTLCompileOptions* game = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) game.mathMode = MTLMathModeSafe;
        MTLCompileOptions* size = [game copy];
        size.optimizationLevel = MTLLibraryOptimizationLevelSize;
        build(device, source + "\n// size\n", size, "optimise for size", 150);
    }
    std::fflush(stdout);
    _exit(0); // Builds past their limit would slow every later measure.
}
