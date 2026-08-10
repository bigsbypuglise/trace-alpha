// Fullscreen triangle from the vertex id alone -- no vertex or index buffer,
// and therefore no buffer to keep in sync with the viewport.
//
// Letterboxing is done by setting the VIEWPORT to the fitted rect rather than
// by moving these vertices, so the quad is always "the whole of wherever we are
// drawing". That keeps aspect handling in one place (the viewport calculation)
// instead of split between C++ and a shader constant.
//
// The view transform (rotate / flip) is applied HERE, to the texture
// coordinate, and that placement is the reason neither pixel shader had to
// change for it. Both of them sample at input.uv and neither can tell that the
// uv arrived rotated -- so 4:2:0/4:2:2/4:4:4, every bit depth, the BGRA path
// and step 9's box average all inherit the transform without a variant.
cbuffer ViewParams : register(b0) {
    // Rows of a 2x2 applied about the CENTRE of the unit square:
    //   source_uv = M * (dest_uv - 0.5) + 0.5
    // Identity is [[1,0],[0,1]], which is what makes "no transform" cost one
    // multiply rather than a branch.
    float4 uvMatrix;   // .xy = row 0, .zw = row 1
};

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
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);

    // Transform the coordinate the triangle carries, not its position. The
    // oversized triangle reaches uv 2.0, and the transform is affine about
    // (0.5, 0.5), so interpolating the transformed corners is identical to
    // transforming the interpolated coordinate -- the rasteriser's linear
    // interpolation commutes with it.
    const float2 c = uv - 0.5;
    o.uv = float2(dot(uvMatrix.xy, c), dot(uvMatrix.zw, c)) + 0.5;
    return o;
}
