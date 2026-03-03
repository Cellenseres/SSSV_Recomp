#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_patch_trace.h"

#define MTX_INTPART_PACK(w1, w2)  (((w1) & 0xFFFF0000) | (((w2) & 0xFFFF0000) >> 16))
#define MTX_FRACPART_PACK(w1, w2) (((w1) << 16) | ((w2) & 0xFFFF))
#define SQ(x) ((x) * (x))

#define ENERGY_RGBA_TEX_PHYS_BASE 0x01040CB0u
#define ENERGY_MASK_TEX_PHYS_BASE 0x0103ECB0u
#define ENERGY_TEX_PHYS_WINDOW    0x00020000u

#define ENERGY_DIAG_DL_WINDOW_CMDS 			4096u // max forward scan before tracker resets
#define ENERGY_DIAG_BACKTRACK_CMDS_CURRENT 	1024  // backtrack limit in the current DL
#define ENERGY_DIAG_BACKTRACK_CMDS_LAYER 	2048  // backtrack limit in layer0/aux DLs

enum {
    ENERGY_SRC_NONE = 0u,
    ENERGY_SRC_RGBA = 1u << 0,
    ENERGY_SRC_MASK = 1u << 1,
};

static inline f32 f32_min(f32 a, f32 b) {
    return (a < b) ? a : b;
}

typedef struct {
    s32 center_x4;
    s32 center_y4;
    s32 scale_x4;
    s32 scale_y4;
    s32 left_x4;
    s32 right_x4;
    s32 top_y4;
    s32 bottom_y4;
    s32 span_x4;
} BillboardScreenSpace;

typedef struct {
    f32 left;
    f32 right;
    f32 top;
    f32 bottom;
} BillboardClipBounds;

typedef struct {
    f32 pos_x;
    f32 pos_y;
    f32 pos_z;
    f32 view_x;
    f32 view_y;
    f32 screen_x4;
    f32 screen_y4;
    f32 z_clip;
    f32 depth_factor;
} BillboardProjection;

typedef struct {
    s16 size_permille;
    s16 clip_expand_x4;
    s16 wrap_span_x4;
    f32 bias_x4;
    f32 bias_y4;
} BillboardFamilyPolicy;

typedef enum {
    BILLBOARD_CACHE_ROW_VIEW_X = 0,
    BILLBOARD_CACHE_ROW_VIEW_Y = 1,
    BILLBOARD_CACHE_ROW_Z_CLIP = 2,
    BILLBOARD_CACHE_ROW_SCREEN = 3,
} BillboardCacheRow;

typedef enum {
    BILLBOARD_CACHE_COEFF_X = 0,
    BILLBOARD_CACHE_COEFF_Y = 1,
    BILLBOARD_CACHE_COEFF_Z = 2,
    BILLBOARD_CACHE_COEFF_BIAS = 3,
} BillboardCacheCoeff;

enum {
    BILLBOARD_FAMILY_CACHE = 0,
    BILLBOARD_FAMILY_MASK = 6,
    BILLBOARD_FAMILY_COUNT = 7,
};

#define BILLBOARD_FAMILY_STAR RC_BILLBOARD_FAMILY_STAR
#define BILLBOARD_FAMILY_ENERGY RC_BILLBOARD_FAMILY_ENERGY
#define BILLBOARD_FAMILY_COLLECTIBLE RC_BILLBOARD_FAMILY_COLLECTIBLE
#define BILLBOARD_FAMILY_DUALSCALE RC_BILLBOARD_FAMILY_DUALSCALE
#define BILLBOARD_FAMILY_PARTICLE RC_BILLBOARD_FAMILY_PARTICLE

static const BillboardFamilyPolicy s_billboard_family_policies[BILLBOARD_FAMILY_COUNT] = {
    [BILLBOARD_FAMILY_CACHE] = { 1000, 0, 0, 0.0f, 0.0f },
    [BILLBOARD_FAMILY_STAR] = { 1000, 0, 0, 0.0f, 0.0f },
    [BILLBOARD_FAMILY_ENERGY] = { 1000, 0, 0, 0.0f, 0.0f },
    [BILLBOARD_FAMILY_COLLECTIBLE] = { 1000, 0, 0, 0.0f, 0.0f },
    [BILLBOARD_FAMILY_DUALSCALE] = { 1000, 0, 0, 0.0f, 0.0f },
    [BILLBOARD_FAMILY_PARTICLE] = { 1000, 0, 0, 0.0f, 0.0f },
    [BILLBOARD_FAMILY_MASK] = { 1000, 0, 0, 0.0f, 0.0f },
};

static u32 s_billboard_sample_frame[BILLBOARD_FAMILY_COUNT] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};
static u32 s_energy_diag_frame = 0xFFFFFFFFu;
static u32 s_energy_diag_count = 0;
static u32 s_energy_settimg_emit_frame = 0xFFFFFFFFu;
static u32 s_energy_rect_emit_frame = 0xFFFFFFFFu;
static Gfx* s_energy_diag_last_dl = NULL;
static u32 s_energy_diag_source_bits = ENERGY_SRC_NONE;
static u32 s_energy_diag_last_settimg_phys = 0;

