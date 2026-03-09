#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_rt64_tagging.h"

#define MTX_INTPART_PACK(w1, w2)  (((w1) & 0xFFFF0000) | (((w2) & 0xFFFF0000) >> 16))
#define MTX_FRACPART_PACK(w1, w2) (((w1) << 16) | ((w2) & 0xFFFF))
#define SQ(x) ((x) * (x))

#define BILLBOARD_FRAME_NONE 0xFFFFFFFFu

#define ENERGY_FRAME_COUNT             8u
#define ENERGY_FRAME_MASK              (ENERGY_FRAME_COUNT - 1u)
#define ENERGY_ANIM_PHASE_SHIFT        1u
#define ENERGY_ANIM_PHASE_MASK         ((ENERGY_FRAME_COUNT * 2u) - 1u)
#define ENERGY_COLOR_FRAME_SHIFT       11u
#define ENERGY_MASK_FRAME_SHIFT        10u
#define ENERGY_COLOR_IMAGE_WIDTH       1u
#define ENERGY_MASK_IMAGE_WIDTH        32u
#define BILLBOARD_TILE_COORD_MAX_32X32 124u
#define BILLBOARD_TILE_MASK_BITS_32X32 5u
#define ENERGY_COLOR_TILE_LINE         8u
#define ENERGY_MASK_TILE_LINE          4u
#define ENERGY_MASK_TILE_TMEM          0x0100u
#define ENERGY_MASK_RENDER_TILE        1u
#define ENERGY_COLOR_BLOCK_LRS         1023u
#define ENERGY_COLOR_BLOCK_DXT         256u
#define BILLBOARD_TRACKER_CAPACITY 128
#define BILLBOARD_TRACKER_MATCH_WINDOW 256
#define BILLBOARD_VERTEX_POOL_CAPACITY 1000

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

typedef struct {
    u32 family_id;
    s32 world_x;
    s32 world_z;
    s32 world_y;
    s32 sig0;
    s32 sig1;
    s32 sig2;
    f32 z_clip;
    f32 screen_left_x4;
    f32 screen_right_x4;
    f32 screen_down_y4;
    f32 screen_up_y4;
    s16 tex_u_max;
    s16 tex_v_max;
} BillboardGeometryDesc;

typedef struct {
    u32 stable_id;
    s32 world_x;
    s32 world_z;
    s32 world_y;
    s32 sig0;
    s32 sig1;
    s32 sig2;
    u8 matched;
} BillboardTrackedId;

typedef struct {
    BillboardTrackedId prev[BILLBOARD_TRACKER_CAPACITY];
    BillboardTrackedId current[BILLBOARD_TRACKER_CAPACITY];
    s32 prev_count;
    s32 current_count;
} BillboardTrackerState;

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

static BillboardTrackerState s_billboard_trackers[BILLBOARD_FAMILY_COUNT];
static u32 s_billboard_tracker_frame = BILLBOARD_FRAME_NONE;
static s32 s_billboard_signature_override_active = 0;
static s32 s_billboard_signature_override_sig0 = 0;
static s32 s_billboard_signature_override_sig1 = 0;
static s32 s_billboard_signature_override_sig2 = 0;
static s16 s_energy_anim_phase = 0;

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

static inline s32 energy_frame_index(void) {
    return (s32)(((u16)s_energy_anim_phase >> ENERGY_ANIM_PHASE_SHIFT) & ENERGY_FRAME_MASK);
}

static inline void* energy_color_frame_ptr(s32 frame) {
    return img_D_01040CB0_7A580_rgba16__png + ((frame & ENERGY_FRAME_MASK) << ENERGY_COLOR_FRAME_SHIFT);
}

static inline void* energy_mask_frame_ptr(s32 frame) {
    return D_0103ECB0_78580 + ((frame & ENERGY_FRAME_MASK) << ENERGY_MASK_FRAME_SHIFT);
}

static inline void energy_set_wrap_tile(Gfx **dl, u32 fmt, u32 siz, u32 line, u32 tmem, u32 tile) {
    gDPSetTile(
        (*dl)++,
        fmt, siz, line, tmem, tile, 0,
        G_TX_NOMIRROR | G_TX_WRAP, BILLBOARD_TILE_MASK_BITS_32X32, G_TX_NOLOD,
        G_TX_NOMIRROR | G_TX_WRAP, BILLBOARD_TILE_MASK_BITS_32X32, G_TX_NOLOD
    );
}

