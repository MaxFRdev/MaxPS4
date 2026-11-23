#pragma once
#include "maxps4_ps4_guest_memory.h"
#include "maxps4_ps4_abi.h"
#ifndef MINIMP3_FLOAT_OUTPUT
#define MINIMP3_FLOAT_OUTPUT
#endif
#include "minimp3.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace maxps4 {
// libSceAjm (Audio Job Manager), after shadPS4's libraries/ajm. A game builds
// batches of jobs with the sceAjmBatchJob* helpers (they only write chunks
// into the game's batch buffer), submits them with sceAjmBatchStartBuffer and
// collects them with sceAjmBatchWait. Here a batch runs synchronously when it
// is submitted, so waiting only retires its id. Only the MP3 decoder exists
// (minimp3): Call of Duty streams its sound as MP3. Jobs of other codecs end
// with a codec error.
//
// Batch chunks: a 32-bit header (identifier in bits 0-5, payload in bits 6-25)
// then, for a job, its byte size; for flags, their low 32 bits (the payload
// holds bits 32-51); for a buffer, its size and 64-bit address.
// Job flags: version 0-2, codec 3-10, run flags 11-12 (codec info, multiple
// frames), control flags 13-15 (reset, initialize, resample), sideband flags
// 45-47 (gapless decode, format, stream).
class PS4Ajm final {
public:
    static int64_t error(uint32_t code) { return ps4::signed32(0x80930000U | code); }
    // Job results (AjmSidebandResult::result).
    static constexpr int32_t result_invalid_parameter = 0x4, result_partial_input = 0x8,
        result_not_enough_room = 0x10, result_codec_error = 0x40000000;
    static constexpr uint32_t statistics_instance = 0x80000, codec_mp3 = 0, codec_limit = 23;
    struct Stats { uint64_t batches = 0, jobs = 0, frames = 0, samples = 0, failed_jobs = 0; int32_t last_result = 0; uint32_t instances = 0; };

    explicit PS4Ajm(PS4GuestMemory& memory) : memory_(memory) {}

