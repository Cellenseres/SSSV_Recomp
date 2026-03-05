#include "cs_sdk/launcher_model3d.h"

#include "core/ui_context.h"
#include "elements/ui_button.h"
#include "elements/ui_clickable.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_slider.h"
#include "rt64_render_hooks.h"
#include "rt64_texture.h"
#include "rt64_texture_cache.h"
#include "ultramodern/ultramodern.hpp"

#include "plume_render_interface.h"
#include "plume_render_interface_builders.h"
#ifdef _WIN32
#include "plume_d3d12.h"
#endif
#if !defined(__APPLE__)
#include "plume_vulkan.h"
#endif
#if defined(__APPLE__)
#include "plume_metal.h"
#include "plume_vulkan.h"
#endif

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "LauncherModelVS.hlsl.spirv.h"
#include "LauncherModelPS.hlsl.spirv.h"
#include "LauncherShadowVS.hlsl.spirv.h"
#include "LauncherShadowPS.hlsl.spirv.h"
#ifdef _WIN32
#include "LauncherModelVS.hlsl.dxil.h"
#include "LauncherModelPS.hlsl.dxil.h"
#include "LauncherShadowVS.hlsl.dxil.h"
#include "LauncherShadowPS.hlsl.dxil.h"
#elif defined(__APPLE__)
#include "LauncherModelVS.hlsl.metal.h"
#include "LauncherModelPS.hlsl.metal.h"
#include "LauncherShadowVS.hlsl.metal.h"
#include "LauncherShadowPS.hlsl.metal.h"
#endif

