// Samples the overlay atlas and applies the tint.
//
// The atlas is PREMULTIPLIED alpha (QImage::Format_ARGB32_Premultiplied), and
// the blend state is ONE / INV_SRC_ALPHA to match. That pairing is what lets a
// single scalar multiply act as an opacity fade: scaling a premultiplied RGBA
// by k gives exactly the premultiplied colour for alpha*k.
Texture2D    atlasTexture : register(t0);
SamplerState atlasSampler : register(s0);

cbuffer OverlayConstants : register(b0) {
    float4 dstRect;
    float4 srcRect;
    float4 tint;
    float4 viewport;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut input) : SV_TARGET {
    return atlasTexture.Sample(atlasSampler, input.uv) * tint;
}