    // sceAjmInitialize(reserved, u32* context).
    int64_t initialize(uint64_t reserved, uint64_t out_context) {
        if (reserved || !out_context) return error(0x05);
        std::lock_guard<std::mutex> lock(lock_);
        const uint32_t id = uint32_t(contexts_.size()) + 1;
        if (!put32(out_context, id)) return error(0x05);
        contexts_.push_back(Context{});
        return 0;
    }
    // sceAjmModuleRegister(context, codec, reserved).
    int64_t module_register(uint32_t context, uint32_t codec, uint64_t reserved) {
        if (reserved) return error(0x05);
        std::lock_guard<std::mutex> lock(lock_);
        Context* c = find_context(context);
        if (!c) return error(0x02);
        if (codec >= codec_limit) return error(0x05);
        if (c->registered[codec]) return error(0x09);
        c->registered[codec] = true;
        return 0;
    }
    int64_t module_unregister(uint32_t context, uint32_t codec) {
        std::lock_guard<std::mutex> lock(lock_);
        Context* c = find_context(context);
        if (!c) return error(0x02);
        if (codec >= codec_limit || !c->registered[codec]) return error(0x0a);
        c->registered[codec] = false;
        return 0;
    }
    // sceAjmInstanceCreate(context, codec, flags, u32* instance): the slot in
    // bits 0-13 and the codec in bits 14-18. Instance flags: version 0-2,
    // channels 3-6, sample format 7-9 (S16, S32, float), gapless loop 10.
    int64_t instance_create(uint32_t context, uint32_t codec, uint64_t flags, uint64_t out_instance) {
        std::lock_guard<std::mutex> lock(lock_);
        Context* c = find_context(context);
        if (!c) return error(0x02);
        if (codec >= codec_limit || !out_instance) return error(0x05);
        if (!(flags & 7)) return error(0x0b);
        if (!c->registered[codec]) return error(0x0a);
        const uint32_t format = uint32_t(flags >> 7) & 7;
        if (format > 2) return error(0x05);
        size_t slot = 1;
        while (slot < instances_.size() && instances_[slot]) ++slot;
        if (slot >= instance_limit) return error(0x07);
        try {
            if (slot >= instances_.size()) instances_.resize(slot + 1);
            instances_[slot] = std::make_unique<Instance>();
        } catch (const std::bad_alloc&) { return error(0x06); }
        Instance& i = *instances_[slot];
        i.codec = codec; i.format = format; i.gapless_loop = (flags >> 10) & 1;
        i.reset();
        const uint32_t id = uint32_t(slot) | (codec << 14);
        if (!put32(out_instance, id)) { instances_[slot].reset(); return error(0x05); }
        return 0;
    }
    int64_t instance_destroy(uint32_t context, uint32_t instance) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!find_context(context)) return error(0x02);
        Instance* i = find_instance(instance);
        if (!i) return error(0x03);
        instances_[instance & slot_mask].reset();
        return 0;
    }
    static int64_t codec_type(uint32_t instance) { return (instance >> 14) & 0x1f; }

    // sceAjmBatchJobControlBufferRa(buffer, instance, flags, sideband input,
    // its size, sideband output, its size, return address): the batch
    // position after the job.
    uint64_t job_control(uint64_t buffer, uint32_t instance, uint64_t flags, uint64_t input, uint64_t input_size,
                         uint64_t output, uint64_t output_size, uint64_t return_address) {
        Writer w;
        if (return_address) w.buffer(return_ident, 0, return_address);
        w.buffer(input_control_ident, input_size, input);
        flags &= instance == statistics_instance ? 0x00000000C0018007ULL : 0x000060000000E7FFULL;
        w.flags(control_flags_ident, flags);
        w.buffer(output_control_ident, output_size, output);
        return w.store(memory_, buffer, instance);
    }
    // sceAjmBatchJobRunBufferRa(buffer, instance, flags, input, its size,
    // output, its size, sideband output, its size, return address).
    uint64_t job_run(uint64_t buffer, uint32_t instance, uint64_t flags, uint64_t input, uint64_t input_size,
                     uint64_t output, uint64_t output_size, uint64_t sideband, uint64_t sideband_size,
                     uint64_t return_address) {
        Writer w;
        if (return_address) w.buffer(return_ident, 0, return_address);
        w.buffer(input_run_ident, input_size, input);
        w.flags(run_flags_ident, flags & 0x0000E00000001FFFULL);
        w.buffer(output_run_ident, output_size, output);
        w.buffer(output_control_ident, sideband_size, sideband);
        return w.store(memory_, buffer, instance);
    }
    // sceAjmBatchJobRunSplitBufferRa: arrays of AjmBuffer {u8* address; u64 size}.
    uint64_t job_run_split(uint64_t buffer, uint32_t instance, uint64_t flags, uint64_t inputs, uint64_t input_count,
                           uint64_t outputs, uint64_t output_count, uint64_t sideband, uint64_t sideband_size,
                           uint64_t return_address) {
        if (input_count > 64 || output_count > 64) return 0;
        Writer w;
        if (return_address) w.buffer(return_ident, 0, return_address);
        for (uint64_t i = 0; i < input_count; ++i) {
            uint8_t pair[16];
            if (!memory_.read(inputs + 16 * i, pair, sizeof pair)) return 0;
            w.buffer(input_run_ident, ps4::get_le(pair + 8, 8), ps4::get_le(pair, 8));
        }
        w.flags(run_flags_ident, flags & 0x0000E00000001FFFULL);
        for (uint64_t i = 0; i < output_count; ++i) {
            uint8_t pair[16];
            if (!memory_.read(outputs + 16 * i, pair, sizeof pair)) return 0;
            w.buffer(output_run_ident, ps4::get_le(pair + 8, 8), ps4::get_le(pair, 8));
        }
        w.buffer(output_control_ident, sideband_size, sideband);
        return w.store(memory_, buffer, instance);
    }
    // sceAjmBatchJobInlineBuffer(buffer, data, size, const void** copy): the
    // data copied into the batch (8-byte aligned), skipped when it runs.
    uint64_t job_inline(uint64_t buffer, uint64_t data, uint64_t size, uint64_t out_address) {
        if (!buffer || size > (1u << 20)) return 0;
        const uint64_t padded = (size + 7) & ~uint64_t(7);
        std::vector<uint8_t> bytes(size_t(8 + padded), 0);
        ps4::put_le(bytes.data(), 0, inline_ident, 4);
        ps4::put_le(bytes.data(), 4, padded, 4);
        if (size && !memory_.read(data, bytes.data() + 8, size_t(size))) return 0;
        if (!memory_.write(buffer, bytes.data(), bytes.size())) return 0;
        if (out_address) {
            uint8_t wire[8]; ps4::put_le(wire, 0, buffer + 8, 8);
            if (!memory_.write(out_address, wire, 8)) return 0;
        }
        return buffer + 8 + padded;
    }

    // sceAjmBatchStartBuffer(context, batch, size, priority, AjmBatchError*,
    // u32* batch id): runs every job now.
    int64_t start(uint32_t context, uint64_t batch, uint32_t size, int32_t, uint64_t, uint64_t out_id) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!find_context(context)) return error(0x02);
        if (size & 7) return error(0x11);
        if (!out_id) return error(0x05);
        std::vector<uint8_t> bytes(size);
        if (size && !memory_.read(batch, bytes.data(), size)) return error(0x16);
        size_t at = 0;
        while (at + 8 <= bytes.size()) {
            const uint32_t header = uint32_t(ps4::get_le(bytes.data() + at, 4));
            const uint32_t job_size = uint32_t(ps4::get_le(bytes.data() + at + 4, 4));
            at += 8;
            if (job_size > bytes.size() - at) return error(0x11);
            if ((header & 0x3f) == inline_ident) { at += job_size; continue; }
            if ((header & 0x3f) != job_ident) return error(0x11);
            run_job((header >> 6) & 0xfffff, bytes.data() + at, job_size);
            at += job_size;
        }
        const uint32_t id = next_batch_++ & 0x7fffffff;
        if (!put32(out_id, id)) return error(0x05);
        pending_.push_back(id);
        if (pending_.size() > 4096) pending_.pop_front();
        ++stats_.batches;
        return 0;
    }
    // sceAjmBatchWait(context, batch, timeout, AjmBatchError*): the batch ran
    // when it was submitted.
    int64_t wait(uint32_t context, uint32_t batch, uint32_t, uint64_t) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!find_context(context)) return error(0x02);
        const auto it = std::find(pending_.begin(), pending_.end(), batch);
        if (it == pending_.end()) return error(0x04);
        pending_.erase(it);
        return 0;
    }
    int64_t cancel(uint32_t context, uint32_t batch) {
        std::lock_guard<std::mutex> lock(lock_);
        if (!find_context(context)) return error(0x02);
        return std::find(pending_.begin(), pending_.end(), batch) == pending_.end() ? error(0x04) : 0;
    }

    // sceAjmDecMp3ParseFrame(stream, size, parse_ofl, AjmDecMp3ParseFrame*):
    // u64 frame size; u32 channels, samples per channel, bitrate, sample
    // rate, encoder delay, frames, total samples, OFL type (40 bytes).
    int64_t parse_mp3_frame(uint64_t stream, uint32_t size, int32_t parse_ofl, uint64_t out) {
        if (!stream || size < 4 || !out) return error(0x05);
        std::vector<uint8_t> bytes(std::min<uint32_t>(size, 8192));
        if (!memory_.read(stream, bytes.data(), bytes.size())) return error(0x05);
        Mp3Frame f;
        if (!parse_header(bytes.data(), uint32_t(bytes.size()), parse_ofl != 0, true, f)) return error(0x05);
        uint8_t wire[40]{};
        ps4::put_le(wire, 0, f.frame_size, 8); ps4::put_le(wire, 8, f.channels, 4);
        ps4::put_le(wire, 12, f.samples_per_channel, 4); ps4::put_le(wire, 16, f.bitrate, 4);
        ps4::put_le(wire, 20, f.sample_rate, 4); ps4::put_le(wire, 24, f.encoder_delay, 4);
        ps4::put_le(wire, 28, f.num_frames, 4); ps4::put_le(wire, 32, f.total_samples, 4);
        ps4::put_le(wire, 36, f.ofl_type, 4);
        return memory_.write(out, wire, sizeof wire) ? 0 : error(0x05);
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(lock_);
        Stats s = stats_;
        s.instances = uint32_t(std::count_if(instances_.begin(), instances_.end(),
            [](const std::unique_ptr<Instance>& i) { return bool(i); }));
        return s;
    }
    void reset() {
        std::lock_guard<std::mutex> lock(lock_);
        contexts_.clear(); instances_.clear(); pending_.clear(); stats_ = {};
    }

