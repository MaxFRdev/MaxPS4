#include "../Native/GCNShaderRuntime.hpp"
#include "GCNTestPrograms.h"
#include <cassert>
#include <cstdio>
namespace rt = maxps4::gpu::gcnrt;
using namespace maxps4::gpu;
int main() {
    constexpr std::uint64_t base = 0x14000, program = 0x10400;
    constexpr unsigned width = 7, height = 9, pitch = 12, size = 144;
    // All BC3 selectors, including pixel 5 crossing the 32-bit boundary.
    constexpr std::uint64_t indices = 0xfac688fac688ull;
    const unsigned pairs[][2] = {{255,0},{0,255},{128,128},{200,50}};
    const float alpha[4][8] = {
        {1,0,6.0f/7,5.0f/7,4.0f/7,3.0f/7,2.0f/7,1.0f/7},
        {0,1,0.2f,0.4f,0.6f,0.8f,0,1},
        {128.0f/255,128.0f/255,128.0f/255,128.0f/255,128.0f/255,128.0f/255,0,1},
        {200.0f/255,50.0f/255,1250.0f/1785,1100.0f/1785,950.0f/1785,800.0f/1785,650.0f/1785,500.0f/1785}
    };
    // Ascending endpoints still use four opaque RGB colours in BC2 and BC3.
    const float rgb[4][3] = {{0,1,0},{1,0,0},{1.0f/3,2.0f/3,0},{2.0f/3,1.0f/3,0}};
    for (unsigned format : {36u,37u}) {
        gcn_test::Arena arena;
        arena.code(program,{0xf0000f00,0x00000400,0xbf810000});
        for (unsigned block=0;block<9;++block) {
            const auto& pair = pairs[block%4];
            const std::uint64_t packed = format == 36 ? 0xfedcba9876543210ull :
                (indices << 16) | (std::uint64_t(pair[1]) << 8) | pair[0];
            arena.put(base+16*block,unsigned(packed));
            arena.put(base+16*block+4,unsigned(packed>>32));
            arena.put(base+16*block+8,0xf80007e0u);
            arena.put(base+16*block+12,0x934e39e4u);
        }
        rt::GCNRes8 texture{{unsigned(base>>8),format<<20,(width-1)|((height-1)<<14),
            0x90800fac,(pitch-1)<<13,0,0,0}};
        rt::GCNMemory memory{arena.bytes.data(),gcn_test::Arena::start,arena.bytes.size()};
        auto expected = [&](unsigned x,unsigned y,unsigned c) {
            const unsigned pixel=(y%4)*4+x%4, block=(y/4)*3+x/4;
            return c<3 ? rgb[(x+y)%4][c] : format==36 ? float(pixel)/15 : alpha[block%4][pixel%8];
        };
        for (bool cached : {false,true}) {
            GCNImageCache cache;
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
                GCNWave wave(arena.memory,1);
                if (cached) wave.images=&cache;
                std::copy_n(texture.d,8,wave.s.begin()); wave.v(0,0)=x; wave.v(1,0)=y;
                std::uint64_t extent=0;
                wave.resource=[&](std::uint64_t address,std::uint64_t bytes) { assert(address==base); extent=bytes; };
                assert(wave.run(program) && extent==size);
                const auto result=rt::gcn_image_load(memory,texture,x,y);
                for (unsigned c=0;c<4;++c) {
                    assert(std::fabs(gcn_detail::f32(wave.v(4+c,0))-expected(x,y,c))<0.000001f);
                    assert(std::fabs(result[c]-expected(x,y,c))<0.000001f);
                }
            }
            if (cached) assert(cache.at(base).size()==size);
        }
        rt::GCNRes4 sampler{{0x12,0,1u<<20,0}};
        const auto filtered=rt::gcn_sample(memory,texture,sampler,true,4.0f,0.5f);
        for (unsigned c=0;c<4;++c) assert(std::fabs(filtered[c]-(expected(3,0,c)+expected(4,0,c))*0.5f)<0.000001f);
        auto swizzled=texture; swizzled.d[3]=(texture.d[3]&~0xfffu)|0x67u;
        const auto result=rt::gcn_image_load(memory,swizzled,1,1);
        assert(std::fabs(result[0]-expected(1,1,3))<0.000001f && result[2]==1 && result[3]==0);
        for (unsigned number : {7u,9u}) {
            auto invalid=texture; invalid.d[1]|=number<<26;
            GCNWave wave(arena.memory,1); std::copy_n(invalid.d,8,wave.s.begin());
            assert(!wave.run(program));
            const auto zero=rt::gcn_image_load(memory,invalid,0,0);
            for (unsigned c=0;c<4;++c) assert(zero[c]==0);
        }
        auto invalid=texture; invalid.d[4]=0;
        GCNWave wave(arena.memory,1); std::copy_n(invalid.d,8,wave.s.begin()); assert(!wave.run(program));
        // Exact final-block boundary, and one-byte truncation must fail safely.
        auto limited=arena.memory;
        limited.read=[&](std::uint64_t a,void* d,std::size_t n) {
            if (a>=base && (a-base>size || n>size-(a-base))) return false;
            return arena.memory.read(a,d,n);
        };
        GCNWave valid(limited,1); std::copy_n(texture.d,8,valid.s.begin()); valid.v(0,0)=6; valid.v(1,0)=8;
        assert(valid.run(program));
        limited.read=[&](std::uint64_t a,void* d,std::size_t n) {
            // Block 7 is the last visible block; block 8 is row padding.
            if (a>=base && (a-base>127 || n>127-(a-base))) return false;
            return arena.memory.read(a,d,n);
        };
        GCNWave truncated(limited,1); std::copy_n(texture.d,8,truncated.s.begin()); truncated.v(0,0)=6; truncated.v(1,0)=8;
        assert(!truncated.run(program));
    }
    std::puts("PASS BC2/BC3: independent RGB/alpha palettes, selector boundary, padded/partial blocks, cache, swizzle, filtering and invalid resources");
}
