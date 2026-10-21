#include "sssv_patch_common.h"

// Reflections in the dome glass (e.g. the glass walls in Snow Joke). When the camera is near a reflecting
// wall, the game sets gCameraVisibilityMask[6] (bit 0: z mirror, bit 1: x mirror, bits 2-11: plane offset)
// and replays the frame's object display lists under a mirror matrix multiplied into the projection. This
// happens before the world geometry is drawn, so the mirrored objects only show through the translucent glass.

// The patch-side DisplayList type stops before these fields; offsets as in the decomp's DisplayList.
#define DL_OPAQUE_LIST(ctx)  ((Gfx*)((u8*)(ctx) + 0x9600))
#define DL_XLU_LIST(ctx)     ((Gfx*)((u8*)(ctx) + 0xBB80))
#define DL_XLU_LIST_LEN      1000
#define DL_MIRROR_MTX(ctx)   ((Mtx*)((u8*)(ctx) + 0x37510))
#define DL_AMBIENT(ctx)      ((u8*)(ctx) + 0x3B638) // lights.a: col[3], pad, colc[3], pad
#define DL_LIGHT0(ctx)       ((u8*)(ctx) + 0x3B640) // lights.l[0]: col[3], pad, colc[3], pad, dir[3], pad
#define DL_MIRROR_AMBIENT(ctx) ((u8*)(ctx) + 0x3B6B0)
#define DL_MIRROR_LIGHT(ctx) ((u8*)(ctx) + 0x3B6B8)

// F3DEX gSPNumLights(1) and gSPLight(l, n).
static inline void sp_num_lights_1(Gfx* pkt) {
    pkt->words.w0 = 0xBC000002u; // G_MOVEWORD, G_MW_NUMLIGHT
    pkt->words.w1 = 0x80000040u; // NUML(1)
}

static inline void sp_light(Gfx* pkt, const void* light, u32 n) {
    pkt->words.w0 = _SHIFTL(G_MOVEMEM, 24, 8) | _SHIFTL((n - 1) * 2 + 0x86, 16, 8) | _SHIFTL(16, 0, 16);
    pkt->words.w1 = (u32)(uintptr_t)light;
}

extern void func_80127D30(Mtx* mtx, s16 zOffset); // build_ship_window_z_projection_matrix
extern void func_80127ED4(Mtx* mtx, s16 xOffset); // build_ship_window_x_projection_matrix

// Start of the translucent object list after the world state that setup_world_perspective_6AB090 puts in
// front of it. That state reloads the projection, which would drop the mirror matrix in the replay.
extern Gfx* gXluStateBarrierEnd;

static inline void load_world_modelview(void) {
    gSPMatrix(gMainDL++, &gDisplayListContext->unk374D0, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
}

static inline void restore_world_projection(void) {
    gSPMatrix(gMainDL++, &gDisplayListContext->unk37410, G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
    gSPMatrix(gMainDL++, &gDisplayListContext->unk37450, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
    gSPMatrix(gMainDL++, &gDisplayListContext->unk37490, G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
}

RECOMP_PATCH void render_ship_window_projection_replay(void) {
    DisplayList* ctx = gDisplayListContext;
    u8* ambient;
    u8* light;
    u8* mirror_ambient;
    u8* mirror_light;
    s32 i;

    if (gCameraVisibilityMask[6] & 1) {
        func_80127D30(DL_MIRROR_MTX(ctx), (gCameraVisibilityMask[6] & 0xFFC) << 1);

        gSPMatrix(gMainDL++, DL_MIRROR_MTX(ctx), G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        if ((gXluStateBarrierEnd >= DL_XLU_LIST(ctx)) && (gXluStateBarrierEnd < DL_XLU_LIST(ctx) + DL_XLU_LIST_LEN)) {
            load_world_modelview();
            gSPDisplayList(gMainDL++, gXluStateBarrierEnd);
        }
        load_world_modelview();
        gSPDisplayList(gMainDL++, DL_OPAQUE_LIST(ctx));
        gSPPopMatrix(gMainDL++, G_MTX_MODELVIEW);

        restore_world_projection();
    } else if (gCameraVisibilityMask[6] & 2) {
        func_80127ED4(DL_MIRROR_MTX(ctx), (gCameraVisibilityMask[6] & 0xFFC) << 1);

        // The x mirror only replays opaque objects, lit with half the ambient and a quarter of the light.
        ambient = DL_AMBIENT(ctx);
        light = DL_LIGHT0(ctx);
        mirror_ambient = DL_MIRROR_AMBIENT(ctx);
        mirror_light = DL_MIRROR_LIGHT(ctx);
        for (i = 0; i < 3; i++) {
            mirror_ambient[i] = ambient[i] >> 1;
            mirror_ambient[4 + i] = ambient[4 + i] >> 1;
            mirror_light[i] = light[i] >> 2;
            mirror_light[4 + i] = light[i] >> 2; // as in the original: colc from col
            mirror_light[8 + i] = light[8 + i];
        }

        sp_num_lights_1(gMainDL++);
        sp_light(gMainDL++, mirror_light, 1);
        sp_light(gMainDL++, mirror_ambient, 2);

        gSPMatrix(gMainDL++, DL_MIRROR_MTX(ctx), G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);
        load_world_modelview();
        gSPDisplayList(gMainDL++, DL_OPAQUE_LIST(ctx));

        sp_num_lights_1(gMainDL++);
        sp_light(gMainDL++, light, 1);
        sp_light(gMainDL++, ambient, 2);

        gSPPopMatrix(gMainDL++, G_MTX_MODELVIEW);

        restore_world_projection();
    }
}
