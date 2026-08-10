// GATE C: three planes in, RGB out. The matrix, the range normalisation and the
// bit-depth scale all arrive as constants, so this is ONE shader for every
// combination of 4:2:0/4:2:2/4:4:4, 8/10/12-bit and BT.601/709/2020 rather than
// a family of compiled variants (plan section 11).
//
// Chroma subsampling needs no code here. The U and V textures are simply
// smaller, and normalised coordinates plus a linear sampler upsample them --
// which is why 4:2:0 and 4:4:4 differ only in the size of two textures.
//
// Letterboxing is likewise absent: the viewport is set to the fitted rect, so
// this shader never learns the picture is not filling the back buffer.
Texture2D    planeY : register(t0);
Texture2D    planeU : register(t1);
Texture2D    planeV : register(t2);
SamplerState srcSampler : register(s0);

cbuffer YuvParams : register(b0) {
    // Rows of the YUV->RGB matrix. float4 rather than float3 because a constant
    // buffer packs to 16 bytes anyway; the fourth component is unused.
    float4 matR;
    float4 matG;
    float4 matB;
    // sampleScale lifts a sample to the [0,1] its code range implies. R8_UNORM
    // already gives value/255, so it is 1.0 at 8 bits; R16_UNORM gives
    // value/65535 for data that only occupies the low N bits, so it is
    // 65535/(2^N - 1) above 8.
    //
    // The offsets and scales are computed on the CPU FOR THE ACTUAL BIT DEPTH,
    // not by reusing the 8-bit 16/255 and 128/255. Those are not the same
    // numbers at 10-bit: black is code 64 of 1023 (0.062561), where 16/255 is
    // 0.062745. Small, but it is a lift of the black point across the whole
    // picture, which is precisely the "global level shift" section 11 warns a
    // wrong factor shows up as.
    float  sampleScale;
    float  lumaOffset;
    float  lumaScale;
    float  chromaOffset;
    float  chromaScale;
    float3 padding;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut input) : SV_TARGET {
    float y = planeY.Sample(srcSampler, input.uv).r * sampleScale;
    float u = planeU.Sample(srcSampler, input.uv).r * sampleScale;
    float v = planeV.Sample(srcSampler, input.uv).r * sampleScale;

    y = (y - lumaOffset) * lumaScale;
    u = (u - chromaOffset) * chromaScale;
    v = (v - chromaOffset) * chromaScale;

    const float3 yuv = float3(y, u, v);
    float3 rgb = float3(dot(matR.xyz, yuv), dot(matG.xyz, yuv), dot(matB.xyz, yuv));

    // Limited-range sources legitimately carry codes below black and above
    // white (footroom and headroom). The CPU path clamps them because swscale
    // writes 8-bit BGRA; clamping here keeps the two paths comparable rather
    // than letting the GPU show detail the CPU cannot, which would read as a
    // colour difference in the A/B when it is a range difference.
    rgb = saturate(rgb);

    // Alpha forced opaque, as in the BGRA shader: the swapchain ignores it and
    // nothing should come to depend on what is in it.
    return float4(rgb, 1.0);
}
