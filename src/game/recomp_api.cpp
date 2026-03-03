#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "recompui/recompui.h"
#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

namespace {

constexpr float kBaseGameAspect = 4.0f / 3.0f;
constexpr float kExpandGameAspect = 16.0f / 9.0f;
constexpr float kAspectEpsilon = 1e-6f;
constexpr float kViScaleEpsilon = 1e-6f;
constexpr int16_t kBaseWidth = 320;
constexpr int16_t kBaseHeight = 240;
constexpr uint32_t kPatchDiagContext = 1u << 0;
constexpr uint32_t kPatchDiagProjView = 1u << 1;
constexpr uint32_t kPatchDiagTexRect = 1u << 2;
constexpr uint32_t kPatchDiagBillboard = 1u << 3;
constexpr uint32_t kPatchDiagWarn = 1u << 4;
constexpr uint32_t kPatchDiagIntro = 1u << 5;
constexpr uint32_t kPatchDiagAll = kPatchDiagContext | kPatchDiagProjView | kPatchDiagTexRect | kPatchDiagBillboard | kPatchDiagWarn | kPatchDiagIntro;
constexpr uint32_t kPatchDiagUnset = 0xFFFFFFFFu;
constexpr size_t kPatchTagLimit = 256;

inline bool vi_scale_is_one(float scale) {
    return std::fabs(scale - 1.0f) <= kViScaleEpsilon;
}

inline int16_t decode_s16_lo(uint32_t v) {
    return static_cast<int16_t>(v & 0xFFFFu);
}

inline int16_t decode_s16_hi(uint32_t v) {
    return static_cast<int16_t>((v >> 16) & 0xFFFFu);
}

std::string normalize_token(std::string token) {
    size_t start = 0;
    while ((start < token.size()) && std::isspace(static_cast<unsigned char>(token[start]))) {
        start++;
    }

    size_t end = token.size();
    while ((end > start) && std::isspace(static_cast<unsigned char>(token[end - 1]))) {
        end--;
    }

    std::string normalized = token.substr(start, end - start);
    for (char& c : normalized) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '-') {
            c = '_';
        }
    }

    return normalized;
}

bool parse_u32_value(const std::string& token, uint32_t& out_value) {
    if (token.empty()) {
        return false;
    }

    char* end_ptr = nullptr;
    const unsigned long raw = std::strtoul(token.c_str(), &end_ptr, 0);
    if ((end_ptr == token.c_str()) || (*end_ptr != '\0')) {
        return false;
    }

    out_value = static_cast<uint32_t>(raw);
    return true;
}

FILE* patch_trace_file() {
    static int initialized = 0;
    static FILE* file = nullptr;

    if (initialized == 0) {
        const char* path = std::getenv("SSSV_PATCH_TRACE_FILE");
        if ((path != nullptr) && (path[0] != '\0')) {
            file = std::fopen(path, "a");
            if (file != nullptr) {
                std::fprintf(stderr, "[SSSV:PATCH] trace file = %s\n", path);
            } else {
                std::fprintf(stderr, "[SSSV:PATCH] failed to open trace file \"%s\"\n", path);
            }
        }
        initialized = 1;
    }

    return file;
}

void patch_trace_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fflush(stderr);

    FILE* file = patch_trace_file();
    if (file != nullptr) {
        va_start(args, fmt);
        std::vfprintf(file, fmt, args);
        va_end(args);
        std::fflush(file);
    }
}