static inline u32 bswap32_u32(u32 v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static inline s32 mul_permille_s32(s32 v, s32 permille) {
    return (v * permille) / 1000;
}

static inline f32 mul_permille_f32(f32 v, s32 permille) {
    return (v * (f32)permille) / 1000.0f;
}

static inline BillboardScreenSpace get_billboard_space(void) {
    BillboardScreenSpace out;
    out.center_x4 = SSSV_BASE_WIDTH * 2;
    out.center_y4 = SSSV_BASE_HEIGHT * 2;
    out.scale_x4  = SSSV_BASE_WIDTH * 2;
    out.scale_y4  = SSSV_BASE_HEIGHT * 2;
    out.left_x4   = 0;
    out.right_x4  = SSSV_BASE_WIDTH * 4;
    out.top_y4    = 0;
    out.bottom_y4 = SSSV_BASE_HEIGHT * 4;
    out.span_x4   = SSSV_BASE_WIDTH * 4;

    return out;
}

static inline const BillboardFamilyPolicy* get_billboard_family_policy(u32 family_id) {
    if (family_id >= BILLBOARD_FAMILY_COUNT) {
        return &s_billboard_family_policies[BILLBOARD_FAMILY_CACHE];
    }
    return &s_billboard_family_policies[family_id];
}

static inline s32 billboard_wrap_span_x4(const BillboardFamilyPolicy* policy, s32 base_span_x4) {
    if (policy->wrap_span_x4 > 0) {
        return policy->wrap_span_x4;
    }
    return (base_span_x4 > 0) ? base_span_x4 : 1;
}

static inline BillboardClipBounds get_billboard_clip_bounds(const BillboardScreenSpace* space, const BillboardFamilyPolicy* policy) {
    BillboardClipBounds out;

    out.left = (f32)(space->left_x4 - policy->clip_expand_x4);
    out.right = (f32)(space->right_x4 + policy->clip_expand_x4);
    out.top = (f32)space->top_y4;
    out.bottom = (f32)space->bottom_y4;
    return out;
}

static inline void billboard_cache_store_row(BillboardCacheRow row, f32 x, f32 y, f32 z, f32 bias) {
    gDisplayListContext->unk38A10[row][BILLBOARD_CACHE_COEFF_X] = x;
    gDisplayListContext->unk38A10[row][BILLBOARD_CACHE_COEFF_Y] = y;
    gDisplayListContext->unk38A10[row][BILLBOARD_CACHE_COEFF_Z] = z;
    gDisplayListContext->unk38A10[row][BILLBOARD_CACHE_COEFF_BIAS] = bias;
}

static inline f32 billboard_cache_value(BillboardCacheRow row, BillboardCacheCoeff coeff) {
    return gDisplayListContext->unk38A10[row][coeff];
}

// The original game stores four cached projection rows in unk38A10:
// view X, view Y, clip Z, and a screen/depth row.
static inline f32 billboard_cache_eval_row(BillboardCacheRow row, f32 pos_x, f32 pos_y, f32 pos_z) {
    return billboard_cache_value(row, BILLBOARD_CACHE_COEFF_BIAS) +
           ((billboard_cache_value(row, BILLBOARD_CACHE_COEFF_Z) * pos_z) +
            ((billboard_cache_value(row, BILLBOARD_CACHE_COEFF_Y) * pos_y) +
             (billboard_cache_value(row, BILLBOARD_CACHE_COEFF_X) * pos_x)));
}

static inline u32 energy_source_from_phys(u32 phys) {
    if ((phys >= ENERGY_RGBA_TEX_PHYS_BASE) && (phys < (ENERGY_RGBA_TEX_PHYS_BASE + ENERGY_TEX_PHYS_WINDOW))) {
        return ENERGY_SRC_RGBA;
    }
    if ((phys >= ENERGY_MASK_TEX_PHYS_BASE) && (phys < (ENERGY_MASK_TEX_PHYS_BASE + ENERGY_TEX_PHYS_WINDOW))) {
        return ENERGY_SRC_MASK;
    }
    return ENERGY_SRC_NONE;
}

static inline u32 energy_source_from_gfx_cmd(const Gfx* cmd, u32* out_phys) {
    u32 w0_candidates[2];
    u32 w1_candidates[2];
    int i, j;
    if (cmd == NULL) {
        return ENERGY_SRC_NONE;
    }

    w0_candidates[0] = cmd->words.w0;
    w0_candidates[1] = bswap32_u32(cmd->words.w0);
    w1_candidates[0] = cmd->words.w1;
    w1_candidates[1] = bswap32_u32(cmd->words.w1);

    for (i = 0; i < 2; i++) {
        u32 op = (w0_candidates[i] >> 24) & 0xFFu;
        if (op != G_SETTIMG) {
            continue;
        }

        for (j = 0; j < 2; j++) {
            u32 phys = w1_candidates[j] & 0x1FFFFFFFu;
            u32 src = energy_source_from_phys(phys);
            if (src != ENERGY_SRC_NONE) {
                if (out_phys != NULL) {
                    *out_phys = phys;
                }
                return src;
            }
        }

        if (out_phys != NULL) {
            *out_phys = w1_candidates[0] & 0x1FFFFFFFu;
        }
    }

    return ENERGY_SRC_NONE;
}

static inline void energy_diag_emit(u32 tag, u32 a, u32 b, u32 c) {
    u32* last_emit_frame = NULL;
    u32 frame = rc_frame_index();

    if ((sssv_patch_diag_flags & (SSSV_DIAG_BILLBOARD | SSSV_DIAG_WARN)) == 0u) {
        return;
    }
    if (tag == PATCH_TAG_ENERGY_SETTIMG_SEEN) {
        last_emit_frame = &s_energy_settimg_emit_frame;
    } else if (tag == PATCH_TAG_ENERGY_RECT_EMIT) {
        last_emit_frame = &s_energy_rect_emit_frame;
    }

    if ((last_emit_frame != NULL) && ((*last_emit_frame != 0xFFFFFFFFu) && ((frame - *last_emit_frame) < 20u))) {
        return;
    }
    if (s_energy_diag_count >= 2u) {
        return;
    }

    if (last_emit_frame != NULL) {
        *last_emit_frame = frame;
    }
    s_energy_diag_count++;
    PATCH_TRACE_DIAG(SSSV_DIAG_WARN, tag, a, b, c);
}

static inline void energy_diag_absorb_cmd(const Gfx* cmd) {
    u32 phys = 0;
    u32 src = energy_source_from_gfx_cmd(cmd, &phys);
    if (phys != 0u) {
        s_energy_diag_last_settimg_phys = phys;
    }
    if (src != ENERGY_SRC_NONE) {
        s_energy_diag_source_bits |= src;
    }
}

static inline void energy_diag_backtrack(Gfx* dl, s32 max_cmds) {
    Gfx* cmd = dl;
    s32 i = 0;
    const u32 wanted = ENERGY_SRC_RGBA | ENERGY_SRC_MASK;

    if (cmd == NULL) {
        return;
    }

    while ((i < max_cmds) && (cmd != NULL)) {
        cmd--;
        i++;
        energy_diag_absorb_cmd(cmd);
        if ((s_energy_diag_source_bits & wanted) == wanted) {
            break;
        }
    }
}

static inline void energy_diag_refresh(Gfx* dl) {
    u32 frame = rc_frame_index();
    if ((sssv_patch_diag_flags & (SSSV_DIAG_BILLBOARD | SSSV_DIAG_WARN)) == 0u) {
        return;
    }

    if (frame != s_energy_diag_frame) {
        s_energy_diag_frame = frame;
        s_energy_diag_count = 0;
        s_energy_diag_last_dl = NULL;
        s_energy_diag_source_bits = ENERGY_SRC_NONE;
        s_energy_diag_last_settimg_phys = 0;
    }

    if (dl != NULL) {
        if ((s_energy_diag_last_dl != NULL) && (dl >= s_energy_diag_last_dl) && (dl <= (s_energy_diag_last_dl + ENERGY_DIAG_DL_WINDOW_CMDS))) {
            while (s_energy_diag_last_dl < dl) {
                energy_diag_absorb_cmd(s_energy_diag_last_dl);
                s_energy_diag_last_dl++;
            }
        }
        s_energy_diag_last_dl = dl;
    }

    if (s_energy_diag_source_bits == ENERGY_SRC_NONE) {
        energy_diag_backtrack(dl, ENERGY_DIAG_BACKTRACK_CMDS_CURRENT);
    }
    if (s_energy_diag_source_bits == ENERGY_SRC_NONE) {
        energy_diag_backtrack(gLayer0DL, ENERGY_DIAG_BACKTRACK_CMDS_LAYER);
    }
    if (s_energy_diag_source_bits == ENERGY_SRC_NONE) {
        energy_diag_backtrack(gAuxDL, ENERGY_DIAG_BACKTRACK_CMDS_LAYER);
    }
}

static inline s32 billboard_scale_size_by_fovy(s32 value) {
    s32 fovy = gLevelConfig.fovY;
    if (fovy == 0) {
        return value;
    }
    return (s32)(((f32)(value * 33)) / (f32)fovy);
}

static inline f32 billboard_project_z_clip(f32 pos_x, f32 pos_y, f32 pos_z) {
    return billboard_cache_eval_row(BILLBOARD_CACHE_ROW_Z_CLIP, pos_x, pos_y, pos_z);
}

static inline f32 billboard_project_depth_factor(f32 z_clip) {
    return (billboard_cache_value(BILLBOARD_CACHE_ROW_SCREEN, BILLBOARD_CACHE_COEFF_BIAS) +
            (billboard_cache_value(BILLBOARD_CACHE_ROW_SCREEN, BILLBOARD_CACHE_COEFF_Z) * z_clip)) / -z_clip;
}

static inline f32 billboard_project_view_x(f32 pos_x, f32 pos_y, f32 pos_z) {
    return billboard_cache_eval_row(BILLBOARD_CACHE_ROW_VIEW_X, pos_x, pos_y, pos_z);
}

static inline f32 billboard_project_view_y(f32 pos_x, f32 pos_y, f32 pos_z) {
    return billboard_cache_eval_row(BILLBOARD_CACHE_ROW_VIEW_Y, pos_x, pos_y, pos_z);
}

static inline f32 billboard_project_screen_x4(const BillboardScreenSpace* space, f32 view_x, f32 z_clip) {
    return ((billboard_cache_value(BILLBOARD_CACHE_ROW_SCREEN, BILLBOARD_CACHE_COEFF_X) * view_x) / z_clip) + (f32)space->center_x4;
}

static inline f32 billboard_project_screen_y4(const BillboardScreenSpace* space, f32 view_y, f32 z_clip) {
    return ((billboard_cache_value(BILLBOARD_CACHE_ROW_SCREEN, BILLBOARD_CACHE_COEFF_Y) * view_y) / z_clip) + (f32)space->center_y4;
}

static inline u16 billboard_compute_prim_depth(f32 depth_factor) {
    return (u16)((depth_factor * 1023.0f * 32.0f) + 32736.0f);
}

static inline u8 billboard_compute_fog_alpha(f32 depth_factor) {
    f32 fog_depth;
    s16 fog_min_scaled;
    s16 fog_max_scaled;

    if (gFogState.min >= (gFogState.max - 1)) {
        return 0;
    }

    fog_min_scaled = gFogState.min << 3;
    fog_max_scaled = gFogState.max << 3;
    fog_depth = f32_min(depth_factor * 7990.0f, 8000.0f);

    if (fog_min_scaled >= (s16)fog_depth) {
        return 0;
    }
    if ((s16)fog_depth >= fog_max_scaled) {
        return 0xFF;
    }
    return (u8)(((fog_depth - fog_min_scaled) * 255.0f) / (fog_max_scaled - fog_min_scaled));
}

static inline s32 billboard_compute_visibility_metric(s32 world_x, s32 world_z, s32 world_y, s8 lod_shift) {
    s32 fovy = (s32)gLevelConfig.fovY;
    s32 delta_x = (world_x >> 16) - (s16)gCameraEyeWorldX;
    s32 delta_z = (world_z >> 16) - (s16)gCameraEyeWorldZ;
    s32 delta_y = (world_y >> 16) - (s16)gCameraEyeWorldY;
    s32 visibility_metric = (SQ(delta_x) + SQ(delta_z) + SQ(delta_y)) >> lod_shift;

    return (visibility_metric * fovy) / 75;
}

static inline void append_tagged_worldmask_texrect(Gfx** dl, s16 xl, s16 yl, s16 xh, s16 yh, s16 s, s16 t, s16 dsdx, s16 dtdy);

static inline void append_fov_mask_billboard(
    u8 fov_mask_index,
    s16 mask_red,
    s16 mask_green,
    s16 mask_blue,
    f32 depth_factor,
    f32 mask_left_x4,
    f32 mask_top_y4,
    f32 mask_right_x4,
    f32 mask_bottom_y4
) {
    s16 dsdx_dtdy;
    u8 fog_alpha;

    if (fov_mask_index == 100) {
        return;
    }

    gDPSetTextureImage(gAuxDL++, G_IM_FMT_I, G_IM_SIZ_16b, 1, &img_fov_masks_ci4__png[(fov_mask_index << 7)]);
    gDPSetTile(gAuxDL++, G_IM_FMT_I, G_IM_SIZ_16b, 0, 0, G_TX_LOADTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD);
    gDPLoadSync(gAuxDL++);
    gDPLoadBlock(gAuxDL++, G_TX_LOADTILE, 0, 0, 63, 2048);
    gDPPipeSync(gAuxDL++);
    gDPSetTile(gAuxDL++, G_IM_FMT_I, G_IM_SIZ_4b, 1, 0, G_TX_RENDERTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMASK, G_TX_NOLOD);
    gDPSetTileSize(gAuxDL++, G_TX_RENDERTILE, 0, 0, 4 * 15, 4 * 15);
    gDPSetEnvColor(gAuxDL++, mask_red, mask_green, mask_blue, 0xFF);
    gSPDisplayList(gAuxDL++, gFovMaskRenderSetupDl);

    gDPSetPrimDepth(gAuxDL++, (u16)(billboard_compute_prim_depth(depth_factor) - gLevelConfig.unk42), 0);
    fog_alpha = billboard_compute_fog_alpha(depth_factor);
    gDPSetPrimColor(gAuxDL++, 0, fog_alpha, gFogState.r, gFogState.g, gFogState.b, 0xFF);

    if (mask_left_x4 < mask_right_x4) {
        dsdx_dtdy = (s16)(65536.0f / (mask_right_x4 - mask_left_x4));
        // Preserve the original axis-swapped mask rectangle argument order.
        append_tagged_worldmask_texrect(
            &gAuxDL,
            (s16)mask_left_x4,
            (s16)mask_top_y4,
            (s16)mask_right_x4,
            (s16)mask_bottom_y4,
            (s16)((dsdx_dtdy * ((u16)mask_left_x4 & 3)) >> 9),
            (s16)(-(dsdx_dtdy * ((u16)mask_top_y4 & 3)) >> 7),
            dsdx_dtdy,
            dsdx_dtdy
        );
        gSPDisplayList(gAuxDL++, gRestoreRgba16TextureLutDl);
    }
}

static inline s16 classify_fov_mask_ground_fallback(
    const BillboardProjection* projection,
    const BillboardScreenSpace* space,
    const BillboardClipBounds* clip,
    s32 world_x,
    s32 world_z,
    f32 mask_half_size_x4
) {
    f32 ground_height_y = sample_ground_height_at_xz(world_x >> 16, world_z >> 16) / 65536.0f;
    f32 ground_z_clip = billboard_project_z_clip(projection->pos_x, projection->pos_y, ground_height_y);
    f32 ground_screen_x4;
    f32 ground_screen_y4;
    f32 mask_left_x4 = projection->screen_x4 - mask_half_size_x4;
    f32 mask_top_y4 = projection->screen_y4 - mask_half_size_x4;

    if (ground_z_clip > -3.0f) {
        return VISIBILITY_OUT_OF_BOUNDS_X;
    }

    ground_screen_x4 = billboard_project_screen_x4(space, projection->view_x, ground_z_clip);
    ground_screen_y4 = billboard_project_screen_y4(space, projection->view_y, ground_z_clip);

    if (((projection->screen_x4 + mask_half_size_x4) < 0.0f) && ((ground_screen_x4 + mask_half_size_x4) < 0.0f)) {
        return VISIBILITY_OUT_OF_BOUNDS_Y;
    }
    if ((clip->right < mask_left_x4) && (clip->right < (ground_screen_x4 - mask_half_size_x4))) {
        return VISIBILITY_OUT_OF_BOUNDS_Y;
    }
    if ((clip->bottom < mask_top_y4) && (clip->bottom < (ground_screen_y4 - mask_half_size_x4))) {
        return VISIBILITY_OUT_OF_BOUNDS_Y;
    }
    return VISIBILITY_OUT_OF_BOUNDS_X;
}

static inline s32 billboard_rect_intersects_clip(const BillboardClipBounds* clip, f32 xl, f32 yl, f32 xh, f32 yh) {
    return (xl < clip->right) && (yl < clip->bottom) && (xh > clip->left) && (yh > clip->top);
}

static inline void emit_billboard_sample_once(
    u32 family,
    s32 world_x,
    s32 world_y,
    s32 world_z,
    f32 screen_x4,
    f32 screen_y4
) {
    u32 frame;
    u32 packed_screen;
    u32 packed_world;

    if ((family >= BILLBOARD_FAMILY_COUNT) || ((sssv_patch_diag_flags & SSSV_DIAG_BILLBOARD) == 0u)) {
        return;
    }

    frame = rc_frame_index();
    if (s_billboard_sample_frame[family] == frame) {
        return;
    }
    s_billboard_sample_frame[family] = frame;

    packed_screen = ((u32)(u16)((s32)screen_x4) << 16) | (u16)((s32)screen_y4);
    packed_world = ((u32)(u16)(world_x >> 16) << 16) | (u16)(world_y >> 16);

    PATCH_TRACE_DIAG(
        SSSV_DIAG_BILLBOARD,
        PATCH_TAG_BILLBOARD_SAMPLE,
        (frame << 8) | family,
        packed_world,
        ((u32)(u16)(world_z >> 16) << 16) | ((u32)(u16)cur_perspective_projection_transform_id)
    );
    PATCH_TRACE_DIAG(
        SSSV_DIAG_BILLBOARD,
        PATCH_TAG_BILLBOARD_SAMPLE,
        ((frame << 8) | family) ^ 0x80000000u,
        packed_screen,
        ((u32)(u16)gScreenWidth << 16) | (u16)gScreenHeight
    );
}

static inline s32 project_billboard_to_screen(
    u32 family_id,
    const BillboardFamilyPolicy* policy,
    s32 world_x,
    s32 world_y,
    s32 world_z,
    const BillboardScreenSpace* space,
    BillboardProjection* out
) {
    out->pos_x = world_x / 65536.0f;
    out->pos_y = world_y / 65536.0f;
    out->pos_z = world_z / 65536.0f;

    out->z_clip = billboard_project_z_clip(out->pos_x, out->pos_y, out->pos_z);
    if (out->z_clip > -3.0f) {
        return 0;
    }

    out->view_x = billboard_project_view_x(out->pos_x, out->pos_y, out->pos_z);
    out->view_y = billboard_project_view_y(out->pos_x, out->pos_y, out->pos_z);
    out->screen_x4 = billboard_project_screen_x4(space, out->view_x, out->z_clip);
    out->screen_y4 = billboard_project_screen_y4(space, out->view_y, out->z_clip);
    out->depth_factor = billboard_project_depth_factor(out->z_clip);

    out->screen_x4 += policy->bias_x4;
    out->screen_y4 += policy->bias_y4;
    emit_billboard_sample_once(family_id, world_x, world_y, world_z, out->screen_x4, out->screen_y4);
    return 1;
}

static inline void append_tagged_billboard_texrect(
    u32 family_id,
    s32 world_x,
    s32 world_y,
    s32 world_z,
    s32 sig0,
    s32 sig1,
    s32 sig2,
    Gfx** dl,
    s16 xl,
    s16 yl,
    s16 xh,
    s16 yh,
    s16 s,
    s16 t,
    s16 dsdx,
    s16 dtdy
) {
    (void)family_id;
    (void)world_x;
    (void)world_y;
    (void)world_z;
    (void)sig0;
    (void)sig1;
    (void)sig2;
    gSPScisTextureRectangle(*dl, xl, yl, xh, yh, G_TX_RENDERTILE, s, t, dsdx, dtdy);
    *dl += 3;
}

static inline void append_tagged_worldmask_texrect(Gfx** dl, s16 xl, s16 yl, s16 xh, s16 yh, s16 s, s16 t, s16 dsdx, s16 dtdy) {
    gSPScisTextureRectangle(*dl, xl, yl, xh, yh, G_TX_RENDERTILE, s, t, dsdx, dtdy);
    *dl += 3;
}

RECOMP_PATCH void update_billboard_projection_cache(void) {
    Mtx* tmp = &gDisplayListContext->unk37490;
    BillboardScreenSpace space = get_billboard_space();
    f32 view_x_mul_x;
    f32 view_x_mul_y;
    f32 view_x_mul_z;
    f32 view_x_bias;
    f32 view_y_mul_x;
    f32 view_y_mul_y;
    f32 view_y_mul_z;
    f32 view_y_bias;
    f32 clip_mul_x;
    f32 clip_mul_y;
    f32 clip_mul_z;
    f32 clip_bias;
    f32 screen_scale_x;
    f32 screen_scale_y;
    f32 depth_mul;
    f32 depth_bias;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x73ED30u);

    if (rc_world_projection_active()) {
        rc_update_camera_cut_skip();
    }

    view_x_mul_x = (f32)(s32)MTX_INTPART_PACK(tmp->m[0][0], tmp->m[2][0]) / 65536.0f;
    view_y_mul_x = (f32)(s32)MTX_FRACPART_PACK(tmp->m[0][0], tmp->m[2][0]) / 65536.0f;
    view_x_mul_y = (f32)(s32)MTX_INTPART_PACK(tmp->m[0][2], tmp->m[2][2]) / 65536.0f;
    view_y_mul_y = (f32)(s32)MTX_FRACPART_PACK(tmp->m[0][2], tmp->m[2][2]) / 65536.0f;
    view_x_mul_z = (f32)(s32)MTX_INTPART_PACK(tmp->m[1][0], tmp->m[3][0]) / 65536.0f;
    view_y_mul_z = (f32)(s32)MTX_FRACPART_PACK(tmp->m[1][0], tmp->m[3][0]) / 65536.0f;
    view_x_bias = (f32)(s32)MTX_INTPART_PACK(tmp->m[1][2], tmp->m[3][2]) / 65536.0f;
    view_y_bias = (f32)(s32)MTX_FRACPART_PACK(tmp->m[1][2], tmp->m[3][2]) / 65536.0f;
    clip_mul_x = (f32)(s32)MTX_INTPART_PACK(tmp->m[0][1], tmp->m[2][1]) / 65536.0f;
    clip_mul_y = (f32)(s32)MTX_INTPART_PACK(tmp->m[0][3], tmp->m[2][3]) / 65536.0f;
    clip_mul_z = (f32)(s32)MTX_INTPART_PACK(tmp->m[1][1], tmp->m[3][1]) / 65536.0f;
    clip_bias = (f32)(s32)MTX_INTPART_PACK(tmp->m[1][3], tmp->m[3][3]) / 65536.0f;

    tmp = &gDisplayListContext->unk37410;
    screen_scale_x = (f32)(s32)MTX_INTPART_PACK(tmp->m[0][0], tmp->m[2][0]) / 65536.0f;
    screen_scale_y = (f32)(s32)MTX_FRACPART_PACK(tmp->m[0][2], tmp->m[2][2]) / 65536.0f;
    depth_mul = (f32)(s32)MTX_INTPART_PACK(tmp->m[1][1], tmp->m[3][1]) / 65536.0f;
    depth_bias = (f32)(s32)MTX_INTPART_PACK(tmp->m[1][3], tmp->m[3][3]) / 65536.0f;

    screen_scale_x *= -(f32)space.scale_x4;
    screen_scale_y *= (f32)space.scale_y4;

    billboard_cache_store_row(BILLBOARD_CACHE_ROW_VIEW_X, view_x_mul_x, view_x_mul_y, view_x_mul_z, view_x_bias);
    billboard_cache_store_row(BILLBOARD_CACHE_ROW_VIEW_Y, view_y_mul_x, view_y_mul_y, view_y_mul_z, view_y_bias);
    billboard_cache_store_row(BILLBOARD_CACHE_ROW_Z_CLIP, clip_mul_x, clip_mul_y, clip_mul_z, clip_bias);
    billboard_cache_store_row(BILLBOARD_CACHE_ROW_SCREEN, screen_scale_x, screen_scale_y, depth_mul, depth_bias);

    emit_billboard_sample_once(
        BILLBOARD_FAMILY_CACHE,
        ((s32)gCameraEyeWorldX) << 16,
        ((s32)gCameraEyeWorldZ) << 16,
        ((s32)gCameraEyeWorldY) << 16,
        0.0f,
        0.0f
    );
}

