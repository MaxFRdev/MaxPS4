#pragma once
// libSceAjm (the PS4's audio decoder) for MP3, decoded on the host with
// minimp3 (CC0, minimp3.h). Call of Duty: Modern Warfare Remastered plays its
// menu music and much of its sound through it.
//
// The batch helpers (sceAjmBatchJobRunBufferRa / ControlBufferRa) are HLE
// too, so the batch the game builds is in MaxPS4's own compact layout and is
// only read back here: sceAjmBatchStartBuffer runs it at once and
// sceAjmBatchWait has nothing to wait for. A record is no larger than the
// console's own job (the game sizes its batch buffer for those).
//
// Job flags (u64): version 0-2, codec 3-10, run flags 11-12 (GET_CODEC_INFO,
// MULTIPLE_FRAMES), control flags 13-15 (RESET, INITIALIZE, RESAMPLE),
// sideband flags 45-47 (GAPLESS_DECODE, FORMAT, STREAM). The sideband output
// is the 8-byte result, then in this order when asked: stream (consumed and
// written bytes, total samples), format, gapless decode, multiple frames,
// codec info.
#include "maxps4_ps4_guest_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// The decoder's code is compiled once, in maxps4_ps4_libkernel.cpp.
#include "minimp3.h"

namespace maxps4 {

class PS4Ajm final {
public:
    explicit PS4Ajm(PS4GuestMemory& memory) : memory_(memory) {}

    // AjmResult bits written to the sideband's result word.
    static constexpr int32_t result_not_initialized = 0x1, result_invalid_data = 0x2,
        result_partial_input = 0x8, result_not_enough_room = 0x10;
    static constexpr int32_t error_invalid_parameter = int32_t(0x80930005u), error_invalid_context = int32_t(0x80930002u);

    void reset() { std::lock_guard g(lock_); instances_.clear(); next_ = 0; }

    int64_t initialize(uint64_t context_out) {
        const uint32_t context = 1;
        return context_out && memory_.write(context_out, &context, 4) ? 0 : error_invalid_parameter;
    }
    // InstanceCreate(context, codec, AjmInstanceFlags, u32* instance): flags
    // bits 3-6 channels, 7-9 sample format (0 S16, 1 S32, 2 float).
    int64_t instance_create(uint32_t codec, uint64_t flags, uint64_t instance_out) {
        if (!instance_out || codec != 0) return error_invalid_parameter; // MP3 only
        std::lock_guard g(lock_);
        auto state = std::make_unique<Instance>();
        mp3dec_init(&state->decoder);
        state->format = uint32_t((flags >> 7) & 7);
        state->channels = uint32_t((flags >> 3) & 15);
        const uint32_t id = (codec << 14) | (++next_ & 0x3fff);
        if (!memory_.write(instance_out, &id, 4)) return error_invalid_parameter;
        instances_[id] = std::move(state);
        return 0;
    }
    int64_t instance_destroy(uint32_t id) { std::lock_guard g(lock_); instances_.erase(id); return 0; }

