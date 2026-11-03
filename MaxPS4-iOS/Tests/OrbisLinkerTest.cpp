#include "../Native/OrbisLinker.hpp"
#include "../Native/OrbisLinkerBridge.h"
#include "OrbisLinkerFixture.h"
#include <iostream>
#include <cstdlib>
#include <fstream>
using Linker = maxps4::OrbisLinker;
using namespace orbis_fixture;
static size_t checks = 0;
static void check(bool ok, const char* message) { ++checks; if (!ok) { std::cerr << "check " << checks << ": " << message << '\n'; std::exit(1); } }
static uint64_t value(const Linker::Image& image, uint64_t address) {
    const auto* p = image.translate(address,8,4); check(p != nullptr,"read relocation");
    uint64_t v = 0; for (size_t i=0;i<8;++i) v |= uint64_t(p[i])<<(i*8); return v;
}
int main(int argc, char** argv) {
    auto provider = make(true), consumer = make(false);
    if (argc == 2) {
        const std::pair<const char*, const std::vector<uint8_t>*> fixtures[] = {{"provider.prx", &provider}, {"app.elf", &consumer}};
        for (const auto& fixture : fixtures) {
            std::ofstream file(std::string(argv[1]) + "/" + fixture.first, std::ios::binary);
            file.write(reinterpret_cast<const char*>(fixture.second->data()), static_cast<std::streamsize>(fixture.second->size()));
            check(bool(file), "write synthetic fixture");
        }
    }
    auto input = [](const std::string& path, const std::vector<uint8_t>& b, uint64_t bias, std::vector<std::string> deps) {
        return Linker::Input{{{path,std::move(deps)},b.data(),b.size()},bias};
    };
    auto batch = [&] { return std::vector<Linker::Input>{input("app.elf",consumer,0x100000,{"provider.prx"}),input("provider.prx",provider,0x200000,{})}; };
    Linker::Image image; std::string diagnostic;
    check(Linker::link(batch(),{},image,diagnostic),diagnostic.c_str());
    check(image.modules.size()==2 && image.modules[0].path=="provider.prx", "dependency first");
    check(value(image,0x102000)==0x201000,"NID JUMP_SLOT resolved");
    check(image.modules[1].entry==0x101000 && image.modules[0].entry==0,"guest entries");
    const auto original = consumer;
    auto reversed = batch(); std::reverse(reversed.begin(),reversed.end()); Linker::Image reordered;
    check(Linker::link(reversed,{},reordered,diagnostic),"reversed input links");
    check(reordered.modules[0].path==image.modules[0].path && value(reordered,0x102000)==value(image,0x102000),"deterministic order and bytes");
    auto reject = [&](std::vector<Linker::Input> inputs, const std::string& expected, size_t budget = 64*1024*1024) {
        check(!Linker::link(inputs,{},image,diagnostic,budget),"expected rejection");
        check(diagnostic.find(expected)!=std::string::npos,diagnostic.c_str());
        check(image.modules.size()==2 && value(image,0x102000)==0x201000,"failed link preserves complete previous image");
    };
    consumer[0x601+std::string("FixtureLibrary").size()+1+std::string("FixtureModule").size()+1] = 'B';
    reject(batch(),"unresolved NID"); consumer=original;
    auto duplicate = batch(); duplicate.push_back(input("duplicate.prx",provider,0x300000,{})); reject(duplicate,"duplicate or ambiguous export");
    auto cycle = batch(); cycle[1].file.request.dependencies={"app.elf"}; reject(cycle,"circulaire");
    auto missing = batch(); missing[0].file.request.dependencies={"absent.prx"}; reject(missing,"manquante");
    auto undeclared = batch(); undeclared[0].file.request.dependencies.clear(); reject(undeclared,"missing explicit dependency");
    put(consumer,0x900,0x20fc); reject(batch(),"relocation target"); consumer=original;
    put(consumer,0x908,(uint64_t{1}<<32)|99); reject(batch(),"unsupported x86-64 relocation"); consumer=original;
    put(consumer,0x908,(uint64_t{99}<<32)|7); reject(batch(),"invalid relocation symbol"); consumer=original;
    reject(batch(),"memory budget",1023); reject(batch(),"budget exceeds",513*1024*1024);
    put(consumer,64+56+4,4,4); reject(batch(),"relocation target"); consumer=original;
    put(consumer,64+4,7,4); reject(batch(),"writable executable"); consumer=original;
    put(consumer,0x900,0x1000); reject(batch(),"relocation target"); consumer=original;
    reject({},"invalid module count");
    auto tooMany=std::vector<Linker::Input>(129,batch()[0]); reject(tooMany,"invalid module count");
    auto tooManyDeps=batch(); tooManyDeps[0].file.request.dependencies.resize(129,"provider.prx"); reject(tooManyDeps,"request budget");
    auto giantPath=batch(); giantPath[0].file.request.dependencies={std::string(256,'a')}; reject(giantPath,"path budget");
    auto giantFile=batch(); giantFile[0].file.size=SIZE_MAX; reject(giantFile,"invalid ELF file size");
    auto truncated = batch(); truncated[0].file.size=63; reject(truncated,"chargeur");
    consumer[0]=0; reject(batch(),"ELF Orbis"); consumer=original;
    setTag(consumer,0x61000035,0x5ff); reject(batch(),"SCE table out of bounds"); consumer=original;
    setTag(consumer,0x6100003b,16); reject(batch(),"symbol entry size"); consumer=original;
    setTag(consumer,0x6100003f,47); reject(batch(),"symbol table budget or size"); consumer=original;
    put(consumer,0x818,0xffffffff,4); reject(batch(),"string offset"); consumer=original;
    for (size_t p=0x400;p<0x600;p+=16) { put(consumer,p,0x61000035); put(consumer,p+8,0); }
    reject(batch(),"duplicate dynamic tag"); consumer=original;
    // Duplicate symbol within one provider is just as ambiguous as across modules.
    setTag(provider,0x6100003f,72); std::copy(provider.begin()+0x818,provider.begin()+0x830,provider.begin()+0x830);
    reject(batch(),"duplicate or ambiguous export"); provider=make(true);
    // A later invalid relocation must not expose an earlier valid write.
    setTag(consumer,0x61000031,48); put(consumer,0x918,0x2008); put(consumer,0x920,(uint64_t{1}<<32)|99);
    reject(batch(),"unsupported x86-64 relocation");
    check(std::all_of(consumer.begin()+0x300,consumer.begin()+0x400,[](uint8_t b){return b==0;}),"caller file never modified"); consumer=original;
    // ABI arithmetic, including signed negative addends and ignored JUMP_SLOT addend.
    for (uint32_t type : {1U,6U,7U,8U}) {
        put(consumer,0x908,(type==8 ? 0 : uint64_t{1}<<32)|type); put(consumer,0x910,UINT64_MAX-7);
        Linker::Image result; check(Linker::link(batch(),{},result,diagnostic),diagnostic.c_str());
        check(value(result,0x102000)==(type==1 ? 0x200ff8 : type==8 ? 0xffff8 : 0x201000),"ABI relocation formula");
    }
    consumer=original;
    put(consumer,0x908,(uint64_t{1}<<32)|1); put(consumer,0x910,uint64_t{1}<<63); reject(batch(),"underflow");
    consumer=original;
    // RELRO is protected after relocation, not left writable for the backend.
    // NONE may contain unused, unmapped offsets/symbols and must not write.
    setTag(consumer,0x61000031,72);
    put(consumer,0x918,UINT64_MAX); put(consumer,0x920,uint64_t{UINT32_MAX}<<32);
    put(consumer,0x928,UINT64_MAX);
    put(consumer,0x930,0x2000); put(consumer,0x938,0); put(consumer,0x940,123);
    Linker::Image none; Linker::Report noneReport;
    check(Linker::link(batch(),{},none,diagnostic,4096,&noneReport),diagnostic.c_str());
    check(value(none,0x102000)==0x201000 && noneReport.ignoredRelocations==2 &&
        noneReport.relocations==1 && noneReport.validatedRelocations==1,"NONE has no write or symbol lookup");
    check(noneReport.text().find("aucune écriture) : 2")!=std::string::npos,"NONE distinct from applied relocations");
    // A real invalid relocation after NONE still rolls back the transaction.
    put(consumer,0x938,99);
    check(!Linker::link(batch(),{},none,diagnostic,4096,&noneReport) &&
        value(none,0x102000)==0x201000 && !noneReport.committed,"NONE preserves late failure rollback");
    consumer=original;
    put(consumer,64+56,0x61000010,4);
    Linker::Image relro; check(Linker::link(batch(),{},relro,diagnostic), "RELRO links");
    check(value(relro,0x102000)==0x201000 && !relro.translate(0x102000,8,2), "RELRO final read-only permissions"); consumer=original;
    // Two partially overlapping writes are rejected before either is applied.
    setTag(consumer,0x61000031,48); put(consumer,0x918,0x2004); put(consumer,0x920,(uint64_t{1}<<32)|7);
    reject(batch(), "overlapping relocation writes"); consumer=original;
    // SCE PLT RELA table has a separately verified DT_RELA encoding.
    put(consumer,0x470,0x61000029); put(consumer,0x480,0x6100002d); put(consumer,0x490,0x6100002b); put(consumer,0x498,7);
    Linker::Image plt; check(Linker::link(batch(),{},plt,diagnostic) && value(plt,0x102000)==0x201000, "SCE PLT RELA");
    put(consumer,0x498,17); reject(batch(), "relocation table encoding"); consumer=original;
    // Exact descriptor versions are part of the key; no guessed compatibility.
    setTag(consumer,0x61000015,uint64_t{1}|(uint64_t{2}<<32)|(uint64_t{1}<<48)); reject(batch(), "PRX library"); consumer=original;
    put(consumer,0x460,0xdeadbeef); reject(batch(), "unsupported dynamic tag"); consumer=original;
    put(consumer,64+3*56+8,UINT64_MAX); reject(batch(), "chargeur"); consumer=original;
    // A PRX entry in zero-filled memory is not admitted as file-backed code.
    put(provider,24,0x1100); put(provider,64+40,0x200); reject(batch(), "invalid module entry"); provider=make(true);
    put(consumer,64+2*56+32,160); reject(batch(), "unterminated dynamic table"); consumer=original;
    put(consumer,64+2*56+32,17); reject(batch(), "dynamic metadata out of bounds"); consumer=original;
    setTag(consumer,0x61000037,1); reject(batch(), "string offset"); consumer=original;
    // Missing descriptor ID, invalid NID alphabet and export outside mapped memory.
    const size_t symbolName = 0x601 + std::string("FixtureLibrary").size()+1+std::string("FixtureModule").size()+1;
    consumer[symbolName]='!'; reject(batch(), "invalid NID alphabet"); consumer=original;
    consumer[symbolName+12]='C'; reject(batch(), "missing library/module ID"); consumer=original;
    put(provider,64+40,0x200); put(provider,0x820,0x1100);
    reject(batch(),"function export outside file-backed code"); provider=make(true);
    put(provider,64+40,0x200); put(provider,0x820,0x10ff); put(provider,0x828,2);
    reject(batch(),"function export outside file-backed code"); provider=make(true);
    put(provider,0x820,0x3000); reject(batch(), "export outside valid segment"); provider=make(true);
    put(consumer,64+3*56,7,4); reject(batch(), "TLS template outside file-backed segment"); consumer=original;
    {
        // Static TLS, variant II: provider loads first (ID 1, block at TP-0x20),
        // consumer second (ID 2, block at TP-0x40).
        auto tlsProvider = makeTLS(true), tlsConsumer = makeTLS(false);
        auto tlsBatch = [&] { return std::vector<Linker::Input>{input("app.elf",tlsConsumer,0x100000,{"provider.prx"}),
                                                                input("provider.prx",tlsProvider,0x200000,{})}; };
        Linker::Image tlsImage; Linker::Report tlsReport;
        check(Linker::link(tlsBatch(),{},tlsImage,diagnostic,64*1024*1024,&tlsReport),diagnostic.c_str());
        check(tlsImage.tls.size()==2 && tlsImage.staticTLSSize==0x40 && tlsImage.staticTLSAlignment==8,"static TLS layout");
        check(tlsImage.tls[0].id==1 && tlsImage.tls[0].offset==0x20 && tlsImage.tls[0].image==0x202080 &&
              tlsImage.tls[0].fileSize==0x10 && tlsImage.tls[0].memorySize==0x20,"provider TLS block");
        check(tlsImage.tls[1].id==2 && tlsImage.tls[1].offset==0x40 && tlsImage.tls[1].image==0x102080,"consumer TLS block");
        check(value(tlsImage,0x102000)==0x201000,"non-TLS JUMP_SLOT unchanged by TLS");
        check(value(tlsImage,0x102008)==1,"DTPMOD64 imported symbol -> provider module ID");
        check(value(tlsImage,0x102010)==0xc,"DTPOFF64 = S + A inside provider block");
        check(value(tlsImage,0x102018)==uint64_t(4)-0x40,"TPOFF64 local = A - block offset (negative)");
        check(value(tlsImage,0x102020)==2,"DTPMOD64 symbol 0 -> own module ID");
        check(tlsReport.tlsModules==2 && tlsReport.text().find("TLS statique : 2 module(s) ; 64 octets")!=std::string::npos,
              "TLS report line");
        auto tlsReject = [&](const std::string& expected) {
            Linker::Image unchanged = tlsImage;
            check(!Linker::link(tlsBatch(),{},tlsImage,diagnostic),"expected TLS rejection");
            check(diagnostic.find(expected)!=std::string::npos,diagnostic.c_str());
            check(tlsImage.tls.size()==unchanged.tls.size() && value(tlsImage,0x102008)==1,"output unchanged on TLS failure");
        };
        const auto tlsOriginal = tlsConsumer;
        put(tlsConsumer,0x908,(uint64_t{2}<<32)|7); tlsReject("TLS symbol/relocation kind mismatch"); tlsConsumer=tlsOriginal;
        put(tlsConsumer,0x910+2*24,0x100); tlsReject("TLS offset outside module TLS block"); tlsConsumer=tlsOriginal;
        put(tlsConsumer,64+4*56+48,3); tlsReject("invalid PT_TLS alignment"); tlsConsumer=tlsOriginal;
        put(tlsConsumer,64+4*56+16,0x3000); tlsReject("TLS template outside file-backed segment"); tlsConsumer=tlsOriginal;
        put(tlsConsumer,64+4*56+40,0x8); tlsReject("invalid PT_TLS size"); tlsConsumer=tlsOriginal;
        put(tlsProvider,0x838,0x20); tlsReject("TLS symbol outside module TLS block"); tlsProvider=makeTLS(true);
        // A TLS relocation in a module without PT_TLS fails closed.
        auto plainTLS = makeTLS(false); put(plainTLS,56,4,2);
        tlsConsumer = plainTLS; tlsReject("without PT_TLS"); tlsConsumer=tlsOriginal;
    }
    // Above every image budget: refused before staging (the batch budget equals the image one).
    put(consumer,64+56+40,513*1024*1024); reject(batch(), "batch memory budget exceeded"); consumer=original;
    // Overflow in S+A, independently of segment-address overflow.
    put(consumer,0x908,(uint64_t{1}<<32)|1); put(consumer,0x910,INT64_MAX);
    auto addOverflow=batch(); addOverflow[1].bias=UINT64_MAX-0x10000;
    reject(addOverflow, "non-canonical"); consumer=original;
    auto overlapping=batch(); overlapping[1].bias=0x100000; reject(overlapping,"overlapping guest modules");
    auto overflow=batch(); overflow[1].bias=UINT64_MAX-0x800; reject(overflow,"guest address overflow");
    auto names=batch(); names[1].file.request.logicalPath="../provider.prx"; reject(names,"chemin");
    // HLE is explicit, type/version-specific and maps to a guest code trampoline.
    auto noExport=make(true); noExport[0x81c]=0x02;
    auto hleBatch=batch(); hleBatch[1]=input("provider.prx",noExport,0x200000,{});
    Linker::Identity id{"AAAAAAAAAAA","FixtureLibrary","FixtureModule",1,0x100,2};
    Linker::Image hleImage;
    check(Linker::link(hleBatch,{{id,0x201000}},hleImage,diagnostic),"explicit HLE guest trampoline");
    check(value(hleImage,0x102000)==0x201000,"HLE relocation");
    check(!Linker::link(hleBatch,{{id,0x201000},{id,0x201000}},hleImage,diagnostic) && diagnostic.find("ambiguous")!=std::string::npos,"duplicate HLE rejected");
    check(!Linker::link(batch(),{{id,0x201000}},hleImage,diagnostic),"HLE/export collision rejected");
    check(!Linker::link(hleBatch,{{id,0x202000}},hleImage,diagnostic),"HLE data target refused");
    id.libraryVersion=2; check(!Linker::link(hleBatch,{{id,0x201000}},hleImage,diagnostic),"HLE version mismatch unresolved");
    // System PRX exports may be protected (visibility 3); imports may not.
    provider[0x81d]=3; check(Linker::link(batch(),{},hleImage,diagnostic),diagnostic.c_str()); provider[0x81d]=0;
    consumer[0x81d]=3; check(!Linker::link(batch(),{},hleImage,diagnostic),"import visibility refused"); consumer=original;
    // HLE ignores the module version (games import libkernel 1.1 for the same functions).
    id.libraryVersion=1; id.moduleVersion=0x909;
    check(Linker::link(hleBatch,{{id,0x201000}},hleImage,diagnostic) && value(hleImage,0x102000)==0x201000,
          "HLE matches any module version");
    // A function with guest code is that routine, in the RX module "<hle-code>".
    {
        Linker::HLE routine{id, 0, 0, false, {0x31, 0xc0, 0xc3}};
        check(Linker::link(hleBatch, {routine}, hleImage, diagnostic), diagnostic.c_str());
        const auto address = value(hleImage, 0x102000);
        const auto* code = hleImage.translate(address, 3, 5);
        check(code && code[0] == 0x31 && code[2] == 0xc3 && address % 16 == 0 && !hleImage.translate(address, 1, 2),
              "HLE guest routine in RX memory");
        routine.token = 1;
        check(!Linker::link(hleBatch, {routine}, hleImage, diagnostic), "HLE routine with a token refused");
    }
    // A data import (STT_OBJECT) gets zeroed RW bytes in "<hle-data>".
    consumer[0x81c]=0x11; hleBatch[0]=batch()[0];
    Linker::Identity object=id; object.type=1;
    check(Linker::link(hleBatch,{{object,0,24}},hleImage,diagnostic),diagnostic.c_str());
    check(hleImage.data.size()==1 && hleImage.data[0].size==24 && hleImage.data[0].nid==id.nid &&
          value(hleImage,0x102000)==hleImage.data[0].address && hleImage.translate(hleImage.data[0].address,24,6) &&
          !hleImage.translate(hleImage.data[0].address,1,1),"HLE data object in RW memory");
    check(!Linker::link(hleBatch,{{object,0,Linker::dataObjectLimit+1}},hleImage,diagnostic),"oversized HLE data object");
    consumer=original;
    // Regression: unused dependency descriptors must not silently disappear.
    put(consumer,0x81c,0x02,1); put(consumer,0x81e,1,2); put(consumer,0x820,0x1000); put(consumer,0x828,1);
    setTag(consumer,0x61000031,0);
    auto unused = batch(); unused[0].file.request.dependencies.clear();
    reject(unused,"missing explicit dependency"); consumer=original;
    // Multiple providers, even with an unreferenced second module identity.
    auto second = provider;
    const size_t moduleOffset = 0x601 + std::string("FixtureLibrary").size()+1;
    second[moduleOffset] = 'S'; second[0x601] = 'S';
    consumer[0x650] = 'S';
    const std::string secondName = "SixtureModule";
    std::copy(secondName.begin(),secondName.end(),consumer.begin()+0x650); consumer[0x650+secondName.size()]=0;
    setTag(consumer,0x61000037,0x70);
    appendTag(consumer,0x6100000f,0x50 | (uint64_t{0x100}<<32) | (uint64_t{2}<<48));
    auto multiple = batch(); multiple[0].file.request.dependencies.push_back("second.prx");
    multiple.push_back(input("second.prx",second,0x300000,{}));
    Linker::Report progress; Linker::Image multi;
    check(Linker::link(multiple,{},multi,diagnostic,4096,&progress),diagnostic.c_str());
    check(multi.modules.size()==3 && progress.dependencies==2 && progress.committed,"multiple PRX metadata dependencies");
    multiple.pop_back();
    check(!Linker::link(multiple,{},multi,diagnostic,4096,&progress) && !progress.committed && multi.modules.size()==3,"missing PRX retains previous batch");
    consumer=original;
    // DT_NEEDED uses only a checked SCE string-table offset, never filename guessing.
    const std::string needed="provider.prx";
    std::copy(needed.begin(),needed.end(),consumer.begin()+0x650); consumer[0x650+needed.size()]=0;
    setTag(consumer,0x61000037,0x70); appendTag(consumer,1,0x50);
    check(Linker::link(batch(),{},multi,diagnostic,4096,&progress) && progress.dependencies==2,"DT_NEEDED closed graph");
    auto noNeeded=batch(); noNeeded[0].file.request.dependencies.clear(); reject(noNeeded,"missing explicit dependency");
    appendTag(consumer,1,0x50); reject(batch(),"duplicate DT_NEEDED"); consumer=original;
    appendTag(consumer,1,UINT64_MAX); reject(batch(),"string offset"); consumer=original;
    // A compatible identity supplied twice is ambiguous even without any exports.
    auto noSymbols=provider; setTag(noSymbols,0x6100003f,24);
    auto ambiguous=batch(); ambiguous[0].file.request.dependencies.push_back("duplicate.prx");
    ambiguous.push_back(input("duplicate.prx",noSymbols,0x300000,{})); reject(ambiguous,"ambiguous PRX module");
    setTag(provider,0x6100000d,uint64_t(moduleOffset-0x600)|(uint64_t{0x200}<<32)|(uint64_t{1}<<48));
    reject(batch(),"incompatible"); provider=make(true);
    // Static executable preparation needs no invented dynamic tables/imports.
    put(consumer,64+3*56,0x12345,4); reject(batch(),"unsupported runtime program header"); consumer=original;
    auto standalone=makeStatic(); Linker::Image staticImage;
    check(Linker::link({input("static.elf",standalone,0x400000,{})},{},staticImage,diagnostic,4096,&progress),diagnostic.c_str());
    check(progress.committed && progress.imports==0 && progress.relocations==0 && progress.memoryBytes==0x300,"static report counts");
    check(staticImage.translate(0x402100,0x100,6) && value(staticImage,0x402100)==0,"BSS is zero and writable");
    check(!staticImage.translate(0x401000,1,2) && !staticImage.translate(0x402000,1,1),"guest W/X permissions");
    check(!staticImage.translate(UINT64_MAX,8,4) && !staticImage.translate(0x4021ff,2,4),"exact memory boundaries");
    auto snapshot=staticImage.mappings(); const auto* owned=snapshot[0].data;
    auto moved=std::move(staticImage); check(moved.translate(snapshot[0].address,1,4)==owned,"mapping ownership follows moved image");
    // Invalid later module/relocation yields no committed counters or writes.
    setTag(consumer,0x61000031,48); put(consumer,0x918,0x2008); put(consumer,0x920,(uint64_t{1}<<32)|99);
    check(!Linker::link(batch(),{},moved,diagnostic,4096,&progress),"late failure with report");
    check(progress.phase=="relocations" && progress.validatedRelocations==1 && !progress.committed &&
        progress.text().find("appliquées : 0")!=std::string::npos && moved.modules.size()==1,"failure report is not a positive result"); consumer=original;
    auto nonCanonical=batch(); nonCanonical[1].bias=uint64_t{1}<<47; reject(nonCanonical,"non-canonical");
    auto edgeCanonical=batch(); edgeCanonical[1].bias=(uint64_t{1}<<47)-0x2000; reject(edgeCanonical,"non-canonical");
    // Every truncation before the last required metadata byte must fail safely.
    for (size_t n=0;n<original.size();++n) {
        auto shortBatch=batch(); shortBatch[0].file.size=n;
        check(!Linker::link(shortBatch,{},moved,diagnostic,4096,&progress) && !progress.committed,"truncated file rejected transactionally");
    }
    check(consumer==original,"source unchanged after successful links");
    const char* deps[]={"provider.prx"};
    maxps4_orbis_module native[]={{"app.elf",consumer.data(),consumer.size(),0x100000,deps,1},{"provider.prx",provider.data(),provider.size(),0x200000,nullptr,0}};
    maxps4_orbis_image* handle=nullptr; char report[256];
    check(maxps4_orbis_link(native,2,1024,&handle,report,sizeof(report))==0,report);
    check(maxps4_orbis_mapping_count(handle)==4,"native mappings"); maxps4_orbis_mapping mapping{};
    check(maxps4_orbis_get_mapping(handle,0,&mapping)==0 && mapping.address==0x201000 && mapping.permissions==5,"guest mapping permissions");
    uint64_t entry=0; check(maxps4_orbis_entry(handle,"app.elf",&entry)==0 && entry==0x101000,"native entry");
    uint8_t copied[8]{}; check(maxps4_orbis_read(handle,0x102000,copied,8)==0 && copied[2]==0x20,"native guest read");
    std::fill(std::begin(copied),std::end(copied),0xa5);
    check(maxps4_orbis_read(handle,0x1020ff,copied,8)<0 && copied[0]==0xa5 && copied[7]==0xa5,"failed native read preserves output");
    check(maxps4_orbis_get_mapping(handle,99,&mapping)<0,"invalid mapping index");
    maxps4_orbis_close(handle); handle=nullptr;
    char detailed[4096];
    check(maxps4_orbis_link_with_report(native,2,1024,&handle,detailed,sizeof(detailed))==0,"native detailed preparation");
    check(std::string(detailed).find("Préparation : réussie")!=std::string::npos &&
        std::string(detailed).find("Exécution : impossible")!=std::string::npos,"preparation distinct from execution");
    maxps4_orbis_close(handle); handle=nullptr;
    check(maxps4_orbis_link_with_report(native,2,1024,&handle,detailed,8)==-ENOSPC && !handle,"truncated report cannot commit image");
    native[0].size=63;
    check(maxps4_orbis_link_with_report(native,2,1024,&handle,detailed,sizeof(detailed))<0 && !handle &&
        std::string(detailed).find("image non engagée")!=std::string::npos,"native rejected report");
    native[0].size=1;
    check(maxps4_orbis_link(native,2,1024,&handle,report,sizeof(report))<0 && !handle && report[0],"failed native transaction");
    std::cout << "Orbis linker: " << checks << " checks passed\n";
}
