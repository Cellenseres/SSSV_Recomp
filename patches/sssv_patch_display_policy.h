#ifndef __SSSV_PATCH_DISPLAY_POLICY_H__
#define __SSSV_PATCH_DISPLAY_POLICY_H__

#include "sssv_patch_ui_domain.h"

// In widescreen the legacy TV-safe inset (8px) collapses to 0; overscan
// fullscreen/border rects are treated as full-frame draws.

static inline s16 display_policy_frame_width(void) {
    if (ui_is_expand_active()) {
        return compute_target_width_for_height_patch(get_safe_screen_height());
    }
    if (gScreenWidth > 0) {
        return gScreenWidth;
    }
    return SSSV_BASE_WIDTH;
}

static inline s16 display_policy_frame_height(void) {
    return (gScreenHeight > 0) ? gScreenHeight : SSSV_BASE_HEIGHT;
}

static inline s16 display_policy_safe_inset(void) {
    return ui_is_expand_active() ? 0 : SSSV_TV_SAFE_INSET;
}

static inline void display_policy_safe_scissor_bounds(s32* ulx, s32* uly, s32* lrx, s32* lry) {
    const s16 frame_w = display_policy_frame_width();
    const s16 frame_h = display_policy_frame_height();
    const s16 inset = display_policy_safe_inset();

    *ulx = inset;
    *uly = inset;
    *lrx = frame_w - inset;
    *lry = frame_h - inset;

    if (*lrx <= *ulx) {
        *ulx = 0;
        *lrx = frame_w;
    }
    if (*lry <= *uly) {
        *uly = 0;
        *lry = frame_h;
    }
}

static inline s32 is_legacy_fullscreen_or_overscan_rect(s16 x0, s16 y0, s16 x1, s16 y1) {
    const s16 frame_w = display_policy_frame_width();
    const s16 frame_h = display_policy_frame_height();
    const s16 legacy_inset = SSSV_TV_SAFE_INSET;

    // Accept both gScreenWidth (dynamic) and SSSV_BASE_WIDTH (hardcoded) as valid right edges.
    // Height is always 240; widescreen only stretches horizontally, so no dual height check.
    const s32 is_native_fullscreen =
        (x0 == 0) && (y0 == 0) &&
        (x1 == frame_w || x1 == SSSV_BASE_WIDTH) &&
        (y1 == frame_h);
    const s32 is_overscan_fullscreen =
        (x0 == legacy_inset) && (y0 == legacy_inset) &&
        (x1 == (frame_w - legacy_inset) || x1 == (SSSV_BASE_WIDTH - legacy_inset)) &&
        (y1 == (frame_h - legacy_inset));

    return is_native_fullscreen || is_overscan_fullscreen;
}

static inline s32 is_legacy_overscan_border_rect(s16 x0, s16 y0, s16 x1, s16 y1) {
    const s16 frame_w = display_policy_frame_width();
    const s16 frame_h = display_policy_frame_height();
    const s16 legacy_inset = SSSV_TV_SAFE_INSET;

    const s32 is_left =
        (x0 == 0) && (y0 == 0) &&
        (x1 == legacy_inset) && (y1 == frame_h);
    const s32 is_right =
        (x0 == (frame_w - legacy_inset)) && (y0 == 0) &&
        (x1 == frame_w) && (y1 == frame_h);
    const s32 is_top =
        (x0 == 0) && (y0 == 0) &&
        (x1 == frame_w) && (y1 == legacy_inset);
    const s32 is_bottom =
        (x0 == 0) && (y0 == (frame_h - legacy_inset)) &&
        (x1 == frame_w) && (y1 == frame_h);

    return is_left || is_right || is_top || is_bottom;
}

#endif
