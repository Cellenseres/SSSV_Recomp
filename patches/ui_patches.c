#include "sssv_patch_common.h"
#include "sssv_render_context.h"
#include "sssv_rt64_tagging.h"
#include "sssv_patch_trace.h"
#include "sssv_patch_ui_domain.h"

static u32 g_dbg_osd_score_calls = 0;
static u32 g_dbg_osd_timer_calls = 0;
static u32 g_dbg_osd_text_calls = 0;
static u32 g_dbg_credits_calls = 0;

static inline u32 begin_hud_pass(void) {
    u32 hud_id = rc_next_hud_transform_id();

    rc_set(RC_HUD_ORTHO);
    rc_note_texrect_context(TRC_HUD_UI, 0x55490001u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;

    rt64_tag_projection_matrix(&gMainDL, SSSV_PROJECTION_HUD_TRANSFORM_ID);
    rt64_tag_model_matrix(&gMainDL, hud_id, 0);
    return hud_id;
}

static inline void end_hud_pass(void) {
    rt64_pop_model_matrix(&gMainDL);
}

RECOMP_PATCH void osd_draw_score(void) {
    const u8 font_lcd = 1;
    const u32 hud_id = begin_hud_pass();
    sprintf((char*)gGameState.scoreText, "%8d", gGameState.score);
    select_font(0, font_lcd, 1, 0);
    display_score(&gMainDL, (u8*)gGameState.scoreText, ui_safe_right_anchor(34), (u16)((gHudBarBaseY >> 2) - 10));
    end_hud_pass();

    g_dbg_osd_score_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_osd_score_calls, 8, 240)) {
        PATCH_TRACE(PATCH_TAG_OSD_SCORE, g_dbg_osd_score_calls, (u16)ui_safe_width(), (u16)hud_id);
    }
}

RECOMP_PATCH void osd_draw_timer(s16 arg0) {
    s16 text[20];
    const u32 hud_id = begin_hud_pass();

    if (gHudTimerSeconds > 0) {
        if (gHudTimerSeconds < 60) {
            sprintf((char*)gHudTimerAscii, "%d", gHudTimerSeconds);
        } else {
            sprintf((char*)gHudTimerAscii, "%d:%02d", gHudTimerSeconds / 60, gHudTimerSeconds % 60);
        }
    }

    prepare_text((u8*)gHudTimerAscii, text);
    load_default_display_list(&gMainDL);
    set_menu_text_color(255, 255, 0, 255);
    select_font(0, 0, 1, 0);
    display_text_centered(&gMainDL, text, ui_safe_center_x(), (u16)arg0, 16.0f, 16.0f);
    end_hud_pass();

    g_dbg_osd_timer_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_osd_timer_calls, 8, 240)) {
        PATCH_TRACE(PATCH_TAG_OSD_TIMER, g_dbg_osd_timer_calls, (u16)ui_safe_center_x(), (u16)hud_id);
    }
}

RECOMP_PATCH void draw_hud_center_text(s16 arg0) {
    s16 saved_width;
    const u32 hud_id = begin_hud_pass();
    load_default_display_list(&gMainDL);
    set_menu_text_color(255, 255, 0, 255);
    select_font(0, 0, 1, 0);
    // Restore native width: display_text_word_wrapped derives its word-wrap threshold from gScreenWidth.
    saved_width = gScreenWidth;
    gScreenWidth = SSSV_BASE_WIDTH;
    display_text_word_wrapped(&gMainDL, gHudCenterText, ui_safe_center_x(), (u16)arg0, 16.0f, 16.0f, 0x10);
    gScreenWidth = saved_width;
    end_hud_pass();

    g_dbg_osd_text_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_osd_text_calls, 8, 240)) {
        PATCH_TRACE(PATCH_TAG_OSD_TEXT, g_dbg_osd_text_calls, (u16)ui_safe_center_x(), (u16)hud_id);
    }
}

RECOMP_PATCH s32 display_credits(void) {
    s16 i;
    s16 wide[58];
    u8 ascii[64];
    const u16 center_x = ui_safe_center_x();
    const u32 hud_id = begin_hud_pass();

    load_default_display_list(&gMainDL);
    set_menu_text_color(0xFF, 0xFF, 0xFF, 0xFF);
    select_font(0, 0, 0, 0);

    for (i = 0; i < 16; i++) {
        sprintf((char*)ascii, "%s", gCreditsLines[i + gCreditsEntryOffset]);
        prepare_text(ascii, wide);
        if ((ascii[0] == 'E') && (ascii[1] == 'N') && (ascii[2] == 'D')) {
            gCreditsEntryOffset = 0;
            i = 1000;
        } else {
            display_text_centered(&gMainDL, wide, center_x, (u16)(gCreditsVerticalPosition + (i * 16)), 16.0f, 16.0f);
        }
    }

    gCreditsVerticalPosition -= 1;
    if (gCreditsVerticalPosition == -16) {
        gCreditsEntryOffset += 1;
        gCreditsVerticalPosition = 0;
    }
    end_hud_pass();

    g_dbg_credits_calls++;
    if (SHOULD_TRACE_PERIODIC(g_dbg_credits_calls, 4, 240)) {
        PATCH_TRACE(PATCH_TAG_CREDITS, g_dbg_credits_calls, center_x, (u16)hud_id);
    }

    if (i > 100) {
        return 1;
    }
    return 0;
}

