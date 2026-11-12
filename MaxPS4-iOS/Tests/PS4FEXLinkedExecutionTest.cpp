#include "../Native/maxps4_fex_runtime.h"
#include "PS4LinkedELFFixture.h"
#include "PS4OrbisExecutionFixture.h"
#include "PS4PlayableFixture.h"
#include "SelfFixture.h"
#include "../Native/PM4MetalDrawPlan.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static uint64_t value(const uint8_t* bytes) {
    uint64_t result = 0;
    for (size_t i = 0; i < 8; ++i) result |= uint64_t(bytes[i]) << (8*i);
    return result;
}
static int playableTest(const char* evidence = nullptr) {
    using namespace playable_fixture;
    for (int restart=0;restart<2;++restart) {
        auto file=app(), provider=orbis_execution::make(true);
        if (restart) {
            const std::vector<uint8_t> pattern{0xb8,0x18,0x20,0x30,0xff};
            const auto at=std::search(file.begin()+0x1000,file.end(),pattern.begin(),pattern.end());
            CHECK(at!=file.end()); *(at+1)=0x19; // Change actual guest pixel instruction.
        }
        const char* deps[]={"provider.prx"};
        const maxps4_orbis_module modules[]={
            {"app.elf",file.data(),file.size(),orbis_execution::app_bias,deps,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process=nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        struct Close { maxps4_fex_engine* p; ~Close(){ maxps4_fex_engine_close(p); } } close{process};
        char reason[1024]{};
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,2,"app.elf",65536,reason,sizeof(reason))==0);
        CHECK(maxps4_fex_engine_configure_frame(process,pixels,input,width,height)==0);
        std::vector<uint8_t> out(width*height*4,0xa5), memory(out.size());
        uint64_t sequence=99; uint32_t backend=99;
        CHECK(maxps4_fex_engine_step_frame(process,0,out.data(),out.size(),&sequence,&backend)==-EAGAIN);
        CHECK(!sequence && !backend && out[0]==0xa5);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_configure_frame(process,pixels,input,0,height)==-EINVAL);
        CHECK(maxps4_fex_engine_step_frame(process,0,out.data(),out.size(),&sequence,&backend)!=0 && out[0]==0xa5);
        CHECK(maxps4_fex_engine_configure_frame(process,UINT64_MAX-3,input,width,height)==-EFAULT);
        CHECK(maxps4_fex_engine_configure_frame(process,pixels,pixels,width,height)==-EINVAL);
        CHECK(maxps4_fex_engine_configure_frame(process,pixels,input,width,height)==0);
        CHECK(maxps4_fex_engine_step_frame(process,32,out.data(),out.size(),&sequence,&backend)==-EINVAL);
        CHECK(maxps4_fex_engine_step_frame(process,0,out.data(),out.size()-1,&sequence,&backend)==-EINVAL);
        CHECK(out[0]==0xa5);
        uint32_t expectedX=7,expectedY=3; uint64_t expectedSequence=0;
        for (uint32_t buttons : {0u,2u,1u,8u,4u,16u,3u,12u,2u,2u,2u,2u}) {
            expectedX=(expectedX+((buttons>>1)&1)-(buttons&1))&15;
            expectedY=(expectedY+((buttons>>3)&1)-((buttons>>2)&1))&7;
            CHECK(maxps4_fex_engine_step_frame(process,buttons,out.data(),out.size(),&sequence,&backend)==0);
            CHECK(sequence==++expectedSequence);
#ifdef VIXL_SIMULATOR
            CHECK(backend==MAXPS4_CPU_FEX_ARM64_SIMULATOR);
#else
            CHECK(backend==MAXPS4_CPU_FEX_ARM64_JIT);
#endif
            CHECK(maxps4_fex_engine_read_ps4(process,pixels,memory.data(),memory.size())==0 && memory==out);
            uint8_t state[16]{};
            CHECK(maxps4_fex_engine_read_ps4(process,input,state,sizeof(state))==0);
            CHECK(state[0]==buttons && state[4]==expectedX && state[8]==expectedY && state[12]==sequence);
            for (uint32_t py=0;py<height;++py) for (uint32_t px=0;px<width;++px) {
                const size_t off=(py*width+px)*4;
                uint32_t pixel=0; for (int b=0;b<4;++b) pixel|=uint32_t(out[off+b])<<(8*b);
                const uint32_t expected = py<16 && px/2==expectedX && py/2==expectedY ?
                    (0xff2080e0 | ((buttons&16)<<16)) : 0xff302018;
                CHECK(pixel==((restart && px==0 && py==0 && expected==0xff302018) ? 0xff302019 : expected));
            }
            uint64_t hash=14695981039346656037ull;
            for (auto byte:out) hash=(hash^byte)*1099511628211ull;
            std::printf("PASS guest-frame restart=%d sequence=%llu backend=%u input=%u x=%u y=%u bytes=%zu FNV64=%llx guest-memory-equal=yes\n",
                restart,(unsigned long long)sequence,backend,buttons,expectedX,expectedY,out.size(),(unsigned long long)hash);
            if (evidence && !restart) {
                std::filesystem::create_directories(evidence);
                std::ofstream rgba(std::filesystem::path(evidence)/("frame-"+std::to_string(sequence)+".rgba"),std::ios::binary);
                rgba.write(reinterpret_cast<const char*>(out.data()),out.size()); CHECK(bool(rgba));
                std::ofstream ppm(std::filesystem::path(evidence)/("frame-"+std::to_string(sequence)+".ppm"),std::ios::binary);
                ppm << "P6\n32 24\n255\n";
                for (size_t off=0;off<out.size();off+=4) ppm.write(reinterpret_cast<const char*>(out.data()+off),3);
                CHECK(bool(ppm));
            }
        }
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,1,"app.elf",65536,reason,sizeof(reason))!=0);
        CHECK(maxps4_fex_engine_step_frame(process,0,out.data(),out.size(),&sequence,&backend)==-EFAULT);
        CHECK(!sequence && !backend);
    }
    std::puts("PASS real FEX framebuffer, input-to-guest-state, successive images, action, machine-code mutation and close/restart");
    return 0;
}
static int animatedGuestTest() {
    using namespace playable_fixture;
    auto file=app(true), provider=orbis_execution::make(true);
    const char* deps[]={"provider.prx"};
    const maxps4_orbis_module modules[]={
        {"app.elf",file.data(),file.size(),orbis_execution::app_bias,deps,1},
        {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
    maxps4_fex_engine* engine=nullptr;
    CHECK(maxps4_fex_engine_open(&engine)==0);
    struct Close { maxps4_fex_engine* p; ~Close(){maxps4_fex_engine_close(p);} } close{engine};
    char reason[1024]{};
    const int prepared=maxps4_fex_engine_prepare_orbis(engine,modules,2,"app.elf",65536,reason,sizeof(reason));
    if (prepared) std::fprintf(stderr,"animated guest prepare failed: status=%d reason=%s ELF_bytes=%zu\\n",prepared,reason,file.size());
    CHECK(prepared==0);
    CHECK(maxps4_fex_engine_configure_frame(engine,pixels,input,width,height)==0);
    CHECK(maxps4_fex_engine_initialize(engine)==0);
    std::vector<uint8_t> image(width*height*4);
    uint64_t sequence=0; uint32_t backend=0;
    for (uint32_t frame=0;frame<20;++frame) {
        CHECK(maxps4_fex_engine_step_frame(engine,frame<2 ? 8u : (frame==9 ? 16u : 0u),image.data(),image.size(),&sequence,&backend)==0);
        CHECK(sequence==frame+1);
#ifdef VIXL_SIMULATOR
        CHECK(backend==MAXPS4_CPU_FEX_ARM64_SIMULATOR);
#else
        CHECK(backend==MAXPS4_CPU_FEX_ARM64_JIT);
#endif
        const uint32_t enemyCell=80+(frame&15);
        const size_t enemyOffset=((enemyCell/16)*2*width+(enemyCell%16)*2)*4;
        uint32_t rgba=0;
        for(int b=0;b<4;++b) rgba|=uint32_t(image[enemyOffset+b])<<(8*b);
        CHECK(rgba==(frame==7 ? 0xff2080e0u : 0xff40d030u));
        if (frame==9) {
            const size_t playerOffset=((5u*2u)*width+(7u*2u))*4u;
            uint32_t player=0;
            for (int b=0;b<4;++b) player|=uint32_t(image[playerOffset+b])<<(8*b);
            CHECK(player==0xffa080e0u); // Action visibly changes red channel in guest code.
        }
        if(frame>0) {
            const uint32_t oldCell=80+((frame-1)&15);
            if(oldCell!=enemyCell) {
                const size_t oldOffset=((oldCell/16)*2*width+(oldCell%16)*2)*4;
                uint32_t oldRGBA=0;
                for(int b=0;b<4;++b) oldRGBA|=uint32_t(image[oldOffset+b])<<(8*b);
                CHECK(oldRGBA==(oldCell==87 ? 0xff2080e0u : 0xff302018u));
            }
        }
    }
    std::puts("PASS guest x86-64 autonomous sprite animation and collision across 20 real FEX frames");
    return 0;
}
int main(int argc, char** argv) {
    using namespace ps4_linked_test;
    // CI logs go to a file: keep every line written before a crash or a kill.
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc==3 && !std::strcmp(argv[1],"--playable-evidence")) return playableTest(argv[2]);
    if (argc==3 && (!std::strcmp(argv[1],"--playable-bundle") || !std::strcmp(argv[1],"--animated-playable-bundle"))) {
        std::filesystem::create_directories(argv[2]);
        const auto file=playable_fixture::app(!std::strcmp(argv[1],"--animated-playable-bundle")), provider=orbis_execution::make(true);
        for (bool prx : {false,true}) {
            const auto& bytes=prx ? provider : file;
            std::ofstream out(std::filesystem::path(argv[2])/(prx ? "provider.prx" : "app.elf"),std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); CHECK(bool(out));
        }
        return 0;
    }
    if (argc==3 && !std::strcmp(argv[1],"--guest-threads-bundle")) {
        // The app's built-in guest thread test (Resources/GuestThreadsDemo).
        std::filesystem::create_directories(argv[2]);
        for (bool cond : {false,true}) {
            const auto bytes=cond ? orbis_execution::make_general_cond_app() : orbis_execution::make_general_threads_app();
            std::ofstream out(std::filesystem::path(argv[2])/(cond ? "cond.elf" : "threads.elf"),std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); CHECK(bool(out));
        }
        return 0;
    }
    if (argc==3 && !std::strcmp(argv[1],"--pad-videoout-bundle")) {
        const auto bytes = orbis_execution::make_general_pad_videoout_app();
        FILE* output = std::fopen(argv[2], "wb"); CHECK(output);
        CHECK(std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size());
        CHECK(std::fclose(output) == 0); return 0;
    }
    if (argc==3 && !std::strcmp(argv[1],"--videoout-bundle")) {
        std::filesystem::create_directories(argv[2]);
        const auto bytes = orbis_execution::make_general_videoout_app(true);
        std::ofstream out(std::filesystem::path(argv[2])/"videoout.elf", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); CHECK(bool(out));
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1],"--orbis-bundle")) {
        const std::filesystem::path directory(argv[2]);
        std::filesystem::create_directories(directory);
        for (bool provider : {false,true}) {
            const auto bytes = orbis_execution::make(provider);
            std::ofstream file(directory/(provider ? "provider.prx" : "app.elf"),std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            CHECK(bool(file));
        }
        std::ofstream manifest(directory/"maxps4.json");
        manifest << R"({"root":"app.elf","modules":[{"path":"app.elf","loadBias":1048576,"dependencies":["provider.prx"]},{"path":"provider.prx","loadBias":2097152,"dependencies":[]}]})";
        CHECK(bool(manifest));
        return 0;
    }
    // Export the identical tested bytes for the application's import picker.
    if (argc == 2 || argc == 3) {
        for (int i = 1; i < argc; ++i) {
            const auto file = i == 1 ? minimal() : combined();
            FILE* out = std::fopen(argv[i],"wb");
            CHECK(out && std::fwrite(file.data(),1,file.size(),out) == file.size());
            CHECK(std::fclose(out) == 0);
        }
        return 0;
    }
    CHECK(playableTest()==0);
    CHECK(animatedGuestTest()==0);
    CHECK(maxps4_fex_test_ps4_libkernel(nullptr) == -EINVAL);
    for (size_t repeat = 0; repeat < 3; ++repeat) {
        maxps4_ps4_elf_test_report report{};
        CHECK(maxps4_fex_test_ps4_libkernel(&report) == 0);
        CHECK(!report.open_status && !report.load_status && !report.link_status);
        CHECK(!report.initialize_status && !report.execute_status && !report.read_status && !report.verification_status);
        CHECK(report.entry == entry && report.segments == 2 && report.imports == 1);
        CHECK(report.resolved_service == maxps4_ps4_resolve_libkernel("aI+OeCz8xrQ", "libkernel", "libkernel", 1));
        CHECK(report.calls == 1 && report.resumed == 1 && report.kernel_return == 1);
        CHECK(report.final_memory == 2 && report.rax == 2 && !report.error[0]);
        CHECK(report.guest_segments_after == 0 && report.jit_regions_after == report.jit_regions_before);
#ifdef VIXL_SIMULATOR
        CHECK(report.backend == MAXPS4_CPU_FEX_ARM64_SIMULATOR);
#else
        CHECK(report.backend == MAXPS4_CPU_FEX_ARM64_JIT);
#endif
        std::printf("PASS built-in app ELF diagnostic: entry=0x%llx NID=aI+OeCz8xrQ kernel=%llu resumed=%u memory=%llu backend=%u guest_segments_after=%u jit_leases_after=%u\n",
            (unsigned long long)report.entry, (unsigned long long)report.kernel_return, report.resumed,
            (unsigned long long)report.final_memory, report.backend, report.guest_segments_after, report.jit_regions_after);
    }
    // SCE tables and fabricated NID supplied by an explicit PRX dependency.
    // Observe different provider machine code results; never substitute 42.
    for (uint32_t provider_value : {40U,7U}) {
        auto app = orbis_execution::make(false);
        auto provider = orbis_execution::make(true,provider_value);
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,2,"app.elf",6,diagnostic,sizeof(diagnostic))==0);
        uint64_t observed=99, logical_entry=99; uint32_t cpu=99;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==-EAGAIN);
        CHECK(!observed && !cpu && !logical_entry);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        for (int repeat=0;repeat<2;++repeat) {
            CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
            CHECK(observed==provider_value+2 && logical_entry==orbis_execution::entry);
#ifdef VIXL_SIMULATOR
            CHECK(cpu==MAXPS4_CPU_FEX_ARM64_SIMULATOR);
#else
            CHECK(cpu==MAXPS4_CPU_FEX_ARM64_JIT);
#endif
            uint8_t stored[4]{};
            CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::data+8,stored,4)==0);
            CHECK(stored[0]==0x34 && stored[1]==0x12 && !stored[2] && !stored[3]);
        }
        std::printf("PASS SCE ELF -> PRX NID/RELA -> FEX CALL/RET -> ADD: RAX=%llu backend=%u entry=0x%llx store=0x1234\n",
            (unsigned long long)observed,cpu,(unsigned long long)logical_entry);
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,1,"app.elf",6,diagnostic,sizeof(diagnostic))!=0);
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==-EFAULT);
        CHECK(!observed && !cpu && !logical_entry);
        maxps4_fex_engine_close(process);
    }
    // Static TLS through FEX: the guest reads its PT_TLS variable via FS.
    // Two template values prove the value comes from the module, not a constant.
    // Third case: the compiler-style form, RAX = FS:[0] then [RAX-8].
    // Fourth case: initial-exec, offset loaded from a read-only GOT slot.
    for (int form = 0; form < 4; ++form) {
        const uint32_t initial = form == 1 ? 0x55U : 0x1234U;
        auto app = form == 2 ? orbis_execution::make_tls_register_app(-8, false, 0x77U)
                 : form == 3 ? orbis_execution::make_tls_initial_exec_app(true, 0x99U)
                             : orbis_execution::make_tls_app(-8, initial);
        const uint32_t expected = form == 2 ? 0x77U : form == 3 ? 0x99U : initial;
        auto provider = orbis_execution::make(true);
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,2,"app.elf",10,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
        CHECK(observed==expected+2 && logical_entry==orbis_execution::entry);
        std::printf("PASS SCE ELF PT_TLS -> static TLS block -> FEX %s -> ADD: RAX=0x%llx backend=%u\n",
            form == 3 ? "MOV RAX,[GOT TPOFF]; MOV EAX,FS:[RAX]" :
            form == 2 ? "MOV RAX,FS:[0]; MOV EAX,[RAX-8]" : "MOV EAX,FS:[-8]", (unsigned long long)observed,cpu);
        maxps4_fex_engine_close(process);
    }
    {
        // System call: the app imports scePthreadSelf from libkernel (no PRX file).
        // The engine registers libkernel HLE; FEX runs CALL -> trampoline SYSCALL
        // -> host PS4Libkernel -> RET, and the guest adds 2 to the thread ID (1).
        auto app = orbis_execution::make_kernel_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        const int prepared = maxps4_fex_engine_prepare_orbis(process,modules,1,"app.elf",10,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        for (int repeat=0;repeat<2;++repeat) {
            CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
            CHECK(observed==3 && logical_entry==orbis_execution::entry);
        }
        std::printf("PASS SCE ELF -> libkernel HLE scePthreadSelf via SYSCALL trampoline -> FEX ADD: RAX=%llu backend=%u\n",
            (unsigned long long)observed,cpu);
        maxps4_fex_engine_close(process);
    }
    {
        // PRX module_start through FEX: the stub calls provider.prx's DT_INIT (stores
        // 40), then JMPs to the entry (+2). 42 proves the PRX started and that
        // neither its DT_INIT_ARRAY (+1000) nor the executable's DT_INIT (99) ran.
        auto app = orbis_execution::make_start_app();
        auto provider = orbis_execution::make_start_provider();
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        const int prepared = maxps4_fex_engine_prepare_orbis(process,modules,2,"app.elf",20,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        for (int repeat=0;repeat<2;++repeat) {
            CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
            CHECK(observed==42 && logical_entry==orbis_execution::entry);
        }
        std::printf("PASS SCE ELF -> startup stub -> provider.prx module_start(0,NULL,NULL) -> entry via FEX: RAX=%llu backend=%u\n",
            (unsigned long long)observed,cpu);
        maxps4_fex_engine_close(process);
    }
    for (uint64_t offset : {0ULL, 8ULL}) {
        // General-dynamic TLS through libkernel HLE __tls_get_addr. Offset 8 is
        // past the 8-byte block: the gate refuses it and the run fails cleanly.
        auto app = orbis_execution::make_tls_get_addr_app(0x4242, offset);
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        const int prepared = maxps4_fex_engine_prepare_orbis(process,modules,1,"app.elf",10,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
        if (!offset) {
            CHECK(ran==0 && observed==0x4242+2);
            std::printf("PASS SCE ELF -> DTPMOD64/DTPOFF64 tls_index -> libkernel HLE __tls_get_addr -> FEX MOV EAX,[RAX]: RAX=0x%llx backend=%u\n",
                (unsigned long long)observed,cpu);
        } else {
            CHECK(ran==-EFAULT);
            std::puts("PASS __tls_get_addr offset outside the TLS block refused at run time (no invalid pointer)");
        }
        maxps4_fex_engine_close(process);
    }
    for (uint32_t clock : {4U, 99U}) {
        // Pointer argument: sceKernelClockGettime(clock, &timespec) writes guest
        // memory through the translated pointer. Clock 99 must fail, not write.
        auto app = orbis_execution::make_clock_app(clock);
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        const int prepared = maxps4_fex_engine_prepare_orbis(process,modules,1,"app.elf",10,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
        uint8_t wire[16]{};
        CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::timespec_address,wire,sizeof(wire))==0);
        uint64_t seconds=0, nanoseconds=0;
        std::memcpy(&seconds,wire,8); std::memcpy(&nanoseconds,wire+8,8);
        if (clock == 4) {
            CHECK(observed==2 && nanoseconds<1000000000ULL && (seconds || nanoseconds));
            std::printf("PASS SCE ELF -> LEA RSI pointer -> libkernel HLE sceKernelClockGettime -> guest timespec %llu.%09llu s, RAX=%llu backend=%u\n",
                (unsigned long long)seconds,(unsigned long long)nanoseconds,(unsigned long long)observed,cpu);
        } else {
            CHECK(observed!=2 && !seconds && !nanoseconds);
            std::printf("PASS invalid clock rejected by libkernel HLE: RAX=0x%llx, timespec untouched\n",(unsigned long long)observed);
        }
        maxps4_fex_engine_close(process);
    }
    for (uint32_t provided : {40U, 7U}) {
        // General execution: identity-mapped image, no instruction list. A loop, a
        // local CALL with a stack frame, an absolute pointer filled by RELATIVE, an
        // unpatched GOT CALL into provider.prx and a BSS store, all run by FEX.
        auto app = orbis_execution::make_general_app(9);
        auto provider = orbis_execution::make(true, provided);
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,2,"app.elf",65536,diagnostic,sizeof(diagnostic))!=0);
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,2,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0 && relocation && relocation%(1024*1024)==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        const uint64_t expected = 110 + 9 + provided;
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        for (int repeat=0;repeat<2;++repeat) {
            CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
            CHECK(observed==expected && logical_entry==orbis_execution::entry+relocation);
            uint8_t stored[4]{};
            CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::general_store+relocation,stored,sizeof(stored))==0);
            CHECK((stored[0]|stored[1]<<8|stored[2]<<16|uint32_t(stored[3])<<24)==expected);
        }
        std::printf("PASS general FEX execution (identity mapping, relocation=0x%llx): loop + stack CALL + absolute pointer + GOT CALL + BSS store -> RAX=%llu backend=%u\n",
            (unsigned long long)relocation,(unsigned long long)observed,cpu);
        maxps4_fex_engine_close(process);
    }
    {
        // HLE with a pointer to the guest stack: the timespec is written there by
        // libkernel, then copied by guest code to app data.
        auto app = orbis_execution::make_general_clock_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=99, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0 && observed==0);
        uint8_t wire[16]{};
        CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::timespec_address+relocation,wire,sizeof(wire))==0);
        uint64_t seconds=0, nanoseconds=0;
        std::memcpy(&seconds,wire,8); std::memcpy(&nanoseconds,wire+8,8);
        CHECK(nanoseconds<1000000000ULL && (seconds || nanoseconds));
        std::printf("PASS general FEX execution -> libkernel HLE sceKernelClockGettime(RSP buffer) -> %llu.%09llu s backend=%u\n",
            (unsigned long long)seconds,(unsigned long long)nanoseconds,cpu);
        maxps4_fex_engine_close(process);
    }
    {
        // sceKernelMmap from general code: the seventh argument is read from the
        // guest stack, the mapping is real host memory in the image's window and
        // guest code dereferences the returned pointer directly.
        auto app = orbis_execution::make_general_mmap_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
        uint8_t wire[8]{};
        CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::mmap_out+relocation,wire,sizeof(wire))==0);
        const uint64_t mapped = value(wire);
        std::printf("general sceKernelMmap: RAX=0x%llx mapping=0x%llx relocation=0x%llx\n",
            (unsigned long long)observed,(unsigned long long)mapped,(unsigned long long)relocation);
        CHECK(observed==0x1236 && mapped>orbis_execution::entry+relocation && mapped%0x4000==0);
        uint8_t stored[4]{};
        CHECK(maxps4_fex_engine_read_ps4(process,mapped+0x100,stored,sizeof(stored))==0 && stored[0]==0x34 && stored[1]==0x12);
        std::printf("PASS general FEX execution -> sceKernelMmap (stack argument) -> guest store/load through the returned pointer: RAX=0x%llx backend=%u\n",
            (unsigned long long)observed,cpu);
        maxps4_fex_engine_close(process);
    }
    {
        // PS4 process entry: RDI = EntryParams, [RSP] = their copy, RSP = 8 mod 16,
        // RSI = exit function. Calling it with 42 ends the run cleanly.
        auto app = orbis_execution::make_general_entry_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        for (int repeat=0;repeat<2;++repeat) {
            uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
            CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
            maxps4_fex_cpu_report report{};
            CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0);
            std::printf("general PS4 entry: RAX=%llu exited=%u status=%d argc=%llu\n",(unsigned long long)observed,
                report.exited,report.exit_status,(unsigned long long)report.argc);
            CHECK(observed==42 && report.exited==1 && report.exit_status==42 && report.argc==1);
        }
        std::puts("PASS general FEX execution -> PS4 entry ABI (EntryParams in RDI, copy at RSP, RSP=8 mod 16) -> exit function(42)");
        maxps4_fex_engine_close(process);
    }
    {
        // The same image in a homebrew-style SELF with plaintext blocks runs the
        // same; with encrypted blocks it is refused before any mapping.
        const auto self = self_fixture::wrap(orbis_execution::make_general_entry_app());
        const auto encrypted = self_fixture::wrap(orbis_execution::make_general_entry_app(), 0x2);
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        const maxps4_orbis_module refused[] = {
            {"eboot.bin",encrypted.data(),encrypted.size(),orbis_execution::app_bias,nullptr,0}};
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,refused,1,"eboot.bin",&relocation,diagnostic,sizeof(diagnostic))==-ENOTSUP);
        CHECK(std::strstr(diagnostic,"chiffré"));
        const maxps4_orbis_module modules[] = {
            {"eboot.bin",self.data(),self.size(),orbis_execution::app_bias,nullptr,0}};
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,1,"eboot.bin",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0);
        CHECK(observed==42 && report.exited==1 && report.exit_status==42);
        std::puts("PASS general FEX execution of a SELF with plaintext blocks (fSELF) -> exit(42); encrypted SELF refused");
        maxps4_fex_engine_close(process);
    }
    {
        // module_start under the general entry stub: the PRX's DT_INIT runs with an
        // aligned stack before the entry; the entry still ends on HLT (not exited).
        auto app = orbis_execution::make_start_app();
        auto provider = orbis_execution::make_start_provider();
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,2,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0 && observed==42);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 && !report.exited);
        std::puts("PASS general FEX execution -> provider.prx module_start -> PS4 entry stub -> entry (HLT)");
        maxps4_fex_engine_close(process);
    }
    {
        // Guest threads: scePthreadCreate x2, threads that wait for each other
        // (so they must run concurrently), RET into the thread-return trampoline
        // and scePthreadExit, then scePthreadJoin x2 and exit(42 + 58).
        auto app = orbis_execution::make_general_threads_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        // Thread services are for general images only: a bounded image that
        // imports them is refused.
        CHECK(maxps4_fex_engine_prepare_orbis(process,modules,1,"app.elf",65536,diagnostic,sizeof(diagnostic))==-ENOEXEC &&
              std::strstr(diagnostic,"unresolved NID 6UgtwV+0zb4"));
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 || ran);
        std::printf("general guest threads: run=%d RAX=%llu exited=%u status=%d\n",ran,(unsigned long long)observed,
            report.exited,report.exit_status);
        CHECK(ran==0 && observed==100 && report.exited==1 && report.exit_status==100);
        std::puts("PASS general FEX execution -> scePthreadCreate x2 (concurrent, own stacks) -> RET / scePthreadExit -> scePthreadJoin x2 -> exit(100)");
        maxps4_fex_engine_close(process);
    }
    {
        // One thread: scePthreadCondTimedwait with nobody to signal times out
        // (no thread needed, so this also runs under the VIXL simulator).
        auto app = orbis_execution::make_general_timedwait_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 || ran);
        const int32_t timed_out = static_cast<int32_t>(0x80020000u + 60);
        std::printf("general timed wait: run=%d exited=%u status=%d\n",ran,report.exited,report.exit_status);
        CHECK(ran==0 && report.exited==1 && report.exit_status==timed_out);
        std::puts("PASS general scePthreadMutexLock + scePthreadCondTimedwait(1000 us) -> ETIMEDOUT");
        maxps4_fex_engine_close(process);
    }
    {
        auto app=orbis_execution::make_general_gnm_submit_app();
        const maxps4_orbis_module module{"submit.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0};
        maxps4_fex_engine* process=nullptr; CHECK(maxps4_fex_engine_open(&process)==0);
        uint64_t relocation=0,observed=0,entry=0; uint32_t backend=0; char diagnostic[512]{};
        const int prepared=maxps4_fex_engine_prepare_orbis_general(process,&module,1,"submit.elf",&relocation,diagnostic,sizeof(diagnostic));
        if(prepared) std::fprintf(stderr,"prepare GNM submit: %s\n",diagnostic);
        CHECK(prepared==0 && maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_gnm_backend(process,1)==0);
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        CHECK(maxps4_fex_engine_set_time_limit(process,10000)==0);
#endif
        int host_status=-1; maxps4_gnm_submission captured{};
        std::thread consumer([&] {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(8);
            uint32_t words[7]{};
            while(std::chrono::steady_clock::now()<end) {
                const int status=maxps4_fex_engine_gnm_snapshot(process,&captured,words,7);
                if(status==-EAGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                if(status || words[0]!=0xc0012d00 || words[1]!=3 || words[2]!=2) { host_status=-2; return; }
                // Explicit test acknowledgment, not a claim of GPU execution.
                host_status=maxps4_fex_engine_gnm_complete(process,captured.ticket,1); return;
            }
        });
        const int ran=maxps4_fex_engine_run_orbis(process,&observed,&backend,&entry);
        consumer.join(); CHECK(ran==0 && host_status==0 && captured.word_count==7 && captured.draw_count==1);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 && report.exited==1 && report.exit_status==123);
        CHECK(maxps4_fex_engine_gnm_complete(process,captured.ticket,1)==-EINVAL);
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,&module,1,"submit.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        const auto allowed=maxps4_ps4_resolve_libkernel("b08AgtPlHPg","libSceGnmDriver","libSceGnmDriver",1);
        maxps4_ps4_kernel_call call{};
        CHECK(allowed && maxps4_fex_engine_call_libkernel(process,allowed,&call,1)==0);
        CHECK(maxps4_fex_engine_gnm_complete(process,captured.ticket,1)==-EINVAL);
        maxps4_fex_engine_close(process);
        std::puts("PASS FEX GNM SubmitCommandBuffers/SubmitDone/AreSubmitsAllowed: independent host handoff, explicit acknowledgment and restaging detach");
    }
    {
        auto app=orbis_execution::make_general_gnm_app();
        const maxps4_orbis_module module{"gnm.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0};
        maxps4_fex_engine* process=nullptr; CHECK(maxps4_fex_engine_open(&process)==0);
        uint64_t relocation=0, observed=0, entry=0; uint32_t backend=0; char diagnostic[512]{};
        const int prepared=maxps4_fex_engine_prepare_orbis_general(process,&module,1,"gnm.elf",&relocation,diagnostic,sizeof(diagnostic));
        if(prepared) std::fprintf(stderr,"prepare GNM: %s\n",diagnostic);
        CHECK(prepared==0);
        uint8_t registers[48]{};
        for(size_t i=0;i<12;++i) for(size_t j=0;j<4;++j) registers[4*i+j]=uint8_t((i==1 ? 0U : uint32_t(0x100+i))>>(8*j));
        const uint64_t data=orbis_execution::data+relocation;
        CHECK(maxps4_fex_engine_write_ps4(process,data+0x300,registers,28)==0);
        CHECK(maxps4_fex_engine_write_ps4(process,data+0x340,registers,48)==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&backend,&entry)==0);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 && report.exited==1 && report.exit_status==123);
        std::vector<uint32_t> combined;
        const size_t offsets[]={0x100,0x180,0x240,0x280,0x2c0}, lengths[]={29,40,10,7,3};
        for(size_t i=0;i<5;++i) {
            std::vector<uint8_t> bytes(lengths[i]*4);
            CHECK(maxps4_fex_engine_read_ps4(process,data+offsets[i],bytes.data(),bytes.size())==0);
            std::vector<uint32_t> words(lengths[i]);
            for(size_t n=0;n<lengths[i];++n) for(size_t j=0;j<4;++j) words[n]|=uint32_t(bytes[n*4+j])<<(j*8);
            if(i==3) CHECK(maxps4::gpu::planPM4Triangles(words.data(),words.size()).drawCount==1);
            combined.insert(combined.end(),words.begin(),words.end());
        }
        const auto commands=maxps4::gpu::interpretPM4(combined.data(),combined.size());
        CHECK(commands.valid && commands.writes.size()==20 && commands.draws.size()==2);
        CHECK(commands.draws[0].indexed && commands.draws[0].indexAddress==data+0x400 && commands.draws[0].indexCount==6);
        CHECK(!commands.draws[1].indexed && commands.draws[1].indexCount==3 && commands.draws[1].writesBefore==19);
        CHECK(commands.writes[0].offset==0x48 && commands.writes[0].value==0x100);
        CHECK(commands.writes.back().offset==0x2aa && commands.writes.back().value==0x1003f);
        CHECK(maxps4::gpu::planPM4Triangles(combined.data(),combined.size()).error==maxps4::gpu::PM4MetalError::unsupported_state);
        maxps4_fex_engine_close(process);
        std::puts("PASS real GNM NIDs on FEX: VS/PS350/VGT register packets and indexed/auto draws; no shader execution claimed");
    }
    {
        auto app = orbis_execution::make_general_user_service_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf", app.data(), app.size(), orbis_execution::app_bias, nullptr, 0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process) == 0);
        char diagnostic[512]{};
        uint64_t relocation = 0, observed = 0, logical_entry = 0;
        uint32_t cpu = 0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process, modules, 1, "app.elf", &relocation, diagnostic, sizeof(diagnostic));
        if (prepared) std::fprintf(stderr, "prepare UserService: %s\n", diagnostic);
        CHECK(prepared == 0 && maxps4_fex_engine_initialize(process) == 0);
        CHECK(maxps4_fex_engine_run_orbis(process, &observed, &cpu, &logical_entry) == 0);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process, &report) == 0 && report.exited == 1 && report.exit_status == 111);
        uint8_t users[32]{}, name[7]{};
        CHECK(maxps4_fex_engine_read_ps4(process, orbis_execution::data + relocation + 0x40, users, sizeof(users)) == 0);
        CHECK(users[0] == 1 && users[8] == 1 && value(users + 12) == UINT64_MAX && users[24] == 0 && users[28] == 1);
        CHECK(maxps4_fex_engine_read_ps4(process, orbis_execution::data + relocation + 0x68, name, sizeof(name)) == 0 && !std::memcmp(name, "MaxPS4", 7));
        CHECK(maxps4_fex_engine_prepare_orbis_general(process, modules, 1, "app.elf", &relocation, diagnostic, sizeof(diagnostic)) == 0);
        const uint32_t get_user = maxps4_ps4_resolve_libkernel("CdWp0oHWGr0", "libSceUserService", "libSceUserService", 1);
        maxps4_ps4_kernel_call call{}; call.rdi = orbis_execution::data + relocation + 0x40;
        CHECK(get_user && maxps4_fex_engine_call_libkernel(process, get_user, &call, 1) == int64_t(0x80960002) - 0x100000000LL);
        maxps4_fex_engine_close(process);
        std::puts("PASS FEX local UserService: lifecycle, exact library errors, user/list/name/login event, re-init -> exit(111), then restaging resets service");
    }
    {
        auto app = orbis_execution::make_general_pad_videoout_app();
        const maxps4_orbis_module module{"pad.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process) == 0);
        char diagnostic[512]{}; uint64_t relocation=0, observed=0, entry=0; uint32_t backend=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,&module,1,"pad.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare Pad/VideoOut: %s\n",diagnostic);
        CHECK(prepared == 0 && maxps4_fex_engine_initialize(process) == 0);
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        CHECK(maxps4_fex_engine_set_time_limit(process,10000) == 0);
#else
        // VIXL has no native signal watchdog; the test process is externally
        // bounded by run-bounded.py, like the existing simulator wait tests.
        CHECK(maxps4_fex_engine_set_time_limit(process,10000) == -ENOTSUP);
#endif
        CHECK(maxps4_fex_engine_pad_update(process,0x4080,0,255,128,128,12,244,1) == 0);
        CHECK(maxps4_fex_engine_pad_update(process,1,0,0,0,0,0,0,1) == -EINVAL);
        size_t frames=0; bool saw_changed=false; int presenter_status=0;
        std::thread presenter([&] {
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (frames < 120 && std::chrono::steady_clock::now()<end) {
                maxps4_videoout_frame frame{}; uint8_t pixels[8]{};
                const int status=maxps4_fex_engine_videoout_snapshot(process,&frame,pixels,sizeof(pixels));
                if (status == -EAGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                if (status || frame.width!=2 || frame.height!=1 || frame.byte_count!=8 ||
                    uint32_t(value(pixels))!=(frames < 60 ? 0xff004080U : 0xff000020U)) { presenter_status=-1; return; }
                if (frames == 59 && maxps4_fex_engine_pad_update(process,0x20,128,128,128,128,0,0,1)) { presenter_status=-2; return; }
                if (frames >= 60) saw_changed=true;
                if (maxps4_fex_engine_videoout_complete(process,frame.ticket,1)) { presenter_status=-3; return; }
                ++frames;
            }
        });
        const int ran=maxps4_fex_engine_run_orbis(process,&observed,&backend,&entry);
        presenter.join();
        CHECK(ran==0 && presenter_status==0 && frames==120 && saw_changed);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 && report.exited==1 && report.exit_status==123);
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,&module,1,"pad.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        const auto service=maxps4_ps4_resolve_libkernel("xk0AcarP3V4","libScePad","libScePad",1);
        maxps4_ps4_kernel_call call{}; call.rdi=1;
        CHECK(service && maxps4_fex_engine_call_libkernel(process,service,&call,1)==int64_t(0x80920005U)-0x100000000LL);
        maxps4_fex_engine_close(process);
        std::puts("PASS FEX Pad input changes guest pixels across 120 explicitly completed VideoOut frames; restaging clears Pad");
    }
    for (bool selfContained : {false, true}) {
        auto app = orbis_execution::make_general_videoout_app(selfContained);
        const maxps4_orbis_module modules[] = {
            {"app.elf", app.data(), app.size(), orbis_execution::app_bias, nullptr, 0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process) == 0);
        char diagnostic[512]{};
        uint64_t relocation = 0, observed = 0, logical_entry = 0;
        uint32_t cpu = 0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process, modules, 1, "app.elf", &relocation, diagnostic, sizeof(diagnostic));
        if (prepared) std::fprintf(stderr, "prepare VideoOut: %s\n", diagnostic);
        CHECK(prepared == 0);
        const uint64_t data = orbis_execution::data + relocation;
        maxps4_ps4_kernel_call create{};
        create.rdi = data + 0x40; create.rsi = data + 0x50;
        const uint32_t create_service = maxps4_ps4_resolve_libkernel("D0OdFMjp46I", "libkernel", "libkernel", 1);
        if (!selfContained) CHECK(create_service && maxps4_fex_engine_call_libkernel(process, create_service, &create, 1) == 0);
        CHECK(maxps4_fex_engine_initialize(process) == 0);
        maxps4_videoout_frame captured{};
        uint8_t captured_pixels[8]{};
        int64_t completion = -1;
        std::thread presenter([&] {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < end) {
                const int status = maxps4_fex_engine_videoout_snapshot(process, &captured, captured_pixels, sizeof(captured_pixels));
                if (status == 0) { completion = maxps4_fex_engine_videoout_complete(process, captured.ticket, 1); return; }
                if (status != -EAGAIN) { completion = status; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const int ran = maxps4_fex_engine_run_orbis(process, &observed, &cpu, &logical_entry);
        presenter.join();
        CHECK(ran == 0 && completion == 0);
        CHECK(captured.width == 2 && captured.height == 1 && captured.pitch_in_pixels == 2 && captured.byte_count == 8);
        const uint8_t expected[] = {0x56, 0x34, 0x12, 0xff, 0xef, 0xcd, 0xab, 0xff};
        CHECK(captured.pixel_format == 0x80002200u && !std::memcmp(captured_pixels, expected, sizeof(expected)));
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process, &report) == 0 && report.exited == 1 && report.exit_status == 123);
        uint8_t argument[8]{};
        CHECK(maxps4_fex_engine_read_ps4(process, data + 0xa0, argument, sizeof(argument)) == 0 && value(argument) == uint64_t(int64_t(-123)));
        CHECK(maxps4_fex_engine_videoout_complete(process, captured.ticket, 1) != 0);
        maxps4_fex_engine_close(process);
        std::puts("PASS FEX writes linear VideoOut pixels, passes stack pitch/fourth flip argument, waits for explicit host confirmation -> exit(123); no Metal presentation claimed");
    }
    {
        auto app = orbis_execution::make_general_graphics_wait_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf", app.data(), app.size(), orbis_execution::app_bias, nullptr, 0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process) == 0);
        char diagnostic[512]{};
        uint64_t relocation = 0, observed = 0, logical_entry = 0;
        uint32_t cpu = 0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process, modules, 1, "app.elf", &relocation, diagnostic, sizeof(diagnostic)) == 0);
        const uint64_t data = orbis_execution::data + relocation;
        maxps4_ps4_kernel_call create{};
        create.rdi = data + 0x40; create.rsi = data + 0x48;
        const uint32_t create_service = maxps4_ps4_resolve_libkernel("D0OdFMjp46I", "libkernel", "libkernel", 1);
        CHECK(create_service != 0 && maxps4_fex_engine_call_libkernel(process, create_service, &create, 1) == 0);
        uint8_t wire[8]{};
        CHECK(maxps4_fex_engine_read_ps4(process, data + 0x40, wire, sizeof(wire)) == 0);
        const uint64_t queue = value(wire);
        const uint32_t add = maxps4_ps4_resolve_libkernel("b0xyllnVY-I", "libSceGnmDriver", "libSceGnmDriver", 1);
        const uint32_t remove = maxps4_ps4_resolve_libkernel("PVT+fuoS9gU", "libSceGnmDriver", "libSceGnmDriver", 1);
        CHECK(add != 0 && remove != 0);
        maxps4_ps4_kernel_call registration{};
        registration.rdi = queue; registration.rsi = 0x40; registration.rdx = 789;
        CHECK(maxps4_fex_engine_call_libkernel(process, add, &registration, 1) == 0);
        uint64_t ticket = 0;
        CHECK(maxps4_fex_engine_graphics_event_ticket(process, queue, 0x40, &ticket) == 0 && ticket != 0);
        CHECK(maxps4_fex_engine_initialize(process) == 0);
        int64_t completion = -1;
        std::thread backend([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            completion = maxps4_fex_engine_complete_graphics_event(process, ticket);
        });
        const int ran = maxps4_fex_engine_run_orbis(process, &observed, &cpu, &logical_entry);
        backend.join();
        CHECK(ran == 0 && completion == 0);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process, &report) == 0);
        CHECK(report.exited == 1 && report.exit_status == 64);
        uint8_t event[32]{};
        CHECK(maxps4_fex_engine_read_ps4(process, data + 0x60, event, sizeof(event)) == 0);
        CHECK(value(event) == 0x40 && event[8] == 0xf2 && event[9] == 0xff && value(event + 16) == 0x40 && value(event + 24) == 789);
        CHECK(maxps4_fex_engine_call_libkernel(process, remove, &registration, 1) == 0);
        CHECK(maxps4_fex_engine_complete_graphics_event(process, ticket) != 0);
        maxps4_fex_engine_close(process);
        std::puts("PASS general FEX waits for explicit GNM backend completion from another host thread -> exit(64); no GPU work simulated");
    }
    {
        auto app = orbis_execution::make_general_equeue_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf", app.data(), app.size(), orbis_execution::app_bias, nullptr, 0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process) == 0);
        char diagnostic[512]{};
        uint64_t relocation = 0, observed = 0, logical_entry = 0;
        uint32_t cpu = 0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process, modules, 1, "app.elf", &relocation, diagnostic, sizeof(diagnostic));
        if (prepared) std::fprintf(stderr, "prepare equeue: %s\n", diagnostic);
        CHECK(prepared == 0 && maxps4_fex_engine_initialize(process) == 0);
        CHECK(maxps4_fex_engine_run_orbis(process, &observed, &cpu, &logical_entry) == 0);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process, &report) == 0);
        CHECK(report.exited == 1 && report.exit_status == 123);
        maxps4_fex_engine_close(process);
        std::puts("PASS general FEX event queue: create, edge trigger, wait (RCX/R8), consumed edge timeout, delete -> exit(123)");
    }
    {
        // libc support: pthread_once as guest code (init runs once), and the
        // POSIX convention: nanosleep(1e9 ns) returns -1, __Error() gives EINVAL.
        auto app = orbis_execution::make_general_libc_support_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
        if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
        CHECK(prepared==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 || ran);
        std::printf("general libc support: run=%d exited=%u status=%d\n",ran,report.exited,report.exit_status);
        CHECK(ran==0 && report.exited==1 && report.exit_status==122);
        std::puts("PASS general scePthreadOnce (guest code, once) + nanosleep -1 + __Error() EINVAL -> exit(122)");
        maxps4_fex_engine_close(process);
    }
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
    {
        auto provider=orbis_execution::make(true), app=orbis_execution::make_general_unaligned_app();
        const char* dependency[]={"provider.prx"};
        const maxps4_orbis_module modules[]={
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process=nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0 && maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_set_time_limit(process,10000)==0);
        char diagnostic[512]{}; uint64_t relocation=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,2,"app.elf",&relocation,diagnostic,sizeof diagnostic)==0);
        for(unsigned repeat=0;repeat<2;++repeat) {
            uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
            const int ran=maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
            CHECK(ran==0 && observed==0x55667789 && cpu==MAXPS4_CPU_FEX_ARM64_JIT);
            uint8_t bytes[8]{};
            CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::unaligned_cell+relocation,bytes,8)==0 && value(bytes)==0x55667788);
            CHECK(maxps4_fex_engine_read_ps4(process,orbis_execution::unaligned_store+relocation,bytes,8)==0 && value(bytes)==0x55667789);
            maxps4_fex_guest_fault fault{};
            CHECK(maxps4_fex_engine_last_guest_fault(process,&fault)==-ENOENT);
        }
        maxps4_fex_engine_close(process);
        std::puts("PASS native ARM64 unaligned TSO loads/stores and LOCK ADD/CMPXCHG across cache lines, repeated execution");
    }
    for (int fault = 0; fault < 2; ++fault) {
        // A child thread that never ends: when the entry exits, the run stops it
        // (no hang). One that faults while the entry joins it ends the run with
        // the child's contained fault. The same engine then runs again.
        auto app = orbis_execution::make_general_stuck_thread_app(fault);
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_set_time_limit(process,10000)==0);
        for (int repeat=0;repeat<2;++repeat) {
            uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
            const auto started = std::chrono::steady_clock::now();
            const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            maxps4_fex_guest_fault guest{};
            const int faulted = maxps4_fex_engine_last_guest_fault(process,&guest);
            std::printf("general stuck thread (%s): run=%d signal=%d address=0x%llx elapsed=%lldms\n",
                fault ? "fault" : "spin",ran,guest.signal,(unsigned long long)guest.address,(long long)elapsed);
            CHECK(elapsed < 5000); // Well before the 10 s watchdog.
            if (fault) {
                CHECK(ran==-EFAULT && faulted==0 && (guest.signal==SIGSEGV || guest.signal==SIGBUS) && !guest.address);
            } else {
                maxps4_fex_cpu_report report{};
                CHECK(ran==0 && faulted==-ENOENT && maxps4_fex_engine_last_cpu_report(process,&report)==0);
                CHECK(report.exited==1 && report.exit_status==0);
            }
        }
        std::puts(fault ? "PASS general guest thread fault during scePthreadJoin -> contained, run returns -EFAULT" :
                          "PASS general entry exits while a guest thread spins -> thread stopped and joined");
        maxps4_fex_engine_close(process);
    }
    for (int spins = 0; spins < 2; ++spins) {
        // The entry hands off with scePthreadExit (the worker carries on), or
        // spins (the worker's exit function stops it): both exit with 77.
        auto app = orbis_execution::make_general_handoff_app(spins);
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_set_time_limit(process,10000)==0);
        for (int repeat=0;repeat<2;++repeat) {
            uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
            const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
            maxps4_fex_cpu_report report{};
            CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 || ran);
            std::printf("general handoff (%s): run=%d exited=%u status=%d\n",spins ? "entry spins" : "scePthreadExit",
                ran,report.exited,report.exit_status);
            CHECK(ran==0 && report.exited==1 && report.exit_status==77);
        }
        std::puts(spins ? "PASS general worker's exit function while the entry spins -> entry stopped, exit(77)" :
                          "PASS general entry scePthreadExit -> worker carries on -> its exit(77) ends the run");
        maxps4_fex_engine_close(process);
    }
    {
        // Producer/consumer: scePthreadMutexLock + scePthreadCondWait in the
        // entry, signal from a worker, on statically initialized objects.
        auto app = orbis_execution::make_general_cond_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_set_time_limit(process,10000)==0);
        for (int repeat=0;repeat<2;++repeat) {
            uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
            const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
            maxps4_fex_cpu_report report{};
            CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 || ran);
            std::printf("general condition variable: run=%d exited=%u status=%d\n",ran,report.exited,report.exit_status);
            CHECK(ran==0 && report.exited==1 && report.exit_status==77);
        }
        std::puts("PASS general scePthreadMutexLock + scePthreadCondWait <- worker scePthreadCondSignal -> exit(77)");
        maxps4_fex_engine_close(process);
    }
    {
        // 16 threads from a detached attribute object plus one scePthreadDetach:
        // more threads than slots, so ended detached threads must give theirs back.
        auto app = orbis_execution::make_general_detached_app();
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        char diagnostic[512]{};
        uint64_t relocation=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,1,"app.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_set_time_limit(process,10000)==0);
        uint64_t observed=0, logical_entry=0; uint32_t cpu=0;
        const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
        maxps4_fex_cpu_report report{};
        CHECK(maxps4_fex_engine_last_cpu_report(process,&report)==0 || ran);
        std::printf("general detached threads: run=%d exited=%u status=%d\n",ran,report.exited,report.exit_status);
        CHECK(ran==0 && report.exited==1 && report.exit_status==17);
        std::puts("PASS general 16 detached threads (attribute) + scePthreadDetach, 8 slots reused -> exit(17)");
        maxps4_fex_engine_close(process);
    }
    {
        // Contained guest faults: each run fails with a diagnostic instead of
        // killing the process, and the same engine then runs a valid image.
        auto provider = orbis_execution::make(true);
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        maxps4_fex_guest_fault fault{};
        CHECK(maxps4_fex_engine_last_guest_fault(process,&fault)==-ENOENT && !fault.signal);
        CHECK(maxps4_fex_engine_set_time_limit(process,3600*1000+1)==-EINVAL);
        CHECK(maxps4_fex_engine_set_time_limit(process,200)==0);
        for (int kind = 0; kind < 8; ++kind) {
            auto app = orbis_execution::make_general_fault_app(kind);
            const char* dependency[] = {"provider.prx"};
            const maxps4_orbis_module modules[] = {
                {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
                {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
            char diagnostic[512]{};
            uint64_t relocation=0;
            const int prepared = maxps4_fex_engine_prepare_orbis_general(process,modules,2,"app.elf",&relocation,diagnostic,sizeof(diagnostic));
            if (prepared) std::fprintf(stderr,"prepare: %s\n",diagnostic);
            CHECK(prepared==0);
            const uint64_t code = orbis_execution::entry+relocation;
            uint64_t observed=99, logical_entry=99; uint32_t cpu=99;
            const auto began = std::chrono::steady_clock::now();
            const int ran = maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-began).count();
            CHECK(maxps4_fex_engine_last_guest_fault(process,&fault)==0);
            std::printf("guest fault kind=%d: status=%d signal=%d address=0x%llx rip=0x%llx code=0x%llx elapsed=%lldms\n",kind,ran,
                fault.signal,(unsigned long long)fault.address,(unsigned long long)fault.rip,(unsigned long long)code,(long long)elapsed);
            const bool illegal = kind==2 || kind>=5;
            CHECK(ran==(kind==3 ? -ETIMEDOUT : illegal ? -EILSEQ : -EFAULT) && !observed && !cpu && !logical_entry);
            CHECK(kind==3 ? fault.signal==SIGURG && !fault.address && elapsed>=200 && elapsed<10000 :
                  illegal ? fault.signal==SIGILL || fault.signal==SIGTRAP : fault.signal==SIGSEGV || fault.signal==SIGBUS);
            CHECK(fault.rip>=code); // Entry block, or the startup stub placed after the image.
            if (kind==1) CHECK(fault.address==code);
            if (kind==4) CHECK(fault.address==code+1);
            if (kind==0) CHECK(fault.address>code);
            // #296: null dispatch contained as SIGILL code 0x4d50. A linked direct
            // CALL leaves State.rip at the caller's block (stale); the fault
            // address is the CALL target recovered from the jump thunk.
            if (kind>=5) CHECK(fault.signal==SIGILL && fault.code==0x4d50);
            if (kind==5) CHECK(fault.address==code+6 && fault.rip==code);
            if (kind==7) CHECK(fault.address==code && fault.rip==code);
        }
        // Recovery: the next image runs normally and clears the fault record.
        auto app = orbis_execution::make_general_app(9);
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        char diagnostic[512]{};
        uint64_t relocation=0, observed=0, logical_entry=0; uint32_t cpu=0;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,2,"app.elf",&relocation,diagnostic,sizeof(diagnostic))==0);
        CHECK(maxps4_fex_engine_run_orbis(process,&observed,&cpu,&logical_entry)==0 && observed==110+9+40);
        CHECK(maxps4_fex_engine_last_guest_fault(process,&fault)==-ENOENT);
        std::puts("PASS contained guest faults (stack guard read, code write, UD2) and 200 ms time limit on JMP $ -> -EFAULT/-EILSEQ/-ETIMEDOUT, then a clean run on the same engine");
        maxps4_fex_engine_close(process);
    }
