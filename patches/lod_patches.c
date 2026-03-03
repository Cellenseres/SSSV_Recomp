#include "sssv_patch_common.h"
#include "sssv_render_context.h"

static inline void apply_lod_overrides(s32* arg3) {

#if SSSV_PATCH_DISABLE_LOD || SSSV_PATCH_DISABLE_FAR_AWAY_CULL
    *arg3 = 0;
    gLodDetailState = 0;
#endif
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
    apply_lod_overrides(&arg3);

    if (arg3 == 0) {
        if (is_world_cell_loaded_6AB9E4(xPos >> 16, zPos >> 16, yPos >> 16) != 0) {
            return VISIBILITY_VISIBLE;
        } else {
            return VISIBILITY_INVISIBLE;
        }
    }

    if (gCameraVisibilityMask[6] & 3) {
        if (gCameraVisibilityMask[6] & 1) {
            if ((classify_visibility_simple(xPos, zPos, ((gCameraVisibilityMask[6] & 0xFFC) << 0x12) - yPos, arg3, arg8) == 0) && (arg9 == 0)) {
                return VISIBILITY_VISIBLE;
            } else {
                return classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9);
            }
        }

        if ((classify_visibility_simple(((gCameraVisibilityMask[6] & 0xFFC) << 0x12) - xPos, zPos, yPos, arg3, arg8) == 0) && (arg9 == 0)) {
            return VISIBILITY_VISIBLE;
        } else {
            return classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9);
        }
    } else {
        return classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9);
    }
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
    apply_lod_overrides(&arg3);

    if (arg3 == 0) {
        if (is_world_cell_loaded_6AB9E4(xPos >> 16, zPos >> 16, yPos >> 16) != 0) {
            return VISIBILITY_VISIBLE;
        } else {
            return VISIBILITY_INVISIBLE;
        }
    }

    if (gCameraVisibilityMask[6] & 1) {
        if ((classify_visibility_simple(xPos, zPos, ((gCameraVisibilityMask[6] & 0xFFC) << 0x12) - yPos, arg3, arg8) == 0) && (arg9 == 0)) {
            return VISIBILITY_VISIBLE;
        }

        return classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9);
    } else {
        return classify_visibility_and_draw_fov_mask(xPos, zPos, yPos, arg3, fovImageIdx, arg5, arg6, arg7, arg8, arg9);
    }
}
