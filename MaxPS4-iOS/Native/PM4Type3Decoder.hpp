#pragma once
// Experimental bounded PM4 type-3 packet framing for PS4 GPU command research.
// This parser does not yet interpret registers or submit GPU work.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace maxps4::gpu {
struct PM4Packet {
    std::uint8_t opcode;
    std::size_t headerWord;
    std::size_t payloadWord;
    std::size_t payloadCount;
};
enum class PM4Error { none, null_input, unsupported_packet_type, truncated_packet, too_many_packets, too_many_words };
struct PM4DecodeResult {
    PM4Error error = PM4Error::none;
    std::size_t errorWord = 0;
    std::uint32_t errorHeader = 0;
    std::vector<PM4Packet> packets;
    explicit operator bool() const { return error == PM4Error::none; }
};

// PM4 type 3 header: [31:30]=3, [29:16]=payload dword count minus one,
// [15:8]=opcode. Reject every other packet type rather than guessing.
inline PM4DecodeResult decodePM4Type3(const std::uint32_t* words,
                                      std::size_t wordCount,
                                      std::size_t maxPackets = 4096) {
    PM4DecodeResult result;
    if (wordCount && !words) {
        result.error = PM4Error::null_input;
        return result;
    }
    if (wordCount > 65536) {
        result.error = PM4Error::too_many_words;
        result.errorWord = 65536;
        return result;
    }
    std::size_t offset = 0;
    while (offset < wordCount) {
        if (result.packets.size() >= maxPackets) {
            result.error = PM4Error::too_many_packets;
            result.errorWord = offset;
            result.errorHeader = words[offset];
            result.packets.clear();
            return result;
        }
        const std::uint32_t header = words[offset];
        if ((header >> 30) != 3) {
            result.error = PM4Error::unsupported_packet_type;
            result.errorWord = offset;
            result.errorHeader = header;
            result.packets.clear();
            return result;
        }
        const std::size_t count = static_cast<std::size_t>((header >> 16) & 0x3fff) + 1;
        if (count > wordCount - offset - 1) {
            result.error = PM4Error::truncated_packet;
            result.errorWord = offset;
            result.errorHeader = header;
            result.packets.clear();
            return result;
        }
        result.packets.push_back(PM4Packet{
            static_cast<std::uint8_t>((header >> 8) & 0xff),
            offset, offset + 1, count
        });
        offset += count + 1;
    }
    return result;
}
} // namespace maxps4::gpu

