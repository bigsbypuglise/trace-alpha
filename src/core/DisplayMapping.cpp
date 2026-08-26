#include "core/DisplayMapping.h"

#include "core/ParallelBands.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>

namespace trace::core {
namespace {

// EXACT 8-BIT QUANTISATION OF THE 2.2 GAMMA CURVE, BY THRESHOLD SEARCH RATHER
// THAN BY powf PER SAMPLE.
//
// powf costs roughly 20ns; at three components over a 1920x1080 pass that is
// ~124ms, which is three frame budgets. A lookup table indexed on the INPUT
// cannot be used instead: the curve is near-vertical at the bottom, so the
// linear values separating output levels 0, 1 and 2 all sit below 1.5e-5 and a
// 16-bit input table cannot tell them apart -- which is exactly the deep shadow
// detail a scene-linear render is being reviewed for.
//
// So the table is over the OUTPUT: 255 thresholds, thresholds[i] being the
// linear value at which the 8-bit result becomes i+1. A binary search is eight
// compares and gives exactly the rounding powf would have produced.
struct GammaThresholds {
    float t[255];
    GammaThresholds() {
        for (int i = 0; i < 255; ++i) {
            const double mid = (static_cast<double>(i) + 0.5) / 255.0;
            t[i] = static_cast<float>(std::pow(mid, 2.2));
        }
    }
};

const GammaThresholds& gammaThresholds() {
    static const GammaThresholds g;
    return g;
}

inline uint8_t gamma22To8(float linear, const float* t) {
    // The first test also catches NaN, which must not fall through into the
    // search: a comparison against NaN is false either way and the loop would
    // return an arbitrary level rather than black.
    if (!(linear > t[0])) return 0;
    if (linear >= t[254]) return 255;
    int lo = 0, hi = 254;
    while (lo < hi) {
        const int mid = (lo + hi + 1) >> 1;
        if (linear >= t[mid]) lo = mid; else hi = mid - 1;
    }
    return static_cast<uint8_t>(lo + 1);
}

inline uint8_t unitTo8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

} // namespace

QString displayMapName(DisplayMap map) {
    switch (map) {
        case DisplayMap::Gamma22:    return QStringLiteral("Gamma 2.2");
        case DisplayMap::Ocio:       return QStringLiteral("OCIO");
        case DisplayMap::Normalise:  return QStringLiteral("Normalise");
        case DisplayMap::SignedUnit: return QStringLiteral("Signed unit");
        case DisplayMap::Raw:        return QStringLiteral("Raw");
    }
    return QStringLiteral("Raw");
}

DisplayMap defaultMapForClass(PassClass cls) {
    switch (cls) {
        case PassClass::Colour:   return DisplayMap::Gamma22;
        case PassClass::Position: return DisplayMap::Normalise;
        case PassClass::Normal:   return DisplayMap::SignedUnit;
        case PassClass::Depth:    return DisplayMap::Normalise;
        case PassClass::Data:     return DisplayMap::Raw;
    }
    return DisplayMap::Raw;
}

void measureFloatRange(const VideoFrame& in, float& lo, float& hi, double& fractionAboveOne) {
    lo = 0.0f;
    hi = 1.0f;
    fractionAboveOne = 0.0;
    if (in.isNull() || !in.buffer || !isFloatRgba(in.buffer->layout())) return;

    const int w = in.buffer->width();
    const int h = in.buffer->height();
    if (w <= 0 || h <= 0) return;
    const uint8_t* base = in.buffer->data();
    const int stride = in.buffer->bytesPerLine();

    std::mutex m;
    float gLo = std::numeric_limits<float>::infinity();
    float gHi = -std::numeric_limits<float>::infinity();
    std::atomic<long long> over{0};

    runInRowBands(h, [&](int y0, int rows) {
        float bLo = std::numeric_limits<float>::infinity();
        float bHi = -std::numeric_limits<float>::infinity();
        long long bOver = 0;
        for (int y = y0; y < y0 + rows; ++y) {
            const float* row =
                reinterpret_cast<const float*>(base + static_cast<std::size_t>(y) * stride);
            for (int x = 0; x < w; ++x) {
                // Colour components only. Alpha has its own range by
                // definition and would drag a position pass's measured span
                // back to 0..1 for no reason.
                for (int c = 0; c < 3; ++c) {
                    const float v = row[x * 4 + c];
                    if (!(v == v)) continue;   // NaN
                    bLo = std::min(bLo, v);
                    bHi = std::max(bHi, v);
                    if (v > 1.0f) ++bOver;
                }
            }
        }
        over.fetch_add(bOver, std::memory_order_relaxed);
        std::lock_guard<std::mutex> g(m);
        gLo = std::min(gLo, bLo);
        gHi = std::max(gHi, bHi);
    });

    if (gLo <= gHi) {
        lo = gLo;
        hi = gHi;
    }
    const double n = static_cast<double>(w) * static_cast<double>(h) * 3.0;
    fractionAboveOne = n > 0.0 ? static_cast<double>(over.load()) / n : 0.0;
}

bool mapFloatToDisplay(const VideoFrame& in, VideoFrame& out, DisplayMap map,
                       DisplayMapResult* result, const DisplayRange* pinned) {
    if (in.isNull() || !in.buffer || !isFloatRgba(in.buffer->layout())) return false;

    const int w = in.buffer->width();
    const int h = in.buffer->height();
    if (w <= 0 || h <= 0) return false;

    // Normalise has to see the whole pass before it can convert any of it. The
    // other mappings do not, but the range is measured for them too, because it
    // is what says whether the picture on screen clipped -- and that is reported
    // beside the mapping name rather than kept quiet.
    float lo = 0.0f, hi = 1.0f;
    double aboveOne = 0.0;
    measureFloatRange(in, lo, hi, aboveOne);

    float scale = 1.0f, offset = 0.0f;
    if (map == DisplayMap::Normalise) {
        // THE PINNED RANGE WINS. Measuring per frame makes the mapping itself a
        // per-frame variable, so the same value maps to a different grey as the
        // playhead moves; the caller pins the range once per pass and the
        // measured figures below are then REPORTING only.
        if (pinned && pinned->valid) {
            lo = pinned->lo;
            hi = pinned->hi;
        }
        const float span = hi - lo;
        // A flat pass would divide by zero and read as pure black. Showing it as
        // mid grey with the measured range beside it says "this is constant"
        // rather than "this is empty".
        scale = span > 1e-12f ? 1.0f / span : 0.0f;
        offset = span > 1e-12f ? -lo * scale : 0.5f;
    } else if (map == DisplayMap::SignedUnit) {
        scale = 0.5f;
        offset = 0.5f;
    }

    auto dst = FrameBuffer::allocate(w, h, PixelLayout::BGRA8);
    if (!dst) return false;
    uint8_t* out8 = dst->data();
    const int dstStride = dst->bytesPerLine();
    const uint8_t* srcBase = in.buffer->data();
    const int srcStride = in.buffer->bytesPerLine();
    const float* thresholds = gammaThresholds().t;

    runInRowBands(h, [&](int y0, int rows) {
        for (int y = y0; y < y0 + rows; ++y) {
            const float* src =
                reinterpret_cast<const float*>(srcBase + static_cast<std::size_t>(y) * srcStride);
            uint8_t* d = out8 + static_cast<std::size_t>(y) * dstStride;
            for (int x = 0; x < w; ++x) {
                const float* p = src + x * 4;
                uint8_t rgb[3];
                for (int c = 0; c < 3; ++c) {
                    const float v = p[c];
                    switch (map) {
                        case DisplayMap::Gamma22:
                            rgb[c] = gamma22To8(v, thresholds);
                            break;
                        case DisplayMap::Ocio:
                        case DisplayMap::Raw:
                            rgb[c] = unitTo8(v);
                            break;
                        case DisplayMap::Normalise:
                        case DisplayMap::SignedUnit:
                            rgb[c] = unitTo8(v * scale + offset);
                            break;
                    }
                }
                // BGRA byte order, opaque. Alpha is DELIBERATELY not taken from
                // the pass and DELIBERATELY not gamma'd: the display buffer is
                // composited onto the black stage, and an EXR's alpha is
                // coverage, not the opacity of the review image. The float frame
                // still carries it for anything that wants it.
                d[x * 4 + 0] = rgb[2];
                d[x * 4 + 1] = rgb[1];
                d[x * 4 + 2] = rgb[0];
                d[x * 4 + 3] = 255;
            }
        }
    });

    out = VideoFrame{};
    out.buffer = std::move(dst);
    out.frameIndex = in.frameIndex;
    out.color = in.color;
    out.previewRes = in.previewRes;

    if (result) {
        result->map = map;
        result->inputLo = lo;
        result->inputHi = hi;
        result->fractionAboveOne = aboveOne;
    }
    return true;
}

} // namespace trace::core
