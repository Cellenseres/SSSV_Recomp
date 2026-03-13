#include "sssv_patch_common.h"

static inline void write_fixed_pair(Mtx *mtx, s32 pair_index, s32 e1, s32 e2) {
    u32 *int_part = (u32 *)&mtx->m[0][0];
    u32 *frac_part = (u32 *)&mtx->m[2][0];

    int_part[pair_index] = (u32)(e1 & 0xFFFF0000) | (u32)((e2 >> 16) & 0xFFFF);
    frac_part[pair_index] = (u32)((e1 << 16) & 0xFFFF0000) | (u32)(e2 & 0xFFFF);
}

// The original code hand-packed these special ship-window matrices through a
// union layout that depends on big-endian halfword ordering. Build the same
// 16.16 matrix words directly so the host endianness no longer matters.
RECOMP_PATCH void func_80127D30(Mtx *arg0, s16 arg1) {
    const s32 shear = ((s32)arg1 << 17);

    write_fixed_pair(arg0, 0, 0x00010000, 0x00000000);
    write_fixed_pair(arg0, 1, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 2, 0x00000000, 0x00010000);
    write_fixed_pair(arg0, 3, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 4, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 5, 0xFFFF0000, 0x00000000);
    write_fixed_pair(arg0, 6, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 7, shear,      0x00010000);
}

RECOMP_PATCH void func_80127ED4(Mtx *arg0, s16 arg1) {
    const s32 shear = ((s32)arg1 << 17);

    write_fixed_pair(arg0, 0, 0xFFFF0000, 0x00000000);
    write_fixed_pair(arg0, 1, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 2, 0x00000000, 0x00010000);
    write_fixed_pair(arg0, 3, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 4, 0x00000000, 0x00000000);
    write_fixed_pair(arg0, 5, 0x00010000, 0x00000000);
    write_fixed_pair(arg0, 6, shear,      0x00000000);
    write_fixed_pair(arg0, 7, 0x00000000, 0x00010000);
}
