#ifndef __SSSV_PATCH_TYPES_H__
#define __SSSV_PATCH_TYPES_H__

#include <stddef.h>
#include <ultra64.h>

#include "sssv_patch_input.h"

#define SSSV_BASE_ASPECT (4.0f / 3.0f)
#define SSSV_BASE_WIDTH 320
#define SSSV_BASE_HEIGHT 240
#define SSSV_TV_SAFE_INSET 8  // TV-safe overscan margin used by the original game (px per side).

#define VISIBILITY_VISIBLE 0
#define VISIBILITY_TOO_FAR 1
#define VISIBILITY_OUT_OF_BOUNDS_X 2
#define VISIBILITY_OUT_OF_BOUNDS_Y 3
#define VISIBILITY_INVISIBLE 4

#define SSSV_PATCH_DISABLE_LOD 1
#define SSSV_PATCH_DISABLE_FAR_AWAY_CULL 1

#define SFX_CHEAT_ENABLED 86
#define CHEAT_INPUT_BUFFER_ADDR ((u8*)0x803F6410)

typedef struct {
    unsigned char col[3];
    char pad1;
    unsigned char colc[3];
    char pad2;
    signed char dir[3];
    char pad3;
} Light_t;

typedef struct {
    unsigned char col[3];
    char pad1;
    unsigned char colc[3];
    char pad2;
} Ambient_t;

typedef union {
    Light_t l;
    long long force_structure_alignment[2];
} Light;

typedef union {
    Ambient_t l;
    long long force_structure_alignment[1];
} Ambient;

typedef struct {
    Ambient a;
    Light l[1];
} Lights1;

typedef struct {
    void* SourceImagePointer;
    void* TlutPointer;
    s16 Stride;
    s16 SubImageWidth;
    s16 SubImageHeight;
    s8 SourceImageType;
    s8 SourceImageBitSize;
    s16 SourceImageOffsetS;
    s16 SourceImageOffsetT;
    s8 dummy[4];
} uSprite_t;

typedef union {
    uSprite_t s;
    long long force_structure_alignment[3];
} uSprite;

typedef struct {
    u8 pad0000[0x0008];
    s16 vi_mode_type;
    s16 screenWidth;
} VIDataMinimal;

typedef struct {
    u8 pad0000[0x32870];
    uSprite sprites[140];
    Mtx modelViewMtx[250];
    Mtx unk37410;
    Mtx unk37450;
    Mtx unk37490;
    Mtx unk374D0;
    u8 pad37510[0x1400];
    s32 usedHilites;
    s32 usedSprites;
    s32 usedModelViewMtxs;
    s32 usedVtxs;
    u8 pad38920[0xF0];
    f32 unk38A10[4][4];
    u8 pad38A50[0x8C0];
    u16 unk39310;
} DisplayList;

typedef struct {
    s16 vscale[4];
    s16 vtrans[4];
} VpRaw;

typedef struct {
    VpRaw vp;
} ViewportStateMinimal;

typedef struct {
    u8 pad0000[0x3BBE8];
    u8* framebuffer;
} FrameContextMinimal;

typedef struct {
    f32 unk0;
    f32 unk4;
    f32 unk8;
    f32 unkC;
    f32 unk10;
    f32 unk14;
} OverlayLookAtMinimal;

typedef struct {
    u8 pad0000[0x48];
    s16 unk48;
} OverlayUiStateMinimal;

typedef struct {
    u16 unk0;
    u16 unk2;
    u16 unk4;
    u16 unk6;
    u16 unk8;
    u16 unkA;
    s16 unkC;
    s16 unkE;
    u8 pad0010[0x0030];
    s16 unk40;
    s16 unk42;
    u8 pad0044[0x009C];
    f32 fovY;
} LevelConfigMinimal;

typedef struct {
    s16 min;
    s16 max;
    u8 r;
    u8 g;
    u8 b;
    u8 pad;
} FogMinimal;

typedef struct {
    s16 unk0;
} Overlay2MenuState;

typedef struct {
    /* 0x0 */ s16 unk0;
    /* 0x2 */ s16 unk2;
    /* 0x4 */ s16 unk4;
    /* 0x6 */ s16 debugMode;
    /* 0x8 */ s16 unk8;
    /* 0xA */ s16 europe;
    /* 0xC */ s16 ice;
    /* 0xE */ s16 jungle;
    /* 0x10 */ s16 desert;
    /* 0x12 */ s16 bcp;
    /* 0x14 */ s16 hidden;
    /* 0x16 */ s16 unk16;
    /* 0x18 */ s16 unk18;
} CheatsMinimal;

typedef union {
    s16 h;
    s32 w;
} PosMinimal;

typedef struct {
    PosMinimal xPos;
    PosMinimal zPos;
    PosMinimal yPos;
} PositionMinimal;

