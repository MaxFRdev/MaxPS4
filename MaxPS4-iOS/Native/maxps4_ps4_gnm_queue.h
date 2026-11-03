#pragma once
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_abi.h"
#include "maxps4_gnm_submission_abi.h"
#include "PM4MetalDrawPlan.hpp"
#include "PM4SoftwareRenderer.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <new>

namespace maxps4 {
// Backend 1 is the existing native triangle diagnostic pipeline. Its preflight
// refuses all guest shader/register state; acceptance means owned work
// enqueued, never GPU or display completion.
// Backend 2 (null, for the boot probe) draws nothing: it retires decodable
// work at submission, applying only its synchronization packets (EOP/EOS/
// RELEASE_MEM label writes and WRITE_DATA to memory) and counting the EOP
// interrupts they request. Waits (WAIT_REG_MEM) and constant buffers are
// ignored.
// Backend 3 (software, for the boot probe) is the null backend that also
// executes each submission with the PM4 software renderer (GCN interpreter)
// before retiring it, so colour targets receive what the game drew.
class PS4GnmQueue final {
public:
    static constexpr size_t buffer_limit=16, word_limit=65536, queue_limit=4;
    static constexpr uint32_t none=0, diagnostic=1, null=2, software=3, host=4;
    using HostConsumer = int32_t (*)(void*, const uint32_t*, size_t);
    // Called synchronously before retirement. The consumer must copy resources
    // and finish GPU work before returning; it must not reenter this queue.
    int32_t host_consumer(void* context, HostConsumer consumer) {
        std::lock_guard<std::mutex> lock(lock_);
        if (kind_ == host) return -EBUSY;
        host_context_=context; host_consumer_=consumer; return 0;
    }
    static int64_t error(uint32_t code) { return ps4::signed32(0x80d10000U|code); }
    explicit PS4GnmQueue(PS4GuestMemory& memory) : memory_(memory),
        guest_{[this](uint64_t a, void* d, size_t n) { return memory_.read(a,d,n); },
               [this](uint64_t a, const void* d, size_t n) { return memory_.write(a,d,n); }} {}
    int32_t backend(uint32_t kind) {
        if (kind>host) return -EINVAL;
        std::lock_guard<std::mutex> lock(lock_);
        if (kind==host && !host_consumer_) return -EINVAL;
        if (kind!=kind_) {
            clear();
            if (kind_==host) { host_context_=nullptr; host_consumer_=nullptr; }
        }
        kind_=kind; failed_=false;
        return 0;
    }
    // True for both backends that retire work at submission (null, software).
    bool null_backend() { std::lock_guard<std::mutex> lock(lock_); return kind_>=null; }
    bool host_backend() { std::lock_guard<std::mutex> lock(lock_); return kind_==host; }
    // Software backend counters and its first error (empty when none).
    gpu::PM4SoftwareRenderer::Stats software_stats() {
        std::lock_guard<std::mutex> lock(lock_);
        return renderer_ ? renderer_->stats : gpu::PM4SoftwareRenderer::Stats{};
    }
    int64_t allowed() {
        std::lock_guard<std::mutex> lock(lock_);
        return kind_ && !failed_ && !sealed_ && jobs_.size()<queue_limit ? 1 : 0;
    }
    // What the last DCB of sceGnmSubmitAndFlipCommandBuffers asks for once the
    // flip is done: its final 64 dwords are a "prepareFlip" NOP that the
    // driver turns into a label write and/or an EOP interrupt.
    struct PreparedFlip { bool label=false, interrupt=false; uint64_t address=0; uint32_t value=0; };
    // `interrupts` (null backend only) receives the EOP interrupts requested;
    // a non-null `flip` requires the prepareFlip packet and receives it.
    int64_t submit(uint32_t count, uint64_t addresses, uint64_t sizes,
                   uint64_t constant_addresses, uint64_t constant_sizes, uint32_t* interrupts=nullptr,
                   PreparedFlip* flip=nullptr) {
        if (interrupts) *interrupts=0;
        if (flip) *flip={};
        std::lock_guard<std::mutex> lock(lock_);
        if (!kind_ || failed_) return error(0x10ff);
        if (sealed_ || jobs_.size()>=queue_limit || next_==UINT64_MAX) return error(0x1001);
        if (!count || count>buffer_limit || !addresses || !sizes || (addresses&7) || (sizes&3)) return error(0x1000);
        // Constant-engine packets/synchronization are not implemented. Never
        // ignore non-null CCB tables, even if they happen to contain zero sizes,
        // unless nothing is drawn.
        if ((constant_addresses || constant_sizes) && kind_<null) return error(0x3fff);
        try {
            std::array<uint8_t,buffer_limit*8> address_bytes{};
            std::array<uint8_t,buffer_limit*4> size_bytes{};
            if (!memory_.read(addresses,address_bytes.data(),count*8) ||
                !memory_.read(sizes,size_bytes.data(),count*4)) return error(0x1000);
            Job job;
            for(size_t i=0;i<count;++i) {
                const uint64_t address=ps4::get_le(address_bytes.data()+8*i,8);
                const uint64_t bytes=ps4::get_le(size_bytes.data()+4*i,4);
                if (!address || (address&3) || !bytes || (bytes&3) || bytes/4>word_limit-job.words.size()) return error(0x1000);
                std::vector<uint8_t> wire(size_t(bytes),0);
                if (!memory_.read(address,wire.data(),wire.size())) return error(0x1000);
                std::vector<uint32_t> words(wire.size()/4);
                for(size_t n=0;n<words.size();++n) words[n]=uint32_t(ps4::get_le(wire.data()+4*n,4));
                // Packet boundaries cannot straddle two separately submitted DCBs.
                if (!gpu::decodePM4Type3(words.data(),words.size())) return error(0x3013);
                job.words.insert(job.words.end(),words.begin(),words.end());
            }
            if (kind_>=null) {
                if (flip && !prepared_flip(job.words,*flip)) return error(0x1000);
                if (kind_==software) {
                    // Draw before the work's labels and interrupts, as the GPU does.
                    if (!renderer_) renderer_=std::make_unique<gpu::PM4SoftwareRenderer>(guest_);
                    renderer_->execute(job.words.data(),job.words.size());
                }
                if (kind_==host && host_consumer_(host_context_,job.words.data(),job.words.size())) {
                    failed_=true; return error(0x10ff);
                }
                const auto retired=retire(job.words);
                if (retired<0) return error(0x1000);
                if (interrupts) *interrupts=uint32_t(retired);
                ++next_; return 0;
            }
            const auto plan=gpu::planPM4Triangles(job.words.data(),job.words.size());
            if (plan.error!=gpu::PM4MetalError::none) return error(0x3fff);
            job.info={next_,uint32_t(job.words.size()),uint32_t(plan.drawCount)};
            jobs_.push_back(std::move(job)); ++next_; return 0;
        } catch(const std::bad_alloc&) { return error(0x1001); }
    }
    // A compute ring span (sceGnmDingDong): its label and memory writes
    // happen, its dispatches are not run yet, and nothing in it can fail the
    // graphics queue. The interrupts it raises, or -1.
    int64_t compute(uint64_t address, uint64_t bytes) {
        if (!address || (address&3) || !bytes || (bytes&3) || bytes/4>word_limit) return -1;
        try {
            std::vector<uint8_t> wire(size_t(bytes),0);
            if (!memory_.read(address,wire.data(),wire.size())) return -1;
            std::vector<uint32_t> words(wire.size()/4);
            for(size_t n=0;n<words.size();++n) words[n]=uint32_t(ps4::get_le(wire.data()+4*n,4));
            std::lock_guard<std::mutex> lock(lock_);
            return retire(words);
        } catch(const std::bad_alloc&) { return -1; }
    }
    int64_t done() {
        std::lock_guard<std::mutex> lock(lock_);
        if (!kind_ || failed_) return error(0x10ff);
        sealed_=!jobs_.empty(); return 0;
    }
    int32_t snapshot(maxps4_gnm_submission& info, uint32_t* words, size_t capacity) {
        info={}; std::lock_guard<std::mutex> lock(lock_);
        if (!words && capacity) return -EINVAL;
        if (jobs_.empty()) return -EAGAIN;
        const auto& job=jobs_.front();
        if (words) {
            if (capacity<job.words.size()) return -EINVAL;
            std::copy(job.words.begin(),job.words.end(),words);
        }
        info=job.info; return 0;
    }
    int32_t complete(uint64_t ticket, bool success) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!ticket || jobs_.empty() || jobs_.front().info.ticket!=ticket) return -EINVAL;
        jobs_.pop_front();
        if (!success) { clear(); failed_=true; }
        if (jobs_.empty()) sealed_=false;
        return 0;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(lock_); clear(); kind_=none;
        host_context_=nullptr; host_consumer_=nullptr;
    }
