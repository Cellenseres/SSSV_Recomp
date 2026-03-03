#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_rt64_tagging.h"
#include "sssv_patch_trace.h"
#include "sssv_patch_ui_domain.h"
#include "sssv_patch_display_policy.h"

#ifndef G_CD_BAYER
#define G_CD_BAYER 0x00000040u
#endif

#ifndef G_AD_PATTERN
#define G_AD_PATTERN 0x00000000u
#endif

static u32 g_dbg_draw_rectangle_calls = 0;
static u32 g_dbg_intro_frame_calls = 0;
static u32 g_dbg_end_display_lists_calls = 0;
static u32 g_dbg_world_perspective_calls = 0;
static u32 g_dbg_depth_clear_calls = 0;
static u32 g_dbg_world_depth_failsafe_calls = 0;

void setup_intro_perspective_638558(Gfx** dl);

static inline void write_main_viewport_expand(s16 viewport_width, s16 viewport_height) {
    gMainViewport.vscale[0] = (s16)(viewport_width * 2);
    gMainViewport.vscale[1] = (s16)(viewport_height * 2);
    gMainViewport.vtrans[0] = (s16)(SSSV_BASE_WIDTH * 2);
    gMainViewport.vtrans[1] = (s16)(viewport_height * 2);
}

static inline s32 widescreen_scissor_active(void) {
    return compute_target_width_for_height_patch(get_safe_screen_height()) > SSSV_BASE_WIDTH;
}

static inline void full_scissor_bounds(s32* ulx, s32* uly, s32* lrx, s32* lry) {
    s32 full_height = get_safe_screen_height(); // always > 0
    s32 full_width = compute_target_width_for_height_patch((s16)full_height);

    if (full_width < SSSV_BASE_WIDTH) {
        full_width = SSSV_BASE_WIDTH;
    }
    if (gScreenWidth > full_width) {
        full_width = gScreenWidth;
    }
    if (gScreenHeight > full_height) {
        full_height = gScreenHeight;
    }

    *ulx = 0;
    *uly = 0;
    *lrx = full_width;
    *lry = full_height;
}

static inline void default_frame_scissor_bounds(s32* ulx, s32* uly, s32* lrx, s32* lry) {
    if (widescreen_scissor_active()) {
        full_scissor_bounds(ulx, uly, lrx, lry);
    } else {
        display_policy_safe_scissor_bounds(ulx, uly, lrx, lry);
    }
}

typedef struct {
    s32 ulx;
    s32 uly;
    s32 lrx;
    s32 lry;
} ScissorRect;

static inline void fill_full_scissor_rect(ScissorRect* rect) {
    full_scissor_bounds(&rect->ulx, &rect->uly, &rect->lrx, &rect->lry);
}

static inline void fill_safe_scissor_rect(ScissorRect* rect) {
    display_policy_safe_scissor_bounds(&rect->ulx, &rect->uly, &rect->lrx, &rect->lry);
}

static inline void fill_intro_default_scissor_rect(s32 widescreen_overlay, ScissorRect* rect) {
    if (widescreen_overlay != 0) {
        fill_full_scissor_rect(rect);
    } else {
        fill_safe_scissor_rect(rect);
    }
}

static inline void set_scissor_rect(Gfx** dl, const ScissorRect* rect) {
    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, rect->ulx, rect->uly, rect->lrx, rect->lry);
}

static inline void set_default_frame_scissor(Gfx** dl) {
    ScissorRect rect;

    default_frame_scissor_bounds(&rect.ulx, &rect.uly, &rect.lrx, &rect.lry);
    set_scissor_rect(dl, &rect);
}

static inline void set_depth_color_image(Gfx** dl) {
    gDPSetColorImage(
        (*dl)++,
        G_IM_FMT_RGBA,
        G_IM_SIZ_16b,
        SSSV_BASE_WIDTH,
        (const void*)(uintptr_t)osVirtualToPhysical((void*)&gDepthImage)
    );
}

static inline void set_framebuffer_color_image(Gfx** dl) {
    gDPSetColorImage(
        (*dl)++,
        G_IM_FMT_RGBA,
        G_IM_SIZ_16b,
        SSSV_BASE_WIDTH,
        (const void*)(uintptr_t)osVirtualToPhysical((void*)gFrameContext->framebuffer)
    );
}

