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
#ifdef _WIN32
#include "LauncherModelVS.hlsl.dxil.h"
#include "LauncherModelPS.hlsl.dxil.h"
#elif defined(__APPLE__)
#include "LauncherModelVS.hlsl.metal.h"
#include "LauncherModelPS.hlsl.metal.h"
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

// Keep launcher_model3d.cpp decoupled from SDL headers for CI portability.
extern "C" int SDL_SetClipboardText(const char* text);

namespace csdk::launcher3d {
namespace {

constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr float kCameraZ = 2.7f;
constexpr float kSpawnDepthDistance = 6.0f;
constexpr float kSpawnScaleMul = 0.08f;
constexpr float kMinRenderableScale = 0.001f;

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
};

struct Gpu {
    plume::RenderInterface* rhi = nullptr;
    plume::RenderDevice* dev = nullptr;
    std::unique_ptr<plume::RenderSampler> sampler;
    std::unique_ptr<plume::RenderShader> vs;
    std::unique_ptr<plume::RenderShader> ps;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderPipeline> pipeline;
    std::unique_ptr<plume::RenderDescriptorSetBuilder> set_builder;
    std::unique_ptr<plume::RenderDescriptorSet> set;
    std::unique_ptr<plume::RenderBuffer> vb;
    std::unique_ptr<plume::RenderBuffer> ib;
    std::unique_ptr<plume::RenderBuffer> cb;
    std::unique_ptr<plume::RenderTexture> tex_albedo;
    std::unique_ptr<plume::RenderTexture> tex_normal;
    std::unique_ptr<plume::RenderTexture> tex_spec;
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
    uint64_t cb_size = 0;
    size_t index_count = 0;
    plume::RenderFormat pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    bool pipeline_uses_depth = false;
    bool pipeline_ok = false;
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

M4 I() {
    M4 o{};
    o.m[0] = 1.0f;
    o.m[5] = 1.0f;
    o.m[10] = 1.0f;
    o.m[15] = 1.0f;
    return o;
}

M4 Mul(const M4& a, const M4& b) {
    M4 o{};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            o.m[r * 4 + c] =
                a.m[r * 4 + 0] * b.m[0 * 4 + c] +
                a.m[r * 4 + 1] * b.m[1 * 4 + c] +
                a.m[r * 4 + 2] * b.m[2 * 4 + c] +
                a.m[r * 4 + 3] * b.m[3 * 4 + c];
        }
    }
    return o;
}

M4 T(float x, float y, float z) {
    M4 o = I();
    o.m[12] = x;
    o.m[13] = y;
    o.m[14] = z;
    return o;
}

M4 S(float x, float y, float z) {
    M4 o = I();
    o.m[0] = x;
    o.m[5] = y;
    o.m[10] = z;
    return o;
}

M4 Rx(float a) {
    M4 o = I();
    const float c = std::cos(a);
    const float s = std::sin(a);
    o.m[5] = c;
    o.m[6] = s;
    o.m[9] = -s;
    o.m[10] = c;
    return o;
}

M4 Ry(float a) {
    M4 o = I();
    const float c = std::cos(a);
    const float s = std::sin(a);
    o.m[0] = c;
    o.m[2] = -s;
    o.m[8] = s;
    o.m[10] = c;
    return o;
}

M4 Rz(float a) {
    M4 o = I();
    const float c = std::cos(a);
    const float s = std::sin(a);
    o.m[0] = c;
    o.m[1] = s;
    o.m[4] = -s;
    o.m[5] = c;
    return o;
}

M4 TRS(const Transform& t) {
    const float d2r = std::numbers::pi_v<float> / 180.0f;
    const M4 model_translate = T(t.position.x, t.position.y, t.position.z);
    const M4 model_rotate_z = Rz(t.rotation_deg.roll * d2r);
    const M4 model_rotate_y = Ry(t.rotation_deg.yaw * d2r);
    const M4 model_rotate_x = Rx(t.rotation_deg.pitch * d2r);
    const M4 model_scale = S(t.scale.x, t.scale.y, t.scale.z);
    return Mul(Mul(Mul(Mul(model_translate, model_rotate_z), model_rotate_y), model_rotate_x), model_scale);
}

