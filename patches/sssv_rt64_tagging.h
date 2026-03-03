#ifndef __SSSV_RT64_TAGGING_H__
#define __SSSV_RT64_TAGGING_H__

#include "sssv_render_context.h"

static inline s32 rt64_tagging_enabled(void) {
    return (sssv_patch_feature_flags & FEATURE_RT64_TAGGING) != 0;
}

static inline void rt64_tag_projection_matrix(Gfx** dl, u32 projection_id) {
    if (!rt64_tagging_enabled()) {
        return;
    }
    rc_ensure_rt64_extended_enabled(dl);
    gEXMatrixGroupSimpleNormal((*dl)++, projection_id, G_EX_NOPUSH, G_MTX_PROJECTION, G_EX_EDIT_NONE);
}

static inline void rt64_tag_model_matrix(Gfx** dl, u32 transform_id, s32 skip_rot) {
    if (!rt64_tagging_enabled()) {
        return;
    }
    rc_ensure_rt64_extended_enabled(dl);

    if (skip_rot != 0) {
        gEXMatrixGroupDecomposedVertsSkipRot((*dl)++, transform_id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    } else {
        gEXMatrixGroupDecomposedVerts((*dl)++, transform_id, G_EX_PUSH, G_MTX_MODELVIEW, G_EX_EDIT_NONE);
    }
}

static inline void rt64_pop_model_matrix(Gfx** dl) {
    if (!rt64_tagging_enabled()) {
        return;
    }
    rc_ensure_rt64_extended_enabled(dl);
    gEXPopMatrixGroup((*dl)++, G_MTX_MODELVIEW);
}

#endif