#ifdef _WIN32
#define GET_SHADER_BLOB(name, format) \
    ((format) == plume::RenderShaderFormat::SPIRV ? name##BlobSPIRV : \
    (format) == plume::RenderShaderFormat::DXIL ? name##BlobDXIL : nullptr)
#define GET_SHADER_SIZE(name, format) \
    ((format) == plume::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : \
    (format) == plume::RenderShaderFormat::DXIL ? std::size(name##BlobDXIL) : 0)
#elif defined(__APPLE__)
#define GET_SHADER_BLOB(name, format) \
    ((format) == plume::RenderShaderFormat::SPIRV ? name##BlobSPIRV : \
    (format) == plume::RenderShaderFormat::METAL ? name##BlobMSL : nullptr)
#define GET_SHADER_SIZE(name, format) \
    ((format) == plume::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : \
    (format) == plume::RenderShaderFormat::METAL ? std::size(name##BlobMSL) : 0)
#else
#define GET_SHADER_BLOB(name, format) ((format) == plume::RenderShaderFormat::SPIRV ? name##BlobSPIRV : nullptr)
#define GET_SHADER_SIZE(name, format) ((format) == plume::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : 0)
#endif

namespace recompui {
    bool is_context_shown(ContextId context);
    namespace config {
        ContextId get_config_context_id();
    }
}

extern "C" int SDL_SetClipboardText(const char* text);

namespace csdk::launcher3d {
namespace {

constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr float kCameraZ = 2.7f;
constexpr float kSpawnDepthDistance = 6.0f;
constexpr float kSpawnScaleMul = 0.08f;
constexpr float kMinRenderableScale = 0.001f;
constexpr plume::RenderFormat kShadowColorFormat = plume::RenderFormat::R32_FLOAT;
constexpr uint32_t kShadowFaceCount = 6;
constexpr float kShadowNearPlaneMin = 0.01f;
constexpr float kShadowFarPlaneMin = 0.10f;
constexpr float kDebugLightMarkerRadius = 0.085f;
constexpr uint32_t kDebugLightMarkerSlices = 32;
constexpr uint32_t kDebugLightMarkerStacks = 20;

struct V2 { float x, y; };
struct V3 { float x, y, z; };
struct V4 { float x, y, z, w; };
struct M4 { float m[16]; };

struct Vertex {
    V3 p{};
    V3 n{ 0.0f, 0.0f, 1.0f };
    V4 t{ 1.0f, 0.0f, 0.0f, 1.0f };
    V2 uv{};
};

struct CpuModel {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint8_t> albedo;
    std::vector<uint8_t> normal;
    std::vector<uint8_t> spec;
    V4 base_color{ 1, 1, 1, 1 };
    V4 spec_color{ 1, 1, 1, 1 };
    float spec_factor = 1.0f;
    bool source_had_normals = false;
    bool generated_smooth_normals = false;
    bool flipped_winding = false;
    bool had_owner_node = false;
    float owner_node_det3 = 1.0f;
    V3 bounds_min{ 0.0f, 0.0f, 0.0f };
    V3 bounds_max{ 0.0f, 0.0f, 0.0f };
    V3 bounds_center{ 0.0f, 0.0f, 0.0f };
    float bounds_radius = 1.0f;
};

struct Constants {
    M4 model{};
    M4 view_proj{};
    V4 light_pos_range{};
    V4 light_dir_intensity{};
    V4 light_color_ambient{};
    V4 camera_spec{};
    V4 base_color{};
    V4 spec_color{};
    V4 shadow_params0{};
    V4 shadow_params1{};
    M4 shadow_view_proj[6]{};
};

struct ShadowPassConstants {
    M4 model{};
    M4 light_view_proj{};
    V4 light_pos_far{};
};

enum class ActiveShadowMode {
    Disabled = 0,
    PointAtlas = 1,
    Spot = 2,
};

struct ShadowRuntime {
    ActiveShadowMode active_mode = ActiveShadowMode::Disabled;
    uint32_t effective_resolution = 0;
    bool dirty = true;
    bool has_signature = false;
    bool resources_ready = false;
    uint64_t render_count = 0;
    std::string fallback_reason{};
    Transform last_transform{};
    Light last_light{};
    ShadowConfig last_shadow_cfg{};
};

struct Gpu {
    plume::RenderInterface* rhi = nullptr;
    plume::RenderDevice* dev = nullptr;
    std::unique_ptr<plume::RenderSampler> sampler;
    std::unique_ptr<plume::RenderSampler> shadow_sampler;
    std::unique_ptr<plume::RenderShader> vs;
    std::unique_ptr<plume::RenderShader> ps;
    std::unique_ptr<plume::RenderShader> shadow_vs;
    std::unique_ptr<plume::RenderShader> shadow_ps;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderPipeline> pipeline;
    std::unique_ptr<plume::RenderPipelineLayout> shadow_layout;
    std::unique_ptr<plume::RenderPipeline> shadow_pipeline;
    std::unique_ptr<plume::RenderDescriptorSetBuilder> set_builder;
    std::unique_ptr<plume::RenderDescriptorSet> set;
    std::unique_ptr<plume::RenderDescriptorSet> marker_set;
    std::unique_ptr<plume::RenderDescriptorSetBuilder> shadow_set_builder;
    std::unique_ptr<plume::RenderDescriptorSet> shadow_set;
    std::unique_ptr<plume::RenderBuffer> vb;
    std::unique_ptr<plume::RenderBuffer> ib;
    std::unique_ptr<plume::RenderBuffer> marker_vb;
    std::unique_ptr<plume::RenderBuffer> marker_ib;
    std::unique_ptr<plume::RenderBuffer> cb;
    std::unique_ptr<plume::RenderBuffer> marker_cb;
    std::unique_ptr<plume::RenderBuffer> shadow_cb;
    std::unique_ptr<plume::RenderTexture> tex_albedo;
    std::unique_ptr<plume::RenderTexture> tex_normal;
    std::unique_ptr<plume::RenderTexture> tex_spec;
    std::unique_ptr<plume::RenderTexture> tex_shadow;
    std::unique_ptr<plume::RenderTexture> tex_shadow_depth;
    std::unique_ptr<plume::RenderFramebuffer> shadow_fb;
    std::unique_ptr<plume::RenderTexture> tex_shadow_fallback;
    std::unique_ptr<plume::RenderCommandQueue> copy_q;
    std::unique_ptr<plume::RenderCommandList> copy_l;
    std::unique_ptr<plume::RenderCommandFence> copy_f;
    std::unique_ptr<plume::RenderBuffer> upload;
    std::unordered_map<const plume::RenderFramebuffer*, std::unique_ptr<plume::RenderTexture>> depth_by_src_fb;
    std::unordered_map<const plume::RenderFramebuffer*, std::unique_ptr<plume::RenderFramebuffer>> fb_with_depth_by_src_fb;
    plume::RenderInputSlot slot{ 0, sizeof(Vertex) };
    uint32_t cb_idx = 0;
    uint32_t a_idx = 0;
    uint32_t n_idx = 0;
    uint32_t s_idx = 0;
    uint32_t shadow_idx = 0;
    uint32_t shadow_cb_idx = 0;
    uint64_t cb_size = 0;
    uint64_t marker_cb_size = 0;
    uint64_t shadow_cb_size = 0;
    uint64_t marker_vb_size = 0;
    uint64_t marker_ib_size = 0;
    size_t index_count = 0;
    size_t marker_index_count = 0;
    plume::RenderFormat pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    plume::RenderFormat shadow_pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    bool pipeline_uses_depth = false;
    bool pipeline_ok = false;
    bool shadow_pipeline_ok = false;
    bool shadow_pipeline_unavailable = false;
    bool ready = false;
};

struct DebugPanel {
#if !defined(NDEBUG)
    bool built = false;
    bool open = false;
    recompui::Element* root = nullptr;
    recompui::Element* content = nullptr;
    recompui::Label* status = nullptr;
    float left_dp = 24.0f;
    float top_dp = 24.0f;
    float drag_start_mouse_x = 0.0f;
    float drag_start_mouse_y = 0.0f;
    float drag_start_left_dp = 24.0f;
    float drag_start_top_dp = 24.0f;
    bool show_light_marker = true;
    float light_marker_scale = 1.0f;
    bool free_camera_enabled = false;
    float camera_yaw_deg = 0.0f;
    float camera_pitch_deg = 0.0f;
    float camera_distance = kCameraZ;
    float camera_fov_deg = 55.0f;
    float camera_target_x = 0.0f;
    float camera_target_y = 0.0f;
    float camera_target_z = 0.0f;
    float camera_drag_start_x = 0.0f;
    float camera_drag_start_y = 0.0f;
    float camera_drag_start_yaw = 0.0f;
    float camera_drag_start_pitch = 0.0f;
#endif
};

struct State {
    std::mutex mx;
    RT64::RenderHookInit* prev_init = nullptr;
    RT64::RenderHookDraw* prev_draw = nullptr;
    RT64::RenderHookDeinit* prev_deinit = nullptr;
    bool hooks_installed = false;

    Config cfg{};
    Config cfg_initial{};
    CpuModel cpu{};
    Gpu gpu{};
    DebugPanel panel{};
    bool configured = false;
    bool enabled = false;
    bool intro_started = false;
    bool intro_finished = false;
    std::chrono::steady_clock::time_point intro_t0{};
    V4 last_clip_center{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool last_draw_attempted = false;
    bool pipeline_diag_reported = false;
    std::string pipeline_diag{};
    ShadowRuntime shadow{};
    bool trace_enabled = true;
    uint64_t trace_install_calls = 0;
    uint64_t trace_init_calls = 0;
    uint64_t trace_draw_calls = 0;
    uint64_t trace_deinit_calls = 0;
    uint64_t trace_pipeline_attempts = 0;
    uint64_t trace_frames_skipped = 0;
    bool trace_first_draw_logged = false;
    bool trace_last_should_draw = false;
    std::string trace_last_reason{};
    std::chrono::steady_clock::time_point trace_last_heartbeat{};
};

State& st() { static State s{}; return s; }

void copy_text_to_clipboard(const std::string& text) {
    if (text.empty()) {
        return;
    }

    const int rc = SDL_SetClipboardText(text.c_str());
    if (rc != 0) {
#if !defined(NDEBUG)
        std::fprintf(stderr, "[CellenseresSDK] launcher3d: copy_text_to_clipboard failed rc=%d\n", rc);
        std::fflush(stderr);
#endif
    }
}

#if !defined(NDEBUG)
void trace_log(const char* fmt, ...) {
    State& s = st();
    if (!s.trace_enabled) {
        return;
    }

    std::fprintf(stdout, "[CellenseresSDK] launcher3d: ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fprintf(stdout, "\n");
    std::fflush(stdout);
}

const char* yes_no(bool v) {
    return v ? "yes" : "no";
}

const char* shader_format_name(plume::RenderShaderFormat fmt) {
    switch (fmt) {
    case plume::RenderShaderFormat::DXIL:
        return "DXIL";
    case plume::RenderShaderFormat::SPIRV:
        return "SPIRV";
    case plume::RenderShaderFormat::METAL:
        return "METAL";
    case plume::RenderShaderFormat::UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

const char* cmd_list_type_name(plume::RenderCommandListType type) {
    switch (type) {
    case plume::RenderCommandListType::DIRECT:
        return "DIRECT";
    case plume::RenderCommandListType::COMPUTE:
        return "COMPUTE";
    case plume::RenderCommandListType::COPY:
        return "COPY";
    case plume::RenderCommandListType::UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

template <typename T>
const void* ptr_addr(T* ptr) {
    return reinterpret_cast<const void*>(ptr);
}
#define LAUNCHER3D_TRACE(...) trace_log(__VA_ARGS__)
#else
#define LAUNCHER3D_TRACE(...) do {} while (0)
#endif

#include "launcher_model3d_math.inl"
#include "launcher_model3d_asset.inl"

void clear_gpu(State& s){
    s.gpu.set.reset();
    s.gpu.marker_set.reset();
    s.gpu.shadow_set.reset();
    s.gpu.vb.reset();
    s.gpu.ib.reset();
    s.gpu.marker_vb.reset();
    s.gpu.marker_ib.reset();
    s.gpu.cb.reset();
    s.gpu.marker_cb.reset();
    s.gpu.shadow_cb.reset();
    s.gpu.tex_albedo.reset();
    s.gpu.tex_normal.reset();
    s.gpu.tex_spec.reset();
    s.gpu.tex_shadow.reset();
    s.gpu.tex_shadow_depth.reset();
    s.gpu.shadow_fb.reset();
    s.gpu.tex_shadow_fallback.reset();
    s.gpu.upload.reset();
    s.gpu.depth_by_src_fb.clear();
    s.gpu.fb_with_depth_by_src_fb.clear();
    s.gpu.pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    s.gpu.shadow_pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    s.gpu.pipeline_uses_depth = false;
    s.gpu.ready=false;
    s.gpu.index_count=0;
    s.gpu.marker_vb_size = 0;
    s.gpu.marker_ib_size = 0;
    s.gpu.marker_index_count=0;
    s.gpu.marker_cb_size = 0;
    s.shadow.resources_ready = false;
    s.shadow.has_signature = false;
    s.shadow.dirty = true;
    s.shadow.active_mode = ActiveShadowMode::Disabled;
}
void clear_pipeline(State& s) {
    clear_gpu(s);
    s.gpu.pipeline.reset();
    s.gpu.shadow_pipeline.reset();
    s.gpu.layout.reset();
    s.gpu.shadow_layout.reset();
    s.gpu.vs.reset();
    s.gpu.ps.reset();
    s.gpu.shadow_vs.reset();
    s.gpu.shadow_ps.reset();
    s.gpu.sampler.reset();
    s.gpu.shadow_sampler.reset();
    s.gpu.set_builder.reset();
    s.gpu.shadow_set_builder.reset();
    s.gpu.copy_l.reset();
    s.gpu.copy_q.reset();
    s.gpu.copy_f.reset();
    s.gpu.pipeline_ok = false;
    s.gpu.shadow_pipeline_ok = false;
    s.gpu.shadow_pipeline_unavailable = false;
    s.pipeline_diag_reported = false;
    s.pipeline_diag.clear();
}

std::unique_ptr<plume::RenderTexture> tex_rgba(
    plume::RenderDevice* d,
    plume::RenderCommandQueue* q,
    plume::RenderCommandList* l,
    plume::RenderCommandFence* f,
    std::unique_ptr<plume::RenderBuffer>& up,
    std::array<uint8_t, 4> c
) {
    auto t = std::make_unique<RT64::Texture>();
    l->begin();
    RT64::TextureCache::setRGBA32(t.get(), d, l, c.data(), 4, 1, 1, 4, up, nullptr);
    l->end();
    q->executeCommandLists(l, f);
    q->waitForCommandFence(f);
    return std::move(t->texture);
}
std::unique_ptr<plume::RenderTexture> tex_file(
    plume::RenderDevice* d,
    plume::RenderCommandQueue* q,
    plume::RenderCommandList* l,
    plume::RenderCommandFence* f,
    std::unique_ptr<plume::RenderBuffer>& up,
    const std::vector<uint8_t>& b,
    std::array<uint8_t, 4> fb
) {
    if (b.empty()) {
        return tex_rgba(d, q, l, f, up, fb);
    }

    std::vector<uint8_t> bytes = b;
    l->begin();
    std::unique_ptr<RT64::Texture> texture(RT64::TextureCache::loadTextureFromBytes(d, l, bytes, up));
    l->end();
    q->executeCommandLists(l, f);
    q->waitForCommandFence(f);

    if (!texture || !texture->texture) {
        return tex_rgba(d, q, l, f, up, fb);
    }

    return std::move(texture->texture);
}

bool ensure_pipeline(State& s){
    if (s.gpu.pipeline_ok && (s.gpu.shadow_pipeline_ok || s.gpu.shadow_pipeline_unavailable)) return true;
    s.trace_pipeline_attempts++;
    if(!s.gpu.rhi||!s.gpu.dev){
        s.pipeline_diag = "No RHI/Device";
        LAUNCHER3D_TRACE(
            "ensure_pipeline: blocked attempt=%llu rhi=%p dev=%p",
            static_cast<unsigned long long>(s.trace_pipeline_attempts),
            ptr_addr(s.gpu.rhi),
            ptr_addr(s.gpu.dev)
        );
        return false;
    }

    auto set_diag = [&](const char* msg)->bool{
        s.pipeline_diag = msg;
#if !defined(NDEBUG)
        if(!s.pipeline_diag_reported){
            std::fprintf(stderr, "[CellenseresSDK] launcher3d: pipeline init failed: %s\n", msg);
            s.pipeline_diag_reported = true;
        }
#endif
        return false;
    };

    auto* d=s.gpu.dev; plume::RenderShaderFormat sf=s.gpu.rhi->getCapabilities().shaderFormat;
    LAUNCHER3D_TRACE(
        "ensure_pipeline: begin attempt=%llu rhi=%p dev=%p shaderFormat=%s",
        static_cast<unsigned long long>(s.trace_pipeline_attempts),
        ptr_addr(s.gpu.rhi),
        ptr_addr(s.gpu.dev),
        shader_format_name(sf)
    );

    plume::RenderSamplerDesc samp{};
    samp.minFilter = plume::RenderFilter::LINEAR;
    samp.magFilter = plume::RenderFilter::LINEAR;
    samp.addressU = plume::RenderTextureAddressMode::WRAP;
    samp.addressV = plume::RenderTextureAddressMode::WRAP;
    samp.addressW = plume::RenderTextureAddressMode::WRAP;
    s.gpu.sampler=d->createSampler(samp);
    if(!s.gpu.sampler) return set_diag("createSampler");
    LAUNCHER3D_TRACE("ensure_pipeline: sampler ok");

    plume::RenderSamplerDesc shadow_samp{};
    shadow_samp.minFilter = plume::RenderFilter::NEAREST;
    shadow_samp.magFilter = plume::RenderFilter::NEAREST;
    shadow_samp.addressU = plume::RenderTextureAddressMode::CLAMP;
    shadow_samp.addressV = plume::RenderTextureAddressMode::CLAMP;
    shadow_samp.addressW = plume::RenderTextureAddressMode::CLAMP;
    s.gpu.shadow_sampler = d->createSampler(shadow_samp);
    if (!s.gpu.shadow_sampler) return set_diag("createShadowSampler");
    LAUNCHER3D_TRACE("ensure_pipeline: shadow sampler ok");

    auto create_model_vs = [&](plume::RenderShaderFormat fmt)->std::unique_ptr<plume::RenderShader>{
        const void* blob = GET_SHADER_BLOB(LauncherModelVS, fmt);
        size_t blob_size = GET_SHADER_SIZE(LauncherModelVS, fmt);
        if(!blob || blob_size == 0) return nullptr;
        return d->createShader(blob, blob_size, "VSMain", fmt);
    };
    auto create_model_ps = [&](plume::RenderShaderFormat fmt)->std::unique_ptr<plume::RenderShader>{
        const void* blob = GET_SHADER_BLOB(LauncherModelPS, fmt);
        size_t blob_size = GET_SHADER_SIZE(LauncherModelPS, fmt);
        if(!blob || blob_size == 0) return nullptr;
        return d->createShader(blob, blob_size, "PSMain", fmt);
    };
    auto create_shadow_vs = [&](plume::RenderShaderFormat fmt)->std::unique_ptr<plume::RenderShader>{
        const void* blob = GET_SHADER_BLOB(LauncherShadowVS, fmt);
        size_t blob_size = GET_SHADER_SIZE(LauncherShadowVS, fmt);
        if(!blob || blob_size == 0) return nullptr;
        return d->createShader(blob, blob_size, "VSMain", fmt);
    };
    auto create_shadow_ps = [&](plume::RenderShaderFormat fmt)->std::unique_ptr<plume::RenderShader>{
        const void* blob = GET_SHADER_BLOB(LauncherShadowPS, fmt);
        size_t blob_size = GET_SHADER_SIZE(LauncherShadowPS, fmt);
        if(!blob || blob_size == 0) return nullptr;
        return d->createShader(blob, blob_size, "PSMain", fmt);
    };

    auto try_shader_set = [&](plume::RenderShaderFormat fmt)->bool{
        LAUNCHER3D_TRACE("ensure_pipeline: try shaders format=%s", shader_format_name(fmt));
        auto vs_try = create_model_vs(fmt);
        auto ps_try = create_model_ps(fmt);
        auto sh_vs_try = create_shadow_vs(fmt);
        auto sh_ps_try = create_shadow_ps(fmt);
        if(vs_try && ps_try && sh_vs_try && sh_ps_try){
            s.gpu.vs = std::move(vs_try);
            s.gpu.ps = std::move(ps_try);
            s.gpu.shadow_vs = std::move(sh_vs_try);
            s.gpu.shadow_ps = std::move(sh_ps_try);
            LAUNCHER3D_TRACE("ensure_pipeline: shader set ok format=%s", shader_format_name(fmt));
            return true;
        }
        LAUNCHER3D_TRACE("ensure_pipeline: shader set failed format=%s", shader_format_name(fmt));
        return false;
    };

    bool shader_ok = false;
    shader_ok = try_shader_set(sf);
#ifdef _WIN32
    if(!shader_ok && sf != plume::RenderShaderFormat::DXIL) shader_ok = try_shader_set(plume::RenderShaderFormat::DXIL);
    if(!shader_ok && sf != plume::RenderShaderFormat::SPIRV) shader_ok = try_shader_set(plume::RenderShaderFormat::SPIRV);
#elif defined(__APPLE__)
    if(!shader_ok && sf != plume::RenderShaderFormat::METAL) shader_ok = try_shader_set(plume::RenderShaderFormat::METAL);
    if(!shader_ok && sf != plume::RenderShaderFormat::SPIRV) shader_ok = try_shader_set(plume::RenderShaderFormat::SPIRV);
#else
    if(!shader_ok && sf != plume::RenderShaderFormat::SPIRV) shader_ok = try_shader_set(plume::RenderShaderFormat::SPIRV);
#endif
    if(!shader_ok) return set_diag("createShader (model/shadow)");
    LAUNCHER3D_TRACE("ensure_pipeline: shaders ready");

    s.gpu.set_builder = std::make_unique<plume::RenderDescriptorSetBuilder>();
    s.gpu.set_builder->begin();
    s.gpu.cb_idx = s.gpu.set_builder->addConstantBuffer(0, 1);
    s.gpu.a_idx = s.gpu.set_builder->addTexture(1);
    s.gpu.n_idx = s.gpu.set_builder->addTexture(2);
    s.gpu.s_idx = s.gpu.set_builder->addTexture(3);
    s.gpu.shadow_idx = s.gpu.set_builder->addTexture(5);
    s.gpu.set_builder->addImmutableSampler(4, s.gpu.sampler.get());
    s.gpu.set_builder->addImmutableSampler(6, s.gpu.shadow_sampler.get());
    s.gpu.set_builder->end();

    s.gpu.shadow_set_builder = std::make_unique<plume::RenderDescriptorSetBuilder>();
    s.gpu.shadow_set_builder->begin();
    s.gpu.shadow_cb_idx = s.gpu.shadow_set_builder->addConstantBuffer(0, 1);
    s.gpu.shadow_set_builder->end();

    plume::RenderPipelineLayoutBuilder lb{};
    lb.begin(false, true);
    lb.addDescriptorSet(*s.gpu.set_builder);
    lb.end();
    s.gpu.layout = lb.create(d);
    if(!s.gpu.layout) return set_diag("createPipelineLayout");
    LAUNCHER3D_TRACE("ensure_pipeline: pipeline layout ok");

    plume::RenderPipelineLayoutBuilder shadow_lb{};
    shadow_lb.begin(false, true);
    shadow_lb.addDescriptorSet(*s.gpu.shadow_set_builder);
    shadow_lb.end();
    s.gpu.shadow_layout = shadow_lb.create(d);
    if (!s.gpu.shadow_layout) return set_diag("createShadowPipelineLayout");
    LAUNCHER3D_TRACE("ensure_pipeline: shadow pipeline layout ok");

    std::vector<plume::RenderInputElement> e{
        {"POSITION", 0, 0, plume::RenderFormat::R32G32B32_FLOAT, 0, offsetof(Vertex, p)},
        {"NORMAL",   0, 1, plume::RenderFormat::R32G32B32_FLOAT, 0, offsetof(Vertex, n)},
        {"TANGENT",  0, 2, plume::RenderFormat::R32G32B32A32_FLOAT, 0, offsetof(Vertex, t)},
        {"TEXCOORD", 0, 3, plume::RenderFormat::R32G32_FLOAT, 0, offsetof(Vertex, uv)}
    };
    plume::RenderGraphicsPipelineDesc pd{};
    pd.pipelineLayout=s.gpu.layout.get();
    pd.vertexShader=s.gpu.vs.get();
    pd.pixelShader=s.gpu.ps.get();
    pd.renderTargetBlend[0]=plume::RenderBlendDesc::Copy();
    pd.renderTargetCount=1;
    pd.primitiveTopology=plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    pd.cullMode=plume::RenderCullMode::BACK;
    pd.frontFace=plume::RenderFrontFace::COUNTER_CLOCKWISE;
    pd.depthClipEnabled=true;
    pd.depthFunction=plume::RenderComparisonFunction::LESS_EQUAL;
    pd.inputSlots=&s.gpu.slot;
    pd.inputSlotsCount=1;
    pd.inputElements=e.data();
    pd.inputElementsCount=(uint32_t)e.size();
    const std::array<plume::RenderFormat, 2> rt_formats = { kSwapChainFormat, plume::RenderFormat::R8G8B8A8_UNORM };
    const std::array<plume::RenderFormat, 3> depth_formats = {
        plume::RenderFormat::D32_FLOAT,
        plume::RenderFormat::D32_FLOAT_S8_UINT,
        plume::RenderFormat::UNKNOWN
    };
    plume::RenderFormat chosen_depth_format = plume::RenderFormat::UNKNOWN;
    for (plume::RenderFormat rt_f : rt_formats) {
        pd.renderTargetFormat[0] = rt_f;
        for (plume::RenderFormat depth_f : depth_formats) {
            if(depth_f == plume::RenderFormat::UNKNOWN){
                pd.depthEnabled = false;
                pd.depthWriteEnabled = false;
                pd.depthTargetFormat = plume::RenderFormat::UNKNOWN;
            } else {
                pd.depthEnabled = true;
                pd.depthWriteEnabled = true;
                pd.depthTargetFormat = depth_f;
            }
            LAUNCHER3D_TRACE(
                "ensure_pipeline: try graphics pipeline RT format=%d depth=%d depthEnabled=%s",
                static_cast<int>(rt_f),
                static_cast<int>(depth_f),
                yes_no(pd.depthEnabled)
            );
            s.gpu.pipeline = d->createGraphicsPipeline(pd);
            if (s.gpu.pipeline) {
                chosen_depth_format = depth_f;
                LAUNCHER3D_TRACE(
                    "ensure_pipeline: graphics pipeline ok RT format=%d depth=%d depthEnabled=%s",
                    static_cast<int>(rt_f),
                    static_cast<int>(depth_f),
                    yes_no(pd.depthEnabled)
                );
                break;
            }
        }
        if (s.gpu.pipeline) {
            break;
        }
    }
    if(!s.gpu.pipeline) return set_diag("createGraphicsPipeline");
    s.gpu.pipeline_depth_format = chosen_depth_format;
    s.gpu.pipeline_uses_depth = (chosen_depth_format != plume::RenderFormat::UNKNOWN);

    plume::RenderGraphicsPipelineDesc spd{};
    spd.pipelineLayout = s.gpu.shadow_layout.get();
    spd.vertexShader = s.gpu.shadow_vs.get();
    spd.pixelShader = s.gpu.shadow_ps.get();
    spd.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    spd.renderTargetCount = 1;
    spd.renderTargetFormat[0] = kShadowColorFormat;
    spd.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    spd.cullMode = plume::RenderCullMode::BACK;
    spd.frontFace = plume::RenderFrontFace::COUNTER_CLOCKWISE;
    spd.depthClipEnabled = true;
    spd.depthEnabled = true;
    spd.depthWriteEnabled = true;
    spd.depthFunction = plume::RenderComparisonFunction::LESS;
    spd.depthBias = 1;
    spd.slopeScaledDepthBias = 2.0f;
    spd.depthBiasClamp = 0.0f;
    spd.inputSlots = &s.gpu.slot;
    spd.inputSlotsCount = 1;
    spd.inputElements = e.data();
    spd.inputElementsCount = static_cast<uint32_t>(e.size());
    s.gpu.shadow_pipeline.reset();
    for (plume::RenderFormat depth_f : { plume::RenderFormat::D32_FLOAT, plume::RenderFormat::D32_FLOAT_S8_UINT }) {
        spd.depthTargetFormat = depth_f;
        s.gpu.shadow_pipeline = d->createGraphicsPipeline(spd);
        if (s.gpu.shadow_pipeline) {
            s.gpu.shadow_pipeline_depth_format = depth_f;
            break;
        }
    }
    s.gpu.shadow_pipeline_ok = (s.gpu.shadow_pipeline != nullptr);
    s.gpu.shadow_pipeline_unavailable = !s.gpu.shadow_pipeline_ok;
    if (!s.gpu.shadow_pipeline_ok) {
        s.shadow.fallback_reason = "Shadow pipeline unavailable";
        LAUNCHER3D_TRACE("ensure_pipeline: shadow pipeline unavailable, continuing without shadows");
    }

    plume::RenderCommandListType queue_type = plume::RenderCommandListType::COPY;
    s.gpu.copy_q=d->createCommandQueue(queue_type);
    LAUNCHER3D_TRACE("ensure_pipeline: create queue COPY -> %p", ptr_addr(s.gpu.copy_q.get()));
    if(!s.gpu.copy_q){
        queue_type = plume::RenderCommandListType::DIRECT;
        s.gpu.copy_q=d->createCommandQueue(queue_type);
        LAUNCHER3D_TRACE("ensure_pipeline: fallback queue DIRECT -> %p", ptr_addr(s.gpu.copy_q.get()));
    }
    if(!s.gpu.copy_q) return set_diag("createCommandQueue(COPY/DIRECT)");
    LAUNCHER3D_TRACE("ensure_pipeline: queue selected type=%s", cmd_list_type_name(queue_type));

    s.gpu.copy_l=s.gpu.copy_q->createCommandList();
    if(!s.gpu.copy_l) return set_diag("createCommandList");
    LAUNCHER3D_TRACE("ensure_pipeline: command list ok");

    s.gpu.copy_f=d->createCommandFence();
    if(!s.gpu.copy_f) return set_diag("createCommandFence");
    LAUNCHER3D_TRACE("ensure_pipeline: fence ok");

    s.gpu.pipeline_ok = true;
    s.pipeline_diag.clear();
    s.pipeline_diag_reported = false;
    LAUNCHER3D_TRACE("ensure_pipeline: success");
    return true;
}

bool cfg_visible() {
    try {
        return recompui::is_context_shown(recompui::config::get_config_context_id());
    }
    catch (...) {
        return false;
    }
}
struct DrawDecision {
    bool draw = false;
    const char* reason = "unknown";
};

DrawDecision draw_decision(const State& s){
    if(!s.enabled) return { false, "disabled" };
    if(!s.configured) return { false, "not configured" };
    if(ultramodern::is_game_started()) return { false, "game started" };
    if(s.cfg.visible_only_on_title_screen && cfg_visible()) return { false, "config context visible" };
    return { true, "ok" };
}

bool should_draw(const State& s){
    return draw_decision(s).draw;
}

struct ShadowPlanes {
    float near_plane = 0.05f;
    float far_plane = 8.0f;
};

struct ShadowAtlasFace {
    int tile_x = 0;
    int tile_y = 0;
    V3 dir{};
    V3 up{};
};

constexpr std::array<ShadowAtlasFace, kShadowFaceCount> kPointFaces = {{
    { 0, 0, { 1.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, // +X
    { 1, 0, { -1.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, // -X
    { 2, 0, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } }, // +Y
    { 0, 1, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } }, // -Y
    { 1, 1, { 0.0f, 0.0f, 1.0f }, { 0.0f, -1.0f, 0.0f } },  // +Z
    { 2, 1, { 0.0f, 0.0f, -1.0f }, { 0.0f, -1.0f, 0.0f } }, // -Z
}};

ShadowPlanes compute_shadow_planes(const State& s, const Transform& tr) {
    const M4 model = TRS(tr);
    const V3 model_center_ws = transform_point(model, s.cpu.bounds_center);
    const V3 light_pos{ s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z };

    const float max_scale_axis = max3(std::abs(tr.scale.x), std::abs(tr.scale.y), std::abs(tr.scale.z));
    const float model_radius_ws = std::max(s.cpu.bounds_radius * max_scale_axis, 0.001f);
    const float light_to_center = len(sub(model_center_ws, light_pos));

    ShadowPlanes planes{};
    planes.near_plane = std::max(s.cfg.shadow.near_plane, kShadowNearPlaneMin);
    planes.far_plane = std::max(light_to_center + model_radius_ws + 0.5f, planes.near_plane + kShadowFarPlaneMin);

    if (s.cfg.light.range > 0.0f) {
        planes.far_plane = std::min(planes.far_plane, s.cfg.light.range);
    }
    if (s.cfg.shadow.far_plane_override > 0.0f) {
        planes.far_plane = s.cfg.shadow.far_plane_override;
    }
    planes.far_plane = std::max(planes.far_plane, planes.near_plane + kShadowFarPlaneMin);
    return planes;
}

uint32_t clamp_shadow_resolution(uint32_t res) {
    if (res >= 2048U) return 2048U;
    if (res >= 1024U) return 1024U;
    return 512U;
}

bool shadows_requested(const State& s) {
    return s.cfg.shadow.enabled && (s.cfg.shadow.mode != ShadowMode::Disabled);
}

void mark_shadow_dirty(State& s, const char* reason = nullptr) {
    s.shadow.dirty = true;
    if (reason != nullptr) {
        s.shadow.fallback_reason = reason;
    }
}

bool ensure_shadow_fallback_texture(State& s) {
    if (s.gpu.tex_shadow_fallback != nullptr) {
        return true;
    }

    if (!(s.gpu.dev && s.gpu.copy_q && s.gpu.copy_l && s.gpu.copy_f)) {
        return false;
    }
    s.gpu.tex_shadow_fallback = tex_rgba(
        s.gpu.dev,
        s.gpu.copy_q.get(),
        s.gpu.copy_l.get(),
        s.gpu.copy_f.get(),
        s.gpu.upload,
        { 255, 255, 255, 255 }
    );
    return (s.gpu.tex_shadow_fallback != nullptr);
}

void clear_shadow_targets(State& s) {
    s.gpu.tex_shadow.reset();
    s.gpu.tex_shadow_depth.reset();
    s.gpu.shadow_fb.reset();
    s.shadow.resources_ready = false;
}

bool create_shadow_targets(State& s, ActiveShadowMode mode, uint32_t face_resolution) {
    clear_shadow_targets(s);
    if (!s.gpu.dev) {
        return false;
    }

    uint32_t tex_w = face_resolution;
    uint32_t tex_h = face_resolution;
    if (mode == ActiveShadowMode::PointAtlas) {
        tex_w = face_resolution * 3U;
        tex_h = face_resolution * 2U;
    }

    auto color_desc = plume::RenderTextureDesc::ColorTarget(tex_w, tex_h, kShadowColorFormat);
    auto depth_desc = plume::RenderTextureDesc::DepthTarget(tex_w, tex_h, s.gpu.shadow_pipeline_depth_format);
    s.gpu.tex_shadow = s.gpu.dev->createTexture(color_desc);
    s.gpu.tex_shadow_depth = s.gpu.dev->createTexture(depth_desc);
    if (!(s.gpu.tex_shadow && s.gpu.tex_shadow_depth)) {
        clear_shadow_targets(s);
        return false;
    }

    const plume::RenderTexture* color_attachment = s.gpu.tex_shadow.get();
    plume::RenderFramebufferDesc fb_desc{};
    fb_desc.colorAttachments = &color_attachment;
    fb_desc.colorAttachmentsCount = 1;
    fb_desc.depthAttachment = s.gpu.tex_shadow_depth.get();
    fb_desc.depthAttachmentReadOnly = false;
    s.gpu.shadow_fb = s.gpu.dev->createFramebuffer(fb_desc);
    if (!s.gpu.shadow_fb) {
        clear_shadow_targets(s);
        return false;
    }

    s.shadow.active_mode = mode;
    s.shadow.effective_resolution = face_resolution;
    s.shadow.resources_ready = true;
    s.shadow.has_signature = false;
    s.shadow.dirty = true;
    return true;
}

bool ensure_shadow_resources(State& s) {
    if (!shadows_requested(s) || !s.gpu.shadow_pipeline_ok) {
        s.shadow.active_mode = ActiveShadowMode::Disabled;
        s.shadow.resources_ready = false;
        return true;
    }

    const uint32_t requested_res = clamp_shadow_resolution(s.cfg.shadow.resolution);
    const bool reuse_current =
        s.shadow.resources_ready &&
        (s.shadow.effective_resolution == requested_res) &&
        ((s.shadow.active_mode == ActiveShadowMode::PointAtlas && s.cfg.shadow.mode != ShadowMode::SpotOnly) ||
         (s.shadow.active_mode == ActiveShadowMode::Spot && s.cfg.shadow.mode == ShadowMode::SpotOnly));
    if (reuse_current) {
        return true;
    }

    struct Candidate { ActiveShadowMode mode; uint32_t res; const char* label; };
    std::vector<Candidate> candidates;
    candidates.reserve(6);

    const auto add_candidate = [&](ActiveShadowMode mode, uint32_t res, const char* label) {
        if ((res != 512U) && (res != 1024U) && (res != 2048U)) {
            return;
        }
        if (res > requested_res) {
            return;
        }
        candidates.push_back({ mode, res, label });
    };

    if (s.cfg.shadow.mode == ShadowMode::SpotOnly) {
        if (requested_res >= 2048U) {
            add_candidate(ActiveShadowMode::Spot, 2048U, "Spot@2048");
            add_candidate(ActiveShadowMode::Spot, 1024U, "Spot@1024");
        } else if (requested_res >= 1024U) {
            add_candidate(ActiveShadowMode::Spot, 1024U, "Spot@1024");
        } else {
            add_candidate(ActiveShadowMode::Spot, 512U, "Spot@512");
        }
    } else {
        if (requested_res >= 2048U) {
            // Required fallback chain: Point2048 -> Point1024 -> Spot2048 -> Spot1024.
            add_candidate(ActiveShadowMode::PointAtlas, 2048U, "Point@2048");
            add_candidate(ActiveShadowMode::PointAtlas, 1024U, "Point@1024");
            add_candidate(ActiveShadowMode::Spot, 2048U, "Spot@2048");
            add_candidate(ActiveShadowMode::Spot, 1024U, "Spot@1024");
        } else if (requested_res >= 1024U) {
            add_candidate(ActiveShadowMode::PointAtlas, 1024U, "Point@1024");
            add_candidate(ActiveShadowMode::Spot, 1024U, "Spot@1024");
        } else {
            add_candidate(ActiveShadowMode::PointAtlas, 512U, "Point@512");
            add_candidate(ActiveShadowMode::Spot, 512U, "Spot@512");
        }
    }

    bool created = false;
    for (const Candidate& c : candidates) {
        if (create_shadow_targets(s, c.mode, c.res)) {
            s.shadow.fallback_reason = c.label;
            created = true;
            break;
        }
    }

    if (!created) {
        clear_shadow_targets(s);
        s.shadow.active_mode = ActiveShadowMode::Disabled;
        s.shadow.resources_ready = false;
        s.shadow.fallback_reason = "Shadow disabled (resource fallback)";
    }
    return true;
}

bool shadow_signature_changed(const State& s, const Transform& tr) {
    if (!s.shadow.has_signature) return true;
    if (!near_equal_transform(s.shadow.last_transform, tr)) return true;
    if (!near_equal_light(s.shadow.last_light, s.cfg.light)) return true;
    if (!near_equal_shadow_cfg(s.shadow.last_shadow_cfg, s.cfg.shadow)) return true;
    return false;
}

void update_shadow_signature(State& s, const Transform& tr) {
    s.shadow.last_transform = tr;
    s.shadow.last_light = s.cfg.light;
    s.shadow.last_shadow_cfg = s.cfg.shadow;
    s.shadow.has_signature = true;
}

struct PrimaryColorAttachment {
    const plume::RenderTexture* texture = nullptr;
    const plume::RenderTextureView* texture_view = nullptr;
    plume::RenderMultisampling multisampling{};
};

bool try_get_primary_color_attachment(const plume::RenderFramebuffer* src_fb, PrimaryColorAttachment& out) {
    if (src_fb == nullptr) {
        return false;
    }

#ifdef _WIN32
    if (const auto* d3d_fb = dynamic_cast<const plume::D3D12Framebuffer*>(src_fb)) {
        if (!d3d_fb->colorTargets.empty() && (d3d_fb->colorTargets[0] != nullptr)) {
            out.texture = d3d_fb->colorTargets[0];
            out.multisampling = d3d_fb->colorTargets[0]->desc.multisampling;
            return true;
        }
    }
#endif

    if (const auto* vk_fb = dynamic_cast<const plume::VulkanFramebuffer*>(src_fb)) {
        if (!vk_fb->colorAttachments.empty() && (vk_fb->colorAttachments[0] != nullptr)) {
            out.texture = vk_fb->colorAttachments[0];
            out.multisampling = vk_fb->colorAttachments[0]->desc.multisampling;
            return true;
        }
    }

#if defined(__APPLE__)
    if (const auto* metal_fb = dynamic_cast<const plume::MetalFramebuffer*>(src_fb)) {
        if (!metal_fb->colorAttachments.empty()) {
            const plume::MetalAttachment& attachment = metal_fb->colorAttachments[0];
            out.multisampling = plume::RenderMultisampling(attachment.sampleCount);
            if (attachment.texture != nullptr) {
                out.texture = attachment.texture;
                return true;
            }

            if (attachment.textureView != nullptr) {
                out.texture_view = attachment.textureView;
                return true;
            }
        }
    }
#endif

    return false;
}

plume::RenderFramebuffer* resolve_framebuffer_for_3d_pass(State& s, plume::RenderFramebuffer* src_fb, plume::RenderCommandList* list){
    if((src_fb == nullptr) || !s.gpu.pipeline_uses_depth || (s.gpu.pipeline_depth_format == plume::RenderFormat::UNKNOWN)){
        return src_fb;
    }

    PrimaryColorAttachment color_attachment{};
    if (!try_get_primary_color_attachment(src_fb, color_attachment)) {
        return src_fb;
    }

    auto recreate_depth_fb = [&](){
        s.gpu.fb_with_depth_by_src_fb.erase(src_fb);
        s.gpu.depth_by_src_fb.erase(src_fb);

        auto depth_tex = s.gpu.dev->createTexture(
            plume::RenderTextureDesc::DepthTarget(
                src_fb->getWidth(),
                src_fb->getHeight(),
                s.gpu.pipeline_depth_format,
                color_attachment.multisampling
            )
        );
        if(!depth_tex){
            LAUNCHER3D_TRACE("resolve_framebuffer_for_3d_pass: depth texture creation failed, fallback without depth");
            s.gpu.pipeline_uses_depth = false;
            s.gpu.pipeline_depth_format = plume::RenderFormat::UNKNOWN;
            return false;
        }

        plume::RenderFramebufferDesc fb_desc{};
        const plume::RenderTexture* color_texture_ptr = color_attachment.texture;
        const plume::RenderTextureView* color_view_ptr = color_attachment.texture_view;
        if (color_texture_ptr != nullptr) {
            fb_desc.colorAttachments = &color_texture_ptr;
            fb_desc.colorAttachmentsCount = 1;
        } else if (color_view_ptr != nullptr) {
            fb_desc.colorAttachmentViews = &color_view_ptr;
            fb_desc.colorAttachmentsCount = 1;
        } else {
            return false;
        }
        fb_desc.depthAttachment = depth_tex.get();
        fb_desc.depthAttachmentReadOnly = false;
        auto depth_fb = s.gpu.dev->createFramebuffer(fb_desc);
        if(!depth_fb){
            LAUNCHER3D_TRACE("resolve_framebuffer_for_3d_pass: framebuffer creation failed, fallback without depth");
            s.gpu.pipeline_uses_depth = false;
            s.gpu.pipeline_depth_format = plume::RenderFormat::UNKNOWN;
            return false;
        }

        s.gpu.depth_by_src_fb[src_fb] = std::move(depth_tex);
        s.gpu.fb_with_depth_by_src_fb[src_fb] = std::move(depth_fb);
        return true;
    };

    bool needs_recreate = false;
    auto depth_it = s.gpu.depth_by_src_fb.find(src_fb);
    auto fb_it = s.gpu.fb_with_depth_by_src_fb.find(src_fb);
    if((depth_it == s.gpu.depth_by_src_fb.end()) || (fb_it == s.gpu.fb_with_depth_by_src_fb.end())){
        needs_recreate = true;
    }
    else if((fb_it->second->getWidth() != src_fb->getWidth()) || (fb_it->second->getHeight() != src_fb->getHeight())){
        needs_recreate = true;
    }

    if(needs_recreate && !recreate_depth_fb()){
        return src_fb;
    }

    auto depth_tex_it = s.gpu.depth_by_src_fb.find(src_fb);
    auto depth_fb_it = s.gpu.fb_with_depth_by_src_fb.find(src_fb);
    if((depth_tex_it == s.gpu.depth_by_src_fb.end()) || (depth_fb_it == s.gpu.fb_with_depth_by_src_fb.end())){
        return src_fb;
    }

    plume::RenderTextureBarrier depth_barrier(depth_tex_it->second.get(), plume::RenderTextureLayout::DEPTH_WRITE);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, &depth_barrier, 1);
    return depth_fb_it->second.get();
}

V3 compute_model_center_ws(const State& s, const Transform& tr) {
    const M4 model = TRS(tr);
    return transform_point(model, s.cpu.bounds_center);
}

M4 compute_spot_shadow_view_proj(const State& s, const Transform& tr, const ShadowPlanes& planes) {
    const V3 light_pos{ s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z };
    const V3 center_ws = compute_model_center_ws(s, tr);
    V3 forward = norm(sub(center_ws, light_pos));
    if (len(forward) <= 1e-6f) {
        forward = { 0.0f, -1.0f, 0.0f };
    }
    const V3 world_up = (std::abs(dot(forward, { 0.0f, 1.0f, 0.0f })) > 0.95f) ? V3{ 0.0f, 0.0f, 1.0f } : V3{ 0.0f, 1.0f, 0.0f };

    const float model_radius_ws = std::max(s.cpu.bounds_radius * max3(std::abs(tr.scale.x), std::abs(tr.scale.y), std::abs(tr.scale.z)), 0.01f);
    const float dist = std::max(len(sub(center_ws, light_pos)), 0.01f);
    const float cone = 2.0f * std::asin(std::clamp(model_radius_ws / dist, 0.0f, 1.0f));
    const float fov = std::clamp(cone * 1.30f, 35.0f * (std::numbers::pi_v<float> / 180.0f), 120.0f * (std::numbers::pi_v<float> / 180.0f));

    const M4 light_view = LookAt(light_pos, add(light_pos, forward), world_up);
    const M4 light_proj = P(fov, 1.0f, planes.near_plane, planes.far_plane);
    return Mul(light_view, light_proj);
}

M4 compute_point_shadow_view_proj(const State& s, uint32_t face_index, const ShadowPlanes& planes) {
    const ShadowAtlasFace& face = kPointFaces[face_index];
    const V3 light_pos{ s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z };
    const M4 light_view = LookAt(light_pos, add(light_pos, face.dir), face.up);
    const M4 light_proj = P(90.0f * (std::numbers::pi_v<float> / 180.0f), 1.0f, planes.near_plane, planes.far_plane);
    return Mul(light_view, light_proj);
}

bool draw_shadow_pass(State& s, plume::RenderCommandList* list, const Transform& tr) {
    if (!s.shadow.resources_ready || (s.gpu.shadow_fb == nullptr) || (s.gpu.shadow_set == nullptr) || (s.gpu.shadow_cb == nullptr)) {
        return false;
    }

    const ShadowPlanes planes = compute_shadow_planes(s, tr);
    const uint32_t face_res = s.shadow.effective_resolution;
    const uint32_t tex_w = s.gpu.shadow_fb->getWidth();
    const uint32_t tex_h = s.gpu.shadow_fb->getHeight();
    if ((face_res == 0U) || (tex_w == 0U) || (tex_h == 0U)) {
        return false;
    }

    plume::RenderTextureBarrier write_barriers[] = {
        { s.gpu.tex_shadow.get(), plume::RenderTextureLayout::COLOR_WRITE },
        { s.gpu.tex_shadow_depth.get(), plume::RenderTextureLayout::DEPTH_WRITE }
    };
    list->barriers(plume::RenderBarrierStage::GRAPHICS, write_barriers, static_cast<uint32_t>(std::size(write_barriers)));

    list->setFramebuffer(s.gpu.shadow_fb.get());
    list->setGraphicsPipelineLayout(s.gpu.shadow_layout.get());
    list->setPipeline(s.gpu.shadow_pipeline.get());
    list->setGraphicsDescriptorSet(s.gpu.shadow_set.get(), 0);

    plume::RenderVertexBufferView vbv(s.gpu.vb.get(), static_cast<uint64_t>(s.cpu.vertices.size()) * sizeof(Vertex));
    plume::RenderIndexBufferView ibv(s.gpu.ib.get(), static_cast<uint64_t>(s.cpu.indices.size()) * sizeof(uint32_t), plume::RenderFormat::R32_UINT);
    list->setVertexBuffers(0, &vbv, 1, &s.gpu.slot);
    list->setIndexBuffer(&ibv);

    ShadowPassConstants shadow_cb{};
    shadow_cb.model = TRS(tr);
    shadow_cb.light_pos_far = { s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z, planes.far_plane };

    const bool point_mode = (s.shadow.active_mode == ActiveShadowMode::PointAtlas);
    const uint32_t pass_count = point_mode ? kShadowFaceCount : 1U;
    for (uint32_t pass = 0; pass < pass_count; pass++) {
        int tile_x = 0;
        int tile_y = 0;
        if (point_mode) {
            tile_x = kPointFaces[pass].tile_x;
            tile_y = kPointFaces[pass].tile_y;
            shadow_cb.light_view_proj = compute_point_shadow_view_proj(s, pass, planes);
        } else {
            shadow_cb.light_view_proj = compute_spot_shadow_view_proj(s, tr, planes);
        }

        const int32_t left = static_cast<int32_t>(tile_x * static_cast<int>(face_res));
        const int32_t top = static_cast<int32_t>(tile_y * static_cast<int>(face_res));
        const int32_t right = left + static_cast<int32_t>(face_res);
        const int32_t bottom = top + static_cast<int32_t>(face_res);
        const plume::RenderRect clear_rect{ left, top, right, bottom };
        const plume::RenderViewport vp{
            static_cast<float>(left),
            static_cast<float>(top),
            static_cast<float>(face_res),
            static_cast<float>(face_res),
            0.0f,
            1.0f
        };
        const plume::RenderRect scissor{ left, top, right, bottom };

        list->clearColor(0, plume::RenderColor(1.0f, 1.0f, 1.0f, 1.0f), &clear_rect, 1);
        list->clearDepth(true, 1.0f, &clear_rect, 1);
        list->setViewports(vp);
        list->setScissors(scissor);

        void* cb_ptr = s.gpu.shadow_cb->map();
        if (cb_ptr == nullptr) {
            return false;
        }
        std::memcpy(cb_ptr, &shadow_cb, sizeof(shadow_cb));
        s.gpu.shadow_cb->unmap();

        list->drawIndexedInstanced(static_cast<uint32_t>(s.gpu.index_count), 1, 0, 0, 0);
    }

    plume::RenderTextureBarrier read_barrier(s.gpu.tex_shadow.get(), plume::RenderTextureLayout::SHADER_READ);
    list->barriers(plume::RenderBarrierStage::GRAPHICS, &read_barrier, 1);
    s.shadow.render_count++;
    return true;
}

bool update_shadows_if_needed(State& s, plume::RenderCommandList* list, const Transform& tr) {
    if (!shadows_requested(s) || !s.gpu.shadow_pipeline_ok) {
        s.shadow.active_mode = ActiveShadowMode::Disabled;
        s.shadow.resources_ready = false;
        return true;
    }

    if (!ensure_shadow_resources(s)) {
        return true;
    }
    if (!s.shadow.resources_ready) {
        return true;
    }

    const bool intro_active = !(s.intro_finished && s.cfg.intro.play_once);
    if (intro_active || shadow_signature_changed(s, tr)) {
        s.shadow.dirty = true;
    }

    if (!s.shadow.dirty) {
        return true;
    }

    if (!draw_shadow_pass(s, list, tr)) {
        s.shadow.fallback_reason = "Shadow draw failed";
        return false;
    }

    update_shadow_signature(s, tr);
    s.shadow.dirty = false;
    return true;
}

#if !defined(NDEBUG)
void log_loaded_model_info(const Config& cfg, const CpuModel& cpu){
    V3 mn{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    V3 mx{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };

    for(const Vertex& v : cpu.vertices){
        mn.x = std::min(mn.x, v.p.x); mn.y = std::min(mn.y, v.p.y); mn.z = std::min(mn.z, v.p.z);
        mx.x = std::max(mx.x, v.p.x); mx.y = std::max(mx.y, v.p.y); mx.z = std::max(mx.z, v.p.z);
    }

    const size_t tri_count = cpu.indices.empty() ? (cpu.vertices.size() / 3) : (cpu.indices.size() / 3);
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: loaded model '%s'\n", cfg.glb_path.string().c_str());
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: vertices=%zu indices=%zu triangles=%zu\n", cpu.vertices.size(), cpu.indices.size(), tri_count);
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: bounds min=(%.3f, %.3f, %.3f) max=(%.3f, %.3f, %.3f)\n", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: textures albedo=%s (%zu bytes), normal=%s (%zu bytes), specular=%s (%zu bytes)\n",
        cpu.albedo.empty() ? "fallback" : "loaded", cpu.albedo.size(),
        cpu.normal.empty() ? "fallback" : "loaded", cpu.normal.size(),
        cpu.spec.empty() ? "fallback" : "loaded", cpu.spec.size());
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: gltf node=%s det3=%.4f winding_flipped=%s normals_src=%s normals_generated=%s\n",
        cpu.had_owner_node ? "yes" : "no",
        cpu.owner_node_det3,
        cpu.flipped_winding ? "yes" : "no",
        cpu.source_had_normals ? "yes" : "no",
        cpu.generated_smooth_normals ? "yes" : "no");
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: material base=(%.3f, %.3f, %.3f, %.3f) spec=(%.3f, %.3f, %.3f, %.3f) specFactor=%.3f\n",
        cpu.base_color.x, cpu.base_color.y, cpu.base_color.z, cpu.base_color.w,
        cpu.spec_color.x, cpu.spec_color.y, cpu.spec_color.z, cpu.spec_color.w, cpu.spec_factor);
    std::fflush(stdout);
}

recompui::Slider* mk_slider(
    recompui::Element* p,
    const std::string& t,
    double mn,
    double mx,
    double stp,
    double init,
    const std::function<void(double)>& cb
) {
    auto c = recompui::get_current_context();
    auto* row = c.create_element<recompui::Element>(p);
    row->set_display(recompui::Display::Flex);
    row->set_flex_direction(recompui::FlexDirection::Column);
    row->set_flex_shrink(0.0f);
    row->set_min_height(40.0f);
    row->set_margin_bottom(5.0f);

    c.create_element<recompui::Label>(row, t, recompui::LabelStyle::Annotation);

    auto* sld = c.create_element<recompui::Slider>(row, recompui::SliderType::Double);
    sld->set_width(100.0f, recompui::Unit::Percent);
    sld->set_min_value(mn);
    sld->set_max_value(mx);
    sld->set_step_value(stp);
    sld->set_precision((stp < 1.0) ? 3 : 1);
    sld->set_value(init);
    sld->add_value_changed_callback(cb);
    return sld;
}
void ensure_panel(State& s,recompui::Element* menu_container){
    if(s.panel.built||!menu_container) return;
    auto* mc=menu_container;

    auto c=recompui::get_current_context();
    s.panel.root=c.create_element<recompui::Element>(mc);
    s.panel.root->set_position(recompui::Position::Absolute);
    s.panel.root->set_top(s.panel.top_dp);
    s.panel.root->set_left(s.panel.left_dp);
    s.panel.root->set_width(520);
    s.panel.root->set_min_width(500);
    s.panel.root->set_max_width(58.0f, recompui::Unit::Percent);
    s.panel.root->set_max_height(96.0f, recompui::Unit::Percent);
    s.panel.root->set_padding(10);
    s.panel.root->set_border_radius(10);
    s.panel.root->set_background_color({0,0,0,170});

    auto* h=c.create_element<recompui::Clickable>(s.panel.root, true);
    h->set_display(recompui::Display::Flex);
    h->set_flex_direction(recompui::FlexDirection::Row);
    h->set_flex_shrink(0.0f);
    h->set_justify_content(recompui::JustifyContent::SpaceBetween);
    h->set_width(100.0f, recompui::Unit::Percent);
    h->set_margin_bottom(6);
    h->add_dragged_callback([&s](float x, float y, recompui::DragPhase phase){
        if(phase == recompui::DragPhase::Start){
            std::lock_guard lock(s.mx);
            if(!s.panel.root){
                return;
            }

            s.panel.drag_start_mouse_x = x;
            s.panel.drag_start_mouse_y = y;
            s.panel.drag_start_left_dp = s.panel.left_dp;
            s.panel.drag_start_top_dp = s.panel.top_dp;
            return;
        }

        if(phase != recompui::DragPhase::Move){
            return;
        }

        recompui::Element* root = nullptr;
        float drag_start_left_dp = 0.0f;
        float drag_start_top_dp = 0.0f;
        float drag_start_mouse_x = 0.0f;
        float drag_start_mouse_y = 0.0f;
        {
            std::lock_guard lock(s.mx);
            root = s.panel.root;
            if(!root){
                return;
            }

            drag_start_left_dp = s.panel.drag_start_left_dp;
            drag_start_top_dp = s.panel.drag_start_top_dp;
            drag_start_mouse_x = s.panel.drag_start_mouse_x;
            drag_start_mouse_y = s.panel.drag_start_mouse_y;
        }

        const float dp_to_px = std::max(root->get_dp_to_pixel_ratio(), 0.001f);
        float new_left_dp = drag_start_left_dp + ((x - drag_start_mouse_x) / dp_to_px);
        float new_top_dp = drag_start_top_dp + ((y - drag_start_mouse_y) / dp_to_px);

        if(auto* parent = root->get_parent()){
            const float parent_w = parent->get_client_width();
            const float parent_h = parent->get_client_height();
            const float panel_w = root->get_client_width();
            if((parent_w > 0.0f) && (panel_w > 0.0f)){
                const float min_left = 16.0f - panel_w;
                const float max_left = parent_w - 16.0f;
                new_left_dp = std::clamp(new_left_dp, min_left, max_left);
            }
            if(parent_h > 0.0f){
                const float min_top = 0.0f;
                const float max_top = std::max(0.0f, parent_h - 24.0f);
                new_top_dp = std::clamp(new_top_dp, min_top, max_top);
            }
        }

        {
            std::lock_guard lock(s.mx);
            s.panel.left_dp = new_left_dp;
            s.panel.top_dp = new_top_dp;
        }

        root->set_left(new_left_dp);
        root->set_top(new_top_dp);
    });

    c.create_element<recompui::Label>(h, "3D Debug", recompui::LabelStyle::Small);
    auto* tgl = c.create_element<recompui::Button>(h, "Open", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    s.panel.content = c.create_element<recompui::Element>(s.panel.root);
    s.panel.content->set_display(recompui::Display::Flex);
    s.panel.content->set_flex_direction(recompui::FlexDirection::Column);
    s.panel.content->set_flex_shrink(0.0f);
    s.panel.content->set_max_height(560.0f);
    s.panel.content->set_overflow_y(recompui::Overflow::Auto);
    s.panel.content->set_overflow_x(recompui::Overflow::Hidden);
    s.panel.content->set_padding_right(8.0f);
    s.panel.content->display_hide();
    tgl->add_pressed_callback([&s, tgl]() {
        bool open = false;
        recompui::Element* content = nullptr;
        {
            std::lock_guard lock(s.mx);
            s.panel.open = !s.panel.open;
            open = s.panel.open;
            content = s.panel.content;
        }

        tgl->set_text(open ? "Close" : "Open");
        if (content != nullptr) {
            if (open) {
                content->display_show();
            } else {
                content->display_hide();
            }
        }
    });
    s.panel.status = c.create_element<recompui::Label>(s.panel.content, "Status", recompui::LabelStyle::Annotation);
    s.panel.status->set_flex_shrink(0.0f);
    s.panel.status->set_margin_bottom(4);

    auto* cam_row = c.create_element<recompui::Element>(s.panel.content);
    cam_row->set_display(recompui::Display::Flex);
    cam_row->set_flex_direction(recompui::FlexDirection::Row);
    cam_row->set_flex_shrink(0.0f);
    cam_row->set_justify_content(recompui::JustifyContent::SpaceBetween);
    cam_row->set_margin_bottom(4.0f);

    auto* cam_hold = c.create_element<recompui::Clickable>(cam_row, true);
    cam_hold->set_flex_grow(1.0f);
    cam_hold->set_display(recompui::Display::Flex);
    cam_hold->set_flex_direction(recompui::FlexDirection::Row);
    cam_hold->set_align_items(recompui::AlignItems::Center);
    cam_hold->set_padding_left(8.0f);
    cam_hold->set_padding_right(8.0f);
    cam_hold->set_height(28.0f);
    cam_hold->set_border_width(1.0f);
    cam_hold->set_border_radius(8.0f);
    cam_hold->set_border_color({ 255, 194, 0, 255 });
    cam_hold->set_background_color({ 70, 45, 0, 170 });
    c.create_element<recompui::Label>(cam_hold, "Hold+Drag Camera", recompui::LabelStyle::Annotation);
    cam_hold->add_dragged_callback([&s](float x, float y, recompui::DragPhase phase) {
        std::lock_guard lock(s.mx);
        if (phase == recompui::DragPhase::Start) {
            s.panel.camera_drag_start_x = x;
            s.panel.camera_drag_start_y = y;
            s.panel.camera_drag_start_yaw = s.panel.camera_yaw_deg;
            s.panel.camera_drag_start_pitch = s.panel.camera_pitch_deg;
            return;
        }
        if (phase == recompui::DragPhase::Move) {
            constexpr float kOrbitSensitivity = 0.20f;
            const float dx = x - s.panel.camera_drag_start_x;
            const float dy = y - s.panel.camera_drag_start_y;
            s.panel.camera_yaw_deg = s.panel.camera_drag_start_yaw + dx * kOrbitSensitivity;
            s.panel.camera_pitch_deg = std::clamp(s.panel.camera_drag_start_pitch - dy * kOrbitSensitivity, -89.0f, 89.0f);
        }
    });

    auto* cam_toggle = c.create_element<recompui::Button>(cam_row, "Cam On/Off", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    cam_toggle->set_margin_left(8.0f);
    cam_toggle->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        s.panel.free_camera_enabled = !s.panel.free_camera_enabled;
    });

    auto* cam_reset_row = c.create_element<recompui::Element>(s.panel.content);
    cam_reset_row->set_display(recompui::Display::Flex);
    cam_reset_row->set_flex_direction(recompui::FlexDirection::Row);
    cam_reset_row->set_flex_shrink(0.0f);
    cam_reset_row->set_justify_content(recompui::JustifyContent::SpaceBetween);
    cam_reset_row->set_margin_bottom(4.0f);

    auto* cam_reset = c.create_element<recompui::Button>(cam_reset_row, "Cam Reset", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    cam_reset->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        s.panel.camera_yaw_deg = 0.0f;
        s.panel.camera_pitch_deg = 0.0f;
        s.panel.camera_distance = kCameraZ;
        s.panel.camera_fov_deg = 55.0f;
        s.panel.camera_target_x = 0.0f;
        s.panel.camera_target_y = 0.0f;
        s.panel.camera_target_z = 0.0f;
    });

    auto* marker_toggle = c.create_element<recompui::Button>(cam_reset_row, "Light Gizmo", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    marker_toggle->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        s.panel.show_light_marker = !s.panel.show_light_marker;
    });

    const auto add_float_slider = [&](const std::string& title, double min_value, double max_value, double step_value, float initial, const std::function<void(float)>& setter) {
        mk_slider(s.panel.content, title, min_value, max_value, step_value, initial, [setter](double v) {
            setter(static_cast<float>(v));
        });
    };
    const auto add_shadow_float = [&](const std::string& title, double min_value, double max_value, double step_value, float initial, const std::function<void(float)>& setter) {
        add_float_slider(title, min_value, max_value, step_value, initial, [&s, setter](float v) {
            setter(v);
            mark_shadow_dirty(s);
        });
    };

    add_float_slider("Cam Dist", 0.2, 24.0, 0.01, s.panel.camera_distance, [&s](float v) {
        std::lock_guard lock(s.mx);
        s.panel.camera_distance = std::max(0.2f, v);
    });
    add_float_slider("Cam FOV", 20.0, 110.0, 0.1, s.panel.camera_fov_deg, [&s](float v) {
        std::lock_guard lock(s.mx);
        s.panel.camera_fov_deg = std::clamp(v, 20.0f, 110.0f);
    });
    add_float_slider("Cam Target X", -8.0, 8.0, 0.01, s.panel.camera_target_x, [&s](float v) {
        std::lock_guard lock(s.mx);
        s.panel.camera_target_x = v;
    });
    add_float_slider("Cam Target Y", -8.0, 8.0, 0.01, s.panel.camera_target_y, [&s](float v) {
        std::lock_guard lock(s.mx);
        s.panel.camera_target_y = v;
    });
    add_float_slider("Cam Target Z", -12.0, 8.0, 0.01, s.panel.camera_target_z, [&s](float v) {
        std::lock_guard lock(s.mx);
        s.panel.camera_target_z = v;
    });
    add_float_slider("Light Gizmo Size", 0.05, 8.0, 0.01, s.panel.light_marker_scale, [&s](float v) {
        std::lock_guard lock(s.mx);
        s.panel.light_marker_scale = std::clamp(v, 0.05f, 8.0f);
    });

    add_shadow_float("Pos X", -6.0, 6.0, 0.01, s.cfg.target_transform.position.x, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.position.x = v; });
    add_shadow_float("Pos Y", -6.0, 6.0, 0.01, s.cfg.target_transform.position.y, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.position.y = v; });
    add_shadow_float("Pos Z", -12.0, 4.0, 0.01, s.cfg.target_transform.position.z, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.position.z = v; });
    add_shadow_float("Rot X", -180.0, 180.0, 0.1, s.cfg.target_transform.rotation_deg.pitch, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.rotation_deg.pitch = v; });
    add_shadow_float("Rot Y", -180.0, 180.0, 0.1, s.cfg.target_transform.rotation_deg.yaw, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.rotation_deg.yaw = v; });
    add_shadow_float("Rot Z", -180.0, 180.0, 0.1, s.cfg.target_transform.rotation_deg.roll, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.rotation_deg.roll = v; });

    mk_slider(s.panel.content, "Scale", 0.01, 4.0, 0.01, s.cfg.target_transform.scale.x, [&s](double v) {
        const float uniform_scale = static_cast<float>(v);
        std::lock_guard lock(s.mx);
        s.cfg.target_transform.scale = { uniform_scale, uniform_scale, uniform_scale };
        mark_shadow_dirty(s);
    });
    add_shadow_float("Light X", -10.0, 10.0, 0.01, s.cfg.light.position_ws.x, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.position_ws.x = v; });
    add_shadow_float("Light Y", -10.0, 10.0, 0.01, s.cfg.light.position_ws.y, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.position_ws.y = v; });
    add_shadow_float("Light Z", -10.0, 10.0, 0.01, s.cfg.light.position_ws.z, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.position_ws.z = v; });
    add_shadow_float("Light Range", 0.1, 50.0, 0.1, s.cfg.light.range, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.range = v; });
    add_shadow_float("Light Int", 0.0, 10.0, 0.01, s.cfg.light.intensity, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.intensity = v; });
    add_shadow_float("Ambient", 0.0, 2.0, 0.01, s.cfg.light.ambient_intensity, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.ambient_intensity = v; });
    add_shadow_float("Light R", 0.0, 4.0, 0.01, s.cfg.light.color.x, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.color.x = v; });
    add_shadow_float("Light G", 0.0, 4.0, 0.01, s.cfg.light.color.y, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.color.y = v; });
    add_shadow_float("Light B", 0.0, 4.0, 0.01, s.cfg.light.color.z, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.color.z = v; });
    add_shadow_float("Shadow Str", 0.0, 1.0, 0.01, s.cfg.shadow.strength, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.shadow.strength = v; });
    add_shadow_float("Shadow Bias", 0.0, 0.02, 0.0001, s.cfg.shadow.depth_bias, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.shadow.depth_bias = v; });
    add_shadow_float("Normal Bias", 0.0, 0.2, 0.0005, s.cfg.shadow.normal_bias, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.shadow.normal_bias = v; });
    add_shadow_float("Shadow Soft", 0.1, 4.0, 0.01, s.cfg.shadow.softness, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.shadow.softness = v; });
    add_shadow_float("Shadow Near", 0.01, 2.0, 0.01, s.cfg.shadow.near_plane, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.shadow.near_plane = v; });
    add_shadow_float("Shadow FarOv", 0.0, 50.0, 0.1, s.cfg.shadow.far_plane_override, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.shadow.far_plane_override = v; });
    add_float_slider("Intro Time", 0.1, 8.0, 0.01, s.cfg.intro.duration_sec, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.intro.duration_sec = v; });
    add_float_slider("Overshoot", 0.0, 1.0, 0.01, s.cfg.intro.overshoot, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.intro.overshoot = v; });
    add_float_slider("Damping", 0.0, 20.0, 0.05, s.cfg.intro.damping, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.intro.damping = v; });

    auto* shadow_row = c.create_element<recompui::Element>(s.panel.content);
    shadow_row->set_display(recompui::Display::Flex);
    shadow_row->set_flex_direction(recompui::FlexDirection::Row);
    shadow_row->set_flex_shrink(0.0f);
    shadow_row->set_justify_content(recompui::JustifyContent::SpaceBetween);

    auto* b_shadow_toggle = c.create_element<recompui::Button>(shadow_row, "Shadow On/Off", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    b_shadow_toggle->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        s.cfg.shadow.enabled = !s.cfg.shadow.enabled;
        mark_shadow_dirty(s);
    });

    auto* b_shadow_mode = c.create_element<recompui::Button>(shadow_row, "Mode Cycle", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    b_shadow_mode->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        if (s.cfg.shadow.mode == ShadowMode::PointCubePreferred) {
            s.cfg.shadow.mode = ShadowMode::SpotOnly;
        } else if (s.cfg.shadow.mode == ShadowMode::SpotOnly) {
            s.cfg.shadow.mode = ShadowMode::Disabled;
        } else {
            s.cfg.shadow.mode = ShadowMode::PointCubePreferred;
        }
        mark_shadow_dirty(s);
    });

    mk_slider(s.panel.content, "Shadow Res", 512.0, 2048.0, 512.0, static_cast<double>(s.cfg.shadow.resolution), [&s](double v) {
        std::lock_guard lock(s.mx);
        s.cfg.shadow.resolution = clamp_shadow_resolution(static_cast<uint32_t>(v));
        mark_shadow_dirty(s);
    });
    auto* row = c.create_element<recompui::Element>(s.panel.content);
    row->set_display(recompui::Display::Flex);
    row->set_flex_direction(recompui::FlexDirection::Row);
    row->set_flex_shrink(0.0f);
    row->set_justify_content(recompui::JustifyContent::SpaceBetween);

    auto* b_reset_pose = c.create_element<recompui::Button>(row, "Reset Pose", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    b_reset_pose->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        s.cfg.target_transform = s.cfg_initial.target_transform;
        mark_shadow_dirty(s);
    });

    auto* b0 = c.create_element<recompui::Button>(row, "Reset Intro", recompui::ButtonStyle::Warning, recompui::ButtonSize::Small);
    b0->add_pressed_callback([]() {
        reset_intro();
    });

    auto* b1 = c.create_element<recompui::Button>(row, "Copy C++", recompui::ButtonStyle::Success, recompui::ButtonSize::Small);
    b1->add_pressed_callback([]() {
        copy_text_to_clipboard(make_cpp_initializer_snippet());
    });

    s.panel.built = true;
}
#endif

