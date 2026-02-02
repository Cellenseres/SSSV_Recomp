#include "sssv_launcher.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

struct KeyframeRot {
    float seconds;
    float deg;
};

struct Keyframe2D {
    float seconds;
    float x;
    float y;
};

enum class InterpolationMethod {
    Linear,
    Smootherstep
};

struct AnimationData {
    uint32_t keyframe_index = 0;
    uint32_t loop_keyframe_index = UINT32_MAX;
    float seconds = 0.0f;
    InterpolationMethod interpolation_method = InterpolationMethod::Linear;
};

struct AnimatedSvg {
    recompui::Element *svg = nullptr;
    std::vector<Keyframe2D> position_keyframes;
    std::vector<Keyframe2D> scale_keyframes;
    std::vector<KeyframeRot> rotation_keyframes;
    AnimationData position_animation;
    AnimationData scale_animation;
    AnimationData rotation_animation;
    float width = 0;
    float height = 0;
};

// Starfield: dotted trails, parallax (size/speed by depth)
static constexpr int STARFIELD_NUM_STARS = 192;
static constexpr int STARFIELD_TRAIL_DOTS = 8;
static constexpr float STARFIELD_TRAIL_SPACING_DP = 10.0f;
static constexpr float STARFIELD_BASE_SPEED_DP = 45.0f;
static constexpr float STARFIELD_SPEED_RANGE_DP = 200.0f;
// Far stars (depth 0) use MIN, near stars (depth 1) use MAX – keep MIN small so distant stars look small.
static constexpr float STARFIELD_SIZE_MIN_DP = 1.2f;
static constexpr float STARFIELD_SIZE_MAX_DP = 7.2f;
// Minimum displayed dot size (dp); allow small dots so far stars don’t get clamped too large.
static constexpr float STARFIELD_DOT_SIZE_MIN_DP = 0.35f;
// Trail length scale: far stars (depth 0) use this fraction of base spacing → shorter trails; near stars (depth 1) use full spacing.
static constexpr float STARFIELD_TRAIL_LENGTH_FAR = 0.2f;
static constexpr float STARFIELD_TRAIL_OPACITY_MAX = 1.0f;
static constexpr float STARFIELD_TRAIL_OPACITY_MIN = 0.18f;

// Opacity for trail dot index t (0 = front, TRAIL_DOTS-1 = back); linear from max to min.
static float starfield_trail_opacity(int t) {
    if (STARFIELD_TRAIL_DOTS <= 1) return STARFIELD_TRAIL_OPACITY_MAX;
    return STARFIELD_TRAIL_OPACITY_MAX + (STARFIELD_TRAIL_OPACITY_MIN - STARFIELD_TRAIL_OPACITY_MAX) * (float)t / (float)(STARFIELD_TRAIL_DOTS - 1);
}

// Size scale for trail dot t (0 = front = 1.0, TRAIL_DOTS-1 = back = min); works for any TRAIL_DOTS count.
static constexpr float STARFIELD_TRAIL_DOT_SCALE_MIN = 0.2f;
static float starfield_trail_dot_scale(int t) {
    if (STARFIELD_TRAIL_DOTS <= 1) return 1.0f;
    return 1.0f - (1.0f - STARFIELD_TRAIL_DOT_SCALE_MIN) * (float)t / (float)(STARFIELD_TRAIL_DOTS - 1);
}

struct StarfieldStar {
    float x = 0.0f;
    float y = 0.0f;
    float speed_dp = 0.0f;
    float size_dp = 0.0f;
    float depth = 0.0f;  // 0 = far (short trail), 1 = near (long trail)
    recompui::Element* dots[STARFIELD_TRAIL_DOTS] = {};
};

struct LauncherContext {
    AnimatedSvg banjo_svg;
    AnimatedSvg kazooie_svg;
    AnimatedSvg jiggy_color_svg;
    AnimatedSvg jiggy_shine_svg;
    AnimatedSvg jiggy_hole_svg;
    AnimatedSvg logo_svg;
    std::array<AnimatedSvg, 4> cloud_svgs;
    recompui::Element* starfield_wrapper = nullptr;
    std::vector<StarfieldStar> starfield_stars;
    recompui::Element* wrapper = nullptr;
    float wrapper_phase = -1.0f;
    std::chrono::steady_clock::time_point last_update_time;
    float seconds = 0.0f;
    bool started = false;
    bool options_enabled = false;
    std::atomic<bool> animation_skipped = false;
    std::atomic<bool> skip_animation_next_update = false;
};

