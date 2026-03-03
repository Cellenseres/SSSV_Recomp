#ifndef __SSSV_PATCH_UI_DOMAIN_H__
#define __SSSV_PATCH_UI_DOMAIN_H__

#include "sssv_patch_common.h"

static inline f32 get_target_aspect(void) {
    f32 target_aspect = recomp_get_target_aspect_ratio(SSSV_BASE_ASPECT);
    if (target_aspect < SSSV_BASE_ASPECT) {
        target_aspect = SSSV_BASE_ASPECT;
    }
    return target_aspect;
}

static inline s16 compute_target_width_for_height_patch(s16 source_height) {
    const f32 target_aspect = get_target_aspect();
    if (target_aspect <= (SSSV_BASE_ASPECT + 0.000001f)) {
        return SSSV_BASE_WIDTH;
    }

    s32 safe_height = (source_height > 0) ? source_height : SSSV_BASE_HEIGHT;
    s32 width = (s32)((safe_height * target_aspect) + 0.5f);
    if (width < SSSV_BASE_WIDTH) {
        width = SSSV_BASE_WIDTH;
    }
    if (width > 0x7FFE) {
        width = 0x7FFE;
    }

    if ((width & 1) != 0) {
        width -= 1;
    }

    return (s16)width;
}

static inline s16 get_safe_screen_height(void) {
    return (gScreenHeight > 0) ? gScreenHeight : SSSV_BASE_HEIGHT;
}

static inline s32 ui_is_expand_active(void) {
    return compute_target_width_for_height_patch(get_safe_screen_height()) > SSSV_BASE_WIDTH;
}

static inline s16 ui_safe_width(void) {
    if (ui_is_expand_active()) {
        return SSSV_BASE_WIDTH;
    }
    if (gScreenWidth <= 0) {
        return SSSV_BASE_WIDTH;
    }
    if (gScreenWidth > SSSV_BASE_WIDTH) {
        return SSSV_BASE_WIDTH;
    }
    return gScreenWidth;
}

static inline u16 ui_safe_center_x(void) {
    return (u16)(ui_safe_width() >> 1);
}

static inline u16 ui_safe_right_anchor(u16 margin) {
    s16 width = ui_safe_width();
    if (margin >= (u16)width) {
        margin = (u16)(width - 1);
    }
    return (u16)(width - margin);
}

static inline s32 terminal_ui_active(void) {
    return (gCameraUiState == 3) || (gCameraUiState == 4);
}

static inline void sync_terminal_right_column_x(void) {
    s16 i;
    s16 safe_width;

    if (!ui_is_expand_active() || !terminal_ui_active()) {
        return;
    }

    safe_width = ui_safe_width();
    for (i = 0; i < 13; i++) {
        s16 x = (s16)(safe_width - 14);
        if (gTerminalStatText[i] != NULL) {
            x = (s16)((safe_width - get_message_width(gTerminalStatText[i])) - 14);
        }
        gTerminalStatTextX[i] = x;
    }
}

#endif
