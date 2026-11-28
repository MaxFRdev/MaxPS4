#include "../Native/PM4Type3Decoder.hpp"
#include <cassert>
#include <cstdint>
using namespace maxps4::gpu;
int main() {
    const std::uint32_t commands[] = {
        (3u << 30) | (1u << 16) | (0x2du << 8), 12u, 34u,
        (3u << 30) | (0x10u << 8), 56u
    };
    auto ok = decodePM4Type3(commands, 5);
    assert(ok && ok.packets.size() == 2);
    assert(ok.packets[0].opcode == 0x2d && ok.packets[0].payloadCount == 2);
    assert(ok.packets[1].opcode == 0x10 && ok.packets[1].payloadWord == 4);
    assert(decodePM4Type3(commands, 2).error == PM4Error::truncated_packet);
    assert(decodePM4Type3(nullptr, 1).error == PM4Error::null_input);
    assert(decodePM4Type3(commands, 5, 1).error == PM4Error::too_many_packets);
    const std::uint32_t invalid[] = {0u};
    assert(decodePM4Type3(invalid, 1).error == PM4Error::unsupported_packet_type);
    // Type 2 fillers between and after packets are skipped.
    const std::uint32_t padded[] = {0x80000000u, commands[0], commands[1], commands[2], 0x80000000u, commands[3], commands[4], 0x80000000u};
    auto skipped = decodePM4Type3(padded, 8);
    assert(skipped && skipped.packets.size() == 2 && skipped.packets[0].payloadWord == 2 && skipped.packets[1].headerWord == 5);
    assert(decodePM4Type3(nullptr, 0));
    assert(decodePM4Type3(commands, 65537).error == PM4Error::too_many_words);
}

