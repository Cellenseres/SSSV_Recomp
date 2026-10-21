#include "sssv_patch_common.h"
#include "sssv_render_context.h"

// Forcing every object to VISIBILITY_VISIBLE changes more than rendering: the game also acts on the
// out-of-bounds/too-far results, and the camera oscillates (it never settles, so it shakes while the
// player moves). The far-away cull override therefore stays off by default; the LOD override only
// raises the detail level of objects the game itself classified as visible.
static inline void apply_far_away_cull_override(s32* arg3) {
#if SSSV_PATCH_DISABLE_FAR_AWAY_CULL
    *arg3 = 0;
    gLodDetailState = 0;
#endif
}

static inline s16 apply_lod_override(s16 visibility) {
#if SSSV_PATCH_DISABLE_LOD
    if (visibility == VISIBILITY_VISIBLE) {
        gLodDetailState = 0;
    }
#endif
    return visibility;
}

// Near a reflecting dome wall (gCameraVisibilityMask[6], see ship_window_patches.c) an object also counts as
// visible when its mirror image is, so its reflection is drawn even while the object itself is off screen.
static inline s32 mirror_image_visible(s32 xPos, s32 zPos, s32 yPos, s32 arg3, s8 arg8, u8 arg9, s32 allow_x_mirror) {
    const s32 plane = (gCameraVisibilityMask[6] & 0xFFC) << 0x12;

    if (arg9 != 0) {
        return 0;
    }
    if (gCameraVisibilityMask[6] & 1) {
        return classify_visibility_simple(xPos, zPos, plane - yPos, arg3, arg8) == 0;
    }
    if (allow_x_mirror && (gCameraVisibilityMask[6] & 2)) {
        return classify_visibility_simple(plane - xPos, zPos, yPos, arg3, arg8) == 0;
    }
    return 0;
}

RECOMP_PATCH s16 classify_object_visibility_6FA0A0(
    s32 xPos,
    s32 zPos,
    s32 yPos,
    s32 arg3,
    u8 fovImageIdx,
    s16 arg5,
    s16 arg6,
    s16 arg7,
    s8 arg8,
    u8 arg9
) {
    rc_set(RC_WORLD3D);
    apply_far_away_cull_override(&arg3);

    if (arg3 == 0) {
        if (is_world_cell_loaded_6AB9E4(xPos >> 16, zPos >> 16, yPos >> 16) != 0) {
            return VISIBILITY_VISIBLE;
        } else {
            return VISIBILITY_INVISIBLE;
        }
    }

    if (mirror_image_visible(xPos, zPos, yPos, arg3, arg8, arg9, 1)) {
        return apply_lod_override(VISIBILITY_VISIBLE);
    }

    return apply_lod_override(classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9));
}

RECOMP_PATCH s16 classify_dynamic_visibility_6FA26C(
    s32 xPos,
    s32 zPos,
    s32 yPos,
    s32 arg3,
    u8 fovImageIdx,
    s16 arg5,
    s16 arg6,
    s16 arg7,
    s8 arg8,
    u8 arg9
) {
    rc_set(RC_WORLD3D);
    apply_far_away_cull_override(&arg3);

    if (arg3 == 0) {
        if (is_world_cell_loaded_6AB9E4(xPos >> 16, zPos >> 16, yPos >> 16) != 0) {
            return VISIBILITY_VISIBLE;
        } else {
            return VISIBILITY_INVISIBLE;
        }
    }

    if (mirror_image_visible(xPos, zPos, yPos, arg3, arg8, arg9, 0)) {
        return apply_lod_override(VISIBILITY_VISIBLE);
    }

    return apply_lod_override(classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9));
}
