#include "sssv_config.h"
#include "sssv_game.h"
#include "sssv_billboard_controls.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "ultramodern/config.hpp"
#include "librecomp/files.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace sssv {

void init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();

    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    // Create general options tab
    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;

    recompui::config::create_general_tab(general_options);

    // Create graphics tab
    recompui::config::create_graphics_tab();

    // Create controls tab
    recompui::config::create_controls_tab();

    // Create sound tab
    recompui::config::create_sound_tab();

    // Create mods tab
    recompui::config::create_mods_tab();

    // Billboard debug tab (visible in Debug builds; hidden in Release)
    {
        using recomp::config::ConfigValueVariant;
        using recomp::config::OptionChangeContext;

        recomp::config::Config &debug_config = recompui::config::create_config_tab("Debug", "debug", false);

        debug_config.add_bool_option(
            "disable_6fa3a4_render",
            "Disable 6FA3A4",
            "Debug: Suppress billboard draw in func_802E8CF4_6FA3A4 (animal FOV masks).",
            false);

        debug_config.add_bool_option(
            "disable_6c5e44_render",
            "Disable 6C5E44",
            "Debug: Suppress billboard draw in func_802B4794_6C5E44 (stars).",
            false);

        debug_config.add_bool_option(
            "disable_73f17c_render",
            "Disable 73F17C",
            "Debug: Suppress billboard draw in func_8032DACC_73F17C (energy items).",
            false);

        debug_config.add_bool_option(
            "disable_73f800_render",
            "Disable 73F800",
            "Debug: Suppress billboard draw in func_8032E150_73F800 (flowers/collectibles with LOD).",
            false);

        debug_config.add_bool_option(
            "disable_740094_render",
            "Disable 740094",
            "Debug: Suppress billboard draw in func_8032E9E4_740094 (collectibles, 2D scaling).",
            false);

        debug_config.add_bool_option(
            "disable_740820_render",
            "Disable 740820",
            "Debug: Suppress billboard draw in func_8032F170_740820 (tree tops/foliage).",
            false);

        // --- Ortho Quad Rewrite toggles (per-function) ---
        debug_config.add_bool_option("rewrite_6c5e44_ortho", "6C5E44 Ortho Quads",
            "Rewrite 6C5E44 (stars) TexRects to interpolated ortho quads.", true);
        debug_config.add_bool_option("rewrite_73f17c_ortho", "73F17C Ortho Quads",
            "Rewrite 73F17C (energy) TexRects to interpolated ortho quads.", true);
        debug_config.add_bool_option("rewrite_73f800_ortho", "73F800 Ortho Quads",
            "Rewrite 73F800 (flowers/collectibles) TexRects to interpolated ortho quads.", true);
        debug_config.add_bool_option("rewrite_740094_ortho", "740094 Ortho Quads",
            "Rewrite 740094 (collectibles 2D) TexRects to interpolated ortho quads.", true);
        debug_config.add_bool_option("rewrite_740820_ortho", "740820 Ortho Quads",
            "Rewrite 740820 (tree tops) TexRects to interpolated ortho quads.", true);

#if defined(NDEBUG)
        debug_config.add_bool_option("rewrite_6c5e44_suppress_original", "6C5E44 Hide Original",
            "Suppress original 6C5E44 draw after ortho rewrite.", true);
        debug_config.add_bool_option("rewrite_73f17c_suppress_original", "73F17C Hide Original",
            "Suppress original 73F17C draw after ortho rewrite.", true);
        debug_config.add_bool_option("rewrite_73f800_suppress_original", "73F800 Hide Original",
            "Suppress original 73F800 draw after ortho rewrite.", true);
        debug_config.add_bool_option("rewrite_740094_suppress_original", "740094 Hide Original",
            "Suppress original 740094 draw after ortho rewrite.", true);
        debug_config.add_bool_option("rewrite_740820_suppress_original", "740820 Hide Original",
            "Suppress original 740820 draw after ortho rewrite.", true);
        debug_config.add_bool_option("log_73f17c_ortho", "Billboard Debug Logs",
            "Log billboard stats and ortho rewrite diagnostics to console.", false);
#else
        debug_config.add_bool_option("rewrite_6c5e44_suppress_original", "6C5E44 Hide Original",
            "Suppress original 6C5E44 draw after ortho rewrite.", false);
        debug_config.add_bool_option("rewrite_73f17c_suppress_original", "73F17C Hide Original",
            "Suppress original 73F17C draw after ortho rewrite.", false);
        debug_config.add_bool_option("rewrite_73f800_suppress_original", "73F800 Hide Original",
            "Suppress original 73F800 draw after ortho rewrite.", false);
        debug_config.add_bool_option("rewrite_740094_suppress_original", "740094 Hide Original",
            "Suppress original 740094 draw after ortho rewrite.", false);
        debug_config.add_bool_option("rewrite_740820_suppress_original", "740820 Hide Original",
            "Suppress original 740820 draw after ortho rewrite.", false);
        debug_config.add_bool_option("log_73f17c_ortho", "Billboard Debug Logs",
            "Log billboard stats and ortho rewrite diagnostics to console.", true);
#endif

        debug_config.add_option_change_callback("disable_6fa3a4_render",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_disable_6fa3a4_render(*v);
                }
            });

        debug_config.add_option_change_callback("disable_6c5e44_render",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_disable_6c5e44_render(*v);
                }
            });

        debug_config.add_option_change_callback("disable_73f17c_render",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_disable_73f17c_render(*v);
                }
            });

        debug_config.add_option_change_callback("disable_73f800_render",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_disable_73f800_render(*v);
                }
            });

        debug_config.add_option_change_callback("disable_740094_render",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_disable_740094_render(*v);
                }
            });

        debug_config.add_option_change_callback("disable_740820_render",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_disable_740820_render(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_73f17c_ortho",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_73f17c_ortho(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_73f17c_suppress_original",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_73f17c_suppress_original(*v);
                }
            });

        debug_config.add_option_change_callback("log_73f17c_ortho",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_log_73f17c_ortho(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_6c5e44_ortho",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_6c5e44_ortho(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_6c5e44_suppress_original",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_6c5e44_suppress_original(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_73f800_ortho",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_73f800_ortho(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_73f800_suppress_original",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_73f800_suppress_original(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_740094_ortho",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_740094_ortho(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_740094_suppress_original",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_740094_suppress_original(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_740820_ortho",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_740820_ortho(*v);
                }
            });

        debug_config.add_option_change_callback("rewrite_740820_suppress_original",
            [](ConfigValueVariant cur, ConfigValueVariant, OptionChangeContext) {
                if (auto v = std::get_if<bool>(&cur)) {
                    sssv::billboard::set_rewrite_740820_suppress_original(*v);
                }
            });
    }

#if defined(NDEBUG)
    // Release: hide the Debug/Test tab from the config UI
    recompui::config::set_tab_visible("debug", false);
#endif

    // Finalize configuration
    recompui::config::finalize();
}

void on_init(uint8_t* rdram, recomp_context* ctx) {
    // Called when the game initializes
    (void)rdram;
    (void)ctx;
}

std::string get_game_thread_name(const OSThread* t) {
    std::string name = "[Game] ";

    // SSSV thread naming based on thread ID/priority
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
