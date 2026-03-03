#ifndef __SSSV_PATCH_ULTRA64_H__
#define __SSSV_PATCH_ULTRA64_H__

#include <stdint.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

typedef struct {
    u32 w0;
    u32 w1;
} Gwords;

typedef union {
    Gwords words;
    u64 force_structure_alignment;
} Gfx;

typedef s32 Mtx_t[4][4];

typedef union {
    Mtx_t m;
    u64 force_structure_alignment;
} Mtx;

// F3DEX command constants used by current patches.
#define G_MTX 0x01u
#define G_MOVEMEM 0x03u
#define G_MOVEWORD 0xBCu
#define G_DL 0x06u
#define G_SPRITE2D_BASE 0x09u
#define G_SPRITE2D_SCALEFLIP 0xBEu
#define G_SPRITE2D_DRAW 0xBDu
#define G_MW_PERSPNORM 0x0Eu
#define G_MW_SEGMENT 0x06u

// Minimal RDP command constants required by patch-side display-list writes.
#define G_SETTIMG 0xFDu
#define G_SETTILE 0xF5u
#define G_LOADTILE 0xF4u
#define G_LOADBLOCK 0xF3u
#define G_SETTILESIZE 0xF2u
#define G_SETCOMBINE 0xFCu
#define G_SETPRIMCOLOR 0xFAu
#define G_SETENVCOLOR 0xFBu
#define G_SETPRIMDEPTH 0xEEu
#define G_SETCIMG 0xFFu
#define G_SETFILLCOLOR 0xF7u
#define G_FILLRECT 0xF6u
#define G_SETSCISSOR 0xEDu
#define G_LOADSYNC 0xE6u
#define G_RDPPIPESYNC 0xE7u
#define G_RDPFULLSYNC 0xE9u
#define G_RDPHALF_1 0xB4u
#define G_RDPHALF_2 0xB3u
#define G_TEXRECT 0xE4u

// Matrix parameter flags for F3DEX.
#define G_MTX_MODELVIEW 0x00u
#define G_MTX_PROJECTION 0x01u
#define G_MTX_MUL 0x00u
#define G_MTX_LOAD 0x02u
#define G_MTX_NOPUSH 0x00u
#define G_MTX_PUSH 0x04u

#define _SHIFTL(v, s, w) \
    ((u32)((((u32)(v)) & ((1u << (w)) - 1u)) << (s)))

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

// Minimal "other mode" subset used by draw_rectangle.
#define G_SC_NON_INTERLACE 0u

#define G_AC_NONE 0u

#define G_IM_FMT_RGBA 0u
#define G_IM_FMT_YUV 1u
#define G_IM_FMT_CI 2u
#define G_IM_FMT_IA 3u
#define G_IM_FMT_I 4u

#define G_IM_SIZ_4b 0u
#define G_IM_SIZ_8b 1u
#define G_IM_SIZ_16b 2u
#define G_IM_SIZ_32b 3u

#define G_TX_LOADTILE 7u
#define G_TX_RENDERTILE 0u
#define G_TX_NOMIRROR 0u
#define G_TX_MIRROR 1u
#define G_TX_WRAP 0u
#define G_TX_CLAMP 2u
#define G_TX_NOMASK 0u
#define G_TX_NOLOD 0u

#define G_CYC_1CYCLE 0x00000000u
#define G_CYC_FILL 0x00300000u

#define G_CD_NOISE 0x00000080u
#define G_AD_DISABLE 0x00000030u

// Render mode constants already expanded for gDPSetRenderMode below.
#define G_RM_NOOP 0x00000000u
#define G_RM_NOOP2 0x00000000u
#define G_RM_OPA_SURF G_RM_NOOP
#define G_RM_OPA_SURF2 G_RM_NOOP2
#define G_RM_AA_XLU_SURF 0x005041C8u
#define G_RM_AA_XLU_SURF2 0x00000000u
#define G_RM_XLU_SURF 0x00504240u
#define G_RM_XLU_SURF2 0x00000000u

#define G_CC_PRIMITIVE 0u

#define G_TP_NONE 0u

#define GPACK_RGBA5551(r, g, b, a) \
    ((((u32)(r) << 8) & 0xF800u) | (((u32)(g) << 3) & 0x07C0u) | (((u32)(b) >> 2) & 0x003Eu) | ((u32)(a) & 0x1u))

static inline u32 gpack_texrect_xy(int x, int y) {
    return ((u32)(x & 0x3FF) << 14) | ((u32)(y & 0x3FF) << 2);
}

