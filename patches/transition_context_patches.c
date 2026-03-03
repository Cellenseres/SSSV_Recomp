#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_rt64_tagging.h"
#include "sssv_patch_ui_domain.h"

static u8 s_transition_r = 0;
static u8 s_transition_g = 0;
static u8 s_transition_b = 0;

RECOMP_PATCH void perform_screen_transition(void) {
    s32 phi_v1;
    s32 alpha;
    s8 size;
    u16 i;
    u16 j;
    const s16 transition_width = ui_safe_width();
    const s16 transition_height = get_safe_screen_height();

    rc_push(RC_TRANSITION);
    rc_note_texrect_context(TRC_TRANSITION, 0x7027A8u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_TRANSITION_TRANSFORM_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_TRANSITION_TRANSFORM_ID;
    rt64_tag_projection_matrix(&gMainDL, SSSV_PROJECTION_TRANSITION_TRANSFORM_ID);

    alpha = 0xFF;
    render_screen_transition_tv_702A68();

    if ((gScreenTransition.unk0 == 2) || (gScreenTransition.unk0 == 1)) {
        switch (gScreenTransition.transitionId) {
            case 0:
                if (gScreenTransition.unk6 < 240) {
                    draw_rectangle(&gMainDL, 0, gScreenTransition.unk6, 320, 240, 0, 0, 0, alpha);
                }
                if (gScreenTransition.unk6 < 240) {
                    gScreenTransition.unk6 += 6;
                }
                if (gScreenTransition.unk2 < 100) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk6 >= 240) {
                    gScreenTransition.unk0 = 1;
                }
                break;
            case 1:
                draw_rectangle(&gMainDL, 0, 0, 320, gScreenTransition.unk6 + 1, 0, 0, 0, alpha);
                if (gScreenTransition.unk6 < 240) {
                    gScreenTransition.unk6 += 6;
                }
                if (gScreenTransition.unk2 < 100) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk6 >= 240) {
                    gScreenTransition.unk0 = 1;
                }
                break;
            case 2:
                phi_v1 = 100 - (gScreenTransition.unk2 * 10);
                if (phi_v1 > 89) {
                    phi_v1 = 89;
                }
                alpha = gTrigLut.unk2D0[(s16)phi_v1];
                draw_rectangle(&gMainDL, 0, 0, 320, 240, s_transition_r, s_transition_g, s_transition_b, alpha);
                if (gScreenTransition.unk2 < 100) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk2 > 9) {
                    gScreenTransition.unk0 = 0;
                }
                break;
            case 3:
            case 14:
            case 16:
            case 17:
            case 18:
                if ((gScreenTransition.transitionId == 3) || (gScreenTransition.transitionId == 14)) {
                    s_transition_r = 0;
                    s_transition_g = 0;
                    s_transition_b = 0;
                }
                if (gScreenTransition.transitionId == 16) {
                    s_transition_r = 255;
                    s_transition_g = 0;
                    s_transition_b = 0;
                }
                if (gScreenTransition.transitionId == 17) {
                    s_transition_r = 0;
                    s_transition_g = 255;
                    s_transition_b = 0;
                }
                if (gScreenTransition.transitionId == 18) {
                    s_transition_r = 0;
                    s_transition_g = 0;
                    s_transition_b = 255;
                }

                if (gScreenTransition.unk2 < 11) {
                    phi_v1 = (gScreenTransition.unk2 - 1) * 10;
                    if (phi_v1 > 89) {
                        phi_v1 = 89;
                    }
                    alpha = gTrigLut.unk2D0[(s16)(89 - phi_v1)];
                    alpha = 255 - alpha;
                } else {
                    alpha = 255;
                    gScreenTransition.unk0 = 1;
                }

                draw_rectangle(&gMainDL, 0, 0, 320, 240, s_transition_r, s_transition_g, s_transition_b, alpha);
                if (gScreenTransition.unk2 < 100) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk2 >= 99) {
                    gScreenTransition.unk0 = 0;
                }
                break;
            case 21:
                phi_v1 = (s32)(1.5f * ((61 - gScreenTransition.unk2) * 1.0f));
                if (phi_v1 > 89) {
                    phi_v1 = 89;
                }
                alpha = gTrigLut.unk2D0[(s16)phi_v1];
                if (alpha < 0) {
                    alpha = 0;
                }
                draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, alpha);
                if (gScreenTransition.unk2 < 100) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk2 >= 99) {
                    gScreenTransition.unk0 = 0;
                }
                break;
            case 15:
            case 22:
                if (gScreenTransition.unk2 < 60) {
                    phi_v1 = (s32)(1.5f * ((gScreenTransition.unk2 - 1) * 1.0f));
                    if (phi_v1 > 89) {
                        phi_v1 = 89;
                    }
                    alpha = gTrigLut.unk2D0[(s16)(89 - phi_v1)];
                    alpha = 0xFF - alpha;
                } else {
                    alpha = 0xFF;
                    gScreenTransition.unk0 = 1;
                }

                if (gScreenTransition.transitionId == 22) {
                    draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, alpha);
                } else {
                    draw_rectangle(&gMainDL, 0, 0, 320, 240, 255, 255, 255, alpha);
                }

                if (gScreenTransition.unk2 < 200) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk2 >= 180) {
                    gScreenTransition.unk0 = 0;
                }
                break;
            case 5:
                size = gScreenTransition.unk6;
                if (size > 8) {
                    size = 8;
                }
                gDPPipeSync(gMainDL++);
                gDPSetCycleType(gMainDL++, G_CYC_1CYCLE);
                gDPSetPrimColor(gMainDL++, 0, 0, 0x00, 0x00, 0x00, 0xFF);
                gDPSetEnvColor(gMainDL++, 0x00, 0x00, 0x00, 0x00);
                gDPSetCombineMode(gMainDL++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
                gDPSetRenderMode(gMainDL++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
                gDPPipeSync(gMainDL++);
                gDPSetTexturePersp(gMainDL++, G_TP_NONE);
                gDPSetFillColor(gMainDL++, 0x00000000);

                for (i = 0; i <= (u16)transition_width; i += 16) {
                    for (j = 0; j <= (u16)transition_height; j += 16) {
                        gDPFillRectangle(gMainDL++, i - size + 8, j - size + 8, i + size + 8, j + size + 8);
                    }
                }
                if (gScreenTransition.unk6 < 9) {
                    gScreenTransition.unk6 += 1;
                } else {
                    gScreenTransition.unk0 = 1;
                }
                gScreenTransition.unk2 += 1;
                break;
            case 4:
                size = 8 - gScreenTransition.unk6;
                if (size < 0) {
                    size = 0;
                }
                gDPPipeSync(gMainDL++);
                gDPSetCycleType(gMainDL++, G_CYC_1CYCLE);
                gDPSetPrimColor(gMainDL++, 0, 0, 0x00, 0x00, 0x00, 0xFF);
                gDPSetEnvColor(gMainDL++, 0x00, 0x00, 0x00, 0x00);
                gDPSetCombineMode(gMainDL++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
                gDPSetRenderMode(gMainDL++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
                gDPPipeSync(gMainDL++);
                gDPSetTexturePersp(gMainDL++, G_TP_NONE);
                gDPSetFillColor(gMainDL++, 0x00000000);

                for (i = 0; i <= (u16)transition_width; i += 16) {
                    for (j = 0; j <= (u16)transition_height; j += 16) {
                        gDPFillRectangle(gMainDL++, i - size + 8, j - size + 8, i + size + 8, j + size + 8);
                    }
                }

                if (gScreenTransition.unk6 < 9) {
                    gScreenTransition.unk6 += 1;
                } else {
                    gScreenTransition.unk0 = 1;
                }
                gScreenTransition.unk2 += 1;
                break;
            case 8:
                draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, alpha);
                gScreenTransition.unk0 = 1;
                if (gScreenTransition.unk2 < 100) {
                    gScreenTransition.unk2 += 1;
                }
                break;
            case 12:
                gScreenTransition.unk0 = 0;
                break;
            case 25:
                draw_rectangle(&gMainDL, 0, 0, 320, 240, 0, 0, 0, alpha);
                if (gScreenTransition.unk2 < 200) {
                    gScreenTransition.unk2 += 1;
                }
                if (gScreenTransition.unk2 > 9) {
                    gScreenTransition.unk0 = 1;
                }
                break;
            case 26:
                gScreenTransition.unk0 = 0;
                break;
        }

        if (gScreenTransition.unk2 > 180) {
            gScreenTransition.unk0 = 0;
        }
    }

    rc_pop();
}