typedef struct {
    /* 0x000 */ PositionMinimal position;
    /* 0x00C */ u8 pad00C[0x140];
    /* 0x14C */ s16 health;
} AnimalMinimal;

typedef struct {
    void* unk0;
    AnimalMinimal* animal;
} AnimalSlotMinimal;

typedef struct {
    u8 pad0000[0x3EB0];
    AnimalSlotMinimal animals[50];
} AnimalStateMinimal;

typedef struct {
    /* 0x0 */ s32 score;
    /* 0x4 */ s16 unk4;
    /* 0x6 */ s16 powercells2;
    /* 0x8 */ s16 level;
    /* 0xA */ u16 unkA;
    /* 0xC */ s16 powercells;
    /* 0xE */ char scoreText[18];
} LevelProgressMinimal;

typedef struct {
    s16 unk0[360];
    s16 unk2D0[90];
    s16 unk384[90];
} TrigLutMinimal;

typedef struct {
    s16 unk0;
    s16 unk2;
    s16 transitionId;
    s16 unk6;
    s16 overlayTV;
} ScreenTransitionMinimal;

_Static_assert(offsetof(VIDataMinimal, vi_mode_type) == 0x08, "VIDataMinimal.vi_mode_type offset mismatch");
_Static_assert(offsetof(VIDataMinimal, screenWidth) == 0x0A, "VIDataMinimal.screenWidth offset mismatch");
_Static_assert(offsetof(LevelConfigMinimal, unkC) == 0x0C, "LevelConfigMinimal.unkC offset mismatch");
_Static_assert(offsetof(LevelConfigMinimal, unkE) == 0x0E, "LevelConfigMinimal.unkE offset mismatch");
_Static_assert(offsetof(LevelConfigMinimal, unk42) == 0x42, "LevelConfigMinimal.unk42 offset mismatch");
_Static_assert(offsetof(LevelConfigMinimal, fovY) == 0xE0, "LevelConfigMinimal.fovY offset mismatch");
_Static_assert(offsetof(DisplayList, modelViewMtx) == 0x33590, "DisplayList.modelViewMtx offset mismatch");
_Static_assert(offsetof(DisplayList, usedHilites) == 0x38910, "DisplayList.usedHilites offset mismatch");
_Static_assert(offsetof(DisplayList, usedSprites) == 0x38914, "DisplayList.usedSprites offset mismatch");
_Static_assert(offsetof(DisplayList, usedModelViewMtxs) == 0x38918, "DisplayList.usedModelViewMtxs offset mismatch");
_Static_assert(offsetof(DisplayList, usedVtxs) == 0x3891C, "DisplayList.usedVtxs offset mismatch");
_Static_assert(offsetof(DisplayList, unk38A10) == 0x38A10, "DisplayList.unk38A10 offset mismatch");
_Static_assert(offsetof(DisplayList, unk39310) == 0x39310, "DisplayList.unk39310 offset mismatch");
_Static_assert(offsetof(FrameContextMinimal, framebuffer) == 0x3BBE8, "FrameContextMinimal.framebuffer offset mismatch");
_Static_assert(offsetof(CheatsMinimal, debugMode) == 0x06, "CheatsMinimal.debugMode offset mismatch");
_Static_assert(offsetof(AnimalMinimal, health) == 0x14C, "AnimalMinimal.health offset mismatch");
_Static_assert(offsetof(AnimalStateMinimal, animals) == 0x3EB0, "AnimalStateMinimal.animals offset mismatch");
_Static_assert(offsetof(LevelProgressMinimal, score) == 0x0, "LevelProgressMinimal.score offset mismatch");
_Static_assert(offsetof(LevelProgressMinimal, scoreText) == 0xE, "LevelProgressMinimal.scoreText offset mismatch");
_Static_assert(sizeof(FogMinimal) == 0x8, "FogMinimal size mismatch");
_Static_assert(offsetof(TrigLutMinimal, unk2D0) == 0x2D0, "TrigLutMinimal.unk2D0 offset mismatch");
_Static_assert(sizeof(ScreenTransitionMinimal) == 0xA, "ScreenTransitionMinimal size mismatch");
_Static_assert(offsetof(OverlayUiStateMinimal, unk48) == 0x48, "OverlayUiStateMinimal.unk48 offset mismatch");
_Static_assert(sizeof(Light_t) == 0xC, "Light_t size mismatch");
_Static_assert(sizeof(Ambient_t) == 0x8, "Ambient_t size mismatch");
_Static_assert(sizeof(Light) == 0x10, "Light size mismatch");
_Static_assert(sizeof(Ambient) == 0x8, "Ambient size mismatch");
_Static_assert(sizeof(Lights1) == 0x18, "Lights1 size mismatch");

#endif