const char* patch_tag_name(uint32_t tag) {
    switch (tag) {
    case 1: return "set_screen_scaling";
    case 2: return "draw_rectangle";
    case 3: return "intro_frame";
    case 4: return "end_display_lists";
    case 5: return "world_perspective";
    case 6: return "depth_clear";
    case 7: return "draw_rectangle_fullclear_promote";
    case 8: return "draw_rectangle_border_skip";
    case 9: return "end_display_lists_vp";
    case 10: return "world_depth_clear_failsafe";
    case 11: return "ui_domain";
    case 12: return "osd_score";
    case 13: return "osd_timer";
    case 14: return "osd_text";
    case 15: return "credits";
    case 16: return "context_enter";
    case 17: return "context_exit";
    case 18: return "projection_id";
    case 19: return "billboard_id";
    case 20: return "id_collision";
    case 21: return "frame_begin";
    case 22: return "frame_end";
    case 23: return "camera_cut_skip";
    case 25: return "proj_view_snapshot";
    case 26: return "texrect_class";
    case 27: return "billboard_sample";
    case 28: return "energy_settimg_seen";
    case 29: return "energy_rect_emit";
    case 30: return "intro_viewport";
    case 31: return "intro_state";
    case 32: return "sprite2d_bg_scale";
    case 33: return "sprite2d_bg_call";
    case 34: return "depth_clear_bounds";
    case 35: return "sprite2d_viewport";
    case 36: return "intro_color_target";
    case 37: return "intro_scissor";
    default: return "unknown";
    }
}

uint32_t patch_tag_from_name(const std::string& name) {
    if (name == "set_screen_scaling") return 1u;
    if (name == "draw_rectangle") return 2u;
    if (name == "intro_frame") return 3u;
    if (name == "end_display_lists") return 4u;
    if (name == "world_perspective") return 5u;
    if (name == "depth_clear") return 6u;
    if (name == "draw_rectangle_fullclear_promote") return 7u;
    if (name == "draw_rectangle_border_skip") return 8u;
    if (name == "end_display_lists_vp") return 9u;
    if (name == "world_depth_clear_failsafe") return 10u;
    if (name == "ui_domain") return 11u;
    if (name == "osd_score") return 12u;
    if (name == "osd_timer") return 13u;
    if (name == "osd_text") return 14u;
    if (name == "credits") return 15u;
    if ((name == "context") || (name == "context_enter")) return 16u;
    if (name == "context_exit") return 17u;
    if ((name == "projection") || (name == "projection_id")) return 18u;
    if (name == "billboard_id") return 19u;
    if (name == "id_collision") return 20u;
    if (name == "frame_begin") return 21u;
    if (name == "frame_end") return 22u;
    if (name == "camera_cut_skip") return 23u;
    if ((name == "proj_view") || (name == "proj_view_snapshot")) return 25u;
    if (name == "texrect_class") return 26u;
    if (name == "billboard_sample") return 27u;
    if ((name == "energy") || (name == "energy_settimg_seen")) return 28u;
    if (name == "energy_rect_emit") return 29u;
    if (name == "intro_viewport") return 30u;
    if ((name == "intro_state") || (name == "intro_guard")) return 31u;
    if ((name == "sprite2d") || (name == "sprite2d_bg_scale")) return 32u;
    if ((name == "sprite2d_bg_call") || (name == "sprite2d_call")) return 33u;
    if ((name == "depth_clear_bounds") || (name == "depth_bounds")) return 34u;
    if ((name == "sprite2d_viewport") || (name == "sprite_viewport")) return 35u;
    if ((name == "intro_color_target") || (name == "color_target")) return 36u;
    if ((name == "intro_scissor") || (name == "scissor")) return 37u;
    return 0u;
}

uint32_t patch_diag_bit_from_name(const std::string& name) {
    if ((name == "context") || (name == "ctx")) return kPatchDiagContext;
    if ((name == "proj") || (name == "proj_view") || (name == "projection")) return kPatchDiagProjView;
    if ((name == "texrect") || (name == "tex")) return kPatchDiagTexRect;
    if ((name == "billboard") || (name == "bb")) return kPatchDiagBillboard;
    if ((name == "warn") || (name == "warning") || (name == "warnings")) return kPatchDiagWarn;
    if ((name == "intro") || (name == "title")) return kPatchDiagIntro;
    return 0u;
}

