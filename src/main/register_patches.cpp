#include "sssv_game.h"
#include "../../RecompiledPatches/patches_bin.h"
#include "../../RecompiledPatches/recomp_overlays.inl"

#include "librecomp/overlays.hpp"
#include "librecomp/game.hpp"

extern "C" void recomp_get_target_aspect_ratio(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_patch_debug_u32(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_get_patch_diag_flags(uint8_t* rdram, recomp_context* ctx);

namespace sssv {

namespace {

using LoadedPatchFn = void (*)(uint8_t* rdram, recomp_context* ctx);

struct LoadedPatchFunction {
    uint32_t address;
    LoadedPatchFn function;
};

constexpr LoadedPatchFunction kLoadedPatchFunctions[] = {
    { 0x8F000000, recomp_get_target_aspect_ratio },
    { 0x8F000004, recomp_patch_debug_u32 },
    { 0x8F000008, recomp_get_patch_diag_flags },
};

void register_loaded_patch_functions() {
    for (const LoadedPatchFunction& entry : kLoadedPatchFunctions) {
        recomp::overlays::add_loaded_function(entry.address, entry.function);
    }
}

} // namespace

void restore_runtime_patch_symbols() {
    register_loaded_patch_functions();
}

void register_patches() {
    // These absolute symbols are needed both during patch registration and again after
    // librecomp reinitializes overlay state on game start.
    restore_runtime_patch_symbols();
    recomp::overlays::register_manual_patch_symbols(manual_patch_symbols);
    recomp::overlays::register_patches(
        reinterpret_cast<const char*>(sssv_patches_bin),
        sizeof(sssv_patches_bin),
        section_table,
        ARRLEN(section_table)
    );
    recomp::overlays::register_base_exports(export_table);
    recomp::overlays::register_base_events(event_names);
}

} // namespace sssv
