#include "../Native/maxps4_guest_elf.h"
#include "PS4OrbisExecutionFixture.h"
#include "../Native/maxps4_ps4_libkernel.h"
#include "../Native/maxps4_ps4_virtual_memory.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
int main() {
    using namespace orbis_execution;
    auto app=make(false), provider=make(true);
    maxps4::OrbisLinker::Image linked;
    std::string reason;
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    maxps4::GuestELF image;
    CHECK(image.prepare_orbis(linked,"app.elf",6,reason)==0);
    CHECK(image.entry()==entry && image.ps4_blockers()==4);
    const auto* code=image.translate(entry,6,5);
    CHECK(code && code[0]==0xe8 && code[5]==0x90);
    CHECK(linked.translate(entry,6,5)[0]==0xff && app[0x200]==0xff);
    const auto* slot=image.translate(data,8,4);
    CHECK(slot && slot[1]==0x40 && slot[2]==0x20);
    CHECK(image.translate(data+0x100,0x100,2));
    CHECK(!image.translate(entry,1,2) && !image.translate(data,1,1));
    CHECK(image.prepare_orbis(linked,"missing",10,reason)!=0 && image.segments().empty());
    CHECK(image.prepare_orbis(linked,"app.elf",5,reason)!=0 && image.segments().empty());
    // Invalid guest store fails before execution and publishes no image.
    app[0x20d]=0; app[0x20e]=0; app[0x20f]=0; app[0x210]=0;
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",100,reason)!=0 && image.segments().empty());
    app=make(false); provider[0x200]=0xeb;
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",100,reason)!=0);
    provider=make(true); provider[0x205]=0xf4;
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",100,reason)!=0);
    provider=make(true);
    // The relocation lands in BSS, which must survive the page-copy boundary.
    app=make(false); orbis_fixture::put(app,0x900,0x8100);
    orbis_fixture::put(app,0x202,0x8100-(0x4000+6),4);
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",6,reason)==0);
    CHECK(image.translate(data+0x100,8,4)[2]==0x20);
    provider=make(true); app=make(false);
    const uint8_t cycle[]={0xff,0x15,0xfa,0x3f,0,0,0xc3};
    std::copy(std::begin(cycle),std::end(cycle),provider.begin()+0x200);
    orbis_fixture::put(provider,64+32,7); orbis_fixture::put(provider,64+40,7);
    orbis_fixture::put(provider,0x828,7); orbis_fixture::put(provider,0x300,provider_bias+0x4000);
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",100,reason)==-ELOOP && image.segments().empty());
    CHECK(reason.find("recursive")!=std::string::npos);
    provider=make(true); app=make(false);
    // A raw readable pointer targets the operand of the currently admitted CALL.
    // Reject overlap before rewriting any stream, even if its bytes decode.
    orbis_fixture::put(app,0x310,entry+1);
    orbis_fixture::put(app,0x202,0x8010-(0x4000+6),4);
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",100,reason)!=0 && image.segments().empty());
    // Static TLS: region after a guard page, template copied below the TCB,
    // TCB self-pointer, and FS reads admitted only inside blocks/TCB.
    provider=make(true);
    auto tls=make_tls_app();
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(linked.tls.size()==1 && linked.staticTLSSize==8);
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0);
    const auto* tcb=image.thread_pointer_host();
    CHECK(tcb!=nullptr);
    uint64_t self=0; std::memcpy(&self,tcb,8);
    CHECK(self==reinterpret_cast<uintptr_t>(tcb));
    uint32_t variable=0; std::memcpy(&variable,tcb-16,4); // 16-byte aligned blocks: TP-16..TP.
    uint32_t below=0; std::memcpy(&below,tcb-8,4);
    CHECK(variable==0 && below==0x1234);
    CHECK(image.segments().back().permissions==6 && image.segments().back().address>data+0x300);
    for (int32_t outside : {-24, 16, 13}) {
        tls=make_tls_app(outside);
        CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-EFAULT && image.segments().empty() && !image.thread_pointer_host());
    }
    // Compiler-style access through RAX = FS:[0], disp8 and disp32 encodings.
    for (bool disp32 : {false, true}) {
        tls=make_tls_register_app(-8,disp32);
        CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0 && image.thread_pointer_host());
        tls=make_tls_register_app(16,disp32); // Past the TCB.
        CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-EFAULT && image.segments().empty());
    }
    tls=make_tls_register_app(-8,false,0x1234,true); // NOP between: RAX no longer trusted.
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-ENOTSUP && image.segments().empty());
    tls=make_tls_register_app(); tls[0x205]=1; // FS:[1] is not the self-pointer load.
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-ENOTSUP);
    // Initial-exec: the linker writes TPOFF64 = -8 into a RELRO GOT slot,
    // which is read-only once mapped; the FS read uses that offset.
    tls=make_tls_initial_exec_app();
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    {
        const auto* got=linked.translate(tls_got,8,4); int64_t offset=0;
        CHECK(got && !linked.translate(tls_got,8,2)); std::memcpy(&offset,got,8); CHECK(offset==-8);
    }
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0 && image.thread_pointer_host());
    CHECK(image.translate(tls_got,8,4) && !image.translate(tls_got,8,2));
    tls=make_tls_initial_exec_app(false); // Writable GOT slot: offset could change at run time.
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-EFAULT && image.segments().empty());
    tls=make_tls_initial_exec_app(true,0x1234,true); // NOP between: RAX no longer the GOT offset.
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-ENOTSUP && image.segments().empty());
    {
        // System call through a linker-synthesized HLE trampoline: libkernel needs
        // no PRX file; the GOT slot receives the trampoline; the CALL is admitted
        // without admitting the trampoline's SYSCALL as guest code.
        const uint32_t token=static_cast<uint32_t>(maxps4::PS4KernelService::thread_self);
        auto kernelApp=make_kernel_app();
        const maxps4::OrbisLinker::Input alone{{{"app.elf",{}},kernelApp.data(),kernelApp.size()},app_bias};
        CHECK(!maxps4::OrbisLinker::link({alone},{},linked,reason) && reason.find("libkernel")!=std::string::npos);
        CHECK(!maxps4::OrbisLinker::link({alone},{{kernel_identity(),0x1000,token}},linked,reason));
        CHECK(maxps4::OrbisLinker::link({alone},{{kernel_identity(),0,token}},linked,reason));
        CHECK(linked.thunks.size()==1 && linked.thunks[0].token==token && linked.modules.back().path=="<hle>");
        const uint64_t thunk=linked.thunks[0].address;
        CHECK(thunk%0x4000==0 && thunk>=data+0x300+0x4000);
        uint64_t slot=0; std::memcpy(&slot,linked.translate(data,8,4),8); CHECK(slot==thunk);
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0 && image.has_hle());
        CHECK(image.translate(entry,6,5)[0]==0xe8);
        const uint8_t expected[]={0x49,0x89,0xca,0xb8,uint8_t(token),0,0,0,0x0f,0x05,0xc3};
        const auto* code=image.translate(thunk,sizeof(expected),5);
        CHECK(code && !std::memcmp(code,expected,sizeof(expected)) && !image.translate(thunk,1,2));
        const auto syscall=reinterpret_cast<uintptr_t>(image.translate(thunk+8,2,5));
        CHECK(image.hle_gate(syscall,token) && !image.hle_gate(syscall,token+1) && !image.hle_gate(syscall+1,token));
        CHECK(!image.hle_gate(reinterpret_cast<uintptr_t>(image.translate(entry,1,5)),token));
        // Pointer argument: LEA into non-executable data is admitted; the HLE gate
        // maps the rebased host pointer back to the logical PS4 address.
        const uint32_t clockToken=static_cast<uint32_t>(maxps4::PS4KernelService::clock_gettime);
        auto clockApp=make_clock_app();
        const maxps4::OrbisLinker::Input clockAlone{{{"app.elf",{}},clockApp.data(),clockApp.size()},app_bias};
        CHECK(maxps4::OrbisLinker::link({clockAlone},{{kernel_identity("QBi7HCK03hw"),0,clockToken}},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0);
        uint64_t logical=0;
        CHECK(image.guest_address(reinterpret_cast<uintptr_t>(image.translate(timespec_address,16,6)),logical) &&
              logical==timespec_address);
        CHECK(!image.guest_address(reinterpret_cast<uintptr_t>(&logical),logical));
        // __tls_get_addr: tls_index filled by DTPMOD64/DTPOFF64; MOV EAX,[RAX] only
        // right after the call; the gate resolves module 1 / offset 0 to TP-8.
        const uint32_t tlsToken=static_cast<uint32_t>(maxps4::PS4KernelService::tls_get_addr);
        auto tlsApp=make_tls_get_addr_app();
        const maxps4::OrbisLinker::Input tlsAlone{{{"app.elf",{}},tlsApp.data(),tlsApp.size()},app_bias};
        CHECK(maxps4::OrbisLinker::link({tlsAlone},{{kernel_identity("vNe1w4diLCs"),0,tlsToken}},linked,reason));
        uint64_t ti[2]{}; std::memcpy(ti,linked.translate(tls_index_address,16,4),16);
        CHECK(ti[0]==1 && ti[1]==0);
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0);
        CHECK(image.tls_address(1,0)==image.thread_pointer_host()-8 && image.tls_address(1,8)==nullptr &&
              image.tls_address(2,0)==nullptr && image.tls_address(0,0)==nullptr);
        auto noTLS=make_kernel_app("vNe1w4diLCs"); // __tls_get_addr without any PT_TLS: refused.
        const maxps4::OrbisLinker::Input noTLSAlone{{{"app.elf",{}},noTLS.data(),noTLS.size()},app_bias};
        CHECK(maxps4::OrbisLinker::link({noTLSAlone},{{kernel_identity("vNe1w4diLCs"),0,tlsToken}},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-EFAULT);
        // An executable's DT_INIT/DT_INIT_ARRAY are validated, never run by the engine.
        auto initApp=make_init_app();
        const std::vector<maxps4::OrbisLinker::HLE> kernelHLE{{kernel_identity(),0,token}};
        auto initInput=[&]{ return std::vector<maxps4::OrbisLinker::Input>{{{{"app.elf",{}},initApp.data(),initApp.size()},app_bias}}; };
        CHECK(maxps4::OrbisLinker::link(initInput(),kernelHLE,linked,reason));
        CHECK(linked.initializers.empty());
        CHECK(image.prepare_orbis(linked,"app.elf",20,reason)==0 && image.start()==entry);
        // PRX module_start: only the provider's DT_INIT, called as (0, NULL, NULL).
        auto startApp=make_start_app(), startProvider=make_start_provider();
        const std::vector<maxps4::OrbisLinker::Input> startInputs{input("app.elf",startApp,false),input("provider.prx",startProvider,true)};
        CHECK(maxps4::OrbisLinker::link(startInputs,{},linked,reason));
        CHECK(linked.initializers.size()==1 && linked.initializers[0]==provider_bias+0x4006);
        CHECK(image.prepare_orbis(linked,"app.elf",20,reason)==0);
        CHECK(image.entry()==entry && image.start()!=entry && image.start()%0x1000==0);
        const uint8_t zero[]={0x31,0xff,0x31,0xf6,0x31,0xd2,0xe8};
        const auto* stub=image.translate(image.start(),16,5);
        CHECK(stub && !std::memcmp(stub,zero,sizeof(zero)) && stub[11]==0xe9 && !image.translate(image.start(),1,2));
        CHECK(image.prepare_orbis(linked,"app.elf",3,reason)==-E2BIG); // module_start counts against the budget.
        orbis_fixture::setTag(initApp,12,0x8010); // DT_INIT into data.
        CHECK(!maxps4::OrbisLinker::link(initInput(),kernelHLE,linked,reason) &&
              reason.find("DT_INIT outside")!=std::string::npos);
        initApp=make_init_app(); orbis_fixture::setTag(initApp,27,7);
        CHECK(!maxps4::OrbisLinker::link(initInput(),kernelHLE,linked,reason) &&
              reason.find("DT_INIT_ARRAY size")!=std::string::npos);
        initApp=make_init_app(); orbis_fixture::put(initApp,0x928,0x8010); // Array entry -> data.
        CHECK(!maxps4::OrbisLinker::link(initInput(),kernelHLE,linked,reason));
        clockApp=make_clock_app(4,0x4000); // LEA into executable code: refused.
        const maxps4::OrbisLinker::Input codePointer{{{"app.elf",{}},clockApp.data(),clockApp.size()},app_bias};
        CHECK(maxps4::OrbisLinker::link({codePointer},{{kernel_identity("QBi7HCK03hw"),0,clockToken}},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==-EFAULT && image.segments().empty());
    }
    tls=make_tls_app(0); // Reading the TCB self-pointer is allowed.
    CHECK(maxps4::OrbisLinker::link({input("app.elf",tls,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)==0);
    // Without PT_TLS, any FS read is refused and no TCB exists.
    app=make(false);
    const uint8_t fsRead[]={0x64,0x8b,0x04,0x25,0,0,0,0,0xf4};
    std::copy(std::begin(fsRead),std::end(fsRead),app.begin()+0x200);
    CHECK(maxps4::OrbisLinker::link({input("app.elf",app,false),input("provider.prx",provider,true)}, {},linked,reason));
    CHECK(image.prepare_orbis(linked,"app.elf",10,reason)!=0 && !image.thread_pointer_host());
    {
        // General mode: guest = host addresses, nothing admitted or rewritten.
        auto general=make_general_app(), target=make(true);
        const std::vector<maxps4::OrbisLinker::Input> inputs{input("app.elf",general,false),input("provider.prx",target,true)};
        CHECK(maxps4::OrbisLinker::link(inputs,{},linked,reason));
        CHECK(image.prepare_orbis(linked,"app.elf",65536,reason)!=0); // JNZ is outside the bounded list.
        uint64_t moved=0;
        CHECK(image.prepare_orbis_general(inputs,{},"app.elf",reason,moved)==0);
        CHECK(moved && moved%maxps4::GuestELF::generalAlignment==0 && image.general() && image.ps4_blockers()==0);
        CHECK(image.entry()==entry+moved && image.start()!=entry+moved);
        {
            // PS4 entry stub: SUB RSP,8; MOV RDI,&EntryParams; PUSH [RDI+8]; PUSH [RDI];
            // MOV RSI,exit; JMP entry; then the exit trampoline (SYSCALL at +8).
            const auto* stub=image.translate(image.start(),48,5);
            CHECK(stub && stub[0]==0x48 && stub[1]==0x83 && stub[4]==0x48 && stub[5]==0xbf && stub[29]==0xe9);
            uint64_t params=0, leave=0;
            std::memcpy(&params,stub+6,8); std::memcpy(&leave,stub+21,8);
            CHECK(leave==image.start()+34 && stub[34+8]==0x0f && stub[34+9]==0x05 && stub[34+10]==0xf4);
            size_t stackBytes=0; auto* stackLow=image.stack_host(stackBytes);
            CHECK(params==reinterpret_cast<uintptr_t>(stackLow)+stackBytes-maxps4::GuestELF::entryParamsBytes);
            CHECK(image.write_entry_params()==params);
            const auto* words=image.translate(params,32,4);
            uint64_t argv0=0; std::memcpy(&argv0,words+8,8);
            CHECK(words[0]==1 && argv0==params+32 && !std::memcmp(image.translate(argv0,10,4),"eboot.bin",10));
            // Then the thread-return trampoline: MOV RDI,RAX; MOV EAX,thread_exit;
            // SYSCALL (an HLE gate site); HLT, where every child thread ends.
            const uint64_t back=image.thread_return();
            CHECK(back==image.start()+48);
            const auto* ret=image.translate(back,14,5);
            CHECK(ret && ret[0]==0x48 && ret[1]==0x89 && ret[2]==0xc7 && ret[3]==0xb8 &&
                  ret[8]==0x0f && ret[9]==0x05 && ret[10]==0xf4);
            CHECK(image.hle_gate(back+8,static_cast<uint32_t>(maxps4::PS4KernelService::thread_exit)));
        }
        const auto* code=image.translate(entry+moved,0x3a,5);
        CHECK(code && reinterpret_cast<uintptr_t>(code)==entry+moved);   // Identity mapping.
        CHECK(!std::memcmp(code,general.data()+0x200,0x3a));             // GOT CALL not rewritten.
        uint64_t pointer=0, got=0;
        std::memcpy(&pointer,image.translate(general_slot+moved,8,4),8);
        std::memcpy(&got,image.translate(data+moved,8,4),8);
        CHECK(pointer==general_cell+moved && *reinterpret_cast<const uint32_t*>(pointer)==7);
        CHECK(got==provider_bias+0x4000+moved && reinterpret_cast<const uint8_t*>(got)[0]==0xb8);
        size_t bytes=0; auto* stack=image.stack_host(bytes);
        CHECK(stack && bytes==maxps4::GuestELF::generalStack);
        const auto low=reinterpret_cast<uintptr_t>(stack);
        CHECK(!image.translate(low-1,1,4) && !image.translate(low+bytes,1,4)); // Guard pages.
        CHECK(image.translate(low,bytes,6) && !image.translate(low,1,1));
        // sceKernelMmap window: host pages at their own address, after the stack.
        uint64_t window=0, windowBytes=0;
        CHECK(image.heap(window,windowBytes) && windowBytes>=maxps4::GuestELF::generalHeap);
        bool listed=false;
        for (const auto size : maxps4::GuestELF::generalHeapSizes) listed=listed || windowBytes==size;
        CHECK(listed);
        CHECK(window>=low+bytes+4096 && window%0x4000==0 && !image.translate(window,1,4));
        {
            maxps4::PS4VirtualMemory vm(image);
            uint64_t first=0, second=0;
            CHECK(vm.map(0,0x4000,3,0x1002,-1,0,first)==maxps4::ps4::ok && first==window);
            CHECK(vm.map(0,0x8000,1,0x1002,-1,0,second)==maxps4::ps4::ok && second==window+0x4000);
            auto* bytesRW=const_cast<uint8_t*>(vm.translate(first,16,2));
            CHECK(bytesRW && reinterpret_cast<uintptr_t>(bytesRW)==first && !bytesRW[0]);
            bytesRW[0]=0x5a; // A real host store at the guest address.
            CHECK(*reinterpret_cast<const uint8_t*>(first)==0x5a && !vm.translate(second,1,2));
            CHECK(vm.protect(first,0x4000,1)==maxps4::ps4::ok && !vm.translate(first,1,2) && vm.translate(first,1,4));
            CHECK(vm.unmap(first,0x4000)==maxps4::ps4::ok && !vm.translate(first,1,4));
            CHECK(vm.map(0,0x4000,3,0x1002,-1,0,first)==maxps4::ps4::ok && first==window);
            CHECK(!vm.translate(first,1,4)[0]); // Unmapped pages come back zeroed.
            uint64_t tooLarge=0;
            CHECK(vm.map(0,windowBytes,3,0x1002,-1,0,tooLarge)==maxps4::ps4::nomem);
            // Host pages: a hint is a search start, MAP_FIXED replaces what the
            // span held, alignment holds, and a span unmapped inside a region
            // can be mapped again.
            using maxps4::ps4::ok;
            uint64_t hinted=0, again=0, fixed=0, aligned=0, big=0, unused=0;
            CHECK(vm.place(window+0x100000,0x4000,0,3,false,hinted)==ok && hinted==window+0x100000);
            CHECK(vm.place(window+0x100000,0x4000,0,3,false,again)==ok && again==window+0x104000);
            CHECK(vm.place(second+0x4000,0x4000,0,3,true,fixed)==ok && fixed==second+0x4000);
            CHECK(vm.translate(second,1,4) && !vm.translate(second,1,2) && vm.translate(fixed,1,2));
            CHECK(vm.place(0,0x4000,0x200000,3,false,aligned)==ok && aligned%0x200000==0);
            CHECK(vm.place(0,0xc000,0,3,false,big)==ok && vm.unmap(big+0x4000,0x4000)==ok);
            CHECK(vm.translate(big,1,4) && !vm.translate(big+0x4000,1,4) && vm.translate(big+0x8000,1,4));
            CHECK(vm.place(big+0x4000,0x4000,0,3,true,fixed)==ok && fixed==big+0x4000 && vm.translate(fixed,1,2));
            // A fixed address outside the window: exactly there when the host
            // grants that span (returned to the host when unmapped), else
            // inside the window; never over a host mapping (the stack).
            CHECK(vm.place(0x1000000,0x8000,0,3,true,unused)==ok);
            CHECK(unused==0x1000000 || (unused>=window && unused<window+windowBytes));
            if (unused==0x1000000) {
                auto* outside=const_cast<uint8_t*>(vm.translate(unused,16,2));
                CHECK(outside && reinterpret_cast<uintptr_t>(outside)==unused);
                outside[0]=0x7e; CHECK(vm.unmap(unused+0x4000,0x4000)==ok && vm.translate(unused,1,4) && !vm.translate(unused+0x4000,1,4));
            }
            CHECK(vm.unmap(unused,0x4000)==ok && !vm.translate(unused,1,4));
            // The application area, when the host granted it at load: fixed
            // mappings inside it land exactly, and go back to PROT_NONE.
            uint64_t areaBase=0, areaSize=0; int areaStatus=0;
            maxps4::PS4VirtualMemory::app_area(areaBase,areaSize,areaStatus);
            CHECK(areaSize ? areaStatus==0 : areaStatus!=0);
            CHECK(!areaSize || (areaBase==maxps4::PS4VirtualMemory::app_area_base && areaSize==maxps4::PS4VirtualMemory::app_area_size));
            if (areaSize) {
                uint64_t inArea=0;
                CHECK(vm.place(areaBase+0x10000,0x8000,0,3,true,inArea)==ok && inArea==areaBase+0x10000 && vm.translate(inArea,0x8000,2));
                CHECK(vm.unmap(inArea,0x8000)==ok && !vm.translate(inArea,1,4));
                CHECK(vm.place(areaBase+0x10000,0x4000,0,3,true,inArea)==ok && inArea==areaBase+0x10000 && vm.unmap(inArea,0x4000)==ok);
            }
            uint64_t relocated=0;
            const uint64_t stackPage=(low+0x3fff)&~uint64_t{0x3fff};
            CHECK(vm.place(stackPage,0x4000,0,3,true,relocated)==ok && relocated>=window && relocated<window+windowBytes);
            CHECK(image.translate(stackPage,1,6) && vm.unmap(relocated,0x4000)==ok);
            CHECK(vm.place(0,0x4000,0x3000,3,false,unused)==maxps4::ps4::inval);
            vm.reset();
            CHECK(!vm.translate(first,1,4) && !vm.allocated());
        }
        {
            // Kernel reservations must support replacing one page with a
            // fixed direct mapping while both neighbouring pages stay reserved.
            using S = maxps4::PS4KernelService;
            maxps4::PS4VirtualMemory vm(image);
            maxps4::PS4GuestMemory memory(image,&vm);
            maxps4::PS4Libkernel kernel(memory,&vm);
            constexpr uint64_t page=0x4000;
            const uint64_t addressOut=low+0x100, physicalOut=low+0x108;
            const uint64_t batch=low+0x120, doneOut=low+0x140;
            uint8_t word[8]{};
            CHECK(memory.write(addressOut,word,sizeof(word)));
            CHECK(kernel.invoke(S::reserve_range,{addressOut,3*page,0,0})==0);
            CHECK(memory.read(addressOut,word,sizeof(word)));
            const uint64_t reserved=maxps4::ps4::get_le(word,8);
            CHECK(reserved>=window && !memory.permitted(reserved,3*page,4));
            CHECK(kernel.invoke(S::direct_allocate_main,{page,0,0,physicalOut})==0);
            CHECK(memory.read(physicalOut,word,sizeof(word)));
            uint8_t entry[32]{};
            maxps4::ps4::put_le(entry,0,reserved+page,8);
            maxps4::ps4::put_le(entry,8,maxps4::ps4::get_le(word,8),8);
            maxps4::ps4::put_le(entry,16,page,8);
            entry[24]=0x33; // CPU and GPU read/write; operation 0 maps direct.
            CHECK(memory.write(batch,entry,sizeof(entry)));
            CHECK(kernel.invoke(S::batch_map,{batch,1,doneOut})==0);
            CHECK(memory.read(doneOut,word,4) && maxps4::ps4::get_le(word,4)==1);
            CHECK(!memory.permitted(reserved,1,4) && memory.permitted(reserved+page,page,6));
            CHECK(!memory.permitted(reserved+2*page,1,4) && vm.allocated()==3*page);
            CHECK(memory.read(reserved+page,word,sizeof(word)) && maxps4::ps4::get_le(word,8)==0);
            CHECK(kernel.invoke(S::mtype_protect,{reserved,page,0,1})==0);
            CHECK(memory.permitted(reserved,page,4) && !memory.permitted(reserved,1,2));
            kernel.reset();
            CHECK(vm.allocated()==0 && !memory.permitted(reserved+page,1,4));
        }
        {
            // Child thread slots after the mmap window: guarded RW stacks (no
            // PT_TLS in this image, so no thread pointer).
            constexpr uint64_t slotStack=maxps4::GuestELF::generalStack;
            uint64_t top=0, tp=1, last=0, lastTP=0;
            CHECK(image.prepare_thread(0,top,tp) && top%16==0 && !tp);
            CHECK(top-slotStack>=window+windowBytes+4096 && image.translate(top-slotStack,slotStack,6));
            CHECK(!image.translate(top-slotStack-1,1,4) && !image.translate(top,1,4) && !image.translate(top-8,8,1));
            CHECK(image.prepare_thread(maxps4::GuestELF::generalThreads-1,last,lastTP) && last>top);
            CHECK(!image.prepare_thread(maxps4::GuestELF::generalThreads,last,lastTP) && !last);
        }
        {
            // With PT_TLS, each slot gets a private copy of the template and its
            // own TCB; preparing it again restores the template.
            auto tlsGeneral=make_tls_app(-8,0x1234);
            const std::vector<maxps4::OrbisLinker::Input> tlsInputs{input("app.elf",tlsGeneral,false),input("provider.prx",target,true)};
            uint64_t tlsMoved=0;
            CHECK(image.prepare_orbis_general(tlsInputs,{},"app.elf",reason,tlsMoved)==0);
            const auto mainTP=reinterpret_cast<uintptr_t>(image.thread_pointer_host());
            uint64_t top=0, tp=0, word=0;
            uint32_t value=0;
            CHECK(mainTP && image.prepare_thread(1,top,tp) && tp && tp!=mainTP && tp>=top && tp%16==0);
            std::memcpy(&word,image.translate(tp,8,4),8);
            std::memcpy(&value,image.translate(tp-8,4,4),4);
            CHECK(word==tp && value==0x1234);
            *const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(image.translate(tp-8,4,2)))=0x9999;
            std::memcpy(&value,image.translate(mainTP-8,4,4),4);
            CHECK(value==0x1234); // The main thread's TLS is separate.
            CHECK(image.prepare_thread(1,top,tp));
            std::memcpy(&value,image.translate(tp-8,4,4),4);
            CHECK(value==0x1234);
            CHECK(image.prepare_orbis_general(inputs,{},"app.elf",reason,moved)==0);
        }
        {
            // The two-thread fixture binds its four libkernel imports to HLE thunks.
            using Service=maxps4::PS4KernelService;
            auto threadsApp=make_general_threads_app();
            const std::pair<const char*,Service> imports[]={{"6UgtwV+0zb4",Service::thread_create},
                {"onNY9Byn-W8",Service::thread_join},{"3kg7rT0NQIs",Service::thread_exit},{"aI+OeCz8xrQ",Service::thread_self}};
            std::vector<maxps4::OrbisLinker::HLE> hle;
            for (const auto& import : imports) hle.push_back({kernel_identity(import.first),0,
                static_cast<uint32_t>(import.second),import.second==Service::thread_exit});
            const std::vector<maxps4::OrbisLinker::Input> threadInputs{
                {{{"app.elf",{}},threadsApp.data(),threadsApp.size()},app_bias}};
            uint64_t threadMoved=0;
            const int threadPrepared=image.prepare_orbis_general(threadInputs,hle,"app.elf",reason,threadMoved);
            if (threadPrepared) std::fprintf(stderr,"threads: %s\n",reason.c_str());
            CHECK(threadPrepared==0);
            for (size_t i=0;i<4;++i) {
                uint64_t thunk=0;
                std::memcpy(&thunk,image.translate(data+threadMoved+8*i,8,4),8);
                CHECK(image.hle_gate(thunk+maxps4::OrbisLinker::syscallOffset,static_cast<uint32_t>(imports[i].second)));
                // scePthreadExit's trampoline ends with HLT, the others return.
                const auto* last=image.translate(thunk+maxps4::OrbisLinker::thunkSize-1,1,5);
                CHECK(last && *last==(imports[i].second==Service::thread_exit ? 0xf4 : 0xc3));
            }
            CHECK(image.prepare_orbis_general(inputs,{},"app.elf",reason,moved)==0);
        }
        // A failed general preparation publishes nothing.
        CHECK(image.prepare_orbis_general(inputs,{},"missing",reason,moved)!=0 && !moved && image.segments().empty());
        auto fixed=orbis_fixture::makeStatic();
        const std::vector<maxps4::OrbisLinker::Input> absolute{{{{"static.elf",{}},fixed.data(),fixed.size()},0x400000}};
        CHECK(image.prepare_orbis_general(absolute,{},"static.elf",reason,moved)==-ENOEXEC);
        CHECK(reason.find("ET_SCE_EXEC")!=std::string::npos && !moved && image.segments().empty() && !image.general());
    }
    std::puts("PASS SCE NID/RELA -> owned page mapping -> guest CALL/RET admission; no CPU execution claimed");
}