bool create_gpu_resources_if_needed(State& s) {
    if (s.gpu.ready) {
        return true;
    }

    auto* d = s.gpu.dev;
    const uint64_t vb_sz = static_cast<uint64_t>(s.cpu.vertices.size()) * sizeof(Vertex);
    const uint64_t ib_sz = static_cast<uint64_t>(s.cpu.indices.size()) * sizeof(uint32_t);
    std::vector<Vertex> marker_vertices;
    std::vector<uint32_t> marker_indices;
    build_debug_light_marker_mesh(marker_vertices, marker_indices);
    const uint64_t marker_vb_sz = static_cast<uint64_t>(marker_vertices.size()) * sizeof(Vertex);
    const uint64_t marker_ib_sz = static_cast<uint64_t>(marker_indices.size()) * sizeof(uint32_t);
    const uint64_t cb_sz = align_up_u64(static_cast<uint64_t>(sizeof(Constants)), 256);
    const uint64_t shadow_cb_sz = align_up_u64(static_cast<uint64_t>(sizeof(ShadowPassConstants)), 256);

    if (!vb_sz || !ib_sz) {
        LAUNCHER3D_TRACE("hook_draw: skipped GPU resource creation because vb/ib size is zero");
        return false;
    }

    LAUNCHER3D_TRACE(
        "hook_draw: creating GPU resources vb=%lluB ib=%lluB marker_vb=%lluB marker_ib=%lluB cb=%lluB(aligned) shadow_cb=%lluB(aligned)",
        static_cast<unsigned long long>(vb_sz),
        static_cast<unsigned long long>(ib_sz),
        static_cast<unsigned long long>(marker_vb_sz),
        static_cast<unsigned long long>(marker_ib_sz),
        static_cast<unsigned long long>(cb_sz),
        static_cast<unsigned long long>(shadow_cb_sz)
    );

    LAUNCHER3D_TRACE("hook_draw: step create vb upload");
    s.gpu.vb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(vb_sz, plume::RenderBufferFlag::VERTEX));
    LAUNCHER3D_TRACE("hook_draw: step create ib upload");
    s.gpu.ib = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(ib_sz, plume::RenderBufferFlag::INDEX));
    LAUNCHER3D_TRACE("hook_draw: step create marker vb/ib upload");
    s.gpu.marker_vb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(std::max<uint64_t>(marker_vb_sz, sizeof(Vertex)), plume::RenderBufferFlag::VERTEX));
    s.gpu.marker_ib = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(std::max<uint64_t>(marker_ib_sz, sizeof(uint32_t)), plume::RenderBufferFlag::INDEX));
    LAUNCHER3D_TRACE("hook_draw: step create cb upload");
    s.gpu.cb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(cb_sz, plume::RenderBufferFlag::CONSTANT));
    s.gpu.cb_size = cb_sz;
    LAUNCHER3D_TRACE("hook_draw: step create marker cb upload");
    s.gpu.marker_cb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(cb_sz, plume::RenderBufferFlag::CONSTANT));
    s.gpu.marker_cb_size = cb_sz;
    LAUNCHER3D_TRACE("hook_draw: step create shadow cb upload");
    s.gpu.shadow_cb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(shadow_cb_sz, plume::RenderBufferFlag::CONSTANT));
    s.gpu.shadow_cb_size = shadow_cb_sz;
    LAUNCHER3D_TRACE("hook_draw: step create descriptor set");
    s.gpu.set = s.gpu.set_builder->create(d);
    LAUNCHER3D_TRACE("hook_draw: step create marker descriptor set");
    s.gpu.marker_set = s.gpu.set_builder->create(d);
    LAUNCHER3D_TRACE("hook_draw: step create shadow descriptor set");
    s.gpu.shadow_set = s.gpu.shadow_set_builder->create(d);

    if (!(s.gpu.vb && s.gpu.ib && s.gpu.cb && s.gpu.marker_cb && s.gpu.shadow_cb && s.gpu.set && s.gpu.marker_set && s.gpu.shadow_set)) {
        LAUNCHER3D_TRACE(
            "hook_draw: resource alloc failed vb=%s ib=%s cb=%s marker_cb=%s shadow_cb=%s set=%s marker_set=%s shadow_set=%s marker_vb=%s marker_ib=%s",
            yes_no(s.gpu.vb != nullptr),
            yes_no(s.gpu.ib != nullptr),
            yes_no(s.gpu.cb != nullptr),
            yes_no(s.gpu.marker_cb != nullptr),
            yes_no(s.gpu.shadow_cb != nullptr),
            yes_no(s.gpu.set != nullptr),
            yes_no(s.gpu.marker_set != nullptr),
            yes_no(s.gpu.shadow_set != nullptr),
            yes_no(s.gpu.marker_vb != nullptr),
            yes_no(s.gpu.marker_ib != nullptr)
        );
        return false;
    }

    LAUNCHER3D_TRACE("hook_draw: step map/fill vb");
    void* vb_ptr = s.gpu.vb->map();
    if (!vb_ptr) {
        LAUNCHER3D_TRACE("hook_draw: vb map failed");
        clear_gpu(s);
        s.pipeline_diag = "VB map failed";
        return false;
    }
    std::memcpy(vb_ptr, s.cpu.vertices.data(), static_cast<size_t>(vb_sz));
    s.gpu.vb->unmap();

    LAUNCHER3D_TRACE("hook_draw: step map/fill ib");
    void* ib_ptr = s.gpu.ib->map();
    if (!ib_ptr) {
        LAUNCHER3D_TRACE("hook_draw: ib map failed");
        clear_gpu(s);
        s.pipeline_diag = "IB map failed";
        return false;
    }
    std::memcpy(ib_ptr, s.cpu.indices.data(), static_cast<size_t>(ib_sz));
    s.gpu.ib->unmap();

    if (!marker_vertices.empty() && !marker_indices.empty() && s.gpu.marker_vb && s.gpu.marker_ib) {
        LAUNCHER3D_TRACE("hook_draw: step map/fill marker vb/ib");
        if (void* marker_vb_ptr = s.gpu.marker_vb->map()) {
            std::memcpy(marker_vb_ptr, marker_vertices.data(), static_cast<size_t>(marker_vb_sz));
            s.gpu.marker_vb->unmap();
            if (void* marker_ib_ptr = s.gpu.marker_ib->map()) {
                std::memcpy(marker_ib_ptr, marker_indices.data(), static_cast<size_t>(marker_ib_sz));
                s.gpu.marker_ib->unmap();
                s.gpu.marker_index_count = marker_indices.size();
                s.gpu.marker_vb_size = marker_vb_sz;
                s.gpu.marker_ib_size = marker_ib_sz;
            } else {
                s.gpu.marker_index_count = 0;
                s.gpu.marker_vb_size = 0;
                s.gpu.marker_ib_size = 0;
                LAUNCHER3D_TRACE("hook_draw: marker ib map failed, disabling light marker");
            }
        } else {
            s.gpu.marker_index_count = 0;
            s.gpu.marker_vb_size = 0;
            s.gpu.marker_ib_size = 0;
            LAUNCHER3D_TRACE("hook_draw: marker vb map failed, disabling light marker");
        }
    } else {
        s.gpu.marker_index_count = 0;
        s.gpu.marker_vb_size = 0;
        s.gpu.marker_ib_size = 0;
    }

    LAUNCHER3D_TRACE("hook_draw: step upload albedo");
    s.gpu.tex_albedo = tex_file(d, s.gpu.copy_q.get(), s.gpu.copy_l.get(), s.gpu.copy_f.get(), s.gpu.upload, s.cpu.albedo, { 255, 255, 255, 255 });
    LAUNCHER3D_TRACE("hook_draw: step upload normal");
    s.gpu.tex_normal = tex_file(d, s.gpu.copy_q.get(), s.gpu.copy_l.get(), s.gpu.copy_f.get(), s.gpu.upload, s.cpu.normal, { 128, 128, 255, 255 });
    LAUNCHER3D_TRACE("hook_draw: step upload spec");
    s.gpu.tex_spec = tex_file(d, s.gpu.copy_q.get(), s.gpu.copy_l.get(), s.gpu.copy_f.get(), s.gpu.upload, s.cpu.spec, { 255, 255, 255, 255 });

    if (!(s.gpu.tex_albedo && s.gpu.tex_normal && s.gpu.tex_spec)) {
        LAUNCHER3D_TRACE(
            "hook_draw: texture setup failed albedo=%s normal=%s spec=%s",
            yes_no(s.gpu.tex_albedo != nullptr),
            yes_no(s.gpu.tex_normal != nullptr),
            yes_no(s.gpu.tex_spec != nullptr)
        );
        return false;
    }
    if (!ensure_shadow_fallback_texture(s)) {
        LAUNCHER3D_TRACE("hook_draw: shadow fallback texture creation failed");
        return false;
    }

    LAUNCHER3D_TRACE("hook_draw: step bind descriptor resources");
    s.gpu.set->setBuffer(s.gpu.cb_idx, s.gpu.cb.get(), s.gpu.cb_size);
    s.gpu.set->setTexture(s.gpu.a_idx, s.gpu.tex_albedo.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.n_idx, s.gpu.tex_normal.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.s_idx, s.gpu.tex_spec.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.shadow_idx, s.gpu.tex_shadow_fallback.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.marker_set->setBuffer(s.gpu.cb_idx, s.gpu.marker_cb.get(), s.gpu.marker_cb_size);
    s.gpu.marker_set->setTexture(s.gpu.a_idx, s.gpu.tex_albedo.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.marker_set->setTexture(s.gpu.n_idx, s.gpu.tex_normal.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.marker_set->setTexture(s.gpu.s_idx, s.gpu.tex_spec.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.marker_set->setTexture(s.gpu.shadow_idx, s.gpu.tex_shadow_fallback.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.shadow_set->setBuffer(s.gpu.shadow_cb_idx, s.gpu.shadow_cb.get(), s.gpu.shadow_cb_size);
    s.gpu.index_count = s.cpu.indices.size();
    s.gpu.ready = true;
    LAUNCHER3D_TRACE("hook_draw: GPU resources ready index_count=%zu marker_index_count=%zu", s.gpu.index_count, s.gpu.marker_index_count);
    return true;
}

Transform compute_current_transform(State& s) {
    if (!s.intro_started) {
        s.intro_started = true;
        s.intro_finished = false;
        s.intro_t0 = std::chrono::steady_clock::now();
    }

    Transform tr = s.cfg.target_transform;
    if (s.intro_finished && s.cfg.intro.play_once) {
        return tr;
    }

    Transform start_transform = tr;
    const V3 spawn_pos = intro_spawn_center_depth(tr);
    const auto spawn_scale = [](float target_scale) {
        const float magnitude = std::max(std::abs(target_scale) * kSpawnScaleMul, kMinRenderableScale);
        return (target_scale < 0.0f) ? -magnitude : magnitude;
    };
    start_transform.position = { spawn_pos.x, spawn_pos.y, spawn_pos.z };
    start_transform.scale = {
        spawn_scale(tr.scale.x),
        spawn_scale(tr.scale.y),
        spawn_scale(tr.scale.z)
    };

    const float duration_sec = std::max(0.05f, s.cfg.intro.duration_sec);
    const float elapsed_sec = std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - s.intro_t0).count();
    const float t = clamp01(elapsed_sec / duration_sec);
    const float eased = ease_intro(
        t,
        std::clamp(s.cfg.intro.overshoot, 0.0f, 1.0f),
        std::max(0.0f, s.cfg.intro.damping)
    );

    tr.position.x = lerp(start_transform.position.x, tr.position.x, eased);
    tr.position.y = lerp(start_transform.position.y, tr.position.y, eased);
    tr.position.z = lerp(start_transform.position.z, tr.position.z, eased);
    tr.scale.x = lerp(start_transform.scale.x, tr.scale.x, eased);
    tr.scale.y = lerp(start_transform.scale.y, tr.scale.y, eased);
    tr.scale.z = lerp(start_transform.scale.z, tr.scale.z, eased);

    if (t >= 1.0f && s.cfg.intro.play_once) {
        s.intro_finished = true;
    }

    return tr;
}

bool record_model_draw(State& s, plume::RenderCommandList* list, plume::RenderFramebuffer* fb, const Transform& tr) {
    plume::RenderFramebuffer* pass_fb = resolve_framebuffer_for_3d_pass(s, fb, list);
    if (pass_fb == nullptr) {
        LAUNCHER3D_TRACE("hook_draw: pass framebuffer is null, skipping");
        return false;
    }

    list->setFramebuffer(pass_fb);
    if (pass_fb != fb && s.gpu.pipeline_uses_depth) {
        list->clearDepth(true, 1.0f);
    }

    const uint32_t w = pass_fb->getWidth();
    const uint32_t h = pass_fb->getHeight();
    if (!w || !h) {
        LAUNCHER3D_TRACE("hook_draw: framebuffer size invalid (%u x %u)", w, h);
        return false;
    }

    Constants c{};
    c.model = TRS(tr);

    V3 camera_pos{ 0.0f, 0.0f, kCameraZ };
    V3 camera_target{ 0.0f, 0.0f, 0.0f };
    float fov_deg = 55.0f;
#if !defined(NDEBUG)
    if (s.panel.free_camera_enabled) {
        camera_target = { s.panel.camera_target_x, s.panel.camera_target_y, s.panel.camera_target_z };
        camera_pos = camera_from_orbit(
            s.panel.camera_yaw_deg,
            std::clamp(s.panel.camera_pitch_deg, -89.0f, 89.0f),
            std::max(0.2f, s.panel.camera_distance),
            camera_target
        );
        fov_deg = std::clamp(s.panel.camera_fov_deg, 20.0f, 110.0f);
    }
#endif

    c.view_proj = Mul(
        LookAt(camera_pos, camera_target, { 0, 1, 0 }),
        P(fov_deg * (std::numbers::pi_v<float> / 180.0f), static_cast<float>(w) / static_cast<float>(h), 0.01f, 100.0f)
    );
    const V4 center_ws = mul_row_vec({ 0.0f, 0.0f, 0.0f, 1.0f }, c.model);
    s.last_clip_center = mul_row_vec(center_ws, c.view_proj);
    c.light_pos_range = { s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z, s.cfg.light.range };
    c.light_dir_intensity = { s.cfg.light.direction_ws.x, s.cfg.light.direction_ws.y, s.cfg.light.direction_ws.z, s.cfg.light.intensity };
    c.light_color_ambient = { s.cfg.light.color.x, s.cfg.light.color.y, s.cfg.light.color.z, s.cfg.light.ambient_intensity };
    c.camera_spec = { camera_pos.x, camera_pos.y, camera_pos.z, s.cpu.spec_factor };
    c.base_color = s.cpu.base_color;
    c.spec_color = s.cpu.spec_color;
    c.shadow_params0 = { 0.0f, 0.0f, 0.0f, 0.0f };
    c.shadow_params1 = { 0.0f, 0.0f, 1.0f, 0.0f };
    set_identity_shadow_matrices(c);

    const bool shadow_enabled = shadows_requested(s) && s.shadow.resources_ready && (s.gpu.tex_shadow != nullptr);
    plume::RenderTexture* shadow_tex = shadow_enabled ? s.gpu.tex_shadow.get() : s.gpu.tex_shadow_fallback.get();
    if (shadow_tex == nullptr) {
        s.pipeline_diag = "Shadow texture missing";
        return false;
    }
    const float inv_shadow_res = (s.shadow.effective_resolution > 0U) ? (1.0f / static_cast<float>(s.shadow.effective_resolution)) : 0.0f;
    if (shadow_enabled) {
        const ShadowPlanes planes = compute_shadow_planes(s, tr);
        const float mode = (s.shadow.active_mode == ActiveShadowMode::PointAtlas) ? 1.0f : 2.0f;
        c.shadow_params0 = {
            std::clamp(s.cfg.shadow.strength, 0.0f, 1.0f),
            std::max(0.0f, s.cfg.shadow.depth_bias),
            std::max(0.0f, s.cfg.shadow.normal_bias),
            std::max(0.1f, s.cfg.shadow.softness)
        };
        c.shadow_params1 = { mode, planes.near_plane, planes.far_plane, inv_shadow_res };
        if (s.shadow.active_mode == ActiveShadowMode::Spot) {
            c.shadow_view_proj[0] = compute_spot_shadow_view_proj(s, tr, planes);
        } else if (s.shadow.active_mode == ActiveShadowMode::PointAtlas) {
            for (uint32_t face_index = 0; face_index < kShadowFaceCount; ++face_index) {
                c.shadow_view_proj[face_index] = compute_point_shadow_view_proj(s, face_index, planes);
            }
        }
    }

    void* cb_ptr = s.gpu.cb->map();
    if (cb_ptr == nullptr) {
        LAUNCHER3D_TRACE("hook_draw: cb map failed");
        s.pipeline_diag = "CB map failed";
        clear_gpu(s);
        return false;
    }
    std::memcpy(cb_ptr, &c, sizeof(c));
    s.gpu.cb->unmap();

    std::array<plume::RenderTextureBarrier, 4> barriers = {{
        { s.gpu.tex_albedo.get(), plume::RenderTextureLayout::SHADER_READ },
        { s.gpu.tex_normal.get(), plume::RenderTextureLayout::SHADER_READ },
        { s.gpu.tex_spec.get(), plume::RenderTextureLayout::SHADER_READ },
        { shadow_tex, plume::RenderTextureLayout::SHADER_READ }
    }};
    list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers.data(), static_cast<uint32_t>(barriers.size()));

    list->setGraphicsPipelineLayout(s.gpu.layout.get());
    list->setPipeline(s.gpu.pipeline.get());
    s.gpu.set->setTexture(s.gpu.a_idx, s.gpu.tex_albedo.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.n_idx, s.gpu.tex_normal.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.s_idx, s.gpu.tex_spec.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.shadow_idx, shadow_tex, plume::RenderTextureLayout::SHADER_READ);
    list->setGraphicsDescriptorSet(s.gpu.set.get(), 0);
    list->setViewports(plume::RenderViewport{ 0, 0, static_cast<float>(w), static_cast<float>(h) });
    list->setScissors(plume::RenderRect{ 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h) });

    plume::RenderVertexBufferView vbv(s.gpu.vb.get(), static_cast<uint64_t>(s.cpu.vertices.size()) * sizeof(Vertex));
    plume::RenderIndexBufferView ibv(s.gpu.ib.get(), static_cast<uint64_t>(s.cpu.indices.size()) * sizeof(uint32_t), plume::RenderFormat::R32_UINT);
    list->setVertexBuffers(0, &vbv, 1, &s.gpu.slot);
    list->setIndexBuffer(&ibv);
    list->drawIndexedInstanced(static_cast<uint32_t>(s.gpu.index_count), 1, 0, 0, 0);

#if !defined(NDEBUG)
    if (s.panel.show_light_marker && (s.gpu.marker_index_count > 0) && s.gpu.marker_vb && s.gpu.marker_ib && s.gpu.marker_cb && s.gpu.marker_set) {
        Transform marker_tr{};
        marker_tr.position = { s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z };
        const float gizmo_scale = std::clamp(s.panel.light_marker_scale, 0.05f, 8.0f);
        marker_tr.scale = { gizmo_scale, gizmo_scale, gizmo_scale };

        Constants marker_c = c;
        marker_c.model = TRS(marker_tr);
        marker_c.base_color = { 1.0f, 0.92f, 0.18f, 1.0f };
        marker_c.spec_color = { 1.0f, 0.95f, 0.4f, 1.0f };
        marker_c.camera_spec.w = 0.28f;
        marker_c.shadow_params0 = { 0.0f, 0.0f, 0.0f, 0.0f };
        marker_c.shadow_params1 = { 0.0f, 0.0f, 1.0f, 0.0f };
        set_identity_shadow_matrices(marker_c);

        if (void* marker_cb_ptr = s.gpu.marker_cb->map()) {
                std::memcpy(marker_cb_ptr, &marker_c, sizeof(marker_c));
                s.gpu.marker_cb->unmap();

                plume::RenderVertexBufferView marker_vbv(
                    s.gpu.marker_vb.get(),
                    s.gpu.marker_vb_size
                );
                plume::RenderIndexBufferView marker_ibv(
                    s.gpu.marker_ib.get(),
                    s.gpu.marker_ib_size,
                    plume::RenderFormat::R32_UINT
                );
                s.gpu.marker_set->setTexture(s.gpu.shadow_idx, shadow_tex, plume::RenderTextureLayout::SHADER_READ);
                list->setGraphicsDescriptorSet(s.gpu.marker_set.get(), 0);
                list->setVertexBuffers(0, &marker_vbv, 1, &s.gpu.slot);
                list->setIndexBuffer(&marker_ibv);
                list->drawIndexedInstanced(static_cast<uint32_t>(s.gpu.marker_index_count), 1, 0, 0, 0);
            }
        }
#endif
    return true;
}

void hook_init(plume::RenderInterface* rhi, plume::RenderDevice* dev);
void hook_draw(plume::RenderCommandList* list, plume::RenderFramebuffer* fb);
void hook_deinit();

void hook_init(plume::RenderInterface* rhi, plume::RenderDevice* dev){
    State& s=st();
    RT64::RenderHookInit* prev=nullptr;
    uint64_t call_no = 0;
    {
        std::lock_guard l(s.mx);
        prev=s.prev_init;
        s.trace_init_calls++;
        call_no = s.trace_init_calls;
    }

    LAUNCHER3D_TRACE(
        "hook_init[%llu]: enter rhi=%p dev=%p prev_exists=%s global_is_ours(init/draw/deinit)=%s/%s/%s",
        static_cast<unsigned long long>(call_no),
        ptr_addr(rhi),
        ptr_addr(dev),
        yes_no(prev != nullptr),
        yes_no(RT64::GetRenderHookInit() == hook_init),
        yes_no(RT64::GetRenderHookDraw() == hook_draw),
        yes_no(RT64::GetRenderHookDeinit() == hook_deinit)
    );

    if(prev) prev(rhi,dev);

    std::lock_guard l(s.mx);
    s.gpu.rhi=rhi;
    s.gpu.dev=dev;
    clear_pipeline(s);
    s.intro_started=false;
    s.intro_finished=false;
    s.trace_first_draw_logged = false;
    s.trace_last_heartbeat = {};
    LAUNCHER3D_TRACE("hook_init[%llu]: state reset complete", static_cast<unsigned long long>(call_no));
}
void hook_draw(plume::RenderCommandList* list, plume::RenderFramebuffer* fb){
    State& s=st();
    RT64::RenderHookDraw* prev=nullptr;
    {
        std::lock_guard l(s.mx);
        s.trace_draw_calls++;
        prev=s.prev_draw;
        if(!s.trace_first_draw_logged){
            s.trace_first_draw_logged = true;
            LAUNCHER3D_TRACE(
                "hook_draw[first]: list=%p fb=%p fbSize=%ux%u prev_exists=%s global_is_ours(init/draw/deinit)=%s/%s/%s",
                ptr_addr(list),
                ptr_addr(fb),
                (fb != nullptr) ? fb->getWidth() : 0,
                (fb != nullptr) ? fb->getHeight() : 0,
                yes_no(prev != nullptr),
                yes_no(RT64::GetRenderHookInit() == hook_init),
                yes_no(RT64::GetRenderHookDraw() == hook_draw),
                yes_no(RT64::GetRenderHookDeinit() == hook_deinit)
            );
        }
    }

    // Draw existing launcher UI first (including 2D background), then overlay the 3D model.
    if(prev) prev(list,fb);

    {
        std::lock_guard l(s.mx);
        const DrawDecision decision = draw_decision(s);
        if(s.trace_last_reason != decision.reason || s.trace_last_should_draw != decision.draw){
            s.trace_last_reason = decision.reason;
            s.trace_last_should_draw = decision.draw;
            LAUNCHER3D_TRACE(
                "hook_draw: gate changed draw=%s reason=%s enabled=%s configured=%s game_started=%s cfg_ctx=%s",
                yes_no(decision.draw),
                decision.reason,
                yes_no(s.enabled),
                yes_no(s.configured),
                yes_no(ultramodern::is_game_started()),
                yes_no(cfg_visible())
            );
        }

        const auto now = std::chrono::steady_clock::now();
        if(s.trace_last_heartbeat == std::chrono::steady_clock::time_point{} || (now - s.trace_last_heartbeat) >= std::chrono::seconds(1)){
            s.trace_last_heartbeat = now;
            LAUNCHER3D_TRACE(
                "heartbeat: installs=%llu init=%llu draw=%llu deinit=%llu should_draw=%s reason=%s rhi=%p dev=%p pipe=%s ready=%s idx=%zu diag=%s skipped=%llu global_is_ours(init/draw/deinit)=%s/%s/%s",
                static_cast<unsigned long long>(s.trace_install_calls),
                static_cast<unsigned long long>(s.trace_init_calls),
                static_cast<unsigned long long>(s.trace_draw_calls),
                static_cast<unsigned long long>(s.trace_deinit_calls),
                yes_no(decision.draw),
                decision.reason,
                ptr_addr(s.gpu.rhi),
                ptr_addr(s.gpu.dev),
                yes_no(s.gpu.pipeline_ok),
                yes_no(s.gpu.ready),
                s.gpu.index_count,
                s.pipeline_diag.empty() ? "OK" : s.pipeline_diag.c_str(),
                static_cast<unsigned long long>(s.trace_frames_skipped),
                yes_no(RT64::GetRenderHookInit() == hook_init),
                yes_no(RT64::GetRenderHookDraw() == hook_draw),
                yes_no(RT64::GetRenderHookDeinit() == hook_deinit)
            );
        }

        if (decision.draw && ensure_pipeline(s)) {
            if (!create_gpu_resources_if_needed(s)) {
                s.last_draw_attempted = false;
                s.trace_frames_skipped++;
                return;
            }

            s.last_draw_attempted = true;
            const Transform tr = compute_current_transform(s);
            if (!update_shadows_if_needed(s, list, tr)) {
                s.shadow.active_mode = ActiveShadowMode::Disabled;
                s.shadow.resources_ready = false;
                mark_shadow_dirty(s, "Shadow runtime fallback");
            }
            if (!record_model_draw(s, list, fb, tr)) {
                s.last_draw_attempted = false;
                s.trace_frames_skipped++;
            }
        } else {
            s.last_draw_attempted = false;
            s.trace_frames_skipped++;
        }
    }
}
void hook_deinit(){
    State& s=st();
    RT64::RenderHookDeinit* prev=nullptr;
    uint64_t call_no = 0;
    {
        std::lock_guard l(s.mx);
        clear_pipeline(s);
        s.gpu.rhi=nullptr;
        s.gpu.dev=nullptr;
        prev=s.prev_deinit;
        s.trace_deinit_calls++;
        call_no = s.trace_deinit_calls;
    }
    LAUNCHER3D_TRACE("hook_deinit[%llu]: prev_exists=%s", static_cast<unsigned long long>(call_no), yes_no(prev != nullptr));
    if(prev) prev();
}

} // namespace

