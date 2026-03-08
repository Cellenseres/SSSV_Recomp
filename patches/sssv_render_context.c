#include "sssv_render_context.h"

#include "sssv_patch_trace.h"

#define RC_STACK_CAPACITY 16
#define RC_COUNTER_MASK 0x00FFFFFFu

#define RC_ERR_STACK_OVERFLOW  0xC0570001u
#define RC_ERR_STACK_UNDERFLOW 0xC0570002u
#define RC_ERR_INVALID_CTX     0xC0570003u

#define RC_CAMERA_CUT_THRESHOLD 600u
RECOMP_EXPORT u32 sssv_patch_feature_flags =
    FEATURE_RENDER_CONTEXT |
    FEATURE_RT64_TAGGING |
    FEATURE_CAMERA_CUT_SKIP;

RECOMP_EXPORT u32 sssv_patch_diag_flags = 0;

s32 cur_perspective_projection_transform_id = 0;
s32 cur_ortho_projection_transform_id = 0;
s32 cur_drawn_model_transform_id = 0;
s32 cur_drawn_model_skip_interpolation = 0;
s32 skip_perspective_interpolation = 0;

static RenderContext g_rc_stack[RC_STACK_CAPACITY];
static s32 g_rc_stack_top = -1;

static u32 g_hud_counter = 0;
static u32 g_overlay_counter = 0;
static u32 g_world_mask_counter = 0;
static u32 g_billboard_star_counter = 0;
static u32 g_billboard_energy_counter = 0;
static u32 g_billboard_collectible_counter = 0;
static u32 g_billboard_dualscale_counter = 0;
static u32 g_billboard_particle_counter = 0;
static u32 g_rt64_extended_slot_mask = 0;
static s32 g_rt64_extended_unknown_enabled = 0;
static Gfx** g_rt64_extended_unknown_dl_slot = NULL;
static s32 g_world_projection_active = 0;
static u32 g_frame_index = 0;
static u32 g_texrect_trace_mask = 0;

static s16 g_prev_cam_world_x = 0;
static s16 g_prev_cam_world_z = 0;
static s16 g_prev_cam_world_y = 0;
static s32 g_prev_cam_valid = 0;

enum {
    RT64_EXT_SLOT_MAIN = 1u << 0,
    RT64_EXT_SLOT_WORLD = 1u << 1,
    RT64_EXT_SLOT_AUX = 1u << 2,
};

static u32 abs_s16_delta(s16 a, s16 b) {
    s32 d = (s32)a - (s32)b;
    return (u32)((d < 0) ? -d : d);
}

static inline u32 rc_rt64_slot_bit(Gfx** dl) {
    if (dl == &gMainDL) {
        return RT64_EXT_SLOT_MAIN;
    }
    if (dl == &gLayer0DL) {
        return RT64_EXT_SLOT_WORLD;
    }
    if (dl == &gAuxDL) {
        return RT64_EXT_SLOT_AUX;
    }
    return 0u;
}

static u32 next_id_in_bucket(u32 start, u32* counter) {
    u32 idx = (*counter) & RC_COUNTER_MASK;
    u32 id = start + idx;
    (*counter) = idx + 1;
    if ((*counter & RC_COUNTER_MASK) == 0) {
        PATCH_TRACE_DIAG(SSSV_DIAG_WARN, PATCH_TAG_ID_COLLISION, start, 0, 0);
    }
    return id;
}

void rc_set_projection_ids_for_context(RenderContext ctx) {
    if ((sssv_patch_feature_flags & FEATURE_RENDER_CONTEXT) == 0) {
        cur_perspective_projection_transform_id = 0;
        cur_ortho_projection_transform_id = 0;
        return;
    }

    switch (ctx) {
        case RC_WORLD3D:
            cur_perspective_projection_transform_id = SSSV_PROJECTION_WORLD_TRANSFORM_ID;
            cur_ortho_projection_transform_id = 0;
            break;
        case RC_HUD_ORTHO:
            cur_perspective_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
            cur_ortho_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
            break;
        case RC_TRANSITION:
            cur_perspective_projection_transform_id = SSSV_PROJECTION_TRANSITION_TRANSFORM_ID;
            cur_ortho_projection_transform_id = SSSV_PROJECTION_TRANSITION_TRANSFORM_ID;
            break;
        case RC_OVERLAY:
            cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_TRANSFORM_ID;
            cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_TRANSFORM_ID;
            break;
        default:
            cur_perspective_projection_transform_id = 0;
            cur_ortho_projection_transform_id = 0;
            break;
    }

    if (ctx != RC_WORLD3D) {
        g_world_projection_active = 0;
    }

    PATCH_TRACE_DIAG(SSSV_DIAG_CONTEXT, PATCH_TAG_PROJECTION_ID, (u32)ctx, (u32)cur_perspective_projection_transform_id, (u32)cur_ortho_projection_transform_id);
}

