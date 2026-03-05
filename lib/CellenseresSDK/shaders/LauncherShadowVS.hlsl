#pragma pack_matrix(row_major)

cbuffer ShadowCB : register(b0) {
    float4x4 model;
    float4x4 light_view_proj;
    float4 light_pos_far;
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
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    float4 world_pos = mul(float4(input.position, 1.0f), model);
    output.position = mul(world_pos, light_view_proj);
    output.world_pos = world_pos.xyz;
    return output;
}
