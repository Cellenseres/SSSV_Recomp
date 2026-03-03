#ifndef __SSSV_CONFIG_H__
#define __SSSV_CONFIG_H__

#include <string>

namespace sssv {
    inline const std::u8string program_id = u8"SSSVRecompiled";
    inline const std::string program_name = "Space Station Silicon Valley: Recompiled";

    void init_config();
}

#endif