static LauncherContext launcher_context;

float interpolate_value(float a, float b, float t, InterpolationMethod method) {
    switch (method) {
    case InterpolationMethod::Smootherstep:
        return a + (b - a) * (t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f));
    case InterpolationMethod::Linear:
    default:
        return a + (b - a) * t;
    }
}

void calculate_rot_from_keyframes(const std::vector<KeyframeRot> &kf, AnimationData &an, float delta_time, float &deg) {
    if (kf.empty()) {
        return;
    }

    an.seconds += delta_time;

    while ((an.keyframe_index < (kf.size() - 1) && (an.seconds >= kf[an.keyframe_index + 1].seconds))) {
        an.keyframe_index++;
    }

    if (an.keyframe_index >= (kf.size() - 1)) {
        deg = kf[an.keyframe_index].deg;
    }
    else {
        float t = (an.seconds - kf[an.keyframe_index].seconds) / (kf[an.keyframe_index + 1].seconds - kf[an.keyframe_index].seconds);
        deg = interpolate_value(kf[an.keyframe_index].deg, kf[an.keyframe_index + 1].deg, t, an.interpolation_method);
    }
}

void calculate_2d_from_keyframes(const std::vector<Keyframe2D> &kf, AnimationData &an, float delta_time, float &x, float &y) {
    if (kf.empty()) {
        return;
    }

    an.seconds += delta_time;

    while ((an.keyframe_index < (kf.size() - 1) && (an.seconds >= kf[an.keyframe_index + 1].seconds))) {
        an.keyframe_index++;
    }

    if ((an.loop_keyframe_index != UINT32_MAX) && (an.keyframe_index >= (kf.size() - 1))) {
        an.seconds = kf[an.loop_keyframe_index].seconds + (an.seconds - kf[an.keyframe_index].seconds);
        an.keyframe_index = an.loop_keyframe_index;
    }

    if (an.keyframe_index >= (kf.size() - 1)) {
        x = kf[an.keyframe_index].x;
        y = kf[an.keyframe_index].y;
    }
    else {
        float t = (an.seconds - kf[an.keyframe_index].seconds) / (kf[an.keyframe_index + 1].seconds - kf[an.keyframe_index].seconds);
        x = interpolate_value(kf[an.keyframe_index].x, kf[an.keyframe_index + 1].x, t, an.interpolation_method);
        y = interpolate_value(kf[an.keyframe_index].y, kf[an.keyframe_index + 1].y, t, an.interpolation_method);
    }
}

AnimatedSvg create_animated_svg(recompui::ContextId context, recompui::Element *parent, const std::string &svg_path, float width, float height) {
    AnimatedSvg animated_svg;
    animated_svg.width = width;
    animated_svg.height = height;
    animated_svg.svg = context.create_element<recompui::Svg>(parent, svg_path);
    animated_svg.svg->set_position(recompui::Position::Absolute);
    animated_svg.svg->set_width(width, recompui::Unit::Dp);
    animated_svg.svg->set_height(height, recompui::Unit::Dp);
    return animated_svg;
}

void update_animated_svg(AnimatedSvg &animated_svg, float delta_time, float bg_width, float bg_height) {
    float position_x = 0.0f, position_y = 0.0f;
    float scale_x = 1.0f, scale_y = 1.0f;
    float rotation_degrees = 0.0f;
    calculate_2d_from_keyframes(animated_svg.position_keyframes, animated_svg.position_animation, delta_time, position_x, position_y);
    calculate_2d_from_keyframes(animated_svg.scale_keyframes, animated_svg.scale_animation, delta_time, scale_x, scale_y);
    calculate_rot_from_keyframes(animated_svg.rotation_keyframes, animated_svg.rotation_animation, delta_time, rotation_degrees);
    animated_svg.svg->set_translate_2D(position_x + bg_width / 2.0f - animated_svg.width / 2.0f, position_y + bg_height / 2.0f - animated_svg.height / 2.0f);
    animated_svg.svg->set_scale_2D(scale_x, scale_y);
    animated_svg.svg->set_rotation(rotation_degrees);
}

bool check_skip_input(SDL_Event* event) {
    switch (event->type) {
    case SDL_KEYDOWN:
        return event->key.keysym.scancode == SDL_SCANCODE_ESCAPE ||
            event->key.keysym.scancode == SDL_SCANCODE_SPACE ||
            (event->key.keysym.scancode == SDL_SCANCODE_RETURN && (event->key.keysym.mod & (KMOD_LALT | KMOD_RALT)) == KMOD_NONE);
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_MOUSEBUTTONDOWN:
        return true;
    default:
        return false;
    }
}

