#pragma pack_matrix(row_major)

cbuffer SceneCB : register(b0) {
    float4x4 model;
    float4x4 view_proj;
    float4 light_pos_range;
    float4 light_dir_intensity;
    float4 light_color_ambient;
    float4 camera_pos_spec_factor;
    float4 base_color_factor;
    float4 spec_color_factor;
    float4 shadow_params0;
    float4 shadow_params1;
    float4x4 shadow_view_proj[6];
};

Texture2D<float4> gAlbedo : register(t1);
Texture2D<float4> gNormal : register(t2);
Texture2D<float4> gSpecular : register(t3);
Texture2D<float4> gShadowAtlas : register(t5);
SamplerState gSampler : register(s4);
SamplerState gShadowSampler : register(s6);

struct PSInput {
    float4 position : SV_POSITION;
    float3 world_pos : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    float4 world_tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
};

float3 safe_normalize(float3 v, float3 fallback) {
    float len2 = dot(v, v);
    if (len2 <= 1e-8f) {
        return fallback;
    }
    return v * rsqrt(len2);
}

float3 srgb_to_linear(float3 c) {
    return pow(max(c, 0.0f), 2.2f.xxx);
}

float3 linear_to_srgb(float3 c) {
    return pow(max(c, 0.0f), (1.0f / 2.2f).xxx);
}

float sample_shadow_atlas(float2 uv) {
    return gShadowAtlas.Sample(gShadowSampler, saturate(uv)).r;
}

float2 clip_to_shadow_uv(float4 clip) {
    return float2(
        clip.x / clip.w * 0.5f + 0.5f,
        0.5f - clip.y / clip.w * 0.5f
    );
}

float2 shadow_uv_inset(float inv_res, float scale) {
    return float2(inv_res * scale, inv_res * scale);
}

float shadow_reference_depth(float3 world_pos, float far_plane) {
    return saturate(distance(world_pos, light_pos_range.xyz) / far_plane);
}

float shadow_bias(float3 world_pos, float3 normal_ws, float3 light_dir_ws, float far_plane) {
    float bias = shadow_params0.y + shadow_params0.z * (1.0f - max(dot(normal_ws, light_dir_ws), 0.0f));
    bias += 0.0005f * shadow_reference_depth(world_pos, far_plane);
    return bias;
}

float shadow_occlusion_spot(float3 world_pos, float3 normal_ws, float3 light_dir_ws) {
    float4 clip = mul(float4(world_pos, 1.0f), shadow_view_proj[0]);
    if (clip.w <= 1e-6f) {
        return 0.0f;
    }

    float3 ndc = clip.xyz / clip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f) {
        return 0.0f;
    }

    float far_plane = max(shadow_params1.z, 1e-3f);
    float2 uv = clip_to_shadow_uv(clip);
    float ref_depth = shadow_reference_depth(world_pos, far_plane) - shadow_bias(world_pos, normal_ws, light_dir_ws, far_plane);

    const int radius = 2;
    float occ = 0.0f;
    float taps = 0.0f;
    float inv_res = max(shadow_params1.w, 1e-6f);
    float kernel_scale = max(shadow_params0.w, 0.1f);
    float2 uv_inset = shadow_uv_inset(inv_res, 1.5f);
    [unroll]
    for (int y = -radius; y <= radius; y++) {
        [unroll]
        for (int x = -radius; x <= radius; x++) {
            float2 o = float2(x, y) * inv_res * kernel_scale;
            float s = sample_shadow_atlas(clamp(uv + o, uv_inset, 1.0f - uv_inset));
            occ += (ref_depth > s) ? 1.0f : 0.0f;
            taps += 1.0f;
        }
    }
    return occ / max(taps, 1.0f);
}

float2 point_face_tile(int face_idx) {
    if (face_idx == 0) return float2(0.0f, 0.0f);
    if (face_idx == 1) return float2(1.0f, 0.0f);
    if (face_idx == 2) return float2(2.0f, 0.0f);
    if (face_idx == 3) return float2(0.0f, 1.0f);
    if (face_idx == 4) return float2(1.0f, 1.0f);
    return float2(2.0f, 1.0f);
}

bool project_point_shadow_world_pos(float3 sample_world_pos, out float2 atlas_uv) {
    atlas_uv = 0.0f.xx;

    float best_score = 1e9f;
    float2 best_uv = 0.0f.xx;
    int best_face = -1;

    [unroll]
    for (int i = 0; i < 6; ++i) {
        float4 clip = mul(float4(sample_world_pos, 1.0f), shadow_view_proj[i]);
        if (clip.w <= 1e-6f) {
            continue;
        }

        float3 ndc = clip.xyz / clip.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f) {
            continue;
        }

        float edge_metric = max(abs(ndc.x), abs(ndc.y));
        if (edge_metric > (1.0f + 1e-4f)) {
            continue;
        }

        float2 uv_face = clip_to_shadow_uv(clip);
        const float inv_face_res = max(shadow_params1.w, 1e-6f);
        const float2 uv_inset = shadow_uv_inset(inv_face_res, 0.5f);
        uv_face = clamp(uv_face, uv_inset, 1.0f - uv_inset);

        if (edge_metric < best_score) {
            best_score = edge_metric;
            best_uv = uv_face;
            best_face = i;
        }
    }

    if (best_face < 0) {
        return false;
    }

    const float2 tile = point_face_tile(best_face);
    atlas_uv = float2((tile.x + best_uv.x) / 3.0f, (tile.y + best_uv.y) / 2.0f);
    return true;
}

