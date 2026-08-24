#pragma once

#include <QString>

#include "core/ExrChannels.h"
#include "core/VideoFrame.h"

namespace trace::core {

// HOW SCENE-REFERRED FLOAT PIXELS BECOME SOMETHING A DISPLAY CAN SHOW.
//
// A float frame has no correct 8-bit reading on its own, so one of these is
// ALWAYS in force for one, and which one is ALWAYS reported on screen. That is
// the requirement stated as "a viewer that silently normalises is a viewer I
// cannot trust for review": normalising is allowed, hiding that it happened is
// not.
enum class DisplayMap {
    // The default for a colour pass with no colour transform loaded: clamp to
    // 0..1 and apply a 2.2 display gamma. It is a placeholder for a real view
    // transform and is labelled as such -- the measured beauty pass has 48-66%
    // of its pixels above 1.0, and every one of those clips here.
    Gamma22,
    // A colour pass with the OCIO stage active. Named here so the reported
    // mapping is one field with one vocabulary; the work is done by
    // ColorTransform, not by this file.
    Ocio,
    // Per-frame min/max across the pass, stretched to 0..1. For position and
    // depth, where there is no natural range at all. The range used is reported
    // with the mapping, because it changes from frame to frame.
    Normalise,
    // v * 0.5 + 0.5. The convention for a unit-vector normal pass.
    SignedUnit,
    // Clamp to 0..1 with no gamma. The honest answer for numeric data whose
    // convention we do not recognise.
    Raw,
};

QString displayMapName(DisplayMap map);

// The mapping a pass gets when the user has not chosen otherwise.
DisplayMap defaultMapForClass(PassClass cls);

// The result of running a mapping, for reporting rather than for drawing.
struct DisplayMapResult {
    DisplayMap map = DisplayMap::Gamma22;
    // The input range the mapping worked over. Meaningful for Normalise, where
    // it is what was measured; for the fixed mappings it is the range observed
    // in the frame, which is what says whether anything clipped.
    float inputLo = 0.0f;
    float inputHi = 1.0f;
    // Fraction of samples above 1.0 in the source. Zero for a mapping where
    // that cannot happen. This is the number that says how much a Gamma22
    // preview is throwing away.
    double fractionAboveOne = 0.0;
};

// RGBAF32 in, BGRA8 out. Returns false when `in` is not a float frame, in which
// case `out` is untouched.
//
// Source pixels are never modified, exactly as in ColorTransform::apply and for
// the same reason: the source is very likely still referenced by the frame cache.
bool mapFloatToDisplay(const VideoFrame& in, VideoFrame& out, DisplayMap map,
                       DisplayMapResult* result);

// Measures the pass's value range and the fraction above 1.0 without producing a
// picture. Used to fill in the reported range for the fixed mappings.
void measureFloatRange(const VideoFrame& in, float& lo, float& hi, double& fractionAboveOne);

} // namespace trace::core
