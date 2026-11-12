#pragma once
#include "OrbisLinkerFixture.h"
#include "../Native/OrbisLinker.hpp"
namespace orbis_execution {
constexpr uint64_t app_bias=0x100000, provider_bias=0x200000;
constexpr uint64_t entry=app_bias+0x4000, data=app_bias+0x8000;
inline std::vector<uint8_t> make(bool provider, uint32_t value=40) {
    auto b=orbis_fixture::make(provider);
    using orbis_fixture::put;
    put(b,24,provider ? 0 : 0x4000);
    put(b,64+16,0x4000); put(b,64+56+16,0x8000);
    put(b,64+48,1); put(b,64+56+48,1);
    if (provider) {
        const uint8_t code[]={0xb8,0,0,0,0,0xc3};
        std::copy(std::begin(code),std::end(code),b.begin()+0x200);
        put(b,0x201,value,4); put(b,0x820,0x4000); put(b,0x828,6);
        put(b,64+32,6); put(b,64+40,6);
    } else {
        const uint8_t code[]={0xff,0x15,0,0,0,0,0x05,2,0,0,0,0xc7,5,0,0,0,0,0x34,0x12,0,0,0xf4};
        std::copy(std::begin(code),std::end(code),b.begin()+0x200);
        put(b,0x202,0x8000-(0x4000+6),4);
        put(b,0x20d,0x8008-(0x4000+21),4);
        put(b,0x900,0x8000);
        put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
        put(b,64+56+40,0x200); // BSS after relocation slot.
    }
    return b;
}
// App with a PT_TLS block (4 template bytes = `initial`, 8 bytes per thread,
// 4-byte aligned): MOV EAX,FS:[disp]; ADD EAX,2; HLT. Its only TLS block sits
// at TP-8, so disp=-8 reads the variable and the program returns initial+2.
constexpr uint64_t tls_template=app_bias+0x8080;
inline std::vector<uint8_t> make_tls_app(int32_t disp=-8, uint32_t initial=0x1234) {
    auto b=make(false);
    using orbis_fixture::put;
    const uint8_t code[]={0x64,0x8b,0x04,0x25,0,0,0,0,0x05,2,0,0,0,0xf4};
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x204,uint32_t(disp),4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    put(b,56,5,2);
    const size_t p=64+4*56;
    put(b,p,7,4); put(b,p+4,4,4); put(b,p+8,0x380); put(b,p+16,0x8080);
    put(b,p+32,4); put(b,p+40,8); put(b,p+48,4);
    put(b,0x380,initial,4);
    return b;
}
// Same module, compiler-style access: MOV RAX,FS:[0]; MOV EAX,[RAX+disp]
// (disp8 or disp32 encoding); ADD EAX,2; HLT. `gap` inserts a NOP between the
// two loads, which must invalidate RAX as the thread pointer.
inline std::vector<uint8_t> make_tls_register_app(int32_t disp=-8, bool disp32=false,
                                                  uint32_t initial=0x1234, bool gap=false) {
    auto b=make_tls_app(-8,initial);
    std::vector<uint8_t> code={0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
    if (gap) code.push_back(0x90);
    if (disp32) { code.insert(code.end(),{0x8b,0x80}); for (int i=0;i<4;++i) code.push_back(uint8_t(uint32_t(disp)>>(8*i))); }
    else code.insert(code.end(),{0x8b,0x40,uint8_t(int8_t(disp))});
    code.insert(code.end(),{0x05,2,0,0,0,0xf4});
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(code.begin(),code.end(),b.begin()+0x200);
    orbis_fixture::put(b,64+32,code.size()); orbis_fixture::put(b,64+40,code.size());
    return b;
}
// Initial-exec model: a GOT slot at 0xC000 receives TPOFF64 (sym 0, A=0 -> -8)
// from the linker; MOV RAX,[RIP+slot]; MOV EAX,FS:[RAX]; ADD EAX,2; HLT.
// With `relro`, the slot lives in a PT_SCE_RELRO segment that becomes read-only
// after linking; otherwise it is an ordinary RW PT_LOAD and must be refused.
constexpr uint64_t tls_got=app_bias+0xc000; // Own 16 KiB page (iOS/macOS arm64).

inline std::vector<uint8_t> make_tls_initial_exec_app(bool relro=true, uint32_t initial=0x1234, bool gap=false) {
    auto b=make_tls_app(-8,initial);
    using orbis_fixture::put;
    std::vector<uint8_t> code={0x48,0x8b,0x05,0,0,0,0};
    if (gap) code.push_back(0x90);
    code.insert(code.end(),{0x64,0x8b,0x00,0x05,2,0,0,0,0xf4});
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(code.begin(),code.end(),b.begin()+0x200);
    put(b,0x203,0xc000-(0x4000+7),4);
    put(b,64+32,code.size()); put(b,64+40,code.size());
    put(b,56,6,2);
    const size_t p=64+5*56;
    put(b,p,relro ? 0x61000010 : 1,4); put(b,p+4,6,4); put(b,p+8,0x3c0); put(b,p+16,0xc000);
    put(b,p+32,8); put(b,p+40,8); put(b,p+48,1);
    put(b,0x918,0xc000); put(b,0x920,18); put(b,0x928,0); // TPOFF64, symbol 0, A=0.
    orbis_fixture::setTag(b,0x61000031,2*24);
    return b;
}
// The app alone, importing scePthreadSelf (public NID aI+OeCz8xrQ) from the
// system libkernel instead of the fixture PRX: CALL [GOT]; ADD EAX,2; store; HLT.
// Thread 1 is the only PS4 thread, so a real HLE call returns 1 + 2 = 3.
inline void use_libkernel(std::vector<uint8_t>& b, const char* nid);
inline std::vector<uint8_t> make_kernel_app(const char* nid="aI+OeCz8xrQ") {
    auto b=make(false);
    use_libkernel(b,nid);
    return b;
}
// Rewrites the fixture's single import to come from the system libkernel.
inline void use_libkernel(std::vector<uint8_t>& b, const char* nid) {
    using orbis_fixture::put;
    std::fill(b.begin()+0x601,b.begin()+0x640,0);
    char strings[]="libkernel\0libkernel\0aI+OeCz8xrQ#B#B";
    std::copy(nid,nid+11,strings+20);
    std::copy(std::begin(strings),std::end(strings),b.begin()+0x601);
    orbis_fixture::setTag(b,0x61000037,1+sizeof(strings));
    orbis_fixture::setTag(b,0x61000015,1|(uint64_t{1}<<32)|(uint64_t{1}<<48));
    orbis_fixture::setTag(b,0x6100000f,11|(uint64_t{1}<<32)|(uint64_t{1}<<48));
    put(b,0x818,21,4);
}
// General-dynamic TLS: tls_index {module, offset} at 0x8020 filled by DTPMOD64
// and DTPOFF64 (symbol 0, A = `offset`); LEA RDI,[RIP+tls_index];
// CALL [GOT] -> __tls_get_addr (NID vNe1w4diLCs); MOV EAX,[RAX]; ADD EAX,2; HLT.
constexpr uint64_t tls_index_address=data+0x20;
inline std::vector<uint8_t> make_tls_get_addr_app(uint32_t initial=0x4242, uint64_t offset=0) {
    auto b=make_tls_app(-8,initial);
    use_libkernel(b,"vNe1w4diLCs");
    using orbis_fixture::put;
    const uint8_t code[]={0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0, 0x8b,0x00, 0x05,2,0,0,0, 0xf4};
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x203,0x8020-(0x4000+7),4);
    put(b,0x209,0x8000-(0x4000+13),4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    put(b,0x918,0x8020); put(b,0x920,16); put(b,0x928,0);      // DTPMOD64 -> module ID
    put(b,0x930,0x8028); put(b,0x938,17); put(b,0x940,offset); // DTPOFF64 -> offset
    orbis_fixture::setTag(b,0x61000031,3*24);
    return b;
}
// An executable with DT_INIT (0x400C, stores 40 at 0x8010) and a DT_INIT_ARRAY
// entry (0x8030, filled by RELATIVE, adds 2). The engine validates these but
// never runs them: an executable's own startup code does, from its entry.
inline std::vector<uint8_t> make_init_app() {
    auto b=make_kernel_app();
    using orbis_fixture::put;
    const uint8_t code[]={
        0x8b,0x05,0,0,0,0, 0x05,2,0,0,0, 0xf4,                       // entry  0x4000
        0xb8,40,0,0,0, 0x89,0x05,0,0,0,0, 0xc3,                      // init_1 0x400C
        0x8b,0x05,0,0,0,0, 0x05,2,0,0,0, 0x89,0x05,0,0,0,0, 0xc3};   // init_2 0x4018
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x202,0x8010-0x4006,4);
    put(b,0x213,0x8010-0x4017,4);
    put(b,0x21a,0x8010-0x401e,4);
    put(b,0x225,0x8010-0x4029,4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    orbis_fixture::appendTag(b,12,0x400c);
    orbis_fixture::appendTag(b,25,0x8030);
    orbis_fixture::appendTag(b,27,8);
    put(b,0x918,0x8030); put(b,0x920,8); put(b,0x928,0x4018); // RELATIVE -> init_2
    orbis_fixture::setTag(b,0x61000031,2*24);
    return b;
}
// module_start: provider.prx DT_INIT (0x4006) stores 40 at its data 0x8010; its
// DT_INIT_ARRAY entry (0x4012) would add 1000. app.elf DT_INIT (0x400C) would
// store 99 there; app entry loads it and adds 2. Only the PRX DT_INIT may run
// (module_start(0, NULL, NULL)), so the result must be exactly 42.
constexpr uint64_t provider_value=provider_bias+0x8010;
inline std::vector<uint8_t> make_start_provider() {
    auto b=make(true);
    using orbis_fixture::put;
    const uint8_t code[]={
        0xb8,40,0,0,0, 0x89,0x05,0,0,0,0, 0xc3,                          // DT_INIT 0x4006
        0x8b,0x05,0,0,0,0, 0x05,0xe8,0x03,0,0, 0x89,0x05,0,0,0,0, 0xc3}; // array  0x4012
    std::copy(std::begin(code),std::end(code),b.begin()+0x206);
    put(b,0x20d,0x8010-0x4011,4);
    put(b,0x214,0x8010-0x4018,4);
    put(b,0x21f,0x8010-0x4023,4);
    put(b,64+32,6+sizeof(code)); put(b,64+40,6+sizeof(code));
    orbis_fixture::appendTag(b,12,0x4006);
    orbis_fixture::appendTag(b,25,0x8030);
    orbis_fixture::appendTag(b,27,8);
    orbis_fixture::appendTag(b,0x6100002f,0x300);
    orbis_fixture::appendTag(b,0x61000031,24);
    orbis_fixture::appendTag(b,0x61000033,24);
    put(b,0x900,0x8030); put(b,0x908,8); put(b,0x910,0x4012); // RELATIVE -> array function
    return b;
}
inline std::vector<uint8_t> make_start_app() {
    auto b=make(false);
    using orbis_fixture::put;
    const uint8_t code[]={
        0x8b,0x05,0,0,0,0, 0x05,2,0,0,0, 0xf4,      // entry   0x4000
        0xb8,99,0,0,0, 0x89,0x05,0,0,0,0, 0xc3};    // DT_INIT 0x400C (must not run)
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x202,uint32_t(provider_value-(app_bias+0x4006)),4);
    put(b,0x213,uint32_t(provider_value-(app_bias+0x4017)),4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    orbis_fixture::appendTag(b,12,0x400c);
    return b;
}
// sceKernelClockGettime (public NID QBi7HCK03hw) with a pointer argument:
// MOV EDI,clock; LEA RSI,[RIP+timespec]; CALL [GOT]; ADD EAX,2; HLT.
// The 16-byte timespec lands in app data at 0x8010; success returns 0 + 2.
constexpr uint64_t timespec_address=data+0x10;
inline std::vector<uint8_t> make_clock_app(uint32_t clock=4, int32_t lea_target=0x8010) {
    auto b=make_kernel_app("QBi7HCK03hw");
    using orbis_fixture::put;
    const uint8_t code[]={0xbf,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0, 0xff,0x15,0,0,0,0, 0x05,2,0,0,0, 0xf4};
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x201,clock,4);
    put(b,0x208,uint32_t(lea_target-(0x4000+12)),4);
    put(b,0x20e,0x8000-(0x4000+18),4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// General execution (identity mapping, no instruction list). The bounded
// admission refuses this code (JNZ, PUSH/POP, local CALL, absolute loads):
//   EDX = 1+..+10 (loop); EAX = twice(EDX) via CALL rel32 with a frame;
//   EDX += *[slot], slot filled by RELATIVE with the address of `cell`;
//   PUSH RDX; CALL [GOT] -> provider.prx (unpatched); POP RDX; EAX += EDX;
//   store EAX in BSS (0x8100); HLT. Result: 110 + cell + provider value.
constexpr uint64_t general_slot=data+0x10, general_cell=data+0x20, general_store=data+0x100;
inline std::vector<uint8_t> make_general_app(uint32_t cell=7) {
    auto b=make(false);
    using orbis_fixture::put;
    const uint8_t code[]={
        0x31,0xd2, 0xb9,10,0,0,0,                 // 0x00 XOR EDX,EDX; MOV ECX,10
        0x01,0xca, 0xff,0xc9, 0x75,0xfa,          // 0x07 loop: ADD EDX,ECX; DEC ECX; JNZ loop
        0x89,0xd7, 0xe8,0x1c,0,0,0,               // 0x0d MOV EDI,EDX; CALL twice (0x30)
        0x89,0xc2, 0x48,0x8b,0x05,0,0,0,0,        // 0x14 MOV EDX,EAX; MOV RAX,[RIP+slot]
        0x03,0x10, 0x52, 0xff,0x15,0,0,0,0,       // 0x1d ADD EDX,[RAX]; PUSH RDX; CALL [GOT]
        0x5a, 0x01,0xd0, 0x89,0x05,0,0,0,0, 0xf4, // 0x26 POP RDX; ADD EAX,EDX; MOV [store],EAX; HLT
        0x55, 0x48,0x89,0xe5, 0x89,0xf8,          // 0x30 twice: PUSH RBP; MOV RBP,RSP; MOV EAX,EDI
        0x01,0xc0, 0x5d, 0xc3};                   // 0x36 ADD EAX,EAX; POP RBP; RET
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x219,0x8010-0x401d,4);
    put(b,0x222,0x8000-0x4026,4);
    put(b,0x22b,0x8100-0x402f,4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    put(b,0x320,cell,4);
    put(b,0x918,0x8010); put(b,0x920,8); put(b,0x928,0x8020); // RELATIVE -> cell
    orbis_fixture::setTag(b,0x61000031,2*24);
    return b;
}
// General-mode guest faults (same provider dependency as make()):
//   0: MOV EAX,[RSP+0x1000] - past the stack top, in its guard page (SIGSEGV)
//   1: MOV DWORD [RIP-10],0 - a store into its own read-only code (SIGSEGV)
//   2: UD2                  - invalid opcode (SIGILL)
//   3: JMP $                - endless loop (needs a time limit)
//   4: LOCK ADD [RIP-8],1    - unaligned atomic write into read-only code
// Each is followed by HLT, which must never be reached.
inline std::vector<uint8_t> make_general_fault_app(int kind) {
    auto b=make(false);
    using orbis_fixture::put;
    std::vector<uint8_t> code;
    if (kind == 0) code={0x8b,0x84,0x24,0x00,0x10,0,0, 0xf4};
    else if (kind == 1) code={0xc7,0x05,0xf6,0xff,0xff,0xff,0,0,0,0, 0xf4};
    else if (kind == 2) code={0x0f,0x0b, 0xf4};
    else if (kind == 3) code={0xeb,0xfe, 0xf4};
    else if (kind == 4) code={0xf0,0x48,0x83,0x05,0xf8,0xff,0xff,0xff,1,0xf4};
    // #296: FEX decodes `66 cf` (IRET with an operand-size override) but its
    // handler reports a decode failure, so CompileBlock returns 0 for a block
    // that starts there. 5: reached by a direct CALL (ExitFunctionLink, target
    // code+6); 6: by a direct JMP (target code+3); 7: at the entry itself.
    else if (kind == 5) code={0xe8,0x01,0,0,0, 0xf4, 0x66,0xcf};
    else if (kind == 6) code={0xeb,0x01, 0xf4, 0x66,0xcf};
    else code={0x66,0xcf, 0xf4};
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(code.begin(),code.end(),b.begin()+0x200);
    put(b,64+32,code.size()); put(b,64+40,code.size());
    return b;
}
// Eight-byte accesses at offset 63 of a cache line. x86 allows both ordinary
// accesses and LOCKed operations here; ARM64 TSO/atomic instructions may trap.
constexpr uint64_t unaligned_cell=data+0x13f, unaligned_store=data+0x17f;
inline std::vector<uint8_t> make_general_unaligned_app() {
    auto b=make(false);
    std::vector<uint8_t> code;
    auto emit=[&](std::initializer_list<uint8_t> op) { code.insert(code.end(),op); };
    auto imm=[&](uint64_t n,size_t width) { for(size_t k=0;k<width;++k) code.push_back(uint8_t(n>>(8*k))); };
    auto rip=[&](std::initializer_list<uint8_t> op,uint64_t target,size_t trailing=0) {
        emit(op); imm(target-(entry+code.size()+4+trailing),4);
    };
    rip({0x48,0xc7,0x05},unaligned_cell,4); imm(0x11223344,4); // MOV qword [cell],imm32
    rip({0x48,0x8b,0x05},unaligned_cell);                  // MOV RAX,[cell]
    emit({0x48,0x83,0xc0,1});                             // ADD RAX,1
    rip({0xf0,0x48,0x83,0x05},unaligned_cell,1); imm(1,1); // LOCK ADD qword [cell],1
    emit({0xba}); imm(0x55667788,4);                      // MOV EDX,replacement
    rip({0xf0,0x48,0x0f,0xb1,0x15},unaligned_cell);       // LOCK CMPXCHG [cell],RDX
    rip({0x48,0x8b,0x05},unaligned_cell);                  // MOV RAX,[cell]
    emit({0x48,0x83,0xc0,1});
    rip({0x48,0x89,0x05},unaligned_store);                 // MOV [store],RAX
    rip({0x48,0x8b,0x05},unaligned_store); emit({0xf4});
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(code.begin(),code.end(),b.begin()+0x200);
    orbis_fixture::put(b,64+32,code.size()); orbis_fixture::put(b,64+40,code.size());
    return b;
}
// sceKernelMmap (public NID PGhQHd-dzv8) from general code, seventh argument
// on the stack: mmap(0, 0x4000, READ|WRITE, ANON|PRIVATE, -1, 0, &out) with
// `out` at app data 0x8010. On success the guest stores 0x1234 at out+0x100
// through the returned pointer, loads it back and returns 0x1236; on failure
// it returns the libkernel error.
constexpr uint64_t mmap_out=data+0x10;
inline std::vector<uint8_t> make_general_mmap_app() {
    auto b=make_kernel_app("PGhQHd-dzv8");
    using orbis_fixture::put;
    const uint8_t code[]={
        0x31,0xff, 0xbe,0,0x40,0,0, 0xba,3,0,0,0,            // 0x00 XOR EDI,EDI; MOV ESI,0x4000; MOV EDX,3
        0xb9,0x02,0x10,0,0, 0x41,0xb8,0xff,0xff,0xff,0xff,  // 0x0c MOV ECX,0x1002; MOV R8D,-1
        0x45,0x31,0xc9, 0x48,0x8d,0x05,0,0,0,0, 0x50,        // 0x17 XOR R9D,R9D; LEA RAX,[out]; PUSH RAX
        0xff,0x15,0,0,0,0, 0x59, 0x85,0xc0, 0x75,0x1c,       // 0x22 CALL [GOT]; POP RCX; TEST EAX,EAX; JNZ end
        0x48,0x8b,0x05,0,0,0,0,                              // 0x2d MOV RAX,[out]
        0xc7,0x80,0,1,0,0,0x34,0x12,0,0,                     // 0x34 MOV DWORD [RAX+0x100],0x1234
        0x8b,0x80,0,1,0,0, 0x05,2,0,0,0, 0xf4};              // 0x3e MOV EAX,[RAX+0x100]; ADD EAX,2; end: HLT
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x21d,0x8010-0x4021,4);
    put(b,0x224,0x8000-0x4028,4);
    put(b,0x230,0x8010-0x4034,4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// PS4 process entry (general mode): checks RDI -> EntryParams {argc 1,
// argv[0] "eboot.bin"}, its first 8 bytes copied at [RSP], RSP = 8 (mod 16),
// then calls the exit function in RSI with argc+41 = 42 (7 if a check fails).
inline std::vector<uint8_t> make_general_entry_app() {
    auto b=make_kernel_app();
    using orbis_fixture::put;
    const uint8_t code[]={
        0x8b,0x07, 0x48,0x8b,0x57,0x08, 0x48,0x8b,0x12,            // 0x00 EAX=argc; RDX=*argv[0]
        0x48,0xb9,'e','b','o','o','t','.','b','i',                 // 0x09 MOV RCX,"eboot.bi"
        0x48,0x39,0xca, 0x75,0x1a,                                 // 0x13 CMP RDX,RCX; JNZ bad
        0x48,0x8b,0x0c,0x24, 0x48,0x3b,0x0f, 0x75,0x11,            // 0x18 CMP [RSP],[RDI]; JNZ bad
        0x48,0x89,0xe1, 0x83,0xe1,0x0f, 0x83,0xf9,0x08, 0x75,0x06, // 0x21 RSP%16==8 or bad
        0x8d,0x78,0x29, 0xff,0xd6, 0xf4,                           // 0x2c EDI=argc+41; CALL RSI; HLT
        0xbf,7,0,0,0, 0xff,0xd6, 0xf4};                            // 0x32 bad: EDI=7; CALL RSI; HLT
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// Several system libkernel imports: symbol i+1 is `nids[i]`, bound by a
// JUMP_SLOT to GOT slot data+8*i.
inline void use_libkernel_imports(std::vector<uint8_t>& b, std::initializer_list<const char*> nids) {
    using orbis_fixture::put;
    use_libkernel(b,*nids.begin());
    std::fill(b.begin()+0x615,b.begin()+0x700,0);
    std::fill(b.begin()+0x818,b.begin()+0x900,0);
    std::fill(b.begin()+0x900,b.begin()+0xa00,0);
    size_t i=0;
    for (const char* nid : nids) {
        const size_t name=21+16*i;
        std::copy(nid,nid+11,b.begin()+0x600+name);
        std::copy_n("#B#B",4,b.begin()+0x600+name+11);
        put(b,0x818+24*i,name,4); b[0x81c+24*i]=0x12;
        put(b,0x900+24*i,0x8000+8*i); put(b,0x908+24*i,((i+1)<<32)|7);
        ++i;
    }
    orbis_fixture::setTag(b,0x61000037,21+16*i);
    orbis_fixture::setTag(b,0x6100003f,24*(i+1));
    orbis_fixture::setTag(b,0x61000031,24*i);
}
// Two guest threads (general mode). The entry creates thread A (entry A, arg
// &40) and thread B (entry B, arg 5) with scePthreadCreate, joins both with
// scePthreadJoin and exits with A's value + B's value = 100 (or the failing
// call's error). A sets flag 0x8068 then spins until B sets 0x806c, and B
// waits for A first: only truly concurrent threads finish. A also checks its
// scePthreadSelf is not the main thread's (1) and that RSP = 8 (mod 16), then
// returns [arg]+2 = 42 (7 on failure); B ends with scePthreadExit(arg+53 = 58).
constexpr uint64_t threads_values=data+0x50;
inline std::vector<uint8_t> make_general_threads_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","onNY9Byn-W8","3kg7rT0NQIs","aI+OeCz8xrQ"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, join=data+8, quit=data+0x10, self=data+0x18;
    constexpr uint64_t handleA=data+0x40, handleB=data+0x48, valueA=data+0x50, valueB=data+0x58;
    constexpr uint64_t arg=data+0x60, flagA=data+0x68, flagB=data+0x6c;
    constexpr size_t A=0x8e, B=0xc4;
    const uint8_t code[]={
        0x53, 0x48,0x89,0xf3,                                  // 0x00 PUSH RBX; MOV RBX,RSI (exit)
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x04 LEA RDI,[handleA]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0x48,0x8d,0x0d,0,0,0,0,        // 0x0d LEA RDX,[A]; LEA RCX,[arg]
        0x45,0x31,0xc0, 0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x61, // 0x1b XOR R8D,R8D; CALL [create]; JNZ fail
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x28 LEA RDI,[handleB]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0xb9,5,0,0,0,                  // 0x31 LEA RDX,[B]; MOV ECX,5
        0x45,0x31,0xc0, 0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x3f, // 0x3d XOR R8D,R8D; CALL [create]; JNZ fail
        0x48,0x8b,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x4a MOV RDI,[handleA]; LEA RSI,[valueA]
        0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x27,                // 0x58 CALL [join]; JNZ fail
        0x48,0x8b,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x62 MOV RDI,[handleB]; LEA RSI,[valueB]
        0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x0f,                // 0x70 CALL [join]; JNZ fail
        0x8b,0x3d,0,0,0,0, 0x03,0x3d,0,0,0,0,                  // 0x7a EDI=[valueA]+[valueB]
        0xff,0xd3, 0xf4,                                       // 0x86 CALL RBX; HLT
        0x89,0xc7, 0xff,0xd3, 0xf4,                            // 0x89 fail: EDI=EAX; CALL RBX; HLT
        // A (0x8e)
        0x8b,0x1f, 0xc7,0x05,0,0,0,0,1,0,0,0,                  // +00 EBX=[arg]; [flagA]=1
        0x83,0x3d,0,0,0,0,0, 0x74,0xf7,                        // +0c spin while [flagB]==0
        0xff,0x15,0,0,0,0, 0x48,0x83,0xf8,0x01, 0x74,0x0f,     // +15 CALL [self]; CMP RAX,1; JE bad
        0x48,0x89,0xe1, 0x83,0xe1,0x0f, 0x83,0xf9,0x08, 0x75,0x04, // +21 RSP%16==8 or bad
        0x8d,0x43,0x02, 0xc3,                                  // +2c EAX=EBX+2; RET
        0xb8,7,0,0,0, 0xc3,                                    // +30 bad: EAX=7; RET
        // B (0xc4)
        0x83,0x3d,0,0,0,0,0, 0x74,0xf7,                        // +00 spin while [flagA]==0
        0xc7,0x05,0,0,0,0,1,0,0,0,                             // +09 [flagB]=1
        0x8d,0x7f,0x35, 0xff,0x15,0,0,0,0,                     // +13 EDI=arg+53; CALL [exit]
        0xb8,9,0,0,0, 0xc3};                                   // +1c never reached: EAX=9; RET
    static_assert(sizeof(code)==B+0x22, "thread fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    // RIP-relative operand at `at` of an instruction ending at `end`.
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x07,0x0b,handleA); rip(0x10,0x14,entry+A); rip(0x17,0x1b,arg); rip(0x20,0x24,create);
    rip(0x2b,0x2f,handleB); rip(0x34,0x38,entry+B); rip(0x42,0x46,create);
    rip(0x4d,0x51,handleA); rip(0x54,0x58,valueA); rip(0x5a,0x5e,join);
    rip(0x65,0x69,handleB); rip(0x6c,0x70,valueB); rip(0x72,0x76,join);
    rip(0x7c,0x80,valueA); rip(0x82,0x86,valueB);
    rip(A+0x04,A+0x0c,flagA); rip(A+0x0e,A+0x13,flagB); rip(A+0x17,A+0x1b,self);
    rip(B+0x02,B+0x07,flagA); rip(B+0x0b,B+0x13,flagB); rip(B+0x18,B+0x1c,quit);
    put(b,0x300+(arg-data),40,4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// One child thread that never returns. With `fault` it loads from address 0
// while the entry waits in scePthreadJoin; otherwise it spins forever and the
// entry exits at once with scePthreadCreate's result (0).
inline std::vector<uint8_t> make_general_stuck_thread_app(bool fault) {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","onNY9Byn-W8"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, join=data+8, handle=data+0x40;
    constexpr size_t child=0x37;
    const uint8_t code[]={
        0x53, 0x48,0x89,0xf3,                                  // 0x00 PUSH RBX; MOV RBX,RSI (exit)
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x04 LEA RDI,[handle]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x0d LEA RDX,[child]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0x85,0xc0,                          // 0x19 CALL [create]; TEST EAX,EAX
        uint8_t(fault ? 0x75 : 0xeb),0x0f,                     // 0x21 JNZ / JMP leave
        0x48,0x8b,0x3d,0,0,0,0, 0x31,0xf6, 0xff,0x15,0,0,0,0,  // 0x23 join(handle, NULL)
        0x89,0xc7, 0xff,0xd3, 0xf4,                            // 0x32 leave: EDI=EAX; CALL RBX; HLT
        // child (0x37)
        uint8_t(fault ? 0x8b : 0xeb), uint8_t(fault ? 0x04 : 0xfe), 0x25,0,0,0,0, 0xc3}; // MOV EAX,[0] / JMP $
    static_assert(sizeof(code)==child+8, "stuck thread fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x07,0x0b,handle); rip(0x10,0x14,entry+child); rip(0x1b,0x1f,create);
    rip(0x26,0x2a,handle); rip(0x2e,0x32,join);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// Producer/consumer on statically initialized objects (slots holding 0). The
// entry creates a worker, locks the mutex and waits on the condition while
// the flag is 0; the worker locks, stores 77 and the flag, signals and unlocks.
// The entry then unlocks, joins and exits with the stored value (or the
// failing call's error).
constexpr uint64_t cond_mutex=data+0x48, cond_var=data+0x50;
inline std::vector<uint8_t> make_general_cond_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","onNY9Byn-W8","9UK1vLZQft4","tn3VlD0hG60","WKAXJ4XBPQ4","kDh-NfxgMtE"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, join=data+8, lock=data+0x10, unlock=data+0x18, wait=data+0x20, signal=data+0x28;
    constexpr uint64_t handle=data+0x40, m=cond_mutex, cv=cond_var, flag=data+0x58, value=data+0x5c;
    constexpr size_t W=0x85;
    const uint8_t code[]={
        0x53, 0x48,0x89,0xf3,                                  // 0x00 PUSH RBX; MOV RBX,RSI (exit)
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x04 LEA RDI,[handle]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x0d LEA RDX,[worker]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x5d,                // 0x19 CALL [create]; JNZ fail
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x23 lock(m)
        0x85,0xc0,0x75,0x4c,                                   // 0x30 JNZ fail
        0x83,0x3d,0,0,0,0,0, 0x75,0x1a,                        // 0x34 loop: [flag]!=0 -> done
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x3d LEA RDI,[cv]; LEA RSI,[m]
        0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x2b, 0xeb,0xdd,     // 0x4b wait; JNZ fail; JMP loop
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x57 done: unlock(m)
        0x48,0x8b,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x64 MOV RDI,[handle]; XOR ESI,ESI
        0xff,0x15,0,0,0,0, 0x85,0xc0,0x75,0x09,                // 0x6d join; JNZ fail
        0x8b,0x3d,0,0,0,0, 0xff,0xd3, 0xf4,                    // 0x77 exit([value]); HLT
        0x89,0xc7, 0xff,0xd3, 0xf4,                            // 0x80 fail: exit(EAX); HLT
        // worker (0x85)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // +00 lock(m)
        0xc7,0x05,0,0,0,0,77,0,0,0,                            // +0d [value]=77
        0xc7,0x05,0,0,0,0,1,0,0,0,                             // +17 [flag]=1
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // +21 signal(cv)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // +2e unlock(m)
        0xc3};                                                 // +3b RET
    static_assert(sizeof(code)==W+0x3c, "condition fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x07,0x0b,handle); rip(0x10,0x14,entry+W); rip(0x1b,0x1f,create);
    rip(0x26,0x2a,m); rip(0x2c,0x30,lock); rip(0x36,0x3b,flag);
    rip(0x40,0x44,cv); rip(0x47,0x4b,m); rip(0x4d,0x51,wait);
    rip(0x5a,0x5e,m); rip(0x60,0x64,unlock); rip(0x67,0x6b,handle); rip(0x6f,0x73,join); rip(0x79,0x7d,value);
    rip(W+0x03,W+0x07,m); rip(W+0x09,W+0x0d,lock); rip(W+0x0f,W+0x17,value); rip(W+0x19,W+0x21,flag);
    rip(W+0x24,W+0x28,cv); rip(W+0x2a,W+0x2e,signal); rip(W+0x31,W+0x35,m); rip(W+0x37,W+0x3b,unlock);
    for (uint64_t slot : {handle, m, cv, flag}) put(b,0x300+(slot-data),0,8);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// Eight threads waiting on a condition in a loop that ignores the result, as
// games do, while the entry spins: at a stop each wait returns at once, so the
// loops run almost only in host code.
inline std::vector<uint8_t> make_general_cond_spin_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","onNY9Byn-W8","9UK1vLZQft4","tn3VlD0hG60","WKAXJ4XBPQ4","kDh-NfxgMtE"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, lock=data+0x10, wait=data+0x20;
    constexpr uint64_t handle=data+0x40, m=cond_mutex, cv=cond_var;
    constexpr size_t W=0x27;
    const uint8_t code[]={
        0x53, 0xbb,8,0,0,0,                                    // 0x00 PUSH RBX; EBX=8
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x06 next: LEA RDI,[handle]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x0f LEA RDX,[worker]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0xff,0xcb, 0x75,0xe1,               // 0x1b CALL [create]; DEC EBX; JNZ next
        0xeb,0xfe,                                             // 0x25 JMP $
        // worker (0x27)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // +00 lock(m)
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // +0d again: LEA RDI,[cv]; LEA RSI,[m]
        0xff,0x15,0,0,0,0, 0xeb,0xea};                         // +1b wait; JMP again
    static_assert(sizeof(code)==W+0x23, "condition spin fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x09,0x0d,handle); rip(0x12,0x16,entry+W); rip(0x1d,0x21,create);
    rip(W+0x03,W+0x07,m); rip(W+0x09,W+0x0d,lock);
    rip(W+0x10,W+0x14,cv); rip(W+0x17,W+0x1b,m); rip(W+0x1d,W+0x21,wait);
    for (uint64_t slot : {handle, m, cv}) put(b,0x300+(slot-data),0,8);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// The same with the entry itself looping on calls, locking and unlocking the
// mutex: it too runs almost only in host code at a stop.
inline std::vector<uint8_t> make_general_busy_stop_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","onNY9Byn-W8","9UK1vLZQft4","tn3VlD0hG60","WKAXJ4XBPQ4","kDh-NfxgMtE"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, lock=data+0x10, unlock=data+0x18, wait=data+0x20;
    constexpr uint64_t handle=data+0x40, m=cond_mutex, cv=cond_var;
    constexpr size_t W=0x41;
    const uint8_t code[]={
        0x53, 0xbb,8,0,0,0,                                    // 0x00 PUSH RBX; EBX=8
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x06 next: LEA RDI,[handle]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x0f LEA RDX,[worker]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0xff,0xcb, 0x75,0xe1,               // 0x1b CALL [create]; DEC EBX; JNZ next
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x25 busy: lock(m)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x32 unlock(m)
        0xeb,0xe4,                                             // 0x3f JMP busy
        // worker (0x41)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // +00 lock(m)
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // +0d again: LEA RDI,[cv]; LEA RSI,[m]
        0xff,0x15,0,0,0,0, 0xeb,0xea};                         // +1b wait; JMP again
    static_assert(sizeof(code)==W+0x23, "busy stop fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x09,0x0d,handle); rip(0x12,0x16,entry+W); rip(0x1d,0x21,create);
    rip(0x28,0x2c,m); rip(0x2e,0x32,lock); rip(0x35,0x39,m); rip(0x3b,0x3f,unlock);
    rip(W+0x03,W+0x07,m); rip(W+0x09,W+0x0d,lock);
    rip(W+0x10,W+0x14,cv); rip(W+0x17,W+0x1b,m); rip(W+0x1d,W+0x21,wait);
    for (uint64_t slot : {handle, m, cv}) put(b,0x300+(slot-data),0,8);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// One thread: lock a statically initialized mutex, then
// scePthreadCondTimedwait(cv, m, 1000 us) with nobody to signal: exits with
// its result, ETIMEDOUT.
inline std::vector<uint8_t> make_general_timedwait_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"9UK1vLZQft4","BmMjYxmew1w"});
    using orbis_fixture::put;
    constexpr uint64_t lock=data, timedwait=data+8, m=cond_mutex, cv=cond_var;
    const uint8_t code[]={
        0x53, 0x48,0x89,0xf3,                                  // 0x00 PUSH RBX; MOV RBX,RSI (exit)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x04 lock(m)
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x11 LEA RDI,[cv]; LEA RSI,[m]
        0xba,0xe8,0x03,0,0, 0xff,0x15,0,0,0,0,                 // 0x1f timedwait(cv, m, 1000)
        0x89,0xc7, 0xff,0xd3, 0xf4};                           // 0x2a exit(EAX); HLT
    static_assert(sizeof(code)==0x2f, "timed wait fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x07,0x0b,m); rip(0x0d,0x11,lock); rip(0x14,0x18,cv); rip(0x1b,0x1f,m); rip(0x26,0x2a,timedwait);
    for (uint64_t slot : {m, cv}) put(b,0x300+(slot-data),0,8);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// General guest: create a user-event queue, trigger/consume an edge, verify
// that the next poll times out, delete the queue and exit with udata (123).
inline std::vector<uint8_t> make_general_user_service_app() {
    auto b = make_kernel_app();
    const std::initializer_list<const char*> nids = {"j3YMu1MVNNo", "CdWp0oHWGr0", "fPhymKNvK-A",
                                                   "yH17Q6NWtVg", "bwFjS+bX9mA", "1xxcMiGu2fo"};
    use_libkernel_imports(b, nids);
    using orbis_fixture::put;
    std::fill(b.begin() + 0x601, b.begin() + 0x700, 0);
    std::string names(1, '\0'); names += "libSceUserService"; names.push_back('\0');
    const size_t module_name = names.size(); names += "libSceUserService"; names.push_back('\0');
    size_t index = 0;
    for (const char* nid : nids) {
        put(b, 0x818 + 24 * index++, names.size(), 4);
        names += nid; names += "#B#B"; names.push_back('\0');
    }
    std::copy(names.begin(), names.end(), b.begin() + 0x600);
    orbis_fixture::setTag(b, 0x61000037, names.size());
    orbis_fixture::setTag(b, 0x6100000f, module_name | (uint64_t{1} << 32) | (uint64_t{1} << 48));
    constexpr uint64_t user = data + 0x40, list = data + 0x48, event = data + 0x58,
                       name = data + 0x68, parameters = data + 0x80;
    std::vector<uint8_t> code;
    std::vector<size_t> failures;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto imm = [&](uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) code.push_back(uint8_t(value >> (8 * i)));
    };
    auto rip = [&](std::initializer_list<uint8_t> op, uint64_t address, size_t trailing = 0) {
        emit(op); imm(address - (entry + code.size() + 4 + trailing), 4);
    };
    auto call = [&](size_t i) { rip({0xff, 0x15}, data + i * 8); };
    auto expect = [&](uint32_t result) {
        if (result) { emit({0x3d}); imm(result, 4); } else emit({0x85, 0xc0});
        emit({0x0f, 0x85}); failures.push_back(code.size()); imm(0, 4);
    };
    emit({0x53, 0x48, 0x89, 0xf3}); // Preserve process exit in RBX.
    rip({0x48, 0x8d, 0x3d}, user); call(1); expect(0x80960002); // Not initialized.
    rip({0x48, 0x8d, 0x3d}, parameters); call(0); expect(0);
    emit({0x31, 0xff}); call(0); expect(0x80960003); // Already initialized.
    rip({0x48, 0x8d, 0x3d}, user); call(1); expect(0);
    rip({0x48, 0x8d, 0x3d}, list); call(2); expect(0);
    rip({0x48, 0x8d, 0x3d}, event); call(3); expect(0);
    rip({0x48, 0x8d, 0x3d}, event); call(3); expect(0x80960007); // Event consumed.
    rip({0x8b, 0x3d}, user); rip({0x48, 0x8d, 0x35}, name); emit({0xba, 7, 0, 0, 0}); call(5); expect(0);
    call(4); expect(0);
    rip({0x48, 0x8d, 0x3d}, user); call(1); expect(0x80960002);
    emit({0x31, 0xff}); call(0); expect(0);
    rip({0x48, 0x8d, 0x3d}, event); call(3); expect(0); // A fresh login after re-init.
    rip({0x8b, 0x3d}, user); emit({0x6b, 0xff, 100});
    rip({0x6b, 0x05}, list, 1); emit({10, 0x01, 0xc7});
    rip({0x03, 0x3d}, event + 4); emit({0xff, 0xd3, 0xf4}); // exit(user*100 + list[0]*10 + event.user) = 111.
    const size_t fail = code.size(); emit({0x89, 0xc7, 0xff, 0xd3, 0xf4});
    for (size_t at : failures) put(code, at, fail - (at + 4), 4);
    b.resize(0xc00 + code.size()); std::copy(code.begin(), code.end(), b.begin() + 0xc00);
    put(b, 64 + 8, 0xc00); put(b, 64 + 32, code.size()); put(b, 64 + 40, code.size());
    put(b, 0x380, 700, 4);
    return b;
}
// Call five real GNM packet-building imports. The host supplies fabricated
// shader descriptors at data+0x300/0x340 through checked guest memory.
inline std::vector<uint8_t> make_general_gnm_app() {
    auto b=make_kernel_app();
    const std::initializer_list<const char*> nids={"gAhCn6UiU4Y","5uFKckiJYRM","HlTPoZ-oY7Y","GGsn7jMTxw4","cFCp0NX8wf0"};
    use_libkernel_imports(b,nids);
    using orbis_fixture::put;
    std::fill(b.begin()+0x601,b.begin()+0x700,0);
    std::string names(1,'\0'); names+="libSceGnmDriver"; names.push_back('\0');
    const size_t module=names.size(); names+="libSceGnmDriver"; names.push_back('\0');
    size_t i=0;
    for(const char* nid:nids) {
        put(b,0x818+24*i++,names.size(),4); names+=nid; names+="#B#B"; names.push_back('\0');
    }
    std::copy(names.begin(),names.end(),b.begin()+0x600);
    orbis_fixture::setTag(b,0x61000037,names.size());
    orbis_fixture::setTag(b,0x6100000f,module|(uint64_t{1}<<32)|(uint64_t{1}<<48));
    std::vector<uint8_t> code; std::vector<size_t> failures;
    auto emit=[&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(),bytes); };
    auto imm=[&](uint64_t value,size_t width) { for(size_t n=0;n<width;++n) code.push_back(uint8_t(value>>(8*n))); };
    auto rip=[&](std::initializer_list<uint8_t> op,uint64_t target) { emit(op); imm(target-(entry+code.size()+4),4); };
    auto call=[&](size_t index) {
        rip({0xff,0x15},data+8*index);
        emit({0x85,0xc0,0x0f,0x85}); failures.push_back(code.size()); imm(0,4);
    };
    auto output=[&](uint64_t at,uint32_t capacity) { rip({0x48,0x8d,0x3d},at); emit({0xbe}); imm(capacity,4); };
    emit({0x53,0x48,0x89,0xf3});
    output(data+0x100,29); rip({0x48,0x8d,0x15},data+0x300); emit({0x31,0xc9}); call(0);
    output(data+0x180,40); rip({0x48,0x8d,0x15},data+0x340); call(1);
    output(data+0x240,10); emit({0xba,6,0,0,0}); rip({0x48,0x8d,0x0d},data+0x400);
    emit({0x45,0x31,0xc0}); call(2);
    output(data+0x280,7); emit({0xba,3,0,0,0,0x31,0xc9}); call(3);
    output(data+0x2c0,3); emit({0xba,63,0,0,0,0xb9,1,0,0,0,0x45,0x31,0xc0}); call(4);
    emit({0xbf,123,0,0,0,0xff,0xd3,0xf4});
    const size_t fail=code.size(); emit({0x89,0xc7,0xff,0xd3,0xf4});
    for(size_t at:failures) put(code,at,fail-(at+4),4);
    b.resize(0xc00+code.size()); std::copy(code.begin(),code.end(),b.begin()+0xc00);
    put(b,64+8,0xc00); put(b,64+32,code.size()); put(b,64+40,code.size());
    put(b,64+56+40,0x500);
    return b;
}
inline std::vector<uint8_t> make_general_gnm_submit_app() {
    auto b=make_kernel_app();
    const std::initializer_list<const char*> nids={"GGsn7jMTxw4","zwY0YV91TTI","yvZ73uQUqrk","b08AgtPlHPg"};
    use_libkernel_imports(b,nids);
    using orbis_fixture::put;
    std::fill(b.begin()+0x601,b.begin()+0x700,0);
    std::string names(1,'\0'); names+="libSceGnmDriver"; names.push_back('\0');
    const size_t module=names.size(); names+="libSceGnmDriver"; names.push_back('\0');
    size_t i=0;
    for(const char* nid:nids) { put(b,0x818+24*i++,names.size(),4); names+=nid; names+="#B#B"; names.push_back('\0'); }
    std::copy(names.begin(),names.end(),b.begin()+0x600);
    orbis_fixture::setTag(b,0x61000037,names.size());
    orbis_fixture::setTag(b,0x6100000f,module|(uint64_t{1}<<32)|(uint64_t{1}<<48));
    std::vector<uint8_t> code; std::vector<size_t> failures;
    auto emit=[&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(),bytes); };
    auto imm=[&](uint64_t value,size_t width) { for(size_t n=0;n<width;++n) code.push_back(uint8_t(value>>(8*n))); };
    auto rip=[&](std::initializer_list<uint8_t> op,uint64_t target) { emit(op); imm(target-(entry+code.size()+4),4); };
    auto call=[&](size_t index) { rip({0xff,0x15},data+8*index); };
    auto check=[&] { emit({0x85,0xc0,0x0f,0x85}); failures.push_back(code.size()); imm(0,4); };
    emit({0x53,0x48,0x89,0xf3});
    rip({0x48,0x8d,0x3d},data+0x100); emit({0xbe,7,0,0,0,0xba,3,0,0,0,0x31,0xc9}); call(0); check();
    rip({0x48,0x8d,0x05},data+0x100); rip({0x48,0x89,0x05},data+0x40);
    emit({0xbf,1,0,0,0}); rip({0x48,0x8d,0x35},data+0x40); rip({0x48,0x8d,0x15},data+0x50);
    emit({0x31,0xc9,0x45,0x31,0xc0}); call(1); check();
    call(2); check();
    const size_t poll=code.size(); call(3); emit({0x85,0xc0,0x0f,0x84}); imm(poll-(code.size()+4),4);
    emit({0xbf,123,0,0,0,0xff,0xd3,0xf4});
    const size_t fail=code.size(); emit({0x89,0xc7,0xff,0xd3,0xf4});
    for(size_t at:failures) put(code,at,fail-(at+4),4);
    b.resize(0xc00+code.size()); std::copy(code.begin(),code.end(),b.begin()+0xc00);
    put(b,64+8,0xc00); put(b,64+32,code.size()); put(b,64+40,code.size());
    put(b,0x350,28,4); return b;
}
// Interactive synthetic CPU renderer: read real libScePad data, write two
// pixels, submit VideoOut and wait for host presentation before reusing them.
// This intentionally uses no PS4 GPU shaders or PM4 commands.
inline std::vector<uint8_t> make_general_pad_videoout_app() {
    auto b = make_kernel_app();
    use_libkernel_imports(b, {"hv1luiJrqQM", "xk0AcarP3V4", "YndgXqQVV7c", "Up36PTk687E",
                            "i6-sR91Wt-4", "w3BY+tAEiQY", "U46NwOiJpys", "zgXifHT9ErY", "1jfXLRVzisc"});
    using orbis_fixture::put;
    // Library B = Pad, C = VideoOut, D = libkernel (usleep).
    std::copy_n("libScePad", 10, b.begin()+0x601);
    std::copy_n("libScePad", 10, b.begin()+0x60b);
    constexpr size_t video_name = 0xc0, kernel_name = 0xd0;
    std::copy_n("libSceVideoOut", 15, b.begin()+0x600+video_name);
    std::copy_n("libkernel", 10, b.begin()+0x600+kernel_name);
    for (size_t i=3; i<8; ++i) std::copy_n("#C#C",4,b.begin()+0x600+21+16*i+11);
    std::copy_n("#D#D",4,b.begin()+0x600+21+16*8+11);
    orbis_fixture::setTag(b,0x61000037,kernel_name+10);
    for (auto tag : {0x61000015,0x6100000f}) {
        orbis_fixture::appendTag(b,tag,video_name|(uint64_t{1}<<32)|(uint64_t{2}<<48));
        orbis_fixture::appendTag(b,tag,kernel_name|(uint64_t{1}<<32)|(uint64_t{3}<<48));
    }
    constexpr uint64_t port=data+0x50, pad=data+0x58, attr=data+0x60,
        pointer=data+0x90, pixels=data+0x100, sample=data+0x120;
    std::vector<uint8_t> code; std::vector<size_t> failures;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(),bytes); };
    auto imm = [&](uint64_t value,size_t width) { for(size_t i=0;i<width;++i) code.push_back(uint8_t(value>>(8*i))); };
    auto rip = [&](std::initializer_list<uint8_t> op,uint64_t target) { emit(op); imm(target-(entry+code.size()+4),4); };
    auto call = [&](size_t i) { rip({0xff,0x15},data+8*i); };
    auto check = [&] { emit({0x85,0xc0,0x0f,0x85}); failures.push_back(code.size()); imm(0,4); };
    auto positive = [&] { emit({0x85,0xc0,0x0f,0x8e}); failures.push_back(code.size()); imm(0,4); };
    auto store = [&](uint64_t target,uint32_t value) { emit({0xc7,0x05}); imm(target-(entry+code.size()+8),4); imm(value,4); };
    emit({0x53,0x48,0x89,0xf3,0x41,0x54,0x48,0x83,0xec,8}); // exit in RBX, frames in R12D; align stack.
    call(0); check();
    emit({0xbf,1,0,0,0,0x31,0xf6,0x31,0xd2,0x31,0xc9}); call(1); positive(); rip({0x89,0x05},pad);
    emit({0x31,0xff,0x31,0xf6,0x31,0xd2,0x31,0xc9}); call(3); positive(); rip({0x89,0x05},port);
    rip({0x48,0x8d,0x3d},attr); emit({0xbe}); imm(0x80002200,4);
    emit({0xba,1,0,0,0,0x31,0xc9,0x41,0xb8,2,0,0,0,0x41,0xb9,1,0,0,0});
    emit({0x48,0x83,0xec,0x10,0x48,0xc7,0x04,0x24,2,0,0,0}); call(4); emit({0x48,0x83,0xc4,0x10}); check();
    rip({0x48,0x8d,0x05},pixels); rip({0x48,0x89,0x05},pointer);
    rip({0x8b,0x3d},port); emit({0x31,0xf6}); rip({0x48,0x8d,0x15},pointer);
    emit({0xb9,1,0,0,0}); rip({0x4c,0x8d,0x05},attr); call(5); check();
    emit({0x41,0xbc,120,0,0,0}); const size_t frame = code.size();
    rip({0x8b,0x3d},pad); rip({0x48,0x8d,0x35},sample); call(2); check();
    // Button bits become visible RGB bytes, with an opaque alpha channel.
    rip({0x8b,0x05},sample); emit({0x0d,0,0,0,0xff}); rip({0x89,0x05},pixels);
    store(pixels+4,0xffabcdef);
    rip({0x8b,0x3d},port); emit({0x31,0xf6,0xba,1,0,0,0,0x31,0xc9}); call(6); check();
    const size_t pending = code.size();
    emit({0xbf,0xe8,3,0,0}); call(8); check(); // Yield while waiting for presentation.
    rip({0x8b,0x3d},port); call(7);
    emit({0x83,0xf8,1,0x0f,0x84}); imm(pending-(code.size()+4),4); check();
    emit({0x41,0xff,0xcc,0x0f,0x85}); imm(frame-(code.size()+4),4);
    emit({0xbf,123,0,0,0,0xff,0xd3,0xf4});
    const size_t fail=code.size(); emit({0x89,0xc7,0xff,0xd3,0xf4});
    for(size_t at:failures) put(code,at,fail-(at+4),4);
    b.resize(0xc00+code.size()); std::copy(code.begin(),code.end(),b.begin()+0xc00);
    put(b,64+8,0xc00); put(b,64+32,code.size()); put(b,64+40,code.size());
    return b;
}
inline std::vector<uint8_t> make_general_videoout_app(bool selfContained = false) {
    auto b = make_kernel_app();
    use_libkernel_imports(b, {"fzyMKs9kim0", "Up36PTk687E", "i6-sR91Wt-4", "w3BY+tAEiQY",
                            "HXzjK9yI30k", "U46NwOiJpys", "rWUTcKdkUzQ", "uquVH4-Du78", "D0OdFMjp46I"});
    using orbis_fixture::put;
    // Symbols 1 and 9 are libkernel; symbols 2..8 belong to libSceVideoOut.
    constexpr size_t library_name = 0xc0;
    std::copy_n("libSceVideoOut", 15, b.begin() + 0x600 + library_name);
    for (size_t i = 1; i < 8; ++i) std::copy_n("#C#C", 4, b.begin() + 0x600 + 21 + 16 * i + 11);
    orbis_fixture::setTag(b, 0x61000037, library_name + 15);
    orbis_fixture::appendTag(b, 0x61000015, library_name | (uint64_t{1} << 32) | (uint64_t{2} << 48));
    orbis_fixture::appendTag(b, 0x6100000f, library_name | (uint64_t{1} << 32) | (uint64_t{2} << 48));
    const uint64_t queue = selfContained ? data + 0x110 : data + 0x40;
    constexpr uint64_t port = data + 0x48, count = data + 0x58,
        attribute = data + 0x60, pointers = data + 0x90, result = data + 0xa0,
        events = data + 0xc0, pixels = data + 0x100;
    std::vector<uint8_t> code;
    std::vector<size_t> failures;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto imm = [&](uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) code.push_back(uint8_t(value >> (8 * i)));
    };
    auto rip = [&](std::initializer_list<uint8_t> op, uint64_t address) {
        emit(op); imm(address - (entry + code.size() + 4), 4);
    };
    auto call = [&](size_t index) { rip({0xff, 0x15}, data + index * 8); };
    auto check = [&] { emit({0x85, 0xc0, 0x0f, 0x85}); failures.push_back(code.size()); imm(0, 4); };
    auto store = [&](uint64_t address, uint32_t pixel) {
        emit({0xc7, 0x05}); imm(address - (entry + code.size() + 8), 4); imm(pixel, 4);
    };
    emit({0x53, 0x48, 0x89, 0xf3}); // Preserve the process exit function.
    if (selfContained) {
        store(data + 0x118, 0x65646976); store(data + 0x11c, 0x6f); // "video" in guest BSS.
        rip({0x48, 0x8d, 0x3d}, queue); rip({0x48, 0x8d, 0x35}, data + 0x118); call(8); check();
    }
    emit({0x31, 0xff, 0x31, 0xf6, 0x31, 0xd2, 0x31, 0xc9}); call(1);
    rip({0x48, 0x89, 0x05}, port); // Open returns a positive handle.
    // SetBufferAttribute(&attr, RGBA, linear, 16:9, width=2, height=1, pitch=2).
    rip({0x48, 0x8d, 0x3d}, attribute);
    emit({0xbe}); imm(0x80002200, 4); emit({0xba, 1, 0, 0, 0, 0x31, 0xc9});
    emit({0x41, 0xb8, 2, 0, 0, 0, 0x41, 0xb9, 1, 0, 0, 0});
    emit({0x48, 0x83, 0xec, 0x10, 0x48, 0xc7, 0x04, 0x24, 2, 0, 0, 0});
    call(2); emit({0x48, 0x83, 0xc4, 0x10}); check();
    store(pixels, 0xff123456); store(pixels + 4, 0xffabcdef);
    rip({0x48, 0x8d, 0x05}, pixels); rip({0x48, 0x89, 0x05}, pointers);
    rip({0x48, 0x8b, 0x3d}, port); emit({0x31, 0xf6});
    rip({0x48, 0x8d, 0x15}, pointers); emit({0xb9, 1, 0, 0, 0});
    rip({0x4c, 0x8d, 0x05}, attribute); call(3); check();
    rip({0x48, 0x8b, 0x3d}, queue); rip({0x48, 0x8b, 0x35}, port);
    emit({0xba, 77, 0, 0, 0}); call(4); check();
    rip({0x48, 0x8b, 0x3d}, port); emit({0x31, 0xf6, 0xba, 1, 0, 0, 0, 0x48, 0xb9});
    imm(uint64_t(int64_t(-123)), 8); call(5); check();
    rip({0x48, 0x8b, 0x3d}, queue); rip({0x48, 0x8d, 0x35}, events);
    emit({0xba, 1, 0, 0, 0}); rip({0x48, 0x8d, 0x0d}, count);
    rip({0x4c, 0x8d, 0x05}, data + 0xb0); call(0); check(); // Bounded failure if the host callback fails.
    rip({0x48, 0x8d, 0x3d}, events); rip({0x48, 0x8d, 0x35}, result); call(6); check();
    rip({0x48, 0x8b, 0x3d}, port); call(7); check();
    rip({0x8b, 0x3d}, result); emit({0xf7, 0xdf, 0xff, 0xd3, 0xf4}); // exit(-flip_argument) = 123.
    const size_t fail = code.size(); emit({0x89, 0xc7, 0xff, 0xd3, 0xf4});
    for (size_t at : failures) put(code, at, fail - (at + 4), 4);
    b.resize(0xc00 + code.size()); // Keep the larger code away from the data segment at file 0x300.
    std::copy(code.begin(), code.end(), b.begin() + 0xc00);
    put(b, 64 + 8, 0xc00); put(b, 64 + 32, code.size()); put(b, 64 + 40, code.size());
    std::copy_n("video", 6, b.begin() + 0x350);
    put(b, 0x3b0, 15000000, 4);
    return b;
}
inline std::vector<uint8_t> make_general_graphics_wait_app() {
    auto b = make_kernel_app();
    use_libkernel_imports(b, {"fzyMKs9kim0", "UoYY0DWMC0U"});
    using orbis_fixture::put;
    // The second symbol belongs to GNM, not libkernel: exercise actual
    // cross-library NID resolution in the general Orbis/FEX path.
    std::copy_n("libSceGnmDriver", 16, b.begin() + 0x640);
    std::copy_n("#C#C", 4, b.begin() + 0x630);
    orbis_fixture::setTag(b, 0x61000037, 0x50);
    orbis_fixture::appendTag(b, 0x61000015, 0x40 | (uint64_t{1} << 32) | (uint64_t{2} << 48));
    orbis_fixture::appendTag(b, 0x6100000f, 0x40 | (uint64_t{1} << 32) | (uint64_t{2} << 48));
    // The host creates a queue/subscription before running this guest. A
    // separate backend thread completes it while FEX is blocked in the wait.
    const uint8_t code[] = {
        0x53, 0x48, 0x89, 0xf3,                         // Save process exit.
        0x48, 0x8b, 0x3d, 0, 0, 0, 0,                  // RDI = queue at data+0x40.
        0x48, 0x8d, 0x35, 0, 0, 0, 0,                  // RSI = event at data+0x60.
        0xba, 1, 0, 0, 0,
        0x48, 0x8d, 0x0d, 0, 0, 0, 0,                  // RCX = count at data+0x80.
        0x45, 0x31, 0xc0,                              // R8 = null (indefinite wait).
        0xff, 0x15, 0, 0, 0, 0,                        // WaitEqueue.
        0x85, 0xc0, 0x75, 18,                          // Error -> exit(EAX).
        0x48, 0x8d, 0x3d, 0, 0, 0, 0,                  // RDI = event.
        0xff, 0x15, 0, 0, 0, 0,                        // sceGnmGetEqEventType -> 64.
        0x89, 0xc7,
        0xff, 0xd3, 0xf4,
        0x89, 0xc7, 0xff, 0xd3, 0xf4};
    static_assert(sizeof(code) == 66, "graphics wait fixture layout");
    std::fill(b.begin() + 0x200, b.begin() + 0x300, 0);
    std::copy(std::begin(code), std::end(code), b.begin() + 0x200);
    auto rip = [&](size_t at, size_t end, uint64_t target) { put(b, 0x200 + at, target - (entry + end), 4); };
    rip(7, 11, data + 0x40); rip(14, 18, data + 0x60);
    rip(26, 30, data + 0x80); rip(35, 39, data);
    rip(46, 50, data + 0x60); rip(52, 56, data + 8);
    std::copy_n("graphics", 9, b.begin() + 0x348);
    put(b, 64 + 32, sizeof(code)); put(b, 64 + 40, sizeof(code));
    return b;
}
inline std::vector<uint8_t> make_general_equeue_app() {
    auto b = make_kernel_app();
    use_libkernel_imports(b, {"D0OdFMjp46I", "WDszmSbWuDk", "F6e0kwo4cnk", "fzyMKs9kim0", "jpFjmgAC5AE"});
    using orbis_fixture::put;
    constexpr uint64_t queue = data + 0x40, name = data + 0x48,
                       events = data + 0x60, count = data + 0x80, timeout = data + 0x84;
    std::vector<uint8_t> code;
    std::vector<size_t> failures;
    auto emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto imm = [&](uint64_t value, size_t width) {
        for (size_t i = 0; i < width; ++i) code.push_back(uint8_t(value >> (8 * i)));
    };
    auto rip = [&](std::initializer_list<uint8_t> op, uint64_t address) {
        emit(op); imm(address - (entry + code.size() + 4), 4);
    };
    auto call = [&](size_t index) { rip({0xff, 0x15}, data + index * 8); };
    auto check = [&] {
        emit({0x85, 0xc0, 0x0f, 0x85}); failures.push_back(code.size()); imm(0, 4);
    };
    auto wait = [&] {
        rip({0x48, 0x8b, 0x3d}, queue); // RDI = queue handle, not its address.
        rip({0x48, 0x8d, 0x35}, events);
        emit({0xba, 1, 0, 0, 0});
        rip({0x48, 0x8d, 0x0d}, count); // Fourth argument must arrive in RCX.
        rip({0x4c, 0x8d, 0x05}, timeout); // Fifth argument R8 points to zero: poll.
        call(3);
    };
    emit({0x53, 0x48, 0x89, 0xf3}); // Save the process exit function in RBX.
    rip({0x48, 0x8d, 0x3d}, queue); rip({0x48, 0x8d, 0x35}, name); call(0); check();
    rip({0x48, 0x8b, 0x3d}, queue); emit({0xbe, 7, 0, 0, 0}); call(1); check();
    rip({0x48, 0x8b, 0x3d}, queue); emit({0xbe, 7, 0, 0, 0, 0xba, 123, 0, 0, 0}); call(2); check();
    wait(); check();
    wait(); // The edge has been consumed; polling again must return ETIMEDOUT.
    emit({0x3d}); imm(0x8002003c, 4);
    emit({0x0f, 0x85}); failures.push_back(code.size()); imm(0, 4);
    rip({0x48, 0x8b, 0x3d}, queue); call(4); check();
    rip({0x8b, 0x3d}, events + 24); emit({0xff, 0xd3, 0xf4}); // exit(event.udata) = 123.
    const size_t fail = code.size();
    emit({0x89, 0xc7, 0xff, 0xd3, 0xf4}); // Any unexpected result exits with that result.
    for (size_t at : failures) put(code, at, fail - (at + 4), 4);
    std::fill(b.begin() + 0x200, b.begin() + 0x300, 0);
    std::copy(code.begin(), code.end(), b.begin() + 0x200);
    for (uint64_t slot : {queue, events, events + 8, events + 16, events + 24, count})
        put(b, 0x300 + (slot - data), 0, 8);
    std::copy_n("events", 7, b.begin() + 0x300 + (name - data));
    put(b, 64 + 32, code.size()); put(b, 64 + 40, code.size());
    return b;
}
// What a game's libc needs: scePthreadOnce twice with an init routine that
// counts its calls (guest-code HLE), then nanosleep with 1e9 ns (POSIX
// convention: -1) and __Error(): exits with count * 100 + errno = 122, or 7
// if nanosleep did not return -1.
inline std::vector<uint8_t> make_general_libc_support_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"14bOACANTBo","NhpspxdjEKU","9BcDykPmo1I"});
    using orbis_fixture::put;
    constexpr uint64_t once=data, nanosleep=data+8, error=data+0x10;
    constexpr uint64_t ctl=data+0x40, count=data+0x48, request=data+0x50;
    constexpr size_t init=0x5c;
    const uint8_t code[]={
        0x53, 0x48,0x89,0xf3,                                  // 0x00 PUSH RBX; MOV RBX,RSI (exit)
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x04 LEA RDI,[ctl]; LEA RSI,[init]
        0xff,0x15,0,0,0,0,                                     // 0x12 once
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x18 LEA RDI,[ctl]; LEA RSI,[init]
        0xff,0x15,0,0,0,0,                                     // 0x26 once again
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x2c LEA RDI,[request]; ESI=0
        0xff,0x15,0,0,0,0, 0x83,0xf8,0xff, 0x75,0x14,          // 0x35 nanosleep; CMP EAX,-1; JNE fail
        0xff,0x15,0,0,0,0, 0x8b,0x38,                          // 0x40 __Error(); MOV EDI,[RAX]
        0x6b,0x05,0,0,0,0,100, 0x01,0xc7,                      // 0x48 EAX=[count]*100; ADD EDI,EAX
        0xff,0xd3, 0xf4,                                       // 0x51 exit(EDI); HLT
        0xbf,7,0,0,0, 0xff,0xd3, 0xf4,                         // 0x54 fail: exit(7); HLT
        // init (0x5c)
        0xff,0x05,0,0,0,0, 0xc3};                              // INC DWORD [count]; RET
    static_assert(sizeof(code)==init+7, "libc support fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x07,0x0b,ctl); rip(0x0e,0x12,entry+init); rip(0x14,0x18,once);
    rip(0x1b,0x1f,ctl); rip(0x22,0x26,entry+init); rip(0x28,0x2c,once);
    rip(0x2f,0x33,request); rip(0x37,0x3b,nanosleep); rip(0x42,0x46,error);
    rip(0x4a,0x4f,count); rip(init+0x02,init+0x06,count);
    for (uint64_t slot : {ctl, count, request}) put(b,0x300+(slot-data),0,8);
    put(b,0x300+(request-data)+8,1000000000,8); // tv_nsec = 1e9: EINVAL.
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// Detached threads give their slot back. The entry makes a detached attribute
// object (scePthreadAttrInit + Setdetachstate(1)) and creates 16 threads with
// it, more than the 8 slots: a create that gets EAGAIN is retried. Then one
// joinable thread is created and detached with scePthreadDetach. Each thread
// does LOCK INC [count]; the entry waits for 17 and exits with it (or with
// the failing call's error).
inline std::vector<uint8_t> make_general_detached_app() {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","4qGrR6eoP9Y","nsYoNRywwNg","-Wreprtu0Qs"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, detach=data+8, attr_init=data+0x10, set_detach=data+0x18;
    constexpr uint64_t handle=data+0x40, attr=data+0x48, count=data+0x50;
    constexpr size_t W=0xc3, fail=0xbe;
    const uint8_t code[]={
        0x53, 0x48,0x89,0xf3,                                  // 0x00 PUSH RBX; MOV RBX,RSI (exit)
        0x48,0x8d,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x04 scePthreadAttrInit(&attr)
        0x85,0xc0, 0x0f,0x85,0,0,0,0,                          // 0x11 JNZ fail
        0x48,0x8d,0x3d,0,0,0,0, 0xbe,1,0,0,0,                  // 0x19 LEA RDI,[attr]; ESI=1
        0xff,0x15,0,0,0,0, 0x85,0xc0, 0x0f,0x85,0,0,0,0,       // 0x25 Setdetachstate; JNZ fail
        0x41,0xbc,16,0,0,0,                                    // 0x33 R12D=16
        0x48,0x8d,0x3d,0,0,0,0, 0x48,0x8d,0x35,0,0,0,0,        // 0x39 loop: LEA RDI,[handle]; LEA RSI,[attr]
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x47 LEA RDX,[worker]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0x3d,0x23,0x00,0x02,0x80, 0x74,0xd9, // 0x53 create; EAGAIN -> loop
        0x85,0xc0, 0x0f,0x85,0,0,0,0, 0x41,0xff,0xcc, 0x75,0xcc, // 0x60 JNZ fail; DEC R12D; JNZ loop
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x6d retry: LEA RDI,[handle]; ESI=0
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x76 LEA RDX,[worker]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0x3d,0x23,0x00,0x02,0x80, 0x74,0xde, // 0x82 create; EAGAIN -> retry
        0x85,0xc0, 0x0f,0x85,0,0,0,0,                          // 0x8f JNZ fail
        0x48,0x8b,0x3d,0,0,0,0, 0xff,0x15,0,0,0,0,             // 0x97 scePthreadDetach([handle])
        0x85,0xc0, 0x0f,0x85,0,0,0,0,                          // 0xa4 JNZ fail
        0x83,0x3d,0,0,0,0,17, 0x75,0xf7,                       // 0xac wait while [count]!=17
        0x8b,0x3d,0,0,0,0, 0xff,0xd3, 0xf4,                    // 0xb5 exit([count]); HLT
        0x89,0xc7, 0xff,0xd3, 0xf4,                            // 0xbe fail: exit(EAX); HLT
        // worker (0xc3)
        0xf0,0xff,0x05,0,0,0,0, 0xc3};                         // +00 LOCK INC [count]; RET
    static_assert(sizeof(code)==W+8, "detached fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x07,0x0b,attr); rip(0x0d,0x11,attr_init); rip(0x15,0x19,entry+fail);
    rip(0x1c,0x20,attr); rip(0x27,0x2b,set_detach); rip(0x2f,0x33,entry+fail);
    rip(0x3c,0x40,handle); rip(0x43,0x47,attr); rip(0x4a,0x4e,entry+W); rip(0x55,0x59,create);
    rip(0x64,0x68,entry+fail);
    rip(0x70,0x74,handle); rip(0x79,0x7d,entry+W); rip(0x84,0x88,create); rip(0x93,0x97,entry+fail);
    rip(0x9a,0x9e,handle); rip(0xa0,0xa4,detach); rip(0xa8,0xac,entry+fail);
    rip(0xae,0xb3,count); rip(0xb7,0xbb,count); rip(W+0x03,W+0x07,count);
    for (uint64_t slot : {handle, attr, count}) put(b,0x300+(slot-data),0,8);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// The entry keeps its exit function, creates a worker, then either sets a flag
// and calls scePthreadExit(0) (`main_spins` false) or spins forever. The worker
// waits for the flag (preset when the entry spins), counts down 2^20, then
// calls the entry's exit function with 77: either way the run exits with 77.
inline std::vector<uint8_t> make_general_handoff_app(bool main_spins) {
    auto b=make_kernel_app();
    use_libkernel_imports(b,{"6UgtwV+0zb4","3kg7rT0NQIs"});
    using orbis_fixture::put;
    constexpr uint64_t create=data, quit=data+8, exitfn=data+0x40, handle=data+0x48, flag=data+0x50;
    constexpr size_t W=0x43;
    const uint8_t code[]={
        0x53, 0x48,0x89,0x35,0,0,0,0,                          // 0x00 PUSH RBX; MOV [exitfn],RSI
        0x48,0x8d,0x3d,0,0,0,0, 0x31,0xf6,                     // 0x08 LEA RDI,[handle]; XOR ESI,ESI
        0x48,0x8d,0x15,0,0,0,0, 0x31,0xc9, 0x45,0x31,0xc0,     // 0x11 LEA RDX,[worker]; ECX=R8D=0
        0xff,0x15,0,0,0,0, 0x85,0xc0, 0x75,0x13,               // 0x1d CALL [create]; JNZ fail
        0xc7,0x05,0,0,0,0,1,0,0,0,                             // 0x27 [flag]=1 (or JMP $)
        0x31,0xff, 0xff,0x15,0,0,0,0, 0xf4,                    // 0x31 scePthreadExit(0); HLT
        0x89,0xc7, 0xff,0x15,0,0,0,0, 0xf4,                    // 0x3a fail: exit(EAX); HLT
        // worker (0x43)
        0x83,0x3d,0,0,0,0,0, 0x74,0xf7,                        // +00 spin while [flag]==0
        0xb9,0,0,0x10,0, 0xff,0xc9, 0x75,0xfc,                 // +09 ECX=2^20; DEC ECX; JNZ
        0xbf,77,0,0,0, 0xff,0x15,0,0,0,0, 0xf4};               // +12 exit(77); HLT
    static_assert(sizeof(code)==W+0x1e, "handoff fixture layout");
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    auto rip=[&](size_t at, size_t end, uint64_t target) { put(b,0x200+at,target-(entry+end),4); };
    rip(0x04,0x08,exitfn); rip(0x0b,0x0f,handle); rip(0x14,0x18,entry+W); rip(0x1f,0x23,create);
    rip(0x3e,0x42,exitfn); rip(W+0x02,W+0x07,flag); rip(W+0x19,W+0x1d,exitfn);
    if (main_spins) {
        b[0x200+0x27]=0xeb; b[0x200+0x28]=0xfe;
        std::fill(b.begin()+0x200+0x29,b.begin()+0x200+0x3a,0x90);
        put(b,0x300+(flag-data),1,4);
    } else {
        rip(0x29,0x31,flag); rip(0x35,0x39,quit);
    }
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
// sceKernelClockGettime(clock, RSP buffer): the timespec lives on the guest
// stack, then is copied to app data at 0x8010. Returns the HLE result.
inline std::vector<uint8_t> make_general_clock_app(uint32_t clock=4) {
    auto b=make_kernel_app("QBi7HCK03hw");
    using orbis_fixture::put;
    const uint8_t code[]={
        0xbf,0,0,0,0, 0x48,0x83,0xec,0x10, 0x48,0x89,0xe6, // MOV EDI,clock; SUB RSP,16; MOV RSI,RSP
        0xff,0x15,0,0,0,0,                                  // 0x0c CALL [GOT]
        0x48,0x8b,0x14,0x24, 0x48,0x89,0x15,0,0,0,0,        // 0x12 MOV RDX,[RSP]; MOV [0x8010],RDX
        0x48,0x8b,0x54,0x24,0x08, 0x48,0x89,0x15,0,0,0,0,   // 0x1d MOV RDX,[RSP+8]; MOV [0x8018],RDX
        0x48,0x83,0xc4,0x10, 0xf4};                         // 0x29 ADD RSP,16; HLT
    std::fill(b.begin()+0x200,b.begin()+0x300,0);
    std::copy(std::begin(code),std::end(code),b.begin()+0x200);
    put(b,0x201,clock,4);
    put(b,0x20e,0x8000-0x4012,4);
    put(b,0x219,0x8010-0x401d,4);
    put(b,0x225,0x8018-0x4029,4);
    put(b,64+32,sizeof(code)); put(b,64+40,sizeof(code));
    return b;
}
inline maxps4::OrbisLinker::Identity kernel_identity(const char* nid="aI+OeCz8xrQ") {
    return {nid,"libkernel","libkernel",1,1,2};
}
inline maxps4::OrbisLinker::Input input(const char* path,const std::vector<uint8_t>& b,bool provider) {
    return {{{path,provider ? std::vector<std::string>{} : std::vector<std::string>{"provider.prx"}},b.data(),b.size()},
            provider ? provider_bias : app_bias};
}
}
