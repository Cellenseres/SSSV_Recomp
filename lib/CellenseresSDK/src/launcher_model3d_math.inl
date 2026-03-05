M4 I() {
    M4 o{};
    o.m[0] = 1.0f;
    o.m[5] = 1.0f;
    o.m[10] = 1.0f;
    o.m[15] = 1.0f;
    return o;
}

void set_identity_shadow_matrices(Constants& c) {
    for (M4& m : c.shadow_view_proj) {
        m = I();
    }
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

float len(V3 v) {
    return std::sqrt(dot(v, v));
}

V4 mul_row_vec(V4 v, const M4& m) {
    return {
        v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8] + v.w * m.m[12],
        v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9] + v.w * m.m[13],
        v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10] + v.w * m.m[14],
        v.x * m.m[3] + v.y * m.m[7] + v.z * m.m[11] + v.w * m.m[15]
    };
}

V3 norm(V3 v) {
    const float length = std::sqrt(std::max(dot(v, v), 1e-12f));
    return { v.x / length, v.y / length, v.z / length };
}

V3 add(V3 a, V3 b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

V3 mul(V3 a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}

float max3(float x, float y, float z) {
    return std::max(x, std::max(y, z));
}

V3 camera_from_orbit(float yaw_deg, float pitch_deg, float distance, V3 target) {
    const float d2r = std::numbers::pi_v<float> / 180.0f;
    const float yaw = yaw_deg * d2r;
    const float pitch = pitch_deg * d2r;
    const float cp = std::cos(pitch);
    const V3 dir{
        cp * std::sin(yaw),
        std::sin(pitch),
        cp * std::cos(yaw)
    };
    return add(target, mul(dir, distance));
}

void build_debug_light_marker_mesh(std::vector<Vertex>& out_vertices, std::vector<uint32_t>& out_indices) {
    out_vertices.clear();
    out_indices.clear();
    out_vertices.reserve((kDebugLightMarkerStacks + 1) * (kDebugLightMarkerSlices + 1));
    out_indices.reserve(kDebugLightMarkerStacks * kDebugLightMarkerSlices * 6);

    for (uint32_t stack = 0; stack <= kDebugLightMarkerStacks; ++stack) {
        const float v = static_cast<float>(stack) / static_cast<float>(kDebugLightMarkerStacks);
        const float phi = std::numbers::pi_v<float> * v;
        const float sin_phi = std::sin(phi);
        const float cos_phi = std::cos(phi);

        for (uint32_t slice = 0; slice <= kDebugLightMarkerSlices; ++slice) {
            const float u = static_cast<float>(slice) / static_cast<float>(kDebugLightMarkerSlices);
            const float theta = 2.0f * std::numbers::pi_v<float> * u;
            const float sin_theta = std::sin(theta);
            const float cos_theta = std::cos(theta);

            const V3 normal{
                sin_phi * cos_theta,
                cos_phi,
                sin_phi * sin_theta
            };
            V3 tangent{ -sin_theta, 0.0f, cos_theta };
            if (len(tangent) <= 1e-6f) {
                tangent = { 1.0f, 0.0f, 0.0f };
            } else {
                tangent = norm(tangent);
            }

            Vertex vtx{};
            vtx.p = mul(normal, kDebugLightMarkerRadius);
            vtx.n = normal;
            vtx.t = { tangent.x, tangent.y, tangent.z, 1.0f };
            vtx.uv = { u, v };
            out_vertices.push_back(vtx);
        }
    }

    const uint32_t stride = kDebugLightMarkerSlices + 1;
    for (uint32_t stack = 0; stack < kDebugLightMarkerStacks; ++stack) {
        for (uint32_t slice = 0; slice < kDebugLightMarkerSlices; ++slice) {
            const uint32_t i0 = stack * stride + slice;
            const uint32_t i1 = i0 + stride;
            const uint32_t i2 = i0 + 1;
            const uint32_t i3 = i1 + 1;

            if (stack != 0) {
                out_indices.push_back(i0);
                out_indices.push_back(i1);
                out_indices.push_back(i2);
            }
            if (stack != (kDebugLightMarkerStacks - 1)) {
                out_indices.push_back(i2);
                out_indices.push_back(i1);
                out_indices.push_back(i3);
            }
        }
    }
}

const char* active_shadow_mode_name(ActiveShadowMode mode) {
    switch (mode) {
    case ActiveShadowMode::PointAtlas:
        return "Point";
    case ActiveShadowMode::Spot:
        return "Spot";
    case ActiveShadowMode::Disabled:
    default:
        return "Disabled";
    }
}

bool near_equal(float a, float b, float eps = 1e-4f) {
    return std::abs(a - b) <= eps;
}

bool near_equal_vec3(const Vec3& a, const Vec3& b, float eps = 1e-4f) {
    return near_equal(a.x, b.x, eps) && near_equal(a.y, b.y, eps) && near_equal(a.z, b.z, eps);
}

bool near_equal_euler(const EulerDeg& a, const EulerDeg& b, float eps = 1e-3f) {
    return near_equal(a.pitch, b.pitch, eps) && near_equal(a.yaw, b.yaw, eps) && near_equal(a.roll, b.roll, eps);
}

bool near_equal_transform(const Transform& a, const Transform& b) {
    return near_equal_vec3(a.position, b.position) &&
           near_equal_euler(a.rotation_deg, b.rotation_deg) &&
           near_equal_vec3(a.scale, b.scale);
}

bool near_equal_light(const Light& a, const Light& b) {
    return near_equal_vec3(a.direction_ws, b.direction_ws) &&
           near_equal_vec3(a.position_ws, b.position_ws) &&
           near_equal(a.range, b.range, 1e-3f) &&
           near_equal_vec3(a.color, b.color) &&
           near_equal(a.intensity, b.intensity, 1e-3f) &&
           near_equal(a.ambient_intensity, b.ambient_intensity, 1e-3f);
}

bool near_equal_shadow_cfg(const ShadowConfig& a, const ShadowConfig& b) {
    return (a.enabled == b.enabled) &&
           (a.mode == b.mode) &&
           (a.resolution == b.resolution) &&
           near_equal(a.strength, b.strength, 1e-3f) &&
           near_equal(a.depth_bias, b.depth_bias, 1e-5f) &&
           near_equal(a.normal_bias, b.normal_bias, 1e-4f) &&
           near_equal(a.softness, b.softness, 1e-3f) &&
           near_equal(a.near_plane, b.near_plane, 1e-4f) &&
           near_equal(a.far_plane_override, b.far_plane_override, 1e-3f);
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

M4 LookAt(V3 e, V3 t, V3 up) {
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

V3 transform_point(const M4& m, V3 p) {
    const V4 v = mul_row_vec({ p.x, p.y, p.z, 1.0f }, m);
    return { v.x, v.y, v.z };
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
    t = clamp01(t);
    const float s_back = 1.70158f + overs * 3.5f;
    const float u = t - 1.0f;
    float v = 1.0f + s_back * u * u * u + (s_back - 1.0f) * u * u;
    v += std::exp(-damping * t) * std::sin(t * std::numbers::pi_v<float> * 2.0f) * overs * 0.08f;
    return v;
}