RECOMP_PATCH void draw_star_billboard_texrect(Gfx **dl, s32 world_x, s32 world_z, s32 world_y, s16 tex_half_width, s16 tex_half_height, s32 sprite_scale) {
    BillboardProjection projection;
    f32 size_x4;
    f32 xl;
    f32 yl;
    f32 xh;
    f32 yh;
    s16 dsdx;
    s16 dtdy;
    BillboardScreenSpace space;
    BillboardClipBounds clip;
    const BillboardFamilyPolicy* policy;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x6C5E44u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_STAR);
    clip = get_billboard_clip_bounds(&space, policy);
    if (project_billboard_to_screen(BILLBOARD_FAMILY_STAR, policy, world_x, world_z, world_y, &space, &projection)) {
        if (projection.depth_factor > 0.0f) {
            sprite_scale = billboard_scale_size_by_fovy(sprite_scale);
            size_x4 = (sprite_scale * 32) / -projection.z_clip;
            size_x4 = mul_permille_f32(size_x4, policy->size_permille);

            if (size_x4 > 15.0f) {
                size_x4 = 15.0f;
            }
            if (size_x4 < 4.0f) {
                size_x4 = 4.0f;
            }

            if (size_x4 > 0.0f) {
                xl = projection.screen_x4 - ((tex_half_width * size_x4) / 128.0f);
                xh = projection.screen_x4 + ((tex_half_width * size_x4) / 128.0f);
                yl = projection.screen_y4 - ((tex_half_height * size_x4) / 128.0f);
                yh = projection.screen_y4 + 2.0f;

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_height << 12) / (xh - xl));
                    dtdy = (s16)((tex_half_width << 12) / (yh - yl));

                    if (billboard_rect_intersects_clip(&clip, xl, yl, xh, yh)) {
                        append_tagged_billboard_texrect(
                            BILLBOARD_FAMILY_STAR,
                            world_x,
                            world_z,
                            world_y,
                            tex_half_width,
                            tex_half_height,
                            sprite_scale,
                            dl,
                            (s16)xl,
                            (s16)yl,
                            (s16)xh,
                            (s16)yh,
                            (s16)((dsdx * ((s16)xl & 3)) >> 9),
                            (s16)(-(dtdy * ((s16)yl & 3)) >> 7),
                            dsdx,
                            dtdy
                        );
                    }
                }
            }
        }
    }
}