#else
    {
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_open(&process)==0);
        CHECK(maxps4_fex_engine_set_time_limit(process,200)==-ENOTSUP && maxps4_fex_engine_set_time_limit(process,0)==0);
        maxps4_fex_engine_close(process);
    }
    std::puts("SKIP guest fault containment and time limit: need native ARM64 JIT code (not the VIXL simulator)");
#endif
    {
        auto provider = orbis_execution::make(true);
        auto app = orbis_execution::make_general_app(9);
        const char* dependency[] = {"provider.prx"};
        const maxps4_orbis_module modules[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        maxps4_fex_engine* process = nullptr;
        CHECK(maxps4_fex_engine_request_stop(nullptr)==-EINVAL);
        CHECK(maxps4_fex_engine_open(&process)==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        char diagnostic[512]{}; uint64_t relocation=0, value=99, entry=99; uint32_t cpu=99;
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,modules,2,"app.elf",&relocation,diagnostic,sizeof diagnostic)==0);
        CHECK(maxps4_fex_engine_request_stop(process)==0);
        CHECK(maxps4_fex_engine_run_orbis(process,&value,&cpu,&entry)==-ECANCELED && !value && !cpu && !entry);
        CHECK(maxps4_fex_engine_run_orbis(process,&value,&cpu,&entry)==-ECANCELED);
        maxps4_fex_engine_close(process);
        std::puts("PASS sticky cancellation before execution; no guest instructions run");
#if (defined(__aarch64__) || defined(__arm64__)) && !defined(VIXL_SIMULATOR)
        app = orbis_execution::make_general_fault_app(3); // infinite JMP loop
        const maxps4_orbis_module spinning[] = {
            {"app.elf",app.data(),app.size(),orbis_execution::app_bias,dependency,1},
            {"provider.prx",provider.data(),provider.size(),orbis_execution::provider_bias,nullptr,0}};
        CHECK(maxps4_fex_engine_open(&process)==0);
        CHECK(maxps4_fex_engine_initialize(process)==0);
        CHECK(maxps4_fex_engine_prepare_orbis_general(process,spinning,2,"app.elf",&relocation,diagnostic,sizeof diagnostic)==0);
        const auto start=std::chrono::steady_clock::now();
        int stop_status=-1; // joined before reading; no CHECK/return inside the worker
        std::thread stop([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop_status=maxps4_fex_engine_request_stop(process); });
        const int ran=maxps4_fex_engine_run_orbis(process,&value,&cpu,&entry);
        stop.join();
        CHECK(stop_status==0 && ran==-ECANCELED && std::chrono::steady_clock::now()-start<std::chrono::seconds(10));
        maxps4_fex_engine_close(process);
        std::puts("PASS concurrent cancellation interrupts a running native guest without closing its engine");
        for (const bool busy : {false,true}) {
            // Game threads looping on a wait the stop makes return at once; with
            // `busy` the main thread loops on calls too.
            auto spin=busy ? orbis_execution::make_general_busy_stop_app() : orbis_execution::make_general_cond_spin_app();
            const maxps4_orbis_module looping[] = {{"app.elf",spin.data(),spin.size(),orbis_execution::app_bias,nullptr,0}};
            CHECK(maxps4_fex_engine_open(&process)==0);
            CHECK(maxps4_fex_engine_prepare_orbis_general(process,looping,1,"app.elf",&relocation,diagnostic,sizeof diagnostic)==0);
            CHECK(maxps4_fex_engine_initialize(process)==0);
            std::chrono::steady_clock::time_point asked;
            int asked_status=-1;
            std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300));
                asked=std::chrono::steady_clock::now();
                asked_status=maxps4_fex_engine_request_stop(process); });
            const int looped=maxps4_fex_engine_run_orbis(process,&value,&cpu,&entry);
            const auto ended=std::chrono::steady_clock::now();
            stopper.join();
            const auto took=std::chrono::duration_cast<std::chrono::milliseconds>(ended-asked).count();
            char activity[4096]{};
            (void)maxps4_fex_engine_thread_activity(process,activity,sizeof activity);
            std::printf("stop with threads looping on a wait%s: %lld ms\n%.160s\n",busy ? ", main too" : "",(long long)took,std::strstr(activity,"Arr"));
            CHECK(asked_status==0 && looped==-ECANCELED && took<1000);
            maxps4_fex_engine_close(process);
            std::puts("PASS a stop ends threads looping on a wait at their next call");
        }
