#ifndef SSSV_LAUNCHER_H
#define SSSV_LAUNCHER_H

#include "recompui/recompui.h"

namespace sssv {
    void launcher_animation_setup(recompui::LauncherMenu* menu);
    void launcher_animation_update(recompui::LauncherMenu* menu);

    constexpr float launcher_options_right_position_start = 48.0f;
    constexpr float launcher_options_right_position_end = 24.0f + 24.0f;
    constexpr float launcher_options_top_offset = 48.0f;
    constexpr float launcher_options_title_offset = 120.0f;
}

#endif
