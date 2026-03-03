#ifndef __SSSV_PATCH_INPUT_H__
#define __SSSV_PATCH_INPUT_H__

#include <ultra64.h>

#ifndef SSSV_OSCONTPAD_DEFINED
#define SSSV_OSCONTPAD_DEFINED
typedef struct {
    u16 button;
    s8 stick_x;
    s8 stick_y;
} OSContPad;
#endif

#ifndef CONT_A
#define CONT_A 0x8000
#endif
#ifndef CONT_B
#define CONT_B 0x4000
#endif
#ifndef Z_TRIG
#define Z_TRIG 0x2000
#endif
#ifndef CONT_UP
#define CONT_UP 0x0800
#endif
#ifndef CONT_DOWN
#define CONT_DOWN 0x0400
#endif
#ifndef CONT_LEFT
#define CONT_LEFT 0x0200
#endif
#ifndef CONT_RIGHT
#define CONT_RIGHT 0x0100
#endif
#ifndef L_TRIG
#define L_TRIG 0x0020
#endif
#ifndef R_TRIG
#define R_TRIG 0x0010
#endif
#ifndef U_CBUTTONS
#define U_CBUTTONS 0x0008
#endif
#ifndef D_CBUTTONS
#define D_CBUTTONS 0x0004
#endif
#ifndef L_CBUTTONS
#define L_CBUTTONS 0x0002
#endif
#ifndef R_CBUTTONS
#define R_CBUTTONS 0x0001
#endif

#endif
