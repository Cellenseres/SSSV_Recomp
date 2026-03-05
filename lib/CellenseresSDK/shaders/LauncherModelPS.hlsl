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
};

Texture2D<float4> gAlbedo : register(t1);
Texture2D<float4> gNormal : register(t2);
Texture2D<float4> gSpecular : register(t3);
SamplerState gSampler : register(s4);

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
    float3 diffuse = albedo * (ambient + ndotl * light_intensity) * light_color;

    float3 view_dir_ws = safe_normalize(camera_pos_spec_factor.xyz - input.world_pos, float3(0.0f, 0.0f, 1.0f));
    float3 half_vec = safe_normalize(light_dir_ws + view_dir_ws, light_dir_ws);
    float ndoth = max(dot(mapped_normal_ws, half_vec), 0.0f);
    float spec_factor = max(camera_pos_spec_factor.w, 0.0f);
    float spec_power = lerp(16.0f, 96.0f, saturate(spec_factor));
    float spec_strength = pow(ndoth, spec_power) * spec_factor;

    float3 specular = spec_strength * spec_color_factor.rgb * specular_tex * light_color * light_intensity;
    float3 final_linear = diffuse + specular;

    return float4(linear_to_srgb(final_linear), 1.0f);
}