#endif
    }
    maxps4_fex_engine* engine = nullptr;
    std::puts("BEGIN real FEX context and linked Orbis execution");
    CHECK(maxps4_fex_engine_open(&engine) == 0 && engine);
    struct Close { maxps4_fex_engine* engine; ~Close() { maxps4_fex_engine_close(engine); } } close{engine};
    // Exercise the actual C ABI against a live FEX engine, without V4 mappings.
    // Fail closed: translated/unmapped addresses cannot enter direct FEX execution.
    CHECK(maxps4_fex_engine_v4_preflight(nullptr,0x1000,0x2000,4096)==-EINVAL);
    CHECK(maxps4_fex_engine_v4_preflight(engine,0x1000,0x2000,4096)==-EAGAIN);

    uint64_t entry_out = 99, result = 99;
    uint32_t imports = 99, backend = 99, calls = 99;
    auto file = minimal();
    CHECK(maxps4_fex_engine_link_ps4(engine,file.data(),file.size(),10,&entry_out,&imports) == 0);
    CHECK(entry_out == entry && imports == 1);
    CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == -EAGAIN);
    CHECK(!result && !backend && !entry_out && !calls);
    std::puts("PASS linking and pre-initialization refusal; initialize real FEX dispatcher");
    CHECK(maxps4_fex_engine_initialize(engine) == 0);
    CHECK(maxps4_fex_engine_v4_preflight(engine,0x1000,0x2000,4096)==-ENOTSUP);
    CHECK(maxps4_fex_engine_v4_preflight(engine,0,0x2000,4096)==-EINVAL);
    CHECK(maxps4_fex_engine_v4_preflight(engine,0x1000,UINT64_MAX,4096)==-EOVERFLOW);

    CHECK(maxps4_fex_engine_probe(engine,&result,&backend) == 0 && result == 42);
    std::puts("PASS existing real FEX JIT 40+2 diagnostic regression (42)");
    std::puts("PASS dispatcher initialization; begin CPU execution");
    for (size_t repeat = 0; repeat < 3; ++repeat) {
        CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == 0);
        CHECK(result == 2 && calls == 1 && entry_out == entry);
