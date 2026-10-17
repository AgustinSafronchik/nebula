// Inline ray-query pass for Nebula.
//
// One thread per pixel. Each pixel builds a primary ray through the game's own
// view-projection matrix and traces it against the frame's top-level
// acceleration structure with an inline RayQuery. The result is a ray-traced
// image of the same scene the rasteriser drew, which is what makes the path
// verifiable: the traced silhouettes have to line up with the game's.
//
// Shading is deliberately simple. The game's textures and materials are not
// bound to this pass, so each hit is lit with one directional light and tinted
// by the instance it belongs to. This is ray-traced geometry with approximate
// shading; material-accurate images need the texture and material bindings
// plumbed through, which is a separate slice.
//
// Matrix layout: inv_view_proj arrives as four float4 rows so the packing is
// unambiguous. world = x*row0 + y*row1 + z*row2 + row3. The camera position is
// the translation row, which the pass uses as the ray origin.

struct RayParams {
    float4 inv_view_proj_row0;
    float4 inv_view_proj_row1;
    float4 inv_view_proj_row2;
    float4 inv_view_proj_row3;
    // x = width, y = height, z = near plane, w = far plane.
    float4 screen;
    // xyz = light direction (towards the light), w = ambient.
    float4 light;
    // x = overall strength, y = miss-sky intensity, zw unused.
    float4 tune;
};

StructuredBuffer<float4> g_instances : register(t1);
RaytracingAccelerationStructure g_tlas : register(t2);
RWTexture2D<float4> g_output : register(u0);

RayParams g_params : register(b0);

// This DXC build does not expose the CommittedStatus enum type or its
// enumerators, so the status is compared against its integer value:
// NOT_COMMITTED is 0 and COMMITTED is 1.
static const uint kNotCommitted = 0u;

// Unprojects a homogeneous NDC point to world space.
float3 world_from_ndc(float ndc_x, float ndc_y, float ndc_z) {
    const RayParams p = g_params;
    return ndc_x * p.inv_view_proj_row0.xyz +
           ndc_y * p.inv_view_proj_row1.xyz +
           ndc_z * p.inv_view_proj_row2.xyz +
           p.inv_view_proj_row3.xyz;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatch_id : SV_DispatchThreadID) {
    const RayParams p = g_params;
    const uint width = (uint)p.screen.x;
    const uint height = (uint)p.screen.y;
    if (dispatch_id.x >= width || dispatch_id.y >= height) {
        return;
    }

    const float ndc_x = ((float)dispatch_id.x + 0.5f) / (float)width * 2.0f - 1.0f;
    const float ndc_y = 1.0f - ((float)dispatch_id.y + 0.5f) / (float)height * 2.0f;

    // The camera sits at the translation row of the inverse view-projection.
    const float3 origin = p.inv_view_proj_row3.xyz;

    // Unproject two points along the view axis and take the difference as the
    // ray direction. This needs no assumptions about the projection's sign
    // conventions, unlike dividing by a single w.
    const float3 near_point = world_from_ndc(ndc_x, ndc_y, 0.0);
    const float3 far_point = world_from_ndc(ndc_x, ndc_y, 1.0);
    const float3 delta = far_point - near_point;
    if (dot(delta, delta) < 1e-12) {
        g_output[dispatch_id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float3 direction = normalize(delta);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = max(p.screen.z, 1e-4);
    ray.TMax = p.screen.w;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(g_tlas, RAY_FLAG_NONE, 0xFF, ray);

    float3 color;
    float coverage;
    if ((uint)query.CommittedStatus() != kNotCommitted) {
        const float4 tint = g_instances[query.CommittedInstanceIndex()];
        const float lambert = saturate(dot(direction, normalize(p.light.xyz))) * 0.8 + 0.2;
        color = tint.rgb * lambert + tint.rgb * p.light.w;
        // Fade the far end of the ray range so distant geometry does not read
        // as flat as the near geometry.
        coverage = saturate(1.0 - query.CommittedRayT() / max(p.screen.w, 1.0));
    } else {
        // Miss: a vertical gradient so the sky reads as background rather than
        // as a hole in the traced image.
        const float t = saturate(direction.y * 0.5 + 0.5);
        color = lerp(float3(0.35, 0.45, 0.62), float3(0.75, 0.82, 0.92), t) * p.tune.y;
        coverage = 0.0;
    }

    g_output[dispatch_id.xy] = float4(color * p.tune.x, coverage);
}