void for_each_env_token(const char* raw_text, const std::function<void(const std::string&)>& fn) {
    if ((raw_text == nullptr) || (raw_text[0] == '\0')) {
        return;
    }

    const char* cursor = raw_text;
    while (*cursor != '\0') {
        while ((*cursor != '\0') && (std::isspace(static_cast<unsigned char>(*cursor)) || (*cursor == ',') || (*cursor == ';') || (*cursor == '|') || (*cursor == '+'))) {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        const char* start = cursor;
        while ((*cursor != '\0') && !std::isspace(static_cast<unsigned char>(*cursor)) && (*cursor != ',') && (*cursor != ';') && (*cursor != '|') && (*cursor != '+')) {
            cursor++;
        }

        fn(std::string(start, cursor - start));
    }
}

uint32_t get_patch_diag_flags_override() {
    static int initialized = 0;
    static uint32_t cached_flags = kPatchDiagUnset;

    if (initialized == 0) {
        const char* env = std::getenv("SSSV_PATCH_DIAG_FLAGS");
        if ((env != nullptr) && (env[0] != '\0')) {
            uint32_t numeric = 0;
            std::string full = normalize_token(env);
            bool parsed = false;

            if (parse_u32_value(full, numeric)) {
                cached_flags = numeric & kPatchDiagAll;
                parsed = true;
            } else {
                uint32_t mask = 0;
                bool any = false;
                for_each_env_token(env, [&](const std::string& raw_token) {
                    const std::string token = normalize_token(raw_token);
                    uint32_t token_value = 0;

                    if ((token == "all") || (token == "*") || (token == "on")) {
                        mask = kPatchDiagAll;
                        any = true;
                        return;
                    }
                    if ((token == "none") || (token == "off")) {
                        mask = 0;
                        any = true;
                        return;
                    }

                    token_value = patch_diag_bit_from_name(token);
                    if (token_value != 0u) {
                        mask |= token_value;
                        any = true;
                        return;
                    }

                    if (parse_u32_value(token, token_value)) {
                        mask |= (token_value & kPatchDiagAll);
                        any = true;
                        return;
                    }

                    std::fprintf(stderr, "[SSSV:PATCH] unknown diag token \"%s\" in SSSV_PATCH_DIAG_FLAGS\n", token.c_str());
                });

                if (any) {
                    cached_flags = mask;
                    parsed = true;
                }
            }

            if (parsed) {
                std::fprintf(stderr, "[SSSV:PATCH] diag flags override = 0x%02X\n", cached_flags);
            } else {
                std::fprintf(stderr, "[SSSV:PATCH] ignored invalid SSSV_PATCH_DIAG_FLAGS=\"%s\"\n", env);
            }
        }

        initialized = 1;
    }

    return cached_flags;
}

bool patch_trace_tag_enabled(uint32_t tag) {
    static int initialized = 0;
    static bool use_filter = false;
    static std::array<uint8_t, kPatchTagLimit> enabled_tags{};

    if (initialized == 0) {
        const char* env = std::getenv("SSSV_PATCH_TRACE_TAGS");
        if ((env != nullptr) && (env[0] != '\0')) {
            uint32_t count = 0;
            for_each_env_token(env, [&](const std::string& raw_token) {
                const std::string token = normalize_token(raw_token);
                uint32_t tag_id = 0;

                if ((token == "all") || (token == "*") || (token == "on")) {
                    use_filter = false;
                    count = 0;
                    return;
                }
                if ((token == "none") || (token == "off")) {
                    use_filter = true;
                    return;
                }

                if (parse_u32_value(token, tag_id) || ((tag_id = patch_tag_from_name(token)) != 0u)) {
                    if (tag_id < kPatchTagLimit) {
                        if (enabled_tags[tag_id] == 0u) {
                            enabled_tags[tag_id] = 1u;
                            count++;
                        }
                        use_filter = true;
                    }
                    return;
                }

                std::fprintf(stderr, "[SSSV:PATCH] unknown tag token \"%s\" in SSSV_PATCH_TRACE_TAGS\n", token.c_str());
            });

            if (use_filter) {
                std::fprintf(stderr, "[SSSV:PATCH] tag filter active (%u tag(s))\n", count);
            }
        }

        initialized = 1;
    }

    if (!use_filter) {
        return true;
    }

    return (tag < kPatchTagLimit) && (enabled_tags[tag] != 0u);
}

const char* energy_reason_name(uint32_t reason) {
    switch (reason) {
    case 1: return "behind_near";
    case 2: return "depth_invalid";
    case 3: return "size_zero";
    case 4: return "rect_invalid";
    case 5: return "clip_reject";
    case 6: return "emitted";
    default: return "unknown";
    }
}

void print_energy_diag(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t source_bits = a;
    const uint32_t reason = c & 0xFFFFu;
    const uint32_t arg6 = (c >> 16) & 0xFFFFu;
    std::fprintf(
        stderr,
        "[SSSV:PATCH][ENERGY] settimg src_bits=0x%02X phys=0x%08X reason=%s(%u) arg6=%u\n",
        source_bits & 0xFFu,
        b,
        energy_reason_name(reason),
        reason,
        arg6
    );
}

void print_energy_rect_diag(uint32_t a, uint32_t b, uint32_t c) {
    const int16_t sx = decode_s16_hi(a);
    const int16_t sy = decode_s16_lo(a);
    const int16_t xl = decode_s16_hi(b);
    const int16_t yl = decode_s16_lo(b);
    const int16_t xh = decode_s16_hi(c);
    const int16_t yh = decode_s16_lo(c);
    std::fprintf(
        stderr,
        "[SSSV:PATCH][ENERGY] screen=(%d,%d) rect=(%d,%d)-(%d,%d)\n",
        sx, sy, xl, yl, xh, yh
    );
}

} // namespace

