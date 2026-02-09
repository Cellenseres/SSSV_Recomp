#include <cstdio>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <array>
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <stdexcept>
#include <cinttypes>
#include <string>

#include "nfd.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always
#endif

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#define private public
#include "recompui/renderer.h"
#undef private
#include "recompui/config.h"
#include "util/file.h"
#include "recompinput/input_events.h"
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"
#include "sssv_config.h"
#include "sssv_game.h"
#include "sssv_launcher.h"
#include "theme.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"
#include "cs_sdk/launcher_music.h"

#ifdef _WIN32
#include <unknwn.h>
#include <objidl.h>
#endif

#include "hle/rt64_application.h"
#include "gbi/rt64_gbi_rdp.h"
#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_s2dex.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <timeapi.h>
#include "SDL_syswm.h"
#endif

const std::string version_string = "0.1.0";

template<typename... Ts>
void exit_error(const char* str, Ts ...args) {
    ((void)fprintf(stderr, str, args), ...);
    assert(false);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) > 0) {
        exit_error("Failed to initialize SDL2: %s\n", SDL_GetError());
    }

    fprintf(stdout, "SDL Video Driver: %s\n", SDL_GetCurrentVideoDriver());

    return {};
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (recompinput::players::is_single_player_mode() || recompinput::players::get_player_is_assigned(controller_num)) {
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }

    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

SDL_Window* window;

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;

#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow("Space Station Silicon Valley: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);

    if (window == nullptr) {
        exit_error("Failed to create window: %s\n", SDL_GetError());
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__) || defined(__ANDROID__)
    return ultramodern::renderer::WindowHandle{ window };
#elif defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window, SDL_Metal_GetLayer(view) };
#else
    static_assert(false && "Unimplemented");
#endif
}

// Launcher music volume scale (0–1): applied on top of main volume so launcher music is quieter.
static constexpr float LAUNCHER_MUSIC_VOLUME_SCALE = 0.1f;

void update_gfx(void*) {
    recompinput::handle_events();
    float main_vol = static_cast<float>(recompui::config::sound::get_main_volume()) / 100.0f;
    float launcher_volume = main_vol * LAUNCHER_MUSIC_VOLUME_SCALE;
    csdk::launcher_music::update(launcher_volume);
}

static SDL_AudioCVT audio_convert;
static SDL_AudioDeviceID audio_device = 0;
static SDL_AudioDeviceID launcher_audio_device = 0;
static bool launcher_audio_failed = false;
static uint32_t launcher_audio_sample_rate = 0;
static uint32_t launcher_audio_channels = 0;

static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;

constexpr uint32_t duplicated_input_frames = 4;
static uint32_t discarded_output_frames;

constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

void queue_samples(int16_t* audio_data, size_t sample_count) {
    static std::vector<float> swap_buffer;
    static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

    size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
    size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
    if (max_sample_count > swap_buffer.size()) {
        swap_buffer.resize(max_sample_count);
    }

    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        swap_buffer[i] = duplicated_sample_buffer[i];
    }

    float cur_main_volume = static_cast<float>(recompui::config::sound::get_main_volume()) / 100.0f;
    for (size_t i = 0; i < sample_count; i += input_channels) {
        swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (0.5f / 32768.0f) * cur_main_volume;
        swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (0.5f / 32768.0f) * cur_main_volume;
    }

    assert(sample_count > duplicated_input_frames * input_channels);

    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
    }

    audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
    audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);

    int ret = SDL_ConvertAudio(&audio_convert);

    if (ret < 0) {
        printf("Error using SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error using SDL audio converter");
    }

    uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / sample_rate;
    uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
    float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

    uint32_t skip_factor = cur_queued_microseconds / 100000;
    if (skip_factor != 0) {
        uint32_t skip_ratio = 1 << skip_factor;
        num_bytes_to_queue /= skip_ratio;
        for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
            samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
            samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
        }
    }

    SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
}