int launcher_event_watch(void* userdata, SDL_Event* event) {
    (void)userdata;
    if (!launcher_context.animation_skipped.load() && check_skip_input(event)) {
        launcher_context.animation_skipped.store(true);
        launcher_context.skip_animation_next_update.store(true);
        return 0;
    }
    else {
        return 1;
    }
}

const float jiggy_scale_anim_start = 0.0f;
const float jiggy_scale_anim_length = 0.75f;
const float jiggy_scale_anim_end = jiggy_scale_anim_start + jiggy_scale_anim_length;
const float jiggy_move_over_start = jiggy_scale_anim_end + 0.5f;
const float jiggy_move_over_length = 0.75f;
const float jiggy_move_over_end = jiggy_move_over_start + jiggy_move_over_length;
const float jiggy_shine_start = jiggy_move_over_end + 0.6f;
const float jiggy_shine_length = 0.8f;

const float animation_skip_time = 10.0f;

static void starfield_respawn(StarfieldStar& star, float bg_width, float bg_height, bool initial) {
    float cx = bg_width * 0.5f;
    if (initial) {
        star.x = (float)(std::rand() % (int)(bg_width + 1)) - cx;
    } else {
        star.x = cx + (float)(std::rand() % (int)(bg_width * 0.4f + 1));
    }
    star.y = (float)(std::rand() % (int)(bg_height + 1)) - bg_height * 0.5f;
    star.depth = (float)(std::rand() % 1000) / 1000.0f;
    star.speed_dp = STARFIELD_BASE_SPEED_DP + star.depth * STARFIELD_SPEED_RANGE_DP;
    star.size_dp = STARFIELD_SIZE_MIN_DP + star.depth * (STARFIELD_SIZE_MAX_DP - STARFIELD_SIZE_MIN_DP);
}

static void starfield_create_layer(recompui::ContextId context, recompui::Element* background_container, float bg_width, float bg_height) {
    launcher_context.starfield_wrapper = context.create_element<recompui::Element>(background_container, 0);
    launcher_context.starfield_wrapper->set_position(recompui::Position::Absolute);
    launcher_context.starfield_wrapper->set_width(100, recompui::Unit::Percent);
    launcher_context.starfield_wrapper->set_height(100, recompui::Unit::Percent);
    launcher_context.starfield_wrapper->set_left(0);
    launcher_context.starfield_wrapper->set_top(0);

    launcher_context.starfield_stars.resize(STARFIELD_NUM_STARS);
    for (int i = 0; i < STARFIELD_NUM_STARS; i++) {
        StarfieldStar& star = launcher_context.starfield_stars[i];
        starfield_respawn(star, bg_width, bg_height, true);
        for (int t = 0; t < STARFIELD_TRAIL_DOTS; t++) {
            recompui::Element* dot = context.create_element<recompui::Element>(launcher_context.starfield_wrapper, 0);
            dot->set_position(recompui::Position::Absolute);
            float trail_scale = starfield_trail_dot_scale(t);
            float size = star.size_dp * trail_scale;
            if (size < STARFIELD_DOT_SIZE_MIN_DP) size = STARFIELD_DOT_SIZE_MIN_DP;
            dot->set_width(size, recompui::Unit::Dp);
            dot->set_height(size, recompui::Unit::Dp);
            dot->set_border_radius(size * 0.5f, recompui::Unit::Dp);
            dot->set_background_color(recompui::Color{ 255, 255, 255, (uint8_t)(255 * starfield_trail_opacity(t)) });
            star.dots[t] = dot;
        }
    }
}

// Trail spacing scales with depth: far stars (depth 0) get shorter trails, near stars (depth 1) get full length.
static float starfield_trail_spacing_for_star(const StarfieldStar& star) {
    return STARFIELD_TRAIL_SPACING_DP * (STARFIELD_TRAIL_LENGTH_FAR + star.depth * (1.0f - STARFIELD_TRAIL_LENGTH_FAR));
}

