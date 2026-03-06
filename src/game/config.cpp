#include "sssv_config.h"
#include "sssv_game.h"
#include "recompui/config.h"
#include "util/file.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sssv {

namespace {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;
    enum class SpriteUpscalingOption {
        Original,
        Upscaled
    };
    constexpr const char* SpriteUpscaleOptionId = "sssv_2d_sprite_upscaling";

    static EnumOptionVector sprite_upscaling_options = {
        { SpriteUpscalingOption::Original, "Original" },
        { SpriteUpscalingOption::Upscaled, "Upscaled" }
    };
}

void init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();

    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;

    recompui::config::create_general_tab(general_options);
    recomp::config::Config& graphics_config = recompui::config::create_graphics_tab();
    graphics_config.add_enum_option(
        SpriteUpscaleOptionId,
        "2D Sprite Upscaling",
        "Controls whether legacy Sprite2D elements such as title art, menu sprites and similar 2D images use RT64's higher resolution sprite upscale path."
        "<br />"
        "<br />"
        "<recomp-color primary>Original</recomp-color> keeps those sprites on the original low-resolution texel grid."
        "<br />"
        "<recomp-color primary>Upscaled</recomp-color> renders them through the shared high-resolution sprite path."
        "<br />"
        "<br />"
        "This only has a visible effect when rendering above the original sprite resolution.",
        sprite_upscaling_options,
        SpriteUpscalingOption::Upscaled
    );
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_mods_tab();
    recompui::config::finalize();
}

bool get_2d_sprite_upscaling_enabled() {
    try {
        recomp::config::Config& graphics_config = recompui::config::get_graphics_config();
        if (!graphics_config.has_option(SpriteUpscaleOptionId)) {
            return true;
        }

        const auto option_value = graphics_config.get_option_value(SpriteUpscaleOptionId);
        const auto sprite_upscaling = static_cast<SpriteUpscalingOption>(std::get<uint32_t>(option_value));
        return sprite_upscaling == SpriteUpscalingOption::Upscaled;
    }
    catch (const std::exception&) {
        return true;
    }
}

void on_init(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;

    // librecomp clears func_map during init_overlays(), so absolute 0x8Fxxxxxx patch
    // bridge symbols must be restored here before the game starts executing.
    restore_runtime_patch_symbols();
}

std::string get_game_thread_name(const OSThread* t) {
    std::string name = "[Game] ";

    switch (t->id) {
        case 0:
            switch (t->priority) {
                case 150:
                    name += "PIMGR";
                    break;
                case 80:
                    name += "VIMGR";
                    break;
                default:
                    name += std::to_string(t->id);
                    break;
            }
            break;
        case 1:
            name += "IDLE";
            break;
        case 3:
            name += "MAIN";
            break;
        case 4:
            name += "AUDIO";
            break;
        case 5:
            name += "SCHED";
            break;
        case 6:
            name += "GRAPH";
            break;
        case 7:
            name += "RMON";
            break;
        default:
            name += std::to_string(t->id);
            break;
    }

    return name;
}

} // namespace sssv
