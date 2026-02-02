#include "cs_sdk/launcher_music.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace csdk::launcher_music {
namespace {
struct State {
    Config config{};
    Callbacks callbacks{};
    std::vector<float> pcm;
    size_t cursor_frames = 0;
    uint32_t pcm_channels = 0;
    bool enabled = false;
    bool loaded = false;
    bool load_attempted = false;
    bool playing = false;
};

State state;

uint16_t read_u16_le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    stream.seekg(0, std::ios::end);
    std::streamoff size = stream.tellg();
    if (size <= 0) {
        return false;
    }
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    stream.read(reinterpret_cast<char*>(out.data()), size);
    return stream.good();
}

struct WavInfo {
    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
};

bool parse_wav(const std::vector<uint8_t>& bytes, WavInfo& info) {
    if (bytes.size() < 12) {
        return false;
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0) {
        return false;
    }
    if (std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    size_t offset = 12;
    bool got_fmt = false;
    bool got_data = false;

    while (offset + 8 <= bytes.size()) {
        const uint8_t* chunk = bytes.data() + offset;
        uint32_t chunk_size = read_u32_le(chunk + 4);
        offset += 8;

        if (offset + chunk_size > bytes.size()) {
            break;
        }

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return false;
            }
            info.format = read_u16_le(bytes.data() + offset + 0);
            info.channels = read_u16_le(bytes.data() + offset + 2);
            info.sample_rate = read_u32_le(bytes.data() + offset + 4);
            info.bits_per_sample = read_u16_le(bytes.data() + offset + 14);
            got_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            info.data = bytes.data() + offset;
            info.data_size = chunk_size;
            got_data = true;
        }

        offset += chunk_size;
        if (chunk_size % 2 == 1) {
            offset += 1;
        }
    }

    return got_fmt && got_data;
}

bool decode_wav_to_float(const WavInfo& info, std::vector<float>& out, uint32_t& out_channels, uint32_t& out_sample_rate) {
    if (info.data == nullptr || info.data_size == 0) {
        return false;
    }
    if (info.channels == 0 || info.sample_rate == 0) {
        return false;
    }

    if (info.format == 1 && info.bits_per_sample == 16) {
        size_t bytes_per_frame = info.channels * 2;
        size_t frame_count = info.data_size / bytes_per_frame;
        if (frame_count == 0) {
            return false;
        }
        out.resize(frame_count * info.channels);
        const int16_t* samples = reinterpret_cast<const int16_t*>(info.data);
        for (size_t i = 0; i < frame_count * info.channels; ++i) {
            out[i] = static_cast<float>(samples[i]) / 32768.0f;
        }
    } else if (info.format == 3 && info.bits_per_sample == 32) {
        size_t bytes_per_frame = info.channels * 4;
        size_t frame_count = info.data_size / bytes_per_frame;
        if (frame_count == 0) {
            return false;
        }
        out.resize(frame_count * info.channels);
        const float* samples = reinterpret_cast<const float*>(info.data);
        std::copy(samples, samples + out.size(), out.begin());
    } else {
        return false;
    }

    out_channels = info.channels;
    out_sample_rate = info.sample_rate;
    return true;
}

std::vector<float> resample_linear(const std::vector<float>& in, uint32_t in_rate, uint32_t out_rate, uint32_t channels) {
    if (in.empty() || in_rate == 0 || out_rate == 0 || channels == 0) {
        return {};
    }
    if (in_rate == out_rate) {
        return in;
    }

    const size_t in_frames = in.size() / channels;
    if (in_frames == 0) {
        return {};
    }
    const size_t out_frames = static_cast<size_t>((static_cast<double>(in_frames) * out_rate) / in_rate);
    if (out_frames == 0) {
        return {};
    }

    std::vector<float> out(out_frames * channels);
    const double rate_ratio = static_cast<double>(in_rate) / static_cast<double>(out_rate);

    for (size_t i = 0; i < out_frames; ++i) {
        double src_pos = static_cast<double>(i) * rate_ratio;
        size_t idx = static_cast<size_t>(src_pos);
        double frac = src_pos - static_cast<double>(idx);

        if (idx >= in_frames - 1) {
            idx = in_frames - 1;
            frac = 0.0;
        }

        size_t idx_next = (idx + 1 < in_frames) ? (idx + 1) : idx;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            float a = in[idx * channels + ch];
            float b = in[idx_next * channels + ch];
            out[i * channels + ch] = static_cast<float>(a * (1.0 - frac) + b * frac);
        }
    }

    return out;
}

