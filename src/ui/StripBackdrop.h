#pragma once

#include <QImage>

namespace trace::core { struct VideoFrame; }

namespace trace::ui {

// UI redesign roadmap step 10, route 2: a blurred copy of the video under the
// top chrome strip, so a strip that MUST stay a native window can still look
// like the design's backdrop-blur.
//
// WHY THIS EXISTS AT ALL, because the obvious answer was measured and is wrong.
// The design's mockup uses CSS `backdrop-filter: blur()`, which blurs what is
// behind the ELEMENT -- the video. Windows' Mica and Acrylic blur what is
// behind the WINDOW -- the desktop. They are different effects, and measured on
// 2026-08-18 every DWM backdrop call returns S_OK and changes the TITLE BAR
// only: 37,987 differing pixels in rows 1..30 and zero anywhere in the client,
// because `TraceD3D11Surface` covers 100% of it. So the pixels have to come
// from the frame, and this is where they come from.
//
// THE OUTPUT IS DELIBERATELY TINY. A backdrop blur is very low frequency, so
// the strip's background is reconstructed from a grid of a few dozen cells
// stretched across it -- which is what keeps a per-frame effect affordable on
// the UI thread. Sampling a handful of source pixels per cell costs the same at
// 8K as at 720p, because the cost is set by the OUTPUT size and not the input:
// the loop is over destination cells, and each reads a fixed 4x4 of source.
//
// It is NOT a downscale of the band and must not become one. Averaging every
// source pixel in a 3840x114 band would be ~438k samples a frame at 4K and
// would scale with resolution, which is the shape of cost this project spends
// its time removing.
struct StripBackdrop {
    // Grid the strip's background is rebuilt from. 48x6 stretched over a
    // 1280x38 strip is ~27px per cell horizontally -- far coarser than the blur
    // radius the design draws, so the stretch is doing the blurring and the
    // explicit pass below only removes cell edges.
    static constexpr int kCellsX = 48;
    static constexpr int kCellsY = 6;
    // Source samples averaged per cell, per axis. 4x4 = 16 reads a cell, so a
    // whole frame's backdrop is 48*6*16 = 4,608 samples whatever the source is.
    static constexpr int kSamplesPerCell = 4;

    // Returns a kCellsX x kCellsY ARGB32 image of the top `coverFraction` of the
    // frame, box-sampled and then blurred, or a null image if the frame carries
    // no layout this understands. A null return is not a failure: the caller
    // falls back to the design package's own solid colour, which is what ships.
    //
    // `coverFraction` is how much of the frame's HEIGHT the strip covers -- the
    // strip's height divided by the height the frame is drawn at.
    static QImage sample(const trace::core::VideoFrame& frame, double coverFraction);
};

} // namespace trace::ui