private:
    static constexpr uint32_t job_ident = 0, input_run_ident = 1, input_control_ident = 2, control_flags_ident = 3,
        run_flags_ident = 4, return_ident = 6, inline_ident = 7, output_run_ident = 17, output_control_ident = 18;
    static constexpr uint32_t slot_mask = 0x3fff;
    static constexpr size_t instance_limit = 1024;
    static constexpr uint64_t run_codec_info = 1ULL << 11, run_multiple_frames = 1ULL << 12,
        control_reset = 1ULL << 13, control_initialize = 1ULL << 14, control_resample = 1ULL << 15,
        sideband_gapless = 1ULL << 45, sideband_format = 1ULL << 46, sideband_stream = 1ULL << 47;

    struct Context { std::array<bool, codec_limit> registered{}; };
    struct Gapless { uint32_t total_samples = 0; uint16_t skip_samples = 0, skipped_samples = 0; };
    struct Instance {
        uint32_t codec = 0, format = 0;
        bool gapless_loop = false;
        mp3dec_t decoder{};
        Gapless init, current;
        uint32_t total_samples = 0, frame_samples = 0, header = 0, channels = 0, sample_rate = 0, bitrate = 0;
        bool has_header = false;
        size_t tag_left = 0; // Bytes of an ID3v2 tag still to skip.
        void reset() {
            mp3dec_init(&decoder);
            total_samples = 0; frame_samples = 0; has_header = false; tag_left = 0;
            current = {init.total_samples, init.skip_samples, 0};
        }
        bool gapless_end() const { return init.total_samples != 0 && current.total_samples == 0; }
        uint32_t sample_bytes() const { return format == 0 ? 2 : 4; }
        // Bytes the next frame needs in the output (shadPS4 GetNextFrameSize).
        uint32_t next_frame_bytes() const {
            const uint32_t skip = std::min<uint32_t>(current.skip_samples, frame_samples);
            const uint32_t samples = init.total_samples != 0 ?
                std::min<uint32_t>(current.total_samples, frame_samples - skip) : frame_samples - skip;
            return samples * std::max<uint32_t>(channels, 1) * sample_bytes();
        }
    };
    struct Mp3Frame {
        uint64_t frame_size = 0;
        uint32_t channels = 0, samples_per_channel = 0, bitrate = 0, sample_rate = 0, encoder_delay = 0,
                 num_frames = 0, total_samples = 0, ofl_type = 0;
        uint32_t version = 0, layer = 0, protection = 0, mode = 0, mode_extension = 0, copyright = 0,
                 original = 0, emphasis = 0, padding = 0;
    };
    // Chunks of one job, written at once.
    struct Writer {
        std::vector<uint8_t> bytes = std::vector<uint8_t>(8, 0);
        void word(uint64_t value, size_t width) {
            const size_t at = bytes.size(); bytes.resize(at + width); ps4::put_le(bytes.data(), at, value, width);
        }
        void buffer(uint32_t ident, uint64_t size, uint64_t address) {
            word(ident, 4); word(uint32_t(size), 4); word(address, 8);
        }
        void flags(uint32_t ident, uint64_t flags) {
            word(ident | (uint32_t((flags >> 32) & 0xfffff) << 6), 4); word(uint32_t(flags), 4);
        }
        uint64_t store(PS4GuestMemory& memory, uint64_t at, uint32_t instance) {
            if (!at) return 0;
            ps4::put_le(bytes.data(), 0, job_ident | ((instance & 0xfffff) << 6), 4);
            ps4::put_le(bytes.data(), 4, bytes.size() - 8, 4);
            return memory.write(at, bytes.data(), bytes.size()) ? at + bytes.size() : 0;
        }
    };
    // The output run buffers of a job, filled in order.
    struct Output {
        struct Span { uint64_t address, size; };
        std::vector<Span> spans;
        size_t current = 0;
        uint64_t remaining() const {
            uint64_t total = 0;
            for (size_t i = current; i < spans.size(); ++i) total += spans[i].size;
            return total;
        }
        bool write(PS4GuestMemory& memory, const uint8_t* data, uint64_t size) {
            while (size && current < spans.size()) {
                Span& s = spans[current];
                const uint64_t n = std::min(size, s.size);
                if (!memory.write(s.address, data, size_t(n))) return false;
                s.address += n; s.size -= n; data += n; size -= n;
                if (!s.size) ++current;
            }
            return size == 0;
        }
    };

    bool put32(uint64_t address, uint32_t value) {
        uint8_t wire[4]; ps4::put_le(wire, 0, value, 4);
        return memory_.write(address, wire, 4);
    }
    Context* find_context(uint32_t id) { return id && id <= contexts_.size() ? &contexts_[id - 1] : nullptr; }
    Instance* find_instance(uint32_t id) {
        const uint32_t slot = id & slot_mask;
        return slot < instances_.size() && instances_[slot] ? instances_[slot].get() : nullptr;
    }

    // MPEG audio header (big-endian bits: sync 11, version 2, layer 2,
    // protection 1, bitrate 4, rate 2, padding 1, private 1, mode 2, mode
    // extension 2, copyright 1, original 1, emphasis 2). Tables as libSceAjm's.
    static bool parse_header(const uint8_t* p, uint32_t size, bool parse_ofl, bool layer3_only, Mp3Frame& f) {
        static constexpr uint32_t rates[4][4] = {{11025, 12000, 8000, 0}, {0, 0, 0, 0}, {22050, 24000, 16000, 0}, {44100, 48000, 32000, 0}};
        static constexpr uint32_t bitrates[4][16] = {
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 0, 0, 0, 0, 0, 0, 0}, {0},
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
            {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}};
        if (size < 4) return false;
        const uint32_t h = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        if ((h >> 21) != 0x7ff) return false;
        f.version = (h >> 19) & 3; f.layer = (h >> 17) & 3; f.protection = (h >> 16) & 1;
        if (layer3_only && f.layer != 1) return false;
        if (f.layer == 0) return false;
        f.sample_rate = rates[f.version][(h >> 10) & 3];
        f.bitrate = bitrates[f.version][(h >> 12) & 15] * 1000;
        if (!f.sample_rate || !f.bitrate) return false;
        f.padding = (h >> 9) & 1; f.mode = (h >> 6) & 3; f.mode_extension = (h >> 4) & 3;
        f.copyright = (h >> 3) & 1; f.original = (h >> 2) & 1; f.emphasis = h & 3;
        f.channels = f.mode == 3 ? 1 : 2;
        if (f.version == 3) { f.frame_size = 144 * f.bitrate / f.sample_rate + f.padding; f.samples_per_channel = 1152; }
        else { f.frame_size = 72 * f.bitrate / f.sample_rate + f.padding; f.samples_per_channel = 576; }
        f.encoder_delay = f.num_frames = f.total_samples = f.ofl_type = 0;
        if (parse_ofl) parse_extended(p, size, f);
        return true;
    }
    // Xing/Info with a LAME tag, VBRI, or Sony's FGH block: encoder delay and
    // total samples (shadPS4 ParseMp3ExtendedHeader).
    static void parse_extended(const uint8_t* begin, uint32_t size, Mp3Frame& f) {
        const uint8_t* end = begin + std::min<uint64_t>(size, f.frame_size);
        const uint8_t* p = begin + 4;
        if (p >= end) return;
        const size_t available = size_t(end - p);
        size_t bit = 0;
        auto read = [&](uint32_t n) {
            uint32_t v = 0;
            for (uint32_t i = 0; i < n; ++i, ++bit)
                v = (v << 1) | (bit / 8 < available ? (p[bit / 8] >> (7 - bit % 8)) & 1 : 0);
            return v;
        };
        if (f.protection == 0) bit += 16;
        if (f.version == 3) bit += f.mode == 3 ? 18 : 20; else bit += f.mode == 3 ? 9 : 10;
        uint32_t part23 = 0;
        const uint32_t granules = f.version == 3 ? 2 : 1;
        for (uint32_t gr = 0; gr < granules; ++gr)
            for (uint32_t ch = 0; ch < f.channels; ++ch) { part23 += read(12); bit += f.version == 3 ? 47 : 51; }
        bit += part23;
        const size_t offset = (bit + 7) / 8;
        p = offset > available ? end : p + offset;
        auto be32 = [](const uint8_t* q) { return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3]; };
        if (p + 8 <= end && (!std::memcmp(p, "Xing", 4) || !std::memcmp(p, "Info", 4))) {
            const uint8_t flags = p[7];
            const uint8_t* field = p + 8;
            uint32_t frames = 0;
            if (flags & 1) { if (field + 4 > end) return; frames = be32(field); f.num_frames = frames; field += 4; }
            if (flags & 2) { field += 4; if (field > end) return; }
            if (flags & 4) { field += 100; if (field > end) return; }
            if (flags & 8) { field += 4; if (field > end) return; }
            if (field + 0x18 <= end && !std::memcmp(field, "LAME", 4)) {
                const uint32_t delay = (uint32_t(field[0x15]) << 4) | (field[0x16] >> 4);
                const uint32_t padding = ((uint32_t(field[0x16]) & 0x0f) << 8) | field[0x17];
                if ((flags & 1) && frames) f.total_samples = frames * f.samples_per_channel - (delay + padding);
                f.encoder_delay = f.samples_per_channel + delay + 529;
                f.ofl_type = 1;
            }
        } else if (p + 26 <= end && !std::memcmp(p, "VBRI", 4)) {
            f.encoder_delay = (uint32_t(p[6]) << 8) | p[7];
            f.ofl_type = 2;
            if (f.frame_size <= size) {
                Mp3Frame next;
                if (parse_header(begin + f.frame_size, uint32_t(size - f.frame_size), true, false, next) && next.ofl_type == 3) {
                    f.encoder_delay += next.encoder_delay; f.total_samples = next.total_samples; f.ofl_type = 4;
                }
            }
        } else if (p + 10 <= end) {
            while (p + 9 < end && *p != 0xB4) ++p;
            if (p + 9 < end && *p == 0xB4) {
                uint8_t crc = 0xff;
                for (int i = 0; i < 9; ++i)
                    for (int j = 7; j >= 0; --j)
                        crc = uint8_t(((crc >> 7) & 1) != ((p[i] >> j) & 1) ? crc * 2 : (crc * 2) ^ 0x45);
                if (p[9] == crc) {
                    f.encoder_delay = (uint32_t(p[1]) << 8) | p[2];
                    f.total_samples = be32(p + 3);
                    f.ofl_type = 3;
                }
            }
        }
    }

    // One job's chunks (shadPS4 AjmJobFromBatchBuffer), then its run
    // (AjmInstance::ExecuteJob).
    void run_job(uint32_t instance_id, const uint8_t* chunks, size_t size) {
        ++stats_.jobs;
        std::vector<uint8_t> input;
        Output output;
        uint64_t flags = 0, input_control = 0, input_control_size = 0, output_control = 0, output_control_size = 0;
        bool bad = false;
        for (size_t at = 0; at + 4 <= size && !bad;) {
            const uint32_t header = uint32_t(ps4::get_le(chunks + at, 4));
            const uint32_t ident = header & 0x3f;
            if (ident == control_flags_ident || ident == run_flags_ident) {
                if (at + 8 > size) { bad = true; break; }
                flags = (uint64_t((header >> 6) & 0xfffff) << 32) | ps4::get_le(chunks + at + 4, 4);
                at += 8; continue;
            }
            if (at + 16 > size) { bad = true; break; }
            const uint64_t length = ps4::get_le(chunks + at + 4, 4), address = ps4::get_le(chunks + at + 8, 8);
            at += 16;
            switch (ident) {
            case input_run_ident:
                if (length) {
                    const size_t old = input.size();
                    if (length > (64u << 20)) { bad = true; break; }
                    input.resize(old + size_t(length));
                    if (!memory_.read(address, input.data() + old, size_t(length))) bad = true;
                }
                break;
            case input_control_ident: input_control = address; input_control_size = length; break;
            case output_run_ident: if (address && length) output.spans.push_back({address, length}); break;
            case output_control_ident: output_control = address; output_control_size = length; break;
            case return_ident: break;
            default: bad = true; break;
            }
        }
        // Sideband output: result, then stream, format, gapless decode,
        // multiple-frame count and codec information as the flags ask.
        std::vector<uint8_t> side(size_t(std::min<uint64_t>(output_control ? output_control_size : 0, 4096)), 0);
        int32_t result = 0, internal = 0;
        auto finish = [&](size_t used) {
            if (side.size() >= 8) {
                ps4::put_le(side.data(), 0, uint32_t(result), 4); ps4::put_le(side.data(), 4, uint32_t(internal), 4);
            }
            if (output_control && !side.empty()) memory_.write(output_control, side.data(), std::min(used, side.size()));
            stats_.last_result = result;
            if (result) ++stats_.failed_jobs;
        };
        if (instance_id == statistics_instance) { finish(side.size()); return; }
        Instance* inst = find_instance(instance_id);
        if (bad || !inst) { result = result_invalid_parameter | int32_t(0x80000000); finish(side.size()); return; }
        if (inst->codec != codec_mp3) { result = result_codec_error; finish(side.size()); return; }
        if (flags & control_reset) inst->reset();
        // Sideband input: format, gapless decode, resample and initialize
        // parameters as the flags ask (only gapless decode changes the run).
        if (input_control && input_control_size) {
            std::vector<uint8_t> in(size_t(std::min<uint64_t>(input_control_size, 4096)));
            if (memory_.read(input_control, in.data(), in.size())) {
                size_t at = 0;
                if ((flags & sideband_format) && at + 24 <= in.size()) at += 24;
                if ((flags & sideband_gapless) && at + 8 <= in.size()) {
                    Gapless g{uint32_t(ps4::get_le(in.data() + at, 4)), uint16_t(ps4::get_le(in.data() + at + 4, 2)), 0};
                    apply_gapless(*inst, g, result);
                }
            }
        }
        const size_t output_size = size_t(output.remaining());
        size_t consumed = 0;
        uint32_t frames = 0;
        if (!input.empty()) {
            for (;;) {
                if (inst->gapless_loop && inst->gapless_end()) { inst->current = {inst->init.total_samples, inst->init.skip_samples, 0}; inst->total_samples = 0; }
                if (!inst->gapless_end() && output.remaining() < inst->next_frame_bytes()) result |= result_not_enough_room;
                if (input.size() - consumed < 4) result |= result_partial_input;
                if (result) break;
                const int32_t r = decode_frame(*inst, input.data() + consumed, input.size() - consumed, consumed, output, frames);
                if (r) { result |= r; break; }
                if (!(flags & run_multiple_frames)) break;
            }
        }
        size_t used = 8;
        auto put = [&](size_t bytes, const uint8_t* data) {
            if (used + bytes > side.size()) return false;
            std::memcpy(side.data() + used, data, bytes); used += bytes; return true;
        };
        if (flags & sideband_stream) {
            uint8_t s[16]; ps4::put_le(s, 0, uint32_t(consumed), 4);
            ps4::put_le(s, 4, uint32_t(output_size - output.remaining()), 4); ps4::put_le(s, 8, inst->total_samples, 8);
            put(sizeof s, s);
        }
        if (flags & sideband_format) {
            uint8_t s[24]{};
            static constexpr uint32_t masks[3] = {0, 0x4, 0x3};
            ps4::put_le(s, 0, inst->channels, 4); ps4::put_le(s, 4, inst->channels <= 2 ? masks[inst->channels] : 0, 4);
            ps4::put_le(s, 8, inst->sample_rate, 4); ps4::put_le(s, 12, inst->format, 4); ps4::put_le(s, 16, inst->bitrate, 4);
            put(sizeof s, s);
        }
        if (flags & sideband_gapless) {
            uint8_t s[8]; ps4::put_le(s, 0, inst->current.total_samples, 4);
            ps4::put_le(s, 4, inst->current.skip_samples, 2); ps4::put_le(s, 6, inst->current.skipped_samples, 2);
            put(sizeof s, s);
        }
        if (flags & run_multiple_frames) { uint8_t s[8]{}; ps4::put_le(s, 0, frames, 4); put(sizeof s, s); }
        if (flags & run_codec_info) {
            // AjmSidebandDecMp3CodecInfo: the header as read (little-endian),
            // CRC flag, channel mode, mode extension, copyright, original, emphasis.
            uint8_t s[16]{};
            if (inst->has_header) {
                const uint32_t h = inst->header;
                ps4::put_le(s, 0, ((h & 0xff) << 24) | ((h & 0xff00) << 8) | ((h >> 8) & 0xff00) | (h >> 24), 4);
                s[4] = uint8_t((h >> 16) & 1); s[5] = uint8_t((h >> 6) & 3); s[6] = uint8_t((h >> 4) & 3);
                s[7] = uint8_t((h >> 3) & 1); s[8] = uint8_t((h >> 2) & 1); s[9] = uint8_t(h & 3);
            }
            put(sizeof s, s);
        }
        stats_.frames += frames;
        finish(used);
    }
    static void apply_gapless(Instance& i, const Gapless& g, int32_t& result) {
        const uint32_t processed = i.init.total_samples - i.current.total_samples;
        if (g.total_samples != 0 || g.skip_samples == 0) {
            if (g.total_samples >= processed) {
                const int64_t difference = int64_t(i.init.total_samples) - g.total_samples;
                i.init.total_samples = g.total_samples;
                i.current.total_samples = uint32_t(int64_t(i.current.total_samples) - difference);
            } else result |= result_invalid_parameter;
        }
        const uint32_t skipped = uint32_t(i.init.skip_samples - i.current.skip_samples);
        if (g.skip_samples != 0 || g.total_samples == 0) {
            if (g.skip_samples >= skipped) {
                const int32_t difference = int32_t(i.init.skip_samples) - g.skip_samples;
                i.init.skip_samples = g.skip_samples;
                i.current.skip_samples = uint16_t(int32_t(i.current.skip_samples) - difference);
            } else result |= result_invalid_parameter;
        }
    }
    // One MP3 frame from `p` (skipping an ID3v2 tag or bytes before a frame
    // header): its PCM after the gapless skip, up to the gapless total.
    int32_t decode_frame(Instance& i, const uint8_t* p, size_t size, size_t& consumed, Output& output, uint32_t& frames) {
        if (i.tag_left) {
            const size_t n = std::min(size, i.tag_left);
            consumed += n; i.tag_left -= n;
            return i.tag_left ? result_partial_input : 0;
        }
        if (size >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
            i.tag_left = 10 + ((size_t(p[6] & 0x7f) << 21) | (size_t(p[7] & 0x7f) << 14) |
                               (size_t(p[8] & 0x7f) << 7) | (p[9] & 0x7f)) + ((p[5] & 0x10) ? 10 : 0);
            return 0;
        }
        Mp3Frame f;
        size_t skip = 0;
        while (skip + 4 <= size && !parse_header(p + skip, uint32_t(std::min<size_t>(size - skip, UINT32_MAX)), false, false, f)) ++skip;
        if (skip + 4 > size) { consumed += size - std::min<size_t>(size, 3); return result_partial_input; }
        if (skip) { consumed += skip; return 0; }
        if (f.frame_size > size) return result_partial_input;
        parse_header(p, uint32_t(std::min<size_t>(size, UINT32_MAX)), true, false, f);
        i.header = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        i.has_header = true;
        i.frame_samples = f.samples_per_channel;
        i.channels = f.channels; i.sample_rate = f.sample_rate; i.bitrate = f.bitrate;
        if (f.total_samples || f.encoder_delay) {
            i.init = {f.total_samples, uint16_t(std::min<uint32_t>(f.encoder_delay, 0xffff)), 0};
            i.current = i.init;
        }
        // minimp3 keeps its bit reservoir across calls when the frame matches
        // the header it saw last: give it this frame's header, so a single
        // frame is enough (it otherwise wants following frames to sync).
        std::memcpy(i.decoder.header, p, 4);
        std::vector<float> pcm(MINIMP3_MAX_SAMPLES_PER_FRAME, 0.0f);
        mp3dec_frame_info_t info{};
        int samples = mp3dec_decode_frame(&i.decoder, p, int(f.frame_size), pcm.data(), &info);
        // A frame whose bit reservoir is not there yet (after a reset) or the
        // Xing/Info frame decodes to silence of a frame's length.
        if (samples <= 0) { samples = int(f.samples_per_channel); std::fill(pcm.begin(), pcm.end(), 0.0f); }
        const uint32_t channels = f.channels;
        consumed += size_t(f.frame_size);
        ++frames;
        uint32_t skip_samples = 0;
        if (i.current.skip_samples) {
            skip_samples = std::min<uint32_t>(uint32_t(samples), i.current.skip_samples);
            i.current.skip_samples = uint16_t(i.current.skip_samples - skip_samples);
        }
        uint32_t keep = uint32_t(samples) - skip_samples;
        if (i.init.total_samples) keep = std::min(keep, i.current.total_samples);
        keep = uint32_t(std::min<uint64_t>(keep, output.remaining() / (uint64_t(channels) * i.sample_bytes())));
        std::vector<uint8_t> bytes(size_t(keep) * channels * i.sample_bytes());
        for (uint32_t s = 0; s < keep * channels; ++s) {
            const float v = std::max(-1.0f, std::min(1.0f, pcm[size_t(skip_samples) * channels + s]));
            if (i.format == 0) ps4::put_le(bytes.data(), size_t(s) * 2, uint16_t(int16_t(v * 32767.0f)), 2);
            else if (i.format == 1) ps4::put_le(bytes.data(), size_t(s) * 4, uint32_t(int32_t(double(v) * 2147483647.0)), 4);
            else { uint32_t raw; std::memcpy(&raw, &pcm[size_t(skip_samples) * channels + s], 4); ps4::put_le(bytes.data(), size_t(s) * 4, raw, 4); }
        }
        output.write(memory_, bytes.data(), bytes.size());
        i.current.skipped_samples = uint16_t(i.current.skipped_samples + (uint32_t(samples) - keep));
        if (i.init.total_samples) i.current.total_samples -= keep;
        i.total_samples += keep;
        stats_.samples += keep;
        return 0;
    }

    PS4GuestMemory& memory_;
    mutable std::mutex lock_;
    std::vector<Context> contexts_;
    std::vector<std::unique_ptr<Instance>> instances_;
    std::deque<uint32_t> pending_;
    uint32_t next_batch_ = 1;
    Stats stats_;
};
} // namespace maxps4
