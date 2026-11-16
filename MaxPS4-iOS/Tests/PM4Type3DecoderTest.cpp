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
    const auto truncated = decodePM4Type3(commands, 2);
    assert(truncated.error == PM4Error::truncated_packet && truncated.errorWord == 0 &&
           truncated.errorHeader == commands[0]);
    assert(decodePM4Type3(nullptr, 1).error == PM4Error::null_input);
    const auto packetLimit = decodePM4Type3(commands, 5, 1);
    assert(packetLimit.error == PM4Error::too_many_packets && packetLimit.errorWord == 3 &&
           packetLimit.errorHeader == commands[3]);
    const std::uint32_t invalid[] = {0u};
    const auto unsupported = decodePM4Type3(invalid, 1);
    assert(unsupported.error == PM4Error::unsupported_packet_type && unsupported.errorWord == 0 &&
           unsupported.errorHeader == invalid[0]);
    // A malformed second packet must identify its own header, not the first one.
    const std::uint32_t secondTruncated[] = {
        (3u << 30) | (0x10u << 8), 42u,
        (3u << 30) | (2u << 16) | (0x2du << 8), 99u
    };
    const auto secondError = decodePM4Type3(secondTruncated, 4);
    assert(secondError.error == PM4Error::truncated_packet && secondError.errorWord == 2 &&
           secondError.errorHeader == secondTruncated[2] && secondError.packets.empty());
    // The packet budget is enforced before the first header is consumed.
    const auto zeroBudget = decodePM4Type3(commands, 5, 0);
    assert(zeroBudget.error == PM4Error::too_many_packets && zeroBudget.errorWord == 0 &&
           zeroBudget.errorHeader == commands[0] && zeroBudget.packets.empty());
    // Empty streams remain valid even with a zero packet budget.
    assert(decodePM4Type3(nullptr, 0, 0));
    assert(decodePM4Type3(nullptr, 0));
    assert(decodePM4Type3(commands, 65537).error == PM4Error::too_many_words);
}

