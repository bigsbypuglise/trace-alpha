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
    // Step 9: a filtered reduction. `footprint` is the size of ONE destination
    // pixel in normalised source coordinates, and `taps` is how many samples to
    // take across it per axis.
    //
    // A single bilinear Sample takes a 2x2 tap wherever it lands, so at a 6.4x
    // downscale it reads 4 source texels of every 41 and throws the rest away.
    // Measured against ffmpeg references, that put Trace 0.74 of the way from a
    // correct area reduction to naked point sampling -- indistinguishable from
    // swscale's `fast_bilinear`, and matched by Qt's raster bilinear on the CPU
    // path, because all three are the same 2x2 tap.
    //
    // taps == 1 collapses the loop below to exactly one Sample at input.uv, so it
    // is bit-identical to the pre-step-9 shader rather than merely close. That is
    // what makes TRACE_GPU_REDUCE=0 a real control, and it is also why the 1:1
    // preview path needs no special case: at ratio 1 the renderer sends 1.
    float2 footprint;
    float  taps;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut input) : SV_TARGET {
    // Box average over the destination pixel's footprint. The offsets are in
    // NORMALISED source coordinates, which is what lets the same loop serve all
    // three planes: a chroma plane is smaller, so the identical uv offset spans
    // proportionally the same area of it, and 4:2:0/4:2:2/4:4:4 continue to differ
    // in nothing but texture size. Sampling the planes in separate loops with
    // per-plane texel steps would have reintroduced exactly the subsampling
    // special-casing GATE C removed.
    //
    // Each tap is still a bilinear 2x2, so N taps per axis cover the footprint
    // with 2N source texels of reach -- N does not have to reach the ratio to
    // cover it. The renderer clamps N; see the caller for why it is not larger.
    const int n = max(1, (int)taps);
    const float inv = 1.0 / (float)n;
    float3 acc = float3(0.0, 0.0, 0.0);
    [loop] for (int j = 0; j < n; ++j) {
        // Tap centres across the footprint: ((k + 0.5)/n - 0.5) puts them at the
        // midpoints of n equal sub-spans, symmetric about the pixel centre. At
        // n == 1 that is exactly 0, i.e. input.uv untouched.
        const float dv = (((float)j + 0.5) * inv - 0.5) * footprint.y;
        [loop] for (int i = 0; i < n; ++i) {
            const float du = (((float)i + 0.5) * inv - 0.5) * footprint.x;
            const float2 uv = input.uv + float2(du, dv);
            acc += float3(planeY.Sample(srcSampler, uv).r,
                          planeU.Sample(srcSampler, uv).r,
                          planeV.Sample(srcSampler, uv).r);
        }
    }
    acc *= sampleScale * inv * inv;

    // Averaging happens in code space, BEFORE range normalisation and the
    // matrix. That ordering is deliberate and it is the cheap one: both remaining
    // steps are affine, so averaging first and transforming once is identical to
    // transforming every tap and averaging after -- for a 4x4 box that is one
    // matrix multiply instead of sixteen. It would NOT be identical if a
    // non-linear step (a transfer function, a tonemap) were ever added between
    // them; if BT.2020 tonemapping arrives, this order has to be revisited.
    float y = acc.r;
    float u = acc.g;
    float v = acc.b;

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