static inline void fill_native_depth_target(Gfx** dl) {
    set_depth_color_image(dl);
    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, 0, 0, SSSV_BASE_WIDTH, SSSV_BASE_HEIGHT);
    gDPSetFillColor((*dl)++, 0xFFFCFFFCu);
    gDPFillRectangle((*dl)++, 0, 0, SSSV_BASE_WIDTH - 1, SSSV_BASE_HEIGHT - 1);
}

static inline void clamp_scissor_interval(s32* min_value, s32* max_value, s32 bound_min, s32 bound_max) {
    if (*min_value < bound_min) {
        *min_value = bound_min;
    }
    if (*max_value > bound_max) {
        *max_value = bound_max;
    }
    if (*max_value < *min_value) {
        *max_value = *min_value;
    }
}

static inline void compute_intro_reveal_scissor_rect(u16 reveal_step, const ScissorRect* inset_rect, ScissorRect* reveal_rect) {
    const s32 center_x = SSSV_BASE_WIDTH >> 1;
    const s32 center_y = SSSV_BASE_HEIGHT >> 1;

    if (reveal_step < 20) {
        reveal_rect->ulx = (center_x - ((s32)reveal_step << 3)) - 1;
        reveal_rect->lrx = center_x + ((s32)reveal_step << 3) + 1;
        reveal_rect->uly = center_y;
        reveal_rect->lry = center_y + 1;
        clamp_scissor_interval(&reveal_rect->ulx, &reveal_rect->lrx, inset_rect->ulx, inset_rect->lrx);
        clamp_scissor_interval(&reveal_rect->uly, &reveal_rect->lry, inset_rect->uly, inset_rect->lry);
        return;
    }

    reveal_rect->ulx = inset_rect->ulx;
    reveal_rect->lrx = inset_rect->lrx;
    reveal_rect->uly = (center_y - (((s32)reveal_step / 2) * 0xC)) + (center_y - 1);
    reveal_rect->lry = (center_y + (((s32)reveal_step / 2) * 0xC)) - center_y;
    clamp_scissor_interval(&reveal_rect->uly, &reveal_rect->lry, inset_rect->uly, inset_rect->lry);
}

static inline void append_sp_texture_cmd(Gfx* pkt, u16 s, u16 t, u8 level, u8 tile, u8 on) {
    pkt->words.w0 = 0xBB000000u | ((u32)level << 11) | ((u32)tile << 8) | (u32)on;
    pkt->words.w1 = ((u32)s << 16) | (u32)t;
}

static inline void append_enddl_cmd(Gfx* pkt) {
    pkt->words.w0 = 0xB8000000u;
    pkt->words.w1 = 0;
}

static inline void append_fullsync_cmd(Gfx* pkt) {
    pkt->words.w0 = 0xE9000000u;
    pkt->words.w1 = 0;
}

static inline void append_sp_load_ucode_ex_cmds(Gfx** dl, const void* ucode_text, const void* ucode_data, u16 data_size) {
    const u32 data_len = (data_size > 0u) ? ((u32)data_size - 1u) : 0u;

    (*dl)->words.w0 = 0xB4000000u;
    (*dl)->words.w1 = (u32)(uintptr_t)ucode_data;
    (*dl)++;

    (*dl)->words.w0 = 0xAF000000u | (data_len & 0xFFFFu);
    (*dl)->words.w1 = (u32)(uintptr_t)ucode_text;
    (*dl)++;
}

static inline void append_sp_clip_ratio_cmds(Gfx** dl, u32 neg_ratio, u32 pos_ratio) {
    (*dl)->words.w0 = 0xBC000404u;
    (*dl)->words.w1 = neg_ratio;
    (*dl)++;

    (*dl)->words.w0 = 0xBC000C04u;
    (*dl)->words.w1 = neg_ratio;
    (*dl)++;

    (*dl)->words.w0 = 0xBC001404u;
    (*dl)->words.w1 = pos_ratio;
    (*dl)++;

    (*dl)->words.w0 = 0xBC001C04u;
    (*dl)->words.w1 = pos_ratio;
    (*dl)++;
}

static inline void append_sp_clip_ratio_4_cmds(Gfx** dl) {
    append_sp_clip_ratio_cmds(dl, 0x00000004u, 0x0000FFFCu);
}

static inline void append_sp_clip_ratio_3_cmds(Gfx** dl) {
    append_sp_clip_ratio_cmds(dl, 0x00000003u, 0x0000FFFDu);
}