static inline u32 gpack_scissor_xy(int x, int y) {
    return ((u32)(x & 0x0FFF) << 12) | ((u32)(y & 0x0FFF));
}

static inline void gImmp1_(Gfx* pkt, u32 cmd, u32 param) {
    pkt->words.w0 = _SHIFTL(cmd, 24, 8);
    pkt->words.w1 = param;
}

static inline void gDPPipeSync_(Gfx* pkt) {
    pkt->words.w0 = _SHIFTL(G_RDPPIPESYNC, 24, 8);
    pkt->words.w1 = 0;
}

static inline void gDPSetScissor_(Gfx* pkt, u32 mode, s32 ulx, s32 uly, s32 lrx, s32 lry) {
    pkt->words.w0 = _SHIFTL(G_SETSCISSOR, 24, 8) | _SHIFTL(mode, 24, 2) | gpack_scissor_xy(ulx * 4, uly * 4);
    pkt->words.w1 = gpack_scissor_xy(lrx * 4, lry * 4);
}

static inline void gDPSetAlphaCompare_(Gfx* pkt, u32 mode) {
    pkt->words.w0 = 0xB9000002u;
    pkt->words.w1 = mode;
}

static inline void gDPSetCycleType_(Gfx* pkt, u32 mode) {
    pkt->words.w0 = 0xBA001402u;
    pkt->words.w1 = mode;
}

static inline void gDPSetColorDither_(Gfx* pkt, u32 mode) {
    pkt->words.w0 = 0xBA000602u;
    pkt->words.w1 = mode;
}

static inline void gDPSetAlphaDither_(Gfx* pkt, u32 mode) {
    pkt->words.w0 = 0xBA000402u;
    pkt->words.w1 = mode;
}

static inline void gDPSetRenderMode_(Gfx* pkt, u32 mode0, u32 mode1) {
    pkt->words.w0 = 0xB900031Du;
    pkt->words.w1 = mode0 | mode1;
}

static inline void gDPSetFillColor_(Gfx* pkt, u32 color) {
    pkt->words.w0 = _SHIFTL(G_SETFILLCOLOR, 24, 8);
    pkt->words.w1 = color;
}

static inline void gDPFillRectangle_(Gfx* pkt, s32 ulx, s32 uly, s32 lrx, s32 lry) {
    pkt->words.w0 = _SHIFTL(G_FILLRECT, 24, 8) | gpack_texrect_xy(lrx, lry);
    pkt->words.w1 = gpack_texrect_xy(ulx, uly);
}

static inline void gDPScisFillRectangle_(Gfx* pkt, s32 ulx, s32 uly, s32 lrx, s32 lry) {
    pkt->words.w0 = _SHIFTL(G_FILLRECT, 24, 8) | gpack_texrect_xy(MAX((s16)lrx, 0), MAX((s16)lry, 0));
    pkt->words.w1 = gpack_texrect_xy(MAX((s16)ulx, 0), MAX((s16)uly, 0));
}

static inline void gDPSetPrimColor_(Gfx* pkt, u32 m, u32 l, u32 r, u32 g, u32 b, u32 a) {
    (void)m;
    (void)l;
    pkt->words.w0 = _SHIFTL(G_SETPRIMCOLOR, 24, 8);
    pkt->words.w1 = _SHIFTL(r, 24, 8) | _SHIFTL(g, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8);
}

static inline void gDPSetCombineMode_(Gfx* pkt, u32 a, u32 b) {
    (void)a;
    (void)b;
    pkt->words.w0 = 0xFCFFFFFFu;
    pkt->words.w1 = 0xFFFDF6FBu;
}

static inline void gDPSetTexturePersp_(Gfx* pkt, u32 mode) {
    pkt->words.w0 = 0xBA001202u;
    pkt->words.w1 = mode;
}

static inline void gDPSetEnvColor_(Gfx* pkt, u32 r, u32 g, u32 b, u32 a) {
    pkt->words.w0 = _SHIFTL(G_SETENVCOLOR, 24, 8);
    pkt->words.w1 = _SHIFTL(r, 24, 8) | _SHIFTL(g, 16, 8) | _SHIFTL(b, 8, 8) | _SHIFTL(a, 0, 8);
}

static inline void gDPSetPrimDepth_(Gfx* pkt, s32 z, s32 dz) {
    pkt->words.w0 = _SHIFTL(G_SETPRIMDEPTH, 24, 8);
    pkt->words.w1 = _SHIFTL((u32)z, 16, 16) | _SHIFTL((u32)dz, 0, 16);
}

