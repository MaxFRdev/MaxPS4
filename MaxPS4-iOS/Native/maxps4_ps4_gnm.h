#pragma once
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_abi.h"
#include <array>
#include <cstring>
#include <initializer_list>

namespace maxps4 {
// Base-PS4 GNM packet generation. These calls WRITE commands; they do not
// submit, execute a shader, signal completion or present a frame.
class PS4Gnm final {
public:
    explicit PS4Gnm(PS4GuestMemory& memory) : memory_(memory) {}
    int64_t draw_auto(uint64_t output, uint32_t size, uint32_t count, uint32_t flags) {
        if (size != 7 || (flags & 0x1ffffffeU)) return -1;
        Packet p; p.add(0x2d, {count, 2}); p.nop(3); return store(output,p);
    }
    int64_t draw_index(uint64_t output, uint32_t size, uint32_t count, uint64_t indices, uint32_t flags) {
        if (size != 10 || !indices || (indices & 1) || (flags & 0x1ffffffeU)) return -1;
        Packet p; p.add(0x27, {count,uint32_t(indices),uint32_t(indices>>32),count,0});
        p.nop(3); return store(output,p);
    }
    int64_t vgt(uint64_t output, uint32_t size, uint32_t group, uint32_t partial, uint32_t eop) {
        if (size != 3 || group >= 256 || (partial | eop) >= 2) return -1;
        Packet p; p.add(0x69,{0x2aa,group | (partial << 16)}); return store(output,p);
    }
    int64_t vs(uint64_t output, uint32_t size, uint64_t registers, uint32_t modifier) {
        if (size < 29 || !registers || (modifier & 0xfcfffc3fU)) return -1;
        std::array<uint32_t,7> r{};
        if (!load(registers,r) || r[1]) return -1;
        Packet p;
        p.add(0x76,{0x48,r[0],0});
        p.add(0x76,{0x4a,modifier ? (r[2] & 0xfcfffc3fU) | modifier : r[2],r[3]});
        p.add(0x69,{0x207,r[6]}); p.add(0x69,{0x1b1,r[4]}); p.add(0x69,{0x1c3,r[5]});
        p.nop(11); return store(output,p);
    }
    int64_t ps350(uint64_t output, uint32_t size, uint64_t registers) {
        if (size < 40) return -1;
        std::array<uint32_t,12> r{};
        if (registers && (!load(registers,r) || r[1])) return -1;
        Packet p; p.add(0x76,{8,r[0],0});
        if (!registers) {
            p.add(0x69,{0x203,0}); p.add(0x69,{0x8f,0xf}); p.nop(29);
        } else {
            p.add(0x76,{10,r[2],r[3]}); p.add(0x69,{0x1c4,r[4],r[5]});
            p.add(0x69,{0x1b3,r[6],r[7]}); p.add(0x69,{0x1b6,r[8]});
            p.add(0x69,{0x1b8,r[9]}); p.add(0x69,{0x203,r[10]}); p.add(0x69,{0x8f,r[11]}); p.nop(11);
        }
        return store(output,p);
    }
    // sceGnmSetPsShader (and Update*): as 3.50 but without registers it only
    // clears DB_SHADER_CONTROL, not CB_SHADER_MASK.
    int64_t ps(uint64_t output, uint32_t size, uint64_t registers) {
        if (registers) return ps350(output, size, registers);
        if (size < 40) return -1;
        Packet p; p.add(0x76,{8,0,0}); p.add(0x69,{0x203,0}); p.nop(32); return store(output,p);
    }
    // DRAW_INDEX_OFFSET_2 (0x35): count, offset into the index buffer, count.
    int64_t draw_index_offset(uint64_t output, uint32_t size, uint32_t offset, uint32_t count, uint32_t flags) {
        if (size != 9) return -1;
        Packet p; p.add(0x35,{count,offset,count,0},flags & 1); p.nop(3); return store(output,p);
    }
    // DRAW_INDIRECT (0x24) / DRAW_INDEX_INDIRECT (0x25): arguments at
    // `data_offset` in the indirect buffer; the vertex/instance offsets go to
    // user SGPRs of the stage (VS 0x4c, ES 0xcc, LS 0x14c past the SH base).
    int64_t draw_indirect(uint64_t output, uint32_t size, uint32_t data_offset, uint32_t stage,
                          uint32_t vertex_sgpr, uint32_t instance_sgpr, uint32_t flags, bool indexed) {
        static constexpr uint32_t sgpr_base[] = {0, 0, 0x4c, 0, 0xcc, 0, 0x14c};
        if (size != 9 || stage > 6 || vertex_sgpr >= 16 || instance_sgpr >= 16) return -1;
        const uint32_t base = sgpr_base[stage];
        Packet p;
        p.add(indexed ? 0x25 : 0x24, {data_offset, vertex_sgpr ? vertex_sgpr + base : 0,
              instance_sgpr ? instance_sgpr + base : 0, indexed ? 0U : 2U}, flags & 1);
        p.nop(3); return store(output,p);
    }
    // DISPATCH_INDIRECT (0x16), compute.
    int64_t dispatch_indirect(uint64_t output, uint32_t size, uint32_t data_offset, uint32_t flags) {
        if (size != 7) return -1;
        Packet p; p.add(0x16,{data_offset,(flags & 0x18) + 1},compute | (flags & 1)); p.nop(3); return store(output,p);
    }
    // The compute queue's defaults: thread management of both shader engines,
    // resource limits, an ACQUIRE_MEM cache flush, then NOP to 256 dwords.
    int64_t dispatch_init_default_state(uint64_t output, uint32_t size) {
        if (size < words_limit) return 0;
        Packet p;
        p.add(0x76,{0x216,0xffffffffU},compute); p.add(0x76,{0x217,0xffffffffU},compute);
        p.add(0x76,{0x215,0x170},compute);
        p.add(0x58,{0x28000000U,0,0,0,0,10});
        p.nop(words_limit-p.used-1);
        return store(output,p) ? -1 : int64_t(words_limit);
    }
    // DrawInitToDefaultContextState (0x20 dwords) / 400 (0x100): the context
    // defaults are not written yet (like init_default_state350): only NOPs of
    // the size the caller expects, so the command buffer has no gap.
    int64_t init_context_state(uint64_t output, uint32_t size, uint32_t expected) {
        if (size != expected) return 0;
        Packet p; p.nop(expected-1);
        return store(output,p) ? -1 : int64_t(expected);
    }
    // IA_MULTI_VGT_PARAM back to its default.
    int64_t reset_vgt(uint64_t output, uint32_t size) {
        if (size != 3) return -1;
        Packet p; p.add(0x69,{0x2aa,0xff}); return store(output,p);
    }
    // A debug marker NOP (0x68750003), sized like push_marker's.
    int64_t set_marker(uint64_t output, uint32_t size, uint64_t marker) {
        const int64_t result = push_marker(output, size, marker);
        if (result) return result;
        uint8_t code[4]; ps4::put_le(code, 0, 0x68750003U, 4);
        return memory_.write(output + 4, code, 4) ? 0 : -1;
    }
    // The rest of the `size` dwords the game reserved, as one NOP: what a
    // writer leaves behind it would otherwise be undefined words.
    template <typename P> int64_t fill(uint64_t output, uint32_t size, P& p) {
        if (size > words_limit || size < p.used + 2) return -1;
        if (size > p.used) p.nop(size - p.used - 1);
        return store(output,p);
    }
    // Set/Update LS, HS, ES and GS shaders (shadPS4 gnmdriver.cpp): program
    // address, resources and the tessellation/geometry context registers.
    int64_t ls(uint64_t output, uint32_t size, uint64_t registers, uint32_t modifier) {
        if (size < 0x17 || !registers) return -1;
        const uint32_t mask = (modifier & 0xfffffc3fU) == 0 ? 0xfffffc3fU : 0xfcfffc3fU;
        std::array<uint32_t,4> r{};
        if ((modifier & mask) || !load(registers,r) || r[1]) return -1;
        Packet p;
        p.add(0x76,{0x148,r[0],0}); p.add(0x76,{0x14b,r[3]});
        p.add(0x76,{0x14a,modifier ? (r[2] & mask) | modifier : r[2],r[3]});
        return fill(output,size,p);
    }
    int64_t hs(uint64_t output, uint32_t size, uint64_t registers, uint32_t config, bool update) {
        if (size < (update ? 0x1dU : 0x1eU) || !registers) return -1;
        std::array<uint32_t,7> r{};
        if (!load(registers,r) || r[1]) return -1;
        Packet p;
        p.add(0x76,{0x108,r[0],0}); p.add(0x76,{0x10a,r[2],r[3]});
        p.add(0x69,{0x286,r[5],r[6]}); p.add(0x69,{0x2db,r[4]}); p.add(0x69,{0x2d6,config});
        return fill(output,size,p);
    }
    int64_t es(uint64_t output, uint32_t size, uint64_t registers, uint32_t modifier) {
        if (size < 0x14 || !registers || (modifier & 0xfcfffc3fU)) return -1;
        std::array<uint32_t,4> r{};
        if (!load(registers,r) || r[1]) return -1;
        Packet p;
        p.add(0x76,{0xc8,r[0],0}); p.add(0x76,{0xca,modifier ? (r[2] & 0xfcfffc3fU) | modifier : r[2],r[3]});
        return fill(output,size,p);
    }
    int64_t gs(uint64_t output, uint32_t size, uint64_t registers) {
        if (size < 0x1d || !registers) return -1;
        std::array<uint32_t,7> r{};
        if (!load(registers,r) || r[1]) return -1;
        Packet p;
        p.add(0x76,{0x88,r[0],0}); p.add(0x76,{0x8a,r[2],r[3]});
        p.add(0x69,{0x2e5,r[4]}); p.add(0x69,{0x29b,r[5]}); p.add(0x69,{0x2e4,r[6]});
        return fill(output,size,p);
    }
    // Debug markers and the embedded pixel shader (whose code is not in guest
    // memory yet): NOPs of the reserved size, so nothing undefined is left.
    int64_t push_color_marker(uint64_t output, uint32_t size, uint64_t marker) {
        std::array<char,512> text{};
        if (!marker || !memory_.read_cstring(marker,text.data(),text.size())) return -1;
        const size_t length=std::strlen(text.data());
        if (((length+0xc)>>2) + ((length+0x10)>>3)*2 + 2 != size) return -1;
        Packet p; return fill(output,size,p);
    }
    int64_t nop_fill(uint64_t output, uint32_t size, uint32_t expected) {
        if (expected && size != expected) return -1;
        Packet p; return fill(output,size,p);
    }
    // ComputeWaitOnAddress(cmdbuf, 14, address, mask, function, reference):
    // WAIT_REG_MEM on memory (a register below 0x10000).
    int64_t compute_wait_address(uint64_t output, uint32_t size, uint64_t address, uint32_t mask,
                                 uint32_t function, uint32_t reference) {
        if (size != 0xe) return -1;
        const bool in_memory = address > 0xffff;
        Packet p;
        p.add(0x3c,{(in_memory ? 0x10U : 0U) | (function & 7),uint32_t(address & (in_memory ? 0xfffffffcU : 0xffffU)),
                    uint32_t(address >> 32),reference,mask,10});
        return fill(output,size,p);
    }
    // ComputeWaitSemaphore(cmdbuf, 10, semaphore, flags): MEM_SEMAPHORE wait.
    int64_t compute_wait_semaphore(uint64_t output, uint32_t size, uint64_t semaphore, uint32_t flags) {
        if (size != 0xa) return -1;
        Packet p;
        p.add(0x39,{uint32_t(semaphore),(flags == 1 ? 0x100000U : 0U) - 0x20000000U + uint32_t(semaphore >> 32)});
        return fill(output,size,p);
    }
    // DispatchIndirectOnMec(cmdbuf, 8, arguments, modifier): DISPATCH_INDIRECT
    // with a 64-bit argument address, on the compute engine.
    int64_t dispatch_indirect_mec(uint64_t output, uint32_t size, uint64_t arguments, uint32_t modifier) {
        if (size != 8 || !arguments || (arguments & 3)) return -1;
        Packet p;
        p.add(0x16,{uint32_t(arguments),uint32_t(arguments >> 32),(modifier & 0x18) | 1},compute | (modifier & 1));
        return fill(output,size,p);
    }
    int64_t cs(uint64_t output, uint32_t size, uint64_t registers, uint32_t modifier) {
        if (size < 25 || !registers || (modifier & 0xfffffc3fU)) return -1;
        std::array<uint32_t,7> r{};
        if (!load(registers,r) || r[1]) return -1;
        Packet p;
        p.add(0x76,{0x20c,r[0],0},compute);
        p.add(0x76,{0x212,modifier ? (r[2] & 0xfffffc3fU) | modifier : r[2],r[3]},compute);
        p.add(0x76,{0x207,r[4],r[5],r[6]},compute); p.nop(11); return store(output,p);
    }
    int64_t dispatch(uint64_t output, uint32_t size, uint32_t x, uint32_t y, uint32_t z, uint32_t flags) {
        if (size != 9 || int32_t(x | y | z) < 0) return -1;
        Packet p; p.add(0x15,{x,y,z,(flags & 0x18) + 1},compute | (flags & 1)); p.nop(3); return store(output,p);
    }
    // The embedded full-screen VS (id 0) sets the registers of shadPS4's copy
    // with a null program address: its code is not placed in guest memory yet,
    // so a drawing backend must recognize it from these registers.
    int64_t embedded_vs(uint64_t output, uint32_t size, uint32_t id) {
        if (id) return ps4::signed32(0x8eee00ffU);
        if (size < 29) return -1;
        Packet p;
        p.add(0x76,{0x48,0,0}); p.add(0x76,{0x4a,0xc0000,4});
        p.add(0x69,{0x207,0}); p.add(0x69,{0x1b1,0}); p.add(0x69,{0x1c3,4});
        p.nop(11); return store(output,p);
    }
    // No flip is ever pending once the next frame's commands are built (the
    // display completes flips on its own), so the wait is a same-size NOP.
    int64_t wait_flip_done(uint64_t output, uint32_t size) {
        if (size != 7) return -1;
        Packet p; p.nop(6); return store(output,p);
    }
    int64_t push_marker(uint64_t output, uint32_t size, uint64_t marker) {
        std::array<char,512> text{};
        if (!marker || !memory_.read_cstring(marker,text.data(),text.size())) return -1;
        const size_t length=std::strlen(text.data());
        const size_t count=((length+8)>>2) + ((length+0xc)>>3)*2;
        if (count+2 != size || size > words_limit) return -1;
        Packet p; p.nop(count+1); p.words[1]=0x68750001U;
        for (size_t i=0;i<=length;++i) p.words[2+i/4] |= uint32_t(uint8_t(text[i])) << (8*(i%4));
        return store(output,p);
    }
    int64_t pop_marker(uint64_t output, uint32_t size) {
        if (size != 6) return -1;
        Packet p; p.nop(5); p.words[1]=0x68750002U; return store(output,p);
    }
    // Only the context clear (CONTEXT_CONTROL, CLEAR_STATE, EOS wait) of the
    // 3.50 default state: its register defaults are not written yet.
    int64_t init_default_state350(uint64_t output, uint32_t size) {
        if (size < words_limit) return 0;
        Packet p;
        for (auto value : {0xc0012800U,0x80000000U,0x80000000U,0xc0001200U,0U,0xc0055800U,
                           0x2ec47fc0U,0xffffffffU,0U,0U,0U,10U}) p.words[p.used++]=value;
        p.nop(words_limit-p.used-1);
        return store(output,p) ? -1 : int64_t(words_limit);
    }
private:
    static constexpr uint32_t compute=2;
    // Largest implemented packet sequence (the default state) is 256 dwords,
    // irrespective of the caller's advertised capacity. Build completely
    // before touching output.
    static constexpr size_t words_limit=256;
    struct Packet {
        std::array<uint32_t,words_limit> words{};
        size_t used=0;
        // `flags`: shader type (bit 1, compute) and predicate (bit 0).
        void add(uint32_t opcode, std::initializer_list<uint32_t> data, uint32_t flags=0) {
            words[used++]=0xc0000000U | (uint32_t(data.size()-1)<<16) | (opcode<<8) | flags;
            for (auto value:data) words[used++]=value;
        }
        void nop(size_t payload) {
            words[used++]=0xc0001000U | (uint32_t(payload-1)<<16);
            used+=payload;
        }
    };
    template<size_t N> bool load(uint64_t address, std::array<uint32_t,N>& values) {
        std::array<uint8_t,N*4> bytes{};
        if ((address & 3) || !memory_.read(address,bytes.data(),bytes.size())) return false;
        for (size_t i=0;i<N;++i) values[i]=uint32_t(ps4::get_le(bytes.data()+4*i,4));
        return true;
    }
    int64_t store(uint64_t address, const Packet& packet) {
        if (!address || (address & 3)) return -1;
        std::array<uint8_t,words_limit*4> wire{};
        for(size_t i=0;i<packet.used;++i) ps4::put_le(wire.data(),4*i,packet.words[i],4);
        return memory_.write(address,wire.data(),packet.used*4) ? 0 : -1;
    }
    PS4GuestMemory& memory_;
};
}