float sample_shadow_point_world_pos(float3 sample_world_pos) {
    float2 atlas_uv = 0.0f.xx;
    if (!project_point_shadow_world_pos(sample_world_pos, atlas_uv)) {
        return 1.0f;
    }
    return sample_shadow_atlas(atlas_uv);
}

float shadow_occlusion_point(float3 world_pos, float3 normal_ws, float3 light_dir_ws) {
    float3 light_to_point = world_pos - light_pos_range.xyz;
    float dist_to_light = length(light_to_point);
    float far_plane = max(shadow_params1.z, 1e-3f);
    if (dist_to_light >= far_plane) {
        return 0.0f;
    }

    float3 base_dir = light_to_point / max(dist_to_light, 1e-6f);
    float ref_depth = saturate(dist_to_light / far_plane);
    ref_depth -= shadow_bias(world_pos, normal_ws, light_dir_ws, far_plane);

    const int radius = 2;
    float occ = 0.0f;
    float taps = 0.0f;
    float inv_face_res = max(shadow_params1.w, 1e-6f);
    float kernel_scale = max(shadow_params0.w, 0.1f);
    float angular_step = inv_face_res * kernel_scale * 2.0f;
    float3 basis_hint = (abs(base_dir.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = safe_normalize(cross(basis_hint, base_dir), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = cross(base_dir, tangent);
    [unroll]
    for (int y = -radius; y <= radius; y++) {
        [unroll]
        for (int x = -radius; x <= radius; x++) {
            float2 jitter = float2(x, y) * angular_step;
            float3 sample_dir = safe_normalize(base_dir + tangent * jitter.x + bitangent * jitter.y, base_dir);
            float3 sample_world_pos = light_pos_range.xyz + sample_dir * max(dist_to_light, 1e-4f);
            float s = sample_shadow_point_world_pos(sample_world_pos);
            occ += (ref_depth > s) ? 1.0f : 0.0f;
            taps += 1.0f;
        }
    }
    return occ / max(taps, 1.0f);
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 normal_ws = safe_normalize(input.world_normal, float3(0.0f, 0.0f, 1.0f));
    float3 tangent_ws = safe_normalize(input.world_tangent.xyz, float3(1.0f, 0.0f, 0.0f));
    float3 bitangent_ws = safe_normalize(cross(normal_ws, tangent_ws) * input.world_tangent.w, float3(0.0f, 1.0f, 0.0f));

    float3 normal_tex = gNormal.Sample(gSampler, input.uv).xyz * 2.0f - 1.0f;
    float3 mapped_normal_ws = safe_normalize(
        tangent_ws * normal_tex.x +
        bitangent_ws * normal_tex.y +
        normal_ws * normal_tex.z,
        normal_ws
    );

    float3 albedo = srgb_to_linear(gAlbedo.Sample(gSampler, input.uv).rgb) * base_color_factor.rgb;
    float3 specular_tex = srgb_to_linear(gSpecular.Sample(gSampler, input.uv).rgb);

    float3 light_dir_ws = safe_normalize(-light_dir_intensity.xyz, float3(0.0f, -1.0f, 0.0f));
    float attenuation = 1.0f;
    float light_range = max(light_pos_range.w, 0.0f);
    if (light_range > 1e-4f) {
        float3 to_light = light_pos_range.xyz - input.world_pos;
        float distance_to_light = length(to_light);
        light_dir_ws = safe_normalize(to_light, light_dir_ws);
        float fade = saturate(1.0f - (distance_to_light / light_range));
        attenuation = fade * fade;
    }
    float light_intensity = max(light_dir_intensity.w, 0.0f) * attenuation;
    float3 light_color = light_color_ambient.rgb;
    float ambient = max(light_color_ambient.w, 0.0f);

    float ndotl = max(dot(mapped_normal_ws, light_dir_ws), 0.0f);

    float shadow_occ = 0.0f;
    int shadow_mode = (int)round(shadow_params1.x);
    if (shadow_mode == 1) {
        shadow_occ = shadow_occlusion_point(input.world_pos, mapped_normal_ws, light_dir_ws);
    } else if (shadow_mode == 2) {
        shadow_occ = shadow_occlusion_spot(input.world_pos, mapped_normal_ws, light_dir_ws);
    }
    float direct_shadow = 1.0f - saturate(shadow_params0.x) * saturate(shadow_occ);

    float3 diffuse = albedo * (ambient + ndotl * light_intensity * direct_shadow) * light_color;

    float3 view_dir_ws = safe_normalize(camera_pos_spec_factor.xyz - input.world_pos, float3(0.0f, 0.0f, 1.0f));
    float3 half_vec = safe_normalize(light_dir_ws + view_dir_ws, light_dir_ws);
    float ndoth = max(dot(mapped_normal_ws, half_vec), 0.0f);
    float spec_factor = max(camera_pos_spec_factor.w, 0.0f);
    float spec_power = lerp(16.0f, 96.0f, saturate(spec_factor));
    float spec_strength = pow(ndoth, spec_power) * spec_factor;

    float3 specular = spec_strength * spec_color_factor.rgb * specular_tex * light_color * light_intensity * direct_shadow;
    float3 final_linear = diffuse + specular;

    return float4(linear_to_srgb(final_linear), 1.0f);
}