void install_render_hook_chain(){
    State& s=st();
    std::lock_guard l(s.mx);
    s.trace_install_calls++;

    RT64::RenderHookInit* before_init = RT64::GetRenderHookInit();
    RT64::RenderHookDraw* before_draw = RT64::GetRenderHookDraw();
    RT64::RenderHookDeinit* before_deinit = RT64::GetRenderHookDeinit();

    LAUNCHER3D_TRACE(
        "install_render_hook_chain[%llu]: hooks_installed=%s before_is_ours(init/draw/deinit)=%s/%s/%s before_exists(init/draw/deinit)=%s/%s/%s",
        static_cast<unsigned long long>(s.trace_install_calls),
        yes_no(s.hooks_installed),
        yes_no(before_init == hook_init),
        yes_no(before_draw == hook_draw),
        yes_no(before_deinit == hook_deinit),
        yes_no(before_init != nullptr),
        yes_no(before_draw != nullptr),
        yes_no(before_deinit != nullptr)
    );

    if(s.hooks_installed){
        return;
    }

    s.prev_init=before_init;
    s.prev_draw=before_draw;
    s.prev_deinit=before_deinit;
    RT64::SetRenderHooks(hook_init,hook_draw,hook_deinit);
    s.hooks_installed=true;

    LAUNCHER3D_TRACE(
        "install_render_hook_chain: installed prev_exists(init/draw/deinit)=%s/%s/%s after_is_ours(init/draw/deinit)=%s/%s/%s",
        yes_no(s.prev_init != nullptr),
        yes_no(s.prev_draw != nullptr),
        yes_no(s.prev_deinit != nullptr),
        yes_no(RT64::GetRenderHookInit() == hook_init),
        yes_no(RT64::GetRenderHookDraw() == hook_draw),
        yes_no(RT64::GetRenderHookDeinit() == hook_deinit)
    );
}

