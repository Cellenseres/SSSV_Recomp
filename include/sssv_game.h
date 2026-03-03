#ifndef __SSSV_GAME_H__
#define __SSSV_GAME_H__

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "recomp.h"
#include "ultramodern/ultra64.h"

namespace sssv {
    void on_init(uint8_t* rdram, recomp_context* ctx);

    void register_overlays();

    void register_patches();

    void restore_runtime_patch_symbols();

    std::string get_game_thread_name(const OSThread* t);
}

#endif
