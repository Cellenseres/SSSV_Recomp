#include "sssv_config.h"
#include "sssv_game.h"
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

    // Finalize configuration
    recompui::config::finalize();
}

void on_init(uint8_t* rdram, recomp_context* ctx) {
    // Called when the game initializes
    // Add any SSSV-specific initialization here
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