void prime_render_backend(void* rhi, void* dev){
    State& s = st();
    std::lock_guard l(s.mx);

    auto* rhi_typed = reinterpret_cast<plume::RenderInterface*>(rhi);
    auto* dev_typed = reinterpret_cast<plume::RenderDevice*>(dev);

    if((rhi_typed == nullptr) || (dev_typed == nullptr)){
        LAUNCHER3D_TRACE("prime_render_backend: ignored null rhi/dev (%p/%p)", ptr_addr(rhi_typed), ptr_addr(dev_typed));
        return;
    }

    const bool changed = (s.gpu.rhi != rhi_typed) || (s.gpu.dev != dev_typed);
    s.gpu.rhi = rhi_typed;
    s.gpu.dev = dev_typed;
    if(changed){
        clear_pipeline(s);
        s.intro_started = false;
        s.intro_finished = false;
        s.trace_first_draw_logged = false;
        s.trace_last_heartbeat = {};
        LAUNCHER3D_TRACE("prime_render_backend: applied rhi=%p dev=%p (pipeline reset)", ptr_addr(s.gpu.rhi), ptr_addr(s.gpu.dev));
    }
    else {
        LAUNCHER3D_TRACE("prime_render_backend: unchanged rhi=%p dev=%p", ptr_addr(s.gpu.rhi), ptr_addr(s.gpu.dev));
    }
}