static inline void patch_world_chunk_clip_ratio_dl(void) {
    const u32 neg_ratio = widescreen_scissor_active() ? 0x00000003u : 0x00000001u;
    const u32 pos_ratio = widescreen_scissor_active() ? 0x0000FFFDu : 0x0000FFFFu;
    u32 found = 0;
    s32 i;

    for (i = 0; i < 64; i++) {
        Gfx* cmd = &gWorldChunkLodSetupDl[i];
        const u32 op = cmd->words.w0 & 0xFF000000u;
        if (op == 0xB8000000u) {
            break;
        }

        if (cmd->words.w0 == 0xBC000404u) {
            cmd->words.w1 = neg_ratio;
            found++;
        } else if (cmd->words.w0 == 0xBC000C04u) {
            cmd->words.w1 = neg_ratio;
            found++;
        } else if (cmd->words.w0 == 0xBC001404u) {
            cmd->words.w1 = pos_ratio;
            found++;
        } else if (cmd->words.w0 == 0xBC001C04u) {
            cmd->words.w1 = pos_ratio;
            found++;
        }

        if (found == 4u) {
            break;
        }
    }
}

static inline void append_sp_set_geometry_mode_cmd(Gfx** dl, u32 mode) {
    (*dl)->words.w0 = 0xB7000000u;
    (*dl)->words.w1 = mode;
    (*dl)++;
}

static inline void append_sp_clear_geometry_mode_cmd(Gfx** dl, u32 mode) {
    (*dl)->words.w0 = 0xB6000000u;
    (*dl)->words.w1 = mode;
    (*dl)++;
}

static inline void append_dp_set_depth_source_prim_cmd(Gfx** dl) {
    (*dl)->words.w0 = 0xB9000201u;
    (*dl)->words.w1 = 0x00000004u;
    (*dl)++;
}

static inline void append_dp_set_texture_filter_average_cmd(Gfx** dl) {
    (*dl)->words.w0 = 0xBA000C02u;
    (*dl)->words.w1 = 0x00003000u;
    (*dl)++;
}

static inline void append_dp_set_render_mode_aa_zb_tex_edge_cmd(Gfx** dl) {
    (*dl)->words.w0 = 0xB900031Du;
    (*dl)->words.w1 = 0x00553078u;
    (*dl)++;
}

static inline void append_dp_set_render_mode_zb_pcl_surf_cmd(Gfx** dl) {
    (*dl)->words.w0 = 0xB900031Du;
    (*dl)->words.w1 = 0x0F0A0233u;
    (*dl)++;
}

static inline void append_sp_num_lights_cmd(Gfx** dl, u8 num_lights) {
    (*dl)->words.w0 = 0xBC000002u;
    (*dl)->words.w1 = 0x80000000u | ((u32)num_lights << 6);
    (*dl)++;
}

static inline void append_sp_light_cmd(Gfx** dl, const void* light, u8 index) {
    const u32 move_index = 0x84u + ((u32)index << 1);

    (*dl)->words.w0 = 0x03000010u | (move_index << 16);
    (*dl)->words.w1 = (u32)(uintptr_t)light;
    (*dl)++;
}

static inline void append_sprite2d_render_state(Gfx** dl) {
    set_framebuffer_color_image(dl);
    gSPViewport((*dl)++, &gMainViewport);
    append_sp_clip_ratio_4_cmds(dl);
    gSPDisplayList((*dl)++, gSprite2DRenderSetupDl);
}

static u32 s_last_proj_view_trace_frame = 0;
static RenderContext s_last_proj_view_trace_ctx = RC_WORLD3D;

static inline void trace_proj_view_snapshot_once(RenderContext ctx) {
    const u32 frame = rc_frame_index();
    if ((sssv_patch_diag_flags & SSSV_DIAG_PROJ_VIEW) == 0u) {
        return;
    }
    if ((s_last_proj_view_trace_frame == frame) && (s_last_proj_view_trace_ctx == ctx)) {
        return;
    }

    s_last_proj_view_trace_frame = frame;
    s_last_proj_view_trace_ctx = ctx;

    PATCH_TRACE_DIAG(
        SSSV_DIAG_PROJ_VIEW,
        PATCH_TAG_PROJ_VIEW_SNAPSHOT,
        (frame << 8) | (u32)ctx,
        (u32)cur_perspective_projection_transform_id,
        ((u32)(u16)gMainViewport.vscale[0] << 16) | (u16)gMainViewport.vtrans[0]
    );
}