M4 P(float fovy, float asp, float n, float f) {
    // Right-handed projection for row-vector math (world * view * proj).
    M4 o{};
    const float q = 1.0f / std::tan(fovy * 0.5f);
    o.m[0] = q / asp;
    o.m[5] = q;
    o.m[10] = f / (n - f);
    o.m[11] = -1.0f;
    o.m[14] = (n * f) / (n - f);
    return o;
}

V3 sub(V3 a, V3 b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

V3 cross(V3 a, V3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

V4 mul_row_vec(V4 v, const M4& m){
    return {
        v.x * m.m[0]  + v.y * m.m[4]  + v.z * m.m[8]  + v.w * m.m[12],
        v.x * m.m[1]  + v.y * m.m[5]  + v.z * m.m[9]  + v.w * m.m[13],
        v.x * m.m[2]  + v.y * m.m[6]  + v.z * m.m[10] + v.w * m.m[14],
        v.x * m.m[3]  + v.y * m.m[7]  + v.z * m.m[11] + v.w * m.m[15]
    };
}

V3 norm(V3 v) {
    const float len = std::sqrt(std::max(dot(v, v), 1e-12f));
    return { v.x / len, v.y / len, v.z / len };
}

V3 intro_spawn_center_depth(const Transform& target) {
    const V3 cam{ 0.0f, 0.0f, kCameraZ };
    const V3 center_target{ 0.0f, 0.0f, target.position.z };
    const V3 away_from_camera = norm(sub(center_target, cam));
    return {
        center_target.x + away_from_camera.x * kSpawnDepthDistance,
        center_target.y + away_from_camera.y * kSpawnDepthDistance,
        center_target.z + away_from_camera.z * kSpawnDepthDistance
    };
}

M4 LookAt(V3 e, V3 t, V3 up){
    // Right-handed camera basis to avoid mirrored X in clip space.
    const V3 z = norm(sub(e, t));
    const V3 x = norm(cross(up, z));
    const V3 y = cross(z, x);

    M4 o = I();
    o.m[0] = x.x;
    o.m[1] = y.x;
    o.m[2] = z.x;
    o.m[4] = x.y;
    o.m[5] = y.y;
    o.m[6] = z.y;
    o.m[8] = x.z;
    o.m[9] = y.z;
    o.m[10] = z.z;
    o.m[12] = -dot(x, e);
    o.m[13] = -dot(y, e);
    o.m[14] = -dot(z, e);
    return o;
}

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }

    return ((value + alignment - 1) / alignment) * alignment;
}

float ease_intro(float t, float overs, float damping) {
    const float s_back = 1.0f + 1.70158f * (0.35f + overs);
    const float u = t - 1.0f;
    float v = 1.0f + s_back * u * u * u + (s_back - 1.0f) * u * u;
    v += std::exp(-damping * t) * std::sin(t * std::numbers::pi_v<float> * 2.0f) * overs * 0.08f;
    return v;
}

bool read_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size <= 0) {
        return false;
    }

    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return f.good();
}

const cgltf_accessor* find_attr(const cgltf_primitive& pr, cgltf_attribute_type ty, int idx = 0) {
    for (cgltf_size i = 0; i < pr.attributes_count; i++) {
        const auto& a = pr.attributes[i];
        if (a.type == ty && a.index == idx) {
            return a.data;
        }
    }

    return nullptr;
}

