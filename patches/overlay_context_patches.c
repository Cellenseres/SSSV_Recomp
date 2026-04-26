#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_rt64_tagging.h"
#include "sssv_patch_ui_domain.h"
#include "sssv_patch_display_policy.h"

#ifndef G_MW_FOG
#define G_MW_FOG 0x08u
#endif

#ifndef G_SETFOGCOLOR
#define G_SETFOGCOLOR 0xF8u
#endif

#define FONT_COMIC_SANS_PATCH 2
#define SFX_UNKNOWN_21_PATCH 21
#define SFX_UNKNOWN_132_PATCH 132
#define SFX_UNKNOWN_133_PATCH 133
#define SFX_UNKNOWN_134_PATCH 134
#define SFX_UNKNOWN_135_PATCH 135
#define TERMINAL_SCENE_DL_OFFSET 0x9600u

#define SIN_PATCH(x) gSineTable256[((s16)(x)) & 0xFF]
#define COS_PATCH(x) gSineTable256[(((s16)(x)) + 0x40) & 0xFF]
#define SSSV_RAND_PATCH(x) (advance_random_seed() & ((x) - 1))

static inline void patch_write_main_viewport_expand(s16 viewport_width, s16 viewport_height) {
    gMainViewport.vscale[0] = (s16)(viewport_width * 2);
    gMainViewport.vscale[1] = (s16)(viewport_height * 2);
    gMainViewport.vtrans[0] = (s16)(SSSV_BASE_WIDTH * 2);
    gMainViewport.vtrans[1] = (s16)(viewport_height * 2);
}

static inline void patch_write_overlay_viewport_expand(s16 viewport_width, s16 viewport_height) {
    gOverlayViewport.vp.vscale[0] = (s16)(viewport_width * 2);
    gOverlayViewport.vp.vscale[1] = (s16)(viewport_height * 2);
    gOverlayViewport.vp.vtrans[0] = (s16)(SSSV_BASE_WIDTH * 2);
    gOverlayViewport.vp.vtrans[1] = (s16)(viewport_height * 2);
}

static inline void patch_apply_safe_scissor(Gfx** dl) {
    s32 scissor_ulx, scissor_uly, scissor_lrx, scissor_lry;

    display_policy_safe_scissor_bounds(&scissor_ulx, &scissor_uly, &scissor_lrx, &scissor_lry);
    gDPSetScissor((*dl)++, G_SC_NON_INTERLACE, scissor_ulx, scissor_uly, scissor_lrx, scissor_lry);
}

static inline s32 patch_terminal_slice_scissor(s16 slice_y, s32* ulx, s32* uly, s32* lrx, s32* lry) {
    const s16 inset = display_policy_safe_inset();

    display_policy_safe_scissor_bounds(ulx, uly, lrx, lry);
    *lry = MIN(*lry, (s32)(gScreenHeight - slice_y - 1) - inset);
    return *lry > *uly;
}

static inline void patch_gSPFogFactor(Gfx* pkt, s16 fm, s16 fo) {
    pkt->words.w0 = _SHIFTL(G_MOVEWORD, 24, 8) | _SHIFTL(0, 8, 16) | _SHIFTL(G_MW_FOG, 0, 8);
    pkt->words.w1 = _SHIFTL((u16)fm, 16, 16) | _SHIFTL((u16)fo, 0, 16);
}

static inline void patch_gDPSetFogColor(Gfx* pkt, u8 r, u8 g, u8 b, u8 a) {
    pkt->words.w0 = _SHIFTL(G_SETFOGCOLOR, 24, 8);
    pkt->words.w1 = _SHIFTL(r, 24, 8) | _SHIFTL(g, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8);
}

static inline Gfx* patch_display_list_offset(DisplayList* ctx, u32 byte_offset) {
    return (Gfx*)((u8*)ctx + byte_offset);
}

static inline void patch_clear_overlay_color(Gfx** dl) {
    draw_rectangle(dl, 0, 0, SSSV_BASE_WIDTH, SSSV_BASE_HEIGHT, 0, 0, 0, 0xFF);
}