namespace sssv::aspect {

float get_target_aspect_ratio(float original_aspect) {
    ultramodern::renderer::GraphicsConfig graphics_config = ultramodern::renderer::get_graphics_config();
    int width = 0;
    int height = 0;
    recompui::get_window_size(width, height);

    switch (graphics_config.ar_option) {
    case ultramodern::renderer::AspectRatio::Original:
    default:
        return original_aspect;
    case ultramodern::renderer::AspectRatio::Expand:
        if (height <= 0) {
            return std::max(kExpandGameAspect, original_aspect);
        }
        return std::max(static_cast<float>(width) / static_cast<float>(height), original_aspect);
    }
}

int16_t compute_target_width_for_height(int16_t source_height) {
    const float target_aspect = get_target_aspect_ratio(kBaseGameAspect);
    if (target_aspect <= (kBaseGameAspect + kAspectEpsilon)) {
        return kBaseWidth;
    }

    const int16_t safe_height = (source_height > 0) ? source_height : kBaseHeight;
    int raw_width = static_cast<int>(std::lround(static_cast<float>(safe_height) * target_aspect));
    raw_width = std::clamp(raw_width, static_cast<int>(kBaseWidth), 0x7FFE);

    // Keep viewport symmetric around center.
    if ((raw_width & 1) != 0) {
        raw_width -= 1;
    }

    return static_cast<int16_t>(raw_width);
}

void export_target_aspect_ratio(uint8_t* rdram, recomp_context* ctx) {
    const float original = _arg<0, float>(rdram, ctx);
    _return(ctx, get_target_aspect_ratio(original));
}

} // namespace sssv::aspect

extern "C" void __ll_rshift_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    int64_t a = (static_cast<int64_t>(ctx->r4) << 32) | (static_cast<uint32_t>(ctx->r5));
    int64_t b = (static_cast<int64_t>(ctx->r6) << 32) | (static_cast<uint32_t>(ctx->r7));
    int64_t ret = a >> b;

    ctx->r2 = static_cast<int32_t>(ret >> 32);
    ctx->r3 = static_cast<int32_t>(ret);
}

extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = 11;
}

extern "C" void __osEnqueueThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    auto queue = static_cast<PTR(PTR(OSThread))>(ctx->r4);
    auto thread = static_cast<PTR(OSThread)>(ctx->r5);
    ultramodern::thread_queue_insert(PASS_RDRAM queue, thread);
    ctx->r2 = 0;
}

extern "C" void __osPopThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    auto queue = static_cast<PTR(PTR(OSThread))>(ctx->r4);
    PTR(OSThread) thread = ultramodern::thread_queue_pop(PASS_RDRAM queue);
    ctx->r2 = static_cast<int32_t>(thread);
}

extern "C" void sssv_osViSetXScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    const float scale = _arg<0, float>(rdram, ctx);
    if (vi_scale_is_one(scale)) {
        osViSetXScale(scale);
    }
}

