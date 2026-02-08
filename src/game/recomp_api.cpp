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

// ============================================================================
// SSSV Widescreen Implementation
// ============================================================================
//
// Strategy: Let RT64's Expand mode handle viewport expansion
//
// How it works:
// 1. Don't modify gScreenWidth - keep game at native 320x240
// 2. Hook guPerspective to adjust projection matrix aspect ratio
// 3. Use gEXSetViewportAlign(G_EX_ORIGIN_CENTER) for proper centering
// 4. RT64's Expand aspect ratio mode expands the viewport automatically
//
// This approach avoids conflicts between our gScreenWidth modifications
// and RT64's internal widescreen handling. RT64 knows how to expand
// the viewport based on the window aspect ratio.
//
// Key addresses (for reference/debugging):
//   gScreenWidth     = 0x80203FD0
//   D_803F2D50.unkDA = 0x803F2E2A (source for gScreenWidth in overlay2)
//   D_80152EA8       = Main viewport
//   D_803B66F0       = UI viewport
//   D_8020540C       = Widescreen flag
// ============================================================================

namespace {

// Sign-extend 32-bit VRAM address for MEM_* macros
constexpr gpr vram32(uint32_t v) {
    return static_cast<gpr>(static_cast<int64_t>(static_cast<int32_t>(v)));
}

// Memory addresses
constexpr gpr ADDR_SCREEN_WIDTH      = vram32(0x80203FD0);
constexpr gpr ADDR_SCREEN_HEIGHT     = vram32(0x80203FD2);
constexpr gpr ADDR_VP_MAIN           = vram32(0x80152EA8);
constexpr gpr ADDR_VP_UI             = vram32(0x803B66F0);
constexpr gpr ADDR_WIDESCREEN_FLAG   = vram32(0x8020540C);
constexpr gpr ADDR_VIDATA_WIDTH      = vram32(0x802053EA);  // D_802053E0.screenWidth
constexpr gpr ADDR_D_803F2D50_UNKDA  = vram32(0x803F2E2A);  // Source for gScreenWidth
constexpr gpr ADDR_GFX_PTR           = vram32(0x801D9EB8);  // D_801D9EB8 (Gfx* display list)

// Base dimensions
constexpr float BASE_WIDTH  = 320.0f;
constexpr float BASE_HEIGHT = 240.0f;
constexpr float BASE_ASPECT = BASE_WIDTH / BASE_HEIGHT;  // 4:3

constexpr int16_t BASE_WIDTH_I  = 320;
constexpr int16_t BASE_HEIGHT_I = 240;

// Cached widescreen state
struct WidescreenState {
    bool enabled = false;
    int16_t target_width = BASE_WIDTH_I;
    float aspect_ratio = BASE_ASPECT;
    bool initialized = false;
};

WidescreenState g_widescreen;

// Helper: Write 16-bit value to VRAM
inline void write_s16(uint8_t* rdram, gpr vram, int16_t value) {
    MEM_H(0, vram) = value;
}

// Helper: Read 16-bit value from VRAM
inline int16_t read_s16(uint8_t* rdram, gpr vram) {
    return static_cast<int16_t>(MEM_H(0, vram));
}

inline uint16_t read_u16(uint8_t* rdram, gpr vram) {
    return static_cast<uint16_t>(MEM_H(0, vram));
}

inline uint32_t read_u32(uint8_t* rdram, gpr vram) {
    return static_cast<uint32_t>(MEM_W(0, vram));
}

// Helper: Write viewport scale and translate values
// N64 viewport uses 2x multiplier for subpixel precision
void write_viewport(uint8_t* rdram, gpr vram,
                    int16_t scale_x, int16_t scale_y,
                    int16_t trans_x, int16_t trans_y) {
    write_s16(rdram, vram + 0,  static_cast<int16_t>(scale_x * 2));  // vscale[0]
    write_s16(rdram, vram + 2,  static_cast<int16_t>(scale_y * 2));  // vscale[1]
    write_s16(rdram, vram + 8,  static_cast<int16_t>(trans_x * 2));  // vtrans[0]
    write_s16(rdram, vram + 10, static_cast<int16_t>(trans_y * 2));  // vtrans[1]
}

bool try_get_gfx_ptr(uint8_t* rdram, gpr vram_ptr, GfxCommand*& out_cmd, gpr& out_gdl, uint8_t*& out_gfx_mem) {
    gpr gdl = MEM_W(0, vram_ptr);
    if (gdl == 0) {
        return false;
    }

    int32_t gdl_signed = static_cast<int32_t>(gdl);
    uint32_t addr = static_cast<uint32_t>(gdl_signed - static_cast<int32_t>(0x80000000));
    if (addr >= 0x800000) {
        return false;
    }

    out_gfx_mem = rdram + addr;
    out_cmd = reinterpret_cast<GfxCommand*>(out_gfx_mem);
    out_gdl = gdl;
    return true;
}

void advance_gfx_ptr(uint8_t* rdram, uint8_t* gfx_mem, GfxCommand* cmd, gpr& gdl, gpr vram_ptr) {
    gdl = ADD32(gdl, static_cast<gpr>(reinterpret_cast<uint8_t*>(cmd) - gfx_mem));
    MEM_W(0, vram_ptr) = static_cast<int32_t>(gdl);
}

// Calculate target aspect ratio from window size
float calculate_target_aspect() {
    int window_width, window_height;
    recompui::get_window_size(window_width, window_height);

    if (window_width <= 0 || window_height <= 0) {
        return BASE_ASPECT;
    }

    float window_aspect = static_cast<float>(window_width) / static_cast<float>(window_height);

    // Never go narrower than original
    return std::max(window_aspect, BASE_ASPECT);
}

// Update widescreen state based on config and window size
void update_widescreen_state() {
    ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();

    bool should_expand = (config.ar_option == ultramodern::renderer::AspectRatio::Expand);

    if (should_expand) {
        int window_width, window_height;
        recompui::get_window_size(window_width, window_height);

        float target_aspect = calculate_target_aspect();
        int16_t target_width = static_cast<int16_t>(std::lround(BASE_HEIGHT * target_aspect));

        // Debug: log window size and calculated target
        static int last_ww = 0, last_wh = 0;
        if (window_width != last_ww || window_height != last_wh) {
            printf("[SSSV] window=%dx%d => aspect=%.3f => target_width=%d\n",
                   window_width, window_height, target_aspect, target_width);
            fflush(stdout);
            last_ww = window_width;
            last_wh = window_height;
        }

        g_widescreen.enabled = true;
        g_widescreen.aspect_ratio = target_aspect;
        g_widescreen.target_width = target_width;
    } else {
        g_widescreen.enabled = false;
        g_widescreen.aspect_ratio = BASE_ASPECT;
        g_widescreen.target_width = BASE_WIDTH_I;
    }

    g_widescreen.initialized = true;
}

} // anonymous namespace

