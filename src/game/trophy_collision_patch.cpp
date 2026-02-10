#include <cstdint>

#include "recomp.h"

namespace {
constexpr gpr vram32(uint32_t v) {
    return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(v)));
}

constexpr gpr ADDR_FBM_TROPHY_HITBOX_SIZE = vram32(0x803AD3F3);
constexpr uint8_t FBM_TROPHY_HITBOX_FIXED_VALUE = 0x15;
}

extern "C" void sssv_patch_trophy_collision_guard(uint8_t* rdram, recomp_context* ctx) {
    if (MEM_BU(ADDR_FBM_TROPHY_HITBOX_SIZE, 0) != FBM_TROPHY_HITBOX_FIXED_VALUE) {
        MEM_B(ADDR_FBM_TROPHY_HITBOX_SIZE, 0) = FBM_TROPHY_HITBOX_FIXED_VALUE;
    }

    (void)ctx;
    (void)rdram;
}