void reset_intro_locked(State& s) {
    s.intro_started = false;
    s.intro_finished = false;
    s.intro_t0 = {};
}

void reset_state_for_reconfigure(State& s, const Config& cfg) {
    s.cfg = cfg;
    s.cfg_initial = cfg;
    s.enabled = false;
    s.configured = false;
    s.cpu = {};
    reset_intro_locked(s);
    clear_gpu(s);
    s.shadow = {};
    s.shadow.dirty = true;
}

bool validate_config(const Config& cfg) {
    if (cfg.glb_path.empty() || !std::filesystem::exists(cfg.glb_path)) {
        std::fprintf(stderr, "[CellenseresSDK] launcher3d: missing GLB '%s'\n", cfg.glb_path.string().c_str());
        return false;
    }

    return true;
}

Config sanitize_config(const Config& cfg) {
    Config out = cfg;

    const auto sanitize_scale = [](float value) {
        if (std::abs(value) < kMinRenderableScale) {
            return (value < 0.0f) ? -kMinRenderableScale : kMinRenderableScale;
        }
        return value;
    };

    out.target_transform.scale.x = sanitize_scale(out.target_transform.scale.x);
    out.target_transform.scale.y = sanitize_scale(out.target_transform.scale.y);
    out.target_transform.scale.z = sanitize_scale(out.target_transform.scale.z);

    out.light.range = std::max(0.1f, out.light.range);
    out.light.intensity = std::max(0.0f, out.light.intensity);
    out.light.ambient_intensity = std::max(0.0f, out.light.ambient_intensity);

    out.intro.duration_sec = std::max(0.05f, out.intro.duration_sec);
    out.intro.overshoot = std::clamp(out.intro.overshoot, 0.0f, 1.0f);
    out.intro.damping = std::max(0.0f, out.intro.damping);

    out.shadow.resolution = clamp_shadow_resolution(out.shadow.resolution);
    out.shadow.strength = std::clamp(out.shadow.strength, 0.0f, 1.0f);
    out.shadow.depth_bias = std::max(0.0f, out.shadow.depth_bias);
    out.shadow.normal_bias = std::max(0.0f, out.shadow.normal_bias);
    out.shadow.softness = std::max(0.1f, out.shadow.softness);
    out.shadow.near_plane = std::max(out.shadow.near_plane, kShadowNearPlaneMin);
    out.shadow.far_plane_override = std::max(0.0f, out.shadow.far_plane_override);
    return out;
}

