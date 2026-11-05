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
        // Three-operand integer instructions on hashed values, one thread per
        // 64-byte record: the kernel must store what the interpreter stores.
        {
            constexpr std::uint64_t program = 0x12000, results = 0x13000;
            const std::uint32_t operations[] = {322, 329, 335, 338, 339, 341, 342, 344, 345, 364};
            const std::uint32_t sgprs[16] = {0, 0, 0, 0, std::uint32_t(results), 64u << 16, 32, 0x50fac, 0x9e3779b1, 0x85ebca6b, 0xc2b2ae35};
            auto load = [&](Arena& arena) {
                std::uint64_t at = program;
                auto emit = [&](std::uint32_t word) { arena.put(at, word); at += 4; };
                for (unsigned k = 0; k < 3; ++k) { emit(0xd2d20002 + k); emit(0x100 | (8 + k) << 9); } // v_mul_lo_u32 v[2+k], v0, s[8+k]
                unsigned k = 0;
                for (const std::uint32_t operation : operations) {
                    emit(0xd0000000 | operation << 17 | (5 + k));
                    emit(0x102 | 0x103 << 9 | 0x104u << 18);                                          // v2, v3, v4
                    emit(0xe0702000 | 4 * k); emit(0x80010000 | (5 + k) << 8);                        // buffer_store_dword v[5+k], v0, s[4:7], 0 idxen
                    ++k;
                }
                emit(0xbf810000);
            };
            Arena expected, actual;
            load(expected); load(actual);
            GCNWave wave(expected.memory, 32);
            for (unsigned k = 0; k < 16; ++k) wave.s[k] = sgprs[k];
            for (unsigned lane = 0; lane < 32; ++lane) wave.v(0, lane) = lane;
            assert(wave.run(program));
            GCNTranslateOptions integers;
            integers.stage = GCNStage::compute; integers.name = "cs_integers"; integers.userCount = 16;
            const auto cs = translateGCN(actual.memory, program, integers);
            if (!cs) { std::printf("FAIL integer translation: %s\n", cs.error.c_str()); return 1; }
            const std::string text = gcnMetalPrelude() + cs.source + gcnMetalComputeEntry("cs_integers");
            id<MTLLibrary> kernels = [device newLibraryWithSource:[NSString stringWithUTF8String:text.c_str()] options:nil error:&error];
            if (!kernels) { std::printf("FAIL integer MSL: %s\n", error.localizedDescription.UTF8String); return 1; }
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[kernels newFunctionWithName:@"gcn_compute"] error:&error];
            if (!pipeline) { std::printf("FAIL integer pipeline: %s\n", error.localizedDescription.UTF8String); return 1; }
            id<MTLBuffer> output = [device newBufferWithBytes:actual.bytes.data() length:actual.bytes.size() options:MTLResourceStorageModeShared];
            DispatchState state{};
            state.base = Arena::start; state.size = actual.bytes.size();
            for (unsigned k = 0; k < 16; ++k) state.user[k] = sgprs[k];
            state.userCount = 16;
            id<MTLCommandBuffer> run = [queue commandBuffer];
            id<MTLComputeCommandEncoder> stage = [run computeCommandEncoder];
            [stage setComputePipelineState:pipeline];
            [stage setBuffer:output offset:0 atIndex:0];
            [stage setBytes:&state length:sizeof state atIndex:1];
            [stage dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [stage endEncoding];
            [run commit];
            [run waitUntilCompleted];
            if (run.error) { std::printf("FAIL integer GPU: %s\n", run.error.localizedDescription.UTF8String); return 1; }
            const auto* stored = static_cast<const std::uint8_t*>(output.contents);
            for (std::size_t k = 0; k < expected.bytes.size(); ++k) if (stored[k] != expected.bytes[k]) {
                std::printf("FAIL integer instructions: byte 0x%zx is %u, not %u\n", k, stored[k], expected.bytes[k]);
                return 1;
            }
            assert(expected.bytes[results + 64 * 31 + 36 - Arena::start] || expected.bytes[results + 64 * 31 + 37 - Arena::start]);
            std::puts("PASS GCN compute on Metal: three-operand integer instructions match the interpreter");
        }
    }
    return 0;
}