RECOMP_PATCH void setup_pause_menu_perspective_a_7A6B30(void) {
    s32 scissor_ulx, scissor_uly, scissor_lrx, scissor_lry;

    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x7A6B30u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_PAUSE_A_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_PAUSE_A_ID;

    const s16 target_vp_w = compute_target_width_for_height_patch(get_safe_screen_height());
    const s16 target_vp_h = get_safe_screen_height();
    patch_write_overlay_viewport_expand(target_vp_w, target_vp_h);

    gDPPipeSync(gMainDL++);
    
    init_f3dex_render(&gMainDL, gDisplayListContext);
    gDPPipeSync(gMainDL++);

    gSPSegment(gMainDL++, 0x04, (const void*)(uintptr_t)osVirtualToPhysical(gMenuSegmentBase));
    gSPViewport(gMainDL++, &gOverlayViewport);
    clear_depth_buffer(&gMainDL);
    gDPSetColorImage(gMainDL++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, (const void*)(uintptr_t)osVirtualToPhysical(gFrameContextPtr->framebuffer));
    patch_clear_overlay_color(&gMainDL);

    guPerspective(&gDisplayListContext->unk37410, &gWorldPerspNorm, 45.0f, get_target_aspect(), 2.0f, 2000.0f, 1.0f);
    guScale(&gDisplayListContext->unk37450, 0.5f, 0.5f, 0.5f);
    guScale(&gDisplayListContext->unk374D0, 1.0f, 1.0f, 1.0f);
    guLookAt(
        &gDisplayListContext->unk37490,
        gPauseMenuLookAt.unk0,
        (gOverlayUiState.unk48 / 700.0f) + gPauseMenuLookAt.unk4,
        gPauseMenuLookAt.unk8,
        gPauseMenuLookAt.unkC,
        gPauseMenuLookAt.unk10,
        gPauseMenuLookAt.unk14,
        0.0f,
        0.0f,
        1.0f
    );
    setup_frame_render_state(gDisplayListContext, &gMainDL);

    gSPDisplayList(gMainDL++, gOverlay3DRenderSetupDl);
    display_policy_safe_scissor_bounds(&scissor_ulx, &scissor_uly, &scissor_lrx, &scissor_lry);
    gDPSetScissor(gMainDL++, G_SC_NON_INTERLACE, scissor_ulx, scissor_uly, scissor_lrx, scissor_lry);
    rt64_tag_projection_matrix(&gMainDL, (u32)cur_perspective_projection_transform_id);
}

RECOMP_PATCH void setup_pause_menu_perspective_b_7A6F04(void) {
    s32 scissor_ulx, scissor_uly, scissor_lrx, scissor_lry;

    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x7A6F04u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_PAUSE_B_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_PAUSE_B_ID;

    const s16 target_vp_w = compute_target_width_for_height_patch(get_safe_screen_height());
    const s16 target_vp_h = get_safe_screen_height();
    patch_write_overlay_viewport_expand(target_vp_w, target_vp_h);

    gDPPipeSync(gMainDL++);
    
    init_f3dex_render(&gMainDL, gDisplayListContext);
    gDPPipeSync(gMainDL++);

    gSPSegment(gMainDL++, 0x04, (const void*)(uintptr_t)osVirtualToPhysical(gMenuSegmentBase));
    gSPViewport(gMainDL++, &gOverlayViewport);
    gDPSetColorImage(gMainDL++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, (const void*)(uintptr_t)osVirtualToPhysical(gFrameContextPtr->framebuffer));
    patch_clear_overlay_color(&gMainDL);

    guPerspective(&gDisplayListContext->unk37410, &gWorldPerspNorm, 45.0f, get_target_aspect(), 2.0f, 6000.0f, 1.0f);
    guScale(&gDisplayListContext->unk37450, 0.5f, 0.5f, 0.5f);
    guScale(&gDisplayListContext->unk374D0, 1.0f, 1.0f, 1.0f);
    guLookAt(
        &gDisplayListContext->unk37490,
        gPauseMenuLookAt.unk0,
        gPauseMenuLookAt.unk4,
        gPauseMenuLookAt.unk8,
        gPauseMenuLookAt.unkC,
        gPauseMenuLookAt.unk10,
        gPauseMenuLookAt.unk14,
        0.0f,
        0.0f,
        1.0f
    );
    setup_frame_render_state(gDisplayListContext, &gMainDL);

    gSPDisplayList(gMainDL++, gOverlay3DRenderSetupDl);
    display_policy_safe_scissor_bounds(&scissor_ulx, &scissor_uly, &scissor_lrx, &scissor_lry);
    gDPSetScissor(gMainDL++, G_SC_NON_INTERLACE, scissor_ulx, scissor_uly, scissor_lrx, scissor_lry);
    rt64_tag_projection_matrix(&gMainDL, (u32)cur_perspective_projection_transform_id);
}