bool image_bytes(const cgltf_image* img, const std::filesystem::path& dir, std::vector<uint8_t>& out) {
    if (!img) {
        return false;
    }

    if (img->buffer_view) {
        const auto* view = img->buffer_view;
        const uint8_t* p = view->data
            ? reinterpret_cast<const uint8_t*>(view->data)
            : ((view->buffer && view->buffer->data) ? (reinterpret_cast<const uint8_t*>(view->buffer->data) + view->offset) : nullptr);
        if (!p || view->size == 0) {
            return false;
        }

        out.assign(p, p + view->size);
        return true;
    }

    if (!img->uri) {
        return false;
    }

    std::string uri = img->uri;
    if (uri.rfind("data:", 0) == 0) {
        const size_t pos = uri.find("base64,");
        if (pos == std::string::npos) {
            return false;
        }

        const char* b64 = uri.c_str() + pos + 7;
        const size_t n = std::strlen(b64);
        const size_t pad = (n && b64[n - 1] == '=') + (n > 1 && b64[n - 2] == '=');
        const cgltf_size out_n = (n / 4) * 3 - pad;

        void* decoded = nullptr;
        cgltf_options options{};
        if (cgltf_load_buffer_base64(&options, out_n, b64, &decoded) != cgltf_result_success || decoded == nullptr) {
            return false;
        }

        out.assign(reinterpret_cast<uint8_t*>(decoded), reinterpret_cast<uint8_t*>(decoded) + out_n);
        std::free(decoded);
        return true;
    }

    std::vector<char> mutable_uri(uri.begin(), uri.end());
    mutable_uri.push_back('\0');
    cgltf_decode_uri(mutable_uri.data());
    return read_file(dir / mutable_uri.data(), out);
}

bool tex_bytes(const cgltf_texture_view& tv, const std::filesystem::path& dir, std::vector<uint8_t>& out) {
    return tv.texture && tv.texture->image && image_bytes(tv.texture->image, dir, out);
}