RECOMP_PATCH void draw_energy_billboard_texrect(Gfx **dl, s32 world_x, s32 world_z, s32 world_y, s16 tex_half_width, s16 tex_half_height, s32 sprite_scale) {
    BillboardProjection projection;
    f32 xl;
    f32 yl;
    f32 xh;
    f32 yh;
    f32 yOffset;
    f32 xOffset;
    f32 size_x4;
    s16 dsdx;
    s16 dtdy;
    BillboardScreenSpace space;
    BillboardClipBounds clip;
    const BillboardFamilyPolicy* policy;
    s32 diag_reason = 0;
    s16 diag_screen_x = 0;
    s16 diag_screen_y = 0;
    s16 diag_xl = 0;
    s16 diag_yl = 0;
    s16 diag_xh = 0;
    s16 diag_yh = 0;
    u32 diag_source_bits = ENERGY_SRC_NONE;
    u32 diag_source_phys = 0;
    dsdx = 0;
    dtdy = 0;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x73F17Cu);
    energy_diag_refresh((dl != NULL) ? *dl : NULL);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_ENERGY);
    clip = get_billboard_clip_bounds(&space, policy);
    diag_source_bits = s_energy_diag_source_bits;
    diag_source_phys = s_energy_diag_last_settimg_phys;

    if (project_billboard_to_screen(BILLBOARD_FAMILY_ENERGY, policy, world_x, world_z, world_y, &space, &projection)) {
        if (projection.depth_factor > 0.0f) {
            diag_screen_x = (s16)projection.screen_x4;
            diag_screen_y = (s16)projection.screen_y4;
            sprite_scale = billboard_scale_size_by_fovy(sprite_scale);
            size_x4 = (sprite_scale * 32) / -projection.z_clip;
            size_x4 = mul_permille_f32(size_x4, policy->size_permille);
            if (size_x4 > 16383.0f) {
                size_x4 = 16383.0f;
            }

            if (size_x4 > 0.0f) {
                gDPSetPrimDepth((*dl)++, (billboard_compute_prim_depth(projection.depth_factor) - gLevelConfig.unk42), 0);

                xOffset = (tex_half_width * size_x4) / 128.0f;
                yOffset = (tex_half_height * size_x4) / 128.0f;

                xl = projection.screen_x4 - xOffset;
                yl = projection.screen_y4 - yOffset;

                xh = projection.screen_x4 + xOffset;
                yh = projection.screen_y4 + yOffset;
                diag_xl = (s16)xl;
                diag_yl = (s16)yl;
                diag_xh = (s16)xh;
                diag_yh = (s16)yh;

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_width << 12) / (yh - yl));
                    dtdy = (s16)((tex_half_height << 12) / (xh - xl));

                    if (billboard_rect_intersects_clip(&clip, xl, yl, xh, yh)) {
                        diag_reason = 6;
                        append_tagged_billboard_texrect(
                            BILLBOARD_FAMILY_ENERGY,
                            world_x,
                            world_z,
                            world_y,
                            tex_half_width,
                            tex_half_height,
                            sprite_scale,
                            dl,
                            (s16)xl,
                            (s16)yl,
                            (s16)xh,
                            (s16)yh,
                            0,
                            (s16)(-((dsdx * ((s16)yl & 3)) >> 7)),
                            dsdx,
                            dtdy
                        );
                    } else {
                        diag_reason = 5;
                    }
                } else {
                    diag_reason = 4;
                }
            } else {
                diag_reason = 3;
            }
        } else {
            diag_reason = 2;
        }
    } else {
        diag_reason = 1;
    }

    if (diag_reason != 6) {
        energy_diag_emit(
            PATCH_TAG_ENERGY_SETTIMG_SEEN,
            diag_source_bits,
            diag_source_phys,
            ((u32)(u16)sprite_scale << 16) | (u16)diag_reason
        );
        energy_diag_emit(
            PATCH_TAG_ENERGY_RECT_EMIT,
            ((u32)(u16)diag_screen_x << 16) | (u16)diag_screen_y,
            ((u32)(u16)diag_xl << 16) | (u16)diag_yl,
            ((u32)(u16)diag_xh << 16) | (u16)diag_yh
        );
    }
}