static void starfield_update(float delta_time, float bg_width, float bg_height) {
    if (!launcher_context.starfield_wrapper || launcher_context.starfield_stars.empty()) return;
    float cx = bg_width * 0.5f;
    float cy = bg_height * 0.5f;

    for (StarfieldStar& star : launcher_context.starfield_stars) {
        star.x -= star.speed_dp * delta_time;
        float spacing = starfield_trail_spacing_for_star(star);
        float trail_length_dp = spacing * (float)(STARFIELD_TRAIL_DOTS - 1);
        float left_edge = -cx - trail_length_dp - 20.0f;
        if (star.x < left_edge) {
            starfield_respawn(star, bg_width, bg_height, false);
            spacing = starfield_trail_spacing_for_star(star);
        }
        for (int t = 0; t < STARFIELD_TRAIL_DOTS; t++) {
            float dx = star.x + (float)t * spacing;
            float trail_scale = starfield_trail_dot_scale(t);
            float dot_size = star.size_dp * trail_scale;
            if (dot_size < STARFIELD_DOT_SIZE_MIN_DP) dot_size = STARFIELD_DOT_SIZE_MIN_DP;
            float dot_x = cx + dx - dot_size * 0.5f;
            float dot_y = cy + star.y - dot_size * 0.5f;
            star.dots[t]->set_width(dot_size, recompui::Unit::Dp);
            star.dots[t]->set_height(dot_size, recompui::Unit::Dp);
            star.dots[t]->set_border_radius(dot_size * 0.5f, recompui::Unit::Dp);
            star.dots[t]->set_translate_2D(dot_x, dot_y, recompui::Unit::Dp);
        }
    }
}

void sssv::launcher_animation_setup(recompui::LauncherMenu *menu) {
    auto context = recompui::get_current_context();
    recompui::Element* background_container = menu->get_background_container();
    background_container->set_background_color({ 0, 0, 0, 255 });

    std::srand((unsigned)std::time(nullptr));
    float initial_bg_width = 1920.0f;
    float initial_bg_height = 1080.0f;
    starfield_create_layer(context, background_container, initial_bg_width, initial_bg_height);

    launcher_context.wrapper = context.create_element<recompui::Element>(background_container, 0);
    launcher_context.wrapper->set_position(recompui::Position::Absolute);
    launcher_context.wrapper->set_width(100, recompui::Unit::Percent);
    launcher_context.wrapper->set_height(100, recompui::Unit::Percent);
    launcher_context.wrapper->set_top(0);

    // Disable and hide the options.
    for (auto option : menu->get_game_options_menu()->get_options()) {
        option->set_font_family("Comic Sans");
        option->set_enabled(false);
        option->set_opacity(0.0f);
        option->set_padding(24.0f);
        auto label = option->get_label();
        label->set_font_size(56.0f);
        label->set_letter_spacing(4.0f);
    }

    // The creation order of these is important.
    // --- Jiggy, Banjo, Kazooie, Clouds: auskommentiert, nur Logo + Starfield aktiv ---
    // launcher_context.jiggy_color_svg = create_animated_svg(context, launcher_context.wrapper, "JiggyColor.svg", 1054.0f, 1044.0f);
    // launcher_context.jiggy_shine_svg = create_animated_svg(context, launcher_context.wrapper, "JiggyShine.svg", 219.0f, 1080.0f);
    // launcher_context.jiggy_hole_svg = create_animated_svg(context, launcher_context.wrapper, "JiggyHole.svg", 2180.0f, 2160.0f);
    // launcher_context.banjo_svg = create_animated_svg(context, launcher_context.wrapper, "Banjo.svg", 649.0f, 622.0f);
    // launcher_context.kazooie_svg = create_animated_svg(context, launcher_context.wrapper, "Kazooie.svg", 626.0f, 774.0f);
    // launcher_context.cloud_svgs[0] = create_animated_svg(context, background_container, "Cloud1.svg", 461.0f, 154.0f);
    // launcher_context.cloud_svgs[1] = create_animated_svg(context, background_container, "Cloud2.svg", 461.0f, 167.0f);
    // launcher_context.cloud_svgs[2] = create_animated_svg(context, background_container, "Cloud3.svg", 295.0f, 167.0f);
    // launcher_context.cloud_svgs[3] = create_animated_svg(context, background_container, "Cloud1.svg", 461.0f, 154.0f);

    launcher_context.logo_svg = create_animated_svg(context, background_container, "Logo.svg", 6187.0f * 0.125f, 2625.0f * 0.125f);

    // --- Jiggy/Banjo/Kazooie keyframes: auskommentiert ---
    // launcher_context.jiggy_hole_svg.position_keyframes = { ... };
    // launcher_context.jiggy_hole_svg.scale_keyframes = { ... };
    // launcher_context.jiggy_hole_svg.rotation_keyframes = { ... };
    // launcher_context.jiggy_color_svg.* = ... (copy from hole)
    // launcher_context.jiggy_shine_svg.position_keyframes = { ... };
    // launcher_context.banjo_svg.position_keyframes = { ... };
    // launcher_context.banjo_svg.scale_keyframes = { ... };
    // launcher_context.banjo_svg.rotation_keyframes = { ... };
    // launcher_context.kazooie_svg.* = ... (mirror banjo)

    // Animate the logo.
    launcher_context.logo_svg.position_keyframes = {
        { 0.0f, 0.0f, -900.0f },
        { 1.0f, 0.0f, -900.0f },
        { 2.0f, 0.0f, -365.0f },
    };

    launcher_context.logo_svg.position_animation.interpolation_method = InterpolationMethod::Smootherstep;

    // --- Wolken-Keyframes: auskommentiert ---
    // launcher_context.cloud_svgs[0..3].position_keyframes / scale_keyframes / loop_keyframe_index

    // Install an event watch to skip the launcher animation if a keyboard, mouse or controller input is detected.
    SDL_AddEventWatch(&launcher_event_watch, nullptr);
}

