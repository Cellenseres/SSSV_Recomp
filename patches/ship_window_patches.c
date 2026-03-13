#include "sssv_patch_common.h"

// The original ship-window path re-rendered world geometry under a second
// projection. The recomp keeps the normal world pass and drops that replay.
RECOMP_PATCH void render_ship_window_projection_replay(void) {
}