static inline void energy_set_32x32_tile_size(Gfx **dl, u32 tile) {
    gDPSetTileSize((*dl)++, tile, 0, 0, BILLBOARD_TILE_COORD_MAX_32X32, BILLBOARD_TILE_COORD_MAX_32X32);
}

static inline void setup_energy_billboard_material(Gfx **dl, s32 frame) {
    void* color_ptr;
    void* mask_ptr;

    frame &= ENERGY_FRAME_MASK;
    color_ptr = energy_color_frame_ptr(frame);
    mask_ptr = energy_mask_frame_ptr(frame);

    gDPSetTextureImage((*dl)++, G_IM_FMT_RGBA, G_IM_SIZ_16b, ENERGY_COLOR_IMAGE_WIDTH, OS_PHYSICAL_TO_K0(color_ptr));
    energy_set_wrap_tile(dl, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0x0000, G_TX_LOADTILE);
    gDPLoadSync((*dl)++);
    gDPLoadBlock((*dl)++, G_TX_LOADTILE, 0, 0, ENERGY_COLOR_BLOCK_LRS, ENERGY_COLOR_BLOCK_DXT);
    gDPPipeSync((*dl)++);
    energy_set_wrap_tile(dl, G_IM_FMT_RGBA, G_IM_SIZ_16b, ENERGY_COLOR_TILE_LINE, 0x0000, G_TX_RENDERTILE);
    energy_set_32x32_tile_size(dl, G_TX_RENDERTILE);

    gDPSetTextureImage((*dl)++, G_IM_FMT_I, G_IM_SIZ_8b, ENERGY_MASK_IMAGE_WIDTH, (void*)(((u32)(uintptr_t)mask_ptr) + 0x80000000u));
    energy_set_wrap_tile(dl, G_IM_FMT_I, G_IM_SIZ_8b, ENERGY_MASK_TILE_LINE, ENERGY_MASK_TILE_TMEM, G_TX_LOADTILE);
    gDPLoadSync((*dl)++);
    gDPLoadTile((*dl)++, G_TX_LOADTILE, 0, 0, BILLBOARD_TILE_COORD_MAX_32X32, BILLBOARD_TILE_COORD_MAX_32X32);
    gDPPipeSync((*dl)++);
    energy_set_wrap_tile(dl, G_IM_FMT_I, G_IM_SIZ_8b, ENERGY_MASK_TILE_LINE, ENERGY_MASK_TILE_TMEM, ENERGY_MASK_RENDER_TILE);
    energy_set_32x32_tile_size(dl, ENERGY_MASK_RENDER_TILE);
    gDPSetCombineLERP(
        (*dl)++,
        PRIMITIVE, 0, TEXEL0, 0,
        PRIMITIVE, 0, TEXEL1, 0,
        0, 0, 0, COMBINED,
        0, 0, 0, COMBINED
    );
}

static inline s32 billboard_abs_i32(s32 value) {
    return (value < 0) ? -value : value;
}

static inline f32 billboard_abs_f32(f32 value) {
    return (value < 0.0f) ? -value : value;
}

static inline f32 billboard_sqrt_f32(f32 value) {
    return __builtin_sqrtf(value);
}

static inline void billboard_copy_tracked_ids(BillboardTrackedId* dst, const BillboardTrackedId* src, s32 count) {
    s32 i;

    for (i = 0; i < count; i++) {
        dst[i] = src[i];
    }
}

static inline s32 billboard_is_geometry_family(u32 family_id) {
    return (family_id == BILLBOARD_FAMILY_STAR) ||
           (family_id == BILLBOARD_FAMILY_ENERGY) ||
           (family_id == BILLBOARD_FAMILY_COLLECTIBLE) ||
           (family_id == BILLBOARD_FAMILY_DUALSCALE);
}

static inline void billboard_signature_override_begin(s32 sig0, s32 sig1, s32 sig2) {
    s_billboard_signature_override_active = 1;
    s_billboard_signature_override_sig0 = sig0;
    s_billboard_signature_override_sig1 = sig1;
    s_billboard_signature_override_sig2 = sig2;
}