extern "C" void sssv_osViSetYScale_recomp(uint8_t* rdram, recomp_context* ctx) {
    const float scale = _arg<0, float>(rdram, ctx);
    if (vi_scale_is_one(scale)) {
        osViSetYScale(scale);
    }
}

extern "C" void recomp_get_target_aspect_ratio(uint8_t* rdram, recomp_context* ctx) {
    sssv::aspect::export_target_aspect_ratio(rdram, ctx);
}

extern "C" void recomp_get_patch_diag_flags(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = static_cast<int32_t>(get_patch_diag_flags_override());
}

extern "C" void recomp_patch_debug_u32(uint8_t* rdram, recomp_context* ctx) {
#if defined(NDEBUG)
    (void)rdram;
    (void)ctx;
#else
    const uint32_t tag = static_cast<uint32_t>(_arg<0, int32_t>(rdram, ctx));
    const uint32_t a = static_cast<uint32_t>(_arg<1, int32_t>(rdram, ctx));
    const uint32_t b = static_cast<uint32_t>(_arg<2, int32_t>(rdram, ctx));
    const uint32_t c = static_cast<uint32_t>(_arg<3, int32_t>(rdram, ctx));

    if (!patch_trace_tag_enabled(tag)) {
        return;
    }

    const char* tag_name = patch_tag_name(tag);

    if (tag == 28u) {
        print_energy_diag(a, b, c);
        std::fflush(stderr);
        return;
    }
    if (tag == 29u) {
        print_energy_rect_diag(a, b, c);
        std::fflush(stderr);
        return;
    }
    if (tag == 20u) {
        patch_trace_printf(
            "[SSSV:PATCH] id_collision transform=0x%08X prev_key=0x%08X new_key=0x%08X\n",
            a, b, c
        );
        return;
    }
    if (tag == 7u || tag == 8u) {
        const int16_t x0 = static_cast<int16_t>(a >> 16);
        const int16_t y0 = static_cast<int16_t>(a & 0xFFFF);
        const int16_t x1 = static_cast<int16_t>(b >> 16);
        const int16_t y1 = static_cast<int16_t>(b & 0xFFFF);
        const uint8_t r = static_cast<uint8_t>(c >> 24);
        const uint8_t g = static_cast<uint8_t>((c >> 16) & 0xFFu);
        const uint8_t b8 = static_cast<uint8_t>((c >> 8) & 0xFFu);
        const uint8_t alpha = static_cast<uint8_t>(c & 0xFFu);
        patch_trace_printf(
            "[SSSV:PATCH] %s rect=(%d,%d)-(%d,%d) rgba=(%u,%u,%u,%u)\n",
            tag_name, x0, y0, x1, y1, r, g, b8, alpha
        );
        return;
    }
    if (tag == 30u) {
        const int16_t screen_w = static_cast<int16_t>(a >> 16);
        const int16_t screen_h = static_cast<int16_t>(a & 0xFFFF);
        const int16_t vscale_x4 = static_cast<int16_t>(b >> 16);
        const int16_t vtrans_x4 = static_cast<int16_t>(b & 0xFFFF);
        const int16_t vscale_y4 = static_cast<int16_t>(c >> 16);
        const int16_t vtrans_y4 = static_cast<int16_t>(c & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] intro_viewport screen=%dx%d vp_x4 scale=%d trans=%d vp_y4 scale=%d trans=%d\n",
            screen_w, screen_h, vscale_x4, vtrans_x4, vscale_y4, vtrans_y4
        );
        return;
    }
    if (tag == 31u) {
        const uint16_t frame = static_cast<uint16_t>(a >> 16);
        const uint8_t state = static_cast<uint8_t>((a >> 8) & 0xFFu);
        const uint8_t stage = static_cast<uint8_t>(a & 0xFFu);
        const int16_t screen_w = static_cast<int16_t>(b >> 16);
        const int16_t screen_h = static_cast<int16_t>(b & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] intro_state frame=%u state=%u stage=%u screen=%dx%d flags=0x%08X\n",
            frame, state, stage, screen_w, screen_h, c
        );
        return;
    }
    if (tag == 32u) {
        const int16_t target_w = static_cast<int16_t>(a);
        const int16_t effective_scale_x = static_cast<int16_t>(b >> 16);
        const int16_t effective_scale_y = static_cast<int16_t>(b & 0xFFFF);
        const int16_t draw_x = static_cast<int16_t>(c >> 16);
        const int16_t draw_y = static_cast<int16_t>(c & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] sprite2d_bg_scale target_w=%d scale=%dx%d draw=%d,%d\n",
            target_w, effective_scale_x, effective_scale_y, draw_x, draw_y
        );
        return;
    }
    if (tag == 33u) {
        const int16_t width = static_cast<int16_t>(a >> 16);
        const int16_t height = static_cast<int16_t>(a & 0xFFFF);
        const int16_t scale_x = static_cast<int16_t>(b >> 16);
        const int16_t scale_y = static_cast<int16_t>(b & 0xFFFF);
        const uint16_t z = static_cast<uint16_t>(c >> 16);
        const uint16_t flags = static_cast<uint16_t>(c & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] sprite2d_bg_call size=%dx%d scale=%dx%d z=0x%04X flags=0x%04X\n",
            width, height, scale_x, scale_y, z, flags
        );
        return;
    }
    if (tag == 34u) {
        const int16_t clear_w = static_cast<int16_t>(a >> 16);
        const int16_t clear_h = static_cast<int16_t>(a & 0xFFFF);
        const int16_t fill_w = static_cast<int16_t>(b >> 16);
        const int16_t fill_h = static_cast<int16_t>(b & 0xFFFF);
        const uint16_t ctx = static_cast<uint16_t>(c >> 16);
        const uint16_t flags = static_cast<uint16_t>(c & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] depth_clear_bounds scissor=%dx%d fill=%dx%d ctx=%u flags=0x%04X\n",
            clear_w, clear_h, fill_w, fill_h, ctx, flags
        );
        return;
    }
    if (tag == 35u) {
        const uint16_t call_id = static_cast<uint16_t>(a >> 16);
        const uint16_t ctx = static_cast<uint16_t>(a & 0xFFFF);
        const int16_t vscale_x4 = static_cast<int16_t>(b >> 16);
        const int16_t vtrans_x4 = static_cast<int16_t>(b & 0xFFFF);
        const int16_t vscale_y4 = static_cast<int16_t>(c >> 16);
        const int16_t vtrans_y4 = static_cast<int16_t>(c & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] sprite2d_viewport call=%u ctx=%u vp_x4 scale=%d trans=%d vp_y4 scale=%d trans=%d\n",
            call_id, ctx, vscale_x4, vtrans_x4, vscale_y4, vtrans_y4
        );
        return;
    }
    if (tag == 36u) {
        const uint16_t stage = static_cast<uint16_t>(a >> 16);
        const uint16_t width = static_cast<uint16_t>(a & 0xFFFFu);
        const uint32_t addr = b;
        const uint16_t is_depth = static_cast<uint16_t>(c >> 16);
        const uint16_t ctx = static_cast<uint16_t>(c & 0xFFFFu);
        patch_trace_printf(
            "[SSSV:PATCH] intro_color_target stage=%u width=%u addr=0x%08X depth=%u ctx=%u\n",
            stage, width, addr, is_depth, ctx
        );
        return;
    }
    if (tag == 37u) {
        const uint16_t stage = static_cast<uint16_t>(a >> 16);
        const uint16_t ctx = static_cast<uint16_t>(a & 0xFFFFu);
        const int16_t ulx = static_cast<int16_t>(b >> 16);
        const int16_t uly = static_cast<int16_t>(b & 0xFFFF);
        const int16_t lrx = static_cast<int16_t>(c >> 16);
        const int16_t lry = static_cast<int16_t>(c & 0xFFFF);
        patch_trace_printf(
            "[SSSV:PATCH] intro_scissor stage=%u ctx=%u rect=(%d,%d)-(%d,%d)\n",
            stage, ctx, ulx, uly, lrx, lry
        );
        return;
    }

    patch_trace_printf(
        "[SSSV:PATCH] %s tag=%u a=%u b=%u c=%u\n",
        tag_name,
        tag,
        a,
        b,
        c
    );
#endif
}
