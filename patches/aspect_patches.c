#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_patch_trace.h"
#include "sssv_patch_ui_domain.h"

static u32 g_dbg_set_screen_scaling_calls = 0;
static u32 g_dbg_ui_domain_calls = 0;
static s16 g_dbg_last_target_width = -1;
static s16 g_dbg_last_screen_width = -1;
static s16 g_dbg_last_vi_width = -1;
static s16 g_dbg_last_height = -1;

RECOMP_PATCH void set_screen_scaling(void) {
    const s16 safe_height = get_safe_screen_height();
    const s16 target_width = compute_target_width_for_height_patch(safe_height);

    rc_begin_frame();
    rc_set(RC_WORLD3D);

    gVIData.screenWidth = target_width;
    if (gScreenWidth != target_width) {
        gScreenWidth = target_width;
    }

    sync_terminal_right_column_x();
    sync_terminal_visible_queue_x();

    g_dbg_set_screen_scaling_calls++;
    if ((g_dbg_set_screen_scaling_calls <= 4) ||
        (g_dbg_set_screen_scaling_calls % 240 == 0) ||
        (target_width != g_dbg_last_target_width) ||
        (gScreenWidth != g_dbg_last_screen_width) ||
        (gVIData.screenWidth != g_dbg_last_vi_width) ||
        (safe_height != g_dbg_last_height)) {
        PATCH_TRACE(PATCH_TAG_SET_SCREEN_SCALING, g_dbg_set_screen_scaling_calls, (u16)target_width, (u16)gScreenWidth);
        g_dbg_last_target_width = target_width;
        g_dbg_last_screen_width = gScreenWidth;
        g_dbg_last_vi_width = gVIData.screenWidth;
        g_dbg_last_height = safe_height;
    }

    g_dbg_ui_domain_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_ui_domain_calls, 8, 300)) {
        PATCH_TRACE(PATCH_TAG_UI_DOMAIN, g_dbg_ui_domain_calls, (u32)rc_current(), (u16)ui_safe_width());
    }
}

RECOMP_PATCH void set_tv_mode_widescreen(void) {
    set_tv_mode_normal();
}
