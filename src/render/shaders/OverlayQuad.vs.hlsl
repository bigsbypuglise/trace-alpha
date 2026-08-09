// One textured, tinted quad per draw, positioned entirely from constants.
//
// This is what makes the overlay cost nothing per video frame: geometry lives
// in a 64-byte constant buffer, so moving the timeline handle, swapping
// play for pause, or fading the whole panel are constant updates rather than
// re-rasterising and re-uploading a texture. The atlas is only rebuilt when
// something about its CONTENT changes -- size, DPI, theme.
cbuffer OverlayConstants : register(b0) {
    // Destination in device pixels: x, y, width, height.
    float4 dstRect;
    // Source in atlas UV: u0, v0, u1, v1.
    float4 srcRect;
    // Multiplied into the sampled colour. Alpha is the fade/opacity term, and
    // because the atlas is premultiplied a scalar multiply of all four channels
    // is the correct way to fade it.
    float4 tint;
    // x = viewport width, y = viewport height, in the same device pixels as
    // dstRect. Passed rather than derived so there is exactly one coordinate
    // space in play and the shader never has to guess which.
    float4 viewport;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    // Triangle strip corners: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(id & 1, (id >> 1) & 1);

    const float2 px = dstRect.xy + corner * dstRect.zw;
    // Device pixels -> NDC. Y flips, because pixel space grows downward.
    const float2 ndc = float2(px.x / viewport.x * 2.0 - 1.0,
                              1.0 - px.y / viewport.y * 2.0);

    VSOut o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv = srcRect.xy + corner * (srcRect.zw - srcRect.xy);
    return o;
}