bool load_glb(const std::filesystem::path& p,CpuModel& out,std::string& err){
    cgltf_options o{}; cgltf_data* d=nullptr;
    if(cgltf_parse_file(&o,p.string().c_str(),&d)!=cgltf_result_success||!d){ err="parse failed"; return false; }
    auto fin=[&](){ if(d) cgltf_free(d); d=nullptr; };
    if(cgltf_load_buffers(&o,d,p.string().c_str())!=cgltf_result_success){ err="load buffers failed"; fin(); return false; }

    const cgltf_primitive* pr=nullptr;
    const cgltf_node* owner_node=nullptr;
    for(cgltf_size n=0;n<d->nodes_count&&!pr;n++){
        const cgltf_node& node=d->nodes[n];
        if(node.mesh==nullptr) continue;
        for(cgltf_size i=0;i<node.mesh->primitives_count;i++){
            const cgltf_primitive& c=node.mesh->primitives[i];
            if(c.type==cgltf_primitive_type_triangles && find_attr(c,cgltf_attribute_type_position)){
                pr=&c;
                owner_node=&node;
                break;
            }
        }
    }
    if(!pr){
        for(cgltf_size m=0;m<d->meshes_count&&!pr;m++){
            for(cgltf_size i=0;i<d->meshes[m].primitives_count;i++){
                const cgltf_primitive& c=d->meshes[m].primitives[i];
                if(c.type==cgltf_primitive_type_triangles && find_attr(c,cgltf_attribute_type_position)){
                    pr=&c;
                    break;
                }
            }
        }
    }
    if(!pr){ err="no triangle primitive"; fin(); return false; }

    auto* pa = find_attr(*pr, cgltf_attribute_type_position);
    auto* na = find_attr(*pr, cgltf_attribute_type_normal);
    auto* ta = find_attr(*pr, cgltf_attribute_type_tangent);
    auto* ua = find_attr(*pr, cgltf_attribute_type_texcoord, 0);
    if(!pa||pa->count==0){ err="no positions"; fin(); return false; }

    M4 node_transform=I();
    if(owner_node!=nullptr){
        cgltf_float world_col_major[16]{};
        cgltf_node_transform_world(owner_node, world_col_major);
        for(int i=0;i<16;i++){
            node_transform.m[i]=(float)world_col_major[i];
        }
    }

    auto transform_pos = [&](const V3& p3)->V3{
        V4 v = mul_row_vec({p3.x,p3.y,p3.z,1.0f}, node_transform);
        return {v.x,v.y,v.z};
    };
    auto transform_dir = [&](const V3& d3)->V3{
        V4 v = mul_row_vec({d3.x,d3.y,d3.z,0.0f}, node_transform);
        return norm({v.x,v.y,v.z});
    };

    const float m00=node_transform.m[0], m01=node_transform.m[1], m02=node_transform.m[2];
    const float m10=node_transform.m[4], m11=node_transform.m[5], m12=node_transform.m[6];
    const float m20=node_transform.m[8], m21=node_transform.m[9], m22=node_transform.m[10];
    const float det3 =
        m00*(m11*m22 - m12*m21) -
        m01*(m10*m22 - m12*m20) +
        m02*(m10*m21 - m11*m20);
    const bool flip_winding = det3 < 0.0f;
    out.had_owner_node = (owner_node != nullptr);
    out.owner_node_det3 = det3;
    out.flipped_winding = flip_winding;
    out.source_had_normals = (na != nullptr);

    size_t vc=(size_t)pa->count; out.vertices.resize(vc); std::array<float,4> v{};
    for(size_t i=0;i<vc;i++){
        cgltf_accessor_read_float(pa,i,v.data(),3);
        out.vertices[i].p=transform_pos({v[0],v[1],v[2]});
        if(na){
            cgltf_accessor_read_float(na,i,v.data(),3);
            out.vertices[i].n=transform_dir({v[0],v[1],v[2]});
        }
        if(ta){
            cgltf_accessor_read_float(ta,i,v.data(),4);
            V3 t_dir = transform_dir({v[0],v[1],v[2]});
            out.vertices[i].t={t_dir.x,t_dir.y,t_dir.z,v[3]};
        }
        if(ua){ cgltf_accessor_read_float(ua,i,v.data(),2); out.vertices[i].uv={v[0],v[1]}; }
    }
    if(pr->indices && pr->indices->count){
        out.indices.resize((size_t)pr->indices->count);
        for(size_t i=0;i<out.indices.size();i++) out.indices[i]=(uint32_t)cgltf_accessor_read_index(pr->indices,i);
    }
    else {
        out.indices.resize(vc);
        for(size_t i=0;i<vc;i++) out.indices[i]=(uint32_t)i;
    }

    if(flip_winding){
        for(size_t i=0;i+2<out.indices.size();i+=3){
            std::swap(out.indices[i+1], out.indices[i+2]);
        }
    }

    if(!na){
        std::vector<V3> accum(vc, V3{0.0f,0.0f,0.0f});
        for(size_t i=0;i+2<out.indices.size();i+=3){
            uint32_t i0=out.indices[i+0], i1=out.indices[i+1], i2=out.indices[i+2];
            if(i0>=vc || i1>=vc || i2>=vc) continue;
            V3 p0=out.vertices[i0].p, p1=out.vertices[i1].p, p2=out.vertices[i2].p;
            V3 e1=sub(p1,p0), e2=sub(p2,p0);
            V3 fn=cross(e1,e2);
            accum[i0]={accum[i0].x+fn.x,accum[i0].y+fn.y,accum[i0].z+fn.z};
            accum[i1]={accum[i1].x+fn.x,accum[i1].y+fn.y,accum[i1].z+fn.z};
            accum[i2]={accum[i2].x+fn.x,accum[i2].y+fn.y,accum[i2].z+fn.z};
        }
        for(size_t i=0;i<vc;i++){
            const float len2 = dot(accum[i],accum[i]);
            out.vertices[i].n = (len2 > 1e-12f) ? norm(accum[i]) : V3{0.0f,0.0f,1.0f};
        }
        out.generated_smooth_normals = true;
    }

    if(pr->material){
        const auto* m=pr->material;
        if(m->has_pbr_metallic_roughness){
            out.base_color = {
                m->pbr_metallic_roughness.base_color_factor[0],
                m->pbr_metallic_roughness.base_color_factor[1],
                m->pbr_metallic_roughness.base_color_factor[2],
                m->pbr_metallic_roughness.base_color_factor[3]
            };
            tex_bytes(m->pbr_metallic_roughness.base_color_texture, p.parent_path(), out.albedo);
        }
        tex_bytes(m->normal_texture, p.parent_path(), out.normal);
        if(m->has_specular){
            out.spec_factor = m->specular.specular_factor;
            out.spec_color = {
                m->specular.specular_color_factor[0],
                m->specular.specular_color_factor[1],
                m->specular.specular_color_factor[2],
                1.0f
            };
            if(!tex_bytes(m->specular.specular_color_texture, p.parent_path(), out.spec)) {
                tex_bytes(m->specular.specular_texture, p.parent_path(), out.spec);
            }
        }
    }
    fin(); return true;
}