#ifdef VIXL_SIMULATOR
        CHECK(backend == MAXPS4_CPU_FEX_ARM64_SIMULATOR);
#else
        CHECK(backend == MAXPS4_CPU_FEX_ARM64_JIT);
#endif
        uint8_t output[8]{};
        CHECK(maxps4_fex_engine_read_ps4(engine,data+0x100,output,8) == 0 && value(output) == 2);
        CHECK(maxps4_fex_engine_read_ps4(engine,data+0x108,output,8) == 0 && value(output) == 1);
    }
    std::printf("PASS linked Orbis ELF -> NID -> FEX -> libkernel -> resumed ADD/store/HLT (backend=%u)\n",backend);
    // Different post-return arithmetic must produce a different observed value.
    // This detects a bridge that simply substitutes the demo's expected 2.
    file = minimal(); file[entry+14] = 7;
    CHECK(maxps4_fex_engine_link_ps4(engine,file.data(),file.size(),10,&entry_out,&imports) == 0);
    CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == 0 && result == 8 && calls == 1);
    uint8_t changed[16]{};
    CHECK(maxps4_fex_engine_read_ps4(engine,data+0x100,changed,sizeof(changed)) == 0);
    CHECK(value(changed) == 8 && value(changed+8) == 1);
    std::puts("PASS changed guest ADD produces memory=8, kernel return remains 1; no fixed demo result");
    Program preserved;
    const uint8_t saved[] = {3,5,12,13,14,15}; // RBX, RBP, R12-R15.
    for (auto reg : saved) preserved.reg(reg,0x12340000+reg);
    preserved.call("scePthreadSelf");
    for (size_t i = 0; i < sizeof(saved); ++i) preserved.store(data+0x100+i*8,saved[i]);
    preserved.bytes({0xf4}); file = preserved.elf();
    CHECK(maxps4_fex_engine_link_ps4(engine,file.data(),file.size(),1000,&entry_out,&imports) == 0);
    CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == 0 && result == 1 && calls == 1);
    uint8_t preserved_bytes[48]{};
    CHECK(maxps4_fex_engine_read_ps4(engine,data+0x100,preserved_bytes,sizeof(preserved_bytes)) == 0);
    for (size_t i = 0; i < sizeof(saved); ++i) CHECK(value(preserved_bytes+i*8) == 0x12340000ULL+saved[i]);
    std::puts("PASS System V callee-saved RBX/RBP/R12-R15 preserved through actual FEX syscall trampoline");
    file = combined();
    CHECK(maxps4_fex_engine_link_ps4(engine,file.data(),file.size(),1000,&entry_out,&imports) == 0 && imports == 5);
    char directory[] = "/tmp/maxps4-linked-XXXXXX";
    CHECK(::mkdtemp(directory));
    char host_path[512]; std::snprintf(host_path,sizeof(host_path),"%s/fixture",directory);
    const int host = ::open(host_path,O_CREAT|O_EXCL|O_WRONLY,0600);
    CHECK(host >= 0 && ::write(host,"PS4!",4) == 4 && ::close(host) == 0);
    struct Remove {
        const char* path; const char* directory;
        ~Remove() { ::unlink(path); ::rmdir(directory); }
    } remove{host_path,directory};
    CHECK(maxps4_fex_engine_mount_app0(engine,directory) == 0);
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == 0);
        CHECK(result == 0 && calls == 5);
        uint8_t output[48]{};
        CHECK(maxps4_fex_engine_read_ps4(engine,data+0x100,output,sizeof(output)) == 0);
        CHECK(value(output) == 3 && value(output+8) == 4 && value(output+16) == 0 && value(output+24) == 0 && value(output+40) == 0);
        const uint64_t allocation = value(output+32);
        CHECK(allocation >= 0x1000000000ULL);
        uint8_t bytes[16]{};
        CHECK(maxps4_fex_engine_read_ps4(engine,data+0x2000,bytes,4) == 0 && !std::memcmp(bytes,"PS4!",4));
        CHECK(maxps4_fex_engine_read_ps4(engine,data+0x2100,bytes,16) == 0);
        CHECK(value(bytes) > 0 && value(bytes+8) < 1000000000);
        const uint8_t pattern[] = {1,2,3,4};
        CHECK(maxps4_fex_engine_write_ps4(engine,allocation,pattern,sizeof(pattern)) == 0);
        CHECK(maxps4_fex_engine_read_ps4(engine,allocation,bytes,4) == 0 && !std::memcmp(bytes,pattern,4));
    }
    std::puts("PASS real FEX calls open/read/close/clock/mmap, register return, fourth and seventh ABI arguments");
    Program writable;
    writable.reg(7,data+0x180); writable.reg(6,0x602); writable.reg(2,0600);
    writable.call("sceKernelOpen"); writable.store(data+0x100);
    writable.bytes({0x48,0x89,0xc7}); // MOV RDI,RAX: use the returned descriptor.
    writable.reg(6,data+0x2000); writable.reg(2,4);
    writable.call("sceKernelWrite"); writable.store(data+0x108);
    writable.reg(7,3); writable.reg(6,data+0x2000); writable.reg(2,2); writable.reg(1,2);
    writable.call("sceKernelPwrite"); writable.store(data+0x110);
    writable.reg(7,3); writable.reg(6,data+0x2100); writable.reg(2,4); writable.reg(1,0);
    writable.call("sceKernelPread"); writable.store(data+0x118);
    writable.reg(7,3); writable.call("sceKernelFsync"); writable.store(data+0x120);
    writable.reg(7,3); writable.call("sceKernelClose"); writable.bytes({0xf4});
    file = writable.elf();
    const char savedata_path[] = "/savedata0/fixture";
    std::memcpy(file.data()+data_file+0x180,savedata_path,sizeof(savedata_path));
    std::memcpy(file.data()+data_file+0x2000,"ABCD",4);
    CHECK(maxps4_fex_engine_link_ps4(engine,file.data(),file.size(),1000,&entry_out,&imports) == 0 && imports == 6);
    CHECK(maxps4_fex_engine_mount(engine,"/savedata0",directory) == 0);
    CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == 0);
    CHECK(result == 0 && calls == 6);
    uint8_t savedata_results[40]{};
    CHECK(maxps4_fex_engine_read_ps4(engine,data+0x100,savedata_results,sizeof(savedata_results)) == 0);
    CHECK(value(savedata_results) == 3 && value(savedata_results+8) == 4 &&
          value(savedata_results+16) == 2 && value(savedata_results+24) == 4 && value(savedata_results+32) == 0);
    uint8_t savedata_bytes[4]{};
    CHECK(maxps4_fex_engine_read_ps4(engine,data+0x2100,savedata_bytes,4) == 0);
    CHECK(!std::memcmp(savedata_bytes,"ABAB",4));
    const int savedata_host = ::open(host_path,O_RDONLY);
    CHECK(savedata_host >= 0 && ::read(savedata_host,savedata_bytes,4) == 4 && ::close(savedata_host) == 0);
    CHECK(!std::memcmp(savedata_bytes,"ABAB",4));
    std::puts("PASS real FEX writes savedata, positioned I/O fourth argument, sync and matching host bytes");
    // A pointer with the numerical value of an application-host address must
    // still be treated as a logical guest pointer and return Orbis EFAULT.
    Program invalid_pointer;
    invalid_pointer.reg(7,reinterpret_cast<uintptr_t>(host_path));
    invalid_pointer.reg(6,0); invalid_pointer.reg(2,0);
    invalid_pointer.call("sceKernelOpen"); invalid_pointer.bytes({0xf4});
    file = invalid_pointer.elf();
    CHECK(maxps4_fex_engine_link_ps4(engine,file.data(),file.size(),100,&entry_out,&imports) == 0);
    CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == 0);
    CHECK(result == static_cast<uint64_t>(-2147352562LL) && calls == 1);
    CHECK(maxps4_fex_engine_link_ps4(engine,nullptr,0,100,&entry_out,&imports) != 0);
    CHECK(!entry_out && !imports);
    CHECK(maxps4_fex_engine_run_linked_ps4(engine,&result,&backend,&entry_out,&calls) == -EFAULT);
    CHECK(!result && !backend && !entry_out && !calls);
    std::puts("PASS host-pointer isolation, failed restage and cleanup; iPhone execution requires separate device validation");
}