RECOMP_PATCH void load_segments(Gfx **arg0, DisplayList *ddl) {
    rc_invalidate_rt64_extended_state();
    rc_ensure_rt64_extended_enabled(arg0);
    // The world chunk LOD setup DL is only valid after the world projection path
    // has initialized its matrices and backing display list state.
    if (rc_world_projection_active()) {
        patch_world_chunk_clip_ratio_dl();
    }

    gSPSegment((*arg0)++, 0, 0);
    gSPSegment((*arg0)++, 1, (const void*)(uintptr_t)osVirtualToPhysical(gSegment1Base));
    gSPSegment((*arg0)++, 2, (const void*)(uintptr_t)osVirtualToPhysical(ddl));
    gSPSegment((*arg0)++, 3, (const void*)(uintptr_t)osVirtualToPhysical(gFontSegmentBase));
    gSPSegment((*arg0)++, 5, (const void*)(uintptr_t)osVirtualToPhysical(gSegment5Base));

    (*arg0)->words.w0 = 0xFE000000u; // gDPSetDepthImage
    (*arg0)->words.w1 = osVirtualToPhysical((void*)&gDepthImage);
    (*arg0)++;
}

static inline void append_world_depth_clear_failsafe(void) {
    if ((gMainDL == NULL) || (gFrameContext == NULL)) {
        return;
    }

    gDPPipeSync(gMainDL++);
    gDPSetCycleType(gMainDL++, G_CYC_FILL);
    fill_native_depth_target(&gMainDL);
    set_framebuffer_color_image(&gMainDL);
    gDPPipeSync(gMainDL++);

    g_dbg_world_depth_failsafe_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_world_depth_failsafe_calls, 8, 240)) {
        PATCH_TRACE(PATCH_TAG_WORLD_DEPTH_CLEAR_FAILSAFE, g_dbg_world_depth_failsafe_calls, (u16)gScreenWidth, (u16)gScreenHeight);
    }
}

RECOMP_PATCH void clear_depth_buffer(Gfx **arg0) {
    gDPPipeSync((*arg0)++);
    gDPSetCycleType((*arg0)++, G_CYC_FILL);

    fill_native_depth_target(arg0);

    set_default_frame_scissor(arg0);

    g_dbg_depth_clear_calls++;
    PATCH_TRACE(PATCH_TAG_DEPTH_CLEAR, g_dbg_depth_clear_calls, (u16)gScreenWidth, (u16)rc_current());
}

RECOMP_PATCH void init_f3dex_render(Gfx **dl, DisplayList *ddl) {
    gDPPipeSync((*dl)++);
    append_sp_load_ucode_ex_cmds(dl, &gspF3DEX_fifoTextStart, &gspF3DEX_fifoDataStart, 2048);
    gDPPipeSync((*dl)++);

    load_segments(dl, ddl);

    set_depth_color_image(dl);
    set_default_frame_scissor(dl);
    gDPSetColorDither((*dl)++, G_CD_BAYER);
    gDPSetAlphaDither((*dl)++, G_AD_PATTERN);

    append_sp_clip_ratio_3_cmds(dl);
}

RECOMP_PATCH void begin_f3dex_render(Gfx **dl, DisplayList *ddl) {
    gDPPipeSync((*dl)++);

    load_segments(dl, ddl);

    set_depth_color_image(dl);
    set_default_frame_scissor(dl);
    gDPSetColorDither((*dl)++, G_CD_BAYER);
    gDPSetAlphaDither((*dl)++, G_AD_PATTERN);

    append_sp_clip_ratio_3_cmds(dl);
}

RECOMP_PATCH void begin_f3dex_render_sync(Gfx **dl, DisplayList *ddl) {
    gDPPipeSync((*dl)++);
    gDPPipeSync((*dl)++);

    load_segments(dl, ddl);

    set_depth_color_image(dl);

    append_sp_clip_ratio_3_cmds(dl);

    set_default_frame_scissor(dl);
    gDPSetColorDither((*dl)++, G_CD_BAYER);
    gDPSetAlphaDither((*dl)++, G_AD_PATTERN);
}

