#ifndef __SSSV_PATCH_TRACE_H__
#define __SSSV_PATCH_TRACE_H__

#include "sssv_patch_common.h"

#ifndef SSSV_PATCH_DEBUG_LOG
#define SSSV_PATCH_DEBUG_LOG 0
#endif

typedef enum {
    SSSV_DIAG_CONTEXT = 1u << 0,
    SSSV_DIAG_PROJ_VIEW = 1u << 1,
    SSSV_DIAG_TEXRECT = 1u << 2,
    SSSV_DIAG_BILLBOARD = 1u << 3,
    SSSV_DIAG_WARN = 1u << 4,
    SSSV_DIAG_INTRO = 1u << 5,
} SssvPatchDiagBits;

extern RECOMP_EXPORT u32 sssv_patch_diag_flags;

typedef enum {
    PATCH_TAG_SET_SCREEN_SCALING = 1,
    PATCH_TAG_DRAW_RECTANGLE = 2,
    PATCH_TAG_INTRO_FRAME = 3,
    PATCH_TAG_END_DISPLAY_LISTS = 4,
    PATCH_TAG_WORLD_PERSPECTIVE = 5,
    PATCH_TAG_DEPTH_CLEAR = 6,
    PATCH_TAG_DRAW_RECTANGLE_FULLCLEAR_PROMOTE = 7,
    PATCH_TAG_DRAW_RECTANGLE_BORDER_SKIP = 8,
    PATCH_TAG_END_DISPLAY_LISTS_VP = 9,
    PATCH_TAG_WORLD_DEPTH_CLEAR_FAILSAFE = 10,
    PATCH_TAG_UI_DOMAIN = 11,
    PATCH_TAG_OSD_SCORE = 12,
    PATCH_TAG_OSD_TIMER = 13,
    PATCH_TAG_OSD_TEXT = 14,
    PATCH_TAG_CREDITS = 15,
    PATCH_TAG_CONTEXT_ENTER = 16,
    PATCH_TAG_CONTEXT_EXIT = 17,
    PATCH_TAG_PROJECTION_ID = 18,
    PATCH_TAG_BILLBOARD_ID = 19,
    PATCH_TAG_ID_COLLISION = 20,
    PATCH_TAG_FRAME_BEGIN = 21,
    PATCH_TAG_FRAME_END = 22,
    PATCH_TAG_CAMERA_CUT_SKIP = 23,
    /* tag 24 unused */
    PATCH_TAG_PROJ_VIEW_SNAPSHOT = 25,
    PATCH_TAG_TEXRECT_CLASS = 26,
    PATCH_TAG_BILLBOARD_SAMPLE = 27,
    PATCH_TAG_ENERGY_SETTIMG_SEEN = 28,
    PATCH_TAG_ENERGY_RECT_EMIT = 29,
    PATCH_TAG_INTRO_VIEWPORT = 30,
    PATCH_TAG_INTRO_STATE = 31,
    PATCH_TAG_SPRITE2D_BG_SCALE = 32,
    PATCH_TAG_SPRITE2D_BG_CALL = 33,
    PATCH_TAG_DEPTH_CLEAR_BOUNDS = 34,
    PATCH_TAG_SPRITE2D_VIEWPORT = 35,
    PATCH_TAG_INTRO_COLOR_TARGET = 36,
    PATCH_TAG_INTRO_SCISSOR = 37,
} PatchTraceTag;

#define PATCH_TRACE_DIAG(mask, tag, a, b, c)           \
    do {                                                \
        if ((sssv_patch_diag_flags & (mask)) != 0u) {  \
            recomp_patch_debug_u32((u32)(tag), (u32)(a), (u32)(b), (u32)(c)); \
        }                                               \
    } while (0)

#if SSSV_PATCH_DEBUG_LOG
#define PATCH_TRACE(tag, a, b, c) recomp_patch_debug_u32((u32)(tag), (u32)(a), (u32)(b), (u32)(c))
#else
#define PATCH_TRACE(tag, a, b, c) ((void)0)
#endif

#define SHOULD_TRACE_PERIODIC(calls, warmup, interval) \
    (((calls) <= (warmup)) || (((interval) != 0) && (((calls) % (interval)) == 0)))

#endif