static inline void billboard_signature_override_end(void) {
    s_billboard_signature_override_active = 0;
    s_billboard_signature_override_sig0 = 0;
    s_billboard_signature_override_sig1 = 0;
    s_billboard_signature_override_sig2 = 0;
}

static inline BillboardTrackerState* billboard_tracker_for_family(u32 family_id) {
    if (!billboard_is_geometry_family(family_id)) {
        return NULL;
    }
    return &s_billboard_trackers[family_id];
}

static inline void billboard_tracker_begin_frame(void) {
    s32 family_id;
    u32 frame = rc_frame_index();
    s32 drop_prev = 0;

    if (s_billboard_tracker_frame == frame) {
        return;
    }

    if ((s_billboard_tracker_frame == BILLBOARD_FRAME_NONE) || ((s_billboard_tracker_frame + 1u) != frame) || (skip_perspective_interpolation != 0)) {
        drop_prev = 1;
    }

    for (family_id = BILLBOARD_FAMILY_STAR; family_id <= BILLBOARD_FAMILY_DUALSCALE; family_id++) {
        BillboardTrackerState* tracker = &s_billboard_trackers[family_id];
        s32 i;

        if (drop_prev != 0) {
            tracker->prev_count = 0;
        } else {
            tracker->prev_count = tracker->current_count;
            billboard_copy_tracked_ids(tracker->prev, tracker->current, tracker->current_count);
            for (i = 0; i < tracker->prev_count; i++) {
                tracker->prev[i].matched = 0;
            }
        }

        tracker->current_count = 0;
    }

    s_billboard_tracker_frame = frame;
}

static inline void billboard_resolve_signature(const BillboardGeometryDesc* desc, s32* sig0, s32* sig1, s32* sig2) {
    if (s_billboard_signature_override_active != 0) {
        *sig0 = s_billboard_signature_override_sig0;
        *sig1 = s_billboard_signature_override_sig1;
        *sig2 = s_billboard_signature_override_sig2;
    } else {
        *sig0 = desc->sig0;
        *sig1 = desc->sig1;
        *sig2 = desc->sig2;
    }
}

static inline s32 billboard_acquire_stable_id(const BillboardGeometryDesc* desc, u32* out_stable_id) {
    BillboardTrackerState* tracker;
    s32 cur_world_x;
    s32 cur_world_z;
    s32 cur_world_y;
    s32 sig0;
    s32 sig1;
    s32 sig2;
    s32 best_index = -1;
    s32 best_distance = 0x7FFFFFFF;
    s32 i;
    BillboardTrackedId* cur_entry;

    if ((out_stable_id == NULL) || !billboard_is_geometry_family(desc->family_id)) {
        return 0;
    }

    billboard_tracker_begin_frame();
    tracker = billboard_tracker_for_family(desc->family_id);
    if (tracker == NULL) {
        return 0;
    }

    if (tracker->current_count >= BILLBOARD_TRACKER_CAPACITY) {
        return 0;
    }

    cur_world_x = desc->world_x >> 16;
    cur_world_z = desc->world_z >> 16;
    cur_world_y = desc->world_y >> 16;
    billboard_resolve_signature(desc, &sig0, &sig1, &sig2);

    for (i = 0; i < tracker->prev_count; i++) {
        BillboardTrackedId* prev_entry = &tracker->prev[i];
        s32 dx;
        s32 dz;
        s32 dy;
        s32 distance;

        if (prev_entry->matched != 0) {
            continue;
        }
        if ((prev_entry->sig0 != sig0) || (prev_entry->sig1 != sig1) || (prev_entry->sig2 != sig2)) {
            continue;
        }

        dx = billboard_abs_i32(prev_entry->world_x - cur_world_x);
        dz = billboard_abs_i32(prev_entry->world_z - cur_world_z);
        dy = billboard_abs_i32(prev_entry->world_y - cur_world_y);
        if ((dx > BILLBOARD_TRACKER_MATCH_WINDOW) || (dz > BILLBOARD_TRACKER_MATCH_WINDOW) || (dy > BILLBOARD_TRACKER_MATCH_WINDOW)) {
            continue;
        }

        distance = dx + dz + dy;
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }

    if (best_index >= 0) {
        *out_stable_id = tracker->prev[best_index].stable_id;
        tracker->prev[best_index].matched = 1;
    } else {
        *out_stable_id = rc_alloc_billboard_transform_id((RcBillboardFamily)desc->family_id);
    }

    cur_entry = &tracker->current[tracker->current_count++];
    cur_entry->stable_id = *out_stable_id;
    cur_entry->world_x = cur_world_x;
    cur_entry->world_z = cur_world_z;
    cur_entry->world_y = cur_world_y;
    cur_entry->sig0 = sig0;
    cur_entry->sig1 = sig1;
    cur_entry->sig2 = sig2;
    cur_entry->matched = 0;
    return 1;
}

