#pragma pack_matrix(row_major)

cbuffer ShadowCB : register(b0) {
    float4x4 model;
    float4x4 light_view_proj;
    float4 light_pos_far;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 world_pos : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET {
    float far_plane = max(light_pos_far.w, 1e-3f);
    float linear_depth = saturate(distance(input.world_pos, light_pos_far.xyz) / far_plane);
    return float4(linear_depth, linear_depth, linear_depth, 1.0f);
}
