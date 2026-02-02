#ifndef __SSSV_GAME_H__
#define __SSSV_GAME_H__

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "recomp.h"
#include "ultramodern/ultra64.h"

namespace sssv {
    // Called when the game initializes
    void on_init(uint8_t* rdram, recomp_context* ctx);

    // Register all overlays
    void register_overlays();

    // Get thread name for debugging
    std::string get_game_thread_name(const OSThread* t);
}

#endif
