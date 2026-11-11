#include "../Native/OrbisPRXPreparation.hpp"
#include "../Native/OrbisAnalysisBridge.h"
#include "../Native/maxps4_fex_runtime.h"
#include "OrbisFixture.h"
#include "PS4LinkedELFFixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <unistd.h>

int main(int argc, char** argv) {
    using namespace orbis_test;
    maxps4::OrbisMetadata m;
    auto standard = ps4_linked_test::minimal();
    assert(m.parse(standard.data(),standard.size()) && !m.sce);
    auto good = fixture();
    assert(m.parse(good.data(),good.size()));
    assert(m.type == 0xfe18 && m.sce && m.symbols.size() == 2 && m.symbols[1].defined);
    assert(m.symbols[1].library == "libA" && m.symbols[1].moduleVersion == 0x100);
    assert(m.relocations.size() == 2 && m.relocations[1].supported);
    auto kernel = fixture(true,false,true);
    assert(m.parse(kernel.data(),kernel.size(),[](const auto& s) {
        return s.moduleVersion == 0x100 && maxps4_ps4_resolve_libkernel(s.nid.c_str(),s.library.c_str(),s.module.c_str(),s.libraryVersion);
    }));
    assert(m.symbols[1].resolved && m.report().find("résolus : 1") != std::string::npos);
    auto consumer = fixture(true);
    assert(m.parse(consumer.data(),consumer.size()));
    assert(!m.symbols[1].resolved && m.symbols[1].module == "modA");
    auto tls = fixture(false,true);
    assert(m.parse(tls.data(),tls.size()) && m.report().find("TLS : initial=4 total=16") != std::string::npos);
    size_t rejection = 0;
    auto rejected = [&](std::vector<uint8_t> f) {
        ++rejection;
        if (m.parse(f.data(),f.size())) { std::fprintf(stderr,"Accepted rejection case %zu\n",rejection); std::abort(); }
        // Program headers decoded before the refusal stay for the report;
        // nothing past them (symbols, relocations) is kept.
        assert(!m.error.empty() && m.symbols.empty() && m.relocations.empty());
    };
    auto bad = good; put(bad,UINT64_MAX,32); rejected(bad);
    bad=good; put(bad,0xffff,56,2); rejected(bad);
    bad=good; put(bad,57,54,2); rejected(bad);
    bad=good; put(bad,3,64+48); rejected(bad);
    bad=good; put(bad,UINT64_MAX,64+16); rejected(bad);
    bad=good; put(bad,0x200,64+32); rejected(bad);
    bad=good; put(bad,0x1080,120+16); rejected(bad);
    bad=good; put(bad,0x1000,0xc00); rejected(bad);
    bad=good; put(bad,0x2004,0xc18); rejected(bad);
    bad=good; put(bad,(99ULL<<32)|6,0xc08); rejected(bad);
    bad=good; put(bad,0,64+2*56+32); rejected(bad);
    bad=good; put(bad,1,0x800+10*16); rejected(bad); // DT_NULL becomes an unterminated DT_NEEDED.
    bad=good; put(bad,0x500,0x808); rejected(bad); // SCE STRTAB out of range.
    bad=good; put(bad,47,0x800+4*16+8); rejected(bad); // SYMTABSZ.
    bad=good; put(bad,UINT32_MAX,0xb18,4); rejected(bad);
    bad=consumer; for (size_t i=0xa00; i<0xb00; ++i) if (bad[i]=='#') { bad[i+1]='C'; break; } rejected(bad);
    bad=good; put(bad,0x1d3d154f,0,4); rejected(bad);
    bad=tls; put(bad,0x4000,64+4*56+16); rejected(bad);
    bad=good; put(bad,1ULL<<47,64+16); rejected(bad);
    auto appendTag = [](std::vector<uint8_t>& f, uint64_t tag, uint64_t value) {
        size_t bytes = size_t(maxps4::OrbisMetadata::read(f.data()+64+2*56+32,8));
        put(f,tag,0x800+bytes-16); put(f,value,0x800+bytes-8);
        put(f,0,0x800+bytes); put(f,0,0x800+bytes+8);
        put(f,bytes+16,64+2*56+32); put(f,bytes+16,64+2*56+40);
    };
    bad=good; appendTag(bad,0x61000025,0x300); appendTag(bad,0x6100003d,16);
    assert(m.parse(bad.data(),bad.size()));
    appendTag(bad,0x6100003d,UINT64_MAX); rejected(bad); // Duplicate singleton is refused too.
    bad=good; appendTag(bad,0x61000025,0x500); appendTag(bad,0x6100003d,16); rejected(bad);
    bad=good; appendTag(bad,0x61000019,9); appendTag(bad,0x61000019,(1ULL<<48)|9);
    assert(m.parse(bad.data(),bad.size())); // Per-library attributes are repeatable and opaque.
    bad=kernel; put(bad,(1ULL<<48)|(2ULL<<32)|maxps4::OrbisMetadata::read(kernel.data()+0x800+8*16+8,4),0x800+8*16+8);
    assert(m.parse(bad.data(),bad.size(),[](const auto& s){ return maxps4_ps4_resolve_libkernel(s.nid.c_str(),s.library.c_str(),s.module.c_str(),s.libraryVersion); }));
    assert(!m.symbols[1].resolved);
    for (uint16_t type : {uint16_t(2),uint16_t(3),uint16_t(0xfe00),uint16_t(0xfe04),uint16_t(0xfe0c),uint16_t(0xfe10),uint16_t(0xfe18)}) {
        bad=good; put(bad,type,16,2); put(bad,0x1000,24); assert(m.parse(bad.data(),bad.size()));
    }
    bad=good; put(bad,0xdead,0xc08); assert(m.parse(bad.data(),bad.size()) && !m.relocations[0].supported);
    bad=good; put(bad,UINT64_MAX,0xc00); put(bad,uint64_t{UINT32_MAX}<<32,0xc08);
    assert(m.parse(bad.data(),bad.size()) && m.relocations[0].type==0 && m.relocations[0].supported);
    // ELF64 stores the RELATIVE symbol index in the unsigned high half of
    // r_info; a displayed "-8" is 0xfffffff8, not r_addend. Keep negative
    // addends valid while rejecting that malformed symbol index precisely.
    bad=good; put(bad,8,0xc08); put(bad,UINT64_MAX-7,0xc10);
    assert(m.parse(bad.data(),bad.size()) && m.relocations[0].symbol==0 && m.relocations[0].addend==-8);
    bad=good; put(bad,(uint64_t{0xfffffff8}<<32)|8,0xc08); put(bad,UINT64_MAX-7,0xc10);
    assert(!m.parse(bad.data(),bad.size()) && m.error.find("symbole 4294967288 (signé -8), addend -8")!=std::string::npos);
    // Bounded deterministic malformed-header coverage.
    for (size_t i=0; i<288; ++i) { bad=good; bad[i]^=0xff;
        if (!m.parse(bad.data(),bad.size())) assert(m.symbols.empty() && m.relocations.empty()); }

    maxps4::OrbisPRXPreparation p;
    std::vector<maxps4::OrbisPRXPreparation::Input> inputs{{"a.prx",good,0x10000},{"b.prx",consumer,0x20000}};
    assert(p.prepare(inputs) && p.modules.size()==2 && p.initializationOrder[0]==0);
    assert(maxps4::OrbisMetadata::read(p.modules[1].regions[1].bytes.data(),8)==0x11000);
    assert(maxps4::OrbisMetadata::read(p.modules[1].regions[1].bytes.data()+8,8)==0x21000);
    for (uint32_t kind : {0U,1U,6U,7U}) {
        auto variant=inputs; put(variant[1].bytes,(1ULL<<32)|kind,0xc08);
        if (kind==1) put(variant[1].bytes,UINT64_MAX,0xc10); // signed -1 addend.
        assert(p.prepare(variant));
        uint64_t expected = kind==0 ? 0 : kind==1 ? 0x10fff : 0x11000;
        assert(maxps4::OrbisMetadata::read(p.modules[1].regions[1].bytes.data(),8)==expected);
    }
    auto initialized=inputs; appendTag(initialized[0].bytes,12,0x1000);
    assert(p.prepare(initialized) && p.modules[0].metadata.init==0x1000);
    p.unload(); assert(p.modules.empty() && p.initializationOrder.empty());
    size_t failure = 0;
    auto fail = [&](auto files) { ++failure; if(p.prepare(files)) { std::fprintf(stderr,"Accepted PRX failure %zu\n",failure); std::abort(); } assert(!p.error.empty() && p.modules.empty() && p.initializationOrder.empty()); };
    fail(std::vector<maxps4::OrbisPRXPreparation::Input>{{"b.prx",consumer,0x20000}});
    auto wrong=inputs; wrong[1].base=0x10000; fail(wrong);
    wrong=inputs; wrong[1].bytes=tls; fail(wrong);
    wrong=inputs; put(wrong[1].bytes,0x8000000000000000ULL,0xc28); fail(wrong);
    wrong=inputs; put(wrong[1].bytes,0xdead,0xc08); fail(wrong);
    wrong=inputs; wrong[1].base=0x800000000000ULL; fail(wrong);
    wrong=inputs; wrong[1].bytes[0xb1d]=2; fail(wrong); // Hidden symbols cannot be interposed.
    wrong=inputs; put(wrong[1].bytes,1,0xc10); fail(wrong); // GLOB_DAT nonzero addend refused.
    wrong=inputs; put(wrong[1].bytes,0,0xb18,4); fail(wrong); // Unnamed undefined symbol cannot resolve to zero.
    wrong=inputs; wrong[1].bytes=kernel; fail(wrong); // No fake HLE trampoline during PRX preparation.
    wrong=inputs; put(wrong[1].bytes,(1ULL<<48)|(0x200ULL<<32)|maxps4::OrbisMetadata::read(consumer.data()+0x800+7*16+8,4),0x800+7*16+8); fail(wrong);
    // Cycle: module A imports itself as module B by selecting another consumer.
    auto cycle=consumer;
    for(size_t i=0xa00;i<0xb00-4;++i) if(!std::memcmp(cycle.data()+i,"modA",4)) cycle[i+3]='B';
    fail(std::vector<maxps4::OrbisPRXPreparation::Input>{{"cycle.prx",cycle,0x10000}});
    assert(p.prepare(inputs));
    wrong=inputs; wrong[1].bytes.resize(20); fail(wrong); // Clears previous success on late failure.

    char path[]="/tmp/maxps4-orbis-XXXXXX"; int fd=mkstemp(path); assert(fd>=0); close(fd);
    { std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(kernel.data()),kernel.size()); }
    std::vector<char> report(2*1024*1024);
    assert(maxps4_orbis_analysis_report(path,report.data(),report.size())==0);
    assert(std::strstr(report.data(),"NID=aI+OeCz8xrQ") && std::strstr(report.data(),"résolus : 1"));
    assert(std::strstr(report.data(),"Préparation runtime Orbis") &&
           std::strstr(report.data(),"Préparation : refusée") &&
           std::strstr(report.data(),"Exécution : impossible"));
    char small[8]; assert(maxps4_orbis_analysis_report(path,small,sizeof(small))<0 && small[7]==0);
    unlink(path);
    assert(maxps4_orbis_analysis_report(path,report.data(),report.size())<0);
    if (argc>1) { std::ofstream out(argv[1],std::ios::binary); out.write(reinterpret_cast<const char*>(kernel.data()),kernel.size()); }
    std::puts("PASS Orbis SCE metadata, qualified NID imports/exports, PRX dependencies/RELA, TLS inventory, malformed files and transactional cleanup; no execution");
}