private:
    struct Job { maxps4_gnm_submission info{}; std::vector<uint32_t> words; };
    void clear() { jobs_.clear(); sealed_=failed_=false; renderer_.reset(); }
    // NOP payload types (shadPS4 PM4CmdNop::PayloadType): PrepareFlip,
    // PrepareFlipLabel, PrepareFlipInterrupt, PrepareFlipInterruptLabel.
    static bool prepared_flip(const std::vector<uint32_t>& words, PreparedFlip& flip) {
        if (words.size()<64) return false;
        const uint32_t* p=words.data()+words.size()-64;
        if (p[0]!=0xc03e1000U) return false;
        switch (p[1]) {
        case 0x68750777U: return true;
        case 0x68750778U: // 32-bit label value, dword-aligned address
            if (p[2]&3) return false;
            flip={true,false,p[2]|uint64_t(p[3])<<32,p[4]}; return true;
        case 0x68750780U: flip={false,true,0,0}; return true;
        case 0x68750781U: // EOP: 16 address high bits
            flip={true,true,(p[2]&~3U)|uint64_t(p[3]&0xffff)<<32,p[4]}; return true;
        default: return false;
        }
    }
    // Null backend: the memory writes of decoded (complete) packets, in order.
    // Returns the EOP interrupts requested, or -1 on an unwritable address.
    int64_t retire(const std::vector<uint32_t>& words) {
        const auto decoded=gpu::decodePM4Type3(words.data(),words.size());
        int64_t interrupts=0;
        auto store=[&](uint64_t address, uint64_t value, size_t bytes) {
            uint8_t wire[8]; ps4::put_le(wire,0,value,bytes);
            return memory_.write(address,wire,bytes);
        };
        // DATA_SEL: 1 = 32-bit data, 2 = 64-bit data, 3 = GPU clock counter.
        auto release=[&](uint64_t address, uint32_t data_sel, uint32_t int_sel, uint32_t lo, uint32_t hi) {
            if (data_sel==1 && !store(address,lo,4)) return false;
            if (data_sel==2 && !store(address,lo|uint64_t(hi)<<32,8)) return false;
            if (data_sel==3 && !store(address,uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()),8)) return false;
            if (int_sel) ++interrupts;
            return true;
        };
        for (const auto& packet : decoded.packets) {
            const uint32_t* p=words.data()+packet.payloadWord;
            const size_t n=packet.payloadCount;
            bool ok=true;
            if (packet.opcode==0x47 && n>=5) // EVENT_WRITE_EOP
                ok=release(p[1]|uint64_t(p[2]&0xffff)<<32,p[2]>>29,(p[2]>>24)&3,p[3],p[4]);
            else if (packet.opcode==0x49 && n>=6) // RELEASE_MEM
                ok=release(p[2]|uint64_t(p[3]&0xffff)<<32,p[1]>>29,(p[1]>>24)&3,p[4],p[5]);
            else if (packet.opcode==0x48 && n>=4 && (p[2]>>29)==2) // EVENT_WRITE_EOS, store data
                ok=store(p[1]|uint64_t(p[2]&0xffff)<<32,p[3],4);
            else if (packet.opcode==0x37 && n>=3) { // WRITE_DATA, DST_SEL memory
                const uint32_t dst=(p[0]>>8)&0xf;
                if (dst==1 || dst==2 || dst==5)
                    for (size_t i=3;ok && i<n;++i) ok=store((p[1]|uint64_t(p[2])<<32)+4*(i-3),p[i],4);
            }
            if (!ok) return -1;
        }
        return interrupts;
    }
    PS4GuestMemory& memory_;
    gpu::GCNMemory guest_;
    std::unique_ptr<gpu::PM4SoftwareRenderer> renderer_;
    std::mutex lock_;
    std::deque<Job> jobs_;
    uint64_t next_=1;
    uint32_t kind_=none;
    bool sealed_=false, failed_=false;
    void* host_context_=nullptr;
    HostConsumer host_consumer_=nullptr;
};
}
