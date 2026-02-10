#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstring>

#include "recomp.h"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "rt64_extended_gbi.h"

// Enable RT64 extended GBI features
// Must be called early in display list construction
extern "C" void sssv_enable_extended_gbi(uint8_t* rdram, recomp_context* ctx) {
    gpr gdl_ptr_ptr = ctx->r4;
    gpr gdl = MEM_W(0, gdl_ptr_ptr);

    if (gdl == 0) {
        return;
    }

    int32_t gdl_signed = static_cast<int32_t>(gdl);
    uint8_t* gfx_mem = rdram + (gdl_signed - static_cast<int32_t>(0x80000000));
    auto* cmd = reinterpret_cast<GfxCommand*>(gfx_mem);

    // Enable extended GBI mode
    gEXEnable(cmd);
    cmd++;

    // Enable extended RDRAM addressing
    gEXSetRDRAMExtended(cmd, 1);
    cmd++;


    gdl = ADD32(gdl, static_cast<gpr>(
        reinterpret_cast<uint8_t*>(cmd) - gfx_mem));
    MEM_W(0, gdl_ptr_ptr) = static_cast<int32_t>(gdl);
}


// ============================================================================
// Required Runtime Functions
// ============================================================================

extern "C" void __ll_rshift_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    int64_t a = (static_cast<int64_t>(ctx->r4) << 32) | (static_cast<uint32_t>(ctx->r5));
    int64_t b = (static_cast<int64_t>(ctx->r6) << 32) | (static_cast<uint32_t>(ctx->r7));
    int64_t ret = a >> b;

    ctx->r2 = static_cast<int32_t>(ret >> 32);
    ctx->r3 = static_cast<int32_t>(ret);
}

extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = 11;  // PFS_ERR_DEVICE - report controller pak not present
}

extern "C" void __osEnqueueThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    auto queue = static_cast<PTR(PTR(OSThread))>(ctx->r4);
    auto thread = static_cast<PTR(OSThread)>(ctx->r5);
    ultramodern::thread_queue_insert(PASS_RDRAM queue, thread);
    ctx->r2 = 0;
}

extern "C" void __osPopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    auto queue = static_cast<PTR(PTR(OSThread))>(ctx->r4);
    PTR(OSThread) thread = ultramodern::thread_queue_pop(PASS_RDRAM queue);
    ctx->r2 = static_cast<int32_t>(thread);
}