RECOMP_PATCH void check_cheats(OSContPad *contPad) {
    u8* cheat_input_buffer = CHEAT_INPUT_BUFFER_ADDR;
    u16 lastButton;
    u16 butDown;
    u32 butDown2;
    s16 text[60];
    const u32 hud_id = rc_next_hud_transform_id();

    rc_set(RC_HUD_ORTHO);
    rc_note_texrect_context(TRC_HUD_UI, 0xCEA70000u);
    cur_perspective_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
    cur_ortho_projection_transform_id = SSSV_PROJECTION_HUD_TRANSFORM_ID;
    rt64_tag_projection_matrix(&gMainDL, SSSV_PROJECTION_HUD_TRANSFORM_ID);

    if (gCheats.debugMode != 0) {
        load_default_display_list(&gMainDL);
        set_menu_text_color(0xFF, 0xFF, 0, 0xFF);
        select_font(0, 0, 0, 0);
        if ((gCheats.debugMode != 0) && (gOverlayMenuState.unk0 == 0)) {
            sprintf(
                gDebugTextBuffer,
                "(%3d  %3d  %4d)",
                gAnimalState.animals[gCurrentAnimalIndex].animal->position.xPos.h >> 6,
                gAnimalState.animals[gCurrentAnimalIndex].animal->position.zPos.h >> 6,
                gAnimalState.animals[gCurrentAnimalIndex].animal->position.yPos.h
            );
            prepare_text((u8*)gDebugTextBuffer, text);
            display_text(&gMainDL, text, ui_safe_right_anchor(20), 20, 16.0f, 16.0f);
            prepare_text((u8*)"Ver - 1.37", text);
            display_text(&gMainDL, text, ui_safe_right_anchor(20), 36, 16.0f, 16.0f);
        }
    }

    lastButton = 0;
    if (gCheatInputDebounceTimer != 0) {
        gCheatInputDebounceTimer--;
    }

    butDown = contPad->button;
    butDown2 = butDown;
    if ((gCheatInputNeutralLatch == 0) && (butDown2 == 0)) {
        gCheatInputNeutralLatch = 1;
        return;
    } else if (butDown2 != 0) {
        gCheatInputNeutralLatch = 0;
    }

    if ((gCheatInputDebounceTimer == 0) && (butDown2 != 0) && (gCheatLastButtonCode != butDown)) {
        u16 down = butDown;
        if (gFrameStepDivisor == 1) {
            gCheatInputDebounceTimer = 12;
        } else {
            gCheatInputDebounceTimer = 6;
        }
        if (down & CONT_A) {
            lastButton = 'A';
        }
        if (down & CONT_B) {
            lastButton = 'B';
        }
        if (down & CONT_UP) {
            lastButton = 'U';
        }
        if (down & CONT_DOWN) {
            lastButton = 'D';
        }
        if (down & CONT_LEFT) {
            lastButton = 'L';
        }
        if (down & CONT_RIGHT) {
            lastButton = 'R';
        }
        if (down & U_CBUTTONS) {
            lastButton = 'N';
        }
        if (down & D_CBUTTONS) {
            lastButton = 'S';
        }
        if (down & L_CBUTTONS) {
            lastButton = 'W';
        }
        if (down & R_CBUTTONS) {
            lastButton = 'E';
        }
        if (down & Z_TRIG) {
            lastButton = 'Z';
        }
        if (down & L_TRIG) {
            lastButton = 'I';
        }
        if (down & R_TRIG) {
            lastButton = 'T';
        }
        gCheatLastButtonCode = (u8)lastButton;
    }

    if (lastButton != 0) {
        if (++gCheatInputCursor >= 20) {
            gCheatInputCursor = 0;
        }
        cheat_input_buffer[gCheatInputCursor] = (u8)lastButton;
    }

    if (check_cheat_code(cheat_input_buffer, "WIZDIZWE")) {
        gAnimalState.animals[gCurrentAnimalIndex].animal->health = 127;
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "UDIZDUZD")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheats.europe = 1 - gCheats.europe;
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "UDZIDEZD")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheats.ice = 1 - gCheats.ice;
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "UDIZDWZD")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheats.jungle = 1 - gCheats.jungle;
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "UDIZDLZD")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheats.desert = 1 - gCheats.desert;
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "UDIZDRZD")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheats.bcp = 1 - gCheats.bcp;
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "DUZIDLZD")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        gCheats.hidden = 1 - gCheats.hidden;
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "UIZDLZDU")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        cheat_activate_scale_pulse_effect();
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "IDZIDUIL")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        cheat_activate_energy_camera_effect();
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "ZDUIRILR")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        cheat_activate_big_head_effect();
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "LRZILZRL")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        cheat_toggle_mystery_bear();
        gCheatInputCursor = 0;
    }
    if (check_cheat_code(cheat_input_buffer, "DANISIL\0")) {
        play_sound_effect(SFX_CHEAT_ENABLED, 0, 0x5000, 1.0f, 64);
        cheat_clear_camera_effects();
        gCheatInputCursor = 0;
    }
    PATCH_TRACE(PATCH_TAG_OSD_TEXT, 0xCEA70000u, hud_id, (u16)ui_safe_width());
}

// gScreenWidth/2 becomes 213 in widescreen; ui_safe_center_x() keeps the message centered.
RECOMP_PATCH void no_controller_message(void) {
    if (gControllerConnected == 0) {
        load_segments(&gMainDL, gDisplayListContext);
        draw_rectangle(&gMainDL, 0, 16, 320, 36, 40, 40, 40, 128);
        gDPPipeSync(gMainDL++);

        load_default_display_list(&gMainDL);
        select_font(0, 0 /* FONT_DEFAULT */, 0, 0);
        set_menu_text_color(0xFF, 0xFF, 0xFF, 0xFF);
        display_text_centered(&gMainDL, gNoControllerMessageText, ui_safe_center_x(), 20, 16.0f, 16.0f);
        gDPPipeSync(gMainDL++);
    }
}
