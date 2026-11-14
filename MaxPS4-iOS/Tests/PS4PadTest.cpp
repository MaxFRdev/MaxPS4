#include "../Native/maxps4_ps4_pad.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
#include <thread>
using namespace maxps4;
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 4096, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Pad pad(memory), other(memory);
    constexpr uint64_t output = 0x10000;
    assert(pad.open(1,0,0,0) == PS4Pad::error(5));
    assert(pad.initialize() == 0 && pad.initialize() == 0);
    assert(pad.open(-1,0,0,0) == ps4::signed32(0x809b0001U));
    assert(pad.open(2,0,0,0) == ps4::signed32(0x809b0081U));
    assert(pad.open(1,2,0,0) == PS4Pad::error(2));
    assert(pad.open(1,0,1,0) == PS4Pad::error(2));
    assert(pad.open(1,0,0,UINT64_MAX) == PS4Pad::error(1));
    const int32_t handle = int32_t(pad.open(1,0,0,0)); assert(handle > 0);
    assert(pad.open(1,0,0,0) == PS4Pad::error(4));
    assert(pad.get_handle(1,0,0) == handle && pad.get_handle(2,0,0) == PS4Pad::error(8));
    assert(pad.read(handle,output,1,true) == 0);
    uint8_t bytes[128]{};
    assert(memory.read(output,bytes,120));
    assert(bytes[4] == 128 && bytes[7] == 128 && bytes[76] == 0);
    assert(ps4::get_le(bytes+24,4) == 0x3f800000);
    assert(pad.update(0x4080,{{0,255,127,129}},12,244,true) == 0);
    assert(pad.update(0x4000,{{0,255,127,129}},12,244,true) == 0); // 0x80 tapped before any read.
    assert(pad.update(1,{{128,128,128,128}},0,0,true) == -22);
    assert(pad.read(handle,0,1,true) == PS4Pad::error(1));
    assert(pad.read(handle,output,0,false) == PS4Pad::error(1));
    assert(pad.read(handle,output,65,false) == PS4Pad::error(1));
    assert(pad.read(handle,UINT64_MAX,1,true) == PS4Pad::error(1));
    assert(pad.read(handle,image.entry(),1,true) == PS4Pad::error(1));
    assert(pad.read(handle,output,64,false) == 1);
    assert(memory.read(output,bytes,128));
    assert(ps4::get_le(bytes,4) == 0x4080 && bytes[4] == 0 && bytes[5] == 255);
    assert(bytes[8] == 12 && bytes[9] == 244 && bytes[76] == 1 && bytes[104] == 1);
    assert(ps4::get_le(bytes+80,8) > 0 && ps4::get_le(bytes+120,8) == 0);
    assert(pad.read(handle,output,1,true) == 0 && memory.read(output,bytes,4) && ps4::get_le(bytes,4) == 0x4000);
    assert(other.read(handle,output,1,true) == PS4Pad::error(3));
    assert(pad.information(handle,output) == 0 && memory.read(output,bytes,28));
    assert(ps4::get_le(bytes+4,2) == 1920 && ps4::get_le(bytes+6,2) == 950);
    assert(bytes[11] == 1 && bytes[12] == 1);
    std::thread producer([&] { for (int i=0;i<1000;++i) assert(pad.update(0x4000,{{1,2,3,4}},5,6,true) == 0); });
    for (int i=0;i<1000;++i) assert(pad.read(handle,output,1,true) == 0);
    producer.join();
    assert(pad.update(0xffff,{{1,2,3,4}},255,255,false) == -22);
    assert(pad.update(0,{{1,2,3,4}},255,255,false) == 0);
    assert(pad.read(handle,output,1,true) == 0 && memory.read(output,bytes,120));
    assert(!ps4::get_le(bytes,4) && bytes[4] == 128 && bytes[8] == 0 && bytes[76] == 0);
    assert(pad.close(handle) == 0 && pad.close(handle) == PS4Pad::error(3));
    const auto second = pad.open(1,0,0,0); assert(second > handle);
    pad.reset(); assert(pad.initialize() == 0);
    assert(pad.open(1,0,0,0) > second);
    assert(pad.read(int32_t(second),output,1,true) == PS4Pad::error(3));
    {
        // Local multiplayer: one virtual pad per local user, never mixed.
        PS4Pad multi(memory);
        assert(multi.initialize() == 0 && multi.logged_in_users() == 1);
        const int32_t p1 = int32_t(multi.open(1,0,0,0)); assert(p1 > 0);
        assert(multi.open(2,0,0,0) == ps4::signed32(0x809b0081U));   // no player 2 yet
        assert(multi.get_handle(2,0,0) == PS4Pad::error(8));
        // A disconnected report before any connection does not log player 2 in.
        assert(multi.update_player(1,0x2000,{{1,2,3,4}},5,6,false) == 0 && multi.logged_in_users() == 1);
        assert(multi.update_player(4,0,{{128,128,128,128}},0,0,true) == -22);      // users 1..4 only
        assert(multi.update_player(1,1,{{128,128,128,128}},0,0,true) == -22);      // invalid button bit
        // Player 2's controller appears: user 2 logs in and opens its own pad.
        assert(multi.update_player(1,0x2000,{{10,20,30,40}},50,60,true) == 0 && multi.logged_in_users() == 3);
        const int32_t p2 = int32_t(multi.open(2,0,0,0)); assert(p2 > 0 && p2 != p1);
        assert(multi.open(2,0,0,0) == PS4Pad::error(4) && multi.get_handle(2,0,0) == p2 && multi.get_handle(1,0,0) == p1);
        assert(multi.open(3,0,0,0) == ps4::signed32(0x809b0081U) && multi.open(5,0,0,0) == ps4::signed32(0x809b0081U));
        assert(multi.update(0x4000,{{200,201,202,203}},7,8,true) == 0);
        uint8_t one[120]{}, two[120]{};
        assert(multi.read(p1,output,1,true) == 0 && memory.read(output,one,120));
        assert(multi.read(p2,output+0x100,1,true) == 0 && memory.read(output+0x100,two,120));
        assert(ps4::get_le(one,4) == 0x4000 && one[4] == 200 && one[8] == 7 && one[76] == 1);
        assert(ps4::get_le(two,4) == 0x2000 && two[4] == 10 && two[7] == 40 && two[8] == 50 && two[9] == 60 && two[76] == 1);
        // Simultaneous input from both controllers: each read keeps its own bits.
        std::thread both([&] {
            for (int i=0;i<1000;++i) {
                assert(multi.update(0x4000 | (i & 1 ? 0x10 : 0),{{1,1,1,1}},0,0,true) == 0);
                assert(multi.update_player(1,0x2000 | (i & 1 ? 0x80 : 0),{{2,2,2,2}},0,0,true) == 0);
            }
        });
        for (int i=0;i<1000;++i) {
            uint8_t a[120]{}, b[120]{};
            assert(multi.read(p1,output,1,true) == 0 && memory.read(output,a,120));
            assert(multi.read(p2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120));
            assert(!(ps4::get_le(a,4) & 0x20a0) && !(ps4::get_le(b,4) & 0x4010));
        }
        both.join();
        // The producer may finish after the loop's last read: its final update
        // (i = 999: player 1 0x4010, player 2 0x2080) can still hold presses no
        // read has seen, and PS4Pad reports such a press once (see the
        // reproduction below). Consume them with one read per pad, so the exact
        // states checked next do not depend on thread timing (it passed on
        // Windows by chance and failed on the Linux runners, #310).
        assert(multi.read(p1,output,1,true) == 0 && multi.read(p2,output+0x100,1,true) == 0);
        // Player 2 disconnects: its pad reads as disconnected, user 2 stays
        // logged in and keeps its handle; player 1 is unaffected.
        assert(multi.update_player(1,0x2000,{{9,9,9,9}},9,9,false) == 0);
        assert(multi.connected(1) == false && multi.logged_in_users() == 3 && multi.get_handle(2,0,0) == p2);
        assert(multi.read(p2,output+0x100,1,true) == 0 && memory.read(output+0x100,two,120));
        assert(ps4::get_le(two,4) == 0 && two[4] == 128 && two[8] == 0 && two[76] == 0);
        assert(multi.information(p2,output+0x200) == 0 && memory.read(output+0x200,two,28) && two[12] == 0);
        assert(multi.update(0x4000,{{1,1,1,1}},0,0,true) == 0);
        assert(multi.read(p1,output,1,true) == 0 && memory.read(output,one,120) && one[76] == 1 && ps4::get_le(one,4) == 0x4000);
        // Reconnection: same handle, connected again, connection count 2.
        assert(multi.update_player(1,0x1000,{{128,128,128,128}},0,0,true) == 0 && multi.connected(1));
        assert(multi.read(p2,output+0x100,1,true) == 0 && memory.read(output+0x100,two,120));
        assert(ps4::get_le(two,4) == 0x1000 && two[76] == 1 && two[104] == 2);
        // Latency stays player 1's; closing player 2 leaves player 1 open.
        uint64_t reads = 0, total = 0, most = 0; multi.latency(reads, total, most);
        assert(multi.close(p2) == 0 && multi.read(p2,output,1,true) == PS4Pad::error(3) && multi.read(p1,output,1,true) == 0);
        // Reset: only user 1 remains; old handles are stale.
        multi.reset(); assert(multi.logged_in_users() == 1 && multi.initialize() == 0);
        assert(multi.read(p1,output,1,true) == PS4Pad::error(3) && multi.open(2,0,0,0) == ps4::signed32(0x809b0081U));
    }
    {
        // Contract behind the #310 CI failure, deterministic: a press released
        // before any read is reported by the next read, once, then the pad
        // reads its current state. Same on every player's slot.
        PS4Pad tap(memory);
        assert(tap.initialize() == 0);
        const int32_t t1 = int32_t(tap.open(1,0,0,0));
        assert(tap.update_player(1,0,{{128,128,128,128}},0,0,true) == 0);
        const int32_t t2 = int32_t(tap.open(2,0,0,0));
        uint8_t w[120]{};
        for (const int32_t handle : {t1, t2}) {
            const size_t player = handle == t1 ? 0 : 1;
            assert(tap.update_player(player,0x4000,{{128,128,128,128}},0,0,true) == 0);
            assert(tap.read(handle,output,1,true) == 0);
            assert(tap.update_player(player,0x4010,{{128,128,128,128}},0,0,true) == 0);   // 0x10 pressed...
            assert(tap.update_player(player,0x4000,{{128,128,128,128}},0,0,true) == 0);   // ...released before a read
            assert(tap.read(handle,output,1,true) == 0 && memory.read(output,w,120) && ps4::get_le(w,4) == 0x4010);
            assert(tap.read(handle,output,1,true) == 0 && memory.read(output,w,120) && ps4::get_le(w,4) == 0x4000);
            // A press already read and then released is not repeated.
            assert(tap.update_player(player,0x4010,{{128,128,128,128}},0,0,true) == 0);
            assert(tap.read(handle,output,1,true) == 0);
            assert(tap.update_player(player,0x4000,{{128,128,128,128}},0,0,true) == 0);
            assert(tap.read(handle,output,1,true) == 0 && memory.read(output,w,120) && ps4::get_le(w,4) == 0x4000);
        }
    }
    {
        // Hybrid mode (controller = player 1, touch = player 2) and mode
        // changes, as the iOS side drives them: player 2 is reported connected
        // with the touch state, and disconnected when the mode leaves hybrid.
        PS4Pad hybrid(memory);
        assert(hybrid.initialize() == 0);
        const int32_t h1 = int32_t(hybrid.open(1,0,0,0));
        // Entering hybrid: player 2 (touch) reported connected, idle.
        assert(hybrid.update_player(1,0,{{128,128,128,128}},0,0,true) == 0 && hybrid.logged_in_users() == 3);
        const int32_t h2 = int32_t(hybrid.open(2,0,0,0)); assert(h2 > 0 && h2 != h1);
        // Controller presses cross, touch holds circle + left, both at once.
        assert(hybrid.update(0x4000,{{128,128,128,128}},0,0,true) == 0);
        assert(hybrid.update_player(1,0x2000 | 0x80,{{0,128,128,128}},0,0,true) == 0);
        uint8_t a[120]{}, b[120]{};
        assert(hybrid.read(h1,output,1,true) == 0 && memory.read(output,a,120));
        assert(hybrid.read(h2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120));
        assert(ps4::get_le(a,4) == 0x4000 && ps4::get_le(b,4) == (0x2000 | 0x80) && b[4] == 0 && a[4] == 128);
        // Held touch buttons stay pressed across reads (no auto-release).
        assert(hybrid.read(h2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120) && ps4::get_le(b,4) == (0x2000 | 0x80));
        // A touch press released before the game read it is still reported once...
        assert(hybrid.update_player(1,0x1000,{{128,128,128,128}},0,0,true) == 0);
        assert(hybrid.update_player(1,0,{{128,128,128,128}},0,0,true) == 0);
        assert(hybrid.read(h2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120) && (ps4::get_le(b,4) & 0x1000));
        // ...but switching to Solo (player 2 disconnected) drops an unread
        // press: nothing surfaces after the next return to hybrid.
        assert(hybrid.update_player(1,0x8000,{{128,128,128,128}},0,0,true) == 0);
        assert(hybrid.update_player(1,0,{{128,128,128,128}},0,0,false) == 0);   // mode -> Solo
        assert(hybrid.read(h2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120) && ps4::get_le(b,4) == 0 && b[76] == 0);
        assert(hybrid.update_player(1,0x8000,{{128,128,128,128}},0,0,true) == 0); // pressed again...
        assert(hybrid.update_player(1,0,{{128,128,128,128}},0,0,false) == 0);     // ...then Solo before any read
        assert(hybrid.update_player(1,0,{{128,128,128,128}},0,0,true) == 0);      // back to hybrid, idle
        assert(hybrid.read(h2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120));
        assert(ps4::get_le(b,4) == 0 && b[76] == 1 && hybrid.get_handle(2,0,0) == h2);
        // Player 1 never saw any of player 2's buttons.
        assert(hybrid.read(h1,output,1,true) == 0 && memory.read(output,a,120) && ps4::get_le(a,4) == 0x4000);
    }
    {
        // #309 bug: in hybrid mode, touch controls sometimes acted on player 1.
        // The iOS touch callbacks sent the touch state straight to player 1
        // (pad_update) between 120 Hz polls; the next poll restored player 1's
        // controller state. PS4Pad reports a press released before any read
        // once (a short tap must reach the game), so player 1 still saw it.
        uint8_t a[120]{}, b[120]{};
        PS4Pad old(memory);
        assert(old.initialize() == 0);
        const int32_t o1 = int32_t(old.open(1,0,0,0));
        assert(old.update_player(1,0,{{128,128,128,128}},0,0,true) == 0);
        const int32_t o2 = int32_t(old.open(2,0,0,0));
        assert(old.update(0x4000,{{128,128,128,128}},0,0,true) == 0);              // poll: controller holds cross
        assert(old.read(o1,output,1,true) == 0);
        assert(old.update(0x2000,{{128,128,128,128}},0,0,true) == 0);              // old touch callback -> player 1
        assert(old.update(0x4000,{{128,128,128,128}},0,0,true) == 0);              // next poll -> controller again
        assert(old.read(o1,output,1,true) == 0 && memory.read(output,a,120));
        assert(ps4::get_le(a,4) & 0x2000);                                       // reproduced: touch circle on player 1
        // The same callback also released player 1's held cross until the poll.
        assert(old.update(0,{{128,128,128,128}},0,0,true) == 0 && old.read(o1,output,1,true) == 0 &&
               memory.read(output,a,120) && !(ps4::get_le(a,4) & 0x4000));
        (void)o2;

        // Fixed sequence (MaxPS4GameView.touchChanged): an immediate touch
        // update goes to the touch owner only (player 2 in hybrid mode), with
        // that player's whole state. Repeated, rapid and held touches,
        // controller held throughout: player 1 never sees a touch button and
        // never loses its own.
        PS4Pad fixed(memory);
        assert(fixed.initialize() == 0);
        const int32_t f1 = int32_t(fixed.open(1,0,0,0));
        assert(fixed.update_player(1,0,{{128,128,128,128}},0,0,true) == 0);
        const int32_t f2 = int32_t(fixed.open(2,0,0,0));
        const uint32_t touch_bits[] = {0x2000, 0x1000, 0x8000, 0x4000, 0x80, 0x10, 0x400, 0x800, 0x8};
        for (int round = 0; round < 200; ++round) {
            const uint32_t t = touch_bits[round % 9] | (round % 3 == 0 ? 0x100 : 0);   // some with L2 held too
            assert(fixed.update(0x4000 | 0x20,{{128,128,128,128}},0,0,true) == 0);        // poll: controller
            assert(fixed.update_player(1,t,{{0,255,128,128}},0,0,true) == 0);            // touch press
            if (round % 2) assert(fixed.update_player(1,0,{{128,128,128,128}},0,0,true) == 0); // rapid release before a read
            assert(fixed.read(f1,output,1,true) == 0 && memory.read(output,a,120));
            assert(fixed.read(f2,output+0x100,1,true) == 0 && memory.read(output+0x100,b,120));
            assert(ps4::get_le(a,4) == (0x4000 | 0x20));                               // player 1: controller only
            assert(ps4::get_le(b,4) & t);                                              // player 2: its touch (held or tapped)
            assert(!(ps4::get_le(b,4) & 0x20) || (t & 0x20));                          // no controller bit on player 2
        }
        // Status for the session report: handles, reads, and logged-in users.
        const auto s1 = fixed.status(0), s2 = fixed.status(1), s3 = fixed.status(2);
        assert(s1.logged_in && s1.handle == f1 && s1.reads == 200 && s1.buttons == (0x4000 | 0x20));
        assert(s2.logged_in && s2.connected && s2.handle == f2 && s2.reads == 200);
        assert(!s3.logged_in && s3.handle == 0 && fixed.status(9).handle == 0);
        // The user / handle association is stable across mode changes.
        assert(fixed.update_player(1,0x2000,{{128,128,128,128}},0,0,false) == 0);      // Solo: player 2 away
        assert(fixed.update_player(1,0,{{128,128,128,128}},0,0,true) == 0);            // hybrid again
        assert(fixed.get_handle(1,0,0) == f1 && fixed.get_handle(2,0,0) == f2 && fixed.status(1).connections == 2);
    }
    std::puts("PASS PS4 Pad lifecycle, wire layout, host input, disconnect, concurrency, stale handles, local multiplayer and the #309 touch routing");
}