RECOMP_PATCH void draw_collectible_billboard_texrect(Gfx **dl, s32 world_x, s32 world_z, s32 world_y, s16 tex_half_width, s16 tex_half_height, s32 sprite_scale) {
    BillboardProjection projection;
    f32 size_x4;
    f32 xl;
    f32 xh;
    s16 dsdx;
    s16 dtdy;
    f32 yh;
    f32 yl;
    f32 xOffset;
    f32 yOffset;
    u8 fog_alpha;
    s32 has_yoffset;
    BillboardScreenSpace space;
    BillboardClipBounds clip;
    const BillboardFamilyPolicy* policy;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x73F800u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_COLLECTIBLE);
    clip = get_billboard_clip_bounds(&space, policy);

    has_yoffset = 0;
    if (project_billboard_to_screen(BILLBOARD_FAMILY_COLLECTIBLE, policy, world_x, world_z, world_y, &space, &projection)) {
        if (projection.depth_factor > 0.0f) {
            sprite_scale = billboard_scale_size_by_fovy(sprite_scale);
            size_x4 = (sprite_scale * 32) / -projection.z_clip;
            size_x4 = mul_permille_f32(size_x4, policy->size_permille);

            if (size_x4 > 16383.0f) {
                size_x4 = 16383.0f;
            }

            if (size_x4 > 0.0f) {
                fog_alpha = billboard_compute_fog_alpha(projection.depth_factor);
                gDPSetPrimColor((*dl)++, 0, fog_alpha, gFogState.r, gFogState.g, gFogState.b, 0xFF);
                gDPSetPrimDepth((*dl)++, (billboard_compute_prim_depth(projection.depth_factor) - gLevelConfig.unk42), 0);

                if (tex_half_height > 32) {
                    tex_half_height -= 32;
                    has_yoffset = 1;
                }

                xOffset = ((tex_half_width * size_x4) / 128.0f);
                yOffset = (tex_half_height * size_x4) / 128.0f;
                xl = projection.screen_x4 - xOffset;

                if (has_yoffset != 0) {
                    yl = projection.screen_y4 - (yOffset * 3.0f);
                } else {
                    yl = projection.screen_y4 - yOffset;
                }

                xh = projection.screen_x4 + xOffset;
                yh = projection.screen_y4 + yOffset;

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_height << 12) / (xh - xl));
                    dtdy = (s16)((tex_half_width << 12) / (yh - yl));

                    if (billboard_rect_intersects_clip(&clip, xl, yl, xh, yh)) {
                        append_tagged_billboard_texrect(
                            BILLBOARD_FAMILY_COLLECTIBLE,
                            world_x,
                            world_z,
                            world_y,
                            tex_half_width,
                            tex_half_height,
                            sprite_scale,
                            dl,
                            (s16)xl,
                            (s16)yl,
                            (s16)xh,
                            (s16)yh,
                            (s16)((dsdx * ((s16)xl & 3)) >> 9),
                            0,
                            dsdx,
                            dtdy
                        );
                    }
                }
            }
        }
    }
}

