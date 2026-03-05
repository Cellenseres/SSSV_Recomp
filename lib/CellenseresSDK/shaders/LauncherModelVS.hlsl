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

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
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

VSOutput VSMain(VSInput input) {
    VSOutput output;

    float4 world_pos = mul(float4(input.position, 1.0f), model);
    float3x3 model3 = (float3x3)model;

    output.position = mul(world_pos, view_proj);
    output.world_pos = world_pos.xyz;
    output.world_normal = safe_normalize(mul(input.normal, model3), float3(0.0f, 0.0f, 1.0f));
    output.world_tangent = float4(safe_normalize(mul(input.tangent.xyz, model3), float3(1.0f, 0.0f, 0.0f)), input.tangent.w);
    output.uv = input.uv;

    return output;
}
