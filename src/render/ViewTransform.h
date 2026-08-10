#pragma once

#include <QSize>

namespace trace::render {

// A temporary VIEWING transform: how the decoded frame is oriented on its way
// to the screen, and nothing else.
//
// It changes no pixel of the source, no frame identity, no timing and no cache
// entry. A frame is still the frame it was; this only decides which way up the
// renderer draws it. That boundary is the reason it lives on VideoRenderer
// rather than anywhere near the decoder: rotating in the decode path would mean
// re-converting, re-caching and re-costing every frame for something the GPU
// does for free in a coordinate transform.
//
// Both backends implement it from this one description. The spec's own fallback
// -- define the actions and defer the rendering -- exists precisely to stop
// rotate/flip being hacked separately into the CPU and GPU paths, and two
// implementations of "rotate" that disagree by a flip is exactly the kind of
// divergence ddb38ca had to go and measure out of the video rect.
struct ViewTransform {
    // Clockwise quarter turns, 0-3. Stored normalised so callers never have to
    // ask whether -90 or 270 was meant.
    int quarterTurns = 0;
    // Applied to the ROTATED image, which is what a user means by "flip
    // horizontally" after having rotated: they are flipping what they can see.
    bool flipH = false;
    bool flipV = false;

    bool isIdentity() const { return quarterTurns == 0 && !flipH && !flipV; }
    // True when the transform exchanges the two axes, so the displayed aspect
    // ratio is the reciprocal of the source's. Every consumer that fits, scales
    // or filters has to ask this; none of them should re-derive it from
    // quarterTurns.
    bool swapsAxes() const { return quarterTurns == 1 || quarterTurns == 3; }

    QSize apply(QSize source) const {
        return swapsAxes() ? QSize(source.height(), source.width()) : source;
    }

    bool operator==(const ViewTransform& o) const {
        return quarterTurns == o.quarterTurns && flipH == o.flipH && flipV == o.flipV;
    }
    bool operator!=(const ViewTransform& o) const { return !(*this == o); }

    static ViewTransform rotated(int turns) {
        ViewTransform t;
        t.quarterTurns = ((turns % 4) + 4) % 4;
        return t;
    }
};

// The transform read from TRACE_VIEW_TRANSFORM, for A/B-ing the two backends
// against each other before the interface pass wires real actions to it.
//
// Accepts a rotation in degrees and/or `h`/`v`, e.g. "90", "180h", "v".
// Deliberately a test knob and not a feature: spec phase 10 owns the actions,
// the menu items and resetting on new media. Returns identity for an empty or
// unparseable value, and identity costs nothing anywhere.
ViewTransform viewTransformFromEnvironment();

} // namespace trace::render
