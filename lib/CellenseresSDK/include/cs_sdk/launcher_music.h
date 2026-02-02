#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace csdk::launcher_music {
    struct Config {
        std::filesystem::path wav_path;
        uint32_t output_sample_rate = 48000;
        uint32_t output_channels = 2;
        uint32_t target_queue_ms = 200;
        uint32_t chunk_frames = 1024;
    };

    struct Callbacks {
        bool (*is_launcher_visible)() = nullptr;
        bool (*is_game_started)() = nullptr;
        uint32_t (*get_queued_ms)() = nullptr;
        bool (*queue_audio)(const float* samples, size_t frames) = nullptr;
        bool (*start_playback)() = nullptr;
        void (*stop_playback)() = nullptr;
    };

    void init(const Config& config, const Callbacks& callbacks);
    void set_enabled(bool enabled);
    void update(float volume);
    void shutdown();
}