RECOMP_PATCH void draw_dualscale_billboard_texrect(Gfx **dl, s32 world_x, s32 world_z, s32 world_y, s16 tex_half_width, s16 tex_half_height, s32 sprite_scale_x, s32 sprite_scale_y) {
    BillboardProjection projection;
    s32 fog_threshold;
    s32 size_y4;
    s32 size_x4;
    s16 dtdy;
    s16 dsdx;
    s16 depth_scaled;
    u8 limit;
    BillboardScreenSpace space;
    BillboardClipBounds clip;
    const BillboardFamilyPolicy* policy;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x740094u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_DUALSCALE);
    clip = get_billboard_clip_bounds(&space, policy);
    if (project_billboard_to_screen(BILLBOARD_FAMILY_DUALSCALE, policy, world_x, world_z, world_y, &space, &projection)) {
        if (projection.depth_factor > 0.0f) {
            sprite_scale_x = billboard_scale_size_by_fovy(sprite_scale_x);
            sprite_scale_y = billboard_scale_size_by_fovy(sprite_scale_y);
            size_x4 = (sprite_scale_x * 32) / -projection.z_clip;
            size_y4 = (sprite_scale_y * 32) / -projection.z_clip;
            size_x4 = mul_permille_s32(size_x4, policy->size_permille);
            size_y4 = mul_permille_s32(size_y4, policy->size_permille);

            if (size_x4 > 16383) {
                size_x4 = 16383;
            }
            if (size_y4 > 16383) {
                size_y4 = 16383;
            }

            if ((size_x4 > 0) && (size_y4 > 0)) {
                f32 xl;
                f32 yl;
                f32 xh;
                f32 yh;
                f32 xOffset;
                f32 yOffset;

                fog_threshold = (1000 - ((1000 - gFogState.min) / 6));
                if (fog_threshold >= gFogState.max) {
                    limit = 0;
                } else {
                    depth_scaled = ((s32)projection.depth_factor * 1000) >> 16;
                    if (fog_threshold >= depth_scaled) {
                        limit = 0;
                    } else {
                        limit = (u8)((((f32)(depth_scaled - fog_threshold)) * 256.0f) / (f32)(gFogState.max - fog_threshold));
                    }
                }

                gDPSetPrimColor((*dl)++, 0, limit, gFogState.r, gFogState.g, gFogState.b, 0xFF);
                gDPSetPrimDepth((*dl)++, (billboard_compute_prim_depth(projection.depth_factor) - gLevelConfig.unk42), 0);

                xOffset = (tex_half_width * size_x4) / 128.0f;
                yOffset = (tex_half_height * size_y4) / 128.0f;

                xl = projection.screen_x4 - xOffset;
                yl = projection.screen_y4 - yOffset;
                xh = projection.screen_x4 + xOffset;
                yh = projection.screen_y4 + yOffset;

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_height << 12) / (xh - xl));
                    dtdy = (s16)((tex_half_width << 12) / (yh - yl));

                    if (billboard_rect_intersects_clip(&clip, xl, yl, xh, yh)) {
                        append_tagged_billboard_texrect(
                            BILLBOARD_FAMILY_DUALSCALE,
                            world_x,
                            world_z,
                            world_y,
                            tex_half_width,
                            tex_half_height,
                            (s32)(((u32)(u16)sprite_scale_x << 16) | (u16)sprite_scale_y),
                            dl,
                            (s16)xl,
                            (s16)yl,
                            (s16)xh,
                            (s16)yh,
                            (s16)((dsdx * ((s16)xl & 3)) >> 9),
                            (s16)(-(dtdy * ((s16)yl & 3)) >> 7),
                            dsdx,
                            dtdy
                        );
                    }
                }
            }
        }
    }
}