void rc_begin_frame(void) {
    g_frame_index++;
    g_texrect_trace_mask = 0;
    g_rc_stack_top = -1;
    g_hud_counter = 0;
    g_overlay_counter = 0;
    g_world_mask_counter = 0;
    cur_drawn_model_transform_id = 0;
    cur_drawn_model_skip_interpolation = 0;
    skip_perspective_interpolation = 0;
    g_rt64_extended_slot_mask = 0;
    g_rt64_extended_unknown_enabled = 0;
    g_rt64_extended_unknown_dl_slot = NULL;
    g_world_projection_active = 0;

    rc_push(RC_WORLD3D);
    PATCH_TRACE_DIAG(SSSV_DIAG_CONTEXT, PATCH_TAG_FRAME_BEGIN, g_frame_index, (u32)rc_current(), 0);
}

void rc_end_frame(void) {
    PATCH_TRACE_DIAG(SSSV_DIAG_CONTEXT, PATCH_TAG_FRAME_END, g_frame_index, (u32)((g_rc_stack_top >= 0) ? g_rc_stack_top : 0), (u32)cur_perspective_projection_transform_id);
    g_rc_stack_top = -1;
    cur_perspective_projection_transform_id = 0;
    cur_ortho_projection_transform_id = 0;
    cur_drawn_model_transform_id = 0;
    cur_drawn_model_skip_interpolation = 0;
    skip_perspective_interpolation = 0;
    g_rt64_extended_slot_mask = 0;
    g_rt64_extended_unknown_enabled = 0;
    g_rt64_extended_unknown_dl_slot = NULL;
    g_world_projection_active = 0;
}

void rc_push(RenderContext ctx) {
    if (g_rc_stack_top + 1 >= RC_STACK_CAPACITY) {
        PATCH_TRACE_DIAG(SSSV_DIAG_WARN, PATCH_TAG_ID_COLLISION, RC_ERR_STACK_OVERFLOW, (u32)ctx, (u32)g_rc_stack_top);
        return;
    }

    g_rc_stack[++g_rc_stack_top] = ctx;
    rc_set_projection_ids_for_context(ctx);
    PATCH_TRACE_DIAG(SSSV_DIAG_CONTEXT, PATCH_TAG_CONTEXT_ENTER, g_frame_index, (u32)ctx, (u32)cur_perspective_projection_transform_id);
}

void rc_pop(void) {
    RenderContext old_ctx = rc_current();

    if (g_rc_stack_top < 0) {
        PATCH_TRACE_DIAG(SSSV_DIAG_WARN, PATCH_TAG_ID_COLLISION, RC_ERR_STACK_UNDERFLOW, 0, 0);
        return;
    }

    g_rc_stack_top--;
    if (g_rc_stack_top >= 0) {
        rc_set_projection_ids_for_context(g_rc_stack[g_rc_stack_top]);
    } else {
        cur_perspective_projection_transform_id = 0;
        cur_ortho_projection_transform_id = 0;
    }

    PATCH_TRACE_DIAG(SSSV_DIAG_CONTEXT, PATCH_TAG_CONTEXT_EXIT, g_frame_index, (u32)old_ctx, (u32)cur_perspective_projection_transform_id);
}

RenderContext rc_current(void) {
    if (g_rc_stack_top < 0) {
        return RC_WORLD3D;
    }
    return g_rc_stack[g_rc_stack_top];
}

void rc_set(RenderContext ctx) {
    if (g_rc_stack_top < 0) {
        rc_push(ctx);
        return;
    }

    g_rc_stack[g_rc_stack_top] = ctx;
    rc_set_projection_ids_for_context(ctx);
    // Reuse CONTEXT_ENTER: rc_set replaces rather than pushes, diagnostic effect is the same.
    PATCH_TRACE_DIAG(SSSV_DIAG_CONTEXT, PATCH_TAG_CONTEXT_ENTER, g_frame_index, (u32)ctx, (u32)cur_perspective_projection_transform_id);
}

u32 rc_next_hud_transform_id(void) {
    return next_id_in_bucket(SSSV_HUD_ID_START, &g_hud_counter);
}

u32 rc_next_overlay_transform_id(void) {
    return next_id_in_bucket(SSSV_OVERLAY_ID_START, &g_overlay_counter);
}

u32 rc_next_world_mask_transform_id(void) {
    return next_id_in_bucket(SSSV_WORLD_MASK_ID_START, &g_world_mask_counter);
}