static inline void gDPSetTextureImage_(Gfx* pkt, u32 fmt, u32 siz, u32 width, const void* img) {
    u32 safe_width = (width == 0u) ? 1u : width;
    pkt->words.w0 = _SHIFTL(G_SETTIMG, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) | _SHIFTL(safe_width - 1u, 0, 12);
    pkt->words.w1 = (u32)(uintptr_t)img;
}

static inline void gDPSetTile_(Gfx* pkt, u32 fmt, u32 siz, u32 line, u32 tmem, u32 tile, u32 palette, u32 cmt, u32 maskt, u32 shiftt, u32 cms, u32 masks, u32 shifts) {
    pkt->words.w0 = _SHIFTL(G_SETTILE, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) | _SHIFTL(line, 9, 9) | _SHIFTL(tmem, 0, 9);
    pkt->words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL(palette, 20, 4) | _SHIFTL(cmt, 18, 2) | _SHIFTL(maskt, 14, 4) | _SHIFTL(shiftt, 10, 4) |
                    _SHIFTL(cms, 8, 2) | _SHIFTL(masks, 4, 4) | _SHIFTL(shifts, 0, 4);
}

static inline void gDPLoadSync_(Gfx* pkt) {
    pkt->words.w0 = _SHIFTL(G_LOADSYNC, 24, 8);
    pkt->words.w1 = 0;
}

static inline void gDPLoadBlock_(Gfx* pkt, u32 tile, u32 uls, u32 ult, u32 lrs, u32 dxt) {
    pkt->words.w0 = _SHIFTL(G_LOADBLOCK, 24, 8) | _SHIFTL(tile, 24, 3) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12);
    pkt->words.w1 = _SHIFTL(lrs, 12, 12) | _SHIFTL(dxt, 0, 12);
}

static inline void gDPLoadTile_(Gfx* pkt, u32 tile, u32 uls, u32 ult, u32 lrs, u32 lrt) {
    pkt->words.w0 = _SHIFTL(G_LOADTILE, 24, 8) | _SHIFTL(tile, 24, 3) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12);
    pkt->words.w1 = _SHIFTL(lrs, 12, 12) | _SHIFTL(lrt, 0, 12);
}

static inline void gDPSetTileSize_(Gfx* pkt, u32 tile, u32 uls, u32 ult, u32 lrs, u32 lrt) {
    pkt->words.w0 = _SHIFTL(G_SETTILESIZE, 24, 8) | _SHIFTL(tile, 24, 3) | _SHIFTL(uls, 12, 12) | _SHIFTL(ult, 0, 12);
    pkt->words.w1 = _SHIFTL(lrs, 12, 12) | _SHIFTL(lrt, 0, 12);
}

static inline void gSPDisplayList_(Gfx* pkt, const void* dl) {
    pkt->words.w0 = _SHIFTL(G_DL, 24, 8);
    pkt->words.w1 = (u32)(uintptr_t)dl;
}

static inline void gSPSprite2DBase_(Gfx* pkt, u32 sprite) {
    pkt->words.w0 = _SHIFTL(G_SPRITE2D_BASE, 24, 8) | _SHIFTL(24u, 0, 16);
    pkt->words.w1 = sprite;
}

static inline void gSPSprite2DScaleFlip_(Gfx* pkt, u32 sx, u32 sy, u32 fx, u32 fy) {
    pkt->words.w0 = _SHIFTL(G_SPRITE2D_SCALEFLIP, 24, 8) | _SHIFTL(fx, 8, 8) | _SHIFTL(fy, 0, 8);
    pkt->words.w1 = _SHIFTL(sx, 16, 16) | _SHIFTL(sy, 0, 16);
}

static inline void gSPSprite2DDraw_(Gfx* pkt, u32 px, u32 py) {
    pkt->words.w0 = _SHIFTL(G_SPRITE2D_DRAW, 24, 8);
    pkt->words.w1 = _SHIFTL(px, 16, 16) | _SHIFTL(py, 0, 16);
}

static inline void gDPSetColorImage_(Gfx* pkt, u32 fmt, u32 siz, u32 width, const void* img) {
    u32 safe_width = (width == 0u) ? 1u : width;
    pkt->words.w0 = _SHIFTL(G_SETCIMG, 24, 8) | _SHIFTL(fmt, 21, 3) | _SHIFTL(siz, 19, 2) | _SHIFTL(safe_width - 1u, 0, 12);
    pkt->words.w1 = (u32)(uintptr_t)img;
}

