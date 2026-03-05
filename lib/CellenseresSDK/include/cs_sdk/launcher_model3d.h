#pragma once

#include <filesystem>
#include <string>

namespace recompui {
    class LauncherMenu;
}

namespace csdk::launcher3d {
    struct Vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct EulerDeg {
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
    };

    struct Transform {
        Vec3 position{};
        EulerDeg rotation_deg{};
        Vec3 scale{ 1.0f, 1.0f, 1.0f };
    };

    struct Light {
        Vec3 direction_ws{ -0.2f, -0.8f, -0.5f };
        Vec3 position_ws{ 0.0f, 1.0f, 2.2f };
        float range = 8.0f;
        Vec3 color{ 1.0f, 1.0f, 1.0f };
        float intensity = 1.25f;
        float ambient_intensity = 0.18f;
    };

    struct IntroAnimation {
        float duration_sec = 1.9f;
        float overshoot = 0.35f;
        float damping = 6.5f;
        bool play_once = true;
    };

    struct Config {
        std::filesystem::path glb_path{};
        Transform target_transform{};
        Light light{};
        IntroAnimation intro{};
        bool visible_only_on_title_screen = true;
    };

    void install_render_hook_chain();
    void prime_render_backend(void* rhi, void* dev);
    bool configure(const Config& cfg);
    void set_enabled(bool enabled);
    void on_launcher_menu_update(recompui::LauncherMenu* menu);
    void reset_intro();
    void shutdown();

    Config get_current_tuning_snapshot();
    std::string make_cpp_initializer_snippet();
}