RECOMP_PATCH void draw_rectangle(Gfx** dl, s16 x0, s16 y0, s16 x1, s16 y1, u8 r, u8 g, u8 b, u8 alpha) {
    if (alpha == 0) {
        return;
    }

    const s32 expanded_widescreen = ui_is_expand_active();
    const s16 screen_w = display_policy_frame_width();
    const s16 screen_h = display_policy_frame_height();

    s16 draw_x0 = x0, draw_y0 = y0;
    s16 draw_x1 = x1, draw_y1 = y1;

    if (expanded_widescreen) {
        if (is_legacy_fullscreen_or_overscan_rect(x0, y0, x1, y1)) {
            draw_x0 = 0; draw_y0 = 0;
            draw_x1 = screen_w; draw_y1 = screen_h;
        }
        else if ((alpha >= 0xC0) && (r <= 8) && (g <= 8) && (b <= 8) &&
                 is_legacy_overscan_border_rect(x0, y0, x1, y1)) {
            return;
        }
    }

    gDPPipeSync((*dl)++);
    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, 0, 0, screen_w, screen_h);
    gDPSetAlphaCompare((*dl)++, G_AC_NONE);

    if (alpha == 0xFF) {
        const u32 packed = GPACK_RGBA5551(r, g, b, 1);
        gDPSetCycleType((*dl)++, G_CYC_FILL);
        gDPSetRenderMode((*dl)++, G_RM_NOOP, G_RM_NOOP2);
        gDPSetFillColor((*dl)++, (packed << 16) | packed);
        gDPFillRectangle((*dl)++, draw_x0, draw_y0, draw_x1 - 1, draw_y1 - 1);
    } else {
        gDPSetCycleType((*dl)++, G_CYC_1CYCLE);
        gDPSetColorDither((*dl)++, G_CD_NOISE);
        gDPSetAlphaDither((*dl)++, G_AD_DISABLE);
        gDPSetPrimColor((*dl)++, 0, 0, r, g, b, alpha);
        if (gAttractModeState == 10) {
            gDPSetRenderMode((*dl)++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        } else {
            gDPSetRenderMode((*dl)++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
        }
        gDPSetCombineMode((*dl)++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
        gDPFillRectangle((*dl)++, draw_x0, draw_y0, draw_x1, draw_y1);
    }

    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, 0, 0, screen_w, screen_h);
    gDPPipeSync((*dl)++);

    g_dbg_draw_rectangle_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_draw_rectangle_calls, 6, 300)) {
        PATCH_TRACE(PATCH_TAG_DRAW_RECTANGLE, g_dbg_draw_rectangle_calls, (u8)alpha, (u16)screen_w);
    }
}

static inline s32 overlay_widescreen_active(void) {
    return (rc_current() == RC_OVERLAY) && widescreen_scissor_active();
}

static inline void append_intro_depth_clear_fallback(Gfx** dl) {
    if ((dl == NULL) || (*dl == NULL) || (gFrameContext == NULL)) {
        return;
    }

    gDPPipeSync((*dl)++);
    gDPSetCycleType((*dl)++, G_CYC_FILL);

    fill_native_depth_target(dl);
    set_framebuffer_color_image(dl);

    gDPPipeSync((*dl)++);
}

RECOMP_PATCH void init_sprite2d_render_zdepth(Gfx **dl, u8 color) {
    append_sp_load_ucode_ex_cmds(dl, &gspSprite2D_fifoTextStart, &gspSprite2D_fifoDataStart, 2048);
    gDPPipeSync((*dl)++);
    append_sprite2d_render_state(dl);
    append_sp_set_geometry_mode_cmd(dl, 0x00000001u);

    gDPSetPrimColor((*dl)++, 0, 0, color, color, color, color);
    append_dp_set_depth_source_prim_cmd(dl);
    append_dp_set_render_mode_aa_zb_tex_edge_cmd(dl);
    append_dp_set_texture_filter_average_cmd(dl);

    set_default_frame_scissor(dl);
}

RECOMP_PATCH void init_sprite2d_render(Gfx **dl, u8 r, u8 g, u8 b, u8 a) {
    gDPPipeSync((*dl)++);

    append_sp_load_ucode_ex_cmds(dl, &gspSprite2D_fifoTextStart, &gspSprite2D_fifoDataStart, 2048);
    gDPPipeSync((*dl)++);

    load_segments(dl, gDisplayListContext);

    append_sprite2d_render_state(dl);
    gDPSetPrimColor((*dl)++, 0, 0, r, g, b, a);
    append_dp_set_texture_filter_average_cmd(dl);

    set_default_frame_scissor(dl);
}

