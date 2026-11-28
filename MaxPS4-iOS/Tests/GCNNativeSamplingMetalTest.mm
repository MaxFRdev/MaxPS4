// Research only: compare current buffer sampling, native texel reads with the
// same arithmetic, and native hardware filtering. Never selects a game backend.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Native/GCNTranslator.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace maxps4::gpu;
struct NativeState { uint64_t base, size; uint32_t vs[16], ps[17], flags, count; uint64_t ranges[48]; uint32_t stepRate[2]; };
static_assert(sizeof(NativeState)==552,"draw-state layout");
int main() {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device) { std::puts("SKIP native sampling research: no Metal device"); return 0; }
        std::string source=gcnMetalPrelude()+R"MSL(
inline float4 native_fetch(texture2d<float> image, int x, int y, uint mode) {
    bool border=false;
    int ax=gcn_wrap(x,int(image.get_width()),mode,border);
    int ay=gcn_wrap(y,int(image.get_height()),mode,border);
    return image.read(uint2(ax,ay));
}
inline float4 native_read_sample(texture2d<float> image, float2 uv, uint mode, bool linear) {
    float2 pixel=uv*float2(image.get_width(),image.get_height());
    if(!linear) return native_fetch(image,int(floor(pixel.x)),int(floor(pixel.y)),mode);
    float2 f=pixel-0.5f;
    int x=int(floor(f.x)), y=int(floor(f.y));
    float ax=f.x-float(x), ay=f.y-float(y);
    float4 a=native_fetch(image,x,y,mode), b=native_fetch(image,x+1,y,mode);
    float4 c=native_fetch(image,x,y+1,mode), d=native_fetch(image,x+1,y+1,mode);
    float4 result;
    for(uint k=0;k<4;++k) result[k]=(a[k]*(1.0f-ax)+b[k]*ax)*(1.0f-ay)+(c[k]*(1.0f-ax)+d[k]*ax)*ay;
    return result;
}
inline float4 native_filter_sample(texture2d<float> image,float2 uv,uint mode,bool linear) {
    constexpr sampler rp(coord::normalized,address::repeat,filter::nearest);
    constexpr sampler rl(coord::normalized,address::repeat,filter::linear);
    constexpr sampler mp(coord::normalized,address::mirrored_repeat,filter::nearest);
    constexpr sampler ml(coord::normalized,address::mirrored_repeat,filter::linear);
    constexpr sampler cp(coord::normalized,address::clamp_to_edge,filter::nearest);
    constexpr sampler cl(coord::normalized,address::clamp_to_edge,filter::linear);
    if(mode==0u) return linear ? image.sample(rl,uv,level(0.0f)) : image.sample(rp,uv,level(0.0f));
    if(mode==1u) return linear ? image.sample(ml,uv,level(0.0f)) : image.sample(mp,uv,level(0.0f));
    return linear ? image.sample(cl,uv,level(0.0f)) : image.sample(cp,uv,level(0.0f));
}
inline void sample_series(uint i,GCNMemory M,constant GCNDrawState& D,texture2d<float> image,uint variant,device float4* output) {
    GCNRes8 t; GCNRes4 s;
    for(uint k=0;k<8;++k) t.d[k]=D.psUser[k];
    for(uint k=0;k<4;++k) s.d[k]=D.psUser[8+k];
    for(uint k=0;k<16;++k) {
        float2 uv=float2(float((i*17u+k*31u)%4093u),float((i*29u+k*11u)%4093u))/4093.0f*1.5f-0.25f;
        output[i*16u+k]=variant==0u ? gcn_sample(M,t,s,false,uv.x,uv.y) :
            variant==1u ? native_read_sample(image,uv,D.vsUser[0],D.flags!=0u) : native_filter_sample(image,uv,D.vsUser[0],D.flags!=0u);
    }
}
kernel void reference_samples(device const uchar* mem [[buffer(0)]],constant GCNDrawState& D [[buffer(1)]],
    device float4* output [[buffer(2)]],texture2d<float> image [[texture(0)]],uint i [[thread_position_in_grid]]) {
    GCNMemory M={mem,D.base,D.size,D.ranges,D.rangeCount}; sample_series(i,M,D,image,0u,output);
}
kernel void read_samples(device const uchar* mem [[buffer(0)]],constant GCNDrawState& D [[buffer(1)]],
    device float4* output [[buffer(2)]],texture2d<float> image [[texture(0)]],uint i [[thread_position_in_grid]]) {
    GCNMemory M={mem,D.base,D.size,D.ranges,D.rangeCount}; sample_series(i,M,D,image,1u,output);
}
kernel void filter_samples(device const uchar* mem [[buffer(0)]],constant GCNDrawState& D [[buffer(1)]],
    device float4* output [[buffer(2)]],texture2d<float> image [[texture(0)]],uint i [[thread_position_in_grid]]) {
    GCNMemory M={mem,D.base,D.size,D.ranges,D.rangeCount}; sample_series(i,M,D,image,2u,output);
}
)MSL";
        MTLCompileOptions* options=[MTLCompileOptions new];
        if(@available(macOS 15.0,iOS 18.0,*)) options.mathMode=MTLMathModeSafe;
        NSError* error=nil;
        id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if(!library) std::printf("MSL: %s\n",error.localizedDescription.UTF8String);
        assert(library);
        id<MTLComputePipelineState> pipelines[3];
        const char* names[]={"reference_samples","read_samples","filter_samples"};
        for(unsigned k=0;k<3;++k) {
            pipelines[k]=[device newComputePipelineStateWithFunction:[library newFunctionWithName:[NSString stringWithUTF8String:names[k]]] error:&error];
            assert(pipelines[k]);
        }
        constexpr unsigned threads=4096, samples=threads*16, outputBytes=samples*4*sizeof(float);
        id<MTLCommandQueue> queue=[device newCommandQueue];
        for(unsigned width : {16u,256u,1024u}) for(unsigned wrap : {0u,1u,2u}) for(unsigned linear : {0u,1u}) {
          @autoreleasepool {
            const NSUInteger alignment=[device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA8Unorm];
            assert(alignment>0);
            const NSUInteger prefix=((256+alignment-1)/alignment)*alignment;
            const unsigned height=width/2,pitch=unsigned((((width+8)*4+alignment-1)/alignment)*alignment/4),row=((width*4+255)/256)*256;
            id<MTLBuffer> memory=[device newBufferWithLength:prefix+pitch*height*4 options:MTLResourceStorageModeShared];
            id<MTLBuffer> upload=[device newBufferWithLength:row*height options:MTLResourceStorageModeShared];
            for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
                uint32_t raw=(x+y*width+1)*2654435761u; raw^=raw>>13;
                std::memcpy(static_cast<uint8_t*>(memory.contents)+prefix+(y*pitch+x)*4,&raw,4);
                std::memcpy(static_cast<uint8_t*>(upload.contents)+y*row+x*4,&raw,4);
            }
            auto* descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
            descriptor.storageMode=MTLStorageModePrivate; descriptor.usage=MTLTextureUsageShaderRead;
            id<MTLTexture> image=[device newTextureWithDescriptor:descriptor]; assert(image);
            MTLTextureDescriptor* linearDescriptor=[descriptor copy]; linearDescriptor.storageMode=MTLStorageModeShared;
            id<MTLTexture> linearImage=[memory newTextureWithDescriptor:linearDescriptor offset:prefix bytesPerRow:pitch*4];
            const bool enabled[]={true,true,true,linearImage!=nil,
                linearImage!=nil && (wrap==2 || [device supportsFamily:MTLGPUFamilyApple2])};
            id<MTLCommandBuffer> copying=[queue commandBuffer];
            id<MTLBlitCommandEncoder> blit=[copying blitCommandEncoder];
            [blit copyFromBuffer:upload sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row*height sourceSize:MTLSizeMake(width,height,1)
                toTexture:image destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [blit endEncoding]; [copying commit]; [copying waitUntilCompleted];
            assert(copying.status==MTLCommandBufferStatusCompleted && !copying.error);
            NativeState state{}; state.count=3; state.flags=linear; state.vs[0]=wrap;
            state.ranges[0]=0x10000; state.ranges[1]=16; state.ranges[3]=0x11000; state.ranges[4]=16;
            state.ranges[6]=0x14000; state.ranges[7]=pitch*height*4; state.ranges[8]=prefix;
            const uint32_t values[]={0x140,10u<<20,(width-1)|((height-1)<<14),0x90800facu,(pitch-1)<<13,0,0,0,wrap|(wrap<<3),0,linear ? 0x500000u : 0u,0};
            std::memcpy(state.ps,values,sizeof values);
            id<MTLBuffer> result[5]; double times[5]{}; unsigned timed[5]{};
            for(auto& output : result) output=[device newBufferWithLength:outputBytes options:MTLResourceStorageModeShared];
            for(unsigned run=0;run<8;++run) for(unsigned turn=0;turn<5;++turn) {
                const unsigned mode=(run+turn)%5;
                if(!enabled[mode]) continue;
                id<MTLCommandBuffer> command=[queue commandBuffer];
                id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
                [encoder setComputePipelineState:pipelines[mode==0 ? 0 : (mode%2==1 ? 1 : 2)]];
                [encoder setBuffer:memory offset:0 atIndex:0]; [encoder setBytes:&state length:sizeof state atIndex:1];
                [encoder setBuffer:result[mode] offset:0 atIndex:2]; [encoder setTexture:mode>=3 ? linearImage : image atIndex:0];
                [encoder dispatchThreads:MTLSizeMake(threads,1,1) threadsPerThreadgroup:MTLSizeMake(64,1,1)];
                [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
                assert(command.status==MTLCommandBufferStatusCompleted && !command.error);
                if(run>=2 && command.GPUStartTime>0) { times[mode]+=(command.GPUEndTime-command.GPUStartTime)*1000; ++timed[mode]; }
            }
            double errors[4]{}; unsigned quantized[4]{}, differing[4]{};
            const auto* reference=static_cast<const float*>(result[0].contents);
            for(unsigned mode=1;mode<5;++mode) {
                if(!enabled[mode]) continue;
                const auto* actual=static_cast<const float*>(result[mode].contents);
                for(unsigned k=0;k<samples*4;++k) {
                    assert(std::isfinite(actual[k]) && std::isfinite(reference[k]));
                    errors[mode-1]=std::max(errors[mode-1],double(std::fabs(reference[k]-actual[k])));
                    quantized[mode-1]=std::max(quantized[mode-1],unsigned(std::abs(int(std::lround(reference[k]*255))-int(std::lround(actual[k]*255)))));
                    differing[mode-1]+=reference[k]!=actual[k];
                }
            }
            // Native reads retain the reference's addressing/filter arithmetic.
            // Hardware filtering is research: differences are reported, never silently accepted for the game.
            assert(errors[0]<=0.000001 && quantized[0]<=1);
            if(enabled[3]) assert(errors[2]<=0.000001 && quantized[2]<=1);
            for(unsigned k=0;k<5;++k) times[k]=timed[k] ? times[k]/timed[k] : -1;
            std::printf("NATIVE SAMPLING %ux%u wrap %u linear %u: reference %.3f ms, read %.3f ms, filter %.3f ms, buffer-read %.3f ms, buffer-filter %.3f ms\n",
                width,height,wrap,linear,times[0],times[1],times[2],times[3],times[4]);
            std::printf("NATIVE ERRORS: read %.8f/%u (%u unequal), filter %.8f/%u (%u unequal), buffer-read %.8f/%u (%u unequal), buffer-filter %.8f/%u (%u unequal)\n",
                errors[0],quantized[0],differing[0],errors[1],quantized[1],differing[1],errors[2],quantized[2],differing[2],errors[3],quantized[3],differing[3]);
          }
        }
        std::puts("PASS native RGBA8 texel-read conformance; hardware filtering remains an unintegrated experiment, not game FPS");
    }
}
