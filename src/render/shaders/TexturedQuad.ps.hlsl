// GATE B presents what swscale already produced: BGRA8. The texture is created
// as DXGI_FORMAT_B8G8R8A8_UNORM, so the hardware handles the channel order and
// this shader is a straight sample.
//
// GATE C replaces this file with the planar YUV conversion (three samplers and
// the matrices in docs/gpu-initiative-plan.md section 11). The vertex shader
// and every other piece of the pipeline stay as they are, which is the reason
// the split is drawn here.
Texture2D    srcTexture : register(t0);
SamplerState srcSampler : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut input) : SV_TARGET {
    // Alpha forced opaque: the swapchain ignores alpha, and a frame carrying
    // anything but 255 there would otherwise depend on that staying true.
    return float4(srcTexture.Sample(srcSampler, input.uv).rgb, 1.0);
}