RECOMP_PATCH void render_terminal_background_scene(void) {
    const s16 target_vp_h = get_safe_screen_height();
    const s16 target_vp_w = compute_target_width_for_height_patch(target_vp_h);
    s32 scissor_ulx, scissor_uly, scissor_lrx, scissor_lry;

    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x79F120u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_TERMINAL_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_TERMINAL_ID;

    switch_to_current_segment(&gMainDL, gDisplayListContext);

    patch_write_main_viewport_expand(target_vp_w, target_vp_h);
    guPerspective(
        &gDisplayListContext->unk37410,
        &gWorldPerspNorm,
        gLevelConfig.fovY,
        get_target_aspect(),
        gLevelConfig.unkC,
        gLevelConfig.unkE,
        1.0f
    );
    guScale(&gDisplayListContext->unk37450, 0.5f, 0.5f, 0.5f);
    guScale(&gDisplayListContext->unk374D0, 1.0f, 1.0f, 1.0f);
    update_world_camera_transform();

    gSPViewport(gMainDL++, &gMainViewport);
    setup_frame_render_state(gDisplayListContext, &gMainDL);

    gDPSetColorImage(
        gMainDL++,
        G_IM_FMT_RGBA,
        G_IM_SIZ_16b,
        320,
        (const void*)(uintptr_t)osVirtualToPhysical(gFrameContextPtr->framebuffer)
    );
    display_policy_safe_scissor_bounds(&scissor_ulx, &scissor_uly, &scissor_lrx, &scissor_lry);
    gDPSetScissor(gMainDL++, G_SC_NON_INTERLACE, scissor_ulx, scissor_uly, scissor_lrx, scissor_lry);
    gSPDisplayList(gMainDL++, gOverlay3DRenderSetupDl);
    gSPDisplayList(gMainDL++, patch_display_list_offset(gDisplayListContext, TERMINAL_SCENE_DL_OFFSET));
}