size_t get_frames_remaining() {
    constexpr float buffer_offset_frames = 1.0f;
    uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);

    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;

    uint32_t frames_per_vi = (sample_rate / 60);
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

void update_audio_converter() {
    int ret = SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate);

    if (ret < 0) {
        printf("Error creating SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error creating SDL audio converter");
    }

    discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
}

void set_frequency(uint32_t freq) {
    sample_rate = freq;
    update_audio_converter();
}

bool reset_audio(uint32_t output_freq) {
    SDL_AudioSpec spec_desired{
        .freq = (int)output_freq,
        .format = AUDIO_F32,
        .channels = (Uint8)output_channels,
        .silence = 0,
        .samples = 0x100,
        .padding = 0,
        .size = 0,
        .callback = nullptr,
        .userdata = nullptr
    };

    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (audio_device == 0) {
        std::string audio_error = std::string("No audio device could be found. Please make sure an audio device is available.\nError opening audio device: ") + std::string(SDL_GetError());
        recompui::message_box(audio_error.c_str());
        return false;
    }

    SDL_PauseAudioDevice(audio_device, 0);

    output_sample_rate = output_freq;
    update_audio_converter();

    return true;
}

namespace {
bool ensure_launcher_audio_device() {
    if (launcher_audio_device != 0) {
        return true;
    }
    if (launcher_audio_failed) {
        return false;
    }

    SDL_AudioSpec spec_desired{
        .freq = (int)output_sample_rate,
        .format = AUDIO_F32,
        .channels = (Uint8)output_channels,
        .silence = 0,
        .samples = 0x100,
        .padding = 0,
        .size = 0,
        .callback = nullptr,
        .userdata = nullptr
    };

    launcher_audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (launcher_audio_device == 0) {
        printf("Launcher BGM: failed to open audio device: %s\n", SDL_GetError());
        launcher_audio_failed = true;
        return false;
    }

    SDL_PauseAudioDevice(launcher_audio_device, 1);
    launcher_audio_sample_rate = output_sample_rate;
    launcher_audio_channels = output_channels;
    return true;
}

bool launcher_start_playback() {
    if (!ensure_launcher_audio_device()) {
        return false;
    }
    SDL_ClearQueuedAudio(launcher_audio_device);
    SDL_PauseAudioDevice(launcher_audio_device, 0);
    return true;
}

void launcher_stop_playback() {
    if (launcher_audio_device == 0) {
        return;
    }
    SDL_ClearQueuedAudio(launcher_audio_device);
    SDL_PauseAudioDevice(launcher_audio_device, 1);
}

uint32_t launcher_get_queued_ms() {
    if (!ensure_launcher_audio_device() || launcher_audio_sample_rate == 0 || launcher_audio_channels == 0) {
        return 0;
    }
    const uint32_t frame_bytes = launcher_audio_channels * sizeof(float);
    if (frame_bytes == 0) {
        return 0;
    }
    uint32_t queued_bytes = SDL_GetQueuedAudioSize(launcher_audio_device);
    uint32_t queued_frames = queued_bytes / frame_bytes;
    return (queued_frames * 1000) / launcher_audio_sample_rate;
}

bool launcher_queue_audio(const float* samples, size_t frames) {
    if (!ensure_launcher_audio_device()) {
        return false;
    }
    if (frames == 0) {
        return true;
    }
    const uint32_t bytes = static_cast<uint32_t>(frames * launcher_audio_channels * sizeof(float));
    SDL_QueueAudio(launcher_audio_device, samples, bytes);
    return true;
}

// Music plays until "Start Game" is clicked; keep playing in Controls, Settings, Mods.
bool launcher_is_visible() {
    return !ultramodern::is_game_started();
}

bool launcher_game_started() {
    return ultramodern::is_game_started();
}

void shutdown_launcher_audio_device() {
    if (launcher_audio_device != 0) {
        SDL_CloseAudioDevice(launcher_audio_device);
        launcher_audio_device = 0;
    }
    launcher_audio_failed = false;
}

enum class UnknownGBIFallback {
    None,
    F3DEX,
    S2DEX,
};

std::string read_ucode_name(uint8_t* rdram, uint32_t data_address) {
    if (rdram == nullptr) {
        return {};
    }

    constexpr uint32_t rdram_mask = 0x7FFFFF;
    constexpr size_t read_size = 0x800;
    std::array<uint8_t, read_size> data_segment{};

    for (size_t i = 0; i < read_size; i++) {
        uint32_t address = (data_address + static_cast<uint32_t>(i)) & rdram_mask;
        data_segment[i] = rdram[address ^ 0x3];
    }

    const uint8_t rsp_pattern[] = "RSP";
    auto* segment_begin = data_segment.data();
    auto* segment_end = data_segment.data() + data_segment.size();
    auto* pattern_begin = rsp_pattern;
    auto* pattern_end = rsp_pattern + sizeof(rsp_pattern) - 1;
    auto* search_result = std::search(segment_begin, segment_end, pattern_begin, pattern_end);
    if (search_result == segment_end) {
        return {};
    }

    size_t valid_chars = 0;
    while ((search_result + valid_chars) < segment_end) {
        uint8_t c = search_result[valid_chars];
        if (c <= 0x0A || c > 0x7E) {
            break;
        }

        valid_chars++;
    }

    if (valid_chars == 0) {
        return {};
    }

    return std::string(reinterpret_cast<const char*>(search_result), valid_chars);
}

UnknownGBIFallback get_unknown_gbi_fallback(uint8_t* rdram, const OSTask* task, std::string* out_ucode_name) {
    if (task == nullptr || task->t.type != M_GFXTASK) {
        return UnknownGBIFallback::None;
    }

    std::string ucode_name = read_ucode_name(rdram, task->t.ucode_data & 0x3FFFFFF);
    if (out_ucode_name != nullptr) {
        *out_ucode_name = ucode_name;
    }

    if (ucode_name.find("F3DTEX/A") != std::string::npos) {
        return UnknownGBIFallback::F3DEX;
    }

    if (ucode_name.find("F3DEX") != std::string::npos || ucode_name.find("F3D") != std::string::npos) {
        return UnknownGBIFallback::F3DEX;
    }

    if (ucode_name.find("S2D") != std::string::npos && ucode_name.find("S2DEX") == std::string::npos) {
        return UnknownGBIFallback::S2DEX;
    }

    if (ucode_name.find("S2DEX") != std::string::npos) {
        return UnknownGBIFallback::S2DEX;
    }

    return UnknownGBIFallback::None;
}

void apply_unknown_gbi_fallback(RT64::Application* app, UnknownGBIFallback fallback) {
    if (app == nullptr || app->interpreter == nullptr) {
        return;
    }

    auto& unknown_gbi = app->interpreter->gbiManager.gbiCache[static_cast<uint32_t>(RT64::GBIUCode::Unknown)];
    unknown_gbi = RT64::GBI{};

    switch (fallback) {
    case UnknownGBIFallback::F3DEX:
        unknown_gbi.ucode = RT64::GBIUCode::F3DEX;
        RT64::GBI_RDP::setup(&unknown_gbi, true);
        RT64::GBI_F3DEX::setup(&unknown_gbi);
        break;
    case UnknownGBIFallback::S2DEX:
        unknown_gbi.ucode = RT64::GBIUCode::S2DEX;
        RT64::GBI_RDP::setup(&unknown_gbi, true);
        RT64::GBI_S2DEX::setup(&unknown_gbi);
        break;
    case UnknownGBIFallback::None:
    default:
        break;
    }
}

class RT64CompatContext final : public ultramodern::renderer::RendererContext {
public:
    RT64CompatContext(std::unique_ptr<ultramodern::renderer::RendererContext> inner_context, uint8_t* rdram)
        : inner(std::move(inner_context)), rdram(rdram) {}

