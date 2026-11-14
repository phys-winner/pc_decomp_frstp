Texture2D<float4> image2D : register(t0);
Texture2DArray<float4> imageArray : register(t1);
TextureCube<float4> imageCube : register(t2);
SamplerState point_sampler : register(s0);
cbuffer Sampling : register(b0) { uint level; uint layer; uint width; uint height; };
struct Output { float4 position : SV_Position; };
Output vs_main(uint id : SV_VertexID) {
    Output o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float2 pixel_uv(Output i) { return i.position.xy / float2(width, height); }
float4 ps_2d(Output i) : SV_Target {
    return image2D.SampleLevel(point_sampler, pixel_uv(i), float(level));
}
float4 ps_array(Output i) : SV_Target {
    return imageArray.SampleLevel(point_sampler, float3(pixel_uv(i), float(layer)), float(level));
}
float4 ps_cube(Output i) : SV_Target {
    float2 p = pixel_uv(i) * 2 - 1;
    float3 direction;
    if (layer == 0) direction = float3(1, -p.y, -p.x);
    else if (layer == 1) direction = float3(-1, -p.y, p.x);
    else if (layer == 2) direction = float3(p.x, 1, p.y);
    else if (layer == 3) direction = float3(p.x, -1, -p.y);
    else if (layer == 4) direction = float3(p.x, -p.y, 1);
    else direction = float3(-p.x, -p.y, -1);
    return imageCube.SampleLevel(point_sampler, direction, float(level));
}