#if !defined(NDEBUG)
void log_config_overview(const Config& cfg) {
    std::fprintf(stdout, "[CellenseresSDK] launcher3d: configure path '%s'\n", cfg.glb_path.string().c_str());
    std::fprintf(
        stdout,
        "[CellenseresSDK] launcher3d: configure flags title_only=%s play_once=%s\n",
        yes_no(cfg.visible_only_on_title_screen),
        yes_no(cfg.intro.play_once)
    );
    std::fprintf(
        stdout,
        "[CellenseresSDK] launcher3d: configure target pos=(%.3f, %.3f, %.3f) rot=(%.3f, %.3f, %.3f) scale=(%.3f, %.3f, %.3f)\n",
        cfg.target_transform.position.x,
        cfg.target_transform.position.y,
        cfg.target_transform.position.z,
        cfg.target_transform.rotation_deg.pitch,
        cfg.target_transform.rotation_deg.yaw,
        cfg.target_transform.rotation_deg.roll,
        cfg.target_transform.scale.x,
        cfg.target_transform.scale.y,
        cfg.target_transform.scale.z
    );
    std::fprintf(
        stdout,
        "[CellenseresSDK] launcher3d: configure shadow enabled=%s mode=%d res=%u strength=%.3f bias=%.5f nBias=%.4f soft=%.3f near=%.3f farOverride=%.3f\n",
        yes_no(cfg.shadow.enabled),
        static_cast<int>(cfg.shadow.mode),
        cfg.shadow.resolution,
        cfg.shadow.strength,
        cfg.shadow.depth_bias,
        cfg.shadow.normal_bias,
        cfg.shadow.softness,
        cfg.shadow.near_plane,
        cfg.shadow.far_plane_override
    );
    std::fflush(stdout);
}
#endif