    bool valid() override {
        return inner != nullptr && inner->valid();
    }

    ultramodern::renderer::SetupResult get_setup_result() const override {
        return inner->get_setup_result();
    }

    ultramodern::renderer::GraphicsApi get_chosen_api() const override {
        return inner->get_chosen_api();
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) override {
        return inner->update_config(old_config, new_config);
    }

    void enable_instant_present() override {
        inner->enable_instant_present();
    }

    void send_dl(const OSTask* task) override {
        maybe_apply_unknown_ucode_fallback(task);
        inner->send_dl(task);
    }

    void send_dummy_workload(uint32_t fb_address) override {
        inner->send_dummy_workload(fb_address);
    }

    void update_screen() override {
        inner->update_screen();
    }

    void shutdown() override {
        inner->shutdown();
    }

    uint32_t get_display_framerate() const override {
        return inner->get_display_framerate();
    }

    float get_resolution_scale() const override {
        return inner->get_resolution_scale();
    }

private:
    std::unique_ptr<ultramodern::renderer::RendererContext> inner;
    uint8_t* rdram = nullptr;
    UnknownGBIFallback active_fallback = UnknownGBIFallback::None;

    void maybe_apply_unknown_ucode_fallback(const OSTask* task) {
        if (task == nullptr || task->t.type != M_GFXTASK) {
            return;
        }

        auto* rt64_context = dynamic_cast<recompui::renderer::RT64Context*>(inner.get());
        if (rt64_context == nullptr || rt64_context->app == nullptr) {
            return;
        }

        std::string ucode_name;
        UnknownGBIFallback fallback = get_unknown_gbi_fallback(rdram, task, &ucode_name);
        // Match the lib_modified behavior: default Unknown ucode path to F3DEX for SSSV.
        if (fallback == UnknownGBIFallback::None) {
            fallback = UnknownGBIFallback::F3DEX;
        }

        if (fallback == active_fallback) {
            return;
        }

        apply_unknown_gbi_fallback(rt64_context->app.get(), fallback);
        active_fallback = fallback;

        const char* fallback_name = (fallback == UnknownGBIFallback::F3DEX) ? "F3DEX" : "S2DEX";
        if (ucode_name.empty()) {
            fprintf(stderr, "[SSSV] RT64 unknown ucode fallback -> %s (default)\n", fallback_name);
        }
        else {
            fprintf(stderr, "[SSSV] RT64 unknown ucode fallback -> %s for \"%s\"\n", fallback_name, ucode_name.c_str());
        }
    }
};
} // namespace

