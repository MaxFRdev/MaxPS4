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
    std::puts("PASS PS4 Pad lifecycle, wire layout, host input, disconnect, concurrency, stale handles and local multiplayer");
}