RECOMP_PATCH void black_out_screen(Gfx **dl) {
    s32 scissor_ulx;
    s32 scissor_uly;
    s32 scissor_lrx;
    s32 scissor_lry;

    if (widescreen_scissor_active()) {
        full_scissor_bounds(&scissor_ulx, &scissor_uly, &scissor_lrx, &scissor_lry);
        append_intro_depth_clear_fallback(dl);  // clears depth; restores color image to game FB

        const u32 black_packed = GPACK_RGBA5551(0, 0, 0, 1);
        gDPPipeSync((*dl)++);
        gDPSetCycleType((*dl)++, G_CYC_FILL);
        gDPSetFillColor((*dl)++, (black_packed << 16) | black_packed);
        gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, 0, 0, scissor_lrx, scissor_lry);
        gDPFillRectangle((*dl)++, 0, 0, scissor_lrx - 1, scissor_lry - 1);
        gDPPipeSync((*dl)++);

        gSPDisplayList((*dl)++, gPrimitiveFillRenderSetupDl);
        return;
    }

    scissor_ulx = SSSV_TV_SAFE_INSET;
    scissor_uly = SSSV_TV_SAFE_INSET;
    scissor_lrx = SSSV_BASE_WIDTH;
    scissor_lry = SSSV_BASE_HEIGHT;

    gSPDisplayList((*dl)++, gPrimitiveFillRenderSetupDl);

    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, scissor_ulx, scissor_uly, scissor_lrx, scissor_lry);
    gDPSetPrimColor((*dl)++, 0, 0, 0x00, 0x00, 0x00, 0xFF);
    gDPSetCombineMode((*dl)++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetRenderMode((*dl)++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPFillRectangle((*dl)++, 0, 0, scissor_lrx, scissor_lry);
}

static inline void append_intro_black_border_mask(Gfx** dl) {
    const s32 inset = SSSV_TV_SAFE_INSET;
    const s32 screen_w = SSSV_BASE_WIDTH;
    const s32 screen_h = SSSV_BASE_HEIGHT;
    const u32 black_packed = GPACK_RGBA5551(0, 0, 0, 1);

    if (!widescreen_scissor_active()) {
        return;
    }
    if ((screen_w <= (inset * 2)) || (screen_h <= (inset * 2))) {
        return;
    }

    gDPPipeSync((*dl)++);
    gDPSetCycleType((*dl)++, G_CYC_FILL);
    gDPSetFillColor((*dl)++, (black_packed << 16) | black_packed);
    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, 0, 0, screen_w, screen_h);

    gDPFillRectangle((*dl)++, 0, 0, screen_w - 1, inset - 1);
    gDPFillRectangle((*dl)++, 0, screen_h - inset, screen_w - 1, screen_h - 1);
    gDPFillRectangle((*dl)++, 0, 0, inset - 1, screen_h - 1);
    gDPFillRectangle((*dl)++, screen_w - inset, 0, screen_w - 1, screen_h - 1);

    gDPPipeSync((*dl)++);
    gSPDisplayList((*dl)++, gPrimitiveFillRenderSetupDl);
}

RECOMP_PATCH void draw_intro_logo_model(void) {
    ScissorRect full_rect;

    fill_full_scissor_rect(&full_rect);

    gDPSetScissor(gMainDL++, G_SC_NON_INTERLACE, full_rect.ulx, full_rect.uly, full_rect.lrx, full_rect.lry);
    gSPDisplayList(gMainDL++, gOpaqueLit3DRenderSetupDl);

    guTranslate(
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs],
        gIntroLogoPosX,
        gIntroLogoPosY,
        gIntroLogoPosZ
    );
    gSPMatrix(
        gMainDL++,
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs++],
        G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW
    );

    build_rotate_scale_translate_matrix(
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs],
        0,
        0,
        0,
        gIntroLogoRotX,
        gIntroLogoRotY,
        gIntroLogoRotZ,
        0x10000,
        0x10000,
        0x10000
    );
    gSPMatrix(
        gMainDL++,
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs++],
        G_MTX_NOPUSH | G_MTX_MUL | G_MTX_MODELVIEW
    );

    gSPDisplayList(gMainDL++, gIntroLogoTextureSetupDl);
    gSPDisplayList(gMainDL++, gIntroLogoModelDl);
    gSPPopMatrix(gMainDL++, G_MTX_MODELVIEW);
}