void clear_gpu(State& s){
    s.gpu.set.reset();
    s.gpu.vb.reset();
    s.gpu.ib.reset();
    s.gpu.cb.reset();
    s.gpu.tex_albedo.reset();
    s.gpu.tex_normal.reset();
    s.gpu.tex_spec.reset();
    s.gpu.upload.reset();
    s.gpu.depth_by_src_fb.clear();
    s.gpu.fb_with_depth_by_src_fb.clear();
    s.gpu.pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    s.gpu.pipeline_uses_depth = false;
    s.gpu.ready=false;
    s.gpu.index_count=0;
}
void clear_pipeline(State& s) {
    clear_gpu(s);
    s.gpu.pipeline.reset();
    s.gpu.layout.reset();
    s.gpu.vs.reset();
    s.gpu.ps.reset();
    s.gpu.sampler.reset();
    s.gpu.set_builder.reset();
    s.gpu.copy_l.reset();
    s.gpu.copy_q.reset();
    s.gpu.copy_f.reset();
    s.gpu.pipeline_ok = false;
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
    if(s.gpu.pipeline_ok) return true;
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

    auto create_vs = [&](plume::RenderShaderFormat fmt)->std::unique_ptr<plume::RenderShader>{
        const void* blob = GET_SHADER_BLOB(LauncherModelVS, fmt);
        size_t blob_size = GET_SHADER_SIZE(LauncherModelVS, fmt);
        if(!blob || blob_size == 0) return nullptr;
        return d->createShader(blob, blob_size, "VSMain", fmt);
    };
    auto create_ps = [&](plume::RenderShaderFormat fmt)->std::unique_ptr<plume::RenderShader>{
        const void* blob = GET_SHADER_BLOB(LauncherModelPS, fmt);
        size_t blob_size = GET_SHADER_SIZE(LauncherModelPS, fmt);
        if(!blob || blob_size == 0) return nullptr;
        return d->createShader(blob, blob_size, "PSMain", fmt);
    };

    auto try_shader_pair = [&](plume::RenderShaderFormat fmt)->bool{
        LAUNCHER3D_TRACE("ensure_pipeline: try shaders format=%s", shader_format_name(fmt));
        auto vs_try = create_vs(fmt);
        auto ps_try = create_ps(fmt);
        if(vs_try && ps_try){
            s.gpu.vs = std::move(vs_try);
            s.gpu.ps = std::move(ps_try);
            LAUNCHER3D_TRACE("ensure_pipeline: shader pair ok format=%s", shader_format_name(fmt));
            return true;
        }
        LAUNCHER3D_TRACE("ensure_pipeline: shader pair failed format=%s", shader_format_name(fmt));
        return false;
    };

    bool shader_ok = false;
    shader_ok = try_shader_pair(sf);
#ifdef _WIN32
    if(!shader_ok && sf != plume::RenderShaderFormat::DXIL) shader_ok = try_shader_pair(plume::RenderShaderFormat::DXIL);
    if(!shader_ok && sf != plume::RenderShaderFormat::SPIRV) shader_ok = try_shader_pair(plume::RenderShaderFormat::SPIRV);
#elif defined(__APPLE__)
    if(!shader_ok && sf != plume::RenderShaderFormat::METAL) shader_ok = try_shader_pair(plume::RenderShaderFormat::METAL);
    if(!shader_ok && sf != plume::RenderShaderFormat::SPIRV) shader_ok = try_shader_pair(plume::RenderShaderFormat::SPIRV);
#else
    if(!shader_ok && sf != plume::RenderShaderFormat::SPIRV) shader_ok = try_shader_pair(plume::RenderShaderFormat::SPIRV);
#endif
    if(!shader_ok) return set_diag("createShader (VS/PS)");
    LAUNCHER3D_TRACE("ensure_pipeline: shaders ready");

    s.gpu.set_builder = std::make_unique<plume::RenderDescriptorSetBuilder>();
    s.gpu.set_builder->begin();
    s.gpu.cb_idx = s.gpu.set_builder->addConstantBuffer(0, 1);
    s.gpu.a_idx = s.gpu.set_builder->addTexture(1);
    s.gpu.n_idx = s.gpu.set_builder->addTexture(2);
    s.gpu.s_idx = s.gpu.set_builder->addTexture(3);
    s.gpu.set_builder->addImmutableSampler(4, s.gpu.sampler.get());
    s.gpu.set_builder->end();

    plume::RenderPipelineLayoutBuilder lb{};
    lb.begin(false, true);
    lb.addDescriptorSet(*s.gpu.set_builder);
    lb.end();
    s.gpu.layout = lb.create(d);
    if(!s.gpu.layout) return set_diag("createPipelineLayout");
    LAUNCHER3D_TRACE("ensure_pipeline: pipeline layout ok");
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
    row->set_margin_bottom(5.0f);

    c.create_element<recompui::Label>(row, t, recompui::LabelStyle::Annotation);

    auto* sld = c.create_element<recompui::Slider>(row, recompui::SliderType::Double);
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
    s.panel.root->set_width(320);
    s.panel.root->set_padding(10);
    s.panel.root->set_border_radius(10);
    s.panel.root->set_background_color({0,0,0,170});

    auto* h=c.create_element<recompui::Clickable>(s.panel.root, true);
    h->set_display(recompui::Display::Flex);
    h->set_flex_direction(recompui::FlexDirection::Row);
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
    s.panel.status->set_margin_bottom(4);
    const auto add_float_slider = [&](const std::string& title, double min_value, double max_value, double step_value, float initial, const std::function<void(float)>& setter) {
        mk_slider(s.panel.content, title, min_value, max_value, step_value, initial, [setter](double v) {
            setter(static_cast<float>(v));
        });
    };

    add_float_slider("Pos X", -6.0, 6.0, 0.01, s.cfg.target_transform.position.x, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.position.x = v; });
    add_float_slider("Pos Y", -6.0, 6.0, 0.01, s.cfg.target_transform.position.y, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.position.y = v; });
    add_float_slider("Pos Z", -12.0, 4.0, 0.01, s.cfg.target_transform.position.z, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.position.z = v; });
    add_float_slider("Rot X", -180.0, 180.0, 0.1, s.cfg.target_transform.rotation_deg.pitch, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.rotation_deg.pitch = v; });
    add_float_slider("Rot Y", -180.0, 180.0, 0.1, s.cfg.target_transform.rotation_deg.yaw, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.rotation_deg.yaw = v; });
    add_float_slider("Rot Z", -180.0, 180.0, 0.1, s.cfg.target_transform.rotation_deg.roll, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.target_transform.rotation_deg.roll = v; });

    mk_slider(s.panel.content, "Scale", 0.01, 4.0, 0.01, s.cfg.target_transform.scale.x, [&s](double v) {
        const float uniform_scale = static_cast<float>(v);
        std::lock_guard lock(s.mx);
        s.cfg.target_transform.scale = { uniform_scale, uniform_scale, uniform_scale };
    });
    add_float_slider("Light X", -10.0, 10.0, 0.01, s.cfg.light.position_ws.x, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.position_ws.x = v; });
    add_float_slider("Light Y", -10.0, 10.0, 0.01, s.cfg.light.position_ws.y, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.position_ws.y = v; });
    add_float_slider("Light Z", -10.0, 10.0, 0.01, s.cfg.light.position_ws.z, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.position_ws.z = v; });
    add_float_slider("Light Range", 0.1, 50.0, 0.1, s.cfg.light.range, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.range = v; });
    add_float_slider("Light Int", 0.0, 10.0, 0.01, s.cfg.light.intensity, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.intensity = v; });
    add_float_slider("Ambient", 0.0, 2.0, 0.01, s.cfg.light.ambient_intensity, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.ambient_intensity = v; });
    add_float_slider("Light R", 0.0, 4.0, 0.01, s.cfg.light.color.x, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.color.x = v; });
    add_float_slider("Light G", 0.0, 4.0, 0.01, s.cfg.light.color.y, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.color.y = v; });
    add_float_slider("Light B", 0.0, 4.0, 0.01, s.cfg.light.color.z, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.light.color.z = v; });
    add_float_slider("Intro Time", 0.1, 8.0, 0.01, s.cfg.intro.duration_sec, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.intro.duration_sec = v; });
    add_float_slider("Overshoot", 0.0, 1.0, 0.01, s.cfg.intro.overshoot, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.intro.overshoot = v; });
    add_float_slider("Damping", 0.0, 20.0, 0.05, s.cfg.intro.damping, [&s](float v) { std::lock_guard lock(s.mx); s.cfg.intro.damping = v; });
    auto* row = c.create_element<recompui::Element>(s.panel.content);
    row->set_display(recompui::Display::Flex);
    row->set_flex_direction(recompui::FlexDirection::Row);
    row->set_justify_content(recompui::JustifyContent::SpaceBetween);

    auto* b_reset_pose = c.create_element<recompui::Button>(row, "Reset Pose", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Small);
    b_reset_pose->add_pressed_callback([&s]() {
        std::lock_guard lock(s.mx);
        s.cfg.target_transform = s.cfg_initial.target_transform;
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
    const uint64_t cb_sz = align_up_u64(static_cast<uint64_t>(sizeof(Constants)), 256);

    if (!vb_sz || !ib_sz) {
        LAUNCHER3D_TRACE("hook_draw: skipped GPU resource creation because vb/ib size is zero");
        return false;
    }

    LAUNCHER3D_TRACE(
        "hook_draw: creating GPU resources vb=%lluB ib=%lluB cb=%lluB(aligned)",
        static_cast<unsigned long long>(vb_sz),
        static_cast<unsigned long long>(ib_sz),
        static_cast<unsigned long long>(cb_sz)
    );

    LAUNCHER3D_TRACE("hook_draw: step create vb upload");
    s.gpu.vb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(vb_sz, plume::RenderBufferFlag::VERTEX));
    LAUNCHER3D_TRACE("hook_draw: step create ib upload");
    s.gpu.ib = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(ib_sz, plume::RenderBufferFlag::INDEX));
    LAUNCHER3D_TRACE("hook_draw: step create cb upload");
    s.gpu.cb = d->createBuffer(plume::RenderBufferDesc::UploadBuffer(cb_sz, plume::RenderBufferFlag::CONSTANT));
    s.gpu.cb_size = cb_sz;
    LAUNCHER3D_TRACE("hook_draw: step create descriptor set");
    s.gpu.set = s.gpu.set_builder->create(d);

    if (!(s.gpu.vb && s.gpu.ib && s.gpu.cb && s.gpu.set)) {
        LAUNCHER3D_TRACE(
            "hook_draw: resource alloc failed vb=%s ib=%s cb=%s set=%s",
            yes_no(s.gpu.vb != nullptr),
            yes_no(s.gpu.ib != nullptr),
            yes_no(s.gpu.cb != nullptr),
            yes_no(s.gpu.set != nullptr)
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

    LAUNCHER3D_TRACE("hook_draw: step bind descriptor resources");
    s.gpu.set->setBuffer(s.gpu.cb_idx, s.gpu.cb.get(), s.gpu.cb_size);
    s.gpu.set->setTexture(s.gpu.a_idx, s.gpu.tex_albedo.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.n_idx, s.gpu.tex_normal.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.set->setTexture(s.gpu.s_idx, s.gpu.tex_spec.get(), plume::RenderTextureLayout::SHADER_READ);
    s.gpu.index_count = s.cpu.indices.size();
    s.gpu.ready = true;
    LAUNCHER3D_TRACE("hook_draw: GPU resources ready index_count=%zu", s.gpu.index_count);
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
    c.view_proj = Mul(
        LookAt({ 0, 0, kCameraZ }, { 0, 0, 0 }, { 0, 1, 0 }),
        P(55.0f * (std::numbers::pi_v<float> / 180.0f), static_cast<float>(w) / static_cast<float>(h), 0.01f, 100.0f)
    );
    const V4 center_ws = mul_row_vec({ 0.0f, 0.0f, 0.0f, 1.0f }, c.model);
    s.last_clip_center = mul_row_vec(center_ws, c.view_proj);
    c.light_pos_range = { s.cfg.light.position_ws.x, s.cfg.light.position_ws.y, s.cfg.light.position_ws.z, s.cfg.light.range };
    c.light_dir_intensity = { s.cfg.light.direction_ws.x, s.cfg.light.direction_ws.y, s.cfg.light.direction_ws.z, s.cfg.light.intensity };
    c.light_color_ambient = { s.cfg.light.color.x, s.cfg.light.color.y, s.cfg.light.color.z, s.cfg.light.ambient_intensity };
    c.camera_spec = { 0, 0, kCameraZ, s.cpu.spec_factor };
    c.base_color = s.cpu.base_color;
    c.spec_color = s.cpu.spec_color;

    void* cb_ptr = s.gpu.cb->map();
    if (cb_ptr == nullptr) {
        LAUNCHER3D_TRACE("hook_draw: cb map failed");
        s.pipeline_diag = "CB map failed";
        clear_gpu(s);
        return false;
    }
    std::memcpy(cb_ptr, &c, sizeof(c));
    s.gpu.cb->unmap();

    plume::RenderTextureBarrier barriers[] = {
        { s.gpu.tex_albedo.get(), plume::RenderTextureLayout::SHADER_READ },
        { s.gpu.tex_normal.get(), plume::RenderTextureLayout::SHADER_READ },
        { s.gpu.tex_spec.get(), plume::RenderTextureLayout::SHADER_READ }
    };
    list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, static_cast<uint32_t>(std::size(barriers)));

    list->setGraphicsPipelineLayout(s.gpu.layout.get());
    list->setPipeline(s.gpu.pipeline.get());
    list->setGraphicsDescriptorSet(s.gpu.set.get(), 0);
    list->setViewports(plume::RenderViewport{ 0, 0, static_cast<float>(w), static_cast<float>(h) });
    list->setScissors(plume::RenderRect{ 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h) });

    plume::RenderVertexBufferView vbv(s.gpu.vb.get(), static_cast<uint64_t>(s.cpu.vertices.size()) * sizeof(Vertex));
    plume::RenderIndexBufferView ibv(s.gpu.ib.get(), static_cast<uint64_t>(s.cpu.indices.size()) * sizeof(uint32_t), plume::RenderFormat::R32_UINT);
    list->setVertexBuffers(0, &vbv, 1, &s.gpu.slot);
    list->setIndexBuffer(&ibv);
    list->drawIndexedInstanced(static_cast<uint32_t>(s.gpu.index_count), 1, 0, 0, 0);
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
        const float clip_w = std::max(std::abs(s.last_clip_center.w), 1e-6f);
        const float ndc_x = s.last_clip_center.x / clip_w;
        const float ndc_y = s.last_clip_center.y / clip_w;
        const float ndc_z = s.last_clip_center.z / clip_w;

        ss << (s.enabled ? "Enabled" : "Disabled")
           << " | " << (s.configured ? "Model OK" : "No Model")
           << " | Draw " << (should_draw(s) ? "Yes" : "No")
           << " | Pipe " << (s.gpu.pipeline_ok ? "Yes" : "No")
           << " | Ready " << (s.gpu.ready ? "Yes" : "No")
           << " | Idx " << s.gpu.index_count
           << " | Try " << (s.last_draw_attempted ? "Yes" : "No")
           << " | NDC(" << ndc_x << "," << ndc_y << "," << ndc_z << ")"
           << " | CfgCtx " << (cfg_visible() ? "On" : "Off")
           << " | " << (s.pipeline_diag.empty() ? "Diag OK" : ("Diag " + s.pipeline_diag))
           << " | " << ((s.intro_finished && s.cfg.intro.play_once) ? "Intro Done" : "Intro Active");

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
    o << "    .visible_only_on_title_screen = " << (c.visible_only_on_title_screen?"true":"false") << ",\n";
    o << "};\n";
    return o.str();
}

} // namespace csdk::launcher3d
