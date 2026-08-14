#pragma once

#include <QSize>

namespace trace::app {

// THE OPENING-WINDOW GEOMETRY CALCULATION, AS A PURE FUNCTION.
//
// It is separated from MainWindow for one reason: spec section 4's validation
// matrix names 100 / 125 / 150 / 200% DPI, and this box runs at 100% only. Every
// devicePixelRatioF() term in the arithmetic is therefore the identity on the
// only machine that can run it, and an expression that is never exercised with a
// real value is an expression nobody has tested. Taking `dpr` as an INPUT rather
// than reading it from a widget is what makes the other three executable.
//
// Nothing here touches a widget, a screen or a window. Everything it needs is an
// argument, so `Trace.exe --window-shape-selftest` can drive the whole matrix
// with no display attached at all -- the same shape as the renderer selftest CI
// already runs.
//
// SYNTHETIC DPR IS NOT MIXED-MONITOR VALIDATION. Injecting 1.25 here proves the
// arithmetic; it does not prove that a real WM_DPICHANGED arrives, that the
// swapchain resizes, or that a window dragged between two differently-scaled
// monitors recomputes.
//
// THE HARDWARE PASS HAPPENED ON 2026-08-14 (plan section 20.4) AND IT FOUND A
// BUG THIS FILE COULD NOT HAVE. Every row of the selftest passed, on a build
// where a window crossing a 100% -> 150% boundary came out the wrong shape with
// the picture pillarboxed inside it -- because section 4's sizing pass was never
// re-run on a DPI change at all. A DPI change is not a WindowStateChange and
// sends no WM_SIZING, so neither of the two paths that reshape ever fired. The
// arithmetic here was correct throughout and nobody was calling it.
//
// That is the durable lesson: a pure function cannot notice that it was not
// invoked, so a green selftest over it says nothing about whether the shipping
// path reaches it. The hardware case lives in scripts/measure/dpimove.ps1.

// Which constraint decided the answer. Reported rather than inferred, because
// "the cap bound it" and "the work area bound it" produce the same number on
// some inputs and different numbers on others, and a table of sizes alone
// cannot say which rule was doing the work.
enum class ShapeBound {
    Natural,   // the media is small enough to open at its natural size
    Cap,       // the 1280x720-equivalent area cap
    WorkArea,  // 80% of the monitor's available work area
    Minimum,   // enlarged to keep the application and transport usable
};

const char* shapeBoundName(ShapeBound bound);

struct ShapeInputs {
    // The media's natural displayed size in SOURCE pixels -- encoded size
    // corrected for sample aspect, with container rotation applied. Source
    // pixels are device pixels: a 1920-wide frame should occupy 1920 physical
    // pixels so the image maps 1:1 to the panel, which is why this is divided
    // by dpr rather than used as a logical size.
    QSize naturalPixels;
    // The on-screen display aspect: media shape composed with the phase 10
    // viewing transforms. Everything below preserves it exactly.
    double aspect = 0.0;
    double dpr = 1.0;
    // Client chrome (menu bar, status bar, HUD, docked transport bar) and the
    // window frame, both in logical pixels. Kept apart because only the first
    // is inside the geometry setGeometry() takes.
    QSize chromeLogical;
    QSize frameLogical;
    QSize workAreaLogical;
    // The viewer's own aspect-correct floor, in logical pixels.
    QSize viewerMinimumLogical;
    // OWNER POLICY, 2026-08-11. The initial viewer is capped to this many
    // logical pixels of AREA, reshaped to the media's aspect -- not to a
    // width and a height, which is what makes it shape-neutral: 16:9 lands on
    // 1280x720, 1:1 on 960x960, 4:5 on 859x1073, 9:16 on 720x1280, all of the
    // same area. Capping a width instead would give a 9:16 clip a window a
    // quarter the size of a 16:9 one.
    double capAreaLogical = 1280.0 * 720.0;
    // The settled 460px floating transport has to fit (spec phase 6 fixed that
    // number and changing it reopens an owner decision), so no window may be
    // narrower than it whatever the media's shape.
    int minTransportWidthLogical = 460;
    // Of the work area, including chrome and frame. The startup window never
    // exceeds this.
    double workAreaFraction = 0.80;
};

struct ShapeResult {
    // What the video client area should be, in logical pixels.
    QSize viewerLogical;
    // ONE proportional scale was applied to the natural size, never a separate
    // clamp per axis. Reported so a distorted result is attributable: if the
    // returned size does not match `aspect`, the scale is not the reason.
    double scale = 1.0;
    ShapeBound bound = ShapeBound::Natural;
    bool valid = false;
};

// Preserves the aspect ratio exactly, by construction: a single scale is chosen
// from every constraint at once and the second axis is then recomputed from the
// first. Width and height are never clamped independently, which is the one way
// this calculation could silently distort the picture's shape.
ShapeResult computeViewerSize(const ShapeInputs& in);

} // namespace trace::app