bool configure(const Config& cfg) {
    const Config sanitized_cfg = sanitize_config(cfg);
#if !defined(NDEBUG)
    log_config_overview(sanitized_cfg);
#endif

    if (!validate_config(sanitized_cfg)) {
        return false;
    }

    CpuModel loaded_cpu{};
    std::string err;
    if (!load_glb(sanitized_cfg.glb_path, loaded_cpu, err)) {
        std::fprintf(stderr, "[CellenseresSDK] launcher3d: load failed: %s\n", err.c_str());
        return false;
    }

#if !defined(NDEBUG)
    log_loaded_model_info(sanitized_cfg, loaded_cpu);
#endif

    State& s = st();
    std::lock_guard l(s.mx);
    reset_state_for_reconfigure(s, sanitized_cfg);
    s.cpu = std::move(loaded_cpu);
    s.enabled = true;
    s.configured = true;
    return true;
}

void set_enabled(bool enabled) {
    State& s = st();
    std::lock_guard l(s.mx);
    s.enabled = enabled && s.configured;
}

void on_launcher_menu_update(recompui::Element* menu_container) {
    State& s = st();
    std::lock_guard l(s.mx);
#if !defined(NDEBUG)
    ensure_panel(s, menu_container);
    if (s.panel.status) {
        std::ostringstream ss;
        ss << "Draw " << (should_draw(s) ? "Yes" : "No")
           << " | Pipe " << (s.gpu.pipeline_ok ? "Yes" : "No")
           << " | Ready " << (s.gpu.ready ? "Yes" : "No")
           << " | Shadow " << active_shadow_mode_name(s.shadow.active_mode)
           << "@" << s.shadow.effective_resolution
           << " | Cam " << (s.panel.free_camera_enabled ? "Free" : "Fixed")
           << " | Gizmo " << (s.panel.show_light_marker ? "On" : "Off")
           << " | Intro " << ((s.intro_finished && s.cfg.intro.play_once) ? "Done" : "Active");

        s.panel.status->set_text(ss.str());
    }
#else
    (void)menu_container;
#endif
}

void reset_intro() {
    State& s = st();
    std::lock_guard l(s.mx);
    reset_intro_locked(s);
    mark_shadow_dirty(s);
}

void shutdown() {
    State& s = st();
    std::lock_guard l(s.mx);
    clear_pipeline(s);
    s.enabled = false;
    s.configured = false;
}

Config get_current_tuning_snapshot() {
    State& s = st();
    std::lock_guard l(s.mx);
    return s.cfg;
}

std::string make_cpp_initializer_snippet() {
    State& s = st();
    std::lock_guard l(s.mx);
    const Config& c = s.cfg;

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "csdk::launcher3d::Config launcher3d_cfg{\n";
    o << "    .glb_path = recompui::file::get_asset_path(\"" << c.glb_path.filename().string() << "\"),\n";
    o << "    .target_transform = {\n";
    o << "        .position = { " << c.target_transform.position.x << "f, " << c.target_transform.position.y << "f, " << c.target_transform.position.z << "f },\n";
    o << "        .rotation_deg = { " << c.target_transform.rotation_deg.pitch << "f, " << c.target_transform.rotation_deg.yaw << "f, " << c.target_transform.rotation_deg.roll << "f },\n";
    o << "        .scale = { " << c.target_transform.scale.x << "f, " << c.target_transform.scale.y << "f, " << c.target_transform.scale.z << "f },\n";
    o << "    },\n";
    o << "    .light = {\n";
    o << "        .direction_ws = { " << c.light.direction_ws.x << "f, " << c.light.direction_ws.y << "f, " << c.light.direction_ws.z << "f },\n";
    o << "        .position_ws = { " << c.light.position_ws.x << "f, " << c.light.position_ws.y << "f, " << c.light.position_ws.z << "f },\n";
    o << "        .range = " << c.light.range << "f,\n";
    o << "        .color = { " << c.light.color.x << "f, " << c.light.color.y << "f, " << c.light.color.z << "f },\n";
    o << "        .intensity = " << c.light.intensity << "f,\n";
    o << "        .ambient_intensity = " << c.light.ambient_intensity << "f,\n";
    o << "    },\n";
    o << "    .intro = { .duration_sec = " << c.intro.duration_sec << "f, .overshoot = " << c.intro.overshoot << "f, .damping = " << c.intro.damping << "f, .play_once = " << (c.intro.play_once?"true":"false") << " },\n";
    o << "    .shadow = {\n";
    o << "        .enabled = " << (c.shadow.enabled ? "true" : "false") << ",\n";
    o << "        .mode = csdk::launcher3d::ShadowMode::";
    if (c.shadow.mode == ShadowMode::SpotOnly) {
        o << "SpotOnly";
    } else if (c.shadow.mode == ShadowMode::Disabled) {
        o << "Disabled";
    } else {
        o << "PointCubePreferred";
    }
    o << ",\n";
    o << "        .resolution = " << c.shadow.resolution << ",\n";
    o << "        .strength = " << c.shadow.strength << "f,\n";
    o << "        .depth_bias = " << c.shadow.depth_bias << "f,\n";
    o << "        .normal_bias = " << c.shadow.normal_bias << "f,\n";
    o << "        .softness = " << c.shadow.softness << "f,\n";
    o << "        .near_plane = " << c.shadow.near_plane << "f,\n";
    o << "        .far_plane_override = " << c.shadow.far_plane_override << "f,\n";
    o << "    },\n";
    o << "    .visible_only_on_title_screen = " << (c.visible_only_on_title_screen?"true":"false") << ",\n";
    o << "};\n";
    return o.str();
}

} // namespace csdk::launcher3d
