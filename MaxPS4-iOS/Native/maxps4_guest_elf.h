#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "OrbisLinker.hpp"

namespace maxps4 {
// A deliberately bounded position-independent ELF demonstration, not a PS4 ABI.
// Guest addresses are translated into one owned host arena; x86 code is data
// for the ARM64 translator and never needs host PROT_EXEC.
class GuestELF final {
public:
    struct Segment {
        uint64_t address, file_offset, file_size, memory_size;
        uint32_t permissions;
    };
    GuestELF() = default;
    ~GuestELF();
    GuestELF(const GuestELF&) = delete;
    GuestELF& operator=(const GuestELF&) = delete;
    int load(const uint8_t* file, size_t length, uint32_t budget);
    // Maps validated plaintext PS4 segments into the same FEX arena, without
    // instruction admission or execution. Guest addresses remain logical;
    // absolute addressing requires an address-space backend before execution.
    // Copies a linked SCE image into owned pages; admits finite, acyclic GOT
    // calls to bounded guest functions. Static TLS only (local-exec reads via
    // FS); no dynamic TLS, arbitrary addressing or HLE.
    int prepare_orbis(const OrbisLinker::Image& linked, const std::string& root,
                      uint32_t budget, std::string& diagnostic);
    // General execution: PS4 addresses ARE host addresses. Links `inputs` once
    // to measure the image, reserves that span (plus startup stub, static TLS
    // and a guarded stack) in the host, then links again with every load bias
    // moved by `relocation` (a multiple of generalAlignment) into it. No
    // instruction list and no code rewriting: absolute pointers, branches and
    // stack accesses work unmodified. ET_SCE_EXEC (fixed addresses) is refused.
    // Guest faults are not contained here: the FEX runtime contains those of
    // generated code on ARM64 hosts.
    // generalSpan: the largest image (all modules) a general reservation holds.
    // generalStack: each guest thread's stack (Unreal Engine call chains run deep).
    static constexpr uint64_t generalAlignment = 1024 * 1024, generalStack = 4 * 1024 * 1024,
                              generalSpan = uint64_t(512) * 1024 * 1024;
    // sceKernelMmap window after the stack (PS4 16 KiB pages), PROT_NONE in
    // the host until PS4VirtualMemory maps pages in it.
    // Only reserved address space: games map their heap and GPU memory there.
    // The largest of these sizes the host reserves (Unreal Engine games reserve
    // 8 GiB memory pools); generalHeap, the smallest, is always required.
    static constexpr uint64_t generalHeap = uint64_t(1024) * 1024 * 1024, generalHeapPage = 0x4000;
    static constexpr uint64_t generalHeapSizes[] = {24 * generalHeap, 16 * generalHeap, 12 * generalHeap, 8 * generalHeap,
                                                    5 * generalHeap, 3 * generalHeap, generalHeap};
    // Address space left free after the reservation when possible: about 9
    // guest threads' FEX lookup caches (fexLookupCacheBytes each). iOS gives
    // the app under 11 GiB: 3 GiB of room left Mortal Kombat: Legacy
    // Kollection (7 threads) a 5 GiB window.
    static constexpr uint64_t generalHeadroom = generalHeap;
    // What FEX reserves per guest thread for its lookup cache, as
    // scripts/patch-fexcore-lookup-cache.py sizes it: 2^34 / 4 KiB * 8 +
    // 64 MiB of block pointers + 1 Mi * 16 bytes of L1. A failed reservation
    // is not checked there: the thread would crash.
    static constexpr uint64_t fexLookupCacheBytes = 112 * 1024 * 1024;
    // Child thread slots after the mmap window (scePthreadCreate): each a
    // guarded RW region holding a stack of generalStack bytes with a private
    // copy of the static TLS (blocks, then the TCB) right above it.
    static constexpr size_t generalThreads = 64;
    int prepare_orbis_general(const std::vector<OrbisLinker::Input>& inputs,
                              const std::vector<OrbisLinker::HLE>& hle, const std::string& root,
                              std::string& diagnostic, uint64_t& relocation);
    int stage_ps4(const uint8_t* file, size_t length);
    // Admission for the bounded rebased FEX path only. Does not remove the
    // general PS4 address-space blocker or authorize arbitrary instructions.
    int admit_staged_cpu(uint32_t budget) const;
    uint32_t ps4_blockers() const { return blockers; }
    struct Execution { uint64_t rax = 0, rip = 0; uint32_t instructions = 0; };
    // Executes only loader-admitted instructions, with logical guest RIP and
    // permission-checked memory accesses. No executable host allocation.
    int interpret(uint32_t budget, Execution& output);
    void clear();
    const uint8_t* translate(uint64_t address, size_t length, uint32_t permissions) const;
    // Staging-only logical-to-host translation; segment permissions are enforced.
    const uint8_t* host_pointer(uint64_t address, size_t bytes, uint32_t permissions) const {
        return translate(address, bytes, permissions);
    }
    uint64_t entry() const { return guest_entry; }
    // Static TLS for prepare_orbis images (variant II): host address of the
    // thread control block that FS must point to, or null without PT_TLS.
    // TCB word 0 holds its own host address; blocks sit just below it.
    const uint8_t* thread_pointer_host() const {
        return thread_pointer ? translate(thread_pointer, 16, 6) : nullptr;
    }
    // HLE gate for the FEX SYSCALL callback: true only when the host RIP is
    // exactly the SYSCALL of a linker-synthesized trampoline carrying `token`.
    bool hle_gate(uint64_t host_rip, uint32_t token) const {
        for (const auto& t : thunks)
            if (t.token == token && reinterpret_cast<uintptr_t>(
                    translate(t.address + OrbisLinker::syscallOffset, 2, 5)) == host_rip) return true;
        return false;
    }
    bool has_hle() const { return !thunks.empty(); }
    // __tls_get_addr for the single static-TLS thread: host address of byte
    // `offset` of module `module`'s block, or null if the module has no block
    // or the offset is outside it. Never returns memory outside the TLS region.
    const uint8_t* tls_address(uint64_t module, uint64_t offset) const {
        return tls_address(thread_pointer, module, offset);
    }
    // The same for the thread whose TCB is at `tp` (a slot's, see prepare_thread).
    const uint8_t* tls_address(uint64_t tp, uint64_t module, uint64_t offset) const {
        for (const auto& block : tls_blocks)
            if (block.id == module)
                return offset < block.memorySize ? translate(tp - block.offset + offset, 1, 6) : nullptr;
        return nullptr;
    }
    // FEX runs the admitted code at rebased host addresses. Map a host address
    // inside this image's arena back to its logical PS4 address (for HLE
    // pointer arguments); false outside the arena. Permissions are checked
    // later by whoever dereferences the logical address.
    bool guest_address(uint64_t host, uint64_t& guest) const {
        const auto base = reinterpret_cast<uintptr_t>(arena);
        if (!arena || host < base || host - base >= arena_size) return false;
        guest = guest_base + (host - base);
        return true;
    }
    const uint8_t* entry_pointer() const { return translate(guest_entry, 1, 5); }
    // Where execution begins: the dynamic startup stub when the image has
    // initializers (prepare_orbis), otherwise the entry itself.
    uint64_t start() const { return start_address ? start_address : guest_entry; }
    const uint8_t* start_pointer() const { return translate(start(), 1, 5); }
    // General images own their RW stack inside the image (so HLE pointer
    // arguments to stack buffers are guest memory); null otherwise.
    uint8_t* stack_host(size_t& bytes) {
        bytes = static_cast<size_t>(stack_bytes);
        return stack_bytes ? const_cast<uint8_t*>(translate(stack_address, bytes, 6)) : nullptr;
    }
    bool general() const { return identity; }
    // General images enter like a PS4 process: RDI = &EntryParams (top of the
    // guest stack, argc 1, argv[0] "eboot.bin"), RSI = an exit function that
    // ends the run (service process_exit). Rewrites them and returns their
    // address, which is also the initial RSP; 0 for other images.
    static constexpr uint64_t entryParamsBytes = 64;
    uint64_t write_entry_params();
    // A program a launcher would start by sceSystemServiceLoadExec gets what the
    // launcher passes, not a program name: an empty argv[0] (Call of Duty's
    // default.elf decodes argv[0] as base64 and "eboot.bin" runs past the stack).
    void start_without_program_name() { program_named = false; }
    // General images: the sceKernelMmap window (guest = host addresses);
    // false otherwise. Not part of segments(): translate() ignores it.
    uint64_t spare_address_space() const { return spare_bytes; }
    bool heap(uint64_t& address, uint64_t& bytes) const {
        address = heap_address; bytes = heap_bytes;
        return heap_bytes != 0;
    }
    // General images: resets child slot `slot` for a new thread. Its TLS gets
    // the template bytes again and a TCB pointing at itself; returns the top of
    // its stack (16-byte aligned) and its thread pointer (0 without PT_TLS).
    bool prepare_thread(size_t slot, uint64_t& stack_top, uint64_t& tp);
    // General images: the thread-return trampoline after the exit function
    // (MOV RDI,RAX; SYSCALL thread_exit; HLT), a child's first return address.
    // 0 for other images.
    uint64_t thread_return() const { return thread_return_address; }
    // Linked HLE data objects (OrbisLinker::Image::data) and the root module's
    // PT_SCE_PROCPARAM address (0 if none), for the runtime to fill and report.
    const std::vector<OrbisLinker::DataObject>& data_objects() const { return hle_data; }
    uint64_t process_param() const { return process_param_address; }
    // The supplied modules as loaded (general images): name, address span,
    // PT_GNU_EH_FRAME and exports (NID -> address), in load order.
    struct ModuleRecord {
        std::string name;
        uint64_t base = 0, end = 0, eh_frame_hdr = 0, eh_frame_hdr_size = 0;
        std::vector<std::pair<std::string, uint64_t>> exports;
        struct Segment { uint64_t address, size; uint32_t protection; }; // PS4 READ 1, WRITE 2, EXEC 4.
        std::vector<Segment> segments;
    };
    const std::vector<ModuleRecord>& module_records() const { return records; }
    const std::vector<Segment>& segments() const { return regions; }
private:
    friend class PS4DynamicLinker;
    int prepare_linked(const OrbisLinker::Image& linked, const std::string& root,
                       uint32_t budget, std::string& diagnostic, bool mapped_identity);
    // With an arena already reserved (general mode), segments are placed in it
    // at their own addresses; otherwise a new arena is mapped for them.
    int map_segments(std::vector<Segment> parsed, const std::vector<const uint8_t*>& sources,
                     uint64_t entry);
    uint32_t blockers = 0;
    std::vector<Segment> regions;
    void* arena = nullptr;
    size_t arena_size = 0;
    bool identity = false;
    uint64_t heap_window = 0; // The mmap window size the reservation holds.
    uint64_t spare_bytes = 0; // Address space left after the reservation (256 MiB steps, 16 GiB at most).
    uint64_t stack_address = 0, stack_bytes = 0, heap_address = 0, heap_bytes = 0,
             entry_params_address = 0, threads_address = 0, thread_slot_bytes = 0,
             thread_return_address = 0;
    uint64_t guest_base = 0, guest_entry = 0, thread_pointer = 0, start_address = 0;
    std::vector<OrbisLinker::Thunk> thunks;
    std::vector<OrbisLinker::DataObject> hle_data;
    bool program_named = true;
    uint64_t process_param_address = 0;
    std::vector<ModuleRecord> records;
    std::vector<OrbisLinker::TLSBlock> tls_blocks;
    std::vector<uint8_t> tls_image; // Static TLS region as initialized (blocks + TCB).
};
// Admission guarantees a finite sequence: MOV EAX,imm32; ADD EAX,imm32;
// NOP; terminal HLT. The ELF loader also admits C7 05 disp32 imm32 stores
// after proving each target lies wholly in a guest RW/non-X segment. This
// standalone validator has no segment proof and still rejects memory operands.
// No branches, stack operations, arbitrary memory accesses or syscalls.
int validate_guest_program(const uint8_t* bytes, size_t length, uint32_t budget);
}