std::vector<float> convert_channels(const std::vector<float>& in, uint32_t in_channels, uint32_t out_channels) {
    if (in_channels == out_channels) {
        return in;
    }
    if (in_channels == 0 || out_channels == 0) {
        return {};
    }

    const size_t in_frames = in.size() / in_channels;
    std::vector<float> out(in_frames * out_channels);

    if (in_channels == 1 && out_channels == 2) {
        for (size_t i = 0; i < in_frames; ++i) {
            float s = in[i];
            out[i * 2 + 0] = s;
            out[i * 2 + 1] = s;
        }
        return out;
    }

    if (in_channels == 2 && out_channels == 1) {
        for (size_t i = 0; i < in_frames; ++i) {
            float l = in[i * 2 + 0];
            float r = in[i * 2 + 1];
            out[i] = 0.5f * (l + r);
        }
        return out;
    }

    return {};
}

bool load_wav() {
    if (state.loaded) {
        return true;
    }
    if (state.config.wav_path.empty()) {
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!read_file_bytes(state.config.wav_path, bytes)) {
        return false;
    }

    WavInfo info{};
    if (!parse_wav(bytes, info)) {
        return false;
    }

    std::vector<float> decoded;
    uint32_t decoded_channels = 0;
    uint32_t decoded_rate = 0;
    if (!decode_wav_to_float(info, decoded, decoded_channels, decoded_rate)) {
        return false;
    }

    std::vector<float> resampled = resample_linear(decoded, decoded_rate, state.config.output_sample_rate, decoded_channels);
    if (resampled.empty()) {
        return false;
    }

    std::vector<float> converted = convert_channels(resampled, decoded_channels, state.config.output_channels);
    if (converted.empty()) {
        return false;
    }

    state.pcm = std::move(converted);
    state.pcm_channels = state.config.output_channels;
    state.loaded = true;
    return true;
}

bool callbacks_valid() {
    return state.callbacks.is_launcher_visible
        && state.callbacks.is_game_started
        && state.callbacks.get_queued_ms
        && state.callbacks.queue_audio
        && state.callbacks.start_playback
        && state.callbacks.stop_playback;
}
} // namespace

void init(const Config& config, const Callbacks& callbacks) {
    state.config = config;
    state.callbacks = callbacks;
}

void set_enabled(bool enabled) {
    state.enabled = enabled;
}

void update(float volume) {
    if (!state.enabled || !callbacks_valid()) {
        return;
    }

    const bool should_play = !state.callbacks.is_game_started()
        && state.callbacks.is_launcher_visible();

    if (!should_play) {
        if (state.playing) {
            state.callbacks.stop_playback();
            state.playing = false;
        }
        return;
    }

    if (!state.loaded) {
        if (state.load_attempted) {
            return;
        }
        state.load_attempted = true;
        if (!load_wav()) {
            return;
        }
    }

    if (!state.playing) {
        if (!state.callbacks.start_playback()) {
            return;
        }
        state.cursor_frames = 0;
        state.playing = true;
    }

    if (state.pcm.empty() || state.pcm_channels == 0) {
        return;
    }

    volume = std::clamp(volume, 0.0f, 1.0f);

    static std::vector<float> temp;
    uint32_t queued_ms = state.callbacks.get_queued_ms();
    size_t safety = 0;

    while (queued_ms < state.config.target_queue_ms) {
        if (safety++ > 128) {
            break;
        }

        const size_t total_frames = state.pcm.size() / state.pcm_channels;
        size_t frames_available = total_frames - state.cursor_frames;
        size_t frames_to_copy = std::min<size_t>(state.config.chunk_frames, frames_available);

        if (frames_to_copy == 0) {
            state.cursor_frames = 0;
            continue;
        }

        temp.resize(frames_to_copy * state.pcm_channels);
        const float* src = state.pcm.data() + state.cursor_frames * state.pcm_channels;

        if (volume >= 0.999f) {
            std::copy(src, src + temp.size(), temp.begin());
        } else {
            for (size_t i = 0; i < temp.size(); ++i) {
                temp[i] = src[i] * volume;
            }
        }

        if (!state.callbacks.queue_audio(temp.data(), frames_to_copy)) {
            break;
        }

        state.cursor_frames += frames_to_copy;
        if (state.cursor_frames >= total_frames) {
            state.cursor_frames = 0;
        }

        queued_ms = state.callbacks.get_queued_ms();
    }
}

void shutdown() {
    if (state.playing && state.callbacks.stop_playback) {
        state.callbacks.stop_playback();
    }
    state = {};
}
} // namespace csdk::launcher_music