// ============================================================================
// Exported API Functions
// ============================================================================

// Called by guPerspective hook to adjust aspect ratio parameter
extern "C" float sssv_get_target_aspect_ratio(float original) {
    ultramodern::renderer::GraphicsConfig config = ultramodern::renderer::get_graphics_config();

    if (config.ar_option == ultramodern::renderer::AspectRatio::Original) {
        return original;
    }

    // Sanity check
    if (!std::isfinite(original) || original < 0.1f || original > 10.0f) {
        return original;
    }

    float target = calculate_target_aspect();
    float result = std::max(target, original);

    // Debug output
    static float last_original = 0.0f, last_result = 0.0f;
    if (original != last_original || result != last_result) {
        printf("[SSSV] guPerspective: original=%.3f target=%.3f result=%.3f\n",
               original, target, result);
        fflush(stdout);
        last_original = original;
        last_result = result;
    }

    // Never make narrower than original
    return result;
}

// Force all widescreen-related memory locations to correct values
// This must be called BEFORE scissors are calculated in the render loop
extern "C" void sssv_force_widescreen_state(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;

    update_widescreen_state();

    if (!g_widescreen.enabled) {
        return;
    }

    // NEW APPROACH: Don't modify gScreenWidth!
    // Let RT64's Expand mode handle the viewport expansion.
    // We only adjust the projection matrix via guPerspective hook.
    //
    // The key insight is that RT64's widescreen works by:
    // 1. Receiving the 320x240 viewport from the game
    // 2. Expanding it horizontally based on the aspect ratio setting
    // 3. Using gEXSetViewportAlign to control centering
    //
    // If we modify gScreenWidth, we conflict with RT64's expansion.

    // Only set the widescreen flag for internal tracking
    MEM_W(0, ADDR_WIDESCREEN_FLAG) = 1;

    // Debug output
    static bool logged = false;
    if (!logged) {
        printf("[SSSV] Widescreen enabled - letting RT64 handle expansion\n");
        printf("[SSSV] Target aspect: %.3f (width would be %d)\n",
               g_widescreen.aspect_ratio, g_widescreen.target_width);
        fflush(stdout);
        logged = true;
    }
}

// Apply correct viewport for 3D rendering
// Called after game code may have reset the viewport
extern "C" void sssv_apply_viewport(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    (void)rdram;

    if (!g_widescreen.initialized) {
        update_widescreen_state();
    }

    // With the new approach, we don't modify viewports.
    // RT64 handles the expansion via its Expand aspect ratio mode.
    // The gEXSetViewportAlign command ensures proper centering.
}

