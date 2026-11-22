// Shared culling helpers for the Soft Mesh Shader GPU-driven cull passes.
// Imported by both the LDS and the subgroup (wave) variants of
// Task.SoftMeshShaderGpuCullCompact / Task.SoftMeshShaderShadowGpuCullCompact.
module SoftMeshShaderCull;
import Shader.Bindless;
import Shader.Types;
import Shader.GpuScene;

// Conservative occlusion test against the previous frame's depth (reverse-Z: larger is closer).
// The box is reduced to its screen rectangle and the depth of its nearest corner, and it counts as
// occluded only when every depth sampled across that rectangle is closer than that corner. Samples
// are at most kOcclusionSampleStride pixels apart, so a gap at least that wide -- a doorway, a
// window -- keeps whatever is seen through it. A rectangle that would need more than
// kOcclusionMaxSamplesPerAxis samples on an axis is treated as visible: it is large on screen, so
// culling it rarely pays, and a coarser grid would miss gaps again. Testing a 6^3 grid of points on
// the box itself missed them: a wall seen only through a doorway had no grid point inside the
// doorway and was culled on alternate frames as the projection jitter moved the points.
public bool IsOccludedVolume(float3 boxMin, float3 boxMax, float4x4 wvp)
{
    const float kOcclusionSampleStride = 8.0f;
    const uint kOcclusionMaxSamplesPerAxis = 24u;

    const uint2 screenSize = Bindless.GetViewStorageTextureDimensions<float>(Bindless.RT_PREV_DEPTHBUFFER);
    if (any(screenSize == uint2(0u, 0u)))
    {
        return false;
    }

    float2 rectMin = float2(1e30f, 1e30f);
    float2 rectMax = float2(-1e30f, -1e30f);
    float nearestDepth = 0.0f;
    for (uint i = 0; i < 8u; ++i)
    {
        const float3 corner = float3(
            (i & 1u) != 0u ? boxMax.x : boxMin.x,
            (i & 2u) != 0u ? boxMax.y : boxMin.y,
            (i & 4u) != 0u ? boxMax.z : boxMin.z);
        const float4 clipPos = mul(wvp, float4(corner, 1.0f));
        if (clipPos.w <= 0.0f)
        {
            return false;
        }
        const float3 ndc = clipPos.xyz / clipPos.w;
        if (ndc.z < 0.0f || ndc.z > 1.0f)
        {
            return false;
        }
        const float2 pixel = (ndc.xy * 0.5f + 0.5f) * float2(screenSize);
        rectMin = min(rectMin, pixel);
        rectMax = max(rectMax, pixel);
        nearestDepth = max(nearestDepth, ndc.z);
    }

    // Only the on-screen part of the box can be seen; the frustum test already rejected boxes that
    // are entirely off screen.
    const float2 lastPixel = float2(screenSize) - 1.0f;
    if (any(rectMax < 0.0f) || any(rectMin > lastPixel))
    {
        return false;
    }
    const int2 pixelMin = int2(clamp(rectMin, float2(0.0f, 0.0f), lastPixel));
    const int2 pixelMax = int2(clamp(rectMax, float2(0.0f, 0.0f), lastPixel));
    const float2 extent = float2(pixelMax - pixelMin);
    const uint2 intervals = uint2(ceil(extent / kOcclusionSampleStride));
    if (any(intervals + 1u > uint2(kOcclusionMaxSamplesPerAxis, kOcclusionMaxSamplesPerAxis)))
    {
        return false;
    }

    let previousDepth = Bindless.GetViewStorageTexture<float>(Bindless.RT_PREV_DEPTHBUFFER);
    const float2 spacing = extent / float2(max(intervals, uint2(1u, 1u)));
    const float visibleThreshold = nearestDepth + 0.00001f;
    for (uint sx = 0; sx <= intervals.x; ++sx)
    {
        for (uint sy = 0; sy <= intervals.y; ++sy)
        {
            const int2 samplePixel = pixelMin + int2(round(spacing * float2(sx, sy)));
            // A previous sample farther than the nearest corner means the box may show there.
            if (previousDepth[min(samplePixel, pixelMax)] < visibleThreshold)
            {
                return false;
            }
        }
    }

    return true;
}