static inline s16 billboard_round_to_s16(f32 value) {
    if (value >= 32767.0f) {
        return 32767;
    }
    if (value <= -32768.0f) {
        return -32768;
    }
    return (s16)((value >= 0.0f) ? (value + 0.5f) : (value - 0.5f));
}

static inline s32 billboard_extract_basis_vectors(
    f32* right_x,
    f32* right_z,
    f32* right_y,
    f32* up_x,
    f32* up_z,
    f32* up_y
) {
    f32 rx = billboard_cache_value(BILLBOARD_CACHE_ROW_VIEW_X, BILLBOARD_CACHE_COEFF_X);
    f32 rz = billboard_cache_value(BILLBOARD_CACHE_ROW_VIEW_X, BILLBOARD_CACHE_COEFF_Y);
    f32 ry = billboard_cache_value(BILLBOARD_CACHE_ROW_VIEW_X, BILLBOARD_CACHE_COEFF_Z);
    f32 ux = billboard_cache_value(BILLBOARD_CACHE_ROW_VIEW_Y, BILLBOARD_CACHE_COEFF_X);
    f32 uz = billboard_cache_value(BILLBOARD_CACHE_ROW_VIEW_Y, BILLBOARD_CACHE_COEFF_Y);
    f32 uy = billboard_cache_value(BILLBOARD_CACHE_ROW_VIEW_Y, BILLBOARD_CACHE_COEFF_Z);
    f32 r_len = billboard_sqrt_f32((rx * rx) + (rz * rz) + (ry * ry));
    f32 u_len = billboard_sqrt_f32((ux * ux) + (uz * uz) + (uy * uy));

    if ((r_len <= 0.0001f) || (u_len <= 0.0001f)) {
        return 0;
    }

    *right_x = rx / r_len;
    *right_z = rz / r_len;
    *right_y = ry / r_len;
    *up_x = ux / u_len;
    *up_z = uz / u_len;
    *up_y = uy / u_len;
    return 1;
}

static inline s32 billboard_screen_extent_to_world_half(
    f32 z_clip,
    f32 screen_extent_x4,
    f32 screen_extent_y4,
    f32* out_world_x,
    f32* out_world_y
) {
    f32 screen_row_x = billboard_abs_f32(billboard_cache_value(BILLBOARD_CACHE_ROW_SCREEN, BILLBOARD_CACHE_COEFF_X));
    f32 screen_row_y = billboard_abs_f32(billboard_cache_value(BILLBOARD_CACHE_ROW_SCREEN, BILLBOARD_CACHE_COEFF_Y));

    if ((screen_row_x <= 0.0001f) || (screen_row_y <= 0.0001f)) {
        return 0;
    }

    *out_world_x = screen_extent_x4 * (-z_clip) / screen_row_x;
    *out_world_y = screen_extent_y4 * (-z_clip) / screen_row_y;
    return 1;
}

static inline void billboard_write_vertex(Vtx* vertex, f32 x, f32 z, f32 y, s16 tc_s, s16 tc_t) {
    vertex->v.ob[0] = billboard_round_to_s16(x);
    vertex->v.ob[1] = billboard_round_to_s16(z);
    vertex->v.ob[2] = billboard_round_to_s16(y);
    vertex->v.flag = 0;
    vertex->v.tc[0] = tc_s;
    vertex->v.tc[1] = tc_t;
    vertex->v.cn[0] = 0xFF;
    vertex->v.cn[1] = 0xFF;
    vertex->v.cn[2] = 0xFF;
    vertex->v.cn[3] = 0xFF;
}

