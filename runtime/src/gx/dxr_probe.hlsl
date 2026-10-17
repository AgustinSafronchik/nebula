// Ray-query pass stand-in, for validating everything except the ray tracing.
//
// The AMD driver in use crashes its own shader compiler on any inline
// RayQuery shader, so the real pass never produces an image. This shader writes
// the same output texture with a synthetic pattern instead, which exercises the
// whole remaining path: the compute dispatch, the UAV, the transition out of
// UNORDERED_ACCESS, and the blend into the presented frame.
//
// Alpha varies across the screen so the blend's screen-style term is visible:
// where alpha is low the rasterised image should dominate, where it is high the
// pattern should dominate.
//
// Selected with GALAXY_DXR_PROBE=1.

RWTexture2D<float4> g_output : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const float2 uv = (float2)id.xy / 1536.0f;
    // A coarse grid over a gradient, so any misalignment between the traced
    // texture and the presented frame is obvious by eye.
    const float2 cell = frac(uv * 12.0f);
    const float grid = step(0.94f, cell.x) + step(0.94f, cell.y);
    const float3 base = float3(0.10f, 0.45f, 0.85f) * uv.y;
    const float3 color = base + float3(0.35f, 0.30f, 0.0f) * saturate(grid);
    // Ramps across the screen so the blend weighting is observable.
    g_output[id.xy] = float4(color, saturate(uv.x));
}

