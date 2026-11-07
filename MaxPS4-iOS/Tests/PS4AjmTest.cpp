// minimp3's decoder is compiled in maxps4_ps4_libkernel.cpp, linked with every test.
#include "../Native/maxps4_ps4_ajm.h"
#include "GuestELFFixture.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace maxps4;
namespace {
// MPEG-1 layer III, no CRC, 128 kbit/s, 44.1 kHz, stereo: 417-byte frames.
// Zero side information and main data decode to silence.
std::vector<uint8_t> silent_frame() {
    std::vector<uint8_t> f(417, 0);
    f[0] = 0xFF; f[1] = 0xFB; f[2] = 0x90; f[3] = 0x00;
    return f;
}
// An Info frame with a LAME tag: 3 frames, 576 samples of encoder delay.
std::vector<uint8_t> info_frame() {
    auto f = silent_frame();
    const char info[] = "Info";
    for (int i = 0; i < 4; ++i) f[36 + i] = uint8_t(info[i]);
    f[43] = 0x01;          // Frame count present.
    f[47] = 3;             // Big-endian frame count.
    const char lame[] = "LAME";
    for (int i = 0; i < 4; ++i) f[48 + i] = uint8_t(lame[i]);
    f[48 + 0x15] = 576 >> 4; f[48 + 0x16] = (576 & 0xf) << 4; f[48 + 0x17] = 0;
    return f;
}
uint64_t get(PS4GuestMemory& memory, uint64_t address, size_t width) {
    uint8_t wire[8]{};
    assert(memory.read(address, wire, width));
    return ps4::get_le(wire, width);
}
}
int main() {
    GuestELF image;
    auto file = guest_test::elf(40, 2, true);
    guest_test::put(file, 65536, 160, 8);
    assert(image.load(file.data(), file.size(), 6) == 0);
    PS4GuestMemory memory(image);
    PS4Ajm ajm(memory);
    constexpr uint64_t context_at = 0x10000, instance_at = 0x10004, batch_id_at = 0x10008, batch = 0x10100,
        sideband = 0x10200, input = 0x10400, output = 0x11000, parsed = 0x13800;

    assert(ajm.module_register(1, 0, 0) == PS4Ajm::error(0x02));
    assert(ajm.initialize(1, context_at) == PS4Ajm::error(0x05));
    assert(ajm.initialize(0, context_at) == 0 && get(memory, context_at, 4) == 1);
    assert(ajm.instance_create(1, 0, 1, instance_at) == PS4Ajm::error(0x0a));
    assert(ajm.module_register(1, 0, 0) == 0 && ajm.module_register(1, 0, 0) == PS4Ajm::error(0x09));
    assert(ajm.instance_create(1, 0, 0, instance_at) == PS4Ajm::error(0x0b));
    // Version 1, 2 channels, S16.
    assert(ajm.instance_create(1, 0, 1 | (2 << 3), instance_at) == 0);
    const uint32_t instance = uint32_t(get(memory, instance_at, 4));
    assert(instance == 1 && PS4Ajm::codec_type(instance) == 0);

    // Two silent frames, decoded in one run job (multiple frames, stream sideband).
    std::vector<uint8_t> stream = silent_frame(), second = silent_frame();
    stream.insert(stream.end(), second.begin(), second.end());
    assert(memory.write(input, stream.data(), stream.size()));
    std::vector<uint8_t> noise(9216, 0x55);
    assert(memory.write(output, noise.data(), noise.size()));
    const uint64_t run_flags = 1 | (1ULL << 12) | (1ULL << 47);
    const uint64_t end = ajm.job_run(batch, instance, run_flags, input, stream.size(), output, 9216, sideband, 24, 0);
    assert(end == batch + 64);
    assert(get(memory, batch, 4) == (instance << 6) && get(memory, batch + 4, 4) == 56);
    assert(ajm.start(1, batch, 60, 0, 0, batch_id_at) == PS4Ajm::error(0x11));
    assert(ajm.start(1, batch, uint32_t(end - batch), 0, 0, batch_id_at) == 0);
    const uint32_t id = uint32_t(get(memory, batch_id_at, 4));
    // Out of input and room after two frames, as libSceAjm reports.
    assert(int32_t(get(memory, sideband, 4)) == (PS4Ajm::result_partial_input | PS4Ajm::result_not_enough_room));
    assert(get(memory, sideband + 8, 4) == 834 && get(memory, sideband + 12, 4) == 9216);
    assert(get(memory, sideband + 16, 8) == 2304);
    for (uint64_t at = 0; at < 9216; at += 512) assert(get(memory, output + at, 2) == 0);
    assert(ajm.wait(1, id, 0, 0) == 0 && ajm.wait(1, id, 0, 0) == PS4Ajm::error(0x04));
    std::puts("PASS Ajm batch: run job decodes MP3 frames, stream sideband, batch wait");

    // Reset through a control job (inline data first), then one frame only.
    const uint64_t inline_end = ajm.job_inline(batch, input, 4, batch_id_at);
    assert(inline_end == batch + 16 && get(memory, batch_id_at, 8) == batch + 8);
    const uint64_t control_end = ajm.job_control(inline_end, instance, 1 | (1ULL << 13), 0, 0, sideband, 8, 0);
    assert(control_end == inline_end + 48);
    const uint64_t one_end = ajm.job_run(control_end, instance, 1 | (1ULL << 47), input, stream.size(), output, 9216, sideband + 64, 24, 0);
    assert(ajm.start(1, batch, uint32_t(one_end - batch), 0, 0, batch_id_at) == 0);
    assert(get(memory, sideband, 4) == 0 && get(memory, sideband + 64, 4) == 0);
    assert(get(memory, sideband + 72, 4) == 417 && get(memory, sideband + 76, 4) == 4608);
    std::puts("PASS Ajm control job resets, inline buffers are skipped, one frame without the multiple-frame flag");

    // sceAjmDecMp3ParseFrame on a LAME Info frame.
    const auto lame = info_frame();
    assert(memory.write(input, lame.data(), lame.size()));
    assert(ajm.parse_mp3_frame(input, uint32_t(lame.size()), 1, parsed) == 0);
    assert(get(memory, parsed, 8) == 417 && get(memory, parsed + 8, 4) == 2 && get(memory, parsed + 12, 4) == 1152);
    assert(get(memory, parsed + 16, 4) == 128000 && get(memory, parsed + 20, 4) == 44100);
    assert(get(memory, parsed + 24, 4) == 1152 + 576 + 529 && get(memory, parsed + 28, 4) == 3);
    assert(get(memory, parsed + 32, 4) == 3 * 1152 - 576 && get(memory, parsed + 36, 4) == 1);
    assert(ajm.parse_mp3_frame(input + 1, 416, 0, parsed) == PS4Ajm::error(0x05));

    // Gapless: the Info frame's delay is skipped and the total caps the output.
    std::vector<uint8_t> gapless = lame;
    for (int i = 0; i < 3; ++i) { auto f = silent_frame(); gapless.insert(gapless.end(), f.begin(), f.end()); }
    assert(memory.write(input, gapless.data(), gapless.size()));
    const uint64_t g_end = ajm.job_control(batch, instance, 1 | (1ULL << 13), 0, 0, sideband, 8, 0);
    const uint64_t g_run = ajm.job_run(g_end, instance, 1 | (1ULL << 12) | (1ULL << 47) | (1ULL << 45),
                                       input, gapless.size(), output, 0x4000, sideband + 64, 32, 0);
    assert(ajm.start(1, batch, uint32_t(g_run - batch), 0, 0, batch_id_at) == 0);
    assert(get(memory, sideband + 72, 4) == 4 * 417);
    // 4 frames of 1152 samples less the 2257-sample skip: 2351 stereo S16 samples.
    assert(get(memory, sideband + 76, 4) == 2351 * 4 && get(memory, sideband + 80, 8) == 2351);
    assert(get(memory, sideband + 88, 4) == 3 * 1152 - 576 - 2351);
    std::puts("PASS Ajm MP3 frame parsing (LAME Info tag) and gapless skip");

    const auto stats = ajm.stats();
    assert(stats.batches == 3 && stats.instances == 1 && stats.frames == 2 + 1 + 4);
    assert(ajm.instance_destroy(1, instance) == 0 && ajm.instance_destroy(1, instance) == PS4Ajm::error(0x03));
    std::puts("PASS Ajm instances are destroyed once");
    return 0;
}