static inline s16 billboard_tc_from_texel_max(s16 texel_max) {
    return (s16)(texel_max << 7);
}

static inline s32 billboard_emit_world_quad(const BillboardGeometryDesc* desc, Gfx** dl) {
    f32 right_x;
    f32 right_z;
    f32 right_y;
    f32 up_x;
    f32 up_z;
    f32 up_y;
    f32 world_left;
    f32 world_right;
    f32 world_down;
    f32 world_up;
    f32 center_x;
    f32 center_z;
    f32 center_y;
    u32 stable_id;
    s32 vtx_index;
    Vtx* vertices;

    if ((dl == NULL) || ((*dl) == NULL) || !billboard_is_geometry_family(desc->family_id)) {
        return 0;
    }
    if ((gDisplayListContext == NULL) || (gDisplayListContext->usedVtxs < 0)) {
        return 0;
    }
    if ((gDisplayListContext->usedVtxs + 4) > BILLBOARD_VERTEX_POOL_CAPACITY) {
        return 0;
    }
    if (!billboard_extract_basis_vectors(&right_x, &right_z, &right_y, &up_x, &up_z, &up_y)) {
        return 0;
    }
    if (!billboard_screen_extent_to_world_half(desc->z_clip, desc->screen_left_x4, desc->screen_down_y4, &world_left, &world_down) ||
        !billboard_screen_extent_to_world_half(desc->z_clip, desc->screen_right_x4, desc->screen_up_y4, &world_right, &world_up)) {
        return 0;
    }
    if (!billboard_acquire_stable_id(desc, &stable_id)) {
        return 0;
    }

    vtx_index = gDisplayListContext->usedVtxs;
    vertices = &gDisplayListContext->unk2C570[vtx_index];
    center_x = desc->world_x / 65536.0f;
    center_z = desc->world_z / 65536.0f;
    center_y = desc->world_y / 65536.0f;

    billboard_write_vertex(
        &vertices[0],
        center_x - (right_x * world_left) - (up_x * world_down),
        center_z - (right_z * world_left) - (up_z * world_down),
        center_y - (right_y * world_left) - (up_y * world_down),
        0,
        billboard_tc_from_texel_max(desc->tex_v_max)
    );
    billboard_write_vertex(
        &vertices[1],
        center_x + (right_x * world_right) - (up_x * world_down),
        center_z + (right_z * world_right) - (up_z * world_down),
        center_y + (right_y * world_right) - (up_y * world_down),
        billboard_tc_from_texel_max(desc->tex_u_max),
        billboard_tc_from_texel_max(desc->tex_v_max)
    );
    billboard_write_vertex(
        &vertices[2],
        center_x + (right_x * world_right) + (up_x * world_up),
        center_z + (right_z * world_right) + (up_z * world_up),
        center_y + (right_y * world_right) + (up_y * world_up),
        billboard_tc_from_texel_max(desc->tex_u_max),
        0
    );
    billboard_write_vertex(
        &vertices[3],
        center_x - (right_x * world_left) + (up_x * world_up),
        center_z - (right_z * world_left) + (up_z * world_up),
        center_y - (right_y * world_left) + (up_y * world_up),
        0,
        0
    );

    gDisplayListContext->usedVtxs += 4;

    gSPMatrix((*dl)++, &gDisplayListContext->unk374D0, G_MTX_PUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    rt64_tag_model_matrix(dl, stable_id, 1);
    gDPSetDepthSource((*dl)++, G_ZS_PIXEL);
    gSPVertex((*dl)++, &gDisplayListContext->unk2C570[vtx_index], 4, 0);
    gSP1Triangle((*dl)++, 0, 1, 2, 0);
    gSP1Triangle((*dl)++, 0, 2, 3, 0);
    rt64_pop_model_matrix(dl);
    gSPPopMatrix((*dl)++, G_MTX_MODELVIEW);
    return 1;
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
    return (u16)((depth_factor * 1023.0f * ENERGY_MASK_IMAGE_WIDTH) + 32736.0f);
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
    return 1;
}

static inline void append_legacy_billboard_texrect(
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
    gSPScisTextureRectangle(*dl, xl, yl, xh, yh, G_TX_RENDERTILE, s, t, dsdx, dtdy);
    *dl += 3;
}

static inline void emit_billboard_geometry_or_fallback(
    const BillboardGeometryDesc* desc,
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
    if (!billboard_emit_world_quad(desc, dl)) {
        append_legacy_billboard_texrect(dl, xl, yl, xh, yh, s, t, dsdx, dtdy);
    }
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
}

RECOMP_PATCH void draw_star_billboard_texrect(Gfx **dl, s32 world_x, s32 world_z, s32 world_y, s16 tex_half_width, s16 tex_half_height, s32 sprite_scale) {
    BillboardProjection projection;
    f32 size_x4;
    f32 xOffset;
    f32 yOffset;
    f32 xl;
    f32 yl;
    f32 xh;
    f32 yh;
    s16 dsdx;
    s16 dtdy;
    BillboardScreenSpace space;
    const BillboardFamilyPolicy* policy;
    BillboardGeometryDesc geometry_desc;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x6C5E44u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_STAR);
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
                xOffset = (tex_half_width * size_x4) / 128.0f;
                yOffset = (tex_half_height * size_x4) / 128.0f;
                xl = projection.screen_x4 - xOffset;
                xh = projection.screen_x4 + xOffset;
                yl = projection.screen_y4 - yOffset;
                yh = projection.screen_y4 + yOffset;

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_height << 12) / (xh - xl));
                    dtdy = (s16)((tex_half_width << 12) / (yh - yl));
                    geometry_desc.family_id = BILLBOARD_FAMILY_STAR;
                    geometry_desc.world_x = world_x;
                    geometry_desc.world_z = world_z;
                    geometry_desc.world_y = world_y;
                    geometry_desc.sig0 = tex_half_width;
                    geometry_desc.sig1 = tex_half_height;
                    geometry_desc.sig2 = sprite_scale;
                    geometry_desc.z_clip = projection.z_clip;
                    geometry_desc.screen_left_x4 = xOffset;
                    geometry_desc.screen_right_x4 = xOffset;
                    geometry_desc.screen_down_y4 = yOffset;
                    geometry_desc.screen_up_y4 = yOffset;
                    geometry_desc.tex_u_max = tex_half_height;
                    geometry_desc.tex_v_max = tex_half_width;

                    emit_billboard_geometry_or_fallback(
                        &geometry_desc,
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
    const BillboardFamilyPolicy* policy;
    BillboardGeometryDesc geometry_desc;
    dsdx = 0;
    dtdy = 0;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x73F17Cu);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_ENERGY);

    if (project_billboard_to_screen(BILLBOARD_FAMILY_ENERGY, policy, world_x, world_z, world_y, &space, &projection)) {
        if (projection.depth_factor > 0.0f) {
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

                if ((xl < xh) && (yl < yh)) {
                    dsdx = (s16)((tex_half_width << 12) / (yh - yl));
                    dtdy = (s16)((tex_half_height << 12) / (xh - xl));
                    geometry_desc.family_id = BILLBOARD_FAMILY_ENERGY;
                    geometry_desc.world_x = world_x;
                    geometry_desc.world_z = world_z;
                    geometry_desc.world_y = world_y;
                    geometry_desc.sig0 = tex_half_width;
                    geometry_desc.sig1 = tex_half_height;
                    geometry_desc.sig2 = sprite_scale;
                    geometry_desc.z_clip = projection.z_clip;
                    geometry_desc.screen_left_x4 = xOffset;
                    geometry_desc.screen_right_x4 = xOffset;
                    geometry_desc.screen_down_y4 = yOffset;
                    geometry_desc.screen_up_y4 = yOffset;
                    geometry_desc.tex_u_max = tex_half_width;
                    geometry_desc.tex_v_max = tex_half_height;

                    emit_billboard_geometry_or_fallback(
                        &geometry_desc,
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
                }
            }
        }
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
    const BillboardFamilyPolicy* policy;
    BillboardGeometryDesc geometry_desc;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x73F800u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_COLLECTIBLE);

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
                    geometry_desc.family_id = BILLBOARD_FAMILY_COLLECTIBLE;
                    geometry_desc.world_x = world_x;
                    geometry_desc.world_z = world_z;
                    geometry_desc.world_y = world_y;
                    geometry_desc.sig0 = tex_half_width;
                    geometry_desc.sig1 = tex_half_height | ((has_yoffset != 0) ? 0x10000 : 0);
                    geometry_desc.sig2 = sprite_scale;
                    geometry_desc.z_clip = projection.z_clip;
                    geometry_desc.screen_left_x4 = xOffset;
                    geometry_desc.screen_right_x4 = xOffset;
                    geometry_desc.screen_down_y4 = yOffset;
                    geometry_desc.screen_up_y4 = (has_yoffset != 0) ? (yOffset * 3.0f) : yOffset;
                    geometry_desc.tex_u_max = tex_half_height;
                    geometry_desc.tex_v_max = tex_half_width;

                    emit_billboard_geometry_or_fallback(
                        &geometry_desc,
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
    const BillboardFamilyPolicy* policy;
    BillboardGeometryDesc geometry_desc;

    rc_note_texrect_context(TRC_WORLD_BILLBOARD, 0x740094u);
    space = get_billboard_space();
    policy = get_billboard_family_policy(BILLBOARD_FAMILY_DUALSCALE);
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
                    geometry_desc.family_id = BILLBOARD_FAMILY_DUALSCALE;
                    geometry_desc.world_x = world_x;
                    geometry_desc.world_z = world_z;
                    geometry_desc.world_y = world_y;
                    geometry_desc.sig0 = tex_half_width;
                    geometry_desc.sig1 = tex_half_height;
                    geometry_desc.sig2 = (s32)(((u32)(u16)sprite_scale_x << 16) | (u16)sprite_scale_y);
                    geometry_desc.z_clip = projection.z_clip;
                    geometry_desc.screen_left_x4 = xOffset;
                    geometry_desc.screen_right_x4 = xOffset;
                    geometry_desc.screen_down_y4 = yOffset;
                    geometry_desc.screen_up_y4 = yOffset;
                    geometry_desc.tex_u_max = tex_half_height;
                    geometry_desc.tex_v_max = tex_half_width;

                    emit_billboard_geometry_or_fallback(
                        &geometry_desc,
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
                        append_legacy_billboard_texrect(
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

RECOMP_PATCH void render_dynamic_texture_billboards_6AE758(void) {
    u8 loaded_texture;
    u8 loaded_texture_2;
    s8 i;
    u8 r;
    u8 g;
    u8 b;
    DynamicTextureMinimal* tex;
    static s32 s_collectible_anim_counter;
    static s32 s_collectible_anim_step;

    loaded_texture = 0xFF;
    loaded_texture_2 = 0xFF;

    s_energy_anim_phase += 1;
    s_energy_anim_phase &= (s16)ENERGY_ANIM_PHASE_MASK;

    if ((guRandom() % 20) == 1) {
        if (s_collectible_anim_step == 5) {
            s_collectible_anim_step = 1;
        } else {
            s_collectible_anim_step = 5;
        }
    }

    s_collectible_anim_counter += s_collectible_anim_step;
    s_collectible_anim_counter %= 9;

    gSPDisplayList(gLayer0DL++, D_01004308_3DBD8);
    gSPDisplayList(gAuxDL++, D_01004B98_3E468);

    for (i = 0; i != -1; i = gDynamicTextureBillboardQueue.textures[i].next) {
        tex = &gDynamicTextureBillboardQueue.textures[i];

        if (tex->category < 32) {
            if (tex->category != loaded_texture) {
                if (loaded_texture != 0xFF) {
                    gDynamicTextureBillboardQueue.textureGroups[loaded_texture] = -1;
                }
                loaded_texture = tex->category;
                if (loaded_texture == 0) {
                    load_dynamic_texture_billboard_texture_pair(&gAuxDL, loaded_texture + (s_collectible_anim_counter / 3));
                } else {
                    load_dynamic_texture_billboard_texture_pair(&gAuxDL, loaded_texture);
                }
            }

            billboard_signature_override_begin(
                tex->category,
                ((s32)(u8)tex->unk10 << 16) | (u16)(s_collectible_anim_counter / 3),
                tex->size
            );
            if (tex->unk10 == 127) {
                draw_collectible_billboard_texrect(&gAuxDL, tex->xPos, tex->zPos, tex->yPos, 31, 63, tex->size * 3);
            } else {
                draw_collectible_billboard_texrect(&gAuxDL, tex->xPos, tex->zPos, tex->yPos, 31, 31, tex->size * 3);
            }
            billboard_signature_override_end();
            gDPPipeSync(gAuxDL++);
        } else {
            r = tex->red;
            g = tex->green;
            b = tex->blue;

            if (tex->category < 48) {
                gDPSetPrimColor(gLayer0DL++, 0, 0, r, g, b, 0xFF);
                gDPSetEnvColor(gLayer0DL++, r, g, b, 0);
            }
            if (tex->category != loaded_texture_2) {
                if (loaded_texture_2 != 0xFF) {
                    gDynamicTextureBillboardQueue.textureGroups[loaded_texture_2] = -1;
                }
                loaded_texture_2 = tex->category;
                if (loaded_texture_2 < 48) {
                    gDPSetCombineLERP(
                        gLayer0DL++,
                        PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT,
                        PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT,
                        0, 0, 0, COMBINED,
                        0, 0, 0, COMBINED
                    );

                    gDPSetTextureImage(gLayer0DL++, G_IM_FMT_I, G_IM_SIZ_16b, 1, OS_PHYSICAL_TO_K0(D_01029E10_636E0 + ((loaded_texture_2 << 10) - 0x8000)));
                    gDPSetTile(gLayer0DL++, G_IM_FMT_I, G_IM_SIZ_16b, 0, 0x0000, G_TX_LOADTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, 5, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, 5, G_TX_NOLOD);
                    gDPLoadSync(gLayer0DL++);
                    gDPLoadBlock(gLayer0DL++, G_TX_LOADTILE, 0, 0, 511, 512);
                    gDPPipeSync(gLayer0DL++);
                    gDPSetTile(gLayer0DL++, G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x0000, G_TX_RENDERTILE, 0, G_TX_NOMIRROR | G_TX_WRAP, 5, G_TX_NOLOD, G_TX_NOMIRROR | G_TX_WRAP, 5, G_TX_NOLOD);
                    gDPSetTileSize(gLayer0DL++, G_TX_RENDERTILE, 0, 0, BILLBOARD_TILE_COORD_MAX_32X32, BILLBOARD_TILE_COORD_MAX_32X32);
                } else if (loaded_texture_2 == 48) {
                    setup_energy_billboard_material(&gLayer0DL, energy_frame_index());
                }
            }
            if (loaded_texture_2 == 0x30) {
                if (tex->unk10 == 60) {
                    gDPSetPrimColor(gLayer0DL++, 0, 0, 255, 255, 255, 255);
                }
                if (tex->unk10 == 59) {
                    gDPSetPrimColor(gLayer0DL++, 0, 0, 255, 160, 255, 255);
                }
                if (tex->unk10 == 58) {
                    gDPSetPrimColor(gLayer0DL++, 0, 0, 255, 0, 255, 255);
                }
            }

            gDPSetDepthSource(gLayer0DL++, G_ZS_PRIM);
            gDPSetRenderMode(gLayer0DL++, G_RM_PASS, G_RM_AA_ZB_XLU_SURF2);

            billboard_signature_override_begin(
                tex->category,
                ((s32)(u8)tex->unk10 << 24) | ((s32)tex->red << 16) | ((s32)tex->green << 8) | tex->blue,
                (loaded_texture_2 == 48) ? energy_frame_index() : tex->size
            );
            draw_energy_billboard_texrect(&gLayer0DL, tex->xPos, tex->zPos, tex->yPos, 0x1F, 0x1F, tex->size * 3);
            billboard_signature_override_end();
            gDPPipeSync(gLayer0DL++);
        }
    }

    billboard_signature_override_end();
    gDynamicTextureBillboardQueue.unk0 = -1;
    gDynamicTextureBillboardQueue.unk1 = -1;
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