RECOMP_PATCH void render_intro_title_reveal(Gfx **dl, u16 reveal_step) {
    ScissorRect inset_rect;
    ScissorRect default_rect;
    ScissorRect reveal_rect;
    const s32 widescreen_overlay = overlay_widescreen_active();

    gIntroLogoPosX = gIntroCamEyeX;
    gIntroLogoPosY = gIntroCamEyeY;

    setup_intro_perspective_638558(&gMainDL);
    gSPDisplayList((*dl)++, gOpaqueLit3DRenderSetupDl);
    append_sp_num_lights_cmd(dl, 1);
    append_sp_light_cmd(dl, &gIntroLogoLight.l, 1);
    append_sp_light_cmd(dl, &gIntroLogoLight.a, 2);

    fill_safe_scissor_rect(&inset_rect);
    fill_intro_default_scissor_rect(widescreen_overlay, &default_rect);

    if (reveal_step < 0x28) {
        compute_intro_reveal_scissor_rect(reveal_step, &inset_rect, &reveal_rect);
        set_scissor_rect(dl, &reveal_rect);
    } else {
        set_scissor_rect(dl, &default_rect);
    }

    build_rotate_scale_translate_matrix(
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs],
        0,
        0,
        0,
        0,
        0,
        0,
        0x80000,
        0x80000,
        0x80000
    );

    gSPMatrix(
        (*dl)++,
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs++],
        G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW
    );

    append_dp_set_render_mode_zb_pcl_surf_cmd(dl);
    append_sp_clear_geometry_mode_cmd(dl, 0x00002000u);
    gSPDisplayList((*dl)++, gIntroRevealQuadDl);
    set_scissor_rect(dl, &default_rect);
}

RECOMP_PATCH void setup_intro_perspective_638558(Gfx** dl) {
    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x638558u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_INTRO_ID;

    guPerspective(&gDisplayListContext->unk37410, gIntroPerspNorm, 33.0f, get_target_aspect(), 100.0f, 15000.0f, 1.0f);

    gSPPerspNormalize((*dl)++, gIntroPerspNorm[0]);

    guScale(&gDisplayListContext->unk37450, gIntroScaleX, gIntroScaleY, gIntroScaleZ);
    guScale(&gDisplayListContext->unk374D0, gIntroScaleX, gIntroScaleY, gIntroScaleZ);
    
    guLookAt(
        &gDisplayListContext->unk37490,
        gIntroCamEyeX,
        gIntroCamEyeY,
        gIntroCamEyeZ,
        0.0f,
        0.0f,
        0.0f,
        gIntroCamUpX,
        gIntroCamUpY,
        gIntroCamUpZ
    );

    gSPMatrix((*dl)++, &gDisplayListContext->unk37410, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix((*dl)++, &gDisplayListContext->unk37450, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPMatrix((*dl)++, &gDisplayListContext->unk37490, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPMatrix((*dl)++, &gDisplayListContext->unk374D0, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    trace_proj_view_snapshot_once(RC_OVERLAY);
    rt64_tag_projection_matrix(dl, (u32)cur_perspective_projection_transform_id);
}

RECOMP_PATCH void render_intro_overlay_frame_63BF88(void) {
    const s16 target_width = compute_target_width_for_height_patch(SSSV_BASE_HEIGHT);

    rc_begin_frame();
    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x63BF88u);

    gScreenWidth = target_width;
    gScreenHeight = SSSV_BASE_HEIGHT;
    load_segments(&gMainDL, gDisplayListContext);
    clear_depth_buffer(&gMainDL);
    write_main_viewport_expand(gScreenWidth, gScreenHeight);

    gSPViewport(gMainDL++, &gMainViewport);
    set_framebuffer_color_image(&gMainDL);
    gDPPipeSync(gMainDL++);

    append_sp_texture_cmd(gMainDL++, 0x8000, 0x8000, 0, 0, 1); // enable texturing

    gDisplayListContext->usedSprites = 0;
    gDisplayListContext->usedModelViewMtxs = 0;
    gDisplayListContext->unk39310 = 0;
    gDisplayListContext->usedHilites = 0;

    render_title_screen_frame(gFrameContext);
    append_intro_black_border_mask(&gMainDL);
    if (gIntroTransitionPending != 0) {
        gCurrentOverlay = 0;
        gOverlayState = 4;
    }

    g_dbg_intro_frame_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_intro_frame_calls, 8, 120)) {
        PATCH_TRACE(PATCH_TAG_INTRO_FRAME, g_dbg_intro_frame_calls, (u16)target_width, (u16)gScreenWidth);
    }
}

RECOMP_PATCH void end_display_lists(void) {
    const s16 safe_height = get_safe_screen_height();
    const s16 target_width = compute_target_width_for_height_patch(safe_height);

    if (target_width > SSSV_BASE_WIDTH) {
        write_main_viewport_expand(target_width, safe_height);
        if (gScreenWidth < target_width) {
            gScreenWidth = target_width;
        }
    }

    append_sp_texture_cmd(gMainDL++, 0, 0, 0, 0, 0);

    append_enddl_cmd(gLayer0DL++);
    append_enddl_cmd(gLayer1DL++);
    append_enddl_cmd(gOpaqueDL++);
    append_enddl_cmd(gXluDL++);
    append_enddl_cmd(gAuxDL++);

    append_fullsync_cmd(gMainDL++);
    append_enddl_cmd(gMainDL++);

    g_dbg_end_display_lists_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_end_display_lists_calls, 10, 240)) {
        PATCH_TRACE(PATCH_TAG_END_DISPLAY_LISTS, g_dbg_end_display_lists_calls, (u32)rc_current(), (u16)target_width);
        PATCH_TRACE(PATCH_TAG_END_DISPLAY_LISTS_VP, (u16)gMainViewport.vscale[0], (u16)gMainViewport.vtrans[0], (u16)gScreenWidth);
    }

    rc_end_frame();
}