public bool IsAABBInFrustum(float3 min, float3 max, float4x4 wvp)
{
    float3 corners[8];
    corners[0] = float3(min.x, min.y, min.z);
    corners[1] = float3(max.x, min.y, min.z);
    corners[2] = float3(min.x, max.y, min.z);
    corners[3] = float3(max.x, max.y, min.z);
    corners[4] = float3(min.x, min.y, max.z);
    corners[5] = float3(max.x, min.y, max.z);
    corners[6] = float3(min.x, max.y, max.z);
    corners[7] = float3(max.x, max.y, max.z);

    bool outside = true;

    outside = true;
    for (int i = 0; i < 8; i++)
    {
        float4 clipPos = mul(wvp, float4(corners[i], 1.0));
        if (clipPos.x >= -clipPos.w) { outside = false; break; }
    }
    if (outside) return false;

    outside = true;
    for (int i = 0; i < 8; i++)
    {
        float4 clipPos = mul(wvp, float4(corners[i], 1.0));
        if (clipPos.x <= clipPos.w) { outside = false; break; }
    }
    if (outside) return false;

    outside = true;
    for (int i = 0; i < 8; i++)
    {
        float4 clipPos = mul(wvp, float4(corners[i], 1.0));
        if (clipPos.y >= -clipPos.w) { outside = false; break; }
    }
    if (outside) return false;

    outside = true;
    for (int i = 0; i < 8; i++)
    {
        float4 clipPos = mul(wvp, float4(corners[i], 1.0));
        if (clipPos.y <= clipPos.w) { outside = false; break; }
    }
    if (outside) return false;

    outside = true;
    for (int i = 0; i < 8; i++)
    {
        float4 clipPos = mul(wvp, float4(corners[i], 1.0));
        if (clipPos.z >= -clipPos.w) { outside = false; break; }
    }
    if (outside) return false;

    outside = true;
    for (int i = 0; i < 8; i++)
    {
        float4 clipPos = mul(wvp, float4(corners[i], 1.0));
        if (clipPos.z <= clipPos.w) { outside = false; break; }
    }
    if (outside) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Discrete LOD selection, shared by the main and shadow cull passes (and their wave variants) so
// every path resolves a level the same way. The choice is made once per proxy here; expansion and
// the vertex stage only ever see the index range it resolved to.
// ---------------------------------------------------------------------------

// Longest screen-space edge of the proxy's projected bounds, in pixels. Anything crossing the near
// plane returns a huge extent: bounds straddling w=0 have no meaningful projected size, and forcing
// full detail there is both cheap (it is close) and safe.
public float ComputeScreenExtent(float3 minPoint, float3 maxPoint, float4x4 wvp, float2 viewportSize)
{
    float2 ndcMin = float2(1e30f, 1e30f);
    float2 ndcMax = float2(-1e30f, -1e30f);
    for (uint i = 0; i < 8u; ++i)
    {
        const float3 corner = float3(
            (i & 1u) != 0u ? maxPoint.x : minPoint.x,
            (i & 2u) != 0u ? maxPoint.y : minPoint.y,
            (i & 4u) != 0u ? maxPoint.z : minPoint.z);
        const float4 clipPos = mul(wvp, float4(corner, 1.0));
        if (clipPos.w <= 1e-6f)
        {
            return 1e30f;
        }
        const float2 ndc = clipPos.xy / clipPos.w;
        ndcMin = min(ndcMin, ndc);
        ndcMax = max(ndcMax, ndc);
    }
    // NDC spans [-1,1], so half the extent maps onto the viewport dimension.
    const float2 pixelExtent = (ndcMax - ndcMin) * 0.5f * viewportSize;
    return max(pixelExtent.x, pixelExtent.y);
}

// Level 0 at or above baseThreshold pixels, halving the bar for each further level. A
// baseThreshold of 0 pins everything to level 0, which is the A/B baseline for measuring LOD.
public uint SelectLod(float screenExtent, uint lodCount, float baseThreshold)
{
    uint lod = 0u;
    float threshold = baseThreshold;
    while (lod + 1u < lodCount && screenExtent < threshold)
    {
        ++lod;
        threshold *= 0.5f;
    }
    return lod;
}

// The extent Task.RayTracingLodSelect sizes a TLAS instance by. Anything with a corner in front of
// the eye goes through ComputeScreenExtent untouched: the main view may draw it, and the traced
// proxy is only guaranteed to be the surface the visibility buffer holds if both sides resolve the
// same level from the same numbers. Bounds entirely behind the eye are never drawn, so they are
// free to differ; projecting them through the mirrored w sizes them as a camera facing backwards
// would, instead of pinning everything behind the player to LOD0.
public float ComputeRayTracingScreenExtent(float3 minPoint, float3 maxPoint, float4x4 wvp, float2 viewportSize)
{
    float2 ndcMin = float2(1e30f, 1e30f);
    float2 ndcMax = float2(-1e30f, -1e30f);
    for (uint i = 0; i < 8u; ++i)
    {
        const float3 corner = float3(
            (i & 1u) != 0u ? maxPoint.x : minPoint.x,
            (i & 2u) != 0u ? maxPoint.y : minPoint.y,
            (i & 4u) != 0u ? maxPoint.z : minPoint.z);
        const float4 clipPos = mul(wvp, float4(corner, 1.0));
        if (clipPos.w >= -1e-6f)
        {
            return ComputeScreenExtent(minPoint, maxPoint, wvp, viewportSize);
        }
        const float2 ndc = clipPos.xy / -clipPos.w;
        ndcMin = min(ndcMin, ndc);
        ndcMax = max(ndcMax, ndc);
    }
    const float2 pixelExtent = (ndcMax - ndcMin) * 0.5f * viewportSize;
    return max(pixelExtent.x, pixelExtent.y);
}
