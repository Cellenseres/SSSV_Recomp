#ifndef __SSSV_CONFIG_H__
#define __SSSV_CONFIG_H__

#include <filesystem>
#include <string>
#include <string_view>

namespace sssv {
    inline const std::u8string program_id = u8"SSSVRecompiled";
    inline const std::string program_name = "Space Station Silicon Valley: Recompiled";

    // Initialize configuration system
    void init_config();
}

#endif