RECOMP_PATCH void draw_particle_billboard_texrect(Gfx **dl, s32 world_x, s32 world_z, s32 world_y, s16 tex_half_width, s16 tex_half_height, s32 sprite_scale_x, s32 sprite_scale_y, u8 wrap_x, s16 offset_limit) {
    BillboardProjection projection;
    f32 yOffset;
    f32 xOffset;
    f32 xl;
    f32 yl;
    f32 xh;
    f32 yh;
    s16 dsdx;
    s16 dtdy;
    s32 size_x4;
    s32 size_y4;
    BillboardScreenSpace space;
    BillboardClipBounds clip;
    const BillboardFamilyPolicy* policy;
    s32 wrap_span_x4;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x740820u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_PARTICLE);
    clip = get_billboard_clip_bounds(&space, policy);
    wrap_span_x4 = billboard_wrap_span_x4(policy, space.span_x4);
    if (project_billboard_to_screen(BILLBOARD_FAMILY_PARTICLE, policy, world_x, world_z, world_y, &space, &projection)) {
        if (projection.depth_factor > 0.0f) {
            sprite_scale_x = billboard_scale_size_by_fovy(sprite_scale_x);
            sprite_scale_y = billboard_scale_size_by_fovy(sprite_scale_y);
            size_x4 = (sprite_scale_x * 32) / -projection.z_clip;
            size_y4 = (sprite_scale_y * 32) / -projection.z_clip;
            size_x4 = mul_permille_s32(size_x4, policy->size_permille);
            size_y4 = mul_permille_s32(size_y4, policy->size_permille);

            if (size_x4 > 16383) {
                size_x4 = 16383;
            }
            if (size_y4 > 16383) {
                size_y4 = 16383;
            }

            if ((size_x4 > 0) && (size_y4 > 0)) {
                gDPSetPrimDepth((*dl)++, (u16)(billboard_compute_prim_depth(projection.depth_factor) - gLevelConfig.unk42), 0);

                xOffset = (tex_half_width * size_x4) / 128.0f;
                yOffset = (tex_half_height * size_y4) / 128.0f;

                if (offset_limit > 0) {
                    if ((offset_limit * 2) < xOffset) {
                        xOffset = (offset_limit * 2);
                    }
                    if ((offset_limit * 2) < yOffset) {
                        yOffset = (offset_limit * 2);
                    }
                }
                if (wrap_x != 0) {
                    while ((f32)space.right_x4 < projection.screen_x4) {
                        projection.screen_x4 -= (f32)wrap_span_x4;
                    }
                    while (projection.screen_x4 < (f32)space.left_x4) {
                        projection.screen_x4 += (f32)wrap_span_x4;
                    }
                }

                xl = projection.screen_x4 - xOffset;
                yl = projection.screen_y4 - yOffset;
                xh = projection.screen_x4 + xOffset;
                yh = projection.screen_y4 + yOffset;

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_height << 12) / (xh - xl));
                    dtdy = (s16)((tex_half_width << 12) / (yh - yl));

                    if (billboard_rect_intersects_clip(&clip, xl, yl, xh, yh)) {
                        append_tagged_billboard_texrect(
                            BILLBOARD_FAMILY_PARTICLE,
                            world_x,
                            world_z,
                            world_y,
                            tex_half_width,
                            tex_half_height,
                            (s32)(((u32)(u16)sprite_scale_x << 16) | (u16)sprite_scale_y),
                            dl,
                            (s16)xl,
                            (s16)yl,
                            (s16)xh,
                            (s16)yh,
                            (s16)((dsdx * ((s16)xl & 3)) >> 9),
                            (s16)(-(dtdy * ((s16)yl & 3)) >> 7),
                            dsdx,
                            dtdy
                        );
                    }
                }
            }
        }
    }
}

