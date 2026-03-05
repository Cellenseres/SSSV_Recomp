#include "sssv_launcher.h"

#include "cs_sdk/launcher_model3d.h"
#include "cs_sdk/ui_bridge.h"
#include "recompui/config.h"
#include "elements/ui_element.h"
#include "elements/ui_image.h"
#include "elements/ui_modal.h"
#include "elements/ui_svg.h"
#include "util/file.h"
#include "ultramodern/ultramodern.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
class DeterministicRng {
public:
    explicit DeterministicRng(uint32_t seed) : state_(seed) {}

    void reset(uint32_t seed) {
        state_ = seed;
    }

    uint32_t next_u32() {
        // Xorshift32 is sufficient here and keeps the sequence stable across runs.
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    float next_unit_float() {
        return static_cast<float>(next_u32() & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    float next_range(float min_value, float max_value) {
        return min_value + (max_value - min_value) * next_unit_float();
    }

private:
    uint32_t state_;
};

constexpr uint32_t kAnimationSeed = 0x53535356u;
constexpr size_t kStarCount = 128;
constexpr size_t kTrailDots = 6;
constexpr float kTrailSpacingDp = 11.0f;
constexpr float kBaseSpeedDp = 42.0f;
constexpr float kSpeedRangeDp = 170.0f;
constexpr float kSizeMinDp = 1.1f;
constexpr float kSizeMaxDp = 6.4f;
constexpr float kDotSizeMinDp = 0.4f;
constexpr float kTrailLengthFar = 0.28f;
constexpr float kTrailScaleMin = 0.24f;
constexpr float kTrailOpacityMin = 0.16f;
constexpr float kTrailOpacityMax = 0.92f;
constexpr float kFixedStepSeconds = 1.0f / 60.0f;
constexpr float kMaxFrameDeltaSeconds = 0.10f;
constexpr float kLogoWidthDp = 6187.0f * 0.125f;
constexpr float kLogoHeightDp = 2625.0f * 0.125f;
constexpr float kLogoIntroDurationSeconds = 2.0f;
constexpr float kLogoStartYDp = -900.0f;
constexpr float kLogoEndYDp = -365.0f;
constexpr float kLogoDriftAmplitudeDp = 10.0f;
constexpr float kLogoDriftFrequencyHz = 0.22f;
constexpr float kTitleBackgroundAssetWidthDp = 1536.0f;
constexpr float kTitleBackgroundAssetHeightDp = 1024.0f;
constexpr int kTitleSlideInSteps = 16;
constexpr float kTitleBouncePhaseStepDegrees = 6.0f;

enum class TitleAnimationPhase {
    Uninitialized,
    SlidingIn,
    Bouncing,
    Complete,
};

struct TitleBackgroundState {
    recompui::Image* image = nullptr;
    std::chrono::steady_clock::time_point last_update_time{};
    float accumulator_seconds = 0.0f;
    float x_offset_dp = 0.0f;
    float slide_step_dp = 0.0f;
    float cover_width_dp = 0.0f;
    float cover_height_dp = 0.0f;
    float base_x_dp = 0.0f;
    float base_y_dp = 0.0f;
    float last_bg_width_dp = 0.0f;
    float last_bg_height_dp = 0.0f;
    float bounce_phase_degrees = 180.0f;
    int bounce_divisor = 0;
    bool started = false;
    TitleAnimationPhase phase = TitleAnimationPhase::Uninitialized;
};

TitleBackgroundState title_background_state;
bool config_background_initialized = false;
bool title_model_configuration_attempted = false;
bool title_model_active = false;
bool title_model_intro_gate_open = false;

struct Launcher3DMenuUpdateScope {
    explicit Launcher3DMenuUpdateScope(recompui::LauncherMenu* menu_in) : menu(menu_in) {}
    ~Launcher3DMenuUpdateScope() {
        csdk::launcher3d::on_launcher_menu_update(menu != nullptr ? menu->get_menu_container() : nullptr);
    }

    recompui::LauncherMenu* menu = nullptr;
};

bool is_title_background_intro_complete() {
    return title_background_state.phase == TitleAnimationPhase::Complete;
}

void apply_title_model_intro_gate(bool should_open_gate) {
    if (!title_model_active || should_open_gate == title_model_intro_gate_open) {
        return;
    }

    title_model_intro_gate_open = should_open_gate;
    csdk::launcher3d::set_enabled(should_open_gate);
    if (should_open_gate) {
        csdk::launcher3d::reset_intro();
    }
}

bool configure_title_intro_model() {
    if (title_model_configuration_attempted) {
        return title_model_active;
    }

    title_model_configuration_attempted = true;

    auto make_title_intro_model_config = []() {
        csdk::launcher3d::Config cfg{};
        cfg.glb_path = recompui::file::get_asset_path("launcher_intro.glb");

        cfg.target_transform.position = { 0.000f, 0.930f, 0.180f };
        cfg.target_transform.rotation_deg = { 11.800f, 180.000f, 0.000f };
        cfg.target_transform.scale = { 0.620f, 0.620f, 0.620f };

        cfg.light.direction_ws = { -0.200f, -0.800f, -0.500f };
        cfg.light.position_ws = { 0.050f, 0.250f, 7.600f };
        cfg.light.range = 22.300f;
        cfg.light.color = { 0.870f, 0.890f, 1.330f };
        cfg.light.intensity = 1.690f;
        cfg.light.ambient_intensity = 0.530f;

        cfg.intro.duration_sec = 1.780f;
        cfg.intro.overshoot = 0.290f;
        cfg.intro.damping = 7.250f;
        cfg.intro.play_once = true;
        cfg.visible_only_on_title_screen = true;
        return cfg;
    };

    const csdk::launcher3d::Config cfg = make_title_intro_model_config();

    std::fprintf(stdout,
        "[CellenseresSDK] launcher_animation: configure_title_intro_model path='%s'\n",
        cfg.glb_path.string().c_str());
    std::fflush(stdout);

    title_model_active = csdk::launcher3d::configure(cfg);
    std::fprintf(stdout,
        "[CellenseresSDK] launcher_animation: configure_title_intro_model result=%s\n",
        title_model_active ? "success" : "failed");
    std::fflush(stdout);
    return title_model_active;
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float smootherstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float intro_sine_256(float degrees) {
    constexpr float radians_per_degree = 3.14159265358979323846f / 180.0f;
    return std::round(std::sin(degrees * radians_per_degree) * 256.0f);
}

void update_title_background_layout(float bg_width_dp, float bg_height_dp) {
    const float cover_scale = std::max(
        bg_width_dp / kTitleBackgroundAssetWidthDp,
        bg_height_dp / kTitleBackgroundAssetHeightDp
    );

    title_background_state.cover_width_dp = kTitleBackgroundAssetWidthDp * cover_scale;
    title_background_state.cover_height_dp = kTitleBackgroundAssetHeightDp * cover_scale;
    // Match the original feel: the background settles with its left edge pinned to the viewport.
    // We still use cover scaling so the viewport is always fully filled without distorting the image.
    title_background_state.base_x_dp = 0.0f;
    title_background_state.base_y_dp = (bg_height_dp - title_background_state.cover_height_dp) * 0.5f;
    title_background_state.last_bg_width_dp = bg_width_dp;
    title_background_state.last_bg_height_dp = bg_height_dp;
}

void reset_title_background_animation(float bg_width_dp, float bg_height_dp) {
    update_title_background_layout(bg_width_dp, bg_height_dp);

    title_background_state.phase = TitleAnimationPhase::SlidingIn;
    title_background_state.started = false;
    title_background_state.accumulator_seconds = 0.0f;
    title_background_state.last_update_time = std::chrono::steady_clock::time_point{};
    title_background_state.bounce_phase_degrees = 180.0f;
    title_background_state.bounce_divisor = 0;

    // Start fully offscreen on the right, then reproduce the original 16-frame slide-in.
    title_background_state.x_offset_dp = bg_width_dp - title_background_state.base_x_dp;
    title_background_state.slide_step_dp = title_background_state.x_offset_dp / static_cast<float>(kTitleSlideInSteps);
}

void simulate_title_background_step(float bg_width_dp) {
    switch (title_background_state.phase) {
    case TitleAnimationPhase::SlidingIn:
        if (title_background_state.x_offset_dp <= 0.0f) {
            title_background_state.phase = TitleAnimationPhase::Bouncing;
        } else {
            title_background_state.x_offset_dp = std::max(0.0f, title_background_state.x_offset_dp - title_background_state.slide_step_dp);
        }
        break;
    case TitleAnimationPhase::Bouncing:
        if (title_background_state.bounce_phase_degrees >= 359.0f) {
            title_background_state.bounce_phase_degrees = 0.0f;
        }

        if ((static_cast<int>(title_background_state.bounce_phase_degrees) % 180) == 0) {
            title_background_state.bounce_divisor += 4;
        }

        if (title_background_state.bounce_divisor > 0) {
            title_background_state.x_offset_dp =
                std::abs(intro_sine_256(title_background_state.bounce_phase_degrees) / static_cast<float>(title_background_state.bounce_divisor))
                * (bg_width_dp / 320.0f);
        } else {
            title_background_state.x_offset_dp = 0.0f;
        }

        if (title_background_state.bounce_divisor < 5) {
            title_background_state.bounce_phase_degrees += kTitleBouncePhaseStepDegrees;
        } else {
            title_background_state.phase = TitleAnimationPhase::Complete;
            title_background_state.x_offset_dp = 0.0f;
        }
        break;
    case TitleAnimationPhase::Complete:
    case TitleAnimationPhase::Uninitialized:
    default:
        title_background_state.x_offset_dp = 0.0f;
        break;
    }
}

void update_title_background_visual() {
    if (title_background_state.image == nullptr) {
        return;
    }

    title_background_state.image->set_width(title_background_state.cover_width_dp, recompui::Unit::Dp);
    title_background_state.image->set_height(title_background_state.cover_height_dp, recompui::Unit::Dp);
    title_background_state.image->set_translate_2D(
        title_background_state.base_x_dp + title_background_state.x_offset_dp,
        title_background_state.base_y_dp,
        recompui::Unit::Dp
    );
}

float trail_dot_scale(size_t index) {
    if (kTrailDots <= 1) {
        return 1.0f;
    }

    return 1.0f - (1.0f - kTrailScaleMin) * static_cast<float>(index) / static_cast<float>(kTrailDots - 1);
}

float trail_dot_opacity(size_t index) {
    if (kTrailDots <= 1) {
        return kTrailOpacityMax;
    }

    return kTrailOpacityMax + (kTrailOpacityMin - kTrailOpacityMax) * static_cast<float>(index) / static_cast<float>(kTrailDots - 1);
}

struct Star {
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
    float speed_dp = 0.0f;
    float size_dp = 0.0f;
    std::array<recompui::Element*, kTrailDots> dots{};
};

class PreGameConfigBackground final : public recompui::Element {
public:
    PreGameConfigBackground(recompui::ResourceId rid, recompui::Element* parent)
        : Element(rid, parent, recompui::Events(recompui::EventType::Update)),
          rng_(kAnimationSeed) {
        set_position(recompui::Position::Absolute);
        set_top(0);
        set_right(0);
        set_bottom(0);
        set_left(0);
        set_pointer_events(recompui::PointerEvents::None);
        set_overflow(recompui::Overflow::Hidden);
        set_border_radius(recompui::theme::border::radius_lg);
        set_background_color({ 0, 0, 0, 255 });

        auto context = recompui::get_current_context();
        stars_.resize(kStarCount);
        for (Star& star : stars_) {
            for (size_t dot_index = 0; dot_index < kTrailDots; ++dot_index) {
                auto* dot = context.create_element<recompui::Element>(this, 0);
                dot->set_position(recompui::Position::Absolute);
                dot->set_background_color({ 255, 255, 255, static_cast<uint8_t>(255.0f * trail_dot_opacity(dot_index)) });
                star.dots[dot_index] = dot;
            }
        }

        logo_ = context.create_element<recompui::Svg>(this, "Logo.svg");
        logo_->set_position(recompui::Position::Absolute);
        logo_->set_width(kLogoWidthDp, recompui::Unit::Dp);
        logo_->set_height(kLogoHeightDp, recompui::Unit::Dp);

        queue_update();
    }

private:
    std::vector<Star> stars_;
    recompui::Svg* logo_ = nullptr;
    DeterministicRng rng_;
    std::chrono::steady_clock::time_point last_update_time_{};
    float accumulator_seconds_ = 0.0f;
    float elapsed_seconds_ = 0.0f;
    bool started_ = false;
    bool visible_ = false;

    std::string_view get_type_name() override {
        return "PreGameConfigBackground";
    }

    void process_event(const recompui::Event& e) override {
        if (e.type != recompui::EventType::Update) {
            return;
        }

        queue_update();

        const bool should_show = !ultramodern::is_game_started();
        if (!should_show) {
            display_hide();
            visible_ = false;
            started_ = false;
            accumulator_seconds_ = 0.0f;
            return;
        }

        display_show();

        const float dp_ratio = get_dp_to_pixel_ratio();
        if (dp_ratio <= 0.0f) {
            return;
        }

        const float bg_width = get_client_width() / dp_ratio;
        const float bg_height = get_client_height() / dp_ratio;
        if (bg_width <= 1.0f || bg_height <= 1.0f) {
            return;
        }

        if (!visible_) {
            reset_animation(bg_width, bg_height);
            visible_ = true;
        }

        const auto now = std::chrono::steady_clock::now();
        float delta_seconds = 0.0f;
        if (started_) {
            delta_seconds = std::chrono::duration_cast<std::chrono::duration<float>>(now - last_update_time_).count();
            delta_seconds = std::clamp(delta_seconds, 0.0f, kMaxFrameDeltaSeconds);
        }
        last_update_time_ = now;
        started_ = true;

        accumulator_seconds_ += delta_seconds;
        while (accumulator_seconds_ >= kFixedStepSeconds) {
            advance_simulation(kFixedStepSeconds, bg_width, bg_height);
            accumulator_seconds_ -= kFixedStepSeconds;
        }

        update_visuals(bg_width, bg_height);
    }

    void reset_animation(float bg_width, float bg_height) {
        rng_.reset(kAnimationSeed);
        elapsed_seconds_ = 0.0f;
        accumulator_seconds_ = 0.0f;
        last_update_time_ = std::chrono::steady_clock::time_point{};
        started_ = false;

        for (Star& star : stars_) {
            spawn_star(star, bg_width, bg_height, true);
        }

        update_visuals(bg_width, bg_height);
    }

    void spawn_star(Star& star, float bg_width, float bg_height, bool initial_spawn) {
        const float half_width = bg_width * 0.5f;
        const float half_height = bg_height * 0.5f;

        star.x = initial_spawn
            ? rng_.next_range(-half_width, half_width)
            : rng_.next_range(half_width, half_width + bg_width * 0.35f);
        star.y = rng_.next_range(-half_height, half_height);
        star.depth = rng_.next_unit_float();
        star.speed_dp = kBaseSpeedDp + star.depth * kSpeedRangeDp;
        star.size_dp = kSizeMinDp + star.depth * (kSizeMaxDp - kSizeMinDp);
    }

    float trail_spacing_for_star(const Star& star) const {
        return kTrailSpacingDp * (kTrailLengthFar + star.depth * (1.0f - kTrailLengthFar));
    }

    void advance_simulation(float delta_seconds, float bg_width, float bg_height) {
        elapsed_seconds_ += delta_seconds;

        const float half_width = bg_width * 0.5f;
        for (Star& star : stars_) {
            star.x -= star.speed_dp * delta_seconds;

            const float spacing = trail_spacing_for_star(star);
            const float trail_length = spacing * static_cast<float>(kTrailDots - 1);
            const float offscreen_left = -half_width - trail_length - 20.0f;
            if (star.x < offscreen_left) {
                spawn_star(star, bg_width, bg_height, false);
            }
        }
    }

    void update_visuals(float bg_width, float bg_height) {
        const float center_x = bg_width * 0.5f;
        const float center_y = bg_height * 0.5f;

        for (Star& star : stars_) {
            const float spacing = trail_spacing_for_star(star);
            for (size_t dot_index = 0; dot_index < kTrailDots; ++dot_index) {
                const float scale = trail_dot_scale(dot_index);
                float dot_size = star.size_dp * scale;
                dot_size = std::max(dot_size, kDotSizeMinDp);

                auto* dot = star.dots[dot_index];
                dot->set_width(dot_size, recompui::Unit::Dp);
                dot->set_height(dot_size, recompui::Unit::Dp);
                dot->set_border_radius(dot_size * 0.5f, recompui::Unit::Dp);

                const float dot_x = center_x + star.x + static_cast<float>(dot_index) * spacing - dot_size * 0.5f;
                const float dot_y = center_y + star.y - dot_size * 0.5f;
                dot->set_translate_2D(dot_x, dot_y, recompui::Unit::Dp);
            }
        }

        const float intro_t = std::clamp(elapsed_seconds_ / kLogoIntroDurationSeconds, 0.0f, 1.0f);
        const float intro_phase = smootherstep(intro_t);
        float logo_y = lerp(kLogoStartYDp, kLogoEndYDp, intro_phase);
        if (intro_t >= 1.0f) {
            const float drift_phase = (elapsed_seconds_ - kLogoIntroDurationSeconds) * 2.0f * 3.14159265f * kLogoDriftFrequencyHz;
            logo_y += std::sin(drift_phase) * kLogoDriftAmplitudeDp;
        }

        logo_->set_opacity(lerp(0.0f, 1.0f, intro_phase));
        logo_->set_translate_2D(
            center_x - kLogoWidthDp * 0.5f,
            center_y - kLogoHeightDp * 0.5f + logo_y,
            recompui::Unit::Dp
        );
    }
};
} // namespace

void sssv::launcher_animation_setup(recompui::LauncherMenu* menu) {
    auto* background_container = menu->get_background_container();
    background_container->set_background_color({ 0, 0, 0, 255 });
    background_container->set_overflow(recompui::Overflow::Hidden);

    title_model_active = configure_title_intro_model();
    title_model_intro_gate_open = false;
    if (title_model_active) {
        // Delay 3D intro until the title background has finished its slide+bounce animation.
        csdk::launcher3d::set_enabled(false);
    }

    csdk::ui::queue_image_from_file(
        "img_space_background.png",
        recompui::file::get_asset_path("img_space_background.png")
    );

    auto context = recompui::get_current_context();
    title_background_state.image = context.create_element<recompui::Image>(background_container, "img_space_background.png");
    title_background_state.image->set_position(recompui::Position::Absolute);
    title_background_state.image->set_top(0);
    title_background_state.image->set_left(0);

    title_background_state.phase = TitleAnimationPhase::Uninitialized;
    title_background_state.started = false;
    title_background_state.accumulator_seconds = 0.0f;
    title_background_state.last_bg_width_dp = 0.0f;
    title_background_state.last_bg_height_dp = 0.0f;
}

void sssv::launcher_animation_update(recompui::LauncherMenu* menu) {
    Launcher3DMenuUpdateScope launcher3d_scope(menu);

    if (!config_background_initialized) {
        setup_config_menu_background_animation();
    }

    auto* background_container = menu->get_background_container();
    if (background_container == nullptr || title_background_state.image == nullptr) {
        return;
    }

    const float dp_ratio = background_container->get_dp_to_pixel_ratio();
    if (dp_ratio <= 0.0f) {
        return;
    }

    const float bg_width_dp = background_container->get_client_width() / dp_ratio;
    const float bg_height_dp = background_container->get_client_height() / dp_ratio;
    if (bg_width_dp <= 1.0f || bg_height_dp <= 1.0f) {
        return;
    }

    const bool needs_layout_refresh =
        title_background_state.phase == TitleAnimationPhase::Uninitialized ||
        title_background_state.last_bg_width_dp != bg_width_dp ||
        title_background_state.last_bg_height_dp != bg_height_dp;

    if (needs_layout_refresh) {
        if (title_background_state.phase == TitleAnimationPhase::Uninitialized) {
            reset_title_background_animation(bg_width_dp, bg_height_dp);
        } else {
            update_title_background_layout(bg_width_dp, bg_height_dp);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    float delta_seconds = 0.0f;
    if (title_background_state.started) {
        delta_seconds = std::chrono::duration_cast<std::chrono::duration<float>>(now - title_background_state.last_update_time).count();
        delta_seconds = std::clamp(delta_seconds, 0.0f, kMaxFrameDeltaSeconds);
    }

    title_background_state.last_update_time = now;
    title_background_state.started = true;

    title_background_state.accumulator_seconds += delta_seconds;
    while (title_background_state.accumulator_seconds >= kFixedStepSeconds) {
        simulate_title_background_step(bg_width_dp);
        title_background_state.accumulator_seconds -= kFixedStepSeconds;
    }

    update_title_background_visual();
    apply_title_model_intro_gate(is_title_background_intro_complete());
}

void sssv::setup_config_menu_background_animation() {
    if (config_background_initialized) {
        return;
    }

    auto* modal = recompui::config::get_config_modal();
    if (modal == nullptr) {
        return;
    }

    auto* modal_element = modal->get_body()->get_parent();
    if (modal_element == nullptr) {
        return;
    }

    recompui::ContextId previous_context = recompui::try_close_current_context();
    auto context = modal->modal_root_context;
    bool opened = context.open_if_not_already();
    auto* background = context.create_element<PreGameConfigBackground>(modal_element);
    csdk::ui::prepend_child(modal_element, background);
    config_background_initialized = true;
    if (opened) {
        context.close();
    }
    if (previous_context != recompui::ContextId::null()) {
        previous_context.open();
    }
}