// RSP microcode - SSSV uses audio RSP
extern RspUcodeFunc aspMain;

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return aspMain;

    default:
        fprintf(stderr, "Unknown task: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// Array of supported GameEntry objects
std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0x912A068AADB0D0C5ULL,
        .internal_name = "SILICON VALLEY",
        .display_name = "Space Station Silicon Valley",
        .game_id = u8"sssv.n64.us.1.0",
        .mod_game_id = "sssv",
        .save_type = recomp::SaveType::Eep4k, // SSSV uses EEPROM
        .thumbnail_bytes = std::span<const char>(),
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = sssv::on_init,
    },
};

#ifdef _WIN32

struct PreloadContext {
    HANDLE handle;
    HANDLE mapping_handle;
    SIZE_T size;
    PVOID view;
};

bool preload_executable(PreloadContext& context) {
    wchar_t module_name[MAX_PATH];
    GetModuleFileNameW(NULL, module_name, MAX_PATH);

    context.handle = CreateFileW(module_name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (context.handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to load executable into memory!");
        context = {};
        return false;
    }

    LARGE_INTEGER module_size;
    if (!GetFileSizeEx(context.handle, &module_size)) {
        fprintf(stderr, "Failed to get size of executable!");
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    context.size = module_size.QuadPart;

    context.mapping_handle = CreateFileMappingW(context.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (context.mapping_handle == nullptr) {
        fprintf(stderr, "Failed to create file mapping of executable!");
        CloseHandle(context.handle);
        context = {};
        return EXIT_FAILURE;
    }

    context.view = MapViewOfFile(context.mapping_handle, FILE_MAP_READ, 0, 0, 0);
    if (context.view == nullptr) {
        fprintf(stderr, "Failed to map view of executable!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE process_handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process_handle == nullptr) {
        fprintf(stderr, "Failed to open own process!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    SIZE_T minimum_set_size, maximum_set_size;
    if (!GetProcessWorkingSetSize(process_handle, &minimum_set_size, &maximum_set_size)) {
        fprintf(stderr, "Failed to get working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (!SetProcessWorkingSetSize(process_handle, minimum_set_size + context.size, maximum_set_size + context.size)) {
        fprintf(stderr, "Failed to set working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (VirtualLock(context.view, context.size) == 0) {
        fprintf(stderr, "Failed to lock view of executable! (Error: %08lx)\n", GetLastError());
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    return true;
}

void release_preload(PreloadContext& context) {
    VirtualUnlock(context.view, context.size);
    CloseHandle(context.mapping_handle);
    CloseHandle(context.handle);
    context = {};
}

static LONG WINAPI top_level_exception_filter(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord) {
        fprintf(stderr, "Unhandled exception 0x%08lX at %p\n",
            info->ExceptionRecord->ExceptionCode,
            info->ExceptionRecord->ExceptionAddress);
        fflush(stderr);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        char dump_name[MAX_PATH];
        snprintf(dump_name, sizeof(dump_name),
            "SSSVRecompiled_crash_%04d%02d%02d_%02d%02d%02d.dmp",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        HANDLE dump_file = CreateFileA(dump_name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dump_file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION dump_info{};
            dump_info.ThreadId = GetCurrentThreadId();
            dump_info.ExceptionPointers = info;
            dump_info.ClientPointers = FALSE;

            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump_file, MiniDumpNormal, &dump_info, nullptr, nullptr);
            CloseHandle(dump_file);
            fprintf(stderr, "Wrote crash dump: %s\n", dump_name);
            fflush(stderr);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

#elif defined(__linux__) || defined(APPLE)

struct PreloadContext {};

bool preload_executable(PreloadContext& context) {
    return true;
}

void release_preload(PreloadContext& context) {}

#else

struct PreloadContext {};

bool preload_executable(PreloadContext& context) {
    return false;
}

void release_preload(PreloadContext& context) {}

#endif

void enable_texture_pack(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    recompui::renderer::enable_texture_pack(context, mod);
}

void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    recompui::renderer::disable_texture_pack(mod);
}

void reorder_texture_pack(recomp::mods::ModContext&) {
    recompui::renderer::trigger_texture_pack_update();
}

void on_launcher_init(recompui::LauncherMenu* menu) {
    auto game_options_menu = menu->init_game_options_menu(
        supported_games[0].game_id,
        supported_games[0].mod_game_id,
        supported_games[0].display_name,
        supported_games[0].thumbnail_bytes,
        recompui::GameOptionsMenuLayout::Right
    );

    game_options_menu->add_default_options();
    game_options_menu->set_width(30, recompui::Unit::Percent);

    // Anchor menu in lower-right corner; Start Game at top, Exit at bottom
    game_options_menu->set_align_items(recompui::AlignItems::FlexEnd);
    game_options_menu->set_flex_direction(recompui::FlexDirection::Column);

    for (auto option : game_options_menu->get_options()) {
        option->set_justify_content(recompui::JustifyContent::FlexEnd);
        option->set_border_radius(0);

        std::vector<recompui::Style*> hover_focus = { &option->hover_style, &option->focus_style };
        for (auto style : hover_focus) {
            style->set_background_color(recompui::theme::color::Transparent);
        }
    }

    // Anchor menu container to viewport right edge so menu stays at lower-right when window is resized
    recompui::Element* menu_container = menu->get_menu_container();
    menu_container->set_width(1440);
    menu_container->unset_left();
    menu_container->set_top(sssv::launcher_options_top_offset);
    menu_container->set_bottom(-sssv::launcher_options_top_offset);
    menu_container->set_right(0);   // right edge of container = right edge of viewport
    menu_container->set_translate_2D(0.0f, 0.0f, recompui::Unit::Percent);

    game_options_menu->unset_left();
    game_options_menu->set_bottom(sssv::launcher_options_top_offset);  // offset from bottom edge
    game_options_menu->set_right(sssv::launcher_options_right_position_start);  // offset from right edge

    menu->remove_default_title();

    sssv::launcher_animation_setup(menu);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

    PreloadContext preload_context;
    bool preloaded = preload_executable(preload_context);

    if (!preloaded) {
        fprintf(stderr, "Failed to preload executable!\n");
    }

#ifdef _WIN32
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(top_level_exception_filter);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--show-console") == 0) {
            if (GetConsoleWindow() == nullptr) {
                AllocConsole();
                freopen("CONIN$", "r", stdin);
                freopen("CONOUT$", "w", stderr);
                freopen("CONOUT$", "w", stdout);
            }
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
            break;
        }
    }

    SetConsoleOutputCP(CP_UTF8);
#endif

#ifdef _WIN32
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif

#if defined(__linux__) && defined(RECOMP_FLATPAK)
    std::error_code ec;
    std::filesystem::current_path("/var/data", ec);
#endif

    NFD_Init();

    recompui::programconfig::set_program_name(sssv::program_name);
    recompui::programconfig::set_program_id(sssv::program_id);

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!reset_audio(48000)) {
        return EXIT_FAILURE;
    }

    std::u8string controller_db_path = (recompui::file::get_program_path() / "recompcontrollerdb.txt").u8string();
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char*>(controller_db_path.c_str())) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recompui::register_primary_font("LDF-ComicSans.ttf", "LDFComicSans");
    recompui::register_extra_font("InterVariable.ttf");

    csdk::launcher_music::Config launcher_music_config{
        .wav_path = recompui::file::get_asset_path("launcher_music.wav"),
        .output_sample_rate = output_sample_rate,
        .output_channels = output_channels,
        .target_queue_ms = 200,
        .chunk_frames = 1024,
    };
    csdk::launcher_music::Callbacks launcher_music_callbacks{
        .is_launcher_visible = launcher_is_visible,
        .is_game_started = launcher_game_started,
        .get_queued_ms = launcher_get_queued_ms,
        .queue_audio = launcher_queue_audio,
        .start_playback = launcher_start_playback,
        .stop_playback = launcher_stop_playback,
    };

    // Initialize launcher music with Cellenseres SDK
    csdk::launcher_music::init(launcher_music_config, launcher_music_callbacks);
    csdk::launcher_music::set_enabled(true);

    recomp::register_config_path(recompui::file::get_app_folder_path());

    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    recompui::register_ui_exports();

    recomptheme::set_custom_theme();

    sssv::register_overlays();

    recompinput::players::set_single_player_mode(true);

    sssv::init_config();

    recompui::register_launcher_init_callback(on_launcher_init);
    recompui::register_launcher_update_callback(sssv::launcher_animation_update);

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) -> std::unique_ptr<ultramodern::renderer::RendererContext> {
            auto presentation_mode = ultramodern::renderer::PresentationMode::PresentEarly;
            auto inner_context = recompui::renderer::create_render_context(rdram, window_handle, presentation_mode, developer_mode);
            return std::make_unique<RT64CompatContext>(std::move(inner_context), rdram);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = recompinput::profiles::get_n64_input,
        .set_rumble = recompinput::set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = recompinput::update_rumble,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = sssv::get_game_thread_name,
    };

    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = enable_texture_pack,
        .on_disabled = disable_texture_pack,
        .on_reordered = reorder_texture_pack,
    };
    auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);

    recomp::mods::register_mod_container_type("rtz", std::vector{ texture_pack_content_type_id }, false);

    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    csdk::launcher_music::shutdown();
    shutdown_launcher_audio_device();

    NFD_Quit();

    if (preloaded) {
        release_preload(preload_context);
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    return EXIT_SUCCESS;
}