static inline void gSPTextureRectangle_(Gfx* pkt, s32 xl, s32 yl, s32 xh, s32 yh, u32 tile, s32 s, s32 t, s32 dsdx, s32 dtdy) {
    pkt[0].words.w0 = _SHIFTL(G_TEXRECT, 24, 8) | _SHIFTL((u32)xh, 12, 12) | _SHIFTL((u32)yh, 0, 12);
    pkt[0].words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL((u32)xl, 12, 12) | _SHIFTL((u32)yl, 0, 12);
    gImmp1_(&pkt[1], G_RDPHALF_1, _SHIFTL((u32)s, 16, 16) | _SHIFTL((u32)t, 0, 16));
    gImmp1_(&pkt[2], G_RDPHALF_2, _SHIFTL((u32)dsdx, 16, 16) | _SHIFTL((u32)dtdy, 0, 16));
}

static inline void gSPScisTextureRectangle_(
    Gfx* pkt,
    s32 xl,
    s32 yl,
    s32 xh,
    s32 yh,
    u32 tile,
    s32 s,
    s32 t,
    s32 dsdx,
    s32 dtdy
) {
    const s16 xl_s16 = (s16)xl;
    const s16 yl_s16 = (s16)yl;
    const s16 xh_s16 = (s16)xh;
    const s16 yh_s16 = (s16)yh;
    const s16 dsdx_s16 = (s16)dsdx;
    const s16 dtdy_s16 = (s16)dtdy;

    s32 s_adj = s;
    s32 t_adj = t;

    if (xl_s16 < 0) {
        const s32 delta = (((s32)xl_s16 * (s32)dsdx_s16) >> 7);
        s_adj -= (dsdx_s16 < 0) ? MAX(delta, 0) : MIN(delta, 0);
    }
    if (yl_s16 < 0) {
        const s32 delta = (((s32)yl_s16 * (s32)dtdy_s16) >> 7);
        t_adj -= (dtdy_s16 < 0) ? MAX(delta, 0) : MIN(delta, 0);
    }

    pkt[0].words.w0 = _SHIFTL(G_TEXRECT, 24, 8) | _SHIFTL((u32)MAX(xh_s16, 0), 12, 12) | _SHIFTL((u32)MAX(yh_s16, 0), 0, 12);
    pkt[0].words.w1 = _SHIFTL(tile, 24, 3) | _SHIFTL((u32)MAX(xl_s16, 0), 12, 12) | _SHIFTL((u32)MAX(yl_s16, 0), 0, 12);
    gImmp1_(&pkt[1], G_RDPHALF_1, _SHIFTL((u32)s_adj, 16, 16) | _SHIFTL((u32)t_adj, 0, 16));
    gImmp1_(&pkt[2], G_RDPHALF_2, _SHIFTL((u32)dsdx_s16, 16, 16) | _SHIFTL((u32)dtdy_s16, 0, 16));
}

