// Fullscreen triangle from the vertex id alone -- no vertex or index buffer,
// and therefore no buffer to keep in sync with the viewport.
//
// Letterboxing is done by setting the VIEWPORT to the fitted rect rather than
// by moving these vertices, so the quad is always "the whole of wherever we are
// drawing". That keeps aspect handling in one place (the viewport calculation)
// instead of split between C++ and a shader constant.
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    // id 0,1,2 -> uv (0,0), (2,0), (0,2): one oversized triangle that covers
    // the viewport, clipped by the rasteriser. Cheaper than two triangles and
    // avoids the diagonal seam a quad can show under some filtering.
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
