#include <cmath>
#include <cstdio>

#include "recomp.h"
#include "ultramodern/ultra64.h"

namespace {
constexpr float kScaleEpsilon = 1e-6f;
}

// RecompiledFuncs routes osViSetXScale/YScale calls here (via CMake compile definitions).
// In Debug, ultramodern currently asserts on scale != 1.0f. Release effectively treats
// non-1.0 scale as a no-op, so mirror that behavior to keep Debug runnable.
extern "C" void sssv_osViSetXScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const float scale = ctx->f12.fl;

    if (std::fabs(scale - 1.0f) <= kScaleEpsilon) {
        osViSetXScale(scale);
        return;
    }

    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr, "[SSSV] Debug workaround: ignoring osViSetXScale(%f)\n", scale);
        warned = true;
    }
}

extern "C" void sssv_osViSetYScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const float scale = ctx->f12.fl;

    if (std::fabs(scale - 1.0f) <= kScaleEpsilon) {
        osViSetYScale(scale);
        return;
    }

    static bool warned = false;
    if (!warned) {
        std::fprintf(stderr, "[SSSV] Debug workaround: ignoring osViSetYScale(%f)\n", scale);
        warned = true;
    }
}
