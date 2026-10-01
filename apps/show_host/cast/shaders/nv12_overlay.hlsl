cbuffer ComposeRegion : register(b0) {
    uint2 region_origin;
    uint2 region_size;
};

Texture2D<float> base_y : register(t0);
Texture2D<float2> base_uv : register(t1);
Texture2D<float4> overlay_rgba : register(t2);
RWTexture2D<float> output_y : register(u0);
RWTexture2D<float2> output_uv : register(u1);

static const float kYBlack = 16.0 / 255.0;
static const float kChromaCenter = 128.0 / 255.0;
static const float3 kY709 = float3(
    46.5594 / 255.0, 156.6288 / 255.0, 15.8118 / 255.0);
static const float3 kU709 = float3(
    -25.664 / 255.0, -86.336 / 255.0, 112.0 / 255.0);
static const float3 kV709 = float3(
    112.0 / 255.0, -101.730 / 255.0, -10.270 / 255.0);

[numthreads(16, 16, 1)]
void compose_y(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= region_size.x
            || dispatch_id.y >= region_size.y) {
        return;
    }
    const uint2 pixel = region_origin + dispatch_id.xy;
    const float4 overlay = overlay_rgba.Load(int3(pixel, 0));
    const float alpha = saturate(overlay.a);
    const float overlay_y = kYBlack * alpha
        + dot(kY709, overlay.rgb);
    output_y[pixel] = saturate(overlay_y
        + base_y.Load(int3(pixel, 0)) * (1.0 - alpha));
}

[numthreads(16, 16, 1)]
void compose_uv(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint2 chroma_size = region_size / 2;
    if (dispatch_id.x >= chroma_size.x
            || dispatch_id.y >= chroma_size.y) {
        return;
    }
    const uint2 pixel = region_origin + dispatch_id.xy * 2;
    const float4 overlay = (
        overlay_rgba.Load(int3(pixel, 0))
        + overlay_rgba.Load(int3(pixel + uint2(1, 0), 0))
        + overlay_rgba.Load(int3(pixel + uint2(0, 1), 0))
        + overlay_rgba.Load(int3(pixel + uint2(1, 1), 0))) * 0.25;
    const float alpha = saturate(overlay.a);
    const float2 overlay_uv = float2(
        kChromaCenter * alpha + dot(kU709, overlay.rgb),
        kChromaCenter * alpha + dot(kV709, overlay.rgb));
    const uint2 chroma = pixel / 2;
    output_uv[chroma] = saturate(overlay_uv
        + base_uv.Load(int3(chroma, 0)) * (1.0 - alpha));
}
