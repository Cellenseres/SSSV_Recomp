#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_rt64_tagging.h"
#include "sssv_patch_ui_domain.h"
#include "sssv_patch_display_policy.h"

RECOMP_PATCH void setup_pause_menu_perspective_a_7A6B30(void) {
    s32 scissor_ulx, scissor_uly, scissor_lrx, scissor_lry;

    rc_set(RC_OVERLAY);
    rc_note_texrect_context(TRC_OVERLAY, 0x7A6B30u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_OVERLAY_PAUSE_A_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_OVERLAY_PAUSE_A_ID;

    const s16 target_vp_w = compute_target_width_for_height_patch(get_safe_screen_height());
    const s16 target_vp_h = get_safe_screen_height();
    gOverlayViewport.vp.vscale[0] = target_vp_w * 2;
    gOverlayViewport.vp.vscale[1] = target_vp_h * 2;
    gOverlayViewport.vp.vtrans[0] = target_vp_w * 2;
    gOverlayViewport.vp.vtrans[1] = target_vp_h * 2;

    gDPPipeSync(gMainDL++);
    
    init_f3dex_render(&gMainDL, gDisplayListContext);
    gDPPipeSync(gMainDL++);

    gSPSegment(gMainDL++, 0x04, (const void*)(uintptr_t)osVirtualToPhysical(gMenuSegmentBase));
    gSPViewport(gMainDL++, &gOverlayViewport);
    clear_depth_buffer(&gMainDL);
    gDPSetColorImage(gMainDL++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, (const void*)(uintptr_t)osVirtualToPhysical(gFrameContext->framebuffer));

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
    gOverlayViewport.vp.vscale[0] = target_vp_w * 2;
    gOverlayViewport.vp.vscale[1] = target_vp_h * 2;
    gOverlayViewport.vp.vtrans[0] = target_vp_w * 2;
    gOverlayViewport.vp.vtrans[1] = target_vp_h * 2;

    gDPPipeSync(gMainDL++);
    
    init_f3dex_render(&gMainDL, gDisplayListContext);
    gDPPipeSync(gMainDL++);

    gSPSegment(gMainDL++, 0x04, (const void*)(uintptr_t)osVirtualToPhysical(gMenuSegmentBase));
    gSPViewport(gMainDL++, &gOverlayViewport);

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

RECOMP_PATCH void render_terminal_background_dna_79FBB4(u16 arg0) {
    u16 norm;
    Mtx* model_mtx = &gDisplayListContext->unk37410;
    u32 transform_id_a;
    u32 transform_id_b;

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
        0.0f,
        0.0f,
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

    transform_id_b = rc_next_overlay_transform_id();
    rt64_tag_model_matrix(&gOpaqueDL, transform_id_b, 0);
    gSPDisplayList(gOpaqueDL++, gTerminalDnaInterlaceRenderSetupDl);
    rt64_pop_model_matrix(&gOpaqueDL);

    spin_dna_helixes(arg0);
}