RECOMP_PATCH void render_terminal_background_dna_79FBB4(u16 arg0) {
    u16 norm;
    Mtx* model_mtx = &gDisplayListContext->unk37410;
    u32 transform_id_a;
    u32 transform_id_b;
    static s16 terminal_dna_roll = 0;

    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x79FBB4u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_TERMINAL_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_TERMINAL_ID;

    guPerspective(&model_mtx[gDisplayListContext->usedModelViewMtxs], &norm, 33.0f, get_target_aspect(), 100.0f, 25000.0f, 1.0f);
    gSPMatrix(gOpaqueDL++, &model_mtx[gDisplayListContext->usedModelViewMtxs++], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPPerspNormalize(gOpaqueDL++, norm);

    guLookAt(
        &model_mtx[gDisplayListContext->usedModelViewMtxs],
        7000.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        (SIN_PATCH(terminal_dna_roll) >> 7) / 3000.0f,
        (COS_PATCH(terminal_dna_roll) >> 7) / 3000.0f,
        1.0f
    );
    gSPMatrix(gOpaqueDL++, &model_mtx[gDisplayListContext->usedModelViewMtxs++], G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);

    guScale(&model_mtx[gDisplayListContext->usedModelViewMtxs], 1.0f, 1.0f, 1.0f);
    gSPMatrix(gOpaqueDL++, &model_mtx[gDisplayListContext->usedModelViewMtxs++], G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

    rt64_tag_projection_matrix(&gOpaqueDL, (u32)cur_perspective_projection_transform_id);

    transform_id_a = rc_next_overlay_transform_id();
    rt64_tag_model_matrix(&gOpaqueDL, transform_id_a, 0);
    gSPDisplayList(gOpaqueDL++, gTerminalDnaOpaqueRenderSetupDl);
    rt64_pop_model_matrix(&gOpaqueDL);

    terminal_dna_roll = (terminal_dna_roll + 1) & 0xFF;

    transform_id_b = rc_next_overlay_transform_id();
    rt64_tag_model_matrix(&gOpaqueDL, transform_id_b, 0);
    gSPDisplayList(gOpaqueDL++, gTerminalDnaInterlaceRenderSetupDl);
    rt64_pop_model_matrix(&gOpaqueDL);

    spin_dna_helixes(arg0);
}

RECOMP_PATCH void render_terminal_stat_text(s32 arg0, s32 arg1) {
    s16 leading_chars;
    s16 value_chars;
    s16 i;
    s16 vertical_offset;

    (void)arg0;
    (void)arg1;

    if (gTerminalTextScrollState.unk82 == 0) {
        if (gTerminalTextScrollState.unk0[0] == 0) {
            leading_chars = 0;
        } else {
            leading_chars = get_raw_message_length(gTerminalTextScrollState.unk0[0]);
        }

        if (gTerminalTextScrollState.unk34[0] == 0) {
            value_chars = 0;
        } else {
            value_chars = get_raw_message_length(gTerminalTextScrollState.unk34[0]);
        }

        if (gTerminalTextScrollState.unk84 >= (leading_chars + value_chars + 3)) {
            gTerminalTextScrollState.unk82 = 1;
            gTerminalTextScrollState.unk84 = 0;
            func_8032C2D0_73D980(SFX_UNKNOWN_21_PATCH, 0x3800, 1.0f);
        } else {
            if (gTerminalTextScrollState.unk34[0] != 0) {
                gTerminalTextScrollState.unk68[0] = terminal_right_column_x_for_text(gTerminalTextScrollState.unk34[0]);
            }

            if ((gTerminalTextScrollState.unk84 < leading_chars) || ((leading_chars + 3) < gTerminalTextScrollState.unk84)) {
                func_8032CD20_73E3D0(0x17, 0x36, 0x1AAA, 0, 0.5f);
            }

            gTerminalTextScrollState.unk84++;
        }
    } else {
        gTerminalTextScrollState.unk82 += 2;
        if (gTerminalTextScrollState.unk82 >= 16) {
            gTerminalTextScrollState.unk82 = 0;

            for (i = 11; i >= 0; i--) {
                gTerminalTextScrollState.unk0[i + 1] = gTerminalTextScrollState.unk0[i];
                gTerminalTextScrollState.unk34[i + 1] = gTerminalTextScrollState.unk34[i];
                gTerminalTextScrollState.unk68[i + 1] = gTerminalTextScrollState.unk68[i];
            }

            gTerminalTextScrollState.unk0[0] = gTerminalStatLabels[gTerminalTextScrollState.unk86];
            gTerminalTextScrollState.unk34[0] = gTerminalStatText[gTerminalTextScrollState.unk86];
            gTerminalTextScrollState.unk68[0] = gTerminalStatTextX[gTerminalTextScrollState.unk86];

            gTerminalTextScrollState.unk86 = (gTerminalTextScrollState.unk86 + 1) % 17;
        }
    }

    rc_push(RC_HUD_ORTHO);
    rc_note_texrect_context(TRC_HUD_UI, 0x79F290u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
    rt64_tag_projection_matrix(&gMainDL, SSSV_PROJECTION_HUD_TRANSFORM_ID);

    vertical_offset = (s16)(210 - gTerminalTextScrollState.unk82);

    if (gTerminalTextScrollState.unk82 == 0) {
        leading_chars = (gTerminalTextScrollState.unk0[0] != 0) ? get_raw_message_length(gTerminalTextScrollState.unk0[0]) : 0;

        if (gTerminalTextScrollState.unk0[0] != 0) {
            func_8012D374(&gMainDL, gTerminalTextScrollState.unk0[0], 25, vertical_offset, 14.0f, 16.0f, gTerminalTextScrollState.unk84);
        }

        if ((gTerminalTextScrollState.unk34[0] != 0) && ((leading_chars + 3) < gTerminalTextScrollState.unk84)) {
            func_8012D374(
                &gMainDL,
                gTerminalTextScrollState.unk34[0],
                gTerminalTextScrollState.unk68[0],
                vertical_offset,
                14.0f,
                16.0f,
                (s16)((gTerminalTextScrollState.unk84 - leading_chars) - 3)
            );
        }
    }

    for (i = 0; i < 13; i++) {
        if ((i != 0) || (gTerminalTextScrollState.unk82 != 0)) {
            if (gTerminalTextScrollState.unk0[i] != 0) {
                func_8012D374(&gMainDL, gTerminalTextScrollState.unk0[i], 25, vertical_offset, 14.0f, 16.0f, -1);
            }

            if (gTerminalTextScrollState.unk34[i] != 0) {
                func_8012D374(&gMainDL, gTerminalTextScrollState.unk34[i], gTerminalTextScrollState.unk68[i], vertical_offset, 14.0f, 16.0f, -1);
            }
        }

        vertical_offset -= 15;
    }

    rc_pop();
}

RECOMP_PATCH void render_terminal_background_frame(void) {
    s32 scissor_ulx, scissor_uly, scissor_lrx, scissor_lry;
    s32 transition_mask;
    s16 i;
    s16 random_offset;
    s16 slice_y;

    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x7A00A8u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_TERMINAL_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_TERMINAL_ID;

    load_segments(&gMainDL, gDisplayListContext);
    switch_to_current_segment(&gMainDL, gDisplayListContext);

    patch_write_main_viewport_expand(
        compute_target_width_for_height_patch(get_safe_screen_height()),
        get_safe_screen_height()
    );
    gSPViewport(gMainDL++, &gMainViewport);
    clear_depth_buffer(&gMainDL);

    gDPSetColorImage(
        gMainDL++,
        G_IM_FMT_RGBA,
        G_IM_SIZ_16b,
        320,
        (const void*)(uintptr_t)osVirtualToPhysical(gFrameContextPtr->framebuffer)
    );
    patch_apply_safe_scissor(&gMainDL);
    patch_gSPFogFactor(gMainDL++, -3072, -22016);
    patch_gDPSetFogColor(gMainDL++, 0x00, 0x00, 0x00, 0x00);

    if (gTerminalFadeStep == 0) {
        start_sfx_volume_fade(1.0f, 0.0f, 20.0f);
        D_803F6470 = 0;
        D_803F646C = 0.0f;
        if (D_80291090.hasRumblePak[0] != 0) {
            func_80137168();
            func_8013724C(0);
        }
    }

    if (D_803F6470 < 100) {
        D_803F6470 += 1;
    }
    if ((D_803F6470 == 5) && (gCameraUiState == 3)) {
        play_sound_effect(SFX_UNKNOWN_135_PATCH, 0, 0x5000, 1.0f, 0x40);
    }
    if (gTerminalFadeStep < 2) {
        draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, 0xFF);
        gTerminalFadeStep++;
    }

    func_8032CD20_73E3D0(0x45, SFX_UNKNOWN_132_PATCH, 6144.0f * D_803F646C, 0, 1.0f);
    func_8032CD20_73E3D0(0xA9, SFX_UNKNOWN_133_PATCH, 4352.0f * D_803F646C, 0, 0.7f);
    func_8032CD20_73E3D0(0x171, SFX_UNKNOWN_133_PATCH, 4352.0f * D_803F646C, 0, 1.0f);
    func_8032CD20_73E3D0(0x10D, SFX_UNKNOWN_134_PATCH, 21760.0f * D_803F646C, 0, 1.0f);

    if (D_803F646C < 1.0f) {
        D_803F646C += 0.016f;
    }

    gScreenHeight = SSSV_BASE_HEIGHT;
    D_803A6CC4_7B8374 = ((SIN_PATCH(D_803F6472 * 2) >> 7) / 3200.0f) + 0.7f;
    D_803A6CC8_7B8378 = ((SIN_PATCH(D_803F6472) >> 7) / 15.0f) + 45.0f;
    D_803F6472++;

    patch_write_main_viewport_expand(
        compute_target_width_for_height_patch(get_safe_screen_height()),
        get_safe_screen_height()
    );

    func_8038CF90_79E640();
    if (gTerminalPhase == 0) {
        draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, 75);
    } else {
        draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, 100);
    }

    switch (gTerminalPhase) {
        case 0:
            if (gScreenWidth < SSSV_BASE_WIDTH) {
                gScreenWidth += 2;
            }
            update_terminal_scene_lighting(0xFF);
            render_terminal_background_scene();

            if (gTerminalFrameCounter++ > 40) {
                gTerminalTransitionCounter = 1;
                gTerminalFrameCounter = 0;
                gTerminalPhase = 1;
            }
            break;

        case 1:
        {
            const s16 terminal_frame_width = terminal_ui_width();

            gScreenWidth = terminal_frame_width;
            load_default_display_list(&gMainDL);
            patch_clear_overlay_color(&gMainDL);
            set_menu_text_color(0xFF, 0xFF, 0xFF, 0xFF);
            select_font(0, FONT_COMIC_SANS_PATCH, 1, 0);
            update_terminal_scene_lighting(0xFF);

            random_offset = SSSV_RAND_PATCH(8);
            for (i = 0; i < 6; i++) {
                slice_y = (s16)(i * 40 + random_offset);

                gDPSetColorImage(
                    gMainDL++,
                    G_IM_FMT_RGBA,
                    G_IM_SIZ_16b,
                    320,
                    (const void*)(uintptr_t)osVirtualToPhysical(
                        gFrameContextPtr->framebuffer + ((((s32)slice_y) * 10) << 6)
                    )
                );

                if (patch_terminal_slice_scissor(slice_y, &scissor_ulx, &scissor_uly, &scissor_lrx, &scissor_lry)) {
                    gDPSetScissor(gMainDL++, G_SC_NON_INTERLACE, scissor_ulx, scissor_uly, scissor_lrx, scissor_lry);
                    gSPDisplayList(gMainDL++, gLayer0DL);
                }
            }

            gDPSetColorImage(
                gMainDL++,
                G_IM_FMT_RGBA,
                G_IM_SIZ_16b,
                320,
                (const void*)(uintptr_t)osVirtualToPhysical(gFrameContextPtr->framebuffer)
            );
            patch_apply_safe_scissor(&gMainDL);

            render_terminal_background_glyphs(&gLayer0DL, gTerminalFrameCounter);
            render_terminal_background_scene();
            render_terminal_background_dna_79FBB4(gTerminalFrameCounter);
            load_default_display_list(&gMainDL);
            set_menu_text_color(0x80, 0xFF, 0x00, 0xFF);
            select_font(0, FONT_COMIC_SANS_PATCH, 1, 0);
            rc_ensure_rt64_extended_enabled(&gMainDL);
            render_terminal_stat_text(0xE, 0x10);

            if (gTerminalFrameCounter++ > 65000) {
                gTerminalFrameCounter = 200;
            }

            if (((gControllerInput->button & CONT_A) != 0) || ((gControllerInput->button & CONT_B) != 0)) {
                if (gTerminalTransitionCounter == 0) {
                    start_sequence_volume_fade(0, 25.0f, 0.0f, 20.0f);
                    gTerminalFadeStep = 0;
                    gTerminalTransitionCounter = 1;
                    gTerminalFrameCounter = 0;
                    gUiFlowState.unk0 = 0;
                    D_803F2C6C = 0;
                    D_803F2C6D = 0;
                    gTerminalPhase = 0;
                    D_803F6460 = 100;
                    gControllerDebounce = 18;
                    draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, 0xFF);

                    if (gCameraUiState == 4) {
                        gCameraUiState = 2;
                        D_803F6468 = 0;
                        D_803F2AA3 = 60;
                    } else {
                        gCameraUiState = 0;
                    }
                }
            }
            break;
        }

        case 2:
            update_terminal_scene_lighting(0xFF);
            render_terminal_background_scene();
            if (gTerminalFrameCounter == 1) {
                func_802F2EEC_70459C(80, 80, 80, 200, 200, 200, 10);
            }
            gTerminalFrameCounter += 1;
            if (gTerminalFrameCounter == 40) {
                gTerminalFrameCounter = 0;
                gCameraUiState = 0;
                gUiFlowState.unk0 = 0;
                D_803F2C6C = 0;
                D_803F2C6D = 0;
                gTerminalPhase = 0;
            }
            break;
    }

    if (gTerminalTransitionCounter != 0) {
        transition_mask = gTerminalTransitionCounter ^ 7;
        gTerminalTransitionCounter = gTerminalTransitionCounter + 1;
        if (transition_mask == 0) {
            gTerminalTransitionCounter = 0;
        }
    }
}