#define gDPPipeSync(pkt) gDPPipeSync_(pkt)
#define gDPSetScissor(pkt, mode, ulx, uly, lrx, lry) gDPSetScissor_(pkt, mode, ulx, uly, lrx, lry)
#define gDPSetAlphaCompare(pkt, mode) gDPSetAlphaCompare_(pkt, mode)
#define gDPSetCycleType(pkt, mode) gDPSetCycleType_(pkt, mode)
#define gDPSetColorDither(pkt, mode) gDPSetColorDither_(pkt, mode)
#define gDPSetAlphaDither(pkt, mode) gDPSetAlphaDither_(pkt, mode)
#define gDPSetRenderMode(pkt, mode0, mode1) gDPSetRenderMode_(pkt, mode0, mode1)
#define gDPSetFillColor(pkt, color) gDPSetFillColor_(pkt, color)
#define gDPFillRectangle(pkt, ulx, uly, lrx, lry) gDPFillRectangle_(pkt, ulx, uly, lrx, lry)
#define gDPScisFillRectangle(pkt, ulx, uly, lrx, lry) gDPScisFillRectangle_(pkt, ulx, uly, lrx, lry)
#define gDPSetPrimColor(pkt, m, l, r, g, b, a) gDPSetPrimColor_(pkt, m, l, r, g, b, a)
#define gDPSetCombineMode(pkt, a, b) gDPSetCombineMode_(pkt, a, b)
#define gDPSetTexturePersp(pkt, mode) gDPSetTexturePersp_(pkt, mode)
#define gDPSetEnvColor(pkt, r, g, b, a) gDPSetEnvColor_(pkt, r, g, b, a)
#define gDPSetPrimDepth(pkt, z, dz) gDPSetPrimDepth_(pkt, z, dz)
#define gDPSetTextureImage(pkt, fmt, siz, width, img) gDPSetTextureImage_(pkt, fmt, siz, width, img)
#define gDPSetTile(pkt, fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts) gDPSetTile_(pkt, fmt, siz, line, tmem, tile, palette, cmt, maskt, shiftt, cms, masks, shifts)
#define gDPLoadSync(pkt) gDPLoadSync_(pkt)
#define gDPLoadBlock(pkt, tile, uls, ult, lrs, dxt) gDPLoadBlock_(pkt, tile, uls, ult, lrs, dxt)
#define gDPLoadTile(pkt, tile, uls, ult, lrs, lrt) gDPLoadTile_(pkt, tile, uls, ult, lrs, lrt)
#define gDPSetTileSize(pkt, tile, uls, ult, lrs, lrt) gDPSetTileSize_(pkt, tile, uls, ult, lrs, lrt)
#define gSPDisplayList(pkt, dl) gSPDisplayList_(pkt, dl)
#define gSPSprite2DBase(pkt, s) gSPSprite2DBase_(pkt, s)
#define gSPSprite2DScaleFlip(pkt, sx, sy, fx, fy) gSPSprite2DScaleFlip_(pkt, (u32)(sx), (u32)(sy), (u32)(fx), (u32)(fy))
#define gSPSprite2DDraw(pkt, px, py) gSPSprite2DDraw_(pkt, (u32)(px), (u32)(py))
#define gDPSetColorImage(pkt, fmt, siz, width, img) gDPSetColorImage_(pkt, fmt, siz, width, img)
#define gSPTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy) gSPTextureRectangle_(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy)
#define gSPScisTextureRectangle(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy) gSPScisTextureRectangle_(pkt, xl, yl, xh, yh, tile, s, t, dsdx, dtdy)

static inline void gSPMatrix(Gfx* pkt, const void* m, u32 p) {
    pkt->words.w0 = _SHIFTL(G_MTX, 24, 8) | _SHIFTL(p, 16, 8) | _SHIFTL((u32)sizeof(Mtx), 0, 16);
    pkt->words.w1 = (u32)(uintptr_t)m;
}

static inline void gSPPopMatrix_(Gfx* pkt, u32 mode) {
    (void)mode;
    pkt->words.w0 = 0xBD000000u;
    pkt->words.w1 = 0;
}

static inline void gSPPerspNormalize(Gfx* pkt, u16 s) {
    pkt->words.w0 = _SHIFTL(G_MOVEWORD, 24, 8) | _SHIFTL(0, 8, 16) | _SHIFTL(G_MW_PERSPNORM, 0, 8);
    pkt->words.w1 = (u32)s;
}

static inline void gSPViewport(Gfx* pkt, const void* vp) {
    pkt->words.w0 = 0x03800010u;
    pkt->words.w1 = (u32)(uintptr_t)vp;
}

static inline void gSPSegment(Gfx* pkt, u32 seg, const void* base) {
    pkt->words.w0 = _SHIFTL(G_MOVEWORD, 24, 8) | _SHIFTL(((seg & 0xFu) * 4u), 8, 16) | _SHIFTL(G_MW_SEGMENT, 0, 8);
    pkt->words.w1 = (u32)(uintptr_t)base;
}

void guPerspective(Mtx* m, u16* perspNorm, f32 fovy, f32 aspect, f32 near, f32 far, f32 scale);
void guScale(Mtx* m, f32 x, f32 y, f32 z);
void guTranslate(Mtx* m, f32 x, f32 y, f32 z);
void guLookAt(Mtx* m, f32 xEye, f32 yEye, f32 zEye, f32 xAt, f32 yAt, f32 zAt, f32 xUp, f32 yUp, f32 zUp);
void guSprite2DInit(void* sprite, void* sourceImagePointer, void* tlutPointer, int stride, int subImageWidth, int subImageHeight, int sourceImageType, int sourceImageBitSize, int sourceImageOffsetS, int sourceImageOffsetT);
void osViSetXScale(f32 scale);
void osViSetYScale(f32 scale);

u32 osVirtualToPhysical(const void* vaddr);

#define OS_K0_TO_PHYSICAL(x) (u32)(((char*)(x) - 0x80000000))

#define gSPPopMatrix(pkt, mode) gSPPopMatrix_(pkt, mode)

#endif