RECOMP_PATCH s16 classify_visibility_and_draw_fov_mask(
    s32 world_x,
    s32 world_z,
    s32 world_y,
    s32 mask_scale,
    u8 fov_mask_index,
    s16 mask_red,
    s16 mask_green,
    s16 mask_blue,
    s8 lod_shift,
    u8 force_mask_draw
) {
    BillboardProjection projection;
    s32 visibility_metric;
    f32 mask_half_size_x4;
    f32 mask_left_x4;
    f32 mask_top_y4;
    f32 mask_right_x4;
    f32 mask_bottom_y4;
    BillboardScreenSpace space;
    BillboardClipBounds clip;
    const BillboardFamilyPolicy* policy;

    rc_note_texrect_context(TRC_WORLD_MASK, 0x6FA3A4u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_MASK);
    clip = get_billboard_clip_bounds(&space, policy);

    if (is_world_cell_loaded_6AB9E4(world_x >> 16, world_z >> 16, world_y >> 16) == 0) {
        return VISIBILITY_INVISIBLE;
    }

    visibility_metric = billboard_compute_visibility_metric(world_x, world_z, world_y, lod_shift);
    if (visibility_metric > 0x4C9000) {
        return VISIBILITY_INVISIBLE;
    }
    if ((visibility_metric <= 0x1000) && (force_mask_draw == 0)) {
        gLodDetailState = 0;
        return VISIBILITY_VISIBLE;
    }

    if (project_billboard_to_screen(BILLBOARD_FAMILY_MASK, policy, world_x, world_z, world_y, &space, &projection)) {
        mask_scale = billboard_scale_size_by_fovy(mask_scale);
        mask_half_size_x4 = (mask_scale * 128 / -projection.z_clip) / 8.0f;
        mask_half_size_x4 = mul_permille_f32(mask_half_size_x4, policy->size_permille);

        mask_left_x4 = projection.screen_x4 - mask_half_size_x4;
        mask_top_y4 = projection.screen_y4 - mask_half_size_x4;
        mask_right_x4 = projection.screen_x4 + mask_half_size_x4;
        mask_bottom_y4 = projection.screen_y4 + mask_half_size_x4;

        if (mask_top_y4 > clip.bottom) {
            return VISIBILITY_OUT_OF_BOUNDS_Y;
        }

        if ((mask_left_x4 < clip.right) && (mask_bottom_y4 > clip.top) && (mask_right_x4 > clip.left)) {
            if ((visibility_metric < 0xE1000) && (force_mask_draw == 0)) {
                gLodDetailState = (visibility_metric < 0x31000) ? 0 : 1;
                return VISIBILITY_VISIBLE;
            }

            append_fov_mask_billboard(
                fov_mask_index,
                mask_red,
                mask_green,
                mask_blue,
                projection.depth_factor,
                mask_left_x4,
                mask_top_y4,
                mask_right_x4,
                mask_bottom_y4
            );
            return VISIBILITY_TOO_FAR;
        }

        return classify_fov_mask_ground_fallback(&projection, &space, &clip, world_x, world_z, mask_half_size_x4);
    }

    if (projection.z_clip <= (lod_shift << 6)) {
        return VISIBILITY_OUT_OF_BOUNDS_X;
    }

    {
        f32 ground_height_y = sample_ground_height_at_xz(world_x >> 16, world_z >> 16) / 65536.0f;
        if (billboard_project_z_clip(projection.pos_x, projection.pos_y, ground_height_y) <= -3.0f) {
            return VISIBILITY_OUT_OF_BOUNDS_X;
        }
    }
    return VISIBILITY_OUT_OF_BOUNDS_Y;
}
