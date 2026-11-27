#include "../Native/maxps4_ps4_gnm_queue.h"
#include "../Native/maxps4_ps4_gnm.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
#include <thread>
using namespace maxps4;
int main() {
    GuestELF image; auto file=guest_test::elf(40,2,true);
    guest_test::put(file,4096,160,8); assert(image.load(file.data(),file.size(),6)==0);
    PS4GuestMemory memory(image); PS4Gnm writer(memory); PS4GnmQueue queue(memory), other(memory);
    constexpr uint64_t base=0x10000, addresses=base+0x100, sizes=base+0x180;
    auto put=[&](uint64_t at,uint64_t value,size_t bytes) {
        uint8_t wire[8]{}; ps4::put_le(wire,0,value,bytes); assert(memory.write(at,wire,bytes));
    };
    auto submit=[&] { return queue.submit(1,addresses,sizes,0,0); };
    put(addresses,base,8); put(sizes,28,4); assert(writer.draw_auto(base,7,3,0)==0);
    assert(!queue.allowed() && submit()==PS4GnmQueue::error(0x10ff));
    assert(queue.done()==PS4GnmQueue::error(0x10ff));
    assert(queue.backend(true)==0 && queue.allowed()==1 && !other.allowed());
    assert(queue.submit(0,addresses,sizes,0,0)==PS4GnmQueue::error(0x1000));
    assert(queue.submit(17,addresses,sizes,0,0)==PS4GnmQueue::error(0x1000));
    assert(queue.submit(1,UINT64_MAX,sizes,0,0)==PS4GnmQueue::error(0x1000));
    assert(queue.submit(1,addresses,sizes,addresses,sizes)==PS4GnmQueue::error(0x3fff));
    for(auto size:{0U,3U,65536U*4+4}) {
        put(sizes,size,4); assert(submit()==PS4GnmQueue::error(0x1000));
    }
    put(sizes,28,4);
    put(base,0xc03f2d00,4); assert(submit()==PS4GnmQueue::error(0x3013));
    assert(writer.draw_auto(base,7,3,0)==0);
    assert(writer.vgt(base+0x40,3,63,0,0)==0);
    put(addresses,base+0x40,8); put(sizes,12,4); assert(submit()==PS4GnmQueue::error(0x3fff));
    put(addresses,base,8); put(sizes,28,4); assert(submit()==0);
    maxps4_gnm_submission info{}; assert(queue.snapshot(info,nullptr,0)==0 && info.word_count==7 && info.draw_count==1);
    const uint64_t first=info.ticket;
    uint32_t words[8]{}; words[0]=0x12345678;
    assert(queue.snapshot(info,words,6)==-EINVAL && info.ticket==0 && words[0]==0x12345678);
    put(base+4,99,4); assert(queue.snapshot(info,words,8)==0 && words[1]==3); // owned immutable copy
    assert(queue.complete(first+1,true)==-EINVAL && queue.allowed()==1);
    assert(writer.draw_auto(base,7,3,0)==0);
    for(int i=0;i<3;++i) assert(submit()==0);
    assert(!queue.allowed() && submit()==PS4GnmQueue::error(0x1001));
    assert(queue.done()==0 && !queue.allowed());
    assert(queue.complete(first,true)==0 && !queue.allowed()); // sealed until ALL queued work is done
    assert(queue.complete(first,true)==-EINVAL);
    for(int i=0;i<3;++i) { assert(queue.snapshot(info,words,8)==0); assert(queue.complete(info.ticket,true)==0); }
    assert(queue.allowed()==1 && queue.snapshot(info,nullptr,0)==-EAGAIN);
    // Two DCBs, one submission. Validate all before publication.
    put(addresses+8,base,8); put(sizes+4,28,4);
    put(addresses+8,UINT64_MAX-3,8);
    assert(queue.submit(2,addresses,sizes,0,0)==PS4GnmQueue::error(0x1000));
    assert(queue.snapshot(info,nullptr,0)==-EAGAIN);
    put(addresses+8,base,8);
    assert(queue.submit(2,addresses,sizes,0,0)==0);
    assert(queue.snapshot(info,nullptr,0)==0 && info.draw_count==2 && info.word_count==14);
    const auto old=info.ticket; assert(queue.backend(false)==0 && queue.complete(old,true)==-EINVAL);
    assert(queue.backend(true)==0 && submit()==0);
    assert(queue.snapshot(info,nullptr,0)==0 && info.ticket>old);
    assert(submit()==0);
    assert(queue.complete(info.ticket,false)==0 && !queue.allowed());
    assert(queue.snapshot(info,nullptr,0)==-EAGAIN);
    assert(submit()==PS4GnmQueue::error(0x10ff));
    assert(queue.backend(true)==0 && submit()==0);
    assert(queue.snapshot(info,nullptr,0)==0); const auto beforeReset=info.ticket;
    queue.reset(); assert(!queue.allowed() && queue.complete(beforeReset,true)==-EINVAL);
    assert(queue.backend(true)==0);
    // Host snapshots/completions can progress independently of guest dispatch.
    std::thread consumer([&] {
        for(int i=0;i<100;++i) {
            maxps4_gnm_submission next{}; uint32_t copied[7]{};
            while(queue.snapshot(next,copied,7)==-EAGAIN) std::this_thread::yield();
            assert(next.ticket>beforeReset && copied[1]==3);
            assert(queue.complete(next.ticket,true)==0);
        }
    });
    for(int i=0;i<100;++i) {
        int64_t result;
        do { result=submit(); if(result) { assert(result==PS4GnmQueue::error(0x1001)); std::this_thread::yield(); } } while(result);
    }
    consumer.join(); assert(queue.snapshot(info,nullptr,0)==-EAGAIN);
    std::puts("PASS GNM bounded owned queue: validation, immutable multi-DCB copies, FIFO, sealing, failure, reset and concurrency");
    // Null backend: a draw, EVENT_WRITE_EOP (32-bit label, interrupt), RELEASE_MEM
    // (64-bit label, no interrupt), WRITE_DATA to memory and EOS; CCB ignored.
    constexpr uint64_t labels=base+0x200;
    assert(queue.backend(7)==-EINVAL && queue.backend(PS4GnmQueue::null)==0 && queue.null_backend());
    assert(writer.draw_auto(base,7,3,0)==0);
    const uint32_t sync[]={
        0xc0044700,0x504,uint32_t(labels),(1U<<29)|(2U<<24),0x1234,0,
        0xc0054900,0x504,2U<<29,uint32_t(labels+8),0,0x89abcdef,0x01234567,
        0xc0033700,5U<<8,uint32_t(labels+16),0,0x55,
        0xc0034800,0x504,uint32_t(labels+24),2U<<29,0x77};
    for(size_t i=0;i<sizeof(sync)/4;++i) put(base+28+4*i,sync[i],4);
    put(sizes,28+sizeof(sync),4);
    uint32_t interrupts=9;
    assert(queue.submit(1,addresses,sizes,addresses,sizes,&interrupts)==0 && interrupts==1);
    auto get=[&](uint64_t at,size_t bytes) { uint8_t wire[8]{}; assert(memory.read(at,wire,bytes)); return ps4::get_le(wire,bytes); };
    assert(get(labels,4)==0x1234 && get(labels+8,8)==0x0123456789abcdefULL && get(labels+16,4)==0x55 && get(labels+24,4)==0x77);
    assert(queue.snapshot(info,nullptr,0)==-EAGAIN && queue.allowed()==1 && queue.done()==0 && queue.allowed()==1);
    // A flip needs the trailing 64-dword prepareFlip NOP; it reports its label.
    PS4GnmQueue::PreparedFlip flip;
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts,&flip)==PS4GnmQueue::error(0x1000));
    constexpr uint64_t flipped=base+0x400; // a copy with room for the 256-byte tail
    assert(writer.draw_auto(flipped,7,3,0)==0);
    for(size_t i=0;i<sizeof(sync)/4;++i) put(flipped+28+4*i,sync[i],4);
    put(addresses,flipped,8);
    const uint64_t tail=flipped+28+sizeof(sync);
    put(tail,0xc03e1000,4); put(tail+4,0x68750778,4); put(tail+8,uint32_t(labels+32),4); put(tail+12,0,4); put(tail+16,0x99,4);
    for(uint64_t at=tail+20;at<tail+256;at+=4) put(at,0,4);
    put(sizes,28+sizeof(sync)+256,4);
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts,&flip)==0 && interrupts==1);
    assert(flip.label && !flip.interrupt && flip.address==labels+32 && flip.value==0x99);
    put(tail+4,0x68750781,4); put(tail+12,0xffff0000,4);
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts,&flip)==0 && flip.label && flip.interrupt && flip.address==labels+32);
    put(tail+4,0x68750777,4);
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts,&flip)==0 && !flip.label && !flip.interrupt);
    put(tail+4,0x12345678,4);
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts,&flip)==PS4GnmQueue::error(0x1000));
    put(addresses,base,8); put(sizes,28+sizeof(sync),4);
    put(base+28+8,UINT64_MAX-3,4); put(base+28+12,(1U<<29)|0xffff,4);
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts)==PS4GnmQueue::error(0x1000));
    // Software backend: same retirement, and the draw reaches the renderer
    // (skipped here: no colour target is bound).
    put(addresses,base,8); put(sizes,28+sizeof(sync),4); put(base+28+8,uint32_t(labels),4); put(base+28+12,(1U<<29)|(2U<<24),4);
    assert(queue.backend(4)==-EINVAL && queue.backend(PS4GnmQueue::software)==0 && queue.null_backend());
    assert(queue.software_stats().draws==0);
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts)==0 && interrupts==1);
    const auto stats=queue.software_stats();
    assert(stats.draws==1 && stats.skipped==1 && stats.failed==0 && stats.error.empty());
    assert(queue.backend(PS4GnmQueue::diagnostic)==0 && !queue.null_backend() && queue.software_stats().draws==0);
    // Host completion precedes synchronization retirement, and a failed GPU
    // submission must not publish labels/events or accept further work.
    struct HostState { PS4GuestMemory* memory; uint64_t label; unsigned calls=0; bool fail=false; } host{&memory,labels};
    auto consume=+[](void* context,const uint32_t* words,size_t count)->int32_t {
        auto& h=*static_cast<HostState*>(context);
        assert(words && count>7 && words[0]==0xc0012d00);
        uint8_t wire[8]{}; assert(h.memory->read(h.label,wire,sizeof wire));
        assert(ps4::get_le(wire,8)==0); // EOP has not retired yet.
        ++h.calls; return h.fail ? -EIO : 0;
    };
    assert(queue.host_consumer(&host,consume)==0 && queue.backend(PS4GnmQueue::host)==0);
    assert(queue.null_backend() && queue.host_consumer(nullptr,nullptr)==-EBUSY);
    put(labels,0,8);
    uint8_t label_bytes[8]{};
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts)==0 && interrupts==1 && host.calls==1);
    assert(memory.read(labels,label_bytes,sizeof label_bytes) && ps4::get_le(label_bytes,8)!=0);
    // An asynchronous compute ring span reaches the consumer too, before its labels.
    put(labels,0,8);
    assert(queue.compute(base,28+sizeof(sync))==1 && host.calls==2 && queue.compute_spans_run()==1);
    assert(memory.read(labels,label_bytes,sizeof label_bytes) && ps4::get_le(label_bytes,8)!=0);
    put(labels,0,8); host.fail=true;
    assert(queue.submit(1,addresses,sizes,0,0,&interrupts)==PS4GnmQueue::error(0x10ff));
    assert(interrupts==0 && host.calls==3 && queue.allowed()==0);
    assert(memory.read(labels,label_bytes,sizeof label_bytes) && ps4::get_le(label_bytes,8)==0);
    assert(queue.submit(1,addresses,sizes,0,0)==PS4GnmQueue::error(0x10ff) && host.calls==3);
    assert(queue.backend(PS4GnmQueue::none)==0 && queue.backend(PS4GnmQueue::host)==-EINVAL);
    assert(queue.host_consumer(&host,consume)==0 && queue.backend(PS4GnmQueue::host)==0);
    queue.reset(); assert(queue.backend(PS4GnmQueue::host)==-EINVAL);
    std::puts("PASS GNM host backend: synchronous consumer, GPU-before-label ordering, failure latch, detach and reset");
    std::puts("PASS GNM null backend: label writes, EOP interrupt count, no ticket, CCB ignored, prepareFlip label/interrupt, bad address refused");
}
