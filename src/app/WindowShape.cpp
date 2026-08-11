#include "app/WindowShape.h"

#include <algorithm>
#include <cmath>

namespace trace::app {

const char* shapeBoundName(ShapeBound bound) {
    switch (bound) {
        case ShapeBound::Natural: return "natural";
        case ShapeBound::Cap: return "cap";
        case ShapeBound::WorkArea: return "work";
        case ShapeBound::Minimum: return "minimum";
    }
    return "?";
}

namespace {

// Width that gives `area` at `aspect`. The whole reason the cap is an area and
// not a width -- see ShapeInputs::capAreaLogical.
double widthForArea(double area, double aspect) {
    return std::sqrt(area * aspect);
}

} // namespace

ShapeResult computeViewerSize(const ShapeInputs& in) {
    ShapeResult out;
    if (!(in.aspect > 0.0) || !(in.dpr > 0.0) || in.naturalPixels.isEmpty()) return out;
    if (in.workAreaLogical.isEmpty()) return out;

    // Natural displayed size in LOGICAL pixels. Source pixels are device pixels,
    // so a higher scale factor means FEWER logical pixels for the same physical
    // size -- getting this the other way round puts a 4K frame in a window half
    // again too big on a 150% display and then scales it back down.
    const double naturalW = in.naturalPixels.width() / in.dpr;
    const double naturalH = in.naturalPixels.height() / in.dpr;
    if (!(naturalW > 0.0) || !(naturalH > 0.0)) return out;

    // Everything below is a SCALE on the natural size. Collecting them all and
    // applying one is what makes "preserve the media's exact display aspect
    // ratio" a property of the code rather than something to be checked
    // afterwards: two independent clamps are the only way this can distort.
    double scale = 1.0;
    ShapeBound bound = ShapeBound::Natural;

    // 1. OWNER CAP: a 1280x720-equivalent AREA, reshaped to the media's aspect.
    //    Natural size is used only when it is already smaller than this, which
    //    is the whole of "use natural displayed size only when it is reasonably
    //    small". Before this, 4K media opened into a window as tall as the
    //    monitor -- correct by the spec's wording and much too big in practice.
    const double capW = widthForArea(in.capAreaLogical, in.aspect);
    if (capW > 0.0 && capW < naturalW) {
        scale = capW / naturalW;
        bound = ShapeBound::Cap;
    }

    // 2. WORK AREA: the OUTER window -- chrome and frame included -- must fit
    //    inside the given fraction of the monitor's work area. Applied after the
    //    cap and as a further scale on the same natural size, never as a second
    //    clamp on the capped result.
    const double budgetW =
        in.workAreaLogical.width() * in.workAreaFraction - in.chromeLogical.width() - in.frameLogical.width();
    const double budgetH =
        in.workAreaLogical.height() * in.workAreaFraction - in.chromeLogical.height() - in.frameLogical.height();
    if (budgetW > 0.0 && budgetH > 0.0) {
        const double fit = std::min(budgetW / naturalW, budgetH / naturalH);
        if (fit < scale) {
            scale = fit;
            bound = ShapeBound::WorkArea;
        }
    }

    // 3. MINIMUM: very small media may be ENLARGED enough to keep the
    //    application and its transport usable. The 460px floating transport is
    //    a settled number (spec phase 6), so no window may be narrower than it
    //    whatever the media's shape -- which on a tall clip is the binding
    //    constraint rather than the viewer's own floor.
    const double minW = std::max<double>(in.minTransportWidthLogical, in.viewerMinimumLogical.width());
    const double minH = in.viewerMinimumLogical.height();
    const double grow = std::max(minW / (naturalW * scale), minH / (naturalH * scale));
    if (grow > 1.0) {
        scale *= grow;
        bound = ShapeBound::Minimum;
    }

    // 4. LAST RESORT: the minimum above is allowed to push past the 80% budget,
    //    because a transport too narrow to use is worse than a large window --
    //    but never past the work area itself, because a window bigger than the
    //    screen cannot be moved or closed. On any monitor this application
    //    supports, 460 logical px plus chrome is far inside the work area and
    //    this branch never runs; it exists so that "never off screen" is a
    //    property rather than an assumption about monitor sizes.
    const double hardW = in.workAreaLogical.width() - in.chromeLogical.width() - in.frameLogical.width();
    const double hardH = in.workAreaLogical.height() - in.chromeLogical.height() - in.frameLogical.height();
    if (hardW > 0.0 && hardH > 0.0) {
        const double hard = std::min(hardW / naturalW, hardH / naturalH);
        if (hard < scale) {
            scale = hard;
            bound = ShapeBound::WorkArea;
        }
    }

    // Width from the single scale, then height RECOMPUTED from the width and the
    // aspect. Rounding both independently is how a ratio drifts by a pixel at
    // small sizes, and a pixel of drift is a black line down one edge of a
    // window whose whole purpose is not to have one.
    int w = std::max(1, static_cast<int>(std::lround(naturalW * scale)));
    int h = std::max(1, static_cast<int>(std::lround(w / in.aspect)));

    out.viewerLogical = QSize(w, h);
    out.scale = scale;
    out.bound = bound;
    out.valid = true;
    return out;
}

} // namespace trace::app