void sssv::launcher_animation_update(recompui::LauncherMenu *menu) {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    float delta_time = launcher_context.started ? std::chrono::duration_cast<std::chrono::milliseconds>(now - launcher_context.last_update_time).count() / 1000.0f : 0.0f;
    if (launcher_context.skip_animation_next_update.exchange(false)) {
        delta_time = std::max(animation_skip_time - launcher_context.seconds, 0.0f);
    }

    launcher_context.seconds += delta_time;
    launcher_context.last_update_time = now;
    launcher_context.started = true;

    recompui::Element* background_container = menu->get_background_container();
    float dp_to_pixel_ratio = background_container->get_dp_to_pixel_ratio();
    float bg_width = background_container->get_client_width() / dp_to_pixel_ratio;
    float bg_height = background_container->get_client_height() / dp_to_pixel_ratio;

    starfield_update(delta_time, bg_width, bg_height);

    // Banjo, Kazooie, Jiggy, Wolken: auskommentiert
    // update_animated_svg(launcher_context.banjo_svg, ...);
    // update_animated_svg(launcher_context.kazooie_svg, ...);
    // update_animated_svg(launcher_context.jiggy_color_svg, ...);
    // update_animated_svg(launcher_context.jiggy_shine_svg, ...);
    // update_animated_svg(launcher_context.jiggy_hole_svg, ...);
    update_animated_svg(launcher_context.logo_svg, delta_time, bg_width, bg_height);
    // for (size_t i = 0; i < launcher_context.cloud_svgs.size(); i++) { update_animated_svg(launcher_context.cloud_svgs[i], ...); }

    float wrapper_phase = std::clamp((launcher_context.seconds - jiggy_move_over_start) / (jiggy_move_over_end - jiggy_move_over_start), 0.0f, 1.0f);
    if (wrapper_phase != launcher_context.wrapper_phase) {
        float x_translation = interpolate_value(0, 1440 * -0.2f, wrapper_phase, InterpolationMethod::Smootherstep);
        launcher_context.wrapper->set_translate_2D(x_translation, 0, recompui::Unit::Dp);
        float y_translation = interpolate_value(0, sssv::launcher_options_top_offset, wrapper_phase, InterpolationMethod::Smootherstep);
        launcher_context.wrapper->set_top(y_translation);
        float scale = interpolate_value(1, 0.666f, wrapper_phase, InterpolationMethod::Smootherstep);
        launcher_context.wrapper->set_scale_2D(scale, scale);

        float game_option_menu_opacity = interpolate_value(0, 1.0f, wrapper_phase, InterpolationMethod::Smootherstep);
        for (auto option : menu->get_game_options_menu()->get_options()) {
            option->set_opacity(game_option_menu_opacity);
        }

        float game_option_menu_right = interpolate_value(sssv::launcher_options_right_position_start, sssv::launcher_options_right_position_end, wrapper_phase, InterpolationMethod::Smootherstep);
        menu->get_game_options_menu()->set_right(game_option_menu_right);

        launcher_context.wrapper_phase = wrapper_phase;
    }

    if (!launcher_context.options_enabled && launcher_context.seconds >= jiggy_move_over_end) {
        SDL_DelEventWatch(&launcher_event_watch, nullptr);

        for (auto option : menu->get_game_options_menu()->get_options()) {
            option->set_enabled(true);
            option->set_opacity(1.0f);
        }

        launcher_context.options_enabled = true;
    }
}
