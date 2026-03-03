#ifndef __SSSV_RENDER_CONTEXT_H__
#define __SSSV_RENDER_CONTEXT_H__

#include "sssv_patch_common.h"
#include "sssv_transform_ids.h"

typedef enum {
    RC_WORLD3D = 0,
    RC_HUD_ORTHO = 1,
    RC_TRANSITION = 2,
    RC_OVERLAY = 3,
} RenderContext;

typedef enum {
    TRC_NONE = 0,
    TRC_WORLD_BILLBOARD = 1,
    TRC_WORLD_MASK = 2,
    TRC_HUD_UI = 3,
    TRC_TRANSITION = 4,
    TRC_OVERLAY = 5,
} TexRectContext;

typedef enum {
    FEATURE_RENDER_CONTEXT  = 1u << 0,
    FEATURE_RT64_TAGGING    = 1u << 1,
    /* bit 2 unused */
    FEATURE_CAMERA_CUT_SKIP = 1u << 3,
} SssvPatchFeatureBits;

typedef enum {
    RC_BILLBOARD_FAMILY_STAR = 1,
    RC_BILLBOARD_FAMILY_ENERGY = 2,
    RC_BILLBOARD_FAMILY_COLLECTIBLE = 3,
    RC_BILLBOARD_FAMILY_DUALSCALE = 4,
    RC_BILLBOARD_FAMILY_PARTICLE = 5,
} RcBillboardFamily;

extern RECOMP_EXPORT u32 sssv_patch_feature_flags;

extern s32 cur_perspective_projection_transform_id;
extern s32 cur_ortho_projection_transform_id;
extern s32 cur_drawn_model_transform_id;
extern s32 cur_drawn_model_skip_interpolation;
extern s32 skip_perspective_interpolation;

void rc_begin_frame(void);
void rc_end_frame(void);

void rc_push(RenderContext ctx);
void rc_pop(void);
RenderContext rc_current(void);
void rc_set(RenderContext ctx);

void rc_set_projection_ids_for_context(RenderContext ctx);

u32 rc_next_hud_transform_id(void);
u32 rc_next_overlay_transform_id(void);
u32 rc_next_world_mask_transform_id(void);

void rc_update_camera_cut_skip(void);
void rc_ensure_rt64_extended_enabled(Gfx** dl);
void rc_invalidate_rt64_extended_state(void);
void rc_mark_world_projection_active(void);
s32 rc_world_projection_active(void);
u32 rc_frame_index(void);
void rc_note_texrect_context(TexRectContext ctx, u32 func_id);

#endif
