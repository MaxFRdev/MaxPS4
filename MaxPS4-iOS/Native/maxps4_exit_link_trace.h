// Issue #296 (MK Trilogy): observe Arm64JITCore::ExitFunctionLink without
// changing it. The dispatcher calls it through CpuStateFrame::Pointers
// .ExitFunctionLink (`ldr x2, ...; blr x2`). MaxPS4 points that slot at a
// pass-through wrapper that:
//  * snapshots the jump thunk + ExitFunctionLinkData record before the call
//    (the thunk is executing, so its memory is mapped);
//  * calls the original with the same arguments and returns its result as is;
//  * checks the record afterwards.
//
// ExitFunctionLink itself only ever changes word 0 of the thunk (to
// `ldr x0, #0x10`) and the HostCode field. If any other field changed (thunk
// words 1-3, GuestRIP, CallerOffset), something else rewrote that memory
// during the call. On iOS, the only thing that does this is the in-place reuse
// of the 16 MiB JIT buffer (ClearCodeCache(Thread, false), reached from
// CompileCode when the buffer is full), hypothesis H2.
//
// Everything here is lock-free, allocation-free and safe to read from the
// guest fault handler.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include "maxps4_dispatch_fault.h"

namespace maxps4 {

enum class LinkRecordState : uint8_t {
    unknown = 0,   // not checked (e.g. unreadable after the call)
    intact = 1,    // byte-identical
    linked = 2,    // only FEX's own edits: word 0 -> ldr x0,#0x10 and/or HostCode
    overwritten = 3, // other fields changed: the memory was reused during the call
};
inline const char* link_record_state_name(LinkRecordState s) {
    switch (s) {
    case LinkRecordState::intact: return "intact";
    case LinkRecordState::linked: return "linked";
    case LinkRecordState::overwritten: return "overwritten";
    default: return "unknown";
    }
}
inline LinkRecordState classify_link_record(const JumpThunk& before, const JumpThunk& after) {
    if (std::memcmp(&before, &after, sizeof before) == 0) return LinkRecordState::intact;
    const bool word0_ok = after.insn[0] == before.insn[0] || after.insn[0] == 0x58000080u;
    const bool rest_same = after.insn[1] == before.insn[1] && after.insn[2] == before.insn[2] &&
                           after.insn[3] == before.insn[3] && after.guest_rip == before.guest_rip &&
                           after.caller_offset == before.caller_offset;
    return word0_ok && rest_same ? LinkRecordState::linked : LinkRecordState::overwritten;
}

// The last ExitFunctionLink call of one FEX thread.
struct ExitLinkCall {
    uint64_t sequence = 0;      // global call number (0: none yet)
    uint64_t record = 0;        // ExitFunctionLinkData* (thunk + 0x10)
    uint64_t guest_rip = 0;     // Record->GuestRIP before the call: the real target
    uint64_t result = 0;        // HostCode returned to the dispatcher
    uint64_t overwrites_before = 0, overwrites_after = 0; // global overwrite count around the call
    JumpThunk after {};         // record after the call (when readable)
    LinkRecordState state = LinkRecordState::unknown;
};

// One slot per FEX thread, claimed by the InternalThreadState pointer.
class ExitLinkCalls {
public:
    static constexpr size_t slots = 64;
    struct Slot {
        std::atomic<uint64_t> owner {0};
        std::atomic<uint64_t> seq {0}; // seqlock: odd while writing
        ExitLinkCall call;
    };
    Slot* claim(uint64_t owner) {
        for (auto& s : slots_) if (s.owner.load(std::memory_order_acquire) == owner) return &s;
        for (auto& s : slots_) {
            uint64_t expected = 0;
            if (s.owner.compare_exchange_strong(expected, owner, std::memory_order_acq_rel)) return &s;
        }
        return nullptr; // more than 64 threads: this one is not traced
    }
    const Slot* find(uint64_t owner) const {
        for (auto& s : slots_) if (owner && s.owner.load(std::memory_order_acquire) == owner) return &s;
        return nullptr;
    }
    // Only the owning thread writes its slot.
    static void store(Slot& s, const ExitLinkCall& c) {
        s.seq.fetch_add(1, std::memory_order_acq_rel);
        s.call = c;
        s.seq.fetch_add(1, std::memory_order_release);
    }
    // From the same thread (fault handler) the copy is always consistent.
    static bool load(const Slot& s, ExitLinkCall& out) {
        const uint64_t a = s.seq.load(std::memory_order_acquire);
        if (a & 1) return false;
        out = s.call;
        return s.seq.load(std::memory_order_acquire) == a && out.sequence;
    }
    void release(uint64_t owner) {
        for (auto& s : slots_) {
            uint64_t expected = owner;
            if (owner && s.owner.load(std::memory_order_acquire) == owner) {
                s.seq.store(0, std::memory_order_relaxed);
                s.call = ExitLinkCall{};
                s.owner.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
            }
        }
    }
    std::atomic<uint64_t> calls {0}, zero_results {0}, overwrites {0}, unreadable {0};
private:
    Slot slots_[slots];
};

} // namespace maxps4
