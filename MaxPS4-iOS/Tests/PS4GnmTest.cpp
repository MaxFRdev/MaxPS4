#include "../Native/maxps4_ps4_gnm.h"
#include "../Native/PM4MetalDrawPlan.hpp"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
using namespace maxps4;
int main() {
    GuestELF image;
    auto file=guest_test::elf(40,2,true); guest_test::put(file,4096,160,8);
    assert(image.load(file.data(),file.size(),6)==0);
    PS4GuestMemory memory(image); PS4Gnm gnm(memory);
    constexpr uint64_t out=0x10000, regs=out+0x200;
    auto load = [&](size_t count) {
        std::vector<uint32_t> words(count); std::vector<uint8_t> bytes(count*4);
        assert(memory.read(out,bytes.data(),bytes.size()));
        for(size_t i=0;i<count;++i) words[i]=uint32_t(ps4::get_le(bytes.data()+4*i,4));
        return words;
    };
    uint8_t registers[48]{};
    for(size_t i=0;i<12;++i) ps4::put_le(registers,i*4,i==1 ? 0 : 0x120+i,4);
    assert(memory.write(regs,registers,sizeof(registers)));
    assert(gnm.draw_auto(out,7,3,0)==0);
    auto words=load(8);
    const uint32_t autoExpected[]={0xc0012d00,3,2,0xc0021000,0,0,0};
    for(size_t i=0;i<7;++i) assert(words[i]==autoExpected[i]);
    assert(words[7]==0 && gpu::planPM4Triangles(words.data(),7).drawCount==1);
    for(auto flags:{2U,0x10000000U}) assert(gnm.draw_auto(out,7,3,flags)==-1);
    assert(gnm.draw_auto(out,6,3,0)==-1 && gnm.draw_auto(out+1,7,3,0)==-1);
    assert(gnm.draw_auto(image.entry(),7,3,0)==-1 && gnm.draw_auto(UINT64_MAX-3,7,3,0)==-1);
    assert(gnm.draw_index(out,10,6,0x123456780ULL,0)==0);
    words=load(10); const uint32_t indexExpected[]={0xc0042700,6,0x23456780,1,6,0,0xc0021000,0,0,0};
    for(size_t i=0;i<10;++i) assert(words[i]==indexExpected[i]);
    auto commands=gpu::interpretPM4(words.data(),words.size());
    assert(commands.valid && commands.draws.size()==1 && commands.draws[0].indexed &&
        commands.draws[0].indexAddress==0x123456780ULL);
    assert(gnm.draw_index(out,10,6,0,0)==-1 && gnm.draw_index(out,10,6,3,0)==-1);
    assert(gnm.draw_index(out,9,6,2,0)==-1 && gnm.draw_index(out,10,6,2,2)==-1);
    assert(gnm.vgt(out,3,255,1,0)==0); words=load(3);
    assert(words[0]==0xc0016900 && words[1]==0x2aa && words[2]==0x100ff);
    assert(gnm.vgt(out,3,256,1,0)==-1 && gnm.vgt(out,3,1,2,0)==-1 && gnm.vgt(out,3,1,0,2)==-1);
    assert(gnm.vs(out,29,regs,0)==0); words=load(29);
    commands=gpu::interpretPM4(words.data(),words.size());
    assert(commands.valid && commands.writes.size()==7 && commands.draws.empty());
    assert(commands.writes[0].offset==0x48 && commands.writes[0].value==0x120);
    assert(commands.writes[2].offset==0x4a && commands.writes[2].value==0x122);
    assert(commands.writes[4].offset==0x207 && commands.writes[4].value==0x126);
    assert(words[17]==0xc00a1000 && words[28]==0);
    assert(gnm.vs(out,UINT32_MAX,regs,0x40)==0); words=load(29);
    assert(words[6]==((0x122 & 0xfcfffc3fU)|0x40));
    assert(gnm.vs(out,28,regs,0)==-1 && gnm.vs(out,29,0,0)==-1);
    assert(gnm.vs(out,29,regs,1)==-1 && gnm.vs(out,29,UINT64_MAX,0)==-1);
    assert(gnm.ps350(out,40,regs)==0); words=load(40);
    commands=gpu::interpretPM4(words.data(),words.size());
    assert(commands.valid && commands.writes.size()==12 && words[28]==0xc00a1000);
    assert(commands.writes[4].offset==0x1c4 && commands.writes[4].value==0x124);
    assert(commands.writes.back().offset==0x8f && commands.writes.back().value==0x12b);
    assert(gnm.ps350(out,40,0)==0); words=load(40);
    commands=gpu::interpretPM4(words.data(),words.size());
    assert(commands.valid && commands.writes.size()==4 && commands.writes.back().value==0xf);
    assert(words[10]==0xc01c1000 && gnm.ps350(out,39,regs)==-1);
    // A rejected input must preserve the complete existing command sequence.
    const auto before=load(40); ps4::put_le(registers,4,1,4);
    assert(memory.write(regs,registers,sizeof(registers)));
    assert(gnm.ps350(out,40,regs)==-1 && load(40)==before);
    assert(gnm.vs(out,29,regs,0)==-1 && load(40)==before);
    // Registers can alias output: read all of them before writing any packet.
    ps4::put_le(registers,4,0,4); assert(memory.write(out,registers,sizeof(registers)));
    assert(gnm.ps350(out,40,out)==0 && load(40)[2]==0x120);
    std::puts("PASS GNM guest command builders: indexed/auto draws, VS/PS350/VGT, exact PM4 and checked atomic copies");
    // Compute shader and dispatch: SET_SH_REG/DISPATCH_DIRECT with the compute shader type.
    for(size_t i=0;i<12;++i) ps4::put_le(registers,i*4,i==1 ? 0 : 0x120+i,4);
    assert(memory.write(regs,registers,sizeof(registers)));
    assert(gnm.cs(out,25,regs,0x40)==0); words=load(25);
    assert(words[0]==0xc0027602 && words[1]==0x20c && words[2]==0x120 && words[4]==0xc0027602 && words[5]==0x212);
    assert(words[6]==((0x122 & 0xfffffc3fU)|0x40) && words[8]==0xc0037602 && words[9]==0x207 && words[12]==0x126);
    assert(words[13]==0xc00a1000 && gpu::decodePM4Type3(words.data(),25));
    assert(gnm.cs(out,24,regs,0)==-1 && gnm.cs(out,25,regs,1)==-1 && gnm.cs(out,25,0,0)==-1);
    assert(gnm.dispatch(out,9,4,2,1,0x9)==0); words=load(9);
    const uint32_t dispatchExpected[]={0xc0031503,4,2,1,9,0xc0021000,0,0,0};
    for(size_t i=0;i<9;++i) assert(words[i]==dispatchExpected[i]);
    assert(gnm.dispatch(out,8,4,2,1,0)==-1 && gnm.dispatch(out,9,0x80000000U,1,1,0)==-1);
    // Embedded VS, flip-done wait and debug markers are exact-size streams.
    assert(gnm.embedded_vs(out,29,0)==0); words=load(29);
    commands=gpu::interpretPM4(words.data(),words.size());
    assert(commands.valid && commands.writes.size()==7 && commands.writes[2].value==0xc0000 && commands.writes[6].value==4);
    assert(gnm.embedded_vs(out,29,1)==ps4::signed32(0x8eee00ffU) && gnm.embedded_vs(out,28,0)==-1);
    assert(gnm.wait_flip_done(out,7)==0 && load(7)[0]==0xc0051000 && gnm.wait_flip_done(out,6)==-1);
    assert(gnm.pop_marker(out,6)==0); words=load(6);
    assert(words[0]==0xc0041000 && words[1]==0x68750002 && gnm.pop_marker(out,7)==-1);
    const char marker[]="Frame";
    assert(memory.write(regs,marker,sizeof(marker)));
    // strlen 5: ((5+8)>>2) + ((5+12)>>3)*2 = 7 payload-1 words, 9 in all.
    assert(gnm.push_marker(out,8,regs)==-1 && gnm.push_marker(out,9,regs)==0); words=load(9);
    assert(words[0]==0xc0071000 && words[1]==0x68750001 && words[2]==0x6d617246 && words[3]==0x65 && words[8]==0);
    assert(gnm.push_marker(out,9,0)==-1 && gpu::decodePM4Type3(words.data(),9));
    // Default state: the context clear, then a NOP up to 256 dwords.
    assert(gnm.init_default_state350(out,255)==0 && gnm.init_default_state350(out,300)==256);
    words=load(256);
    assert(words[0]==0xc0012800 && words[3]==0xc0001200 && words[5]==0xc0055800 && words[11]==10);
    assert(words[12]==0xc0f21000 && gpu::decodePM4Type3(words.data(),256));
    std::puts("PASS GNM compute shader/dispatch, embedded VS, flip-done wait, markers and default state streams");
    // More writers: each fills exactly the size the game passes, as decodable PM4.
    assert(gnm.draw_index_offset(out,9,12,6,0)==0); words=load(9);
    assert(words[0]==0xc0033500 && words[1]==6 && words[2]==12 && words[3]==6 && words[5]==0xc0021000);
    assert(gpu::decodePM4Type3(words.data(),9) && gnm.draw_index_offset(out,8,12,6,0)==-1);
    assert(gnm.draw_indirect(out,9,0x40,2,3,4,1,false)==0); words=load(9);
    assert(words[0]==0xc0032401 && words[1]==0x40 && words[2]==0x4f && words[3]==0x50 && words[4]==2);
    assert(gnm.draw_indirect(out,9,0x40,2,0,0,0,true)==0); words=load(9);
    assert(words[0]==0xc0032500 && words[2]==0 && words[3]==0 && words[4]==0 && gpu::decodePM4Type3(words.data(),9));
    assert(gnm.draw_indirect(out,9,0,7,0,0,0,false)==-1 && gnm.draw_indirect(out,9,0,2,16,0,0,false)==-1);
    assert(gnm.dispatch_indirect(out,7,0x80,0)==0); words=load(7);
    assert(words[0]==0xc0011602 && words[1]==0x80 && words[2]==1 && gpu::decodePM4Type3(words.data(),7));
    assert(gnm.dispatch_init_default_state(out,255)==0 && gnm.dispatch_init_default_state(out,256)==256);
    words=load(256); assert(words[0]==0xc0017602 && words[1]==0x216 && gpu::decodePM4Type3(words.data(),256));
    assert(gnm.init_context_state(out,0x20,0x20)==0x20 && gnm.init_context_state(out,0x21,0x20)==0);
    words=load(0x20); assert(words[0]==0xc01e1000 && gpu::decodePM4Type3(words.data(),0x20));
    assert(gnm.reset_vgt(out,3)==0); words=load(3); assert(words[0]==0xc0016900 && words[1]==0x2aa && words[2]==0xff);
    assert(gnm.ps(out,40,0)==0); words=load(40);
    assert(words[0]==0xc0027600 && words[4]==0xc0016900 && words[5]==0x203 && gpu::decodePM4Type3(words.data(),40));
    assert(memory.write(regs,marker,sizeof(marker))); // The default states above overwrote it.
    assert(gnm.set_marker(out,9,regs)==0); words=load(9); assert(words[1]==0x68750003);
    std::puts("PASS GNM indirect and offset draws, indirect dispatch, compute/context defaults, VGT reset, PS and set markers");
    // LS/HS/ES/GS, markers and compute waits fill exactly the reserved size.
    for(size_t i=0;i<12;++i) ps4::put_le(registers,i*4,i==1 ? 0 : 0x120+i,4);
    assert(memory.write(regs,registers,sizeof(registers)));
    assert(gnm.ls(out,23,regs,0)==0); words=load(23);
    assert(words[0]==0xc0027600 && words[1]==0x148 && words[2]==0x120 && words[11]==0xc00a1000 && gpu::decodePM4Type3(words.data(),23));
    assert(gnm.ls(out,22,regs,0)==-1 && gnm.ls(out,23,0,0)==-1);
    assert(gnm.hs(out,30,regs,0x55,false)==0); words=load(30);
    assert(words[0]==0xc0027600 && words[1]==0x108 && gpu::decodePM4Type3(words.data(),30));
    assert(gnm.hs(out,29,regs,0x55,false)==-1 && gnm.hs(out,29,regs,0x55,true)==0 && gpu::decodePM4Type3(load(29).data(),29));
    assert(gnm.es(out,20,regs,0)==0 && gpu::decodePM4Type3(load(20).data(),20) && load(20)[1]==0xc8);
    assert(gnm.gs(out,29,regs)==0); words=load(29);
    assert(words[1]==0x88 && gpu::decodePM4Type3(words.data(),29));
    ps4::put_le(registers,4,1,4); assert(memory.write(regs,registers,sizeof(registers)));
    assert(gnm.gs(out,29,regs)==-1 && gnm.es(out,20,regs,0)==-1); // A program address above 40 bits.
    assert(gnm.compute_wait_address(out,14,0x123456780ULL,0xff,3,7)==0); words=load(14);
    assert(words[0]==0xc0053c00 && words[1]==0x13 && words[2]==0x23456780 && words[3]==1 && words[4]==7 && words[5]==0xff);
    assert(gpu::decodePM4Type3(words.data(),14) && gnm.compute_wait_address(out,13,0x10000,1,3,7)==-1);
    assert(gnm.compute_wait_semaphore(out,10,0x1000,1)==0 && load(10)[0]==0xc0013900 && gpu::decodePM4Type3(load(10).data(),10));
    assert(gnm.dispatch_indirect_mec(out,8,0x80,9)==0); words=load(8);
    assert(words[0]==0xc0021603 && words[1]==0x80 && words[2]==0 && words[3]==9 && words[4]==0xc0021000);
    assert(gnm.dispatch_indirect_mec(out,8,0x82,0)==-1 && gnm.dispatch_indirect_mec(out,7,0x80,0)==-1);
    assert(gnm.nop_fill(out,4,4)==0 && load(4)[0]==0xc0021000 && gnm.nop_fill(out,5,4)==-1 && gnm.nop_fill(out,1,0)==-1);
    assert(memory.write(regs,marker,sizeof(marker)));
    assert(gnm.push_color_marker(out,9,regs)==-1 && gnm.push_color_marker(out,10,regs)==0 && gpu::decodePM4Type3(load(10).data(),10));
    std::puts("PASS GNM LS/HS/ES/GS shaders, color and ding-dong markers, compute waits and MEC indirect dispatch");
}
