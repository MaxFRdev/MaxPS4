// A translated GCN compute program as a Metal kernel on a real GPU, against
// the interpreter: two groups of 32 threads load V# records, store them to
// an 8_8_8_8 UNORM buffer and a linear image. Guest memory must match.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
using namespace maxps4::gpu;
using namespace gcn_test;
namespace {
// Must match GCNDispatchState in gcnMetalComputeEntry().
struct DispatchState { std::uint64_t base, size; std::uint32_t user[16], userCount, rsrc2, rangeCount, pad; std::uint64_t ranges[48]; };
}
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP GCN compute on Metal: no Metal device"); return 0; }
        Arena interpreted, translated;
        loadComputeCS(interpreted); loadComputeCS(translated);
        for (unsigned group = 0; group < 2; ++group) {
            GCNWave wave(interpreted.memory, 32);
            for (unsigned k = 0; k < 16; ++k) wave.s[k] = computeSgprs[k];
            wave.s[16] = group;
            for (unsigned lane = 0; lane < 32; ++lane) wave.v(0, lane) = lane;
            assert(wave.run(computeCS));
        }
        GCNTranslateOptions options;
        options.stage = GCNStage::compute; options.name = "cs_copy"; options.userCount = 16;
        const auto cs = translateGCN(translated.memory, computeCS, options);
        if (!cs) { std::printf("FAIL translation: %s\n", cs.error.c_str()); return 1; }
        const std::string source = gcnMetalPrelude() + cs.source + gcnMetalComputeEntry("cs_copy");
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:nil error:&error];
        if (!library) { std::printf("FAIL MSL: %s\n", error.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"gcn_compute"] error:&error];
        if (!state) { std::printf("FAIL pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
        id<MTLBuffer> memory = [device newBufferWithBytes:translated.bytes.data() length:translated.bytes.size()
                                                  options:MTLResourceStorageModeShared];
        DispatchState dispatch{};
        dispatch.base = Arena::start; dispatch.size = translated.bytes.size();
        for (unsigned k = 0; k < 16; ++k) dispatch.user[k] = computeSgprs[k];
        dispatch.userCount = 16; dispatch.rsrc2 = 0x80; // thread group id x after the user SGPRs
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:state];
        [encoder setBuffer:memory offset:0 atIndex:0];
        [encoder setBytes:&dispatch length:sizeof dispatch atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.error) { std::printf("FAIL GPU: %s\n", commands.error.localizedDescription.UTF8String); return 1; }
        const auto* gpu = static_cast<const std::uint8_t*>(memory.contents);
        std::size_t different = 0;
        for (std::size_t k = 0; k < interpreted.bytes.size(); ++k) different += gpu[k] != interpreted.bytes[k];
        if (different) { std::printf("FAIL GCN compute on Metal: %zu bytes differ from the interpreter\n", different); return 1; }
        assert(gpu[computeImage + 4 * (8 + 1) + 2 - Arena::start] == 32);
        std::puts("PASS GCN compute on Metal: buffer and image stores match the interpreter");
    }
    return 0;
}
