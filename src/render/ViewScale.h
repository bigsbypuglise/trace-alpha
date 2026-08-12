#pragma once

#include <QPointF>
#include <QSize>

namespace trace::render {

// HOW LARGE the picture is drawn and WHERE it sits -- as distinct from which way
// up it is (ViewTransform) and how wide its stored samples are (the pixel
// aspect). The three are separate structures for the same reason they are
// separate menu commands: rotating a zoomed picture must not un-zoom it, and
// zooming a rotated one must not straighten it.
//
// Like the transform, this is VIEWING STATE ONLY. No decoded pixel, frame
// index, cache entry or timing changes with it, which is why it lives beside
// the renderer and nowhere near the decoder.
struct ViewScale {
    // Fit-to-window is the default and is what Trace did exclusively before
    // spec phase 15: the picture is scaled to touch the viewport and centred.
    // While this is true `scale` and `pan` are not read at all -- the fit path
    // is bit-for-bit the expression it was, so the default costs one branch.
    bool fitToWindow = true;

    // DEVICE PIXELS PER DISPLAYED SOURCE PIXEL. 1.0 is Actual Size: one stored
    // sample of the source, after the pixel aspect and the transform, occupies
    // exactly one device pixel. Device rather than logical for the same reason
    // RenderStats::lastDrawSize is device: it is the grid the frame is
    // rasterised onto, and a logical answer differs from it by the dpr.
    double scale = 1.0;

    // The picture's centre relative to the viewport's, in device pixels.
    // Zero is centred, which is what every path in Trace did before panning
    // existed. Only meaningful while the picture is larger than the viewport;
    // clampPan forces it to zero on any axis where it is not, so "the image
    // touches all four viewport edges" (spec section 4) cannot be broken by a
    // stale offset left over from a previous zoom.
    QPointF pan;

    // THE FULL-RESOLUTION SOURCE'S ON-SCREEN SIZE, with the pixel aspect and
    // the composed transform ALREADY APPLIED -- computed once by the host, at
    // the same point it composes the transform, rather than re-derived by each
    // backend from the frame it happens to be holding.
    //
    // IT IS HERE BECAUSE A SCALE IS MEANINGLESS WITHOUT THE THING IT SCALES,
    // AND THE OBVIOUS REFERENCE IS WRONG. A backend knows only the frame it was
    // given, and during a drag that frame is a PREVIEW converted to the
    // viewport's size -- a 1280x720 stand-in for a 3840x2160 source. Scaling
    // the delivered frame would draw Actual Size at 1280x720 during the drag
    // and at 3840x2160 on release, so the picture would jump by a factor of
    // three at the moment the user stopped. Scaling the reference instead draws
    // the preview into exactly the rect the landing will occupy, which is the
    // same relationship preview and landing already have at fit.
    //
    // Empty means "no media, or the host has not said" -- the fit path is taken
    // and the frame's own size is used, which is the pre-phase-15 behaviour.
    QSize referenceDisplayed;
};

} // namespace trace::render