u32 rc_alloc_billboard_transform_id(RcBillboardFamily family) {
    switch (family) {
        case RC_BILLBOARD_FAMILY_STAR:
            return next_id_in_bucket(SSSV_BILLBOARD_STAR_ID_START, &g_billboard_star_counter);
        case RC_BILLBOARD_FAMILY_ENERGY:
            return next_id_in_bucket(SSSV_BILLBOARD_ENERGY_ID_START, &g_billboard_energy_counter);
        case RC_BILLBOARD_FAMILY_COLLECTIBLE:
            return next_id_in_bucket(SSSV_BILLBOARD_COLLECTIBLE_ID_START, &g_billboard_collectible_counter);
        case RC_BILLBOARD_FAMILY_DUALSCALE:
            return next_id_in_bucket(SSSV_BILLBOARD_DUALSCALE_ID_START, &g_billboard_dualscale_counter);
        case RC_BILLBOARD_FAMILY_PARTICLE:
            return next_id_in_bucket(SSSV_BILLBOARD_PARTICLE_ID_START, &g_billboard_particle_counter);
        default:
            PATCH_TRACE_DIAG(SSSV_DIAG_WARN, PATCH_TAG_ID_COLLISION, RC_ERR_INVALID_CTX, (u32)family, 0);
            return next_id_in_bucket(SSSV_BILLBOARD_PARTICLE_ID_START, &g_billboard_particle_counter);
    }
}

void rc_update_camera_cut_skip(void) {
    u32 delta_sum;

    if ((sssv_patch_feature_flags & FEATURE_CAMERA_CUT_SKIP) == 0) {
        skip_perspective_interpolation = 0;
        return;
    }

    if (!g_prev_cam_valid) {
        g_prev_cam_world_x = (s16)gCameraEyeWorldX;
        g_prev_cam_world_z = (s16)gCameraEyeWorldZ;
        g_prev_cam_world_y = (s16)gCameraEyeWorldY;
        g_prev_cam_valid = 1;
        skip_perspective_interpolation = 0;
        return;
    }

    delta_sum = abs_s16_delta((s16)gCameraEyeWorldX, g_prev_cam_world_x) +
                abs_s16_delta((s16)gCameraEyeWorldZ, g_prev_cam_world_z) +
                abs_s16_delta((s16)gCameraEyeWorldY, g_prev_cam_world_y);

    skip_perspective_interpolation = (delta_sum > RC_CAMERA_CUT_THRESHOLD) ? 1 : 0;
    if (skip_perspective_interpolation != 0) {
        PATCH_TRACE_DIAG(SSSV_DIAG_WARN, PATCH_TAG_CAMERA_CUT_SKIP, delta_sum, (u16)gCameraEyeWorldX, (u16)gCameraEyeWorldZ);
    }

    g_prev_cam_world_x = (s16)gCameraEyeWorldX;
    g_prev_cam_world_z = (s16)gCameraEyeWorldZ;
    g_prev_cam_world_y = (s16)gCameraEyeWorldY;
}

void rc_ensure_rt64_extended_enabled(Gfx** dl) {
    u32 slot_bit;

    if ((dl == NULL) || ((*dl) == NULL)) {
        return;
    }
    if ((sssv_patch_feature_flags & FEATURE_RT64_TAGGING) == 0) {
        return;
    }
    slot_bit = rc_rt64_slot_bit(dl);
    if ((slot_bit != 0u) && ((g_rt64_extended_slot_mask & slot_bit) != 0u)) {
        return;
    }
    if ((slot_bit == 0u) && (g_rt64_extended_unknown_enabled != 0) && (g_rt64_extended_unknown_dl_slot == dl)) {
        return;
    }

    gEXEnable((*dl)++);
    if (slot_bit != 0u) {
        g_rt64_extended_slot_mask |= slot_bit;
    } else {
        g_rt64_extended_unknown_enabled = 1;
        g_rt64_extended_unknown_dl_slot = dl;
    }
}

void rc_invalidate_rt64_extended_state(void) {
    g_rt64_extended_slot_mask = 0;
    g_rt64_extended_unknown_enabled = 0;
    g_rt64_extended_unknown_dl_slot = NULL;
}

void rc_mark_world_projection_active(void) {
    g_world_projection_active = 1;
}

s32 rc_world_projection_active(void) {
    return g_world_projection_active;
}

u32 rc_frame_index(void) {
    return g_frame_index;
}

void rc_note_texrect_context(TexRectContext ctx, u32 func_id) {
    u32 ctx_bit;

    if ((ctx <= TRC_NONE) || (ctx > TRC_OVERLAY)) {
        PATCH_TRACE_DIAG(SSSV_DIAG_WARN, PATCH_TAG_ID_COLLISION, RC_ERR_INVALID_CTX, (u32)ctx, func_id);
        return;
    }

    ctx_bit = 1u << ((u32)ctx - 1u);
    if ((g_texrect_trace_mask & ctx_bit) != 0u) {
        return;
    }

    g_texrect_trace_mask |= ctx_bit;
    PATCH_TRACE_DIAG(SSSV_DIAG_TEXRECT, PATCH_TAG_TEXRECT_CLASS, g_frame_index, (u32)ctx, func_id);
}