RECOMP_PATCH void setup_world_perspective_6AB090(DisplayList* arg0) {
    const f32 target_aspect = get_target_aspect();
    const s16 target_width = compute_target_width_for_height_patch((gScreenHeight > 0) ? gScreenHeight : SSSV_BASE_HEIGHT);

    rc_set(RC_WORLD3D);
    rc_mark_world_projection_active();
    cur_perspective_projection_transform_id = SSSV_PROJECTION_WORLD_TRANSFORM_ID;
    rc_update_camera_cut_skip();
    rc_ensure_rt64_extended_enabled(&gMainDL);
    rc_ensure_rt64_extended_enabled(&gLayer0DL);
    rc_ensure_rt64_extended_enabled(&gAuxDL);

    if (target_width > SSSV_BASE_WIDTH) {
        append_world_depth_clear_failsafe();
    }

    guPerspective(
        &arg0->unk37410,
        &gWorldPerspNorm,
        gLevelConfig.fovY,
        target_aspect,
        gLevelConfig.unkC,
        gLevelConfig.unkE,
        1.0f
    );
    guScale(&arg0->unk37450, 0.5f, 0.5f, 0.5f);
    guScale(&arg0->unk374D0, 1.0f, 1.0f, 1.0f);
    update_world_camera_transform();

    g_dbg_world_perspective_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_world_perspective_calls, 6, 240)) {
        const s32 aspect_x1000 = (s32)(target_aspect * 1000.0f + 0.5f);
        PATCH_TRACE(PATCH_TAG_WORLD_PERSPECTIVE, g_dbg_world_perspective_calls, (u32)aspect_x1000, (u16)target_width);
    }
}

RECOMP_PATCH void setup_frame_render_state(DisplayList* arg0, Gfx** arg1) {
    const RenderContext ctx = rc_current();

    if (ctx == RC_WORLD3D) {
        const s16 vp_height = get_safe_screen_height();
        s16 vp_width = compute_target_width_for_height_patch(vp_height);
        if (vp_width < SSSV_BASE_WIDTH) {
            vp_width = SSSV_BASE_WIDTH;
        }

        write_main_viewport_expand(vp_width, vp_height);
        gSPViewport((*arg1)++, &gMainViewport);
    }

    gSPMatrix((*arg1)++, &arg0->unk37410, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix((*arg1)++, &arg0->unk37450, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPMatrix((*arg1)++, &arg0->unk37490, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPMatrix((*arg1)++, &arg0->unk374D0, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPPerspNormalize((*arg1)++, gWorldPerspNorm);

    trace_proj_view_snapshot_once(ctx);

    if (cur_perspective_projection_transform_id != 0) {
        rt64_tag_projection_matrix(arg1, (u32)cur_perspective_projection_transform_id);
    }
}
