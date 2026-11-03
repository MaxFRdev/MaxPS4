#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include "../Native/GCNShaderRuntime.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace maxps4::gpu;
struct State { uint64_t base, size; uint32_t vs[16], ps[17], flags, count; uint64_t ranges[48]; };
int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { std::puts("SKIP texture footprint GPU benchmark: no device"); return 0; }
        std::string shader = gcnMetalPrelude() + R"MSL(
inline float4 texture_sum(uint index, GCNMemory M, constant GCNDrawState& D, uint mode) {
    GCNRes8 t; GCNRes4 s;
    for (uint k=0;k<8;++k) t.d[k]=D.psUser[k];
    for (uint k=0;k<4;++k) s.d[k]=D.psUser[8+k];
    float4 sum=float4(0);
    for (uint k=0;k<32;++k) {
        float u=float((index+k*17u)%257u)/257.0f;
        float v=float((index*3u+k*7u)%257u)/257.0f;
        sum += mode==1u ? gcn_sample(M,t,s,false,u,v) : gcn_sample_at(M,t,s,false,u,v,false,0ul);
    }
    return sum;
}
kernel void fast_texture(device const uchar* mem [[buffer(0)]], constant GCNDrawState& D [[buffer(1)]],
    device float4* result [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    GCNMemory M={mem,D.base,D.size,D.ranges,D.rangeCount}; result[i]=texture_sum(i,M,D,1u);
}
kernel void checked_texture(device const uchar* mem [[buffer(0)]], constant GCNDrawState& D [[buffer(1)]],
    device float4* result [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    GCNMemory M={mem,D.base,D.size,D.ranges,D.rangeCount}; result[i]=texture_sum(i,M,D,0u);
}
)MSL";
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:shader.c_str()] options:options error:&error];
        if (!library) std::printf("MSL: %s\n", error.localizedDescription.UTF8String);
        assert(library);
        constexpr unsigned count=4096;
        id<MTLComputePipelineState> pipelines[2];
        const char* names[]={"checked_texture","fast_texture"};
        for(unsigned variant=0;variant<2;++variant) {
            id<MTLFunction> function=[library newFunctionWithName:[NSString stringWithUTF8String:names[variant]]];
            pipelines[variant]=[device newComputePipelineStateWithFunction:function error:&error];
            assert(pipelines[variant]);
        }
        for(const unsigned width : {16u,256u}) for(const unsigned format : {10u,16u,35u,36u,37u,38u,39u}) for(const unsigned number : {0u,1u}) {
        if(number && format<38) continue;
        const unsigned height=width/2, bytes=format==10 ? 4 : 2;
        const unsigned blockBytes=(format==35 || format==38) ? 8 : 16;
        const unsigned span=format>=35 ? ((width+3)/4)*((height+3)/4)*blockBytes : width*height*bytes;
        id<MTLBuffer> memory=[device newBufferWithLength:256+span options:MTLResourceStorageModeShared];
        auto* pixels=static_cast<uint8_t*>(memory.contents)+256;
        if(format>=35) for(unsigned k=0;k<span/blockBytes;++k) {
            const uint32_t endpoints=k%2 ? 0xf80007e0u : 0x07e0f800u, selectors=0x934e39e4;
            auto* block=pixels+k*blockBytes;
            if(format>=38) {
                for(unsigned c=0;c<(format==38 ? 1u : 2u);++c) {
                    const uint64_t channel=((c ? 0x053977053977ull : 0xfac688fac688ull)<<16) |
                        (number ? ((k+c)%2 ? 0x7f80ull : 0x807full) : ((k+c)%2 ? 0xff00ull : 0x00ffull));
                    std::memcpy(block+c*8,&channel,8);
                }
                continue;
            }
            if(format!=35) {
                const uint64_t alpha=format==36 ? 0xfedcba9876543210ull :
                    (0xfac688fac688ull<<16) | (k%2 ? 0xff00ull : 0x00ffull);
                std::memcpy(block,&alpha,8); block+=8;
            }
            std::memcpy(block,&endpoints,4); std::memcpy(block+4,&selectors,4);
        }
        else for(unsigned k=0;k<width*height;++k) {
            const uint32_t raw=(k*971u)|0xff000000u;
            std::memcpy(pixels+k*bytes,&raw,bytes);
        }
        State state{}; state.count=3;
        state.ranges[0]=0x10000; state.ranges[1]=16;
        state.ranges[3]=0x11000; state.ranges[4]=16;
        state.ranges[6]=0x14000; state.ranges[7]=span; state.ranges[8]=256;
        const uint32_t descriptors[]={0x140,(format<<20)|(number<<26),(width-1)|((height-1)<<14),0x90800facu,(width-1)<<13,0,0,0,0x12,0,0x500000,0};
        std::memcpy(state.ps,descriptors,sizeof descriptors);
        id<MTLCommandQueue> queue=[device newCommandQueue];
        id<MTLBuffer> results[2]; double times[2]{};
        for(unsigned variant=0;variant<2;++variant) {
            results[variant]=[device newBufferWithLength:count*4*sizeof(float) options:MTLResourceStorageModeShared];
        }
            // Interleave variants to limit clock/thermal bias; discard two warmups.
            for(unsigned run=0;run<8;++run) for(unsigned turn=0;turn<2;++turn) {
                const unsigned variant=(turn+run)%2;
                id<MTLCommandBuffer> command=[queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
                [encoder setComputePipelineState:pipelines[variant]]; [encoder setBuffer:memory offset:0 atIndex:0];
                [encoder setBytes:&state length:sizeof state atIndex:1]; [encoder setBuffer:results[variant] offset:0 atIndex:2];
                [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
                [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
                assert(command.status==MTLCommandBufferStatusCompleted && !command.error);
                if(run>=2 && command.GPUStartTime>0) times[variant]+=(command.GPUEndTime-command.GPUStartTime)*1000/6;
            }
        const float* checked=static_cast<const float*>(results[0].contents);
        for(unsigned variant=1;variant<2;++variant) {
            const float* result=static_cast<const float*>(results[variant].contents);
            for(unsigned k=0;k<count*4;++k) assert(std::fabs(checked[k]-result[k])<0.00001f);
        }
        if(format>=35) {
            namespace rt=maxps4::gpu::gcnrt;
            rt::GCNMemory cpu{static_cast<const unsigned char*>(memory.contents),0,0,state.ranges,state.count};
            rt::GCNRes8 texture{}; rt::GCNRes4 sampler{};
            std::copy_n(descriptors,8,texture.d); std::copy_n(descriptors+8,4,sampler.d);
            for(unsigned i=0;i<32;++i) {
                float expected[4]{};
                for(unsigned k=0;k<32;++k) {
                    const float u=float((i+k*17u)%257u)/257.0f, v=float((i*3u+k*7u)%257u)/257.0f;
                    const auto texel=rt::gcn_sample(cpu,texture,sampler,false,u,v);
                    for(unsigned c=0;c<4;++c) expected[c]+=texel[c];
                }
                for(unsigned c=0;c<4;++c) assert(std::fabs(checked[i*4+c]-expected[c])<0.0001f);
            }
            std::printf("PASS BC%u %s CPU/Metal sampled channels\n",format-34,number ? "SNORM" : "UNORM");
        }
        std::printf("PASS texture footprint GPU: %ux%u format %u number %u, checked %.3f ms, resolved %.3f ms; equal output (not iPhone FPS)\n",width,height,format,number,times[0],times[1]);
        }
    }
}