    // The batch helpers: write one record at `position`, return past it.
    uint64_t job_run(uint64_t position, uint32_t instance, uint64_t flags, uint64_t input, uint64_t input_size,
                     uint64_t output, uint64_t output_size, uint64_t sideband, uint64_t sideband_size) {
        RunRecord r{};
        r.magic_kind = magic | kind_run; r.instance = instance; r.flags = flags;
        r.input = input; r.output = output; r.sideband = sideband;
        r.input_size = uint32_t(std::min<uint64_t>(input_size, UINT32_MAX));
        r.output_size = uint32_t(std::min<uint64_t>(output_size, UINT32_MAX));
        r.sideband_size = uint32_t(std::min<uint64_t>(sideband_size, UINT32_MAX));
        return position && memory_.write(position, &r, sizeof r) ? position + sizeof r : 0;
    }
    uint64_t job_control(uint64_t position, uint32_t instance, uint64_t flags, uint64_t sideband_in,
                         uint64_t sideband_in_size, uint64_t sideband_out, uint64_t sideband_out_size) {
        ControlRecord r{};
        r.magic_kind = magic | kind_control; r.instance = instance; r.flags = flags;
        r.sideband_in = sideband_in; r.sideband_out = sideband_out;
        r.sideband_in_size = uint32_t(std::min<uint64_t>(sideband_in_size, UINT32_MAX));
        r.sideband_out_size = uint32_t(std::min<uint64_t>(sideband_out_size, UINT32_MAX));
        return position && memory_.write(position, &r, sizeof r) ? position + sizeof r : 0;
    }
    // BatchStartBuffer(context, batch, size, priority, AjmBatchError*, u32* id).
    int64_t batch_start(uint64_t batch, uint64_t size, uint64_t batch_id_out) {
        if (!batch || size > (1u << 20)) return error_invalid_parameter;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (size && !memory_.read(batch, bytes.data(), bytes.size())) return error_invalid_parameter;
        std::lock_guard g(lock_);
        for (size_t at = 0; at + 4 <= bytes.size();) {
            uint32_t head = 0;
            std::memcpy(&head, bytes.data() + at, 4);
            if ((head & ~0xffu) != magic) break; // not one of ours: the end
            if ((head & 0xff) == kind_run && at + sizeof(RunRecord) <= bytes.size()) {
                RunRecord r; std::memcpy(&r, bytes.data() + at, sizeof r);
                run(r); at += sizeof r; ++jobs_;
            } else if ((head & 0xff) == kind_control && at + sizeof(ControlRecord) <= bytes.size()) {
                ControlRecord r; std::memcpy(&r, bytes.data() + at, sizeof r);
                control(r); at += sizeof r; ++jobs_;
            } else break;
        }
        const uint32_t id = ++batches_;
        if (batch_id_out) memory_.write(batch_id_out, &id, 4);
        return 0;
    }
    // Decoded so far, for the report.
    uint64_t jobs() const { return jobs_; }
    uint64_t samples() const { return samples_; }
    // How runs ended, and the last one's sizes, for the report.
    std::string note() {
        std::lock_guard g(lock_);
        char text[320];
        std::snprintf(text, sizeof text, "%llu lancements, %llu sans trame (%llu place insuffisante, %llu entrée partielle, %llu instance inconnue), %llu contrôles · dernier : entrée %u octets, sortie %u octets, %u trames, %u canaux, %u Hz, format %u, drapeaux 0x%llx, résultat 0x%x",
                      (unsigned long long)runs_, (unsigned long long)empty_, (unsigned long long)room_, (unsigned long long)partial_,
                      (unsigned long long)unknown_, (unsigned long long)controls_, last_.input_size, last_.output_size, last_.frames,
                      last_.channels, last_.rate, last_.format, (unsigned long long)last_.flags, unsigned(last_.result));
        return text;
    }

private:
    static constexpr uint32_t magic = 0x414a4d00u, kind_run = 1, kind_control = 2;
    struct RunRecord {
        uint32_t magic_kind, instance; uint64_t flags, input, output, sideband;
        uint32_t input_size, output_size, sideband_size, pad;
    };
    struct ControlRecord {
        uint32_t magic_kind, instance; uint64_t flags, sideband_in, sideband_out;
        uint32_t sideband_in_size, sideband_out_size;
    };
    static_assert(sizeof(RunRecord) == 56 && sizeof(ControlRecord) == 40);
    struct Instance {
        mp3dec_t decoder;
        uint32_t format = 0, channels = 2, last_channels = 2, last_rate = 48000, last_bitrate = 0;
        uint64_t total = 0; // samples per channel decoded since the last reset
    };
    struct Sideband {
        int32_t result = 0;
        int32_t consumed = 0, written = 0; uint64_t total = 0;
        uint32_t channels = 0, rate = 0, bitrate = 0, format = 0, frames = 0;
    };
    // The result, then the sections the flags ask for, as far as `size` allows.
    void write_sideband(uint64_t address, uint32_t size, uint64_t flags, const Sideband& s) {
        if (!address) return;
        std::vector<uint8_t> out;
        auto put = [&out](const void* data, size_t bytes) {
            const auto* p = static_cast<const uint8_t*>(data);
            out.insert(out.end(), p, p + bytes);
        };
        const int32_t result[2] = {s.result, 0};
        put(result, 8);
        if (flags >> 47 & 1) { // STREAM
            put(&s.consumed, 4); put(&s.written, 4); put(&s.total, 8);
        }
        if (flags >> 46 & 1) { // FORMAT
            const uint32_t mask = s.channels == 1 ? 0x4u : s.channels == 2 ? 0x3u : (1u << s.channels) - 1;
            const uint32_t format[6] = {s.channels, mask, s.rate, s.format, s.bitrate, 0};
            put(format, sizeof format);
        }
        if (flags >> 45 & 1) { const uint32_t gapless[2] = {0, 0}; put(gapless, 8); } // GAPLESS_DECODE
        if (flags >> 12 & 1) { const uint32_t mframe[2] = {s.frames, 0}; put(mframe, 8); } // MULTIPLE_FRAMES
        if (flags >> 11 & 1) { const uint8_t info[16] = {}; put(info, 16); }            // GET_CODEC_INFO
        if (out.size() > size) out.resize(size);
        if (!out.empty()) memory_.write(address, out.data(), out.size());
    }
    void control(const ControlRecord& r) {
        ++controls_;
        Sideband s;
        auto found = instances_.find(r.instance);
        if (found == instances_.end()) s.result = result_not_initialized;
        else {
            Instance& in = *found->second;
            if (r.flags >> 13 & 1) { mp3dec_init(&in.decoder); in.total = 0; } // RESET
            s.channels = in.last_channels; s.rate = in.last_rate; s.bitrate = in.last_bitrate; s.format = in.format;
            s.total = in.total;
        }
        write_sideband(r.sideband_out, r.sideband_out_size, r.flags, s);
    }
    void run(const RunRecord& r) {
        Sideband s;
        ++runs_;
        auto found = instances_.find(r.instance);
        if (found == instances_.end()) { ++unknown_; ++empty_; s.result = result_not_initialized; write_sideband(r.sideband, r.sideband_size, r.flags, s); return; }
        Instance& in = *found->second;
        const bool multiple = r.flags >> 12 & 1;
        const size_t input_size = std::min<size_t>(r.input_size, 16u << 20), output_size = std::min<size_t>(r.output_size, 64u << 20);
        std::vector<uint8_t> input(input_size);
        if (input_size && !memory_.read(r.input, input.data(), input_size)) {
            s.result = result_invalid_data; write_sideband(r.sideband, r.sideband_size, r.flags, s); return;
        }
        const size_t sample_bytes = in.format == 0 ? 2 : 4;
        const size_t worst = MINIMP3_MAX_SAMPLES_PER_FRAME * sample_bytes; // both channels of one frame
        std::vector<uint8_t> output;
        output.reserve(std::min(output_size, worst * 16));
        mp3dec_frame_info_t peek{};
        size_t consumed = 0;
        mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        for (;;) {
            if (consumed >= input_size) { if (!s.frames) s.result |= result_partial_input; break; }
            // Room for this frame as it is (mono, or MPEG-2's 576 samples, need
            // less than the worst case): the header says, without decoding.
            peek = {};
            const int frame_samples = mp3dec_decode_frame(&in.decoder, input.data() + consumed, int(input_size - consumed), nullptr, &peek);
            if (!peek.frame_bytes) { s.result |= result_partial_input; break; } // needs more input
            const size_t frame_bytes_out = size_t(std::max(frame_samples, 0)) * size_t(std::max(peek.channels, 1)) * sample_bytes;
            if (output.size() + std::min(frame_bytes_out, worst) > output_size) { s.result |= result_not_enough_room; break; }
            mp3dec_frame_info_t info{};
            const int samples = mp3dec_decode_frame(&in.decoder, input.data() + consumed, int(input_size - consumed), pcm, &info);
            if (!info.frame_bytes) { s.result |= result_partial_input; break; } // needs more input
            consumed += size_t(info.frame_bytes);
            if (samples <= 0) continue; // tags, junk, or a frame still filling the reservoir
            in.last_channels = uint32_t(info.channels); in.last_rate = uint32_t(info.hz);
            in.last_bitrate = uint32_t(info.bitrate_kbps) * 1000;
            const size_t values = size_t(samples) * size_t(info.channels);
            const size_t at = output.size();
            output.resize(at + values * sample_bytes);
            for (size_t i = 0; i < values; ++i) {
                const int16_t v = pcm[i];
                if (in.format == 0) std::memcpy(&output[at + i * 2], &v, 2);
                else if (in.format == 1) { const int32_t w = int32_t(v) * 65536; std::memcpy(&output[at + i * 4], &w, 4); }
                else { const float f = float(v) / 32768.0f; std::memcpy(&output[at + i * 4], &f, 4); }
            }
            in.total += uint64_t(samples); samples_ += uint64_t(samples);
            ++s.frames;
            if (!multiple) break;
        }
        if (!output.empty() && !memory_.write(r.output, output.data(), output.size())) s.result |= result_invalid_data;
        // A frame decoded is not a partial input: only "no frame at all" is.
        if (s.frames) s.result &= ~result_partial_input;
        s.consumed = int32_t(consumed); s.written = int32_t(output.size()); s.total = in.total;
        s.channels = in.last_channels; s.rate = in.last_rate; s.bitrate = in.last_bitrate; s.format = in.format;
        write_sideband(r.sideband, r.sideband_size, r.flags, s);
        if (!s.frames) {
            ++empty_;
            if (s.result & result_not_enough_room) ++room_;
            else if (s.result & result_partial_input) ++partial_;
        }
        last_ = {r.input_size, r.output_size, s.frames, s.channels, s.rate, s.format, r.flags, s.result};
    }

    PS4GuestMemory& memory_;
    std::mutex lock_;
    std::map<uint32_t, std::unique_ptr<Instance>> instances_;
    uint32_t next_ = 0, batches_ = 0;
    uint64_t jobs_ = 0, samples_ = 0;
    uint64_t runs_ = 0, empty_ = 0, room_ = 0, partial_ = 0, unknown_ = 0, controls_ = 0;
    struct Last { uint32_t input_size, output_size, frames, channels, rate, format; uint64_t flags; int32_t result; } last_{};
};

} // namespace maxps4
