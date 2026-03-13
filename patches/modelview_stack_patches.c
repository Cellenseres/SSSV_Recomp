#include "sssv_patch_common.h"

#if SSSV_PATCH_FIX_MODELVIEW_STACK_BALANCE

// render_rig_instance_xlu() builds a local transform hierarchy for the
// ship/look-at rig. Keep the stack balanced so later translucent draws start
// from the caller's MODELVIEW depth.

static u8** const sRigBillboardTextures = (u8**)0x803A8370u;

static inline void append_joint_pose_transform(u8 push, const RigPoseStateMinimal* pose) {
    append_rig_modelview_transform_xlu(
        push,
        pose->current.translation[0],
        pose->current.translation[1],
        pose->current.translation[2],
        pose->current.rotation_euler[0],
        pose->current.rotation_euler[1],
        pose->current.rotation_euler[2]
    );
}

static inline void build_rig_root_matrix(Mtx* mtx, s32 x, s32 y, s32 z, s16 pitch, s16 yaw, s32 scale) {
    func_80125FE0(mtx, x, y, z, pitch, yaw, scale, scale, scale);
}

static inline void draw_rig_face_billboard(const RigPoseStateMinimal* root_pose, s32 x, s32 y, s32 z, s16 yaw, s32 scale) {
    func_8034C8F8_75DFA8(
        (s16)(root_pose->current.translation[0] + (x >> 16)),
        (s16)(root_pose->current.translation[1] + (y >> 16)),
        (s16)(root_pose->current.translation[2] + (z >> 16)),
        (s16)(((yaw - root_pose->current.rotation_euler[2]) * 256) / 360),
        sRigBillboardTextures[1],
        (s16)((scale << 3) >> 16),
        (s16)((scale << 4) >> 16),
        0x9B,
        0,
        0,
        0,
        8,
        1
    );
}

RECOMP_PATCH void render_rig_instance_xlu(s32 x, s32 y, s32 z, s16 pitch, s16 yaw, s32 scale, u8 rig_variant, u8 pose_bank) {
    s16 i;
    s16 j;
    s16 joint_index;

    if (pose_bank == 2) {
        draw_rig_face_billboard(&gRigPoseStates[pose_bank][0], x, y, z, yaw, scale);
    }

    scale /= 5;
    build_rig_root_matrix(
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs],
        x,
        y,
        z,
        pitch,
        yaw,
        scale
    );

    gSPMatrix(
        gXluDL++,
        &gDisplayListContext->modelViewMtx[gDisplayListContext->usedModelViewMtxs++],
        G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW
    );

    append_joint_pose_transform(0, &gRigPoseStates[pose_bank][0]);
    gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][6]);

    if (pose_bank != 2) {
        joint_index = 4;
        j = 0;

        for (i = 0; i < 2; i++) {
            append_joint_pose_transform(1, &gRigPoseStates[pose_bank][joint_index]);
            gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][j]);
            joint_index++;
            j++;

            append_joint_pose_transform(0, &gRigPoseStates[pose_bank][joint_index]);
            gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][j]);
            joint_index++;
            j++;

            append_joint_pose_transform(0, &gRigPoseStates[pose_bank][joint_index]);
            gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][j]);
            joint_index++;
            j++;

            gSPPopMatrix(gXluDL++, G_MTX_MODELVIEW);
        }
    }

    append_joint_pose_transform(1, &gRigPoseStates[pose_bank][1]);
    gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][7]);

    if (pose_bank != 2) {
        append_joint_pose_transform(1, &gRigPoseStates[pose_bank][2]);
        gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][8]);
        gSPPopMatrix(gXluDL++, G_MTX_MODELVIEW);
    }

    append_joint_pose_transform(1, &gRigPoseStates[pose_bank][1]);

    if (pose_bank == 1) {
        gDPSetPrimColor(gXluDL++, 0, 0, 0xEA, 0xE6, 0xFF, 0xFF);
        gDPSetCombineLERP(
            gXluDL++,
            SHADE, 0, PRIMITIVE, 0,
            SHADE, 0, PRIMITIVE, 0,
            SHADE, 0, PRIMITIVE, 0,
            SHADE, 0, PRIMITIVE, 0
        );
    }

    append_joint_pose_transform(1, &gRigPoseStates[pose_bank][3]);

    gSPDisplayList(gXluDL++, gRigPartDisplayLists[rig_variant][9]);
    gSPPopMatrix(gXluDL++, G_MTX_MODELVIEW);
    gSPPopMatrix(gXluDL++, G_MTX_MODELVIEW);
    gSPPopMatrix(gXluDL++, G_MTX_MODELVIEW);

    // Return to the caller's stack depth before the next translucent draw.
    gSPPopMatrix(gXluDL++, G_MTX_MODELVIEW);
}

#endif