// Combined function for hooks at the start of render functions
// Forces widescreen state AND applies viewport
extern "C" void sssv_widescreen_pre_render(uint8_t* rdram, recomp_context* ctx) {
    sssv_force_widescreen_state(rdram, ctx);
    sssv_apply_viewport(rdram, ctx);
}

// Called at end of frame to ensure viewport is correct for final render
extern "C" void sssv_widescreen_end_frame(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;

    if (!g_widescreen.initialized) {
        update_widescreen_state();
    }

    if (!g_widescreen.enabled) {
        return;
    }

    // Re-apply viewport in case it was modified
    sssv_apply_viewport(rdram, ctx);
}

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

    // Enable extended GBI mode (1 command)
    gEXEnable(cmd);
    cmd++;

    // Enable extended RDRAM addressing (1 command)
    gEXSetRDRAMExtended(cmd, 1);
    cmd++;

    // Set viewport alignment to center - this tells RT64 to center the
    // viewport in the output window, which is crucial for widescreen
    // gEXSetViewportAlign is G_EX_COMMAND2 = 2 commands
    if (g_widescreen.enabled) {
        gEXSetViewportAlign(cmd, G_EX_ORIGIN_CENTER, 0, 0);
        cmd += 2;
    }

    gdl = ADD32(gdl, static_cast<gpr>(
        reinterpret_cast<uint8_t*>(cmd) - gfx_mem));
    MEM_W(0, gdl_ptr_ptr) = static_cast<int32_t>(gdl);
}

// ============================================================================
// Legacy API - kept for compatibility with existing hooks
// ============================================================================

extern "C" void sssv_apply_screen_aspect_ratio(uint8_t* rdram, recomp_context* ctx) {
    sssv_force_widescreen_state(rdram, ctx);
    sssv_apply_viewport(rdram, ctx);
}

extern "C" void sssv_apply_expanded_viewport_end_frame(uint8_t* rdram, recomp_context* ctx) {
    sssv_widescreen_end_frame(rdram, ctx);
}

extern "C" void sssv_apply_expanded_viewport_main(uint8_t* rdram, recomp_context* ctx) {
    sssv_apply_viewport(rdram, ctx);
}

// Debug logging - always enabled for now
extern "C" void sssv_log_screen_state(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;

    int16_t sw = read_s16(rdram, ADDR_SCREEN_WIDTH);
    int16_t sh = read_s16(rdram, ADDR_SCREEN_HEIGHT);
    int16_t vsx = read_s16(rdram, ADDR_VP_MAIN + 0) / 2;
    int16_t vsy = read_s16(rdram, ADDR_VP_MAIN + 2) / 2;
    int16_t vtx = read_s16(rdram, ADDR_VP_MAIN + 8) / 2;
    int16_t vty = read_s16(rdram, ADDR_VP_MAIN + 10) / 2;

    int16_t ui_vsx = read_s16(rdram, ADDR_VP_UI + 0) / 2;
    int16_t ui_vtx = read_s16(rdram, ADDR_VP_UI + 8) / 2;

    int16_t unkda = read_s16(rdram, ADDR_D_803F2D50_UNKDA);
    int16_t viwidth = read_s16(rdram, ADDR_VIDATA_WIDTH);

    static int16_t last_sw = 0, last_vsx = 0, last_vtx = 0;
    if (sw != last_sw || vsx != last_vsx || vtx != last_vtx) {
        printf("[SSSV] gScreen=%dx%d unkDA=%d viW=%d\n", sw, sh, unkda, viwidth);
        printf("[SSSV] main_vp: scale=%d,%d trans=%d,%d\n", vsx, vsy, vtx, vty);
        printf("[SSSV] ui_vp: scale=%d trans=%d\n", ui_vsx, ui_vtx);
        fflush(stdout);
        last_sw = sw; last_vsx = vsx; last_vtx = vtx;
    }
}

extern "C" void sssv_log_screen_state_after(uint8_t* rdram, recomp_context* ctx) {
    sssv_log_screen_state(rdram, ctx);
}

// Called AFTER game's viewport setup - just for debugging now
extern "C" void sssv_fix_viewport_after_game(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;

    if (!g_widescreen.initialized) {
        update_widescreen_state();
    }

    if (!g_widescreen.enabled) {
        return;
    }

    // Just log viewport state for debugging - don't modify
    int16_t cur_vsx = read_s16(rdram, ADDR_VP_MAIN + 0);
    int16_t cur_vtx = read_s16(rdram, ADDR_VP_MAIN + 8);

    static int16_t last_cur_vsx = 0;
    if (cur_vsx != last_cur_vsx) {
        printf("[SSSV] game viewport: vscale=%d vtrans=%d (320 expected for 4:3)\n",
               cur_vsx / 2, cur_vtx / 2);
        fflush(stdout);
        last_cur_vsx = cur_vsx;
    }
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
